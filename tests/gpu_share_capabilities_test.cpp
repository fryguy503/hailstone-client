#include <doctest/doctest.h>

#include <limits>

#include "gpu_lab/gpu_share_capabilities.h"

using namespace hailstone::render;

namespace {
GpuShareCapabilities handoff_capabilities() {
    GpuShareCapabilities out;
    out.result = VK_SUCCESS;
    out.transport = GpuShareTransport::OpaqueWin32;
    out.memory_extension = out.semaphore_extension = true;
    out.transfer_buffer = {true, true, true, false, true, VK_SUCCESS};
    out.geometry_buffer = out.transfer_buffer;
    out.binary_semaphore_queried = out.binary_semaphore_importable = out.binary_semaphore_exportable = true;
    return out;
}
} // namespace

TEST_CASE("GPU share capabilities: incomplete memory and synchronization support cannot enable external handoff") {
    CHECK_FALSE(external_transfer_supported({}));
    const auto supported = handoff_capabilities();
    REQUIRE(external_transfer_supported(supported));
    for (int missing = 0; missing < 11; ++missing) {
        auto candidate = supported;
        switch (missing) {
        case 0: candidate.transport = GpuShareTransport::None; break;
        case 1: candidate.result = VK_ERROR_INITIALIZATION_FAILED; break;
        case 2: candidate.memory_extension = false; break;
        case 3: candidate.semaphore_extension = false; break;
        case 4: candidate.transfer_buffer.importable = false; break;
        case 5: candidate.geometry_buffer.exportable = false; break;
        case 6: candidate.geometry_buffer.queried = false; break;
        case 7: candidate.geometry_buffer.result = VK_ERROR_FORMAT_NOT_SUPPORTED; break;
        case 8: candidate.binary_semaphore_queried = false; break;
        case 9: candidate.binary_semaphore_importable = false; break;
        case 10: candidate.geometry_buffer.compatible_handle = false; break;
        }
        CAPTURE(missing);
        CHECK_FALSE(external_transfer_supported(candidate));
    }
}

TEST_CASE("GPU share capabilities: vendor-neutral decisions use actual resource capabilities") {
    // Synthetic labels do not establish hardware support; identical capabilities must yield identical decisions.
    for (const auto* model : {"AMD synthetic", "NVIDIA synthetic", "Intel synthetic"}) {
        CAPTURE(model);
        auto candidate = handoff_capabilities();
        CHECK(external_transfer_supported(candidate));
        candidate.transport = GpuShareTransport::OpaqueFd;
        CHECK(external_transfer_supported(candidate));
        candidate.geometry_buffer.dedicated_only = true;
        CHECK(external_transfer_supported(candidate)); // a dedicated transfer is allowed, but needs different allocation handling
        candidate.binary_semaphore_exportable = false;
        CHECK_FALSE(external_transfer_supported(candidate));
    }
}

TEST_CASE("GPU alias smoke: bounds reject zero unaligned oversized and overflowing requests before GPU use") {
    CHECK(valid_gpu_alias_smoke_options({}));
    CHECK(valid_gpu_alias_smoke_options({1, 4}));
    CHECK(valid_gpu_alias_smoke_options({64, VkDeviceSize{1} << 20}));
    CHECK_FALSE(valid_gpu_alias_smoke_options({0, 4}));
    CHECK_FALSE(valid_gpu_alias_smoke_options({65, 4}));
    CHECK_FALSE(valid_gpu_alias_smoke_options({1, 0}));
    CHECK_FALSE(valid_gpu_alias_smoke_options({1, 3}));
    CHECK_FALSE(valid_gpu_alias_smoke_options({1, 5}));
    CHECK_FALSE(valid_gpu_alias_smoke_options({1, (VkDeviceSize{1} << 20) + 4}));
    CHECK_FALSE(valid_gpu_alias_smoke_options({64, std::numeric_limits<VkDeviceSize>::max()}));
    const auto rejected = run_gpu_alias_smoke(VK_NULL_HANDLE, {64, std::numeric_limits<VkDeviceSize>::max()});
    CHECK_FALSE(rejected.matched);
    CHECK(rejected.verified_words == 0);
    CHECK_FALSE(rejected.error.empty());
}
