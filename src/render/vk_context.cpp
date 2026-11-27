#define VOLK_IMPLEMENTATION
#include "render/vk_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "core/env.h"
#include "core/log.h"
#include "core/png_write.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/descriptor_limits.h"
#include "render/frame_gen_logic.h"
#include "render/frame_sync.h"
#include "render/gpu_requirements.h"
#include "render/gpu_shared_cache.h"
#include "render/swapchain_size.h"
#include "render/vk_util.h"
#include "assets/texture.h"

namespace hailstone::render {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        log::error("vulkan: {}", data->pMessage);
    else
        log::warn("vulkan: {}", data->pMessage);
    return VK_FALSE;
}

// VMA's view of the memory types under the non-coherent fallback (render/gpu_fallbacks.h): the driver's, with
// HOST_COHERENT dropped. A plain function pointer, so the choice is a file-level flag; one VkContext sets it before
// it creates its allocator.
bool g_hide_coherent = false;
VKAPI_ATTR void VKAPI_CALL memory_properties_without_coherent(VkPhysicalDevice device,
                                                             VkPhysicalDeviceMemoryProperties* properties) {
    vkGetPhysicalDeviceMemoryProperties(device, properties);
    GpuFallbacks forced;
    forced.non_coherent = g_hide_coherent;
    for (std::uint32_t i = 0; i < properties->memoryTypeCount; ++i)
        properties->memoryTypes[i].propertyFlags =
            memory_flags_seen(properties->memoryTypes[i].propertyFlags, forced);
}
static_assert(kHostCoherentBit == VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

bool has_layer(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

VkShaderModule load_module(VkDevice device, const std::filesystem::path& path) {
    return create_shader_module(device, path);
}

// shaders/post.frag's push constants.
struct PostPush {
    float exposure = 1.0f; // linear scale, 2^EV
    std::uint32_t tonemap = 0;
    std::uint32_t flags = 0;
    std::uint32_t frame = 0;
    glm::vec4 depth{0.0f}; // the projection's depth terms: view depth = y / (d + x); z: the AO's strength; w: far
    glm::vec4 grade{0.0f, 1.0f, 1.0f, 0.0f}; // brightness, contrast, gamma; w: the bloom's share (0 none)
    glm::vec4 sun{0.0f};       // the shafts: the sun's UV, strength (0 none)
    glm::vec4 sun_color{0.0f}; // their light, linear
    glm::vec4 offset{0.0f};    // the scene's place on the image (xy, pixels): gl_FragCoord - offset is its texel
    glm::vec4 mood{1.0f};      // the colour grade (set_color_grade): rgb the tint before the tonemap, w saturation
};
static_assert(sizeof(PostPush) <= 128);

// shaders/bloom_down.frag's and bloom_up.frag's push constants.
struct BloomPush {
    glm::vec2 texel{0.0f};  // one texel of the source, in UV
    glm::vec2 target{0.0f}; // one pixel of the level drawn, in UV
    std::uint32_t first = 0;
};

// shaders/fsr_easu.frag's and fsr_rcas.frag's push constants.
struct UpscalePush {
    glm::vec4 sizes{0.0f};  // EASU: the source's width and height, the output's width and height
    glm::vec4 offset{0.0f}; // RCAS: the 3D view's place on the image (xy, pixels)
    float sharpness = 0.0f; // RCAS: 0 none .. 1 the most
    std::uint32_t flags = 0; // RCAS: kPostEncodeOutput, kPostDither
    std::uint32_t frame = 0; // RCAS: the dither's
    std::uint32_t pad = 0;
};

// shaders/temporal_prep.frag's push constants.
struct PrepPush {
    glm::mat4 reproject{1.0f}; // this frame's clip space (unjittered) to the last frame's (render/temporal.h)
    glm::vec4 depth{0.0f};     // view depth terms x, y (as PostPush::depth); z: the AO's strength; w: 1 to apply it
    glm::vec4 jitter{0.0f};    // xy: this frame's jitter in NDC
};
static_assert(sizeof(PrepPush) <= 128);

// shaders/taa_resolve.frag's push constants.
struct ResolvePush {
    glm::vec4 sizes{0.0f};  // the scene's width and height, the view's
    glm::vec4 jitter{0.0f}; // xy: the jitter in scene pixels; z: the exposure (linear); w: 1 to drop the history
    glm::vec4 params{0.0f}; // x: the most weight the history keeps
};
// The history keeps at most this many frames' worth of weight: each new frame is at least 1/12 of the result.
constexpr float kHistoryMaxWeight = 12.0f;

// shaders/ao.frag's push constants.
struct AoPush {
    glm::vec4 projection{0.0f}; // tan_half_x, tan_half_y, depth terms x, y (as PostPush::depth)
    glm::vec4 params{0.0f};     // radius (world units), intensity, max radius in pixels, frame
};

// shaders/zone_fog.frag's (render/zone_fog.h ZoneFogView).
struct ZoneFogPush {
    glm::vec4 right{0.0f};   // xyz right x tan(half fov x); w depth term x
    glm::vec4 up{0.0f};      // xyz up x tan(half fov y); w depth term y
    glm::vec4 forward{0.0f}; // w the sky's march
    glm::vec4 camera{0.0f};  // w the lowest floor
    glm::vec4 color{0.0f};   // w the floors' range
    glm::vec4 glow{0.0f};
    glm::vec4 to_sun{0.0f};
    glm::vec4 grid{0.0f};    // the field's corner, 1 / its size
};
static_assert(sizeof(ZoneFogPush) == 128, "within the push constants every device has");

// A full-screen triangle into colour attachments of `formats` (shaders/post.vert with `fragment`), added onto what
// is there when `additive`.
VkPipeline fullscreen_pipeline(VkDevice device, VkPipelineCache cache, VkPipelineLayout layout, VkShaderModule vertex,
                               VkShaderModule fragment, std::span<const VkFormat> formats, bool additive = false,
                               const VkPipelineColorBlendAttachmentState* blend = nullptr) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xf;
    if (additive) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.srcAlphaBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (blend) ba = *blend;
    const std::vector<VkPipelineColorBlendAttachmentState> blends(formats.size(), ba);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<std::uint32_t>(blends.size());
    cb.pAttachments = blends.data();
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = static_cast<std::uint32_t>(formats.size());
    ri.pColorAttachmentFormats = formats.data();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &ri;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &dss;
    info.pColorBlendState = &cb;
    info.pDynamicState = &ds;
    info.layout = layout;
    VkPipeline pipeline;
    vk_check(vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline),
             "vkCreateGraphicsPipelines (full screen)");
    return pipeline;
}
VkPipeline fullscreen_pipeline(VkDevice device, VkPipelineCache cache, VkPipelineLayout layout, VkShaderModule vertex,
                               VkShaderModule fragment, VkFormat format, bool additive = false) {
    return fullscreen_pipeline(device, cache, layout, vertex, fragment, std::span<const VkFormat>(&format, 1), additive);
}
constexpr std::uint32_t kPostEncodeOutput = 2; // the swapchain is UNORM: encode sRGB in the shader
constexpr std::uint32_t kPostDither = 4;
constexpr std::uint32_t kPostAo = 8;    // apply the AO image
constexpr std::uint32_t kPostBlur = 16; // the distance blur

} // namespace

SunShafts sun_shafts_for(const glm::mat4& view_proj, const glm::vec3& to_sun, const glm::vec3& light) {
    SunShafts out;
    glm::vec4 clip = view_proj * glm::vec4(to_sun, 0.0f); // a direction: the point at infinity
    if (clip.w <= 1e-4f) return out;
    glm::vec2 ndc = glm::vec2(clip) / clip.w;
    out.uv = glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f); // the viewport is flipped
    float edge = std::max(std::abs(ndc.x), std::abs(ndc.y));
    auto smooth = [](float a, float b, float x) {
        float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    out.strength = (1.0f - smooth(1.0f, 2.0f, edge)) * smooth(-0.02f, 0.08f, to_sun.z);
    out.color = light;
    return out;
}

VkExtent2D scaled_extent(VkExtent2D view, float scale) {
    scale = std::clamp(scale, kMinRenderScale, 1.0f);
    auto side = [scale](std::uint32_t n) {
        return static_cast<std::uint32_t>(std::max(1L, std::lround(static_cast<double>(n) * scale)));
    };
    return {side(view.width), side(view.height)};
}

VkPipelineRenderingCreateInfo VkContext::scene_rendering(bool scene) const {
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = scene && scene_motion() ? 3u : 1u;
    ri.pColorAttachmentFormats = scene_color_formats_.data();
    ri.depthAttachmentFormat = depth_format_;
    return ri;
}

SceneBlends VkContext::scene_blends(const VkPipelineColorBlendAttachmentState& color, SceneOutput output,
                                    bool scene) const {
    return make_scene_blends(color, output, scene && scene_motion());
}

SceneBlends make_scene_blends(const VkPipelineColorBlendAttachmentState& color, SceneOutput output, bool motion_on) {
    SceneBlends out;
    out.states[0] = color;
    if (!motion_on) return out;
    out.count = 3;
    // The motion is written as it is (the last draw over a pixel is the one in front, as the depth test keeps it);
    // the reactive mask keeps the largest of what was drawn over the pixel.
    VkPipelineColorBlendAttachmentState motion{};
    // A see-through part with motion of its own also writes its depth in blue (kMotionFormat): it writes no depth.
    const bool see_through = output == SceneOutput::MotionReactive || output == SceneOutput::MotionReactiveWhereSeen;
    const bool writes_motion = output == SceneOutput::Motion || see_through;
    motion.colorWriteMask = writes_motion ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT : 0u;
    if (see_through) motion.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (output == SceneOutput::MotionReactiveWhereSeen) {
        // The shader's alpha is 0 or 1: the motion is replaced or kept, never mixed.
        motion.blendEnable = VK_TRUE;
        motion.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        motion.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        motion.colorBlendOp = VK_BLEND_OP_ADD;
        motion.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        motion.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        motion.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendAttachmentState reactive{};
    if (output == SceneOutput::Reactive || see_through) {
        reactive.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        reactive.blendEnable = VK_TRUE;
        reactive.srcColorBlendFactor = reactive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        reactive.srcAlphaBlendFactor = reactive.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        reactive.colorBlendOp = reactive.alphaBlendOp = VK_BLEND_OP_MAX;
    }
    out.states[1] = motion;
    out.states[2] = reactive;
    return out;
}

Tonemap parse_tonemap(std::string_view name) {
    if (name == "agx") return Tonemap::AgX;
    if (name == "aces") return Tonemap::Aces;
    if (name == "soft") return Tonemap::Soft;
    return Tonemap::None;
}

namespace {
std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}
} // namespace

VkContext::VkContext(platform::Window& window, bool validation, std::unique_ptr<UpscalerProvider> upscaler)
    : window_(window), provider_(std::move(upscaler)) {
    const char* benchmark = core::env("GPU_BENCHMARK");
    gpu_benchmark_ = benchmark && std::strcmp(benchmark, "1") == 0;
    // A developer's setting (docs/graphics.md "Forcing the GPU fallbacks"): unset for every player.
    if (const char* forced = core::env("GPU_FALLBACKS")) {
        const GpuFallbackParse parsed = parse_gpu_fallbacks(forced);
        for (const std::string& word : parsed.unknown)
            log::warn("HAILSTONE_GPU_FALLBACKS: no fallback is called \"{}\"", word);
        fallbacks_ = parsed.forced;
        if (fallbacks_.any()) log::warn("GPU fallbacks forced: {}", gpu_fallback_names(fallbacks_));
    }
    if (auto loader = window_.bundled_vulkan_loader())
        volkInitializeCustom(loader); // the MoltenVK a macOS release ships, as the window loaded it
    else
        vk_check(volkInitialize(), "volkInitialize (is a Vulkan driver installed?)");
    create_instance(validation);
    surface_ = window_.create_surface(instance_);
    pick_physical_device();
    create_device();
    if (provider_) {
        HailstoneUpscalerDevice d{};
        d.size = sizeof d;
        d.api_version = VK_API_VERSION_1_3;
        d.instance = instance_;
        d.physical_device = physical_;
        d.device = device_;
        d.queue = queue_;
        d.queue_family = queue_family_;
        // The reactive mask is drawn beside the colour only with independentBlend (scene_motion()).
        d.flags = caps_.independent_blend ? HAILSTONE_UPSCALER_DEVICE_REACTIVE : 0u;
        d.get_instance_proc_addr = reinterpret_cast<void*>(vkGetInstanceProcAddr);
        d.get_device_proc_addr = reinterpret_cast<void*>(vkGetDeviceProcAddr);
        if (provider_->create(d))
            log::info("upscaler {}: {} is ready", provider_->name(), provider_->label());
        else
            drop_provider("it did not start");
    }
    create_swapchain();
    create_frames();
    create_shadow_resources();
}

void VkContext::run_provider(VkCommandBuffer cmd, const FrameInfo& frame, const Image& depth, bool keep,
                             std::uint32_t phases, float near_plane, float far_plane, float fov_y, float exposure,
                             float sharpness, std::chrono::steady_clock::time_point now) {
    // What the provider reads was last written by draws; it may read it from any stage (compute, transfers), and its
    // output was last read by the post pass a frame ago.
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        m.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    image_barrier(cmd, fsr3_output_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    auto image = [](const Image& i, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkImageLayout layout,
                    VkImageAspectFlags aspect) {
        HailstoneUpscalerImage out{};
        if (!i.image) return out;
        out.image = reinterpret_cast<std::uint64_t>(i.image);
        out.view = reinterpret_cast<std::uint64_t>(i.view);
        out.format = static_cast<std::uint32_t>(format);
        out.width = extent.width;
        out.height = extent.height;
        out.usage = usage;
        out.layout = static_cast<std::uint32_t>(layout);
        out.aspect = aspect;
        return out;
    };
    constexpr VkImageUsageFlags kInput = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    HailstoneUpscalerFrame f{};
    f.size = sizeof f;
    f.flags = keep ? 0u : HAILSTONE_UPSCALER_FRAME_RESET;
    f.command_buffer = cmd;
    f.color = image(lit_, kSceneFormat, scene_extent_, kInput | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    f.depth = image(depth, depth_format_, scene_extent_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    f.motion = image(motion_, kMotionFormat, scene_extent_, kInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    f.reactive = image(reactive_, kReactiveFormat, scene_extent_, kInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    f.output = image(fsr3_output_, kSceneFormat, view_extent_,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
    f.jitter_x = jitter_px_.x;
    f.jitter_y = jitter_px_.y;
    f.jitter_phases = phases;
    f.frame_slot = frame.frame_slot;
    f.frames_in_flight = kFramesInFlight;
    f.motion_scale_x = static_cast<float>(scene_extent_.width);
    f.motion_scale_y = static_cast<float>(scene_extent_.height);
    f.near_plane = near_plane;
    f.far_plane = far_plane;
    f.fov_y = fov_y;
    f.exposure = exposure;
    f.frame_seconds = provider_last_.time_since_epoch().count() != 0
                          ? std::chrono::duration<float>(now - provider_last_).count()
                          : 1.0f / 60.0f;
    provider_last_ = now;
    f.sharpness = sharpness;
    if (!provider_->evaluate(f)) {
        // This frame's picture is whatever the output held; from the next one the client's own upscaler takes over
        // (create_scene_targets, with the device idle).
        provider_failed_ = true;
        resize_pending_ = true;
    }
    image_barrier(cmd, fsr3_output_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VkContext::drop_provider(std::string_view why) {
    if (!provider_) return;
    log::warn("upscaler {}: not used, {}; the client's own upscaler is used instead", provider_->name(), why);
    provider_.reset(); // destroys it when it was made: every caller has the device idle
}

VkContext::~VkContext() {
    if (device_) vkDeviceWaitIdle(device_);
    // Every upload is done and every retired object can go (before the allocator does).
    uploads_.all_done([this](const Upload& u) { recycle_upload(u); });
    retired_.flush();
    if (gpu_shared_requested_) {
        const auto stats = gpu_shared_cache_ ? gpu_shared_cache_->stats() : GpuSharedCache::Stats{};
        log::info("GPU asset sharing totals: shared bindings {}, unique imports {}, local hits {}, broker hits {}, "
                  "private fallbacks {}, shared frames {}, imported allocation bytes {}",
                  gpu_shared_bindings_, stats.imports, stats.local_hits, stats.broker_hits, gpu_shared_fallbacks_,
                  gpu_shared_frames_, stats.imported_allocation_bytes);
    }
    // Imported owners retired by the renderers are released before the importing device.
    gpu_shared_cache_.reset();
    if (gpu_queries_) vkDestroyQueryPool(device_, gpu_queries_, nullptr);
    for (VkFence f : free_fences_) vkDestroyFence(device_, f, nullptr);
    free_fences_.clear();
    if (upload_pool_) vkDestroyCommandPool(device_, upload_pool_, nullptr);
    for (auto& f : frames_) {
        if (f.in_flight) vkDestroyFence(device_, f.in_flight, nullptr);
        if (f.image_available) vkDestroySemaphore(device_, f.image_available, nullptr);
        if (f.generated_available) vkDestroySemaphore(device_, f.generated_available, nullptr);
        if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
    }
    destroy_swapchain();
    fsr3_.reset();
    provider_.reset();
    destroy_frame_gen();
    destroy_post_pipeline();
    destroy_shadow_map();
    destroy_point_shadow_map();
    if (shadow_pool_) vkDestroyDescriptorPool(device_, shadow_pool_, nullptr);
    if (shadow_set_layout_) vkDestroyDescriptorSetLayout(device_, shadow_set_layout_, nullptr);
    if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
    if (water_sampler_) vkDestroySampler(device_, water_sampler_, nullptr);
    if (water_depth_sampler_) vkDestroySampler(device_, water_depth_sampler_, nullptr);
    if (temporal_buffer_) vmaDestroyBuffer(allocator_, temporal_buffer_, temporal_alloc_);
    save_pipeline_cache_file();
    if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VkContext::create_instance(bool validation) {
    std::uint32_t version = 0;
    vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3)
        throw std::runtime_error("Vulkan 1.3 is required; the loader reports " +
                                 std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
                                 std::to_string(VK_API_VERSION_MINOR(version)));

    // What the window needs, as the library offers it (render/gpu_requirements.h: the portability opt-in only where
    // the loader has it, a missing one named).
    std::vector<std::string> wanted;
    for (const char* name : window_.required_instance_extensions()) wanted.emplace_back(name);
    std::uint32_t offered_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &offered_count, nullptr);
    std::vector<VkExtensionProperties> offered_props(offered_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &offered_count, offered_props.data());
    std::vector<std::string> offered;
    for (const VkExtensionProperties& e : offered_props) offered.emplace_back(e.extensionName);
    const InstanceExtensions chosen = choose_instance_extensions(wanted, offered);
    if (!chosen.missing.empty()) {
        std::string names;
        for (const std::string& name : chosen.missing) names += (names.empty() ? "" : ", ") + name;
        throw std::runtime_error("vkCreateInstance: the Vulkan library lacks " + names);
    }
    // An upscaler provider's own (render/upscaler_provider.h): one the loader lacks keeps it out, not the client.
    std::vector<std::string> provider_extensions;
    if (provider_) {
        std::uint32_t min_api = 0;
        if (!provider_->instance_requirements(provider_extensions, min_api))
            drop_provider("it cannot run here");
        else if (min_api > VK_API_VERSION_1_3)
            drop_provider(std::format("it needs Vulkan {}.{}", VK_API_VERSION_MAJOR(min_api), VK_API_VERSION_MINOR(min_api)));
        else if (const auto missing = missing_names(provider_extensions, offered); !missing.empty())
            drop_provider("the Vulkan library lacks " + joined(missing));
        if (!provider_) provider_extensions.clear();
    }
    std::vector<const char*> extensions;
    for (const std::string& name : chosen.enable) extensions.push_back(name.c_str());
    for (const std::string& name : provider_extensions)
        if (std::find(chosen.enable.begin(), chosen.enable.end(), name) == chosen.enable.end())
            extensions.push_back(name.c_str());
    std::vector<const char*> layers;
    VkInstanceCreateFlags flags = chosen.portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;

    if (validation) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            log::warn("validation requested but VK_LAYER_KHRONOS_validation is not installed");
            validation = false;
        }
    }
    {
        std::string names;
        for (const char* name : extensions) names += (names.empty() ? "" : " ") + std::string(name);
        log::info("Vulkan instance extensions: {}", names);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Hailstone";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "Hailstone";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    vk_check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    volkLoadInstance(instance_);

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        m.pfnUserCallback = debug_callback;
        vk_check(vkCreateDebugUtilsMessengerEXT(instance_, &m, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
    }
}

void VkContext::pick_physical_device() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Each GPU's support is kept so that, when none qualifies, the error names what each one lacks
    // (render/gpu_requirements.h); app/startup_error.h turns it into the player's dialog.
    std::vector<GpuSupport> seen;
    std::vector<std::uint32_t> queue_families(devices.size(), UINT32_MAX);
    for (std::size_t device_index = 0; device_index < devices.size(); ++device_index) {
        VkPhysicalDevice pd = devices[device_index];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        GpuSupport& gpu = seen.emplace_back();
        gpu.name = props.deviceName;
        gpu.kind = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? GpuKind::Discrete
                   : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GpuKind::Integrated : GpuKind::Other;
        gpu.api_major = VK_API_VERSION_MAJOR(props.apiVersion);
        gpu.api_minor = VK_API_VERSION_MINOR(props.apiVersion);
        // The 1.2 and 1.3 feature structs are only valid to query on a 1.3 device.
        if (props.apiVersion < VK_API_VERSION_1_3) continue;

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        gpu.dynamic_rendering = f13.dynamicRendering == VK_TRUE;
        gpu.synchronization2 = f13.synchronization2 == VK_TRUE;
        gpu.runtime_descriptor_array = f12.runtimeDescriptorArray == VK_TRUE;
        gpu.nonuniform_sampled_image_indexing = f12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
        gpu.descriptor_binding_partially_bound = f12.descriptorBindingPartiallyBound == VK_TRUE;
        gpu.descriptor_binding_variable_count = f12.descriptorBindingVariableDescriptorCount == VK_TRUE;
        gpu.separate_depth_stencil_layouts = f12.separateDepthStencilLayouts == VK_TRUE;

        // A queue that does graphics and presents to our surface.
        std::uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, families.data());
        std::uint32_t family = UINT32_MAX;
        for (std::uint32_t i = 0; i < qcount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { family = i; break; }
        }
        gpu.presents = family != UINT32_MAX;
        queue_families[device_index] = family;
    }
    const char* requested_name = core::env("GPU_NAME");
    const GpuSelection selected = choose_gpu(seen, requested_name ? requested_name : "");
    if (!selected.index) throw std::runtime_error(selected.error);
    physical_ = devices[*selected.index];
    queue_family_ = queue_families[*selected.index];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical_, &features);
    // A forced fallback (fallbacks_) turns a capability off as a GPU without it reports it, before anything reads it.
    caps_.bc_textures = features.textureCompressionBC == VK_TRUE && !fallbacks_.no_bc;
    caps_.independent_blend = features.independentBlend == VK_TRUE && !fallbacks_.no_independent_blend;
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R8G8B8A8_SRGB, &fp);
        constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        caps_.srgb_mip_blit = (fp.optimalTilingFeatures & need) == need && !fallbacks_.no_mip_blit;
    }
    caps_.max_anisotropy = features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 0.0f;
    // Frame generation's GPU time (the performance window): every graphics queue can time where this is set.
    timestamp_period_ns_ = props.limits.timestampComputeAndGraphics ? props.limits.timestampPeriod : 0.0f;
    {
        std::uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qcount, families.data());
        timestamp_bits_ = queue_family_ < qcount ? families[queue_family_].timestampValidBits : 0u;
        if (timestamp_bits_ == 0) timestamp_period_ns_ = 0.0f;
    }
    caps_.msaa_counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    caps_.fsr3 = Fsr3Upscaler::supported(physical_);
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    p12.pNext = &driver;
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(physical_, &props2);
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical_, &f2);
    // The texture arrays (render/descriptor_limits.h): update-after-bind where the device has it, for its limits.
    caps_.update_after_bind =
        f12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE && !fallbacks_.no_update_after_bind;
    const VkPhysicalDeviceLimits& l = props.limits;
    caps_.max_sampled_images =
        caps_.update_after_bind
            ? texture_array_limit({p12.maxPerStageDescriptorUpdateAfterBindSamplers,
                                   p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                   p12.maxPerStageUpdateAfterBindResources, p12.maxDescriptorSetUpdateAfterBindSamplers,
                                   p12.maxDescriptorSetUpdateAfterBindSampledImages})
            : texture_array_limit({l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorSampledImages,
                                   l.maxPerStageResources, l.maxDescriptorSetSamplers,
                                   l.maxDescriptorSetSampledImages});
    gpu_name_ = props.deviceName;
    gpu_profile_.name = props.deviceName;
    gpu_profile_.kind = seen[*selected.index].kind;
    gpu_profile_.vendor_id = props.vendorID;
    {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
        for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
            if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                gpu_profile_.device_local_bytes = std::max<std::uint64_t>(gpu_profile_.device_local_bytes,
                                                                          memory.memoryHeaps[i].size);
    }
    gpu_description_ = std::format("{} ({}, {}), Vulkan {}.{}.{}", props.deviceName, driver.driverName, driver.driverInfo,
                                   VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                                   VK_API_VERSION_PATCH(props.apiVersion));
    log::info("GPU: {} (Vulkan {}.{}.{}), BC textures: {}, textures per array: {}{}", props.deviceName,
              VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
              VK_API_VERSION_PATCH(props.apiVersion), caps_.bc_textures ? "yes" : "no", caps_.max_sampled_images,
              caps_.update_after_bind ? " (update after bind)" : "");
}

void VkContext::create_device() {
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, available.data());
    for (const auto& e : available)
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) extensions.push_back("VK_KHR_portability_subset");

    gpu_shared_requested_ = GpuSharedCache::requested();
    const char* shared_extension = gpu_shared_requested_ ? GpuSharedCache::device_extension() : nullptr;
    const bool shared_supported = shared_extension &&
        std::any_of(available.begin(), available.end(), [shared_extension](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, shared_extension) == 0;
        });
    if (shared_supported) extensions.push_back(shared_extension);
    else if (gpu_shared_requested_)
        log::warn("GPU asset sharing: external-memory transport unavailable; immutable assets remain private");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    // The client's features, then an upscaler provider's beside them (render/upscaler_provider.h DeviceFeatures).
    DeviceFeatures features;
    VkPhysicalDeviceVulkan13Features& f13 = features.f13;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // required in 1.3; glslc emits it for discard
    VkPhysicalDeviceVulkan12Features& f12 = features.f12;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE; // nonuniformEXT in zone.frag and particle.frag
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = caps_.update_after_bind ? VK_TRUE : VK_FALSE;
    // The depth format may fall back to one with stencil (below); every barrier on the depth images names the depth
    // aspect alone with DEPTH_ATTACHMENT_OPTIMAL, which on such a format is valid only with this feature.
    f12.separateDepthStencilLayouts = VK_TRUE;
    VkPhysicalDeviceFeatures2& f2 = features.f2;
    f2.features.textureCompressionBC = caps_.bc_textures ? VK_TRUE : VK_FALSE;
    f2.features.samplerAnisotropy = caps_.max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE;
    f2.features.independentBlend = caps_.independent_blend ? VK_TRUE : VK_FALSE;
    // FSR 3.1's storage images in R8, R16F, RG16F ... (Fsr3Upscaler::supported requires it for DeviceCaps::fsr3).
    f2.features.shaderStorageImageExtendedFormats = caps_.fsr3 ? VK_TRUE : VK_FALSE;

    // An upscaler provider's extensions and features: one the GPU lacks keeps the provider out, not the client.
    std::vector<std::string> provider_extensions;
    if (provider_) {
        DeviceFeatures wanted;
        if (!provider_->device_requirements(instance_, physical_, provider_extensions, wanted)) {
            drop_provider("this GPU cannot run it");
        } else {
            std::vector<std::string> offered;
            for (const auto& e : available) offered.emplace_back(e.extensionName);
            std::vector<std::string> missing = missing_names(provider_extensions, offered);
            DeviceFeatures have;
            vkGetPhysicalDeviceFeatures2(physical_, &have.f2);
            for (std::string& m : missing_features(wanted, have)) missing.push_back(std::move(m));
            if (!missing.empty())
                drop_provider("the GPU lacks " + joined(missing));
            else
                add_features(features, wanted);
        }
        if (!provider_) provider_extensions.clear();
    }
    for (const std::string& name : provider_extensions)
        if (std::none_of(extensions.begin(), extensions.end(),
                         [&name](const char* e) { return name == e; }))
            extensions.push_back(name.c_str());

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features.f2;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    vk_check(vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    if (fallbacks_.non_coherent) {
        // VMA reads the memory types once, here; from then on it treats host-visible memory as a GPU without coherent
        // memory would have it. The memory itself is still coherent, so a missing flush shows no wrong pixels here.
        g_hide_coherent = true;
        fns.vkGetPhysicalDeviceMemoryProperties = memory_properties_without_coherent;
    }
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = physical_;
    ai.device = device_;
    ai.instance = instance_;
    ai.pVulkanFunctions = &fns;
    vk_check(vmaCreateAllocator(&ai, &allocator_), "vmaCreateAllocator");

    {
        // D32_SFLOAT, else a format with stencil (render/gpu_fallbacks.h DepthCandidate's order); a forced fallback
        // skips the preferred ones.
        constexpr std::array<VkFormat, 3> kFormats{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                                   VK_FORMAT_D24_UNORM_S8_UINT};
        std::array<bool, 3> usable{};
        for (std::size_t i = 0; i < kFormats.size(); ++i) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(physical_, kFormats[i], &fp);
            constexpr VkFormatFeatureFlags kNeed =
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            usable[i] = (fp.optimalTilingFeatures & kNeed) == kNeed;
        }
        const DepthChoice depth = choose_depth_format(usable, fallbacks_);
        if (!depth.note.empty()) log::warn("GPU fallbacks: {}", depth.note);
        if (depth.format) {
            depth_format_ = kFormats[static_cast<std::size_t>(*depth.format)];
            if (fallbacks_.depth_stencil || fallbacks_.depth_d24)
                log::info("GPU fallbacks: depth format {}", depth_candidate_name(*depth.format));
        }
    }
    if (depth_format_ == VK_FORMAT_UNDEFINED) throw std::runtime_error("no usable depth format");
    create_pipeline_cache();
    if (shared_supported) {
        try {
            gpu_shared_cache_ = std::make_unique<GpuSharedCache>(physical_, device_, queue_family_, gpu_name_, instance_);
            log::info("GPU asset sharing: connected to the texture broker");
        } catch (const std::exception& e) {
            // Sharing is an extra: without it every texture is the client's own, as with sharing off.
            log::warn("GPU asset sharing: not started ({}); textures stay private", e.what());
            gpu_shared_cache_.reset();
        }
    }
}

void VkContext::note_shared_gpu_texture(bool imported) {
    if (!gpu_shared_requested_) return;
    std::uint64_t& count = imported ? gpu_shared_bindings_ : gpu_shared_fallbacks_;
    ++count;
    // Sparse coverage logs let native test automation distinguish actual imports from private fallback.
    if (count == 1 || (count & (count - 1)) == 0) {
        const auto stats = gpu_shared_cache_ ? gpu_shared_cache_->stats() : GpuSharedCache::Stats{};
        log::info("GPU asset sharing coverage: shared bindings {}, unique imports {}, local hits {}, broker hits {}, "
                  "private fallbacks {}", gpu_shared_bindings_, stats.imports, stats.local_hits, stats.broker_hits,
                  gpu_shared_fallbacks_);
    }
}

void VkContext::create_pipeline_cache() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    cache_device_.vendor_id = props.vendorID;
    cache_device_.device_id = props.deviceID;
    cache_device_.driver_version = props.driverVersion;
    std::memcpy(cache_device_.uuid.data(), props.pipelineCacheUUID, cache_device_.uuid.size());
    pipeline_cache_path_ = platform::pipeline_cache_file();
    std::vector<std::byte> data = load_pipeline_cache(pipeline_cache_path_, cache_device_);
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS && !data.empty()) {
        log::warn("pipeline cache {}: the driver refused it; starting empty", pipeline_cache_path_.string());
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) pipeline_cache_ = VK_NULL_HANDLE;
    }
}

void VkContext::save_pipeline_cache_file() {
    if (!pipeline_cache_) return;
    std::size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<std::byte> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);
    if (!save_pipeline_cache(pipeline_cache_path_, data, cache_device_))
        log::warn("could not write the pipeline cache {}", pipeline_cache_path_.string());
}

void VkContext::create_swapchain(VkSwapchainKHR old, const VkSurfaceCapabilitiesKHR* known_caps) {
    VkSurfaceCapabilitiesKHR caps;
    if (known_caps)
        caps = *known_caps;
    else
        vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
    formats.resize(count);
    if (formats.empty()) throw std::runtime_error("swapchain: the surface reports no formats");
    // The post pass writes sRGB-encoded values itself (shaders/post.frag), so a UNORM target is
    // preferred; an SRGB one also works (the pass then leaves the encoding to it).
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    }
    swapchain_format_ = chosen.format;

    // The window's size this swapchain was made for: begin_frame_commands makes it again when that changes. Not
    // extent_, which the surface may set to another size (currentExtent) for as long as the window keeps this one;
    // when the two differ the surface is watched for a while (render/swapchain_size.h), in case it was still at the
    // old size of a resize.
    window_.framebuffer_size(made_for_.width, made_for_.height);
    if (caps.currentExtent.width != UINT32_MAX) {
        extent_ = caps.currentExtent;
        surface_watch_ = surface_watch_frames(Extent{extent_.width, extent_.height},
                                              Extent{made_for_.width, made_for_.height});
    } else {
        surface_watch_ = 0;
        extent_ = made_for_;
        extent_.width = std::clamp(extent_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    min_image_count_ = std::max(caps.minImageCount, 2u);
    // One more image under frame generation, which holds two per real frame (render/frame_gen_logic.h).
    const std::uint32_t image_count =
        swapchain_image_count(caps.minImageCount, caps.maxImageCount, scene_options_.frame_gen);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    screenshots_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (!screenshots_supported_) log::warn("swapchain: the surface cannot be copied from; screenshots are off");
    // The readback (end_frame) is 4 bytes a pixel in BGRA or RGBA order: a fallback format (formats[0] above, say
    // FP16 or 10-bit HDR) is not, and copying it would overrun the buffer.
    if (screenshots_supported_ && !screenshot_format_supported(chosen.format)) {
        screenshots_supported_ = false;
        log::warn("swapchain: format {} is not 8-bit RGBA or BGRA; screenshots are off", static_cast<int>(chosen.format));
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available
    {
        std::uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
        auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
        // Frame generation keeps FIFO whatever the vsync setting: it paces the generated frame and the real one a
        // refresh apart.
        if (scene_options_.frame_gen) {
        } else if (vsync_ == Vsync::Off) {
            if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (vsync_ == Vsync::Adaptive && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
            info.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        }
    }
    info.clipped = VK_TRUE;
    info.oldSwapchain = old; // retired by this call, whether it succeeds or not
    vk_check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    log::debug("swapchain: {}x{}, present mode {}", extent_.width, extent_.height, static_cast<int>(info.presentMode));
    switch (info.presentMode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: present_mode_ = "immediate"; break;
    case VK_PRESENT_MODE_MAILBOX_KHR: present_mode_ = "mailbox"; break;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: present_mode_ = "FIFO relaxed (adaptive vsync)"; break;
    default: present_mode_ = "FIFO (vsync)"; break;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());

    views_.resize(count);
    render_finished_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        views_[i] = create_image_view(device_, images_[i], swapchain_format_, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(device_, &s, nullptr, &render_finished_[i]), "vkCreateSemaphore");
    }

    create_scene_targets();
}

VkContext::Image VkContext::create_target(VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                                          VkImageAspectFlags aspect, VkExtent2D extent) {
    if (extent.width == 0) extent = scene_extent_;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {extent.width, extent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    Image out;
    vk_check(vmaCreateImage(allocator_, &ii, &ai, &out.image, &out.alloc, nullptr), "create render target");
    out.view = create_image_view(device_, out.image, format, aspect, 1);
    return out;
}

void VkContext::destroy_target(Image& image) {
    if (image.view) vkDestroyImageView(device_, image.view, nullptr);
    if (image.image) vmaDestroyImage(allocator_, image.image, image.alloc);
    image = {};
}

VkRect2D VkContext::scene_area() const {
    VkRect2D r = scene_rect_;
    if (r.extent.width == 0 || r.extent.height == 0) return {{0, 0}, extent_};
    r.extent.width = std::min(r.extent.width, extent_.width);
    r.extent.height = std::min(r.extent.height, extent_.height);
    r.offset.x = std::clamp(r.offset.x, 0, static_cast<std::int32_t>(extent_.width - r.extent.width));
    r.offset.y = std::clamp(r.offset.y, 0, static_cast<std::int32_t>(extent_.height - r.extent.height));
    return r;
}

void VkContext::create_scene_targets() {
    scene_area_ = scene_area();
    view_extent_ = scene_area_.extent;
    scene_extent_ = scaled_extent(view_extent_, scene_options_.render_scale);
    if (scene_samples_ != VK_SAMPLE_COUNT_1_BIT)
        scene_msaa_ = create_target(kSceneFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                    scene_samples_, VK_IMAGE_ASPECT_COLOR_BIT);
    scene_resolved_ = create_target(kSceneFormat,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    // The depth is kept for the AO and the post pass: multisampled, sample 0 resolves into depth_resolved_.
    bool msaa = scene_samples_ != VK_SAMPLE_COUNT_1_BIT;
    depth_ = create_target(depth_format_,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               (msaa ? 0u : static_cast<std::uint32_t>(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)),
                           scene_samples_, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (msaa)
        depth_resolved_ = create_target(depth_format_,
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    water_color_ = create_target(kSceneFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    water_depth_ = create_target(depth_format_, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    write_water_descriptors();
    ao_ = create_target(kAoFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
    for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
        bloom_extent_[i] = {std::max(1u, scene_extent_.width >> (i + 1)), std::max(1u, scene_extent_.height >> (i + 1))};
        bloom_[i] = create_target(kSceneFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, bloom_extent_[i]);
    }
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // An upscaler provider that failed, or cannot upscale between these sizes, gives way to the client's own
    // temporal upscaler: its targets are made below instead. Every caller has the device idle.
    if (scene_options_.upscaler == Upscaler::External) {
        if (provider_failed_)
            drop_provider("it failed");
        else if (provider_ && !provider_->resize(scene_extent_, view_extent_))
            drop_provider(std::format("it cannot upscale {}x{} to {}x{}", scene_extent_.width, scene_extent_.height,
                                      view_extent_.width, view_extent_.height));
        if (!provider_) scene_options_.upscaler = Upscaler::Taa;
    }
    const bool taa = scene_options_.upscaler == Upscaler::Taa;
    const bool fsr3 = scene_options_.upscaler == Upscaler::Fsr3 && fsr3_;
    const bool external = scene_options_.upscaler == Upscaler::External && provider_;
    if (upscaling())
        upscale_source_ = create_target(kUpscaleFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (upscaling() || taa)
        upscaled_ = create_target(kUpscaleFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, view_extent_);
    if (scene_motion()) {
        object_motion_ = create_target(kMotionFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        reactive_ = create_target(kReactiveFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (taa || fsr3 || external) { // the temporal inputs (temporal_prep.frag), for any resolve
        // A provider may copy the colour (sdk/hailstone_upscaler.h: TRANSFER_SRC).
        lit_ = create_target(kSceneFormat, kUsage | (external ? VkImageUsageFlags{VK_IMAGE_USAGE_TRANSFER_SRC_BIT} : VkImageUsageFlags{0}),
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        motion_ = create_target(kMotionFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (taa)
        for (Image& h : history_)
            h = create_target(kSceneFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, view_extent_);
    if (fsr3 || external) {
        fsr3_output_ = create_target(kSceneFormat,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         (external ? VkImageUsageFlags{VK_IMAGE_USAGE_TRANSFER_DST_BIT} : VkImageUsageFlags{0}),
                                     VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, view_extent_);
        if (fsr3) fsr3_->resize(scene_extent_, view_extent_); // every caller has the device idle
    }
    if (frame_gen_ && (taa || fsr3 || external)) {
        // Frame generation's pictures at the view's size, and the UI's image at the window's.
        for (Image& p : pictures_)
            p = create_target(kUpscaleFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, view_extent_);
        generated_ = create_target(kUpscaleFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, view_extent_);
        overlay_ = create_target(swapchain_format_, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, extent_);
        frame_gen_->resize(scene_extent_);
    }
    history_valid_ = false; // new targets hold nothing
    picture_ready_ = false;
    write_post_descriptors();
    bind_frame_gen();
}

void VkContext::destroy_scene_targets() {
    destroy_target(scene_msaa_);
    destroy_target(scene_resolved_);
    destroy_target(depth_);
    destroy_target(depth_resolved_);
    destroy_target(ao_);
    for (Image& level : bloom_) destroy_target(level);
    destroy_target(water_color_);
    destroy_target(water_depth_);
    destroy_target(upscale_source_);
    destroy_target(upscaled_);
    destroy_target(lit_);
    destroy_target(motion_);
    destroy_target(object_motion_);
    destroy_target(reactive_);
    for (Image& h : history_) destroy_target(h);
    destroy_target(fsr3_output_);
    for (Image& p : pictures_) destroy_target(p);
    destroy_target(generated_);
    destroy_target(overlay_);
}

void VkContext::set_scene_options(const SceneOptions& options, const std::filesystem::path& shader_dir) {
    const VkExtent2D scaled = scaled_extent(view_extent_, options.render_scale);
    const bool rescaled = scaled.width != scene_extent_.width || scaled.height != scene_extent_.height;
    SceneOptions chosen = options;
    if (chosen.upscaler == Upscaler::External && (!provider_ || provider_failed_)) {
        if (!provider_fallback_logged_)
            log::warn("upscaler: the chosen upscaler provider is not running; {} is used",
                      caps_.fsr3 ? "AMD FSR 3.1" : "the temporal upscaler (taa)");
        provider_fallback_logged_ = true;
        chosen.upscaler = Upscaler::Fsr3; // and Taa below where the GPU cannot run it
    }
    if (chosen.upscaler == Upscaler::Fsr3 && !caps_.fsr3) {
        if (!fsr3_fallback_logged_)
            log::warn("upscaler: AMD FSR 3.1 needs storage images in its formats and quad subgroup operations, which "
                      "this GPU lacks; the temporal upscaler (taa) is used");
        fsr3_fallback_logged_ = true;
        chosen.upscaler = Upscaler::Taa;
    }
    const bool upscaler_changed = chosen.upscaler != scene_options_.upscaler;
    frame_gen_requested_ = chosen.frame_gen;
    // Frame generation follows the temporal upscaler's motion: without one it is off.
    if (chosen.frame_gen && chosen.upscaler == Upscaler::Fsr1) {
        if (!frame_gen_fallback_logged_)
            log::warn("frame generation needs a temporal upscaler (display.upscaler taa or fsr3); it stays off");
        frame_gen_fallback_logged_ = true;
        chosen.frame_gen = false;
    }
    const bool frame_gen_changed = chosen.frame_gen != scene_options_.frame_gen;
    scene_options_ = chosen;
    // The largest supported count not above the request; one under the temporal upscaler, which resolves the edges
    // itself (and whose motion and depth are taken per pixel).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    for (std::uint32_t n = 2; n <= kMaxMsaa && n <= options.msaa && scene_options_.upscaler == Upscaler::Fsr1; n *= 2)
        if (caps_.msaa_counts & n) samples = static_cast<VkSampleCountFlagBits>(n);
    if (!post_pipeline_) create_post_pipeline(shader_dir);
    if (frame_gen_changed) { // after the post pipelines: its RCAS uses their layout
        vkDeviceWaitIdle(device_);
        if (scene_options_.frame_gen)
            create_frame_gen(shader_dir);
        else
            destroy_frame_gen();
        resize_pending_ = true; // the swapchain's image count and present mode
    }
    if (scene_options_.upscaler == Upscaler::Fsr3 && !fsr3_) {
        vkDeviceWaitIdle(device_);
        fsr3_ = std::make_unique<Fsr3Upscaler>(device_, allocator_, pipeline_cache_, shader_dir);
        // HAILSTONE_FSR3_TIMING: each pass's GPU time (Fsr3Upscaler::timing_summary, logged by the viewer's --frames).
        if (core::env("FSR3_TIMING")) fsr3_->enable_timing(timestamp_period_ns_);
    }
    std::uint32_t shadow = options.shadow_size > 0 ? std::clamp(options.shadow_size, 256u, 8192u) : 1u;
    if (shadow != shadow_size_) {
        vkDeviceWaitIdle(device_);
        create_shadow_map(shadow);
    }
    std::uint32_t point_slots = std::min(options.light_shadows, kMaxPointShadows);
    std::uint32_t point_size = point_slots ? std::clamp(options.light_shadow_size, 128u, 1024u) : 1u;
    if (point_slots != point_slots_ || point_size != point_size_) {
        vkDeviceWaitIdle(device_);
        create_point_shadow_map(point_size, point_slots);
    }
    if (samples != scene_samples_ || rescaled || upscaler_changed || frame_gen_changed) {
        vkDeviceWaitIdle(device_);
        destroy_scene_targets();
        scene_samples_ = samples;
        create_scene_targets();
    }
    log::info("scene: {}x MSAA, upscaler {}{}, render scale {:.2f} ({}x{}), tonemap {}, exposure {:+.2f} EV, shadows {}, "
              "light shadows {} x {}, grade {:+.2f} / {:.2f} / {:.2f}",
              static_cast<int>(scene_samples_), upscaler_name(scene_options_.upscaler),
              scene_options_.frame_gen ? " + frame generation" : "",
              std::clamp(options.render_scale, kMinRenderScale, 1.0f), scene_extent_.width,
              scene_extent_.height, static_cast<int>(options.tonemap), options.exposure,
              options.shadow_size ? shadow_size_ : 0u, point_slots_, point_slots_ ? point_size_ : 0u,
              options.brightness, options.contrast, options.gamma);
}

void VkContext::create_post_pipeline(const std::filesystem::path& shader_dir) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device_, &si, nullptr, &post_sampler_), "vkCreateSampler (post)");
    si.magFilter = VK_FILTER_LINEAR; // the scene, for the distance blur's taps
    si.minFilter = VK_FILTER_LINEAR;
    vk_check(vkCreateSampler(device_, &si, nullptr, &post_linear_sampler_), "vkCreateSampler (post, linear)");

    // The post set: 0 the scene, 1 the AO, 2 the depth, 3 the bloom, 4 the object motion and 5 the reactive mask
    // (temporal_prep.frag's; the AO image where they are not made); the AO set: 0 the depth; the bloom sets (the AO's
    // layout): 0 the level read; then FSR 1's two (the AO's layout): EASU's source, RCAS's; then the temporal
    // upscaler's four (the post set's layout): the two resolve sets, the two post sets; then FSR 3.1's post set.
    constexpr std::uint32_t kPostBindings = 6;
    VkDescriptorSetLayoutBinding b[kPostBindings];
    for (std::uint32_t i = 0; i < kPostBindings; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = kPostBindings;
    li.pBindings = b;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &post_set_layout_), "vkCreateDescriptorSetLayout (post)");
    li.bindingCount = 1;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &ao_set_layout_), "vkCreateDescriptorSetLayout (AO)");
    constexpr std::uint32_t kSets = 2 + kBloomLevels + 1 + 2 + 4 + 1;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * kPostBindings + 1 + kBloomLevels + 1 + 2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    vk_check(vkCreateDescriptorPool(device_, &pi, nullptr, &post_pool_), "vkCreateDescriptorPool (post)");
    std::array<VkDescriptorSetLayout, kSets> layouts;
    layouts.fill(ao_set_layout_);
    layouts[0] = post_set_layout_;
    for (std::uint32_t i = kSets - 5; i < kSets; ++i) layouts[i] = post_set_layout_;
    std::array<VkDescriptorSet, kSets> sets;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = post_pool_;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(device_, &ai, sets.data()), "vkAllocateDescriptorSets (post)");
    post_set_ = sets[0];
    ao_set_ = sets[1];
    std::copy(sets.begin() + 2, sets.begin() + 2 + bloom_sets_.size(), bloom_sets_.begin());
    easu_set_ = sets[kSets - 7];
    rcas_set_ = sets[kSets - 6];
    resolve_sets_ = {sets[kSets - 5], sets[kSets - 4]};
    post_taa_sets_ = {sets[kSets - 3], sets[kSets - 2]};
    post_fsr3_set_ = sets[kSets - 1];
    write_post_descriptors();

    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &post_set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    vk_check(vkCreatePipelineLayout(device_, &pli, nullptr, &post_layout_), "vkCreatePipelineLayout (post)");
    VkPushConstantRange apc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(AoPush)};
    pli.pSetLayouts = &ao_set_layout_;
    pli.pPushConstantRanges = &apc;
    vk_check(vkCreatePipelineLayout(device_, &pli, nullptr, &ao_layout_), "vkCreatePipelineLayout (AO)");
    VkPushConstantRange bpc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush)};
    pli.pPushConstantRanges = &bpc;
    vk_check(vkCreatePipelineLayout(device_, &pli, nullptr, &bloom_layout_), "vkCreatePipelineLayout (bloom)");
    VkPushConstantRange upc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UpscalePush)};
    pli.pPushConstantRanges = &upc;
    vk_check(vkCreatePipelineLayout(device_, &pli, nullptr, &upscale_layout_), "vkCreatePipelineLayout (FSR)");
    pli.pSetLayouts = &post_set_layout_;
    VkPushConstantRange ppc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PrepPush)};
    pli.pPushConstantRanges = &ppc;
    vk_check(vkCreatePipelineLayout(device_, &pli, nullptr, &prep_layout_), "vkCreatePipelineLayout (TAA prep)");
    VkPushConstantRange rpc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResolvePush)};
    pli.pPushConstantRanges = &rpc;
    vk_check(vkCreatePipelineLayout(device_, &pli, nullptr, &resolve_layout_), "vkCreatePipelineLayout (TAA resolve)");

    VkShaderModule vs = load_module(device_, shader_dir / "post.vert.spv");
    VkShaderModule fs = load_module(device_, shader_dir / "post.frag.spv");
    VkShaderModule ao = load_module(device_, shader_dir / "ao.frag.spv");
    post_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, post_layout_, vs, fs, swapchain_format_);
    ao_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, ao_layout_, vs, ao, kAoFormat);
    VkShaderModule down = load_module(device_, shader_dir / "bloom_down.frag.spv");
    VkShaderModule up = load_module(device_, shader_dir / "bloom_up.frag.spv");
    bloom_down_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, bloom_layout_, vs, down, kSceneFormat);
    bloom_up_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, bloom_layout_, vs, up, kSceneFormat, true);
    post_upscale_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, post_layout_, vs, fs, kUpscaleFormat);
    VkShaderModule easu = load_module(device_, shader_dir / "fsr_easu.frag.spv");
    VkShaderModule rcas = load_module(device_, shader_dir / "fsr_rcas.frag.spv");
    easu_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, upscale_layout_, vs, easu, kUpscaleFormat);
    rcas_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, upscale_layout_, vs, rcas, swapchain_format_);
    VkShaderModule prep = load_module(device_, shader_dir / "temporal_prep.frag.spv");
    VkShaderModule resolve = load_module(device_, shader_dir / "taa_resolve.frag.spv");
    const VkFormat prep_formats[2] = {kSceneFormat, kMotionFormat};
    prep_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, prep_layout_, vs, prep, prep_formats);
    resolve_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, resolve_layout_, vs, resolve, kSceneFormat);
    for (VkShaderModule m : {vs, fs, ao, down, up, easu, rcas, prep, resolve}) vkDestroyShaderModule(device_, m, nullptr);

    // The zone's fog: two samplers (the depth, the field), a set per field from a pool of its own.
    VkDescriptorSetLayoutBinding fb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo fli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    fli.bindingCount = 2;
    fli.pBindings = fb;
    vk_check(vkCreateDescriptorSetLayout(device_, &fli, nullptr, &zone_fog_set_layout_),
             "vkCreateDescriptorSetLayout (zone fog)");
    // A field's set is retired with it while the next one is in use: a few at once.
    constexpr std::uint32_t kFogSets = 4;
    VkDescriptorPoolSize fsize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kFogSets};
    VkDescriptorPoolCreateInfo fpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    fpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    fpi.maxSets = kFogSets;
    fpi.poolSizeCount = 1;
    fpi.pPoolSizes = &fsize;
    vk_check(vkCreateDescriptorPool(device_, &fpi, nullptr, &zone_fog_pool_), "vkCreateDescriptorPool (zone fog)");
    VkPushConstantRange fpc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ZoneFogPush)};
    VkPipelineLayoutCreateInfo fpli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    fpli.setLayoutCount = 1;
    fpli.pSetLayouts = &zone_fog_set_layout_;
    fpli.pushConstantRangeCount = 1;
    fpli.pPushConstantRanges = &fpc;
    vk_check(vkCreatePipelineLayout(device_, &fpli, nullptr, &zone_fog_layout_), "vkCreatePipelineLayout (zone fog)");
    // Over the scene: rgb the fog's light + the scene x what shows through (the shader's alpha); the alpha kept.
    VkPipelineColorBlendAttachmentState over{};
    over.blendEnable = VK_TRUE;
    over.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    over.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    over.colorBlendOp = VK_BLEND_OP_ADD;
    over.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    over.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    over.alphaBlendOp = VK_BLEND_OP_ADD;
    over.colorWriteMask = 0xf;
    VkShaderModule fvs = load_module(device_, shader_dir / "post.vert.spv");
    VkShaderModule fog = load_module(device_, shader_dir / "zone_fog.frag.spv");
    zone_fog_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, zone_fog_layout_, fvs, fog,
                                             std::span<const VkFormat>(&kSceneFormat, 1), false, &over);
    vkDestroyShaderModule(device_, fvs, nullptr);
    vkDestroyShaderModule(device_, fog, nullptr);
}

void VkContext::write_post_descriptors() {
    if (!post_set_) return;
    const Image& depth = depth_resolved_.image ? depth_resolved_ : depth_;
    constexpr VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    struct Write { VkDescriptorSet set; std::uint32_t binding; VkDescriptorImageInfo image; };
    std::vector<Write> writes = {
        {post_set_, 0, {post_linear_sampler_, scene_resolved_.view, kRead}},
        {post_set_, 1, {post_sampler_, ao_.view, kRead}},
        {post_set_, 2, {post_sampler_, depth.view, kRead}},
        {post_set_, 3, {post_linear_sampler_, bloom_[0].view, kRead}},
        {ao_set_, 0, {post_sampler_, depth.view, kRead}},
        {bloom_sets_[0], 0, {post_linear_sampler_, scene_resolved_.view, kRead}},
    };
    for (std::uint32_t i = 0; i < kBloomLevels; ++i)
        writes.push_back({bloom_sets_[i + 1], 0, {post_linear_sampler_, bloom_[i].view, kRead}});
    if (zone_fog_set_) writes.push_back({zone_fog_set_, 0, {post_sampler_, depth.view, kRead}});
    // Every set of the post set's layout: the object motion and the reactive mask (the AO image without them).
    const VkImageView object_motion = object_motion_.view ? object_motion_.view : ao_.view;
    const VkImageView reactive = reactive_.view ? reactive_.view : ao_.view;
    for (VkDescriptorSet set :
         {post_set_, resolve_sets_[0], resolve_sets_[1], post_taa_sets_[0], post_taa_sets_[1], post_fsr3_set_}) {
        writes.push_back({set, 4, {post_sampler_, object_motion, kRead}});
        writes.push_back({set, 5, {post_sampler_, reactive, kRead}});
    }
    // FSR 1's and the sharpening's, while their images exist.
    if (upscale_source_.view) writes.push_back({easu_set_, 0, {post_linear_sampler_, upscale_source_.view, kRead}});
    if (upscaled_.view) writes.push_back({rcas_set_, 0, {post_sampler_, upscaled_.view, kRead}});
    // The temporal upscaler's, under it: resolve set i writes history i and reads the other.
    if (lit_.view && history_[0].view)
        for (std::uint32_t i = 0; i < 2; ++i) {
            writes.push_back({resolve_sets_[i], 0, {post_linear_sampler_, lit_.view, kRead}});
            writes.push_back({resolve_sets_[i], 1, {post_sampler_, motion_.view, kRead}});
            writes.push_back({resolve_sets_[i], 2, {post_sampler_, depth.view, kRead}});
            writes.push_back({resolve_sets_[i], 3, {post_linear_sampler_, history_[i ^ 1].view, kRead}});
            writes.push_back({post_taa_sets_[i], 0, {post_linear_sampler_, history_[i].view, kRead}});
            writes.push_back({post_taa_sets_[i], 1, {post_sampler_, ao_.view, kRead}});
            writes.push_back({post_taa_sets_[i], 2, {post_sampler_, depth.view, kRead}});
            writes.push_back({post_taa_sets_[i], 3, {post_linear_sampler_, bloom_[0].view, kRead}});
        }
    // FSR 3.1's output in the scene's place, under it.
    if (fsr3_output_.view) {
        writes.push_back({post_fsr3_set_, 0, {post_linear_sampler_, fsr3_output_.view, kRead}});
        writes.push_back({post_fsr3_set_, 1, {post_sampler_, ao_.view, kRead}});
        writes.push_back({post_fsr3_set_, 2, {post_sampler_, depth.view, kRead}});
        writes.push_back({post_fsr3_set_, 3, {post_linear_sampler_, bloom_[0].view, kRead}});
    }
    std::vector<VkWriteDescriptorSet> w(writes.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = writes[i].set;
        w[i].dstBinding = writes[i].binding;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &writes[i].image;
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(w.size()), w.data(), 0, nullptr);
}

void VkContext::set_zone_fog_field(const ZoneFogField& field) {
    // The old field and its set go once the frames that may draw with them are done.
    if (zone_fog_set_ || zone_fog_texture_.image) {
        retire([this, set = zone_fog_set_, texture = std::move(zone_fog_texture_)]() mutable {
            if (set) vkFreeDescriptorSets(device_, zone_fog_pool_, 1, &set);
            texture.destroy(device_, allocator_);
        });
        zone_fog_set_ = VK_NULL_HANDLE;
        zone_fog_texture_ = {};
    }
    if (!field.valid() || !zone_fog_pool_) return;
    assets::Image image;
    image.width = static_cast<std::uint32_t>(field.nx);
    image.height = static_cast<std::uint32_t>(field.ny);
    std::vector<std::byte> texels(field.texels.size() * 4);
    for (std::size_t i = 0; i < field.texels.size(); ++i)
        for (std::size_t c = 0; c < 4; ++c) texels[i * 4 + c] = static_cast<std::byte>((field.texels[i] >> (8 * c)) & 0xffu);
    image.mips.push_back(std::move(texels));
    UploadBatch batch(*this);
    zone_fog_texture_ = batch.texture(image, TextureUse::Data);
    batch.submit();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = zone_fog_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &zone_fog_set_layout_;
    if (vkAllocateDescriptorSets(device_, &ai, &zone_fog_set_) != VK_SUCCESS) {
        log::warn("the zone's fog: no descriptor set left; it is not drawn");
        zone_fog_set_ = VK_NULL_HANDLE;
        return;
    }
    const glm::vec2 size = glm::vec2(static_cast<float>(field.nx), static_cast<float>(field.ny)) * field.cell;
    zone_fog_grid_ = glm::vec4(field.lo, 1.0f / size.x, 1.0f / size.y);
    zone_fog_floor_ = glm::vec2(field.floor_lo, field.floor_range);
    const Image& depth = depth_resolved_.image ? depth_resolved_ : depth_;
    const VkDescriptorImageInfo images[2] = {
        {post_sampler_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {post_linear_sampler_, zone_fog_texture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = zone_fog_set_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void VkContext::draw_zone_fog(const FrameInfo& frame, const AoView& view, float depth_x, float depth_y) {
    const ZoneFogView& fog = *zone_fog_view_;
    VkCommandBuffer cmd = frame.cmd;
    // The resolve (or the scene's own drawing) wrote the colour; the fog blends over it.
    image_barrier(cmd, scene_resolved_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo target{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    target.imageView = scene_resolved_.view;
    target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, frame.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &target;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, frame.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    ZoneFogPush push;
    push.right = glm::vec4(fog.right * view.tan_half_x, depth_x);
    push.up = glm::vec4(fog.up * view.tan_half_y, depth_y);
    push.forward = glm::vec4(fog.forward, std::min(fog.reach, view.far_plane));
    push.camera = glm::vec4(fog.camera, zone_fog_floor_.x);
    push.color = glm::vec4(fog.color, zone_fog_floor_.y);
    push.glow = glm::vec4(fog.glow, fog.camera_past);
    // The time wrapped, so the noise stays precise far from its origin; the mist shifts once every 68 minutes.
    push.to_sun = glm::vec4(fog.to_sun, std::fmod(fog.time, 4096.0f));
    push.grid = zone_fog_grid_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, zone_fog_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, zone_fog_layout_, 0, 1, &zone_fog_set_, 0, nullptr);
    vkCmdPushConstants(cmd, zone_fog_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void VkContext::destroy_post_pipeline() {
    if (zone_fog_pipeline_) vkDestroyPipeline(device_, zone_fog_pipeline_, nullptr);
    if (zone_fog_layout_) vkDestroyPipelineLayout(device_, zone_fog_layout_, nullptr);
    if (zone_fog_pool_) vkDestroyDescriptorPool(device_, zone_fog_pool_, nullptr); // its sets with it
    if (zone_fog_set_layout_) vkDestroyDescriptorSetLayout(device_, zone_fog_set_layout_, nullptr);
    zone_fog_texture_.destroy(device_, allocator_);
    zone_fog_texture_ = {};
    zone_fog_pipeline_ = VK_NULL_HANDLE;
    zone_fog_layout_ = VK_NULL_HANDLE;
    zone_fog_pool_ = VK_NULL_HANDLE;
    zone_fog_set_layout_ = VK_NULL_HANDLE;
    zone_fog_set_ = VK_NULL_HANDLE;
    if (post_pipeline_) vkDestroyPipeline(device_, post_pipeline_, nullptr);
    if (post_layout_) vkDestroyPipelineLayout(device_, post_layout_, nullptr);
    if (ao_pipeline_) vkDestroyPipeline(device_, ao_pipeline_, nullptr);
    if (ao_layout_) vkDestroyPipelineLayout(device_, ao_layout_, nullptr);
    if (bloom_down_pipeline_) vkDestroyPipeline(device_, bloom_down_pipeline_, nullptr);
    if (bloom_up_pipeline_) vkDestroyPipeline(device_, bloom_up_pipeline_, nullptr);
    if (bloom_layout_) vkDestroyPipelineLayout(device_, bloom_layout_, nullptr);
    bloom_down_pipeline_ = bloom_up_pipeline_ = VK_NULL_HANDLE;
    if (post_upscale_pipeline_) vkDestroyPipeline(device_, post_upscale_pipeline_, nullptr);
    if (easu_pipeline_) vkDestroyPipeline(device_, easu_pipeline_, nullptr);
    if (rcas_pipeline_) vkDestroyPipeline(device_, rcas_pipeline_, nullptr);
    post_upscale_pipeline_ = easu_pipeline_ = rcas_pipeline_ = VK_NULL_HANDLE;
    if (upscale_layout_) vkDestroyPipelineLayout(device_, upscale_layout_, nullptr);
    upscale_layout_ = VK_NULL_HANDLE;
    easu_set_ = rcas_set_ = VK_NULL_HANDLE;
    if (prep_pipeline_) vkDestroyPipeline(device_, prep_pipeline_, nullptr);
    if (resolve_pipeline_) vkDestroyPipeline(device_, resolve_pipeline_, nullptr);
    if (prep_layout_) vkDestroyPipelineLayout(device_, prep_layout_, nullptr);
    if (resolve_layout_) vkDestroyPipelineLayout(device_, resolve_layout_, nullptr);
    prep_pipeline_ = resolve_pipeline_ = VK_NULL_HANDLE;
    prep_layout_ = resolve_layout_ = VK_NULL_HANDLE;
    resolve_sets_ = {};
    post_taa_sets_ = {};
    post_fsr3_set_ = VK_NULL_HANDLE;
    bloom_layout_ = VK_NULL_HANDLE;
    bloom_sets_ = {};
    if (ao_set_layout_) vkDestroyDescriptorSetLayout(device_, ao_set_layout_, nullptr);
    ao_pipeline_ = VK_NULL_HANDLE;
    ao_layout_ = VK_NULL_HANDLE;
    ao_set_layout_ = VK_NULL_HANDLE;
    ao_set_ = VK_NULL_HANDLE;
    if (post_pool_) vkDestroyDescriptorPool(device_, post_pool_, nullptr);
    if (post_set_layout_) vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
    if (post_sampler_) vkDestroySampler(device_, post_sampler_, nullptr);
    if (post_linear_sampler_) vkDestroySampler(device_, post_linear_sampler_, nullptr);
    post_linear_sampler_ = VK_NULL_HANDLE;
    post_pipeline_ = VK_NULL_HANDLE;
    post_layout_ = VK_NULL_HANDLE;
    post_pool_ = VK_NULL_HANDLE;
    post_set_ = VK_NULL_HANDLE;
    post_set_layout_ = VK_NULL_HANDLE;
    post_sampler_ = VK_NULL_HANDLE;
}

void VkContext::destroy_swapchain() {
    destroy_scene_targets();
    for (VkSemaphore s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    render_finished_.clear();
    views_.clear();
    images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VkContext::recreate_swapchain() {
    // Asked before anything is let go: a surface minimised since the window's state was last read (Windows: 0x0)
    // cannot have a swapchain, and a throw from vkCreateSwapchainKHR would leave none and no scene targets. The
    // frame is skipped and resize_pending_ stays set, so the next frame asks again.
    VkSurfaceCapabilitiesKHR caps;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");
    if (surface_extent_empty(Extent{caps.currentExtent.width, caps.currentExtent.height})) return false;
    wait_idle();
    destroy_scene_targets();
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    views_.clear();
    images_.clear();
    // The old swapchain and its render_finished semaphores may still be in use by presentation after the device is
    // idle (vkDeviceWaitIdle does not cover a present's semaphore wait), and nothing signals when that ends without
    // VK_EXT_swapchain_maintenance1's present fences. So they go once the frames in flight on the new swapchain are
    // done: presentation of the old images, queued before those frames on the same queue, is over by then.
    VkSwapchainKHR old = std::exchange(swapchain_, VK_NULL_HANDLE);
    std::vector<VkSemaphore> semaphores = std::move(render_finished_);
    render_finished_.clear();
    retired_.push({frames_begun_ + kFramesInFlight, uploads_.last()}, [device = device_, old, semaphores] {
        for (VkSemaphore s : semaphores) vkDestroySemaphore(device, s, nullptr);
        if (old) vkDestroySwapchainKHR(device, old, nullptr);
    });
    create_swapchain(old, &caps);
    resize_pending_ = false;
    return true;
}

void VkContext::create_frames() {
    for (auto& f : frames_) {
        VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        p.queueFamilyIndex = queue_family_;
        vk_check(vkCreateCommandPool(device_, &p, nullptr, &f.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool = f.pool;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(device_, &a, &f.cmd), "vkAllocateCommandBuffers");
        VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(device_, &s, nullptr, &f.image_available), "vkCreateSemaphore");
        vk_check(vkCreateSemaphore(device_, &s, nullptr, &f.generated_available), "vkCreateSemaphore");
        VkFenceCreateInfo fe{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fe.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vkCreateFence(device_, &fe, nullptr, &f.in_flight), "vkCreateFence");
    }
    VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    p.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    p.queueFamilyIndex = queue_family_;
    vk_check(vkCreateCommandPool(device_, &p, nullptr, &upload_pool_), "vkCreateCommandPool");
}

void VkContext::localize_shared_textures() {
    // Sharing has stopped: the imports become the client's own textures, then whoever holds a copy of their
    // image or view takes the new ones. Between frames, with the device idle: no set in use is rewritten.
    if (!shared_detach_logged_) {
        shared_detach_logged_ = true;
        log::warn("GPU asset sharing stopped ({}); this client goes on with its own textures",
                  gpu_shared_cache_->detach_reason());
    }
    wait_idle();
    const std::size_t moved = gpu_shared_cache_->localize(queue_, allocator_);
    log::info("GPU asset sharing: {} shared textures copied into this client's own memory", moved);
    for (const auto& [id, refresh] : shared_texture_listeners_) refresh();
}

std::uint64_t VkContext::on_shared_textures_moved(std::function<void()> refresh) {
    const std::uint64_t id = ++shared_texture_listener_ids_;
    shared_texture_listeners_.emplace(id, std::move(refresh));
    return id;
}

void VkContext::remove_shared_textures_listener(std::uint64_t id) { shared_texture_listeners_.erase(id); }

void VkContext::wait_idle() {
    vkDeviceWaitIdle(device_);
    // Every frame submitted is done; one being recorded is not (what it recorded so far may still be drawn).
    frames_done_ = recording_ ? frames_begun_ - 1 : frames_begun_;
    uploads_.all_done([this](const Upload& u) { recycle_upload(u); });
    retired_.collect({frames_done_, uploads_.completed()});
}

void VkContext::retire(std::function<void()> release, std::uint32_t frames_ahead) {
    const GpuPoint at{frames_begun_ + frames_ahead, uploads_.last()};
    // Nothing in flight can use it (right after wait_idle, between frames): it goes now, as it did after a wait.
    if (gpu_point_reached(at, {frames_done_, uploads_.completed()})) {
        if (release) release();
        return;
    }
    retired_.push(at, std::move(release));
}

void VkContext::retire_after_upload(std::uint64_t serial, std::function<void()> release) {
    retired_.push({0, serial}, std::move(release));
}

void VkContext::recycle_upload(const Upload& u) {
    vkFreeCommandBuffers(device_, upload_pool_, 1, &u.cmd);
    if (vkResetFences(device_, 1, &u.fence) == VK_SUCCESS) free_fences_.push_back(u.fence);
    else vkDestroyFence(device_, u.fence, nullptr);
}

void VkContext::collect_retired() {
    uploads_.poll([this](const Upload& u) { return vkGetFenceStatus(device_, u.fence) == VK_SUCCESS; },
                  [this](const Upload& u) { recycle_upload(u); });
    retired_.collect({frames_done_, uploads_.completed()});
}

std::uint64_t VkContext::submit_upload(const std::function<void(VkCommandBuffer)>& record, VkDeviceSize staging_bytes) {
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool = upload_pool_;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    Upload u;
    vk_check(vkAllocateCommandBuffers(device_, &a, &u.cmd), "vkAllocateCommandBuffers");
    try {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(u.cmd, &begin), "vkBeginCommandBuffer");
        record(u.cmd);
        vk_check(vkEndCommandBuffer(u.cmd), "vkEndCommandBuffer");
        if (!free_fences_.empty()) {
            u.fence = free_fences_.back();
            free_fences_.pop_back();
        } else {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vk_check(vkCreateFence(device_, &fi, nullptr, &u.fence), "vkCreateFence");
        }
        VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmd_info.commandBuffer = u.cmd;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        vk_check(vkQueueSubmit2(queue_, 1, &submit, u.fence), "vkQueueSubmit2");
    } catch (...) {
        // Nothing reached the GPU: the buffer and the fence (unsignalled) can go back.
        vkFreeCommandBuffers(device_, upload_pool_, 1, &u.cmd);
        if (u.fence) free_fences_.push_back(u.fence);
        throw;
    }
    const std::uint64_t serial = uploads_.submitted(u, staging_bytes);
    // What is done goes; past the staging cap (a zone's whole upload at once), the oldest are waited for.
    collect_retired();
    while (uploads_.bytes_in_flight() > kMaxStagingInFlight && uploads_.pending() > 1) {
        uploads_.wait_oldest(
            [this](const Upload& w) { vk_check(vkWaitForFences(device_, 1, &w.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"); },
            [this](const Upload& w) { recycle_upload(w); });
        retired_.collect({frames_done_, uploads_.completed()});
    }
    return serial;
}

void VkContext::release_finished_uploads(std::uint64_t budget_bytes) {
    // Over the budget: wait for the oldest upload, so its staging goes now rather than at the next frame.
    while (budget_bytes > 0 && uploads_.bytes_in_flight() > budget_bytes && uploads_.pending() > 1) {
        uploads_.wait_oldest(
            [this](const Upload& w) { vk_check(vkWaitForFences(device_, 1, &w.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"); },
            [this](const Upload& w) { recycle_upload(w); });
        retired_.collect({frames_done_, uploads_.completed()});
    }
    collect_retired();
}

void VkContext::immediate_submit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool = upload_pool_;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device_, &a, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    record(cmd);
    vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vk_check(vkCreateFence(device_, &fi, nullptr, &fence), "vkCreateFence");
    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    vk_check(vkQueueSubmit2(queue_, 1, &submit, fence), "vkQueueSubmit2");
    vk_check(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, upload_pool_, 1, &cmd);
}

std::vector<VkContext::HeapUse> VkContext::heap_use() const {
    std::vector<HeapUse> out;
    if (!allocator_) return out;
    const VkPhysicalDeviceMemoryProperties* mem = nullptr;
    vmaGetMemoryProperties(allocator_, &mem);
    if (!mem) return out;
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator_, budgets.data());
    for (std::uint32_t h = 0; h < mem->memoryHeapCount; ++h) {
        const VmaBudget& b = budgets[h];
        out.push_back({(mem->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0, b.usage, b.budget,
                       b.statistics.allocationBytes, b.statistics.allocationCount});
    }
    return out;
}

bool VkContext::request_screenshot(std::filesystem::path path, std::uint32_t max_width) {
    if (!screenshots_supported_) {
        log::warn("screenshot {} not taken: the swapchain's images cannot be copied from or are not 8-bit RGBA",
                  path.string());
        return false;
    }
    screenshot_path_ = std::move(path);
    screenshot_max_width_ = max_width;
    return true;
}

std::optional<FrameInfo> VkContext::begin_frame_commands() {
    if (gpu_shared_cache_) {
        gpu_shared_cache_->keep_alive();
        if (gpu_shared_cache_->needs_localize()) localize_shared_textures();
    }
    if (window_.minimized()) return std::nullopt;

    std::uint32_t w = 0, h = 0;
    window_.framebuffer_size(w, h);
    if (w == 0 || h == 0) return std::nullopt;
    std::optional<Extent> surface;
    if (!resize_pending_ && surface_watch_ > 0) {
        --surface_watch_;
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) == VK_SUCCESS &&
            caps.currentExtent.width != UINT32_MAX)
            surface = Extent{caps.currentExtent.width, caps.currentExtent.height};
    }
    if (swapchain_outdated({w, h}, {made_for_.width, made_for_.height}, surface, {extent_.width, extent_.height}))
        resize_pending_ = true;
    if (resize_pending_ && !recreate_swapchain()) return std::nullopt;
    // The 3D view's rectangle (set_scene_rect): a new size makes the scene targets again, once it has settled
    // (render/swapchain_size.h SceneResizeDebounce): dragging a /viewport edge would otherwise wait for the device
    // and rebuild every target every frame. Until then the old rectangle is drawn whole; a move alone is taken at
    // once.
    if (VkRect2D area = scene_area(); scene_resize_.update(Extent{area.extent.width, area.extent.height},
                                                           Extent{view_extent_.width, view_extent_.height})) {
        wait_idle();
        destroy_scene_targets();
        create_scene_targets();
    } else if (area.extent.width == view_extent_.width && area.extent.height == view_extent_.height) {
        scene_area_ = area;
    }

    Frame& f = frames_[frame_index_];
    vk_check(vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    // That fence was the frame kFramesInFlight before this one's: it and every frame before it are done.
    frames_done_ = std::max(frames_done_, frames_done_at(frames_begun_ + 1, kFramesInFlight));
    collect_retired();

    std::uint32_t image_index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.image_available, VK_NULL_HANDLE, &image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        resize_pending_ = true;
        return std::nullopt;
    }
    if (r != VK_SUBOPTIMAL_KHR) vk_check(r, "vkAcquireNextImageKHR");

    vk_check(vkResetFences(device_, 1, &f.in_flight), "vkResetFences");
    vk_check(vkResetCommandBuffer(f.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(f.cmd, &begin), "vkBeginCommandBuffer");

    ++frames_begun_;
    recording_ = true;
    // The passes' GPU time: the slot's last frame is done (its fence above), so its timestamps are read, then the
    // slot starts again.
    read_gpu_marks(frame_index_);
    if (gpu_timing_ && !gpu_queries_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = (kMaxGpuMarks + 1) * kFramesInFlight;
        if (vkCreateQueryPool(device_, &qi, nullptr, &gpu_queries_) != VK_SUCCESS) {
            gpu_queries_ = VK_NULL_HANDLE;
            gpu_queries_failed_ = true;
            gpu_timing_ = false;
        }
    }
    if (gpu_timing_ && gpu_queries_) {
        const std::uint32_t first = (kMaxGpuMarks + 1) * frame_index_;
        vkCmdResetQueryPool(f.cmd, gpu_queries_, first, kMaxGpuMarks + 1);
        vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpu_queries_, first);
        gpu_started_[frame_index_] = true;
    }
    // The temporal upscaler's jitter for this frame: a cycle as long as the upscale asks (render/temporal.h).
    if (temporal()) {
        const float scale = static_cast<float>(scene_extent_.width) / static_cast<float>(std::max(view_extent_.width, 1u));
        jitter_px_ = jitter_offset(frames_begun_, jitter_phases(scale));
    } else {
        jitter_px_ = glm::vec2(0.0f);
    }
    jitter_ndc_ = jitter_ndc(jitter_px_, scene_extent_.width, scene_extent_.height);
    temporal_view_.reset();
    return FrameInfo{f.cmd, scene_extent_, scene_area_.offset, image_index, frame_index_};
}

std::optional<FrameInfo> VkContext::begin_frame(const glm::vec4& clear_color) {
    auto frame = begin_frame_commands();
    if (frame) begin_scene(*frame, clear_color);
    return frame;
}

void VkContext::begin_scene(const FrameInfo& frame, const glm::vec4& clear_color) {
    VkCommandBuffer cmd = frame.cmd;
    // The scene target is shared by the frames in flight: the previous frame's post pass read it.
    constexpr VkPipelineStageFlags2 kColorStages =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    image_barrier(cmd, scene_resolved_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorStages, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (scene_msaa_.image)
        image_barrier(cmd, scene_msaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // The depth targets: the previous frame's AO and post passes read them.
    const auto depth_sync = depth_frame_sync(scene_samples_);
    auto prepare_depth = [&](VkImage image, AttachmentSync sync) {
        image_barrier(cmd, image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      sync.stages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, sync.access, sync.stages, sync.access);
    };
    prepare_depth(depth_.image, depth_sync.attachment);
    if (depth_resolved_.image) prepare_depth(depth_resolved_.image, depth_sync.sampled);

    // begin_translucent's copies are read only after it writes them; until then their contents do not matter.
    for (VkImage copy : {water_color_.image, water_depth_.image})
        image_barrier(cmd, copy, copy == water_depth_.image ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The object motion and the reactive mask (under scene_motion()): the previous frame's prep pass read them.
    for (const Image* extra : {&object_motion_, &reactive_})
        if (extra->image)
            image_barrier(cmd, extra->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorStages, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    write_temporal_block(cmd);
    begin_scene_rendering(cmd, false, clear_color);
    scene_open_ = true;
}

void VkContext::write_temporal_block(VkCommandBuffer cmd) {
    // This frame's camera and the last one's (set_temporal_view; the same without a history, so only what moves on
    // its own has a motion), for the scene shaders' motion (shaders/motion.glsl).
    TemporalBlock block;
    if (temporal_view_) {
        block.view_proj = temporal_view_->proj * temporal_view_->view;
        block.prev_view_proj = history_valid_ && !camera_cut(prev_view_.view, temporal_view_->view)
            ? prev_view_.proj * prev_view_.view : block.view_proj;
    }
    block.info.x = object_motion_.image ? 1.0f : 0.0f;
    // After the last frame's scene read it; before this frame's does.
    constexpr VkPipelineStageFlags2 kReaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    // vkCmdUpdateBuffer is a transfer command of its own stage (not a copy): ALL_TRANSFER covers it.
    bb.srcStageMask = kReaders | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; // the last frame's reads, and its own update
    bb.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    bb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = temporal_buffer_;
    bb.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &bb;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdUpdateBuffer(cmd, temporal_buffer_, 0, sizeof(block), &block);
    bb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.dstStageMask = kReaders;
    bb.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void VkContext::begin_scene_rendering(VkCommandBuffer cmd, bool load, const glm::vec4& clear_color) {
    auto linear = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.clearValue.color = {{linear(clear_color.r), linear(clear_color.g), linear(clear_color.b), clear_color.a}};
    // Stored, multisampled too: begin_translucent goes on drawing into the same targets.
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (scene_msaa_.image) {
        color.imageView = scene_msaa_.view;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = scene_resolved_.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        color.imageView = scene_resolved_.view;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depth_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.clearValue.depthStencil = {0.0f, 0}; // reverse Z
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (depth_resolved_.image) {
        depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        depth.resolveImageView = depth_resolved_.view;
        depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    // Under scene_motion(): the object motion (cleared to kMotionUnset) and the reactive mask (cleared to 0).
    VkRenderingAttachmentInfo colors[3] = {color, color, color};
    std::uint32_t count = 1;
    if (object_motion_.image) {
        for (std::uint32_t i = 1; i < 3; ++i) {
            colors[i].imageView = i == 1 ? object_motion_.view : reactive_.view;
            colors[i].resolveMode = VK_RESOLVE_MODE_NONE;
            colors[i].resolveImageView = VK_NULL_HANDLE;
        }
        colors[1].clearValue.color = {{kMotionUnset, kMotionUnset, 0.0f, 0.0f}};
        colors[2].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        count = 3;
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, scene_extent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = count;
    rendering.pColorAttachments = colors;
    rendering.pDepthAttachment = &depth;
    // Drawn from secondary command buffers (set_scene_secondary): the primary then holds only their execution, and
    // each secondary sets the dynamic state itself (set_scene_state).
    if (scene_secondary_) rendering.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    vkCmdBeginRendering(cmd, &rendering);
    if (!scene_secondary_) set_scene_state(cmd);
}

void VkContext::set_scene_state(VkCommandBuffer cmd) const {
    // Flip Y so that +Y is up in clip space, as in GL-style projections.
    VkViewport vp{0.0f, static_cast<float>(scene_extent_.height), static_cast<float>(scene_extent_.width),
                  -static_cast<float>(scene_extent_.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, scene_extent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

VkCommandBufferInheritanceRenderingInfo VkContext::scene_inheritance() const {
    // As begin_scene_rendering renders: the colour, and under the motion targets the object motion and the reactive
    // mask; the depth alone (no stencil attachment, whatever the depth format holds).
    VkCommandBufferInheritanceRenderingInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    info.colorAttachmentCount = object_motion_.image ? 3u : 1u;
    info.pColorAttachmentFormats = scene_color_formats_.data();
    info.depthAttachmentFormat = depth_format_;
    info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    info.rasterizationSamples = scene_samples_;
    return info;
}

void VkContext::begin_translucent(const FrameInfo& frame) {
    if (!scene_open_) return;
    VkCommandBuffer cmd = frame.cmd;
    vkCmdEndRendering(cmd);
    // Recorded on the workers, the scene's rendering takes no timestamps between its buffers (the client's own marks
    // are left out then): its opaque part is timed here as one.
    if (scene_secondary_) gpu_mark(frame, "scene opaque (worker-recorded)");
    // The resolved colour and depth (sample 0) into the copies the water samples.
    const Image& depth = depth_resolved_.image ? depth_resolved_ : depth_;
    const auto depth_sync = depth_frame_sync(scene_samples_);
    struct Copy { VkImage from, to; VkImageAspectFlags aspect; VkImageLayout layout; VkPipelineStageFlags2 stages;
                  VkAccessFlags2 access; };
    const Copy copies[2] = {
        {scene_resolved_.image, water_color_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        {depth.image, water_depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
         depth_sync.sampled.stages, depth_sync.sampled.access},
    };
    for (const Copy& c : copies) {
        image_barrier(cmd, c.from, c.aspect, c.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.stages, c.access,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        image_barrier(cmd, c.to, c.aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = {c.aspect, 0, 0, 1};
        region.dstSubresource = {c.aspect, 0, 0, 1};
        region.extent = {scene_extent_.width, scene_extent_.height, 1};
        vkCmdCopyImage(cmd, c.from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &region);
        image_barrier(cmd, c.to, c.aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        // Back to an attachment: the next rendering loads (the single-sampled targets) or resolves into it again.
        image_barrier(cmd, c.from, c.aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.layout,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, c.stages, c.access);
    }
    // The object motion and the reactive mask go on as they are, as do the multisampled targets (stored by the first
    // rendering, loaded by the next).
    for (const Image* extra : {&object_motion_, &reactive_})
        if (extra->image)
            image_barrier(cmd, extra->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (scene_msaa_.image)
        image_barrier(cmd, scene_msaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (depth_resolved_.image)
        image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_sync.attachment.stages,
                      depth_sync.attachment.access, depth_sync.attachment.stages, depth_sync.attachment.access);
    begin_scene_rendering(cmd, true, glm::vec4(0.0f));
}

void VkContext::begin_overlay(const FrameInfo& frame) {
    if (!scene_open_) return;
    scene_open_ = false;
    const bool secondary = scene_secondary_;
    scene_secondary_ = false; // the next frame's scene says again (set_scene_secondary)
    // This frame's AO view (set_ao_view), used up here.
    const std::optional<AoView> taken = ao_view_;
    ao_view_.reset();
    const std::optional<SunShafts> shafts = shafts_;
    shafts_.reset();
    const bool zone_fog = taken && zone_fog_view_ && zone_fog_set_ && zone_fog_pipeline_;
    const std::optional<glm::vec4> mood = mood_;
    mood_.reset();
    // The temporal upscaler runs in frames that gave their camera (set_temporal_view); any other drops the history.
    const std::optional<TemporalView> view = temporal_view_;
    temporal_view_.reset();
    const bool taa = view && temporal() && prep_pipeline_;
    if (!taa) history_valid_ = false;
    VkCommandBuffer cmd = frame.cmd;
    vkCmdEndRendering(cmd);
    // As begin_translucent's: the rest of a worker-recorded scene timed as one, its resolve included.
    gpu_mark(frame, secondary ? "scene (worker-recorded)" : "scene end (resolve)");

    const bool ao = taken && scene_options_.ambient_occlusion && ao_pipeline_;
    // The depth for the AO and the post pass (a depth resolve runs in the colour output stage).
    const Image& depth = depth_resolved_.image ? depth_resolved_ : depth_;
    const auto depth_sync = depth_frame_sync(scene_samples_).sampled;
    image_barrier(cmd, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth_sync.stages, depth_sync.access,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (ao && depth_resolved_.image) {
        // An unconfirmed driver workaround: this repeats the scopes of the depth image barrier just above
        // (frame_sync.h depth_frame_sync(...).sampled: the resolve's colour-output writes before fragment sampled
        // reads), so by the Vulkan spec it adds nothing. It is kept because the AO flicker over grass on Radeon
        // RX 9070 XT was reported gone with it, but the same change also stopped faded lawn blades sending NaN
        // (shaders/lawn.vert blade_at), the more likely cure. docs/unknowns.md "World rendering" says what would
        // settle it.
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
    // The ambient occlusion (shaders/ao.frag), when this frame's view was given.
    float depth_x = 0.0f, depth_y = 0.0f; // view depth = y / (d + x), reverse Z (FlyCamera::projection)
    if (taken) {
        float n = taken->near_plane, f = taken->far_plane;
        depth_x = n / (f - n);
        depth_y = f * n / (f - n);
    }
    // The fog past the zone's edge, over the scene before anything reads it (render/zone_fog.h).
    if (zone_fog) {
        draw_zone_fog(frame, *taken, depth_x, depth_y);
        gpu_mark(frame, "zone fog");
    }
    zone_fog_view_.reset();
    image_barrier(cmd, ao_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (ao) {
        VkRenderingAttachmentInfo target{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        target.imageView = ao_.view;
        target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        target.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
        r.renderArea = {{0, 0}, scene_extent_};
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &target;
        vkCmdBeginRendering(cmd, &r);
        VkViewport avp{0.0f, static_cast<float>(scene_extent_.height), static_cast<float>(scene_extent_.width),
                       -static_cast<float>(scene_extent_.height), 0.0f, 1.0f};
        VkRect2D ascissor{{0, 0}, scene_extent_};
        vkCmdSetViewport(cmd, 0, 1, &avp);
        vkCmdSetScissor(cmd, 0, 1, &ascissor);
        AoPush push;
        push.projection = glm::vec4(taken->tan_half_x, taken->tan_half_y, depth_x, depth_y);
        push.params = glm::vec4(taken->radius, taken->intensity, 0.1f * static_cast<float>(scene_extent_.height),
                                static_cast<float>(post_frame_ % 64u));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 0, 1, &ao_set_, 0, nullptr);
        vkCmdPushConstants(cmd, ao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        gpu_mark(frame, "ambient occlusion");
    }
    image_barrier(cmd, ao_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    image_barrier(cmd, scene_resolved_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (const Image* extra : {&object_motion_, &reactive_})
        if (extra->image)
            image_barrier(cmd, extra->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The bloom, with the AO's frames; otherwise its first level is only made readable (the post set holds it).
    const bool bloom = taken && scene_options_.bloom > 0.0f && bloom_down_pipeline_;
    if (bloom) {
        draw_bloom(cmd);
        gpu_mark(frame, "bloom");
    } else {
        image_barrier(cmd, bloom_[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    // The 3D view fills its rectangle (/viewport, set_scene_rect); the rest of the image is black under the UI.
    const VkRect2D area = scene_area_;
    PostPush push;
    push.exposure = std::exp2(scene_options_.exposure);
    push.tonemap = static_cast<std::uint32_t>(scene_options_.tonemap);
    push.flags = (swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB
                      ? 0u : kPostEncodeOutput) |
                 (scene_options_.dither ? kPostDither : 0u) | (ao && !taa ? kPostAo : 0u) |
                 (taken && scene_options_.distance_blur ? kPostBlur : 0u);
    push.frame = post_frame_++;
    push.depth = glm::vec4(depth_x, depth_y, scene_options_.ao_strength, taken ? taken->far_plane : 0.0f);
    // The bloom is mixed in at up to 8 %.
    push.grade = glm::vec4(scene_options_.brightness, scene_options_.contrast, std::max(scene_options_.gamma, 0.1f),
                           bloom ? std::min(scene_options_.bloom, 1.0f) * 0.08f : 0.0f);
    push.offset = glm::vec4(static_cast<float>(area.offset.x), static_cast<float>(area.offset.y), 0.0f, 0.0f);
    if (taken && mood && scene_options_.color_grade) push.mood = *mood;
    if (taken && shafts && shafts->strength > 0.0f) {
        push.sun = glm::vec4(shafts->uv, shafts->strength, 0.0f);
        push.sun_color = glm::vec4(shafts->color, 0.0f);
    }
    // The temporal upscaler: temporal_prep.frag applies the AO and writes the motion at the scene's size,
    // taa_resolve.frag accumulates that into this frame's history at the view's, and the post pass below reads the
    // history in the scene's place (post_taa_sets_), at the view's size.
    VkDescriptorSet post_set = post_set_;
    // Under FSR 3.1 its upscaler takes the resolve's place (render/fsr3_upscaler.h), on the same inputs; so does an
    // upscaler provider (render/upscaler_provider.h) under External.
    const bool external = taa && scene_options_.upscaler == Upscaler::External && provider_ && !provider_failed_ &&
                          fsr3_output_.image;
    const bool fsr3 = taa && !external && scene_options_.upscaler == Upscaler::Fsr3 && fsr3_ && fsr3_output_.image;
    bool kept_history = false; // the temporal history went on from the last frame's (frame generation's test)
    if (taa) {
        const bool keep = history_valid_ && !camera_cut(prev_view_.view, view->view);
        kept_history = keep;
        fg_camera_steady_ = !history_valid_ || !camera_cut(prev_view_.view, view->view, kFrameGenMaxStep, kHistoryMaxTurn);
        PrepPush prep;
        if (history_valid_) prep.reproject = reprojection(prev_view_.view, prev_view_.proj, view->view, view->proj);
        prep.depth = glm::vec4(depth_x, depth_y, scene_options_.ao_strength, ao ? 1.0f : 0.0f);
        // z: the draws' own motion and the reactive mask were drawn (scene_motion()); else those bindings hold the AO.
        prep.jitter = glm::vec4(jitter_ndc_, object_motion_.image ? 1.0f : 0.0f, 0.0f);
        const Image* prep_targets[2] = {&lit_, &motion_};
        draw_into(cmd, prep_targets, scene_extent_, prep_pipeline_, prep_layout_, post_set_, &prep, sizeof(prep));
        if (fsr3 || external) {
            const float scale = static_cast<float>(scene_extent_.width) / static_cast<float>(std::max(view_extent_.width, 1u));
            const std::uint32_t phases = jitter_phases(scale);
            // The camera's planes and vertical field of view, from the plain projection (FlyCamera::projection:
            // reverse Z, p[2][2] = n / (f - n), p[3][2] = f n / (f - n)).
            float near_plane = 1.0f, far_plane = 1000.0f, fov_y = 1.0f;
            const glm::mat4& pr = view->proj;
            if (pr[2][2] > 0.0f && pr[1][1] > 0.0f) {
                far_plane = pr[3][2] / pr[2][2];
                near_plane = pr[2][2] * far_plane / (1.0f + pr[2][2]);
                fov_y = 2.0f * std::atan(1.0f / pr[1][1]);
            }
            const auto now = std::chrono::steady_clock::now();
            const float sharpness = std::clamp(scene_options_.sharpness, 0.0f, 1.0f);
            if (fsr3) {
                Fsr3Upscaler::Inputs in;
                in.color = lit_.view;
                in.depth = depth.view;
                in.motion = motion_.view;
                // The reactive mask (R8 at the scene's size, 0..1, SHADER_READ_ONLY_OPTIMAL): the blended and additive
                // draws' (scene_motion()); none without independentBlend, FSR's default 0.
                in.reactive = reactive_.view;
                in.output = fsr3_output_.image;
                in.output_view = fsr3_output_.view;
                Fsr3Upscaler::Params fp;
                fp.frame_slot = frame.frame_slot;
                fp.jitter = jitter_px_;
                fp.jitter_phases = phases;
                fp.near_plane = near_plane;
                fp.far_plane = far_plane;
                fp.fov_y = fov_y;
                if (fsr3_last_.time_since_epoch().count() != 0)
                    fp.frame_seconds = std::chrono::duration<float>(now - fsr3_last_).count();
                fsr3_last_ = now;
                fp.reset = !keep;
                fp.sharpness = sharpness;
                fsr3_->dispatch(cmd, in, fp);
            } else {
                run_provider(cmd, frame, depth, keep, phases, near_plane, far_plane, fov_y, push.exposure, sharpness, now);
            }
            post_set = post_fsr3_set_;
        } else {
            // The history read: one never written (or dropped) is only made readable; the resolve does not look at it.
            const Image& prev = history_[history_index_ ^ 1];
            if (!keep)
                image_barrier(cmd, prev.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            ResolvePush resolve;
            resolve.sizes = glm::vec4(static_cast<float>(scene_extent_.width), static_cast<float>(scene_extent_.height),
                                      static_cast<float>(view_extent_.width), static_cast<float>(view_extent_.height));
            resolve.jitter = glm::vec4(jitter_px_, push.exposure, keep ? 0.0f : 1.0f);
            resolve.params = glm::vec4(kHistoryMaxWeight, 0.0f, 0.0f, 0.0f);
            draw_into(cmd, history_[history_index_], view_extent_, resolve_pipeline_, resolve_layout_,
                      resolve_sets_[history_index_], &resolve, sizeof(resolve));
            post_set = post_taa_sets_[history_index_];
            history_index_ ^= 1;
        }
        prev_view_ = *view;
        history_valid_ = true;
    }
    // Below a render scale of 1 without it, FSR 1: the post pass's picture at the scene's size (encoded, as EASU wants
    // it; the dither waits for the last pass, or EASU would enlarge it), EASU's upscale of it to the view's size, then
    // RCAS sharpens that into the swapchain below. Under the temporal upscaler RCAS alone sharpens the post pass's
    // picture at the view's size, when sharpness is above 0; under FSR 3.1 its own RCAS has done it.
    const bool fsr = !taa && upscaling() && post_pipeline_;
    // A provider that sharpens itself (HAILSTONE_UPSCALER_SHARPENS) is as FSR 3.1; one that does not gets RCAS.
    const bool own_sharpening = fsr3 || (external && provider_ && provider_->sharpens());
    const bool sharpen = fsr || (taa && !own_sharpening && scene_options_.sharpness > 0.0f && upscaled_.image);
    UpscalePush up;
    if (taa && sharpen) {
        PostPush source = push;
        source.flags = (push.flags & ~kPostDither) | kPostEncodeOutput;
        source.offset = glm::vec4(0.0f);
        draw_into(cmd, upscaled_, view_extent_, post_upscale_pipeline_, post_layout_, post_set, &source, sizeof(source));
    }
    if (sharpen) {
        up.offset = push.offset;
        up.sharpness = std::clamp(scene_options_.sharpness, 0.0f, 1.0f);
        up.flags = push.flags & (kPostEncodeOutput | kPostDither);
        up.frame = push.frame;
    }
    if (fsr) {
        PostPush source = push;
        source.flags = (push.flags & ~kPostDither) | kPostEncodeOutput;
        source.offset = glm::vec4(0.0f);
        draw_into(cmd, upscale_source_, scene_extent_, post_upscale_pipeline_, post_layout_, post_set_, &source,
                  sizeof(source));
        up.sizes = glm::vec4(static_cast<float>(scene_extent_.width), static_cast<float>(scene_extent_.height),
                             static_cast<float>(view_extent_.width), static_cast<float>(view_extent_.height));
        draw_into(cmd, upscaled_, view_extent_, easu_pipeline_, upscale_layout_, easu_set_, &up, sizeof(up));
    }

    if (frame_gen_active()) {
        // Frame generation: the picture without the UI into this frame's picture (display-encoded, dithered), and the
        // UI into its own image below instead of the swapchain; end_frame composites them (finish_frame_gen).
        const Image& picture = pictures_[picture_index_];
        if (sharpen) {
            UpscalePush into = up;
            into.offset = glm::vec4(0.0f);
            into.flags = kPostEncodeOutput | (push.flags & kPostDither);
            draw_into(cmd, picture, view_extent_, rcas_picture_pipeline_, upscale_layout_, rcas_set_, &into, sizeof(into));
        } else {
            PostPush into = push;
            into.flags |= kPostEncodeOutput;
            into.offset = glm::vec4(0.0f);
            draw_into(cmd, picture, view_extent_, post_upscale_pipeline_, post_layout_, post_set, &into, sizeof(into));
        }
        fg_temporal_ = taa;
        fg_history_kept_ = kept_history;
        // The UI's image: cleared to nothing; the UI's blending leaves its colour premultiplied and its alpha the
        // coverage (fg_composite.frag).
        image_barrier(cmd, overlay_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ui{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ui.imageView = overlay_.view;
        ui.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ui.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ui.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        ui.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
        r.renderArea = {{0, 0}, extent_};
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &ui;
        vkCmdBeginRendering(cmd, &r);
        VkViewport uvp{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y + static_cast<std::int32_t>(area.extent.height)),
                       static_cast<float>(area.extent.width), -static_cast<float>(area.extent.height), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &uvp);
        vkCmdSetScissor(cmd, 0, 1, &area);
        overlay_open_ = true;
        return;
    }

    image_barrier(cmd, images_[frame.image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const bool whole = area.extent.width == extent_.width && area.extent.height == extent_.height;
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = views_[frame.image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = whole ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR; // the post pass covers it
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);

    VkViewport vp{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y + static_cast<std::int32_t>(area.extent.height)),
                  static_cast<float>(area.extent.width), -static_cast<float>(area.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &area);
    if (sharpen) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rcas_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscale_layout_, 0, 1, &rcas_set_, 0, nullptr);
        vkCmdPushConstants(cmd, upscale_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(up), &up);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    } else if (post_pipeline_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &post_set, 0, nullptr);
        vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    gpu_mark(frame, "upscale and post");
}

void VkContext::draw_into(VkCommandBuffer cmd, const Image& target, VkExtent2D extent, VkPipeline pipeline,
                          VkPipelineLayout layout, VkDescriptorSet set, const void* push, std::uint32_t push_size) {
    const Image* targets[1] = {&target};
    draw_into(cmd, targets, extent, pipeline, layout, set, push, push_size);
}

void VkContext::draw_into(VkCommandBuffer cmd, std::span<const Image* const> targets, VkExtent2D extent,
                          VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, const void* push,
                          std::uint32_t push_size) {
    // At most kMaxDrawTargets attachments (the temporal prep writes two): a fixed array, as this runs several
    // times a frame.
    std::array<VkRenderingAttachmentInfo, kMaxDrawTargets> atts{};
    std::uint32_t att_count = 0;
    if (targets.size() > kMaxDrawTargets) throw std::logic_error("draw_into: too many targets");
    for (const Image* target : targets) {
        // The previous frame read it.
        image_barrier(cmd, target->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = target->view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[att_count++] = att;
    }
    VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
    r.renderArea = {{0, 0}, extent};
    r.layerCount = 1;
    r.colorAttachmentCount = att_count;
    r.pColorAttachments = atts.data();
    vkCmdBeginRendering(cmd, &r);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    for (const Image* target : targets)
        image_barrier(cmd, target->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VkContext::draw_bloom(VkCommandBuffer cmd) {
    auto pass = [&](VkPipeline pipeline, std::uint32_t target, std::uint32_t source_set, VkExtent2D source,
                    bool load, bool first) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = bloom_[target].view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const VkExtent2D e = bloom_extent_[target];
        VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
        r.renderArea = {{0, 0}, e};
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &r);
        VkViewport vp{0.0f, 0.0f, static_cast<float>(e.width), static_cast<float>(e.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, e};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        BloomPush push;
        push.texel = glm::vec2(1.0f / static_cast<float>(source.width), 1.0f / static_cast<float>(source.height));
        push.target = glm::vec2(1.0f / static_cast<float>(e.width), 1.0f / static_cast<float>(e.height));
        push.first = first ? 1u : 0u;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &bloom_sets_[source_set], 0,
                                nullptr);
        vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        image_barrier(cmd, bloom_[target].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    // Down: the scene into level 0, each level into the next.
    for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
        image_barrier(cmd, bloom_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        pass(bloom_down_pipeline_, i, i, i == 0 ? scene_extent_ : bloom_extent_[i - 1], false, i == 0);
    }
    // Up: each level spread onto the one above it.
    for (std::uint32_t i = kBloomLevels - 1; i-- > 0;) {
        image_barrier(cmd, bloom_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        pass(bloom_up_pipeline_, i, i + 2, bloom_extent_[i + 1], true, false);
    }
}

void VkContext::create_frame_gen(const std::filesystem::path& shader_dir) {
    frame_gen_ = std::make_unique<FrameGen>(device_, allocator_, pipeline_cache_, shader_dir);
    VkShaderModule vs = load_module(device_, shader_dir / "post.vert.spv");
    VkShaderModule synth = load_module(device_, shader_dir / "fg_synth.frag.spv");
    VkShaderModule composite = load_module(device_, shader_dir / "fg_composite.frag.spv");
    VkShaderModule rcas = load_module(device_, shader_dir / "fsr_rcas.frag.spv");
    fg_synth_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, frame_gen_->synth_layout(), vs, synth, kUpscaleFormat);
    fg_composite_pipeline_ =
        fullscreen_pipeline(device_, pipeline_cache_, frame_gen_->composite_layout(), vs, composite, swapchain_format_);
    rcas_picture_pipeline_ = fullscreen_pipeline(device_, pipeline_cache_, upscale_layout_, vs, rcas, kUpscaleFormat);
    for (VkShaderModule m : {vs, synth, composite, rcas}) vkDestroyShaderModule(device_, m, nullptr);
    if (timestamp_period_ns_ > 0.0f) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kFramesInFlight;
        if (vkCreateQueryPool(device_, &qi, nullptr, &fg_queries_) != VK_SUCCESS) fg_queries_ = VK_NULL_HANDLE;
    }
    fg_query_state_.fill(0);
    fg_stats_ = FrameGenStats{};
}

void VkContext::destroy_frame_gen() {
    for (VkPipeline* p : {&fg_synth_pipeline_, &fg_composite_pipeline_, &rcas_picture_pipeline_}) {
        if (*p) vkDestroyPipeline(device_, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    frame_gen_.reset();
    if (fg_queries_) vkDestroyQueryPool(device_, fg_queries_, nullptr);
    fg_queries_ = VK_NULL_HANDLE;
    picture_ready_ = false;
}

std::string VkContext::fsr3_timing() const {
    return fsr3_ ? fsr3_->timing_summary() : std::string{};
}

void VkContext::read_frame_gen_time(std::uint32_t frame_slot) {
    // The slot's last frame is done (begin_frame waited for its fence), so its timestamps are there unless the
    // device dropped them; nothing is waited for.
    if (!fg_queries_ || fg_query_state_[frame_slot] == 0) return;
    std::array<std::uint64_t, 2> t{};
    if (vkGetQueryPoolResults(device_, fg_queries_, 2 * frame_slot, 2, sizeof t, t.data(), sizeof(std::uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || t[1] < t[0])
        return;
    const float ms = static_cast<float>(static_cast<double>(t[1] - t[0]) * timestamp_period_ns_ / 1.0e6);
    (fg_query_state_[frame_slot] == 2 ? fg_stats_.gpu_ms : fg_stats_.gpu_real_ms) = ms;
}

void VkContext::gpu_mark(const FrameInfo& frame, const char* name) {
    if (!recording_ || !gpu_timing_ || !gpu_started_[frame.frame_slot]) return;
    auto& marks = gpu_marks_[frame.frame_slot];
    if (marks.size() >= kMaxGpuMarks) return;
    marks.push_back(name);
    vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpu_queries_,
                         (kMaxGpuMarks + 1) * frame.frame_slot + static_cast<std::uint32_t>(marks.size()));
}

void VkContext::read_gpu_marks(std::uint32_t frame_slot) {
    auto& marks = gpu_marks_[frame_slot];
    const bool started = gpu_started_[frame_slot];
    gpu_started_[frame_slot] = false;
    if (!started || marks.empty() || !gpu_queries_) {
        marks.clear();
        return;
    }
    // Done, as read_frame_gen_time's: nothing is waited for, and a result the device did not give is dropped.
    std::array<std::uint64_t, kMaxGpuMarks + 1> ticks{};
    const auto n = static_cast<std::uint32_t>(marks.size()) + 1u;
    if (vkGetQueryPoolResults(device_, gpu_queries_, (kMaxGpuMarks + 1) * frame_slot, n, sizeof(std::uint64_t) * n,
                              ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        render::gpu_sections(marks, std::span(ticks).first(n), timestamp_period_ns_, timestamp_bits_, gpu_sections_);
        ++gpu_sections_serial_;
    }
    marks.clear();
}

FrameGenStats VkContext::frame_gen_stats() const {
    FrameGenStats s = fg_stats_;
    s.requested = frame_gen_requested_;
    s.active = frame_gen_active();
    s.no_motion = frame_gen_requested_ && !scene_options_.frame_gen;
    s.images = static_cast<std::uint32_t>(images_.size());
    s.min_images = min_image_count_;
    s.waits = frame_gen_fits(s.images, s.min_images);
    s.present_mode = present_mode_;
    return s;
}

void VkContext::bind_frame_gen() {
    if (!frame_gen_ || !overlay_.image) return;
    FrameGen::Views v;
    v.depth = (depth_resolved_.image ? depth_resolved_ : depth_).view;
    v.motion = motion_.view;
    v.reactive = reactive_.view;
    v.placeholder = ao_.view;
    v.pictures = {pictures_[0].view, pictures_[1].view};
    v.generated = generated_.view;
    v.overlay = overlay_.view;
    v.nearest = post_sampler_;
    v.linear = post_linear_sampler_;
    frame_gen_->bind(v);
}

void VkContext::composite_into(VkCommandBuffer cmd, std::uint32_t image_index, VkDescriptorSet set) {
    image_barrier(cmd, images_[image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = views_[image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the composite covers the whole image
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
    r.renderArea = {{0, 0}, extent_};
    r.layerCount = 1;
    r.colorAttachmentCount = 1;
    r.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &r);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    FrameGen::CompositePush push;
    push.area[0] = static_cast<float>(scene_area_.offset.x);
    push.area[1] = static_cast<float>(scene_area_.offset.y);
    push.area[2] = static_cast<float>(scene_area_.extent.width);
    push.area[3] = static_cast<float>(scene_area_.extent.height);
    push.flags = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB ? 1u : 0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fg_composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, frame_gen_->composite_layout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, frame_gen_->composite_layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

std::optional<std::uint32_t> VkContext::finish_frame_gen(const FrameInfo& frame, bool readback) {
    VkCommandBuffer cmd = frame.cmd;
    image_barrier(cmd, overlay_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const std::uint32_t slot = picture_index_;
    read_frame_gen_time(frame.frame_slot);
    if (fg_queries_) {
        vkCmdResetQueryPool(cmd, fg_queries_, 2 * frame.frame_slot, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, fg_queries_, 2 * frame.frame_slot);
    }
    ++fg_stats_.real;
    fg_stats_.pacing_wait_ms = 0.0f;
    FrameGenState state;
    state.enabled = true;
    state.temporal_frame = fg_temporal_;
    state.history_kept = fg_history_kept_;
    state.camera_steady = fg_camera_steady_;
    state.previous_ready = picture_ready_;
    std::optional<std::uint32_t> generated;
    const bool ready = frame_gen_ready(state);
    fg_generated_last_ = false;
    if (ready || frame_gen_repeat(state)) {
        // A second image for the in-between frame (or the real one again). With min + 2 images it may be waited for (FIFO frees one each
        // refresh: that wait is what holds the game at half the refresh); with fewer, only taken when free.
        const std::uint64_t timeout =
            frame_gen_fits(static_cast<std::uint32_t>(images_.size()), min_image_count_) ? UINT64_MAX : 0;
        std::uint32_t index = 0;
        const auto wait_from = std::chrono::steady_clock::now();
        const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, timeout,
                                                 frames_[frame.frame_slot].generated_available, VK_NULL_HANDLE, &index);
        fg_stats_.pacing_wait_ms =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - wait_from).count();
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) resize_pending_ = true;
            if (ready) {
                frame_gen_->record_motion(cmd, kFrameGenTime);
                FrameGen::SynthPush push;
                push.view_w = static_cast<float>(view_extent_.width);
                push.view_h = static_cast<float>(view_extent_.height);
                push.t = kFrameGenTime;
                push.reactive = reactive_.image ? 1.0f : 0.0f;
                draw_into(cmd, generated_, view_extent_, fg_synth_pipeline_, frame_gen_->synth_layout(),
                          frame_gen_->synth_set(slot), &push, sizeof(push));
            }
            composite_into(cmd, index, frame_gen_->composite_set(slot, ready));
            // Its second scope reaches the copy stage when end_frame may read it back: that barrier's first scope
            // (the copy stage) then follows this layout transition. A second scope of NONE orders nothing after it.
            image_barrier(cmd, images_[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          readback ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_NONE, 0);
            generated = index;
            fg_generated_last_ = ready;
            ++(ready ? fg_stats_.generated : fg_stats_.skipped_cut);
        } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            resize_pending_ = true;
            ++fg_stats_.skipped_no_image;
        } else if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            vk_check(r, "vkAcquireNextImageKHR (frame generation)");
        } else {
            ++fg_stats_.skipped_no_image;
        }
    }
    // The real frame, left as the frame's own image always is at this point (the screenshot and the present follow).
    composite_into(cmd, frame.image_index, frame_gen_->composite_set(slot, false));
    if (fg_queries_) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, fg_queries_, 2 * frame.frame_slot + 1);
        // A shown-twice frame (frame_gen_repeat) also sets `generated` but synthesises nothing: only a synthesised
        // in-between frame's GPU time is the generated one.
        fg_query_state_[frame.frame_slot] = fg_generated_last_ ? 2 : 1;
    }
    picture_ready_ = true;
    picture_index_ ^= 1u;
    return generated;
}

FrameOutcome VkContext::end_frame(const FrameInfo& frame) {
    Frame& f = frames_[frame.frame_slot];
    VkImage image = images_[frame.image_index];
    begin_overlay(frame);
    vkCmdEndRendering(frame.cmd);
    gpu_mark(frame, "UI");
    // Frame generation: the UI went into its own image; the real frame and the in-between one are composited now.
    std::optional<std::uint32_t> generated;
    if (overlay_open_) {
        overlay_open_ = false;
        // A --screenshot-generated readback may follow (shot_generated below, which needs this too).
        generated = finish_frame_gen(frame, !screenshot_path_.empty() && screenshot_generated_);
        gpu_mark(frame, "frame generation");
    } else {
        picture_ready_ = false; // a frame without its picture: the next one has nothing to start from
    }
    presented_last_ = generated ? 2u : 1u;

    // Optional screenshot: copy the finished image into a host-visible buffer.
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_alloc = VK_NULL_HANDLE;
    void* readback_data = nullptr;
    if (!screenshot_path_.empty() && !screenshots_supported_) {
        // Asked before a swapchain made without TRANSFER_SRC: the copy would be invalid.
        log::warn("screenshot {} not taken: the swapchain's images cannot be copied from or are not 8-bit RGBA",
                  screenshot_path_.string());
        screenshot_path_.clear();
    }
    // --screenshot-generated: the in-between frame saved as asked and the real one beside it (the two are one frame
    // apart; the real one before the in-between came from the last frame).
    const bool shot_generated =
        !screenshot_path_.empty() && screenshot_generated_ && generated.has_value() && fg_generated_last_;
    const VkDeviceSize shot_bytes = VkDeviceSize{extent_.width} * extent_.height * 4;
    if (!screenshot_path_.empty()) {
        // The command buffer is open: a failure here is logged and the frame goes on without the copy (a throw
        // would leave it half recorded).
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = shot_bytes * (shot_generated ? 2 : 1);
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        if (VkResult made = vmaCreateBuffer(allocator_, &bi, &ai, &readback, &readback_alloc, &out); made != VK_SUCCESS) {
            log::error("screenshot {} not taken: no readback buffer (VkResult {})", screenshot_path_.string(),
                       static_cast<int>(made));
            screenshot_path_.clear();
            readback = VK_NULL_HANDLE;
            readback_alloc = VK_NULL_HANDLE;
        } else {
            readback_data = out.pMappedData;
        }
    }
    if (readback) {
        // The images copied: the frame's own, and first its in-between frame's (already in PRESENT_SRC:
        // finish_frame_gen) with --screenshot-generated.
        // The in-between frame's composite writes were made available by finish_frame_gen's barrier, whose second
        // scope (the copy stage, asked for by its readback flag) this one's first scope chains to; the real frame's
        // are made available here.
        struct Shot {
            VkImage image;
            VkImageLayout layout;
            VkPipelineStageFlags2 src_stage;
            VkAccessFlags2 src_access;
        };
        std::vector<Shot> shots;
        if (shot_generated)
            shots.push_back({images_[*generated], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, 0});
        shots.push_back({image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
        for (std::size_t i = 0; i < shots.size(); ++i) {
            image_barrier(frame.cmd, shots[i].image, VK_IMAGE_ASPECT_COLOR_BIT, shots[i].layout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shots[i].src_stage, shots[i].src_access,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.bufferOffset = shot_bytes * i;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent_.width, extent_.height, 1};
            vkCmdCopyImageToBuffer(frame.cmd, shots[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                                   &region);
        }
        // The copy's writes made available to the host, which reads the buffer after the fence.
        VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback;
        host.offset = 0;
        host.size = VK_WHOLE_SIZE;
        VkDependencyInfo host_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        host_dep.bufferMemoryBarrierCount = 1;
        host_dep.pBufferMemoryBarriers = &host;
        vkCmdPipelineBarrier2(frame.cmd, &host_dep);
        for (const Shot& shot : shots)
            image_barrier(frame.cmd, shot.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        image_barrier(frame.cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    }
    // Neighbour zones and dressed actors may add imports while the frame is recorded. Snapshot only now, so
    // their acquire runs before every recorded draw, and keep owners alive until the release has completed. The
    // barriers cover every live import, not only those this frame samples: which ones a frame samples is not
    // tracked (the renderers bind whole texture arrays), and an ownership barrier on an image in a read-only layout
    // costs little.
    const GpuSharedCache::Snapshot shared = gpu_shared_cache_ ? gpu_shared_cache_->snapshot() : GpuSharedCache::Snapshot{};
    const bool shared_frame = !shared.empty();
    if (shared_frame) {
        if (!f.shared_acquire) {
            // Only a client that imports textures needs these two.
            VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            a.commandPool = f.pool;
            a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            a.commandBufferCount = 2;
            std::array<VkCommandBuffer, 2> commands{};
            vk_check(vkAllocateCommandBuffers(device_, &a, commands.data()), "allocate shared-image commands");
            f.shared_acquire = commands[0];
            f.shared_release = commands[1];
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkResetCommandBuffer(f.shared_acquire, 0), "reset shared-image acquire commands");
        vk_check(vkBeginCommandBuffer(f.shared_acquire, &begin), "begin shared-image acquire commands");
        gpu_shared_cache_->record_acquire(f.shared_acquire, shared);
        vk_check(vkEndCommandBuffer(f.shared_acquire), "end shared-image acquire commands");
        vk_check(vkResetCommandBuffer(f.shared_release, 0), "reset shared-image release commands");
        vk_check(vkBeginCommandBuffer(f.shared_release, &begin), "begin shared-image release commands");
        gpu_shared_cache_->record_release(f.shared_release, shared);
        vk_check(vkEndCommandBuffer(f.shared_release), "end shared-image release commands");
    }
    vk_check(vkEndCommandBuffer(frame.cmd), "vkEndCommandBuffer");

    // The frame's image, and under frame generation the in-between frame's too.
    std::array<VkSemaphoreSubmitInfo, 2> waits{};
    std::array<VkSemaphoreSubmitInfo, 2> signals{};
    for (auto& w : waits) {
        w.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        w.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    for (auto& s : signals) {
        s.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        s.stageMask = kPresentSignalStage;
    }
    waits[0].semaphore = f.image_available;
    signals[0].semaphore = render_finished_[frame.image_index];
    if (generated) {
        waits[1].semaphore = f.generated_available;
        signals[1].semaphore = render_finished_[*generated];
    }
    const std::uint32_t semaphores = generated ? 2u : 1u;
    std::array<VkCommandBufferSubmitInfo, 3> commands{};
    for (auto& command : commands) command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commands[0].commandBuffer = shared_frame ? f.shared_acquire : frame.cmd;
    commands[1].commandBuffer = frame.cmd;
    commands[2].commandBuffer = f.shared_release;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = semaphores;
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = shared_frame ? 3u : 1u;
    submit.pCommandBufferInfos = commands.data();
    submit.signalSemaphoreInfoCount = semaphores;
    submit.pSignalSemaphoreInfos = signals.data();
    if (shared_frame) {
        // Only the GPU's use of the imports is serialized across clients: the frame takes the lease, submits, waits
        // for its own fence (bounded) and gives the lease back. Whatever goes wrong detaches sharing for this client
        // alone (a lease not granted in time, the broker gone, a fence that does not pass in time: the dropped lease
        // closes the connection and the broker releases it), and the frame is still submitted: its fence was reset
        // and its image acquired. Before the next frame records, the imports are copied into private images
        // (begin_frame_commands) and the game goes on without sharing.
        const auto lease_started = gpu_benchmark_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto lease = gpu_shared_cache_->acquire_frame();
        if (gpu_benchmark_)
            benchmark_lease_wait_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lease_started).count();
        vk_check(vkQueueSubmit2(queue_, 1, &submit, f.in_flight), "submit frame with external images");
        recording_ = false;
        if (lease) {
            constexpr std::uint64_t kSharedFrameTimeout = 2'000'000'000; // ns
            const auto fence_started = gpu_benchmark_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const VkResult done = vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, kSharedFrameTimeout);
            if (gpu_benchmark_)
                benchmark_shared_fence_wait_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fence_started).count();
            if (done == VK_SUCCESS) {
                if (lease.finish()) ++gpu_shared_frames_;
            } else if (done != VK_TIMEOUT) {
                vk_check(done, "complete external-image frame");
            } // VK_TIMEOUT: the lease is dropped below, which detaches
        }
        if (gpu_shared_frames_ == 1 && !shared_first_logged_) {
            shared_first_logged_ = true;
            log::info("GPU asset sharing: first frame completed with {} imported images", shared.size());
        }
        if (!gpu_shared_cache_->active() && !shared_detach_logged_) {
            shared_detach_logged_ = true;
            log::warn("GPU asset sharing stopped ({}); this client goes on with its own textures",
                      gpu_shared_cache_->detach_reason());
        }
    } else {
        vk_check(vkQueueSubmit2(queue_, 1, &submit, f.in_flight), "vkQueueSubmit2");
        recording_ = false;
    }

    // The in-between frame first: FIFO shows it a refresh before the real one.
    if (generated) {
        VkPresentInfoKHR first{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        first.waitSemaphoreCount = 1;
        first.pWaitSemaphores = &render_finished_[*generated];
        first.swapchainCount = 1;
        first.pSwapchains = &swapchain_;
        first.pImageIndices = &*generated;
        const VkResult gr = vkQueuePresentKHR(queue_, &first);
        if (gr == VK_ERROR_OUT_OF_DATE_KHR || gr == VK_SUBOPTIMAL_KHR)
            resize_pending_ = true;
        else
            vk_check(gr, "vkQueuePresentKHR (generated frame)");
    }
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_[frame.image_index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &frame.image_index;
    VkResult r = vkQueuePresentKHR(queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        resize_pending_ = true;
    else
        vk_check(r, "vkQueuePresentKHR");

    if (gpu_benchmark_ && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
        const auto at = std::chrono::steady_clock::now();
        if (++benchmark_present_submissions_ == 1) gpu_benchmark_start_ = at;
        if (benchmark_present_submissions_ == 1 || benchmark_present_submissions_ % 120 == 0)
            log::info("GPU benchmark progress: gpu=\"{}\" presents={} shared_frames={} elapsed_seconds={:.3f} "
                      "shared_images_this_frame={} "
                      "lease_wait_ms={:.3f} shared_fence_wait_ms={:.3f}", gpu_name_, benchmark_present_submissions_,
                      gpu_shared_frames_, std::chrono::duration<double>(at - gpu_benchmark_start_).count(),
                      shared.size(),
                      benchmark_lease_wait_ms_, benchmark_shared_fence_wait_ms_);
    }

    if (readback) {
        vk_check(vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        vk_check(vmaInvalidateAllocation(allocator_, readback_alloc, 0, VK_WHOLE_SIZE), "invalidate readback");
        for (std::size_t shot = 0; shot < (shot_generated ? 2u : 1u); ++shot) {
            std::filesystem::path path = screenshot_path_;
            if (shot == 1) path.replace_filename(path.stem().string() + "_real" + path.extension().string());
            const auto* bytes = static_cast<const std::byte*>(readback_data) + shot_bytes * shot;
            std::vector<std::byte> rgba(bytes, bytes + shot_bytes);
            if (swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM || swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB)
                for (std::size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
            for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = std::byte{0xff};
            std::uint32_t width = extent_.width, height = extent_.height;
            if (screenshot_max_width_ && width > screenshot_max_width_) {
                // Shrunk by a whole factor, each pixel the average of its block.
                const std::uint32_t factor = (width + screenshot_max_width_ - 1) / screenshot_max_width_;
                const std::uint32_t w = width / factor, h = height / factor;
                std::vector<std::byte> small(std::size_t{w} * h * 4);
                for (std::uint32_t y = 0; y < h; ++y)
                    for (std::uint32_t x = 0; x < w; ++x)
                        for (std::uint32_t c = 0; c < 4; ++c) {
                            std::uint32_t sum = 0;
                            for (std::uint32_t dy = 0; dy < factor; ++dy)
                                for (std::uint32_t dx = 0; dx < factor; ++dx)
                                    sum += std::to_integer<std::uint32_t>(
                                        rgba[((std::size_t{y} * factor + dy) * width + std::size_t{x} * factor + dx) * 4 + c]);
                            small[(std::size_t{y} * w + x) * 4 + c] = static_cast<std::byte>(sum / (factor * factor));
                        }
                rgba = std::move(small);
                width = w;
                height = h;
            }
            if (write_png_rgba(path, rgba, width, height))
                log::info("screenshot saved to {}", path.string());
            else
                log::error("could not write screenshot {}", path.string());
        }
        vmaDestroyBuffer(allocator_, readback, readback_alloc);
        screenshot_path_.clear();
    }

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    return {true, r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR};
}

namespace {

// One cascade (array layer) of the shadow map between depth writes and sampling.
void shadow_barrier(VkCommandBuffer cmd, VkImage image, std::uint32_t first_layer, std::uint32_t layers,
                    VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, first_layer, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

} // namespace

void VkContext::create_shadow_resources() {
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        if (f == VK_FORMAT_D32_SFLOAT && fallbacks_.shadow_d16) continue; // render/gpu_fallbacks.h
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(physical_, f, &fp);
        constexpr VkFormatFeatureFlags kNeed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((fp.optimalTilingFeatures & kNeed) == kNeed) { shadow_format_ = f; break; }
    }
    if (shadow_format_ == VK_FORMAT_UNDEFINED) shadow_format_ = VK_FORMAT_D16_UNORM;

    // Hardware 2x2 comparison filtering; outside the map is lit (depth 1).
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vk_check(vkCreateSampler(device_, &si, nullptr, &shadow_sampler_), "vkCreateSampler (shadow)");

    VkSamplerCreateInfo wi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    wi.magFilter = wi.minFilter = VK_FILTER_LINEAR;
    wi.addressModeU = wi.addressModeV = wi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device_, &wi, nullptr, &water_sampler_), "vkCreateSampler (water)");
    wi.magFilter = wi.minFilter = VK_FILTER_NEAREST;
    vk_check(vkCreateSampler(device_, &wi, nullptr, &water_depth_sampler_), "vkCreateSampler (water depth)");

    // 0 the shadow map, 1 and 2 begin_translucent's colour and depth, 3 the shadow map's depths as they are (the
    // water map's), 4 the point shadow map, 5 the temporal block (TemporalBlock, shaders/motion.glsl).
    VkDescriptorSetLayoutBinding b[6];
    for (std::uint32_t i = 0; i < 5; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[5] = {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = b;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &shadow_set_layout_), "vkCreateDescriptorSetLayout (shadow)");
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    vk_check(vkCreateDescriptorPool(device_, &pi, nullptr, &shadow_pool_), "vkCreateDescriptorPool (shadow)");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = shadow_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &shadow_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &ai, &shadow_set_), "vkAllocateDescriptorSets (shadow)");
    // The temporal block: one buffer, written by each frame's commands (begin_scene) after the last frame's reads.
    VkBufferCreateInfo tbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    tbi.size = sizeof(TemporalBlock);
    tbi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo tai{};
    tai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(vmaCreateBuffer(allocator_, &tbi, &tai, &temporal_buffer_, &temporal_alloc_, nullptr),
             "vmaCreateBuffer (temporal block)");
    VkDescriptorBufferInfo tinfo{temporal_buffer_, 0, sizeof(TemporalBlock)};
    VkWriteDescriptorSet tw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    tw.dstSet = shadow_set_;
    tw.dstBinding = 5;
    tw.descriptorCount = 1;
    tw.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    tw.pBufferInfo = &tinfo;
    vkUpdateDescriptorSets(device_, 1, &tw, 0, nullptr);
    immediate_submit([&](VkCommandBuffer cmd) {
        const TemporalBlock block;
        vkCmdUpdateBuffer(cmd, temporal_buffer_, 0, sizeof(block), &block);
    });
    create_shadow_map(1); // placeholders until set_scene_options gives the sizes
    create_point_shadow_map(1, 0);
    write_water_descriptors();
}

void VkContext::write_water_descriptors() {
    if (!shadow_set_ || !water_color_.view) return;
    VkDescriptorImageInfo ii[2] = {
        {water_sampler_, water_color_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {water_depth_sampler_, water_depth_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = shadow_set_;
        w[i].dstBinding = i + 1;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void VkContext::create_shadow_map(std::uint32_t size) {
    destroy_shadow_map();
    shadow_size_ = size;
    ++shadow_generation_;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = shadow_format_;
    ii.extent = {size, size, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kShadowLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    vk_check(vmaCreateImage(allocator_, &ii, &ai, &shadow_image_, &shadow_alloc_, nullptr), "create shadow map");
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = shadow_image_;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    v.format = shadow_format_;
    v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowLayers};
    vk_check(vkCreateImageView(device_, &v, nullptr, &shadow_array_view_), "vkCreateImageView (shadow)");
    for (std::uint32_t i = 0; i < kShadowLayers; ++i) {
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        vk_check(vkCreateImageView(device_, &v, nullptr, &shadow_layer_views_[i]), "vkCreateImageView (shadow layer)");
    }
    // Cleared to 1 (lit) and left ready for sampling, the state between frames.
    immediate_submit([&](VkCommandBuffer cmd) {
        shadow_barrier(cmd, shadow_image_, 0, kShadowLayers, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearDepthStencilValue one{1.0f, 0};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowLayers};
        vkCmdClearDepthStencilImage(cmd, shadow_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
        shadow_barrier(cmd, shadow_image_, 0, kShadowLayers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    VkDescriptorImageInfo info[2] = {{shadow_sampler_, shadow_array_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {water_depth_sampler_, shadow_array_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = shadow_set_;
        w[i].dstBinding = i == 0 ? 0u : 3u;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void VkContext::destroy_shadow_map() {
    for (VkImageView& v : shadow_layer_views_) {
        if (v) vkDestroyImageView(device_, v, nullptr);
        v = VK_NULL_HANDLE;
    }
    if (shadow_array_view_) vkDestroyImageView(device_, shadow_array_view_, nullptr);
    if (shadow_image_) vmaDestroyImage(allocator_, shadow_image_, shadow_alloc_);
    shadow_array_view_ = VK_NULL_HANDLE;
    shadow_image_ = VK_NULL_HANDLE;
    shadow_size_ = 0;
}

void VkContext::begin_shadow_cascade(const FrameInfo& frame, std::uint32_t cascade, bool secondary) {
    VkCommandBuffer cmd = frame.cmd;
    shadow_open_ = cascade;
    // The previous frame's scene read this layer.
    shadow_barrier(cmd, shadow_image_, cascade, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, kDepthStages,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = shadow_layer_views_[cascade];
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {shadow_size_, shadow_size_}};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    if (secondary) rendering.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    vkCmdBeginRendering(cmd, &rendering);
    if (!secondary) set_shadow_state(cmd, cascade);
}

VkCommandBufferInheritanceRenderingInfo VkContext::shadow_inheritance() const {
    // As begin_shadow_cascade renders: the depth alone, one sample.
    VkCommandBufferInheritanceRenderingInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    info.depthAttachmentFormat = shadow_format_;
    info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    return info;
}

void VkContext::set_shadow_state(VkCommandBuffer cmd, std::uint32_t cascade) const {
    VkViewport vp{0.0f, 0.0f, static_cast<float>(shadow_size_), static_cast<float>(shadow_size_), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {shadow_size_, shadow_size_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // Seen from straight above, steep surfaces (tree cones, leaf cards) would be pushed under the ground by a
    // slope-scaled bias: the cover and water maps have none (their readers allow a margin instead).
    if (cascade >= kCoverLayer) vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    else vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f);
}

void VkContext::end_shadow_cascade(const FrameInfo& frame) {
    vkCmdEndRendering(frame.cmd);
    shadow_barrier(frame.cmd, shadow_image_, shadow_open_, 1, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VkContext::create_point_shadow_map(std::uint32_t size, std::uint32_t slots) {
    destroy_point_shadow_map();
    point_size_ = size;
    point_slots_ = slots;
    ++point_generation_;
    // A placeholder cube when off, so binding 4 is always valid.
    const std::uint32_t layers = std::max(slots, 1u) * 2u * kPointShadowFaces;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = shadow_format_;
    ii.extent = {size, size, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    vk_check(vmaCreateImage(allocator_, &ii, &ai, &point_image_, &point_alloc_, nullptr), "create point shadow map");
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = point_image_;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    v.format = shadow_format_;
    v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    vk_check(vkCreateImageView(device_, &v, nullptr, &point_array_view_), "vkCreateImageView (point shadow)");
    point_layer_views_.assign(slots ? layers : 0u, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < point_layer_views_.size(); ++i) {
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        vk_check(vkCreateImageView(device_, &v, nullptr, &point_layer_views_[i]), "vkCreateImageView (point face)");
    }
    immediate_submit([&](VkCommandBuffer cmd) {
        shadow_barrier(cmd, point_image_, 0, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearDepthStencilValue one{1.0f, 0};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
        vkCmdClearDepthStencilImage(cmd, point_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
        shadow_barrier(cmd, point_image_, 0, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    VkDescriptorImageInfo info{shadow_sampler_, point_array_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = shadow_set_;
    w.dstBinding = 4;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

void VkContext::destroy_point_shadow_map() {
    for (VkImageView v : point_layer_views_)
        if (v) vkDestroyImageView(device_, v, nullptr);
    point_layer_views_.clear();
    if (point_array_view_) vkDestroyImageView(device_, point_array_view_, nullptr);
    if (point_image_) vmaDestroyImage(allocator_, point_image_, point_alloc_);
    point_array_view_ = VK_NULL_HANDLE;
    point_image_ = VK_NULL_HANDLE;
    point_size_ = 0;
    point_slots_ = 0;
}

void VkContext::begin_point_shadow(const FrameInfo& frame, std::uint32_t cube, std::uint32_t face, bool clear) {
    VkCommandBuffer cmd = frame.cmd;
    point_open_ = cube * kPointShadowFaces + face;
    shadow_barrier(cmd, point_image_, point_open_, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, kDepthStages,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = point_layer_views_[point_open_];
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {point_size_, point_size_}};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(point_size_), static_cast<float>(point_size_), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {point_size_, point_size_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void VkContext::end_point_shadow(const FrameInfo& frame) {
    vkCmdEndRendering(frame.cmd);
    shadow_barrier(frame.cmd, point_image_, point_open_, 1, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VkContext::copy_point_shadow(const FrameInfo& frame, std::uint32_t from, std::uint32_t to) {
    VkCommandBuffer cmd = frame.cmd;
    const std::uint32_t src = from * kPointShadowFaces, dst = to * kPointShadowFaces;
    shadow_barrier(cmd, point_image_, src, kPointShadowFaces, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    shadow_barrier(cmd, point_image_, dst, kPointShadowFaces, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, src, kPointShadowFaces};
    region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, dst, kPointShadowFaces};
    region.extent = {point_size_, point_size_, 1};
    vkCmdCopyImage(cmd, point_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, point_image_,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    shadow_barrier(cmd, point_image_, src, kPointShadowFaces, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // begin_point_shadow then takes the live faces from READ_ONLY on to drawing over them.
    shadow_barrier(cmd, point_image_, dst, kPointShadowFaces, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kDepthStages,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

} // namespace hailstone::render
