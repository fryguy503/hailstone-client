// mq/lua_mq.h and mq/commands.h: MacroQuest Lua scripts over the test world (tests/mq_test_world.h), held to the
// server's rules.
#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <thread>

#include <imgui.h>

#include "core/ini_file.h"
#include "core/log.h"
#include "core/sha256.h"
#include "mq/commands.h"
#include "mq/lua_mq.h"
#include "mq/lua_script.h" // ScriptRun, HostWork (the host's insides)
#include "mq_test_world.h"
#include "scratch_dir.h"
#include "app/web_service.h"
#include "plugin/web_access.h"
#include "web/browser_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Host {
    mq_test::World world{"hailstone_mq_lua_test"};
    fs::path lua_dir = world.dir / "lua";
    fs::path config_dir = world.dir / "config";
    plugin::Hooks hooks;
    plugin::Policy policy;
    mq::Registry reg;
    mq::Context ctx;
    std::vector<std::string> printed, sent;
    std::uint32_t target = 0;
    std::int64_t clock = 0;
    std::unique_ptr<mq::Commands> commands;
    std::unique_ptr<mq::ScriptHost> scripts;

    explicit Host(std::uint32_t unlisted_caps = plugin::kCapAll, plugin::WebAccess* web = nullptr,
                  std::int64_t runaway_ms = 0) {
        fs::create_directories(lua_dir);
        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        if (unlisted_caps) allow_unlisted(unlisted_caps);
        mq::CommandEnv ce;
        ce.hooks = &hooks;
        ce.registry = &reg;
        ce.context = [this]() -> const mq::Context& { return ctx; };
        ce.echo = [this](const std::string& t) { printed.push_back(t); };
        ce.run = [this](const std::string& l) { run(l); };
        ce.set_target = [this](std::uint32_t id) { target = id; };
        commands = std::make_unique<mq::Commands>(ce);
        mq::ScriptEnv se;
        se.hooks = &hooks;
        se.policy = &policy;
        se.registry = &reg;
        se.context = ce.context;
        se.print = ce.echo;
        se.run = ce.run;
        se.dirs = {lua_dir};
        se.config_dir = config_dir;
        se.now_ms = [this] { return clock; };
        se.web = web;
        se.runaway_ms = runaway_ms;
        scripts = std::make_unique<mq::ScriptHost>(se);
    }
    ~Host() {
        scripts.reset();
        commands.reset();
    }

    // A hailstone.cfg letting the player's own scripts do `caps`, with no Lua socials.
    void allow_unlisted(std::uint32_t caps) {
        plugin::Rules r;
        r.configured = true;
        r.player_script_caps = caps;
        r.social_mode = plugin::ButtonMode::Off;
        policy.set(r);
    }
    // A hailstone.cfg whose server sent these scripts (in lua_dir), the player's not allowed.
    plugin::Rules server_scripts(const std::vector<std::string>& files) {
        plugin::Rules r;
        r.configured = true;
        r.social_mode = plugin::ButtonMode::Off;
        for (const auto& f : files)
            r.server_files.push_back({plugin::FileKind::Script, "scripts/" + f, *parse_sha256(*mq::script_file_hash(lua_dir / f))});
        return r;
    }
    // GameConnection::send_command: the hooks first, then the server.
    void run(const std::string& line) {
        std::string l = line;
        if (hooks.command(l)) return;
        sent.push_back(l);
    }
    void write(const std::string& file, const std::string& text) {
        fs::create_directories((lua_dir / file).parent_path());
        std::ofstream(lua_dir / file, std::ios::binary) << text;
    }
    bool printed_line(const std::string& text) const {
        return std::find(printed.begin(), printed.end(), text) != printed.end();
    }
    void pulses(int n, std::int64_t step_ms = 0) {
        for (int i = 0; i < n; ++i) {
            clock += step_ms;
            scripts->pulse();
        }
    }
};

} // namespace

TEST_CASE("MQ Lua: spell data exposes raw display fields") {
    Host h;
    h.write("spell_fields.lua", R"(
local hs = require('hailstone')
local s = assert(hs.spell(3001))
assert(s.id == 3001 and s.name == 'Test Mesmerize')
assert(s.range == 0 and s.ae_range == 0 and s.push_back == 0)
assert(s.duration_cap == 8 and s.duration_formula == 0)
assert(s.spell_type == 0 and s.resist == 0 and s.resist_adj == 0)
assert(s.spell_icon == 0 and s.category == 0 and s.subcategory == 0)
assert(s.cast_on_you == '' and s.cast_on_other == '' and s.wear_off == '')
assert(#s.effects == 12 and s.effects[1].effect == 31 and s.effects[1].base == 1)
assert(s.effects[1].base2 == 0 and s.effects[1].max == 0 and s.effects[1].formula == 0)
assert(hs.spell(45000) == nil)
print('spell fields ok')
)");
    REQUIRE(h.scripts->run("spell_fields", {}));
    h.pulses(1);
    CHECK(h.printed_line("spell fields ok"));
}

TEST_CASE("MQ Lua: hailstone.raid is a columnar roster") {
    Host h;
    using A = net::GameConnectionTestAccess;
    auto& raid = A::raid(*h.world.conn);
    raid.active = true;
    raid.leader = "Groupie";
    raid.locked = true;
    raid.loot_type = 3;
    raid.looters = {"Tester"};
    raid.target = "Tester";
    raid.members = {
        mq_test::raid_member("Groupie", 0, 2, 48, true, true),
        mq_test::raid_member("Tester", 0, 2, 50),
        mq_test::raid_member("Third", 1, 1, 45, false, false, true),
    };
    auto& leadership = A::leadership(*h.world.conn);
    leadership.raid_assist_names[0] = "Groupie";
    leadership.raid_assist_names[1] = "Third";
    h.write("raid.lua", R"(
local hs = require('hailstone')
local r = assert(hs.raid())
print('raid', r.active, r.count, r.leader, r.target, r.locked, r.loot_type)
print('m1', r.name[1], r.spawn_id[1], r.group[1], r.flags[1], r.pct_hp[1])
print('m2', r.name[2], r.spawn_id[2], r.flags[2], r.pct_hp[2], r.pct_mana[2], r.pct_endurance[2])
print('m3', r.name[3], r.spawn_id[3], r.group[3], r.flags[3], r.pct_hp[3])
print('flags', hs.RAID_LEADER, hs.RAID_MAIN_ASSIST, hs.RAID_LOOTER, hs.RAID_ME, r.slot == nil)
)");
    REQUIRE(h.scripts->run("raid", {}));
    h.pulses(1);
    CHECK(h.printed_line("raid true 3 1 2 true 3"));
    CHECK(h.printed_line("m1 Groupie 12 0 167 100"));
    CHECK(h.printed_line("m2 Tester 1 416 75 25 50"));
    CHECK(h.printed_line("m3 Third 0 1 12 -1"));
    CHECK(h.printed_line("flags 1 4 32 256 true"));
}

TEST_CASE("MQ Lua: hailstone.raid outside the game and without read") {
    Host h;
    h.ctx.conn = nullptr;
    h.write("raid_nil.lua", "print('nil', require('hailstone').raid() == nil)\n");
    REQUIRE(h.scripts->run("raid_nil", {}));
    h.pulses(1);
    CHECK(h.printed_line("nil true"));

    Host r(plugin::kCapCommands);
    r.write("raid_denied.lua", "print(pcall(require('hailstone').raid))\n");
    REQUIRE(r.scripts->run("raid_denied", {}));
    r.pulses(1);
    bool denied = false;
    for (const auto& line : r.printed)
        denied |= line.find("hailstone.raid needs the server's 'read' permission") != std::string::npos;
    CHECK(denied);
}

TEST_CASE("MQ Lua: mq.TLO, print, mq.cmd, mq.parse and the script's arguments") {
    Host h;
    h.write("hello.lua", R"(
local mq = require('mq')
print('hp', mq.TLO.Me.PctHPs())
print(mq.TLO.Target.Name(), mq.TLO.Target.Distance())
print(mq.TLO.Me.Buff(1)(), mq.TLO.Me.Buff('Holy Armor').ID())
print('name ' .. mq.TLO.Me.Name)
print(mq.TLO.Me.PctHPs() > 50, mq.TLO.Me.PctHPs > 80, mq.TLO.Nope.Deeper())
print(tostring(mq.TLO.Me.Class), mq.TLO.Me.Class.ShortName())
print(#mq.getAllSpawns(), mq.TLO.SpawnCount('npc')())
print(mq.TLO.Spawn('npc fippy').CleanName(), mq.TLO.Group.Member(1).Name())
mq.cmd('/sit')
mq.cmdf('/tell %s hi %d', 'Groupie', 5)
mq.cmd.echo('hello', 'world')
print(mq.parse('${Me.Level} ${Me.Gem[1]}'))
print(...)
printf('%s-%d', 'f', 3)
)");
    REQUIRE(h.scripts->run("hello", {"a1", "b2"}));
    CHECK(h.scripts->running("hello"));
    h.pulses(1);
    CHECK(h.printed_line("hp 75"));
    CHECK(h.printed_line("a_rat01 50.0"));
    CHECK(h.printed_line("Holy Armor 1"));
    CHECK(h.printed_line("name Tester"));
    CHECK(h.printed_line("true false nil"));
    CHECK(h.printed_line("Cleric CLR"));
    CHECK(h.printed_line("5 2"));
    CHECK(h.printed_line("Fippy Darkpaw Groupie"));
    CHECK(h.printed_line("hello world"));
    CHECK(h.printed_line("50 Minor Healing"));
    CHECK(h.printed_line("a1 b2"));
    CHECK(h.printed_line("f-3"));
    CHECK(h.sent == std::vector<std::string>{"/sit", "/tell Groupie hi 5"});
    CHECK_FALSE(h.scripts->running("hello")); // the chunk returned
    CHECK(h.printed_line("[Lua] hello ended"));
}

TEST_CASE("MQ Lua: mq.delay waits on the clock or a condition; binds and events run") {
    Host h;
    h.write("loop/init.lua", R"(
local mq = require('mq')
local util = require('util')
local n = 0
mq.bind('/bump', function(a) n = n + tonumber(a) end)
mq.event('slain', '#*#You have slain #1#!#*#', function(line, who) print('killed', who) end)
mq.delay('1s')
print('after delay')
mq.delay(10000, function() return n >= 3 end)
print('bumped', n, util.twice(n))
while true do
    mq.doevents()
    mq.delay(100)
end
)");
    h.write("loop/util.lua", "local M = {} function M.twice(x) return x * 2 end return M\n");
    REQUIRE(h.scripts->run("loop", {}));
    h.pulses(1); // starts at 0: the delay ends at 1000
    h.pulses(3, 300);
    CHECK_FALSE(h.printed_line("after delay"));
    h.pulses(1, 200);
    CHECK(h.printed_line("after delay"));
    h.run("/bump 1");
    h.pulses(2, 100);
    CHECK_FALSE(h.printed_line("bumped 1 2"));
    h.run("/BUMP 2");
    h.pulses(1, 100);
    CHECK(h.printed_line("bumped 3 6"));
    plugin::ChatLine line{"You have slain a rat!", 0, 0};
    h.hooks.chat(line);
    plugin::ChatLine other{"Nothing here", 0, 0};
    h.hooks.chat(other);
    h.pulses(2, 100);
    CHECK(h.printed_line("killed a rat"));
    CHECK(h.scripts->running("loop"));
    h.run("/lua stop loop");
    CHECK_FALSE(h.scripts->running("loop"));
    CHECK_FALSE(h.hooks.has_command("/bump")); // the script's bind went with it
}

TEST_CASE("MQ Lua: nothing before the zone-in, anything without a hailstone.cfg, only the server's under one") {
    Host h(0);
    h.write("a.lua", "while true do require('mq').delay(10) end\n");
    h.write("b.lua", "print(1)\n");
    std::string why;
    CHECK_FALSE(h.scripts->run("a", {}, &why));
    CHECK(why.find("until the zone-in") != std::string::npos);
    // No hailstone.cfg: no rules.
    h.policy.set(plugin::Rules{});
    REQUIRE(h.scripts->run("a", {}));
    CHECK(h.scripts->list().front().caps == plugin::kCapAll);
    h.scripts->stop("a");
    h.pulses(1);
    // Under a hailstone.cfg the server's own run, the player's do not.
    h.policy.set(h.server_scripts({"a.lua"}));
    h.scripts->policy_changed();
    REQUIRE(h.scripts->run("a", {}));
    CHECK(h.scripts->list().front().caps == plugin::kCapAll);
    h.pulses(1);
    CHECK_FALSE(h.scripts->run("b", {}, &why));
    CHECK(why == "b may not run: this server runs only its own scripts (its hailstone.cfg)");
    // The player cannot stop or pause the server's.
    CHECK(h.scripts->stop("a"));
    h.run("/lua stop");
    h.run("/lua pause a");
    h.pulses(1);
    CHECK(h.scripts->running("a"));
    CHECK(h.printed_line("[Lua] a is this server's own script: it keeps running"));
    // The rules no longer having it stop it.
    h.policy.set(h.server_scripts({}));
    h.scripts->policy_changed();
    CHECK_FALSE(h.scripts->running("a"));
}

TEST_CASE("MQ Lua: a module must be allowed too") {
    Host h(0);
    h.write("main.lua", "local m = require('helper') print('got', m)\n");
    h.write("helper.lua", "return 42\n");
    h.policy.set(h.server_scripts({"main.lua"}));
    REQUIRE(h.scripts->run("main", {}));
    h.pulses(1);
    CHECK_FALSE(h.printed_line("got 42"));
    bool refused = false;
    for (const auto& l : h.printed) refused |= l.find("module 'helper'") != std::string::npos &&
                                              l.find("may not run") != std::string::npos;
    CHECK(refused);
    h.policy.set(h.server_scripts({"main.lua", "helper.lua"}));
    REQUIRE(h.scripts->run("main", {}));
    h.pulses(1);
    CHECK(h.printed_line("got 42"));
}

TEST_CASE("MQ Lua: required managed scripts start without autorun and cannot be stopped or paused") {
    Host h;
    const std::string text = "while true do require('mq').delay(10) end\n";
    h.write("needed/init.lua", text);
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(text), plugin::kCapRead, "needed", "owner/needed"}});
    h.policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/needed\n"));
    h.scripts->policy_changed();
    REQUIRE(h.scripts->running("needed"));
    std::string problem;
    CHECK_FALSE(h.scripts->required_addons_ready(&problem)); // the entry has not run yet
    CHECK(problem.empty());
    h.pulses(1);
    CHECK(h.scripts->required_addons_ready(&problem));
    CHECK(h.scripts->list().front().kept);
    CHECK(h.scripts->stop("needed"));
    CHECK(h.scripts->pause("needed", true));
    h.run("/lua stop");
    h.run("/lua pause");
    CHECK(h.scripts->running("needed"));
    CHECK(h.scripts->list().front().status == "RUNNING");
    CHECK(h.printed_line("[Lua] needed is required by this server: it keeps running"));
    h.policy.clear();
    h.scripts->policy_changed();
    CHECK_FALSE(h.scripts->running("needed"));
}

TEST_CASE("MQ Lua: required script startup failures prevent readiness") {
    Host h;
    const std::string text = "error('required startup failed')\n";
    h.write("needed/init.lua", text);
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(text), plugin::kCapRead, "needed", "owner/needed"}});
    h.policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/needed\n"));
    h.scripts->policy_changed();
    h.pulses(1);
    std::string problem;
    CHECK_FALSE(h.scripts->required_addons_ready(&problem));
    // Named by its repository, as the server's policy names it, not by its folder.
    CHECK(problem.find("Required add-on owner/needed") != std::string::npos);
}

TEST_CASE("MQ Lua: Retry starts a required script that failed again, once, and only its recorded bytes") {
    Host h;
    const std::string text = "error('required startup failed')\n";
    h.write("needed/init.lua", text);
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(text), plugin::kCapRead, "needed", "owner/needed"}});
    h.policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/needed\n"));
    h.scripts->policy_changed();
    h.pulses(1);
    REQUIRE_FALSE(h.scripts->running("needed"));
    CHECK(h.scripts->retry_required_addons() == 1);
    CHECK(h.scripts->running("needed")); // started again (it fails again at its first pulse)
    CHECK(h.scripts->retry_required_addons() == 0); // running: left alone
    h.pulses(1);
    CHECK_FALSE(h.scripts->running("needed"));
    h.write("needed/init.lua", "-- other bytes\n"); // not what the requirement names: not started
    CHECK(h.scripts->retry_required_addons() == 0);
    CHECK_FALSE(h.scripts->running("needed"));
    h.policy.clear(); // no rules: nothing is required
    CHECK(h.scripts->retry_required_addons() == 0);
}

TEST_CASE("MQ Lua: a required hash also supplied by a blocked add-on refuses readiness") {
    Host h;
    const std::string text = "while true do require('mq').delay(10) end\n";
    h.write("needed/init.lua", text);
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(text), plugin::kCapRead, "needed", "owner/needed"},
                          {plugin::FileKind::Script, sha256(text), plugin::kCapRead, "blocked", "owner/blocked"}});
    h.policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/needed\nblock = owner/blocked\n"));
    h.scripts->policy_changed();
    CHECK_FALSE(h.scripts->running("needed"));
    std::string problem;
    CHECK_FALSE(h.scripts->required_addons_ready(&problem));
    CHECK(problem.find("not permitted") != std::string::npos);
}

TEST_CASE("MQ Lua: missing or shadowed required entry points cannot disappear from readiness checks") {
    Host h;
    const std::string text = "while true do require('mq').delay(10) end\n";
    h.write("needed/init.lua", text);
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(text), plugin::kCapRead, "needed", "owner/needed"}});
    auto rules = plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/needed\n");
    REQUIRE(rules.addons.error.empty());
    SUBCASE("the managed entry is missing") {
        fs::remove(h.lua_dir / "needed/init.lua");
    }
    SUBCASE("a different server script shadows the managed entry") {
        const fs::path server = h.world.dir / "server/scripts";
        fs::create_directories(server);
        const std::string shadow = "while true do require('mq').delay(20) end\n";
        std::ofstream(server / "needed.lua", std::ios::binary) << shadow;
        h.scripts->set_server_dir(server);
        rules.server_files.push_back({plugin::FileKind::Script, "scripts/needed.lua", sha256(shadow)});
    }
    h.policy.set(rules);
    h.scripts->policy_changed();
    CHECK_FALSE(h.scripts->running("needed"));
    std::string problem;
    CHECK_FALSE(h.scripts->required_addons_ready(&problem));
    CHECK_FALSE(problem.empty());
}

TEST_CASE("MQ Lua: blocks apply to an installed add-on's entry, its paths and already-loaded modules") {
    Host h;
    const std::string entry = "print('blocked entry')\n", helper = "return {value = 42}\n";
    h.write("blocked/init.lua", entry);
    h.write("blocked/helper.lua", helper);
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(entry), plugin::kCapAll, "blocked", "owner/blocked"},
                          {plugin::FileKind::Script, sha256(helper), plugin::kCapAll, "blocked", "owner/blocked"}});
    h.write("main.lua", "local helper = require('blocked.helper') while true do require('mq').delay(10) end\n");
    auto rules = plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = allow\n");
    h.policy.set(rules);
    REQUIRE(h.scripts->run("main", {}));
    h.pulses(1);
    REQUIRE(h.scripts->running("main"));
    rules.addons.block = {"owner/blocked"};
    h.policy.set(rules);
    std::string problem;
    CHECK_FALSE(h.scripts->required_addons_ready(&problem)); // before policy_changed retires the loaded module
    CHECK(problem.find("module") != std::string::npos);
    h.scripts->policy_changed();
    CHECK_FALSE(h.scripts->running("main"));
    CHECK(h.scripts->required_addons_ready());
    CHECK_FALSE(h.scripts->run("blocked", {}));
    CHECK_FALSE(h.scripts->run("blocked/init", {}));
    CHECK_FALSE(h.scripts->run("blocked/init.lua", {}));
    CHECK_FALSE(h.scripts->run("./blocked/init.lua", {}));
    CHECK_FALSE(h.scripts->run("blocked/./init.lua", {}));
    CHECK_FALSE(h.scripts->run("blocked\\init.lua", {}));
    REQUIRE(h.scripts->run("main", {}));
    h.pulses(1);
    CHECK_FALSE(h.scripts->running("main")); // require cannot reopen the blocked module
}

TEST_CASE("MQ Lua: a script is found whatever the case of its name, on any file system") {
    Host h;
    h.write("Relative/init.lua", "print('relative entry')\n");
    REQUIRE(h.scripts->run("./Relative", {}));
    h.pulses(1);
    CHECK(h.printed_line("relative entry"));
    h.write("AutoLoot/InIt.LuA", "print('mixed entry')\n");
    REQUIRE(h.scripts->run("autoloot", {}));
    h.pulses(1);
    CHECK(h.printed_line("mixed entry"));
}

TEST_CASE("MQ Lua: a required script whose folder has capitals starts and is ready") {
    Host h;
    const std::string code = "while true do require('mq').delay(10) end\n";
    h.write("MiXeD/InIt.LuA", code);
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(code), plugin::kCapRead, "MiXeD", "owner/mixed"}});
    h.policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/mixed\n"));
    h.scripts->policy_changed();
    REQUIRE(h.scripts->running("mixed"));
    h.pulses(1);
    CHECK(h.scripts->required_addons_ready());
}

TEST_CASE("MQ Lua: loaded module rechecks are confined to opted-in add-on rules") {
    Host h(plugin::kCapRead);
    const std::string helper = "return {}\n";
    h.write("helper.lua", helper);
    h.write("main.lua", "require('helper') while true do require('mq').delay(10) end\n");
    h.policy.set_ceilings({{plugin::FileKind::Script, sha256(helper), plugin::kCapRead, "helper", "owner/helper"}});
    REQUIRE(h.scripts->run("main", {}));
    h.pulses(1);
    h.allow_unlisted(plugin::kCapAll);
    h.scripts->policy_changed();
    CHECK(h.scripts->running("main"));
    CHECK(h.scripts->caps_of("main") == plugin::kCapAll);
    h.policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = allow\n"));
    h.scripts->policy_changed();
    CHECK_FALSE(h.scripts->running("main"));
}

TEST_CASE("MQ Lua: a module whose file may do less than the requiring script is refused") {
    Host h; // the player's scripts may do everything
    // An add-on named "mq" (read, ui) ships mq/Icons.lua; a full script's require('mq.Icons') must not run it.
    h.write("mq/Icons.lua", "print('payload ran') return {}\n");
    h.write("lib/Write.lua", "return {w = 7}\n"); // a library the player installed: the player's file
    h.write("main.lua", R"(
print(pcall(require, 'mq.Icons'))
print('lib', require('lib.Write').w)
)");
    // The add-on's own script, with its own module: both under its ceiling; the player's library may do more.
    h.write("addon/init.lua", "print('addon', require('util').x, require('lib.Write').w)\n");
    h.write("addon/util.lua", "return {x = 1}\n");
    auto hash = [&](const std::string& f) { return *parse_sha256(*mq::script_file_hash(h.lua_dir / f)); };
    const std::uint32_t addon = plugin::kCapRead | plugin::kCapUi;
    h.policy.set_ceilings({{plugin::FileKind::Script, hash("mq/Icons.lua"), addon},
                           {plugin::FileKind::Script, hash("addon/init.lua"), addon},
                           {plugin::FileKind::Script, hash("addon/util.lua"), addon}});
    REQUIRE(h.scripts->run("main", {}));
    REQUIRE(h.scripts->run("addon", {}));
    h.pulses(1);
    for (const auto& l : h.printed) CHECK(l.find("payload ran") == std::string::npos);
    bool refused = false;
    for (const auto& l : h.printed)
        refused |= l.rfind("false", 0) == 0 && l.find("module 'mq.Icons'") != std::string::npos &&
                   l.find("may not run in main") != std::string::npos;
    CHECK(refused);
    CHECK(h.printed_line("lib 7"));
    CHECK(h.printed_line("addon 1 7"));
}

TEST_CASE("MQ Lua: the server's scripts folder comes first; its hailstone.cfg's [scripts] run starts them") {
    Host h(0);
    fs::path server = h.world.dir / "server" / "scripts";
    fs::create_directories(server);
    std::ofstream(server / "hello.lua", std::ios::binary) << "print('server hello')\n";
    h.write("hello.lua", "print('player hello')\n");
    h.scripts->set_server_dir(server);
    plugin::Rules r = plugin::parse_server_cfg("[scripts]\nrun = hello\n");
    r.server_files.push_back({plugin::FileKind::Script, "scripts/hello.lua", *parse_sha256(*mq::script_file_hash(server / "hello.lua"))});
    h.policy.set(r);
    h.scripts->policy_changed();
    h.pulses(1);
    CHECK(h.printed_line("server hello"));
    CHECK_FALSE(h.printed_line("player hello"));
}

TEST_CASE("MQ Lua: found() lists the scripts on disk, the server's first, with what may run") {
    Host h(0);
    h.write("alpha.lua", "print(1)\n");
    h.write("Beta/init.lua", "print(2)\n");
    h.write("gamma/helper.lua", "return 1\n"); // a folder without init.lua: not a script
    h.write("config/settings.lua", "return {}\n"); // mq.configDir's: not a script
    h.write("notes.txt", "not lua\n");
    fs::path server = h.world.dir / "server" / "scripts";
    fs::create_directories(server);
    std::ofstream(server / "alpha.lua", std::ios::binary) << "print('server alpha')\n";
    std::ofstream(server / "hud.lua", std::ios::binary) << "print('hud')\n";
    h.scripts->set_server_dir(server);
    // Before the zone-in nothing may run, and each says why.
    auto f = h.scripts->found();
    REQUIRE(f.size() == 3);
    CHECK(f[0].name == "alpha");
    CHECK(f[0].server); // the server's copy hides the player's, as run() finds it
    CHECK(f[0].entry == server / "alpha.lua");
    CHECK(f[1].name == "beta");
    CHECK_FALSE(f[1].server);
    CHECK(f[1].entry == h.lua_dir / "Beta" / "init.lua");
    CHECK(f[2].name == "hud");
    for (const auto& s : f) {
        CHECK(s.caps == 0);
        CHECK(s.why.find("until the zone-in") != std::string::npos);
    }
    // Under a hailstone.cfg with the server's hud.lua only: that one may run.
    plugin::Rules r;
    r.configured = true;
    r.social_mode = plugin::ButtonMode::Off;
    r.server_files.push_back({plugin::FileKind::Script, "scripts/hud.lua", *parse_sha256(*mq::script_file_hash(server / "hud.lua"))});
    h.policy.set(r);
    f = h.scripts->found();
    REQUIRE(f.size() == 3);
    CHECK(f[0].caps == 0);
    CHECK(f[0].why == "this server runs only its own scripts (its hailstone.cfg)");
    CHECK(f[2].caps == plugin::kCapAll);
    CHECK(f[2].why.empty());
    // An edited file is hashed again.
    std::ofstream(server / "hud.lua", std::ios::binary) << "print('hud, edited')\n";
    fs::last_write_time(server / "hud.lua", fs::last_write_time(server / "hud.lua") + std::chrono::seconds(5));
    CHECK(h.scripts->found()[2].caps == 0);
    // The kept flag: the server's own, running under its hailstone.cfg.
    h.policy.set(h.server_scripts({"alpha.lua"}));
    h.scripts->set_server_dir({});
    REQUIRE(h.scripts->run("alpha", {}));
    REQUIRE(h.scripts->list().size() == 1);
    CHECK(h.scripts->list().front().kept);
}

TEST_CASE("MQ Lua: the character's autorun list starts its scripts at the login, not again at a zone change") {
    Host h(0);
    h.write("auto.lua", "while true do require('mq').delay(10) end\n");
    h.write("other.lua", "while true do require('mq').delay(10) end\n");
    test::ScratchDir chars("mq_autorun");
    fs::path file = chars.path() / "world" / "Tester" / "scripts.ini";
    // No character yet: nothing to keep it in.
    CHECK_FALSE(h.scripts->autorun_available());
    CHECK_FALSE(h.scripts->set_autorun("auto", true));
    // The character's file, before the zone-in's rules: nothing starts.
    h.scripts->set_autorun_file(file);
    CHECK(h.scripts->autorun_available());
    REQUIRE(h.scripts->set_autorun("Auto", true));
    CHECK(h.scripts->autorun("auto"));
    CHECK_FALSE(h.scripts->autorun("other"));
    CHECK(h.scripts->autorun_list() == std::vector<std::string>{"auto"});
    {
        IniFile ini;
        REQUIRE(ini.load(file));
        CHECK(ini.get_int("Autorun", "auto", 0) == 1);
    }
    h.scripts->policy_changed();
    CHECK_FALSE(h.scripts->running("auto"));

    // The login's zone-in: the rules allow the player's scripts, and the list's start.
    h.allow_unlisted(plugin::kCapAll);
    h.scripts->policy_changed();
    CHECK(h.scripts->running("auto"));
    CHECK_FALSE(h.scripts->running("other"));
    // The player stops it; the next zone-in sets the same file and starts nothing.
    h.scripts->stop("auto");
    h.pulses(1);
    h.scripts->set_autorun_file(file);
    CHECK_FALSE(h.scripts->running("auto"));

    // Another session reads the file back; a new file set while the rules allow it starts its scripts at once.
    h.scripts->set_autorun_file({});
    CHECK_FALSE(h.scripts->autorun("auto"));
    h.scripts->set_autorun_file(file);
    CHECK(h.scripts->autorun("auto"));
    CHECK(h.scripts->running("auto"));
    // Taken off the list: written, and kept off.
    REQUIRE(h.scripts->set_autorun("auto", false));
    IniFile ini;
    REQUIRE(ini.load(file));
    CHECK_FALSE(ini.get("Autorun", "auto"));
    CHECK(h.scripts->autorun_list().empty());
    // A name that would break the file is refused.
    CHECK_FALSE(h.scripts->set_autorun("a=b", true));
}

TEST_CASE("MQ Lua: capabilities hold commands, reads, events and windows to the policy") {
    Host h(plugin::kCapRead | plugin::kCapCommands);
    h.write("c.lua", R"(
local mq = require('mq')
mq.cmd('/sit')
mq.cmd('/target id 11')
mq.cmd('/who')
mq.cmd('/docommand /cast 1')
mq.cmd('/echo ${Me.Level}')
print(pcall(mq.event, 'x', '#*#', function() end))
print(pcall(mq.imgui.init, 'w', function() end))
)");
    REQUIRE(h.scripts->run("c", {}));
    h.pulses(1);
    CHECK(h.sent == std::vector<std::string>{"/who"});
    CHECK(h.target == 0);
    CHECK(h.printed_line("50"));
    auto refused = [&](const std::string& what) {
        for (const auto& l : h.printed)
            if (l.find(what) != std::string::npos && l.find("permission") != std::string::npos) return true;
        return false;
    };
    CHECK(refused("/sit needs the server's 'move' permission"));
    CHECK(refused("/target needs the server's 'target' permission"));
    CHECK(refused("/cast needs the server's 'actions' permission"));
    bool event_refused = false, ui_refused = false;
    for (const auto& l : h.printed) {
        event_refused |= l.rfind("false", 0) == 0 && l.find("'events' permission") != std::string::npos;
        ui_refused |= l.rfind("false", 0) == 0 && l.find("'ui' permission") != std::string::npos;
    }
    CHECK(event_refused);
    CHECK(ui_refused);

    // Without read, TLOs fail; the player's own typing is never limited.
    Host r(plugin::kCapCommands);
    r.write("d.lua", "print(pcall(function() return require('mq').TLO.Me.Name() end))\n");
    REQUIRE(r.scripts->run("d", {}));
    r.pulses(1);
    bool read_refused = false;
    for (const auto& l : r.printed) read_refused |= l.find("'read' permission") != std::string::npos;
    CHECK(read_refused);
    r.run("/target id 11");
    CHECK(r.target == 11);
    r.run("/echo ${Me.PctHPs}");
    CHECK(r.printed_line("75"));
}

TEST_CASE("MQ Lua: mq.bind needs 'commands' and never takes the client's own commands") {
    Host h;
    h.write("b.lua", R"(
local mq = require('mq')
for _, c in ipairs({'/say', '/g', '/tell', '/r', '/lua', '/echo'}) do print(c, pcall(mq.bind, c, function() end)) end
mq.bind('/mine', function(a) print('mine', a) end)
mq.delay(100000)
)");
    REQUIRE(h.scripts->run("b", {}));
    h.pulses(1);
    for (const char* c : {"/say", "/g", "/tell", "/r", "/lua", "/echo"}) {
        INFO(c);
        bool refused = false;
        for (const auto& l : h.printed)
            refused |= l.rfind(std::string(c) + " false", 0) == 0 && l.find("the client's own command") != std::string::npos;
        CHECK(refused);
    }
    h.run("/g hello");
    CHECK(h.sent == std::vector<std::string>{"/g hello"}); // the game's /gsay, not a script's
    h.run("/mine x");
    h.pulses(1);
    CHECK(h.printed_line("mine x"));

    Host r(plugin::kCapRead);
    r.write("c.lua", "print(pcall(require('mq').bind, '/mine', function() end))\n");
    REQUIRE(r.scripts->run("c", {}));
    r.pulses(1);
    bool refused = false;
    for (const auto& l : r.printed) refused |= l.rfind("false", 0) == 0 && l.find("'commands' permission") != std::string::npos;
    CHECK(refused);
    CHECK_FALSE(r.hooks.has_command("/mine"));
}

namespace {
std::string g_clipboard;
} // namespace

TEST_CASE("MQ Lua: the clipboard is only for a script that may do everything") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) { return g_clipboard.c_str(); };
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* t) { g_clipboard = t; };
    g_clipboard = "the player's secret";
    const char* code = R"(
print('get', pcall(ui.GetClipboardText))
print('set', pcall(ImGui.SetClipboardText, 'planted'))
)";
    {
        Host h(plugin::kCapAll & ~plugin::kCapPackets); // an add-on's ceiling, or a server's player_caps
        h.write("c.lua", code);
        REQUIRE(h.scripts->run("c", {}));
        h.pulses(1);
        int refused = 0;
        for (const auto& l : h.printed)
            refused += l.find("false") != std::string::npos && l.find("may use the clipboard") != std::string::npos;
        CHECK(refused == 2);
        CHECK(g_clipboard == "the player's secret");
    }
    {
        Host h;
        h.write("c.lua", code);
        REQUIRE(h.scripts->run("c", {}));
        h.pulses(1);
        CHECK(h.printed_line("get true the player's secret"));
        CHECK(h.printed_line("set true"));
        CHECK(g_clipboard == "planted");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("MQ Lua: a script that never yields is paused each frame, not left to hang the client") {
    Host h;
    h.write("spin.lua", "local n = 0 while true do n = n + 1 end\n");
    REQUIRE(h.scripts->run("spin", {}));
    h.pulses(3);
    CHECK(h.scripts->running("spin"));
    h.scripts->stop("spin");
    CHECK_FALSE(h.scripts->running("spin"));
}

TEST_CASE("MQ Lua: mq.pickle writes under the config directory and dofile reads it back as data") {
    Host h;
    h.write("cfg.lua", R"(
local mq = require('mq')
mq.pickle('settings/test.lua', {a = 1, b = {'x', 'y'}, name = 'bob'})
local t = dofile('settings/test.lua')
print(t.a, t.b[2], t.name)
print(pcall(mq.pickle, '../outside.lua', {}))
print(load, io, os.execute)
)");
    REQUIRE(h.scripts->run("cfg", {}));
    h.pulses(1);
    CHECK(fs::exists(h.config_dir / "settings" / "test.lua"));
    CHECK(h.printed_line("1 y bob"));
    CHECK_FALSE(fs::exists(h.world.dir / "outside.lua"));
    CHECK(h.printed_line("nil nil nil"));
}

TEST_CASE("MQ Lua: loadfile and dofile give back only data, never a function a file made") {
    Host h;
    fs::create_directories(h.config_dir);
    auto put = [&](const char* name, const char* text) {
        std::ofstream(h.config_dir / name, std::ios::binary) << text;
    };
    put("data.lua", "local t = {a = 1, list = {'x', 'y'}} t.self = t return t, 'second'\n");
    put("closure.lua", "return {run = function(mq) mq.cmd('/sit') end}\n");
    put("bare.lua", "return function() return 1 end\n");
    put("method.lua", "return ('').rep\n");
    put("key.lua", "return {[function() end] = true}\n");
    put("broken.lua", "return {} + 1\n"); // (no `error` in its empty environment)
    h.write("cfg_data.lua", R"(
local mq = require('mq')
local t, second = dofile('data.lua')
print('data', t.a, t.list[2], t.self == t, second)
local f = assert(loadfile('data.lua'))
print('again', f().a, f().a)
for _, name in ipairs({'closure.lua', 'bare.lua', 'method.lua', 'key.lua'}) do
    local g, why = loadfile(name)
    print(name, g, why and why:find('not data', 1, true) ~= nil)
    print(name, pcall(dofile, name))
end
print('broken', loadfile('broken.lua'))
-- What the script writes itself is held to the same: text through mq.pickle's write, then loaded.
mq._write_config('written.lua', 'return function(m) return m end')
print('written', loadfile('written.lua'))
)");
    REQUIRE(h.scripts->run("cfg_data", {}));
    // File work can use the host's time slice; observe completion before checking the final refusal messages.
    for (int pulse = 0; pulse < 64 && h.scripts->running("cfg_data"); ++pulse) h.pulses(1);
    REQUIRE_FALSE(h.scripts->running("cfg_data"));
    CHECK(h.printed_line("data 1 y true second"));
    CHECK(h.printed_line("again 1 1"));
    for (const char* name : {"closure.lua", "bare.lua", "method.lua", "key.lua"}) {
        CHECK(h.printed_line(std::string(name) + " nil true"));
        bool refused = false;
        for (const auto& l : h.printed) refused |= l.rfind(std::string(name) + " false", 0) == 0;
        CHECK(refused);
    }
    bool broken = false, written = false;
    for (const auto& l : h.printed) {
        broken |= l.rfind("broken nil", 0) == 0 && l.find("broken.lua:1:") != std::string::npos;
        written |= l.rfind("written nil", 0) == 0 && l.find("not data") != std::string::npos;
    }
    CHECK(broken);
    CHECK(written);
}

TEST_CASE("MQ Lua: mq.pickle's writes are held to a size per file and per config directory") {
    Host h(plugin::kCapRead | plugin::kCapCommands); // a folder of its own: scripts/config/limits/
    const fs::path own = h.config_dir / "limits";
    fs::create_directories(own);
    for (std::size_t i = 0; i + 1 < mq::kMaxConfigFiles; ++i)
        std::ofstream(own / ("f" + std::to_string(i) + ".lua"), std::ios::binary) << "return {}\n";
    h.write("limits.lua", R"(
local mq = require('mq')
print('big', pcall(mq.pickle, 'big.lua', {s = string.rep('a', 9 * 1024 * 1024)}))
print('last', pcall(mq.pickle, 'last.lua', {v = 1}))
print('full', pcall(mq.pickle, 'more.lua', {v = 2}))
print('replace', pcall(mq.pickle, 'f1.lua', {v = 3}))
print('read', dofile('f1.lua').v)
)");
    REQUIRE(h.scripts->run("limits", {}));
    h.pulses(1);
    bool big = false, full = false;
    for (const auto& l : h.printed) {
        big |= l.rfind("big false", 0) == 0 && l.find("over 8 MiB") != std::string::npos;
        full |= l.rfind("full false", 0) == 0 && l.find("config directory is full") != std::string::npos;
    }
    CHECK(big);
    CHECK(h.printed_line("last true"));
    CHECK(full);
    CHECK(h.printed_line("replace true"));
    CHECK(h.printed_line("read 3"));
    CHECK_FALSE(fs::exists(own / "big.lua"));
    CHECK_FALSE(fs::exists(own / "more.lua"));
}

TEST_CASE("MQ Lua: require of a huge name that is not there is a plain error, and the script goes on") {
    Host h;
    h.write("huge_require.lua", R"(
for i = 1, 3 do
    local ok, why = pcall(require, string.rep('a', 4 * 1024 * 1024))
    print('try', i, ok, #why > 4 * 1024 * 1024, why:sub(1, 8))
end
print('parse', mq == nil, require('mq').parse('${Me.PctHPs}'))
)");
    REQUIRE(h.scripts->run("huge_require", {}));
    h.pulses(1);
    for (int i = 1; i <= 3; ++i) CHECK(h.printed_line("try " + std::to_string(i) + " false true module '"));
    CHECK(h.printed_line("parse false 75"));
}

TEST_CASE("MQ commands: /target, ${} for typed lines, /noparse, /squelch") {
    Host h;
    h.run("/target fippy");
    CHECK(h.target == 11);
    h.run("/target clear");
    CHECK(h.target == 0);
    h.run("/target id 12");
    CHECK(h.target == 12);
    h.run("/tell Groupie my hp is ${Me.PctHPs}");
    CHECK(h.sent.back() == "/tell Groupie my hp is 75");
    h.run("/noparse /tell Groupie ${Me.PctHPs}");
    CHECK(h.sent.back() == "/tell Groupie ${Me.PctHPs}");
    std::size_t before = h.printed.size();
    h.run("/squelch /target nobody_here");
    CHECK(h.printed.size() == before);
    h.run("/target nobody_here");
    CHECK(h.printed.back() == "There are no spawns matching: (nobody_here)");
    h.run("/target");
    CHECK(h.target == 12); // unchanged
    CHECK(h.printed.back().rfind("Usage: /target", 0) == 0);
}

// ---------------------------------------------------------------- Lua socials

namespace {

// A hailstone.cfg's [socials].
void allow_buttons(Host& h, plugin::ButtonMode mode, std::uint32_t caps, int press_commands = 0, bool faces = true) {
    plugin::Rules r;
    r.configured = true;
    r.social_mode = mode;
    r.social_caps = caps;
    r.social_press_commands = press_commands;
    r.social_faces = faces;
    h.policy.set(r);
    h.scripts->policy_changed();
}

mq::ButtonPress press(std::string code, std::string face = {}) {
    mq::ButtonPress p;
    p.social = 4;
    p.name = "Test";
    p.lua.press = std::move(code);
    p.lua.face = std::move(face);
    return p;
}

bool printed_part(const Host& h, const std::string& text) {
    for (const auto& l : h.printed)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("Lua socials: off before the zone-in and when the server's hailstone.cfg says so") {
    Host h(0);
    std::string why;
    CHECK_FALSE(h.scripts->press_button(press("mq.cmd('/sit')"), &why));
    CHECK(why.find("until the zone-in") != std::string::npos);
    allow_buttons(h, plugin::ButtonMode::Off, plugin::kCapAll);
    CHECK_FALSE(h.scripts->press_button(press("mq.cmd('/sit')"), &why));
    CHECK(why == "this server does not run Lua socials");
    CHECK(h.printed_line("[Lua] social 'Test': this server does not run Lua socials"));
    CHECK(h.sent.empty());
    // A server that allows the player's scripts does not make socials run.
    h.allow_unlisted(plugin::kCapAll);
    CHECK_FALSE(h.scripts->press_button(press("mq.cmd('/sit')")));
    // No hailstone.cfg: full mode.
    h.policy.set(plugin::Rules{});
    CHECK(h.scripts->button_rules({}).mode == plugin::ButtonMode::Full);
    CHECK(h.scripts->press_button(press("mq.cmd('/sit')")));
}

TEST_CASE("Lua socials: press mode runs at once, keeps state, sees the modifiers, holds to caps and limits") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapRead | plugin::kCapActions | plugin::kCapTarget, 2);
    auto p = press(R"(
state.n = (state.n or 0) + 1
if button.shift then
    mq.cmd('/cast 2')
elseif mq.TLO.Me.PctHPs() < 80 then
    mq.cmdf('/cast %d', state.n)
end
)");
    REQUIRE(h.scripts->press_button(p));
    CHECK(h.sent == std::vector<std::string>{"/cast 1"});
    REQUIRE(h.scripts->press_button(p));
    CHECK(h.sent.back() == "/cast 2");
    p.shift = true;
    REQUIRE(h.scripts->press_button(p));
    CHECK(h.sent.size() == 3);
    CHECK(h.sent.back() == "/cast 2");
    CHECK_FALSE(h.scripts->button_running(4));

    // Caps: /sit is movement.
    REQUIRE(h.scripts->press_button(press("mq.cmd('/sit')")));
    CHECK(h.sent.size() == 3);
    CHECK(printed_part(h, "/sit needs the server's 'move' permission"));
    // At most two commands a press.
    CHECK_FALSE(h.scripts->press_button(press("mq.cmd('/cast 1') mq.cmd('/cast 2') mq.cmd('/cast 3')")));
    CHECK(h.sent.size() == 5);
    CHECK(printed_part(h, "at most 2 commands"));
    // No waiting, no events, no endless loops.
    CHECK_FALSE(h.scripts->press_button(press("mq.delay(100)")));
    CHECK(printed_part(h, "without waiting"));
    CHECK_FALSE(h.scripts->press_button(press("mq.event('x', '#*#', function() end)")));
    CHECK(printed_part(h, "'events' permission"));
    CHECK_FALSE(h.scripts->press_button(press("while true do end")));
    CHECK(printed_part(h, "ran longer than 50 ms"));
    // A syntax error is told, and the next press tells it again.
    CHECK_FALSE(h.scripts->press_button(press("if then")));
    std::string why;
    CHECK_FALSE(h.scripts->press_button(press("if then"), &why));
    CHECK(why.find("social 'Test' (press)") != std::string::npos);
}

// A string match runs no Lua instructions, so the count hook never stopped one: the vendored Lua's long-match check
// (ui::lua::kMatchCheckKey, the script state's match_check) holds it to the same limit.
TEST_CASE("Lua socials: a runaway string match is stopped at the press's limit") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapRead, 2);
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(h.scripts->press_button(press("string.find(('a'):rep(1e5), ('a-'):rep(20) .. 'b')")));
    CHECK(printed_part(h, "ran longer than 50 ms"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(h.scripts->press_button(press("assert(string.find(('a'):rep(100) .. 'b', 'a-b') == 1)")));
}

// The long-match stop is the runaway's own, so a pcall around the match passes it on (Script::runaway).
TEST_CASE("Lua socials: a pcall around a runaway string match does not catch the stop") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapRead, 2);
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(h.scripts->press_button(
        press("while true do pcall(string.find, ('a'):rep(1e5), ('a-'):rep(20) .. 'b') end")));
    CHECK(printed_part(h, "ran longer than 50 ms"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST_CASE("Lua socials: editing the code starts a new state; forget_button drops it") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapAll);
    auto p = press("state.n = (state.n or 0) + 1 print('n', state.n)");
    h.scripts->press_button(p);
    h.scripts->press_button(p);
    CHECK(h.printed_line("n 2"));
    p.lua.press += " -- changed";
    h.scripts->press_button(p);
    CHECK(h.printed.back() == "n 1");
    h.scripts->forget_button(4);
    h.scripts->press_button(p);
    CHECK(h.printed.back() == "n 1");
}

TEST_CASE("Lua socials: full mode waits across frames; a second press stops it") {
    Host h(0);
    auto p = press("mq.cmd('/cast 1') mq.delay('1s') mq.cmd('/cast 2')");
    allow_buttons(h, plugin::ButtonMode::Full, plugin::kCapAll);
    REQUIRE(h.scripts->press_button(p));
    CHECK(h.sent == std::vector<std::string>{"/cast 1"});
    CHECK(h.scripts->button_running(4));
    h.pulses(2, 600);
    CHECK(h.sent.size() == 2);
    CHECK_FALSE(h.scripts->button_running(4));

    REQUIRE(h.scripts->press_button(p));
    CHECK(h.scripts->button_running(4));
    REQUIRE(h.scripts->press_button(p)); // stops
    CHECK_FALSE(h.scripts->button_running(4));
    h.pulses(3, 600);
    CHECK(h.sent.size() == 3);

    // The policy withdrawing full mode stops it.
    REQUIRE(h.scripts->press_button(p));
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapAll);
    CHECK_FALSE(h.scripts->button_running(4));
}

TEST_CASE("Lua socials: faces read the game state, at most every 200 ms, and run no commands") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapAll);
    game::LuaSocial l{"mq.cmd('/sit')", R"(
state.faces = (state.faces or 0) + 1
return {label = 'HP ' .. mq.TLO.Me.PctHPs(), color = 0xff0000, cooldown = 1.5, overlay = state.faces,
        enabled = false, tooltip = 'tip'}
)"};
    const auto& f = h.scripts->button_face(4, "Test", l);
    CHECK(f.has);
    CHECK(f.label == "HP 75");
    CHECK(f.color == 0xff0000u);
    CHECK(f.cooldown == 1.0f);
    CHECK(f.overlay == "1");
    CHECK_FALSE(f.enabled);
    CHECK(f.tooltip == "tip");
    CHECK(h.scripts->button_face(4, "Test", l).overlay == "1");
    h.clock += 250;
    CHECK(h.scripts->button_face(4, "Test", l).overlay == "2");

    game::LuaSocial bad{"", "mq.cmd('/sit') return 'x'"};
    h.clock += 250;
    const auto& b = h.scripts->button_face(5, "Bad", bad);
    CHECK_FALSE(b.has);
    CHECK(b.error.find("cannot run commands") != std::string::npos);
    CHECK(h.sent.empty());
    game::LuaSocial colours{"", "return {label = 'c', color = {1, 0.5, 0}}"};
    CHECK(h.scripts->button_face(6, "C", colours).color == 0xff7f00u);

    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapAll, 0, false);
    h.clock += 250;
    CHECK_FALSE(h.scripts->button_face(4, "Test", l).has);
}

TEST_CASE("Lua socials: a dry run lists the commands and waits without sending anything") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapRead | plugin::kCapActions, 0);
    auto p = press("mq.cmd('/cast ${Me.Level}') mq.delay(500) mq.cmd('/sit') state.x = 1");
    auto d = h.scripts->dry_run_button(p);
    CHECK(d.ok);
    REQUIRE(d.lines.size() == 3);
    CHECK(d.lines[0] == "/cast 50");
    CHECK(d.lines[1] == "(wait 500 ms)");
    CHECK(d.lines[2] == "/sit    (blocked: needs 'move')");
    CHECK(h.sent.empty());
    CHECK(d.rules.mode == plugin::ButtonMode::Press);

    // Without Lua socials every command shows as blocked, and reads still work.
    allow_buttons(h, plugin::ButtonMode::Off, 0);
    d = h.scripts->dry_run_button(press("mq.cmd('/cast 1') print(mq.TLO.Me.Level())"));
    CHECK(d.ok);
    REQUIRE(d.lines.size() == 1);
    CHECK(d.lines[0] == "/cast 1    (blocked: needs 'actions')");
    d = h.scripts->dry_run_button(press("error('boom')"));
    CHECK_FALSE(d.ok);
    CHECK(d.error.find("boom") != std::string::npos);
}

TEST_CASE("TLO completion: TLO names, members through the live value and its type's parents, a NULL TLO's type") {
    Host h;
    auto has = [](const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); };
    auto tlos = h.reg.complete(h.ctx, "Ta");
    CHECK(has(tlos, "Target"));
    CHECK_FALSE(has(tlos, "Me"));
    auto me = h.reg.complete(h.ctx, "Me.Pct");
    CHECK(has(me, "PctHPs"));
    CHECK(has(me, "PctMana"));
    CHECK(std::is_sorted(me.begin(), me.end()));
    auto spawn = h.reg.complete(h.ctx, "Me.Na"); // character inherits spawn's Name
    CHECK(has(spawn, "Name"));
    auto deeper = h.reg.complete(h.ctx, "Me.Buff[1].Na"); // buff -> spell
    CHECK(has(deeper, "Name"));
    auto str = h.reg.complete(h.ctx, "Me.Name.Len");
    CHECK(has(str, "Length"));
    CHECK(h.reg.complete(h.ctx, "Nope.X").empty());
}

#ifdef HAILSTONE_MOCK_WEB_HELPER
// mq.web (mq/lua_mq_web.cpp) over the app's WebService and the stand-in helper (tests/mock_web_helper.cpp): the
// script's own tab on the server's sites, a message both ways; without 'web', an error.
TEST_CASE("MQ Lua: mq.web opens the script's own tabs, on the server's sites, under 'web'") {
    web::BrowserHost browser([] { return web::process_transport({HAILSTONE_MOCK_WEB_HELPER}); });
    app::WebService web({[&] { return &browser; }, [] { return nullptr; }, [] { return std::string(); },
                         [] { return std::vector<std::string>{"example.org"}; }, [] {}});
    {
        Host h(plugin::kCapAll, &web);
        h.write("page.lua", R"(
local mq = require('mq')
local tab, why = mq.web.open('https://www.example.org/', 64, 48)
print('open', tab ~= nil)
local bad, why2 = mq.web.open('https://evil.com/')
print('elsewhere', bad == nil and why2 ~= nil)
print('tabs', #mq.web.tabs())
mq.web.post(tab, 'ping')
while true do
  local got = mq.web.messages(tab)
  if #got > 0 then print('got ' .. got[1]) break end
  mq.delay(10)
end
print('url ' .. mq.web.state(tab).url)
while true do mq.delay(1000) end
)");
        REQUIRE(h.scripts->run("page", {}));
        // A real helper process answers through real pipes: wait by the wall clock, not a count of pulses (a slow
        // machine, such as the macOS runner, starts the process later than 3000 quick pulses took).
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!h.printed_line("url https://www.example.org/") && std::chrono::steady_clock::now() < until) {
            browser.poll(0);
            h.pulses(1, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(h.printed_line("open true"));
        CHECK(h.printed_line("elsewhere true"));
        CHECK(h.printed_line("tabs 1"));
        CHECK(h.printed_line("got echo:ping"));
        CHECK(h.printed_line("url https://www.example.org/"));
        // Another owner (the player, another script) sees none of it.
        CHECK(web.tabs("").empty());
        CHECK(web.tabs("script:other").empty());
        const auto mine = web.tabs("script:page");
        REQUIRE(mine.size() == 1);
        CHECK_FALSE(web.navigate("script:other", plugin::kCapAll, mine[0], "https://example.org/"));
        CHECK_FALSE(web.state("", mine[0]));
        h.scripts->stop("page");
        h.pulses(2);
    }
    {
        Host h(plugin::kCapAll & ~plugin::kCapWeb, &web);
        h.write("noweb.lua", "local mq = require('mq')\nprint('available', mq.web.available())\nmq.web.open('https://example.org/')\n");
        REQUIRE(h.scripts->run("noweb", {}));
        h.pulses(3);
        CHECK(h.printed_line("available false"));
        bool refused = false;
        for (const auto& line : h.printed) refused |= line.find("needs the server's 'web' permission") != std::string::npos;
        CHECK(refused);
    }
    browser.shutdown();
}
#endif

TEST_CASE("MQ Lua: a script without every capability keeps its settings in a folder of its own") {
    Host h(plugin::kCapRead | plugin::kCapCommands); // the player's scripts (an add-on's ceiling alike)
    fs::create_directories(h.config_dir);
    std::ofstream(h.config_dir / "Shared.lua", std::ios::binary) << "return {owner = 'server'}\n";
    const std::string shared = (h.config_dir / "Shared.lua").generic_string();
    h.write("capped.lua", std::string(R"(
local mq = require('mq')
print('dir', mq.configDir)
mq.pickle(mq.configDir .. '/Mine.lua', {v = 1})
print('mine', dofile(mq.configDir .. '/Mine.lua').v)
print('rel', pcall(mq.pickle, 'Rel.lua', {v = 2}))
)") + "print('shared', pcall(mq.pickle, '" + shared + "', {owner = 'addon'}))\n" +
                              "print('read', pcall(dofile, '" + shared + "'))\n");
    REQUIRE(h.scripts->run("capped", {}));
    h.pulses(1);
    CHECK(h.printed_line("dir " + (h.config_dir / "capped").string()));
    CHECK(h.printed_line("mine 1"));
    CHECK(h.printed_line("rel true"));
    CHECK(fs::exists(h.config_dir / "capped" / "Mine.lua"));
    CHECK(fs::exists(h.config_dir / "capped" / "Rel.lua"));
    bool write_refused = false, read_refused = false;
    for (const auto& l : h.printed) {
        write_refused |= l.rfind("shared false", 0) == 0 && l.find("outside the config directory") != std::string::npos;
        read_refused |= l.rfind("read false", 0) == 0;
    }
    CHECK(write_refused);
    CHECK(read_refused);
    std::ifstream in(h.config_dir / "Shared.lua", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "return {owner = 'server'}\n");

    // A script with every capability keeps the shared folder, and reaches the others' folders.
    Host f;
    f.write("full.lua", R"(
local mq = require('mq')
print('dir', mq.configDir)
mq.pickle('capped/Theirs.lua', {v = 3})
)");
    REQUIRE(f.scripts->run("full", {}));
    f.pulses(1);
    CHECK(f.printed_line("dir " + f.config_dir.string()));
    CHECK(fs::exists(f.config_dir / "capped" / "Theirs.lua"));
}

TEST_CASE("MQ Lua: the config directory's files are data, never started or required as code") {
    Host h;
    h.write("config/evil.lua", "print('ran evil')\n");
    h.write("config/init.lua", "print('ran init')\n");
    h.write("user.lua", "print(pcall(require, 'config.evil'))\n");
    std::string why;
    CHECK_FALSE(h.scripts->run("config/evil", {}, &why));
    CHECK(why.find("no script named") != std::string::npos);
    CHECK_FALSE(h.scripts->run("CONFIG/evil.lua", {}));
    CHECK_FALSE(h.scripts->run("config", {}));
    h.run("/lua run config/evil");
    REQUIRE(h.scripts->run("user", {}));
    h.pulses(2);
    for (const auto& l : h.printed) CHECK(l.find("ran ") == std::string::npos);
    bool refused = false;
    for (const auto& l : h.printed) refused |= l.rfind("false", 0) == 0 && l.find("not found") != std::string::npos;
    CHECK(refused);
}

TEST_CASE("MQ Lua: a script name stays in the script folders; the config directory in any case or spelling") {
    Host h;
    std::ofstream(h.world.dir / "outside.lua", std::ios::binary) << "print('ran outside')\n";
    const std::string abs = (h.world.dir / "outside").generic_string();
    std::string why;
    CHECK_FALSE(h.scripts->run(abs, {}, &why));
    CHECK(why.find("no script named") != std::string::npos);
    CHECK_FALSE(h.scripts->run(abs + ".lua", {}));
    h.run("/lua run " + abs);
    // The config directory as Windows or a case-insensitive volume would take it.
    h.write("Config/evil.lua", "print('ran evil')\n");
    CHECK_FALSE(h.scripts->run("Config/evil", {}));
    h.write("config./evil2.lua", "print('ran evil')\n");
    CHECK_FALSE(h.scripts->run("config./evil2", {}));
    // A link inside the folder to a file outside it.
    std::error_code ec;
    fs::create_symlink(h.world.dir / "outside.lua", h.lua_dir / "link.lua", ec);
    if (!ec) CHECK_FALSE(h.scripts->run("link", {}));
    // require: nothing absolute either, and the config directory in another case is still data.
    h.write("user.lua", "print(pcall(require, '" + abs + "'))\nprint(pcall(require, 'Config.evil'))\n");
    REQUIRE(h.scripts->run("user", {}));
    h.pulses(2);
    for (const auto& l : h.printed) CHECK(l.find("ran ") == std::string::npos);
}

TEST_CASE("MQ Lua: a script started by a plugin or script gets no more than its starter may do") {
    Host h;
    h.write("b.lua", "local mq = require('mq') mq.delay(100000)\n");
    {
        plugin::CallerScope scope(plugin::kCapRead | plugin::kCapCommands, "starter");
        h.run("/lua run b"); // (as mq.cmd('/lua run b') from a script with those)
    }
    REQUIRE(h.scripts->running("b"));
    for (const auto& s : h.scripts->list())
        if (s.name == "b") CHECK(s.caps == (plugin::kCapRead | plugin::kCapCommands));
    h.scripts->stop("b");
    h.pulses(1);
    // A starter that may give nothing the file would get: refused.
    {
        plugin::Rules r;
        r.configured = true;
        r.player_script_caps = plugin::kCapChat;
        r.social_mode = plugin::ButtonMode::Off;
        h.policy.set(r);
    }
    {
        plugin::CallerScope scope(plugin::kCapRead, "starter");
        std::string why;
        CHECK_FALSE(h.scripts->run("b", {}, &why));
        CHECK(why.find("starter") != std::string::npos);
    }
    // The player's own start: everything the file may do.
    REQUIRE(h.scripts->run("b", {}));
    for (const auto& s : h.scripts->list())
        if (s.name == "b") CHECK(s.caps == plugin::kCapChat);
}

TEST_CASE("MQ commands: a window's line (LiteralLine) is sent as it is, without ${}") {
    Host h;
    {
        mq::LiteralLine literal;
        h.run("/guildmotd Welcome ${Me.Name}, hp ${Me.PctHPs}");
    }
    CHECK(h.sent.back() == "/guildmotd Welcome ${Me.Name}, hp ${Me.PctHPs}");
    h.run("/say ${Me.PctHPs}"); // typed lines again
    CHECK(h.sent.back() == "/say 75");
}

TEST_CASE("MQ commands: a plugin's command rewrite is held to the script's capabilities too") {
    Host h;
    // A compiled plugin's command_line hook (plugin/native_plugins.cpp): a line filter registered after MQ's, which
    // turns its own alias into another command.
    plugin::Hooks::Owner plugin_owner = h.hooks.new_owner("aliases");
    h.hooks.on_command_line(plugin_owner, [](std::string& line) {
        if (line.rfind("/myalias", 0) == 0) line = "/target id 12";
        if (line.rfind("/mysay", 0) == 0) line = "/say from the alias";
        return plugin::Flow::Continue;
    });
    {
        // A script that may run commands but not change the target (kCapTarget).
        plugin::CallerScope scope(plugin::kCapCommands | plugin::kCapRead | plugin::kCapChat, "capped");
        h.run("/target id 12");
        CHECK(h.target == 0);
        h.run("/myalias");
        CHECK(h.target == 0); // the rewritten line needs 'target' too
        CHECK(printed_part(h, "capped: /target needs the server's 'target' permission"));
        // A rewrite it may run goes, once.
        const std::size_t sent = h.sent.size();
        h.run("/mysay");
        REQUIRE(h.sent.size() == sent + 1);
        CHECK(h.sent.back() == "/say from the alias");
        // ${} is expanded once, before the rewrite, as before.
        h.run("/say ${Me.PctHPs}");
        CHECK(h.sent.back() == "/say 75");
    }
    h.run("/myalias"); // the player: anything
    CHECK(h.target == 12);
}

namespace {

// An ImGui context with a frame open, for ScriptHost::imgui().
struct ImGuiFrame {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiFrame() {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        io.DisplaySize = ImVec2(800, 600);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
    }
    ~ImGuiFrame() {
        ImGui::Render();
        ImGui::DestroyContext(ctx);
    }
};

} // namespace

TEST_CASE("MQ Lua: a script's tables cannot have __gc; nothing it registers outlives it") {
    Host h;
    h.write("gc.lua", R"(
local mq = require('mq')
print('gc', pcall(setmetatable, {}, {__gc = function() mq.bind('/late', function() end) end}))
print('plain', getmetatable(setmetatable({}, {__index = {x = 1}})).__index.x)
-- A TLO value's metatable has a __gc of its own; a script cannot reach it to swap that out.
print('tlometa', type(getmetatable(mq.TLO.Me)), getmetatable(mq.TLO.Me))
print('tloset', pcall(function() getmetatable(mq.TLO.Me).__gc = function() end end))
mq.bind('/early', function() end)
mq.delay(100000)
)");
    REQUIRE(h.scripts->run("gc", {}));
    h.pulses(1);
    CHECK(printed_part(h, "gc false"));
    CHECK(printed_part(h, "a script's tables cannot have __gc"));
    CHECK(h.printed_line("plain 1"));
    CHECK(h.printed_line("tlometa string mq.TLO"));
    CHECK(printed_part(h, "tloset false"));
    CHECK(h.hooks.has_command("/early"));
    h.run("/lua stop gc");
    CHECK_FALSE(h.scripts->running("gc"));
    // A finalizer at the close would have bound /late as the client's own, holding the freed script.
    CHECK_FALSE(h.hooks.has_command("/late"));
    CHECK_FALSE(h.hooks.has_command("/early"));
}

TEST_CASE("MQ Lua: a window that destroys itself and fails names itself; the others still draw") {
    ImGuiFrame frame;
    Host h;
    h.write("w.lua", R"(
local mq = require('mq')
mq.imgui.init('A', function() mq.imgui.destroy('A') error('x') end)
mq.imgui.init('B', function() print('drew B') end)
mq.delay(100000)
)");
    REQUIRE(h.scripts->run("w", {}));
    h.pulses(1);
    h.scripts->imgui();
    CHECK(printed_part(h, "window A: "));
    CHECK(h.printed_line("drew B") == false); // the error ends the script: B is not drawn after it
    CHECK_FALSE(h.scripts->running("w"));

    // Destroying another window skips it; the rest are drawn this frame.
    h.write("v.lua", R"(
local mq = require('mq')
mq.imgui.init('A', function() print('drew A') mq.imgui.destroy('B') end)
mq.imgui.init('B', function() print('drew B') end)
mq.imgui.init('C', function() print('drew C') end)
mq.delay(100000)
)");
    REQUIRE(h.scripts->run("v", {}));
    h.pulses(1);
    h.scripts->imgui();
    CHECK(h.printed_line("drew A"));
    CHECK_FALSE(h.printed_line("drew B"));
    CHECK(h.printed_line("drew C"));
    CHECK(h.scripts->running("v"));
}

TEST_CASE("MQ Lua: a window that never returns is stopped, also after a hook ran on the script's state") {
    ImGuiFrame frame;
    // A 200 ms limit for the test (kRunawayMs, 5 s, in the game): the hook's own 50 ms must not be left in place.
    Host h(plugin::kCapAll, nullptr, 200);
    h.write("spinwin.lua", R"(
local mq = require('mq')
hailstone.on_packet('zone', 'in', 0x7e57, function() return false end)
mq.imgui.init('Spin', function() while true do end end)
mq.delay(100000)
)");
    REQUIRE(h.scripts->run("spinwin", {}));
    h.pulses(1);
    net::AppPacket in(0x7e57);
    CHECK(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, in) == plugin::Flow::Continue);
    h.scripts->imgui(); // the runaway limit, then the script ends
    CHECK(printed_part(h, "window Spin: "));
    CHECK(printed_part(h, "ran longer than 200 ms"));
    CHECK_FALSE(h.scripts->running("spinwin"));
}

TEST_CASE("MQ Lua: mq.delay takes math.huge and huge text as a long wait, and NaN as a bad time") {
    Host h;
    h.write("d.lua", R"(
local mq = require('mq')
print('huge', mq._delay_ms(math.huge), mq._delay_ms(-math.huge), mq._delay_ms(1e300), mq._delay_ms('1e300s'))
print('int', mq._delay_ms(math.maxinteger), mq._delay_ms(1500), mq._delay_ms('2s'), mq._delay_ms(2.9))
print('nan', pcall(mq._delay_ms, 0/0))
print('nantext', pcall(mq._delay_ms, 'nan'))
print('inftext', mq._delay_ms('inf'))
print('tloint', mq.TLO.Int('nan')(), mq.TLO.Int('1e300')(), mq.TLO.Int('-inf')(), mq.TLO.Int('7.9')())
mq.delay(math.huge)
print('never')
)");
    REQUIRE(h.scripts->run("d", {}));
    h.pulses(3, 1000);
    CHECK(h.printed_line("huge 9000000000000000 -9000000000000000 9000000000000000 9000000000000000"));
    CHECK(h.printed_line("int 9000000000000000 1500 2000 2"));
    CHECK(printed_part(h, "nan false"));
    CHECK(printed_part(h, "nantext false"));
    CHECK(h.printed_line("inftext 9000000000000000"));
    CHECK(h.printed_line("tloint nil nil nil 7"));
    CHECK_FALSE(h.printed_line("never"));
    CHECK(h.scripts->running("d"));
}

TEST_CASE("MQ Lua: mq.event's patterns, as MQ's: wildcards, arguments by number, case, links, |${...}|") {
    Host h;
    h.write("ev.lua", R"(
local mq = require('mq')
mq.event('slain', '#*#You have slain #1#!#*#', function(line, who) print('slain', who) end)
mq.event('hits', '#2# hits #1# for #3# points.', function(line, a, b, c) print('hits', a, b, c) end)
mq.event('gap', 'gap #3# #1#', function(line, a, b, c) print('gap', a, b == '' and '(empty)' or b, c) end)
mq.event('lit', '(1+1) = [#1#]', function(line, v) print('lit', v) end)
mq.event('hash', '##a## #1#', function(line, v) print('hash', v) end)
mq.event('waves', '#1# waves', function(line, who) print('waves', who, line) end)
mq.event('raw', '#1# nods', function(line, who) print('raw', #who) end, {keepLinks = true})
mq.event('me', 'hello |${String[Soandso]}|', function(line) print('me', line) end)
mq.event('any', '#*#', function(line) if #line > 100 then print('long', #line) end end)
while true do mq.doevents() mq.delay(10) end
)");
    REQUIRE(h.scripts->run("ev", {}));
    h.pulses(1);
    const std::string link_bob = "\x12" "1Bob\x12";
    for (const std::string& text : {std::string("You have slain a rat!"), std::string("A GNOLL HITS you FOR 12 POINTS."),
                                   std::string("gap x y"), std::string("(1+1) = [2]"), std::string("#a# x y"),
                                   std::string("(1+1) = 2"), link_bob + " waves", link_bob + " nods",
                                   std::string("Hello SOANDSO"), std::string(3000, 'z')}) {
        plugin::ChatLine line{text, 0, 0};
        h.hooks.chat(line);
    }
    h.pulses(2, 10);
    CHECK(h.printed_line("slain a rat"));
    CHECK(h.printed_line("hits you A GNOLL 12")); // case ignored, arguments by number
    CHECK(h.printed_line("gap y (empty) x"));      // a number the pattern lacks: ""
    CHECK(h.printed_line("lit 2"));
    CHECK(h.printed_line("hash x y"));
    CHECK(h.printed_line("waves Bob Bob waves")); // the link shown as its text, in the line too
    CHECK(h.printed_line("raw " + std::to_string(link_bob.size()))); // keepLinks: the tag as it came
    CHECK(h.printed_line("me Hello SOANDSO"));     // |${...}| read when the line came
    for (const auto& l : h.printed) CHECK(l.find("long") == std::string::npos); // 2048 bytes or more: no events
}

TEST_CASE("Lua socials: a dry run does nothing but note commands and waits") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Full, plugin::kCapAll & ~plugin::kCapPackets);
    auto d = h.scripts->dry_run_button(press(R"(
mq.cmd('/sit')
print('bind', pcall(mq.bind, '/drybind', function() end))
print('event', pcall(mq.event, 'e', '#*#', function() end))
print('window', pcall(mq.imgui.init, 'w', function() end))
print('pickle', pcall(mq.pickle, 'dry.lua', {a = 1}))
print('ability', pcall(hailstone.use_ability, 'skill', 'Taunt'))
print('read', mq.TLO.Me.Level())
)"));
    CHECK(d.ok);
    REQUIRE(d.lines.size() == 1);
    CHECK(d.lines[0] == "/sit");
    CHECK(h.sent.empty());
    for (const char* what : {"bind false", "event false", "window false", "pickle false", "ability false"}) {
        INFO(what);
        bool refused = false;
        for (const auto& l : h.printed)
            refused |= l.rfind(what, 0) == 0 && l.find("does nothing in a dry run") != std::string::npos;
        CHECK(refused);
    }
    CHECK(h.printed_line("read 50"));
    CHECK_FALSE(h.hooks.has_command("/drybind"));
    std::error_code ec;
    bool written = false;
    for (auto it = fs::recursive_directory_iterator(h.world.dir, ec); !ec && it != fs::recursive_directory_iterator(); ++it)
        written |= it->path().filename() == "dry.lua";
    CHECK_FALSE(written);
    // A real press still does it.
    CHECK(h.scripts->press_button(press("mq.bind('/drybind', function() end)")));
    CHECK(h.hooks.has_command("/drybind"));
}

TEST_CASE("MQ Lua: caps_of is list()'s caps for a running or paused script, 0 for any other name") {
    Host h(plugin::kCapRead | plugin::kCapCommands);
    h.write("c.lua", "require('mq').delay(100000)\n");
    REQUIRE(h.scripts->run("c", {}));
    h.pulses(1);
    CHECK(h.scripts->caps_of("c") == (plugin::kCapRead | plugin::kCapCommands));
    REQUIRE(h.scripts->list().size() == 1);
    CHECK(h.scripts->caps_of("c") == h.scripts->list()[0].caps);
    h.scripts->pause("c", true);
    CHECK(h.scripts->caps_of("c") == (plugin::kCapRead | plugin::kCapCommands));
    CHECK(h.scripts->caps_of("C") == 0u); // the tag's name exactly, as plugin_caps compared it
    CHECK(h.scripts->caps_of("other") == 0u);
    h.scripts->stop("c");
    CHECK(h.scripts->caps_of("c") == 0u);
}

TEST_CASE("MQ Lua: a window that starts and stops other scripts while the list is drawn") {
    ImGuiFrame frame;
    Host h;
    h.write("launch.lua", R"(
local mq = require('mq')
local frame = 0
mq.imgui.init('L', function()
  frame = frame + 1
  if frame == 1 then mq.cmd('/lua stop b') end
  if frame == 2 then mq.cmd('/lua run d') mq.cmd('/lua run e') end
end)
mq.delay(100000)
)");
    for (const char* n : {"b", "c", "d", "e"})
        h.write(std::string(n) + ".lua", std::string("local mq = require('mq') mq.imgui.init('") + n +
                                             "', function() print('drew " + n + "') end) mq.delay(100000)\n");
    REQUIRE(h.scripts->run("launch", {}));
    REQUIRE(h.scripts->run("b", {}));
    REQUIRE(h.scripts->run("c", {}));
    h.pulses(1);
    h.scripts->imgui(); // the stop removes nothing from the list being drawn
    CHECK_FALSE(h.scripts->running("b"));
    CHECK(h.scripts->running("c"));
    CHECK(h.printed_line("drew c"));
    CHECK_FALSE(h.printed_line("drew b"));
    std::size_t listed = 0;
    for (const auto& i : h.scripts->list()) listed += i.name == "b";
    CHECK(listed == 0); // reaped once the walk ended
    h.scripts->imgui(); // two more scripts while the list is walked
    CHECK(h.scripts->running("d"));
    CHECK(h.scripts->running("e"));
    h.pulses(1);
    h.scripts->imgui();
    CHECK(h.printed_line("drew d"));
    CHECK(h.printed_line("drew e"));
}

TEST_CASE("MQ Lua: a bind that runs its own command is stopped, not left to overflow the stack") {
    Host h;
    h.write("loop.lua", "local mq = require('mq') mq.bind('/loopme', function() mq.cmd('/loopme') end) mq.delay(100000)\n");
    h.write("other.lua", "local mq = require('mq') mq.delay(100000)\n");
    REQUIRE(h.scripts->run("loop", {}));
    REQUIRE(h.scripts->run("other", {}));
    h.pulses(1);
    h.run("/loopme");
    CHECK(printed_part(h, "ran each other more than 16 deep"));
    h.pulses(1);
    CHECK_FALSE(h.scripts->running("loop"));
    CHECK(h.scripts->running("other"));
}

TEST_CASE("MQ Lua: a bind's command with too many words is refused; the script runs on") {
    Host h;
    h.write("words.lua", "local mq = require('mq') mq.bind('/words', function(...) print('words', select('#', ...)) end)"
                         " mq.cmd('/words' .. string.rep(' w', 1001)) mq.cmd('/words a b') mq.delay(100000)\n");
    REQUIRE(h.scripts->run("words", {}));
    h.pulses(2);
    CHECK(printed_part(h, "too many words for its bind (1001) is refused"));
    CHECK(printed_part(h, "words 2"));
    CHECK(h.scripts->running("words"));
}

TEST_CASE("MQ Lua: the frame's pause never yields a coroutine the script made itself") {
    Host h;
    // Each producer runs well past the 3 ms slice between its yields.
    h.write("gen.lua", R"(
local mq = require('mq')
local function spin() local t = os.clock() while os.clock() - t < 0.02 do end end
local gen = coroutine.wrap(function() spin() coroutine.yield('done') end)
print('got', gen())
local n = 0
for v in coroutine.wrap(function() for i = 1, 3 do spin() coroutine.yield(i) end end) do n = n + v end
print('sum', n)
)");
    REQUIRE(h.scripts->run("gen", {}));
    for (int i = 0; i < 100 && h.scripts->running("gen"); ++i) h.pulses(1);
    CHECK(h.printed_line("got done"));
    CHECK(h.printed_line("sum 6"));
}

TEST_CASE("Lua socials: a `button` table with metamethods of the social's own is set raw") {
    Host h(0);
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapAll);
    auto p = press(R"(
if state.n then print('pressed', rawget(button, 'presses')) end
state.n = 1
button = setmetatable({}, {__newindex = function() error('x') end, __index = function() error('y') end})
)");
    REQUIRE(h.scripts->press_button(p));
    REQUIRE(h.scripts->press_button(p)); // set_button writes past __newindex, where an error could only abort
    CHECK(h.printed_line("pressed 2"));
}

TEST_CASE("MQ Lua: a script that ends with its heap full is stopped with its error, not an abort") {
    Host h;
    // Fills the 256 MB heap to within a few bytes, every byte reachable, then fails: the host's traceback needs memory.
    h.write("full.lua", R"(
local h
local n = 4096
while n >= 1 do
  pcall(function() while true do h = {h, string.rep('x', n)} end end)
  n = n // 2
end
pcall(function() while true do h = {h} end end)
error('bye')
)");
    REQUIRE(h.scripts->run("full", {}));
    for (int i = 0; i < 20000 && h.scripts->running("full"); ++i) h.pulses(1);
    CHECK_FALSE(h.scripts->running("full"));
    CHECK(printed_part(h, "[Lua] full ended with an error"));
}

TEST_CASE("MQ Lua: a script's error is logged once, with its traceback") {
    Host h;
    h.write("logonce.lua", "error('logonce: the one failure')\n");
    REQUIRE(h.scripts->run("logonce", {}));
    for (int i = 0; i < 10 && h.scripts->running("logonce"); ++i) h.pulses(1);
    CHECK_FALSE(h.scripts->running("logonce"));
    CHECK(printed_part(h, "[Lua] logonce ended with an error"));
    char recent[16 * 200 + 1];
    const std::size_t n = log::copy_recent_lines(recent, sizeof recent);
    const std::string_view lines(recent, n);
    std::size_t count = 0;
    for (std::size_t at = lines.find("logonce: the one failure"); at != std::string_view::npos;
         at = lines.find("logonce: the one failure", at + 1))
        ++count;
    CHECK(count == 1);
    CHECK(lines.find("stack traceback") != std::string_view::npos);
}

namespace {
bool printed_with(const Host& h, const std::string& start, const std::string& part) {
    return std::any_of(h.printed.begin(), h.printed.end(), [&](const std::string& l) {
        return l.rfind(start, 0) == 0 && l.find(part) != std::string::npos;
    });
}
std::size_t count_part(const std::vector<std::string>& lines, const std::string& part) {
    return static_cast<std::size_t>(
        std::count_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; }));
}
} // namespace

TEST_CASE("MQ Lua: mq.event holds a script's events, patterns and names to their limits") {
    Host h;
    h.write("ev.lua", R"(
local mq = require('mq')
local fn = function() end
print('long', pcall(mq.event, 'x', string.rep('a', 1025), fn))
print('name', pcall(mq.event, string.rep('n', 257), 'a', fn))
print('max', pcall(mq.event, 'y', string.rep('a', 1024), fn))
for i = 1, 255 do mq.event('e' .. i, 'p' .. i, fn) end
print('more', pcall(mq.event, 'z', 'q', fn))
print('again', pcall(mq.event, 'e1', 'the same name', fn))
)");
    REQUIRE(h.scripts->run("ev", {}));
    h.pulses(1);
    CHECK(printed_with(h, "long false", "a pattern is at most 1024 bytes"));
    CHECK(printed_with(h, "name false", "a name is at most 256 bytes"));
    CHECK(h.printed_line("max true"));
    CHECK(printed_with(h, "more false", "a script has at most 256 events"));
    CHECK(h.printed_line("again true")); // replacing one is no new event

    Host t;
    t.write("ev.lua", R"(
local mq = require('mq')
for i = 1, 32 do mq.event('b' .. i, string.rep('b', 1024), function() end) end
print('total', pcall(mq.event, 'b33', 'x', function() end))
print('smaller', pcall(mq.event, 'b1', 'x', function() end))
)");
    REQUIRE(t.scripts->run("ev", {}));
    t.pulses(1);
    CHECK(printed_with(t, "total false", "a script's patterns hold at most 32768 bytes"));
    CHECK(t.printed_line("smaller true"));
}

TEST_CASE("MQ Lua: another's line runs an event's handler with no more than that code may do") {
    Host h;
    h.write("watch.lua", R"(
local mq = require('mq')
mq.event('inv', "#1# tells you, 'invite'", function(line, who)
    mq.cmd('/sit')
    mq.delay(10)
    mq.cmd('/sit')
    print('handled', who, pcall(mq.bind, '/w2', function() end))
end)
while true do mq.doevents() mq.delay(10) end
)");
    REQUIRE(h.scripts->run("watch", {}));
    h.pulses(1);
    // A read-only add-on's print: the handler runs, but neither /sit is sent, nor before nor after its wait.
    plugin::ChatLine fake{"Bob tells you, 'invite'", 0, 0};
    fake.from_code = true;
    fake.origin_caps = plugin::kCapRead | plugin::kCapEvents;
    h.hooks.chat(fake);
    h.pulses(4, 10);
    CHECK(h.sent.empty());
    CHECK(count_part(h.printed, "/sit needs the server's 'move' permission") == 2);
    CHECK(printed_with(h, "handled Bob false", "'commands' permission"));
    // The server's line: the handler has the script's own.
    plugin::ChatLine real{"Ann tells you, 'invite'", 0, 0};
    h.hooks.chat(real);
    h.pulses(4, 10);
    CHECK(h.sent == std::vector<std::string>{"/sit", "/sit"});
    CHECK(h.printed_line("handled Ann true"));
    // A script with every capability writing it is no narrower than the handler: as the server's.
    plugin::ChatLine full{"Cid tells you, 'invite'", 0, 0};
    full.from_code = true;
    full.origin_caps = plugin::kCapAll;
    h.hooks.chat(full);
    h.pulses(4, 10);
    CHECK(h.sent.size() == 4u);
}

TEST_CASE("MQ Lua: a line written while a script's or plugin's code runs is marked as its") {
    plugin::Hooks hooks;
    net::ConnectionOptions opts;
    opts.hooks = &hooks;
    mq_test::World w("hailstone_mq_lua_origin", opts);
    std::vector<plugin::ChatLine> seen;
    hooks.on_chat(hooks.new_owner("watch"), [&](plugin::ChatLine& l) {
        seen.push_back(l);
        return plugin::Flow::Continue;
    });
    net::GameConnectionTestAccess::chat(*w.conn, "the server's");
    {
        plugin::CallerScope scope(plugin::kCapRead, "addon");
        w.conn->ui_message("the add-on's");
    }
    REQUIRE(seen.size() == 2u);
    CHECK_FALSE(seen[0].from_code);
    CHECK(seen[0].origin_caps == plugin::kCapAll);
    CHECK(seen[1].from_code);
    CHECK(seen[1].origin_caps == plugin::kCapRead);
}

TEST_CASE("MQ Lua: a bind another ran keeps its starter's capabilities after it waits") {
    Host h;
    h.write("a.lua", R"(
local mq = require('mq')
mq.bind('/dosit', function()
    mq.cmd('/sit')
    mq.delay(10)
    mq.cmd('/sit')
    print('after', pcall(mq.event, 'q', 'x', function() end))
end)
while true do mq.delay(10) end
)");
    REQUIRE(h.scripts->run("a", {}));
    h.pulses(1);
    {
        plugin::CallerScope scope(plugin::kCapCommands, "lesser");
        h.run("/dosit");
    }
    h.pulses(3, 10);
    CHECK(h.sent.empty());
    CHECK(count_part(h.printed, "/sit needs the server's 'move' permission") == 2);
    CHECK(printed_with(h, "after false", "'events' permission"));
    h.run("/dosit"); // the player's: the script's own
    h.pulses(3, 10);
    CHECK(h.sent == std::vector<std::string>{"/sit", "/sit"});
    CHECK(h.printed_line("after true"));
}

TEST_CASE("MQ Lua: /lua stop and pause reach only a script that may do no more than the caller") {
    Host h;
    h.write("big.lua", "local mq = require('mq') while true do mq.delay(10) end\n");
    REQUIRE(h.scripts->run("big", {}));
    h.pulses(1);
    {
        plugin::CallerScope scope(plugin::kCapRead | plugin::kCapCommands, "addon");
        h.run("/lua stop big");
        h.run("/lua pause big");
        h.run("/lua stop");
        h.run("/lua pause");
    }
    CHECK(h.scripts->running("big"));
    for (const auto& s : h.scripts->list())
        if (s.name == "big") CHECK(s.status == "RUNNING");
    CHECK(count_part(h.printed, "addon may not stop or pause big") == 2u);
    {
        plugin::CallerScope scope(plugin::kCapAll, "equal");
        h.run("/lua pause big");
    }
    for (const auto& s : h.scripts->list())
        if (s.name == "big") CHECK(s.status == "PAUSED");
    h.run("/lua stop big");
    CHECK_FALSE(h.scripts->running("big"));
}

TEST_CASE("MQ Lua: mq.bind does not take another script's command") {
    Host h;
    h.write("a.lua", "local mq = require('mq') mq.bind('/mine', function() print('a ran') end) while true do mq.delay(10) end\n");
    h.write("b.lua", R"(
local mq = require('mq')
print('b', pcall(mq.bind, '/mine', function() print('b ran') end))
while true do mq.delay(10) end
)");
    REQUIRE(h.scripts->run("a", {}));
    h.pulses(1);
    REQUIRE(h.scripts->run("b", {}));
    h.pulses(1);
    CHECK(printed_with(h, "b false", "/mine is lua:a's command"));
    h.run("/mine");
    h.pulses(1);
    CHECK(h.printed_line("a ran"));
    CHECK_FALSE(h.printed_line("b ran"));
}

TEST_CASE("MQ Lua: no two scripts share a config folder; an old shared name moves when it is only theirs") {
    CHECK(mq::script_folder_name("capped") == "capped");
    CHECK(mq::script_folder_name("a b") == "a b");
    CHECK(mq::script_folder_name("a_b") == "a_b");
    CHECK(mq::script_folder_name("a/b") == "a%2Fb");
    CHECK(mq::script_folder_name("50%") == "50%25");
    CHECK(mq::script_folder_name("x.") == "x%2E");
    CHECK(mq::script_folder_name("..") == ".%2E");
    CHECK(mq::script_folder_name("con") == "%63on");
    CHECK(mq::script_folder_name("caf\xc3\xa9") == "caf%C3%A9");
    CHECK(mq::script_folder_name("") == "%");

    Host h(plugin::kCapRead);
    // "my script"'s settings from before, in config/my_script/: no other script had that name, so they move.
    fs::create_directories(h.config_dir / "my_script");
    std::ofstream(h.config_dir / "my_script" / "Old.lua", std::ios::binary) << "return {v = 7}\n";
    h.write("my script.lua", "local mq = require('mq') print('old', dofile(mq.configDir .. '/Old.lua').v)\n");
    REQUIRE(h.scripts->run("my script", {}));
    h.pulses(1);
    CHECK(h.printed_line("old 7"));
    CHECK(fs::exists(h.config_dir / "my script" / "Old.lua"));
    CHECK_FALSE(fs::exists(h.config_dir / "a_b"));
    // "a b" and "a_b" both there: config/a_b is a_b's, and "a b" starts with a folder of its own.
    fs::create_directories(h.config_dir / "a_b");
    std::ofstream(h.config_dir / "a_b" / "Theirs.lua", std::ios::binary) << "return {v = 1}\n";
    h.write("a_b.lua", "print('a_b')\n");
    h.write("a b.lua", "local mq = require('mq') print('dir', mq.configDir) print('theirs', pcall(dofile, mq.configDir .. '/Theirs.lua'))\n");
    REQUIRE(h.scripts->run("a b", {}));
    h.pulses(1);
    CHECK(h.printed_line("dir " + (h.config_dir / "a b").string()));
    CHECK(printed_with(h, "theirs false", ""));
    CHECK(fs::exists(h.config_dir / "a_b" / "Theirs.lua"));
}

TEST_CASE("Lua socials: hailstone.server_dir in a dry run gives the folder and makes none") {
    Host h(0);
    plugin::Rules r;
    r.configured = true;
    r.social_mode = plugin::ButtonMode::Full;
    r.social_caps = plugin::kCapAll;
    r.social_faces = true;
    r.dir = h.world.dir / "servers" / "test";
    h.policy.set(r);
    h.scripts->policy_changed();
    auto d = h.scripts->dry_run_button(press("print('dir', hailstone.server_dir())"));
    CHECK(d.ok);
    // The social's name, "social 'Test'", written as a folder name: the quotes are kept, nothing is made.
    const fs::path dir = r.dir / "plugin-config" / "social 'Test'";
    CHECK(h.printed_line("dir " + dir.string()));
    CHECK_FALSE(fs::exists(r.dir / "plugin-config"));
    CHECK(h.scripts->press_button(press("hailstone.server_dir()")));
    CHECK(fs::is_directory(dir));
}

TEST_CASE("MQ Lua: the memory limit comes back after an error skipped a HostWork's end") {
    mq::Script s;
    alignas(mq::HostWork) unsigned char buf[sizeof(mq::HostWork)];
    {
        mq::ScriptRun run(s);
        CHECK(s.limited == 1);
        new (buf) mq::HostWork(s); // its destructor never runs, as when an error longjmps past it
        CHECK(s.limited == 0);
        mq::repair_limit(s); // the next instruction hook
        CHECK(s.limited == 1);
        new (buf) mq::HostWork(s);
    }
    CHECK(s.limited == 0); // the ScriptRun put back what it found
    mq::repair_limit(s);   // outside any run: nothing to repair
    CHECK(s.limited == 0);
    {
        mq::ScriptRun run(s);
        CHECK(s.limited == 1);
        mq::ScriptRun inner(s);
        CHECK(s.limited == 2);
    }
    CHECK(s.limited == 0);
    CHECK(s.runs == 0);
}

TEST_CASE("MQ commands: /docommand, /noparse and /squelch nest at most 16 deep; mq.cmd's line is at most 2047 bytes") {
    Host h;
    auto nest = [](int n, const std::string& tail) {
        std::string line;
        for (int i = 0; i < n; ++i) line += i % 3 == 0 ? "/docommand " : i % 3 == 1 ? "/noparse " : "/docommand ";
        return line + tail;
    };
    h.run(nest(16, "/echo deep enough"));
    CHECK(h.printed_line("deep enough"));
    h.run(nest(17, "/echo too deep"));
    CHECK_FALSE(h.printed_line("too deep"));
    CHECK(printed_part(h, "nest at most 16 deep"));
    // Without end: no stack overflow.
    std::string huge;
    for (int i = 0; i < 100000; ++i) huge += "/docommand ";
    h.run(huge + "/echo never");
    CHECK_FALSE(h.printed_line("never"));
    // Nested /noparse keeps the outer one's: the line is still not parsed after the inner one ends.
    h.run("/noparse /docommand /noparse /tell Groupie ${Me.PctHPs}");
    CHECK(h.sent.back() == "/tell Groupie ${Me.PctHPs}");

    h.write("nest.lua", R"(
local mq = require('mq')
print('long', pcall(mq.cmd, string.rep('/docommand ', 100000) .. '/echo x'))
mq.cmd(string.rep('/docommand ', 100) .. '/echo y')
mq.cmd(string.rep('/docommand ', 15) .. '/echo fits')
print('max', pcall(mq.cmd, '/echo ' .. string.rep('z', 2041)))
print('over', pcall(mq.cmd, '/echo ' .. string.rep('z', 2042)))
)");
    REQUIRE(h.scripts->run("nest", {}));
    h.pulses(1);
    CHECK(printed_part(h, "long false"));
    CHECK(printed_part(h, "mq.cmd: a command line is at most 2047 bytes"));
    CHECK_FALSE(h.printed_line("x"));
    CHECK_FALSE(h.printed_line("y"));
    CHECK(h.printed_line("fits"));
    CHECK(h.printed_line("max true"));
    CHECK(printed_part(h, "over false"));
}

TEST_CASE("MQ Lua: a runaway caught by pcall, xpcall or a coroutine of the script's own still stops it") {
    ImGuiFrame frame;
    for (const char* body : {
             "while true do pcall(function() while true do end end) end",
             "while true do xpcall(function() while true do end end, function(e) return e end) end",
             "while true do coroutine.resume(coroutine.create(function() while true do end end)) end",
             "while true do coroutine.resume(coroutine.create(function() while true do pcall(function() while true do end end) end end)) end",
             "while true do pcall(table.sort, {3, 2, 1, 4, 5}, function(a, b) while true do end end) end",
         }) {
        CAPTURE(body);
        Host h(plugin::kCapAll, nullptr, 200);
        h.write("spin.lua", std::string("local mq = require('mq')\nmq.imgui.init('Spin', function() ") + body +
                                " end)\nmq.delay(100000)\n");
        REQUIRE(h.scripts->run("spin", {}));
        h.pulses(1);
        h.scripts->imgui();
        CHECK(printed_part(h, "ran longer than 200 ms"));
        CHECK_FALSE(h.scripts->running("spin"));
    }
}

TEST_CASE("MQ Lua: pcall, xpcall and coroutine.resume still work, and mq.delay yields through pcall") {
    Host h;
    h.write("pc.lua", R"(
local mq = require('mq')
print('p', pcall(function(a, b) return a + b end, 2, 3))
print('e', select(2, pcall(error, 'boom', 0)))
print('x', xpcall(function() error('bad', 0) end, function(e) return 'handled ' .. e end))
local co = coroutine.create(function(a) local b = coroutine.yield(a + 1) return b * 2 end)
print('c', coroutine.resume(co, 1))
print('c', coroutine.resume(co, 5))
print('closed', coroutine.close(coroutine.create(function() end)))
print('d', pcall(function() mq.delay(10) return 'waited' end))
)");
    REQUIRE(h.scripts->run("pc", {}));
    h.pulses(3, 20);
    CHECK(h.printed_line("p true 5"));
    CHECK(h.printed_line("e boom"));
    CHECK(h.printed_line("x false handled bad"));
    CHECK(h.printed_line("c true 2"));
    CHECK(h.printed_line("c true 10"));
    CHECK(h.printed_line("closed true"));
    CHECK(h.printed_line("d true waited"));
}

TEST_CASE("Lua socials: a press that caught its runaway is stopped, and the next press runs again") {
    Host h;
    allow_buttons(h, plugin::ButtonMode::Press, plugin::kCapAll);
    auto p = press(R"(
if state.ran then mq.cmd('/sit') return end
state.ran = true
while true do pcall(function() while true do end end) end
)");
    CHECK_FALSE(h.scripts->press_button(p));
    CHECK(printed_part(h, "ran longer than 50 ms"));
    CHECK(h.sent.empty());
    CHECK(h.scripts->press_button(p));
    REQUIRE(h.sent.size() == 1);
    CHECK(h.sent.back() == "/sit");
}

TEST_CASE("Lua socials: hailstone.ui_function, option and menu are refused") {
    Host h;
    allow_buttons(h, plugin::ButtonMode::Full, plugin::kCapAll); // (full mode: a social may have 'ui')
    h.scripts->press_button(press("hailstone.ui_function('f', function() end)"));
    CHECK(printed_part(h, "hailstone.ui_function: a Lua social cannot add to the UI"));
    h.scripts->press_button(press("hailstone.option{name = 'o', get = function() return 0 end, set = function(v) end}"));
    CHECK(printed_part(h, "hailstone.option: a Lua social cannot add to the UI"));
    h.scripts->press_button(press("hailstone.menu('M', '/sit')"));
    CHECK(printed_part(h, "hailstone.menu: a Lua social cannot add to the UI"));
}

TEST_CASE("MQ Lua: a narrowed event handler whose own line raises its event again does not hold the client") {
    Host h;
    int fed = 0;
    // A line a read-only add-on writes each time the handler runs: the same event again, with the add-on's origin.
    h.hooks.add_command(h.hooks.new_owner("feeder"), "/feed", [&](std::string_view) {
        ++fed;
        plugin::ChatLine again{"Bob tells you, 'loop'", 0, 0};
        again.from_code = true;
        again.origin_caps = plugin::kCapRead | plugin::kCapEvents;
        h.hooks.chat(again);
    });
    h.write("loop.lua", R"(
local mq = require('mq')
mq.event('loop', "#1# tells you, 'loop'", function() mq.cmd('/feed') end)
while true do mq.doevents() mq.delay(10) end
)");
    REQUIRE(h.scripts->run("loop", {}));
    h.pulses(1);
    plugin::ChatLine first{"Bob tells you, 'loop'", 0, 0};
    first.from_code = true;
    first.origin_caps = plugin::kCapRead | plugin::kCapEvents;
    h.hooks.chat(first);
    h.pulses(2, 10); // each pulse ends: the doevents loop is the script's own Lua, which pauses after its slice
    CHECK(fed > 1);
    CHECK(h.scripts->running("loop"));
}

TEST_CASE("MQ Lua: no script may have a Lua social's name") {
    Host h;
    h.write("social 'test'.lua", "print('ran')\n");
    std::string why;
    CHECK_FALSE(h.scripts->run("social 'test'", {}, &why));
    CHECK(why.find("a script's name cannot start with") != std::string::npos);
    CHECK_FALSE(h.scripts->run("SOCIAL 'Test'", {}, &why));
    h.pulses(1);
    CHECK_FALSE(h.printed_line("ran"));
}
