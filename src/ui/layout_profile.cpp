#include "ui/layout_profile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "core/atomic_file.h"
#include "core/byte_io.h"

namespace hailstone::ui {
namespace {

constexpr std::string_view kMagic = "Hailstone layout profile 1\n";
constexpr std::size_t kMaxBytes = 2 * 1024 * 1024;
constexpr std::string_view kSuffix = ".hslayout";
constexpr std::size_t kMaxRectName = 128;
constexpr int kMaxRectCoordinate = 65536;

std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::filesystem::path profile_path(const std::filesystem::path& directory, std::string_view name) {
    return directory / ("profile-" + folded(name) + std::string(kSuffix));
}

bool window_key(std::string_view section, std::string_view key) {
    const auto name = folded(section);
    if (name == "layoutprofileswindow") return false;
    if (name == "chatmanager") return true;
    constexpr std::string_view keys[] = {
        "iniversion", "xposwindowed", "yposwindowed", "widthwindowed", "heightwindowed", "minimizedwindowed",
        "restorexposwindowed", "restoreyposwindowed", "restorewidthwindowed", "restoreheightwindowed",
        "show", "bgtint.red", "bgtint.green", "bgtint.blue", "bgtype", "fades", "delay", "duration",
        "alpha", "fadetoalpha", "locked", "globalfadeduration", "globalfadedelay", "globalalpha",
        "globalfadetoalpha", "percentsize", "showkeymap", "showspinner",
    };
    return std::ranges::find(keys, folded(key)) != std::end(keys);
}

bool preview_coordinate(int value) {
    return value >= -kMaxRectCoordinate && value <= kMaxRectCoordinate;
}

bool usable_preview_rect(const LayoutProfileRect& rect, int width, int height) {
    if (rect.name.empty() || rect.name.size() > kMaxRectName ||
        rect.name.find_first_not_of(' ') == std::string::npos ||
        std::ranges::any_of(rect.name, [](unsigned char c) { return c < 32 || c == 127; })) return false;
    return width > 0 && height > 0 && preview_coordinate(rect.x) && preview_coordinate(rect.y) &&
           rect.w > 0 && rect.w <= kMaxRectCoordinate && rect.h > 0 && rect.h <= kMaxRectCoordinate &&
           rect.x < width && rect.y < height && rect.x + rect.w > 0 && rect.y + rect.h > 0;
}

std::vector<LayoutProfileRect> recorded_preview_rects(const LayoutProfile& profile) {
    std::vector<LayoutProfileRect> rects;
    rects.reserve(std::min(profile.rects.size(), kLayoutProfileMaxRects));
    for (const auto& rect : profile.rects) {
        if (usable_preview_rect(rect, profile.width, profile.height)) rects.push_back(rect);
        if (rects.size() == kLayoutProfileMaxRects) break;
    }
    return rects;
}

std::optional<LayoutProfile> read_profile_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > kMaxBytes || ec) return std::nullopt;
    auto bytes = read_text_file(path);
    if (!bytes || bytes->size() > kMaxBytes) return std::nullopt;
    if (!bytes->starts_with(kMagic)) return std::nullopt;
    ByteReader reader(bytes->data() + kMagic.size(), bytes->size() - kMagic.size());
    try {
        auto field = [&] {
            auto value = reader.bytes(reader.le<std::uint32_t>());
            return std::string_view(reinterpret_cast<const char*>(value.data()), value.size());
        };
        LayoutProfile profile;
        profile.name = field();
        if (!valid_layout_profile_name(profile.name) || profile_path(path.parent_path(), profile.name) != path)
            return std::nullopt;
        profile.width = reader.le<std::int32_t>();
        profile.height = reader.le<std::int32_t>();
        profile.windows.parse(field());
        profile.chat.parse(field());
        profile.pack_settings = field();
        const auto count = reader.le<std::uint32_t>();
        if (count > kLayoutProfileMaxRects) return std::nullopt;
        profile.rects.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto name = field();
            const int x = reader.le<std::int32_t>(), y = reader.le<std::int32_t>();
            const int w = reader.le<std::int32_t>(), h = reader.le<std::int32_t>();
            if (name.size() > kMaxRectName) continue;
            LayoutProfileRect rect{std::string(name), x, y, w, h};
            if (usable_preview_rect(rect, profile.width, profile.height)) profile.rects.push_back(std::move(rect));
        }
        if (reader.empty() && profile.width > 0 && profile.height > 0) return profile;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> profile_files(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != kSuffix) continue;
        std::error_code type_error;
        if (it->is_regular_file(type_error)) paths.push_back(it->path());
    }
    return paths;
}

bool same_settings(const IniFile& a, const IniFile& b) {
    std::size_t count = 0;
    for (const auto& section : a.sections())
        for (const auto& key : a.keys(section)) {
            if (a.get(section, key) != b.get(section, key)) return false;
            ++count;
        }
    std::size_t other_count = 0;
    for (const auto& section : b.sections()) other_count += b.keys(section).size();
    return count == other_count;
}

} // namespace

bool layout_profile_manager_setting(std::string_view key) {
    return key.starts_with("hud.") && key.ends_with(".LayoutProfilesWindow");
}

bool valid_layout_profile_name(std::string_view name) {
    if (name.empty() || name.size() > 48 || name.front() == ' ' || name.back() == ' ') return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == ' ' || c == '-' || c == '_';
    });
}

std::vector<std::string> list_layout_profiles(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const auto& path : profile_files(directory))
        if (auto profile = read_profile_file(path)) names.push_back(std::move(profile->name));
    std::ranges::sort(names, {}, folded);
    return names;
}

std::optional<LayoutProfile> read_layout_profile(const std::filesystem::path& directory, std::string_view name) {
    if (!valid_layout_profile_name(name)) return std::nullopt;
    return read_profile_file(profile_path(directory, name));
}

bool has_saved_layout_profile(const std::filesystem::path& directory, const LayoutProfile& current) {
    // Match what the profile format can restore. INI reload trims the trailing spaces of a live chat title.
    IniFile windows, chat;
    windows.parse(current.windows.to_string());
    chat.parse(current.chat.to_string());
    for (const auto& path : profile_files(directory)) {
        const auto profile = read_profile_file(path);
        // Preview geometry and capture size are descriptive. Only restorable settings determine a match.
        if (profile && profile->pack_settings == current.pack_settings && same_settings(profile->windows, windows) &&
            same_settings(profile->chat, chat)) return true;
    }
    return false;
}

LayoutProfileSave save_layout_profile(const std::filesystem::path& directory, const LayoutProfile& profile,
                                      bool overwrite) {
    if (directory.empty() || !valid_layout_profile_name(profile.name)) return LayoutProfileSave::Failed;
    const auto path = profile_path(directory, profile.name);
    const std::array<std::string, 3> payload = {
        profile.windows.to_string(), profile.chat.to_string(), profile.pack_settings,
    };
    const auto rects = recorded_preview_rects(profile);
    std::size_t size = kMagic.size() + (payload.size() + 4) * sizeof(std::uint32_t) + profile.name.size();
    for (const auto& part : payload) size += part.size();
    for (const auto& rect : rects) size += sizeof(std::uint32_t) + 4 * sizeof(std::int32_t) + rect.name.size();
    if (size > kMaxBytes || profile.width <= 0 || profile.height <= 0) return LayoutProfileSave::Failed;

    ByteWriter writer;
    writer.reserve(size);
    writer.fixed_string(kMagic, kMagic.size());
    auto field = [&](std::string_view value) {
        writer.le(static_cast<std::uint32_t>(value.size()));
        writer.fixed_string(value, value.size());
    };
    field(profile.name);
    writer.le(static_cast<std::int32_t>(profile.width));
    writer.le(static_cast<std::int32_t>(profile.height));
    for (const auto& part : payload) field(part);
    writer.le(static_cast<std::uint32_t>(rects.size()));
    for (const auto& rect : rects) {
        field(rect.name);
        writer.le(static_cast<std::int32_t>(rect.x));
        writer.le(static_cast<std::int32_t>(rect.y));
        writer.le(static_cast<std::int32_t>(rect.w));
        writer.le(static_cast<std::int32_t>(rect.h));
    }
    const auto& bytes = writer.data();
    const std::string_view contents(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (overwrite) return write_file_atomic(path, contents) ? LayoutProfileSave::Saved : LayoutProfileSave::Failed;
    const auto result = create_file_atomic(path, contents);
    if (result == AtomicCreate::Exists) return LayoutProfileSave::Exists;
    return result == AtomicCreate::Created ? LayoutProfileSave::Saved : LayoutProfileSave::Failed;
}

IniFile capture_layout_windows(const IniFile& current) {
    IniFile snapshot;
    for (const auto& section : current.sections())
        for (const auto& key : current.keys(section))
            if (window_key(section, key)) snapshot.set(section, key, current.get_string(section, key));
    return snapshot;
}

IniFile replace_layout_windows(IniFile current, const IniFile& snapshot) {
    for (const auto& section : current.sections()) {
        if (folded(section) == "chatmanager") current.erase_section(section);
        else
            for (const auto& key : current.keys(section))
                if (window_key(section, key)) current.erase(section, key);
    }
    for (const auto& section : snapshot.sections())
        for (const auto& key : snapshot.keys(section))
            if (window_key(section, key)) current.set(section, key, snapshot.get_string(section, key));
    return current;
}

} // namespace hailstone::ui
