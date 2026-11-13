#include "texpack/trees.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <system_error>

#include "assets/gltf_mesh.h"
#include "assets/zone.h"
#include "core/game_dir.h"
#include "core/atomic_file.h"
#include "texpack/texpack.h"

namespace hailstone::texpack {

namespace {

constexpr float kPi = 3.14159265358979f;
// The drawn trunk's radius against the game's prism corners: between the prism's faces (half the corner distance for
// three sides) and its corners, so a player stopped by the prism stands about at the bark.
constexpr float kTrunkFit = 0.8f;

// A small deterministic generator, so a tree comes out the same on every platform (<random>'s distributions do not).
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e019ull) {
        if (s == 0) s = 1;
    }
    std::uint32_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<std::uint32_t>((s * 0x2545f4914f6cdd1dull) >> 32);
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
    glm::vec3 unit() {
        for (;;) {
            glm::vec3 v(range(-1, 1), range(-1, 1), range(-1, 1));
            const float d = glm::dot(v, v);
            if (d > 0.01f && d <= 1.0f) return v / std::sqrt(d);
        }
    }
};

glm::vec4 texel(const assets::Image& img, glm::vec2 uv) {
    const float u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
    const auto x = std::min(img.width - 1, static_cast<std::uint32_t>(u * float(img.width)));
    const auto y = std::min(img.height - 1, static_cast<std::uint32_t>(v * float(img.height)));
    const std::byte* p = &img.mips[0][(std::size_t(y) * img.width + x) * 4];
    return {float(std::to_integer<int>(p[0])) / 255.0f, float(std::to_integer<int>(p[1])) / 255.0f,
            float(std::to_integer<int>(p[2])) / 255.0f, float(std::to_integer<int>(p[3])) / 255.0f};
}

glm::vec3 mean_colour(const assets::Image& img) {
    glm::dvec3 sum(0.0);
    double n = 0;
    for (std::size_t i = 0; i + 3 < img.mips[0].size(); i += 4) {
        if (std::to_integer<int>(img.mips[0][i + 3]) < 128) continue;
        for (int c = 0; c < 3; ++c) sum[c] += std::to_integer<int>(img.mips[0][i + std::size_t(c)]) / 255.0;
        n += 1;
    }
    return n > 0 ? glm::vec3(sum / n) : glm::vec3(0.3f);
}

bool usable(const assets::Image* img) {
    return img && img->width && img->height && img->format == assets::PixelFormat::RGBA8 && !img->mips.empty() &&
           img->mips[0].size() >= std::size_t(img->width) * img->height * 4;
}

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    const auto k = static_cast<std::size_t>(std::clamp(p, 0.0f, 1.0f) * float(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

} // namespace

std::optional<TreeShape> measure_tree(const assets::Mesh& mesh, const TreeTextureLookup& texture, std::string* why) {
    auto fail = [&](const char* text) -> std::optional<TreeShape> {
        if (why) *why = text;
        return std::nullopt;
    };
    TreeShape shape;
    // The solid, opaque parts are the trunk; the cut-out ones the leaves.
    std::vector<int> welded; // per trunk triangle corner, an index into pts
    std::vector<glm::vec3> pts;
    struct LeafTri {
        glm::vec3 p[3];
        glm::vec2 uv[3];
        const assets::Image* img;
    };
    std::vector<LeafTri> leaves;
    glm::dvec3 bark_sum(0.0);
    double bark_n = 0;
    auto weld = [&](const glm::vec3& p) {
        for (std::size_t i = 0; i < pts.size(); ++i)
            if (glm::distance(pts[i], p) < 0.05f) return static_cast<int>(i);
        pts.push_back(p);
        return static_cast<int>(pts.size() - 1);
    };
    for (const auto& part : mesh.parts) {
        if (part.material >= mesh.materials.size()) continue;
        const auto& m = mesh.materials[part.material];
        const bool cutout = m.blend == assets::BlendMode::Masked;
        const assets::Image* img = texture ? texture(m) : nullptr;
        if (!usable(img)) img = nullptr;
        for (std::uint32_t k = 0; k + 2 < part.index_count; k += 3) {
            std::uint32_t t[3];
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                t[c] = mesh.indices[part.first_index + k + std::uint32_t(c)];
                ok &= t[c] < mesh.vertices.size();
            }
            if (!ok) continue;
            if (cutout) {
                if (!img) continue;
                LeafTri lt;
                for (int c = 0; c < 3; ++c) lt.p[c] = mesh.vertices[t[c]].position, lt.uv[c] = mesh.vertices[t[c]].uv;
                lt.img = img;
                leaves.push_back(lt);
            } else if (part.solid && m.blend == assets::BlendMode::Opaque) {
                for (int c = 0; c < 3; ++c) welded.push_back(weld(mesh.vertices[t[c]].position));
            }
        }
        if (!cutout && part.solid && img) {
            const glm::vec3 c = mean_colour(*img);
            bark_sum += glm::dvec3(c) * double(part.index_count);
            bark_n += part.index_count;
        }
    }
    if (welded.empty()) return fail("no solid trunk");
    if (leaves.empty()) return fail("no cut-out leaves");
    if (bark_n > 0) shape.bark_colour = glm::vec3(bark_sum / bark_n);

    // The trunk's skeleton: rings by their hop count from the bottom along the prisms' edges, split where a level's
    // vertices do not join (a fork), each ring's parent the ring below it shares the most edges with.
    const std::size_t n = pts.size();
    std::vector<std::vector<int>> adj(n);
    for (std::size_t t = 0; t + 2 < welded.size(); t += 3)
        for (int a = 0; a < 3; ++a) {
            const int u = welded[t + std::size_t(a)], v = welded[t + std::size_t((a + 1) % 3)];
            if (u == v) continue;
            if (std::find(adj[std::size_t(u)].begin(), adj[std::size_t(u)].end(), v) == adj[std::size_t(u)].end()) {
                adj[std::size_t(u)].push_back(v);
                adj[std::size_t(v)].push_back(u);
            }
        }
    float zmin = pts[0].z;
    for (const auto& p : pts) zmin = std::min(zmin, p.z);
    // Pieces that do not join (a limb as a prism of its own, TREETRY's second one) each start from their own bottom
    // and hang from the ring nearest their root.
    std::vector<int> piece(n, -1);
    std::vector<std::vector<int>> pieces;
    for (std::size_t i = 0; i < n; ++i) {
        if (piece[i] >= 0) continue;
        pieces.emplace_back();
        std::vector<int> stack{static_cast<int>(i)};
        piece[i] = static_cast<int>(pieces.size() - 1);
        while (!stack.empty()) {
            const int v = stack.back();
            stack.pop_back();
            pieces.back().push_back(v);
            for (int w : adj[std::size_t(v)])
                if (piece[std::size_t(w)] < 0) piece[std::size_t(w)] = piece[i], stack.push_back(w);
        }
    }
    auto piece_bottom = [&](const std::vector<int>& vs) {
        float z = pts[std::size_t(vs[0])].z;
        for (int v : vs) z = std::min(z, pts[std::size_t(v)].z);
        return z;
    };
    std::sort(pieces.begin(), pieces.end(), [&](const auto& a, const auto& b) { return piece_bottom(a) < piece_bottom(b); });
    std::vector<int> hop(n, -1), cluster(n, -1);
    for (const auto& vs : pieces) {
        if (vs.size() < 3) continue; // a stray point
        // The first ring: the vertices nearest the lowest one along the piece (a limb's rings lean with it).
        int low = vs[0];
        for (int v : vs)
            if (pts[std::size_t(v)].z < pts[std::size_t(low)].z) low = v;
        int far = low;
        for (int v : vs)
            if (glm::distance(pts[std::size_t(v)], pts[std::size_t(low)]) > glm::distance(pts[std::size_t(far)], pts[std::size_t(low)])) far = v;
        const glm::vec3 base = pts[std::size_t(low)];
        const float length = glm::distance(pts[std::size_t(far)], base);
        const glm::vec3 along = length > 1e-3f ? (pts[std::size_t(far)] - base) / length : glm::vec3(0, 0, 1);
        const float first_ring = std::max(0.6f, 0.15f * length);
        std::vector<int> queue;
        for (int v : vs)
            if (glm::dot(pts[std::size_t(v)] - base, along) <= first_ring) hop[std::size_t(v)] = 0, queue.push_back(v);
        for (std::size_t q = 0; q < queue.size(); ++q)
            for (int v : adj[std::size_t(queue[q])])
                if (hop[std::size_t(v)] < 0) hop[std::size_t(v)] = hop[std::size_t(queue[q])] + 1, queue.push_back(v);
        const std::size_t first_node = shape.trunk.size();
        std::vector<std::vector<int>> members;
        std::vector<int> cluster_hop;
        for (int i : queue) { // in hop order
            if (cluster[std::size_t(i)] >= 0) continue;
            const int id = static_cast<int>(first_node + members.size());
            members.emplace_back();
            cluster_hop.push_back(hop[std::size_t(i)]);
            std::vector<int> stack{i};
            cluster[std::size_t(i)] = id;
            while (!stack.empty()) {
                const int v = stack.back();
                stack.pop_back();
                members.back().push_back(v);
                for (int w : adj[std::size_t(v)])
                    if (cluster[std::size_t(w)] < 0 && hop[std::size_t(w)] == hop[std::size_t(v)]) {
                        cluster[std::size_t(w)] = id;
                        stack.push_back(w);
                    }
            }
        }
        for (std::size_t c = 0; c < members.size(); ++c) {
            TreeShape::Node node;
            for (int v : members[c]) node.centre += pts[std::size_t(v)];
            node.centre /= float(members[c].size());
            for (int v : members[c]) node.radius += glm::distance(pts[std::size_t(v)], node.centre);
            node.radius /= float(members[c].size());
            if (cluster_hop[c] > 0) {
                std::vector<int> votes(first_node + members.size(), 0);
                for (int v : members[c])
                    for (int w : adj[std::size_t(v)])
                        if (hop[std::size_t(w)] == cluster_hop[c] - 1) ++votes[std::size_t(cluster[std::size_t(w)])];
                node.parent = static_cast<int>(std::max_element(votes.begin(), votes.end()) - votes.begin());
            } else if (first_node > 0) {
                float best = 1e30f;
                for (std::size_t k = 0; k < first_node; ++k) {
                    const float d = glm::distance(shape.trunk[k].centre, node.centre);
                    if (d < best) best = d, node.parent = static_cast<int>(k);
                }
            }
            shape.trunk.push_back(node);
        }
    }
    if (shape.trunk.empty()) return fail("no solid trunk");

    // The leaves' shape: points spread over the cards by area, kept where the texture is opaque.
    double area = 0;
    for (const auto& lt : leaves) area += 0.5 * glm::length(glm::cross(lt.p[1] - lt.p[0], lt.p[2] - lt.p[0]));
    const double target = std::clamp(area * 6.0, 2000.0, 40000.0);
    Rng rng(0x7ee5);
    glm::dvec3 leaf_sum(0.0);
    double carry = 0;
    for (const auto& lt : leaves) {
        const double a = 0.5 * glm::length(glm::cross(lt.p[1] - lt.p[0], lt.p[2] - lt.p[0]));
        carry += a / area * target;
        for (; carry >= 1.0; carry -= 1.0) {
            float r1 = rng.uniform(), r2 = rng.uniform();
            if (r1 + r2 > 1.0f) r1 = 1.0f - r1, r2 = 1.0f - r2;
            const glm::vec3 p = lt.p[0] + r1 * (lt.p[1] - lt.p[0]) + r2 * (lt.p[2] - lt.p[0]);
            const glm::vec2 uv = lt.uv[0] + r1 * (lt.uv[1] - lt.uv[0]) + r2 * (lt.uv[2] - lt.uv[0]);
            const glm::vec4 t = texel(*lt.img, uv);
            if (t.a < 0.5f) continue;
            shape.foliage.push_back(p);
            leaf_sum += glm::dvec3(t.r, t.g, t.b);
            if (shape.leaf_palette.size() < 256 && rng.next() % 16 == 0) shape.leaf_palette.emplace_back(t.r, t.g, t.b);
        }
    }
    if (shape.foliage.size() < 50) return fail("its leaves' texture is nearly all cut out");
    shape.leaf_colour = glm::vec3(leaf_sum / double(shape.foliage.size()));
    shape.foliage_min = shape.foliage_max = shape.foliage[0];
    for (const auto& p : shape.foliage) shape.foliage_min = glm::min(shape.foliage_min, p), shape.foliage_max = glm::max(shape.foliage_max, p);

    // A pine: its trunk runs well up into the leaves and the leaves are widest low down.
    const float fh = std::max(shape.foliage_max.z - shape.foliage_min.z, 0.01f);
    float trunk_top = zmin;
    for (const auto& node : shape.trunk) trunk_top = std::max(trunk_top, node.centre.z);
    const glm::vec2 axis((shape.foliage_min.x + shape.foliage_max.x) * 0.5f, (shape.foliage_min.y + shape.foliage_max.y) * 0.5f);
    std::vector<float> low, high;
    for (const auto& p : shape.foliage) {
        const float t = (p.z - shape.foliage_min.z) / fh;
        const float r = glm::distance(glm::vec2(p), axis);
        if (t < 0.33f) low.push_back(r);
        else if (t > 0.67f) high.push_back(r);
    }
    // ... on one upright trunk (a fork or a lean is a broadleaf tree's).
    int roots = 0, tips = 0;
    std::vector<int> children(shape.trunk.size(), 0);
    for (const auto& node : shape.trunk) {
        if (node.parent < 0) ++roots;
        else ++children[std::size_t(node.parent)];
    }
    for (int c : children) tips += c == 0;
    const auto& foot = shape.trunk.front().centre;
    auto top_node = std::max_element(shape.trunk.begin(), shape.trunk.end(), [](const auto& a, const auto& b) { return a.centre.z < b.centre.z; });
    const float lean = glm::distance(glm::vec2(top_node->centre), glm::vec2(foot)) / std::max(top_node->centre.z - foot.z, 0.01f);
    shape.conifer = roots == 1 && tips == 1 && lean < 0.2f && trunk_top >= shape.foliage_min.z + 0.45f * fh &&
                    percentile(low, 0.9f) > 1.35f * percentile(high, 0.9f);
    return shape;
}

namespace {

struct Builder {
    assets::Mesh mesh;
    std::vector<std::uint32_t> idx[2]; // bark, leaves

    // The shade baked into the vertices (assets::Vertex::color, which multiplies a placement's light): set by the
    // generator per part of the tree.
    float shade = 1.0f;

    std::uint32_t vert(const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv) {
        assets::Vertex v;
        v.position = p;
        v.normal = glm::dot(n, n) > 0.0f ? glm::normalize(n) : glm::vec3(0, 0, 1);
        v.uv = uv;
        const auto g = static_cast<std::uint32_t>(std::lround(std::clamp(shade, 0.0f, 1.0f) * 255.0f));
        v.color = 0xff000000u | g << 16 | g << 8 | g;
        mesh.vertices.push_back(v);
        return static_cast<std::uint32_t>(mesh.vertices.size() - 1);
    }
    void tri(int mat, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        idx[mat].push_back(a), idx[mat].push_back(b), idx[mat].push_back(c);
    }

    // A tube along `path` (counter-clockwise outward), frames carried along it; v runs with length / `v_units`.
    void tube(const std::vector<glm::vec3>& path, const std::vector<float>& radius, int sides, float u_repeats,
              float v_units, float v_start = 0.0f) {
        if (path.size() < 2) return;
        glm::vec3 t0 = path[1] - path[0];
        if (glm::dot(t0, t0) < 1e-8f) t0 = glm::vec3(0, 0, 1);
        t0 = glm::normalize(t0);
        glm::vec3 u = std::abs(t0.z) < 0.9f ? glm::cross(glm::vec3(0, 0, 1), t0) : glm::cross(glm::vec3(1, 0, 0), t0);
        u = glm::normalize(u);
        float v_at = v_start;
        std::uint32_t prev = 0;
        for (std::size_t i = 0; i < path.size(); ++i) {
            glm::vec3 t = i + 1 < path.size() ? path[i + 1] - path[i] : path[i] - path[i - 1];
            if (i > 0 && i + 1 < path.size()) t = glm::normalize(path[i + 1] - path[i]) + glm::normalize(path[i] - path[i - 1]);
            if (glm::dot(t, t) < 1e-8f) t = t0;
            t = glm::normalize(t);
            u = u - glm::dot(u, t) * t;
            u = glm::dot(u, u) > 1e-8f ? glm::normalize(u) : glm::normalize(glm::cross(glm::vec3(0.31f, 0.52f, 0.79f), t));
            const glm::vec3 w = glm::cross(t, u);
            if (i > 0) v_at += glm::distance(path[i], path[i - 1]) / v_units;
            const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());
            for (int j = 0; j <= sides; ++j) {
                const float a = 2.0f * kPi * float(j) / float(sides);
                const glm::vec3 d = std::cos(a) * u + std::sin(a) * w;
                vert(path[i] + d * radius[i], d, {u_repeats * float(j) / float(sides), v_at});
            }
            if (i > 0)
                for (int j = 0; j < sides; ++j) {
                    const std::uint32_t a = prev + std::uint32_t(j), b = a + 1;
                    const std::uint32_t c = first + std::uint32_t(j) + 1, d = first + std::uint32_t(j);
                    tri(0, a, b, c);
                    tri(0, a, c, d);
                }
            prev = first;
        }
    }

    // A two-sided card strip: `centre` points, `side` the half-width vectors there; u across, v from v0 to v1.
    void strip(const std::vector<glm::vec3>& centre, const std::vector<glm::vec3>& side, const std::vector<glm::vec3>& normal,
               float v0, float v1) {
        std::uint32_t prev = 0;
        for (std::size_t i = 0; i < centre.size(); ++i) {
            const float v = v0 + (v1 - v0) * float(i) / float(centre.size() - 1);
            const std::uint32_t l = vert(centre[i] - side[i], normal[i], {0.0f, v});
            vert(centre[i] + side[i], normal[i], {1.0f, v});
            if (i > 0) {
                tri(1, prev, prev + 1, l + 1);
                tri(1, prev, l + 1, l);
            }
            prev = l;
        }
    }

    void finish() {
        assets::Material bark, leaf;
        bark.name = "bark";
        leaf.name = "leaves";
        leaf.blend = assets::BlendMode::Masked;
        leaf.two_sided = true;
        mesh.materials = {bark, leaf};
        for (int m = 0; m < 2; ++m) {
            if (idx[m].empty()) continue;
            assets::MeshPart part;
            part.first_index = static_cast<std::uint32_t>(mesh.indices.size());
            part.index_count = static_cast<std::uint32_t>(idx[m].size());
            part.material = static_cast<std::uint32_t>(m);
            part.solid = false;
            mesh.indices.insert(mesh.indices.end(), idx[m].begin(), idx[m].end());
            mesh.parts.push_back(part);
        }
        mesh.front_ccw = true;
    }
};

// The trunk's paths: the longest root-to-tip chain first, then each other tip from where it leaves a drawn path.
std::vector<std::vector<int>> trunk_paths(const TreeShape& shape) {
    const std::size_t n = shape.trunk.size();
    std::vector<int> children(n, 0);
    for (const auto& node : shape.trunk)
        if (node.parent >= 0) ++children[std::size_t(node.parent)];
    std::vector<int> tips;
    for (std::size_t i = 0; i < n; ++i)
        if (children[i] == 0) tips.push_back(static_cast<int>(i));
    std::sort(tips.begin(), tips.end(), [&](int a, int b) { return shape.trunk[std::size_t(a)].centre.z > shape.trunk[std::size_t(b)].centre.z; });
    std::vector<bool> drawn(n, false);
    std::vector<std::vector<int>> out;
    for (int tip : tips) {
        std::vector<int> path;
        for (int v = tip; v >= 0; v = shape.trunk[std::size_t(v)].parent) {
            path.push_back(v);
            if (drawn[std::size_t(v)]) break;
        }
        std::reverse(path.begin(), path.end());
        for (int v : path) drawn[std::size_t(v)] = true;
        if (path.size() >= 2) out.push_back(std::move(path));
    }
    return out;
}

// Drawn pictures: an RGBA8 canvas with the cut-out in alpha.
struct Canvas {
    std::uint32_t w, h;
    std::vector<glm::vec4> px;
    Canvas(std::uint32_t width, std::uint32_t height) : w(width), h(height), px(std::size_t(width) * height, glm::vec4(0.0f)) {}
    void put(int x, int y, const glm::vec3& c) {
        if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
        px[std::size_t(y) * w + std::size_t(x)] = glm::vec4(c, 1.0f);
    }
    // A thick line of `width` pixels.
    void line(glm::vec2 a, glm::vec2 b, float width, const glm::vec3& c) {
        const glm::vec2 lo = glm::min(a, b) - width, hi = glm::max(a, b) + width;
        const glm::vec2 d = b - a;
        const float len2 = std::max(glm::dot(d, d), 1e-6f);
        for (int y = std::max(0, int(lo.y)); y <= std::min(int(h) - 1, int(hi.y)); ++y)
            for (int x = std::max(0, int(lo.x)); x <= std::min(int(w) - 1, int(hi.x)); ++x) {
                const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
                const float t = std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f);
                if (glm::distance(p, a + t * d) <= width * 0.5f) put(x, y, c);
            }
    }
    assets::Image image(const glm::vec3& target) const {
        // Scaled so that its opaque texels average `target`: the game tree's own colour.
        glm::dvec3 sum(0.0);
        double n = 0;
        for (const auto& p : px)
            if (p.a > 0.5f) sum += glm::dvec3(p), n += 1;
        glm::vec3 gain(1.0f);
        if (n > 0)
            for (int c = 0; c < 3; ++c) gain[c] = sum[c] > 1e-6 ? float(target[c] * n / sum[c]) : 1.0f;
        assets::Image img;
        img.width = w, img.height = h;
        img.mips.emplace_back(std::size_t(w) * h * 4);
        for (std::size_t i = 0; i < px.size(); ++i) {
            for (int c = 0; c < 3; ++c)
                img.mips[0][i * 4 + std::size_t(c)] =
                    std::byte(static_cast<std::uint8_t>(std::lround(std::clamp(px[i][c] * gain[c], 0.0f, 1.0f) * 255.0f)));
            img.mips[0][i * 4 + 3] = std::byte(px[i].a > 0.5f ? 255 : 0);
        }
        return img;
    }
};

glm::vec3 vary(Rng& rng, float amount) {
    const float l = rng.range(1.0f - amount, 1.0f + amount);
    return glm::vec3(l * rng.range(0.94f, 1.06f), l, l * rng.range(0.9f, 1.1f));
}

// A leaf's colour: one of the game leaves' own (the mean when there are none), varied a little.
glm::vec3 pick(const std::vector<glm::vec3>& palette, const glm::vec3& mean, Rng& rng, float amount) {
    const glm::vec3 base = palette.empty() ? mean : palette[rng.next() % palette.size()];
    return base * vary(rng, amount);
}

// A spray of needles along a twig up the middle, base at v = 0 (row 0), tip at v = 1.
// `strands`: a willow's hanging strand instead, its leaves long and close along the twig.
assets::Image draw_needles(std::uint32_t size, const glm::vec3& colour, const std::vector<glm::vec3>& palette, Rng& rng,
                           bool strands = false) {
    Canvas cv(size, size);
    const float s = float(size);
    const float twig_w = s * 0.012f;
    // Side twigs first, then needles on every twig.
    struct Twig { glm::vec2 a, b; float reach; };
    std::vector<Twig> twigs{{{s * 0.5f, 0.0f}, {s * 0.5f, s * 0.97f}, 0.36f}};
    for (int i = 0; i < 7; ++i) {
        const float t = 0.12f + 0.11f * float(i);
        const float side = (i % 2) ? 1.0f : -1.0f;
        const glm::vec2 a(s * 0.5f, t * s);
        const float len = s * (0.36f - 0.25f * t) * rng.range(0.8f, 1.0f);
        twigs.push_back({a, a + glm::vec2(side * len * 0.82f, len * 0.57f), 0.2f});
    }
    // Two layers: shadowed needles underneath, lit ones over them. Needles a few texels wide, close together, so the
    // spray keeps its cover in the smaller mips (a cut-out thins as it is filtered down).
    for (int layer = 0; layer < 2; ++layer)
        for (const auto& tw : twigs) {
            const glm::vec2 d = tw.b - tw.a;
            const float len = glm::length(d);
            const glm::vec2 dir = d / len, perp(-dir.y, dir.x);
            for (float t = 0.0f; t < len; t += s * (layer ? 0.006f : 0.009f)) {
                const glm::vec2 p = tw.a + dir * t;
                const float taper = 1.0f - 0.5f * t / len;
                for (float side : {-1.0f, 1.0f}) {
                    const float angle = strands ? rng.range(0.2f, 0.45f) : rng.range(0.55f, 1.05f);
                    const glm::vec2 nd = glm::normalize(perp * side * std::sin(angle) + dir * std::cos(angle));
                    const float nl = s * tw.reach * (layer ? 0.33f : 0.36f) * (strands ? 1.6f : 1.0f) * taper * rng.range(0.75f, 1.1f);
                    const glm::vec3 c = pick(palette, colour, rng, 0.12f) * (layer ? (side > 0 ? 1.12f : 0.95f) : 0.55f);
                    cv.line(p, p + nd * nl, std::max(2.0f, s * (layer ? 0.007f : 0.011f) * (strands ? 1.8f : 1.0f)), c);
                }
            }
        }
    for (const auto& tw : twigs) cv.line(tw.a, tw.b, twig_w, glm::vec3(0.32f, 0.24f, 0.17f));
    return cv.image(colour);
}

// A cluster of leaves on a few twigs from the bottom middle.
assets::Image draw_leaves(std::uint32_t size, const glm::vec3& colour, const std::vector<glm::vec3>& palette, Rng& rng) {
    Canvas cv(size, size);
    const float s = float(size);
    struct Leaf { glm::vec2 at, dir; float len; };
    std::vector<Leaf> list;
    std::vector<std::pair<glm::vec2, glm::vec2>> twigs;
    for (int i = 0; i < 5; ++i) {
        const float a = -0.9f + 0.45f * float(i) + rng.range(-0.1f, 0.1f);
        const glm::vec2 base(s * 0.5f, s * 0.98f), dir(std::sin(a), -std::cos(a));
        const glm::vec2 tip = base + dir * s * rng.range(0.55f, 0.75f);
        twigs.emplace_back(base, tip);
        for (float t = 0.25f; t <= 1.0f; t += 0.13f) {
            const glm::vec2 p = base + (tip - base) * t;
            for (float side : {-1.0f, 1.0f}) {
                const float la = a + side * rng.range(0.5f, 1.1f);
                list.push_back({p, glm::vec2(std::sin(la), -std::cos(la)), s * rng.range(0.12f, 0.17f)});
            }
        }
        list.push_back({tip, dir, s * 0.16f});
    }
    for (const auto& [a, b] : twigs) cv.line(a, b, s * 0.012f, glm::vec3(0.3f, 0.24f, 0.17f));
    for (const auto& lf : list) {
        const glm::vec2 perp(-lf.dir.y, lf.dir.x);
        const float half = lf.len * 0.24f;
        const glm::vec3 base = pick(palette, colour, rng, 0.12f);
        const glm::vec2 lo = glm::min(lf.at, lf.at + lf.dir * lf.len) - lf.len * 0.3f;
        const glm::vec2 hi = glm::max(lf.at, lf.at + lf.dir * lf.len) + lf.len * 0.3f;
        for (int y = std::max(0, int(lo.y)); y <= std::min(int(size) - 1, int(hi.y)); ++y)
            for (int x = std::max(0, int(lo.x)); x <= std::min(int(size) - 1, int(hi.x)); ++x) {
                const glm::vec2 p = glm::vec2(float(x) + 0.5f, float(y) + 0.5f) - lf.at;
                const float along = glm::dot(p, lf.dir) / lf.len, across = glm::dot(p, perp);
                if (along < 0.0f || along > 1.0f) continue;
                const float width = half * std::pow(std::sin(kPi * along), 0.75f);
                if (std::abs(across) > width) continue;
                const float edge = std::abs(across) / std::max(width, 1e-3f);
                float shade = (across > 0 ? 1.06f : 0.88f) * (1.0f - 0.22f * edge * edge);
                if (std::abs(across) < std::max(0.8f, s * 0.0025f)) shade *= 1.18f; // the midrib
                cv.put(x, y, base * shade);
            }
    }
    return cv.image(colour);
}

// A drawn bark, without a photo: furrows down the trunk.
assets::Image draw_bark(std::uint32_t size, const glm::vec3& colour, Rng& rng) {
    Canvas cv(size, size);
    std::vector<float> furrow(size);
    for (std::uint32_t x = 0; x < size; ++x) {
        const float a = 2.0f * kPi * float(x) / float(size);
        furrow[x] = 0.5f + 0.25f * std::sin(a * 7.0f) + 0.15f * std::sin(a * 13.0f + 1.3f) + 0.1f * std::sin(a * 29.0f + 0.4f);
    }
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const float n = rng.range(0.85f, 1.15f);
            cv.px[std::size_t(y) * size + x] = glm::vec4(colour * (0.55f + 0.7f * furrow[x]) * n, 1.0f);
        }
    return cv.image(colour);
}

assets::Image planes_image(const Planes& p, const glm::vec3& gain) {
    assets::Image img;
    img.width = p.width, img.height = p.height;
    img.mips.emplace_back(std::size_t(p.width) * p.height * 4);
    for (std::uint32_t y = 0; y < p.height; ++y)
        for (std::uint32_t x = 0; x < p.width; ++x) {
            const std::size_t o = (std::size_t(y) * p.width + x) * 4;
            for (std::uint32_t c = 0; c < 3; ++c) {
                const float v = p.at(x, y, std::min(c, p.channels - 1)) * gain[int(c)];
                img.mips[0][o + c] = std::byte(static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
            }
            img.mips[0][o + 3] = std::byte{255};
        }
    return img;
}

} // namespace

std::optional<TreeStyle> tree_style_from_name(std::string_view name) {
    if (name == "auto") return TreeStyle::Auto;
    if (name == "pine") return TreeStyle::Pine;
    if (name == "broadleaf") return TreeStyle::Broadleaf;
    if (name == "willow") return TreeStyle::Willow;
    return std::nullopt;
}

GeneratedTree generate_tree(const TreeShape& shape, const TreeOptions& options) {
    GeneratedTree out;
    const bool pine = options.style == TreeStyle::Pine || (options.style == TreeStyle::Auto && shape.conifer);
    const bool willow = options.style == TreeStyle::Willow;
    Builder b;
    Rng rng(options.seed);
    const glm::vec3 up(0, 0, 1);
    const glm::vec3 fmin = shape.foliage_min, fmax = shape.foliage_max;
    const float fh = std::max(fmax.z - fmin.z, 0.01f);
    const float bark_units = static_cast<float>(std::max(options.bark_units, 0.5));
    auto bark_sides = [](float r) { return r > 2.0f ? 10 : r > 0.8f ? 7 : r > 0.35f ? 4 : 3; };
    auto around = [&](float r) { return std::max(1.0f, std::round(2.0f * kPi * r / bark_units)); };

    // The trunk along the game's prisms.
    const auto paths = trunk_paths(shape);
    std::vector<glm::vec3> leader;   // the main path's points (with a pine's leader above them)
    std::vector<float> leader_r;
    for (std::size_t pi = 0; pi < paths.size(); ++pi) {
        std::vector<glm::vec3> path;
        std::vector<float> r;
        for (int v : paths[pi]) {
            path.push_back(shape.trunk[std::size_t(v)].centre);
            r.push_back(shape.trunk[std::size_t(v)].radius * kTrunkFit);
        }
        if (pi == 0) {
            // A pine's leader goes on up through its crown from the last ring under the tip.
            if (pine && path.size() >= 2 && r.back() < 0.25f * r.front()) {
                path.pop_back(), r.pop_back();
                const glm::vec3 from = path.back();
                const float r0 = r.back();
                const glm::vec3 top(from.x, from.y, std::max(from.z + 1.0f, fmax.z - fh * 0.02f));
                for (int k = 1; k <= 6; ++k) {
                    const float t = float(k) / 6.0f;
                    path.push_back(glm::mix(from, top, t));
                    r.push_back(std::max(r0 * (1.0f - t), 0.04f));
                }
            }
            leader = path, leader_r = r;
        }
        const float r_mid = r[r.size() / 2];
        b.tube(path, r, bark_sides(r.front()), around(r_mid), bark_units);
    }
    // A point on the leader at height z.
    auto axis_at = [&](float z) {
        if (leader.empty()) return glm::vec3(0, 0, z);
        if (z <= leader.front().z) return leader.front();
        for (std::size_t i = 1; i < leader.size(); ++i)
            if (z <= leader[i].z) {
                const float t = (z - leader[i - 1].z) / std::max(leader[i].z - leader[i - 1].z, 1e-4f);
                return glm::mix(leader[i - 1], leader[i], t);
            }
        return leader.back();
    };

    const glm::vec3 centre = (fmin + fmax) * 0.5f;
    if (pine) {
        // How far the needles reach at each height: a high percentile of the leaves' distance from the leader.
        constexpr int kBands = 24;
        std::array<std::vector<float>, kBands> dist;
        for (const auto& p : shape.foliage) {
            const int band = std::clamp(int((p.z - fmin.z) / fh * kBands), 0, kBands - 1);
            dist[std::size_t(band)].push_back(glm::distance(glm::vec2(p), glm::vec2(axis_at(p.z))));
        }
        std::array<float, kBands> reach{};
        for (int i = 0; i < kBands; ++i) reach[std::size_t(i)] = percentile(dist[std::size_t(i)], 0.92f);
        auto reach_at = [&](float z) {
            const float f = std::clamp((z - fmin.z) / fh * kBands - 0.5f, 0.0f, float(kBands - 1));
            const int i = std::min(int(f), kBands - 2);
            return glm::mix(reach[std::size_t(i)], reach[std::size_t(i + 1)], f - float(i));
        };
        const int whorls = std::clamp(int(fh / 2.0f), 12, 26);
        for (int k = 0; k < whorls; ++k) {
            const float t = (float(k) + 0.5f + rng.range(-0.3f, 0.3f)) / float(whorls);
            const float z = fmin.z + fh * (0.03f + 0.9f * t);
            const int count = 5 + int(rng.next() % 4);
            for (int j = 0; j < count; ++j) {
                const float az = float(k) * 2.4f + 2.0f * kPi * float(j) / float(count) + rng.range(-0.4f, 0.4f);
                // Branches start above where their needles reach to (they droop into the measured shape).
                const float len = reach_at(z - fh * 0.04f) * rng.range(0.72f, 1.02f);
                if (len < 0.8f) continue;
                const glm::vec3 dir(std::cos(az), std::sin(az), 0.0f);
                // Low branches droop, the young ones near the top reach up.
                const float droop = len * (rng.range(0.15f, 0.35f) - 0.4f * t * t);
                const glm::vec3 p0 = axis_at(z);
                std::vector<glm::vec3> path{p0, p0 + dir * len * 0.5f - up * droop * 0.45f, p0 + dir * len - up * droop * 0.8f};
                // Shade: the sprays deep in the crown and low down darker.
                b.shade = 0.55f + 0.45f * std::min(1.0f, 0.4f + 0.6f * t);
                const float r0 = std::max(0.04f * len, 0.08f);
                b.tube(path, {r0, r0 * 0.6f, r0 * 0.25f}, 3, 1.0f, bark_units);
                // Two crossed needle sprays along the branch.
                const glm::vec3 flat = glm::normalize(glm::cross(up, dir));
                const float twist = rng.range(0.85f, 1.15f);
                for (float tilt : {rng.range(-0.15f, 0.15f), twist, -twist}) {
                    std::vector<glm::vec3> c, side, nrm;
                    for (int s = 0; s <= 3; ++s) {
                        const float u = 0.06f + 0.94f * float(s) / 3.0f;
                        const glm::vec3 p = u < 0.5f ? glm::mix(path[0], path[1], u * 2.0f) : glm::mix(path[1], path[2], u * 2.0f - 1.0f);
                        const float half = len * (0.36f - 0.16f * u);
                        const glm::vec3 axis_dir = glm::normalize(path[2] - path[0]);
                        const glm::vec3 sd = flat * std::cos(tilt) + glm::cross(axis_dir, flat) * std::sin(tilt);
                        c.push_back(p);
                        side.push_back(sd * half);
                        nrm.push_back(glm::normalize(p - axis_at(p.z)) * 0.6f + up * 0.8f);
                    }
                    b.strip(c, side, nrm, 0.0f, 1.0f);
                }
                // A clump at the branch's end, so the crown reads as pads rather than single fronds.
                const glm::vec3 tip = path[2];
                const glm::vec3 out_dir = glm::normalize(dir + up * 0.3f);
                const float half = len * rng.range(0.22f, 0.3f);
                const glm::vec3 a = glm::normalize(glm::cross(up, out_dir)), c2 = glm::normalize(glm::cross(out_dir, a));
                for (const glm::vec3& across : {a, c2}) {
                    const glm::vec3 along = glm::normalize(glm::cross(across, out_dir));
                    const glm::vec3 nrm = glm::normalize(out_dir + up);
                    b.strip({tip - along * half, tip + along * half}, {across * half, across * half}, {nrm, nrm}, 0.0f, 1.0f);
                }
            }
        }
        b.shade = 1.0f;
        // The crown: sprays round the top of the leader.
        const glm::vec3 top = leader.empty() ? glm::vec3(centre.x, centre.y, fmax.z) : leader.back();
        for (int j = 0; j < 4; ++j) {
            const float az = kPi * float(j) / 4.0f;
            const glm::vec3 sd(std::cos(az), std::sin(az), 0.0f);
            const float h = fh * 0.12f;
            std::vector<glm::vec3> c{top - up * h, top - up * h * 0.5f, top};
            std::vector<glm::vec3> side{sd * h * 0.35f, sd * h * 0.25f, sd * h * 0.08f};
            std::vector<glm::vec3> nrm{sd + up, sd + up, up};
            b.strip(c, side, nrm, 0.0f, 1.0f);
        }
        Rng tex(options.seed * 31u + 7u);
        out.leaf = draw_needles(options.leaf_size, shape.leaf_colour, shape.leaf_palette, tex);
    } else {
        // Space colonisation from the trunk into the leaves' shape.
        struct Node {
            glm::vec3 pos;
            int parent;
            float radius = 0.0f;
            bool fixed = false;
            int children = 0;
        };
        std::vector<Node> nodes;
        const float height = fmax.z - (leader.empty() ? fmin.z : leader.front().z);
        const float step = std::max(0.5f, height / 30.0f);
        for (const auto& path : paths) {
            int prev = -1;
            for (std::size_t i = 0; i < path.size(); ++i) {
                const auto& tn = shape.trunk[std::size_t(path[i])];
                if (i > 0) {
                    const auto& pn = shape.trunk[std::size_t(path[i - 1])];
                    const int pieces = std::max(1, int(glm::distance(pn.centre, tn.centre) / step));
                    for (int k = 1; k < pieces; ++k) {
                        const float t = float(k) / float(pieces);
                        nodes.push_back({glm::mix(pn.centre, tn.centre, t), prev, glm::mix(pn.radius, tn.radius, t) * kTrunkFit, true});
                        prev = static_cast<int>(nodes.size() - 1);
                    }
                }
                nodes.push_back({tn.centre, prev, tn.radius * kTrunkFit, true});
                prev = static_cast<int>(nodes.size() - 1);
            }
        }
        const std::size_t fixed_count = nodes.size();
        std::vector<glm::vec3> attract;
        const std::size_t want = 1200;
        const std::size_t stride = std::max<std::size_t>(1, shape.foliage.size() / want);
        // The cards are planes through the trunk: each point turned round it at its height and distance, so the
        // points fill the crown the cards outline.
        const glm::vec2 hub((fmin.x + fmax.x) * 0.5f, (fmin.y + fmax.y) * 0.5f);
        for (std::size_t i = rng.next() % stride; i < shape.foliage.size(); i += stride) {
            const glm::vec3& p = shape.foliage[i];
            const float a = rng.range(0.0f, 2.0f * kPi), r = glm::distance(glm::vec2(p), hub);
            attract.emplace_back(hub.x + r * std::cos(a), hub.y + r * std::sin(a), p.z);
        }
        const float extent = std::max({fmax.x - fmin.x, fmax.y - fmin.y, fh});
        const float influence = extent * 0.45f, kill = step * 1.7f;
        for (int iter = 0; iter < 200 && !attract.empty(); ++iter) {
            std::vector<glm::vec3> pull(nodes.size(), glm::vec3(0.0f));
            std::vector<int> pulled(nodes.size(), 0);
            for (const auto& a : attract) {
                int best = -1;
                float best_d = influence;
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    const float d = glm::distance(nodes[i].pos, a);
                    if (d < best_d) best_d = d, best = static_cast<int>(i);
                }
                if (best < 0) continue;
                pull[std::size_t(best)] += (a - nodes[std::size_t(best)].pos) / std::max(best_d, 1e-4f);
                ++pulled[std::size_t(best)];
            }
            const std::size_t before = nodes.size();
            for (std::size_t i = 0; i < before; ++i) {
                if (!pulled[i]) continue;
                glm::vec3 d = pull[i] / float(pulled[i]) + up * 0.12f;
                if (glm::dot(d, d) < 1e-6f) continue;
                d = glm::normalize(d);
                nodes.push_back({nodes[i].pos + d * step, static_cast<int>(i)});
            }
            if (nodes.size() == before) break;
            std::erase_if(attract, [&](const glm::vec3& a) {
                for (std::size_t i = before; i < nodes.size(); ++i)
                    if (glm::distance(nodes[i].pos, a) < kill) return true;
                return false;
            });
        }
        // Radii by the pipe model from the tips down; the trunk keeps the game's.
        for (const auto& nd : nodes)
            if (nd.parent >= 0) ++nodes[std::size_t(nd.parent)].children;
        const float tip_r = std::max(0.06f, height * 0.004f);
        std::vector<float> acc(nodes.size(), 0.0f);
        for (std::size_t i = nodes.size(); i-- > fixed_count;) {
            Node& nd = nodes[i];
            nd.radius = nd.children ? std::pow(acc[i], 1.0f / 2.5f) : tip_r;
            if (nd.parent >= 0) {
                const float cap = nodes[std::size_t(nd.parent)].fixed ? nodes[std::size_t(nd.parent)].radius * 0.7f : 1e9f;
                nd.radius = std::min(nd.radius, cap);
                acc[std::size_t(nd.parent)] += std::pow(nd.radius, 2.5f);
            }
        }
        // Branch chains: from a node whose parent forks (or is the trunk) along single children.
        std::vector<std::vector<int>> kids(nodes.size());
        for (std::size_t i = fixed_count; i < nodes.size(); ++i) kids[std::size_t(nodes[i].parent)].push_back(static_cast<int>(i));
        std::vector<int> depth_to_tip(nodes.size(), 0);
        for (std::size_t i = nodes.size(); i-- > fixed_count;)
            if (nodes[i].parent >= int(fixed_count))
                depth_to_tip[std::size_t(nodes[i].parent)] = std::max(depth_to_tip[std::size_t(nodes[i].parent)], depth_to_tip[i] + 1);
        for (std::size_t i = fixed_count; i < nodes.size(); ++i) {
            const int parent = nodes[i].parent;
            if (parent >= int(fixed_count) && kids[std::size_t(parent)].size() == 1) continue; // inside a chain
            std::vector<glm::vec3> path{nodes[std::size_t(parent)].pos};
            std::vector<float> r{nodes[i].radius * 1.1f};
            for (int v = static_cast<int>(i);;) {
                path.push_back(nodes[std::size_t(v)].pos);
                r.push_back(nodes[std::size_t(v)].radius);
                if (kids[std::size_t(v)].size() != 1) break;
                v = kids[std::size_t(v)][0];
            }
            if (r.front() < 0.12f) continue; // twigs: hidden in the leaves
            b.shade = path.front().z > fmin.z ? 0.6f : 1.0f;
            b.tube(path, r, bark_sides(r.front()), 1.0f, bark_units);
        }
        // A willow's strands hang from its branch tips in the upper crown, down towards the bottom of its leaves.
        if (willow) {
            for (std::size_t i = fixed_count; i < nodes.size(); ++i) {
                if (depth_to_tip[i] > 1 || nodes[i].pos.z < fmin.z + 0.35f * fh || rng.uniform() > 0.8f) continue;
                const glm::vec3 top = nodes[i].pos;
                const float len = std::min(fh * rng.range(0.35f, 0.65f), top.z - fmin.z + fh * 0.05f);
                const float half = extent * rng.range(0.035f, 0.05f);
                const float az = rng.range(0.0f, kPi);
                glm::vec3 outward = top - centre;
                outward.z = 0.0f;
                outward = glm::dot(outward, outward) > 1e-4f ? glm::normalize(outward) : glm::vec3(1, 0, 0);
                b.shade = std::clamp(0.55f + 0.45f * (top.z - fmin.z) / fh, 0.5f, 1.0f);
                for (float turn : {0.0f, kPi * 0.5f}) {
                    const glm::vec3 sd(std::cos(az + turn), std::sin(az + turn), 0.0f);
                    // A slight outward sway, as the strands hang off the crown's edge.
                    std::vector<glm::vec3> c{top, top - up * len * 0.5f + outward * len * 0.06f, top - up * len + outward * len * 0.08f};
                    const glm::vec3 nrm = glm::normalize(outward + up * 0.3f);
                    b.strip(c, {sd * half, sd * half, sd * half * 0.7f}, {nrm, nrm, nrm}, 0.0f, 1.0f);
                }
            }
            Rng tex(options.seed * 31u + 7u);
            out.leaf = draw_needles(options.leaf_size, shape.leaf_colour, shape.leaf_palette, tex, true);
        }
        // Leaf clusters round the branch tips.
        const float card = extent * 0.2f;
        for (std::size_t i = fixed_count; !willow && i < nodes.size(); ++i) {
            const int d = depth_to_tip[i];
            const float chance = d == 0 ? 1.0f : d == 1 ? 0.8f : d == 2 ? 0.5f : d == 3 ? 0.25f : 0.0f;
            if (rng.uniform() >= chance) continue;
            glm::vec3 outward = nodes[i].pos - centre;
            outward = glm::dot(outward, outward) > 1e-4f ? glm::normalize(outward) : up;
            const glm::vec3 n = glm::normalize(outward + rng.unit() * 0.9f);
            const glm::vec3 a = glm::normalize(glm::cross(std::abs(n.z) < 0.95f ? up : glm::vec3(1, 0, 0), n));
            const glm::vec3 c = glm::cross(n, a);
            const float s = card * rng.range(0.8f, 1.2f) * 0.5f;
            const glm::vec3 mid = nodes[i].pos + outward * s * 0.4f;
            const glm::vec3 nrm = glm::normalize(outward * 0.75f + n * 0.25f);
            // Shade: darker towards the crown's middle and its underside.
            const glm::vec3 rel = (mid - centre) / glm::max((fmax - fmin) * 0.5f, glm::vec3(0.01f));
            b.shade = std::clamp(0.5f + 0.35f * std::min(glm::length(rel), 1.0f) + 0.15f * std::clamp(rel.z, -1.0f, 1.0f), 0.4f, 1.0f);
            b.strip({mid - c * s, mid + c * s}, {a * s, a * s}, {nrm, nrm}, 1.0f, 0.0f);
        }
        if (!willow) {
            Rng tex(options.seed * 31u + 7u);
            out.leaf = draw_leaves(options.leaf_size, shape.leaf_colour, shape.leaf_palette, tex);
        }
    }

    // Bark: the photo scaled to the game bark's colour, else drawn.
    if (options.bark) {
        const auto& maps = *options.bark;
        glm::vec3 gain(1.0f);
        for (int c = 0; c < 3; ++c) gain[c] = maps.mean[c] > 1e-4f ? shape.bark_colour[c] / maps.mean[c] : 1.0f;
        out.bark = planes_image(maps.diff, gain);
        out.bark_normal = planes_image(maps.nor, glm::vec3(1.0f));
        out.bark_rough = planes_image(maps.rough, glm::vec3(1.0f));
    } else {
        Rng tex(options.seed * 17u + 3u);
        out.bark = draw_bark(options.bark_size, shape.bark_colour, tex);
    }

    b.finish();
    b.mesh.materials[0].frames = {options.bark_name};
    b.mesh.materials[1].frames = {options.leaf_name};
    out.mesh = std::move(b.mesh);
    return out;
}

std::string tree_texture_name(const std::string& actor, const char* what) {
    std::string a;
    for (char ch : actor) a += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (a.ends_with("_actordef")) a.resize(a.size() - 9);
    return "hstree_" + a + "_" + what + ".dds";
}

bool write_tree(const std::filesystem::path& pack_dir, const std::string& zone, const std::string& actor,
                const GeneratedTree& tree, std::uint64_t source_hash, std::string* error) {
    std::string z;
    for (char ch : zone) z += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::filesystem::path dir = pack_dir / "models" / z;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto put = [&](const std::filesystem::path& file, const std::vector<std::byte>& bytes) {
        if (write_file_atomic(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) return true;
        if (error) *error = "cannot write " + file.string();
        return false;
    };
    std::string stem = tree_texture_name(actor, "");
    stem = stem.substr(7, stem.size() - 7 - 5); // the actor's short name
    if (!put(dir / (stem + ".glb"), assets::write_glb(tree.mesh, source_hash))) return false;
    init_encoders();
    auto texture = [&](const assets::Image& img, FileKind kind, const char* what, const char* suffix) {
        if (img.width == 0) return true;
        std::string name = tree_texture_name(actor, what);
        name = name.substr(0, name.size() - 4) + suffix + ".dds";
        return put(dir / name, encode_pack_file(img, kind, false));
    };
    return texture(tree.leaf, FileKind::Masked, "leaf", "") && texture(tree.bark, FileKind::Color, "bark", "") &&
           texture(tree.bark_normal, FileKind::Normal, "bark", ".n") && texture(tree.bark_rough, FileKind::Rough, "bark", ".r");
}

std::size_t build_trees(const MaterialRules& rules, const GameDir& game, const std::filesystem::path& pack_dir,
                        MaterialLibrary* library, const TreeBuildOptions& options, std::vector<std::string>* notes) {
    std::map<std::string, std::vector<const TreeRule*>> by_zone;
    for (const TreeRule& t : rules.trees) by_zone[t.zone].push_back(&t);
    std::size_t written = 0, done = 0;
    for (const auto& [zone_name, list] : by_zone) {
        if (options.cancelled && options.cancelled()) break;
        const assets::Zone zone = assets::load_zone(game, zone_name);
        // The game textures as the meshes' UVs read them, decoded once a zone.
        std::map<std::string, assets::Image> images;
        auto lookup = [&](const assets::Material& m) -> const assets::Image* {
            if (m.frames.empty()) return nullptr;
            const bool masked = m.blend == assets::BlendMode::Masked;
            const std::string key = m.frames[0] + (masked ? "#m" : "");
            if (auto it = images.find(key); it != images.end()) return &it->second;
            auto data = zone.read_file(m.frames[0]);
            if (!data) return nullptr;
            auto img = assets::decode_texture(*data, {.palette_index0_transparent = masked, .wld_bmp_rows = true});
            if (!img) return nullptr;
            assets::Image rgba = assets::to_rgba8(*img);
            rgba.mips.resize(1);
            return &images.emplace(key, std::move(rgba)).first->second;
        };
        for (const TreeRule* rule : list) {
            if (options.cancelled && options.cancelled()) break;
            if (options.progress) options.progress(done, rules.trees.size());
            ++done;
            auto note = [&](const std::string& why) {
                if (notes) notes->push_back(zone_name + " " + rule->actor + ": " + why);
            };
            const assets::Mesh* mesh = nullptr;
            std::string actor;
            for (const auto& [name, m] : zone.objects) {
                std::string l = name;
                for (char& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (l == rule->actor || l == rule->actor + "_actordef") mesh = &m, actor = name;
            }
            if (!mesh) {
                note("no such object in the zone");
                continue;
            }
            std::string why;
            auto shape = measure_tree(*mesh, lookup, &why);
            if (!shape) {
                note(why);
                continue;
            }
            TreeOptions opt;
            // The seed from the names (FNV-1a), so a tree comes out the same on every build and platform.
            std::uint32_t h = 2166136261u;
            for (char ch : zone_name + "/" + rule->actor) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
            opt.seed = h;
            opt.style = tree_style_from_name(rule->style).value_or(TreeStyle::Auto);
            opt.leaf_size = options.leaf_size;
            opt.bark_size = options.bark_size;
            if (library && !rule->bark.empty())
                if (const MaterialClass* cls = rules.find(rule->bark)) {
                    opt.bark = library->tiled(rule->bark, 1, options.bark_size);
                    opt.bark_units = cls->metres * cls->magnify / 0.3048; // EQ's unit about a foot (docs/graphics.md)
                }
            opt.bark_name = tree_texture_name(actor, "bark");
            opt.leaf_name = tree_texture_name(actor, "leaf");
            const GeneratedTree tree = generate_tree(*shape, opt);
            if (!write_tree(pack_dir, zone_name, actor, tree, assets::object_mesh_hash(*mesh), &why)) {
                note(why);
                continue;
            }
            ++written;
        }
    }
    if (options.progress) options.progress(done, rules.trees.size());
    return written;
}

} // namespace hailstone::texpack
