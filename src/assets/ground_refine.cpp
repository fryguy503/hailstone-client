#include "assets/ground_refine.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace hailstone::assets {

namespace {

// Past this many units from the origin a vertex is a damaged mesh's (as assets/zone_flora.cpp takes it).
constexpr float kMaxCoord = 100000.0f;

bool sane(const glm::vec3& p) {
    return std::abs(p.x) <= kMaxCoord && std::abs(p.y) <= kMaxCoord && std::abs(p.z) <= kMaxCoord; // NaN fails
}

bool is_ground(const Mesh& mesh, const MeshPart& part) {
    if (mesh.front_ccw || mesh.heightmap_terrain || !part.solid || part.material >= mesh.materials.size()) return false;
    const Material& m = mesh.materials[part.material];
    return m.blend == BlendMode::Opaque && m.eqg_shader == EqgShader::None;
}

// Corners are welded on a 1/128-unit grid: the same corner in two region meshes, each placed by its own centre, may
// differ in its last bits.
struct WeldKey {
    std::int64_t x = 0, y = 0, z = 0;
    bool operator==(const WeldKey&) const = default;
};
struct WeldHash {
    std::size_t operator()(const WeldKey& k) const {
        std::uint64_t h = static_cast<std::uint64_t>(k.x) * 0x9e3779b97f4a7c15ull;
        h ^= static_cast<std::uint64_t>(k.y) + 0x7f4a7c159e3779b9ull + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(k.z) + 0x94d049bb133111ebull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};
WeldKey weld_key(const glm::vec3& p) {
    auto q = [](float v) { return static_cast<std::int64_t>(std::llround(static_cast<double>(v) * 128.0)); };
    return {q(p.x), q(p.y), q(p.z)};
}

struct Face {
    std::uint32_t mesh = 0, first = 0; // the triangle's first index in terrain[mesh]
    std::uint32_t w[3] = {};           // welded corners
    glm::vec3 n{0.0f};                 // the lit side's unit normal
};

// One edge of the ground, from its lower welded corner to its higher: the curve's two inner control points.
struct Edge {
    glm::vec3 c0{0.0f}, c1{0.0f}; // nearer the lower corner, nearer the higher
    std::uint32_t segments = 1;
    bool curved = false;
};

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    return a < b ? (std::uint64_t{a} << 32 | b) : (std::uint64_t{b} << 32 | a);
}

std::uint32_t pow2_at_least(float v, std::uint32_t cap) {
    std::uint32_t n = 1;
    while (n < cap && static_cast<float>(n) < v) n *= 2;
    return n;
}

glm::vec3 bezier(const glm::vec3& p0, const glm::vec3& c0, const glm::vec3& c1, const glm::vec3& p1, float t) {
    const float s = 1.0f - t;
    return s * s * s * p0 + 3.0f * s * s * t * c0 + 3.0f * s * t * t * c1 + t * t * t * p1;
}

std::uint32_t lerp_color(std::uint32_t a, std::uint32_t b, std::uint32_t c, float wa, float wb, float wc) {
    std::uint32_t out = 0;
    for (int k = 0; k < 4; ++k) {
        const int shift = 8 * k;
        const float v = wa * static_cast<float>((a >> shift) & 0xffu) + wb * static_cast<float>((b >> shift) & 0xffu) +
                        wc * static_cast<float>((c >> shift) & 0xffu);
        out |= static_cast<std::uint32_t>(std::clamp(std::lround(v), 0L, 255L)) << shift;
    }
    return out;
}

float distance_to_plane(const glm::vec3& p, const glm::vec3& a, const glm::vec3& n) { return std::abs(glm::dot(p - a, n)); }

} // namespace

GroundRefineStats refine_ground(Zone& zone, const GroundRefineOptions& options) {
    GroundRefineStats stats;
    zone.draw_terrain.clear();
    zone.draw_terrain_source.clear();
    const std::uint32_t max_segments = pow2_at_least(static_cast<float>(std::max(1u, options.max_segments)), 64);

    // The ground's faces, and their corners welded by position.
    std::vector<Face> faces;
    std::vector<glm::vec3> welded;
    std::unordered_map<WeldKey, std::uint32_t, WeldHash> weld;
    std::vector<std::vector<std::int32_t>> face_of(zone.terrain.size()); // per triangle: its face, or -1
    for (std::size_t mi = 0; mi < zone.terrain.size(); ++mi) {
        const Mesh& mesh = zone.terrain[mi];
        face_of[mi].assign(mesh.indices.size() / 3, -1);
        for (const MeshPart& part : mesh.parts) {
            if (!is_ground(mesh, part)) continue;
            const std::size_t end = std::min<std::size_t>(std::size_t{part.first_index} + part.index_count, mesh.indices.size());
            for (std::size_t i = part.first_index; i + 3 <= end; i += 3) {
                const Vertex* v[3];
                bool ok = true;
                for (int k = 0; k < 3 && ok; ++k) {
                    const std::uint32_t at = mesh.indices[i + static_cast<std::size_t>(k)];
                    ok = at < mesh.vertices.size() && sane(mesh.vertices[at].position);
                    if (ok) v[k] = &mesh.vertices[at];
                }
                if (!ok) continue;
                glm::vec3 n = glm::cross(v[1]->position - v[0]->position, v[2]->position - v[0]->position);
                const float len = glm::length(n);
                if (!(len > 1e-4f)) continue;
                n /= len;
                // The lit side, from the file's vertex normals (as the flora takes it); by the winding when they are
                // no help (WLD triangles are clockwise from the front).
                const float side = glm::dot(n, v[0]->normal + v[1]->normal + v[2]->normal);
                if (side < 0.0f || (side == 0.0f && !mesh.front_ccw)) n = -n;
                Face f;
                f.mesh = static_cast<std::uint32_t>(mi);
                f.first = static_cast<std::uint32_t>(i);
                f.n = n;
                for (int k = 0; k < 3; ++k) {
                    auto [it, added] = weld.try_emplace(weld_key(v[k]->position), static_cast<std::uint32_t>(welded.size()));
                    if (added) welded.push_back(v[k]->position);
                    f.w[k] = it->second;
                }
                if (f.w[0] == f.w[1] || f.w[1] == f.w[2] || f.w[0] == f.w[2]) continue; // collapsed by the weld
                face_of[mi][i / 3] = static_cast<std::int32_t>(faces.size());
                faces.push_back(f);
            }
        }
    }
    if (faces.empty()) return stats;
    stats.ground_triangles = faces.size();

    // The faces round each welded corner, in face order (so two corners that average the same faces add them in the
    // same order and get the same bits).
    std::vector<std::uint32_t> start(welded.size() + 1, 0), around(faces.size() * 3);
    for (const Face& f : faces)
        for (std::uint32_t w : f.w) ++start[w + 1];
    for (std::size_t i = 1; i < start.size(); ++i) start[i] += start[i - 1];
    {
        std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
        for (std::uint32_t fi = 0; fi < faces.size(); ++fi)
            for (std::uint32_t w : faces[fi].w) around[fill[w]++] = fi;
    }
    auto corner_angle = [&](const Face& f, std::uint32_t w) {
        int k = 0;
        while (f.w[k] != w) ++k;
        const glm::vec3& p = welded[w];
        const glm::vec3 a = glm::normalize(welded[f.w[(k + 1) % 3]] - p), b = glm::normalize(welded[f.w[(k + 2) % 3]] - p);
        return std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f));
    };
    const float crease_cos = std::cos(glm::radians(std::clamp(options.crease_degrees, 0.0f, 180.0f)));
    std::vector<glm::vec3> corner_n(faces.size() * 3);
    for (std::size_t fi = 0; fi < faces.size(); ++fi)
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t w = faces[fi].w[k];
            glm::vec3 sum(0.0f);
            for (std::uint32_t j = start[w]; j < start[w + 1]; ++j) {
                const Face& g = faces[around[j]];
                if (glm::dot(g.n, faces[fi].n) >= crease_cos) sum += corner_angle(g, w) * g.n;
            }
            const float len = glm::length(sum);
            corner_n[fi * 3 + static_cast<std::size_t>(k)] = len > 1e-6f ? sum / len : faces[fi].n;
        }

    // The edges: curved where exactly two ground faces meet on it and agree on both corners' normals.
    struct EdgeUse {
        std::uint32_t face[2] = {};
        int k[2] = {}; // the edge from face corner k to k + 1
        int count = 0;
    };
    std::unordered_map<std::uint64_t, EdgeUse> uses;
    uses.reserve(faces.size() * 2);
    for (std::uint32_t fi = 0; fi < faces.size(); ++fi)
        for (int k = 0; k < 3; ++k) {
            EdgeUse& u = uses[edge_key(faces[fi].w[k], faces[fi].w[(k + 1) % 3])];
            if (u.count < 2) {
                u.face[u.count] = fi;
                u.k[u.count] = k;
            }
            ++u.count;
        }
    auto normal_at = [&](std::uint32_t fi, std::uint32_t w) {
        int k = 0;
        while (faces[fi].w[k] != w) ++k;
        return corner_n[fi * 3 + static_cast<std::size_t>(k)];
    };
    const float tolerance = std::max(options.tolerance, 1e-4f);
    const float max_offset = std::max(options.max_offset, 0.0f);
    std::unordered_map<std::uint64_t, Edge> edges;
    edges.reserve(uses.size());
    for (const auto& [key, u] : uses) {
        const auto lo = static_cast<std::uint32_t>(key >> 32), hi = static_cast<std::uint32_t>(key & 0xffffffffu);
        const glm::vec3 &p0 = welded[lo], &p1 = welded[hi];
        Edge e;
        e.c0 = (2.0f * p0 + p1) / 3.0f;
        e.c1 = (p0 + 2.0f * p1) / 3.0f;
        if (u.count == 2) {
            const glm::vec3 n0 = normal_at(u.face[0], lo), n1 = normal_at(u.face[0], hi);
            if (n0 == normal_at(u.face[1], lo) && n1 == normal_at(u.face[1], hi)) {
                // PN triangle edge control points: each third point pulled onto its corner's tangent plane.
                glm::vec3 d0 = -glm::dot(p1 - p0, n0) / 3.0f * n0, d1 = -glm::dot(p0 - p1, n1) / 3.0f * n1;
                // The curve lies within max(|d0|, |d1|) of the chord (its weights 3t(1-t)^2 + 3t^2(1-t) <= 3/4).
                const float d = std::max(glm::length(d0), glm::length(d1));
                if (d > max_offset && d > 0.0f) {
                    d0 *= max_offset / d;
                    d1 *= max_offset / d;
                }
                const float deviation = 0.75f * std::max(glm::length(d0), glm::length(d1));
                if (deviation > tolerance) {
                    e.c0 += d0;
                    e.c1 += d1;
                    e.curved = true;
                    // A chord of a segment 1/n long cuts the curve by about deviation / n^2.
                    e.segments = pow2_at_least(std::sqrt(deviation / tolerance), max_segments);
                    ++stats.curved_edges;
                }
            }
        }
        edges.emplace(key, e);
    }

    // Each terrain mesh again, its ground triangles cut up.
    zone.draw_terrain.resize(zone.terrain.size());
    zone.draw_terrain_source.resize(zone.terrain.size());
    for (std::size_t mi = 0; mi < zone.terrain.size(); ++mi) {
        const Mesh& mesh = zone.terrain[mi];
        Mesh& out = zone.draw_terrain[mi];
        auto& source = zone.draw_terrain_source[mi];
        out.name = mesh.name;
        out.materials = mesh.materials;
        out.front_ccw = mesh.front_ccw;
        out.heightmap_terrain = mesh.heightmap_terrain;
        out.vertices = mesh.vertices;
        out.indices.reserve(mesh.indices.size());
        source.reserve(mesh.indices.size() / 3);
        for (const MeshPart& part : mesh.parts) {
            MeshPart np = part;
            np.first_index = static_cast<std::uint32_t>(out.indices.size());
            const std::size_t end = std::min<std::size_t>(std::size_t{part.first_index} + part.index_count, mesh.indices.size());
            for (std::size_t i = part.first_index; i + 3 <= end; i += 3) {
                const auto tri = static_cast<std::uint32_t>(i / 3);
                const std::int32_t fi = face_of[mi][tri];
                if (fi < 0) {
                    out.indices.insert(out.indices.end(), mesh.indices.begin() + static_cast<std::ptrdiff_t>(i),
                                       mesh.indices.begin() + static_cast<std::ptrdiff_t>(i + 3));
                    source.push_back(tri);
                    continue;
                }
                const Face& f = faces[static_cast<std::size_t>(fi)];
                const Vertex* v[3];
                for (int k = 0; k < 3; ++k) v[k] = &mesh.vertices[mesh.indices[i + static_cast<std::size_t>(k)]];
                const glm::vec3* n = &corner_n[static_cast<std::size_t>(fi) * 3];
                // The edges in the triangle's order, corner k to k + 1, with their control points that way round.
                const Edge* e[3];
                glm::vec3 c[3][2];
                std::uint32_t segments = 1;
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t a = f.w[k], b = f.w[(k + 1) % 3];
                    e[k] = &edges.at(edge_key(a, b));
                    c[k][0] = a < b ? e[k]->c0 : e[k]->c1;
                    c[k][1] = a < b ? e[k]->c1 : e[k]->c0;
                    segments = std::max(segments, e[k]->segments);
                }
                const glm::vec3 P[3] = {welded[f.w[0]], welded[f.w[1]], welded[f.w[2]]};
                // The PN centre point: the edges' control points' mean, moved half as far again from the corners'.
                const glm::vec3 mid = (c[0][0] + c[0][1] + c[1][0] + c[1][1] + c[2][0] + c[2][1]) / 6.0f;
                const glm::vec3 centre = mid + (mid - (P[0] + P[1] + P[2]) / 3.0f) * 0.5f;
                const float bulge = distance_to_plane(centre, P[0], f.n);
                if (bulge > tolerance) segments = std::max(segments, pow2_at_least(std::sqrt(bulge / tolerance), max_segments));
                if (segments == 1) { // flat: the file's own triangle, with the smoothed normals
                    for (int k = 0; k < 3; ++k) {
                        Vertex nv = *v[k];
                        nv.position = P[k];
                        nv.normal = n[k];
                        out.indices.push_back(static_cast<std::uint32_t>(out.vertices.size()));
                        out.vertices.push_back(nv);
                    }
                    source.push_back(tri);
                    ++stats.triangles;
                    continue;
                }
                const std::uint32_t S = segments;
                auto make_vertex = [&](float wa, float wb, float wc, const glm::vec3& pos) {
                    Vertex nv;
                    nv.position = pos;
                    const glm::vec3 nn = wa * n[0] + wb * n[1] + wc * n[2];
                    const float len = glm::length(nn);
                    nv.normal = len > 1e-6f ? nn / len : f.n;
                    nv.uv = wa * v[0]->uv + wb * v[1]->uv + wc * v[2]->uv;
                    nv.uv2 = wa * v[0]->uv2 + wb * v[1]->uv2 + wc * v[2]->uv2;
                    nv.color = lerp_color(v[0]->color, v[1]->color, v[2]->color, wa, wb, wc);
                    nv.color2 = lerp_color(v[0]->color2, v[1]->color2, v[2]->color2, wa, wb, wc);
                    const glm::vec3 flat = wa * P[0] + wb * P[1] + wc * P[2];
                    stats.max_offset = std::max(stats.max_offset, glm::length(pos - flat));
                    out.vertices.push_back(nv);
                    return static_cast<std::uint32_t>(out.vertices.size() - 1);
                };
                // The PN patch at weights (u, s, t) on corners 0, 1, 2.
                auto patch = [&](float u, float s, float t) {
                    return u * u * u * P[0] + s * s * s * P[1] + t * t * t * P[2] +
                           3.0f * (u * u * s * c[0][0] + u * s * s * c[0][1] + s * s * t * c[1][0] + s * t * t * c[1][1] +
                                   t * t * u * c[2][0] + t * u * u * c[2][1]) +
                           6.0f * u * s * t * centre;
                };
                auto emit = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2) {
                    if (i0 == i1 || i1 == i2 || i0 == i2) return;
                    out.indices.insert(out.indices.end(), {i0, i1, i2});
                    source.push_back(tri);
                    ++stats.triangles;
                };
                // The rim: the corners and each edge's own samples, corner k to k + 1. A sample is counted and
                // evaluated from the edge's lower welded corner, so the triangle on its other side makes the same
                // points bit for bit.
                std::uint32_t corner[3];
                for (int k = 0; k < 3; ++k)
                    corner[k] = make_vertex(k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f, P[k]);
                std::vector<std::uint32_t> rim[3];
                for (int k = 0; k < 3; ++k) {
                    const int k1 = (k + 1) % 3;
                    const bool from_k = f.w[k] < f.w[k1];
                    const int lo_k = from_k ? k : k1, hi_k = from_k ? k1 : k;
                    const Edge& ed = *e[k];
                    const std::uint32_t m = ed.segments;
                    rim[k].push_back(corner[k]);
                    for (std::uint32_t r = 1; r < m; ++r) {
                        const std::uint32_t sample = from_k ? r : m - r;
                        const float tt = static_cast<float>(sample) / static_cast<float>(m);
                        float w[3] = {0.0f, 0.0f, 0.0f};
                        w[lo_k] = 1.0f - tt;
                        w[hi_k] = tt;
                        rim[k].push_back(make_vertex(w[0], w[1], w[2], bezier(P[lo_k], ed.c0, ed.c1, P[hi_k], tt)));
                    }
                    rim[k].push_back(corner[k1]);
                }
                // The inside: a grid of q segments a side over the points whose weights (out of S) are all at least
                // 1; for S = 2, the centre alone.
                const std::uint32_t q = S == 2 ? 0 : S - 3;
                const float inv = 1.0f / static_cast<float>(S);
                auto inner_at = [q](std::uint32_t b, std::uint32_t cc) { return static_cast<std::size_t>(cc) * (q + 1) + b; };
                std::vector<std::uint32_t> inner(static_cast<std::size_t>(q + 1) * (q + 1), 0);
                if (S == 2) {
                    const float third = 1.0f / 3.0f;
                    inner[0] = make_vertex(third, third, third, patch(third, third, third));
                } else {
                    for (std::uint32_t cc = 0; cc <= q; ++cc)
                        for (std::uint32_t b = 0; b + cc <= q; ++b) {
                            const float u = static_cast<float>(q - b - cc + 1) * inv, s = static_cast<float>(b + 1) * inv,
                                        t = static_cast<float>(cc + 1) * inv;
                            inner[inner_at(b, cc)] = make_vertex(u, s, t, patch(u, s, t));
                        }
                    for (std::uint32_t cc = 0; cc < q; ++cc)
                        for (std::uint32_t b = 0; b + cc < q; ++b) {
                            emit(inner[inner_at(b, cc)], inner[inner_at(b + 1, cc)], inner[inner_at(b, cc + 1)]);
                            if (b + cc + 2 <= q)
                                emit(inner[inner_at(b + 1, cc)], inner[inner_at(b + 1, cc + 1)], inner[inner_at(b, cc + 1)]);
                        }
                }
                // Each edge's strip between its rim and the inner grid's side along it (same direction), zipped by
                // how far along each point lies.
                for (int k = 0; k < 3; ++k) {
                    auto side = [&](std::uint32_t j) {
                        if (k == 0) return inner[inner_at(j, 0)];
                        if (k == 1) return inner[inner_at(q - j, j)];
                        return inner[inner_at(0, q - j)];
                    };
                    const auto m = static_cast<std::uint32_t>(rim[k].size() - 1);
                    std::uint32_t r = 0, j = 0;
                    while (r < m || j < q) {
                        if (j == q || (r < m && (r + 1) * q <= (j + 1) * m)) {
                            emit(rim[k][r], rim[k][r + 1], side(j));
                            ++r;
                        } else {
                            emit(rim[k][r], side(j + 1), side(j));
                            ++j;
                        }
                    }
                }
            }
            np.index_count = static_cast<std::uint32_t>(out.indices.size()) - np.first_index;
            out.parts.push_back(np);
        }
    }
    return stats;
}

} // namespace hailstone::assets
