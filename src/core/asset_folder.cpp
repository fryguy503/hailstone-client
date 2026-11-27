#include "core/asset_folder.h"

#include <algorithm>
#include <functional>
#include <system_error>

#include "core/game_dir.h"

namespace hailstone {

namespace {
constexpr const char* kSoundArchives = "snd*.pfs";

// A file name of the form snd<anything>.pfs, any case.
bool is_sound_archive(std::string_view lower_name) {
    return lower_name.size() > 7 && lower_name.starts_with("snd") && lower_name.ends_with(".pfs");
}

// True when the file `parts[i..]` exists under `at`, matching each component case-insensitively. Every match is
// tried, as a case-sensitive file system can hold both Resources/ and RESOURCES/.
bool exists_nocase(const std::filesystem::path& at, const std::vector<std::string>& parts, std::size_t i) {
    std::error_code ec;
    if (i == parts.size()) return std::filesystem::is_regular_file(at, ec);
    for (auto it = std::filesystem::directory_iterator(at, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        if (GameDir::normalize(it->path().filename().string()) == parts[i] && exists_nocase(it->path(), parts, i + 1))
            return true;
    return false;
}

bool exists_nocase(const std::filesystem::path& root, std::string_view relative) {
    std::vector<std::string> parts;
    std::string rel = GameDir::normalize(relative);
    for (std::size_t start = 0; start <= rel.size();) {
        std::size_t slash = std::min(rel.find('/', start), rel.size());
        if (slash > start) parts.push_back(rel.substr(start, slash - start));
        start = slash + 1;
    }
    return !parts.empty() && exists_nocase(root, parts, 0);
}

AssetFolderCheck check(const std::function<bool(const std::string&)>& has, bool any_sound_archive) {
    AssetFolderCheck out;
    out.is_directory = true;
    for (const auto& f : asset_folder_required_files())
        if (!has(f)) out.missing.push_back(f);
    if (!any_sound_archive) out.missing.push_back(kSoundArchives);
    for (const auto& f : asset_folder_rof2_files())
        if (!has(f)) out.missing_rof2.push_back(f);
    for (const auto& f : asset_folder_optional_files())
        if (!has(f)) out.warnings.push_back(f);
    out.live = has("spells_us_str.txt");
    out.ok = out.missing.empty() && out.missing_rof2.empty();
    return out;
}
} // namespace

const std::vector<std::string>& asset_folder_required_files() {
    static const std::vector<std::string> files{
        "spells_us.txt",
        "eqstr_us.txt",
        "dbstr_us.txt",
        "eqlsstr_us.txt",
        "Resources/SkillCaps.txt",
        "Resources/BaseData.txt",
        "racedata.txt",
        "Resources/GlobalLoad.txt",
        "global_chr.s3d",
        "sky.s3d",
        "uifiles/default/EQUI_Animations.xml",
        "soundassets.txt",
    };
    return files;
}

const std::vector<std::string>& asset_folder_rof2_files() {
    static const std::vector<std::string> files{
        "Resources/OnDemandResources.txt",
        "Resources/SpellRequirementAssociations.txt",
        "Resources/PlayerCustomization.txt",
        "spellsnew.edd",
    };
    return files;
}

const std::vector<std::string>& asset_folder_optional_files() {
    static const std::vector<std::string> files{"eqgame.exe"};
    return files;
}

AssetFolderCheck validate_asset_folder(const std::filesystem::path& folder) {
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return {};
    bool sounds = false;
    for (auto it = std::filesystem::directory_iterator(folder, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec) && is_sound_archive(GameDir::normalize(it->path().filename().string()))) {
            sounds = true;
            break;
        }
    }
    return check([&](const std::string& rel) { return exists_nocase(folder, rel); }, sounds);
}

AssetFolderCheck validate_asset_folder(const GameDir& game) {
    if (game.roots().empty()) return {};
    auto top = game.list("");
    bool sounds = std::any_of(top.begin(), top.end(),
                              [](const GameDir::Entry& e) { return is_sound_archive(GameDir::normalize(e.name)); });
    return check([&](const std::string& rel) { return game.exists(rel); }, sounds);
}

bool is_live_asset_folder(const GameDir& game) { return game.find_base("spells_us_str.txt").has_value(); }

const std::vector<std::string>& server_matched_files() {
    static const std::vector<std::string> files{
        "spells_us.txt",          "eqstr_us.txt",           "dbstr_us.txt",
        "Resources/SkillCaps.txt", "Resources/BaseData.txt", "Resources/GlobalLoad.txt",
    };
    return files;
}

std::vector<std::string> missing_server_files(const GameDir& game) {
    std::vector<std::string> out;
    if (!is_live_asset_folder(game)) return out;
    for (const auto& f : server_matched_files())
        if (!game.from_overlay(f)) out.push_back(f);
    return out;
}

const std::vector<std::string>& classic_zone_files() {
    static const std::vector<std::string> files{
        "commons.s3d", "ecommons.s3d", "misty.s3d",     "sro.s3d",          "oasis.s3d",  "highpass.s3d",
        "tox.s3d",     "oot.s3d",      "lavastorm.s3d", "everfrost.s3d",    "bazaar.s3d", "highpasshold.s3d",
        "barter.eqg",  "freeporttemple.eqg",
    };
    return files;
}

ClassicFolderCheck check_classic_folder(const std::filesystem::path& folder) {
    ClassicFolderCheck out;
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return out;
    for (const auto& f : classic_zone_files()) (exists_nocase(folder, f) ? out.has : out.missing).push_back(f);
    out.ok = !out.has.empty();
    return out;
}

bool classic_preferred_file(std::string_view name) {
    if (name.find('/') != std::string_view::npos) return false;
    for (std::string_view zone : {"everfrost", "lavastorm"})
        if (name.size() > zone.size() && name.starts_with(zone) && (name[zone.size()] == '.' || name[zone.size()] == '_'))
            return true;
    return false;
}

GameDir open_game_dir(const std::filesystem::path& base, const std::filesystem::path& classic) {
    GameDir game(base);
    if (!classic.empty())
        game.set_classic(classic, is_live_asset_folder(game) ? GameDir::PreferRule(classic_preferred_file)
                                                             : GameDir::PreferRule());
    return game;
}

std::string describe(const AssetFolderCheck& check) {
    if (check.ok) return {};
    if (!check.is_directory) return "not a folder";
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& f : v) s += (s.empty() ? "" : ", ") + f;
        return s;
    };
    std::string out;
    if (!check.missing.empty()) out = "missing " + join(check.missing);
    if (!check.missing_rof2.empty())
        out += (out.empty() ? "" : "; ") + std::string("missing files required for compatibility (") + join(check.missing_rof2) + ")";
    return out;
}

} // namespace hailstone
