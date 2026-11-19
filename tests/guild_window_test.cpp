// The guild window's roster over the hs API (app/lua_ui_social.cpp GuildWindow): the lists are built once a frame
// and again after the window's own state changes, so a sorted list reading every row stays cheap.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "mock_server/harness.h"
#include "mq/commands.h"
#include "mq/tlo.h"
#include "net/game_connection.h"
#include "net/social_packets.h"
#include "plugin/hooks.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

std::string run(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

net::AppPacket roster(std::initializer_list<const char*> names) {
    net::GuildMemberList l;
    l.player_name = "Tester";
    l.guild_id = 3;
    for (const char* n : names) {
        net::GuildMember m;
        m.name = n;
        m.level = 50;
        m.rank = 5;
        m.zone_id = 2;
        l.members.push_back(m);
    }
    return net::encode(l);
}

} // namespace

TEST_CASE("guild window: the roster list is built once a frame, and again after a click") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("guild_window");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");

        const std::string list = "'GuildManagementWnd', 'MemberList'";
        conn.receive_zone_packet_for_test(roster({"Alice", "Bob", "Carl"}));
        api.begin_frame();
        CHECK(run(L, "return (hs.ListInfo(" + list + "))") == "3");
        CHECK(run(L, "return hs.ListRow(" + list + ", 1)[1]") == "Bob");
        CHECK(run(L, "return (select(3, hs.ListInfo(" + list + ")))") == "-1");
        // A click selects at once, in the same frame.
        CHECK(run(L, "return hs.ListClick(" + list + ", 2, 'left')") == "true");
        CHECK(run(L, "return (select(3, hs.ListInfo(" + list + ")))") == "2");
        // A new roster shows from the next frame, the selection kept by name.
        conn.receive_zone_packet_for_test(roster({"Aaron", "Alice", "Bob", "Carl"}));
        api.begin_frame();
        CHECK(run(L, "return (hs.ListInfo(" + list + "))") == "4");
        CHECK(run(L, "return hs.ListRow(" + list + ", 0)[1]") == "Aaron");
        CHECK(run(L, "return (select(3, hs.ListInfo(" + list + ")))") == "3");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("guild window: Go to URL and Join Channel send the guild's text as it is; only an http(s) page opens") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("guild_window_links");
        GameDir game(scratch.path());
        plugin::Hooks hooks;
        net::ConnectionOptions opts;
        opts.hooks = &hooks;
        net::GameConnection conn(game, opts);
        // MQ's command filter (${...} with the player's read permission), then what reaches the interpreter.
        mq::Registry reg;
        mq::register_game_tlos(reg);
        mq::Context mctx;
        mctx.registry = &reg;
        mctx.conn = &conn;
        mq::CommandEnv ce;
        ce.hooks = &hooks;
        ce.registry = &reg;
        ce.context = [&]() -> const mq::Context& { return mctx; };
        mq::Commands commands(ce);
        std::vector<std::string> lines;
        const auto capture = hooks.new_owner("capture");
        hooks.on_command_line(capture, [&](std::string& line) {
            lines.push_back(line);
            return plugin::Flow::Drop;
        });

        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        auto click = [&](const char* button) {
            api.begin_frame();
            return run(L, std::string("return hs.ClickChild('GuildManagementWnd', '") + button + "', 'left')");
        };
        auto set = [&](std::uint32_t action, const std::string& text) {
            net::AppPacket p = net::encode_guild_update_text(action, text);
            conn.receive_zone_packet_for_test(p);
        };

        // An officer's text holding ${...}: sent as it is, not filled in with the clicker's state.
        set(0, "https://guild.example.org/?who=${Me.Name}");
        set(1, "chan${Me.Name}");
        CHECK(click("GotoURL") == "true");
        CHECK(click("JoinChannel") == "true");
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "/url https://guild.example.org/?who=${Me.Name}");
        CHECK(lines[1] == "/join chan${Me.Name}");

        // Only web pages: another scheme is not opened; one without a scheme gets http://, as /www.
        lines.clear();
        set(0, "javascript:alert(1)");
        CHECK(click("GotoURL") == "true");
        set(0, "file:///etc/passwd");
        CHECK(click("GotoURL") == "true");
        CHECK(lines.empty());
        set(0, "www.guild.example.org");
        CHECK(click("GotoURL") == "true");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "/url http://www.guild.example.org");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("mock server: guild banner page selects kits, edits a draft, and saves the chosen appearance") {
    using namespace hailstone::test;
    auto config = MockConfig::standard();
    auto& zone = config.zones.at(2);
    zone.start_x = 40.0f;
    zone.start_y = -15.0f;
    zone.start_z = 4.0f;
    ManualClock clock;
    auto options = quick_options();
    options.session.clock = clock.source();
    Rig rig(std::move(config), std::move(options));
    REQUIRE(rig.to_zone());
    constexpr std::uint32_t guild_id = 3, rank = 5;
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{0, 22, guild_id}));
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{0, 23, rank}));
    for (std::uint32_t permission : {1u, 2u, 3u})
        rig.conn.receive_zone_packet_for_test(net::encode_guild_rank_permission(rank, permission, true, guild_id));

    ScratchDir scratch("guild_banner_categories");
    std::filesystem::create_directories(scratch.path() / "Resources");
    std::ofstream(scratch.path() / "Resources" / "BannerCategories.txt") << "7^Fixture category\n";
    GameDir game(scratch.path());
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &rig.conn;
        host.game = &game;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        REQUIRE(run(L, "g = 'GuildManagementWnd'; list = 'BannerKitList'; "
                       "function click(child) return hs.ClickChild(g, child, 'left') end; "
                       "function enabled(child) return (hs.ChildState(g, child)) end; "
                       "function value(child) return (hs.WindowText(g, child)) end; "
                       "hs.ShowWindow(g, true); hs.SetTabPage(g, 'Subwindows', 4); return true") == "true");
        CHECK(run(L, "return hs.ChildShown(g, 'BannerData')") == "false");
        CHECK(run(L, "return (hs.ComboInfo(g, 'BannerFilter'))[2]") == "Fixture category");
        CHECK(run(L, "return hs.ComboSelect(g, 'BannerFilter', 1)") == "true");
        const auto filtered = [&] {
            return std::ranges::any_of(rig.server.zone_log, [](const auto& packet) {
                if (packet.opcode != net::to_wire(net::Op::GuildBannerKitRequest) || packet.data.size() != 141) return false;
                ByteReader reader(packet.data);
                reader.seek(137);
                return reader.le<std::uint32_t>() == (1u << 7);
            });
        };
        REQUIRE(rig.until(filtered));
        clock.hold();

        ByteWriter kits;
        kits.zeros(4);
        kits.le<std::uint32_t>(2);
        kits.le<std::uint32_t>(5);
        kits.le<std::uint32_t>(3);
        kits.le<std::uint32_t>(50);
        kits.fixed_string("Fixture available kit", 128);
        kits.le<std::uint8_t>(1);
        kits.le<std::uint32_t>(100);
        kits.le<std::uint32_t>(60);
        kits.fixed_string("Fixture unavailable kit", 128);
        kits.le<std::uint8_t>(0);
        kits.le<std::uint32_t>(101);
        rig.conn.receive_zone_packet_for_test({net::to_wire(net::Op::GuildBannerKitList), kits.take()});
        const auto status_packet = [](std::uint32_t mode) {
            ByteWriter status;
            status.zeros(4);
            status.le(mode);
            status.zeros(4);
            status.le<std::uint32_t>(40);
            status.le<std::uint32_t>(102);
            status.le<std::int32_t>(3661000);
            status.fixed_string("Fixture active banner", 128);
            status.zeros(16);
            status.le<std::uint32_t>(40);
            status.le<std::uint32_t>(4);
            status.le<std::uint32_t>(1);
            status.le<std::uint32_t>(0x102030);
            status.le<std::uint32_t>(0xabcdef);
            return net::AppPacket{net::to_wire(net::Op::GuildBannerStatus), status.take()};
        };
        rig.conn.receive_zone_packet_for_test(status_packet(0));
        api.begin_frame();

        CHECK(run(L, "return hs.ChildShown(g, 'BannerData')") == "true");
        CHECK(run(L, "return (hs.ListInfo(g, list))") == "3");
        CHECK(run(L, "return hs.ListRow(g, list, 0)[2]") == "Fixture active banner");
        CHECK(run(L, "return value('TimeRemainingValueLabel')") == "01:01:01");
        CHECK(run(L, "return enabled('RemoveBannerButton')") == "false");
        CHECK(run(L, "return hs.ListClick(g, list, 0, 'left')") == "true");
        CHECK(run(L, "return enabled('RemoveBannerButton')") == "true");
        CHECK(run(L, "return hs.ListClick(g, list, 1, 'left')") == "true");
        CHECK(run(L, "return enabled('PlantBannerButton')") == "true");
        CHECK(run(L, "return enabled('RemoveBannerButton')") == "false");
        CHECK(run(L, "return click('PlantBannerButton')") == "true");
        REQUIRE(rig.until([&] {
            clock.advance(2ms);
            return MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerPlant) == 1;
        }));
        const auto plant = std::ranges::find_if(rig.server.zone_log, [](const auto& packet) {
            return packet.opcode == net::to_wire(net::Op::GuildBannerPlant);
        });
        REQUIRE(plant != rig.server.zone_log.end());
        ByteReader placement(plant->data);
        CHECK(placement.le<std::uint32_t>() == 50);
        placement.seek(68);
        CHECK(placement.f32() == -15.0f); // native plant puts server Y first, unlike ordinary spawn positions
        CHECK(placement.f32() == 40.0f);
        CHECK(placement.f32() == 4.0f);
        CHECK(placement.le<std::uint8_t>() == 1);

        REQUIRE(rig.conn.zone_info());
        const auto allowed = *rig.conn.zone_info();
        auto prohibited = allowed;
        prohibited.banner_placement = net::BannerPlacement::CannotPlace;
        rig.conn.receive_zone_packet_for_test(net::encode(prohibited));
        CHECK(run(L, "return enabled('PlantBannerButton')") == "false");
        rig.conn.receive_zone_packet_for_test(net::encode(allowed));
        CHECK(run(L, "return enabled('PlantBannerButton')") == "true");
        CHECK(run(L, "return hs.ListClick(g, list, 2, 'left')") == "true");
        CHECK(run(L, "return enabled('PlantBannerButton')") == "false");

        // The status handler @0x685670 asks again (RequestBannerKitList @0x684ad0: kit list and status requests)
        // after a mode 2 reply only while the editor is open.
        const auto requests = [&] {
            return std::pair{MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerKitRequest),
                             MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerStatusRequest)};
        };
        const auto settle = [&] {
            int frames = 0;
            rig.until([&] {
                clock.advance(2ms);
                return ++frames > 25;
            });
        };
        settle();
        const auto before = requests();
        rig.conn.receive_zone_packet_for_test(status_packet(2));
        settle();
        CHECK(requests() == before);
        CHECK(run(L, "return click('EditBannerButton')") == "true");
        rig.conn.receive_zone_packet_for_test(status_packet(2));
        REQUIRE(rig.until([&] {
            clock.advance(2ms);
            return requests() == std::pair{before.first + 1, before.second + 1};
        }));
        settle();
        CHECK(requests() == std::pair{before.first + 1, before.second + 1});
        CHECK(run(L, "return hs.ChildShown(g, 'SaveBannerSettings')") == "true");
        CHECK(run(L, "click('Layer1TextureNext'); return value('BannerBackgroundPattern')") == "0");
        CHECK(run(L, "click('CancelEditBanner'); return value('BannerBackgroundPattern')") == "4");
        CHECK(run(L, "click('EditBannerButton'); click('Layer2TextureNext'); return value('BannerEmblemPattern')") == "2");
        CHECK(run(L, "return click('Layer1ColorBtn')") == "true");
        REQUIRE(host.color_pick);
        CHECK(host.color_pick->rgb == 0x102030u);
        auto choose = std::move(host.color_pick->done);
        host.color_pick.reset();
        choose(0xff304050u);
        CHECK(run(L, "click('SaveBannerSettings'); return hs.ChildShown(g, 'SaveBannerSettings')") == "true");
        REQUIRE(rig.conn.confirmation());
        CHECK(rig.conn.confirmation()->text ==
              "You must have a: guild banner modification item to save change to banner settings.");
        const auto token_packet = [](std::uint32_t id) {
            ByteWriter token;
            token.zeros(8);
            token.le(id);
            token.zeros(4);
            token.fixed_string("Fixture modification token", 64);
            return net::AppPacket{net::to_wire(net::Op::GuildBannerModificationToken), token.take()};
        };
        // SaveBannerSettings @0x6833a0 refuses only item ID -1, naming the item; ID 0 is sent like any other.
        rig.conn.receive_zone_packet_for_test(token_packet(net::kNoBannerKit));
        CHECK(run(L, "click('SaveBannerSettings'); return hs.ChildShown(g, 'SaveBannerSettings')") == "true");
        REQUIRE(rig.conn.confirmation());
        CHECK(rig.conn.confirmation()->text ==
              "You must have a: Fixture modification token to save change to banner settings.");
        rig.conn.receive_zone_packet_for_test(token_packet(0));
        CHECK(run(L, "click('SaveBannerSettings'); return hs.ChildShown(g, 'SaveBannerSettings')") == "false");
        CHECK(run(L, "return value('BannerEmblemPattern')") == "1"); // saved draft yields to server-authoritative preview
        REQUIRE(rig.until([&] {
            clock.advance(2ms);
            return MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerSave) == 1;
        }));
        const auto save = std::ranges::find_if(rig.server.zone_log, [](const auto& packet) {
            return packet.opcode == net::to_wire(net::Op::GuildBannerSave);
        });
        REQUIRE(save != rig.server.zone_log.end());
        ByteReader style(save->data);
        style.skip(4);
        CHECK(style.le<std::uint32_t>() == 4);
        CHECK(style.le<std::uint32_t>() == 2);
        CHECK(style.le<std::uint32_t>() == 0x304050u);
        CHECK(style.le<std::uint32_t>() == 0xabcdefu);
        CHECK(style.le<std::uint32_t>() == 0);
        REQUIRE(rig.until([&] {
            clock.advance(2ms);
            return MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerPlant) == 2;
        }));
        const auto reapply = std::ranges::find_if(rig.server.zone_log.rbegin(), rig.server.zone_log.rend(),
            [](const auto& packet) { return packet.opcode == net::to_wire(net::Op::GuildBannerPlant); });
        REQUIRE(reapply != rig.server.zone_log.rend());
        ByteReader reapply_request(reapply->data);
        CHECK(reapply_request.le<std::uint32_t>() == 40);
        reapply_request.seek(80);
        CHECK(reapply_request.le<std::uint8_t>() == 0);
        CHECK(reapply_request.le<std::uint8_t>() == 1);

        CHECK(run(L, "hs.ListClick(g, list, 0, 'left'); return click('RemoveBannerButton')") == "true");
        REQUIRE(rig.until([&] {
            clock.advance(2ms);
            return MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerRemove) == 1;
        }));
        CHECK(run(L, "return (hs.ListInfo(g, list))") == "3"); // server still owns the active banner
        const auto status_requests = MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerStatusRequest);
        CHECK(run(L, "hs.ShowWindow(g, false); return hs.ChildShown(g, 'BannerData')") == "false");
        CHECK(run(L, "hs.ShowWindow(g, true); return hs.ChildShown(g, 'BannerData')") == "false");
        REQUIRE(rig.until([&] {
            clock.advance(2ms);
            return MockEqServer::count(rig.server.zone_log, net::Op::GuildBannerStatusRequest) > status_requests;
        }));
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
