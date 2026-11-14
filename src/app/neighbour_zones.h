#pragma once

// The neighbour zones past the outdoor zone lines (docs/graphics.md, "Neighbour zones"; the arithmetic is
// game/neighbour_zones.h): which lines qualify, loading each neighbour's terrain and placed objects from the asset
// folder on a background thread at low priority once the zone is entered, uploading it when ready, drawing it in
// the modern lighting, and freeing it on zoning or when the camera is far from its line.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

#include "game/neighbour_zones.h"
#include "render/zone_renderer.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { struct Zone; }
namespace hailstone::game {
class CollisionMesh;
struct SceneLight;
struct ViewDistance;
} // namespace hailstone::game

namespace hailstone::app {

class NeighbourZones {
public:
    NeighbourZones(render::VkContext& vk, const GameDir& game, std::filesystem::path shader_dir);
    ~NeighbourZones();
    NeighbourZones(const NeighbourZones&) = delete;
    NeighbourZones& operator=(const NeighbourZones&) = delete;

    struct ZoneIn {
        const assets::Zone* zone = nullptr;
        const game::CollisionMesh* collision = nullptr;
        std::vector<game::ZonePointTarget> points;
        std::uint32_t zone_id = 0;
        bool outdoor = false; // OP_NewZone: a sky and an outdoor time type
    };
    // After zone-in, with the zone points: finds the lines and which of them qualify here (quick, on this thread).
    void enter(const ZoneIn& in);
    // Zoning: drops every neighbour and abandons a load under way.
    void clear();
    // Each frame: starts the nearest wanted neighbour's load, uploads one that is ready, frees those far away.
    void update(const glm::vec3& camera, float far_plane, bool enabled);

    void set_scene_light(const game::SceneLight& light);
    void set_lighting(const render::SceneLighting& lighting);
    // render::ZoneRenderer::set_lod_scale, for each neighbour's renderer.
    void set_lod_scale(float scale);
    void draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
              const game::ViewDistance& view, double time_seconds, render::ZonePhase phase);

    bool entered() const { return entered_; }
    // A load is under way (update starts and finishes them), its ground being sampled included.
    bool busy() const { return job_ != nullptr || pending_.has_value(); }
    std::size_t loaded() const;
    // The CPU time the last draw calls took, in ms (Opaque plus Translucent).
    double draw_ms() const { return draw_ms_; }
    // What the last frame's draws drew (triangles, draw calls).
    std::size_t drawn_triangles() const { return drawn_triangles_; }
    std::size_t drawn_calls() const { return drawn_calls_; }

private:
    struct Job;
    struct Line {
        std::string name; // the neighbour's short name
        game::NeighbourLines lines;
        game::Seam seam;
        game::NeighbourOffset first;
        std::vector<glm::vec3> points;
        std::vector<std::vector<float>> floors_here; // the current zone's floors at points - 8 outward
        game::NeighbourVerdict verdict = game::NeighbourVerdict::Draw;
        bool failed = false;
        std::unique_ptr<render::ZoneRenderer> renderer;
        std::size_t bytes = 0;
        // Loaded, but the camera is past its free reach: not drawn, and freed only when another needs its room, on
        // zoning or with the setting off (freeing waits for the GPU: render::ZoneRenderer's destructor).
        bool idle = false;
    };
    // A load about to start: the current zone's ground past the seam is sampled first, on this thread (the loader
    // must not touch this zone) a few rows a frame, then the loader starts.
    struct Pending {
        std::size_t line = 0;
        std::shared_ptr<Job> job;
        game::GroundSampler ground;
        float top = 0.0f;
        float distance = 0.0f;
        double ms = 0.0;    // the frames' time on it so far
        int frames = 0;
    };
    void start(Pending& p);
    void finish(Job& job);
    static void run(std::shared_ptr<Job> job, const GameDir* game);

    render::VkContext& vk_;
    const GameDir& game_;
    std::filesystem::path shader_dir_;
    std::vector<Line> lines_;
    bool entered_ = false;
    const game::CollisionMesh* collision_ = nullptr; // the current zone's, until clear()
    glm::vec3 zone_lo_{0.0f}, zone_hi_{0.0f};
    std::shared_ptr<Job> job_;
    std::thread thread_;
    std::optional<Pending> pending_;
    struct Abandoned {
        std::shared_ptr<Job> job;
        std::thread thread;
    };
    std::vector<Abandoned> abandoned_; // loads cut off by zoning, joined once they end
    double draw_ms_ = 0.0, draw_acc_ = 0.0;
    std::size_t drawn_triangles_ = 0, drawn_calls_ = 0, triangles_acc_ = 0, calls_acc_ = 0;
};

} // namespace hailstone::app
