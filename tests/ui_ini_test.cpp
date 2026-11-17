// Window places from the UI ini files (ui/ui_ini.h): CSidlScreenWnd::ConvertToRes and LoadIniInfo's order.
#include <doctest/doctest.h>

#include "ui/ui_ini.h"

#include <fstream>
#include <sstream>

#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("convert_to_res: left, middle and right anchoring") {
    // Left of the middle: unchanged.
    CHECK(ui::convert_to_res(17, 100, 4096, 1280) == 17);
    // Right of the middle: the same gap to the right edge.
    CHECK(ui::convert_to_res(3984, 112, 4096, 1280) == 1168);
    CHECK(ui::convert_to_res(3749, 52, 4096, 720) == 373);
    // Over the middle: the same offset from the middle.
    CHECK(ui::convert_to_res(1968, 160, 4096, 1280) == 560);
    // Stored at 1280 x 720 (StoreIniPosition): a window at the top over the vertical middle.
    CHECK(ui::convert_to_res(0, 395, 720, 4096) == 1688);
    CHECK(ui::convert_to_res(1688, 395, 4096, 720) == 0);
    // Same screen: only pulled back on screen when it was on screen.
    CHECK(ui::convert_to_res(1000, 200, 1280, 1280) == 1000);
    CHECK(ui::convert_to_res(1200, 200, 1280, 1280) == 1200); // already past the edge: kept
    CHECK(ui::convert_to_res(-50, 200, 1280, 1280) == -50);
    CHECK(ui::convert_to_res(1400, 200, 1280, 1280) == 1080);
}

// A UI.ini value near the int limits (hand-edited or broken): the sums wrap in 32 bits as the original's do, with no
// signed overflow.
TEST_CASE("convert_to_res: values near the int limits wrap as the original's 32-bit sums") {
    CHECK(ui::convert_to_res(2147483600, 200, 4096, 1280) == 1080);
    CHECK(ui::convert_to_res(2147483600, 200, 1280, 1280) == 0);
    CHECK(ui::convert_to_res(-2147483600, -200, 4096, 1280) == 1480);
}

TEST_CASE("window_ini_info: the window classes' IniFlags and versions") {
    auto p = ui::window_ini_info("PlayerWindow");
    CHECK(p.flags == 0xffffffffu);
    CHECK(p.section == "PlayerWindow");
    auto inv = ui::window_ini_info("InventoryWindow");
    CHECK(inv.flags == 0xfffffcffu);
    CHECK(inv.version == 2);
    auto hb = ui::window_ini_info("HotButtonWnd3");
    CHECK(hb.flags == 0xfffffdffu);
    CHECK(hb.version == 2);
    CHECK(ui::window_ini_info("Chat1").section == "MainChat");
    // Hailstone: the aura window keeps its Show but starts closed (CAuraWnd's 0xffffffff has bit 9, bInitVisibility).
    CHECK(ui::window_ini_info("AuraWindow").flags == 0xfffffdffu);
    CHECK((ui::window_ini_info("ExtendedTargetWnd").flags & 0x200u) != 0);
    const auto addons = ui::window_ini_info("AddonPolicy");
    CHECK(addons.section == "AddonPolicy");
    CHECK((addons.flags & 0x300u) == 0x300u); // remember visibility and show on first use
    CHECK(ui::window_ini_info("LootWnd").flags == 0xfffffcffu); // CSidlScreenWnd2
    // CItemDisplayManager::CreateWindowInstance: "ItemDisplay%d" by the displays made before.
    auto id = ui::window_ini_info("ItemDisplayWindow");
    CHECK(id.section == "ItemDisplay0");
    CHECK(id.flags == 0xfffffeffu);
    CHECK(ui::window_ini_info("ItemDisplayWindow#3").section == "ItemDisplay2");
}

TEST_CASE("UiIni: defaults.ini under the character's file, windowed keys on a 4096 screen") {
    ui::UiIni ini;
    ini.load("/nonexistent/defaults.ini", "/nonexistent/UI_x_y.ini");
    ini.defaults().parse("[PlayerWindow]\nShow=1\nXPosWindowed=1968\nYPosWindowed=3749\n"
                         "[HotButtonWnd]\nShow=1\nXPosWindowed=3212\nYPosWindowed=3749\nWidthWindowed=506\nHeightWindowed=52\n"
                         "[ActionsWindow]\nShow=1\n");
    auto player = ui::window_ini_info("PlayerWindow");
    auto r = ini.place(player, {100, 100, 160, 90}, 1280, 720, true);
    CHECK(r.x == 560);
    CHECK(r.y == 373);
    CHECK(r.w == 160);
    // Show comes from defaults.ini only for "HotButtonWnd".
    CHECK_FALSE(ini.shown(player));
    CHECK_FALSE(ini.shown(ui::window_ini_info("ActionsWindow")));
    CHECK(ini.shown(ui::window_ini_info("HotButtonWnd")) == true);
    auto hb = ini.place(ui::window_ini_info("HotButtonWnd"), {0, 0, 600, 60}, 1920, 1080, true);
    CHECK(hb.x == 1036);
    CHECK(hb.y == 733);
    CHECK(hb.w == 506);
    CHECK(hb.h == 52);
    // Not sizable: the XML size stays.
    auto fixed = ini.place(ui::window_ini_info("HotButtonWnd"), {0, 0, 600, 60}, 1920, 1080, false);
    CHECK(fixed.w == 600);

    // The character's file wins; its Show counts.
    ini.file().parse("[PlayerWindow]\nINIVersion=1\nShow=0\nXPosWindowed=10\nYPosWindowed=20\n");
    r = ini.place(player, {100, 100, 160, 90}, 1280, 720, true);
    CHECK(r.x == 10);
    CHECK(r.y == 20);
    CHECK(ini.shown(player) == false);

    // An older INIVersion in the file than the window's: the file is not used (defaults.ini still is).
    auto inv = ui::window_ini_info("InventoryWindow");
    ini.file().parse("[InventoryWindow]\nINIVersion=1\nXPosWindowed=10\nYPosWindowed=20\n");
    ini.defaults().parse("[InventoryWindow]\nXPosWindowed=186\nYPosWindowed=383\n");
    r = ini.place(inv, {0, 0, 394, 300}, 1280, 720, true);
    CHECK(r.x == 186);
    CHECK(r.y == 383);

    // At least 4 pixels stay on screen.
    ini.file().parse("[PlayerWindow]\nXPosWindowed=4090\n");
    r = ini.place(player, {0, 0, 160, 90}, 1280, 720, true);
    CHECK(r.x == 1274); // off screen where it was stored: moved, not pulled back
}

TEST_CASE("UiIni: store writes the windowed keys and the shown state") {
    ui::UiIni ini;
    ini.load("/nonexistent/defaults.ini", "/nonexistent/UI_x_y.ini");
    auto player = ui::window_ini_info("PlayerWindow");
    ini.store(player, {560, 373, 160, 90}, 1280, 720, true);
    CHECK(ini.file().get("PlayerWindow", "XPosWindowed") == "1968");
    CHECK(ini.file().get("PlayerWindow", "YPosWindowed") == "3749");
    CHECK(ini.file().get("PlayerWindow", "WidthWindowed") == "160");
    CHECK(ini.file().get("PlayerWindow", "INIVersion") == "1");
    ini.store_shown(player, false);
    CHECK(ini.file().get("PlayerWindow", "Show") == "0");
    // No shown state for windows without IniFlags bit 8.
    ini.store_shown(ui::window_ini_info("InventoryWindow"), true);
    CHECK_FALSE(ini.file().get("InventoryWindow", "Show"));
    CHECK(ini.dirty());
}

TEST_CASE("UiIni: changes are written at most every 2 s, however many frames pass") {
    test::ScratchDir dir("ui_ini_save");
    auto file = dir.path() / "UI.ini";
    ui::UiIni ini;
    ini.load(dir.path() / "defaults.ini", file);
    auto read = [&] {
        std::ifstream in(file);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    };
    auto t = std::chrono::steady_clock::now();
    ini.put("Test", "A", "1");
    CHECK(ini.save_if_due(t)); // the first change goes out at once
    CHECK(read().find("A=1") != std::string::npos);
    // A thousand frames a second for 1.9 s: nothing more is written.
    bool wrote = false;
    for (int ms = 1; ms < 1900; ++ms) {
        ini.put("Test", "A", std::to_string(ms));
        wrote = ini.save_if_due(t + std::chrono::milliseconds(ms)) || wrote;
    }
    CHECK_FALSE(wrote);
    CHECK(read().find("A=1\n") != std::string::npos);
    CHECK(ini.save_if_due(t + std::chrono::milliseconds(2000)));
    CHECK(read().find("A=1899") != std::string::npos);
    // Nothing changed: nothing to write.
    CHECK_FALSE(ini.save_if_due(t + std::chrono::milliseconds(5000)));
}
