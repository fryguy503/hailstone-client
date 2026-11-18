#include "render/horizon.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "core/byte_io.h"

namespace hailstone::render {

namespace {

// The value at which `share` of the total weight lies below, of (value, weight) pairs (sorted here).
float weighted_percentile(std::vector<std::pair<float, float>>& samples, float total, float share) {
    std::sort(samples.begin(), samples.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    const float at = total * share;
    float run = 0.0f;
    for (const auto& [value, weight] : samples) {
        run += weight;
        if (run >= at) return value;
    }
    return samples.back().first;
}

float smooth(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Value noise from an integer hash, the same on every platform.
float lattice(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u +
                      seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xffffffu) / 16777216.0f;
}

float value_noise(glm::vec2 p, std::uint32_t seed) {
    const glm::vec2 i = glm::floor(p);
    glm::vec2 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const auto x = static_cast<std::int32_t>(i.x), y = static_cast<std::int32_t>(i.y);
    const float a = lattice(x, y, seed), b = lattice(x + 1, y, seed);
    const float c = lattice(x, y + 1, seed), d = lattice(x + 1, y + 1, seed);
    return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
}

// Octaves of value noise, 0..1; ridged: sharp crests (mountains).
float fbm(glm::vec2 p, int octaves, std::uint32_t seed, bool ridged) {
    float h = 0.0f, amp = 0.5f, total = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        float n = value_noise(p, seed + static_cast<std::uint32_t>(o) * 101u);
        if (ridged) n = 1.0f - std::abs(n * 2.0f - 1.0f);
        h += n * amp;
        total += amp;
        amp *= 0.5f;
        p = glm::vec2(1.6f * p.x - 1.2f * p.y, 1.2f * p.x + 1.6f * p.y) + glm::vec2(5.2f, 1.3f);
    }
    return h / total;
}

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The mesh's lines along one axis: `step` apart over lo..hi, then wider by kHorizonGrowth a step out to `reach` past
// it.
std::vector<float> axis_lines(float lo, float hi, float step, float reach) {
    std::vector<float> inner;
    const int n = std::max(1, static_cast<int>(std::ceil((hi - lo) / step)));
    for (int k = 0; k <= n; ++k) inner.push_back(lo + static_cast<float>(k) * step);
    std::vector<float> left, right;
    float s = step;
    for (float x = inner.front(); x > inner.front() - reach;) {
        s *= kHorizonGrowth;
        x -= s;
        left.push_back(x);
    }
    s = step;
    for (float x = inner.back(); x < inner.back() + reach;) {
        s *= kHorizonGrowth;
        x += s;
        right.push_back(x);
    }
    std::vector<float> out(left.rbegin(), left.rend());
    out.insert(out.end(), inner.begin(), inner.end());
    out.insert(out.end(), right.begin(), right.end());
    return out;
}

// Marks the square under `p` as covered by terrain (its lowest and highest point) or water (its surface).
void mark(HorizonField& g, const glm::vec3& p, bool liquid) {
    const auto fx = std::floor((p.x - g.lo.x) / g.cell), fy = std::floor((p.y - g.lo.y) / g.cell);
    if (!(fx >= 0.0f && fy >= 0.0f && fx < static_cast<float>(g.nx) && fy < static_cast<float>(g.ny))) return;
    const std::size_t i = g.at(static_cast<int>(fx), static_cast<int>(fy));
    if (liquid) {
        g.wet[i] = 1;
        g.water[i] = std::max(g.water[i], p.z);
    } else {
        g.dry[i] = 1;
        g.zmin[i] = std::min(g.zmin[i], p.z);
        g.zmax[i] = std::max(g.zmax[i], p.z);
    }
}

// Past the ground (the squares `off` it), how much of the nearest stretch of its edge is water, 0..1. A square takes
// its nearest ground square's (`nearest`) as water when there is water (`water`) straight in from it within
// kHorizonShoreWall: a harbour's wall or quay along the edge (South Qeynos's) does not close the sea off, and straight
// in only, so a river's mouth stays as narrow as it is. Then averaged over the squares off the ground within half
// kHorizonShoreWidth, one axis then the other: a river's mouth is no shore.
std::vector<float> shore_share(int nx, int ny, float cell, const std::vector<int>& nearest,
                               const std::vector<std::uint8_t>& off, const std::vector<std::uint8_t>& water) {
    const std::size_t n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    std::vector<float> edge(n, 0.0f);
    const auto unx = static_cast<std::size_t>(nx);
    const float wall = kHorizonShoreWall / cell;
    for (std::size_t i = 0; i < n; ++i) {
        if (!off[i] || nearest[i] < 0) continue;
        const auto e = static_cast<std::size_t>(nearest[i]);
        const glm::vec2 at(static_cast<float>(i % unx), static_cast<float>(i / unx));
        const glm::vec2 from(static_cast<float>(e % unx), static_cast<float>(e / unx));
        const glm::vec2 in = glm::length(from - at) > 0.0f ? glm::normalize(from - at) : glm::vec2(0.0f);
        const int steps = static_cast<int>(2.0f * wall);
        for (int k = 0; k <= steps; ++k) {
            const glm::vec2 q = glm::floor(from + in * (0.5f * static_cast<float>(k)) + 0.5f);
            if (q.x < 0.0f || q.y < 0.0f || q.x >= static_cast<float>(nx) || q.y >= static_cast<float>(ny)) break;
            if (water[static_cast<std::size_t>(q.y) * unx + static_cast<std::size_t>(q.x)]) {
                edge[i] = 1.0f;
                break;
            }
        }
    }
    const int r = std::max(1, static_cast<int>(std::lround(0.5f * kHorizonShoreWidth / cell)));
    for (int axis = 0; axis < 2; ++axis) {
        std::vector<float> out(n, 0.0f);
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                const auto i = static_cast<std::size_t>(y) * unx + static_cast<std::size_t>(x);
                if (!off[i]) continue;
                float sum = 0.0f, count = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    const int px = axis == 0 ? x + k : x, py = axis == 0 ? y : y + k;
                    if (px < 0 || py < 0 || px >= nx || py >= ny) continue;
                    const auto j = static_cast<std::size_t>(py) * unx + static_cast<std::size_t>(px);
                    if (!off[j]) continue;
                    sum += edge[j];
                    count += 1.0f;
                }
                out[i] = sum / count;
            }
        edge.swap(out);
    }
    return edge;
}

// The land's shape away from the zone: its hills' seed and heights.
struct Shape {
    std::uint32_t seed = 0;
    float near_amp = 0.0f; // the rolling hills'
    float far_amp = 0.0f;  // the mountains'
};

Shape shape_of(const HorizonLand& land) {
    Shape s;
    s.seed = static_cast<std::uint32_t>(land.seed * 10.0f);
    const float rugged = std::clamp(land.relief / 600.0f, 0.2f, 1.5f);
    s.near_amp = 200.0f + 300.0f * rugged;
    s.far_amp = 900.0f + 900.0f * rugged;
    return s;
}

// One vertex of the land.
struct Point {
    float h = 0.0f;
    bool tucked = false; // in a covered square or the empty square beside one, under the zone's terrain
    float sea = 0.0f;    // the sea past the zone's water edge (0..1)
    float peak = 0.0f;   // how far up the mountains (rock from half way)
    float level = 0.0f;  // the sea's surface here (the zone's water)
};

Point land_point(const HorizonField& g, const Shape& k, glm::vec2 p) {
    Point out;
    float extra = 0.0f;
    const std::size_t i = g.square(p, extra);
    if (extra <= 0.0f && std::isfinite(g.under[i])) {
        out.tucked = true;
        out.h = g.under[i];
        // On the zone's water or beside it, the sea's surface reaches it (horizon_mesh draws it past the zone's own).
        out.sea = g.covered(i) ? (g.wet[i] && (!g.dry[i] || g.water[i] >= g.zmax[i] - 2.0f) ? 1.0f : 0.0f) : g.sea[i];
        out.level = g.water_level;
        return out;
    }
    if (extra <= 0.0f && !g.open[i]) {
        out.h = g.edge[i] - 20.0f; // a pocket's lid
        return out;
    }
    const float d = (std::max(g.dist[i] - 0.5f, 0.0f)) * g.cell + extra;
    // Warped: the hills' lines bend and branch, not a value noise's grid of blobs; the mountains ridged and sharper the
    // higher they stand, their foothills rolling into them.
    const glm::vec2 warp(fbm(p / 2600.0f, 3, k.seed + 21u, false), fbm(p / 2600.0f, 3, k.seed + 22u, false));
    const glm::vec2 q = p + (warp - 0.5f) * 1800.0f;
    const float roll = smooth(0.15f, 0.85f, fbm(q / 1500.0f, 5, k.seed, false));
    const float ridge = fbm(q / 3600.0f, 6, k.seed + 7u, true);
    const float mass = smooth(0.3f, 0.75f, fbm(q / 7000.0f, 3, k.seed + 9u, false));
    const float mtn = std::pow(ridge, 1.5f) * (0.75f + 0.75f * mass);
    const float rise = k.near_amp * roll * smooth(kHorizonHillsFrom, kHorizonHillsFull, d) +
                       k.far_amp * mtn * smooth(kHorizonMountainsFrom, kHorizonMountainsFull, d);
    const float start = g.edge[i] - 20.0f;
    float h = glm::mix(start, g.land.ground + rise, smooth(0.0f, kHorizonBlend, d));
    // Past a shore the sea goes on to the land's reach: nothing says where the next coast is (docs/unknowns.md).
    // Where the shore ends, the coast wanders in headlands and bays rather than following the blur.
    float sea_here = g.sea[i];
    if (sea_here > 0.0f && sea_here < 1.0f)
        sea_here = smooth(0.0f, 1.0f, sea_here + 0.9f * (fbm(p / 700.0f, 4, k.seed + 31u, false) - 0.5f));
    if (sea_here > 0.0f) {
        const float floor = glm::mix(g.water_level - 20.0f, g.water_level - kHorizonSeaDepth, smooth(0.0f, 800.0f, d));
        h = glm::mix(h, floor, sea_here);
    }
    out.sea = sea_here;
    out.level = g.water_level;
    // The wear (horizon_terrain): none where the ground is held.
    if (g.terrain) h += g.terrain->sample(g.terrain->delta, p);
    out.h = h;
    out.peak = smooth(0.45f, 0.75f, (h - g.land.ground) / k.far_amp);
    return out;
}

} // namespace

HorizonLand horizon_land(std::span<const HorizonTriangle> triangles, float seed) {
    HorizonLand out;
    out.seed = seed;
    std::vector<std::pair<float, float>> xs, ys, zs;
    float level_area = 0.0f;
    for (const auto& t : triangles) {
        if (t.liquid) continue;
        const glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
        const float twice = glm::length(n);
        if (!(twice > 1e-4f) || !std::isfinite(twice)) continue;
        if (std::abs(n.z) / twice < kHorizonLevel) continue; // walls and slopes
        const float area = 0.5f * twice;
        const glm::vec3 mid = (t.a + t.b + t.c) / 3.0f;
        level_area += area;
        xs.emplace_back(mid.x, area);
        ys.emplace_back(mid.y, area);
        zs.emplace_back(mid.z, area);
    }
    if (level_area < kHorizonMinArea) return out;
    out.lo = {weighted_percentile(xs, level_area, kHorizonExtentCut),
              weighted_percentile(ys, level_area, kHorizonExtentCut)};
    out.hi = {weighted_percentile(xs, level_area, 1.0f - kHorizonExtentCut),
              weighted_percentile(ys, level_area, 1.0f - kHorizonExtentCut)};
    const float z10 = weighted_percentile(zs, level_area, 0.1f);
    out.ground = weighted_percentile(zs, level_area, 0.5f);
    const float z90 = weighted_percentile(zs, level_area, 0.9f);
    out.relief = std::clamp(z90 - z10, kHorizonMinRelief, kHorizonMaxRelief);
    out.valid = true;
    return out;
}

std::vector<glm::vec3> horizon_lawn_ground(const HorizonField& field, const HorizonMesh& mesh, float reach) {
    std::vector<glm::vec3> out;
    if (!field.valid()) return out;
    for (std::size_t i = 0; i + 2 < mesh.ground.size(); i += 3) {
        const std::uint32_t a = mesh.ground[i], b = mesh.ground[i + 1], c = mesh.ground[i + 2];
        if (a >= mesh.positions.size() || b >= mesh.positions.size() || c >= mesh.positions.size()) continue;
        const glm::vec3 mid = (mesh.positions[a] + mesh.positions[b] + mesh.positions[c]) / 3.0f;
        float extra = 0.0f;
        const std::size_t sq = field.square(glm::vec2(mid), extra);
        if (extra > 0.0f || !field.open[sq]) continue;
        if (field.dist[sq] * field.cell > reach + 0.5f * field.cell) continue;
        if (sq < field.sea.size() && field.sea[sq] > 0.5f) continue;
        if (!mesh.splat.empty()) {
            const glm::vec2 s = (mesh.splat[a] + mesh.splat[b] + mesh.splat[c]) / 3.0f;
            if (s.x > 0.5f || s.y > 0.5f) continue;
        }
        out.push_back(mesh.positions[a]);
        out.push_back(mesh.positions[b]);
        out.push_back(mesh.positions[c]);
    }
    return out;
}

bool horizon_open_below(std::string_view zone) {
    static constexpr std::string_view kZones[] = {"airplane", "poair"};
    for (std::string_view z : kZones) {
        if (z.size() != zone.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < z.size() && same; ++i) {
            const char c = zone[i];
            same = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == z[i];
        }
        if (same) return true;
    }
    return false;
}

float horizon_seed(std::string_view zone) {
    std::uint32_t h = 2166136261u; // FNV-1a
    for (char c : zone) {
        h ^= static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        h *= 16777619u;
    }
    return static_cast<float>(h % 10007u) * 0.1f; // 0 .. 1000.6
}

std::size_t HorizonField::square(glm::vec2 p, float& extra) const {
    const glm::vec2 hi = lo + glm::vec2(static_cast<float>(nx), static_cast<float>(ny)) * cell;
    const glm::vec2 q = glm::clamp(p, lo, hi - 0.01f);
    extra = glm::length(p - q);
    const int x = std::clamp(static_cast<int>((q.x - lo.x) / cell), 0, nx - 1);
    const int y = std::clamp(static_cast<int>((q.y - lo.y) / cell), 0, ny - 1);
    return at(x, y);
}

bool HorizonField::covers(glm::vec2 p) const {
    if (nx <= 0 || ny <= 0) return false;
    float extra = 0.0f;
    const std::size_t i = square(p, extra);
    return extra <= 0.0f && covered(i);
}

std::int64_t HorizonField::mesh_square(glm::vec2 p) const {
    if (xs.size() < 2 || ys.size() < 2) return -1;
    const auto c = std::upper_bound(xs.begin(), xs.end(), p.x) - xs.begin() - 1;
    const auto r = std::upper_bound(ys.begin(), ys.end(), p.y) - ys.begin() - 1;
    if (c < 0 || r < 0 || c + 1 >= static_cast<std::ptrdiff_t>(xs.size()) ||
        r + 1 >= static_cast<std::ptrdiff_t>(ys.size()))
        return -1;
    return static_cast<std::int64_t>(r) * static_cast<std::int64_t>(xs.size()) + c;
}

HorizonField horizon_field(std::span<const HorizonTriangle> triangles, const HorizonLand& land) {
    HorizonField g;
    g.land = land;
    if (!land.valid) return g;
    const float inf = std::numeric_limits<float>::infinity();

    // The grid: round the zone's terrain, strays left out, with a border of empty squares.
    const glm::vec2 keep_lo = land.lo - kHorizonStray, keep_hi = land.hi + kHorizonStray;
    auto kept = [&](const glm::vec3& p) {
        return p.x >= keep_lo.x && p.y >= keep_lo.y && p.x <= keep_hi.x && p.y <= keep_hi.y;
    };
    glm::vec2 lo(inf), hi(-inf);
    for (const auto& t : triangles)
        for (const glm::vec3* p : {&t.a, &t.b, &t.c})
            if (finite(*p) && kept(*p)) {
                lo = glm::min(lo, glm::vec2(*p));
                hi = glm::max(hi, glm::vec2(*p));
            }
    if (!(lo.x <= hi.x)) {
        g.land.valid = false;
        return g;
    }
    const float border = 4.0f * g.cell;
    g.lo = glm::floor((lo - border) / g.cell) * g.cell;
    g.nx = static_cast<int>(std::ceil((hi.x + border - g.lo.x) / g.cell));
    g.ny = static_cast<int>(std::ceil((hi.y + border - g.lo.y) / g.cell));
    const std::size_t cells = static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny);
    g.zmin.assign(cells, inf);
    g.zmax.assign(cells, -inf);
    g.water.assign(cells, -inf);
    g.dry.assign(cells, 0);
    g.wet.assign(cells, 0);
    g.wet_middle.assign(cells, 0);

    // Each triangle onto the squares: along its edges (a wall's top and foot), and at the squares' centres inside it.
    std::vector<float> water_levels;
    for (const auto& t : triangles) {
        if (!finite(t.a) || !finite(t.b) || !finite(t.c)) continue;
        if (!kept(t.a) && !kept(t.b) && !kept(t.c)) continue;
        const glm::vec3 corners[3] = {t.a, t.b, t.c};
        for (int e = 0; e < 3; ++e) {
            const glm::vec3 a = corners[e], b = corners[(e + 1) % 3];
            const float len = glm::length(glm::vec2(b - a));
            const int steps = std::max(1, static_cast<int>(std::ceil(len / (0.5f * g.cell))));
            for (int s = 0; s <= steps; ++s)
                mark(g, glm::mix(a, b, static_cast<float>(s) / static_cast<float>(steps)), t.liquid);
        }
        const glm::vec2 a(t.a), b(t.b), c(t.c);
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::abs(area) < 1.0f) continue;
        const glm::vec2 tlo = glm::min(a, glm::min(b, c)), thi = glm::max(a, glm::max(b, c));
        const int x0 = std::max(0, static_cast<int>(std::floor((tlo.x - g.lo.x) / g.cell)));
        const int y0 = std::max(0, static_cast<int>(std::floor((tlo.y - g.lo.y) / g.cell)));
        const int x1 = std::min(g.nx - 1, static_cast<int>(std::floor((thi.x - g.lo.x) / g.cell)));
        const int y1 = std::min(g.ny - 1, static_cast<int>(std::floor((thi.y - g.lo.y) / g.cell)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 p = g.lo + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) * g.cell;
                const float w1 = ((p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y)) / area;
                const float w2 = ((b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y)) / area;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                mark(g, glm::vec3(p, w0 * t.a.z + w1 * t.b.z + w2 * t.c.z), t.liquid);
                if (t.liquid) g.wet_middle[g.at(x, y)] = 1;
            }
        if (t.liquid) water_levels.push_back((t.a.z + t.b.z + t.c.z) / 3.0f);
    }
    g.has_water = !water_levels.empty();
    if (g.has_water) {
        std::nth_element(water_levels.begin(), water_levels.begin() + static_cast<std::ptrdiff_t>(water_levels.size() / 2),
                         water_levels.end());
        g.water_level = water_levels[water_levels.size() / 2];
    }

    // Every empty square is outside: past the zone's edges, and in a pocket the zone's terrain closes round (a gap
    // between its walls, a hole in it, which would show the void), where the land is a lid just under the rim. Open:
    // the empty squares the grid's border reaches without crossing terrain.
    std::vector<std::uint8_t> outside(cells, 0);
    for (std::size_t i = 0; i < cells; ++i) outside[i] = g.covered(i) ? 0 : 1;
    g.open.assign(cells, 0);
    std::vector<std::pair<int, int>> stack;
    auto reach = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= g.nx || y >= g.ny) return;
        const std::size_t i = g.at(x, y);
        if (g.open[i] || g.covered(i)) return;
        g.open[i] = 1;
        stack.emplace_back(x, y);
    };
    for (int x = 0; x < g.nx; ++x) {
        reach(x, 0);
        reach(x, g.ny - 1);
    }
    for (int y = 0; y < g.ny; ++y) {
        reach(0, y);
        reach(g.nx - 1, y);
    }
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        reach(x + 1, y);
        reach(x - 1, y);
        reach(x, y + 1);
        reach(x, y - 1);
    }

    // The distance from each outside square to the nearest covered one, carrying which one (two passes, each square
    // taking the nearest of its neighbours' nearest).
    std::vector<int> source(cells, -1);
    g.dist.assign(cells, inf);
    for (std::size_t i = 0; i < cells; ++i)
        if (g.covered(i)) {
            source[i] = static_cast<int>(i);
            g.dist[i] = 0.0f;
        }
    auto relax = [&](int x, int y, int nx, int ny) {
        if (nx < 0 || ny < 0 || nx >= g.nx || ny >= g.ny) return;
        const std::size_t i = g.at(x, y);
        if (!outside[i]) return;
        const int s = source[g.at(nx, ny)];
        if (s < 0) return;
        const int sx = s % g.nx, sy = s / g.nx;
        const float d = std::hypot(static_cast<float>(x - sx), static_cast<float>(y - sy));
        if (d < g.dist[i]) {
            g.dist[i] = d;
            source[i] = s;
        }
    };
    for (int y = 0; y < g.ny; ++y)
        for (int x = 0; x < g.nx; ++x) {
            relax(x, y, x - 1, y);
            relax(x, y, x - 1, y - 1);
            relax(x, y, x, y - 1);
            relax(x, y, x + 1, y - 1);
        }
    for (int y = g.ny - 1; y >= 0; --y)
        for (int x = g.nx - 1; x >= 0; --x) {
            relax(x, y, x + 1, y);
            relax(x, y, x + 1, y + 1);
            relax(x, y, x, y + 1);
            relax(x, y, x - 1, y + 1);
        }

    // The zone's edge's height, carried out from the nearest covered square and smoothed (its water's surface where
    // that is water).
    g.edge.assign(cells, land.ground);
    std::vector<std::uint8_t> water(cells, 0), off(cells, 0);
    for (std::size_t i = 0; i < cells; ++i) {
        water[i] = g.has_water && g.wet[i] && (!g.dry[i] || g.water[i] >= g.zmax[i] - 2.0f) ? 1 : 0;
        off[i] = outside[i];
    }
    for (std::size_t i = 0; i < cells; ++i) {
        const int s = source[i];
        if (s < 0) continue;
        const auto si = static_cast<std::size_t>(s);
        g.edge[i] = water[si] ? g.water[si] : g.zmax[si];
    }
    for (int pass = 0; pass < 2; ++pass)
        for (int axis = 0; axis < 2; ++axis) {
            const std::vector<float> edge = g.edge;
            constexpr int kRadius = 2;
            for (int y = 0; y < g.ny; ++y)
                for (int x = 0; x < g.nx; ++x) {
                    const std::size_t i = g.at(x, y);
                    if (!outside[i]) continue;
                    float se = 0.0f, n = 0.0f;
                    for (int k = -kRadius; k <= kRadius; ++k) {
                        const int px = axis == 0 ? x + k : x, py = axis == 0 ? y : y + k;
                        if (px < 0 || py < 0 || px >= g.nx || py >= g.ny) continue;
                        const std::size_t j = g.at(px, py);
                        if (source[j] < 0) continue;
                        se += edge[j];
                        n += 1.0f;
                    }
                    g.edge[i] = se / n;
                }
        }
    // Whether the zone's edge is a shore there (shore_share), so the sea goes on past a quay along the edge and not
    // past a river's mouth.
    g.sea = shore_share(g.nx, g.ny, g.cell, source, off, water);

    // How low the land is tucked: under a covered square's terrain; beside one, just under the lowest of them (a
    // wall's foot, the open ground at a zone line).
    g.under.assign(cells, inf);
    for (int y = 0; y < g.ny; ++y)
        for (int x = 0; x < g.nx; ++x) {
            const std::size_t i = g.at(x, y);
            const float own = g.dry[i] ? g.zmin[i] : g.water[i];
            if (g.covered(i)) {
                g.under[i] = own - kHorizonTuck;
                continue;
            }
            if (!outside[i]) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int px = x + dx, py = y + dy;
                    if (px < 0 || py < 0 || px >= g.nx || py >= g.ny) continue;
                    const std::size_t j = g.at(px, py);
                    if (!g.covered(j)) continue;
                    // Beside the zone's water alone, as deep as under it: the sea goes on over it.
                    g.under[i] = std::min(g.under[i], g.dry[j] ? g.zmin[j] - 5.0f : g.water[j] - kHorizonTuck);
                }
        }

    // The mesh's lines: the grid's squares over it and kHorizonFine past it (bigger squares for a big zone), then
    // growing.
    const int fine_cells = static_cast<int>(std::ceil(kHorizonFine / g.cell));
    const int across = std::max(g.nx, g.ny) + 2 * fine_cells;
    const int scale = std::max(1, (across + kHorizonMaxFine - 1) / kHorizonMaxFine);
    const float step = g.cell * static_cast<float>(scale);
    const float fine = std::ceil(kHorizonFine / step) * step;
    const glm::vec2 ghi = g.lo + glm::vec2(static_cast<float>(g.nx), static_cast<float>(g.ny)) * g.cell;
    g.xs = axis_lines(g.lo.x - fine, ghi.x + fine, step, kHorizonReach - fine);
    g.ys = axis_lines(g.lo.y - fine, ghi.y + fine, step, kHorizonReach - fine);
    return g;
}

float HorizonTerrain::sample(const std::vector<float>& layer, glm::vec2 p) const {
    if (!valid() || layer.size() != delta.size()) return 0.0f;
    const glm::vec2 q = (p - lo) / cell;
    if (!(q.x >= 0.0f && q.y >= 0.0f && q.x <= static_cast<float>(nx - 1) && q.y <= static_cast<float>(ny - 1)))
        return 0.0f;
    const int x0 = std::min(static_cast<int>(q.x), nx - 2), y0 = std::min(static_cast<int>(q.y), ny - 2);
    const float fx = q.x - static_cast<float>(x0), fy = q.y - static_cast<float>(y0);
    auto at = [&](int x, int y) {
        return layer[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x)];
    };
    return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}

HorizonTerrain horizon_terrain(const HorizonField& g) {
    HorizonTerrain t;
    if (!g.valid()) return t;
    const Shape k = shape_of(g.land);
    const glm::vec2 lo(g.xs.front(), g.ys.front()), hi(g.xs.back(), g.ys.back());
    const float side = std::max(hi.x - lo.x, hi.y - lo.y);
    t.cell = std::max(kHorizonCell, side / static_cast<float>(kHorizonTerrainMax));
    t.lo = lo;
    t.nx = static_cast<int>(std::ceil((hi.x - lo.x) / t.cell)) + 1;
    t.ny = static_cast<int>(std::ceil((hi.y - lo.y) / t.cell)) + 1;
    const int nx = t.nx, ny = t.ny;
    const std::size_t n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    auto at = [&](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x); };
    // The land as it stands (in squares of height, so the slopes are the wear's own), and how free each node is.
    std::vector<float> h(n), h0(n), free(n, 0.0f), visits(n, 0.0f);
    // Each node on its own, so the rows are shared out among a few threads (the result is the same however many).
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < nx; ++x) {
                const glm::vec2 p = lo + glm::vec2(static_cast<float>(x), static_cast<float>(y)) * t.cell;
                const Point pt = land_point(g, k, p);
                float extra = 0.0f;
                const std::size_t i = g.square(p, extra);
                const float d = std::max(g.dist[i] - 0.5f, 0.0f) * g.cell + extra;
                const bool held = pt.tucked || (extra <= 0.0f && !g.open[i]);
                h[at(x, y)] = h0[at(x, y)] = pt.h / t.cell;
                if (!held)
                    free[at(x, y)] = smooth(200.0f, kHorizonTerrainFree, d) * (1.0f - smooth(0.6f, 0.95f, pt.sea));
            }
    };
    {
        const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
        std::vector<std::thread> pool;
        const int per = (ny + threads - 1) / threads;
        for (int k0 = per; k0 < ny; k0 += per) pool.emplace_back(rows, k0, std::min(ny, k0 + per));
        rows(0, std::min(ny, per));
        for (auto& th : pool) th.join();
    }
    auto height = [&](glm::vec2 q, glm::vec2* grad) {
        const int x0 = std::clamp(static_cast<int>(q.x), 0, nx - 2), y0 = std::clamp(static_cast<int>(q.y), 0, ny - 2);
        const float fx = q.x - static_cast<float>(x0), fy = q.y - static_cast<float>(y0);
        const float a = h[at(x0, y0)], b = h[at(x0 + 1, y0)], c = h[at(x0, y0 + 1)], d = h[at(x0 + 1, y0 + 1)];
        if (grad) *grad = glm::vec2((b - a) * (1.0f - fy) + (d - c) * fy, (c - a) * (1.0f - fx) + (d - b) * fx);
        return a * (1.0f - fx) * (1.0f - fy) + b * fx * (1.0f - fy) + c * (1.0f - fx) * fy + d * fx * fy;
    };
    // Droplets (Hans Theobald Beyer, "Implementation of a method for hydraulic erosion", 2015): each runs downhill
    // with inertia, takes up ground while it can carry more than it holds and drops it where it slows or climbs.
    constexpr float kInertia = 0.08f, kCapacity = 3.0f, kMinCapacity = 0.01f, kErode = 0.25f, kDeposit = 0.3f;
    constexpr float kEvaporate = 0.02f, kGravity = 4.0f;
    constexpr int kSteps = kHorizonDropletSteps, kRadius = 2;
    std::uint32_t rng = 0x9e3779b9u ^ (k.seed * 2654435761u);
    auto next = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return static_cast<float>(rng & 0xffffffu) / 16777216.0f;
    };
    // A node's wear is kept within kMaxCut under and kMaxFill over where it stood: a pit the droplets keep running into
    // otherwise deepens without end. Returns what was moved.
    const float kMaxCut = kHorizonTerrainMaxCut / t.cell, kMaxFill = kHorizonTerrainMaxFill / t.cell; // squares of height
    auto wear = [&](std::size_t i, float amount) {
        const float before = h[i];
        h[i] = std::clamp(h[i] + amount, h0[i] - kMaxCut, h0[i] + kMaxFill);
        return h[i] - before;
    };
    // The droplet's brush: the nodes within kRadius, weighted by how near (their weights add up to 1).
    struct Brush {
        int dx, dy;
        float w;
    };
    std::vector<Brush> brush;
    {
        float weights = 0.0f;
        for (int by = -kRadius; by <= kRadius; ++by)
            for (int bx = -kRadius; bx <= kRadius; ++bx) {
                const float r = std::sqrt(static_cast<float>(bx * bx + by * by));
                if (r < static_cast<float>(kRadius)) {
                    brush.push_back({bx, by, static_cast<float>(kRadius) - r});
                    weights += static_cast<float>(kRadius) - r;
                }
            }
        for (Brush& b : brush) b.w /= weights;
    }
    const auto droplets = static_cast<std::size_t>(static_cast<float>(n) * kHorizonDroplets);
    for (std::size_t drop = 0; drop < droplets; ++drop) {
        glm::vec2 pos(next() * static_cast<float>(nx - 2), next() * static_cast<float>(ny - 2));
        if (free[at(static_cast<int>(pos.x), static_cast<int>(pos.y))] <= 0.0f) continue;
        glm::vec2 dir(0.0f);
        float speed = 1.0f, water = 1.0f, sediment = 0.0f;
        for (int step = 0; step < kSteps; ++step) {
            const int cx = static_cast<int>(pos.x), cy = static_cast<int>(pos.y);
            glm::vec2 grad;
            const float here = height(pos, &grad);
            dir = dir * kInertia - grad * (1.0f - kInertia);
            const float len = glm::length(dir);
            if (!(len > 1e-6f)) break;
            dir /= len;
            const glm::vec2 was = pos;
            pos += dir;
            if (!(pos.x >= 0.0f && pos.y >= 0.0f && pos.x < static_cast<float>(nx - 1) && pos.y < static_cast<float>(ny - 1)))
                break;
            const float dh = height(pos, nullptr) - here;
            const float capacity = std::max(-dh * speed * water * kCapacity, kMinCapacity);
            const glm::vec2 off = was - glm::vec2(static_cast<float>(cx), static_cast<float>(cy));
            visits[at(cx, cy)] += water;
            if (sediment > capacity || dh > 0.0f) {
                const float drop_amount = dh > 0.0f ? std::min(dh, sediment) : (sediment - capacity) * kDeposit;
                sediment -= drop_amount;
                const float w[4] = {(1 - off.x) * (1 - off.y), off.x * (1 - off.y), (1 - off.x) * off.y, off.x * off.y};
                const std::size_t c[4] = {at(cx, cy), at(cx + 1, cy), at(cx, cy + 1), at(cx + 1, cy + 1)};
                for (int j = 0; j < 4; ++j) wear(c[j], drop_amount * w[j] * free[c[j]]);
            } else {
                const float take = std::min((capacity - sediment) * kErode, -dh);
                for (const Brush& b : brush) {
                    const int x = cx + b.dx, y = cy + b.dy;
                    if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
                    const std::size_t j = at(x, y);
                    if (free[j] <= 0.0f) continue;
                    sediment -= wear(j, -take * b.w * free[j]);
                }
            }
            speed = std::sqrt(std::max(speed * speed - dh * kGravity, 0.0f)); // faster downhill
            water *= 1.0f - kEvaporate;
        }
    }
    // Slumping: ground steeper than kTalus between neighbours slides down, half the excess a pass.
    constexpr float kTalus = 0.9f;
    for (int pass = 0; pass < 6; ++pass)
        for (int y = 0; y + 1 < ny; ++y)
            for (int x = 0; x + 1 < nx; ++x)
                for (int dirn = 0; dirn < 2; ++dirn) {
                    const std::size_t a = at(x, y), b = dirn == 0 ? at(x + 1, y) : at(x, y + 1);
                    const float diff = h[a] - h[b];
                    if (std::abs(diff) <= kTalus) continue;
                    const std::size_t from = diff > 0.0f ? a : b, to = diff > 0.0f ? b : a;
                    const float move = 0.25f * (std::abs(diff) - kTalus) * std::min(free[from], free[to]);
                    h[from] -= move;
                    h[to] += move;
                }
    t.delta.resize(n);
    t.wet.resize(n);
    // On the cache's steps (write_horizon_terrain): an eighth of a unit, a 255th of the water.
    auto step_delta = [](float v) {
        return std::round(std::clamp(v, -kHorizonTerrainMaxCut, kHorizonTerrainMaxFill) * 8.0f) / 8.0f;
    };
    double total = 0.0;
    std::size_t counted = 0;
    for (std::size_t i = 0; i < n; ++i) {
        t.delta[i] = step_delta((h[i] - h0[i]) * t.cell * free[i]);
        if (visits[i] > 0.0f) {
            total += visits[i];
            ++counted;
        }
    }
    const float mean = counted ? static_cast<float>(total / static_cast<double>(counted)) : 1.0f;
    {
        float lo_d = 0.0f, hi_d = 0.0f;
        for (float v : t.delta) {
            lo_d = std::min(lo_d, v);
            hi_d = std::max(hi_d, v);
        }
        t.worn_lo = lo_d;
        t.worn_hi = hi_d;
    }
    // Wet where more water ran than most places, softened over a square each way.
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            float sum = 0.0f;
            int count = 0;
            for (int by = -1; by <= 1; ++by)
                for (int bx = -1; bx <= 1; ++bx) {
                    const int px = x + bx, py = y + by;
                    if (px < 0 || py < 0 || px >= nx || py >= ny) continue;
                    sum += visits[at(px, py)];
                    ++count;
                }
            t.wet[at(x, y)] = std::round((1.0f - std::exp(-(sum / static_cast<float>(count)) / (3.0f * mean))) * 255.0f) / 255.0f;
        }
    return t;
}

float horizon_height(const HorizonField& field, glm::vec2 p) {
    if (!field.valid()) return 0.0f;
    return land_point(field, shape_of(field.land), p).h;
}

HorizonMesh horizon_mesh(std::span<const HorizonTriangle> triangles, const HorizonLand& land) {
    return horizon_mesh(horizon_field(triangles, land));
}

HorizonMesh horizon_mesh(const HorizonField& g) {
    HorizonMesh mesh;
    if (!g.valid()) return mesh;
    mesh.water_level = g.water_level;
    const std::vector<float>& xs = g.xs;
    const std::vector<float>& ys = g.ys;
    const std::size_t cols = xs.size(), rows = ys.size();
    const Shape k = shape_of(g.land);

    std::vector<Point> points(cols * rows);
    mesh.positions.resize(cols * rows);
    for (std::size_t r = 0; r < rows; ++r)
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t v = r * cols + c;
            const glm::vec2 p(xs[c], ys[r]);
            points[v] = land_point(g, k, p);
            mesh.positions[v] = glm::vec3(p, points[v].h);
        }

    // The normals from the neighbouring vertices; the light left by woods and valleys.
    mesh.normals.resize(mesh.positions.size(), glm::vec3(0.0f, 0.0f, 1.0f));
    mesh.shade.resize(mesh.positions.size(), 1.0f);
    mesh.splat.resize(mesh.positions.size(), glm::vec2(0.0f));
    for (std::size_t r = 0; r < rows; ++r)
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t v = r * cols + c;
            const std::size_t c0 = c > 0 ? c - 1 : c, c1 = c + 1 < cols ? c + 1 : c;
            const std::size_t r0 = r > 0 ? r - 1 : r, r1 = r + 1 < rows ? r + 1 : r;
            const glm::vec3 dx = mesh.positions[r * cols + c1] - mesh.positions[r * cols + c0];
            const glm::vec3 dy = mesh.positions[r1 * cols + c] - mesh.positions[r0 * cols + c];
            const glm::vec3 n = glm::cross(dx, dy);
            if (glm::dot(n, n) > 1e-8f && n.z > 0.0f) mesh.normals[v] = glm::normalize(n);
            if (points[v].tucked) continue;
            const glm::vec2 p(mesh.positions[v]);
            const float woods = smooth(0.52f, 0.64f, fbm(p / 650.0f, 3, k.seed + 13u, false)) *
                                smooth(0.75f, 0.9f, mesh.normals[v].z) * (1.0f - points[v].peak);
            const float valley = smooth(-100.0f, 400.0f, mesh.positions[v].z - g.land.ground);
            // Broad patches of lighter and darker ground, so the texture's repeat does not show across the plains.
            const float patches = 0.88f + 0.24f * fbm(p / 380.0f, 3, k.seed + 17u, false);
            {
                // Rock on the steep and the high, sand along the water; the runnels and valleys the water wore
                // darker and lusher.
                const float wet = g.terrain ? g.terrain->sample(g.terrain->wet, p) : 0.0f;
                const float rock = std::max(smooth(0.8f, 0.62f, mesh.normals[v].z), smooth(0.55f, 0.85f, points[v].peak));
                // Sand only by the sea (a beach a few units above its surface, the shallows under it).
                const float sand = points[v].sea > 0.02f
                                       ? smooth(points[v].level + 14.0f, points[v].level + 3.0f, mesh.positions[v].z) *
                                             (1.0f - rock)
                                       : 0.0f;
                mesh.splat[v] = glm::vec2(rock, sand);
                const float woods_here = std::clamp(0.6f * woods + 0.8f * wet, 0.0f, 1.0f) * (1.0f - rock);
                mesh.shade[v] = std::min((1.0f - 0.32f * woods_here) * glm::mix(0.88f, 1.0f, valley) * patches, 1.0f);
            }
            // Under the sea, darker with the depth: the water map that darkens a zone's own sea floor does not reach
            // this far (ZoneRenderer: the land's draws are left out of it), and a sunlit floor under clear water
            // reads as dry land.
            if (points[v].sea >= 0.5f)
                mesh.shade[v] *= glm::mix(1.0f, kHorizonSeaFloorShade,
                                          smooth(0.0f, kHorizonSeaFloorDark, points[v].level - mesh.positions[v].z));
        }

    // The squares: two triangles each along the shorter diagonal in height, those under the zone too (seen through the
    // back of its walls, which have only their inner faces). Where the sea is, its surface too.
    std::vector<std::uint32_t> water_vertex(mesh.positions.size(), std::numeric_limits<std::uint32_t>::max());
    const bool any_sea = std::any_of(points.begin(), points.end(), [](const Point& pt) { return pt.sea >= 0.5f; });
    auto surface = [&](std::size_t v) {
        if (water_vertex[v] == std::numeric_limits<std::uint32_t>::max()) {
            water_vertex[v] = static_cast<std::uint32_t>(mesh.positions.size());
            mesh.positions.push_back(glm::vec3(glm::vec2(mesh.positions[v]), points[v].level));
            mesh.normals.emplace_back(0.0f, 0.0f, 1.0f);
            mesh.shade.push_back(1.0f);
            if (!mesh.splat.empty()) mesh.splat.emplace_back(0.0f);
        }
        return water_vertex[v];
    };
    auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        // One draw, the rock blended in by the vertices' splat (zone.frag kLandSplat).
        mesh.ground.insert(mesh.ground.end(), {a, b, c});
    };
    for (std::size_t r = 0; r + 1 < rows; ++r)
        for (std::size_t c = 0; c + 1 < cols; ++c) {
            const std::size_t q[4] = {r * cols + c, r * cols + c + 1, (r + 1) * cols + c + 1, (r + 1) * cols + c};
            const auto a = static_cast<std::uint32_t>(q[0]), b = static_cast<std::uint32_t>(q[1]);
            const auto cc = static_cast<std::uint32_t>(q[2]), d = static_cast<std::uint32_t>(q[3]);
            const float z[4] = {mesh.positions[q[0]].z, mesh.positions[q[1]].z, mesh.positions[q[2]].z,
                                mesh.positions[q[3]].z};
            if (std::abs(z[0] - z[2]) <= std::abs(z[1] - z[3])) {
                add(a, b, cc);
                add(a, cc, d);
            } else {
                add(a, b, d);
                add(b, cc, d);
            }
            if (!any_sea) continue;
            bool open_sea = true, below = false, some_sea = false;
            for (std::size_t corner : q) {
                open_sea &= points[corner].sea >= 0.5f;
                some_sea |= points[corner].sea >= 0.5f;
                below |= mesh.positions[corner].z < points[corner].level;
            }
            // The water over any square the sea reaches, its edge where the land rises through it (a coast following
            // the ground, not the squares).
            {
                open_sea = some_sea;
                // Not where the corners' seas stand at different levels (a neighbour's beside the zone's): a sloped
                // sheet of water.
                float lo_level = points[q[0]].level, hi_level = lo_level;
                for (std::size_t corner : q) {
                    lo_level = std::min(lo_level, points[corner].level);
                    hi_level = std::max(hi_level, points[corner].level);
                }
                if (hi_level - lo_level > 2.0f) open_sea = false;
            }
            if (!open_sea || !below) continue;
            // Not over the zone's own terrain or water (which draws its own), up to the square beside it, and over a
            // square its water only touches along a side.
            float extra = 0.0f;
            const std::size_t middle = g.square(0.25f * (glm::vec2(mesh.positions[q[0]]) + glm::vec2(mesh.positions[q[1]]) +
                                                         glm::vec2(mesh.positions[q[2]]) + glm::vec2(mesh.positions[q[3]])),
                                                extra);
            if (extra <= 0.0f && (g.dry[middle] || g.wet_middle[middle])) continue;
            const std::uint32_t wa = surface(q[0]), wb = surface(q[1]), wc = surface(q[2]), wd = surface(q[3]);
            mesh.water.insert(mesh.water.end(), {wa, wb, wc, wa, wc, wd});
        }
    return mesh;
}

std::optional<float> horizon_surface(const HorizonField& g, const HorizonMesh& mesh, glm::vec2 p, glm::vec3* normal) {
    const std::size_t cols = g.xs.size(), rows = g.ys.size();
    if (cols < 2 || rows < 2 || mesh.positions.size() < cols * rows) return std::nullopt;
    if (!(p.x >= g.xs.front() && p.x <= g.xs.back() && p.y >= g.ys.front() && p.y <= g.ys.back())) return std::nullopt;
    auto line = [](const std::vector<float>& v, float x) {
        const auto it = std::upper_bound(v.begin(), v.end(), x);
        const auto i = static_cast<std::size_t>(std::max<std::ptrdiff_t>(it - v.begin() - 1, 0));
        return std::min(i, v.size() - 2);
    };
    const std::size_t c = line(g.xs, p.x), r = line(g.ys, p.y);
    const glm::vec3 q0 = mesh.positions[r * cols + c], q1 = mesh.positions[r * cols + c + 1],
                    q2 = mesh.positions[(r + 1) * cols + c + 1], q3 = mesh.positions[(r + 1) * cols + c];
    const float fx = (p.x - q0.x) / (q1.x - q0.x), fy = (p.y - q0.y) / (q3.y - q0.y);
    // The triangle horizon_mesh splits the square into there (along the diagonal shorter in height).
    glm::vec3 a, b, cc;
    if (std::abs(q0.z - q2.z) <= std::abs(q1.z - q3.z)) {
        if (fx >= fy) a = q0, b = q1, cc = q2;
        else a = q0, b = q2, cc = q3;
    } else {
        if (fx + fy <= 1.0f) a = q0, b = q1, cc = q3;
        else a = q1, b = q2, cc = q3;
    }
    const glm::vec3 n = glm::cross(b - a, cc - a);
    if (!(std::abs(n.z) > 1e-6f)) return std::nullopt;
    if (normal) *normal = glm::normalize(n.z < 0.0f ? -n : n);
    // The plane through the three: n . (x - a) = 0.
    return a.z - (n.x * (p.x - a.x) + n.y * (p.y - a.y)) / n.z;
}

std::vector<HorizonTree> horizon_trees(const HorizonField& g, const HorizonMesh& mesh, float woods, float open) {
    std::vector<HorizonTree> out;
    if (!g.valid() || !(woods > 0.0f) || !(open >= woods)) return out;
    const Shape k = shape_of(g.land);
    // The spots: one a square of `woods` each way, jittered within it, over the land out to the trees' reach.
    const glm::vec2 lo(std::max(g.xs.front(), g.lo.x - kHorizonTreeReach),
                       std::max(g.ys.front(), g.lo.y - kHorizonTreeReach));
    const glm::vec2 hi(std::min(g.xs.back(), g.lo.x + static_cast<float>(g.nx) * g.cell + kHorizonTreeReach),
                       std::min(g.ys.back(), g.lo.y + static_cast<float>(g.ny) * g.cell + kHorizonTreeReach));
    if (!(hi.x > lo.x) || !(hi.y > lo.y)) return out;
    const auto nx = static_cast<std::int32_t>(std::ceil((hi.x - lo.x) / woods));
    const auto ny = static_cast<std::int32_t>(std::ceil((hi.y - lo.y) / woods));
    const float open_share = (woods / open) * (woods / open);
    struct Found {
        float d;
        HorizonTree tree;
    };
    std::vector<Found> found;
    for (std::int32_t y = 0; y < ny; ++y)
        for (std::int32_t x = 0; x < nx; ++x) {
            auto rnd = [&](std::uint32_t salt) { return lattice(x, y, k.seed * 7919u + salt); };
            const glm::vec2 p = lo + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) +
                                      glm::vec2(rnd(1u), rnd(2u))) * woods;
            float extra = 0.0f;
            const std::size_t i = g.square(p, extra);
            if (extra <= 0.0f && (g.covered(i) || !g.open[i] || std::isfinite(g.under[i]))) continue;
            const float d = std::max(g.dist[i] - 0.5f, 0.0f) * g.cell + extra;
            if (d < kHorizonTreeFrom || d > kHorizonTreeReach) continue;
            // The woods as horizon_mesh shades them (with no slope or peak yet: those are tested below). Tested first,
            // as it is cheap and most spots go on it.
            float in_woods = smooth(0.52f, 0.64f, fbm(p / 650.0f, 3, k.seed + 13u, false));
            // The woods follow the water, thick in the valleys and runnels it wore.
            if (g.terrain) in_woods = std::clamp(0.6f * in_woods + 1.2f * g.terrain->sample(g.terrain->wet, p), 0.0f, 1.0f);
            float keep = in_woods + (1.0f - in_woods) * open_share;
            if (d > kHorizonTreeThin) keep *= kHorizonTreeThin / d;
            if (rnd(3u) >= keep) continue;
            const Point pt = land_point(g, k, p);
            if (pt.tucked || pt.sea > 0.05f || pt.peak > 0.2f) continue;
            glm::vec3 n(0.0f, 0.0f, 1.0f);
            const std::optional<float> z = horizon_surface(g, mesh, p, &n);
            if (!z || n.z < kHorizonTreeSlope) continue;
            // Not standing in the water, where the land dips under the zone's own surface.
            if (pt.sea > 0.0f && *z < pt.level + 4.0f) continue;
            HorizonTree t;
            t.foot = glm::vec3(p, *z - kHorizonTreeSink);
            t.yaw = rnd(4u) * 6.2831853f;
            t.scale = 0.8f + 0.4f * rnd(5u);
            t.pick = rnd(6u);
            found.push_back({d, t});
        }
    if (found.size() > kHorizonTreeMax) {
        std::nth_element(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(kHorizonTreeMax), found.end(),
                         [](const Found& a, const Found& b) { return a.d < b.d; });
        found.resize(kHorizonTreeMax);
    }
    out.reserve(found.size());
    for (const Found& f : found) out.push_back(f.tree);
    return out;
}


} // namespace hailstone::render

namespace hailstone::render {

namespace {
constexpr std::uint32_t kTerrainMagic = 0x31525448u; // "HTR1"
// 3: hills and mountains further out (kHorizonHillsFrom..kHorizonMountainsFull); 2: from the zone alone (1: shaped
// by the neighbour zones' ground too); warped ridged hills, droplet wear
// (kHorizonDroplets, kHorizonDropletSteps) and slumping. Raise it whenever the generator changes what it makes from
// the same ground.
constexpr std::uint32_t kTerrainVersion = 3;

// FNV-1a over the bytes of what the terrain is worn from.
struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    // NaN as one value, so that a NaN's bits do not change the key.
    void f32(float v) { u32(std::isnan(v) ? 0x7fc00000u : std::bit_cast<std::uint32_t>(v)); }
    void floats(const std::vector<float>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (float f : v) f32(f);
    }
    void bytes8(const std::vector<std::uint8_t>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (std::uint8_t b : v) byte(b);
    }
};
} // namespace

std::uint64_t horizon_terrain_key(const HorizonField& g) {
    Fnv f;
    f.u32(kTerrainVersion);
    f.f32(g.land.seed);
    f.f32(g.land.ground);
    f.f32(g.land.relief);
    f.f32(g.lo.x);
    f.f32(g.lo.y);
    f.f32(g.cell);
    f.u32(static_cast<std::uint32_t>(g.nx));
    f.u32(static_cast<std::uint32_t>(g.ny));
    for (const auto* layer : {&g.zmin, &g.zmax, &g.water, &g.dist, &g.edge, &g.sea, &g.under, &g.xs, &g.ys})
        f.floats(*layer);
    f.bytes8(g.dry);
    f.bytes8(g.wet);
    f.bytes8(g.open);
    f.f32(g.water_level);
    return f.h;
}

std::vector<std::byte> write_horizon_terrain(const HorizonTerrain& t, std::uint64_t key) {
    ByteWriter w;
    w.le(kTerrainMagic);
    w.le(kTerrainVersion);
    w.le(key);
    w.f32(t.lo.x);
    w.f32(t.lo.y);
    w.f32(t.cell);
    w.f32(t.worn_lo);
    w.f32(t.worn_hi);
    const bool ok = t.valid();
    w.le(static_cast<std::uint32_t>(ok ? t.nx : 0));
    w.le(static_cast<std::uint32_t>(ok ? t.ny : 0));
    if (ok) {
        // An eighth of a unit (the generator's own steps), and the water in 255ths.
        for (float d : t.delta) w.le(static_cast<std::int16_t>(std::lround(std::clamp(d, -4000.0f, 4000.0f) * 8.0f)));
        for (float v : t.wet) w.le(static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
    }
    return w.take();
}

std::optional<HorizonTerrain> read_horizon_terrain(std::span<const std::byte> data, std::uint64_t key) {
    try {
        ByteReader r(data);
        if (r.le<std::uint32_t>() != kTerrainMagic || r.le<std::uint32_t>() != kTerrainVersion) return std::nullopt;
        if (r.le<std::uint64_t>() != key) return std::nullopt;
        HorizonTerrain t;
        t.lo.x = r.f32();
        t.lo.y = r.f32();
        t.cell = r.f32();
        t.worn_lo = r.f32();
        t.worn_hi = r.f32();
        const auto nx = r.le<std::uint32_t>(), ny = r.le<std::uint32_t>();
        if (!std::isfinite(t.lo.x) || !std::isfinite(t.lo.y) || !(t.cell >= 1.0f && t.cell <= 4096.0f) ||
            !std::isfinite(t.worn_lo) || !std::isfinite(t.worn_hi) || nx < 2 || ny < 2 ||
            nx > static_cast<std::uint32_t>(kHorizonTerrainMax + 1) || ny > static_cast<std::uint32_t>(kHorizonTerrainMax + 1))
            return std::nullopt;
        const std::size_t n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
        if (r.remaining() != n * 3) return std::nullopt;
        t.nx = static_cast<int>(nx);
        t.ny = static_cast<int>(ny);
        t.delta.resize(n);
        t.wet.resize(n);
        for (float& d : t.delta) d = static_cast<float>(r.le<std::int16_t>()) / 8.0f;
        for (float& v : t.wet) v = static_cast<float>(r.le<std::uint8_t>()) / 255.0f;
        return t;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

} // namespace hailstone::render
