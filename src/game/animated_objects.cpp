#include "game/animated_objects.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "assets/zone.h"

namespace hailstone::game {

AnimatedObjects AnimatedObjects::build(const assets::Zone& zone) {
    AnimatedObjects out;
    std::unordered_map<std::string, std::uint32_t> numbers; // actor -> definition
    for (const auto& p : zone.placements) {
        const auto anim = zone.object_animations.find(p.actor);
        if (anim == zone.object_animations.end()) continue;
        const auto mesh = zone.objects.find(p.actor);
        if (mesh == zone.objects.end()) continue;
        auto [number, fresh] = numbers.try_emplace(p.actor, static_cast<std::uint32_t>(out.definitions_.size()));
        if (fresh) {
            Definition d;
            d.animation = anim->second;
            // One mesh per moving node with triangles: the object's vertices, only that node's triangles.
            const assets::Mesh& m = mesh->second;
            const auto& vn = d.animation.vertex_node;
            std::vector<std::uint16_t> nodes;
            for (std::uint32_t i = 0; i + 2 < m.indices.size(); i += 3) {
                const std::uint16_t n = moving_triangle_node(vn, m.indices[i], m.indices[i + 1], m.indices[i + 2]);
                if (n != assets::kStillVertex && std::find(nodes.begin(), nodes.end(), n) == nodes.end())
                    nodes.push_back(n);
            }
            for (std::uint16_t n : nodes) {
                assets::Mesh part;
                part.vertices = m.vertices;
                part.materials = m.materials;
                part.front_ccw = m.front_ccw;
                for (const auto& mp : m.parts) {
                    assets::MeshPart kept = mp;
                    kept.first_index = static_cast<std::uint32_t>(part.indices.size());
                    for (std::uint32_t i = 0; i + 2 < mp.index_count; i += 3) {
                        if (std::size_t{mp.first_index} + i + 2 >= m.indices.size()) break;
                        const std::uint32_t a = m.indices[mp.first_index + i], b = m.indices[mp.first_index + i + 1],
                                            c = m.indices[mp.first_index + i + 2];
                        if (moving_triangle_node(vn, a, b, c) != n) continue;
                        part.indices.insert(part.indices.end(), {a, b, c});
                    }
                    kept.index_count = static_cast<std::uint32_t>(part.indices.size()) - kept.first_index;
                    if (kept.index_count) part.parts.push_back(kept);
                }
                ObjectShape shape = ObjectShape::from_mesh(part);
                // A render-only replacement is drawn as a static instance in ZoneRenderer. Its original mesh
                // still supplies physics, but must not supply an invisible moving floor to effects.
                if (zone.draw_objects.contains(p.actor)) shape.visual.clear();
                if (shape.records.empty() && shape.visual.empty()) continue;
                // The farthest its triangles get from the object's origin, at every key and between keys.
                float reach = 0.0f;
                std::vector<glm::mat4> pose;
                const std::uint32_t samples = std::clamp<std::uint32_t>(d.animation.period_ms / 8, 8, 512);
                for (std::uint32_t k = 0; k <= samples; ++k) {
                    d.animation.pose(std::uint64_t{d.animation.period_ms} * k / samples, pose);
                    for (const auto& r : shape.records)
                        for (const glm::vec3& v : {r.v0, r.v1, r.v2})
                            reach = std::max(reach, glm::length(glm::vec3(pose[n] * glm::vec4(v, 1.0f))));
                    for (const auto& r : shape.visual)
                        for (const glm::vec3& v : {r.v0, r.v1, r.v2})
                            reach = std::max(reach, glm::length(glm::vec3(pose[n] * glm::vec4(v, 1.0f))));
                }
                d.nodes.push_back(n);
                d.shapes.push_back(std::move(shape));
                d.reach.push_back(reach * 1.01f + 0.5f);
            }
            out.definitions_.push_back(std::move(d));
        }
        const Definition& d = out.definitions_[number->second];
        const float scale = glm::length(glm::vec3(p.transform[0]));
        for (std::uint32_t s = 0; s < d.shapes.size(); ++s) {
            Part part;
            part.definition = number->second;
            part.shape = s;
            part.placement = p.transform;
            part.placement_inverse = glm::inverse(p.transform);
            part.scale = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
            part.centre = glm::vec3(p.transform[3]);
            part.reach = d.reach[s] * part.scale;
            const auto index = static_cast<std::uint32_t>(out.parts_.size());
            out.parts_.push_back(part);
            if (!std::isfinite(part.centre.x) || !std::isfinite(part.centre.y) || !std::isfinite(part.reach)) continue;
            const auto x0 = static_cast<std::int32_t>(std::floor((part.centre.x - part.reach) / kCell));
            const auto x1 = static_cast<std::int32_t>(std::floor((part.centre.x + part.reach) / kCell));
            const auto y0 = static_cast<std::int32_t>(std::floor((part.centre.y - part.reach) / kCell));
            const auto y1 = static_cast<std::int32_t>(std::floor((part.centre.y + part.reach) / kCell));
            if (std::int64_t{x1} - x0 > 16 || std::int64_t{y1} - y0 > 16) {
                out.big_.push_back(index); // asked by every query rather than spread over many cells
                continue;
            }
            for (std::int32_t x = x0; x <= x1; ++x)
                for (std::int32_t y = y0; y <= y1; ++y) out.grid_[cell_key(x, y)].push_back(index);
        }
    }
    out.set_time(0);
    return out;
}

void AnimatedObjects::placed(const Part& part, glm::mat4& transform, glm::mat4& inverse) const {
    const Definition& d = definitions_[part.definition];
    const std::uint16_t node = d.nodes[part.shape];
    if (node < d.pose.size()) {
        transform = part.placement * d.pose[node];
        inverse = d.pose_inverse[node] * part.placement_inverse;
    } else {
        transform = part.placement;
        inverse = part.placement_inverse;
    }
}

glm::mat4 AnimatedObjects::part_transform(std::size_t i) const {
    glm::mat4 t, inv;
    placed(parts_[i], t, inv);
    return t;
}

void AnimatedObjects::set_time(std::uint64_t ms) {
    time_ms_ = ms;
    visual_time_ms_ = ms;
    for (Definition& d : definitions_) {
        d.animation.pose(ms, d.pose);
        d.visual_pose=d.pose;
        d.pose_inverse.resize(d.pose.size());
        for (std::size_t i = 0; i < d.pose.size(); ++i) d.pose_inverse[i] = glm::inverse(d.pose[i]);
    }
}

void AnimatedObjects::set_visual_time(std::uint64_t ms) {
    visual_time_ms_=ms;
    for(auto& definition:definitions_)definition.animation.pose(ms,definition.visual_pose);
}

glm::mat4 AnimatedObjects::placed_visual(const Part& part) const {
    const auto& definition=definitions_[part.definition];
    const auto node=definition.nodes[part.shape];
    return node<definition.visual_pose.size()?part.placement*definition.visual_pose[node]:part.placement;
}

void AnimatedObjects::near(const glm::vec3& lo, const glm::vec3& hi, std::vector<std::uint32_t>& out) const {
    out.clear();
    if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(hi.x) || !std::isfinite(hi.y)) return;
    const auto cx0 = static_cast<std::int32_t>(std::floor(lo.x / kCell)), cy0 = static_cast<std::int32_t>(std::floor(lo.y / kCell));
    const auto cx1 = static_cast<std::int32_t>(std::floor(hi.x / kCell)), cy1 = static_cast<std::int32_t>(std::floor(hi.y / kCell));
    if (std::int64_t{cx1} - cx0 > 64 || std::int64_t{cy1} - cy0 > 64) {
        for (std::uint32_t i = 0; i < parts_.size(); ++i) out.push_back(i); // a huge query: every part
        return;
    }
    out = big_;
    for (std::int32_t x = cx0; x <= cx1; ++x)
        for (std::int32_t y = cy0; y <= cy1; ++y)
            if (auto it = grid_.find(cell_key(x, y)); it != grid_.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool AnimatedObjects::sweep_sphere(SphereSweep& sweep) const {
    if (parts_.empty()) return false;
    const glm::vec3 a = sweep.start, b = sweep.end;
    std::vector<std::uint32_t> candidates;
    near(glm::min(a, b) - sweep.radius, glm::max(a, b) + sweep.radius, candidates);
    bool hit = false;
    glm::mat4 transform, inverse;
    for (std::uint32_t i : candidates) {
        const Part& p = parts_[i];
        placed(p, transform, inverse);
        hit |= game::sweep_sphere(definitions_[p.definition].shapes[p.shape], transform, inverse, p.scale, sweep);
    }
    return hit;
}

std::optional<float> AnimatedObjects::floor_below(const glm::vec3& at, float step_up) const {
    if (parts_.empty()) return std::nullopt;
    std::vector<std::uint32_t> candidates;
    near(at, at, candidates);
    std::optional<float> best;
    glm::mat4 transform, inverse;
    for (std::uint32_t i : candidates) {
        const Part& p = parts_[i];
        if (glm::distance(glm::vec2(p.centre), glm::vec2(at)) > p.reach) continue;
        placed(p, transform, inverse);
        if (auto z = game::floor_under(definitions_[p.definition].shapes[p.shape], inverse, at, step_up);
            z && (!best || *z > *best))
            best = z;
    }
    return best;
}

bool AnimatedObjects::blocks(const glm::vec3& a, const glm::vec3& b) const {
    if (parts_.empty()) return false;
    std::vector<std::uint32_t> candidates;
    near(glm::min(a, b), glm::max(a, b), candidates);
    glm::mat4 transform, inverse;
    for (std::uint32_t i : candidates) {
        const Part& p = parts_[i];
        placed(p, transform, inverse);
        if (game::blocks(definitions_[p.definition].shapes[p.shape], transform, inverse, p.scale, a, b)) return true;
    }
    return false;
}

std::optional<float> AnimatedObjects::visual_surface_below(const glm::vec3& at) const {
    if (parts_.empty()) return std::nullopt;
    std::vector<std::uint32_t> candidates;
    near(at, at, candidates);
    std::optional<float> best;
    for (std::uint32_t i : candidates) {
        const Part& p = parts_[i];
        if (glm::distance(glm::vec2(p.centre), glm::vec2(at)) > p.reach) continue;
        const auto transform=placed_visual(p);
        if (auto z = game::visual_surface_under(definitions_[p.definition].shapes[p.shape], transform, at);
            z && (!best || *z > *best)) best = z;
    }
    return best;
}

bool AnimatedObjects::visual_surfaces(const glm::vec2& low,const glm::vec2& high,
                                     const std::function<bool(const CollisionTriangle&)>& visit,std::uint32_t& work) const {
    std::vector<std::uint32_t> candidates;
    near(glm::vec3(low,0),glm::vec3(high,0),candidates);
    for(const auto i:candidates){
        if(work==0)return false;
        --work;
        const auto& part=parts_[i];
        const auto transform=placed_visual(part);
        for(const auto& tri:definitions_[part.definition].shapes[part.shape].visual){
            if(work==0)return false;
            --work;
            const auto world=CollisionTriangle::make(glm::vec3(transform*glm::vec4(tri.v0,1)),
                glm::vec3(transform*glm::vec4(tri.v1,1)),glm::vec3(transform*glm::vec4(tri.v2,1)),0);
            const auto lo=glm::min(glm::min(glm::vec2(world.v0),glm::vec2(world.v1)),glm::vec2(world.v2));
            const auto hi=glm::max(glm::max(glm::vec2(world.v0),glm::vec2(world.v1)),glm::vec2(world.v2));
            if(world.normal.z>0&&hi.x>=low.x&&hi.y>=low.y&&lo.x<=high.x&&lo.y<=high.y&&!visit(world))return false;
        }
    }
    return true;
}

} // namespace hailstone::game
