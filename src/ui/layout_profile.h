#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ini_file.h"

namespace hailstone::ui {

struct LayoutProfileRect {
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const LayoutProfileRect&) const = default;
};

namespace lua {
// The active pack's visible windows, clipped to the viewport. Implemented by the ImGui Lua layer.
std::vector<LayoutProfileRect> snapshot_pack_window_rects(int width, int height);
}

inline constexpr std::size_t kLayoutProfileMaxRects = 256;

struct LayoutProfile {
    std::string name;
    int width = 0, height = 0;
    IniFile windows, chat;
    std::string pack_settings;
    // Preview bounds in the saved viewport's coordinates. Reading and saving drop unusable ones.
    std::vector<LayoutProfileRect> rects;
};

// Names are local to a pack and matched case-insensitively on all platforms.
bool valid_layout_profile_name(std::string_view name);
std::vector<std::string> list_layout_profiles(const std::filesystem::path& directory);
std::optional<LayoutProfile> read_layout_profile(const std::filesystem::path& directory, std::string_view name);
bool has_saved_layout_profile(const std::filesystem::path& directory, const LayoutProfile& current);
enum class LayoutProfileSave { Saved, Exists, Failed };
LayoutProfileSave save_layout_profile(const std::filesystem::path& directory, const LayoutProfile& profile,
                                      bool overwrite);

// The stock UI.ini also contains unrelated character settings. Replace only window placement and chat layout keys.
IniFile capture_layout_windows(const IniFile& current);
IniFile replace_layout_windows(IniFile current, const IniFile& snapshot);
bool layout_profile_manager_setting(std::string_view key);

} // namespace hailstone::ui
