#include <doctest/doctest.h>

#include <initializer_list>

#include "render/frame_sync.h"

using namespace hailstone::render;

TEST_CASE("single-sample scene depth uses depth testing for water and AO transitions") {
    const auto frame = depth_frame_sync(VK_SAMPLE_COUNT_1_BIT);
    const auto stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const auto access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    CHECK(frame.attachment.stages == stages);
    CHECK(frame.attachment.access == access);
    CHECK(frame.sampled.stages == stages);
    CHECK(frame.sampled.access == access);
}

TEST_CASE("four-sample scene depth includes resolve reads before rendering resumes") {
    const auto frame = depth_frame_sync(VK_SAMPLE_COUNT_4_BIT);
    CHECK((frame.attachment.stages & VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT) != 0);
    CHECK((frame.attachment.stages & VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT) != 0);
    CHECK((frame.attachment.access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);
    CHECK((frame.attachment.stages & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) != 0);
    CHECK((frame.attachment.access & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) != 0);
    CHECK((frame.attachment.access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) == 0);
}

TEST_CASE("four-sample scene resolve target uses color output for water copies and AO reads") {
    const auto frame = depth_frame_sync(VK_SAMPLE_COUNT_4_BIT);
    CHECK(frame.sampled.stages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(frame.sampled.access == (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
    CHECK((frame.sampled.access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) == 0);
}

TEST_CASE("every multisample count uses the same fixed-function depth resolve scopes") {
    const auto reference = depth_frame_sync(VK_SAMPLE_COUNT_4_BIT);
    for (auto samples : {VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_16_BIT,
                         VK_SAMPLE_COUNT_32_BIT, VK_SAMPLE_COUNT_64_BIT}) {
        const auto frame = depth_frame_sync(samples);
        CHECK(frame.attachment.stages == reference.attachment.stages);
        CHECK(frame.attachment.access == reference.attachment.access);
        CHECK(frame.sampled.stages == reference.sampled.stages);
        CHECK(frame.sampled.access == reference.sampled.access);
    }
}

TEST_CASE("presentation waits for final layout transitions in both private and shared frames") {
    CHECK(kPresentSignalStage == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
}
