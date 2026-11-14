#pragma once

#include <volk.h>

namespace hailstone::render {

struct AttachmentSync {
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
};

enum class DepthAttachmentRole { SingleSample, MultisampleSource, ResolveTarget };

// Fixed-function depth resolves execute in COLOR_ATTACHMENT_OUTPUT, including the read of the multisample
// source and the read/write of the single-sample target. Ordinary depth testing still executes in EARLY/LATE.
constexpr AttachmentSync depth_attachment_sync(DepthAttachmentRole role) {
    constexpr VkPipelineStageFlags2 tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags2 depth_access =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (role == DepthAttachmentRole::ResolveTarget)
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    if (role == DepthAttachmentRole::MultisampleSource)
        return {tests | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                depth_access | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT};
    return {tests, depth_access};
}

struct DepthFrameSync {
    AttachmentSync attachment; // depth testing, and the resolve source when multisampling
    AttachmentSync sampled;    // the single-sample image copied for water and sampled by AO/post
};

constexpr DepthFrameSync depth_frame_sync(VkSampleCountFlagBits samples) {
    if (samples != VK_SAMPLE_COUNT_1_BIT)
        return {depth_attachment_sync(DepthAttachmentRole::MultisampleSource),
                depth_attachment_sync(DepthAttachmentRole::ResolveTarget)};
    const auto single = depth_attachment_sync(DepthAttachmentRole::SingleSample);
    return {single, single};
}

// The presentation semaphore also covers the final layout transition, whether or not external images are used.
inline constexpr VkPipelineStageFlags2 kPresentSignalStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

} // namespace hailstone::render
