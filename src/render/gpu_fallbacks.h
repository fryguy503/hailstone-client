#pragma once

// Forced GPU fallbacks: a developer setting (HAILSTONE_GPU_FALLBACKS, docs/graphics.md "Forcing the GPU fallbacks")
// that makes VkContext take the path a weaker GPU takes even where this one offers the preferred one, so those paths
// can be drawn under the validation layers on any machine (tools/nightly.sh). Unset, nothing changes. Kept apart from
// Vulkan so the parsing and the choices are tested; render/vk_context.cpp applies them.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::render {

struct GpuFallbacks {
    // Depth: the first format with stencil (D32_SFLOAT_S8_UINT, then D24_UNORM_S8_UINT) instead of D32_SFLOAT.
    bool depth_stencil = false;
    // Depth: D24_UNORM_S8_UINT alone.
    bool depth_d24 = false;
    // Host-visible memory as non-coherent: VMA is shown every memory type without HOST_COHERENT, so it aligns
    // host-visible allocations to nonCoherentAtomSize and every vmaFlushAllocation calls vkFlushMappedMemoryRanges.
    bool non_coherent = false;
    // Shadow maps in D16_UNORM, as a GPU without linearly filtered D32_SFLOAT has them.
    bool shadow_d16 = false;
    bool no_bc = false;                   // DeviceCaps::bc_textures off: BC textures decoded to RGBA8 on the CPU
    bool no_mip_blit = false;             // DeviceCaps::srgb_mip_blit off: mips built on the CPU
    bool no_update_after_bind = false;    // DeviceCaps::update_after_bind off: the texture arrays' plain limits
    bool no_independent_blend = false;    // DeviceCaps::independent_blend off: no scene motion or reactive mask

    bool any() const {
        return depth_stencil || depth_d24 || non_coherent || shadow_d16 || no_bc || no_mip_blit ||
               no_update_after_bind || no_independent_blend;
    }
};

struct GpuFallbackParse {
    GpuFallbacks forced;
    std::vector<std::string> unknown; // words that name no fallback, to be logged
};

// The setting's words, separated by commas or spaces: depth-stencil, depth-d24, non-coherent, shadow-d16, no-bc,
// no-mip-blit, no-update-after-bind, no-independent-blend, or all (every one but depth-d24, which most desktop GPUs
// lack).
GpuFallbackParse parse_gpu_fallbacks(std::string_view text);

// The setting's names for what `forced` holds, comma-separated ("" for none): the log's words.
std::string gpu_fallback_names(const GpuFallbacks& forced);

// The depth formats in VkContext's order of preference.
enum class DepthCandidate : std::uint8_t { D32, D32S8, D24S8 };
inline constexpr std::array<DepthCandidate, 3> kDepthCandidates{DepthCandidate::D32, DepthCandidate::D32S8,
                                                                DepthCandidate::D24S8};

struct DepthChoice {
    std::optional<DepthCandidate> format; // nullopt: none usable
    // Set when a forced depth fallback could not be had on this GPU: what was taken instead, for the log.
    std::string note;
};

// The depth format from which of kDepthCandidates the GPU can attach and sample (`usable`, in that order). A forced
// fallback skips the formats before it; where the GPU lacks it the normal choice stands, with a note.
DepthChoice choose_depth_format(const std::array<bool, 3>& usable, const GpuFallbacks& forced);

std::string_view depth_candidate_name(DepthCandidate format);

// A memory type's property flags as VMA is shown them: HOST_COHERENT (Vulkan's 0x4) dropped under non_coherent.
inline constexpr std::uint32_t kHostCoherentBit = 0x4;
inline std::uint32_t memory_flags_seen(std::uint32_t flags, const GpuFallbacks& forced) {
    return forced.non_coherent ? flags & ~kHostCoherentBit : flags;
}

} // namespace hailstone::render
