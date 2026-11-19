// A compiled plugin as a server extension (sdk/hailstone_plugin.h ABI 2, plugin/native_plugins.h): the test plugin
// tests/plugins/ExtendTest.c sets class masks, pets, switches and vitals and answers hooks through the chain, only
// with HAILSTONE_CAP_EXTEND and while active. An ABI 1 plugin still loads.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "game/combat.h"
#include "game/eq_names.h"
#include "game/server_ext_chain.h"
#include "game/spells.h"
#include "mq/commands.h"
#include "net/social_packets.h"
#include "plugin/native_plugins.h"
#include "plugin/ui_bridge.h"
#include "app/lua_ui.h"
#include "app/lua_ui_plugins.h"
#include "ui/lua/lua_host.h"
#include "mq_test_world.h"

#include <fstream>
#include <imgui.h>
#include <lua.hpp>

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Host {
    mq_test::World world{"hailstone_native_extension_test"};
    fs::path plugin_dir = world.dir / "plugins";
    plugin::Hooks hooks;
    plugin::Policy policy;
    mq::Registry reg;
    mq::Context ctx;
    game::ExtensionChain chain;
    plugin::UiBridge bridge;
    std::vector<std::string> printed;
    float max_zoom = 50.0f;
    std::map<std::tuple<int, int, int>, std::string> hot; // the hot buttons (ini strings) and pages
    std::map<int, int> hot_pages;
    std::unique_ptr<plugin::NativePlugins> plugins;

    Host() {
        fs::create_directories(plugin_dir);
        for (fs::path built : {fs::path(HAILSTONE_EXTEND_PLUGIN), fs::path(HAILSTONE_EXTEND_PLUGIN_V1),
                               fs::path(HAILSTONE_EXTEND_PLUGIN_NEW)})
            fs::copy_file(built, plugin_dir / built.filename(), fs::copy_options::overwrite_existing);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        plugin::NativeEnv ne;
        ne.hooks = &hooks;
        ne.policy = &policy;
        ne.registry = &reg;
        ne.context = [this]() -> const mq::Context& { return ctx; };
        ne.print = [this](const std::string& t, std::uint32_t) { printed.push_back(t); };
        ne.run = [this](const std::string& l) {
            std::string line = l;
            hooks.command(line);
        };
        ne.dirs = {plugin_dir};
        ne.cache_dir = world.dir / "cache";
        ne.config_dir = world.dir / "config";
        ne.extensions = &chain;
        ne.ui = &bridge;
        ne.connection = [this] { return world.conn.get(); };
        ne.max_camera_distance = [this] { return max_zoom; };
        ne.set_max_camera_distance = [this](float d) { max_zoom = d; };
        ne.hot_button = [this](int bar, int page, int button) -> std::optional<std::string> {
            if (bar < 0 || bar >= 10) return std::nullopt;
            auto it = hot.find({bar, page, button});
            return it == hot.end() ? std::string() : it->second;
        };
        ne.set_hot_button = [this](int bar, int page, int button, const std::string& spec) {
            hot[{bar, page, button}] = spec;
            return true;
        };
        ne.hot_bar_page = [this](int bar) { return hot_pages[bar]; };
        ne.set_hot_bar_page = [this](int bar, int page) { hot_pages[bar] = page; };
        plugins = std::make_unique<plugin::NativePlugins>(ne);
    }
    ~Host() { plugins.reset(); }
    void approve(std::uint32_t caps, bool provisional = false) {
        plugin::Rules r;
        r.configured = true;
        r.provisional = provisional;
        r.player_plugin_caps = caps;
        r.dir = world.dir / "servers" / "test";
        policy.set(r);
    }
    void run(const std::string& line) {
        std::string l = line;
        hooks.command(l);
    }
    bool printed_part(const std::string& t) const {
        return std::any_of(printed.begin(), printed.end(), [&](const std::string& p) {
            return p.find(t) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE("plugin server extension: setters, switches and hooks through the chain while active") {
    Host h;
    net::GameConnection& c = *h.world.conn;
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("ExtendTest"));

    // The setters went into the connection.
    CHECK(c.server_class_mask() == 0x3);
    CHECK(c.classes_mask("Gennro") == 0x8001);
    REQUIRE(c.pets().list.size() == 2);
    CHECK(c.pets().focused == 300);

    // Switches, vitals and hooks answer through the chain.
    CHECK(h.chain.wide_item_links(c));
    CHECK_FALSE(h.chain.aa_any_level(c));
    CHECK(h.chain.hide_check_class(c, 4) == 9);
    CHECK(h.chain.character_vitals(c).max_hp == 1234);
    CHECK_FALSE(h.chain.character_vitals(c).cur_hp);
    CHECK(h.chain.is_spellcaster(c) == true);
    CHECK(h.chain.guild_member_classes(c, 1003) == 3);
    CHECK(h.chain.guild_member_classes(c, 2) == 0);
    std::string label = "stock";
    h.chain.label_text(c, 42, label);
    CHECK(label == "plugin label");
    label = "stock";
    h.chain.label_text(c, 41, label);
    CHECK(label == "stock");
    CHECK(h.chain.hide_xtarget_slot(c, 19));
    CHECK_FALSE(h.chain.hide_xtarget_slot(c, 18));
    // The maps: the plugin's mark, starting from the client's defaults; a spawn's group and con colour.
    {
        const auto& spawns = c.world().spawns;
        game::MapSpawnMark d;
        d.rgb = 0xff0000;
        d.label = "a rat";
        auto rat = h.chain.map_spawn(c, spawns.at(10), d);
        REQUIRE(rat);
        CHECK(rat->rgb == 0x00aa00u);
        CHECK(rat->layer == 1);
        CHECK(rat->label == "a rat");
        auto groupie = h.chain.map_spawn(c, spawns.at(12), d);
        REQUIRE(groupie);
        CHECK(groupie->label == "Groupie con " + std::to_string(game::con_chat_color(game::con_level(spawns.at(12), 50, 0))));
        CHECK(groupie->rgb == 0xff0000u);
        CHECK_FALSE(h.chain.map_spawn(c, spawns.at(11), d));
    }

    net::WhoResponse who;
    net::WhoEntry e;
    e.name = "Bob";
    e.level = 60;
    who.entries.push_back(e);
    auto lines = h.chain.who_lines(c, who);
    REQUIRE(lines);
    CHECK(*lines == std::vector<std::string>{"who: 1", "Bob level 60"});

    game::SpellData s7, s8;
    s7.id = 7;
    s8.id = 8;
    auto level_line = h.chain.level_needed_line(c);
    REQUIRE(level_line);
    CHECK(level_line(s7) == "Level Needed: test");
    CHECK_FALSE(level_line(s8)); // the stock line

    const game::SkillRules* rules = h.chain.skill_rules(c);
    REQUIRE(rules);
    CHECK(rules->gate(rules->ctx, 53, 1, 50) == false);
    CHECK_FALSE(rules->gate(rules->ctx, 10, 1, 50).has_value());

    // The item hooks.
    CHECK(h.chain.item_right_click(c, net::ItemSlot::possessions(net::kSlotCursor)));
    CHECK(h.printed_part("[click] cursor"));
    CHECK_FALSE(h.chain.item_right_click(c, net::ItemSlot::possessions(net::kSlotGeneral1)));
    net::Item potion, other;
    potion.id = 5001;
    other.id = 5002;
    CHECK(h.chain.merchant_level(c, potion) == "L42");
    CHECK_FALSE(h.chain.merchant_level(c, other));

    // The readers. The player's HP is Cur_HP / Max_HP, the items' HP included (GameConnection::own_hp): 750 from the
    // server and 250 from the items; the plugin gave Max_HP whole above (set_vitals, 1234), so nothing is added to it.
    c.set_own_hp_items(250, 0);
    h.run("/extread");
    CHECK(h.printed_part("[read] player Tester 50 class 2 target 10 group 1 hp 1000/1234"));
    CHECK(h.printed_part("[read] spawns 5"));
    CHECK(h.printed_part("[read] spawn a_rat01 npc 1"));
    CHECK(h.printed_part("[read] spell Holy Armor cleric 3"));
    CHECK(h.printed_part("[read] item Healing Potion x2"));
    CHECK(h.printed_part("[read] buff 11"));
    CHECK(h.printed_part("[read] eqstr -1"));
    CHECK(h.printed_part("[read] old 1 size 8"));

    // next_spawn walks in id order from its sorted list, and a spawn gone or come since is seen.
    {
        std::vector<std::string> ids;
        std::string after = "0";
        for (int n = 0; n < 10; ++n) {
            h.printed.clear();
            h.run("/extnext " + after);
            REQUIRE(h.printed.size() == 1);
            if (h.printed[0] == "[next] none") break;
            after = h.printed[0].substr(7);
            ids.push_back(after);
        }
        CHECK(ids.size() == 5);
        CHECK(std::is_sorted(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) {
            return std::stoul(a) < std::stoul(b);
        }));
        h.run("/extnext 0");
        const std::uint32_t second = static_cast<std::uint32_t>(std::stoul(ids[1]));
        net::GameConnectionTestAccess::world(*h.world.conn).spawns.erase(second);
        h.printed.clear();
        h.run("/extnext " + ids[0]);
        CHECK(h.printed == std::vector<std::string>{"[next] " + ids[2]});
        h.world.add(second, "back_again", true, 1, {});
        h.printed.clear();
        h.run("/extnext " + ids[0]);
        CHECK(h.printed == std::vector<std::string>{"[next] " + ids[1]});
        // A walk resumed from a saved id, after one spawn came and another went (the same count, and the id it
        // had next still there), sees the new one.
        h.run("/extnext 0");
        auto& world = net::GameConnectionTestAccess::world(*h.world.conn);
        REQUIRE(ids.front() == "1");
        REQUIRE(world.spawns.erase(13) == 1);
        ++world.spawn_set_serial; // as OP_DeleteSpawn
        h.world.add(5, "newcomer", true, 1, {});
        h.printed.clear();
        h.run("/extnext 2");
        CHECK(h.printed == std::vector<std::string>{"[next] 5"});
    }

    // The rest of the host: folders, zone names, actions, the camera.
    h.world.conn->set_window_pet(300);
    net::GameConnectionTestAccess::profile(c)->skills.assign(100, 0);
    net::GameConnectionTestAccess::profile(c)->skills[53] = 12;
    game::AltAbility rank;
    rank.id = 9001;
    rank.group = 777;
    net::GameConnectionTestAccess::alt_adv(c).add(rank);
    net::GameConnectionTestAccess::alt_adv(c).owned.push_back({9001, 3, 0});
    h.run("/extmore");
    bool yellow = false;
    while (auto line = c.poll_chat()) yellow = yellow || (line->text == "[more] yellow" && line->color == 15);
    CHECK(yellow);
    const fs::path server_dir = h.world.dir / "servers" / "test" / "plugin-config" / "ExtendTest";
    CHECK(h.printed_part("[more] server " + std::to_string(server_dir.string().size()) + " " + server_dir.string()));
    CHECK(fs::is_directory(server_dir));
    CHECK(h.printed_part("[more] character 0 ")); // no world yet, so no character folder
    CHECK(h.printed_part("[more] zone qeynos -1"));
    CHECK(c.pets().focused == 301);
    CHECK(h.printed_part("[more] inspect 1"));
    CHECK(h.printed_part("[more] camera 90"));
    CHECK(h.printed_part("[more] hot 33 HA1,A-1,0000000000000000,0,Attack page 4 bad -1"));
    CHECK(h.printed_part("[more] skill 12 aa 3 none 0"));
    CHECK(h.printed_part("[more] player pet 301 stand 100 casting 0"));
    CHECK(h.printed_part("[more] server_info none")); // before the world login
    // The world login: the saved server (its profile, from the world's name) and the world.
    auto& ticket = net::GameConnectionTestAccess::ticket(c);
    ticket.server_name = "Triune World";
    ticket.world_host = "203.0.113.7";
    ServerProfile saved;
    saved.id = profile_id_for_world(ticket.server_name);
    saved.name = "Project Triune";
    saved.host = "login.example.test";
    saved.port = 5998;
    REQUIRE(save_profile(saved, h.world.dir / "servers"));
    h.run("/extmore");
    CHECK(h.printed_part("[more] server_info Project Triune|login.example.test|5998|Triune World|203.0.113.7"));

    // Inactive: the stock answers; active again: its own.
    h.run("/extoff");
    CHECK_FALSE(h.chain.wide_item_links(c));
    CHECK_FALSE(h.chain.is_spellcaster(c));
    CHECK_FALSE(h.chain.who_lines(c, who));
    h.run("/exton");
    CHECK(h.chain.wide_item_links(c));

    // The server's rules change and no longer let it extend the client: out of the chain, still loaded.
    h.approve(plugin::kCapAll & ~plugin::kCapExtend);
    h.plugins->policy_changed({});
    CHECK(h.plugins->loaded("ExtendTest"));
    CHECK(h.chain.empty());
    CHECK_FALSE(rules->gate(rules->ctx, 53, 1, 50).has_value());
    h.approve(plugin::kCapAll);

    // Unloaded: out of the chain; what the core kept gives the stock answers.
    h.plugins->unload("ExtendTest");
    CHECK(h.chain.empty());
    CHECK_FALSE(rules->gate(rules->ctx, 53, 1, 50).has_value());
    CHECK_FALSE(level_line(s7));
}

TEST_CASE("plugin server extension: needs HAILSTONE_CAP_EXTEND; an ABI 1 plugin still loads") {
    Host h;
    net::GameConnection& c = *h.world.conn;
    h.approve(plugin::kCapAll & ~plugin::kCapExtend);
    REQUIRE(h.plugins->load("ExtendTest"));
    CHECK(h.chain.empty());
    CHECK(c.server_class_mask() == 0);
    CHECK(c.pets().list.empty());
    CHECK(h.printed_part("'extend' permission"));
    h.plugins->unload("ExtendTest");

    REQUIRE(h.plugins->load("ExtendTestV1"));
    CHECK(h.printed_part("[ExtendTestV1] loaded"));
    CHECK(h.chain.empty()); // no extend: no member
    h.plugins->unload("ExtendTestV1");

    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("ExtendTestV1"));
    CHECK_FALSE(h.chain.empty()); // a member, though it answers nothing
    CHECK_FALSE(h.chain.wide_item_links(c));
    h.plugins->unload("ExtendTestV1");
}

// The SDK's exports were nmsc_plugin_* up to 0.6 and are hailstone_plugin_* since: the client takes either.
// ExtendTestV1 has only the old abi / set_host names and ExtendTest its hooks under the old name (above); ExtendTestNew
// exports its hooks under the new one, and HAILSTONE_PLUGIN gives it both names of the other two.
TEST_CASE("plugin server extension: the hook table under its export name after 0.6") {
    Host h;
    net::GameConnection& c = *h.world.conn;
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("ExtendTestNew"));
    CHECK(c.server_class_mask() == 0x3);
    CHECK(h.chain.is_spellcaster(c) == true);
    CHECK(h.chain.guild_member_classes(c, 1003) == 3);
    h.plugins->unload("ExtendTestNew");
    CHECK(h.chain.empty());
}

TEST_CASE("plugin server extension: before the zone-in only a plugin with 'login' loads") {
    Host h;
    h.approve(plugin::kCapAll & ~plugin::kCapLogin, true);
    std::string why;
    CHECK_FALSE(h.plugins->load("ExtendTest", &why));
    CHECK(why.find("'login'") != std::string::npos);
    h.approve(plugin::kCapAll, true);
    REQUIRE(h.plugins->load("ExtendTest"));
    CHECK(h.world.conn->classes_mask("Gennro") == 0x8001); // at character select it can set the masks
    // The zone-in's rules without 'login' keep it: login matters only before the zone-in.
    h.approve(plugin::kCapAll & ~plugin::kCapLogin);
    h.plugins->policy_changed({});
    CHECK(h.plugins->loaded("ExtendTest"));
    h.plugins->unload("ExtendTest");
}

TEST_CASE("connection: the last login's rules, provisional, from the world login to the zone-in") {
    mq_test::World w("hailstone_provisional_rules_test");
    using A = net::GameConnectionTestAccess;
    A::state(*w.conn) = net::ConnectionState::CharacterSelect;
    w.conn->update();
    REQUIRE(w.conn->server_rules());
    CHECK(w.conn->server_rules()->provisional);
    const std::uint32_t serial = w.conn->server_rules_serial();
    w.conn->update();
    CHECK(w.conn->server_rules_serial() == serial); // made once, not every frame
    A::state(*w.conn) = net::ConnectionState::Disconnected;
    w.conn->update();
    CHECK_FALSE(w.conn->server_rules());
}

// A server without Hailstone's script sends no file list: no hailstone.cfg, but the rules still name the server's
// folder, where a plugin's per-server files (server_config_dir) live.
TEST_CASE("connection: the zone-in's rules keep the server's folder when the server sent no file list") {
    net::ConnectionOptions options;
    options.server_files_dir = test::unique_temp_path("hailstone_no_list_rules_test");
    mq_test::World w("hailstone_no_list_rules_world", options);
    using A = net::GameConnectionTestAccess;
    A::state(*w.conn) = net::ConnectionState::InZone;
    A::update_server_rules(*w.conn);
    REQUIRE(w.conn->server_rules());
    CHECK_FALSE(w.conn->server_rules()->configured);
    CHECK_FALSE(w.conn->server_rules()->provisional);
    CHECK(w.conn->server_rules()->dir == options.server_files_dir);
}

// The scripts keep running through a zone change (the Scripts window; docs/plugins.md): the zone-in's rules stay
// through every state of it, so nothing re-checks what runs; camping to character select makes them provisional.
TEST_CASE("connection: the zone-in's rules last through a zone change") {
    mq_test::World w("hailstone_zoning_rules_test");
    using A = net::GameConnectionTestAccess;
    A::state(*w.conn) = net::ConnectionState::InZone;
    A::set_server_rules(*w.conn, plugin::Rules{});
    const std::uint32_t serial = w.conn->server_rules_serial();
    for (auto st : {net::ConnectionState::Zoning, net::ConnectionState::ZoneConnecting,
                    net::ConnectionState::ZoneEntering, net::ConnectionState::InZone}) {
        A::state(*w.conn) = st;
        w.conn->update();
        REQUIRE(w.conn->server_rules());
        CHECK_FALSE(w.conn->server_rules()->provisional);
        CHECK(w.conn->server_rules_serial() == serial);
    }
    A::state(*w.conn) = net::ConnectionState::CharacterSelect;
    w.conn->update();
    REQUIRE(w.conn->server_rules());
    CHECK(w.conn->server_rules()->provisional);
}

namespace {
std::string lua_eval(lua_State* L, const char* expr) {
    std::string code = std::string("return tostring(") + expr + ")";
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_pop(L, 1);
        return "error: " + e;
    }
    std::string v = lua_tostring(L, -1);
    lua_pop(L, 1);
    return v;
}
// The same in a plugin folder's own globals (ui::lua::LuaUi::set_folders); "nil" before it has any.
std::string lua_eval_in(ui::lua::LuaUi& lua, const char* tag, const char* expr) {
    lua_State* L = lua.state();
    std::string code = std::string("return tostring(") + expr + ")";
    if (luaL_loadstring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_pop(L, 1);
        return "error: " + e;
    }
    lua.push_folder_env_for_test(tag);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return "nil";
    }
    lua_setupvalue(L, -2, 1);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_pop(L, 1);
        return "error: " + e;
    }
    std::string v = lua_tostring(L, -1);
    lua_pop(L, 1);
    return v;
}
} // namespace

TEST_CASE("plugin windows: its ui/<pack>/ folder, hs.Plugin calls, options and menu entries") {
    Host h;
    h.approve(plugin::kCapAll);
    // Its window, in its folder beside the binary (plugins/ExtendTest/ui/pack/).
    const fs::path ui_dir = h.plugin_dir / "ExtendTest" / "ui" / "pack";
    fs::create_directories(ui_dir);
    std::ofstream(ui_dir / "ext.lua") << "opts = hs.PluginOptions()\n"
                                         "menu = hs.PluginMenu()\n"
                                         "ui.window('ExtWin', function()\n"
                                         "  local p = hs.Plugin('ExtendTest')\n"
                                         "  info = p and p.info('hello', {key = 'k'})\n"
                                         "  via_call = p and p.call('info').level\n"
                                         "  missing = p and p.nope()\n"
                                         "end)\n";
    REQUIRE(h.plugins->load("ExtendTest"));
    REQUIRE(h.bridge.folders().size() == 1);
    CHECK(h.bridge.folders()[0].second == h.plugin_dir / "ExtendTest");
    CHECK(h.bridge.has_plugin("extendtest"));

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, ht = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &ht);
    {
        const fs::path pack = h.world.dir / "pack";
        fs::create_directories(pack);
        app::UiHost eh;
        eh.conn = h.world.conn.get();
        eh.plugin_ui = &h.bridge;
        ui::lua::LuaUi lua(pack, h.world.dir / "ui.txt");
        lua.add_ui_api([&](lua_State* L) { app::register_plugins_api(L, eh); });
        auto frame = [&] {
            std::vector<ui::lua::LuaUi::Folder> folders;
            for (const auto& [name, folder] : h.bridge.folders()) folders.push_back({name, folder / "ui" / "pack"});
            lua.set_folders(std::move(folders));
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        lua_State* L = lua.state();
        frame();
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "info and info.echo") == "hello");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "info and info.field") == "k");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "info and info.level") == "50");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "info and #info.rows") == "3");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "info and info.rows[3]") == "3");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "via_call") == "50");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "missing") == "nil");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "opts[1].name .. ' ' .. opts[1].label .. ' ' .. opts[1].value .. ' ' .. opts[1].max") ==
              "mode Mode 2 3");
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "menu[1].label .. ' ' .. menu[1].command") == "Ext Window /exton");
        CHECK(lua_eval(L, "(function() hs.SetPluginOption('ExtendTest', 'mode', 9) return hs.PluginOptions()[1].value end)()") ==
              "3"); // clamped to its range

        // Unloaded: its windows, functions, options and menu go.
        h.plugins->unload("ExtendTest");
        CHECK(h.bridge.folders().empty());
        CHECK_FALSE(h.bridge.has_plugin("ExtendTest"));
        frame();
        CHECK(lua_eval_in(lua, "plugin:ExtendTest", "info") == "nil"); // its globals went with it
        CHECK(lua_eval(L, "hs.Plugin('ExtendTest')") == "nil");
        CHECK(lua_eval(L, "#hs.PluginOptions()") == "0");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("plugin windows: the bridge alone") {
    plugin::UiBridge b;
    CHECK_FALSE(b.has_plugin("x"));
    b.add_function("X", "f", [](const std::vector<plugin::UiValue>& a) {
        return plugin::UiValue::integer(static_cast<std::int64_t>(a.size()));
    });
    CHECK(b.has_plugin("x"));
    auto r = b.call("x", "f", {plugin::UiValue::string("a"), plugin::UiValue::boolean(true)});
    REQUIRE(r);
    CHECK(r->i == 2);
    CHECK_FALSE(b.call("x", "g", {}));
    int v = 0;
    b.add_option({"X", "o", "O", 0, 1, [&] { return v; }, [&](int n) { v = n; }});
    b.add_option({"X", "o", "Other", 0, 5, [&] { return v; }, [&](int n) { v = n; }}); // replaces
    REQUIRE(b.options().size() == 1);
    CHECK(b.options()[0].label == "Other");
    b.add_menu({"X", "Open", "/x"});
    b.set_folder("X", "/a");
    b.set_folder("x", "/b");
    REQUIRE(b.folders().size() == 1);
    CHECK(b.folders()[0].second == fs::path("/b"));
    b.remove("x");
    CHECK_FALSE(b.has_plugin("X"));
    CHECK(b.options().empty());
    CHECK(b.menu().empty());
}

TEST_CASE("plugin server extension: set_class_name names classes while the plugin may extend the client") {
    game::clear_class_name_overrides();
    Host h;
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("ExtendTest"));
    h.run("/extclass 1 Vanguard VAN");
    h.run("/extclass 8 Skirmisher");
    CHECK(game::class_name(1) == "Vanguard");
    CHECK(game::class_short_name(1) == "VAN");
    CHECK(game::class_name(8) == "Skirmisher");
    CHECK(game::class_short_name(8) == "BRD");
    CHECK(game::class_name(20) == "Vanguard Guildmaster");
    // NULL restores the stock name; refused input changes nothing.
    h.run("/extclass 8");
    CHECK(game::class_name(8) == "Bard");
    h.run("/extclass 17 Nobody NOB");
    h.run("/extclass 1 Vanguard VANGD");
    CHECK(game::class_name(1) == "Vanguard");
    CHECK(game::class_short_name(1) == "VAN");

    // The server's rules take `extend` away: its names go.
    h.approve(plugin::kCapAll & ~plugin::kCapExtend);
    h.plugins->policy_changed({});
    CHECK(h.plugins->loaded("ExtendTest"));
    CHECK(game::class_name(1) == "Warrior");
    h.run("/extclass 1 Vanguard VAN"); // refused without it
    CHECK(game::class_name(1) == "Warrior");

    // Unloaded: its names go.
    h.approve(plugin::kCapAll);
    h.plugins->unload("ExtendTest");
    REQUIRE(h.plugins->load("ExtendTest"));
    h.run("/extclass 1 Vanguard VAN");
    CHECK(game::class_name(1) == "Vanguard");
    h.plugins->unload("ExtendTest");
    CHECK(game::class_name(1) == "Warrior");
}
