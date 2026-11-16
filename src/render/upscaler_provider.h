#pragma once

// An upscaler provider (sdk/hailstone_upscaler.h, docs/plugins.md "Upscaler providers"): a temporal upscaler from
// outside the client, an add-on in upscalers/<name>/, loaded at the start when display.upscaler names it. This is the
// client's side of it: finding and loading the library, the requirements it adds to the Vulkan instance and device
// (checked against what they offer), and the calls, each guarded so a provider that says no or throws is dropped
// and the client goes on without it.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <volk.h>

#include "platform/shared_library.h"
#include "render/temporal.h"
#include "../../sdk/hailstone_upscaler.h"

namespace hailstone::render {

// The features a device is made with, as one chain: VkPhysicalDeviceFeatures2 -> Vulkan 1.1 -> 1.2 -> 1.3, then
// `extra`, an upscaler provider's own extension structs (its memory, linked among themselves), the shape
// HailstoneUpscaler::device_requirements fills. Copies relink their own chain.
struct DeviceFeatures {
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    void* extra = nullptr;

    DeviceFeatures() { link(); }
    DeviceFeatures(const DeviceFeatures& o) : f2(o.f2), f11(o.f11), f12(o.f12), f13(o.f13), extra(o.extra) { link(); }
    DeviceFeatures& operator=(const DeviceFeatures& o) {
        f2 = o.f2;
        f11 = o.f11;
        f12 = o.f12;
        f13 = o.f13;
        extra = o.extra;
        link();
        return *this;
    }
    void link() {
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = extra;
    }
};

// After a provider filled `f` (and may have put structs of its own anywhere in its chain): those structs gathered
// into `f.extra`, in their order, and the chain made the four structs then them again. False when the chain holds a
// second struct of one of the four types (the Vulkan spec forbids it), or loops.
bool gather_extra_features(DeviceFeatures& f);

// Turns on in `into` every feature `want` has on, and takes `want`'s extra structs when `into` has none.
void add_features(DeviceFeatures& into, const DeviceFeatures& want);
// The features `want` has on that `have` lacks, named by their struct and place ("Vulkan 1.2 feature 7"): empty when
// the device has them all.
std::vector<std::string> missing_features(const DeviceFeatures& want, const DeviceFeatures& have);
// The names in `want` that are not in `have`.
std::vector<std::string> missing_names(const std::vector<std::string>& want, const std::vector<std::string>& have);

// What an upscaler provider's name may be: an add-on's name (core/addon_manifest.h addon_name_ok) that is not one of
// display.upscaler's own values (fsr1, taa, fsr3, off).
bool upscaler_provider_name_ok(std::string_view name);
// Where the provider `name` is: upscalers_dir/<name>/<name><this platform's library suffix>.
std::filesystem::path upscaler_provider_library(const std::filesystem::path& upscalers_dir, std::string_view name);
// The providers installed in `upscalers_dir`: each folder with a library of its name, sorted by name.
std::vector<std::string> installed_upscaler_providers(const std::filesystem::path& upscalers_dir);
// display.upscaler's value as the scene's upscaler: parse_upscaler's (render/temporal.h), but External for a name an
// upscaler provider may have (installed or not: VkContext falls back from one that is not running).
Upscaler upscaler_from_setting(std::string_view name);

// The folder a provider is loaded from when `copies_dir` is given: a copy of upscalers_dir/<name>/ in
// copies_dir/<name>-<key>/, the key taken from its files' names, sizes and times, so the add-on manager can replace
// the add-on's folder while the copy is loaded (Windows keeps a loaded DLL's folder from moving). A copy already
// there is used as it is (a box set's other windows, the next start); the provider's older copies are deleted where
// nothing holds them. Empty when the copy could not be made (the caller loads in place).
std::filesystem::path upscaler_provider_copy(const std::filesystem::path& upscalers_dir, std::string_view name,
                                             const std::filesystem::path& copies_dir);

class UpscalerProvider;
// The provider display.upscaler (`name`) asks for, from `upscalers_dir` (through a copy in `copies_dir`, when given),
// or nullptr: for the client's own upscalers, and for a provider that did not load (logged).
std::unique_ptr<UpscalerProvider> load_upscaler_setting(std::string_view name,
                                                         const std::filesystem::path& upscalers_dir,
                                                         const std::filesystem::path& copies_dir = {});

class UpscalerProvider {
public:
    // Loads upscalers_dir/<name>/ (from `load_from`, a copy of that folder, when given) and asks for its table;
    // nullptr, with `error` saying why, when it is not there, does not load, has no hailstone_upscaler, says no or
    // is for a newer ABI.
    static std::unique_ptr<UpscalerProvider> load(const std::filesystem::path& upscalers_dir, std::string_view name,
                                                  std::string& error, const std::filesystem::path& load_from = {});
    ~UpscalerProvider();
    UpscalerProvider(const UpscalerProvider&) = delete;
    UpscalerProvider& operator=(const UpscalerProvider&) = delete;

    const std::string& name() const { return name_; }   // its folder's (display.upscaler)
    const std::string& label() const { return label_; } // its own ("Intel XeSS")
    // The bytes actually loaded, which may be an older copy after an add-on update. An opted-in server policy
    // checks this folder rather than mistaking the newly installed provider for the one Vulkan is using.
    const std::filesystem::path& loaded_folder() const { return loaded_folder_; }
    bool sharpens() const { return (flags_ & HAILSTONE_UPSCALER_SHARPENS) != 0; }

    // Each false means the provider is out (logged); the caller drops it.
    bool instance_requirements(std::vector<std::string>& extensions, std::uint32_t& min_api_version);
    bool device_requirements(VkInstance instance, VkPhysicalDevice physical, std::vector<std::string>& extensions,
                             DeviceFeatures& features);
    bool create(const HailstoneUpscalerDevice& device);
    bool resize(VkExtent2D render, VkExtent2D view);
    bool evaluate(const HailstoneUpscalerFrame& frame);
    // Calls the provider's destroy when create succeeded (the device idle); the destructor does too.
    void destroy();

private:
    UpscalerProvider() = default;
    void log(int level, std::string_view line) const;
    static void host_log(void* host, int level, const char* line);
    // Runs one of the table's calls, catching what a C++ provider lets out: false then, logged.
    template <class F> bool guarded(const char* what, F&& call);

    platform::SharedLibrary library_;
    const HailstoneUpscaler* table_ = nullptr;
    HailstoneUpscalerHost host_{};
    std::string name_, label_, folder_utf8_, config_utf8_;
    std::filesystem::path loaded_folder_;
    std::uint32_t flags_ = 0;
    bool created_ = false;
};

} // namespace hailstone::render
