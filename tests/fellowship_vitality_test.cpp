#include <doctest/doctest.h>

#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq_values.h"
#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "mock_server/profile_builder.h"
#include "net/fellowship_packets.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;

namespace {

net::AppPacket vitality(std::uint64_t normal, std::uint64_t cap, std::uint32_t aa = 0, std::uint32_t aa_cap = 0) {
    ByteWriter w;
    w.le(normal); w.le(cap); w.le(aa); w.le<std::uint32_t>(0); w.le(aa_cap);
    return net::AppPacket(net::kOpFellowshipVitality, w.take());
}

struct GaugeHost {
    test::EmptyGameDir dir;
    GameDir game{dir.path()};
    net::GameConnection conn{game, {}};
    app::EqHost host;
    GaugeHost() {
        host.game = &game;
        host.conn = &conn;
        host.in_game = true;
    }
};

std::string drawn(lua_State* L) {
    REQUIRE(luaL_dostring(L, "return table.concat(vitality_seen, '|')") == LUA_OK);
    const std::string text = lua_tostring(L, -1);
    lua_settop(L, 0);
    return text;
}

} // namespace

TEST_CASE("fellowship vitality: native gauges preserve unsigned precision and bound empty or over-cap pools") {
    GaugeHost t;
    CHECK(app::eq_gauge(t.host, 147).value == 0);
    CHECK(app::eq_gauge(t.host, 147).text.empty());
    CHECK(app::eq_gauge(t.host, 148).text.empty());
    t.conn.receive_zone_packet_for_test(vitality(9, 0, 7, 0));
    CHECK(app::eq_gauge(t.host, 147).value == 0);
    CHECK(app::eq_gauge(t.host, 147).text == "9 / 0");
    CHECK(app::eq_gauge(t.host, 148).value == 0);
    t.conn.receive_zone_packet_for_test(vitality(1, 3, 2, 3));
    CHECK(app::eq_gauge(t.host, 147).value == 333);
    CHECK(app::eq_gauge(t.host, 148).value == 666);
    t.conn.receive_zone_packet_for_test(vitality(12, 10, 5, 4));
    CHECK(app::eq_gauge(t.host, 147).value == 1000);
    CHECK(app::eq_gauge(t.host, 148).value == 1000);
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    constexpr auto max_aa = std::numeric_limits<std::uint32_t>::max();
    t.conn.receive_zone_packet_for_test(vitality(max - 1, max, max_aa - 1, max_aa));
    CHECK(app::eq_gauge(t.host, 147).value == 999);
    CHECK(app::eq_gauge(t.host, 148).value == 999);
    CHECK(app::eq_gauge(t.host, 147).text == "18446744073709551614 / 18446744073709551615");
    CHECK(app::eq_gauge(t.host, 148).text == "4294967294 / 4294967295");
    t.conn.receive_zone_packet_for_test(vitality(max / 2, max));
    CHECK(app::eq_gauge(t.host, 147).value == 499);
    t.conn.receive_zone_packet_for_test(vitality(max / 2 + 1, max));
    CHECK(app::eq_gauge(t.host, 147).value == 500);
    t.conn.receive_zone_packet_for_test(vitality(max, max, max_aa, max_aa));
    CHECK(app::eq_gauge(t.host, 147).value == 1000);
    CHECK(app::eq_gauge(t.host, 148).value == 1000);
    t.host.in_game = false;
    CHECK(app::eq_gauge(t.host, 147).value == -1);
    CHECK(app::eq_gauge(t.host, 148).value == -1);
}

TEST_CASE("fellowship vitality: profile pools and subsequent updates reach the native gauges") {
    GaugeHost t;
    test::ProfileSpec profile;
    profile.fellowship_vitality_normal = 5000000000ull;
    profile.fellowship_vitality_aa = 17;
    t.conn.receive_zone_packet_for_test(test::build_player_profile(profile));
    CHECK(app::eq_gauge(t.host, 147).text == "5000000000 / 0");
    CHECK(app::eq_gauge(t.host, 148).text == "17 / 0");
    t.conn.receive_zone_packet_for_test(vitality(7500000000ull, 10000000000ull, 25, 50));
    CHECK(app::eq_gauge(t.host, 147).value == 750);
    CHECK(app::eq_gauge(t.host, 148).value == 500);
    CHECK(app::eq_gauge(t.host, 147).text == "7500000000 / 10000000000");
    REQUIRE(t.conn.profile());
    CHECK(t.conn.profile()->fellowship_vitality_normal == 7500000000ull);
    CHECK(t.conn.profile()->fellowship_vitality_aa == 25);
    t.conn.disconnect();
    CHECK(app::eq_gauge(t.host, 147).value == 0);
    CHECK(app::eq_gauge(t.host, 147).text.empty());
    CHECK(app::eq_gauge(t.host, 148).text.empty());
}

TEST_CASE("fellowship vitality: both inventory packs draw the received pools and refresh their progress") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::Rig rig;
        REQUIRE(rig.to_zone());
        test::ScratchDir scratch("fellowship_inventory_vitality");
        const auto scripts = std::filesystem::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        app::EqHost host;
        host.game = &rig.game;
        host.conn = &rig.conn;
        host.in_game = host.ui_ready = true;
        app::EqApi api(host);
        ui::lua::LuaUi lua(scripts, scratch.path() / "ui.txt");
        lua.add_eq_api([&](lua_State* L) { api.register_functions(L); });
        api.load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI_Test_Server.ini", {});
        api.show("InventoryWindow", true);
        lua_State* L = lua.state();
        REQUIRE(luaL_dostring(L, R"(
            vitality_seen = {}
            local draw = ui.DrawText
            ui.DrawText = function(...)
                for _, value in ipairs({...}) do
                    if type(value) == 'string' then vitality_seen[#vitality_seen + 1] = value end
                end
                return draw(...)
            end
        )") == LUA_OK);
        const auto frames = [&](const ImVec2& size) {
            for (int i = 0; i < 3; ++i) {
                REQUIRE(luaL_dostring(L, "vitality_seen = {}") == LUA_OK);
                ImGui::GetIO().DisplaySize = size;
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                api.begin_frame();
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        };
        frames(ImVec2(1600, 900));
        CHECK(drawn(L).find("XP vitality") == std::string::npos); // stock server without a vitality feed
        REQUIRE(rig.conn.player_status().level == 50);
        rig.conn.receive_zone_packet_for_test(vitality(5000000000ull, 10000000000ull, 15, 60));
        frames(ImVec2(1600, 900));
        CHECK(drawn(L).find("XP vitality") != std::string::npos);
        CHECK(drawn(L).find("AA vitality") == std::string::npos); // the native inventory hides AA before level 51
        test::ProfileSpec profile;
        profile.level = 51;
        rig.conn.receive_zone_packet_for_test(test::build_player_profile(profile));
        REQUIRE(rig.conn.player_status().level == 51);
        rig.conn.receive_zone_packet_for_test(vitality(5000000000ull, 10000000000ull, 15, 60));
        for (const auto& size : {ImVec2(1600, 900), ImVec2(960, 540)}) {
            frames(size);
            const auto text = drawn(L);
            CHECK(text.find("XP vitality") != std::string::npos);
            CHECK(text.find("AA vitality") != std::string::npos);
            CHECK(text.find("50.0%") != std::string::npos);
            CHECK(text.find("25.0%") != std::string::npos);
            CHECK(text.find("5000000000 / 10000000000") != std::string::npos);
        }
        rig.conn.receive_zone_packet_for_test(vitality(2500000000ull, 10000000000ull, 30, 60));
        frames(ImVec2(1600, 900));
        CHECK(drawn(L).find("2500000000 / 10000000000") != std::string::npos);
    }
}
