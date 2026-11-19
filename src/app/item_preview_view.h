#pragma once

// The item preview's 3D view (CObjectPreviewWnd's OPW_ViewRenderArea, game/item_preview.h): a copy of the player
// wearing the previewed item, or the item's own model, drawn by a renderer of its own into an image of its own each
// frame the window asks for it (before the scene), which the window then shows. The original's view is its own
// CRender camera on the preview actor (0x5770f0, 0x576120's fit, the OPW_ buttons and mouse turning, panning
// and zooming it); here an orbit camera fits the model's bounds and the buttons, drags and wheel move it (their steps
// are this client's; docs/unknowns.md).

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

#include <glm/glm.hpp>

#include "assets/character.h"
#include "render/vk_context.h"

namespace hailstone::render { class ActorRenderer; }

namespace hailstone::app {

class ActorEmitters;

class ItemPreviewView {
public:
    ItemPreviewView(render::VkContext& vk, const GameDir& game, std::shared_ptr<const assets::CharacterLibrary> library,
                    const std::filesystem::path& shader_dir);
    ~ItemPreviewView();
    ItemPreviewView(const ItemPreviewView&) = delete;
    ItemPreviewView& operator=(const ItemPreviewView&) = delete;

    const assets::CharacterLibrary& library() const { return *library_; }
    // What to show: `model` dressed as `mesh` (null: its own geometry), or nothing. A new subject resets the camera
    // (0x576120). `stand_in`: the model is set_item_subject's one-bone stand-in, framed by its own bounds.
    void set_subject(std::shared_ptr<const assets::CharacterModel> model, std::shared_ptr<const assets::CharacterMesh> mesh,
                     float scale, float z_offset, bool stand_in = false);
    // The item alone: its model `number` (IT<n>) on a one-bone stand-in; false when the item has no model.
    bool set_item_subject(std::uint32_t number);
    void clear_subject();
    // The clip the preview actor plays: standing, its idle.
    static const assets::AnimationClip& shown_clip(const assets::CharacterModel& model);
    // The model-space bounds the camera fits (before the subject's scale): the model's own, or for a body-less
    // skeleton (an IVM illusion; not `stand_in`, set_item_subject's stand-in) the items it holds, posed as shown.
    static void framed_bounds(const assets::CharacterModel& model, const assets::CharacterMesh* mesh, bool stand_in,
                              glm::vec3& lo, glm::vec3& hi);
    bool has_subject() const { return model_ != nullptr; }

    // The image for this frame, `width` x `height` pixels (an ImTextureID), drawn when the frame is recorded; null
    // while there is nothing to show.
    void* image(int width, int height);
    // The OPW_ buttons and the mouse: "rotate" (dx, dy: radians), "pan" (dx, dy: fractions of the view), "zoom"
    // (factor), "reset".
    void control(std::string_view what, float x, float y);

    // Records this frame's image when one was asked for since the last frame (between VkContext::begin_frame_commands
    // and begin_scene).
    // Advance and upload particle textures outside a frame, before record().
    void prepare(float dt);
    void record(const render::FrameInfo& frame);

private:
    struct Target {
        int width = 0, height = 0;
        VkImage msaa = VK_NULL_HANDLE, resolve = VK_NULL_HANDLE, depth = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
        VmaAllocation msaa_alloc = VK_NULL_HANDLE, resolve_alloc = VK_NULL_HANDLE, depth_alloc = VK_NULL_HANDLE,
                      out_alloc = VK_NULL_HANDLE;
        VkImageView msaa_view = VK_NULL_HANDLE, resolve_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE,
                    out_view = VK_NULL_HANDLE, out_attachment_view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE; // ImGui's
        VkDescriptorSet resolve_set = VK_NULL_HANDLE;
        bool drawn = false; // the out image has been through a frame (shader-read layout)
    };
    void make_target(Target& t, int width, int height);
    void destroy_images(Target& t);
    void create_resolve(const std::filesystem::path& shader_dir);
    void destroy_resolve();
    void reset_camera();
    std::pair<glm::vec3, glm::vec3> camera() const; // eye, target

    render::VkContext& vk_;
    std::shared_ptr<const assets::CharacterLibrary> library_;
    std::unique_ptr<render::ActorRenderer> actors_;
    std::unique_ptr<ActorEmitters> emitters_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolve_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool resolve_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout resolve_layout_ = VK_NULL_HANDLE;
    VkPipeline resolve_pipeline_ = VK_NULL_HANDLE;
    std::array<Target, render::VkContext::kFramesInFlight> targets_{};
    int want_width_ = 0, want_height_ = 0; // asked for this frame (0: not asked)
    std::uint32_t want_slot_ = 0;

    std::shared_ptr<const assets::CharacterModel> model_;
    std::shared_ptr<const assets::CharacterMesh> mesh_;
    float scale_ = 1.0f, z_offset_ = 0.0f;
    bool stand_in_ = false;
    float time_ = 0.0f;
    // The orbit camera: around the model's centre (+ pan), at `distance_` x zoom.
    glm::vec3 centre_{0.0f};
    float radius_ = 1.0f;
    float yaw_ = 0.0f, pitch_ = 0.0f, zoom_ = 1.0f;
    glm::vec2 pan_{0.0f};
};

} // namespace hailstone::app
