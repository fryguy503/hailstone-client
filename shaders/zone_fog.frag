#version 450

// The fog bank past the zone's edge (render/zone_fog.h): each pixel's line of sight marched from the camera to what
// it shows (kZoneFogReach for the sky) through the fog's field; the haze's colour mixed over the scene by what is
// left of the light (blended: rgb the fog's light, a what shows through).

layout(set = 0, binding = 0) uniform sampler2D depth_map; // the scene's depth (sample 0), reverse Z
layout(set = 0, binding = 1) uniform sampler2D field;     // render::ZoneFogField: r distance past, g the floor

layout(push_constant) uniform Push {
    vec4 right;   // xyz the camera's right x tan(half fov x); w: view depth = up.w / (d + right.w)
    vec4 up;      // xyz its up x tan(half fov y); w: see right
    vec4 forward; // xyz; w the sky's march
    vec4 camera;  // xyz; w the fog's lowest floor
    vec4 color;   // rgb the haze's colour (linear); w the floors' range
    vec4 glow;    // rgb the sun's light in the haze (light x glow x 0.25); w the field's distance past the zone at the camera
    vec4 to_sun;  // xyz; w the time (seconds), which the mist drifts with
    vec4 grid;    // xy the field's corner, zw 1 / its size (world units)
} pc;

layout(location = 0) out vec4 o_color;

// render/zone_fog.h
const float kRamp = 1400.0;
const float kTop = 250.0;
const float kSoft = 100.0;
const float kSwell = 100.0;
const float kClear = 300.0;
const float kClearFade = 900.0;
const float kDensity = 0.0025;
const float kLift = 1.35;
const float kDistanceStep = 16.0;
const int kSteps = 40;

// Value noise, 0..1, smooth between the lattice's points.
// Dave Hoskins's hash without sine (fast, the same on every GPU).
float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float density(vec3 p) {
    vec2 uv = (p.xy - pc.grid.xy) * pc.grid.zw;
    vec2 f = textureLod(field, uv, 0.0).rg;
    vec2 outside = max(-uv, 0.0) + max(uv - 1.0, 0.0);
    // Pushed out by the camera's own distance past the zone: the camera is in the zone, whatever the field says.
    float d = f.r * 255.0 * kDistanceStep + length(outside / pc.grid.zw) - pc.glow.w;
    if (d <= 0.0) return 0.0; // the zone's own ground: clear, and nothing more to work out
    float ramp = smoothstep(0.0, kRamp, d);
    float floor_z = pc.camera.w + f.g * pc.color.w;
    floor_z = mix(floor_z, min(floor_z, pc.camera.z - 6.0), smoothstep(0.0, 64.0, pc.glow.w));
    float over = p.z - floor_z;
    // The bank's mean (render::zone_fog_density) broken up: its top rises and falls by up to kSwell over broad
    // swells, and its thickness varies in patches, drifting slowly.
    vec2 q = p.xy + vec2(pc.to_sun.w * 6.0, pc.to_sun.w * 2.5);
    float top = kTop + (noise(q / 900.0) - 0.5) * 2.0 * kSwell;
    if (over > top + kSoft) return 0.0;
    float patches = 0.45 + 1.1 * noise(q / 360.0 + 7.3);
    float near = smoothstep(kClear, kClearFade, distance(p, pc.camera.xyz));
    return kDensity * ramp * patches * near * (1.0 - smoothstep(top - kSoft, top + kSoft, over));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 size = vec2(textureSize(depth_map, 0));
    float depth = texelFetch(depth_map, p, 0).r;
    vec2 ndc = (vec2(p) + 0.5) / size * 2.0 - 1.0;
    // The viewport is flipped: row 0 is the top of the view.
    vec3 ray = pc.forward.xyz + ndc.x * pc.right.xyz - ndc.y * pc.up.xyz; // per unit of view depth
    float length_per_depth = length(ray);
    vec3 dir = ray / length_per_depth;
    float reach = depth <= 0.0 ? pc.forward.w : min(pc.up.w / (depth + pc.right.w) * length_per_depth, pc.forward.w);

    // Steps closer together near the camera (t = reach x s^2), each moved by a little ordered dither.
    const float bayer[16] = float[](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    float jitter = (bayer[(p.x & 3) + (p.y & 3) * 4] + 0.5) / 16.0;
    float tau = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        float s0 = float(i) / float(kSteps), s1 = float(i + 1) / float(kSteps);
        float sm = (float(i) + jitter) / float(kSteps);
        tau += density(pc.camera.xyz + dir * (reach * sm * sm)) * reach * (s1 * s1 - s0 * s0);
        if (tau > 7.0) break; // under 0.1 % shows through: the rest makes no difference
    }
    float t = exp(-tau);
    float sun = pow(max(dot(dir, pc.to_sun.xyz), 0.0), 6.0);
    o_color = vec4((pc.color.rgb * kLift + pc.glow.rgb * sun) * (1.0 - t), t);
}
