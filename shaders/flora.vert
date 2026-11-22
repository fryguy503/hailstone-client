#version 450
#extension GL_GOOGLE_include_directive : require

// The flora (render/flora_renderer.h): a plant per instance, a quad standing on its foot turned by its turn, or two
// crossed ones (vertices 6..11; collapsed for a single quad). Its top sways to and fro over its period, and bends away
// from whoever walks through it (FloraFrame's pushers). Modern: the light that does not cast a shadow here (the sky,
// the place's baked light, the zone's point lights) per vertex; the sun in flora.frag.

#include "lighting.glsl"

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 time; // x seconds, y the last frame's (the motion's)
} pc;

// render/scene_lighting.h SceneLighting.
layout(std430, set = 0, binding = 0) readonly buffer Lighting {
    SCENE_LIGHTING_FIELDS
} lighting;

// render/flora_renderer.cpp FloraFrame.
layout(std430, set = 0, binding = 1) readonly buffer Frame {
    vec4 classic;    // rgb the classic lighting's one light (linear), w 1 modern
    vec4 params;     // x the pushers
    vec4 pushers[8]; // xyz a foot, w how far round it the plants bend
} frame;

layout(location = 0) in vec4 i_position_turn; // xyz the foot, w the turn (radians)
layout(location = 1) in vec4 i_size;          // x width, y height, z the sway's reach (of the height), w its period
layout(location = 2) in uint i_color;         // the ground's colour (RGBA8, 0x80 neutral) for *INHERITVERTEXCOLOR
layout(location = 3) in uint i_flags;         // bits 0..7 the texture, 8..10 the quarter (4 the whole), 11 crossed

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_light; // modern: the shadowless light; classic: the light
layout(location = 2) out vec3 v_world;
layout(location = 3) flat out vec3 v_tint;
layout(location = 4) flat out uint v_texture;
layout(location = 5) out float v_root; // 0 at the foot, 1 at the top
layout(location = 6) out vec4 v_now;   // the motion (shaders/motion.glsl): this frame's place, unjittered
layout(location = 7) out vec4 v_then;  // and the last frame's

#define MOTION_SET 1
#include "motion.glsl"

void main() {
    const vec2 corners[6] = vec2[6](vec2(-1, 0), vec2(1, 0), vec2(1, 1), vec2(-1, 0), vec2(1, 1), vec2(-1, 1));
    int quad = gl_VertexIndex / 6;
    vec2 c = corners[gl_VertexIndex % 6];
    bool crossed = (i_flags & 0x800u) != 0u;
    if (quad == 1 && !crossed) c = vec2(0.0); // a single quad: the second collapses
    float turn = i_position_turn.w + float(quad) * 1.5707963;
    vec3 across = vec3(cos(turn), sin(turn), 0.0);
    vec3 foot = i_position_turn.xyz;
    vec3 p = foot + across * (c.x * i_size.x * 0.5) + vec3(0.0, 0.0, c.y * i_size.y);
    vec3 sway_then = vec3(0.0); // the sway a frame ago less now's, for the motion
    if (c.y > 0.5) {
        if (i_size.w > 0.0) {
            // The sway: the top moves along the plant's own line, its phase from where it stands.
            float phase = dot(foot.xy, vec2(0.13, 0.071));
            float s = sin(6.2831853 * pc.time.x / i_size.w + phase);
            vec3 way = vec3(cos(i_position_turn.w + 0.7), sin(i_position_turn.w + 0.7), 0.0);
            p += way * (s * i_size.z * i_size.y);
            float before = sin(6.2831853 * pc.time.y / i_size.w + phase);
            sway_then = way * ((before - s) * i_size.z * i_size.y);
        }
        // Bent away from a pusher's foot, down and out, the more the nearer it stands.
        int n = int(frame.params.x + 0.5);
        for (int k = 0; k < n; ++k) {
            vec4 q = frame.pushers[k];
            vec2 d = foot.xy - q.xy;
            float dist = length(d);
            if (dist >= q.w || abs(foot.z - q.z) > 10.0) continue;
            float bend = 1.0 - dist / q.w;
            bend = bend * bend * (3.0 - 2.0 * bend);
            vec2 away = dist > 1e-3 ? d / dist : vec2(cos(i_position_turn.w), sin(i_position_turn.w));
            p.xy += away * (bend * i_size.y * 0.7);
            p.z -= bend * i_size.y * 0.45;
        }
    }
    uint q4 = (i_flags >> 8) & 7u;
    vec4 uv = q4 < 4u ? vec4(float(q4 & 1u) * 0.5, float(q4 >> 1) * 0.5, 0.5, 0.5) : vec4(0.0, 0.0, 1.0, 1.0);
    v_uv = uv.xy + vec2((c.x + 1.0) * 0.5, 1.0 - c.y) * uv.zw;
    v_tint = unpackUnorm4x8(i_color).rgb * (255.0 / 128.0);
    v_texture = i_flags & 0xffu;
    v_world = p;
    v_root = c.y;
    if (frame.classic.w > 0.5) {
        // A blade's light comes from above: the sky over the ground, the place's baked light, the lamps near it.
        const vec3 up = vec3(0.0, 0.0, 1.0);
        float sky_share;
        vec3 placed = probe_light(lighting.probe, lighting.sky.w, sky_share);
        vec3 light = hemisphere(up, lighting.sky.rgb, lighting.ground.rgb) * sky_share + placed +
                     to_linear(lighting.special_ambient.rgb) + lighting.vision.rgb;
        int count = int(lighting.params.y + 0.5);
        vec3 carried_sum = vec3(0.0), carried_top = vec3(0.0);
        for (int i = 0; i < count; ++i)
            add_scene_light(light, carried_sum, carried_top, lighting.carried, i,
                            point_light_modern(lighting.lights[i], p, up, 0.5));
        light += carried_light_total(carried_sum, carried_top, lighting.carried);
        v_light = light;
    } else {
        v_light = frame.classic.rgb;
    }
    gl_Position = pc.view_proj * vec4(p, 1.0);
    if (temporal.info.x > 0.0)
        motion_clips(p, p + sway_then, v_now, v_then);
    else
        v_now = v_then = vec4(0.0, 0.0, 0.0, 1.0);
}
