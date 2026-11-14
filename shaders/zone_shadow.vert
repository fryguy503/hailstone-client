#version 450
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"

// Zone geometry into one layer of the shadow map (render/zone_renderer.cpp draw_shadow), the layer in tex2.w: a cascade,
// its matrix from the Scene block, or the cover map (render/cover_map.h), looking straight down. The plants sway as
// they do in the scene (zone.vert).

layout(set = 1, binding = 0) uniform Scene {
    mat4 view_proj;
    vec4 fog_color;
    vec4 fog;
    vec4 camera_pos;
    SCENE_LIGHTING_FIELDS
} scene;

layout(push_constant) uniform Push {
    uvec4 tex;
    uvec4 tex2; // slot 4, mode, flags, cascade
    vec4 params; // alpha_ref, ...
    vec4 slide;
    vec4 extra;
    vec4 extra2;
    vec4 extra3;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;
layout(location = 7) in vec4 in_sway;
layout(location = 8) in vec4 in_row0; // an instanced model's placement (zone.vert)
layout(location = 9) in vec4 in_row1;
layout(location = 10) in vec4 in_row2;
layout(location = 11) in vec4 in_normal_row0; // w: the sway's scale

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = in_uv;
    vec3 position = instance_point(in_position, in_row0, in_row1, in_row2);
    vec4 sway = instance_sway(in_sway, in_row0, in_row1, in_normal_row0.w);
    vec3 p = position + wind_offset(position, sway, scene.wind, scene.time.x);
    if (pc.tex2.w < 4u) {
        gl_Position = scene.shadow_view_proj[pc.tex2.w] * vec4(p, 1.0);
    } else {
        vec2 uv = (p.xy - scene.cover.xy) * scene.cover.z;
        gl_Position = vec4(uv * 2.0 - 1.0, (scene.cover.w - p.z) * scene.cover_params.x, 1.0);
    }
}
