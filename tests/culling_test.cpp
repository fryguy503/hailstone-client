// render/culling.h: the frustum planes and the box tests the zone renderer culls its chunks with.
#include <doctest/doctest.h>

#include <random>

#include "render/actor_renderer.h"
#include "render/camera.h"
#include "render/cover_map.h"
#include "render/culling.h"
#include "render/shadows.h"

#include "app/item_preview_view.h"
#include "app/nameplate_input.h"

using namespace hailstone::render;

namespace {

Box box_around(glm::vec3 c, float half) { return {c - glm::vec3(half), c + glm::vec3(half)}; }

bool clip_inside(const glm::mat4& m, glm::vec3 p) {
    glm::vec4 c = m * glm::vec4(p, 1.0f);
    return c.w > 0.0f && std::abs(c.x) <= c.w && std::abs(c.y) <= c.w && c.z >= 0.0f && c.z <= c.w;
}

// Conservative: a box any of whose sample points the volume holds must pass.
void check_never_drops(const glm::mat4& m, glm::vec3 lo, glm::vec3 hi, float max_half) {
    Frustum f = frustum_from(m);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    int inside = 0, culled = 0;
    for (int i = 0; i < 4000; ++i) {
        glm::vec3 c = lo + (hi - lo) * glm::vec3(u(rng), u(rng), u(rng));
        glm::vec3 half = glm::vec3(u(rng), u(rng), u(rng)) * max_half;
        Box b{c - half, c + half};
        bool any = false;
        for (int k = 0; k < 27 && !any; ++k) {
            glm::vec3 t(static_cast<float>(k % 3), static_cast<float>(k / 3 % 3), static_cast<float>(k / 9));
            any = clip_inside(m, b.min + (b.max - b.min) * t * 0.5f);
        }
        bool pass = box_in_frustum(f, b);
        if (any) {
            ++inside;
            REQUIRE(pass);
        }
        culled += !pass;
    }
    CHECK(inside > 100);
    CHECK(culled > 100); // the test does cull
}

} // namespace

TEST_CASE("culling: the camera frustum keeps what is ahead and drops what is behind, aside or past the far plane") {
    FlyCamera cam;
    cam.position = {100.0f, 200.0f, 30.0f}; // yaw 0: looking along +X
    glm::mat4 vp = cam.projection(1280, 720, 2000.0f) * cam.view();
    Frustum f = frustum_from(vp);
    CHECK(box_in_frustum(f, box_around({300, 200, 30}, 5)));       // straight ahead
    CHECK(box_in_frustum(f, box_around(cam.position, 1)));          // round the camera
    CHECK_FALSE(box_in_frustum(f, box_around({-100, 200, 30}, 5))); // behind
    CHECK_FALSE(box_in_frustum(f, box_around({2300, 200, 30}, 5))); // past the far plane
    CHECK_FALSE(box_in_frustum(f, box_around({300, 900, 30}, 5)));  // far off to the left
    CHECK_FALSE(box_in_frustum(f, box_around({300, 200, 800}, 5))); // far above
    // A box across the edge of the view stays, though its centre is outside.
    CHECK(box_in_frustum(f, Box{{300, 100, 25}, {310, 900, 35}}));
    // A long, thin box crossing the view behind and ahead of the camera.
    CHECK(box_in_frustum(f, Box{{-500, 199, 29}, {700, 201, 31}}));
}

TEST_CASE("culling: the camera frustum never drops a box that shows") {
    FlyCamera cam;
    cam.position = {-50.0f, 75.0f, 10.0f};
    cam.yaw = 0.7f;
    cam.pitch = -0.3f;
    glm::mat4 vp = cam.projection(1600, 900, 1500.0f) * cam.view();
    check_never_drops(vp, glm::vec3(-1700, -1600, -1600), glm::vec3(1600, 1700, 1600), 120.0f);
}

TEST_CASE("culling: a sun cascade keeps its slice and the casters towards the light, nothing past its sides") {
    ShadowCamera cam;
    cam.position = {0, 0, 0};
    cam.forward = {1, 0, 0};
    cam.right = {0, -1, 0};
    cam.up = {0, 0, 1};
    cam.tan_half_x = 1.0f;
    cam.tan_half_y = 0.6f;
    glm::vec3 to_light = glm::normalize(glm::vec3(0.3f, -0.4f, 0.8f));
    auto s = fit_shadow_cascades(cam, to_light, 1200.0f, kShadowCasterReach, 2048);
    for (std::uint32_t c = 0; c < kShadowCascades; ++c) {
        Frustum f = frustum_from(s.view_proj[c]);
        float mid = c == 0 ? s.splits[0] * 0.5f : 0.5f * (s.splits[static_cast<int>(c) - 1] + s.splits[static_cast<int>(c)]);
        glm::vec3 centre = cam.forward * mid;
        CHECK(box_in_frustum(f, box_around(centre, 1)));
        // A caster well towards the light, outside the slice, still shadows it.
        CHECK(box_in_frustum(f, box_around(centre + to_light * (0.9f * kShadowCasterReach), 1)));
        // Beyond the light's end of the box, and well to the side, nothing.
        CHECK_FALSE(box_in_frustum(f, box_around(centre + to_light * (kShadowCasterReach + 4000.0f), 1)));
        glm::vec3 side = glm::normalize(glm::cross(to_light, glm::vec3(0, 0, 1)));
        CHECK_FALSE(box_in_frustum(f, box_around(centre + side * 5000.0f, 1)));
    }
    check_never_drops(s.view_proj[3], glm::vec3(-2500), glm::vec3(2500), 150.0f);
}

TEST_CASE("culling: the cover map's box is what its view sees") {
    CoverMap map = fit_cover_map({1000.0f, -500.0f, 40.0f}, 1024);
    Box b = cover_box(map.cover, map.params);
    // Its corners land on the map's edges and depth range.
    glm::vec4 lo = cover_clip(map, b.min), hi = cover_clip(map, b.max);
    CHECK(lo.x == doctest::Approx(-1.0f));
    CHECK(lo.y == doctest::Approx(-1.0f));
    CHECK(lo.z == doctest::Approx(1.0f));
    CHECK(hi.x == doctest::Approx(1.0f));
    CHECK(hi.y == doctest::Approx(1.0f));
    CHECK(hi.z == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(boxes_overlap(b, box_around({1000.0f, -500.0f, 40.0f}, 1)));
    CHECK_FALSE(boxes_overlap(b, box_around({1000.0f + kCoverHalfSize + 50.0f, -500.0f, 40.0f}, 1)));
    CHECK_FALSE(boxes_overlap(b, box_around({1000.0f, -500.0f, 40.0f + kCoverAbove + 50.0f}, 1)));
    CHECK_FALSE(boxes_overlap(b, box_around({1000.0f, -500.0f, 40.0f - kCoverBelow - 50.0f}, 1)));
}

TEST_CASE("culling: a caster's sphere is kept by every cascade it reaches into, dropped by the others") {
    // The actors' and doors' shadow draws test their bounding spheres so (ActorRenderer / ObjectRenderer::draw_shadow).
    Box b = sphere_box({10.0f, -20.0f, 5.0f}, 4.0f);
    CHECK(b.min == glm::vec3(6.0f, -24.0f, 1.0f));
    CHECK(b.max == glm::vec3(14.0f, -16.0f, 9.0f));
    ShadowCamera cam;
    cam.forward = {1, 0, 0};
    cam.right = {0, -1, 0};
    cam.up = {0, 0, 1};
    cam.tan_half_x = 1.0f;
    cam.tan_half_y = 0.6f;
    glm::vec3 to_light = glm::normalize(glm::vec3(0.3f, -0.4f, 0.8f));
    auto s = fit_shadow_cascades(cam, to_light, 1200.0f, kShadowCasterReach, 2048);
    Frustum near_slice = frustum_from(s.view_proj[0]);
    // At the camera: in the first cascade. Far down the view, well past its slice and to the side: not in it.
    CHECK(box_in_frustum(near_slice, sphere_box({5.0f, 0.0f, 0.0f}, 6.0f)));
    glm::vec3 side = glm::normalize(glm::cross(to_light, glm::vec3(0, 0, 1)));
    CHECK_FALSE(box_in_frustum(near_slice, sphere_box(side * 3000.0f, 6.0f)));
    // A sphere whose centre is outside but whose edge reaches in is kept.
    glm::vec3 out = side * 3000.0f;
    CHECK(box_in_frustum(near_slice, sphere_box(out, glm::length(out) + 10.0f)));
}

TEST_CASE("culling: boxes overlap when they share a point") {
    Box a{{0, 0, 0}, {10, 10, 10}};
    CHECK(boxes_overlap(a, Box{{5, 5, 5}, {6, 6, 6}}));    // inside
    CHECK(boxes_overlap(a, Box{{10, 0, 0}, {20, 10, 10}})); // touching a face
    CHECK(boxes_overlap(a, Box{{-5, -5, -5}, {20, 20, 20}})); // around it
    CHECK_FALSE(boxes_overlap(a, Box{{11, 0, 0}, {20, 10, 10}}));
    CHECK_FALSE(boxes_overlap(a, Box{{0, 0, -9}, {10, 10, -1}}));
}

TEST_CASE("culling: a spawn is posed when it can show, in the view or in a shadow's reach") {
    FlyCamera cam;
    cam.position = {100.0f, 200.0f, 30.0f}; // yaw 0: looking along +X
    Frustum f = frustum_from(cam.projection(1280, 720, 2000.0f) * cam.view());
    const glm::vec3 eye = cam.position;
    // No view set (the viewers): always.
    CHECK(actor_needs_pose(nullptr, {-5000, 0, 0}, 3, false, eye, 260));
    // In view: always, shadows or not.
    CHECK(actor_needs_pose(&f, {300, 200, 30}, 3, false, eye, 260));
    // Behind the camera: only for a shadow, within its reach (the bounds count).
    const glm::vec3 behind(0, 200, 30); // 100 behind
    CHECK_FALSE(actor_needs_pose(&f, behind, 3, false, eye, 260));
    CHECK(actor_needs_pose(&f, behind, 3, true, eye, 260));
    CHECK_FALSE(actor_needs_pose(&f, {-300, 200, 30}, 3, true, eye, 260)); // 400 behind
    CHECK(actor_needs_pose(&f, {-300, 200, 30}, 150, true, eye, 260));     // a big one, its edge within reach
    CHECK(actor_needs_pose(&f, {-300, 200, 30}, 3, true, eye, 0));        // no reach: every spawn casts
    // Partly in view: posed.
    CHECK(actor_needs_pose(&f, {95, 200, 30}, 10, false, eye, 260));
}

namespace {

// The view the frustum test below looks through.
constexpr int kViewWidth = 1280, kViewHeight = 720;
constexpr float kFarPlane = 2000.0f;

// A body-less skeleton (race 127's IVM, cut to a root and a right hand) holding one item vertex, dressed as
// CharacterLibrary::dress dresses it (held_reach). The hand's rest pose turns the item's +X reach into +Y and lifts
// it, a non-trivial bone transform; its idle ("P01") lifts the hand farther than the rest pose holds it.
struct BodilessFixture {
    static constexpr std::uint16_t kHandBone = 1;
    static constexpr float kItemReach = 20.0f;
    static constexpr float kBodyRadius = 1.0f;
    static constexpr float kHandTurnDegrees = 90.0f;
    static constexpr float kHandLift = 3.0f;
    static constexpr float kSwingLift = 4.0f;
    hailstone::assets::CharacterModel model;
    hailstone::assets::CharacterMesh mesh;
    BodilessFixture() {
        using namespace hailstone::assets;
        model.code = "IVM";
        model.bone_names = {"IVM_TRACK", "IVMR_POINT_TRACK"};
        model.parents = {-1, 0};
        model.order = {0, kHandBone};
        model.bounding_radius = kBodyRadius;
        AnimationClip rest;
        rest.tracks.resize(model.parents.size());
        rest.tracks[0].push_back(BonePose{});
        BonePose hand;
        hand.rotation = glm::angleAxis(glm::radians(kHandTurnDegrees), glm::vec3(0, 0, 1));
        hand.translation = glm::vec3(0, 0, kHandLift);
        rest.tracks[kHandBone].push_back(hand);
        AnimationClip swing = rest;
        BonePose lifted = hand;
        lifted.translation.z += kSwingLift;
        swing.tracks[kHandBone] = AnimationTrack{lifted, hand}; // starts lifted, then back to rest
        swing.frame_count = static_cast<std::uint32_t>(swing.tracks[kHandBone].size());
        model.clips[""] = rest;
        model.clips["P01"] = swing;
        SkinnedVertex v;
        v.position = glm::vec3(kItemReach, 0.0f, 0.0f); // a weapon reaching well past the authored body radius
        v.bones[0] = kHandBone;
        mesh.vertices.push_back(v);
        mesh.held_reach = dressed_reach(model, mesh);
    }
    // The item vertex's distance from the model origin with the hand at `lift`.
    static float item_distance(float lift) { return std::sqrt(kItemReach * kItemReach + lift * lift); }
    // The item vertex in `clip`'s first key, posed as the renderer poses it.
    float posed_distance(std::string_view clip) const {
        const auto* c = model.clip(clip);
        std::vector<glm::mat4> world;
        model.sample(*c, 0.0f, false, world);
        return glm::length(glm::vec3(world[kHandBone] * glm::vec4(mesh.vertices.front().position, 1.0f)));
    }
};

} // namespace

TEST_CASE("culling: a body-less skeleton's held item widens the renderer's sphere, in any pose of its clips") {
    constexpr float kDistanceAlongView = 200.0f;
    constexpr float kEdgeMargin = 10.0f;
    constexpr float kActorScale = 1.5f;
    BodilessFixture fx;
    ActorDraw a;
    a.model = &fx.model;
    a.mesh = &fx.mesh;
    a.clip = fx.model.clip("");
    a.scale = kActorScale;
    // The renderer's own radius (prepare and spawn_bounds) holds the posed item at rest and in the swing.
    const float radius = ActorRenderer::draw_radius(a);
    REQUIRE(fx.posed_distance("") == doctest::Approx(BodilessFixture::item_distance(BodilessFixture::kHandLift)));
    REQUIRE(fx.posed_distance("P01") ==
            doctest::Approx(BodilessFixture::item_distance(BodilessFixture::kHandLift + BodilessFixture::kSwingLift)));
    CHECK(radius >= fx.posed_distance("") * kActorScale);
    CHECK(radius >= fx.posed_distance("P01") * kActorScale);
    const float body = fx.model.bounding_radius * kActorScale;
    // A bare one has only its body's radius.
    hailstone::assets::CharacterMesh bare;
    a.mesh = &bare;
    CHECK(ActorRenderer::draw_radius(a) == doctest::Approx(body));

    FlyCamera cam; // at the origin, yaw 0: looking along +X
    Frustum f = frustum_from(cam.projection(kViewWidth, kViewHeight, kFarPlane) * cam.view());
    // Just outside the body sphere at the view's edge, but the item's sphere still reaches in. Z is up in EQ world
    // space, so the camera's horizontal half-extent (tan_half_x) bounds the Y offset.
    const float half_x =
        kDistanceAlongView * hailstone::game::camera_fov(cam.view_fov_units(), kViewWidth, kViewHeight).tan_half_x;
    const glm::vec3 edge(cam.position.x + kDistanceAlongView, cam.position.y - half_x - kEdgeMargin, cam.position.z);
    CHECK_FALSE(actor_needs_pose(&f, edge, body, false, {}, 0.0f));
    CHECK(actor_needs_pose(&f, edge, radius, false, {}, 0.0f));
}

TEST_CASE("item preview: a body-less skeleton is framed where its idle holds the items, not its rest pose") {
    BodilessFixture fx;
    const auto& shown = hailstone::app::ItemPreviewView::shown_clip(fx.model);
    REQUIRE(&shown == fx.model.clip("P01"));
    glm::vec3 lo, hi;
    hailstone::app::ItemPreviewView::framed_bounds(fx.model, &fx.mesh, false, lo, hi);
    // The one item vertex, where the idle's first frame puts it; the rest pose has it elsewhere.
    std::vector<glm::mat4> idle, rest;
    fx.model.sample(shown, 0.0f, false, idle);
    fx.model.sample(*fx.model.clip(""), 0.0f, false, rest);
    const glm::vec4 item(fx.mesh.vertices.front().position, 1.0f);
    const glm::vec3 shown_at(idle[BodilessFixture::kHandBone] * item);
    const glm::vec3 rest_at(rest[BodilessFixture::kHandBone] * item);
    REQUIRE(glm::length(shown_at - rest_at) > 0.0f);
    CHECK(lo.x == doctest::Approx(shown_at.x));
    CHECK(lo.y == doctest::Approx(shown_at.y));
    CHECK(lo.z == doctest::Approx(shown_at.z));
    CHECK(hi.z == doctest::Approx(shown_at.z));
    // set_item_subject's stand-in keeps the bounds it was given.
    fx.model.bounds_max = glm::vec3(BodilessFixture::kItemReach);
    hailstone::app::ItemPreviewView::framed_bounds(fx.model, &fx.mesh, true, lo, hi);
    CHECK(lo == fx.model.bounds_min);
    CHECK(hi == fx.model.bounds_max);
}

TEST_CASE("culling: a rider's radius bounds its mount's seat frame's largest stretch, not its own scale") {
    constexpr float kRiderScale = 0.5f;
    constexpr float kParentStretch = 3.0f; // a non-uniformly scaled parent bone: x 3 along X only
    constexpr float kSeatTurnDegrees = 45.0f;
    BodilessFixture fx;
    ActorDraw a;
    a.model = &fx.model;
    a.mesh = &fx.mesh;
    a.clip = fx.model.clip("");
    a.scale = kRiderScale;
    const float bound = ActorRenderer::bound_radius(fx.model, &fx.mesh);
    CHECK(ActorRenderer::draw_radius(a) == doctest::Approx(bound * kRiderScale)); // not seated: its own scale
    // Seated (ActorRenderer::seat_rider): drawn in the seat bone's frame, a turn under the stretched parent, whose
    // columns are not perpendicular. The longest column is shorter than the stretch along the parent's X.
    const glm::mat4 turn = glm::rotate(glm::mat4(1.0f), glm::radians(kSeatTurnDegrees), glm::vec3(0, 0, 1));
    a.attached = true;
    a.parent = glm::scale(glm::mat4(1.0f), glm::vec3(kParentStretch, 1.0f, 1.0f)) * turn;
    const glm::mat3 frame(a.parent);
    const float longest_column =
        std::max({glm::length(frame[0]), glm::length(frame[1]), glm::length(frame[2])});
    const glm::vec3 along_x = glm::vec3(glm::inverse(turn) * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    const float stretched = glm::length(frame * along_x);
    REQUIRE(stretched == doctest::Approx(kParentStretch));
    REQUIRE(longest_column < stretched);
    CHECK(ActorRenderer::draw_radius(a) >= bound * stretched);
}

TEST_CASE("point shadows: a spell's light beside a seated rider is not taken as inside it") {
    constexpr float kBodyRadius = 2.0f;
    constexpr float kSeatTurnDegrees = 45.0f;
    constexpr float kLightRadius = 30.0f;
    constexpr float kMidway = 0.5f;
    // A rider with a body, seated in a turned seat frame of uniform scale 1: its reach grows by the frame's bound
    // (draw_radius), its inside radius stays its own.
    hailstone::assets::CharacterModel m;
    m.bounding_radius = kBodyRadius;
    ActorDraw a;
    a.model = &m;
    a.attached = true;
    a.parent = glm::rotate(glm::mat4(1.0f), glm::radians(kSeatTurnDegrees), glm::vec3(0, 0, 1));
    a.position = glm::vec3(a.parent[3]);
    const float reach = ActorRenderer::draw_radius(a);
    const float inside = ActorRenderer::inside_radius(a);
    REQUIRE(inside == doctest::Approx(kBodyRadius));
    REQUIRE(reach > inside);
    // A spell's light between the two: beside the rider, which casts its shadow; the widened radius would have
    // taken the rider for the light's caster and left it out.
    const glm::vec3 beside = a.position + glm::vec3(inside + (reach - inside) * kMidway, 0.0f, 0.0f);
    CHECK(ActorRenderer::casts_point_shadow(beside, kLightRadius, a.position, reach, inside, true));
    CHECK_FALSE(ActorRenderer::casts_point_shadow(beside, kLightRadius, a.position, reach, reach, true));
    // Within its body it is still the caster's (or target's) light, and casts only for a light that is not a spell's.
    const glm::vec3 within = a.position + glm::vec3(inside * kMidway, 0.0f, 0.0f);
    CHECK_FALSE(ActorRenderer::casts_point_shadow(within, kLightRadius, a.position, reach, inside, true));
    CHECK(ActorRenderer::casts_point_shadow(within, kLightRadius, a.position, reach, inside, false));
    // Out of the light's reach it casts nothing.
    const glm::vec3 far = a.position + glm::vec3(kLightRadius + reach, 0.0f, 0.0f);
    CHECK_FALSE(ActorRenderer::casts_point_shadow(far, kLightRadius, a.position, reach, inside, false));
}

TEST_CASE("culling: a dressed item's bounds follow its skin binding and the bone frame of its emitters") {
    using namespace hailstone::assets;
    constexpr float kBindingShift = 5.0f;
    constexpr float kEmitterShift = 2.0f;
    BodilessFixture fx;
    // An attached piece's own palette entry, after the model's bones (an EQG item on a WLD hand): the vertex hangs
    // from the hand through the binding.
    CharacterMesh::SkinBinding binding;
    binding.host_bone = BodilessFixture::kHandBone;
    binding.local = glm::translate(glm::mat4(1.0f), glm::vec3(kBindingShift, 0.0f, 0.0f));
    fx.mesh.skin_bindings.push_back(binding);
    fx.mesh.vertices.front().bones[0] = static_cast<std::uint16_t>(fx.model.parents.size());
    glm::vec3 lo, hi;
    REQUIRE(dressed_bounds(fx.model, fx.mesh, lo, hi));
    const float through_binding = std::sqrt((BodilessFixture::kItemReach + kBindingShift) *
                                                (BodilessFixture::kItemReach + kBindingShift) +
                                            BodilessFixture::kHandLift * BodilessFixture::kHandLift);
    CHECK(glm::length(hi) == doctest::Approx(through_binding));
    CHECK(dressed_reach(fx.model, fx.mesh) >= glm::length(hi));

    // An emitter sits at its bone's own frame: with an inverse bind (EQG) its place is not the skinned one.
    CharacterMesh aura;
    MeshEmitter e;
    e.bone = BodilessFixture::kHandBone;
    e.local = glm::translate(glm::mat4(1.0f), glm::vec3(kEmitterShift, 0.0f, 0.0f));
    aura.emitters.push_back(e);
    fx.model.inverse_bind.assign(fx.model.parents.size(),
                                 glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -BodilessFixture::kHandLift)));
    REQUIRE(dressed_bounds(fx.model, aura, lo, hi));
    // The hand turns +X into +Y and lifts it: the emitter at (0, shift, lift), ignoring the inverse bind.
    CHECK(lo.y == doctest::Approx(kEmitterShift));
    CHECK(lo.z == doctest::Approx(BodilessFixture::kHandLift));
    CHECK(dressed_reach(fx.model, aura) >= glm::length(lo));
}

TEST_CASE("nameplate: a spawn without a body top is anchored at its eye, seated or not") {
    constexpr float kBodyTop = 4.0f;
    constexpr float kEye = 2.5f;
    constexpr float kSeatHeight = 10.0f;
    constexpr float kGroundHeight = 30.0f;
    const glm::vec3 base(0.0f, 0.0f, kGroundHeight); // the spawn's model origin
    const glm::vec3 seat = base + glm::vec3(0.0f, 0.0f, kSeatHeight);
    const glm::vec3 up(0.0f, 0.0f, hailstone::app::kNameplateHeadOffset);
    using hailstone::app::nameplate_head;
    CHECK(nameplate_head(base, kBodyTop, kEye, std::nullopt) == base + glm::vec3(0, 0, kBodyTop) + up);
    CHECK(nameplate_head(base, std::nullopt, kEye, std::nullopt) == base + glm::vec3(0, 0, kEye) + up);
    // A rider's top on its mount already holds its body's top; a meshless rider's is the seat itself.
    CHECK(nameplate_head(base, kBodyTop, kEye, seat) == seat + up);
    CHECK(nameplate_head(base, std::nullopt, kEye, seat) == seat + glm::vec3(0, 0, kEye) + up);
}

TEST_CASE("camera: the FOV offset widens the view without changing the chosen FOV") {
    FlyCamera chosen, widened;
    chosen.fov_units = 100.0f;
    widened.fov_units = 90.0f;
    widened.fov_offset_units = 10.0f;
    CHECK(widened.projection(1280, 720, 1000.0f) == chosen.projection(1280, 720, 1000.0f));
    CHECK(widened.fov_units == 90.0f);
    widened.fov_offset_units = 0.0f;
    chosen.fov_units = 90.0f;
    CHECK(widened.projection(1280, 720, 1000.0f) == chosen.projection(1280, 720, 1000.0f));
}

TEST_CASE("actor_phase_passes splits the actors' passes round the grass") {
    using hailstone::render::ActorPhase;
    using hailstone::render::actor_phase_passes;
    // Opaque: the opaque and alpha-tested pass alone, before the grass; Translucent: blended and additive, after it.
    CHECK(actor_phase_passes(ActorPhase::Opaque).first == 0);
    CHECK(actor_phase_passes(ActorPhase::Opaque).end == 1);
    CHECK(actor_phase_passes(ActorPhase::Translucent).first == 1);
    CHECK(actor_phase_passes(ActorPhase::Translucent).end == 3);
    CHECK(actor_phase_passes(ActorPhase::All).first == 0);
    CHECK(actor_phase_passes(ActorPhase::All).end == 3);
}
