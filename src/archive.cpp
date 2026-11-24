#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <zlib.h>

namespace chunkpress::detail {
namespace {

constexpr std::array<unsigned char, 8> signature{'C','H','N','K','P','R','S','1'};

void storeInteger(unsigned char* out, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        out[i] = static_cast<unsigned char>(value >> (8 * i));
    }
}

std::uint64_t loadInteger(const unsigned char* in, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

std::uint32_t checksum(const Bytes& bytes) {
    return static_cast<std::uint32_t>(crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
}

[[noreturn]] void invalid(const std::string& reason) {
    throw std::runtime_error("Invalid archive: " + reason);
}

} // namespace

void systemError(const std::string& operation, int error) {
    throw std::system_error(error, std::generic_category(), operation);
}

Fd::~Fd() {
    if (fd_ >= 0) ::close(fd_);
}

Input::Input(const Path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) systemError("Open " + path.string(), errno);
    fd_.reset(fd);
    if (::fstat(fd, &initial_) < 0) systemError("Stat " + path.string(), errno);
    if (!S_ISREG(initial_.st_mode) || initial_.st_size < 0) {
        throw std::runtime_error("Choose a regular file: " + path.string());
    }
}

void Input::read(std::uint64_t offset, unsigned char* out, std::size_t size) const {
    while (size > 0) {
        const ssize_t count = ::pread(fd_.get(), out, size, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) systemError("Read input", errno);
        if (count == 0) throw std::runtime_error("Unexpected end of input");
        size -= count;
        out += count;
        offset += count;
    }
}

void Input::checkUnchanged() const {
    struct stat current{};
    if (::fstat(fd_.get(), &current) < 0) systemError("Check input", errno);
    const auto sameTime = [](timespec a, timespec b) {
        return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
    };
    if (current.st_size != initial_.st_size || !sameTime(current.st_mtim, initial_.st_mtim)
        || !sameTime(current.st_ctim, initial_.st_ctim)) {
        throw std::runtime_error("An input file changed during the operation");
    }
}

Output::Output(const Path& path) : path_(path) {
    if (path.filename().empty() || path.filename() == "." || path.filename() == "..") {
        throw std::runtime_error("Choose an output filename");
    }
    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        throw std::runtime_error("Output already exists: " + path.string());
    }
    if (errno != ENOENT) systemError("Check output " + path.string(), errno);
    const auto name = (path.parent_path() / ".chunkpress-XXXXXX").string();
    temporary_.assign(name.begin(), name.end());
    temporary_.push_back('\0');
    const int fd = ::mkostemp(temporary_.data(), O_CLOEXEC);
    if (fd < 0) systemError("Create output in " + (path.has_parent_path() ? path.parent_path().string() : "."), errno);
    fd_.reset(fd);
}

Output::~Output() {
    if (temporaryPresent_) ::unlink(temporary_.data());
}

void Output::write(const unsigned char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t count = ::write(fd_.get(), data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) systemError("Write output", errno);
        if (count == 0) throw std::runtime_error("Short output write");
        data += count;
        size -= count;
        size_ += count;
    }
}

void Output::rewriteHeader(const unsigned char* data) {
    std::size_t offset = 0;
    while (offset < headerSize) {
        const ssize_t count = ::pwrite(fd_.get(), data + offset, headerSize - offset, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) systemError("Write archive header", errno);
        if (count == 0) throw std::runtime_error("Short header write");
        offset += count;
    }
}

void Output::commit() {
    while (::fsync(fd_.get()) < 0) {
        if (errno != EINTR) systemError("Sync output", errno);
    }
    if (::renameat2(AT_FDCWD, temporary_.data(), AT_FDCWD, path_.c_str(), RENAME_NOREPLACE) == 0) {
        temporaryPresent_ = false;
        return;
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
        systemError("Publish " + path_.string(), errno);
    }
    // A same-directory link also publishes atomically without replacing the destination.
    if (::link(temporary_.data(), path_.c_str()) < 0) systemError("Publish " + path_.string(), errno);
}

unsigned Layout::size(std::uint64_t id) const {
    return static_cast<unsigned>(std::min<std::uint64_t>(chunkSize, original - id * chunkSize));
}

ArchiveWriter::ArchiveWriter(Output& output, Layout layout) : output_(output), layout_(layout) {
    const std::array<unsigned char, headerSize> blank{};
    output_.write(blank.data(), blank.size());
    index_.reserve(layout.count());
}

void ArchiveWriter::append(const Chunk& chunk) {
    std::lock_guard lock(writeMutex_);
    const auto size = static_cast<unsigned>(chunk.bytes.size());
    const auto offset = output_.size();
    output_.write(chunk.bytes.data(), size);
    index_.push_back({chunk.id, offset, chunk.original, size, chunk.crc});
}

void ArchiveWriter::finish() {
    std::lock_guard lock(writeMutex_);
    if (index_.size() != layout_.count()) throw std::runtime_error("Incomplete archive index");
    const auto indexOffset = output_.size();
    std::array<unsigned char, entrySize> record{};
    Bytes batch;
    batch.reserve(1024 * entrySize);
    for (const auto& entry : index_) {
        storeInteger(record.data(), entry.id, 8);
        storeInteger(record.data() + 8, entry.offset, 8);
        storeInteger(record.data() + 16, entry.original, 4);
        storeInteger(record.data() + 20, entry.compressed, 4);
        storeInteger(record.data() + 24, entry.crc, 4);
        batch.insert(batch.end(), record.begin(), record.end());
        if (batch.size() == 1024 * entrySize) {
            output_.write(batch.data(), batch.size());
            batch.clear();
        }
    }
    output_.write(batch.data(), batch.size());

    std::array<unsigned char, headerSize> header{};
    std::copy(signature.begin(), signature.end(), header.begin());
    storeInteger(header.data() + 8, 1, 4);
    storeInteger(header.data() + 12, layout_.chunkSize, 4);
    storeInteger(header.data() + 16, layout_.original, 8);
    storeInteger(header.data() + 24, index_.size(), 8);
    storeInteger(header.data() + 32, indexOffset, 8);
    output_.rewriteHeader(header.data());
}

Layout readIndex(const Input& input, std::vector<Entry>& index) {
    if (input.size() < headerSize) invalid("missing header");
    std::array<unsigned char, headerSize> header{};
    input.read(0, header.data(), header.size());
    if (!std::equal(signature.begin(), signature.end(), header.begin())) invalid("unrecognized signature");
    if (loadInteger(header.data() + 8, 4) != 1) invalid("unsupported version");
    const auto chunkSize = static_cast<unsigned>(loadInteger(header.data() + 12, 4));
    if (chunkSize < minChunk || chunkSize > maxChunk) invalid("chunk size out of range");
    const Layout layout{loadInteger(header.data() + 16, 8), chunkSize};
    const auto count = loadInteger(header.data() + 24, 8);
    const auto indexOffset = loadInteger(header.data() + 32, 8);
    if (count != layout.count() || count > maxChunks || indexOffset < headerSize
        || indexOffset > input.size() || count * entrySize != input.size() - indexOffset) {
        invalid("index size or offset");
    }

    index.resize(count);
    std::array<unsigned char, 1024 * entrySize> block{};
    std::uint64_t expectedOffset = headerSize;
    for (std::uint64_t begin = 0; begin < count; begin += 1024) {
        const auto length = std::min<std::uint64_t>(1024, count - begin);
        input.read(indexOffset + begin * entrySize, block.data(), length * entrySize);
        for (std::size_t i = 0; i < length; ++i) {
            const auto* record = block.data() + i * entrySize;
            const auto id = loadInteger(record, 8);
            const auto offset = loadInteger(record + 8, 8);
            const auto original = static_cast<unsigned>(loadInteger(record + 16, 4));
            const auto compressed = static_cast<unsigned>(loadInteger(record + 20, 4));
            const auto crc = static_cast<unsigned>(loadInteger(record + 24, 4));
            if (id >= count || index[id].original != 0 || original != layout.size(id)
                || compressed == 0 || compressed > compressBound(original) || offset != expectedOffset
                || offset > indexOffset || compressed > indexOffset - offset || loadInteger(record + 28, 4) != 0) {
                invalid("invalid chunk index entry");
            }
            index[id] = {id, offset, original, compressed, crc};
            expectedOffset += compressed;
        }
    }
    if (expectedOffset != indexOffset) invalid("unindexed payload bytes");
    return layout;
}

Chunk readChunk(const Input& input, const Layout& layout, std::uint64_t id) {
    Chunk chunk{id, Bytes(layout.size(id))};
    input.read(id * layout.chunkSize, chunk.bytes.data(), chunk.bytes.size());
    return chunk;
}

void encode(Chunk& chunk) {
    chunk.original = static_cast<unsigned>(chunk.bytes.size());
    chunk.crc = checksum(chunk.bytes);
    uLongf length = compressBound(chunk.original);
    Bytes packed(length);
    if (compress2(packed.data(), &length, chunk.bytes.data(), chunk.original, 6) != Z_OK) {
        throw std::runtime_error("zlib failed to compress chunk " + std::to_string(chunk.id));
    }
    packed.resize(length);
    chunk.bytes = std::move(packed);
}

void decode(const Input& input, const Entry& entry, Bytes& packed, Bytes& decoded) {
    packed.resize(entry.compressed);
    decoded.resize(entry.original);
    input.read(entry.offset, packed.data(), packed.size());
    uLong packedLength = entry.compressed;
    uLongf decodedLength = entry.original;
    const auto status = uncompress2(decoded.data(), &decodedLength, packed.data(), &packedLength);
    if (status != Z_OK || decodedLength != entry.original || packedLength != entry.compressed
        || checksum(decoded) != entry.crc) {
        invalid("chunk " + std::to_string(entry.id) + " failed decompression or checksum");
    }
}

} // namespace chunkpress::detail
