// MacroQuest's .cfg files (app/autoexec.h): the lookup, the lines, and when Automation runs them.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/autoexec.h"
#include "app/automation.h"
#include "core/path_utf8.h"
#include "mq/commands.h"
#include "mq/lua_mq.h"
#include "mq_test_world.h"
#include "net/server_files.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// The same file. On a file system that ignores case (Windows) the found path keeps the asked-for spelling.
bool same_file(const std::optional<fs::path>& found, const fs::path& expected) {
    std::error_code ec;
    return found && fs::equivalent(*found, expected, ec);
}

app::CfgFolders folders_in(const fs::path& root) {
    app::CfgFolders f;
    f.data = root;
    f.global = root / "cfg";
    f.server = root / "servers" / "p" / "cfg";
    f.character = root / "servers" / "p" / "characters" / "w" / "Tester" / "cfg";
    return f;
}

} // namespace

TEST_CASE("cfg files: MacroQuest's name, and the lines a file runs") {
    CHECK(app::cfg_file_name("InGame") == "InGame.cfg");
    CHECK(app::cfg_file_name("MQ2Nav-AutoExec") == "MQ2Nav-AutoExec.cfg");
    CHECK(app::cfg_file_name("notes.txt") == "notes.txt"); // a name with a '.' is taken as it is
    // Line ends of either kind, a byte order mark, comments (';' MacroQuest's, '#' Hailstone's), blank and indented lines.
    const auto lines = app::cfg_lines("\xEF\xBB\xBF/echo one\r\n; a comment\n\n   \n# another\n  /echo two\r\n/echo three");
    CHECK(lines == std::vector<std::string>{"/echo one", "/echo two", "/echo three"});
    CHECK(app::cfg_lines("").empty());
    CHECK(app::cfg_lines("\r\n\r\n").empty());
    CHECK(app::cfg_lines("/echo a ; not a comment") == std::vector<std::string>{"/echo a ; not a comment"});
}

TEST_CASE("cfg files: only the most specific layer's file runs, autoexec/ first, in any case") {
    test::ScratchDir scratch("cfg_layers");
    const auto f = folders_in(scratch.path());
    CHECK_FALSE(app::find_cfg(f, "InGame"));
    write(f.global / "InGame.cfg", "/echo global\n");
    CHECK(same_file(app::find_cfg(f, "InGame"), f.global / "InGame.cfg"));
    CHECK(same_file(app::find_cfg(f, "ingame"), f.global / "InGame.cfg")); // any case, as on Windows
    write(f.global / "Autoexec" / "INGAME.CFG", "/echo global autoexec\n");
    CHECK(same_file(app::find_cfg(f, "InGame"), f.global / "Autoexec" / "INGAME.CFG"));
    write(f.server / "ingame.cfg", "/echo server\n");
    CHECK(same_file(app::find_cfg(f, "InGame"), f.server / "ingame.cfg"));
    write(f.character / "InGame.cfg", "/echo character\n");
    CHECK(same_file(app::find_cfg(f, "InGame"), f.character / "InGame.cfg"));
    // A layer that is not known is not looked in.
    auto no_character = f;
    no_character.character.clear();
    CHECK(same_file(app::find_cfg(no_character, "InGame"), f.server / "ingame.cfg"));
    // A name that would leave the folder is no name.
    for (const char* bad : {"", ".", "..", "../InGame", "sub/InGame", "sub\\InGame", "C:InGame", "a\nb"}) {
        CAPTURE(bad);
        CHECK_FALSE(app::find_cfg(f, bad));
    }
    // A folder of that name is not a file.
    fs::create_directories(f.global / "zoned.cfg");
    CHECK_FALSE(app::find_cfg(f, "zoned"));
}

TEST_CASE("cfg files: a /loadcfg path is the player's only, a .cfg file in a cfg folder; a server's download never "
          "lands in cfg/") {
    test::ScratchDir scratch("cfg_paths");
    const auto f = folders_in(scratch.path());
    write(f.global / "extra" / "fights.cfg", "/echo fights\n");
    write(f.server / "srv.cfg", "/echo server\n");
    write(f.character / "mine.cfg", "/echo mine\n");
    CHECK(app::resolve_cfg(f, "cfg/extra/fights", true) == fs::weakly_canonical(f.global / "extra" / "fights.cfg"));
    CHECK(app::resolve_cfg(f, "cfg/extra/fights.cfg", true).has_value());
    CHECK(app::resolve_cfg(f, "servers/p/cfg/srv", true).has_value());
    CHECK(app::resolve_cfg(f, path_utf8(f.character / "mine.cfg"), true).has_value()); // absolute, inside
    // Not typed: a script or plugin names a file, never a path, even one inside a cfg folder.
    CHECK_FALSE(app::resolve_cfg(f, "cfg/extra/fights", false));
    CHECK(app::resolve_cfg(f, "fights", false) == std::nullopt); // (a name: the layers' own files only)
    // Elsewhere in the data folder: a plugin's settings, the chat history, a server's assets.
    write(scratch.path() / "plugin-config" / "x.ini", "auth_key = secret\n");
    write(scratch.path() / "servers" / "p" / "assets" / "evil.cfg", "/echo no\n");
    write(scratch.path() / "extra" / "fights.cfg", "/echo no\n");
    CHECK_FALSE(app::resolve_cfg(f, "plugin-config/x.ini", true));
    CHECK_FALSE(app::resolve_cfg(f, "servers/p/assets/evil", true));
    CHECK_FALSE(app::resolve_cfg(f, "extra/fights", true));
    CHECK_FALSE(app::resolve_cfg(f, "cfg/../extra/fights", true));
    // Inside a cfg folder, but not a .cfg file.
    write(f.global / "notes.txt", "/echo no\n");
    CHECK_FALSE(app::resolve_cfg(f, "cfg/notes.txt", true));
    write(scratch.path().parent_path() / (path_utf8(scratch.path().filename()) + "-outside") / "evil.cfg", "/echo no\n");
    CHECK_FALSE(app::resolve_cfg(f, "../" + path_utf8(scratch.path().filename()) + "-outside/evil", true));
    CHECK_FALSE(app::resolve_cfg(f, "cfg/extra/missing", true));
    write(f.global / "InGame.cfg", "/echo global\n");
    CHECK(app::resolve_cfg(f, "InGame", false) == f.global / "InGame.cfg"); // a plain name: the layers
    fs::remove_all(scratch.path().parent_path() / (path_utf8(scratch.path().filename()) + "-outside"));
    // load_cfg says why a script's path is refused.
    const auto refused = app::load_cfg(f, "cfg/extra/fights", false);
    REQUIRE(refused);
    CHECK(refused->lines.empty());
    CHECK_FALSE(refused->refused.empty());
    const auto typed = app::load_cfg(f, "cfg/extra/fights", true);
    REQUIRE(typed);
    CHECK(typed->refused.empty());
    CHECK(typed->lines == std::vector<std::string>{"/echo fights"});
    // Whatever path a server lists, its file goes to assets/, scripts/, plugins/ or hailstone.cfg: never a cfg/ folder.
    CHECK(net::server_file_destination("cfg/InGame.cfg") == "assets/cfg/InGame.cfg");
    CHECK(net::server_file_destination("characters/w/Tester/cfg/zoned.cfg") == "assets/characters/w/Tester/cfg/zoned.cfg");
}

namespace {

// Automation as the client builds it, its hooks given to the connection, so the cfg lines' /echo come back as chat.
struct CfgHost {
    std::unique_ptr<app::Automation> automation;
    std::optional<mq_test::World> world;
    fs::path root;
    explicit CfgHost(const fs::path& scratch) : root(scratch) {
        app::AutomationEnv env;
        env.data_dir = root / "data";
        env.install_dir = root / "install";
        env.connection = [this] { return world ? world->conn.get() : nullptr; };
        env.server_name = [] { return std::string("TestServer"); };
        env.server_dir = [this] { return root / "data" / "servers" / "p"; };
        env.character_dir = [this] { return root / "data" / "servers" / "p" / "characters" / "w" / "Tester"; };
        automation = std::make_unique<app::Automation>(std::move(env));
        net::ConnectionOptions options;
        options.hooks = &automation->hooks();
        world.emplace("hailstone_autoexec_test", options);
        net::GameConnectionTestAccess::set_server_rules(*world->conn, plugin::Rules{}); // no hailstone.cfg: free
    }
    ~CfgHost() { automation->shutdown(); }
    fs::path global() const { return root / "data" / "cfg"; }
    fs::path character() const { return root / "data" / "servers" / "p" / "characters" / "w" / "Tester" / "cfg"; }
    std::vector<std::string> frame() {
        automation->frame();
        std::vector<std::string> out;
        while (auto e = world->conn->poll_chat()) out.push_back(e->text);
        return out;
    }
};

using Lines = std::vector<std::string>;

} // namespace

TEST_CASE("cfg files: AutoExec once, CharSelect each time, InGame and the rest at world entry, zoned at each zone-in") {
    test::ScratchDir scratch("cfg_events");
    CfgHost h(scratch.path());
    for (const char* name : {"AutoExec", "CharSelect", "InGame", "TestServer_Tester", "Cleric", "zoned", "qeynos2"})
        write(h.global() / app::cfg_file_name(name), std::string("/echo ") + name + "\n");
    // The character's own InGame.cfg is the one that runs, not the global one as well.
    write(h.character() / "InGame.cfg", "; the character's\n/echo InGame (Tester)\n");
    plugin::Hooks& hooks = h.automation->hooks();
    h.frame(); // (nothing yet)

    hooks.game_state(plugin::kGameStateCharSelect);
    CHECK(h.frame() == Lines{"AutoExec", "CharSelect"});
    // The first zone-in comes before the game state says "in game": its files wait for the world entry's.
    hooks.game_state(plugin::kGameStateLoggingIn);
    hooks.zone(plugin::ZonePhase::End);
    CHECK(h.frame().empty());
    hooks.game_state(plugin::kGameStateInGame);
    // MacroQuest's order: the zone's own file at once, zoned.cfg a pulse later.
    CHECK(h.frame() == Lines{"InGame (Tester)", "TestServer_Tester", "Cleric", "qeynos2"});
    CHECK(h.frame() == Lines{"zoned"});
    CHECK(h.frame().empty());

    // Zoning while in game: the zone's and zoned, not InGame again.
    hooks.zone(plugin::ZonePhase::Begin);
    hooks.zone(plugin::ZonePhase::End);
    CHECK(h.frame() == Lines{"qeynos2"});
    CHECK(h.frame() == Lines{"zoned"});

    // Back to character select: CharSelect, no second AutoExec; the next entry runs InGame again.
    hooks.game_state(plugin::kGameStateCharSelect);
    CHECK(h.frame() == Lines{"CharSelect"});
    hooks.game_state(plugin::kGameStateInGame);
    CHECK(h.frame() == Lines{"InGame (Tester)", "TestServer_Tester", "Cleric"});
}

TEST_CASE("cfg files: /loadcfg runs a file's lines at once, parsed, and says when there is none") {
    test::ScratchDir scratch("cfg_loadcfg");
    CfgHost h(scratch.path());
    write(h.global() / "buffs.cfg", "/echo ${Me.Name} buffs\n/loadcfg more\n");
    write(h.character() / "more.cfg", "/echo more\n");
    h.world->conn->send_command("/loadcfg buffs");
    CHECK(h.frame() == Lines{"Tester buffs", "more"});
    h.world->conn->send_command("/loadcfg nope");
    CHECK(h.frame() == Lines{"Could not /loadcfg 'nope'"});
    h.world->conn->send_command("/loadcfg");
    CHECK(h.frame() == Lines{"Usage: /loadcfg <filename>"});
    // A file that loads itself is not run inside itself.
    write(h.global() / "loop.cfg", "/echo loop\n/loadcfg loop\n");
    h.world->conn->send_command("/loadcfg loop");
    const auto said = h.frame();
    REQUIRE(said.size() == 2);
    CHECK(said[0] == "loop");
    CHECK(said[1].find("loop.cfg is already running") != std::string::npos);
    // Two files that load each other: each once.
    write(h.global() / "ping.cfg", "/echo ping\n/loadcfg pong\n");
    write(h.global() / "pong.cfg", "/echo pong\n/loadcfg ping\n");
    h.world->conn->send_command("/loadcfg ping");
    const auto both = h.frame();
    REQUIRE(both.size() == 3);
    CHECK(both[0] == "ping");
    CHECK(both[1] == "pong");
    CHECK(both[2].find("ping.cfg is already running") != std::string::npos);
}

TEST_CASE("cfg files: one /loadcfg runs at most kMaxCfgLines lines in all, its nested files' included") {
    test::ScratchDir scratch("cfg_budget");
    CfgHost h(scratch.path());
    // Twenty lines that each /loadcfg a file of a hundred: 2020 lines without a budget.
    std::string hundred, twenty;
    for (int i = 0; i < 100; ++i) hundred += "/echo line\n";
    for (int i = 0; i < 20; ++i) twenty += "/loadcfg hundred\n";
    write(h.global() / "hundred.cfg", hundred);
    write(h.global() / "twenty.cfg", twenty);
    // The lines that run, counted as they reach the command filters (the chat queue keeps only the last 500).
    int echoes = 0;
    plugin::Hooks& hooks = h.automation->hooks();
    hooks.on_command_line(hooks.new_owner("count"), [&echoes](std::string& line) {
        if (line == "/echo line") ++echoes;
        return plugin::Flow::Continue;
    });
    h.world->conn->send_command("/loadcfg twenty");
    const auto said = h.frame();
    REQUIRE_FALSE(said.empty());
    // 1000 lines run: the ten /loadcfg lines and 990 echoes; then one line says the budget ended.
    CHECK(echoes == mq::kMaxCfgLines - 10);
    CHECK(said.back().find("stopped after 1000") != std::string::npos);
    CHECK(std::count_if(said.begin(), said.end(), [](const std::string& l) {
              return l.find("stopped after") != std::string::npos;
          }) == 1);
    // The next /loadcfg has a budget of its own.
    echoes = 0;
    h.world->conn->send_command("/loadcfg hundred");
    CHECK(h.frame().size() == 100);
    CHECK(echoes == 100);
    // The files due at one moment share one budget too.
    echoes = 0;
    write(h.global() / "AutoExec.cfg", twenty);
    hooks.game_state(plugin::kGameStateCharSelect);
    const auto batch = h.frame();
    CHECK(echoes == mq::kMaxCfgLines - 10);
    CHECK(batch.back().find("stopped after 1000") != std::string::npos);
}

TEST_CASE("cfg files: a script's /loadcfg takes a name, not a path; the player's path must be a cfg folder's; big "
          "files are refused") {
    test::ScratchDir scratch("cfg_callers");
    CfgHost h(scratch.path());
    write(h.global() / "ok.cfg", "/echo ok\n");
    write(h.root / "data" / "plugin-config" / "x.ini", "/echo secret\nsecret words\n");
    {
        plugin::CallerScope scope(plugin::kCapAll, "lua:probe");
        h.world->conn->send_command("/loadcfg cfg/ok");
        const auto said = h.frame();
        REQUIRE(said.size() == 1);
        CHECK(said[0].find("by its name only") != std::string::npos);
        h.world->conn->send_command("/loadcfg ok");
        CHECK(h.frame() == Lines{"ok"});
    }
    h.world->conn->send_command("/loadcfg plugin-config/x.ini");
    CHECK(h.frame() == Lines{"Could not /loadcfg 'plugin-config/x.ini'"});
    h.world->conn->send_command("/loadcfg cfg/ok");
    CHECK(h.frame() == Lines{"ok"});
    // Larger than kMaxCfgBytes: refused with a chat line, by name or path.
    write(h.global() / "big.cfg", std::string(app::kMaxCfgBytes + 1, '#'));
    h.world->conn->send_command("/loadcfg big");
    auto said = h.frame();
    REQUIRE(said.size() == 1);
    CHECK(said[0].find("larger than 64 KB") != std::string::npos);
    h.world->conn->send_command("/loadcfg cfg/big.cfg");
    said = h.frame();
    REQUIRE(said.size() == 1);
    CHECK(said[0].find("larger than 64 KB") != std::string::npos);
}

TEST_CASE("cfg files: a script's start runs its <name>-AutoExec.cfg") {
    test::ScratchDir scratch("cfg_script_autoexec");
    CfgHost h(scratch.path());
    write(h.root / "data" / "scripts" / "hello.lua", "local mq = require('mq')\nmq.delay(100000)\n");
    write(h.global() / "hello-AutoExec.cfg", "/echo hello's cfg\n");
    h.frame();
    REQUIRE(h.automation->scripts().run("hello", {}));
    const auto said = h.frame();
    CHECK(std::find(said.begin(), said.end(), "hello's cfg") != said.end());
}
