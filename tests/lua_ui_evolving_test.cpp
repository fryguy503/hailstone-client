#include <doctest/doctest.h>

#include <limits>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "plugin/policy.h"

using namespace hailstone;

namespace {
struct TransferLua {
    app::UiHost host;
    app::UiApi api{host};
    lua_State* state = luaL_newstate();
    explicit TransferLua(net::GameConnection& connection) {
        host.conn = &connection;
        host.in_game = host.ui_ready = true;
        luaL_openlibs(state);
        lua_newtable(state);
        api.register_functions(state);
        lua_setglobal(state, "hs");
    }
    ~TransferLua() { lua_close(state); }
    void check(const std::string& code) {
        INFO(code);
        const auto top = lua_gettop(state);
        const auto status = luaL_dostring(state, code.c_str());
        REQUIRE_MESSAGE(status == LUA_OK, (status == LUA_OK ? "" : lua_tostring(state, -1)));
        REQUIRE(lua_isboolean(state, -1));
        CHECK(lua_toboolean(state, -1));
        lua_settop(state, top);
    }
};
net::Item transfer_item(std::uint32_t number, std::uint64_t record, int slot) {
    net::Item item;
    item.id = number;
    item.name = "Evolving " + std::to_string(number);
    item.is_evolving = 1;
    item.instance_id = static_cast<std::uint32_t>(record);
    item.parcel_item_id = static_cast<std::uint32_t>(record >> 32);
    item.slot = net::ItemSlot::possessions(static_cast<std::int16_t>(slot));
    item.evolving.evolve_level = 2;
    item.evolving.evolve_max_level = 5;
    item.evolving.progress = 12.5;
    return item;
}
void deliver(net::GameConnection& connection, const net::AppPacket& packet) {
    net::GameConnectionTestAccess::handle_zone(connection, packet);
}
} // namespace

TEST_CASE("hs evolving transfer: exact identities, approved pair and one confirmed request") {
    CHECK(app::ui_call_caps("ItemExpTransfer") == 0);
    CHECK(app::ui_call_caps("ItemDisplayCanTransfer") == 0);
    CHECK(app::ui_call_caps("ItemExpTransferSelect") == plugin::kCapActions);
    CHECK(app::ui_call_caps("ItemExpTransferConfirm") == plugin::kCapActions);
    CHECK(app::ui_call_caps("ItemDisplayTransfer") == plugin::kCapActions);
    test::Rig rig;
    REQUIRE(rig.to_zone());
    constexpr auto source_id = std::numeric_limits<std::uint64_t>::max();
    constexpr auto destination_id = source_id - 1;
    const auto source = transfer_item(101, source_id, 23);
    const auto destination = transfer_item(102, destination_id, 24);
    auto& inventory = net::GameConnectionTestAccess::inventory(rig.conn);
    REQUIRE(inventory.put(source.slot, source));
    REQUIRE(inventory.put(destination.slot, destination));
    TransferLua lua(rig.conn);
    std::vector<net::Item> displays{source};
    lua.host.item_displays = &displays;
    displays.push_back(destination);
    auto invalid = source;
    invalid.id = 999;
    displays.push_back(invalid);
    invalid = source;
    invalid.instance_id = 1;
    displays.push_back(invalid);
    invalid = source;
    invalid.is_evolving = 0;
    displays.push_back(invalid);
    lua.check(R"lua(
      return hs.ItemDisplayCanTransfer('ItemDisplayWindow#2')
        and not hs.ItemDisplayCanTransfer('ItemDisplayWindow#3')
        and not hs.ItemDisplayCanTransfer('ItemDisplayWindow#4')
        and not hs.ItemDisplayCanTransfer('ItemDisplayWindow#5')
        and not hs.ItemDisplayCanTransfer('ItemDisplayWindow#1')
        and not hs.ItemDisplayTransfer('ItemDisplayWindow#3')
    )lua");
    lua.check(R"lua(
      local s=hs.ItemExpTransfer()
      return not s.shown and not s.canTransfer and #s.items==0
        and hs.ItemDisplayCanTransfer('ItemDisplayWindow')
        and hs.ItemDisplayTransfer('ItemDisplayWindow')
    )lua");
    lua.check(R"lua(
      local s=hs.ItemExpTransfer()
      return s.shown and s.source=='18446744073709551615' and s.destination==''
        and type(s.revision)=='string' and #s.items==2 and s.items[1].progress==12.5
        and not hs.ItemExpTransferSelect('destination',18446744073709551614)
        and not hs.ItemExpTransferSelect('destination','018446744073709551614')
        and not hs.ItemExpTransferSelect('destination','18446744073709551616')
        and not hs.ItemExpTransferSelect('destination',s.source)
        and hs.ItemExpTransferSelect('destination','18446744073709551614')
    )lua");
    net::EvolvingTransferDetails details;
    details.source_id = source_id;
    details.destination_id = destination_id;
    details.compatibility = 75;
    details.max_transfer_level = 4;
    details.source = source;
    details.destination = destination;
    details.destination->name = "Projected destination";
    details.destination->evolving.evolve_level = 4;
    deliver(rig.conn, net::encode(details));
    lua.check(R"lua(
      local s=hs.ItemExpTransfer()
      _G.approved=s
      return s.compatible and s.canTransfer and not s.pending and s.maxTransferLevel==4
        and s.compatibility==75 and s.previewDestination.name=='Projected destination'
        and s.items[2].name=='Evolving 102'
        and not hs.ItemExpTransferConfirm(s.destination,s.source,s.revision)
        and not hs.ItemExpTransferConfirm(s.source,s.destination,'0')
        and not hs.ItemExpTransferConfirm(s.source,s.destination,tonumber(s.revision))
        and hs.ItemExpTransferConfirm(s.source,s.destination,s.revision)
        and not hs.ItemExpTransferConfirm(s.source,s.destination,s.revision)
        and hs.ItemExpTransfer().submitted
    )lua");
    lua.check(R"lua(
      hs.ShowWindow('ItemExpTransferWnd',false)
      return not hs.ItemExpTransfer().shown
        and not hs.ItemExpTransferConfirm(approved.source,approved.destination,approved.revision)
    )lua");
}

TEST_CASE("hs evolving transfer: clearing, removed items and inactive sessions invalidate approval") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    const auto source = transfer_item(201, 11, 23);
    const auto destination = transfer_item(202, 12, 24);
    auto& inventory = net::GameConnectionTestAccess::inventory(rig.conn);
    REQUIRE(inventory.put(source.slot, source));
    REQUIRE(inventory.put(destination.slot, destination));
    TransferLua lua(rig.conn);
    lua.check(R"lua(
      hs.ShowWindow('ItemExpTransferWnd',true)
      return hs.ItemExpTransfer().shown
        and hs.ItemExpTransferSelect('source','11')
        and hs.ItemExpTransferSelect('destination','12')
    )lua");
    deliver(rig.conn, net::encode(net::EvolvingTransferDetails{11,12,100,5,source,destination}));
    lua.check(R"lua(
      _G.before=hs.ItemExpTransfer()
      return before.canTransfer and hs.ItemExpTransferSelect('destination','')
        and hs.ItemExpTransfer().destination==''
        and not hs.ItemExpTransferConfirm(before.source,before.destination,before.revision)
        and hs.ItemExpTransferSelect('destination','12')
    )lua");
    deliver(rig.conn, net::encode(net::EvolvingTransferDetails{11,12,100,5,source,destination}));
    REQUIRE(inventory.take(source.slot));
    lua.check(R"lua(
      local s=hs.ItemExpTransfer()
      return not s.canTransfer and not hs.ItemExpTransferConfirm(s.source,s.destination,s.revision)
    )lua");
    lua.host.in_game = false;
    lua.check(R"lua(
      local s=hs.ItemExpTransfer()
      return not s.shown and #s.items==0 and not s.canTransfer
        and not hs.ItemExpTransferSelect('source','11')
        and not hs.ItemExpTransferConfirm('11','12','1')
    )lua");
}

TEST_CASE("hs evolving transfer: a kept record is refilled in place, with no garbage while nothing changes") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    TransferLua lua(rig.conn);
    lua.check(R"lua(
      into={previewSource={}, items={}}
      local s=hs.ItemExpTransfer(into)
      return rawequal(s, into) and not s.shown and #s.items==0 and s.previewSource==nil
    )lua");
    lua.check(R"lua(
      local items=into.items
      collectgarbage('stop') local b=collectgarbage('count')
      local s=hs.ItemExpTransfer(into)
      local a=collectgarbage('count') collectgarbage('restart')
      return rawequal(s, into) and rawequal(s.items, items) and a==b
    )lua");
    // Without it, a new table with the same fields, as before.
    lua.check(R"lua(
      local n=hs.ItemExpTransfer()
      return not rawequal(n, into) and n.shown==into.shown and n.revision==into.revision and n.status==into.status
    )lua");
}
