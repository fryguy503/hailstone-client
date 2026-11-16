#include "render/upscaler_provider.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <format>
#include <system_error>

#include "core/addon_manifest.h"
#include "core/log.h"
#include "core/path_utf8.h"

namespace hailstone::render {

namespace {

// Each features struct is its sType and pNext, then VkBool32s only, up to its last field: walked as such, from
// `first` to `last` (both fields of it), so no padding at its end is read.
struct BoolRange {
    const char* label;
    std::size_t first, last; // offsets of the first and last VkBool32
};
constexpr BoolRange kCore{"Vulkan 1.0", offsetof(VkPhysicalDeviceFeatures2, features) +
                                             offsetof(VkPhysicalDeviceFeatures, robustBufferAccess),
                          offsetof(VkPhysicalDeviceFeatures2, features) +
                              offsetof(VkPhysicalDeviceFeatures, inheritedQueries)};
constexpr BoolRange k11{"Vulkan 1.1", offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess),
                        offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters)};
constexpr BoolRange k12{"Vulkan 1.2", offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge),
                        offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId)};
constexpr BoolRange k13{"Vulkan 1.3", offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess),
                        offsetof(VkPhysicalDeviceVulkan13Features, maintenance4)};

VkBool32 read_bool(const void* s, std::size_t offset) {
    VkBool32 v;
    std::memcpy(&v, static_cast<const unsigned char*>(s) + offset, sizeof v);
    return v;
}

void write_bool(void* s, std::size_t offset, VkBool32 v) {
    std::memcpy(static_cast<unsigned char*>(s) + offset, &v, sizeof v);
}

// The four structs, by their place in DeviceFeatures.
struct Part {
    BoolRange r;
    std::size_t at;
};
const Part kParts[] = {{kCore, offsetof(DeviceFeatures, f2)},
                       {k11, offsetof(DeviceFeatures, f11)},
                       {k12, offsetof(DeviceFeatures, f12)},
                       {k13, offsetof(DeviceFeatures, f13)}};

const unsigned char* bytes(const DeviceFeatures& f, const Part& p) {
    return reinterpret_cast<const unsigned char*>(&f) + p.at;
}

constexpr const char* kLogPrefix = "upscaler";

// What the provider gets as vkGetInstanceProcAddr: the loader's, looked up at each call. The provider is loaded
// before VkContext starts volk, so the loader's own pointer is not there yet when its table is asked for.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL forward_get_instance_proc_addr(VkInstance instance, const char* name) {
    return vkGetInstanceProcAddr ? vkGetInstanceProcAddr(instance, name) : nullptr;
}

} // namespace

bool gather_extra_features(DeviceFeatures& f) {
    std::vector<VkBaseOutStructure*> extra;
    std::size_t steps = 0;
    for (auto* s = static_cast<VkBaseOutStructure*>(f.f2.pNext); s; s = s->pNext) {
        if (++steps > 64) return false; // a chain that loops (or is absurd)
        if (s == reinterpret_cast<VkBaseOutStructure*>(&f.f11) || s == reinterpret_cast<VkBaseOutStructure*>(&f.f12) ||
            s == reinterpret_cast<VkBaseOutStructure*>(&f.f13))
            continue;
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ||
            s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
            s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
            s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
            return false;
        extra.push_back(s);
    }
    for (std::size_t i = 0; i < extra.size(); ++i) extra[i]->pNext = i + 1 < extra.size() ? extra[i + 1] : nullptr;
    f.extra = extra.empty() ? nullptr : extra.front();
    f.link();
    return true;
}

void add_features(DeviceFeatures& into, const DeviceFeatures& want) {
    if (!into.extra && want.extra) {
        into.extra = want.extra;
        into.link();
    }
    for (const Part& p : kParts)
        for (std::size_t o = p.r.first; o <= p.r.last; o += sizeof(VkBool32))
            if (read_bool(bytes(want, p), o))
                write_bool(reinterpret_cast<unsigned char*>(&into) + p.at, o, VK_TRUE);
}

std::vector<std::string> missing_features(const DeviceFeatures& want, const DeviceFeatures& have) {
    std::vector<std::string> out;
    for (const Part& p : kParts)
        for (std::size_t o = p.r.first; o <= p.r.last; o += sizeof(VkBool32))
            if (read_bool(bytes(want, p), o) && !read_bool(bytes(have, p), o))
                out.push_back(std::format("{} feature {}", p.r.label, (o - p.r.first) / sizeof(VkBool32)));
    return out;
}

std::vector<std::string> missing_names(const std::vector<std::string>& want, const std::vector<std::string>& have) {
    std::vector<std::string> out;
    for (const std::string& n : want)
        if (std::find(have.begin(), have.end(), n) == have.end()) out.push_back(n);
    return out;
}

bool upscaler_provider_name_ok(std::string_view name) {
    return addon_name_ok(name) && name != "fsr1" && name != "taa" && name != "fsr3" && name != "off";
}

std::filesystem::path upscaler_provider_library(const std::filesystem::path& upscalers_dir, std::string_view name) {
    const std::string file = std::string(name) + std::string(platform::shared_library_suffix());
    return upscalers_dir / std::filesystem::path(std::u8string(name.begin(), name.end())) /
           std::filesystem::path(std::u8string(file.begin(), file.end()));
}

std::vector<std::string> installed_upscaler_providers(const std::filesystem::path& upscalers_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(upscalers_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string name = path_utf8(it->path().filename());
        if (upscaler_provider_name_ok(name) &&
            std::filesystem::is_regular_file(upscaler_provider_library(upscalers_dir, name), ec))
            out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Upscaler upscaler_from_setting(std::string_view name) {
    const Upscaler u = parse_upscaler(name);
    if (u == Upscaler::Fsr1 && name != upscaler_name(Upscaler::Fsr1) && upscaler_provider_name_ok(name))
        return Upscaler::External;
    return u;
}

std::filesystem::path upscaler_provider_copy(const std::filesystem::path& upscalers_dir, std::string_view name,
                                             const std::filesystem::path& copies_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path source = upscaler_provider_library(upscalers_dir, name).parent_path();
    // The key: each file's place, size and time, FNV-1a over them, in a sorted order.
    std::vector<std::string> lines;
    for (fs::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto size = it->file_size(ec);
        const auto time = it->last_write_time(ec).time_since_epoch().count();
        lines.push_back(std::format("{}|{}|{}", generic_path_utf8(fs::relative(it->path(), source, ec)), size,
                                    static_cast<long long>(time)));
    }
    if (ec || lines.empty()) return {};
    std::sort(lines.begin(), lines.end());
    std::uint64_t h = 1469598103934665603ull;
    for (const std::string& line : lines)
        for (unsigned char c : line + "\n") h = (h ^ c) * 1099511628211ull;
    const std::string prefix = std::string(name) + "-";
    const fs::path copy = copies_dir / fs::path(std::u8string(prefix.begin(), prefix.end())).concat(std::format("{:016x}", h));
    // A copy is made beside its place and renamed into it whole, so another window never loads half of one.
    if (!fs::is_directory(copy, ec)) {
        const fs::path part = fs::path(copy).concat(std::format(".part{:x}", static_cast<std::uint64_t>(
                                                                          std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(part, ec);
        if (!ec) fs::copy(source, part, fs::copy_options::recursive, ec);
        if (!ec) fs::rename(part, copy, ec);
        if (ec) {
            std::error_code ignored;
            fs::remove_all(part, ignored);
            if (!fs::is_directory(copy, ignored)) return {}; // not even another window's
        }
    }
    // The provider's older copies (its name, '-', 16 hex digits: not another provider whose name goes on from its):
    // deleted where nothing has them loaded.
    auto ours = [&prefix](const std::string& n) {
        return n.size() == prefix.size() + 16 && n.starts_with(prefix) &&
               std::all_of(n.begin() + static_cast<std::ptrdiff_t>(prefix.size()), n.end(),
                           [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    for (fs::directory_iterator it(copies_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string other = path_utf8(it->path().filename());
        if (ours(other) && it->path() != copy) {
            std::error_code ignored;
            fs::remove_all(it->path(), ignored);
        }
    }
    return copy;
}

std::unique_ptr<UpscalerProvider> load_upscaler_setting(std::string_view name,
                                                         const std::filesystem::path& upscalers_dir,
                                                         const std::filesystem::path& copies_dir) {
    if (upscaler_from_setting(name) != Upscaler::External) return nullptr;
    std::string error;
    std::filesystem::path from;
    if (!copies_dir.empty() && upscaler_provider_name_ok(name)) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(upscaler_provider_library(upscalers_dir, name), ec)) {
            from = upscaler_provider_copy(upscalers_dir, name, copies_dir);
            if (from.empty()) log::warn("upscaler {}: no copy could be made in {}; loaded in place", name,
                                        path_utf8(copies_dir));
        }
    }
    std::unique_ptr<UpscalerProvider> p = UpscalerProvider::load(upscalers_dir, name, error, from);
    if (p)
        log::info("upscaler {}: loaded {} from {}", p->name(), p->label(), path_utf8(upscalers_dir / p->name()));
    else
        log::warn("upscaler {}: not loaded, {}; the client's own upscaler is used instead", name, error);
    return p;
}

std::unique_ptr<UpscalerProvider> UpscalerProvider::load(const std::filesystem::path& upscalers_dir,
                                                         std::string_view name, std::string& error,
                                                         const std::filesystem::path& load_from) {
    if (!upscaler_provider_name_ok(name)) {
        error = std::format("'{}' is not a provider's name", name);
        return nullptr;
    }
    const std::filesystem::path file = upscaler_provider_library(upscalers_dir, name);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        error = std::format("{} is not installed ({} not found)", name, path_utf8(file));
        return nullptr;
    }
    std::unique_ptr<UpscalerProvider> p(new UpscalerProvider());
    p->name_ = std::string(name);
    p->label_ = p->name_;
    p->config_utf8_ = path_utf8(file.parent_path());
    const std::filesystem::path library = load_from.empty() ? file : load_from / file.filename();
    p->loaded_folder_ = library.parent_path();
    p->folder_utf8_ = path_utf8(library.parent_path());
    p->library_ = platform::SharedLibrary(library);
    if (!p->library_.loaded()) {
        error = std::format("{} did not load: {}", path_utf8(library), p->library_.error());
        return nullptr;
    }
    auto entry = reinterpret_cast<HailstoneUpscalerEntry>(p->library_.symbol(HAILSTONE_UPSCALER_ENTRY));
    if (!entry) {
        error = std::format("{} has no {}", path_utf8(file), HAILSTONE_UPSCALER_ENTRY);
        return nullptr;
    }
    p->host_.abi = HAILSTONE_UPSCALER_ABI;
    p->host_.size = sizeof(HailstoneUpscalerHost);
    p->host_.host = p.get();
    p->host_.folder = p->folder_utf8_.c_str();
    p->host_.config_dir = p->config_utf8_.c_str();
    p->host_.log = &UpscalerProvider::host_log;
    p->host_.get_instance_proc_addr = reinterpret_cast<void*>(&forward_get_instance_proc_addr);
    const HailstoneUpscaler* table = nullptr;
    try {
        table = entry(&p->host_);
    } catch (...) {
        table = nullptr;
    }
    if (!table) {
        error = std::format("{} said no (its lines in the log say why)", name);
        return nullptr;
    }
    // The table must hold every function this client calls; one built for a newer ABI may rely on calls it lacks.
    if (table->abi > HAILSTONE_UPSCALER_ABI || table->size < offsetof(HailstoneUpscaler, destroy) + sizeof(table->destroy)) {
        error = std::format("{} is for a newer Hailstone (upscaler ABI {}, this one has {})", name, table->abi,
                            HAILSTONE_UPSCALER_ABI);
        return nullptr;
    }
    if (!table->instance_requirements || !table->device_requirements || !table->create || !table->resize ||
        !table->evaluate || !table->destroy) {
        error = std::format("{} lacks one of the functions its table must have", name);
        return nullptr;
    }
    p->table_ = table;
    if (table->name && *table->name) p->label_ = std::string(table->name).substr(0, 64);
    p->flags_ = table->flags;
    return p;
}

UpscalerProvider::~UpscalerProvider() {
    destroy();
    table_ = nullptr;
    library_.unload();
}

void UpscalerProvider::log(int level, std::string_view line) const {
    if (level >= HAILSTONE_UPSCALER_LOG_WARN)
        log::warn("{} {}: {}", kLogPrefix, name_, line);
    else
        log::info("{} {}: {}", kLogPrefix, name_, line);
}

void UpscalerProvider::host_log(void* host, int level, const char* line) {
    if (host && line) static_cast<const UpscalerProvider*>(host)->log(level, line);
}

template <class F> bool UpscalerProvider::guarded(const char* what, F&& call) {
    if (!table_) return false;
    try {
        if (call()) return true;
        log(HAILSTONE_UPSCALER_LOG_WARN, std::format("{} said no", what));
    } catch (const std::exception& e) {
        log(HAILSTONE_UPSCALER_LOG_ERROR, std::format("{} threw: {}", what, e.what()));
    } catch (...) {
        log(HAILSTONE_UPSCALER_LOG_ERROR, std::format("{} threw", what));
    }
    return false;
}

namespace {
void take_names(const char* const* names, std::uint32_t count, std::vector<std::string>& into) {
    for (std::uint32_t i = 0; names && i < count; ++i)
        if (names[i] && *names[i] &&
            std::find(into.begin(), into.end(), names[i]) == into.end())
            into.emplace_back(names[i]);
}
} // namespace

bool UpscalerProvider::instance_requirements(std::vector<std::string>& extensions, std::uint32_t& min_api_version) {
    const char* const* names = nullptr;
    std::uint32_t count = 0;
    min_api_version = 0;
    if (!guarded("instance_requirements", [&] {
            return table_->instance_requirements(&host_, &names, &count, &min_api_version) != 0;
        }))
        return false;
    take_names(names, count, extensions);
    return true;
}

bool UpscalerProvider::device_requirements(VkInstance instance, VkPhysicalDevice physical,
                                           std::vector<std::string>& extensions, DeviceFeatures& features) {
    const char* const* names = nullptr;
    std::uint32_t count = 0;
    DeviceFeatures asked;
    if (!guarded("device_requirements", [&] {
            return table_->device_requirements(&host_, instance, physical, &names, &count, &asked.f2) != 0;
        }))
        return false;
    // Structs of its own for extension features: gathered after the four, which it may have relinked.
    if (!gather_extra_features(asked)) {
        log(HAILSTONE_UPSCALER_LOG_WARN, "device_requirements gave a second struct of the Vulkan 1.0 to 1.3 features "
                                         "(or a chain that loops)");
        return false;
    }
    take_names(names, count, extensions);
    features = asked;
    return true;
}

bool UpscalerProvider::create(const HailstoneUpscalerDevice& device) {
    created_ = guarded("create", [&] { return table_->create(&host_, &device) != 0; });
    return created_;
}

bool UpscalerProvider::resize(VkExtent2D render, VkExtent2D view) {
    return created_ && guarded("resize", [&] {
               return table_->resize(&host_, render.width, render.height, view.width, view.height) != 0;
           });
}

bool UpscalerProvider::evaluate(const HailstoneUpscalerFrame& frame) {
    return created_ && guarded("evaluate", [&] { return table_->evaluate(&host_, &frame) != 0; });
}

void UpscalerProvider::destroy() {
    if (!created_ || !table_) return;
    created_ = false;
    try {
        table_->destroy(&host_);
    } catch (...) {
        log(HAILSTONE_UPSCALER_LOG_ERROR, "destroy threw");
    }
}

} // namespace hailstone::render
