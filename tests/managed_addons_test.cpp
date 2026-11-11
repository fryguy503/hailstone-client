#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/distribution.h"
#include "core/sha256.h"
#include "mq/lua_mq.h"
#include "plugin/native_plugins.h"
#include "plugin/ui_bridge.h"
#include "scratch_dir.h"

#define HAILSTONE_HOST
#include "hailstone_builtin_addon.h"

using namespace hailstone;

namespace {
struct State {
    const PluginHostApi* host = nullptr;
    int initialized = 0, stopped = 0, pulses = 0, states = 0, zones = 0, packets = 0, commands = 0;
    bool fail_host = false, fail_init = false;
};
State* current = nullptr;

void set_host(const PluginHostApi* host) {
    current->host = host;
    if (current->fail_host) throw std::runtime_error("fixture host failure");
}
void initialize() {
    ++current->initialized;
    // A linked module's callbacks retain capability checks: this descriptor cannot issue commands.
    current->host->do_command(current->host->host, "/echo forbidden");
    if (current->fail_init) throw std::runtime_error("fixture initialization failure");
}
void shutdown() { ++current->stopped; current->host = nullptr; }
void pulse() { ++current->pulses; }
void game_state(int) { ++current->states; }
void zone() { ++current->zones; }
int packet(int, int, HailstonePacket*) { ++current->packets; return 0; }

HailstoneBuiltinAddon descriptor() {
    HailstoneBuiltinAddon addon{};
    addon.size = sizeof(addon);
    addon.abi = HAILSTONE_PLUGIN_ABI;
    addon.name = "FixtureBuiltin";
    addon.capabilities = HAILSTONE_CAP_READ | HAILSTONE_CAP_EVENTS | HAILSTONE_CAP_UI |
                         HAILSTONE_CAP_PACKETS | HAILSTONE_CAP_EXTEND | HAILSTONE_CAP_LOGIN;
    addon.set_host = set_host;
    addon.initialize = initialize;
    addon.shutdown = shutdown;
    addon.pulse = pulse;
    addon.game_state = game_state;
    addon.begin_zone = zone;
    addon.end_zone = zone;
    addon.zoned = zone;
    addon.packet = packet;
    return addon;
}

struct Host {
    plugin::Hooks hooks;
    plugin::Policy policy;
    plugin::UiBridge ui;
    std::unique_ptr<plugin::NativePlugins> plugins;
    explicit Host(State& state, const std::filesystem::path& folder = {}) {
        current = &state;
        plugin::NativeEnv env;
        env.hooks = &hooks;
        env.policy = &policy;
        env.ui = &ui;
        env.run = [&state](const std::string&) { ++state.commands; };
        if (!folder.empty()) env.dirs = {folder};
        env.cache_dir = folder / "cache";
        plugins = std::make_unique<plugin::NativePlugins>(std::move(env));
    }
    ~Host() { plugins.reset(); current = nullptr; }
};
} // namespace

TEST_CASE("managed built-in uses SDK callbacks and survives unload, policy changes and reconnect states") {
    REQUIRE(distribution::managed());
    test::ScratchDir scratch("managed_builtin");
    State state;
    {
        Host host(state, scratch.path());
        auto addon = descriptor();
        std::string why;
        const auto ui = scratch.path() / "builtin" / "fixture" / "ui";
        REQUIRE(host.plugins->load_builtin(addon, ui, &why));
        CHECK(state.initialized == 1);
        CHECK(state.commands == 0);
        REQUIRE(state.host);
        CHECK(state.host->caps(state.host->host) == addon.capabilities);
        REQUIRE(host.ui.folders().size() == 1);
        CHECK(host.ui.folders().front().first == "plugin:FixtureBuiltin");
        CHECK(host.ui.folders().front().second == ui.parent_path());
        CHECK_FALSE(host.plugins->unload("FixtureBuiltin"));
        host.plugins->unload_all();
        CHECK(host.plugins->loaded("FixtureBuiltin"));
        host.policy.set(plugin::parse_server_cfg("[plugins]\nplayer=off\n[scripts]\nplayer=off\n"));
        host.plugins->policy_changed({"*"});
        host.policy.clear();
        host.plugins->policy_changed({"*"});
        CHECK(host.plugins->loaded_caps("FixtureBuiltin") == addon.capabilities);
        REQUIRE(host.ui.folders().size() == 1);
        host.hooks.game_state(plugin::kGameStateInGame);
        host.hooks.game_state(plugin::kGameStatePreCharSelect);
        host.hooks.game_state(plugin::kGameStateInGame);
        host.hooks.zone(plugin::ZonePhase::Begin);
        host.hooks.zone(plugin::ZonePhase::End);
        host.hooks.zone(plugin::ZonePhase::Zoned);
        host.hooks.pulse();
        net::AppPacket incoming(0x1234, {});
        CHECK(host.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, incoming) == plugin::Flow::Continue);
        CHECK(state.states == 3);
        CHECK(state.zones == 3);
        CHECK(state.pulses == 1);
        CHECK(state.packets == 1);
        CHECK(state.stopped == 0);
        const auto listed = host.plugins->list();
        REQUIRE(listed.size() == 1);
        CHECK(listed.front().builtin);
        CHECK(listed.front().kept);
        CHECK_FALSE(host.plugins->load_builtin(addon, {}, &why));
        CHECK_FALSE(std::filesystem::exists(scratch.path() / "cache"));
    }
    CHECK(state.stopped == 1); // Only destruction of the host releases a required linked module.
}

TEST_CASE("managed built-in descriptor and initialization failures leave no callbacks or UI registration") {
    REQUIRE(distribution::managed());
    State state;
    Host host(state);
    auto addon = descriptor();
    SUBCASE("newer ABI") { ++addon.abi; }
    SUBCASE("missing host callback") { addon.set_host = nullptr; }
    SUBCASE("automation capability") { addon.capabilities |= HAILSTONE_CAP_MOVE; }
    SUBCASE("host callback throws") { state.fail_host = true; }
    SUBCASE("initialization throws") { state.fail_init = true; }
    SUBCASE("invalid owner name") { addon.name = "../other"; }
    std::string why;
    CHECK_FALSE(host.plugins->load_builtin(addon, "trusted/ui", &why));
    CHECK_FALSE(why.empty());
    CHECK_FALSE(host.plugins->loaded("FixtureBuiltin"));
    CHECK(host.ui.folders().empty());
    host.hooks.pulse();
    CHECK(state.pulses == 0);
}

TEST_CASE("managed mode refuses dynamic libraries and server policy cannot reenable user code") {
    REQUIRE(distribution::managed());
    test::ScratchDir scratch("managed_dynamic");
    std::ofstream(scratch.path() / "UserPlugin.dll") << "fixture, never a library";
    State state;
    Host host(state, scratch.path());
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer=on\nplayer_caps=all\n[scripts]\nplayer=on\nplayer_caps=all\n[socials]\nmode=full\n");
    const auto digest = sha256("fixture");
    rules.server_files.push_back({plugin::FileKind::Plugin, "plugins/UserPlugin.dll", digest});
    rules.server_files.push_back({plugin::FileKind::Script, "scripts/UserScript.lua", digest});
    host.policy.set(rules);
    CHECK(host.policy.caps(plugin::FileKind::Plugin, digest) == 0);
    CHECK(host.policy.caps(plugin::FileKind::Script, digest) == 0);
    CHECK(host.policy.button_rules().mode == plugin::ButtonMode::Off);
    CHECK_FALSE(host.policy.button_rules().faces);
    std::string why;
    CHECK_FALSE(host.plugins->load("UserPlugin", &why));
    CHECK(why.find("disabled") != std::string::npos);
    host.plugins->policy_changed({"*"});
    host.plugins->answer_consent(true);
    CHECK_FALSE(host.plugins->has_plugin("UserPlugin"));
    CHECK(host.plugins->list().empty());
    CHECK_FALSE(host.plugins->consent());
    CHECK_FALSE(std::filesystem::exists(scratch.path() / "cache"));
    rules.configured = false; // Missing rules also cannot expand the distribution's code admission policy.
    host.policy.set(rules);
    CHECK(host.policy.caps(plugin::FileKind::Plugin, digest) == 0);
}

TEST_CASE("managed mode rejects Lua scripts, server autorun and every Lua social execution lane") {
    REQUIRE(distribution::managed());
    test::ScratchDir scratch("managed_lua");
    std::ofstream(scratch.path() / "UserScript.lua") << "return 42";
    plugin::Hooks hooks;
    plugin::Policy policy;
    auto rules = plugin::parse_server_cfg("[scripts]\nplayer=on\nplayer_caps=all\nrun=UserScript\n[socials]\nmode=full\n");
    rules.server_files.push_back({plugin::FileKind::Script, "scripts/UserScript.lua", sha256("return 42")});
    policy.set(rules);
    mq::ScriptEnv env;
    env.hooks = &hooks;
    env.policy = &policy;
    env.dirs = {scratch.path()};
    mq::ScriptHost scripts(std::move(env));
    std::string why;
    CHECK_FALSE(scripts.run("UserScript", {}, &why));
    CHECK(why.find("disabled") != std::string::npos);
    scripts.policy_changed({"UserScript"});
    scripts.set_autorun_file(scratch.path() / "scripts.ini");
    CHECK_FALSE(scripts.set_autorun("UserScript", true));
    CHECK_FALSE(scripts.autorun_available());
    CHECK(scripts.found().empty());
    CHECK(scripts.list().empty());
    CHECK_FALSE(std::filesystem::exists(scratch.path() / "scripts.ini"));
    mq::ButtonPress press;
    press.social = 0;
    press.name = "Fixture";
    press.lua.press = "error('must never execute')";
    press.lua.face = "return {label='must never execute'}";
    CHECK_FALSE(scripts.press_button(press, &why));
    const auto dry = scripts.dry_run_button(press);
    CHECK_FALSE(dry.error.empty());
    const auto& face = scripts.button_face(0, press.name, press.lua);
    CHECK_FALSE(face.has);
    CHECK(face.error.empty());
    CHECK_FALSE(scripts.button_running(0));
    CHECK(scripts.list().empty());
}
