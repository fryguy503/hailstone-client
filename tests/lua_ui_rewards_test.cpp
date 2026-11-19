#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "core/byte_io.h"
#include "game/reward_selection.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"
#include "reward_test_packets.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;
namespace fixture = test::reward_fixture;

namespace {
using Lane = game::RewardLane;
struct RewardGui {
    ImGuiContext* context = ImGui::CreateContext();
    RewardGui() {
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().DisplaySize = {1280, 900};
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~RewardGui() { ImGui::DestroyContext(context); }
};
game::RewardSelection& rewards(net::GameConnection& connection) {
    return const_cast<game::RewardSelection&>(connection.rewards());
}
game::RewardLaneState& lane(net::GameConnection& connection, Lane value = Lane::Pending) {
    return const_cast<game::RewardLaneState&>(connection.rewards().lane(value));
}
void deliver(net::GameConnection& connection, net::AppPacket packet) {
    net::GameConnectionTestAccess::handle_zone(connection, packet);
}
struct RewardLua {
    test::ScratchDir scratch{"lua_ui_rewards_layout"};
    app::UiHost host;
    app::UiApi api{host};
    lua_State* state = luaL_newstate();
    explicit RewardLua(net::GameConnection& connection) {
        host.conn = &connection;
        host.in_game = host.ui_ready = true;
        api.load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI_Test_Server.ini", {});
        luaL_openlibs(state);
        lua_newtable(state);
        api.register_functions(state);
        lua_setglobal(state, "hs");
    }
    ~RewardLua() { lua_close(state); }
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
std::string quoted(std::uint64_t value) { return "'" + std::to_string(value) + "'"; }
std::string choice_args(const game::RewardLaneState& view, std::uint32_t option = 12) {
    return quoted(view.revision) + "," + quoted(view.tabs.at(0).token) + "," + std::to_string(option);
}
const net::AppPacket* last(const std::vector<net::AppPacket>& packets, std::uint32_t action) {
    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        if (it->data.size() < 4) continue;
        ByteReader reader(it->data);
        if (reader.le<std::uint32_t>() == action) return &*it;
    }
    return nullptr;
}
} // namespace

TEST_CASE("hs reward bridge: exact typed DTO and full-width opaque identities") {
    RewardGui gui;
    mq_test::World world("lua_ui_reward_dto");
    deliver(*world.conn, fixture::collection());
    deliver(*world.conn, fixture::collection(Lane::Preview, 13));
    auto& pending = lane(*world.conn);
    pending.revision = std::numeric_limits<std::uint64_t>::max() - 1;
    pending.tabs[0].token = std::numeric_limits<std::uint64_t>::max();
    auto& tab = pending.tabs[0];
    tab.reward.title = "Literal <reward> & /cast 1";
    tab.reward.pending_id = 0xffffffff;
    tab.reward.set_id = 0xfffffffe;
    tab.reward.multiplier = 1.25f;
    tab.reward.options[1].wire_id = 0xffffffff;
    tab.reward.options[1].flags = 0xff;
    auto& entry = tab.reward.options[1].entries[0];
    entry.entry_id = 0xfffffffd;
    entry.string_id = 0xfffffffc;
    entry.description = "Plain <item> & \"description\"";
    entry.items[0] = {0xffffffff, 0xfffffffb, true, 0xfffffffa, "Item <literal>"};
    auto& common = tab.reward.options[0].entries;
    net::RewardEntry money;
    money.type = net::RewardEntryType::Money;
    money.entry_id = 30;
    money.values = {11, 22, 33, 44};
    common.push_back(money);
    net::RewardEntry ability;
    ability.type = net::RewardEntryType::AAAbility;
    ability.entry_id = 31;
    ability.values = {1234, 7, 0, 0};
    common.push_back(ability);
    net::RewardEntry points;
    points.type = net::RewardEntryType::AAPoints;
    points.entry_id = 32;
    points.values[0] = 9;
    points.flag = 0x80;
    common.push_back(points);
    points.type = net::RewardEntryType::GenericPoints;
    points.values = {1, 2, 15, 444};
    common.push_back(points);
    RewardLua lua(*world.conn);
    lua.check(R"lua(
      local s=hs.RewardSelection()
      local p=s.pending
      local t=p.tabs[1]
      local c=t.options[1]
      local o=t.options[2]
      local e=o.entries[1]
      local item=e.items[1]
      return s.shown==true and p.shown==true and s.preview.shown==true
        and type(p.revision)=='string' and p.revision=='18446744073709551614'
        and type(t.token)=='string' and t.token=='18446744073709551615'
        and t.title=='Literal <reward> & /cast 1' and t.pending_id==4294967295 and t.set_id==4294967294
        and t.multiplier==1.25 and t.blocked==false
        and c.common==true and o.common==false and o.wire_id==4294967295 and o.flags==255
        and e.type==2 and e.kind_label=='Item' and e.entry_id==4294967293 and e.string_id==4294967292
        and e.description=='Plain <item> & "description"' and e.details==''
        and item.id==4294967295 and item.quantity==4294967291 and item.lore==true
        and item.lore_group==4294967290 and item.name=='Item <literal>'
        and c.entries[2].kind_label=='Money' and c.entries[2].details=='13p 27g 41s 55c'
        and c.entries[3].kind_label=='Alternate ability' and c.entries[3].details=='Ability rank 1234, cumulative rank 7'
        and c.entries[4].details=='11 points' and c.entries[4].flag==128 and c.entries[4].values[1]==9
        and c.entries[5].details=='18 points' and c.entries[5].values[1]==1 and c.entries[5].values[2]==2
        and c.entries[5].values[3]==15 and c.entries[5].values[4]==444
    )lua");
    lua.check("return hs.RewardSelection().pending.tabs[1].token ~= hs.RewardSelection().preview.tabs[1].token");
    tab.reward.multiplier = 0.5f;
    common[1].values = {3, 1, 0xffffffffu, 0xfffffffdu};
    lua.check("return hs.RewardSelection().pending.tabs[1].options[1].entries[2].details=='1p 0g 0s -1c'");
    tab.reward.multiplier = 2.0f;
    common[1].values = {0x7fffffffu, 0x80000000u, 2, 3};
    lua.check("return hs.RewardSelection().pending.tabs[1].options[1].entries[2].details=='Amount unavailable'");
    tab.reward.multiplier = 0.0f;
    lua.check("return hs.RewardSelection().pending.tabs[1].options[1].entries[2].details=='0p 0g 0s 0c'");
    lua.host.in_game = false;
    lua.check("return hs.RewardSelection()==nil");
    lua.host.in_game = true;
    lua.host.conn = nullptr;
    lua.check("return hs.RewardSelection()==nil");
}

TEST_CASE("hs reward bridge: cached revisions omit tables while pending metadata stays fresh") {
    RewardGui gui;
    mq_test::World world("lua_ui_reward_cache");
    deliver(*world.conn, fixture::collection());
    deliver(*world.conn, fixture::collection(Lane::Preview, 13));
    RewardLua lua(*world.conn);
    lua.check("saved=hs.RewardSelection(); return saved.pending.tabs~=nil and saved.preview.tabs~=nil");
    auto& pending = lane(*world.conn);
    pending.claim_pending = true;
    pending.claim_uncertain = true;
    pending.inspect_pending = true;
    pending.message = "Pending <literal> status";
    lua.check(R"lua(
      local s=hs.RewardSelection(saved.pending.revision,saved.preview.revision)
      return s.pending.unchanged==true and s.pending.tabs==nil and s.preview.unchanged==true and s.preview.tabs==nil
        and s.pending.claim_pending==true and s.pending.claim_uncertain==true and s.pending.inspect_pending==true
        and s.pending.message=='Pending <literal> status' and s.pending.shown==true
    )lua");
    // Invalid optional cache hints request a full snapshot, never an action/error.
    lua.check("local s=hs.RewardSelection(123,{});return s.pending.unchanged==false and s.pending.tabs~=nil and s.preview.tabs~=nil");
    deliver(*world.conn, fixture::collection());
    lua.check(R"lua(
      local s=hs.RewardSelection(saved.pending.revision,saved.preview.revision)
      return s.pending.unchanged==false and #s.pending.tabs==1 and s.pending.tabs[1].token~=saved.pending.tabs[1].token
        and s.preview.unchanged==true and s.preview.tabs==nil
    )lua");
    net::RewardPacket empty;
    empty.lane = Lane::Pending;
    rewards(*world.conn).receive(std::move(empty));
    lua.check("local s=hs.RewardSelection(saved.pending.revision,saved.preview.revision);return s.pending.unchanged==false and #s.pending.tabs==0");
}

TEST_CASE("hs reward bridge: strict decimal-u64 and integer-u32 arguments reject coercion") {
    RewardGui gui;
    mq_test::World world("lua_ui_reward_args");
    deliver(*world.conn, fixture::collection());
    RewardLua lua(*world.conn);
    const auto& view = world.conn->rewards().lane(Lane::Pending);
    const auto revision = quoted(view.revision), token = quoted(view.tabs[0].token);
    for (const std::string invalid : {"nil", "{}", "true", "1", "1.0", "''", "'01'", "'-1'", "'+1'", "' 1'", "'1 '",
         "'18446744073709551616'", "'123456789012345678901'", "'1\\0x'"}) {
        INFO(invalid);
        lua.check("local ok,why=hs.RewardSelectionClaim(" + invalid + "," + token + ",12);return ok==false and why=='Invalid or retired reward choice.'");
        lua.check("local ok,why=hs.RewardSelectionClaim(" + revision + "," + invalid + ",12);return ok==false and why=='Invalid or retired reward choice.'");
    }
    for (const std::string invalid : {"nil", "{}", "false", "'12'", "12.0", "0/0", "math.huge", "-1", "4294967296"}) {
        INFO(invalid);
        lua.check("local ok,why=hs.RewardSelectionClaim(" + revision + "," + token + "," + invalid + ");return ok==false and why=='Invalid or retired reward choice.'");
        for (int at = 0; at < 3; ++at) {
            const std::string option = at == 0 ? invalid : "12";
            const std::string entry = at == 1 ? invalid : "20";
            const std::string item = at == 2 ? invalid : "1000";
            lua.check("local ok,why=hs.RewardSelectionInspect('pending'," + revision + "," + token + "," + option + "," + entry + "," + item + ");return ok==false and why=='Invalid or retired reward item.'");
        }
    }
    // Accepted scalar ranges reach the connection's live-session check. This
    // disconnected fixture must not turn them into a malformed-argument error.
    for (const auto* value : {"0", "4294967295"})
        lua.check("local ok,why=hs.RewardSelectionClaim('18446744073709551615','18446744073709551614'," + std::string(value) + ");return ok==false and why~='Invalid or retired reward choice.'");
    for (const auto* invalid_lane : {"nil", "0", "'Pending'", "'pending\\0preview'", "'other'"})
        lua.check("local ok,why=hs.RewardSelectionInspect(" + std::string(invalid_lane) + "," + revision + "," + token + ",12,20,1000);return ok==false and why=='Invalid or retired reward item.'");
    CHECK_FALSE(world.conn->rewards().lane(Lane::Pending).claim_pending);
    CHECK_FALSE(world.conn->rewards().lane(Lane::Pending).inspect_pending);
}

TEST_CASE("hs reward bridge: local visibility close and replacement connection identities") {
    RewardGui gui;
    mq_test::World first("lua_ui_reward_first");
    mq_test::World second("lua_ui_reward_second");
    deliver(*first.conn, fixture::collection());
    deliver(*first.conn, fixture::collection(Lane::Preview, 13));
    deliver(*second.conn, fixture::collection());
    RewardLua lua(*first.conn);
    lua.check("old=hs.RewardSelection();return hs.WindowShown('RewardSelectionWnd')==true");
    lua.check("local ok=hs.RewardSelectionClose('pending');local s=hs.RewardSelection();return ok==true and s.pending.shown==false and s.preview.shown==true and s.shown==true");
    lua.check("hs.ShowWindow('RewardSelectionWnd',false);return hs.RewardSelection().shown==false and hs.WindowShown('RewardSelectionWnd')==false");
    lua.check("local ok,why=hs.RewardSelectionClose('other');return ok==false and why=='Unknown reward page.'");
    lua.host.conn = second.conn.get();
    lua.check(R"lua(
      local s=hs.RewardSelection(old.pending.revision,old.preview.revision)
      return s.pending.unchanged==false and s.pending.tabs~=nil and s.pending.revision~=old.pending.revision
        and s.pending.tabs[1].token~=old.pending.tabs[1].token
    )lua");
    lua.check("local ok=hs.RewardSelectionClaim(old.pending.revision,old.pending.tabs[1].token,12);return ok==false");
    CHECK_FALSE(second.conn->rewards().lane(Lane::Pending).claim_pending);
}

TEST_CASE("hs reward bridge: getter is passive and explicit full-width requests reach the mock server") {
    RewardGui gui;
    plugin::Hooks hooks;
    std::vector<net::AppPacket> outgoing;
    hooks.on_packet(plugin::Hooks::kClient, plugin::Stream::Zone, plugin::Direction::Out, 0,
        [&](net::AppPacket& packet) {
            if (packet.opcode == net::reward_opcode(Lane::Pending) || packet.opcode == net::reward_opcode(Lane::Preview)) outgoing.push_back(packet);
            return plugin::Flow::Continue;
        });
    auto options = test::quick_options();
    options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    deliver(rig.conn, fixture::collection(Lane::Pending, 0xffffffff, 0xffffffff));
    deliver(rig.conn, fixture::collection(Lane::Preview, 0xfffffffd, 0xfffffffc));
    lane(rig.conn).tabs[0].reward.options[1].entries[0].entry_id = 0xfffffffe;
    RewardLua lua(rig.conn);
    lua.check("saved=hs.RewardSelection();return saved.shown==true");
    for (int i = 0; i < 3; ++i) lua.check("return hs.RewardSelection(saved.pending.revision,saved.preview.revision).pending.unchanged==true");
    CHECK(outgoing.empty());
    lua.check("local ok=hs.RestorePendingRewards();return ok==true");
    const auto* restore = last(outgoing, 6);
    REQUIRE(restore);
    ByteReader restored(restore->data);
    CHECK(restored.le<std::uint32_t>() == 6);
    CHECK(restored.empty());
    lua.check("local ok=hs.RewardSelectionInspect('pending',saved.pending.revision,saved.pending.tabs[1].token,4294967295,4294967294,4294967295);return ok==true");
    const auto* inspection = last(outgoing, 1);
    REQUIRE(inspection);
    ByteReader inspected(inspection->data);
    CHECK(inspected.le<std::uint32_t>() == 1);
    CHECK(inspected.le<std::uint32_t>() == 700);
    CHECK(inspected.le<std::uint32_t>() == 0xffffffff);
    CHECK(inspected.le<std::uint32_t>() == 0xfffffffe);
    CHECK(inspected.le<std::uint32_t>() == 0xffffffff);
    CHECK(inspected.empty());
    lua.check("local ok=hs.RewardSelectionInspect('preview',saved.preview.revision,saved.preview.tabs[1].token,4294967293,20,4294967292);return ok==true");
    const auto* preview = last(outgoing, 1);
    REQUIRE(preview);
    CHECK(preview->opcode == net::reward_opcode(Lane::Preview));
    ByteReader previewed(preview->data);
    for (const auto field : {1u, 700u, 0xfffffffdu, 20u, 0xfffffffcu}) CHECK(previewed.le<std::uint32_t>() == field);
    CHECK(previewed.empty());
    const auto before_preview_claim = outgoing.size();
    lua.check("local ok=hs.RewardSelectionClaim(saved.preview.revision,saved.preview.tabs[1].token,4294967293);return ok==false");
    CHECK(outgoing.size() == before_preview_claim);
    lua.check("local ok=hs.RewardSelectionClaim(saved.pending.revision,saved.pending.tabs[1].token,4294967295);return ok==true");
    const auto* claim = last(outgoing, 3);
    REQUIRE(claim);
    ByteReader claimed(claim->data);
    CHECK(claimed.le<std::uint32_t>() == 3);
    CHECK(claimed.le<std::uint32_t>() == 42);
    CHECK(claimed.le<std::uint32_t>() == 700);
    CHECK(claimed.le<std::uint32_t>() == 0xffffffff);
    CHECK(claimed.le<std::uint32_t>() == 0); // stock sender trailer, never an extra UI argument
    CHECK(claimed.empty());
    REQUIRE(rig.until([&] {
        return test::MockEqServer::count(rig.server.zone_log, net::Op::AchievementReward) >= 3 &&
               test::MockEqServer::count(rig.server.zone_log, net::Op::RewardSelection) >= 1;
    }));
    CHECK(rig.conn.rewards().lane(Lane::Pending).claim_pending);
    const auto before = outgoing.size();
    lua.check("return hs.RewardSelection(saved.pending.revision,saved.preview.revision).pending.claim_pending==true");
    CHECK(outgoing.size() == before);
}

TEST_CASE("hs reward bridge: plugin UI without actions cannot inspect claim or restore") {
    RewardGui gui;
    mq_test::World world("lua_ui_reward_capabilities");
    deliver(*world.conn, fixture::collection());
    RewardLua binding(*world.conn);
    test::ScratchDir scratch("lua_ui_reward_plugin");
    const auto pack = scratch.path() / "pack";
    const auto plugin_root = scratch.path() / "plugin";
    std::filesystem::create_directories(pack);
    std::filesystem::create_directories(plugin_root / "ui" / "pack");
    std::ofstream(pack / "empty.lua") << "ui.window('Reward test pack',function()end)\n";
    std::ofstream(plugin_root / "ui" / "pack" / "empty.lua") << "ui.window('Reward test plugin',function()end)\n";
    ui::lua::LuaUi lua(pack, scratch.path() / "ui.txt");
    lua.add_ui_api([&](lua_State* state) { binding.api.register_functions(state); });
    std::uint32_t caps = plugin::kCapRead | plugin::kCapUi;
    lua.set_caller_scope([&](std::string_view) -> std::shared_ptr<void> {
        return std::make_shared<plugin::CallerScope>(caps, "reward-test-plugin");
    });
    // LuaUi takes the selected pack's actual Lua directory, after UiBridge has
    // resolved <owner>/ui/<pack>; it does not scan the owner root recursively.
    lua.set_folders({{"reward-test-plugin", plugin_root / "ui" / "pack"}});
    ImGui::NewFrame();
    lua.frame();
    ImGui::Render();
    const auto in_plugin = [&](const std::string& code) {
        INFO(code);
        auto* state = lua.state();
        REQUIRE(luaL_loadstring(state, code.c_str()) == LUA_OK);
        lua.push_folder_env_for_test("reward-test-plugin");
        REQUIRE(lua_istable(state, -1));
        REQUIRE(lua_setupvalue(state, -2, 1) != nullptr);
        const int status = lua_pcall(state, 0, 1, 0);
        REQUIRE_MESSAGE(status == LUA_OK, (status == LUA_OK ? "" : lua_tostring(state, -1)));
        CHECK(lua_toboolean(state, -1));
        lua_settop(state, 0);
    };
    const auto args = choice_args(world.conn->rewards().lane(Lane::Pending));
    in_plugin("return hs.RewardSelection()~=nil");
    in_plugin("return hs.RewardSelectionClaim(" + args + ")==nil");
    in_plugin("return hs.RewardSelectionInspect('pending'," + args + ",20,1000)==nil");
    in_plugin("return hs.RestorePendingRewards()==nil");
    CHECK_FALSE(world.conn->rewards().lane(Lane::Pending).claim_pending);
    CHECK_FALSE(world.conn->rewards().lane(Lane::Pending).inspect_pending);
    CHECK(app::ui_call_caps("RewardSelectionClaim") == plugin::kCapActions);
    CHECK(app::ui_call_caps("RewardSelectionInspect") == plugin::kCapActions);
    CHECK(app::ui_call_caps("RestorePendingRewards") == plugin::kCapActions);
    caps |= plugin::kCapActions;
    in_plugin("local ok,why=hs.RewardSelectionClaim(" + args + ");return ok==false and why~=nil");
}

TEST_CASE("hs reward bridge: a kept record is refilled in place, with no garbage while nothing changes") {
    RewardGui gui;
    mq_test::World world("lua_ui_reward_into");
    deliver(*world.conn, fixture::collection());
    RewardLua lua(*world.conn);
    lua.check(R"lua(
      into={}
      local s=hs.RewardSelection(nil, nil, into)
      lanes={s.pending, s.preview}
      return rawequal(s, into) and s.shown and #s.pending.tabs>=1 and s.preview.tabs~=nil
    )lua");
    lua.check(R"lua(
      local pr, vr=into.pending.revision, into.preview.revision
      collectgarbage('stop') local b=collectgarbage('count')
      local s=hs.RewardSelection(pr, vr, into)
      local a=collectgarbage('count') collectgarbage('restart')
      return rawequal(s, into) and rawequal(s.pending, lanes[1]) and rawequal(s.preview, lanes[2])
        and s.pending.unchanged and s.pending.tabs==nil and s.preview.unchanged and s.preview.tabs==nil and a==b
    )lua");
    // Changed again: the tabs come back in a new list.
    lua.check(R"lua(
      local s=hs.RewardSelection(nil, nil, into)
      return not s.pending.unchanged and #s.pending.tabs>=1
    )lua");
}
