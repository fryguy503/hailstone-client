// The modern buff trays' "Add to Blocked Buffs" (ui/modern/lib/bufftray.lua): the Blocked Buffs window's own edit box
// and Add over the hs API (app/lua_ui_dialogs.cpp BlockedBuffWindow), with a buff's spell id, against the mock server.
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/ini_file.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/packet_io.h"
#include "net/spell_packets.h"

using namespace hailstone;
using namespace hailstone::test;

TEST_CASE("Blocked buffs: a buff's spell id in the window's edit box, then Add, asks the server to block it") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        // Holy Armor (11), a beneficial spell, in the game folder's spell table (read when the connection is made).
        EmptyGameDir dir;
        std::ofstream(dir.path() / "spells_us.txt")
            << mq_test::spell_line({{0, "11"}, {1, "Holy Armor"}, {13, "2000"}, {17, "270"}, {83, "1"}, {98, "5"}})
            << "\n";
        GameDir game(dir.path());
        MockEqServer server(MockConfig::standard());
        net::GameConnection conn(game, quick_options());
        auto until = [&](const std::function<bool()>& done) {
            return pump([&] { conn.update(); server.poll(); }, done);
        };
        conn.start(server.ticket());
        REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
        conn.enter_world("Gennro");
        REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));
        REQUIRE(conn.spell_table().get(11));
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        REQUIRE(luaL_dostring(L, "hs.SetWindowText('BlockedBuffWnd', 'BW_EditSpellToBlock', '11') "
                                 "return hs.ClickChild('BlockedBuffWnd', 'BW_AddSpellToBlock', 'left')") == LUA_OK);
        CHECK(lua_toboolean(L, -1));
        lua_close(L);
        auto sent = [&] {
            return std::find_if(server.zone_log.begin(), server.zone_log.end(),
                                [](const net::AppPacket& p) { return net::is_op(p, net::Op::BlockedBuffs); });
        };
        REQUIRE(until([&] { return sent() != server.zone_log.end(); }));
        auto b = net::decode_blocked_buffs(*sent());
        REQUIRE(b);
        CHECK_FALSE(b->pet);
        REQUIRE_FALSE(b->spells.empty());
        CHECK(b->spells[0] == 11);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Blocked buffs: kept in the character ini, sent at the zone-in; a new block takes the spell group off") {
    // CEverQuest::ProcessLocalPCIni @0x51e770 reads [BlockedSpells] / [BlockedPetSpells] and sends them; the
    // handlers write all 30 slots back after each change. Handle_OP_BlockedBuffs @0x4bed90 takes the effects of a
    // spell its list did not have, and of that spell's SpellGroup, off the player or the pet.
    EmptyGameDir dir;
    {
        std::ofstream f(dir.path() / "spells_us.txt");
        // Holy Armor (11) and its rank II (12) share SpellGroup 900; Skin like Wood (13) is another.
        f << mq_test::spell_line({{0, "11"}, {1, "Holy Armor"}, {13, "2000"}, {17, "270"}, {83, "1"}, {98, "5"},
                                  {207, "900"}})
          << "\n";
        f << mq_test::spell_line({{0, "12"}, {1, "Holy Armor Rk. II"}, {13, "2000"}, {17, "270"}, {83, "1"},
                                  {98, "5"}, {207, "900"}})
          << "\n";
        f << mq_test::spell_line({{0, "13"}, {1, "Skin like Wood"}, {13, "2000"}, {17, "270"}, {83, "1"}, {98, "5"}})
          << "\n";
    }
    GameDir game(dir.path());
    IniFile ini;
    ini.set("BlockedSpells", "BlockedSpellID0", "-1");
    ini.set("BlockedSpells", "BlockedSpellID1", "13");
    ini.set("BlockedSpells", "BlockedSpellID2", "garbage");
    ini.set("BlockedPetSpells", "BlockedPetSpellID29", "13");
    MockEqServer server(MockConfig::standard());
    net::GameConnection conn(game, quick_options());
    conn.set_character_ini(
        [&](std::string_view section, std::string_view key, int fallback) { return ini.get_int(section, key, fallback); },
        [&](std::string_view section, std::string_view key, const std::string& value) { ini.set(section, key, value); });
    auto until = [&](const std::function<bool()>& done) { return pump([&] { conn.update(); server.poll(); }, done); };
    conn.start(server.ticket());
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
    conn.enter_world("Gennro");
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));
    // With a character ini the zone-in itself sends nothing; the app's restore does, from the ini.
    CHECK(MockEqServer::count(server.zone_log, net::Op::BlockedBuffs) == 0);
    conn.restore_blocked_buffs();
    REQUIRE(until([&] { return MockEqServer::count(server.zone_log, net::Op::BlockedBuffs) == 2; }));
    for (const auto& packet : server.zone_log) {
        if (!net::is_op(packet, net::Op::BlockedBuffs)) continue;
        auto sent = net::decode_blocked_buffs(packet);
        REQUIRE(sent);
        CHECK(sent->initialise);
        CHECK(sent->spells == std::vector<std::int32_t>{13});
    }
    CHECK(conn.blocked_buffs(false) == std::vector<std::int32_t>{13});
    CHECK(conn.blocked_buffs(true) == std::vector<std::int32_t>{13});

    // The player wears rank II (slot 4) and Skin like Wood (slot 5, blocked already); the pet wears rank II (slot 2).
    for (auto [slot, spell] : {std::pair{4u, 12u}, std::pair{5u, 13u}}) {
        net::BuffPacket active;
        active.entity_id = server.config.own_spawn_id;
        active.buff.spell_id = spell;
        active.buff.duration = 10;
        active.slot = slot;
        server.zone.send_reliable(net::encode(active));
    }
    constexpr std::uint32_t kPet = 4321;
    net::BuffIcons pet_icons;
    pet_icons.entity_id = kPet;
    pet_icons.all_buffs = 1;
    pet_icons.entries.push_back({2, 12, 10, 0, "Gennro"});
    auto pet_packet = net::encode(pet_icons, false);
    pet_packet.opcode = net::to_wire(net::Op::PetBuffWindow);
    server.zone.send_reliable(pet_packet);
    REQUIRE(until([&] {
        return conn.spell_state().buffs[4].spell == 12 && conn.spell_state().buffs[5].spell == 13 &&
               conn.pet_buffs().size() == 1;
    }));
    const auto removals = [&] { return MockEqServer::count(server.zone_log, net::Op::BuffRemoveRequest); };

    // An initialise answer takes nothing off.
    net::BlockedBuffs answer;
    answer.spells = {13, 11};
    net::BlockedBuffs restored = answer;
    restored.initialise = true;
    server.zone.send_reliable(net::encode(restored));
    REQUIRE(until([&] { return conn.blocked_buffs(false) == answer.spells; }));
    CHECK(removals() == 0);
    // The list as it was, then the server's answer to adding Holy Armor: rank II comes off by its SpellGroup, Skin
    // like Wood (blocked before) stays.
    net::BlockedBuffs before;
    before.spells = {13};
    before.initialise = true;
    server.zone.send_reliable(net::encode(before));
    REQUIRE(until([&] { return conn.blocked_buffs(false) == before.spells; }));
    server.zone.send_reliable(net::encode(answer));
    REQUIRE(until([&] { return removals() == 1; }));
    auto removed = std::find_if(server.zone_log.begin(), server.zone_log.end(),
                                [](const net::AppPacket& p) { return net::is_op(p, net::Op::BuffRemoveRequest); });
    CHECK(removed->data == net::encode_buff_remove_request(4, server.config.own_spawn_id).data);
    CHECK(ini.get_int("BlockedSpells", "BlockedSpellID0", 0) == 13);
    CHECK(ini.get_int("BlockedSpells", "BlockedSpellID1", 0) == 11);
    CHECK(ini.get_int("BlockedSpells", "BlockedSpellID2", 0) == -1);
    CHECK(ini.get_int("BlockedSpells", "BlockedSpellID29", 0) == -1);

    // The pet's: rank II comes off the pet.
    net::BlockedBuffs pet = answer;
    pet.pet = true;
    server.zone.send_reliable(net::encode(pet));
    REQUIRE(until([&] { return removals() == 2; }));
    removed = std::find_if(server.zone_log.rbegin(), server.zone_log.rend(),
                           [](const net::AppPacket& p) { return net::is_op(p, net::Op::BuffRemoveRequest); })
                  .base() -
              1;
    CHECK(removed->data == net::encode_buff_remove_request(2, kPet).data);
    CHECK(ini.get_int("BlockedPetSpells", "BlockedPetSpellID1", 0) == 11);
    CHECK(ini.get_int("BlockedPetSpells", "BlockedPetSpellID29", 0) == -1);

    // Removing one and clearing write the ini again.
    net::BlockedBuffs gone;
    gone.spells = {13};
    server.zone.send_reliable(net::encode(gone, true)); // OP_RemoveBlockedBuffs
    server.zone.send_reliable(net::encode_clear_blocked_buffs(true));
    REQUIRE(until([&] { return conn.blocked_buffs(false).size() == 1 && conn.blocked_buffs(true).empty(); }));
    CHECK(ini.get_int("BlockedSpells", "BlockedSpellID0", 0) == 11);
    CHECK(ini.get_int("BlockedSpells", "BlockedSpellID1", 0) == -1);
    CHECK(ini.get_int("BlockedPetSpells", "BlockedPetSpellID0", 0) == -1);
    CHECK(removals() == 2);
}
