#include "render/horizon.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

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

// The zone's terrain laid on squares: what covers each, how high.
struct Grid {
    glm::vec2 lo{0.0f};
    float cell = kHorizonCell;
    int nx = 0, ny = 0;
    std::vector<float> zmin, zmax, water;
    std::vector<std::uint8_t> land, wet;
    std::vector<std::uint8_t> outside; // reached from the border without crossing terrain
    std::vector<float> dist;           // to the nearest covered square, in squares (outside ones)
    std::vector<float> edge, sea;      // the zone's edge's height there, and whether it is water (0..1)
    std::vector<float> under;          // a covered square's, or one beside it: how high the land is tucked

    std::size_t at(int x, int y) const { return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) +
                                                static_cast<std::size_t>(x); }
    bool covered(std::size_t i) const { return land[i] || wet[i]; }
    // The square holding `p`, clamped to the grid; `extra` the distance from p to the grid when it lies outside.
    std::size_t square(glm::vec2 p, float& extra) const {
        const glm::vec2 hi = lo + glm::vec2(static_cast<float>(nx), static_cast<float>(ny)) * cell;
        const glm::vec2 q = glm::clamp(p, lo, hi - 0.01f);
        extra = glm::length(p - q);
        const int x = std::clamp(static_cast<int>((q.x - lo.x) / cell), 0, nx - 1);
        const int y = std::clamp(static_cast<int>((q.y - lo.y) / cell), 0, ny - 1);
        return at(x, y);
    }
    void mark(const glm::vec3& p, bool liquid) {
        const auto fx = std::floor((p.x - lo.x) / cell), fy = std::floor((p.y - lo.y) / cell);
        if (!(fx >= 0.0f && fy >= 0.0f && fx < static_cast<float>(nx) && fy < static_cast<float>(ny))) return;
        const std::size_t i = at(static_cast<int>(fx), static_cast<int>(fy));
        if (liquid) {
            wet[i] = 1;
            water[i] = std::max(water[i], p.z);
        } else {
            land[i] = 1;
            zmin[i] = std::min(zmin[i], p.z);
            zmax[i] = std::max(zmax[i], p.z);
        }
    }
};

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The mesh's lines along one axis: kHorizonCell x `scale` apart over the grid (lo..hi), then wider by kHorizonGrowth a
// step out to kHorizonReach past it.
std::vector<float> axis_lines(float lo, float hi, float step) {
    std::vector<float> inner;
    const int n = std::max(1, static_cast<int>(std::ceil((hi - lo) / step)));
    for (int k = 0; k <= n; ++k) inner.push_back(lo + static_cast<float>(k) * step);
    std::vector<float> left, right;
    float s = step;
    for (float x = inner.front(); x > inner.front() - kHorizonReach;) {
        s *= kHorizonGrowth;
        x -= s;
        left.push_back(x);
    }
    s = step;
    for (float x = inner.back(); x < inner.back() + kHorizonReach;) {
        s *= kHorizonGrowth;
        x += s;
        right.push_back(x);
    }
    std::vector<float> out(left.rbegin(), left.rend());
    out.insert(out.end(), inner.begin(), inner.end());
    out.insert(out.end(), right.begin(), right.end());
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

float horizon_seed(std::string_view zone) {
    std::uint32_t h = 2166136261u; // FNV-1a
    for (char c : zone) {
        h ^= static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        h *= 16777619u;
    }
    return static_cast<float>(h % 10007u) * 0.1f; // 0 .. 1000.6
}

std::int64_t horizon_cell(glm::vec2 p) {
    const auto x = static_cast<std::int64_t>(std::floor(p.x / kHorizonFootprintCell));
    const auto y = static_cast<std::int64_t>(std::floor(p.y / kHorizonFootprintCell));
    return (x << 32) ^ (y & 0xffffffffLL);
}

std::vector<std::int64_t> horizon_footprint(std::span<const HorizonTriangle> triangles) {
    constexpr float cell = kHorizonFootprintCell;
    constexpr float kFar = 1e6f; // past any zone: a broken vertex
    std::vector<std::int64_t> out;
    auto mark = [&](glm::vec2 p) { out.push_back(horizon_cell(p)); };
    for (const auto& t : triangles) {
        if (!finite(t.a) || !finite(t.b) || !finite(t.c)) continue;
        const glm::vec2 a(t.a), b(t.b), c(t.c);
        const glm::vec2 tlo = glm::min(a, glm::min(b, c)), thi = glm::max(a, glm::max(b, c));
        if (glm::any(glm::greaterThan(glm::abs(tlo), glm::vec2(kFar))) ||
            glm::any(glm::greaterThan(glm::abs(thi), glm::vec2(kFar))))
            continue;
        const glm::vec2 corners[3] = {a, b, c};
        for (int e = 0; e < 3; ++e) {
            const glm::vec2 p = corners[e], q = corners[(e + 1) % 3];
            const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(q - p) / (0.5f * cell))));
            for (int s = 0; s <= steps; ++s) mark(glm::mix(p, q, static_cast<float>(s) / static_cast<float>(steps)));
        }
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::abs(area) < 1.0f) continue;
        for (float y = (std::floor(tlo.y / cell) + 0.5f) * cell; y <= thi.y; y += cell)
            for (float x = (std::floor(tlo.x / cell) + 0.5f) * cell; x <= thi.x; x += cell) {
                const float w1 = ((x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y)) / area;
                const float w2 = ((b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y)) / area;
                if (w1 >= 0.0f && w2 >= 0.0f && w1 + w2 <= 1.0f) mark({x, y});
            }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

HorizonMesh horizon_mesh(std::span<const HorizonTriangle> triangles, const HorizonLand& land) {
    HorizonMesh mesh;
    if (!land.valid) return mesh;
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
    if (!(lo.x <= hi.x)) return mesh;
    Grid g;
    const float border = 4.0f * g.cell;
    g.lo = glm::floor((lo - border) / g.cell) * g.cell;
    g.nx = static_cast<int>(std::ceil((hi.x + border - g.lo.x) / g.cell));
    g.ny = static_cast<int>(std::ceil((hi.y + border - g.lo.y) / g.cell));
    const std::size_t cells = static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny);
    g.zmin.assign(cells, inf);
    g.zmax.assign(cells, -inf);
    g.water.assign(cells, -inf);
    g.land.assign(cells, 0);
    g.wet.assign(cells, 0);

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
            for (int s = 0; s <= steps; ++s) g.mark(glm::mix(a, b, static_cast<float>(s) / static_cast<float>(steps)), t.liquid);
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
                g.mark(glm::vec3(p, w0 * t.a.z + w1 * t.b.z + w2 * t.c.z), t.liquid);
            }
        if (t.liquid) water_levels.push_back((t.a.z + t.b.z + t.c.z) / 3.0f);
    }
    bool has_water = !water_levels.empty();
    if (has_water) {
        std::nth_element(water_levels.begin(), water_levels.begin() + static_cast<std::ptrdiff_t>(water_levels.size() / 2),
                         water_levels.end());
        mesh.water_level = water_levels[water_levels.size() / 2];
    }

    // Every empty square is outside: past the zone's edges, and in a pocket the zone's terrain closes round (a gap
    // between its walls, a hole in it, which would show the void), where the land is a lid just under the rim. Open:
    // the empty squares the grid's border reaches without crossing terrain.
    g.outside.assign(cells, 0);
    for (std::size_t i = 0; i < cells; ++i) g.outside[i] = g.covered(i) ? 0 : 1;
    std::vector<std::uint8_t> open(cells, 0);
    std::vector<std::pair<int, int>> stack;
    auto reach = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= g.nx || y >= g.ny) return;
        const std::size_t i = g.at(x, y);
        if (open[i] || g.covered(i)) return;
        open[i] = 1;
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
        if (!g.outside[i]) return;
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

    // The zone's edge's height and whether it is water, carried out from the nearest covered square and smoothed.
    g.edge.assign(cells, land.ground);
    g.sea.assign(cells, 0.0f);
    for (std::size_t i = 0; i < cells; ++i) {
        const int s = source[i];
        if (s < 0) continue;
        const auto si = static_cast<std::size_t>(s);
        const bool water_edge = has_water && g.wet[si] && (!g.land[si] || g.water[si] >= g.zmax[si] - 2.0f);
        g.edge[i] = water_edge ? g.water[si] : g.zmax[si];
        g.sea[i] = water_edge ? 1.0f : 0.0f;
    }
    for (int pass = 0; pass < 2; ++pass)
        for (int axis = 0; axis < 2; ++axis) {
            const std::vector<float> edge = g.edge, sea = g.sea;
            constexpr int kRadius = 2;
            for (int y = 0; y < g.ny; ++y)
                for (int x = 0; x < g.nx; ++x) {
                    const std::size_t i = g.at(x, y);
                    if (!g.outside[i]) continue;
                    float se = 0.0f, ss = 0.0f, n = 0.0f;
                    for (int k = -kRadius; k <= kRadius; ++k) {
                        const int px = axis == 0 ? x + k : x, py = axis == 0 ? y : y + k;
                        if (px < 0 || py < 0 || px >= g.nx || py >= g.ny) continue;
                        const std::size_t j = g.at(px, py);
                        if (source[j] < 0) continue;
                        se += edge[j];
                        ss += sea[j];
                        n += 1.0f;
                    }
                    g.edge[i] = se / n;
                    g.sea[i] = ss / n;
                }
        }

    // How low the land is tucked: under a covered square's terrain; beside one, just under the lowest of them (a
    // wall's foot, the open ground at a zone line).
    g.under.assign(cells, inf);
    for (int y = 0; y < g.ny; ++y)
        for (int x = 0; x < g.nx; ++x) {
            const std::size_t i = g.at(x, y);
            const float own = g.land[i] ? g.zmin[i] : g.water[i];
            if (g.covered(i)) {
                g.under[i] = own - kHorizonTuck;
                continue;
            }
            if (!g.outside[i]) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int px = x + dx, py = y + dy;
                    if (px < 0 || py < 0 || px >= g.nx || py >= g.ny) continue;
                    const std::size_t j = g.at(px, py);
                    if (!g.covered(j)) continue;
                    g.under[i] = std::min(g.under[i], (g.land[j] ? g.zmin[j] : g.water[j]) - 5.0f);
                }
        }

    // The mesh's grid.
    const int scale = std::max(1, static_cast<int>(std::ceil(static_cast<float>(std::max(g.nx, g.ny)) / 192.0f)));
    const float step = g.cell * static_cast<float>(scale);
    const glm::vec2 ghi = g.lo + glm::vec2(static_cast<float>(g.nx), static_cast<float>(g.ny)) * g.cell;
    const std::vector<float> xs = axis_lines(g.lo.x, ghi.x, step), ys = axis_lines(g.lo.y, ghi.y, step);
    const std::size_t cols = xs.size(), rows = ys.size();

    const auto seed = static_cast<std::uint32_t>(land.seed * 10.0f);
    const float rugged = std::clamp(land.relief / 600.0f, 0.2f, 1.5f);
    const float near_amp = 200.0f + 300.0f * rugged; // the rolling hills'
    const float far_amp = 900.0f + 900.0f * rugged;  // the mountains'
    // Tucked: in a covered square or the empty square beside one, under the zone's terrain.
    enum Kind : std::uint8_t { kLand, kTucked };
    std::vector<std::uint8_t> kind(cols * rows, kLand);
    std::vector<float> sea(cols * rows, 0.0f), peak(cols * rows, 0.0f);
    mesh.positions.resize(cols * rows);
    for (std::size_t r = 0; r < rows; ++r)
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t v = r * cols + c;
            const glm::vec2 p(xs[c], ys[r]);
            float extra = 0.0f;
            const std::size_t i = g.square(p, extra);
            float h = 0.0f;
            if (extra <= 0.0f && std::isfinite(g.under[i])) {
                kind[v] = kTucked;
                h = g.under[i];
            } else {
                const float d = (std::max(g.dist[i] - 0.5f, 0.0f)) * g.cell + extra;
                if (extra <= 0.0f && !open[i]) {
                    mesh.positions[v] = glm::vec3(p, g.edge[i] - 20.0f); // a pocket's lid
                    continue;
                }
                const float roll = smooth(0.2f, 0.8f, fbm(p / 1300.0f, 4, seed, false)) * 0.85f +
                                   fbm(p / 420.0f, 2, seed + 3u, false) * 0.15f;
                float mtn = fbm(p / 3200.0f, 5, seed + 7u, true);
                mtn *= mtn;
                const float rise = near_amp * roll * smooth(0.0f, 800.0f, d) + far_amp * mtn * smooth(800.0f, 5000.0f, d);
                h = glm::mix(g.edge[i] - 20.0f, land.ground + rise, smooth(0.0f, kHorizonBlend, d));
                if (g.sea[i] > 0.0f) {
                    const float floor = glm::mix(mesh.water_level - 20.0f, mesh.water_level - kHorizonSeaDepth,
                                                 smooth(0.0f, 800.0f, d));
                    const float coast = floor + 1.3f * rise * smooth(2500.0f, 8000.0f, d);
                    h = glm::mix(h, coast, g.sea[i]);
                }
                sea[v] = g.sea[i];
                peak[v] = smooth(0.45f, 0.75f, (h - land.ground) / far_amp);
            }
            mesh.positions[v] = glm::vec3(p, h);
        }

    // The normals from the neighbouring vertices; the light left by woods and valleys.
    mesh.normals.resize(mesh.positions.size(), glm::vec3(0.0f, 0.0f, 1.0f));
    mesh.shade.resize(mesh.positions.size(), 1.0f);
    for (std::size_t r = 0; r < rows; ++r)
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t v = r * cols + c;
            const std::size_t c0 = c > 0 ? c - 1 : c, c1 = c + 1 < cols ? c + 1 : c;
            const std::size_t r0 = r > 0 ? r - 1 : r, r1 = r + 1 < rows ? r + 1 : r;
            const glm::vec3 dx = mesh.positions[r * cols + c1] - mesh.positions[r * cols + c0];
            const glm::vec3 dy = mesh.positions[r1 * cols + c] - mesh.positions[r0 * cols + c];
            const glm::vec3 n = glm::cross(dx, dy);
            if (glm::dot(n, n) > 1e-8f && n.z > 0.0f) mesh.normals[v] = glm::normalize(n);
            if (kind[v] != kLand) continue;
            const glm::vec2 p(mesh.positions[v]);
            const float woods = smooth(0.52f, 0.64f, fbm(p / 650.0f, 3, seed + 13u, false)) *
                                smooth(0.75f, 0.9f, mesh.normals[v].z) * (1.0f - peak[v]);
            const float valley = smooth(-100.0f, 400.0f, mesh.positions[v].z - land.ground);
            // Broad patches of lighter and darker ground, so the texture's repeat does not show across the plains.
            const float patches = 0.88f + 0.24f * fbm(p / 380.0f, 3, seed + 17u, false);
            mesh.shade[v] = std::min((1.0f - 0.3f * woods) * glm::mix(0.85f, 1.0f, valley) * patches, 1.0f);
        }

    // The squares: two triangles each along the shorter diagonal in height, those under the zone too (seen through the
    // back of its walls, which have only their inner faces). Where the sea is, its surface too.
    std::vector<std::uint32_t> water_vertex(mesh.positions.size(), std::numeric_limits<std::uint32_t>::max());
    auto surface = [&](std::size_t v) {
        if (water_vertex[v] == std::numeric_limits<std::uint32_t>::max()) {
            water_vertex[v] = static_cast<std::uint32_t>(mesh.positions.size());
            mesh.positions.push_back(glm::vec3(glm::vec2(mesh.positions[v]), mesh.water_level));
            mesh.normals.emplace_back(0.0f, 0.0f, 1.0f);
            mesh.shade.push_back(1.0f);
        }
        return water_vertex[v];
    };
    auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const glm::vec3 pa = mesh.positions[a], pb = mesh.positions[b], pc = mesh.positions[c];
        const glm::vec3 n = glm::cross(pb - pa, pc - pa);
        const float len = glm::length(n);
        const bool steep = len > 0.0f && n.z / len < kHorizonRock;
        const bool high = (peak[a] + peak[b] + peak[c]) / 3.0f > 0.5f;
        auto& out = steep || high ? mesh.rock : mesh.ground;
        out.insert(out.end(), {a, b, c});
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
            if (!has_water) continue;
            bool open_sea = true, below = false;
            for (std::size_t k = 0; k < 4; ++k) {
                open_sea &= kind[q[k]] == kLand && sea[q[k]] >= 0.5f;
                below |= z[k] < mesh.water_level;
            }
            if (!open_sea || !below) continue;
            const std::uint32_t wa = surface(q[0]), wb = surface(q[1]), wc = surface(q[2]), wd = surface(q[3]);
            mesh.water.insert(mesh.water.end(), {wa, wb, wc, wa, wc, wd});
        }
    return mesh;
}

} // namespace hailstone::render
