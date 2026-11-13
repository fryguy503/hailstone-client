#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <volk.h>
#include <vk_mem_alloc.h>

#include "render/pipeline_cache.h"
#include "render/point_shadows.h"
#include "render/retire_queue.h"
#include "render/shadows.h"

namespace hailstone::platform { class Window; }

namespace hailstone::render {

class GpuSharedCache;

// Optional device capabilities the renderers adapt to.
struct DeviceCaps {
    bool bc_textures = false;  // BC1-3 sampled images (absent on some arm64 GPUs)
    // R8G8B8A8_SRGB images can be blitted with linear filtering: UploadBatch::texture builds an opaque texture's mips
    // on the GPU in linear light only then (render::gpu_builds_mips), and on the CPU otherwise.
    bool srgb_mip_blit = false;
    float max_anisotropy = 0.0f; // 0 when sampler anisotropy is unsupported
    // The textures one bindless array may hold (render/descriptor_limits.h), under the update-after-bind limits when
    // update_after_bind is set.
    std::uint32_t max_sampled_images = 0;
    // The texture arrays' sets are update-after-bind (descriptorBindingSampledImageUpdateAfterBind): their limits are
    // far higher on some drivers (Intel on Windows: 64 samplers per stage without, 1048576 with).
    bool update_after_bind = false;
    VkSampleCountFlags msaa_counts = VK_SAMPLE_COUNT_1_BIT; // colour and depth framebuffer sample counts
};

// How the scene is drawn and shown. The scene renders in linear colour into a floating-point target (multisampled
// when msaa > 1), which a full-screen pass tonemaps into the swapchain before the UI is drawn on top.
enum class Tonemap : std::uint8_t { None, AgX, Aces };

struct SceneOptions {
    std::uint32_t msaa = 4;         // samples per pixel; cut to what the device supports
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
    // The 3D view drawn at this share of its size on each side (display.render_scale, 0.5..1) and brought up to it by
    // FSR 1 (shaders/fsr_easu.frag, sharpened by fsr_rcas.frag at `sharpness`, 0 none .. 1 the most); 1 draws it
    // at its own size, without them.
    float render_scale = 1.0f;
    float sharpness = 0.5f;
};

// The scene targets' size for a 3D view of `view` pixels at `scale` (SceneOptions::render_scale, kept in 0.5..1):
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

// Parses display.tonemap's values (none, agx, aces); unknown names give None.
Tonemap parse_tonemap(std::string_view name);

// One frame being recorded. Rendering into the scene target (colour + depth) is already begun.
struct FrameInfo {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // The scene target's size, in pixels: the 3D view's (VkContext::set_scene_rect) times SceneOptions::render_scale.
    // Window coordinates (picking, the nameplates) take the view's rectangle instead.
    VkExtent2D extent{};
    VkOffset2D offset{};   // the 3D view's place on the window's image
    std::uint32_t image_index = 0;
    std::uint32_t frame_slot = 0; // 0..kFramesInFlight-1, for per-frame resources
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

    VkContext(platform::Window& window, bool validation);
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
    void begin_shadow_cascade(const FrameInfo& frame, std::uint32_t cascade);
    void end_shadow_cascade(const FrameInfo& frame);
    bool shadows_enabled() const { return scene_options_.shadow_size > 0; }
    // Turns on the AO and the distance blur for this frame (the modern lighting's 3D view); call before begin_overlay.
    void set_ao_view(const AoView& view) { ao_view_ = view; }
    // The sun's shafts for this frame (post.frag marches from each pixel toward the sun over the depth: the sky lets
    // the light through, everything else blocks it); call before begin_overlay.
    void set_sun_shafts(const SunShafts& shafts) { shafts_ = shafts; }
    // This frame's colour grade (render/atmosphere.h ColorGrade: rgb a tint of the linear scene before the tonemap,
    // w the saturation after it); none, or with SceneOptions::color_grade off, leaves the picture as it is.
    void set_color_grade(const glm::vec4& mood) { mood_ = mood; }
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
    void end_frame(const FrameInfo& frame);

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
    // The swapchain's images can be read back (screenshots).
    bool can_screenshot() const { return screenshots_supported_; }
    // The GPU and its driver, for bug reports: "AMD Radeon RX 7900 XTX (radv, Mesa 25.2.1), Vulkan 1.4.321".
    const std::string& gpu_description() const { return gpu_description_; }
    // The GPU's name alone ("AMD Radeon RX 7900 XTX").
    const std::string& gpu_name() const { return gpu_name_; }
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
    // Coverage diagnostics for the explicit immutable upload path; not native GPU-memory measurements.
    void note_shared_gpu_texture(bool imported);
    VkFormat color_format() const { return swapchain_format_; } // the swapchain: UI pipelines
    VkFormat scene_format() const { return kSceneFormat; }      // scene pipelines
    VkSampleCountFlagBits scene_samples() const { return scene_samples_; }
    VkFormat depth_format() const { return depth_format_; }
    const DeviceCaps& caps() const { return caps_; }
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
    static constexpr std::uint32_t kBloomLevels = 6; // 1/2 .. 1/64 of the screen

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer shared_acquire = VK_NULL_HANDLE;
        VkCommandBuffer shared_release = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
    };

    void create_instance(bool validation);
    void pick_physical_device();
    void create_device();
    void create_pipeline_cache();
    void save_pipeline_cache_file();
    void create_swapchain(VkSwapchainKHR old = VK_NULL_HANDLE);
    void destroy_swapchain();
    void recreate_swapchain();
    void create_frames();
    void create_scene_targets();
    void destroy_scene_targets();
    void create_post_pipeline(const std::filesystem::path& shader_dir);
    void write_post_descriptors();
    void destroy_post_pipeline();
    void draw_bloom(VkCommandBuffer cmd);
    void create_shadow_resources();
    void create_shadow_map(std::uint32_t size);
    void destroy_shadow_map();
    void create_point_shadow_map(std::uint32_t size, std::uint32_t slots);
    void destroy_point_shadow_map();
    void write_water_descriptors();
    void begin_scene_rendering(VkCommandBuffer cmd, bool load, const glm::vec4& clear_color);

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

    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    SceneOptions scene_options_;
    VkSampleCountFlagBits scene_samples_ = VK_SAMPLE_COUNT_1_BIT;
    Image scene_msaa_;     // the multisampled colour target (msaa > 1 only)
    Image scene_resolved_; // the single-sampled scene the post pass reads
    Image depth_;          // scene_samples_ samples
    Image depth_resolved_; // its sample 0, for the AO and the post pass (msaa > 1 only)
    Image ao_;             // the ambient occlusion, R8
    std::optional<AoView> ao_view_;
    std::optional<SunShafts> shafts_;
    std::optional<glm::vec4> mood_;
    // With a render scale below 1 (upscaling()): the post pass's picture at the scene's size, and FSR 1's upscale of it
    // at the view's, which fsr_rcas.frag sharpens into the swapchain.
    Image upscale_source_;
    Image upscaled_;
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
    std::string gpu_description_;
    std::string gpu_name_;
};

} // namespace hailstone::render
