// The client option commands' rules (game/option_commands.h).
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/ini_file.h"
#include "game/client_options.h"
#include "game/hotbuttons.h"
#include "game/chat_input.h"
#include "game/keymap.h"
#include "game/option_commands.h"
#include "game/task_window.h"
#include "platform/binary_stdio.h"
#include "platform/local_time.h"
#include "mq_test_world.h"

using namespace hailstone;
using namespace hailstone::game;

TEST_CASE("option commands: word tests") {
    CHECK(is_numeric_string("12"));
    CHECK(is_numeric_string("-3"));
    CHECK(is_numeric_string("+"));
    CHECK_FALSE(is_numeric_string(""));
    CHECK_FALSE(is_numeric_string("1a"));
    CHECK(off_on_toggle("off", "off", "on") == Switch::Off);
    CHECK(off_on_toggle("OFFbeat", "off", "on") == Switch::Off); // 3 characters
    CHECK(off_on_toggle("one", "off", "on") == Switch::On);      // 2 characters
    CHECK(off_on_toggle("", "off", "on") == Switch::Toggle);
    CHECK(off_on_toggle("of", "off", "on") == Switch::Toggle);
}

TEST_CASE("option commands: /copylayout's ini is a bare file name, never a path") {
    CHECK(bare_file_name("UI_Bob_Server.ini"));
    CHECK(bare_file_name("defaults.ini"));
    CHECK(bare_file_name("a..ini")); // dots inside a name are no folder
    CHECK_FALSE(bare_file_name(""));
    CHECK_FALSE(bare_file_name("."));
    CHECK_FALSE(bare_file_name(".."));
    CHECK_FALSE(bare_file_name("../hailstone.ini"));
    CHECK_FALSE(bare_file_name("sub/UI.ini"));
    CHECK_FALSE(bare_file_name("..\\eqclient.ini"));
    CHECK_FALSE(bare_file_name("/etc/passwd"));
    CHECK_FALSE(bare_file_name("C:UI.ini"));
    CHECK_FALSE(bare_file_name("C:\\x\\UI.ini"));
    CHECK_FALSE(bare_file_name(std::string_view("UI\0.ini", 6)));
}

TEST_CASE("option commands: /safelock and the locked interface") {
    std::string pw;
    CHECK(safelock_command(pw, "") == std::vector<std::uint32_t>{13287, 13288});
    CHECK(safelock_command(pw, "Secret") == std::vector<std::uint32_t>{13291});
    CHECK(pw == "Secret");
    CHECK(safelock_command(pw, "") == std::vector<std::uint32_t>{kStrInterfaceLocked});
    CHECK(safelock_command(pw, "wrong") == std::vector<std::uint32_t>{13290});
    CHECK(safelock_command(pw, "SECRET") == std::vector<std::uint32_t>{13289}); // not case-sensitive
    CHECK(pw.empty());
    CHECK(safelock_command(pw, std::string(0x20, 'x')) == std::vector<std::uint32_t>{13292});
    CHECK(pw.empty());

    CHECK(locked_line_allowed("/safelock x", "/safelock"));
    CHECK(locked_line_allowed("/safe x", "/safelock"));
    CHECK_FALSE(locked_line_allowed("/SAFELOCK x", "/safelock")); // CompareN with case
    CHECK_FALSE(locked_line_allowed("/sit", "/safelock"));
    CHECK_FALSE(locked_line_allowed("hello", "/safelock"));
    CHECK_FALSE(locked_line_allowed(";tell x", "/safelock"));
    CHECK_FALSE(locked_line_allowed("[x", "/safelock"));
}

TEST_CASE("option commands: the key commands a locked interface refuses (__ExecuteCmd's control flag)") {
    using C = KeyCommand;
    auto needs = [](C c) { return key_command_needs_control(static_cast<int>(c)); };
    // Movement, targeting, hot buttons and pages, gems, the Actions window's keys, camp, sit: refused.
    for (C c : {C::AUTORUN, C::JUMP, C::FORWARD, C::STRAFE_RIGHT, C::MOUSELOOK, C::AUTOPRIM, C::CONSIDER, C::DUCK,
                C::INVENTORY, C::USE, C::CENTERVIEW, C::TOGGLECAM, C::TARGETME, C::PARTY5, C::TARGETNPC,
                C::CYCLECORPSETARGETS, C::NETSTAT, C::HOT1_1, C::HOT10_12, C::HOTPAGE1_1, C::HOTPAGE10_10, C::CAST1,
                C::CAST12, C::CYCLEREPLY, C::CYCLEREPLY_BACK, C::TOGGLETARGET, C::SPELLBOOK, C::ABILITIES, C::MAIN,
                C::WHO, C::CAMP, C::SIT_STAND, C::RUN_WALK, C::SCREENCAP, C::TOGGLETWOTARGETS, C::TETHER_CAMERA,
                C::CLOSE_TOP_WINDOW, C::CLEAR_TARGET, C::CMD_TOGGLE_FIND_WINDOW, C::CMD_OPENING_ATTACK1,
                C::CMD_TOGGLE_COMBAT_ABILITY_WIN, C::CMD_STOP_CAST, C::POTION_SLOT_1, C::POTION_SLOT_5,
                C::TOGGLE_POTION_BELT, C::TOGGLE_BANDOLIER, C::TOGGLE_INV_BAGS, C::TOGGLE_SKILLS_WND,
                C::CMD_EXTENDED_TARGET_WINDOW, C::CMD_STOP_ACTION, C::CMD_TOGGLE_BANK_BAGS, C::CMD_LOCK_AGGRO})
        CHECK_MESSAGE(needs(c), static_cast<int>(c));
    // The window toggles, HAIL, TELL, REPLY, the camera's pitch, zoom and clip, the extended target keys: still act.
    for (C c : {C::HAIL, C::TELL, C::PITCHUP, C::PITCHDOWN, C::ZOOMIN, C::ZOOMOUT, C::FULLSCREEN, C::REPLY,
                C::BACKDROP, C::CLIP_IN, C::CLIP_OUT, C::VOICE_ON, C::HISTORY_UP, C::PAGEDOWN, C::CMDMODE_SAY,
                C::LOCK_WINDOWS, C::TOGGLE_PLAYERWIN, C::TOGGLE_HOTBOX1WIN, C::TOGGLE_MAINMENUWIN,
                C::TOGGLE_OPTIONSWIN, C::TOGGLE_FRIENDSWIN, C::TOGGLE_RAIDWIN, C::TOGGLE_GUILDMGMTWIN, C::FORWARD_CAM,
                C::PITCHDOWN_CAM, C::TOGGLE_ADVREQUESTWIN, C::TOGGLE_LEADERSHIPWIN, C::TOGGLE_TRIBUTEBENEFITWIN,
                C::CMD_TOGGLETASKWIN, C::CMD_TOGGLEVOICEWIN, C::CMD_TOGGLE_TITLE_WND, C::OPEN_ETN_HELP,
                C::TARGET_XTARGET_1, C::TARGET_XTARGET_20, C::ADD_XTARGET, C::REMOVE_XTARGET_BY_SLOT, C::CMD_MOVE_DOWN,
                C::CMD_TOGGLE_ALARM, C::CMD_TOGGLE_AGGRO, C::CMD_TOGGLE_BARTER_SEARCH, C::TOGGLEQAMARKER})
        CHECK_FALSE_MESSAGE(needs(c), static_cast<int>(c));
    CHECK_FALSE(key_command_needs_control(kChatKeyFirst)); // the chat keys (479..)
    CHECK_FALSE(key_command_needs_control(499));
    CHECK_FALSE(key_command_needs_control(-1));
}

TEST_CASE("option commands: CanUseCommandNow refuses while the interface is locked") {
    mq_test::World w("hailstone_safelock_test");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    while (c.poll_chat()) {}
    A::safelock(c) = "pw";
    REQUIRE(c.interface_locked());
    c.camp_command(""); // Cmd_camp: CanUseCommandNow first
    auto e = c.poll_chat();
    REQUIRE(e);
    CHECK(e->text == c.strings().format(kStrInterfaceLocked, std::span<const std::string>{}));
    CHECK(e->color == static_cast<std::uint32_t>(kLockedColor));
    CHECK_FALSE(c.poll_chat());
    A::safelock(c).clear();
}

TEST_CASE("option commands: /charinfo") {
    auto name = [](std::uint32_t id) { return id == 1 ? std::string("Qeynos") : std::string("Freeport"); };
    std::uint32_t one[] = {1, 0, 0, 0, 1};
    CHECK(charinfo_line(one, name) == "You are currently bound in: Qeynos, Qeynos"); // bind 3 (unset) differs
    std::uint32_t same[] = {1, 1, 0, 0, 0};
    CHECK(charinfo_line(same, name) == "You are currently bound in: Qeynos");
    std::uint32_t later[] = {0, 2, 0, 0, 0};
    CHECK(charinfo_line(later, name) == "You are currently bound in: , Freeport");
}

TEST_CASE("option commands: /log") {
    auto c = log_command("on", "off", "on", false);
    CHECK(c.on);
    CHECK(c.name == "eqlog");
    c = log_command("on raid", "off", "on", false);
    CHECK(c.name == "raid");
    c = log_command("", "off", "on", true);
    CHECK_FALSE(c.on);
    c = log_command("off", "off", "on", true);
    CHECK_FALSE(c.on);
    CHECK(c.name == "eqlog");
    std::string line = log_line(0, "hello");
    CHECK(line.front() == '[');
    CHECK(line.ends_with("] hello\n"));
    CHECK(line.size() == std::string("[Thu Jan  1 00:00:00 1970] hello\n").size());
}

TEST_CASE("option commands: /shownames") {
    CHECK(shownames_command("", "off").usage);
    CHECK(shownames_command("off", "off").off);
    CHECK(shownames_command("2", "off").level == 2);
    CHECK(shownames_command("2", "off").eqstr == 13295);
    CHECK(shownames_command("9", "off").level == 4); // out of range: 4
    CHECK(shownames_command("x", "off").level == 4);
    CHECK(shownames_command("6", "off").eqstr == 8875);
}

TEST_CASE("option commands: /usercolor") {
    CHECK(usercolor_command("").page == kUserColorUsage);
    CHECK(usercolor_command("help").page == 1);
    CHECK(usercolor_command("PAGE7").page == 7);
    auto c = usercolor_command("3 10 20 30");
    CHECK_FALSE(c.page.has_value());
    CHECK(c.index == 3);
    CHECK(c.red == 10);
    CHECK(c.blue == 30);
    CHECK(usercolor_command("3 10 20").page == kUserColorUsage);   // blue missing
    CHECK(usercolor_command("100 1 2 3").page == kUserColorUsage); // index above 99
    CHECK(usercolor_command("3 256 2 3").page == kUserColorUsage);
    c = usercolor_command("12 reset");
    CHECK(c.reset);
    CHECK(c.index == 12);
    CHECK(usercolor_help_lines(0).size() == 5);
    CHECK(usercolor_help_lines(7).size() == 9);
}

TEST_CASE("option commands: /hotbutton") {
    CHECK(hotbutton_command("").usage);
    auto h = hotbutton_command("Hi 5 /say hello there");
    CHECK(h.name == "Hi");
    CHECK(h.color == 5);
    CHECK(h.line == "/say hello there");
    h = hotbutton_command("Hi /say hello");
    CHECK(h.color == 0);
    CHECK(h.line == "/say hello");
    h = hotbutton_command("Hi");
    CHECK(h.line.empty());
}

TEST_CASE("option commands: /viewport") {
    CHECK_FALSE(viewport_command("", 800, 600));
    CHECK_FALSE(viewport_command("1 2 3", 800, 600));
    auto v = viewport_command("reset", 800, 600);
    REQUIRE(v);
    CHECK(viewport_line(*v) == "x=0 y=0 w=800 h=600 scale=1");
    v = viewport_command("700 10 200 100", 800, 600); // pushed back inside the screen
    REQUIRE(v);
    CHECK(viewport_line(*v) == "x=600 y=10 w=200 h=100 scale=0");
    v = viewport_command("0 0 400 300 1", 800, 600);
    REQUIRE(v);
    CHECK(v->v[6] == 50); // (400 - 0 + 1) * 100 / 800
    CHECK(viewport_ini(*v)[2].second == 400);
}

TEST_CASE("option commands: /particledensity, /mixahead") {
    CHECK(particle_density("2") == 2);
    CHECK(particle_density("high") == 3);
    CHECK(particle_density("Off") == 0);
    CHECK_FALSE(particle_density("4"));
    CHECK_FALSE(particle_density(""));
    CHECK(mixahead_value("8") == 8);
    CHECK_FALSE(mixahead_value("0"));
    CHECK_FALSE(mixahead_value("101"));
    // Cmd_particledensity_Set's values and SyncDisplayPage's choices back.
    CHECK(particle_density_value(0) == 0.0f);
    CHECK(particle_density_value(1) == 0.25f);
    CHECK(particle_density_value(2) == 0.5f);
    CHECK(particle_density_value(3) == 1.0f);
    for (int level = 0; level <= 3; ++level) CHECK(particle_density_choice(particle_density_value(level)) == level);
    CHECK(particle_density_choice(-1.0f) == 0);
    CHECK(particle_density_choice(0.3f) == 2);
    CHECK(particle_density_choice(0.8f) == 3);
}

TEST_CASE("option commands: the shadow clip plane's distance") {
    CHECK(shadow_clip_distance(0) == 10.0f);
    CHECK(shadow_clip_distance(kDefaultShadowClipPlane) == 260.0f);
    CHECK(shadow_clip_distance(100) == 510.0f);
}

TEST_CASE("option commands: the viewport's rectangle (ResetUIForResolution)") {
    auto v = viewport_command("240 120 800 450", 1280, 720);
    REQUIRE(v);
    ViewRect r = viewport_rect(*v, 1280, 720);
    CHECK(r.x == 240);
    CHECK(r.y == 120);
    CHECK(r.w == 800);
    CHECK(r.h == 450);
    // Not scaled: the same pixels on a larger screen; on a smaller one pushed back inside it.
    r = viewport_rect(*v, 2560, 1440);
    CHECK((r.x == 240 && r.w == 800));
    r = viewport_rect(*v, 900, 500);
    CHECK((r.x == 100 && r.w == 800 && r.y == 50 && r.h == 450));
    // Scaled: the percentages of the new screen.
    v = viewport_command("0 0 640 360 1", 1280, 720);
    REQUIRE(v);
    r = viewport_rect(*v, 2560, 1440);
    CHECK((r.x == 0 && r.y == 0 && r.w == 1280 && r.h == 720));
    // Reset: the whole screen, whatever its size.
    v = viewport_command("reset", 1280, 720);
    REQUIRE(v);
    r = viewport_rect(*v, 1920, 1080);
    CHECK((r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080));
    // Kept as text and read back.
    auto back = parse_viewport(viewport_text(*v));
    REQUIRE(back);
    CHECK(back->v == v->v);
    CHECK_FALSE(parse_viewport("1 2 3"));
    CHECK_FALSE(parse_viewport(""));
    // A hand-edited scale: the product wraps in 32 bits before the divide (it overflowed, and MSVC folded it).
    auto edited = parse_viewport("0 0 100 100 0 0 2237014 0");
    REQUIRE(edited);
    r = viewport_rect(*edited, 1920, 1080);
    CHECK(r.w == 995); // 1920 * 2237014 = 0x1_0001_8500, 0x18500 / 100
}

TEST_CASE("option commands: tracking choices and ini values") {
    auto eqstr = [](std::uint32_t id) -> std::string {
        switch (id) {
        case 13156: return "green";
        case 11005: return "gray";
        case 13157: return "blue";
        case 13161: return "lblue";
        case 13120: return "normal";
        case 6353: return "alpha";
        case 13144: return "distance";
        default: return "?";
        }
    };
    CHECK(trackfilter_choice("green", eqstr)->bit == 0x01);
    CHECK(trackfilter_choice("gray", eqstr)->bit == 0x40);
    CHECK(trackfilter_choice("lb", eqstr)->bit == 0x20);
    CHECK(trackfilter_choice("b", eqstr)->bit == 0x02);
    CHECK_FALSE(trackfilter_choice("purple", eqstr));
    CHECK(tracksort_choice("d", eqstr)->type == 2);
    CHECK(tracksort_choice("RC", eqstr)->type == 6);
    CHECK(tracksort_choice("ralpha", eqstr)->type == 4);
    CHECK(std::string(tracksort_choice("a", eqstr)->ini) == "ALPHA");
    CHECK_FALSE(tracksort_choice("x", eqstr));

    CHECK(track_players_value(std::nullopt) == 1);
    CHECK(track_players_value(std::string("GROUP")) == 2);
    CHECK(track_players_value(std::string("FALSE")) == 0);
    CHECK(track_sort_value(std::string("RDISTANCE")) == 5);
    CHECK(track_sort_value(std::string("CONSIDER")) == 3);

    IniFile ini;
    ini.set("Defaults", "TrackPlayers", "GROUP");
    ini.set("Defaults", "TrackPets", "FALSE");
    ini.set("Defaults", "TrackSortType", "RALPHA");
    ini.set("Defaults", "Log", "TRUE");
    ClientOptions opts;
    CHECK(opts.on(ClientOption::TrackMercs));
    CHECK(opts.on(ClientOption::InvWinLabels));
    opts.load(ini);
    CHECK(opts.get(ClientOption::TrackPlayers) == 2);
    CHECK_FALSE(opts.on(ClientOption::TrackPets));
    CHECK(opts.on(ClientOption::TrackMercs)); // missing: TRUE
    CHECK(opts.get(ClientOption::TrackSortType) == 4);
    CHECK(opts.on(ClientOption::Log));
    IniFile out;
    opts.store(ClientOption::TrackPlayers, out);
    opts.store(ClientOption::TrackSortType, out);
    opts.store(ClientOption::TrackPets, out);
    CHECK(out.get("Defaults", "TrackPlayers") == std::optional<std::string>("GROUP"));
    CHECK(out.get("Defaults", "TrackSortType") == std::optional<std::string>("RALPHA"));
    CHECK(out.get("Defaults", "TrackPets") == std::optional<std::string>("FALSE"));
}

TEST_CASE("option commands: /netstats lines") {
    NetStats s;
    s.master = 40;
    s.sent_success = 0.75f;
    auto lines = netstats_lines(s);
    REQUIRE(lines.size() >= 10);
    CHECK(lines[0].text.empty());
    CHECK(lines[2].text == "Ping Times");
    CHECK(lines[2].color == 0xf);
    CHECK(lines[3].text == "   master=40");
    bool found = false;
    for (const auto& l : lines) found = found || l.text == "   sent=25.00%";
    CHECK(found);
}

TEST_CASE("option commands: /hotbutton's social store (0x4ea9f0)") {
    HotButtonStore store;
    store.social(0).name = "Taken";
    store.social(0).lines[1] = "kept";
    CHECK(store_social(store, "Hi", "/say hi", 5, -1, true) == 1); // the first empty one
    CHECK(store.social(1).name == "Hi");
    CHECK(store.social(1).color == 5);
    CHECK(store_social(store, "taken", "/say again", 2, -1, true) == 0); // the same name (any case) is replaced
    CHECK(store.social(0).name == "taken");
    CHECK(store.social(0).lines[0] == "/say again");
    CHECK(store.social(0).lines[1] == "kept"); // the other lines stay
    CHECK(store_social(store, "AVeryLongSocialName", "x", 0, 200, false) == 2); // start out of range: 0
    CHECK(store.social(2).name == "AVeryLongSocial");
}

TEST_CASE("local time: platform::local_time and utc_time break a time down; ctime_text and task_ctime_text format it") {
    // Noon UTC on 2026-10-04 is that date in every time zone (UTC-12 to UTC+14).
    const std::time_t t = 1791115200;
    auto tm = hailstone::platform::local_time(t);
    REQUIRE(tm);
    CHECK(tm->tm_year == 126);
    CHECK(tm->tm_mon == 9);
    CHECK(tm->tm_mday == 4);
    std::tm copy = *tm;
    CHECK(std::mktime(&copy) == t);
    auto utc = hailstone::platform::utc_time(t);
    REQUIRE(utc);
    CHECK(utc->tm_year == 126);
    CHECK(utc->tm_mon == 9);
    CHECK(utc->tm_mday == 4);
    CHECK(utc->tm_hour == 12);
    CHECK(utc->tm_min == 0);
    const std::string line = hailstone::game::ctime_text(t);
    CHECK(line.starts_with("Sun Oct  4 "));
    CHECK(line.ends_with(" 2026"));
    const std::string task = hailstone::game::task_ctime_text(static_cast<std::uint32_t>(t));
    CHECK(task.starts_with("Sun Oct 04 "));
    CHECK(task.ends_with(" 2026"));
}

TEST_CASE("option commands: the chat log keeps its file open, each line on disk at once, another name another file") {
    // OutputTextToLog: one open per log, not per line (a raid's hundreds of lines a second); a log parser reading
    // the file still sees each line as it is written.
    mq_test::World w("hailstone_chat_log_test");
    using A = net::GameConnectionTestAccess;
    net::GameConnection& c = *w.conn;
    auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    A::ticket(c).server_name = "Test Server"; // the character's folder needs a server
    A::client_options(c).set(ClientOption::Log, 1);
    A::open_chat_log(c, "raid");
    REQUIRE(c.chat_logging());
    const std::filesystem::path raid = c.chat_log_path();
    A::chat(c, "first line");
    CHECK(read(raid).ends_with("] first line\n"));
    std::FILE* const stream = A::chat_log_stream(c);
    REQUIRE(stream != nullptr);
    CHECK_FALSE(platform::stream_inherited_by_children(stream)); // the browser helper does not hold it open
    A::chat(c, "second line");
    CHECK(A::chat_log_stream(c) == stream); // the same open file, not one per line
    std::string text = read(raid);
    CHECK(text.find("] first line\n") != std::string::npos);
    CHECK(text.ends_with("] second line\n"));

    // A write that failed (a full disk, a file pulled away) leaves the file in error: the next line opens it anew.
    A::fail_chat_log(c);
    REQUIRE(std::ferror(A::chat_log_stream(c)));
    A::chat(c, "after a failure");
    REQUIRE(A::chat_log_stream(c) != nullptr);
    CHECK_FALSE(std::ferror(A::chat_log_stream(c)));
    CHECK(read(raid).ends_with("] after a failure\n"));

    A::open_chat_log(c, "other");
    const std::filesystem::path other = c.chat_log_path();
    REQUIRE(other != raid);
    A::chat(c, "third line");
    CHECK(read(other).ends_with("] third line\n"));
    CHECK(read(raid).find("third") == std::string::npos);

    // Off: nothing more is written, and the file is closed (it can be moved or deleted); on again it is opened anew.
    A::log_command(c, "off"); // (Log is on: "off", or a toggle where the words are not known, turns it off)
    REQUIRE_FALSE(c.chat_logging());
    CHECK(A::chat_log_stream(c) == nullptr);
    A::chat(c, "not logged");
    CHECK(read(other).find("not logged") == std::string::npos);
    std::error_code ec;
    std::filesystem::remove(other, ec);
    CHECK_FALSE(ec);
    A::client_options(c).set(ClientOption::Log, 1);
    A::open_chat_log(c, "other");
    A::chat(c, "fourth line");
    CHECK(read(other).ends_with("] fourth line\n"));
}
