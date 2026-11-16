#pragma once

// Draws the name plates over the 3D view, hidden wherever the scene is nearer than the plate (docs/graphics.md "Name
// plates"). The client's draw_nameplates and the viewer's --plate-test both draw through it. ImGui has no depth test,
// so each plate's text runs through a pipeline of ours (shaders/plate_depth.*) that tests every pixel against the
// scene's depth, bound by ImGui draw callbacks: same frame, pixel exact.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "game/nameplate_style.h"
#include "game/nameplate_presentation.h"
#include "game/nameplate_overlap.h"
#include "game/clicks.h"
#include "game/picking.h"
#include "render/vk_context.h"

namespace hailstone::app {

// One world plate: the ordinary caption, or a scoped add-on presentation in the same depth layer.
struct NameplateText {
    glm::vec2 pos{0.0f};            // window pixels (ImGui's): the middle of the name, its top edge
    float depth = 0.0f;             // the head's view depth, the distance along the view direction
    std::string name;
    std::vector<std::string> lines; // under the name, one a line: an NPC's owner, a player's guild
    ImU32 color = IM_COL32_WHITE;
    bool target = false;            // drawn last, never faded by the names over it
    bool through_walls = false;     // never hidden by the scene (the target's, option target_plate_through_walls)
    bool highlight = false;         // the dark plate, edge and marker (the target's, option target_plate_highlight)
    float alpha = 1.0f;             // 0..1 on everything drawn (an overlapped name fades, game/nameplate_overlap.h)
    float scale = 1.0f;             // on the style's size (option plate_far_scale, game::plate_distance_factor)
    std::optional<game::NameplatePresentation> presentation; // appearance only: never changes visibility or depth
};

// How every plate's text is drawn (game/nameplate_style.h): the defaults are ImGui's current font at its size with a
// drop shadow, the look before the options.
struct NameplateDrawStyle {
    struct Icon {
        ImTextureID texture{};
        ImVec2 uv0{0, 0}, uv1{1, 1};
    };
    ImFont* font = nullptr;         // nullptr: ImGui's current font
    float size = 0.0f;              // ImGui pixels; 0: the font's own size
    bool faux_bold = false;         // each letter struck twice, a pixel apart (a face with no bold file)
    game::PlateEdge edge = game::PlateEdge::Shadow;
    float tag_scale = 1.0f;         // the lines under the name, on the name's size
    std::function<std::optional<Icon>(std::uint32_t)> spell_icon; // optional art from the player's own game files
};

// The painter has no GPU dependency: the viewer and headless tests use the same measurements and draw commands.
glm::vec2 nameplate_size(const NameplateText& plate, const NameplateDrawStyle& style);
void paint_nameplate(ImDrawList& draw, const NameplateText& plate, const NameplateDrawStyle& style);
glm::vec4 nameplate_bounds(const NameplateText& plate, const NameplateDrawStyle& style);

struct NameplateHit {
    std::uint32_t id = 0;
    glm::vec4 bounds{0.0f};
    glm::vec3 head{0.0f};
    bool through_walls = false;
    float depth = 0.0f;
};
std::optional<std::uint32_t> pick_nameplate(const std::vector<NameplateHit>& hits, glm::vec2 at,
                                           const std::function<bool(const NameplateHit&)>& eligible);
bool nameplate_mouse_eligible(const game::MaskSpawn& spawn, std::uint8_t options, bool self, int camera);
std::optional<game::PickSegment> nameplate_click_ray(const glm::mat4& inverse_scene, const glm::vec4& view,
                                                    const glm::vec3& camera, glm::vec2 at);
std::optional<float> nameplate_scene_depth(const game::PickSegment& ray, const game::PickScene& scene,
                                          const glm::vec3& forward);
bool nameplate_depth_visible(float plate_depth, const std::optional<float>& scene_depth);

struct NameplateGesture {
    std::optional<std::uint32_t> id;
    glm::vec2 pressed{0.0f};
    bool cancelled = false;
    bool captured_elsewhere = false;
    void begin(std::uint32_t spawn, glm::vec2 at) { id = spawn; pressed = at; cancelled = false; captured_elsewhere = false; }
    void update(glm::vec2 at, bool blocked) {
        captured_elsewhere = captured_elsewhere || blocked;
        const glm::vec2 delta = at - pressed;
        if (blocked || glm::dot(delta, delta) > 25.0f) cancelled = true;
    }
    bool transfer_drag(bool held, bool allow_look) {
        if (!id || !cancelled || captured_elsewhere || !held || !allow_look) return false;
        clear();
        return true;
    }
    std::optional<std::uint32_t> release(glm::vec2 at, std::optional<std::uint32_t> hit, bool blocked) {
        update(at, blocked);
        const auto selected = !cancelled && id == hit ? id : std::nullopt;
        id.reset();
        return selected;
    }
    void clear() { id.reset(); cancelled = false; captured_elsewhere = false; }
};

struct NameplateStack {
    bool enabled = false;
    float gap = 4.0f, max_offset = 96.0f;
    std::uint32_t id = 0; // stable order for actors at equal depth
};
// Optional, bounded upward stacking. The target stays pinned; depth and horizontal position never change.
void stack_nameplate_boxes(std::vector<game::PlateBox>& boxes, const std::vector<NameplateStack>& settings);

class NameplateLayer {
public:
    // When the pipeline cannot be made (a missing shader) the log says so and the plates are drawn plain.
    NameplateLayer(render::VkContext& vk, const std::filesystem::path& shader_dir);
    ~NameplateLayer();
    NameplateLayer(const NameplateLayer&) = delete;
    NameplateLayer& operator=(const NameplateLayer&) = delete;

    // Starts a frame of plates on `dl`; `view`: the 3D view in window pixels (x, y, width, height), `near_plane` and
    // `far_plane` the projection's (they give the scene's view depth from its depth buffer).
    void begin(const render::FrameInfo& frame, ImDrawList* dl, const glm::vec4& view, float near_plane, float far_plane);
    bool draw(const NameplateText& plate);
    // The complete drawn box under pos (caption, or replacement bars, badges and text).
    glm::vec2 size_of(const NameplateText& plate) const;
    glm::vec4 bounds_of(const NameplateText& plate) const { return nameplate_bounds(plate, style_); }
    // From the next draw (the client's, set each frame from its options).
    void set_style(const NameplateDrawStyle& style) { style_ = style; }
    // Off: plates are drawn plain (the viewer's --plate-test-no-depth, the reference of its comparison).
    void set_depth_test(bool on) { depth_test_ = on; }
    // The alpha where the scene hides text: 0 (the viewer's default) drops it; only the viewer's test sets it.
    void set_occluded_alpha(float alpha) { occluded_alpha_ = alpha; }
    bool supports_presentations() const { return usable_ && depth_test_; }

private:
    static constexpr std::uint32_t kMaxPlates = 1024; // a frame's, each with its own parameters in the buffer
    struct Params {
        glm::vec4 view;  // the view on the framebuffer, pixels
        glm::vec4 depth; // the depth terms, the plate's depth, the margin
        glm::vec4 extra; // the alpha of text behind the scene
    };
    struct Callback {
        NameplateLayer* layer;
        std::uint32_t index;
    };
    static void bind(const ImDrawList*, const ImDrawCmd* cmd);
    void create(const std::filesystem::path& shader_dir);
    void paint(const NameplateText& plate) const;

    render::VkContext& vk_;
    VkDevice device_;
    VmaAllocator allocator_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE; // set 1: the depth and the parameters
    VkDescriptorSetLayout font_layout_ = VK_NULL_HANDLE; // set 0, as ImGui's: its font texture
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, render::VkContext::kFramesInFlight> sets_{};
    std::array<VkBuffer, render::VkContext::kFramesInFlight> buffers_{};
    std::array<VmaAllocation, render::VkContext::kFramesInFlight> allocations_{};
    std::array<void*, render::VkContext::kFramesInFlight> mapped_{};
    VkDeviceSize stride_ = 0;

    bool usable_ = false; // the pipeline was made
    bool depth_test_ = true;
    float occluded_alpha_ = 0.0f;
    NameplateDrawStyle style_;
    // This frame's.
    std::uint32_t slot_ = 0;
    ImDrawList* dl_ = nullptr;
    glm::vec4 view_{0.0f};
    glm::vec2 depth_terms_{0.0f};
    std::uint32_t plates_ = 0;
};

} // namespace hailstone::app
