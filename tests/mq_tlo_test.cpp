// mq/tlo.h, mq/spawn_search.h and mq/parse.h: MacroQuest's TLOs over a small world (tests/mq_test_world.h).
#include <doctest/doctest.h>

#include "manual_clock.h"
#include "mq/parse.h"
#include "mq/spawn_search.h"
#include "mq/tlo.h"
#include "mq_test_world.h"

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
    CHECK(f.p("${Me.Inventory[pack2].Container}") == "4");
    CHECK(f.p("${Me.Inventory[pack2].Item[1]}") == "Healing Potion");
    CHECK(f.p("${Me.FreeInventory}") == "11"); // 8 empty general slots + 3 in the bag
    CHECK(f.p("${Cursor}") == "NULL");
    CHECK(f.p("${Window[InventoryWindow].Open} ${Window[BankWnd].Open}") == "TRUE FALSE");
    CHECK(f.p("${EverQuest.GameState} ${EverQuest.Server}") == "INGAME TestServer");
    CHECK(f.p("${Int[7.9]} ${Float[2].Deci} ${Bool[0]} ${Bool[yes]}") == "7 2.0 FALSE TRUE");
    // No whole number for NaN, an infinity or one past int64 (the cast would be undefined); in range is cut as before.
    CHECK(f.p("${Int[nan]} ${Int[inf]} ${Int[-inf]} ${Int[1e300]} ${Int[-1e300]}") == "NULL NULL NULL NULL NULL");
    CHECK(f.p("${Float[nan].Int} ${Float[inf].Int} ${Float[1e300].Int}") == "NULL NULL NULL");
    CHECK(f.p("${Int[-7.9]} ${Float[1e18].Int} ${Float[-2.5].Int}") == "-7 1000000000000000000 -2");
    CHECK(f.p("${Select[b,a,b,c]}") == "2");
    CHECK(f.p("${Heading[90].ShortName}") == "E");
    CHECK(f.p("${Heading[0].ShortName}") == "N");
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
