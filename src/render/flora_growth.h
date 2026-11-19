#pragma once

// Which of a zone's flora tiles are grown, growing or to grow (render::FloraRenderer's bookkeeping, without Vulkan):
// the tiles grow on one background thread (core::LatestJobs), a few queued at a time, nearest first, so the queue
// follows the camera. A grown tile is kept while in keep and drawn until it is grown again when stale (regrow); a
// tile whose job threw is logged once and not queued again until something changes there (regrow, restart, or it
// leaves keep).
//
// `Grown` is the tile's result; it has a `bool stale`. Everything here is on the owner's thread; the jobs touch only
// what they captured.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/latest_jobs.h"
#include "core/log.h"

namespace hailstone::render {

template <class Grown>
class FloraGrowth {
public:
    using Job = std::function<Grown()>;
    using TileTest = std::function<bool(std::uint32_t tile)>;

    explicit FloraGrowth(std::size_t in_flight = 3) : in_flight_(in_flight) {}

    // A frame's growth: lets go of the grown tiles out of `keep`, takes the tiles grown since the last call (kept while
    // in keep), drops the jobs of tiles that left it, and queues the missing and stale tiles of `near` (by distance,
    // nearest first) until `in_flight` are queued or growing. `make_job` makes a tile's job (on this thread: whatever
    // it reads of the owner's state it must copy).
    void update(std::span<const std::pair<float, std::uint32_t>> near, const TileTest& keep,
                const std::function<Job(std::uint32_t tile)>& make_job) {
        for (auto it = grown_.begin(); it != grown_.end();) {
            if (!keep(it->first)) it = grown_.erase(it);
            else ++it;
        }
        std::erase_if(failed_, [&](std::uint32_t t) { return !keep(t); }); // back in reach later: tried again
        for (auto& [t, g] : jobs_.take())
            if (growing_.erase(t) > 0 && keep(t)) grown_.insert_or_assign(t, std::move(g));
        for (auto it = growing_.begin(); it != growing_.end();) {
            if (!keep(*it)) {
                jobs_.cancel(*it);
                it = growing_.erase(it);
            } else if (!jobs_.pending(*it)) {
                // Not handed back nor pending: its work threw. Not queued again until something changes there.
                log::warn("flora: tile {} failed to grow; it is left as it was until the cover there changes", *it);
                failed_.insert(*it);
                it = growing_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [d2, t] : near) {
            if (growing_.size() >= in_flight_) break;
            if (growing_.contains(t) || failed_.contains(t)) continue;
            if (auto it = grown_.find(t); it != grown_.end() && !it->second.stale) continue;
            jobs_.submit(t, make_job(t));
            growing_.insert(t);
        }
    }

    // Cover came or went where `touches` says: those grown tiles grow again (drawn until then), those growing are
    // dropped and queued again, and those that failed are tried again.
    void regrow(const TileTest& touches) {
        for (auto& [t, g] : grown_)
            if (touches(t)) g.stale = true;
        for (auto it = growing_.begin(); it != growing_.end();) {
            if (touches(*it)) {
                jobs_.cancel(*it);
                it = growing_.erase(it);
            } else {
                ++it;
            }
        }
        std::erase_if(failed_, [&](std::uint32_t t) { return touches(t); });
    }

    // Lets go of everything: the tiles grow again from the start (the flora was set up again).
    void restart() {
        grown_.clear();
        for (std::uint32_t t : growing_) jobs_.cancel(t);
        growing_.clear();
        failed_.clear();
    }

    const std::unordered_map<std::uint32_t, Grown>& grown() const { return grown_; }
    bool growing(std::uint32_t tile) const { return growing_.contains(tile); }
    std::size_t growing_count() const { return growing_.size(); }
    bool failed(std::uint32_t tile) const { return failed_.contains(tile); }
    // Waits until no job is queued or running (tests).
    void wait_idle() { jobs_.wait_idle(); }

private:
    std::size_t in_flight_;
    std::unordered_map<std::uint32_t, Grown> grown_; // by tile
    std::unordered_set<std::uint32_t> growing_;      // the tiles with a job on jobs_
    std::unordered_set<std::uint32_t> failed_;       // the tiles whose job threw, until something changes there
    // Last: stopped and joined first as this goes, so whatever a job uses may go after it.
    core::LatestJobs<std::uint32_t, Grown> jobs_;
};

} // namespace hailstone::render
