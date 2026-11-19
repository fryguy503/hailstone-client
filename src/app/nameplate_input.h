#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include <glm/glm.hpp>
#include "game/clicks.h"
#include "game/picking.h"

namespace hailstone::app {

struct NameplateHit {
    std::uint32_t id = 0;
    glm::vec4 bounds{0.0f};
    glm::vec3 head{0.0f};
    bool through_walls = false;
    float depth = 0.0f;
    std::uint64_t owner = 0;
    std::uint64_t incarnation = 0;
};
std::optional<std::uint32_t> pick_nameplate(const std::vector<NameplateHit>& hits, glm::vec2 at,
                                           const std::function<bool(const NameplateHit&)>& eligible);
bool nameplate_mouse_eligible(const game::MaskSpawn& spawn, std::uint8_t options, bool self, int camera);
std::optional<game::PickSegment> nameplate_click_ray(const glm::mat4& inverse_scene, const glm::vec4& view,
                                                    const glm::vec3& camera, glm::vec2 at);
std::optional<float> nameplate_scene_depth(const game::PickSegment& ray, const game::PickScene& scene,
                                          const glm::vec3& forward);
bool nameplate_depth_visible(float plate_depth, const std::optional<float>& scene_depth);

// A nameplate sits this far above the head (or the eye, for a spawn without a body top).
inline constexpr float kNameplateHeadOffset = 1.5f;
// Where a spawn's nameplate is anchored: kNameplateHeadOffset above its head. `body_top`: the drawn model's rest-pose
// top above `base` (the model origin), nullopt for a spawn without one (no model, or a meshless skeleton such as a
// bare IVM, whose bounds are empty): its eye, `eye` above `base`, stands in. `seat`: a rider's top on its mount
// (ActorRenderer::drawn_rider_top: the seat bone raised by the body's top, the seat itself without a body), where the
// head is measured from instead of `base`; a rider without a body top takes its eye height above the seat.
glm::vec3 nameplate_head(const glm::vec3& base, std::optional<float> body_top, float eye,
                         const std::optional<glm::vec3>& seat);

struct NameplateGesture {
    std::optional<std::uint32_t> id;
    glm::vec2 pressed{0.0f};
    bool cancelled = false;
    bool captured_elsewhere = false;
    std::uint64_t owner = 0, incarnation = 0;
    const void* connection = nullptr;
    std::uint16_t zone = 0;
    void begin(std::uint32_t spawn, glm::vec2 at, std::uint64_t source = 0, std::uint64_t actor = 0,
               const void* session = nullptr, std::uint16_t area = 0) {
        id = spawn; pressed = at; cancelled = false; captured_elsewhere = false;
        owner = source; incarnation = actor; connection = session; zone = area;
    }
    bool identity_matches(std::uint64_t source, std::uint64_t actor, const void* session, std::uint16_t area) const {
        return owner == source && incarnation == actor && connection == session && zone == area;
    }
    void update(glm::vec2 at, bool blocked) {
        captured_elsewhere = captured_elsewhere || blocked;
        const glm::vec2 delta = at - pressed;
        if (blocked || glm::dot(delta, delta) > 25.0f) cancelled = true;
    }
    bool transfer_drag(bool held, bool allow_look) {
        if (!id || !cancelled || captured_elsewhere || !held || !allow_look) return false;
        clear();
        return true;
    }
    std::optional<std::uint32_t> release(glm::vec2 at, std::optional<std::uint32_t> hit, bool blocked,
                                        std::uint64_t source = 0, std::uint64_t actor = 0,
                                        const void* session = nullptr, std::uint16_t area = 0) {
        update(at, blocked);
        const auto selected = !cancelled && id == hit && identity_matches(source, actor, session, area) ? id : std::nullopt;
        id.reset();
        return selected;
    }
    void clear() { id.reset(); cancelled = false; captured_elsewhere = false; owner = incarnation = 0; connection = nullptr; zone = 0; }
};


} // namespace hailstone::app
