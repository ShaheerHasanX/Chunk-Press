#pragma once

#include "chunkpress.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace chunkpress::detail {

using Bytes = std::vector<unsigned char>;
inline constexpr unsigned headerSize = 40;
inline constexpr unsigned entrySize = 32;
inline constexpr std::uint64_t maxChunks = 1'000'000;

struct Entry {
    std::uint64_t id;
    std::uint64_t offset;
    std::uint32_t original;
    std::uint32_t compressed;
    std::uint32_t crc;
};

struct Chunk {
    std::uint64_t id;
    Bytes bytes;
    std::uint32_t crc = 0;
    std::uint32_t original = 0;
};

[[noreturn]] void systemError(const std::string& operation, int error);

class Fd {
public:
    Fd() = default;
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    void reset(int fd) { fd_ = fd; }  // Set once, directly after open or mkstemp.
private:
    int fd_ = -1;
};

class Input {
public:
    explicit Input(const Path& path);
    std::uint64_t size() const { return initial_.st_size; }
    void read(std::uint64_t offset, unsigned char* out, std::size_t size) const;
    void checkUnchanged() const;
private:
    Fd fd_;
    struct stat initial_{};
};

class Output {
public:
    explicit Output(const Path& path);
    ~Output();
    void write(const unsigned char* data, std::size_t size);
    void rewriteHeader(const unsigned char* data);
    std::uint64_t size() const { return size_; }
    void commit();
private:
    Path path_;
    std::vector<char> temporary_;
    Fd fd_;
    std::uint64_t size_ = 0;
    bool temporaryPresent_ = true;
};

struct Layout {
    std::uint64_t original;
    unsigned chunkSize;
    std::uint64_t count() const { return original / chunkSize + (original % chunkSize != 0); }
    unsigned size(std::uint64_t id) const;
};

class ArchiveWriter {
public:
    ArchiveWriter(Output& output, Layout layout);
    void append(const Chunk& chunk);
    void finish();
private:
    Output& output_;
    Layout layout_;
    std::vector<Entry> index_;
    std::mutex writeMutex_;
};

Layout readIndex(const Input& input, std::vector<Entry>& index);
Chunk readChunk(const Input& input, const Layout& layout, std::uint64_t id);
void encode(Chunk& chunk);
void decode(const Input& input, const Entry& entry, Bytes& packed, Bytes& decoded);

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Operation cancelled") {}
};

struct Job {
    mutable std::mutex mutex;
    std::condition_variable changed;
    Progress progress;
    std::exception_ptr failure;
    unsigned active = 0;
    unsigned targetCapacity = 0;
    unsigned maxCapacity = 0;
    bool started = false;
    bool paused = false;
    bool cancelled = false;
    bool sealed = false;
    bool finished = false;

    bool stopping() const { return cancelled || failure; }
    void check() const;
    void begin(Progress initial, unsigned capacityLimit = 0);
    void finish();
    void finalize();
    void record(const Chunk& chunk, std::uint64_t bytes);
    void fail(std::exception_ptr error);
    void waitForWork(std::unique_lock<std::mutex>& lock);
    void setPhase();
};

struct Engine {
    static Result compress(const Path& input, const Path& output, const Options& options, Control& control);
    static Result restore(const Path& archive, const Path& output, const Path& reference, Control& control);
};

} // namespace chunkpress::detail
