#include "game/collision.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "assets/zone.h"
#include "game/animated_objects.h"
#include "game/mechanics/fpu.h"
#include "game/zone_ladders.h"

namespace hailstone::game {

namespace {

// The material side of a triangle record's bit 1 ("no material, or a
// transparent one"): here every surface that is not drawn opaque or masked
// and is not an invisible collision surface.
bool transparent(assets::BlendMode b) {
    using assets::BlendMode;
    return !(b == BlendMode::Opaque || b == BlendMode::Masked || b == BlendMode::Invisible);
}

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// 0x1001fa60: (t[a] + t[b] + t[c]) * 0.33333299 (0x101338cc),
// t[i] = i / 256 (CRender +0x85a0), at the game's 24-bit precision; the
// caller stores it to a float.
float vertex_alpha_light(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const mechanics::Fpu fpu;
    auto t = [](std::uint32_t colour) { return static_cast<double>(static_cast<float>(colour >> 24) / 256.0f); };
    double sum = fpu.add(fpu.add(t(a), t(b)), t(c));
    return static_cast<float>(fpu.mul(sum, static_cast<double>(std::bit_cast<float>(0x3eaaaa9fu))));
}

// The height of the triangle's plane at q when q lies inside it in XY (not for vertical or degenerate ones).
std::optional<float> height_in(const CollisionTriangle& tri, const glm::vec2& q) {
    const glm::vec2 a(tri.v0), b(tri.v1), c(tri.v2);
    const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::abs(d) < 1e-6f) return std::nullopt;
    const float u = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / d;
    const float v = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / d;
    const float w = 1.0f - u - v;
    if (u < 0.0f || v < 0.0f || w < 0.0f) return std::nullopt;
    return u * tri.v0.z + v * tri.v1.z + w * tri.v2.z;
}

// Which triangles a query has met, for the queries that walk several grid cells (a triangle is listed in each cell
// its box overlaps). A stamp per triangle, kept per thread (the neighbour zones' loader queries its meshes off the
// main thread) and grown once to the largest mesh, so a query neither allocates nor clears the zone's count; a
// query started from inside another's callback on the same thread gets its own marks.
class SeenTriangles {
public:
    explicit SeenTriangles(std::size_t count) : marks_(marks()), nested_(marks_.busy) {
        if (nested_) {
            local_.assign(count, false);
            return;
        }
        marks_.busy = true;
        if (marks_.stamp.size() < count) marks_.stamp.resize(count, 0);
        if (++marks_.generation == 0) { // wrapped: old stamps could match again
            std::fill(marks_.stamp.begin(), marks_.stamp.end(), 0u);
            marks_.generation = 1;
        }
    }
    ~SeenTriangles() {
        if (!nested_) marks_.busy = false;
    }
    SeenTriangles(const SeenTriangles&) = delete;
    SeenTriangles& operator=(const SeenTriangles&) = delete;

    // True the first time t is asked.
    bool first(std::uint32_t t) {
        if (nested_) {
            if (local_[t]) return false;
            local_[t] = true;
            return true;
        }
        if (marks_.stamp[t] == marks_.generation) return false;
        marks_.stamp[t] = marks_.generation;
        return true;
    }

private:
    struct Marks {
        std::vector<std::uint32_t> stamp;
        std::uint32_t generation = 0;
        bool busy = false;
    };
    static Marks& marks() {
        thread_local Marks m;
        return m;
    }
    Marks& marks_;
    bool nested_;
    std::vector<bool> local_;
};

} // namespace

CollisionMesh CollisionMesh::build(const assets::Zone& zone) {
    return build_scene(zone, false);
}

CollisionMesh CollisionMesh::build_visual(const assets::Zone& zone) {
    return build_scene(zone, true);
}

CollisionMesh CollisionMesh::build_scene(const assets::Zone& zone, bool visual) {
    CollisionMesh m;
    // `moving`: an animated object's vertex nodes; its moving triangles are game::AnimatedObjects', not ours.
    auto add_mesh = [&](const assets::Mesh& mesh, const glm::mat4& transform, bool terrain, bool ladder,
                        const std::vector<std::uint16_t>* moving = nullptr) {
        std::vector<glm::vec3> world(mesh.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
            world[i] = glm::vec3(transform * glm::vec4(mesh.vertices[i].position, 1.0f));
        for (const auto& part : mesh.parts) {
            std::uint8_t flags = part.solid ? 0 : kTrianglePassable;
            if (!terrain) flags |= kTriangleObject;
            if (ladder) flags |= kTriangleLadder;
            if (part.material >= mesh.materials.size() || transparent(mesh.materials[part.material].blend))
                flags |= kTriangleTransparent;
            if (part.material < mesh.materials.size() &&
                mesh.materials[part.material].blend == assets::BlendMode::Invisible)
                flags |= kTriangleInvisible;
            if (part.material >= mesh.materials.size() || !visual_surface_material(mesh.materials[part.material]))
                flags |= kTriangleNoVisualSurface;
            for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
                // A part reaching past the index list (a damaged file) ends there.
                if (std::size_t{part.first_index} + i + 2 >= mesh.indices.size()) break;
                auto ia = mesh.indices[part.first_index + i], ib = mesh.indices[part.first_index + i + 1],
                     ic = mesh.indices[part.first_index + i + 2];
                if (ia >= world.size() || ib >= world.size() || ic >= world.size()) continue;
                if (moving && moving_triangle_node(*moving, ia, ib, ic) != assets::kStillVertex) continue;
                if (!finite(world[ia]) || !finite(world[ib]) || !finite(world[ic])) continue;
                if (mesh.front_ccw) std::swap(ib, ic);
                const auto triangle = CollisionTriangle::make(world[ia], world[ib], world[ic], flags);
                if (visual && !visual_surface(triangle)) continue;
                m.triangles_.push_back(triangle);
                m.light_.push_back(terrain ? vertex_alpha_light(mesh.vertices[ia].color, mesh.vertices[ib].color,
                                                                mesh.vertices[ic].color)
                                           : std::numeric_limits<float>::quiet_NaN());
                if (terrain) {
                    for (auto k : {ia, ib, ic}) {
                        if (!m.bounds_) m.bounds_.emplace(world[k], world[k]);
                        m.bounds_->first = glm::min(m.bounds_->first, world[k]);
                        m.bounds_->second = glm::max(m.bounds_->second, world[k]);
                    }
                }
            }
        }
    };
    const bool refined = visual && zone.draw_terrain.size() == zone.terrain.size() &&
                         zone.draw_terrain_source.size() == zone.terrain.size();
    for (const auto& mesh : refined ? zone.draw_terrain : zone.terrain)
        add_mesh(mesh, glm::mat4(1.0f), true, false);
    for (const auto& p : zone.placements) {
        auto it = zone.objects.find(p.actor);
        if (it == zone.objects.end()) continue;
        if (visual) {
            if (const auto replacement = zone.draw_objects.find(p.actor); replacement != zone.draw_objects.end()) {
                add_mesh(replacement->second, p.transform, false, false);
                continue; // the renderer draws this static replacement instead of the original animated mesh
            }
        }
        const bool ladder = is_ladder_actor(p.actor);
        const auto anim = zone.object_animations.find(p.actor);
        add_mesh(it->second, p.transform, false, ladder,
                 anim != zone.object_animations.end() ? &anim->second.vertex_node : nullptr);
        if (!ladder || visual) continue;
        for (const CollisionTriangle& t : ladder_slab(it->second, p.transform)) {
            m.triangles_.push_back(t);
            m.light_.push_back(std::numeric_limits<float>::quiet_NaN());
        }
    }
    m.index();
    return m;
}

CollisionMesh CollisionMesh::from_triangles(std::vector<CollisionTriangle> triangles) {
    CollisionMesh m;
    m.triangles_ = std::move(triangles);
    for (const auto& t : m.triangles_) {
        for (const glm::vec3& v : {t.v0, t.v1, t.v2}) {
            if (!m.bounds_) m.bounds_.emplace(v, v);
            m.bounds_->first = glm::min(m.bounds_->first, v);
            m.bounds_->second = glm::max(m.bounds_->second, v);
        }
    }
    m.index();
    return m;
}

void CollisionMesh::index() {
    for (std::uint32_t t = 0; t < triangles_.size(); ++t) {
        const auto& tri = triangles_[t];
        glm::vec3 lo = glm::min(glm::min(tri.v0, tri.v1), tri.v2);
        glm::vec3 hi = glm::max(glm::max(tri.v0, tri.v1), tri.v2);
        const auto x0 = cell_of(lo.x), x1 = cell_of(hi.x), y0 = cell_of(lo.y), y1 = cell_of(hi.y);
        if ((std::int64_t{x1} - x0 + 1) * (std::int64_t{y1} - y0 + 1) > 4096) {
            big_.push_back(t); // a huge triangle: tested by every query
            continue;
        }
        for (auto x = x0; x <= x1; ++x)
            for (auto y = y0; y <= y1; ++y) cells_[cell_key(x, y)].push_back(t);
    }
    // Find trails ask for thousands of exact ground heights per frame. Dense terrain can put several thousand
    // triangles in one coarse cell; narrow its ground-only candidates without changing any intersection maths.
    for (const auto& [key, candidates] : cells_) {
        if (candidates.size() <= 32) continue;
        const auto bits = std::bit_cast<std::uint64_t>(key);
        const auto cx = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits >> 32));
        const auto cy = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
        for (const bool visual : {false, true}) {
            if (!visual && candidates.size() <= 128) continue;
            std::array<std::vector<std::uint32_t>, kGroundDivisions * kGroundDivisions> bins;
            for (const std::uint32_t t : candidates) {
                const auto& tri = triangles_[t];
                if (!(visual ? visual_surface(tri) : visible_ground(tri)) ||
                    !finite(tri.v0) || !finite(tri.v1) || !finite(tri.v2)) continue;
                const glm::vec3 lo = glm::min(glm::min(tri.v0, tri.v1), tri.v2);
                const glm::vec3 hi = glm::max(glm::max(tri.v0, tri.v1), tri.v2);
                // Keep boundary neighbours too: a barycentric weight can round to zero a float's step beyond an edge.
                const float inf = std::numeric_limits<float>::infinity();
                const auto x0 = ground_subcell(std::nextafter(lo.x, -inf), cx);
                const auto x1 = ground_subcell(std::nextafter(hi.x, inf), cx);
                const auto y0 = ground_subcell(std::nextafter(lo.y, -inf), cy);
                const auto y1 = ground_subcell(std::nextafter(hi.y, inf), cy);
                for (auto x = x0; x <= x1; ++x)
                    for (auto y = y0; y <= y1; ++y) bins[y * kGroundDivisions + x].push_back(t);
            }
            auto& grid = visual ? visual_cells_[key] : ground_cells_[key];
            for (std::size_t part = 0; part < bins.size(); ++part) grid.offsets[part + 1] = grid.offsets[part] + bins[part].size();
            grid.candidates.reserve(grid.offsets.back());
            for (const auto& bin : bins) grid.candidates.insert(grid.candidates.end(), bin.begin(), bin.end());
        }
    }
}

template <class Fn>
void CollisionMesh::for_each_at(const glm::vec3& p, Fn&& fn) const {
    if (auto it = cells_.find(cell_key(cell_of(p.x), cell_of(p.y))); it != cells_.end())
        for (std::uint32_t t : it->second) fn(triangles_[t]);
    // A triangle too large for the grid is in no cell (index()), so every point query tests it too.
    for (std::uint32_t t : big_) fn(triangles_[t]);
}

std::optional<float> CollisionMesh::floor_below(const glm::vec3& p, float step_up) const {
    std::optional<float> best;
    const float limit = p.z + step_up;
    for_each_at(p, [&](const CollisionTriangle& tri) {
        if (!movement_solid(tri)) return;
        // Barycentric test in XY, then the plane height at (x, y).
        glm::vec2 a(tri.v0), b(tri.v1), c(tri.v2), q(p);
        float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(d) < 1e-6f) return; // vertical or degenerate
        float u = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / d;
        float v = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / d;
        float w = 1.0f - u - v;
        if (u < 0.0f || v < 0.0f || w < 0.0f) return;
        float z = u * tri.v0.z + v * tri.v1.z + w * tri.v2.z;
        if (z <= limit && (!best || z > *best)) best = z;
    });
    return best;
}

std::optional<float> CollisionMesh::ceiling_above(const glm::vec3& p) const {
    std::optional<float> best;
    for_each_at(p, [&](const CollisionTriangle& tri) {
        if (!movement_solid(tri)) return;
        glm::vec2 a(tri.v0), b(tri.v1), c(tri.v2), q(p);
        float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(d) < 1e-6f) return;
        float u = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / d;
        float v = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / d;
        float w = 1.0f - u - v;
        if (u < 0.0f || v < 0.0f || w < 0.0f) return;
        float z = u * tri.v0.z + v * tri.v1.z + w * tri.v2.z;
        if (z > p.z && (!best || z < *best)) best = z;
    });
    return best;
}

std::vector<CollisionMesh::SurfaceHit> CollisionMesh::surfaces_above(const glm::vec3& p) const {
    std::vector<SurfaceHit> out;
    for_each_at(p, [&](const CollisionTriangle& tri) {
        if (!probe_solid(tri)) return;
        if (const auto z = height_in(tri, glm::vec2(p)); z && *z > p.z) out.push_back({*z, tri.normal});
    });
    std::sort(out.begin(), out.end(), [](const SurfaceHit& a, const SurfaceHit& b) { return a.z < b.z; });
    return out;
}

std::optional<float> CollisionMesh::ground_below(const glm::vec3& p) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return std::nullopt;
    std::optional<float> best;
    auto check = [&](const CollisionTriangle& tri) {
        const auto z = height_in(tri, glm::vec2(p));
        if (z && *z <= p.z && (!best || *z > *best)) best = z;
    };
    const auto cx = cell_of(p.x), cy = cell_of(p.y);
    const auto key = cell_key(cx, cy);
    if (const auto it = ground_cells_.find(key); it != ground_cells_.end()) {
        const auto x = ground_subcell(p.x, cx), y = ground_subcell(p.y, cy);
        const auto part = y * kGroundDivisions + x;
        const auto& grid = it->second;
        for (auto i = grid.offsets[part]; i < grid.offsets[part + 1]; ++i) check(triangles_[grid.candidates[i]]);
    } else if (const auto cell = cells_.find(key); cell != cells_.end()) {
        for (const std::uint32_t t : cell->second)
            if (visible_ground(triangles_[t])) check(triangles_[t]);
    }
    for (const std::uint32_t t : big_)
        if (visible_ground(triangles_[t])) check(triangles_[t]);
    return best;
}

std::optional<float> CollisionMesh::roof_above(const glm::vec3& p) const {
    std::optional<float> best;
    for_each_at(p, [&](const CollisionTriangle& tri) {
        if (!probe_solid(tri) || (tri.flags & kTriangleInvisible)) return;
        const auto z = height_in(tri, glm::vec2(p));
        if (z && *z > p.z && (!best || *z < *best)) best = z;
    });
    return best;
}

std::optional<float> CollisionMesh::visual_surface_below(const glm::vec3& p) const {
    if (!finite(p)) return std::nullopt;
    std::optional<float> best;
    const auto check = [&](const CollisionTriangle& tri) {
        const auto z = height_in(tri, glm::vec2(p));
        if (z && std::isfinite(*z) && *z <= p.z && (!best || *z > *best)) best = z;
    };
    const auto cx = cell_of(p.x), cy = cell_of(p.y);
    const auto key = cell_key(cx, cy);
    if (const auto it = visual_cells_.find(key); it != visual_cells_.end()) {
        const auto part = ground_subcell(p.y, cy) * kGroundDivisions + ground_subcell(p.x, cx);
        const auto& grid = it->second;
        for (auto i = grid.offsets[part]; i < grid.offsets[part + 1]; ++i) check(triangles_[grid.candidates[i]]);
    } else if (const auto cell = cells_.find(key); cell != cells_.end()) {
        for (const auto t : cell->second)
            if (visual_surface(triangles_[t])) check(triangles_[t]);
    }
    for (const auto t : big_)
        if (visual_surface(triangles_[t])) check(triangles_[t]);
    return best;
}

bool CollisionMesh::visual_surfaces(const glm::vec2& low,const glm::vec2& high,
                                   const std::function<bool(const CollisionTriangle&)>& visit,std::uint32_t& work) const {
    if(!std::isfinite(low.x)||!std::isfinite(low.y)||!std::isfinite(high.x)||!std::isfinite(high.y)||
       low.x>high.x||low.y>high.y)return false;
    const auto x0=cell_of(low.x),x1=cell_of(high.x),y0=cell_of(low.y),y1=cell_of(high.y);
    if((std::int64_t{x1}-x0+1)*(std::int64_t{y1}-y0+1)>static_cast<std::int64_t>(work))return false;
    SeenTriangles seen(triangles_.size()); // (a plugin's surface patches ask this many times a frame: no set each call)
    const auto sample=[&](std::uint32_t index){
        if(work==0)return false;
        --work;
        if(!seen.first(index))return true;
        const auto& tri=triangles_[index];
        if(!visual_surface(tri))return true;
        const auto lo=glm::min(glm::min(glm::vec2(tri.v0),glm::vec2(tri.v1)),glm::vec2(tri.v2));
        const auto hi=glm::max(glm::max(glm::vec2(tri.v0),glm::vec2(tri.v1)),glm::vec2(tri.v2));
        if(hi.x<low.x||hi.y<low.y||lo.x>high.x||lo.y>high.y)return true;
        return visit(tri);
    };
    for(auto x=x0;x<=x1;++x)for(auto y=y0;y<=y1;++y){
        if(work==0)return false;
        --work;
        if(const auto cell=cells_.find(cell_key(x,y));cell!=cells_.end())
            for(const auto index:cell->second)if(!sample(index))return false;
    }
    for(const auto index:big_)if(!sample(index))return false;
    return true;
}

bool CollisionMesh::line_of_sight(const glm::vec3& a, const glm::vec3& b) const {
    glm::vec3 d = b - a;
    float length = glm::length(d);
    if (length < 1e-3f) return true;
    // A NaN or infinite end (a script's LineOfSight, a server teleport) is answered like a line too long to test.
    if (!finite(a) || !finite(b)) return true;
    const auto x0 = cell_of(std::min(a.x, b.x)), x1 = cell_of(std::max(a.x, b.x));
    const auto y0 = cell_of(std::min(a.y, b.y)), y1 = cell_of(std::max(a.y, b.y));
    // The span in 64 bits: an int32 x1 - x0 wrapped negative for ends 2^31 cells apart and let the loop run.
    if ((std::int64_t{x1} - x0 + 1) * (std::int64_t{y1} - y0 + 1) > 256) return true; // too long to bother
    // Only the box's cells the segment passes over (within a unit, for rounding): a triangle it crosses is listed in
    // the cell under the crossing (index() lists a triangle in every cell its box overlaps), so the others cannot
    // change the answer; a triangle too large for the grid is in no cell and is tested after them. A diagonal sight
    // line's box held up to 256 cells, most of them nowhere near it; the nameplates ask this for every name in view
    // each frame.
    const glm::vec2 from(a), dir(d);
    auto passes_over = [&](std::int32_t x, std::int32_t y) {
        constexpr float kMargin = 1.0f;
        const glm::vec2 lo(static_cast<float>(x) * kCell - kMargin, static_cast<float>(y) * kCell - kMargin);
        const glm::vec2 hi(lo.x + kCell + 2.0f * kMargin, lo.y + kCell + 2.0f * kMargin);
        float t0 = 0.0f, t1 = 1.0f; // the segment's part inside the (slab) rectangle
        for (int i = 0; i < 2; ++i) {
            if (std::abs(dir[i]) < 1e-6f) {
                if (from[i] < lo[i] || from[i] > hi[i]) return false;
                continue;
            }
            float u = (lo[i] - from[i]) / dir[i], v = (hi[i] - from[i]) / dir[i];
            if (u > v) std::swap(u, v);
            t0 = std::max(t0, u);
            t1 = std::min(t1, v);
            if (t0 > t1) return false;
        }
        return true;
    };
    auto blocks = [&](const CollisionTriangle& tri) {
        if (!probe_solid(tri)) return false;
        // Moller-Trumbore, hits strictly inside the segment.
        glm::vec3 e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0;
        glm::vec3 pv = glm::cross(d, e2);
        float det = glm::dot(e1, pv);
        if (std::abs(det) < 1e-8f) return false;
        float inv = 1.0f / det;
        glm::vec3 tv = a - tri.v0;
        float u = glm::dot(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f) return false;
        glm::vec3 qv = glm::cross(tv, e1);
        float v = glm::dot(d, qv) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;
        float hit = glm::dot(e2, qv) * inv;
        return hit > 0.001f && hit < 0.999f;
    };
    SeenTriangles seen(triangles_.size());
    for (auto x = x0; x <= x1; ++x) {
        for (auto y = y0; y <= y1; ++y) {
            if (!passes_over(x, y)) continue;
            auto it = cells_.find(cell_key(x, y));
            if (it == cells_.end()) continue;
            for (std::uint32_t t : it->second)
                if (seen.first(t) && blocks(triangles_[t])) return false; // a triangle over several cells once
        }
    }
    for (std::uint32_t t : big_)
        if (blocks(triangles_[t])) return false;
    return true;
}

std::optional<CollisionMesh::SegmentHit> CollisionMesh::first_hit(const glm::vec3& a, const glm::vec3& b, bool back_faces) const {
    const glm::vec3 d = b - a;
    std::optional<SegmentHit> best;
    for_each_on_segment(a, b, [&](const CollisionTriangle& tri) {
        if (tri.flags & (kTriangleTransparent | 0x10)) return;
        if (!back_faces && glm::dot(tri.normal, d) > 0.0f) return; // a back face
        // Moller-Trumbore.
        glm::vec3 e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0;
        glm::vec3 pv = glm::cross(d, e2);
        float det = glm::dot(e1, pv);
        if (std::abs(det) < 1e-8f) return;
        float inv = 1.0f / det;
        glm::vec3 tv = a - tri.v0;
        float u = glm::dot(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f) return;
        glm::vec3 qv = glm::cross(tv, e1);
        float v = glm::dot(d, qv) * inv;
        if (v < 0.0f || u + v > 1.0f) return;
        float t = glm::dot(e2, qv) * inv;
        if (t < 0.0f || t > 1.0f || (best && best->t <= t)) return;
        best = SegmentHit{t, tri.normal};
    });
    return best;
}

void CollisionMesh::for_each_on_segment(const glm::vec3& a, const glm::vec3& b,
                                        const std::function<void(const CollisionTriangle&)>& fn) const {
    if (!finite(a) || !finite(b)) return;
    SeenTriangles seen(triangles_.size());
    auto visit = [&](std::int32_t x, std::int32_t y) {
        auto it = cells_.find(cell_key(x, y));
        if (it == cells_.end()) return;
        for (std::uint32_t t : it->second) {
            if (!seen.first(t)) continue;
            if (triangles_[t].flags & kTriangleLadderSlab) continue; // the movement's alone
            fn(triangles_[t]);
        }
    };
    // The cells the segment's XY projection crosses (Amanatides and Woo).
    glm::vec2 p(a.x / kCell, a.y / kCell), q(b.x / kCell, b.y / kCell);
    auto x = cell_of(a.x), y = cell_of(a.y);
    const auto x_end = cell_of(b.x), y_end = cell_of(b.y);
    glm::vec2 d = q - p;
    int step_x = d.x > 0 ? 1 : (d.x < 0 ? -1 : 0), step_y = d.y > 0 ? 1 : (d.y < 0 ? -1 : 0);
    float t_max_x = step_x ? ((step_x > 0 ? std::floor(p.x) + 1.0f - p.x : p.x - std::floor(p.x)) / std::abs(d.x)) : 2.0f;
    float t_max_y = step_y ? ((step_y > 0 ? std::floor(p.y) + 1.0f - p.y : p.y - std::floor(p.y)) / std::abs(d.y)) : 2.0f;
    float t_dx = step_x ? 1.0f / std::abs(d.x) : 2.0f, t_dy = step_y ? 1.0f / std::abs(d.y) : 2.0f;
    for (int guard = 0; guard < 100000; ++guard) {
        visit(x, y);
        if (x == x_end && y == y_end) break;
        if (t_max_x < t_max_y) {
            if (t_max_x > 1.0f) break;
            x += step_x;
            t_max_x += t_dx;
        } else {
            if (t_max_y > 1.0f) break;
            y += step_y;
            t_max_y += t_dy;
        }
    }
    for (std::uint32_t t : big_) fn(triangles_[t]);
}

void CollisionMesh::sweep_sphere(SphereSweep& s) const { (void)sweep_sphere_hit(s); }

template <class Fn>
void CollisionMesh::for_each_near_sweep(const SphereSweep& s, Fn&& fn) const {
    // The swept sphere's box: the original walks its AABB tree with the
    // segment inflated by the radius; the grid cells under it hold the same
    // candidates (in another order: equal-t ties may pick another normal).
    glm::vec3 lo = glm::min(s.start, s.end) - glm::vec3(s.radius);
    glm::vec3 hi = glm::max(s.start, s.end) + glm::vec3(s.radius);
    if (!finite(lo) || !finite(hi)) return;
    const auto x0 = cell_of(lo.x), x1 = cell_of(hi.x), y0 = cell_of(lo.y), y1 = cell_of(hi.y);
    if ((std::int64_t{x1} - x0 + 1) * (std::int64_t{y1} - y0 + 1) > 4096) return; // a sweep across the zone: not a move
    const bool several = x1 > x0 || y1 > y0;
    std::optional<SeenTriangles> seen;
    if (several) seen.emplace(triangles_.size());
    for (auto x = x0; x <= x1; ++x) {
        for (auto y = y0; y <= y1; ++y) {
            auto it = cells_.find(cell_key(x, y));
            if (it == cells_.end()) continue;
            for (std::uint32_t t : it->second) {
                const auto& tri = triangles_[t];
                glm::vec3 tlo = glm::min(glm::min(tri.v0, tri.v1), tri.v2), thi = glm::max(glm::max(tri.v0, tri.v1), tri.v2);
                if (tlo.x > hi.x || thi.x < lo.x || tlo.y > hi.y || thi.y < lo.y || tlo.z > hi.z || thi.z < lo.z) continue;
                // A triangle spanning several cells is listed in each of them.
                if (several && !seen->first(t)) continue;
                fn(t);
            }
        }
    }
    for (std::uint32_t t : big_) fn(t);
}

std::int32_t CollisionMesh::sweep_sphere_hit(SphereSweep& s) const {
    std::int32_t hit = -1;
    for_each_near_sweep(s, [&](std::uint32_t t) {
        if (!movement_kept(triangles_[t])) return;
        if (collide_triangle(s, triangles_[t])) hit = static_cast<std::int32_t>(t);
    });
    return hit;
}

bool CollisionMesh::touches_passable_ladder(const SphereSweep& s) const {
    // The DLL's model sweep (0x100c5af0) tests a passable triangle only when its flags also have bit 2: such a hit
    // short of the stop runs the hit callback (CollidingType 6 for a ladder) without stopping the sphere. Nothing
    // seen sets bit 2 on a WLD model's triangles (0x10057540 sets bit 0 alone), so this test is Hailstone's own:
    // it treats a passable ladder's triangles as if they had it. Since the sphere goes into such a ladder, a sphere
    // that already overlaps one of its triangles where the move starts (the DLL's zero-length test) touches it too;
    // without that the climber inside the rungs meets them only every other frame and gravity wins.
    bool touched = false;
    const SphereSweep still = SphereSweep::make(s.start, s.start, s.radius);
    for_each_near_sweep(s, [&](std::uint32_t t) {
        if (touched) return;
        const CollisionTriangle& tri = triangles_[t];
        constexpr std::uint8_t kPassableLadder = kTrianglePassable | kTriangleLadder;
        if ((tri.flags & kPassableLadder) != kPassableLadder) return;
        CollisionTriangle touch = tri;
        touch.flags = static_cast<std::uint8_t>(touch.flags & ~kTrianglePassable);
        SphereSweep probe = s; // tested up to the sweep's stop, which it does not move
        SphereSweep at_start = still;
        touched = collide_triangle(probe, touch) || collide_triangle(at_start, touch);
    });
    return touched;
}

} // namespace hailstone::game
