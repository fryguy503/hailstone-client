#include "render/gpu_fallbacks.h"

namespace hailstone::render {

GpuFallbackParse parse_gpu_fallbacks(std::string_view text) {
    GpuFallbackParse out;
    GpuFallbacks& f = out.forced;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find_first_of(", \t", at);
        const std::string_view word = text.substr(at, end == std::string_view::npos ? text.size() - at : end - at);
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (word.empty()) continue;
        if (word == "depth-stencil") f.depth_stencil = true;
        else if (word == "depth-d24") f.depth_d24 = true;
        else if (word == "non-coherent") f.non_coherent = true;
        else if (word == "shadow-d16") f.shadow_d16 = true;
        else if (word == "no-bc") f.no_bc = true;
        else if (word == "no-mip-blit") f.no_mip_blit = true;
        else if (word == "no-update-after-bind") f.no_update_after_bind = true;
        else if (word == "no-independent-blend") f.no_independent_blend = true;
        else if (word == "all") {
            f.depth_stencil = f.non_coherent = f.shadow_d16 = f.no_bc = f.no_mip_blit = f.no_update_after_bind =
                f.no_independent_blend = true;
        } else {
            out.unknown.emplace_back(word);
        }
    }
    return out;
}

std::string gpu_fallback_names(const GpuFallbacks& f) {
    std::string out;
    auto add = [&out](bool on, const char* name) {
        if (!on) return;
        if (!out.empty()) out += ", ";
        out += name;
    };
    add(f.depth_stencil, "depth-stencil");
    add(f.depth_d24, "depth-d24");
    add(f.non_coherent, "non-coherent");
    add(f.shadow_d16, "shadow-d16");
    add(f.no_bc, "no-bc");
    add(f.no_mip_blit, "no-mip-blit");
    add(f.no_update_after_bind, "no-update-after-bind");
    add(f.no_independent_blend, "no-independent-blend");
    return out;
}

std::string_view depth_candidate_name(DepthCandidate format) {
    switch (format) {
    case DepthCandidate::D32: return "D32_SFLOAT";
    case DepthCandidate::D32S8: return "D32_SFLOAT_S8_UINT";
    case DepthCandidate::D24S8: return "D24_UNORM_S8_UINT";
    }
    return "?";
}

DepthChoice choose_depth_format(const std::array<bool, 3>& usable, const GpuFallbacks& forced) {
    auto first_from = [&usable](std::size_t start) -> std::optional<DepthCandidate> {
        for (std::size_t i = start; i < kDepthCandidates.size(); ++i)
            if (usable[i]) return kDepthCandidates[i];
        return std::nullopt;
    };
    DepthChoice choice;
    choice.format = first_from(0);
    if (!forced.depth_d24 && !forced.depth_stencil) return choice;
    // depth-d24 is the narrower request; where it cannot be had, depth-stencil (when also given) is tried.
    if (forced.depth_d24 && usable[2]) {
        choice.format = DepthCandidate::D24S8;
        return choice;
    }
    std::string note;
    if (forced.depth_d24) note = "depth-d24 cannot be forced: this GPU has no attachable, sampleable D24_UNORM_S8_UINT";
    if (forced.depth_stencil) {
        if (const std::optional<DepthCandidate> fallback = first_from(1)) {
            choice.format = fallback;
        } else {
            if (!note.empty()) note += "; ";
            note += "depth-stencil cannot be forced: this GPU has no attachable, sampleable depth format with stencil";
        }
    }
    if (!note.empty())
        choice.note = note + "; using " + (choice.format ? std::string(depth_candidate_name(*choice.format)) : "none");
    return choice;
}

} // namespace hailstone::render
