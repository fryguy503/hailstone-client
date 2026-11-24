#version 450

// A name plate's text, hidden wherever the scene is nearer than the plate: each pixel is tested against the scene's
// own depth (the one the AO reads), in the frame it is drawn (app/nameplate_layer.h). Where it is hidden it is faded to
// the occluded alpha, or dropped when that is 0.

layout(location = 0) in struct { vec4 color; vec2 uv; } inv;
layout(location = 0) out vec4 fColor;

layout(set = 0, binding = 0) uniform texture2D sTexture; // ImGui's, bound by its draw commands (the image and,
layout(set = 1, binding = 0) uniform sampler sSampler;   // in set 1, the sampler: split since ImGui 1.92.8)
layout(set = 2, binding = 0) uniform sampler2D u_depth;  // the scene's depth, reverse Z
layout(set = 2, binding = 1) uniform Plate {
    vec4 view;  // the 3D view on the framebuffer, pixels: x, y, width, height
    vec4 depth; // view depth = y / (d + x) (x, y); the plate's view depth; the margin it may lie behind a surface
    vec4 extra; // x: the alpha of text behind the scene (0: not drawn)
} u;

void main() {
    vec2 size = vec2(textureSize(u_depth, 0));
    ivec2 px = clamp(ivec2((gl_FragCoord.xy - u.view.xy) / u.view.zw * size), ivec2(0), ivec2(size) - 1);
    float d = texelFetch(u_depth, px, 0).r;
    // d = 0 is the far plane: nothing was drawn there.
    float scene = d > 0.0 ? u.depth.y / (d + u.depth.x) : 1e30;
    float alpha = 1.0;
    if (u.depth.z > scene + u.depth.w) {
        if (u.extra.x <= 0.0) discard;
        alpha = u.extra.x;
    }
    fColor = inv.color * texture(sampler2D(sTexture, sSampler), inv.uv.st);
    fColor.a *= alpha;
}
