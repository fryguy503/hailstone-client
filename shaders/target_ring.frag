#version 450
#extension GL_GOOGLE_include_directive : require

// The target ring (render/target_ring_renderer.h; docs/graphics.md "The target ring"), Hailstone's look: a soft band
// in the con colour with a halo, dashes on an inner track turning round it, a notch outside it where the target faces,
// the sweep in from outside on a new target and the pulse while attacking (game::target_ring_look).
//
// Where it lies: each pixel's scene point is rebuilt from the depth copy. A level-ish surface within the height range
// of the feet takes the ring as a decal, so it follows the ground and anything standing in front hides it (a leg is
// not level, a wall is not, a character's head is out of range). A level surface with open air just below it (a
// belly, a back, the top of a paw raised off the ground) is not ground (overhangs). Elsewhere the ring is a level one
// at the feet, shown where it is in front of the scene; behind it, only the x-ray share.

layout(set = 0, binding = 2) uniform sampler2D scene_depth;

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 camera_time;
    vec4 screen;
    vec4 haze;
    vec4 forward;
} pc;

layout(location = 0) in vec3 v_world;
layout(location = 1) flat in vec3 v_normal;
layout(location = 2) flat in vec4 v_feet;
layout(location = 3) flat in vec4 v_color;
layout(location = 4) flat in vec4 v_shape;
layout(location = 5) flat in vec4 v_cues;

layout(location = 0) out vec4 o_color;
layout(location = 2) out float o_reactive; // the temporal upscaler's reactive mask (shaders/reactive.glsl)

#include "reactive.glsl"

const float kTau = 6.28318530718;

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

float gauss(float x) { return exp(-x * x); }

// The ring's cover at `d` from the feet (world units, level): x the band's core (the bright part), y everything.
vec2 ring_at(vec2 d) {
    float radius = v_feet.w, w = v_shape.x, spin = v_shape.y, facing = v_shape.z;
    float pulse = v_cues.x, sweep = v_cues.y;
    float dist = length(d);
    float a = atan(d.y, d.x) - facing;
    float e = (dist - radius) / w;
    float core = gauss(e * 1.3) * (1.0 + 0.5 * pulse);
    float halo = gauss(e * 0.35) * (0.28 + 0.25 * pulse);
    // The inner track's dashes: about one a world unit and a half round, turning.
    float count = clamp(floor(radius * 2.2), 12.0, 48.0);
    float f = fract((a + spin) / kTau * count);
    float dash = smoothstep(0.0, 0.08, f) * smoothstep(0.62, 0.54, f);
    float track = gauss((dist - (radius - 2.6 * w)) / (0.5 * w)) * dash * 0.6;
    // The facing notch: a chevron just outside the band pointing the way the target faces.
    float along = dist * cos(a), across = abs(dist * sin(a));
    float t = (along - (radius + 1.6 * w)) / (2.6 * w);
    float notch = 0.0;
    if (t > 0.0 && t < 1.0) {
        float edge = 2.2 * w * (1.0 - t) - across;
        notch = smoothstep(0.0, 0.25 * w, edge) * smoothstep(0.0, 0.15, t) * 0.9;
    }
    // The sweep: a ring closing in from 1.6 radii as it fades.
    float swept = sweep > 0.0 ? gauss((dist - radius * (1.0 + 0.6 * sweep)) / (1.5 * w)) * sweep : 0.0;
    float all = core + halo + track + notch + swept;
    return vec2(core + notch * 0.5 + swept, all);
}

// Whether a point `drop` below the scene point `p` is in sight: then `p` lies on something with air under it (the
// target's belly or back, another character), not on the ground, where that point is inside the ground and the
// ground in front of it hides it. Off the screen it cannot tell, and says no.
bool overhangs(vec3 p, float drop) {
    vec3 q = p - vec3(0.0, 0.0, drop);
    vec4 clip = pc.view_proj * vec4(q, 1.0);
    if (clip.w <= 1e-4) return false;
    vec2 ndc = clip.xy / clip.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); // the scene's viewport is flipped
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
    float sd = textureLod(scene_depth, uv, 0.0).r;
    float scene_d = sd > 0.0 ? pc.screen.y / (sd + pc.screen.x) : 1e9;
    float q_d = dot(q - pc.camera_time.xyz, pc.forward.xyz);
    // The ground in front of a point below it is at least about `drop` nearer along the ray.
    return q_d < scene_d - 0.4 * drop;
}

void main() {
    vec3 cam = pc.camera_time.xyz;
    vec3 ray = v_world - cam;
    float ray_d = dot(ray, pc.forward.xyz);
    vec2 suv = gl_FragCoord.xy * pc.screen.zw;
    float sd = texture(scene_depth, suv).r;
    bool has_scene = sd > 0.0; // reverse Z: the sky is 0
    float scene_d = has_scene ? pc.screen.y / (sd + pc.screen.x) : 1e9;
    vec3 p = cam + ray * (scene_d / max(ray_d, 1e-4));
    // The surface's slope from the neighbouring pixels, before any pixel of the quad is discarded.
    vec3 n = cross(dFdx(p), dFdy(p));
    float level = abs(n.z) / max(length(n), 1e-6);

    // The far faces only: each pixel once.
    if (dot(v_normal, ray) < 0.0 || ray_d <= 1e-4) discard;

    float range = v_shape.w;
    vec3 at;
    float shown = 1.0;
    // How far below to look for air: under a wolf's belly, not through the ground; a paw's top that close to the
    // ground still takes the ring.
    float drop = clamp(0.12 * v_feet.w, 0.3, 1.0);
    if (has_scene && abs(p.z - v_feet.z) < range && level > 0.5 && !overhangs(p, drop)) {
        at = p; // on the ground
    } else {
        if (abs(ray.z) < 1e-5) discard;
        float k = (v_feet.z - cam.z) / ray.z;
        if (k <= 0.0) discard;
        at = cam + ray * k;
        if (k * ray_d > scene_d + 0.05) shown = v_cues.z; // behind the scene
    }
    if (shown <= 0.0) discard;

    vec2 cover = ring_at(at.xy - v_feet.xy);
    float alpha = clamp(cover.y, 0.0, 1.0) * v_color.a * shown;
    if (alpha < 1e-3) discard;
    // The core past 1 by the glow so the bloom finds it; the rest the plain colour.
    vec3 light = to_linear(v_color.rgb) * (1.0 + v_cues.w * clamp(cover.x, 0.0, 1.5));
    // The haze between it and the eye (as the spell bolts).
    if (pc.haze.z > 0.5) {
        vec3 dv = at - cam;
        float dist = length(dv) * pc.haze.x;
        alpha *= exp(-3.5 * dist * dist * exp(-max(dv.z, 0.0) * pc.haze.y));
    }
    o_color = vec4(light * alpha, alpha);
    o_reactive = reactive_of(o_color);
}
