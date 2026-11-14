// Stock windows over the eq API driven by packets (app/lua_eq_misc.cpp, lua_eq_player.cpp, lua_eq_tint.cpp,
// lua_eq_dialogs.cpp): the respawn window, the context tip window, the colour picker, the options window's Colors
// page, the extended target window's context menu, and the pet window's gauges for several pets.
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_hotbar.h"
#include "app/lua_eq_windows.h"
#include "core/ini_file.h"
#include "game/bazaar.h"
#include "game/hot_button_menu.h"
#include "game/hotbuttons.h"
#include "game_files.h"
#include "mock_server/harness.h"
#include "net/game_connection.h"
#include "net/group_packets.h"
#include "net/market_packets.h"
#include "net/notice_packets.h"
#include "net/opcodes.h"
#include "net/pet_packets.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
}
void put_f32(std::vector<std::byte>& b, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    put_u32(b, v);
}

// OP_RespawnWindow as EQEmu's SendRespawnBinds writes it.
net::AppPacket respawn_packet() {
    std::vector<std::byte> b;
    put_u32(b, 0);     // the default option
    put_u32(b, 60000); // time remaining
    put_u32(b, 0);     // delay
    put_u32(b, 2);
    auto option = [&](std::uint32_t id, std::uint32_t zone, const char* name, std::uint8_t type) {
        put_u32(b, id);
        put_u32(b, zone);
        for (int i = 0; i < 4; ++i) put_f32(b, 0.0f);
        for (const char* c = name; *c; ++c) b.push_back(static_cast<std::byte>(*c));
        b.push_back(std::byte{0});
        b.push_back(static_cast<std::byte>(type));
    };
    option(0, 2, "Bind Location", 0);
    option(1, 2, "Resurrect", 1);
    return net::AppPacket(net::to_wire(net::Op::RespawnWindow), std::move(b));
}

struct Harness {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch{"ui_windows_test"};
    fs::path dir = dir_scratch.path();
    net::GameConnection conn;
    app::EqHost host;
    std::unique_ptr<app::EqApi> api;
    explicit Harness(const GameDir& game, net::ConnectionOptions options = {}) : conn(game, std::move(options)) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        api = std::make_unique<app::EqApi>(host);
        api->load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
    }
    ~Harness() {
        api.reset();
        ImGui::DestroyContext(ctx);
        fs::remove_all(dir);
    }
};

} // namespace

TEST_CASE("Group role menus: self, member rows, permissions and confirmed role checks") {
    hailstone::test::ScratchDir game_scratch("ui_windows_group_roles");
    GameDir game(game_scratch.path());
    Harness t(game);
    net::PlayerProfile pp;
    pp.name = "Leader";
    t.conn.set_profile_for_test(pp);
    CHECK_FALSE(t.api->child_shown("GroupWindow", "Hailstone_RolesButton").has_value());
    CHECK_FALSE(t.api->click_child("GroupWindow", "Hailstone_RolesButton"));
    CHECK_FALSE(t.api->context_menu("PlayerWindow", 10, 20));
    CHECK_FALSE(t.api->context_menu("GroupWindow", 10, 20));

    net::GroupUpdateB group;
    group.leader = "Leader";
    net::GroupUpdateMember self;
    self.name = "Leader";
    self.slot = 0;
    self.main_tank = 1;
    group.members.push_back(self);
    net::GroupUpdateMember member;
    member.name = "Member";
    member.slot = 1;
    member.main_assist = 1;
    group.members.push_back(member);
    net::GroupUpdateMember merc;
    merc.name = "Mercenary";
    merc.slot = 2;
    merc.merc = 1;
    group.members.push_back(merc);
    t.conn.receive_zone_packet_for_test(net::encode(group));

    // There is no roster button or background role menu. A member's name is the entry point.
    CHECK_FALSE(t.api->child_shown("GroupWindow", "Hailstone_RolesButton").has_value());
    CHECK_FALSE(t.api->click_child("GroupWindow", "Hailstone_RolesButton"));
    CHECK_FALSE(t.api->context_menu("GroupWindow", 20, 30));
    CHECK_FALSE(t.api->context_menu("PlayerWindow", 20, 30));
    CHECK(t.host.menus.empty());
    t.host.click_rect = {20, 30, 80, 50}; // a pad right click anchors to the member's item
    REQUIRE(t.api->click_child("GroupWindow", "Gauge1", "right"));
    REQUIRE(t.host.menus.size() == 1);
    CHECK(t.host.menus[0].x == 80);
    CHECK(t.host.menus[0].y == 40);
    REQUIRE(t.host.menus[0].rows.size() == 5);
    CHECK(t.host.menus[0].rows[0].text == "Member");

    // The player's own name offers the same roles even though the group gauges omit self.
    REQUIRE(t.api->click_child("PlayerWindow", "PlayerHP", "right"));
    REQUIRE(t.host.menus.size() == 1);
    CHECK(t.host.menus[0].rows[0].text == "Leader");
    CHECK(t.host.menus[0].rows[1].check);
    CHECK_FALSE(t.host.menus[0].rows[2].check);
    CHECK(t.host.menus[0].rows[1].enabled);
    CHECK(t.host.menus[0].rows[2].enabled);
    CHECK(t.host.menus[0].rows[3].enabled);
    CHECK(t.api->child_shown("PlayerWindow", "GroupRoleTank").value_or(false));

    REQUIRE(t.api->click_child("GroupWindow", "Gauge1", "right"));
    REQUIRE(t.host.menus.size() == 1);
    const auto& member_rows = t.host.menus[0].rows;
    REQUIRE(member_rows.size() == 5);
    CHECK(member_rows[0].text == "Member");
    CHECK_FALSE(member_rows[1].check);
    CHECK(member_rows[2].check);
    CHECK(t.api->child_shown("GroupWindow", "GroupRoleAssist1").value_or(false));
    // Heading, the three roles and the zone note: no "Window" row (the gamepad pack has no window menu for it; the
    // modern frame menu is the rest of the frame's right click).
    CHECK(member_rows[4].text == "Assign in the same zone; clear roles from any zone");
    CHECK_FALSE(member_rows[4].enabled);
    for (const auto& row : member_rows) CHECK(row.text != "Window");

    REQUIRE(t.api->click_child("GroupWindow", "Gauge2", "right"));
    REQUIRE(t.host.menus.size() == 1);
    CHECK(t.host.menus[0].rows[0].text == "Mercenary");
    CHECK(t.host.menus[0].rows[1].enabled); // mercenaries may tank, but cannot assist or pull
    CHECK_FALSE(t.host.menus[0].rows[2].enabled);
    CHECK_FALSE(t.host.menus[0].rows[3].enabled);

    // A server refresh changes the checks and the authority, without a local optimistic role assignment.
    group.leader = "Member";
    group.members[1].main_assist = 0;
    group.members[1].puller = 1;
    t.conn.receive_zone_packet_for_test(net::encode(group));
    REQUIRE(t.api->click_child("GroupWindow", "Gauge1", "right"));
    CHECK_FALSE(t.host.menus[0].rows[2].check);
    CHECK(t.host.menus[0].rows[3].check);
    CHECK_FALSE(t.host.menus[0].rows[1].enabled);
    CHECK_FALSE(t.host.menus[0].rows[2].enabled);
    CHECK_FALSE(t.host.menus[0].rows[3].enabled);
    CHECK_FALSE(t.api->menu_click(0, 1));

    t.host.menus.clear();
    t.conn.set_client_option_for_session(game::ClientOption::ContextMenus, 0);
    CHECK_FALSE(t.api->context_menu("GroupWindow", 20, 30));
    CHECK(t.api->click_child("GroupWindow", "Gauge1", "right"));
    CHECK(t.host.menus.empty());
    CHECK(t.api->click_child("PlayerWindow", "PlayerHP", "right"));
    CHECK(t.host.menus.empty());
}

TEST_CASE("Group role icons: player and member flags follow confirmed assignments and disband") {
    test::ScratchDir game_scratch("ui_windows_group_role_icons");
    GameDir game(game_scratch.path());
    Harness t(game);
    net::PlayerProfile pp;
    pp.name = "Leader";
    t.conn.set_profile_for_test(pp);
    const std::array<const char*, 3> roles = {"GroupRoleTank", "GroupRoleAssist", "GroupRolePuller"};
    for (const char* role : roles) {
        CHECK_FALSE(t.api->child_shown("PlayerWindow", role).value_or(true));
        CHECK_FALSE(t.api->child_shown("GroupWindow", std::string(role) + "1").value_or(true));
    }

    net::GroupUpdateB group;
    group.leader = "Leader";
    net::GroupUpdateMember self;
    self.name = "Leader";
    self.slot = 0;
    self.main_tank = self.main_assist = self.puller = 1;
    group.members.push_back(self);
    net::GroupUpdateMember member;
    member.name = "Member";
    member.slot = 1;
    group.members.push_back(member);
    t.conn.receive_zone_packet_for_test(net::encode(group));

    for (std::size_t i = 0; i < roles.size(); ++i) {
        CHECK(t.api->child_shown("PlayerWindow", roles[i]).value_or(false));
        CHECK_FALSE(t.api->child_shown("GroupWindow", std::string(roles[i]) + "1").value_or(true));
    }

    // A confirmed reassignment removes the player's icon and adds the member's matching icon.
    for (int role = 1; role <= 3; ++role) {
        net::GroupRole change;
        change.leader = "Leader";
        change.member = "Member";
        change.role = role;
        change.on = 1;
        t.conn.receive_zone_packet_for_test(net::encode_group_role(change));
        const auto index = static_cast<std::size_t>(role - 1);
        CHECK_FALSE(t.api->child_shown("PlayerWindow", roles[index]).value_or(true));
        CHECK(t.api->child_shown("GroupWindow", std::string(roles[index]) + "1").value_or(false));
    }

    t.conn.receive_zone_packet_for_test(net::encode_group_generic(net::Op::GroupDisbandYou, {"Leader", "Member"}));
    for (const char* role : roles) {
        CHECK_FALSE(t.api->child_shown("PlayerWindow", role).value_or(true));
        CHECK_FALSE(t.api->child_shown("GroupWindow", std::string(role) + "1").value_or(true));
    }
    t.host.menus.clear();
    CHECK_FALSE(t.api->click_child("PlayerWindow", "PlayerHP", "right"));
    CHECK(t.host.menus.empty());
}

TEST_CASE("Group role UI packs: icons beside fitted names and name right clicks in player and group windows") {
    for (const std::string pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_windows_group_role_pack");
        GameDir game(scratch.path());
        Harness t(game);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 900);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const auto source = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        ui::lua::LuaUi lua(source, scratch.path() / "pack.txt");
        lua.set_entry_scripts(pack == "modern" ? std::vector<std::string>{"player.lua", "group.lua"}
                                                : std::vector<std::string>{"hud.lua", "group.lua"});
        lua.add_eq_api([&](lua_State* state) {
            t.api->register_functions(state);
            lua_pushcfunction(state, [](lua_State* L) { lua_pushboolean(L, true); return 1; });
            lua_setfield(state, -2, "InGame");
        });
        // Observe the real shipped helper and text drawing without substituting either renderer.
        REQUIRE(lua.run_for_test("", R"lua(
          partyRoleDraws, partyNameRects, partyRoleTips = {}, {}, {}
          local register, load, text, tooltip = ui.window, dofile, ui.DrawText, ui.SetTooltip
          ui.window = function(name, fn)
            return register(name, function(...)
              partyRoleWindow = name
              fn(...)
              partyRoleWindow = nil
            end)
          end
          dofile = function(path)
            local module = load(path)
            if path == 'lib/role_icons.lua' then
              local draw = module.Draw
              module.Draw = function(mask, x, y, size, gap, colors)
                if mask > 0 then partyRoleDraws[partyRoleWindow] = {mask, x, y, size, module.Width(mask, size, gap)} end
                return draw(mask, x, y, size, gap, colors)
              end
            end
            return module
          end
          ui.DrawText = function(x, y, r, g, b, a, value, ...)
            if value:match('^Leader') or value:match('^Member') then
              local w, h = ui.CalcTextSize(value)
              partyNameRects[partyRoleWindow] = {x, y, w, h}
            end
            return text(x, y, r, g, b, a, value, ...)
          end
          ui.SetTooltip = function(value)
            partyRoleTips[value] = true
            return tooltip(value)
          end
          return true
        )lua") == "true");

        net::PlayerProfile pp;
        pp.name = "LeaderWithAnExtremelyLongNameForRoleLayout";
        t.conn.set_profile_for_test(pp);
        net::GroupUpdateB group;
        group.leader = pp.name;
        net::GroupUpdateMember self;
        self.name = pp.name;
        self.main_tank = self.main_assist = self.puller = 1;
        group.members.push_back(self);
        net::GroupUpdateMember member;
        member.name = "MemberWithAnExtremelyLongNameForRoleLayout";
        member.slot = 1;
        group.members.push_back(member);
        t.conn.receive_zone_packet_for_test(net::encode(group));
        t.api->show("PlayerWindow", true);
        t.api->show("GroupWindow", true);
        const auto frames = [&](int count) {
            for (int i = 0; i < count; ++i) {
                REQUIRE(lua.run_for_test("", "partyRoleDraws,partyNameRects,partyRoleTips={}, {}, {};return true") == "true");
                io.DeltaTime = 1.0f / 60.0f;
                t.api->begin_frame();
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
        };
        const auto record = [&](const char* table, const std::string& window, int fields) {
            std::vector<float> values;
            lua_State* state = lua.state();
            lua_getglobal(state, table);
            lua_getfield(state, -1, window.c_str());
            if (lua_istable(state, -1)) {
                for (int i = 1; i <= fields; ++i) {
                    lua_rawgeti(state, -1, i);
                    values.push_back(static_cast<float>(lua_tonumber(state, -1)));
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 2);
            return values;
        };
        const auto check_icons = [&](const std::string& window) {
            INFO(window);
            const auto icons = record("partyRoleDraws", window, 5);
            const auto name = record("partyNameRects", window, 4);
            REQUIRE(icons.size() == 5);
            REQUIRE(name.size() == 4);
            CHECK(icons[0] == 7); // all three confirmed roles draw on the same name row
            CHECK(icons[4] > icons[3] * 3);
            CHECK(icons[1] >= name[0] + name[2]);
            CHECK(std::abs((icons[2] + icons[3] / 2) - (name[1] + name[3] / 2)) < 2);
            const std::string imgui_name = pack == "modern" ? "###modern." + window + "Window" : window;
            const auto* drawn = ImGui::FindWindowByName(imgui_name.c_str());
            REQUIRE(drawn);
            CHECK(icons[1] >= drawn->Pos.x);
            CHECK(icons[1] + icons[4] <= drawn->Pos.x + drawn->Size.x);
            const float gap = (icons[4] - 3 * icons[3]) / 2;
            const std::array<const char*, 3> labels = {"Main Tank", "Main Assist", "Puller"};
            for (std::size_t role = 0; role < labels.size(); ++role) {
                io.AddMousePosEvent(icons[1] + static_cast<float>(role) * (icons[3] + gap) + icons[3] / 2,
                                   icons[2] + icons[3] / 2);
                frames(2);
                CHECK(lua.run_for_test("", "return partyRoleTips['" + std::string(labels[role]) + "']") == "true");
            }
            return ImVec2(icons[1] + icons[3] / 2, icons[2] + icons[3] / 2);
        };
        const auto right_click = [&](ImVec2 at, const std::string& expected_name) {
            t.host.menus.clear();
            io.AddMousePosEvent(at.x, at.y);
            frames(2);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            frames(1);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
            frames(1);
            REQUIRE(t.host.menus.size() == 1);
            REQUIRE(t.host.menus[0].rows.size() == 5);
            CHECK(t.host.menus[0].x == doctest::Approx(std::floor(at.x)));
            CHECK(t.host.menus[0].y == doctest::Approx(std::floor(at.y)));
            CHECK(t.host.menus[0].rows[0].text == expected_name);
        };
        frames(3);
        const auto player = check_icons(pack == "modern" ? "Player" : "HUD Player");
        right_click(player, pp.name);

        group.members[0].main_tank = group.members[0].main_assist = group.members[0].puller = 0;
        group.members[1].main_tank = group.members[1].main_assist = group.members[1].puller = 1;
        t.conn.receive_zone_packet_for_test(net::encode(group));
        frames(2);
        const auto group_name = check_icons(pack == "modern" ? "Group" : "HUD Group");
        right_click(group_name, member.name);
        if (pack == "gamepad") {
            REQUIRE(lua.run_for_test("", "GPSTATE.groupOpen=true;return true") == "true");
            frames(3);
            const auto dialog_name = check_icons("Group");
            right_click(dialog_name, member.name);
        }
    }
}

namespace {

// A UI pack's scripts over a Harness, for the group role tests below: frames run as the client runs them.
struct RolePackRig {
    test::ScratchDir scratch;
    GameDir game;
    Harness t;
    std::unique_ptr<ui::lua::LuaUi> lua;
    RolePackRig(const std::string& pack, std::vector<std::string> scripts)
        : scratch("ui_windows_group_role_rig"), game(scratch.path()), t(game) {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 900);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const auto source = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        lua = std::make_unique<ui::lua::LuaUi>(source, scratch.path() / "pack.txt");
        lua->set_entry_scripts(std::move(scripts));
        lua->add_eq_api([this](lua_State* state) {
            t.api->register_functions(state);
            lua_pushcfunction(state, [](lua_State* L) { lua_pushboolean(L, true); return 1; });
            lua_setfield(state, -2, "InGame");
        });
    }
    ~RolePackRig() { lua.reset(); }
    // `before` runs between ImGui's NewFrame and the scripts (a pad focus put on an item, say).
    void frames(int count, const std::function<void()>& before = {}) {
        for (int i = 0; i < count; ++i) {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            t.api->begin_frame();
            ImGui::NewFrame();
            if (before) before();
            lua->frame();
            ImGui::Render();
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
    }
    std::string run(const std::string& code) { return lua->run_for_test("", code); }
    void group(const std::string& self) {
        net::PlayerProfile pp;
        pp.name = self;
        t.conn.set_profile_for_test(pp);
        net::GroupUpdateB g;
        g.leader = self;
        net::GroupUpdateMember me;
        me.name = self;
        g.members.push_back(me);
        net::GroupUpdateMember member;
        member.name = "Member";
        member.slot = 1;
        g.members.push_back(member);
        t.conn.receive_zone_packet_for_test(net::encode(g));
    }
};

// The screen rectangle the script hook below saved for the drawn text starting with `prefix` (empty when none).
std::vector<float> drawn_text_rect(RolePackRig& r) {
    std::vector<float> v;
    lua_State* L = r.lua->state();
    lua_getglobal(L, "roleTextRect");
    if (lua_istable(L, -1))
        for (int i = 1; i <= 4; ++i) {
            lua_rawgeti(L, -1, i);
            v.push_back(static_cast<float>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    lua_pop(L, 1);
    return v;
}

constexpr const char* kTextRectHook = R"lua(
  roleTextPrefix, roleTextRect, rolePopups = nil, nil, {}
  local text, popup = ui.DrawText, ui.OpenPopup
  ui.DrawText = function(x, y, r, g, b, a, value, ...)
    if roleTextPrefix and type(value) == 'string' and value:sub(1, #roleTextPrefix) == roleTextPrefix then
      local w, h = ui.CalcTextSize(value)
      roleTextRect = {x, y, w, h}
    end
    return text(x, y, r, g, b, a, value, ...)
  end
  ui.OpenPopup = function(id, ...)
    rolePopups[id] = true
    return popup(id, ...)
  end
  return true
)lua";

} // namespace

TEST_CASE("Group role UI packs: the modern Player window's name keeps the frame menu when no role menu opens") {
    RolePackRig r("modern", {"player.lua"});
    REQUIRE(r.run(kTextRectHook) == "true");
    net::PlayerProfile pp;
    pp.name = "Soloist";
    r.t.conn.set_profile_for_test(pp);
    r.t.api->show("PlayerWindow", true);
    REQUIRE(r.run("roleTextPrefix='Soloist';return true") == "true");
    auto& io = ImGui::GetIO();
    r.frames(3);
    const auto name = drawn_text_rect(r);
    REQUIRE(name.size() == 4);
    const ImVec2 at(name[0] + name[2] / 2, name[1] + name[3] / 2);
    const auto right_click = [&](bool alt) {
        // (a left click on empty screen first shuts a popup the last click opened)
        io.AddMousePosEvent(1590, 890);
        r.frames(1);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        r.frames(1);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        r.frames(1);
        r.t.host.menus.clear();
        REQUIRE(r.run("rolePopups={};return true") == "true");
        io.AddKeyEvent(ImGuiMod_Alt, alt);
        io.AddMousePosEvent(at.x, at.y);
        r.frames(2);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        r.frames(1);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        r.frames(1);
        io.AddKeyEvent(ImGuiMod_Alt, false);
        r.frames(1);
    };

    // Solo there is no role menu: the click is the frame's, and its layout menu opens as anywhere else on it.
    right_click(false);
    CHECK(r.t.host.menus.empty());
    CHECK(r.run("return rolePopups['##hudmenu'] == true") == "true");

    // In a group the name opens the role menu, and the frame menu stays shut.
    r.group("Soloist");
    r.frames(2);
    right_click(false);
    REQUIRE(r.t.host.menus.size() == 1);
    CHECK(r.t.host.menus[0].rows[0].text == "Soloist");
    CHECK(r.run("return rolePopups['##hudmenu'] == true") == "false");

    // Alt with the right click is the frame's menu, in a group too.
    right_click(true);
    CHECK(r.t.host.menus.empty());
    CHECK(r.run("return rolePopups['##hudmenu'] == true") == "true");
}

TEST_CASE("Group role UI packs: the gamepad HUD player panel takes the mouse only on its name row in a group") {
    RolePackRig r("gamepad", {"hud.lua"});
    net::PlayerProfile pp;
    pp.name = "Soloist";
    r.t.conn.set_profile_for_test(pp);
    r.t.api->show("PlayerWindow", true);
    r.frames(3);
    const auto* panel = ImGui::FindWindowByName("HUD Player");
    REQUIRE(panel);
    CHECK((panel->Flags & ImGuiWindowFlags_NoInputs) != 0);
    const auto* row = ImGui::FindWindowByName("HUD Player Name");
    CHECK((row == nullptr || !row->Active)); // solo the name does nothing, so nothing takes the mouse

    r.group("Soloist");
    r.frames(3);
    panel = ImGui::FindWindowByName("HUD Player");
    REQUIRE(panel);
    CHECK((panel->Flags & ImGuiWindowFlags_NoInputs) != 0);
    row = ImGui::FindWindowByName("HUD Player Name");
    REQUIRE(row);
    REQUIRE(row->Active);
    CHECK(row->Size.y < panel->Size.y / 3);
    CHECK(row->Pos.y >= panel->Pos.y);
    CHECK(row->Pos.y + row->Size.y <= panel->Pos.y + panel->Size.y / 2);

    // Over the gauges (the panel's lower half) no window of the UI takes the mouse; over the name the row does.
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(panel->Pos.x + panel->Size.x / 2, panel->Pos.y + panel->Size.y * 0.75f);
    r.frames(2);
    CHECK(ImGui::GetCurrentContext()->HoveredWindow == nullptr);
    CHECK_FALSE(io.WantCaptureMouse);
    const ImVec2 at(row->Pos.x + 4, row->Pos.y + row->Size.y / 2);
    io.AddMousePosEvent(at.x, at.y);
    r.frames(2);
    CHECK(ImGui::GetCurrentContext()->HoveredWindow == ImGui::FindWindowByName("HUD Player Name"));
    r.t.host.menus.clear();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    r.frames(1);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    r.frames(1);
    REQUIRE(r.t.host.menus.size() == 1);
    CHECK(r.t.host.menus[0].rows[0].text == "Soloist");
}

TEST_CASE("Group role UI packs: Y on a gamepad Group window row opens that member's role menu with the pad's focus") {
    RolePackRig r("gamepad", {"group.lua", "menus.lua"});
    // A pad in menu mode; padPress is the button that went down this frame.
    REQUIRE(r.run(R"lua(
      padPress = nil
      eq.gamepad = {
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
      return true
    )lua") == "true");
    r.group("Leader");
    r.frames(1);
    REQUIRE(r.run("GPSTATE.groupOpen = true;return true") == "true");
    r.frames(3);
    auto* window = ImGui::FindWindowByName("Group");
    REQUIRE(window);
    const auto press_y_on = [&](const char* id) {
        r.t.host.menus.clear();
        const ImGuiID item = window->GetID(id);
        const auto focus = [&] {
            ImGui::SetWindowFocus("Group");
            ImGui::SetFocusID(item, window);
        };
        r.frames(1, focus);
        REQUIRE(r.run("padPress='y';return true") == "true");
        r.frames(1, focus);
        REQUIRE(r.run("padPress=nil;return true") == "true");
        r.frames(2);
    };

    press_y_on("##m1");
    REQUIRE(r.t.host.menus.size() == 1);
    CHECK(r.t.host.menus[0].rows[0].text == "Member");
    CHECK(r.t.host.menus[0].rows[1].enabled); // the leader may give roles
    CHECK(r.t.conn.world().target_id == 0);   // Y is not A: the member is not targeted
    // menus.lua draws it with the pad's focus, so A and the D-pad work it as any game menu
    const auto* menu = ImGui::FindWindowByName("##gamemenu1");
    REQUIRE(menu);
    CHECK(menu->Active);
    REQUIRE(ImGui::GetCurrentContext()->NavWindow);
    CHECK(ImGui::GetCurrentContext()->NavWindow->RootWindow == menu);

    // The player's own row (first in a group) opens the player's role menu.
    press_y_on("##self");
    REQUIRE(r.t.host.menus.size() == 1);
    CHECK(r.t.host.menus[0].rows[0].text == "Leader");
}

TEST_CASE("Group role menus: a server update while open preserves the displayed toggle intent") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    Harness t(rig.game);
    t.host.conn = &rig.conn;
    net::GroupUpdateB group;
    group.leader = "Gennro";
    net::GroupUpdateMember self;
    self.name = "Gennro";
    self.slot = 0;
    group.members.push_back(self);
    net::GroupUpdateMember member;
    member.name = "Tank";
    member.slot = 1;
    group.members.push_back(member);
    rig.server.zone.send_reliable(net::encode(group));
    REQUIRE(rig.until([&] { return rig.conn.group().count() == 2; }));
    auto sent_roles = [&] {
        std::vector<net::GroupRole> roles;
        for (const auto& packet : rig.server.zone_log)
            if (packet.opcode == net::to_wire(net::Op::GroupRoles))
                if (auto role = net::decode_group_role(packet)) roles.push_back(*role);
        return roles;
    };

    REQUIRE(t.api->click_child("GroupWindow", "Gauge1", "right"));
    REQUIRE(t.host.menus.size() == 1);
    CHECK_FALSE(t.host.menus[0].rows[2].check); // Main Assist was unchecked when the menu opened
    net::GroupRole change;
    change.member = "Tank";
    change.leader = "Gennro";
    change.role = 2;
    change.on = 1;
    rig.server.zone.send_reliable(net::encode_group_role(change));
    REQUIRE(rig.until([&] { return rig.conn.group().slots[1].main_assist; }));
    REQUIRE(t.api->menu_click(0, 2));
    REQUIRE(rig.until([&] { return sent_roles().size() == 1; }));
    CHECK(sent_roles()[0].member == "Tank");
    CHECK(sent_roles()[0].role == 2);
    CHECK(sent_roles()[0].on == 1); // the stale unchecked menu still requests assignment

    REQUIRE(t.api->click_child("GroupWindow", "Gauge1", "right"));
    CHECK(t.host.menus[0].rows[2].check); // this time the menu offers to clear Main Assist
    change.on = 0;
    rig.server.zone.send_reliable(net::encode_group_role(change));
    REQUIRE(rig.until([&] { return !rig.conn.group().slots[1].main_assist; }));
    REQUIRE(t.api->menu_click(0, 2));
    REQUIRE(rig.until([&] { return sent_roles().size() == 2; }));
    CHECK(sent_roles()[1].on == 0);
    CHECK_FALSE(rig.conn.group().slots[1].main_assist);
}

TEST_CASE("OP_PotionBelt: the right button held") {
    auto p = net::encode_potion_belt_hold(2, true);
    REQUIRE(p.data.size() == 12);
    std::uint32_t v[3];
    std::memcpy(v, p.data.data(), 12);
    CHECK(v[0] == 3);
    CHECK(v[1] == 2);
    CHECK(v[2] == 2);
}

TEST_CASE("OP_Bandolier: the right button held on a set's slot") {
    auto p = net::encode_bandolier_hold(5, 3, true);
    REQUIRE(p.data.size() == 12);
    std::uint32_t action = 0, flags = 0;
    std::memcpy(&action, p.data.data(), 4);
    std::memcpy(&flags, p.data.data() + 8, 4);
    CHECK(action == 3);
    CHECK(std::to_integer<int>(p.data[4]) == 5);
    CHECK(std::to_integer<int>(p.data[5]) == 3);
    CHECK(flags == 2);
}

TEST_CASE("Respawn window: the options, the default row and Select") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    CHECK_FALSE(t.api->shown("RespawnWnd"));
    t.conn.receive_zone_packet_for_test(respawn_packet());
    REQUIRE(t.api->shown("RespawnWnd"));
    CHECK(t.conn.respawn_window()->options.size() == 2);
    auto markers = t.api->list_texts("RespawnWnd", "OptionsList");
    REQUIRE(markers.size() == 2);
    CHECK(markers[0] == "*"); // the default option
    CHECK(markers[1] == "r"); // a resurrection
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Select: older than the delay (0)
    CHECK(t.conn.respawn_ready());
    CHECK(t.api->click_list("RespawnWnd", "OptionsList", 1, 0));
    CHECK(t.api->click_child("RespawnWnd", "SelectButton"));
    CHECK(t.conn.respawn_chosen());
}

TEST_CASE("Tip window: the duel tip, its history and the show options") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    CHECK_FALSE(t.api->shown("TipWindow"));
    CHECK(t.api->activate_tip(26));
    CHECK(t.api->shown("TipWindow"));
    CHECK(t.api->click_child("TipWindow", "CloseButton"));
    CHECK_FALSE(t.api->shown("TipWindow"));
    CHECK(t.api->activate_tip(31));
    CHECK(t.api->click_child("TipWindow", "NoShowThisButton")); // death tip off
    CHECK(t.api->click_child("TipWindow", "CloseButton"));
    CHECK_FALSE(t.api->activate_tip(31));
    CHECK(t.api->activate_tip(26));
    CHECK(t.api->click_child("TipWindow", "NoShowButton")); // no tips at all
    CHECK_FALSE(t.api->activate_tip(26));
    CHECK_FALSE(t.api->activate_tip(0x5b));
}

TEST_CASE("Social editor: 0x758bb0 fills it, a colour button, Clear, Accept writes the social") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    game::HotButtonStore store;
    t.host.hot_buttons = &store;
    game::Social& s = store.social(13); // page 2, button 2
    s.name = "Wave";
    s.color = 2;
    s.lines[0] = "/wave";
    s.cooldown_length = 5000;
    REQUIRE(t.host.open_social_editor);
    t.host.open_social_editor(13);
    REQUIRE(t.api->shown("SocialEditWnd"));
    CHECK(t.api->child_text("SocialEditWnd", "SEW_NameInput") == "Wave");
    CHECK(t.api->child_text("SocialEditWnd", "SEW_Line0Input") == "/wave");
    // Nothing reaches the social before Accept.
    CHECK(t.api->set_child_text("SocialEditWnd", "SEW_NameInput", "A name longer than fifteen"));
    CHECK(t.api->set_child_text("SocialEditWnd", "SEW_Line4Input", "/pause 5, /bow"));
    CHECK(s.name == "Wave");
    CHECK(t.api->click_child("SocialEditWnd", "SEW_TC13Button"));
    CHECK(t.api->click_child("SocialEditWnd", "SEW_Accept_Button"));
    CHECK_FALSE(t.api->shown("SocialEditWnd"));
    CHECK(s.name == "A name longer t");
    CHECK(s.lines[0] == "/wave");
    CHECK(s.lines[4] == "/pause 5, /bow");
    CHECK(s.color == 13);
    CHECK(s.cooldown_length == 0);
    // Clear empties the name and the lines; closing without Accept leaves the social as it was.
    t.host.open_social_editor(13);
    CHECK(t.api->click_child("SocialEditWnd", "SEW_Clear_Button"));
    CHECK(t.api->child_text("SocialEditWnd", "SEW_NameInput") == "");
    CHECK(t.api->child_text("SocialEditWnd", "SEW_Line4Input") == "");
    t.api->show("SocialEditWnd", false);
    CHECK(s.name == "A name longer t");
    // The Actions window's socials page: the name in the social's colour, and a right click opens the editor.
    CHECK(app::social_text_color(13) == app::Rgb{240 / 255.0f, 0, 0});
    CHECK(app::social_text_color(20) == app::Rgb{0x60 / 255.0f, 0x60 / 255.0f, 0x60 / 255.0f});
}

TEST_CASE("Hotbutton menu: 0x5536b0's clicks and the rows CHotButton_UpdateContextMenu shows") {
    using A = game::HotButtonMouseAction;
    using M = game::HotButtonMouse;
    game::HotButton empty, social, item, krono, gem;
    social.type = game::HotButtonType::Social;
    item.type = game::HotButtonType::InventorySlot;
    krono.type = game::HotButtonType::KronoSlot;
    gem.type = game::HotButtonType::SpellGem;
    // an empty button's left or quick right click opens the menu, unless something is on the cursor
    CHECK(game::hot_button_mouse(empty, M::LeftClick, false) == A::Menu);
    CHECK(game::hot_button_mouse(empty, M::RightClick, false) == A::Menu);
    CHECK(game::hot_button_mouse(empty, M::LeftClick, true) == A::Drop);
    CHECK(game::hot_button_mouse(empty, M::RightClick, true) == A::RightClick);
    CHECK(game::hot_button_mouse(social, M::LeftClick, false) == A::Press);
    CHECK(game::hot_button_mouse(social, M::RightClick, false) == A::RightClick);
    CHECK(game::hot_button_mouse(item, M::RightClick, false) == A::None);
    CHECK(game::hot_button_mouse(social, M::RightHeld, false) == A::Menu);
    CHECK(game::hot_button_mouse(empty, M::RightHeld, true) == A::Menu);
    auto rows = game::hot_button_menu_rows(empty, false);
    CHECK_FALSE(rows.activate);
    CHECK_FALSE(rows.clear);
    CHECK_FALSE(rows.assign_label);
    CHECK(rows.assign_social);
    CHECK(rows.change_size);
    rows = game::hot_button_menu_rows(krono, false);
    CHECK_FALSE(rows.activate);
    CHECK(rows.consume);
    CHECK(rows.inspect);
    rows = game::hot_button_menu_rows(gem, false);
    CHECK(rows.memorize);
    CHECK_FALSE(rows.unmemorize);
    CHECK_FALSE(rows.inspect);
    CHECK_FALSE(rows.clear_icon);
    gem.icon_type = 3;
    rows = game::hot_button_menu_rows(gem, true);
    CHECK_FALSE(rows.memorize);
    CHECK(rows.unmemorize);
    CHECK(rows.inspect);
    CHECK(rows.clear_icon);
    CHECK_FALSE(game::hot_button_menu_rows(gem, true, false).unmemorize); // no cast spell window

    const GameDir* g = test::game_dir();
    if (!g) return;
    Harness t(*g);
    game::HotButtonStore store;
    t.host.hot_buttons = &store;
    t.host.set_hot_button = [&](int bar, int button, const game::HotButton& b) { store.current(bar, button) = b; };
    int pressed = -1;
    bool mouse = true;
    t.host.press_hot_button = [&](int, int button, bool m) { pressed = button, mouse = m; };
    t.host.hot_button_label = [&](int bar, int button) { return store.current(bar, button).label; };
    // A right click on an empty button: Change Size and Assign Social only.
    store.social(0).name = "Wave";
    CHECK(t.api->click_hot_button("HotButtonWnd", "HB_Button4", "right"));
    REQUIRE(t.host.menus.size() == 1);
    REQUIRE(t.host.menus[0].rows.size() == 2);
    CHECK(t.host.menus[0].rows[0].text == "Change Size");
    CHECK(t.host.menus[0].rows[1].text == "Assign Social");
    CHECK(t.api->menu_click(0, 1)); // Page 1 .. Page 10
    REQUIRE(t.host.menus.size() == 2);
    REQUIRE(t.host.menus[1].rows.size() == 10);
    CHECK(t.host.menus[1].rows[1].text == "Page 2");
    CHECK(t.api->menu_click(1, 0));
    REQUIRE(t.host.menus.size() == 3);
    REQUIRE(t.host.menus[2].rows.size() == 12);
    CHECK(t.host.menus[2].rows[0].text == "Wave");
    CHECK(t.host.menus[2].rows[1].text == "<New Social>");
    // a new social: the button holds it and the social editor opens on it
    CHECK(t.api->menu_click(2, 1));
    CHECK(store.current(0, 3).type == game::HotButtonType::Social);
    CHECK(store.current(0, 3).slot == 1);
    CHECK(t.api->shown("SocialEditWnd"));
    CHECK(t.api->set_child_text("SocialEditWnd", "SEW_NameInput", "Hail"));
    CHECK(t.api->set_child_text("SocialEditWnd", "SEW_Line0Input", "/say Hail, %T"));
    CHECK(t.api->click_child("SocialEditWnd", "SEW_Accept_Button"));
    CHECK(store.social(1).name == "Hail");
    // a held right button on it: Activate, Clear, Change Size, Assign Icon, Assign Label, Assign Social
    CHECK(t.api->click_hot_button("HotButtonWnd", "HB_Button4", "rightheld"));
    REQUIRE(t.host.menus.size() == 1);
    REQUIRE(t.host.menus[0].rows.size() == 6);
    CHECK(t.host.menus[0].rows[0].text == "Activate");
    CHECK(t.host.menus[0].rows[2].text == "Change Size");
    CHECK(t.host.menus[0].rows[3].text == "Assign Icon");
    CHECK(t.host.menus[0].rows[4].text == "Assign Label");
    CHECK(t.api->menu_click(0, 0));
    CHECK(pressed == 3);
    CHECK_FALSE(mouse); // DoHotButton(n, 0)
    // Assign Label on a social renames the social
    CHECK(t.api->click_hot_button("HotButtonWnd", "HB_Button4", "rightheld"));
    CHECK(t.api->menu_click(0, 4));
    REQUIRE(t.host.text_entry);
    CHECK(t.host.text_entry->prompt == "Enter label:");
    CHECK(t.host.text_entry->max_chars == 15);
    t.host.text_entry->done("Greet");
    CHECK(store.social(1).name == "Greet");
    // a left click on it runs it; a quick right click is 0x54fc10's (the social editor again)
    pressed = -1;
    CHECK(t.api->click_hot_button("HotButtonWnd", "HB_Button4", "left"));
    CHECK(pressed == 3);
    CHECK(mouse);
    // Clear empties it
    CHECK(t.api->click_hot_button("HotButtonWnd", "HB_Button4", "rightheld"));
    CHECK(t.api->menu_click(0, 1));
    CHECK(store.current(0, 3).empty());
    // a label for another kind of button goes on the button
    game::HotButton who;
    who.type = game::HotButtonType::MenuButton;
    store.current(0, 5) = who;
    CHECK(t.api->click_hot_button("HotButtonWnd", "HB_Button6", "rightheld"));
    CHECK(t.api->menu_click(0, 4));
    REQUIRE(t.host.text_entry);
    t.host.text_entry->done("Who all");
    CHECK(store.current(0, 5).label == "Who all");
    CHECK(store.current(0, 5).type == game::HotButtonType::MenuButton);
}

TEST_CASE("Hot bar: Change Size, Assign Icon, the bar's menu, its settings and key map") {
    const GameDir* g = test::game_dir();
    if (!g) return;
    Harness t(*g);
    game::HotButtonStore store;
    t.host.hot_buttons = &store;
    t.host.set_hot_button = [&](int bar, int button, const game::HotButton& b) { store.current(bar, button) = b; };
    game::HotButton who;
    who.type = game::HotButtonType::MenuButton;
    store.current(1, 0) = who;
    // Change Size: the bar's CQuantityWnd 40..175 from PercentSize (100); its answer is the bar's PercentSize
    CHECK(t.api->click_hot_button("HotButtonWnd2", "HB_Button1", "rightheld"));
    REQUIRE(t.host.menus.size() == 1);
    CHECK(t.host.menus[0].rows[2].text == "Change Size");
    CHECK(t.api->menu_click(0, 2));
    REQUIRE(t.host.quantity);
    CHECK(t.host.quantity->min == 40);
    CHECK(t.host.quantity->max == 175);
    CHECK(t.host.quantity->slider == 60);
    auto done = std::move(t.host.quantity->done);
    t.host.quantity.reset();
    done(150);
    CHECK(app::hot_bar_settings(1).percent == 150);
    // Assign Icon: the icon selection window on the button's (-1, -1): type Spell (1), icon 0; Item, then Accept
    CHECK(t.api->click_hot_button("HotButtonWnd2", "HB_Button1", "rightheld"));
    CHECK(t.api->menu_click(0, 3));
    REQUIRE(t.api->shown("IconSelectionWnd"));
    CHECK(t.api->child_text("IconSelectionWnd", "ISW_Slot_EditBox") == "0");
    CHECK(t.api->child_text("IconSelectionWnd", "ISW_PageRange_Label") == "Icons: 0 - 49");
    CHECK(t.api->select_combo("IconSelectionWnd", "ISW_Type_Combobox", 0));
    CHECK(t.api->child_text("IconSelectionWnd", "ISW_Slot_EditBox") == "500");
    CHECK(t.api->click_child("IconSelectionWnd", "ISW_PageRight_Button"));
    CHECK(t.api->child_text("IconSelectionWnd", "ISW_PageIndex_Label") == "2");
    CHECK(t.api->click_child("IconSelectionWnd", "ISW_Icon:560"));
    CHECK(t.api->click_child("IconSelectionWnd", "ISW_Accept_Button"));
    CHECK_FALSE(t.api->shown("IconSelectionWnd"));
    CHECK(store.current(1, 0).icon_type == 0);
    CHECK(store.current(1, 0).icon_slot == 560);
    CHECK(store.current(1, 0).type == game::HotButtonType::MenuButton);
    // the bar's own menu (a right click on the window)
    CHECK(t.api->context_menu("HotButtonWnd2", 10, 10));
    REQUIRE(t.host.menus.size() == 1);
    const auto& rows = t.host.menus[0].rows;
    REQUIRE(rows.size() == 11);
    CHECK(rows[0].text == "Window");
    CHECK(rows[2].text == "Load hotbutton set");
    CHECK_FALSE(rows[2].enabled); // no sets
    CHECK(rows[3].text == "Save hotbutton set");
    CHECK(rows[5].text == "Clear current hotbuttons");
    CHECK(rows[7].text == "Button Size");
    CHECK(rows[8].text == "Show Key Map");
    CHECK(rows[8].check);
    CHECK(rows[9].text == "Show Spinner");
    CHECK(rows[10].text == "Open New Bar");
    CHECK(rows[10].enabled); // bars 2..10 start hidden
    // Show Spinner off hides the page buttons
    CHECK(t.api->menu_click(0, 9));
    CHECK(t.api->child_shown("HotButtonWnd2", "HB_HorizontalBarPageButtons") == false);
    // Save: a text entry of 1..24 characters
    CHECK(t.api->context_menu("HotButtonWnd2", 10, 10));
    CHECK(t.api->menu_click(0, 3));
    REQUIRE(t.host.text_entry);
    CHECK(t.host.text_entry->prompt == "Enter hotbutton set name:");
    CHECK(t.host.text_entry->min_chars == 1);
    CHECK(t.host.text_entry->max_chars == 24);
    t.host.text_entry.reset();
    // Clear current hotbuttons asks first
    CHECK(t.api->context_menu("HotButtonWnd2", 10, 10));
    CHECK(t.api->menu_click(0, 5));
    REQUIRE(t.host.confirm);
    CHECK(t.host.confirm->text.starts_with("Are you sure you want to clear all"));
    t.host.confirm.reset();
    // Open New Bar shows the first hidden bar
    std::string first_hidden;
    for (int b = 1; b <= 10 && first_hidden.empty(); ++b) {
        std::string screen = b == 1 ? "HotButtonWnd" : "HotButtonWnd" + std::to_string(b);
        if (!t.api->shown(screen)) first_hidden = screen;
    }
    REQUIRE_FALSE(first_hidden.empty());
    CHECK(t.api->context_menu("HotButtonWnd2", 10, 10));
    CHECK(t.api->menu_click(0, 10));
    CHECK(t.api->shown(first_hidden));
}

TEST_CASE("Colour picker: a swatch, then Accept answers the caller") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    std::uint32_t got = 0;
    t.host.pick_color(0x123456, [&](std::uint32_t c) { got = c; });
    REQUIRE(t.api->shown("ColorPickerWnd"));
    CHECK(t.api->click_child("ColorPickerWnd", "CPW_TC12Button"));
    CHECK(t.api->click_child("ColorPickerWnd", "CPW_Accept_Button"));
    CHECK(got == 0xfff07f00u);
    CHECK_FALSE(t.api->shown("ColorPickerWnd"));
}

TEST_CASE("Options window: the Colors page saves a user colour to the character's chat.ini, not eqclient.ini") {
    const GameDir* real = test::game_dir();
    if (!real) return;
    // A game folder of its own with the real strings and an eqclient.ini, which must stay as it is.
    hailstone::test::ScratchDir root_scratch("ui_windows_game");
    fs::path root = root_scratch.path();
    fs::create_directories(root);
    if (auto s = real->find("eqstr_us.txt")) fs::create_symlink(*s, root / "eqstr_us.txt");
    const std::string eqclient = "[TextColors]\nUser_3_Red=1\nUser_3_Green=2\nUser_3_Blue=3\n";
    { std::ofstream(root / "eqclient.ini", std::ios::binary) << eqclient; }
    {
        GameDir game(root);
        Harness t(game);
        CHECK(app::default_user_color_rgb(1) == 0xbe28beu);
        CHECK(t.api->click_list("OptionsWindow", "ColorChooseList", 1, 0)); // Tell (user colour 1)
        CHECK(t.api->click_child("OptionsWindow", "UserColorChange"));
        REQUIRE(t.api->shown("ColorPickerWnd"));
        CHECK(t.api->click_child("ColorPickerWnd", "CPW_TC0Button"));
        CHECK(t.api->click_child("ColorPickerWnd", "CPW_Accept_Button"));
        CHECK(app::user_color_rgb(1) == 0xf00000u);
        CHECK(app::user_color_rgb(2) == 0x010203u); // eqclient.ini's, as the default
        IniFile ini;
        REQUIRE(ini.load(t.dir / "chat.ini")); // beside the character's UI ini
        CHECK(ini.get_int("TextColors", "User_2_Red", -1) == 0xf0);
        CHECK(ini.get_int("TextColors", "User_2_Green", -1) == 0);
        std::ifstream in(root / "eqclient.ini", std::ios::binary);
        CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == eqclient);
    }
    fs::remove_all(root);
}

TEST_CASE("Options window: the target ring's colour buttons pick a con's colour and put them all back") {
    // No game files needed: an empty folder of its own.
    hailstone::test::ScratchDir root_scratch("ui_windows_ring");
    fs::create_directories(root_scratch.path());
    GameDir game(root_scratch.path());
    Harness t(game);
    std::map<std::string, int, std::less<>> options;
    for (int i = 0; i < 7; ++i) options[std::format("target_ring_color_{}", i)] = 0x112233;
    t.host.find_option = [&](std::string_view o) -> std::optional<int> {
        auto it = options.find(o);
        return it == options.end() ? std::nullopt : std::optional<int>(it->second);
    };
    t.host.set_option = [&](std::string_view o, int v) { options[std::string(o)] = v; };

    CHECK(t.api->click_child("OptionsWindow", "TargetRingColor3"));
    REQUIRE(t.host.color_pick);
    CHECK(t.host.color_pick->rgb == 0x112233u); // starts from the con's colour
    t.host.color_pick->done(0xfff07f00u);
    CHECK(options["target_ring_color_3"] == 0xf07f00); // the alpha byte dropped
    CHECK(options["target_ring_color_2"] == 0x112233);

    t.host.color_pick.reset();
    CHECK(t.api->click_child("OptionsWindow", "TargetRingColor7")); // not a con section: nothing to pick
    CHECK_FALSE(t.host.color_pick);

    CHECK(t.api->click_child("OptionsWindow", "TargetRingColorDefault"));
    for (int i = 0; i < 7; ++i) CHECK(options[std::format("target_ring_color_{}", i)] == -1);
}

TEST_CASE("Extended target window: its context menu (rows, a submenu, an option)") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    REQUIRE(t.api->context_menu("ExtendedTargetWnd", 10, 10));
    REQUIRE(t.host.menus.size() == 1);
    const auto& rows = t.host.menus[0].rows;
    REQUIRE(rows.size() == 35); // no Show Player Aggro Percent without expansion bit 0x40000
    CHECK(rows[1].separator);
    CHECK_FALSE(rows[4].enabled); // Current Target: no target
    // the last row opens the target set submenu
    CHECK(t.api->menu_click(0, static_cast<int>(rows.size()) - 1));
    CHECK(t.host.menus.size() == 2);
    CHECK(t.host.menus[1].rows.size() == 3);
    CHECK_FALSE(t.host.menus[1].rows[0].enabled); // Load target set: no sets saved
    CHECK(t.host.menus[1].rows[1].enabled);       // Save target set
    // Mana Display (row 31) toggles the window's option, on by default (the ctor's +0x4c0)
    CHECK(t.api->context_menu("ExtendedTargetWnd", 10, 10));
    CHECK(t.host.menus[0].rows[31].check);
    CHECK(t.api->menu_click(0, 31));
    CHECK(t.host.menus.empty());
    CHECK(t.api->context_menu("ExtendedTargetWnd", 10, 10));
    CHECK_FALSE(t.host.menus[0].rows[31].check);
}

TEST_CASE("Several pets (a server's plugin's list): the pet window's Pet 2 / Pet 3 gauges and its pet") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game, {});
    // Before a server's list (a stock server) the extra gauges stay hidden.
    CHECK(t.conn.pet_window_extra_pets().empty());

    auto& world = t.conn.world_for_test();
    world.player_id = 10;
    game::Spawn a;
    a.id = 21, a.name = "Nuleketh000", a.hp_percent = 80;
    game::Spawn b;
    b.id = 22, b.name = "Xyllor000", b.hp_percent = 55;
    world.spawns[a.id] = a;
    world.spawns[b.id] = b;
    // The third pet's spawn has not arrived: not on a gauge.
    t.conn.set_pet_list({{21, 13}, {22, 11}, {23, 0}});
    CHECK(t.conn.pets().focused == 21); // the first listed, as nothing was focused
    CHECK(t.conn.pet_window_extra_pets() == std::vector<std::uint32_t>{22});
    CHECK(t.api->child_text("PetInfoWindow", "Pet2HPGauge") == "Xyllor");

    // Another pet in the window (the NMS plugin's /petcycle), only on this side; the gauges follow.
    t.conn.set_window_pet(22);
    CHECK(t.conn.pet_window_extra_pets() == std::vector<std::uint32_t>{21});
    CHECK(t.api->child_text("PetInfoWindow", "Pet2HPGauge") == "Nuleketh");
    t.conn.set_window_pet(23);
    CHECK(t.conn.pet_window_extra_pets() == std::vector<std::uint32_t>{21, 22});
    t.conn.set_window_pet(21);

    // A click on a gauge targets the pet it shows.
    CHECK(t.api->click_child("PetInfoWindow", "Pet2HPGauge"));
    CHECK(t.conn.target() == 22);
}

TEST_CASE("Bazaar purchase window: the voucher cost by To Me (BZC_TokenLabel) from the search replies' u16") {
    hailstone::test::ScratchDir game_scratch("ui_windows_bazaar");
    GameDir game(game_scratch.path());
    Harness t(game);
    // EQEmu's search results: u32 0, u16 (its trader zone id), u32 trader, u32 count, the entries.
    net::BazaarResults r;
    r.zone_id = 151;
    r.trader_id = 789;
    net::BazaarResult e;
    e.trader_id = 789, e.serial = "0000000000004321", e.cost = 900, e.quantity = 1, e.item_id = 5023, e.icon = 592;
    e.name = "Fine Steel Sword";
    r.results.push_back(e);
    t.conn.receive_zone_packet_for_test(net::encode(r));
    CHECK(t.conn.bazaar_search_state().voucher_cost == 151);
    // SendBazaarDeliveryCosts as RoF2 gets it (action 0, u32 vouchers, float parcel rate, u32): the u16 is 2.
    std::vector<std::byte> cost(16);
    float rate = 0.1f;
    cost[4] = std::byte{2};
    std::memcpy(cost.data() + 8, &rate, 4);
    t.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::BazaarSearch), cost));
    CHECK(t.conn.bazaar_search_state().voucher_cost == 2);
    REQUIRE(t.conn.bazaar_search_state().results.size() == 1);

    // Buying the row: the confirmation shows the price, the parcel fee and "x 2" by the voucher button.
    net::PlayerProfile pp;
    pp.expansions = 0x40000; // a row's Buy needs the RoF expansion (7728)
    t.conn.set_profile_for_test(pp);
    CHECK(t.api->click_list("BazaarSearchWnd", "BZR_ItemList", 0, 0));
    CHECK(t.api->click_child("BazaarSearchWnd", "BZR_BuyButton"));
    REQUIRE(t.api->shown("BazaarConfirmationWnd"));
    CHECK(t.api->child_text("BazaarConfirmationWnd", "BZC_TokenLabel") == "x 2");
    CHECK(t.api->child_text("BazaarConfirmationWnd", "BZC_TotalPrice") == "Item Price: " + game::coin_compact(900));
    CHECK(t.api->click_child("BazaarConfirmationWnd", "BZC_CancelButton"));
    CHECK_FALSE(t.api->shown("BazaarConfirmationWnd"));
}

TEST_CASE("Bazaar search: the row's own Buy button, Find Trader and Hide Traders (CBazaarSearchWnd @0x635990)") {
    hailstone::test::ScratchDir game_scratch("ui_windows_bazaar_find");
    GameDir game(game_scratch.path());
    Harness t(game);
    const char* W = "BazaarSearchWnd";
    // Two traders: one in this zone (zone 0 here) with a spawn, one in another zone.
    net::BazaarTrader near_t;
    near_t.zone_id = 0, near_t.trader_id = 789, near_t.entity_id = 41, near_t.name = "Nearby";
    net::BazaarTrader far_t;
    far_t.zone_id = 151, far_t.trader_id = 790, far_t.entity_id = 42, far_t.name = "Faraway";
    t.conn.receive_zone_packet_for_test(net::encode_bazaar_add_trader(near_t));
    t.conn.receive_zone_packet_for_test(net::encode_bazaar_add_trader(far_t));
    net::BazaarResults r;
    net::BazaarResult e;
    e.trader_id = 789, e.serial = "0000000000004321", e.cost = 900, e.quantity = 1, e.item_id = 5023, e.icon = 592;
    e.name = "Fine Steel Sword";
    r.results.push_back(e);
    e.trader_id = 790, e.serial = "0000000000004322", e.name = "Rusty Sword";
    r.results.push_back(e);
    t.conn.receive_zone_packet_for_test(net::encode(r));
    REQUIRE(t.conn.bazaar_search_state().results.size() == 2);
    t.api->show(W, true);

    // The rows carry their own Buy button in column 8; the template button is hidden.
    auto rows = t.api->list_rows(W, "BZR_ItemList");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0][8] == "Buy");
    CHECK(t.api->child_shown(W, "BZR_BuyButton") == false);
    // Without the RoF expansion a row's Buy refuses (7728).
    CHECK(t.api->click_list(W, "BZR_ItemList", 0, 8));
    CHECK_FALSE(t.api->shown("BazaarConfirmationWnd"));
    net::PlayerProfile pp;
    pp.expansions = 0x40000;
    t.conn.set_profile_for_test(pp);
    CHECK(t.api->click_list(W, "BZR_ItemList", 0, 8));
    CHECK(t.api->shown("BazaarConfirmationWnd"));
    CHECK(t.api->click_child("BazaarConfirmationWnd", "BZC_CancelButton"));

    // Find Trader: enabled for a row whose trader is in this zone; a find forgets nothing and waits 15 s.
    auto& rows_sorted = t.conn.bazaar_search_state().results;
    int near_row = rows_sorted[0].trader_id == 789 ? 0 : 1;
    CHECK(t.api->click_list(W, "BZR_ItemList", 1 - near_row, 0));
    CHECK(t.api->child_state(W, "BZR_FindTraderButton") == false);
    CHECK(t.api->click_list(W, "BZR_ItemList", near_row, 0));
    CHECK(t.api->child_state(W, "BZR_FindTraderButton") == true);
    CHECK(t.api->click_child(W, "BZR_FindTraderButton"));
    CHECK(t.conn.bazaar_search_state().found_trader == 41);
    CHECK(t.api->child_state(W, "BZR_FindTraderButton") == false); // 15 s apart

    // Hide Traders (IsInvisible): every trader but the found one is hidden; a click forgets the found trader.
    auto& world = t.conn.world_for_test();
    world.player_id = 1;
    world.spawns[1].id = 1;
    world.spawns[41].id = 41, world.spawns[41].trader = true;
    world.spawns[43].id = 43, world.spawns[43].trader = true;
    world.spawns[44].id = 44; // not a trader
    CHECK_FALSE(t.conn.market_hidden(world.spawns[43]));
    CHECK_FALSE(t.conn.market_hides_any()); // (the frame then asks no spawn)
    t.conn.bazaar_set_hide_traders(true, false); // as the UI ini's HideTraders loads it
    CHECK(t.conn.market_hides_any());
    CHECK_FALSE(t.conn.market_hidden(world.spawns[41]));
    CHECK(t.conn.market_hidden(world.spawns[43]));
    CHECK_FALSE(t.conn.market_hidden(world.spawns[44]));
    CHECK_FALSE(t.conn.market_hidden(world.spawns[1]));
    t.conn.bazaar_set_hide_traders(false, false);
    CHECK(t.api->click_child(W, "BZR_HideTradersButton"));
    CHECK(t.api->child_checked(W, "BZR_HideTradersButton") == true);
    CHECK(t.conn.bazaar_search_state().found_trader == 0);
    CHECK(t.conn.market_hidden(world.spawns[41]));
    // A buyer: the barter search's Hide Buyers, the same way.
    world.spawns[45].id = 45, world.spawns[45].buyer = true;
    CHECK_FALSE(t.conn.market_hidden(world.spawns[45]));
    t.conn.barter_search_set_hide(true, true);
    CHECK(t.conn.barter_search_state().found_buyer == 0xffffffffu);
    CHECK(t.conn.market_hidden(world.spawns[45]));
    t.conn.bazaar_set_hide_traders(false, false);
    CHECK(t.conn.market_hides_any()); // Hide Buyers alone
}

TEST_CASE("Loot window: Link All, Loot All and Done without a loot session do nothing") {
    // Found live: a click on Link All in the frame after the server ended the session read a null session.
    const GameDir* game = test::game_dir();
    if (!game) return;
    Harness t(*game);
    REQUIRE_FALSE(t.conn.loot_session());
    CHECK_FALSE(t.api->click_child("LootWnd", "BroadcastButton"));
    t.api->click_child("LootWnd", "LootAllButton");
    t.api->click_child("LootWnd", "DoneButton");
    CHECK_FALSE(t.conn.loot_session());
}

TEST_CASE("Tracking window: its controls run the /track commands, auto update kept (CTrackingWnd::WndNotification)") {
    hailstone::test::ScratchDir game_scratch("ui_windows_tracking");
    std::ofstream(game_scratch.path() / "eqstr_us.txt") << "EQST0002\n12946 off\n12947 on\n13148 group\n"
                                                         << "13140 Tracking players on.\n13141 Tracking players off.\n"
                                                         << "13159 red\n13163 You will NOT see %1 NPCs.\n"
                                                         << "13164 You will see %1 NPCs.\n13149 Sorting normal.\n"
                                                         << "13150 Sorting by distance.\n4109 Pets off.\n";
    GameDir game(game_scratch.path());
    net::ConnectionOptions opts;
    opts.servers_dir = game_scratch.path() / "servers";
    Harness t(game, opts);
    const char* W = "TrackingWnd";
    net::PlayerProfile pp;
    pp.class_ = 1; // a warrior: the con filters and the sort are disabled and read 0
    t.conn.set_profile_for_test(pp);
    CHECK(t.api->child_state(W, "TRW_FilterRedButton") == false);
    CHECK(t.api->child_state(W, "TRW_TrackSortCombobox") == false);
    CHECK(t.api->child_state(W, "TRW_TrackPlayersCombobox") != false);

    pp.class_ = 4; // a ranger
    t.conn.set_profile_for_test(pp);
    CHECK(t.api->child_state(W, "TRW_FilterRedButton") == true);
    CHECK(t.api->click_child(W, "TRW_FilterRedButton"));
    CHECK((t.conn.tracking().filter.con_mask & game::kTrackRed) != 0);
    CHECK(t.api->child_checked(W, "TRW_FilterRedButton") == true);
    CHECK(t.api->click_child(W, "TRW_FilterRedButton")); // toggles back
    CHECK((t.conn.tracking().filter.con_mask & game::kTrackRed) == 0);
    // The sort combo: /tracksort with its letters (saved as TrackSortType).
    CHECK(t.api->select_combo(W, "TRW_TrackSortCombobox", 2));
    CHECK(t.conn.tracking().sort_order == 2);
    CHECK(t.conn.client_options().get(game::ClientOption::TrackSortType) == 2);
    // Players: off; pets: off (the check box's new state as /trackpets off).
    CHECK(t.api->select_combo(W, "TRW_TrackPlayersCombobox", 0));
    CHECK(t.conn.tracking().filter.players == 0);
    CHECK(t.conn.client_options().get(game::ClientOption::TrackPlayers) == 0);
    REQUIRE(t.conn.tracking().filter.pets);
    CHECK(t.api->click_child(W, "TRW_TrackPetsCheckbox"));
    CHECK_FALSE(t.conn.tracking().filter.pets);
    CHECK_FALSE(t.conn.client_options().on(game::ClientOption::TrackPets));
    // Auto update: kept and written as TrackAutoUpdate TRUE.
    CHECK_FALSE(t.conn.tracking().auto_update);
    CHECK(t.api->click_child(W, "TRW_AutoUpdateButton"));
    CHECK(t.conn.tracking().auto_update);
    CHECK(t.conn.client_options().on(game::ClientOption::TrackAutoUpdate));
    // As the option is written (the character's options.ini; no character here): TRUE / FALSE.
    IniFile ini;
    t.conn.client_options().store(game::ClientOption::TrackAutoUpdate, ini);
    CHECK(ini.get("Defaults", "TrackAutoUpdate") == std::optional<std::string>("TRUE"));
    // Read back each time the window opens (Activate).
    game::ClientOptions read;
    read.load(ini);
    CHECK(read.on(game::ClientOption::TrackAutoUpdate));
}

// The hot bar's Lock layout row (Hailstone's): on the window's own menu (the modern UI's frames), with the layout
// setting it toggles; not on eq.HotBarMenu's (the gamepad pack's pages, which have no frame to lock).
TEST_CASE("Hot bar: Lock layout on the window's menu only, toggling hud.lock.<window>") {
    const GameDir* g = test::game_dir();
    if (!g) return;
    Harness t(*g);
    std::map<std::string, std::string, std::less<>> settings;
    t.host.layout_setting = [&](std::string_view key) {
        auto it = settings.find(key);
        return it == settings.end() ? std::string() : it->second;
    };
    t.host.set_layout_setting = [&](std::string_view key, std::string_view value) {
        settings[std::string(key)] = std::string(value);
    };
    auto lock_row = [&]() -> const app::EqHost::MenuRow* {
        REQUIRE(t.host.menus.size() == 1);
        for (const auto& r : t.host.menus[0].rows)
            if (r.text == "Lock layout") return &r;
        return nullptr;
    };
    CHECK(t.api->context_menu("HotButtonWnd2", 10, 10));
    const app::EqHost::MenuRow* lock = lock_row();
    REQUIRE(lock);
    CHECK_FALSE(lock->check);
    const int at = static_cast<int>(lock - t.host.menus[0].rows.data());
    CHECK(t.api->menu_click(0, at));
    CHECK(settings["hud.lock.HotButtonWnd2"] == "1");
    CHECK(t.api->context_menu("HotButtonWnd2", 10, 10));
    REQUIRE(lock_row());
    CHECK(lock_row()->check);
    CHECK(t.api->menu_click(0, at));
    CHECK(settings["hud.lock.HotButtonWnd2"] == "0");
    // eq.HotBarMenu's path (no layout lock): the row is not there
    t.host.menus.clear();
    app::open_hot_bar_menu(t.host, 1, 10, 10);
    CHECK(lock_row() == nullptr);
}
