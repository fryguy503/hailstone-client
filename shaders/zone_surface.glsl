#ifndef HAILSTONE_ZONE_SURFACE_GLSL
#define HAILSTONE_ZONE_SURFACE_GLSL

// EQG CBGG1/CBSG1 surface channels (SPL/SModelCBSG1, PS2): RG is the tangent direction, B self-illumination,
// A gloss. The positive tangent Z keeps the glow channel out of the normal, including a dark rock's B=0.
vec3 zone_surface_normal(vec4 texel, bool packed_channels) {
    return packed_channels ? vec3(texel.x * 2.0f - 1.0f, texel.y * 2.0f - 1.0f, 0.9f) : vec3(texel) * 2.0f - 1.0f;
}

float zone_surface_gloss(vec4 texel, bool packed_channels, float fallback) {
    return packed_channels ? clamp(texel.w, 0.0f, 1.0f) : fallback;
}

// Modern light sources live above display white, as additive particles do. Classic keeps the authored intensity.
float zone_emission_gain(bool modern) { return modern ? 2.5f : 1.0f; }

// `albedo` is linear in modern lighting, display RGB in classic. Emission is independent of baked light, sky share
// and shadow visibility; a missing/black glow mask adds nothing.
vec3 zone_surface_glow(vec3 albedo, float mask, bool modern) {
    return albedo * (clamp(mask, 0.0f, 1.0f) * zone_emission_gain(modern));
}

#endif
