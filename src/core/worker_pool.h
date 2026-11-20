#pragma once

// A small fixed pool of worker threads for the frame's own work (docs/graphics.md "Worker threads"): a parallel_for
// that splits a loop into chunks the calling thread works on too, and background tasks the frame starts and later
// waits for (the next frame's particle simulation, recorded while this frame records).
//
// The frame owns one pool (app::ClientApp) and registers it with set_workers; the renderers call the free
// parallel_for, which runs the loop on the calling thread when no pool is registered (the tests, the tools, a pool
// of size 0). A parallel_for called from inside a worker also runs inline; parallel_for_nested spreads such a loop
// over the workers too.
//
// The work must touch only what its caller hands it: disjoint ranges of the caller's arrays, read-only shared state.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace hailstone::core {

class WorkerPool {
public:
    // `workers` threads besides the caller's (0: everything runs on the caller).
    explicit WorkerPool(unsigned workers);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

    // Calls fn(begin, end) over [0, count) in chunks of at least `grain`, on the workers and the calling thread, and
    // returns when every chunk is done. The first exception a chunk throws is rethrown here, after the others end.
    template <class Fn> void parallel_for(std::size_t count, std::size_t grain, Fn&& fn) {
        using F = std::remove_reference_t<Fn>;
        run_batch(count, grain, [](void* f, std::size_t b, std::size_t e) { (*static_cast<F*>(f))(b, e); },
                  const_cast<void*>(static_cast<const void*>(&fn)));
    }

    // A background job's handle: wait() returns once it has run (running it on the caller if no worker has started
    // it yet) and rethrows what it threw. A handle not waited for waits in its destructor. The pool must outlive
    // every handle it gave out.
    class Task {
    public:
        Task() = default;
        Task(Task&&) noexcept = default;
        Task& operator=(Task&& other) noexcept;
        ~Task();
        bool valid() const { return state_ != nullptr; }
        void wait();

    private:
        friend class WorkerPool;
        struct State;
        Task(WorkerPool* pool, std::shared_ptr<State> state) : pool_(pool), state_(std::move(state)) {}
        WorkerPool* pool_ = nullptr;
        std::shared_ptr<State> state_;
    };
    Task run(std::function<void()> work);

    // Whether the calling thread is one of a pool's workers.
    static bool on_worker();

private:
    struct Batch;
    struct Job {
        Batch* batch = nullptr;                  // a parallel_for's helper
        std::shared_ptr<Task::State> task;       // or a background task
    };
    void run_batch(std::size_t count, std::size_t grain, void (*call)(void*, std::size_t, std::size_t), void* fn);
    static void work_on(Batch& batch);
    static void run_task(Task::State& state);
    bool steal(const Task::State* state); // takes the task off the queue if no worker has started it
    void loop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    std::vector<std::thread> threads_; // last: start after the members above are made
};

// The number of workers for this machine: one fewer than its hardware threads (the frame's own thread), leaving room
// for the loader, dresser, audio and network threads, at most 6.
unsigned default_worker_count();

// The frame's pool, or nullptr: set by its owner for the pool's lifetime (main thread, before the frame loop).
WorkerPool* workers();
void set_workers(WorkerPool* pool);

// WorkerPool::parallel_for on the registered pool, or inline on the calling thread when there is none or the call is
// made from inside a worker.
template <class Fn> void parallel_for(std::size_t count, std::size_t grain, Fn&& fn) {
    if (count == 0) return;
    WorkerPool* pool = workers();
    if (!pool || pool->workers() == 0 || WorkerPool::on_worker() || count <= grain) {
        fn(std::size_t{0}, count);
        return;
    }
    pool->parallel_for(count, grain, std::forward<Fn>(fn));
}

// parallel_for that is split over the workers even when called from one: from a background task (the actor effects'
// step) or from a chunk of another loop (a renderer's draw recorded in a SceneRecorder lane), where parallel_for runs
// inline. It cannot deadlock: the calling thread takes chunks itself and then waits only for helpers a worker has
// already started, and a thread that is waiting never starts another job, so no wait can come back round to it. For
// heavy loops that are often reached from a worker (the particle systems' step and quads); the others keep
// parallel_for, so nesting there stays inline as before.
template <class Fn> void parallel_for_nested(std::size_t count, std::size_t grain, Fn&& fn) {
    if (count == 0) return;
    WorkerPool* pool = workers();
    if (!pool || pool->workers() == 0 || count <= grain) {
        fn(std::size_t{0}, count);
        return;
    }
    pool->parallel_for(count, grain, std::forward<Fn>(fn));
}

} // namespace hailstone::core
