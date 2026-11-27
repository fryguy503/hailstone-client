#include "ui/asset_picker.h"

#include <mutex>
#include <system_error>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/asset_folder_choice.h"
#include "core/log.h"
#include "platform/open_folder_dialog.h"
#include "platform/window.h"
#include "render/vk_context.h"
#include "ui/imgui_layer.h"

namespace hailstone::ui {

namespace {

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize;

const ImVec4 kErrorColor{1.0f, 0.42f, 0.38f, 1.0f};
const ImVec4 kDimColor{0.62f, 0.64f, 0.68f, 1.0f};
const ImVec4 kGoodColor{0.45f, 0.85f, 0.45f, 1.0f};

// A heading and the files under it, as one wrapped line.
void files_line(const char* heading, const std::vector<std::string>& files, const ImVec4& color) {
    std::string text = heading;
    for (std::size_t i = 0; i < files.size(); ++i) text += (i ? ", " : " ") + files[i];
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

bool is_folder(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

} // namespace

// ---- FolderDialog -----------------------------------------------------------

struct FolderDialog::State {
    std::mutex mutex;
    bool open = false;
    std::optional<Result> result;
};

FolderDialog::FolderDialog() : state_(std::make_shared<State>()) {}

void FolderDialog::open(SDL_Window* window, const std::string& start) {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->open) return;
        state_->open = true;
        state_->result.reset();
    }
    // The callback owns a reference, so it may come after this dialog object is gone.
    auto* keep = new std::shared_ptr<State>(state_);
    auto callback = [](void* userdata, const char* const* files, int) {
        std::unique_ptr<std::shared_ptr<State>> owned(static_cast<std::shared_ptr<State>*>(userdata));
        State& s = **owned;
        Result r;
        if (!files) r.error = SDL_GetError();
        else if (files[0]) r.folder = files[0];
        std::lock_guard lock(s.mutex);
        s.open = false;
        s.result = std::move(r);
    };
    platform::show_open_folder_dialog(callback, keep, window, start.empty() ? nullptr : start.c_str());
}

bool FolderDialog::is_open() const {
    std::lock_guard lock(state_->mutex);
    return state_->open;
}

std::optional<FolderDialog::Result> FolderDialog::take() {
    std::lock_guard lock(state_->mutex);
    std::optional<Result> r = std::move(state_->result);
    state_->result.reset();
    return r;
}

// ---- The picker screen --------------------------------------------------------

void set_picker_folder(AssetPickerView& view, std::string folder) {
    view.folder = std::move(folder);
    view.error.clear();
    view.conflict.clear();
    if (view.folder.empty()) view.check.reset();
    else view.check = validate_asset_folder(view.folder);
    if (view.check && view.check->ok) view.conflict = data_folder_conflict(view.folder, view.data_folder);
}

AssetPickerAction asset_picker_screen(AssetPickerView& view, bool dialog_open) {
    AssetPickerAction action = AssetPickerAction::None;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    const float width = em * 36.0f;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, vp->Size.y * 0.9f));
    ImGui::Begin("Asset folder##hailstone", nullptr, kDialogFlags);

    ImGui::TextUnformatted("Hailstone");
    ImGui::SameLine();
    ImGui::TextColored(kDimColor, "asset folder");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Choose the folder containing your game files. Hailstone reads those files in place.");
    if (!view.reason.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", view.reason.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    // The folder: typed (Enter checks it) or chosen with the system's dialog.
    const char* browse = "Choose folder...";
    float browse_w = ImGui::CalcTextSize(browse).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
    std::string typed = view.folder;
    if (ImGui::InputTextWithHint("##folder", "Folder containing your game files", &typed,
                                 ImGuiInputTextFlags_EnterReturnsTrue))
        set_picker_folder(view, typed);
    else if (typed != view.folder) {
        view.folder = typed; // checked on Enter
        view.check.reset();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(dialog_open);
    if (ImGui::Button(browse)) action = AssetPickerAction::Browse;
    ImGui::EndDisabled();
    if (!view.error.empty()) ImGui::TextColored(kErrorColor, "The folder dialog failed: %s", view.error.c_str());

    // What the check found.
    if (view.check) {
        const AssetFolderCheck& c = *view.check;
        if (!c.is_directory) {
            ImGui::TextColored(kErrorColor, "This is not a folder.");
        } else if (c.ok && !view.conflict.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextWrapped("%s", view.conflict.c_str());
            ImGui::PopStyleColor();
        } else if (c.ok) {
            if (c.live) {
                ImGui::PushStyleColor(ImGuiCol_Text, kGoodColor);
                ImGui::TextWrapped("This folder contains current game files. Spells, game text and earlier versions "
                                   "of updated zones can come from your server's asset folder.");
                ImGui::PopStyleColor();
            } else {
                ImGui::TextColored(kGoodColor, "This folder contains compatible game files.");
            }
        } else {
            if (!c.missing.empty())
                files_line("Missing game files:", c.missing, kErrorColor);
            if (!c.missing_rof2.empty())
                files_line("Missing files required for compatibility:", c.missing_rof2, kErrorColor);
        }
        if (c.is_directory && !c.warnings.empty())
            files_line("Not found, but not needed to play:", c.warnings, kDimColor);
    } else if (!view.folder.empty()) {
        ImGui::TextColored(kDimColor, "Press Enter to check this folder.");
    }

    // The folders chosen before.
    if (!view.recent.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText("Recent folders");
        std::optional<std::size_t> pick, remove;
        float remove_w = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        for (std::size_t i = 0; i < view.recent.size(); ++i) {
            const std::string& r = view.recent[i];
            ImGui::PushID(static_cast<int>(i));
            bool here = same_folder(r, view.folder);
            bool gone = !is_folder(r);
            if (gone) ImGui::PushStyleColor(ImGuiCol_Text, kDimColor);
            if (ImGui::Selectable(r.c_str(), here, 0,
                                  ImVec2(ImGui::GetContentRegionAvail().x - remove_w - ImGui::GetStyle().ItemSpacing.x, 0.0f)))
                pick = i;
            if (gone) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("This folder is not there any more.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) remove = i;
            ImGui::PopID();
        }
        if (pick) set_picker_folder(view, view.recent[*pick]);
        else if (remove) view.recent = remove_recent_folder(view.recent, view.recent[*remove]);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    bool ready = view.check && view.check->ok && view.conflict.empty() && !dialog_open;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Continue", ImVec2(em * 7.0f, 0.0f))) action = AssetPickerAction::Continue;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(view.cancel ? "Cancel" : "Quit")) action = AssetPickerAction::Quit;

    ImGui::End();
    return action;
}

std::optional<std::filesystem::path> run_asset_picker(platform::Window& window, render::VkContext& vk,
                                                      ImGuiLayer& imgui, AssetPickerView& view, int max_frames,
                                                      const std::string& screenshot) {
    FolderDialog dialog;
    view.window_closed = false;
    for (int frame = 0;; ++frame) {
        if (!window.poll_events() || window.take_close_request()) { // a held close too (the client's, at login)
            view.window_closed = true;
            break;
        }
        if (max_frames > 0 && frame >= max_frames) break;
        if (!screenshot.empty() && frame == max_frames - 1) vk.request_screenshot(screenshot);
        if (auto r = dialog.take()) {
            if (r->folder) set_picker_folder(view, *r->folder);
            else if (!r->error.empty()) {
                view.error = r->error;
                log::warn("folder dialog: {}", r->error);
            }
        }
        imgui.new_frame();
        switch (asset_picker_screen(view, dialog.is_open())) {
        case AssetPickerAction::Browse: {
            std::string start = is_folder(view.folder) ? view.folder : std::string();
            dialog.open(window.sdl_window(), start);
            break;
        }
        case AssetPickerAction::Continue:
            imgui.end_frame_without_render();
            return std::filesystem::path(view.folder);
        case AssetPickerAction::Quit: imgui.end_frame_without_render(); return std::nullopt;
        case AssetPickerAction::None: break;
        }
        auto f = vk.begin_frame(glm::vec4(0.05f, 0.06f, 0.08f, 1.0f));
        if (!f) {
            imgui.end_frame_without_render();
            continue;
        }
        imgui.render(*f);
        vk.end_frame(*f);
    }
    return std::nullopt;
}

} // namespace hailstone::ui
