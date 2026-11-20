// core::WorkerPool: the frame's parallel_for and background tasks.
#include <doctest/doctest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "core/worker_pool.h"

using hailstone::core::WorkerPool;

TEST_CASE("WorkerPool parallel_for covers every index once") {
    for (unsigned workers : {0u, 1u, 3u}) {
        WorkerPool pool(workers);
        for (std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{1000}}) {
            for (std::size_t grain : {std::size_t{1}, std::size_t{3}, std::size_t{64}, std::size_t{5000}}) {
                std::vector<std::atomic<int>> hits(count);
                pool.parallel_for(count, grain, [&](std::size_t begin, std::size_t end) {
                    CHECK(begin < end);
                    CHECK(end - begin <= grain);
                    for (std::size_t i = begin; i < end; ++i) hits[i].fetch_add(1);
                });
                for (std::size_t i = 0; i < count; ++i) REQUIRE(hits[i].load() == 1);
            }
        }
    }
}

TEST_CASE("WorkerPool parallel_for rethrows a chunk's exception after the rest end") {
    WorkerPool pool(3);
    std::atomic<int> ran{0};
    CHECK_THROWS_AS(pool.parallel_for(100, 1,
                                      [&](std::size_t begin, std::size_t) {
                                          ran.fetch_add(1);
                                          if (begin == 50) throw std::runtime_error("chunk");
                                      }),
                    std::runtime_error);
    // The pool still works afterwards.
    std::atomic<int> sum{0};
    pool.parallel_for(10, 1, [&](std::size_t begin, std::size_t) { sum.fetch_add(static_cast<int>(begin)); });
    CHECK(sum.load() == 45);
}

TEST_CASE("WorkerPool parallel_for finishes while every worker is held by a task") {
    // A background task holding the only worker: the caller does every chunk itself, and does not wait for it.
    WorkerPool pool(1);
    std::mutex m;
    std::condition_variable cv;
    bool entered = false, open = false;
    auto task = pool.run([&] {
        std::unique_lock lock(m);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return open; });
    });
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return entered; });
    }
    std::atomic<int> count{0};
    pool.parallel_for(64, 1, [&](std::size_t begin, std::size_t end) { count.fetch_add(static_cast<int>(end - begin)); });
    CHECK(count.load() == 64);
    {
        std::lock_guard lock(m);
        open = true;
    }
    cv.notify_all();
    task.wait();
}

TEST_CASE("WorkerPool tasks run once, and wait rethrows") {
    for (unsigned workers : {0u, 2u}) {
        WorkerPool pool(workers);
        std::atomic<int> runs{0};
        auto task = pool.run([&] { runs.fetch_add(1); });
        task.wait();
        CHECK(runs.load() == 1);
        CHECK_FALSE(task.valid());
        task.wait(); // a second wait does nothing
        CHECK(runs.load() == 1);

        auto failing = pool.run([] { throw std::runtime_error("task"); });
        CHECK_THROWS_AS(failing.wait(), std::runtime_error);
    }
}

TEST_CASE("WorkerPool runs a task nobody waited for before it goes") {
    std::atomic<int> runs{0};
    {
        WorkerPool pool(1);
        WorkerPool::Task a = pool.run([&] { runs.fetch_add(1); });
        WorkerPool::Task b = pool.run([&] { runs.fetch_add(1); });
        // b's handle waits as it is replaced; a's at the end of the scope.
        b = pool.run([&] { runs.fetch_add(1); });
    }
    CHECK(runs.load() == 3);
}

TEST_CASE("parallel_for runs inline without a registered pool and inside a worker") {
    hailstone::core::set_workers(nullptr);
    std::vector<int> v(100, 0);
    hailstone::core::parallel_for(v.size(), 8, [&](std::size_t begin, std::size_t end) {
        CHECK(begin == 0);
        CHECK(end == v.size());
        for (std::size_t i = begin; i < end; ++i) v[i] = static_cast<int>(i);
    });
    CHECK(std::accumulate(v.begin(), v.end(), 0) == 4950);

    WorkerPool pool(2);
    hailstone::core::set_workers(&pool);
    std::atomic<int> nested{0};
    hailstone::core::parallel_for(8, 1, [&](std::size_t, std::size_t) {
        // From a worker (or the caller) a nested loop must still finish: it runs inline on a worker.
        hailstone::core::parallel_for(16, 1, [&](std::size_t begin, std::size_t end) {
            nested.fetch_add(static_cast<int>(end - begin));
        });
    });
    CHECK(nested.load() == 8 * 16);
    hailstone::core::set_workers(nullptr);
}

TEST_CASE("parallel_for_nested covers every index from a task and from inside another loop") {
    hailstone::core::set_workers(nullptr);
    std::vector<int> v(50, 0);
    hailstone::core::parallel_for_nested(v.size(), 4, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) v[i] = 1;
    });
    CHECK(std::accumulate(v.begin(), v.end(), 0) == 50); // inline without a pool

    for (unsigned workers : {0u, 1u, 3u}) {
        WorkerPool pool(workers);
        hailstone::core::set_workers(&pool);
        // Inside a chunk of another loop (a renderer's draw in a recording lane): every chunk of each inner loop once.
        std::vector<std::atomic<int>> hits(8 * 64);
        hailstone::core::parallel_for(8, 1, [&](std::size_t outer_begin, std::size_t outer_end) {
            for (std::size_t o = outer_begin; o < outer_end; ++o)
                hailstone::core::parallel_for_nested(64, 2, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) hits[o * 64 + i].fetch_add(1);
                });
        });
        for (const auto& h : hits) CHECK(h.load() == 1);
        // From a background task (the actor effects' step), and from a loop inside that.
        std::atomic<int> sum{0};
        auto task = pool.run([&] {
            hailstone::core::parallel_for_nested(4, 1, [&](std::size_t outer_begin, std::size_t outer_end) {
                for (std::size_t o = outer_begin; o < outer_end; ++o)
                    hailstone::core::parallel_for_nested(100, 7, [&](std::size_t begin, std::size_t end) {
                        sum.fetch_add(static_cast<int>(end - begin));
                    });
            });
        });
        task.wait();
        CHECK(sum.load() == 400);
        hailstone::core::set_workers(nullptr);
    }
}
