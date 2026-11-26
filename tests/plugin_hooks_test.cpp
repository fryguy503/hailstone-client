// plugin/hooks.h: the hook bus's order, dropping, commands and owner removal.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "plugin/hooks.h"

using namespace hailstone;
using plugin::Direction;
using plugin::Flow;
using plugin::Hooks;
using plugin::Stream;

TEST_CASE("hooks: packet hooks filter by stream, direction and opcode, change and drop") {
    Hooks h;
    auto a = h.new_owner("a");
    std::vector<int> seen;
    h.on_packet(a, Stream::Zone, Direction::In, 0x1234, [&](net::AppPacket& p) {
        seen.push_back(1);
        p.data.push_back(std::byte{7});
        return Flow::Continue;
    });
    h.on_packet(a, Stream::Zone, Direction::In, 0, [&](net::AppPacket& p) {
        seen.push_back(2);
        return p.opcode == 0x9999 ? Flow::Drop : Flow::Continue;
    });
    CHECK(h.has_packet_hooks(Stream::Zone, Direction::In));
    CHECK_FALSE(h.has_packet_hooks(Stream::Zone, Direction::Out));
    CHECK_FALSE(h.has_packet_hooks(Stream::World, Direction::In));
    net::AppPacket p(0x1234);
    CHECK(h.packet(Stream::Zone, Direction::In, p) == Flow::Continue);
    CHECK(p.data.size() == 1);
    CHECK(seen == std::vector<int>{1, 2});
    net::AppPacket q(0x9999);
    CHECK(h.packet(Stream::Zone, Direction::In, q) == Flow::Drop);
    net::AppPacket w(0x1234);
    CHECK(h.packet(Stream::World, Direction::In, w) == Flow::Continue);
    CHECK(w.data.empty());
}

TEST_CASE("hooks: commands are case-insensitive, the latest wins, removal brings back the one before") {
    Hooks h;
    auto a = h.new_owner("a"), b = h.new_owner("b");
    std::string got;
    h.add_command(a, "/Hello", [&](std::string_view args) { got = "a:" + std::string(args); });
    std::string line = "/hello  there world";
    CHECK(h.command(line));
    CHECK(got == "a:there world");
    h.add_command(b, "hello", [&](std::string_view args) { got = "b:" + std::string(args); });
    line = "/HELLO x";
    CHECK(h.command(line));
    CHECK(got == "b:x");
    h.remove(b);
    line = "/hello y";
    CHECK(h.command(line));
    CHECK(got == "a:y");
    line = "/other";
    CHECK_FALSE(h.command(line));
    CHECK(h.commands() == std::vector<std::string>{"/hello"});
}

TEST_CASE("hooks: the command catalog changes when add-on commands come and go") {
    Hooks hooks;
    auto guest = hooks.new_guest("addon");
    auto serial = hooks.command_catalog_serial();
    auto run = [](std::string_view) {};
    CHECK_FALSE(hooks.add_command(guest, "/say", run));
    CHECK(hooks.command_catalog_serial() == serial);
    REQUIRE(hooks.add_command(guest, "/ExampleAddon", run));
    CHECK(hooks.command_catalog_serial() != serial);
    CHECK(hooks.commands() == std::vector<std::string>{"/exampleaddon"});
    serial = hooks.command_catalog_serial();
    hooks.remove_command(guest, "unknown");
    CHECK(hooks.command_catalog_serial() == serial);
    hooks.remove_command(guest, "EXAMPLEADDON");
    CHECK(hooks.command_catalog_serial() != serial);
    CHECK(hooks.commands().empty());
    REQUIRE(hooks.add_command(guest, "exampleaddon", run));
    serial = hooks.command_catalog_serial();
    hooks.remove(guest);
    CHECK(hooks.command_catalog_serial() != serial);
    CHECK(hooks.commands().empty());
}

TEST_CASE("hooks: a plugin's or script's owner cannot take the client's own commands") {
    Hooks h;
    auto client = h.new_owner("lua"), g1 = h.new_guest("plugin:a"), g2 = h.new_guest("lua:b");
    std::string got;
    h.add_command(client, "/lua", [&](std::string_view) { got = "client"; });
    for (const char* name : {"/say", "/tell", "/g", "/gu", "/r", "/LUA", "lua", "/", "/who"}) {
        INFO(name);
        CHECK(h.reserved_command(name));
        CHECK_FALSE(h.add_command(g1, name, [&](std::string_view) { got = "guest"; }));
    }
    std::string line = "/lua run x";
    CHECK(h.command(line));
    CHECK(got == "client");
    line = "/g hello";
    CHECK_FALSE(h.command(line)); // left for the game's /gsay
    // A new name is the guest's; another guest may take it over (MQ's latest wins), and give it back.
    CHECK_FALSE(h.reserved_command("/mytool"));
    CHECK(h.add_command(g1, "/mytool", [&](std::string_view) { got = "a"; }));
    CHECK_FALSE(h.reserved_command("/mytool"));
    CHECK(h.add_command(g2, "/mytool", [&](std::string_view) { got = "b"; }));
    line = "/mytool";
    CHECK(h.command(line));
    CHECK(got == "b");
    h.remove(g2);
    line = "/mytool";
    CHECK(h.command(line));
    CHECK(got == "a");
    // The client's own owners are not held to it.
    CHECK(h.add_command(client, "/who", [&](std::string_view) {}));
}

TEST_CASE("hooks: line filters rewrite or take a line before the commands") {
    Hooks h;
    auto a = h.new_owner("a");
    h.on_command_line(a, [](std::string& line) {
        if (line == "/secret") return Flow::Drop;
        if (auto at = line.find("${Me}"); at != std::string::npos) line.replace(at, 5, "Soandso");
        return Flow::Continue;
    });
    std::string line = "/tell ${Me} hi";
    CHECK_FALSE(h.command(line));
    CHECK(line == "/tell Soandso hi");
    line = "/secret";
    CHECK(h.command(line));
}

TEST_CASE("hooks: chat lines change or drop; a hook removing its owner mid-event stops the rest") {
    Hooks h;
    auto a = h.new_owner("a"), b = h.new_owner("b");
    int b_calls = 0;
    h.on_chat(a, [&](plugin::ChatLine& l) {
        if (l.text == "spam") return Flow::Drop;
        l.text += "!";
        if (l.text == "bye!") h.remove(b);
        return Flow::Continue;
    });
    h.on_chat(b, [&](plugin::ChatLine&) {
        ++b_calls;
        return Flow::Continue;
    });
    plugin::ChatLine l{"hi", 0, 0};
    CHECK(h.chat(l) == Flow::Continue);
    CHECK(l.text == "hi!");
    CHECK(b_calls == 1);
    plugin::ChatLine spam{"spam", 0, 0};
    CHECK(h.chat(spam) == Flow::Drop);
    CHECK(b_calls == 1);
    plugin::ChatLine bye{"bye", 0, 0};
    h.chat(bye);
    CHECK(b_calls == 1);
}

TEST_CASE("hooks: events reach every owner; packets sent from packet hooks stop nesting") {
    Hooks h;
    auto a = h.new_owner("a");
    int pulses = 0, states = 0, zones = 0, spawns = 0;
    h.on_pulse(a, [&] { ++pulses; });
    h.on_game_state(a, [&](int s) { states += s; });
    h.on_zone(a, [&](plugin::ZonePhase) { ++zones; });
    h.on_spawn(a, [&](std::uint32_t id, bool added) { spawns += added ? static_cast<int>(id) : -1; });
    h.pulse();
    h.game_state(plugin::kGameStateInGame);
    h.zone(plugin::ZonePhase::Zoned);
    CHECK(h.has_spawn_hooks());
    h.spawn(5, true);
    h.spawn(5, false);
    CHECK(pulses == 1);
    CHECK(states == 5);
    CHECK(zones == 1);
    CHECK(spawns == 4);
    int depth = 0;
    h.on_packet(a, Stream::Zone, Direction::Out, 0, [&](net::AppPacket& p) {
        ++depth;
        net::AppPacket again(p.opcode);
        h.packet(Stream::Zone, Direction::Out, again); // a plugin sending from its own hook
        return Flow::Continue;
    });
    net::AppPacket p(1);
    h.packet(Stream::Zone, Direction::Out, p);
    CHECK(depth == 8);
    h.remove(a);
    CHECK_FALSE(h.has_spawn_hooks());
    h.pulse();
    CHECK(pulses == 1);
}

TEST_CASE("hooks: a command that runs itself stops nesting instead of overflowing the stack") {
    plugin::Hooks h;
    auto o = h.new_owner("p");
    int runs = 0;
    h.add_command(o, "/again", [&](std::string_view) {
        ++runs;
        std::string line = "/again";
        CHECK(h.command(line)); // past the limit: taken (dropped), not run
    });
    std::string line = "/again";
    CHECK(h.command(line));
    CHECK(runs == 64);
    // The depth comes back: the next line runs again.
    runs = 0;
    line = "/again";
    CHECK(h.command(line));
    CHECK(runs == 64);
}

TEST_CASE("hooks: hooks added or removed while an event runs keep the event's own list") {
    Hooks h;
    auto a = h.new_owner("a"), b = h.new_owner("b"), c = h.new_owner("c");
    std::vector<std::string> seen;
    int added_runs = 0;
    bool first = true;
    h.on_pulse(a, [&] {
        seen.push_back("a");
        if (!first) return;
        first = false;
        h.remove(b); // a later hook removed mid-event: it does not run, now or later
        // Enough new hooks that the list grows its storage while it is being walked: none runs this event.
        for (int i = 0; i < 64; ++i) h.on_pulse(c, [&] { ++added_runs; });
        // A nested event walks the list with the new hooks in it.
        h.pulse();
    });
    h.on_pulse(b, [&] { seen.push_back("b"); });
    h.on_pulse(c, [&] { seen.push_back("c"); });
    h.pulse();
    // The outer event: a, (nested: a, c, 64 added), c. The added hooks ran only in the nested event.
    CHECK(seen == std::vector<std::string>{"a", "a", "c", "c"});
    CHECK(added_runs == 64);
    seen.clear();
    h.pulse();
    CHECK(seen == std::vector<std::string>{"a", "c"});
    CHECK(added_runs == 128);
    h.remove(c);
    h.pulse();
    CHECK(added_runs == 128);
}
