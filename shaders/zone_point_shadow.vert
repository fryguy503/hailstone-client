#version 450
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"

// Zone geometry into one face of a light's shadow cube (render/zone_renderer.cpp draw_point_shadow): the light and
// its radius in slide, the face in tex2.w, kPointShadowFaceScale and kPointShadowNear in extra.xy. The plants stand
// still: the cube is drawn once and kept.

layout(push_constant) uniform Push {
    uvec4 tex;
    uvec4 tex2; // slot 4, mode, flags, face
    vec4 params; // alpha_ref, ...
    vec4 slide;  // the light, its radius
    vec4 extra;  // the face scale, the near distance
    vec4 extra2;
    vec4 extra3;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;
layout(location = 8) in vec4 in_row0; // an instanced model's placement (zone.vert)
layout(location = 9) in vec4 in_row1;
layout(location = 10) in vec4 in_row2;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_from_light;

void main() {
    v_uv = in_uv;
    v_from_light = instance_point(in_position, in_row0, in_row1, in_row2) - pc.slide.xyz;
    gl_Position = point_shadow_clip(v_from_light, pc.tex2.w, pc.extra.x, pc.slide.w);
}
