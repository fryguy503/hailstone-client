// The TLO members a combat script needs (mq/tlo_game.cpp): Me.AbilityReady, Me.CombatAbilityReady,
// Me.AltAbilityReady and their timers over mq::Context::ability, and the Ability Book's answer to it
// (app::ability_state) against the mock server; Target.Buff and the rest from OP_TargetBuffs; line of sight.
#include <doctest/doctest.h>

#include <array>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/coords.h"
#include "game/mechanics/character.h"
#include "mock_server/harness.h"
#include "mq/parse.h"
#include "mq/tlo.h"
#include "mq_test_world.h"

using namespace hailstone;
using namespace hailstone::test;

TEST_CASE("TLO: Me.AbilityReady, CombatAbilityReady, AltAbilityReady and their timers") {
    mq_test::World world{"hailstone_mq_abilities_test"};
    mq::Registry reg;
    mq::register_game_tlos(reg);
    mq::Context ctx;
    ctx.registry = &reg;
    ctx.conn = world.conn.get();
    auto p = [&](std::string_view text) { return mq::parse_macro_data(ctx, text); };

    // Without an ability source nothing is ready and there are no timers.
    CHECK(p("${Me.AbilityReady[Kick]}") == "FALSE");
    CHECK(p("${Me.CombatAbilityTimer[Fury]}") == "NULL");

    std::string asked;
    ctx.ability = [&](mq::AbilityKind kind, std::string_view name) -> std::optional<mq::AbilityState> {
        asked = std::string(name);
        if (kind == mq::AbilityKind::Skill && name == "Kick") return mq::AbilityState{"Kick", true, 0, 6000};
        if (kind == mq::AbilityKind::Discipline && name == "Fury") return mq::AbilityState{"Fury Rk. II", false, 12500, 60000};
        if (kind == mq::AbilityKind::AltAbility && name == "35") return mq::AbilityState{"Lay on Hands", true, 0, 4320000};
        return std::nullopt;
    };
    CHECK(p("${Me.AbilityReady[Kick]}") == "TRUE");
    CHECK(p("${Me.AbilityReady[Bash]}") == "FALSE"); // not the player's
    CHECK(p("${Me.CombatAbilityReady[Kick]}") == "FALSE"); // a skill is not a discipline
    CHECK(p("${Me.CombatAbilityReady[Fury]}") == "FALSE");
    CHECK(p("${Me.CombatAbilityTimer[Fury].TotalSeconds}") == "12");
    CHECK(p("${Me.CombatAbilityTimer[Fury].Raw}") == "12500");
    CHECK(p("${Me.AltAbilityReady[35]}") == "TRUE");
    CHECK(p("${Me.AltAbilityTimer[35].Raw}") == "0");
    CHECK(p("${Me.AbilityTimer[Bash]}") == "NULL");
    CHECK(asked == "Bash");
    asked.clear();
    CHECK(p("${Me.AbilityReady}") == "FALSE"); // no name: not asked
    CHECK(asked.empty());
}

TEST_CASE("ability_state: the mock server's warrior has Kick ready, and no discipline or AA of a made-up name") {
    EmptyGameDir dir;
    GameDir game(dir.path());
    MockEqServer server(MockConfig::standard());
    net::GameConnection conn(game, quick_options());
    auto until = [&](const std::function<bool()>& done) { return pump([&] { conn.update(); server.poll(); }, done); };
    conn.start(server.ticket());
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
    conn.enter_world("Gennro");
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));

    bool ran = false;
    net::GameConnectionTestAccess::with_character(conn, [&](game::mechanics::Character& ch) {
        ran = true;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.character = &ch;
        // The mock's profile gives skill n the value n, so Kick (30) is the player's; nothing used it yet.
        auto kick = app::ability_state(host, mq::AbilityKind::Skill, "30");
        REQUIRE(kick);
        CHECK(kick->ready);
        CHECK(kick->remaining_ms == 0);
        CHECK(kick->name == conn.skill_name(30));
        auto by_name = app::ability_state(host, mq::AbilityKind::Skill, kick->name);
        REQUIRE(by_name);
        CHECK(by_name->name == kick->name);
        CHECK_FALSE(app::ability_state(host, mq::AbilityKind::Skill, "No Such Skill"));
        CHECK_FALSE(app::ability_state(host, mq::AbilityKind::Discipline, "No Such Discipline"));
        CHECK_FALSE(app::ability_state(host, mq::AbilityKind::AltAbility, "No Such Ability"));
        CHECK_FALSE(app::ability_state(host, mq::AbilityKind::Skill, ""));
        // use_ability presses its hot button (the skill's), and nothing for an ability the player lacks.
        std::vector<game::HotButton> pressed;
        host.run_button = [&](const game::HotButton& b) { pressed.push_back(b); };
        CHECK(app::use_ability(host, mq::AbilityKind::Skill, kick->name));
        REQUIRE(pressed.size() == 1);
        CHECK(pressed[0].type == game::HotButtonType::Skill);
        CHECK(pressed[0].slot == 30);
        CHECK_FALSE(app::use_ability(host, mq::AbilityKind::Discipline, "No Such Discipline"));
        host.in_game = false;
        CHECK_FALSE(app::use_ability(host, mq::AbilityKind::Skill, "30"));
        CHECK(pressed.size() == 1);
    });
    CHECK(ran);
}

TEST_CASE("hs.AbilityBook lists the activated AAs and never a passive one") {
    EmptyGameDir dir;
    GameDir game(dir.path());
    MockEqServer server(MockConfig::standard());
    net::GameConnection conn(game, quick_options());
    auto until = [&](const std::function<bool()>& done) { return pump([&] { conn.update(); server.poll(); }, done); };
    conn.start(server.ticket());
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
    conn.enter_world("Gennro");
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));
    // Two owned AAs: group 701 casts a spell (activated), group 702 has none (passive).
    auto& aa = net::GameConnectionTestAccess::alt_adv(conn);
    game::AltAbility active, passive;
    active.id = 9101;
    active.group = 701;
    active.spell = 11;
    active.refresh = 600; // on reuse from now: the book reads it through aa_readiness_bulk
    active.timers = {7};
    passive.id = 9102;
    passive.group = 702;
    aa.add(active);
    aa.add(passive);
    aa.owned.push_back({9101, 1, 0});
    aa.owned.push_back({9102, 1, 0});
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    aa.set_timer(7, static_cast<std::uint32_t>(now), static_cast<std::uint32_t>(now), now);

    bool ran = false;
    net::GameConnectionTestAccess::with_character(conn, [&](game::mechanics::Character& ch) {
        ran = true;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.character = &ch;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        // `true` once asked for the passive AAs too; it is ignored now.
        REQUIRE(luaL_dostring(L, "local keys = {} "
                                 "for _, e in ipairs(hs.AbilityBook(true)) do "
                                 "  if e.kind == 'aa' then keys[#keys + 1] = e.key end "
                                 "end "
                                 "return table.concat(keys, ',')") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) == "aa:701");
        // Its readiness, as ability_state gives it for the one AA.
        REQUIRE(luaL_dostring(L, "for _, e in ipairs(hs.AbilityBook()) do "
                                 "  if e.key == 'aa:701' then return e.reuse, e.remaining, e.ready end "
                                 "end") == LUA_OK);
        const auto one = app::ability_state(host, mq::AbilityKind::AltAbility, "701");
        REQUIRE(one);
        CHECK(lua_tointeger(L, -3) * 1000 == one->reuse_ms);
        CHECK(lua_tointeger(L, -2) * 1000 == one->remaining_ms);
        CHECK(lua_toboolean(L, -1) == 0);
        CHECK_FALSE(one->ready);
        CHECK(one->reuse_ms == 600000);
        lua_close(L);
    });
    CHECK(ran);
}

TEST_CASE("ability_state finds an owned, activated AA by name or group among many, never a passive one") {
    EmptyGameDir dir;
    GameDir game(dir.path());
    MockEqServer server(MockConfig::standard());
    net::GameConnection conn(game, quick_options());
    auto until = [&](const std::function<bool()>& done) { return pump([&] { conn.update(); server.poll(); }, done); };
    conn.start(server.ticket());
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
    conn.enter_world("Gennro");
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));
    // 300 owned AAs (groups 1000..1299), every third passive. With no title strings and no spell of that id
    // each is named "Ability <group>".
    auto& aa = net::GameConnectionTestAccess::alt_adv(conn);
    for (std::int32_t n = 0; n < 300; ++n) {
        game::AltAbility a;
        a.id = static_cast<std::uint32_t>(20000 + n);
        a.group = 1000 + n;
        a.spell = n % 3 == 0 ? 0 : 900000 + n;
        aa.add(a);
        aa.owned.push_back({a.id, 1, 0});
    }

    bool ran = false;
    net::GameConnectionTestAccess::with_character(conn, [&](game::mechanics::Character& ch) {
        ran = true;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.character = &ch;
        auto last = app::ability_state(host, mq::AbilityKind::AltAbility, "ability 1298"); // case ignored
        REQUIRE(last);
        CHECK(last->name == "Ability 1298");
        CHECK(last->ready);
        auto by_group = app::ability_state(host, mq::AbilityKind::AltAbility, "1001");
        REQUIRE(by_group);
        CHECK(by_group->name == "Ability 1001");
        CHECK_FALSE(app::ability_state(host, mq::AbilityKind::AltAbility, "Ability 1297")); // passive
        CHECK_FALSE(app::ability_state(host, mq::AbilityKind::AltAbility, "1000"));         // passive
        CHECK_FALSE(app::ability_state(host, mq::AbilityKind::AltAbility, "999"));
    });
    CHECK(ran);
}

TEST_CASE("TLO: Target.Buff and the rest from OP_TargetBuffs, the states by spell effect, Me's own buffs") {
    mq_test::World world{"hailstone_mq_target_buffs_test"};
    mq::Registry reg;
    mq::register_game_tlos(reg);
    mq::Context ctx;
    ctx.registry = &reg;
    ctx.conn = world.conn.get();
    auto p = [&](std::string_view text) { return mq::parse_macro_data(ctx, text); };

    // The target (a_rat01, id 10) before the server sent its buffs.
    CHECK(p("${Target.BuffsPopulated}") == "FALSE");
    CHECK(p("${Target.BuffCount}") == "0");
    CHECK(p("${Target.Mezzed}") == "NULL");

    auto& sp = net::GameConnectionTestAccess::spells(*world.conn);
    auto buff = [](std::uint32_t spell, std::uint32_t caster, std::int32_t tics) {
        game::BuffSlot b;
        b.spell = spell;
        b.caster_id = caster;
        b.tics = tics;
        b.received_ms = net::GameConnection::now_ms();
        return b;
    };
    sp.target_buffs_entity = 10;
    sp.target_buffs = {{0, buff(3001, 1, 3)}, {4, buff(3002, 99, 10)}};
    CHECK(p("${Target.BuffsPopulated}") == "TRUE");
    CHECK(p("${Target.BuffCount}") == "2");
    CHECK(p("${Target.Buff[1]}") == "Test Mesmerize");
    CHECK(p("${Target.Buff[2].ID}") == "5"); // the slot the server gave
    CHECK(p("${Target.Buff[Slow]}") == "Test Slow");
    CHECK(p("${Target.Buff[=Slow]}") == "NULL");
    CHECK(p("${Target.Buff[3]}") == "NULL");
    auto secs = std::stoi(p("${Target.BuffDuration[Test Mesmerize].TotalSeconds}"));
    CHECK(secs >= 17);
    CHECK(secs <= 18);
    CHECK(p("${Target.MyBuff[Test Mesmerize]}") == "Test Mesmerize"); // the player (id 1) cast it
    CHECK(p("${Target.MyBuff[Test Slow]}") == "NULL");
    CHECK(p("${Target.MyBuffDuration[Test Slow]}") == "NULL");
    CHECK(p("${Target.Mezzed}") == "Test Mesmerize");
    CHECK(p("${Target.Slowed.ID}") == "5");
    CHECK(p("${Target.Rooted}") == "NULL");
    CHECK(p("${Target.Snared}") == "NULL");

    // Another spawn's buffs are not known; a buff value from before reads as nothing once they are another's.
    CHECK(p("${Spawn[id 11].BuffsPopulated}") == "FALSE");
    CHECK(p("${Spawn[id 11].Buff[1]}") == "NULL");
    mq::Value held = reg.member(ctx, reg.tlo(ctx, "Target"), "Buff", "1");
    sp.target_buffs_entity = 11;
    CHECK(reg.text(ctx, reg.member(ctx, held, "Spell")) == "NULL");

    // The player's own: Me.Buff stays the player's slots; the spawn members read them too.
    CHECK(p("${Me.BuffsPopulated}") == "TRUE");
    CHECK(p("${Me.BuffCount}") == "2"); // Holy Armor and the song
    CHECK(p("${Me.Mezzed}") == "NULL");
    CHECK(p("${Me.MyBuff[Holy Armor]}") == "NULL"); // no caster recorded
}

TEST_CASE("TLO: Spawn.LineOfSight and LineOfSight[y,x,z:y,x,z]") {
    mq_test::World world{"hailstone_mq_los_test"};
    mq::Registry reg;
    mq::register_game_tlos(reg);
    mq::Context ctx;
    ctx.registry = &reg;
    ctx.conn = world.conn.get();
    auto p = [&](std::string_view text) { return mq::parse_macro_data(ctx, text); };

    // No zone collision: not known.
    CHECK(p("${Target.LineOfSight}") == "NULL");
    CHECK(p("${LineOfSight[0,0,0:10,10,0]}") == "NULL");
    CHECK(p("${Me.LineOfSight}") == "TRUE");

    std::uint32_t asked = 0;
    ctx.can_see = [&](std::uint32_t id) -> std::optional<bool> {
        asked = id;
        return id == 10;
    };
    std::array<float, 3> from{}, to{};
    ctx.line_of_sight = [&](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::optional<bool> {
        from = a;
        to = b;
        return a[2] < 50.0f;
    };
    CHECK(p("${Target.LineOfSight}") == "TRUE");
    CHECK(asked == 10);
    CHECK(p("${Spawn[id 11].LineOfSight}") == "FALSE");
    CHECK(p("${LineOfSight[1,2,3:4,5,6]}") == "TRUE");
    // MQ's y,x,z in the server's coordinates, handed on as world coordinates.
    const glm::vec3 a = game::server_to_world({2, 1, 3}), b = game::server_to_world({5, 4, 6});
    CHECK(from == std::array<float, 3>{a.x, a.y, a.z});
    CHECK(to == std::array<float, 3>{b.x, b.y, b.z});
    CHECK(p("${LineOfSight[1, 2, 100 : 4,5,6]}") == "FALSE");
    CHECK(p("${LineOfSight[1,2:4,5,6]}") == "NULL");
    CHECK(p("${LineOfSight[1,2,3]}") == "NULL");
    CHECK(p("${LineOfSight[a,b,c:4,5,6]}") == "NULL");
    // Past the world, inf or nan: no sight line to ask about (the collision grid overflowed int32 on these).
    from = {};
    CHECK(p("${LineOfSight[-1e12,0,0:0,0,0]}") == "NULL");
    CHECK(p("${LineOfSight[0,0,0:1000000,0,0]}") == "NULL");
    CHECK(p("${LineOfSight[inf,0,0:0,0,0]}") == "NULL");
    CHECK(p("${LineOfSight[0,nan,0:0,0,0]}") == "NULL");
    CHECK(from == std::array<float, 3>{});
    CHECK(p("${LineOfSight[-99999,2,3:4,5,6]}") == "TRUE");
}
