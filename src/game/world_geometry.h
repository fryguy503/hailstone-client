#pragma once

// Transient, already-positioned world geometry. The producer owns terrain
// sampling and draws a complete batch; there is no inferred floor or height.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hailstone::game {

struct WorldGeometryVertex {
    // Zone-file world axes, exactly as HailstoneSpawn and the scene camera:
    // right-handed, Z up. Server packet X/Y are swapped at the packet boundary
    // (game/coords.h); a producer handling raw server positions converts once.
    std::array<float, 3> position{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f}; // display sRGB, straight alpha; every channel 0..1
};

enum class WorldGeometryPrimitive : std::uint8_t { Triangles, Lines };

struct WorldGeometryId {
    std::uint64_t owner = 0; // opaque host-issued owner; separate producers cannot claim each other's tags
    std::uint64_t tag = 0;
    std::uint64_t revision = 0;
    bool operator==(const WorldGeometryId&) const = default;
};

struct WorldGeometryBatch {
    WorldGeometryId id;
    WorldGeometryPrimitive primitive = WorldGeometryPrimitive::Triangles;
    std::span<const WorldGeometryVertex> vertices; // borrowed only for validation/recording, not retained
};

inline constexpr std::uint32_t kWorldGeometryMaxBatches = 256;
inline constexpr std::uint32_t kWorldGeometryMaxVertices = 98304;
// This extension's numeric safety limit, not a change to movement or zone
// rules: 32 times the packed spawn coordinate range (signed 19-bit eighths).
// It rejects finite FLT_MAX inputs before the GPU's float matrix products.
inline constexpr float kWorldGeometryMaxCoordinate = 1048576.0f;

enum class WorldGeometryValidation : std::uint8_t {
    Valid,
    Empty,
    InvalidPrimitive,
    InvalidVertexCount,
    InvalidVertex,
    DuplicateTag,
    BatchLimit,
    VertexLimit,
};

inline WorldGeometryValidation validate_world_geometry(const WorldGeometryBatch& batch) {
    std::size_t stride = 0;
    switch (batch.primitive) {
    case WorldGeometryPrimitive::Triangles: stride = 3; break;
    case WorldGeometryPrimitive::Lines: stride = 2; break;
    default: return WorldGeometryValidation::InvalidPrimitive;
    }
    if (batch.vertices.empty()) return WorldGeometryValidation::Empty;
    if (batch.vertices.size() > kWorldGeometryMaxVertices) return WorldGeometryValidation::VertexLimit;
    if (batch.vertices.size() % stride != 0) return WorldGeometryValidation::InvalidVertexCount;
    for (const auto& vertex : batch.vertices) {
        for (float value : vertex.position)
            if (!std::isfinite(value) || std::abs(value) > kWorldGeometryMaxCoordinate)
                return WorldGeometryValidation::InvalidVertex;
        for (float value : vertex.color)
            if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                return WorldGeometryValidation::InvalidVertex;
    }
    return WorldGeometryValidation::Valid;
}

// One frame's CPU validation and GPU storage budget. Rejected input consumes
// an attempt, and inspected vertices consume work budget even when invalid.
// A duplicate owner/tag (including another revision) cannot record twice in
// the same frame. A failed upload also keeps its reservation until next frame.
class WorldGeometryFrameBudget {
public:
    WorldGeometryValidation admit(const WorldGeometryBatch& batch) {
        if (attempts_ == kWorldGeometryMaxBatches) return WorldGeometryValidation::BatchLimit;
        ++attempts_;
        for (std::size_t index = 0; index < ids_count_; ++index)
            if (ids_[index].owner == batch.id.owner && ids_[index].tag == batch.id.tag)
                return WorldGeometryValidation::DuplicateTag;
        if (batch.vertices.size() > kWorldGeometryMaxVertices - inspected_)
            return WorldGeometryValidation::VertexLimit;
        inspected_ += static_cast<std::uint32_t>(batch.vertices.size());
        const auto validation = validate_world_geometry(batch);
        if (validation != WorldGeometryValidation::Valid) return validation;
        ids_[ids_count_++] = batch.id;
        reserved_ += static_cast<std::uint32_t>(batch.vertices.size());
        return validation;
    }

    std::uint32_t reserved_vertices() const { return reserved_; }

private:
    std::array<WorldGeometryId, kWorldGeometryMaxBatches> ids_{};
    std::size_t ids_count_ = 0;
    std::uint32_t attempts_ = 0;
    std::uint32_t inspected_ = 0;
    std::uint32_t reserved_ = 0;
};

} // namespace hailstone::game
