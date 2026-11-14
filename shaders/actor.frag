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
    vec4 camera_pos; // w: the part's native armor surface-table index; zero is the old material path
    uint texture_index;
    uint bone_offset;
    uint tint; // 0xAARRGGBB; high byte 0 = none
    uint texture2; // SkinMeshLuclin's second texture (s_SamplerDiffuse1); 0xffffffff = none
} pc;

layout(set = 0, binding = 2) uniform sampler2D textures[];

layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map; // render/vk_context.h shadow_set
layout(set = 1, binding = 3) uniform sampler2DArray water_map;        // its depths: the water map's
layout(set = 1, binding = 4) uniform sampler2DArrayShadow point_map;  // the zone lights' cubes (render/point_shadows.h)

// render/scene_lighting.h SceneLighting.
struct ActorSurface {
    vec4 params; // shininess, gloss, environment strength, unused
    uvec4 slots; // native normal, octahedral environment, flags, unused
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

layout(location = 0) out vec4 o_color;

// Native EQG normal maps store a full RGB vector; a texture pack's BC5 maps store RG and reconstruct Z instead.
vec3 armor_normal(vec3 n, uint slot) {
    vec3 dp1 = dFdx(v_world), dp2 = dFdy(v_world);
    vec2 duv1 = dFdx(v_uv), duv2 = dFdy(v_uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = max(dot(t, t), dot(b, b));
    if (!(scale > 1e-20)) return n;
    float inv = inversesqrt(scale);
    vec3 m = texture(textures[slot], v_uv).rgb * 2.0 - 1.0;
    vec3 mapped = t * inv * m.x + b * inv * m.y + n * m.z;
    float len = length(mapped);
    return len > 1e-6 ? mapped / len : n;
}

vec2 armor_environment_uv(vec3 d) {
    d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-6);
    vec2 p = d.z >= 0.0 ? d.xy : (1.0 - abs(d.yx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}

vec3 armor_shine(ActorSurface surface, vec3 n, vec3 view, float sun) {
    vec3 shine = vec3(0.0);
    if ((surface.slots.z & 1u) != 0u) {
        vec3 halfway = view + lighting.to_light.xyz;
        float len = length(halfway);
        vec3 h = len > 1e-6 ? halfway / len : n;
        float power = surface.params.x;
        shine += lighting.light.rgb * sun * max(dot(n, lighting.to_light.xyz), 0.0) *
                 pow(max(dot(n, h), 0.0), power) * surface.params.y * (power + 8.0) / 25.0;
    }
    if ((surface.slots.z & 2u) != 0u && surface.slots.y != 0u) {
        vec3 r = reflect(-view, n);
        float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 5.0);
        vec3 environment = to_linear(textureLod(textures[surface.slots.y], armor_environment_uv(r), 0.5).rgb);
        shine += environment * min(surface.params.z, 1.0) * mix(0.25, 1.0, fresnel);
    }
    return shine;
}

void main() {
    // In the colour pass the slots' high 16 bits carry a texture pack's maps (render/actor_renderer.cpp).
    uint colour_slot = pc.texture_index & 0xffffu, second_slot = pc.texture2 & 0xffffu;
    uint normal_slot = pc.texture2 >> 16, rough_slot = pc.texture_index >> 16;
    vec4 tex = texture(textures[colour_slot], v_uv);
    float out_alpha;
    // A cut-out's alpha grows with the mip level sampled (not the original's): the smaller levels average its thin
    // parts below the test's reference, and a model made of them (the Iksar skeleton SKI, Chroma at 192/255, whose
    // DXT5 alpha is no strict cut-out to keep coverage for) faded to specks a few dozen units away.
    float test_alpha = pc.fog.z > 0.0
        ? tex.a * (1.0 + max(textureQueryLod(textures[colour_slot], v_uv).x, 0.0) * 0.25)
        : tex.a;
    if (!alpha_test(test_alpha, pc.fog.z, pc.fog.w, out_alpha)) discard;
    // The actor's SetTint colour (the effect's Tint parameter; none = white).
    vec3 tint = (pc.tint >> 24) != 0u
        ? vec3(float((pc.tint >> 16) & 0xffu), float((pc.tint >> 8) & 0xffu), float(pc.tint & 0xffu)) / 255.0
        : vec3(1.0);
    vec3 base;
    if (second_slot != 0xffffu) {
        // SkinMeshLuclin (ps_1_1): lerp(t1.a, t1.rgb * Tint.rgb, t0.rgb), lit after.
        vec4 t1 = texture(textures[second_slot], v_uv);
        base = mix(tex.rgb, t1.rgb * tint, t1.a);
    } else {
        base = tex.rgb * tint; // SkinMeshOld: lit * Tint.rgb
    }
    vec3 color;
    if (lighting.params.x > 0.5) {
        // Modern (render/scene_lighting.h): the daylight, the special ambient and the nearby point lights, per pixel
        // and wrapped for the rounded bodies, never below the floor so a character stays readable in the dark.
        float len = length(v_normal);
        vec3 n = len > 1e-6 ? v_normal / len : vec3(0.0, 0.0, 1.0);
        // Native armor maps take precedence over the pack; the surface's normal still places the shadows.
        ActorSurface surface = lighting.surfaces[uint(pc.camera_pos.w)];
        vec3 sn = surface.slots.x != 0u ? armor_normal(n, surface.slots.x)
            : (normal_slot != 0u ? PACK_NORMAL(n, v_world, v_uv, texture(textures[normal_slot], v_uv).rg * 2.0 - 1.0) : n);
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
        for (int i = 0; i < count; ++i)
            light += point_light_modern(lighting.lights[i], v_world, sn, 0.4) *
                     point_shadow(point_map, lighting.point_shadow_light, lighting.point_shadow_slot,
                                  lighting.point_shadow_params, lighting.lights[i].position_radius, v_world, n);
        light = max(light, vec3(lighting.params.w));
        vec3 shaded = to_linear(base) * light;
        if (surface.slots.x == 0u && (surface.slots.z & 1u) == 0u && (normal_slot != 0u || rough_slot != 0u))
            shaded += pack_specular(sn, normalize(lighting.camera.xyz - v_world), lighting.to_light.xyz,
                                     lighting.light.rgb * (sun * water_sun),
                                     rough_slot != 0u ? mix(0.5, 1.0, texture(textures[rough_slot], v_uv).r) : 0.85);
        vec3 view_delta = lighting.camera.xyz - v_world;
        float view_length = length(view_delta);
        vec3 armor_view = view_length > 1e-6 ? view_delta / view_length : sn;
        shaded += armor_shine(surface, sn, armor_view, sun * water_sun);
        // The rain's sheen on whoever stands out in it (shaders/weather.glsl), lighter than the ground's; no snow:
        // they move, and drifts fixed in the world would slide over them.
        if (lighting.weather.x > 0.0) {
            float open = sky_open(shadow_map, lighting.cover, lighting.cover_params, lighting.shadow_params.y, v_world + n * 0.5);
            if (open > 0.0) {
                vec3 horizon = lighting.haze.z > 0.5 ? lighting.haze_color.rgb : to_linear(pc.fog_color.rgb);
                shaded = weather_surface(shaded, shaded, v_world, sn, normalize(lighting.camera.xyz - v_world),
                                          vec4(lighting.weather.x, 0.0, 0.0, 0.0), open, horizon, lighting.sky.rgb,
                                          lighting.time.x, 0.0, false, 0.6);
            }
        }
        color = apply_haze(shaded, v_world, lighting.camera, lighting.haze, lighting.haze_color,
                           lighting.to_light, lighting.light);
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
        color = to_linear(mix(pc.fog_color.rgb, color, v_fog));
    }
    o_color = vec4(color, out_alpha);
}
