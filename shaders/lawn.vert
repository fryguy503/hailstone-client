#version 450
#extension GL_GOOGLE_include_directive : require

// Hailstone's lawn (render/flora_renderer.h, assets/zone_flora.h): a patch per instance, its blades built here, no
// texture. A patch's blades come in a fixed order, placed by a low-discrepancy sequence (R2) shifted per patch, so
// any first few of them cover the square evenly: the draw (the push block's lod) takes the first lod.x, and with
// distance the last of them shrink away one by one while the rest widen (lawn_blades), so no draw's switch shows.
// Each blade is a strip of `segments` pieces, the last a point, narrow at the root, widest a third of the way up,
// standing on the ground (the patch's slope), curving over as it rises (combed the way its neighbours lean), leaning
// with the wind as gusts roll across the zone and bent away from whoever walks through it. Its height wanders a
// little round the patch's (the patch's own wanders slowly over the zone, assets::lawn_height_noise). Modern: the
// light that casts no shadow here per vertex, as flora.vert; the sun in lawn.frag.

#include "lighting.glsl"

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 time; // x seconds
    vec4 lod;    // x this draw's blades, y segments, z kLawnCell, w kLawnBladeWidth
    vec4 curve;  // x kLawnFull, y kLawnNear, z kLawnMid
    vec4 counts; // xyz kLawnLods' blades
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

layout(location = 0) in vec4 i_position_turn; // xyz the patch's centre on the ground
layout(location = 1) in vec4 i_size;          // x the square's side, y the blades' height, zw the slope (dz/dx, dz/dy)
layout(location = 2) in uint i_color;         // the ground's colour (RGBA8)
layout(location = 3) in uint i_flags;         // unused

layout(location = 0) out vec3 v_light; // modern: the shadowless light; classic: the light
layout(location = 1) out vec3 v_world;
layout(location = 2) out vec3 v_normal; // the blade's face, towards the camera
layout(location = 3) flat out vec3 v_ground; // the ground's colour (display values)
layout(location = 4) out float v_up;    // 0 at the root, 1 at the tip
layout(location = 5) flat out float v_shade; // the blade's own shade, about 1

uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float next(inout uint s) {
    s = hash(s + 0x9e3779b9u);
    return float(s & 0xffffu) / 65535.0;
}

// render/flora_renderer.cpp lawn_blades, line for line: how many of a patch's blades stand at distance `d`.
float lawn_blades(float d) {
    if (!(d > pc.curve.x)) return pc.counts.x;
    if (d <= pc.curve.y) return mix(pc.counts.x, pc.counts.y, smoothstep(pc.curve.x, pc.curve.y, d));
    if (d <= pc.curve.z) return mix(pc.counts.y, pc.counts.z, smoothstep(pc.curve.y, pc.curve.z, d));
    return pc.counts.z;
}

void main() {
    const int segments = max(int(pc.lod.y + 0.5), 1);
    const int per_blade = (segments - 1) * 6 + 3;
    const int blade = gl_VertexIndex / per_blade;
    const int v = gl_VertexIndex - blade * per_blade;

    // Which level up the blade and which edge: the pieces' two triangles, then the point.
    int level;
    float edge;
    if (v < (segments - 1) * 6) {
        int piece = v / 6, k = v - piece * 6;
        const int up[6] = int[6](0, 0, 1, 0, 1, 1);
        const float sides[6] = float[6](-1.0, 1.0, 1.0, -1.0, 1.0, -1.0);
        level = piece + up[k];
        edge = sides[k];
    } else {
        int k = v - (segments - 1) * 6;
        level = k == 2 ? segments : segments - 1;
        edge = k == 0 ? -1.0 : k == 1 ? 1.0 : 0.0;
    }
    float t = float(level) / float(segments);

    // The blade's own draws, from where the patch stands (the same blade every frame, in every draw) and its number.
    vec3 centre = i_position_turn.xyz;
    uvec2 cell = uvec2(ivec2(floor(centre.xy * 4.0)));
    uint patch_seed = hash(cell.x * 73856093u ^ cell.y * 19349663u);
    uint s = hash(patch_seed ^ uint(blade) * 83492791u);
    vec2 shift = vec2(float(patch_seed & 0xffffu), float(patch_seed >> 16)) / 65535.0;
    vec2 at = (fract(shift + float(blade) * vec2(0.7548776662, 0.5698402910)) - 0.5) * i_size.x;
    vec3 root = centre + vec3(at, dot(at, i_size.zw));
    // How much of it stands: the blades past lawn_blades(distance) shrink away over three places in the order.
    float shown = lawn_blades(distance(root, lighting.camera.xyz));
    float grown = clamp((shown - float(blade)) / 3.0, 0.0, 1.0);
    float height = i_size.y * (0.85 + 0.25 * next(s)) * grown;
    // Combed: the blades lean the way a slowly turning field over the zone points (broad swathes of grass lying
    // together), each a little off it, and turn their flat side to the way they lean.
    vec2 g = root.xy * 0.011;
    float comb = sin(g.x + sin(g.y * 1.7) * 1.3) * 1.6 + sin(g.y * 0.8 - g.x * 0.6 + 2.0) * 1.2;
    float lean_turn = comb + (next(s) - 0.5) * 0.9;
    float facing = lean_turn + 1.5707963 + (next(s) - 0.5) * 0.8;
    float lean = height * (0.4 + 0.25 * next(s));
    // Wider as fewer stand (render::lawn_blade_width), and as the patch covers more squares far off.
    float width = pc.lod.w * sqrt(pc.counts.x / max(shown, 1.0)) * (i_size.x / pc.lod.z) * (0.8 + 0.4 * next(s));
    if (grown <= 0.0) width = 0.0;
    v_shade = 0.94 + 0.12 * next(s);
    float phase = next(s) * 6.2831853;

    // Where the tip goes: its own lean, the wind (a steady push, gusts rolling across the zone in the wind's direction
    // and a flutter of its own), and away from a pusher's feet.
    vec2 bend = vec2(cos(lean_turn), sin(lean_turn)) * lean;
    float sec = pc.time.x;
    vec4 wind = lighting.wind;
    float roll = dot(root.xy, wind.xy) * 0.02;
    float gust = 0.5 + 0.5 * sin(sec * 1.1 - roll) * sin(sec * 0.43 - roll * 0.7 + 1.3);
    float flutter = sin(sec * 2.7 + phase) * 0.5 + sin(sec * 4.1 + phase * 1.7) * 0.25;
    float strength = wind.z * (0.35 + wind.w * gust);
    bend += wind.xy * (strength * height * 0.6) + vec2(cos(phase), sin(phase)) * (flutter * height * (0.04 + 0.08 * wind.z));
    int n = int(frame.params.x + 0.5);
    float press = 0.0;
    for (int k = 0; k < n; ++k) {
        vec4 q = frame.pushers[k];
        vec2 d = root.xy - q.xy;
        float dist = length(d);
        if (dist >= q.w || abs(root.z - q.z) > 10.0) continue;
        float b = 1.0 - dist / q.w;
        b = b * b * (3.0 - 2.0 * b);
        vec2 away = dist > 1e-3 ? d / dist : vec2(cos(facing), sin(facing));
        bend += away * (b * height * 0.8);
        press = max(press, b);
    }
    // Curving over: the tip's offset grows with the square of the way up. Bent, it dips so the blade keeps its length.
    float reach = length(bend);
    float limit = height * 0.9;
    if (reach > limit) {
        bend *= limit / reach;
        reach = limit;
    }
    float rise = height * (1.0 - 0.5 * (reach * reach) / (height * height)) * (1.0 - 0.25 * press);
    vec3 across = vec3(cos(facing), sin(facing), 0.0);
    // Narrow at the root, widest a third of the way up, to a point.
    float profile = (1.0 - t) * (0.55 + 1.35 * t);
    vec3 p = root + vec3(bend * t * t, rise * t) + across * (edge * width * 0.5 * profile);

    // The face: across the blade and along it (the curve's slope there).
    vec3 along = normalize(vec3(bend * 2.0 * t, max(rise, 0.01)));
    vec3 normal = normalize(cross(across, along));
    if (dot(normal, lighting.camera.xyz - root) < 0.0) normal = -normal;

    v_world = p;
    v_normal = normal;
    v_up = t;
    v_ground = unpackUnorm4x8(i_color).rgb;
    if (frame.classic.w > 0.5) {
        const vec3 up = vec3(0.0, 0.0, 1.0);
        float sky_share;
        vec3 placed = probe_light(lighting.probe, lighting.sky.w, sky_share);
        vec3 light = hemisphere(up, lighting.sky.rgb, lighting.ground.rgb) * sky_share + placed +
                     to_linear(lighting.special_ambient.rgb) + lighting.vision.rgb;
        int count = int(lighting.params.y + 0.5);
        for (int i = 0; i < count; ++i) light += point_light_modern(lighting.lights[i], p, up, 0.5);
        v_light = light;
    } else {
        v_light = frame.classic.rgb;
    }
    gl_Position = pc.view_proj * vec4(p, 1.0);
}
