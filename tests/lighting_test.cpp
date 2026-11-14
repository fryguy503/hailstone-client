// game/light_list.h and render/scene_lighting.h: the enhanced lighting mode's lights and the block the shaders read.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/common.hpp>
#include <glm/trigonometric.hpp>
#include <glm/geometric.hpp>

#include "assets/model.h"
#include "game/inventory.h"
#include "assets/spell_effects.h"
#include "game/light_list.h"
#include "game/world_state.h"
#include "game/particles.h"
#include "game/scene_light.h"
#include "game/sky_clock.h"
#include "game/weather.h"
#include "render/ambient_probe.h"
#include "render/actor_surface.h"
#include "render/atmosphere.h"
#include "render/scene_lighting.h"
#include "render/vk_context.h"
#include "render/weather_renderer.h"

using namespace hailstone;

TEST_CASE("actor armor surfaces: defaults keep the old path; optional maps and material highlights stay distinct") {
    assets::EffectParams effect;
    CHECK_FALSE(render::has_actor_surface(effect));
    const auto plain = render::actor_surface(effect, 0, 0);
    CHECK(plain.slots == glm::uvec4(0));
    CHECK(plain.params == glm::vec4(40.0f, 0.2f, 0.0f, 0.0f));

    effect.normal_map = "armor_n.dds";
    effect.specular = true;
    effect.reflective = true;
    effect.shininess = 80.0f;
    effect.gloss = 0.4f;
    effect.env_strength = 0.8f;
    REQUIRE(render::has_actor_surface(effect));
    const auto mapped = render::actor_surface(effect, 7, 11);
    CHECK(mapped.params == glm::vec4(80.0f, 0.4f, 0.8f, 0.0f));
    CHECK(mapped.slots.x == 7);
    CHECK(mapped.slots.y == 11);
    CHECK(mapped.slots.z == (render::kActorSurfaceSpecular | render::kActorSurfaceReflective));
    // A missing cube suppresses reflection without dropping the authored highlight or inventing a sky texture.
    const auto missing = render::actor_surface(effect, 0, 0);
    CHECK(missing.slots.x == 0); // the shader then uses the pack's normal, when present
    CHECK(missing.slots.z == render::kActorSurfaceSpecular);
    effect.reflective = false;
    CHECK(render::actor_surface(effect, 7, 11).slots.z == render::kActorSurfaceSpecular);
}

TEST_CASE("actor armor surfaces: malformed scalars and a full frame cannot produce invalid shader indexes") {
    assets::EffectParams effect;
    effect.shininess = std::numeric_limits<float>::quiet_NaN();
    effect.gloss = std::numeric_limits<float>::infinity();
    effect.env_strength = -1.0f;
    const auto bounded = render::actor_surface(effect, 0, 0);
    CHECK(bounded.params == glm::vec4(40.0f, 0.2f, 0.0f, 0.0f));
    effect.shininess = 0.0f;
    effect.gloss = -4.0f;
    effect.env_strength = std::numeric_limits<float>::max();
    CHECK(render::actor_surface(effect, 0, 0).params == glm::vec4(1.0f, 0.0f, 16.0f, 0.0f));
    CHECK(render::actor_surface_range(1, 3) == 1);
    CHECK(render::actor_surface_range(render::kMaxActorSurfaces - 1, 1) == render::kMaxActorSurfaces - 1);
    CHECK_FALSE(render::actor_surface_range(render::kMaxActorSurfaces - 1, 2));
    CHECK_FALSE(render::actor_surface_range(0, 1)); // zero belongs to the old material path
    CHECK_FALSE(render::actor_surface_range(render::kMaxActorSurfaces, 0));
    CHECK_FALSE(render::actor_surface_range(1, std::numeric_limits<std::size_t>::max()));
    // Every permitted table index survives the unused push-constant float word without rounding or denormals.
    const auto last = render::kMaxActorSurfaces - 1;
    CHECK(static_cast<std::uint32_t>(static_cast<float>(last)) == last);
}

// The shader's own kernel, compiled as C++ with GLM's float operations (shaders/ao_kernel.glsl).
namespace ao_kernel_test {
using glm::vec3;
using glm::dot;
using glm::min;
using glm::max;
using glm::clamp;
using glm::inversesqrt;
#include "../shaders/ao_kernel.glsl"
}

TEST_CASE("select_lights: dynamic lights first, then the zone's nearest, within range and the cap") {
    std::vector<assets::PointLight> zone{
        {{100, 0, 0}, {1, 0, 0}, 20.0f},  // its sphere is 80 away
        {{30, 0, 0}, {0, 1, 0}, 10.0f},   // 20 away
        {{900, 0, 0}, {0, 0, 1}, 50.0f},  // out of range
        {{5, 0, 0}, {1, 1, 1}, 0.0f},     // no radius
        {{0, 2, 0}, {1, 1, 0}, 5.0f},     // the centre is inside it
    };
    std::vector<game::LightSource> dynamic{{{0, 0, 50}, {1, 1, 1}, 60.0f}, {{0, 0, 10}, {1, 1, 1}, 60.0f}};
    auto out = game::select_lights(zone, dynamic, {0, 0, 0}, 200.0f, 32);
    REQUIRE(out.size() == 5);
    CHECK(out[0].position.z == 10.0f); // the nearer dynamic light first
    CHECK(out[1].position.z == 50.0f);
    CHECK(out[2].color == glm::vec3(1, 1, 0));
    CHECK(out[3].color == glm::vec3(0, 1, 0));
    CHECK(out[4].color == glm::vec3(1, 0, 0));
    auto capped = game::select_lights(zone, dynamic, {0, 0, 0}, 200.0f, 3);
    REQUIRE(capped.size() == 3);
    CHECK(capped[2].color == glm::vec3(1, 1, 0));
    CHECK(game::select_lights({}, {}, {0, 0, 0}, 100.0f, 8).empty());
}

TEST_CASE("carried light: the brightest worn or general-slot item, and its look") {
    game::Inventory inv;
    CHECK(game::carried_light_value(inv) == 0);
    net::Item torch;
    torch.name = "Torch";
    torch.light = 2;
    inv.put(net::ItemSlot::possessions(net::kSlotGeneral1), torch);
    net::Item stone;
    stone.name = "Lightstone";
    stone.light = 10;
    inv.put(net::ItemSlot::possessions(net::kSlotRange), stone);
    net::Item banked = stone;
    banked.light = 11;
    inv.put(net::ItemSlot{1, 0, -1, -1, 0, 0}, banked); // the bank does not count
    CHECK(game::carried_light_value(inv) == 10);

    CHECK_FALSE(game::carried_light(0, {}));
    CHECK_FALSE(game::carried_light(200, {}));
    auto t = game::carried_light(2, {1, 2, 3});
    REQUIRE(t);
    CHECK(t->position == glm::vec3(1, 2, 3));
    CHECK(t->radius > 0.0f);
    CHECK(t->color.r > t->color.b); // warm
    CHECK(game::carried_light(11, {})->radius > game::carried_light(1, {})->radius);
}

TEST_CASE("carried light follows the carrier's displayed body, independently of camera control") {
    game::Spawn spawn;
    spawn.light_type = 2;
    spawn.position = {1, 2, 3};
    auto light = game::spawn_carried_light(spawn);
    REQUIRE(light);
    CHECK(light->position == glm::vec3(1, 2, 7));
    CHECK(light->color == game::carried_light(2, {})->color);
    spawn.display_valid = true;
    spawn.display_position = {20, 30, 40};
    light = game::spawn_carried_light(spawn);
    REQUIRE(light);
    CHECK(light->position == glm::vec3(20, 30, 44));
    spawn.light_type = 0;
    CHECK_FALSE(game::spawn_carried_light(spawn));
    spawn.light_type = 255;
    CHECK_FALSE(game::spawn_carried_light(spawn));
}

TEST_CASE("char select key light: on the camera's side, above the model, reaching past it") {
    const glm::vec3 feet(0.0f, 0.0f, 1.0f), camera(20.0f, 0.0f, 8.0f);
    const float height = 7.5f;
    const glm::vec3 chest = feet + glm::vec3(0.0f, 0.0f, height * 0.6f);
    auto l = game::char_select_key_light(feet, height, camera);
    CHECK(l.position.x > 0.0f);            // towards the camera
    CHECK(l.position.y > 0.0f);            // to the camera's left (looking along -x, left is +y)
    CHECK(l.position.z > feet.z + height); // above the head
    CHECK(l.radius > glm::length(l.position - chest) * 1.5f);
    CHECK(l.color.r > l.color.b); // a little warm

    // A camera farther out (a bigger race) moves the light out with it; one straight above still gives a light.
    auto far = game::char_select_key_light(feet, height, {60.0f, 0.0f, 8.0f});
    CHECK(far.position.x > l.position.x);
    CHECK(far.radius > glm::length(far.position - chest));
    auto above = game::char_select_key_light(feet, height, feet + glm::vec3(0.0f, 0.0f, 30.0f));
    CHECK(std::isfinite(above.position.x));
    CHECK(above.radius > 0.0f);
}

TEST_CASE("particle lights: the glowing emitters, merged when close, by their tint and texture") {
    auto def = [](const char* texture, bool additive) {
        assets::EmitterDefinition d;
        d.texture = texture;
        d.additive = additive ? 1u : 0u;
        d.duration = -5.0f;
        d.initial_count = 10;
        d.width = 4.0f;
        d.height_start = 1.0f;
        return d;
    };
    CHECK(game::particle_glows(def("sparkle.dds", true)));
    CHECK_FALSE(game::particle_glows(def("smokec.dds", true)));
    CHECK(game::particle_glows(def("flamesp501.dds", false)));
    CHECK_FALSE(game::particle_glows(def("cloudsp501.dds", false)));
    auto thread = def("", true);
    thread.billboard = game::kTrailStrandBillboard;
    CHECK_FALSE(game::particle_glows(thread));
    game::ParticleSystem guides({thread});
    guides.set_still(1, 0, {game::Particle{}});
    CHECK(game::particle_lights(guides, {}, 8).empty());

    // Record 0 is never used (the EDD's numbers start at 1).
    std::vector<assets::EmitterDefinition> defs = {{}, def("sparkle.dds", true), def("cloudsp501.dds", false),
                                                   def("FLAMEsp501.dds", false)};
    defs[3].color_start = defs[3].color_end = glm::uvec3(255, 128, 0);
    game::ParticleSystem ps(defs);
    game::SpellEmitterRequest r;
    r.emitter = 1;
    r.position = {0, 0, 0};
    ps.create(r);
    r.emitter = 2; // smoke: no light
    r.position = {100, 0, 0};
    ps.create(r);
    r.emitter = 3; // flame, 5 units off: merges with the sparkle
    r.position = {5, 0, 0};
    ps.create(r);
    ps.update(0.01f, {});
    auto tint = [](const std::string& t) { return t == "FLAMEsp501.dds" ? glm::vec3(1.0f, 0.5f, 0.0f) : glm::vec3(1.0f); };
    auto lights = game::particle_lights(ps, tint, 8);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].position.x > 0.0f);
    CHECK(lights[0].position.x < 5.0f);
    CHECK(lights[0].radius >= 40.0f);
    CHECK(lights[0].color.r > lights[0].color.g); // the flame's orange in it
    CHECK(lights[0].color.b > 0.0f);              // and the sparkle's white
    CHECK(game::particle_lights(ps, tint, 0).empty());
}

TEST_CASE("make_scene_lighting: the shader block; classic carries no lights") {
    static_assert(sizeof(render::GpuPointLight) == 32);
    static_assert(sizeof(render::SceneLighting) ==
                  30 * 16 + 4 * 64 + (2 * render::kMaxPointShadows + 1) * 16 + render::kMaxSceneLights * 32);
    game::SceneLight scene;
    scene.ambient = {0.1f, 0.2f, 0.3f};
    scene.directional = {0.5f, 0.5f, 0.5f};
    scene.vision_lift = {0.15f, 0.1f, 0.0f};
    std::vector<game::LightSource> lights(40, game::LightSource{{1, 2, 3}, {0.5f, 0.25f, 1}, 30});
    auto classic = render::make_scene_lighting(scene, lights, 1, render::LightingMode::Classic);
    CHECK(classic.params == glm::vec4(0, 0, 0, 0));
    CHECK(classic.ambient == glm::vec4(0.1f, 0.2f, 0.3f, 0));
    CHECK(classic.vision == glm::vec4(0.15f, 0.1f, 0.0f, 0));
    CHECK(classic.lights[0].position_radius == glm::vec4(0));
    render::Daylight sky;
    sky.light = {2, 2, 2};
    sky.baked = 0.25f;
    sky.baked_lamps = 0.5f;
    auto modern = render::make_scene_lighting(scene, lights, 1, render::LightingMode::Modern, sky);
    CHECK(modern.light == glm::vec4(2, 2, 2, 0));
    CHECK(modern.sky.w == 0.25f);    // the baked colour's share (shaders/zone.frag)
    CHECK(modern.ground.w == 0.5f);  // and its lamps'
    CHECK(modern.params.x == 1.0f);
    CHECK(modern.params.y == static_cast<float>(render::kMaxSceneLights)); // cut to the cap
    CHECK(modern.params.z == 1.0f);
    CHECK(modern.lights[31].position_radius == glm::vec4(1, 2, 3, 30));
    CHECK(modern.lights[31].color == glm::vec4(0.5f, 0.25f, 1, 0));
    auto few = render::make_scene_lighting(scene, std::span(lights).first(2), 5, render::LightingMode::Modern);
    CHECK(few.params.y == 2.0f);
    CHECK(few.params.z == 2.0f); // no more dynamic lights than lights
}

TEST_CASE("lighting modes by name: enhanced is the old name of modern") {
    CHECK(render::parse_lighting_mode("modern") == render::LightingMode::Modern);
    CHECK(render::parse_lighting_mode("enhanced") == render::LightingMode::Modern);
    CHECK(render::parse_lighting_mode("classic") == render::LightingMode::Classic);
    CHECK(!render::parse_lighting_mode("bright"));
}

TEST_CASE("daylight: the sun high in the south at noon, red and low at dusk, the moon by night, none indoors") {
    auto noon = render::daylight(12.0f, true);
    CHECK(noon.to_light.z == doctest::Approx(std::sin(glm::radians(render::kSkyNoonElevation))).epsilon(0.001));
    CHECK(noon.to_light.x < -0.5f); // south (-X; +X is north)
    CHECK(std::abs(noon.to_light.y) < 1e-4f);
    CHECK(noon.light.r > 2.5f);
    CHECK(noon.light.b > 0.7f * noon.light.r); // nearly white
    CHECK(noon.sky.b > noon.sky.r);            // a blue sky

    auto evening = render::daylight(17.5f, true); // 7.5 degrees above the horizon
    CHECK(evening.to_light.z > 0.0f);
    CHECK(evening.to_light.z < 0.2f);
    CHECK(evening.light.r > evening.light.g);
    CHECK(evening.light.g > evening.light.b);
    CHECK(evening.light.r < noon.light.r);

    auto midnight = render::daylight(24.0f, true);
    CHECK(midnight.to_light.z > 0.8f); // the full moon, opposite the sun (a little off its path), high in the south
    CHECK(midnight.to_light.x < 0.0f);
    CHECK(midnight.light.b > midnight.light.r);
    CHECK(midnight.light.b < 0.5f);
    CHECK(midnight.sky.b < 0.2f);

    // The sun's elevation is continuous through the day: morning mirrors afternoon.
    auto morning = render::daylight(9.0f, true);
    auto afternoon = render::daylight(15.0f, true);
    CHECK(morning.to_light.z == doctest::Approx(afternoon.to_light.z).epsilon(0.001));
    CHECK(morning.to_light.y == doctest::Approx(-afternoon.to_light.y).epsilon(0.001));

    auto inside = render::daylight(12.0f, false);
    CHECK(inside.light == glm::vec3(0.0f));
    CHECK(inside.sky.r > 0.0f);
}

TEST_CASE("daylight: the moon through its phases") {
    CHECK(render::moon_lit(0.0f) == doctest::Approx(0.0f));
    CHECK(render::moon_lit(0.25f) == doctest::Approx(0.5f));
    CHECK(render::moon_lit(0.5f) == doctest::Approx(1.0f));

    // New: beside the sun. First quarter: high at sunset (where the sun was at noon), set by morning.
    glm::vec3 noon_sun = render::daylight(12.0f, true).to_sun;
    CHECK(glm::dot(render::moon_direction(12.0f, 0.0f), noon_sun) > 0.99f);
    CHECK(glm::dot(render::moon_direction(12.0f, 0.0f), noon_sun) < 0.9999f); // never right over it
    CHECK(render::moon_direction(18.0f, 0.25f).z > 0.8f);
    CHECK(render::moon_direction(6.0f, 0.25f).z < -0.7f);
    // Full: rises as the sun sets.
    CHECK(std::abs(render::moon_direction(18.0f, 0.5f).z) < 0.06f);
    CHECK(render::moon_direction(18.0f, 0.5f).y * render::daylight(18.0f, true).to_sun.y < 0.0f);

    // The night's light: the full moon's, more than the new moon's (still above nothing) and none with it down.
    auto full = render::daylight(24.0f, true, 0.5f);
    auto quarter = render::daylight(24.0f, true, 0.75f); // last quarter: rising at midnight
    auto down = render::daylight(24.0f, true, 0.1f);     // a young moon: set in the evening
    CHECK(full.moon_lit == doctest::Approx(1.0f));
    CHECK(glm::length(full.light) > glm::length(quarter.light));
    CHECK(down.light == glm::vec3(0.0f));
    CHECK(full.sky.b > down.sky.b); // the sky it lights
    CHECK(down.sky.b > 0.05f);      // the starlight: never black
    // The full moon overhead gives kFullMoon; the waxing gibbous (three quarters lit) a little over half of it.
    CHECK(full.light.b == doctest::Approx(render::kFullMoon.b).epsilon(0.02));
    float lit = render::moon_lit(0.333f);
    CHECK(0.125f + 0.875f * lit * lit < 0.65f);

    // The blood moon: the same strength, red.
    auto blood = render::daylight(24.0f, true, 0.5f, 1.0f);
    CHECK(blood.blood_moon == 1.0f);
    CHECK(blood.light.r > 3.0f * blood.light.g);
    CHECK(blood.sky.r > blood.sky.b * 0.5f);
}

TEST_CASE("sky clock: the blood moon on every third month's full moon; the northern lights some nights") {
    game::SkyClock c;
    c.set(1, 0, 0.0, 15, 3); // the 15th of the 3rd month, as it begins: full
    CHECK(c.blood_moon(0.0) == doctest::Approx(1.0f));
    c.set(1, 0, 0.0, 15, 4); // the 4th month: none
    CHECK(c.blood_moon(0.0) == 0.0f);
    c.set(24, 0, 0.0, 12, 6); // three days before full: none
    CHECK(c.blood_moon(0.0) == 0.0f);
    c.set(1, 0, 0.0, 15, 0); // the month unknown: none
    CHECK(c.blood_moon(0.0) == 0.0f);
    // It fades: a day of game time (72 real minutes) further on it is less.
    c.set(13, 0, 0.0, 15, 12);
    float at = c.blood_moon(0.0), later = c.blood_moon(72.0 * 60.0);
    CHECK(at > 0.5f);
    CHECK(later < at);
    // The northern lights: the same for a night, none on most nights, 0.3..1 on the rest.
    int nights = 0, lit = 0;
    for (int month = 1; month <= 12; ++month)
        for (int day = 1; day <= 28; ++day) {
            c.set(22, 0, 0.0, day, month);
            float a = c.aurora(0.0);
            CHECK(a == c.aurora(3.0 * 60.0 * 3.0)); // three game hours on, the same night
            CHECK((a == 0.0f || (a >= 0.3f && a <= 1.0f)));
            ++nights;
            if (a > 0.0f) ++lit;
        }
    CHECK(lit > nights / 5);
    CHECK(lit < nights * 3 / 5);
}

TEST_CASE("daylight: the zone's baked colour by day, by night and indoors") {
    // By day all of it, as the builders lit it; by night a dim remainder, the lamps' part too (the pools carry them).
    auto noon = render::daylight(12.0f, true);
    CHECK(noon.baked == doctest::Approx(1.0f));
    CHECK(noon.baked_lamps == doctest::Approx(1.0f));
    CHECK(render::daylight(16.0f, true).baked == doctest::Approx(1.0f)); // the sun 24 degrees up
    auto full = render::daylight(24.0f, true, 0.5f);
    auto down = render::daylight(24.0f, true, 0.1f);
    CHECK(full.baked == doctest::Approx(render::kNightBaked + render::kMoonBaked));
    CHECK(down.baked == doctest::Approx(render::kNightBaked)); // the moon down: none of its share
    CHECK(full.baked_lamps == doctest::Approx(render::kNightBakedLamps));
    CHECK(full.baked_lamps < 0.5f);
    // Dusk falls between.
    auto dusk = render::daylight(18.0f, true, 0.5f);
    CHECK(dusk.baked > down.baked);
    CHECK(dusk.baked < 1.0f);
    // Indoors the baked colour is the only light: most of it stays, day or night.
    auto inside = render::daylight(12.0f, false);
    CHECK(inside.baked == doctest::Approx(render::kIndoorBaked));
    CHECK(inside.baked_lamps == doctest::Approx(render::kIndoorBakedLamps));
    CHECK(render::daylight(24.0f, false).baked == inside.baked);
    // A storm dims it a little.
    render::Daylight storm = noon;
    render::apply_storm(storm, 1.0f);
    CHECK(storm.baked < noon.baked);
    CHECK(storm.baked > 0.5f);
}

TEST_CASE("under water: a blue-green medium lit from above, thick at every height, absorbing") {
    glm::vec3 noon = render::underwater_color(render::daylight(12.0f, true));
    glm::vec3 night = render::underwater_color(render::daylight(24.0f, true));
    CHECK(noon.g > noon.r);
    CHECK(noon.b > noon.r);
    CHECK(noon.g > 0.3f); // display values
    CHECK(night.g < noon.g * 0.5f);
    render::SceneLighting l;
    render::set_underwater(l, {1, 2, 3}, noon);
    CHECK(l.haze.x == doctest::Approx(1.0f / render::kUnderwaterReach));
    CHECK(l.haze.y == 0.0f);
    CHECK(l.haze.z == 2.0f);
    CHECK(l.haze_color.g < noon.g); // linear
    CHECK(l.camera == glm::vec4(1, 2, 3, 1));
}

TEST_CASE("modern view distance and haze") {
    CHECK(render::extended_far_plane(1000.0f, 2.5f) == 2500.0f);
    CHECK(render::extended_far_plane(1000.0f, 0.5f) == 1000.0f); // never nearer than the original
    CHECK(render::extended_far_plane(9000.0f, 3.0f) == render::kMaxViewDistance);
    render::SceneLighting l;
    render::set_haze(l, {1, 2, 3}, 2000.0f, {1.0f, 0.5f, 0.0f});
    CHECK(l.haze.x == doctest::Approx(1.0 / 2000.0));
    CHECK(l.haze.z == 1.0f);
    CHECK(l.haze_color.r == doctest::Approx(1.0));
    CHECK(l.haze_color.g == doctest::Approx(0.214).epsilon(0.01)); // 0.5 in linear
    CHECK(l.camera == glm::vec4(1, 2, 3, 1));
    CHECK(l.haze.w == doctest::Approx(render::kHazeGlow)); // a bright haze glows fully round the sun
}

TEST_CASE("the haze's sun glow follows its own brightness") {
    // A day's fog glows fully; nektulos's 80/90/70 at noon is one.
    CHECK(render::haze_glow(glm::vec3(0.5f)) == doctest::Approx(render::kHazeGlow));
    CHECK(render::haze_glow(glm::vec3(0.080f, 0.103f, 0.061f)) == doctest::Approx(render::kHazeGlow));
    // Its fog at 5:50 (SetFog's hour tables: 19/14/10) is nearly black: the glow is a tenth of it, not a flat sunlit
    // wash in front of a black backdrop.
    render::SceneLighting dawn;
    render::set_haze(dawn, {0, 0, 0}, 400.0f, glm::vec3(0.075f, 0.055f, 0.039f));
    CHECK(dawn.haze.w > 0.0f);
    CHECK(dawn.haze.w < 0.1f * render::kHazeGlow);
    // What it adds at the sun under a 2.8 sun stays within a few times the haze's own colour.
    CHECK(2.8f * dawn.haze.w * 0.25f < 5.0f * dawn.haze_color.r);
    CHECK(render::haze_glow(glm::vec3(0.0f)) == 0.0f);
    // In step with the brightness, never past full.
    CHECK(render::haze_glow(glm::vec3(0.01f)) < render::haze_glow(glm::vec3(0.02f)));
    CHECK(render::haze_glow(glm::vec3(2.0f)) == doctest::Approx(render::kHazeGlow));
    // Under water there is none.
    render::SceneLighting water;
    render::set_underwater(water, {0, 0, 0}, glm::vec3(0.2f, 0.5f, 0.6f));
    CHECK(water.haze.w == 0.0f);
}

TEST_CASE("modern view: drawn further unless in a special region") {
    game::ViewDistance v;
    v.far_plane = 1000.0f;
    v.fog_start = 300.0f;
    v.fog_end = 850.0f;
    auto out = render::modern_view(v, 2.5f, true);
    CHECK(out.view.far_plane == 2500.0f);
    CHECK(out.view.fog_end == 2500.0f);
    CHECK(out.haze_length == 2500.0f);
    auto water = render::modern_view(v, 2.5f, false);
    CHECK(water.view.far_plane == 1000.0f);
    CHECK(water.haze_length == 850.0f);
}

TEST_CASE("sun shafts: on screen, off screen, behind and set") {
    // Looking along +X with a simple projection: clip = (y-ish, z-ish) / x.
    glm::mat4 vp(0.0f);
    vp[1][0] = -1.0f; // clip.x = -y (screen right is -Y, as the client's camera looking north)
    vp[2][1] = 1.0f;  // clip.y = z
    vp[0][3] = 1.0f;  // clip.w = x
    auto ahead = render::sun_shafts_for(vp, glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f)), glm::vec3(1.0f));
    CHECK(ahead.strength == doctest::Approx(1.0f));
    CHECK(ahead.uv.x == doctest::Approx(0.5f));
    CHECK(ahead.uv.y < 0.5f); // above the middle (the viewport is flipped)
    auto far_side = render::sun_shafts_for(vp, glm::normalize(glm::vec3(1.0f, -3.0f, 0.3f)), glm::vec3(1.0f));
    CHECK(far_side.strength == 0.0f); // three screens' halves to the right
    auto behind = render::sun_shafts_for(vp, glm::normalize(glm::vec3(-1.0f, 0.0f, 0.3f)), glm::vec3(1.0f));
    CHECK(behind.strength == 0.0f);
    auto set = render::sun_shafts_for(vp, glm::normalize(glm::vec3(1.0f, 0.0f, -0.2f)), glm::vec3(1.0f));
    CHECK(set.strength == 0.0f);
}

TEST_CASE("render scale: the scene targets' size for the 3D view, kept in 50..100 percent") {
    auto same = [](VkExtent2D a, std::uint32_t w, std::uint32_t h) { return a.width == w && a.height == h; };
    CHECK(same(render::scaled_extent({1920, 1080}, 1.0f), 1920, 1080));
    CHECK(same(render::scaled_extent({1920, 1080}, 0.5f), 960, 540));
    CHECK(same(render::scaled_extent({2560, 1440}, 0.67f), 1715, 965)); // each side rounded
    CHECK(same(render::scaled_extent({1920, 1080}, 0.1f), 960, 540));   // no lower than half
    CHECK(same(render::scaled_extent({1920, 1080}, 2.0f), 1920, 1080)); // never above the view
    CHECK(same(render::scaled_extent({1, 1}, 0.5f), 1, 1));             // at least a pixel
}

TEST_CASE("weather look: a new zone starts it over; the snow lying in one does not follow into the next") {
    game::Weather weather;
    weather.handle_message(1, 10); // snow, the heaviest
    auto steady = [] { return std::int32_t{0x4000}; };
    render::WeatherLook look;
    double t = 0.0;
    for (; t < 200.0; t += 0.1) {
        weather.update(t, true, steady, 86); // the Dreadlands
        look.update(weather, 0.1f, 86);
    }
    CHECK(look.snow > 0.5f);
    CHECK(look.storm > 0.5f);
    // The zone id unknown (0) keeps it: a zone-in has not told it yet.
    look.update(weather, 0.1f, 0);
    CHECK(look.snow > 0.5f);
    // The Bazaar: nothing falls there, and nothing lies there either.
    weather.handle_message(0xff, 0);
    look.update(weather, 0.1f, 151);
    CHECK(look.snow < 0.001f); // a frame's worth: the snow still falling until the weather's next step
    CHECK(look.storm < 0.01f);
    CHECK(look.wet == 0.0f);
    CHECK(look.puddles == 0.0f);
    for (; t < 260.0; t += 0.1) {
        weather.update(t, true, steady, 151);
        look.update(weather, 0.1f, 151);
    }
    CHECK(look.snow == 0.0f);
    CHECK(look.storm == 0.0f);
}

TEST_CASE("weather look: rain soaks, then puddles fill and dry slower; the rings only while it falls") {
    game::Weather weather;
    weather.handle_message(0, 10); // rain, the heaviest
    auto steady = [] { return std::int32_t{0x4000}; };
    render::WeatherLook look;
    double t = 0.0;
    auto run = [&](double seconds) {
        for (double end = t + seconds; t < end; t += 0.1) {
            weather.update(t, true, steady);
            look.update(weather, 0.1f);
        }
    };
    run(40.0);
    CHECK(look.wet == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(look.puddles > 0.2f);
    CHECK(look.puddles < 0.6f); // still filling
    CHECK(look.rain == doctest::Approx(1.0f).epsilon(0.01));
    run(80.0);
    CHECK(look.puddles == doctest::Approx(1.0f));
    weather.handle_message(0xff, 0);
    run(130.0);
    CHECK(look.rain == 0.0f);
    CHECK(look.wet == 0.0f);      // the ground dried
    CHECK(look.puddles > 0.3f);   // the puddles not yet
    run(120.0);
    CHECK(look.puddles == 0.0f);

    render::SceneLighting l;
    look.snow = 0.25f;
    look.flash = 0.5f;
    render::set_weather(l, look);
    CHECK(l.weather == glm::vec4(look.wet, 0.25f, look.rain, look.puddles));
    CHECK(l.time.z == 0.5f);
}

TEST_CASE("the sky's path: the sun rises in the east, stands south at noon, sets in the west; the pole is north") {
    // World +X is north, -Y east (game/coords.h: the server's heading 0, north, faces world +X).
    glm::vec3 rise = render::daylight(6.0f, true).to_sun, set = render::daylight(18.0f, true).to_sun;
    CHECK(rise.y == doctest::Approx(-1.0f).epsilon(0.001));
    CHECK(set.y == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(std::abs(rise.z) < 1e-4f);
    glm::vec3 pole = render::sky_pole();
    CHECK(pole.x > 0.7f);
    CHECK(pole.z == doctest::Approx(std::cos(glm::radians(render::kSkyNoonElevation))).epsilon(0.001));
    // The sun keeps its angle to the pole through the day (a circle about it).
    for (float h : {3.0f, 9.0f, 12.0f, 15.0f, 21.0f})
        CHECK(glm::dot(render::daylight(h, true).to_sun, pole) == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("the sky's light follows the modelled sky: blue by day, warmer and dimmer low, none at night") {
    auto noon = render::sky_light(render::daylight(12.0f, true).to_sun);
    CHECK(noon.ambient.b > noon.ambient.r);
    float l = glm::dot(noon.ambient, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    CHECK(l > 0.3f);
    CHECK(l < 0.8f);
    // The horizon is near white by day (ozone), not green.
    CHECK(noon.horizon.g < 1.1f * std::max(noon.horizon.r, noon.horizon.b));
    auto low = render::sky_light(render::daylight(17.6f, true).to_sun);
    CHECK(low.horizon_sun.r > low.horizon_sun.b); // the sky towards a low sun is warm
    CHECK(glm::length(low.ambient) < glm::length(noon.ambient));
    auto night = render::sky_light(render::daylight(0.0f, true).to_sun);
    CHECK(glm::length(night.ambient) < 1e-3f);
    // The daylight's ambient: the model's by day, the night's floor by night.
    CHECK(render::daylight(12.0f, true).sky.b > 0.3f);
    CHECK(render::daylight(0.0f, true, 0.0f).sky.b > 0.05f);
}

TEST_CASE("the haze eases between places; a reset takes the new one at once") {
    render::HazeBlend blend;
    render::HazeTarget a{glm::vec3(0.5f), 4000.0f, 1.0f}, b{glm::vec3(0.1f, 0.0f, 0.0f), 300.0f, 0.0f};
    CHECK(blend.update(a, 0.016f).length == 4000.0f); // the first frame: at once
    float half = blend.update(b, 0.5f).length;
    CHECK(half < 4000.0f);
    CHECK(half > 300.0f);
    for (int i = 0; i < 200; ++i) blend.update(b, 0.016f);
    CHECK(blend.current().length == doctest::Approx(300.0f).epsilon(0.01));
    CHECK(blend.current().extend == 0.0f);
    blend.reset();
    CHECK(blend.update(a, 0.016f).color == glm::vec3(0.5f));
}

TEST_CASE("modern view: part of the way between the original's view and the extended one") {
    game::ViewDistance v;
    v.far_plane = 1000.0f;
    v.fog_start = 100.0f;
    v.fog_end = 900.0f;
    auto none = render::modern_view(v, 4.0f, 0.0f), all = render::modern_view(v, 4.0f, 1.0f),
         half = render::modern_view(v, 4.0f, 0.5f);
    CHECK(none.view.far_plane == 1000.0f);
    CHECK(all.view.far_plane == 4000.0f);
    CHECK(half.view.far_plane == doctest::Approx(2500.0f));
    CHECK(half.haze_length == doctest::Approx(0.5f * (none.haze_length + all.haze_length)));
}

TEST_CASE("the haze outdoors: towards the sky's horizon, less in a thick fog; the weather's band") {
    glm::vec3 fog(0.5f, 0.5f, 0.6f), horizon(0.3f, 0.32f, 0.34f);
    glm::vec3 clear = render::haze_sky_color(fog, horizon, true, 2000.0f);
    glm::vec3 thick = render::haze_sky_color(fog, horizon, true, 400.0f);
    CHECK(thick == fog);
    CHECK(clear.b < fog.b);
    CHECK(render::haze_sky_color(fog, horizon, false, 2000.0f).b > clear.b); // no sky drawn: more of the fog
    // A dim fog by day gets no glare from a bright horizon.
    glm::vec3 dim(0.01f), bright(1.0f);
    CHECK(render::haze_sky_color(dim, bright, true, 2000.0f).r < 0.05f);

    render::HazeTarget h{glm::vec3(0.4f, 0.5f, 0.7f), 4000.0f, 1.0f};
    render::HazeTarget rain = h, snow = h;
    render::weather_haze(rain, 1.0f, 0.0f);
    render::weather_haze(snow, 1.0f, 1.0f);
    CHECK(rain.length == doctest::Approx(4000.0f * (1.0f - render::kStormHazeReach)));
    CHECK(snow.length < rain.length);
    float l = glm::dot(h.color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    CHECK(glm::dot(rain.color, glm::vec3(0.2126f, 0.7152f, 0.0722f)) < l);
    CHECK(glm::dot(snow.color, glm::vec3(0.2126f, 0.7152f, 0.0722f)) > l);
    CHECK(rain.color.b - rain.color.r < h.color.b - h.color.r); // greyer
    CHECK(render::lightning_haze(h.color, 1.0f, 1.0f).b > h.color.b + 0.1f);
}

TEST_CASE("weather look: the clouds gather and clear over tens of seconds; the drops thicken and thin") {
    game::Weather weather;
    weather.handle_message(0, 10);
    auto steady = [] { return std::int32_t{0x4000}; };
    render::WeatherLook look;
    double t = 0.0;
    auto run = [&](double seconds) {
        for (double end = t + seconds; t < end; t += 0.1) {
            weather.update(t, true, steady);
            look.update(weather, 0.1f);
        }
    };
    run(5.0);
    CHECK(weather.precipitation() == doctest::Approx(0.1f)); // the original: there within a second
    CHECK(look.storm > 0.05f);
    CHECK(look.storm < 0.5f);
    CHECK(look.precipitation < 0.08f);
    run(60.0);
    CHECK(look.storm > 0.95f);
    CHECK(look.precipitation == doctest::Approx(0.1f).epsilon(0.02));
    weather.handle_message(0xff, 0);
    run(10.0);
    CHECK(weather.precipitation() == 0.0f);
    CHECK(look.precipitation > 0.01f); // still thinning
    CHECK(look.storm > 0.5f);          // the clouds slower to break
    run(120.0);
    CHECK(look.storm == 0.0f);
    CHECK(look.precipitation == 0.0f);
    // Snow takes over in a few seconds.
    weather.handle_message(1, 5);
    run(20.0);
    CHECK(look.snowing > 0.95f);
}

TEST_CASE("lightning: three flashes over a fifth of a second and an afterglow") {
    CHECK(render::lightning_flash(0.0f) == doctest::Approx(1.0f).epsilon(0.13));
    CHECK(render::lightning_flash(0.05f) < 0.4f);
    CHECK(render::lightning_flash(0.07f) > render::lightning_flash(0.06f)); // the second
    CHECK(render::lightning_flash(0.19f) > render::lightning_flash(0.17f)); // the third
    CHECK(render::lightning_flash(1.0f) < 0.01f);
    CHECK(render::lightning_flash(5.0f) == 0.0f);
}

TEST_CASE("colour grade: warm at sunset, cool and paler at night, greyer in a storm, neutral in brightness") {
    glm::vec3 fog(0.3f);
    auto noon = render::color_grade(render::daylight(12.0f, true), true, fog, 0.0f);
    auto dusk = render::color_grade(render::daylight(17.7f, true), true, fog, 0.0f);
    auto night = render::color_grade(render::daylight(0.0f, true), true, fog, 0.0f);
    auto storm = render::color_grade(render::daylight(12.0f, true), true, fog, 1.0f);
    const glm::vec3 lum(0.2126f, 0.7152f, 0.0722f);
    for (const auto& g : {noon, dusk, night, storm}) CHECK(glm::dot(g.tint, lum) == doctest::Approx(1.0f));
    CHECK(dusk.tint.r > dusk.tint.b);
    CHECK(night.tint.b > night.tint.r);
    CHECK(night.saturation < noon.saturation);
    CHECK(storm.saturation < noon.saturation);
    // Subtle: never far from neutral.
    for (const auto& g : {noon, dusk, night, storm}) {
        CHECK(std::abs(g.tint.r - 1.0f) < 0.15f);
        CHECK(std::abs(g.saturation - 1.0f) < 0.3f);
    }
    // The zone's fog hue shows a little: unrest's purple.
    auto purple = render::color_grade(render::daylight(12.0f, false), false, glm::vec3(0.02f, 0.003f, 0.045f), 0.0f);
    CHECK(purple.tint.b > purple.tint.g);
}

TEST_CASE("ambient probe: the baked light of the place, nearest surfaces first; eased between places") {
    render::AmbientProbe probe;
    CHECK(probe.sample(glm::vec3(0.0f)).w < 0.0f); // nothing
    // A dark room (no sky) at the origin, a bright open yard 200 units away.
    for (int i = 0; i < 20; ++i) probe.add(glm::vec3(float(i % 5) * 4.0f, float(i / 5) * 4.0f, 0.0f), 0x00202020u);
    for (int i = 0; i < 20; ++i) probe.add(glm::vec3(200.0f + float(i % 5) * 4.0f, float(i / 5) * 4.0f, 0.0f), 0xffc0c0c0u);
    glm::vec4 room = probe.sample(glm::vec3(8.0f, 8.0f, 3.0f)), yard = probe.sample(glm::vec3(208.0f, 8.0f, 3.0f));
    CHECK(room.r == doctest::Approx(0x20 / 255.0f));
    CHECK(room.a == doctest::Approx(0.0f));
    CHECK(yard.a == doctest::Approx(1.0f));
    CHECK(probe.sample(glm::vec3(1000.0f)).w < 0.0f); // out of reach

    render::ProbeBlend blend;
    glm::vec4 first = blend.update(room, 0.016f);
    CHECK(first.w == doctest::Approx(1.0f)); // 1 + the sky's share
    glm::vec4 step = blend.update(yard, 0.1f);
    CHECK(step.w > 1.0f);
    CHECK(step.w < 1.9f);
    CHECK(blend.update(probe.sample(glm::vec3(1000.0f)), 0.1f) == blend.update(probe.sample(glm::vec3(1000.0f)), 0.1f));
}

TEST_CASE("classic_actor_light: the region shaders' light for the baked light where the player stands") {
    render::SceneLighting l;
    l.ambient = glm::vec4(0.2f, 0.2f, 0.2f, 0.0f);
    l.special_ambient = glm::vec4(0.05f, 0.05f, 0.05f, 0.0f);
    l.directional = glm::vec4(0.5f, 0.5f, 0.5f, 0.0f);
    l.direction = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f); // from straight above
    const glm::vec3 up(0.0f, 0.0f, 1.0f), down(0.0f, 0.0f, -1.0f);

    // No probe: no baked light, the sky's full share.
    glm::vec3 top = render::classic_actor_light(l, up);
    CHECK(top.x == doctest::Approx(0.75f));
    // Facing away from the sun: the ambients, raised to the floor.
    l.ambient = glm::vec4(0.1f, 0.1f, 0.1f, 0.0f);
    CHECK(render::classic_actor_light(l, down).x == doctest::Approx(render::kClassicActorFloor));

    // A bright baked place at night (the Bazaar's walls): the baked colour carries it, the sky only by its share.
    l.directional = glm::vec4(0.0f);
    l.probe = glm::vec4(0.7f, 0.6f, 0.5f, 1.5f);
    glm::vec3 lit = render::classic_actor_light(l, up);
    CHECK(lit.x == doctest::Approx(0.7f + 0.05f + 0.5f * 0.1f));
    CHECK(lit.z == doctest::Approx(0.5f + 0.05f + 0.5f * 0.1f));
    // Never past 1.
    l.probe = glm::vec4(1.0f, 1.0f, 1.0f, 2.0f);
    CHECK(render::classic_actor_light(l, up) == glm::vec3(1.0f));

    // Full bright (the item preview, and the renderer before it has a scene light): the texture as it is.
    CHECK(render::classic_actor_light(render::classic_full_bright(), down) == glm::vec3(1.0f));
    CHECK(render::classic_full_bright().params.x == 0.0f);
}

TEST_CASE("ao kernel radius: the world radius far off, the pixels' own size capped close up") {
    // Far off the 72 px cap never bites: 24 px, and the world radius comes back out.
    CHECK(ao_kernel_test::ao_kernel_radius(4.0f, 24.0f, 100.0f, 0.6f, 720.0f) == doctest::Approx(4.0f));
    // Close up the cap governs: 1200 px cut to 72, the kernel shrinks to those pixels' world size.
    float r_px = glm::min(4.0f / (2.0f * 0.6f) * 0.5f * 720.0f, 72.0f);
    CHECK(r_px == doctest::Approx(72.0f));
    CHECK(ao_kernel_test::ao_kernel_radius(4.0f, r_px, 2.0f, 0.6f, 720.0f) == doctest::Approx(0.24f));
    // The same view in more pixels: doubling both keeps the world radius.
    CHECK(ao_kernel_test::ao_kernel_radius(4.0f, 50.0f, 10.0f, 0.6f, 720.0f) ==
          doctest::Approx(ao_kernel_test::ao_kernel_radius(4.0f, 100.0f, 10.0f, 0.6f, 1440.0f)));
}

TEST_CASE("ao sample: flat, convex and distant samples occlude nothing; a concave one does") {
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    CHECK(ao_kernel_test::ao_sample(glm::vec3(1.0f, 0.0f, 0.0f), n, 4.0f, 0.01f) == 0.0f); // coplanar
    CHECK(ao_kernel_test::ao_sample(glm::vec3(0.0f, 0.0f, -1.0f), n, 4.0f, 0.01f) == 0.0f); // behind
    CHECK(ao_kernel_test::ao_sample(glm::vec3(0.0f, 0.0f, 10.0f), n, 4.0f, 0.01f) == 0.0f); // past the radius
    // Nearly coplanar facets must not acquire contact shadows from their tiny angle alone.
    CHECK(ao_kernel_test::ao_sample(glm::vec3(1.0f, 0.0f, 0.05f), n, 4.0f, 0.0f) == 0.0f);
    float corner = ao_kernel_test::ao_sample(glm::vec3(0.0f, 0.0f, 1.0f), n, 4.0f, 0.01f);
    CHECK(corner > 0.0f);
    CHECK(corner <= 1.0f);
    // A zero offset stays finite, and more bias only takes occlusion away.
    CHECK(ao_kernel_test::ao_sample(glm::vec3(0.0f), n, 4.0f, 0.0f) == 0.0f);
    CHECK(std::isfinite(ao_kernel_test::ao_sample(glm::vec3(0.0f), n, 4.0f, 0.01f)));
    float biased = ao_kernel_test::ao_sample(glm::vec3(0.0f, 0.0f, 1.0f), n, 4.0f, 0.5f);
    CHECK(biased > 0.0f);
    CHECK(biased < corner);
}

TEST_CASE("ao sample: dimensionless, so one shape at any scale occludes the same") {
    const glm::vec3 n(0.0f, 0.0f, 1.0f), o(1.0f, 2.0f, 2.0f);
    float a = ao_kernel_test::ao_sample(o, n, 6.0f, 0.06f);
    CHECK(a > 0.0f);
    CHECK(ao_kernel_test::ao_sample(o * 2.5f, n, 15.0f, 0.15f) == doctest::Approx(a));
    CHECK(ao_kernel_test::ao_sample(o * 0.1f, n, 0.6f, 0.006f) == doctest::Approx(a));
}

TEST_CASE("ao sample: at the default intensity, a mid-range occluder shades about as the old kernel did") {
    // The kernel before ao_kernel.glsl, with its own scale (intensity 4, x 5, normalised by r^6): its weight grew as
    // 1/|v|, which blotched near samples black. Mid-range (1 to 3 units, 30 to 60 degrees above the surface), the
    // bounded kernel at render::AoView's intensity must stay within a factor of two of it, or contact shadows fade.
    const float radius = 4.0f, samples = 12.0f, intensity = render::AoView{}.intensity;
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    auto old_shade = [&](const glm::vec3& v) {
        const float vv = glm::dot(v, v), f = std::max(radius * radius - vv, 0.0f);
        return f * f * f * std::max(glm::dot(v, n) / (vv + 0.01f), 0.0f) * 4.0f * 5.0f /
               (std::pow(radius, 6.0f) * samples);
    };
    for (float d : {1.0f, 2.0f, 3.0f})
        for (float deg : {30.0f, 45.0f, 60.0f}) {
            const float a = glm::radians(deg);
            const glm::vec3 v(d * std::cos(a), 0.0f, d * std::sin(a));
            const float shade = ao_kernel_test::ao_sample(v, n, radius, 0.0f) * intensity / samples;
            CAPTURE(d);
            CAPTURE(deg);
            CHECK(shade > 0.5f * old_shade(v));
            CHECK(shade < 2.0f * old_shade(v));
        }
}

TEST_CASE("ao close fade: none within reach of a face, full from 12 units, rising in between") {
    CHECK(ao_kernel_test::ao_close_fade(0.5f) == 0.0f);
    CHECK(ao_kernel_test::ao_close_fade(4.0f) == 0.0f);
    CHECK(ao_kernel_test::ao_close_fade(8.0f) == doctest::Approx(0.5f));
    CHECK(ao_kernel_test::ao_close_fade(12.0f) == doctest::Approx(1.0f));
    CHECK(ao_kernel_test::ao_close_fade(500.0f) == doctest::Approx(1.0f));
    float last = 0.0f;
    for (float d = 4.0f; d <= 12.0f; d += 0.5f) {
        const float f = ao_kernel_test::ao_close_fade(d);
        CHECK(f >= last);
        last = f;
    }
}

TEST_CASE("ao visibility: open, shaded and closed stay within [0, 1]") {
    CHECK(ao_kernel_test::ao_visibility(0.0f, 4.0f, 12.0f) == doctest::Approx(1.0f));
    float part = ao_kernel_test::ao_visibility(1.5f, 4.0f, 12.0f);
    CHECK(part == doctest::Approx(0.5f));
    CHECK(ao_kernel_test::ao_visibility(12.0f, 4.0f, 12.0f) == 0.0f);
}
