#include "app/neighbour_zones.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <span>

#include "assets/zone.h"
#include "core/atomic_file.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "core/work_budget.h"
#include "game/collision.h"
#include "game/zone_names.h"
#include "platform/crash_log.h"
#include "platform/paths.h"
#include "platform/process.h"

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
// The neighbour's ground for the land round the current zone (render::HorizonSource): at most this far past the
// current zone's box, sampled kGroundSamples x kGroundSamples times a square (their median, from two hits).
constexpr float kSourceReach = 8000.0f;
constexpr int kGroundSamples = 2;
constexpr int kSourceMaxSide = 1024;
// A cached source is used when it was moved by an offset within this of the one found now.
constexpr float kCacheOffsetSlack = 1.0f;
// Where the land stands in for the neighbour's ground (game::LandMask): its cells, and the neighbour's ground within
// this height of its own measured ground is ground; what stands over a land lower than that by more is left out.
constexpr float kLandMaskCell = 64.0f;
constexpr float kLandTolerance = 24.0f;
// Its terrain is drawn where its part of the land has sunk at least this far under it (render::horizon_point): the
// two then stay apart by kHorizonTuck times it (no fighting over pixels), and where its terrain stops, the step down to
// the land is at most that.
constexpr float kTerrainSettled = 0.3f;
// How many cells on from where its terrain is not drawn it is left out while it stands over the land (land_mask).
constexpr int kFinReach = 6;

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

// The neighbour's ground in the current zone's coordinates (render::HorizonSource): its terrain's highest visible
// ground (`collision` built without its placed objects) on squares over its box (moved by `offset`, within
// kSourceReach of the current zone's box), past the seam only (before it lies the current zone). A square takes the
// median of its samples, from two hits at least, and keeps the highest of them (HorizonSource::top).
render::HorizonSource measure_ground(const std::string& name, const assets::Zone& zone,
                                     const game::CollisionMesh& collision, const glm::vec3& offset,
                                     const game::Seam& seam, const glm::vec3& here_lo, const glm::vec3& here_hi,
                                     const std::atomic<bool>& cancelled) {
    const float cell = render::kHorizonCell;
    glm::vec2 lo = glm::max(glm::vec2(zone.bounds_min) + glm::vec2(offset), glm::vec2(here_lo) - kSourceReach);
    glm::vec2 hi = glm::min(glm::vec2(zone.bounds_max) + glm::vec2(offset), glm::vec2(here_hi) + kSourceReach);
    lo = glm::floor(lo / cell) * cell;
    const int nx = std::min(kSourceMaxSide, static_cast<int>(std::ceil((hi.x - lo.x) / cell)));
    const int ny = std::min(kSourceMaxSide, static_cast<int>(std::ceil((hi.y - lo.y) / cell)));
    if (nx <= 0 || ny <= 0) return {};
    const glm::vec4 plane = game::seam_clip_plane(seam, 0.0f);
    const float top = zone.bounds_max.z + 10.0f;
    std::vector<float> ground(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny),
                              std::numeric_limits<float>::quiet_NaN());
    std::vector<float> highest = ground;
    std::vector<float> hits;
    for (int y = 0; y < ny; ++y) {
        if (cancelled) return {};
        for (int x = 0; x < nx; ++x) {
            const glm::vec2 corner = lo + glm::vec2(static_cast<float>(x), static_cast<float>(y)) * cell;
            if (game::plane_side(plane, glm::vec3(corner + 0.5f * cell, seam.centre.z)) < 0.0f) continue;
            hits.clear();
            for (int j = 0; j < kGroundSamples; ++j)
                for (int i = 0; i < kGroundSamples; ++i) {
                    const glm::vec2 p = corner + (glm::vec2(static_cast<float>(i), static_cast<float>(j)) + 0.5f) *
                                                     (cell / static_cast<float>(kGroundSamples));
                    if (auto h = floor_at(collision, glm::vec3(p - glm::vec2(offset), 0.0f), top)) hits.push_back(*h);
                }
            if (hits.size() < 2) continue;
            std::sort(hits.begin(), hits.end());
            const std::size_t m = hits.size() / 2;
            const float h = hits.size() % 2 ? hits[m] : 0.5f * (hits[m - 1] + hits[m]);
            const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
            ground[i] = h + offset.z;
            highest[i] = hits.back() + offset.z;
        }
    }
    return render::horizon_source(name, lo, cell, nx, ny, std::move(ground), std::move(highest));
}

// Where the land stands in for the neighbour's ground (game::LandMask), over its measured ground: its terrain is drawn
// where its part of the land has sunk under it (render::horizon_point with `only`, its index in `sources`, at least
// kTerrainSettled); what stands there is drawn where the land (over the current zone's own terrain, that terrain,
// `here`) lies no lower than the neighbour's ground less kLandTolerance. Where its terrain stops, it must stop where it
// meets the land (game::spread_terrain_cut, `highest` its terrain's own highest points, render::source_top, at most
// kFinReach cells on).
game::LandMask land_mask(const render::HorizonField& field, const render::HorizonSources& sources, int only,
                         const render::HorizonSource& self, const game::GroundGrid& here,
                         const std::function<std::optional<float>(glm::vec2)>& highest,
                         const std::atomic<bool>& cancelled) {
    game::LandMask mask;
    // The box of the measured ground (the source is padded by its fade).
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (int y = 0; y < self.ny; ++y)
        for (int x = 0; x < self.nx; ++x)
            if (std::isfinite(self.height[static_cast<std::size_t>(y) * static_cast<std::size_t>(self.nx) +
                                          static_cast<std::size_t>(x)])) {
                const glm::vec2 c = self.lo + glm::vec2(static_cast<float>(x), static_cast<float>(y)) * self.cell;
                lo = glm::min(lo, c);
                hi = glm::max(hi, c + self.cell);
            }
    if (!(lo.x < hi.x)) return mask;
    mask.origin = lo;
    mask.cell = kLandMaskCell;
    mask.width = static_cast<int>(std::ceil((hi.x - lo.x) / mask.cell));
    mask.height = static_cast<int>(std::ceil((hi.y - lo.y) / mask.cell));
    const std::size_t n = static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height);
    mask.ground.assign(n, std::numeric_limits<float>::quiet_NaN());
    mask.stands.assign(n, 1);
    mask.terrain.assign(n, 1);
    mask.own.assign(n, 0);
    mask.surface.assign(n, std::numeric_limits<float>::quiet_NaN());
    for (int y = 0; y < mask.height; ++y) {
        if (cancelled) return {};
        for (int x = 0; x < mask.width; ++x) {
            const glm::vec2 p = mask.origin + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) * mask.cell;
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.width) +
                                  static_cast<std::size_t>(x);
            mask.own[i] = field.covers(p);
            const auto g = render::source_ground(self, p);
            if (!g) continue;
            mask.ground[i] = *g;
            const render::HorizonPoint land = render::horizon_point(field, sources, p, only);
            mask.terrain[i] = land.settled >= kTerrainSettled;
            if (field.covers(p)) {
                const auto z = here.height_at(p.x, p.y);
                mask.stands[i] = !z || *z >= *g - kLandTolerance;
                if (z) mask.surface[i] = *z;
            } else {
                mask.surface[i] = land.height;
                mask.stands[i] = render::horizon_height(field, sources, p) >= *g - kLandTolerance;
            }
        }
    }
    // The fins: from where its terrain is not drawn into where it is, while it stands over the land.
    const std::size_t spread =
        game::spread_terrain_cut(mask, highest, kLandTolerance, kFinReach, [&] { return cancelled.load(); });
    if (spread) log::debug("neighbour zone {}: its terrain left out of {} more cells where it stood over the land",
                           self.name, spread);
    return mask;
}


} // namespace

std::filesystem::path horizon_cache_dir(std::string_view server, std::string_view zone) {
    return platform::cache_dir() / "horizon" / std::string(server.empty() ? std::string_view("default") : server) /
           std::string(zone);
}

std::uint64_t horizon_cache_key(const GameDir& game, std::string_view zone, std::string_view neighbour) {
    auto size_of = [&](std::string_view z) -> std::uint64_t {
        for (const char* ext : {".s3d", ".eqg"})
            if (auto p = game.find(std::string(z) + ext)) {
                std::error_code ec;
                const auto n = std::filesystem::file_size(*p, ec);
                if (!ec) return static_cast<std::uint64_t>(n);
            }
        return 0;
    };
    const std::uint64_t a = size_of(zone), b = size_of(neighbour);
    if (a == 0 || b == 0) return 0;
    return (a * 0x9e3779b97f4a7c15ull) ^ (b + 0x632be59bd9b4e019ull + (a << 6) + (a >> 2));
}

std::vector<render::HorizonCacheEntry> horizon_cache_entries(const GameDir& game, const std::filesystem::path& dir,
                                                            std::string_view zone) {
    std::vector<render::HorizonCacheEntry> out;
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".bin") continue;
        const auto text = read_text_file(entry.path());
        if (!text) continue;
        auto e = render::read_horizon_cache(std::as_bytes(std::span(text->data(), text->size())));
        if (!e || e->zone != zone || !e->source.valid()) continue;
        const std::uint64_t key = horizon_cache_key(game, zone, e->neighbour);
        if (key == 0 || key != e->key) {
            log::info("neighbour zone {}: its cached ground is for other files, measured again", e->neighbour);
            continue;
        }
        out.push_back(std::move(*e));
    }
    return out;
}

render::HorizonSources cached_horizon_sources(const GameDir& game, const std::filesystem::path& dir,
                                              std::string_view zone) {
    render::HorizonSources out;
    for (auto& e : horizon_cache_entries(game, dir, zone))
        out.push_back(std::make_shared<const render::HorizonSource>(std::move(e.source)));
    render::sort_sources(out);
    return out;
}

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
    bool survey = false; // only measure its ground (render::HorizonSource), for the land; draw nothing
    glm::vec3 here_lo{0.0f}, here_hi{0.0f}; // the current zone's box
    std::shared_ptr<const render::HorizonBuild> horizon; // the current zone's land (null: none)
    render::HorizonSources sources;                      // the other neighbours' ground it is shaped by
    std::map<std::string, std::vector<std::byte>> land_files;
    std::shared_ptr<const render::HorizonSource> cached; // the cache's, moved by cached_offset
    glm::vec3 cached_offset{0.0f};
    std::string zone_name;
    std::filesystem::path cache_path;
    std::uint64_t cache_key = 0;
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
    std::shared_ptr<const render::HorizonSource> source; // its ground, as used
    bool source_new = false;                             // measured now (not the cache's)
    std::size_t land_squares = 0;                        // the squares of the land it draws
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
    horizon_.reset();
    sources_.clear();
    land_files_.clear();
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
    zone_name_ = in.zone->name;
    horizon_ = in.horizon;
    cache_dir_ = in.cache_dir;
    if (horizon_) {
        sources_ = horizon_->sources;
        // The land's textures, which the neighbours' parts of it borrow (read here: the loader must not touch this
        // zone).
        for (const render::HorizonLook* look : {&horizon_->ground, &horizon_->rock})
            for (const auto& name : look->frames)
                if (!land_files_.contains(name))
                    if (auto data = in.zone->read_file(name)) land_files_.emplace(name, std::move(*data));
    }
    const auto cached = horizon_ ? horizon_cache_entries(game_, cache_dir_, zone_name_)
                                 : std::vector<render::HorizonCacheEntry>{};
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
        if (!l.name.empty()) l.cache_key = horizon_cache_key(game_, zone_name_, l.name);
        for (const auto& e : cached)
            if (e.neighbour == l.name) {
                l.source = std::make_shared<const render::HorizonSource>(e.source);
                l.source_offset = e.offset;
            }
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
        // Its ground in the current zone's coordinates, for the land round the current zone: the cache's when it was
        // moved by the same offset, else measured here, from its terrain alone (its placed objects, trees and houses,
        // are not its ground), and cached.
        if (job->cached && glm::distance(job->cached_offset, job->offset.offset) <= kCacheOffsetSlack) {
            job->source = job->cached;
        } else {
            auto placements = std::move(zone.placements);
            const game::CollisionMesh terrain = game::CollisionMesh::build(zone);
            zone.placements = std::move(placements);
            if (stop()) return;
            auto measured = measure_ground(job->name, zone, terrain, job->offset.offset, job->seam, job->here_lo,
                                           job->here_hi, job->cancelled);
            if (stop()) return;
            if (measured.valid()) {
                job->source = std::make_shared<const render::HorizonSource>(std::move(measured));
                job->source_new = true;
                if (!job->cache_path.empty() && job->cache_key != 0) {
                    render::HorizonCacheEntry entry;
                    entry.zone = job->zone_name;
                    entry.neighbour = job->name;
                    entry.key = job->cache_key;
                    entry.offset = job->offset.offset;
                    entry.source = *job->source;
                    const auto bytes = render::write_horizon_cache(entry);
                    write_file_atomic(job->cache_path,
                                      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                }
            }
        }
        if (job->survey || stop()) {
            job->ms = ms_since(t0);
            job->done = true;
            return;
        }
        for (auto& l : zone.lights) l.position += job->offset.offset;
        render::ZoneRenderOptions options;
        options.multipass_lighting = false;
        options.offset = job->offset.offset;
        options.keep = {game::seam_clip_plane(job->seam, kSeamMargin)};
        // Its part of the land round the current zone, which draws its ground there; of its own geometry only what
        // stands on that ground stays (game::split_off_land). Without the land: its ground goes only where the current
        // zone's doubles it (game::split_off_ground).
        game::LandMask mask;
        std::shared_ptr<render::LandPatch> patch;
        if (job->horizon && job->source) {
            render::HorizonSources all = job->sources;
            all.push_back(job->source);
            render::sort_sources(all);
            const auto self = std::find(all.begin(), all.end(), job->source);
            if (self != all.end()) {
                patch = std::make_shared<render::LandPatch>();
                patch->mesh = render::horizon_mesh(job->horizon->field, all, static_cast<int>(self - all.begin()));
                patch->ground = job->horizon->ground;
                patch->rock = job->horizon->rock;
                patch->files = job->land_files;
                if (patch->mesh.squares.empty()) patch.reset();
                else {
                    mask = land_mask(
                        job->horizon->field, all, static_cast<int>(self - all.begin()), *job->source, job->ground,
                        [&](glm::vec2 p) { return render::source_top(*job->source, p); },
                        job->cancelled);
                    // Where its terrain is left out, its part of the land comes back up from under it.
                    for (std::size_t v = 0; v < patch->mesh.positions.size() && v < patch->mesh.settled.size(); ++v) {
                        glm::vec3& at = patch->mesh.positions[v];
                        if (patch->mesh.settled[v] > 0.0f && !mask.terrain_at(at.x, at.y)) {
                            at.z += render::kHorizonTuck * patch->mesh.settled[v];
                            patch->mesh.settled[v] = 0.0f;
                        }
                    }
                }
                if (stop()) return;
            }
        }
        if (patch && !mask.empty()) {
            job->land_squares = patch->mesh.squares.size();
            options.land = patch;
            options.split = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                render::ZoneRenderOptions::Part part) {
                using Part = render::ZoneRenderOptions::Part;
                const game::LandPart as = part == Part::Terrain ? game::LandPart::Terrain
                                          : part == Part::Liquid ? game::LandPart::Liquid
                                                                 : game::LandPart::Placed;
                return game::split_off_land(mask, a, b, c, as, kLandTolerance);
            };
        } else if (!job->ground.empty()) {
            options.split = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, render::ZoneRenderOptions::Part) {
                return game::split_off_ground(job->ground, a, b, c, kGroundTolerance);
            };
        }
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
    if (job.source) {
        l.source = job.source;
        l.source_offset = o.offset;
        if (job.source_new) {
            std::erase_if(sources_, [&](const auto& s) { return s->name == job.name; });
            sources_.push_back(job.source);
            render::sort_sources(sources_);
        }
        log::info("neighbour zone {}: its ground for the land round the zone {} ({}x{} squares){}", job.name,
                  job.source_new ? "measured and cached" : "from the cache", job.source->nx, job.source->ny,
                  job.survey ? ", the next visit's land takes its shape" : "");
    }
    if (job.survey && job.error.empty() && job.source) return;
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
        l.renderer->set_distant(true); // its part of the land round the zone
        l.renderer->set_debug_tint(debug_tint_);
        l.bytes = job.bytes;
        log::info("neighbour zone {}: open sky {:.0f}%, prepared in {:.0f} ms on the loader, uploaded in {:.1f} ms: "
                  "{} triangles, {} textures, {:.1f} MB, {} squares of the land round the zone",
                  job.name, job.open_there * 100.0f, job.ms, ms_since(t0), l.renderer->stats().triangles,
                  l.renderer->stats().textures, static_cast<double>(job.bytes) / (1024.0 * 1024.0), job.land_squares);
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
    if (!next) {
        // Nothing to draw: the next qualifying neighbour whose ground is not known yet is measured, so the land takes
        // its shape on the next visit (and when it is drawn).
        if (!horizon_) return;
        Line* survey = nullptr;
        float nearest = std::numeric_limits<float>::max();
        for (auto& l : lines_) {
            if (l.source || l.surveyed || l.failed || l.renderer || l.verdict != game::NeighbourVerdict::Draw) continue;
            const float d = game::distance_xy(camera, l.seam.lo, l.seam.hi);
            if (d < nearest) {
                nearest = d;
                survey = &l;
            }
        }
        if (!survey) return;
        survey->surveyed = true;
        log::info("neighbour zone {}: measuring its ground for the land round the zone", survey->name);
        job_ = make_job(*survey, true);
        thread_ = std::thread(&NeighbourZones::run, job_, &game_);
        return;
    }
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
    auto job = make_job(*next, false);
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

std::shared_ptr<NeighbourZones::Job> NeighbourZones::make_job(const Line& l, bool survey) const {
    auto job = std::make_shared<Job>();
    job->name = l.name;
    job->lines = l.lines;
    job->seam = l.seam;
    job->first = l.first;
    job->points = l.points;
    job->floors_here = l.floors_here;
    job->max_images = vk_.caps().max_sampled_images;
    job->survey = survey;
    job->here_lo = zone_lo_;
    job->here_hi = zone_hi_;
    job->horizon = horizon_;
    for (const auto& s : sources_)
        if (s->name != l.name) job->sources.push_back(s);
    if (!survey) job->land_files = land_files_;
    job->cached = l.source;
    job->cached_offset = l.source_offset;
    job->zone_name = zone_name_;
    if (!cache_dir_.empty()) job->cache_path = cache_dir_ / (l.name + ".bin");
    job->cache_key = l.cache_key;
    return job;
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

void NeighbourZones::set_debug_tint(bool on) {
    debug_tint_ = on;
    for (auto& l : lines_)
        if (l.renderer) l.renderer->set_debug_tint(on);
}

void NeighbourZones::set_lod_scale(float scale) {
    for (auto& l : lines_)
        if (l.renderer) l.renderer->set_lod_scale(scale);
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
