#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"
#include "coverage.glsl"
#include "weather.glsl"

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 fog_color; // a: fog density
    vec4 fog; // start, end, alpha_ref, opacity
    vec4 camera_pos; // w: time-of-day brightness
    uint texture_index;
    uint instance;
    uint maps; // a texture pack's normal map slot | roughness map slot << 16 (0 = none)
    uint flags; // bit 0 unlit, bit 1 no fog
} pc;

layout(set = 0, binding = 2) uniform sampler2D textures[];

layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map; // render/vk_context.h shadow_set
layout(set = 1, binding = 3) uniform sampler2DArray water_map;        // its depths: the water map's
layout(set = 1, binding = 4) uniform sampler2DArrayShadow point_map;  // the zone lights' cubes (render/point_shadows.h)

// render/scene_lighting.h SceneLighting.
layout(std430, set = 0, binding = 1) readonly buffer Lighting {
    SCENE_LIGHTING_FIELDS
} lighting;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) in float v_fog; // oFog (object.vert)
layout(location = 3) in vec3 v_normal;

layout(location = 4) in vec4 v_now;
layout(location = 5) in vec4 v_then;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec2 o_motion;   // the opaque pass's (render/vk_context.h SceneOutput)
layout(location = 2) out float o_reactive; // the blended and additive passes'

#define MOTION_SET 1
#include "motion.glsl"

void main() {
    // The isometric camera's cut (game/iso_occlusion.h).
    if (reveal_discard(v_world, gl_FragCoord.xy, lighting.reveal_origin, lighting.reveal_axis, lighting.reveal_shape,
                       lighting.reveal_roof))
        discard;
    vec4 tex = texture(textures[pc.texture_index], v_uv);
    float out_alpha;
    if (!alpha_test(tex.a, pc.fog.z, pc.fog.w, out_alpha)) discard;
    vec3 color;
    if ((pc.flags & 1u) != 0u) {
        color = to_linear(tex.rgb);
        if ((pc.flags & 2u) == 0u)
            color = lighting.params.x > 0.5
                ? apply_haze(color, v_world, lighting.camera, lighting.haze, lighting.haze_color,
                             lighting.to_light, lighting.light)
                : to_linear(mix(pc.fog_color.rgb, tex.rgb, v_fog));
    } else if (lighting.params.x > 0.5) {
        // Modern (render/scene_lighting.h): lit like the zone around it, per pixel, with the nearby point lights;
        // alpha-tested plants let the light through.
        float len = length(v_normal);
        vec3 n = len > 1e-6 ? v_normal / len : vec3(0.0, 0.0, 1.0);
        // A texture pack's normal map shades; the surface's normal still places the shadows.
        uint normal_slot = pc.maps & 0xffffu, rough_slot = pc.maps >> 16;
        vec3 sn = normal_slot != 0u ? PACK_NORMAL(n, v_world, v_uv, texture(textures[normal_slot], v_uv).rg * 2.0 - 1.0) : n;
        float ndl = dot(sn, lighting.to_light.xyz);
        ndl = pc.fog.z > 0.0 ? clamp(ndl * 0.6 + 0.4, 0.0, 1.0) : max(ndl, 0.0);
        float sun = shadow_visibility(shadow_map, lighting.shadow_view_proj, lighting.shadow_splits, lighting.shadow_texel,
                                 lighting.shadow_params, lighting.shadow_origin, lighting.shadow_forward, v_world, n);
        ndl *= sun;
        float water_sun, water_sky;
        underwater_light(water_map, lighting.cover, lighting.cover_params, v_world, lighting.to_light, lighting.time.x,
                         water_sun, water_sky);
        // The baked light of the place it stands in, and how much of the sky reaches it there.
        float sky_share;
        vec3 placed = probe_light(lighting.probe, lighting.sky.w, sky_share);
        vec3 light = hemisphere(sn, lighting.sky.rgb, lighting.ground.rgb) * (water_sky * sky_share) + placed +
                     lighting.light.rgb * ndl * water_sun + to_linear(lighting.special_ambient.rgb) + lighting.vision.rgb;
        int count = int(lighting.params.y + 0.5);
        vec3 carried_sum = vec3(0.0), carried_top = vec3(0.0);
        for (int i = 0; i < count; ++i) {
            vec3 c = point_light_modern(lighting.lights[i], v_world, sn, 0.0);
            // Out of reach or facing away: nothing to shadow (the product is 0 either way).
            if (c != vec3(0.0))
                add_scene_light(light, carried_sum, carried_top, lighting.carried, i,
                                c * scene_light_shadow(point_map, lighting.point_shadow_light, lighting.point_shadow_slot,
                                                       lighting.point_shadow_params, lighting.lights[i], v_world, n));
        }
        light += carried_light_total(carried_sum, carried_top, lighting.carried);
        vec3 surface = to_linear(tex.rgb) * max(light, vec3(lighting.params.w));
        if (pc.maps != 0u)
            surface += pack_specular(sn, normalize(lighting.camera.xyz - v_world), lighting.to_light.xyz,
                                     lighting.light.rgb * (sun * water_sun),
                                     rough_slot != 0u ? mix(0.5, 1.0, texture(textures[rough_slot], v_uv).r) : 0.85);
        // The weather (shaders/weather.glsl): snow on the roofs, ledges and boughs, the rain's soak; no puddles.
        if (lighting.weather.x > 0.0 || lighting.weather.y > 0.0) {
            float open = sky_open(shadow_map, lighting.cover, lighting.cover_params, lighting.shadow_params.y, v_world + n * 0.5);
            if (open > 0.0) {
                vec3 horizon = lighting.haze.z > 0.5 ? lighting.haze_color.rgb : to_linear(pc.fog_color.rgb);
                surface = weather_surface(surface, to_linear(vec3(0.88, 0.9, 0.94)) * max(light, vec3(lighting.params.w)),
                                          v_world, sn, normalize(lighting.camera.xyz - v_world), lighting.weather, open,
                                          horizon, lighting.sky.rgb, 1.0);
            }
        }
        color = (pc.flags & 2u) == 0u
            ? apply_haze(surface, v_world, lighting.camera,
                         lighting.haze, lighting.haze_color, lighting.to_light, lighting.light) : surface;
        color += underwater_shafts(water_map, lighting.cover, lighting.cover_params, lighting.camera.xyz, v_world,
                                   lighting.to_light, lighting.light, lighting.haze_color.rgb, lighting.time.x,
                                   caustic_hash(gl_FragCoord.xy));
    } else {
        color = to_linear(mix(pc.fog_color.rgb, tex.rgb * pc.camera_pos.w, v_fog));
    }
    o_color = vec4(color, out_alpha);
    o_motion = motion_uv(v_now, v_then);
    o_reactive = reactive_of(o_color);
}
