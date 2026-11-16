#include "app/addon_upscaler_policy.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>

#include "core/ascii_case.h"

namespace hailstone::app {

namespace {

bool loaded_files_match(const InstalledAddon& addon, const std::filesystem::path& folder,
                        const std::function<bool()>& canceled) {
    if (addon.files.empty() || folder.empty()) return false;
    for (const auto& file : addon.files) {
        if (canceled && canceled()) return false;
        const std::filesystem::path relative(
            std::u8string(reinterpret_cast<const char8_t*>(file.path.data()), file.path.size()));
        // Registries are checked when read, but keep this helper safe for a caller's own records too.
        if (relative.empty() || relative.is_absolute() || relative.has_root_name()) return false;
        for (const auto& part : relative)
            if (part == "..") return false;
        std::ifstream in(folder / relative, std::ios::binary);
        if (!in) return false;
        Sha256 hash;
        std::array<char, 65536> bytes{};
        std::uint64_t size = 0;
        while (in) {
            if (canceled && canceled()) return false;
            in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const auto count = in.gcount();
            if (count > 0) {
                size += static_cast<std::uint64_t>(count);
                if (size > file.size) return false;
                hash.update(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            }
        }
        if (in.bad() || size != file.size || hash.finish() != file.sha256) return false;
    }
    return true;
}

} // namespace

std::string addon_upscaler_problem(const plugin::AddonRules& rules, const AddonRegistry& registry,
                                  std::string_view active_name, const std::filesystem::path& loaded_folder,
                                  const std::function<bool()>& canceled) {
    if (!rules.enabled) return {};
    if (!rules.error.empty()) return rules.error;
    const InstalledAddon* active = nullptr;
    for (const auto& addon : registry.addons)
        if (addon.kind == AddonKind::Upscaler && iequals(addon.name, active_name)) active = &addon;
    if (!active_name.empty()) {
        const bool unreadable = std::any_of(registry.unreadable.begin(), registry.unreadable.end(),
                                           [&](const UnreadableAddon& addon) {
                                               return iequals(addon.name, active_name) &&
                                                      (!addon.kind || *addon.kind == AddonKind::Upscaler);
                                           });
        if (unreadable)
            return std::format("The running upscaler '{}' has an unreadable add-on record. Reinstall it and restart "
                               "Hailstone before joining this server.", active_name);
        const std::string_view source = active ? std::string_view(active->source) : std::string_view{};
        if (!plugin::addon_allowed(rules, active_name, source) || (!rules.default_allow && !active))
            return std::format("This server does not allow the active upscaler '{}'. Choose an allowed upscaler "
                               "or a built-in option in Graphics, then restart Hailstone before joining.", active_name);
        if (active && !loaded_files_match(*active, loaded_folder, canceled))
            return std::format("The running upscaler '{}' does not match its installed add-on. Reinstall it if "
                               "needed, then restart Hailstone before joining this server.", active_name);
    }
    for (const auto& addon : registry.addons) {
        if (addon.kind != AddonKind::Upscaler || !plugin::addon_required(rules, addon.name, addon.source)) continue;
        if (!iequals(addon.name, active_name))
            return std::format("This server requires the '{}' upscaler to run. Select it in Graphics and restart "
                               "Hailstone before joining.", addon.name);
    }
    return {};
}

} // namespace hailstone::app
