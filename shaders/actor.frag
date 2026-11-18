#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"
#include "coverage.glsl"
#include "weather.glsl"
#include "preview_alpha.glsl"
#include "zone_surface.glsl"

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 fog_color; // a: fog density
    vec4 fog; // start, end, alpha_ref, opacity
    vec4 camera_pos; // w: 12-bit native armor surface index, bit 12 unlit, bit 13 no fog
    uint texture_index;
    uint bone_offset;
    uint tint; // 0xAARRGGBB; high byte 0 = none
    uint texture2; // SkinMeshLuclin's second texture (s_SamplerDiffuse1); 0xffffffff = none
} pc;

layout(set = 0, binding = 2) uniform sampler2D textures[];

// The additive pass (render/actor_renderer.cpp): its motion is kept only where it adds something to see
// (SceneOutput::MotionReactiveWhereSeen blends it by o_motion's alpha).
layout(constant_id = 2) const bool kAdditive = false;
layout(constant_id = 3) const bool kPreview = false;

layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map; // render/vk_context.h shadow_set
layout(set = 1, binding = 3) uniform sampler2DArray water_map;        // its depths: the water map's
layout(set = 1, binding = 4) uniform sampler2DArrayShadow point_map;  // the zone lights' cubes (render/point_shadows.h)

// render/scene_lighting.h SceneLighting.
struct ActorSurface {
    vec4 params; // shininess, gloss, environment strength, coverage UV scale
    uvec4 slots; // native normal, octahedral environment, flags, coverage/second diffuse/banner cloth tint
    vec4 slide; // authored colour and alpha UV velocities
};
layout(std430, set = 0, binding = 1) readonly buffer Lighting {
    SCENE_LIGHTING_FIELDS
    ActorSurface surfaces[];
} lighting;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) in float v_light;
layout(location = 3) in float v_fog; // oFog (actor.vert)
layout(location = 4) in vec3 v_normal;

layout(location = 5) in vec4 v_now;
layout(location = 6) in vec4 v_then;
layout(location = 7) in vec2 v_uv2;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_motion;   // the opaque and blended passes' (render/vk_context.h SceneOutput); blue
                                          // the depth, which only the blended pass writes (MotionReactive)
layout(location = 2) out float o_reactive; // the blended and additive passes'

#define MOTION_SET 1
#include "motion.glsl"

// Native packed maps share the zone sampler's RG normal, B illumination and A gloss channels.
vec3 armor_normal_vector(vec3 n, vec3 m) {
    vec3 dp1 = dFdx(v_world), dp2 = dFdy(v_world);
    vec2 duv1 = dFdx(v_uv), duv2 = dFdy(v_uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = max(dot(t, t), dot(b, b));
    // No tangent frame (collapsed UVs, a helper pixel): the surface's own normal. Not n * m.z, which a map's
    // negative z (lava's dark blue, an RGB map's low blue) would turn to face away.
    if (!(scale > 1e-20)) return n;
    float inv = inversesqrt(scale);
    vec3 mapped = t * inv * m.x + b * inv * m.y + n * m.z;
    return mapped;
}

vec3 armor_normal(vec3 n, vec4 texel, bool packed_channels) {
    vec3 mapped = armor_normal_vector(n, zone_surface_normal(texel, packed_channels));
    float len = length(mapped);
    return len > 1e-6 ? mapped / len : n;
}

vec2 armor_environment_uv(vec3 d) {
    d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-6);
    vec2 p = d.z >= 0.0 ? d.xy : (1.0 - abs(d.yx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}

vec3 armor_shine(ActorSurface surface, vec3 n, vec3 view, float sun, float gloss) {
    vec3 shine = vec3(0.0);
    if ((surface.slots.z & 1u) != 0u) {
        vec3 halfway = view + lighting.to_light.xyz;
        float len = length(halfway);
        vec3 h = len > 1e-6 ? halfway / len : n;
        float power = surface.params.x;
        shine += lighting.light.rgb * sun * max(dot(n, lighting.to_light.xyz), 0.0) *
                 pow(max(dot(n, h), 0.0), power) * gloss * (power + 8.0) / 25.0;
    }
    if ((surface.slots.z & 2u) != 0u && surface.slots.y != 0u) {
        vec3 r = reflect(-view, n);
        float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 5.0);
        vec3 environment = to_linear(textureLod(textures[surface.slots.y], armor_environment_uv(r), 0.5).rgb);
        shine += environment * min(surface.params.z, 1.0) * mix(0.25, 1.0, fresnel);
    }
    return shine;
}

vec3 material_tint(uint argb) {
    if ((argb >> 24) == 0u) return vec3(1.0);
    return vec3(float((argb >> 16) & 0xffu), float((argb >> 8) & 0xffu), float(argb & 0xffu)) / 255.0;
}

void main() {
    uint material = uint(pc.camera_pos.w);
    bool unlit = (material & 4096u) != 0u, fogged = (material & 8192u) == 0u;
    // In the colour pass the slots' high 16 bits carry a texture pack's maps (render/actor_renderer.cpp).
    uint colour_slot = pc.texture_index & 0xffffu, second_slot = pc.texture2 & 0xffffu;
    uint normal_slot = pc.texture2 >> 16, rough_slot = pc.texture_index >> 16;
    ActorSurface surface = lighting.surfaces[material & 4095u];
    bool waterfall = (surface.slots.z & 32u) != 0u;
    bool lava = (surface.slots.z & 64u) != 0u;
    vec2 color_uv = waterfall || lava ? v_uv + surface.slide.xy * lighting.time.w : v_uv;
    vec2 secondary_uv = v_uv + surface.slide.zw * lighting.time.w;
    vec4 tex = texture(textures[colour_slot], color_uv);
    if (waterfall) tex.a = texture(textures[colour_slot], v_uv + surface.slide.zw * lighting.time.w).a;
    float lava_alpha = tex.a;
    if (lava) tex.a = 1.0;
    if ((surface.slots.z & 16u) != 0u)
        tex.rgb = surface_coverage_color(tex.rgb, texture(textures[surface.slots.w], v_uv * surface.params.w).rgb);
    float out_alpha;
    // A cut-out's alpha grows with the mip level sampled (not the original's): the smaller levels average its thin
    // parts below the test's reference, and a model made of them (the Iksar skeleton SKI, Chroma at 192/255, whose
    // DXT5 alpha is no strict cut-out to keep coverage for) faded to specks a few dozen units away.
    float test_alpha = pc.fog.z > 0.0
        ? tex.a * (1.0 + max(textureQueryLod(textures[colour_slot], v_uv).x, 0.0) * 0.25)
        : tex.a;
    if (!alpha_test(test_alpha, pc.fog.z, pc.fog.w, out_alpha)) discard;
#ifdef HAILSTONE_PREVIEW
    // Alpha-to-coverage accepts some samples; their stored alpha must be 1 so resolving does not square coverage.
    // This separate shader variant never combines SampleMask with fixed-function alpha-to-coverage.
    gl_SampleMask[0] = kAlphaToCoverage ? alpha_coverage_samples(out_alpha, gl_FragCoord.xy) : -1;
    if (kAlphaToCoverage) out_alpha = 1.0;
#endif
    vec3 tint = material_tint(pc.tint);
    vec3 base;
    if ((surface.slots.z & 128u) != 0u) {
        base = tex.rgb * material_tint(surface.slots.w);
        if (second_slot != 0xffffu) {
            vec4 emblem = texture(textures[second_slot], v_uv2);
            base = mix(base, emblem.rgb * tint, emblem.a);
        }
    } else if (second_slot != 0xffffu) {
        // SkinMeshLuclin (ps_1_1): lerp(t1.a, t1.rgb * Tint.rgb, t0.rgb), lit after.
        vec4 t1 = texture(textures[second_slot], v_uv);
        base = mix(tex.rgb, t1.rgb * tint, t1.a);
    } else {
        base = tex.rgb * tint; // SkinMeshOld: lit * Tint.rgb
    }
    bool packed_channels = (surface.slots.z & 4u) != 0u;
    bool glowing = (surface.slots.z & 8u) != 0u;
    // The classic lighting reads the native map only for its glow (lava takes its own samples below), so a part that
    // does not glow skips the fetch there. Both conditions are uniform across the draw.
    vec4 native_surface = surface.slots.x != 0u && (lighting.params.x > 0.5 || glowing)
                              ? texture(textures[surface.slots.x], v_uv) : vec4(0.0);
    vec3 lava_secondary = lava && surface.slots.w != 0u ? texture(textures[surface.slots.w], secondary_uv).rgb * tint : base;
    vec3 lava_normal = vec3(0.0);
    if (lava && surface.slots.x != 0u)
        lava_normal = 2.0 * (texture(textures[surface.slots.x], color_uv).rgb +
                             texture(textures[surface.slots.x], secondary_uv).rgb) - 2.0;
    vec3 color;
    if (unlit) {
        // AddAlpha's authored SELECTARG1 texture colour is independent of scene light, including in previews.
        color = to_linear(base);
        if (fogged)
            color = lighting.params.x > 0.5
                ? apply_haze(color, v_world, lighting.camera, lighting.haze, lighting.haze_color,
                             lighting.to_light, lighting.light)
                : to_linear(mix(pc.fog_color.rgb, base, v_fog));
    } else if (lighting.params.x > 0.5) {
        // Modern (render/scene_lighting.h): the daylight, the special ambient and the nearby point lights, per pixel
        // and wrapped for the rounded bodies, never below the floor so a character stays readable in the dark.
        float len = length(v_normal);
        vec3 n = len > 1e-6 ? v_normal / len : vec3(0.0, 0.0, 1.0);
        // Native armor maps take precedence over the pack; the surface's normal still places the shadows.
        vec3 sn = surface.slots.x != 0u ? armor_normal(n, native_surface, packed_channels)
            : (normal_slot != 0u ? PACK_NORMAL(n, v_world, v_uv, texture(textures[normal_slot], v_uv).rg * 2.0 - 1.0) : n);
        if (lava && surface.slots.x != 0u) {
            vec3 moving_normal = armor_normal_vector(n, lava_normal);
            float moving_length = length(moving_normal);
            sn = moving_length > 1e-6 ? moving_normal / moving_length : n;
        }
        float ndl = clamp((dot(sn, lighting.to_light.xyz) + 0.3) / 1.3, 0.0, 1.0);
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
        for (int i = 0; i < count; ++i) {
            vec3 c = point_light_modern(lighting.lights[i], v_world, sn, 0.4);
            // Out of reach or facing away: nothing to shadow (the product is 0 either way).
            if (c != vec3(0.0))
                light += c * scene_light_shadow(point_map, lighting.point_shadow_light, lighting.point_shadow_slot,
                                                lighting.point_shadow_params, lighting.lights[i], v_world, n);
        }
        light = max(light, vec3(lighting.params.w));
        vec3 shaded = to_linear(base) * light;
        if (lava) {
            float bump = clamp(dot(armor_normal_vector(n, lava_normal), lighting.to_light.xyz), 0.0, 1.0);
            shaded = surface_lava_color(to_linear(base) * (light + bump * lighting.light.rgb),
                                         to_linear(lava_secondary), lava_alpha, bump);
        }
        if (surface.slots.x == 0u && (surface.slots.z & 1u) == 0u && (normal_slot != 0u || rough_slot != 0u))
            shaded += pack_specular(sn, normalize(lighting.camera.xyz - v_world), lighting.to_light.xyz,
                                     lighting.light.rgb * (sun * water_sun),
                                     rough_slot != 0u ? mix(0.5, 1.0, texture(textures[rough_slot], v_uv).r) : 0.85);
        vec3 view_delta = lighting.camera.xyz - v_world;
        float view_length = length(view_delta);
        vec3 armor_view = view_length > 1e-6 ? view_delta / view_length : sn;
        shaded += armor_shine(surface, sn, armor_view, sun * water_sun,
                              zone_surface_gloss(native_surface, packed_channels, surface.params.y));
        // The rain's sheen on whoever stands out in it (shaders/weather.glsl), lighter than the ground's; no snow:
        // they move, and drifts fixed in the world would slide over them.
        if (lighting.weather.x > 0.0) {
            float open = sky_open(shadow_map, lighting.cover, lighting.cover_params, lighting.shadow_params.y, v_world + n * 0.5);
            if (open > 0.0) {
                vec3 horizon = lighting.haze.z > 0.5 ? lighting.haze_color.rgb : to_linear(pc.fog_color.rgb);
                shaded = weather_surface(shaded, shaded, v_world, sn, normalize(lighting.camera.xyz - v_world),
                                          vec4(lighting.weather.x, 0.0, 0.0, 0.0), open, horizon, lighting.sky.rgb, 0.6);
            }
        }
        if (glowing) shaded += zone_surface_glow(to_linear(base), native_surface.b, true);
        color = fogged ? apply_haze(shaded, v_world, lighting.camera, lighting.haze, lighting.haze_color,
                                    lighting.to_light, lighting.light) : shaded;
        color += underwater_shafts(water_map, lighting.cover, lighting.cover_params, lighting.camera.xyz, v_world,
                                   lighting.to_light, lighting.light, lighting.haze_color.rgb, lighting.time.x,
                                   caustic_hash(gl_FragCoord.xy));
    } else {
        // Classic (render::classic_actor_light, its mirror; not the original's, which is not traced): the region
        // shaders' vertex light for the zone's baked light where the player stands, so a character takes the light of
        // the walls round it; v_light shapes the body.
        float len = length(v_normal);
        vec3 n = len > 1e-6 ? v_normal / len : vec3(0.0, 0.0, 1.0);
        vec3 baked = vec3(0.0);
        float share = 1.0;
        if (lighting.probe.w > 0.5) {
            baked = lighting.probe.rgb;
            share = lighting.probe.w - 1.0;
        }
        vec3 dn = lighting.direction.xyz;
        vec3 light = baked + lighting.special_ambient.rgb +
                     share * (lighting.ambient.rgb + lighting.bounce.rgb * max(dot(n, dn), 0.0) +
                              lighting.directional.rgb * max(dot(n, -dn), 0.0));
        light = clamp(light, vec3(0.25), vec3(1.0)); // render::kClassicActorFloor
        color = min(base * light * v_light, vec3(1.0));
        if (lava) {
            float bump = clamp(dot(armor_normal_vector(n, lava_normal), -dn), 0.0, 1.0);
            color = surface_lava_color(color + base * bump * lighting.directional.rgb,
                                        lava_secondary, lava_alpha, bump);
        }
        if (glowing) color += zone_surface_glow(base, native_surface.b, false);
        color = to_linear(mix(pc.fog_color.rgb, color, v_fog));
    }
    o_color = vec4(color, kPreview && kAdditive ? preview_additive_alpha(color) : out_alpha);
    // Additive, a fragment adds its colour as it is: one that adds little (a glow's faint spread) leaves the motion of
    // what is behind it.
    o_motion = vec4(motion_uv(v_now, v_then), gl_FragCoord.z,
                    kAdditive ? step(0.02, max(color.r, max(color.g, color.b))) : 1.0);
    o_reactive = reactive_of(o_color);
}
