// ui::keyboard_for_ui (ui/imgui_layer.h): a press the windows take leaves the keys the game's, so held movement keys
// stay held; an edit box, a modal dialog and an explicit request still take them.
#include <doctest/doctest.h>

#include <functional>
#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>

#include "ui/imgui_layer.h"
#include "app/ui_visibility.h"
#include "app/actions_ui.h"
#include "core/config.h"
#include "platform/window.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

struct Rects {
    ImVec2 button, edit; // centres
};

class Ui {
public:
    Ui() {
        ctx_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    }
    ~Ui() { ImGui::DestroyContext(ctx_); }
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // One frame with the mouse there; returns whether the keys were the UI's in it (as ImGuiLayer::wants_keyboard).
    bool frame(ImVec2 mouse, bool down, const std::function<void()>& extra = {}) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        const int requested = GImGui->WantCaptureKeyboardNextFrame;
        ui::keep_tab_from_windows(); // (as ImGuiLayer::begin_frame)
        ImGui::NewFrame();
        const bool keys = ui::keyboard_for_ui(requested);
        imgui_captured = io.WantCaptureKeyboard;
        ImGui::SetNextWindowPos(ImVec2(100, 100));
        ImGui::SetNextWindowSize(ImVec2(300, 200));
        ImGui::Begin("Window", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::Button("Go", ImVec2(120, 30));
        rects.button = centre();
        ImGui::InputText("##edit", text_, sizeof(text_));
        rects.edit = centre();
        edit_active = ImGui::IsItemActive();
        if (extra) extra();
        ImGui::End();
        ImGui::Render();
        return keys;
    }
    // Pressed for two frames (the item is active from the press's frame on), then released.
    bool hold(ImVec2 at) {
        frame(at, false);
        frame(at, true);
        bool keys = frame(at, true);
        frame(at, false);
        return keys;
    }

    Rects rects;
    bool imgui_captured = false;
    bool edit_active = false; // the edit box being typed in, in the last frame

private:
    static ImVec2 centre() {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    }
    ImGuiContext* ctx_ = nullptr;
    char text_[32] = {};
};

const ImVec2 kAway{900, 700};

} // namespace

TEST_CASE("UI keyboard: a button or window held by the mouse leaves the keys the game's") {
    Ui ui;
    CHECK_FALSE(ui.frame(kAway, false));
    // A button held down: ImGui would take the keyboard (its active item), the game keeps it.
    ui.frame(ui.rects.button, false);
    ui.frame(ui.rects.button, true);
    CHECK_FALSE(ui.frame(ui.rects.button, true));
    CHECK(ui.imgui_captured);
    ui.frame(ui.rects.button, false);
    CHECK_FALSE(ui.frame(kAway, false));
    // The title bar (a window drag) and the window's background.
    CHECK_FALSE(ui.hold(ImVec2(250, 105)));
    CHECK_FALSE(ui.hold(ImVec2(350, 280)));
}

TEST_CASE("UI keyboard: an edit box being typed in, a modal dialog and an explicit request take the keys") {
    Ui ui;
    ui.frame(kAway, false);
    // A click into the edit box: it keeps the keys after the button is let go.
    ui.hold(ui.rects.edit);
    CHECK(ui.frame(kAway, false));
    // A click elsewhere ends the typing.
    ui.hold(kAway);
    CHECK_FALSE(ui.frame(kAway, false));

    // A frame that asks for the keys (ui.CaptureKeyboard) has them in the next.
    ui.frame(kAway, false, [] { ImGui::SetNextFrameWantCaptureKeyboard(true); });
    CHECK(ui.frame(kAway, false));
    CHECK_FALSE(ui.frame(kAway, false));

    // A modal dialog open.
    auto modal = [] {
        if (!ImGui::IsPopupOpen("Ask")) ImGui::OpenPopup("Ask");
        if (ImGui::BeginPopupModal("Ask", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("?");
            ImGui::EndPopup();
        }
    };
    ui.frame(kAway, false, modal);
    CHECK(ui.frame(kAway, false, modal));
}

TEST_CASE("UI keys: Ctrl is Ctrl in the windows on every system (no macOS Cmd swap)") {
    Ui ui;
    ui::configure_keys();
    ImGuiIO& io = ImGui::GetIO();
    CHECK_FALSE(io.ConfigMacOSXBehaviors);
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.0f / 60.0f;
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::NewFrame();
    CHECK(io.KeyCtrl);
    CHECK_FALSE(io.KeySuper);
    ImGui::EndFrame();
}

TEST_CASE("UI gamepad: opening a combo keeps focus within the tracked window") {
    Ui ui;
    CHECK_FALSE(ui::detail::pad_window_has_focus("Window"));
    ui.frame(kAway, false, [] {
        ImGui::SetWindowFocus();
        CHECK(ui::detail::pad_window_has_focus("Window"));

        // Open the native combo so its real popup window takes navigation focus.
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID("Map Pack")), ImGuiPopupFlags_None);
        const bool open = ImGui::BeginCombo("Map Pack", "Default");
        CHECK(open);
        if (open) {
            CHECK(GImGui->NavWindow == ImGui::GetCurrentWindow());
            CHECK(GImGui->NavWindow->RootWindow != ImGui::FindWindowByName("Window"));
            CHECK(ui::detail::pad_window_has_focus("Window"));
            ImGui::Selectable("Default", true);
            ImGui::EndCombo();
        }

        ImGui::Begin("Other window", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetWindowFocus();
        CHECK_FALSE(ui::detail::pad_window_has_focus("Window"));
        CHECK(ui::detail::pad_window_has_focus("Other window"));
        ImGui::End();
    });
}

TEST_CASE("UI keys: Tab with nothing typed in stays the game's; in an edit box it moves to the next field") {
    Ui ui;
    ImGuiIO& io = ImGui::GetIO();
    bool second_active = false;
    auto second = [&] {
        static char more[16] = {};
        ImGui::InputText("##more", more, sizeof(more));
        second_active = ImGui::IsItemActive();
    };
    auto tap_tab = [&] {
        io.AddKeyEvent(ImGuiKey_Tab, true);
        ui.frame(kAway, false, second);
        io.AddKeyEvent(ImGuiKey_Tab, false);
        for (int i = 0; i < 3; ++i) ui.frame(kAway, false, second);
    };
    // The window focused by a click on its background, nothing active: the Tab does not open the edit box (the chat
    // input took the keys this way).
    ui.frame(kAway, false, second);
    ui.hold(ImVec2(350, 280));
    REQUIRE(GImGui->NavWindow != nullptr);
    tap_tab();
    CHECK_FALSE(ui.edit_active);
    CHECK_FALSE(second_active);
    CHECK(GImGui->ActiveId == 0);

    // Typing in the edit box: Tab goes on to the next field, as before.
    ui.hold(ui.rects.edit);
    ui.frame(kAway, false, second);
    REQUIRE(ui.edit_active);
    tap_tab();
    CHECK_FALSE(ui.edit_active);
    CHECK(second_active);
}

TEST_CASE("UI visibility key: F10 remains available during keyboard capture without starting game actions") {
    test::ScratchDir dir("ui_visibility_key");
    Config config;
    app::ActionsUi actions(config, dir.path() / "hailstone.ini");
    const int toggle = static_cast<int>(game::KeyCommand::FULLSCREEN);
    REQUIRE(actions.keymap().binding(toggle, 0) == game::KeyCombo{0x44});
    CHECK_FALSE(actions.keymap().binding(toggle, 1).bound());
    CHECK(game::command_label_eqstr(toggle) == 3153u); // Toggle UI Display
    CHECK(game::command_category(toggle) == 0x40u); // Options > Keys > Windows
    CHECK(std::find(game::listed_commands().begin(), game::listed_commands().end(), toggle) != game::listed_commands().end());
    platform::InputState input;
    input.key_events = {{0x11, true, false}}; // held W before an edit box takes the keyboard
    const auto movement = actions.process_keys(input, true);
    REQUIRE(std::any_of(movement.begin(), movement.end(), [](const auto& event) {
        return event.command == static_cast<int>(game::KeyCommand::FORWARD) && event.down;
    }));
    input.key_events = {{0x44, true, false}, {0x44, true, true}, {0x10, true, false}}; // F10, repeat, Q
    auto events = actions.process_keys(input, false);
    CHECK(std::count_if(events.begin(), events.end(), [&](const auto& event) { return event.command == toggle && event.down; }) == 1);
    CHECK(std::none_of(events.begin(), events.end(), [&](const auto& event) { return event.down && event.command != toggle; }));
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.command == static_cast<int>(game::KeyCommand::FORWARD) && !event.down;
    }));
    CHECK_FALSE(actions.keymap().held(game::KeyCommand::FORWARD));
    input.key_events = {{0x44, true, true}, {0x44, false, false}};
    CHECK(actions.process_keys(input, false).empty()); // neither holding nor releasing toggles again
    input.key_events = {{0x44, true, false}};
    events = actions.process_keys(input, true);
    REQUIRE(events.size() == 1);
    CHECK(events.front().command == toggle); CHECK(events.front().down);
    input.key_events = {{0x44, false, false}};
    events = actions.process_keys(input, true);
    REQUIRE(events.size() == 1);
    CHECK(events.front().command == toggle); CHECK_FALSE(events.front().down);
    input.key_events = {{0x44, true, false}};
    events = actions.process_keys(input, true);
    REQUIRE(events.size() == 1);
    CHECK(events.front().command == toggle); CHECK(events.front().down); // a second press can restore the UI
}

TEST_CASE("UI visibility key: Options capture, both remapped banks and reset persist through the real key map") {
    test::ScratchDir dir("ui_visibility_remap");
    const auto path = dir.path() / "hailstone.ini";
    Config config;
    app::ActionsUi actions(config, path);
    const int toggle = static_cast<int>(game::KeyCommand::FULLSCREEN);
    platform::InputState input;
    // Changing another action to F10 consumes it rather than hiding the key-binding editor.
    actions.capture_key(static_cast<int>(game::KeyCommand::INVENTORY), 1, true);
    input.key_events = {{0x44, true, false}};
    CHECK(actions.process_keys(input, false).empty());
    CHECK(actions.keymap().binding(static_cast<int>(game::KeyCommand::INVENTORY), 1) == game::KeyCombo{0x44});
    CHECK(actions.key_capture().first == -1);
    actions.capture_key(toggle, 0, true);
    input.key_down[0x1d] = true; input.key_events = {{0x57, true, false}}; // Ctrl+F11
    CHECK(actions.process_keys(input, false).empty());
    actions.capture_key(toggle, 1, true);
    input.key_down[0x1d] = false; input.key_down[0x2a] = true; input.key_events = {{0x58, true, false}}; // Shift+F12
    CHECK(actions.process_keys(input, false).empty());
    Config saved;
    REQUIRE(saved.load(path));
    CHECK(saved.get_int("KEYMAPPING_FULLSCREEN_1", 0) == 0x20000057);
    CHECK(saved.get_int("KEYMAPPING_FULLSCREEN_2", 0) == 0x40000058);
    app::ActionsUi restored(saved, path);
    input.key_down.fill(false); input.key_events = {{0x44, true, false}};
    CHECK(restored.process_keys(input, false).empty()); // no hardcoded F10 escape after remapping
    input.key_down[0x1d] = true; input.key_events = {{0x57, true, false}};
    auto events = restored.process_keys(input, false);
    REQUIRE(events.size() == 1);
    CHECK(events.front().command == toggle); CHECK(events.front().down);
    input.key_down[0x1d] = false; input.key_events = {{0x57, true, false}};
    CHECK(restored.process_keys(input, false).empty()); // modifiers remain part of the binding
    input.key_down[0x2a] = true; input.key_events = {{0x58, true, false}};
    events = restored.process_keys(input, false);
    REQUIRE(events.size() == 1);
    CHECK(events.front().command == toggle); CHECK(events.front().down);
    restored.capture_key(toggle, 0, true);
    input.key_down.fill(false); input.key_events = {{0x01, true, false}};
    CHECK(restored.process_keys(input, false).empty());
    CHECK_FALSE(restored.keymap().binding(toggle, 0).bound());
    Config cleared;
    REQUIRE(cleared.load(path));
    CHECK(cleared.get_int("KEYMAPPING_FULLSCREEN_1", -1) == 0);
    restored.reset_keys();
    CHECK(restored.keymap().binding(toggle, 0) == game::KeyCombo{0x44});
    CHECK_FALSE(restored.keymap().binding(toggle, 1).bound());
    Config defaults;
    REQUIRE(defaults.load(path));
    CHECK_FALSE(defaults.get("KEYMAPPING_FULLSCREEN_1"));
    CHECK_FALSE(defaults.get("KEYMAPPING_FULLSCREEN_2"));
}

// F10's hide is the world's only, and a question only the player may answer (the compiled-plugin consent) shows the
// interface while it waits (app/ui_visibility.h).
TEST_CASE("UI visibility: F10 hides in the world only, and a waiting consent question shows the interface") {
    using hailstone::app::ui_shown;
    CHECK(ui_shown(false, true, false, false));
    CHECK_FALSE(ui_shown(true, true, false, false));
    CHECK(ui_shown(true, false, false, false)); // pre-game and loading screens
    CHECK(ui_shown(true, true, true, false));   // a zone still settling
    CHECK(ui_shown(true, true, false, true));   // the consent question waits
}
