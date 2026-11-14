#include "game/neighbour_zones.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>

#include "assets/regions.h"
#include "game/coords.h"

namespace hailstone::game {

namespace {

float median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const std::size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5f * (v[m - 1] + v[m]);
}

float spread(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return *hi - *lo;
}

} // namespace

LineTarget line_target_from_server(float server_x, float server_y, float server_z) {
    // World x is the server's y and world y the server's x (game/coords.h).
    LineTarget t;
    t.position = server_to_world(glm::vec3(server_x, server_y, server_z));
    t.keep = glm::bvec3(keeps(server_y), keeps(server_x), keeps(server_z));
    for (int i = 0; i < 3; ++i)
        if (t.keep[i]) t.position[i] = 0.0f;
    return t;
}

std::vector<NeighbourLines> collect_neighbour_lines(const assets::ZoneRegions& regions, const glm::vec3& zone_lo,
                                                    const glm::vec3& zone_hi, const std::vector<ZonePointTarget>& points,
                                                    std::uint32_t current_zone) {
    // Each zone-line region's destination: (zone, a key for its target so each zone point counts once).
    struct Dest {
        std::uint32_t zone = 0;
        std::string key;
        LineTarget target;
    };
    std::map<std::uint32_t, Dest> by_region;
    for (std::uint32_t r = 1; r <= regions.region_count(); ++r) {
        const auto info = regions.info(r);
        if (!info.is_zone_line()) continue;
        const auto& z = *info.zone_line;
        Dest d;
        if (z.reference) {
            auto it = std::find_if(points.begin(), points.end(), [&](const ZonePointTarget& p) {
                return z.zone_point >= 0 && p.number == static_cast<std::uint32_t>(z.zone_point);
            });
            if (it == points.end()) continue;
            d.zone = it->zone_id;
            d.key = std::format("#{}", it->number);
            d.target = line_target_from_server(it->server_x, it->server_y, it->server_z);
        } else {
            d.zone = z.zone_id;
            d.key = info.tag;
            d.target = line_target_from_server(z.server_x, z.server_y, z.server_z);
        }
        if (d.zone == 0 || d.zone == current_zone) continue;
        by_region.emplace(r, std::move(d));
    }
    std::vector<NeighbourLines> out;
    if (by_region.empty()) return out;
    const glm::vec3 margin(200.0f, 200.0f, 200.0f);
    auto cells = regions.leaf_cells(zone_lo - margin, zone_hi + margin,
                                    [&](std::uint32_t r) { return by_region.contains(r); });
    std::map<std::uint32_t, std::size_t> group; // zone -> index in out
    std::map<std::uint32_t, std::vector<std::string>> keys;
    for (const auto& [r, d] : by_region) {
        auto [it, fresh] = group.try_emplace(d.zone, out.size());
        if (fresh) {
            out.push_back({});
            out.back().zone_id = d.zone;
        }
        auto& k = keys[d.zone];
        if (std::find(k.begin(), k.end(), d.key) == k.end()) {
            k.push_back(d.key);
            out[it->second].targets.push_back(d.target);
        }
    }
    for (const auto& c : cells) {
        auto it = by_region.find(c.region);
        if (it == by_region.end()) continue;
        out[group.at(it->second.zone)].cells.push_back({c.lo, c.hi});
    }
    std::erase_if(out, [](const NeighbourLines& l) { return l.cells.empty(); });
    for (auto& l : out) {
        l.lo = l.cells.front().lo;
        l.hi = l.cells.front().hi;
        for (const auto& c : l.cells) {
            l.lo = glm::min(l.lo, c.lo);
            l.hi = glm::max(l.hi, c.hi);
        }
    }
    return out;
}

Seam seam_of(const NeighbourLines& lines, const glm::vec3& zone_lo, const glm::vec3& zone_hi) {
    Seam s;
    s.lo = lines.lo;
    s.hi = lines.hi;
    s.centre = 0.5f * (lines.lo + lines.hi);
    // A line box clearly thinner one way than the other faces that way (a tunnel mouth near a zone's corner can be
    // nearer the other edge); a squarer one faces the zone edge it is nearest.
    const glm::vec2 size(lines.hi.x - lines.lo.x, lines.hi.y - lines.lo.y);
    int only = -1;
    if (size.x * kSeamThinRatio <= size.y) only = 0;
    else if (size.y * kSeamThinRatio <= size.x) only = 1;
    float best = std::numeric_limits<float>::max();
    for (int a = 0; a < 2; ++a) {
        if (only >= 0 && a != only) continue;
        const float low = s.centre[a] - zone_lo[a], high = zone_hi[a] - s.centre[a];
        if (low < best) {
            best = low;
            s.axis = a;
            s.sign = -1.0f;
        }
        if (high < best) {
            best = high;
            s.axis = a;
            s.sign = 1.0f;
        }
    }
    // The inner face: the side the player comes from.
    s.face = s.sign > 0.0f ? lines.lo[s.axis] : lines.hi[s.axis];
    return s;
}

std::optional<ReturnLine> find_return_line(const std::vector<LineCell>& neighbour_cells, const Seam& seam,
                                           const glm::vec3& arrival, float reach) {
    const int a = seam.axis;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    std::size_t n = 0;
    for (const auto& c : neighbour_cells) {
        if (distance_xy(arrival, c.lo, c.hi) > reach) continue;
        // Behind the arrival, towards the current zone (back along the seam's outward direction).
        const glm::vec3 mid = 0.5f * (c.lo + c.hi);
        if ((mid[a] - arrival[a]) * -seam.sign < -50.0f) continue;
        lo = glm::min(lo, c.lo);
        hi = glm::max(hi, c.hi);
        ++n;
    }
    if (n == 0) return std::nullopt;
    ReturnLine r;
    r.cells = n;
    r.lo = lo;
    r.hi = hi;
    r.centre = 0.5f * (lo + hi);
    // The neighbour's line faces the other way: its inner face is on the far side from the current zone.
    r.face = seam.sign > 0.0f ? hi[a] : lo[a];
    return r;
}

Seam return_seam(const ReturnLine& back, const Seam& seam) {
    Seam s;
    s.axis = seam.axis;
    s.sign = -seam.sign;
    s.face = back.face;
    s.centre = back.centre;
    s.lo = back.lo;
    s.hi = back.hi;
    return s;
}

NeighbourOffset offset_from_targets(const NeighbourLines& lines, const Seam& seam) {
    NeighbourOffset o;
    const int t = seam.axis, side = 1 - seam.axis;
    std::vector<float> e[3];
    for (const auto& target : lines.targets) {
        e[t].push_back(target.keep[t] ? 0.0f : seam.face - target.position[t]);
        e[side].push_back(target.keep[side] ? 0.0f : seam.centre[side] - target.position[side]);
        // z: kept, or found from the ground in refine_offset; the arrival's height is a spawn point over the ground.
        e[2].push_back(target.keep[2] ? 0.0f : seam.lo.z - target.position.z);
    }
    for (int i = 0; i < 3; ++i) {
        // A target that keeps a coordinate says the zones share it exactly: that wins over the others' estimates.
        const bool kept = std::any_of(lines.targets.begin(), lines.targets.end(), [&](const LineTarget& x) { return x.keep[i]; });
        o.offset[i] = kept ? 0.0f : median(e[i]);
        o.spread[i] = spread(e[i]);
    }
    o.estimates = lines.targets.size();
    o.detail = std::format("{} targets, axis {}{}, face {:.1f}", lines.targets.size(), seam.sign > 0 ? '+' : '-',
                           seam.axis == 0 ? 'x' : 'y', seam.face);
    return o;
}

NeighbourOffset refine_offset(NeighbourOffset o, const Seam& seam, const NeighbourLines& lines,
                              const std::optional<ReturnLine>& back, const std::vector<glm::vec3>& points,
                              const Floors& floor_here, const Floors& floor_there, float tolerance) {
    const int t = seam.axis, side = 1 - seam.axis;
    auto kept = [&](int axis) {
        return std::any_of(lines.targets.begin(), lines.targets.end(), [&](const LineTarget& x) { return x.keep[axis]; });
    };
    if (back) {
        if (!kept(t)) {
            const float d = seam.face - back->face;
            if (std::abs(d - o.offset[t]) <= tolerance) {
                o.detail += std::format("; faces {:.1f} (targets {:.1f})", d, o.offset[t]);
                o.offset[t] = d;
                o.return_line = true;
            } else {
                o.detail += std::format("; faces {:.1f} disagree with the targets' {:.1f}", d, o.offset[t]);
            }
        }
        if (!kept(side)) {
            const float d = seam.centre[side] - back->centre[side];
            if (std::abs(d - o.offset[side]) <= 2.0f * tolerance) {
                o.detail += std::format("; middles {:.1f} (targets {:.1f})", d, o.offset[side]);
                o.offset[side] = d;
                o.return_line = true;
            }
        }
    }
    if (floor_here && floor_there) {
        const glm::vec3 u = seam.outward();
        // Every pair of floors (one here, one there) at each point gives a candidate difference.
        std::vector<std::vector<float>> pairs;
        for (const auto& p : points) {
            const auto a = floor_here(p - 8.0f * u);
            const auto b = floor_there(p + 8.0f * u - glm::vec3(o.offset.x, o.offset.y, 0.0f));
            std::vector<float> d;
            for (float x : a)
                for (float y : b) d.push_back(x - y);
            if (!d.empty()) pairs.push_back(std::move(d));
        }
        // The difference the most points agree on (within kGroundAgree), ties to the one nearest the first estimate:
        // the targets' z is a spawn point's, so the estimate may be off by more than a layer's height (an invisible
        // floor, a bridge, a sea floor under the shore).
        const float hint = o.offset.z;
        std::optional<float> pick;
        std::size_t support = 0;
        for (const auto& list : pairs)
            for (float c : list) {
                std::size_t n = 0;
                for (const auto& other : pairs)
                    if (std::any_of(other.begin(), other.end(),
                                    [&](float d) { return std::abs(d - c) <= kGroundAgree; }))
                        ++n;
                if (n > support || (n == support && pick && std::abs(c - hint) < std::abs(*pick - hint))) {
                    support = n;
                    pick = c;
                }
            }
        std::vector<float> dz;
        if (pick)
            for (const auto& list : pairs) {
                const float d = *std::min_element(list.begin(), list.end(), [&](float x, float y) {
                    return std::abs(x - *pick) < std::abs(y - *pick);
                });
                if (std::abs(d - *pick) <= kGroundAgree) dz.push_back(d);
            }
        // Half the points at least find ground on both sides, and two of them and a third at least agree (a road
        // through hills the two zones shape differently: three of seven).
        if (dz.size() >= 2 && pairs.size() * 2 >= points.size() && dz.size() * 3 >= pairs.size()) {
            const float d = median(dz);
            o.detail += std::format("; ground {:.1f} from {} of {} points (spread {:.1f}, targets {:.1f})", d,
                                    dz.size(), points.size(), spread(dz), o.offset.z);
            o.offset.z = d;
            o.spread.z = spread(dz);
            o.ground = true;
        }
    }
    return o;
}

std::vector<glm::vec3> seam_points(const Seam& seam, int count) {
    std::vector<glm::vec3> out;
    const int side = 1 - seam.axis;
    const float a = seam.lo[side], b = seam.hi[side];
    for (int i = 0; i < count; ++i) {
        glm::vec3 p = seam.centre;
        p[seam.axis] = seam.face;
        const float f = count > 1 ? (static_cast<float>(i) + 0.5f) / static_cast<float>(count) : 0.5f;
        p[side] = a + (b - a) * f;
        out.push_back(p);
    }
    return out;
}

glm::vec4 seam_clip_plane(const Seam& seam, float margin) {
    const glm::vec3 u = seam.outward();
    return glm::vec4(u, -(seam.sign * seam.face + margin));
}

const char* to_string(NeighbourVerdict v) {
    switch (v) {
    case NeighbourVerdict::Draw: return "drawn";
    case NeighbourVerdict::Off: return "off";
    case NeighbourVerdict::IndoorHere: return "indoor zone";
    case NeighbourVerdict::NoLine: return "no zone line";
    case NeighbourVerdict::ClosedHere: return "line under a roof";
    case NeighbourVerdict::ClosedThere: return "arrival under a roof";
    case NeighbourVerdict::NoFiles: return "no files";
    case NeighbourVerdict::Disagree: return "zone points disagree";
    case NeighbourVerdict::NoReturn: return "no line back";
    }
    return "?";
}

NeighbourVerdict neighbour_verdict(const NeighbourCheck& c) {
    if (!c.enabled) return NeighbourVerdict::Off;
    if (!c.outdoor_here) return NeighbourVerdict::IndoorHere;
    if (c.cells == 0) return NeighbourVerdict::NoLine;
    if (!c.files) return NeighbourVerdict::NoFiles;
    if (c.open_here >= 0.0f && c.open_here < kOpenSkyShare) return NeighbourVerdict::ClosedHere;
    if (c.spread > kMaxTargetSpread) return NeighbourVerdict::Disagree;
    if (c.open_there >= 0.0f && c.open_there < kOpenSkyShare) return NeighbourVerdict::ClosedThere;
    if (c.return_line == 0) return NeighbourVerdict::NoReturn;
    return NeighbourVerdict::Draw;
}

float open_sky_share(const std::vector<glm::vec3>& points,
                     const std::function<std::optional<float>(const glm::vec3&)>& floor,
                     const std::function<std::optional<float>(const glm::vec3&)>& ceiling) {
    std::size_t tested = 0, open = 0;
    for (const auto& p : points) {
        auto f = floor(p);
        if (!f) continue;
        ++tested;
        auto c = ceiling(glm::vec3(p.x, p.y, *f + 5.0f));
        if (!c || *c - *f > kOpenSkyHeight) ++open;
    }
    return tested ? static_cast<float>(open) / static_cast<float>(tested) : 0.0f;
}

std::optional<float> GroundGrid::at(float x, float y) const {
    if (heights.empty()) return std::nullopt;
    const float fx = std::floor((x - origin.x) / cell), fy = std::floor((y - origin.y) / cell);
    if (!(fx >= 0.0f && fy >= 0.0f && fx < static_cast<float>(width) && fy < static_cast<float>(height))) return std::nullopt;
    const float h = heights[static_cast<std::size_t>(fy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(fx)];
    if (std::isnan(h)) return std::nullopt;
    return h;
}

void seam_band(const Seam& seam, const glm::vec3& zone_lo, const glm::vec3& zone_hi, float margin, glm::vec2& lo,
               glm::vec2& hi) {
    const int a = seam.axis, side = 1 - seam.axis;
    lo[side] = zone_lo[side] - margin;
    hi[side] = zone_hi[side] + margin;
    if (seam.sign > 0.0f) {
        lo[a] = seam.face;
        hi[a] = std::max(seam.face, zone_hi[a]) + margin;
    } else {
        lo[a] = std::min(seam.face, zone_lo[a]) - margin;
        hi[a] = seam.face;
    }
}

GroundGrid sample_ground(const glm::vec2& lo, const glm::vec2& hi, float cell,
                         const std::function<std::optional<float>(float x, float y)>& floor) {
    GroundSampler sampler(lo, hi, cell);
    sampler.step(floor, [] { return true; });
    return sampler.take();
}

GroundSampler::GroundSampler(const glm::vec2& lo, const glm::vec2& hi, float cell) {
    GroundGrid& g = grid_;
    g.origin = lo;
    g.cell = cell;
    g.width = std::max(0, static_cast<int>(std::ceil((hi.x - lo.x) / cell)));
    g.height = std::max(0, static_cast<int>(std::ceil((hi.y - lo.y) / cell)));
    if (g.width == 0 || g.height == 0 || static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height) > 4'000'000) {
        g.width = g.height = 0;
        return;
    }
    g.heights.assign(static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height),
                     std::numeric_limits<float>::quiet_NaN());
}

bool GroundSampler::step(const std::function<std::optional<float>(float x, float y)>& floor,
                         const std::function<bool()>& more) {
    GroundGrid& g = grid_;
    for (bool first = true; row_ < g.height && (first || more()); first = false, ++row_) {
        const int y = row_;
        for (int x = 0; x < g.width; ++x)
            if (auto h = floor(g.origin.x + (static_cast<float>(x) + 0.5f) * g.cell,
                               g.origin.y + (static_cast<float>(y) + 0.5f) * g.cell))
                g.heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.width) + static_cast<std::size_t>(x)] = *h;
    }
    return done();
}

std::optional<float> GroundGrid::height_at(float x, float y) const {
    const auto own = at(x, y);
    if (!own) return std::nullopt;
    // The four cell centres round (x, y).
    const float fx = (x - origin.x) / cell - 0.5f, fy = (y - origin.y) / cell - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    auto sample = [&](float i, float j) {
        const auto h = at(origin.x + (i + 0.5f) * cell, origin.y + (j + 0.5f) * cell);
        return h ? *h : *own;
    };
    const float bottom = sample(x0, y0) + (sample(x0 + 1.0f, y0) - sample(x0, y0)) * tx;
    const float top = sample(x0, y0 + 1.0f) + (sample(x0 + 1.0f, y0 + 1.0f) - sample(x0, y0 + 1.0f)) * tx;
    return bottom + (top - bottom) * ty;
}

bool GroundGrid::inside_ground(float x, float y) const {
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            if (!at(x + static_cast<float>(i) * cell, y + static_cast<float>(j) * cell)) return false;
    return true;
}

namespace {

// A triangle cut along a grid's cells (origin, cell, width x height): each piece as barycentric weights, the
// outermost cells reaching out to the triangle's box so no part goes missing. `drops(piece)` says which go. Returns the
// pieces to keep (empty: none), or nullopt when none goes (the whole triangle stays). A triangle wholly outside the
// grid stays; a huge one (far more cells than any ground triangle spans) is decided whole.
template <class Drops>
std::optional<std::vector<BaryPolygon>> split_cells(glm::vec2 origin, float cell, int width, int height,
                                                    const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                                    Drops&& drops) {
    auto position = [&](const glm::vec3& w) { return a * w.x + b * w.y + c * w.z; };
    const glm::vec2 lo = glm::min(glm::min(glm::vec2(a), glm::vec2(b)), glm::vec2(c));
    const glm::vec2 hi = glm::max(glm::max(glm::vec2(a), glm::vec2(b)), glm::vec2(c));
    const float gx1 = origin.x + static_cast<float>(width) * cell;
    const float gy1 = origin.y + static_cast<float>(height) * cell;
    if (hi.x <= origin.x || hi.y <= origin.y || lo.x >= gx1 || lo.y >= gy1) return std::nullopt;
    const int i0 = static_cast<int>(std::floor((lo.x - origin.x) / cell));
    const int i1 = static_cast<int>(std::floor((hi.x - origin.x) / cell));
    const int j0 = static_cast<int>(std::floor((lo.y - origin.y) / cell));
    const int j1 = static_cast<int>(std::floor((hi.y - origin.y) / cell));
    const BaryPolygon whole{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    if ((i0 == i1 && j0 == j1) || static_cast<long>(i1 - i0 + 1) * static_cast<long>(j1 - j0 + 1) > 4096) {
        if (!drops(whole)) return std::nullopt;
        return std::vector<BaryPolygon>{};
    }
    std::vector<BaryPolygon> keep;
    bool dropped = false;
    BaryPolygon poly, scratch;
    auto lerp = [](const glm::vec3& p, const glm::vec3& q, float t) { return p + (q - p) * t; };
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const float x0 = origin.x + static_cast<float>(i) * cell, x1 = x0 + cell;
            const float y0 = origin.y + static_cast<float>(j) * cell, y1 = y0 + cell;
            poly = whole;
            if (i > i0) clip_polygon(poly, glm::vec4(1.0f, 0.0f, 0.0f, -x0), position, lerp, scratch);
            if (i < i1) clip_polygon(poly, glm::vec4(-1.0f, 0.0f, 0.0f, x1), position, lerp, scratch);
            if (j > j0) clip_polygon(poly, glm::vec4(0.0f, 1.0f, 0.0f, -y0), position, lerp, scratch);
            if (j < j1) clip_polygon(poly, glm::vec4(0.0f, -1.0f, 0.0f, y1), position, lerp, scratch);
            if (poly.empty()) continue;
            if (drops(poly)) dropped = true;
            else keep.push_back(poly);
        }
    if (!dropped) return std::nullopt;
    return keep;
}

bool faces_up(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    return len > 1e-6f && std::abs(n.z) >= 0.6f * len;
}

} // namespace

std::optional<std::vector<BaryPolygon>> split_off_ground(const GroundGrid& ground, const glm::vec3& a,
                                                         const glm::vec3& b, const glm::vec3& c, float tolerance) {
    if (ground.empty() || !faces_up(a, b, c)) return std::nullopt;
    auto position = [&](const glm::vec3& w) { return a * w.x + b * w.y + c * w.z; };
    return split_cells(ground.origin, ground.cell, ground.width, ground.height, a, b, c, [&](const BaryPolygon& poly) {
        for (const glm::vec3& w : poly) {
            const glm::vec3 p = position(w);
            const auto h = ground.height_at(p.x, p.y);
            if (!h || std::abs(p.z - *h) > tolerance || !ground.inside_ground(p.x, p.y)) return false;
        }
        return true;
    });
}

std::optional<float> LandMask::ground_at(float x, float y) const {
    if (empty()) return std::nullopt;
    auto cell_at = [&](int i, int j) -> std::optional<float> {
        if (i < 0 || j < 0 || i >= width || j >= height) return std::nullopt;
        const float h = ground[static_cast<std::size_t>(j) * static_cast<std::size_t>(width) + static_cast<std::size_t>(i)];
        if (std::isnan(h)) return std::nullopt;
        return h;
    };
    const float gx = (x - origin.x) / cell, gy = (y - origin.y) / cell;
    if (!(gx >= 0.0f && gy >= 0.0f && gx < static_cast<float>(width) && gy < static_cast<float>(height)))
        return std::nullopt;
    const auto own = cell_at(static_cast<int>(gx), static_cast<int>(gy));
    if (!own) return std::nullopt;
    const float fx = gx - 0.5f, fy = gy - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    auto sample = [&](float i, float j) {
        const auto h = cell_at(static_cast<int>(i), static_cast<int>(j));
        return h ? *h : *own;
    };
    const float bottom = sample(x0, y0) + (sample(x0 + 1.0f, y0) - sample(x0, y0)) * tx;
    const float top = sample(x0, y0 + 1.0f) + (sample(x0 + 1.0f, y0 + 1.0f) - sample(x0, y0 + 1.0f)) * tx;
    return bottom + (top - bottom) * ty;
}

bool LandMask::stands_at(float x, float y) const {
    if (empty()) return true;
    const float gx = (x - origin.x) / cell, gy = (y - origin.y) / cell;
    if (!(gx >= 0.0f && gy >= 0.0f && gx < static_cast<float>(width) && gy < static_cast<float>(height))) return true;
    return stands[static_cast<std::size_t>(static_cast<int>(gy)) * static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(static_cast<int>(gx))] != 0;
}

std::optional<std::vector<BaryPolygon>> split_off_land(const LandMask& mask, const glm::vec3& a, const glm::vec3& b,
                                                       const glm::vec3& c, float tolerance) {
    if (mask.empty()) return std::nullopt;
    const bool up = faces_up(a, b, c);
    auto position = [&](const glm::vec3& w) { return a * w.x + b * w.y + c * w.z; };
    return split_cells(mask.origin, mask.cell, mask.width, mask.height, a, b, c, [&](const BaryPolygon& poly) {
        glm::vec3 mid(0.0f);
        bool ground = up;
        for (const glm::vec3& w : poly) {
            const glm::vec3 p = position(w);
            mid += p;
            if (ground) {
                const auto h = mask.ground_at(p.x, p.y);
                ground = h && std::abs(p.z - *h) <= tolerance;
            }
        }
        mid /= static_cast<float>(poly.size());
        return ground || !mask.stands_at(mid.x, mid.y);
    });
}

float distance_xy(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) {
    const float dx = std::max({lo.x - p.x, 0.0f, p.x - hi.x});
    const float dy = std::max({lo.y - p.y, 0.0f, p.y - hi.y});
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace hailstone::game
