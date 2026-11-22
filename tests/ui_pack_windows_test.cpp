// The shipped UIs (ui/modern, ui/gamepad) run for real over Dear ImGui with no renderer: every script loads, and the
// windows each draws for itself (the ones that once came from the Classic UI among them) draw with the game asking
// for them, with no script error.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
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

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "app/gamepad.h"
#include "core/byte_io.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "net/aa_packets.h"
#include "net/game_connection.h"
#include "net/evolving_packets.h"
#include "net/group_packets.h"
#include "net/merchant_packets.h"
#include "net/packet_io.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"
#include "net/task_packets.h"
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
#include "plugin/ui_bridge.h"
#include "web/browser_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Session {
    GameDir game;
    net::GameConnection conn;
    ui::ChatView view;
    app::UiHost host;
    app::UiApi api;
    ui::lua::LuaUi lua;
    ImVec2 display_size{1600, 900};
    Session(const fs::path& dir, const std::string& pack, net::ConnectionOptions options = {}, bool in_game = true)
        : game(dir / "game"), conn(game, std::move(options)), api(host), lua(dir / pack, dir / "ui.txt") {
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.in_game = host.ui_ready = in_game;
        lua.add_ui_api([this, in_game](lua_State* L) {
            api.register_functions(L);
            if (in_game) {
                lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
                lua_setfield(L, -2, "InGame");
            }
            hailstone::test::sync_ui_api_aliases(L);
        });
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
    }
    void frames(int n, ui::InGameView* fallback = nullptr, const ui::LoadingView* loading = nullptr) {
        for (int i = 0; i < n; ++i) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = display_size;
            io.DeltaTime = 1.0f / 60.0f;
            hailstone::test::sync_ui_api_aliases(lua.state());
            api.begin_frame();
            ImGui::NewFrame();
            if (fallback) (void)ui::in_game_overlay(*fallback);
            // As the app draws the loading screen (ClientApp::run): over everything, or, while the plugins' question
            // waits (Automation::frame's guard), a window after the others with the question raised above it.
            const bool question = loading && host.plugin_consent && host.plugin_consent().has_value();
            ui::lua::set_consent_guard(question);
            if (loading && !question) ui::loading_overlay(*loading);
            lua.frame();
            if (question) {
                ui::loading_overlay(*loading, false);
                ui::lua::raise_guarded_windows();
            }
            ImGui::Render();
            ui::lua::set_consent_guard(false);
        }
    }
};

struct ReplacementCheck {
    explicit ReplacementCheck(const app::UiApi& api) {
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
    "RealEstateItemsWnd", "RealEstateNeighborhoodWnd", "RealEstatePlotSearchWnd", "ScriptsWnd", "ItemExpTransferWnd"};

} // namespace

TEST_CASE("UI packs (gamepad): real window chrome fits resized viewports and explicit positions") {
    struct ScaleGuard {
        float previous = ui::ui_scale();
        ~ScaleGuard() { ui::set_ui_scale(previous); }
    } scale_guard;
    for (float scale : {1.0f, 2.0f, 0.5f}) {
        CAPTURE(scale);
        test::ScratchDir scratch("ui_gamepad_window_bounds");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::create_directories(dir / "gamepad");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui/gamepad/lib", dir / "gamepad/lib",
                 fs::copy_options::recursive);
        std::ofstream(dir / "gamepad/bounds.lua") << R"lua(
            local W = dofile('lib/window.lua')
            desiredWidth = 1720
            local invisible, line = ui.InvisibleButton, ui.DrawLine
            ui.InvisibleButton = function(id, ...)
                local clicked = invisible(id, ...)
                if id == '##close' then closeRect = {ui.GetItemRect()} end
                return clicked
            end
            ui.DrawLine = function(x0,y0,x1,y1,...)
                if y0 == y1 then dividers[#dividers+1] = {x0,y0,x1,y1} end
                return line(x0,y0,x1,y1,...)
            end
            ui.window('Geometry', function()
                dividers = {}
                if W.begin('Geometry', {w=desiredWidth,h=960,x=requestX,y=requestY,title='Title',subtitle='Subtitle',
                        hints={{'a','Select'},{'b','Close'}},noFocus=true}) then
                    contentRect = {W.area()}
                    ui.InvisibleButton('Content', 40, 12)
                    bodyRect = {ui.GetItemRect()}
                end
                closed = W.finish()
            end)
        )lua";
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        ImFontConfig font; font.SizePixels = 20.0f * scale;
        io.Fonts->AddFontDefault(&font);
        unsigned char* pixels = nullptr; int texture_width = 0, texture_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &texture_width, &texture_height);
        ui::set_ui_scale(scale);
        Session s(dir, "gamepad");
        auto frame = [&](float width, float height) {
            io.DisplaySize = ImVec2(width, height); io.DeltaTime = 1.0f / 60.0f;
            s.api.begin_frame(); ImGui::NewFrame(); s.lua.frame(); ImGui::Render();
            CHECK(s.lua.load_status().error_count == 0);
        };
        auto table = [&](const char* name) {
            std::vector<double> values;
            lua_State* L = s.lua.state(); lua_getglobal(L, name); REQUIRE(lua_istable(L, -1));
            for (std::size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i)); values.push_back(lua_tonumber(L, -1)); lua_pop(L, 1);
            }
            lua_pop(L, 1); return values;
        };
        auto check_bounds = [&](float width, float height) {
            const auto* w = ImGui::FindWindowByName("Geometry"); REQUIRE(w);
            CHECK(w->Pos.x >= 0); CHECK(w->Pos.y >= 0);
            CHECK(w->Pos.x + w->Size.x <= width + 0.01f); CHECK(w->Pos.y + w->Size.y <= height + 0.01f);
            const auto area = table("contentRect"), close = table("closeRect"), body = table("bodyRect");
            REQUIRE(area.size() == 6); REQUIRE(close.size() == 4); REQUIRE(body.size() == 4);
            CHECK(area[0] >= w->Pos.x); CHECK(area[1] > w->Pos.y);
            CHECK(area[2] <= w->Pos.x + w->Size.x); CHECK(area[3] < w->Pos.y + w->Size.y);
            CHECK(area[4] > 0); CHECK(area[5] > 0);
            CHECK(close[0] >= w->Pos.x); CHECK(close[1] >= w->Pos.y);
            CHECK(close[2] <= width); CHECK(close[3] <= height);
            CHECK(body[0] >= area[0]); CHECK(body[1] >= area[1]);
            CHECK(body[2] <= area[2]); CHECK(body[3] <= area[3]);
            bool header = false, footer = false;
            lua_State* L = s.lua.state(); lua_getglobal(L, "dividers");
            for (std::size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i)); lua_rawgeti(L, -1, 2);
                const double y = lua_tonumber(L, -1); lua_pop(L, 2);
                header = header || (y > w->Pos.y && y < area[1]);
                footer = footer || (y > area[3] && y < w->Pos.y + w->Size.y);
            }
            lua_pop(L, 1); CHECK(header); CHECK(footer);
        };
        for (int desired : {1720, 1500}) {
            REQUIRE(s.lua.run_for_test("", "desiredWidth=" + std::to_string(desired) + " return true") == "true");
            for (const auto [width, height] : {std::pair{1920.0f,1080.0f}, {720.0f,700.0f}, {1280.0f,360.0f}, {1920.0f,1080.0f}}) {
                CAPTURE(desired); CAPTURE(width); CAPTURE(height);
                frame(width, height); frame(width, height); check_bounds(width, height);
                const auto* w = ImGui::FindWindowByName("Geometry"); REQUIRE(w);
                if (width == 1920) {
                    CHECK(w->Size.x == desired); CHECK(w->Size.y == 960);
                    CHECK(w->Pos.x == doctest::Approx((1920.0f - static_cast<float>(desired)) / 2)); CHECK(w->Pos.y == 60);
                } else if (width == 720) {
                    CHECK(w->Size.x == 708); CHECK(w->Size.y == 480); CHECK(w->Pos.x == 6); CHECK(w->Pos.y == 110);
                }
            }
        }
        for (const auto [x,y] : {std::pair{-10000, -10000}, {0,0}, {10000,10000}}) {
            CAPTURE(x); CAPTURE(y);
            REQUIRE(s.lua.run_for_test("", "requestX="+std::to_string(x)+" requestY="+std::to_string(y)+" return true") == "true");
            frame(720,700); frame(720,700); check_bounds(720,700);
            const auto* w = ImGui::FindWindowByName("Geometry"); REQUIRE(w);
            CHECK(w->Pos.x == (x > 0 ? 12 : 0)); CHECK(w->Pos.y == (y > 0 ? 220 : 0));
        }
        const auto close = table("closeRect");
        io.AddMousePosEvent(static_cast<float>((close[0]+close[2])/2), static_cast<float>((close[1]+close[3])/2));
        io.AddMouseButtonEvent(0,true); frame(720,700);
        io.AddMouseButtonEvent(0,false); frame(720,700);
        CHECK(s.lua.run_for_test("", "return closed") == "true");
    }
}

TEST_CASE("UI packs (gamepad): stock Raid and Map stay inside narrow and short native viewports") {
    test::ScratchDir scratch("ui_gamepad_stock_bounds"); const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui/gamepad", dir / "gamepad", fs::copy_options::recursive);
    std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Session s(dir, "gamepad"); s.lua.set_entry_scripts({"raid.lua", "map.lua"});
    for (int member = 0; member < 72; ++member) {
        net::RaidUpdate update; update.general.action = net::kRaidAddMember;
        update.general.player_name = "Member"+std::to_string(member); update.general.parameter = static_cast<std::uint32_t>(member/6);
        update.class_ = 2; update.level = 60;
        s.conn.receive_zone_packet_for_test(net::encode_raid_update(update));
    }
    REQUIRE(s.conn.raid().active); REQUIRE(s.conn.raid().members.size() == 72);
    REQUIRE(s.lua.run_for_test("", "hs.UI.windows.Show('RaidWindow',true) hs.UI.windows.Show('MapViewWnd',true) return true") == "true");
    auto frame = [&](float w, float h) {
        io.DisplaySize = ImVec2(w,h); io.DeltaTime = 1.0f/60.0f;
        s.api.begin_frame(); ImGui::NewFrame(); s.lua.frame(); ImGui::Render();
        CHECK(s.lua.load_status().error_count == 0);
    };
    for (const auto [w,h] : {std::pair{720.0f,700.0f}, {1280.0f,360.0f}}) {
        CAPTURE(w); CAPTURE(h); frame(w,h); frame(w,h);
        REQUIRE(s.lua.run_for_test("", "return hs.UI.windows.ListClick('RaidWindow','RAID_PlayerList',0,'left')") == "true");
        frame(w,h); frame(w,h);
        for (const char* name : {"Raid", "Map"}) {
            CAPTURE(name); const auto* window = ImGui::FindWindowByName(name); REQUIRE(window);
            CHECK(window->Active); CHECK(window->Pos.x >= 0); CHECK(window->Pos.y >= 0);
            CHECK(window->Pos.x+window->Size.x <= w); CHECK(window->Pos.y+window->Size.y <= h);
            CHECK(window->Size.x > 0); CHECK(window->Size.y > 0);
        }
        const auto* raid = ImGui::FindWindowByName("Raid"); REQUIRE(raid);
        const ImGuiWindow* actions = nullptr;
        for (const auto* child : raid->DC.ChildWindows) if (child->Name && std::string_view(child->Name).find("##raidactions") != std::string_view::npos) actions=child;
        REQUIRE(actions); CHECK(actions->Active); CHECK(actions->Size.y > 0);
        CHECK(actions->Pos.x >= raid->Pos.x); CHECK(actions->Pos.y > raid->Pos.y);
        CHECK(actions->Pos.x+actions->Size.x <= raid->Pos.x+raid->Size.x);
        CHECK(actions->Pos.y+actions->Size.y < raid->Pos.y+raid->Size.y);
        if (h <= 360) {
            CHECK(actions->ScrollMax.y > 0);
            ImGui::SetScrollY(const_cast<ImGuiWindow*>(actions), actions->ScrollMax.y);
            frame(w,h); frame(w,h);
            CHECK(actions->Scroll.y == doctest::Approx(actions->ScrollMax.y));
        }
    }
}

TEST_CASE("UI packs: context submenus cascade inward at either screen edge and accept a choice") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_pack_context_menu_cascade");
        const auto dir = scratch.path();
        const auto src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        Session s(dir, pack);
        s.lua.set_entry_scripts({"menus.lua"});

        int choices = 0;
        using Row = app::UiHost::MenuRow;
        Row spell;
        spell.text = "Resist Magic";
        spell.act = [&] { ++choices; };
        Row subcategory;
        subcategory.text = "Resist Buff";
        subcategory.submenu = [&] { return std::vector{spell}; };
        Row category;
        category.text = "Utility Beneficial";
        category.submenu = [&] { return std::vector{subcategory}; };

        const auto panel = [](int menu) {
            const std::string name = "##gamemenu" + std::to_string(menu);
            const auto* window = ImGui::FindWindowByName(name.c_str());
            REQUIRE(window);
            REQUIRE(window->Active);
            return window;
        };
        const auto move_to = [&](const ImGuiWindow& window) {
            io.AddMousePosEvent(window.Pos.x + window.Size.x / 2.0f, window.Pos.y + window.Size.y / 2.0f);
            s.frames(2);
        };
        for (const bool at_right : {true, false}) {
            CAPTURE(at_right);
            io.AddMousePosEvent(-100, -100);
            s.frames(1);
            s.host.open_menu(at_right ? 1580.0f : 8.0f, at_right ? 890.0f : 180.0f, {category});
            s.frames(2);
            REQUIRE(s.host.menus.size() == 1);
            move_to(*panel(1));
            REQUIRE(s.host.menus.size() == 2);
            move_to(*panel(2));
            REQUIRE(s.host.menus.size() == 3);
            REQUIRE(s.lua.load_status().error_count == 0);

            const ImGuiWindow* parent = nullptr;
            for (int m = 1; m <= 3; ++m) {
                const auto* window = panel(m);
                CHECK(window->Pos.x >= 0.0f);
                CHECK(window->Pos.x + window->Size.x <= io.DisplaySize.x);
                CHECK(window->Pos.y >= 0.0f);
                CHECK(window->Pos.y + window->Size.y <= io.DisplaySize.y);
                if (parent) {
                    const float gap = at_right ? parent->Pos.x - (window->Pos.x + window->Size.x)
                                               : window->Pos.x - (parent->Pos.x + parent->Size.x);
                    CHECK(std::abs(gap) <= 1.0f); // ImGui rounds dimensions to pixels
                }
                parent = window;
            }
            move_to(*panel(3));
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            s.frames(1);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            s.frames(2);
            CHECK(choices == (at_right ? 1 : 2));
            CHECK(s.host.menus.empty());
        }
    }
}

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
                std::string show = std::string("hs.ShowWindow('") + screen + "', true)";
                REQUIRE(luaL_dostring(L, show.c_str()) == LUA_OK);
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                std::string hide = std::string("hs.ShowWindow('") + screen + "', false)";
                REQUIRE(luaL_dostring(L, hide.c_str()) == LUA_OK);
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: evolving transfer reviews server previews and drawn confirmation sends once") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool gamepad_pack = std::string_view(pack) == "gamepad";
        test::ScratchDir scratch("ui_pack_evolving_transfer_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        std::ofstream(dir / "UI_Test_Server.ini") << "[Tips]\nShowTips=0\nShowTOTD=0\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            test::Rig rig;
            REQUIRE(rig.to_zone());
            Session s(dir, pack);
            s.host.conn = &rig.conn;
            auto item = [](std::uint32_t id, std::uint32_t record, int slot, std::string name) {
                net::Item value;
                value.id = id;
                value.name = std::move(name);
                value.is_evolving = 1;
                value.instance_id = record;
                value.slot = net::ItemSlot::possessions(static_cast<std::int16_t>(slot));
                value.evolving.evolve_level = 2;
                value.evolving.evolve_max_level = 5;
                value.evolving.progress = 12.5;
                return value;
            };
            const auto source = item(101, 11, 23, "Source evolving item");
            const auto destination = item(102, 12, 24, "Destination evolving item");
            rig.conn.receive_zone_packet_for_test(net::encode_char_inventory({source, destination}));
            rig.conn.open_evolving_transfer();
            REQUIRE(rig.conn.select_evolving_transfer(true, 11));
            REQUIRE(rig.conn.select_evolving_transfer(false, 12));
            auto projected_source = source;
            projected_source.name = "Projected source";
            projected_source.evolving.evolve_level = 1;
            auto projected_destination = destination;
            projected_destination.name = "Projected destination";
            projected_destination.evolving.evolve_level = 4;
            rig.conn.receive_zone_packet_for_test(net::encode(net::EvolvingTransferDetails{
                11, 12, 75, 4, projected_source, projected_destination}));
            REQUIRE(rig.conn.can_transfer_evolving());
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                INFO(code);
                const int rc = luaL_dostring(L, code);
                REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
                lua_settop(L, 0);
            };
            run(R"lua(
                controls, lines, padPress = {}, {}, nil
                local function record(label)
                  if not label:find('evolve-',1,true) then return end
                  local key=label:match('##(.*)$')
                  controls[key]={rect={ui.GetItemRect()},id=ui.GetItemID()}
                end
                local button,invisible,text=ui.Button,ui.InvisibleButton,ui.Text
                ui.Button=function(label,...)
                  local clicked=button(label,...);record(label);return clicked
                end
                ui.InvisibleButton=function(label,...)
                  local clicked=invisible(label,...);record(label);return clicked
                end
                ui.Text=function(value,...)
                  lines[value]=true;return text(value,...)
                end
                ui.SetSetting('hud.size.ItemExpTransferWnd','760,620')
                ui.SetSetting('test.pos.ItemExpTransferWnd','20,20')
                hs.ShowWindow('TipWindow',false)
            )lua");
            if (gamepad_pack) run(R"lua(
                hs.gamepad={
                  connected=function() return true end,type=function() return 'xbox' end,
                  mode=function() return 'menu' end,set_mode=function() return 'menu' end,
                  pressed=function(button) return button==padPress end,
                  down=function(button) return button==padPress end,released=function() return false end,
                  axis=function() return 0 end,glyph=function(button) return button:upper() end,
                  takes_b=function() end,request_focus=function(window) ui.SetWindowFocus(window) end,
                }
            )lua");
            s.frames(30); // the gamepad's initial window-focus settling period
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("assert(lines['Compatibility: 75%'] and lines['Max transfer level: 4'])");
            run("assert(lines['Source after transfer: Projected source (level 1/5, 12.5% experience)'])");
            run("assert(lines['Destination after transfer: Projected destination (level 4/5, 12.5% experience)'])");
            auto rectangle = [&](const char* key) {
                lua_getglobal(L, "controls");
                lua_getfield(L, -1, key);
                REQUIRE(lua_istable(L, -1));
                lua_getfield(L, -1, "rect");
                ImVec4 r;
                float* fields[] = {&r.x, &r.y, &r.z, &r.w};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    *fields[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_settop(L, 0);
                return r;
            };
            auto click = [&](const char* key) {
                const auto r = rectangle(key);
                io.AddMousePosEvent((r.x + r.z) / 2, (r.y + r.w) / 2);
                s.frames(1);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                s.frames(1);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            };
            auto transfer_count = [&] {
                int count = 0;
                for (const auto& packet : rig.server.zone_log)
                    if (const auto request = net::decode_evolving_request(packet);
                        request && request->action == net::EvolvingAction::Transfer) ++count;
                return count;
            };
            click("evolve-transfer");
            run("assert(lines['Confirm experience transfer'] and controls['evolve-confirm'])");
            CHECK_FALSE(rig.conn.evolving_transfer().submitted);
            click("evolve-cancel");
            CHECK(rig.conn.can_transfer_evolving());
            CHECK_FALSE(rig.conn.evolving_transfer().submitted);
            if (gamepad_pack) {
                click("evolve-transfer");
                run("padPress='b'");
                io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
                s.frames(1);
                run("padPress=nil");
                io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
                s.frames(2);
                CHECK(rig.conn.evolving_transfer().shown);
                CHECK(rig.conn.can_transfer_evolving());
                CHECK_FALSE(rig.conn.evolving_transfer().submitted);
            }
            rig.run_for(std::chrono::milliseconds(20));
            CHECK(transfer_count() == 0);
            click("evolve-transfer");
            if (gamepad_pack) {
                // Put ImGui's navigation on the actual drawn Confirm item, then activate it with A.
                lua_getglobal(L, "controls");
                lua_getfield(L, -1, "evolve-confirm");
                lua_getfield(L, -1, "id");
                const auto id = static_cast<ImGuiID>(lua_tointeger(L, -1));
                lua_settop(L, 0);
                ImGuiWindow* content = nullptr;
                for (ImGuiWindow* window : ctx->Windows)
                    if (window->Active && std::string_view(window->Name).find("##evolve-content") != std::string_view::npos)
                        content = window;
                REQUIRE(content);
                ImGui::FocusWindow(content);
                ImGui::SetFocusID(id, content);
                ImGui::SetNavCursorVisible(true);
                io.AddMousePosEvent(-100, -100);
                s.frames(1);
                io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
                s.frames(1);
                io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
                s.frames(2);
            } else click("evolve-confirm");
            CHECK(rig.conn.evolving_transfer().submitted);
            REQUIRE(rig.until([&] { return transfer_count() == 1; }));
            s.frames(3);
            rig.run_for(std::chrono::milliseconds(20));
            CHECK(transfer_count() == 1);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: active discipline shows seconds and its icon cancels on a mouse click") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_pack_discipline_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            auto* L = s.lua.state();
            auto run = [&](const char* code) {
                const int rc = luaL_dostring(L, code);
                INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
                REQUIRE(rc == LUA_OK);
                const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_settop(L, 0);
                return result;
            };
            run(R"lua(
discActive, discFraction, discTime, endClicks = true, 1, "1:00", 0
local shown, replace, child, text, art, gauge, click = hs.WindowShown, hs.ReplaceWindow,
  hs.ChildShown, hs.WindowText, hs.ChildArt, hs.Gauge, hs.ClickChild
hs.WindowShown = function(win) if win == "CombatAbilityWnd" then return true end return shown(win) end
hs.ReplaceWindow = function(win)
  if win == "CombatAbilityWnd" then return true, 40, 40, 360, 190 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return discActive end
  return child(win, id)
end
hs.WindowText = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectLabel" then return "Test Discipline" end
  return text(win, id)
end
hs.ChildArt = function(win, id, kind)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return nil end
  return art(win, id, kind)
end
hs.Gauge = function(kind)
  if kind == 26 then gaugeReads = (gaugeReads or 0) + 1; return discFraction, discTime end
  return gauge(kind)
end
hs.ClickChild = function(win, id, button)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then
    assert(button == "left")
    endClicks, discActive = endClicks + 1, false
    return true
  end
  return click(win, id, button)
end
local button, drawText = ui.InvisibleButton, ui.DrawText
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##cawEffect" then discIcon = { ui.GetItemRect() } end
  return hit
end
ui.DrawText = function(x, y, r, g, b, a, value)
  if value == discTime then drewDiscTime = true end
  return drawText(x, y, r, g, b, a, value)
end
)lua");
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(run("return tostring(drewDiscTime and gaugeReads > 0 and discIcon ~= nil)") == "true");
            run("discFraction, discTime, drewDiscTime = 0.5, '30s', false");
            s.frames(2);
            CHECK(run("return tostring(drewDiscTime)") == "true");
            lua_getglobal(L, "discIcon");
            float rect[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            CHECK(run("return tostring(endClicks)") == "1");
            run("gaugeReads, drewDiscTime, discIcon = 0, false, nil");
            s.frames(2);
            CHECK(run("return tostring(gaugeReads == 0 and not drewDiscTime and discIcon == nil)") == "true");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the gamepad's first focus is the discipline list, and LB ends the active discipline") {
    test::ScratchDir scratch("ui_pack_discipline_pad_test");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad", dir / "gamepad",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "gamepad");
        auto* L = s.lua.state();
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        // A pad in menu mode; padPress is the button that went down this frame.
        run(R"lua(
padPress, windowShown, discActive, endClicks, uses = nil, false, true, 0, 0
hs.gamepad = {
  connected = function() return true end,
  type = function() return 'xbox' end,
  mode = function() return 'menu' end,
  set_mode = function() return 'menu' end,
  pressed = function(b) return b == padPress end,
  down = function(b) return b == padPress end,
  released = function() return false end,
  axis = function() return 0 end,
  glyph = function(b) return string.upper(b) end,
  takes_b = function() end,
  request_focus = function(window) ui.SetWindowFocus(window) end,
}
local shown, replace, child, text, art, click = hs.WindowShown, hs.ReplaceWindow, hs.ChildShown, hs.WindowText,
  hs.ChildArt, hs.ClickChild
hs.WindowShown = function(win) if win == "CombatAbilityWnd" then return windowShown end return shown(win) end
hs.ReplaceWindow = function(win)
  if win == "CombatAbilityWnd" then return true, 40, 40, 360, 190 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return discActive end
  return child(win, id)
end
hs.WindowText = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectLabel" then return "Test Discipline" end
  return text(win, id)
end
hs.ChildArt = function(win, id, kind)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return nil end
  return art(win, id, kind)
end
hs.Gauge = function() return 0.5, "30s" end
hs.Disciplines = function()
  return { { index = 0, spell = 101, name = "Guard", icon = 0, level = 50, endurance = 0, reuse = 60,
    remaining = 0, timer = 1 } }
end
hs.PressButtonFrom = function() uses = uses + 1 end
hs.ClickChild = function(win, id, button)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then
    endClicks, discActive = endClicks + 1, false
    return true
  end
  return click(win, id, button)
end
local button = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##cawEffect" then effectFocused = ui.IsItemFocused() end
  if id == "##row" then rowFocused = ui.IsItemFocused() end
  return hit
end
)lua");
        s.frames(240); // the pack takes the pad's focus for a window only from 3 seconds after zoning in
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        run("windowShown = true");
        s.frames(30); // the window appears with the focus; past its settle time
        CHECK(run("return tostring(rowFocused == true and effectFocused == false)") == "true");
        auto& io = ImGui::GetIO();
        // A press of A uses the focused discipline; the running one is not ended. (ImGui activates only with its
        // navigation cursor shown, as it is once the pad has moved it.)
        ImGui::SetNavCursorVisible(true);
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
        s.frames(1);
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
        s.frames(2);
        CHECK(run("return tostring(endClicks == 0 and uses == 1)") == "true");
        // LB ends it.
        run("padPress = 'lb'");
        s.frames(1);
        run("padPress = nil");
        s.frames(2);
        CHECK(run("return tostring(endClicks)") == "1");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: offered-task starting-zone hint and explicit preview follow the selected offer") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("ui_pack_task_offer_rewards");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            auto* L = s.lua.state();
            auto run = [&](const char* code) {
                const int rc = luaL_dostring(L, code);
                INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
                REQUIRE(rc == LUA_OK);
                const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_settop(L, 0);
                return result;
            };
            run(R"lua(
offerSelected, offerPreviewEnabled, offerPreviewClicks = 0, true, {}
offerWindowShown, activeTaskShown, activePreviewEnabled, activePreviewClicks = true, false, true, 0
offerTexts = {}
assert(type(hs.ChildState) == "function", "ChildState must be registered before the fixture wraps it")
local shown, replace, childShown, childState, text, list, row, listClick, click =
  hs.WindowShown, hs.ReplaceWindow, hs.ChildShown, hs.ChildState,
  hs.WindowText, hs.ListInfo, hs.ListRow, hs.ListClick, hs.ClickChild
hs.WindowShown = function(win)
  if win == "TaskSelectWnd" then return offerWindowShown end
  if win == "TaskWnd" then return activeTaskShown end
  return shown(win)
end
hs.ReplaceWindow = function(win)
  if win == "TaskSelectWnd" then return true, 40, 40, 600, 400 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "TaskSelectWnd" then return true end
  return childShown(win, id)
end
hs.ChildState = function(win, id)
  if win == "TaskSelectWnd" then
    if id == "TSEL_PreviewRewardButton" then return offerPreviewEnabled, false end
    return true, false
  end
  if win == "TaskWnd" and id == "TASK_PreviewRewardButton" then return activePreviewEnabled, false end
  return childState(win, id)
end
hs.ListInfo = function(win, id)
  if win == "TaskSelectWnd" and id == "TaskList" then return 2, 2, offerSelected end
  if win == "TaskWnd" then
    if id == "TASK_TaskList" then return 1, 3, 0 end
    if id == "TASK_TaskElementList" then return 1, 3, -1 end
  end
  return list(win, id)
end
hs.ListRow = function(win, id, index)
  if win == "TaskSelectWnd" and id == "TaskList" then
    return {index == 0 and "First offer" or "Second offer", ""}
  end
  if win == "TaskWnd" then
    assert(index == 0)
    if id == "TASK_TaskList" then return {"Q", "Active task", "Unlimited"} end
    if id == "TASK_TaskElementList" then return {"ActiveObjective", "0/1", "North Qeynos"} end
  end
  return row(win, id, index)
end
hs.ListClick = function(win, id, index, ...)
  if win == "TaskSelectWnd" and id == "TaskList" then offerSelected = index; return end
  if win == "TaskWnd" and id == "TASK_TaskList" then assert(index == 0); return end
  return listClick(win, id, index, ...)
end
hs.WindowText = function(win, id)
  if win == "TaskSelectWnd" and id == "Description" then
    return offerSelected == 0 and "FirstDescription<br>TaskBeginsInFirst" or "SecondDescription<br>TaskBeginsInSecond"
  end
  if win == "TaskWnd" and id == "TASK_Description" then return "ActiveTaskDescription" end
  return text(win, id)
end
hs.ClickChild = function(win, id, button)
  if win == "TaskSelectWnd" then
    assert(id == "TSEL_PreviewRewardButton" and button == "left", "Preview must use its matching native action")
    offerPreviewClicks[#offerPreviewClicks + 1] = offerSelected
    return true
  end
  if win == "TaskWnd" then
    assert(id == "TASK_PreviewRewardButton" and button == "left", "Active preview must use its matching native action")
    activePreviewClicks = activePreviewClicks + 1
    return true
  end
  return click(win, id, button)
end
local button, invisible, colored, wrapped, drawText = ui.Button, ui.InvisibleButton, ui.TextColored, ui.TextWrapped, ui.DrawText
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Preview Reward##TSEL_PreviewRewardButton" then offerPreviewRect = {ui.GetItemRect()} end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##offerpreview" or id == "##taskpreview" then offerPreviewRect = {ui.GetItemRect()} end
  return hit
end
ui.TextColored = function(r, g, b, a, value)
  offerTexts[value] = true
  return colored(r, g, b, a, value)
end
ui.TextWrapped = function(value)
  offerTexts[value] = true
  return wrapped(value)
end
ui.DrawText = function(x, y, r, g, b, a, value)
  offerTexts[value] = true
  return drawText(x, y, r, g, b, a, value)
end
)lua");
            s.frames(20);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(run("return tostring(offerTexts.TaskBeginsInFirst and offerPreviewRect ~= nil and #offerPreviewClicks == 0)") == "true");
            auto click_preview = [&] {
                lua_getglobal(L, "offerPreviewRect");
                REQUIRE(lua_istable(L, -1));
                float rect[4]{};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    rect[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_settop(L, 0);
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
                s.frames(1);
                io.AddMouseButtonEvent(0, true);
                s.frames(1);
                io.AddMouseButtonEvent(0, false);
                s.frames(1);
            };
            click_preview();
            CHECK(run("return tostring(#offerPreviewClicks == 1 and offerPreviewClicks[1] == 0)") == "true");
            run("offerSelected, offerTexts = 1, {}");
            s.frames(2);
            CHECK(run("return tostring(offerTexts.TaskBeginsInSecond and not offerTexts.TaskBeginsInFirst and #offerPreviewClicks == 1)") == "true");
            click_preview();
            CHECK(run("return tostring(#offerPreviewClicks == 2 and offerPreviewClicks[2] == 1)") == "true");
            run("offerPreviewEnabled = false");
            s.frames(2);
            click_preview();
            CHECK(run("return tostring(#offerPreviewClicks)") == "2");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (std::string_view(pack) == "gamepad") {
                run("offerWindowShown, activeTaskShown, offerPreviewRect, offerTexts = false, true, nil, {}");
                s.frames(20);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                CHECK(run("return tostring(offerTexts.ActiveTaskDescription and offerTexts.ActiveObjective and offerPreviewRect ~= nil and activePreviewClicks == 0)") == "true");
                click_preview();
                CHECK(run("return tostring(activePreviewClicks)") == "1");
                run("activePreviewEnabled = false");
                s.frames(2);
                click_preview();
                CHECK(run("return tostring(activePreviewClicks == 1 and #offerPreviewClicks == 2)") == "true");
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: linked discipline cooldowns show red fills and matching seconds until ready") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_pack_discipline_cooldown_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack);
            auto* L = s.lua.state();
            auto run = [&](const char* code) {
                const int rc = luaL_dostring(L, code);
                INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
                REQUIRE(rc == LUA_OK);
                const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_settop(L, 0);
                return result;
            };
            run(R"lua(
sharedLeft, expectedTime, timerDraws, redArea, redFills, abilityClicks = 600, "10:00", 0, 0, 0, 0
local shown, replace, child, text, cooldown, click = hs.WindowShown, hs.ReplaceWindow,
  hs.ChildShown, hs.WindowText, hs.ButtonCoolDown, hs.ClickChild
local names, reuse = { "Long Guard", "Short Guard", "Independent" }, { 600, 300, 120 }
hs.WindowShown = function(win) if win == "CombatAbilityWnd" then return true end return shown(win) end
hs.ReplaceWindow = function(win)
  if win == "CombatAbilityWnd" then return true, 40, 40, 360, 190 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return false end
  return child(win, id)
end
hs.WindowText = function(win, id)
  local n = id:match("^CAW_Button(%d)$")
  if win == "CombatAbilityWnd" and n then return names[tonumber(n)] or "" end
  return text(win, id)
end
hs.ButtonCoolDown = function(win, id, y0, y1)
  local n = id:match("^CAW_Button(%d)$")
  n = n and tonumber(n)
  if win == "CombatAbilityWnd" and n and n <= 2 and sharedLeft > 0 then
    return y0, 0, 0, 0, 0.25, sharedLeft * 1000, reuse[n] * 1000
  end
  return cooldown(win, id, y0, y1)
end
local row = 0
hs.Disciplines = function()
  row = 0
  local out = {}
  for i = 1, 3 do
    out[i] = { index = i - 1, spell = 100 + i, name = names[i], icon = 0, level = 50,
      endurance = 0, reuse = reuse[i], remaining = i <= 2 and sharedLeft or 0, timer = i <= 2 and 1 or 2 }
  end
  return out
end
hs.ClickChild = function(win, id, button)
  if win == "CombatAbilityWnd" and id == "CAW_Button1" and button == "left" then
    abilityClicks = abilityClicks + 1; return true
  end
  return click(win, id, button)
end
hs.PressButtonFrom = function(win, id, n)
  assert(win == "CombatAbilityWnd" and id == "Discipline" and n == 1)
  abilityClicks = abilityClicks + 1
end
local button, rect, drawText = ui.InvisibleButton, ui.DrawRectFilled, ui.DrawText
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##row" then row = row + 1 end
  if id == "##caw1" or (id == "##row" and row == 1) then firstAbility = { ui.GetItemRect() } end
  return hit
end
ui.DrawRectFilled = function(x0, y0, x1, y1, r, g, b, a, ...)
  if a and math.abs(a - 0.20) < 0.0001 and r > g * 1.5 and r > b * 1.5 then
    redFills, redArea = redFills + 1, redArea + (x1 - x0) * (y1 - y0)
  end
  return rect(x0, y0, x1, y1, r, g, b, a, ...)
end
ui.DrawText = function(x, y, r, g, b, a, value)
  if value == expectedTime then timerDraws = timerDraws + 1 end
  return drawText(x, y, r, g, b, a, value)
end
)lua");
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("redFills, redArea, timerDraws = 0, 0, 0");
            s.frames(1);
            CHECK(run("return tostring(redFills == 2 and timerDraws >= 2)") == "true");
            run("fullArea = redArea");
            lua_getglobal(L, "firstAbility");
            float rect[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            CHECK(run("return tostring(abilityClicks)") == "1"); // The normal use handler still receives the click.
            run("sharedLeft, expectedTime, redFills, redArea, timerDraws = 30, '0:30', 0, 0, 0");
            s.frames(1);
            CHECK(run("return tostring(redFills == 2 and timerDraws >= 2 and redArea < fullArea)") == "true");
            run("sharedLeft, redFills, timerDraws = 0, 0, 0");
            s.frames(1);
            CHECK(run("return tostring(redFills == 0 and timerDraws == 0)") == "true");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
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
            CHECK(s.api.list_texts("RespawnWnd", "OptionsList") == std::vector<std::string>{"*", "r"});

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

TEST_CASE("UI packs: the respawn window leaves the focus alone for a question that is not a resurrection") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool gamepad_pack = std::string_view(pack) == "gamepad";
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        test::ScratchDir scratch("ui_pack_respawn_plain_question_test");
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
            app::UiHost::Confirm q;
            q.text = "A question of a window's own";
            s.host.confirm = std::move(q);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            ImGuiWindow* confirm = ImGui::FindWindowByName(gamepad_pack ? "Confirm" : "###modern.ConfirmationDialogBox");
            ImGuiWindow* respawn = ImGui::FindWindowByName(gamepad_pack ? "Respawn" : "###modern.RespawnWnd");
            REQUIRE(confirm);
            REQUIRE(respawn);
            CHECK(confirm->Active);
            REQUIRE(luaL_dostring(s.lua.state(),
                "assert(hs.ChildShown('ConfirmationDialogBox', 'ResurrectionOffer') == false)") == LUA_OK);
            ImGui::FocusWindow(respawn);
            s.frames(3);
            REQUIRE(GImGui->NavWindow);
            CHECK(GImGui->NavWindow->RootWindow == respawn);
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
local TextColored, DrawText, ListInfo = ui.TextColored, ui.DrawText, hs.ListInfo
ui.TextColored = function(r, g, b, a, text, ...)
  if text == "Resurrect" then RespawnPaintColor = { r, g, b } end
  return TextColored(r, g, b, a, text, ...)
end
ui.DrawText = function(x, y, r, g, b, a, text, ...)
  if text == "Resurrect" then RespawnPaintColor = { r, g, b } end
  return DrawText(x, y, r, g, b, a, text, ...)
end
hs.ListInfo = function(win, child)
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
                REQUIRE(luaL_dostring(s.lua.state(),
                    "assert(hs.ChildShown('ConfirmationDialogBox', 'ResurrectionOffer') == true)") == LUA_OK);
                // Another window taken in hand while the offer waits (typing in chat) keeps the focus: the offer
                // was put in front once, as it appeared, not every frame (which would take the active widget).
                ImGui::FocusWindow(respawn);
                s.frames(3);
                REQUIRE(GImGui->NavWindow);
                CHECK(GImGui->NavWindow->RootWindow == respawn);
                CHECK(confirm->Active);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

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
                CHECK(markers[1] == "r"); // the original's marker stays; Yes only enables the line
                if (accept) {
                    REQUIRE(luaL_dostring(s.lua.state(),
                        "assert(RespawnPaintColor and math.abs(RespawnPaintColor[1]-0.25)<0.001 and "
                        "math.abs(RespawnPaintColor[2]-1)<0.001 and math.abs(RespawnPaintColor[3]-0.35)<0.001)") == LUA_OK);
                }
                // Yes leaves the selection where it was (0x74e9e0 never changes it).
                REQUIRE(luaL_dostring(s.lua.state(), "assert(RespawnPaintSelected==0)") == LUA_OK);
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

TEST_CASE("UI packs (modern): login Add-ons opens within narrow and short viewports") {
    const auto src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    test::ScratchDir scratch("ui_pack_login_addon_bounds");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        Session s(dir, "modern", {}, false);
        s.display_size = ImVec2(1280, 720);
        ui::LoginView login;
        login.servers_dir = dir / "servers";
        login.ui = login.default_ui = "modern";
        ServerProfile profile;
        profile.id = "alpha";
        profile.name = "Alpha";
        profile.host = "127.0.0.1";
        profile.username = "first";
        profile.accounts = {{"first", "Bob", "World One", {{"Bob", "World One", 12, "WAR"}}, false, false}};
        login.profiles = {profile};
        ui::select_profile(login, 0);
        login.status = {"Enter your password to log in.", true};
        login.addons.available = login.addons.browsed = true;
        login.addons.suggestions = {
            {"", "owner/one", "Required to play on this server: owner/one.", true, true},
            {"", "owner/two", "Required to play on this server: owner/two.", true, true},
            {"", "owner/three", "Required to play on this server: owner/three.", true, true},
            {"", "owner/extra", "Alpha suggests another add-on: owner/extra.", true}};
        s.host.login = &login;
        lua_State* L = s.lua.state();
        // Observe the real button bounds. The button only reports a click when it receives mouse input.
        REQUIRE(luaL_dostring(L, R"lua(
            ui.SetSetting('test.login', 'boxset')
            local button, lastFrame = ui.Button, -1
            ui.Button = function(label, ...)
                local clicked = button(label, ...)
                local frame = ui.GetFrameCount()
                if lastFrame ~= frame then buttonRects, lastFrame = {}, frame end
                if not buttonRects[label] then buttonRects[label] = { ui.GetItemRect() } end
                return clicked
            end
        )lua") == LUA_OK);
        auto button_rect = [&](const char* label) {
            INFO(label);
            lua_getglobal(L, "buttonRects");
            REQUIRE(lua_istable(L, -1));
            lua_getfield(L, -1, label);
            REQUIRE(lua_istable(L, -1));
            std::array<float, 4> rect{};
            for (std::size_t i = 0; i < rect.size(); ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i + 1));
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
            return ImRect(rect[0], rect[1], rect[2], rect[3]);
        };
        auto click = [&](const char* label) {
            INFO(label);
            const auto rect = button_rect(label);
            REQUIRE(ImRect(0, 0, s.display_size.x, s.display_size.y).Contains(rect));
            const auto center = rect.GetCenter();
            io.AddMousePosEvent(center.x, center.y);
            s.frames(1);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            s.frames(1);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            s.frames(6); // Let a newly opened card settle at its measured height.
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        };
        auto active = [](const char* key) -> ImGuiWindow* {
            auto* window = ImGui::FindWindowByName(key);
            return window && window->Active ? window : nullptr;
        };
        auto fits = [&](const ImGuiWindow* window) {
            REQUIRE(window);
            CHECK(ImRect(0, 0, s.display_size.x, s.display_size.y).Contains(window->Rect()));
        };
        constexpr char login_key[] = "###modern.pregame.login";
        constexpr char box_key[] = "###modern.pregame.login.boxset";
        constexpr char quick_key[] = "###modern.pregame.login.quick";
        constexpr char addons_key[] = "###modern.pregame.login.addons";

        s.frames(4);
        fits(active(login_key));
        REQUIRE(active(box_key));
        REQUIRE(active(quick_key));
        click("Add-ons");
        REQUIRE(login.addons.open);
        auto* addons = active(addons_key);
        fits(addons);
        CHECK(addons->Rect().GetCenter().x == doctest::Approx(640.0f).epsilon(0.003));
        CHECK_FALSE(active(login_key));
        CHECK_FALSE(active(box_key));
        CHECK_FALSE(active(quick_key));
        CHECK(button_rect("Close").Max.y <= addons->Pos.y + 112);
        click("Close");
        CHECK_FALSE(login.addons.open);
        CHECK_FALSE(active(addons_key));
        fits(active(login_key));

        click("Review required add-on");
        CHECK(s.host.login_action == static_cast<int>(ui::LoginAction::AddonSuggestLook));
        CHECK(login.addons.choice == "owner/one");
        // Apply the client's response to AddonSuggestLook without fetching a real add-on.
        login.addons.open = true;
        auto& offer = login.addons.offer.emplace();
        offer.name = "one";
        offer.kind = "plugin";
        offer.source = "owner/one";
        offer.description = "An add-on the player needs to review before joining this server.";
        offer.caps = {"read the game state", "draw windows"};
        s.display_size = ImVec2(760, 380);
        s.frames(4);
        addons = active(addons_key);
        fits(addons);
        CHECK(addons->ScrollbarY);
        CHECK(button_rect("Close").Max.y <= addons->Pos.y + 112);
        click("Close");
        CHECK_FALSE(login.addons.open);
        auto* login_window = active(login_key);
        fits(login_window);
        CHECK(login_window->ScrollbarY);

        login.addons.offer.reset();
        s.display_size = ImVec2(380, 680);
        s.frames(4);
        fits(active(login_key));
        click("Add-ons");
        REQUIRE(login.addons.open);
        fits(active(addons_key));
        click("Close");
        CHECK_FALSE(login.addons.open);

        s.display_size = ImVec2(1900, 900);
        s.frames(4);
        click("Add-ons");
        addons = active(addons_key);
        login_window = active(login_key);
        fits(addons);
        fits(login_window);
        CHECK(login_window->Rect().Max.x < addons->Pos.x);
        CHECK(addons->Pos.y == login_window->Pos.y);
    }
    ImGui::DestroyContext(ctx);
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
            REQUIRE(luaL_dostring(s.lua.state(), "hs.InGame = function() return false end; hs.UI.session.InGame = hs.InGame") == LUA_OK);

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
            if (std::string(pack) == "gamepad") {
                // An unavailable installed descriptor remains in the list, while controller cycling skips it.
                ui::UiPack unavailable;
                unavailable.id = "addon.example.unavailable";
                unavailable.name = "Unavailable interface";
                unavailable.available = false;
                unavailable.unavailable_reason = "No native runtime";
                login.ui_packs.insert(login.ui_packs.begin() + 1, unavailable);
                login.ui = "modern";
                REQUIRE(luaL_dostring(s.lua.state(), R"lua(
local original = ui.InvisibleButton
INTERFACE_PICKED, INTERFACE_SEEN = false, 0
ui.InvisibleButton = function(id, ...)
  local hit = original(id, ...)
  if id == '##ui' then
    INTERFACE_SEEN = INTERFACE_SEEN + 1
    if not INTERFACE_PICKED then INTERFACE_PICKED = true; return true end
  end
  return hit
end
RESTORE_INTERFACE_BUTTON = function() ui.InvisibleButton = original end
)lua") == LUA_OK);
                s.frames(2);
                CHECK(login.ui == "gamepad");
                CHECK(s.lua.run_for_test("", "return INTERFACE_SEEN > 0") == "true");
                REQUIRE(luaL_dostring(s.lua.state(), "RESTORE_INTERFACE_BUTTON()") == LUA_OK);
                login.ui_packs.erase(login.ui_packs.begin() + 1);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            // A returned ticket is an optional action, usable without a password, and hidden while busy.
            lua_State* login_lua = s.lua.state();
            REQUIRE(luaL_dostring(login_lua, R"lua(
sessionButtons, clickSession = 0, false
local button, invisible = ui.Button, ui.InvisibleButton
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Sign in with session" then
    sessionButtons = sessionButtons + 1
    return clickSession or hit
  end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##resume_token" then
    sessionButtons = sessionButtons + 1
    return clickSession or hit
  end
  return hit
end
)lua") == LUA_OK);
            auto session_buttons = [&] {
                lua_getglobal(login_lua, "sessionButtons");
                const auto n = lua_tointeger(login_lua, -1);
                lua_pop(login_lua, 1);
                return n;
            };
            s.frames(2);
            CHECK(session_buttons() == 0);
            login.session_ticket_available = true;
            REQUIRE(luaL_dostring(login_lua, "clickSession = true") == LUA_OK);
            s.frames(2);
            CHECK(session_buttons() > 0);
            CHECK(s.host.login_action == static_cast<int>(ui::LoginAction::ResumeToken));
            s.host.login_action.reset();
            login.busy = true;
            REQUIRE(luaL_dostring(login_lua, "sessionButtons = 0") == LUA_OK);
            s.frames(2);
            CHECK(session_buttons() == 0);
            CHECK_FALSE(s.host.login_action);
            login.busy = false;
            login.session_ticket_available = false;
            s.frames(2);
            CHECK(session_buttons() == 0);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
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
            // An installed upscaler (docs/addons.md "Upscalers").
            ui::AddonRowView upscaler = tool;
            upscaler.name = "xess";
            upscaler.kind = "upscaler";
            upscaler.source = "owner/xess";
            upscaler.can_start = false;
            upscaler.folder = "/x/upscalers/xess";
            login.addons.rows.push_back(upscaler);
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
            // An upscaler's: no permissions, its own warning.
            login.addons.offer->kind = "upscaler";
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
            login.addons.suggestions.front().required = true;
            login.addons.suggestions.front().text = "Required to play on this server: servplug.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            REQUIRE(luaL_dostring(s.lua.state(), "assert(hs.UI.pregame.LoginScreen().addons.suggestions[1].required); "
                                 "assert(not hs.UI.pregame.LoginDo('addonSuggestLater', 'owner/servplug')); "
                                 "assert(not hs.UI.pregame.LoginDo('addonSuggestNever', 'owner/servplug')); "
                                 "assert(hs.UI.pregame.LoginDo('addonSuggestLook', 'owner/servplug'))") == LUA_OK);
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
            REQUIRE(luaL_dostring(s.lua.state(), "local v=hs.ServerSelect(); assert(v.servers[2].status=='Locked' and v.servers[2].canJoin); "
                                     "assert(hs.ServerSelectDo('play',2)); assert(not hs.ServerSelectDo('play',3))") == LUA_OK);
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

TEST_CASE("UI packs: the login screen's More options pick the texture pack (hs.Option texture_pack)") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_login_texture_pack_test");
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
            REQUIRE(luaL_dostring(s.lua.state(), "hs.InGame = function() return false end") == LUA_OK);
            // A new server (no saved accounts, so the gamepad pack's More page has no box set list above).
            ui::LoginView login;
            login.servers_dir = dir / "servers";
            login.ui_packs = {{"modern", "Modern", "", "mouse", false, {}}, {"gamepad", "Gamepad", "", "gamepad", false, {}}};
            login.ui = login.default_ui = pack;
            login.game_folders = {{"/games/rof2", false, true}};
            s.host.login = &login;
            int chosen = 0; // Off
            bool asked = false;
            std::vector<int> set;
            s.host.find_option = [&](std::string_view o) -> std::optional<int> {
                if (o != "texture_pack") return std::nullopt;
                asked = true;
                return chosen;
            };
            s.host.option_choices = [](std::string_view o) {
                return o == "texture_pack" ? std::vector<std::string>{"Off", "classic", "default"} : std::vector<std::string>{};
            };
            s.host.set_option = [&](std::string_view o, int v) {
                if (o != "texture_pack") return;
                set.push_back(v);
                chosen = v;
            };
            lua_State* L = s.lua.state();
            // More options open from the first frame; a click on the pack "classic": the modern pack's list entry,
            // the gamepad pack's field and then the second row of its list.
            REQUIRE(luaL_dostring(L, R"lua(
ui.SetSetting('test.login', 'more')
clickPack, rows = false, 0
local selectable, invisible = ui.Selectable, ui.InvisibleButton
ui.Selectable = function(label, ...)
  local hit = selectable(label, ...)
  return hit or (clickPack and label == "classic")
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if not clickPack then return hit end
  if id == "##texpack" then return true end
  if id == "##row" then
    rows = rows + 1
    return hit or rows == 2
  end
  return hit
end
)lua") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(asked);
            CHECK(set.empty());
            if (std::string(pack) == "modern") {
                // the list drawn as an open combo would draw it (its entries in the row itself: no popup to end)
                REQUIRE(luaL_dostring(L, R"lua(
local begin, finish, faked = ui.BeginCombo, ui.EndCombo, false
ui.BeginCombo = function(id, ...)
  if id == "##texture_pack" and clickPack then faked = true; return true end
  return begin(id, ...)
end
ui.EndCombo = function()
  if faked then faked = false; return end
  return finish()
end
clickPack = true
)lua") == LUA_OK);
                s.frames(1);
            } else {
                REQUIRE(luaL_dostring(L, "clickPack = true") == LUA_OK);
                s.frames(1); // the field opens the list
                REQUIRE(luaL_dostring(L, "rows = 0") == LUA_OK);
                s.frames(1); // its second row
            }
            REQUIRE(luaL_dostring(L, "clickPack = false") == LUA_OK);
            CHECK(set == std::vector<int>{1});
            CHECK(chosen == 1);
            s.frames(2); // the note that it is used from the next start
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
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
            REQUIRE(luaL_dostring(L, "local n = hs.ListInfo('MarketplaceWnd', 'MKPW_CategoriesTree') return n") == LUA_OK);
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

TEST_CASE("UI packs: the spell book's sets and set editor draw; hs.SaveSpellSet / EditSpellSet save, memorizing nothing") {
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
            CHECK(number("return hs.SaveSpellSet('Heals', { 200, -1, 201, nil, 999 })") == 1);
            CHECK(sets.at(0).name == "Heals");
            CHECK(sets.at(0).spells[0] == 200);
            CHECK(sets.at(0).spells[1] == -1);
            CHECK(sets.at(0).spells[2] == 201);
            CHECK(sets.at(0).spells[3] == -1);
            CHECK(sets.at(0).spells[4] == 999);
            CHECK(number("return hs.SaveSpellSet('', { 200 }) or -1") == -1);
            CHECK(number("return hs.EditSpellSet(1, 'Alpha', { 'x', 202, -5 })") == 1);
            CHECK(sets.at(0).name == "Alpha");
            CHECK(sets.at(0).spells[0] == -1);
            CHECK(sets.at(0).spells[1] == 202);
            CHECK(sets.at(0).spells[2] == -1);
            CHECK(number("return hs.EditSpellSet(2, 'Beta', {}) or -1") == -1); // set 2 is not in use
            CHECK(number("return hs.SaveSpellSet('Pulls', { 202, 201 })") == 2);
            // nothing was memorized: no gem, no memorize, no set being memorized
            for (int g = 0; g < game::kSpellGems; ++g)
                CHECK(conn.spell_state().gems[static_cast<std::size_t>(g)].spell ==
                      gems_before[static_cast<std::size_t>(g)].spell);
            CHECK_FALSE(conn.spell_state().memorizing.has_value());
            CHECK_FALSE(conn.memorizing_set());
            run("local t = hs.SpellSets() assert(t[1].name == 'Alpha' and t[1].spells[2] == 202 and t[2].name == 'Pulls')");

            // The views: the list of sets, the editor of set 1, a new set, a new set from the gems, back.
            run("hs.ShowWindow('SpellBookWnd', true)");
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
                std::string code = "return hs.SaveSpellSet('Set" + std::to_string(i) + "', { 200 })";
                CHECK(number(code.c_str()) > 0);
            }
            CHECK(number("return hs.SaveSpellSet('Eleventh', { 200 }) or -1") == -1);
            CHECK(number("return hs.SaveSpellSet('pulls', { 201 })") > 0);
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
            hs.InGame = function() return false end
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
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('SpellBookWnd', true) ui.SetSetting('test.book.tab', 'details')"
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
            REQUIRE(luaL_dostring(L, "hs.ShowWindow('SpellBookWnd', true) ui.SetSetting('test.book.tab', 'details')"
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
            REQUIRE(luaL_dostring(L, "ui.SetSetting('test.inv.page', 'currency') hs.ShowWindow('InventoryWindow', true)") ==
                    LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // the page keeps the stock Display Missing off with nothing held (modern: Show all off)
            REQUIRE(luaL_dostring(L, "return (select(2, hs.ChildState('InventoryWindow', 'AltCurr_DisplayMissingButton')))") ==
                    LUA_OK);
            CHECK_FALSE(lua_toboolean(L, -1));
            lua_settop(L, 0);
            REQUIRE(luaL_dostring(L, "hs.ListClick('InventoryWindow', 'AltCurr_PointList', 0, 'left') "
                                     "return (hs.ChildState('InventoryWindow', 'AltCurr_CreateItemButton'))") == LUA_OK);
            CHECK(lua_toboolean(L, -1)); // 31, held: Claim is on
            lua_settop(L, 0);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// Hailstone's Find window (app/lua_ui_find.cpp): the find key's state opens it (modern: the map on its Find page;
// gamepad: a window of its own), it draws the zone's NPCs, and the map closing closes it.
TEST_CASE("UI packs: the Find window draws the zone's NPCs and follows hs.FindOpen") {
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
            REQUIRE(luaL_dostring(L, "ui.SetSetting('map.npcMarks', '1') hs.FindOpen(true)") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (std::string(pack) == "modern") {
                CHECK(s.api.shown("MapViewWnd")); // the map opened on its Find page
                REQUIRE(luaL_dostring(L, "hs.ShowWindow('MapViewWnd', false)") == LUA_OK);
                s.frames(2);
                CHECK_FALSE(s.host.find_open); // the map closing closed Find
                REQUIRE(luaL_dostring(L, "hs.FindOpen(true)") == LUA_OK);
                s.frames(2);
                CHECK(s.api.shown("MapViewWnd"));
                s.host.find_open = false; // the find key again: the map goes
                s.frames(2);
                CHECK_FALSE(s.api.shown("MapViewWnd"));
            } else {
                REQUIRE(luaL_dostring(L, "hs.FindOpen(false)") == LUA_OK);
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
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('ExtendedTargetWnd', true)") == LUA_OK);
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

// The modern extended target window: a row for every slot the server gives, an empty one too (its right click is how
// an empty slot gets a type, Current Target among them), sized to its rows until the player sizes the frame; a frame
// shorter than its rows scrolls them.
TEST_CASE("UI packs (modern): the extended target window shows empty slots and scrolls when sized short") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_xtarget_rows_test");
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
        // Five slots, the first holding a gnoll; the other four empty.
        ByteWriter b;
        b.le<std::uint32_t>(5);
        b.le<std::uint32_t>(1);
        b.le<std::uint32_t>(0);
        b.le<std::uint8_t>(1);
        b.le<std::uint32_t>(101);
        b.fixed_string("a gnoll", 8);
        s.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::XTargetResponse), b.take()));
        game::Spawn& m = s.conn.world_for_test().spawns[101];
        m.id = 101;
        m.name = "a gnoll";
        m.is_npc = true;
        m.hp_percent = 80;
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('ExtendedTargetWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        auto rows = []() -> ImGuiWindow* {
            for (ImGuiWindow* win : GImGui->Windows)
                if (std::string_view(win->Name).find("/##xtrows") != std::string_view::npos) return win;
            return nullptr;
        };
        ImGuiWindow* area = rows();
        REQUIRE(area != nullptr);
        // Five rows of 30 + 3 pixels at scale 1: the four empty slots are drawn too, and the frame fits them.
        CHECK(area->ContentSize.y >= 5 * 33 - 4);
        CHECK(area->ScrollMax.y == doctest::Approx(0.0f));
        // A frame the player made short: the same rows, scrolling.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.ExtendedTargetWnd', '210,90')") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->ContentSize.y >= 5 * 33 - 4);
        CHECK(area->ScrollMax.y > 0.0f);
    }
    ImGui::DestroyContext(ctx);
}

// The modern extended target window with the most slots (20): six rows tall by default, the rest scrolling; Auto
// expand (hud.expand.ExtendedTargetWnd, the slot menu's row and the frame's) grows it down to the last filled slot.
TEST_CASE("UI packs (modern): the extended target window starts six rows tall and Auto expand grows it to its targets") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_xtarget_expand_test");
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
        // Twenty slots: slot 0 and slot 15 hold a spawn.
        ByteWriter b;
        b.le<std::uint32_t>(20);
        b.le<std::uint32_t>(2);
        for (auto [slot, id, name] : {std::tuple{0u, 101u, std::string("a gnoll")}, std::tuple{15u, 102u, std::string("a pup")}}) {
            b.le<std::uint32_t>(slot);
            b.le<std::uint8_t>(1);
            b.le<std::uint32_t>(id);
            b.fixed_string(name, name.size() + 1);
            game::Spawn& m = s.conn.world_for_test().spawns[id];
            m.id = id;
            m.name = name;
            m.is_npc = true;
            m.hp_percent = 80;
        }
        s.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::XTargetResponse), b.take()));
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.UI.windows.Show('ExtendedTargetWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        auto rows = []() -> ImGuiWindow* {
            for (ImGuiWindow* win : GImGui->Windows)
                if (std::string_view(win->Name).find("/##xtrows") != std::string_view::npos) return win;
            return nullptr;
        };
        ImGuiWindow* area = rows();
        REQUIRE(area != nullptr);
        // Every slot is a row (30 + 3 pixels at scale 1); the frame shows about six of them and scrolls the rest.
        CHECK(area->ContentSize.y >= 20 * 33 - 4);
        CHECK(area->Size.y >= 6 * 33 - 4);
        CHECK(area->Size.y < 7 * 33);
        CHECK(area->ScrollMax.y > 0.0f);
        // Auto expand: tall enough for the rows down to slot 15 (16 rows); the four after it still scroll.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.expand.ExtendedTargetWnd', '1')") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->Size.y >= 16 * 33 - 4);
        CHECK(area->Size.y < 17 * 33);
        CHECK(area->ScrollMax.y > 0.0f);
        // A size of the player's own taller than the targets need is kept.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.ExtendedTargetWnd', '210,800')") == LUA_OK);
        s.frames(3);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->Size.y > 17 * 33);
        // Off again: the player's size, here a short one, and the rows scroll.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.ExtendedTargetWnd', '210,120') "
                                 "ui.SetSetting('hud.expand.ExtendedTargetWnd', '0')") == LUA_OK);
        s.frames(3);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->Size.y < 3 * 33);
        CHECK(area->ScrollMax.y > 0.0f);
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
for _, t in ipairs(hs.WindowList('AAWindow', 'ModernList') or {}) do out[#out + 1] = table.concat(t, '|') end
return table.concat(out, ';'))lua") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) ==
              "Innate Strength|0/3|2||1||0|0;Mystical Shielding|0/3|5|Defense|3|The Planes of Power|1|5");
        lua_settop(L, 0);
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('AAWindow', true)") == LUA_OK);
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
        REQUIRE(luaL_dostring(L, "hs.ListClick('AAWindow', 'ModernList', 1, 'left')") == LUA_OK);
        s.frames(2);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "local _, _, a = hs.ListInfo('AAWindow', 'ModernList') local _, _, b = "
                                 "hs.ListInfo('AAWindow', 'List3') return a, b, (hs.WindowText('AAWindow', "
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
            REQUIRE(luaL_dostring(L, "hs.WindowList('AAWindow', 'ModernList') "
                                     "return (hs.WindowText('AAWindow', 'ModernSelected'))") == LUA_OK);
            std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return out.substr(0, out.find('\t'));
        };
        REQUIRE(luaL_dostring(L, "hs.WindowList('AAWindow', 'ModernList') "
                                 "hs.ListClick('AAWindow', 'ModernList', 0, 'left')") == LUA_OK);
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
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('AAWindow', true)") == LUA_OK);
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
// real pipes): hs.Web* from the window's Lua, and the window drawing the tabs without script errors.
TEST_CASE("UI packs: the Lua Scripts window and hs.Scripts*: list, run, pause, stop, autorun") {
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
            run("local l, info = hs.Scripts(true); assert(#l == 2 and l[1].name == 'loop' and l[2].name == 'other');"
                "assert(not l[1].allowed and l[1].why:find('zone%-in') and l[1].status == '' and l[1].found);"
                "assert(not info.autorun and info.folders[1]:find('scripts'));"
                "local ok, why = hs.ScriptRun('loop'); assert(not ok and why:find('may not run'))");
            run("hs.ShowWindow('ScriptsWnd', true)");
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            policy.set(plugin::Rules{}); // no hailstone.cfg: the player's scripts run
            run("local l = hs.Scripts(true); assert(l[1].allowed and l[1].caps ~= '')");
            // A plugin's code: a script of its own name alone, and no folders.
            fs::create_directories(dir / "pf" / "a");
            fs::create_directories(dir / "pf" / "b");
            std::ofstream(dir / "pf" / "a" / "w.lua") << "x = 1\n";
            std::ofstream(dir / "pf" / "b" / "w.lua") << "x = 1\n";
            s.lua.set_folders({{"loop", dir / "pf" / "a"}, {"plugin:x", dir / "pf" / "b"}});
            CHECK(s.lua.run_for_test("loop", "local l, info = hs.Scripts() return #l .. ' ' .. l[1].name .. ' ' .. "
                                             "#info.folders") == "1 loop 0");
            CHECK(s.lua.run_for_test("plugin:x", "local l, info = hs.Scripts() return #l .. ' ' .. #info.folders") ==
                  "0 0");
            run("local l, info = hs.Scripts(); assert(#l == 2 and #info.folders >= 1)"); // the pack: all, as before
            run("assert(hs.ScriptRun('loop', 'a \\\"b c\\\"'))");
            CHECK(scripts.running("loop"));
            run("local l = hs.Scripts(); assert(l[1].status == 'running' and not l[1].kept)");
            s.frames(2);
            bool drawn = false; // the window is there (not only free of errors)
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                drawn |= win->Active && std::string(win->Name).find("Lua Scripts") != std::string::npos;
            CHECK(drawn);
            run("assert(hs.ScriptPause('loop')); assert(hs.Scripts()[1].status == 'paused')");
            s.frames(2);
            run("assert(hs.ScriptStop('loop')); assert(not hs.ScriptStop('nope'))");
            CHECK_FALSE(scripts.running("loop"));
            // The character's autorun list: refused before a character, kept in its scripts.ini after.
            run("assert(not hs.SetScriptAutorun('other', true))");
            scripts.set_autorun_file(dir / "chars" / "scripts.ini");
            run("local _, info = hs.Scripts(); assert(info.autorun); assert(hs.SetScriptAutorun('other', true));"
                "local l = hs.Scripts(true); assert(l[2].autorun and not l[1].autorun)");
            CHECK(scripts.autorun("other"));
            CHECK(fs::exists(dir / "chars" / "scripts.ini"));
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("hs.ShowWindow('ScriptsWnd', false)");
            s.host.scripts = nullptr;
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the web browser window and hs.Web* with a browser") {
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
            run("assert(hs.WebAvailable())");
            run("local id, why = hs.WebOpen('file:///etc/passwd'); assert(id == nil and why:find('not an http'))");
            run("TAB = hs.WebOpen('example.org', 64, 48); assert(TAB)");
            // A script's tab is not the player's: the window does not list it.
            web::TabOptions theirs;
            theirs.owner = "somescript";
            REQUIRE(browser.open("https://example.org/", theirs) != 0);
            run("assert(#hs.WebTabs() == 1 and hs.WebTabs()[1] == TAB)");
            run("hs.ShowWindow('BrowserWnd', true)");
            const web::Tab* t = browser.tab(1);
            REQUIRE(t);
            for (int i = 0; i < 2000 && t->title.empty(); ++i) {
                browser.poll(0);
                s.frames(1);
            }
            CHECK(t->title == "Mock https://example.org");
            CHECK(t->options.profile == (dir / "web" / "server").string());
            run("local st = hs.WebState(TAB); assert(st.url == 'https://example.org' and not st.closed)");
            run("local ok, why = hs.WebNavigate(TAB, 'file:///etc/passwd'); assert(not ok and why)");
            // Typed text without a scheme gets https:// in front: "javascript:..." is a (bad) web address, no script.
            run("assert(hs.WebNavigate(TAB, 'javascript:alert(1)'))");
            CHECK(t->url == "https://javascript:alert(1)");
            run("assert(hs.WebNavigate(TAB, 'https://example.org/b'))");
            run("hs.WebNav(TAB, 'reload'); hs.WebText(TAB, 'hi')");
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(wants_text == 0); // the page was never clicked: it has not the keyboard
            run("hs.ShowWindow('BrowserWnd', false)");
            // The page's input (hs.WebView): a click reaches the page (the stand-in paints the pixel white) and gives
            // it the keyboard; a click elsewhere takes the keyboard back.
            run("ui.window('ViewTest', function() ui.SetNextWindowPos(0, 0); ui.Begin('ViewTest');"
                " hs.WebView(TAB, 64, 48); VX, VY = ui.GetItemRect(); ui.End() end)");
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
            run("hs.WebClose(TAB); assert(hs.WebState(TAB) == nil)");

            // A plugin's window (its folder's ui/<pack>/): hs.Web* as "plugin:<name>", with the plugin's permissions.
            std::uint32_t plug_caps = plugin::kCapAll & ~plugin::kCapWeb;
            s.host.plugin_caps = [&](std::string_view name) { return name == "plugin:Plug" ? plug_caps : 0u; };
            fs::create_directories(dir / "plug");
            std::ofstream(dir / "plug" / "pw.lua")
                << "ui.window('PlugWeb', function()\n"
                   "  if PW_DONE then return end\n"
                   "  PW_DONE = true\n"
                   "  PW_TAB, PW_WHY = hs.WebOpen('https://example.org/')\n"
                   "  PW_PLAYER_TABS = #hs.WebTabs()\n"
                   "  PW_EXTERNAL = hs.WebOpenExternal('https://example.org/')\n"
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
        REQUIRE(luaL_dostring(L, "hs.SetUiIni('Main', 'GlobalAlpha', '128')") == LUA_OK);
        s.frames(40);
        CHECK(max_alpha(player) <= 128);
        CHECK(max_alpha(player) >= 120);
        // Fades: after the delay (2 s) and the fade (0.5 s) with the mouse away, the Fade-to Transparency.
        REQUIRE(luaL_dostring(L, "hs.SetUiIni('Main', 'GlobalAlpha', '255') hs.SetUiIni('Main', 'GlobalFadeToAlpha', '64') "
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
                                 "hs.ShowWindow('GuildManagementWnd', true)") == LUA_OK);
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
        REQUIRE(luaL_dostring(L, "return (hs.WindowText('GroupWindow', 'Hailstone_Inviter'))") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) == "Bob");
        lua_settop(L, 0);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A server's compiled plugins ask before they load (docs/plugins.md): each pack draws the question in the client's
// place (hs.PluginConsent; "PluginConsent" replaced), and nothing while there is none.
// A window key reaches a pack's window only while the pack claims it (hs.ReplaceWindow, each frame): the achievement
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

TEST_CASE("UI packs: server shroud catalogs and storage draw in both packs") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("ui_pack_shroud_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            test::Rig rig;
            REQUIRE(rig.to_zone());
            Session s(dir, pack);
            s.host.conn = &rig.conn;
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                INFO(code);
                const int result = luaL_dostring(L, code);
                INFO((lua_tostring(L, -1) ? lua_tostring(L, -1) : ""));
                REQUIRE(result == LUA_OK);
                lua_settop(L, 0);
            };
            auto offer = [&](std::uint32_t context) {
                ByteWriter payload;
                payload.le(context);
                payload.zeros(8);
                net::detail::write_cstring(payload, "PROGRESSION\a" "12\a" "Scouts\a" "Scout forms\a"
                    "BRANCH\a" "44\a" "Trackers\a" "Track a target\a" "50\a"
                    "TEMPLATE\a" "Kobold\a" "391\a" "20\a" "1\a"
                    "TEMPLATE\a" "Gnoll\a" "392\a" "25\a" "0");
                rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::ShroudSelectionWindow, std::move(payload)));
            };
            auto drawn = [&](std::string title, std::string_view screen) {
                if (std::string_view(pack) == "modern") title += "###modern." + std::string(screen);
                const auto* window = ImGui::FindWindowByName(title.c_str());
                return window && window->WasActive;
            };
            s.frames(2);
            CHECK_FALSE(s.api.shown("ProgressionSelectionWnd"));
            offer(0);
            s.frames(2);
            CHECK(s.api.shown("ProgressionSelectionWnd"));
            CHECK(s.api.replaces("ProgressionSelectionWnd"));
            CHECK(drawn("Spirit Shroud Selection", "ProgressionSelectionWnd"));
            run(R"lua(
                local screen = "ProgressionSelectionWnd"
                local windows = hs.UI.windows
                assert(windows.ListInfo(screen, "Forms") == 2)
                assert(windows.ListRow(screen, "Forms", 0)[2] == "Kobold")
                assert(windows.ListClick(screen, "Forms", 1, "left"))
                assert(windows.ChildState(screen, "Accept") == false)
                windows.Show(screen, false)
            )lua");
            CHECK_FALSE(s.api.shown("ProgressionSelectionWnd"));

            game::Task mission;
            mission.task_id = 926;
            mission.title = "Scout the ruins";
            mission.type = game::kTaskTypeShared;
            rig.conn.receive_zone_packet_for_test(net::encode_task_description(mission));
            offer(mission.task_id);
            s.frames(2);
            CHECK(s.api.shown("TaskTemplateSelectWnd"));
            CHECK(s.api.replaces("TaskTemplateSelectWnd"));
            CHECK(drawn("Monster Mission Selection", "TaskTemplateSelectWnd"));
            run(R"lua(hs.UI.windows.Show("TaskTemplateSelectWnd", false))lua");
            CHECK_FALSE(s.api.shown("TaskTemplateSelectWnd"));

            ByteWriter roles;
            roles.zeros(8);
            roles.le(mission.task_id);
            roles.zeros(5);
            roles.le<std::uint32_t>(1);
            roles.le<std::uint32_t>(913);
            roles.le<std::int32_t>(1);
            roles.le<std::int32_t>(1);
            roles.zeros(4);
            roles.le<std::uint8_t>(1);
            net::detail::write_cstring(roles, "Watch Captain");
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::MonsterMissionCatalog, std::move(roles)));
            s.frames(2);
            CHECK(drawn("Monster Mission Selection", "TaskTemplateSelectWnd"));
            run(R"lua(
                local screen = "TaskTemplateSelectWnd"
                local windows = hs.UI.windows
                local role = windows.ListRow(screen, "Forms", 0)
                assert(role[1] == "Required" and role[2] == "Watch Captain" and role[5] == "0 / 1")
                assert(windows.ListClick(screen, "Forms", 0, "left"))
                assert(windows.ChildState(screen, "Accept"))
                assert(windows.ClickChild(screen, "Accept", "left"))
                assert(windows.ChildState(screen, "Accept") == false)
            )lua");
            ByteWriter refused;
            refused.zeros(8);
            refused.le<std::uint32_t>(913);
            refused.le<std::uint32_t>(1);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::MonsterMissionResult, std::move(refused)));
            run(R"lua(assert(hs.UI.windows.ChildState("TaskTemplateSelectWnd", "Accept")))lua");
            rig.conn.receive_zone_packet_for_test(net::encode(std::vector<net::SharedTaskMember>{{"Helper", true, 0}}));
            ByteWriter assignment;
            assignment.zeros(8);
            assignment.fixed_string("Helper", 64);
            assignment.le<std::uint32_t>(913);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::MonsterMissionAssignment, std::move(assignment)));
            s.frames(2);
            run(R"lua(
                local screen = "TaskTemplateSelectWnd"
                local windows = hs.UI.windows
                local role = windows.ListRow(screen, "Forms", 0)
                assert(role[4] == "Full" and role[5] == "1 / 1")
                assert(windows.ChildState(screen, "Accept") == false)
                windows.Show(screen, false)
            )lua");

            ByteWriter progress;
            progress.le<std::uint32_t>(1);
            constexpr std::string_view progress_text = "PROGRESSION\a" "12\a" "Scouts\a" "Scout forms\a"
                "1\a" "2\a" "Gnoll\a" "Gnoll form\a" "50";
            progress.bytes(std::as_bytes(std::span(progress_text.data(), progress_text.size())));
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::ShroudProgress, std::move(progress)));
            run(R"lua(
                local windows = hs.UI.windows
                assert(windows.ChildShown("InventoryWindow", "ShroudProgressButton") == true)
                assert(windows.ClickChild("InventoryWindow", "ShroudProgressButton", "left"))
            )lua");
            s.frames(2);
            CHECK(drawn("Spirit Shroud Progress", "ShroudProgressWnd"));
            run(R"lua(hs.UI.windows.Show("ShroudProgressWnd", false))lua");

            REQUIRE(rig.conn.profile());
            net::Spawn banker;
            banker.spawn_id = 133;
            banker.name = "Shroud Banker";
            banker.npc = 1;
            banker.class_ = 40;
            banker.motion.x = rig.conn.profile()->x;
            banker.motion.y = rig.conn.profile()->y;
            banker.motion.z = rig.conn.profile()->z;
            rig.conn.receive_zone_packet_for_test(net::encode(banker));
            ByteWriter open;
            open.le<std::uint32_t>(0);
            open.le(banker.spawn_id);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::AltStorage, std::move(open)));
            ByteWriter items;
            items.le<std::uint32_t>(3600);
            items.le<std::uint32_t>(1);
            net::detail::write_cstring(items, "Tracker's Ledger");
            items.le<std::uint32_t>(431);
            items.le<std::uint64_t>(937);
            items.le<std::uint32_t>(2);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::AltStorageList, std::move(items)));
            s.frames(2);
            CHECK(s.api.shown("AltStorageWnd"));
            CHECK(s.api.replaces("AltStorageWnd"));
            CHECK(drawn("Shroud Bank", "AltStorageWnd"));
            run(R"lua(
                local windows = hs.UI.windows
                local screen = "AltStorageWnd"
                assert(windows.ListRow(screen, "ItemList", 0)[1] == "Tracker's Ledger")
                assert(windows.Text(screen, "TimerLabel") == "01:00:00")
                assert(windows.ChildState(screen, "GetButton") == false)
            )lua");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the server plugins' question draws above the loading cover in each pack") {
    // In the world, and on the loading screen, where a required plugin's question holds the zone-in (no game or
    // character UI is ready there).
    for (const bool in_game : {true, false})
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        CAPTURE(in_game);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_consent_") + pack + (in_game ? "_game" : "_load"));
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack, {}, in_game);
            std::optional<plugin::ConsentPrompt> prompt;
            s.host.plugin_consent = [&] { return prompt; };
            s.host.answer_plugin_consent = [&](std::optional<bool>) { prompt.reset(); };
            ui::LoadingView loading;
            loading.title = "Checking required add-ons";
            loading.detail = "Review the required plugin's permission request to continue.";
            auto consent_window = [&]() -> ImGuiWindow* {
                for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                    if (win->WasActive && (std::string(win->Name).find("###modern.PluginConsent") != std::string::npos ||
                                           std::string(win->Name) == "Server Plugins"))
                        return win;
                return nullptr;
            };
            auto draw_index = [](const ImDrawList* list) {
                const ImDrawData* data = ImGui::GetDrawData();
                for (int i = 0; i < data->CmdListsCount; ++i)
                    if (data->CmdLists[i] == list) return i;
                return -1;
            };
            auto foreground_cover = [&] {
                const ImDrawList* foreground = ImGui::GetForegroundDrawList();
                CHECK_FALSE(foreground->VtxBuffer.empty());
                CHECK(draw_index(foreground) == ImGui::GetDrawData()->CmdListsCount - 1);
                CHECK(ImGui::GetBackgroundDrawList()->VtxBuffer.empty());
            };
            s.frames(2, nullptr, &loading);
            CHECK(s.api.replaces("PluginConsent")); // the client's own prompt stays off
            CHECK(consent_window() == nullptr);
            foreground_cover();
            plugin::ConsentPrompt p;
            p.server = "Alpha";
            p.serial = 1;
            p.files.push_back({"MQ2Thing", "MQ2Thing.so", 2u * 1024 * 1024, {}});
            p.files.push_back({"MQ2Other", "MQ2Other.so", 4096, {}});
            prompt = p;
            s.frames(3, nullptr, &loading);
            ImGuiWindow* consent = consent_window();
            REQUIRE(consent);
            // The loading screen is a window over every other one (in the world: the pack's windows, drawn before it),
            // beneath the question alone; nothing goes in the foreground list over the question.
            ImGuiWindow* cover = ImGui::FindWindowByName(ui::kLoadingCoverWindow);
            REQUIRE(cover);
            CHECK_FALSE(cover->DrawList->VtxBuffer.empty());
            CHECK(draw_index(consent->DrawList) > draw_index(cover->DrawList));
            int beneath = 0;
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows) {
                if (win == cover || win->RootWindow == consent || !win->WasActive || win->DrawList->VtxBuffer.empty() ||
                    draw_index(win->DrawList) < 0)
                    continue;
                CAPTURE(win->Name);
                CHECK(draw_index(win->DrawList) < draw_index(cover->DrawList));
                ++beneath;
            }
            if (in_game) CHECK(beneath > 0); // (the pack draws its game windows; the check above has something to do)
            CHECK(ImGui::GetForegroundDrawList()->VtxBuffer.empty());
            // The mouse anywhere but the question is over the cover, not the windows beneath it.
            ImGui::GetIO().AddMousePosEvent(consent->Pos.x > 40.0f ? 5.0f : 1595.0f, 895.0f);
            s.frames(1, nullptr, &loading);
            CHECK(ImGui::GetCurrentContext()->HoveredWindow == cover);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            prompt.reset();
            s.frames(2, nullptr, &loading);
            CHECK(consent_window() == nullptr);
            foreground_cover();
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// A required add-on that failed in the world (docs/plugins.md "Opt-in add-on policies"): each pack draws the Server
// add-ons warning in the client's place ("AddonPolicy" replaced) only while there is one, with a close box, and its
// Retry calls the client's with that failure's serial; no Retry where the failure cannot be retried.
TEST_CASE("UI packs: the Server add-ons warning shows only for a failure, can be closed, and Retry reaches the client") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_addon_policy_") + pack);
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
            ui::AddonPolicyView view;
            int retries = 0;
            s.host.addon_policy = &view;
            s.host.retry_required_addons = [&] { ++retries; };
            auto window = [&]() -> ImGuiWindow* {
                for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                    if (win->WasActive && (std::string(win->Name).find("###modern.AddonPolicy") != std::string::npos ||
                                           std::string(win->Name) == "Server add-ons"))
                        return win;
                return nullptr;
            };
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"lua(
local button, invisible = ui.Button, ui.InvisibleButton
retryRect = nil
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Retry##addonpolicy" then retryRect = { ui.GetItemRect() } end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##addonpolicyretry" then retryRect = { ui.GetItemRect() } end
  if id == "##close" then closeSeen = true end -- (the gamepad pack's own close button)
  return hit
end
)lua") == LUA_OK);
            s.frames(2);
            CHECK(s.api.replaces("AddonPolicy")); // the client's own window stays off
            CHECK(window() == nullptr);           // nothing failed: nothing shown

            view.failed = view.can_retry = true;
            view.serial = 1;
            view.message = "Required add-on owner/thing failed with an error in a command";
            s.frames(3);
            ImGuiWindow* shown = window();
            REQUIRE(shown != nullptr);
            lua_getglobal(L, "closeSeen");
            CHECK((shown->HasCloseButton || lua_toboolean(L, -1))); // the player can put it away
            lua_settop(L, 0);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            lua_getglobal(L, "retryRect");
            REQUIRE(lua_istable(L, -1));
            float rect[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            CHECK(retries == 1);

            // A failure Retry cannot help with: shown, without Retry.
            view.can_retry = false;
            view.serial = 2;
            luaL_dostring(L, "retryRect = nil");
            s.frames(3);
            CHECK(window() != nullptr);
            lua_getglobal(L, "retryRect");
            CHECK(lua_isnil(L, -1));
            lua_settop(L, 0);

            view.failed = false;
            view.serial = 3;
            s.frames(3);
            CHECK(window() == nullptr);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// A click meant for something else the instant the question pops up must not answer it: both buttons wait a
// second after the question shows. The real loading cover must leave both answers reachable afterward.
TEST_CASE("UI packs: both plugin answers arm and remain reachable above the loading cover") {
    for (const bool in_game : {true, false})
    for (const bool allow : {true, false})
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        CAPTURE(in_game);
        CAPTURE(allow);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_consent_arm_") + pack +
                                               (in_game ? "_game" : "_load") + (allow ? "_allow" : "_deny"));
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            Session s(dir, pack, {}, in_game);
            ui::LoadingView loading;
            loading.title = "Checking required add-ons";
            loading.detail = "Review the required plugin's permission request to continue.";
            std::optional<plugin::ConsentPrompt> prompt;
            std::vector<std::optional<bool>> answers;
            s.host.plugin_consent = [&] { return prompt; };
            s.host.answer_plugin_consent = [&](std::optional<bool> a) {
                answers.push_back(a);
                prompt.reset();
            };
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"lua(
local button, invisible = ui.Button, ui.InvisibleButton
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Allow##consent" then allowRect = { ui.GetItemRect() } end
  if label == "Don't allow##consent" then denyRect = { ui.GetItemRect() } end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##consentallow" then allowRect = { ui.GetItemRect() } end
  if id == "##consentdeny" then denyRect = { ui.GetItemRect() } end
  return hit
end
)lua") == LUA_OK);
            auto click_answer = [&] {
                lua_getglobal(L, allow ? "allowRect" : "denyRect");
                REQUIRE(lua_istable(L, -1));
                float rect[4]{};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    rect[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_settop(L, 0);
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
                s.frames(1, nullptr, &loading);
                io.AddMouseButtonEvent(0, true);
                s.frames(1, nullptr, &loading);
                io.AddMouseButtonEvent(0, false);
                s.frames(1, nullptr, &loading);
            };
            plugin::ConsentPrompt p;
            p.server = "Alpha";
            p.serial = 4;
            p.files.push_back({"MQ2Thing", "MQ2Thing.so", 4096, {}});
            prompt = p;
            s.frames(3, nullptr, &loading);
            click_answer(); // within its first second: nothing
            CHECK(answers.empty());
            CHECK(prompt.has_value());
            s.frames(65, nullptr, &loading);
            click_answer();
            REQUIRE(answers.size() == 1);
            CHECK(answers[0] == std::optional<bool>(allow));
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
                                 "'40,40') hs.ShowWindow('AAWindow', true)") == LUA_OK);
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
        run("ui.SetSetting('test.pos.AAWindow', '40,40') hs.ShowWindow('AAWindow', true)");
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
        CHECK(run("return tostring(hs.WindowShown('AAWindow'))") == "true"); // (rolled up, not closed)
        // closed and opened again: still rolled up
        run("hs.ShowWindow('AAWindow', false)");
        s.frames(2);
        run("hs.ShowWindow('AAWindow', true)");
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

namespace {
// Runs `script` in the gamepad pack (its lib/ files reachable with dofile) inside a window; it returns "ok".
std::string run_gamepad_lib_script(const char* script, const char* scratch_name) {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch(scratch_name);
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    std::string out;
    {
        Session s(dir, "gamepad");
        s.frames(1);
        ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
        s.api.begin_frame();
        ImGui::NewFrame();
        lua_State* L = s.lua.state();
        const int rc = luaL_dostring(L, script);
        out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        if (rc != LUA_OK) out = "error: " + out;
        lua_settop(L, 0);
        ImGui::Render();
    }
    ImGui::DestroyContext(ctx);
    return out;
}
} // namespace

// The gamepad pack's text edits (lib/theme.lua): Delete takes the last character, a length limit and a shortened
// title cut, and a text shortened from its start begins, only between UTF-8 characters; a byte that is not UTF-8
// (Latin-1) counts as one.
TEST_CASE("UI packs (gamepad): text edits never split a UTF-8 character") {
    constexpr const char* kScript = R"lua(
local T = dofile("lib/theme.lua")
local function check(ok, what) if not ok then error(what, 2) end end
-- Delete
check(T.dropChar("") == "", "empty")
check(T.dropChar("ab") == "a", "ascii")
check(T.dropChar("a\u{E9}") == "a", "two bytes")
check(T.dropChar("a\u{20AC}") == "a", "three bytes")
check(T.dropChar("a\u{1F600}") == "a", "four bytes")
check(T.dropChar("\u{E9}") == "", "only one")
check(T.dropChar("a\xA9") == "a", "latin-1 byte")
check(T.dropChar("\xA9\xA9") == "\xA9", "stray continuation bytes, one at a time")
-- a limit in bytes, cut between characters
check(T.cutBytes("abc", 5) == "abc" and T.cutBytes("abc", 2) == "ab" and T.cutBytes("abc", 0) == "", "ascii cut")
check(T.cutBytes("a\u{20AC}b", 1) == "a", "before")
check(T.cutBytes("a\u{20AC}b", 2) == "a", "inside (1)")
check(T.cutBytes("a\u{20AC}b", 3) == "a", "inside (2)")
check(T.cutBytes("a\u{20AC}b", 4) == "a\u{20AC}", "after")
check(T.cutBytes("\u{E9}\u{E9}", 3) == "\u{E9}", "two-byte")
check(T.cutBytes("ab\xA9\xA9\xA9\xA9c", 4) == "ab\xA9\xA9", "not UTF-8: by bytes")
-- every cut of a mixed text is a whole-character start of it
local text = "\u{C9}l\u{E9}onore \u{1F600} d\u{2019}Ambr\u{E9}e"
for n = 0, #text + 1 do
  local c = T.cutBytes(text, n)
  check(#c <= n and text:sub(1, #c) == c and utf8.len(c) ~= nil, "cut " .. n)
  check(n > #text or #c > n - 4, "cut too short " .. n)
end
-- a start moved on to a character
check(T.charStart("a\u{20AC}b", 1) == 1 and T.charStart("a\u{20AC}b", 2) == 2, "on a start")
check(T.charStart("a\u{20AC}b", 3) == 5 and T.charStart("a\u{20AC}b", 4) == 5, "inside")
check(T.charStart("a\u{E9}", 3) == 4, "to the end")
check(T.charStart("a\xA9\xA9\xA9\xA9", 2) == 2, "not UTF-8: as it is")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_utf8_test") == "ok");
}

// A click deferred until A is released (lib/window.lua W.deferred, used by lib/exchange.lua and lib/markets.lua)
// runs while the window it was asked in is still drawn, waits while A is held, and is dropped once that window is gone
// rather than firing into whichever window draws next.
TEST_CASE("UI packs (gamepad): a click deferred to A's release is dropped when its window closes") {
    constexpr const char* kScript = R"lua(
local W = dofile("lib/window.lua")
local function check(ok, what) if not ok then error(what, 2) end end
local held = false
local real = hs.UI.gamepad.Down
hs.UI.gamepad.Down = function(b) return b == "a" and held end
local frame = ui.GetFrameCount()
local ran = 0
local slot = {}
local function ask(owner)
  GPSTATE.currentWindow = owner
  W.deferred(slot, function() ran = ran + 1 end)
  GPSTATE.currentWindow = nil
end
-- its window drawn last frame: waits for A, then runs once
ask("Owner")
GPSTATE.drawn.Owner = frame - 1
held = true
W.runDeferred(slot)
check(ran == 0 and slot.fn ~= nil, "ran with A held")
held = false
W.runDeferred(slot)
check(ran == 1 and slot.fn == nil, "did not run on release")
W.runDeferred(slot)
check(ran == 1, "ran twice")
-- its window gone (not drawn for a few frames): dropped, even once A is up
ask("Owner")
GPSTATE.drawn.Owner = frame - 5
W.runDeferred(slot)
check(ran == 1 and slot.fn == nil, "a closed window's click fired")
-- asked outside a window: as before, runs on release
ask(nil)
W.runDeferred(slot)
check(ran == 2, "a click without a window did not run")
hs.UI.gamepad.Down = real
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_deferred_test") == "ok");
}

// A pad button a window reads itself (T.settledPress: the Quantity, colour, appearance and icon dialogs' A) is
// dropped while the pack's settling time after a window gains the focus runs, as T.settle drops a button's press.
TEST_CASE("UI packs (gamepad): a dialog's own A waits out the settling time") {
    constexpr const char* kScript = R"lua(
local T = dofile("lib/theme.lua")
local function check(ok, what) if not ok then error(what, 2) end end
local down = true
local fake = { menuPressed = function(b) return b == "a" and down end }
GPSTATE.padSettleUntil = ui.GetTime() + 1
check(not T.settledPress(fake, "a"), "pressed while settling")
GPSTATE.padSettleUntil = ui.GetTime() - 1
check(T.settledPress(fake, "a"), "not pressed once settled")
down = false
check(not T.settledPress(fake, "a"), "pressed without a press")
GPSTATE.padSettleUntil = nil
down = true
check(T.settledPress(fake, "a"), "no settling time yet")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_settle_test") == "ok");
}

// The gamepad STML parser (lib/stml.lua): a "<" that opens no tag is text, shown with what follows it, in the colour
// it is in; a closed tag is still dropped.
TEST_CASE("UI packs (gamepad): STML keeps the text after an unmatched <") {
    constexpr const char* kScript = R"lua(
local S = dofile("lib/stml.lua")
local function check(ok, what) if not ok then error(what, 2) end end
check(S.plain(S.parse("a < b")) == "a < b", "a < b: " .. S.plain(S.parse("a < b")))
check(S.plain(S.parse("<3")) == "<3", "<3")
check(S.plain(S.parse("x <")) == "x <", "a < at the end")
local segs = S.parse('<c "#ff0000">love &amp; <3', { 0, 0, 0, 1 })
check(S.plain(segs) == "love & <3", "entities and the rest: " .. S.plain(segs))
local last = segs[#segs]
check(last[1] == "<3" and last[2][1] == 1 and last[2][2] == 0, "the rest takes the colour it is in")
check(S.plain(S.parse("one <b>two</b> three")) == "one two three", "closed tags dropped")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_stml_lt_test") == "ok");
}

// The gamepad bag grids' cell ids (lib/items.lua I.cellId) are the strings they were, made once.
TEST_CASE("UI packs (gamepad): bag grid cell ids are kept") {
    constexpr const char* kScript = R"lua(
local I = dofile("lib/items.lua")
local function check(ok, what) if not ok then error(what, 2) end end
check(I.cellId("sell", 23) == "##sell23_-1", "a bag slot")
check(I.cellId("sell", 25, 3) == "##sell25_3", "a bag's slot")
check(I.cellId("inv", 25, 3) == "##inv25_3", "another grid")
check(rawequal(I.cellId("sell", 25, 3), I.cellId("sell", 25, 3)), "kept")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_cellid_test") == "ok");
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
            local ListInfo, ListRow, WindowText, WhoResults = hs.ListInfo, hs.ListRow, hs.WindowText, hs.WhoResults
            hs.ListInfo = function(win, child, ...)
              if win == "RaidWindow" then
                if child == "RAID_PlayerList" then return #fake.members, 6, -1 end
                return 0, 0, -1
              end
              if win == "GuildManagementWnd" and child == "MemberList" then return 0, 0, -1 end
              return ListInfo(win, child, ...)
            end
            hs.ListRow = function(win, child, row, ...)
              if win == "RaidWindow" then
                return child == "RAID_PlayerList" and fake.members[row + 1] or nil
              end
              return ListRow(win, child, row, ...)
            end
            hs.WindowText = function(win, child, ...)
              if win == "RaidWindow" and child == "MOTDViewer" then return fake.raidMotd end
              if win == "GuildManagementWnd" and child == "MOTDViewer" then return fake.guildMotd end
              if win == "GuildManagementWnd" and child == "GuildName" then return "The Guild" end
              if win == "ItemDisplayWindow" and child == "ItemDescription" then return fake.desc end
              return WindowText(win, child, ...)
            end
            hs.WhoResults = function() return fake.who, fake.players end
            -- (the stock guild and item windows show only in a guild / for an item: shown here on asking)
            fake.shown = {}
            local ReplaceWindow, WindowShown = hs.ReplaceWindow, hs.WindowShown
            hs.ReplaceWindow = function(win, ...)
              local r = ReplaceWindow(win, ...)
              if fake.shown[win] ~= nil then return fake.shown[win] end
              return r
            end
            hs.WindowShown = function(win)
              local r = WindowShown(win)
              if fake.shown[win] ~= nil then return fake.shown[win] end
              return r
            end
        )lua");

        // the raid: its message of the day at once, a new member at once (the row count), a renamed one within 0.25 s
        run("hs.ShowWindow('RaidWindow', true) seen = {}");
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
        run("hs.ShowWindow('RaidWindow', false)");

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

TEST_CASE("UI packs (gamepad): Final Result stays reachable and dispatches only an enabled explicit click") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir scratch("ui_pack_evolving_final_test");
    fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        Session s(dir, "gamepad");
        s.frames(2);
        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            INFO(code);
            const int result = luaL_dostring(L, code);
            INFO((lua_tostring(L, -1) ? lua_tostring(L, -1) : ""));
            REQUIRE(result == LUA_OK);
            lua_settop(L, 0);
        };
        run(R"lua(
            finalTest = { shown = true, enabled = true, click = false, calls = 0, drawn = 0 }
            local WindowShown, ChildShown = hs.WindowShown, hs.ChildShown
            local WindowText, ChildState, ClickChild = hs.WindowText, hs.ChildState, hs.ClickChild
            hs.WindowShown = function(win, ...)
              if win == "ItemDisplayWindow" then return true end
              return WindowShown(win, ...)
            end
            hs.ChildShown = function(win, child, ...)
              if win == "ItemDisplayWindow" and child == "Hailstone_EvolvingFinalResult" then
                return finalTest.shown
              end
              return ChildShown(win, child, ...)
            end
            hs.ChildState = function(win, child, ...)
              if win == "ItemDisplayWindow" and child == "Hailstone_EvolvingFinalResult" then
                return finalTest.enabled
              end
              return ChildState(win, child, ...)
            end
            hs.WindowText = function(win, child, ...)
              if win == "ItemDisplayWindow" then
                if child == "Hailstone_EvolvingFinalResult" then return "Final Result" end
                if child == "ItemDescription" then return ("A very long item description. "):rep(500) end
              end
              return WindowText(win, child, ...)
            end
            hs.ClickChild = function(win, child, button, ...)
              if win == "ItemDisplayWindow" and child == "Hailstone_EvolvingFinalResult" then
                assert(button == "left")
                finalTest.calls = finalTest.calls + 1
                return true
              end
              return ClickChild(win, child, button, ...)
            end
            local InvisibleButton = ui.InvisibleButton
            ui.InvisibleButton = function(id, w, h, flags, ...)
              if id == "##evolvingfinal" then
                finalTest.drawn = finalTest.drawn + 1
                assert((flags & ui.ButtonFlags.EnableNav) ~= 0, "Final Result must allow pad focus")
                local x, y = ui.GetCursorScreenPos()
                local wx, wy = ui.GetWindowPos()
                local ww, wh = ui.GetWindowSize()
                assert(x >= wx and y >= wy and x + w <= wx + ww and y + h <= wy + wh,
                  "Final Result must remain inside the item window")
                InvisibleButton(id, w, h, flags, ...)
                return finalTest.click
              end
              return InvisibleButton(id, w, h, flags, ...)
            end
        )lua");
        s.frames(2);
        run("assert(finalTest.drawn == 2 and finalTest.calls == 0)");
        run("finalTest.click = true");
        s.frames(1);
        run("assert(finalTest.calls == 1)");
        run("finalTest.enabled = false");
        s.frames(1);
        run("assert(finalTest.calls == 1) finalTest.shown = false finalTest.drawn = 0");
        s.frames(1);
        run("assert(finalTest.calls == 1 and finalTest.drawn == 0)");
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
                  local st = hs.WindowText("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Stat") or ""
                  local v = (hs.WindowText("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Value") or "")
                    :match("^%s*(.-)%s*$")
                  if hs.ChildShown("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Stat") ~= false
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
        std::ofstream(dir / "game" / "eqstr_us.txt") << "3029 (Combat)\n3036 Effect: %1 %2\n";
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
            helm.effects[net::kEffectProc].effect = -1;
            helm.effects[net::kEffectProc].name = "Frost & Fire";
            std::vector<net::Item> displays{helm};
            std::vector<std::uint64_t> ids{1};
            s.host.item_displays = &displays;
            s.host.item_display_ids = &ids;
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"(
                test_seen = {}
                local dt, tc, tw = ui.DrawText, ui.TextColored, ui.TextWrapped
                ui.DrawText = function(...) local a = { ... } test_seen[tostring(a[#a])] = true return dt(...) end
                ui.TextColored = function(...) local a = { ... } test_seen[tostring(a[5])] = true return tc(...) end
                ui.TextWrapped = function(t, ...) test_seen[tostring(t)] = true return tw(t, ...) end)") ==
                    LUA_OK);
            auto seen = [&](const std::string& text) {
                REQUIRE(luaL_dostring(L, ("return test_seen['" + text + "'] == true").c_str()) == LUA_OK);
                bool b = lua_toboolean(L, -1);
                lua_settop(L, 0);
                return b;
            };
            s.frames(3);
            // The HP change for you beside the item's HP in the stat grid, not as a line of its own below it.
            const std::string heading = pack == std::string("modern") ? "vs your Old Helm (Head)"
                                                                      : "Compared with your Old Helm (Head)";
            CHECK(seen(heading));
            CHECK(seen("Upgrade"));
            CHECK(seen("60"));
            CHECK(seen("+50"));
            CHECK(seen("Gains Proc: Frost & Fire"));
            CHECK(seen("Effect: Frost & Fire (Combat)"));
            CHECK_FALSE(seen("+50 HP"));
            CHECK_FALSE(seen("+50 HP (" + std::to_string(ch.max_hp(true)) + " to " + std::to_string(ch.max_hp(true) + 50) + ")"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

            if (pack == std::string("modern")) {
                // The tooltip: the verdict under the name, the HP change beside the item's HP, the rest as "+n NAME".
                REQUIRE(luaL_dostring(L, R"(
                    test_seen = {}
                    local items = dofile("lib/items.lua")
                    local c = { usable = true, whyNot = "", notes = {}, slots = { { slot = 2, slotName = "Head",
                      current = { "Old Helm" }, verdict = "upgrade", score = 60, gained = {}, lost = {},
                      changes = { { key = "hp", name = "HP", before = 100, after = 150, delta = 50, weight = 1 },
                                  { key = "str", name = "STR", before = 80, after = 85, delta = 5, weight = 0.5 } } } } }
                    local dt, tx, td = ui.DrawText, ui.Text, ui.TextDisabled
                    ui.Text = function(t, ...) test_seen[tostring(t)] = true return tx(t, ...) end
                    ui.TextDisabled = function(t, ...) test_seen[tostring(t)] = true return td(t, ...) end
                    ui.window("Compare tooltip test", function()
                      items.Tooltip({ name = "Iron Helm", hp = 60, weight = 3 }, "Head", c)
                    end))") == LUA_OK);
                s.frames(2);
                CHECK(seen("Upgrade"));
                CHECK(seen("HP 60"));
                CHECK(seen("+50"));
                CHECK(seen("+5 STR"));
                CHECK_FALSE(seen("+50 HP"));
                CHECK(seen("vs your Old Helm (Head)"));
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                REQUIRE(luaL_dostring(L, "ui.window('Compare tooltip test', function() end)") == LUA_OK);
            }

            option = 0;
            REQUIRE(luaL_dostring(L, "test_seen = {}") == LUA_OK);
            s.frames(2);
            CHECK_FALSE(seen(heading));
            CHECK_FALSE(seen("+50"));
            CHECK_FALSE(seen("Gains Proc: Frost & Fire"));
            CHECK(seen("Effect: Frost & Fire (Combat)"));
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
            REQUIRE(luaL_dostring(L, (std::string("hs.ShowWindow('") + screen + "', true)").c_str()) == LUA_OK);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            REQUIRE(luaL_dostring(L, (std::string("hs.ShowWindow('") + screen + "', false)").c_str()) == LUA_OK);
        }
        // the map's World view (the atlas and the zone finder, lib/atlas.lua) over the small atlas above, then a search
        // and a route followed (the banner over the Zone view)
        // (a window's stored title changes only in some cases, so the World view is told by the zone finder's list)
        auto finder_drawn = [] {
            for (ImGuiWindow* win : GImGui->Windows)
                if (std::string_view(win->Name).find("##zonelist") != std::string_view::npos && win->Active) return true;
            return false;
        };
        REQUIRE(luaL_dostring(L, "ui.SetSetting('map.view', 'world') hs.ShowWindow('MapViewWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(finder_drawn());
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('MapViewWnd', false)") == LUA_OK);
        s.frames(1);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('atlas.route', 'qeytoqrg') ui.SetSetting('map.view', 'zone')"
                                 " hs.ShowWindow('MapViewWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK_FALSE(finder_drawn()); // the Zone view
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('MapViewWnd', false) ui.SetSetting('atlas.route', '') ui.SetSetting('map.view', '')") == LUA_OK);
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

TEST_CASE("UI packs: both guild banner pages draw the catalog and appearance editor") {
    const auto ui_source = fs::path(__FILE__).parent_path().parent_path() / "ui";
    for (const std::string pack : {"modern", "gamepad"}) {
        INFO(pack);
        hailstone::test::ScratchDir scratch("ui_pack_guild_banners");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(ui_source / pack, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            Session s(dir, pack);
            s.frames(2);
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"lua(
                local g = 'GuildManagementWnd'
                local shown, text, info, row = hs.ChildShown, hs.WindowText, hs.ListInfo, hs.ListRow
                fixture = { editing = false, lists = 0, layers = 0 }
                hs.ChildShown = function(win, child, ...)
                    if win == g and child == 'SaveBannerSettings' then return fixture.editing end
                    return shown(win, child, ...)
                end
                hs.WindowText = function(win, child, ...)
                    if win == g then
                        if child == 'GuildName' then return 'Fixture guild' end
                        if child == 'BannerDescription' then return 'Fixture description' end
                        if child == 'BannerBackgroundPattern' or child == 'BannerEmblemPattern' then
                            fixture.layers = fixture.layers + 1
                            return '1'
                        end
                    end
                    return text(win, child, ...)
                end
                hs.ListInfo = function(win, child, ...)
                    if win == g and child == 'BannerKitList' then
                        fixture.lists = fixture.lists + 1
                        return 1, 2, 0, {'', 'Banner'}, {0, 400}, false
                    end
                    return info(win, child, ...)
                end
                hs.ListRow = function(win, child, ...)
                    if win == g and child == 'BannerKitList' then return {'', 'Fixture kit'}, 1, 1, 1 end
                    return row(win, child, ...)
                end
                hs.SetTabPage(g, 'Subwindows', 4)
                hs.ShowWindow(g, true)
            )lua") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            REQUIRE(luaL_dostring(L, "assert(fixture.lists > 0); fixture.editing = true") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(luaL_dostring(L, "assert(fixture.layers > 0)") == LUA_OK);
        }
        ImGui::DestroyContext(ctx);
    }
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
            hs.ShowWindow('RaidWindow', true)
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

TEST_CASE("UI packs: raid member context menus and leadership hotkeys are reachable through drawn controls") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool modern = std::string_view(pack) == "modern";
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir scratch("ui_pack_raid_leadership_test");
        fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        std::ofstream(dir / "UI_Test_Server.ini") << "[Tips]\nShowTips=0\nShowTOTD=0\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            Session s(dir, pack);
            Config gamepad_config;
            app::Gamepad gamepad(gamepad_config, {});
            s.host.gamepad = &gamepad;
            auto hot_buttons = std::make_unique<game::HotButtonStore>();
            s.host.hot_buttons = hot_buttons.get();
            s.host.set_hot_button = [&](int bar, int button, const game::HotButton& value) {
                hot_buttons->current(bar, button) = value;
            };
            hailstone::test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            const std::string me = conn.character_name();
            net::RaidUpdate raid;
            raid.general.action = net::kRaidAddMember;
            raid.general.leader_name = me;
            raid.class_ = 1;
            raid.level = 60;
            for (int i = 0; i < 18; ++i) {
                raid.general.player_name = i ? "Member" + std::to_string(i) : me;
                raid.general.parameter = static_cast<std::uint32_t>(i / 6);
                conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            }
            conn.set_raid_target("Member1");
            lua_State* L = s.lua.state();
            auto run = [&](const std::string& code) {
                INFO(code);
                REQUIRE(luaL_dostring(L, code.c_str()) == LUA_OK);
            };
            // Record the real controls' rectangles; clicks below are mouse input, never direct ClickChild calls.
            run(R"lua(
                controls, memberPoints = {}, {}
                local button, invisible, menuItem, drawText = ui.Button, ui.InvisibleButton, ui.MenuItem, ui.DrawText
                local function record(id)
                  controls[id] = { ui.GetItemRect() }
                end
                ui.Button = function(id, ...)
                  local clicked = button(id, ...)
                  record(id)
                  return clicked
                end
                ui.InvisibleButton = function(id, ...)
                  local clicked = invisible(id, ...)
                  record(id)
                  return clicked
                end
                ui.MenuItem = function(label, ...)
                  local clicked = menuItem(label, ...)
                  controls['menu.' .. label] = { ui.GetItemRect() }
                  return clicked
                end
                ui.DrawText = function(...)
                  local x, y, text = select(1, ...), select(2, ...), select(7, ...)
                  if text == 'Member2' then memberPoints[text] = {x + 3, y + 3} end
                  if text == 'Main Assist' or text == 'Mark NPC' then
                    controls['menu.' .. text] = { ui.GetItemRect() }
                  end
                  return drawText(...)
                end
                ui.SetSetting('hud.size.RaidWindow', '1000,700')
                ui.SetSetting('test.pos.RaidWindow', '20,20')
                hs.ShowWindow('RaidWindow', true)
            )lua");
            s.frames(4);
            // The gamepad pack's initial help tip covers the action column; dismiss unrelated onboarding first.
            run("hs.ShowWindow('TipWindow', false)");
            s.frames(2);
            auto rectangle = [&](const std::string& id) {
                lua_getglobal(L, "controls");
                lua_getfield(L, -1, id.c_str());
                REQUIRE(lua_istable(L, -1));
                ImVec4 r;
                float* fields[] = {&r.x, &r.y, &r.z, &r.w};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    *fields[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_settop(L, 0);
                return r;
            };
            auto mouse_click = [&](ImVec2 at, int button) {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(at.x, at.y);
                s.frames(1);
                io.AddMouseButtonEvent(button, true);
                s.frames(1);
                io.AddMouseButtonEvent(button, false);
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            };
            auto click = [&](const std::string& id) {
                const ImVec4 r = rectangle(id);
                mouse_click(ImVec2((r.x + r.z) / 2, (r.y + r.w) / 2), ImGuiMouseButton_Left);
            };
            auto member_menu = [&] {
                lua_getglobal(L, "memberPoints");
                lua_getfield(L, -1, "Member2");
                REQUIRE(lua_istable(L, -1));
                lua_rawgeti(L, -1, 1);
                const float x = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2);
                const float y = static_cast<float>(lua_tonumber(L, -1));
                lua_settop(L, 0);
                mouse_click(ImVec2(x, y), ImGuiMouseButton_Right);
                REQUIRE(s.host.menus.size() == 1);
                REQUIRE_FALSE(s.host.menus[0].rows.empty());
                CHECK(s.host.menus[0].rows.front().text == "Member2");
            };
            const std::array<const char*, 3> control_ids = modern ? std::array<const char*, 3>{
                     "Invite Expedition##RAID_InviteExpedButton", "Invite Task##RAID_InviteTaskButton",
                     "Member actions##RAID_MemberActionsButton"}
                                        : std::array<const char*, 3>{"##top6", "##top7", "##memberactions"};
            for (const char* id : control_ids)
                CHECK(rectangle(id).z > 0);
            run(modern ? "assert(controls['Main Assist##RAID_MainAssistButton'] == nil)"
                       : "assert(controls['##assist'] == nil)");
            auto delegates = [&] {
                std::vector<net::DelegateAbility> result;
                for (const auto& p : rig.server.zone_log)
                    if (p.opcode == net::to_wire(net::Op::RaidDelegateAbility))
                        if (auto d = net::decode_delegate_ability(p)) result.push_back(*d);
                return result;
            };
            // A different member was selected first: opening the menu must use the row under the right click.
            member_menu();
            const auto disabled_assist = std::find_if(s.host.menus[0].rows.begin(), s.host.menus[0].rows.end(),
                                                     [](const auto& row) { return row.text == "Main Assist"; });
            REQUIRE(disabled_assist != s.host.menus[0].rows.end());
            CHECK_FALSE(disabled_assist->enabled);
            CHECK(delegates().empty());
            s.host.menus.clear();
            s.frames(2);
            raid = {};
            raid.general.action = net::kRaidSetLeader;
            raid.general.player_name = raid.general.leader_name = me;
            raid.leadership[16] = 3;
            raid.leadership[0x13] = raid.leadership[0x14] = raid.leadership[0x1c] = 1;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            s.frames(2);
            CHECK(conn.leadership_ability_active(0x13, 1));
            CHECK(conn.raid().leader == me);
            conn.set_raid_target("Member1");
            member_menu();
            REQUIRE(std::any_of(s.host.menus[0].rows.begin(), s.host.menus[0].rows.end(), [](const auto& row) {
                return row.text == "Main Assist" && row.enabled;
            }));
            click("menu.Main Assist");
            REQUIRE(rig.until([&] { return delegates().size() == 1; }));
            CHECK(delegates()[0].ability == 3);
            CHECK(delegates()[0].name == "Member2");
            member_menu();
            click("menu.Mark NPC");
            REQUIRE(rig.until([&] { return delegates().size() == 2; }));
            CHECK(delegates()[1].ability == 4);
            CHECK(delegates()[1].name == "Member2");
            click(modern ? "Member actions##RAID_MemberActionsButton" : "##memberactions");
            REQUIRE(s.host.menus.size() == 1);
            CHECK(s.host.menus[0].rows.front().text == "Member2");
            s.host.menus.clear();
            s.frames(2);

            // Disabling context menus restores direct member controls in each shipped pack.
            conn.set_client_option_for_session(game::ClientOption::ContextMenus, 0);
            run("controls = {}");
            s.frames(3);
            run(modern ? "assert(controls['Member actions##RAID_MemberActionsButton'] == nil)"
                       : "assert(controls['##memberactions'] == nil)");
            click(modern ? "Main Assist##RAID_MainAssistButton" : "##assist");
            REQUIRE(rig.until([&] { return delegates().size() == 3; }));
            CHECK(delegates()[2].ability == 3);
            CHECK(delegates()[2].name == "Member2");
            click(modern ? "Mark NPC##RAID_MarkNPCButton" : "##marker");
            REQUIRE(rig.until([&] { return delegates().size() == 4; }));
            CHECK(delegates()[3].ability == 4);
            CHECK(delegates()[3].name == "Member2");
            raid = {};
            raid.general.action = net::kRaidSetLootType;
            raid.general.parameter = 3;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            if (!modern) {
                const ImVec4 r = rectangle("##assist");
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent((r.x + r.z) / 2, (r.y + r.w) / 2);
                s.frames(1);
                io.AddMouseWheelEvent(0, -8);
                s.frames(3);
            }
            click(modern ? "Add Looter##RAID_AddLooterButton" : "##addlooter");
            const auto add_looter = net::encode(net::RaidGeneral{net::kRaidCmdAddLooter, me, "Member2"});
            REQUIRE(rig.until([&] {
                return std::any_of(rig.server.zone_log.begin(), rig.server.zone_log.end(), [&](const auto& packet) {
                    return packet.opcode == add_looter.opcode && packet.data == add_looter.data;
                });
            }));
            conn.set_client_option_for_session(game::ClientOption::ContextMenus, 1);

            run("hs.ShowWindow('RaidWindow', false) hs.ShowWindow('LeadershipWindow', true)"
                " hs.ListInfo('LeadershipWindow', 'LEList2') hs.ListClick('LeadershipWindow', 'LEList2', 0, 'left')");
            s.frames(20); // let the gamepad window's focus-settling interval finish before pressing its actions
            {
                // Purchase is offered with any point, below the RoF2 cost too: the server's costs decide.
                net::LeadershipExp points;
                points.raid_points = 1;
                conn.receive_zone_packet_for_test(net::encode(points));
                run("assert(hs.ChildState('LeadershipWindow', 'LEW_PurchaseButton') == true)");
                points.raid_points = 0;
                conn.receive_zone_packet_for_test(net::encode(points));
                run("assert(hs.ChildState('LeadershipWindow', 'LEW_PurchaseButton') == false)");
            }
            for (int n = 1; n <= 3; ++n) {
                CAPTURE(n);
                const std::string id = modern ? "Create Hotkey " + std::to_string(n) + "##LEW_HotkeyButton" + std::to_string(n)
                                             : "##LeadershipWindowLEW_HotkeyButton" + std::to_string(n);
                run("assert(hs.ChildState('LeadershipWindow', 'LEW_HotkeyButton" + std::to_string(n) + "'))");
                click(id);
                REQUIRE(s.host.cursor_button);
                CHECK(s.host.cursor_button->type == game::HotButtonType::Leadership);
                CHECK(s.host.cursor_button->slot == 16 * 8 + n);
                if (modern) {
                    s.host.cursor_button.reset();
                } else {
                    run("assert(GPSTATE.assign and GPSTATE.assign.from == 'Leadership')");
                    s.frames(20);
                    click("##slot"); // the final cross-hotbar slot drawn by the assignment picker
                    CHECK_FALSE(s.host.cursor_button);
                    run("assert(not GPSTATE.assign)");
                    CHECK(hot_buttons->current(0, 7).type == game::HotButtonType::Leadership);
                    CHECK(hot_buttons->current(0, 7).slot == 16 * 8 + n);
                    s.frames(20);
                }
            }
            // A passive selection disables all three creation controls instead of making inert hot buttons.
            run("hs.ListInfo('LeadershipWindow', 'LEList2') hs.ListClick('LeadershipWindow', 'LEList2', 1, 'left')");
            s.frames(2);
            click(modern ? "Create Hotkey 1##LEW_HotkeyButton1" : "##LeadershipWindowLEW_HotkeyButton1");
            CHECK_FALSE(s.host.cursor_button);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: leadership names follow received ranks, purchases and training eligibility") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool modern = std::string_view(pack) == "modern";
        test::ScratchDir scratch("ui_pack_leadership_colors");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        std::ofstream(dir / "UI_Test_Server.ini") << "[Tips]\nShowTips=0\nShowTOTD=0\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            Session s(dir, pack);
            Config config;
            app::Gamepad gamepad(config, {});
            platform::GamepadState pad;
            pad.connected = true;
            gamepad.set_state(&pad);
            gamepad.set_mode(app::PadMode::Menu);
            s.host.gamepad = &gamepad;
            test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            const std::string me = conn.character_name();
            auto owned = [&](std::uint32_t ability, std::uint32_t rank) {
                ByteWriter w;
                w.le(ability);
                w.le(rank);
                w.le<std::uint32_t>(0);
                conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::UpdateLeadershipAA), w.take()));
            };
            for (const std::uint32_t ability : {0u, 1u, 16u, 17u}) owned(ability, 1);
            owned(0, 3);
            auto group = [&](int count, const std::string& leader) {
                net::GroupUpdateB u;
                u.leader = leader;
                for (int i = 0; i < count; ++i) {
                    net::GroupUpdateMember member;
                    member.slot = static_cast<std::uint32_t>(i);
                    member.name = i ? "Friend" + std::to_string(i) : me;
                    u.members.push_back(member);
                }
                conn.receive_zone_packet_for_test(net::encode(u));
            };
            net::GroupLeadershipUpdate active_group;
            active_group.ranks[0] = active_group.ranks[1] = active_group.ranks[8] = 1;
            group(2, "Friend1");
            conn.receive_zone_packet_for_test(net::encode(active_group));
            auto* L = s.lua.state();
            auto run = [&](const std::string& code) {
                const int rc = luaL_dostring(L, code.c_str());
                INFO(code);
                INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
                REQUIRE(rc == LUA_OK);
                lua_settop(L, 0);
            };
            run(R"lua(
                leadershipDrawn = {}
                local draw = ui.DrawText
                ui.DrawText = function(x, y, r, g, b, a, text, ...)
                    if type(text) == 'string' then leadershipDrawn[text] = {r, g, b} end
                    return draw(x, y, r, g, b, a, text, ...)
                end
                function checkLeadershipColor(list, name, r, g, b, rendered)
                    local count = hs.ListInfo('LeadershipWindow', list)
                    local found
                    for i = 0, count - 1 do
                        local row, cr, cg, cb = hs.ListRow('LeadershipWindow', list, i)
                        if row[1] == name then found = {cr, cg, cb} break end
                    end
                    assert(found, 'Missing leadership row: ' .. name)
                    local function check(c)
                        assert(math.abs(c[1] - r) < 0.001 and math.abs(c[2] - g) < 0.001
                            and math.abs(c[3] - b) < 0.001, 'Unexpected leadership color: ' .. name)
                    end
                    check(found)
                    if rendered then
                        assert(leadershipDrawn[name], 'Leadership name was not drawn: ' .. name)
                        check(leadershipDrawn[name])
                    end
                end
                ui.SetSetting('hud.size.LeadershipWindow', '1000,700')
                ui.SetSetting('test.pos.LeadershipWindow', '20,20')
                hs.ShowWindow('LeadershipWindow', true)
            )lua");
            auto render = [&] {
                run("leadershipDrawn = {}");
                s.frames(3);
                REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
            };
            auto gold = [&](const char* list, const char* name) {
                run(std::string("checkLeadershipColor('") + list + "', '" + name +
                    "', 214 / 255, 178 / 255, 40 / 255, true)");
            };
            auto green = [&](const char* list, const char* name) {
                run(std::string("checkLeadershipColor('") + list + "', '" + name + "', 0, 1, 0, true)");
            };
            render();
            CHECK_FALSE(conn.leadership_ability_active(1, 1));
            green("LEList1", "NPC Health");
            green("LEList1", "Mark NPC"); // leader rank 1 suffices even when the personal purchase is rank 3
            group(3, "Friend1");
            render();
            CHECK(conn.leadership_ability_active(1, 1));
            CHECK_FALSE(conn.leadership_ability_active(0, 1));
            green("LEList1", "NPC Health");
            green("LEList1", "Mark NPC"); // the window's color does not check the current member's role
            const auto profile = *conn.profile();
            auto without_god = profile;
            without_god.expansions &= ~0x40u;
            conn.set_profile_for_test(without_god);
            render();
            CHECK_FALSE(conn.leadership_ability_active(1, 1));
            green("LEList1", "NPC Health");
            conn.set_profile_for_test(profile);
            // The current leader's ranks take precedence even without a personal purchase.
            run("checkLeadershipColor('LEList1', 'Spell Awareness', 0, 1, 0, false)");
            conn.receive_zone_packet_for_test(net::encode(net::GroupLeadershipUpdate{}));
            render();
            gold("LEList1", "NPC Health");
            gold("LEList1", "Mark NPC");
            run("checkLeadershipColor('LEList1', 'Spell Awareness', 192 / 255, 192 / 255, 192 / 255, false)");
            owned(1, 0);
            render();
            run("checkLeadershipColor('LEList1', 'NPC Health', 192 / 255, 192 / 255, 192 / 255, true)");
            net::LeadershipExp points;
            points.group_points = 10;
            conn.receive_zone_packet_for_test(net::encode(points));
            render();
            run("checkLeadershipColor('LEList1', 'NPC Health', 1, 1, 1, true)");
            run("checkLeadershipColor('LEList1', 'Delegate Mark NPC', 1, 1, 1, true)");
            owned(0, 0);
            conn.receive_zone_packet_for_test(net::encode(points));
            render();
            run("checkLeadershipColor('LEList1', 'Delegate Mark NPC', 192 / 255, 192 / 255, 192 / 255, true)");
            owned(0, 0xffffffffu);
            conn.receive_zone_packet_for_test(net::encode(points));
            render();
            run("checkLeadershipColor('LEList1', 'Mark NPC', 192 / 255, 192 / 255, 192 / 255, true)");
            run("checkLeadershipColor('LEList1', 'Delegate Mark NPC', 192 / 255, 192 / 255, 192 / 255, true)");
            owned(1, 1);
            render();
            gold("LEList1", "NPC Health");
            conn.receive_zone_packet_for_test(net::encode(active_group));
            group(2, "Friend1");
            render();
            CHECK_FALSE(conn.leadership_ability_active(1, 1));
            green("LEList1", "NPC Health");
            conn.receive_zone_packet_for_test(net::encode_group_generic(net::Op::GroupDisbandYou, {me, me}));
            render();
            gold("LEList1", "NPC Health");

            net::RaidUpdate raid;
            raid.general.action = net::kRaidAddMember;
            raid.general.leader_name = "Raider1";
            for (int i = 0; i < 17; ++i) {
                raid.general.player_name = i ? "Raider" + std::to_string(i) : me;
                conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            }
            raid = {};
            raid.general.action = net::kRaidSetLeader;
            raid.general.leader_name = "Raider1";
            raid.leadership[16] = raid.leadership[17] = 1;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            if (modern) {
                std::optional<ImVec2> at;
                ImGuiContext& g = *ImGui::GetCurrentContext();
                for (int i = 0; i < g.TabBars.GetMapSize() && !at; ++i)
                    if (ImGuiTabBar* bar = g.TabBars.TryGetMapData(i))
                        for (ImGuiTabItem& tab : bar->Tabs)
                            if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)) == "Raid abilities")
                                at = ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width / 2,
                                            (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2);
                REQUIRE(at);
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(at->x, at->y);
                s.frames(1);
                io.AddMouseButtonEvent(0, true);
                s.frames(1);
                io.AddMouseButtonEvent(0, false);
            } else {
                run("ui.SetWindowFocus('Leadership')");
                pad.pressed[static_cast<std::size_t>(platform::PadButton::RB)] = true;
                s.frames(1);
                pad.pressed.fill(false);
            }
            render();
            CHECK_FALSE(conn.leadership_ability_active(17, 1));
            green("LEList2", "NPC Health");
            green("LEList2", "Mark NPC");
            raid = {};
            raid.general.action = net::kRaidAddMember;
            raid.general.player_name = "Raider17";
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            render();
            CHECK(conn.leadership_ability_active(17, 1));
            CHECK_FALSE(conn.leadership_ability_active(16, 1));
            green("LEList2", "NPC Health");
            green("LEList2", "Mark NPC");
            raid.general.action = net::kRaidRemoveMember;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            render();
            CHECK_FALSE(conn.leadership_ability_active(17, 1));
            green("LEList2", "NPC Health");
            raid.general.action = net::kRaidDisband;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            render();
            gold("LEList2", "NPC Health");
        }
        ImGui::DestroyContext(ctx);
    }
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
            if (o == "upscaler") return 4; // the first upscaler add-on (render/temporal.h kFirstProviderChoice)
            return std::nullopt;
        };
        std::set<std::string> listed;
        s.host.option_choices = [&listed](std::string_view o) {
            listed.insert(std::string(o));
            if (o == "upscaler_providers") return std::vector<std::string>{"Intel XeSS"};
            return o == "texture_pack" ? std::vector<std::string>{"Off", "default", "hd"} : std::vector<std::string>{};
        };
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('OptionsWindow', true)") == LUA_OK);
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
        // The Upscaler choices go on with the upscaler add-ons installed, one of them chosen.
        CHECK(asked.contains("upscaler"));
        CHECK(listed.contains("upscaler_providers"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: Options changes the Find trail style, Tab targeting and respects addon contributions to name plates") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(std::string(pack));
        test::ScratchDir scratch("ui_pack_display_options");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* prior_context = ImGui::GetCurrentContext();
        auto destroy_context = [prior_context](ImGuiContext* context) {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(prior_context);
        };
        std::unique_ptr<ImGuiContext, decltype(destroy_context)> ctx(ImGui::CreateContext(), destroy_context);
        ImGui::SetCurrentContext(ctx.get());
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            plugin::UiBridge bridge;
            Session s(dir, pack);
            s.host.plugin_ui = &bridge;
            s.host.plugin_caps = [](std::string_view owner) {
                return owner == "plugin:captions" || owner == "plugin:visibility"
                    ? plugin::kCapUi | plugin::kCapExtend : 0u;
            };
            std::vector<std::pair<std::string, std::string>> actions;
            s.host.submit_chat = [&](const std::string& line) { actions.emplace_back(plugin::caller_name(), line); };
            std::map<std::string_view, int> settings{
                {"find_trail_style", 0}, {"show_invis_undead", 0},
                {"tab_target_through_walls", 0}, {"tab_target_offscreen", 0}};
            std::map<std::string_view, std::vector<int>> changes;
            s.host.find_option = [&](std::string_view name) -> std::optional<int> {
                if (auto it = settings.find(name); it != settings.end()) return it->second;
                return std::nullopt;
            };
            s.host.set_option = [&](std::string_view name, int value) {
                if (auto it = settings.find(name); it != settings.end()) {
                    changes[it->first].push_back(value);
                    it->second = value;
                }
            };
            lua_State* L = s.lua.state();
            // Record real control coordinates without changing their activation or rendering.
            REQUIRE(luaL_dostring(L, R"lua(
                widgets = {}
                -- Keep the fixture's settings and the two notices in the Gamepad list's visible rows.
                local supports = hs.UI.Supports
                hs.UI.Supports = function(module, member)
                    return (module ~= "interfaces" or member ~= "Status") and supports(module, member)
                end
                local combo, selectable, checkbox, text = ui.BeginCombo, ui.Selectable, ui.Checkbox, ui.DrawText
                local button, wrapped = ui.Button, ui.TextWrapped
                local buttons = { ["Open caption settings"] = "action", ["Open visibility settings"] = "visibility action" }
                local notices = { ["captions: Captions are active."] = "section notice",
                    ["captions: Captions own the brackets."] = "captions reason",
                    ["visibility: Visibility owns the brackets."] = "visibility reason" }
                local function record(name)
                    local x0, y0, x1, y1 = ui.GetItemRect()
                    widgets[name] = { (x0 + x1) / 2, (y0 + y1) / 2 }
                end
                ui.Button = function(label, ...)
                    local clicked = button(label, ...)
                    if buttons[label] then record(buttons[label]) end
                    return clicked
                end
                ui.TextWrapped = function(label, ...)
                    wrapped(label, ...)
                    if notices[label] then record(notices[label]) end
                end
                ui.BeginCombo = function(label, ...)
                    local open = combo(label, ...)
                    if label == "##find_trail_style" then record("combo") end
                    return open
                end
                ui.Selectable = function(label, ...)
                    local clicked = selectable(label, ...)
                    local name = label:match("^Rainbow trail##") and "rainbow"
                        or label:match("^Gold line %(default%)##") and "navigation"
                    if name then record(name) end
                    return clicked
                end
                ui.Checkbox = function(label, ...)
                    local value, changed = checkbox(label, ...)
                    if label == "##show_invis_undead" then record("Bracket Invis vs. Undead")
                    elseif label == "##tab_target_through_walls" then record("Target through walls and doors")
                    elseif label == "##tab_target_offscreen" then record("Target off-screen") end
                    return value, changed
                end
                ui.DrawText = function(...)
                    local label = select(7, ...)
                    local key = notices[label] or buttons[label]
                    if key or label == "Interface" or label == "Find trail" or label == "Bracket Invis vs. Undead"
                        or label == "Target through walls and doors" or label == "Target off-screen" then
                        local x, y = ...
                        local w, h = ui.CalcTextSize(label)
                        widgets[key or label] = { x + w / 2, y + h / 2 }
                    end
                    return text(...)
                end
                hs.ShowWindow("OptionsWindow", true)
            )lua") == LUA_OK);
            s.frames(3);
            auto point = [&](const char* name) {
                lua_getglobal(L, "widgets");
                lua_getfield(L, -1, name);
                REQUIRE_MESSAGE(lua_istable(L, -1), std::string(name));
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
                REQUIRE(settings.at("find_trail_style") == 1);
                click(point("combo"));
                click(point("navigation"));
            } else {
                click(point("Interface"));
                CHECK(changes.empty());
                click(point("Find trail"));
                REQUIRE(settings.at("find_trail_style") == 1);
                click(point("Find trail"));
            }
            CHECK(settings.at("find_trail_style") == 0);
            CHECK(changes["find_trail_style"] == std::vector<int>{1, 0});
            CHECK_FALSE(changes.contains("show_invis_undead"));
            click(point("Bracket Invis vs. Undead"));
            CHECK(settings.at("show_invis_undead") == 1);
            click(point("Bracket Invis vs. Undead"));
            CHECK(settings.at("show_invis_undead") == 0);
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0});

            REQUIRE(bridge.set_contribution({"captions", "status", "OptionsWindow", "section:nameplates",
                                             "Captions are active.", {}, {}}));
            REQUIRE(bridge.set_contribution({"captions", "brackets", "OptionsWindow", "setting:show_invis_undead",
                                             "Captions own the brackets.", "Open caption settings", "/captions", true}));
            REQUIRE(bridge.set_contribution({"visibility", "brackets", "OptionsWindow", "setting:show_invis_undead",
                                             "Visibility owns the brackets.", "Open visibility settings", "/visibility", true}));
            s.frames(3);
            CHECK(point("section notice").y < point("Bracket Invis vs. Undead").y);
            CHECK(point("captions reason").y < point("visibility reason").y);
            click(point("Bracket Invis vs. Undead"));
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0});
            click(point("action"));
            CHECK(actions == std::vector<std::pair<std::string, std::string>>{{"plugin:captions", "/captions"}});

            if (std::string_view(pack) == "modern") {
                click(point("combo"));
                click(point("rainbow"));
            } else {
                click(point("Find trail"));
            }
            CHECK(settings.at("find_trail_style") == 1);

            // Releasing a click after its notice disappears must not run the action that moved into its row.
            const auto removed_action = point("action");
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(removed_action.x, removed_action.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            REQUIRE(bridge.remove_contribution("captions", "brackets"));
            s.frames(3);
            io.AddMouseButtonEvent(0, false);
            s.frames(3);
            REQUIRE(actions.size() == 1);
            click(point("visibility action"));
            REQUIRE(actions.size() == 2);
            CHECK(actions.back() == std::pair<std::string, std::string>{"plugin:visibility", "/visibility"});
            click(point("Bracket Invis vs. Undead"));
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0});
            REQUIRE(bridge.remove_contribution("visibility", "brackets"));
            s.frames(3);
            click(point("Bracket Invis vs. Undead"));
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0, 1});

            CHECK_FALSE(changes.contains("tab_target_through_walls"));
            CHECK_FALSE(changes.contains("tab_target_offscreen"));
            click(point("Target through walls and doors"));
            CHECK(settings.at("tab_target_through_walls") == 1);
            CHECK(settings.at("tab_target_offscreen") == 0);
            if (std::string_view(pack) == "gamepad") {
                // The off-screen option is just below the visible rows at this window size.
                ImGui::GetIO().AddMouseWheelEvent(0, -1);
                s.frames(3);
            }
            click(point("Target off-screen"));
            CHECK(settings.at("tab_target_through_walls") == 1);
            CHECK(settings.at("tab_target_offscreen") == 1);
            click(point("Target through walls and doors"));
            CHECK(settings.at("tab_target_through_walls") == 0);
            CHECK(settings.at("tab_target_offscreen") == 1);
            click(point("Target off-screen"));
            CHECK(settings.at("tab_target_offscreen") == 0);
            CHECK(changes["tab_target_through_walls"] == std::vector<int>{1, 0});
            CHECK(changes["tab_target_offscreen"] == std::vector<int>{1, 0});
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
    }
}

// A bag window resized: as many slots to a row as its width holds, its height grown to fit the rows; unsized, rows
// of five. A click lands on the slot drawn under the mouse (hs.BagSlotClick's index).
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
            hs.BagInfo = function(win) if win == 'Bag1' then return 'Test Bag', 10, false, 'inventory', 23 end end
            hs.BagSlotItem = function() return nil end
            local WindowShown = hs.WindowShown -- (the game opens a bag window only on a bag it has)
            hs.WindowShown = function(win) if win == 'Bag1' then return true end return WindowShown(win) end
            hs.BagSlotClick = function(win, i, button) if button == 'left' then bagClicked = i end end
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

// The buff menu (ui/modern/lib/buffmenu.lua): a right click on a target's or the pet's buff opens that buff's menu,
// and its rows act on the buff that was clicked: Spell Info on the target's, Add to Blocked Pet Spells on the pet's.
// The frame's own menu stays shut (the buff took the click); Alt with a right click still opens it.
TEST_CASE("UI packs (modern): a right click on a target's or the pet's buff opens the buff menu") {
    test::ScratchDir scratch("ui_pack_buff_menu_test");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Session s(dir, "modern");
        auto* L = s.lua.state();
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        // A target and a pet with one buff each, and the menu's rows recorded: the pick is the row that acts.
        run(R"lua(
targetShown, petShown = true, true
targetIcon, petIcon = nil, nil
menuLabels, menuPick, shownSpell, blocked = {}, nil, nil, {}
local buffSlot, label, shown, gauge, menuItem, showSpell, setText, clickChild =
  hs.BuffSlot, hs.Label, hs.WindowShown, hs.Gauge, ui.MenuItem, hs.ShowSpell, hs.SetWindowText, hs.ClickChild
hs.BuffSlot = function(win, id, overlays)
  if win == "TargetWindow" and id == "Buff0" then return 11, 60000, "Holy Armor", true end
  if win == "PetInfoWindow" and id == "PetBuff0" then return 11, 60000, "Holy Armor", true end
  return buffSlot(win, id, overlays)
end
hs.Label = function(n) if n == 28 then return "a rat" end return label(n) end
hs.WindowShown = function(win)
  if win == "TargetWindow" then return targetShown end
  if win == "PetInfoWindow" then return petShown end
  return shown(win)
end
hs.Gauge = function(k)
  if k == 6 then return 1, 0, 100 end
  if k == 16 then return 1, "Pet", 100 end
  if k == 27 or k == 41 or k == 149 then return 0, "", -1 end
  return gauge(k)
end
hs.ShowSpell = function(id) shownSpell = id end
hs.SetWindowText = function(win, id, text)
  if win == "BlockedPetBuffWnd" then blocked.edit = id .. "=" .. text end
  return setText(win, id, text)
end
hs.ClickChild = function(win, id, button)
  if win == "BlockedPetBuffWnd" then blocked.add = id .. ":" .. tostring(button); return true end
  return clickChild(win, id, button)
end
ui.MenuItem = function(text, ...)
  menuLabels[#menuLabels + 1] = text
  if text == menuPick then return true end
  return menuItem(text, ...)
end
local button = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##tbBuff0" then targetIcon = { ui.GetItemRect() } end
  if id == "##PetBuff0" then petIcon = { ui.GetItemRect() } end
  return hit
end
)lua");
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(run("return tostring(targetIcon ~= nil and petIcon ~= nil)") == "true");
        auto centre = [&](const char* name) {
            lua_getglobal(L, name);
            float r[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                r[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            return ImVec2((r[0] + r[2]) / 2, (r[1] + r[3]) / 2);
        };
        // The mouse moves onto the icon and the button goes down in the same frame: the frame before hovered
        // something else, so an icon that lets a later item lie over it would lose the click to the frame.
        auto press = [&](ImVec2 at) {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(1, true);
            s.frames(1);
        };
        auto release = [&] {
            ImGui::GetIO().AddMouseButtonEvent(1, false);
            s.frames(1);
        };
        auto closeMenu = [&] {
            ImGui::ClosePopupsExceptModals();
            s.frames(1);
            run("menuPick, menuLabels = nil, {}");
        };
        // the target's buff: Spell Info, and the frame's menu stays shut
        run("menuPick = 'Spell Info'");
        press(centre("targetIcon"));
        CHECK(run("return tostring(shownSpell)") == "11");
        CHECK(run("return table.concat(menuLabels, '|')") == "Spell Info");
        run("menuPick = nil");
        release();
        closeMenu();
        // Alt with a right click still opens the frame's own menu
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
        press(centre("targetIcon"));
        CHECK(run("return menuLabels[1]") == "Lock this frame");
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
        release();
        closeMenu();
        // the pet's buff: Add to Blocked Pet Spells, on the pet's own list
        run("menuPick = 'Add to Blocked Pet Spells'");
        press(centre("petIcon"));
        CHECK(run("return blocked.edit") == "BW_EditSpellToBlockFromPet=11");
        CHECK(run("return blocked.add") == "BW_AddSpellToBlockFromPet:left");
        CHECK(run("return table.concat(menuLabels, '|')") == "Spell Info|Add to Blocked Pet Spells");
        run("menuPick = nil");
        release();
        closeMenu();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): buff tray options preserve the default layout and old slot settings") {
    test::ScratchDir scratch("ui_pack_buff_tray_slots");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    {
        Session s(dir, "modern");
        auto* L = s.lua.state();
        lua_pushcfunction(L, [](lua_State* state) {
            const auto* window = ImGui::GetCurrentWindowRead();
            const std::string_view name = window ? window->Name : "";
            if (name.ends_with("###modern.BuffWindow")) lua_pushliteral(state, "BuffWindow");
            else if (name.ends_with("###modern.ShortDurationBuffWindow")) lua_pushliteral(state, "ShortDurationBuffWindow");
            else lua_pushnil(state);
            return 1;
        });
        lua_setglobal(L, "testTrayWindow");
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        run(R"lua(
hs.ShowWindow('BuffWindow', true)
hs.ShowWindow('ShortDurationBuffWindow', true)
ui.SetSetting('hud.pos.BuffWindow', '600,160')
ui.SetSetting('hud.pos.ShortDurationBuffWindow', '600,480')
ui.SetSetting('hud.perRow.BuffWindow', 3)
local names = {
  BuffWindow = { Buff2 = 'Spirit of Wolf', Buff4 = 'Holy Armor', Buff5 = 'Clarity' },
  ShortDurationBuffWindow = { Buff0 = 'Chorus of the Singing Grove', Buff1 = 'Psalm' },
}
local slot, button, textColored, menuItem = hs.BuffSlot, ui.InvisibleButton, ui.TextColored, ui.MenuItem
local drawText, drawRect, beginMenu, endMenu = ui.DrawText, ui.DrawRectFilled, ui.BeginMenu, ui.EndMenu
hs.BuffSlot = function(win, id, overlays)
  local name = names[win] and names[win][id]
  if name then return 11, id == 'Buff0' and 43000 or 60000, name, true end
  return slot(win, id, overlays)
end
buffRects, songRects, menuRows = {}, {}, {}
function clearTrayDraws()
  trayText = { BuffWindow = {}, ShortDurationBuffWindow = {} }
  trayPanel = {}
end
clearTrayDraws()
ui.InvisibleButton = function(id, ...)
  local pressed = button(id, ...)
  local n = tonumber(id:match('^##Buff(%d+)$'))
  local tray = n and testTrayWindow()
  if tray == 'BuffWindow' then
    buffRects[n] = { ui.GetItemRect() }
    local _, h = ui.GetWindowSize()
    trayHeight = h
  elseif tray == 'ShortDurationBuffWindow' then
    songRects[n] = { ui.GetItemRect() }
  end
  return pressed
end
ui.TextColored = function(r, g, b, a, label)
  if type(label) == 'string' and label:find('^Spirit of Wolf') then tooltip = label end
  return textColored(r, g, b, a, label)
end
ui.MenuItem = function(label, ...)
  menuRows[label] = true
  if label == pick then
    pick = nil
    ui.CloseCurrentPopup()
    return true
  end
  return menuItem(label, ...)
end
-- Exercise the layout choices in the live frame popup without depending on the submenu's hover delay.
local forcedLayout
ui.BeginMenu = function(label, ...)
  if label == 'Layout' and (pick == 'Vertical' or pick == 'Horizontal') then
    forcedLayout = true
    return true
  end
  return beginMenu(label, ...)
end
ui.EndMenu = function()
  if forcedLayout then forcedLayout = false; return end
  return endMenu()
end
ui.DrawText = function(x, y, r, g, b, a, text, ...)
  local tray = testTrayWindow()
  if tray then trayText[tray][text] = { x, y } end
  return drawText(x, y, r, g, b, a, text, ...)
end
ui.DrawRectFilled = function(x0, y0, x1, y1, ...)
  local tray = testTrayWindow()
  if tray then
    local x, y = ui.GetWindowPos()
    local w, h = ui.GetWindowSize()
    if x0 == x and y0 == y and x1 == x + w and y1 == y + h then trayPanel[tray] = true end
  end
  return drawRect(x0, y0, x1, y1, ...)
end
)lua");
        auto point_at_buff = [&](bool song = false) {
            REQUIRE(luaL_dostring(L, song ? "return songRects[0][1] + 10, songRects[0][2] + 10"
                                          : "return buffRects[2][1] + 10, buffRects[2][2] + 10") == LUA_OK);
            io.AddMousePosEvent(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
            lua_settop(L, 0);
        };
        auto choose = [&](const char* label, bool song = false) {
            run((std::string("pick = '") + label + "'").c_str());
            point_at_buff(song);
            io.AddKeyEvent(ImGuiMod_Alt, true);
            io.AddMouseButtonEvent(1, true);
            s.frames(1);
            CHECK(run("return tostring(pick == nil)") == "true");
            io.AddMouseButtonEvent(1, false);
            io.AddKeyEvent(ImGuiMod_Alt, false);
            ImGui::ClosePopupsExceptModals();
            run("buffRects = {}");
            s.frames(2);
        };
        auto title = [&] {
            run("tooltip = nil");
            point_at_buff();
            s.frames(2);
            return run("return tooltip");
        };
        s.frames(3);
        REQUIRE(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
compactX, compactY, compactHeight, pitch = a[1], a[2], trayHeight, a[1] - b[1]
local d, e = songRects[0], songRects[1]
return tostring(pitch == 38 and a[3] - a[1] == 34 and b[1] - c[1] == pitch and a[2] == b[2] and a[2] == c[2]
  and d[3] - d[1] == 26 and d[1] - e[1] == 30 and d[2] == e[2])
)lua") == "true");
        CHECK(title() == "Spirit of Wolf");
        CHECK(run(R"lua(
local buffs, songs = trayText.BuffWindow, trayText.ShortDurationBuffWindow
return tostring(buffs['1m'] and songs['1m'] and not buffs['Spirit of Wolf'] and not songs['Chorus of the Singing Grove']
  and not buffs['#3'] and not buffs.Buffs and not songs.Songs
  and not trayPanel.BuffWindow and not trayPanel.ShortDurationBuffWindow)
)lua") == "true");

        choose("Show slot numbers in tooltips");
        CHECK(run("return ui.GetSetting('hud.showSlotNumbers.BuffWindow')") == "1");
        CHECK(run("return tostring(buffRects[2][1] == compactX and trayHeight == compactHeight)") == "true");
        CHECK(title() == "Spirit of Wolf - Slot 3");
        choose("Show slot numbers in tooltips");

        choose("Show empty slots");
        CHECK(run("return ui.GetSetting('hud.showEmptySlots.BuffWindow')") == "1");
        CHECK(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
return tostring(a[1] == compactX - 2 * pitch and a[2] == compactY
  and b[1] == compactX - pitch and b[2] > compactY and c[1] == a[1] and c[2] == b[2]
  and trayHeight - compactHeight == b[2] - compactY
  and buffRects[0] == nil and buffRects[1] == nil and buffRects[3] == nil)
)lua") == "true");
        CHECK(title() == "Spirit of Wolf");
        choose("Show empty slots");

        // A bare right click keeps the spell actions. Window options opens the same frame menu as Alt+right-click.
        run("menuRows = {}; pick = 'Window options'");
        point_at_buff();
        io.AddMouseButtonEvent(1, true);
        s.frames(1);
        io.AddMouseButtonEvent(1, false);
        s.frames(2);
        CHECK(run("return tostring(pick == nil and menuRows['Spell Info'] and menuRows.Remove"
                  " and menuRows['Allow window resizing'])") == "true");
        ImGui::ClosePopupsExceptModals();
        s.frames(2);

        ImGuiWindow* buffs = ImGui::FindWindowByName("Buffs###modern.BuffWindow");
        ImGuiWindow* songs = ImGui::FindWindowByName("Songs###modern.ShortDurationBuffWindow");
        REQUIRE(buffs);
        REQUIRE(songs);
        const ImVec2 defaultSize = buffs->Size;
        const ImVec2 songSize = songs->Size;
        auto drag = [&](ImVec2 start, float dx, float dy) {
            io.AddMousePosEvent(start.x, start.y);
            s.frames(2);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 4; ++i) {
                io.AddMousePosEvent(start.x + dx * static_cast<float>(i) / 4,
                                    start.y + dy * static_cast<float>(i) / 4);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(3);
        };
        auto leftEdge = [&] { return ImVec2(buffs->Pos.x + 2, buffs->Pos.y + buffs->Size.y / 2); };
        auto bottomEdge = [&] { return ImVec2(buffs->Pos.x + buffs->Size.x / 2, buffs->Pos.y + buffs->Size.y - 2); };
        drag(leftEdge(), 40, 0);
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow') or ''") == "");
        CHECK(buffs->Size.x == defaultSize.x);
        CHECK(buffs->Size.y == defaultSize.y);

        choose("Allow window resizing");
        CHECK(run("return ui.GetSetting('hud.buffResize.BuffWindow')") == "1");
        const float anchoredRight = buffs->Pos.x + buffs->Size.x;
        drag(leftEdge(), 40, 0);
        const auto manualSize = run("return ui.GetSetting('hud.size.BuffWindow') or ''");
        REQUIRE_FALSE(manualSize.empty());
        CHECK(buffs->Size.x < defaultSize.x);
        CHECK(buffs->Size.y > defaultSize.y);
        CHECK(buffs->Pos.x + buffs->Size.x == doctest::Approx(anchoredRight).epsilon(0.002));
        CHECK(songs->Size.x == songSize.x);
        CHECK(songs->Size.y == songSize.y);
        run("ui.SetSetting('hud.lock.BuffWindow', '1')");
        drag(leftEdge(), -30, 0);
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow')") == manualSize);
        CHECK(buffs->Pos.x + buffs->Size.x == doctest::Approx(anchoredRight).epsilon(0.002));
        run("ui.SetSetting('hud.lock.BuffWindow', '0')");
        choose("Allow window resizing");
        const ImVec2 wrapped = buffs->Size;
        drag(leftEdge(), -30, 0);
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow')") == manualSize);
        CHECK(buffs->Size.x == wrapped.x);
        CHECK(buffs->Size.y == wrapped.y);
        choose("Reset size");
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow') or ''") == "");
        CHECK(buffs->Size.x == defaultSize.x);
        CHECK(buffs->Size.y == defaultSize.y);

        choose("Vertical");
        CHECK(run("return ui.GetSetting('hud.buffLayout.BuffWindow')") == "vertical");
        CHECK(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
return tostring(a[1] == b[1] and b[1] == c[1] and a[2] < b[2] and b[2] < c[2]
  and songRects[0][2] == songRects[1][2])
)lua") == "true");
        choose("Allow window resizing");
        drag(bottomEdge(), 0, -43);
        CHECK(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
return tostring(a[1] == b[1] and c[1] < a[1] and a[2] < b[2] and c[2] == a[2])
)lua") == "true");

        choose("Show spell names");
        choose("Show slot numbers");
        choose("Show remaining time");
        choose("Show title bar");
        CHECK(run("return ui.GetSetting('hud.buffNames.BuffWindow')") == "1");
        CHECK(run("return ui.GetSetting('hud.buffSlotBadges.BuffWindow')") == "1");
        CHECK(run("return ui.GetSetting('hud.buffHideTime.BuffWindow')") == "1");
        CHECK(run("return ui.GetSetting('hud.buffTitleBar.BuffWindow')") == "1");
        run("clearTrayDraws()");
        s.frames(2);
        CHECK(run(R"lua(
local buffs, songs = trayText.BuffWindow, trayText.ShortDurationBuffWindow
return tostring(buffs['Spirit of Wolf'] and buffs['#3'] and buffs.Buffs and not buffs['1m']
  and songs['1m'] and not songs['Chorus of the Singing Grove'] and not songs.Songs and not trayPanel.BuffWindow)
)lua") == "true");
        run("pick = 'Show window background'");
        io.AddMousePosEvent(buffs->Pos.x + buffs->Size.x / 2, buffs->Pos.y + 6);
        io.AddMouseButtonEvent(1, true);
        s.frames(1);
        io.AddMouseButtonEvent(1, false);
        s.frames(2);
        CHECK(run("return tostring(pick == nil)") == "true");
        CHECK(run("return ui.GetSetting('hud.buffBackground.BuffWindow')") == "1");
        choose("Show title bar");
        run("clearTrayDraws()");
        s.frames(2);
        CHECK(run("return tostring(trayPanel.BuffWindow and not trayText.BuffWindow.Buffs"
                  " and not trayPanel.ShortDurationBuffWindow)") == "true");

        // Songs can opt into a named horizontal ribbon while Buffs stays a vertical list.
        choose("Show spell names", true);
        run("clearTrayDraws()");
        s.frames(2);
        CHECK(run(R"lua(
local a, b = songRects[0], songRects[1]
return tostring(a[1] > b[1] and a[2] == b[2] and ui.GetSetting('hud.buffLayout.BuffWindow') == 'vertical')
)lua") == "true");
        CHECK(run(R"lua(
local text = trayText.ShortDurationBuffWindow
local timer, lines = text['43s'], {}
if not timer then return 'false' end
for label, pos in pairs(text) do
  if pos[1] == timer[1] and pos[2] < timer[2] then lines[#lines + 1] = { label, pos[2] } end
end
table.sort(lines, function(a, b) return a[2] < b[2] end)
return tostring(#lines == 2 and lines[1][1] .. ' ' .. lines[2][1] == 'Chorus of the Singing Grove')
)lua") == "true");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): the Options search draws every tab, and only the sections that match") {
    test::ScratchDir scratch("ui_pack_options_search");
    const fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        Session s(dir, "modern");
        // Every option there (at 0), so every row is offered to the search.
        s.host.find_option = [](std::string_view) -> std::optional<int> { return 0; };
        lua_State* L = s.lua.state();
        // The search field answers with optSearch, the tab optTab is selected, and the section headings drawn are
        // kept; a plugin with one option gives the Plugins tab.
        REQUIRE(luaL_dostring(L, R"lua(
            optSearch, optTab, optTabs, headings = "", nil, {}, {}
            local input, separator, tabItem = ui.InputTextWithHint, ui.SeparatorText, ui.BeginTabItem
            ui.InputTextWithHint = function(label, ...)
                local text, changed = input(label, ...)
                if label == "##optionssearch" then return optSearch, true end
                return text, changed
            end
            ui.SeparatorText = function(text)
                headings[text] = true
                return separator(text)
            end
            ui.BeginTabItem = function(name, flags)
                if not optTabs[name] then
                    optTabs[name] = true
                    optTabs[#optTabs + 1] = name
                end
                if name == optTab then flags = (flags or 0) | ui.TabItemFlags.SetSelected end
                return tabItem(name, flags)
            end
            hs.PluginOptions = function()
                return { { plugin = "Demo Helper", owner = "plugin:demo", name = "on", label = "Enabled", min = 0,
                           max = 1, value = 0 } }
            end
            hs.ShowWindow("OptionsWindow", true)
        )lua") == LUA_OK);
        s.frames(3);
        auto lua_string = [&](const char* code) {
            REQUIRE_MESSAGE(luaL_dostring(L, code) == LUA_OK, lua_tostring(L, -1));
            std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return out;
        };
        const std::string tabs = lua_string("return table.concat(optTabs, ',')");
        REQUIRE(tabs.find("Plugins") != std::string::npos);
        // Draws every tab with the search; gives the headings drawn in all of them, sorted.
        auto headings_for = [&](const char* search) {
            lua_pushstring(L, search);
            lua_setglobal(L, "optSearch");
            REQUIRE(luaL_dostring(L, "headings = {}") == LUA_OK);
            for (int i = 1;; ++i) {
                lua_getglobal(L, "optTabs");
                lua_rawgeti(L, -1, i);
                const bool more = lua_isstring(L, -1);
                if (more) lua_setglobal(L, "optTab");
                lua_settop(L, 0);
                if (!more) break;
                s.frames(3);
            }
            return lua_string("local t = {} for k in pairs(headings) do t[#t + 1] = k end table.sort(t) "
                              "return table.concat(t, ',')");
        };
        const std::string all = headings_for("");
        CHECK(all.find("Target Ring") != std::string::npos);
        CHECK(all.find("Name Plates") != std::string::npos);
        CHECK(all.find("Demo Helper") != std::string::npos);
        auto has = [](const std::string& list, std::string_view name) {
            return ("," + list + ",").find("," + std::string(name) + ",") != std::string::npos;
        };
        // The section's title is searched: all its rows show, and the sections with nothing that matches go (the
        // controls tab's key list keeps its own heading).
        const std::string ring = headings_for("target ring");
        CHECK(has(ring, "Target Ring"));
        CHECK_FALSE(has(ring, "Name Plates"));
        CHECK_FALSE(has(ring, "Volume"));
        const std::string demo = headings_for("demo"); // a plugin's section by the plugin's name
        CHECK(demo == "Demo Helper,Key Bindings");
        const std::string fov = headings_for("fov"); // Field of View, by its initials
        CHECK(has(fov, "View"));
        CHECK_FALSE(has(fov, "Target Ring"));
        // Nothing matches: no section's heading or empty table, only the key list's own heading.
        CHECK(headings_for("zzqqxx") == "Key Bindings");
        CHECK(headings_for("") == all);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}
