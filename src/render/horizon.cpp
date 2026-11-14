#include "render/horizon.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
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
    int owner = -1;      // the source with the most cover here
    float cover = 0.0f;  // its cover
    float weight = 0.0f; // the `only` source's cover times the band's (its own ground's share)
};

Point land_point(const HorizonField& g, const HorizonSources& sources, const Shape& k, glm::vec2 p, int only) {
    Point out;
    float extra = 0.0f;
    const std::size_t i = g.square(p, extra);
    if (extra <= 0.0f && std::isfinite(g.under[i])) {
        out.tucked = true;
        out.h = g.under[i];
        return out;
    }
    if (extra <= 0.0f && !g.open[i]) {
        out.h = g.edge[i] - 20.0f; // a pocket's lid
        return out;
    }
    const float d = (std::max(g.dist[i] - 0.5f, 0.0f)) * g.cell + extra;
    const float roll = smooth(0.2f, 0.8f, fbm(p / 1300.0f, 4, k.seed, false)) * 0.85f +
                       fbm(p / 420.0f, 2, k.seed + 3u, false) * 0.15f;
    float mtn = fbm(p / 3200.0f, 5, k.seed + 7u, true);
    mtn *= mtn;
    const float rise = k.near_amp * roll * smooth(0.0f, 800.0f, d) + k.far_amp * mtn * smooth(800.0f, 5000.0f, d);
    const float start = g.edge[i] - 20.0f;
    float h = glm::mix(start, g.land.ground + rise, smooth(0.0f, kHorizonBlend, d));
    if (g.sea[i] > 0.0f) {
        const float floor = glm::mix(g.water_level - 20.0f, g.water_level - kHorizonSeaDepth, smooth(0.0f, 800.0f, d));
        const float coast = floor + 1.3f * rise * smooth(2500.0f, 8000.0f, d);
        h = glm::mix(h, coast, g.sea[i]);
    }
    out.sea = g.sea[i];
    // The neighbours' ground: from the zone's edge it eases onto theirs across the band, and fades back into the hills
    // past it as their cover does.
    float sum = 0.0f, weighed = 0.0f, cover = 0.0f, only_cover = 0.0f;
    for (std::size_t s = 0; s < sources.size(); ++s) {
        const HorizonSourceSample smp = sample_source(*sources[s], p);
        if (!(smp.cover > 0.0f)) continue;
        sum += smp.cover;
        weighed += smp.cover * smp.height;
        if (smp.cover > cover) {
            cover = smp.cover;
            out.owner = static_cast<int>(s);
        }
        if (static_cast<int>(s) == only) only_cover = smp.cover;
    }
    float flat = 0.0f; // how much of the neighbours' ground (no mountains on it)
    if (sum > 0.0f) {
        const float theirs = weighed / sum;
        const float band = std::clamp(kHorizonBandSlope * std::abs(start - theirs), kHorizonBandMin, kHorizonBandMax);
        const float across = smooth(0.0f, band, d);
        h = glm::mix(h, glm::mix(start, theirs, across), cover);
        out.sea *= 1.0f - cover;
        out.cover = cover;
        out.weight = only_cover * across;
        flat = cover;
    }
    out.h = h;
    out.peak = smooth(0.45f, 0.75f, (h - g.land.ground) / k.far_amp) * (1.0f - flat);
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

    // The zone's edge's height and whether it is water, carried out from the nearest covered square and smoothed.
    g.edge.assign(cells, land.ground);
    g.sea.assign(cells, 0.0f);
    for (std::size_t i = 0; i < cells; ++i) {
        const int s = source[i];
        if (s < 0) continue;
        const auto si = static_cast<std::size_t>(s);
        const bool water_edge = g.has_water && g.wet[si] && (!g.dry[si] || g.water[si] >= g.zmax[si] - 2.0f);
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
                    if (!outside[i]) continue;
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
                    g.under[i] = std::min(g.under[i], (g.dry[j] ? g.zmin[j] : g.water[j]) - 5.0f);
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

void sort_sources(HorizonSources& sources) {
    std::erase_if(sources, [](const auto& s) { return !s || !s->valid(); });
    std::stable_sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a->name < b->name; });
    sources.erase(std::unique(sources.begin(), sources.end(),
                              [](const auto& a, const auto& b) { return a->name == b->name; }),
                  sources.end());
}

HorizonSource horizon_source(std::string name, glm::vec2 lo, float cell, int nx, int ny, std::vector<float> ground) {
    HorizonSource s;
    s.name = std::move(name);
    s.cell = cell;
    if (nx <= 0 || ny <= 0 || !(cell > 0.0f) ||
        ground.size() != static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny))
        return s;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // Padded by the fade, so the squares past the ground carry its nearest height and distance.
    const int pad = static_cast<int>(std::ceil(kHorizonSourceFade / cell)) + 1;
    s.nx = nx + 2 * pad;
    s.ny = ny + 2 * pad;
    s.lo = lo - static_cast<float>(pad) * cell;
    const auto at = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(s.nx) + static_cast<std::size_t>(x);
    };
    std::vector<float> raw(static_cast<std::size_t>(s.nx) * static_cast<std::size_t>(s.ny), nan);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            const float h = ground[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x)];
            raw[at(x + pad, y + pad)] = std::isfinite(h) ? h : nan;
        }
    // Opened: the lowest ground within kHorizonOpening squares, then the highest of those lows. Something standing
    // above the ground narrower than the window goes; the ground's own shape stays.
    constexpr int r = kHorizonOpening;
    auto window = [&](const std::vector<float>& in, bool lowest) {
        std::vector<float> out(in.size(), nan);
        for (int y = 0; y < s.ny; ++y)
            for (int x = 0; x < s.nx; ++x) {
                if (!std::isfinite(in[at(x, y)])) continue;
                float v = lowest ? inf : -inf;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        const int px = x + dx, py = y + dy;
                        if (px < 0 || py < 0 || px >= s.nx || py >= s.ny) continue;
                        const float h = in[at(px, py)];
                        if (!std::isfinite(h)) continue;
                        v = lowest ? std::min(v, h) : std::max(v, h);
                    }
                out[at(x, y)] = v;
            }
        return out;
    };
    s.height = window(window(raw, true), false);
    finish_horizon_source(s);
    return s;
}

void finish_horizon_source(HorizonSource& s) {
    const std::size_t n = static_cast<std::size_t>(std::max(s.nx, 0)) * static_cast<std::size_t>(std::max(s.ny, 0));
    s.fill.assign(n, 0.0f);
    s.dist.assign(n, std::numeric_limits<float>::infinity());
    if (n == 0 || s.height.size() != n) {
        s.fill.clear();
        s.dist.clear();
        return;
    }
    std::vector<int> nearest(n, -1);
    for (std::size_t i = 0; i < n; ++i)
        if (std::isfinite(s.height[i])) {
            nearest[i] = static_cast<int>(i);
            s.dist[i] = 0.0f;
        }
    auto relax = [&](int x, int y, int px, int py) {
        if (px < 0 || py < 0 || px >= s.nx || py >= s.ny) return;
        const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(s.nx) + static_cast<std::size_t>(x);
        const int src = nearest[static_cast<std::size_t>(py) * static_cast<std::size_t>(s.nx) + static_cast<std::size_t>(px)];
        if (src < 0) return;
        const int sx = src % s.nx, sy = src / s.nx;
        const float d = std::hypot(static_cast<float>(x - sx), static_cast<float>(y - sy)) * s.cell;
        if (d < s.dist[i]) {
            s.dist[i] = d;
            nearest[i] = src;
        }
    };
    for (int y = 0; y < s.ny; ++y)
        for (int x = 0; x < s.nx; ++x) {
            relax(x, y, x - 1, y);
            relax(x, y, x - 1, y - 1);
            relax(x, y, x, y - 1);
            relax(x, y, x + 1, y - 1);
        }
    for (int y = s.ny - 1; y >= 0; --y)
        for (int x = s.nx - 1; x >= 0; --x) {
            relax(x, y, x + 1, y);
            relax(x, y, x + 1, y + 1);
            relax(x, y, x, y + 1);
            relax(x, y, x - 1, y + 1);
        }
    for (std::size_t i = 0; i < n; ++i)
        if (nearest[i] >= 0) s.fill[i] = s.height[static_cast<std::size_t>(nearest[i])];
}

HorizonSourceSample sample_source(const HorizonSource& s, glm::vec2 p) {
    HorizonSourceSample out;
    if (!s.valid()) return out;
    // Bilinear between the squares' centres, clamped to the grid; past it, further by the distance to it.
    const glm::vec2 hi = s.lo + glm::vec2(static_cast<float>(s.nx), static_cast<float>(s.ny)) * s.cell;
    const glm::vec2 q = glm::clamp(p, s.lo, hi);
    const float extra = glm::length(p - q);
    const float fx = std::clamp((q.x - s.lo.x) / s.cell - 0.5f, 0.0f, static_cast<float>(s.nx - 1));
    const float fy = std::clamp((q.y - s.lo.y) / s.cell - 0.5f, 0.0f, static_cast<float>(s.ny - 1));
    const int x0 = std::min(static_cast<int>(fx), s.nx - 1), y0 = std::min(static_cast<int>(fy), s.ny - 1);
    const int x1 = std::min(x0 + 1, s.nx - 1), y1 = std::min(y0 + 1, s.ny - 1);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    auto at = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(s.nx) + static_cast<std::size_t>(x);
    };
    auto lerp2 = [&](const std::vector<float>& v) {
        return glm::mix(glm::mix(v[at(x0, y0)], v[at(x1, y0)], tx), glm::mix(v[at(x0, y1)], v[at(x1, y1)], tx), ty);
    };
    const float dist = lerp2(s.dist) + extra;
    if (!std::isfinite(dist)) return out; // a source with no ground at all
    out.height = lerp2(s.fill);
    out.cover = 1.0f - smooth(0.0f, kHorizonSourceFade, dist);
    return out;
}

std::optional<float> source_ground(const HorizonSource& s, glm::vec2 p) {
    if (!s.valid()) return std::nullopt;
    const float gx = (p.x - s.lo.x) / s.cell, gy = (p.y - s.lo.y) / s.cell;
    if (!(gx >= 0.0f && gy >= 0.0f && gx < static_cast<float>(s.nx) && gy < static_cast<float>(s.ny))) return std::nullopt;
    auto at = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(s.nx) + static_cast<std::size_t>(x);
    };
    const float own = s.height[at(static_cast<int>(gx), static_cast<int>(gy))];
    if (!std::isfinite(own)) return std::nullopt;
    const float fx = std::clamp(gx - 0.5f, 0.0f, static_cast<float>(s.nx - 1));
    const float fy = std::clamp(gy - 0.5f, 0.0f, static_cast<float>(s.ny - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, s.nx - 1), y1 = std::min(y0 + 1, s.ny - 1);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    auto h = [&](int x, int y) {
        const float v = s.height[at(x, y)];
        return std::isfinite(v) ? v : own;
    };
    return glm::mix(glm::mix(h(x0, y0), h(x1, y0), tx), glm::mix(h(x0, y1), h(x1, y1), tx), ty);
}

float horizon_height(const HorizonField& field, const HorizonSources& sources, glm::vec2 p) {
    if (!field.valid()) return 0.0f;
    return land_point(field, sources, shape_of(field.land), p, -1).h;
}

HorizonMesh horizon_mesh(std::span<const HorizonTriangle> triangles, const HorizonLand& land) {
    return horizon_mesh(horizon_field(triangles, land), {});
}

HorizonMesh horizon_mesh(const HorizonField& g, const HorizonSources& sources, int only) {
    HorizonMesh mesh;
    if (!g.valid()) return mesh;
    if (only >= static_cast<int>(sources.size())) return mesh;
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
            points[v] = land_point(g, sources, k, p, only);
            mesh.positions[v] = glm::vec3(p, points[v].h);
        }

    // The normals from the neighbouring vertices; the light left by woods and valleys.
    mesh.normals.resize(mesh.positions.size(), glm::vec3(0.0f, 0.0f, 1.0f));
    mesh.shade.resize(mesh.positions.size(), 1.0f);
    mesh.weight.resize(mesh.positions.size(), 0.0f);
    for (std::size_t r = 0; r < rows; ++r)
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t v = r * cols + c;
            mesh.weight[v] = points[v].weight;
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
            mesh.shade[v] = std::min((1.0f - 0.3f * woods) * glm::mix(0.85f, 1.0f, valley) * patches, 1.0f);
        }

    // The squares: two triangles each along the shorter diagonal in height, those under the zone too (seen through the
    // back of its walls, which have only their inner faces). Where the sea is, its surface too. With `only`, the
    // squares whose corner with the most cover is that source's.
    std::vector<std::uint32_t> water_vertex(mesh.positions.size(), std::numeric_limits<std::uint32_t>::max());
    auto surface = [&](std::size_t v) {
        if (water_vertex[v] == std::numeric_limits<std::uint32_t>::max()) {
            water_vertex[v] = static_cast<std::uint32_t>(mesh.positions.size());
            mesh.positions.push_back(glm::vec3(glm::vec2(mesh.positions[v]), mesh.water_level));
            mesh.normals.emplace_back(0.0f, 0.0f, 1.0f);
            mesh.shade.push_back(1.0f);
            mesh.weight.push_back(0.0f);
        }
        return water_vertex[v];
    };
    auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const glm::vec3 pa = mesh.positions[a], pb = mesh.positions[b], pc = mesh.positions[c];
        const glm::vec3 n = glm::cross(pb - pa, pc - pa);
        const float len = glm::length(n);
        const bool steep = len > 0.0f && n.z / len < kHorizonRock;
        const bool high = (points[a].peak + points[b].peak + points[c].peak) / 3.0f > 0.5f;
        auto& out = steep || high ? mesh.rock : mesh.ground;
        out.insert(out.end(), {a, b, c});
    };
    for (std::size_t r = 0; r + 1 < rows; ++r)
        for (std::size_t c = 0; c + 1 < cols; ++c) {
            const std::size_t q[4] = {r * cols + c, r * cols + c + 1, (r + 1) * cols + c + 1, (r + 1) * cols + c};
            if (only >= 0) {
                int owner = -1;
                float most = 0.0f;
                for (std::size_t corner : q)
                    if (points[corner].cover > most) {
                        most = points[corner].cover;
                        owner = points[corner].owner;
                    }
                if (owner != only) continue;
                mesh.squares.push_back(static_cast<std::int64_t>(r) * static_cast<std::int64_t>(cols) +
                                       static_cast<std::int64_t>(c));
            }
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
            if (!g.has_water || only >= 0) continue;
            bool open_sea = true, below = false;
            for (std::size_t corner : q) {
                open_sea &= !points[corner].tucked && points[corner].sea >= 0.5f;
                below |= mesh.positions[corner].z < mesh.water_level;
            }
            if (!open_sea || !below) continue;
            const std::uint32_t wa = surface(q[0]), wb = surface(q[1]), wc = surface(q[2]), wd = surface(q[3]);
            mesh.water.insert(mesh.water.end(), {wa, wb, wc, wa, wc, wd});
        }
    return mesh;
}

namespace {
constexpr std::uint32_t kCacheMagic = 0x314e5a48u; // "HZN1"
constexpr std::uint32_t kCacheVersion = 1;
constexpr std::size_t kCacheMaxName = 64;
constexpr int kCacheMaxSide = 4096;

void write_name(ByteWriter& w, std::string_view s) {
    const std::size_t n = std::min(s.size(), kCacheMaxName);
    w.le(static_cast<std::uint16_t>(n));
    w.bytes(std::as_bytes(std::span(s.data(), n)));
}

std::string read_name(ByteReader& r) {
    const auto n = r.le<std::uint16_t>();
    if (n > kCacheMaxName) throw std::out_of_range("horizon cache: name too long");
    const auto b = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
} // namespace

std::vector<std::byte> write_horizon_cache(const HorizonCacheEntry& e) {
    ByteWriter w;
    w.le(kCacheMagic);
    w.le(kCacheVersion);
    write_name(w, e.zone);
    write_name(w, e.neighbour);
    w.le(e.key);
    for (int i = 0; i < 3; ++i) w.f32(e.offset[i]);
    const HorizonSource& s = e.source;
    w.f32(s.lo.x);
    w.f32(s.lo.y);
    w.f32(s.cell);
    w.le(static_cast<std::uint32_t>(std::max(s.nx, 0)));
    w.le(static_cast<std::uint32_t>(std::max(s.ny, 0)));
    for (float h : s.height) w.f32(h);
    return w.take();
}

std::optional<HorizonCacheEntry> read_horizon_cache(std::span<const std::byte> data) {
    try {
        ByteReader r(data);
        if (r.le<std::uint32_t>() != kCacheMagic || r.le<std::uint32_t>() != kCacheVersion) return std::nullopt;
        HorizonCacheEntry e;
        e.zone = read_name(r);
        e.neighbour = read_name(r);
        e.key = r.le<std::uint64_t>();
        for (int i = 0; i < 3; ++i) e.offset[i] = r.f32();
        HorizonSource& s = e.source;
        s.name = e.neighbour;
        s.lo.x = r.f32();
        s.lo.y = r.f32();
        s.cell = r.f32();
        const auto nx = r.le<std::uint32_t>(), ny = r.le<std::uint32_t>();
        if (!std::isfinite(s.lo.x) || !std::isfinite(s.lo.y) || !std::isfinite(e.offset.x) ||
            !std::isfinite(e.offset.y) || !std::isfinite(e.offset.z) || !(s.cell >= 1.0f && s.cell <= 4096.0f) ||
            nx == 0 || ny == 0 || nx > static_cast<std::uint32_t>(kCacheMaxSide) ||
            ny > static_cast<std::uint32_t>(kCacheMaxSide))
            return std::nullopt;
        const std::size_t n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
        if (r.remaining() != n * 4) return std::nullopt;
        s.nx = static_cast<int>(nx);
        s.ny = static_cast<int>(ny);
        s.height.resize(n);
        for (float& h : s.height) {
            h = r.f32();
            if (std::isinf(h)) h = std::numeric_limits<float>::quiet_NaN();
        }
        finish_horizon_source(s);
        return e;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

} // namespace hailstone::render
