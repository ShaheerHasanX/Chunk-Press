#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <deque>
#include <functional>
#include <pthread.h>
#include <semaphore.h>

namespace chunkpress::detail {
namespace {

using Clock = std::chrono::steady_clock;

class Semaphore {
public:
    explicit Semaphore(unsigned value) {
        if (sem_init(&value_, 0, value) < 0) systemError("Initialize semaphore", errno);
    }
    ~Semaphore() { sem_destroy(&value_); }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    bool take() {
        while (sem_trywait(&value_) < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return false;
            systemError("Wait for semaphore", errno);
        }
        return true;
    }

    void give() {
        if (sem_post(&value_) < 0) systemError("Post semaphore", errno);
    }

private:
    sem_t value_{};
};

// The job mutex protects both queues. Semaphore changes and deque changes happen
// in the same critical section, including resizing a queue with work already in it.
class Queue {
public:
    explicit Queue(unsigned capacity) : slots_(capacity), capacity_(capacity) {}
    unsigned depth() const { return depth_; }

    void resize(unsigned capacity) {
        const auto unused = [this](unsigned limit) { return limit > depth_ ? limit - depth_ : 0; };
        auto oldFree = unused(capacity_);
        const auto newFree = unused(capacity);
        while (oldFree < newFree) {
            slots_.give();
            ++oldFree;
        }
        while (oldFree > newFree) {
            if (!slots_.take()) throw std::logic_error("Queue slot count is inconsistent");
            --oldFree;
        }
        capacity_ = capacity;
    }

    bool push(Chunk& chunk, bool high) {
        if (!slots_.take()) return false;
        lanes_[high ? 0 : 1].push_back(std::move(chunk));
        ++depth_;
        items_.give();
        return true;
    }

    bool pop(Chunk& chunk) {
        if (!items_.take()) return false;
        auto& lane = lanes_[0].empty() ? lanes_[1] : lanes_[0];
        chunk = std::move(lane.front());
        lane.pop_front();
        --depth_;
        if (depth_ < capacity_) slots_.give();
        return true;
    }

private:
    Semaphore slots_;
    Semaphore items_{0};
    unsigned capacity_;
    unsigned depth_ = 0;
    std::array<std::deque<Chunk>, 2> lanes_;
};

class Threads {
public:
    explicit Threads(Job& job) : job_(job) { ids_.reserve(maxWorkers + 3); }
    ~Threads() {
        for (auto id : ids_) pthread_join(id, nullptr);
    }

    void start(std::function<void()> function) {
        auto payload = std::make_unique<Task>(Task{job_, std::move(function)});
        pthread_t id{};
        const int status = pthread_create(&id, nullptr, invoke, payload.get());
        if (status != 0) systemError("Create POSIX thread", status);
        payload.release();
        ids_.push_back(id);
    }

private:
    struct Task {
        Job& job;
        std::function<void()> function;
    };

    static void* invoke(void* argument) {
        std::unique_ptr<Task> task(static_cast<Task*>(argument));
        try {
            task->function();
        } catch (...) {
            task->job.fail(std::current_exception());
        }
        return nullptr;
    }

    Job& job_;
    std::vector<pthread_t> ids_;
};

class Pipeline {
public:
    Pipeline(Job& job, const Input& input, ArchiveWriter& writer, const Output& output,
             Layout layout, const Options& options)
        : job_(job), input_(input), writer_(writer), output_(output), layout_(layout),
          options_(options), inputQueue_(options.bufferCapacity), outputQueue_(options.bufferCapacity),
          workerSlots_(options.activeLimit == 0 ? options.workers : options.activeLimit),
          readersLeft_(static_cast<unsigned>(std::min<std::uint64_t>(2, layout.count()))),
          workersLeft_(options.workers), inputEnded_(readersLeft_ == 0) {}

    void run() {
        {
            Threads threads(job_);
            try {
                const auto readerCount = readersLeft_;
                threads.start([this] { write(); });
                for (unsigned id = 0; id < options_.workers; ++id) {
                    threads.start([this, id] { work(id); });
                }
                for (unsigned id = 0; id < readerCount; ++id) {
                    threads.start([this] { read(); });
                }
            } catch (...) {
                job_.fail(std::current_exception());
            }
        }
        std::lock_guard lock(job_.mutex);
        job_.check();
    }

private:
    // A resize is applied under the queue lock just before a queue operation.
    void applyCapacity() {
        inputQueue_.resize(job_.targetCapacity);
        outputQueue_.resize(job_.targetCapacity);
    }

    void publish() {
        job_.progress.inputDepth = inputQueue_.depth();
        job_.progress.outputDepth = outputQueue_.depth();
        job_.setPhase();
        job_.changed.notify_all();
    }

    void read() {
        std::unique_lock lock(job_.mutex);
        while (nextId_ < layout_.count() && !job_.stopping()) {
            job_.changed.wait(lock, [this] { return job_.stopping() || !job_.paused; });
            if (job_.stopping() || nextId_ == layout_.count()) break;
            const auto id = nextId_++;
            ++job_.active;
            ++job_.progress.readersActive;
            lock.unlock();
            auto chunk = readChunk(input_, layout_, id);
            lock.lock();
            --job_.active;
            --job_.progress.readersActive;
            publish();

            const bool high = options_.priority == Priority::evenFirst && id % 2 == 0;
            job_.changed.wait(lock, [&] {
                if (job_.stopping()) return true;
                applyCapacity();
                return inputQueue_.push(chunk, high);
            });
            publish();
        }
        if (--readersLeft_ == 0) inputEnded_ = true;
        publish();
    }

    void work(unsigned id) {
        std::unique_lock lock(job_.mutex);
        while (!job_.stopping()) {
            Chunk chunk{};
            bool acquired = false;
            job_.changed.wait(lock, [&] {
                if (job_.stopping() || (inputEnded_ && inputQueue_.depth() == 0)) return true;
                if (job_.paused || !workerSlots_.take()) return false;
                applyCapacity();
                acquired = inputQueue_.pop(chunk);
                if (!acquired) workerSlots_.give();
                return acquired;
            });
            if (!acquired) break;

            ++job_.active;
            job_.progress.workers[id] = WorkerState::compressing;
            publish();
            lock.unlock();
            encode(chunk);
            lock.lock();
            workerSlots_.give();
            --job_.active;
            job_.progress.workers[id] = WorkerState::waitingForWriter;
            publish();

            job_.changed.wait(lock, [&] {
                if (job_.stopping()) return true;
                applyCapacity();
                return outputQueue_.push(chunk, false);
            });
            job_.progress.workers[id] = WorkerState::idle;
            publish();
        }
        job_.progress.workers[id] = WorkerState::stopped;
        if (--workersLeft_ == 0) outputEnded_ = true;
        publish();
    }

    void write() {
        std::unique_lock lock(job_.mutex);
        while (!job_.stopping()) {
            Chunk chunk{};
            bool acquired = false;
            job_.changed.wait(lock, [&] {
                if (job_.stopping() || (outputEnded_ && outputQueue_.depth() == 0)) return true;
                if (job_.paused) return false;
                applyCapacity();
                acquired = outputQueue_.pop(chunk);
                return acquired;
            });
            if (!acquired) break;
            ++job_.active;
            job_.progress.writerActive = true;
            publish();
            lock.unlock();
            writer_.append(chunk);
            lock.lock();
            --job_.active;
            job_.progress.writerActive = false;
            job_.record(chunk, output_.size());
            publish();
        }
    }

    Job& job_;
    const Input& input_;
    ArchiveWriter& writer_;
    const Output& output_;
    const Layout layout_;
    const Options options_;
    Queue inputQueue_;
    Queue outputQueue_;
    Semaphore workerSlots_;
    unsigned readersLeft_;
    unsigned workersLeft_;
    std::uint64_t nextId_ = 0;
    bool inputEnded_;
    bool outputEnded_ = false;
};

class FinishJob {
public:
    explicit FinishJob(Job& job) : job_(job) {}
    ~FinishJob() { job_.finish(); }
private:
    Job& job_;
};

void runSequential(Job& job, const Input& input, ArchiveWriter& writer, const Output& output,
                   Layout layout) {
    for (std::uint64_t id = 0; id < layout.count(); ++id) {
        {
            std::unique_lock lock(job.mutex);
            job.waitForWork(lock);
            job.progress.workers[0] = WorkerState::compressing;
        }
        auto chunk = readChunk(input, layout, id);
        encode(chunk);
        writer.append(chunk);
        {
            std::lock_guard lock(job.mutex);
            --job.active;
            job.progress.workers[0] = WorkerState::idle;
            job.record(chunk, output.size());
            job.setPhase();
        }
    }
}

void validateOptions(const Options& options) {
    if (options.workers == 0 || options.workers > maxWorkers || options.activeLimit > options.workers
        || options.chunkSize < minChunk || options.chunkSize > maxChunk
        || options.bufferCapacity == 0 || options.bufferCapacity > maxBuffer) {
        throw std::runtime_error("Settings out of range");
    }
    // At most two resident queue backlogs plus a raw and encoded buffer per worker.
    const auto poolSize = options.sequential ? 1 : options.workers;
    const auto bufferCapacity = options.sequential ? 1 : options.bufferCapacity;
    if (std::uint64_t{2 * (bufferCapacity + poolSize + 3)} * options.chunkSize > (512ull << 20)) {
        throw std::runtime_error("Settings exceed the 512 MiB chunk-buffer budget. Reduce chunk size, workers or buffer capacity.");
    }
}

} // namespace

void Job::check() const {
    if (failure) std::rethrow_exception(failure);
    if (cancelled) throw Cancelled();
}

void Job::begin(Progress initial, unsigned capacityLimit) {
    std::lock_guard lock(mutex);
    if (started) throw std::logic_error("Use a fresh control object for each operation");
    check();
    started = true;
    progress = initial;
    targetCapacity = initial.bufferCapacity;
    maxCapacity = capacityLimit;
    setPhase();
}

void Job::finish() {
    std::lock_guard lock(mutex);
    finished = true;
    progress.readersActive = 0;
    progress.writerActive = false;
    progress.inputDepth = 0;
    progress.outputDepth = 0;
    std::fill_n(progress.workers, progress.workerCount, WorkerState::stopped);
    setPhase();
    changed.notify_all();
}

void Job::finalize() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [this] { return !paused || stopping(); });
    check();
    sealed = true;
    setPhase();
}

void Job::record(const Chunk& chunk, std::uint64_t bytes) {
    progress.processedBytes += chunk.original;
    ++progress.completedChunks;
    progress.archiveBytes = bytes;
}

void Job::fail(std::exception_ptr error) {
    std::lock_guard lock(mutex);
    if (!failure && !cancelled) failure = error;
    setPhase();
    changed.notify_all();
}

void Job::waitForWork(std::unique_lock<std::mutex>& lock) {
    changed.wait(lock, [this] { return !paused || stopping(); });
    check();
    ++active;
}

void Job::setPhase() {
    if (finished) progress.phase = Phase::finished;
    else if (stopping()) progress.phase = Phase::cancelling;
    else if (sealed) progress.phase = Phase::finalizing;
    else if (paused) progress.phase = active ? Phase::pausing : Phase::paused;
    else progress.phase = started ? Phase::running : Phase::ready;
}

Result Engine::compress(const Path& path, const Path& destination, const Options& options, Control& control) {
    const auto start = Clock::now();
    validateOptions(options);
    Input input(path);
    const Layout layout{input.size(), options.chunkSize};
    if (layout.count() > maxChunks) throw std::runtime_error("Too many chunks. Choose a larger chunk size.");
    unsigned maxCapacity = 0;
    Progress initial;
    initial.originalBytes = layout.original;
    initial.totalChunks = layout.count();
    initial.workerCount = options.sequential ? 1 : options.workers;
    if (!options.sequential) {
        initial.bufferCapacity = options.bufferCapacity;
        maxCapacity = std::min(maxBuffer, (512u << 20) / (2 * options.chunkSize) - options.workers - 3);
    }
    auto& job = *control.job_;
    job.begin(initial, maxCapacity);
    FinishJob done(job);
    Output output(destination);
    ArchiveWriter writer(output, layout);
    if (options.sequential) runSequential(job, input, writer, output, layout);
    else Pipeline(job, input, writer, output, layout, options).run();

    job.finalize();
    writer.finish();
    input.checkUnchanged();
    output.commit();
    {
        std::lock_guard lock(job.mutex);
        job.progress.archiveBytes = output.size();
    }
    return {layout.original, output.size(), layout.count(), std::chrono::duration<double>(Clock::now() - start).count()};
}

Result Engine::restore(const Path& path, const Path& destination, const Path& referencePath, Control& control) {
    const auto start = Clock::now();
    Input input(path);
    std::vector<Entry> index;
    const auto layout = readIndex(input, index);
    std::unique_ptr<Input> reference;
    if (!referencePath.empty()) {
        reference = std::make_unique<Input>(referencePath);
        if (reference->size() != layout.original) throw std::runtime_error("Reference has a different size");
    }

    auto& job = *control.job_;
    Progress initial;
    initial.originalBytes = layout.original;
    initial.totalChunks = layout.count();
    initial.archiveBytes = input.size();
    job.begin(initial);
    FinishJob done(job);
    std::unique_ptr<Output> output;
    if (!destination.empty()) output = std::make_unique<Output>(destination);

    Bytes packed, decoded, source;
    for (const auto& entry : index) {
        {
            std::unique_lock lock(job.mutex);
            job.waitForWork(lock);
        }
        decode(input, entry, packed, decoded);
        if (reference) {
            source.resize(entry.original);
            reference->read(entry.id * layout.chunkSize, source.data(), source.size());
            if (source != decoded) {
                throw std::runtime_error("Reference differs at chunk " + std::to_string(entry.id));
            }
        }
        if (output) output->write(decoded.data(), decoded.size());
        {
            std::lock_guard lock(job.mutex);
            --job.active;
            job.progress.processedBytes += entry.original;
            ++job.progress.completedChunks;
            job.setPhase();
        }
    }

    job.finalize();
    input.checkUnchanged();
    if (reference) reference->checkUnchanged();
    if (output) output->commit();
    return {layout.original, input.size(), layout.count(), std::chrono::duration<double>(Clock::now() - start).count()};
}

} // namespace chunkpress::detail

namespace chunkpress {

Control::Control() : job_(std::make_unique<detail::Job>()) {}
Control::~Control() = default;

void Control::pause() {
    std::lock_guard lock(job_->mutex);
    if (!job_->stopping() && !job_->sealed && !job_->finished) {
        job_->paused = true;
        job_->setPhase();
    }
}

void Control::resume() {
    std::lock_guard lock(job_->mutex);
    job_->paused = false;
    job_->setPhase();
    job_->changed.notify_all();
}

void Control::cancel() {
    std::lock_guard lock(job_->mutex);
    if (!job_->sealed && !job_->finished) {
        job_->cancelled = true;
        job_->setPhase();
        job_->changed.notify_all();
    }
}

void Control::resize(unsigned capacity) {
    std::lock_guard lock(job_->mutex);
    if (capacity == 0 || capacity > job_->maxCapacity) {
        throw std::runtime_error("Buffer capacity exceeds the current settings' memory budget");
    }
    if (!job_->stopping() && !job_->sealed && !job_->finished) {
        job_->targetCapacity = capacity;
        job_->progress.bufferCapacity = capacity;
        job_->changed.notify_all();
    }
}

Progress Control::progress() const {
    std::lock_guard lock(job_->mutex);
    return job_->progress;
}

Result compress(const Path& input, const Path& output, const Options& options, Control& control) {
    return detail::Engine::compress(input, output, options, control);
}

Result extract(const Path& archive, const Path& output, const Path& reference, Control& control) {
    if (output.empty()) throw std::runtime_error("Choose an output filename");
    return detail::Engine::restore(archive, output, reference, control);
}

Result verify(const Path& archive, const Path& reference, Control& control) {
    return detail::Engine::restore(archive, {}, reference, control);
}

} // namespace chunkpress
