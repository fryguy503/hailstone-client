#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"
#include "coverage.glsl"
#include "weather.glsl"

// Zone geometry pixels by the original's effect:
//   0 classic region effects (RegionC1 / RegionOld*): texture x lit colour
//   1 heightmap terrain tiles (Terrain_NoDetail, _1Detail .. _3Detail)
//   2 RegionWaterFall   3 RegionLava (and Lava2)   4 RegionTerrain (EQG .ter)
//   5 RegionWater (and LavaH), without the environment reflection
//   6 a classic region's detail pass (RegionOldDetailSingle / RegionOldDetailPalette): texture x lit colour,
//     alpha the pass's vertex alpha (v_color2.a), the texture's alpha not read

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 2, binding = 0) uniform sampler2DArrayShadow shadow_map; // render/vk_context.h shadow_set
layout(set = 2, binding = 1) uniform sampler2D scene_color; // VkContext::begin_translucent's copies (kWaterCopies)
layout(set = 2, binding = 2) uniform sampler2D scene_depth;
layout(set = 2, binding = 3) uniform sampler2DArray water_map; // the shadow map's depths: the water map's
layout(set = 2, binding = 4) uniform sampler2DArrayShadow point_map; // the zone lights' cubes (render/point_shadows.h)

layout(set = 1, binding = 0) uniform Scene {
    mat4 view_proj;
    vec4 fog_color;
    vec4 fog;        // start, end, k = fmod(a_fTime, 100) in seconds, density
    vec4 camera_pos;
    SCENE_LIGHTING_FIELDS
} scene;

// The zone's lights for the modern lighting, per pixel (render/light_grid.h, ZoneRenderer::build).
layout(std430, set = 1, binding = 1) readonly buffer ZoneLights {
    vec4 grid;  // origin x, y, 1 / cell size
    uvec4 dims; // cells across, cells down, light count
    PointLightGpu lights[];
} zone_lights;
layout(std430, set = 1, binding = 2) readonly buffer LightCells { uvec2 cells[]; } light_cells; // first, count
layout(std430, set = 1, binding = 3) readonly buffer LightIndices { uint indices[]; } light_indices;

layout(push_constant) uniform Push {
    uvec4 tex;
    uvec4 tex2;   // slot 4, mode, flags, a texture pack's maps (normal slot | roughness slot << 16; 0 = none)
    vec4 params;  // alpha_ref, opacity, coverage scale
    vec4 slide;
    vec4 extra;
    vec4 extra2;
    vec4 extra3;
    vec4 extra4; // water: the reflection colour (rgb) and amount (a)
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_light;
layout(location = 2) in vec3 v_world;
layout(location = 3) in vec2 v_uv2;
layout(location = 4) in vec4 v_color;
layout(location = 5) in vec4 v_color2;
layout(location = 6) in vec3 v_normal;
layout(location = 7) in float v_fog; // oFog (zone.vert)
layout(location = 8) in vec3 v_point;
layout(location = 9) in vec3 v_rest; // the plants at rest, as the lights' shadow cubes hold them (point_shadow)
layout(location = 10) flat in float v_lod_distance; // zone.vert: a level of detail's fades

layout(location = 0) out vec4 o_color;

const uint kUnlit = 1u;
const uint kNoFog = 2u;
const uint kWaterSurface = 8u; // render/zone_renderer.h Liquid
const uint kLavaSurface = 16u;
const uint kWaterCopies = 32u;
const uint kNormalMap = 64u;   // render/zone_renderer.cpp: slot 1 is a tangent-space normal map (modern)
const uint kSpecular = 128u;   // a sun highlight: extra.x shininess, extra.y gloss
const uint kReflective = 256u; // the sky reflected: extra.z strength
const uint kMolten = 512u;     // RegionLava whose texture's name says lava or magma: drawn as lava (modern)
const uint kEnergy = 1024u;    // RegionLavaH (harbingers' towers): drawn as lava (modern)
const uint kEnvMap = 2048u;    // water: slot 2 is its own environment, an octahedral map (a "noswap" cube)
const uint kFlagLodFade = 4096u; // a level of detail: extra3 its band (coverage.glsl lod_fade_samples)
const uint kTintNeighbour = 32768u; // render/zone_renderer.h set_debug_tint: a neighbour's own geometry
const uint kTintLand = 65536u;      // its own ground in its part of the land

vec4 tex(uint i, vec2 uv) { return texture(textures[nonuniformEXT(i)], uv); }

// The light of the pixel: classic, the lit vertex colour v0 (display values); modern, the linear HDR light.
bool g_modern;
vec3 g_light;
float g_shadow = 1.0; // the sun's or moon's shadow (modern)

// A surface colour (display values) under the pixel's light.
vec3 lit(vec3 albedo) { return g_modern ? to_linear(albedo) * g_light : albedo * g_light; }
// A colour the surface gives off itself (lava): in the modern mode brighter than lit white, so it blooms.
vec3 emit(vec3 c) { return g_modern ? to_linear(c) * 2.5 : c; }

// A torch's flicker: warm lights waver by up to 12 %, each on its own (by its number); others hold steady.
float flicker(uint index, vec3 color) {
    if (color.r < color.b * 1.3 || color.r < 0.2) return 1.0;
    float t = scene.time.x * 7.0 + float(index) * 17.31;
    float f = sin(t) * 0.5 + sin(t * 2.37 + 1.3) * 0.3 + sin(t * 5.11 + 2.1) * 0.2;
    return 1.0 - 0.06 * (f + 1.0);
}

// The zone's lights at a pixel, in two parts: `display`, half the MPL passes' sum (game::point_light_term, colour x
// 0.5, each saturating on its own) as the base the zone's builders balanced their interiors with, and `pools`, the
// modern lights' bright centres (light_pool, linear), so a torch lights a round pool around it. Both stop
// where something stands between the light and the pixel (the lights with a shadow cube, point_shadow). `counted` is
// what all the lights give the pixel as game::point_light_term weighs them (display values), the share of the baked
// colour that is theirs; `blocked` is the part of it that stops where something stands in the way, for the baked
// colour, which counts the light without what stands in its way, to give back.
// `wrap` softens the facing term (foliage).
void zone_point_lights(vec3 p, vec3 n, float wrap, out vec3 display, out vec3 pools, out vec3 counted,
                       out vec3 blocked) {
    display = vec3(0.0);
    pools = vec3(0.0);
    counted = vec3(0.0);
    blocked = vec3(0.0);
    uvec4 dims = zone_lights.dims;
    if (dims.x == 0u) return;
    ivec2 c = ivec2(floor((p.xy - zone_lights.grid.xy) * zone_lights.grid.z));
    if (c.x < 0 || c.y < 0 || c.x >= int(dims.x) || c.y >= int(dims.y)) return;
    uvec2 cell = light_cells.cells[uint(c.y) * dims.x + uint(c.x)];
    for (uint i = 0u; i < cell.y; ++i) {
        uint index = light_indices.indices[cell.x + i];
        PointLightGpu l = zone_lights.lights[index];
        float r = l.position_radius.w;
        vec3 d = l.position_radius.xyz - p;
        float d2 = dot(d, d);
        float falloff = 1.0 - min(d2 / (r * r), 1.0);
        if (falloff <= 0.0) continue;
        float facing = clamp((dot(n, d * inversesqrt(max(d2, 1e-6))) + wrap) / (1.0 + wrap), 0.0, 1.0);
        float shadow = point_shadow(point_map, scene.point_shadow_light, scene.point_shadow_slot,
                                    scene.point_shadow_params, l.position_radius, v_rest, n);
        float f = flicker(index, l.color.rgb) * shadow;
        counted += l.color.rgb * facing * falloff;
        blocked += l.color.rgb * facing * falloff * (1.0 - shadow);
        display += min(l.color.rgb * 0.25 * facing * falloff * f, vec3(0.5));
        pools += baked_to_linear(l.color.rgb) * 3.0 * facing * light_pool(d2, r) * f;
    }
}

// The modern light (render/scene_lighting.h): the vertex colour as baked light, its alpha weighing the sky's share
// (as the classic region shader weighs the ambient and the sun), the zone's lights.wld lights as the classic MPL term
// sums them (per pixel from the light grid; per vertex, v_point, without one), the sun or moon, and the dynamic
// point lights (the light the player carries, the spells') per pixel, shadowed. Characters and objects take the zone's lights per pixel.
// `n` is the surface's, for the shadows; `sn` the one it is shaded with (a texture pack's normal map, else n).
vec3 modern_light(vec3 n, vec3 sn, bool foliage) {
    float a = v_color.a;
    float ndl = dot(sn, scene.to_light.xyz);
    // Alpha-tested leaves and grass let light through: wrapped, never fully dark on the far side.
    ndl = foliage ? clamp(ndl * 0.6 + 0.4, 0.0, 1.0) : max(ndl, 0.0);
    g_shadow = shadow_visibility(shadow_map, scene.shadow_view_proj, scene.shadow_splits, scene.shadow_texel,
                                 scene.shadow_params, scene.shadow_origin, scene.shadow_forward, v_world, n);
    ndl *= g_shadow;
    // The baked colour, the special ambient and the zone's lights add as the classic shader adds them, in display
    // values: converted after the sum, dim interiors keep the brightness their builders gave them. The zone's lights
    // are taken per pixel when the grid has them (else the MPL vertex term).
    vec3 points = v_point, pools = vec3(0.0), counted = vec3(0.0), blocked = vec3(0.0);
    if (zone_lights.dims.x > 0u) zone_point_lights(v_world, sn, foliage ? 0.5 : 0.0, points, pools, counted, blocked);
    // The baked colour in two parts. Its rest, what is left once the lights it counts are taken out (never below a
    // third of it): the ambient and, outdoors, the sun its builders put in it. Its lamps' part, less what the shadows
    // take back. By day both stay as they were baked; by night (and indoors, less so) they fade to scene.sky.w and
    // scene.ground.w of themselves (render::Daylight::baked, baked_lamps), and the half MPL term with the lamps', so
    // the lamps are not counted three times over and the moon, the sky and the pools round the lamps light the zone:
    // dark between the lamps, never black (the rest's share and the sky).
    vec3 rest = max(v_color.rgb - counted, v_color.rgb * 0.33);
    vec3 lamps = max(v_color.rgb - blocked, v_color.rgb * 0.33) - rest;
    vec3 baked = rest * scene.sky.w + max(lamps, vec3(0.0)) * scene.ground.w;
    // An uncoloured mesh (no baked light: every vertex the loader's opaque white, as an EQG placement without lighting
    // data) is lit as the doors and characters are, by the sky, the sun and the lights: its white would add a full
    // display white of baked light, glaring beside the ground.
    if (all(greaterThan(v_color, vec4(0.998)))) baked = vec3(0.0);
    // Under water, the light through the waves.
    float water_sun, water_sky;
    underwater_light(water_map, scene.cover, scene.cover_params, v_world, scene.to_light, scene.time.x, water_sun,
                     water_sky);
    vec3 light = pools + baked_to_linear(baked + scene.special_ambient.rgb + points * scene.ground.w) +
                 a * (hemisphere(sn, scene.sky.rgb, scene.ground.rgb) * water_sky + scene.light.rgb * ndl * water_sun +
                      scene.vision.rgb);
    // The dynamic lights, stopped where something stands in their way (the carried light and the brightest spells
    // have shadow cubes of their own; render/point_shadows.h set_dynamic).
    int count = int(scene.params.z + 0.5);
    for (int i = 0; i < count; ++i)
        light += point_light_modern(scene.lights[i], v_world, sn, foliage ? 0.5 : 0.0) *
                 point_shadow(point_map, scene.point_shadow_light, scene.point_shadow_slot, scene.point_shadow_params,
                              scene.lights[i].position_radius, v_rest, n);
    return light;
}

// ---- The modern lighting's water and lava ----

float view_depth(float d) { return scene.screen.y / (d + scene.screen.x); }

// A world point on the screen (UV, the viewport is flipped) and its view depth.
vec2 screen_uv(vec3 p, out float depth) {
    vec4 c = scene.view_proj * vec4(p, 1.0);
    depth = c.w;
    vec2 ndc = c.xy / max(c.w, 1e-4);
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float value_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}

// Value noise (0..1) with its gradient: x the value, yz d/dp. Quintic, so the gradient is smooth too.
vec3 noise_grad(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    float a = hash12(i), b = hash12(i + vec2(1, 0)), c = hash12(i + vec2(0, 1)), d = hash12(i + vec2(1, 1));
    float k = a - b - c + d;
    return vec3(a + (b - a) * u.x + (c - a) * u.y + k * u.x * u.y, du * (vec2(b - a, c - a) + k * u.yx));
}

// The waves' slope (dh/dx, dh/dy) at a point. Twelve waves from 18 units long down to half a unit, their directions
// spread up to about 70 degrees either side of the wind (SceneLighting::wind; a light breeze when there is none), a
// few running across it, each with its own phase, all about as steep (long ones in step would band). So the crests are not ruled lines,
// the plane they run over is warped up to ten units by slow noise, and the short ripples gather in drifting patches
// (gusts on the surface) and calm between them. Two layers of drifting noise add the fine chop. The wind's strength
// raises all of it. Each wave fades out once it is under four pixels long (no shimmer in the distance).
vec2 wave_slope(vec2 p, float t, float footprint) {
    vec2 wind = dot(scene.wind.xy, scene.wind.xy) > 1e-4 ? normalize(scene.wind.xy) : vec2(0.8, 0.6);
    vec2 across = vec2(-wind.y, wind.x);
    float strength = 0.55 + 0.7 * scene.wind.z;
    // The warp: up to ten units, changing over some thirty, so even the long crests bend and break up.
    vec2 q = p + 10.0 * vec2(value_noise(p * 0.033 + vec2(t * 0.01, 3.1)), value_noise(p * 0.031 + vec2(7.7, -t * 0.012)));
    float gust = smoothstep(0.25, 0.8, value_noise(p * 0.035 - wind * t * 0.06));
    vec2 slope = vec2(0.0);
    for (int i = 0; i < 12; ++i) {
        float fi = float(i);
        float len = 18.0 * pow(0.73, fi);
        // Spread about the wind by the golden angle's fractions; every fourth runs across it.
        float angle = (fract(fi * 0.618034 + 0.31) - 0.5) * 2.4 + (i % 4 == 3 ? 1.4 : 0.0);
        vec2 d = wind * cos(angle) + across * sin(angle);
        float k = 6.2831853 / len;
        float fade = 1.0 - smoothstep(0.25, 1.0, footprint * 4.0 / len);
        if (fade <= 0.0) continue;
        float steep = 0.05 * pow(0.95, fi) * mix(1.0, mix(0.35, 1.6, gust), smoothstep(8.0, 2.0, len));
        float phase = k * dot(d, q) - sqrt(16.0 * k) * t + hash12(vec2(fi, 17.0)) * 6.2831853;
        slope += d * (steep * cos(phase) * fade);
    }
    // The fine chop: noise drifting with the wind and across it, under a unit and a half across.
    float chop = (1.0 - smoothstep(0.1, 0.5, footprint)) * mix(0.3, 1.0, gust);
    if (chop > 0.0) {
        vec3 a = noise_grad(p * 0.9 + wind * t * 0.8);
        vec3 b = noise_grad(p * 1.7 - across * t * 0.6 + 13.0);
        slope += (a.yz * 0.9 + b.yz * 1.7) * 0.012 * chop;
    }
    return slope * strength;
}

// The sky seen in direction r, where the screen does not show it: the haze's colour at the horizon, deepening
// overhead towards the sky light's hue at about the horizon's brightness, with the sun's glow in the haze round it.
vec3 sky_reflection(vec3 r) {
    vec3 horizon = scene.haze.z > 0.5 ? scene.haze_color.rgb : to_linear(scene.fog_color.rgb);
    float h = dot(horizon, vec3(0.2126, 0.7152, 0.0722)), s = dot(scene.sky.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 zenith = s > 1e-5 ? scene.sky.rgb * (h * 0.8 / s) : horizon;
    vec3 c = mix(horizon, mix(horizon, zenith, 0.75), pow(smoothstep(0.0, 0.7, r.z), 0.6));
    float sun = max(dot(r, scene.to_light.xyz), 0.0);
    return c + scene.light.rgb * (pow(sun, 8.0) * 0.06 + pow(sun, 64.0) * 0.12) * min(h * 8.0, 1.0);
}

// Where a direction lies in an octahedral map (assets::octahedral_uv).
vec2 octahedral_uv(vec3 d) {
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    vec2 p = d.z >= 0.0 ? d.xy : (1.0 - abs(d.yx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}

// What a water reflects where the screen does not show it: its material's own environment (kEnvMap: a cave's, a
// volcano's), else the sky.
vec3 water_environment(vec3 r, uint flags) {
    if ((flags & kEnvMap) != 0u) return to_linear(textureLod(textures[nonuniformEXT(pc.tex.z)], octahedral_uv(r), 0.5).rgb);
    return sky_reflection(normalize(vec3(r.xy, max(r.z, 0.02))));
}

// The sky as the frame drew it in direction r (a direction, at infinity), when the screen holds it there unhidden:
// the clouds, the sun and the stars the water should mirror. The weight is 0 off the screen or behind the world, and
// fades towards the screen's edges.
float screen_sky(vec3 r, out vec3 color) {
    color = vec3(0.0);
    vec4 c = scene.view_proj * vec4(r, 0.0);
    if (c.w <= 1e-4) return 0.0;
    vec2 ndc = c.xy / c.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    vec2 edge = min(uv, 1.0 - uv);
    if (min(edge.x, edge.y) <= 0.0) return 0.0;
    if (textureLod(scene_depth, uv, 0.0).r > 0.0) return 0.0;
    color = textureLod(scene_color, uv, 0.0).rgb;
    return smoothstep(0.0, 0.1, min(edge.x, edge.y));
}

// Screen-space reflection: marches r from p over the opaque depth until it passes just behind a surface. The weight
// fades towards the screen's edges, and is 0 when nothing was hit.
float trace_reflection(vec3 p, vec3 r, out vec3 color) {
    color = vec3(0.0);
    float t = 1.0, prev = 0.0;
    for (int i = 0; i < 40; ++i) {
        float w;
        vec2 uv = screen_uv(p + r * t, w);
        if (w <= 0.5 || uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
        float d = textureLod(scene_depth, uv, 0.0).r;
        if (d > 0.0 && w > view_depth(d)) {
            if (w - view_depth(d) < max(t - prev, 2.0) * 2.0) {
                // Close the gap between the last step in front and this one behind.
                float lo = prev, hi = t;
                for (int j = 0; j < 5; ++j) {
                    float mid = 0.5 * (lo + hi);
                    vec2 m = screen_uv(p + r * mid, w);
                    float md = textureLod(scene_depth, m, 0.0).r;
                    if (md > 0.0 && w > view_depth(md)) hi = mid; else lo = mid;
                }
                uv = screen_uv(p + r * hi, w);
                color = textureLod(scene_color, uv, 0.0).rgb;
                vec2 edge = min(uv, 1.0 - uv);
                return smoothstep(0.0, 0.08, min(edge.x, edge.y));
            }
        }
        prev = t;
        t = t * 1.15 + 1.0;
    }
    return 0.0;
}

// The water: the floor seen through it, coloured and dimmed by the depth of water in between, the sky and the scene
// reflected by Fresnel, the sun's glint on moving waves, and foam where it is shallow. `albedo` is the water's own
// colour (linear); alpha matters only without the copies (ZonePhase::All), where the original's blend stays.
vec3 modern_water(vec3 albedo, inout float alpha, uint flags, vec3 tint) {
    float t = scene.time.x;
    vec3 to_eye = scene.camera_pos.xyz - v_world;
    float dist = length(to_eye);
    vec3 view = to_eye / max(dist, 1e-4);
    bool below = view.z < 0.0;
    float footprint = length(fwidth(v_world.xy));
    // From below, the waves are flattened: at the grazing angles there they band.
    vec3 n = normalize(vec3(-wave_slope(v_world.xy, t, footprint) * (below ? 0.25 : 1.0), 1.0));
    vec3 nv_n = below ? -n : n;
    float nv = clamp(dot(nv_n, view), 0.0, 1.0);
    float fresnel = below ? 0.0 : 0.02 + 0.98 * pow(1.0 - nv, 5.0);

    // The water's colour where it is deep: its albedo under the light of the surface.
    vec3 deep = albedo * g_light * 0.5;
    vec3 color = deep;
    vec3 seen = deep;     // what lies behind the surface
    float vertical = 1e4; // the depth of water under the pixel
    float rise = 1.0;     // how steeply the floor rises under the surface (depth per unit across)
    bool copies = (flags & kWaterCopies) != 0u;
    if (copies) {
        vec2 uv = gl_FragCoord.xy * scene.screen.zw;
        float surface = 1.0 / gl_FragCoord.w;
        float d0 = texture(scene_depth, uv).r;
        float floor0 = d0 > 0.0 ? view_depth(d0) : 1e5;
        float thick = max(floor0 - surface, 0.0);
        float vertical0 = thick * abs(view.z);
        rise = length(vec2(dFdx(vertical0), dFdy(vertical0))) / max(footprint, 1e-3);
        vec2 ruv = uv + n.xy * 0.05 * clamp(thick / 12.0, 0.0, 1.0) * clamp(30.0 / dist, 0.2, 1.0);
        float d1 = texture(scene_depth, ruv).r;
        float floor1 = d1 > 0.0 ? view_depth(d1) : 1e5;
        if (floor1 < surface) { ruv = uv; floor1 = floor0; } // the bent ray met something in front of the water
        thick = max(floor1 - surface, 0.0);
        vertical = thick * abs(view.z);
        seen = texture(scene_color, ruv).rgb;
        if (below) {
            // From under the water: the sky through Snell's window overhead (the light from above bent into a cone
            // about 97 degrees across), the sun bright in it; outside the window the water mirrors itself. The
            // absorption on the way to the eye is the underwater haze's (render::set_underwater).
            vec3 r = refract(-view, nv_n, 1.33);
            float window = dot(r, r) > 0.0 ? smoothstep(0.0, 0.3, r.z) : 0.0;
            vec3 above = scene.sky.rgb * 1.6 + scene.light.rgb * pow(max(dot(r, scene.to_light.xyz), 0.0), 48.0) * 4.0;
            // The mirror shows the water itself: its colour, darker (as shaders/underwater.frag carries it on).
            vec3 mirror = scene.haze.z > 1.5 ? scene.haze_color.rgb * 0.55 : deep;
            color = mix(mirror, above * mix(vec3(1.0), albedo * 2.0, 0.25), window);
        } else {
            // Absorption along the path, stronger for the colours the water is not.
            vec3 absorb = (1.0 - clamp(albedo * 2.0, 0.0, 1.0)) * 0.09 + 0.012;
            vec3 transmit = exp(-absorb * min(thick, 400.0));
            color = mix(deep, seen, transmit);
        }
    }
    if (!below) {
        vec3 r = reflect(-view, n);
        r.z = max(r.z, 0.02);
        r = normalize(r);
        vec3 reflected = water_environment(r, flags);
        if (copies) {
            vec3 sky;
            float ws = (flags & kEnvMap) != 0u ? 0.0 : screen_sky(r, sky);
            reflected = mix(reflected, sky, ws);
            vec3 hit;
            float w = trace_reflection(v_world + vec3(0.0, 0.0, 0.05), r, hit);
            reflected = mix(reflected, hit, w);
        }
        // An EQG material's reflection colour (bloodfalls' falls reflect blood red).
        color = mix(color, reflected * tint, fresnel);
        // The sun's glint: sharp, bright enough to bloom.
        float spec = pow(max(dot(r, scene.to_light.xyz), 0.0), 400.0) * 40.0 +
                     pow(max(dot(r, scene.to_light.xyz), 0.0), 60.0) * 1.0;
        color += scene.light.rgb * spec * g_shadow;
    }
    // Foam where the water is shallow, broken up by two drifting layers of noise, out as far as the shore is seen (it
    // must not draw itself in ahead of the player). The band widens with the pixel's footprint so a far shore keeps a
    // line a few pixels thick, and where the noise would shimmer the foam settles to its average.
    float band = clamp(footprint * 2.0, 4.0, 10.0);
    // Only where the floor rises to the surface (a beach, a bank, the foot of a wall): a shallow pool with a level
    // floor stays clear.
    float shore = (1.0 - smoothstep(0.0, band, vertical)) * (1.0 - smoothstep(1500.0, 3000.0, dist)) *
                  smoothstep(0.015, 0.1, rise);
    if (shore > 0.0 && !below) {
        float f = value_noise(v_world.xy * 0.9 + vec2(t * 0.15, t * 0.05)) * 0.6 +
                  value_noise(v_world.xy * 2.3 - vec2(t * 0.1, -t * 0.12)) * 0.4;
        float near = smoothstep(0.8 - shore * 0.45, 0.95 - shore * 0.4, f);
        float foam = mix(0.25, near, 1.0 - smoothstep(0.4, 2.0, footprint)) * shore;
        color = mix(color, g_light * 0.75, foam * 0.6);
    }
    // A soft edge: the last half unit of depth fades into what is under it.
    if (copies && !below) color = mix(seen, color, smoothstep(0.0, 0.5, vertical));
    if (!copies) alpha = max(alpha, fresnel);
    return color;
}

// Water on a steep surface (an EQG RegionWater mesh draped over a fall: bloodfalls): a thin sheet running down, not
// a level body. `wn` is the class's two scrolling normal samples (tangent space), which ripple it; it shows its own
// colour lit as a surface, the sky and the scene by Fresnel (in the material's tint), the sun's glint, and lets a
// little of what is behind it through.
vec3 falling_water(vec3 albedo, vec3 surface, vec3 wn, vec3 tint, uint flags) {
    vec3 view = normalize(scene.camera_pos.xyz - v_world);
    vec3 g = normalize(v_normal);
    if (dot(g, view) < 0.0) g = -g; // two-sided
    // The ripples on a frame from the screen derivatives (the meshes carry no tangents).
    vec3 dp1 = dFdx(v_world), dp2 = dFdy(v_world);
    vec2 duv1 = dFdx(v_uv), duv2 = dFdy(v_uv);
    vec3 t = cross(dp2, g) * duv1.x + cross(g, dp1) * duv2.x, b = cross(dp2, g) * duv1.y + cross(g, dp1) * duv2.y;
    float s = max(dot(t, t), dot(b, b));
    vec3 n = s > 1e-20 ? normalize((t * wn.x + b * wn.y) * inversesqrt(s) * 0.6 + g * max(wn.z, 0.3)) : g;
    float nv = clamp(dot(n, view), 0.0, 1.0);
    float fresnel = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    vec3 r = reflect(-view, n);
    vec3 reflected = water_environment(r, flags);
    // The diffuse map running down it streaks the sheet.
    float streak = dot(surface, vec3(0.299, 0.587, 0.114)) / max(dot(albedo, vec3(0.299, 0.587, 0.114)), 1e-3);
    vec3 color = albedo * g_light * 0.6 * clamp(streak, 0.4, 1.8);
    if ((flags & kWaterCopies) != 0u) {
        vec3 sky;
        if ((flags & kEnvMap) == 0u) reflected = mix(reflected, sky, screen_sky(r, sky));
        vec2 uv = gl_FragCoord.xy * scene.screen.zw + n.xy * 0.01;
        color = mix(texture(scene_color, uv).rgb * albedo * 2.0, color, 0.75);
    }
    color = mix(color, reflected * tint, max(fresnel, 0.15));
    float spec = pow(max(dot(r, scene.to_light.xyz), 0.0), 120.0) * 6.0;
    return color + scene.light.rgb * spec * g_shadow * tint;
}

// Two octaves of value noise, 0..1.
float fbm2(vec2 p) { return value_noise(p) * 0.65 + value_noise(p * 2.13 + 7.1) * 0.35; }

// Whether a texture's colour is a fire's (red over green over blue): lava, magma, embers.
bool warm(vec3 c) { return c.r > c.g * 1.15 && c.g >= c.b * 0.9 && c.r > 0.25; }

// The light molten rock gives off at a heat h (0 cooling .. 1 at its hottest): deep red through orange to yellow, as
// a body's glow runs, brighter than lit white so it blooms (linear).
vec3 molten_glow(float h) {
    vec3 c = mix(vec3(0.3, 0.015, 0.0), vec3(1.0, 0.22, 0.015), smoothstep(0.0, 0.55, h));
    c = mix(c, vec3(1.0, 0.6, 0.18), smoothstep(0.55, 1.0, h));
    return c * (0.3 + 3.7 * h * h);
}

// Lava (modern): its heat from how bright the texture is, churned by slow noise drifting with the flow at `p` (world
// units / 22, or the texture's for a wall), so plates of dark crust break and close over the molten rock and hot
// seams run between them; the hot parts glow (bloom), the crust is dark rock lit by the scene, warmed near the heat.
// A fire-coloured texture glows in the colours of heat; another colour (green or violet "lava") glows in its own hue.
// `molten` is how much of the surface is the class's molten layer (1 - the crust mask).
vec3 modern_lava(vec3 texel, float molten, vec2 p) {
    float t = scene.time.x;
    float luma = dot(texel, vec3(0.299, 0.587, 0.114));
    float churn = fbm2(p + vec2(t * 0.02, -t * 0.013));
    float seams = 1.0 - abs(fbm2(p * 2.7 - vec2(t * 0.035, t * 0.021)) * 2.0 - 1.0);
    float open = smoothstep(0.3, 0.72, churn); // 0 under a crust plate, 1 where it has broken open
    float heat = smoothstep(0.25, 0.85, luma) * mix(0.4, 1.15, open) + pow(seams, 6.0) * 0.5 * (1.0 - 0.5 * open);
    heat = clamp(max(heat, molten * smoothstep(0.1, 0.5, luma)), 0.0, 1.0);
    // A slow throb, out of step across the surface.
    heat *= 0.92 + 0.08 * sin(t * 1.1 + value_noise(p * 0.5) * 6.2831853);
    bool fire = warm(texel);
    vec3 glow = fire ? molten_glow(heat) : to_linear(texel) * (0.4 + 3.6 * heat * heat);
    // The crust: dark basalt (a fire-coloured texture's own colour is the heat's, not the rock's) lit by the scene,
    // warmed from below as it gets hotter.
    vec3 rock = fire ? mix(vec3(0.07, 0.06, 0.06), texel * 0.45, heat) : texel * mix(0.5, 1.0, heat);
    vec3 crust = lit(rock) + glow * 0.25;
    return mix(crust, glow, smoothstep(0.2, 0.65, heat));
}

// The weather on the ground (shaders/weather.glsl): snow on the level surfaces, the rain's soak, and water standing
// in the low spots. A cut-out surface (`cutout`: leaves, fronds, grates) has no surface for water to stand on, and its
// normals are a crown's or a card's, mostly facing up: no puddles or snow and a lighter soak, or a generated tree's
// crown takes the sky's colour or turns white.
vec3 weathered(vec3 color, vec3 n, bool cutout) {
    float footprint = length(fwidth(v_world));
    float open = sky_open(shadow_map, scene.cover, scene.cover_params, scene.shadow_params.y, v_world + n * 0.5);
    if (open <= 0.0) return color;
    vec3 horizon = scene.haze.z > 0.5 ? scene.haze_color.rgb : to_linear(scene.fog_color.rgb);
    vec4 weather = scene.weather;
    if (cutout) weather.y = 0.0;
    return weather_surface(color, lit(vec3(0.88, 0.9, 0.94)), v_world, n, normalize(scene.camera_pos.xyz - v_world),
                           weather, open, horizon, scene.sky.rgb, scene.time.x, footprint, !cutout, cutout ? 0.3 : 1.0);
}

vec3 mapped_normal(vec3 n, uint slot) {
    vec3 dp1 = dFdx(v_world), dp2 = dFdy(v_world);
    vec2 duv1 = dFdx(v_uv), duv2 = dFdy(v_uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = max(dot(t, t), dot(b, b));
    if (!(scale > 1e-20)) return n;
    float inv = inversesqrt(scale);
    vec3 m = tex(slot, v_uv).rgb * 2.0 - 1.0;
    vec3 mapped = t * inv * m.x + b * inv * m.y + n * m.z;
    float l = length(mapped);
    return l > 1e-6 ? mapped / l : n;
}

// The specular classes' highlight of the sun (Blinn-Phong at the material's shininess, its gloss as the strength) and
// the reflective classes' sky (in place of their environment map), both in the open sky's share of the vertex.
vec3 surface_shine(vec3 n, uint flags) {
    vec3 view = normalize(scene.camera_pos.xyz - v_world);
    vec3 shine = vec3(0.0);
    if ((flags & kSpecular) != 0u) {
        vec3 h = normalize(view + scene.to_light.xyz);
        float ndl = max(dot(n, scene.to_light.xyz), 0.0);
        float power = max(pc.extra.x, 1.0);
        shine += scene.light.rgb * g_shadow * ndl * pow(max(dot(n, h), 0.0), power) * pc.extra.y * (power + 8.0) / 25.0;
    }
    if ((flags & kReflective) != 0u) {
        vec3 r = reflect(-view, n);
        float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 5.0);
        shine += sky_reflection(normalize(vec3(r.xy, max(r.z, 0.02)))) * min(pc.extra.z, 1.0) * mix(0.25, 1.0, fresnel);
    }
    return shine * v_color.a;
}

void main() {
    // The isometric camera's cut (game/iso_occlusion.h). The depth pre-pass (zone_depth.frag) must discard every pixel
    // this shader discards, here and at the alpha test below: the colour pass over it tests depth equal and writes
    // none (tests/depth_prepass_test.cpp checks the two call the same discard helpers).
    if (reveal_discard(v_world, gl_FragCoord.xy, scene.reveal_origin, scene.reveal_axis, scene.reveal_shape,
                       scene.reveal_roof))
        discard;
    // A level of detail in its fade into the next (render/lod_bands.h): each sample shows one of the two. The mask is
    // written on every path (an unwritten one is undefined), and zone_depth.frag writes the same.
    int lod_samples = lod_fade_samples((pc.tex2.z & kFlagLodFade) != 0u, v_lod_distance, pc.extra3, gl_FragCoord.xy);
    if (lod_samples == 0) discard;
    gl_SampleMask[0] = lod_samples;
    // A neighbour's part of the land: this draw's ground or the other's (coverage.glsl land_blend_discard).
    if (land_blend_discard(pc.tex2.z, v_color2.a, v_world)) discard;
    uint mode = pc.tex2.y;
    uint flags = pc.tex2.z;
    bool unlit = (flags & kUnlit) != 0u;
    g_modern = scene.params.x > 0.5;
    float len = length(v_normal);
    vec3 n = len > 1e-6 ? v_normal / len : vec3(0.0, 0.0, 1.0);
    // The EQG materials' normal maps (modern): the tangent frame from the position's and the texture coordinates'
    // screen derivatives (the meshes carry no tangents), the map's green along +v as Direct3D's maps have it.
    if (g_modern && mode == 0u && (flags & kNormalMap) != 0u && !unlit) n = mapped_normal(n, pc.tex.y);
    // A texture pack's maps, on the classic effect in the modern lighting.
    uint normal_slot = pc.tex2.w & 0xffffu, rough_slot = pc.tex2.w >> 16;
    bool mapped = g_modern && !unlit && mode == 0u && (flags & (kWaterSurface | kLavaSurface)) == 0u;
    vec3 sn = mapped && normal_slot != 0u ? PACK_NORMAL(n, v_world, v_uv, tex(normal_slot, v_uv).rg * 2.0 - 1.0) : n;
    if (unlit) g_light = vec3(1.0);
    else if (g_modern) g_light = modern_light(n, sn, pc.params.x > 0.0);
    else g_light = v_light.rgb; // the lit vertex colour (zone.vert)
    float k = scene.fog.z;
    vec3 color;
    float alpha;
    if (mode == 1u) {
        // Terrain_*Detail: rgb = 2 v0 v1 ColorMap (detail blend), a = ColorMap.a.
        vec4 cmap = tex(pc.tex.x, v_uv);
        vec3 m = tex(pc.tex.y, v_uv).rgb;
        int n = int(pc.extra.w + 0.5);
        vec3 detail = vec3(1.0);
        if (n == 1) {
            detail = tex(pc.tex.z, v_uv2 * pc.extra.x).rgb;
        } else if (n == 2) {
            detail = tex(pc.tex.z, v_uv2 * pc.extra.x).rgb * (1.0 - m.g) + tex(pc.tex.w, v_uv2 * pc.extra.y).rgb * m.g;
        } else if (n >= 3) {
            detail = tex(pc.tex.z, v_uv2 * pc.extra.x).rgb * m.r + tex(pc.tex.w, v_uv2 * pc.extra.y).rgb * m.g +
                     tex(pc.tex2.x, v_uv2 * pc.extra.z).rgb * m.b;
        }
        // NoDetail is a fixed-function technique (DX6, no pixel shader): ColorMap x v0.
        color = n == 0 ? lit(cmap.rgb) : lit(2.0 * v_color2.rgb * cmap.rgb * detail);
        alpha = cmap.a;
    } else if (mode == 2u) {
        // RegionWaterFall PS11: rgb = Diffuse0(t0).rgb x v0, a = Diffuse0(t1).a.
        vec2 t0 = v_uv + pc.slide.xy * k;
        vec2 t1 = v_uv + pc.slide.zw * k;
        vec4 c0 = tex(pc.tex.x, t0), c1 = tex(pc.tex.x, t1);
        alpha = c1.a;
        if (g_modern) {
            // The colour and the coverage scroll at different rates, so the colour is often read where the texture
            // is clear (black in most falls) under a sheet the alpha says is there: dark curtains in the water. The
            // colour of the water is the texture's own where it shows, else its average; the sheet is a little
            // brighter where it is thick (white water) and glints.
            vec3 average = textureLod(textures[nonuniformEXT(pc.tex.x)], v_uv, 14.0).rgb /
                           max(textureLod(textures[nonuniformEXT(pc.tex.x)], v_uv, 14.0).a, 0.05);
            vec3 water = (c0.rgb * c0.a + c1.rgb * c1.a + min(average, vec3(1.0)) * 0.15) / (c0.a + c1.a + 0.15);
            color = lit(water) * (1.0 + 0.35 * c0.a * c1.a);
            vec3 view = normalize(scene.camera_pos.xyz - v_world);
            float rim = pow(1.0 - abs(dot(normalize(v_normal), view)), 3.0);
            color += sky_reflection(vec3(0.0, 0.0, 1.0)) * 0.25 * rim * v_color.a;
        } else {
            color = lit(c0.rgb);
        }
    } else if (mode == 3u) {
        // RegionLava PS14 with PointLight0 = 0 (single pass):
        // c = Diffuse0(A) v0, rgb = lerp(2 Diffuse1(B), c, Diffuse0(A).a), a = 1.
        vec4 d0 = tex(pc.tex.x, v_uv + pc.slide.xy * k);
        vec3 d1 = tex(pc.tex.y, v_uv + pc.slide.zw * k).rgb;
        // The class draws lava and other slow-moving surfaces (energy, slime, mist): the modern lighting makes a
        // fire-coloured one lava (or one its texture's name calls lava, kMolten).
        vec3 average = textureLod(textures[nonuniformEXT(pc.tex.x)], v_uv, 14.0).rgb;
        if (g_modern && ((flags & kMolten) != 0u || warm(average)))
            color = modern_lava(mix(min(2.0 * d1, vec3(1.0)), d0.rgb, d0.a), 1.0 - d0.a,
                                v_world.xy * 0.045 + (pc.slide.xy + pc.slide.zw) * k * 0.5);
        else
            color = mix(emit(2.0 * d1), lit(d0.rgb), d0.a);
        alpha = 1.0;
    } else if (mode == 4u) {
        // RegionTerrain PS11: rgb = (Detail1 w.x + Detail2 w.y) 2 Coverage v1 v0, w = UV set 2.
        vec3 d1 = tex(pc.tex.x, v_uv).rgb;
        vec3 d2 = tex(pc.tex.y, v_uv).rgb;
        vec3 cov = tex(pc.tex.z, v_uv * pc.params.z).rgb;
        color = lit((d1 * v_uv2.x + d2 * v_uv2.y) * 2.0 * cov * v_color.rgb);
        alpha = 1.0;
    } else if (mode == 5u) {
        // RegionWater PS20 without the cube-map reflection (the scene SkyCube
        // is not built): two scrolling normal samples, Fresnel, the water
        // colours by n.v; a = F x 2.9.
        // The normal map is slot 1 (frames hold the diffuse map when the material has one, else the normal map).
        uint wave_slot = pc.tex.y != 0u ? pc.tex.y : pc.tex.x;
        vec2 uv = v_uv * pc.extra.z;
        vec3 n1 = tex(wave_slot, uv - pc.slide.xy * k).rgb * 2.0 - 1.0;
        vec3 n2 = tex(wave_slot, uv * 2.0 + pc.slide.zw * k).rgb * 2.0 - 1.0;
        vec3 wn = n1 + n2;
        float dist = length(v_world - scene.camera_pos.xyz);
        wn.z += 2.0 * (1.0 - clamp((300.0 - dist) / 300.0, 0.0, 1.0));
        wn = normalize(wn);
        vec3 view = normalize(scene.camera_pos.xyz - v_world);
        float nv = clamp(dot(wn, view), 0.0, 1.0);
        float F = (1.0 - pc.extra.x) * pow(1.0 - nv, pc.extra.y) + pc.extra.x;
        vec3 water = mix(pc.extra2.rgb, pc.extra3.rgb, 1.0 - nv);
        if (g_modern && (flags & kEnergy) != 0u) {
            // RegionLavaH (harbingers' towers): the original's Fresnel bias of 1 makes it opaque, its diffuse map is
            // molten rock: drawn as lava running down the walls, the two scrolling normal samples bending it, its
            // noise taken in the texture's own coordinates (the walls stand upright).
            float t = scene.time.x;
            vec2 bend = (n1.xy + n2.xy) * 0.03;
            vec3 rock = tex(pc.tex.x, uv + bend + vec2(0.0, -t * 0.02)).rgb;
            color = modern_lava(rock, 0.3, uv * 3.0 + bend * 4.0 + vec2(0.0, -t * 0.05));
            alpha = 1.0;
        } else if (g_modern) {
            alpha = clamp(F * 2.9, 0.0, 1.0);
            // The water's colour: its two water colours, or where both are black (bloodfalls) the diffuse map's.
            vec3 albedo = mix(pc.extra2.rgb, pc.extra3.rgb, 0.5);
            if (dot(albedo, vec3(1.0)) < 0.03 && pc.tex.x != pc.tex.y)
                albedo = textureLod(textures[nonuniformEXT(pc.tex.x)], v_uv, 14.0).rgb;
            // The reflection colour, as strong as the material reflects (white, at least half, for plain water).
            vec3 tint = mix(vec3(1.0), pc.extra4.rgb, clamp(pc.extra4.a, 0.0, 1.0));
            // A steep surface is a fall, not a level body of water.
            if (abs(v_normal.z) < 0.6 * max(length(v_normal), 1e-6))
                color = falling_water(to_linear(albedo),
                                      to_linear(pc.tex.x != pc.tex.y ? tex(pc.tex.x, uv - pc.slide.xy * k).rgb : albedo),
                                      normalize(wn), to_linear(tint), flags);
            else
                color = modern_water(to_linear(albedo), alpha, flags, to_linear(tint));
        } else {
            vec3 v_lit = g_light + abs(dot(wn, -scene.direction.xyz)) * scene.directional.rgb;
            color = v_lit * water;
        }
        if (!g_modern) alpha = clamp(F * 2.9, 0.0, 1.0);
    } else if (mode == 6u) {
        // COLOROP MODULATE (texture x diffuse); ALPHAOP MODULATE of TEXTUREFACTOR (0xff) and the diffuse alpha.
        color = lit(tex(pc.tex.x, v_uv).rgb);
        alpha = v_color2.a;
    } else {
        vec4 t = tex(pc.tex.x, v_uv);
        color = lit(t.rgb);
        alpha = t.a;
        // The sun's or moon's highlight, shadowed, by the sky's share of the light (v_color.a).
        if (mapped && pc.tex2.w != 0u)
            color += pack_specular(sn, normalize(scene.camera_pos.xyz - v_world), scene.to_light.xyz,
                                   scene.light.rgb * (g_shadow * v_color.a),
                                   rough_slot != 0u ? mix(0.5, 1.0, tex(rough_slot, v_uv).r) : 0.85);
        if (g_modern && (flags & kWaterSurface) != 0u) {
            // The texture's average (its last mip) is the water's colour, a little of the texture itself kept.
            vec3 average = textureLod(textures[nonuniformEXT(pc.tex.x)], v_uv, 14.0).rgb;
            color = modern_water(to_linear(mix(average, t.rgb, 0.3)), alpha, flags, vec3(1.0));
        } else if (g_modern && (flags & kLavaSurface) != 0u) {
            color = modern_lava(t.rgb, 0.0, v_world.xy * 0.045);
        }
    }
    if (g_modern && !unlit && mode == 0u && (flags & (kSpecular | kReflective)) != 0u) color += surface_shine(n, flags);
    if (g_modern && !unlit && (flags & (kWaterSurface | kLavaSurface)) == 0u && mode != 2u && mode != 3u &&
        (scene.weather.x > 0.0 || scene.weather.y > 0.0 || scene.weather.w > 0.0))
        color = weathered(color, n, pc.params.x > 0.0);
    // Alpha test, GREATEREQUAL against the reference (0 = off), or coverage when multisampled.
    float out_alpha;
    if (!alpha_test(alpha, pc.params.x, pc.params.y, out_alpha)) discard;
    // The modern water over the copies is the whole result (it blends the floor itself).
    if (g_modern && (flags & kWaterCopies) != 0u) out_alpha = 1.0;
    // The neighbour zones' debugging view: what comes from where.
    if ((flags & kTintNeighbour) != 0u) color = mix(color, vec3(dot(color, vec3(0.3, 0.6, 0.1))) * vec3(1.6, 0.2, 1.4), 0.75);
    if ((flags & kTintLand) != 0u) color = mix(color, vec3(dot(color, vec3(0.3, 0.6, 0.1))) * vec3(0.2, 1.6, 0.3), 0.6);
    if (g_modern) {
        if ((flags & kNoFog) == 0u)
            color = apply_haze(color, v_world, scene.camera, scene.haze, scene.haze_color, scene.to_light, scene.light);
        color += underwater_shafts(water_map, scene.cover, scene.cover_params, scene.camera.xyz, v_world, scene.to_light,
                                   scene.light, scene.haze_color.rgb, scene.time.x, caustic_hash(gl_FragCoord.xy));
    } else {
        if ((flags & kNoFog) == 0u) color = mix(scene.fog_color.rgb, color, v_fog);
        color = to_linear(color);
    }
    o_color = vec4(color, out_alpha);
}
