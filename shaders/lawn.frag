#version 450
#extension GL_GOOGLE_include_directive : require

// Hailstone's lawn (lawn.vert): a blade's colour is the ground's at its root (so the carpet meets the ground it grows
// from), shaded down there, brighter and a touch warmer towards the tip. Modern: lit from above and through the
// blade (the sun or moon through its shadow, a little more when the light is behind it), the vertex's other light,
// into the haze. Classic: the scene's one light.

#include "lighting.glsl"

// render/scene_lighting.h SceneLighting.
layout(std430, set = 0, binding = 0) readonly buffer Lighting {
    SCENE_LIGHTING_FIELDS
} lighting;

layout(std430, set = 0, binding = 1) readonly buffer Frame {
    vec4 classic;
    vec4 params;
    vec4 pushers[8];
} frame;

layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map; // render/vk_context.h shadow_set

layout(location = 0) in vec3 v_light;
layout(location = 1) in vec3 v_world;
layout(location = 2) in vec3 v_normal;
layout(location = 3) flat in vec3 v_ground;
layout(location = 4) in float v_up;
layout(location = 5) flat in float v_shade;

layout(location = 0) out vec4 o_color;

// flora.frag's blade_shadow: one hardware-filtered comparison.
float blade_shadow(vec3 p) {
    if (lighting.shadow_params.x < 0.5) return 1.0;
    vec4 splits = lighting.shadow_splits;
    float depth = dot(p - lighting.shadow_origin.xyz, lighting.shadow_forward.xyz);
    int c = depth < splits.x ? 0 : depth < splits.y ? 1 : depth < splits.z ? 2 : depth < splits.w ? 3 : 4;
    if (c > 3) return 1.0;
    vec4 s = lighting.shadow_view_proj[c] * vec4(p + vec3(0.0, 0.0, lighting.shadow_texel[c] * 1.5), 1.0);
    vec3 q = s.xyz / s.w;
    float lit = texture(shadow_map, vec4(q.xy * 0.5 + 0.5, float(c), q.z));
    return mix(lit, 1.0, smoothstep(splits.w * 0.85, splits.w, depth));
}

void main() {
    // The isometric camera's cut (game/iso_occlusion.h).
    if (reveal_discard(v_world, gl_FragCoord.xy, lighting.reveal_origin, lighting.reveal_axis, lighting.reveal_shape,
                       lighting.reveal_roof))
        discard;
    float up = clamp(v_up, 0.0, 1.0);
    // The ground's hue, its brightness held within a band (and its colour a little stronger), so a pale or a dark
    // texel does not make a pale or a dark patch: the carpet stays even across the ground's variations.
    vec3 ground = to_linear(v_ground);
    float lum = max(dot(ground, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    ground *= clamp(lum, 0.045, 0.1) / lum;
    ground = max(mix(vec3(dot(ground, vec3(0.2126, 0.7152, 0.0722))), ground, 1.1), vec3(0.0));
    // Towards the tip: brighter, and warmer (less blue).
    vec3 albedo = ground * mix(0.85, 1.35, up) * mix(vec3(1.0), vec3(1.06, 1.04, 0.8), up * up) * v_shade;
    float roots = mix(0.55, 1.0, smoothstep(0.0, 0.6, up));
    if (frame.classic.w < 0.5) {
        o_color = vec4(albedo * roots * v_light, 1.0);
        return;
    }
    // A blade's light is soft: its face's normal turned most of the way up (the lawn is lit as the ground under it,
    // not blade by blade), and some of the light that falls on its back comes through.
    vec3 face = normalize(v_normal);
    vec3 n = normalize(mix(face, vec3(0.0, 0.0, 1.0), 0.7));
    float ndl = max(dot(n, lighting.to_light.xyz), 0.0);
    float through = max(dot(-face, lighting.to_light.xyz), 0.0) * 0.35 * up;
    float sun = (ndl * 0.85 + through) * blade_shadow(v_world);
    vec3 light = (v_light + lighting.light.rgb * sun) * roots;
    vec3 surface = albedo * max(light, vec3(lighting.params.w));
    o_color = vec4(apply_haze(surface, v_world, lighting.camera, lighting.haze, lighting.haze_color, lighting.to_light,
                              lighting.light),
                   1.0);
}
