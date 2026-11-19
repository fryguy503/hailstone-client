#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "achievement_test_packets.h"
#include "core/byte_io.h"
#include "game/achievements.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;

namespace {
struct AchievementGui {
    ImGuiContext* context = ImGui::CreateContext();
    AchievementGui() {
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().DisplaySize = {1280, 900};
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~AchievementGui() { ImGui::DestroyContext(context); }
};
struct AchievementLua {
    test::ScratchDir scratch{"lua_ui_achievement_layout"};
    app::UiHost host;
    app::UiApi api{host};
    lua_State* state = luaL_newstate();
    explicit AchievementLua(net::GameConnection& connection) {
        host.conn = &connection;
        host.in_game = host.ui_ready = true;
        api.load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI_Test_Server.ini", {});
        luaL_openlibs(state);
        lua_newtable(state);
        api.register_functions(state);
        lua_setglobal(state, "hs");
    }
    ~AchievementLua() { lua_close(state); }
    void check(const std::string& code) {
        INFO(code);
        const int top = lua_gettop(state);
        const int status = luaL_dostring(state, code.c_str());
        REQUIRE_MESSAGE(status == LUA_OK, (status == LUA_OK ? "" : lua_tostring(state, -1)));
        REQUIRE(lua_isboolean(state, -1));
        CHECK(lua_toboolean(state, -1));
        lua_settop(state, top);
    }
};
game::Achievements& achievements(net::GameConnection& connection) {
    return const_cast<game::Achievements&>(connection.achievements());
}
net::AchievementCatalog catalog() {
    net::AchievementCatalog result;
    net::AchievementDefinition first;
    first.id = 42;
    first.name = "Literal <achievement> & /cast";
    first.description = "Description <literal> & \"quotes\"";
    first.icon_id = 0xffffffff;
    first.version = 0xfffffffe;
    first.persistent = true;
    first.points = 7;
    first.has_reward = true;
    first.components[1].push_back({101, 1, 10, "Counted objective", "Objective <literal>", 255});
    first.components[3].push_back({303, 3, 99, "Display text", "No inferred requirement", 0});
    first.component_indices[1].emplace(101, 0);
    first.component_indices[3].emplace(303, 0);
    net::AchievementDefinition hidden;
    hidden.id = 900;
    hidden.name = "Hidden definition";
    hidden.points = 11;
    result.definitions.push_back(std::move(first));
    result.definitions.push_back(std::move(hidden));
    result.definition_indices.emplace(42, 0);
    result.definition_indices.emplace(900, 1);
    net::AchievementCategory root;
    root.id = 55;
    root.name = "Root <family>";
    root.description = "Category description";
    root.icon = "literal_icon.dds";
    root.display_order = 2;
    root.child_ids = {66};
    root.associations.push_back({42, "Association text", 4});
    net::AchievementCategory child;
    child.id = 66;
    child.parent_id = 55;
    child.name = "Child";
    child.associations.push_back({900, "Hidden association", 5});
    result.categories.push_back(std::move(root));
    result.categories.push_back(std::move(child));
    result.category_indices.emplace(55, 0);
    result.category_indices.emplace(66, 1);
    return result;
}
net::AchievementState own_state(net::AchievementStatus status = net::AchievementStatus::Open) {
    net::AchievementState result;
    result.status = status;
    result.satisfied[1] = {1};
    result.counts[1] = {std::nullopt};
    if (status == net::AchievementStatus::Completed) result.completed_at = 0xffffffff;
    return result;
}
void initialize(net::GameConnection& connection, bool progress = false) {
    auto& model = achievements(connection);
    model.receive_catalog(catalog());
    net::AchievementPrimary primary;
    primary.serial = 1;
    primary.dense = true;
    primary.updates.push_back({0, own_state()});
    net::AchievementState hidden;
    hidden.status = net::AchievementStatus::Hidden;
    primary.updates.push_back({1, std::move(hidden)});
    REQUIRE(model.receive_primary(std::move(primary)));
    if (progress) REQUIRE(model.receive_progress({}));
    model.show();
}
std::string quoted(std::uint64_t value) { return "'" + std::to_string(value) + "'"; }
} // namespace

TEST_CASE("hs achievement bridge: received catalog and independent opaque caches") {
    AchievementGui gui;
    mq_test::World world("lua_ui_achievement_catalog");
    initialize(*world.conn);
    AchievementLua lua(*world.conn);
    lua.check(R"lua(
      first=hs.Achievements()
      local c=first.catalog.categories[1]
      local d=first.catalog.definitions[1]
      return first.shown==true and first.ready==true and first.progress_ready==false
        and type(first.catalog_token)=='string' and type(first.state_revision)=='string'
        and c.id==55 and c.parent_id==4294967295 and c.name=='Root <family>'
        and c.description=='Category description' and c.icon=='literal_icon.dds' and c.display_order==2
        and c.child_ids[1]==66 and c.associations[1].achievement_id==42
        and c.associations[1].text=='Association text' and c.associations[1].display_order==4
        and d.index==0 and d.id==42 and d.name=='Literal <achievement> & /cast'
        and d.description=='Description <literal> & "quotes"' and d.icon_id==4294967295
        and d.version==4294967294 and d.persistent==true and d.has_reward==true and d.points==7
        and first.states[1].known==true and first.states[1].status=='Open'
        and first.states[2].status=='Hidden'
    )lua");
    const auto& model = world.conn->achievements();
    lua.check("return first.catalog_token==" + quoted(model.catalog_token()) + " and first.state_revision==" + quoted(model.revision()));
    lua.check(R"lua(
      local s=hs.Achievements(first.catalog_token,first.state_revision)
      return s.catalog_unchanged==true and s.catalog==nil and s.states_unchanged==true and s.states==nil
    )lua");
    REQUIRE(achievements(*world.conn).receive_progress({}));
    lua.check(R"lua(
      second=hs.Achievements(first.catalog_token,first.state_revision)
      return second.catalog_unchanged==true and second.catalog==nil
        and second.states_unchanged==false and second.states~=nil and second.progress_ready==true
    )lua");
    achievements(*world.conn).receive_catalog(catalog());
    lua.check(R"lua(
      local s=hs.Achievements(first.catalog_token,second.state_revision)
      return s.catalog_unchanged==false and s.catalog~=nil and s.states_unchanged==false
        and s.catalog_token~=first.catalog_token and s.ready==false
    )lua");
}

TEST_CASE("hs achievement bridge: unknown zero aggregated counts and text-only bucket stay distinct") {
    AchievementGui gui;
    mq_test::World world("lua_ui_achievement_counts");
    initialize(*world.conn);
    AchievementLua lua(*world.conn);
    lua.check(R"lua(
      snapshot=hs.Achievements()
      local ok=hs.AchievementOpen(snapshot.catalog_token,0)
      local d=hs.Achievement()
      local c,t=d.own.components[1],d.own.components[2]
      return ok==true and d.shown==true and d.source=='own' and d.definition_index==0 and d.id==42
        and d.catalog_token==snapshot.catalog_token and type(d.revision)=='string'
        and d.own.known==true and d.own.status=='Open'
        and c.id==101 and c.type==1 and c.required_count==10 and c.satisfied_known==true and c.satisfied==true
        and c.count_known==false and c.count==nil and c.count_text==nil
        and t.id==303 and t.type==3 and t.name=='Display text' and t.description=='No inferred requirement'
        and t.satisfied_known==false and t.count_known==false and t.count==nil and t.count_text==nil
    )lua");
    auto& model = achievements(*world.conn);
    REQUIRE(model.receive_progress({}));
    lua.check("local c=hs.Achievement().own.components[1];return c.count_known==true and c.count==0 and c.satisfied==true");
    REQUIRE(model.receive_progress({{42,101,100000,1,4},{42,101,0xffffffff,1,6}}));
    lua.check("local d=hs.Achievement();local c=d.own.components[1];return c.count==10 and d.own.status=='Open'");
    REQUIRE(model.receive_progress({{42,101,100000,1,2}}));
    lua.check("return hs.Achievement().own.components[1].count==8");
    net::AchievementPrimary primary;
    primary.serial = 2;
    primary.updates.push_back({0, own_state(net::AchievementStatus::Completed)});
    REQUIRE(model.receive_primary(std::move(primary)));
    lua.check("local d=hs.Achievement();return d.own.status=='Completed' and d.own.completion_time==4294967295");
    net::AchievementPrimary wide;
    wide.serial = 3;
    wide.dense = true;
    auto wide_state = own_state();
    wide_state.counts[1] = {std::numeric_limits<std::uint64_t>::max()};
    wide.updates.push_back({0, std::move(wide_state)});
    net::AchievementState hidden;
    hidden.status = net::AchievementStatus::Hidden;
    wide.updates.push_back({1, std::move(hidden)});
    REQUIRE(model.receive_primary(std::move(wide)));
    lua.check("local c=hs.Achievement().own.components[1];return c.count_known==true and c.count_text=='18446744073709551615'");
}

TEST_CASE("hs achievement bridge: comparison and linked detail never replace own progress") {
    AchievementGui gui;
    mq_test::World world("lua_ui_achievement_comparison");
    initialize(*world.conn, true);
    auto& model = achievements(*world.conn);
    AchievementLua lua(*world.conn);
    REQUIRE(model.open_selection(model.catalog_token(), 0));
    auto request = model.reserve_compare(12, 7, "Groupie", 100);
    REQUIRE(request);
    lua.check("local s=hs.Achievements();return s.comparison.pending==true and s.states[1].status=='Open'");
    std::vector<net::AchievementState> compared{own_state(net::AchievementStatus::Completed), net::AchievementState{}};
    REQUIRE(model.receive_comparison(std::move(compared), 12, 7));
    lua.check(R"lua(
      local s=hs.Achievements()
      local d=hs.Achievement()
      return s.comparison.ready==true and s.comparison.name=='Groupie'
        and s.states[1].status=='Open' and d.own.status=='Open' and d.own.components[1].count==0
        and d.comparison.name=='Groupie' and d.comparison.status=='Completed'
        and d.comparison.components[1].satisfied==true and d.comparison.components[1].count_known==false
        and d.comparison.components[1].count==nil
    )lua");
    net::AchievementLink linked;
    linked.player_name = "Linked <player>";
    linked.achievement_id = 42;
    linked.state = own_state(net::AchievementStatus::Completed);
    linked.state.counts[1][0] = 17;
    linked.metadata = "Linked <player>^42^0^1^4294967295^17^";
    REQUIRE(model.open_link(std::move(linked)));
    model.show(false);
    lua.check(R"lua(
      local d=hs.Achievement()
      return hs.Achievements().shown==false and d.shown==true and d.source=='link'
        and d.player_name=='Linked <player>' and d.own.status=='Completed'
        and d.own.components[1].count_known==true and d.own.components[1].count==17
        and d.comparison==nil
    )lua");
    lua.check(R"lua(
      hs.ShowWindow('AchievementWnd',false)
      local hidden=hs.Achievement()
      hs.ShowWindow('AchievementWnd',true)
      local reopened=hs.Achievement()
      return hidden.shown==false and hidden.source=='link' and reopened.shown==true
        and reopened.source=='link' and reopened.player_name=='Linked <player>'
        and reopened.own.components[1].count==17 and hs.Achievements().shown==false
    )lua");
    lua.check("local s=hs.Achievements();return hs.AchievementOpen(s.catalog_token,0)==true and hs.Achievement().source=='own' and hs.Achievement().own.status=='Open'");
}

TEST_CASE("hs achievement bridge: strict tokens indices passive visibility and connection replacement") {
    AchievementGui gui;
    mq_test::World first("lua_ui_achievement_first"), second("lua_ui_achievement_second");
    initialize(*first.conn, true);
    initialize(*second.conn, true);
    AchievementLua lua(*first.conn);
    lua.check("old=hs.Achievements();return hs.Achievement()==nil and hs.WindowShown('AchievementsWnd')==true and hs.WindowShown('AchievementWnd')==false");
    const auto epoch = quoted(first.conn->achievements().catalog_token());
    for (const std::string invalid : {"nil", "{}", "true", "1", "1.0", "''", "'01'", "'-1'", "'+1'", "' 1'", "'1 '",
         "'18446744073709551616'", "'123456789012345678901'", "'1\\0x'"}) {
        INFO(invalid);
        lua.check("local ok=hs.AchievementOpen(" + invalid + ",0);return ok==false and hs.Achievement()==nil");
    }
    for (const std::string invalid : {"nil", "{}", "false", "'0'", "0.0", "0/0", "math.huge", "-1", "4294967296", "42"}) {
        INFO(invalid);
        lua.check("local ok=hs.AchievementOpen(" + epoch + "," + invalid + ");return ok==false and hs.Achievement()==nil");
    }
    lua.check("return hs.AchievementOpen('18446744073709551615',0)==false and hs.AchievementOpen('9007199254740993',0)==false");
    lua.check("return hs.AchievementOpen(" + epoch + ",0)==true and hs.WindowShown('AchievementWnd')==true");
    lua.check("hs.ShowWindow('AchievementWnd',false);return hs.WindowShown('AchievementWnd')==false and hs.WindowShown('AchievementsWnd')==true");
    lua.check("hs.ShowWindow('AchievementsWnd',false);return hs.Achievements().shown==false");
    lua.host.conn = second.conn.get();
    lua.check(R"lua(
      local s=hs.Achievements(old.catalog_token,old.state_revision)
      return s.catalog_token~=old.catalog_token and s.catalog_unchanged==false and s.catalog~=nil
        and s.state_revision~=old.state_revision and s.states_unchanged==false and s.states~=nil
        and hs.AchievementOpen(old.catalog_token,0)==false and hs.Achievement()==nil
    )lua");
    lua.host.in_game = false;
    lua.check("return hs.Achievements()==nil and hs.Achievement()==nil and hs.AchievementOpen(" + quoted(second.conn->achievements().catalog_token()) + ",0)==false");
}

TEST_CASE("hs achievement bridge: plugin UI requires capabilities for explicit actions") {
    AchievementGui gui;
    mq_test::World world("lua_ui_achievement_caps");
    initialize(*world.conn, true);
    AchievementLua binding(*world.conn);
    test::ScratchDir scratch("lua_ui_achievement_plugin");
    const auto pack = scratch.path() / "pack";
    const auto plugin_pack = scratch.path() / "plugin" / "ui" / "pack";
    std::filesystem::create_directories(pack);
    std::filesystem::create_directories(plugin_pack);
    std::ofstream(pack / "empty.lua") << "ui.window('Achievement pack',function()end)\n";
    std::ofstream(plugin_pack / "empty.lua") << "ui.window('Achievement plugin',function()end)\n";
    ui::lua::LuaUi lua(pack, scratch.path() / "ui.txt");
    lua.add_ui_api([&](lua_State* state) { binding.api.register_functions(state); });
    std::uint32_t caps = plugin::kCapRead | plugin::kCapUi;
    lua.set_caller_scope([&](std::string_view) -> std::shared_ptr<void> {
        return std::make_shared<plugin::CallerScope>(caps, "achievement-test-plugin");
    });
    lua.set_folders({{"achievement-test-plugin", plugin_pack}});
    ImGui::NewFrame();
    lua.frame();
    ImGui::Render();
    const auto in_plugin = [&](const std::string& code) {
        INFO(code);
        CHECK(lua.run_for_test("achievement-test-plugin", code) == "true");
    };
    const auto epoch = quoted(world.conn->achievements().catalog_token());
    in_plugin("return hs.Achievements()~=nil and hs.Achievement()==nil");
    for (const auto& call : std::array<std::string, 6>{"hs.AchievementRefresh()", "hs.AchievementCompare()", "hs.AchievementClearCompare()",
         "hs.AchievementOpen(" + epoch + ",0)", "hs.AchievementReward(" + epoch + ",0)", "hs.AchievementLink(" + epoch + ",0)"})
        in_plugin("return " + call + "==nil");
    CHECK_FALSE(world.conn->achievements().selection());
    CHECK_FALSE(world.conn->achievements().compare_pending());
    caps |= plugin::kCapCommands;
    in_plugin("return hs.AchievementOpen(" + epoch + ",0)==true and hs.Achievement().source=='own'");
    in_plugin("return hs.AchievementClearCompare()==true and hs.AchievementCompare()==nil and hs.AchievementLink(" + epoch + ",0)==nil");
    caps = plugin::kCapRead | plugin::kCapUi | plugin::kCapActions;
    in_plugin("return hs.AchievementOpen(" + epoch + ",0)==nil and hs.AchievementReward(" + epoch + ",0)==false");
    // No ready own snapshot means a Chat-capable Link is rejected by the real
    // handler, while still distinguishing admission from a capability denial.
    achievements(*world.conn).receive_catalog(catalog());
    caps = plugin::kCapRead | plugin::kCapUi | plugin::kCapChat;
    const auto replacement = quoted(world.conn->achievements().catalog_token());
    in_plugin("return hs.AchievementLink(" + replacement + ",0)==false and hs.AchievementRefresh()==nil");
    CHECK(app::ui_call_caps("Achievements") == 0);
    CHECK(app::ui_call_caps("Achievement") == 0);
    CHECK(app::ui_call_caps("AchievementOpen") == plugin::kCapCommands);
    CHECK(app::ui_call_caps("AchievementClearCompare") == plugin::kCapCommands);
    CHECK(app::ui_call_caps("AchievementRefresh") == plugin::kCapActions);
    CHECK(app::ui_call_caps("AchievementCompare") == plugin::kCapActions);
    CHECK(app::ui_call_caps("AchievementReward") == plugin::kCapActions);
    CHECK(app::ui_call_caps("AchievementLink") == plugin::kCapChat);
}

TEST_CASE("hs achievement bridge: literal packet ingress is passive and reward uses the received index") {
    AchievementGui gui;
    plugin::Hooks hooks;
    std::vector<net::AppPacket> outgoing;
    hooks.on_packet(plugin::Hooks::kClient, plugin::Stream::Zone, plugin::Direction::Out, 0,
        [&](net::AppPacket& packet) {
            if (packet.opcode == 0x47cb || packet.opcode == 0x6411 || packet.opcode == 0x1665 || packet.opcode == 0x675d)
                outgoing.push_back(packet);
            return plugin::Flow::Continue;
        });
    auto options = test::quick_options();
    options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    namespace packets = test::achievement_fixture;
    for (const auto& packet : {packets::catalog(), packets::primary(), packets::progress()})
        net::GameConnectionTestAccess::handle_zone(rig.conn, packet);
    AchievementLua lua(rig.conn);
    const auto before = outgoing.size();
    lua.check(R"lua(
      current=hs.Achievements()
      local ok=hs.AchievementOpen(current.catalog_token,1)
      local d=hs.Achievement()
      return current.ready==true and current.progress_ready==true and ok==true
        and d.id==900 and d.definition_index==1 and d.name=='Second & <Achievement>'
        and d.own.components[2].id==201 and d.own.components[2].count_known==true and d.own.components[2].count==2
    )lua");
    CHECK(outgoing.size() == before);
    lua.check("local ok=hs.AchievementReward(current.catalog_token,900);return ok==false");
    CHECK(outgoing.size() == before);
    lua.check("return hs.AchievementReward(current.catalog_token,1)==true");
    REQUIRE(outgoing.size() == before + 1);
    CHECK(outgoing.back().opcode == 0x6411);
    ByteReader reward(outgoing.back().data);
    CHECK(reward.le<std::uint32_t>() == 5);
    CHECK(reward.le<std::uint32_t>() == 1);
    CHECK(reward.empty());
    lua.check("return hs.AchievementRefresh()==true");
    REQUIRE(outgoing.size() == before + 2);
    CHECK(outgoing.back().opcode == 0x47cb);
    ByteReader refresh(outgoing.back().data);
    CHECK(refresh.le<std::uint8_t>() == 0x30);
    CHECK(refresh.empty());
    const auto after = outgoing.size();
    lua.check("local s=hs.Achievements(current.catalog_token,current.state_revision);return s.refresh_pending==true");
    lua.check("return hs.AchievementRefresh()==false");
    CHECK(outgoing.size() == after);
}

TEST_CASE("hs achievement bridge: a kept record is refilled in place, with no garbage while nothing changes") {
    AchievementGui gui;
    mq_test::World world("lua_ui_achievement_into");
    initialize(*world.conn);
    AchievementLua lua(*world.conn);
    lua.check(R"lua(
      into={selected_index=9}
      local s=hs.Achievements(nil, nil, into)
      cmp=s.comparison
      return rawequal(s, into) and s.catalog~=nil and s.states~=nil and s.selected_index==nil and type(cmp)=='table'
    )lua");
    lua.check(R"lua(
      local t, r=into.catalog_token, into.state_revision
      collectgarbage('stop') local b=collectgarbage('count')
      local s=hs.Achievements(t, r, into)
      local a=collectgarbage('count') collectgarbage('restart')
      return rawequal(s, into) and s.catalog_unchanged and s.states_unchanged and s.catalog==nil and s.states==nil
        and rawequal(s.comparison, cmp) and a==b
    )lua");
    // The cached parts a caller writes back (as the Modern browser does) are cleared by the next call.
    lua.check(R"lua(
      into.catalog, into.states={}, {}
      local s=hs.Achievements(into.catalog_token, into.state_revision, into)
      return s.catalog==nil and s.states==nil
    )lua");
}
