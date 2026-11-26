// MacroQuest's commands that act for the player (GameConnection::removelev_command, removeaura_command,
// removebuff_command and memspell_command; the app registers them with /makemevisible and /click): against the
// mock server.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/opcodes.h"

using namespace hailstone;
using namespace hailstone::test;
using A = net::GameConnectionTestAccess;

namespace {

std::vector<std::string> chat_lines(net::GameConnection& conn) {
    std::vector<std::string> out;
    while (auto e = conn.poll_chat()) out.push_back(e->text);
    return out;
}

bool said(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
}

// Every class may use these at level 1 (columns 104..119).
std::shared_ptr<game::SpellTable> test_spells() {
    auto spells = std::make_shared<game::SpellTable>();
    std::initializer_list<std::pair<int, std::string>> all_classes = {
        {104, "1"}, {105, "1"}, {106, "1"}, {107, "1"}, {108, "1"}, {109, "1"}, {110, "1"}, {111, "1"},
        {112, "1"}, {113, "1"}, {114, "1"}, {115, "1"}, {116, "1"}, {117, "1"}, {118, "1"}, {119, "1"}};
    auto with = [&](std::initializer_list<std::pair<int, std::string>> set) {
        std::vector<std::pair<int, std::string>> cols(all_classes);
        cols.insert(cols.end(), set.begin(), set.end());
        std::vector<std::string> fields(237, "0");
        for (int i = 1; i <= 8; ++i) fields[static_cast<std::size_t>(i)].clear();
        for (const auto& [c, v] : cols) fields[static_cast<std::size_t>(c)] = v;
        std::string line;
        for (std::size_t i = 0; i < fields.size(); ++i) line += (i ? "^" : "") + fields[i];
        return line;
    };
    // A levitation (SPA 57) and a buff without one, both beneficial; a spell for level 70 only.
    REQUIRE(spells->parse(with({{0, "3009"}, {1, "Test Levitate"}, {17, "10"}, {20, "1"}, {83, "1"}, {86, "57"}}) + "\n" +
                          with({{0, "3010"}, {1, "Test Armor"}, {17, "10"}, {20, "5"}, {83, "1"}, {86, "1"}}) + "\n" +
                          mq_test::spell_line({{0, "3011"}, {1, "Test Lofty"}, {83, "1"}, {104, "70"}, {105, "70"},
                                               {106, "70"}, {107, "70"}})) == 3);
    return spells;
}

// The mock's character is a warrior, whom the spell book refuses (it opens for spellcasters): a cleric here.
void make_caster(net::GameConnection& c) {
    auto profile = *c.profile();
    profile.class_ = 2;
    c.set_profile_for_test(profile);
}

// The last OP_BuffRemoveRequest the server had: buff slot, entity.
std::pair<std::uint32_t, std::uint32_t> last_buff_remove(const std::vector<net::AppPacket>& log) {
    const auto it = std::find_if(log.rbegin(), log.rend(), [](const net::AppPacket& p) {
        return p.opcode == net::to_wire(net::Op::BuffRemoveRequest);
    });
    REQUIRE(it != log.rend());
    REQUIRE(it->data.size() >= 8);
    auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(it->data[at + i]) << (8 * i);
        return v;
    };
    return {u32(0), u32(4)};
}

} // namespace

TEST_CASE("player commands: /removelev clicks off each levitation buff and says so") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    c.set_spell_table_for_test(test_spells());
    auto& sp = A::spells(c);
    sp.buffs[2].spell = 3009;
    sp.buffs[2].tics = 10;
    sp.buffs[4].spell = 3010;
    sp.buffs[4].tics = 10;
    chat_lines(c);
    const int before = MockEqServer::count(rig.server.zone_log, net::Op::BuffRemoveRequest);
    c.removelev_command();
    CHECK(said(chat_lines(c), "Removing: Test Levitate"));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::BuffRemoveRequest) > before; }));
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::BuffRemoveRequest) == before + 1); // the armor stays
    // No levitation: nothing.
    sp.buffs[2] = {};
    c.removelev_command();
    CHECK(chat_lines(c).empty());
}

TEST_CASE("player commands: /removeaura removes each aura whose name holds the text") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    chat_lines(c);
    c.removeaura_command("  ");
    CHECK(said(chat_lines(c), "Usage: /removeaura <auraname> or <aurapartialname>"));
    auto& auras = A::auras(c);
    auras.push_back({net::kAuraAdd, 0, 501, 0, "Aura of the Muse"});
    auras.push_back({net::kAuraAdd, 0, 502, 0, "Champion's Aura"});
    const int before = MockEqServer::count(rig.server.zone_log, net::Op::UpdateAura);
    c.removeaura_command("muse");
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::UpdateAura) > before; }));
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::UpdateAura) == before + 1);
    // Any case, any part: both.
    c.removeaura_command("AURA");
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::UpdateAura) == before + 3; }));
}

TEST_CASE("player commands: /memspell memorizes a spell of the book into one gem, by name or id") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    make_caster(c);
    c.set_spell_table_for_test(test_spells());
    auto& sp = A::spells(c);
    sp.book[0] = 3010;
    sp.book[1] = 3009;
    sp.book[2] = 3011;
    // Out of range, not in the book, a level the player has not: nothing.
    c.memspell_command("13 \"Test Armor\"");
    CHECK_FALSE(c.memorizing_set());
    c.memspell_command("0 \"Test Armor\"");
    CHECK_FALSE(c.memorizing_set());
    c.memspell_command("3 \"Minor Healing\"");
    CHECK_FALSE(c.memorizing_set());
    c.memspell_command("3 \"Test Lofty\"");
    CHECK_FALSE(c.memorizing_set());
    c.memspell_command("3 \"Test\""); // the name is exact
    CHECK_FALSE(c.memorizing_set());
    // By the exact name, any case: gem 3's set memorization starts (MemorizeSet with that gem alone).
    c.memspell_command("3 \"test levitate\"");
    CHECK(c.memorizing_set());
}

TEST_CASE("player commands: /memspell takes a spell id of the book, and an unquoted name") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    make_caster(c);
    c.set_spell_table_for_test(test_spells());
    auto& sp = A::spells(c);
    sp.book[0] = 3010;
    c.memspell_command("2 3009"); // not in the book
    CHECK_FALSE(c.memorizing_set());
    c.memspell_command("2 3010");
    CHECK(c.memorizing_set());
}

TEST_CASE("player commands: /memspell's unquoted name is the rest of the line") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    make_caster(c);
    c.set_spell_table_for_test(test_spells());
    A::spells(c).book[5] = 3010;
    c.memspell_command("1 Test Armor");
    CHECK(c.memorizing_set());
}

TEST_CASE("player commands: /removebuff clicks off the player's first buff whose name matches") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    c.set_spell_table_for_test(test_spells());
    auto& sp = A::spells(c);
    sp.buffs[3].spell = 3010; // Test Armor
    sp.buffs[3].tics = 10;
    sp.buffs[45].spell = 3009; // Test Levitate, a song slot
    sp.buffs[45].tics = 10;
    auto& log = rig.server.zone_log;
    const int before = MockEqServer::count(log, net::Op::BuffRemoveRequest);
    // No match, an exact name that is only a part, nothing named: nothing sent.
    c.removebuff_command("Spirit of Wolf");
    c.removebuff_command("=Test");
    c.removebuff_command("");
    c.removebuff_command("-pet test"); // no pet
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(MockEqServer::count(log, net::Op::BuffRemoveRequest) == before);
    // Any part, any case: the first match only.
    c.removebuff_command("test");
    REQUIRE(rig.until([&] { return MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 1; }));
    CHECK(last_buff_remove(log) == std::pair{3u, c.world().player_id});
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 1);
    // A song, by its whole name; quoted.
    c.removebuff_command("\"=test levitate\"");
    REQUIRE(rig.until([&] { return MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 2; }));
    CHECK(last_buff_remove(log) == std::pair{45u, c.world().player_id});
}

TEST_CASE("player commands: /removebuff -pet and -both click off the focused pet's buff") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    c.set_spell_table_for_test(test_spells());
    auto pet = make_spawn("Test Pet", 104, 1, 36, -280.0f, -148.0f, 3.5f);
    pet.is_pet = true;
    pet.pet_owner_id = c.world().player_id;
    rig.server.add_spawn(pet);
    REQUIRE(rig.until([&] { return c.pets().focused == pet.spawn_id; }));
    auto& pet_buffs = A::pet_buffs(c);
    game::BuffSlot armor;
    armor.spell = 3010;
    armor.tics = 10;
    game::BuffSlot lev = armor;
    lev.spell = 3009;
    pet_buffs = {{9, armor}, {4, lev}};
    auto& sp = A::spells(c);
    sp.buffs[0] = armor;
    auto& log = rig.server.zone_log;
    const int before = MockEqServer::count(log, net::Op::BuffRemoveRequest);
    // The pet's only: its window's first match, by slot.
    c.removebuff_command("-pet test");
    REQUIRE(rig.until([&] { return MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 1; }));
    CHECK(last_buff_remove(log) == std::pair{4u, pet.spawn_id});
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 1);
    // Both: the pet's, then the player's.
    c.removebuff_command("-both Test Armor");
    REQUIRE(rig.until([&] { return MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 3; }));
    CHECK(last_buff_remove(log) == std::pair{0u, c.world().player_id});
    // /removepetbuff: the pet's first match only, never the player's (E3's HUD sends it).
    c.removepetbuff_command("=test armor");
    REQUIRE(rig.until([&] { return MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 4; }));
    CHECK(last_buff_remove(log) == std::pair{9u, pet.spawn_id});
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 4);
    // Nothing named, or no pet buff of that name: nothing sent.
    c.removepetbuff_command("");
    c.removepetbuff_command("Spirit of Wolf");
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(MockEqServer::count(log, net::Op::BuffRemoveRequest) == before + 4);
}
