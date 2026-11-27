#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <volk.h>
#include <vk_mem_alloc.h>

#include "render/frame_gen.h"
#include "render/frame_gen_logic.h"
#include "render/gpu_timer.h"
#include "render/graphics_tier.h"
#include "render/fsr3_upscaler.h"
#include "render/pipeline_cache.h"
#include "render/point_shadows.h"
#include "render/retire_queue.h"
#include "render/shadows.h"
#include "render/swapchain_size.h"
#include "render/temporal.h"
#include "render/upscaler_provider.h"
#include "render/gpu.h"
#include "render/gpu_fallbacks.h"
#include "render/zone_fog.h"

namespace hailstone::platform { class Window; }

namespace hailstone::render {

class GpuSharedCache;

// Optional device capabilities the renderers adapt to.
struct DeviceCaps {
    bool bc_textures = false;  // BC1-3 sampled images (absent on some arm64 GPUs)
    // R8G8B8A8_SRGB images can be blitted with linear filtering: UploadBatch::texture builds an opaque texture's mips
    // on the GPU in linear light only then (render::gpu_builds_mips), and on the CPU otherwise.
    bool srgb_mip_blit = false;
    // Colour attachments may blend differently (independentBlend): the scene's motion and reactive mask beside its
    // colour need it (VkContext::scene_motion).
    bool independent_blend = false;
    float max_anisotropy = 0.0f; // 0 when sampler anisotropy is unsupported
    // The textures one bindless array may hold (render/descriptor_limits.h), under the update-after-bind limits when
    // update_after_bind is set.
    std::uint32_t max_sampled_images = 0;
    // The texture arrays' sets are update-after-bind (descriptorBindingSampledImageUpdateAfterBind): their limits are
    // far higher on some drivers (Intel on Windows: 64 samplers per stage without, 1048576 with).
    bool update_after_bind = false;
    VkSampleCountFlags msaa_counts = VK_SAMPLE_COUNT_1_BIT; // colour and depth framebuffer sample counts
    // AMD FSR 3.1's upscaler can run (render/fsr3_upscaler.h supported): FSR's storage formats and quad subgroup
    // operations in compute. Without it display.upscaler fsr3 falls back to taa.
    bool fsr3 = false;
};

// How the scene is drawn and shown. The scene renders in linear colour into a floating-point target (multisampled
// when msaa > 1), which a full-screen pass tonemaps into the swapchain before the UI is drawn on top.
// The tone curve (shaders/post.frag). Soft keeps display values to 0.8 as they are and rolls the rest off towards
// white (shaders/classic_light.glsl soft_highlights): the smooth classic lighting's (LightingMode::SmoothClassic).
enum class Tonemap : std::uint8_t { None, AgX, Aces, Soft };

// The most MSAA samples the scene is drawn with: a request for more (display.msaa 8, --msaa 8) is drawn at this.
inline constexpr std::uint32_t kMaxMsaa = 4;

struct SceneOptions {
    std::uint32_t msaa = 4;         // samples per pixel; cut to kMaxMsaa and to what the device supports
    Tonemap tonemap = Tonemap::None;
    float exposure = 0.0f;          // EV: the scene is scaled by 2^exposure before the tonemap
    bool dither = true;             // break up banding in fog and sky gradients
    std::uint32_t shadow_size = 2048; // texels per side of each shadow cascade (render/shadows.h); 0: no shadows
    // The zone lights' shadows (render/point_shadows.h): how many lights (at most kMaxPointShadows; 0 none) and the
    // texels per side of each cube face.
    std::uint32_t light_shadows = 8;
    std::uint32_t light_shadow_size = 512;
    bool ambient_occlusion = true;    // screen-space AO (shaders/ao.frag) in frames that give set_ao_view
    float ao_strength = 0.85f;        // how far the AO darkens the scene (0..1)
    bool distance_blur = true;        // soften the far distance a little, with the AO's frames
    // Draw the opaque geometry depth-only first, then colour it with the depth test equal and no depth write
    // (render/zone_renderer.cpp draw_depth): a pixel covered by several surfaces is shaded once.
    bool depth_prepass = false;
    // Bright light spreading into its surroundings (0 off .. 1 strong), with the AO's frames: a chain of
    // half-size levels of the scene (shaders/bloom_down.frag, bloom_up.frag) mixed in before the tonemap.
    float bloom = 0.5f;
    // The picture's grade after the tonemap, in display values (the UI is not graded): brightness is added (0 none),
    // contrast scales around mid grey (1 none), gamma bends the curve (1 none, above 1 brighter shadows).
    float brightness = 0.0f;
    float contrast = 1.0f;
    float gamma = 1.0f;
    // The modern look's colour grade (set_color_grade; display.color_grade): by the time of day, the weather and the
    // zone's mood.
    bool color_grade = true;
    // The 3D view drawn at this share of its size on each side (display.render_scale, kMinRenderScale..1) and brought up to it by
    // FSR 1 (shaders/fsr_easu.frag, sharpened by fsr_rcas.frag at `sharpness`, 0 none .. 1 the most); 1 draws it
    // at its own size, without them.
    float render_scale = 1.0f;
    float sharpness = 0.5f;
    // How the view is brought up to its size (render/temporal.h, display.upscaler). Taa draws with one sample
    // whatever msaa says, jitters the scene's projection each frame (projection_jitter) and resolves it into a
    // history at the view's size; `sharpness` then sharpens that with RCAS (0 none). Fsr3 does the same with AMD
    // FSR 3.1's upscaler in place of the resolve, its own RCAS at `sharpness`; Taa where DeviceCaps::fsr3 is false.
    // External runs the upscaler provider VkContext was made with in FSR 3.1's place (Fsr3, then Taa, without one).
    Upscaler upscaler = Upscaler::Fsr1;
    // Frame generation (render/frame_gen.h, display.frame_gen): a frame made between each two real ones from the
    // temporal upscaler's motion and depth, presented before the real one, the UI drawn sharp over both. Only with a
    // temporal upscaler; the swapchain then keeps vertical sync (FIFO paces the two frames) and one more image.
    bool frame_gen = false;
};

// The scene targets' size for a 3D view of `view` pixels at `scale` (SceneOptions::render_scale, kept in
// kMinRenderScale..1):
// each side rounded, at least one pixel.
VkExtent2D scaled_extent(VkExtent2D view, float scale);

// The sun's light shafts (god rays) for one frame: where the sun is on the screen (UV, may lie off it), how strong
// (0 none), and its light (linear).
struct SunShafts {
    glm::vec2 uv{0.5f};
    float strength = 0.0f;
    glm::vec3 color{1.0f};
};

// The shafts for the sun at `to_sun` (a direction) seen through `view_proj`, lit by `light` (linear): strongest with
// the sun on the screen and above the horizon, fading as it leaves the screen (by half a screen past the edge) or
// sets; none behind the camera.
SunShafts sun_shafts_for(const glm::mat4& view_proj, const glm::vec3& to_sun, const glm::vec3& light);

// The view the screen-space AO reconstructs positions with: FlyCamera's projection terms, and the AO's reach.
struct AoView {
    float tan_half_x = 1.0f;
    float tan_half_y = 1.0f;
    float near_plane = 1.0f;
    float far_plane = 1000.0f;
    float radius = 4.0f;    // world units: well under a character's height (6), or legs smear the ground
    // ao_visibility's scale (shaders/ao_kernel.glsl): 12 puts a contact corner's shade near the old kernel's (4 with
    // its x5 and 1/|v| weight) at mid range, while a low-poly face's shallow creases stay unshaded.
    float intensity = 12.0f;
};

// Parses display.tonemap's values (none, agx, aces, soft); unknown names give None.
Tonemap parse_tonemap(std::string_view name);

// One frame being recorded. Rendering into the scene target (colour + depth) is already begun.
// What a scene pipeline's fragment shader writes besides the colour, under the temporal upscaler
// (VkContext::scene_blends; docs/graphics.md "Temporal upscaling"): its own motion (`o_motion`, location 1: geometry
// that moves by itself or sways) or how much the history should give way to it (`o_reactive`, location 2: blended and
// additive draws, which have no motion of their own), or both (`MotionReactive`: a blended part of something that
// moves, a character's see-through shield or hair, whose pixels must move with it, not with what shows through:
// frame generation would split it). `MotionReactiveWhereSeen` is the same for an additive part of something that
// moves (a shield's glowing rim): its motion and depth are blended in by the fragment's alpha, which the shader sets
// to 1 only where the glow adds something to see, so a faint glow spread past the body leaves what is behind it to
// move as it does. The other attachments are masked off.
enum class SceneOutput : std::uint8_t { Nothing, Motion, Reactive, MotionReactive, MotionReactiveWhereSeen };

// A scene pipeline's colour blend states, one per colour attachment of the scene (VkContext::scene_blends). info()
// points into `states`: keep this object alive while the pipeline is made.
struct SceneBlends {
    std::array<VkPipelineColorBlendAttachmentState, 3> states{};
    std::uint32_t count = 1;
    VkPipelineColorBlendStateCreateInfo info() const {
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = count;
        cb.pAttachments = states.data();
        return cb;
    }
};

// A scene pipeline's blend states for colour blend `color` and `output` (VkContext::scene_blends): the colour alone
// without `motion`, else the colour, the object motion and the reactive mask.
SceneBlends make_scene_blends(const VkPipelineColorBlendAttachmentState& color, SceneOutput output, bool motion);

// The value the object-motion attachment is cleared to: no draw gave this pixel a motion of its own, and
// shaders/temporal_prep.frag takes the camera's.
inline constexpr float kMotionUnset = 30000.0f;

struct FrameInfo {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // The scene target's size, in pixels: the 3D view's (VkContext::set_scene_rect) times SceneOptions::render_scale.
    // Window coordinates (picking, the nameplates) take the view's rectangle instead.
    VkExtent2D extent{};
    VkOffset2D offset{};   // the 3D view's place on the window's image
    std::uint32_t image_index = 0;
    std::uint32_t frame_slot = 0; // 0..kFramesInFlight-1, for per-frame resources
};

// The real frame's queue results, independent of an optional generated frame.
// Presented means accepted by vkQueuePresentKHR, not a visible-pixel guarantee.
struct FrameOutcome {
    bool submitted = false;
    bool presented = false;
};

// Vulkan 1.3 device, swapchain, scene target and frame loop. Uses dynamic
// rendering and synchronization2, so there are no render pass or framebuffer
// objects. Depth uses reverse Z: clear to 0, test GREATER_OR_EQUAL.
//
// A frame: begin_frame_commands starts the command buffer; the shadow cascades
// may be drawn (begin_shadow_cascade .. end_shadow_cascade); begin_scene begins
// rendering into the scene target (scene_format(), scene_samples(),
// depth_format()); begin_overlay resolves it, tonemaps it into the swapchain
// image and leaves rendering open there (color_format(), one sample, no depth)
// for the UI; end_frame presents. begin_frame is begin_frame_commands and
// begin_scene together; end_frame calls begin_overlay itself when nothing did. Between begin_scene and
// begin_overlay, begin_translucent may split the scene: what is drawn so far is copied for the modern water to
// read (shadow_set() bindings 1 and 2) and drawing goes on into the same target.
class VkContext {
public:
    static constexpr std::uint32_t kFramesInFlight = 2;

    // `upscaler`: an upscaler provider (render/upscaler_provider.h) to run when SceneOptions::upscaler is External.
    // Its instance and device requirements are added here; one the GPU or the loader cannot meet, or that says no,
    // is dropped (logged), and External then falls back.
    VkContext(platform::Window& window, bool validation, std::unique_ptr<UpscalerProvider> upscaler = nullptr);
    ~VkContext();
    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    // Waits for the frame slot, acquires an image and begins rendering into the scene target, cleared to
    // `clear_color` (display sRGB, as the fog colour is given; converted to linear here).
    // Returns nullopt when nothing can be drawn this frame (minimised, resizing).
    std::optional<FrameInfo> begin_frame(const glm::vec4& clear_color);
    std::optional<FrameInfo> begin_frame_commands();
    void begin_scene(const FrameInfo& frame, const glm::vec4& clear_color);
    void begin_overlay(const FrameInfo& frame);
    // Copies the scene's colour and depth so far (the opaque world) for the water to refract and reflect, then goes
    // on rendering into the scene target. At most once per frame, inside the scene.
    void begin_translucent(const FrameInfo& frame);

    // Depth-only rendering into one layer of the shadow map (shadow_format(), one sample, no colour attachment,
    // a slope-scaled depth bias set): a cascade, or kCoverLayer. Every cascade is drawn in a frame that uses shadows;
    // the cover map when it is used.
    // `secondary`: its draws come from secondary command buffers (render/scene_recorder.h), which set the layer's
    // dynamic state themselves (set_shadow_state); the rendering then holds nothing but vkCmdExecuteCommands.
    void begin_shadow_cascade(const FrameInfo& frame, std::uint32_t cascade, bool secondary = false);
    // A secondary command buffer's inheritance for a shadow layer's rendering, and the dynamic state
    // begin_shadow_cascade sets for `cascade` (the viewport, the scissor, the depth bias), into `cmd`.
    VkCommandBufferInheritanceRenderingInfo shadow_inheritance() const;
    void set_shadow_state(VkCommandBuffer cmd, std::uint32_t cascade) const;
    void end_shadow_cascade(const FrameInfo& frame);
    bool shadows_enabled() const { return scene_options_.shadow_size > 0; }
    // Turns on the AO and the distance blur for this frame (the modern lighting's 3D view); call before begin_overlay.
    void set_ao_view(const AoView& view) { ao_view_ = view; }
    // The sun's shafts for this frame (post.frag marches from each pixel toward the sun over the depth: the sky lets
    // the light through, everything else blocks it); call before begin_overlay.
    void set_sun_shafts(const SunShafts& shafts) { shafts_ = shafts; }
    // The fog bank past the zone's edge (render/zone_fog.h): its field, uploaded (an invalid one clears it; the zone's,
    // at zone-in), and this frame's view, drawn over the scene as it ends in frames that also gave set_ao_view (the
    // depth's terms are the AO view's); call before begin_overlay.
    void set_zone_fog_field(const ZoneFogField& field);
    void set_zone_fog(const ZoneFogView& view) { zone_fog_view_ = view; }
    // This frame's colour grade (render/atmosphere.h ColorGrade: rgb a tint of the linear scene before the tonemap,
    // w the saturation after it); none, or with SceneOptions::color_grade off, leaves the picture as it is.
    void set_color_grade(const glm::vec4& mood) { mood_ = mood; }
    // The temporal upscaler (SceneOptions::upscaler Taa): the NDC offset to put on this frame's scene projection
    // (render/temporal.h jittered), zero without it. Valid from begin_frame_commands. Picking, the name plates and the
    // sun's place on the screen take the projection without it.
    glm::vec2 projection_jitter() const { return jitter_ndc_; }
    // This frame's camera without the jitter: the motion vectors and the history follow it. A frame that does not give
    // it is drawn without the temporal upscaler (FSR 1 below a render scale of 1) and the history is dropped. Call
    // before begin_overlay.
    void set_temporal_view(const glm::mat4& view, const glm::mat4& proj) { temporal_view_ = TemporalView{view, proj}; }
    // Drops the history at the next frame: what it holds is no longer on the screen (a new zone).
    void reset_history() { history_valid_ = false; }
    // The upscaler provider that made it through the device's creation (SceneOptions::upscaler External runs it), or
    // null.
    const UpscalerProvider* upscaler_provider() const { return provider_.get(); }
    const UpscalerProvider* active_upscaler_provider() const {
        return scene_options_.upscaler == Upscaler::External && !provider_failed_ ? provider_.get() : nullptr;
    }
    // The temporal upscaler runs (SceneOptions::upscaler Taa, Fsr3 or External, its targets made).
    bool temporal() const { return lit_.image != VK_NULL_HANDLE; }
    // Frame generation runs (SceneOptions::frame_gen under a temporal upscaler, its images made).
    bool frame_gen_active() const { return frame_gen_ && overlay_.image != VK_NULL_HANDLE; }
    // Presented frames per real frame over the last frame: 2 when it generated one, else 1 (the performance window).
    std::uint32_t frames_presented() const { return presented_last_; }
    // Whether the last frame's second image was an in-between frame (else, with two, the real frame again).
    bool frame_generated() const { return fg_generated_last_; }
    // Frame generation's counters, pacing and GPU time (the performance window).
    FrameGenStats frame_gen_stats() const;
    // The passes' GPU time (render/gpu_timer.h), for the performance window: on only while it is asked for (the
    // window open) and the device can time. gpu_mark ends a section after the commands recorded so far; outside a
    // frame, while off, or past kMaxGpuMarks it does nothing. gpu_sections is the last frame read back, and
    // gpu_sections_serial counts the frames read, so a reader adds each one once.
    void set_gpu_timing(bool on) { gpu_timing_ = on && timestamp_capable(); }
    bool timestamp_capable() const { return timestamp_period_ns_ > 0.0f && !gpu_queries_failed_; }
    void gpu_mark(const FrameInfo& frame, const char* name);
    std::span<const GpuSection> gpu_sections() const { return gpu_sections_; }
    std::uint64_t gpu_sections_serial() const { return gpu_sections_serial_; }
    // Scene pipelines are made for the scene's colour attachments: the colour alone, or under a temporal upscaler
    // (scene_motion()) the colour, the object motion (kMotionFormat) and the reactive mask (kReactiveFormat), as
    // begin_scene renders into. A renderer drawing into targets of its own (the item previews) passes `scene` false
    // for the colour alone. Decided by set_scene_options, before the renderers are made, as the sample count.
    // Without independentBlend (DeviceCaps) the temporal upscaler runs on the camera's motion alone.
    bool scene_motion() const { return scene_options_.upscaler != Upscaler::Fsr1 && caps_.independent_blend; }
    VkPipelineRenderingCreateInfo scene_rendering(bool scene = true) const;
    // This frame's scene takes its draws from secondary command buffers (render/scene_recorder.h): set before
    // begin_scene, cleared when the scene ends (begin_overlay). The scene's renderings then hold nothing but
    // vkCmdExecuteCommands.
    void set_scene_secondary(bool on) { scene_secondary_ = on; }
    bool scene_secondary() const { return scene_secondary_; }
    // A secondary command buffer's inheritance for the scene's rendering: its attachments' formats and samples.
    VkCommandBufferInheritanceRenderingInfo scene_inheritance() const;
    // The scene's dynamic state as begin_scene sets it (the flipped viewport, the scissor), into `cmd`: each secondary
    // command buffer of the scene starts with it.
    void set_scene_state(VkCommandBuffer cmd) const;
    SceneBlends scene_blends(const VkPipelineColorBlendAttachmentState& color, SceneOutput output,
                             bool scene = true) const;
    std::uint32_t shadow_size() const { return shadow_size_; }
    // Counts the shadow maps made: a new one holds nothing (the cover and water maps are drawn again).
    std::uint32_t shadow_generation() const { return shadow_generation_; }
    VkFormat shadow_format() const { return shadow_format_; }
    // The point shadow map (render/point_shadows.h): 2 x light_shadows cubes of six layers each, the slots' caches
    // then their live cubes, light_shadow_size texels a side; 0 slots when off.
    std::uint32_t point_shadow_slots() const { return point_slots_; }
    std::uint32_t point_shadow_size() const { return point_size_; }
    // Counts the point shadow maps made: a new one holds no cubes.
    std::uint32_t point_shadow_generation() const { return point_generation_; }
    // Depth-only rendering into face `face` of cube `cube`, as begin_shadow_cascade, without a depth bias (the
    // shaders write the distance); cleared to 1 or, with `clear` false, drawn over what it holds.
    void begin_point_shadow(const FrameInfo& frame, std::uint32_t cube, std::uint32_t face, bool clear);
    void end_point_shadow(const FrameInfo& frame);
    // Copies cube `from`'s six faces into cube `to` (a slot's cache into its live cube), outside rendering.
    void copy_point_shadow(const FrameInfo& frame, std::uint32_t from, std::uint32_t to);
    // The shadow map as the scene shaders sample it: binding 0, a sampler2DArrayShadow (fragment stage); binding 1
    // the scene's colour and binding 2 its depth as begin_translucent copied them (sampler2D, read in this frame
    // only after begin_translucent); binding 3 the shadow map's depths themselves (sampler2DArray, nearest: the water
    // map's); binding 4 the point shadow map's faces (sampler2DArrayShadow).
    VkDescriptorSetLayout shadow_set_layout() const { return shadow_set_layout_; }
    VkDescriptorSet shadow_set() const { return shadow_set_; }
    FrameOutcome end_frame(const FrameInfo& frame);

    // Call once before the scene renderers are made (their pipelines take the
    // sample count); later calls change the tonemap and exposure, and the
    // sample count only for renderers made after them. The post-process
    // shaders are read from shader_dir.
    void set_scene_options(const SceneOptions& options, const std::filesystem::path& shader_dir);
    // The 3D view's rectangle on the window's image, in pixels (/viewport); an empty one is the whole image. The scene
    // target takes its size (made again when that changes, at the next frame); the post pass draws it into the
    // rectangle and clears the rest of the image to black, under the UI.
    void set_scene_rect(const VkRect2D& rect) { scene_rect_ = rect; }
    const SceneOptions& scene_options() const { return scene_options_; }

    // Vertical sync: Off (immediate, else mailbox), On (FIFO) or Adaptive (FIFO relaxed: tears only when a frame is
    // late); a mode the surface lacks falls back to FIFO. The swapchain is rebuilt on the next frame.
    enum class Vsync : std::uint8_t { Off, On, Adaptive };
    void set_vsync(Vsync v) {
        if (v == vsync_) return;
        vsync_ = v;
        resize_pending_ = true;
    }
    Vsync vsync() const { return vsync_; }

    // Records commands into a one-off command buffer and waits for them to finish: for work whose result the CPU
    // needs at once. Uploads go through submit_upload.
    void immediate_submit(const std::function<void(VkCommandBuffer)>& record);
    // Records commands into a one-off command buffer and submits them on the frame's queue without waiting: queue
    // order (and the barriers the commands end with) makes what they write visible to every frame submitted after,
    // the one being recorded included. Returns the upload's serial, for retire_after_upload; `staging_bytes` is what
    // the upload keeps alive until it is done (past kMaxStagingInFlight in flight, the oldest uploads are waited for).
    std::uint64_t submit_upload(const std::function<void(VkCommandBuffer)>& record, VkDeviceSize staging_bytes);
    // The staging the uploads in flight hold (host memory the GPU has not read yet).
    std::uint64_t upload_staging_in_flight() const { return uploads_.bytes_in_flight(); }
    // Frees the staging of the uploads the GPU is done with, waiting for the oldest when more than `budget_bytes`
    // is in flight (0: never wait). The frame loop releases what is done each frame; a long build (the zone's)
    // calls this too, so the staging of what it has already sent does not pile up until the first frame.
    void release_finished_uploads(std::uint64_t budget_bytes = 0);
    // Runs `release` (destroying Vulkan objects) once the GPU is done with everything submitted so far and the frame
    // being recorded: for what a frame in flight, or an upload, may still use. `frames_ahead`: frames not begun yet
    // that may use it too (1 for the UI's textures: ImGui's draw lists are built before their frame begins). Main
    // thread only.
    void retire(std::function<void()> release, std::uint32_t frames_ahead = 0);
    // Runs `release` once upload `serial` (submit_upload's) is done: its staging buffers.
    void retire_after_upload(std::uint64_t serial, std::function<void()> release);
    // Waits for the device to be idle; what was retired is released.
    void wait_idle();
    static constexpr VkDeviceSize kMaxStagingInFlight = VkDeviceSize{256} << 20;
    // The frame slot the next begin_frame_commands uses (per-frame resources picked before it, by the UI).
    std::uint32_t next_frame_slot() const { return frame_index_; }
    // How many frames begin_frame_commands has begun (the one being recorded included). When it reads n, every frame
    // up to n - kFramesInFlight is done on the GPU (its slot's fence was waited for).
    std::uint64_t frames_begun() const { return frames_begun_; }

    // Saves the next presented frame as a PNG (waits for the GPU on that frame); with `max_width`, a frame wider
    // than that is shrunk by a whole factor (averaged) until it fits. False (and nothing is saved) when the swapchain's
    // images cannot be copied from (the surface does not offer TRANSFER_SRC).
    bool request_screenshot(std::filesystem::path path, std::uint32_t max_width = 0);
    // With frame generation, the screenshot saves the frame's in-between frame instead of the real one (the viewer's
    // --screenshot-generated; the real one when the frame made none).
    void screenshot_generated(bool on) { screenshot_generated_ = on; }
    // The swapchain's images can be read back (screenshots).
    bool can_screenshot() const { return screenshots_supported_; }
    // The GPU and its driver, for bug reports: "AMD Radeon RX 7900 XTX (radv, Mesa 25.2.1), Vulkan 1.4.321".
    const std::string& gpu_description() const { return gpu_description_; }
    // The GPU's name alone ("AMD Radeon RX 7900 XTX").
    const std::string& gpu_name() const { return gpu_name_; }
    // The GPU in use, for the first start's graphics settings (render/graphics_tier.h); cpu_threads left 0.
    const GpuProfile& gpu_profile() const { return gpu_profile_; }
    // FSR 3.1's passes' mean GPU times (HAILSTONE_FSR3_TIMING); empty without them.
    std::string fsr3_timing() const;
    // Each memory heap's use and budget as the allocator sees them (VMA's heap budgets), for the performance window.
    struct HeapUse {
        bool device_local = false; // VRAM, else system memory
        std::uint64_t usage = 0, budget = 0; // the process's use of the heap, and how much it may use
        std::uint64_t allocated = 0;         // what the allocator's blocks hold
        std::uint32_t allocations = 0;
    };
    std::vector<HeapUse> heap_use() const;

    // The pipeline cache every pipeline is made with: read from cache/pipelines.bin when the device is made and
    // written back when the context goes (render/pipeline_cache.h).
    VkPipelineCache pipeline_cache() const { return pipeline_cache_; }
    // The scene's depth the post pass reads (the resolved one under MSAA), in shader-read layout from begin_overlay to
    // the end of the frame, and the nearest-clamped sampler for it: the name plates test their text against it.
    VkImageView overlay_depth_view() const { return (depth_resolved_.image ? depth_resolved_ : depth_).view; }
    VkSampler post_sampler() const { return post_sampler_; }

    VkInstance instance() const { return instance_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    std::uint32_t queue_family() const { return queue_family_; }
    // Swapchain image counts; they may change when the swapchain is recreated.
    std::uint32_t image_count() const { return static_cast<std::uint32_t>(images_.size()); }
    std::uint32_t min_image_count() const { return min_image_count_; }
    VkPhysicalDevice physical_device() const { return physical_; }
    VmaAllocator allocator() const { return allocator_; }
    GpuSharedCache* gpu_shared_cache() const { return gpu_shared_cache_.get(); }
    // Called (between frames, the device idle) after shared textures moved into this client's own images when
    // sharing stopped: a holder of Texture copies refreshes them (refresh_shared_texture) and rewrites its
    // descriptors. Returns an id for remove_shared_textures_listener.
    std::uint64_t on_shared_textures_moved(std::function<void()> refresh);
    void remove_shared_textures_listener(std::uint64_t id);
    // Coverage diagnostics for the explicit immutable upload path; not native GPU-memory measurements.
    void note_shared_gpu_texture(bool imported);
    VkFormat color_format() const { return swapchain_format_; } // the swapchain: UI pipelines
    VkFormat scene_format() const { return kSceneFormat; }      // scene pipelines
    VkSampleCountFlagBits scene_samples() const { return scene_samples_; }
    VkFormat depth_format() const { return depth_format_; }
    const DeviceCaps& caps() const { return caps_; }
    // The fallbacks HAILSTONE_GPU_FALLBACKS forces (render/gpu_fallbacks.h); none for a player.
    const GpuFallbacks& forced_fallbacks() const { return fallbacks_; }
    // What a set holding a bindless texture array adds (DeviceCaps::update_after_bind): to the array's binding, its
    // layout and the pool it comes from. The array's binding also takes VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT.
    VkDescriptorBindingFlags texture_array_binding_flags() const {
        return caps_.update_after_bind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0;
    }
    VkDescriptorSetLayoutCreateFlags texture_array_layout_flags() const {
        return caps_.update_after_bind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
    }
    VkDescriptorPoolCreateFlags texture_array_pool_flags() const {
        return caps_.update_after_bind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
    }
    VkExtent2D swapchain_extent() const { return extent_; } // the window's size in pixels

private:
    static constexpr VkFormat kSceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kAoFormat = VK_FORMAT_R8_UNORM;
    // FSR 1's pictures: tonemapped and sRGB-encoded, in 10 bits so the steps do not band before the dither.
    static constexpr VkFormat kUpscaleFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    // The temporal upscaler's motion vectors: UV to last frame's place (shaders/temporal_prep.frag), and in blue the
    // depth of a see-through part drawn there with its own motion (SceneOutput::MotionReactive; 0 none), which writes
    // no depth: frame generation takes it as the nearer surface there.
    static constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kReactiveFormat = VK_FORMAT_R8_UNORM;
    std::array<VkFormat, 3> scene_color_formats_{kSceneFormat, kMotionFormat, kReactiveFormat};
    // The temporal block the scene shaders read (shadow_set() binding 5, shaders/motion.glsl), written into the
    // frame's commands by begin_scene.
    struct TemporalBlock {
        glm::mat4 view_proj{1.0f};      // this frame's, unjittered
        glm::mat4 prev_view_proj{1.0f}; // the last frame's, unjittered (this frame's when there is no history)
        glm::vec4 info{0.0f};           // x: 1 when the motion attachments are drawn
    };
    VkBuffer temporal_buffer_ = VK_NULL_HANDLE;
    VmaAllocation temporal_alloc_ = VK_NULL_HANDLE;
    void write_temporal_block(VkCommandBuffer cmd);
    static constexpr std::uint32_t kBloomLevels = 6; // 1/2 .. 1/64 of the screen

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer shared_acquire = VK_NULL_HANDLE;
        VkCommandBuffer shared_release = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkSemaphore generated_available = VK_NULL_HANDLE; // frame generation's second image
        VkFence in_flight = VK_NULL_HANDLE;
    };

    void create_instance(bool validation);
    void pick_physical_device();
    void create_device();
    void create_pipeline_cache();
    void save_pipeline_cache_file();
    // `caps`: the surface's capabilities when the caller has just asked for them (recreate_swapchain), else asked here.
    void create_swapchain(VkSwapchainKHR old = VK_NULL_HANDLE, const VkSurfaceCapabilitiesKHR* caps = nullptr);
    void destroy_swapchain();
    // False, with nothing let go and resize_pending_ left set, while the surface has no size to make one at
    // (render/swapchain_size.h surface_extent_empty): the frame is skipped.
    bool recreate_swapchain();
    void create_frames();
    void localize_shared_textures();
    void create_scene_targets();
    void destroy_scene_targets();
    void create_post_pipeline(const std::filesystem::path& shader_dir);
    void write_post_descriptors();
    void destroy_post_pipeline();
    void draw_zone_fog(const FrameInfo& frame, const AoView& view, float depth_x, float depth_y);
    void draw_bloom(VkCommandBuffer cmd);
    void create_shadow_resources();
    void create_shadow_map(std::uint32_t size);
    void destroy_shadow_map();
    void create_point_shadow_map(std::uint32_t size, std::uint32_t slots);
    void destroy_point_shadow_map();
    void write_water_descriptors();
    void begin_scene_rendering(VkCommandBuffer cmd, bool load, const glm::vec4& clear_color);
    bool scene_secondary_ = false; // set_scene_secondary

    platform::Window& window_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    DeviceCaps caps_;
    std::unique_ptr<GpuSharedCache> gpu_shared_cache_;
    bool gpu_shared_requested_ = false;
    std::uint64_t gpu_shared_bindings_ = 0;
    std::uint64_t gpu_shared_fallbacks_ = 0;
    std::uint64_t gpu_shared_frames_ = 0;
    bool shared_first_logged_ = false;
    bool shared_detach_logged_ = false;
    std::map<std::uint64_t, std::function<void()>> shared_texture_listeners_;
    std::uint64_t shared_texture_listener_ids_ = 0;
    bool gpu_benchmark_ = false;
    std::uint64_t benchmark_present_submissions_ = 0;
    std::chrono::steady_clock::time_point gpu_benchmark_start_{};
    double benchmark_lease_wait_ms_ = 0.0;
    double benchmark_shared_fence_wait_ms_ = 0.0;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    PipelineCacheDevice cache_device_;
    std::filesystem::path pipeline_cache_path_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkRect2D scene_rect_{};  // set_scene_rect's
    VkRect2D scene_area_{};  // the rectangle drawn this frame (scene_rect_ within the image)
    VkExtent2D view_extent_{};  // scene_area_'s size when the scene targets were made
    SceneResizeDebounce scene_resize_; // when a new scene_rect_ size makes them again
    VkExtent2D scene_extent_{}; // the scene targets' size: view_extent_ at the render scale
    bool upscaling() const { return scene_extent_.width != view_extent_.width || scene_extent_.height != view_extent_.height; }
    VkRect2D scene_area() const;
    std::uint32_t min_image_count_ = 2;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    // One per swapchain image: presentation may still use a semaphore after
    // the frame's fence signalled, so these cannot be per frame in flight.
    std::vector<VkSemaphore> render_finished_;

    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    Image create_target(VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                        VkImageAspectFlags aspect, VkExtent2D extent = {});
    void destroy_target(Image& image);
    // One full-screen triangle of `pipeline` into all of `target` (`extent`), which is left readable by fragment
    // shaders; `push` is the pipeline's push constants.
    void draw_into(VkCommandBuffer cmd, const Image& target, VkExtent2D extent, VkPipeline pipeline,
                   VkPipelineLayout layout, VkDescriptorSet set, const void* push, std::uint32_t push_size);
    // The same into several targets of one size at once (the pipeline's colour attachments, in order).
    static constexpr std::size_t kMaxDrawTargets = 4;
    void draw_into(VkCommandBuffer cmd, std::span<const Image* const> targets, VkExtent2D extent, VkPipeline pipeline,
                   VkPipelineLayout layout, VkDescriptorSet set, const void* push, std::uint32_t push_size);

    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    GpuFallbacks fallbacks_;
    SceneOptions scene_options_;
    VkSampleCountFlagBits scene_samples_ = VK_SAMPLE_COUNT_1_BIT;
    Image scene_msaa_;     // the multisampled colour target (msaa > 1 only)
    Image scene_resolved_; // the single-sampled scene the post pass reads
    Image depth_;          // scene_samples_ samples
    Image depth_resolved_; // its sample 0, for the AO and the post pass (msaa > 1 only)
    Image ao_;             // the ambient occlusion, R8
    std::optional<AoView> ao_view_;
    std::optional<SunShafts> shafts_;
    std::optional<ZoneFogView> zone_fog_view_;
    std::optional<glm::vec4> mood_;
    // With a render scale below 1 (upscaling()): the post pass's picture at the scene's size, and FSR 1's upscale of it
    // at the view's, which fsr_rcas.frag sharpens into the swapchain.
    Image upscale_source_;
    Image upscaled_;
    // The temporal upscaler (SceneOptions::upscaler Taa): temporal_prep.frag's colour (the scene with its AO) and
    // motion at the scene's size, and taa_resolve.frag's two histories at the view's (written in turn: the frame
    // writes history_[history_index_] and reads the other). upscaled_ is made too, for RCAS.
    Image lit_;
    Image motion_;
    // Under scene_motion(), the scene's second and third colour attachments: what the draws wrote of their own motion
    // (kMotionUnset elsewhere) and the reactive mask (0 .. 1: how much a pixel is made of draws without motion).
    Image object_motion_;
    Image reactive_;
    std::array<Image, 2> history_{};
    // Under FSR 3.1 (SceneOptions::upscaler Fsr3) or a provider (External): its output at the view's size (RGBA16F,
    // storage; with a provider also a transfer destination), which the post pass reads (post_fsr3_set_), and FSR 3.1
    // itself (made when the option first asks for it).
    Image fsr3_output_;
    std::unique_ptr<Fsr3Upscaler> fsr3_;
    // The upscaler provider (External), from the constructor; dropped when it fails (provider_failed_ then has the
    // next scene targets fall back to Taa).
    std::unique_ptr<UpscalerProvider> provider_;
    bool provider_failed_ = false;
    bool provider_fallback_logged_ = false;
    std::chrono::steady_clock::time_point provider_last_{}; // the last provider frame's time (its frame time)
    void drop_provider(std::string_view why);
    // Records the provider's frame (draw's resolve, in FSR 3.1's place) into fsr3_output_.
    void run_provider(VkCommandBuffer cmd, const FrameInfo& frame, const Image& depth, bool keep, std::uint32_t phases,
                      float near_plane, float far_plane, float fov_y, float exposure, float sharpness,
                      std::chrono::steady_clock::time_point now);
    // Frame generation (frame_gen_active()): the two pictures without the UI (this frame's is pictures_[picture_index_],
    // the last one's the other), the in-between frame, and the UI's image (the window's size, the swapchain's format)
    // that the overlay pass draws into instead of the swapchain; end_frame composites them (finish_frame_gen).
    std::array<Image, 2> pictures_{};
    Image generated_;
    Image overlay_;
    std::unique_ptr<FrameGen> frame_gen_;
    std::uint32_t picture_index_ = 0;
    bool picture_ready_ = false;   // the last frame left its picture, at this size
    bool overlay_open_ = false;    // begin_overlay rendered into overlay_ this frame
    bool fg_temporal_ = false;     // this frame ran the temporal upscaler
    bool fg_history_kept_ = false; // and its history went on from the last frame's
    bool fg_camera_steady_ = true; // and the camera moved less than kFrameGenMaxStep since the last frame
    std::uint32_t presented_last_ = 1;
    bool fg_generated_last_ = false;
    bool frame_gen_fallback_logged_ = false;
    bool frame_gen_requested_ = false; // SceneOptions::frame_gen as asked (scene_options_ drops it under FSR 1)
    FrameGenStats fg_stats_;           // counters and the last measurements (frame_gen_stats fills in the rest)
    const char* present_mode_ = "";    // the swapchain's present mode
    // Two timestamps per frame in flight round finish_frame_gen's GPU work (none where the device cannot time).
    VkQueryPool fg_queries_ = VK_NULL_HANDLE;
    float timestamp_period_ns_ = 0.0f;  // VkPhysicalDeviceLimits::timestampPeriod, 0: no timestamps
    std::array<std::uint8_t, kFramesInFlight> fg_query_state_{}; // 0 unwritten, 1 a real frame, 2 with an in-between
    void read_frame_gen_time(std::uint32_t frame_slot);
    // The passes' timestamps (gpu_mark): kMaxGpuMarks + 1 a frame in flight, the first the frame's start.
    VkQueryPool gpu_queries_ = VK_NULL_HANDLE;
    bool gpu_timing_ = false;
    bool gpu_queries_failed_ = false; // the pool could not be made: no pass timing
    std::uint32_t timestamp_bits_ = 0; // the queue family's timestampValidBits
    std::array<std::vector<const char*>, kFramesInFlight> gpu_marks_{}; // each slot's names, its timestamps 1..n
    std::array<bool, kFramesInFlight> gpu_started_{};                   // its start timestamp written
    std::vector<GpuSection> gpu_sections_;
    std::uint64_t gpu_sections_serial_ = 0;
    void read_gpu_marks(std::uint32_t frame_slot);
    VkPipeline fg_synth_pipeline_ = VK_NULL_HANDLE;
    VkPipeline fg_composite_pipeline_ = VK_NULL_HANDLE;
    VkPipeline rcas_picture_pipeline_ = VK_NULL_HANDLE; // RCAS into a picture (kUpscaleFormat), not the swapchain
    void create_frame_gen(const std::filesystem::path& shader_dir);
    void destroy_frame_gen();
    void bind_frame_gen();
    // Ends a frame-generation frame: the real frame composited into its image and, when one can be made, the
    // in-between frame into a second image acquired here. Returns that image's index, or nullopt. `readback`: that
    // image may be copied for a screenshot next (end_frame), so its move to PRESENT_SRC is chained to the copy stage.
    std::optional<std::uint32_t> finish_frame_gen(const FrameInfo& frame, bool readback);
    void composite_into(VkCommandBuffer cmd, std::uint32_t image_index, VkDescriptorSet set);
    std::chrono::steady_clock::time_point fsr3_last_{}; // the last FSR 3.1 frame's time (its frame time)
    bool fsr3_fallback_logged_ = false;
    struct TemporalView {
        glm::mat4 view{1.0f};
        glm::mat4 proj{1.0f};
    };
    std::optional<TemporalView> temporal_view_; // set_temporal_view's, this frame's
    TemporalView prev_view_;                    // the last temporal frame's
    bool history_valid_ = false;
    std::uint32_t history_index_ = 0;
    glm::vec2 jitter_px_{0.0f};  // this frame's jitter, scene pixels
    glm::vec2 jitter_ndc_{0.0f}; // the same in NDC (projection_jitter)
    Image water_color_; // begin_translucent's copies: the scene's colour,
    Image water_depth_; // and its depth (sample 0)
    std::array<Image, kBloomLevels> bloom_{};
    std::array<VkExtent2D, kBloomLevels> bloom_extent_{};
    bool scene_open_ = false; // rendering into the scene target is begun

    VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool post_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet post_set_ = VK_NULL_HANDLE;
    VkSampler post_sampler_ = VK_NULL_HANDLE;
    VkSampler post_linear_sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
    VkPipeline post_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ao_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet ao_set_ = VK_NULL_HANDLE;
    VkPipelineLayout ao_layout_ = VK_NULL_HANDLE;
    VkPipeline ao_pipeline_ = VK_NULL_HANDLE;
    // The zone's fog (set_zone_fog_field): its own pool, a set per field (0 the depth, 1 the field), retired with it.
    VkDescriptorSetLayout zone_fog_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool zone_fog_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet zone_fog_set_ = VK_NULL_HANDLE;
    VkPipelineLayout zone_fog_layout_ = VK_NULL_HANDLE;
    VkPipeline zone_fog_pipeline_ = VK_NULL_HANDLE;
    Texture zone_fog_texture_;
    glm::vec4 zone_fog_grid_{0.0f};  // the field's corner, 1 / its size
    glm::vec2 zone_fog_floor_{0.0f}; // its lowest floor, the floors' range
    // The bloom passes read through the AO's set layout (one sampler): set 0 the scene, set i + 1 level i.
    std::array<VkDescriptorSet, kBloomLevels + 1> bloom_sets_{};
    VkPipelineLayout bloom_layout_ = VK_NULL_HANDLE;
    VkPipeline bloom_down_pipeline_ = VK_NULL_HANDLE;
    VkPipeline bloom_up_pipeline_ = VK_NULL_HANDLE;
    std::uint32_t post_frame_ = 0; // the dither's frame index
    // FSR 1 (upscaling()): post.frag into upscale_source_, fsr_easu.frag up into upscaled_, fsr_rcas.frag into the
    // swapchain; the two read through the AO's set layout.
    VkPipeline post_upscale_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout upscale_layout_ = VK_NULL_HANDLE;
    VkPipeline easu_pipeline_ = VK_NULL_HANDLE;
    VkPipeline rcas_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet easu_set_ = VK_NULL_HANDLE;
    VkDescriptorSet rcas_set_ = VK_NULL_HANDLE;
    // The temporal upscaler: temporal_prep.frag reads the post set (the scene, the AO, the depth) into lit_ and
    // motion_; taa_resolve.frag reads resolve_sets_[i] (lit_, motion_, the depth, the history not written, through the
    // post set's layout) into history_[i]; post.frag then reads post_taa_sets_[i] (history_[i] in the scene's place).
    VkPipeline prep_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout prep_layout_ = VK_NULL_HANDLE;
    VkPipeline resolve_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout resolve_layout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> resolve_sets_{};
    std::array<VkDescriptorSet, 2> post_taa_sets_{};
    VkDescriptorSet post_fsr3_set_ = VK_NULL_HANDLE; // post.frag with fsr3_output_ in the scene's place

    VkFormat shadow_format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t shadow_size_ = 0;
    std::uint32_t shadow_generation_ = 0;
    VkImage shadow_image_ = VK_NULL_HANDLE;
    VmaAllocation shadow_alloc_ = VK_NULL_HANDLE;
    VkImageView shadow_array_view_ = VK_NULL_HANDLE; // every cascade, for sampling
    std::array<VkImageView, kShadowLayers> shadow_layer_views_{}; // one per layer, for rendering
    std::uint32_t shadow_open_ = 0; // the cascade begin_shadow_cascade began
    VkImage point_image_ = VK_NULL_HANDLE;
    VmaAllocation point_alloc_ = VK_NULL_HANDLE;
    VkImageView point_array_view_ = VK_NULL_HANDLE;    // every face, for sampling
    std::vector<VkImageView> point_layer_views_;       // one per face, for rendering
    std::uint32_t point_size_ = 0;
    std::uint32_t point_slots_ = 0;
    std::uint32_t point_open_ = 0; // the layer begin_point_shadow began
    std::uint32_t point_generation_ = 0;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;
    VkSampler water_sampler_ = VK_NULL_HANDLE;       // linear, clamped: the colour copy
    VkSampler water_depth_sampler_ = VK_NULL_HANDLE; // nearest, clamped: the depth copy
    VkDescriptorSetLayout shadow_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool shadow_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet shadow_set_ = VK_NULL_HANDLE;

    std::array<Frame, kFramesInFlight> frames_{};
    std::uint32_t frame_index_ = 0;
    bool resize_pending_ = false;
    VkExtent2D made_for_{}; // the window's framebuffer size when the swapchain was made
    std::uint32_t surface_watch_ = 0; // frames left to compare the surface's currentExtent with extent_
    std::uint64_t frames_begun_ = 0;
    Vsync vsync_ = Vsync::On;

    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    // submit_upload's submissions in flight, what waits for them or for the frames (retire), and the last frame known
    // done on the GPU (render/retire_queue.h).
    struct Upload {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
    };
    UploadTracker<Upload> uploads_;
    std::vector<VkFence> free_fences_;
    RetireQueue retired_;
    std::uint64_t frames_done_ = 0;
    bool recording_ = false; // frame frames_begun_ is being recorded (begun, not yet submitted)
    void recycle_upload(const Upload& u);
    void collect_retired();
    bool screenshots_supported_ = false; // the swapchain was made with TRANSFER_SRC
    std::filesystem::path screenshot_path_;
    std::uint32_t screenshot_max_width_ = 0;
    bool screenshot_generated_ = false;
    std::string gpu_description_;
    std::string gpu_name_;
    GpuProfile gpu_profile_;
};

} // namespace hailstone::render
