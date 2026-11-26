#pragma once

// Developer UI demo (--ui-demo): shows each dev screen with fake data so the
// screens can be checked without a server. F1-F6 switch screens; the buttons
// walk through a fake login -> server -> character -> loading -> zone flow.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "core/config.h"

#include "game/inventory.h"
#include "ui/dev_screens.h"
#include "ui/item_windows.h"

namespace hailstone::app {

enum class DemoScreen { Login, Servers, Characters, NoCharacters, Loading, InGame, Items,
                        ClientChat, ClientChatExpanded };

// Parses the --ui-demo screen names.
bool parse_demo_screen(std::string_view name, DemoScreen& out);

struct DemoFrameInput {
    float dt = 0.0f;
    std::string zone;            // shown in the in-game panel
    float x = 0.0f, y = 0.0f, z = 0.0f; // camera position, EQ world coordinates
    float heading_deg = 0.0f;
};

class UiDemo {
public:
    explicit UiDemo(DemoScreen start);
    ~UiDemo();
    UiDemo(const UiDemo&) = delete;
    UiDemo& operator=(const UiDemo&) = delete;

    // Draws the current screen. Call between ImGuiLayer::new_frame and render.
    void draw(const DemoFrameInput& in);
    bool quit_requested() const { return quit_; }
    // Item icons for the items screen (optional; needs the game files).
    void set_item_icons(ui::ItemIconCache* icons) { item_icons_ = icons; }

private:
    struct ModernChat;
    void show(DemoScreen s);
    void favorite_world(int row); // marks or unmarks a row and sorts the list again (not kept: a demo)

    DemoScreen screen_;
    // The login screen shows copies of the saved servers (their server.ini files in a scratch folder, removed with
    // the demo) and its own copy of hailstone.ini, never saved: Save, Delete and logging in change only those.
    std::filesystem::path scratch_;
    Config config_;
    ui::LoginView login_;
    ui::ServerSelectView servers_;
    ui::CharacterSelectView chars_;
    ui::CharacterSelectView no_chars_;
    ui::LoadingView loading_;
    ui::InGameView game_;
    float timer_ = 0.0f; // fake request / loading time
    bool loading_running_ = false;
    bool quit_ = false;
    std::unique_ptr<ModernChat> modern_chat_;
    // Items screen: a fake inventory and corpse.
    void init_items();
    game::Inventory items_inv_;
    ui::ItemWindowsState items_state_;
    std::map<std::int16_t, net::Item> items_loot_;
    ui::ItemIconCache* item_icons_ = nullptr;
};

} // namespace hailstone::app
