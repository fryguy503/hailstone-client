// The group finder (CGroupSearchWnd / CGroupSearchFiltersWnd): its packets, rules, filters and the connection's
// posts, searches and results.

#include <doctest/doctest.h>

#include <cstdlib>

#include "app/lua_ui_windows.h"
#include "core/byte_io.h"
#include "core/ini_file.h"
#include "game/chat_text.h"
#include "game/group_search.h"
#include "mock_server/harness.h"
#include "net/group_packets.h"
#include "net/social_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

std::uint32_t u32(const AppPacket& p, std::size_t at) {
    ByteReader r(p.data.data(), p.data.size());
    r.seek(at);
    return r.le<std::uint32_t>();
}
std::uint8_t u8(const AppPacket& p, std::size_t at) { return static_cast<std::uint8_t>(p.data.at(at)); }
std::string text(const AppPacket& p, std::size_t at) {
    std::string s;
    for (std::size_t i = at; i < at + 64 && i < p.data.size() && p.data[i] != std::byte{0}; ++i) s.push_back(static_cast<char>(p.data[i]));
    return s;
}
std::vector<AppPacket> sent(const MockEqServer& server, Op op) {
    std::vector<AppPacket> out;
    for (const auto& p : server.zone_log)
        if (p.opcode == to_wire(op)) out.push_back(p);
    return out;
}

} // namespace

TEST_CASE("group search packets: LFG 80, LFP 84, match requests 16 / 12, the replies") {
    LfgCommand g;
    g.on = true;
    g.match_filter = true;
    g.from_level = 55;
    g.to_level = 65;
    g.comments = std::string(80, 'c');
    AppPacket a = encode(g);
    CHECK(a.opcode == 0x6060);
    REQUIRE(a.data.size() == 80);
    CHECK(u8(a, 4) == 1);
    CHECK(u8(a, 5) == 1);
    CHECK(u32(a, 8) == 55);
    CHECK(u32(a, 12) == 65);
    CHECK(text(a, 16).size() == 63);

    LfpCommand l;
    l.on = true;
    l.from_level = 1;
    l.to_level = 100;
    l.classes = 0x1fffe;
    l.comments = "need heals";
    AppPacket b = encode(l);
    CHECK(b.opcode == 0x49a9);
    REQUIRE(b.data.size() == 84);
    CHECK(u8(b, 4) == 1);
    CHECK(u8(b, 5) == 0);
    CHECK(u32(b, 16) == 0x1fffe);
    CHECK(text(b, 20) == "need heals");

    AppPacket q = encode_lfg_matches_request(1, 100, 0xffffffff);
    CHECK(q.opcode == 0x0340);
    REQUIRE(q.data.size() == 16);
    CHECK(u32(q, 12) == 0xffffffff);
    AppPacket r = encode_lfp_matches_request(10, 20);
    CHECK(r.opcode == 0x4d7d);
    REQUIRE(r.data.size() == 12);
    CHECK(u32(r, 8) == 20);

    // EQEmu's replies (zone/worldserver.cpp HandleLFGMatches / HandleLFPMatches), signed shorts.
    std::vector<LfgMatch> players{{"lfg!", "Bob", 2, 60, 202, -1, 0}, {"", "Ann", 16, 5, 1, 7, 1}};
    auto pd = decode_lfg_matches(encode(players));
    REQUIRE(pd);
    REQUIRE(pd->size() == 2);
    CHECK((*pd)[0].name == "Bob");
    CHECK((*pd)[0].comments == "lfg!");
    CHECK((*pd)[0].guild == -1);
    CHECK((*pd)[1].class_ == 16);
    CHECK((*pd)[1].anon == 1);
    LfpMatch m;
    m.comments = "xp group";
    m.from_level = 50;
    m.to_level = 60;
    m.classes = 1u << 2;
    m.members = {{"Lead", 1, 58, 202, 3}, {"Two", 2, 57, 202, 3}};
    m.count = 2;
    auto gd = decode_lfp_matches(encode(std::vector<LfpMatch>{m, m}));
    REQUIRE(gd);
    REQUIRE(gd->size() == 2);
    CHECK((*gd)[1].count == 2);
    REQUIRE((*gd)[1].members.size() == 2);
    CHECK((*gd)[1].members[1].name == "Two");
    CHECK((*gd)[0].classes == 4);
    // A cut record is dropped, the ones before it kept.
    AppPacket cut = encode(std::vector<LfpMatch>{m, m});
    cut.data.resize(cut.data.size() - 3);
    auto cd = decode_lfp_matches(cut);
    REQUIRE(cd);
    CHECK(cd->size() == 1);
}

TEST_CASE("group search rules: class order and mask, levels, ranges, the query lock, errors") {
    CHECK(game::group_search_class_id(0) == 8);   // Brd
    CHECK(game::group_search_class_id(14) == 1);  // War
    CHECK(game::group_search_class_id(16) == 0);
    std::array<bool, 16> boxes{};
    boxes[3] = true; // Clr
    CHECK(game::group_search_class_mask(boxes) == 1u << 2);
    CHECK(game::group_search_all_classes() == 0x1fffe);
    CHECK(game::group_search_default_levels(3, true) == std::array<int, 2>{1, 8});
    CHECK(game::group_search_default_levels(98, true) == std::array<int, 2>{93, 100});
    CHECK(game::group_search_default_levels(0, false) == std::array<int, 2>{1, 100});
    CHECK(game::group_search_range_ok(1, 100));
    CHECK(game::group_search_range_ok(150, 0) == false);
    CHECK(game::group_search_range_ok(101, 200) == false);
    CHECK(game::group_search_range_ok(10, 5) == false);
    CHECK(game::group_search_range_ok(-5, 1));
    CHECK(game::group_search_int("12x") == 12);
    CHECK(game::group_search_int("") == 0);
    CHECK(game::group_search_query_lock(10000, 10000) == 15);
    CHECK(game::group_search_query_lock(10999, 10000) == 15);
    CHECK(game::group_search_query_lock(11000, 10000) == 14);
    CHECK(game::group_search_query_lock(25000, 10000) == 0);
    CHECK(game::group_search_query_lock(5000, 0) == 10);
    CHECK(game::group_search_error_eqstr(1) == 6713);
    CHECK(game::group_search_error_eqstr(3) == 6715);
    CHECK(game::group_search_error_eqstr(0) == 0);
    CHECK(game::censor_bad_words("hi there") == "hi there");
}

TEST_CASE("group search filters: [ADDITIONALFILTERS] saved and read back, the rules in the original's order") {
    IniFile ini;
    game::GroupSearchFilters f;
    CHECK(f.player_mode == 1); // the ignore list by default
    f.load(ini);
    CHECK(f.player_mode == 1);
    CHECK(f.guild_mode == 0);
    f.player_mode = 2;
    f.guild_mode = 1;
    f.exclusive = true;
    game::GroupSearchFilters::add_guild(f.excluded, "Bad Guild");
    game::GroupSearchFilters::add_guild(f.excluded, "Worse");
    game::GroupSearchFilters::add_guild(f.excluded, "Bad Guild");
    CHECK(f.excluded.size() == 2);
    f.save(ini);
    CHECK(ini.get_string("ADDITIONALFILTERS", "GUILDEXCLUDED1") == "Worse");
    CHECK(ini.get("ADDITIONALFILTERS", "GUILDEXCLUDED2").value_or("x").empty()); // the end mark
    CHECK(ini.get("ADDITIONALFILTERS", "GUILDDESIRED0").value_or("x").empty());
    game::GroupSearchFilters g;
    g.load(ini);
    CHECK(g.player_mode == 2);
    CHECK(g.guild_mode == 1);
    CHECK(g.exclusive);
    CHECK(g.excluded == f.excluded);
    CHECK(g.desired.empty());

    auto ignored = [](const std::string& n) { return n == "Pest"; };
    auto friend_of = [](const std::string& n) { return n == "Pal"; };
    game::GroupSearchFilters r;
    CHECK(game::group_search_filtered(r, "Pest", "", ignored, friend_of));
    CHECK_FALSE(game::group_search_filtered(r, "Pal", "", ignored, friend_of));
    r.player_mode = 2;
    CHECK(game::group_search_filtered(r, "Stranger", "", ignored, friend_of));
    // A guild rule overrides the player rule (each active rule overwrites the answer).
    r.player_mode = 1;
    r.guild_mode = 1;
    r.excluded = {"Bad Guild"};
    CHECK_FALSE(game::group_search_filtered(r, "Pest", "Good", ignored, friend_of));
    CHECK(game::group_search_filtered(r, "Pal", "Bad Guild", ignored, friend_of));
    r.guild_mode = 2;
    r.desired = {"Good"};
    CHECK(game::group_search_filtered(r, "Pal", "Unknown Guild", ignored, friend_of));
    CHECK_FALSE(game::group_search_filtered(r, "Pest", "Good", ignored, friend_of));
}

TEST_CASE("mock server: /lfg, the group finder's posts, searches, results and joining a group") {
    Rig rig(MockConfig::standard(), quick_options());
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.world().player());
    const int level = static_cast<int>(conn.world().player()->level);
    auto lv = game::group_search_default_levels(level, true);

    // /lfg on: Cmd_lfgroup's own record, then the window's post with its fields.
    conn.send_command("/lfg on");
    REQUIRE(rig.until([&] { return sent(server, Op::LFGCommand).size() == 2; }));
    auto lfg = sent(server, Op::LFGCommand);
    CHECK(u8(lfg[0], 4) == 1);
    CHECK(u32(lfg[0], 8) == 0);
    CHECK(u32(lfg[0], 12) == 100);
    CHECK(u32(lfg[1], 8) == static_cast<std::uint32_t>(lv[0]));
    CHECK(u32(lfg[1], 12) == static_cast<std::uint32_t>(lv[1]));
    CHECK(conn.group_search().lfg.on);
    CHECK(conn.social().lfg);
    CHECK_FALSE(conn.group_search_player_changed());
    conn.group_search().player_comments = "anything";
    CHECK(conn.group_search_player_changed());
    conn.group_search_post_player(true);
    REQUIRE(rig.until([&] { return sent(server, Op::LFGCommand).size() == 3; }));
    CHECK(text(sent(server, Op::LFGCommand)[2], 16) == "anything");
    conn.send_command("/lfg off");
    REQUIRE(rig.until([&] { return sent(server, Op::LFGCommand).size() == 5; }));
    CHECK_FALSE(conn.group_search().lfg.on);
    CHECK(conn.group_search().lfg_remove_checked);
    // Anything else shows the window.
    conn.send_command("/lfg");
    CHECK(conn.group_search().window_open);

    // A bad range: the dialog (6713) and nothing sent.
    conn.group_search().group_min = "90";
    conn.group_search().group_max = "10";
    conn.group_search_post_group(false);
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->ok_only);
    conn.answer_confirmation(false);
    conn.group_search().group_max = "95";
    conn.group_search().roles.fill(false);
    conn.group_search_query_players(); // no class: 6714
    REQUIRE(conn.confirmation());
    conn.answer_confirmation(false);
    conn.group_search().roles[3] = true; // Clr
    conn.group_search().group_comments = "need a cleric";
    conn.group_search_post_group(false);
    REQUIRE(rig.until([&] { return !sent(server, Op::LFPCommand).empty(); }));
    auto lfp = sent(server, Op::LFPCommand).back();
    CHECK(u32(lfp, 8) == 90);
    CHECK(u32(lfp, 12) == 95);
    CHECK(u32(lfp, 16) == 1u << 2);
    CHECK(text(lfp, 20) == "need a cleric");

    // Get Matches: the group page's range and classes for players; Show All for groups.
    conn.group_search_query_players();
    REQUIRE(rig.until([&] { return !sent(server, Op::LFGGetMatchesRequest).empty(); }));
    auto q = sent(server, Op::LFGGetMatchesRequest).back();
    CHECK(u32(q, 4) == 90);
    CHECK(u32(q, 12) == 1u << 2);
    conn.group_search().groups_show_all = true;
    conn.group_search_query_groups();
    REQUIRE(rig.until([&] { return !sent(server, Op::LFPGetMatchesRequest).empty(); }));
    CHECK(u32(sent(server, Op::LFPGetMatchesRequest).back(), 8) == 100);

    // The replies: the ignore list (the default filter) drops a player unless the search was Show All.
    conn.send_command("/ignore Pest");
    server.zone.send_reliable(encode(std::vector<LfgMatch>{{"", "Pest", 2, 60, 1, -1, 0}, {"hi", "Fine", 2, 60, 1, -1, 0}}));
    REQUIRE(rig.until([&] { return conn.group_search().results_serial == 1; }));
    bool pest_ignored = !conn.social().ignored.empty();
    CHECK(conn.group_search().players.size() == (pest_ignored ? 1u : 2u));
    LfpMatch m;
    m.members = {{"Pest", 1, 58, 202, 3}};
    m.count = 1;
    server.zone.send_reliable(encode(std::vector<LfpMatch>{m}));
    REQUIRE(rig.until([&] { return conn.group_search().results_serial == 2; }));
    CHECK(conn.group_search().groups.size() == 1); // Show All

    // Joining someone's group ends both posts (0x678550: not the leader).
    std::size_t lfg_before = sent(server, Op::LFGCommand).size(), lfp_before = sent(server, Op::LFPCommand).size();
    conn.group_search_joined_group();
    REQUIRE(rig.until([&] { return sent(server, Op::LFPCommand).size() == lfp_before + 1; }));
    CHECK(sent(server, Op::LFGCommand).size() == lfg_before + 1);
    CHECK_FALSE(conn.group_search().lfp.on);
}

TEST_CASE("CListWnd's own sort: header clicks, the window's 0x10 answer, else the texts bytewise") {
    app::ListData d;
    for (const char* t : {"10", "9", "b", "B", "9"}) d.rows.push_back(app::ListRow{{t}});
    std::vector<int> keys{0, 1, 2, 3, 4};
    app::ListSort s;
    CHECK(s.order(d.rows) == std::vector<std::size_t>{0, 1, 2, 3, 4}); // unsorted: the window's order
    s.header_click(0);
    CHECK(s.ascending);
    // bytewise ("10" < "9" < "B" < "b"), stable for the equal "9"s
    CHECK(s.order(d.rows) == std::vector<std::size_t>{0, 1, 4, 3, 2});
    s.header_click(0); // the same column flips the direction
    CHECK(!s.ascending);
    CHECK(s.order(d.rows) == std::vector<std::size_t>{2, 3, 1, 4, 0});
    s.header_click(0);
    // 0x677920's atoi answer
    auto atoi_cmp = [&](std::size_t a, std::size_t b) -> std::optional<int> {
        int x = std::atoi(d.rows[a].texts[0].c_str()), y = std::atoi(d.rows[b].texts[0].c_str());
        return x < y ? -1 : x > y ? 1 : 0;
    };
    auto order = s.order(d.rows, atoi_cmp);
    CHECK(order == std::vector<std::size_t>{2, 3, 1, 4, 0}); // "b", "B" are 0
    app::ListSort::apply(d, keys, order);
    CHECK(keys == std::vector<int>{2, 3, 1, 4, 0});
    CHECK(d.rows[4].texts[0] == "10");
}
