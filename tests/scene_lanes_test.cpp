// render::SceneLanes: the scene recorder's lanes (render/scene_recorder.h), recorded on the workers.
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "core/worker_pool.h"
#include "render/scene_lanes.h"

using namespace hailstone;

TEST_CASE("Scene lanes: each lane's jobs record in order on one thread, and the jobs keep the order they came in") {
    const std::array<int, 4> owners{};
    // Jobs from four renderers, interleaved as a frame queues them.
    const std::vector<std::size_t> order{0, 1, 0, 2, 1, 0, 3, 3, 2, 0, 1, 1, 0};
    for (unsigned workers : {0u, 1u, 3u}) {
        CAPTURE(workers);
        core::WorkerPool pool(workers);
        core::set_workers(&pool);
        render::SceneLanes lanes;
        for (int frame = 0; frame < 50; ++frame) {
            lanes.clear();
            for (std::size_t owner : order) lanes.add(&owners[owner]);
            REQUIRE(lanes.jobs() == order.size());
            REQUIRE(lanes.lanes() == 4);
            for (std::size_t j = 0; j < order.size(); ++j) REQUIRE(lanes.lane_of(j) == order[j]); // by first use
            // A fake command buffer per job: what its lane had recorded before it, and the thread that recorded it.
            struct Recorded {
                int calls = 0, before = -1;
                std::thread::id thread;
            };
            std::vector<Recorded> buffers(order.size());
            std::array<int, 4> recorded{};
            std::mutex mutex;
            lanes.record([&](std::size_t j) {
                const std::size_t lane = lanes.lane_of(j);
                std::lock_guard lock(mutex);
                ++buffers[j].calls;
                buffers[j].before = recorded[lane]++;
                buffers[j].thread = std::this_thread::get_id();
            });
            // The buffers executed in the jobs' order: each once, each lane's in order, on its lane's one thread.
            std::array<int, 4> seen{};
            std::array<std::thread::id, 4> thread{};
            for (std::size_t j = 0; j < order.size(); ++j) {
                const std::size_t lane = order[j];
                REQUIRE(buffers[j].calls == 1);
                CHECK(buffers[j].before == seen[lane]++);
                if (thread[lane] == std::thread::id{}) thread[lane] = buffers[j].thread;
                CHECK(buffers[j].thread == thread[lane]);
            }
        }
        core::set_workers(nullptr);
    }
}

TEST_CASE("Scene lanes: cleared, a frame starts with no lanes") {
    int a = 0, b = 0;
    render::SceneLanes lanes;
    CHECK(lanes.add(&a) == 0);
    CHECK(lanes.add(&b) == 1);
    lanes.clear();
    CHECK(lanes.jobs() == 0);
    CHECK(lanes.lanes() == 0);
    CHECK(lanes.add(&b) == 0);
    int calls = 0;
    lanes.record([&](std::size_t j) {
        CHECK(j == 0);
        ++calls;
    });
    CHECK(calls == 1);
    lanes.clear();
    lanes.record([&](std::size_t) { ++calls; });
    CHECK(calls == 1);
}

TEST_CASE("Scene lanes: the shadow layers' targets, each executed in its jobs' order, a renderer's layers on one thread") {
    // As the frame queues the shadow layers: for each layer (the target) the zone, the characters (the sun's cascades
    // only) and the objects, each renderer a lane across the layers.
    const std::array<int, 3> renderers{};
    constexpr std::size_t kLayers = 6, kCascades = 4;
    for (unsigned workers : {0u, 1u, 3u}) {
        CAPTURE(workers);
        core::WorkerPool pool(workers);
        core::set_workers(&pool);
        render::SceneLanes lanes;
        for (int frame = 0; frame < 50; ++frame) {
            lanes.clear();
            std::vector<std::pair<std::size_t, std::size_t>> queued; // (renderer, layer) per job
            for (std::size_t layer = 0; layer < kLayers; ++layer)
                for (std::size_t r = 0; r < renderers.size(); ++r) {
                    if (r == 1 && layer >= kCascades) continue;
                    lanes.add(&renderers[r], layer);
                    queued.push_back({r, layer});
                }
            REQUIRE(lanes.lanes() == renderers.size());
            for (std::size_t j = 0; j < queued.size(); ++j) {
                CHECK(lanes.lane_of(j) == queued[j].first);
                CHECK(lanes.target_of(j) == queued[j].second);
            }
            // Recorded: each lane's layers in order on one thread.
            std::vector<std::size_t> layer_seen(queued.size(), kLayers);
            std::vector<std::thread::id> thread(queued.size());
            std::array<std::size_t, 3> next_layer{};
            std::array<bool, 3> in_order{true, true, true};
            std::mutex mutex;
            lanes.record([&](std::size_t j) {
                std::lock_guard lock(mutex);
                const std::size_t r = lanes.lane_of(j);
                if (lanes.target_of(j) < next_layer[r]) in_order[r] = false;
                next_layer[r] = lanes.target_of(j);
                layer_seen[j] = lanes.target_of(j);
                thread[j] = std::this_thread::get_id();
            });
            for (bool ok : in_order) CHECK(ok);
            for (std::size_t j = 0; j < queued.size(); ++j) {
                CHECK(layer_seen[j] == queued[j].second);
                for (std::size_t k = 0; k < queued.size(); ++k)
                    if (queued[k].first == queued[j].first) CHECK(thread[k] == thread[j]);
            }
            // Executed: each layer's jobs in the order they came (zone, characters, objects), every job once.
            std::size_t executed = 0;
            for (std::size_t layer = 0; layer < kLayers; ++layer) {
                std::vector<std::size_t> order;
                lanes.each_in(layer, [&](std::size_t j) { order.push_back(j); });
                std::vector<std::size_t> expected;
                for (std::size_t j = 0; j < queued.size(); ++j)
                    if (queued[j].second == layer) expected.push_back(j);
                CHECK(order == expected);
                CHECK(order.size() == (layer < kCascades ? 3u : 2u));
                executed += order.size();
            }
            CHECK(executed == queued.size());
        }
        core::set_workers(nullptr);
    }
}

TEST_CASE("Scene lanes: a job queued without a target draws into the first") {
    int a = 0;
    render::SceneLanes lanes;
    lanes.add(&a);
    lanes.add(&a, 2);
    CHECK(lanes.target_of(0) == 0);
    CHECK(lanes.target_of(1) == 2);
    int calls = 0;
    lanes.each_in(1, [&](std::size_t) { ++calls; });
    CHECK(calls == 0);
    lanes.each_in(0, [&](std::size_t j) { CHECK(j == 0); ++calls; });
    CHECK(calls == 1);
    lanes.clear();
    lanes.each_in(0, [&](std::size_t) { ++calls; });
    CHECK(calls == 1);
}

TEST_CASE("Scene lanes: a frame after a busier one records only its own jobs") {
    int a = 0, b = 0, c = 0;
    render::SceneLanes lanes;
    lanes.add(&a);
    lanes.add(&b);
    lanes.add(&c);
    lanes.add(&a);
    lanes.clear();
    // The next frame has fewer lanes: the kept lists from the last frame record nothing.
    CHECK(lanes.add(&b) == 0);
    lanes.add(&b);
    CHECK(lanes.lanes() == 1);
    CHECK(lanes.jobs() == 2);
    std::vector<std::size_t> recorded;
    lanes.record([&](std::size_t j) { recorded.push_back(j); });
    CHECK(recorded == std::vector<std::size_t>{0, 1});
    lanes.clear();
    lanes.record([&](std::size_t j) { recorded.push_back(j); });
    CHECK(recorded.size() == 2);
    // A new lane after that reuses an emptied list.
    CHECK(lanes.add(&c) == 0);
    CHECK(lanes.add(&a) == 1);
    recorded.clear();
    lanes.record([&](std::size_t j) { recorded.push_back(j); });
    std::sort(recorded.begin(), recorded.end());
    CHECK(recorded == std::vector<std::size_t>{0, 1});
}
