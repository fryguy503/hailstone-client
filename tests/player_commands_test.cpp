// MacroQuest's commands that act for the player (GameConnection::removelev_command, removeaura_command and
// memspell_command; the app registers them with /makemevisible and /click): against the mock server.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
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
