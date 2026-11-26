#include "game/picking.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "game/sine_table.h"

namespace hailstone::game {

namespace {

constexpr float kFltEpsilon = 1.1920929e-7f; // 0x10131c14
constexpr float kSphereSlack = 0.01f;        // dist² - 0.01 <= r²

using dvec3 = glm::dvec3;

// Ericson, Real-Time Collision Detection 5.1.5: the closest point of triangle abc to p.
dvec3 closest_on_triangle(const dvec3& p, const dvec3& a, const dvec3& b, const dvec3& c) {
    dvec3 ab = b - a, ac = c - a, ap = p - a;
    double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return a;
    dvec3 bp = p - b;
    double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return b;
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) return a + ab * (d1 / (d1 - d3));
    dvec3 cp = p - c;
    double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return c;
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) return a + ac * (d2 / (d2 - d6));
    double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Ericson 5.1.9: the closest points of segments p1q1 and p2q2; returns the squared distance and s on the first.
double segment_segment(const dvec3& p1, const dvec3& q1, const dvec3& p2, const dvec3& q2, double& s) {
    dvec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    double t = 0.0;
    constexpr double kEps = 1e-12;
    if (a <= kEps && e <= kEps) {
        s = 0.0;
        return glm::dot(r, r);
    }
    if (a <= kEps) {
        s = 0.0;
        t = std::clamp(f / e, 0.0, 1.0);
    } else {
        double c = glm::dot(d1, r);
        if (e <= kEps) {
            t = 0.0;
            s = std::clamp(-c / a, 0.0, 1.0);
        } else {
            double b = glm::dot(d1, d2), denom = a * e - b * b;
            s = denom != 0.0 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    dvec3 d = (p1 + d1 * s) - (p2 + d2 * t);
    return glm::dot(d, d);
}

} // namespace

float cmath_cot_folded(float angle) {
    // 0x100b93d0: stored as an integer with truncation, & 0x1ff, quadrants 1 and 2 folded with ~i, & 0x7f; entry
    // +0x1088 + 16 i of the table CMath::InitSinCosTables builds (@0x100b9790): cos[i] / sin[i], 1e30 at i = 0.
    auto i = static_cast<std::int32_t>(static_cast<std::int64_t>(std::trunc(angle)) & 0x1ff);
    std::int32_t q = i & 0x180;
    if (q == 0x100 || q == 0x80) i = ~i;
    i &= 0x7f;
    if (i == 0) return 1.0e30f;
    float x = static_cast<float>(i);
    return cmath_cos(x) / cmath_sin(x);
}

PickSegment screen_ray(const PickCamera& cam, float x, float y) {
    // 0x10006860 (vtable +0x78): the camera-space direction.
    float k = cmath_cot_folded(cam.half_fov);
    float cx = cam.half_width, cy = cam.half_height;
    float dx = ((x - cx) + 0.5f) / (cx * k);
    float dy = ((cy - 0.5f) - y) / ((k * cy) * cam.fov_ratio);
    // vtable +0x80: into the world by the camera's rows; 0x100076d0: normalised (zero below FLT_EPSILON).
    glm::vec3 dir = cam.right * dx + cam.up * dy + cam.forward;
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < kFltEpsilon) dir = glm::vec3(0.0f);
    else dir *= 1.0f / len;
    return {cam.position, dir * cam.yon};
}

SegmentTriangleDistance segment_triangle_distance(const PickSegment& s, const glm::vec3& a, const glm::vec3& b,
                                                  const glm::vec3& c) {
    dvec3 p0(s.start), d(s.dir), A(a), B(b), C(c);
    dvec3 p1 = p0 + d;
    // The segment through the triangle: distance 0 at the crossing.
    dvec3 e1 = B - A, e2 = C - A, pv = glm::cross(d, e2);
    double det = glm::dot(e1, pv);
    if (std::abs(det) > 1e-18) {
        double inv = 1.0 / det;
        dvec3 tv = p0 - A;
        double u = glm::dot(tv, pv) * inv;
        dvec3 qv = glm::cross(tv, e1);
        double v = glm::dot(d, qv) * inv;
        double t = glm::dot(e2, qv) * inv;
        if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && t >= 0.0 && t <= 1.0) return {0.0f, static_cast<float>(t)};
    }
    // Otherwise the nearest of the segment against the three edges and its ends against the triangle.
    double best = 0.0, best_t = 0.0;
    bool any = false;
    auto consider = [&](double dist, double t) {
        if (!any || dist < best) {
            best = dist;
            best_t = t;
            any = true;
        }
    };
    double sp = 0.0;
    consider(segment_segment(p0, p1, A, B, sp), sp);
    consider(segment_segment(p0, p1, B, C, sp), sp);
    consider(segment_segment(p0, p1, C, A, sp), sp);
    dvec3 q = closest_on_triangle(p0, A, B, C);
    consider(glm::dot(q - p0, q - p0), 0.0);
    q = closest_on_triangle(p1, A, B, C);
    consider(glm::dot(q - p1, q - p1), 1.0);
    return {static_cast<float>(best), static_cast<float>(best_t)};
}

float point_segment_distance_squared(const PickSegment& s, const glm::vec3& p) {
    float dd = glm::dot(s.dir, s.dir);
    float t = dd > 0.0f ? std::clamp(glm::dot(p - s.start, s.dir) / dd, 0.0f, 1.0f) : 0.0f;
    glm::vec3 q = s.start + s.dir * t - p;
    return glm::dot(q, q);
}

std::optional<glm::vec3> pick_ground_placement(const PickSegment& seg, const PickScene& scene) {
    if (!scene.world) return std::nullopt;
    float first = 1.0f;
    bool ground = false;
    scene.world(seg, [&](const PickTriangle& tri) {
        constexpr auto hidden = kTriangleTransparent | kTriangleInvisible | kTriangleLadderSlab |
                                kTriangleNoVisualSurface;
        const float facing = glm::dot(tri.normal, seg.dir);
        if ((tri.flags & hidden) || !(facing < 0.0f || (facing > 0.0f && tri.visual_two_sided))) return;
        const auto hit = segment_triangle_distance(seg, tri.v0, tri.v1, tri.v2);
        if (!(hit.distance_squared <= kPickHitDistanceSquared && hit.t < first)) return;
        first = hit.t;
        ground = facing < 0.0f && !(tri.flags & kTrianglePassable) && tri.normal.z >= 0.5f;
    });
    if (!ground) return std::nullopt;
    return seg.start + seg.dir * first;
}

PickHit pick_actor(const PickSegment& seg, const PickScene& scene, std::uint32_t required, std::uint32_t skip) {
    PickHit out;
    const float dir_len = glm::length(seg.dir);
    auto centre_t = [&](const glm::vec3& p) {
        return dir_len > 0.0f ? std::max(0.0f, glm::length(p - seg.start) / dir_len) : 0.0f;
    };
    for (int pass = 0; pass < 4; ++pass) {
        const int mode = pass & 1;          // passes 2 and 4 run in mode 1 (sphere mode)
        const bool passable_blocking = pass < 2; // +0x61: 1 for passes 1 and 2, cleared for 3 and 4
        out.passes = pass + 1;
        // 0x10008100: t, the world best, the actor best to 1, no actor.
        float info_t = 1.0f, world_best = 1.0f, actor_best = 1.0f;
        int found = -1;

        // The world first (CTerrainBase, TTerrainCollisionTester<LineSegment> and slot 0 / slot 7).
        if (scene.world) {
            scene.world(seg, [&](const PickTriangle& tri) {
                if ((tri.flags & kTrianglePassable) && !passable_blocking) return;
                if (glm::dot(tri.normal, seg.dir) > 0.0f) return; // faces away
                auto d = segment_triangle_distance(seg, tri.v0, tri.v1, tri.v2);
                if (!(d.distance_squared <= kPickHitDistanceSquared) || !(d.t <= info_t)) return;
                info_t = d.t;
                out.normal = tri.normal;
                world_best = std::min(world_best, d.t);
            });
        }

        // Slot 6 (0x100083c0): whether the actor under test is the new nearest.
        auto accept = [&](int i, float t) {
            const PickActor& a = scene.actors[static_cast<std::size_t>(i)];
            if (i == scene.exclude_a || i == scene.exclude_b) return;
            if (!(t <= world_best) || !(t < actor_best)) return;
            if (a.volume == 2) {
                t = std::max(t, centre_t(a.position));
                if (!(t <= world_best) || !(t < actor_best)) return;
            }
            if (mode == 0 && a.hierarchical) {
                // 0x10008250: the segment must pass near the skeleton.
                bool near_bone = false;
                for (const auto& [bone, child] : a.bones) {
                    glm::vec3 c = (bone + child) * 0.5f;
                    float r = std::max(glm::length(bone - child) * 0.5f, 0.25f);
                    if (point_segment_distance_squared(seg, c) - kSphereSlack <= r * r) {
                        near_bone = true;
                        break;
                    }
                }
                if (!near_bone) return;
            }
            found = i;
            info_t = actor_best = t;
        };

        for (std::size_t k = 0; k < scene.actors.size(); ++k) {
            const PickActor& a = scene.actors[k];
            const int i = static_cast<int>(k);
            if (a.disabled || a.invisible || (a.mask & skip) != 0) continue; // the filter (slot 2)
            if (a.volume == 3) continue;
            if ((mode != 0 && a.sphere_in_mode1) || a.volume == 2) {
                float r = a.sphere_radius;
                if (!(point_segment_distance_squared(seg, a.position) - kSphereSlack <= r * r)) continue;
                float t = centre_t(a.position);
                if (!(t <= info_t)) continue;
                glm::vec3 n = seg.start + seg.dir * t - a.position;
                float nl = glm::length(n);
                out.normal = nl > 0.0f ? n / nl : glm::vec3(0.0f);
                accept(i, t);
                continue;
            }
            if (!a.triangles) continue;
            float r = a.bound_radius;
            if (!(point_segment_distance_squared(seg, a.bound_center) - kSphereSlack <= r * r)) continue;
            float model_best = 1.0f;
            a.triangles([&](const PickTriangle& tri) {
                float facing = glm::dot(tri.normal, seg.dir);
                if (a.static_mesh ? facing >= 0.0f : facing > 0.0f) return;
                auto d = segment_triangle_distance(seg, tri.v0, tri.v1, tri.v2);
                if (!(d.distance_squared <= kPickHitDistanceSquared)) return;
                float t = a.static_mesh ? d.t * 0.99f : d.t;
                if (!(t < model_best)) return;
                model_best = t;
                out.normal = tri.normal;
                accept(i, t);
            });
        }

        if (found >= 0) {
            const PickActor& a = scene.actors[static_cast<std::size_t>(found)];
            if (required == 0 || (a.mask & required) != 0) {
                out.actor = found;
                out.t = info_t;
                out.world_t = world_best;
                // 0x100cd3e0: the clamp point, 0.001 short of the hit along the segment.
                glm::vec3 v = seg.dir * info_t;
                float l = glm::length(v);
                out.point = l > 0.0f ? seg.start + v / l * std::max(l - 0.001f, 0.0f) : seg.start;
                return out;
            }
        }
        out.world_t = world_best;
    }
    out.actor = -1;
    return out;
}

} // namespace hailstone::game
