// Experimental shared-texture coordinator and procedural cross-process consumer proof. No game assets are used.
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

#include "core/env.h"
#include "render/gpu_shared_cache.h"

using namespace hailstone;
namespace {
std::uint32_t number(std::string_view text) {
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::invalid_argument("Invalid GPU cache numeric option");
    return value;
}
}
int main(int argc, char** argv) {
    try {
        render::GpuCacheBrokerOptions options;
        if (const char* model = core::env("GPU_NAME")) options.device_model = model;
        enum class Mode { None, Serve, Proof, Status, Stop } mode = Mode::None;
        std::uint32_t iterations = 32;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help") {
                std::puts("Usage: hailstone_gpu_cache_broker --serve [--hold-seconds N] [--validation]\n"
                    "       hailstone_gpu_cache_broker --consumer-proof [--iterations N] [--validation]\n"
                    "       hailstone_gpu_cache_broker --status | --stop-if-idle\n"
                    "GPU: HAILSTONE_GPU_NAME or --device-model NAME / --device-index N.\n"
                    "Authenticated local endpoint and token are accepted only through HAILSTONE_GPU_IPC_* environment variables.");
                return 0;
            }
            if (arg == "--validation") { options.validation = true; continue; }
            if (arg == "--serve" || arg == "--consumer-proof" || arg == "--status" || arg == "--stop-if-idle") {
                if (mode != Mode::None) throw std::invalid_argument("Select exactly one GPU cache tool mode");
                mode = arg == "--serve" ? Mode::Serve : arg == "--consumer-proof" ? Mode::Proof : arg == "--status" ? Mode::Status : Mode::Stop;
                continue;
            }
            if (arg != "--device-model" && arg != "--device-index" && arg != "--hold-seconds" && arg != "--iterations" &&
                arg != "--budget-mib" && arg != "--max-textures") throw std::invalid_argument("Unknown GPU cache option");
            if (++i == argc) throw std::invalid_argument("GPU cache option needs a value");
            if (arg == "--device-model") { options.device_model = argv[i]; continue; }
            const auto value = number(argv[i]);
            if (arg == "--device-index") options.device_index = value;
            else if (arg == "--hold-seconds") options.hold_seconds = value;
            else if (arg == "--iterations") iterations = value;
            else if (arg == "--budget-mib") options.budget_bytes = std::uint64_t{value} << 20;
            else options.maximum_entries = value;
        }
        if (mode == Mode::Serve) return render::run_gpu_cache_broker(options);
        if (mode == Mode::Proof) return render::run_gpu_cache_consumer_proof(options, iterations);
        if (mode == Mode::Status || mode == Mode::Stop) return render::run_gpu_cache_control(mode == Mode::Stop);
        throw std::invalid_argument("Select a GPU cache tool mode; see --help");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU cache: %s\n", error.what()); return 1;
    }
}
