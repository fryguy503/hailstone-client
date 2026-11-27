#include <doctest/doctest.h>

#include "render/gpu_fallbacks.h"

#include <array>
#include <cstdint>
#include <string>

using namespace hailstone::render;

TEST_CASE("GPU fallbacks: unset or empty forces nothing") {
    CHECK_FALSE(parse_gpu_fallbacks("").forced.any());
    CHECK_FALSE(parse_gpu_fallbacks(" , ,").forced.any());
    CHECK(parse_gpu_fallbacks("").unknown.empty());
    CHECK(gpu_fallback_names(GpuFallbacks{}).empty());
}

TEST_CASE("GPU fallbacks: each word forces its fallback alone") {
    auto only = [](const char* word) { return parse_gpu_fallbacks(word).forced; };
    CHECK(only("depth-stencil").depth_stencil);
    CHECK(only("depth-d24").depth_d24);
    CHECK(only("non-coherent").non_coherent);
    CHECK(only("shadow-d16").shadow_d16);
    CHECK(only("no-bc").no_bc);
    CHECK(only("no-mip-blit").no_mip_blit);
    CHECK(only("no-update-after-bind").no_update_after_bind);
    CHECK(only("no-independent-blend").no_independent_blend);
    const GpuFallbacks one = only("non-coherent");
    CHECK(gpu_fallback_names(one) == "non-coherent");
    CHECK_FALSE(one.depth_stencil);
    CHECK_FALSE(one.no_bc);
}

TEST_CASE("GPU fallbacks: lists, all and unknown words") {
    const GpuFallbackParse list = parse_gpu_fallbacks("no-bc, depth-stencil  bogus,non-coherent");
    CHECK(list.forced.no_bc);
    CHECK(list.forced.depth_stencil);
    CHECK(list.forced.non_coherent);
    CHECK_FALSE(list.forced.no_mip_blit);
    REQUIRE(list.unknown.size() == 1);
    CHECK(list.unknown[0] == "bogus");
    CHECK(gpu_fallback_names(list.forced) == "depth-stencil, non-coherent, no-bc");

    const GpuFallbacks all = parse_gpu_fallbacks("all").forced;
    CHECK(gpu_fallback_names(all) ==
          "depth-stencil, non-coherent, shadow-d16, no-bc, no-mip-blit, no-update-after-bind, no-independent-blend");
    CHECK_FALSE(all.depth_d24); // most desktop GPUs lack D24; asked for by name only

    // Case matters, as for the other HAILSTONE_ settings' words.
    CHECK(parse_gpu_fallbacks("No-BC").unknown.size() == 1);
}

TEST_CASE("GPU fallbacks: the depth format") {
    const std::array<bool, 3> every{true, true, true};
    const std::array<bool, 3> desktop_amd{true, true, false}; // RADV: no D24_UNORM_S8_UINT
    const std::array<bool, 3> only_d32{true, false, false};
    GpuFallbacks none;
    GpuFallbacks stencil;
    stencil.depth_stencil = true;
    GpuFallbacks d24;
    d24.depth_d24 = true;
    GpuFallbacks both = stencil;
    both.depth_d24 = true;

    SUBCASE("unforced: D32_SFLOAT first, then the stencil formats in order") {
        CHECK(choose_depth_format(every, none).format == DepthCandidate::D32);
        CHECK(choose_depth_format({false, true, true}, none).format == DepthCandidate::D32S8);
        CHECK(choose_depth_format({false, false, true}, none).format == DepthCandidate::D24S8);
        CHECK_FALSE(choose_depth_format({false, false, false}, none).format);
        CHECK(choose_depth_format(every, none).note.empty());
    }
    SUBCASE("depth-stencil takes what a GPU without D32_SFLOAT would") {
        CHECK(choose_depth_format(every, stencil).format == DepthCandidate::D32S8);
        CHECK(choose_depth_format({true, false, true}, stencil).format == DepthCandidate::D24S8);
        CHECK(choose_depth_format(every, stencil).note.empty());
    }
    SUBCASE("depth-d24 takes D24_UNORM_S8_UINT where there is one") {
        CHECK(choose_depth_format(every, d24).format == DepthCandidate::D24S8);
        CHECK(choose_depth_format(every, both).format == DepthCandidate::D24S8);
    }
    SUBCASE("a fallback the GPU lacks keeps the normal choice and says so") {
        const DepthChoice no_d24 = choose_depth_format(desktop_amd, d24);
        CHECK(no_d24.format == DepthCandidate::D32);
        CHECK(no_d24.note.find("depth-d24 cannot be forced") != std::string::npos);
        CHECK(no_d24.note.find("using D32_SFLOAT") != std::string::npos);

        const DepthChoice then_stencil = choose_depth_format(desktop_amd, both);
        CHECK(then_stencil.format == DepthCandidate::D32S8);
        CHECK(then_stencil.note.find("using D32_SFLOAT_S8_UINT") != std::string::npos);

        const DepthChoice no_stencil = choose_depth_format(only_d32, stencil);
        CHECK(no_stencil.format == DepthCandidate::D32);
        CHECK(no_stencil.note.find("depth-stencil cannot be forced") != std::string::npos);
    }
}

TEST_CASE("GPU fallbacks: non-coherent hides HOST_COHERENT and nothing else") {
    GpuFallbacks forced;
    forced.non_coherent = true;
    constexpr std::uint32_t device_local = 0x1, host_visible = 0x2, host_cached = 0x8;
    const std::uint32_t coherent_mapped = device_local | host_visible | kHostCoherentBit;
    CHECK(memory_flags_seen(coherent_mapped, GpuFallbacks{}) == coherent_mapped);
    CHECK(memory_flags_seen(coherent_mapped, forced) == (device_local | host_visible));
    CHECK(memory_flags_seen(host_visible | host_cached, forced) == (host_visible | host_cached));
    CHECK(memory_flags_seen(device_local, forced) == device_local);
}
