#version 450
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"

// Zone geometry. Per-frame constants in the Scene block (set 1), per-draw
// ones in push constants; see render/zone_renderer.cpp and docs/graphics.md.

layout(set = 1, binding = 0) uniform Scene {
    mat4 view_proj;
    vec4 fog_color;
    vec4 fog;        // start, end, a_fTime-based scroll k (seconds), density
    vec4 camera_pos;
    SCENE_LIGHTING_FIELDS
} scene;

layout(push_constant) uniform Push {
    uvec4 tex;    // texture slots 0..3
    uvec4 tex2;   // slot 4, mode, flags, unused
    vec4 params;  // alpha_ref, opacity, coverage scale, unused
    vec4 slide;   // e_fSlide1X, 1Y, 2X, 2Y
    vec4 extra;   // mode-specific (detail scales + count; water Fresnel bias, power, UV scale)
    vec4 extra2;  // mode-specific (water colour 1)
    vec4 extra3;  // mode-specific (water colour 2)
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in vec2 in_uv2;
layout(location = 5) in vec4 in_color2;
layout(location = 6) in vec3 in_point_light;
layout(location = 7) in vec4 in_sway; // the wind's hold on a plant's vertex (lighting.glsl wind_offset)
// Per instance (lighting.glsl instance_point): an instanced model's placement, the identity for the zone's own.
layout(location = 8) in vec4 in_row0;
layout(location = 9) in vec4 in_row1;
layout(location = 10) in vec4 in_row2;
layout(location = 11) in vec4 in_normal_row0; // w: the sway's scale
layout(location = 12) in vec4 in_normal_row1;
layout(location = 13) in vec4 in_normal_row2;
layout(location = 14) in vec4 in_tint; // times the vertex colour: the placement's baked light

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_light;
layout(location = 2) out vec3 v_world;
layout(location = 3) out vec2 v_uv2;
layout(location = 4) out vec4 v_color;
layout(location = 5) out vec4 v_color2;
layout(location = 6) out vec3 v_normal;
layout(location = 7) out float v_fog;
layout(location = 8) out vec3 v_point; // the MPL point light term (in v_light; zone.frag's modern light)
layout(location = 9) out vec3 v_rest;  // the position without the wind: the lights' shadow cubes are drawn so
layout(location = 10) flat out float v_lod_distance; // an instanced model's placement's from the camera (its fades)

// The depth pre-pass (zone_depth.frag) and the colour pass are two pipelines over this shader: the position must come
// out bit for bit the same in both, or the colour pass's equal depth test drops pixels.
invariant gl_Position;

// The effects' fog (RegionC1 vs_1_1): per
// vertex, f = clamp((|eye - pos| - FogStart) FogRangeInv fFogRange, 0, fFogRange)
// with FogRangeInv = 1 / (end - start), fFogRange = 10, and
// oFog = exp(-(f FogDensity)^2) (1: no fog); the pixel is lerp(fog, colour, oFog).
float fog_factor(vec3 p) {
    float dist = length(scene.camera_pos.xyz - p);
    float range_inv = 1.0 / max(scene.fog.y - scene.fog.x, 1.0);
    float f = clamp((dist - scene.fog.x) * range_inv * 10.0, 0.0, 10.0);
    float x = f * scene.fog.w;
    return exp(-(x * x));
}

const uint kFlagMultiPass = 4u; // add the lights.wld point lights (MPL path)
const uint kModeWldDetail = 6u;  // a classic region's detail pass (zone.frag)

void main() {
    vec3 position = instance_point(in_position, in_row0, in_row1, in_row2);
    vec3 n = instance_normal(in_normal, in_normal_row0, in_normal_row1, in_normal_row2);
    vec4 color = in_color * in_tint;
    v_uv = in_uv;
    v_uv2 = in_uv2;
    v_color = color;
    v_color2 = in_color2;
    v_normal = n;
    // RenderEffects/SPL/RegionC1 and RegionOldA (vs_1_1), per vertex:
    // rgb = C.rgb + C.a Ambient + SpecialAmbient
    //     + C.a (Bounce max(N.Dn, 0) + Directional max(N.-Dn, 0)), saturated.
    // SPL/RegionOldDetailSingle and RegionOldDetailPalette (vs_1_1, a detail pass) add the same terms without the
    // alpha's weight: rgb = C.rgb + Ambient + SpecialAmbient + Bounce max(N.Dn, 0) + Directional max(N.-Dn, 0).
    vec3 dn = scene.direction.xyz;
    float a = pc.tex2.y == kModeWldDetail ? 1.0 : color.a;
    vec3 rgb = color.rgb + a * scene.ambient.rgb + scene.special_ambient.rgb +
               a * (scene.bounce.rgb * max(dot(n, dn), 0.0) + scene.directional.rgb * max(dot(n, -dn), 0.0));
    rgb = clamp(rgb, 0.0, 1.0);
    // The multi-pass path adds one pass per point light; the per-vertex sum of
    // the saturated light terms is precomputed (lights are static).
    v_point = (pc.tex2.z & kFlagMultiPass) != 0u ? in_point_light : vec3(0.0);
    rgb += v_point;
    v_light = vec4(rgb, 1.0);
    vec4 sway = instance_sway(in_sway, in_row0, in_row1, in_normal_row0.w);
    vec3 p = position + wind_offset(position, sway, scene.wind, scene.time.x);
    if (pc.tex2.y == kModeWldDetail) {
        // The batch's TEXTUREFACTOR alpha (the region draw, 0x10018e2e): d = how far the camera is outside the
        // batch's box along x plus along y (box: in_uv2 min x, min y; in_point_light max x, max y); drawn while
        // d < min(600, FogEnd x 1.414), alpha min(160, round((1 - d / 600) x 255)); the pass's alpha is it x the
        // vertex alpha.
        vec2 c = scene.camera_pos.xy;
        vec2 lo = in_uv2, hi = in_point_light.xy;
        float d = max(lo.x - c.x, 0.0) + max(c.x - hi.x, 0.0) + max(lo.y - c.y, 0.0) + max(c.y - hi.y, 0.0);
        float factor = d < min(600.0, scene.fog.y * 1.414) ? min(160.0, roundEven((1.0 - d * 0.0016666667) * 255.0))
                                                           : 0.0;
        v_color2.a = in_color2.a * factor / 255.0;
    }
    v_world = p;
    v_rest = position;
    v_lod_distance = distance(scene.camera_pos.xyz, vec3(in_row0.w, in_row1.w, in_row2.w));
    v_fog = fog_factor(p);
    gl_Position = scene.view_proj * vec4(p, 1.0);
}
