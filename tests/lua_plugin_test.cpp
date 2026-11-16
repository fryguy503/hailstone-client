// A Lua script as a plugin (mq/lua_hailstone.cpp, docs/plugins.md "Lua plugins"): the hailstone module's server
// extension, packets, UI functions and readers, held to the script's capabilities, over the test world.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <fstream>

#include "core/ascii_case.h"
#include "game/server_ext_chain.h"
#include "game/spells.h"
#include "mq/commands.h"
#include "mq/lua_mq.h"
#include "net/social_packets.h"
#include "plugin/ui_bridge.h"
#include "mq_test_world.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Host {
    mq_test::World world{"hailstone_lua_plugin_test"};
    fs::path lua_dir = world.dir / "lua";
    plugin::Hooks hooks;
    plugin::Policy policy;
    mq::Registry reg;
    mq::Context ctx;
    game::ExtensionChain chain;
    plugin::UiBridge bridge;
    std::vector<std::string> printed, sent;
    std::vector<std::pair<plugin::Stream, net::AppPacket>> packets;
    std::int64_t clock = 0;
    float max_zoom = 50.0f;
    std::vector<std::string> plugin_names; // the compiled plugins ScriptEnv::plugin_named knows
    std::unique_ptr<mq::Commands> commands;
    std::unique_ptr<mq::ScriptHost> scripts;

    Host() {
        fs::create_directories(lua_dir);
        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        allow(plugin::kCapAll);
        mq::CommandEnv ce;
        ce.hooks = &hooks;
        ce.registry = &reg;
        ce.context = [this]() -> const mq::Context& { return ctx; };
        ce.echo = [this](const std::string& t) { printed.push_back(t); };
        ce.run = [this](const std::string& l) { run(l); };
        commands = std::make_unique<mq::Commands>(ce);
        mq::ScriptEnv se;
        se.hooks = &hooks;
        se.policy = &policy;
        se.registry = &reg;
        se.context = ce.context;
        se.print = ce.echo;
        se.run = ce.run;
        se.dirs = {lua_dir};
        se.config_dir = world.dir / "config";
        se.now_ms = [this] { return clock; };
        se.extensions = &chain;
        se.ui = &bridge;
        se.connection = [this] { return world.conn.get(); };
        se.send = [this](plugin::Stream s, const net::AppPacket& p) { packets.emplace_back(s, p); };
        se.max_camera_distance = [this] { return max_zoom; };
        se.set_max_camera_distance = [this](float d) { max_zoom = d; };
        se.plugin_named = [this](std::string_view n) {
            return std::any_of(plugin_names.begin(), plugin_names.end(), [&](const std::string& p) { return iequals(p, n); });
        };
        scripts = std::make_unique<mq::ScriptHost>(se);
    }
    ~Host() {
        scripts.reset();
        commands.reset();
    }
    void allow(std::uint32_t caps) {
        plugin::Rules r;
        r.configured = true;
        r.player_script_caps = caps;
        r.social_mode = plugin::ButtonMode::Off;
        r.dir = world.dir / "servers" / "test";
        policy.set(r);
    }
    void run(const std::string& line) {
        std::string l = line;
        if (hooks.command(l)) return;
        sent.push_back(l);
    }
    void write(const std::string& file, const std::string& text) {
        fs::create_directories((lua_dir / file).parent_path());
        std::ofstream(lua_dir / file, std::ios::binary) << text;
    }
    bool printed_part(const std::string& t) const {
        return std::any_of(printed.begin(), printed.end(), [&](const std::string& p) { return p.find(t) != std::string::npos; });
    }
};

constexpr const char* kPlugin = R"lua(
local hs = require('hailstone')
hs.set_player_classes(3)
hs.set_character_classes('Gennro', 0x8001)
hs.set_pet_list({{spawn = 300, class = 13}, {spawn = 301, class = 13}})
hs.set_flag('wide_item_links', true)
hs.set_flag('research_classes', true)
hs.set_flag('backstab_either_hand', true)
hs.set_flag('solo_adventure', true)
hs.set_vitals{max_hp = 1234}
hs.hooks{
  is_spellcaster = function() return true end,
  guild_member_classes = function(v) if v >= 1000 then return v - 1000 end return 0 end,
  label = function(id, text) if id == 42 then return 'lua label' end end,
  skill_gate = function(skill) if skill == 53 then return false end end,
  who_lines = function(who) return {'who: ' .. #who.entries, who.entries[1].name} end,
  item_level_line = function(id) if id == 7 then return 'LN' end end,
  hide_xtarget_slot = function(slot) return slot == 19 end,
  spell_level_needed = function(id, class) return 42 end,
  item_right_click = function(slot) return slot.slot == 33 end,
  reuse_focus_dropped = function(id) return id == 4498 end,
  map_spawn = function(s)
    if s.id == 10 then return {color = 0x123456, layer = 7, label = s.name .. '!'} end
    return s.id == 11
  end,
}
hs.on_packet('zone', 'in', 0x7e57, function(op, data) return true end)
hs.on_packet('zone', 'out', 0x7e58, function(op, data) return 'xyz' end)
mode = 2
hs.ui_function('info', function(a, t)
  local p = hs.player()
  return {echo = a, field = t and t.key, level = p.level, rat = hs.spawn(10).name, spell = hs.spell(11).name,
          spawns = #hs.spawns(), buff = hs.buff(1).spell_id, item = hs.item(24, 0).name}
end)
hs.option{name = 'mode', label = 'Mode', min = 0, max = 3, get = function() return mode end,
          set = function(v) mode = v end}
hs.menu('Ext', '/extlua')
hs.send_packet('zone', 0x1234, string.pack('<I4', 7))
mq.bind('/extoff', function() hs.set_active(false) end)
mq.bind('/exton', function() hs.set_active(true) end)
)lua";

} // namespace

TEST_CASE("Lua plugin: managed installed add-ons start disable update and remove without script commands") {
    Host h;
    const std::string first = "hailstone.hooks{nameplate_style=function() return {name='managed one'} end}";
    h.write("managed/init.lua", first);
    mq::ManagedScript a;
    a.name = "managed"; a.entry = h.lua_dir / "managed" / "init.lua";
    a.hash = sha256(first); a.revision = sha256(std::string_view("package one"));
    a.enabled = a.verified = true;
    h.policy.set_ceilings({{plugin::FileKind::Script, a.hash, plugin::kCapRead | plugin::kCapUi | plugin::kCapExtend}});
    h.scripts->set_managed_addons({a});
    h.scripts->pulse();
    REQUIRE(h.scripts->running("managed"));
    CHECK(h.scripts->caps_of("managed") == (plugin::kCapRead | plugin::kCapUi | plugin::kCapExtend));
    auto& c = *h.world.conn;
    const auto& rat = c.world().spawns.at(10);
    auto p = h.chain.nameplate_style(c, rat, {});
    REQUIRE(p); CHECK(p->name == "managed one");
    a.enabled = false; h.scripts->set_managed_addons({a});
    CHECK_FALSE(h.scripts->running("managed"));
    CHECK_FALSE(h.chain.nameplate_style(c, rat, {}));
    CHECK_FALSE(h.scripts->run("managed", {})); // ordinary autorun cannot bypass Add-ons disabled
    for (const auto* alias : {"managed/init", "managed/init.lua", "managed.lua", "managed/./init"})
        CHECK_FALSE(h.scripts->run(alias, {})); // all of these resolve to the installed init.lua
    a.enabled = true; h.scripts->set_managed_addons({a}); h.scripts->pulse();
    CHECK(h.scripts->running("managed"));
    CHECK_FALSE(h.scripts->run("managed/init", {})); // an enabled add-on has one canonical runtime owner
    const std::string second = "hailstone.hooks{nameplate_style=function() return {name='managed two'} end}";
    h.write("managed/init.lua", second);
    a.hash = sha256(second); a.revision = sha256(std::string_view("package two"));
    h.scripts->set_managed_addons({a}); h.scripts->pulse();
    p = h.chain.nameplate_style(c, rat, {});
    REQUIRE(p); CHECK(p->name == "managed two");
    h.scripts->set_managed_addons({});
    CHECK_FALSE(h.scripts->running("managed"));
    CHECK_FALSE(h.chain.nameplate_style(c, rat, {}));
}

TEST_CASE("Lua plugin: managed activation honors game-state policy source identity and modified records") {
    Host h;
    const std::string text = "hailstone.hooks{nameplate_style=function() return {name='managed'} end}";
    h.write("managed/init.lua", text);
    mq::ManagedScript a;
    a.name = "managed"; a.entry = h.lua_dir / "managed" / "init.lua";
    a.hash = sha256(text); a.revision = a.hash; a.enabled = a.verified = true;
    h.policy.clear();
    h.scripts->set_managed_addons({a}); h.scripts->pulse();
    CHECK_FALSE(h.scripts->running("managed"));
    h.allow(0); h.scripts->policy_changed(); h.scripts->pulse();
    CHECK_FALSE(h.scripts->running("managed"));
    h.allow(plugin::kCapAll); h.scripts->policy_changed(); h.scripts->pulse();
    CHECK(h.scripts->running("managed"));
    a.verified = false; h.scripts->set_managed_addons({a});
    CHECK_FALSE(h.scripts->running("managed"));
    CHECK_FALSE(h.scripts->run("managed", {}));
    a.verified = true;
    h.write("server/managed/init.lua", "print('a server-owned shadow must not be started as this add-on')");
    h.scripts->set_server_dir(h.lua_dir / "server");
    h.scripts->set_managed_addons({a}); h.scripts->pulse();
    CHECK_FALSE(h.scripts->running("managed"));
    h.scripts->set_server_dir({});
    a.hash = sha256(std::string_view("wrong expected bytes"));
    h.scripts->set_managed_addons({a}); h.scripts->pulse();
    CHECK_FALSE(h.scripts->running("managed"));
}

TEST_CASE("Lua plugin: adopting managed metadata also stops an already running path alias") {
    Host h;
    const std::string text = "hailstone.hooks{nameplate_style=function() return {name='alias'} end}";
    h.write("managed/init.lua", text);
    REQUIRE(h.scripts->run("managed/init", {}));
    h.scripts->pulse();
    REQUIRE(h.scripts->running("managed/init"));
    mq::ManagedScript a;
    a.name = "managed"; a.entry = h.lua_dir / "managed" / "init.lua";
    a.hash = sha256(text); a.revision = a.hash; a.enabled = false; a.verified = true;
    h.scripts->set_managed_addons({a});
    CHECK_FALSE(h.scripts->running("managed/init"));
    CHECK_FALSE(h.scripts->run("managed/init.lua", {}));
    a.enabled = true; h.scripts->set_managed_addons({a}); h.scripts->pulse();
    CHECK(h.scripts->running("managed"));
    CHECK_FALSE(h.scripts->running("managed/init"));
}

TEST_CASE("Lua plugin: require cannot load a managed entry under another script owner") {
    Host h;
    h.allow(plugin::kCapRead | plugin::kCapUi | plugin::kCapExtend);
    const std::string text = "hailstone.hooks{nameplate_style=function() return {name='managed'} end}";
    h.write("managed/init.lua", text);
    mq::ManagedScript a;
    a.name = "managed"; a.entry = h.lua_dir / "managed" / "init.lua";
    a.hash = sha256(text); a.revision = a.hash; a.enabled = false; a.verified = true;
    h.scripts->set_managed_addons({a});
    const std::string loader = R"lua(
assert(not pcall(require, 'managed'))
assert(not pcall(require, 'managed.init'))
print('managed module entry refused')
)lua";
    h.write("loader.lua", loader);
    REQUIRE(h.scripts->run("loader", {})); h.scripts->pulse();
    CHECK(h.printed_part("managed module entry refused"));
    a.enabled = true; h.scripts->set_managed_addons({a}); h.scripts->pulse();
    h.printed.clear();
    h.write("loader2.lua", loader);
    REQUIRE(h.scripts->run("loader2", {})); h.scripts->pulse();
    CHECK(h.printed_part("managed module entry refused"));
    CHECK(h.scripts->running("managed"));
}

TEST_CASE("Lua plugin: nameplate text hooks follow the script lifetime") {
    Host h;
    auto& c = *h.world.conn;
    const auto& rat = c.world().spawns.at(10);
    h.write("plate.lua", R"lua(
require('hailstone').hooks{
  nameplate = function(spawn, text)
    if spawn.npc and not spawn.corpse then return text .. ' [' .. spawn.level .. ']' end
  end,
}
)lua");
    REQUIRE(h.scripts->run("plate", {}));
    h.scripts->pulse();
    std::string text = "rat";
    h.chain.nameplate_text(c, rat, text);
    CHECK(text == "rat [2]");
    text = "corpse";
    h.chain.nameplate_text(c, c.world().spawns.at(13), text);
    CHECK(text == "corpse");
    h.scripts->stop("plate");
    text = "rat";
    h.chain.nameplate_text(c, rat, text);
    CHECK(text == "rat");
}

TEST_CASE("Lua plugin: the nameplate hook's spawn table is reused and refilled for every plate") {
    Host h;
    auto& c = *h.world.conn;
    h.write("plate_reuse.lua", R"lua(
local first
require('hailstone').hooks{
  nameplate = function(spawn, text)
    first = first or spawn
    return tostring(rawequal(first, spawn)) .. ':' .. spawn.id .. ':' .. spawn.name .. ':' .. tostring(spawn.corpse)
  end,
}
)lua");
    REQUIRE(h.scripts->run("plate_reuse", {}));
    h.scripts->pulse();
    const auto& rat = c.world().spawns.at(10);
    const auto& corpse = c.world().spawns.at(13);
    std::string a = "x", b = "x";
    h.chain.nameplate_text(c, rat, a);
    h.chain.nameplate_text(c, corpse, b);
    // The same table both times, and nothing of the first spawn left in the second.
    CHECK(a == "true:10:" + rat.name + ":false");
    CHECK(b == "true:13:" + corpse.name + ":true");
}

TEST_CASE("Lua plugin: nameplate presentation has bounded data and follows script lifetime") {
    Host h;
    auto& c = *h.world.conn;
    const auto& rat = c.world().spawns.at(10);
    h.write("plate_style.lua", R"lua(
local hs = require('hailstone')
assert(hs.nameplate_api_version == 2)
hs.hooks{nameplate_style = function(s, ctx)
  assert(ctx.target and not ctx.self and ctx.name == 'assembled rat')
  assert(ctx.hp_percent == 80 and ctx.mana_percent == nil and ctx.aggro_percent == nil)
  assert(ctx.distance == 50 and ctx.now_ms == 1000 and ctx.dt_ms == 16)
  assert(ctx.group_mark == 0 and ctx.raid_mark == 0 and ctx.casting == nil)
  return {name='rat', subtitle='NPC', width=160, height=14, mana_height=5, cast_height=8,
    font_scale=1.1, alpha=0.9, scale=1.2, offset_y=8, show_name=true, show_marker=false,
    stack=true,stack_gap=6,max_stack=128,texture='gradient',name_position='inside',text_align='right',border_width=3,rounding=5,
    health={value=ctx.hp_percent,color=0x12ff34,text='80%'},
    badges={{icon='eye',label='Sees invis',color=0xffa500,spell_icon=37},{icon='diamond',label='1'}}}
end}
mq.bind('/plate_off', function() hs.set_active(false) end)
mq.bind('/plate_on', function() hs.set_active(true) end)
)lua");
    REQUIRE(h.scripts->run("plate_style", {}));
    h.scripts->pulse();
    game::NameplateContext context;
    context.name = "assembled rat";
    context.distance = 50.0f;
    context.now_ms = 1000.0;
    context.dt_ms = 16.0;
    auto p = h.chain.nameplate_style(c, rat, context);
    REQUIRE(p);
    CHECK(p->name == "rat");
    CHECK(p->subtitle == "NPC");
    CHECK(p->width == 160.0f);
    CHECK(p->height == 14.0f);
    CHECK_FALSE(p->show_marker);
    CHECK(p->stack);
    CHECK(p->stack_gap == 6.0f);
    CHECK(p->max_stack == 128.0f);
    CHECK(p->texture == "gradient");
    CHECK(p->name_position == "inside");
    CHECK(p->text_align == "right");
    CHECK(p->border_width == 3.0f);
    CHECK(p->rounding == 5.0f);
    REQUIRE(p->health);
    CHECK(p->health->color == 0x12ff34);
    CHECK(p->health->value == 80.0f);
    CHECK(p->health->text == "80%");
    CHECK_FALSE(p->mana);
    REQUIRE(p->badges.size() == 2);
    CHECK(p->badges[0].icon == "eye");
    REQUIRE(p->badges[0].spell_icon);
    CHECK(*p->badges[0].spell_icon == 37);
    CHECK_FALSE(p->badges[1].spell_icon);
    h.run("/plate_off");
    h.scripts->pulse();
    CHECK_FALSE(h.chain.nameplate_style(c, rat, context));
    h.run("/plate_on");
    h.scripts->pulse();
    CHECK(h.chain.nameplate_style(c, rat, context));
    h.scripts->stop("plate_style");
    CHECK_FALSE(h.chain.nameplate_style(c, rat, context));
}

TEST_CASE("Lua plugin: malformed nameplate presentation always leaves the normal plate") {
    for (const std::string expression : {
        "nil", "false", "42", "'text'", "{width=0/0}", "{alpha=math.huge}", "{width=513}",
        "{show_name='yes'}", "{stack=1}", "{stack_gap=-1}", "{stack_gap=21}", "{max_stack=201}",
        "{texture='bogus'}", "{frame_style='picture'}", "{target_style='sparkles'}", "{badge_position='center'}",
        "{health_text_align='middle'}", "{cast_text_align=false}", "{badge_scale=0.4}", "{badge_scale=2.6}",
        "{cast_gap=-1}", "{cast_gap=25}", "{cast_width=601}", "{cast_width=math.huge}",
        "{through_walls=1}", "{clickable='yes'}",
        "{name_position='top'}", "{text_align='middle'}", "{rounding=13}", "{border_width=-1}",
        "{health={value=-1}}", "{health={value=101}}", "{health={color=1}}",
        "{text_color=-1}", "{text_color=0x1000000}", "{text_color=1.5}",
        "{name=string.rep('x',257)}", "{name='a\\0b'}", "{subtitle='a\\nb'}",
        "{health={value=50,text=string.rep('x',129)}}", "{badges={{icon='invalid'}}}",
        "{badges={{icon='eye',label=string.rep('x',33)}}}", "{badges={1}}",
        "{badges={{icon='circle',spell_icon=-1}}}", "{badges={{icon='circle',spell_icon=65536}}}",
        "{badges={{icon='circle',spell_icon=1.5}}}", "{badges={{icon='circle',spell_icon='1'}}}",
        "{badges={{icon='eye'},{icon='eye'},{icon='eye'},{icon='eye'},{icon='eye'},"
                   "{icon='eye'},{icon='eye'},{icon='eye'},{icon='eye'}}}",
        "setmetatable({}, {__index=function() error('must not run') end})",
        "{health=setmetatable({value=50}, {__index=function() error('must not run') end})}",
        "{badges=setmetatable({}, {__len=function() error('must not run') end})}",
        "{badges={setmetatable({icon='eye'}, {__index=function() error('must not run') end})}}"
    }) {
        CAPTURE(expression);
        Host h;
        auto& c = *h.world.conn;
        h.write("bad_plate.lua", "require('hailstone').hooks{nameplate_style=function() return " + expression + " end}");
        REQUIRE(h.scripts->run("bad_plate", {}));
        h.scripts->pulse();
        CHECK_FALSE(h.chain.nameplate_style(c, c.world().spawns.at(10), {}));
        CHECK(h.scripts->running("bad_plate")); // invalid data is ignored; no metamethod runs
    }
}

TEST_CASE("Lua plugin: nameplate context only exposes known resources and active observed casts") {
    Host h;
    auto& c = *h.world.conn;
    auto& group = net::GameConnectionTestAccess::group(c);
    group.mana_percent[12] = 37;
    group.endurance_percent[12] = 65;
    auto& spells = net::GameConnectionTestAccess::spells(c);
    spells.others[12] = {200, net::GameConnection::now_ms(), 60000};
    h.write("known_plate.lua", R"lua(
require('hailstone').hooks{nameplate_style=function(s,c)
  if s.id == 12 then
    assert(c.in_group and not c.self and not c.target)
    assert(c.mana_percent == 37 and c.endurance_percent == 65)
    assert(c.casting.spell_id == 200 and c.casting.name == 'Minor Healing')
    assert(c.casting.duration_ms == 60000 and c.casting.elapsed_ms >= 0)
    return {mana={value=c.mana_percent},cast={value=50,text=c.casting.name}}
  end
  assert(c.mana_percent == nil and c.endurance_percent == nil and c.casting == nil)
  return {hide=true}
end}
)lua");
    REQUIRE(h.scripts->run("known_plate", {}));
    h.scripts->pulse();
    auto p = h.chain.nameplate_style(c, c.world().spawns.at(12), {});
    REQUIRE(p);
    REQUIRE(p->mana);
    CHECK(p->mana->value == 37.0f);
    REQUIRE(p->cast);
    CHECK(p->cast->text == "Minor Healing");
    auto absent = h.chain.nameplate_style(c, c.world().spawns.at(10), {});
    REQUIRE(absent);
    CHECK(absent->hide);
}

TEST_CASE("Lua plugin: presentation API 2 validates structural skins and explicit manual interaction") {
    Host h;
    h.write("crest_skin.lua", R"lua(
local hs=require('hailstone')
assert(hs.nameplate_api_version == 2)
hs.hooks{nameplate_style=function()
  return {frame_style='runic',texture='segments',target_style='chevrons',badge_position='left',
          badge_scale=2.5,cast_gap=24,cast_width=600,health_text_align='left',cast_text_align='right',
          through_walls=true,clickable=false}
end}
)lua");
    REQUIRE(h.scripts->run("crest_skin", {}));
    h.scripts->pulse();
    const auto& c = *h.world.conn;
    const auto p = h.chain.nameplate_style(c, c.world().spawns.at(10), {});
    REQUIRE(p);
    CHECK(p->frame_style == "runic");
    CHECK(p->texture == "segments");
    CHECK(p->target_style == "chevrons");
    CHECK(p->badge_position == "left");
    CHECK(p->badge_scale == 2.5f);
    CHECK(p->cast_gap == 24.0f);
    CHECK(p->cast_width == 600.0f);
    CHECK(p->health_text_align == "left");
    CHECK(p->cast_text_align == "right");
    CHECK(p->through_walls);
    CHECK_FALSE(p->clickable);
    h.scripts->stop("crest_skin");
    h.write("crest_defaults.lua", "require('hailstone').hooks{nameplate_style=function() return {} end}");
    REQUIRE(h.scripts->run("crest_defaults", {}));
    h.scripts->pulse();
    const auto defaults = h.chain.nameplate_style(c, c.world().spawns.at(10), {});
    REQUIRE(defaults);
    CHECK_FALSE(defaults->through_walls);
    CHECK(defaults->clickable);
    CHECK(defaults->frame_style == "solid");
}

TEST_CASE("Lua plugin: nameplate presentation needs read UI and extension capabilities") {
    for (const std::uint32_t caps : std::array<std::uint32_t, 3>{plugin::kCapExtend, plugin::kCapExtend | plugin::kCapRead,
                                                             plugin::kCapExtend | plugin::kCapUi}) {
        Host h;
        h.allow(caps);
        h.write("limited_plate.lua", "hailstone.hooks{nameplate_style=function() return {hide=true} end}");
        REQUIRE(h.scripts->run("limited_plate", {}));
        h.scripts->pulse();
        const auto& c = *h.world.conn;
        CHECK_FALSE(h.chain.nameplate_style(c, c.world().spawns.at(10), {}));
    }
}

TEST_CASE("Lua plugin: own nameplate context uses real vitals including an authoritative zero") {
    Host h;
    auto& c = *h.world.conn;
    auto& status = net::GameConnectionTestAccess::status(c);
    status.mana = 0;
    auto& cast = net::GameConnectionTestAccess::spells(c).casting;
    cast.spell = 200;
    cast.cast_time = 60000;
    cast.eta_ms = net::GameConnection::now_ms() + cast.cast_time;
    h.write("own_plate.lua", R"lua(
hailstone.hooks{nameplate_style=function(s,c)
  assert(c.self and c.in_group and not c.target)
  assert(c.hp_percent == 75 and c.mana_percent == 0 and c.endurance_percent == 50)
  assert(c.casting.spell_id == 200 and c.casting.duration_ms == 60000)
  return {health={value=c.hp_percent},mana={value=c.mana_percent}}
end}
)lua");
    REQUIRE(h.scripts->run("own_plate", {}));
    h.scripts->pulse();
    auto p = h.chain.nameplate_style(c, c.world().spawns.at(1), {});
    REQUIRE(p);
    REQUIRE(p->mana);
    CHECK(p->mana->value == 0.0f);
}

TEST_CASE("Lua plugin: a removed actor cannot leave resources casts or buffs on a reused spawn id") {
    Host h;
    auto& c = *h.world.conn;
    auto& group = net::GameConnectionTestAccess::group(c);
    auto& spells = net::GameConnectionTestAccess::spells(c);
    group.mana_percent[12] = 37;
    group.endurance_percent[12] = 65;
    spells.others[12] = {200, net::GameConnection::now_ms(), 60000};
    spells.target_buffs_entity = 12;
    game::BuffSlot buff;
    buff.spell = 200;
    spells.target_buffs.emplace_back(0, buff);
    c.receive_zone_packet_for_test(net::encode(net::AggroTargetInfo{0, 12}));
    net::AggroUpdate update;
    update.entries = {{net::kAggroPlayer, 75}};
    c.receive_zone_packet_for_test(net::encode(update));
    c.receive_zone_packet_for_test(net::encode_delete_spawn(12));
    CHECK_FALSE(group.mana_percent.contains(12));
    CHECK_FALSE(group.endurance_percent.contains(12));
    CHECK_FALSE(spells.others.contains(12));
    CHECK(spells.target_buffs.empty());
    CHECK(spells.target_buffs_entity == 0);
    CHECK_FALSE(c.aggro().received[net::kAggroPlayer]);
    h.world.add(12, "Replacement", false, 50, {0, 0, 0});
    h.write("reused_plate.lua", R"lua(
hailstone.hooks{nameplate_style=function(s,c)
  assert(c.mana_percent == nil and c.endurance_percent == nil and c.casting == nil and c.aggro_percent == nil)
  assert(not mq.TLO.Spawn(s.id).BuffsPopulated())
  return {name='safe'}
end}
)lua");
    REQUIRE(h.scripts->run("reused_plate", {}));
    h.scripts->pulse();
    CHECK(h.chain.nameplate_style(c, c.world().spawns.at(12), {}));
}

TEST_CASE("Lua plugin: nameplate aggro distinguishes authoritative zero and target changes") {
    Host h;
    auto& c = *h.world.conn;
    h.write("aggro_plate.lua", R"lua(
hailstone.hooks{nameplate_style=function(s,c)
  return {subtitle=c.aggro_percent == nil and 'unknown' or tostring(c.aggro_percent)}
end}
)lua");
    REQUIRE(h.scripts->run("aggro_plate", {}));
    h.scripts->pulse();
    const auto& rat = c.world().spawns.at(10);
    const auto& fippy = c.world().spawns.at(11);
    auto subtitle = [&](const game::Spawn& spawn) {
        auto p = h.chain.nameplate_style(c, spawn, {});
        REQUIRE(p);
        REQUIRE(p->subtitle);
        return *p->subtitle;
    };
    CHECK(subtitle(rat) == "unknown");
    c.receive_zone_packet_for_test(net::encode(net::AggroTargetInfo{0, 10}));
    CHECK(subtitle(rat) == "unknown");
    net::AggroUpdate update;
    update.entries = {{net::kAggroPlayer, 75}};
    c.receive_zone_packet_for_test(net::encode(update));
    CHECK(subtitle(rat) == "75.0");
    CHECK(subtitle(fippy) == "unknown");
    // A new target's target-info precedes its own resource update. The old 75 must never appear there.
    c.receive_zone_packet_for_test(net::encode(net::AggroTargetInfo{0, 11}));
    CHECK(subtitle(fippy) == "unknown");
    CHECK(subtitle(rat) == "unknown");
    update.entries = {{net::kAggroPlayer, 0}};
    c.receive_zone_packet_for_test(net::encode(update));
    CHECK(subtitle(fippy) == "0.0");
    c.receive_zone_packet_for_test(net::encode(net::AggroTargetInfo{0, 11}));
    CHECK(subtitle(fippy) == "0.0"); // repeat target info keeps a valid reading
    c.receive_zone_packet_for_test(net::encode(net::AggroTargetInfo{0, 0}));
    CHECK(subtitle(fippy) == "unknown");
}

TEST_CASE("Lua plugin: a borrowed nameplate spawn table cannot poison the next refill") {
    Host h;
    h.write("poison_plate.lua", R"lua(
hailstone.hooks{nameplate_style=function(s,c)
  setmetatable(s,{__newindex=function() error('must not run') end})
  s.name=nil
  return {name='safe'}
end}
)lua");
    REQUIRE(h.scripts->run("poison_plate", {}));
    h.scripts->pulse();
    const auto& c = *h.world.conn;
    CHECK(h.chain.nameplate_style(c, c.world().spawns.at(10), {}));
    CHECK(h.chain.nameplate_style(c, c.world().spawns.at(12), {}));
    CHECK(h.scripts->running("poison_plate"));
}

TEST_CASE("Lua plugin: the hailstone module's extension, packets, UI and readers; it stays after its main chunk") {
    Host h;
    net::GameConnection& c = *h.world.conn;
    h.write("extplug/init.lua", kPlugin);
    REQUIRE(h.scripts->run("extplug", {}));
    h.scripts->pulse();
    CHECK(h.scripts->running("extplug")); // its main chunk returned; what it gave the client keeps it

    // Setters and switches.
    CHECK(c.server_class_mask() == 3);
    CHECK(c.classes_mask("Gennro") == 0x8001);
    REQUIRE(c.pets().list.size() == 2);
    CHECK(h.chain.wide_item_links(c));
    CHECK(h.chain.research_classes(c));
    CHECK_FALSE(h.chain.focus_classes(c));
    CHECK(h.chain.backstab_either_hand(c));
    CHECK(h.chain.solo_adventure(c));
    CHECK(h.chain.character_vitals(c).max_hp == 1234);

    // Hooks.
    CHECK(h.chain.is_spellcaster(c) == true);
    CHECK(h.chain.guild_member_classes(c, 1003) == 3);
    std::string label = "stock";
    h.chain.label_text(c, 42, label);
    CHECK(label == "lua label");
    label = "stock";
    h.chain.label_text(c, 1, label);
    CHECK(label == "stock");
    CHECK(h.chain.hide_xtarget_slot(c, 19));
    CHECK_FALSE(h.chain.hide_xtarget_slot(c, 2));
    game::SpellData disc;
    disc.id = 4498;
    CHECK(h.chain.reuse_focus_dropped(c, disc));
    disc.id = 200;
    CHECK_FALSE(h.chain.reuse_focus_dropped(c, disc));
    // map_spawn: a table changes the mark, true keeps the defaults, false hides.
    game::MapSpawnMark d;
    d.rgb = 0xff0000;
    d.label = "x";
    const auto& spawns = h.world.conn->world().spawns;
    auto rat = h.chain.map_spawn(c, spawns.at(10), d);
    REQUIRE(rat);
    CHECK(rat->rgb == 0x123456u);
    CHECK(rat->layer == 3);
    CHECK(rat->label == "a_rat01!");
    auto fippy = h.chain.map_spawn(c, spawns.at(11), d);
    REQUIRE(fippy);
    CHECK(fippy->rgb == 0xff0000u);
    CHECK(fippy->label == "x");
    CHECK_FALSE(h.chain.map_spawn(c, spawns.at(12), d));
    game::SpellData s7, s8;
    s7.id = 7;
    s8.id = 8;
    CHECK(h.chain.spell_level_needed(c, s7, 2) == 42);
    auto line = h.chain.level_needed_line(c);
    REQUIRE(line);
    CHECK(line(s7) == "LN");
    CHECK_FALSE(line(s8)); // the stock line
    const game::SkillRules* rules = h.chain.skill_rules(c);
    REQUIRE(rules);
    CHECK(rules->gate(rules->ctx, 53, 1, 50) == false);
    CHECK_FALSE(rules->gate(rules->ctx, 10, 1, 50).has_value());
    net::WhoResponse who;
    net::WhoEntry e;
    e.name = "Bob";
    who.entries.push_back(e);
    auto lines = h.chain.who_lines(c, who);
    REQUIRE(lines);
    CHECK(*lines == std::vector<std::string>{"who: 1", "Bob"});
    CHECK(h.chain.item_right_click(c, net::ItemSlot::possessions(net::kSlotCursor)));
    CHECK_FALSE(h.chain.item_right_click(c, net::ItemSlot::possessions(net::kSlotGeneral1)));

    // Packets: one dropped, one rewritten, one sent.
    net::AppPacket in(0x7e57), out(0x7e58, std::vector<std::byte>(4));
    CHECK(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, in) == plugin::Flow::Drop);
    CHECK(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::Out, out) == plugin::Flow::Continue);
    CHECK(out.data.size() == 3);
    REQUIRE(h.packets.size() == 1);
    CHECK(h.packets[0].second.opcode == 0x1234);
    CHECK(h.packets[0].second.data.size() == 4);

    // UI: a function the windows call, with the readers inside; an option; a menu entry.
    plugin::UiValue table;
    table.kind = plugin::UiValue::Kind::Table;
    table.fields.emplace_back("key", plugin::UiValue::string("k"));
    auto r = h.bridge.call("extplug", "info", {plugin::UiValue::string("hello"), table});
    REQUIRE(r);
    REQUIRE(r->field("echo"));
    CHECK(r->field("echo")->s == "hello");
    CHECK(r->field("field")->s == "k");
    CHECK(r->field("level")->i == 50);
    CHECK(r->field("rat")->s == "a_rat01");
    CHECK(r->field("spell")->s == "Holy Armor");
    CHECK(r->field("spawns")->i == 5);
    CHECK(r->field("buff")->i == 11);
    CHECK(r->field("item")->s == "Healing Potion");
    REQUIRE(h.bridge.options().size() == 1);
    CHECK(h.bridge.options()[0].get() == 2);
    h.bridge.options()[0].set(3);
    CHECK(h.bridge.options()[0].get() == 3);
    REQUIRE(h.bridge.menu().size() == 1);
    CHECK(h.bridge.menu()[0].command == "/extlua");

    // Inactive, then active again (from its own commands).
    h.run("/extoff");
    h.scripts->pulse();
    CHECK_FALSE(h.chain.wide_item_links(c));
    CHECK_FALSE(h.chain.is_spellcaster(c));
    h.run("/exton");
    h.scripts->pulse();
    CHECK(h.chain.is_spellcaster(c) == true);

    // Stopped: everything it gave the client goes.
    h.scripts->stop("extplug");
    CHECK(h.chain.empty());
    CHECK_FALSE(h.bridge.has_plugin("extplug"));
    CHECK(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, in) == plugin::Flow::Continue);
    CHECK_FALSE(rules->gate(rules->ctx, 53, 1, 50).has_value());
    CHECK_FALSE(line(s7));
}

TEST_CASE("Lua plugin: hailstone.player's HP is Cur_HP / Max_HP, the items' HP included") {
    Host h;
    // 750 / 1000 from the server; the player's items add 250 (net::GameConnection::own_hp).
    h.world.conn->set_own_hp_items(250, 250);
    h.write("hp.lua", R"lua(
local p = require('hailstone').player()
print('[hp] ' .. p.cur_hp .. '/' .. p.max_hp)
)lua");
    REQUIRE(h.scripts->run("hp", {}));
    h.scripts->pulse();
    CHECK(h.printed_part("[hp] 1000/1250"));
}

TEST_CASE("Lua plugin: held to its capabilities; an error or a runaway in a hook ends it") {
    {
        Host h;
        h.allow(plugin::kCapRead | plugin::kCapCommands);
        h.write("noext.lua", "require('hailstone').hooks{ is_spellcaster = function() return true end }\n");
        REQUIRE(h.scripts->run("noext", {}));
        h.scripts->pulse();
        CHECK(h.printed_part("needs the server's 'extend' permission"));
        CHECK(h.chain.empty());
    }
    {
        Host h;
        h.write("boom.lua", "hailstone.hooks{ is_spellcaster = function() error('boom') end }\n");
        REQUIRE(h.scripts->run("boom", {}));
        h.scripts->pulse();
        REQUIRE(h.scripts->running("boom"));
        CHECK_FALSE(h.chain.is_spellcaster(*h.world.conn)); // the stock answer
        CHECK(h.printed_part("boom"));
        h.scripts->pulse();
        CHECK_FALSE(h.scripts->running("boom"));
        CHECK(h.chain.empty());
    }
    {
        Host h;
        h.write("spin.lua", "hailstone.hooks{ cast_class = function() while true do end end }\n");
        REQUIRE(h.scripts->run("spin", {}));
        h.scripts->pulse();
        CHECK(h.chain.cast_class(*h.world.conn) == 0);
        CHECK(h.printed_part("ran longer than 50 ms"));
    }
}

TEST_CASE("Lua plugin: a hook's number beyond an integer (NaN, math.huge) is no answer") {
    Host h;
    h.write("huge.lua", R"(
local answers = {[1] = math.huge, [2] = 0/0, [3] = -1e300, [4] = 12.75}
hailstone.hooks{ discipline_reuse = function(id) return answers[id] end }
)");
    REQUIRE(h.scripts->run("huge", {}));
    h.scripts->pulse();
    for (std::int32_t id : {1, 2, 3}) {
        INFO(id);
        CHECK_FALSE(h.chain.discipline_reuse_timer(*h.world.conn, id));
    }
    CHECK(h.chain.discipline_reuse_timer(*h.world.conn, 4) == 12u); // a float in range is cut, as before
    CHECK(h.scripts->running("huge"));
}

TEST_CASE("Lua plugin: its folder's ui/ is its windows for each pack") {
    Host h;
    h.write("withui/init.lua", "-- windows only\n");
    h.write("withui/ui/modern/w.lua", "ui.window('W', function() end)\n");
    REQUIRE(h.scripts->run("withui", {}));
    h.scripts->pulse();
    CHECK(h.scripts->running("withui")); // its windows keep it
    REQUIRE(h.bridge.folders().size() == 1);
    CHECK(h.bridge.folders()[0].second == h.lua_dir / "withui");
    h.scripts->stop("withui");
    CHECK(h.bridge.folders().empty());
}

TEST_CASE("Lua plugin: hailstone.server_rule reads the server's own sections of hailstone.cfg") {
    Host h;
    plugin::Rules r = plugin::parse_server_cfg("[scripts]\nplayer = on\n[autocombat]\nhunter = off\nmax_pull = 120\n");
    h.policy.set(r);
    h.write("rules.lua", R"lua(
local hs = require('hailstone')
print('[rules] ' .. tostring(hs.server_rule('autocombat', 'hunter')) .. ' ' .. tostring(hs.server_rule('AutoCombat', 'Max_Pull'))
      .. ' ' .. tostring(hs.server_rule('autocombat', 'assist')) .. ' ' .. tostring(hs.server_rule('scripts', 'player')))
)lua");
    REQUIRE(h.scripts->run("rules", {}));
    h.scripts->pulse();
    CHECK(h.printed_part("[rules] off 120 nil nil")); // the client's own sections are not offered
    // A server without a hailstone.cfg has none.
    h.policy.set(plugin::Rules{});
    REQUIRE(h.scripts->run("rules", {}));
    h.scripts->pulse();
    CHECK(h.printed_part("[rules] nil nil nil nil"));
}

TEST_CASE("Lua plugin: a bad argument or a raising table is an error; nothing is half made") {
    // (Lua is built as C: each error is a longjmp, so these are read before a C++ object exists. A leak checker,
    // -DHAILSTONE_SANITIZE=ON, is what sees a string or a reference left behind.)
    Host h;
    h.write("bad.lua", R"lua(
local hs = require('hailstone')
local raising = setmetatable({}, {__index = function(_, k) error('no ' .. k) end})
print('[bad] option', pcall(hs.option, {name = 'x', get = function() return 0 end}))
print('[bad] option2', pcall(hs.option, setmetatable({name = 'y', get = function() return 0 end}, getmetatable(raising))))
print('[bad] rule', pcall(hs.server_rule, 'autocombat'))
print('[bad] pets', pcall(hs.set_pet_list, {{spawn = 1, class = 2}, raising}))
hs.set_pet_list({{spawn = 300, class = 13}, setmetatable({}, {__index = {spawn = 301, class = 14}})})
print('[bad] done')
)lua");
    REQUIRE(h.scripts->run("bad", {}));
    h.scripts->pulse();
    CHECK(h.printed_part("[bad] option false"));
    CHECK(h.printed_part("[bad] option2 false"));
    CHECK(h.printed_part("[bad] rule false"));
    CHECK(h.printed_part("[bad] pets false"));
    CHECK(h.printed_part("no spawn"));
    CHECK(h.printed_part("[bad] done"));
    CHECK(h.bridge.options().empty());
    // A table's __index still answers for an entry.
    const auto& pets = h.world.conn->pets().list;
    REQUIRE(pets.size() == 2);
    CHECK(pets[1].spawn_id == 301);
    CHECK(pets[1].class_id == 14);
}

TEST_CASE("Lua plugin: folders, zone names, actions and the camera") {
    Host h;
    net::GameConnection& c = *h.world.conn;
    c.set_pet_list({{300, 13}, {301, 13}});
    auto& ticket = net::GameConnectionTestAccess::ticket(c);
    ticket.server_name = "Triune World";
    ticket.world_host = "203.0.113.7";
    ServerProfile saved;
    saved.id = profile_id_for_world(ticket.server_name);
    saved.name = "Project Triune";
    saved.host = "login.example.test";
    REQUIRE(save_profile(saved, h.world.dir / "servers"));
    h.write("more.lua", R"lua(
local hs = require('hailstone')
local sv = hs.server()
print('[more] server_info ' .. sv.name .. '|' .. sv.host .. '|' .. sv.port .. '|' .. sv.world .. '|' .. sv.world_host)
hs.chat('[more] yellow', 15)
local p = hs.player()
print('[more] server ' .. tostring(hs.server_dir()))
print('[more] character ' .. tostring(hs.character_dir()))
print('[more] zone ' .. hs.zone_name(1) .. ' ' .. tostring(hs.zone_name(999999)))
print('[more] pet ' .. p.pet_id .. ' stand ' .. p.stand_state .. ' casting ' .. p.casting_spell_id)
hs.set_window_pet(301)
hs.clear_spell_recovery()
hs.close_spell_book()
hs.inspect_item(5001, {1, 2, 3, 4, 5, 6})
hs.set_max_camera_distance(90)
print('[more] camera ' .. math.floor(hs.max_camera_distance()))
)lua");
    REQUIRE(h.scripts->run("more", {}));
    h.scripts->pulse();
    bool yellow = false;
    while (auto e = c.poll_chat()) yellow = yellow || (e->text == "[more] yellow" && e->color == 15);
    CHECK(yellow);
    const fs::path server_dir = h.world.dir / "servers" / "test" / "plugin-config" / "more";
    CHECK(h.printed_part("[more] server " + server_dir.string()));
    CHECK(fs::is_directory(server_dir));
    // (the world's name gives the character a folder: servers/<profile>/characters/<world>/Tester/)
    CHECK(h.printed_part((fs::path("Tester") / "plugin-config" / "more").string()));
    CHECK(h.printed_part("[more] zone qeynos nil"));
    CHECK(h.printed_part("[more] pet 300 stand 100 casting 0"));
    CHECK(c.pets().focused == 301);
    CHECK(h.printed_part("[more] camera 90"));
    CHECK(h.printed_part("[more] server_info Project Triune|login.example.test|5997|Triune World|203.0.113.7"));

    // Without the actions permission, an action is an error that ends the script.
    Host n;
    n.allow(plugin::kCapRead | plugin::kCapCommands);
    n.write("noact.lua", "require('hailstone').close_spell_book()\n");
    REQUIRE(n.scripts->run("noact", {}));
    n.scripts->pulse();
    CHECK(n.printed_part("needs the server's 'actions' permission"));
}

TEST_CASE("Lua plugin: a UI function takes at most kMaxCallArgs arguments; its folders are its name as one folder") {
    Host h;
    h.write("odd%name.lua", R"lua(
local hs = require('hailstone')
hs.ui_function('count', function(...) return select('#', ...) end)
print('[odd] ' .. hs.server_dir())
)lua");
    REQUIRE(h.scripts->run("odd%name", {}));
    h.scripts->pulse();
    const std::vector<plugin::UiValue> most(plugin::UiBridge::kMaxCallArgs, plugin::UiValue::integer(1));
    auto r = h.bridge.call("odd%name", "count", most);
    REQUIRE(r);
    CHECK(r->i == static_cast<std::int64_t>(plugin::UiBridge::kMaxCallArgs));
    std::vector<plugin::UiValue> more = most;
    more.push_back(plugin::UiValue::integer(1));
    CHECK_FALSE(h.bridge.call("odd%name", "count", more));
    // '%' itself is written %25: no other name gives that folder.
    const fs::path dir = h.world.dir / "servers" / "test" / "plugin-config" / "odd%25name";
    CHECK(h.printed_part("[odd] " + dir.string()));
    CHECK(fs::is_directory(dir));
}

TEST_CASE("Lua plugin: a script with a compiled plugin's name does not get the plugin's plugin-config folder") {
    Host h;
    h.plugin_names = {"NMS"};
    h.write("nms.lua", "print('[nms] ' .. require('hailstone').server_dir())\n");
    REQUIRE(h.scripts->run("nms", {}));
    h.scripts->pulse();
    const fs::path base = h.world.dir / "servers" / "test" / "plugin-config";
    CHECK(h.printed_part("[nms] " + (base / "script%3Anms").string()));
    CHECK(fs::is_directory(base / "script%3Anms"));
    CHECK_FALSE(fs::exists(base / "nms"));
    CHECK_FALSE(fs::exists(base / "NMS"));
}

TEST_CASE("Lua plugin: after_combat_ability gets the spell id and whether it was used") {
    Host h;
    h.write("aca.lua", "hailstone.hooks{ after_combat_ability = function(id, used) print('aca', id, used) end }\n");
    REQUIRE(h.scripts->run("aca", {}));
    h.scripts->pulse();
    game::SpellData spell;
    spell.id = 717;
    h.chain.after_combat_ability(*h.world.conn, spell, true);
    CHECK(h.printed_part("aca 717 true"));
    CHECK(h.scripts->running("aca"));
}

TEST_CASE("Lua plugin: a hooks table with a metatable is read in a protected call") {
    {
        Host h;
        h.write("idx.lua", R"(
hailstone.hooks(setmetatable({}, {__index = function(t, k) if k == 'is_spellcaster' then return function() return true end end end}))
)");
        REQUIRE(h.scripts->run("idx", {}));
        h.scripts->pulse();
        CHECK(h.chain.is_spellcaster(*h.world.conn));
        CHECK(h.scripts->running("idx"));
    }
    {
        Host h;
        h.write("bad.lua", "hailstone.hooks(setmetatable({}, {__index = function(t, k) error('no ' .. k) end}))\n");
        REQUIRE(h.scripts->run("bad", {}));
        h.scripts->pulse();
        CHECK_FALSE(h.chain.is_spellcaster(*h.world.conn)); // an error there ends the script, not the client
        CHECK(h.printed_part("no is_spellcaster"));
        h.scripts->pulse();
        CHECK_FALSE(h.scripts->running("bad"));
    }
    {
        Host h; // an __index that stops its own script and still gives the hook: the hook is not called
        h.write("quit.lua", R"(
hailstone.hooks(setmetatable({}, {__index = function(t, k)
  mq.cmd('/lua stop quit')
  return function() print('hook ran') return true end
end}))
)");
        REQUIRE(h.scripts->run("quit", {}));
        h.scripts->pulse();
        CHECK_FALSE(h.chain.is_spellcaster(*h.world.conn));
        CHECK_FALSE(h.printed_part("hook ran"));
        h.scripts->pulse();
        CHECK_FALSE(h.scripts->running("quit"));
    }
}

TEST_CASE("Lua plugin: a script restarted by another during pulse keeps the UI it registers") {
    Host h;
    h.write("x.lua", "hailstone.ui_function('f', function() return 7 end)\n");
    h.write("restarter.lua", "mq.cmd('/lua stop x')\nmq.cmd('/lua run x')\n");
    REQUIRE(h.scripts->run("x", {}));
    h.scripts->pulse();
    REQUIRE(h.bridge.has_plugin("x"));
    REQUIRE(h.scripts->run("restarter", {}));
    h.scripts->pulse(); // the old x closes in this walk; the new one first runs next frame
    CHECK(h.scripts->running("x"));
    h.scripts->pulse();
    CHECK(h.bridge.has_plugin("x"));
    CHECK(h.bridge.call("x", "f", {}));
}

TEST_CASE("Lua plugin: a script that stops itself cannot run itself again until it has stopped") {
    Host h;
    h.write("self.lua", "hailstone.ui_function('f', function() return 1 end)\nmq.cmd('/lua stop self')\nmq.cmd('/lua run self')\n");
    REQUIRE(h.scripts->run("self", {}));
    h.scripts->pulse();
    CHECK(h.printed_part("self is still stopping"));
    CHECK_FALSE(h.scripts->running("self"));
    CHECK_FALSE(h.bridge.has_plugin("self"));
}

TEST_CASE("Lua plugin: two scripts that restart each other run one step a frame") {
    Host h;
    h.write("ping.lua", "mq.cmd('/lua stop pong')\nmq.cmd('/lua run pong')\n");
    h.write("pong.lua", "mq.cmd('/lua stop ping')\nmq.cmd('/lua run ping')\n");
    REQUIRE(h.scripts->run("ping", {}));
    for (int i = 0; i < 4; ++i) h.scripts->pulse(); // before, the first pulse never returned
    CHECK(h.scripts->list().size() <= 2);
}
