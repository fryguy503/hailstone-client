#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "game/world_geometry.h"
#include "hailstone_world_geometry.h"

namespace hailstone::plugin {

struct WorldQueries {
    std::function<int(float, float, float, float&)> floor;
    std::function<int(std::uint32_t, HailstoneWorldAnchor&)> anchor;
    std::function<int(const HailstoneWorldSurfacePatch&, HailstoneWorldSurfacePatchResult&,
                      std::uint32_t& remaining_work)> surface_patch;
};

struct OwnedWorldBatch {
    game::WorldGeometryId id;
    game::WorldGeometryPrimitive primitive = game::WorldGeometryPrimitive::Triangles;
    std::vector<game::WorldGeometryVertex> vertices;
    bool recorded = false;
    game::WorldGeometryBatch view() const { return {id, primitive, vertices}; }
};

struct OwnedWorldReceipt {
    std::uint64_t owner = 0;
    HailstoneWorldReceipt receipt{};
};

// Main-thread admission and provenance only. This class knows no GPU API or
// server protocol. The application supplies scene queries and actual outcomes.
class WorldGeometryHost {
public:
    static constexpr std::uint32_t kMaximumQueries = 262144;
    void begin(HailstoneWorldFrame frame, WorldQueries queries);
    void seal() { accepting_ = false; }
    void cancel();
    void retire(std::uint64_t owner);
    std::uint32_t capabilities() const { return capabilities_; }
    bool active() const { return active_; }
    HailstoneWorldFrame frame() const { return frame_; }
    const std::vector<OwnedWorldBatch>& batches() const { return batches_; }

    int floor(std::uint64_t frame, std::uint32_t epoch, float x, float y, float z, float* out);
    int anchor(std::uint64_t frame, std::uint32_t epoch, std::uint32_t id, HailstoneWorldAnchor* out);
    int surface_patch(const HailstoneWorldSurfacePatch* query, HailstoneWorldSurfacePatchResult* out);
    bool submit(std::uint64_t owner, const HailstoneWorldSubmission* submission);
    void recorded(std::size_t batch, std::uint32_t vertices);
    std::vector<OwnedWorldReceipt> finish(bool submitted, bool presented);

private:
    bool current(std::uint64_t frame, std::uint32_t epoch) const;
    HailstoneWorldFrame frame_{};
    WorldQueries queries_;
    game::WorldGeometryFrameBudget budget_;
    std::vector<OwnedWorldBatch> batches_;
    std::uint32_t capabilities_ = 0;
    std::uint32_t attempts_ = 0, inspected_ = 0;
    bool active_ = false, accepting_ = false;
};

} // namespace hailstone::plugin
