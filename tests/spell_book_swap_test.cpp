// Moving spells within the spell book (GameConnection::spell_book_swap_click): CSpellBookWnd's quick right click
// (Send_OP_SwapSpell @0x75ceb0) and the server's echo (Handle_OP_SwapSpell @0x75d380), against the mock EQEmu.

#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "net/spell_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

std::uint32_t u32_at(const AppPacket& p, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, p.data.data() + at, 4);
    return v;
}

const AppPacket& last_swap(const std::vector<AppPacket>& log) {
    for (auto it = log.rbegin(); it != log.rend(); ++it)
        if (from_wire(it->opcode) == Op::SwapSpell) return *it;
    FAIL("no OP_SwapSpell sent");
    return log.back();
}

std::vector<std::string> drain_chat(GameConnection& c) {
    std::vector<std::string> lines;
    while (auto e = c.poll_chat()) lines.push_back(e->text);
    return lines;
}

// spells_us.txt lines: id, name, field 149 (Deletable).
std::shared_ptr<game::SpellTable> two_spells() {
    auto line = [](const char* id, const char* name, const char* deletable) {
        std::vector<std::string> f(237, "0");
        f[0] = id;
        f[1] = name;
        f[149] = deletable;
        std::string s;
        for (std::size_t i = 0; i < f.size(); ++i) s += (i ? "^" : "") + f[i];
        return s;
    };
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line("200", "Minor Healing", "0") + "\n" + line("201", "Courage", "1")) == 2);
    return table;
}

} // namespace

TEST_CASE("OP_SwapSpell: {u32 from, u32 to}") {
    AppPacket p = encode(SwapSpell{3, 17});
    CHECK(from_wire(p.opcode) == Op::SwapSpell);
    REQUIRE(p.data.size() == kSwapSpellSize);
    CHECK(u32_at(p, 0) == 3);
    CHECK(u32_at(p, 4) == 17);
    auto d = decode_swap_spell(p);
    REQUIRE(d);
    CHECK(d->from_slot == 3);
    CHECK(d->to_slot == 17);
}

TEST_CASE("spell book: a right click picks a slot up, the next sends OP_SwapSpell, the echo swaps the slots") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.set_spell_table_for_test(two_spells());
    // Two spells scribed into book slots 0 and 9 (OP_MemorizeSpell, scribing 0).
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{0, 200, kMemScribe, 0}));
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{9, 201, kMemScribe, 0}));
    REQUIRE(conn.spell_state().book[0] == 200);
    REQUIRE(conn.spell_state().book[9] == 201);
    auto swaps = [&] { return MockEqServer::count(rig.server.zone_log, Op::SwapSpell); };

    // Moving (the controlled spawn's SpeedRun above 0) and standing: nothing.
    conn.set_speed_run(1.0f);
    conn.spell_book_swap_click(0);
    CHECK(conn.spell_book_swap_slot() == -1);
    conn.set_sitting(true);
    REQUIRE(conn.sitting());
    drain_chat(conn);

    // The first click picks slot 0 up (12046); the same slot again puts it back, silently.
    conn.spell_book_swap_click(0);
    CHECK(conn.spell_book_swap_slot() == 0);
    auto lines = drain_chat(conn);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].rfind("[12046]", 0) == 0); // no eqstr_us.txt here: the id
    conn.spell_book_swap_click(0);
    CHECK(conn.spell_book_swap_slot() == -1);
    CHECK(drain_chat(conn).empty());

    // A Deletable spell adds 4027.
    conn.spell_book_swap_click(9);
    CHECK(conn.spell_book_swap_slot() == 9);
    lines = drain_chat(conn);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("[4027]", 0) == 0);

    // Another slot sends {picked, slot}; the pick stays until the echo, and the book is unchanged until then.
    conn.spell_book_swap_click(20);
    REQUIRE(rig.until([&] { return swaps() == 1; }));
    const AppPacket sent = last_swap(rig.server.zone_log);
    REQUIRE(sent.data.size() == kSwapSpellSize);
    CHECK(u32_at(sent, 0) == 9);
    CHECK(u32_at(sent, 4) == 20);
    CHECK(conn.spell_book_swap_slot() == 9);
    CHECK(conn.spell_state().book[9] == 201);

    // EQEmu echoes the packet: the slots swap (an empty slot too), 1388, the pick is dropped.
    conn.receive_zone_packet_for_test(sent);
    CHECK(conn.spell_state().book[20] == 201);
    CHECK(conn.spell_state().book[9] == game::kNoSpell);
    CHECK(conn.spell_book_swap_slot() == -1);
    lines = drain_chat(conn);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].rfind("[1388]", 0) == 0); // Swapping Spell Book Scribe slots.

    // Two spells swap places.
    conn.spell_book_swap_click(20);
    conn.spell_book_swap_click(0);
    REQUIRE(rig.until([&] { return swaps() == 2; }));
    conn.receive_zone_packet_for_test(last_swap(rig.server.zone_log));
    CHECK(conn.spell_state().book[0] == 201);
    CHECK(conn.spell_state().book[20] == 200);

    // An echo with a slot out of the book is ignored (the original breaks into the debugger).
    conn.receive_zone_packet_for_test(encode(SwapSpell{0, 720}));
    CHECK(conn.spell_state().book[0] == 201);
}

TEST_CASE("hs.SpellBookMemorize: with replace_gem_spell off, an empty gem only (StartSpellMemorization, bReplace 0)") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.set_spell_table_for_test(two_spells());
    REQUIRE(conn.profile());
    PlayerProfile pp = *conn.profile();
    pp.class_ = 2; // a cleric: the book opens
    conn.set_profile_for_test(pp);
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{0, 200, kMemScribe, 0}));
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{1, 201, kMemScribe, 0}));
    // Gem 0 holds Minor Healing (OP_MemorizeSpell, memorize).
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{0, 200, kMemMemorize, 0}));
    REQUIRE(conn.spell_state().gems[0].spell == 200);
    conn.set_sitting(true);
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.get_option = [](std::string_view o) { return o == "replace_gem_spell" ? 0 : 1; };
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        REQUIRE(luaL_dostring(L, "hs.SpellBookMemorize(1, 0)") == LUA_OK); // gem 0 is taken: nothing
        CHECK_FALSE(conn.spell_state().memorizing.has_value());
        REQUIRE(luaL_dostring(L, "hs.SpellBookMemorize(1, 1)") == LUA_OK); // gem 1 is empty
        REQUIRE(conn.spell_state().memorizing.has_value());
        CHECK(conn.spell_state().memorizing->gem == 1);
        CHECK(conn.spell_state().memorizing->spell == 201);
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

// Hailstone's replace_gem_spell (on by default): a book spell dropped on a gem that holds another spell memorizes
// there as into an empty gem. OP_MemorizeSpell goes out for that gem; EQEmu's Client::MemSpell forgets the old spell
// (OP_MemorizeSpell 2) and memorizes the new (1), and the forget says nothing ("You forget" is for a forget asked
// for). The gem keeps its spell until then. A spell already in a gem is still refused.
TEST_CASE("hs.SpellBookMemorize: with replace_gem_spell on, a spell memorizes over the gem's spell") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.set_spell_table_for_test(two_spells());
    REQUIRE(conn.profile());
    PlayerProfile pp = *conn.profile();
    pp.class_ = 2; // a cleric: the book opens
    conn.set_profile_for_test(pp);
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{0, 200, kMemScribe, 0}));
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{1, 201, kMemScribe, 0}));
    conn.receive_zone_packet_for_test(encode(MemorizeSpell{0, 200, kMemMemorize, 0})); // gem 0: Minor Healing
    REQUIRE(conn.spell_state().gems[0].spell == 200);
    conn.set_sitting(true);
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        bool replace = true;
        host.get_option = [&replace](std::string_view o) { return o == "replace_gem_spell" ? (replace ? 1 : 0) : 0; };
        CHECK(host.gem_takes_book_spell(0));
        CHECK(host.gem_takes_book_spell(1));
        CHECK_FALSE(host.gem_takes_book_spell(-1));
        CHECK_FALSE(host.gem_takes_book_spell(game::kSpellGems));
        replace = false;
        CHECK_FALSE(host.gem_takes_book_spell(0));
        CHECK(host.gem_takes_book_spell(1));
        replace = true;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        // the gem's own spell again: refused as already memorized
        REQUIRE(luaL_dostring(L, "hs.SpellBookMemorize(0, 0)") == LUA_OK);
        CHECK_FALSE(conn.spell_state().memorizing.has_value());
        REQUIRE(luaL_dostring(L, "hs.SpellBookMemorize(1, 0)") == LUA_OK);
        REQUIRE(conn.spell_state().memorizing.has_value());
        CHECK(conn.spell_state().memorizing->gem == 0);
        CHECK(conn.spell_state().memorizing->spell == 201);
        CHECK(conn.spell_state().gems[0].spell == 200); // kept until the server answers
        // the memorize runs to its end: OP_MemorizeSpell {gem 0, 201, memorize}
        auto mems = [&] { return MockEqServer::count(rig.server.zone_log, Op::MemorizeSpell); };
        const auto before = mems();
        std::vector<std::int64_t> steps;
        const std::int64_t t0 = GameConnection::now_ms();
        for (int i = 1; i <= 400; ++i) steps.push_back(t0 + i * 1000);
        conn.set_game_steps(steps);
        REQUIRE(rig.until([&] { return mems() > before; }));
        conn.set_game_steps(std::nullopt);
        const AppPacket* sent = nullptr;
        for (auto it = rig.server.zone_log.rbegin(); it != rig.server.zone_log.rend(); ++it)
            if (from_wire(it->opcode) == Op::MemorizeSpell) {
                sent = &*it;
                break;
            }
        REQUIRE(sent != nullptr);
        auto d = decode_memorize_spell(*sent);
        REQUIRE(d);
        CHECK(d->slot == 0);
        CHECK(d->spell_id == 201);
        CHECK(d->scribing == kMemMemorize);
        drain_chat(conn);
        // EQEmu's answer: forget gem 0, then memorize 201 into it
        conn.receive_zone_packet_for_test(encode(MemorizeSpell{0, 200, kMemForget, 0}));
        conn.receive_zone_packet_for_test(encode(MemorizeSpell{0, 201, kMemMemorize, 0}));
        CHECK(conn.spell_state().gems[0].spell == 201);
        CHECK_FALSE(conn.spell_state().memorizing.has_value());
        for (const auto& line : drain_chat(conn)) CHECK(line.find("12496") == std::string::npos); // no You forget
        // A spell picked up from the book and clicked onto the full gem (HandleSpellLeftClick) memorizes there too;
        // with the option off it does not.
        host.cursor_spell = 0; // Minor Healing
        replace = false;
        REQUIRE(luaL_dostring(L, "hs.SpellGemClick('CSPW_Spell0', 'left')") == LUA_OK);
        CHECK_FALSE(conn.spell_state().memorizing.has_value());
        replace = true;
        REQUIRE(luaL_dostring(L, "hs.SpellGemClick('CSPW_Spell0', 'left')") == LUA_OK);
        REQUIRE(conn.spell_state().memorizing.has_value());
        CHECK(conn.spell_state().memorizing->gem == 0);
        CHECK(conn.spell_state().memorizing->spell == 200);
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
