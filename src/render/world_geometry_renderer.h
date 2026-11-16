#pragma once

// Unlit colored triangles and one-pixel line segments, with reverse-Z depth
// testing and alpha blending into the scene. The caller supplies final world
// positions and compositing order. No texture, floor, depth-copy or lighting
// dependency; works in both scene looks before begin_overlay.

#include <array>
#include <cstdint>
#include <filesystem>

#include <glm/glm.hpp>

#include "game/world_geometry.h"
#include "render/gpu.h"
#include "render/vk_context.h"

namespace hailstone::render {

// Every clip-coordinate dot product stays finite for admitted world positions,
// even with an invalid but finite camera matrix. Independent of GPU state.
bool world_geometry_view_valid(const glm::mat4& matrix);

enum class WorldGeometryRecordStatus : std::uint8_t {
    Recorded,
    Rejected,
    InvalidFrame,
    InvalidView,
    UploadFailed,
};

struct WorldGeometryBatchResult {
    game::WorldGeometryId id;
    std::uint64_t frame = 0; // VkContext::frames_begun() for this recording
    WorldGeometryRecordStatus status = WorldGeometryRecordStatus::Rejected;
    game::WorldGeometryValidation validation = game::WorldGeometryValidation::Valid;
    std::uint32_t recorded_vertices = 0;
};

class WorldGeometryRenderer {
public:
    WorldGeometryRenderer(VkContext& vk, const std::filesystem::path& shader_dir);
    ~WorldGeometryRenderer();
    WorldGeometryRenderer(const WorldGeometryRenderer&) = delete;
    WorldGeometryRenderer& operator=(const WorldGeometryRenderer&) = delete;

    // Main thread, between begin_scene and begin_overlay, using the current
    // FrameInfo. May be called repeatedly: each complete batch appends to this
    // fenced frame slot. Vertex spans are copied before this returns. Budgets
    // reset only on a newly begun frame; nothing persists into the next frame.
    // A marker's fill, thick outline and glyph can be one triangle batch so
    // admission and recording cover all of its required parts atomically.
    //
    // Recorded means vkCmdDraw was recorded for exactly this owner/tag/revision,
    // with no omitted vertices. It does not mean submitted, GPU-complete,
    // visible (geometry may be clipped/occluded/transparent), or presented.
    WorldGeometryBatchResult record(const FrameInfo& frame, const glm::mat4& view_proj,
                                    const game::WorldGeometryBatch& batch);

private:
    void create_pipelines(const std::filesystem::path& shader_dir);
    void release_resources();

    struct FrameState {
        std::uint64_t generation = 0;
        game::WorldGeometryFrameBudget budget;
    };
    VkContext& vk_;
    std::array<Buffer, VkContext::kFramesInFlight> vertices_{};
    std::array<void*, VkContext::kFramesInFlight> mapped_{};
    std::array<FrameState, VkContext::kFramesInFlight> frames_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 2> pipelines_{}; // triangles, lines
};

} // namespace hailstone::render
