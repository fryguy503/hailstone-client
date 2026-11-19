#pragma once

// A GameConnection wired to a MockEqServer. Both are driven from one loop on the test thread: the connection's
// update() and the mock's poll(). The loop only sleeps a millisecond between passes, and every wait ends as soon as
// its condition holds, so a test costs what its protocol exchange costs; the deadline is a safety net for a hang.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "core/game_dir.h"
#include "manual_clock.h"
#include "mock_server/mock_eqemu.h"
#include "net/game_connection.h"

namespace hailstone::test {

// An empty directory standing in for the RoF2 install: GameConnection reads its data files from it and, when they
// are missing, warns and runs with built-in defaults. (Tests that need real files use tests/game_files.h and
// skip without EQ_GAME_DIR.)
//
// It holds one data file, eqstr_us.txt, with fixture templates for the few messages the tests read, so the lines
// can be asserted on. The ids and their use are the ones docs/protocol/rof2-packets.md names (1410 say, 1414 group,
// 1420 shout, whose arguments are the speaker, the language and the text); the texts are the fixture's own, not the
// game's.
class EmptyGameDir {
public:
    EmptyGameDir() {
        static int counter = 0;
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("hailstone_mock_game_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
        std::ofstream(path_ / "eqstr_us.txt") << "EQST0002\n"
                                              << "1410 SAY %1%2: %3\n"
                                              << "1414 GROUP %1%2: %3\n"
                                              << "1420 SHOUT %1%2: %3\n"
                                              << "1400 TOLD %1: %2\n"
                                              << "12293 CAMPING\n"
                                              << "12946 off\n"
                                              << "12947 on\n"
                                              << "13867 Bash\n"  // a skill's name, as CSkillMgr_GetNameToken asks for it
                                              << "13889 Forage\n"
                                              << "13893 Kick\n"
                                              << "9342 NO ROOM FOR THE BANDOLIER\n"
                                              << "4557 SHROUD BANK NOT EMPTY\n"
                                              << "4558 SHROUD BANK NOTHING TO MOVE\n"
                                              << "6188 SHROUD COIN %1\n";
    }
    ~EmptyGameDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    EmptyGameDir(const EmptyGameDir&) = delete;
    EmptyGameDir& operator=(const EmptyGameDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

using namespace std::chrono_literals;

// Runs `step` (the loop body) until `done` holds or the deadline passes; true when done held.
inline bool pump(const std::function<void()>& step, const std::function<bool()>& done,
                 std::chrono::milliseconds limit = 10s) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (true) {
        step();
        if (done()) return true;
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
}

// The connection's session timers shortened so a zone-in takes milliseconds instead of waiting out the 150 ms
// multi-buffer hold at every hop: no hold, a 2 ms service pass, resends 50 ms after the round trip estimate. The
// protocol is the same; ConnectionOptions{} (the original's timers) is used by the one test that checks those.
inline net::ConnectionOptions quick_options() {
    net::ConnectionOptions o;
    o.session.hold_time = 0ms;
    o.session.service_interval = 2ms;
    o.session.resend_delay_adjust = 50;
    return o;
}

struct Rig {
    MockEqServer server;
    EmptyGameDir dir;
    GameDir game;
    net::GameConnection conn;
    std::function<void()> on_step; // after each update (a test's plugins: their OnPulse)

    explicit Rig(MockConfig config = MockConfig::standard(), net::ConnectionOptions options = quick_options())
        : server(std::move(config)), game(dir.path()), conn(game, std::move(options)) {}

    void step() {
        conn.update();
        if (on_step) on_step();
        server.poll();
    }

    bool until(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s) {
        return pump([&] { step(); }, done, limit);
    }
    bool until_state(net::ConnectionState s, std::chrono::milliseconds limit = 10s) {
        return until([&] { return conn.state() == s; }, limit);
    }
    // Runs the loop for a fixed time: only for assertions that something does NOT happen.
    void run_for(std::chrono::milliseconds t) {
        auto end = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < end) {
            step();
            std::this_thread::sleep_for(1ms);
        }
    }

    // start() at the world server, up to the character list.
    bool to_character_select() {
        conn.start(server.ticket());
        return until_state(net::ConnectionState::CharacterSelect);
    }
    // ... and through the world and the zone-in to InZone. The mock answers the client's last zone-in packet
    // (OP_ClientReady) with an OP_HPUpdate; once that is applied, everything the server sent during the zone-in
    // (the stand state before it, in order) has been handled too.
    bool to_zone(const std::string& character = "Gennro") {
        if (!to_character_select()) return false;
        conn.enter_world(character);
        if (!until_state(net::ConnectionState::InZone)) return false;
        return until([&] { return conn.player_status().max_hp != 0; });
    }

    std::vector<net::ChatEvent> chat() {
        std::vector<net::ChatEvent> out;
        while (auto e = conn.poll_chat()) out.push_back(std::move(*e));
        return out;
    }
};

inline bool has_line(const std::vector<net::ChatEvent>& lines, const std::string& text) {
    for (const auto& l : lines)
        if (l.text == text) return true;
    return false;
}

} // namespace hailstone::test
