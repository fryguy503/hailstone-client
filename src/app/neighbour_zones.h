#pragma once

// The neighbour zones past the outdoor zone lines (docs/graphics.md, "Neighbour zones"; the arithmetic is
// game/neighbour_zones.h): which lines qualify, loading each neighbour's terrain and placed objects from the asset
// folder on a background thread at low priority once the zone is entered, uploading it when ready, drawing it in
// the modern lighting, and freeing it on zoning or when the camera is far from its line. Past its line the land round
// the current zone (render/horizon.h) is its ground: its ground is measured (render::HorizonSource, cached on disk),
// it draws its part of the land with its own ground material faded into the current zone's, and of its own geometry
// only what stands on that ground. Every qualifying neighbour is measured, drawn or not, so the next visit's land
// takes the shape of all of them.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
        // The land round the zone (render::ZoneRenderer::horizon_build; null without), and where its neighbours'
        // ground is cached (horizon_cache_dir).
        std::shared_ptr<const render::HorizonBuild> horizon;
        std::filesystem::path cache_dir;
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

    // The squares (render::ZoneRenderer::footprint) of the neighbours drawn now, for the current zone's
    // set_distant_hidden; and a number that changes when which are drawn does.
    void footprint(std::vector<std::int64_t>& out) const;
    std::uint64_t drawn_key() const;

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
        // The neighbour's ground (render/horizon.h), from the cache or measured this visit, and the offset it was
        // moved by; the key its cache file must have (cache_key).
        std::shared_ptr<const render::HorizonSource> source;
        glm::vec3 source_offset{0.0f};
        std::uint64_t cache_key = 0;
        bool surveyed = false; // measured, or being measured, without being drawn
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
    // A job for line `l`: what every load takes (the line, the land, the neighbours' ground, the cache).
    std::shared_ptr<Job> make_job(const Line& l, bool survey) const;
    void finish(Job& job);
    static void run(std::shared_ptr<Job> job, const GameDir* game);

    render::VkContext& vk_;
    const GameDir& game_;
    std::filesystem::path shader_dir_;
    std::vector<Line> lines_;
    bool entered_ = false;
    const game::CollisionMesh* collision_ = nullptr; // the current zone's, until clear()
    glm::vec3 zone_lo_{0.0f}, zone_hi_{0.0f};
    std::string zone_name_;
    std::shared_ptr<const render::HorizonBuild> horizon_; // the current zone's land
    render::HorizonSources sources_; // the land's, then those measured this visit (they shape the parts drawn)
    std::filesystem::path cache_dir_;
    std::map<std::string, std::vector<std::byte>> land_files_; // the land's textures' files, for the neighbours
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

// Where the neighbours' ground of `zone` is cached for `server` (cache/horizon/<server>/<zone>/, docs/layout.md).
std::filesystem::path horizon_cache_dir(std::string_view server, std::string_view zone);
// What a cache file of `zone`'s neighbour `neighbour` must say it was measured from: the two zones' files' sizes (0
// when either is missing, which no file matches).
std::uint64_t horizon_cache_key(const GameDir& game, std::string_view zone, std::string_view neighbour);
// The neighbours' ground cached for `zone` in `dir` that still matches the asset folder (render::HorizonSources, for
// ZoneRenderOptions::horizon_sources), with the offsets they were moved by.
std::vector<render::HorizonCacheEntry> horizon_cache_entries(const GameDir& game, const std::filesystem::path& dir,
                                                            std::string_view zone);
render::HorizonSources cached_horizon_sources(const GameDir& game, const std::filesystem::path& dir,
                                              std::string_view zone);

} // namespace hailstone::app
