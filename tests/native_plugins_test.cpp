// plugin/native_plugins.h with the example plugin (plugins/example/MQ2Example.c, built next to the tests): loaded when
// the server's rules allow it, its MQ callbacks on the hook bus, held to its capabilities.
#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <fstream>
#include <thread>
#include <stdexcept>

#include "core/sha256.h"
#include "core/binary_measurement.h"
#include "net/item_packets.h"
#include "game/server_ext_chain.h"
#include "mq/commands.h"
#include "mq/parse.h"
#include "platform/tcp_socket.h"
#include "plugin/native_plugins.h"
#include "mq_test_world.h"
#include "plugin_consent_helper.h"
#include "app/addon_requirements.h"
#include "app/web_service.h"
#include "plugin/web_access.h"
#include "web/browser_host.h"

#define HAILSTONE_HOST // the SDK's structs only, not the plugin side
#include "hailstone_plugin.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Host {
    std::shared_ptr<game::ExtensionChain> extensions = std::make_shared<game::ExtensionChain>();
    mq_test::World world{"hailstone_native_plugins_test", [this] {
        net::ConnectionOptions options; options.plugins = extensions; return options;
    }()};
    fs::path plugin_dir = world.dir / "plugins";
    plugin::Hooks hooks;
    plugin::Policy policy;
    mq::Registry reg;
    mq::Context ctx;
    std::vector<std::string> printed, sent;
    std::vector<std::pair<int, std::string>> used, checked; // use_ability, ability_state (kind and name)
    bool ability_ready = true;
    std::vector<plugin::AbilityBulkRow> ability_rows; // get_ability_data_bulk's rows
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
        ne.extensions = extensions.get();
        ne.use_ability = [this](mq::AbilityKind kind, std::string_view name) {
            used.emplace_back(static_cast<int>(kind), std::string(name));
            return true;
        };
        ne.ability = [this](mq::AbilityKind kind, std::string_view name) -> std::optional<mq::AbilityState> {
            checked.emplace_back(static_cast<int>(kind), std::string(name));
            mq::AbilityState st;
            st.name = std::string(name);
            st.ready = ability_ready;
            st.remaining_ms = ability_ready ? 0 : 1500;
            st.reuse_ms = 6000;
            return st;
        };
        ne.ability_bulk = [this] { return ability_rows; };
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

TEST_CASE("native plugins: received item decode commits both outputs only for an accepted prefix") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    net::ItemPacket item;
    item.type = net::kItemPacketTrade;
    item.item.id = 7001;
    item.item.name = "Decoder fixture";
    item.item.slot = net::ItemSlot::possessions(net::kSlotGeneral1, 2);
    auto packet = net::encode_item_packet(false, item);
    REQUIRE(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, packet) == plugin::Flow::Continue);
    h.run("/probedecode 3");
    CHECK(h.printed_line("[Probe] decode 0 slot_unchanged 1 item_unchanged 1 id 1515870810"));
    h.run("/probedecode");
    CHECK(h.printed_line("[Probe] decode 1 slot_unchanged 0 item_unchanged 0 id 7001"));
    h.approve(plugin::kCapAll & ~plugin::kCapRead); h.plugins->policy_changed({});
    h.printed.clear();
    h.run("/probedecode");
    CHECK(h.printed_line("[Probe] decode 0 slot_unchanged 1 item_unchanged 1 id 1515870810"));
    h.approve(plugin::kCapAll); h.plugins->policy_changed({});
    packet.data.clear();
    REQUIRE(h.hooks.packet(plugin::Stream::Zone, plugin::Direction::In, packet) == plugin::Flow::Continue);
    h.printed.clear();
    h.run("/probedecode");
    CHECK(h.printed_line("[Probe] decode 0 slot_unchanged 1 item_unchanged 1 id 1515870810"));
}

TEST_CASE("native plugins: a raid member is targeted by spawn id and incarnation, a reused id refused") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    auto& w = net::GameConnectionTestAccess::world(*h.world.conn);
    w.spawns.at(1).incarnation = 100;
    auto& actor = w.spawns.at(12); actor.incarnation = 200; actor.hp_known = true;
    net::GameConnectionTestAccess::group(*h.world.conn).mana_percent[12] = 42;
    auto& raid = net::GameConnectionTestAccess::raid(*h.world.conn);
    raid.active = true; raid.leader = "Tester";
    game::RaidMember member; member.name = "Groupie"; member.group = 0;
    raid.members.push_back(member);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probeservices");
    CHECK(h.printed_line("[Probe] services spawn 1 token 200 flags 3 mana 42"));
    h.run("/probeselectraid stale");
    CHECK(h.printed_line("[Probe] raid selection 0")); CHECK(w.target_id == 10);
    h.run("/probeselectraid");
    CHECK(h.printed_line("[Probe] raid selection 1")); CHECK(w.target_id == 12);
    // Not in the roster any more: refused, though the spawn is the same.
    raid.members.clear(); w.target_id = 10; h.printed.clear();
    h.run("/probeselectraid");
    CHECK(h.printed_line("[Probe] raid selection 0")); CHECK(w.target_id == 10);
}

TEST_CASE("native plugins: raid actor selection requires target permission") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll & ~plugin::kCapTarget);
    auto& w = net::GameConnectionTestAccess::world(*h.world.conn);
    w.spawns.at(1).incarnation = 100; w.spawns.at(12).incarnation = 200;
    auto& raid = net::GameConnectionTestAccess::raid(*h.world.conn);
    raid.active = true;
    game::RaidMember member; member.name = "Groupie"; raid.members.push_back(member);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probeservices"); h.run("/probeselectraid");
    CHECK(h.printed_line("[Probe] raid selection 0")); CHECK(w.target_id == 10);
}

// raid_action numbers groups as get_raid_data_bulk does (0..11, -1 none) and is the raid leader's, in a locked raid.
TEST_CASE("native plugins: raid_action moves a member only as the locked raid's leader, to a group with room") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    auto& raid = net::GameConnectionTestAccess::raid(*h.world.conn);
    raid.active = true; raid.leader = "Someone"; raid.locked = true;
    game::RaidMember member; member.name = "Groupie"; member.group = 0;
    raid.members.push_back(member);
    for (int i = 0; i < 6; ++i) {
        game::RaidMember full; full.name = "Full" + std::to_string(i); full.group = 3;
        raid.members.push_back(full);
    }
    REQUIRE(h.plugins->load("HostProbe"));
    auto move = [&](const std::string& args) {
        h.printed.clear();
        h.run("/proberaidmove " + args);
        return h.printed.empty() ? std::string{} : h.printed.back();
    };
    CHECK(move("Groupie 2") == "[Probe] raid move 0"); // not the leader
    raid.leader = h.world.conn->character_name();
    CHECK(move("Groupie 2") == "[Probe] raid move 1");
    CHECK(move("Groupie -1") == "[Probe] raid move 1");
    CHECK(move("Groupie 3") == "[Probe] raid move 0");  // six there already
    CHECK(move("Groupie 12") == "[Probe] raid move 0"); // no such group
    CHECK(move("Nobody 2") == "[Probe] raid move 0");
    raid.locked = false;
    CHECK(move("Groupie 2") == "[Probe] raid move 0");
}

TEST_CASE("native plugins: binary measurement uses the admitted library copy and preserves failures") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    const auto measured = measure_binary_file(h.file);
    REQUIRE(measured);
    REQUIRE(h.plugins->load("HostProbe"));
    const auto expected = "[Probe] binary kind 2 bytes " + std::to_string(measured->byte_count) +
                          " fnv " + std::to_string(measured->fnv1a63) + " sha " + to_hex(measured->sha256);
    h.run("/probebinary 2");
    CHECK(h.printed_line(expected));
    h.printed.clear();
    { std::ofstream replacement(h.file, std::ios::binary | std::ios::trunc); replacement << "new source file"; }
    h.run("/probebinary 2");
    CHECK(h.printed_line(expected)); // updating the installed file does not change the loaded module's identity
    h.run("/probebinary 99");
    CHECK(h.printed_line("[Probe] binary result 0 untouched 1"));
    h.printed.clear(); h.run("/probebinary 2 3");
    CHECK(h.printed_line("[Probe] binary result 0 untouched 1"));
    h.printed.clear(); h.run("/probebinary 2 4");
    CHECK(h.printed_line("[Probe] binary result 1 untouched 1"));
    h.run("/probebinarythread");
    CHECK(h.printed_line("[Probe] binary thread 0"));
}

TEST_CASE("native plugins: map policy validates copied hooks, prefixes, capabilities and callback recursion") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll); REQUIRE(h.plugins->load("HostProbe"));
    CHECK(h.world.conn->server_ext().map_policy(*h.world.conn).flags == 0);
    h.run("/probemapset 7");
    auto policy = h.world.conn->server_ext().map_policy(*h.world.conn);
    CHECK(policy.flags == 7); CHECK(policy.name == "Probe map");
    h.run("/probemappolicy");
    CHECK(h.printed_line("[Probe] map result 1 flags 7 untouched 0 name Probe map"));
    h.run("/probemappolicy 3");
    CHECK(h.printed_line("[Probe] map result 0 flags 1515870810 untouched 1 name -"));
    h.run("/probemappolicy 4");
    CHECK(h.printed_line("[Probe] map result 1 flags 1515870810 untouched 1 name -"));
    h.run("/probemapthread"); CHECK(h.printed_line("[Probe] map thread 0"));
    for (int malformed = 1; malformed <= 4; ++malformed) {
        h.run("/probemapset 7 " + std::to_string(malformed));
        CHECK(h.world.conn->server_ext().map_policy(*h.world.conn).flags == 0);
    }
    h.run("/probemapset 7 5");
    CHECK(h.world.conn->server_ext().map_policy(*h.world.conn).flags == 7);
    CHECK(h.printed_line("[Probe] nested map 0"));
    h.run("/probemapset 7 6");
    CHECK(h.world.conn->server_ext().map_policy(*h.world.conn).flags == 0); // retires inside its callback
    CHECK(h.world.conn->server_ext().map_policy(*h.world.conn).flags == 0);
    h.run("/probemapset 7");
    h.approve(plugin::kCapAll & ~plugin::kCapUi); h.plugins->policy_changed({});
    CHECK(h.world.conn->server_ext().map_policy(*h.world.conn).flags == 0);
}

TEST_CASE("native plugins: host provider exceptions return failure before crossing SDK frames") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll); REQUIRE(h.plugins->load("HostProbe"));
    struct Provider final : game::ServerExtension {
        bool fail = true;
        game::MapPolicy map_policy(const net::GameConnection&) const override {
            if (fail) throw std::runtime_error("host policy failed");
            return {game::MapPolicyCustomSpawns, "Host provider", 1};
        }
    };
    auto provider = std::make_shared<Provider>(); h.extensions->add(provider);
    CHECK_NOTHROW(h.run("/probemappolicy"));
    CHECK(h.printed_line("[Probe] map result 0 flags 1515870810 untouched 1 name -"));
    CHECK(h.plugins->loaded("HostProbe"));
    provider->fail = false;
    h.run("/probemappolicy");
    CHECK(h.printed_line("[Probe] map result 1 flags 1 untouched 0 name Host provider"));
}

TEST_CASE("native plugins: map policy reads need READ and old hook tables keep stock defaults") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll & ~plugin::kCapRead); REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probemappolicy");
    CHECK(h.printed_line("[Probe] map result 0 flags 1515870810 untouched 1 name -"));
    Host old;
    old.approve(plugin::kCapAll); REQUIRE(old.plugins->load("MQ2Example"));
    CHECK(old.world.conn->server_ext().map_policy(*old.world.conn).flags == 0);
}

TEST_CASE("native plugins: map policy accepts old aligned prefixes and changes across unseen owner transitions") {
    Host h(false,nullptr,HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);REQUIRE(h.plugins->load("HostProbe"));
    const auto current=[&]{return h.world.conn->server_ext().map_policy(*h.world.conn);};
    const auto lifecycle=[&]{return h.world.conn->server_ext().lifecycle_revision(*h.world.conn);};
    const auto initial_lifecycle=lifecycle();
    for(const int format:{7,8,0}) {
        h.run("/probemapset 7 "+std::to_string(format)+" 42");
        const auto policy=current();CHECK(policy.flags==7);CHECK(policy.name=="Probe map");CHECK(policy.revision!=0);
        CHECK(lifecycle()==initial_lifecycle); // map-only content and optional record format do not retire Tracking
        for(const int size:{72,80,88}) {
            h.run("/probemapprefix "+std::to_string(size));
            CHECK(h.printed_line("[Probe] map prefix "+std::to_string(size)+" result 1 flags 7 canary 1 revision "+
                (size>=80?"1":"0")+" context "+(size>=88?"1":"0")));
        }
    }
    auto before=current().revision;CHECK(current().revision==before);
    h.run("/probemapset 7 0 43");CHECK(current().revision!=before);CHECK(lifecycle()==initial_lifecycle);
    before=current().revision;h.run("/probemapcycle");CHECK(current().revision!=before);CHECK(lifecycle()!=initial_lifecycle);
    before=current().revision;
    const auto before_caps=lifecycle();
    h.approve(plugin::kCapAll&~plugin::kCapUi);h.plugins->policy_changed({});
    h.approve(plugin::kCapAll);h.plugins->policy_changed({});
    CHECK(current().revision!=before); // no policy query observed the revoked interval
    CHECK(lifecycle()!=before_caps);
    before=current().revision;
    REQUIRE(h.plugins->unload("HostProbe"));REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probemapset 7 0 43");CHECK(current().revision!=before);
}

TEST_CASE("native plugins: binary measurement requires read permission") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll & ~plugin::kCapRead);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probebinary 2");
    CHECK(h.printed_line("[Probe] binary result 0 untouched 1"));
}

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

// A plugin built against the 0.7.1-beta SDK (tests/plugins/Sdk071Plugin.c, its frozen header): it exports
// hailstone_plugin_set_host and nmsc_plugin_set_host only, and reads the host table by the released positions. The
// table grows at its end, so it loads and its calls land where its header put them.
TEST_CASE("native plugins: a plugin built against the 0.7.1 SDK loads and finds its released host entries") {
    Host h(false, nullptr, HAILSTONE_SDK071_PLUGIN);
    h.approve(plugin::kCapAll);
    std::string why;
    REQUIRE_MESSAGE(h.plugins->load("Sdk071Plugin", &why), why);
    CHECK(h.printed_line("[Sdk071] loaded"));
    h.run("/sdk071");
    // Whole 16-byte xtarget records (remainder 0): the call reached get_xtarget_data_bulk, not a later entry.
    CHECK(h.printed_line("[Sdk071] abi 3 table 1 name Sdk071Plugin xtarget 0"));
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
    CHECK_FALSE(q->files[0].required_addon); // the server's own: "Allow this server's plugin?"
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

TEST_CASE("native plugins: required managed plugins autoload with consent and stay loaded") {
    Host h;
    const fs::path profile = h.world.dir / "servers" / "required";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins");
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/example\n");
    rules.dir = profile;
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "MQ2Example", "owner/example"}});
    h.policy.set(rules);
    h.plugins->policy_changed({}); // neither autoload nor a server-delivered file
    REQUIRE(h.plugins->consent());
    CHECK(h.plugins->consent()->files[0].required_addon); // the player's own add-on, which the server requires
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    std::string problem;
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK(problem.empty());
    h.plugins->answer_consent(false);
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK(problem.find("not approved") != std::string::npos);
    CHECK_FALSE(h.plugins->load("MQ2Example")); // the player's explicit retry asks again
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(true);
    REQUIRE(h.plugins->loaded("MQ2Example"));
    CHECK(h.plugins->required_addons_ready(&problem));
    CHECK(problem.empty());
    CHECK_FALSE(h.plugins->unload("MQ2Example"));
    h.run("/plugin MQ2Example unload");
    CHECK(h.plugins->loaded("MQ2Example"));
    rules.addons.required.clear();
    rules.addons.block = {"owner/example"};
    h.policy.set(rules);
    CHECK_FALSE(h.plugins->required_addons_ready(&problem)); // the loaded plugin must retire first
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK(h.plugins->required_addons_ready());
}

TEST_CASE("native plugins: required readiness is worked out again only when the rules, answers or plugins change") {
    Host h;
    const fs::path profile = h.world.dir / "servers" / "required";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins");
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/example\n");
    rules.dir = profile;
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "MQ2Example", "owner/example"}});
    h.policy.set(rules);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(true);
    REQUIRE(h.plugins->loaded("MQ2Example"));
    CHECK(h.plugins->required_addons_ready());
    // The answers file is not read each frame: an edit from outside the client shows with the next rules.
    std::ofstream(profile / plugin::kApprovalsName, std::ios::binary | std::ios::trunc)
        << "hailstone-plugin-approvals 1\ndeny " << to_hex(h.hash()) << " MQ2Example\n";
    CHECK(h.plugins->required_addons_ready());
    h.policy.set(rules); // the next zone-in's rules
    std::string problem;
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK(problem.find("not approved") != std::string::npos);
    // An answer the client writes shows at once.
    REQUIRE(plugin::set_plugin_consent(profile, "MQ2Example", h.hash(), true));
    CHECK(h.plugins->required_addons_ready());
    // So does a plugin that goes.
    h.plugins->unload_all();
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK(problem.find("did not load") != std::string::npos);
}

TEST_CASE("native plugins: blocks name an installed plugin by its repository, not a hand-installed one") {
    Host h;
    h.policy.set(plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\ndefault = allow\n"
                                          "block = owner/example\n"));
    CHECK(h.plugins->list().front().caps != 0); // put in by hand: no repository for a list to name
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "MQ2Example", "owner/example"}});
    CHECK(h.plugins->list().front().caps == 0);
    CHECK_FALSE(h.plugins->load("mq2example"));
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK_FALSE(h.plugins->consent());
}

TEST_CASE("native plugins: a required hash also supplied by a blocked add-on refuses readiness without consent") {
    Host h;
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "MQ2Example", "owner/needed"},
                          {plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "blocked", "owner/blocked"}});
    h.policy.set(plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                         "required = owner/needed\nblock = owner/blocked\n"));
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    CHECK_FALSE(h.plugins->consent());
    std::string problem;
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK(problem.find("not permitted") != std::string::npos);
}

TEST_CASE("native plugins: missing or shadowed required libraries cannot disappear from readiness checks") {
    Host h;
    const auto expected = h.hash();
    const fs::path profile = h.world.dir / "servers/required";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins");
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/example\n");
    REQUIRE(rules.addons.error.empty());
    h.policy.set_ceilings({{plugin::FileKind::Plugin, expected, plugin::kCapAll, "MQ2Example", "owner/example"}});
    SUBCASE("the managed library is missing") {
        fs::remove(h.file);
    }
    SUBCASE("a different server library shadows the managed entry") {
        const fs::path shadow = profile / "plugins" / h.file.filename();
        fs::copy_file(h.file, shadow);
        std::ofstream(shadow, std::ios::binary | std::ios::app) << "different bytes";
        std::ifstream f(shadow, std::ios::binary);
        const auto hash = sha256(std::string(std::istreambuf_iterator<char>(f), {}));
        rules.server_files.push_back({plugin::FileKind::Plugin, "plugins/" + shadow.filename().string(), hash});
    }
    h.policy.set(rules);
    h.plugins->policy_changed({});
    CHECK_FALSE(h.plugins->loaded("MQ2Example"));
    std::string problem;
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK_FALSE(problem.empty());
}

TEST_CASE("native plugins: dismissing required consent reports a failure instead of waiting without a prompt") {
    Host h;
    const fs::path profile = h.world.dir / "servers" / "required";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins");
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/example\n");
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "MQ2Example", "owner/example"}});
    h.policy.set(rules);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(std::nullopt);
    CHECK_FALSE(h.plugins->consent());
    std::string problem;
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK(problem.find("needs your approval") != std::string::npos);
}

TEST_CASE("native plugins: a required plugin initialization failure prevents readiness after consent") {
    Host h(false, nullptr, HAILSTONE_PROBE_THROW_PLUGIN);
    const fs::path profile = h.world.dir / "servers" / "required";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins");
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/throw\n");
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "HostProbeThrow", "owner/throw"}});
    h.policy.set(rules);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(true);
    CHECK_FALSE(h.plugins->loaded("HostProbeThrow"));
    std::string problem;
    CHECK_FALSE(h.plugins->required_addons_ready(&problem));
    CHECK(problem.find("InitializePlugin failed") != std::string::npos);
}

// A required plugin whose callback let an exception out is a requirement not met, worked out again at once.
TEST_CASE("native plugins: a required plugin that throws in the world is a warning, and play goes on") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    const fs::path profile = h.world.dir / "servers" / "required";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins");
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/probe\n");
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "HostProbe", "owner/probe"}});
    h.policy.set(rules);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(true);
    REQUIRE(h.plugins->loaded("HostProbe"));

    // The gate as ClientApp::addon_world_allowed drives it.
    app::AddonAdmission admission;
    auto frame = [&] {
        return admission.update(&rules, 1, true, [] { return std::string(); }, [&]() -> std::optional<std::string> {
            std::string problem;
            if (h.plugins->required_addons_ready(&problem)) return std::string();
            if (problem.empty()) return std::nullopt;
            return problem;
        });
    };
    CHECK(frame().step == app::AddonAdmission::Step::Start);
    REQUIRE(frame().step == app::AddonAdmission::Step::Play);
    REQUIRE(admission.admitted());
    CHECK(frame().warning.empty()); // readiness worked out, and kept

    h.run("/probethrow"); // the command's callback lets an exception out
    std::string problem;
    CHECK_FALSE(h.plugins->required_addons_ready(&problem)); // seen although nothing else changed
    CHECK(problem == "Required add-on owner/probe failed with an error in a command");
    auto r = frame();
    CHECK(r.step == app::AddonAdmission::Step::Play); // never a disconnect or a hold
    CHECK(r.warning == problem);
    CHECK(frame().warning.empty()); // said once
    CHECK(h.plugins->loaded("HostProbe"));
    CHECK(admission.admitted());
}

// Retry (the Server add-ons warning): the required plugin that threw is unloaded and loaded again once; the warning
// clears, and a later throw is warned about again.
TEST_CASE("native plugins: Retry reloads a required plugin that threw, once, and the warning clears") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    const fs::path profile = h.world.dir / "servers" / "required";
    fs::create_directories(profile / "plugins");
    h.plugins->set_server_dir(profile / "plugins");
    auto rules = plugin::parse_server_cfg("[plugins]\nplayer = on\n[addons]\nenforce = on\nrequired = owner/probe\n");
    h.policy.set_ceilings({{plugin::FileKind::Plugin, h.hash(), plugin::kCapAll, "HostProbe", "owner/probe"}});
    h.policy.set(rules);
    h.plugins->policy_changed({});
    REQUIRE(h.plugins->consent());
    h.plugins->answer_consent(true);
    REQUIRE(h.plugins->loaded("HostProbe"));
    CHECK(h.plugins->retry_required_addons() == 0); // running fine: left alone

    app::AddonAdmission admission;
    auto frame = [&] {
        return admission.update(&rules, 1, true, [] { return std::string(); }, [&]() -> std::optional<std::string> {
            std::string problem;
            if (h.plugins->required_addons_ready(&problem)) return std::string();
            if (problem.empty()) return std::nullopt;
            return problem;
        });
    };
    frame();
    REQUIRE(frame().step == app::AddonAdmission::Step::Play);
    REQUIRE(admission.admitted());
    CHECK(admission.failure().empty());
    const auto quiet = admission.failure_serial();

    h.run("/probethrow");
    CHECK_FALSE(frame().warning.empty());
    CHECK(admission.failure() == "Required add-on owner/probe failed with an error in a command");
    CHECK(admission.failure_retriable()); // the running add-ons said so: Retry may help
    CHECK(admission.failure_serial() != quiet);
    const auto failed = admission.failure_serial();
    frame();
    CHECK(admission.failure_serial() == failed); // the same failure keeps its serial

    CHECK(h.plugins->retry_required_addons() == 1);
    CHECK(h.plugins->loaded("HostProbe")); // loaded again (approved bytes: no new question)
    CHECK_FALSE(h.plugins->consent());
    std::string problem;
    CHECK(h.plugins->required_addons_ready(&problem));
    CHECK(frame().warning.empty());
    CHECK(admission.failure().empty());
    CHECK(admission.failure_serial() != failed);
    CHECK(h.plugins->retry_required_addons() == 0); // once: nothing failed now

    h.run("/probethrow"); // a new failure after the retry is warned about again
    CHECK(frame().warning == "Required add-on owner/probe failed with an error in a command");
    CHECK(admission.admitted());
}

// A failure the zone's rules give (an add-on missing for this zone's policy), not the running add-ons, is not one
// Retry can help with.
TEST_CASE("addon admission: a failure from the zone's rules is not retriable, and leaving clears it") {
    auto rules = plugin::parse_server_cfg("[addons]\nenforce = on\nrequired = owner/probe\n");
    app::AddonAdmission admission;
    std::string zone;
    auto frame = [&](std::uint32_t serial) {
        return admission.update(&rules, serial, true, [&] { return zone; },
                                [] { return std::optional<std::string>(std::string()); });
    };
    frame(1);
    frame(1);
    REQUIRE(admission.admitted());
    zone = "Required add-on owner/probe is not installed. Open Add-ons to install it.";
    CHECK(frame(2).warning == zone);
    CHECK(admission.failure() == zone);
    CHECK_FALSE(admission.failure_retriable());
    const auto serial = admission.failure_serial();
    admission.update(nullptr, 0, false, [] { return std::string(); },
                     [] { return std::optional<std::string>(std::string()); }); // character select
    CHECK(admission.failure().empty());
    CHECK(admission.failure_serial() != serial);
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
    // ability_state's 0 means "on reuse": off the client's thread it is -1 ("unknown"), the times untouched, and the
    // host's state is never asked.
    CHECK(h.printed_line("[Probe] thread: ability_state -1, -2 of -2 ms"));
    CHECK(h.checked.empty());
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
    // The world's Holy Armor (slot 0) and Selo's (slot 3); a disease counter and a song. The duration is the ticks
    // left: the disease counter's 7 came two ticks (12 s) ago, so 5 are left.
    const std::int64_t now = net::GameConnection::now_ms();
    sp.buffs[5].spell = 3004;
    sp.buffs[5].caster_id = 10;
    sp.buffs[5].tics = 7;
    sp.buffs[5].received_ms = now - 2 * game::kBuffTickMs;
    sp.buffs[5].num_hits = 2;
    sp.buffs[42].spell = 3373;
    sp.buffs[42].tics = 3;
    sp.buffs[42].received_ms = now;
    sp.buffs[62].spell = 99999; // the discipline slot; a spell not in the table (type 0, no counter)
    sp.buffs[62].tics = 9;
    sp.buffs[62].received_ms = now;
    h.run("/probebuffs");
    CHECK(h.printed_line("[Probe] buff slot 0: spell 11, caster 0, tics 10, hits 0, spell type 1, counter type -1, "
                         "counters 0, slot type 0"));
    CHECK(h.printed_line("[Probe] buff slot 3: spell 717, caster 0, tics 2, hits 0, spell type 1, counter type -1, "
                         "counters 0, slot type 0"));
    // disease, starts at 4
    CHECK(h.printed_line("[Probe] buff slot 5: spell 3004, caster 10, tics 5, hits 2, spell type 0, counter type 0, "
                         "counters 4, slot type 0"));
    // a song
    CHECK(h.printed_line("[Probe] buff slot 42: spell 3373, caster 0, tics 3, hits 0, spell type 0, counter type -1, "
                         "counters 0, slot type 1"));
    // the disc slot, with room for it
    CHECK(h.printed_line("[Probe] buff slot 62: spell 99999, caster 0, tics 9, hits 0, spell type 0, counter type -1, "
                         "counters 0, slot type 2"));
    CHECK(h.printed_line("[Probe] buffs 1827 bytes, 58 empty")); // slots 0-62 of 29 bytes
    // Nothing counts down while the buff timers are on hold (SPA 436): the count as the server sent it.
    sp.buff_timers_on_hold = true;
    h.printed.clear();
    h.run("/probebuffs");
    CHECK(h.printed_line("[Probe] buff slot 5: spell 3004, caster 10, tics 7, hits 2, spell type 0, counter type 0, "
                         "counters 4, slot type 0"));
    sp.buff_timers_on_hold = false;
    // A permanent buff is the server's negative count as sent, not a long count: -1 (formula 50), -4 (an aura's).
    sp.buffs[42].tics = -4;
    sp.buffs[62].tics = -1;
    h.printed.clear();
    h.run("/probebuffs");
    CHECK(h.printed_line("[Probe] buff slot 42: spell 3373, caster 0, tics -4, hits 0, spell type 0, counter type -1, "
                         "counters 0, slot type 1"));
    CHECK(h.printed_line("[Probe] buff slot 62: spell 99999, caster 0, tics -1, hits 0, spell type 0, "
                         "counter type -1, counters 0, slot type 2"));
    sp.buffs[42].tics = 3;
    sp.buffs[62].tics = 9;
    // All the slots or nothing: a buffer smaller than 62 records gets none.
    h.printed.clear();
    h.run("/probebuffs 1797");
    CHECK(h.printed_line("[Probe] buffs 0 bytes, 0 empty"));
    // 62 records as before: the disc slot only goes out with room for a 63rd.
    h.run("/probebuffs 1798");
    CHECK(h.printed_line("[Probe] buffs 1798 bytes, 58 empty"));
    h.run("/probebuffs 1826");
    CHECK_FALSE(h.printed_line("[Probe] buff slot 62: spell 99999, caster 0, tics 9, hits 0, spell type 0, "
                               "counter type -1, counters 0, slot type 2"));
    CHECK_FALSE(h.printed_line("[Probe] buffs 1827 bytes, 58 empty"));

    // No pet buffs: nothing. Then the pet window's, in the server's order (the slots are not sent).
    h.run("/probepetbuffs");
    CHECK(h.printed_line("[Probe] pet buffs 0 bytes"));
    auto pet_buff = [now](std::uint32_t spell, std::int32_t tics, std::int64_t ticks_ago = 0) {
        game::BuffSlot b;
        b.spell = spell;
        b.tics = tics;
        b.received_ms = now - ticks_ago * game::kBuffTickMs;
        return b;
    };
    // The first came a tick ago: 6 sent, 5 left.
    A::pet_buffs(*h.world.conn) = {{2, pet_buff(3002, 6, 1)}, {0, pet_buff(11, 1)}, {7, pet_buff(99999, 4)}};
    h.run("/probepetbuffs");
    CHECK(h.printed_line("[Probe] pet buff: spell 3002, tics 5, spell type 0"));
    CHECK(h.printed_line("[Probe] pet buff: spell 11, tics 1, spell type 1"));
    CHECK(h.printed_line("[Probe] pet buff: spell 99999, tics 4, spell type 0")); // a spell not known: type 0
    CHECK(h.printed_line("[Probe] pet buffs 36 bytes")); // 3 records of 12 bytes
    // Only whole records go into a buffer too small for all.
    h.printed.clear();
    h.run("/probepetbuffs 35");
    CHECK(h.printed_line("[Probe] pet buff: spell 3002, tics 5, spell type 0"));
    CHECK(h.printed_line("[Probe] pet buff: spell 11, tics 1, spell type 1"));
    CHECK_FALSE(h.printed_line("[Probe] pet buff: spell 99999, tics 4, spell type 0"));
    CHECK(h.printed_line("[Probe] pet buffs 24 bytes"));
}
TEST_CASE("native plugins: get_buff_name_data_bulk, laid out as docs/plugins.md says") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    using A = net::GameConnectionTestAccess;
    auto& sp = A::spells(*h.world.conn);
    // The world's Holy Armor (slot 0) and Selo's (slot 3), with no caster's name; a disease counter and a song, as
    // get_buff_data_bulk's test, with the caster's name in place of its id.
    const std::int64_t now = net::GameConnection::now_ms();
    sp.buffs[5].spell = 3004;
    sp.buffs[5].caster_id = 12;
    sp.buffs[5].caster_name = "Groupie";
    sp.buffs[5].tics = 7;
    sp.buffs[5].received_ms = now - 2 * game::kBuffTickMs;
    sp.buffs[5].num_hits = 2;
    sp.buffs[42].spell = 3373;
    sp.buffs[42].tics = -4; // an aura's: permanent, as sent
    sp.buffs[42].received_ms = now;
    sp.buffs[62].spell = 99999; // the discipline slot; a spell not in the table (type 0, no counter)
    sp.buffs[62].caster_name = "Tester";
    sp.buffs[62].tics = 9;
    sp.buffs[62].received_ms = now;
    sp.buffs[20].caster_name = "Nobody"; // an empty slot: all zeros, its name not sent
    h.run("/probebuffnames");
    CHECK(h.printed_line("[Probe] buff name slot 0: spell 11, caster -, tics 10, hits 0, spell type 1, "
                         "counter type -1, counters 0, slot type 0"));
    CHECK(h.printed_line("[Probe] buff name slot 3: spell 717, caster -, tics 2, hits 0, spell type 1, "
                         "counter type -1, counters 0, slot type 0"));
    CHECK(h.printed_line("[Probe] buff name slot 5: spell 3004, caster Groupie, tics 5, hits 2, spell type 0, "
                         "counter type 0, counters 4, slot type 0"));
    CHECK(h.printed_line("[Probe] buff name slot 42: spell 3373, caster -, tics -4, hits 0, spell type 0, "
                         "counter type -1, counters 0, slot type 1"));
    CHECK(h.printed_line("[Probe] buff name slot 62: spell 99999, caster Tester, tics 9, hits 0, spell type 0, "
                         "counter type -1, counters 0, slot type 2"));
    // slots 0-62 of 29 bytes, and the names "Groupie" and "Tester"
    CHECK(h.printed_line("[Probe] buff names 1840 bytes, 58 empty, 0 bytes left"));
    // get_buff_data_bulk is unchanged: the caster's id, 29 bytes a slot.
    h.run("/probebuffs");
    CHECK(h.printed_line("[Probe] buff slot 5: spell 3004, caster 12, tics 5, hits 2, spell type 0, counter type 0, "
                         "counters 4, slot type 0"));
    CHECK(h.printed_line("[Probe] buffs 1827 bytes, 58 empty"));
    // All the buff and song slots, names included, or nothing: 62 records of 29 bytes and "Groupie" is 1805.
    h.printed.clear();
    h.run("/probebuffnames 1804");
    CHECK(h.printed_line("[Probe] buff names 0 bytes, 0 empty, 0 bytes left"));
    h.run("/probebuffnames 1805");
    CHECK(h.printed_line("[Probe] buff names 1805 bytes, 58 empty, 0 bytes left"));
    // The disc slot only goes out with room for its record and its name.
    h.printed.clear();
    h.run("/probebuffnames 1839");
    CHECK_FALSE(h.printed_line("[Probe] buff name slot 62: spell 99999, caster Tester, tics 9, hits 0, "
                               "spell type 0, counter type -1, counters 0, slot type 2"));
    CHECK(h.printed_line("[Probe] buff names 1805 bytes, 58 empty, 0 bytes left"));
}
TEST_CASE("native plugins: get_spawns_data_bulk, laid out as docs/plugins.md says") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    // The test world's five spawns (mq_test_world.h), in no set order; the player (1, level 50) at the origin. The
    // con colours are RoF2's table for a level 50: itself white (10), the level 48 blue (4), the low ones grey (6).
    h.run("/probespawns");
    CHECK(h.printed_line("[Probe] spawn 1: level 50, class 2, race 1, hp 100%, con 10, name Tester"));
    CHECK(h.printed_line("[Probe] spawn 10: level 2, class 1, race 1, hp 80%, con 6, name a_rat01"));
    CHECK(h.printed_line("[Probe] spawn 11: level 8, class 1, race 1, hp 80%, con 6, name Fippy_Darkpaw000"));
    CHECK(h.printed_line("[Probe] spawn 12: level 48, class 2, race 1, hp 100%, con 4, name Groupie"));
    CHECK(h.printed_line("[Probe] spawn 13: level 3, class 1, race 1, hp 80%, con 6, name a_rat02"));
    // Every record's strings framed right: nothing left over.
    CHECK(h.printed_line("[Probe] spawns 5, player 0.00 0.00 0.00, 0 bytes left"));
    // Below 4096 bytes nothing; at 4112 (the header and the 4096 kept free) one record, then it stops.
    h.printed.clear();
    h.run("/probespawns 4095");
    CHECK(h.printed_line("[Probe] spawns 0 bytes"));
    h.run("/probespawns 4112");
    CHECK(h.printed_line("[Probe] spawns 1, player 0.00 0.00 0.00, 0 bytes left"));
    CHECK(h.printed_line("[Probe] spawns past the end 0"));
}
TEST_CASE("native plugins: get_spawns_data_bulk stops before a record that would not fit") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    // Names and suffixes come from the server uncapped: give every spawn 20,000-byte ones, so one record is more
    // than 80,000 bytes, far past the 4096 kept free.
    for (auto& [id, s] : net::GameConnectionTestAccess::world(*h.world.conn).spawns) {
        s.name = std::string(20000, 'n');
        s.suffix = std::string(20000, 's');
    }
    // Room for the header and the 4096 only: no record, and nothing written past the end.
    h.run("/probespawns 8192");
    CHECK(h.printed_line("[Probe] spawns 0, player 0.00 0.00 0.00, 0 bytes left"));
    CHECK(h.printed_line("[Probe] spawns past the end 0"));
    // Room for one record but not two: one, whole.
    h.printed.clear();
    h.run("/probespawns 150000");
    CHECK(h.printed_line("[Probe] spawns 1, player 0.00 0.00 0.00, 0 bytes left"));
    CHECK(h.printed_line("[Probe] spawns past the end 0"));
    // The whole buffer: all five.
    h.printed.clear();
    h.run("/probespawns");
    CHECK(h.printed_line("[Probe] spawns 5, player 0.00 0.00 0.00, 0 bytes left"));
}
TEST_CASE("native plugins: get_xtarget_data_bulk, laid out as docs/plugins.md says") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    using A = net::GameConnectionTestAccess;
    // No list yet: nothing.
    h.run("/probextargets");
    CHECK(h.printed_line("[Probe] xtargets 0 bytes"));
    auto& xs = A::xtargets(*h.world.conn).slots;
    xs.resize(5);
    xs[0] = {HS_BULK_XTARGET_AUTO_HATER, 2, 10, "a_rat01"};          // an auto hater, a_rat01 at 80%
    xs[1] = {HS_BULK_XTARGET_AUTO_HATER, 2, 11, "Fippy_Darkpaw000"}; // Fippy, set to 55% below
    xs[2] = {HS_BULK_XTARGET_AUTO_HATER, 2, 13, "a_rat02"};          // the test world's corpse: no HP
    xs[3] = {HS_BULK_XTARGET_SPECIFIC_NPC, 1, 0, "Somebody"};        // a named slot without a spawn in the zone
    // slot 4 empty
    auto& aggro = A::aggro(*h.world.conn).pct;
    aggro[net::kAggroXTarget1] = 100;
    aggro[net::kAggroXTarget1 + 1] = 42;
    aggro[net::kAggroPlayer] = 77;         // the player's own entry, not an extended target's
    aggro[net::kAggroXTarget1 + 2] = 60;   // the corpse still shows its percent, as the window does
    aggro[net::kAggroXTarget1 + 4] = 33;   // left from a mob the slot held before: not sent for an empty slot
    A::world(*h.world.conn).spawns.at(11).hp_percent = 55;
    h.run("/probextargets");
    CHECK(h.printed_line("[Probe] xtarget 0: type 1, spawn 10, aggro 100%, hp 80%"));
    CHECK(h.printed_line("[Probe] xtarget 1: type 1, spawn 11, aggro 42%, hp 55%"));
    CHECK(h.printed_line("[Probe] xtarget 2: type 1, spawn 13, aggro 60%, hp 0%"));
    CHECK(h.printed_line("[Probe] xtarget 3: type 3, spawn 0, aggro 0%, hp 0%"));
    CHECK(h.printed_line("[Probe] xtarget 4: type 1, spawn 0, aggro 0%, hp 0%"));
    CHECK(h.printed_line("[Probe] xtargets 80 bytes")); // 5 records of 16 bytes
    // Only whole records go into a buffer too small for all.
    h.printed.clear();
    h.run("/probextargets 47");
    CHECK(h.printed_line("[Probe] xtarget 1: type 1, spawn 11, aggro 42%, hp 55%"));
    CHECK_FALSE(h.printed_line("[Probe] xtarget 2: type 1, spawn 13, aggro 60%, hp 0%"));
    CHECK(h.printed_line("[Probe] xtargets 32 bytes"));
}
TEST_CASE("native plugins: get_target_buff_data_bulk, laid out as docs/plugins.md says") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    using A = net::GameConnectionTestAccess;
    auto& sp = A::spells(*h.world.conn);
    auto& w = A::world(*h.world.conn);
    REQUIRE(w.target_id == 10); // the test world targets a_rat01
    // No OP_TargetBuffs yet: the header alone, count 0 and total 0.
    h.run("/probetargetbuffs");
    CHECK(h.printed_line("[Probe] target buffs count 0, total 0, 8 bytes, 0 records, 0 empty, 0 bytes left"));
    const std::int64_t now = net::GameConnection::now_ms();
    auto target_buff = [now](std::uint32_t spell, std::int32_t tics, std::string caster, std::int64_t ticks_ago = 0) {
        game::BuffSlot b;
        b.spell = spell;
        b.tics = tics;
        b.received_ms = now - ticks_ago * game::kBuffTickMs;
        b.caster_name = std::move(caster);
        return b;
    };
    sp.target_buffs_entity = 10;
    // An entry with no spell (index 2), with a caster's name, is left out and not counted. The duration is the ticks
    // left: the first came three ticks ago, 8 sent, 5 left.
    sp.target_buffs = {{3, target_buff(3002, 8, "Tester", 3)}, {0, target_buff(11, 1, "Groupie")},
                       {5, target_buff(0, 6, "Nobody")}, {7, target_buff(99999, 4, "")}};
    // In the order the client keeps them (the slots are not sent); a spell not known has type 0, no caster no name.
    h.run("/probetargetbuffs");
    CHECK(h.printed_line("[Probe] target buff slot 3: spell 3002, tics 5, spell type 0, caster Tester"));
    CHECK(h.printed_line("[Probe] target buff slot 0: spell 11, tics 1, spell type 1, caster Groupie"));
    CHECK(h.printed_line("[Probe] target buff slot 7: spell 99999, tics 4, spell type 0, caster -"));
    // 8 for the header, then 20+6, 20+7, 20+0: the entry with no spell is not sent
    CHECK(h.printed_line("[Probe] target buffs count 3, total 3, 81 bytes, 3 records, 0 empty, 0 bytes left"));
    // Only whole records, names included, go into a buffer too small for all, and nothing past its end; the header's
    // count says how many follow and its total how many the client holds, so the plugin sees it was cut short. The third record (20 bytes) would
    // fit where the second (27) does not, but nothing follows the first cut: the records stay in order.
    h.printed.clear();
    h.run("/probetargetbuffs 60");
    CHECK(h.printed_line("[Probe] target buff slot 3: spell 3002, tics 5, spell type 0, caster Tester"));
    CHECK_FALSE(h.printed_line("[Probe] target buff slot 0: spell 11, tics 1, spell type 1, caster Groupie"));
    CHECK_FALSE(h.printed_line("[Probe] target buff slot 7: spell 99999, tics 4, spell type 0, caster -"));
    CHECK(h.printed_line("[Probe] target buffs count 1, total 3, 34 bytes, 1 records, 0 empty, 0 bytes left"));
    CHECK(h.printed_line("[Probe] target buffs past the end 0"));
    h.printed.clear();
    h.run("/probetargetbuffs 61");
    CHECK(h.printed_line("[Probe] target buffs count 2, total 3, 61 bytes, 2 records, 0 empty, 0 bytes left"));
    CHECK(h.printed_line("[Probe] target buffs past the end 0"));
    // The last record needs its 20 bytes: one byte short, it is left out.
    h.printed.clear();
    h.run("/probetargetbuffs 80");
    CHECK_FALSE(h.printed_line("[Probe] target buff slot 7: spell 99999, tics 4, spell type 0, caster -"));
    CHECK(h.printed_line("[Probe] target buffs count 2, total 3, 61 bytes, 2 records, 0 empty, 0 bytes left"));
    CHECK(h.printed_line("[Probe] target buffs past the end 0"));
    h.printed.clear();
    h.run("/probetargetbuffs 27"); // the header and not even one fixed part
    CHECK(h.printed_line("[Probe] target buffs count 0, total 3, 8 bytes, 0 records, 0 empty, 0 bytes left"));
    CHECK(h.printed_line("[Probe] target buffs past the end 0"));
    h.printed.clear();
    h.run("/probetargetbuffs 7"); // not even the header: nothing
    CHECK(h.printed_line("[Probe] target buffs 0 bytes"));
    CHECK(h.printed_line("[Probe] target buffs past the end 0"));
    // A new target whose buffs have not come yet: the old target's are not given as its.
    h.printed.clear();
    w.target_id = 11;
    h.run("/probetargetbuffs");
    CHECK(h.printed_line("[Probe] target buffs count 0, total 0, 8 bytes, 0 records, 0 empty, 0 bytes left"));
    // The player itself targeted: the read does not hide the player's own list, though the client's target buff
    // list draws nothing then (docs/plugins.md).
    h.printed.clear();
    w.target_id = w.player_id;
    sp.target_buffs_entity = w.player_id;
    sp.target_buffs = {{1, target_buff(11, 2, "Self")}};
    h.run("/probetargetbuffs");
    CHECK(h.printed_line("[Probe] target buff slot 1: spell 11, tics 2, spell type 1, caster Self"));
    CHECK(h.printed_line("[Probe] target buffs count 1, total 1, 32 bytes, 1 records, 0 empty, 0 bytes left"));
    // No target at all.
    h.printed.clear();
    w.target_id = 0;
    sp.target_buffs_entity = 0;
    h.run("/probetargetbuffs");
    CHECK(h.printed_line("[Probe] target buffs count 0, total 0, 8 bytes, 0 records, 0 empty, 0 bytes left"));
    // No game connection at all (the login screen): count 0 and total 0 too, as the other list reads.
    h.printed.clear();
    auto conn = std::move(h.world.conn);
    h.run("/probetargetbuffs");
    CHECK(h.printed_line("[Probe] target buffs count 0, total 0, 8 bytes, 0 records, 0 empty, 0 bytes left"));
    h.world.conn = std::move(conn);
}
TEST_CASE("native plugins: get_target_buff_data_bulk holds the countdowns the target window holds") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    using A = net::GameConnectionTestAccess;
    net::GameConnection& conn = *h.world.conn;
    auto& sp = A::spells(conn);
    auto& w = A::world(conn);
    // 3010 suspendable, 3011 not, 3012 suspendable with SPA 0x1b4 (excused from the player's hold only).
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(mq_test::spell_line({{0, "3010"}, {1, "Test Ward"}, {83, "1"}, {200, "1"}}) + "\n" +
                         mq_test::spell_line({{0, "3011"}, {1, "Test Plain"}, {83, "1"}}) + "\n" +
                         mq_test::spell_line({{0, "3012"}, {1, "Test Stasis"}, {83, "1"}, {86, "436"}, {200, "1"}})) ==
            3);
    conn.set_spell_table_for_test(table);
    const std::int64_t now = net::GameConnection::now_ms();
    auto target_buff = [now](std::uint32_t spell) {
        game::BuffSlot b;
        b.spell = spell;
        b.tics = 10;
        b.received_ms = now - 3 * game::kBuffTickMs; // three ticks ago
        return b;
    };
    REQUIRE(w.target_id == 10);
    sp.target_buffs_entity = 10;
    sp.target_buffs = {{0, target_buff(3010)}, {1, target_buff(3011)}, {2, target_buff(3012)}};
    auto read = [&h] {
        h.printed.clear();
        h.run("/probetargetbuffs");
    };
    // Stock: everything counts down.
    read();
    CHECK(h.printed_line("[Probe] target buff slot 0: spell 3010, tics 7, spell type 1, caster -"));
    CHECK(h.printed_line("[Probe] target buff slot 1: spell 3011, tics 7, spell type 1, caster -"));
    CHECK(h.printed_line("[Probe] target buff slot 2: spell 3012, tics 7, spell type 1, caster -"));
    // The player's hold does not reach another spawn's buffs.
    sp.buff_timers_on_hold = true;
    read();
    CHECK(h.printed_line("[Probe] target buff slot 0: spell 3010, tics 7, spell type 1, caster -"));
    // A zone that suspends buffs holds anyone's suspendable ones, SPA 0x1b4 or not.
    sp.buff_timers_on_hold = false;
    sp.zone_suspends_buffs = true;
    read();
    CHECK(h.printed_line("[Probe] target buff slot 0: spell 3010, tics 10, spell type 1, caster -"));
    CHECK(h.printed_line("[Probe] target buff slot 1: spell 3011, tics 7, spell type 1, caster -"));
    CHECK(h.printed_line("[Probe] target buff slot 2: spell 3012, tics 10, spell type 1, caster -"));
    // The player its own target: its hold holds the list, but not the spell with SPA 0x1b4 (@0x4dcf20).
    sp.zone_suspends_buffs = false;
    sp.buff_timers_on_hold = true;
    w.target_id = w.player_id;
    sp.target_buffs_entity = w.player_id;
    CHECK(conn.target_buffs_are_own());
    read();
    CHECK(h.printed_line("[Probe] target buff slot 0: spell 3010, tics 10, spell type 1, caster -"));
    CHECK(h.printed_line("[Probe] target buff slot 1: spell 3011, tics 7, spell type 1, caster -"));
    CHECK(h.printed_line("[Probe] target buff slot 2: spell 3012, tics 7, spell type 1, caster -"));
    sp.target_buffs_entity = 10;
    CHECK_FALSE(conn.target_buffs_are_own());
}

TEST_CASE("native plugins: get_avail_aa_data_bulk and get_avail_disc_data_bulk, laid out as docs/plugins.md says") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    using A = net::GameConnectionTestAccess;
    // Nothing owned, no disciplines: the header alone, count 0 and total 0.
    h.run("/probeaas");
    CHECK(h.printed_line("[Probe] aas count 0, total 0, 8 bytes, 0 bytes left"));
    h.run("/probediscs");
    CHECK(h.printed_line("[Probe] discs count 0, total 0, 8 bytes, 0 bytes left"));

    // Owned ranks by their ability (group) id with the rank's name (dbstr type 1), in the owned list's order: an empty
    // slot and a rank the AA table does not hold (no group id known) are left out. A title the game folder's
    // dbstr_us.txt lacks gives an empty name.
    auto& aa = A::alt_adv(*h.world.conn);
    auto rank = [](std::uint32_t id, std::int32_t group, std::int32_t number, std::int32_t title) {
        game::AltAbility a;
        a.id = id;
        a.group = group;
        a.rank = number;
        a.title_sid = title;
        return a;
    };
    aa.add(rank(1003, 5, 3, 9001)); // the third rank of ability 5, Innate Strength
    aa.add(rank(2001, 77, 1, -1));  // no title
    aa.owned = {{1003, 3, 0}, {0, 0, 0}, {9999, 1, 0}, {2001, 1, 0}};
    h.run("/probeaas");
    CHECK(h.printed_line("[Probe] aas 0: ability 5, name Innate Strength"));
    CHECK(h.printed_line("[Probe] aas 1: ability 77, name -"));
    CHECK(h.printed_line("[Probe] aas count 2, total 2, 39 bytes, 0 bytes left")); // 8, 8+15, 8+0
    // As many whole records, names included, as fit after the header, whose total still says how many the client
    // holds; below the header's 8 bytes nothing, and nothing past the end.
    h.printed.clear();
    h.run("/probeaas 38");
    CHECK(h.printed_line("[Probe] aas 0: ability 5, name Innate Strength"));
    CHECK_FALSE(h.printed_line("[Probe] aas 1: ability 77, name -"));
    CHECK(h.printed_line("[Probe] aas count 1, total 2, 31 bytes, 0 bytes left"));
    CHECK(h.printed_line("[Probe] aas past the end 0"));
    h.printed.clear();
    h.run("/probeaas 8"); // the header alone: no record fits
    CHECK(h.printed_line("[Probe] aas count 0, total 2, 8 bytes, 0 bytes left"));
    h.printed.clear();
    h.run("/probeaas 7");
    CHECK(h.printed_line("[Probe] aas 0 bytes"));
    CHECK(h.printed_line("[Probe] aas past the end 0"));

    // The profile's discipline list without its empty entries, in list order, each with its spell's name (none for
    // a spell not in spells_us.txt).
    A::profile(*h.world.conn)->disciplines = {4498, 0, 99999, 0, 0};
    h.run("/probediscs");
    CHECK(h.printed_line("[Probe] discs 0: spell 4498, name Test Discipline"));
    CHECK(h.printed_line("[Probe] discs 1: spell 99999, name -"));
    CHECK(h.printed_line("[Probe] discs count 2, total 2, 39 bytes, 0 bytes left")); // 8, 8+15, 8+0
    h.printed.clear();
    h.run("/probediscs 38");
    CHECK(h.printed_line("[Probe] discs 0: spell 4498, name Test Discipline"));
    CHECK_FALSE(h.printed_line("[Probe] discs 1: spell 99999, name -"));
    CHECK(h.printed_line("[Probe] discs count 1, total 2, 31 bytes, 0 bytes left"));
    CHECK(h.printed_line("[Probe] discs past the end 0"));
    h.printed.clear();
    h.run("/probediscs 7");
    CHECK(h.printed_line("[Probe] discs 0 bytes"));
    CHECK(h.printed_line("[Probe] discs past the end 0"));
    // Before the profile: the header, count 0 and total 0.
    h.printed.clear();
    A::profile(*h.world.conn).reset();
    h.run("/probediscs");
    CHECK(h.printed_line("[Probe] discs count 0, total 0, 8 bytes, 0 bytes left"));
    h.run("/probeaas"); // the owned list is still there, but goes out only with the profile
    CHECK(h.printed_line("[Probe] aas count 0, total 0, 8 bytes, 0 bytes left"));
    // No game connection at all (the login screen): count 0 and total 0 too.
    h.printed.clear();
    auto conn = std::move(h.world.conn);
    h.run("/probeaas");
    CHECK(h.printed_line("[Probe] aas count 0, total 0, 8 bytes, 0 bytes left"));
    h.run("/probediscs");
    CHECK(h.printed_line("[Probe] discs count 0, total 0, 8 bytes, 0 bytes left"));
    h.world.conn = std::move(conn);
}
TEST_CASE("native plugins: get_raid_data_bulk roster, pets, positions and edge cases") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
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

    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid active 1, count 3, total 3, leader 0, target 1, locked 1, loot 3, 412 bytes"));
    // Leader, group leader, main assist, looter (the leader) and present; 100% HP from the test spawn.
    CHECK(h.printed_line("[Probe] raid 0: Groupie 12 g0 f167 l48 c2 hp100 mp-1 ep-1 pet0 ph-1"));
    // The listed looter, in the zone and the player: own HP 75%, mana 25%, endurance 50%.
    CHECK(h.printed_line("[Probe] raid 1: Tester 1 g0 f416 l50 c2 hp75 mp25 ep50 pet0 ph-1"));
    // No spawn in this zone: the roster's level and class, the marker flag, and no percentages.
    CHECK(h.printed_line("[Probe] raid 2: Third 0 g1 f12 l45 c1 hp-1 mp-1 ep-1 pet0 ph-1"));

    h.printed.clear();
    h.run("/proberaid 39"); // not even the header: nothing
    CHECK(h.printed_line("[Probe] raid 0 bytes"));
    h.run("/proberaid 40"); // the header alone; count < total says the buffer was too small
    CHECK(h.printed_line("[Probe] raid active 1, count 0, total 3, leader 0, target 1, locked 1, loot 3, 40 bytes"));
    h.run("/proberaid 164"); // header + one whole record
    CHECK(h.printed_line("[Probe] raid active 1, count 1, total 3, leader 0, target 1, locked 1, loot 3, 164 bytes"));
    CHECK(h.printed_line("[Probe] raid 0: Groupie 12 g0 f167 l48 c2 hp100 mp-1 ep-1 pet0 ph-1"));
    CHECK_FALSE(h.printed_line("[Probe] raid 1: Tester 1 g0 f416 l50 c2 hp75 mp25 ep50 pet0 ph-1"));

    // 4 Entire Raid: every member may loot, so the unlisted third member gains the looter flag too.
    h.printed.clear();
    raid.loot_type = 4;
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid active 1, count 3, total 3, leader 0, target 1, locked 1, loot 4, 412 bytes"));
    CHECK(h.printed_line("[Probe] raid 0: Groupie 12 g0 f167 l48 c2 hp100 mp-1 ep-1 pet0 ph-1"));
    CHECK(h.printed_line("[Probe] raid 1: Tester 1 g0 f416 l50 c2 hp75 mp25 ep50 pet0 ph-1"));
    CHECK(h.printed_line("[Probe] raid 2: Third 0 g1 f44 l45 c1 hp-1 mp-1 ep-1 pet0 ph-1"));

    raid.clear();
    h.printed.clear();
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid active 0, count 0, total 0, leader 4294967295, target 4294967295, locked 0, loot 1, 40 bytes"));

    // Pets, position and distance, group mana/endurance, an ungrouped member, a mercenary's clean-name fallback
    // and an ordinary NPC of the same name. One Host for the whole test: the reference reader is loaded once.
    auto& w = A::world(*h.world.conn);
    h.world.add(50, "Pin", false, 10, {3, 4, 0});
    h.world.add(51, "Pin`s_pet", true, 5, {3, 4, 0});
    h.world.add(52, "Mercenary001", true, 20, {1, 2, 3});
    h.world.add(53, "Guard", true, 30, {0, 0, 0});
    w.spawns[51].pet_owner_id = 50;
    w.spawns[51].hp_percent = 55;
    w.spawns[52].mercenary = true;
    auto& group = A::group(*h.world.conn);
    group.mana_percent[50] = 40;
    group.endurance_percent[50] = 60;
    raid.active = true;
    raid.locked = false;
    raid.leader.clear();
    raid.target.clear();
    raid.looters.clear();
    raid.loot_type = 1;
    raid.members = {
        mq_test::raid_member("Pin", 0, 1, 10),
        mq_test::raid_member("Mercenary"), // the cleaned-name fallback, for a mercenary
        mq_test::raid_member("Guard", 0),  // an ordinary NPC with that name is not a raid member
    };
    h.printed.clear();
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid active 1, count 3, total 3, leader 4294967295, target 4294967295, locked 0, loot 1, 412 bytes"));
    CHECK(h.printed_line("[Probe] raid 0: Pin 50 g0 f128 l10 c1 hp100 mp40 ep60 pet51 ph55"));
    CHECK(h.printed_line("[Probe] raid pos 0: 3.00 4.00 0.00 5.00"));
    CHECK(h.printed_line("[Probe] raid 1: Mercenary 52 g-1 f128 l20 c1 hp80 mp-1 ep-1 pet0 ph-1"));
    CHECK(h.printed_line("[Probe] raid pos 1: 1.00 2.00 3.00 3.74"));
    CHECK(h.printed_line("[Probe] raid 2: Guard 0 g0 f0 l0 c0 hp-1 mp-1 ep-1 pet0 ph-1"));
    CHECK(h.printed_line("[Probe] raid pos 2: 0.00 0.00 0.00 0.00"));

    // A mercenary and a temporary pet carry their owner in pet_owner_id too (EQEmu's FillSpawnStruct), but neither is
    // the member's pet: with a real pet up that one answers, and with only a mercenary and a swarm pet there is none.
    h.world.add(54, "Pinmerc001", true, 20, {3, 4, 0});
    h.world.add(55, "Solo", false, 30, {0, 0, 0});
    h.world.add(56, "Solomerc001", true, 30, {0, 0, 0});
    h.world.add(57, "Solo`s_swarm", true, 30, {0, 0, 0});
    w.spawns[54].mercenary = true;
    w.spawns[54].pet_owner_id = 50;
    w.spawns[56].mercenary = true;
    w.spawns[56].pet_owner_id = 55;
    w.spawns[57].temp_pet = true;
    w.spawns[57].pet_owner_id = 55;
    raid.members = {mq_test::raid_member("Pin", 0, 1, 10), mq_test::raid_member("Solo", 1, 2, 30)};
    h.printed.clear();
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid 0: Pin 50 g0 f128 l10 c1 hp100 mp40 ep60 pet51 ph55"));
    CHECK(h.printed_line("[Probe] raid 1: Solo 55 g1 f128 l30 c2 hp100 mp-1 ep-1 pet0 ph-1"));
    for (std::uint32_t id : {54u, 55u, 56u, 57u}) w.spawns.erase(id);

    // The player's own record with no spawn and max HP 0: HP is unknown, not 0, but the roster's level and class
    // and the player's own mana and endurance still answer.
    auto& ps = A::status(*h.world.conn);
    ps.hp = 0;
    ps.max_hp = 0;
    w.spawns.erase(1);
    raid.members = {mq_test::raid_member("Tester", 0, 2, 50)};
    h.printed.clear();
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid active 1, count 1, total 1, leader 4294967295, target 4294967295, locked 0, loot 1, 164 bytes"));
    CHECK(h.printed_line("[Probe] raid 0: Tester 0 g0 f256 l50 c2 hp-1 mp25 ep50 pet0 ph-1"));
    CHECK(h.printed_line("[Probe] raid pos 0: 0.00 0.00 0.00 0.00"));

    // Mana and endurance past RoF2's limits: cur * 100 does not fit 32 bits, and the percent is still right.
    ps.mana = 30'000'000;
    ps.max_mana = 40'000'000;
    ps.endurance = 25'000'000;
    ps.max_endurance = 50'000'000;
    h.printed.clear();
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid 0: Tester 0 g0 f256 l50 c2 hp-1 mp75 ep50 pet0 ph-1"));

    // The player's spawn is in the world but world.player_id is not known yet: the character name says which member
    // is the player, and the distance is measured from that spawn.
    h.world.add(1, "Tester", false, 50, {0, 0, 0});
    h.world.add(58, "Farther", false, 20, {0, 0, 7});
    w.player_id = 0;
    raid.members = {mq_test::raid_member("Tester", 0, 2, 50), mq_test::raid_member("Farther", 0, 2, 20)};
    h.printed.clear();
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid 0: Tester 1 g0 f384 l50 c2 hp-1 mp75 ep50 pet0 ph-1"));
    CHECK(h.printed_line("[Probe] raid 1: Farther 58 g0 f128 l20 c2 hp100 mp-1 ep-1 pet0 ph-1"));
    CHECK(h.printed_line("[Probe] raid pos 1: 0.00 0.00 7.00 7.00"));
    w.player_id = 1;
    w.spawns.erase(58);

    // No 72-member cap: every whole record fits, and reserved says how many the roster has.
    raid.members.clear();
    for (int i = 0; i < 73; ++i)
        raid.members.push_back(mq_test::raid_member("M" + std::to_string(i), static_cast<std::uint32_t>(i % 12), 1, 50));
    h.printed.clear();
    h.run("/proberaid");
    CHECK(h.printed_line("[Probe] raid active 1, count 73, total 73, leader 4294967295, target 4294967295, locked 0, loot 1, 9092 bytes"));
    CHECK(h.printed_line("[Probe] raid 72: M72 0 g0 f0 l50 c1 hp-1 mp-1 ep-1 pet0 ph-1"));
}
TEST_CASE("native plugins: cursor_origin gives the cursor item's origin, and needs 'read'") {
    using A = net::GameConnectionTestAccess;
    auto server_item = [](std::uint32_t id) {
        net::ItemPacket ip;
        ip.type = net::kItemPacketTrade;
        ip.item.id = id;
        ip.item.name = "Found";
        ip.item.slot = net::ItemSlot::possessions(net::kSlotCursor);
        return net::encode_item_packet(false, ip);
    };
    {
        Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
        h.approve(plugin::kCapAll);
        REQUIRE(h.plugins->load("HostProbe"));
        h.run("/probecursor");
        CHECK(h.printed_line("[Probe] cursor origin 0")); // nothing held
        A::handle_zone(*h.world.conn, server_item(2001));
        REQUIRE(h.world.conn->inventory().cursor());
        h.run("/probecursor");
        CHECK(h.printed_line("[Probe] cursor origin 2")); // HAILSTONE_CURSOR_ORIGIN_SERVER
    }
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll & ~plugin::kCapRead);
    REQUIRE(h.plugins->load("HostProbe"));
    A::handle_zone(*h.world.conn, server_item(2001));
    REQUIRE(h.world.conn->inventory().cursor());
    h.run("/probecursor");
    CHECK(h.printed_line("[Probe] cursor origin 0"));
    CHECK(h.printed_line("[Plugin] HostProbe: cursor_origin needs the server's 'read' permission"));
}
TEST_CASE("native plugins: the buff bulk reads need 'read'") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll & ~plugin::kCapRead);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probebuffs 4096");
    CHECK(h.printed_line("[Probe] buffs 0 bytes, 0 empty"));
    CHECK(h.printed_line("[Plugin] HostProbe: get_buff_data_bulk needs the server's 'read' permission"));
    h.run("/probebuffnames 4096");
    CHECK(h.printed_line("[Probe] buff names 0 bytes, 0 empty, 0 bytes left"));
    h.run("/probepetbuffs 1024");
    CHECK(h.printed_line("[Probe] pet buffs 0 bytes"));
    h.run("/probespawns 16384");
    CHECK(h.printed_line("[Probe] spawns 0 bytes")); // the denial is said once per plugin, above
    h.run("/probextargets 512");
    CHECK(h.printed_line("[Probe] xtargets 0 bytes"));
    h.run("/proberaid 512");
    CHECK(h.printed_line("[Probe] raid 0 bytes"));
    h.run("/probetargetbuffs 2048");
    CHECK(h.printed_line("[Probe] target buffs 0 bytes"));
    h.run("/probeaas 2048");
    CHECK(h.printed_line("[Probe] aas 0 bytes"));
    h.run("/probediscs 2048");
    CHECK(h.printed_line("[Probe] discs 0 bytes"));
}

TEST_CASE("native plugins: use_ability and ability_state, by kind and name, held to 'read' and 'actions'") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probeability 0 Kick"); // HS_ABILITY_SKILL
    REQUIRE(h.used.size() == 1);
    CHECK(h.used[0].first == static_cast<int>(mq::AbilityKind::Skill));
    CHECK(h.used[0].second == "Kick");
    REQUIRE(h.checked.size() == 1);
    CHECK(h.checked[0].first == static_cast<int>(mq::AbilityKind::Skill));
    CHECK(h.checked[0].second == "Kick");
    CHECK(h.printed_line("[Probe] ability 0 Kick: state 1, 0 of 6000 ms, use 1"));

    // On reuse: ability_state says so, with the remaining time; use_ability is still the plugin's to try.
    h.ability_ready = false;
    h.run("/probeability 1 Guard"); // HS_ABILITY_DISCIPLINE
    REQUIRE(h.used.size() == 2);
    CHECK(h.used[1].first == static_cast<int>(mq::AbilityKind::Discipline));
    CHECK(h.used[1].second == "Guard");
    CHECK(h.printed_line("[Probe] ability 1 Guard: state 0, 1500 of 6000 ms, use 1"));

    Host denied(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    denied.approve(plugin::kCapAll & ~(plugin::kCapRead | plugin::kCapActions));
    REQUIRE(denied.plugins->load("HostProbe"));
    denied.run("/probeability 2 Innate");
    CHECK(denied.printed_line("[Plugin] HostProbe: ability_state needs the server's 'read' permission"));
    CHECK(denied.printed_line("[Plugin] HostProbe: use_ability needs the server's 'actions' permission"));
    CHECK(denied.used.empty());
    CHECK(denied.checked.empty());
}

TEST_CASE("native plugins: get_ability_data_bulk, its layout, truncation and the read gate") {
    Host h(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    h.approve(plugin::kCapAll);
    h.ability_rows = {
        {mq::AbilityKind::Skill, 30, true, 0, 6000, "Kick"},
        {mq::AbilityKind::Discipline, 4498, false, 1500, 60000, "Test Discipline"},
        {mq::AbilityKind::AltAbility, 5, true, 0, 6000, "Innate Strength"},
    };
    REQUIRE(h.plugins->load("HostProbe"));
    h.run("/probeabilities");
    CHECK(h.printed_line("[Probe] ability 0: kind 0, id 30, ready 1, 0 of 6000 ms, name Kick"));
    CHECK(h.printed_line("[Probe] ability 1: kind 1, id 4498, ready 0, 1500 of 60000 ms, name Test Discipline"));
    CHECK(h.printed_line("[Probe] ability 2: kind 2, id 5, ready 1, 0 of 6000 ms, name Innate Strength"));
    CHECK(h.printed_line("[Probe] abilities count 3, total 3, 138 bytes, 3 records, 0 bytes left"));

    // Whole records only: the third needs 47 bytes, 100 leaves 9 after the first two.
    h.printed.clear();
    h.run("/probeabilities 100");
    CHECK(h.printed_line("[Probe] ability 0: kind 0, id 30, ready 1, 0 of 6000 ms, name Kick"));
    CHECK(h.printed_line("[Probe] ability 1: kind 1, id 4498, ready 0, 1500 of 60000 ms, name Test Discipline"));
    CHECK_FALSE(h.printed_line("[Probe] ability 2: kind 2, id 5, ready 1, 0 of 6000 ms, name Innate Strength"));
    CHECK(h.printed_line("[Probe] abilities count 2, total 3, 91 bytes, 2 records, 0 bytes left"));
    CHECK(h.printed_line("[Probe] abilities past the end 0"));
    h.printed.clear();
    h.run("/probeabilities 8"); // the header alone
    CHECK(h.printed_line("[Probe] abilities count 0, total 3, 8 bytes, 0 records, 0 bytes left"));
    h.printed.clear();
    h.run("/probeabilities 7"); // below the header
    CHECK(h.printed_line("[Probe] abilities 0 bytes"));
    CHECK(h.printed_line("[Probe] abilities past the end 0"));

    // Nothing to list: the header alone, count 0 and total 0.
    Host empty(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    empty.approve(plugin::kCapAll);
    REQUIRE(empty.plugins->load("HostProbe"));
    empty.run("/probeabilities");
    CHECK(empty.printed_line("[Probe] abilities count 0, total 0, 8 bytes, 0 records, 0 bytes left"));

    // Without read: 0 bytes, and the denial said once.
    Host denied(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    denied.approve(plugin::kCapAll & ~plugin::kCapRead);
    REQUIRE(denied.plugins->load("HostProbe"));
    denied.run("/probeabilities");
    CHECK(denied.printed_line("[Probe] abilities 0 bytes"));
    CHECK(denied.printed_line("[Plugin] HostProbe: get_ability_data_bulk needs the server's 'read' permission"));

    // No game connection at all (the login screen): the header alone, count and total 0, as get_avail_aa_data_bulk
    // and get_avail_disc_data_bulk; the rows are not asked for.
    Host disconnected(false, nullptr, HAILSTONE_PROBE_PLUGIN);
    disconnected.approve(plugin::kCapAll);
    disconnected.ability_rows = {{mq::AbilityKind::Skill, 30, true, 0, 6000, "Kick"}};
    REQUIRE(disconnected.plugins->load("HostProbe"));
    auto conn = std::move(disconnected.world.conn);
    disconnected.run("/probeabilities");
    CHECK(disconnected.printed_line("[Probe] abilities count 0, total 0, 8 bytes, 0 records, 0 bytes left"));
    disconnected.printed.clear();
    disconnected.run("/probeabilities 7"); // below the header: nothing written
    CHECK(disconnected.printed_line("[Probe] abilities 0 bytes"));
    CHECK(disconnected.printed_line("[Probe] abilities past the end 0"));
    disconnected.world.conn = std::move(conn);
}
