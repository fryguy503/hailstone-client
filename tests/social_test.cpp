// The slash command table and line interpreter (game/slash_commands.h), the
// social rules (game/social.h), and the social, guild, raid and chat-server
// packets byte for byte against the server's RoF2 translator
// (common/patches/rof2.cpp, rof2_structs.h), its pass-through structs and
// EQEmu's UCS (ucs/clientlist.cpp).

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "game/eq_strings.h"
#include "game/slash_commands.h"
#include "game/guild_notes.h"
#include "game/social.h"
#include "game/world_state.h"
#include "net/chat_server.h"
#include "net/opcodes.h"
#include "net/social_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

std::vector<std::uint8_t> bytes_of(const AppPacket& p) {
    std::vector<std::uint8_t> out;
    for (std::byte b : p.data) out.push_back(std::to_integer<std::uint8_t>(b));
    return out;
}

AppPacket packet(std::uint16_t op, const std::vector<std::uint8_t>& b) {
    std::vector<std::byte> d;
    for (auto v : b) d.push_back(std::byte{v});
    return AppPacket(op, std::move(d));
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& b, std::size_t at) {
    return std::uint32_t{b[at]} | std::uint32_t{b[at + 1]} << 8 | std::uint32_t{b[at + 2]} << 16 | std::uint32_t{b[at + 3]} << 24;
}

std::string str_at(const std::vector<std::uint8_t>& b, std::size_t at) {
    return std::string(reinterpret_cast<const char*>(b.data() + at));
}

std::string command_name(std::string_view word, bool gm = false, const game::EqStrings* s = nullptr) {
    auto i = game::find_slash_command(word, gm, s);
    return i ? game::slash_commands()[*i].name : std::string("-");
}

// The eqstr templates the tests use (as in eqstr_us.txt).
game::EqStrings test_strings() {
    game::EqStrings s;
    s.set(5001, "Players in EverQuest:");
    s.set(5025, "%1[%2 %3] %4 (%5) %6 %7 %8 %9");
    s.set(5024, "%1[ANONYMOUS] %2 %3");
    s.set(5006, "ZONE: %1");
    s.set(5028, "There is %1 player in EverQuest.");
    s.set(5036, "There are %1 players in EverQuest.");
    s.set(6357, "%1%2%3%4%T5%6");
    s.set(1502, "Warrior");
    s.set(1503, "Champion");
    s.set(1506, "Cleric");
    s.set(1507, "Vicar");
    s.set(12311, " AFK ");
    s.set(12312, "* GM * ");
    s.set(12313, " <LINKDEAD>");
    s.set(12315, " TRADER");
    s.set(12969, "%1 day");
    s.set(12970, "%1 days");
    s.set(12971, "%1 hour");
    s.set(12972, "%1 hours");
    s.set(12973, "%1 minute");
    s.set(12974, "%1 minutes");
    s.set(6686, "%1 second");
    s.set(6687, "%1 seconds");
    s.set(6688, "%1");
    s.set(6689, "%1 and %2");
    s.set(6690, "%1, %2, and %3");
    s.set(6691, "%1, %2, %3, and %4");
    s.set(6706, "Game Time: %1");
    s.set(13205, "ALL");
    s.set(13122, "guild");
    s.set(3118, "corpse");
    s.set(3119, "GM");
    s.set(3120, "NONGM");
    s.set(3112, "friend");
    s.set(3121, "LFG");
    s.set(673, "ripoff");
    s.set(14157, "/lfg");
    s.set(13950, "/who");
    return s;
}

} // namespace

TEST_CASE("slash commands: the original's table and prefix matching") {
    auto table = game::slash_commands();
    CHECK(table.size() == 384); // the original's 391 without four voice macro commands, two card-game names and /mp3
    // The card game is left out. Achievements, the Marketplace and real estate stay (a
    // server that runs them answers their packets).
    CHECK(command_name("/marketplace") == "/marketplace");
    CHECK(command_name("/achievements") == "/achievements");
    for (const char* c : {"/legendsofnorrath", "/tcg"})
        CHECK(command_name(c) == "-");
    CHECK(command_name("/realestate") == "/realestate");
    CHECK(command_name("/leaverealestate") == "/leaverealestate");
    CHECK(command_name("/claim") == "/claim"); // (the veteran rewards: EQEmu has them)
    CHECK(command_name("/mp3") == "-"); // no music player for the player's own files
    // Voice macros are not implemented: /vplay /vtell /vgroup /vraid are unknown commands; /vr is still /vreport.
    CHECK(command_name("/vplay") == "-");
    CHECK(command_name("/vtell") == "-");
    CHECK(command_name("/vgroup") == "-");
    CHECK(command_name("/vraid") == "-");
    CHECK(game::parse_command_line("/vtell Bob 1", false).kind == game::CommandLine::Kind::Invalid);
    CHECK(command_name("/vr") == "/vreport");
    CHECK(std::string(table[0].name) == "/who");
    CHECK(std::string(table[0].handler) == "Cmd_who");
    // A typed word runs the first command (in table order) it is a prefix of.
    CHECK(command_name("/wh") == "/who");
    CHECK(command_name("/g") == "/gsay");
    CHECK(command_name("/raidwin") == "/raidwindow"); // (the raid window: its key's toggle)
    CHECK(command_name("/gu") == "/guildsay");
    CHECK(command_name("/t") == "/tell");
    CHECK(command_name("/r") == "/reply");
    CHECK(command_name("/con") == "/consider");
    CHECK(command_name("/inv") == "/invite");
    CHECK(command_name("/s") == "/split");
    CHECK(command_name("/SAY") == "/say");
    CHECK(command_name("'") == "'");
    CHECK(command_name(":") == ":");
    CHECK(command_name("/nosuch") == "-");
    // GM-only commands never match for players (the loop stops at them).
    CHECK(command_name("/zone") == "-");
    CHECK(command_name("/zone", true) == "/zone");
    // Localised names come first: /lfg is /lfgroup's eqstr name.
    auto s = test_strings();
    CHECK(command_name("/lfg", false, &s) == "/lfgroup");
    CHECK(command_name("/lfg") == "/lfgroup");
}

TEST_CASE("slash commands: GetNextCmdArg and line kinds") {
    std::string_view rest = "  'two words' next";
    CHECK(game::next_cmd_arg(rest) == "two words");
    CHECK(game::next_cmd_arg(rest) == "next");
    CHECK(game::next_cmd_arg(rest).empty());
    rest = "Bob\x12" "link\x12";
    CHECK(game::next_cmd_arg(rest) == "Bob");

    using K = game::CommandLine::Kind;
    auto l = game::parse_command_line("/tell Bob hello there", false);
    CHECK(l.kind == K::Command);
    CHECK(std::string(game::slash_commands()[l.index].name) == "/tell");
    CHECK(l.args == "Bob hello there");
    CHECK(game::parse_command_line("hello", false).kind == K::Text);
    CHECK(game::parse_command_line("#zone qeynos", false).kind == K::Text);
    CHECK(game::parse_command_line("/bogus", false).kind == K::Invalid);
    auto c = game::parse_command_line(";join general", false);
    CHECK(c.kind == K::ChatServer);
    CHECK(c.text == "join general");
    auto say = game::parse_command_line("'hi all", false);
    CHECK(say.kind == K::Command);
    CHECK(std::string(game::slash_commands()[say.index].handler) == "Cmd_say");
    CHECK(say.args == "hi all");
    CHECK(game::parse_command_line("   ", false).kind == K::Empty);
}

TEST_CASE("who: OP_WhoAllRequest is 156 bytes, filters at the RoF2 offsets") {
    WhoRequest r;
    r.name = "Bob";
    r.class_ = 2;
    r.level_low = 10;
    r.level_high = 20;
    r.type = 3;
    auto p = encode(r);
    CHECK(p.opcode == to_wire(Op::WhoAllRequest));
    auto b = bytes_of(p);
    REQUIRE(b.size() == kWhoRequestSize);
    CHECK(str_at(b, 0) == "Bob");
    CHECK(u32_at(b, 128) == 0xffffffffu); // race
    CHECK(u32_at(b, 132) == 2);           // class
    CHECK(u32_at(b, 136) == 10);
    CHECK(u32_at(b, 140) == 20);
    CHECK(u32_at(b, 144) == 0xffffffffu); // gm
    CHECK(u32_at(b, 148) == 0xffffffffu); // guild
    CHECK(u32_at(b, 152) == 3);           // type
    auto back = decode_who_request(p);
    REQUIRE(back);
    CHECK(back->name == "Bob");
    CHECK(back->type == 3);
}

TEST_CASE("who: filter words (Who_BuildAndSendRequest)") {
    auto s = test_strings();
    game::WhoContext ctx;
    ctx.guild_by_name = [](const std::string& n) -> std::optional<std::uint32_t> {
        if (n == "Knights") return 7u;
        return std::nullopt;
    };
    auto w = game::parse_who_args("all cleric 30 20", ctx, s);
    CHECK(w.type == 3);
    CHECK(w.class_ == 2);
    CHECK(w.level_low == 20);
    CHECK(w.level_high == 30);
    w = game::parse_who_args("45", ctx, s);
    CHECK(w.level_low == 45);
    CHECK(w.level_high == 45);
    w = game::parse_who_args("bob", ctx, s);
    CHECK(w.name == "bob");
    CHECK(w.type == 0);
    w = game::parse_who_args("LFG", ctx, s);
    CHECK(w.guild == static_cast<std::uint32_t>(-3));
    w = game::parse_who_args("guild", ctx, s);
    REQUIRE(w.error);
    CHECK(*w.error == 12336);
    ctx.own_guild_id = 5;
    CHECK(game::parse_who_args("guild", ctx, s).guild == 5);
    CHECK(game::parse_who_args("guild12", ctx, s).guild == 12);
    w = game::parse_who_args("Knights", ctx, s);
    CHECK(w.guild == 7);
    CHECK(game::parse_who_args("GM", ctx, s).gm == 1);
    CHECK(game::parse_who_args("human", ctx, s).race == 1);
    for (const char* args : {"friend", "friends", "all friend", "all friends", "FRIENDS ALL"}) {
        w = game::parse_who_args(args, ctx, s);
        CHECK_FALSE(w.error);
        CHECK(w.friends);
        CHECK(w.guild == static_cast<std::uint32_t>(-2));
    }
    s.set(3112, "amigos");
    CHECK(game::parse_who_args("all amigos", ctx, s).friends);
}

TEST_CASE("who: OP_FriendsWho carries the RoF2 scope and comma-separated saved list") {
    auto p = encode_friends_who({"Bob", "Other.Alice", "Anon"}, false);
    CHECK(p.opcode == to_wire(Op::FriendsWho));
    CHECK(p.opcode == 0x3956);
    const std::string expected = "0,Bob,Other.Alice,Anon";
    auto bytes = bytes_of(p);
    REQUIRE(bytes.size() == expected.size() + 1);
    CHECK(std::equal(expected.begin(), expected.end(), bytes.begin()));
    CHECK(bytes.back() == 0);

    p = encode_friends_who({"Solo"}, true);
    bytes = bytes_of(p);
    CHECK(bytes == std::vector<std::uint8_t>{'1', ',', 'S', 'o', 'l', 'o', 0});

    // CChatService::GetFriendName @0x7efa90 strips the current world's "World." with __strnicmp (any case).
    p = encode_friends_who({"World.Bob", "Other.Alice", "world.Case", "Worldly.Name", "Bare"}, true, "World");
    bytes = bytes_of(p);
    CHECK(str_at(bytes, 0) == "1,Bob,Other.Alice,Case,Worldly.Name,Bare");
    CHECK(bytes.size() == std::strlen("1,Bob,Other.Alice,Case,Worldly.Name,Bare") + 1);
}

TEST_CASE("who: OP_WhoAllResponse decodes and formats like HandleWhoAllResponse") {
    WhoResponse r;
    r.title_string = 5001;
    r.line = "---------------------------";
    r.footer_string = 5036;
    r.footer_count = 2;
    WhoEntry a;
    a.format = 5025;
    a.name = "Bob";
    a.rank_string = 12312;
    a.guild = "<Knights>";
    a.zone_string = 5006;
    a.zone_id = 4; // qeynos2... any id; the short name comes from the zone table
    a.class_ = 2;
    a.level = 30;
    a.race = 1;
    WhoEntry b;
    b.format = 5024;
    b.name = "Anon";
    b.flags = 1; // AFK
    r.entries = {a, b};
    auto p = encode(r);
    auto d = decode_who_response(p);
    REQUIRE(d);
    REQUIRE(d->entries.size() == 2);
    CHECK(d->entries[0].name == "Bob");
    CHECK(d->entries[0].guild == "<Knights>");
    CHECK(d->entries[1].flags == 1);
    CHECK(d->footer_string == 5036);
    CHECK(d->footer_count == 2);
    // The footer count is read at +0x2c and the entry count at +0x3c.
    auto bytes = bytes_of(p);
    CHECK(u32_at(bytes, 0x28) == 5036);
    CHECK(u32_at(bytes, 0x2c) == 2);
    CHECK(u32_at(bytes, 0x3c) == 2);

    auto s = test_strings();
    auto lines = game::format_who_response(*d, s, false);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "Players in EverQuest:");
    CHECK(lines[2].rfind("* GM * [30 Cleric] Bob (Human) <Knights> ZONE: ", 0) == 0);
    CHECK(lines[3] == " AFK [ANONYMOUS] Anon ");
    CHECK(lines[4] == "There are 2 players in EverQuest.");
    // Above level 50 the class column carries the level title (0x44d420).
    d->entries[0].level = 51;
    CHECK(game::format_who_response(*d, s, false)[2].find("[51 Vicar (Cleric)]") != std::string::npos);
    d->entries[0].level = 30;
    // NMS multiclass: the class field is a bit mask.
    d->entries[0].class_ = (1u << 0) | (1u << 1); // warrior + cleric
    auto multi = game::format_who_response(*d, s, true);
    CHECK(multi[2].find("[30 WAR/CLR]") != std::string::npos);
}

TEST_CASE("random: Cmd_random's numbers") {
    auto r = game::parse_random_args("");
    CHECK(!r.error);
    CHECK(r.low == 0);
    CHECK(r.high == 0);
    r = game::parse_random_args("50");
    CHECK(r.low == 0);
    CHECK(r.high == 50);
    r = game::parse_random_args("100 20");
    CHECK(r.low == 20);
    CHECK(r.high == 100);
    CHECK(*game::parse_random_args("1234567").error == 13060);
    CHECK(*game::parse_random_args("-5").error == 13061);
    CHECK(*game::parse_random_args("40000").error == 13062);
    auto p = encode_random_request(20, 100);
    auto b = bytes_of(p);
    REQUIRE(b.size() == 8);
    CHECK(u32_at(b, 0) == 20);
    CHECK(u32_at(b, 4) == 100);
    RandomReply rr{1, 100, 42, "Bob"};
    auto back = decode_random_reply(encode(rr));
    REQUIRE(back);
    CHECK(back->result == 42);
    CHECK(back->name == "Bob");
    CHECK(bytes_of(encode(rr)).size() == 76);
}

TEST_CASE("played and time texts") {
    auto s = test_strings();
    CHECK(game::format_duration(s, 0) == "0 seconds");
    CHECK(game::format_duration(s, 61) == "1 minute and 1 second");
    CHECK(game::format_duration(s, 90061) == "1 day, 1 hour, 1 minute, and 1 second");
    CHECK(game::format_duration(s, 7200) == "2 hours");
    CHECK(game::long_date(2026, 10, 20) == "Tuesday, October 20, 2026");
    CHECK(game::long_date_time(2026, 10, 20, 9, 5, 0) == "Tuesday, October 20, 2026 09:05:00");
    // The weekday is the game's, (day - 1) % 7 from Sunday (EQWorldData::CurrentGameTime), not the calendar's.
    CHECK(game::format_game_time(s, 3176, 3, 14, 1) == "Game Time: Saturday, March 14, 3176 - 12 AM");
    CHECK(game::format_game_time(s, 3176, 3, 14, 14) == "Game Time: Saturday, March 14, 3176 - 1 PM");
    CHECK(game::format_game_time(s, 3176, 3, 1, 13) == "Game Time: Sunday, March 1, 3176 - 12 PM");
    CHECK(game::format_game_time(s, 3176, 3, 28, 13) == "Game Time: Saturday, March 28, 3176 - 12 PM");
    CHECK(game::format_game_time(s, 3176, 3, 9, 13) == "Game Time: Monday, March 9, 3176 - 12 PM");
}

TEST_CASE("outgoing chat variables handle missing targets and pets and preserve literal text") {
    const game::EqStrings strings;
    auto expand = [&](std::string_view text, const game::Spawn* target = nullptr, const game::Spawn* pet = nullptr) {
        return game::expand_chat_variables(text, strings, target, pet);
    };
    CHECK(expand("%T %M %S %O %P %R") == "Target Pet it it its Race");

    game::Spawn npc;
    npc.name = "#a_gnoll012";
    npc.race = 39;
    npc.gender = 3;
    npc.is_npc = true;
    game::Spawn pet;
    pet.name = "#a_wolf007";
    CHECK(expand("%t's pet is %m. %S thanked %O for %p help. %R.", &npc, &pet) ==
          "a gnoll's pet is a wolf. it thanked it for its help. NPC.");
    CHECK(expand("'%t' item%t 100% %X %%T %", &npc) == "'a gnoll' item%t 100% %X %%T %");
    CHECK(expand("%S%S") == "itit");

    game::Spawn corpse;
    corpse.is_corpse = true;
    CHECK(expand("%R", &corpse) == "Corpse");
}

TEST_CASE("outgoing chat variables follow DoPercentConvert's positions and limit") {
    const game::EqStrings strings;
    auto expand = [&](std::string_view text, const game::Spawn* target = nullptr, bool sending = true) {
        return game::expand_chat_variables(text, strings, target, nullptr, sending);
    };
    // A code within the first two bytes, or after a space or an apostrophe.
    CHECK(expand("a%t") == "aTarget");
    CHECK(expand("ab%t") == "ab%t");
    CHECK(expand("x %t, x'%t") == "x Target, x'Target");
    // After a code the next search is measured from two bytes past it, so a code straight after a long expansion
    // is not one.
    game::Spawn npc;
    npc.name = "#a_gnoll012";
    npc.is_npc = true;
    CHECK(expand("%T%S", &npc) == "a gnoll%S");
    CHECK(expand("%T %S", &npc) == "a gnoll he");
    // A '%' that is no code is "&PCT;" in sent text (sent as '%'), which moves the measure past it; a shown line
    // leaves it, so the next '%' can be a code there.
    CHECK(expand("%%t") == "%%t");
    CHECK(expand("%%t", nullptr, false) == "%Target");
    CHECK(expand("100%") == "100%");
    // After eleven expansions every further code position gets '@'; a '%' elsewhere is left.
    CHECK(expand("%O %O %O %O %O %O %O %O %O %O %O %O %X x%O") == "it it it it it it it it it it it @O @X x%O");
}

TEST_CASE("chat templates per channel (msg_new_text)") {
    CHECK(game::channel_format(8).eqstr == 1410);
    CHECK(game::channel_format(2).eqstr == 1414);
    CHECK(game::channel_format(0).eqstr == 1415);
    CHECK(game::channel_format(7).eqstr == 1416);
    CHECK(game::channel_format(15).eqstr == 5112);
    CHECK(game::channel_format(5).eqstr == 1422);
    CHECK(!game::channel_format(5).has_language_slot);
    CHECK(game::channel_format(22).eqstr == 1036);
    CHECK(game::own_chat_echo(8) == 12344);
    CHECK(game::own_chat_echo(2) == 12346);
    CHECK(game::own_chat_echo(15) == 5109);
    CHECK(game::own_chat_echo(7) == 0);
}

TEST_CASE("emote, assist, lfg and consent packets") {
    auto e = decode_emote(encode(Emote{0, "Bob waves."}));
    REQUIRE(e);
    CHECK(e->text == "Bob waves.");
    auto a = bytes_of(encode_assist(1234));
    REQUIRE(a.size() == 4);
    CHECK(u32_at(a, 0) == 1234);
    LfgCommand lc;
    lc.on = true;
    lc.to_level = 100;
    lc.comments = "Bob";
    auto l = bytes_of(encode(lc));
    REQUIRE(l.size() == 80);
    CHECK(u32_at(l, 4) == 1);
    CHECK(u32_at(l, 12) == 100);
    CHECK(str_at(l, 16) == "Bob");
    auto c = encode_consent(false, "Bob");
    CHECK(c.opcode == to_wire(Op::Consent));
    CHECK(bytes_of(c).size() == 4);
    CHECK(encode_consent(true, "Bob").opcode == to_wire(Op::ConsentDeny));
}

TEST_CASE("guild packets: invite 136, remove 140, accept 136, MOTD 648") {
    GuildCommand g;
    g.other_name = "Alice";
    g.my_name = "Bob";
    g.guild_id = 12;
    g.rank = 8;
    auto inv = bytes_of(encode_guild_invite(g));
    REQUIRE(inv.size() == kGuildInviteSize);
    CHECK(str_at(inv, 0) == "Alice");
    CHECK(str_at(inv, 64) == "Bob");
    CHECK(inv[128] == 12);
    CHECK(u32_at(inv, 132) == 8);
    auto rem = bytes_of(encode_guild_remove(g));
    CHECK(rem.size() == kGuildRemoveSize);
    auto back = decode_guild_command(encode_guild_invite(g));
    REQUIRE(back);
    CHECK(back->my_name == "Bob");
    CHECK(back->guild_id == 12);
    CHECK(back->rank == 8);

    GuildInviteAccept acc{"Bob", "Alice", kGuildInviteDecline, 12};
    auto ab = bytes_of(encode(acc));
    REQUIRE(ab.size() == 136);
    CHECK(u32_at(ab, 128) == 9);
    CHECK(u32_at(ab, 132) == 12);

    // Hailstone's world-wide invitation (server/'s script): 80 bytes either way.
    HailstoneGuild ng{HailstoneGuildAction::Invite, "Alice", 12, 8};
    auto np = encode_hailstone_guild(ng);
    CHECK(np.opcode == kOpHailstoneGuild);
    auto nb = bytes_of(np);
    REQUIRE(nb.size() == kHailstoneGuildSize);
    CHECK(u32_at(nb, 0) == kHailstoneGuildMagic);
    CHECK(nb[4] == 1);
    CHECK(nb[6] == 1);
    CHECK(str_at(nb, 8) == "Alice");
    CHECK(u32_at(nb, 72) == 12);
    CHECK(u32_at(nb, 76) == 8);
    CHECK(!decode_hailstone_guild(np)); // the client's opcode is not a reply
    auto nr = decode_hailstone_guild(encode_hailstone_guild(ng, true));
    REQUIRE(nr);
    CHECK(nr->action == HailstoneGuildAction::Invite);
    CHECK(nr->name == "Alice");
    CHECK(nr->guild_id == 12);
    CHECK(nr->value == 8);
    auto bad = encode_hailstone_guild(ng, true);
    bad.data[0] = std::byte{0};
    CHECK(!decode_hailstone_guild(bad));

    GuildMotd m{"Bob", "Bob", "Raid at 8"};
    auto mb = bytes_of(encode_set_guild_motd(m));
    CHECK(mb.size() == 648);
    CHECK(str_at(mb, 136) == "Raid at 8");
    auto md = decode_guild_motd(encode_guild_motd(m));
    REQUIRE(md);
    CHECK(md->set_by == "Bob");
    CHECK(md->motd == "Raid at 8");
    CHECK(bytes_of(encode_get_guild_motd(12, "Bob")).size() == 0x288);
    CHECK(bytes_of(encode_guild_leader("Bob", "Alice")).size() == 128);

    auto note = bytes_of(encode_guild_public_note(7, "Bob", "Alice", "Officer"));
    REQUIRE(note.size() == 388);
    CHECK(u32_at(note, 0) == 7); // the guild id (Send_OP_GuildPublicNote)
    CHECK(str_at(note, 4) == "Bob");
    CHECK(str_at(note, 68) == "Alice");
    CHECK(str_at(note, 132) == "Officer");
    auto flags = bytes_of(encode_guild_manage_banker("Alice", 3));
    REQUIRE(flags.size() == 136);
    CHECK(str_at(flags, 4).empty()); // Send_OP_GuildManageBanker leaves the first 0x44 bytes zero
    CHECK(str_at(flags, 68) == "Alice");
    CHECK(u32_at(flags, 132) == 3);

    // CGuildMgmtWnd's OP_GuildUpdate records.
    auto url = bytes_of(encode_guild_update_text(0, "http://x"));
    REQUIRE(url.size() == 0x1050);
    CHECK(str_at(url, 0x50) == "http://x");
    auto rn = bytes_of(encode_guild_rank_name(3, "Captain"));
    REQUIRE(rn.size() == 0xb8);
    CHECK(u32_at(rn, 0) == 4);
    CHECK(u32_at(rn, 0x50) == 3);
    CHECK(str_at(rn, 0x54) == "Captain");
    auto rp = bytes_of(encode_guild_rank_permission(2, 14, true));
    REQUIRE(rp.size() == 0x5c);
    CHECK(u32_at(rp, 0) == 5);
    CHECK(u32_at(rp, 0x54) == 14);
    CHECK(rp[0x58] == 1);

    GuildPromote pr{"Alice", "Bob", 7, 6};
    auto pb = bytes_of(encode(pr));
    REQUIRE(pb.size() == kGuildPromoteSize);
    CHECK(str_at(pb, 0) == "Alice");
    CHECK(str_at(pb, 64) == "Bob");
    CHECK(u32_at(pb, 128) == 7);  // the guild id (Send_OP_GuildPromote)
    CHECK(u32_at(pb, 132) == 6); // EQEmu's Handle_OP_GuildPromote reads the new rank here
    auto pd = decode_guild_promote(encode(pr));
    REQUIRE(pd);
    CHECK(pd->target == "Alice");
    CHECK(pd->new_rank == 6);

    // Send_OP_GuildWar / Send_OP_GuildPeace: 140 bytes, 1 at +0x88.
    auto war = bytes_of(encode_guild_war(GuildWarPeace{"Carl", "Bob", 7, 14}));
    REQUIRE(war.size() == kGuildWarSize);
    CHECK(str_at(war, 0) == "Carl");
    CHECK(str_at(war, 64) == "Bob");
    CHECK(u32_at(war, 128) == 7);
    CHECK(u32_at(war, 132) == 14);
    CHECK(u32_at(war, 136) == 1);
    auto peace = encode_guild_peace(GuildWarPeace{"Carl", "Bob", 7, 0});
    CHECK(peace.opcode == to_wire(Op::GuildPeace));
    CHECK(u32_at(bytes_of(peace), 132) == 0);
    auto wd = decode_guild_war_peace(peace);
    REQUIRE(wd);
    CHECK(wd->other == "Carl");
    CHECK(wd->guild_id == 7);
}

TEST_CASE("guild member list: big-endian numbers (ENCODE(OP_GuildMemberList))") {
    GuildMemberList l;
    l.player_name = "Bob";
    l.guild_id = 3;
    GuildMember m;
    m.name = "Alice";
    m.level = 50;
    m.class_ = 2;
    m.rank = 5;
    m.public_note = "healer";
    m.zone_id = 2;
    m.tribute_enable = 1;
    m.trophy_enable = 1;
    m.total_tribute = 1234;
    m.last_tribute = 1700000000;
    l.members = {m, m};
    l.members[1].name = "Carl";
    l.members[1].tribute_flag = 0;
    auto p = encode(l);
    auto b = bytes_of(p);
    // "Bob\0", then the guild id and count big-endian.
    CHECK(b[4] == 0);
    CHECK(b[7] == 3);
    CHECK(b[11] == 2);
    auto d = decode_guild_member_list(p);
    REQUIRE(d);
    REQUIRE(d->members.size() == 2);
    CHECK(d->members[0].name == "Alice");
    CHECK(d->members[0].level == 50);
    CHECK(d->members[0].rank == 5);
    CHECK(d->members[0].public_note == "healer");
    CHECK(d->members[0].zone_id == 2);
    CHECK(d->members[1].name == "Carl");
    // The tribute fields (the original's reader @0x424e80): on, trophy on, donations, last donation, flag.
    CHECK(d->members[0].tribute_enable == 1);
    CHECK(d->members[0].trophy_enable == 1);
    CHECK(d->members[0].total_tribute == 1234);
    CHECK(d->members[0].last_tribute == 1700000000);
    CHECK(d->members[0].tribute_flag == 1);
    CHECK(d->members[1].tribute_flag == 0);
}

TEST_CASE("raid packets: commands 140 bytes, updates by action") {
    RaidGeneral g;
    g.action = kRaidInvite;
    g.player_name = "Alice";
    g.leader_name = "Bob";
    auto p = encode(g);
    CHECK(p.opcode == to_wire(Op::RaidInvite));
    auto b = bytes_of(p);
    REQUIRE(b.size() == kRaidGeneralSize);
    CHECK(u32_at(b, 0) == 3);
    CHECK(str_at(b, 4) == "Alice");
    CHECK(str_at(b, 72) == "Bob");

    RaidUpdate add;
    add.general.action = kRaidAddMember;
    add.general.player_name = "Alice";
    add.general.leader_name = "Alice";
    add.general.parameter = 2;
    add.class_ = 2;
    add.level = 60;
    add.group_leader = 1;
    auto ap = encode_raid_update(add);
    CHECK(bytes_of(ap).size() == 148);
    CHECK(bytes_of(ap)[140] == 2);
    auto ad = decode_raid_update(ap);
    REQUIRE(ad);
    CHECK(ad->general.parameter == 2);
    CHECK(ad->level == 60);
    CHECK(ad->group_leader == 1);

    RaidUpdate none;
    none.general.action = kRaidNoRaid;
    none.general.player_name = "Bob";
    auto np = encode_raid_update(none);
    CHECK(bytes_of(np).size() == 136);
    auto nd = decode_raid_update(np);
    REQUIRE(nd);
    CHECK(nd->general.action == kRaidNoRaid);

    RaidUpdate motd;
    motd.general.action = kRaidSetMotd;
    motd.motd = "Pull at 9";
    auto mp = encode_raid_update(motd);
    CHECK(bytes_of(mp).size() == 1164);
    CHECK(decode_raid_update(mp)->motd == "Pull at 9");
}

TEST_CASE("raid leader commands: RaidGeneral actions and the 1164-byte MOTD") {
    auto lock = bytes_of(encode(RaidGeneral{kRaidCmdLock, "Bob", "Bob", 0}));
    REQUIRE(lock.size() == kRaidGeneralSize);
    CHECK(u32_at(lock, 0) == 8);
    auto move = bytes_of(encode(RaidGeneral{kRaidMoveGroup, "Bob", "Alice", 3}));
    CHECK(str_at(move, 72) == "Alice");
    CHECK(u32_at(move, 136) == 3);
    auto motd = bytes_of(encode_set_raid_motd("Bob", "Pull at 9"));
    REQUIRE(motd.size() == 1164);
    CHECK(u32_at(motd, 0) == 35);
    CHECK(str_at(motd, 4) == "Bob");
    CHECK(str_at(motd, 140) == "Pull at 9");
}

TEST_CASE("raid notes: the 204-byte note and OP_RaidUpdate 0x24") {
    // Send_OP_RaidInvite_0047c430: {0x24, me, raid id at +0x44, the member at +0x48}, the note at +0x8c.
    auto note = bytes_of(encode_set_raid_note("Bob", 77, "Alice", "Main tank"));
    REQUIRE(note.size() == 204);
    CHECK(u32_at(note, 0) == 0x24);
    CHECK(str_at(note, 4) == "Bob");
    CHECK(u32_at(note, 0x44) == 77);
    CHECK(str_at(note, 0x48) == "Alice");
    CHECK(str_at(note, 0x8c) == "Main tank");
    // EQEmu's raidSetNote: the member at +0x48 (its leader_name), the note at +0x8c.
    RaidUpdate u;
    u.general = RaidGeneral{kRaidSetNote, "Bob", "Alice", 0};
    u.note = std::string(80, 'n');
    auto d = decode_raid_update(encode_raid_update(u));
    REQUIRE(d);
    CHECK(d->general.leader_name == "Alice");
    CHECK(d->note == std::string(63, 'n')); // Utf8SafeStrncpy(.., 0x40)
    // RaidCreated's raid id at +0x44
    RaidUpdate c;
    c.general = RaidGeneral{kRaidCreate, "Bob", "Bob", 0, 1234};
    CHECK(decode_raid_update(encode_raid_update(c))->general.raid_id == 1234);
}

TEST_CASE("raid state") {
    game::RaidState r;
    CHECK(r.add({"Alice", 0, 2, 60, true}));
    CHECK(!r.add({"alice", 1, 2, 60, false}));
    CHECK(r.members.size() == 1);
    CHECK(r.members[0].group == 1);
    CHECK(r.is_member("ALICE"));
    // a member added again keeps its note
    r.members[0].note = "tank";
    CHECK(!r.add({"Alice", 2, 2, 60, false}));
    CHECK(r.members[0].note == "tank");
    CHECK(r.remove("Alice"));
    CHECK(!r.remove("Alice"));
}

TEST_CASE("chat server: login, commands and events (EQEmu UCS)") {
    ChatServerInfo info;
    info.server_short_name = "nms";
    info.character = "Bob";
    info.connection_type = 'G';
    info.mail_key = "0000ABCD";
    auto login = encode_mail_login(info);
    CHECK(login.opcode == ucs::kMailLogin);
    auto lb = bytes_of(login);
    CHECK(lb[0] == 0); // UCS skips the first byte
    CHECK(str_at(lb, 1) == "SOE.EQ.nms.Bob");
    CHECK(str_at(lb, 1 + std::strlen("SOE.EQ.nms.Bob") + 1) == "G0000ABCD");
    auto cmd = bytes_of(encode_mail_command("join general"));
    CHECK(str_at(cmd, 1) == "join general");

    std::vector<std::uint8_t> msg;
    for (const char* s : {"general", "nms.Alice", "hello"}) {
        msg.insert(msg.end(), s, s + std::strlen(s));
        msg.push_back(0);
    }
    auto e = decode_chat_server_packet(packet(ucs::kChannelMessage, msg));
    REQUIRE(e);
    CHECK(e->kind == ChatServerEvent::Kind::Message);
    CHECK(e->channel == "general");
    CHECK(e->sender == "nms.Alice");
    CHECK(e->text == "hello");
    std::vector<std::uint8_t> sys = {0, 0};
    for (char c : std::string("Channels: 1=general(3)")) sys.push_back(static_cast<std::uint8_t>(c));
    sys.push_back(0);
    auto se = decode_chat_server_packet(packet(ucs::kChannelMessage, sys));
    REQUIRE(se);
    CHECK(se->channel.empty());
    CHECK(se->text == "Channels: 1=general(3)");
    std::vector<std::uint8_t> buddy = {1, 'A', 'l', 'i', 'c', 'e', 0};
    auto be = decode_chat_server_packet(packet(ucs::kBuddy, buddy));
    REQUIRE(be);
    CHECK(be->kind == ChatServerEvent::Kind::Buddy);
    CHECK(be->added);
    CHECK(be->text == "Alice");
    std::vector<std::uint8_t> ign = {0, 'x', 0};
    auto ie = decode_chat_server_packet(packet(ucs::kIgnore, ign));
    REQUIRE(ie);
    CHECK(ie->added);
}

TEST_CASE("social emotes: send_social's texts") {
    game::EqStrings s;
    s.set(12500, "%1smiles");
    s.set(12501, "%1 beams a smile at %2");
    s.set(12700, "You smile");
    s.set(12701, "You beam a smile at %1");
    s.set(12510, "%1hugs %2self");
    s.set(12511, "%1 hugs %2");
    s.set(12710, "You hug yourself");
    s.set(12711, "You hug %1");
    s.set(12548, "%1 boggles at %2, shaking %3 head and looking confused");
    s.set(12748, "%1 boggle at %2, shaking %3 head and looking confused");
    s.set(12209, "he");
    s.set(12210, "she");
    s.set(12297, "him");
    s.set(12298, "her");
    s.set(12301, "his");
    s.set(12299, "her");
    s.set(12302, "You");
    s.set(12303, "you");
    s.set(12304, "your");
    s.set(12305, "You emote '%1'");
    auto smile = game::social_for_handler("Cmd_smile");
    REQUIRE(smile);
    CHECK(smile->id == 0);
    CHECK(smile->animation == 77);
    auto t = game::build_social(s, *smile, nullptr, 0, 0, "");
    CHECK(t.others == " smiles.");
    CHECK(t.echo == "You smile.");
    std::string bob = "Bob";
    t = game::build_social(s, *smile, &bob, 0, 0, "");
    CHECK(t.others == " beams a smile at Bob.");
    CHECK(t.echo == "You beam a smile at Bob.");
    auto hug = game::social_for_handler("Cmd_hug");
    REQUIRE(hug);
    CHECK(game::build_social(s, *hug, nullptr, 1, 0, "").others == " hugs herself.");
    auto boggle = game::social_for_handler("Cmd_boggle");
    REQUIRE(boggle);
    t = game::build_social(s, *boggle, &bob, 0, 1, "");
    CHECK(t.others == " boggles at Bob, shaking his head and looking confused.");
    CHECK(t.echo == "You boggle at Bob, shaking your head and looking confused.");
    t = game::build_social(s, *smile, &bob, 0, 0, "is happy");
    CHECK(t.others == " is happy.");
    CHECK(t.echo == "You emote 'is happy'");
    auto wave = game::social_for_handler("Cmd_wave");
    REQUIRE(wave);
    CHECK(wave->needs_standing);
    CHECK(!smile->needs_standing);
    CHECK(game::social_for_handler("Cmd_gesture"));
    CHECK(!game::social_for_handler("Cmd_who"));
    auto e = bytes_of(encode_social_emote(" smiles."));
    REQUIRE(e.size() == 1028);
    CHECK(u32_at(e, 0) == 0xffffffffu);
    CHECK(str_at(e, 4) == " smiles.");
}

TEST_CASE("Hailstone AFK keepalive: input tells the zone once a minute and clears the zone's own AFK") {
    using namespace hailstone::game;
    AfkKeepalive k;
    // No input yet: nothing.
    CHECK(afk_keepalive_step(k, 1000, false, false) == AfkAction::None);
    k.last_input_ms = 2000; // the first input sends one at once
    CHECK(afk_keepalive_step(k, 2000, false, false) == AfkAction::Keepalive);
    k.last_input_ms = 50000;
    CHECK(afk_keepalive_step(k, 61999, false, false) == AfkAction::None);
    CHECK(afk_keepalive_step(k, 62000, false, false) == AfkAction::Keepalive);
    // No input since the keepalive: none, however long.
    CHECK(afk_keepalive_step(k, 500000, false, false) == AfkAction::None);
    // The zone's auto AFK: input before it does not clear it, input after it does.
    k.zone_afk_since_ms = 600000;
    k.last_input_ms = 590000;
    CHECK(afk_keepalive_step(k, 600500, true, false) == AfkAction::None);
    k.last_input_ms = 601000;
    CHECK(afk_keepalive_step(k, 601000, true, false) == AfkAction::Clear);
    CHECK(k.zone_afk_since_ms == -1);
    // /afk stays until /afk takes it off.
    k.last_input_ms = 700000;
    CHECK(afk_keepalive_step(k, 700000, true, true) == AfkAction::None);
}

TEST_CASE("guild personal notes: the GN_ file (CGuildMgmtWnd::LoadPersonalNotes / SavePersonalNotes)") {
    game::GuildPersonalNotes n;
    // "%X %s %[^\n": a line short of a note is skipped; a name read again only takes the time
    n.parse("0000ABCD Alice Bring the potions\r\nzz Bob nope\n12 Carol\n1F alice second\n00000010 Dave  two words\n", 777);
    REQUIRE(n.entries.size() == 2);
    CHECK(n.entries[0].name == "Alice");
    CHECK(n.entries[0].note == "Bring the potions\r");
    CHECK(n.entries[0].time == 777);
    CHECK(n.entries[1].name == "Dave");
    CHECK(n.entries[1].note == "two words");
    CHECK(n.entries[1].time == 0x10);
    // the name cut to 63 bytes, the note to 255
    game::GuildPersonalNotes cut;
    cut.parse("1 " + std::string(80, 'n') + " " + std::string(300, 'x') + "\n", 0);
    REQUIRE(cut.entries.size() == 1);
    CHECK(cut.entries[0].name.size() == 63);
    CHECK(cut.entries[0].note.size() == 255);
    // SetPersonalNote keeps a known name's time; a new one is stamped
    n.set("DAVE", "healer", 999);
    n.set("Erin", "tank", 999);
    CHECK(*n.get("dave") == "healer");
    CHECK(n.entries[1].time == 0x10);
    CHECK(n.entries[2].time == 999);
    // the lookup on adding a member stamps its time
    CHECK(*n.find("erin", 5) == "tank");
    CHECK(n.entries[2].time == 5);
    CHECK(n.find("Zed", 5) == nullptr);
    CHECK(n.format() == "00000309 Alice Bring the potions\r\n00000010 Dave healer\n00000005 Erin tank\n");
}

TEST_CASE("guild appearance lines: Handle_OP_SpawnAppearance cases 0x16 and 0x17") {
    using namespace hailstone::game;
    GuildState g;
    g.names = {{18, "Hailstone Guild Lab"}, {5, "Crimson Tide"}};
    g.guild_id = 0xffffffff;
    // Our own spawn joins, then leaves.
    auto join = guild_id_appearance_line(g, true, 0xffffffff, 18, "Me");
    CHECK(join.eqstr == 12878);
    CHECK(join.args == std::vector<std::string>{"Hailstone Guild Lab"});
    g.guild_id = 18;
    CHECK(guild_id_appearance_line(g, true, 18, 18, "Me").eqstr == 0); // the zone-in's repeat
    auto leave = guild_id_appearance_line(g, true, 18, 0xffffffff, "Me");
    CHECK(leave.eqstr == 12880);
    CHECK(leave.args == std::vector<std::string>{"Hailstone Guild Lab"});
    g.guild_id = 77; // a guild the list does not name
    CHECK(guild_id_appearance_line(g, true, 77, 0xffffffff, "Me").eqstr == 12879);
    // Others: joining ours, leaving ours, anything else silent.
    g.guild_id = 18;
    auto other = guild_id_appearance_line(g, false, 0xffffffff, 18, "Bob");
    CHECK(other.eqstr == 12881);
    CHECK(other.args == std::vector<std::string>{"Bob"});
    CHECK(guild_id_appearance_line(g, false, 18, 0xffffffff, "Bob").eqstr == 12882);
    CHECK(guild_id_appearance_line(g, false, 18, 18, "Bob").eqstr == 0);
    CHECK(guild_id_appearance_line(g, false, 0xffffffff, 5, "Bob").eqstr == 0);
    CHECK(guild_id_appearance_line(g, false, 5, 0xffffffff, "Bob").eqstr == 0);

    // Ranks: ours against GuildStatus, another member's against its spawn; only 1..8.
    g.status = 8;
    g.rank_names[7] = "Lab Rat";
    auto mine = guild_rank_appearance_line(g, true, 18, 8, 7, "Me");
    CHECK(mine.eqstr == 7655);
    CHECK(mine.args == std::vector<std::string>{"Lab Rat"});
    CHECK(guild_rank_appearance_line(g, true, 18, 7, 8, "Me").eqstr == 0); // already Recruit
    CHECK(guild_rank_appearance_line(g, true, 18, 8, 0, "Me").eqstr == 0); // removal's rank 0
    auto theirs = guild_rank_appearance_line(g, false, 18, 8, 6, "Bob");
    CHECK(theirs.eqstr == 7656);
    CHECK(theirs.args == std::vector<std::string>{"Bob", "Junior Member"});
    CHECK(guild_rank_appearance_line(g, false, 18, 6, 6, "Bob").eqstr == 0);
    CHECK(guild_rank_appearance_line(g, false, 5, 8, 6, "Bob").eqstr == 0); // another guild's member
    CHECK(guild_rank_appearance_line(g, false, 18, 8, 9, "Bob").eqstr == 0);
}

TEST_CASE("slash commands: cmd_atol reads a number as the original's 32-bit atol") {
    CHECK(game::cmd_atol("") == 0);
    CHECK(game::cmd_atol("abc") == 0);
    CHECK(game::cmd_atol("  12x") == 12);
    CHECK(game::cmd_atol("\t+7") == 7);
    CHECK(game::cmd_atol("-5") == -5);
    CHECK(game::cmd_atol("2147483647") == 2147483647);
    CHECK(game::cmd_atol("-2147483648") == std::numeric_limits<std::int32_t>::min());
    // Past the long's range the digits wrap in 32 bits, the same on every platform (std::atol's long is 64 bits
    // on Linux and macOS, and its overflow undefined).
    CHECK(game::cmd_atol("2147483648") == std::numeric_limits<std::int32_t>::min());
    CHECK(game::cmd_atol("4294967297") == 1);
    CHECK(game::cmd_atol("9999999999999999999") == static_cast<std::int32_t>(9999999999999999999ull & 0xffffffffull));
}
