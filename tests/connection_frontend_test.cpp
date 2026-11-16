#include <doctest/doctest.h>

#include "app/connection_frontend.h"
#include "core/game_dir.h"
#include "mock_server/harness.h"
#include "scratch_dir.h"

using namespace hailstone;

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
            [&] { trace.push_back("assets"); },
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
        CHECK(trace == std::vector<std::string>{"quit"});
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
        [&] { trace.push_back("assets"); },
        [&](const std::string& name) { trace.push_back("skin:" + name); },
        [&](net::GameConnection::QuitRequest) { trace.push_back("quit"); },
        [&] { trace.push_back("disconnected"); });
    if (proceed) trace.push_back("addon gate");
    CHECK_FALSE(proceed);
    CHECK_FALSE(skin);
    CHECK(trace == std::vector<std::string>{"disconnected"});
}

TEST_CASE("connection front end: a pending skin loads before a required-addon wait") {
    test::ScratchDir scratch("connection_front_skin");
    GameDir game(scratch.path());
    net::GameConnection connection(game);
    std::optional<std::string> skin = "gamepad";
    std::vector<std::string> trace;
    const bool proceed = app::connection_frontend(connection, skin,
        [&] { trace.push_back("assets"); },
        [&](const std::string& name) { trace.push_back("skin:" + name); },
        [&](net::GameConnection::QuitRequest) { trace.push_back("quit"); },
        [&] { trace.push_back("disconnected"); });
    if (proceed) trace.push_back("addon gate");
    CHECK(proceed);
    CHECK_FALSE(skin);
    CHECK(trace == std::vector<std::string>{"assets", "skin:gamepad", "addon gate"});
}
