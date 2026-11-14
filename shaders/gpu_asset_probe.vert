#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec2 uv;

layout(push_constant) uniform Camera {
    mat4 mvp;
} camera;

void main() {
    gl_Position = camera.mvp * vec4(position, 1.0);
    uv = texcoord;
}
