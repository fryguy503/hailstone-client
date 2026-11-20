// More stock windows over the hs API (app/lua_ui_names.cpp, ...): the name change windows.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>
#include <map>
#include <memory>
#include <string>

#include <imgui.h>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/ini_file.h"
#include "game_files.h"
#include "net/game_connection.h"
#include "net/opcodes.h"
#include "ui/dev_screens.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Harness {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch{"ui_more_windows_test"};
    fs::path dir = dir_scratch.path();
    net::GameConnection conn;
    app::UiHost host;
    std::unique_ptr<app::UiApi> api;
    std::map<std::string, std::string> char_ini; // "section/key" -> value
    explicit Harness(const GameDir& game) : conn(game, {}) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        api = std::make_unique<app::UiApi>(host);
        api->load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
        conn.set_character_ini(
            [this](std::string_view s, std::string_view k, int fallback) {
                auto it = char_ini.find(std::string(s) + "/" + std::string(k));
                return it == char_ini.end() ? fallback : std::atoi(it->second.c_str());
            },
            [this](std::string_view s, std::string_view k, const std::string& v) {
                char_ini[std::string(s) + "/" + std::string(k)] = v;
            });
    }
    ~Harness() {
        api.reset();
        ImGui::DestroyContext(ctx);
        fs::remove_all(dir);
    }
    void packet(net::Op op, std::vector<std::byte> data = {}) {
        conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(op), std::move(data)));
    }
};

std::vector<std::byte> rename_answer(const std::string& name, std::int32_t result) {
    std::vector<std::byte> b(0x84);
    for (std::size_t i = 0; i < name.size(); ++i) b[i] = static_cast<std::byte>(name[i]);
    auto r = static_cast<std::uint32_t>(result);
    for (int i = 0; i < 4; ++i) b[0x80 + static_cast<std::size_t>(i)] = static_cast<std::byte>((r >> (8 * i)) & 0xff);
    return b;
}

} // namespace

TEST_CASE("Name change window: the reminder, Disable Reminder, the command's checks and the server's answer") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    CHECK_FALSE(t.api->shown("NameChangeWnd"));
    t.packet(net::Op::InvokeNameChangeLazy);
    REQUIRE(t.api->shown("NameChangeWnd"));
    CHECK(t.api->child_text("NameChangeWnd", "NCW_CloseButton") == "Remind Me Later");
    CHECK(t.api->child_text("NameChangeWnd", "NCW_Prompt")->rfind("You are eligible for a name change!", 0) == 0);
    // Disable Reminder asks; yes saves the flag and closes the window.
    CHECK(t.api->click_child("NameChangeWnd", "NCW_DisableReminderButton"));
    REQUIRE(t.conn.confirmation());
    CHECK(t.conn.confirmation()->timeout_ms == 20000);
    t.conn.answer_confirmation(true);
    CHECK_FALSE(t.api->shown("NameChangeWnd"));
    CHECK(t.char_ini["Preferences/DisableNameChangeReminder"] == "1");
    // The lazy invitation now stays away; the immediate one turns the reminder back on.
    t.packet(net::Op::InvokeNameChangeLazy);
    CHECK_FALSE(t.api->shown("NameChangeWnd"));
    t.packet(net::Op::InvokeNameChangeImmediate);
    CHECK(t.api->shown("NameChangeWnd"));
    CHECK(t.char_ini["Preferences/DisableNameChangeReminder"] == "0");
    CHECK(t.api->click_child("NameChangeWnd", "NCW_CloseButton"));
    CHECK_FALSE(t.api->shown("NameChangeWnd"));
    // The ini's flag is read when the character enters the world.
    t.char_ini["Preferences/DisablePetNameChangeReminder"] = "1";
    t.conn.load_name_change_flags();
    t.packet(net::Op::InvokeChangePetName);
    CHECK_FALSE(t.api->shown("NameChangePetWnd"));
    // /changepetname with no name: the manual window, Close and no Disable Reminder.
    t.conn.name_change_command(net::GameConnection::kNamePet, "");
    REQUIRE(t.api->shown("NameChangePetWnd"));
    CHECK(t.api->child_text("NameChangePetWnd", "NCPW_CloseButton") == "Close");
    // Submit Name with a short name: refused before any question.
    CHECK(t.api->set_child_text("NameChangePetWnd", "NCPW_NameEdit", "Abc"));
    CHECK(t.api->click_child("NameChangePetWnd", "NCPW_SubmitButton"));
    CHECK_FALSE(t.conn.confirmation());
    CHECK(t.api->set_child_text("NameChangePetWnd", "NCPW_NameEdit", "Fluffy"));
    CHECK(t.api->click_child("NameChangePetWnd", "NCPW_SubmitButton"));
    REQUIRE(t.conn.confirmation());
    CHECK(t.conn.confirmation()->text.find("Fluffy") != std::string::npos);
    t.conn.answer_confirmation(false);
    // The server's answer: printed, shown as an OK dialog, and a success closes the window.
    t.packet(net::Op::ChangePetName, rename_answer("Fluffy", 1));
    REQUIRE(t.conn.confirmation());
    CHECK(t.conn.confirmation()->ok_only);
    t.conn.answer_confirmation(false);
    CHECK_FALSE(t.api->shown("NameChangePetWnd"));
    CHECK(t.api->child_text("NameChangePetWnd", "NCPW_SubmitButton") == "Submit Name");
}

TEST_CASE("Tip of the day: totd.txt's tips, Show Another Tip, the show option; a context tip hides it") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    CHECK_FALSE(t.api->shown("TipWindow#2"));
    REQUIRE(t.api->activate_tip_of_day(false));
    CHECK(t.api->shown("TipWindow#2"));
    auto first = t.api->child_text("TipWindow#2", "Display");
    REQUIRE(first);
    CHECK(first->find("<BR>") != std::string::npos);
    CHECK(t.api->click_child("TipWindow#2", "NextTipButton"));
    CHECK(t.api->child_text("TipWindow#2", "Display") != first); // more than five tips: never the last one again
    CHECK(t.api->click_child("TipWindow#2", "NoShowButton"));
    CHECK(t.api->child_checked("TipWindow#2", "NoShowButton") == true);
    CHECK(t.api->click_child("TipWindow#2", "CloseButton"));
    CHECK_FALSE(t.api->activate_tip_of_day(false)); // ShowTOTD off
    CHECK(t.api->activate_tip_of_day(true));        // the help window's Tips button
    CHECK(t.api->activate_tip(0));                  // the inventory tip: the context window's OnShow
    CHECK(t.api->shown("TipWindow"));
    CHECK_FALSE(t.api->shown("TipWindow#2"));
}

TEST_CASE("Help window: help.html, a link followed, Back, Contents") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    t.api->toggle("HelpWindow");
    REQUIRE(t.api->shown("HelpWindow"));
    auto contents = t.api->child_text("HelpWindow", "Display");
    REQUIRE(contents);
    CHECK(contents->find("Welcome to EverQuest") != std::string::npos);
    CHECK(t.api->stml_link("HelpWindow", "Display", -1, "file:///help/movement.html"));
    auto page = t.api->child_text("HelpWindow", "Display");
    CHECK(page != contents);
    CHECK(t.api->child_state("HelpWindow", "BackButton") == true);
    CHECK(t.api->click_child("HelpWindow", "BackButton"));
    CHECK(t.api->child_text("HelpWindow", "Display") == contents);
    CHECK(t.api->child_state("HelpWindow", "BackButton") == false);
}

TEST_CASE("Help window: Back goes at most 64 pages back") {
    hailstone::test::ScratchDir game_dir{"help_history_test"};
    fs::create_directories(game_dir.path() / "help");
    for (int i = 0; i < 2; ++i)
        std::ofstream(game_dir.path() / "help" / ("p" + std::to_string(i) + ".html")) << "page " << i;
    std::ofstream(game_dir.path() / "help" / "help.html") << "contents";
    GameDir game(game_dir.path());
    Harness t(game);
    t.api->toggle("HelpWindow");
    REQUIRE(t.api->child_text("HelpWindow", "Display") == "contents");
    for (int i = 0; i < 100; ++i)
        CHECK(t.api->stml_link("HelpWindow", "Display", -1, i % 2 ? "file:///help/p1.html" : "file:///help/p0.html"));
    int back = 0;
    while (t.api->child_state("HelpWindow", "BackButton") == true && back < 200) {
        CHECK(t.api->click_child("HelpWindow", "BackButton"));
        ++back;
    }
    CHECK(back == 64);
    CHECK(t.api->child_text("HelpWindow", "Display") == "page 1"); // the 36th page shown, not the contents
}

TEST_CASE("Context tips: a window's OnShow activates its tip once as it opens") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    t.api->begin_frame();
    t.api->show("InventoryWindow", true);
    t.api->begin_frame();
    CHECK(t.api->shown("TipWindow"));
    CHECK(t.api->child_text("TipWindow", "Display")->size() > 20);
}

TEST_CASE("Context tips: the ids are the original table's (0xb503ac), the event tips' pages") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    auto page = [&](int tip) {
        REQUIRE(t.api->activate_tip(tip));
        return t.api->child_text("TipWindow", "Display").value_or("");
    };
    // after the voice macro (0x38) and audio trigger (0x46) entries: the titles, hot bar and item display tips
    CHECK(page(0x39).find("Custom Titles Tips") != std::string::npos);
    CHECK(page(0x3a).find("Barter Buyer Tips") != std::string::npos);
    CHECK(page(0x59).find("Item Display Tips") != std::string::npos);
    CHECK_FALSE(t.api->activate_tip(0x5a));
    // the event tips: group invitation, guild invitation, consent, red con, experience
    for (int tip : {0x0c, 0x19, 0x1c, 0x20, 0x09, 0x1b, 0x22}) CHECK(page(tip).size() > 20);
}

TEST_CASE("Layout copy: another character's layout for a size, onto this one's windowed places") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    fs::path chars = t.dir / "characters" / "World";
    fs::create_directories(chars / "Me");
    fs::create_directories(chars / "Other");
    {
        std::ofstream(chars / "Me" / "UI.ini") << "[InventoryWindow]\nXPosWindowed=10\nYPosWindowed=10\n";
        std::ofstream(chars / "Other" / "UI.ini") << "[InventoryWindow]\nXPos1280x1024=300\nYPos1280x1024=200\nShow=1\n"
                                                 << "[MapViewWnd]\nXPosWindowed=5\n";
        std::ofstream(chars / "Other" / "hotbuttons.ini") << "[Socials]\nPage1Button1Name=Wave\n";
    }
    t.api->load_layout(t.dir / "defaults.ini", chars / "Me" / "UI.ini", {});
    REQUIRE(t.host.layout_sources);
    auto sources = t.host.layout_sources();
    REQUIRE(sources.size() == 2); // Other's 1280x1024 and Windowed; not Me's own Windowed
    CHECK(sources[0].label == "Other");
    CHECK(sources[0].mode == "1280x1024");
    t.api->show("LayoutCopyWindow", true);
    CHECK(t.api->list_texts("LayoutCopyWindow", "LC_LayoutsList").size() == 2);
    CHECK(t.api->click_list("LayoutCopyWindow", "LC_LayoutsList", 0, 0));
    CHECK(t.api->click_child("LayoutCopyWindow", "LC_CopyButton"));
    REQUIRE(t.host.confirm);
    auto done = std::move(t.host.confirm->done);
    t.host.confirm.reset();
    done(true);
    t.api->begin_frame(); // placed again from the file
    IniFile me;
    REQUIRE(me.load(chars / "Me" / "UI.ini"));
    CHECK(me.get("InventoryWindow", "XPosWindowed") == "300");
    CHECK(me.get("InventoryWindow", "YPosWindowed") == "200");
    CHECK(me.get("InventoryWindow", "Show") == "1");
    // The current file's own windowed layout onto itself is refused.
    {
        app::UiHost::LayoutSource self{chars / "Me" / "UI.ini", "Me", "Windowed"};
        auto refused = t.host.copy_layout(self, {});
        REQUIRE(refused);
        CHECK(refused->first == 6337u);
    }
}

TEST_CASE("Layout copy: the characters played last first, their world named when there are several; the first-entry offer") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    fs::path chars = t.dir / "characters";
    for (const char* who : {"World/Me", "World/Old", "World/New", "Other World/Alt"}) fs::create_directories(chars / who);
    auto write = [&](const fs::path& f, int age_hours) {
        std::ofstream(f) << "[InventoryWindow]\nXPosWindowed=10\n";
        fs::last_write_time(f, fs::file_time_type::clock::now() - std::chrono::hours(age_hours));
    };
    write(chars / "World" / "Old" / "UI.ini", 30);
    write(chars / "World" / "New" / "UI.ini", 1);
    write(chars / "Other World" / "Alt" / "UI.ini", 5);
    std::ofstream(t.dir / "defaults.ini") << "[InventoryWindow]\nXPos1024x768=4\n";
    // Me has no UI.ini yet: its first entry.
    t.api->load_layout(t.dir / "defaults.ini", chars / "World" / "Me" / "UI.ini", {});
    auto sources = t.host.layout_sources();
    REQUIRE(sources.size() == 4);
    CHECK(sources[0].label == "New");
    CHECK(sources[1].label == "Alt");
    CHECK(sources[2].label == "Old");
    CHECK(sources[1].world == "Other World");
    CHECK(sources[1].character_dir == chars / "Other World" / "Alt");
    CHECK(sources[3].label == "Defaults.ini");
    CHECK(sources[3].character_dir.empty());

    // The offer: shown, the character played last selected, every box checked, the reason and the offer box shown.
    std::optional<int> offer;
    t.host.find_option = [&](std::string_view o) -> std::optional<int> {
        return o == "offer_layout_copy" ? offer : std::nullopt;
    };
    t.host.set_option = [&](std::string_view o, int v) { if (o == "offer_layout_copy") offer = v; };
    REQUIRE(t.api->activate_window("LayoutCopyWindow", 0));
    CHECK(t.api->shown("LayoutCopyWindow"));
    auto rows = t.api->list_texts("LayoutCopyWindow", "LC_LayoutsList");
    REQUIRE(rows.size() == 4);
    CHECK(rows[1] == "Alt (Other World)");
    for (const char* box : {"LC_CopyHotButtonCheckBox", "LC_CopyHotButtonLoadoutsCheckBox", "LC_CopySocialsCheckBox",
                            "LC_CopyChatCheckBox", "LC_CopyOptionsCheckBox", "LC_OfferCheckBox"})
        CHECK(t.api->child_checked("LayoutCopyWindow", box) == true);
    CHECK(t.api->child_shown("LayoutCopyWindow", "LC_FirstVisitText") == true);
    CHECK_FALSE(t.api->activate_window("LayoutCopyWindow", 0)); // already open
    // The offer box turns the option off.
    CHECK(t.api->click_child("LayoutCopyWindow", "LC_OfferCheckBox"));
    CHECK(offer == 0);
    // Defaults.ini selected: no chat or options to copy.
    CHECK(t.api->click_list("LayoutCopyWindow", "LC_LayoutsList", 3, 0));
    CHECK(t.api->child_state("LayoutCopyWindow", "LC_CopyChatCheckBox") == false);
    CHECK(t.api->child_state("LayoutCopyWindow", "LC_CopyOptionsCheckBox") == false);
    t.api->show("LayoutCopyWindow", false);
    // Off: not offered. Opened by hand (/copylayout), the window is the original's: no reason, no row, boxes off.
    CHECK_FALSE(t.api->activate_window("LayoutCopyWindow", 0));
    CHECK_FALSE(t.api->shown("LayoutCopyWindow"));
    t.api->show("LayoutCopyWindow", true);
    CHECK(t.api->child_shown("LayoutCopyWindow", "LC_FirstVisitText") == false);
    CHECK(t.api->child_checked("LayoutCopyWindow", "LC_CopyChatCheckBox") == false);
    t.api->show("LayoutCopyWindow", false);
    // On, but only defaults.ini to copy from: not offered.
    offer = 1;
    for (const char* who : {"World/Old", "World/New", "Other World/Alt"}) fs::remove(chars / who / "UI.ini");
    CHECK_FALSE(t.api->activate_window("LayoutCopyWindow", 0));
}

TEST_CASE("Load skin window: the user interfaces, the running one selected; Load runs /loadskin") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    t.host.option_choices = [](std::string_view) { return std::vector<std::string>{"Classic", "Modern"}; };
    t.host.find_option = [](std::string_view o) -> std::optional<int> { return o == "ui_name_now" ? std::optional<int>(1) : std::nullopt; };
    std::string sent;
    t.host.submit_chat = [&](const std::string& line) { sent = line; };
    t.api->show("LoadskinWnd", true);
    CHECK(t.api->list_texts("LoadskinWnd", "SkinList") == std::vector<std::string>{"Classic", "Modern"});
    CHECK(t.api->child_state("LoadskinWnd", "LoadButton") == true);
    CHECK(t.api->click_list("LoadskinWnd", "SkinList", 0, 0));
    CHECK(t.api->click_child("LoadskinWnd", "INIButton")); // off
    CHECK(t.api->click_child("LoadskinWnd", "LoadButton"));
    CHECK(sent == "/loadskin \"Classic\" 0");
}

TEST_CASE("Video modes window: each size once, the refresh rates in full screen, Accept asks to keep it") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    int mode = 1, resolution = 0;
    t.host.option_choices = [](std::string_view) {
        return std::vector<std::string>{"1920 x 1080 (60 Hz)", "1920 x 1080 (144 Hz)", "1280 x 720 (60 Hz)", "320 x 200 (60 Hz)"};
    };
    t.host.find_option = [&](std::string_view o) -> std::optional<int> {
        if (o == "window_mode") return mode;
        if (o == "resolution") return resolution;
        return std::nullopt;
    };
    t.host.set_option = [&](std::string_view o, int v) {
        if (o == "resolution") resolution = v;
    };
    mode = 2; // full screen
    t.api->show("VideoModesWindow", true);
    CHECK(t.api->list_texts("VideoModesWindow", "VIDW_VideoResolutionsList") == std::vector<std::string>{"1920x1080", "1280x720"});
    CHECK(t.api->click_list("VideoModesWindow", "VIDW_VideoResolutionsList", 1, 0));
    CHECK(t.api->click_child("VideoModesWindow", "VIDW_AcceptButton"));
    CHECK(resolution == 2);
    REQUIRE(t.host.confirm);
    auto done = std::move(t.host.confirm->done);
    t.host.confirm.reset();
    done(false); // not kept: back to the mode before
    CHECK(resolution == 0);
}

TEST_CASE("Story window: storyline\\story*.txt newest first, the oldest unread selected, LastRead kept") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    std::map<std::string, int> opts;
    t.host.find_option = [&](std::string_view o) -> std::optional<int> {
        auto it = opts.find(std::string(o));
        return it == opts.end() ? std::optional<int>(0) : std::optional<int>(it->second);
    };
    t.host.set_option = [&](std::string_view o, int v) { opts[std::string(o)] = v; };
    t.api->show("StoryWnd", true);
    auto rows = t.api->list_texts("StoryWnd", "StoryList");
    REQUIRE(rows.size() > 3);
    auto page = t.api->child_text("StoryWnd", "Display");
    REQUIRE(page);
    CHECK(!page->empty());           // none read: the oldest story shown
    CHECK(opts["news_last_read"] > 0); // and LastRead moved to its date
    CHECK(t.api->click_child("StoryWnd", "AutomaticButton"));
    CHECK(opts["news_automatic"] == 1);
}

// The storyline pages a server ships beside its stories (the guides' index pages lead into folders of pages): a
// file:/// link from the game folder, a bare name beside the page shown, a web link not followed. No game folder
// needed: the pages are written to a scratch one.
TEST_CASE("Story window: a page's href links are followed, web links are not") {
    hailstone::test::ScratchDir scratch("ui_more_windows_story_links");
    const fs::path root = scratch.path() / "game";
    fs::create_directories(root / "storyline" / "Epic");
    std::ofstream(root / "storyline" / "storyEpicsGuide.txt")
        << "Epic Weapon Guides\n<a href=\"file:///storyline/Epic/BardEpic.txt\">Bard</a> <a href=\"http://example.com/\">web</a>\n";
    std::ofstream(root / "storyline" / "Epic" / "BardEpic.txt") << "Bard epic page <a href=\"ClericEpic.txt\">next</a>\n";
    std::ofstream(root / "storyline" / "Epic" / "ClericEpic.txt") << "Cleric epic page\n";
    GameDir game(root);
    Harness t(game);
    std::map<std::string, int> opts;
    t.host.find_option = [&](std::string_view o) -> std::optional<int> {
        auto it = opts.find(std::string(o));
        return it == opts.end() ? std::optional<int>(0) : std::optional<int>(it->second);
    };
    t.host.set_option = [&](std::string_view o, int v) { opts[std::string(o)] = v; };
    t.api->show("StoryWnd", true);
    REQUIRE(t.api->list_texts("StoryWnd", "StoryList").size() == 1); // (the pages in folders are not stories)
    CHECK(t.api->click_list("StoryWnd", "StoryList", 0, 0));
    auto guide = t.api->child_text("StoryWnd", "Display");
    REQUIRE(guide);
    CHECK(guide->find("Epic Weapon Guides") != std::string::npos);
    CHECK(t.api->stml_link("StoryWnd", "Display", -1, "http://example.com/")); // taken, not followed
    CHECK(t.api->child_text("StoryWnd", "Display") == guide);
    CHECK(t.api->stml_link("StoryWnd", "Display", -1, "file:///storyline/Epic/BardEpic.txt"));
    auto bard = t.api->child_text("StoryWnd", "Display");
    REQUIRE(bard);
    CHECK(bard->find("Bard epic page") != std::string::npos);
    CHECK(t.api->stml_link("StoryWnd", "Display", -1, "ClericEpic.txt")); // beside the page shown
    auto cleric = t.api->child_text("StoryWnd", "Display");
    REQUIRE(cleric);
    CHECK(cleric->find("Cleric epic page") != std::string::npos);
    CHECK_FALSE(t.api->stml_link("StoryWnd", "Display", 27, "x")); // a WndNotify link: no handler
    CHECK(t.api->click_list("StoryWnd", "StoryList", 0, 0)); // the story again
    CHECK(t.api->child_text("StoryWnd", "Display") == guide);
}

TEST_CASE("Server select (stock page): the worlds, Play, the last world's quick connect, Logout") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    ui::ServerSelectView v;
    v.servers = {{"Alpha", "Up", true}, {"Beta", "Down", false}};
    v.selected = 1;
    t.host.server_select = &v;
    t.host.last_server = "Alpha";
    CHECK(t.api->list_texts("serverselect", "ServerList") == std::vector<std::string>{"Alpha", "Beta"});
    CHECK(t.api->child_state("serverselect", "PlayButton") == false); // Beta cannot be joined
    CHECK(t.api->child_text("serverselect", "PlayLastServerButton") == "Alpha");
    CHECK(t.api->click_list("serverselect", "ServerList", 0, 0));
    CHECK(t.api->click_child("serverselect", "PlayButton"));
    CHECK(t.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Play));
    t.host.server_select_action.reset();
    CHECK(t.api->click_child("serverselect", "ExitButton"));
    CHECK(t.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Back));
}

TEST_CASE("Character creation (stock page): race, class, deity and city from world's tables; the stats; Create") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    net::CharCreateData data;
    net::CharCreateAllocation a;
    a.index = 7;
    a.base = {75, 70, 75, 75, 75, 80, 75};
    a.points = {0, 0, 0, 0, 0, 30, 0};
    data.allocations.push_back(a);
    data.combos.push_back({0, 1, 2, 201, 7, 2}); // Human Cleric of Bertoxxulous, South Qeynos
    data.combos.push_back({0, 1, 2, 202, 7, 2});
    data.combos.push_back({0, 1, 1, 201, 7, 2}); // Human Warrior
    data.combos.push_back({2, 128, 2, 202, 7, 106}); // an Iksar Cleric, Kunark required
    t.conn.receive_world_packet_for_test(encode(data));
    REQUIRE(t.conn.char_create_data());
    std::string sent;
    net::CharCreate request;
    t.host.char_create_open = true;
    t.host.char_create_submit = [&](const std::string& n, const net::CharCreate& cc) { sent = n, request = cc; };
    CHECK(t.api->shown("CharacterCreation"));
    CHECK(t.api->child_checked("CharacterCreation", "CC_Race_Human_Button_Male") == true);
    CHECK(t.api->child_state("CharacterCreation", "CC_Class_Wizard_Button") == false);
    CHECK(t.api->click_child("CharacterCreation", "CC_Class_Cleric_Button"));
    CHECK(t.api->child_checked("CharacterCreation", "CC_Class_Cleric_Button") == true);
    CHECK(t.api->child_text("CharacterCreation", "CCA_Points_Left_Display_Button") == "30");
    CHECK(t.api->child_text("CharacterCreation", "CCA_Stat5_Display_Button") == "80");
    // Create with points left: 1577 and the stats opened.
    CHECK(t.api->set_child_text("CharacterCreation", "CC_Name_Edit", "Testname"));
    CHECK(t.api->click_child("CharacterCreation", "CC_Create_Button"));
    CHECK(sent.empty());
    CHECK(t.host.confirm);
    t.host.confirm.reset();
    CHECK(t.api->child_checked("CharacterCreation", "CC_Advanced_Button") == true);
    // A right click on Wisdom puts 25 in it (base + 25), Up puts the rest elsewhere.
    CHECK(t.api->click_child("CharacterCreation", "CCA_Stat5_Button", "right"));
    CHECK(t.api->child_text("CharacterCreation", "CCA_Stat5_Display_Button") == "105");
    for (int i = 0; i < 5; ++i) CHECK(t.api->click_child("CharacterCreation", "CCA_Stat0_Up_Button"));
    CHECK(t.api->child_text("CharacterCreation", "CCA_Points_Left_Display_Button") == "0");
    CHECK(t.api->child_state("CharacterCreation", "CCA_Stat0_Up_Button") == false);
    CHECK(t.api->click_child("CharacterCreation", "CC_Create_Button"));
    CHECK(sent == "Testname");
    CHECK(request.race == 1);
    CHECK(request.class_ == 2);
    CHECK(request.wis == 105);
    CHECK(request.str == 80);
    CHECK(request.start_zone == 2);
}

TEST_CASE("Tribute trophy window: refused without House of Thule; the personal and guild views") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    t.api->show("TributeTrophyWnd", true); // no profile: no House of Thule (5422)
    CHECK_FALSE(t.api->shown("TributeTrophyWnd"));
    CHECK(t.api->child_checked("TributeTrophyWnd", "TTW_ViewPersonal_Btn") == true);
    CHECK(t.api->child_text("TributeTrophyWnd", "TTW_CurrentPointsLabel") == "0");
    CHECK(t.api->child_text("TributeTrophyWnd", "TTW_LockButton") == "Modify");
    CHECK(t.api->child_state("TributeTrophyWnd", "TTW_UpgradeButton") == false);
    CHECK(t.api->click_child("TributeTrophyWnd", "TTW_ViewGuild_Btn"));
    CHECK(t.api->child_checked("TributeTrophyWnd", "TTW_ViewGuild_Btn") == true);
    CHECK(t.api->child_checked("TributeTrophyWnd", "TTW_ViewPersonal_Btn") == false);
    CHECK(t.api->list_texts("TributeTrophyWnd", "TTW_AvailableTrophyBenefitList").empty());
}

namespace {
std::vector<std::byte> special_message(std::uint8_t speak, std::uint8_t journal, const std::string& sayer, const std::string& text) {
    std::vector<std::byte> b;
    auto put = [&](std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    };
    put(speak, 1);
    put(journal, 1);
    put(0, 1);   // language
    put(10, 4);  // type
    put(0, 4);   // target
    for (char c : sayer) b.push_back(static_cast<std::byte>(c));
    b.push_back(std::byte{0});
    put(10, 4), put(20, 4), put(30, 4);
    for (char c : text) b.push_back(static_cast<std::byte>(c));
    b.push_back(std::byte{0});
    return b;
}
} // namespace

TEST_CASE("NPC journal: OP_SpecialMesg lines kept while logging, the lists, a category, a search, Save") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    std::vector<std::byte> exp(68);
    exp[64] = std::byte{0x30}; // Lost Dungeons of Norrath and Gates of Discord
    t.conn.receive_world_packet_for_test(net::AppPacket(net::to_wire(net::Op::ExpansionInfo), exp));
    // the character's UI ini with JournalLog on
    { std::ofstream(t.dir / "UI_Journal.ini") << "[JournalNPCWnd]\nJournalLog=1\n"; }
    t.api->load_layout(t.dir / "defaults.ini", t.dir / "UI_Journal.ini", {});
    t.packet(net::Op::SpecialMesg, special_message(1, 2, "Guard Gehnus", "Hail, traveler."));
    t.packet(net::Op::SpecialMesg, special_message(1, 0, "Guard Gehnus", "Not for the journal."));
    t.packet(net::Op::SpecialMesg, special_message(1, 2, "Merchant Ilia", "Buy something!"));
    t.api->begin_frame(); // the window takes the lines
    t.api->show("JournalNPCWnd", true);
    REQUIRE(t.api->shown("JournalNPCWnd"));
    auto npcs = t.api->list_texts("JournalNPCWnd", "NPCList");
    REQUIRE(npcs.size() == 2);
    CHECK(t.api->header_click_list("JournalNPCWnd", "NPCList", 0)); // by name
    npcs = t.api->list_texts("JournalNPCWnd", "NPCList");
    CHECK(npcs == std::vector<std::string>{"Guard Gehnus", "Merchant Ilia"});
    CHECK(t.api->click_list("JournalNPCWnd", "NPCList", 0, 0));
    REQUIRE(t.api->list_texts("JournalNPCWnd", "DialogList").size() == 1);
    CHECK(t.api->click_list("JournalNPCWnd", "DialogList", 0, 0));
    CHECK(t.api->child_text("JournalNPCWnd", "Text") == "Guard Gehnus says 'Hail, traveler.'");
    // a category from the category window, given to the entry
    t.api->show("JournalCatWnd", true);
    CHECK(t.api->set_child_text("JournalCatWnd", "NameInput", "Quests"));
    CHECK(t.api->click_child("JournalCatWnd", "AddButton"));
    CHECK(t.api->list_texts("JournalCatWnd", "CategoryList") == std::vector<std::string>{"Quests"});
    t.api->show("JournalNPCWnd", false);
    t.api->show("JournalNPCWnd", true);
    CHECK(t.api->click_list("JournalNPCWnd", "NPCList", 0, 0));
    CHECK(t.api->click_list("JournalNPCWnd", "DialogList", 0, 0));
    CHECK(t.api->select_combo("JournalNPCWnd", "CategorySelect", 1));
    CHECK(t.api->list_rows("JournalNPCWnd", "DialogList").at(0).at(1) == "Quests");
    // the filter: only NPCs with an entry in the category
    CHECK(t.api->select_combo("JournalNPCWnd", "CategoryFilter", 1));
    CHECK(t.api->list_texts("JournalNPCWnd", "NPCList") == std::vector<std::string>{"Guard Gehnus"});
    CHECK(t.api->select_combo("JournalNPCWnd", "CategoryFilter", 0));
    // a search through the text entry window
    CHECK(t.api->click_child("JournalNPCWnd", "SearchButton"));
    REQUIRE(t.host.text_entry);
    auto done = t.host.text_entry->done;
    t.host.text_entry.reset();
    done("buy");
    CHECK(t.api->list_texts("JournalNPCWnd", "NPCList") == std::vector<std::string>{"Merchant Ilia"});
    CHECK(t.api->child_text("JournalNPCWnd", "SearchButton") == "buy");
    CHECK(t.api->click_child("JournalNPCWnd", "SearchButton")); // clears it
    CHECK(t.api->list_texts("JournalNPCWnd", "NPCList").size() == 2);
    CHECK(t.api->click_child("JournalNPCWnd", "SaveButton"));
    CHECK(fs::exists(t.dir / "CJ.hdr"));
    CHECK(fs::exists(t.dir / "CJ.txt"));
}
