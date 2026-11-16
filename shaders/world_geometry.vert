#version 450

// Already-positioned, unlit world geometry; no projection onto an inferred floor.
layout(push_constant) uniform Push {
    mat4 view_proj;
} pc;

layout(location = 0) in vec3 i_position;
layout(location = 1) in vec4 i_color; // display sRGB, straight alpha
layout(location = 0) out vec4 v_color;

vec3 to_linear(vec3 color) {
    return mix(color / 12.92, pow((color + 0.055) / 1.055, vec3(2.4)), step(0.04045, color));
}

void main() {
    gl_Position = pc.view_proj * vec4(i_position, 1.0);
    v_color = vec4(to_linear(i_color.rgb), i_color.a);
}
