#include "chunkpress.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using namespace chunkpress;

namespace {

void require(bool condition, const char* explanation) {
    if (!condition) throw std::runtime_error(explanation);
}

template<class Condition>
void waitUntil(Condition condition, const char* explanation) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, explanation);
        std::this_thread::sleep_for(1ms);
    }
}

class TempDirectory {
public:
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "chunkpress-core-XXXXXX").string();
        auto* result = ::mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("Cannot create a test directory");
        path = result;
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    Path path;
};

void writeRandom(const Path& path, unsigned mebibytes) {
    std::ofstream output(path, std::ios::binary);
    std::mt19937 generator(42);
    std::string buffer(1024 * 1024, '\0');
    for (unsigned i = 0; i < mebibytes; ++i) {
        for (auto& byte : buffer) byte = static_cast<char>(generator() >> 24);
        output.write(buffer.data(), buffer.size());
    }
    require(bool(output), "Could not write the fixture");
}

void confirmNoTemporaries(const Path& directory) {
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        require(item.path().filename().string().find(".chunkpress-") != 0, "Output temporary file leaked");
    }
}

void pauseResizeAndResume(const Path& source, const Path& target) {
    Options options;
    options.workers = 4;
    options.activeLimit = 1;
    options.chunkSize = 256 * 1024;
    options.bufferCapacity = 8;
    options.priority = Priority::evenFirst;
    Control control;
    control.pause();
    auto job = std::async(std::launch::async, [&] { return compress(source, target, options, control); });
    try {
        waitUntil([&] { return control.progress().totalChunks > 0; }, "Pipeline never started");
        require(control.progress().phase == Phase::paused, "Preflight pause was ignored");
        control.resize(16);
        control.resume();
        waitUntil([&] {
            const auto progress = control.progress();
            unsigned active = 0;
            for (unsigned i = 0; i < progress.workerCount; ++i) {
                if (progress.workers[i] == WorkerState::compressing) ++active;
            }
            require(active <= 1, "Active worker semaphore allowed too many workers");
            return active == 1 && progress.completedChunks > 0;
        }, "No live compression work was observed");

        control.pause();
        waitUntil([&] { return control.progress().phase == Phase::paused; }, "Pipeline failed to quiesce");
        const auto paused = control.progress();
        control.resize(1);
        std::this_thread::sleep_for(25ms);
        const auto stillPaused = control.progress();
        require(paused.processedBytes == stillPaused.processedBytes, "Paused job continued writing chunks");
        require(stillPaused.bufferCapacity == 1, "Live shrink did not update the queue target");
        require(stillPaused.readersActive == 0 && !stillPaused.writerActive, "Paused job retained active I/O");
        require(!std::filesystem::exists(target), "Unfinished archive was visible");
        control.resume();
        const auto result = job.get();
        require(result.originalBytes == std::filesystem::file_size(source), "Wrong original size");
    } catch (...) {
        control.cancel();
        throw;
    }
    Control verifier;
    verify(target, source, verifier);
    confirmNoTemporaries(target.parent_path());
}

void cancelWhilePaused(const Path& source, const Path& target) {
    Options options;
    options.bufferCapacity = 1;
    Control control;
    control.pause();
    auto job = std::async(std::launch::async, [&] { return compress(source, target, options, control); });
    try {
        waitUntil([&] { return control.progress().totalChunks > 0; }, "Cancelled job never reached the pipeline");
        control.cancel();
        bool cancelled = false;
        try {
            job.get();
        } catch (const std::exception& error) {
            cancelled = std::string(error.what()) == "Operation cancelled";
        }
        require(cancelled, "Paused compression did not acknowledge cancellation");
    } catch (...) {
        control.cancel();
        throw;
    }
    require(!std::filesystem::exists(target), "Cancelled archive was published");
    confirmNoTemporaries(target.parent_path());
}

void competeForOutput(const Path& source, const Path& target) {
    const auto create = [&] {
        Control control;
        Options options;
        options.workers = 2;
        try {
            compress(source, target, options, control);
            return 1;
        } catch (const std::exception& error) {
            const auto message = std::string(error.what());
            require(message.find("exists") != std::string::npos, "Unexpected competing-writer error");
            return 0;
        }
    };
    auto first = std::async(std::launch::async, create);
    auto second = std::async(std::launch::async, create);
    require(first.get() + second.get() == 1, "More or fewer than one archive was committed");
    Control verifier;
    verify(target, source, verifier);
    confirmNoTemporaries(target.parent_path());
}

} // namespace

int main() {
    try {
        TempDirectory directory;
        const auto large = directory.path / "data.bin";
        const auto small = directory.path / "small.txt";
        writeRandom(large, 24);
        std::ofstream(small) << "chunkpress\n";
        pauseResizeAndResume(large, directory.path / "paused.cpa");
        cancelWhilePaused(large, directory.path / "cancelled.cpa");
        competeForOutput(small, directory.path / "contested.cpa");
        std::cout << "Pause, live resize, resume, worker cap, cancellation, and competing output passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
