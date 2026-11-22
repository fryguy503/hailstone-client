#include "app/client_app_internal.h"

#include <format>

#include "core/asset_folder_choice.h"
#include "core/path_utf8.h"

namespace hailstone::app::client_internal {
namespace {

std::string file_label(CharacterImportFileKind kind) {
    switch (kind) {
        case CharacterImportFileKind::Settings: return "Character settings";
        case CharacterImportFileKind::Layout: return "Layout";
        case CharacterImportFileKind::Trader: return "Trader settings";
        case CharacterImportFileKind::Barter: return "Barter settings";
        case CharacterImportFileKind::HotbarSet: return "Saved hotbar set";
    }
    return {};
}

bool import_destination_allowed(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code ec;
    const auto data = std::filesystem::weakly_canonical(platform::data_dir(), ec);
    if (ec || data.empty() || !data_folder_conflict(source, data).empty()) return false;
    const auto character = std::filesystem::weakly_canonical(target.parent_path(), ec);
    if (ec) return false;
    const auto source_dir = source.empty() ? source : std::filesystem::weakly_canonical(source, ec);
    if (ec) return false;
    for (auto dir = source_dir; !dir.empty(); dir = dir.parent_path()) {
        if (same_folder(dir, character)) return false;
        if (dir == dir.parent_path()) break;
    }
    for (auto dir = character; !dir.empty(); dir = dir.parent_path()) {
        if (same_folder(dir, source_dir)) return false;
        if (same_folder(dir, data)) return true;
        if (dir == dir.parent_path()) break;
    }
    return false;
}

} // namespace

bool ClientApp::character_import_target_active() const {
    return conn_ && conn_->state() == net::ConnectionState::InZone && !conn_->interface_locked() && player_ready_ &&
           actions_ui_ && !character_import_target_.empty() && actions_ui_->char_ini_path() == character_import_target_ &&
           game_.base_root() == character_import_root_;
}

void ClientApp::open_character_import() {
    if (character_import_ui_.visible()) return;
    if (!actions_ui_ || !player_ready_ || conn_->state() != net::ConnectionState::InZone ||
        actions_ui_->char_ini_path().empty()) {
        conn_->show_chat("Enter a character before using /import.", 15);
        return;
    }
    character_import_target_ = actions_ui_->char_ini_path();
    character_import_root_ = game_.base_root();
    if (!import_destination_allowed(character_import_root_, character_import_target_)) {
        conn_->show_chat("Choose a game data folder outside the active character's settings path before importing.", 15);
        close_character_import();
        return;
    }
    auto inventory = discover_character_import_sources(character_import_root_);
    character_import_sources_ = std::move(inventory.sources);
    std::vector<CharacterImportUi::Source> sources;
    sources.reserve(character_import_sources_.size());
    for (const auto& source : character_import_sources_) {
        CharacterImportUi::Source view;
        view.label = std::format("{}  /  {}", source.character, source.world);
        for (const auto& file : source.files) {
            view.files.push_back({file.name, file_label(file.kind)});
            view.has_actions = view.has_actions || file.kind == CharacterImportFileKind::Settings;
        }
        sources.push_back(std::move(view));
    }
    const auto character_dir = character_import_target_.parent_path();
    const auto* player = conn_->world().player();
    auto target = std::format("{}  /  {}", player ? game::display_name(player->name) : path_utf8(character_dir.filename()),
                              char_view_.server_name);
    character_import_ui_.open(std::move(target), std::move(sources), std::move(inventory.error),
                              actions_ui_->has_import_backup());
    if (gamepad_ && gamepad_->active() && (ui_name_ == "gamepad" || gamepad_->mode() == PadMode::Menu))
        gamepad_->request_focus(CharacterImportUi::kWindowName);
}

void ClientApp::close_character_import() {
    character_import_ui_.close();
    character_import_sources_.clear();
    pending_character_import_.reset();
    character_import_root_.clear();
    character_import_target_.clear();
}

void ClientApp::draw_character_import() {
    if (!character_import_ui_.visible()) return;
    if (!character_import_target_active()) {
        close_character_import();
        return;
    }
    character_import_ui_.draw({
        [this](const CharacterImportUi::Choice& choice) { return preview_character_import(choice); },
        [this] { return apply_character_import(); },
        [this] { return restore_character_import(); },
    });
    if (!character_import_ui_.visible()) close_character_import();
}

CharacterImportUi::Preview ClientApp::preview_character_import(const CharacterImportUi::Choice& choice) {
    pending_character_import_.reset();
    CharacterImportUi::Preview view;
    if (!character_import_target_active() || choice.source >= character_import_sources_.size()) {
        view.error = "The source or active character has changed. Reopen /import.";
        return view;
    }
    auto bytes = read_character_import_source(character_import_root_, character_import_sources_[choice.source], view.error);
    if (!bytes) return view;
    const game::CharacterImportOptions options{.socials = choice.socials, .hot_buttons = choice.hotbars};
    auto preview = actions_ui_->preview_import(*bytes, options);
    if (!preview.plan.error.empty()) {
        view.error = std::move(preview.plan.error);
        return view;
    }
    for (auto& row : preview.plan.rows) {
        std::string slot;
        if (row.area == game::CharacterImportArea::Social) slot = std::format("Social page {}, slot {}", row.page, row.slot);
        else slot = std::format("Bar {}, page {}, slot {}", row.bar, row.page, row.slot);
        auto outcome = CharacterImportUi::Outcome::Skip;
        if (row.disposition == game::CharacterImportDisposition::Add) outcome = CharacterImportUi::Outcome::Add;
        if (row.disposition == game::CharacterImportDisposition::Keep) outcome = CharacterImportUi::Outcome::Keep;
        view.rows.push_back({std::move(slot), std::move(row.name), std::move(row.reason), std::move(row.lines), outcome});
    }
    preview.plan.rows.clear();
    pending_character_import_ = PendingCharacterImport{std::move(preview), options, choice.source, std::move(*bytes)};
    return view;
}

CharacterImportUi::Result ClientApp::apply_character_import() {
    auto pending = std::exchange(pending_character_import_, std::nullopt);
    if (!pending || !character_import_target_active()) return {false, "The preview has expired. Reopen /import.", false};
    CharacterImportUi::Result result;
    auto bytes = read_character_import_source(character_import_root_, character_import_sources_[pending->source],
                                              result.message);
    if (bytes && *bytes == pending->bytes && import_destination_allowed(character_import_root_, character_import_target_)) {
        result.ok = actions_ui_->apply_import(pending->preview, *bytes, pending->options, result.message);
        if (result.ok) result.message = std::format("Added {} settings to the active character.", pending->preview.plan.additions);
    } else if (bytes) {
        result.message = "The source changed after preview. Review the import again.";
    }
    result.can_restore = actions_ui_->has_import_backup();
    return result;
}

CharacterImportUi::Result ClientApp::restore_character_import() {
    pending_character_import_.reset();
    if (!character_import_target_active() || !import_destination_allowed(character_import_root_, character_import_target_))
        return {false, "The active character has changed. Reopen /import.", false};
    CharacterImportUi::Result result;
    result.ok = actions_ui_->restore_import(result.message);
    if (result.ok) result.message = "Restored the active character's previous settings.";
    result.can_restore = actions_ui_->has_import_backup();
    return result;
}

} // namespace hailstone::app::client_internal
