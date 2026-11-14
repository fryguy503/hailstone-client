// Alpha-to-coverage for the opaque pass (render/vk_context.h SceneOptions msaa): the pipeline sets constant_id 0
// when the scene is multisampled, and the alpha test then becomes the coverage of the pixel's samples.

layout(constant_id = 0) const bool kAlphaToCoverage = false;
// The scene's samples a pixel (1, 2, 4 or 8; render/zone_renderer.cpp create_pipelines).
layout(constant_id = 1) const int kSceneSamples = 1;

// The samples of a pixel a level of detail covers in its fades (render/lod_bands.h lod_presence), as gl_SampleMask:
// all of them outside its fades, none where it is gone. `band`: its near and far ends as the scene sees them and the
// fade's share of the distance (kLodFade); `dist`: the placement's distance from the camera (zone.vert
// v_lod_distance). Where two levels meet, the one fading out takes the samples below a split and the one fading in
// those from it, the split at the share fading out plus the pixel's noise, so every sample shows exactly one of the
// two; the samples are then turned by the noise, the same in both, so neighbouring pixels share them out unalike.
// With one sample a pixel, it is a dither.
int lod_fade_samples(bool fading, float dist, vec4 band, vec2 frag) {
    if (!fading) return -1;
    float k = band.z;
    float fade_in = band.x > 0.0 ? clamp((dist - band.x * (1.0 - 0.5 * k)) / (k * band.x), 0.0, 1.0) : 1.0;
    float fade_out = clamp((band.y * (1.0 + 0.5 * k) - dist) / (k * band.y), 0.0, 1.0);
    float n = fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715)))); // interleaved gradient noise
    int count = kSceneSamples;
    int keep_below = clamp(int(floor(fade_out * float(count) + n)), 0, count);
    int keep_from = clamp(int(floor((1.0 - fade_in) * float(count) + n)), 0, count);
    int mask = ((1 << keep_below) - 1) & ~((1 << keep_from) - 1);
    int turn = int(fract(n * 7.0) * float(count)) % count;
    int all = (1 << count) - 1;
    return ((mask << turn) | (mask >> (count - turn))) & all;
}

// The alpha test's edge as coverage: a ramp about one pixel wide around the reference, so the edge is smooth under
// MSAA. The ramp is at most 0.05 of alpha wide: on far, minified textures the alpha changes a lot across a pixel, and
// a wider ramp gives texels well under the reference half coverage (dark specks against the sky in gfaydark).
float alpha_coverage(float alpha, float ref) {
    return clamp((alpha - ref) / clamp(fwidth(alpha), 1e-4, 0.05) + 0.5, 0.0, 1.0);
}

// The colour target's alpha after the alpha test (GREATEREQUAL against ref, 0 = off). Returns false to discard.
bool alpha_test(float alpha, float ref, float opacity, out float out_alpha) {
    if (kAlphaToCoverage) {
        out_alpha = ref > 0.0 ? alpha_coverage(alpha, ref) : 1.0;
        return true;
    }
    out_alpha = alpha * opacity;
    return !(ref > 0.0 && alpha < ref);
}
