// mq/tlo.h, mq/spawn_search.h and mq/parse.h: MacroQuest's TLOs over a small world (tests/mq_test_world.h).
#include <doctest/doctest.h>

#include <algorithm>

#include "core/build_info.h"
#include "manual_clock.h"
#include "game/doors.h"
#include "mq/commands.h"
#include "mq/parse.h"
#include "plugin/hooks.h"
#include "mq/spawn_search.h"
#include "mq/tlo.h"
#include "mq_test_world.h"
#include "game/server_ext.h"

using namespace hailstone;
using mq::Value;

namespace {

struct Fixture {
    mq_test::World world{"hailstone_mq_tlo_test"};
    mq::Registry reg;
    mq::Context ctx;
    Fixture() {
        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        ctx.server_name = "TestServer";
        ctx.window_open = [](std::string_view w) { return w == "InventoryWindow"; };
    }
    std::string p(std::string_view text) { return mq::parse_macro_data(ctx, text); }
};

} // namespace

TEST_CASE("TLO policy denies resolved names before running providers and leaves their catalog visible") {
    Fixture f;
    int calls = 0;
    const auto before = f.reg.tlo_catalog_serial();
    auto provider = [&](const mq::Context&, std::string_view index) {
        ++calls;
        return Value::string(std::string(index));
    };
    f.reg.add_tlo("AddonNotes", provider, {}, mq::TloOrigin::Addon);
    CHECK(f.reg.tlo_catalog_serial() != before);
    CHECK(f.p("${aDdOnNoTeS[before]}") == "before");
    CHECK(calls == 1);

    auto rules = plugin::parse_server_cfg("[tlos]\ndefault = allow\nblock = AddonNotes, SpawnCount, Target\n");
    rules.provisional = true;
    net::GameConnectionTestAccess::set_server_rules(*f.world.conn, rules);
    CHECK(f.p("${AddonNotes[cached]}") == "cached");
    CHECK(calls == 2);
    rules.provisional = false;
    net::GameConnectionTestAccess::set_server_rules(*f.world.conn, rules);
    CHECK(f.reg.tlo(f.ctx, "addonnotes", "direct").is_nil());
    CHECK(f.p("${aDdOnNoTeS[blocked].Length} ${sPaWnCoUnT[npc]} ${Math.Calc[2+3]}") == "NULL NULL 5.00");
    CHECK(f.p("${${String[AddonNotes]}[nested]}") == "NULL");
    CHECK(calls == 2);
    CHECK(f.reg.complete(f.ctx, "Addon").empty());
    CHECK(f.reg.complete(f.ctx, "Target.Na").empty());
    const auto me_members = f.reg.complete(f.ctx, "Me.Na");
    CHECK(std::find(me_members.begin(), me_members.end(), "Name") != me_members.end());
    CHECK(f.reg.has_tlo("AddonNotes"));

    const auto catalog = f.reg.tlo_catalog();
    const auto addon = std::find_if(catalog.begin(), catalog.end(), [](const auto& row) { return row.name == "AddonNotes"; });
    REQUIRE(addon != catalog.end());
    CHECK(addon->origin == mq::TloOrigin::Addon);
    CHECK(std::count_if(catalog.begin(), catalog.end(), [](const auto& row) { return row.origin == mq::TloOrigin::Addon; }) == 1);
    const auto me = std::find_if(catalog.begin(), catalog.end(), [](const auto& row) { return row.name == "Me"; });
    REQUIRE(me != catalog.end());
    CHECK(me->origin == mq::TloOrigin::BuiltIn);

    const auto registered = f.reg.tlo_catalog_serial();
    f.reg.add_tlo("addonnotes", provider, {}, mq::TloOrigin::Addon);
    CHECK(f.reg.tlo_catalog_serial() == registered);
    // A built-in without a description is still built in: the origin is its own field.
    f.reg.add_tlo("addonnotes", provider);
    CHECK(f.reg.tlo_catalog_serial() != registered);
    const auto relabelled = f.reg.tlo_catalog();
    CHECK(std::find_if(relabelled.begin(), relabelled.end(), [](const auto& row) {
              return row.name == "AddonNotes";
          })->origin == mq::TloOrigin::BuiltIn);
    f.reg.remove_tlo("ADDONNOTES");
    CHECK(f.reg.tlo_catalog_serial() != registered);
    CHECK_FALSE(f.reg.has_tlo("AddonNotes"));
    net::GameConnectionTestAccess::set_server_rules(*f.world.conn, std::nullopt);
    CHECK(f.p("${Me.Name}") == "Tester");
}

TEST_CASE("TLO policy tells the player once per caller and root when it blocks a lookup") {
    Fixture f;
    std::vector<std::string> told;
    f.reg.set_denial_notice([&](const std::string& text) { told.push_back(text); });
    net::GameConnectionTestAccess::set_server_rules(*f.world.conn, plugin::parse_server_cfg("[tlos]\nblock = Target\n"));
    CHECK(f.p("${target.Name} ${Target.Level} ${Me.Name}") == "NULL NULL Tester");
    CHECK(f.reg.complete(f.ctx, "Targ").empty()); // completion is not a lookup: it tells nothing
    REQUIRE(told.size() == 1);
    CHECK(told[0] == "[MQ] the server blocks TLO Target");
    {
        plugin::CallerScope script(plugin::kCapAll, "demo");
        CHECK(f.p("${Target.Name}") == "NULL");
        CHECK(f.p("${TARGET.Name}") == "NULL");
    }
    REQUIRE(told.size() == 2);
    CHECK(told[1] == "[MQ] demo: the server blocks TLO Target");
    // New rules (a re-sync on zoning) tell again.
    net::GameConnectionTestAccess::set_server_rules(*f.world.conn, plugin::parse_server_cfg("[tlos]\nblock = Target\n"));
    CHECK(f.p("${Target.Name}") == "NULL");
    REQUIRE(told.size() == 3);
    CHECK(told[2] == told[0]);
}

// A server extension's zone_display_name changes the label the HUD and map show, not the zone's name: Zone and
// Zone.Name stay the zone's long name, as scripts store and compare it.
TEST_CASE("TLO: Zone.Name is the zone's own name whatever a server extension displays") {
    struct Renamer final : game::ServerExtension {
        std::optional<std::string> zone_display_name(const net::GameConnection&, std::uint32_t, std::uint32_t) const override {
            return "Shown Elsewhere";
        }
    };
    net::ConnectionOptions options;
    options.plugins = std::make_shared<Renamer>();
    mq_test::World world("hailstone_mq_tlo_zone_name", options);
    mq::Registry reg;
    mq::register_game_tlos(reg);
    mq::Context ctx;
    ctx.registry = &reg;
    ctx.conn = world.conn.get();
    REQUIRE(world.conn->zone_display_name() == "Shown Elsewhere");
    CHECK(mq::parse_macro_data(ctx, "${Zone.Name} / ${Zone}") == "North Qeynos / North Qeynos");
}

TEST_CASE("TLO: Me's HP is Cur_HP / Max_HP, the items' HP EQEmu's HP update leaves out included") {
    Fixture f;
    // 750 / 1000 from the server; the player's items add 250 (app/stats_ui.cpp share_own_hp_items).
    f.world.conn->set_own_hp_items(250, 250);
    CHECK(f.p("${Me.CurrentHPs}/${Me.MaxHPs}") == "1000/1250");
    CHECK(f.p("${Me.PctHPs}") == "80");
    // The player's own spawn (Spawn[...].PctHPs, a self target's Target.PctHPs, a plugin's spawn) shows the same.
    net::GameConnection& c = *f.world.conn;
    REQUIRE(c.world().player());
    CHECK(c.world().player()->hp_percent == 80);
    // A server extension that gives Max_HP whole: only Cur_HP gets the items' HP.
    c.set_own_hp_items(250, 0);
    CHECK(f.p("${Me.CurrentHPs}/${Me.MaxHPs}") == "1000/1000");
    CHECK(c.world().player()->hp_percent == 100);

    // A geared player below its items' HP: EQEmu's own HP update is below 1 while the player still stands.
    auto& ps = net::GameConnectionTestAccess::status(c);
    ps.hp = -100;
    c.set_own_hp_items(260, 260);
    CHECK(f.p("${Me.CurrentHPs}/${Me.MaxHPs}") == "160/1260");
    CHECK(c.world().player()->hp_percent == 12);
    CHECK_FALSE(c.player_down());
    c.set_own_hp_items(0, 0);
    CHECK(c.player_down()); // without the items' HP: down, as the packet's value says

    // Dead: nothing added to the current HP.
    c.set_own_hp_items(250, 250);
    ps.dead = true;
    ps.hp = 0;
    CHECK(f.p("${Me.CurrentHPs}") == "0");
    CHECK(f.p("${Me.PctHPs}") == "0");
    CHECK(c.player_down());
}

TEST_CASE("TLO: Me's own state") {
    Fixture f;
    CHECK(f.p("${Me}") == "Tester");
    CHECK(f.p("${Me.Name}") == "Tester");
    CHECK(f.p("${Me.Level}") == "50");
    CHECK(f.p("${Me.PctHPs}") == "75");
    CHECK(f.p("${Me.CurrentHPs}/${Me.MaxHPs}") == "750/1000");
    CHECK(f.p("${Me.PctMana}") == "25");
    CHECK(f.p("${Me.PctEndurance}") == "50");
    CHECK(f.p("${Me.Class}") == "Cleric");
    CHECK(f.p("${Me.Class.ShortName}") == "CLR");
    CHECK(f.p("${Me.Combat}") == "TRUE");
    CHECK(f.p("${Me.Sitting}") == "FALSE");
    CHECK(f.p("${Me.Platinum}p ${Me.Cash}") == "12p 12345");
    CHECK(f.p("${Me.AAPoints}") == "7");
    CHECK(f.p("${Me.ID}") == "1"); // from the spawn (character inherits spawn)
    CHECK(f.p("${Me.Grouped}") == "TRUE");
}

TEST_CASE("TLO: spells, gems and buffs") {
    Fixture f;
    CHECK(f.p("${Me.Gem[1]}") == "Minor Healing");
    CHECK(f.p("${Me.Gem[2].ID}") == "11");
    CHECK(f.p("${Me.Gem[Holy Armor]}") == "2");
    CHECK(f.p("${Me.Gem[3]}") == "NULL");
    CHECK(f.p("${Me.Buff[1]}") == "Holy Armor");
    CHECK(f.p("${Me.Buff[Holy Armor].ID}") == "1");
    CHECK(f.p("${Me.Buff[Holy].Spell.ID}") == "11"); // a partial name
    CHECK(f.p("${Me.Buff[1].Range}") == "0.00");     // a spell member through the buff
    CHECK(f.p("${Me.Buff[2]}") == "NULL");           // the song is not a buff
    CHECK(f.p("${Me.Song[1]}") == "Selo's Accelerando");
    CHECK(f.p("${Me.CountBuffs} ${Me.CountSongs}") == "1 1");
    // The buff and song windows' slots (RoF2's 42 and 20), and the buff slots one long buff leaves.
    CHECK(f.p("${Me.MaxBuffSlots} ${Me.MaxSongSlots} ${Me.FreeBuffSlots}") == "42 20 41");
    // 10 ticks = 60 s, counted from when it arrived.
    auto secs = std::stoi(f.p("${Me.Buff[1].Duration.TotalSeconds}"));
    CHECK(secs >= 59);
    CHECK(secs <= 60);
    CHECK(f.p("${Spell[200].Name}") == "Minor Healing");
    CHECK(f.p("${Spell[Minor Healing].ID}") == "200");
    CHECK(f.p("${Spell[200].Range}") == "100.00");
    CHECK(f.p("${Spell[200].CastTime}") == "1.50");
    CHECK(f.p("${Spell[200].TargetType}") == "Single");
    CHECK(f.p("${Spell[200].SpellType}") == "Beneficial");
    CHECK(f.p("${Spell[11].Level}") == "3"); // the cleric's level column
    CHECK(f.p("${Spell[200].Level}") == "1");
    CHECK(f.p("${Spell[99999]}") == "NULL");
}

TEST_CASE("TLO: Target, Spawn, SpawnCount, NearestSpawn and the search words") {
    Fixture f;
    CHECK(f.p("${Target}") == "a rat");
    CHECK(f.p("${Target.Name}") == "a_rat01");
    CHECK(f.p("${Target.ID}") == "10");
    CHECK(f.p("${Target.Type}") == "NPC");
    CHECK(f.p("${Target.PctHPs}") == "80");
    CHECK(f.p("${Target.Distance}") == "50.00");
    CHECK(f.p("${Target.X},${Target.Y}") == "30.00,40.00");
    CHECK(f.p("${Target.Loc}") == "40.00, 30.00, 0.00");
    CHECK(f.p("${Target.Level}") == "2");
    CHECK(f.p("${Target.Named}") == "FALSE");
    CHECK(f.p("${Spawn[11].Named}") == "TRUE");
    CHECK(f.p("${Spawn[11].CleanName}") == "Fippy Darkpaw");
    CHECK(f.p("${Spawn[npc fippy].ID}") == "11");
    CHECK(f.p("${Spawn[pc =Groupie].Distance}") == "20.00");
    CHECK(f.p("${Spawn[pc =Group]}") == "NULL"); // exact
    CHECK(f.p("${SpawnCount[npc]}") == "2");
    CHECK(f.p("${SpawnCount[npc radius 60]}") == "1");
    CHECK(f.p("${SpawnCount[corpse]}") == "1");
    CHECK(f.p("${SpawnCount[pc]}") == "2");
    CHECK(f.p("${SpawnCount[npc range 3 10]}") == "1");
    CHECK(f.p("${SpawnCount[named]}") == "1");
    CHECK(f.p("${NearestSpawn[npc]}") == "a rat");
    CHECK(f.p("${NearestSpawn[2,npc]}") == "Fippy Darkpaw");
    CHECK(f.p("${NearestSpawn[3,npc]}") == "NULL");
    CHECK(f.p("${SpawnCount[npc loc 0 100 radius 5]}") == "1");
    CHECK(f.p("${Target.HeadingTo.Degrees}").substr(0, 3) != "NUL");
}

TEST_CASE("spawn search: targetable is the game's IsTargetable, body type 11 left out") {
    Fixture f;
    auto& w = net::GameConnectionTestAccess::world(*f.world.conn);
    CHECK(f.p("${SpawnCount[npc targetable]}") == "2");
    // A server's portal trigger: the Targetable flag on, body type 11 (EQEmu's NoTarget).
    w.spawns[11].body_type = 11;
    CHECK(f.p("${Spawn[11].Targetable}") == "TRUE"); // the flag alone, as MQ's Spawn.Targetable
    CHECK(f.p("${SpawnCount[npc targetable]}") == "1");
    // MQ's GetSpawnType makes body type 11 an "untargetable" spawn, which an "npc" search leaves out.
    CHECK(f.p("${SpawnCount[npc]}") == "1");
    CHECK(f.p("${SpawnCount[untargetable]}") == "1");
    w.spawns[11].body_type = 1;
    w.spawns[11].targetable = false;
    CHECK(f.p("${SpawnCount[npc targetable]}") == "1");
}

TEST_CASE("spawn search: parsed words") {
    auto s = mq::parse_spawn_search("npc radius 50 zradius 10 range 5 9 named \"a rat\" notid 3 id 7");
    CHECK(s.kind == mq::SpawnSearch::Kind::Npc);
    CHECK(s.radius == 50);
    CHECK(s.zradius == 10);
    CHECK(s.min_level == 5);
    CHECK(s.max_level == 9);
    CHECK(s.named);
    CHECK(s.name == "a rat");
    CHECK(s.not_id == 3u);
    CHECK(s.id == 7u);
    auto e = mq::parse_spawn_search("pc =Soandso class wizard");
    CHECK(e.exact);
    CHECK(e.name == "Soandso");
    CHECK(e.class_name == "wizard");
    // MQ's NotID is one id: the last notid's.
    CHECK(mq::parse_spawn_search("notid 3 notid 4").not_id == 4u);
    // A bare number is a level; a bare class name (long or short) is the class; the whole text a long class name
    // with a space is that class, not a name and the "knight" keyword.
    auto b = mq::parse_spawn_search("npc 45 wiz");
    CHECK(b.min_level == 45);
    CHECK(b.max_level == 45);
    CHECK(b.class_name == "Wizard");
    CHECK(b.name.empty());
    auto sk = mq::parse_spawn_search("shadow knight");
    CHECK(sk.class_name == "Shadow Knight");
    CHECK_FALSE(sk.knight);
    // Plain words make the name, the first "=" asks for the whole name; "name" sets it outright.
    CHECK(mq::parse_spawn_search("Fippy Darkpaw").name == "Fippy Darkpaw");
    auto n = mq::parse_spawn_search("Fippy name =Lord_Bob");
    CHECK(n.name == "Lord_Bob");
    CHECK(n.exact);
    // loc without z (or z 0) takes the player's z and only two words; nopcnear without a number is 200.
    auto l = mq::parse_spawn_search("loc 10 20 nopcnear");
    REQUIRE(l.loc);
    CHECK(l.loc->x == 10.0f);
    CHECK(l.loc->y == 20.0f);
    CHECK_FALSE(l.loc_z);
    CHECK(l.nopcnear == 200.0f);
    auto l3 = mq::parse_spawn_search("loc 10 20 30 nopcnear 50");
    CHECK(l3.loc_z == 30.0f);
    CHECK(l3.nopcnear == 50.0f);
    // Flags, light names, player states.
    auto fl = mq::parse_spawn_search("xtarhater raid nogroup noguild merchant banker tribute tank light TR "
                                     "playerstate 4 playerstate 8 body Humanoid race Human");
    CHECK(fl.xtarhater);
    CHECK(fl.raid);
    CHECK(fl.nogroup);
    CHECK(fl.noguild);
    CHECK(fl.merchant);
    CHECK(fl.banker);
    CHECK(fl.tribute);
    CHECK(fl.tank);
    CHECK(fl.light);
    CHECK(fl.light_name == "TR");
    CHECK(fl.player_state == 12u);
    CHECK(fl.body_name == "Humanoid");
    CHECK(fl.race_name == "Human");
    CHECK(fl.name.empty());
    // "light" with no light name after it: any light, and the next word stays a word.
    auto la = mq::parse_spawn_search("light rat");
    CHECK(la.light);
    CHECK(la.light_name.empty());
    CHECK(la.name == "rat");
    for (auto [word, kind] : {std::pair{"mount", mq::SpawnSearch::Kind::Mount}, {"pcpet", mq::SpawnSearch::Kind::PcPet},
                              {"npcpet", mq::SpawnSearch::Kind::NpcPet}, {"trigger", mq::SpawnSearch::Kind::Trigger},
                              {"chest", mq::SpawnSearch::Kind::Chest}, {"aura", mq::SpawnSearch::Kind::Aura},
                              {"banner", mq::SpawnSearch::Kind::Banner}, {"flyer", mq::SpawnSearch::Kind::Flyer}}) {
        INFO(word);
        CHECK(mq::parse_spawn_search(word).kind == kind);
    }
    // MQ's /target cycling: next and prev keep the direction (the last word wins) and are no part of the name.
    CHECK(mq::parse_spawn_search("npc next").direction == mq::SpawnSearch::Direction::Next);
    auto pv = mq::parse_spawn_search("next npc prev");
    CHECK(pv.direction == mq::SpawnSearch::Direction::Prev);
    CHECK(pv.kind == mq::SpawnSearch::Kind::Npc);
    CHECK(pv.name.empty());
}

TEST_CASE("spawn search: /target takes the nearest, next or prev match") {
    Fixture f;
    auto& w = net::GameConnectionTestAccess::world(*f.world.conn);
    auto id = [&](const char* text) {
        const mq::SpawnSearch q = mq::parse_spawn_search(text);
        const game::Spawn* s = mq::pick_target(q, mq::search_spawns(q, w, nullptr), w);
        return s ? s->id : 0u;
    };
    // Distances: the player at 0, Groupie at 20, the rat at 50, Fippy at 100, the corpse at 200.
    w.target_id = 0;
    CHECK(id("npc") == 10u);
    // next / prev walk the nearest-first matches from the current target; no wrap at either end.
    w.target_id = 10;
    CHECK(id("npc next") == 11u);
    CHECK(id("npc prev") == 0u);
    w.target_id = 11;
    CHECK(id("npc prev") == 10u);
    CHECK(id("npc next") == 0u);
    // A target that is no match starts from its own distance: next the first match farther away, prev the last nearer.
    w.target_id = 12;
    CHECK(id("npc next") == 10u);
    CHECK(id("npc prev") == 0u);
    w.target_id = 13;
    CHECK(id("npc prev") == 11u);
    CHECK(id("npc next") == 0u);
    // With no target, next and prev take the nearest match, as /target does.
    w.target_id = 0;
    CHECK(id("npc next") == 10u);
    CHECK(id("npc prev") == 10u);
    // Your own spawn matches "pc"; the nearest leaves it out while Groupie matches too.
    CHECK(id("pc") == 12u);
    // The same rule holds for next and prev: you are stepped over while Groupie matches, and the current target still
    // says where the walk starts.
    w.target_id = 12;
    CHECK(id("pc next") == 0u); // Groupie is the last match
    CHECK(id("pc prev") == 0u); // the only earlier match is you, and you are left out
    w.target_id = 1;
    CHECK(id("pc next") == 12u); // past you, to Groupie
    CHECK(id("pc prev") == 0u); // nothing before you
    w.target_id = 0;
    CHECK(id("pc next") == 12u);
    CHECK(id("pc prev") == 12u);
    // You alone still match, and are taken when there is no target.
    CHECK(id("id 1") == 1u);
    CHECK(id("npc nobody_here") == 0u);
}

TEST_CASE("spawn search: MQ's matching rules") {
    Fixture f;
    auto& w = net::GameConnectionTestAccess::world(*f.world.conn);
    auto count = [&](const char* q) { return f.p(std::string("${SpawnCount[") + q + "]}"); };

    // radius is 3D: Fippy is 100 away on the ground and 2 up.
    CHECK(count("npc radius 100") == "1");
    CHECK(count("npc radius 100.03") == "2");
    // radius 0 is a radius (only the player stands at 0); one of 10000 or more is none.
    CHECK(count("radius 0") == "1");
    CHECK(count("radius 10000") == "5");
    // zradius is measured from the player's z: Fippy stands 2 up.
    CHECK(count("npc zradius 1") == "1");
    CHECK(count("npc loc 0 100 5 zradius 4") == "1"); // loc's z: Fippy 3 below it

    // A bare number is a level, a bare class word the class.
    CHECK(count("8") == "1");
    CHECK(count("cleric") == "2"); // Tester and Groupie
    CHECK(count("pc CLR") == "2");
    CHECK(count("class Warrior npc") == "2");

    // Pets: an NPC's pet is "npc" and "npcpet", a player's "pcpet" only; nopet leaves both out.
    f.world.add(20, "Tester`s_pet", true, 10, game::server_to_world({5, 5, 0}));
    f.world.add(21, "Fippys_pet", true, 4, game::server_to_world({6, 6, 0}));
    w.spawns[20].pet_owner_id = 1;
    w.spawns[21].pet_owner_id = 11;
    CHECK(count("pcpet") == "1");
    CHECK(f.p("${Spawn[pcpet].ID}") == "20");
    CHECK(count("npcpet") == "1");
    CHECK(f.p("${Spawn[npcpet].ID}") == "21");
    CHECK(count("pet") == "2");
    CHECK(count("npc") == "3"); // the rat, Fippy and Fippy's pet
    CHECK(count("npc nopet") == "2");
    w.spawns.erase(20);
    w.spawns.erase(21);

    // Corpses: an NPC's has no deity, a player's has one.
    CHECK(count("npccorpse") == "1");
    CHECK(count("pccorpse") == "0");
    w.spawns[13].deity = 201;
    CHECK(count("pccorpse") == "1");
    CHECK(count("npccorpse") == "0");
    w.spawns[13].deity = 0;

    // Service classes and roles.
    w.spawns[11].class_id = 41;
    CHECK(count("merchant") == "1");
    CHECK(f.p("${Spawn[11].Named}") == "FALSE"); // a merchant is not named
    w.spawns[11].class_id = 25; // a GM trainer
    CHECK(count("npc gm") == "1");
    w.spawns[11].class_id = 1;
    CHECK(count("npc gm") == "0");
    CHECK(count("healer") == "2"); // the two clerics
    CHECK(count("tank") == "3");   // the warriors: the rat, Fippy and the rat's corpse
    w.spawns[11].class_id = 2;
    CHECK(count("npc tank") == "2"); // "npc" asks nothing of the roles
    w.spawns[11].class_id = 1;

    // Group: only with a group, the player too; nogroup the rest.
    CHECK(count("group") == "2");
    CHECK(count("pc nogroup") == "0");
    net::GameConnectionTestAccess::group(*f.world.conn).active = false;
    CHECK(count("group") == "0");
    CHECK(count("pc nogroup") == "2");
    net::GameConnectionTestAccess::group(*f.world.conn).active = true;
    // Raid: the player always; a member by name and class.
    CHECK(count("raid") == "1");
    // A server has not supplied fellowship membership yet.
    CHECK(count("fellowship") == "0");

    // nopcnear: Groupie (in the group) does not count; without a group the player does.
    CHECK(count("npc nopcnear 60") == "2");
    net::GameConnectionTestAccess::group(*f.world.conn).active = false;
    CHECK(count("npc nopcnear 60") == "1"); // the rat is 50 from the player
    net::GameConnectionTestAccess::group(*f.world.conn).active = true;

    // Light, player state, guild.
    w.spawns[12].light_type = 2;
    CHECK(count("light") == "1");
    CHECK(count("light TR") == "1");
    CHECK(count("light CDL") == "0");
    w.spawns[12].player_state = 4;
    CHECK(count("playerstate 4") == "1");
    CHECK(count("playerstate 3") == "0");
    w.spawns[12].guild_id = 7;
    CHECK(count("pc noguild") == "1");
    CHECK(count("guildname Nobody") == "0"); // an unknown guild: no filter, "Nobody" is the name

    // Names: a part of the name as sent or cleaned; "=" the whole cleaned name, '#' kept.
    CHECK(count("Fippy_Dark") == "1");
    CHECK(count("=Fippy Darkpaw") == "1");
    CHECK(count("=Fippy") == "0");
    w.spawns[11].name = "#Fippy_Darkpaw000";
    // The exact name keeps the '#' while the part search drops it, so no "=" text finds a '#' name (MQ's
    // ExactSearchCleanNames off); a part of it still does.
    CHECK(count("=Fippy Darkpaw") == "0");
    CHECK(count("=#Fippy Darkpaw") == "0");
    CHECK(count("Fippy Darkpaw") == "1");

    // los asks the player's sight check; not known keeps the spawn.
    std::optional<bool> seen;
    f.ctx.can_see = [&](std::uint32_t id) -> std::optional<bool> { return id == 10 ? seen : std::optional<bool>(true); };
    CHECK(count("npc los") == "2");
    seen = false;
    CHECK(count("npc los") == "1");

    // No alert lists: nearalert finds nothing, the others ask nothing.
    CHECK(count("npc nearalert 1") == "0");
    CHECK(count("npc alert 1 noalert 2 notnearalert 3") == "2");
}

TEST_CASE("spawn search: fellowship membership and member corpses") {
    Fixture f;
    auto& c = *f.world.conn;
    auto& w = net::GameConnectionTestAccess::world(c);
    net::FellowshipState fellowship;
    fellowship.id = 75;
    net::FellowshipMember member;
    member.character_id = 9876; member.name = "Tester";
    fellowship.members.push_back(member);
    auto src = mq::sources_of(c);
    src.fellowship = &fellowship;
    auto found = mq::search_spawns(mq::parse_spawn_search("pc fellowship"), w, src);
    REQUIRE(found.size() == 1);
    CHECK(found.front()->id == w.player_id);
    member.character_id = 9877; member.name = "Groupie";
    fellowship.members.push_back(member);
    CHECK(mq::search_spawns(mq::parse_spawn_search("pc fellowship"), w, src).size() == 2);
    f.world.add(99, "Groupie's corpse01", false, 48, {1, 1, 1});
    w.spawns[99].is_corpse = true;
    w.spawns[99].deity = 201; // MQ distinguishes a player's corpse by its nonzero deity
    found = mq::search_spawns(mq::parse_spawn_search("pccorpse fellowship"), w, src);
    REQUIRE(found.size() == 1);
    CHECK(found.front()->id == 99);
    fellowship.members.clear();
    CHECK(mq::search_spawns(mq::parse_spawn_search("fellowship"), w, src).empty());
}

TEST_CASE("spawn search: xtarhater and the guild words") {
    Fixture f;
    auto& c = *f.world.conn;
    auto& w = net::GameConnectionTestAccess::world(c);
    mq::SearchSources src = mq::sources_of(c);
    game::XTargetState xt;
    xt.slots.resize(3);
    xt.slots[0] = {1, 1, 10, "a rat"};
    xt.slots[1] = {2, 1, 11, "Fippy"}; // a specific NPC, not an auto hater
    src.xtargets = &xt;
    CHECK(mq::search_spawns(mq::parse_spawn_search("xtarhater"), w, src).size() == 1);
    xt.slots[0].status = 0; // empty
    CHECK(mq::search_spawns(mq::parse_spawn_search("xtarhater"), w, src).empty());

    game::GuildState guild;
    guild.names[7] = "Hailstone Testers";
    guild.guild_id = 7;
    w.spawns[12].guild_id = 7;
    auto q = mq::parse_spawn_search("guild", &guild);
    CHECK(q.guild_id == 7);
    CHECK(mq::search_spawns(q, w, src).size() == 1);
    auto byname = mq::parse_spawn_search("guildname \"hailstone testers\"", &guild);
    CHECK(byname.guild_id == 7);
    CHECK(byname.name.empty());
    // Not in a guild: "guild" is no filter.
    guild.guild_id = 0xffffffff;
    CHECK(mq::search_spawns(mq::parse_spawn_search("guild", &guild), w, src).size() == w.spawns.size());
}

TEST_CASE("TLO: Group, items, Zone and the rest") {
    Fixture f;
    CHECK(f.p("${Group.Members}") == "1");
    CHECK(f.p("${Group.GroupSize}") == "2");
    CHECK(f.p("${Group.Member[0]}") == "Tester");
    CHECK(f.p("${Group.Member[1].Name}") == "Groupie");
    CHECK(f.p("${Group.Member[1].Distance}") == "20.00"); // groupmember inherits spawn
    CHECK(f.p("${Group.Member[Groupie]}") == "1");
    CHECK(f.p("${Group.Leader}") == "Groupie");
    CHECK(f.p("${Group.MainTank.Name}") == "Groupie");
    CHECK(f.p("${Zone.ShortName} / ${Zone}") == "qeynos2 / North Qeynos");
    CHECK(f.p("${FindItem[potion].ID}") == "5001");
    CHECK(f.p("${FindItem[=Healing Potion].Stack}") == "5");
    CHECK(f.p("${FindItem[=Potion]}") == "NULL");
    CHECK(f.p("${FindItemCount[healing potion]}") == "7"); // 5 loose + 2 in the bag
    // A number is the item's id, as in MQ (not a name holding those digits); an empty index is NULL.
    CHECK(f.p("${FindItem[5001]} ${FindItemCount[5001]}") == "Healing Potion 7");
    CHECK(f.p("${FindItemCount[500]} ${FindItem[]} ${FindItemCount[]}") == "0 NULL NULL");
    CHECK(f.p("${Me.Inventory[pack2].Container}") == "4");
    CHECK(f.p("${Me.Inventory[pack2].Item[1]}") == "Healing Potion");
    CHECK(f.p("${Me.FreeInventory}") == "11"); // 8 empty general slots + 3 in the bag
    CHECK(f.p("${Cursor}") == "NULL");
    CHECK(f.p("${Window[InventoryWindow].Open} ${Window[BankWnd].Open}") == "TRUE FALSE");
    // pack1..10: TRUE while the slot holds a bag (its slots work with the window closed).
    CHECK(f.p("${Window[pack2].Open} ${Window[pack1].Open} ${Window[pack11].Open}") == "TRUE FALSE FALSE");
    CHECK(f.p("${EverQuest.GameState} ${EverQuest.Server}") == "INGAME TestServer");
    // MacroQuest.Path[config] is the scripts' settings folder; MQ's other names stay NULL until settled.
    CHECK(f.p("${MacroQuest.Path[config]}") == "NULL"); // no folder given
    f.ctx.config_path = "/data/scripts/config";
    CHECK(f.p("${MacroQuest.Path[config]} ${MacroQuest.Path[CONFIG]}") == "/data/scripts/config /data/scripts/config");
    CHECK(f.p("${MacroQuest.Path[macros]} ${MacroQuest.Path}") == "NULL NULL");
    // Hailstone.Path is the same member under the client's name (TLO names take any case).
    CHECK(f.p("${Hailstone.Path[config]} ${HailStone.Path[Config]}") == "/data/scripts/config /data/scripts/config");
    CHECK(f.p("${Hailstone.Path[macros]} ${Hailstone.Path}") == "NULL NULL");
    // Their other members are EverQuest's, as in MQ: the server's name, the game state.
    CHECK(f.p("${MacroQuest.Server} ${Hailstone.Server}") == "TestServer TestServer");
    CHECK(f.p("${MacroQuest.Server.Equal[testserver]} ${MacroQuest.GameState}") == "TRUE INGAME");
    // Build is MQ's build target (4, Emu), a number E3 casts; BuildDate the build's yyyymmdd, Version Hailstone's.
    CHECK(f.p("${MacroQuest.Build} ${Hailstone.Build}") == "4 4");
    const std::string date = std::to_string(build_date());
    CHECK(date.size() == 8);
    CHECK(build_date() >= 20260101);
    CHECK(f.p("${MacroQuest.BuildDate}") == date);
    CHECK(f.p("${Hailstone.BuildDate}") == date);
    CHECK(f.p("${MacroQuest.Version}") == release_version());
    CHECK(f.p("${Hailstone.Version}") == release_version());
    CHECK(f.p("${Int[7.9]} ${Float[2].Deci} ${Bool[0]} ${Bool[yes]}") == "7 2.0 FALSE TRUE");
    // No whole number for NaN, an infinity or one past int64 (the cast would be undefined); in range is cut as before.
    CHECK(f.p("${Int[nan]} ${Int[inf]} ${Int[-inf]} ${Int[1e300]} ${Int[-1e300]}") == "NULL NULL NULL NULL NULL");
    CHECK(f.p("${Float[nan].Int} ${Float[inf].Int} ${Float[1e300].Int}") == "NULL NULL NULL");
    CHECK(f.p("${Int[-7.9]} ${Float[1e18].Int} ${Float[-2.5].Int}") == "-7 1000000000000000000 -2");
    CHECK(f.p("${Select[b,a,b,c]}") == "2");
    CHECK(f.p("${Heading[90].ShortName}") == "E");
    CHECK(f.p("${Heading[0].ShortName}") == "N");
}

TEST_CASE("TLO: an index past the type it names is none, not the one it wraps to") {
    Fixture f;
    // Each wraps to something the fixture has: spell 200, spawn 1, General1 (slot 23), bag sub 0, member 0, gem 1.
    CHECK(f.p("${Spell[200]}") == "Minor Healing");
    CHECK(f.p("${Spell[4294967496]}") == "NULL");
    CHECK(f.p("${Spawn[1]}") == "Tester");
    CHECK(f.p("${Spawn[4294967297]}") == "NULL");
    CHECK(f.p("${Me.Inventory[23]}") == "Healing Potion");
    CHECK(f.p("${Me.Inventory[65559]}") == "NULL");
    CHECK(f.p("${Me.Inventory[-1]}") == "NULL");
    CHECK(f.p("${Me.Inventory[pack2].Item[1]}") == "Healing Potion");
    CHECK(f.p("${Me.Inventory[pack2].Item[65537]}") == "NULL");
    CHECK(f.p("${Group.Member[0]}") == "Tester");
    CHECK(f.p("${Group.Member[4294967296]}") == "NULL");
    CHECK(f.p("${Me.SpellReady[1]}") == "TRUE");
    CHECK(f.p("${Me.SpellReady[4294967297]}") == "FALSE");
    CHECK(f.p("${Me.SpellReady[2147483648]}") == "FALSE"); // (once INT_MIN - 1)
    CHECK(f.p("${Me.SpellReady[-2147483648]}") == "FALSE");
}

TEST_CASE("TLO: an item's TimerReady, its own and its recast type's click reuse, as MQ's") {
    Fixture f;
    auto& conn = *f.world.conn;
    auto& inventory = net::GameConnectionTestAccess::inventory(conn);
    auto* item = inventory.at(net::ItemSlot::possessions(net::kSlotGeneral1 + 1, 0));
    REQUIRE(item != nullptr);
    item->name = "Fixture Clicky";
    auto& click = item->effects[net::kEffectClick];
    click.effect = 1;
    click.extra[3] = 17;

    CHECK(f.p("${FindItem[=Missing Clicky].TimerReady}") == "NULL");
    CHECK(f.p("${FindItem[=Fixture Clicky].TimerReady}") == "0");
    // MQ's: NULL for an item without a click effect.
    click.effect = 0;
    CHECK(f.p("${FindItem[=Fixture Clicky].TimerReady}") == "NULL");
    click.effect = 1;

    conn.set_item_recast_timer(item, 120, -1);
    const int left = std::stoi(f.p("${FindItem[=Fixture Clicky].TimerReady}"));
    CHECK(left > 0);
    CHECK(left <= 120);

    conn.reset_all_item_recast_timers();
    conn.set_item_recast_timer(nullptr, 120, 17);
    CHECK(std::stoi(f.p("${Me.Inventory[pack2].Item[1].TimerReady}")) > 0);

    conn.reset_all_item_recast_timers();
    CHECK(f.p("${FindItem[=Fixture Clicky].TimerReady}") == "0");
}

TEST_CASE("TLO: Math, If and Range follow MQ's") {
    Fixture f;
    CHECK(f.p("${Math.Calc[1+2*3]}") == "7.00");
    CHECK(f.p("${Math.Calc[${Me.PctHPs}<80]}") == "1.00");
    CHECK(f.p("${Math.Calc[1/0]} ${Math.Calc[]} ${Math.Calc[x]} ${Math}") == "NULL NULL NULL NULL");
    CHECK(f.p("${Math.Calc[1/3]}") == "0.33");
    CHECK(mq::evaluate(f.ctx, "Math.Calc[1/3]").f == static_cast<double>(1.0f / 3.0f)); // MQ keeps a float
    CHECK(f.p("${Math.Abs[-2.5]} ${Math.Sqrt[16]} ${Math.Sin[90]} ${Math.Cos[180]} ${Math.Asin[1]}") ==
          "2.50 4.00 1.00 -1.00 90.00");
    CHECK(f.p("${Math.Hex[255]} ${Math.Hex[-1]} ${Math.Not[0]}") == "0xFF 0xFFFFFFFF -1");
    CHECK(f.p("${Math.Dec[ff]} ${Math.Dec[0x10]} ${Math.Dec[FFFFFFFF]} ${Math.Dec[-1]} ${Math.Dec[zz]}") ==
          "255 16 -1 -1 NULL");
    CHECK(f.p("${Math.Dec[123456789ab]}") == "-1"); // past 32 bits: held at 0xFFFFFFFF
    CHECK(f.p("${Math.Clamp[15,1,10]} ${Math.Clamp[5 1 10]} ${Math.Clamp[0,1,10]} ${Math.Clamp[5,9,1]}") ==
          "10 5 1 9");
    CHECK(f.p("${Math.Rand[5,5]} ${Math.Rand[6,5]}") == "5 NULL");
    for (int i = 0; i < 50; ++i) {
        const auto v = mq::evaluate(f.ctx, "Math.Rand[3]");
        REQUIRE(v.kind == Value::Kind::Int);
        CHECK(v.i >= 0);
        CHECK(v.i <= 3);
    }
    CHECK(f.p("${Math.Distance[3,4,0:0,0,0]}") == "5.00");
    CHECK(f.p("${Math.Distance[3 4 12:0 0 0]}") == "13.00");
    CHECK(f.p("${Math.Distance[${Me.Y},${Me.X},${Me.Z}]}") == "0.00"); // the second point is the player

    CHECK(f.p("${If[1>2,yes,no]} ${If[TRUE,yes,no]}") == "no yes");
    CHECK(f.p("${If[1,a,b,c]} ${If[0,a,b,c]}") == "a b,c"); // the false branch runs to the end
    CHECK(f.p("${If[1~a,b~c]} ${If[0~a,b~c]}") == "a,b c");
    CHECK(f.p("${If[x,a,b]} ${If[1,a]} ${If[]}") == "NULL NULL NULL");

    CHECK(f.p("${Range.Between[1,10:10]} ${Range.Inside[1,10:10]} ${Range.Inside[1,10:5]}") == "TRUE FALSE TRUE");
    CHECK(f.p("${Range.Between[1,10]} ${Range.Between[]}") == "FALSE NULL");
}

TEST_CASE("TLO: int, float and string members as MQ's") {
    Fixture f;
    CHECK(f.p("${Int[255].Hex} ${Int[-1].Hex} ${Int[4294967296].Hex}") == "0xFF 0xFFFFFFFF 0x100000000");
    CHECK(f.p("${Int[65538].LowPart} ${Int[65538].HighPart} ${Int[8589934595].LowPart} ${Int[8589934595].HighPart}") ==
          "2 1 3 2");
    CHECK(f.p("${Int[1234567].Prettify} ${Int[-1234567].Prettify[2]} ${Int[123].Prettify}") ==
          "1,234,567 -1,234,567.00 123");
    CHECK(f.p("${Int[5].Double} ${Int[5].Float}") == "5.00 5.00");
    CHECK(f.p("${Float[1234.5].Prettify} ${Float[1234567.891].Prettify[1]}") == "1,234.50 1,234,567.9");
    CHECK(f.p("${Float[1.25].Precision} ${Float[1.25].Precision[1]}") == "NULL 1.2");

    CHECK(f.p("${String[abc].Left[0]} ${String[abc].Left[-1]} ${String[abc].Right[-1]} ${String[abc].Right[5]}") ==
          "NULL ab bc abc");
    CHECK(f.p("${String[abcdef].Mid[0,2]} ${String[abcdef].Mid[2,]} ${String[abcdef].Mid[2,-1]} ${String[abc].Mid[2]}") ==
          "ab bcdef bcdef NULL");
    CHECK(f.p("${String[abc].Mid[9,1]}|") == "|");
    CHECK(f.p("${String[abc].Find[]} ${String[abc].Count[]} ${String[abc].Equal[]}") == "NULL NULL NULL");
    CHECK(f.p("${String[abc].Compare[ABD]} ${String[abc].Compare[ABC]} ${String[b].CompareCS[a]}") == "-1 0 1");
    CHECK(f.p("${String[abc].NotEqualCS[ABC]} ${String[abc].NotEqualCS[abc]}") == "TRUE FALSE");
    // Arg is MQ's GetArg: runs of separators are one, quotes group, an empty argument is NULL.
    CHECK(f.p("${String[a,,b].Arg[2,,]} ${String[\"a b\" c].Arg[1]} ${String[a b].Arg[3]} ${String[a b].Arg[x]}") ==
          "b a b NULL NULL");
    CHECK(f.p("${String[a  b].Arg[2]}") == "b");
    // Token looks for each next separator from one past the last one.
    CHECK(f.p("${String[a,b].Token[1,,]} ${String[,a,b].Token[2,,]} ${String[a,b].Token[3,,]}") == "a b NULL");
    CHECK(f.p("${String[a,,b].Token[2,,]}|${String[a,,b].Token[3,,]}") == "|b");
    CHECK(f.p("${String[aXbXc].Replace[X,--]} ${String[aaa].Replace[a,aa]} ${String[abc].Replace[x]}") ==
          "a--b--c aaaaaa NULL");
    // Replace nested in itself, each making its text ten times longer: cut to 2047 bytes, not grown without end.
    std::string grow = "aaaaaaaaaa";
    for (int i = 0; i < 30; ++i) grow = "${String[" + grow + "].Replace[a,aaaaaaaaaa]}";
    CHECK(f.p(grow) == std::string(2047, 'a'));
    CHECK(f.p("${String[" + std::string(300, 'a') + "].Replace[a,bbbbbbbbbb]}") == std::string(2047, 'b'));
    CHECK(f.p("${String[plain text].StripLinks}") == "plain text");
}

TEST_CASE("parse: strings, nesting, unknowns") {
    Fixture f;
    CHECK(f.p("${Me.Name.Length}") == "6");
    CHECK(f.p("${Me.Name.Upper}") == "TESTER");
    CHECK(f.p("${Me.Name.Left[3]}") == "Tes");
    CHECK(f.p("${String[a,b c].Arg[2]}") == "c");
    CHECK(f.p("${Me.Gem[${Int[1]}]}") == "Minor Healing");
    CHECK(f.p("${Target.Name.Find[rat]}") == "3");
    CHECK(f.p("${Nope}") == "NULL");
    CHECK(f.p("${Me.Nope.Deeper}") == "NULL");
    CHECK(f.p("no tokens here") == "no tokens here");
    CHECK(f.p("${Me.Name") == "${Me.Name"); // unclosed
    CHECK(f.p("/tell ${Group.Member[1]} hp ${Me.PctHPs}%") == "/tell Groupie hp 75%");
    // Plain values for Lua: an object's plain value is its type's.
    Value v = mq::evaluate(f.ctx, "Me.Gem[1]");
    CHECK(v.kind == Value::Kind::Object);
    CHECK(f.reg.plain(f.ctx, v).s == "Minor Healing");
    CHECK(mq::evaluate(f.ctx, "Me.PctHPs").i == 75);
    CHECK(mq::evaluate(f.ctx, "Target.Distance").f == doctest::Approx(50.0));
}

TEST_CASE("parse: a value's text is not parsed again (a name holding ${...} stays text)") {
    Fixture f;
    // Spawn names, guild names, titles and item names are what the server and other players chose.
    net::GameConnectionTestAccess::world(*f.world.conn).spawns[11].name = "x${Me.Name}${Me.PctHPs}";
    const std::string name = f.p("${Spawn[id 11].CleanName}");
    CHECK(name.find("${Me.Name}") != std::string::npos);
    CHECK(name.find("Tester") == std::string::npos);
    CHECK(f.p("hi ${Spawn[id 11].CleanName}!") == "hi " + name + "!");
    // A value inside an index is the index's text, not parsed again either.
    CHECK(f.p("${Spawn[id ${Spawn[id 11].ID}].ID}") == "11");
    // The macro's own nesting still works, any number of levels and side by side.
    CHECK(f.p("${Me.Gem[${Int[${Int[1]}]}]} ${Me.Name}") == "Minor Healing Tester");
    CHECK(f.p("} ${Me.Name}} {") == "} Tester} {");
    CHECK(f.p("${Me.Name ${Me.Name}") == "${Me.Name Tester"); // the outer one unclosed: as typed
    // Deep nesting stops at a limit instead of running the stack out.
    std::string deep;
    for (int i = 0; i < 5000; ++i) deep += "${";
    CHECK(f.p(deep) == deep);
}

TEST_CASE("TLO: a registry's own TLOs come and go") {
    Fixture f;
    f.reg.add_tlo("MyPlugin", [](const mq::Context&, std::string_view index) {
        return Value::string("hello " + std::string(index));
    });
    CHECK(f.p("${MyPlugin[there]}") == "hello there");
    CHECK(f.p("${MyPlugin[x].Length}") == "7");
    f.reg.remove_tlo("myplugin");
    CHECK(f.p("${MyPlugin}") == "NULL");
}

TEST_CASE("TLO: numbers from text out of range, NaN or infinite are no id, loc or heading") {
    Fixture f;
    // Ids are read as MQ's GetIntFromString: the integer at the start, nothing (0, no spawn) when there is none or
    // it is past 32 bits; "-5" is 0xfffffffb, no spawn either.
    for (const char* q : {"id -5", "id nan", "id 4294967296", "id x"}) {
        INFO(q);
        CHECK(mq::search_spawns(mq::parse_spawn_search(q), f.world.conn->world(), nullptr).empty());
    }
    CHECK(f.p("${Spawn[id -5]}") == "NULL");
    CHECK(mq::parse_spawn_search("id 1e300").id == 1u); // the "1"
    CHECK(mq::parse_spawn_search("id 7").id == 7u);
    // A loc of NaN or infinity measures nothing (a NaN distance is never past the radius), and no radius limits
    // nothing: no undefined cast either way.
    CHECK(mq::search_spawns(mq::parse_spawn_search("loc nan 0"), f.world.conn->world(), nullptr).size() == 5);
    CHECK(mq::search_spawns(mq::parse_spawn_search("loc inf 0 0 radius 10"), f.world.conn->world(), nullptr).empty());
    CHECK(mq::parse_spawn_search("loc 1e300 0").loc->x == 0.0f); // past float: not read, as MQ's float read
    // A huge radius is no limit, a NaN one too; levels read as integers.
    auto s = mq::parse_spawn_search("notid -1 radius 1e300 level 1e300");
    CHECK(s.not_id == 0xffffffffu);
    CHECK(s.radius >= 10000.0);
    CHECK(s.max_level == 1);
    CHECK(mq::search_spawns(mq::parse_spawn_search("radius nan"), f.world.conn->world(), nullptr).size() == 5);
    auto r = mq::parse_spawn_search("range -1e300 nan");
    CHECK(r.min_level == -1);
    CHECK(r.max_level == 200);
    // LineOfSight[] passes the collision grid only finite points within float.
    int asked = 0;
    f.ctx.line_of_sight = [&](const std::array<float, 3>&, const std::array<float, 3>&) -> std::optional<bool> {
        ++asked;
        return true;
    };
    CHECK(f.p("${LineOfSight[1e300,0,0:0,0,0]}") == "NULL");
    CHECK(f.p("${LineOfSight[0,nan,0:0,0,0]}") == "NULL");
    CHECK(f.p("${LineOfSight[0,0,0:0,0,-inf]}") == "NULL");
    CHECK(asked == 0);
    CHECK(f.p("${LineOfSight[1,2,3:4,5,6]}") == "TRUE");
    CHECK(asked == 1);
    CHECK(f.p("${Heading[nan]}") == "NULL");
    CHECK(f.p("${Heading[inf].ShortName}") == "NULL");
    CHECK(f.p("${Heading[-90].ShortName}") == "W");
    CHECK(mq::in_range<std::uint8_t>(300.0) == std::nullopt);
    CHECK(mq::in_range<std::uint8_t>(44.9) == std::uint8_t{44});
    CHECK(mq::in_range<std::uint32_t>(-0.5) == 0u); // cut toward zero, as a cast
    // Read at run time, as the parsers' numbers are: MSVC folds a constant 1e300 into the guarded cast (C4756).
    const double huge = std::stod("1e300");
    CHECK(mq::in_range<float>(huge) == std::nullopt);
    CHECK(mq::in_range<float>(std::numeric_limits<double>::quiet_NaN()) == std::nullopt);
}

TEST_CASE("TLO: GameTime advances between packets and carries the game calendar") {
    test::ManualClock clock;
    clock.hold();
    net::ConnectionOptions options;
    options.session.clock = clock.source();
    mq_test::World world("hailstone_mq_game_time_test", options);
    mq::Registry reg;
    mq::register_game_tlos(reg);
    mq::Context ctx;
    ctx.registry = &reg;
    ctx.conn = world.conn.get();
    auto parse = [&](std::string_view text) { return mq::parse_macro_data(ctx, text); };
    CHECK(parse("${GameTime}") == "NULL");
    world.conn->receive_zone_packet_for_test(net::encode(net::TimeOfDay{18, 59, 28, 12, 3200}));
    CHECK(parse("${GameTime}") == "18:59");
    CHECK(parse("${GameTime.Night}") == "FALSE");
    clock.advance(std::chrono::milliseconds{2999});
    CHECK(parse("${GameTime.Time24}") == "18:59");
    clock.advance(std::chrono::milliseconds{1});
    CHECK(parse("${GameTime}") == "19:00");
    CHECK(parse("${GameTime.Time24}") == "19:00");
    CHECK(parse("${GameTime.Hour}:${GameTime.Minute}") == "19:0");
    CHECK(parse("${GameTime.Day}/${GameTime.Month}/${GameTime.Year}") == "28/12/3200");
    CHECK(parse("${GameTime.Night}") == "TRUE");
    clock.advance(std::chrono::milliseconds{6 * 60 * 3000});
    CHECK(parse("${GameTime}") == "01:00");
    CHECK(parse("${GameTime.Day}/${GameTime.Month}/${GameTime.Year}") == "1/1/3201");
    CHECK(parse("${GameTime.Night}") == "TRUE");
}

TEST_CASE("spawn search: random text parses and searches without fault") {
    // The parser reads what scripts and other players' names put in ${...}: any bytes, any keyword with its words
    // missing (run it under the sanitizer build).
    Fixture f;
    static constexpr const char* kWords[] = {"loc", "range", "id", "name", "light", "guildname", "nopcnear", "radius",
                                             "zradius", "playerstate", "notid", "\"", "=", "#", "nan", "-1", "1e300",
                                             "0", "npc", "pcpet", "class", "body", "race", "group", "\t"};
    std::uint32_t seed = 12345;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    for (int n = 0; n < 2000; ++n) {
        std::string text;
        const auto len = next() % 8;
        for (std::uint32_t k = 0; k < len; ++k) {
            if (next() % 4 == 0) text += static_cast<char>(next() % 256);
            else text += kWords[next() % std::size(kWords)];
            if (next() % 2) text += ' ';
        }
        auto q = mq::parse_spawn_search(text, &f.world.conn->guild());
        CHECK_NOTHROW(mq::search_spawns(q, f.world.conn->world(), mq::sources_of(*f.world.conn)));
    }
}

TEST_CASE("TLO: Me.Bandolier[name|#] and .Active, .Index, .Name, .Item[n]") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& sets = A::bandolier(*f.world.conn);
    sets.assign(game::kBandolierSets, {});
    auto belt = [](std::uint32_t id, const char* name) {
        game::BeltItem b;
        b.id = id;
        b.icon = id + 1;
        b.name = name;
        return b;
    };
    sets[0].name = "Melee";
    sets[0].items = {belt(5101, "Cudgel"), belt(5102, "Buckler"), belt(0, ""), belt(0, "")};
    sets[2].name = "Bow";
    sets[2].items = {belt(0, ""), belt(0, ""), belt(5103, "Longbow"), belt(5104, "Arrow")};
    auto wear = [&](std::int16_t slot, std::uint32_t id) {
        net::Item it;
        it.id = id;
        it.name = "worn";
        A::inventory(*f.world.conn).put(net::ItemSlot::possessions(slot), it);
    };

    // By name (any case) or number; a name no set has is NULL.
    CHECK(f.p("${Me.Bandolier[melee]} ${Me.Bandolier[MELEE].Index} ${Me.Bandolier[Bow].Index}") == "Melee 1 3");
    CHECK(f.p("${Me.Bandolier[1].Name} ${Me.Bandolier[3].Name}") == "Melee Bow");
    CHECK(f.p("${Me.Bandolier[Caster]}") == "NULL");
    CHECK(f.p("${Me.Bandolier[Caster].Active}") == "NULL");
    // A set without a name answers NULL; numbers are clamped to 1..20.
    CHECK(f.p("${Me.Bandolier[2].Active} ${Me.Bandolier[2].Name}") == "NULL NULL");
    CHECK(f.p("${Me.Bandolier[0].Name} ${Me.Bandolier[99].Name}") == "Melee NULL");

    // Active: each of the four slots holds the set's item, an empty one an empty slot.
    CHECK(f.p("${Me.Bandolier[Melee].Active} ${Me.Bandolier[Bow].Active}") == "FALSE FALSE");
    wear(net::kSlotPrimary, 5101);
    CHECK(f.p("${Me.Bandolier[Melee].Active}") == "FALSE"); // the Buckler is not on
    wear(net::kSlotSecondary, 5102);
    CHECK(f.p("${Me.Bandolier[Melee].Active} ${Me.Bandolier[Bow].Active}") == "TRUE FALSE");
    wear(net::kSlotAmmo, 5104); // an item in a slot the set leaves empty
    CHECK(f.p("${Me.Bandolier[Melee].Active}") == "FALSE");
    (void)A::inventory(*f.world.conn).take(net::ItemSlot::possessions(net::kSlotPrimary));
    (void)A::inventory(*f.world.conn).take(net::ItemSlot::possessions(net::kSlotSecondary));
    wear(net::kSlotRange, 5103);
    CHECK(f.p("${Me.Bandolier[Melee].Active} ${Me.Bandolier[Bow].Active}") == "FALSE TRUE");

    // Item[n]: the set's items (1 primary .. 4 ammo, clamped).
    CHECK(f.p("${Me.Bandolier[Melee].Item[1]} ${Me.Bandolier[Melee].Item[2].ID}") == "Cudgel 5102");
    CHECK(f.p("${Me.Bandolier[Bow].Item[4].Name} ${Me.Bandolier[Bow].Item[4].IconID}") == "Arrow 5105");
    CHECK(f.p("${Me.Bandolier[Bow].Item[9].Name} ${Me.Bandolier[Bow].Item[0].ID}") == "Arrow 0");
    CHECK(f.p("${Me.Bandolier[Bow].Item}") == "NULL");
}

TEST_CASE("TLO: the aggro meter's readings (Me, Target, XTarget, Group members)") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    CHECK(f.p("${Me.PctAggro} ${Target.PctAggro} ${Target.SecondaryPctAggro}") == "0 0 0");
    CHECK(f.p("${Target.SecondaryAggroPlayer}") == "NULL");
    auto& ag = A::aggro(*f.world.conn);
    ag.pct[net::kAggroPlayer] = 45;
    ag.pct[net::kAggroSecondary] = 30;
    ag.pct[net::kAggroGroup1] = 12;
    ag.pct[net::kAggroXTarget1 + 1] = 88;
    ag.secondary_id = 12;
    CHECK(f.p("${Me.PctAggro} ${Target.PctAggro} ${Target.SecondaryPctAggro} ${Me.SecondaryPctAggro}") == "45 45 30 30");
    CHECK(f.p("${Target.SecondaryAggroPlayer.Name}") == "Groupie");
    // Target is still a spawn: its members and plain value are a spawn's.
    CHECK(f.p("${Target} ${Target.ID}") == "a rat 10");
    // The group's members by index: 0 the player, then the group's entries.
    CHECK(f.p("${Group.Member[0].PctAggro} ${Group.Member[1].PctAggro}") == "45 12");
    // Me.XTarget[n] is the slot's spawn, with the slot's own reading.
    auto& xt = A::xtargets(*f.world.conn);
    xt.slots.resize(5);
    xt.slots[1].spawn_id = 11;
    CHECK(f.p("${Me.XTargetSlots}") == "5");
    CHECK(f.p("${Me.XTarget[2]} ${Me.XTarget[2].ID} ${Me.XTarget[2].PctAggro}") == "Fippy Darkpaw 11 88");
    CHECK(f.p("${Me.XTarget[1]}") == "NULL");
}

TEST_CASE("TLO: Me's endurance, AA points, auto fire, cast and gem timers, disciplines") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    CHECK(f.p("${Me.Endurance}") == "50");
    CHECK(f.p("${Me.AutoFire}") == "FALSE");
    A::profile(*f.world.conn)->aa_points_spent = 20;
    A::alt_adv(*f.world.conn).assigned[0] = 20;
    CHECK(f.p("${Me.AAPointsTotal} ${Me.AAPointsAssigned}") == "27 20");
    // Nothing being cast: no time left.
    CHECK(f.p("${Me.CastTimeLeft}") == "0");
    auto& sp = A::spells(*f.world.conn);
    sp.casting.spell = 200;
    sp.casting.eta_ms = net::GameConnection::now_ms() + 2500;
    const int cast_left = std::stoi(f.p("${Me.CastTimeLeft}"));
    CHECK(cast_left > 2000);
    CHECK(cast_left <= 2500);
    sp.casting = {};
    // A gem's recast: by number or by the spell's name; NULL for an empty gem.
    CHECK(f.p("${Me.GemTimer[1]}") == "0");
    sp.gems[0].ready = net::GameConnection::eq_time() + 5000;
    const int gem_left = std::stoi(f.p("${Me.GemTimer[Minor Healing]}"));
    CHECK(gem_left > 4000);
    CHECK(gem_left <= 5000);
    CHECK(f.p("${Me.GemTimer[1].TotalSeconds}") == std::to_string(gem_left / 1000));
    CHECK(f.p("${Me.GemTimer[5]}") == "NULL");
    // Past int: no gem, not the one it wraps to (gem 1), nor INT_MIN - 1.
    CHECK(f.p("${Me.GemTimer[4294967297]}") == "NULL");
    CHECK(f.p("${Me.GemTimer[2147483648]}") == "NULL");
    // The active discipline: the last buff slot's.
    CHECK(f.p("${Me.ActiveDisc}") == "NULL");
    CHECK(f.p("${Bool[${Me.ActiveDisc.ID}]}") == "FALSE");
    sp.buffs[game::kBuffSlots - 1].spell = 4498;
    sp.buffs[game::kBuffSlots - 1].tics = 10;
    CHECK(f.p("${Me.ActiveDisc} ${Me.ActiveDisc.ID}") == "Test Discipline 4498");
    // Combat abilities: the n-th one's spell, or a name's number.
    A::profile(*f.world.conn)->disciplines = {0, 4498};
    CHECK(f.p("${Me.CombatAbility[2]} ${Me.CombatAbility[Test Discipline]}") == "Test Discipline 2");
    CHECK(f.p("${Me.CombatAbility[1]} ${Me.CombatAbility[Holy Armor]}") == "NULL NULL");
    // No ability source in this fixture: no skill.
    CHECK(f.p("${Me.Ability[Kick]}") == "FALSE");
}

TEST_CASE("TLO: Me.AltAbility by name or ability id, with its rank, reuse and spell") {
    Fixture f;
    auto& aa = net::GameConnectionTestAccess::alt_adv(*f.world.conn);
    game::AltAbility a;
    a.id = 1003;
    a.group = 5;
    a.rank = 3;
    a.max_rank = 5;
    a.title_sid = 9001; // Innate Strength
    a.spell = 200;
    a.refresh = 600;
    aa.add(a);
    aa.owned = {{1003, 3, 0}};
    CHECK(f.p("${Me.AltAbility[Innate Strength]}") == "5");
    CHECK(f.p("${Me.AltAbility[5].Name}") == "Innate Strength");
    CHECK(f.p("${Me.AltAbility[4294967301]}") == "NULL"); // 5 + 2^32: no group, not group 5
    CHECK(f.p("${Me.AltAbility[innate strength].ID} ${Me.AltAbility[5].Rank} ${Me.AltAbility[5].MaxRank}") == "5 3 5");
    CHECK(f.p("${Me.AltAbility[5].ReuseTime} ${Me.AltAbility[5].Passive}") == "600 FALSE");
    CHECK(f.p("${Me.AltAbility[5].Spell} ${Me.AltAbility[5].Spell.ID} ${Me.AltAbility[5].Spell.Mana}") ==
          "Minor Healing 200 10");
    CHECK(f.p("${Bool[${Me.AltAbility[Innate Strength].Spell}]}") == "TRUE");
    CHECK(f.p("${Me.AltAbility[Mystical Shielding]} ${Me.AltAbility[6]}") == "NULL NULL"); // not owned
}

// MQ's AltAbility TLO: any ability the AA table holds, owned or not (Me.AltAbility answers only the player's).
TEST_CASE("TLO: AltAbility finds an ability the table holds by name or group id, owned or not") {
    Fixture f;
    auto& aa = net::GameConnectionTestAccess::alt_adv(*f.world.conn);
    game::AltAbility r1;
    r1.id = 2001;
    r1.group = 6;
    r1.rank = 1;
    r1.max_rank = 2;
    r1.title_sid = 9002; // Mystical Shielding
    r1.spell = 11;       // Holy Armor
    game::AltAbility r2 = r1;
    r2.id = 2002;
    r2.rank = 2;
    r2.spell = 200; // Minor Healing
    aa.add(r1);
    aa.add(r2);
    // Not owned: the first rank the table holds, by name (any case) or group id.
    CHECK(f.p("${AltAbility[Mystical Shielding]} ${AltAbility[mystical shielding].Rank}") == "6 1");
    CHECK(f.p("${AltAbility[6].Rank} ${AltAbility[6].Spell} ${AltAbility[6].Spell.ID}") == "1 Holy Armor 11");
    // E3's check before it reads the spell's id.
    CHECK(f.p("${Bool[${AltAbility[Mystical Shielding].Spell}]}") == "TRUE");
    CHECK(f.p("${Me.AltAbility[Mystical Shielding]}") == "NULL");
    // Owned: the player's rank, by name.
    aa.owned = {{2002, 2, 0}};
    CHECK(f.p("${AltAbility[Mystical Shielding].Rank} ${AltAbility[Mystical Shielding].Spell.ID}") == "2 200");
    // Nothing of that name or group; an empty index.
    CHECK(f.p("${AltAbility[Innate Strength]} ${AltAbility[99]} ${AltAbility[]}") == "NULL NULL NULL");
}

// MQ's FindItemBank and FindItemBankCount: the bank, then the shared bank, bags included; a number is an item id.
TEST_CASE("TLO: FindItemBank and FindItemBankCount search the bank and the shared bank") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& inv = A::inventory(*f.world.conn);
    auto make = [](std::uint32_t id, const char* name, std::uint32_t count) {
        net::Item it;
        it.id = id;
        it.name = name;
        it.stack_count = count;
        return it;
    };
    net::Item bag = make(17005, "Large Sack", 0);
    bag.item_class = 1;
    bag.bag_slots = 4;
    REQUIRE(inv.put(net::ItemSlot{net::kInvBank, 3, -1, -1, 0, 0}, bag));
    REQUIRE(inv.put(net::ItemSlot{net::kInvBank, 3, 1, -1, 0, 0}, make(13073, "Bone Chips", 20)));
    REQUIRE(inv.put(net::ItemSlot{net::kInvBank, 7, -1, -1, 0, 0}, make(13073, "Bone Chips", 5)));
    REQUIRE(inv.put(net::ItemSlot{net::kInvSharedBank, 0, -1, -1, 0, 0}, make(13074, "Zombie Skin", 2)));
    // Carried items are not the bank's.
    REQUIRE(inv.put(net::ItemSlot::possessions(net::kSlotGeneral1), make(13075, "Bone Chips of the Ages", 1)));
    CHECK(f.p("${FindItemBankCount[=Bone Chips]} ${FindItemBankCount[bone]} ${FindItemBankCount[13073]}") == "25 25 25");
    CHECK(f.p("${FindItemBankCount[=Zombie Skin]} ${FindItemBankCount[=Rusty Dagger]}") == "2 0");
    // The first found: the bank before the shared bank, a bag's contents after the bag.
    CHECK(f.p("${FindItemBank[=Bone Chips]} ${FindItemBank[=Bone Chips].ItemSlot}") == "Bone Chips 3");
    CHECK(f.p("${FindItemBank[skin]} ${FindItemBank[13074].ID}") == "Zombie Skin 13074");
    CHECK(f.p("${Bool[${FindItemBank[=Bone Chips of the Ages]}]} ${FindItemBank[]}") == "FALSE NULL");
}

TEST_CASE("TLO: item members E3 reads (ItemReady, Timer, Clicky, stacks, worn slots, augments, links)") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& inv = A::inventory(*f.world.conn);
    net::Item wand;
    wand.name = "Wand of Testing";
    wand.id = 6001;
    wand.equip_slots = (1u << 13) | (1u << 14); // the hands
    wand.effects[net::kEffectClick].effect = 200;
    wand.effects[net::kEffectClick].type = 4;
    wand.tradeskills = 1;
    wand.no_drop = 0;
    wand.aug_sockets[0] = {7, 1, 0};
    wand.aug_sockets[2] = {3, 1, 0};
    net::Item stone;
    stone.name = "Aug Stone";
    stone.id = 6002;
    wand.contents.push_back({0, stone});
    inv.put(net::ItemSlot::possessions(net::kSlotGeneral1 + 2), wand);
    // A click effect without a recast: ready, its timer 0.
    CHECK(f.p("${Me.ItemReady[Wand of Testing]} ${Me.ItemReady[=wand of testing]} ${Me.ItemReady[6001]}") == "TRUE TRUE TRUE");
    CHECK(f.p("${FindItem[Wand].Timer} ${FindItem[Wand].Clicky} ${FindItem[Wand].EffectType}") == "0 Minor Healing Click Worn");
    // An item without a click effect is never ready and has no timer.
    CHECK(f.p("${Me.ItemReady[Healing Potion]} ${FindItem[Healing Potion].Timer}") == "FALSE NULL");
    CHECK(f.p("${Me.ItemReady[Nothing Like It]}") == "FALSE");
    // Past uint32: no item, not the id it wraps to (6001), and -1 is not 0xffffffff.
    CHECK(f.p("${Me.ItemReady[4294973297]} ${Me.ItemReady[-1]}") == "FALSE FALSE");
    // Stacks over the packs and their bags (5 + 2 of 20 each).
    CHECK(f.p("${FindItem[Healing Potion].StackCount} ${FindItem[Healing Potion].FreeStack}") == "7 33");
    CHECK(f.p("${FindItem[Wand].StackCount} ${FindItem[Wand].FreeStack}") == "0 0");
    CHECK(f.p("${FindItem[Wand].WornSlot[1]} ${FindItem[Wand].WornSlot[2]} ${FindItem[Wand].WornSlot[3]}") == "13 14 NULL");
    CHECK(f.p("${FindItem[Wand].WornSlot[mainhand]} ${FindItem[Wand].WornSlot[head]}") == "TRUE FALSE");
    CHECK(f.p("${FindItem[Wand].NoTrade} ${FindItem[Wand].Tradeskills}") == "TRUE TRUE");
    net::Item token;
    token.name = "Trade Token";
    token.no_drop = 255; // EQEmu's tradeable
    inv.put(net::ItemSlot::possessions(net::kSlotGeneral1 + 3), token);
    CHECK(f.p("${FindItem[Trade Token].NoTrade} ${FindItem[Trade Token].Tradeskills}") == "FALSE FALSE");
    // Augments: two visible sockets, the first holding a stone.
    CHECK(f.p("${FindItem[Wand].Augs}") == "2");
    CHECK(f.p("${FindItem[Wand].AugSlot[1]} ${FindItem[Wand].AugSlot[1].Name} ${FindItem[Wand].AugSlot[1].Type}") ==
          "Aug Stone Aug Stone 7");
    CHECK(f.p("${FindItem[Wand].AugSlot[1].Item.ID} ${FindItem[Wand].AugSlot[1].Empty}") == "6002 FALSE");
    CHECK(f.p("${FindItem[Wand].AugSlot[3]} ${FindItem[Wand].AugSlot[3].Empty} ${FindItem[Wand].AugSlot[7]}") ==
          "NULL TRUE NULL");
    CHECK(f.p("${Me.Inventory[pack3].AugSlot[1].Name}") == "Aug Stone");
    // ItemLink: the link's text, with the \x12 tags for CLICKABLE.
    const std::string link = f.p("${FindItem[Wand].ItemLink}");
    const std::string clickable = f.p("${FindItem[Wand].ItemLink[CLICKABLE]}");
    CHECK(link.find("Wand of Testing") != std::string::npos);
    CHECK(link.front() != '\x12');
    REQUIRE(clickable.size() == link.size() + 2);
    CHECK(clickable.front() == '\x12');
    CHECK(clickable.back() == '\x12');
}

TEST_CASE("TLO: spell members E3 reads, Spell.Duration in ticks") {
    Fixture f;
    CHECK(f.p("${Spell[Selo's Accelerando].DurationWindow} ${Spell[Holy Armor].DurationWindow}") == "1 0");
    CHECK(f.p("${Spell[Test Slow].Base[1]} ${Spell[Test Slow].Base[13]} ${Spell[Test Slow].SlowPct}") == "70 NULL 30");
    CHECK(f.p("${Spell[Holy Armor].SlowPct} ${Spell[Holy Armor].Dispellable} ${Spell[Holy Armor].EnduranceCost}") ==
          "0 TRUE 0");
    CHECK(f.p("${Spell[Holy Armor].Subcategory} ${Spell[Holy Armor].Description}") == "Unknown NULL");
    CHECK(f.p("${Spell[Minor Healing].MyRange}") == "100.00");
    // Duration: a plain value of ticks; the timestamp members on its milliseconds.
    const int ticks = std::stoi(f.p("${Spell[Holy Armor].Duration}"));
    CHECK(f.p("${Spell[Holy Armor].Duration.TotalSeconds}") == std::to_string(ticks * 6));
    // The player holds Holy Armor in buff slot 1: cast again it refreshes that slot, and it stacks with itself.
    CHECK(f.p("${Spell[Holy Armor].WillLand}") == "1");
    CHECK(f.p("${Spell[Holy Armor].StacksWith[Holy Armor]}") == "TRUE");
    CHECK(f.p("${Spell[Holy Armor].StacksWith[No Such Spell]}") == "NULL");
    // No pet buffs: the first pet slot.
    CHECK(f.p("${Spell[Holy Armor].WillLandPet}") == "1");
}

TEST_CASE("TLO: spawns' Aggressive, Primary, Secondary and Body; the group's Present, Injured and LowMana") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& rat = A::world(*f.world.conn).spawns[10];
    CHECK(f.p("${Spawn[10].Aggressive}") == "FALSE");
    rat.player_state = 4;
    CHECK(f.p("${Spawn[10].Aggressive} ${Target.Aggressive}") == "TRUE TRUE");
    rat.appearance.material[7] = 10001;
    rat.appearance.material[8] = 200;
    CHECK(f.p("${Spawn[10].Primary} ${Spawn[10].Secondary}") == "10001 200");
    rat.body_type = 1;
    CHECK(f.p("${Spawn[10].Body} ${Spawn[10].Body.Name} ${Spawn[10].Body.ID}") == "Humanoid Humanoid 1");
    CHECK(f.p("${Spawn[10].Body.Name.NotEqual[Undead]}") == "TRUE");
    // Groupie is in the zone.
    CHECK(f.p("${Group.Present}") == "1");
    // The player's own HP and mana by percent, the others' as their spawn and the group's mana shows them.
    const int my_hp = std::stoi(f.p("${Me.PctHPs}"));
    CHECK(f.p("${Group.Injured[" + std::to_string(my_hp + 1) + "]}") == "1");
    CHECK(f.p("${Group.Injured[" + std::to_string(my_hp) + "]}") == "0");
    const int my_mana = std::stoi(f.p("${Me.PctMana}"));
    A::group(*f.world.conn).mana_percent[12] = 10;
    CHECK(f.p("${Group.LowMana[" + std::to_string(my_mana + 1) + "]}") == (my_mana + 1 > 10 ? "2" : "1"));
    CHECK(f.p("${Group.LowMana[5]}") == "0");
    CHECK(f.p("${Group.Injured}") == "0");
}

TEST_CASE("TLO: Raid and its members") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    // No raid: the TLO is there, with no members.
    CHECK(f.p("${Raid}") == "NULL");
    CHECK(f.p("${Raid.Members} ${Raid.Member[1]} ${Raid.Leader}") == "0 NULL NULL");
    auto& raid = A::raid(*f.world.conn);
    raid.active = true;
    raid.leader = "Groupie";
    raid.members = {mq_test::raid_member("Tester", 0, 2, 50, true), mq_test::raid_member("Groupie", 1, 1, 48),
                    mq_test::raid_member("Faraway", 0xffffffff, 12, 60)};
    raid.loot_type = 3;
    raid.looters = {"Faraway"};
    CHECK(f.p("${Raid.Members} ${Raid.TotalLevels} ${Raid.LootType}") == "3 158 3");
    CHECK(f.p("${Raid.Member[1]} ${Raid.Member[2].Name} ${Raid.Member[Faraway]}") == "Tester Groupie Faraway");
    CHECK(f.p("${Raid.Leader} ${Raid.Leader.RaidLeader} ${Raid.Member[1].RaidLeader}") == "Groupie TRUE FALSE");
    CHECK(f.p("${Raid.Member[1].Group} ${Raid.Member[2].Group} ${Raid.Member[3].Group}") == "1 2 0");
    CHECK(f.p("${Raid.Member[1].GroupLeader} ${Raid.Member[2].Level} ${Raid.Member[3].Class.ShortName}") == "TRUE 48 WIZ");
    // Looters: the leader, and with loot type 3 the named looters.
    CHECK(f.p("${Raid.Member[2].Looter} ${Raid.Member[3].Looter} ${Raid.Member[1].Looter}") == "TRUE TRUE FALSE");
    // A member's other members are its spawn's, when it is in the zone.
    CHECK(f.p("${Raid.Member[2].ID} ${Raid.Member[2].Spawn.ID}") == "12 12");
    CHECK(f.p("${Raid.Member[3].Spawn} ${Raid.Member[3].ID}") == "NULL NULL");
    CHECK(f.p("${Raid.Member[4]} ${Raid.Member[Nobody]}") == "NULL NULL");
}

TEST_CASE("TLO: Corpse, the loot window's corpse and its items") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    CHECK(f.p("${Corpse} ${Corpse.Open}") == "FALSE FALSE");
    CHECK(f.p("${Corpse.Items} ${Corpse.Item[1]}") == "NULL NULL");
    auto& loot = A::loot(*f.world.conn);
    loot.emplace();
    loot->corpse_id = 13; // a_rat02, a corpse
    loot->corpse_name = "a_rat02";
    loot->open = true;
    net::Item fang;
    fang.name = "Rat Fang";
    fang.id = 13069;
    fang.stack_count = 2;
    fang.max_stack = 20;
    fang.tradeskills = 1;
    fang.no_drop = 255;
    loot->items[net::kCorpseSlotBegin] = fang;
    net::Item whisker;
    whisker.name = "Rat Whisker";
    whisker.id = 13070;
    loot->items[net::kCorpseSlotBegin + 1] = whisker;
    CHECK(f.p("${Corpse} ${Corpse.Open} ${Corpse.Items}") == "TRUE TRUE 2");
    CHECK(f.p("${Corpse.Item[1]} ${Corpse.Item[1].ID} ${Corpse.Item[1].Stack} ${Corpse.Item[1].Tradeskills}") ==
          "Rat Fang 13069 2 TRUE");
    CHECK(f.p("${Corpse.Item[2].NoDrop} ${Corpse.Item[1].NoDrop} ${Corpse.Item[3]}") == "TRUE FALSE NULL");
    CHECK(f.p("${Corpse.Item[Whisker]} ${Corpse.Item[=rat fang].ID} ${Corpse.Item[=Rat]}") == "Rat Whisker 13069 NULL");
    CHECK(f.p("${Corpse.Item[1].ItemLink}").find("Rat Fang") != std::string::npos);
    // Its other members are the corpse spawn's.
    CHECK(f.p("${Corpse.ID} ${Corpse.Dead}") == "13 TRUE");
    loot->open = false;
    CHECK(f.p("${Corpse.Open} ${Corpse.Item[1]} ${Corpse.ID}") == "FALSE NULL NULL");
}

// The stacking and stats reads a script asks many times a frame (E3's StacksWith over each of a target's buffs) share
// one sheet (GameConnection::with_cached_character), built again after update(), a packet sent or a local change.
TEST_CASE("TLO: spell stacking and stats reads share one character until the state may have changed") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& c = *f.world.conn;
    const std::uint32_t start = c.character_builds_for_test();
    const std::string first = f.p("${Spell[Holy Armor].WillLand} ${Spell[Holy Armor].StacksWith[Holy Armor]} "
                                  "${Spell[Holy Armor].WillLandPet} ${Spell[Holy Armor].Duration} "
                                  "${Spell[Minor Healing].MyRange} ${Me.SpellShieldBonus} ${Me.SpellDamageBonus}");
    for (int i = 0; i < 5; ++i) CHECK(f.p("${Spell[Holy Armor].StacksWith[Holy Armor]}") == "TRUE");
    CHECK(c.character_builds_for_test() == start + 1);
    // The same answers from a new build.
    c.invalidate_character();
    CHECK(f.p("${Spell[Holy Armor].WillLand} ${Spell[Holy Armor].StacksWith[Holy Armor]} "
              "${Spell[Holy Armor].WillLandPet} ${Spell[Holy Armor].Duration} "
              "${Spell[Minor Healing].MyRange} ${Me.SpellShieldBonus} ${Me.SpellDamageBonus}") == first);
    CHECK(c.character_builds_for_test() == start + 2);
    // A frame, a packet the client sends (an action), a buff the client drops itself: built again.
    c.update();
    f.p("${Spell[Holy Armor].WillLand}");
    CHECK(c.character_builds_for_test() == start + 3);
    c.send_zone_packet(net::AppPacket{});
    f.p("${Spell[Holy Armor].WillLand}");
    CHECK(c.character_builds_for_test() == start + 4);
    c.clear_buff_locally(40); // an empty slot: only that it was dropped locally matters here
    f.p("${Spell[Holy Armor].WillLand}");
    CHECK(c.character_builds_for_test() == start + 5);
    // An item worn changes the stats read after it (the test's accessor stands for the change).
    net::Item robe;
    robe.name = "Shielding Robe";
    robe.id = 7001;
    robe.equip_slots = 1u << net::kSlotChest;
    robe.races = 0xffffffffu;
    robe.classes = 0xffff;
    robe.spell_shield = 12;
    A::inventory(c).put(net::ItemSlot::possessions(net::kSlotChest), robe);
    CHECK(f.p("${Me.SpellShieldBonus}") == "12");
}

TEST_CASE("TLO: Me's Invulnerable, Mount, favour, tribute, spell bonuses, PetBuff; spells' counters") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& sp = A::spells(*f.world.conn);
    CHECK(f.p("${Me.Invulnerable}") == "NULL");
    // Spell 3008 (the fixture's Test Aura) has SPA 40: invulnerable.
    sp.buffs[5].spell = 3008;
    sp.buffs[5].tics = 3;
    sp.buffs[5].received_ms = net::GameConnection::now_ms();
    CHECK(f.p("${Me.Invulnerable} ${Bool[${Me.Invulnerable}]}") == "Test Aura TRUE");
    // Tribute.
    auto& tribute = A::tribute(*f.world.conn);
    tribute.points = 1500;
    tribute.career = 9000;
    CHECK(f.p("${Me.CurrentFavor} ${Me.CareerFavor} ${Me.TributeActive}") == "1500 9000 FALSE");
    tribute.settings.active = true;
    CHECK(f.p("${Me.TributeActive}") == "TRUE");
    // The items' spell shielding and spell damage.
    CHECK(f.p("${Me.SpellShieldBonus} ${Me.SpellDamageBonus}") == "0 0");
    net::Item robe;
    robe.name = "Shielding Robe";
    robe.id = 7001;
    robe.equip_slots = 1u << net::kSlotChest;
    robe.races = 0xffffffffu; // every race: an item the character's race may not use adds nothing
    robe.classes = 0xffff;
    robe.spell_shield = 12;
    robe.spell_dmg = 30;
    A::inventory(*f.world.conn).put(net::ItemSlot::possessions(net::kSlotChest), robe);
    CHECK(f.p("${Me.SpellShieldBonus} ${Me.SpellDamageBonus}") == "12 30");
    // Past RoF2's cap of 35: the items' whole total (a server may allow it; MQ caps it).
    net::Item legs = robe;
    legs.name = "Shielding Leggings";
    legs.id = 7002;
    legs.equip_slots = 1u << net::kSlotLegs;
    legs.spell_shield = 30;
    legs.spell_dmg = 0;
    A::inventory(*f.world.conn).put(net::ItemSlot::possessions(net::kSlotLegs), legs);
    CHECK(f.p("${Me.SpellShieldBonus}") == "42");
    // Mount: the spawn it rides.
    CHECK(f.p("${Me.Mount}") == "NULL");
    A::world(*f.world.conn).spawns[1].mount_id = 11;
    CHECK(f.p("${Me.Mount.ID} ${Bool[${Me.Mount.ID}]}") == "11 TRUE");
    // PetBuff: by slot, or a spell's slot.
    auto& pet = A::pet_buffs(*f.world.conn);
    game::BuffSlot b;
    b.spell = 11; // Holy Armor
    pet.emplace_back(2, b);
    CHECK(f.p("${Me.PetBuff[3]} ${Me.PetBuff[Holy Armor]} ${Me.PetBuff[1]} ${Me.PetBuff[Minor Healing]}") ==
          "Holy Armor 3 NULL NULL");
    // CounterType and CounterNumber: the spell's (and so a buff's) counters.
    CHECK(f.p("${Spell[Test Plague].CounterType} ${Spell[Test Plague].CounterNumber}") == "Disease 4");
    CHECK(f.p("${Spell[Test Venom].CounterType} ${Spell[Test Venom].CounterNumber}") == "Disease 3");
    CHECK(f.p("${Spell[Holy Armor].CounterType} ${Spell[Holy Armor].CounterNumber}") == "None 0");
    sp.buffs[6].spell = 3004;
    sp.buffs[6].tics = 3;
    sp.buffs[6].received_ms = net::GameConnection::now_ms();
    CHECK(f.p("${Me.Buff[Test Plague].CounterType} ${Me.Buff[Test Plague].CounterNumber}") == "Disease 4");
}

TEST_CASE("TLO: Me.Pet and Pet read the pet window's buffs, targeted or not") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    f.world.add(14, "Tester`s_pet", true, 40, game::server_to_world({5, 5, 0}));
    A::world(*f.world.conn).spawns[14].pet_owner_id = 1;
    CHECK(f.p("${Me.Pet} ${Me.Pet.ID} ${Pet.ID} ${Me.Pet.Buff[1]}") == "Tester`s pet 14 14 NULL");
    auto& pet = A::pet_buffs(*f.world.conn);
    game::BuffSlot plague;
    plague.spell = 3004; // Test Plague: 4 disease counters
    plague.tics = 10;
    plague.received_ms = net::GameConnection::now_ms();
    game::BuffSlot armor;
    armor.spell = 11; // Holy Armor
    armor.tics = 20;
    armor.received_ms = net::GameConnection::now_ms();
    pet.emplace_back(2, armor);
    pet.emplace_back(0, plague);
    // Buff[n] is the pet window's slot n; Buff[name] the slot of the first whose name starts so.
    CHECK(f.p("${Me.Pet.Buff[1]} ${Me.Pet.Buff[2]} ${Me.Pet.Buff[3]}") == "Test Plague NULL Holy Armor");
    CHECK(f.p("${Me.Pet.Buff[Holy]} ${Me.Pet.Buff[Test Plague]} ${Me.Pet.Buff[Armor]}") == "3 1 NULL");
    CHECK(f.p("${Me.Pet.Buff[1].CounterType} ${Me.Pet.Buff[1].CounterNumber} ${Me.Pet.Buff[3].ID}") == "Disease 4 3");
    CHECK(f.p("${Pet.Buff[3]} ${Me.Pet.BuffCount}") == "Holy Armor 2");
    const int secs = std::stoi(f.p("${Me.Pet.BuffDuration[Holy Armor].TotalSeconds}"));
    CHECK(secs > 110);
    CHECK(secs <= 120);
    CHECK(f.p("${Me.Pet.BuffDuration[2]}") == "NULL");
    // Targeted, with no target buffs from the server: still the pet window's.
    A::world(*f.world.conn).target_id = 14;
    CHECK(f.p("${Me.Pet.Buff[1]} ${Target.Buff[Test Plague]}") == "Test Plague Test Plague");

    A::set_server_rules(*f.world.conn, plugin::parse_server_cfg("[tlos]\nblock = Pet\n"));
    CHECK(f.p("${Pet.ID} ${Me.Pet.ID} ${Debuff[pet].Count}") == "NULL 14 4");
}

TEST_CASE("TLO: Me.Bank, the bank's coin, and InvSlot") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& inv = A::inventory(*f.world.conn);
    net::Item sword;
    sword.name = "Banked Sword";
    sword.id = 8001;
    inv.put({net::kInvBank, 0, -1, -1, 0, 0}, sword);
    net::Item chest;
    chest.name = "Bank Chest";
    chest.id = 8002;
    chest.item_class = 1;
    chest.bag_slots = 8;
    inv.put({net::kInvBank, 2, -1, -1, 0, 0}, chest);
    net::Item gem;
    gem.name = "Banked Gem";
    gem.id = 8003;
    gem.stack_count = 3;
    inv.put({net::kInvBank, 2, 4, -1, 0, 0}, gem);
    net::Item shared;
    shared.name = "Shared Ring";
    shared.id = 8004;
    inv.put({net::kInvSharedBank, 1, -1, -1, 0, 0}, shared);
    // Bank[n]: the bank's slot n; past the 24 bank slots, the shared bank's.
    CHECK(f.p("${Me.Bank[1]} ${Me.Bank[1].ID} ${Me.Bank[2]}") == "Banked Sword 8001 NULL");
    CHECK(f.p("${Me.Bank[3]} ${Me.Bank[3].Container} ${Me.Bank[3].Item[5]} ${Me.Bank[3].Item[5].Stack}") ==
          "Bank Chest 8 Banked Gem 3");
    CHECK(f.p("${Me.Bank[26]} ${Me.SharedBank[2].ID} ${Me.Bank[0]} ${Me.Bank[20]}") == "Shared Ring 8004 NULL NULL");
    CHECK(f.p("${Me.Bank[1].ItemLink}").find("Banked Sword") != std::string::npos);
    // The bank's coin.
    auto& coins = A::bank_coins(*f.world.conn);
    coins.platinum = 1234;
    coins.gold = 5;
    coins.silver = 6;
    coins.copper = 7;
    coins.shared_platinum = 50;
    CHECK(f.p("${Me.PlatinumBank} ${Me.GoldBank} ${Me.SilverBank} ${Me.CopperBank} ${Me.PlatinumShared}") ==
          "1234 5 6 7 50");
    CHECK(f.p("${Me.CashBank}") == "1234567");
    // InvSlot: by MQ's number or name; .Item the item there.
    CHECK(f.p("${InvSlot[pack1]} ${InvSlot[pack1].Item} ${InvSlot[23].Item.ID}") == "23 Healing Potion 5001");
    CHECK(f.p("${InvSlot[pack2].Item.Items} ${InvSlot[pack2].Item.Container}") == "1 4");
    CHECK(f.p("${InvSlot[272].Item} ${InvSlot[272].Pack} ${InvSlot[272].Slot}") == "Healing Potion 24 0"); // pack2, slot 1
    CHECK(f.p("${InvSlot[bank1].Item} ${InvSlot[2000].Name} ${InvSlot[2002].Item}") == "Banked Sword bank1 Bank Chest");
    CHECK(f.p("${InvSlot[2056].Item} ${InvSlot[2056].Pack} ${InvSlot[2056].Slot}") == "Banked Gem 2002 4");
    CHECK(f.p("${InvSlot[sharedbank2].Item} ${InvSlot[2501].Name}") == "Shared Ring sharedbank2");
    CHECK(f.p("${InvSlot[chest].Name} ${InvSlot[chest].Item} ${InvSlot[nowhere]}") == "chest NULL 0");
}

TEST_CASE("TLO: Merchant, its stock and ItemsReceived") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    CHECK(f.p("${Merchant} ${Merchant.Open} ${Merchant.ItemsReceived} ${Merchant.Items}") == "FALSE FALSE FALSE NULL");
    auto& m = A::merchant(*f.world.conn);
    m.emplace();
    m->npc_id = 11;
    m->open = true;
    m->rate = 1.25f;
    // Open, its stock not come yet.
    CHECK(f.p("${Merchant} ${Merchant.ItemsReceived} ${Merchant.Items}") == "TRUE FALSE 0");
    net::Item bread;
    bread.name = "Loaf of Bread";
    bread.id = 13010;
    m->items[3] = bread;
    net::Item water;
    water.name = "Water Flask";
    water.id = 13006;
    m->items[7] = water;
    CHECK(f.p("${Merchant.ItemsReceived} ${Merchant.Items} ${Merchant.Markup}") == "TRUE 2 1.25");
    CHECK(f.p("${Merchant.Item[1]} ${Merchant.Item[2].ID} ${Merchant.Item[3]}") == "Loaf of Bread 13006 NULL");
    CHECK(f.p("${Merchant.Item[Flask]} ${Merchant.Item[=loaf of bread].ID}") == "Water Flask 13010");
    // Its other members are the merchant's spawn's.
    CHECK(f.p("${Merchant.ID} ${Merchant.CleanName}") == "11 Fippy Darkpaw");
    m->open = false;
    CHECK(f.p("${Merchant.Open} ${Merchant.ItemsReceived} ${Merchant.Item[1]}") == "FALSE FALSE NULL");
}

TEST_CASE("TLO: Window.Open from the connection's state, with no UI") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& c = *f.world.conn;
    // The fixture's UI shows InventoryWindow alone: these windows answer from the connection, not from it.
    CHECK(f.p("${Window[MerchantWnd].Open} ${Window[BigBankWnd]} ${Window[LootWnd].Open} ${Window[TradeWnd].Open}") ==
          "FALSE FALSE FALSE FALSE");
    CHECK(f.p("${Window[SpellBookWnd].Open} ${Window[CastingWindow].Open} ${Window[ConfirmationDialogBox].Open}") ==
          "FALSE FALSE FALSE");
    A::merchant(c).emplace().open = true;
    A::bank(c).emplace().slots = 24;
    A::spell_book_open(c) = true;
    CHECK(f.p("${Window[merchantwnd].Open} ${Window[BigBankWnd].Open} ${Window[BankWnd].Open} ${Window[SpellBookWnd]}") ==
          "TRUE TRUE FALSE TRUE");
    A::bank(c)->slots = 16; // CBankWnd from BankWnd for a 16-slot bank
    CHECK(f.p("${Window[BigBankWnd].Open} ${Window[BankWnd].Open}") == "FALSE TRUE");
    auto& t = A::trade(c).emplace();
    t.open = true;
    t.with_npc = true;
    CHECK(f.p("${Window[GiveWnd].Open} ${Window[TradeWnd].Open}") == "TRUE FALSE");
    t.with_npc = false;
    CHECK(f.p("${Window[GiveWnd].Open} ${Window[TradeWnd].Open}") == "FALSE TRUE");
    // A window the connection knows nothing of asks the UI.
    CHECK(f.p("${Window[InventoryWindow].Open} ${Window[FriendsWindow].Open}") == "TRUE FALSE");
}

TEST_CASE("TLO: /doortarget and DoorTarget") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    auto& doors = A::doors(*f.world.conn);
    game::Door near;
    near.id = 3;
    near.name = "POKCABDOOR";
    near.x = 10; // the server's x and y, as a spawn's X and Y
    near.y = 0;
    near.z = 1;
    game::Door far;
    far.id = 7;
    far.name = "POKCABDOOR2";
    far.x = 0;
    far.y = 40;
    doors = {near, far};
    plugin::Hooks hooks;
    std::vector<std::string> said;
    mq::CommandEnv ce;
    ce.hooks = &hooks;
    ce.registry = &f.reg;
    ce.context = [&f]() -> const mq::Context& { return f.ctx; };
    ce.echo = [&said](const std::string& s) { said.push_back(s); };
    ce.set_door_target = [&f](std::optional<std::uint8_t> id) { f.world.conn->set_door_target(id); };
    mq::Commands commands(ce);
    auto run = [&](std::string line) { hooks.command(line); };
    CHECK(f.p("${DoorTarget}") == "NULL");
    // By name: the nearest whose name holds the text.
    run("/doortarget cabdoor");
    CHECK(said.back() == "Switch 3 'POKCABDOOR' targeted.");
    CHECK(f.p("${DoorTarget} ${DoorTarget.ID} ${DoorTarget.Name} ${DoorTarget.IsTargeted}") == "3 3 POKCABDOOR TRUE");
    CHECK(f.p("${DoorTarget.X} ${DoorTarget.Y} ${DoorTarget.Z}") == "10.00 0.00 1.00");
    // On the flat, less half its size (scale 1.0); in 3D the height too.
    CHECK(f.p("${DoorTarget.Distance}") == "9.50");
    CHECK(f.p("${DoorTarget.Distance3D}").substr(0, 4) == "9.55");
    run("/doortarget id 7");
    CHECK(f.p("${DoorTarget.ID} ${DoorTarget.Distance}") == "7 39.50");
    run("/doortarget id");
    CHECK(said.back() == "[MQ] DoorTarget: id specified but no number provided.");
    CHECK(f.p("${DoorTarget}") == "NULL");
    run("/doortarget nosuchdoor");
    CHECK(said.back() == "[MQ] Couldn't find switch 'nosuchdoor' to target.");
    run("/doortarget");
    CHECK(f.p("${DoorTarget}") == "3"); // no text: the nearest door
    run("/doortarget clear");
    CHECK(said.back() == "Switch target cleared.");
    CHECK(f.p("${DoorTarget}") == "NULL");
    // The zone's doors going take the target with them.
    run("/doortarget id 3");
    doors.clear();
    CHECK(f.p("${DoorTarget}") == "NULL");
}

TEST_CASE("TLO: Me.TargetOfTarget and Instance, EverQuest.Foreground and Ping, Mercenary") {
    Fixture f;
    using A = net::GameConnectionTestAccess;
    CHECK(f.p("${Me.TargetOfTarget}") == "NULL");
    A::target_of_target(*f.world.conn) = 12;
    CHECK(f.p("${Me.TargetOfTarget} ${Me.TargetOfTarget.ID}") == "Groupie 12");
    CHECK(f.p("${Me.Instance}") == "0");
    // No window to have the focus in the test; the app answers with its window's.
    CHECK(f.p("${EverQuest.Foreground}") == "FALSE");
    bool focused = true;
    f.ctx.foreground = [&focused] { return focused; };
    CHECK(f.p("${EverQuest.Foreground}") == "TRUE");
    CHECK(f.p("${EverQuest.Ping}") == "0"); // no session: no round trips yet
    // Mercenary: NONE without one; its other members its spawn's.
    CHECK(f.p("${Mercenary} ${Mercenary.State} ${Mercenary.Name}") == "NONE NONE NONE");
    f.world.add(15, "Tanker", true, 50, game::server_to_world({8, 8, 0}));
    A::world(*f.world.conn).spawns[15].mercenary = true;
    auto& grp = A::group(*f.world.conn);
    grp.slots[2].name = "Tanker";
    grp.slots[2].merc = true;
    grp.slots[2].owner_name = "Tester";
    auto& merc = A::merc(*f.world.conn);
    merc.has_mercenary = 1;
    merc.state = game::kMercActive;
    merc.current_index = 0;
    CHECK(f.p("${Mercenary} ${Mercenary.State} ${Mercenary.StateID} ${Mercenary.Index}") == "ACTIVE ACTIVE 5 1");
    CHECK(f.p("${Mercenary.Name} ${Mercenary.CleanName} ${Mercenary.ID}") == "Tanker Tanker 15");
    merc.state = game::kMercSuspended;
    CHECK(f.p("${Mercenary.State}") == "SUSPENDED");
}
