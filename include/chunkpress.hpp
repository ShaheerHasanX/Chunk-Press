#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace chunkpress {

inline constexpr unsigned minChunk = 4 * 1024;
inline constexpr unsigned maxChunk = 16 * 1024 * 1024;
inline constexpr unsigned maxWorkers = 32;
inline constexpr unsigned maxBuffer = 64;

enum class Priority { fifo, evenFirst };
enum class Phase { ready, running, pausing, paused, cancelling, finalizing, finished };
enum class WorkerState { idle, compressing, waitingForWriter, stopped };

struct Options {
    unsigned workers = 4;
    unsigned activeLimit = 0;  // Zero uses the full pool.
    unsigned chunkSize = 1024 * 1024;
    unsigned bufferCapacity = 8;  // Capacity of each of the two queues.
    Priority priority = Priority::fifo;
    bool sequential = false;
};

struct Progress {
    Phase phase = Phase::ready;
    std::uint64_t originalBytes = 0;
    std::uint64_t processedBytes = 0;
    std::uint64_t archiveBytes = 0;
    std::uint64_t totalChunks = 0;
    std::uint64_t completedChunks = 0;
    unsigned inputDepth = 0;
    unsigned outputDepth = 0;
    unsigned bufferCapacity = 0;
    unsigned readersActive = 0;
    unsigned workerCount = 0;
    WorkerState workers[maxWorkers]{};
    bool writerActive = false;
};

struct Result {
    std::uint64_t originalBytes;
    std::uint64_t archiveBytes;
    std::uint64_t chunks;
    double seconds;
};

namespace detail { struct Job; struct Engine; }

// A control object belongs to one job. Its public methods are safe across threads.
class Control {
public:
    Control();
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
    void pause();
    void resume();
    void cancel();
    void resize(unsigned capacity);
    Progress progress() const;

private:
    friend struct detail::Engine;
    std::unique_ptr<detail::Job> job_;
};

using Path = std::filesystem::path;
Result compress(const Path& input, const Path& output, const Options& options, Control& control);
// An optional reference is compared byte-for-byte before the restored file is committed.
Result extract(const Path& archive, const Path& output, const Path& reference, Control& control);
Result verify(const Path& archive, const Path& reference, Control& control);

} // namespace chunkpress
