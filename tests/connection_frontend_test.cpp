#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <memory>

#include "app/connection_frontend.h"
#include "app/automation.h"
#include "core/game_dir.h"
#include "core/sha256.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "plugin_consent_helper.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;

#ifdef HAILSTONE_EXAMPLE_PLUGIN
namespace {
struct CharacterEntryHost {
    test::ScratchDir scratch{"character_entry_consent"};
    test::Rig rig;
    std::unique_ptr<app::Automation> automation;
    std::filesystem::path profile = scratch.path() / "profile";
    std::filesystem::path file;

    CharacterEntryHost(bool login = true, bool preapproved = false, bool configured = true) {
        const std::filesystem::path built = HAILSTONE_EXAMPLE_PLUGIN;
        const auto plugin_dir = (configured ? profile : scratch.path() / "data") / "plugins";
        std::filesystem::create_directories(plugin_dir);
        file = plugin_dir / built.filename();
        std::filesystem::copy_file(built, file);
        if (preapproved) REQUIRE(test::answer_server_plugin(profile, file));
        app::AutomationEnv env;
        env.data_dir = scratch.path() / "data";
        env.install_dir = scratch.path() / "install";
        env.connection = [this] { return &rig.conn; };
        automation = std::make_unique<app::Automation>(std::move(env));
        REQUIRE(rig.to_character_select());
        plugin::Rules rules;
        rules.configured = configured;
        rules.provisional = true;
        rules.dir = profile;
        if (configured) {
            std::ifstream in(file, std::ios::binary);
            const std::string bytes(std::istreambuf_iterator<char>(in), {});
            // A server-distributed library asks for approval. Its capabilities are the server's own.
            if (login) rules.server_files.push_back({plugin::FileKind::Plugin, "plugins/MQ2Example", sha256(bytes)});
            else rules.player_plugin_caps = plugin::kCapRead;
        }
        net::GameConnectionTestAccess::set_server_rules(rig.conn, rules);
    }
    ~CharacterEntryHost() {
        automation->shutdown();
        ui::lua::set_consent_guard(false);
    }

    std::size_t entries() const {
        return static_cast<std::size_t>(std::count_if(rig.server.world_log.begin(), rig.server.world_log.end(),
            [](const auto& packet) { return packet.opcode == net::to_wire(net::Op::EnterWorld); }));
    }
};
} // namespace

TEST_CASE("character entry: first-frame LOGIN consent holds entry and automatic retries until answered") {
    for (const auto answer : {std::optional<bool>(true), std::optional<bool>(false), std::optional<bool>()}) {
        CAPTURE(answer.has_value());
        CAPTURE(answer.value_or(false));
        CharacterEntryHost h;
        int pulses = 0;
        h.automation->hooks().on_pulse(plugin::Hooks::kClient, [&] { ++pulses; });
        // The app updates the connection before Automation::frame. The entry gate must find the newly arrived
        // provisional rules on this very first frame, without executing a pulse or loading unapproved code.
        CHECK_FALSE(h.automation->plugin_consent());
        bool auto_sent = app::enter_character(h.rig.conn, *h.automation, "Gennro");
        CHECK_FALSE(auto_sent);
        REQUIRE(h.automation->plugin_consent());
        const auto serial = h.automation->plugin_consent()->serial;
        CHECK(h.automation->plugin_caps("plugin:MQ2Example") == 0);
        CHECK(h.rig.conn.character_name().empty());
        CHECK(h.rig.conn.state() == net::ConnectionState::CharacterSelect);
        CHECK(h.entries() == 0);
        CHECK(pulses == 0);
        for (int frame = 0; frame < 3; ++frame)
            if (!auto_sent) auto_sent = app::enter_character(h.rig.conn, *h.automation, "Gennro");
        CHECK_FALSE(auto_sent);
        CHECK(h.automation->plugin_consent()->serial == serial);
        CHECK(h.entries() == 0);

        h.automation->answer_plugin_consent(answer);
        if (!auto_sent) auto_sent = app::enter_character(h.rig.conn, *h.automation, "Gennro");
        CHECK(auto_sent);
        CHECK(h.automation->plugin_caps("plugin:MQ2Example") == (answer.value_or(false) ? plugin::kCapAll : 0));
        CHECK(pulses == 0);
        CHECK_FALSE(app::enter_character(h.rig.conn, *h.automation, "Gennro"));
        REQUIRE(h.rig.until_state(net::ConnectionState::InZone));
        CHECK(h.entries() == 1);
    }
}

TEST_CASE("character entry: Return Home waits for consent and preserves its exact request") {
    CharacterEntryHost h;
    CHECK_FALSE(app::enter_character(h.rig.conn, *h.automation, "Gennro", true));
    REQUIRE(h.automation->plugin_consent());
    h.automation->answer_plugin_consent(true);
    REQUIRE(app::enter_character(h.rig.conn, *h.automation, "Gennro", true));
    REQUIRE(h.rig.until_state(net::ConnectionState::InZone));
    CHECK(h.entries() == 1);
    for (const auto& packet : h.rig.server.world_log) {
        if (packet.opcode != net::to_wire(net::Op::EnterWorld)) continue;
        const auto request = net::decode_enter_world(packet);
        REQUIRE(request);
        CHECK(request->name == "Gennro");
        CHECK(request->return_home == 0x101);
    }
    // The character-entry helper is not the zone-transfer path; an already in-zone connection does not re-enter.
    CHECK_FALSE(app::enter_character(h.rig.conn, *h.automation, "Other"));
    CHECK(h.rig.conn.character_name() == "Gennro");
}

TEST_CASE("character entry: changed login rules retire the old server's pending question before entry") {
    CharacterEntryHost h;
    REQUIRE_FALSE(app::enter_character(h.rig.conn, *h.automation, "Gennro"));
    REQUIRE(h.automation->plugin_consent());
    net::GameConnectionTestAccess::set_server_rules(h.rig.conn, std::nullopt);
    CHECK(app::enter_character(h.rig.conn, *h.automation, "Gennro"));
    CHECK_FALSE(h.automation->plugin_consent());
    CHECK(h.automation->plugin_caps("plugin:MQ2Example") == 0);
    REQUIRE(h.rig.until_state(net::ConnectionState::InZone));
    CHECK(h.entries() == 1);
}

TEST_CASE("character entry: pending in-zone consent does not intercept ordinary world-mediated zoning") {
    CharacterEntryHost h(true, true);
    REQUIRE(app::enter_character(h.rig.conn, *h.automation, "Gennro"));
    REQUIRE(h.rig.until_state(net::ConnectionState::InZone));
    REQUIRE(plugin::forget_plugin_consent(h.profile, "MQ2Example"));
    plugin::Rules rules;
    rules.configured = true;
    rules.provisional = false;
    rules.dir = h.profile;
    std::ifstream in(h.file, std::ios::binary);
    const std::string bytes(std::istreambuf_iterator<char>(in), {});
    rules.server_files = {{plugin::FileKind::Plugin, "plugins/MQ2Example", sha256(bytes)}};
    net::GameConnectionTestAccess::set_server_rules(h.rig.conn, rules);
    h.automation->frame();
    REQUIRE(h.automation->plugin_consent());
    const auto serial = h.automation->plugin_consent()->serial;
    CHECK_FALSE(app::enter_character(h.rig.conn, *h.automation, "Gennro"));
    CHECK(h.automation->plugin_consent()->serial == serial);
    h.rig.server.request_zone_change(1, 20, 10, 3);
    REQUIRE(h.rig.until_state(net::ConnectionState::Zoning));
    REQUIRE(h.rig.until_state(net::ConnectionState::InZone));
    CHECK(h.entries() == 2);
    CHECK(h.rig.server.zone_entries == 2);
}

TEST_CASE("character entry: approved LOGIN, non-LOGIN and unconfigured policies need no pending answer") {
    for (int policy = 0; policy < 3; ++policy) {
        CAPTURE(policy);
        CharacterEntryHost h(policy != 1, policy == 0, policy != 2);
        REQUIRE(app::enter_character(h.rig.conn, *h.automation, "Gennro"));
        CHECK_FALSE(h.automation->plugin_consent());
        CHECK(h.automation->plugin_caps("plugin:MQ2Example") == (policy == 1 ? 0 : plugin::kCapAll));
        REQUIRE(h.rig.until_state(net::ConnectionState::InZone));
        CHECK(h.entries() == 1);
    }
}
#endif

TEST_CASE("connection front end: a required-addon wait cannot consume quit or exit") {
    for (const auto& command : {std::string("/quit"), std::string("/exit")}) {
        CAPTURE(command);
        // send_command dispatches game commands only with a zone session, just as it does while a required
        // add-on's consent prompt holds the frontend at the loading screen after the network zone-in.
        test::Rig rig;
        REQUIRE(rig.to_zone());
        auto& connection = rig.conn;
        std::optional<std::string> skin = "modern";
        std::vector<std::string> trace;
        connection.send_command(command);
        const bool proceed = app::connection_frontend(connection, skin,
            [&](const std::string& name) { trace.push_back("skin:" + name); },
            [&](net::GameConnection::QuitRequest request) {
                CHECK(request == (command == "/exit" ? net::GameConnection::QuitRequest::Exit
                                                      : net::GameConnection::QuitRequest::Quit));
                trace.push_back("quit");
                connection.disconnect();
            },
            [&] { trace.push_back("disconnected"); });
        if (proceed) trace.push_back("addon gate");
        CHECK_FALSE(proceed);
        CHECK_FALSE(skin);
        CHECK(trace == std::vector<std::string>{"skin:modern", "quit"});
        CHECK(connection.take_quit_request() == net::GameConnection::QuitRequest::None);
    }
}

TEST_CASE("connection front end: a disconnected loading connection leaves before the addon gate") {
    test::ScratchDir scratch("connection_front_drop");
    GameDir game(scratch.path());
    net::GameConnection connection(game);
    connection.disconnect();
    std::optional<std::string> skin = "gamepad";
    std::vector<std::string> trace;
    const bool proceed = app::connection_frontend(connection, skin,
        [&](const std::string& name) { trace.push_back("skin:" + name); },
        [&](net::GameConnection::QuitRequest) { trace.push_back("quit"); },
        [&] { trace.push_back("disconnected"); });
    if (proceed) trace.push_back("addon gate");
    CHECK_FALSE(proceed);
    CHECK_FALSE(skin);
    CHECK(trace == std::vector<std::string>{"skin:gamepad", "disconnected"});
}

TEST_CASE("connection front end: a pending skin loads before a required-addon wait") {
    test::ScratchDir scratch("connection_front_skin");
    GameDir game(scratch.path());
    net::GameConnection connection(game);
    std::optional<std::string> skin = "gamepad";
    std::vector<std::string> trace;
    const bool proceed = app::connection_frontend(connection, skin,
        [&](const std::string& name) { trace.push_back("skin:" + name); },
        [&](net::GameConnection::QuitRequest) { trace.push_back("quit"); },
        [&] { trace.push_back("disconnected"); });
    if (proceed) trace.push_back("addon gate");
    CHECK(proceed);
    CHECK_FALSE(skin);
    CHECK(trace == std::vector<std::string>{"skin:gamepad", "addon gate"});
}
