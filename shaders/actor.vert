#version 450

// Skinned character models: every vertex follows up to four bones. The bone matrices
// already include the instance's position, heading and scale.

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 fog_color; // a: fog density
    vec4 fog; // start, end, alpha_ref, opacity
    vec4 camera_pos; // w: the part's native armor surface-table index (fragment shader)
    uint texture_index;
    uint bone_offset;
    uint tint; // 0xAARRGGBB; high byte 0 = none
    uint texture2; // SkinMeshLuclin's second texture (s_SamplerDiffuse1); 0xffffffff = none
} pc;

layout(std430, set = 0, binding = 0) readonly buffer Bones {
    mat4 bones[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_bones;
layout(location = 4) in vec4 in_weights;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_world;
layout(location = 2) out float v_light;
layout(location = 3) out float v_fog;
layout(location = 4) out vec3 v_normal; // world space (the modern lighting, actor.frag)

// The effects' fog (RegionC1 vs_1_1): per
// vertex, f = clamp((|eye - pos| - FogStart) FogRangeInv fFogRange, 0, fFogRange)
// with FogRangeInv = 1 / (end - start), fFogRange = 10, and
// oFog = exp(-(f FogDensity)^2) (1: no fog); the pixel is lerp(fog, colour, oFog).
float fog_factor(vec3 p) {
    float dist = length(pc.camera_pos.xyz - p);
    float range_inv = 1.0 / max(pc.fog.y - pc.fog.x, 1.0);
    float f = clamp((dist - pc.fog.x) * range_inv * 10.0, 0.0, 10.0);
    float x = f * pc.fog_color.a;
    return exp(-(x * x));
}

void main() {
    // One bone (WLD, weights 1,0,0,0) or up to four (EQG).
    mat4 bone = bones[pc.bone_offset + in_bones.x] * in_weights.x;
    if (in_weights.y > 0.0) bone += bones[pc.bone_offset + in_bones.y] * in_weights.y;
    if (in_weights.z > 0.0) bone += bones[pc.bone_offset + in_bones.z] * in_weights.z;
    if (in_weights.w > 0.0) bone += bones[pc.bone_offset + in_bones.w] * in_weights.w;
    vec4 world = bone * vec4(in_position, 1.0);
    vec3 n = mat3(bone) * in_normal;
    float len = length(n);
    n = len > 1e-6 ? n / len : vec3(0.0, 0.0, 1.0);
    // The classic lighting's shape (actor.frag): a soft key light from above, so a body lit by a flat ambient
    // still reads as round (0.7 .. 1).
    v_light = 0.85 + 0.15 * dot(n, normalize(vec3(0.4, 0.3, 0.85)));
    v_normal = n;
    v_uv = in_uv;
    v_world = world.xyz;
    v_fog = fog_factor(world.xyz);
    gl_Position = pc.view_proj * world;
}
