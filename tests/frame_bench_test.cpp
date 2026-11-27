// --bench's frame timing and its log line (app/frame_bench.h), which tools/perf_track.py reads.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app/frame_bench.h"

using namespace hailstone;

TEST_CASE("Frame bench: percentiles by nearest rank, mean and max") {
    std::vector<float> ms;
    for (int i = 100; i >= 1; --i) ms.push_back(static_cast<float>(i)); // 1..100, out of order
    const app::BenchStats s = app::bench_stats(ms);
    CHECK(s.frames == 100);
    CHECK(s.mean_ms == doctest::Approx(50.5));
    CHECK(s.p50_ms == doctest::Approx(50.0));
    CHECK(s.p95_ms == doctest::Approx(95.0));
    CHECK(s.p99_ms == doctest::Approx(99.0));
    CHECK(s.max_ms == doctest::Approx(100.0));

    const app::BenchStats one = app::bench_stats({7.0f});
    CHECK(one.p50_ms == doctest::Approx(7.0));
    CHECK(one.p99_ms == doctest::Approx(7.0));
    CHECK(app::bench_stats({}).frames == 0);
}

TEST_CASE("Frame bench: a long frame among steady ones is a hitch") {
    std::vector<float> ms(200, 5.0f);
    ms[150] = 60.0f;
    CHECK(app::bench_stats(ms).hitches == 1);
    CHECK(app::bench_stats(std::vector<float>(200, 5.0f)).hitches == 0);
}

TEST_CASE("Frame bench: frames are timed only after the scene has been ready for the settle time") {
    app::FrameBench b(3, 2.0);
    CHECK_FALSE(b.add(false, 0.0, 9.0f)); // loading
    CHECK_FALSE(b.add(true, 1.0, 9.0f));  // ready from 1.0
    CHECK_FALSE(b.add(true, 2.5, 9.0f));  // still settling
    CHECK(b.times().empty());
    CHECK_FALSE(b.add(true, 3.0, 4.0f));
    // Another zone-in: the settle and the frames start over.
    CHECK_FALSE(b.add(false, 3.1, 4.0f));
    CHECK(b.times().empty());
    CHECK_FALSE(b.add(true, 4.0, 4.0f));
    CHECK_FALSE(b.add(true, 6.0, 1.0f));
    CHECK_FALSE(b.add(true, 6.1, 2.0f));
    CHECK(b.add(true, 6.2, 3.0f));
    CHECK(b.done());
    CHECK(b.add(false, 7.0, 99.0f)); // done stays done
    CHECK(b.times() == std::vector<float>{1.0f, 2.0f, 3.0f});

    app::FrameBench off(0, 0.0);
    CHECK_FALSE(off.add(true, 0.0, 1.0f));
    CHECK_FALSE(off.done());
}

TEST_CASE("Frame bench: the log line is key=value pairs, memory only where known") {
    app::BenchStats s;
    s.frames = 2;
    s.mean_ms = 1.5;
    s.p50_ms = 1.0;
    s.p95_ms = s.p99_ms = s.max_ms = 2.0;
    app::BenchMemory m;
    m.resident = 512ull * 1024 * 1024;
    m.gpu = 3ull * 1024 * 1024 / 2;
    CHECK(app::bench_line("qeynos hills=2", s, m) ==
          "bench: label=qeynos_hills_2 frames=2 mean_ms=1.500 p50_ms=1.000 p95_ms=2.000 p99_ms=2.000 "
          "max_ms=2.000 hitches=0 resident_mib=512.0 gpu_mib=1.5");
    CHECK(app::bench_line("", s, {}).starts_with("bench: label=unnamed frames=2 "));
}
