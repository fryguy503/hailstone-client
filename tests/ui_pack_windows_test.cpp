// The shipped UIs (ui/modern, ui/gamepad) run for real over Dear ImGui with no renderer: every script loads, and the
// windows each draws for itself (the ones that once came from the Classic UI among them) draw with the game asking
// for them, with no script error.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <set>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/gamepad.h"
#include "core/byte_io.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "net/aa_packets.h"
#include "net/game_connection.h"
#include "net/group_packets.h"
#include "net/merchant_packets.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"
#include "game/mechanics/character.h"
#include "game/mechanics/tables.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "mock_server/harness.h"
#include "manual_clock.h"
#include "scratch_dir.h"
#include "app/web_service.h"
#include "mq/lua_mq.h"
#include "plugin/policy.h"
#include "web/browser_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Session {
    GameDir game;
    net::GameConnection conn;
    ui::ChatView view;
    app::EqHost host;
    app::EqApi api;
    ui::lua::LuaUi lua;
    Session(const fs::path& dir, const std::string& pack, net::ConnectionOptions options = {})
        : game(dir / "game"), conn(game, std::move(options)), api(host), lua(dir / pack, dir / "ui.txt") {
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.in_game = host.ui_ready = true;
        lua.add_eq_api([this](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
        });
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
    }
    void frames(int n, ui::InGameView* fallback = nullptr) {
        for (int i = 0; i < n; ++i) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600, 900);
            io.DeltaTime = 1.0f / 60.0f;
            api.begin_frame();
            ImGui::NewFrame();
            if (fallback) (void)ui::in_game_overlay(*fallback);
            lua.frame();
            ImGui::Render();
        }
    }
};

struct ReplacementCheck {
    explicit ReplacementCheck(const app::EqApi& api) {
        ui::lua::set_replaced_check([p = &api](std::string_view screen) { return p->replaces(screen); });
    }
    ~ReplacementCheck() { ui::lua::set_replaced_check({}); }
};

net::AppPacket respawn_window_packet(std::uint8_t validity) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(60000);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(2);
    for (std::uint32_t id = 0; id < 2; ++id) {
        w.le(id);
        w.le<std::uint32_t>(2);
        for (int i = 0; i < 4; ++i) w.f32(0);
        const std::string_view name = id == 0 ? "Bind Location" : "Resurrect";
        w.bytes(std::as_bytes(std::span(name.data(), name.size())));
        w.le<std::uint8_t>(0);
        w.le<std::uint8_t>(id == 0 ? 0 : validity);
    }
    return net::AppPacket(net::to_wire(net::Op::RespawnWindow), w.take());
}

net::AppPacket resurrection_offer_packet() {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint16_t>(2);
    w.le<std::uint16_t>(0);
    w.f32(30);
    w.f32(40);
    w.f32(12);
    w.le<std::uint32_t>(0);
    w.fixed_string("Gennro", 64);
    w.le<std::uint32_t>(0);
    w.fixed_string("Healer", 64);
    w.le<std::uint32_t>(100);
    w.fixed_string("Gennro's corpse", 64);
    w.le<std::uint32_t>(0);
    w.zeros(8);
    return net::AppPacket(net::to_wire(net::Op::RezzRequest), w.take());
}

// Windows the packs redid from the Classic UI's (and a few more of theirs): shown one after another.
constexpr const char* kScreens[] = {
    "GroupSearchWnd", "GroupSearchFiltersWnd", "GroupMentorWindow", "FellowshipWnd", "SocialEditWnd",
    "IconSelectionWnd", "CombatAbilityWnd", "JournalNPCWnd", "JournalCatWnd", "HelpWindow", "StoryWnd",
    "AlertHistoryWnd", "ZonePathWnd", "TributeTrophyWnd",
    "LoadskinWnd", "LayoutCopyWindow", "VideoModesWindow", "NameChangeWnd", "NameChangePetWnd", "NameChangeMercWnd",
    "OptionsWindow", "AlarmWnd", "TipWindow", "NoteWindow", "EditLabelWnd", "BugReportWindow", "FeedbackWindow",
    "ClaimWnd", "ColorPickerWnd", "BodyTintWnd", "PlayerCustomizationWnd", "TitleWnd", "BreathWindow",
    "AggroMeterWnd", "PotionBeltWnd", "BandolierWnd", "BlockedBuffWnd", "BlockedPetBuffWnd", "InspectWnd",
    "LeadershipWindow", "LFGuildWnd", "MailAddressBookWindow", "MailIgnoreListWindow", "RaidOptionsWindow",
    "CombatSkillSelectWnd", "AdventureRequestWnd", "AdventureStatsWnd", "AdventureLeaderboardWnd", "DynamicZoneWnd",
    "PvPStatsWnd", "PvpLeaderboardWnd", "BazaarSearchWnd", "BarterSearchWnd", "TrainWindow", "TrackingWnd",
    "GuildBankWnd", "HotButtonWnd2", "HotButtonWnd3", "BrowserWnd", "RealEstatePurchaseWnd", "RealEstateManageWnd",
    "RealEstateItemsWnd", "RealEstateNeighborhoodWnd", "RealEstatePlotSearchWnd", "ScriptsWnd"};

} // namespace

TEST_CASE("UI packs: every script of the shipped UIs loads, and their windows draw without script errors") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        REQUIRE(fs::exists(src / "ui.ini"));
        hailstone::test::ScratchDir dir_scratch("ui_pack_windows_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        CHECK_FALSE(fs::exists(dir / "classic")); // nothing of the Classic UI is there to borrow

        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            lua_State* L = s.lua.state();
            for (const char* screen : kScreens) {
                INFO(screen);
                std::string show = std::string("eq.ShowWindow('") + screen + "', true)";
                REQUIRE(luaL_dostring(L, show.c_str()) == LUA_OK);
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                std::string hide = std::string("eq.ShowWindow('") + screen + "', false)";
                REQUIRE(luaL_dostring(L, hide.c_str()) == LUA_OK);
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: hover respawn draws available and unavailable choices without script errors") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        test::ScratchDir scratch("ui_pack_respawn_test");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &width, &height);
        {
            test::ManualClock clock;
            clock.hold();
            net::ConnectionOptions options;
            options.session.clock = clock.source();
            Session s(dir, pack, std::move(options));
            s.conn.receive_zone_packet_for_test(respawn_window_packet(1));
            clock.advance(std::chrono::milliseconds(1));
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            ImGuiWindow* respawn = ImGui::FindWindowByName(std::string_view(pack) == "modern"
                ? "###modern.RespawnWnd" : "Respawn");
            REQUIRE(respawn);
            CHECK(respawn->Active);
            CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
            CHECK(s.api.list_texts("RespawnWnd", "OptionsList") == std::vector<std::string>{"*", "unavailable"});

            CHECK(s.api.click_list("RespawnWnd", "OptionsList", 1, 0));
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(s.api.child_state("RespawnWnd", "SelectButton") == false);
            CHECK(s.api.click_child("RespawnWnd", "SelectButton"));
            CHECK_FALSE(s.conn.respawn_chosen());

            s.conn.receive_zone_packet_for_test(respawn_window_packet(0));
            clock.advance(std::chrono::milliseconds(1));
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
            CHECK(s.api.list_texts("RespawnWnd", "OptionsList") == std::vector<std::string>{"*", ""});
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: resurrection keeps the original respawn choices and timer until an explicit destination") {
    for (const char* pack : {"modern", "gamepad"}) {
        for (int choice : {0, 1, 2}) {
            const bool accept = choice != 0, manual_bind = choice == 2;
            INFO(pack, " accept=", accept, " manual_bind=", manual_bind);
            const bool gamepad_pack = std::string_view(pack) == "gamepad";
            const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
            test::ScratchDir scratch("ui_pack_respawn_question_test");
            const fs::path dir = scratch.path();
            fs::create_directories(dir / "game");
            fs::copy(src, dir / pack, fs::copy_options::recursive);
            ImGuiContext* ctx = ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            unsigned char* px = nullptr;
            int width = 0, height = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &width, &height);
            {
                test::ManualClock clock;
                auto options = test::quick_options();
                options.session.clock = clock.source();
                test::Rig rig(test::MockConfig::standard(), std::move(options));
                REQUIRE(rig.to_zone());
                clock.hold();
                Config config;
                platform::GamepadState state;
                state.connected = true;
                state.type = platform::PadType::Xbox;
                app::Gamepad pad(config, dir / "gamepad.ini");
                pad.set_state(&state);
                Session s(dir, pack);
                auto& conn = rig.conn;
                s.host.conn = &conn;
                s.host.gamepad = &pad;
                // Observe the actual pack's paint and selected-row input, rather than only its shared model.
                REQUIRE(luaL_dostring(s.lua.state(), R"lua(
local TextColored, DrawText, ListInfo = ui.TextColored, ui.DrawText, eq.ListInfo
ui.TextColored = function(r, g, b, a, text, ...)
  if text == "Resurrect" then RespawnPaintColor = { r, g, b } end
  return TextColored(r, g, b, a, text, ...)
end
ui.DrawText = function(x, y, r, g, b, a, text, ...)
  if text == "Resurrect" then RespawnPaintColor = { r, g, b } end
  return DrawText(x, y, r, g, b, a, text, ...)
end
eq.ListInfo = function(win, child)
  local rows, columns, selected, labels, widths, multi = ListInfo(win, child)
  if win == "RespawnWnd" and child == "OptionsList" then RespawnPaintSelected = selected end
  return rows, columns, selected, labels, widths, multi
end
)lua") == LUA_OK);

                net::Death death;
                death.spawn_id = death.corpse_id = conn.world().player_id;
                death.killer_id = 101;
                death.damage = 5000;
                conn.receive_zone_packet_for_test(net::encode(death));
                conn.receive_zone_packet_for_test(respawn_window_packet(1));
                if (manual_bind) CHECK(s.api.click_list("RespawnWnd", "OptionsList", 0, 0));
                conn.receive_zone_packet_for_test(resurrection_offer_packet());
                REQUIRE(conn.confirmation());
                REQUIRE(conn.confirmation()->id == net::kConfirmRezz);
                clock.advance(std::chrono::milliseconds(1));
                s.frames(3); // both packets arrived before the first UI frame, within its initial three seconds
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                ImGuiWindow* confirm = ImGui::FindWindowByName(gamepad_pack ? "Confirm" : "###modern.ConfirmationDialogBox");
                REQUIRE(confirm);
                CHECK(confirm->Active);
                REQUIRE(GImGui->NavWindow);
                CHECK(GImGui->NavWindow->RootWindow == confirm);
                const char* respawn_name = gamepad_pack ? "Respawn" : "###modern.RespawnWnd";
                ImGuiWindow* respawn = ImGui::FindWindowByName(respawn_name);
                REQUIRE(respawn);
                CHECK(respawn->Active);
                CHECK(s.api.replaces("RespawnWnd"));
                REQUIRE(luaL_dostring(s.lua.state(),
                    "assert(RespawnPaintColor and math.abs(RespawnPaintColor[1]-1)<0.001 and "
                    "math.abs(RespawnPaintColor[2]-0.25)<0.001 and math.abs(RespawnPaintColor[3]-0.25)<0.001); "
                    "assert(RespawnPaintSelected==0)") == LUA_OK);
                // Match the app's fallback draw before the Lua pass: the pack must retain ownership while
                // the question is present, so a second "You have died" window cannot replace its original.
                ReplacementCheck replacements(s.api);
                ui::InGameView fallback;
                fallback.respawn.options = {{0, "Bind Location"}, {1, "Resurrect"}};
                fallback.respawn.ready = true;
                s.frames(3, &fallback);
                ImGuiWindow* duplicate = ImGui::FindWindowByName("You have died##hailstone_respawn");
                CHECK((!duplicate || !duplicate->Active));
                CHECK(respawn->Active);
                CHECK(confirm->Active);
                CHECK(GImGui->NavWindow->RootWindow == confirm);
                auto requests = pad.take_focus_requests();
                if (gamepad_pack) {
                    REQUIRE_FALSE(requests.empty());
                    CHECK(requests.back() == "Confirm");
                    CHECK(std::find(requests.begin(), requests.end(), "Respawn") == requests.end());
                }

                CHECK(s.api.click_child("ConfirmationDialogBox", accept ? "Yes_Button" : "No_Button"));
                CHECK_FALSE(conn.confirmation());
                CHECK_FALSE(conn.respawn_chosen());
                CHECK(conn.respawn_option_available(0));
                CHECK(conn.respawn_option_available(1) == accept);
                const auto remaining = conn.respawn_ms_left();
                clock.advance(std::chrono::milliseconds(2000));
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                respawn = ImGui::FindWindowByName(respawn_name);
                REQUIRE(respawn);
                CHECK(respawn->Active);
                CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
                CHECK(s.api.child_state("RespawnWnd", "OptionsList") == true);
                CHECK(conn.respawn_ms_left() == remaining - 2000);
                CHECK_FALSE(conn.respawn_chosen());
                auto markers = s.api.list_texts("RespawnWnd", "OptionsList");
                REQUIRE(markers.size() == 2);
                CHECK(markers[0] == "*");
                CHECK(markers[1] == (accept ? "" : "unavailable"));
                if (accept) {
                    REQUIRE(luaL_dostring(s.lua.state(),
                        "assert(RespawnPaintColor and math.abs(RespawnPaintColor[1]-0.25)<0.001 and "
                        "math.abs(RespawnPaintColor[2]-1)<0.001 and math.abs(RespawnPaintColor[3]-0.35)<0.001)") == LUA_OK);
                }
                const char* expected_selection = accept && !manual_bind
                    ? "assert(RespawnPaintSelected==1)" : "assert(RespawnPaintSelected==0)";
                REQUIRE(luaL_dostring(s.lua.state(), expected_selection) == LUA_OK);
                if (gamepad_pack) {
                    requests = pad.take_focus_requests();
                    REQUIRE_FALSE(requests.empty());
                    CHECK(requests.back() == "Respawn");
                }
                CHECK(s.api.click_list("RespawnWnd", "OptionsList", accept ? 1 : 0, 0));
                CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
                CHECK_FALSE(conn.respawn_chosen());
                CHECK(s.api.click_child("RespawnWnd", "SelectButton"));
                CHECK(conn.respawn_chosen());
                CHECK(s.api.child_state("RespawnWnd", "SelectButton") == false);
                net::ZonePlayerToBind bind;
                bind.zone_id = 2;
                bind.zone_name = accept ? "Resurrection destination" : "Bind destination";
                conn.receive_zone_packet_for_test(net::encode(bind));
                s.frames(2);
                CHECK_FALSE(conn.respawn_window());
                CHECK_FALSE(respawn->Active);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            ImGui::DestroyContext(ctx);
        }
    }
}

TEST_CASE("UI packs: each draws the login, server select and character select screens in the client's place") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_pregame_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            s.host.in_game = s.host.ui_ready = false;
            REQUIRE(luaL_dostring(s.lua.state(), "eq.InGame = function() return false end") == LUA_OK);

            // The login screen: saved servers with accounts and characters (no keyring), then a new server, then a
            // test login's.
            ui::LoginView login;
            login.servers_dir = dir / "servers";
            login.ui_packs = {{"modern", "Modern", "", "mouse", false, {}}, {"gamepad", "Gamepad", "", "gamepad", false, {}}};
            login.ui = login.default_ui = pack;
            ServerProfile p;
            p.id = "alpha";
            p.name = "Alpha";
            p.host = "127.0.0.1";
            p.username = "first";
            p.accounts = {{"first", "Bob", "World One", {{"Bob", "World One", 12, "WAR"}}, false, false},
                          {"second", "", "", {{"Cid", "World Two", 50, "SHD/CLR"}}, false, false}};
            login.profiles = {p};
            ui::select_profile(login, 0);
            login.game_folders = {{"/games/rof2", false, true}};
            login.status = {"Could not connect.", true};
            login.version = "0.2.0-alpha (abc1234)"; // the small version line at the foot
            s.host.login = &login;
            // the modern pack's box set card open from the first frame (test.login = boxset)
            REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.login', 'boxset')") == LUA_OK);
            s.frames(3);
            CHECK(s.api.replaces("LoginScreen"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (std::string(pack) == "modern") CHECK(ImGui::FindWindowByName("###modern.pregame.login.boxset") != nullptr);
            // The updater's notice in each of its states (docs/updater.md).
            login.update.version = "9.0.0";
            login.update.current = "0.1.0-alpha";
            login.update.notes = "https://example.invalid/r";
            login.update.has_package = true;
            login.update.can_restart = true;
            for (const char* state : {"available", "downloading", "ready", "failed", ""}) {
                login.update.state = state;
                login.update.progress = 0.4f;
                login.update.error = "no network";
                login.update.message = *state ? "" : "The update was cut off, so 0.1.0-alpha was put back.";
                s.frames(2);
                CHECK_MESSAGE(ImGui::FindWindowByName("UI errors") == nullptr, state);
            }
            login.update = {};
            // The corner: this build, each of the updater's words on it, What's new and the GitHub page.
            login.release_notes = {"A thing that changed", "Another, long enough to wrap over more than one line of the "
                                                            "popup that shows the release notes"};
            login.project_page = "https://example.invalid/hailstone";
            // What's new open (test.whatsnew opens it once in the modern pack; the gamepad pack's page is below)
            REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.whatsnew', '1')") == LUA_OK);
            for (const char* check : {"off", "checking", "uptodate", "unknown", "newer"}) {
                login.update.check = check;
                login.update.off_reason = "a development build";
                s.frames(2);
                CHECK_MESSAGE(ImGui::FindWindowByName("UI errors") == nullptr, check);
            }
            if (std::string(pack) == "gamepad") {
                // What's new is a page of its own there (test.login = notes, as the screen appears again)
                REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.login', 'notes')") == LUA_OK);
                s.host.login = nullptr;
                s.frames(2);
                s.host.login = &login;
                s.frames(3);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            login.update = {};
            login.release_notes.clear();
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // The add-on manager's panel (docs/addons.md): an install card, and rows of each group and state.
            login.addons.open = true;
            login.addons.available = true;
            login.addons.link = "https://github.com/owner/autoloot";
            login.addons.status = {"Looking at https://github.com/owner/autoloot...", false};
            login.addons.server = "Alpha";
            ui::AddonOfferView offer;
            offer.name = "autoloot";
            offer.kind = "plugin";
            offer.version = "1.3.0";
            offer.author = "Some One";
            offer.description = "Loots what you tell it to.";
            offer.source = "owner/autoloot";
            offer.page = "https://github.com/owner/autoloot";
            offer.key = "0123456789abcdef";
            offer.caps = {"read the game state", "draw windows"};
            offer.blocked = "an add-on named autoloot is installed already";
            login.addons.offer = offer;
            for (const char* state : {"uptodate", "available", "downloading", "ready", "approval", "modified",
                                      "blocked", "failed"}) {
                ui::AddonRowView r;
                r.name = std::string("addon-") + state;
                r.kind = "script";
                r.group = "installed";
                r.version = "1.0.0";
                r.source = "owner/" + r.name;
                r.page = "https://github.com/owner/" + r.name;
                r.state = state;
                r.state_text = std::string("State ") + state;
                r.caps = {"read the game state"};
                if (std::string(state) == "approval") r.new_caps = {"speak for you"};
                if (std::string(state) == "modified") r.modified = {"init.lua"};
                r.can_update = r.can_remove = true;
                r.auto_update = true;
                r.folder = "/x/scripts/" + r.name;
                login.addons.rows.push_back(r);
            }
            ui::AddonRowView manual;
            manual.name = "byhand";
            manual.kind = "plugin";
            manual.group = "manual";
            manual.note = "This server runs only its own plugins: it does not run here.";
            manual.folder = "/x/plugins/byhand";
            login.addons.rows.push_back(manual);
            ui::AddonRowView server = manual;
            server.name = "hud";
            server.group = "server";
            server.note = "Provided by Alpha: you allowed it, so it runs there.";
            server.consent = "allowed";
            login.addons.rows.push_back(server);
            server.name = "map";
            server.consent = "denied";
            login.addons.rows.push_back(server);
            // An installed tool (a program of its own): Start.
            ui::AddonRowView tool;
            tool.name = "texstudio";
            tool.kind = "tool";
            tool.group = "installed";
            tool.version = "1.0.0";
            tool.source = "owner/texstudio";
            tool.state = "uptodate";
            tool.can_remove = tool.can_start = true;
            tool.folder = "/x/tools/texstudio";
            login.addons.rows.push_back(tool);
            // The store (docs/addons.md "Finding add-ons"): an item in each state, then the list being fetched, then
            // an empty one.
            for (const char* state : {"install", "update", "installed", "manual", "unavailable"}) {
                ui::AddonStoreItemView it;
                it.name = std::string("store-") + state;
                it.source = "owner/" + it.name;
                it.page = "https://github.com/" + it.source;
                it.kind = std::string(state) == "unavailable" ? "" : "plugin";
                it.version = "2.0.0";
                it.author = "Some One";
                it.description = "Does a thing for you, described in a sentence long enough to wrap in the card.";
                it.worlds = std::string(state) == "install" ? "Alpha World" : "";
                it.installed_version = std::string(state) == "update" || std::string(state) == "installed" ? "1.0.0" : "";
                it.state = state;
                it.problem = std::string(state) == "unavailable" ? "store-unavailable has no build for linux-x64" : "";
                login.addons.store.push_back(it);
            }
            login.addons.browsed = true;
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // A tool's install card: no permissions, the program's warning.
            login.addons.offer->kind = "tool";
            login.addons.offer->caps.clear();
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.offer->kind = "plugin";
            login.addons.offer->caps = {"read the game state", "draw windows"};
            // An older version than the one installed: its warning and the button that says so.
            login.addons.offer->installed_version = "1.4.0";
            login.addons.offer->older = true;
            login.addons.offer->blocked.clear();
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.offer->older = false;
            login.addons.offer->installed_version.clear();
            login.addons.offer->blocked = "an add-on named autoloot is installed already";
            if (std::string(pack) == "modern") {
                // the list of their own, then back to the store
                REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.addontab', 'mine')") == LUA_OK);
                s.frames(3);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.addontab', 'store')") == LUA_OK);
            }
            login.addons.store.clear();
            login.addons.browsing = true;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.browsing = false;
            login.addons.store_error = "The list of add-ons could not be fetched.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.offer.reset();
            login.addons.available = false;
            login.addons.why_off = "Add-ons cannot be fetched here.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons = {};
            // Suggested add-ons (docs/addons.md "Suggested add-ons"): the index's and a server's.
            login.addons.suggestions = {
                {"servplug", "owner/servplug", "Alpha World has a Hailstone add-on for its own features: servplug.", false},
                {"", "owner/extra", "Alpha suggests a Hailstone add-on: owner/extra.", true}};
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons = {};
            s.frames(2);
            ui::new_profile(login);
            s.frames(2);
            login.transient = true;
            login.busy = true;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.login = nullptr;

            ui::ServerSelectView worlds;
            worlds.servers = {{"Open World", "Up", true, true}, {"Locked World", "Locked", true},
                             {"Down World", "Down", false}};
            worlds.selected = 0;
            s.host.server_select = &worlds;
            s.host.last_server = "Open World";
            s.host.login_server = "Alpha";
            s.frames(3);
            CHECK(s.api.replaces("serverselect"));
            REQUIRE(luaL_dostring(s.lua.state(), "local v=eq.ServerSelect(); assert(v.servers[2].status=='Locked' and v.servers[2].canJoin); "
                                     "assert(eq.ServerSelectDo('play',2)); assert(not eq.ServerSelectDo('play',3))") == LUA_OK);
            CHECK(s.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Play));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            worlds.addon_hint = "Open World has a Hailstone add-on: go Back and open Add-ons to look at it.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.server_select = nullptr;

            // Character select, with characters and without; with the scene and without.
            ui::CharacterSelectView chars;
            chars.server_name = "World One";
            chars.characters = {{"Bob", 12, 1, {1}, "North Qeynos"}, {"Cid", 50, 2, {5, 2}, "Kaladim"}};
            chars.selected = 1;
            chars.scene = true;
            s.host.ui_ready = true;
            s.host.game_state = 1;
            s.host.char_select = &chars;
            s.frames(3);
            CHECK(s.api.replaces("CharacterListWnd"));
            chars.characters.clear();
            chars.selected = -1;
            chars.scene = false;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.char_select = nullptr;

            // Character creation, with the scene and without, waiting on the world, and while customizing.
            ui::CharacterCreateView create;
            auto opt = [](std::uint32_t race, std::uint32_t cls, std::uint32_t deity, std::uint32_t zone) {
                ui::CreateOption o{race, cls, deity, zone, {}, {}};
                o.base = {75, 75, 75, 75, 75, 75, 75};
                o.points = {10, 0, 0, 10, 0, 0, 5};
                return o;
            };
            for (std::uint32_t race : {1u, 2u, 3u, 128u})
                for (std::uint32_t cls : {1u, 2u, 3u, 11u}) create.options.push_back(opt(race, cls, 201, 2));
            create.options.push_back(opt(1, 1, 202, 9));
            create.scene = true;
            s.host.game_state = 2;
            s.host.char_create_view = &create;
            s.frames(3);
            CHECK(s.api.replaces("CharacterCreateScreen"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            create.name = "Bobby";
            create.scene = false;
            create.status = {"That name is taken.", true};
            s.frames(2);
            create.busy = true;
            s.frames(2);
            s.host.customization_shown = true;
            s.frames(2);
            s.host.customization_shown = false;
            create.options.clear(); // no tables from the server
            create.option = -1;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.char_create_view = nullptr;
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the marketplace draws its shelves, details and pages from a filled store") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_marketplace_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            auto& m = s.conn.marketplace();
            for (std::uint32_t i = 0; i < 6; ++i) {
                net::MarketplaceEntry e;
                e.id = 100 + i;
                e.name = "Item " + std::to_string(i);
                e.category = i < 3 ? "Gear/Head" : "Fun";
                e.price = 10 * (i + 1);
                e.description = "Line one<BR>marketid=101";
                e.is_new = i == 0;
                e.is_featured = i == 4;
                e.was_price = i == 2 ? 99 : 0;
                e.available_until = i == 1 ? 4102444800 : -1;
                e.item_of_week = i == 4;
                m.catalog.push_back(e);
            }
            m.window.populate(m.catalog, true);
            for (std::uint32_t i = 0; i < 6; ++i) m.window.apply_state(m.catalog, {100 + i, 0, i == 0 ? 1001u : 0u, 0, i == 3 ? "Maker" : ""});
            m.window.rebuild_tree(true);
            m.window.refilter(m.catalog);
            m.wallet = 25;
            m.open = true;
            lua_State* L = s.lua.state();
            // The landing page, the grid, the list, then the confirmation and result pages.
            for (int step = 0; step < 6; ++step) {
                INFO(step);
                if (step == 1) m.window.click_category(m.catalog, 1);
                if (step == 2) m.window.set_view(game::MarketplaceView::List);
                if (step == 3) m.window.page = game::MarketplacePage::TaxInfo;
                if (step == 4) m.window.page = game::MarketplacePage::Confirm;
                if (step == 5) {
                    m.window.page = game::MarketplacePage::Results;
                    m.result_text = "Done.";
                    m.window.preview_tab = true;
                }
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            REQUIRE(luaL_dostring(L, "local n = eq.ListInfo('MarketplaceWnd', 'MKPW_CategoriesTree') return n") == LUA_OK);
            CHECK(lua_tointeger(L, -1) >= 4);
            lua_settop(L, 0);
        }
        ImGui::DestroyContext(ctx);
    }
}

namespace {

// spells_us.txt lines: id and name (the rest 0).
std::shared_ptr<game::SpellTable> book_spells() {
    auto line = [](const char* id, const char* name) {
        std::vector<std::string> f(237, "0");
        f[0] = id;
        f[1] = name;
        std::string out;
        for (std::size_t i = 0; i < f.size(); ++i) out += (i ? "^" : "") + f[i];
        return out;
    };
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line("200", "Minor Healing") + "\n" + line("201", "Courage") + "\n" + line("202", "Strike")) == 3);
    return table;
}

} // namespace

TEST_CASE("UI packs: the spell book's sets and set editor draw; eq.SaveSpellSet / EditSpellSet save, memorizing nothing") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_spellsets_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            // the book opens only in a zone (CSpellBookWnd::AboutToShow): the mock EQEmu's connection in place of
            // the session's own
            hailstone::test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            conn.set_spell_table_for_test(book_spells());
            REQUIRE(conn.profile());
            net::PlayerProfile pp = *conn.profile();
            pp.class_ = 2; // a cleric: the book opens
            conn.set_profile_for_test(pp);
            for (std::uint32_t i = 0; i < 3; ++i)
                conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{i, 200 + i, net::kMemScribe, 0}));
            REQUIRE(conn.spell_state().book[2] == 202);
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                INFO(code);
                REQUIRE(luaL_dostring(L, code) == LUA_OK);
            };
            auto number = [&](const char* code) -> lua_Integer {
                run(code);
                lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : -100;
                lua_settop(L, 0);
                return v;
            };
            const auto& sets = conn.spell_loadouts();
            const auto gems_before = conn.spell_state().gems;
            // Saved from a table, not the gems: a spell not in the book (999) is kept; nil, -1, negatives and
            // non-numbers are empty gems.
            CHECK(number("return eq.SaveSpellSet('Heals', { 200, -1, 201, nil, 999 })") == 1);
            CHECK(sets.at(0).name == "Heals");
            CHECK(sets.at(0).spells[0] == 200);
            CHECK(sets.at(0).spells[1] == -1);
            CHECK(sets.at(0).spells[2] == 201);
            CHECK(sets.at(0).spells[3] == -1);
            CHECK(sets.at(0).spells[4] == 999);
            CHECK(number("return eq.SaveSpellSet('', { 200 }) or -1") == -1);
            CHECK(number("return eq.EditSpellSet(1, 'Alpha', { 'x', 202, -5 })") == 1);
            CHECK(sets.at(0).name == "Alpha");
            CHECK(sets.at(0).spells[0] == -1);
            CHECK(sets.at(0).spells[1] == 202);
            CHECK(sets.at(0).spells[2] == -1);
            CHECK(number("return eq.EditSpellSet(2, 'Beta', {}) or -1") == -1); // set 2 is not in use
            CHECK(number("return eq.SaveSpellSet('Pulls', { 202, 201 })") == 2);
            // nothing was memorized: no gem, no memorize, no set being memorized
            for (int g = 0; g < game::kSpellGems; ++g)
                CHECK(conn.spell_state().gems[static_cast<std::size_t>(g)].spell ==
                      gems_before[static_cast<std::size_t>(g)].spell);
            CHECK_FALSE(conn.spell_state().memorizing.has_value());
            CHECK_FALSE(conn.memorizing_set());
            run("local t = eq.SpellSets() assert(t[1].name == 'Alpha' and t[1].spells[2] == 202 and t[2].name == 'Pulls')");

            // The views: the list of sets, the editor of set 1, a new set, a new set from the gems, back.
            run("eq.ShowWindow('SpellBookWnd', true)");
            REQUIRE(conn.spell_book_open());
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            const bool modern = std::string(pack) == "modern";
            const char* views[] = {"sets", "1", "new", "gems", "2"};
            const char* gamepad_views[] = {"sets", "edit1", "editnew", "edit2", "book"};
            for (int v = 0; v < 5; ++v) {
                std::string code = modern ? std::string("ui.SetSetting('test.book.edit', '") + views[v] + "')"
                                          : std::string("ui.SetSetting('test.book.view', '") + gamepad_views[v] + "')";
                if (modern && v == 0) code = "ui.SetSetting('test.book.tab', 'sets')";
                INFO(code);
                run(code.c_str());
                s.frames(3);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            CHECK(ImGui::FindWindowByName(modern ? "###modern.SpellBookWnd" : "Spell Book") != nullptr);
            // all ten in use: a new name is refused, an existing one still saves
            for (int i = 0; i < 8; ++i) {
                std::string code = "return eq.SaveSpellSet('Set" + std::to_string(i) + "', { 200 })";
                CHECK(number(code.c_str()) > 0);
            }
            CHECK(number("return eq.SaveSpellSet('Eleventh', { 200 }) or -1") == -1);
            CHECK(number("return eq.SaveSpellSet('pulls', { 201 })") > 0);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// Two widgets of one window with the same ID make ImGui's "conflicting ID" popup show when either is hovered: the
// Memorized filter and the details pane's Memorize button were both "##mem". The mouse crosses the whole book (in
// the details view, a spell selected) and no hovered item may share its ID.
// A row's Remove in the add-ons card asks first, then removes: the question must open (it was opened from inside the
// row's ID scope while drawn at the window's, so it never showed and nothing was removed), for a script and a plugin.
TEST_CASE("UI packs (modern): an add-on's Remove asks, and its answer removes that add-on") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_addon_remove_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        s.host.in_game = s.host.ui_ready = false;
        lua_State* L = s.lua.state();
        // Every row unfolded; a button labelled `press` reports a click at its `press_at`th drawing in a frame; the
        // texts drawn are kept.
        REQUIRE(luaL_dostring(L, R"(
            eq.InGame = function() return false end
            press, press_at, seen, texts = nil, 0, 0, {}
            local tn, b, t = ui.TreeNode, ui.Button, ui.Text
            ui.TreeNode = function(l) tn(l) return true end
            ui.Button = function(l, ...)
              local r = b(l, ...)
              if l == press then
                seen = seen + 1
                if seen == press_at then return true end
              end
              return r
            end
            ui.Text = function(x, ...) texts[#texts + 1] = tostring(x) return t(x, ...) end
            ui.SetSetting('test.addontab', 'mine')
        )") == LUA_OK);
        ui::LoginView login;
        login.servers_dir = dir / "servers";
        login.ui = login.default_ui = "modern";
        login.addons.open = true;
        for (const char* kind : {"script", "plugin"}) {
            ui::AddonRowView r;
            r.name = std::string("my") + kind;
            r.kind = kind;
            r.group = "installed";
            r.version = "1.0.0";
            r.can_remove = true;
            login.addons.rows.push_back(r);
        }
        s.host.login = &login;
        s.frames(3);
        REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        auto asked = [&](const std::string& q) {
            REQUIRE(luaL_dostring(L, "return table.concat(texts, '|')") == LUA_OK);
            const std::string all = lua_tostring(L, -1);
            lua_settop(L, 0);
            return all.find(q) != std::string::npos;
        };
        int row = 0;
        for (const char* name : {"myscript", "myplugin"}) {
            INFO(name);
            ++row;
            s.host.login_action.reset();
            // The row's own Remove (the rows' buttons come in list order).
            REQUIRE(luaL_dostring(L, ("press, press_at, seen, texts = 'Remove', " + std::to_string(row) + ", 0, {}").c_str())
                    == LUA_OK);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "press, seen, texts = nil, 0, {}") == LUA_OK);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(asked(std::string("Remove ") + name + "?"));
            CHECK_FALSE(s.host.login_action);
            // The question's Remove: drawn after both rows' buttons.
            REQUIRE(luaL_dostring(L, "press, press_at, seen, texts = 'Remove', 3, 0, {}") == LUA_OK);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "press, seen = nil, 0") == LUA_OK);
            CHECK(s.host.login_action == static_cast<int>(ui::LoginAction::AddonRemove));
            CHECK(login.addons.choice == name);
            s.frames(2);
        }
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): no two of the spell book's widgets share an ID") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_spellbook_ids_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = true;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        hailstone::test::Rig rig;
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        s.host.conn = &conn;
        conn.set_spell_table_for_test(book_spells());
        REQUIRE(conn.profile());
        net::PlayerProfile pp = *conn.profile();
        pp.class_ = 2; // a cleric: the book opens
        conn.set_profile_for_test(pp);
        for (std::uint32_t i = 0; i < 3; ++i)
            conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{i, 200 + i, net::kMemScribe, 0}));
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "eq.ShowWindow('SpellBookWnd', true) ui.SetSetting('test.book.tab', 'details')"
                                 " ui.SetSetting('test.book.select', '200')") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        ImGuiWindow* book = ImGui::FindWindowByName("###modern.SpellBookWnd");
        REQUIRE(book != nullptr);
        const ImVec2 at = book->Pos, size = book->Size;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        int conflicts = 0;
        for (float y = at.y + 4; y < at.y + size.y; y += 12) { // widgets here are 24 px tall or more
            for (float x = at.x + 4; x < at.x + size.x; x += 16) {
                ImGui::GetIO().AddMousePosEvent(x, y);
                s.frames(1);
                if (g.DebugDrawIdConflicts != 0) {
                    ++conflicts;
                    MESSAGE("conflicting ID hovered at " << x - at.x << ", " << y - at.y);
                }
            }
        }
        CHECK(conflicts == 0);
    }
    ImGui::DestroyContext(ctx);
}

// With every usable gem full, the details pane's Memorize button (and a double-click, which takes the same gem) has
// no empty gem to use. A gem picked in the strip takes the spell in place of its own when Hailstone's
// replace_gem_spell is on ("Replace gem N"); off, the button stays "No free gem" and does nothing, as the original
// memorizes only into an empty gem. (A bug report: a bard with twelve full gems could only memorize after forgetting
// one.)
TEST_CASE("UI packs (modern): the spell book's Memorize replaces the spell of a picked full gem") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    for (bool replace : {true, false}) {
        INFO("replace_gem_spell " << replace);
        hailstone::test::ScratchDir dir_scratch("ui_pack_spellbook_replace_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / "modern", fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, "modern");
            hailstone::test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            s.host.get_option = [replace](std::string_view o) { return o == "replace_gem_spell" ? (replace ? 1 : 0) : 0; };
            s.host.find_option = [replace](std::string_view o) -> std::optional<int> {
                if (o == "replace_gem_spell") return replace ? 1 : 0;
                return std::nullopt;
            };
            // spells 200..212 scribed into book slots 0..12; 200.. memorized into every usable gem, 212 left over
            std::string lines;
            for (int i = 0; i <= 12; ++i) {
                std::vector<std::string> f(237, "0");
                f[0] = std::to_string(200 + i);
                f[1] = "Spell " + std::to_string(i);
                for (std::size_t k = 0; k < f.size(); ++k) lines += (k ? "^" : "") + f[k];
                lines += "\n";
            }
            auto table = std::make_shared<game::SpellTable>();
            REQUIRE(table->parse(lines) == 13);
            conn.set_spell_table_for_test(table);
            REQUIRE(conn.profile());
            net::PlayerProfile pp = *conn.profile();
            pp.class_ = 2; // a cleric: the book opens
            conn.set_profile_for_test(pp);
            for (std::uint32_t i = 0; i <= 12; ++i)
                conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{i, 200 + i, net::kMemScribe, 0}));
            const int usable = conn.usable_gems();
            REQUIRE(usable >= 3);
            for (int g = 0; g < usable; ++g)
                conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{
                    static_cast<std::uint32_t>(g), 200 + static_cast<std::uint32_t>(g), net::kMemMemorize, 0}));
            conn.set_sitting(true);
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, "eq.ShowWindow('SpellBookWnd', true) ui.SetSetting('test.book.tab', 'details')"
                                     " ui.SetSetting('test.book.select', '212') ui.SetSetting('test.book.gem', '3')") ==
                    LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            ImGuiWindow* book = ImGui::FindWindowByName("###modern.SpellBookWnd");
            REQUIRE(book != nullptr);
            // the mouse over the Memorize button, found by its ID
            ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiID button = book->GetID("##memorize");
            bool over = false;
            const ImVec2 at = book->Pos, size = book->Size;
            for (float y = at.y + 4; y < at.y + size.y && !over; y += 12) {
                for (float x = at.x + 4; x < at.x + size.x && !over; x += 16) {
                    ImGui::GetIO().AddMousePosEvent(x, y);
                    s.frames(1);
                    over = g.HoveredId == button;
                }
            }
            REQUIRE(over);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            s.frames(1);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (replace) {
                REQUIRE(conn.spell_state().memorizing.has_value());
                CHECK(conn.spell_state().memorizing->gem == 2);
                CHECK(conn.spell_state().memorizing->spell == 212);
                CHECK(conn.spell_state().gems[2].spell == 202); // kept until the server answers
            } else {
                CHECK_FALSE(conn.spell_state().memorizing.has_value());
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the inventory's alternate currencies draw, and a row selects the stock list's row") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_currency_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);

        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            std::vector<net::AltCurrencyEntry> entries{{31, 22000, 1500, 1000, true}, {32, 22001, 1501, 500, true}};
            s.conn.receive_zone_packet_for_test(net::encode_alt_currency_populate(entries));
            s.conn.receive_zone_packet_for_test(net::encode_alt_currency_update({31, 7}));
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, "ui.SetSetting('test.inv.page', 'currency') eq.ShowWindow('InventoryWindow', true)") ==
                    LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // the page keeps the stock Display Missing off with nothing held (modern: Show all off)
            REQUIRE(luaL_dostring(L, "return (select(2, eq.ChildState('InventoryWindow', 'AltCurr_DisplayMissingButton')))") ==
                    LUA_OK);
            CHECK_FALSE(lua_toboolean(L, -1));
            lua_settop(L, 0);
            REQUIRE(luaL_dostring(L, "eq.ListClick('InventoryWindow', 'AltCurr_PointList', 0, 'left') "
                                     "return (eq.ChildState('InventoryWindow', 'AltCurr_CreateItemButton'))") == LUA_OK);
            CHECK(lua_toboolean(L, -1)); // 31, held: Claim is on
            lua_settop(L, 0);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// Hailstone's Find window (app/lua_eq_find.cpp): the find key's state opens it (modern: the map on its Find page;
// gamepad: a window of its own), it draws the zone's NPCs, and the map closing closes it.
TEST_CASE("UI packs: the Find window draws the zone's NPCs and follows eq.FindOpen") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_find_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);

        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            auto& world = s.conn.world_for_test();
            auto add = [&](std::uint32_t id, const char* name, std::uint8_t cls, float x) {
                game::Spawn sp;
                sp.id = id;
                sp.name = name;
                sp.class_id = cls;
                sp.is_npc = true;
                sp.position = {x, 0, 0};
                world.spawns[id] = sp;
            };
            add(2, "Arms_Dealer01", 41, 10);
            add(3, "Banker_Bob00", 40, 20);
            add(4, "Wilt00", 20, 30);
            ++world.spawn_set_serial;
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, "ui.SetSetting('map.npcMarks', '1') eq.FindOpen(true)") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (std::string(pack) == "modern") {
                CHECK(s.api.shown("MapViewWnd")); // the map opened on its Find page
                REQUIRE(luaL_dostring(L, "eq.ShowWindow('MapViewWnd', false)") == LUA_OK);
                s.frames(2);
                CHECK_FALSE(s.host.find_open); // the map closing closed Find
                REQUIRE(luaL_dostring(L, "eq.FindOpen(true)") == LUA_OK);
                s.frames(2);
                CHECK(s.api.shown("MapViewWnd"));
                s.host.find_open = false; // the find key again: the map goes
                s.frames(2);
                CHECK_FALSE(s.api.shown("MapViewWnd"));
            } else {
                REQUIRE(luaL_dostring(L, "eq.FindOpen(false)") == LUA_OK);
                s.frames(2);
            }
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// The modern extended target window as a library (ui/modern/lib/xtarget.lua): a window of the same name that calls
// it with a row part gets its style and draw called for each filled slot, with the slot number, and draws cleanly.
TEST_CASE("UI packs (modern): lib/xtarget.lua takes a plugin's part on each extended target row") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_xtarget_lib_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    // The test's window, a plugin's (its own folder of windows), takes the pack's place.
    fs::create_directories(dir / "plug");
    std::ofstream(dir / "plug" / "xtarget_cover.lua") << R"lua(
local xtarget = dofile("lib/xtarget.lua")
drawn, styled = {}, {}
ui.window("Extended Target", function()
  xtarget.window({ width = 262, row = { width = 52,
    style = function(r) styled[#styled + 1] = r.n return r.n == 1 and { alpha = 0.45, strike = true, tag = { "IGNORED" } } or nil end,
    draw = function(r, x0, y0, x1, y1, s) drawn[#drawn + 1] = string.format("%d:%s:%d", r.n, r.name, math.floor(x1 - x0 + 0.5)) end,
  } })
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        for (auto [slot, id, name] : {std::tuple{0u, 101u, std::string("a gnoll")}, std::tuple{1u, 102u, std::string("a pup")}}) {
            ByteWriter b;
            b.le<std::uint32_t>(5);
            b.le<std::uint32_t>(1);
            b.le<std::uint32_t>(slot);
            b.le<std::uint8_t>(1);
            b.le<std::uint32_t>(id);
            b.fixed_string(name, name.size() + 1);
            s.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::XTargetResponse), b.take()));
            game::Spawn& m = s.conn.world_for_test().spawns[id];
            m.id = id;
            m.name = name;
            m.is_npc = true;
            m.hp_percent = 80;
        }
        s.lua.set_folders({{"plug", dir / "plug"}});
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "eq.ShowWindow('ExtendedTargetWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        // (the plugin's globals are its own: run in them)
        REQUIRE(luaL_loadstring(L, "return table.concat(drawn, ','), #styled") == LUA_OK);
        s.lua.push_folder_env_for_test("plug");
        lua_setupvalue(L, -2, 1);
        REQUIRE(lua_pcall(L, 0, 2, 0) == LUA_OK);
        std::string out = lua_tostring(L, -2) ? lua_tostring(L, -2) : "";
        INFO(out);
        CHECK(out.find("0:a gnoll:") != std::string::npos);
        CHECK(out.find("1:a pup:") != std::string::npos);
        CHECK(out.find("0:a gnoll:52") != std::string::npos); // the part's width, at UI scale 1
        CHECK(out.find("2:") == std::string::npos);          // an empty slot gets no part
        CHECK(lua_tointeger(L, -1) >= 2);
        lua_settop(L, 0);
    }
    ImGui::DestroyContext(ctx);
}

// The modern AA window as a library (ui/modern/lib/aa.lua): a window of the same name that calls it with a row part,
// a details part and a banner gets each called, the row's with the ability's name, ranks and cost, and draws cleanly.
// Its one list (AAWindow's ModernList) holds every tab's abilities, with the tab, era and activated columns.
TEST_CASE("UI packs (modern): lib/aa.lua takes a script's part on each AA row, in the details and as a banner") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_aa_lib_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    std::ofstream(dir / "game" / "dbstr_us.txt")
        << "9001^1^Innate Strength^0^\n9002^1^Mystical Shielding^0^\n5^20^The Planes of Power^0^\n7^19^Defense^0^\n";
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    fs::create_directories(dir / "plug");
    std::ofstream(dir / "plug" / "aa_cover.lua") << R"lua(
local aa = dofile("lib/aa.lua")
drawn, styled, details, banners = {}, {}, 0, 0
ui.window("Alternate Advancement", function()
  aa.window({
    row = { width = 40,
      style = function(r) styled[#styled + 1] = r.name return { ring = { 1, 0.8, 0.2, 1 }, tag = { "AUTO" } } end,
      draw = function(r, x0, y0, x1, y1, s)
        drawn[#drawn + 1] = string.format("%s:%d/%d:%s:%d", r.name, r.rank, r.max, r.cost, math.floor(x1 - x0 + 0.5))
      end },
    details = function(sel, x, y, w, s) details = details + 1 return 20 * s end,
    banner = function(x, y, w, s) banners = banners + 1 return 24 * s end,
  })
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        net::PlayerProfile pp;
        pp.class_ = 2;
        pp.level = 60;
        s.conn.set_profile_for_test(pp);
        for (auto [id, title, cost] : {std::tuple{100u, 9001, 2}, std::tuple{200u, 9002, 5}}) {
            game::AltAbility a;
            a.id = id;
            a.group = static_cast<std::int32_t>(id / 10);
            a.rank = 1;
            a.max_rank = 3;
            a.cost = cost;
            a.level_req = 10;
            a.classes = 1u << 2;
            a.type = id == 100 ? 1 : 3; // General, Class
            a.title_sid = title;
            if (id == 200) {
                a.spell = 1234; // activated
                a.refresh = 60;
                a.expansion = 5;
                a.category = 7;
            }
            a.prereq_groups = {0};
            a.prereq_ranks = {0};
            s.conn.receive_zone_packet_for_test(net::encode_send_aa_table(a));
        }
        s.lua.set_folders({{"plug", dir / "plug"}});
        lua_State* L = s.lua.state();
        // the one list: both tabs' abilities, each with its tab, era and activated
        REQUIRE(luaL_dostring(L, R"lua(
local out = {}
for _, t in ipairs(eq.WindowList('AAWindow', 'ModernList') or {}) do out[#out + 1] = table.concat(t, '|') end
return table.concat(out, ';'))lua") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) ==
              "Innate Strength|0/3|2||1||0|0;Mystical Shielding|0/3|5|Defense|3|The Planes of Power|1|5");
        lua_settop(L, 0);
        REQUIRE(luaL_dostring(L, "eq.ShowWindow('AAWindow', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_loadstring(L, "return table.concat(drawn, ','), #styled, banners") == LUA_OK);
        s.lua.push_folder_env_for_test("plug");
        lua_setupvalue(L, -2, 1);
        REQUIRE(lua_pcall(L, 0, 3, 0) == LUA_OK);
        std::string out = lua_tostring(L, -3) ? lua_tostring(L, -3) : "";
        INFO(out);
        CHECK(out.find("Innate Strength:0/3:2:40") != std::string::npos); // the part's width, at UI scale 1
        CHECK(out.find("Mystical Shielding:0/3:5:40") != std::string::npos);
        CHECK(lua_tointeger(L, -2) >= 2);
        CHECK(lua_tointeger(L, -1) >= 1);
        lua_settop(L, 0);
        // A selected ability brings the details part; a row of the one list selects that ability (the second row:
        // the Class tab's), and the list and the stock tab's list both show it selected.
        REQUIRE(luaL_dostring(L, "eq.ListClick('AAWindow', 'ModernList', 1, 'left')") == LUA_OK);
        s.frames(2);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "local _, _, a = eq.ListInfo('AAWindow', 'ModernList') local _, _, b = "
                                 "eq.ListInfo('AAWindow', 'List3') return a, b, (eq.WindowText('AAWindow', "
                                 "'ModernSelected'))") == LUA_OK);
        CHECK(lua_tointeger(L, -3) == 1);
        CHECK(lua_tointeger(L, -2) == 0);
        CHECK(std::string(lua_tostring(L, -1)).starts_with("200\tMystical Shielding\t"));
        lua_settop(L, 0);
        REQUIRE(luaL_loadstring(L, "return details") == LUA_OK);
        s.lua.push_folder_env_for_test("plug");
        lua_setupvalue(L, -2, 1);
        REQUIRE(lua_pcall(L, 0, 1, 0) == LUA_OK);
        CHECK(lua_tointeger(L, -1) >= 1);
        lua_settop(L, 0);
    }
    ImGui::DestroyContext(ctx);
}

// Once the selected rank is bought, the AA window selects the ability's next rank (the one its list now shows), so
// that Train is ready for it without a click on the row again.
TEST_CASE("UI packs (modern): the AA window's selection follows its ability to the next rank once bought") {
    hailstone::test::ScratchDir dir_scratch("ui_pack_aa_follow_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        Session s(dir, "modern");
        net::PlayerProfile pp;
        pp.class_ = 2;
        pp.level = 60;
        s.conn.set_profile_for_test(pp);
        for (auto [id, r, prev, next] : {std::tuple{100u, 1, -1, 101}, std::tuple{101u, 2, 100, 102},
                                         std::tuple{102u, 3, 101, -1}}) {
            game::AltAbility a;
            a.id = id;
            a.group = 10;
            a.rank = r;
            a.prev_id = prev;
            a.next_id = next;
            a.max_rank = 3;
            a.cost = 1;
            a.classes = 1u << 2;
            a.type = 1;
            a.prereq_groups = {0};
            a.prereq_ranks = {0};
            s.conn.receive_zone_packet_for_test(net::encode_send_aa_table(a));
        }
        lua_State* L = s.lua.state();
        auto selected = [&] {
            REQUIRE(luaL_dostring(L, "eq.WindowList('AAWindow', 'ModernList') "
                                     "return (eq.WindowText('AAWindow', 'ModernSelected'))") == LUA_OK);
            std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return out.substr(0, out.find('\t'));
        };
        REQUIRE(luaL_dostring(L, "eq.WindowList('AAWindow', 'ModernList') "
                                 "eq.ListClick('AAWindow', 'ModernList', 0, 'left')") == LUA_OK);
        lua_settop(L, 0);
        CHECK(selected() == "100");
        auto own = [&](std::uint32_t id) {
            net::RespondAA r;
            r.owned = {{id, 1, 0}};
            s.conn.receive_zone_packet_for_test(net::encode(r));
        };
        own(100);
        CHECK(selected() == "101");
        own(101);
        CHECK(selected() == "102");
        own(102); // the last rank bought: it stays selected
        CHECK(selected() == "102");
    }
    ImGui::DestroyContext(ctx);
}

// The modern AA window's list starts sorted by name, A to Z (not the game's order); a click on a column's head sorts
// by that column and a second click the other way; dragging a head moves the column. Both are kept as settings.
TEST_CASE("UI packs (modern): the AA list sorts by name, by a clicked head, and a head drags to move its column") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_aa_sort_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    std::ofstream(dir / "game" / "dbstr_us.txt") << "9001^1^Innate Strength^0^\n9002^1^Mystical Shielding^0^\n";
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    fs::create_directories(dir / "plug");
    // the row part records the rows in the order drawn, the part's left edge and the first row's top
    std::ofstream(dir / "plug" / "aa_sort.lua") << R"lua(
local aa = dofile("lib/aa.lua")
drawn, px, top = {}, 0, 1e9
ui.window("Alternate Advancement", function()
  drawn, top = {}, 1e9
  aa.window({ row = { width = 20, draw = function(r, x0, y0)
    drawn[#drawn + 1] = r.name
    px, top = x0, math.min(top, y0)
  end } })
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int tw = 0, th = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
    {
        Session s(dir, "modern");
        net::PlayerProfile pp;
        pp.class_ = 2;
        pp.level = 60;
        s.conn.set_profile_for_test(pp);
        // the game's order: Mystical Shielding (General) before Innate Strength (Class)
        for (auto [id, title, cost, type] : {std::tuple{100u, 9002, 2, 1}, std::tuple{200u, 9001, 5, 3}}) {
            game::AltAbility a;
            a.id = id;
            a.group = static_cast<std::int32_t>(id / 10);
            a.rank = 1;
            a.max_rank = 3;
            a.cost = cost;
            a.level_req = 10;
            a.classes = 1u << 2;
            a.type = type;
            a.title_sid = title;
            a.prereq_groups = {0};
            a.prereq_ranks = {0};
            s.conn.receive_zone_packet_for_test(net::encode_send_aa_table(a));
        }
        s.lua.set_folders({{"plug", dir / "plug"}});
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "eq.ShowWindow('AAWindow', true)") == LUA_OK);
        s.frames(3);
        REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        auto read = [&](double& part_x, double& top_y) {
            REQUIRE(luaL_loadstring(L, "return table.concat(drawn, ','), px, top") == LUA_OK);
            s.lua.push_folder_env_for_test("plug");
            lua_setupvalue(L, -2, 1);
            REQUIRE(lua_pcall(L, 0, 3, 0) == LUA_OK);
            std::string out = lua_tostring(L, -3) ? lua_tostring(L, -3) : "";
            part_x = lua_tonumber(L, -2);
            top_y = lua_tonumber(L, -1);
            lua_settop(L, 0);
            return out;
        };
        // (the window is the script's, so its settings are the script's own: "plugin.<tag>.<key>")
        auto setting = [&](const char* key) {
            REQUIRE(luaL_dostring(L, (std::string("return ui.GetSetting('plugin.plug.") + key + "') or ''").c_str()) ==
                    LUA_OK);
            std::string v = lua_tostring(L, -1);
            lua_settop(L, 0);
            return v;
        };
        double part_x = 0, top_y = 0;
        CHECK(read(part_x, top_y) == "Innate Strength,Mystical Shielding");
        // the cost head: right-aligned at the rows' right (short of the part), its text over the rows' first line
        const float cost_x = static_cast<float>(part_x) - 8.0f - 19.0f;
        const float head_y = static_cast<float>(top_y) - 10.0f;
        auto click = [&](float x, float y) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            s.frames(1);
            ImGui::GetIO().AddMouseButtonEvent(0, true);
            s.frames(1);
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            s.frames(2);
        };
        click(cost_x, head_y);
        CHECK(setting("aa.sort") == "cost:asc");
        CHECK(read(part_x, top_y) == "Mystical Shielding,Innate Strength");
        click(cost_x, head_y);
        CHECK(setting("aa.sort") == "cost:desc");
        CHECK(read(part_x, top_y) == "Innate Strength,Mystical Shielding");
        // dragged left of every head, the cost column goes first; the drag does not sort
        ImGui::GetIO().AddMousePosEvent(cost_x, head_y);
        s.frames(1);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        s.frames(1);
        for (int i = 1; i <= 10; ++i) {
            ImGui::GetIO().AddMousePosEvent(cost_x - static_cast<float>(i) * 60.0f, head_y);
            s.frames(1);
        }
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        s.frames(2);
        CHECK(setting("aa.columns").starts_with("cost,name,"));
        CHECK(setting("aa.sort") == "cost:desc");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// Every row the EQ menu can show (RoF2's CascadeMenu.txt, other skins' and later clients' names for the same
// windows, the rows this client adds) has a line icon of its own in both packs, not the fallback dot, and draws.
TEST_CASE("UI packs: every EQ menu row has a menu icon") {
    constexpr const char* kScript = R"lua(
local icons = dofile("lib/menuicons.lua")
local rows = {
  "Navigation", "Compass", "Find", "Map", "Atlas/Map",
  "Players", "Group Status", "Group Window", "Guild", "Raid", "Friends/Ignore", "Looking for Group",
  "Looking for Guild",
  "Actions", "Actions Window", "Combat Abilities", "Ability Book", "Target", "Extended Target", "Hotkeys",
  "Hotkeys 1", "Hotkeys 10", "Bandolier", "Alarm", "Alarm Window", "Aggro Meter",
  "Quests", "Expeditions", "Instance Information", "Quest Journal", "NPC Journal", "Storyline",
  "Character", "Player Information", "Player Info Window", "Pet", "Spell Gems", "Spell Book",
  "Alternate Advancement", "Buffs", "Short Duration Buffs", "Buffs (Short Duration)", "Leadership", "Skills",
  "Blocked Buffs", "Blocked Pet Buffs", "Auras",
  "Miscellaneous", "Bazaar Search", "Barter Search", "Tribute", "PvP Stats", "Mail", "Mercenaries",
  "Inventory", "Options", "Help", "Everquest Help", "Lua Scripts", "Web Browser", "Report a Bug", "Camp & Quit",
  "Marketplace", "Real Estate", "Manage", "Plot Search", "Real Estate Items", "Plugins",
}
local missing = {}
ui.Begin("menu icons test")
for _, t in ipairs(rows) do
  if icons.Name(t) == "dot" then missing[#missing + 1] = t end
  icons.Draw(t, 40, 40, 18, { 1, 1, 1 }, 1)
end
ui.End()
return table.concat(missing, ", ")
)lua";
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_menu_icons_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            s.frames(1);
            ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
            s.api.begin_frame();
            ImGui::NewFrame();
            lua_State* L = s.lua.state();
            int rc = luaL_dostring(L, kScript);
            std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            INFO(out);
            CHECK(rc == LUA_OK);
            CHECK(out.empty()); // (on success: the rows with no icon of their own)
            lua_settop(L, 0);
            ImGui::Render();
        }
        ImGui::DestroyContext(ctx);
    }
}

#ifdef HAILSTONE_MOCK_WEB_HELPER
// The browser window in both packs with a browser behind it (the stand-in helper, tests/mock_web_helper.cpp, over
// real pipes): eq.Web* from the window's Lua, and the window drawing the tabs without script errors.
TEST_CASE("UI packs: the Lua Scripts window and eq.Scripts*: list, run, pause, stop, autorun") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_scripts_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::create_directories(dir / "scripts" / "other");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        std::ofstream(dir / "scripts" / "loop.lua", std::ios::binary) << "while true do require('mq').delay(10) end\n";
        std::ofstream(dir / "scripts" / "other" / "init.lua", std::ios::binary) << "print(1)\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            plugin::Hooks hooks;
            plugin::Policy policy;
            mq::Registry reg;
            mq::Context mctx;
            mq::ScriptEnv se;
            se.hooks = &hooks;
            se.policy = &policy;
            se.registry = &reg;
            se.context = [&]() -> const mq::Context& { return mctx; };
            se.dirs = {dir / "scripts"};
            se.config_dir = dir / "scripts" / "config";
            mq::ScriptHost scripts(se);
            Session s(dir, pack);
            s.host.scripts = &scripts;
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                const bool ok = luaL_dostring(L, code) == LUA_OK;
                if (!ok) std::fprintf(stderr, "lua: %s: %s\n", code, lua_tostring(L, -1));
                REQUIRE(ok);
            };
            // Before the zone-in's rules: listed, not allowed, with the reason; no character's list yet.
            run("local l, info = eq.Scripts(true); assert(#l == 2 and l[1].name == 'loop' and l[2].name == 'other');"
                "assert(not l[1].allowed and l[1].why:find('zone%-in') and l[1].status == '' and l[1].found);"
                "assert(not info.autorun and info.folders[1]:find('scripts'));"
                "local ok, why = eq.ScriptRun('loop'); assert(not ok and why:find('may not run'))");
            run("eq.ShowWindow('ScriptsWnd', true)");
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            policy.set(plugin::Rules{}); // no hailstone.cfg: the player's scripts run
            run("local l = eq.Scripts(true); assert(l[1].allowed and l[1].caps ~= '')");
            run("assert(eq.ScriptRun('loop', 'a \\\"b c\\\"'))");
            CHECK(scripts.running("loop"));
            run("local l = eq.Scripts(); assert(l[1].status == 'running' and not l[1].kept)");
            s.frames(2);
            bool drawn = false; // the window is there (not only free of errors)
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                drawn |= win->Active && std::string(win->Name).find("Lua Scripts") != std::string::npos;
            CHECK(drawn);
            run("assert(eq.ScriptPause('loop')); assert(eq.Scripts()[1].status == 'paused')");
            s.frames(2);
            run("assert(eq.ScriptStop('loop')); assert(not eq.ScriptStop('nope'))");
            CHECK_FALSE(scripts.running("loop"));
            // The character's autorun list: refused before a character, kept in its scripts.ini after.
            run("assert(not eq.SetScriptAutorun('other', true))");
            scripts.set_autorun_file(dir / "chars" / "scripts.ini");
            run("local _, info = eq.Scripts(); assert(info.autorun); assert(eq.SetScriptAutorun('other', true));"
                "local l = eq.Scripts(true); assert(l[2].autorun and not l[1].autorun)");
            CHECK(scripts.autorun("other"));
            CHECK(fs::exists(dir / "chars" / "scripts.ini"));
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("eq.ShowWindow('ScriptsWnd', false)");
            s.host.scripts = nullptr;
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the web browser window and eq.Web* with a browser") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_browser_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            web::BrowserHost browser([] { return web::process_transport({HAILSTONE_MOCK_WEB_HELPER}); });
            Session s(dir, pack);
            int wants_text = 0;
            app::WebService web({[&] { return &browser; }, [] { return nullptr; },
                                 [&] { return (dir / "web" / "server").string(); }, [] { return std::vector<std::string>{}; },
                                 [&] { ++wants_text; }});
            s.host.web = &web;
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                const bool ok = luaL_dostring(L, code) == LUA_OK;
                if (!ok) std::fprintf(stderr, "lua: %s: %s\n", code, lua_tostring(L, -1));
                REQUIRE(ok);
            };
            run("assert(eq.WebAvailable())");
            run("local id, why = eq.WebOpen('file:///etc/passwd'); assert(id == nil and why:find('not an http'))");
            run("TAB = eq.WebOpen('example.org', 64, 48); assert(TAB)");
            // A script's tab is not the player's: the window does not list it.
            web::TabOptions theirs;
            theirs.owner = "somescript";
            REQUIRE(browser.open("https://example.org/", theirs) != 0);
            run("assert(#eq.WebTabs() == 1 and eq.WebTabs()[1] == TAB)");
            run("eq.ShowWindow('BrowserWnd', true)");
            const web::Tab* t = browser.tab(1);
            REQUIRE(t);
            for (int i = 0; i < 2000 && t->title.empty(); ++i) {
                browser.poll(0);
                s.frames(1);
            }
            CHECK(t->title == "Mock https://example.org");
            CHECK(t->options.profile == (dir / "web" / "server").string());
            run("local st = eq.WebState(TAB); assert(st.url == 'https://example.org' and not st.closed)");
            run("local ok, why = eq.WebNavigate(TAB, 'file:///etc/passwd'); assert(not ok and why)");
            // Typed text without a scheme gets https:// in front: "javascript:..." is a (bad) web address, no script.
            run("assert(eq.WebNavigate(TAB, 'javascript:alert(1)'))");
            CHECK(t->url == "https://javascript:alert(1)");
            run("assert(eq.WebNavigate(TAB, 'https://example.org/b'))");
            run("eq.WebNav(TAB, 'reload'); eq.WebText(TAB, 'hi')");
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(wants_text == 0); // the page was never clicked: it has not the keyboard
            run("eq.ShowWindow('BrowserWnd', false)");
            // The page's input (eq.WebView): a click reaches the page (the stand-in paints the pixel white) and gives
            // it the keyboard; a click elsewhere takes the keyboard back.
            run("ui.window('ViewTest', function() ui.SetNextWindowPos(0, 0); ui.Begin('ViewTest');"
                " eq.WebView(TAB, 64, 48); VX, VY = ui.GetItemRect(); ui.End() end)");
            s.frames(1);
            lua_getglobal(L, "VX");
            lua_getglobal(L, "VY");
            const float vx = static_cast<float>(lua_tonumber(L, -2)), vy = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 2);
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(vx + 10, vy + 5);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddInputCharacter('z');
            s.frames(1);
            CHECK(wants_text > 0);
            // The clicked pixel (10, 5), once the page has the view's size (the browser window had it bigger).
            auto clicked = [&] {
                const std::size_t at = (5 * std::size_t{t->view.width} + 10) * 4;
                return t->view.width == 64 && t->view.bgra.size() > at && t->view.bgra[at] == std::byte{0xff};
            };
            for (int i = 0; i < 20000 && !clicked(); ++i) {
                browser.poll(0);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            CHECK(clicked());
            io.AddMousePosEvent(vx + 400, vy + 400);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            const int before = wants_text;
            s.frames(2);
            CHECK(wants_text == before); // the keyboard is back with the game
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("eq.WebClose(TAB); assert(eq.WebState(TAB) == nil)");

            // A plugin's window (its folder's ui/<pack>/): eq.Web* as "plugin:<name>", with the plugin's permissions.
            std::uint32_t plug_caps = plugin::kCapAll & ~plugin::kCapWeb;
            s.host.plugin_caps = [&](std::string_view name) { return name == "plugin:Plug" ? plug_caps : 0u; };
            fs::create_directories(dir / "plug");
            std::ofstream(dir / "plug" / "pw.lua")
                << "ui.window('PlugWeb', function()\n"
                   "  if PW_DONE then return end\n"
                   "  PW_DONE = true\n"
                   "  PW_TAB, PW_WHY = eq.WebOpen('https://example.org/')\n"
                   "  PW_PLAYER_TABS = #eq.WebTabs()\n"
                   "  PW_EXTERNAL = eq.WebOpenExternal('https://example.org/')\n"
                   "end)\n";
            s.lua.set_folders({{"plugin:Plug", dir / "plug"}});
            s.frames(2);
            auto run_plug = [&](const char* code) { // in the plugin's own globals
                bool ok = luaL_loadstring(L, code) == LUA_OK;
                if (ok) {
                    s.lua.push_folder_env_for_test("plugin:Plug");
                    lua_setupvalue(L, -2, 1);
                    ok = lua_pcall(L, 0, 0, 0) == LUA_OK;
                }
                if (!ok) std::fprintf(stderr, "lua: %s: %s\n", code, lua_tostring(L, -1));
                REQUIRE(ok);
            };
            run_plug("assert(PW_TAB == nil and PW_WHY:find('web'), tostring(PW_WHY))");
            run_plug("assert(PW_EXTERNAL == false)");
            CHECK(web.tabs("plugin:Plug").empty());
            plug_caps = plugin::kCapAll;
            run_plug("PW_DONE = nil");
            s.frames(2);
            run_plug("assert(PW_TAB ~= nil and PW_PLAYER_TABS == 1)");
            CHECK(web.tabs("plugin:Plug").size() == 1);
            CHECK(web.tabs("").empty()); // the pack's own window sees none of the plugin's tabs
            s.lua.set_folders({});
            browser.shutdown();
        }
        ImGui::DestroyContext(ctx);
    }
}
#endif
namespace {

// The most opaque vertex a window drew last frame (0..255), or -1 when the window is not there.
int max_alpha(const char* name) {
    ImGuiWindow* w = ImGui::FindWindowByName(name);
    if (!w || !w->WasActive) return -1;
    int out = 0;
    for (const ImDrawVert& v : w->DrawList->VtxBuffer) out = std::max(out, static_cast<int>((v.col >> IM_COL32_A_SHIFT) & 0xff));
    return out;
}

} // namespace

TEST_CASE("UI packs (modern): the Options window's Window Transparency and a window's Fades") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_fade_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        const char* player = "Player###modern.PlayerWindow";
        ImGui::GetIO().MousePos = ImVec2(-1000, -1000); // away from every window
        s.frames(3);
        REQUIRE(max_alpha(player) == 255);
        // Window Transparency: every window at most that opaque (eased over the fade's half second).
        REQUIRE(luaL_dostring(L, "eq.SetUiIni('Main', 'GlobalAlpha', '128')") == LUA_OK);
        s.frames(40);
        CHECK(max_alpha(player) <= 128);
        CHECK(max_alpha(player) >= 120);
        // Fades: after the delay (2 s) and the fade (0.5 s) with the mouse away, the Fade-to Transparency.
        REQUIRE(luaL_dostring(L, "eq.SetUiIni('Main', 'GlobalAlpha', '255') eq.SetUiIni('Main', 'GlobalFadeToAlpha', '64') "
                                 "ui.SetSetting('hud.fades.PlayerWindow', '1')") == LUA_OK);
        s.frames(60); // back to full first (the mouse counts as over at the start)
        CHECK(max_alpha(player) == 255); // within the delay
        s.frames(150);
        CHECK(max_alpha(player) <= 64);
        // The mouse back over it: full again after the fade.
        ImGuiWindow* win = ImGui::FindWindowByName(player);
        REQUIRE(win);
        ImGui::GetIO().MousePos = ImVec2(win->Pos.x + 5, win->Pos.y + 5);
        s.frames(40);
        CHECK(max_alpha(player) == 255);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): a resizable dialog's Scale zooms the whole window") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_dialog_scale_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        const char* guild = "Guild###modern.GuildManagementWnd";
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.GuildManagementWnd', '600,450') "
                                 "eq.ShowWindow('GuildManagementWnd', true)") == LUA_OK);
        s.frames(3);
        ImGuiWindow* win = ImGui::FindWindowByName(guild);
        REQUIRE(win);
        const ImVec2 before = win->Size;
        CHECK(before.x == doctest::Approx(600).epsilon(0.02));
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.scale.GuildManagementWnd', '1.5')") == LUA_OK);
        s.frames(3);
        CHECK(win->Size.x == doctest::Approx(before.x * 1.5f).epsilon(0.02));
        CHECK(win->Size.y == doctest::Approx(before.y * 1.5f).epsilon(0.02));
        // its size in design pixels is kept
        REQUIRE(luaL_dostring(L, "return ui.GetSetting('hud.size.GuildManagementWnd')") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) == "600,450");
        lua_settop(L, 0);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): an invitation to a group pops up with who sent it") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_group_invite_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        const char* popup = "Group Invite###modern.Group Invite";
        auto shown = [&] {
            ImGuiWindow* win = ImGui::FindWindowByName(popup);
            return win && win->WasActive;
        };
        s.frames(2);
        CHECK_FALSE(shown());
        net::GroupInvite inv;
        inv.invitee = "Tester";
        inv.inviter = "Bob";
        s.conn.receive_zone_packet_for_test(net::encode(inv));
        s.frames(2);
        CHECK(shown());
        CHECK(s.api.replaces("GroupInvite")); // the client's own prompt stays off: one popup, not two
        REQUIRE(luaL_dostring(L, "return (eq.WindowText('GroupWindow', 'Hailstone_Inviter'))") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) == "Bob");
        lua_settop(L, 0);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A server's compiled plugins ask before they load (docs/plugins.md): each pack draws the question in the client's
// place (eq.PluginConsent; "PluginConsent" replaced), and nothing while there is none.
// A window key reaches a pack's window only while the pack claims it (eq.ReplaceWindow, each frame): the achievement
// and reward keys must work while their windows are hidden.
TEST_CASE("UI packs: each claims the achievement and reward windows while they are hidden, so their keys open them") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_progress_keys_") + pack);
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            s.frames(2);
            CHECK_FALSE(s.api.shown("AchievementsWnd"));
            CHECK_FALSE(s.api.shown("RewardSelectionWnd"));
            CHECK(s.api.replaces("AchievementsWnd"));
            CHECK(s.api.replaces("RewardSelectionWnd"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the server plugins' question draws in each pack in the client's place") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_consent_") + pack);
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            std::optional<plugin::ConsentPrompt> prompt;
            s.host.plugin_consent = [&] { return prompt; };
            s.host.answer_plugin_consent = [&](std::optional<bool>) { prompt.reset(); };
            auto drawn = [&] {
                for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                    if (win->WasActive && (std::string(win->Name).find("###modern.PluginConsent") != std::string::npos ||
                                           std::string(win->Name) == "Server Plugins"))
                        return true;
                return false;
            };
            s.frames(2);
            CHECK(s.api.replaces("PluginConsent")); // the client's own prompt stays off
            CHECK_FALSE(drawn());
            plugin::ConsentPrompt p;
            p.server = "Alpha";
            p.serial = 1;
            p.files.push_back({"MQ2Thing", "MQ2Thing.so", 2u * 1024 * 1024, {}});
            p.files.push_back({"MQ2Other", "MQ2Other.so", 4096, {}});
            prompt = p;
            s.frames(3);
            CHECK(drawn());
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            prompt.reset();
            s.frames(2);
            CHECK_FALSE(drawn());
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// The gamepad pack has no popup of its own: the client's prompt stays, and its group window gives it the pad.
TEST_CASE("UI packs (gamepad): an invitation to a group keeps the client's own prompt") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch("ui_pack_group_invite_gamepad_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "gamepad");
        net::GroupInvite inv;
        inv.invitee = "Tester";
        inv.inviter = "Bob";
        s.conn.receive_zone_packet_for_test(net::encode(inv));
        s.frames(2);
        CHECK_FALSE(s.api.replaces("GroupInvite"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A resizable dialog's bottom-right corner resizes it (the AA window): its size in design pixels grows on both
// axes and its scale stays (the scale is the right-click menu's). Its right edge changes the width alone.
TEST_CASE("UI packs (modern): a dialog's corner and edge resize it, neither scales it") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_dialog_corner_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        auto setting = [&](const char* name) {
            REQUIRE(luaL_dostring(L, (std::string("return ui.GetSetting('") + name + "') or ''").c_str()) == LUA_OK);
            std::string v = lua_tostring(L, -1);
            lua_settop(L, 0);
            return v;
        };
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.AAWindow', '600,450') ui.SetSetting('test.pos.AAWindow', "
                                 "'40,40') eq.ShowWindow('AAWindow', true)") == LUA_OK);
        s.frames(3);
        ImGuiWindow* win = ImGui::FindWindowByName("Alternate Advancement###modern.AAWindow");
        REQUIRE(win);
        const ImVec2 before = win->Size;
        ImGuiIO& io = ImGui::GetIO();
        auto drag = [&](ImVec2 from, float dx, float dy) {
            io.AddMousePosEvent(from.x, from.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 10; ++i) {
                io.AddMousePosEvent(from.x + dx * static_cast<float>(i) / 10, from.y + dy * static_cast<float>(i) / 10);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(3);
        };
        // the corner: a larger design size on both axes, the scale untouched
        drag(ImVec2(win->Pos.x + win->Size.x - 4, win->Pos.y + win->Size.y - 4), 120, 90);
        CHECK(setting("hud.scale.AAWindow").empty());
        int sw = 0, sh = 0;
        REQUIRE(std::sscanf(setting("hud.size.AAWindow").c_str(), "%d,%d", &sw, &sh) == 2);
        CHECK(sw > 680);
        CHECK(sh > 510);
        CHECK(win->Size.x > before.x + 80);
        CHECK(win->Size.y > before.y + 60);
        // the right edge: a wider design size, the height and scale unchanged
        const ImVec2 grown = win->Size;
        drag(ImVec2(win->Pos.x + win->Size.x - 2, win->Pos.y + win->Size.y / 2), 60, 0);
        CHECK(setting("hud.scale.AAWindow").empty());
        CHECK(win->Size.x > grown.x + 30);
        CHECK(win->Size.y == doctest::Approx(grown.y).epsilon(0.01));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A resizable HUD frame's bottom-right corner resizes it (the chat): a larger design size, the scale untouched. A
// frame sized to what it shows (the player) has no corner grip: a drag there moves it and leaves its scale alone.
TEST_CASE("UI packs (modern): a frame's corner resizes it, never scales it") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_frame_corner_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        auto setting = [&](const char* name) {
            REQUIRE(luaL_dostring(L, (std::string("return ui.GetSetting('") + name + "') or ''").c_str()) == LUA_OK);
            std::string v = lua_tostring(L, -1);
            lua_settop(L, 0);
            return v;
        };
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.Chat1', '400,200') ui.SetSetting('test.pos.Chat1', '40,300') "
                                 "ui.SetSetting('test.pos.PlayerWindow', '600,40')") == LUA_OK);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(-1000, -1000);
        s.frames(3);
        ImGuiWindow* chat = ImGui::FindWindowByName("###modern.Chat1");
        ImGuiWindow* player = ImGui::FindWindowByName("Player###modern.PlayerWindow");
        REQUIRE(chat);
        REQUIRE(player);
        auto drag = [&](ImVec2 from, float dx, float dy) {
            io.AddMousePosEvent(from.x, from.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 10; ++i) {
                io.AddMousePosEvent(from.x + dx * static_cast<float>(i) / 10, from.y + dy * static_cast<float>(i) / 10);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(3);
        };
        const ImVec2 before = chat->Size;
        drag(ImVec2(chat->Pos.x + chat->Size.x - 4, chat->Pos.y + chat->Size.y - 4), 100, 60);
        CHECK(setting("hud.scale.Chat1").empty());
        int sw = 0, sh = 0;
        REQUIRE(std::sscanf(setting("hud.size.Chat1").c_str(), "%d,%d", &sw, &sh) == 2);
        CHECK(sw > 460);
        CHECK(sh > 230);
        CHECK(chat->Size.x > before.x + 60);
        CHECK(chat->Size.y > before.y + 30);
        const ImVec2 playerSize = player->Size;
        drag(ImVec2(player->Pos.x + player->Size.x - 4, player->Pos.y + player->Size.y - 4), 80, 40);
        CHECK(setting("hud.scale.PlayerWindow").empty());
        CHECK(player->Size.x == doctest::Approx(playerSize.x));
        CHECK(player->Size.y == doctest::Approx(playerSize.y));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A dialog's roll-up button sits beside its close box (ImGui's collapse button at the right of the title bar): a click
// rolls it up to its title bar and keeps that in hud.rolled.<window>, through closing and opening it again; a second
// click unrolls it. A question to answer (rollup = false) has none.
TEST_CASE("UI packs (modern): a dialog rolls up from the button beside its close box") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_dialog_rollup_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
            std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return v;
        };
        run("ui.SetSetting('test.pos.AAWindow', '40,40') eq.ShowWindow('AAWindow', true)");
        s.frames(3);
        ImGuiWindow* win = ImGui::FindWindowByName("Alternate Advancement###modern.AAWindow");
        REQUIRE(win);
        CHECK_FALSE(win->Collapsed);
        CHECK((win->Flags & ImGuiWindowFlags_NoCollapse) == 0);
        const float fullHeight = win->Size.y;
        ImGuiIO& io = ImGui::GetIO();
        auto clickRollup = [&] {
            // where ImGui puts it with WindowMenuButtonPosition = Right: left of the close box
            const ImGuiStyle& st = ImGui::GetStyle();
            const float fs = win->TitleBarHeight - st.FramePadding.y * 2;
            const ImVec2 at(win->Pos.x + win->Size.x - st.FramePadding.x - fs - st.ItemInnerSpacing.x - fs / 2,
                            win->Pos.y + st.FramePadding.y + fs / 2);
            io.AddMousePosEvent(at.x, at.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(2);
        };
        clickRollup();
        CHECK(win->Collapsed);
        CHECK(win->Size.y < fullHeight / 2);
        CHECK(run("return ui.GetSetting('hud.rolled.AAWindow')") == "1");
        CHECK(run("return tostring(eq.WindowShown('AAWindow'))") == "true"); // (rolled up, not closed)
        // closed and opened again: still rolled up
        run("eq.ShowWindow('AAWindow', false)");
        s.frames(2);
        run("eq.ShowWindow('AAWindow', true)");
        s.frames(2);
        CHECK(win->Active);
        CHECK(win->Collapsed);
        clickRollup();
        CHECK_FALSE(win->Collapsed);
        CHECK(run("return ui.GetSetting('hud.rolled.AAWindow') or ''") == "");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The gamepad pack's shared pieces that every window calls each frame, against the way they worked before they were
// made cheaper: T.fit (a binary search; never half a UTF-8 character), G.wrapped (a line measured whole only near the
// width; maxLines lines drawn, the bottom of all), CH.testOpen (off for good once test.show is empty in the world).
TEST_CASE("UI packs (gamepad): T.fit, G.wrapped and CH.testOpen give what they gave") {
    constexpr const char* kScript = R"lua(
local T = dofile("lib/theme.lua")
local G = dofile("lib/stock.lua")
local CH = dofile("lib/child.lua")
local function check(ok, what) if not ok then error(what, 2) end end
ui.Begin("gamepad lib test")
-- T.fit: the old byte-by-byte search, for ASCII
local function oldFit(text, w)
  if ui.CalcTextSize(text) <= w then return text end
  local n = #text
  while n > 1 and ui.CalcTextSize(text:sub(1, n) .. "...") > w do n = n - 1 end
  return text:sub(1, n) .. "..."
end
for _, text in ipairs({ "", "a", "ab", "Lord Nagafen's Lair", "A very long item name, of the ages (Legendary)" }) do
  for w = -5, 400, 3 do
    check(T.fit(text, w) == oldFit(text, w), "ascii " .. text .. " @" .. w)
  end
end
-- T.fit, UTF-8: the longest start that ends a character and fits, never fewer than the first character
local function isCont(b) return b and b >= 0x80 and b < 0xC0 end
for _, text in ipairs({ "\u{C9}l\u{E9}onore d\u{2019}Ambr\u{E9}e", "\u{1F600}\u{1F600}x\u{E9}\u{E9}\u{E9}", "\u{E9}" }) do
  local first = 1
  while isCont(text:byte(first + 1)) do first = first + 1 end
  for w = -5, 300, 2 do
    local got = T.fit(text, w)
    local want = text
    if ui.CalcTextSize(text) > w then
      local best = first
      for n = first, #text do
        if not isCont(text:byte(n + 1)) and ui.CalcTextSize(text:sub(1, n) .. "...") <= w then best = n end
      end
      want = text:sub(1, best) .. "..."
    end
    check(got == want, "utf8 @" .. w .. ": " .. got .. " / " .. want)
  end
end
-- G.wrapped: the old wrap (every line measured whole), the lines drawn and the bottom
local function oldWrapped(text, x, y, width, maxLines, out)
  local _, lh = ui.CalcTextSize("Ag")
  local n = 0
  for para in (text .. "\n"):gmatch("(.-)\n") do
    local line = ""
    for word in para:gmatch("%S+") do
      local try = line == "" and word or (line .. " " .. word)
      if ui.CalcTextSize(try) > width and line ~= "" then
        if not maxLines or n < maxLines then out[#out + 1] = y .. ":" .. line end
        y, n, line = y + lh, n + 1, word
      else
        line = try
      end
    end
    if not maxLines or n < maxLines then out[#out + 1] = y .. ":" .. line end
    y, n = y + lh, n + 1
  end
  return y
end
local words = {}
for i = 1, 120 do words[#words + 1] = ({ "a", "gnoll", "pup", "the", "Commonlands", "of", "W", "iiii", "MMMM" })[i % 9 + 1] end
local long = table.concat(words, " ") .. "\n\nA second paragraph  with  spaces.\nSupercalifragilisticexpialidocious!"
local drawn
local draw = T.draw
T.draw = function(x, y, text) drawn[#drawn + 1] = y .. ":" .. text end
for _, maxLines in ipairs({ false, 1, 3 }) do
  for width = 20, 600, 7 do
    drawn = {}
    local want = {}
    local yNew = G.wrapped(long, 10, 100, width, nil, maxLines or nil)
    local yOld = oldWrapped(long, 10, 100, width, maxLines or nil, want)
    check(yNew == yOld, "bottom @" .. width)
    check(table.concat(drawn, "|") == table.concat(want, "|"), "lines @" .. width)
    if maxLines then check(#drawn <= maxLines, "maxLines @" .. width) end
  end
end
T.draw = draw
ui.End()
-- CH.testOpen: test.show empty in the world turns it off for good (as M.testShow and G.testShow); a list is read
GPSTATE.testOpenOff = nil
ui.SetSetting("test.show", "NoSuchWnd,OtherWnd")
CH.testOpen("OtherWnd")
check(not GPSTATE.testOpenOff and GPSTATE.testOpenList.names.OtherWnd and GPSTATE.testOpenList.names.NoSuchWnd,
  "list read")
ui.SetSetting("test.show", "")
CH.testOpen("OtherWnd")
check(GPSTATE.testOpenOff == true, "off when empty")
ui.SetSetting("test.show", "OtherWnd")
GPSTATE.testOpenList = nil
CH.testOpen("OtherWnd")
check(GPSTATE.testOpenList == nil, "stays off")
return "ok"
)lua";
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch("ui_pack_gamepad_lib_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "gamepad");
        s.frames(1);
        ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
        s.api.begin_frame();
        ImGui::NewFrame();
        lua_State* L = s.lua.state();
        int rc = luaL_dostring(L, kScript);
        std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        INFO(out);
        CHECK(rc == LUA_OK);
        CHECK(out == "ok");
        lua_settop(L, 0);
        ImGui::Render();
    }
    ImGui::DestroyContext(ctx);
}

// The gamepad windows keep what is costly to read or lay out (a raid's members, the raid's and the guild's message of
// the day, the /who answer, an item's wrapped description) between frames: a change still shows. The stock windows'
// texts and lists are stood in for from Lua, and every text the theme draws is recorded.
TEST_CASE("UI packs (gamepad): cached window contents follow a change") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch("ui_pack_gamepad_caches_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "gamepad");
        s.frames(2);
        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            INFO(code);
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
        };
        auto saw = [&](const char* needle) {
            lua_getglobal(L, "saw");
            lua_pushstring(L, needle);
            REQUIRE(lua_pcall(L, 1, 1, 0) == LUA_OK);
            bool found = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return found;
        };
        run(R"lua(
            seen = {}
            -- (every theme text, T.text, T.draw and T.textIn alike, ends in ui.DrawText(x, y, r, g, b, a, text))
            local DrawText = ui.DrawText
            ui.DrawText = function(...)
              seen[#seen + 1] = select(7, ...)
              return DrawText(...)
            end
            function saw(needle)
              for _, s in ipairs(seen) do
                if type(s) == "string" and s:find(needle, 1, true) then return true end
              end
              return false
            end
            fake = {
              raidMotd = "Raid message one", members = { { "1", "Alpha", "50", "Warrior", "", "Leader" } },
              guildMotd = "Guild message one", desc = "The first description",
              who = 1, players = { { name = "Echo", line = "[50 Warrior] Echo (Human) <Guild> ZONE: qeynos" } },
            }
            local ListInfo, ListRow, WindowText, WhoResults = eq.ListInfo, eq.ListRow, eq.WindowText, eq.WhoResults
            eq.ListInfo = function(win, child, ...)
              if win == "RaidWindow" then
                if child == "RAID_PlayerList" then return #fake.members, 6, -1 end
                return 0, 0, -1
              end
              if win == "GuildManagementWnd" and child == "MemberList" then return 0, 0, -1 end
              return ListInfo(win, child, ...)
            end
            eq.ListRow = function(win, child, row, ...)
              if win == "RaidWindow" then
                return child == "RAID_PlayerList" and fake.members[row + 1] or nil
              end
              return ListRow(win, child, row, ...)
            end
            eq.WindowText = function(win, child, ...)
              if win == "RaidWindow" and child == "MOTDViewer" then return fake.raidMotd end
              if win == "GuildManagementWnd" and child == "MOTDViewer" then return fake.guildMotd end
              if win == "GuildManagementWnd" and child == "GuildName" then return "The Guild" end
              if win == "ItemDisplayWindow" and child == "ItemDescription" then return fake.desc end
              return WindowText(win, child, ...)
            end
            eq.WhoResults = function() return fake.who, fake.players end
            -- (the stock guild and item windows show only in a guild / for an item: shown here on asking)
            fake.shown = {}
            local ReplaceWindow, WindowShown = eq.ReplaceWindow, eq.WindowShown
            eq.ReplaceWindow = function(win, ...)
              local r = ReplaceWindow(win, ...)
              if fake.shown[win] ~= nil then return fake.shown[win] end
              return r
            end
            eq.WindowShown = function(win)
              local r = WindowShown(win)
              if fake.shown[win] ~= nil then return fake.shown[win] end
              return r
            end
        )lua");

        // the raid: its message of the day at once, a new member at once (the row count), a renamed one within 0.25 s
        run("eq.ShowWindow('RaidWindow', true) seen = {}");
        s.frames(2);
        CHECK(saw("MOTD: Raid message one"));
        CHECK(saw("Alpha"));
        run("fake.raidMotd = 'Raid message two' "
            "fake.members[2] = { '2', 'Bravo', '40', 'Cleric', '', '' } seen = {}");
        s.frames(1);
        CHECK(saw("MOTD: Raid message two"));
        CHECK_FALSE(saw("Raid message one"));
        CHECK(saw("Bravo"));
        run("fake.members[2] = { '2', 'Charlie', '40', 'Cleric', '', '' }");
        s.frames(20);
        run("seen = {}");
        s.frames(1);
        CHECK(saw("Charlie"));
        CHECK_FALSE(saw("Bravo"));
        run("eq.ShowWindow('RaidWindow', false)");

        // the guild's message of the day
        run("fake.shown.GuildManagementWnd = true seen = {}");
        s.frames(2);
        CHECK(saw("Guild message one"));
        run("fake.guildMotd = '<b>Guild</b>   message two' seen = {}");
        s.frames(1);
        CHECK(saw("Guild message two"));
        CHECK_FALSE(saw("Guild message one"));
        run("fake.shown.GuildManagementWnd = false");

        // the /who answer: each row's line cut down to what follows the name; another reply within 0.25 s
        run("GPSTATE.whoOpen = true seen = {}");
        s.frames(2);
        CHECK(saw("Echo"));
        CHECK(saw("(Human) <Guild> ZONE: qeynos"));
        run("fake.who = 2 fake.players = { { name = 'Delta', line = '[60 Cleric] Delta (Erudite)' } }");
        s.frames(20);
        run("seen = {}");
        s.frames(1);
        CHECK(saw("Delta"));
        CHECK(saw("(Erudite)"));
        CHECK_FALSE(saw("Echo"));
        run("GPSTATE.whoOpen = false");

        // an item's description, wrapped once while it stays the same
        run("fake.shown.ItemDisplayWindow = true seen = {}");
        s.frames(2);
        CHECK(saw("The first description"));
        run("fake.desc = 'Another description' seen = {}");
        s.frames(1);
        CHECK(saw("Another description"));
        CHECK_FALSE(saw("The first description"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The modern item display reads the window's info lines and stat grid again only when the item changes (or every
// quarter second): another item shows at once, and what it draws is what the window holds.
TEST_CASE("UI packs (modern): an item display draws the shown item's stats, and another item at once") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_item_display_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        auto item = [](std::uint32_t id, const char* name, int str) {
            net::Item it;
            it.id = id;
            it.name = name;
            it.classes = 0xffff;
            it.races = 0xffff;
            it.equip_slots = 1u << 13;
            it.damage = 4;
            it.delay = 29;
            it.astr = static_cast<std::int8_t>(str);
            it.ac = str * 2;
            return it;
        };
        std::vector<net::Item> displays{item(1001, "Test Sword", 3)};
        std::vector<std::uint64_t> ids{1};
        s.host.item_displays = &displays;
        s.host.item_display_ids = &ids;
        lua_State* L = s.lua.state();
        // Every string drawn (DrawText, TextColored); then: is each stat value the window holds among them?
        REQUIRE(luaL_dostring(L, R"(
            test_seen = {}
            local dt, tc = ui.DrawText, ui.TextColored
            ui.DrawText = function(...) local a = { ... } test_seen[tostring(a[#a])] = true return dt(...) end
            ui.TextColored = function(...) local a = { ... } test_seen[tostring(a[5])] = true return tc(...) end
            function test_check()
              local n, missing = 0, {}
              for c = 1, 3 do
                for r = 1, 26 do
                  local st = eq.WindowText("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Stat") or ""
                  local v = (eq.WindowText("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Value") or "")
                    :match("^%s*(.-)%s*$")
                  if eq.ChildShown("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Stat") ~= false
                      and st:match("%S") and v ~= "" then
                    n = n + 1
                    if not test_seen[v] then missing[#missing + 1] = v end
                  end
                end
              end
              return n, table.concat(missing, ",")
            end)") == LUA_OK);
        auto check = [&](const char* name) {
            REQUIRE(luaL_dostring(L, "test_seen = {}") == LUA_OK);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "return test_check()") == LUA_OK);
            CHECK(lua_tointeger(L, -2) > 0);
            CHECK(std::string(lua_tostring(L, -1)) == "");
            lua_settop(L, 0);
            REQUIRE(luaL_dostring(L, (std::string("return test_seen['") + name + "'] == true").c_str()) == LUA_OK);
            CHECK(lua_toboolean(L, -1));
            lua_settop(L, 0);
        };
        s.frames(2);
        check("Test Sword");
        // another item in the same window: drawn the next frame
        displays[0] = item(1002, "Other Blade", 7);
        check("Other Blade");
        // one of the same name with other stats: drawn within the quarter second the parse is kept
        displays[0] = item(1003, "Other Blade", 11);
        s.frames(20);
        check("Other Blade");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// Hailstone's item comparison (docs/item-compare.md): a linked item's display says what wearing it would change,
// in both packs, and the Options switch turns it off.
TEST_CASE("UI packs: the item display compares the item with what you wear") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_item_compare_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            namespace mech = game::mechanics;
            mech::GameTables tables;
            tables.class_levels.parse("60^1^400^0^300^3^6^0.9^0^1.25\n");
            tables.skill_caps.parse("1^15^60^210^0\n");
            mech::CharacterSheet sheet;
            sheet.level = sheet.highest_level = 60;
            sheet.base_str = sheet.base_sta = sheet.base_cha = sheet.base_dex = sheet.base_int = sheet.base_agi =
                sheet.base_wis = 75;
            sheet.buffs.resize(mech::kPlayerBuffSlots);
            net::Item old_helm;
            old_helm.name = "Old Helm";
            old_helm.classes = old_helm.races = 0xffff;
            old_helm.equip_slots = 1u << 2;
            old_helm.hp = 10;
            sheet.possessions[2] = old_helm;
            mech::Character ch(sheet, tables, [](std::int32_t) -> const game::SpellData* { return nullptr; });
            ch.process_all_stats();
            s.host.character = &ch;
            int option = 1;
            s.host.find_option = [&](std::string_view o) -> std::optional<int> { return o == "item_compare" ? option : 1; };

            net::Item helm = old_helm;
            helm.id = 1001;
            helm.name = "Iron Helm";
            helm.hp = 60;
            std::vector<net::Item> displays{helm};
            std::vector<std::uint64_t> ids{1};
            s.host.item_displays = &displays;
            s.host.item_display_ids = &ids;
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"(
                test_seen = {}
                local dt, tc = ui.DrawText, ui.TextColored
                ui.DrawText = function(...) local a = { ... } test_seen[tostring(a[#a])] = true return dt(...) end
                ui.TextColored = function(...) local a = { ... } test_seen[tostring(a[5])] = true return tc(...) end)") ==
                    LUA_OK);
            auto seen = [&](const std::string& text) {
                REQUIRE(luaL_dostring(L, ("return test_seen['" + text + "'] == true").c_str()) == LUA_OK);
                bool b = lua_toboolean(L, -1);
                lua_settop(L, 0);
                return b;
            };
            s.frames(3);
            const std::string hp = "+50 HP (" + std::to_string(ch.max_hp(true)) + " to " +
                                   std::to_string(ch.max_hp(true) + 50) + ")";
            CHECK(seen("Compared with your Old Helm (Head)"));
            CHECK(seen("Upgrade"));
            CHECK(seen(hp));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

            option = 0;
            REQUIRE(luaL_dostring(L, "test_seen = {}") == LUA_OK);
            s.frames(2);
            CHECK_FALSE(seen("Compared with your Old Helm (Head)"));
        }
        ImGui::DestroyContext(ctx);
    }
}

// The modern windows whose per-frame reading was cut down (the reads kept to what changed, the tables made once)
// draw without script errors when shown.
TEST_CASE("UI packs (modern): the inventory, loot, trade, raid, guild, merchant, map and their kin draw") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_data_windows_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    // a small atlas for the map's World view
    fs::create_directories(dir / "game" / "Atlas" / "Default");
    std::ofstream(dir / "game" / "Atlas" / "Default" / "Default.xml")
        << "<Label Text = \"Antonica\" X = 5 Y = 6/><Link ClickAction = 1 FileToOpen = \"Q.xml\" ToolTip = \"Q\">"
           "<Rect X = 0 Y = 0 CX = 50 CY = 50/></Link><Link ClickAction = 2 FileToOpen = \"qeynos2\" ToolTip = \"North Qeynos\">"
           "<Rect X = 60 Y = 0 CX = 50 CY = 50/></Link>";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        s.frames(2);
        for (const char* screen : {"InventoryWindow", "LootWnd", "TradeWnd", "RaidWindow", "GuildManagementWnd",
                                   "MerchantWnd", "MapViewWnd", "AuraWindow", "PotionBeltWnd", "Bag1",
                                   "MarketplaceWnd", "PointMerchantWnd", "MMTW_MerchantWnd", "MMGW_ManageWnd",
                                   "MIW_InfoWnd", "SpellDisplayWindow"}) {
            INFO(screen);
            REQUIRE(luaL_dostring(L, (std::string("eq.ShowWindow('") + screen + "', true)").c_str()) == LUA_OK);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            REQUIRE(luaL_dostring(L, (std::string("eq.ShowWindow('") + screen + "', false)").c_str()) == LUA_OK);
        }
        // the map's World view (the atlas and the zone finder, lib/atlas.lua) over the small atlas above, then a search
        // and a route followed (the banner over the Zone view)
        // (a window's stored title changes only in some cases, so the World view is told by the zone finder's list)
        auto finder_drawn = [] {
            for (ImGuiWindow* win : GImGui->Windows)
                if (std::string_view(win->Name).find("##zonelist") != std::string_view::npos && win->Active) return true;
            return false;
        };
        REQUIRE(luaL_dostring(L, "ui.SetSetting('map.view', 'world') eq.ShowWindow('MapViewWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(finder_drawn());
        REQUIRE(luaL_dostring(L, "eq.ShowWindow('MapViewWnd', false)") == LUA_OK);
        s.frames(1);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('atlas.route', 'qeytoqrg') ui.SetSetting('map.view', 'zone')"
                                 " eq.ShowWindow('MapViewWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK_FALSE(finder_drawn()); // the Zone view
        REQUIRE(luaL_dostring(L, "eq.ShowWindow('MapViewWnd', false) ui.SetSetting('atlas.route', '') ui.SetSetting('map.view', '')") == LUA_OK);
        s.frames(1);
        // the dialogs a test setting draws though the game has not opened them (their opts.shown, set each frame)
        REQUIRE(luaL_dostring(L, "ui.SetSetting('test.draw.MarketplaceWnd', '1') ui.SetSetting('test.draw.ZonePathWnd', '1')"
                                 " ui.SetSetting('test.draw.PointMerchantWnd', '1')") == LUA_OK);
        s.frames(2);
        CHECK(ImGui::FindWindowByName("Zone Path###modern.ZonePathWnd") != nullptr);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('test.draw.MarketplaceWnd', '') ui.SetSetting('test.draw.ZonePathWnd', '')"
                                 " ui.SetSetting('test.draw.PointMerchantWnd', '')") == LUA_OK);
        s.frames(2);
        ImGuiWindow* zp = ImGui::FindWindowByName("Zone Path###modern.ZonePathWnd");
        CHECK((zp == nullptr || !zp->WasActive));
    }
    ImGui::DestroyContext(ctx);
}

// The raid window's members move between groups for the raid leader: a member dragged onto another group's card
// in an unlocked raid locks it first (the game moves members only in a locked raid, CRaidWnd::UpdateButtons) and is
// moved once the lock comes back; in a locked raid at once; a full group is refused. Each is the stock window's row
// click and group button, so what goes out is CRaid_SendMoveGroup's OP_RaidInvite {6, leader, member, group}.
TEST_CASE("UI packs (modern): the raid leader drags a member onto another group") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_raid_move_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        hailstone::test::Rig rig;
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        s.host.conn = &conn;
        const std::string me = conn.character_name();
        auto raid = [&](std::uint32_t action, const std::string& player, std::uint32_t group = 0) {
            net::RaidUpdate u;
            u.general.action = action;
            u.general.player_name = player;
            u.general.leader_name = me;
            u.general.parameter = group;
            u.class_ = 1;
            u.level = 50;
            conn.receive_zone_packet_for_test(net::encode_raid_update(u));
        };
        raid(net::kRaidCreate, me);
        raid(net::kRaidAddMember, me, 0);
        raid(net::kRaidAddMember, "Alpha", 0);
        raid(net::kRaidAddMember, "Bravo", 1);
        for (int i = 1; i <= 6; ++i) raid(net::kRaidAddMember, "Full" + std::to_string(i), 3); // group 4 is full
        REQUIRE(conn.raid().members.size() == 9);
        REQUIRE(conn.raid().leader == me);
        // OP_RaidInvite's raid commands so far: {action, the member (leader_name), parameter}
        auto commands = [&] {
            std::vector<std::tuple<std::uint32_t, std::string, std::uint32_t>> v;
            for (const auto& p : rig.server.zone_log) {
                if (p.opcode != net::to_wire(net::Op::RaidInvite)) continue;
                ByteReader r(p.data);
                std::uint32_t action = r.le<std::uint32_t>();
                r.fixed_string(64);
                r.skip(4);
                std::string member = r.fixed_string(64);
                v.emplace_back(action, member, r.le<std::uint32_t>());
            }
            return v;
        };
        auto count = [&](std::uint32_t action, const std::string& member, std::uint32_t parameter) {
            int n = 0;
            for (const auto& [a, m, p] : commands()) n += a == action && m == member && p == parameter;
            return n;
        };

        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            INFO(code);
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
        };
        // every text drawn, with where (T.Text ends in ui.DrawText(x, y, r, g, b, a, text))
        run(R"lua(
            drawn = {}
            local DrawText = ui.DrawText
            ui.DrawText = function(...)
              local x, y, text = select(1, ...), select(2, ...), select(7, ...)
              if type(text) == "string" then drawn[text] = { x, y } end
              return DrawText(...)
            end
            ui.SetSetting('hud.size.RaidWindow', '1200,640')
            ui.SetSetting('test.pos.RaidWindow', '20,20')
            eq.ShowWindow('RaidWindow', true)
        )lua");
        s.frames(4);
        REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        auto at = [&](const char* text) -> ImVec2 {
            lua_getglobal(L, "drawn");
            lua_getfield(L, -1, text);
            ImVec2 p(-1, -1);
            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                p = ImVec2(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
                lua_pop(L, 2);
            }
            lua_settop(L, 0);
            return p;
        };
        ImGuiIO& io = ImGui::GetIO();
        auto drag = [&](ImVec2 from, ImVec2 to) {
            io.AddMousePosEvent(from.x, from.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 10; ++i) {
                io.AddMousePosEvent(from.x + (to.x - from.x) * static_cast<float>(i) / 10,
                                    from.y + (to.y - from.y) * static_cast<float>(i) / 10);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(2);
        };
        auto name = [&](const char* text) {
            ImVec2 p = at(text);
            REQUIRE(p.x >= 0);
            return ImVec2(p.x + 4, p.y + 4);
        };
        // (a point inside group g's card: under its header)
        auto card = [&](const char* header) {
            ImVec2 p = at(header);
            REQUIRE(p.x >= 0);
            return ImVec2(p.x + 20, p.y + 40);
        };

        // /raidwin (/raidwindow's prefix) asks the app for the raid window, which toggles it as the raid key does
        conn.send_command("/raidwin");
        CHECK(conn.take_window_toggle() == std::optional<std::string>("raid"));

        // unlocked: the drop locks the raid; nothing moves until the lock comes back
        CHECK(at("The game moves players only while the raid is locked. Dragging a player locks it for you.").x >= 0);
        drag(name("Alpha"), card("Group 3"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(rig.until([&] { return count(net::kRaidCmdLock, me, 0) == 1; }));
        CHECK(count(net::kRaidMoveGroup, "Alpha", 2) == 0);
        raid(net::kRaidLock, me);
        s.frames(2);
        REQUIRE(rig.until([&] { return count(net::kRaidMoveGroup, "Alpha", 2) == 1; }));
        CHECK(conn.raid().target == "Alpha");
        // EQEmu's answer (Raid::SendRaidMoveAll): the member removed and added again in its new group
        raid(net::kRaidRemoveMember, "Alpha");
        raid(net::kRaidAddMember, "Alpha", 2);
        s.frames(3);

        // locked: a drop moves at once, out of the groups too (parameter -1)
        drag(name("Bravo"), card("Group 5"));
        REQUIRE(rig.until([&] { return count(net::kRaidMoveGroup, "Bravo", 4) == 1; }));
        drag(name("Alpha"), card("Not in a group"));
        REQUIRE(rig.until([&] { return count(net::kRaidMoveGroup, "Alpha", 0xffffffffu) == 1; }));
        // a full group takes no one
        drag(name("Bravo"), card("Group 4"));
        rig.run_for(std::chrono::milliseconds(100));
        CHECK(count(net::kRaidMoveGroup, "Bravo", 3) == 0);
        CHECK(count(net::kRaidCmdLock, me, 0) == 1); // (locked once only)
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): the Options window's Textures section picks the texture pack and starts the studio") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_textures_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        std::set<std::string> asked;
        s.host.find_option = [&](std::string_view o) -> std::optional<int> {
            asked.insert(std::string(o));
            if (o == "texture_pack") return 2;
            if (o == "texture_studio") return 1;
            return std::nullopt;
        };
        s.host.option_choices = [](std::string_view o) {
            return o == "texture_pack" ? std::vector<std::string>{"Off", "default", "hd"} : std::vector<std::string>{};
        };
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "eq.ShowWindow('OptionsWindow', true)") == LUA_OK);
        s.frames(3);
        // a click on the Graphics tab (found in the window's tab bar)
        ImGuiContext& g = *ImGui::GetCurrentContext();
        std::optional<ImVec2> at;
        for (int i = 0; i < g.TabBars.GetMapSize() && !at; ++i)
            if (ImGuiTabBar* bar = g.TabBars.TryGetMapData(i))
                for (ImGuiTabItem& tab : bar->Tabs)
                    if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)) == "Graphics")
                        at = ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width / 2, (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2);
        REQUIRE(at);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at->x, at->y);
        s.frames(1);
        io.AddMouseButtonEvent(0, true);
        s.frames(1);
        io.AddMouseButtonEvent(0, false);
        s.frames(3);
        CHECK(asked.contains("texture_pack"));
        CHECK(asked.contains("texture_studio")); // the Texture Studio button is offered
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: Options changes the Find trail style in both directions") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_pack_find_trail_options");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            Session s(dir, pack);
            int selected = 0;
            std::vector<int> changes;
            s.host.find_option = [&](std::string_view name) -> std::optional<int> {
                return name == "find_trail_style" ? std::optional(selected) : std::nullopt;
            };
            s.host.set_option = [&](std::string_view name, int value) {
                if (name == "find_trail_style") {
                    changes.push_back(value);
                    selected = value;
                }
            };
            lua_State* L = s.lua.state();
            // Record real control coordinates without changing their activation or rendering.
            REQUIRE(luaL_dostring(L, R"lua(
                findTrailWidgets = {}
                local combo, selectable, text = ui.BeginCombo, ui.Selectable, ui.DrawText
                ui.BeginCombo = function(label, ...)
                    local open = combo(label, ...)
                    if label == "##find_trail_style" then
                        local x0, y0, x1, y1 = ui.GetItemRect()
                        findTrailWidgets.combo = { (x0 + x1) / 2, (y0 + y1) / 2 }
                    end
                    return open
                end
                ui.Selectable = function(label, ...)
                    local clicked = selectable(label, ...)
                    local name = label:match("^Rainbow trail##") and "rainbow"
                        or label:match("^Gold line %(default%)##") and "navigation"
                    if name then
                        local x0, y0, x1, y1 = ui.GetItemRect()
                        findTrailWidgets[name] = { (x0 + x1) / 2, (y0 + y1) / 2 }
                    end
                    return clicked
                end
                ui.DrawText = function(...)
                    local label = select(7, ...)
                    if label == "Interface" or label == "Find trail" then
                        local x, y = ...
                        local w, h = ui.CalcTextSize(label)
                        findTrailWidgets[label] = { x + w / 2, y + h / 2 }
                    end
                    return text(...)
                end
                eq.ShowWindow("OptionsWindow", true)
            )lua") == LUA_OK);
            s.frames(3);
            auto point = [&](const char* name) {
                lua_getglobal(L, "findTrailWidgets");
                lua_getfield(L, -1, name);
                REQUIRE_MESSAGE(lua_istable(L, -1), name);
                lua_rawgeti(L, -1, 1);
                const float x = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2);
                const float y = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 3);
                return ImVec2(x, y);
            };
            auto click = [&](const ImVec2& at) {
                ImGuiIO& io = ImGui::GetIO();
                io.AddMousePosEvent(at.x, at.y);
                s.frames(1);
                io.AddMouseButtonEvent(0, true);
                s.frames(1);
                io.AddMouseButtonEvent(0, false);
                s.frames(3);
            };
            if (std::string_view(pack) == "modern") {
                std::optional<ImVec2> tab_at;
                ImGuiContext& g = *ImGui::GetCurrentContext();
                for (int i = 0; i < g.TabBars.GetMapSize() && !tab_at; ++i)
                    if (ImGuiTabBar* bar = g.TabBars.TryGetMapData(i))
                        for (ImGuiTabItem& tab : bar->Tabs)
                            if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)) == "Display")
                                tab_at = ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width / 2,
                                                (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2);
                REQUIRE(tab_at);
                click(*tab_at);
                CHECK(changes.empty());
                click(point("combo"));
                click(point("rainbow"));
                REQUIRE(selected == 1);
                click(point("combo"));
                click(point("navigation"));
            } else {
                click(point("Interface"));
                CHECK(changes.empty());
                click(point("Find trail"));
                REQUIRE(selected == 1);
                click(point("Find trail"));
            }
            CHECK(selected == 0);
            REQUIRE(changes.size() == 2);
            CHECK(changes[0] == 1);
            CHECK(changes[1] == 0);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// A bag window resized: as many slots to a row as its width holds, its height grown to fit the rows; unsized, rows
// of five. A click lands on the slot drawn under the mouse (eq.BagSlotClick's index).
TEST_CASE("UI packs (modern): a resized bag window lays its slots out to its width") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_bag_reflow_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, R"(
            bagClicked = nil
            eq.BagInfo = function(win) if win == 'Bag1' then return 'Test Bag', 10, false, 'inventory', 23 end end
            eq.BagSlotItem = function() return nil end
            local WindowShown = eq.WindowShown -- (the game opens a bag window only on a bag it has)
            eq.WindowShown = function(win) if win == 'Bag1' then return true end return WindowShown(win) end
            eq.BagSlotClick = function(win, i, button) if button == 'left' then bagClicked = i end end
            ui.SetSetting('test.pos.Bag1', '100,100')
        )") == LUA_OK);
        s.frames(3);
        REQUIRE(luaL_dostring(L, "return ui.GetIO().uiScale or 1") == LUA_OK);
        const float scale = static_cast<float>(lua_tonumber(L, -1));
        lua_settop(L, 0);
        auto window = [] {
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows) {
                if (std::string_view(win->Name).ends_with("###modern.Bag1") && win->WasActive) return win;
            }
            return static_cast<ImGuiWindow*>(nullptr);
        };
        ImGuiIO& io = ImGui::GetIO();
        // the slot clicked at (col, row) of the grid: 44 design pixels and a gap of 5, below the 30 of the toolbar
        auto click = [&](int col, int row) {
            ImGuiWindow* win = window();
            REQUIRE(win);
            const ImVec2 o = win->DC.CursorStartPos;
            const float x = o.x + (static_cast<float>(col) * 49 + 22) * scale;
            const float y = o.y + (30 + static_cast<float>(row) * 49 + 22) * scale;
            REQUIRE(luaL_dostring(L, "bagClicked = nil") == LUA_OK);
            io.AddMousePosEvent(x, y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(2);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "return bagClicked or -1") == LUA_OK);
            const int i = static_cast<int>(lua_tointeger(L, -1));
            lua_settop(L, 0);
            return i;
        };
        auto bottomOfRows = [&](int rows) {
            ImGuiWindow* win = window();
            REQUIRE(win);
            return win->DC.CursorStartPos.y + (30 + static_cast<float>(rows) * 49 - 5) * scale;
        };
        // unsized: two rows of five
        CHECK(click(4, 0) == 4);
        CHECK(click(0, 1) == 5);
        // wide: one row of ten, the window no taller than that needs and still all of it
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.Bag1', '560,60')") == LUA_OK);
        s.frames(3);
        CHECK(click(7, 0) == 7);
        CHECK(click(0, 1) == -1);
        ImGuiWindow* win = window();
        REQUIRE(win);
        CHECK(win->Pos.y + win->Size.y >= bottomOfRows(1));
        CHECK(win->Pos.y + win->Size.y < bottomOfRows(2));
        // narrow (below its least width): four to a row, three rows, the window grown to hold them
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.Bag1', '100,60')") == LUA_OK);
        s.frames(3);
        CHECK(click(3, 0) == 3);
        CHECK(click(1, 2) == 9);
        win = window();
        REQUIRE(win);
        CHECK(win->Pos.y + win->Size.y >= bottomOfRows(3));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}
