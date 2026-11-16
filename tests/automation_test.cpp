// app/automation.h as the client builds it: a script's hold on the Ability Book's autoskill
// (hailstone.hold_autoskill) lasts while it runs, and the player's list reaches it (hailstone.autoskill_list).
#include <doctest/doctest.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/automation.h"
#include "app/addon_install.h"
#include "mq/lua_mq.h"
#include "mq_test_world.h"
#include "core/sha256.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {
struct AutomationHost {
    mq_test::World world{"hailstone_automation_test"};
    std::unique_ptr<app::Automation> automation;
    std::vector<std::pair<mq::AbilityKind, std::string>> used;
    AutomationHost() {
        net::GameConnectionTestAccess::set_server_rules(*world.conn, plugin::Rules{}); // no hailstone.cfg: free
        app::AutomationEnv env;
        env.data_dir = world.dir / "data";
        env.install_dir = world.dir / "install";
        env.connection = [this] { return world.conn.get(); };
        env.autoskill_list = [] {
            game::AutoskillRule kick;
            kick.key = "skill:30";
            kick.min_endurance_pct = 20;
            kick.target_hp_below = 90;
            game::AutoskillRule mend;
            mend.key = "skill:32";
            mend.self_hp_below = 50;
            mend.when = game::AutoskillWhen::Always;
            mend.keep_buff = true;
            return std::vector<std::pair<game::AutoskillRule, std::string>>{{kick, "Kick"}, {mend, "Mend"}};
        };
        env.use_ability = [this](mq::AbilityKind kind, std::string_view name) {
            used.emplace_back(kind, std::string(name));
            return name != "None";
        };
        automation = std::make_unique<app::Automation>(std::move(env));
    }
    ~AutomationHost() { automation->shutdown(); }
    void write(const std::string& name, const std::string& text) {
        const fs::path file = world.dir / "data" / "scripts" / name;
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    std::vector<std::string> chat() {
        std::vector<std::string> out;
        while (auto e = world.conn->poll_chat()) out.push_back(e->text);
        return out;
    }
};
} // namespace

TEST_CASE("Automation: a script holds autoskill off while it runs; stopping it lets go") {
    AutomationHost h;
    h.automation->frame();
    CHECK_FALSE(h.automation->autoskill_held());
    h.write("holder.lua", "local mq = require('mq')\nrequire('hailstone').hold_autoskill(true)\n"
                          "while true do mq.delay(10) end\n");
    REQUIRE(h.automation->scripts().run("holder", {}));
    h.automation->frame();
    CHECK(h.automation->autoskill_held());
    h.automation->scripts().stop("holder");
    CHECK_FALSE(h.automation->autoskill_held());

    // Let go by the script itself.
    h.write("brief.lua", "local mq = require('mq')\nlocal hs = require('hailstone')\nhs.hold_autoskill(true)\n"
                         "mq.delay(0)\nhs.hold_autoskill(false)\nwhile true do mq.delay(0) end\n");
    REQUIRE(h.automation->scripts().run("brief", {}));
    h.automation->frame();
    CHECK(h.automation->autoskill_held());
    bool released = false;
    for (int i = 0; i < 10 && !released; ++i) {
        h.automation->frame();
        released = !h.automation->autoskill_held();
    }
    CHECK(released);
    CHECK(h.automation->scripts().running("brief"));
}

TEST_CASE("Automation: prepared enforcement identities are applied after profile and managed-install changes") {
    AutomationHost h;
    const fs::path data = h.world.dir / "data";
    const std::string original = "local tag = 0 while true do require('mq').delay(10) end\n";
    app::InstalledAddon addon;
    addon.name = "managed";
    addon.source = "owner/managed";
    addon.version = "1.0.0";
    addon.caps = plugin::kCapRead;
    addon.files = {{"init.lua", original.size(), sha256(original)}};
    fs::create_directories(app::addons_dir(data));
    REQUIRE(app::write_installed_addons(data, {addon}));
    h.write("managed/init.lua", original);
    auto change = [&](int tag) {
        const std::string code = "local tag = " + std::to_string(tag) +
                                 " while true do require('mq').delay(10) end\n";
        h.write("managed/extra.lua", code);
        h.write("copy.lua", code); // a renamed copy needs the scanned hash's source, not a runtime name match
    };
    change(1);
    h.automation->set_addon_ceilings(app::addon_ceilings(data));
    h.automation->frame();
    REQUIRE(h.automation->scripts().run("copy", {}));
    h.automation->frame();
    REQUIRE(h.automation->scripts().running("copy"));

    auto enforced = plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = allow\n"
                                             "block = owner/managed\n");
    enforced.dir = h.world.dir / "profile";
    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, enforced);
    h.automation->set_addon_ceilings(app::addon_ceilings(data, true)); // the completed worker snapshot
    h.automation->frame();
    CHECK_FALSE(h.automation->scripts().running("copy"));
    CHECK_FALSE(h.automation->scripts().run("copy", {}));

    // A managed update publishes another completed scan; Automation applies it without scanning the folders itself.
    change(2);
    h.automation->set_addon_ceilings(app::addon_ceilings(data, true));
    CHECK_FALSE(h.automation->scripts().run("copy", {}));

    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, plugin::Rules{});
    h.automation->frame();
    REQUIRE(h.automation->scripts().run("copy", {}));
    h.automation->frame();
    CHECK(h.automation->scripts().running("copy"));
    REQUIRE(h.automation->scripts().stop("copy"));
    change(3);
    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, enforced);
    h.automation->set_addon_ceilings(app::addon_ceilings(data, true));
    h.automation->frame();
    CHECK_FALSE(h.automation->scripts().run("copy", {}));

    auto closed = plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/managed\n");
    closed.dir = enforced.dir;
    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, closed);
    h.automation->frame();
    CHECK(h.automation->scripts().running("managed"));
    CHECK(h.automation->scripts().caps_of("managed") == plugin::kCapRead);
    CHECK_FALSE(h.automation->scripts().run("copy", {}));
}

TEST_CASE("Automation: recovery stays stopped across rule refreshes and retries explicitly") {
    AutomationHost h;
    const std::string code = "while true do require('mq').delay(10) end\n";
    h.write("needed/init.lua", code);
    auto rules = plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/needed\n");
    rules.dir = h.world.dir / "profile";
    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, rules);
    h.automation->set_addon_ceilings({{plugin::FileKind::Script, sha256(code), plugin::kCapRead,
                                      "needed", "owner/needed"}});
    h.automation->frame();
    REQUIRE(h.automation->required_addons_ready());
    CHECK(h.automation->has_required_addons());
    const auto serial = h.automation->required_addons_serial();
    for (int frame = 0; frame < 8; ++frame) {
        h.automation->frame();
        CHECK(h.automation->required_addons_ready());
        CHECK(h.automation->required_addons_serial() == serial);
    }
    h.automation->emergency_stop_required_addons();
    CHECK(h.automation->required_addons_stopped());
    CHECK_FALSE(h.automation->scripts().running("needed"));
    CHECK_FALSE(h.automation->required_addons_ready());
    CHECK(h.automation->required_addons_serial() != serial);
    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, rules);
    h.automation->frame();
    CHECK_FALSE(h.automation->scripts().running("needed"));
    h.automation->retry_required_addons();
    CHECK_FALSE(h.automation->required_addons_stopped());
    h.automation->frame();
    CHECK(h.automation->required_addons_ready());
    h.automation->emergency_stop_required_addons();
    // Resetting a connection's session does not run anything against its old rules.
    h.automation->reset_required_addons_session();
    CHECK_FALSE(h.automation->has_required_addons());
    CHECK_FALSE(h.automation->scripts().running("needed"));
    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, std::nullopt);
    h.automation->frame();
    CHECK(h.automation->required_addons_ready());
    CHECK_FALSE(h.automation->has_required_addons());
}

TEST_CASE("Automation: fresh enforcement waits for complete identities before any new start") {
    AutomationHost h;
    const auto data = h.world.dir / "data";
    const std::string needed = "while true do require('mq').delay(10) end\n";
    const std::string original = "-- original\n" + needed, copied = "-- changed code\n" + needed;
    app::InstalledAddon required;
    required.name = "needed";
    required.source = "owner/needed";
    required.version = "1.0.0";
    required.caps = plugin::kCapRead;
    required.files = {{"init.lua", needed.size(), sha256(needed)}};
    auto blocked = required;
    blocked.name = "blocked";
    blocked.source = "owner/blocked";
    blocked.files = {{"init.lua", original.size(), sha256(original)}};
    fs::create_directories(app::addons_dir(data));
    REQUIRE(app::write_installed_addons(data, {required, blocked}));
    h.write("needed/init.lua", needed);
    h.write("blocked/init.lua", original);
    h.write("blocked/extra.lua", copied);
    h.write("copy.lua", copied);
    h.write("new.lua", "-- new\n" + needed);
    h.automation->set_addon_ceilings(app::addon_ceilings(data));
    h.automation->frame();
    REQUIRE(h.automation->scripts().run("copy", {}));
    h.automation->frame();
    REQUIRE(h.automation->scripts().running("copy"));

    h.automation->set_addon_verification_pending(true);
    auto rules = plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\n"
                                          "required = owner/needed\nblock = owner/blocked\n");
    rules.dir = h.world.dir / "profile";
    net::GameConnectionTestAccess::set_server_rules(*h.world.conn, rules);
    h.automation->frame();
    CHECK(h.automation->scripts().running("copy")); // last trusted policy stays active during checking
    CHECK_FALSE(h.automation->scripts().running("needed"));
    std::string why;
    CHECK_FALSE(h.automation->scripts().run("new", {}, &why));
    CHECK(why.find("verification") != std::string::npos);
    CHECK_FALSE(h.automation->required_addons_ready());
    h.automation->frame();
    CHECK_FALSE(h.automation->scripts().running("needed"));

    h.automation->set_addon_ceilings(app::addon_ceilings(data, true));
    h.automation->frame();
    CHECK_FALSE(h.automation->scripts().running("copy"));
    CHECK_FALSE(h.automation->scripts().run("copy", {}));
    CHECK(h.automation->scripts().running("needed"));
    CHECK(h.automation->required_addons_ready());
    CHECK(h.automation->scripts().run("new", {}));
}

TEST_CASE("Automation: hailstone.autoskill_list gives the player's list with names and conditions") {
    AutomationHost h;
    h.automation->frame();
    h.write("list.lua", R"lua(
local hs = require('hailstone')
for i, r in ipairs(hs.autoskill_list()) do
  print(string.format('[list] %d %s %s %d %d %d %d %d %s %s', i, r.key, r.name, r.endurance, r.mana,
    r.target_hp_below, r.target_hp_above, r.self_hp_below, r.when, tostring(r.keep_buff)))
end
)lua");
    REQUIRE(h.automation->scripts().run("list", {}));
    h.automation->frame();
    const auto lines = h.chat();
    auto has = [&](const std::string& t) { return std::find(lines.begin(), lines.end(), t) != lines.end(); };
    CHECK(has("[list] 1 skill:30 Kick 20 0 90 0 0 fighting false"));
    CHECK(has("[list] 2 skill:32 Mend 0 0 0 0 50 always true"));
}

TEST_CASE("Automation: hailstone.use_ability by kind and name") {
    AutomationHost h;
    h.automation->frame();
    h.write("use.lua", R"lua(
local hs = require('hailstone')
print('[use] ' .. tostring(hs.use_ability('skill', 'Kick')) .. ' ' .. tostring(hs.use_ability('disc', 'Fury'))
      .. ' ' .. tostring(hs.use_ability('aa', 'None')) .. ' ' .. tostring(pcall(hs.use_ability, 'spell', 'x')))
)lua");
    REQUIRE(h.automation->scripts().run("use", {}));
    h.automation->frame();
    const auto lines = h.chat();
    CHECK(std::find(lines.begin(), lines.end(), "[use] true true false false") != lines.end());
    REQUIRE(h.used.size() == 3);
    CHECK(h.used[0] == std::pair{mq::AbilityKind::Skill, std::string("Kick")});
    CHECK(h.used[1] == std::pair{mq::AbilityKind::Discipline, std::string("Fury")});
    CHECK(h.used[2] == std::pair{mq::AbilityKind::AltAbility, std::string("None")});
}

TEST_CASE("Automation: hailstone.alt_abilities lists the AA window's abilities; hailstone.train_aa buys one rank") {
    AutomationHost h;
    auto& aa = net::GameConnectionTestAccess::alt_adv(*h.world.conn);
    auto rank = [](std::uint32_t id, std::int32_t group, std::int32_t r, std::int32_t prev, std::int32_t next,
                   std::int32_t type, std::int32_t title) {
        game::AltAbility a;
        a.id = id;
        a.group = group;
        a.rank = r;
        a.prev_id = prev;
        a.next_id = next;
        a.max_rank = 2;
        a.cost = 3 * r;
        a.total_cost = 3 * r;
        a.level_req = 40 + r;
        a.classes = 1u << 2; // cleric
        a.type = type;
        a.title_sid = title;
        a.prereq_groups = {0};
        a.prereq_ranks = {0};
        return a;
    };
    // Innate Strength (general, two ranks, the first owned); Mystical Shielding (class, needs Innate Strength 2).
    aa.add(rank(100, 10, 1, -1, 101, 1, 9001));
    aa.add(rank(101, 10, 2, 100, -1, 1, 9001));
    game::AltAbility shield = rank(200, 20, 1, -1, -1, 3, 9002);
    shield.max_rank = 1;
    shield.prereq_groups = {10};
    shield.prereq_ranks = {2};
    aa.add(shield);
    aa.owned = {{100, 3, 0}};
    aa.points = 10;
    aa.spent = 3;
    h.automation->frame();
    h.write("aa.lua", R"lua(
local hs = require('hailstone')
local p = hs.player()
print('[aa] points ' .. p.aa_points .. ' spent ' .. p.aa_spent)
for _, a in ipairs(hs.alt_abilities()) do
  local pre = {}
  for _, q in ipairs(a.prereqs) do pre[#pre + 1] = q.group .. ':' .. q.rank .. ':' .. tostring(q.met) end
  print(string.format('[aa] %d %d %s tab%d %d/%d cost %d level %d %s %s [%s]', a.id, a.group, a.name, a.tab, a.rank,
    a.max_rank, a.cost, a.level, tostring(a.can_train), tostring(a.maxed), table.concat(pre, ',')))
end
print('[aa] train ' .. tostring(hs.train_aa(20)) .. ' ' .. tostring(hs.train_aa(10)) .. ' ' .. tostring(hs.train_aa(99)))
)lua");
    REQUIRE(h.automation->scripts().run("aa", {}));
    h.automation->frame();
    const auto lines = h.chat();
    INFO(lines.size());
    auto has = [&](const std::string& t) { return std::find(lines.begin(), lines.end(), t) != lines.end(); };
    CHECK(has("[aa] points 10 spent 3"));
    CHECK(has("[aa] 101 10 Innate Strength tab1 1/2 cost 6 level 42 true false []"));
    CHECK(has("[aa] 200 20 Mystical Shielding tab3 0/1 cost 3 level 41 false false [10:2:false]"));
    // Mystical Shielding needs Innate Strength 2 (nothing sent); Innate Strength's next rank is bought; no group 99.
    CHECK(has("[aa] train false true false"));

    // At the maximum: no cost, maxed, nothing to buy.
    aa.owned = {{101, 6, 0}};
    h.write("maxed.lua", R"lua(
local hs = require('hailstone')
for _, a in ipairs(hs.alt_abilities()) do
  if a.group == 10 then print('[max] ' .. a.id .. ' ' .. a.rank .. ' ' .. a.cost .. ' ' .. tostring(a.maxed)) end
end
print('[max] train ' .. tostring(hs.train_aa(10)) .. ' ' .. tostring(hs.train_aa(20)))
)lua");
    REQUIRE(h.automation->scripts().run("maxed", {}));
    h.automation->frame();
    const auto more = h.chat();
    auto has2 = [&](const std::string& t) { return std::find(more.begin(), more.end(), t) != more.end(); };
    CHECK(has2("[max] 101 2 0 true"));
    CHECK(has2("[max] train false true")); // the prerequisite is met now
}

#ifdef HAILSTONE_EXAMPLE_PLUGIN
// A script add-on that names itself after a compiled plugin (a "read,ui" script called "nms" beside the NMS server's
// plugin) gets neither the plugin's capabilities in its windows nor its folder, functions, options or menu: each is
// the owner's by kind and name (plugin::UiBridge::owner_tag).
TEST_CASE("Automation: a script named after a compiled plugin takes none of its place") {
    AutomationHost h;
    const fs::path built = HAILSTONE_EXAMPLE_PLUGIN;
    const fs::path plugin_dir = h.world.dir / "data" / "plugins" / "MQ2Example";
    fs::create_directories(plugin_dir);
    fs::copy_file(built, plugin_dir / built.filename(), fs::copy_options::overwrite_existing);
    h.automation->frame(); // the rules: no hailstone.cfg, every plugin loads
    const std::uint32_t plugin_caps = h.automation->plugin_caps("plugin:MQ2Example");
    REQUIRE(plugin_caps == plugin::kCapAll);
    auto& ui = h.automation->plugin_ui();
    ui.add_function("MQ2Example", "secret", [](const std::vector<plugin::UiValue>&) {
        return plugin::UiValue::string("plugin");
    });

    h.write("mq2example/init.lua", R"lua(
local hs = require('hailstone')
hs.ui_function('secret', function() return 'script' end)
hs.menu('Mine', '/echo mine')
while true do require('mq').delay(10) end
)lua");
    h.write("mq2example/ui/modern/w.lua", "ui.window('W', function() end)\n");
    std::string why;
    REQUIRE_MESSAGE(h.automation->scripts().run("mq2example", {}, &why), why);
    h.automation->frame();
    // Its ceiling as an add-on that declared read and ui.
    std::string hex;
    for (const auto& s : h.automation->scripts().list())
        if (s.name == "mq2example") hex = s.sha256;
    auto hash = parse_sha256(hex);
    REQUIRE(hash);
    h.automation->set_addon_ceilings({{plugin::FileKind::Script, *hash, plugin::kCapRead | plugin::kCapUi}});
    h.automation->frame();
    REQUIRE(h.automation->scripts().running("mq2example"));

    CHECK(h.automation->plugin_caps("script:mq2example") == (plugin::kCapRead | plugin::kCapUi));
    CHECK(h.automation->plugin_caps("plugin:MQ2Example") == plugin::kCapAll);
    CHECK(h.automation->plugin_caps("MQ2Example") == 0); // a bare name is no one
    CHECK(h.automation->plugin_caps("plugin:mq2example") == 0); // the exact name
    CHECK(h.automation->plugin_caps("plugin:NoSuchPlugin") == 0);
    // Answered from the loaded plugin, not by reading and hashing the plugin files (it is asked many times a frame):
    // with the file gone from the folder, the loaded plugin still has its capabilities.
    {
        std::error_code ec;
        fs::remove(plugin_dir / built.filename(), ec);
        REQUIRE_FALSE(ec);
        CHECK(h.automation->plugin_caps("plugin:MQ2Example") == plugin::kCapAll);
        fs::copy_file(built, plugin_dir / built.filename(), fs::copy_options::overwrite_existing);
    }
    // Both folders, each under its owner's tag; the plugin's is still its own.
    REQUIRE(ui.folders().size() == 2);
    CHECK(ui.folders()[0].first == "plugin:MQ2Example");
    CHECK(ui.folders()[0].second == plugin_dir);
    CHECK(ui.folders()[1].first == "script:mq2example");
    // eq.Plugin("MQ2Example") is the compiled plugin's; the script's by its tag.
    CHECK(ui.call("MQ2Example", "secret", {})->s == "plugin");
    CHECK(ui.call("script:mq2example", "secret", {})->s == "script");
    REQUIRE(ui.menu().size() == 1);
    CHECK(ui.menu()[0].owner() == "script:mq2example");

    // The script stops: only its own entries go.
    h.automation->scripts().stop("mq2example");
    REQUIRE(ui.folders().size() == 1);
    CHECK(ui.folders()[0].first == "plugin:MQ2Example");
    CHECK(ui.call("MQ2Example", "secret", {})->s == "plugin");
    CHECK(ui.menu().empty());
}
#endif

TEST_CASE("Automation: hailstone.item gives an item's click effect, and how long its recast has to go") {
    AutomationHost h;
    using A = net::GameConnectionTestAccess;
    net::Item clicky;
    clicky.name = "Mask of Tests";
    clicky.id = 4242;
    clicky.req_level = 60;
    net::ItemEffect& click = clicky.effects[net::kEffectClick];
    click.effect = 1234;
    click.type = 4;
    click.level = 55;
    click.extra = {static_cast<std::uint32_t>(-1), 3000, 120, 7, 0}; // charges, cast ms, recast s, timer group
    clicky.last_cast_time = static_cast<std::uint32_t>(std::time(nullptr)) + 90; // 90 s to go
    A::inventory(*h.world.conn).put(net::ItemSlot::possessions(net::kSlotGeneral1 + 2), clicky);
    h.automation->frame();
    h.write("clicky.lua", R"lua(
local hs = require('hailstone')
local it = hs.item(25)
print(string.format('[item] %s %d %d %d %d %d %d %d %d %d', it.name, it.click_spell, it.click_type, it.click_level,
  it.click_cast_time, it.click_recast, it.click_recast_type, it.click_max_charges, it.req_level, it.bag_slots))
print('[ready] ' .. tostring(it.click_ready_in >= 85 and it.click_ready_in <= 90))
local bag = hs.item(24)
print('[bag] ' .. bag.bag_slots .. ' ' .. tostring(bag.click_spell) .. ' ' .. tostring(bag.click_ready_in))
)lua");
    REQUIRE(h.automation->scripts().run("clicky", {}));
    h.automation->frame();
    const auto lines = h.chat();
    auto has = [&](const std::string& t) { return std::find(lines.begin(), lines.end(), t) != lines.end(); };
    CHECK(has("[item] Mask of Tests 1234 4 55 3000 120 7 -1 60 0"));
    CHECK(has("[ready] true"));
    CHECK(has("[bag] 4 0 nil")); // no click effect: no click fields
}
