// The Options window's social options acting as the original's COptionsWnd::GeneralPageWndNotification @0x702640
// and DisplayPageWndNotification @0x7090f0 do (game/client_options.h, net/game_connection_social.cpp): the
// anonymity rules of /anonymous and /roleplay, how loadOptions reads the confirmation keys, and, against the mock
// EQEmu, the spawn appearances, the guild invitation declined at once and the raid invitation's confirmation.

#include <doctest/doctest.h>

#include <fstream>

#include "core/ini_file.h"
#include "game/chat_text.h"
#include "game/client_options.h"
#include "game/social.h"
#include "mock_server/harness.h"
#include "net/social_packets.h"
#include "net/zone_packets.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;
using game::ClientOption;

TEST_CASE("Cmd_anonymous / Cmd_roleplay: the anonymity flags, the lines and what is sent") {
    using game::anonymous_command;
    using game::roleplay_command;
    // Utf8StrNICmp(args, word, count): the first characters, case-insensitive; the argument's end counts.
    CHECK(game::starts_with_word("OFF", "off", 3));
    CHECK(game::starts_with_word("offline", "off", 3));
    CHECK_FALSE(game::starts_with_word("of", "off", 3));
    CHECK(game::starts_with_word("only", "on", 2));
    CHECK_FALSE(game::starts_with_word("", "on", 2));

    // /anonymous: toggles, "on" sets, "off" clears; roleplaying refuses (13235, nothing sent).
    auto a = anonymous_command(0, "", "off", "on");
    CHECK(a.flags == 1);
    CHECK(a.eqstr == 13236);
    CHECK(a.changed);
    a = anonymous_command(1, "", "off", "on");
    CHECK(a.flags == 0);
    CHECK(a.eqstr == 13237);
    CHECK(anonymous_command(1, "on", "off", "on").flags == 1);
    CHECK(anonymous_command(0, "off", "off", "on").flags == 0);
    a = anonymous_command(2, "on", "off", "on");
    CHECK_FALSE(a.changed);
    CHECK(a.eqstr == 13235);

    // /roleplay: the roleplay bit; not while anonymous (8873).
    auto r = roleplay_command(0, "", "off", "on");
    CHECK(r.flags == 2);
    CHECK(r.eqstr == 13233);
    r = roleplay_command(2, "", "off", "on");
    CHECK(r.flags == 0);
    CHECK(r.eqstr == 13234);
    CHECK(roleplay_command(2, "on", "off", "on").flags == 2);
    r = roleplay_command(1, "", "off", "on");
    CHECK_FALSE(r.changed);
    CHECK(r.eqstr == 8873);
    // Both bits (the Options window's boxes allow it): "off" clears the roleplay bit only.
    CHECK(roleplay_command(3, "off", "off", "on").flags == 1);
}

TEST_CASE("Client options: the confirmation keys read as loadOptions' ReadClientINIBool and AcceptKickRequests") {
    game::ClientOptions opts;
    IniFile ini;
    opts.load(ini); // all missing: the defaults
    CHECK(opts.on(ClientOption::DismissMercenaryConfirm));
    CHECK(opts.on(ClientOption::RaidInviteConfirm));
    CHECK(opts.on(ClientOption::OfflineModeConfirm));
    CHECK(opts.on(ClientOption::AutojoinHelp));
    CHECK(opts.on(ClientOption::GuildInvites));
    CHECK_FALSE(opts.on(ClientOption::AcceptKickRequests));
    // ReadClientINIBool: 'F', 'f' or '0' first is false, anything else true.
    ini.set("Options", "DismissMercenaryConfirm", "FALSE");
    ini.set("Options", "RaidInviteConfirm", "TRUE");
    ini.set("Options", "OfflineModeConfirm", "0");
    ini.set("Defaults", "AutojoinHelp", "false");
    // AcceptKickRequests: 'T', 't' or '1' true, else false.
    ini.set("Defaults", "AcceptKickRequests", "TRUE");
    opts.load(ini);
    CHECK_FALSE(opts.on(ClientOption::DismissMercenaryConfirm));
    CHECK(opts.on(ClientOption::RaidInviteConfirm));
    CHECK_FALSE(opts.on(ClientOption::OfflineModeConfirm));
    CHECK_FALSE(opts.on(ClientOption::AutojoinHelp));
    CHECK(opts.on(ClientOption::AcceptKickRequests));
    ini.set("Defaults", "AcceptKickRequests", "yes");
    opts.load(ini);
    CHECK_FALSE(opts.on(ClientOption::AcceptKickRequests));
}

namespace {

// The client's OP_SpawnAppearance packets to the zone, in order.
std::vector<SpawnAppearance> appearances(const MockEqServer& server) {
    std::vector<SpawnAppearance> out;
    for (const auto& p : server.zone_log)
        if (from_wire(p.opcode) == Op::SpawnAppearance)
            if (auto a = decode_spawn_appearance(p)) out.push_back(*a);
    return out;
}

bool sent(const MockEqServer& server, std::uint16_t type, std::uint32_t parameter) {
    for (const auto& a : appearances(server))
        if (a.type == type && a.parameter == parameter) return true;
    return false;
}

} // namespace

TEST_CASE("mock server: the Options' social boxes send the original's spawn appearances") {
    EmptyGameDir servers;
    ConnectionOptions options = quick_options();
    options.profile_id = "test-server";
    options.servers_dir = servers.path();
    Rig rig(MockConfig::standard(), options);
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());

    // Anonymous: the flags +1, spawn appearance 0x15 with them, the spawn's flags, Options/Anonymous saved.
    conn.change_client_option(ClientOption::Anonymous, 1);
    REQUIRE(rig.until([&] { return sent(server, 21, 1); }));
    CHECK(conn.anon_flags() == 1);
    // Roleplaying as well: both bits (3), as the original's boxes add them.
    conn.change_client_option(ClientOption::Roleplaying, 1);
    REQUIRE(rig.until([&] { return sent(server, 21, 3); }));
    CHECK(conn.anon_flags() == 3);
    IniFile ini;
    REQUIRE(ini.load(servers.path() / "test-server" / "characters" / "Mock EQEmu" / "Gennro" / "options.ini"));
    CHECK(ini.get_int("Options", "Anonymous", -1) == 3);
    // /roleplay off clears the roleplay bit and keeps the option in step.
    conn.send_command("/roleplay off");
    REQUIRE(rig.until([&] { return appearances(server).back().type == 21 && appearances(server).back().parameter == 1; }));
    CHECK(conn.client_options().get(ClientOption::Anonymous) == 1);
    CHECK(conn.client_options().get(ClientOption::Roleplaying) == 0);
    // The server telling us our flags (Handle_OP_SpawnAppearance 0x15 for us): Options/Anonymous takes them.
    server.zone.send_reliable(encode(SpawnAppearance{static_cast<std::uint16_t>(server.config.own_spawn_id), 21, 0}));
    REQUIRE(rig.until([&] { return conn.anon_flags() == 0; }));
    CHECK(conn.client_options().get(ClientOption::Anonymous) == 0);

    // Autosplit (0x1c), the auto consents (0x28..0x2a) and the helm (0x2b).
    CHECK(conn.character_setting("loot_autosplit") == 0);
    CHECK(conn.set_character_setting("loot_autosplit", 1));
    CHECK(conn.autosplit());
    REQUIRE(rig.until([&] { return sent(server, 28, 1); }));
    CHECK(conn.set_character_setting("auto_consent_raid", 1));
    REQUIRE(rig.until([&] { return sent(server, 41, 1); }));
    CHECK(conn.character_setting("auto_consent_raid") == 1);
    CHECK(conn.character_setting("auto_consent_group") == 0);
    CHECK(conn.set_character_setting("show_helm", 0));
    REQUIRE(rig.until([&] { return sent(server, 43, 0); }));
    CHECK(conn.character_setting("show_helm") == 0);
    CHECK_FALSE(conn.world().player()->appearance.show_helm);
    CHECK_FALSE(conn.set_character_setting("guild_invites", 0)); // an ini option: not one of these
}

TEST_CASE("mock server: Autosplit remembers the character's choice after relog") {
    ScratchDir servers("autosplit");
    ConnectionOptions options = quick_options();
    options.profile_id = "test-server";
    options.servers_dir = servers.path();
    MockConfig config = MockConfig::standard();
    config.characters.push_back(MockCharacter{.name = "Other", .inventory = {}});

    {
        Rig first(config, options);
        REQUIRE(first.to_zone());
        CHECK(first.conn.character_setting("loot_autosplit") == 0);
        // A reliable packet confirms the mock received any earlier login traffic.
        first.conn.send_command("/tgb");
        REQUIRE(first.until([&] { return MockEqServer::count(first.server.zone_log, Op::TGB) == 1; }));
        CHECK_FALSE(sent(first.server, 28, 0));
        CHECK_FALSE(sent(first.server, 28, 1));
        REQUIRE(first.conn.set_character_setting("loot_autosplit", 1));
        REQUIRE(first.until([&] { return sent(first.server, 28, 1); }));
    }
    {
        Rig other(config, options);
        REQUIRE(other.to_zone("Other"));
        CHECK(other.conn.character_setting("loot_autosplit") == 0);
        const auto source = servers.path() / "test-server" / "characters" / "Mock EQEmu" / "Gennro";
        REQUIRE(std::filesystem::is_regular_file(source / "options.ini"));
        other.conn.copy_character_settings(source, false, true);
    }
    {
        Rig other(config, options);
        REQUIRE(other.to_zone("Other"));
        CHECK(other.conn.character_setting("loot_autosplit") == 0);
    }
    {
        Rig restored(config, options);
        REQUIRE(restored.to_zone());
        CHECK(restored.conn.character_setting("loot_autosplit") == 1);
        restored.conn.send_command("/tgb");
        REQUIRE(restored.until([&] { return MockEqServer::count(restored.server.zone_log, Op::TGB) == 1; }));
        CHECK_FALSE(sent(restored.server, 28, 0));
        CHECK_FALSE(sent(restored.server, 28, 1));
        restored.conn.send_command("/autosplit"); // Toggle from the remembered on state.
        REQUIRE(restored.until([&] { return sent(restored.server, 28, 0); }));
        CHECK(restored.conn.character_setting("loot_autosplit") == 0);
    }
    Rig disabled(config, options);
    REQUIRE(disabled.to_zone());
    CHECK(disabled.conn.character_setting("loot_autosplit") == 0);
}

TEST_CASE("mock server: GuildInvites off declines another guild's invitation; RaidInviteConfirm asks first") {
    EmptyGameDir servers; // the character's options.ini (written by the changes below) in a scratch folder
    ConnectionOptions options = quick_options();
    options.profile_id = "test-server";
    options.servers_dir = servers.path();
    Rig rig(MockConfig::standard(), options);
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());

    // Handle_OP_GuildInvite with Options/GuildInvites off: answered at once with 0xb, no invitation kept.
    conn.change_client_option(ClientOption::GuildInvites, 0);
    GuildCommand invite;
    invite.other_name = "Gennro";
    invite.my_name = "Recruiter";
    invite.guild_id = 5;
    invite.rank = 8;
    server.zone.send_reliable(encode_guild_invite(invite));
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::GuildInviteAccept) == 1; }));
    for (const auto& p : server.zone_log)
        if (from_wire(p.opcode) == Op::GuildInviteAccept) {
            auto a = decode_guild_invite_accept(p);
            REQUIRE(a);
            CHECK(a->inviter == "Recruiter");
            CHECK(a->new_member == "Gennro");
            CHECK(a->response == kGuildInviteAutoDecline);
            CHECK(a->guild_id == 5);
        }
    CHECK(conn.guild().inviter.empty());

    // A raid invitation with RaidInviteConfirm on (the default): the confirmation; yes accepts.
    RaidUpdate u;
    u.general.action = kRaidInviteSent;
    u.general.player_name = "Gennro";
    u.general.leader_name = "Boss";
    server.zone.send_reliable(encode_raid_update(u));
    REQUIRE(rig.until([&] { return conn.confirmation().has_value(); }));
    CHECK(conn.confirmation()->id == kConfirmRaidInvite);
    CHECK(conn.raid().inviter == "Boss");
    conn.answer_confirmation(true);
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::RaidInvite) == 1; }));
    for (const auto& p : server.zone_log)
        if (from_wire(p.opcode) == Op::RaidInvite) {
            auto g = decode_raid_update(p);
            REQUIRE(g);
            CHECK(g->general.action == kRaidAccept);
            CHECK(g->general.player_name == "Boss");
        }
    CHECK(conn.raid().inviter.empty());

    // Off: CRaid_HandleInvite (the raid window's accept), no confirmation.
    conn.change_client_option(ClientOption::RaidInviteConfirm, 0);
    u.general.leader_name = "Other";
    server.zone.send_reliable(encode_raid_update(u));
    REQUIRE(rig.until([&] { return conn.raid().inviter == "Other"; }));
    CHECK_FALSE(conn.confirmation().has_value());
}

namespace {
std::vector<HailstoneGuild> hailstone_guild_sent(const MockEqServer& server) {
    std::vector<HailstoneGuild> out;
    for (const auto& p : server.zone_log)
        if (auto g = decode_hailstone_guild(p, false)) out.push_back(*g);
    return out;
}
} // namespace

TEST_CASE("mock server: world-wide guild invitations through server/'s script (OP_HailstoneGuild)") {
    EmptyGameDir servers;
    ConnectionOptions options = quick_options();
    options.profile_id = "test-server";
    options.servers_dir = servers.path();
    Rig rig(MockConfig::standard(), options);
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());

    // The hello after the zone-in; until the script answers, invitations stay stock.
    REQUIRE(rig.until([&] { return !hailstone_guild_sent(server).empty(); }));
    CHECK(hailstone_guild_sent(server)[0].action == HailstoneGuildAction::Hello);
    CHECK_FALSE(conn.guild_world_invites());

    // In guild 5 at rank 1 with permission 7 (the zone-in's SpawnAppearance 22 / 23, OP_GuildUpdate action 5).
    auto me = static_cast<std::uint16_t>(server.config.own_spawn_id);
    server.zone.send_reliable(encode(SpawnAppearance{me, 22, 5}));
    server.zone.send_reliable(encode(SpawnAppearance{me, 23, 1}));
    std::vector<std::byte> perm(92);
    perm[0] = std::byte{5};
    perm[0x50] = std::byte{1};
    perm[0x54] = std::byte{7};
    perm[0x58] = std::byte{1};
    server.zone.send_reliable(AppPacket(to_wire(Op::GuildUpdate), perm));
    REQUIRE(rig.until([&] { return conn.guild().permissions.has(1, 7); }));

    // Without the script's answer an absent player gets the stock OP_GuildInvite (the server says "must be in zone").
    conn.send_command("/guildinvite Faraway");
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::GuildInvite) == 1; }));

    // With it, OP_HailstoneGuild action 1 {whom, our guild, rank 8}.
    server.zone.send_reliable(encode_hailstone_guild({HailstoneGuildAction::Hello, "", 0, kHailstoneGuildWorldInvites}, true));
    REQUIRE(rig.until([&] { return conn.guild_world_invites(); }));
    conn.send_command("/guildinvite Faraway");
    REQUIRE(rig.until([&] { return hailstone_guild_sent(server).size() == 2; }));
    auto inv = hailstone_guild_sent(server)[1];
    CHECK(inv.action == HailstoneGuildAction::Invite);
    CHECK(inv.name == "Faraway");
    CHECK(inv.guild_id == 5);
    CHECK(inv.value == 8);
    CHECK(MockEqServer::count(server.zone_log, Op::GuildInvite) == 1);

    // An invitation from another zone: the stock question, answered with OP_HailstoneGuild action 2 (not
    // OP_GuildInviteAccept).
    server.zone.send_reliable(encode(SpawnAppearance{me, 22, 0xffffffff}));
    REQUIRE(rig.until([&] { return !conn.guild().in_guild(); }));
    server.zone.send_reliable(encode_hailstone_guild({HailstoneGuildAction::Invite, "Recruiter", 9, 8}, true));
    REQUIRE(rig.until([&] { return conn.guild().inviter == "Recruiter"; }));
    CHECK(conn.guild().invite_world);
    conn.answer_guild_invite(true);
    REQUIRE(rig.until([&] { return hailstone_guild_sent(server).size() == 3; }));
    auto ans = hailstone_guild_sent(server)[2];
    CHECK(ans.action == HailstoneGuildAction::Answer);
    CHECK(ans.name == "Recruiter");
    CHECK(ans.guild_id == 9);
    CHECK(ans.value == 8);
    CHECK(MockEqServer::count(server.zone_log, Op::GuildInviteAccept) == 0);
    CHECK(conn.guild().inviter.empty());

    // Options/GuildInvites off: declined at once with 0xb, the same way.
    conn.change_client_option(ClientOption::GuildInvites, 0);
    server.zone.send_reliable(encode_hailstone_guild({HailstoneGuildAction::Invite, "Recruiter", 9, 8}, true));
    REQUIRE(rig.until([&] { return hailstone_guild_sent(server).size() == 4; }));
    CHECK(hailstone_guild_sent(server)[3].value == kGuildInviteAutoDecline);
    CHECK(conn.guild().inviter.empty());
}

TEST_CASE("mock server: SpawnAppearance 22 / 23 / 52 print the guild join, leave and rank lines and set guild_show") {
    Rig rig(MockConfig::standard(), quick_options());
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    auto me = static_cast<std::uint16_t>(server.config.own_spawn_id);
    rig.chat();

    // Joining (Handle_OP_SpawnAppearance case 0x16): 12878; the rank (0x17) against GuildStatus: 7655.
    server.zone.send_reliable(encode(SpawnAppearance{me, 22, 5}));
    server.zone.send_reliable(encode(SpawnAppearance{me, 23, 8}));
    server.zone.send_reliable(encode(SpawnAppearance{me, 52, 1}));
    REQUIRE(rig.until([&] { return conn.guild().status == 8; }));
    REQUIRE(rig.until([&] { return conn.world().player() && conn.world().player()->guild_show; }));
    auto lines = rig.chat();
    CHECK(has_line(lines, "[12878] "));
    CHECK(has_line(lines, "[7655] Recruit"));
    CHECK(conn.guild().guild_id == 5);
    CHECK(conn.world().player()->guild_rank == 8);

    // The zone-in's repeats print nothing; the removal's rank 0 leaves GuildStatus; leaving prints 12879 (no name for
    // guild 5 in the list).
    server.zone.send_reliable(encode(SpawnAppearance{me, 22, 5}));
    server.zone.send_reliable(encode(SpawnAppearance{me, 23, 8}));
    server.zone.send_reliable(encode(SpawnAppearance{me, 23, 0}));
    server.zone.send_reliable(encode(SpawnAppearance{me, 22, 0xffffffff}));
    REQUIRE(rig.until([&] { return !conn.guild().in_guild(); }));
    lines = rig.chat();
    CHECK(lines.size() == 1);
    CHECK(has_line(lines, "[12879]"));
    CHECK(conn.guild().status == 8);
    CHECK(conn.world().player()->guild_rank == 0);
}

TEST_CASE("mock server: an offline trader's spawn record (other_data bit 1) and SpawnAppearance 53 set its offline flag") {
    Rig rig(MockConfig::standard(), quick_options());
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    net::Spawn away = make_spawn("Seller", 140, 0, 1, -285, -148, 3.5f);
    away.trader = true;
    away.other_data = 2; // bOfflineMode
    server.add_spawn(away);
    net::Spawn here = make_spawn("Shopper", 141, 0, 1, -280, -148, 3.5f);
    here.trader = true;
    server.add_spawn(here);
    REQUIRE(rig.until([&] { return conn.world().spawns.contains(140) && conn.world().spawns.contains(141); }));
    CHECK(conn.world().spawns.at(140).offline);
    CHECK(conn.world().spawns.at(140).trader);
    CHECK_FALSE(conn.world().spawns.at(140).buyer);
    CHECK_FALSE(conn.world().spawns.at(141).offline);

    server.zone.send_reliable(encode(SpawnAppearance{141, 53, 1}));
    REQUIRE(rig.until([&] { return conn.world().spawns.at(141).offline; }));
    server.zone.send_reliable(encode(SpawnAppearance{140, 53, 0}));
    REQUIRE(rig.until([&] { return !conn.world().spawns.at(140).offline; }));
}

TEST_CASE("mock server: copying another character's chat settings and game options (the layout copy's Hailstone boxes)") {
    EmptyGameDir servers;
    ConnectionOptions options = quick_options();
    options.profile_id = "test-server";
    options.servers_dir = servers.path();
    Rig rig(MockConfig::standard(), options);
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::filesystem::path world = servers.path() / "test-server" / "characters" / "Mock EQEmu";
    std::filesystem::path other = world / "Other";
    std::filesystem::create_directories(other);
    const int filter = 0;
    REQUIRE(game::ChatFilters::options(filter) >= 2);
    const int setting = (conn.chat_filters().settings[filter] + 1) % game::ChatFilters::options(filter);
    {
        std::ofstream(other / "chat.ini") << "[Options]\n" << game::ChatFilters::ini_key(filter) << "=" << setting
                                          << "\n[TextColors]\nUser_1_Red=12\n";
        std::ofstream(other / "options.ini") << "[Options]\nLootAllConfirm=0\nAnonymous=3\n";
    }
    REQUIRE(conn.client_options().get(ClientOption::LootAllConfirm) == 1);

    // Neither box: nothing copied.
    conn.copy_character_settings(other, false, false);
    CHECK_FALSE(std::filesystem::exists(world / "Gennro" / "options.ini"));

    conn.copy_character_settings(other, true, true);
    CHECK(conn.chat_filters().settings[filter] == setting);
    CHECK(conn.client_options().get(ClientOption::LootAllConfirm) == 0);
    CHECK(conn.anon_flags() == 0); // the public flags stay the character's own
    CHECK(conn.client_options().get(ClientOption::Anonymous) == 0);
    IniFile chat, opts;
    REQUIRE(chat.load(world / "Gennro" / "chat.ini"));
    CHECK(chat.get_int("Options", game::ChatFilters::ini_key(filter), -1) == setting);
    CHECK(chat.get("TextColors", "User_1_Red") == "12");
    REQUIRE(opts.load(world / "Gennro" / "options.ini"));
    CHECK(opts.get_int("Options", "LootAllConfirm", -1) == 0);
    CHECK_FALSE(opts.get("Options", "Anonymous").has_value());

    // The character's own folder onto itself does nothing.
    conn.copy_character_settings(world / "Gennro", true, true);
    CHECK(conn.client_options().get(ClientOption::LootAllConfirm) == 0);
}
