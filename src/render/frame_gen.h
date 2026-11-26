#pragma once

// Frame generation's GPU side (docs/graphics.md "Frame generation"): the in-between frame's motion, made from this
// frame's motion and depth (shaders/fg_scatter.comp, fg_fill.comp), and the descriptor sets the synthesis
// (shaders/fg_synth.frag) and the composite of a presented frame (fg_composite.frag) read. VkContext makes their two
// full-screen pipelines with these layouts, owns the pictures and the swapchain, and presents (render/vk_context.cpp
// end_frame); render/frame_gen_logic.h holds the decisions.

#include <array>
#include <cstdint>
#include <filesystem>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace hailstone::render {

class FrameGen {
public:
    FrameGen(VkDevice device, VmaAllocator allocator, VkPipelineCache cache, const std::filesystem::path& shader_dir);
    ~FrameGen();
    FrameGen(const FrameGen&) = delete;
    FrameGen& operator=(const FrameGen&) = delete;

    // The motion images at the scene's size (made again on a change; the device is idle).
    void resize(VkExtent2D scene);

    // The pictures and maps the passes read. `pictures`: the two pictures without the UI (written in turn; set i makes
    // the frame between picture i ^ 1 and picture i); `generated`: the in-between frame; `overlay`: the UI's image;
    // `reactive` VK_NULL_HANDLE when the scene has no reactive mask (`placeholder` stands in). Called after every
    // change to any of them, the device idle.
    struct Views {
        VkImageView depth = VK_NULL_HANDLE;
        VkImageView motion = VK_NULL_HANDLE;
        VkImageView reactive = VK_NULL_HANDLE;
        VkImageView placeholder = VK_NULL_HANDLE;
        std::array<VkImageView, 2> pictures{};
        VkImageView generated = VK_NULL_HANDLE;
        VkImageView overlay = VK_NULL_HANDLE;
        VkSampler nearest = VK_NULL_HANDLE;
        VkSampler linear = VK_NULL_HANDLE;
    };
    void bind(const Views& views);

    // Records step 1 and 2 (the in-between motion, from this frame's motion and depth, both readable by fragment
    // shaders as the frame left them) into `cmd`, outside rendering, at time `t`.
    void record_motion(VkCommandBuffer cmd, float t);

    // The synthesis' layout and set (picture `slot` is this frame's) and the composite's: the set for picture `slot`
    // (a real frame) or, with `generated`, the in-between frame.
    VkPipelineLayout synth_layout() const { return synth_layout_; }
    VkDescriptorSet synth_set(std::uint32_t slot) const { return synth_sets_[slot & 1u]; }
    VkPipelineLayout composite_layout() const { return composite_layout_; }
    VkDescriptorSet composite_set(std::uint32_t slot, bool generated) const {
        return generated ? composite_sets_[2] : composite_sets_[slot & 1u];
    }

    // synth_layout's push constants (shaders/fg_synth.frag) and composite_layout's (fg_composite.frag).
    struct SynthPush {
        float view_w = 0.0f, view_h = 0.0f;
        float t = 0.5f;
        float reactive = 0.0f;
    };
    struct CompositePush {
        float area[4]{};
        std::uint32_t flags = 0;
        std::uint32_t pad[3]{};
    };

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    Image make_image(VkFormat format, VkImageUsageFlags usage);
    void free_image(Image& image);

    VkDevice device_;
    VmaAllocator allocator_;
    VkExtent2D scene_{};
    Image key_, prev_key_, field_, motion_t_; // fg_scatter.comp's and fg_fill.comp's, kept in GENERAL
    bool fresh_ = true;            // made since the last record_motion: their layouts are still UNDEFINED

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> synth_sets_{};
    std::array<VkDescriptorSet, 3> composite_sets_{};
    VkPipelineLayout compute_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout synth_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
    VkPipeline scatter_pipeline_ = VK_NULL_HANDLE;
    VkPipeline fill_pipeline_ = VK_NULL_HANDLE;
};

} // namespace hailstone::render
