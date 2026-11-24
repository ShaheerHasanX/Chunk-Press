#include "chunkpress.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <future>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

volatile std::sig_atomic_t interrupted = 0;

void interrupt(int) {
    interrupted = 1;
}

const char* usage = R"(Usage:
  chunkpress-cli compress INPUT OUTPUT [--workers N] [--chunk-kib N]
      [--capacity N] [--active-limit N] [--priority fifo|even] [--sequential] [--json]
  chunkpress-cli extract ARCHIVE OUTPUT [--reference ORIGINAL] [--json]
  chunkpress-cli verify ARCHIVE [ORIGINAL] [--json]

Existing outputs are protected. Ctrl+C cancels at a chunk boundary.
)";

unsigned number(std::string_view raw, unsigned minimum, unsigned maximum) {
    unsigned result{};
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()
        || result < minimum || result > maximum) {
        throw std::runtime_error("Expected an integer from " + std::to_string(minimum) + " to "
                                 + std::to_string(maximum) + ", got " + std::string(raw));
    }
    return result;
}

void printResult(const chunkpress::Result& result, bool json, const char* message) {
    const auto ratio = result.originalBytes == 0 ? 0.0 :
        static_cast<double>(result.archiveBytes) / result.originalBytes;
    std::cout << std::fixed << std::setprecision(6);
    if (json) {
        std::cout << "{\"original_bytes\":" << result.originalBytes
                  << ",\"archive_bytes\":" << result.archiveBytes
                  << ",\"chunks\":" << result.chunks
                  << ",\"seconds\":" << result.seconds << ",\"archive_ratio\":";
        if (result.originalBytes) std::cout << ratio;
        else std::cout << "null";
        std::cout << "}\n";
        return;
    }
    std::cout << message << "\nOriginal: " << result.originalBytes << " bytes\nArchive: "
              << result.archiveBytes << " bytes\nChunks: " << result.chunks << "\nArchive / original: ";
    if (result.originalBytes) std::cout << std::setprecision(2) << 100 * ratio << "%\n";
    else std::cout << "n/a (empty file)\n";
    std::cout << std::setprecision(3) << "Time: " << result.seconds << " s\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << usage;
            return 0;
        }
        if (argc < 3) throw std::runtime_error(usage);
        const std::string_view command = argv[1];
        const bool compress = command == "compress";
        const bool extract = command == "extract";
        const bool verify = command == "verify";
        if (!compress && !extract && !verify) throw std::runtime_error(usage);

        const chunkpress::Path input = argv[2];
        chunkpress::Path output, reference;
        int from = 3;
        if (compress || extract) {
            if (argc < 4) throw std::runtime_error(usage);
            output = argv[from++];
        } else if (argc > 3 && std::string_view(argv[3]) != "--json") {
            reference = argv[from++];
        }

        chunkpress::Options options;
        bool json = false;
        for (int i = from; i < argc; ++i) {
            const std::string_view argument = argv[i];
            const auto value = [&]() -> std::string_view {
                if (++i == argc) throw std::runtime_error("Missing value for " + std::string(argument));
                return argv[i];
            };
            if (argument == "--json") json = true;
            else if (extract && argument == "--reference") reference = value();
            else if (compress && argument == "--workers") options.workers = number(value(), 1, chunkpress::maxWorkers);
            else if (compress && argument == "--chunk-kib") {
                options.chunkSize = number(value(), chunkpress::minChunk / 1024, chunkpress::maxChunk / 1024) * 1024;
            } else if (compress && argument == "--capacity") options.bufferCapacity = number(value(), 1, chunkpress::maxBuffer);
            else if (compress && argument == "--active-limit") options.activeLimit = number(value(), 1, chunkpress::maxWorkers);
            else if (compress && argument == "--sequential") options.sequential = true;
            else if (compress && argument == "--priority") {
                const auto order = value();
                if (order != "fifo" && order != "even") throw std::runtime_error("Priority must be fifo or even");
                options.priority = order == "fifo" ? chunkpress::Priority::fifo : chunkpress::Priority::evenFirst;
            } else {
                throw std::runtime_error("Unknown option " + std::string(argument));
            }
        }

        struct sigaction handler{};
        handler.sa_handler = interrupt;
        sigemptyset(&handler.sa_mask);
        if (sigaction(SIGINT, &handler, nullptr) < 0 || sigaction(SIGTERM, &handler, nullptr) < 0) {
            throw std::runtime_error("Could not install cancellation handlers");
        }
        chunkpress::Control control;
        auto result = std::async(std::launch::async, [&] {
            if (compress) return chunkpress::compress(input, output, options, control);
            if (extract) return chunkpress::extract(input, output, reference, control);
            return chunkpress::verify(input, reference, control);
        });
        while (result.wait_for(std::chrono::milliseconds(15)) != std::future_status::ready) {
            if (interrupted) control.cancel();
        }
        const char* message = "Archive created.";
        if (extract) message = reference.empty() ? "File extracted. Archive checks passed." : "Extracted and matched to the original byte-for-byte.";
        if (verify) message = reference.empty() ? "Archive integrity checks passed." : "Archive matches the original byte-for-byte.";
        printResult(result.get(), json, message);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return interrupted ? 130 : 1;
    }
}
