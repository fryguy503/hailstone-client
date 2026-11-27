#include "core/asset_folder_choice.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <system_error>

#include "core/ascii_case.h"
#include "core/config.h"
#include "core/game_dir.h"

namespace hailstone {

namespace {
constexpr char kRecentSeparator = '|'; // not allowed in a Windows path, and rare elsewhere

std::string folder_key(const std::filesystem::path& p) {
    // As UTF-8: generic_string() throws on Windows for a name the ANSI code page cannot show.
    const std::u8string u = p.lexically_normal().generic_u8string();
    std::string s(u.begin(), u.end());
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return s;
}
} // namespace

// The override, then assets.path (resolve_asset_folder without `beside`).
static AssetFolderResolution resolve_saved_asset_folder(const std::filesystem::path& override_dir, const Config& config,
                                                        const AssetFolderValidator& check,
                                                        const std::filesystem::path& data_folder);

AssetFolderResolution resolve_asset_folder(const std::filesystem::path& override_dir, const Config& config,
                                           const AssetFolderValidator& validate,
                                           const std::filesystem::path& data_folder,
                                           const std::filesystem::path& beside) {
    auto check = [&](const std::filesystem::path& p) { return validate ? validate(p) : validate_asset_folder(p); };
    AssetFolderResolution out = resolve_saved_asset_folder(override_dir, config, check, data_folder);
    // Nothing saved works (a first start, or a moved folder): the installation Hailstone sits in, when it is one.
    if (out.source == AssetFolderSource::Picker && !beside.empty() && !asset_folder_pick_requested(config) &&
        (override_dir.empty() || out.reason.empty())) {
        std::error_code ec;
        if (std::filesystem::is_directory(beside, ec) && check(beside).ok &&
            (data_folder.empty() || data_folder_conflict(beside, data_folder, true).empty())) {
            out.source = AssetFolderSource::Found;
            out.path = beside;
            out.reason.clear();
            out.check = {};
        }
    }
    return out;
}

static AssetFolderResolution resolve_saved_asset_folder(const std::filesystem::path& override_dir, const Config& config,
                                                        const AssetFolderValidator& check,
                                                        const std::filesystem::path& data_folder) {
    AssetFolderResolution out;
    std::filesystem::path saved = config.get_string("assets.path", "");
    if (!override_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(override_dir, ec)) {
            out.source = AssetFolderSource::Override;
            out.path = override_dir;
            return out;
        }
        out.path = saved;
        out.reason = "The folder given with --game-dir or EQ_GAME_DIR is not a folder: " + override_dir.string();
        return out;
    }
    out.path = saved;
    if (saved.empty()) return out;
    if (asset_folder_pick_requested(config)) {
        out.reason = "Choose the asset folder to play with.";
        return out;
    }
    if (!data_folder.empty())
        if (std::string why = data_folder_conflict(saved, data_folder); !why.empty()) {
            out.reason = why;
            return out;
        }
    AssetFolderCheck c = check(saved);
    if (c.ok) {
        out.source = AssetFolderSource::Config;
        return out;
    }
    out.reason = c.is_directory ? "The saved folder is missing compatible game files."
                                : "The saved asset folder is not there any more.";
    out.check = std::move(c);
    return out;
}

bool data_folder_subfolder_allowed() {
#if defined(HAILSTONE_ALLOW_IN_EQ_FOLDER) && HAILSTONE_ALLOW_IN_EQ_FOLDER
    return true;
#else
    return false;
#endif
}

std::string data_folder_conflict(const std::filesystem::path& asset_folder, const std::filesystem::path& data_folder,
                                 bool allow_subfolder) {
    namespace fs = std::filesystem;
    if (asset_folder.empty() || data_folder.empty()) return {};
    auto normal = [](const fs::path& p) {
        std::error_code ec;
        fs::path out = fs::weakly_canonical(p, ec);
        if (ec) out = fs::absolute(p, ec).lexically_normal();
        return fs::path(folder_key(out));
    };
    fs::path rel = normal(data_folder).lexically_relative(normal(asset_folder));
    if (rel.empty() || *rel.begin() == "..") return {};
    if (rel == ".")
        return "Hailstone keeps its own files in " + data_folder.string() +
               ", this same folder, and would write into the installation, which it only reads. Move Hailstone into "
               "a folder of its own (or set HAILSTONE_DATA_DIR) and start it again.";
    // A folder of Hailstone's own directly in the installation (<EverQuest>/hailstone/, where a server's patcher may
    // install it), and only in builds made with HAILSTONE_ALLOW_IN_EQ_FOLDER: writing there changes none of the
    // installation's files, and GameDir never indexes that folder (GameDir::kOwnFolder). Any other place inside the
    // installation (maps/, uifiles/, Resources/ or deeper) holds files the game reads, so it stays refused.
    const std::u8string name = rel.generic_u8string(); // as UTF-8: generic_string() throws for some names on Windows
    const bool own_child = std::distance(rel.begin(), rel.end()) == 1 &&
                           iequals(std::string_view(reinterpret_cast<const char*>(name.data()), name.size()),
                                   GameDir::kOwnFolder);
    if (allow_subfolder && own_child) return {};
    if (allow_subfolder)
        return "Hailstone keeps its own files in " + data_folder.string() +
               ", inside this folder, and would write into the installation, which it only reads. Inside the "
               "game folder only a folder named \"" + std::string(GameDir::kOwnFolder) +
               "\" directly in it may hold Hailstone (or set HAILSTONE_DATA_DIR); move it and start it again.";
    return "Hailstone keeps its own files in " + data_folder.string() +
           ", inside this folder, and would write into the installation, which it only reads. Move Hailstone out of "
           "the game folder (or set HAILSTONE_DATA_DIR) and start it again.";
}

bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b) {
    return folder_key(a) == folder_key(b);
}

std::vector<std::string> recent_asset_folders(const Config& config) {
    return split_recent_folders(config.get_string("assets.recent", ""));
}

std::vector<std::string> split_recent_folders(std::string_view all) {
    std::vector<std::string> out;
    for (std::size_t start = 0; start <= all.size();) {
        std::size_t end = std::min(all.find(kRecentSeparator, start), all.size());
        std::string one(all.substr(start, end - start));
        start = end + 1;
        if (one.empty()) continue;
        if (std::none_of(out.begin(), out.end(), [&](const std::string& r) { return same_folder(r, one); }))
            out.push_back(std::move(one));
    }
    return out;
}

std::vector<std::string> push_recent_folder(std::vector<std::string> recent, std::string_view folder, std::size_t max) {
    recent = remove_recent_folder(std::move(recent), folder);
    if (!folder.empty()) recent.insert(recent.begin(), std::string(folder));
    if (recent.size() > max) recent.resize(max);
    return recent;
}

std::vector<std::string> remove_recent_folder(std::vector<std::string> recent, std::string_view folder) {
    std::filesystem::path f(folder);
    std::erase_if(recent, [&](const std::string& r) { return same_folder(r, f); });
    return recent;
}

std::string join_recent_folders(const std::vector<std::string>& recent) {
    std::string out;
    for (const auto& r : recent) {
        if (r.empty() || r.find(kRecentSeparator) != std::string::npos) continue; // would not read back as one
        if (!out.empty()) out += kRecentSeparator;
        out += r;
    }
    return out;
}

void remember_asset_folder(Config& config, const std::filesystem::path& folder) {
    // Made absolute: a typed "../RoF2" would mean another folder when the client starts from another directory.
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(folder, ec);
    std::filesystem::path p = ec ? folder : abs.lexically_normal();
    if (!p.has_filename() && p.has_relative_path()) p = p.parent_path(); // "/games/RoF2/" -> "/games/RoF2"
    std::string text = p.string();
    config.set("assets.path", text);
    config.set("assets.recent", join_recent_folders(push_recent_folder(recent_asset_folders(config), text)));
    request_asset_folder_pick(config, false);
}

void request_asset_folder_pick(Config& config, bool pick) {
    if (pick) config.set_bool("assets.pick", true);
    else config.erase("assets.pick");
}

bool asset_folder_pick_requested(const Config& config) { return config.get_bool("assets.pick", false); }

} // namespace hailstone
