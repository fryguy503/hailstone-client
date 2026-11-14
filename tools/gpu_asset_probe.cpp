// Real-asset, single-device diagnostic only. No accounts, network clients, asset files, or settings are written.
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "gpu_lab/gpu_probe_scene.h"
#include "assets/texture_overrides.h"
#include "assets/zone.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "core/png_write.h"
#include "platform/process.h"
#include "platform/shared_file.h"
#include "render/gpu.h"
#include "gpu_lab/gpu_asset_view_probe.h"
#include "render/material_state.h"

#ifndef HAILSTONE_GPU_ASSET_SHADER_DIR
#define HAILSTONE_GPU_ASSET_SHADER_DIR "shaders"
#endif

using namespace hailstone;
namespace {
constexpr std::string_view camera_recipe = "zone-drawn-xy-median-z-aerial-v2";
const char* boolean(bool value) { return value ? "true" : "false"; }
std::string quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
std::uint32_t number(std::string_view value) {
    std::uint32_t out = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) throw std::invalid_argument("invalid number");
    return out;
}
void check(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("Vulkan operation failed"); }
struct Options {
    std::filesystem::path game, textures, capture;
    std::string zone = "gfaydark";
    std::uint32_t views = 2, device = 0, hold = 0, width = 512, height = 512;
    bool sharing = false, compare = false, validation = false, help = false, check_capture = false;
};
Options parse(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") { out.help = true; continue; }
        if (arg == "--compare") { out.compare = true; continue; }
        if (arg == "--check-capture") { out.check_capture = true; continue; }
        if (arg == "--validation") { out.validation = true; continue; }
        if (arg != "--game-dir" && arg != "--texture-folder" && arg != "--capture-directory" && arg != "--zone" && arg != "--views" &&
            arg != "--sharing" && arg != "--device-index" && arg != "--hold-seconds" && arg != "--width" && arg != "--height")
            throw std::invalid_argument("unknown option");
        if (++i >= argc) throw std::invalid_argument("missing option value");
        const std::string_view value(argv[i]);
        if (arg == "--game-dir") out.game = argv[i];
        else if (arg == "--texture-folder") out.textures = argv[i];
        else if (arg == "--capture-directory") out.capture = argv[i];
        else if (arg == "--zone") out.zone = value;
        else if (arg == "--sharing") {
            if (value != "on" && value != "off") throw std::invalid_argument("invalid sharing mode");
            out.sharing = value == "on";
        } else if (arg == "--views") out.views = number(value);
        else if (arg == "--device-index") out.device = number(value);
        else if (arg == "--hold-seconds") out.hold = number(value);
        else if (arg == "--width") out.width = number(value);
        else out.height = number(value);
    }
    if (out.help) return out;
    if (out.game.empty() || (out.views != 2 && out.views != 6) || out.width < 64 || out.width > 2048 ||
        out.height < 64 || out.height > 2048 || out.hold > 3600 || (out.compare && out.hold != 0) ||
        (!out.capture.empty() && (!out.compare || !out.capture.is_absolute())) ||
        (out.check_capture && out.capture.empty()) ||
        out.zone.empty() || !std::all_of(out.zone.begin(), out.zone.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        })) throw std::invalid_argument("invalid diagnostic bounds");
    return out;
}

bool within_input(const std::filesystem::path& candidate, const std::filesystem::path& input) {
    if (input.empty()) return false;
    const auto root = std::filesystem::canonical(input);
    // Existing ancestors are compared by file identity, including Windows case folding, junctions and short names.
    for (auto ancestor = candidate; !ancestor.empty();) {
        std::error_code error;
        if (std::filesystem::exists(ancestor, error) && std::filesystem::equivalent(ancestor, root, error) && !error)
            return true;
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    return false;
}

std::filesystem::path capture_destination(const Options& options, bool create) {
    if (options.capture.empty()) return {};
    std::error_code error;
    const auto status = std::filesystem::symlink_status(options.capture, error);
    if (std::filesystem::exists(status) || (error && error != std::errc::no_such_file_or_directory))
        throw std::invalid_argument("capture directory must be new");
    const auto destination = std::filesystem::weakly_canonical(options.capture);
    if (!std::filesystem::is_directory(destination.parent_path()) || within_input(destination, options.game) ||
        within_input(destination, options.textures)) throw std::invalid_argument("capture directory overlaps input assets");
    if (create) {
        // The platform helper must create the leaf with its private ACL/mode; it rejects a precreated broad leaf.
        if (!platform::prepare_private_cache_directory(destination))
            throw std::runtime_error("capture directory creation failed");
        const auto created = std::filesystem::symlink_status(destination);
        if (!std::filesystem::is_directory(created) || std::filesystem::is_symlink(created) ||
            within_input(std::filesystem::canonical(destination), options.game) ||
            within_input(std::filesystem::canonical(destination), options.textures))
            throw std::runtime_error("capture directory validation failed");
    }
    return destination;
}
struct Validation { std::atomic<std::uint32_t> errors{0}, warnings{0}, general{0}, validation{0}, performance{0}; };
VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto& state = *static_cast<Validation*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++state.errors;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++state.warnings;
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) ++state.general;
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) ++state.validation;
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) ++state.performance;
    try {
        const auto id = quote(data && data->pMessageIdName ? std::string_view(data->pMessageIdName).substr(0, 256) : "unnamed");
        std::fprintf(stderr, "{\"kind\":\"validation\",\"message_type\":%u,\"severity\":%u,\"id\":%s}\n",
            type, static_cast<unsigned>(severity), id.c_str());
    } catch (...) { std::fputs("{\"kind\":\"validation\",\"id\":\"unavailable\"}\n", stderr); }
    return VK_FALSE;
}
struct Instance {
    VkInstance handle = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    void reset() {
        if (messenger) vkDestroyDebugUtilsMessengerEXT(handle, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
        if (handle) vkDestroyInstance(handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    ~Instance() { reset(); }
};
std::vector<std::uint32_t> shader(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("shader unavailable");
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size < 20 || size > 1024 * 1024 || size % 4 != 0) throw std::runtime_error("invalid shader length");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("shader read failed");
    ByteReader input(bytes);
    std::vector<std::uint32_t> words;
    while (input.remaining()) words.push_back(input.le<std::uint32_t>());
    if (words.front() != 0x07230203) throw std::runtime_error("invalid shader magic");
    return words;
}
std::vector<glm::mat4> cameras(const assets::GpuProbeScene& scene, const Options& options) {
    std::vector<glm::mat4> out;
    auto low = glm::vec3(std::numeric_limits<float>::max()), high = -low;
    std::vector<float> heights;
    std::vector<bool> used(scene.vertices.size());
    for (const auto index : scene.indices) {
        if (used[index]) continue;
        used[index] = true;
        const auto point = scene.vertices[index].position;
        low = glm::min(low, point); high = glm::max(high, point);
        heights.push_back(point.z);
    }
    if (heights.empty()) throw std::runtime_error("camera has no drawn vertices");
    const auto median = heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2);
    std::nth_element(heights.begin(), median, heights.end());
    const glm::vec3 center{(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, *median};
    // Death floors and unused geometry remain uploaded; they must not aim the camera kilometres below the zone.
    const float span = std::max(1.0f, std::max(high.x - low.x, high.y - low.y) * 0.65f);
    const float aspect = static_cast<float>(options.width) / static_cast<float>(options.height);
    auto projection = glm::orthoRH_ZO(-span * aspect, span * aspect, -span, span, span * 0.01f, span * 8.0f);
    projection[1][1] *= -1;
    for (std::uint32_t i = 0; i < options.views; ++i) {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(options.views);
        const auto eye = center + span * glm::vec3(std::cos(angle) * 2, std::sin(angle) * 2, 1.25f);
        out.push_back(projection * glm::lookAtRH(eye, center, glm::vec3(0, 0, 1)));
    }
    return out;
}
std::string render_hash(const assets::GpuProbeScene& scene, const Options& options, const std::vector<glm::mat4>& views,
    std::span<const std::uint32_t> vertex_spirv, std::span<const std::uint32_t> fragment_spirv) {
    ByteWriter bytes;
    bytes.le(options.width); bytes.le(options.height); bytes.le(options.views);
    for (const auto& view : views) for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) bytes.f32(view[column][row]);
    Sha256 hash;
    hash.update("gpu-probe-render-v2");
    hash.update(camera_recipe);
    hash.update(std::as_bytes(std::span(scene.scene_digest)));
    hash.update(bytes.data());
    for (const auto code : {vertex_spirv, fragment_spirv}) {
        ByteWriter shader_bytes;
        shader_bytes.le(static_cast<std::uint64_t>(code.size()));
        for (const auto word : code) shader_bytes.le(word);
        hash.update(shader_bytes.data());
    }
    return to_hex(hash.finish());
}
void hashes(const std::vector<render::GpuAssetViewPixels>& pixels) {
    std::putchar('[');
    for (std::size_t i = 0; i < pixels.size(); ++i) std::printf("%s\"%s\"", i ? "," : "", to_hex(pixels[i].hash).c_str());
    std::putchar(']');
}
void counts(const std::vector<render::GpuAssetViewPixels>& pixels) {
    std::putchar('[');
    for (std::size_t i = 0; i < pixels.size(); ++i)
        std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(pixels[i].non_background_pixels));
    std::putchar(']');
}
void category(const char* name, const render::GpuAssetMemoryCategory& value) {
    std::printf("\"%s\":{\"allocations\":%llu,\"logical_bytes\":%llu,\"allocated_bytes\":%llu}", name,
        static_cast<unsigned long long>(value.allocations), static_cast<unsigned long long>(value.logical_bytes),
        static_cast<unsigned long long>(value.allocated_bytes));
}
std::vector<Sha256Digest> run(VkPhysicalDevice physical, const assets::GpuProbeScene& scene, const Options& options,
    const std::vector<glm::mat4>& views, const std::vector<std::uint32_t>& vert, const std::vector<std::uint32_t>& frag,
    Validation& validation) {
    std::string error;
    VkPhysicalDeviceProperties device_properties{};
    vkGetPhysicalDeviceProperties(physical, &device_properties);
    const auto device_api = std::to_string(VK_API_VERSION_MAJOR(device_properties.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_MINOR(device_properties.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_PATCH(device_properties.apiVersion));
    auto renderer = render::GpuAssetViewProbe::create(physical, scene, views, vert, frag,
        {.share_immutable = options.sharing, .width = options.width, .height = options.height}, &error);
    if (!renderer || !renderer->render(&error)) throw std::runtime_error(error.empty() ? "GPU asset rendering failed" : error);
    const auto& pixels = renderer->pixels();
    if (pixels.size() != options.views) throw std::runtime_error("view count guard failed");
    if (validation.errors.load()) throw std::runtime_error("GPU validation guard failed");
    if (!options.capture.empty()) {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const auto png = encode_png_rgba(pixels[i].rgba, pixels[i].width, pixels[i].height);
            const auto destination = options.capture / ((options.sharing ? std::string("on-view-") : std::string("off-view-")) + std::to_string(i + 1) + ".png");
            auto temporary = platform::CacheTempFile::create(options.capture);
            if (png.empty() || !temporary || !temporary->write(std::as_bytes(std::span(png))) ||
                temporary->publish(destination) != platform::PublishResult::Published)
                throw std::runtime_error("capture publication failed");
        }
    }
    // Optional local captures also help diagnose a failed coverage guard; failure never emits a ready record.
    if (std::any_of(pixels.begin(), pixels.end(), [](const auto& p) {
            return p.non_background_pixels <= static_cast<std::uint64_t>(p.width) * p.height / 20;
        })) throw std::runtime_error("five-percent pixel coverage guard failed");
    if (pixels[0].hash == pixels[1].hash) throw std::runtime_error("different-camera pixel guard failed");
    std::vector<Sha256Digest> result;
    for (const auto& pixel : pixels) result.push_back(pixel.hash);
    const auto recipe = render_hash(scene, options, views, vert, frag);
    std::printf("{\"kind\":\"ready\",\"schema_version\":1,\"sharing\":\"%s\",\"views\":%u,\"width\":%u,\"height\":%u,"
        "\"device_index\":%u,\"device_model\":%s,\"device_api\":%s,\"target_fps\":30,\"camera_recipe\":%s,\"inventory_hash\":\"%s\",\"render_recipe_hash\":\"%s\","
        "\"gpu_complete\":true,\"validation_enabled\":%s,\"validation_errors\":%u,\"validation_warnings\":%u,\"pid\":%lu,\"pixel_hashes\":",
        options.sharing ? "on" : "off", options.views, options.width, options.height, options.device,
        quote(device_properties.deviceName).c_str(), quote(device_api).c_str(),
        quote(camera_recipe).c_str(), to_hex(scene.scene_digest).c_str(), recipe.c_str(), boolean(options.validation),
        validation.errors.load(), validation.warnings.load(), platform::process_id());
    hashes(pixels); std::printf(",\"non_background_pixels\":"); counts(pixels);
    const auto& stats = renderer->stats();
    std::printf(",\"allocation_inventory_is_physical_vram\":false,");
    category("immutable_geometry", stats.immutable_geometry); std::putchar(',');
    category("immutable_textures", stats.immutable_textures); std::putchar(',');
    category("private_targets", stats.private_targets); std::putchar(','); category("readback", stats.readback);
    std::putchar(','); category("staging_peak", stats.staging_peak);
    std::printf(",\"staging_live_bytes\":%llu,\"simplified_static_shader\":true,\"cross_process\":false,\"bc_fallback_textures\":%llu,\"upload_format_copies\":[",
        static_cast<unsigned long long>(stats.staging_live_bytes), static_cast<unsigned long long>(stats.bc_fallback_textures));
    for (std::size_t i = 0; i < stats.upload_format_copies.size(); ++i)
        std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(stats.upload_format_copies[i]));
    std::printf("]}\n"); std::fflush(stdout);
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    if (options.hold) {
        // Process-scoped input reader owns no renderer state. EOF can stop the finite render hold early.
        std::thread([stopped] { while (std::getchar() != EOF) {} stopped->store(true); }).detach();
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now(), deadline = start + std::chrono::seconds(options.hold);
    auto next = start, progress = start + std::chrono::seconds(1);
    std::vector<double> times;
    while (Clock::now() < deadline && !stopped->load()) {
        const auto before = Clock::now();
        if (!renderer->render(&error, false) || validation.errors.load()) throw std::runtime_error("held GPU render failed");
        const auto after = Clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(after - before).count());
        if (after >= progress) {
            std::printf("{\"kind\":\"progress\",\"sharing\":\"%s\",\"frames_rendered\":%llu,\"elapsed_seconds\":%.6f,\"validation_errors\":%u}\n",
                options.sharing ? "on" : "off", static_cast<unsigned long long>(renderer->stats().frames_rendered),
                std::chrono::duration<double>(after - start).count(), validation.errors.load());
            std::fflush(stdout); progress = after + std::chrono::seconds(1);
        }
        next += std::chrono::microseconds(33333);
        if (next < after) next = after;
        std::this_thread::sleep_until(next);
    }
    if (!renderer->render(&error)) throw std::runtime_error("final readback failed");
    for (std::size_t i = 0; i < result.size(); ++i)
        if (renderer->pixels()[i].hash != result[i]) throw std::runtime_error("held pixels changed");
    const auto frames = renderer->stats().frames_rendered;
    renderer.reset(); // Include device teardown validation in the final result.
    if (validation.errors.load()) throw std::runtime_error("GPU validation failed");
    std::sort(times.begin(), times.end());
    auto percentile = [&](double p) { return times.empty() ? 0.0 : times[static_cast<std::size_t>(p * static_cast<double>(times.size() - 1))]; };
    std::printf("{\"kind\":\"summary\",\"sharing\":\"%s\",\"frames_rendered\":%llu,\"frame_ms_min\":%.6f,\"frame_ms_median\":%.6f,"
        "\"frame_ms_p95\":%.6f,\"frame_ms_max\":%.6f,\"validation_errors\":%u,\"validation_warnings\":%u,\"gpu_complete\":true,"
        "\"timed_hold_frames\":%llu,\"frame_timing_scope\":\"GPU-fenced render wall time; excludes pacing and ready/final CPU pixel hashing\"}\n",
        options.sharing ? "on" : "off", static_cast<unsigned long long>(frames), percentile(0), percentile(0.5), percentile(0.95), percentile(1),
        validation.errors.load(), validation.warnings.load(), static_cast<unsigned long long>(times.size()));
    std::fflush(stdout);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    const char* phase = "arguments";
    try {
        auto options = parse(argc, argv);
        if (options.help) {
            std::puts("hailstone_gpu_asset_probe --game-dir PATH [--zone gfaydark] [--views 2|6] [--sharing off|on]\n"
                "  [--device-index N] [--hold-seconds 0..3600] [--width 64..2048] [--height 64..2048]\n"
                "  [--texture-folder PATH] [--validation] [--compare] [--capture-directory NEW_ABSOLUTE_DIR]\n"
                "  --check-capture checks/creates the guarded private capture leaf, then exits without Vulkan or assets.\n"
                "One device, offscreen static-asset views; no game clients or external handles. Hold renders at 30 FPS; stdin EOF stops it.");
            return 0;
        }
        if (!options.capture.empty()) (void)capture_destination(options, false);
        if (options.check_capture) {
            (void)capture_destination(options, true);
            std::puts("{\"kind\":\"capture_check\",\"created_private_directory\":true,\"vulkan_initialized\":false,\"asset_payloads_read\":false}");
            return 0;
        }
        log::set_level(log::Level::Error);
        phase = "Vulkan initialization";
        check(volkInitialize());
        std::uint32_t version = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion) check(vkEnumerateInstanceVersion(&version));
        if (version < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan1.3 required");
        std::uint32_t count = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> extensions(count);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()));
        auto has = [&](const char* name) { return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) { return std::string_view(e.extensionName) == name; }); };
        const char* layer = "VK_LAYER_KHRONOS_validation";
        if (options.validation) {
            check(vkEnumerateInstanceLayerProperties(&count, nullptr));
            std::vector<VkLayerProperties> layers(count);
            check(vkEnumerateInstanceLayerProperties(&count, layers.data()));
            if (!has("VK_EXT_debug_utils") || !std::any_of(layers.begin(), layers.end(), [layer](const auto& p) { return std::string_view(p.layerName) == layer; }))
                throw std::runtime_error("validation unavailable");
        }
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Hailstone static GPU asset diagnostic";
        application.apiVersion = VK_API_VERSION_1_3;
        std::vector<const char*> enabled;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &application;
        if (has("VK_KHR_portability_enumeration")) { enabled.push_back("VK_KHR_portability_enumeration"); create.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; }
        if (options.validation) enabled.push_back("VK_EXT_debug_utils");
        create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()); create.ppEnabledExtensionNames = enabled.data();
        Validation validation;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        if (options.validation) {
            create.enabledLayerCount = 1; create.ppEnabledLayerNames = &layer;
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = message; debug.pUserData = &validation; create.pNext = &debug;
        }
        Instance instance;
        check(vkCreateInstance(&create, nullptr, &instance.handle)); volkLoadInstance(instance.handle);
        if (options.validation) {
            if (!vkCreateDebugUtilsMessengerEXT || !vkDestroyDebugUtilsMessengerEXT) throw std::runtime_error("debug utils unavailable");
            check(vkCreateDebugUtilsMessengerEXT(instance.handle, &debug, nullptr, &instance.messenger));
        }
        check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()));
        if (options.device >= count) throw std::runtime_error("device unavailable");
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(devices[options.device], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) throw std::runtime_error("selected device belowVulkan1.3");
        phase = "real static assets";
        assets::texture_overrides().set_folder(options.textures);
        GameDir game(options.game);
        auto zone = assets::load_zone(game, options.zone);
        const assets::GpuProbeScenePreparation preparation{
            [](const assets::Material& material) {
                const auto state = render::material_state(material);
                return assets::GpuProbeMaterialPolicy{state.draw, state.masked_texture, !state.alpha_unused(), state.opacity, state.alpha_ref};
            }, [](assets::Image image, const assets::GpuProbeMaterialPolicy& policy) {
                (void)policy;
                // ZoneRenderer::prepare uses Color for base frames; reads_alpha selects the decode variant separately.
                if (auto prepared = render::prepare_texture_image(image, render::TextureUse::Color, false)) return std::move(*prepared);
                return image;
            }};
        auto scene = assets::build_gpu_probe_scene(zone, preparation);
        zone = {}; // Release archive/parser/decoded source storage before native GPU measurements.
        std::array<std::uint64_t, 7> formats{};
        for (const auto& texture : scene.textures) formats[static_cast<std::size_t>(texture.image.format)] += texture.upload_bytes;
        const auto& inventory = scene.inventory;
        std::printf("{\"kind\":\"inventory\",\"schema_version\":1,\"inventory_hash\":\"%s\",\"geometry_hash\":\"%s\",\"vertices\":%llu,\"indices\":%llu,"
            "\"draws\":%llu,\"textures\":%llu,\"vertex_bytes\":%llu,\"index_bytes\":%llu,\"texture_bytes\":%llu,\"unique_texture_bytes\":%llu,"
            "\"unique_texture_contents\":%llu,\"missing_textures\":%llu,\"missing_objects\":%llu,\"material_effect_draws\":%llu,\"masked_draws\":%llu,"
            "\"translucent_draws\":%llu,\"animated_materials\":%llu,\"static_auxiliary_textures_included\":false,\"shader_scope\":\"static positions UV first-frame diffuse fixed alpha test; no vertex colors material blend fog lighting effects actors sky or gameplay\",\"source_texture_bytes_by_format\":[",
            to_hex(scene.scene_digest).c_str(), to_hex(scene.geometry_digest).c_str(), static_cast<unsigned long long>(scene.vertices.size()),
            static_cast<unsigned long long>(scene.indices.size()), static_cast<unsigned long long>(scene.draws.size()), static_cast<unsigned long long>(scene.textures.size()),
            static_cast<unsigned long long>(inventory.vertex_bytes), static_cast<unsigned long long>(inventory.index_bytes), static_cast<unsigned long long>(inventory.texture_bytes),
            static_cast<unsigned long long>(inventory.unique_texture_bytes), static_cast<unsigned long long>(inventory.unique_texture_contents),
            static_cast<unsigned long long>(inventory.missing_textures), static_cast<unsigned long long>(inventory.missing_objects),
            static_cast<unsigned long long>(inventory.material_effect_draws), static_cast<unsigned long long>(inventory.masked_draws),
            static_cast<unsigned long long>(inventory.translucent_draws), static_cast<unsigned long long>(inventory.animated_materials));
        for (std::size_t i = 0; i < formats.size(); ++i) std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(formats[i]));
        std::printf("]}\n"); std::fflush(stdout);
        const auto views = cameras(scene, options);
        const auto directory = std::filesystem::path(HAILSTONE_GPU_ASSET_SHADER_DIR);
        const auto vert = shader(directory / "gpu_asset_probe.vert.spv"), frag = shader(directory / "gpu_asset_probe.frag.spv");
        if (!options.capture.empty()) options.capture = capture_destination(options, true);
        phase = "GPU static-asset rendering";
        if (options.compare) {
            auto first = options; first.sharing = false;
            const auto off = run(devices[options.device], scene, first, views, vert, frag, validation);
            first.sharing = true;
            const auto on = run(devices[options.device], scene, first, views, vert, frag, validation);
            if (off != on) throw std::runtime_error("OFF/ON pixel mismatch");
            std::printf("{\"kind\":\"compare\",\"matched\":true,\"views\":%u,\"validation_errors\":%u,\"cross_process\":false}\n", options.views, validation.errors.load());
        } else (void)run(devices[options.device], scene, options, views, vert, frag, validation);
        instance.reset();
        if (validation.errors.load()) throw std::runtime_error("instance teardown validation failed");
        std::printf("{\"kind\":\"complete\",\"validation_errors\":%u,\"validation_warnings\":%u,\"validation_general_messages\":%u,"
            "\"validation_rule_messages\":%u,\"validation_performance_messages\":%u,\"cross_process_sharing_proven\":false}\n",
            validation.errors.load(), validation.warnings.load(), validation.general.load(), validation.validation.load(), validation.performance.load());
        return 0;
    } catch (const std::invalid_argument&) {
        std::fputs("gpu_asset_probe: invalid arguments; use --help\n", stderr); return 2;
    } catch (const std::exception& error) {
        // Only the GPU-only phase receives in-memory scenes with no source path fields.
        const auto reason = std::string_view(phase) == "GPU static-asset rendering" ? error.what() : "asset or initialization failure";
        std::fprintf(stderr, "{\"kind\":\"error\",\"phase\":%s,\"reason\":%s,\"asset_paths_emitted\":false}\n",
            quote(phase).c_str(), quote(reason).c_str()); return 1;
    }
}
