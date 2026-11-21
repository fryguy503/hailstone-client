/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
/* The SDK's example upscaler provider (sdk/hailstone_upscaler.h, docs/plugins.md "Upscaler providers"): it stretches
 * the scene's colour to the view's size with a linear blit. No temporal accumulation: it shows the calls, the
 * layouts and the Vulkan plumbing a real provider (XeSS, FSR 4) needs, and is a test fixture. Built with the
 * client into build/hailstone/upscalers/example/; display.upscaler = example (or --upscaler example) runs it.
 *
 * Built three ways for the tests (CMakeLists.txt): as it is; with EXAMPLE_REFUSE, whose hailstone_upscaler says no;
 * with EXAMPLE_ABI set past the client's, which the client refuses. EXAMPLE_REFUSE_DEVICE refuses every GPU, and
 * EXAMPLE_FAIL_EVALUATE fails its 30th frame (the client then goes on without it). */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

#include "hailstone_upscaler.h"

#ifndef EXAMPLE_ABI
#define EXAMPLE_ABI HAILSTONE_UPSCALER_ABI
#endif

static const HailstoneUpscalerHost* g_host;
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkCmdPipelineBarrier2 g_barrier;
static PFN_vkCmdBlitImage g_blit;
static VkExtent2D g_render, g_view;

static void say(int level, const char* line) {
    if (g_host && g_host->log) g_host->log(g_host->host, level, line);
}

static int instance_requirements(void* host, const char* const** names, uint32_t* count, uint32_t* min_api) {
    (void)host;
    *names = NULL;
    *count = 0;
    *min_api = VK_MAKE_API_VERSION(0, 1, 3, 0);
    return 1;
}

static int device_requirements(void* host, void* instance, void* physical, const char* const** names, uint32_t* count,
                               void* features) {
    (void)host;
    (void)features; /* it needs no feature past the client's */
    *names = NULL;
    *count = 0;
#ifdef EXAMPLE_REFUSE_DEVICE
    (void)instance;
    (void)physical;
    say(HAILSTONE_UPSCALER_LOG_INFO, "this build refuses every GPU");
    return 0;
#else
    /* A linear blit of RGBA16F: required by the Vulkan spec, checked as a real provider checks what it needs. */
    PFN_vkGetPhysicalDeviceFormatProperties props =
        (PFN_vkGetPhysicalDeviceFormatProperties)g_gipa((VkInstance)instance, "vkGetPhysicalDeviceFormatProperties");
    if (!props) return 0;
    VkFormatProperties fp;
    props((VkPhysicalDevice)physical, VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        say(HAILSTONE_UPSCALER_LOG_WARN, "this GPU cannot blit RGBA16F with linear filtering");
        return 0;
    }
    return 1;
#endif
}

static int create(void* host, const HailstoneUpscalerDevice* d) {
    (void)host;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)d->get_device_proc_addr;
    g_barrier = (PFN_vkCmdPipelineBarrier2)gdpa((VkDevice)d->device, "vkCmdPipelineBarrier2");
    g_blit = (PFN_vkCmdBlitImage)gdpa((VkDevice)d->device, "vkCmdBlitImage");
    return g_barrier && g_blit;
}

static int resize(void* host, uint32_t rw, uint32_t rh, uint32_t vw, uint32_t vh) {
    (void)host;
    char line[128];
    g_render.width = rw;
    g_render.height = rh;
    g_view.width = vw;
    g_view.height = vh;
    snprintf(line, sizeof line, "%ux%u to %ux%u", (unsigned)rw, (unsigned)rh, (unsigned)vw, (unsigned)vh);
    say(HAILSTONE_UPSCALER_LOG_INFO, line);
    return 1;
}

static void color_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    g_barrier(cmd, &dep);
}

static int evaluate(void* host, const HailstoneUpscalerFrame* f) {
    (void)host;
#ifdef EXAMPLE_FAIL_EVALUATE
    static int frames = 0;
    if (++frames == 30) {
        say(HAILSTONE_UPSCALER_LOG_WARN, "this build fails its 30th frame");
        return 0;
    }
#endif
    VkCommandBuffer cmd = (VkCommandBuffer)f->command_buffer;
    VkImage src = (VkImage)(uintptr_t)f->color.image;
    VkImage dst = (VkImage)(uintptr_t)f->output.image;
    if (!src || !dst) return 0;
    /* The colour from SHADER_READ_ONLY_OPTIMAL to a transfer source, and back after (the header's rule: every image
     * in the layout it came in when evaluate returns). The output is in GENERAL, where a blit may write. */
    color_barrier(cmd, src, (VkImageLayout)f->color.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageBlit region = {0};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[1].x = (int32_t)f->color.width;
    region.srcOffsets[1].y = (int32_t)f->color.height;
    region.srcOffsets[1].z = 1;
    region.dstSubresource = region.srcSubresource;
    region.dstOffsets[1].x = (int32_t)f->output.width;
    region.dstOffsets[1].y = (int32_t)f->output.height;
    region.dstOffsets[1].z = 1;
    g_blit(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, (VkImageLayout)f->output.layout, 1, &region,
           VK_FILTER_LINEAR);
    color_barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkImageLayout)f->color.layout,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    return 1;
}

static void destroy(void* host) {
    (void)host;
    g_barrier = NULL;
    g_blit = NULL;
}

static uint32_t ui_contributions(void* host, const HailstoneUiContribution* const** entries) {
    (void)host;
    static const HailstoneUiContribution notice = {
        sizeof(HailstoneUiContribution), 0, "linear-blit", "OptionsWindow", "section:graphics",
        "The example upscaler uses a linear blit.", NULL, NULL,
    };
    static const HailstoneUiContribution* const notices[] = {&notice};
    *entries = notices;
    return 1;
}

static const HailstoneUpscaler g_table = {
    EXAMPLE_ABI,
    sizeof(HailstoneUpscaler),
    "Example (linear blit)",
    0, /* no sharpening of its own: the client's RCAS runs after it */
    0,
    instance_requirements,
    device_requirements,
    create,
    resize,
    evaluate,
    destroy,
    ui_contributions,
};

HAILSTONE_UPSCALER_API const HailstoneUpscaler* hailstone_upscaler(const HailstoneUpscalerHost* host) {
    if (!host || host->abi < 1 || host->size < sizeof(HailstoneUpscalerHost)) return NULL;
    g_host = host;
    g_gipa = (PFN_vkGetInstanceProcAddr)host->get_instance_proc_addr;
#ifdef EXAMPLE_REFUSE
    say(HAILSTONE_UPSCALER_LOG_INFO, "this build says no");
    return NULL;
#else
    return &g_table;
#endif
}
