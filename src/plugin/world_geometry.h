#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/world_geometry.h"
#include "hailstone_world_geometry.h"

namespace hailstone::plugin {

struct WorldPlacementScene {
    std::uint32_t epoch;
    std::uint32_t zone_id;
    std::uint32_t instance_id;
    bool operator==(const WorldPlacementScene&) const = default;
};

using WorldPlacementPoint = std::array<float, 3>;

// The host owns the input gesture. Each owner can read its own result once.
class WorldPlacementHost {
public:
    bool start(std::uint64_t owner, WorldPlacementScene scene);
    bool get(std::uint64_t owner, std::optional<WorldPlacementScene> scene, HailstoneWorldPlacement* out);
    bool aiming() const { return aim_.has_value(); }
    void update(std::optional<WorldPlacementPoint> ground, bool pressed, bool released);
    void cancel();
    void cancel(std::uint64_t owner);
    // The player's own cancel (Escape, a right click): until player_input, the owner cannot start another aim, so a
    // plugin that arms again at once cannot keep the player's world clicks.
    void player_cancel();
    // A press in one of the windows: owners held off by player_cancel may aim again.
    void player_input() { held_off_.clear(); }
    void retire(std::uint64_t owner);

private:
    struct Result {
        WorldPlacementScene scene;
        std::optional<WorldPlacementPoint> ground;
    };
    struct Aim {
        std::uint64_t owner;
        Result result;
        bool pressed = false;
    };
    std::optional<Aim> aim_;
    std::unordered_map<std::uint64_t, Result> results_;
    std::unordered_set<std::uint64_t> held_off_;
};

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
