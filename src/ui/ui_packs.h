#pragma once

// Registered interfaces come from <client>/ui and installed UI add-ons' owned roots. Each ui.ini declares its
// presentation metadata and optional base. Approved bases supply missing windows/helpers; settings belong to
// the selected stable ID (a built-in folder name or addon.<addon>.<pack>).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::ui {

struct UiPack {
    std::string id;          // stable registry ID (ui.name)
    std::string name;        // shown in the Options window
    std::string description;
    std::string input;       // mouse, gamepad, touch (comma-separated)
    bool game_art = false;   // game_art = all: every uifiles/default file; else only the item, spell and gem icons
    std::filesystem::path dir;
    std::string base;        // immediate local/built-in/qualified base reference; empty for a complete pack
    std::vector<std::filesystem::path> base_dirs; // approved registered roots in the validated chain, nearest first
    std::string source = "builtin"; // builtin or addon; identity and settings never depend on the display name
    std::string addon{};      // owning installed UI add-on, empty for built-in/manual ui/ packs
    std::string local_id{};   // folder name inside its source root
    std::vector<std::string> base_ids{}; // the qualified IDs corresponding to base_dirs
    std::string runtime = "lua";
    bool available = true;
    std::string unavailable_reason{};
    std::string version{}, revision{}; // installed bundle version and manifest generation (including approved bases)
    bool operator==(const UiPack&) const = default;
};

struct InterfaceStatus {
    UiPack selected, loaded;
    std::string state = "loading";
    std::vector<std::string> bases, diagnostics;
    std::size_t script_count = 0, loaded_script_count = 0, error_count = 0, window_count = 0;
};

// The valid UIs under `root`, by name; no outside links, missing/cyclic bases or chains longer than 32 packs.
// Invalid packs are logged and optional diagnostics appended. input/game_art inherit only when not declared.
std::vector<UiPack> list_ui_packs(const std::filesystem::path& root,
                                std::vector<std::string>* diagnostics = nullptr);
// Managed bundles are <managed_root>/<addon>/ui/<pack>/ui.ini. Only the installed UI names supplied by the
// registry authorize roots. Their IDs are addon.<addon>.<pack>; each component is an ASCII add-on name.
std::vector<UiPack> list_ui_packs(const std::filesystem::path& root, const std::filesystem::path& managed_root,
                                const std::vector<std::string>& installed_ui_addons,
                                std::vector<std::string>* diagnostics = nullptr);
// The available UI `id`, else available "modern", else the first available entry; null when none is available.
const UiPack* find_ui_pack(const std::vector<UiPack>& packs, std::string_view id);
// Exact installed record, including an unavailable backend; never substitutes another interface.
const UiPack* find_ui_pack_exact(const std::vector<UiPack>& packs, std::string_view id);
// A plugin's nearest existing ui/<selected-or-base> folder; an existing empty folder is an intentional override.
std::filesystem::path plugin_ui_folder(const std::filesystem::path& plugin, const UiPack& pack);

} // namespace hailstone::ui
