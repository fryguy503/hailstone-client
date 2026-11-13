// plugin/native_plugins.h with the example plugin (plugins/example/MQ2Example.c, built next to the tests): loaded when
// the server's rules allow it, its MQ callbacks on the hook bus, held to its capabilities.
#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <fstream>
#include <thread>

#include "core/sha256.h"
#include "mq/commands.h"
#include "mq/parse.h"
#include "platform/tcp_socket.h"
#include "plugin/native_plugins.h"
#include "mq_test_world.h"
#include "plugin_consent_helper.h"
#include "app/web_service.h"
#include "plugin/web_access.h"
#include "web/browser_host.h"

#define HAILSTONE_HOST // the SDK's structs only, not the plugin side
#include "hailstone_plugin.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Host {
    mq_test::World world{"hailstone_native_plugins_test"};
    fs::path plugin_dir = world.dir / "plugins";
    plugin::Hooks hooks;
    plugin::Policy policy;
    mq::Registry reg;
    mq::Context ctx;
    std::vector<std::string> printed, sent;
    std::vector<std::pair<plugin::Stream, net::AppPacket>> packets;
    std::unique_ptr<mq::Commands> commands;
    std::unique_ptr<plugin::NativePlugins> plugins;
    fs::path file;

    // own_folder: the binary in the plugin's own folder, plugins/MQ2Example/, not directly in plugins/.
    // `built`: the plugin to put in the folder (the example; tests/plugins/HostProbe.cpp ...).
    explicit Host(bool own_folder = false, plugin::WebAccess* web = nullptr, fs::path built = HAILSTONE_EXAMPLE_PLUGIN) {
        fs::path dir = own_folder ? plugin_dir / "MQ2Example" : plugin_dir;
        fs::create_directories(dir);
        file = dir / built.filename();
        fs::copy_file(built, file, fs::copy_options::overwrite_existing);
        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        mq::CommandEnv ce;
        ce.hooks = &hooks;
        ce.registry = &reg;
        ce.context = [this]() -> const mq::Context& { return ctx; };
        ce.echo = [this](const std::string& t) { printed.push_back(t); };
        ce.run = [this](const std::string& l) { run(l); };
        commands = std::make_unique<mq::Commands>(ce);
        plugin::NativeEnv ne;
        ne.hooks = &hooks;
        ne.policy = &policy;
        ne.registry = &reg;
        ne.context = ce.context;
        ne.print = [this](const std::string& t, std::uint32_t) { printed.push_back(t); };
        ne.run = ce.run;
        ne.send = [this](plugin::Stream s, const net::AppPacket& p) { packets.emplace_back(s, p); };
        ne.dirs = {plugin_dir};
        ne.cache_dir = world.dir / "cache";
        ne.config_dir = world.dir / "config";
        ne.web = web;
        ne.connection = [this] { return world.conn.get(); };
        plugins = std::make_unique<plugin::NativePlugins>(ne);
    }
    ~Host() {
        plugins.reset();
        commands.reset();
    }
    Sha256Digest hash() const {
        std::ifstream f(file, std::ios::binary);
        return sha256(std::string(std::istreambuf_iterator<char>(f), {}));
    }
    // A hailstone.cfg letting the player's own plugins do `caps` (0: not at all).
    void approve(std::uint32_t caps) {
        plugin::Rules r;
        r.configured = true;
        r.player_plugin_caps = caps;
        policy.set(r);
    }
    // A hailstone.cfg with the plugin as the server's own, the player's not allowed.
    void server_plugin() {
        plugin::Rules r;
        r.configured = true;
        r.server_files.push_back({plugin::FileKind::Plugin, "plugins/MQ2Example", hash()});
        policy.set(r);
    }
    void run(const std::string& line) {
        std::string l = line;
        if (hooks.command(l)) return;
        sent.push_back(l);
    }
    std::string p(std::string_view text) { return mq::parse_macro_data(ctx, text); }
    bool printed_line(const std::string& t) const { return std::find(printed.begin(), printed.end(), t) != printed.end(); }
};

} // namespace

TEST_CASE("native plugins: off before the zone-in; free without a hailstone.cfg; the player's only when it says") {
    Host h;
    auto all = h.plugins->list();
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "MQ2Example");
    CHECK(all[0].caps == 0);
    std::string why;
    CHECK_FALSE(h.plugins->load("MQ2Example", &why));
    CHECK(why.find("until the zone-in") != std::string::npos);
    // A hailstone.cfg allowing the player's scripts only.
    plugin::Rules scripts_only;
    scripts_only.configured = true;
    scripts_only.player_script_caps = plugin::kCapAll;
    h.policy.set(scripts_only);
    CHECK_FALSE(h.plugins->load("mq2example", &why));
    CHECK(why.find("runs only its own plugins") != std::string::npos);
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    // No hailstone.cfg: no rules.
    h.policy.set(plugin::Rules{});
    REQUIRE(h.plugins->load("MQ2Example"));
    CHECK(h.plugins->list()[0].caps == plugin::kCapAll);
    h.plugins->unload("MQ2Example");
}

TEST_CASE("native plugins: MQ callbacks, commands and TLOs, then unloaded cleanly") {
    Host h;
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("MQ2Example"));
    CHECK(h.printed_line("[MQ2Example] loaded"));
    h.run("/hello there");
    CHECK(h.printed_line("[MQ2Example] Hello, there!"));
    CHECK(h.p("${Example}") == "Example");
    h.hooks.pulse();
    h.hooks.pulse();
    CHECK(h.p("${Example.Pulses}") == "2");
    CHECK(h.p("${Example[abc].Index}") == "abc");
    CHECK(h.p("${Example.Twice[21]}") == "42");
    CHECK(h.p("${Example.MyHP}") == "75");
    CHECK(h.p("${Example.Nope}") == "NULL");
    h.hooks.zone(plugin::ZonePhase::Zoned);
    CHECK(h.p("${Example.Zones}") == "1");
    plugin::ChatLine hi{"hello", 0, 0}, spam{"buy spam now", 0, 0};
    CHECK(h.hooks.chat(hi) == plugin::Flow::Continue);
    CHECK(h.hooks.chat(spam) == plugin::Flow::Drop);
    CHECK(h.p("${Example.Chats}") == "2");
    // Packets: an incoming 0x7e57 dropped, an outgoing 0x7e58 rewritten.
    net::AppPacket in(0x7e57), keep(0x1234), out(0x7e58, std::vector<std::byte>(10));
    CHECK(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, in) == plugin::Flow::Drop);
    CHECK(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, keep) == plugin::Flow::Continue);
    CHECK(h.hooks.packet(plugin::Stream::World, plugin::Direction::Out, out) == plugin::Flow::Continue);
    REQUIRE(out.data.size() == 4);
    CHECK(out.data[3] == std::byte{4});
    h.run("/plugin list");
    CHECK(h.printed_line("[Plugin] MQ2Example - loaded (" + plugin::caps_text(plugin::kCapAll) + ")"));
    h.run("/plugin MQ2Example unload");
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK_FALSE(h.hooks.has_command("/hello"));
    CHECK(h.p("${Example}") == "NULL");
    CHECK_FALSE(h.hooks.has_packet_hooks(plugin::Stream::Zone, plugin::Direction::In));
}

TEST_CASE("native plugins: a TLO name already taken is refused, and stays after the plugin unloads") {
    Host h;
    h.approve(plugin::kCapAll);
    h.reg.add_tlo("Example", [](const mq::Context&, std::string_view) { return mq::Value::string("client's"); });
    REQUIRE(h.plugins->load("MQ2Example"));
    CHECK(h.p("${Example}") == "client's");
    // Its RemoveTopLevelObject("Example") at shutdown leaves the one it did not add.
    h.plugins->unload("MQ2Example");
    CHECK(h.p("${Example}") == "client's");
    CHECK(h.reg.has_tlo("Me"));
}

TEST_CASE("native plugins: callbacks it may not use are left off; the policy can unload it") {
    Host h;
    h.approve(plugin::kCapCommands); // no read, events or packets
    REQUIRE(h.plugins->load("MQ2Example"));
    bool chat_denied = false, packets_denied = false;
    for (const auto& l : h.printed) {
        chat_denied |= l.find("OnIncomingChat needs the server's 'events' permission") != std::string::npos;
        packets_denied |= l.find("OnPacket needs the server's 'packets' permission") != std::string::npos;
    }
    CHECK(chat_denied);
    CHECK(packets_denied);
    CHECK_FALSE(h.hooks.has_packet_hooks(plugin::Stream::Zone, plugin::Direction::In));
    plugin::ChatLine spam{"spam", 0, 0};
    CHECK(h.hooks.chat(spam) == plugin::Flow::Continue);
    CHECK(h.p("${Example.MyHP}") == "NULL"); // its ParseMacroData was refused
    // DoCommand is held to the plugin's capabilities: /hello is its own command (commands), but no chat.
    h.run("/hello");
    CHECK(h.printed_line("[MQ2Example] Hello, world!"));
    h.approve(0);
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    // Approved again with "*" autoload: it comes back by itself.
    h.approve(plugin::kCapAll);
    h.plugins->policy_changed({"*"});
    CHECK(h.plugins->loaded("MQ2Example"));
}

// A narrower policy keeps the plugin loaded (it still has caps), and its hooks stay registered, but it no longer
// sees the chat, packets or spawns it lost the permission for.
TEST_CASE("native plugins: a plugin whose capabilities narrow loses its chat and packet hooks at once") {
    Host h;
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("MQ2Example"));
    plugin::ChatLine spam{"buy spam now", 0, 0};
    net::AppPacket in(0x7e57);
    REQUIRE(h.hooks.chat(spam) == plugin::Flow::Drop);
    REQUIRE(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, in) == plugin::Flow::Drop);
    h.approve(plugin::kCapCommands | plugin::kCapRead);
    h.plugins->policy_changed({"*"});
    REQUIRE(h.plugins->loaded("MQ2Example"));
    plugin::ChatLine spam2{"buy spam now", 0, 0};
    net::AppPacket in2(0x7e57);
    CHECK(h.hooks.chat(spam2) == plugin::Flow::Continue);
    CHECK(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, in2) == plugin::Flow::Continue);
    CHECK(h.p("${Example.Chats}") == "1"); // the second line never reached it
    // Given back: it sees them again (its hooks were never taken out).
    h.approve(plugin::kCapAll);
    h.plugins->policy_changed({"*"});
    plugin::ChatLine spam3{"buy spam now", 0, 0};
    CHECK(h.hooks.chat(spam3) == plugin::Flow::Drop);
}

TEST_CASE("native plugins: AddCommand needs 'commands'") {
    Host h;
    h.approve(plugin::kCapRead);
    REQUIRE(h.plugins->load("MQ2Example"));
    CHECK_FALSE(h.hooks.has_command("/hello"));
    bool denied = false;
    for (const auto& l : h.printed) denied |= l.find("AddCommand needs the server's 'commands' permission") != std::string::npos;
    CHECK(denied);
}

TEST_CASE("native plugins: the server's own load by themselves and stay; an edited copy is the player's") {
    Host h;
    h.server_plugin();
    h.plugins->policy_changed({}); // nothing named to autoload: the server's load anyway
    REQUIRE(h.plugins->loaded("MQ2Example"));
    CHECK(h.plugins->list()[0].server);
    CHECK(h.plugins->list()[0].kept);
    h.run("/plugin MQ2Example unload");
    CHECK(h.plugins->loaded("MQ2Example"));
    CHECK(h.printed_line("[Plugin] MQ2Example is this server's own plugin: it stays loaded"));
    h.plugins->unload_all();
    { std::ofstream(h.file, std::ios::binary | std::ios::app) << "x"; }
    std::string why;
    CHECK_FALSE(h.plugins->load("MQ2Example", &why));
    CHECK(why.find("runs only its own plugins") != std::string::npos);
}

TEST_CASE("native plugins: the server's plugins folder comes before the player's") {
    Host h;
    fs::path server = h.world.dir / "server" / "plugins";
    fs::create_directories(server);
    fs::copy_file(h.file, server / h.file.filename());
    h.plugins->set_server_dir(server);
    h.server_plugin(); // the server's copy's hash
    REQUIRE(test::answer_server_plugin(server.parent_path(), server / h.file.filename())); // the player said yes
    { std::ofstream(h.file, std::ios::binary | std::ios::app) << "x"; } // the player's differs
    // The same name in both folders: listed once, as the server's, which is the one that loads.
    auto all = h.plugins->list();
    REQUIRE(all.size() == 1);
    CHECK(all[0].file == server / h.file.filename());
    CHECK(all[0].server);
    REQUIRE(h.plugins->load("MQ2Example"));
    all = h.plugins->list();
    REQUIRE(all.size() == 1);
    CHECK(all[0].loaded);
    CHECK(all[0].file == server / h.file.filename());
    h.plugins->unload_all();
    CHECK(h.printed_line("[MQ2Example] loaded"));
}

namespace {
// The example plugin as a server sent it: only in servers/alpha/plugins (the player's copy gone), the server's own
// under its hailstone.cfg. Returns the server's folder (servers/alpha).
fs::path from_server(Host& h) {
    const fs::path profile = h.world.dir / "servers" / "alpha";
    fs::create_directories(profile / "plugins");
    const fs::path copy = profile / "plugins" / h.file.filename();
    fs::copy_file(h.file, copy, fs::copy_options::overwrite_existing);
    fs::remove(h.file);
    h.file = copy;
    h.plugins->set_server_dir(profile / "plugins");
    h.server_plugin();
    return profile;
}
} // namespace

TEST_CASE("native plugins: the server's compiled plugin waits for the player's yes, kept for its bytes") {
    Host h;
    const fs::path profile = from_server(h);
    const std::uint64_t size = fs::file_size(h.file);
    CHECK_FALSE(h.plugins->consent());
    h.plugins->policy_changed({}); // the zone-in: it would load, so it asks
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    auto q = h.plugins->consent();
    REQUIRE(q);
    CHECK(q->profile_dir == profile);
    REQUIRE(q->files.size() == 1);
    CHECK(q->files[0].name == "MQ2Example");
    CHECK(q->files[0].file == h.file);
    CHECK(q->files[0].size == size);
    CHECK(q->files[0].sha256 == h.hash());
    CHECK(h.printed_line("[Plugin] MQ2Example waits for your answer: this server's compiled plugins load only once "
                         "you allow them"));
    auto all = h.plugins->list();
    REQUIRE(all.size() == 1);
    CHECK(all[0].asks);
    CHECK(all[0].consent == plugin::Consent::Unasked);
    // Asked once: the rules coming again do not ask twice, nor say it again.
    const auto serial = q->serial;
    const auto said = h.printed.size();
    h.plugins->policy_changed({});
    CHECK(h.plugins->consent()->serial == serial);
    CHECK(h.printed.size() == said);
    h.run("/plugin list");
    CHECK(h.printed_line("[Plugin] MQ2Example - not loaded: waits for your answer (" + plugin::caps_text(plugin::kCapAll) + ")"));

    h.plugins->answer_consent(true);
    CHECK_FALSE(h.plugins->consent());
    CHECK(h.plugins->loaded("MQ2Example"));
    CHECK(plugin::plugin_consent(profile, h.hash()) == plugin::Consent::Allowed);
    CHECK(fs::is_regular_file(profile / plugin::kApprovalsName));
    CHECK(h.plugins->list()[0].consent == plugin::Consent::Allowed);

    // The next login: allowed already (the file says so), it loads without a question.
    h.plugins->unload_all();
    h.policy.clear();
    h.plugins->policy_changed({});
    h.server_plugin();
    h.plugins->policy_changed({});
    CHECK(h.plugins->loaded("MQ2Example"));
    CHECK_FALSE(h.plugins->consent());
    h.plugins->unload_all();
    // Other bytes: asked again (the yes was for the old ones).
    { std::ofstream(h.file, std::ios::binary | std::ios::app) << "x"; }
    h.server_plugin();
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    REQUIRE(h.plugins->consent());
    CHECK(h.plugins->consent()->files[0].sha256 == h.hash());
    CHECK(h.plugins->list()[0].consent == plugin::Consent::Unasked);
}

TEST_CASE("native plugins: a server plugin the player refused stays unloaded and is not asked about at each zone-in") {
    Host h;
    const fs::path profile = from_server(h);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(false);
    CHECK_FALSE(h.plugins->consent());
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK(plugin::plugin_consent(profile, h.hash()) == plugin::Consent::Denied);
    CHECK(h.printed_line("[Plugin] MQ2Example is not loaded: you did not allow it. The add-on list on the login screen "
                         "can ask again."));
    // The next zone-in: no question, nothing loads, nothing said.
    const auto said = h.printed.size();
    h.policy.clear();
    h.plugins->policy_changed({});
    h.server_plugin();
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->consent());
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK(h.printed.size() == said);
    CHECK(h.plugins->list()[0].consent == plugin::Consent::Denied);
    // The player loading it asks again.
    std::string why;
    CHECK_FALSE(h.plugins->load("MQ2Example", &why));
    CHECK(why.find("waits for your answer") != std::string::npos);
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(true);
    CHECK(h.plugins->loaded("MQ2Example"));
    // Taken back (the add-on list's "Don't allow it any more"): the next check of the rules unloads it, though it is
    // the server's own and kept.
    REQUIRE(plugin::revoke_plugin_consent(profile, "MQ2Example"));
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK(h.printed_line("[Plugin] MQ2Example: you no longer allow it on this server"));
    CHECK_FALSE(h.plugins->consent());
    // Forgotten ("Ask me again"): asked at the next check.
    REQUIRE(plugin::forget_plugin_consent(profile, "MQ2Example"));
    h.plugins->policy_changed({});
    CHECK(h.plugins->consent());
}

TEST_CASE("native plugins: a script's /plugin load never puts the question up, after a no or a put away") {
    Host h;
    from_server(h);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(false);
    REQUIRE_FALSE(h.plugins->consent());
    auto from_script = [&] {
        plugin::CallerScope scope(plugin::kCapAll, "serverscript"); // as mq.cmd('/plugin MQ2Example load')
        h.run("/plugin MQ2Example load");
    };
    from_script();
    CHECK_FALSE(h.plugins->consent());
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK(h.printed_line("[Plugin] MQ2Example is not loaded: this server's compiled plugins load only once you allow "
                         "them, and only you can ask for that"));
    // Typed by the player: asked again; put away, a script cannot bring it back either.
    h.run("/plugin MQ2Example load");
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(std::nullopt);
    from_script();
    CHECK_FALSE(h.plugins->consent());
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
}

TEST_CASE("native plugins: a server plugin's question put away comes back; out of the world or on another server it goes") {
    Host h;
    const fs::path profile = from_server(h);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(std::nullopt); // the close box: nothing kept
    CHECK_FALSE(h.plugins->consent());
    CHECK(plugin::plugin_consent(profile, h.hash()) == plugin::Consent::Unasked);
    CHECK_FALSE(fs::exists(profile / plugin::kApprovalsName));
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    // Disconnected: nothing runs, no question.
    h.policy.clear();
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->consent());
    h.server_plugin();
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    // Another server's folder: the question was this server's.
    h.plugins->set_server_dir(h.world.dir / "servers" / "beta" / "plugins");
    CHECK_FALSE(h.plugins->consent());
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
}

TEST_CASE("native plugins: another window's answer in the file settles the question") {
    Host h;
    const fs::path profile = from_server(h);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->recheck_consent();
    REQUIRE(h.plugins->consent()); // nothing answered yet
    REQUIRE(test::answer_server_plugin(profile, h.file)); // a box set's other Hailstone said yes
    h.plugins->recheck_consent();
    CHECK_FALSE(h.plugins->consent());
    CHECK(h.plugins->loaded("MQ2Example"));
}

TEST_CASE("native plugins: the player's own plugins do not ask") {
    Host h;
    const fs::path profile = h.world.dir / "servers" / "alpha";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins"); // the server sent none
    h.approve(plugin::kCapAll);
    h.plugins->policy_changed({"*"});
    CHECK(h.plugins->loaded("MQ2Example"));
    CHECK_FALSE(h.plugins->consent());
    CHECK_FALSE(h.plugins->list()[0].asks);
    CHECK_FALSE(fs::exists(profile / plugin::kApprovalsName));
}

TEST_CASE("native plugins: a binary in the plugin's own folder under plugins/") {
    Host h(true);
    auto all = h.plugins->list();
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "MQ2Example");
    CHECK(all[0].file == h.file);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("MQ2Example"));
    CHECK(h.plugins->loaded("MQ2Example"));
    h.plugins->unload("MQ2Example");
}

#ifdef HAILSTONE_MOCK_WEB_HELPER
// ABI 3 (sdk/hailstone_plugin.h web_*) through the example plugin's /webexample, over the app's WebService and the
// stand-in helper: the plugin's own tab, a message both ways, the server's sites, 'web' required, closed at unload.
TEST_CASE("native plugins: web pages of the plugin's own (ABI 3), under 'web', closed when it unloads") {
    web::BrowserHost browser([] { return web::process_transport({HAILSTONE_MOCK_WEB_HELPER}); });
    app::WebService web({[&] { return &browser; }, [] { return nullptr; }, [] { return std::string(); },
                         [] { return std::vector<std::string>{"example.org"}; }, [] {}});
    {
        Host h(false, &web);
        h.approve(plugin::kCapAll);
        REQUIRE(h.plugins->load("MQ2Example"));
        h.run("/webexample https://evil.com/");
        CHECK(web.tabs("plugin:MQ2Example").empty());
        bool said = false;
        for (const auto& line : h.printed) said |= line.find("not on a site") != std::string::npos;
        CHECK(said);
        h.run("/webexample https://example.org/");
        const auto tabs = web.tabs("plugin:MQ2Example");
        REQUIRE(tabs.size() == 1);
        CHECK(h.printed_line("[MQ2Example] page " + std::to_string(tabs[0]) + " open"));
        h.run("/webexample post ping");
        // Up to 10 s of real time (a real process answers through real pipes), not a count of polls: on a loaded
        // machine 3000 polls without a pause passed before the helper was scheduled.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!h.printed_line("[MQ2Example] page: echo:ping") && std::chrono::steady_clock::now() < until) {
            browser.poll(0);
            h.hooks.pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(h.printed_line("[MQ2Example] page: echo:ping"));
        CHECK(web.tabs("").empty()); // not the player's
        REQUIRE(h.plugins->unload("MQ2Example"));
        CHECK(web.tabs("plugin:MQ2Example").empty());
    }
    {
        Host h(false, &web);
        h.approve(plugin::kCapAll & ~plugin::kCapWeb);
        REQUIRE(h.plugins->load("MQ2Example"));
        h.run("/webexample https://example.org/");
        CHECK(web.tabs("plugin:MQ2Example").empty());
        CHECK(h.printed_line("[Plugin] MQ2Example: web_open needs the server's 'web' permission"));
    }
    browser.shutdown();
}
#endif

TEST_CASE("native plugins: host functions from another thread do nothing; the caller's capabilities are none there") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probethread");
    CHECK(h.printed_line("[Probe] thread: add_command 0, caps 0, imgui null"));
    CHECK_FALSE(h.printed_line("[Probe] written from the thread"));
    CHECK_FALSE(h.printed_line("from the thread"));
    CHECK_FALSE(h.hooks.has_command("/fromthread"));
    // The caller's state belongs to the client's thread: another sees no capabilities and changes nothing.
    plugin::CallerScope scope(plugin::kCapRead, "script");
    std::uint32_t caps = 1;
    std::string name;
    std::thread([&] {
        plugin::CallerScope other(plugin::kCapAll, "thread");
        caps = plugin::caller_caps();
        name = std::string(plugin::caller_name());
    }).join();
    CHECK(caps == 0);
    CHECK(name == "another thread");
    CHECK(plugin::caller_caps() == plugin::kCapRead);
    CHECK(plugin::caller_name() == "script");
}

TEST_CASE("native plugins: a connection opened with 'net' closes when the policy takes 'net' away") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    platform::TcpListener listener;
    std::string error;
    REQUIRE(listener.listen(platform::Endpoint::v4(127, 0, 0, 1, 0), error));
    h.run("/probetcp " + std::to_string(listener.local_endpoint()->port));
    REQUIRE(h.printed_line("[Probe] tcp open 1"));
    h.run("/probetcpsend 1");
    CHECK((h.printed_line("[Probe] tcp send 5 state 0") || h.printed_line("[Probe] tcp send 5 state 1")));
    // Everything but 'net': it stays loaded, and its connection is gone.
    h.approve(plugin::kCapAll & ~plugin::kCapNet);
    h.plugins->policy_changed({"*"});
    REQUIRE(h.plugins->loaded("HostProbe"));
    h.run("/probetcpsend 1");
    CHECK(h.printed_line("[Probe] tcp send -1 state -1"));
    h.run("/probetcp " + std::to_string(listener.local_endpoint()->port));
    CHECK(h.printed_line("[Probe] tcp open 0"));
}

TEST_CASE("native plugins: has_plugin knows a plugin on disk or loaded, in any case") {
    Host h;
    h.approve(plugin::kCapAll);
    CHECK(h.plugins->has_plugin("mq2example"));
    CHECK_FALSE(h.plugins->has_plugin("nosuch"));
    REQUIRE(h.plugins->load("MQ2Example"));
    CHECK(h.plugins->has_plugin("MQ2EXAMPLE"));
}

TEST_CASE("native plugins: a line a script or plugin wrote is not incoming chat") {
    Host h;
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("MQ2Example"));
    plugin::ChatLine spam{"buy spam now", 0, 0};
    spam.from_code = true;
    spam.origin_caps = plugin::kCapRead;
    CHECK(h.hooks.chat(spam) == plugin::Flow::Continue); // neither OnFilterChat's drop
    CHECK(h.p("${Example.Chats}") == "0");               // nor OnIncomingChat
    plugin::ChatLine server{"buy spam now", 0, 0};
    CHECK(h.hooks.chat(server) == plugin::Flow::Drop);
    CHECK(h.p("${Example.Chats}") == "1");
}

TEST_CASE("native plugins: an exception out of a plugin is caught; out of InitializePlugin it leaves nothing behind") {
    {
        Host h(false, nullptr, HAILSTONE_PROBE_THROW_PLUGIN);
        h.approve(plugin::kCapAll);
        std::string why;
        CHECK_FALSE(h.plugins->load("HostProbeThrow", &why));
        CHECK(why == "HostProbeThrow did not load: its InitializePlugin failed with an error");
        CHECK_FALSE(h.plugins->loaded("HostProbeThrow"));
        CHECK_FALSE(h.hooks.has_command("/probethread"));
        CHECK_FALSE(h.hooks.has_command("/probethrow"));
        CHECK_FALSE(h.reg.has_tlo("Probe"));
        CHECK(h.p("${Probe}") == "NULL");
    }
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probethrow");
    CHECK(h.printed_line("[Plugin] HostProbe: a command failed with an error (see the log)"));
    CHECK(h.p("${Probe}") == "probe");
    CHECK(h.p("${Probe.Throw}") == "NULL");
    h.plugins->imgui(); // OnUpdateImGui throws
    h.run("/probethrow");
    CHECK(h.plugins->loaded("HostProbe"));
    REQUIRE(h.plugins->unload("HostProbe"));
    CHECK_FALSE(h.reg.has_tlo("Probe"));
}

namespace {
// A browser with one tab of the probe's: only its state.
struct OneTab : plugin::WebAccess {
    bool available() override { return true; }
    std::uint32_t open(const std::string&, std::uint32_t, const std::string&, int, int, std::string*) override { return 0; }
    bool navigate(const std::string&, std::uint32_t, std::uint32_t, const std::string&, std::string*) override { return false; }
    bool nav(const std::string&, std::uint32_t, std::uint32_t, const std::string&) override { return false; }
    void close(const std::string&, std::uint32_t) override {}
    bool eval(const std::string&, std::uint32_t, std::uint32_t, const std::string&) override { return false; }
    bool post(const std::string&, std::uint32_t, std::uint32_t, const std::string&) override { return false; }
    bool text(const std::string&, std::uint32_t, std::uint32_t, const std::string&) override { return false; }
    std::deque<std::string> messages; // tab 1's
    std::optional<std::string> take_message(const std::string& owner, std::uint32_t tab) override {
        if (owner != "plugin:HostProbe" || tab != 1 || messages.empty()) return std::nullopt;
        std::string m = messages.front();
        messages.pop_front();
        return m;
    }
    std::optional<std::size_t> next_message_size(const std::string& owner, std::uint32_t tab) override {
        if (owner != "plugin:HostProbe" || tab != 1 || messages.empty()) return std::nullopt;
        return messages.front().size();
    }
    std::optional<plugin::WebState> state(const std::string& owner, std::uint32_t tab) override {
        if (owner != "plugin:HostProbe" || tab != 1) return std::nullopt;
        plugin::WebState s;
        s.url = "https://example.org/";
        s.title = "Example";
        return s;
    }
    std::vector<std::uint32_t> tabs(const std::string&) override { return {}; }
    bool view(const std::string&, std::uint32_t, std::uint32_t, float, float) override { return false; }
    void close_owner(const std::string&) override {}
};
} // namespace

TEST_CASE("native plugins: web_state fills what a shorter HailstoneWebState holds, as the other structs") {
    OneTab web;
    Host h(false, &web, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probestate " + std::to_string(sizeof(HailstoneWebState)));
    CHECK(h.printed_line("[Probe] state 1, size " + std::to_string(sizeof(HailstoneWebState)) +
                         ", url https://example.org/, title Example"));
    // A plugin whose struct ended before the title: the url, the title left alone, its size kept.
    const std::size_t shorter = offsetof(HailstoneWebState, title);
    h.run("/probestate " + std::to_string(shorter));
    CHECK(h.printed_line("[Probe] state 1, size " + std::to_string(shorter) + ", url https://example.org/, title untouched"));
    h.run("/probestate 2"); // not even its size
    CHECK(h.printed_line("[Probe] state 0, size 2, url -, title untouched"));
}

// web_message's length with no buffer leaves the message queued, so a plugin can make room for one too long for
// its buffer; given a buffer the message is taken, cut or not (a loop with a fixed buffer still drains the queue).
TEST_CASE("native plugins: web_message with no buffer gives the length and keeps the message") {
    OneTab web;
    web.messages = {"a long message of thirty-two byte", "next"};
    Host h(false, &web, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probemessage 0");
    CHECK(h.printed_line("[Probe] message 33, -"));
    CHECK(web.messages.size() == 2);
    h.run("/probemessage 64");
    CHECK(h.printed_line("[Probe] message 33, a long message of thirty-two byte"));
    h.run("/probemessage 5");
    CHECK(h.printed_line("[Probe] message 4, next"));
    h.run("/probemessage 0");
    CHECK(h.printed_line("[Probe] message -1, -"));
}
TEST_CASE("native plugins: get_buff_data_bulk and get_pet_buff_data_bulk, laid out as the SDK says") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    using A = net::GameConnectionTestAccess;
    auto& sp = A::spells(*h.world.conn);
    REQUIRE(sp.buffs.size() == 63);
    // The world's Holy Armor (slot 0) and Selo's (slot 3); a disease counter and a song.
    sp.buffs[5].spell = 3004;
    sp.buffs[5].caster_id = 10;
    sp.buffs[5].tics = 7;
    sp.buffs[5].num_hits = 2;
    sp.buffs[42].spell = 3373;
    sp.buffs[42].tics = 3;
    h.run("/probebuffs");
    CHECK(h.printed_line("[Probe] buff 0: 11 0 10 0 1 -1 0 0"));
    CHECK(h.printed_line("[Probe] buff 3: 717 0 2 0 1 -1 0 0"));
    CHECK(h.printed_line("[Probe] buff 5: 3004 10 7 2 0 0 4 0")); // disease, starts at 4
    CHECK(h.printed_line("[Probe] buff 42: 3373 0 3 0 0 -1 0 1"));
    CHECK(h.printed_line("[Probe] buffs 1798 bytes, 58 empty")); // slots 0-61 of 29 bytes; the discipline slot not sent
    // All the slots or nothing: a buffer smaller than 62 records gets none.
    h.printed.clear();
    h.run("/probebuffs 1797");
    CHECK(h.printed_line("[Probe] buffs 0 bytes, 0 empty"));
    h.run("/probebuffs 1798");
    CHECK(h.printed_line("[Probe] buffs 1798 bytes, 58 empty"));

    // No pet buffs: nothing. Then the pet window's, in the server's order (the slots are not sent).
    h.run("/probepetbuffs");
    CHECK(h.printed_line("[Probe] pet buffs 0 bytes"));
    auto pet_buff = [](std::uint32_t spell, std::uint32_t tics) {
        game::BuffSlot b;
        b.spell = spell;
        b.tics = tics;
        return b;
    };
    A::pet_buffs(*h.world.conn) = {{2, pet_buff(3002, 5)}, {0, pet_buff(11, 1)}, {7, pet_buff(99999, 4)}};
    h.run("/probepetbuffs");
    CHECK(h.printed_line("[Probe] pet buff: 3002 5 0"));
    CHECK(h.printed_line("[Probe] pet buff: 11 1 1"));
    CHECK(h.printed_line("[Probe] pet buff: 99999 4 0")); // a spell not known: type 0
    CHECK(h.printed_line("[Probe] pet buffs 36 bytes")); // 3 records of 12 bytes
    // Only whole records go into a buffer too small for all.
    h.printed.clear();
    h.run("/probepetbuffs 35");
    CHECK(h.printed_line("[Probe] pet buff: 3002 5 0"));
    CHECK(h.printed_line("[Probe] pet buff: 11 1 1"));
    CHECK_FALSE(h.printed_line("[Probe] pet buff: 99999 4 0"));
    CHECK(h.printed_line("[Probe] pet buffs 24 bytes"));
}
TEST_CASE("native plugins: get_spawns_data_bulk, laid out as docs/plugins.md says") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    // The test world's five spawns (mq_test_world.h), in no set order; the player (1) at the origin.
    h.run("/probespawns");
    CHECK(h.printed_line("[Probe] spawn 1: 50 2 1 100 Tester"));
    CHECK(h.printed_line("[Probe] spawn 10: 2 1 1 80 a_rat01"));
    CHECK(h.printed_line("[Probe] spawn 11: 8 1 1 80 Fippy_Darkpaw000"));
    CHECK(h.printed_line("[Probe] spawn 12: 48 2 1 100 Groupie"));
    CHECK(h.printed_line("[Probe] spawn 13: 3 1 1 80 a_rat02"));
    CHECK(h.printed_line("[Probe] spawns 5, player 0 0 0, 0 bytes left")); // every record's strings framed right
    // Below 4096 bytes nothing; at 4112 (the header and the 4096 kept free) one record, then it stops.
    h.printed.clear();
    h.run("/probespawns 4095");
    CHECK(h.printed_line("[Probe] spawns 0 bytes"));
    h.run("/probespawns 4112");
    CHECK(h.printed_line("[Probe] spawns 1, player 0 0 0, 0 bytes left"));
}
TEST_CASE("native plugins: the buff bulk reads need 'read'") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll & ~plugin::kCapRead);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probebuffs 4096");
    CHECK(h.printed_line("[Probe] buffs 0 bytes, 0 empty"));
    CHECK(h.printed_line("[Plugin] HostProbe: get_buff_data_bulk needs the server's 'read' permission"));
    h.run("/probepetbuffs 1024");
    CHECK(h.printed_line("[Probe] pet buffs 0 bytes"));
    h.run("/probespawns 16384");
    CHECK(h.printed_line("[Probe] spawns 0 bytes")); // the denial is said once per plugin, above
}
