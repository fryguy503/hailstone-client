// The Python tools beside the client: server/install.py (installs the server script into an EQEmu server's
// eqemu_config.json) and tools/check_core_names.py (CI's check that the client's sources name no custom server).
// Each is run with python3 -I in a scratch folder; without python3 (or on Windows) the cases check nothing, but a
// script missing from the repository fails them.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "scratch_dir.h"

#if !defined(_WIN32)

namespace fs = std::filesystem;

namespace {

fs::path repo_root() {
    return fs::path(__FILE__).parent_path().parent_path();
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string quote(const fs::path& p) {
    std::string s = "'";
    for (char c : p.string()) s += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return s + "'";
}

bool have_python() {
    return std::system("python3 -I -c '' >/dev/null 2>&1") == 0;
}

// Runs python3 -I `script` `args` and gives its exit code; its output goes to `out`.
int run_python(const fs::path& script, const std::string& args, const fs::path& out) {
    const fs::path code = out.string() + ".code";
    const std::string cmd = "python3 -I " + quote(script) + " " + args + " >" + quote(out) + " 2>&1; echo $? >" +
                            quote(code);
    if (std::system(cmd.c_str()) == -1) return -1;
    return std::atoi(read_all(code).c_str());
}

} // namespace

TEST_CASE("install.py keeps eqemu_config.json's mode: the database password stays private") {
    // The scripts are in the repository: a path that misses them is a broken test, not a skip.
    REQUIRE(fs::exists(repo_root() / "server" / "install.py"));
    if (!have_python()) return;
    hailstone::test::ScratchDir dir("install_py");
    const fs::path config = dir.path() / "eqemu_config.json";
    std::ofstream(config) << R"({"server": {"database": {"password": "x"}, "directories": {"quests": "quests/"}}})";
    fs::permissions(config, fs::perms::owner_read | fs::perms::owner_write);
    const fs::path script = repo_root() / "server" / "install.py";
    REQUIRE(run_python(script, quote(dir.path()), dir.path() / "out") == 0);
    CHECK(read_all(config).find("hailstone/quests/") != std::string::npos);
    CHECK(fs::exists(dir.path() / "hailstone" / "quests" / "global" / "script_init.lua"));
    CHECK((fs::status(config).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK_FALSE(fs::exists(config.string() + ".tmp"));

    fs::permissions(config, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    REQUIRE(run_python(script, quote(dir.path()) + " --remove", dir.path() / "out") == 0);
    CHECK(read_all(config).find("hailstone/quests/") == std::string::npos);
    CHECK_FALSE(fs::exists(dir.path() / "hailstone"));
    CHECK((fs::status(config).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
}

TEST_CASE("check_core_names.py: a name with NMS then any C, and NMS in a Lua long string, are reported") {
    REQUIRE(fs::exists(repo_root() / "tools" / "check_core_names.py"));
    if (!have_python()) return;
    hailstone::test::ScratchDir dir("core_names");
    const fs::path root = dir.path() / "repo";
    fs::create_directories(root / "src");
    fs::create_directories(root / "ui");
    const fs::path script = repo_root() / "tools" / "check_core_names.py";
    // The old names in their own spellings pass.
    std::ofstream(root / "src" / "ok.h") << "namespace nmsc::game { struct NmscHook; int NMSC_OP_X; }\n"
                                            "#include \"nmsc_plugin.h\"\n"
                                            "const char* old = \"nms-client\"; // NMSChat in a comment\n";
    std::ofstream(root / "ui" / "ok.lua") << "local x = nmsc\n--[[ NMSChannel in a comment ]]\nlocal s = [[ plain ]]\n";
    CHECK(run_python(script, quote(root), dir.path() / "out") == 0);
    // Server names that start with NMS and a C, and text in long strings, do not.
    std::ofstream(root / "src" / "bad.h") << "constexpr int kNmsCombatOp = 1;\nint nmsCursor;\nint NmsClientRules;\n"
                                             "int nmscombat;\n";
    std::ofstream(root / "ui" / "bad.lua") << "local NMSChannel = 7\nlocal t = [[ -- nms_rules ]]\n"
                                              "local u = [==[\nNMS ]] here\n]==]\n";
    CHECK(run_python(script, quote(root), dir.path() / "out") == 1);
    const std::string out = read_all(dir.path() / "out");
    for (const char* line : {"src/bad.h:1:", "src/bad.h:2:", "src/bad.h:3:", "src/bad.h:4:", "ui/bad.lua:1:",
                             "ui/bad.lua:2:", "ui/bad.lua:4:"})
        CHECK_MESSAGE(out.find(line) != std::string::npos, line);
    CHECK(out.find("ok.h") == std::string::npos);
    CHECK(out.find("ok.lua") == std::string::npos);
}

TEST_CASE("check_core_names.py: raw strings are code whatever they hold, and ui ini and Objective-C++ are scanned") {
    if (!have_python()) return;
    hailstone::test::ScratchDir dir("core_names_raw");
    const fs::path root = dir.path() / "repo";
    fs::create_directories(root / "src");
    fs::create_directories(root / "ui" / "pack");
    const fs::path script = repo_root() / "tools" / "check_core_names.py";
    // A raw string's "//" and quotes are text, and a comment after it still goes.
    std::ofstream(root / "src" / "ok.cpp") << "auto a = R\"x(\"q\" )\" text)x\"; // NMS\n"
                                              "/* nms */ int FOOR = 0;\n";
    std::ofstream(root / "ui" / "pack" / "ui.ini") << "; NMS in a comment\nname=Modern\n";
    CHECK(run_python(script, quote(root), dir.path() / "out") == 0);
    std::ofstream(root / "src" / "bad.cpp") << "const char* s = R\"(\n// not a comment\nnms_login\n)\";\n"
                                               "auto t = u8R\"d(\"//\" NMS)d\";\n";
    std::ofstream(root / "ui" / "pack" / "bad.ini") << "# fine\nname=NMS pack\n";
    std::ofstream(root / "src" / "bad.mm") << "auto s = @\"nms\";\n";
    CHECK(run_python(script, quote(root), dir.path() / "out") == 1);
    const std::string out = read_all(dir.path() / "out");
    for (const char* line : {"src/bad.cpp:3:", "src/bad.cpp:5:", "ui/pack/bad.ini:2:", "src/bad.mm:1:"})
        CHECK_MESSAGE(out.find(line) != std::string::npos, line);
    CHECK(out.find("ok.cpp") == std::string::npos);
    CHECK(out.find("ui.ini") == std::string::npos);
}

#endif
