#version 450

layout(set = 0, binding = 0) uniform sampler2D diffuse_texture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(push_constant) uniform Material {
    layout(offset = 64) float alpha_cutoff;
} material;

void main() {
    vec4 sampled = texture(diffuse_texture, uv);
    if (sampled.a < material.alpha_cutoff) discard;
    color = vec4(sampled.rgb, 1.0);
}
