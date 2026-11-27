#include "app/frame_bench.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "app/perf_ui.h"
#include "platform/process.h"
#include "render/vk_context.h"

namespace hailstone::app {

BenchStats bench_stats(const std::vector<float>& frame_ms) {
    BenchStats s;
    if (frame_ms.empty()) return s;
    s.frames = static_cast<int>(frame_ms.size());
    std::vector<float> sorted = frame_ms;
    std::sort(sorted.begin(), sorted.end());
    auto rank = [&](double p) {
        auto k = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
        return static_cast<double>(sorted[std::clamp<std::size_t>(k, 1, sorted.size()) - 1]);
    };
    double sum = 0.0;
    for (float ms : sorted) sum += ms;
    s.mean_ms = sum / static_cast<double>(sorted.size());
    s.p50_ms = rank(0.50);
    s.p95_ms = rank(0.95);
    s.p99_ms = rank(0.99);
    s.max_ms = sorted.back();
    HitchBaseline baseline;
    for (float ms : frame_ms)
        if (baseline.add(ms)) ++s.hitches;
    return s;
}

BenchMemory bench_memory(const render::VkContext& vk) {
    BenchMemory m;
    m.resident = platform::resident_memory();
    m.commit = platform::private_commit_memory();
    std::uint64_t gpu = 0;
    bool any = false;
    for (const auto& heap : vk.heap_use())
        if (heap.device_local) {
            gpu += heap.allocated;
            any = true;
        }
    if (any) m.gpu = gpu;
    return m;
}

std::string bench_line(std::string_view label, const BenchStats& stats, const BenchMemory& memory) {
    std::string name(label.empty() ? std::string_view("unnamed") : label);
    for (char& c : name)
        if (c == ' ' || c == '=' || c == '\t') c = '_';
    std::string line = std::format("bench: label={} frames={} mean_ms={:.3f} p50_ms={:.3f} p95_ms={:.3f} "
                                   "p99_ms={:.3f} max_ms={:.3f} hitches={}",
                                   name, stats.frames, stats.mean_ms, stats.p50_ms, stats.p95_ms, stats.p99_ms,
                                   stats.max_ms, stats.hitches);
    constexpr double mib = 1024.0 * 1024.0;
    if (memory.resident) line += std::format(" resident_mib={:.1f}", static_cast<double>(*memory.resident) / mib);
    if (memory.commit) line += std::format(" commit_mib={:.1f}", static_cast<double>(*memory.commit) / mib);
    if (memory.gpu) line += std::format(" gpu_mib={:.1f}", static_cast<double>(*memory.gpu) / mib);
    return line;
}

bool FrameBench::add(bool ready, double now_seconds, float frame_ms) {
    if (wanted_ <= 0) return false;
    if (done()) return true;
    if (!ready) {
        ready_since_.reset();
        times_.clear();
        return false;
    }
    if (!ready_since_) {
        ready_since_ = now_seconds;
        return false;
    }
    if (now_seconds - *ready_since_ < settle_) return false;
    times_.push_back(frame_ms);
    return done();
}

} // namespace hailstone::app
