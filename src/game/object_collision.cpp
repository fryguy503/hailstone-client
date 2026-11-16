#include "game/object_collision.h"

#include <algorithm>
#include <cmath>

#include "assets/model.h"

namespace hailstone::game {

namespace {

bool solid(assets::BlendMode b) {
    using assets::BlendMode;
    return b == BlendMode::Opaque || b == BlendMode::Masked || b == BlendMode::Invisible;
}

// Moller-Trumbore on the segment a + t d, t in 0..1.
std::optional<float> segment_hit(const glm::vec3& a, const glm::vec3& d, const glm::vec3& p0, const glm::vec3& p1,
                                 const glm::vec3& p2) {
    glm::vec3 e1 = p1 - p0, e2 = p2 - p0;
    glm::vec3 pv = glm::cross(d, e2);
    float det = glm::dot(e1, pv);
    if (std::abs(det) < 1e-10f) return std::nullopt;
    float inv = 1.0f / det;
    glm::vec3 tv = a - p0;
    float u = glm::dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return std::nullopt;
    glm::vec3 qv = glm::cross(tv, e1);
    float v = glm::dot(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return std::nullopt;
    float t = glm::dot(e2, qv) * inv;
    if (t < 0.0f || t > 1.0f) return std::nullopt;
    return t;
}

float segment_point_distance(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p) {
    glm::vec3 d = b - a;
    float len2 = glm::dot(d, d);
    float t = len2 > 0.0f ? std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::distance(a + d * t, p);
}

} // namespace

bool visual_surface_material(const assets::Material& material) {
    if (material.blend != assets::BlendMode::Opaque && material.blend != assets::BlendMode::Masked) return false;
    using assets::EqgShader;
    switch (material.eqg_shader) {
    case EqgShader::None: case EqgShader::Chroma: case EqgShader::Terrain:
    case EqgShader::Lava: case EqgShader::Lava2: case EqgShader::TerrainTile: return true;
    default: return false;
    }
}

ObjectShape ObjectShape::from_mesh(const assets::Mesh& mesh) {
    ObjectShape s;
    for (const auto& v : mesh.vertices) s.radius = std::max(s.radius, glm::length(v.position));
    bool any = false;
    for (const auto& part : mesh.parts) {
        const bool visible = part.material < mesh.materials.size() && visual_surface_material(mesh.materials[part.material]);
        if (!part.solid && !visible) continue;
        bool see_through = part.material < mesh.materials.size() && !solid(mesh.materials[part.material].blend);
        for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
            // A part reaching past the index list (a damaged file) ends there.
            if (std::size_t{part.first_index} + i + 2 >= mesh.indices.size()) break;
            std::uint32_t idx[3] = {mesh.indices[part.first_index + i], mesh.indices[part.first_index + i + 1],
                                    mesh.indices[part.first_index + i + 2]};
            if (idx[0] >= mesh.vertices.size() || idx[1] >= mesh.vertices.size() || idx[2] >= mesh.vertices.size())
                continue;
            std::uint32_t b = mesh.front_ccw ? idx[2] : idx[1], c = mesh.front_ccw ? idx[1] : idx[2];
            if (visible)
                s.visual.push_back(CollisionTriangle::make(mesh.vertices[idx[0]].position, mesh.vertices[b].position,
                                                           mesh.vertices[c].position, 0));
            if (!part.solid) continue;
            s.records.push_back(CollisionTriangle::make(mesh.vertices[idx[0]].position, mesh.vertices[b].position,
                                                        mesh.vertices[c].position,
                                                        see_through ? kTriangleTransparent : 0));
            if (see_through) continue;
            for (auto k : idx) {
                glm::vec3 p = mesh.vertices[k].position;
                s.tris.push_back(p);
                s.lo = any ? glm::min(s.lo, p) : p;
                s.hi = any ? glm::max(s.hi, p) : p;
                any = true;
            }
        }
    }
    return s;
}

bool sweep_sphere(const ObjectShape& shape, const glm::mat4& transform, const glm::mat4& inverse, float scale,
                  SphereSweep& sweep) {
    if (shape.records.empty() || !(scale > 0.0f)) return false;
    // The bounding sphere first (the model's radius at its scale).
    glm::vec3 centre(transform[3]);
    glm::vec3 d = sweep.delta * sweep.t;
    float len2 = glm::dot(d, d);
    float along = len2 > 0.0f ? std::clamp(glm::dot(centre - sweep.start, d) / len2, 0.0f, 1.0f) : 0.0f;
    if (glm::distance(sweep.start + d * along, centre) > shape.radius * scale + sweep.radius) return false;
    glm::vec3 from(inverse * glm::vec4(sweep.start, 1.0f));
    glm::vec3 to(inverse * glm::vec4(sweep.end, 1.0f));
    SphereSweep local = SphereSweep::make(from, to, sweep.radius / scale);
    local.t = sweep.t;
    local.collided = sweep.collided;
    bool hit = false;
    for (const auto& tri : shape.records) hit |= collide_triangle(local, tri);
    if (!hit) return false;
    sweep.t = local.t;
    sweep.collided = true;
    sweep.normal = normalize_or_zero(glm::vec3(transform * glm::vec4(local.normal, 0.0f)));
    return true;
}

std::optional<float> first_hit(const ObjectShape& shape, const glm::mat4& inverse, const glm::vec3& a,
                               const glm::vec3& b) {
    glm::vec3 la = glm::vec3(inverse * glm::vec4(a, 1.0f));
    glm::vec3 d = glm::vec3(inverse * glm::vec4(b, 1.0f)) - la;
    std::optional<float> best;
    for (std::size_t i = 0; i + 2 < shape.tris.size(); i += 3)
        if (auto t = segment_hit(la, d, shape.tris[i], shape.tris[i + 1], shape.tris[i + 2]); t && (!best || *t < *best))
            best = t;
    return best;
}

bool blocks(const ObjectShape& shape, const glm::mat4& transform, const glm::mat4& inverse, float scale,
            const glm::vec3& a, const glm::vec3& b) {
    if (shape.empty()) return false;
    if (segment_point_distance(a, b, glm::vec3(transform[3])) > shape.radius * scale + 0.5f) return false;
    glm::vec3 la = glm::vec3(inverse * glm::vec4(a, 1.0f));
    glm::vec3 d = glm::vec3(inverse * glm::vec4(b, 1.0f)) - la;
    for (std::size_t i = 0; i + 2 < shape.tris.size(); i += 3)
        if (auto t = segment_hit(la, d, shape.tris[i], shape.tris[i + 1], shape.tris[i + 2]); t && *t > 0.001f && *t < 0.999f)
            return true;
    return false;
}

std::optional<float> floor_under(const ObjectShape& shape, const glm::mat4& inverse, const glm::vec3& at, float step_up) {
    if (shape.empty()) return std::nullopt;
    glm::vec3 top(at.x, at.y, at.z + step_up), bottom(at.x, at.y, at.z - 1000.0f);
    auto t = first_hit(shape, inverse, top, bottom);
    if (!t) return std::nullopt;
    return top.z + (bottom.z - top.z) * *t;
}

std::optional<float> visual_surface_under(const ObjectShape& shape, const glm::mat4& transform,
                                         const glm::vec3& at) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) return std::nullopt;
    std::optional<float> best;
    for (const auto& local : shape.visual) {
        const glm::vec3 a(transform * glm::vec4(local.v0, 1.0f));
        const glm::vec3 b(transform * glm::vec4(local.v1, 1.0f));
        const glm::vec3 c(transform * glm::vec4(local.v2, 1.0f));
        if (!(glm::cross(a - b, c - b).z > 0.0f)) continue;
        const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (!std::isfinite(d) || std::abs(d) < 1e-6f) continue;
        const float u = ((b.y - c.y) * (at.x - c.x) + (c.x - b.x) * (at.y - c.y)) / d;
        const float v = ((c.y - a.y) * (at.x - c.x) + (a.x - c.x) * (at.y - c.y)) / d;
        const float w = 1.0f - u - v;
        if (!(u >= 0.0f && v >= 0.0f && w >= 0.0f)) continue;
        const float z = u * a.z + v * b.z + w * c.z;
        if (std::isfinite(z) && z <= at.z && (!best || z > *best)) best = z;
    }
    return best;
}

} // namespace hailstone::game
