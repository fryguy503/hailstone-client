#pragma once

// The scene's lighting as the shaders read it (shaders/lighting.glsl): the scene light the original computes
// (game/scene_light.h), the modern mode's daylight, and the point lights near the camera (game/light_list.h).
//
// Classic is the original's look: zone vertices lit by the scene light, characters and objects drawn with their
// fixed light, in display (sRGB) values. Modern (docs/graphics.md) lights everything per pixel in linear HDR: the sun
// or moon and a sky/ground ambient from the day clock, the zone's vertex colours as baked light (their alpha weighs
// the sky's share, as in the original's region shader), and the nearby point lights.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "game/view_distance.h"
#include "render/gpu.h"
#include "render/point_shadows.h"
#include "render/shadows.h"
#include "render/vk_context.h"

namespace hailstone::game { struct SceneLight; struct LightSource; }

namespace hailstone::render {

// The lighting, low to high (the Options window's Lighting choices, in this order). SmoothClassic is the classic
// lighting with its region light per pixel and its highlights rolled off (Hailstone's own; docs/graphics.md "Smooth
// classic lighting"); everything else treats it as Classic, which is the original's look.
enum class LightingMode : std::uint8_t { Classic, SmoothClassic, Modern };

// display.lighting / `/lighting` values: classic, smooth, modern ("enhanced", the old name, is modern); nullopt
// otherwise.
std::optional<LightingMode> parse_lighting_mode(std::string_view name);
// The value display.lighting saves for `mode`.
std::string_view lighting_mode_name(LightingMode mode);
// display.tonemap's "auto": AgX with the modern lighting, Soft with the smooth classic, none (the original's
// clipping) with the classic.
Tonemap automatic_tonemap(LightingMode mode);

// The modern mode's light from the sky, in linear HDR units (1 = the white a surface shows at exposure 0). Not the
// original's: the direction follows its sun path (assets::sky_sweep_heading, overhead at noon, the horizon at 6:00
// and 18:00), the colours come from the sun's elevation through an air-mass model.
struct Daylight {
    glm::vec3 to_light{0.0f, 0.0f, 1.0f}; // towards the sun by day, the moon by night
    glm::vec3 light{0.0f};                // its colour x intensity
    glm::vec3 sky{0.0f};                  // ambient from above
    glm::vec3 ground{0.0f};               // ambient from below
    glm::vec3 to_sun{0.0f, 0.0f, 1.0f};   // towards the sun, above or below the horizon (the modern sky)
    glm::vec3 to_moon{0.0f, 0.0f, -1.0f}; // towards the moon, likewise
    float moon_lit = 1.0f;                // how much of its face is lit, 0 new .. 1 full
    float blood_moon = 0.0f;              // game::SkyClock::blood_moon: the moon and its light red
    // How much of the zone's baked vertex colour the zone geometry keeps (display values, shaders/zone.frag
    // modern_light), in two parts: `baked`, its rest (the ambient and sun its builders put in it), and `baked_lamps`,
    // the part its lamps gave it. 1 by day, as the builders lit it; by night a dim remainder of both, so the moon,
    // the sky and the lamps' pools carry the scene; indoors, where it is the only light, more of both.
    float baked = 1.0f;
    float baked_lamps = 1.0f;
    // How strong the zone's lamps (its lights.wld or EQG lights) shine as real lights, on characters and objects and in
    // their pools on the zone (display.baked_lamp_light, 0..kMaxBakedLamps; Hailstone's own), at any hour, so they
    // match the walls the slider brightens or darkens. The lights the player and spells make are left as they are.
    float lamp_light = 1.0f;
};

// `outdoor`: OP_NewZone time types 1, 2 and 5. Indoors there is no sun, a dim neutral ambient and the baked colour at
// kIndoorBaked (its lamps' part at kIndoorBakedLamps); outdoors the baked colour fades from 1 with the sun 15 degrees
// up to kNightBaked once it is 6 below (kMoonBaked more under the full moon), its lamps' part to kNightBakedLamps.
// `moon_phase` (game::SkyClock::moon_phase, 0 new, 0.5 full) places the moon and sets how much it lights the night:
// steeply more towards full (the lit part squared), the full moon kFullMoon, the new moon an eighth of it (earthshine).
// `blood_moon` (game::SkyClock::blood_moon) turns its light from kFullMoon's cool blue to kBloodMoon's red.
inline constexpr float kNightBaked = 0.3f;
inline constexpr float kMoonBaked = 0.15f;
inline constexpr glm::vec3 kFullMoon{0.20f, 0.24f, 0.36f};
inline constexpr glm::vec3 kBloodMoon{0.36f, 0.08f, 0.05f};
inline constexpr float kNightBakedLamps = 0.35f;
inline constexpr float kIndoorBaked = 0.5f;
inline constexpr float kIndoorBakedLamps = 0.7f;
// Hailstone's own darkness controls (Options > Ambient Light, Baked Lamp Light; 1 as above, 0 none), for a darker
// night and dungeon. `ambient` (display.ambient_light) scales the fill light that keeps the dark readable: indoors the
// ambient and the baked colour's rest; outdoors the starlight floor and the baked colour's rest by night (kNightBaked
// and kMoonBaked). `baked_lamps` (display.baked_lamp_light, 0..kMaxBakedLamps) scales the baked colour's lamps' part
// indoors and by night (kIndoorBakedLamps, kNightBakedLamps), leaving the pools round the lamps; above 1 the lamps
// light their rooms more than the builders baked. The day, the sun, the moon and the sky it lights stay as they are.
inline constexpr float kMaxBakedLamps = 3.0f;
Daylight daylight(float hour, bool outdoor, float moon_phase = 0.5f, float blood_moon = 0.0f, float ambient = 1.0f,
                  float baked_lamps = 1.0f);

// How much of the modern haze's colour stays with `ambient` (display.ambient_light, 0..1): the haze is lit air, and
// with less fill light in the dark it would leave the distance glowing grey. Indoors `ambient`; outdoors from
// `ambient` by night to 1 by day (the sun `sun_z` 6 degrees below the horizon .. 15 above, as the baked colour fades).
float haze_ambient_scale(float sun_z, bool outdoor, float ambient);

// The sky's tilt (this client's own; the original's sun path is not traced): the sun, moon and stars turn about a pole
// standing 90 - kSkyNoonElevation degrees over the northern horizon (+X), so the sun rises due east (-Y) at 6:00,
// stands kSkyNoonElevation over the south (-X) at noon and sets due west (+Y) at 18:00, and noon's shadows fall north.
// shaders/atmosphere.frag's kPole is the same.
inline constexpr float kSkyNoonElevation = 55.0f;
glm::vec3 sky_pole();

// The moon through its phases (0 new, 0.5 full): it rises later each day, standing where the sun stood phase x 24
// hours earlier, so a waxing moon is in the evening sky and the full moon rises at sunset; and the part of its face
// the sun lights.
glm::vec3 moon_direction(float hour, float phase);
float moon_lit(float phase);

// The light on the modern sky's clouds (render::SkyFrame::cloud_light, linear): the sky's and the sun's, so they are
// white by day, warm at sunrise and sunset and dark by night.
glm::vec3 cloud_light(const Daylight& daylight);

// The lights characters, objects and plants take per pixel (the zone takes its own from its light grid, uncapped):
// the dynamic ones and the zone's nearest to the camera. 128, so a city's or dungeon's lamps light the characters in
// all of a room and not only the nearest 32 lamps' (about 130 units round the player in a lit city). shaders/lighting.glsl
// SCENE_LIGHTING_FIELDS's lights[] is the same size.
inline constexpr std::size_t kMaxSceneLights = 128;
// How far from the camera the zone's lamps are taken (game::select_lights' range; display.lamp_light_distance).
inline constexpr float kLampLightDistance = 600.0f;
inline constexpr float kMinLampLightDistance = 100.0f;
inline constexpr float kMaxLampLightDistance = 1500.0f;

struct GpuPointLight {
    glm::vec4 position_radius{0.0f}; // xyz world position, w radius
    glm::vec4 color{0.0f};
};

struct SceneLighting;
// Each of the scene's lights (SceneLighting::lights) with its entry among the shadowed ones (the point shadow fields,
// PointShadows::apply): color.w 1 + the first that is the same light (same_shadow_light), 0 none. The shaders read
// it instead of searching (shaders/lighting.glsl scene_light_shadow).
void tag_shadowed_lights(SceneLighting& lighting);

// std140 and std430 lay this out the same (vec4s and mat4s only); shaders/lighting.glsl SCENE_LIGHTING_FIELDS.
struct SceneLighting {
    glm::vec4 ambient{0.0f}; // w: 1 the smooth classic lighting (ZoneRenderer::set_classic_smooth; the zone only)
    glm::vec4 special_ambient{0.0f};
    glm::vec4 directional{0.0f};
    glm::vec4 direction{0.0f};
    glm::vec4 bounce{0.0f};
    glm::vec4 params{0.0f}; // x: 1 modern, y: light count, z: how many of the first lights are dynamic, w: floor
    glm::vec4 to_light{0.0f, 0.0f, 1.0f, 0.0f}; // Daylight, modern only
    glm::vec4 light{0.0f};
    glm::vec4 sky{0.0f};
    glm::vec4 ground{0.0f};
    // The sun's or moon's shadow cascades (render/shadows.h), modern only.
    std::array<glm::mat4, kShadowCascades> shadow_view_proj{};
    glm::vec4 shadow_splits{0.0f};  // the far view depth of each cascade
    glm::vec4 shadow_texel{0.0f};   // a texel's world size in each cascade
    glm::vec4 shadow_params{0.0f};  // x: 1 shadows drawn, y: 1 / map size
    glm::vec4 shadow_origin{0.0f};  // the camera position
    glm::vec4 shadow_forward{0.0f}; // the camera's forward (view depth for the cascade choice)
    // The modern mode's haze (set_haze; shaders/lighting.glsl apply_haze).
    glm::vec4 haze{0.0f};       // x: 1 / length, y: 1 / height scale, z: 1 on, w: the sun's glow in it
    glm::vec4 haze_color{0.0f}; // linear
    glm::vec4 camera{0.0f};
    glm::vec4 screen{0.0f}; // set_screen: the depth terms and the target's texel size, for the modern water
    glm::vec4 time{0.0f};   // x: seconds, wrapped every 1024 (the water's waves), y: the last frame's (the zone renderer
                            // sets it, for zone.vert's motion), z: the lightning's flash; w: actor material seconds mod 100
    glm::vec4 wind{0.0f};   // xy: the way the wind blows (unit), z: its strength (0..1), w: the gusts' (render::wind_at)
    glm::vec4 cover{0.0f};  // the cover map (render/cover_map.h): its corner, 1 / its size, its top
    glm::vec4 cover_params{0.0f}; // x: 1 / its depth range, y: 1 the cover map drawn, z: 1 the water map drawn
    glm::vec4 weather{0.0f}; // render/weather_renderer.h set_weather: x wet, y snow, z rain falling, w puddles (0..1)
    // The modern mode's infravision / ultravision (game::SceneLight::vision_lift): what the original's vision floor adds
    // to the ambient, which the modern lighting replaces with its sky. Added as linear light (x the vertex alpha on the
    // zone), so it shows against the modern night as the raise shows against the classic one. w: the zone lamps' pools
    // on the zone (Daylight::lamp_light; make_scene_lighting sets it, 1 as before).
    glm::vec4 vision{0.0f};
    // The zone's baked light where the player stands (render/ambient_probe.h ProbeBlend), for the characters and the
    // doors in the modern lighting: rgb the vertex colour (display values), w 1 + the sky's share; w 0 none.
    glm::vec4 probe{0.0f};
    // The isometric camera's cut through what stands in front of the player (game/iso_occlusion.h IsoReveal;
    // shaders/lighting.glsl reveal_discard): origin w 0 is off, as in every other camera.
    glm::vec4 reveal_origin{0.0f};
    glm::vec4 reveal_axis{0.0f};
    glm::vec4 reveal_shape{0.0f};
    glm::vec4 reveal_roof{0.0f};
    // The carried lights (Hailstone's Carried Light Stacking; shaders/lighting.glsl add_scene_light): x how many of the
    // first lights are carried, y how much of the rest each pixel adds to the brightest one (display.
    // carried_light_stacking, 0..1; 1 they add as every light does).
    glm::vec4 carried{0.0f, 1.0f, 0.0f, 0.0f};
    // The zone lights' shadows (render/point_shadows.h PointShadows::apply), modern only.
    std::array<glm::vec4, kMaxPointShadows> point_shadow_light{}; // xyz the light, w its radius
    // x: its cube's first layer, y: its strength, z: a zone light's shadow key (point_shadow_keys), 0 a moving light
    std::array<glm::vec4, kMaxPointShadows> point_shadow_slot{};
    glm::vec4 point_shadow_params{0.0f}; // x: how many, y: 1 / face size, z: kPointShadowFaceScale, w: kPointShadowNear
    std::array<GpuPointLight, kMaxSceneLights> lights{}; // color.w: its shadow entry + 1 (tag_shadowed_lights)
};

// What the modern water needs of the view: view depth = y / (d + x) of the reverse-Z depth d (FlyCamera's near and far
// planes), one pixel of the target in UV, and the time.
void set_screen(SceneLighting& lighting, float near_plane, float far_plane, std::uint32_t width, std::uint32_t height,
                double seconds);

// The modern mode's haze instead of the original's fog: an optical depth of kHazeDepth (d / length)^2 along a level
// line of sight (clear up close, fading over the whole distance), thinning for points above the camera; the colour is
// the zone's fog colour (display values, as SetFog gives it), with a glow towards the sun (haze_glow).
inline constexpr float kHazeDepth = 3.5f;
void set_haze(SceneLighting& lighting, const glm::vec3& camera, float length, const glm::vec3& fog_color);
// The same with the colour already linear (render/atmosphere.h haze_target).
void set_haze_linear(SceneLighting& lighting, const glm::vec3& camera, float length, const glm::vec3& color);

// The sun's glow in the haze (SceneLighting::haze.w; the shaders add the light x glow x 0.25 towards the sun): the
// sunlight the haze scatters, so no more than the haze's own colour says it is lit. kHazeGlow in a haze at least
// kHazeGlowFull bright (linear luminance: a day's fog), less in a darker one, in step with it. The fog colour SetFog
// gives darkens by the original's hour tables, which lag the modern sun: at dawn the sun is up and bright while the
// fog is still nearly black, and an undimmed glow turned everything in the haze towards the sun one flat tan, cut
// out against the dark fog behind (nektulos, 5:50).
inline constexpr float kHazeGlow = 0.35f;
inline constexpr float kHazeGlowFull = 0.05f;
float haze_glow(const glm::vec3& haze_linear);

// Under water (the camera in a water region): the water instead of the haze. Its colour (display values, as set_haze
// and VkContext::begin_scene take a fog colour) is the daylight's sky and sun in a blue-green medium; the haze, as
// thick at every height, reaches kUnderwaterReach; red is absorbed first, then green, then blue (haze.z 2,
// shaders/lighting.glsl apply_haze). Not the original's (its water fog is the zone's).
inline constexpr float kUnderwaterReach = 220.0f;
glm::vec3 underwater_color(const Daylight& daylight);
void set_underwater(SceneLighting& lighting, const glm::vec3& camera, const glm::vec3& color);

// How far the modern mode draws: the original's far plane x `multiplier` (display.view_distance), at most
// kMaxViewDistance, never nearer than the original's.
inline constexpr float kMaxViewDistance = 12000.0f;
float extended_far_plane(float original, float multiplier);

// The haze's length as thick as the zone's fog in `view` (SetFog's start, end and density): the original's fog and
// the haze half clear at the same distance. Density 0.1 (innothule, nektulos version 0, the commons) is a light fog,
// 0.33 (most zones) a thick one. 0 with the fog off.
float zone_haze_length(const game::ViewDistance& view);

// The modern mode's view: the original's `view` drawn out to extended_far_plane when `extend` (not under water, in
// lava or in a thick-fog region), with the fog distances moved out with it (the renderers cull characters at the fog
// end), and the haze's length: the zone's fog's (zone_haze_length), at most the far plane; the far plane, or the
// fog's end when not extended, with the fog off.
struct ModernView {
    game::ViewDistance view;
    float haze_length = 0.0f;
};
ModernView modern_view(const game::ViewDistance& view, float multiplier, bool extend);
// Part of the way (`extend` 0 the original's view .. 1 the extended one), as render::HazeBlend eases between them.
ModernView modern_view(const game::ViewDistance& view, float multiplier, float extend);

// Fills the shadow fields from the cascades (`map_size` texels per side).
void set_shadows(SceneLighting& lighting, const ShadowCascades& cascades, std::uint32_t map_size,
                 const glm::vec3& camera, const glm::vec3& forward);

// `lights`: dynamic lights first (the first `dynamic` of them), then the zone's; more than kMaxSceneLights are cut.
// The zone's are scaled by `daylight.lamp_light`, which also goes to vision.w for the zone shader's pools. The first
// `carried` of the dynamic ones are carried lights, combined with `carried_stacking` of the rest (SceneLighting::carried).
SceneLighting make_scene_lighting(const game::SceneLight& scene, std::span<const game::LightSource> lights,
                                  std::size_t dynamic, LightingMode mode, const Daylight& daylight = {},
                                  std::size_t carried = 0, float carried_stacking = 1.0f);

// The classic lighting of a character (shaders/actor.frag), this client's own: how the original lights its skinned
// models (SkinMeshOld's `lit`) is not traced (docs/unknowns.md). It is the region shaders' vertex light
// (shaders/zone.vert) for a vertex whose colour is the zone's baked light where the player stands (the probe; none:
// black with the sky's full share), so a character takes the light of the walls round it, by day and by night:
//   rgb = C.rgb + C.a (Ambient + Bounce max(N.Dn, 0) + Directional max(N.-Dn, 0)) + SpecialAmbient,
// kept between kClassicActorFloor and 1, so nobody turns into a silhouette.
inline constexpr float kClassicActorFloor = 0.25f;
glm::vec3 classic_actor_light(const SceneLighting& lighting, const glm::vec3& normal);

// A classic scene light that leaves the characters' textures as they are (ambient 1): the item preview's, and the
// actor renderer's until it is given one.
SceneLighting classic_full_bright();

// One host-visible, mapped buffer per frame in flight.
class FrameBuffers {
public:
    FrameBuffers(VkContext& vk, VkDeviceSize size, VkBufferUsageFlags usage);
    ~FrameBuffers();
    FrameBuffers(const FrameBuffers&) = delete;
    FrameBuffers& operator=(const FrameBuffers&) = delete;

    void write(std::uint32_t frame_slot, const void* data, VkDeviceSize size);
    VkBuffer buffer(std::size_t frame_slot) const { return buffers_[frame_slot].buffer; }
    VkDeviceSize size() const { return size_; }

private:
    VkContext& vk_;
    VkDeviceSize size_;
    std::array<Buffer, VkContext::kFramesInFlight> buffers_{};
    std::array<void*, VkContext::kFramesInFlight> mapped_{};
};

} // namespace hailstone::render
