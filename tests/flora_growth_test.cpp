// render::FloraGrowth: which zone flora tiles are grown, growing or to grow, off the frame (FloraRenderer's
// bookkeeping, without Vulkan).
#include <doctest/doctest.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "render/flora_growth.h"

using hailstone::render::FloraGrowth;

namespace {

struct Tile {
    int made = 0; // which job made it
    bool stale = false;
};
using Growth = FloraGrowth<Tile>;
using Near = std::vector<std::pair<float, std::uint32_t>>;

// Holds the worker inside a job until released.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void pass() {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return open; });
    }
    void release() {
        std::lock_guard lock(m);
        open = true;
        cv.notify_all();
    }
};

const auto kAll = [](std::uint32_t) { return true; };

Near near_of(std::initializer_list<std::uint32_t> tiles) {
    Near out;
    float d = 0.0f;
    for (std::uint32_t t : tiles) out.emplace_back(d += 1.0f, t);
    return out;
}

} // namespace

TEST_CASE("flora growth: the nearest tiles are queued a few at a time and kept once grown") {
    Growth growth(2);
    int jobs = 0;
    std::vector<std::uint32_t> asked;
    const auto make = [&](std::uint32_t t) -> Growth::Job {
        asked.push_back(t);
        const int n = ++jobs;
        return [n] { return Tile{n}; };
    };
    const Near near = near_of({7, 3, 9});
    growth.update(near, kAll, make);
    CHECK(asked == std::vector<std::uint32_t>{7, 3}); // nearest first, two in flight
    CHECK(growth.growing(7));
    CHECK(growth.growing(3));
    CHECK_FALSE(growth.growing(9));
    growth.wait_idle();
    growth.update(near, kAll, make); // takes 7 and 3, queues 9
    CHECK(growth.grown().size() == 2);
    CHECK(asked == std::vector<std::uint32_t>{7, 3, 9});
    growth.wait_idle();
    growth.update(near, kAll, make);
    CHECK(growth.grown().size() == 3);
    CHECK(growth.growing_count() == 0);
    growth.update(near, kAll, make);
    CHECK(asked.size() == 3); // a grown tile is not grown again
}

TEST_CASE("flora growth: tiles out of keep are let go, growing or grown") {
    Growth growth(3);
    Gate gate;
    const auto make = [&](std::uint32_t t) -> Growth::Job {
        return [&gate, t] {
            if (t == 2) gate.pass();
            return Tile{static_cast<int>(t)};
        };
    };
    growth.update(near_of({1, 2}), kAll, make);
    // 1 grows; 2 is held in its job.
    std::set<std::uint32_t> keep{1, 2};
    const auto kept = [&](std::uint32_t t) { return keep.contains(t); };
    keep.erase(2); // 2 leaves keep while growing: its job is dropped
    growth.update(Near{}, kept, make);
    CHECK_FALSE(growth.growing(2));
    gate.release();
    growth.wait_idle();
    growth.update(Near{}, kept, make);
    CHECK(growth.grown().contains(1));
    CHECK_FALSE(growth.grown().contains(2)); // its result is never handed back
    keep.clear();
    growth.update(Near{}, kept, make);
    CHECK(growth.grown().empty());
}

TEST_CASE("flora growth: a regrown tile is drawn as it was until it has grown again") {
    Growth growth(3);
    int jobs = 0;
    const auto make = [&](std::uint32_t) -> Growth::Job {
        const int n = ++jobs;
        return [n] { return Tile{n}; };
    };
    const Near near = near_of({1, 2});
    growth.update(near, kAll, make);
    growth.wait_idle();
    growth.update(near, kAll, make);
    REQUIRE(growth.grown().size() == 2);
    const int first = growth.grown().at(1).made;
    growth.regrow([](std::uint32_t t) { return t == 1; });
    CHECK(growth.grown().at(1).stale); // still there, to be drawn
    CHECK_FALSE(growth.grown().at(2).stale);
    growth.update(near, kAll, make); // queued again
    CHECK(growth.growing(1));
    CHECK(growth.grown().contains(1));
    growth.wait_idle();
    growth.update(near, kAll, make);
    CHECK_FALSE(growth.grown().at(1).stale);
    CHECK(growth.grown().at(1).made != first);
    CHECK(jobs == 3);
}

TEST_CASE("flora growth: a tile growing with the old cover is dropped by regrow and queued again") {
    Growth growth(3);
    Gate gate;
    int jobs = 0;
    const auto make = [&](std::uint32_t) -> Growth::Job {
        const int n = ++jobs;
        return [&gate, n] {
            if (n == 1) gate.pass();
            return Tile{n};
        };
    };
    const Near near = near_of({5});
    growth.update(near, kAll, make);
    CHECK(growth.growing(5));
    growth.regrow([](std::uint32_t) { return true; });
    CHECK_FALSE(growth.growing(5));
    growth.update(near, kAll, make); // queued again with the new cover
    CHECK(growth.growing(5));
    gate.release();
    growth.wait_idle();
    growth.update(near, kAll, make);
    REQUIRE(growth.grown().contains(5));
    CHECK(growth.grown().at(5).made == 2); // the first job's result is never handed back
}

TEST_CASE("flora growth: restart lets go of every tile") {
    Growth growth(3);
    Gate gate;
    const auto make = [&](std::uint32_t t) -> Growth::Job {
        return [&gate, t] {
            if (t == 2) gate.pass();
            return Tile{static_cast<int>(t)};
        };
    };
    growth.update(near_of({1}), kAll, make);
    growth.wait_idle();
    growth.update(near_of({1, 2}), kAll, make);
    REQUIRE(growth.grown().contains(1));
    REQUIRE(growth.growing(2));
    growth.restart();
    CHECK(growth.grown().empty());
    CHECK(growth.growing_count() == 0);
    gate.release();
    growth.wait_idle();
    growth.update(Near{}, kAll, make);
    CHECK(growth.grown().empty()); // what grew from the old flora is dropped
}

TEST_CASE("flora growth: a tile whose growing throws waits for a change instead of being queued every frame") {
    Growth growth(3);
    int jobs = 0;
    bool fail = true;
    const auto make = [&](std::uint32_t) -> Growth::Job {
        ++jobs;
        return [fail]() -> Tile {
            if (fail) throw std::runtime_error("bad tile");
            return Tile{1};
        };
    };
    const Near near = near_of({4});
    growth.update(near, kAll, make);
    growth.wait_idle();
    growth.update(near, kAll, make);
    CHECK(growth.failed(4));
    CHECK_FALSE(growth.growing(4));
    for (int frame = 0; frame < 5; ++frame) growth.update(near, kAll, make);
    CHECK(jobs == 1); // not queued again
    // The cover there changes: tried again.
    fail = false;
    growth.regrow([](std::uint32_t) { return true; });
    CHECK_FALSE(growth.failed(4));
    growth.update(near, kAll, make);
    growth.wait_idle();
    growth.update(near, kAll, make);
    CHECK(jobs == 2);
    CHECK(growth.grown().contains(4));
    // Failing again, then leaving reach and coming back: tried again.
    fail = true;
    growth.restart();
    growth.update(near, kAll, make);
    growth.wait_idle();
    growth.update(near, kAll, make);
    REQUIRE(growth.failed(4));
    growth.update(Near{}, [](std::uint32_t) { return false; }, make);
    CHECK_FALSE(growth.failed(4));
}
