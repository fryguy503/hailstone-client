#version 450
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"

// The distant hills round an outdoor zone (render/horizon.h, ZoneRenderer::draw_horizon; Hailstone's own, the modern
// lighting): three rings of land past the zone's edges, behind everything (drawn after the sky, before the zone, so
// anything nearer covers them). Each pixel's view ray meets each ring's upright cylinder about the zone's centre; the
// land there stands as high as a noise along the ring says, periodic round it (sampled on a circle), so the hills are
// the zone's own every visit and move with parallax as the camera does. Lit by the frame's sun and sky as the zone is,
// then fogged by its haze, with the distance held under a share of the haze's length so they stay in sight.
// Vertex shader: underwater.vert (the view direction at each corner).

layout(set = 1, binding = 0) uniform Scene {
    mat4 view_proj;
    vec4 fog_color;
    vec4 fog;
    vec4 camera_pos;
    SCENE_LIGHTING_FIELDS
} scene;

layout(push_constant) uniform Push {
    vec4 ring;   // xy the centre, z the first ring's radius, w the ground's height (the hills' foot)
    vec4 shape;  // x the zone's relief, y its seed, z the strength (render::horizon_strength)
    vec4 ground_color; // linear
    vec4 slope_color;  // linear
} pc;

layout(location = 0) in vec3 v_dir;
layout(location = 0) out vec4 o_color;

const int kLayers = 3;
// Per ring, nearest first: how far past the first ring it stands; its hills' length along it (one noise cell); the
// angle its tallest hills rise to over the foot, seen from the centre (degrees), and the share of the zone's relief
// added; how steeply it rises away from the zone (for its light); how much of it is bare rock towards the top; and
// the share of the haze's length its distance is held under.
const float kOffset[kLayers] = float[](0.0, 1400.0, 3600.0);
const float kLength[kLayers] = float[](900.0, 1500.0, 2600.0);
const float kAngle[kLayers] = float[](2.5, 4.0, 6.5);
const float kRelief[kLayers] = float[](0.6, 0.9, 1.2);
const float kRise[kLayers] = float[](0.35, 0.6, 0.9);
const float kRock[kLayers] = float[](0.0, 0.25, 0.55);
const float kHaze[kLayers] = float[](0.3, 0.42, 0.55);
// Per ring: how much of its own colour is left after the distance's fade (it goes towards the haze's colour, the
// far ring to a pale outline) and how much colour it keeps; and how high its fog banks reach, as a share of its hills
// at the least and the most (they rise and fall along the ring and drift).
const float kKeep[kLayers] = float[](0.7, 0.5, 0.32);
const float kSaturation[kLayers] = float[](0.75, 0.6, 0.45);
const float kMistLow[kLayers] = float[](0.15, 0.25, 0.35);
const float kMistHigh[kLayers] = float[](0.5, 0.6, 0.7);
// The fog's drift: round a circle once every 1024 s (the scene clock wraps there, so it never jumps), this far
// across, about 4 units a second.
const float kDrift = 650.0;

float hash2(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash2(i), b = hash2(i + vec2(1.0, 0.0)), c = hash2(i + vec2(0.0, 1.0)), d = hash2(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// The land's height over the hills' foot at `rel` (from the zone's centre), 0..1: a noise over the ground, one cell
// kLength across, so a ring is a section through it and the slopes either way come from the same land. Rolling hills
// near, ridged mountains far.
float ridge(int layer, vec2 rel) {
    vec2 q = rel / kLength[layer] + vec2(pc.shape.y + 17.0 * float(layer), pc.shape.y * 0.37);
    float h = 0.0, amp = 0.5, total = 0.0;
    for (int o = 0; o < 5; ++o) {
        float n = value_noise(q);
        if (layer == 2) n = 1.0 - abs(n * 2.0 - 1.0); // ridged
        h += n * amp;
        total += amp;
        amp *= 0.5;
        q = mat2(1.6, 1.2, -1.2, 1.6) * q + vec2(5.2, 1.3);
    }
    h /= total;
    h = layer == 2 ? h * h : smoothstep(0.15, 0.85, h);
    return mix(0.15, 1.0, h);
}

float amplitude(int layer, float radius) {
    float rugged = mix(0.7, 1.8, clamp(pc.shape.x / 900.0, 0.0, 1.0));
    return radius * tan(radians(kAngle[layer])) * rugged + pc.shape.x * kRelief[layer];
}

// How much of ring `layer` at `rel`, `height` (0 its foot, 1 its tallest hills) up, the fog banks hide, 0..1: their
// top rises and falls along the ring and drifts with the scene's time; soft above it, thick below.
float mist(int layer, vec2 rel, float height) {
    float w = scene.time.x * (6.2831853 / 1024.0);
    vec2 drift = kDrift * vec2(cos(w), sin(w));
    vec2 q = (rel + drift) / 1100.0 + pc.shape.yy * 0.5 + vec2(3.1 * float(layer));
    float n = value_noise(q) * 0.65 + value_noise(q * 2.7 + vec2(1.7, 9.2)) * 0.35;
    float top = mix(kMistLow[layer], kMistHigh[layer], n);
    return 1.0 - smoothstep(top - 0.25, top + 0.08, height);
}

void main() {
    vec3 dir = normalize(v_dir);
    vec3 cam = scene.camera.xyz;
    vec2 o = cam.xy - pc.ring.xy;
    float a = dot(dir.xy, dir.xy);

    // Where the ray meets each ring (the far side of its cylinder), and its height there; worked out for every pixel
    // first, so the derivatives for the edges are taken with every pixel of the quad still running.
    vec3 hit[kLayers];
    float pz[kLayers], wz[kLayers], radius[kLayers];
    bool any = false;
    for (int i = 0; i < kLayers; ++i) {
        radius[i] = pc.ring.z + kOffset[i];
        float b = dot(o, dir.xy);
        float c = dot(o, o) - radius[i] * radius[i];
        float disc = b * b - a * c;
        float t = (a > 1e-6 && disc >= 0.0) ? (-b + sqrt(disc)) / a : -1.0;
        hit[i] = cam + dir * max(t, 0.0);
        pz[i] = t > 0.0 ? hit[i].z : 1e9;
        wz[i] = max(fwidth(pz[i]), 1e-3);
        any = any || pz[i] < pc.ring.w + amplitude(i, radius[i]);
    }
    if (!any) discard; // over every ring's tallest hills: the sky

    float hold = 1.0 / max(scene.haze.x, 1e-6); // the haze's length
    vec3 haze_only = apply_haze(vec3(0.0), cam + dir * (3.0 * hold), scene.camera, scene.haze, scene.haze_color,
                                scene.to_light, scene.light);
    vec3 acc = vec3(0.0);
    float alpha = 0.0;
    // Looking down short of the first ring: the plain before the hills, at their foot, nearer and so clearer.
    float plain = dir.z < -1e-4 ? (pc.ring.w - cam.z) / dir.z : -1.0;
    if (plain > 0.0 && (pz[0] > 1e8 || plain < length(hit[0] - cam))) {
        // A sea of fog, only a hint of the ground showing through it where it thins.
        vec3 p = cam + dir * plain;
        vec2 rel = p.xy - pc.ring.xy;
        vec3 ground = pc.ground_color.rgb * (scene.sky.rgb + scene.light.rgb * max(scene.to_light.z, 0.0));
        ground = apply_haze(ground, cam + dir * min(plain, kHaze[0] * hold), scene.camera, scene.haze,
                            scene.haze_color, scene.to_light, scene.light);
        float thin = mist(0, rel, -0.1);
        float w = scene.time.x * (6.2831853 / 1024.0);
        float gaps = value_noise((rel + kDrift * vec2(cos(w), sin(w))) / 700.0 + pc.shape.yy);
        float fog = mix(0.75, 0.97, smoothstep(0.3, 0.7, gaps)) * thin;
        o_color = vec4(mix(ground, haze_only * 1.04, fog), clamp(pc.shape.z, 0.0, 1.0));
        return;
    }
    for (int i = 0; i < kLayers; ++i) {
        if (pz[i] > 1e8) continue;
        float r = radius[i];
        vec2 rel = hit[i].xy - pc.ring.xy;
        float amp = amplitude(i, r);
        float top = pc.ring.w + amp * ridge(i, rel);
        float cover = clamp((top - pz[i]) / wz[i] + 0.5, 0.0, 1.0);
        if (cover <= 0.0) continue;

        // The land's slope, for the light: its hills turn to and from the sun; and the ring rises away from the zone.
        const float e = 24.0;
        vec2 grad = amp * vec2(ridge(i, rel + vec2(e, 0.0)) - ridge(i, rel - vec2(e, 0.0)),
                               ridge(i, rel + vec2(0.0, e)) - ridge(i, rel - vec2(0.0, e))) / (2.0 * e);
        vec2 out_dir = rel / max(length(rel), 1e-3);
        vec3 n = normalize(vec3(-(grad + kRise[i] * out_dir), 1.0));

        float height = clamp((pz[i] - pc.ring.w) / max(amp, 1.0), 0.0, 1.0);
        vec3 albedo = mix(pc.ground_color.rgb, pc.slope_color.rgb, smoothstep(0.3, 0.6, 1.0 - n.z));
        albedo = mix(albedo, pc.slope_color.rgb, kRock[i] * smoothstep(0.4, 0.95, height));
        // Patches (woods and clearings, scree), fixed to the land, and the valleys' shade.
        float ang = atan(rel.y, rel.x);
        albedo *= 0.75 + 0.5 * value_noise(vec2(ang * r / 150.0, pz[i] / 60.0) + pc.shape.yy);
        albedo *= mix(0.65, 1.0, smoothstep(0.0, 0.8, height));
        vec3 color = albedo * (hemisphere(n, scene.sky.rgb, scene.ground.rgb) +
                               scene.light.rgb * max(dot(n, scene.to_light.xyz), 0.0));

        // Distant land loses its colour and goes towards the sky's: the haze over the held distance, then the
        // ring's own fade, the far ones most.
        float grey = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = mix(vec3(grey), color, kSaturation[i]);
        float dist = length(hit[i] - cam);
        color = apply_haze(color, cam + dir * min(dist, kHaze[i] * hold), scene.camera, scene.haze, scene.haze_color,
                           scene.to_light, scene.light);
        color = mix(haze_only, color, kKeep[i]);
        // The fog banks round their feet, a little lit by the sun.
        vec3 fog_color = haze_only * 1.04 + scene.light.rgb * 0.01;
        color = mix(color, fog_color, 0.92 * mist(i, rel, height));

        acc += (1.0 - alpha) * cover * color;
        alpha += (1.0 - alpha) * cover;
    }
    if (alpha <= 0.0) discard;
    o_color = vec4(acc / alpha, alpha * clamp(pc.shape.z, 0.0, 1.0));
}
