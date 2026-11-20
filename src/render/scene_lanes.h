#pragma once

// SceneRecorder's lanes, without Vulkan (render/scene_recorder.h): which lane and which target each queued job is in,
// the recording of the lanes at once (core::parallel_for), a lane's jobs in the order they came on one thread, and
// each target's jobs in the order they came, the order SceneRecorder executes their buffers in. A lane may span
// targets (a renderer's draws into several shadow layers): its jobs are still recorded in order on one thread.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "core/worker_pool.h"

namespace hailstone::render {

class SceneLanes {
public:
    // The lanes' own lists are kept, emptied, for the next frame's (their capacity: no allocation once they have grown).
    void clear() {
        lane_of_.clear();
        target_of_.clear();
        lanes_.clear();
        for (auto& jobs : by_lane_) jobs.clear();
    }
    // Queues the next job (its index: the jobs so far) in `lane`, drawing into `target`; returns the lane's index, by
    // first use this frame.
    std::size_t add(const void* lane, std::size_t target = 0) {
        auto it = std::find(lanes_.begin(), lanes_.end(), lane);
        const auto index = static_cast<std::size_t>(it - lanes_.begin());
        if (it == lanes_.end()) {
            lanes_.push_back(lane);
            if (by_lane_.size() < lanes_.size()) by_lane_.emplace_back();
        }
        by_lane_[index].push_back(lane_of_.size());
        lane_of_.push_back(index);
        target_of_.push_back(target);
        return index;
    }
    std::size_t jobs() const { return lane_of_.size(); }
    std::size_t lanes() const { return lanes_.size(); }
    std::size_t lane_of(std::size_t job) const { return lane_of_[job]; }
    std::size_t target_of(std::size_t job) const { return target_of_[job]; }

    // Calls fn(job) for every job: the lanes on the workers, each lane's jobs in order on one thread.
    template <class Fn> void record(Fn&& fn) const {
        core::parallel_for(lanes_.size(), 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t lane = begin; lane < end; ++lane)
                for (std::size_t job : by_lane_[lane]) fn(job);
        });
    }
    // Calls fn(job) for each job drawing into `target`, in the order they came, on this thread.
    template <class Fn> void each_in(std::size_t target, Fn&& fn) const {
        for (std::size_t job = 0; job < target_of_.size(); ++job)
            if (target_of_[job] == target) fn(job);
    }

private:
    std::vector<std::size_t> lane_of_;              // each job's lane, in the jobs' order
    std::vector<std::size_t> target_of_;            // each job's target, in the jobs' order
    std::vector<const void*> lanes_;                // this frame's lanes, by index
    std::vector<std::vector<std::size_t>> by_lane_; // each lane's jobs, in order (the first lanes_.size() in use)
};

} // namespace hailstone::render
