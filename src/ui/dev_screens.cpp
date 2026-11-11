#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <ctime>
#include <format>
#include <numbers>

#include <imgui.h>
#include <imgui_internal.h> // ImHashStr, IsKeyPressed with an owner (the pad's X)
#include <imgui_internal.h> // the activating input (the pad opens the on-screen keyboard), ClearActiveID
#include <imgui_stdlib.h>

#include "core/ascii_case.h"
#include "core/config.h"
#include "core/distribution.h"
#include "core/wipe.h"
#include "game/eq_names.h"
#include "game/zone_names.h"
#include "game/gamepad_bindings.h"

namespace hailstone::ui {

namespace {

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

const ImVec4 kErrorColor{1.0f, 0.42f, 0.38f, 1.0f};
const ImVec4 kDimColor{0.62f, 0.64f, 0.68f, 1.0f};
const ImVec4 kGoodColor{0.45f, 0.85f, 0.45f, 1.0f};

// Places the next window centred on the screen with a size in font-height units.
void center_next_window(float width_em, float height_em = 0.0f) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width_em * em, height_em * em), ImGuiCond_Always);
}

// A pre-game dialog over the character select scene: at the left, so the model in the middle shows.
void left_next_window(float width_em, float height_em = 0.0f) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + em, vp->GetCenter().y), ImGuiCond_Always, ImVec2(0.0f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width_em * em, height_em * em), ImGuiCond_Always);
}

// Rotating arc, drawn inline at the cursor.
void spinner(float radius) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float size = radius * 2.0f;
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(pos.x + radius, pos.y + radius);
    float t = static_cast<float>(ImGui::GetTime()) * 6.0f;
    constexpr float kPi = std::numbers::pi_v<float>;
    dl->PathArcTo(c, radius * 0.8f, t, t + kPi * 1.5f, 24);
    dl->PathStroke(ImGui::GetColorU32(ImGuiCol_ButtonActive), 0, radius * 0.3f);
}

void status_line(const StatusLine& status, bool busy) {
    if (busy) {
        spinner(ImGui::GetFontSize() * 0.5f);
        ImGui::SameLine();
    }
    if (status.text.empty()) {
        if (!busy) ImGui::TextUnformatted(" ");
        else ImGui::TextUnformatted("Working...");
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, status.is_error ? kErrorColor : kDimColor);
    ImGui::TextWrapped("%s", status.text.c_str());
    ImGui::PopStyleColor();
}

// Width of a row of buttons, for right-aligning them.
float buttons_width(std::initializer_list<const char*> labels) {
    const ImGuiStyle& s = ImGui::GetStyle();
    float w = 0.0f;
    for (const char* l : labels) w += ImGui::CalcTextSize(l, nullptr, true).x + s.FramePadding.x * 2.0f;
    return w + s.ItemSpacing.x * static_cast<float>(labels.size() - 1);
}

bool enter_pressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
}

ImVec4 channel_color(ChatChannel c) {
    switch (c) {
    case ChatChannel::System: return {1.00f, 1.00f, 0.40f, 1.0f};
    case ChatChannel::Say: return {0.92f, 0.92f, 0.92f, 1.0f};
    case ChatChannel::Tell: return {0.85f, 0.45f, 0.95f, 1.0f};
    case ChatChannel::Group: return {0.45f, 0.85f, 1.00f, 1.0f};
    case ChatChannel::Guild: return {0.40f, 0.95f, 0.40f, 1.0f};
    case ChatChannel::Raid: return {1.00f, 0.60f, 0.20f, 1.0f};
    case ChatChannel::Shout: return {1.00f, 0.35f, 0.35f, 1.0f};
    case ChatChannel::Auction: return {0.30f, 0.80f, 0.55f, 1.0f};
    case ChatChannel::OOC: return {0.55f, 0.90f, 0.70f, 1.0f};
    case ChatChannel::Emote: return {0.70f, 0.70f, 0.70f, 1.0f};
    case ChatChannel::Combat: return {0.95f, 0.75f, 0.60f, 1.0f};
    case ChatChannel::Spell: return {0.55f, 0.65f, 1.00f, 1.0f};
    case ChatChannel::Error: return {1.00f, 0.30f, 0.30f, 1.0f};
    case ChatChannel::Count: break;
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

ImVec4 rgb_color(std::uint32_t rgb) {
    return {static_cast<float>((rgb >> 16) & 0xff) / 255.0f, static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
            static_cast<float>(rgb & 0xff) / 255.0f, 1.0f};
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

// ---- The gamepad on the pre-game screens ----------------------------------

struct ScreenPad {
    bool connected = false;
    platform::PadType type = platform::PadType::Generic;
    bool popup_open = false; // a popup was open at the end of the last screen (B closed it, it does not go back)
};
ScreenPad g_pad;

// ImGui's gamepad navigation is on (the pad in game or menu mode outside the world: ClientApp::update_gamepad).
bool pad_nav() {
    const ImGuiIO& io = ImGui::GetIO();
    return g_pad.connected && (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) &&
           (io.BackendFlags & ImGuiBackendFlags_HasGamepad);
}

// The last item was activated with the pad's A (Y, ImGui's "input" button, activates it too, and is left out).
bool pad_activated() {
    const ImGuiContext& g = *GImGui;
    return pad_nav() && g.NavActivateId != 0 && g.NavActivateId == g.LastItemData.ID &&
           g.NavInputSource == ImGuiInputSource_Gamepad && ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
}

// The on-screen keyboard: one field at a time, edited in a copy and written back when accepted.
struct Keyboard {
    std::string* text_target = nullptr; // the field's text (a view's member, alive across frames)
    int* int_target = nullptr;          // or its number
    std::string text, title;
    bool password = false, no_blank = false, digits = false, shift = false;
    bool open_popup = false; // OpenPopup at the screen's keyboard_popup (the screen window's ID stack)
    bool open() const { return text_target || int_target; }
};
Keyboard g_keyboard;
constexpr const char* kKeyboardPopup = "Keyboard##hailstone_osk";
constexpr std::size_t kKeyboardMax = 64;

void open_keyboard(std::string* text, int* number, const char* title, ImGuiInputTextFlags flags) {
    Keyboard k;
    k.text_target = text;
    k.int_target = number;
    k.text = text ? *text : number ? std::to_string(*number) : std::string();
    k.title = title;
    k.password = flags & ImGuiInputTextFlags_Password;
    k.no_blank = flags & ImGuiInputTextFlags_CharsNoBlank;
    k.digits = number != nullptr;
    k.open_popup = true;
    g_keyboard = std::move(k);
}

// A text field the pad can fill: activated with the pad, it opens the on-screen keyboard instead of typing.
bool text_field(const char* id, std::string& value, ImGuiInputTextFlags flags, const char* title,
                const char* hint = nullptr) {
    bool done = hint ? ImGui::InputTextWithHint(id, hint, &value, flags) : ImGui::InputText(id, &value, flags);
    if (ImGui::IsItemActivated() && GImGui->ActiveIdSource == ImGuiInputSource_Gamepad) {
        ImGui::ClearActiveID();
        open_keyboard(&value, nullptr, title, flags);
        return false;
    }
    return done;
}

// A button's label as the connected pad has it, in a small box.
void glyph_box(platform::PadButton button) {
    const char* label = game::pad_glyph(g_pad.type, button);
    float em = ImGui::GetFontSize();
    ImVec2 text = ImGui::CalcTextSize(label);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(text.x + em * 0.6f, ImGui::GetTextLineHeight());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImGuiCol_Button), em * 0.25f);
    dl->AddText(ImVec2(pos.x + em * 0.3f, pos.y), ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::Dummy(size);
}

// The screen's pad buttons, on one line (only while the pad drives the screens).
void pad_hints(std::initializer_list<std::pair<platform::PadButton, const char*>> hints) {
    if (!pad_nav()) return;
    float em = ImGui::GetFontSize();
    bool first = true;
    for (const auto& [button, text] : hints) {
        if (!first) ImGui::SameLine(0.0f, em * 0.9f);
        first = false;
        glyph_box(button);
        ImGui::SameLine(0.0f, em * 0.3f);
        ImGui::TextColored(kDimColor, "%s", text);
    }
}

// A pad button pressed for the screen itself: not while the keyboard or a popup has the pad (B closing a popup
// closes it this frame, so the popup of the last frame counts).
bool pad_screen_pressed(ImGuiKey key) {
    return pad_nav() && !g_keyboard.open() && !g_pad.popup_open &&
           !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
           !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(key, false);
}

// The on-screen keyboard, a modal over the screen whose field opened it: A types the key under the cursor, X
// deletes, Y types a space, LB is shift, Start (or Done) accepts and B cancels. The real keyboard's Enter and
// Escape work too. Call at the end of each screen window, before End().
void keyboard_popup() {
    if (g_keyboard.open_popup) {
        ImGui::OpenPopup(kKeyboardPopup);
        g_keyboard.open_popup = false;
    }
    g_pad.popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!g_keyboard.open()) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kKeyboardPopup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        g_keyboard = {}; // closed some other way
        return;
    }
    Keyboard& k = g_keyboard;
    const float em = ImGui::GetFontSize();
    const ImVec2 key(em * 2.2f, em * 2.0f);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width = key.x * 10.0f + gap * 9.0f;
    auto type = [&](char c) {
        if (k.text.size() < kKeyboardMax) k.text.push_back(c);
    };
    bool accept = false, cancel = false;

    ImGui::TextUnformatted(k.title.c_str());
    {
        // The text so far, with a caret.
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 size(width, ImGui::GetFrameHeight());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg),
                          ImGui::GetStyle().FrameRounding);
        std::string shown = (k.password ? std::string(k.text.size(), '*') : k.text) + "_";
        const ImVec2 fp = ImGui::GetStyle().FramePadding;
        dl->AddText(ImVec2(pos.x + fp.x, pos.y + fp.y), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        ImGui::Dummy(size);
    }
    ImGui::Spacing();

    // Y is also ImGui's "input" button, which activates the key under the cursor: that press types the space only.
    const bool y_down = ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp);
    static constexpr const char* kRows[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@"};
    static constexpr const char* kShifted[] = {"!#$%&*()+=", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM,?/"};
    for (int row = 0; row < 4; ++row) {
        const char* keys = k.shift ? kShifted[row] : kRows[row];
        ImGui::BeginDisabled(k.digits && row > 0);
        for (int i = 0; keys[i]; ++i) {
            if (i) ImGui::SameLine();
            const char c = keys[i];
            const bool usable = !k.digits || (c >= '0' && c <= '9');
            ImGui::BeginDisabled(!usable);
            const char label[2] = {c, '\0'};
            ImGui::PushID(row * 16 + i);
            if (ImGui::Button(label, key) && !y_down) type(c);
            if (row == 1 && i == 0 && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
    }
    const float wide = (width - gap * 4.0f) / 5.0f;
    if (ImGui::Button(k.shift ? "SHIFT" : "Shift", ImVec2(wide, key.y)) && !y_down) k.shift = !k.shift;
    ImGui::SameLine();
    ImGui::BeginDisabled(k.no_blank || k.digits);
    if (ImGui::Button("Space", ImVec2(wide, key.y)) && !y_down) type(' ');
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(wide, key.y)) && !y_down && !k.text.empty()) k.text.pop_back();
    ImGui::SameLine();
    if (ImGui::Button("Done", ImVec2(wide, key.y)) && !y_down) accept = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(wide, key.y)) && !y_down) cancel = true;

    if (pad_nav()) {
        // (X is locked to its owner, ImGuiLayer::new_frame, away from ImGui's window switcher)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, ImGuiInputFlags_None, ImHashStr("##PadX")) && !k.text.empty())
            k.text.pop_back();
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) && !k.no_blank && !k.digits) type(' ');
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) k.shift = !k.shift;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) accept = true;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) cancel = true;
        ImGui::Spacing();
        pad_hints({{platform::PadButton::A, "Type"},
                   {platform::PadButton::X, "Delete"},
                   {platform::PadButton::Y, "Space"},
                   {platform::PadButton::LB, "Shift"},
                   {platform::PadButton::Start, "Done"},
                   {platform::PadButton::B, "Cancel"}});
    }
    if (enter_pressed()) accept = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;

    if (accept) {
        if (k.text_target) *k.text_target = k.text;
        if (k.int_target && !k.text.empty()) {
            int value = 0;
            auto [end, ec] = std::from_chars(k.text.data(), k.text.data() + k.text.size(), value);
            if (ec == std::errc()) *k.int_target = value;
        }
    }
    if (accept || cancel) {
        g_keyboard = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void set_screen_gamepad(bool connected, platform::PadType type) {
    g_pad.connected = connected;
    g_pad.type = type;
}

// ---- Login ----------------------------------------------------------------

std::string update_notice_text(const UpdateNotice& n) {
    std::string text;
    if (n.state == "available") {
        text = std::format("Hailstone {} is out (this is {}).", n.version, n.current);
        if (!n.has_package) text += " Download it from its release page.";
    } else if (n.state == "downloading") {
        text = n.progress >= 0.0f
                   ? std::format("Downloading Hailstone {}: {}%", n.version, static_cast<int>(n.progress * 100.0f))
                   : std::format("Downloading Hailstone {}...", n.version);
    } else if (n.state == "ready") {
        text = n.install_on_quit ? std::format("Hailstone {} is downloaded: it is installed when you quit.", n.version)
                                 : std::format("Hailstone {} is downloaded.", n.version);
    } else if (n.state == "failed") {
        text = std::format("The update to Hailstone {} failed: {}", n.version, n.error);
    }
    if (!n.message.empty()) text = text.empty() ? n.message : n.message + "\n" + text;
    return text;
}

std::string update_status_text(const UpdateNotice& n) {
    if (n.check == "checking") return "Checking for updates...";
    if (n.check == "uptodate") return "Up to date";
    if (n.check == "unknown") return "Could not check for updates";
    if (n.check == "newer") {
        if (n.state == "downloading") return std::format("Downloading {}...", n.version);
        if (n.state == "ready") return std::format("{} is ready: restart to update", n.version);
        if (n.state == "failed") return std::format("The update to {} failed", n.version);
        return std::format("Version {} is out", n.version);
    }
    return n.off_reason.empty() ? std::string("Updates are off") : "Updates are off: " + n.off_reason;
}

namespace {

// The updater's notice under the login card's buttons, with its buttons.
LoginAction update_notice(const UpdateNotice& n) {
    LoginAction action = LoginAction::None;
    const std::string text = update_notice_text(n);
    if (text.empty()) return action;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("%s", text.c_str());
    bool first = true;
    auto button = [&](const char* label, LoginAction a) {
        if (!first) ImGui::SameLine();
        first = false;
        if (ImGui::SmallButton(label)) action = a;
    };
    if (n.state == "available" && n.has_package) button("Download", LoginAction::UpdateDownload);
    if (n.state == "failed") button("Try again", LoginAction::UpdateDownload);
    if (n.state == "ready" && n.can_restart) button("Restart now", LoginAction::UpdateRestart);
    if (!n.notes.empty() && !n.state.empty()) button("Release notes", LoginAction::UpdateNotes);
    if (n.state == "available") button("Skip this version", LoginAction::UpdateSkip);
    return action;
}

// The picked server's suggested add-ons (docs/addons.md "Suggested add-ons"), each with Look at it, Not now and
// Don't ask again; `choice` gets the one clicked.
LoginAction addon_suggestions(const AddonsView& a, std::string& choice) {
    LoginAction action = LoginAction::None;
    for (const auto& sg : a.suggestions) {
        ImGui::PushID(sg.source.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextWrapped("%s", sg.text.c_str());
        auto button = [&](const char* label, LoginAction act, const char* tip) {
            if (ImGui::SmallButton(label)) {
                action = act;
                choice = sg.source;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        button("Look at it", LoginAction::AddonSuggestLook, "What it is and what it asks to do; nothing is installed yet");
        ImGui::SameLine();
        button("Not now", LoginAction::AddonSuggestLater, "Not suggested again until Hailstone starts again");
        ImGui::SameLine();
        button("Don't ask again", LoginAction::AddonSuggestNever, "Not suggested again for this server");
        ImGui::PopID();
    }
    return action;
}

// The add-on manager's window (docs/addons.md): a GitHub link, the install card of what it names, and every script
// and plugin with what it can do. The server's own are listed with no button that removes or stops them.
LoginAction addons_window(AddonsView& a, bool busy, bool* open) {
    LoginAction action = LoginAction::None;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 34.0f, em * 30.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Add-ons##hailstone", open)) {
        ImGui::End();
        return action;
    }
    auto choose = [&](const std::string& name, LoginAction act) {
        a.choice = name;
        action = act;
    };
    auto list = [](const std::vector<std::string>& lines) {
        for (const auto& l : lines) ImGui::BulletText("%s", l.c_str());
    };
    if (!a.status.text.empty()) {
        if (a.status.is_error) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", a.status.text.c_str());
        else ImGui::TextWrapped("%s", a.status.text.c_str());
    }

    if (a.offer) {
        const AddonOfferView& o = *a.offer;
        ImGui::Separator();
        ImGui::Text("%s %s (%s)", o.name.c_str(), o.version.c_str(), o.kind.c_str());
        if (!o.author.empty()) ImGui::TextDisabled("by %s, from %s", o.author.c_str(), o.source.c_str());
        else ImGui::TextDisabled("from %s", o.source.c_str());
        if (!o.description.empty()) ImGui::TextWrapped("%s", o.description.c_str());
        if (!o.installed_version.empty()) ImGui::Text("Installed now: %s", o.installed_version.c_str());
        if (o.kind != "tool") {
            ImGui::TextUnformatted("It asks to:");
            list(o.caps);
        }
        ImGui::TextWrapped("%s", o.key.empty() ? "Not signed: later versions are taken from whoever controls the "
                                                 "repository."
                                               : ("Signed with key " + o.key + ": later versions must carry it.").c_str());
        if (o.kind == "plugin")
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                               "A plugin is a program: the permissions limit what it does through Hailstone, not what "
                               "it can do on your computer. Install only plugins you trust.");
        else if (o.kind == "tool")
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                               "A tool is a program of its own: it runs on your computer with your rights, outside "
                               "Hailstone, which starts it when you ask. Install only tools you trust.");
        if (o.older)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "This is an older version than the one installed (%s): "
                               "installing it goes back to %s.", o.installed_version.c_str(), o.version.c_str());
        if (!o.blocked.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", o.blocked.c_str());
        ImGui::BeginDisabled(!o.blocked.empty());
        if (ImGui::Button(o.older ? "Install the older version" : o.installed_version.empty() ? "Install"
                                                                                               : "Install this version")) {
            action = LoginAction::AddonInstall;
            a.choice_on = o.older;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) action = LoginAction::AddonCancel;
        ImGui::SameLine();
        if (ImGui::Button("Repository")) choose("", LoginAction::AddonPage);
    }

    // The store (docs/addons.md "Finding add-ons"): every add-on of the add-on index; Install opens its card.
    if (ImGui::BeginTabBar("##addontabs")) {
        if (ImGui::BeginTabItem("Find add-ons")) {
            if (a.available && !a.browsed && !a.browsing) action = LoginAction::AddonBrowse;
            if (a.browsing) {
                ImGui::TextDisabled("Finding add-ons...");
            } else if (a.store.empty()) {
                ImGui::TextWrapped("%s", !a.available           ? a.why_off.c_str()
                                         : !a.store_error.empty() ? a.store_error.c_str()
                                                                  : "No add-ons are listed yet.");
            }
            for (const auto& it : a.store) {
                ImGui::PushID(it.source.c_str());
                ImGui::SeparatorText(it.name.c_str());
                if (!it.version.empty())
                    ImGui::TextDisabled("%s %s%s%s", it.kind.c_str(), it.version.c_str(), it.author.empty() ? "" : ", by ",
                                        it.author.c_str());
                if (!it.worlds.empty()) ImGui::TextWrapped("For %s", it.worlds.c_str());
                if (!it.description.empty()) ImGui::TextWrapped("%s", it.description.c_str());
                if (it.state == "install" || it.state == "update") {
                    ImGui::BeginDisabled(a.inspecting);
                    const std::string label = it.state == "install" ? std::string("Install") : "Update to " + it.version;
                    if (ImGui::Button(label.c_str())) choose(it.source, LoginAction::AddonStoreLook);
                    ImGui::EndDisabled();
                } else if (it.state == "installed") {
                    ImGui::TextDisabled("Installed %s", it.installed_version.c_str());
                } else if (it.state == "manual") {
                    ImGui::TextDisabled("One of this name is in your folders already.");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", it.problem.c_str());
                }
                ImGui::PopID();
            }
            if (a.available && a.browsed) {
                ImGui::Spacing();
                if (ImGui::SmallButton("Refresh the list")) {
                    a.choice_on = true;
                    action = LoginAction::AddonBrowse;
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("My add-ons")) {
            ImGui::EndTabItem();
        } else {
            ImGui::EndTabBar();
            ImGui::End();
            return action;
        }
        ImGui::EndTabBar();
    }
    if (a.available) {
        ImGui::TextWrapped("Paste a GitHub link to a script or plugin (https://github.com/owner/repo):");
        ImGui::SetNextItemWidth(-em * 6.0f);
        const bool enter = ImGui::InputTextWithHint("##addonlink", "https://github.com/owner/repo", &a.link,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(a.inspecting || a.link.empty());
        if ((ImGui::Button("Look") || enter) && !a.inspecting && !a.link.empty()) action = LoginAction::AddonInspect;
        ImGui::EndDisabled();
    } else {
        ImGui::TextWrapped("%s", a.why_off.c_str());
    }

    ImGui::Separator();
    if (a.available) {
        if (ImGui::SmallButton("Check for updates")) action = LoginAction::AddonCheck;
    }
    const char* groups[][2] = {{"installed", "Installed add-ons"}, {"manual", "In your folders"}, {"server", nullptr}};
    for (const auto& g : groups) {
        const std::string group = g[0];
        const std::string title = g[1] ? g[1] : "From " + (a.server.empty() ? std::string("this server") : a.server);
        bool any = false;
        for (const auto& r : a.rows) any = any || r.group == group;
        if (!any) continue;
        ImGui::SeparatorText(title.c_str());
        for (const auto& r : a.rows) {
            if (r.group != group) continue;
            ImGui::PushID((group + ":" + r.name).c_str());
            const std::string label = r.name + (r.version.empty() ? "" : " " + r.version) + "  (" + r.kind + ")";
            if (ImGui::TreeNode("##row", "%s", label.c_str())) {
                if (!r.description.empty()) ImGui::TextWrapped("%s", r.description.c_str());
                if (!r.source.empty()) ImGui::TextDisabled("from %s%s", r.source.c_str(), r.signed_ ? ", signed" : "");
                if (!r.state_text.empty()) ImGui::TextWrapped("%s", r.state_text.c_str());
                if (!r.caps.empty()) {
                    ImGui::TextUnformatted("May:");
                    list(r.caps);
                }
                if (!r.new_caps.empty()) {
                    ImGui::TextUnformatted("The new version also asks to:");
                    list(r.new_caps);
                }
                if (!r.modified.empty()) ImGui::TextDisabled("Changed by you: %zu file(s)", r.modified.size());
                if (!r.note.empty()) ImGui::TextWrapped("%s", r.note.c_str());
                if (r.group == "installed") {
                    bool on = r.auto_update;
                    if (ImGui::Checkbox("Update by itself", &on)) {
                        a.choice = r.name;
                        a.choice_on = on;
                        action = LoginAction::AddonAuto;
                    }
                    if (r.can_update) {
                        ImGui::SameLine();
                        const char* what = r.state == "approval" ? "Approve and update"
                                           : r.state == "modified" ? "Update anyway"
                                           : r.state == "failed"   ? "Try again"
                                                                   : "Update";
                        if (ImGui::Button(what)) choose(r.name, LoginAction::AddonUpdate);
                    }
                    if (!r.page.empty()) {
                        ImGui::SameLine();
                        if (ImGui::Button("Repository")) choose(r.name, LoginAction::AddonPage);
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!r.can_remove || busy);
                    if (ImGui::Button("Remove")) ImGui::OpenPopup("##remove");
                    ImGui::EndDisabled();
                    if (ImGui::BeginPopup("##remove")) {
                        ImGui::Text("Remove %s? Its own files go; yours in its folder stay.", r.name.c_str());
                        if (ImGui::Button("Remove")) {
                            choose(r.name, LoginAction::AddonRemove);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                }
                if (r.consent == "allowed") {
                    if (ImGui::Button("Don't allow it any more")) choose(r.name, LoginAction::AddonRevoke);
                    ImGui::SameLine();
                } else if (r.consent == "denied") {
                    if (ImGui::Button("Ask me again")) choose(r.name, LoginAction::AddonAskAgain);
                    ImGui::SameLine();
                }
                if (!r.folder.empty()) {
                    if (r.group == "installed") ImGui::SameLine();
                    if (ImGui::Button("Open folder")) choose(r.name, LoginAction::AddonFolder);
                }
                if (r.can_start) {
                    ImGui::SameLine();
                    if (ImGui::Button("Start")) choose(r.name, LoginAction::AddonStart);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
    return action;
}

// The screen's top left corner (docs/updater.md "What the player sees"): this build, whether it is up to date (or the
// updater's notice with its buttons), What's new (this release's part of CHANGELOG.md) and Hailstone's page.
LoginAction about_corner(const LoginView& view) {
    LoginAction action = LoginAction::None;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(em, em), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(em * 18.0f, 0.0f), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##hailstone_about", nullptr, flags)) {
        ImGui::Text("Hailstone %s", view.update.current.c_str());
        if (!view.version.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("This build: %s", view.version.c_str());
        ImGui::PushTextWrapPos(0.0f);
        if (update_notice_text(view.update).empty()) {
            const ImVec4 colour = view.update.check == "uptodate" ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                                                                  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(colour, "%s", update_status_text(view.update).c_str());
        } else if (LoginAction a = update_notice(view.update); a != LoginAction::None) {
            action = a;
        }
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(view.release_notes.empty());
        if (ImGui::SmallButton("What's new")) ImGui::OpenPopup("##whatsnew");
        ImGui::EndDisabled();
        if (!view.project_page.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("GitHub")) action = LoginAction::OpenProject;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", view.project_page.c_str());
        }
        ImGui::SetNextWindowSize(ImVec2(em * 36.0f, 0.0f), ImGuiCond_Always);
        if (ImGui::BeginPopup("##whatsnew")) {
            ImGui::SeparatorText(("What's new in Hailstone " + view.update.current).c_str());
            ImGui::BeginChild("##notes", ImVec2(0.0f, em * 24.0f));
            ImGui::PushTextWrapPos(0.0f);
            for (const auto& line : view.release_notes) {
                ImGui::Bullet();
                ImGui::TextWrapped("%s", line.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    return action;
}

} // namespace

LoginAction login_screen(LoginView& view) {
    LoginAction action = LoginAction::None;
    center_next_window(33.0f);
    ImGui::Begin("Login##hailstone", nullptr, kDialogFlags | ImGuiWindowFlags_AlwaysAutoResize);
    // AlwaysAutoResize keeps the height to the content; the width comes from the label column.
    float label_w = ImGui::GetFontSize() * 6.0f;
    float field_w = ImGui::GetFontSize() * 25.0f; // the accounts table and the buttons row fit
    bool submit = false;
    // The pad's navigation cursor starts on the first field still empty, else on Login (never typing: the pad
    // fills a field with the on-screen keyboard).
    const bool pad = pad_nav();
    const bool appearing = ImGui::IsWindowAppearing();

    ImGui::TextUnformatted("Hailstone");
    ImGui::SameLine();
    ImGui::TextColored(kDimColor, "developer login");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginDisabled(view.busy);
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(field_w);
    };
    constexpr ImGuiInputTextFlags kEnter = ImGuiInputTextFlags_EnterReturnsTrue;
    const bool picked = view.selected >= 0 && view.selected < static_cast<int>(view.profiles.size());
    // The game files (the asset folder): RoF2 or live, for every server.
    if (!view.game_folders.empty()) {
        label("Game files");
        const GameFolder& current = view.game_folders.front();
        if (view.game_folder_fixed) {
            ImGui::TextColored(kDimColor, "%s", game_folder_label(current).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nSet by --game-dir or EQ_GAME_DIR.", current.path.c_str());
        } else {
            if (ImGui::BeginCombo("##gamefiles", game_folder_label(current).c_str())) {
                for (std::size_t i = 0; i < view.game_folders.size(); ++i) {
                    const GameFolder& f = view.game_folders[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::BeginDisabled(!f.usable);
                    if (ImGui::Selectable(game_folder_label(f).c_str(), i == 0) && i != 0) {
                        view.game_choice = f.path;
                        action = LoginAction::UseGameFolder;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s%s", f.path.c_str(), f.usable ? "" : "\nNot usable now: see Browse.");
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::Selectable("Browse...")) action = LoginAction::BrowseGameFolder;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The EverQuest files to play with, RoF2 or live: %s\nA server's own files go over them.",
                                  current.path.c_str());
        }
    }
    if (!view.transient) {
        // The saved servers: pick one, start a new one, or delete the picked one.
        label("Server");
        const ImGuiStyle& style = ImGui::GetStyle();
        float buttons = buttons_width({"New", "Delete"}) + style.ItemSpacing.x;
        ImGui::SetNextItemWidth(field_w - buttons);
        std::string preview = picked ? view.profiles[static_cast<std::size_t>(view.selected)].name : "(new server)";
        if (ImGui::BeginCombo("##profile", preview.c_str())) {
            for (int i = 0; i < static_cast<int>(view.profiles.size()); ++i) {
                const ServerProfile& p = view.profiles[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                if (ImGui::Selectable(p.name.c_str(), view.selected == i)) select_profile(view, i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s:%d", p.host.c_str(), p.port);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New")) new_profile(view);
        ImGui::SameLine();
        ImGui::BeginDisabled(!picked);
        if (ImGui::Button("Delete")) ImGui::OpenPopup("Delete server?##hailstone");
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal("Delete server?##hailstone", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete \"%s\"?", preview.c_str());
            ImGui::TextColored(kDimColor, "Its folder (the server's asset files, maps and characters' files) is moved to");
            ImGui::TextColored(kDimColor, "servers/.deleted/, from where it can be brought back by hand.");
            if (ImGui::Button("Delete")) {
                action = LoginAction::Delete;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                (pad && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        // The fast way in: a character seen on one of this server's accounts, played at once (its account's saved
        // password, its world, then the character).
        if (auto quick = quick_characters(view); !quick.empty()) {
            label("Characters");
            if (view.quick_selected >= static_cast<int>(quick.size())) view.quick_selected = -1;
            constexpr ImGuiTableFlags kTable =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;
            const float em = ImGui::GetFontSize();
            const float rows = static_cast<float>(std::min<std::size_t>(quick.size(), 6));
            const float height = (rows + 1.0f) * ImGui::GetTextLineHeightWithSpacing() + em * 0.5f;
            ImGui::BeginGroup();
            if (ImGui::BeginTable("quick", 4, kTable, ImVec2(field_w, height))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Lvl", ImGuiTableColumnFlags_WidthFixed, em * 1.8f);
                ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
                ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
                ImGui::TableHeadersRow();
                for (int i = 0; i < static_cast<int>(quick.size()); ++i) {
                    const QuickCharacter& q = quick[static_cast<std::size_t>(i)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    if (pad && appearing && view.quick_selected == i) ImGui::SetKeyboardFocusHere();
                    if (ImGui::Selectable(q.character.name.c_str(), view.quick_selected == i,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                        view.quick_selected = i;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || pad_activated())
                            action = LoginAction::PlayCharacter;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s on %s\nAccount %s%s", q.character.name.c_str(),
                                          q.character.world.c_str(), q.username.c_str(),
                                          q.password_saved ? "" : "\nNo saved password: type it, then Play.");
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    if (q.character.level > 0) ImGui::Text("%d", q.character.level);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(q.character.classes.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(kDimColor, "%s", q.username.c_str());
                }
                ImGui::EndTable();
            }
            const bool can_quick = view.quick_selected >= 0;
            ImGui::BeginDisabled(!can_quick);
            if (ImGui::Button("Play", ImVec2(em * 7.0f, 0.0f))) action = LoginAction::PlayCharacter;
            ImGui::EndDisabled();
            if (can_quick) {
                const QuickCharacter& q = quick[static_cast<std::size_t>(view.quick_selected)];
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kDimColor, "%s on %s", q.character.name.c_str(), q.character.world.c_str());
            }
            ImGui::EndGroup();
            ImGui::Separator();
        }
        label("Name");
        text_field("##name", view.name, 0, "Server name", view.host.c_str());
    }
    label(view.transient ? "Server" : "Address");
    submit |= text_field("##host", view.host, kEnter | ImGuiInputTextFlags_CharsNoBlank, "Server address");
    label("Port");
    // Without EnterReturnsTrue: with it, InputInt keeps a typed number only when Enter is pressed and
    // drops it when the field loses focus.
    ImGui::InputInt("##port", &view.port, 0, 0);
    if (ImGui::IsItemActivated() && GImGui->ActiveIdSource == ImGuiInputSource_Gamepad) {
        ImGui::ClearActiveID();
        open_keyboard(nullptr, &view.port, "Port", 0);
    }
    if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        submit = true;
    view.port = std::clamp(view.port, 1, 65535);
    if (!view.transient) {
        // The user interface this server uses (servers/<id>/server.ini [Server] ui), from its next login on.
        label("Interface");
        const UiPack* ui_pack = nullptr;
        for (const auto& p : view.ui_packs)
            if (p.id == view.ui) ui_pack = &p;
        if (ImGui::BeginCombo("##ui", ui_pack ? ui_pack->name.c_str() : view.ui.c_str())) {
            for (const auto& p : view.ui_packs) {
                ImGui::PushID(p.id.c_str());
                if (ImGui::Selectable(p.name.c_str(), &p == ui_pack)) view.ui = p.id;
                if (ImGui::IsItemHovered() && !p.description.empty())
                    ImGui::SetTooltip("%s\nMade for: %s", p.description.c_str(), p.input.c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The look and layout of the game's windows on this server. Kept with the server.");
    }
    label("Username");
    // The server's saved accounts in a list beside the field: picking one fills its saved password.
    const bool has_accounts = !view.transient && !view.accounts.empty();
    if (has_accounts) ImGui::SetNextItemWidth(field_w - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    if (appearing && view.username.empty() && !pad) ImGui::SetKeyboardFocusHere();
    submit |= text_field("##user", view.username, kEnter, "Username");
    std::optional<std::string> picked_account;
    // A saved account typed in full counts as picked.
    if (ImGui::IsItemDeactivatedAfterEdit() && view.password.empty()) picked_account = view.username;
    if (appearing && pad && view.username.empty()) ImGui::SetItemDefaultFocus();
    if (has_accounts) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##accounts", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
            for (const auto& a : view.accounts) {
                ImGui::PushID(a.username.c_str());
                std::string row = a.username + (a.character.empty() ? "" : "  (" + a.character + ")");
                if (ImGui::Selectable(row.c_str(), a.username == view.username)) picked_account = a.username;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This server's saved accounts");
    }
    if (picked_account) pick_account(view, *picked_account);
    label("Password");
    if (appearing && !view.username.empty() && !pad) ImGui::SetKeyboardFocusHere();
    submit |= text_field("##pass", view.password, kEnter | ImGuiInputTextFlags_Password, "Password");
    if (appearing && pad && !view.username.empty()) ImGui::SetItemDefaultFocus();
    if (!view.transient) {
        ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::SameLine(label_w);
        ImGui::Checkbox("Remember account", &view.remember);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the username in this server's accounts.");
        ImGui::SameLine();
        const bool keyring = can_save_passwords(view);
        ImGui::BeginDisabled(!view.remember || !keyring);
        bool save_password = view.save_password && view.remember && keyring;
        if (ImGui::Checkbox("Save password", &save_password)) view.save_password = save_password;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", keyring ? "Kept in the system's keyring once the server takes it, never in "
                                              "Hailstone's folder.\nNeeded for an account in the box set."
                                            : "There is no system keyring Hailstone can use (libsecret, the Credential "
                                              "Manager or the Keychain), so no password is saved.");
        if (picked && !view.accounts.empty()) {
            // The box set: the ticked accounts log in together, each in its own window (Log in box).
            label("Accounts");
            constexpr ImGuiTableFlags kTable =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit;
            const float em = ImGui::GetFontSize();
            std::optional<std::string> forget;
            if (ImGui::BeginTable("accounts", 5, kTable, ImVec2(field_w, 0.0f))) {
                ImGui::TableSetupColumn("Box");
                ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("World", ImGuiTableColumnFlags_WidthFixed, em * 7.5f);
                ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthFixed, em * 6.5f);
                ImGui::TableSetupColumn("##forget");
                ImGui::TableHeadersRow();
                for (auto& a : view.accounts) {
                    ImGui::PushID(a.username.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool can_box = a.save_password && keyring;
                    ImGui::BeginDisabled(!can_box);
                    bool box = a.box && can_box;
                    if (ImGui::Checkbox("##box", &box)) {
                        a.box = box;
                        action = LoginAction::AccountsChanged;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", can_box ? "In the box set: Log in box starts a window for it."
                                                        : "Log in with it once with Save password ticked to box it.");
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (a.username == view.username) ImGui::TextColored(kGoodColor, "%s", a.username.c_str());
                    else ImGui::TextUnformatted(a.username.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", a.save_password ? "Password saved" : "No password saved");
                    // The world a box login plays (the server select skipped): one the login server listed, or
                    // the one played last there.
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##world", a.world.empty() ? "(last)" : a.world.c_str())) {
                        if (ImGui::Selectable("(last played)", a.world.empty()) && !a.world.empty()) {
                            a.world.clear();
                            a.character.clear();
                            action = LoginAction::AccountsChanged;
                        }
                        for (const std::string& w : view.worlds) {
                            if (ImGui::Selectable(w.c_str(), w == a.world) && w != a.world) {
                                a.world = w;
                                // Its character there: the one entered last on it, else the first seen.
                                a.character.clear();
                                for (const auto& c : a.characters)
                                    if (c.world == w && a.character.empty()) a.character = c.name;
                                action = LoginAction::AccountsChanged;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", view.worlds.empty()
                                                    ? "The game servers show here once this server has listed them "
                                                      "(log in once)."
                                                    : "The game server this account plays in a box login.");
                    // Its character on that world, as the world's character select listed them; empty: choose at
                    // character select.
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##character", a.character.empty() ? "(choose)" : a.character.c_str())) {
                        if (ImGui::Selectable("(choose)", a.character.empty()) && !a.character.empty()) {
                            a.character.clear();
                            action = LoginAction::AccountsChanged;
                        }
                        for (const auto& c : a.characters) {
                            if (!a.world.empty() && c.world != a.world) continue;
                            std::string row = c.name + (c.level > 0 ? std::format("  {} {}", c.level, c.classes) : "");
                            if (a.world.empty()) row += "  (" + c.world + ")";
                            if (ImGui::Selectable(row.c_str(), c.name == a.character) && c.name != a.character) {
                                a.character = c.name;
                                a.world = c.world;
                                action = LoginAction::AccountsChanged;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The character a box login enters; (choose): pick it at character select.");
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Forget")) forget = a.username;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (forget) {
                view.account_choice = *forget;
                ImGui::OpenPopup("Forget account?##hailstone");
            }
            if (ImGui::BeginPopupModal("Forget account?##hailstone", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Forget %s on this server?", view.account_choice.c_str());
                ImGui::TextColored(kDimColor, "Its saved password leaves the system keyring.");
                if (ImGui::Button("Forget")) {
                    action = LoginAction::ForgetAccount;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                    (pad && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        // The server's own client files (servers/<id>/assets), found before the game files.
        label("Server files");
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(label_w + field_w);
        if (picked) {
            // Shown from the data folder on (servers/<id>/assets), the whole path as the tooltip.
            std::filesystem::path dir = selected_assets_dir(view);
            std::filesystem::path shown = dir.lexically_relative(view.servers_dir.parent_path());
            if (shown.empty()) shown = dir;
            ImGui::TextColored(kDimColor, "%s", shown.generic_string().c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dir.string().c_str());
        } else ImGui::TextColored(kDimColor, "The server's own files: a folder made when it is saved.");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(!picked);
        if (ImGui::Button("Open folder")) action = LoginAction::OpenAssets;
        ImGui::EndDisabled();
        ImGui::EndGroup();
    }

    ImGui::Spacing();
    bool ready = !view.username.empty() && !view.password.empty() && !view.host.empty();
    ImGui::BeginDisabled(!ready);
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Button("Login", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) submit = true;
    ImGui::EndDisabled();
    if (pad_screen_pressed(ImGuiKey_GamepadStart) && !view.busy) {
        // Start logs in, or asks for what is still missing.
        if (ready) submit = true;
        else if (view.host.empty()) open_keyboard(&view.host, nullptr, "Server address", ImGuiInputTextFlags_CharsNoBlank);
        else if (view.username.empty()) open_keyboard(&view.username, nullptr, "Username", 0);
        else open_keyboard(&view.password, nullptr, "Password", ImGuiInputTextFlags_Password);
    }
    if (!view.transient) {
        if (const std::size_t boxed = box_accounts(view).size(); picked && boxed > 0) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!box_ready(view));
            if (ImGui::Button(std::format("Log in box ({})", boxed).c_str())) action = LoginAction::LoginBox;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The box set's accounts: this window logs in the first, and a new window starts "
                                  "for each of the others. Each enters its character.");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(view.host.empty());
        if (ImGui::Button("Save")) action = LoginAction::Save;
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit")) action = LoginAction::Quit;
    ImGui::EndDisabled();
    // The add-on manager (docs/addons.md), a window of its own.
    if (!distribution::managed()) {
        ImGui::SameLine();
        if (ImGui::Button("Add-ons...")) view.addons.open = !view.addons.open;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Install scripts and plugins from GitHub, and keep them up to date.");
    }
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Music", &view.pregame_music)) action = LoginAction::MusicChanged;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The music of the login, server select and character select screens (not the game's).");

    ImGui::Spacing();
    ImGui::PushTextWrapPos(label_w + field_w);
    status_line(view.status, view.busy);
    if (!view.busy && !distribution::managed())
        if (LoginAction a = addon_suggestions(view.addons, view.addons.choice); a != LoginAction::None) action = a;
    ImGui::PopTextWrapPos();
    pad_hints({{platform::PadButton::A, "Select"}, {platform::PadButton::Start, "Login"}});
    keyboard_popup();
    ImGui::End();

    if (view.addons.open && !distribution::managed())
        if (LoginAction a = addons_window(view.addons, view.busy, &view.addons.open); a != LoginAction::None) action = a;
    if (LoginAction a = about_corner(view); a != LoginAction::None) action = a;

    if (submit && ready && !view.busy) action = LoginAction::Submit;
    return action;
}

std::string game_folder_label(const GameFolder& folder) {
    std::filesystem::path p(folder.path);
    std::string name = (p.has_filename() ? p.filename() : p.parent_path().filename()).string();
    const char* kind = !folder.usable ? "Not usable" : folder.live ? "Live" : "RoF2";
    return std::string(kind) + ": " + (name.empty() ? folder.path : name);
}

std::string selected_profile_id(const LoginView& view) {
    if (view.transient || view.selected < 0 || view.selected >= static_cast<int>(view.profiles.size())) return {};
    return view.profiles[static_cast<std::size_t>(view.selected)].id;
}

std::filesystem::path selected_assets_dir(const LoginView& view) {
    std::string id = selected_profile_id(view);
    return id.empty() ? std::filesystem::path() : profile_assets_dir(id, view.servers_dir);
}

void select_profile(LoginView& view, int index) {
    if (index < 0 || index >= static_cast<int>(view.profiles.size())) {
        new_profile(view);
        return;
    }
    const ServerProfile& p = view.profiles[static_cast<std::size_t>(index)];
    view.selected = index;
    view.name = p.name;
    view.host = p.host;
    view.port = std::clamp(p.port, 1, 65535);
    view.remember = p.remember;
    view.accounts = p.accounts;
    view.ui = p.ui.empty() ? view.default_ui : p.ui;
    view.worlds = listed_worlds(p.id, view.servers_dir);
    for (const auto& a : view.accounts)
        for (const auto& c : a.characters)
            if (std::find(view.worlds.begin(), view.worlds.end(), c.world) == view.worlds.end())
                view.worlds.push_back(c.world);
    pick_account(view, p.remember ? p.username : std::string());
    view.quick_selected = default_quick_character(view);
}

bool can_save_passwords(const LoginView& view) { return view.credentials && view.credentials->available(); }

std::vector<QuickCharacter> quick_characters(const LoginView& view) {
    std::vector<QuickCharacter> out;
    if (view.transient) return out;
    std::string last_user;
    if (view.selected >= 0 && view.selected < static_cast<int>(view.profiles.size()))
        last_user = view.profiles[static_cast<std::size_t>(view.selected)].username;
    for (const auto& a : view.accounts)
        for (const auto& c : a.characters)
            out.push_back({a.username, c, a.save_password && can_save_passwords(view),
                           a.username == last_user && c.name == a.character && c.world == a.world});
    return out;
}

int default_quick_character(const LoginView& view) {
    auto list = quick_characters(view);
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i].last) return static_cast<int>(i);
    return list.empty() ? -1 : 0;
}

std::string credential_server(const LoginView& view) { return platform::credential_server(view.host, view.port); }

void pick_account(LoginView& view, std::string_view username) {
    view.username = std::string(username);
    core::wipe(view.password);
    view.save_password = false;
    const SavedAccount* a = nullptr;
    for (const auto& acc : view.accounts)
        if (!username.empty() && acc.username.size() == username.size() && contains_ci(acc.username, username)) a = &acc;
    if (!a) return;
    view.username = a->username;
    view.save_password = a->save_password && can_save_passwords(view);
    if (view.save_password)
        if (auto pw = view.credentials->load(credential_server(view), a->username)) view.password = std::move(*pw);
}

bool save_login_password(LoginView& view) {
    if (view.transient || !view.remember || !view.save_password || !can_save_passwords(view) || view.username.empty() ||
        view.password.empty())
        return true;
    if (view.credentials->save(credential_server(view), view.username, view.password)) return true;
    view.status = {"The password was not saved: " + view.credentials->error(), true};
    return false;
}

std::vector<SavedAccount> box_accounts(const LoginView& view) {
    std::vector<SavedAccount> out;
    for (const auto& a : view.accounts)
        if (a.box) out.push_back(a);
    return out;
}

bool box_ready(const LoginView& view) {
    auto set = box_accounts(view);
    if (set.empty() || !can_save_passwords(view)) return false;
    for (const auto& a : set)
        if (!a.save_password) return false;
    return true;
}

bool forget_account(LoginView& view, Config& config, std::string_view username) {
    std::string name(username); // username may be account_choice or a list entry
    if (view.credentials) view.credentials->erase(credential_server(view), name);
    std::erase_if(view.accounts, [&](const SavedAccount& a) { return a.username == name; });
    if (view.username == name) {
        view.username.clear();
        view.password.clear();
        view.save_password = false;
    }
    return store_login_settings(view, config).has_value();
}

void new_profile(LoginView& view) {
    LoginView fresh;
    view.selected = -1;
    view.name.clear();
    view.host = fresh.host;
    view.port = fresh.port;
    view.remember = fresh.remember;
    view.accounts.clear();
    view.worlds.clear();
    view.quick_selected = -1;
    view.username.clear();
    view.password.clear();
    view.save_password = false;
    view.ui = view.default_ui;
}

bool select_profile_named(LoginView& view, std::string_view id_or_name) {
    const ServerProfile* p = find_profile(view.profiles, id_or_name);
    for (std::size_t i = 0; !p && i < view.profiles.size(); ++i)
        if (view.profiles[i].name.size() == id_or_name.size() && contains_ci(view.profiles[i].name, id_or_name))
            p = &view.profiles[i];
    if (!p) return false;
    select_profile(view, static_cast<int>(p - view.profiles.data()));
    return true;
}

ServerProfile profile_from_view(const LoginView& view) {
    ServerProfile p;
    bool picked = !view.transient && view.selected >= 0 && view.selected < static_cast<int>(view.profiles.size());
    if (picked) p = view.profiles[static_cast<std::size_t>(view.selected)]; // its id and what else it keeps
    auto trim = [](std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return std::string(s);
    };
    p.name = trim(view.name);
    p.host = trim(view.host);
    if (p.name.empty()) p.name = p.host;
    p.port = std::clamp(view.port, 1, 65535);
    p.username = view.username;
    p.remember = view.remember;
    p.accounts = view.accounts;
    // A remembered username joins the accounts (at the end), with whether its password is kept.
    if (view.remember && !trim(view.username).empty()) {
        SavedAccount* a = find_account(p, view.username);
        if (!a) {
            p.accounts.push_back(SavedAccount{.username = view.username});
            a = &p.accounts.back();
        }
        a->save_password = view.save_password && can_save_passwords(view);
    }
    p.ui = view.ui;
    if (view.transient) p.id.clear();
    else if (!picked) p.id = make_profile_id(p.name, view.servers_dir);
    return p;
}

bool load_login_settings(LoginView& view, Config& config, const std::filesystem::path& servers) {
    view.servers_dir = servers;
    view.profiles = list_profiles(servers);
    view.default_ui = config.get_string("ui.name", "modern");
    bool changed = false;
    if (view.profiles.empty() && config.get("login.host")) {
        // The settings of before the server profiles: login.host, login.port, login.username, login.remember.
        ServerProfile p;
        p.host = config.get_string("login.host");
        p.name = p.host;
        p.port = std::clamp(config.get_int("login.port", p.port), 1, 65535);
        p.remember = config.get_bool("login.remember", true);
        p.username = config.get_string("login.username");
        p.id = make_profile_id(p.name, servers);
        if (!p.host.empty() && save_profile(p, servers)) {
            for (const char* key : {"login.host", "login.port", "login.username", "login.remember"}) config.erase(key);
            config.set("login.profile", p.id);
            changed = true;
            view.profiles = list_profiles(servers);
        }
    }
    int index = view.profiles.empty() ? -1 : 0;
    if (auto id = config.get("login.profile"))
        if (const ServerProfile* p = find_profile(view.profiles, *id))
            index = static_cast<int>(p - view.profiles.data());
    select_profile(view, index);
    return changed;
}

std::optional<ServerProfile> store_login_settings(LoginView& view, Config& config) {
    if (view.transient) return std::nullopt;
    ServerProfile p = profile_from_view(view);
    if (p.host.empty()) {
        view.status = {"The server needs an address.", true};
        return std::nullopt;
    }
    if (!save_profile(p, view.servers_dir)) {
        view.status = {"Could not save " + profile_ini(p.id, view.servers_dir).string(), true};
        return std::nullopt;
    }
    config.set("login.profile", p.id);
    // A password no longer kept leaves the keyring; one to keep is saved once the login takes it
    // (save_login_password), so a mistyped one never is.
    if (view.credentials && !view.username.empty() && !(view.remember && view.save_password))
        view.credentials->erase(platform::credential_server(p.host, p.port), view.username);
    std::string username = view.username, password = view.password;
    bool save_password = view.save_password;
    view.profiles = list_profiles(view.servers_dir);
    if (const ServerProfile* saved = find_profile(view.profiles, p.id)) {
        view.selected = static_cast<int>(saved - view.profiles.data());
        view.accounts = saved->accounts;
    }
    view.save_password = save_password;
    // What was typed stays, even when the username is not remembered.
    view.username = std::move(username);
    view.password = std::move(password);
    core::wipe(password);
    return p;
}

bool delete_login_profile(LoginView& view, Config& config) {
    std::string id = selected_profile_id(view);
    if (id.empty()) return false;
    const ServerProfile gone = view.profiles[static_cast<std::size_t>(view.selected)];
    if (!remove_profile(id, view.servers_dir)) {
        view.status = {"Could not delete " + profile_dir(id, view.servers_dir).string(), true};
        return false;
    }
    // Its passwords, unless another saved server has the same address (they share them).
    bool shared = false;
    for (const auto& p : view.profiles)
        shared |= p.id != gone.id && p.port == gone.port && platform::credential_server(p.host, p.port) ==
                                                                 platform::credential_server(gone.host, gone.port);
    if (view.credentials && !shared)
        for (const auto& a : gone.accounts)
            if (a.save_password) view.credentials->erase(platform::credential_server(gone.host, gone.port), a.username);
    if (config.get_string("login.profile") == id) config.erase("login.profile");
    view.profiles = list_profiles(view.servers_dir);
    select_profile(view, view.profiles.empty() ? -1 : 0);
    return true;
}

// ---- Server select --------------------------------------------------------

std::vector<std::size_t> server_list_order(const std::vector<std::string>& names,
                                           const std::vector<std::string>& favorites) {
    std::vector<std::size_t> order(names.size());
    std::vector<bool> fav(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        order[i] = i;
        fav[i] = std::find(favorites.begin(), favorites.end(), names[i]) != favorites.end();
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (fav[a] != fav[b]) return static_cast<bool>(fav[a]);
        const int c = icompare(names[a], names[b]);
        return c != 0 ? c < 0 : names[a] < names[b];
    });
    return order;
}

std::vector<std::string> parse_favorite_worlds(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const std::size_t bar = text.find('|');
        std::string_view name = text.substr(0, bar);
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.emplace_back(name);
        if (bar == std::string_view::npos) break;
        text.remove_prefix(bar + 1);
    }
    return out;
}

std::string join_favorite_worlds(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (n.empty() || n.find('|') != std::string::npos) continue;
        if (!out.empty()) out += '|';
        out += n;
    }
    return out;
}

ServerSelectAction server_select_screen(ServerSelectView& view) {
    ServerSelectAction action = ServerSelectAction::None;
    center_next_window(34.0f, 24.0f);
    ImGui::Begin("Select a server##hailstone", nullptr, kDialogFlags);
    // With the pad the picked row follows the navigation cursor, which starts on it.
    const bool pad = pad_nav();
    const bool appearing = ImGui::IsWindowAppearing();

    float footer = ImGui::GetFrameHeightWithSpacing() * (pad ? 3.0f : 2.0f) + ImGui::GetStyle().ItemSpacing.y;
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    ImGui::BeginDisabled(view.busy);
    if (ImGui::BeginTable("servers", 3, kTable, ImVec2(0.0f, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Server", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Favourite", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(view.servers.size()); ++i) {
            const ServerEntry& s = view.servers[static_cast<std::size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (pad && appearing && view.selected == i) ImGui::SetKeyboardFocusHere();
            if (ImGui::Selectable(s.name.c_str(), view.selected == i,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                view.selected = i;
                if ((ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || pad_activated()) && s.can_join)
                    action = ServerSelectAction::Play;
            }
            if (pad && ImGui::IsItemFocused()) view.selected = i;
            ImGui::TableNextColumn();
            ImGui::TextColored(s.can_join ? kGoodColor : kErrorColor, "%s", s.status.c_str());
            // The favourite mark (after the row, which lets it take the click): a favourite world is listed first
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(s.favorite ? "Unstar##fav" : "Star##fav")) {
                view.favorite_row = i;
                action = ServerSelectAction::Favorite;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(s.favorite ? "A favourite: listed first. Click to unmark it"
                                             : "Mark as a favourite: favourites are listed first");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (view.servers.empty()) ImGui::TextColored(kDimColor, "No servers listed.");
    ImGui::EndDisabled();

    status_line(view.status, view.busy);
    if (!view.addon_hint.empty()) ImGui::TextWrapped("%s", view.addon_hint.c_str());
    bool can_play = !view.busy && view.selected >= 0 && view.selected < static_cast<int>(view.servers.size()) &&
                    view.servers[static_cast<std::size_t>(view.selected)].can_join;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons_width({"Back", "Play"}));
    if (ImGui::Button("Back")) action = ServerSelectAction::Back;
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_play);
    if (ImGui::Button("Play")) action = ServerSelectAction::Play;
    ImGui::EndDisabled();
    if (can_play && enter_pressed() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        action = ServerSelectAction::Play;
    if (pad_screen_pressed(ImGuiKey_GamepadStart)) action = ServerSelectAction::Play;
    if (pad_screen_pressed(ImGuiKey_GamepadFaceRight)) action = ServerSelectAction::Back;
    pad_hints({{platform::PadButton::A, "Play"}, {platform::PadButton::Start, "Play"}, {platform::PadButton::B, "Back"}});
    keyboard_popup();
    ImGui::End();
    if (action == ServerSelectAction::Play && !can_play) action = ServerSelectAction::None;
    return action;
}

// ---- Character select -----------------------------------------------------

CharacterSelectAction character_select_screen(CharacterSelectView& view) {
    CharacterSelectAction action = CharacterSelectAction::None;
    if (view.scene) left_next_window(34.0f, 28.0f);
    else center_next_window(46.0f, 28.0f);
    std::string title = view.server_name.empty() ? std::string("Select a character")
                                                 : std::format("Select a character - {}", view.server_name);
    ImGui::Begin(std::format("{}###hailstone_charselect", title).c_str(), nullptr, kDialogFlags);
    // With the pad the picked row follows the navigation cursor, which starts on it.
    const bool pad = pad_nav();
    const bool appearing = ImGui::IsWindowAppearing();

    // under the list: the status line, the buttons and the pad's hints; one left out made the window scroll
    float rows = 2.0f + (pad ? 1.0f : 0.0f);
    float footer = ImGui::GetFrameHeightWithSpacing() * rows + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginDisabled(view.busy);
    if (view.characters.empty()) {
        ImGui::BeginChild("empty", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
        ImGui::Spacing();
        ImGui::TextColored(kDimColor, "No characters on this account yet. Use Create Character.");
        ImGui::EndChild();
    } else {
        constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("characters", 5, kTable, ImVec2(0.0f, -footer))) {
            float em = ImGui::GetFontSize();
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, em * 3.0f);
            ImGui::TableSetupColumn("Race", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(view.characters.size()); ++i) {
                const CharacterEntry& c = view.characters[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (pad && appearing && view.selected == i) ImGui::SetKeyboardFocusHere();
                if (ImGui::Selectable(c.name.c_str(), view.selected == i,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                    view.selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || pad_activated())
                        action = CharacterSelectAction::EnterWorld;
                }
                if (pad && ImGui::IsItemFocused()) view.selected = i;
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%d", c.level);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(game::race_name(c.race).c_str());
                ImGui::TableNextColumn();
                std::string classes = game::class_list_name(c.classes, c.classes.size() > 1);
                ImGui::TextUnformatted(classes.c_str());
                if (c.classes.size() > 1 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", game::class_list_name(c.classes).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(c.zone.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndDisabled();

    status_line(view.status, view.busy);
    bool can_enter = !view.busy && view.selected >= 0 && view.selected < static_cast<int>(view.characters.size());
    if (ImGui::Button("Logout")) action = CharacterSelectAction::Logout;
    ImGui::SameLine();
    ImGui::BeginDisabled(view.busy);
    if (ImGui::Button("Create Character")) action = CharacterSelectAction::Create;
    ImGui::SameLine();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!can_enter);
    if (ImGui::Button("Delete")) ImGui::OpenPopup("Delete character?");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Delete character?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (view.selected >= 0 && view.selected < static_cast<int>(view.characters.size()))
            ImGui::Text("Delete %s permanently?", view.characters[static_cast<std::size_t>(view.selected)].name.c_str());
        if (ImGui::Button("Delete")) {
            action = CharacterSelectAction::Delete;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || (pad && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // Return Home (CLW_Home_Button), asked first as the original does (dialog 0x65, eqstr 6137, 10 s).
    const CharacterEntry* home =
        can_enter && view.characters[static_cast<std::size_t>(view.selected)].can_go_home
            ? &view.characters[static_cast<std::size_t>(view.selected)]
            : nullptr;
    ImGui::SameLine();
    ImGui::BeginDisabled(home == nullptr);
    if (ImGui::Button("Return Home")) ImGui::OpenPopup("Return home?");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Return home?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (home) ImGui::TextUnformatted(home->home_question.c_str());
        ImGui::BeginDisabled(home == nullptr);
        if (ImGui::Button("Yes")) {
            action = CharacterSelectAction::ReturnHome;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("No") || (pad && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons_width({"Back", "Enter World"}));
    if (ImGui::Button("Back")) action = CharacterSelectAction::Back;
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_enter);
    if (ImGui::Button("Enter World")) action = CharacterSelectAction::EnterWorld;
    ImGui::EndDisabled();
    if (can_enter && enter_pressed() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        action = CharacterSelectAction::EnterWorld;
    if (pad_screen_pressed(ImGuiKey_GamepadStart)) action = CharacterSelectAction::EnterWorld;
    if (pad_screen_pressed(ImGuiKey_GamepadFaceRight)) action = CharacterSelectAction::Back;
    pad_hints({{platform::PadButton::A, "Enter World"},
               {platform::PadButton::Start, "Enter World"},
               {platform::PadButton::B, "Back"}});
    keyboard_popup();
    ImGui::End();
    if (action == CharacterSelectAction::EnterWorld && !can_enter) action = CharacterSelectAction::None;
    if (action == CharacterSelectAction::ReturnHome && home == nullptr) action = CharacterSelectAction::None;
    return action;
}

// ---- Character creation -------------------------------------------------

CharacterCreateAction character_create_screen(CharacterCreateView& view) {
    CharacterCreateAction action = CharacterCreateAction::None;
    if (view.scene) left_next_window(30.0f);
    else center_next_window(30.0f);
    ImGui::Begin("Create a character###hailstone_charcreate", nullptr, kDialogFlags | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BeginDisabled(view.busy);

    create_normalize(view);
    const std::vector<std::uint32_t> races = create_races(view), classes = create_classes(view);
    float field = ImGui::GetFontSize() * 16.0f;
    const bool pad = pad_nav();
    ImGui::SetNextItemWidth(field);
    text_field("Name", view.name, ImGuiInputTextFlags_CharsNoBlank, "Character name");
    if (pad && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("Race", game::race_name(view.race).c_str())) {
        for (auto r : races)
            if (ImGui::Selectable(game::race_name(r).c_str(), r == view.race)) view.race = r;
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("Class", game::class_name(view.class_id).c_str())) {
        for (auto c : classes)
            if (ImGui::Selectable(game::class_name(c).c_str(), c == view.class_id)) view.class_id = c;
        ImGui::EndCombo();
    }
    int gender = static_cast<int>(view.gender);
    ImGui::RadioButton("Male", &gender, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Female", &gender, 1);
    view.gender = static_cast<std::uint32_t>(gender);
    create_normalize(view);

    // Deity, then start city (create_deities, create_cities).
    auto city_label = [](std::uint32_t z) { return game::zone_long_name(z); };
    std::uint32_t deity = view.option >= 0 ? view.options[static_cast<std::size_t>(view.option)].deity : 0;
    std::uint32_t city = view.option >= 0 ? view.options[static_cast<std::size_t>(view.option)].start_zone : 0;
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("Deity", view.option >= 0 ? create_deity_label(view, deity).c_str() : "-")) {
        for (auto d : create_deities(view))
            if (ImGui::Selectable(create_deity_label(view, d).c_str(), d == deity)) create_set_deity(view, d);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("Start city", view.option >= 0 ? city_label(city).c_str() : "-")) {
        for (auto z : create_cities(view))
            if (ImGui::Selectable(city_label(z).c_str(), z == city)) create_set_city(view, z);
        ImGui::EndCombo();
    }

    if (view.option >= 0) {
        static constexpr const char* kStats[7] = {"Strength", "Dexterity", "Agility", "Stamina",
                                                   "Intelligence", "Wisdom", "Charisma"};
        const CreateOption& o = view.options[static_cast<std::size_t>(view.option)];
        ImGui::SeparatorText("Stats");
        const float label_w = ImGui::CalcTextSize("Intelligence").x + ImGui::GetStyle().ItemSpacing.x;
        const float button = ImGui::GetFrameHeight();
        for (int i = 0; i < 7; ++i) {
            const auto si = static_cast<std::size_t>(i);
            ImGui::PushID(i);
            // a stat the class puts points in is green, as CCA_Stat<n>_Button
            if (o.points[si] > 0) ImGui::TextColored(kGoodColor, "%s", kStats[i]);
            else ImGui::TextUnformatted(kStats[i]);
            ImGui::SameLine(label_w);
            ImGui::BeginDisabled(view.stats[si] <= static_cast<int>(o.base[si]));
            if (ImGui::Button("-", ImVec2(button, 0))) create_stat_down(view, i);
            ImGui::EndDisabled();
            ImGui::SameLine();
            // right-aligned in a three-digit column
            std::string value = std::to_string(view.stats[si]);
            ImGui::Dummy(ImVec2(ImGui::CalcTextSize("000").x - ImGui::CalcTextSize(value.c_str()).x, 0));
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(value.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(view.points_left <= 0 || view.stats[si] >= create_stat_cap(view, i));
            if (ImGui::Button("+", ImVec2(button, 0))) create_stat_up(view, i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::Text("Points left: %d", view.points_left);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) create_reset_stats(view);
    }
    if (view.scene && ImGui::Button("Appearance")) action = CharacterCreateAction::Appearance;
    ImGui::TextColored(kDimColor, "Name: 4-15 letters, first letter upper case.");
    ImGui::EndDisabled();

    status_line(view.status, view.busy);
    if (view.option >= 0 && view.points_left > 0)
        ImGui::TextColored(kDimColor, "Spend the %d points left before creating.", view.points_left);
    bool can_create = create_ready(view);
    if (ImGui::Button("Cancel")) action = CharacterCreateAction::Cancel;
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_create);
    if (ImGui::Button("Create")) action = CharacterCreateAction::Create;
    ImGui::EndDisabled();
    if (pad_screen_pressed(ImGuiKey_GamepadStart) && can_create) action = CharacterCreateAction::Create;
    if (pad_screen_pressed(ImGuiKey_GamepadFaceRight) && !view.busy) action = CharacterCreateAction::Cancel;
    pad_hints({{platform::PadButton::A, "Select"}, {platform::PadButton::Start, "Create"}, {platform::PadButton::B, "Cancel"}});
    keyboard_popup();
    ImGui::End();
    return action;
}

void create_normalize(CharacterCreateView& view) {
    const std::vector<std::uint32_t> races = create_races(view);
    if (!races.empty() && std::find(races.begin(), races.end(), view.race) == races.end()) view.race = races.front();
    const std::vector<std::uint32_t> classes = create_classes(view);
    if (!classes.empty() && std::find(classes.begin(), classes.end(), view.class_id) == classes.end())
        view.class_id = classes.front();
    const CreateOption* cur = view.option >= 0 && view.option < static_cast<int>(view.options.size())
                                  ? &view.options[static_cast<std::size_t>(view.option)]
                                  : nullptr;
    if (!cur || cur->race != view.race || cur->class_id != view.class_id) {
        view.option = -1;
        for (int i = 0; i < static_cast<int>(view.options.size()) && view.option < 0; ++i) {
            const auto& o = view.options[static_cast<std::size_t>(i)];
            if (o.race == view.race && o.class_id == view.class_id) view.option = i;
        }
    }
    // The stats: reset to the allocation's base whenever the combination changes (race, class or deity reset them
    // in the original; a combination's allocation can differ by city too).
    if (view.option != view.stats_for) create_reset_stats(view);
}

std::vector<std::uint32_t> create_races(const CharacterCreateView& view) {
    std::vector<std::uint32_t> races;
    for (const auto& o : view.options)
        if (std::find(races.begin(), races.end(), o.race) == races.end()) races.push_back(o.race);
    std::sort(races.begin(), races.end());
    return races;
}

std::vector<std::uint32_t> create_classes(const CharacterCreateView& view) {
    std::vector<std::uint32_t> classes;
    for (const auto& o : view.options)
        if (o.race == view.race && std::find(classes.begin(), classes.end(), o.class_id) == classes.end())
            classes.push_back(o.class_id);
    std::sort(classes.begin(), classes.end());
    return classes;
}

std::string create_deity_label(const CharacterCreateView& view, std::uint32_t deity) {
    std::string n = view.deity_name ? view.deity_name(deity) : std::string();
    return n.empty() ? std::format("Deity {}", deity) : n;
}

std::vector<std::uint32_t> create_deities(const CharacterCreateView& view) {
    std::vector<std::uint32_t> deities;
    for (const auto& o : view.options)
        if (o.race == view.race && o.class_id == view.class_id &&
            std::find(deities.begin(), deities.end(), o.deity) == deities.end())
            deities.push_back(o.deity);
    std::sort(deities.begin(), deities.end(), [&](std::uint32_t a, std::uint32_t b) {
        return create_deity_label(view, a) < create_deity_label(view, b);
    });
    return deities;
}

std::vector<std::uint32_t> create_cities(const CharacterCreateView& view) {
    std::vector<std::uint32_t> cities;
    if (view.option < 0 || view.option >= static_cast<int>(view.options.size())) return cities;
    const std::uint32_t deity = view.options[static_cast<std::size_t>(view.option)].deity;
    for (const auto& o : view.options)
        if (o.race == view.race && o.class_id == view.class_id && o.deity == deity &&
            std::find(cities.begin(), cities.end(), o.start_zone) == cities.end())
            cities.push_back(o.start_zone);
    std::sort(cities.begin(), cities.end(),
              [](std::uint32_t a, std::uint32_t b) { return game::zone_long_name(a) < game::zone_long_name(b); });
    return cities;
}

namespace {
int create_row(const CharacterCreateView& view, std::uint32_t deity, std::uint32_t zone) {
    for (int i = 0; i < static_cast<int>(view.options.size()); ++i) {
        const auto& o = view.options[static_cast<std::size_t>(i)];
        if (o.race == view.race && o.class_id == view.class_id && o.deity == deity && (zone == 0 || o.start_zone == zone))
            return i;
    }
    return -1;
}
} // namespace

bool create_set_deity(CharacterCreateView& view, std::uint32_t deity) {
    const std::uint32_t city =
        view.option >= 0 && view.option < static_cast<int>(view.options.size()) ? view.options[static_cast<std::size_t>(view.option)].start_zone : 0;
    int row = city ? create_row(view, deity, city) : -1;
    if (row < 0) row = create_row(view, deity, 0);
    if (row < 0) return false;
    view.option = row;
    create_normalize(view);
    return true;
}

bool create_set_city(CharacterCreateView& view, std::uint32_t zone) {
    if (view.option < 0 || view.option >= static_cast<int>(view.options.size()) || zone == 0) return false;
    int row = create_row(view, view.options[static_cast<std::size_t>(view.option)].deity, zone);
    if (row < 0) return false;
    view.option = row;
    create_normalize(view);
    return true;
}

bool create_ready(const CharacterCreateView& view) {
    return !view.busy && view.option >= 0 && view.option < static_cast<int>(view.options.size()) &&
           view.stats_for == view.option && view.points_left == 0 && view.name.size() >= 4;
}

bool create_stat_max(CharacterCreateView& view, int stat) {
    bool any = false;
    while (create_stat_up(view, stat)) any = true;
    return any;
}

namespace {
// A stat's base or free points from the world server's allocation table, as an int, at most 255 (no stock value
// comes near it): a value near 2^32 overflowed the points sum and the base + 25 cap.
int create_value(std::uint32_t v) { return static_cast<int>(std::min<std::uint32_t>(v, 255)); }
} // namespace

void create_reset_stats(CharacterCreateView& view) {
    view.stats_for = view.option;
    view.points_left = 0;
    view.stats.fill(0);
    if (view.option < 0 || view.option >= static_cast<int>(view.options.size())) return;
    const CreateOption& o = view.options[static_cast<std::size_t>(view.option)];
    for (std::size_t i = 0; i < 7; ++i) {
        view.stats[i] = create_value(o.base[i]);
        view.points_left += create_value(o.points[i]);
    }
}

int create_stat_cap(const CharacterCreateView& view, int stat) {
    if (view.option < 0 || view.option >= static_cast<int>(view.options.size()) || stat < 0 || stat >= 7) return 0;
    int base = create_value(view.options[static_cast<std::size_t>(view.option)].base[static_cast<std::size_t>(stat)]);
    return std::min(base + 25, 150);
}

bool create_stat_up(CharacterCreateView& view, int stat) {
    if (view.stats_for != view.option || stat < 0 || stat >= 7) return false;
    auto& v = view.stats[static_cast<std::size_t>(stat)];
    if (view.points_left <= 0 || v + 1 > create_stat_cap(view, stat)) return false;
    ++v, --view.points_left;
    return true;
}

bool create_stat_down(CharacterCreateView& view, int stat) {
    if (view.stats_for != view.option || view.option < 0 || stat < 0 || stat >= 7) return false;
    auto& v = view.stats[static_cast<std::size_t>(stat)];
    if (v - 1 < create_value(view.options[static_cast<std::size_t>(view.option)].base[static_cast<std::size_t>(stat)]))
        return false;
    --v, ++view.points_left;
    return true;
}

// ---- Loading --------------------------------------------------------------

void loading_screen(const LoadingView& view) {
    center_next_window(24.0f);
    ImGui::Begin("##hailstone_loading", nullptr,
                 kDialogFlags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    float width = ImGui::GetFontSize() * 22.0f;
    ImGui::TextUnformatted(view.title.c_str());
    if (!view.detail.empty()) ImGui::TextColored(kDimColor, "%s", view.detail.c_str());
    if (!view.waiting) {
        ImGui::End();
        return;
    }
    ImGui::Spacing();
    if (view.progress >= 0.0f) {
        float p = std::clamp(view.progress, 0.0f, 1.0f);
        ImGui::ProgressBar(p, ImVec2(width, 0.0f), std::format("{:.0f}%", p * 100.0f).c_str());
    } else {
        spinner(ImGui::GetFontSize() * 0.6f);
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(width - ImGui::GetFontSize() * 1.2f - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    }
    ImGui::End();
}

void loading_overlay(const LoadingView& view) {
    // Drawn into the foreground list, over every window of the frame (the UI pack's windows draw beneath it), on
    // the loading screen's background, with loading_screen's panel: title, detail, bar.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (view.picture.id) draw_screen_picture(dl, view.picture);
    else dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, 255));
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    const float width = font * 22.0f;
    const float bar_h = font + style.FramePadding.y * 2.0f;
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const bool bar = view.waiting && view.progress >= 0.0f;
    float inner_h = line + (view.detail.empty() ? 0.0f : line) + (view.waiting ? style.ItemSpacing.y + bar_h : 0.0f);
    ImVec2 size(width + style.WindowPadding.x * 2.0f, inner_h + style.WindowPadding.y * 2.0f);
    ImVec2 c = vp->GetCenter();
    ImVec2 p0(std::floor(c.x - size.x * 0.5f), std::floor(c.y - size.y * 0.5f));
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImVec4 bg = style.Colors[ImGuiCol_WindowBg];
    bg.w = 1.0f;
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(bg), style.WindowRounding);
    if (style.WindowBorderSize > 0.0f)
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), style.WindowRounding, 0, style.WindowBorderSize);
    ImVec2 at(p0.x + style.WindowPadding.x, p0.y + style.WindowPadding.y);
    dl->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), view.title.c_str());
    at.y += line;
    if (!view.detail.empty()) {
        dl->AddText(at, ImGui::ColorConvertFloat4ToU32(kDimColor), view.detail.c_str());
        at.y += line;
    }
    if (!view.waiting) return;
    at.y += style.ItemSpacing.y;
    if (bar) {
        float p = std::clamp(view.progress, 0.0f, 1.0f);
        ImVec2 b1(at.x + width, at.y + bar_h);
        dl->AddRectFilled(at, b1, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        if (p > 0.0f)
            dl->AddRectFilled(at, ImVec2(at.x + width * p, b1.y), ImGui::GetColorU32(ImGuiCol_PlotHistogram),
                              style.FrameRounding);
        std::string label = std::format("{:.0f}%", p * 100.0f);
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        // ImGui::ProgressBar's place for the label: just past the fill, kept inside the bar.
        float x = std::clamp(at.x + width * p + style.ItemSpacing.x, at.x, b1.x - ts.x - style.ItemInnerSpacing.x);
        dl->AddText(ImVec2(x, at.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    } else {
        float r = font * 0.6f;
        ImVec2 sc(at.x + r, at.y + r);
        float t = static_cast<float>(ImGui::GetTime()) * 6.0f;
        constexpr float kPi = std::numbers::pi_v<float>;
        dl->PathArcTo(sc, r * 0.8f, t, t + kPi * 1.5f, 24);
        dl->PathStroke(ImGui::GetColorU32(ImGuiCol_ButtonActive), 0, r * 0.3f);
    }
}

// ---- In game --------------------------------------------------------------

void ChatView::add(ChatChannel channel, std::string text, std::uint32_t rgb, std::uint32_t color, std::string peer) {
    lines.push_back({channel, std::move(text), rgb, color, std::move(peer), 0, false,
                     static_cast<std::int64_t>(std::time(nullptr))});
    lines.back().seq = ++revision;
    while (lines.size() > max_lines) lines.pop_front();
    scroll_to_bottom = true;
}

void hud_panel(const HudView& view, bool& show_chat, bool& show_spawns, InGameActions& actions) {
    float pad = ImGui::GetFontSize() * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("##hailstone_hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted(view.zone.empty() ? "(no zone)" : view.zone.c_str());
    ImGui::Separator();
    ImGui::Text("Loc  %.1f, %.1f, %.1f", view.y, view.x, view.z);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("In /loc order: y, x, z");
    ImGui::Text("Heading  %.0f deg", view.heading);
    ImGui::Text("FPS  %.0f", view.fps);
    ImGui::TextUnformatted("Link ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(view.connected ? kGoodColor : kErrorColor, "%s", view.connection.c_str());
    ImGui::Separator();
    ImGui::Checkbox("Chat", &show_chat);
    ImGui::SameLine();
    ImGui::Checkbox("Spawns", &show_spawns);
    if (ImGui::Button("Camp")) actions.camp = true;
    ImGui::SameLine();
    // Dropping the connection (no camp) asks first: the panel sits where game windows are often placed, and a
    // stray click on it sent the player straight back to the login screen.
    static bool confirm_disconnect = false;
    if (!confirm_disconnect) {
        if (ImGui::Button("Disconnect")) confirm_disconnect = true;
    } else {
        if (ImGui::Button("Really disconnect")) {
            actions.disconnect = true;
            confirm_disconnect = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) confirm_disconnect = false;
    }
    ImGui::End();
}

namespace {

// A chat line with item links: the names are shown in a link colour and a
// click asks for the item (CChatWindow::WndNotification, XWM_LINK). Such
// lines are not wrapped.
void chat_line_with_links(const std::string& text, ChatView& view) {
    bool first = true;
    for (auto& seg : game::split_links(text)) {
        if (!first) ImGui::SameLine(0.0f, 0.0f);
        first = false;
        if (!seg.link) {
            ImGui::TextUnformatted(seg.text.c_str());
            continue;
        }
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s", seg.text.c_str());
        if (ImGui::IsItemHovered()) {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, IM_COL32(115, 190, 255, 255));
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemClicked()) view.clicked_link = seg.link;
    }
}

} // namespace

std::optional<std::string> chat_window(ChatView& view, bool* open) {
    std::optional<std::string> submitted;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImVec2 size(std::min(36.0f * em, vp->WorkSize.x * 0.6f), std::min(14.0f * em, vp->WorkSize.y * 0.4f));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + em * 0.6f, vp->WorkPos.y + vp->WorkSize.y - size.y - em * 0.6f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (!ImGui::Begin("Chat##hailstone", open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return submitted;
    }
    float input_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("scrollback", ImVec2(0.0f, -input_h), ImGuiChildFlags_None);
    bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    // Every line is drawn (the history is capped): wrapped lines have different heights, which a
    // ImGuiListClipper (one height for all) cannot place.
    ImGui::PushTextWrapPos(0.0f);
    for (const ChatLine& l : view.lines) {
        ImGui::PushStyleColor(ImGuiCol_Text, l.rgb ? rgb_color(l.rgb) : channel_color(l.channel));
        if (l.text.find('\x12') == std::string::npos) ImGui::TextUnformatted(l.text.c_str());
        else chat_line_with_links(l.text, view);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (view.scroll_to_bottom && at_bottom) ImGui::SetScrollHereY(1.0f);
    view.scroll_to_bottom = false;
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##chatinput", "Type here, Enter to send", &view.input,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (!view.input.empty()) submitted = std::move(view.input);
        view.input.clear();
        ImGui::SetKeyboardFocusHere(-1); // keep typing
        view.scroll_to_bottom = true;
    }
    ImGui::End();
    return submitted;
}

std::optional<std::uint32_t> spawn_list_window(SpawnListView& view, bool* open) {
    std::optional<std::uint32_t> clicked;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImVec2 size(18.0f * em, std::min(22.0f * em, vp->WorkSize.y * 0.6f));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - em * 0.6f, vp->WorkPos.y + em * 0.6f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    std::string title = std::format("Spawns ({})###hailstone_spawns", view.spawns.size());
    if (!ImGui::Begin(title.c_str(), open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return clicked;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter by name", &view.filter);

    // Nearest first.
    std::vector<const SpawnEntry*> rows;
    rows.reserve(view.spawns.size());
    for (const SpawnEntry& s : view.spawns)
        if (contains_ci(s.name, view.filter)) rows.push_back(&s);
    std::sort(rows.begin(), rows.end(), [](const SpawnEntry* a, const SpawnEntry* b) { return a->distance < b->distance; });

    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("spawns", 3, kTable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Lvl", ImGuiTableColumnFlags_WidthFixed, em * 2.0f);
        ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, em * 3.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const SpawnEntry& s = *rows[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(s.id));
                if (ImGui::Selectable(s.name.c_str(), view.selected_id == s.id, ImGuiSelectableFlags_SpanAllColumns)) {
                    view.selected_id = s.id;
                    clicked = s.id;
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%d", s.level);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", s.distance);
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
    return clicked;
}

namespace {

// A labelled bar: fraction 0..1, text over it.
void points_bar(const char* label, float fraction, ImVec4 color, const std::string& text) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 3.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), text.c_str());
    ImGui::PopStyleColor();
}

std::string points_text(std::int64_t cur, std::int64_t max) {
    return max > 0 ? std::format("{} / {}", cur, max) : std::format("{}", cur);
}

} // namespace

void player_window(const PlayerView& view, InGameActions& actions) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + em * 0.6f, vp->WorkPos.y + em * 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(16.0f * em, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("Player##hailstone_player", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%s", view.name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kDimColor, "level %d", view.level);
    if (view.dead) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "(dead)");
    }
    auto frac = [](std::int64_t cur, std::int64_t max) {
        return max > 0 ? static_cast<float>(cur) / static_cast<float>(max) : (cur > 0 ? 1.0f : 0.0f);
    };
    points_bar("HP", frac(view.hp, view.max_hp), ImVec4(0.80f, 0.18f, 0.18f, 1.0f), points_text(view.hp, view.max_hp));
    if (view.max_mana > 0 || view.mana > 0)
        points_bar("Mana", frac(view.mana, view.max_mana), ImVec4(0.20f, 0.35f, 0.90f, 1.0f),
                   points_text(view.mana, view.max_mana));
    points_bar("End", frac(view.endurance, view.max_endurance), ImVec4(0.85f, 0.75f, 0.15f, 1.0f),
               points_text(view.endurance, view.max_endurance));
    points_bar("Exp", static_cast<float>(view.exp) / 330.0f, ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
               std::format("{:.1f}%", static_cast<float>(view.exp) * 100.0f / 330.0f));
    if (ImGui::Button(view.sitting ? "Stand" : "Sit")) actions.toggle_sit = true;
    if (view.attacking) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "Attacking");
    }
    ImGui::End();
}

void target_window(const TargetView& view, bool attacking, InGameActions& actions) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f - 8.0f * em, vp->WorkPos.y + em * 0.6f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(16.0f * em, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("Target##hailstone_target", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    if (!view.has_target) {
        ImGui::TextColored(kDimColor, "No target");
        ImGui::TextColored(kDimColor, "Click a spawn, Tab / F8 nearest NPC, F1 yourself.");
    } else {
        ImGui::TextColored(rgb_color(view.con_rgb), "%s", view.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kDimColor, "L%d  %.0f away", view.level, view.distance);
        points_bar("HP", static_cast<float>(view.hp_percent) / 100.0f, ImVec4(0.80f, 0.18f, 0.18f, 1.0f),
                   std::format("{}%", view.hp_percent));
        ImGui::BeginDisabled(view.is_corpse || view.is_self);
        if (ImGui::Button(attacking ? "Stop attack" : "Attack")) actions.toggle_attack = true;
        ImGui::SameLine();
        if (ImGui::Button("Consider")) actions.consider = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) actions.clear_target = true;
    }
    ImGui::End();
}

void respawn_window(const RespawnView& view, InGameActions& actions) {
    if (view.options.empty()) return;
    center_next_window(20.0f);
    ImGui::Begin("You have died##hailstone_respawn", nullptr, kDialogFlags | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Respawn in %u s, or choose:", view.seconds_left);
    ImGui::BeginDisabled(!view.ready);
    for (const auto& [id, label] : view.options)
        if (ImGui::Button(label.c_str())) actions.respawn = id;
    ImGui::EndDisabled();
    ImGui::End();
}

InGameActions in_game_overlay(InGameView& view) {
    InGameActions actions;
    if (view.dev_windows) hud_panel(view.hud, view.show_chat, view.show_spawns, actions);
    // Each off while its Lua window (ui/<pack>) stands in for it.
    if (!lua::replaces("PlayerWindow")) player_window(view.player, actions);
    if (!lua::replaces("TargetWindow")) target_window(view.target, view.player.attacking, actions);
    if (!lua::replaces("RespawnWnd")) respawn_window(view.respawn, actions);
    if (view.show_chat && !lua::replaces("Chat1")) actions.chat_submit = chat_window(view.chat, &view.show_chat);
    if (view.dev_windows && view.show_spawns) actions.target_spawn = spawn_list_window(view.spawns, &view.show_spawns);
    return actions;
}

} // namespace hailstone::ui
