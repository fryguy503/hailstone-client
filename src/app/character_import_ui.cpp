#include "app/character_import_ui.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include <imgui.h>

namespace hailstone::app {
namespace {

void literal(std::string_view text) {
    if (text.empty()) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

const char* outcome_label(CharacterImportUi::Outcome outcome) {
    switch (outcome) {
    case CharacterImportUi::Outcome::Add: return "Add";
    case CharacterImportUi::Outcome::Keep: return "Keep";
    case CharacterImportUi::Outcome::Skip: return "Skip";
    }
    return "Skip";
}

} // namespace

void CharacterImportUi::open(std::string target, std::vector<Source> sources, std::string scan_error,
                             bool can_restore) {
    close();
    target_ = std::move(target);
    sources_ = std::move(sources);
    scan_error_ = std::move(scan_error);
    can_restore_ = can_restore;
    show(Page::Source);
    open_ = true;
}

void CharacterImportUi::close() {
    open_ = false;
    can_restore_ = false;
    target_.clear();
    sources_.clear();
    scan_error_.clear();
    preview_error_.clear();
    choice_ = {};
    preview_ = {};
    result_ = {};
    additions_ = matches_ = 0;
}

const CharacterImportUi::Source* CharacterImportUi::source() const {
    return choice_.source < sources_.size() ? &sources_[choice_.source] : nullptr;
}

void CharacterImportUi::show(Page page) {
    if (page == Page::Source) preview_ = {};
    page_ = page;
    reset_scroll_ = true;
}

void CharacterImportUi::draw_source() {
    if (!scan_error_.empty()) literal(scan_error_);
    const Source* selected = source();
    if (!selected) {
        if (scan_error_.empty()) literal("No character settings files were found in the configured game data folder.");
        return;
    }

    ImGui::TextUnformatted("Source character");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##source", selected->label.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool active = i == choice_.source;
            const std::string item = sources_[i].label + "###character";
            if (ImGui::Selectable(item.c_str(), active) && !active) {
                choice_ = {i, false, false};
                preview_error_.clear();
            }
            if (active) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    selected = source();
    ImGui::SeparatorText("Detected INI files");
    if (ImGui::BeginTable("##import-files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (const auto& file : selected->files) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            literal(file.name);
            ImGui::TableNextColumn();
            literal(file.label);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Settings to import");
    ImGui::BeginDisabled(!selected->has_actions);
    ImGui::Checkbox("Socials", &choice_.socials);
    ImGui::Checkbox("Hotbar buttons", &choice_.hotbars);
    ImGui::EndDisabled();
    if (!selected->has_actions) literal("This source has no importable character settings file.");
    literal("Keyboard bindings come from shared settings.");
    if (!preview_error_.empty()) {
        ImGui::Spacing();
        literal(preview_error_);
    }
}

void CharacterImportUi::draw_review() const {
    const Source* selected = source();
    if (selected) literal("From " + selected->label);
    ImGui::Text("%zu to add, %zu skipped, %zu already present", additions_, preview_.rows.size() - additions_ - matches_,
                matches_);
    if (!additions_) literal("There are no changes to apply.");
    literal("Hailstone fills empty slots and saves a backup. Imported socials are saved for manual use.");

    ImGui::SeparatorText("Planned changes");
    if (ImGui::BeginTable("##import-preview", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.95f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.55f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableHeadersRow();
        for (const auto& row : preview_.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            literal(row.slot);
            ImGui::TableNextColumn();
            literal(row.name);
            for (const auto& line : row.lines) literal(line);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(outcome_label(row.outcome));
            literal(row.reason);
        }
        ImGui::EndTable();
    }
}

void CharacterImportUi::show_result(Result result) {
    can_restore_ = result.can_restore;
    result_ = std::move(result);
    preview_ = {};
    show(Page::Result);
}

void CharacterImportUi::draw_footer(const Callbacks& callbacks) {
    ImGui::Separator();
    switch (page_) {
    case Page::Source: {
        const Source* selected = source();
        const bool can_preview = callbacks.preview && selected && selected->has_actions &&
                                 (choice_.socials || choice_.hotbars);
        ImGui::BeginDisabled(!can_preview);
        if (ImGui::Button("Preview import") && can_preview) {
            preview_ = callbacks.preview(choice_);
            preview_error_ = preview_.error;
            if (preview_error_.empty()) {
                additions_ = matches_ = 0;
                for (const auto& row : preview_.rows) {
                    if (row.outcome == Outcome::Add) ++additions_;
                    if (row.outcome == Outcome::Keep) ++matches_;
                }
                show(Page::Review);
            }
        }
        ImGui::EndDisabled();
        break;
    }
    case Page::Review: {
        if (ImGui::Button("Back")) show(Page::Source);
        ImGui::SameLine();
        const bool can_apply = additions_ && callbacks.apply;
        ImGui::BeginDisabled(!can_apply);
        if (ImGui::Button("Apply import") && can_apply) show_result(callbacks.apply());
        ImGui::EndDisabled();
        return;
    }
    case Page::Result:
        if (ImGui::Button("Back to sources")) show(Page::Source);
        break;
    case Page::Restore: {
        if (ImGui::Button("Cancel")) show(Page::Source);
        ImGui::SameLine();
        const bool can_restore = can_restore_ && callbacks.restore;
        ImGui::BeginDisabled(!can_restore);
        if (ImGui::Button("Confirm restore") && can_restore) show_result(callbacks.restore());
        ImGui::EndDisabled();
        return;
    }
    }
    if (can_restore_ && callbacks.restore) {
        ImGui::SameLine();
        if (ImGui::Button("Restore backup")) show(Page::Restore);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) open_ = false;
}

void CharacterImportUi::draw(const Callbacks& callbacks) {
    if (!open_) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 47.0f, viewport->WorkSize.x),
                                    std::min(em * 32.0f, viewport->WorkSize.y)), ImGuiCond_Appearing);
    if (ImGui::Begin(kWindowName, &open_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        literal("Import into " + target_);
        ImGui::TextDisabled("Source files are read-only.");
        ImGui::Separator();
        const float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        if (reset_scroll_) {
            ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
            reset_scroll_ = false;
        }
        if (ImGui::BeginChild("##import-content", ImVec2(0.0f, -footer_height))) {
            switch (page_) {
            case Page::Source: draw_source(); break;
            case Page::Review: draw_review(); break;
            case Page::Result:
                ImGui::SeparatorText(result_.ok ? "Changes saved" : "Unable to save changes");
                literal(result_.message);
                break;
            case Page::Restore:
                ImGui::SeparatorText("Restore backup?");
                literal("Restore the settings saved immediately before the last import for this character.");
                break;
            }
        }
        ImGui::EndChild();
        draw_footer(callbacks);
    }
    ImGui::End();
    if (!open_) close();
}

} // namespace hailstone::app
