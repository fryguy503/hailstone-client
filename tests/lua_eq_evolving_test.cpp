#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_caps.h"
#include "game/item_links.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "plugin/policy.h"

using namespace hailstone;

namespace {
struct TransferLua {
    app::EqHost host;
    app::EqApi api{host};
    lua_State* state = luaL_newstate();
    explicit TransferLua(net::GameConnection& connection) {
        host.conn = &connection;
        host.in_game = host.ui_ready = true;
        luaL_openlibs(state);
        lua_newtable(state);
        api.register_functions(state);
        lua_setglobal(state, "eq");
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

TEST_CASE("eq evolving transfer: exact identities, approved pair and one confirmed request") {
    CHECK(app::eq_call_caps("ItemExpTransfer") == 0);
    CHECK(app::eq_call_caps("ItemDisplayCanTransfer") == 0);
    CHECK(app::eq_call_caps("ItemExpTransferSelect") == plugin::kCapActions);
    CHECK(app::eq_call_caps("ItemExpTransferConfirm") == plugin::kCapActions);
    CHECK(app::eq_call_caps("ItemDisplayTransfer") == plugin::kCapActions);
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
      return eq.ItemDisplayCanTransfer('ItemDisplayWindow#2')
        and not eq.ItemDisplayCanTransfer('ItemDisplayWindow#3')
        and not eq.ItemDisplayCanTransfer('ItemDisplayWindow#4')
        and not eq.ItemDisplayCanTransfer('ItemDisplayWindow#5')
        and not eq.ItemDisplayCanTransfer('ItemDisplayWindow#1')
        and not eq.ItemDisplayTransfer('ItemDisplayWindow#3')
    )lua");
    lua.check(R"lua(
      local s=eq.ItemExpTransfer()
      return not s.shown and not s.canTransfer and #s.items==0
        and eq.ItemDisplayCanTransfer('ItemDisplayWindow')
        and eq.ItemDisplayTransfer('ItemDisplayWindow')
    )lua");
    lua.check(R"lua(
      local s=eq.ItemExpTransfer()
      return s.shown and s.source=='18446744073709551615' and s.destination==''
        and type(s.revision)=='string' and #s.items==2 and s.items[1].progress==12.5
        and not eq.ItemExpTransferSelect('destination',18446744073709551614)
        and not eq.ItemExpTransferSelect('destination','018446744073709551614')
        and not eq.ItemExpTransferSelect('destination','18446744073709551616')
        and not eq.ItemExpTransferSelect('destination',s.source)
        and eq.ItemExpTransferSelect('destination','18446744073709551614')
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
      local s=eq.ItemExpTransfer()
      _G.approved=s
      return s.compatible and s.canTransfer and not s.pending and s.maxTransferLevel==4
        and s.compatibility==75 and s.previewDestination.name=='Projected destination'
        and s.items[2].name=='Evolving 102'
        and not eq.ItemExpTransferConfirm(s.destination,s.source,s.revision)
        and not eq.ItemExpTransferConfirm(s.source,s.destination,'0')
        and not eq.ItemExpTransferConfirm(s.source,s.destination,tonumber(s.revision))
        and eq.ItemExpTransferConfirm(s.source,s.destination,s.revision)
        and not eq.ItemExpTransferConfirm(s.source,s.destination,s.revision)
        and eq.ItemExpTransfer().submitted
    )lua");
    lua.check(R"lua(
      eq.ShowWindow('ItemExpTransferWnd',false)
      return not eq.ItemExpTransfer().shown
        and not eq.ItemExpTransferConfirm(approved.source,approved.destination,approved.revision)
    )lua");
}

TEST_CASE("eq evolving transfer: clearing, removed items and inactive sessions invalidate approval") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    const auto source = transfer_item(201, 11, 23);
    const auto destination = transfer_item(202, 12, 24);
    auto& inventory = net::GameConnectionTestAccess::inventory(rig.conn);
    REQUIRE(inventory.put(source.slot, source));
    REQUIRE(inventory.put(destination.slot, destination));
    TransferLua lua(rig.conn);
    lua.check(R"lua(
      eq.ShowWindow('ItemExpTransferWnd',true)
      return eq.ItemExpTransfer().shown
        and eq.ItemExpTransferSelect('source','11')
        and eq.ItemExpTransferSelect('destination','12')
    )lua");
    deliver(rig.conn, net::encode(net::EvolvingTransferDetails{11,12,100,5,source,destination}));
    lua.check(R"lua(
      _G.before=eq.ItemExpTransfer()
      return before.canTransfer and eq.ItemExpTransferSelect('destination','')
        and eq.ItemExpTransfer().destination==''
        and not eq.ItemExpTransferConfirm(before.source,before.destination,before.revision)
        and eq.ItemExpTransferSelect('destination','12')
    )lua");
    deliver(rig.conn, net::encode(net::EvolvingTransferDetails{11,12,100,5,source,destination}));
    REQUIRE(inventory.take(source.slot));
    lua.check(R"lua(
      local s=eq.ItemExpTransfer()
      return not s.canTransfer and not eq.ItemExpTransferConfirm(s.source,s.destination,s.revision)
    )lua");
    lua.host.in_game = false;
    lua.check(R"lua(
      local s=eq.ItemExpTransfer()
      return not s.shown and #s.items==0 and not s.canTransfer
        and not eq.ItemExpTransferSelect('source','11')
        and not eq.ItemExpTransferConfirm('11','12','1')
    )lua");
}

TEST_CASE("eq evolving Final Result: validates the displayed link and sends the native six words") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    TransferLua lua(rig.conn);
    auto source = transfer_item(301, 31, 23);
    source.evolving.final_item_id = 305;
    std::vector<net::Item> displays{source};
    lua.host.item_displays = &displays;
    const auto hash = game::item_link_hash(game::item_link_hash_input(source));
    const auto link = "1^301^305^2^" + std::to_string(hash);
    lua.check("return eq.StmlLink('ItemDisplayWindow','IDW_ItemInfo1',27,'" + link + "')");
    lua.check("return eq.StmlLink('ItemDisplayWindow','IDW_ItemInfo1',27,'" + link + "junk')");
    lua.check("return eq.StmlLink('ItemDisplayWindow','IDW_ItemInfo1',27,'1^999^305^2^" + std::to_string(hash) + "')");
    lua.check("return eq.StmlLink('ItemDisplayWindow','IDW_ItemInfo1',27,'" + link + "')");
    const auto count = [&] {
        return std::count_if(rig.server.zone_log.begin(), rig.server.zone_log.end(), [](const net::AppPacket& packet) {
            if (packet.opcode != net::to_wire(net::Op::EvolveItem) || packet.data.size() != 24) return false;
            ByteReader reader(packet.data);
            return reader.le<std::uint32_t>() == 4;
        });
    };
    REQUIRE(rig.until([&] { return count() >= 2; }));
    CHECK(count() == 2);
    const auto expected = net::encode(net::EvolvingFinalResult{301,305,2,hash,true});
    for (const auto& packet : rig.server.zone_log)
        if (packet.opcode == net::to_wire(net::Op::EvolveItem)) CHECK(packet.data == expected.data);
}
