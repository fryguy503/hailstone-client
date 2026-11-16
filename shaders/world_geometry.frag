#version 450
#extension GL_GOOGLE_include_directive : require
#include "reactive.glsl"

layout(location = 0) in vec4 v_color; // linear light, straight alpha
layout(location = 0) out vec4 o_color;
layout(location = 2) out float o_reactive;

void main() {
    o_color = vec4(v_color.rgb * v_color.a, v_color.a);
    o_reactive = reactive_of(o_color);
}
