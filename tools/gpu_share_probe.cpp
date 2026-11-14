// Headless GPU sharing diagnostics. No assets, identifiers, external handles, or client processes are accessed.
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "gpu_lab/gpu_share_capabilities.h"

namespace {
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
void check(VkResult status, const char* operation) {
    if (status != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}
std::uint64_t number(std::string_view value) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("expected an unsigned decimal number");
    return result;
}
void memory_fields(const hailstone::render::ExternalMemoryCapability& capability) {
    std::printf("\"queried\":%s,\"vk_result\":%d,\"importable\":%s,\"exportable\":%s,\"dedicated_only\":%s,\"compatible_handle\":%s",
        boolean(capability.queried), static_cast<int>(capability.result), boolean(capability.importable),
        boolean(capability.exportable), boolean(capability.dedicated_only), boolean(capability.compatible_handle));
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

struct Validation {
    std::atomic<std::uint32_t> errors{0}, warnings{0};
};
VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto& state = *static_cast<Validation*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) state.errors.fetch_add(1);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) state.warnings.fetch_add(1);
    // Only static validation IDs are printed. Driver messages can include object details unrelated to the proof.
    const std::string_view id = data->pMessageIdName ? data->pMessageIdName : "validation-message";
    const bool safe = std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    std::fprintf(stderr, "gpu_share_probe validation: %s\n", safe ? id.data() : "validation-message");
    return VK_FALSE;
}
} // namespace

int main(int argc, char** argv) {
    using namespace hailstone::render;
    try {
        bool smoke = false;
        bool validation_requested = false;
        bool help = false;
        bool changed_bounds = false;
        std::uint32_t selected = 0;
        GpuAliasSmokeOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--help") { help = true; continue; }
            if (argument == "--smoke") { smoke = true; continue; }
            if (argument == "--validation") { validation_requested = true; continue; }
            if (argument != "--device-index" && argument != "--views" && argument != "--bytes")
                throw std::invalid_argument("unknown argument; use --help");
            if (++i >= argc) throw std::invalid_argument("missing option value");
            const auto value = number(argv[i]);
            if (argument == "--bytes") { options.bytes = value; changed_bounds = true; }
            else {
                if (value > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("option out of range");
                if (argument == "--views") { options.views = static_cast<std::uint32_t>(value); changed_bounds = true; }
                else selected = static_cast<std::uint32_t>(value);
            }
        }
        if (help) {
            std::puts("hailstone_gpu_share_probe [--smoke] [--validation] [--device-index N] [--views N] [--bytes N]\n"
                      "Queries every adapter without ID/UUID properties. --smoke tests one logical device,\n"
                      "one device-local allocation and up to 64 aliases; it is not a multi-process test.\n"
                      "Smoke bounds: views 1..64, bytes 4..1048576 in multiples of 4; defaults 24 and 65536.\n"
                      "--validation requires VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils.");
            return 0;
        }
        if ((changed_bounds && !smoke) || !valid_gpu_alias_smoke_options(options))
            throw std::invalid_argument("invalid smoke options; use --help");
        check(volkInitialize(), "initialize Vulkan loader");
        std::uint32_t version = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion) check(vkEnumerateInstanceVersion(&version), "query loader version");
        if (version < VK_API_VERSION_1_1) throw std::runtime_error("Vulkan 1.1 loader required");
        std::uint32_t layer_count = 0;
        check(vkEnumerateInstanceLayerProperties(&layer_count, nullptr), "instance layer count");
        std::vector<VkLayerProperties> layers(layer_count);
        check(vkEnumerateInstanceLayerProperties(&layer_count, layers.data()), "instance layers");
        const char* validation_layer = "VK_LAYER_KHRONOS_validation";
        const bool validation_available = std::any_of(layers.begin(), layers.end(), [validation_layer](const auto& layer) {
            return std::strcmp(layer.layerName, validation_layer) == 0;
        });
        if (validation_requested && !validation_available) throw std::runtime_error("Khronos validation layer unavailable");
        std::uint32_t extension_count = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr), "instance extension count");
        std::vector<VkExtensionProperties> extensions(extension_count);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data()), "instance extensions");
        const char* portability = "VK_KHR_portability_enumeration";
        const bool has_portability = std::any_of(extensions.begin(), extensions.end(), [portability](const auto& extension) {
            return std::strcmp(extension.extensionName, portability) == 0;
        });
        const char* debug_utils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const bool has_debug = std::any_of(extensions.begin(), extensions.end(), [debug_utils](const auto& extension) {
            return std::strcmp(extension.extensionName, debug_utils) == 0;
        });
        if (validation_requested && !has_debug) throw std::runtime_error("debug utils extension unavailable");
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Hailstone GPU sharing diagnostics";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &application;
        std::vector<const char*> enable;
        if (has_portability) { create.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; enable.push_back(portability); }
        if (validation_requested) enable.push_back(debug_utils);
        create.enabledExtensionCount = static_cast<std::uint32_t>(enable.size());
        create.ppEnabledExtensionNames = enable.data();
        Validation validation;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        if (validation_requested) {
            create.enabledLayerCount = 1;
            create.ppEnabledLayerNames = &validation_layer;
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = validation_message;
            debug.pUserData = &validation;
            create.pNext = &debug;
        }
        Instance instance;
        check(vkCreateInstance(&create, nullptr, &instance.handle), "create Vulkan instance");
        volkLoadInstance(instance.handle);
        if (validation_requested) {
            if (!vkCreateDebugUtilsMessengerEXT || !vkDestroyDebugUtilsMessengerEXT)
                throw std::runtime_error("debug utils entry points unavailable");
            check(vkCreateDebugUtilsMessengerEXT(instance.handle, &debug, nullptr, &instance.messenger), "create validation messenger");
        }
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "physical device count");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "physical devices");
        if (!count || selected >= count) throw std::invalid_argument("device index unavailable");
        std::printf("{\"kind\":\"probe\",\"device_count\":%u,\"id_properties_queried\":false,"
                    "\"cross_process_sharing_proven\":false,\"parallel_client_read_sharing_proven\":false,"
                    "\"validation_layer_available\":%s,\"validation_enabled\":%s}\n", count,
                    boolean(validation_available), boolean(validation_requested));
        for (std::uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            const auto capabilities = query_gpu_share_capabilities(devices[i]);
            std::printf("{\"kind\":\"device\",\"index\":%u,\"model\":%s,\"api\":\"%u.%u.%u\","
                        "\"transport\":%s,\"memory_extension\":%s,\"semaphore_extension\":%s,\"buffer_handoff_capable\":%s,\"vk_result\":%d}\n",
                i, quote(properties.deviceName).c_str(), VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
                quote(gpu_share_transport_name(capabilities.transport)).c_str(), boolean(capabilities.memory_extension),
                boolean(capabilities.semaphore_extension), boolean(external_transfer_supported(capabilities)), static_cast<int>(capabilities.result));
            std::printf("{\"kind\":\"buffer\",\"device\":%u,\"usage\":\"transfer\",", i);
            memory_fields(capabilities.transfer_buffer);
            std::printf("}\n{\"kind\":\"buffer\",\"device\":%u,\"usage\":\"vertex-index-transfer\",", i);
            memory_fields(capabilities.geometry_buffer);
            std::printf("}\n{\"kind\":\"binary_semaphore\",\"device\":%u,\"queried\":%s,\"importable\":%s,\"exportable\":%s}\n", i,
                boolean(capabilities.binary_semaphore_queried), boolean(capabilities.binary_semaphore_importable),
                boolean(capabilities.binary_semaphore_exportable));
            for (const auto& image : capabilities.images) {
                std::printf("{\"kind\":\"image\",\"device\":%u,\"format\":%d,\"usage\":\"optimal-sampled-transfer\",", i, static_cast<int>(image.format));
                memory_fields(image.memory);
                std::printf("}\n");
            }
        }
        int exit_code = 0;
        if (smoke) {
            const auto result = run_gpu_alias_smoke(devices[selected], options);
            std::printf("{\"kind\":\"same_device_alias_smoke\",\"device\":%u,\"vk_result\":%d,\"matched\":%s,"
                        "\"views\":%u,\"bytes_per_view\":%llu,\"device_allocations\":%u,\"device_allocation_bytes\":%llu,"
                        "\"readback_allocation_bytes\":%llu,\"verified_words\":%llu,\"cross_process\":false,\"error\":%s}\n",
                selected, static_cast<int>(result.result), boolean(result.matched), result.views,
                static_cast<unsigned long long>(result.bytes_per_view), result.device_allocation_bytes ? 1u : 0u,
                static_cast<unsigned long long>(result.device_allocation_bytes),
                static_cast<unsigned long long>(result.readback_allocation_bytes), static_cast<unsigned long long>(result.verified_words),
                quote(result.error).c_str());
            exit_code = result.matched ? 0 : 1;
        }
        instance.reset();
        std::printf("{\"kind\":\"validation\",\"enabled\":%s,\"errors\":%u,\"warnings\":%u}\n",
            boolean(validation_requested), validation.errors.load(), validation.warnings.load());
        return validation.errors.load() ? 1 : exit_code;
    } catch (const std::invalid_argument& error) {
        std::fprintf(stderr, "gpu_share_probe: %s\n", error.what());
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gpu_share_probe: %s\n", error.what());
        return 1;
    }
}
