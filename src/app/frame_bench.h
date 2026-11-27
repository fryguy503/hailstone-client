#pragma once

// --bench N (docs/ci.md "Nightly performance"): once the scene is ready (the client: in the zone, its settle over;
// the zone viewer: from its first frame) and has stayed ready for a settle time, N frames are timed and summed up in
// one log line that tools/perf_track.py reads:
//
//   bench: label=<zone> frames=N mean_ms=... p50_ms=... p95_ms=... p99_ms=... max_ms=... hitches=...
//          resident_mib=... commit_mib=... gpu_mib=...
//
// on a single line, the memory fields only where the system reports them. Then the program quits. A scene that stops
// being ready (another zone-in) starts the settle and the frames over.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::render { class VkContext; }

namespace hailstone::app {

struct BenchStats {
    int frames = 0;
    double mean_ms = 0.0, p50_ms = 0.0, p95_ms = 0.0, p99_ms = 0.0, max_ms = 0.0;
    int hitches = 0; // app::HitchBaseline's hitches among the timed frames, in their order
};

// The frame times' summary; percentiles by nearest rank (the smallest time at least that share of frames is at or
// under). All zero for no frames.
BenchStats bench_stats(const std::vector<float>& frame_ms);

struct BenchMemory {
    std::optional<std::uint64_t> resident, commit; // bytes (platform::resident_memory, private_commit_memory)
    std::optional<std::uint64_t> gpu;              // bytes the allocator holds in device-local heaps
};

// The process's memory now, and the device-local bytes `vk`'s allocator holds.
BenchMemory bench_memory(const render::VkContext& vk);

// The log line above, without a newline. `label` has its spaces and '=' replaced by '_', so the line stays
// key=value pairs.
std::string bench_line(std::string_view label, const BenchStats& stats, const BenchMemory& memory);

class FrameBench {
public:
    // `frames` > 0 to time; `settle_seconds` the scene must have been ready before the first timed frame.
    FrameBench(int frames, double settle_seconds) : wanted_(frames), settle_(settle_seconds) {}

    // Each frame: whether the scene is ready, the time now (seconds, any steady origin), and the last frame's time.
    // True once `frames` frames are timed (and from then on).
    bool add(bool ready, double now_seconds, float frame_ms);
    bool done() const { return wanted_ > 0 && static_cast<int>(times_.size()) >= wanted_; }
    const std::vector<float>& times() const { return times_; }

private:
    int wanted_;
    double settle_;
    std::optional<double> ready_since_;
    std::vector<float> times_;
};

} // namespace hailstone::app
