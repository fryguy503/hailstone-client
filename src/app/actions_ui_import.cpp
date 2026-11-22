#include "app/actions_ui.h"

#include <memory>
#include <system_error>

#include "core/atomic_file.h"
#include "core/sha256.h"
#include "game/lua_social.h"
#include "platform/read_only_directory.h"

namespace hailstone::app {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kImportTargetLimit = 2 * 1024 * 1024;
constexpr std::string_view kBackupHeader = "Hailstone character import 1\n";
constexpr std::size_t kDigestLength = 64;
using SocialSlots = std::bitset<game::kSocialPages * game::kSocialButtons>;

bool missing(const fs::file_status& status, const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found);
}

bool safe_existing_parent(fs::path directory) {
    std::error_code ec;
    while (!directory.empty()) {
        const auto status = fs::symlink_status(directory, ec);
        if (!missing(status, ec))
            return !ec && fs::is_directory(status) && platform::ReadOnlyDirectory::open(directory).has_value();
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return false;
}

std::optional<SocialSlots> scripted_slots(const fs::path& directory) {
    SocialSlots slots;
    std::error_code ec;
    const auto status = fs::symlink_status(directory, ec);
    if (missing(status, ec)) return slots;
    if (ec || !fs::is_directory(status)) return std::nullopt;
    for (int index = 0; index < game::kSocialPages * game::kSocialButtons; ++index) {
        const auto entry = fs::symlink_status(directory / game::lua_social_file_name(index), ec);
        if (ec && !missing(entry, ec)) return std::nullopt;
        if (!missing(entry, ec)) slots.set(static_cast<std::size_t>(index));
    }
    return slots;
}

bool social_ini_changed(const IniFile& before, const IniFile& after, int index) {
    const std::string key = "Page" + std::to_string(index / game::kSocialButtons + 1) +
                            "Button" + std::to_string(index % game::kSocialButtons + 1);
    for (const std::string_view field : {"Name", "Color", "Line1", "Line2", "Line3", "Line4", "Line5"}) {
        const std::string entry = key + std::string(field);
        if (before.get("Socials", entry) != after.get("Socials", entry)) return true;
    }
    return false;
}

std::optional<std::string> read_settings(const fs::path& path, std::size_t limit, bool allow_missing = false) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (allow_missing && missing(status, ec) && safe_existing_parent(path.parent_path())) return std::string();
    if (ec || !fs::is_regular_file(status)) return std::nullopt;
    const auto directory = platform::ReadOnlyDirectory::open(path.parent_path());
    return directory ? directory->read(path.filename(), limit) : std::nullopt;
}

fs::path backup_path(const fs::path& target, const std::string& digest) {
    // The full digest is verified inside the backup. A short name leaves room for the atomic writer's suffix.
    return target.parent_path() / "import-backups" / (digest.substr(0, 24) + ".ini");
}

std::optional<std::string> read_backup(const fs::path& target, std::string_view revision) {
    const auto digest = to_hex(sha256(revision));
    const auto prefix = std::string(kBackupHeader) + digest + '\n';
    const auto offset = prefix.size() + kDigestLength;
    auto bytes = read_settings(backup_path(target, digest), kImportTargetLimit + offset + 1);
    if (!bytes || !bytes->starts_with(prefix) || bytes->size() <= offset || (*bytes)[offset] != '\n') return std::nullopt;
    const std::string_view data(*bytes);
    const auto contents = data.substr(offset + 1);
    if (to_hex(sha256(contents)) != data.substr(prefix.size(), kDigestLength) || contents == revision)
        return std::nullopt;
    return std::string(contents);
}
} // namespace

std::optional<std::string> ActionsUi::import_revision(std::string& error) const {
    if (char_ini_path_.empty() || dirty_) {
        error = "Wait for the active character's settings to save and try again.";
        return std::nullopt;
    }
    auto bytes = read_settings(char_ini_path_, kImportTargetLimit, true);
    if (!bytes) {
        error = "The active character's settings could not be read.";
        return std::nullopt;
    }
    IniFile on_disk;
    on_disk.parse(*bytes);
    if (on_disk.to_string() != char_ini_.to_string()) {
        error = "The active character's settings changed on disk. Re-enter the character before importing.";
        return std::nullopt;
    }
    return bytes;
}

ActionsUi::ImportPreview ActionsUi::stage_import(std::string_view source, game::CharacterImportOptions options,
                                                 game::HotButtonStore& staged, IniFile& ini) const {
    ImportPreview preview;
    const auto before = import_revision(preview.plan.error);
    if (!before) return preview;
    preview.target = char_ini_path_;
    preview.before = *before;

    if (options.socials || options.hot_buttons) {
        const auto protected_socials = scripted_slots(lua_socials_dir());
        if (!protected_socials) {
            preview.plan.error = "The active character's scripted socials could not be checked.";
            return preview;
        }
        options.protected_socials |= *protected_socials;
    }
    preview.plan = game::plan_character_import(source, store_, options, staged);
    if (!preview.plan.error.empty() || preview.plan.additions == 0) return preview;
    ini = char_ini_;
    game::write_character_import_additions(ini, staged, preview.plan);
    preview.after = ini.to_string();
    if (preview.after.size() > kImportTargetLimit) preview.plan.error = "The resulting character settings are too large.";
    return preview;
}

ActionsUi::ImportPreview ActionsUi::preview_import(std::string_view source, game::CharacterImportOptions options) const {
    auto staged = std::make_unique<game::HotButtonStore>();
    IniFile ini;
    return stage_import(source, options, *staged, ini);
}

bool ActionsUi::apply_import(const ImportPreview& preview, std::string_view source, game::CharacterImportOptions options,
                             std::string& error) {
    if (current_social() >= 0) {
        error = "Wait for the running social to finish before applying.";
        return false;
    }
    auto staged = std::make_unique<game::HotButtonStore>();
    IniFile ini;
    const ImportPreview checked = stage_import(source, options, *staged, ini);
    if (!checked.plan.error.empty()) {
        error = checked.plan.error;
        return false;
    }
    if (checked.plan.additions == 0 || checked.target != preview.target || checked.before != preview.before ||
        checked.after != preview.after || checked.after == checked.before) {
        error = "The preview is out of date. Review the changes again.";
        return false;
    }
    const auto digest = to_hex(sha256(checked.after));
    const auto path = backup_path(checked.target, digest);
    const auto backup = std::string(kBackupHeader) + digest + '\n' + to_hex(sha256(checked.before)) + '\n' + checked.before;
    if (!safe_existing_parent(path.parent_path()) || !write_file_atomic(path, backup)) {
        error = "Hailstone could not save an import backup.";
        return false;
    }
    const auto before = import_revision(error);
    if (!before || *before != checked.before) {
        if (before) error = "The character settings changed while creating the backup. Preview the import again.";
        return false;
    }
    if (!ini.save(checked.target)) {
        error = "Hailstone could not save the imported settings.";
        return false;
    }
    char_ini_ = std::move(ini);
    store_ = std::move(*staged);
    return true;
}

bool ActionsUi::has_import_backup() const {
    std::string error;
    const auto revision = import_revision(error);
    return revision && read_backup(char_ini_path_, *revision).has_value();
}

bool ActionsUi::restore_import(std::string& error) {
    if (current_social() >= 0) {
        error = "Wait for the running social to finish before restoring.";
        return false;
    }
    const auto revision = import_revision(error);
    if (!revision) return false;
    const auto backup = read_backup(char_ini_path_, *revision);
    if (!backup) {
        error = "There is no backup for the current settings. Changes made after import are protected.";
        return false;
    }
    const auto protected_socials = scripted_slots(lua_socials_dir());
    if (!protected_socials) {
        error = "The active character's scripted socials could not be checked.";
        return false;
    }
    IniFile ini;
    ini.parse(*backup);
    auto restored = std::make_unique<game::HotButtonStore>(store_);
    restored->load(ini);
    for (int index = 0; index < game::kSocialPages * game::kSocialButtons; ++index) {
        const auto& current = store_.social(index);
        auto& previous = restored->social(index);
        const bool unchanged = current.name == previous.name && current.color == previous.color &&
                               current.lines == previous.lines;
        if ((current.is_lua() || protected_socials->test(static_cast<std::size_t>(index))) &&
            social_ini_changed(ini, char_ini_, index)) {
            error = "A scripted social now uses a slot affected by this restore.";
            return false;
        }
        if (current.is_lua() || unchanged)
            previous = current;
    }
    const auto latest = import_revision(error);
    if (!latest || *latest != *revision) {
        if (latest) error = "The character settings changed while preparing the restore.";
        return false;
    }
    if (!write_file_atomic(char_ini_path_, *backup)) {
        error = "Hailstone could not restore the import backup.";
        return false;
    }
    char_ini_ = std::move(ini);
    store_ = std::move(*restored);
    return true;
}
} // namespace hailstone::app
