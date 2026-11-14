#include "app/neighbour_zones.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>

#include "assets/zone.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "core/work_budget.h"
#include "game/collision.h"
#include "game/zone_names.h"
#include "platform/process.h"
#include "platform/crash_log.h"

namespace hailstone::app {

namespace {

using Clock = std::chrono::steady_clock;

// Which neighbours are loaded: those whose line is within the view's reach plus kLoadReach of the camera, nearest
// first, at most kMaxLoaded at once; freed past the reach plus kFreeReach.
constexpr float kLoadReach = 1500.0f;
constexpr float kFreeReach = 3500.0f;
constexpr std::size_t kMaxLoaded = 2;
// Loads cut off by zoning that may still be finishing before a new one starts (NeighbourZones::update).
constexpr std::size_t kMaxAbandoned = 1;
// A neighbour's textures at most this many texels across: it is seen from past the line, never close.
constexpr std::uint32_t kMaxTextureSize = 128;
// How far past the seam's face the neighbour starts (game::seam_clip_plane).
constexpr float kSeamMargin = 0.0f;
// The current zone's ground past the seam: sampled every kGroundCell units, from kGroundProbe over the line's top.
constexpr float kGroundCell = 24.0f;
constexpr float kGroundProbe = 100.0f;
// The neighbour's ground within this height of the current zone's goes (game::split_off_ground).
constexpr float kGroundTolerance = 24.0f;
// How far into the neighbour its open sky is tested, past the seam.
constexpr float kArrivalProbe = 40.0f;
// The frame's time for sampling a load's ground past the seam (NeighbourZones::Pending); at least a row a frame.
constexpr double kGroundBudgetMs = 1.0;

// The ground under (p.x, p.y), from `top` down: the line's own height range, not the zone's highest surface. The
// ground as seen (CollisionMesh::ground_below): a tunnel roof's underside is not ground, nor an invisible plane (some
// zones close their edges or bottoms with them).
std::optional<float> floor_at(const game::CollisionMesh& c, const glm::vec3& p, float top) {
    return c.ground_below(glm::vec3(p.x, p.y, top));
}

// The first surface seen above p (CollisionMesh::roof_above): an invisible lid over a line is no roof.
std::optional<float> roof_at(const game::CollisionMesh& c, const glm::vec3& p) { return c.roof_above(p); }

// Every floor under (p.x, p.y) from `top` down to `bottom`, highest first (at most a few).
std::vector<float> floors_at(const game::CollisionMesh& c, const glm::vec3& p, float top, float bottom) {
    std::vector<float> out;
    for (auto h = floor_at(c, p, top); h && *h >= bottom && out.size() < 6; h = floor_at(c, p, *h - 2.0f))
        out.push_back(*h);
    return out;
}

std::string heights(const std::vector<std::vector<float>>& v) {
    std::string out;
    for (const auto& list : v) {
        out += ' ';
        if (list.empty()) out += '-';
        for (std::size_t i = 0; i < list.size(); ++i) out += std::format("{}{:.0f}", i ? "/" : "", list[i]);
    }
    return out;
}

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

} // namespace

struct NeighbourZones::Job {
    // In.
    std::string name;
    game::NeighbourLines lines;
    game::Seam seam;
    game::NeighbourOffset first;
    std::vector<glm::vec3> points;
    std::vector<std::vector<float>> floors_here;
    game::GroundGrid ground; // the current zone's ground past the seam
    std::uint32_t max_images = 0;
    // Out.
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};
    render::ZoneRenderer::PreparedPtr prepared;
    game::NeighbourOffset offset;
    float open_there = -1.0f;
    bool return_line = false;
    std::string error;
    std::size_t bytes = 0;
    std::size_t triangles_source = 0;
    double ms = 0.0;
};

NeighbourZones::NeighbourZones(render::VkContext& vk, const GameDir& game, std::filesystem::path shader_dir)
    : vk_(vk), game_(game), shader_dir_(std::move(shader_dir)) {}

NeighbourZones::~NeighbourZones() {
    clear();
    for (auto& a : abandoned_)
        if (a.thread.joinable()) a.thread.join();
}

void NeighbourZones::clear() {
    pending_.reset();
    if (job_) {
        job_->cancelled = true;
        abandoned_.push_back({std::move(job_), std::move(thread_)});
        job_.reset();
    }
    lines_.clear(); // the renderers wait for the GPU as they go
    entered_ = false;
    collision_ = nullptr;
}

void NeighbourZones::footprint(std::vector<std::int64_t>& out) const {
    for (const Line& l : lines_)
        if (l.renderer && !l.idle) out.insert(out.end(), l.renderer->footprint().begin(), l.renderer->footprint().end());
}

std::uint64_t NeighbourZones::drawn_key() const {
    std::uint64_t key = 0;
    for (const Line& l : lines_)
        if (l.renderer && !l.idle) key = key * 1000003u + l.renderer->id();
    return key;
}

std::size_t NeighbourZones::loaded() const {
    return static_cast<std::size_t>(std::count_if(lines_.begin(), lines_.end(), [](const Line& l) { return l.renderer != nullptr; }));
}

void NeighbourZones::enter(const ZoneIn& in) {
    clear();
    entered_ = true;
    if (!in.zone) return;
    collision_ = in.collision;
    zone_lo_ = in.zone->bounds_min;
    zone_hi_ = in.zone->bounds_max;
    const auto t0 = Clock::now();
    auto groups = game::collect_neighbour_lines(in.zone->regions, in.zone->bounds_min, in.zone->bounds_max, in.points,
                                                in.zone_id);
    for (auto& g : groups) {
        Line l;
        l.name = game::zone_short_name(g.zone_id);
        l.seam = game::seam_of(g, in.zone->bounds_min, in.zone->bounds_max);
        l.first = game::offset_from_targets(g, l.seam);
        l.points = game::seam_points(l.seam);
        game::NeighbourCheck check;
        check.outdoor_here = in.outdoor;
        check.cells = g.cells.size();
        check.files = !l.name.empty() && (game_.exists(l.name + ".s3d") || game_.exists(l.name + ".eqg"));
        check.spread = std::max(l.first.spread.x, l.first.spread.y);
        if (in.collision) {
            const glm::vec3 u = l.seam.outward();
            const float top = l.seam.hi.z + 20.0f;
            for (const auto& p : l.points)
                l.floors_here.push_back(floors_at(*in.collision, p - 8.0f * u, top, l.seam.lo.z - 100.0f));
            std::vector<glm::vec3> inside;
            for (const auto& p : l.points) inside.push_back(p - 8.0f * u);
            check.open_here = game::open_sky_share(
                inside, [&](const glm::vec3& p) { return floor_at(*in.collision, p, top); },
                [&](const glm::vec3& p) { return roof_at(*in.collision, p); });
        }
        l.verdict = game::neighbour_verdict(check);
        log::info("neighbour zone {} ({}): {} line cells, box ({:.0f} {:.0f} {:.0f})-({:.0f} {:.0f} {:.0f}), seam {}{} "
                  "at {:.1f}, open sky {:.0f}%: {}; first offset ({:.1f} {:.1f} {:.1f}), spread ({:.1f} {:.1f} {:.1f}), {}",
                  l.name, g.zone_id, g.cells.size(), g.lo.x, g.lo.y, g.lo.z, g.hi.x, g.hi.y, g.hi.z,
                  l.seam.sign > 0 ? '+' : '-', l.seam.axis == 0 ? 'x' : 'y', l.seam.face, check.open_here * 100.0f,
                  game::to_string(l.verdict), l.first.offset.x, l.first.offset.y, l.first.offset.z, l.first.spread.x,
                  l.first.spread.y, l.first.spread.z, l.first.detail);
        l.lines = std::move(g);
        lines_.push_back(std::move(l));
    }
    log::info("neighbour zones: {} lines to other zones, {} drawable, found in {:.1f} ms", lines_.size(),
              std::count_if(lines_.begin(), lines_.end(),
                            [](const Line& l) { return l.verdict == game::NeighbourVerdict::Draw; }),
              ms_since(t0));
}

void NeighbourZones::run(std::shared_ptr<Job> job, const GameDir* game) {
    platform::CrashStack crash_stack;
    platform::lower_current_thread_priority(); // the frame and the network come first
    const auto t0 = Clock::now();
    try {
        // A cancelled job (clear(): zoning, quitting) stops at the next step, so the join in ~NeighbourZones or the
        // next clear() does not wait out a whole load; load_zone stops between its files.
        auto stop = [&] {
            if (!job->cancelled) return false;
            job->ms = ms_since(t0);
            job->done = true;
            return true;
        };
        assets::Zone zone;
        try {
            zone = assets::load_zone(*game, job->name, [&] { return job->cancelled.load(); });
        } catch (const assets::ZoneLoadCancelled&) {
            stop();
            return;
        }
        if (stop()) return;
        const game::CollisionMesh collision = game::CollisionMesh::build(zone);
        if (stop()) return;
        // Its zone lines, for the return line.
        std::vector<game::LineCell> cells;
        for (const auto& c : zone.regions.leaf_cells(zone.bounds_min - glm::vec3(200.0f), zone.bounds_max + glm::vec3(200.0f),
                                                     [&](std::uint32_t r) { return zone.regions.info(r).is_zone_line(); }))
            cells.push_back({c.lo, c.hi});
        const glm::vec3 arrival = job->seam.centre - job->first.offset;
        auto back = game::find_return_line(cells, job->seam, arrival);
        job->return_line = back.has_value();
        auto floor_here = [&](const glm::vec3& p) -> std::vector<float> {
            const glm::vec3 u = job->seam.outward();
            for (std::size_t i = 0; i < job->points.size(); ++i)
                if (glm::distance(job->points[i] - 8.0f * u, p) < 0.01f) return job->floors_here[i];
            return {};
        };
        // The neighbour's ground from the same height, moved by the first estimate.
        const float top_there = job->seam.hi.z + 20.0f - job->first.offset.z;
        const float bottom_there = job->seam.lo.z - 100.0f - job->first.offset.z;
        std::vector<std::vector<float>> floors_there;
        auto floor_there = [&](const glm::vec3& p) {
            auto h = floors_at(collision, p, top_there, bottom_there);
            floors_there.push_back(h);
            return h;
        };
        job->offset = game::refine_offset(job->first, job->seam, job->lines, back, job->points, floor_here, floor_there);
        job->offset.detail += std::format("; ground here{}, there{}", heights(job->floors_here), heights(floors_there));
        if (back)
            job->offset.detail += std::format("; return line: {} cells, face {:.1f}", back->cells, back->face);
        else
            job->offset.detail += "; no return line found";
        // The open sky just past the seam, in the neighbour.
        std::vector<glm::vec3> there;
        const glm::vec3 shift(job->offset.offset.x, job->offset.offset.y, 0.0f);
        for (const auto& p : job->points) there.push_back(p + kArrivalProbe * job->seam.outward() - shift);
        job->open_there = game::open_sky_share(
            there, [&](const glm::vec3& p) { return floor_at(collision, p, top_there); },
            [&](const glm::vec3& p) { return roof_at(collision, p); });
        // The neighbour's own line back, tested as the current zone's is: inside it, under its roof or not.
        if (back) {
            const game::Seam rs = game::return_seam(*back, job->seam);
            const float top_back = rs.hi.z + 20.0f;
            std::vector<glm::vec3> inside_back;
            for (const auto& p : game::seam_points(rs)) inside_back.push_back(p - 8.0f * rs.outward());
            const float open_back = game::open_sky_share(
                inside_back, [&](const glm::vec3& p) { return floor_at(collision, p, top_back); },
                [&](const glm::vec3& p) { return roof_at(collision, p); });
            job->offset.detail += std::format("; open sky at the line back {:.0f}%", open_back * 100.0f);
            job->open_there = std::min(job->open_there, open_back);
        }
        if (job->open_there < game::kOpenSkyShare || !back || job->cancelled) {
            job->ms = ms_since(t0);
            job->done = true;
            return;
        }
        for (auto& l : zone.lights) l.position += job->offset.offset;
        render::ZoneRenderOptions options;
        options.multipass_lighting = false;
        options.offset = job->offset.offset;
        options.keep = {game::seam_clip_plane(job->seam, kSeamMargin)};
        if (!job->ground.empty())
            options.split = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
                return game::split_off_ground(job->ground, a, b, c, kGroundTolerance);
            };
        options.behind = true;
        options.max_texture_size = kMaxTextureSize;
        options.details = false;
        options.material_maps = false;
        options.cancelled = [&job] { return job->cancelled.load(); };
        job->prepared = render::ZoneRenderer::prepare(zone, options, job->max_images);
        if (job->cancelled || !job->prepared) { // (null only when cancelled)
            job->prepared.reset();
            job->ms = ms_since(t0);
            job->done = true;
            return;
        }
        job->bytes = render::ZoneRenderer::prepared_bytes(*job->prepared);
    } catch (const std::exception& e) {
        job->error = e.what();
    }
    job->ms = ms_since(t0);
    job->done = true;
}

void NeighbourZones::finish(Job& job) {
    auto it = std::find_if(lines_.begin(), lines_.end(), [&](const Line& l) { return l.name == job.name; });
    if (it == lines_.end()) return;
    Line& l = *it;
    const auto& o = job.offset;
    if (!job.error.empty()) {
        log::warn("neighbour zone {}: cannot load: {}", job.name, job.error);
        l.failed = true;
        return;
    }
    log::info("neighbour zone {}: offset ({:.1f} {:.1f} {:.1f}) (spread {:.1f} {:.1f} {:.1f}); {}", job.name,
              o.offset.x, o.offset.y, o.offset.z, o.spread.x, o.spread.y, o.spread.z, o.detail);
    if (!job.prepared) {
        game::NeighbourCheck check;
        check.outdoor_here = true;
        check.cells = l.lines.cells.size();
        check.open_there = job.open_there;
        check.return_line = job.return_line ? 1 : 0;
        l.verdict = game::neighbour_verdict(check);
        log::info("neighbour zone {}: open sky {:.0f}% past the seam, {}: {}", job.name, job.open_there * 100.0f,
                  job.return_line ? "a line back" : "no line back", game::to_string(l.verdict));
        return;
    }
    const auto t0 = Clock::now();
    try {
        l.renderer = std::make_unique<render::ZoneRenderer>(vk_, std::move(job.prepared), shader_dir_);
        l.bytes = job.bytes;
        log::info("neighbour zone {}: open sky {:.0f}%, prepared in {:.0f} ms on the loader, uploaded in {:.1f} ms: "
                  "{} triangles, {} textures, {:.1f} MB",
                  job.name, job.open_there * 100.0f, job.ms, ms_since(t0), l.renderer->stats().triangles,
                  l.renderer->stats().textures, static_cast<double>(job.bytes) / (1024.0 * 1024.0));
    } catch (const std::exception& e) {
        log::warn("neighbour zone {}: upload failed: {}", job.name, e.what());
        l.failed = true;
    }
}

void NeighbourZones::update(const glm::vec3& camera, float far_plane, bool enabled) {
    // Loads zoning cut off: joined once they end.
    std::erase_if(abandoned_, [](Abandoned& a) {
        if (!a.job->done) return false;
        if (a.thread.joinable()) a.thread.join();
        return true;
    });
    if (job_ && job_->done) {
        if (thread_.joinable()) thread_.join();
        finish(*job_);
        job_.reset();
    }
    if (!entered_) return;
    // Switched off: all of them go. Past the free reach: no longer drawn, and freed when another needs the room
    // (below) or on zoning, as freeing one waits for the GPU (~ZoneRenderer) and would hitch the frame.
    for (auto& l : lines_) {
        if (!l.renderer) continue;
        if (!enabled) {
            log::info("neighbour zone {}: freed ({:.1f} MB)", l.name, static_cast<double>(l.bytes) / (1024.0 * 1024.0));
            l.renderer.reset();
            continue;
        }
        const bool idle = game::distance_xy(camera, l.seam.lo, l.seam.hi) > far_plane + kFreeReach;
        if (idle != l.idle)
            log::info("neighbour zone {}: {}", l.name, idle ? "out of reach, not drawn" : "in reach again");
        l.idle = idle;
    }
    if (!enabled) pending_.reset();
    // The ground of a load about to start, a few rows a frame.
    if (pending_) {
        core::WorkBudget budget(kGroundBudgetMs);
        auto t = Clock::now();
        const auto& collision = *collision_;
        const float top = pending_->top;
        const bool done = pending_->ground.step(
            [&](float x, float y) { return floor_at(collision, glm::vec3(x, y, 0.0f), top); },
            [&] {
                const auto now = Clock::now();
                budget.done(std::chrono::duration<double, std::milli>(now - t).count());
                t = now;
                return budget.may_start();
            });
        budget.done(ms_since(t));
        pending_->ms += budget.spent_ms();
        ++pending_->frames;
        if (done) {
            start(*pending_);
            pending_.reset();
        }
        return;
    }
    // At most kMaxAbandoned loads cut off by zoning may still be running: past that the next load waits for them, so
    // rapid zoning does not pile up loader threads.
    if (!enabled || job_ || abandoned_.size() >= kMaxAbandoned) return;
    Line* next = nullptr;
    float best = far_plane + kLoadReach;
    for (auto& l : lines_) {
        if (l.renderer || l.failed || l.verdict != game::NeighbourVerdict::Draw) continue;
        const float d = game::distance_xy(camera, l.seam.lo, l.seam.hi);
        if (d < best) {
            best = d;
            next = &l;
        }
    }
    if (!next) return;
    if (loaded() >= kMaxLoaded) {
        // Make room: the farthest loaded one goes if it is farther than the new one.
        Line* far = nullptr;
        float worst = best;
        for (auto& l : lines_)
            if (l.renderer) {
                const float d = game::distance_xy(camera, l.seam.lo, l.seam.hi);
                if (d > worst) {
                    worst = d;
                    far = &l;
                }
            }
        if (!far) return;
        log::info("neighbour zone {}: freed for {}", far->name, next->name);
        far->renderer.reset();
        far->idle = false;
    }
    auto job = std::make_shared<Job>();
    job->name = next->name;
    job->lines = next->lines;
    job->seam = next->seam;
    job->first = next->first;
    job->points = next->points;
    job->floors_here = next->floors_here;
    job->max_images = vk_.caps().max_sampled_images;
    if (!collision_) {
        log::info("neighbour zone {}: loading ({:.0f} units from its line)", next->name, best);
        job_ = job;
        thread_ = std::thread(&NeighbourZones::run, std::move(job), &game_);
        return;
    }
    // The current zone's ground past the seam (its filler), sampled here over the next frames: the loader must not
    // touch this zone.
    glm::vec2 lo, hi;
    game::seam_band(next->seam, zone_lo_, zone_hi_, 50.0f, lo, hi);
    pending_.emplace(Pending{static_cast<std::size_t>(next - lines_.data()), std::move(job),
                             game::GroundSampler(lo, hi, kGroundCell), next->seam.hi.z + kGroundProbe, best});
}

void NeighbourZones::start(Pending& p) {
    const Line& l = lines_[p.line];
    p.job->ground = p.ground.take();
    log::info("neighbour zone {}: ground past the seam sampled on a {}x{} grid in {:.1f} ms over {} frames", l.name,
              p.job->ground.width, p.job->ground.height, p.ms, p.frames);
    log::info("neighbour zone {}: loading ({:.0f} units from its line)", l.name, p.distance);
    job_ = p.job;
    thread_ = std::thread(&NeighbourZones::run, std::move(p.job), &game_);
}

void NeighbourZones::set_scene_light(const game::SceneLight& light) {
    for (auto& l : lines_)
        if (l.renderer) l.renderer->set_scene_light(light);
}

void NeighbourZones::set_lighting(const render::SceneLighting& lighting) {
    for (auto& l : lines_)
        if (l.renderer) l.renderer->set_lighting(lighting);
}

void NeighbourZones::draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                          const game::ViewDistance& view, double time_seconds, render::ZonePhase phase) {
    const auto t0 = Clock::now();
    for (auto& l : lines_)
        if (l.renderer && !l.idle) {
            l.renderer->draw(frame, view_proj, camera_pos, view, time_seconds, phase);
            const auto d = l.renderer->take_drawn();
            triangles_acc_ += d.triangles;
            calls_acc_ += d.calls;
        }
    const double ms = ms_since(t0);
    if (phase == render::ZonePhase::Translucent || phase == render::ZonePhase::All) {
        draw_ms_ = draw_acc_ + ms;
        draw_acc_ = 0.0;
        drawn_triangles_ = triangles_acc_;
        drawn_calls_ = calls_acc_;
        triangles_acc_ = calls_acc_ = 0;
    } else {
        draw_acc_ = ms;
    }
}

} // namespace hailstone::app
