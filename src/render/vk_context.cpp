#define VOLK_IMPLEMENTATION
#include "render/vk_context.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <format>
#include <string>
#include <utility>

#include "core/env.h"
#include "core/log.h"
#include "core/png_write.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/descriptor_limits.h"
#include "render/frame_sync.h"
#include "render/gpu_requirements.h"
#include "render/gpu_shared_cache.h"
#include "render/swapchain_size.h"
#include "render/vk_util.h"

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

// shaders/ao.frag's push constants.
struct AoPush {
    glm::vec4 projection{0.0f}; // tan_half_x, tan_half_y, depth terms x, y (as PostPush::depth)
    glm::vec4 params{0.0f};     // radius (world units), intensity, max radius in pixels, frame
};

// A full-screen triangle into one colour attachment of `format` (shaders/post.vert with `fragment`), added onto
// what is there when `additive`.
VkPipeline fullscreen_pipeline(VkDevice device, VkPipelineCache cache, VkPipelineLayout layout, VkShaderModule vertex,
                               VkShaderModule fragment, VkFormat format, bool additive = false) {
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
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format;
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
    scale = std::clamp(scale, 0.5f, 1.0f);
    auto side = [scale](std::uint32_t n) {
        return static_cast<std::uint32_t>(std::max(1L, std::lround(static_cast<double>(n) * scale)));
    };
    return {side(view.width), side(view.height)};
}

Tonemap parse_tonemap(std::string_view name) {
    if (name == "agx") return Tonemap::AgX;
    if (name == "aces") return Tonemap::Aces;
    return Tonemap::None;
}

VkContext::VkContext(platform::Window& window, bool validation) : window_(window) {
    const char* benchmark = core::env("GPU_BENCHMARK");
    gpu_benchmark_ = benchmark && std::strcmp(benchmark, "1") == 0;
    if (auto loader = window_.bundled_vulkan_loader())
        volkInitializeCustom(loader); // the MoltenVK a macOS release ships, as the window loaded it
    else
        vk_check(volkInitialize(), "volkInitialize (is a Vulkan driver installed?)");
    create_instance(validation);
    surface_ = window_.create_surface(instance_);
    pick_physical_device();
    create_device();
    create_swapchain();
    create_frames();
    create_shadow_resources();
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
    for (VkFence f : free_fences_) vkDestroyFence(device_, f, nullptr);
    free_fences_.clear();
    if (upload_pool_) vkDestroyCommandPool(device_, upload_pool_, nullptr);
    for (auto& f : frames_) {
        if (f.in_flight) vkDestroyFence(device_, f.in_flight, nullptr);
        if (f.image_available) vkDestroySemaphore(device_, f.image_available, nullptr);
        if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
    }
    destroy_swapchain();
    destroy_post_pipeline();
    destroy_shadow_map();
    destroy_point_shadow_map();
    if (shadow_pool_) vkDestroyDescriptorPool(device_, shadow_pool_, nullptr);
    if (shadow_set_layout_) vkDestroyDescriptorSetLayout(device_, shadow_set_layout_, nullptr);
    if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
    if (water_sampler_) vkDestroySampler(device_, water_sampler_, nullptr);
    if (water_depth_sampler_) vkDestroySampler(device_, water_depth_sampler_, nullptr);
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
    std::vector<const char*> extensions;
    for (const std::string& name : chosen.enable) extensions.push_back(name.c_str());
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
    caps_.bc_textures = features.textureCompressionBC == VK_TRUE;
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R8G8B8A8_SRGB, &fp);
        constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        caps_.srgb_mip_blit = (fp.optimalTilingFeatures & need) == need;
    }
    caps_.max_anisotropy = features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 0.0f;
    caps_.msaa_counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
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
    caps_.update_after_bind = f12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
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

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // required in 1.3; glslc emits it for discard
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE; // nonuniformEXT in zone.frag and particle.frag
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = caps_.update_after_bind ? VK_TRUE : VK_FALSE;
    // The depth format may fall back to one with stencil (below); every barrier on the depth images names the depth
    // aspect alone with DEPTH_ATTACHMENT_OPTIMAL, which on such a format is valid only with this feature.
    f12.separateDepthStencilLayouts = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.textureCompressionBC = caps_.bc_textures ? VK_TRUE : VK_FALSE;
    f2.features.samplerAnisotropy = caps_.max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f2;
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
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = physical_;
    ai.device = device_;
    ai.instance = instance_;
    ai.pVulkanFunctions = &fns;
    vk_check(vmaCreateAllocator(&ai, &allocator_), "vmaCreateAllocator");

    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(physical_, f, &fp);
        constexpr VkFormatFeatureFlags kNeed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & kNeed) == kNeed) { depth_format_ = f; break; }
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

void VkContext::create_swapchain(VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR caps;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
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
    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

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
        if (vsync_ == Vsync::Off) {
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
    if (upscaling()) {
        constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        upscale_source_ = create_target(kUpscaleFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        upscaled_ = create_target(kUpscaleFormat, kUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, view_extent_);
    }
    write_post_descriptors();
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
}

void VkContext::set_scene_options(const SceneOptions& options, const std::filesystem::path& shader_dir) {
    const VkExtent2D scaled = scaled_extent(view_extent_, options.render_scale);
    const bool rescaled = scaled.width != scene_extent_.width || scaled.height != scene_extent_.height;
    scene_options_ = options;
    // The largest supported count not above the request.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    for (std::uint32_t n = 2; n <= 16 && n <= options.msaa; n *= 2)
        if (caps_.msaa_counts & n) samples = static_cast<VkSampleCountFlagBits>(n);
    if (!post_pipeline_) create_post_pipeline(shader_dir);
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
    if (samples != scene_samples_ || rescaled) {
        vkDeviceWaitIdle(device_);
        destroy_scene_targets();
        scene_samples_ = samples;
        create_scene_targets();
    }
    log::info("scene: {}x MSAA, render scale {:.2f} ({}x{}), tonemap {}, exposure {:+.2f} EV, shadows {}, light "
              "shadows {} x {}, grade {:+.2f} / {:.2f} / {:.2f}",
              static_cast<int>(scene_samples_), std::clamp(options.render_scale, 0.5f, 1.0f), scene_extent_.width,
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

    // The post set: 0 the scene, 1 the AO, 2 the depth, 3 the bloom; the AO set: 0 the depth; the bloom sets (the
    // AO's layout): 0 the level read; then FSR 1's two (the AO's layout): EASU's source, RCAS's.
    VkDescriptorSetLayoutBinding b[4];
    for (std::uint32_t i = 0; i < 4; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = b;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &post_set_layout_), "vkCreateDescriptorSetLayout (post)");
    li.bindingCount = 1;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &ao_set_layout_), "vkCreateDescriptorSetLayout (AO)");
    constexpr std::uint32_t kSets = 2 + kBloomLevels + 1 + 2;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 + kBloomLevels + 1 + 2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    vk_check(vkCreateDescriptorPool(device_, &pi, nullptr, &post_pool_), "vkCreateDescriptorPool (post)");
    std::array<VkDescriptorSetLayout, kSets> layouts;
    layouts.fill(ao_set_layout_);
    layouts[0] = post_set_layout_;
    std::array<VkDescriptorSet, kSets> sets;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = post_pool_;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(device_, &ai, sets.data()), "vkAllocateDescriptorSets (post)");
    post_set_ = sets[0];
    ao_set_ = sets[1];
    std::copy(sets.begin() + 2, sets.begin() + 2 + bloom_sets_.size(), bloom_sets_.begin());
    easu_set_ = sets[kSets - 2];
    rcas_set_ = sets[kSets - 1];
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
    for (VkShaderModule m : {vs, fs, ao, down, up, easu, rcas}) vkDestroyShaderModule(device_, m, nullptr);
}

void VkContext::write_post_descriptors() {
    if (!post_set_) return;
    const Image& depth = depth_resolved_.image ? depth_resolved_ : depth_;
    constexpr VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    std::vector<VkDescriptorImageInfo> ii = {
        {post_linear_sampler_, scene_resolved_.view, kRead},
        {post_sampler_, ao_.view, kRead},
        {post_sampler_, depth.view, kRead},
        {post_linear_sampler_, bloom_[0].view, kRead},
        {post_sampler_, depth.view, kRead},
        {post_linear_sampler_, scene_resolved_.view, kRead},
    };
    for (const Image& level : bloom_) ii.push_back({post_linear_sampler_, level.view, kRead});
    // FSR 1's, while upscaling (their images exist only then).
    const std::size_t fsr = ii.size();
    if (upscale_source_.view) {
        ii.push_back({post_linear_sampler_, upscale_source_.view, kRead});
        ii.push_back({post_sampler_, upscaled_.view, kRead});
    }
    std::vector<VkWriteDescriptorSet> w(ii.size());
    for (std::uint32_t i = 0; i < w.size(); ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = i < 4 ? post_set_ : i == 4 ? ao_set_ : i < fsr ? bloom_sets_[i - 5] : i == fsr ? easu_set_ : rcas_set_;
        w[i].dstBinding = i < 4 ? i : 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(w.size()), w.data(), 0, nullptr);
}

void VkContext::destroy_post_pipeline() {
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

void VkContext::recreate_swapchain() {
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
    create_swapchain(old);
    resize_pending_ = false;
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
        log::warn("screenshot {} not taken: the swapchain's images cannot be copied from", path.string());
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
    if (resize_pending_) recreate_swapchain();
    // The 3D view's rectangle (set_scene_rect): a new size makes the scene targets again.
    if (VkRect2D area = scene_area(); area.extent.width != view_extent_.width || area.extent.height != view_extent_.height) {
        wait_idle();
        destroy_scene_targets();
        create_scene_targets();
    } else {
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
    begin_scene_rendering(cmd, false, clear_color);
    scene_open_ = true;
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

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, scene_extent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);

    // Flip Y so that +Y is up in clip space, as in GL-style projections.
    VkViewport vp{0.0f, static_cast<float>(scene_extent_.height), static_cast<float>(scene_extent_.width),
                  -static_cast<float>(scene_extent_.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, scene_extent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void VkContext::begin_translucent(const FrameInfo& frame) {
    if (!scene_open_) return;
    VkCommandBuffer cmd = frame.cmd;
    vkCmdEndRendering(cmd);
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
    // The multisampled targets were stored by the first rendering and are loaded as they are.
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
    // This frame's AO view (set_ao_view), used up here.
    const std::optional<AoView> taken = ao_view_;
    ao_view_.reset();
    const std::optional<SunShafts> shafts = shafts_;
    shafts_.reset();
    const std::optional<glm::vec4> mood = mood_;
    mood_.reset();
    VkCommandBuffer cmd = frame.cmd;
    vkCmdEndRendering(cmd);

    // The depth for the AO and the post pass (a depth resolve runs in the colour output stage).
    const Image& depth = depth_resolved_.image ? depth_resolved_ : depth_;
    const auto depth_sync = depth_frame_sync(scene_samples_).sampled;
    image_barrier(cmd, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth_sync.stages, depth_sync.access,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The ambient occlusion (shaders/ao.frag), when this frame's view was given.
    const bool ao = taken && scene_options_.ambient_occlusion && ao_pipeline_;
    float depth_x = 0.0f, depth_y = 0.0f; // view depth = y / (d + x), reverse Z (FlyCamera::projection)
    if (taken) {
        float n = taken->near_plane, f = taken->far_plane;
        depth_x = n / (f - n);
        depth_y = f * n / (f - n);
    }
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
    }
    image_barrier(cmd, ao_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    image_barrier(cmd, scene_resolved_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The bloom, with the AO's frames; otherwise its first level is only made readable (the post set holds it).
    const bool bloom = taken && scene_options_.bloom > 0.0f && bloom_down_pipeline_;
    if (bloom) {
        draw_bloom(cmd);
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
                 (scene_options_.dither ? kPostDither : 0u) | (ao ? kPostAo : 0u) |
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
    // Below a render scale of 1, FSR 1: the post pass's picture at the scene's size (encoded, as EASU wants it; the
    // dither waits for the last pass, or EASU would enlarge it), EASU's upscale of it to the view's size, then RCAS
    // sharpens that into the swapchain below.
    const bool fsr = upscaling() && post_pipeline_;
    UpscalePush up;
    if (fsr) {
        PostPush source = push;
        source.flags = (push.flags & ~kPostDither) | kPostEncodeOutput;
        source.offset = glm::vec4(0.0f);
        draw_into(cmd, upscale_source_, scene_extent_, post_upscale_pipeline_, post_layout_, post_set_, &source,
                  sizeof(source));
        up.sizes = glm::vec4(static_cast<float>(scene_extent_.width), static_cast<float>(scene_extent_.height),
                             static_cast<float>(view_extent_.width), static_cast<float>(view_extent_.height));
        draw_into(cmd, upscaled_, view_extent_, easu_pipeline_, upscale_layout_, easu_set_, &up, sizeof(up));
        up.offset = push.offset;
        up.sharpness = std::clamp(scene_options_.sharpness, 0.0f, 1.0f);
        up.flags = push.flags & (kPostEncodeOutput | kPostDither);
        up.frame = push.frame;
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
    if (fsr) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rcas_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscale_layout_, 0, 1, &rcas_set_, 0, nullptr);
        vkCmdPushConstants(cmd, upscale_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(up), &up);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    } else if (post_pipeline_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &post_set_, 0, nullptr);
        vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
}

void VkContext::draw_into(VkCommandBuffer cmd, const Image& target, VkExtent2D extent, VkPipeline pipeline,
                          VkPipelineLayout layout, VkDescriptorSet set, const void* push, std::uint32_t push_size) {
    // The previous frame read it.
    image_barrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
    r.renderArea = {{0, 0}, extent};
    r.layerCount = 1;
    r.colorAttachmentCount = 1;
    r.pColorAttachments = &att;
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
    image_barrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

void VkContext::end_frame(const FrameInfo& frame) {
    Frame& f = frames_[frame.frame_slot];
    VkImage image = images_[frame.image_index];
    begin_overlay(frame);
    vkCmdEndRendering(frame.cmd);

    // Optional screenshot: copy the finished image into a host-visible buffer.
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_alloc = VK_NULL_HANDLE;
    void* readback_data = nullptr;
    if (!screenshot_path_.empty() && !screenshots_supported_) {
        // Asked before a swapchain made without TRANSFER_SRC: the copy would be invalid.
        log::warn("screenshot {} not taken: the swapchain's images cannot be copied from", screenshot_path_.string());
        screenshot_path_.clear();
    }
    if (!screenshot_path_.empty()) {
        // The command buffer is open: a failure here is logged and the frame goes on without the copy (a throw
        // would leave it half recorded).
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize{extent_.width} * extent_.height * 4;
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
        image_barrier(frame.cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(frame.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
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
        image_barrier(frame.cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_finished_[frame.image_index];
    signal.stageMask = kPresentSignalStage;
    std::array<VkCommandBufferSubmitInfo, 3> commands{};
    for (auto& command : commands) command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commands[0].commandBuffer = shared_frame ? f.shared_acquire : frame.cmd;
    commands[1].commandBuffer = frame.cmd;
    commands[2].commandBuffer = f.shared_release;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = shared_frame ? 3u : 1u;
    submit.pCommandBufferInfos = commands.data();
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
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
        std::vector<std::byte> rgba(static_cast<const std::byte*>(readback_data),
                                    static_cast<const std::byte*>(readback_data) + std::size_t{extent_.width} * extent_.height * 4);
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
        if (write_png_rgba(screenshot_path_, rgba, width, height))
            log::info("screenshot saved to {}", screenshot_path_.string());
        else
            log::error("could not write screenshot {}", screenshot_path_.string());
        vmaDestroyBuffer(allocator_, readback, readback_alloc);
        screenshot_path_.clear();
    }

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
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
    // water map's), 4 the point shadow map.
    VkDescriptorSetLayoutBinding b[5];
    for (std::uint32_t i = 0; i < 5; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = b;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &shadow_set_layout_), "vkCreateDescriptorSetLayout (shadow)");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    vk_check(vkCreateDescriptorPool(device_, &pi, nullptr, &shadow_pool_), "vkCreateDescriptorPool (shadow)");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = shadow_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &shadow_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &ai, &shadow_set_), "vkAllocateDescriptorSets (shadow)");
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

void VkContext::begin_shadow_cascade(const FrameInfo& frame, std::uint32_t cascade) {
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
    vkCmdBeginRendering(cmd, &rendering);
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
