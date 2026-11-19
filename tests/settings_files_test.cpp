// Writing the settings files and the log (core/atomic_file.h, core/config.h, core/log.h): whole files only, two
// clients sharing one hailstone.ini, and the earlier runs' logs kept.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/atomic_file.h"
#include "core/config.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (std::string("hailstone_settings_") + tag + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read(const fs::path& file) { return read_text_file(file).value_or("<missing>"); }

// The folder's entries, to see that no temporary file is left.
int entries(const fs::path& dir) {
    return static_cast<int>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}
} // namespace

TEST_CASE("Settings files: a number setting that is NaN or an infinity reads as its default") {
    Config c;
    c.parse("a = 1.5\nb = 2x\nc = nan\nd = inf\ne = -INFINITY\nf =\ng = word\nh = 1e39\n");
    CHECK(c.get_float("a", 9.0f) == 1.5f);
    CHECK(c.get_float("b", 9.0f) == 2.0f); // the number the value starts with
    CHECK(c.get_float("c", 9.0f) == 9.0f);
    CHECK(c.get_float("d", 9.0f) == 9.0f);
    CHECK(c.get_float("e", 9.0f) == 9.0f);
    CHECK(c.get_float("f", 9.0f) == 9.0f);
    CHECK(c.get_float("g", 9.0f) == 9.0f);
    CHECK(c.get_float("h", 9.0f) == 9.0f); // past float's range: strtof's HUGE_VALF
    CHECK(c.get_float("missing", 9.0f) == 9.0f);
}

TEST_CASE("Settings files: written whole, through a temporary file") {
    TempDir tmp("atomic");
    const fs::path file = tmp.path / "sub" / "a.ini";
    REQUIRE(write_file_atomic(file, "one\n"));
    CHECK(read(file) == "one\n");
    REQUIRE(write_file_atomic(file, "two\n"));
    CHECK(read(file) == "two\n");
    CHECK(entries(file.parent_path()) == 1);
    // A folder in the way: nothing written, the old file as it was.
    fs::create_directories(tmp.path / "dir.ini");
    CHECK_FALSE(write_file_atomic(tmp.path / "dir.ini", "x"));
    CHECK(fs::is_directory(tmp.path / "dir.ini"));

    IniFile ini;
    ini.set("S", "k", "v");
    REQUIRE(ini.save(file));
    CHECK(read(file) == "[S]\nk=v\n");
    CHECK(entries(file.parent_path()) == 1);
}

TEST_CASE("Settings files: two clients with one hailstone.ini keep each other's changes") {
    TempDir tmp("merge");
    const fs::path file = tmp.path / "hailstone.ini";
    {
        Config first;
        first.set("ui.name", "classic");
        first.set("assets.path", "/hs");
        first.set("keys.jump", "space");
        REQUIRE(first.save(file));
    }
    Config a, b;
    REQUIRE(a.load(file));
    REQUIRE(b.load(file));
    // A changes the asset folder and saves; B, started before, changes a key binding and the server.
    a.set("assets.path", "/rof2");
    REQUIRE(a.save(file));
    b.set("keys.jump", "j");
    b.set("login.profile", "nms");
    b.set("ui.name", "classic"); // the same value: not a change
    b.erase("missing.key");
    REQUIRE(b.save(file));
    Config disk;
    REQUIRE(disk.load(file));
    CHECK(disk.get_string("assets.path") == "/rof2"); // A's change is not undone
    CHECK(disk.get_string("keys.jump") == "j");
    CHECK(disk.get_string("login.profile") == "nms");
    CHECK(disk.get_string("ui.name") == "classic");
    // B now holds the merged keys, and erasing works through a merge too.
    CHECK(b.get_string("assets.path") == "/rof2");
    a.erase("keys.jump"); // A never saw "j": its erase still goes over it
    a.set("audio.volume", "5");
    REQUIRE(a.save(file));
    REQUIRE(disk.load(file));
    CHECK_FALSE(disk.get("keys.jump"));
    CHECK(disk.get_string("login.profile") == "nms");
    CHECK(disk.get_string("audio.volume") == "5");

    // One program alone writes everything it holds, as always: an erase removes the key.
    Config alone;
    REQUIRE(alone.load(file));
    alone.erase("audio.volume");
    REQUIRE(alone.save(file));
    REQUIRE(disk.load(file));
    CHECK_FALSE(disk.get("audio.volume"));
    // A Config never read from the file replaces it.
    Config fresh;
    fresh.set("only", "1");
    REQUIRE(fresh.save(file));
    REQUIRE(disk.load(file));
    CHECK(disk.entries().size() == 1);
}

TEST_CASE("Settings files: a character's settings over hailstone.ini") {
    test::ScratchDir tmp("character_settings");
    const fs::path shared_file = tmp.path() / "hailstone.ini";
    const fs::path one = tmp.path() / "servers" / "s" / "characters" / "w" / "One" / "hailstone.ini";
    const fs::path two = tmp.path() / "servers" / "s" / "characters" / "w" / "Two" / "hailstone.ini";
    Config c;
    c.set("display.render_scale", "100");
    c.set("assets.path", "/hs");
    c.set("window.width", "1280");
    c.set("gamepad.bind.a", "jump");
    REQUIRE(c.save(shared_file));

    // The first entry: the character's file starts as a copy of the settings that are not shared.
    REQUIRE(c.attach_character(one));
    CHECK(c.character_path() == one);
    REQUIRE(fs::exists(one));
    Config disk;
    REQUIRE(disk.load(one));
    CHECK(disk.get_string("display.render_scale") == "100");
    CHECK(disk.get_string("gamepad.bind.a") == "jump");
    CHECK_FALSE(disk.get("assets.path"));
    CHECK_FALSE(disk.get("window.width"));

    // While it plays, its settings change only its file; the shared keys still go to hailstone.ini.
    c.set("display.render_scale", "50");
    c.erase("gamepad.bind.a");
    c.set("assets.path", "/rof2");
    c.set("window.width", "800");
    REQUIRE(c.save(shared_file));
    CHECK(c.get_string("display.render_scale") == "50");
    CHECK_FALSE(c.get("gamepad.bind.a"));
    REQUIRE(disk.load(one));
    CHECK(disk.get_string("display.render_scale") == "50");
    CHECK_FALSE(disk.get("gamepad.bind.a"));
    CHECK_FALSE(disk.get("assets.path"));
    REQUIRE(disk.load(shared_file));
    CHECK(disk.get_string("display.render_scale") == "100");
    CHECK(disk.get_string("gamepad.bind.a") == "jump");
    CHECK(disk.get_string("assets.path") == "/rof2");
    CHECK(disk.get_string("window.width") == "800");
    // entries() is what get() sees: the shared keys and the character's.
    std::vector<std::string> keys;
    for (const auto& e : c.entries()) keys.push_back(e.first);
    CHECK(keys == std::vector<std::string>{"assets.path", "window.width", "display.render_scale"});

    // Back to the login: hailstone.ini's settings again.
    c.detach_character();
    CHECK(c.character_path().empty());
    CHECK(c.get_string("display.render_scale") == "100");
    CHECK(c.get_string("gamepad.bind.a") == "jump");

    // A second character starts from the shared settings, not the first one's.
    c.set("display.render_scale", "75");
    REQUIRE(c.save(shared_file));
    REQUIRE(c.attach_character(two));
    CHECK(c.get_string("display.render_scale") == "75");
    // The first one keeps its own when it comes back; a later change to the shared ones does not reach it.
    REQUIRE(c.attach_character(one));
    CHECK(c.get_string("display.render_scale") == "50");
    CHECK_FALSE(c.get("gamepad.bind.a"));

    // A save with only the character's keys changed leaves hailstone.ini alone (another client's changes stay).
    Config other;
    REQUIRE(other.load(shared_file));
    other.set("update.skip", "1.0");
    REQUIRE(other.save(shared_file));
    c.set("display.render_scale", "60");
    REQUIRE(c.save(shared_file));
    REQUIRE(disk.load(shared_file));
    CHECK(disk.get_string("update.skip") == "1.0");
    REQUIRE(disk.load(one));
    CHECK(disk.get_string("display.render_scale") == "60");
}

TEST_CASE("Settings files: a command-line flag's value holds for the run and is never saved") {
    test::ScratchDir tmp("run_override");
    const fs::path shared_file = tmp.path() / "hailstone.ini";
    const fs::path one = tmp.path() / "servers" / "s" / "characters" / "w" / "One" / "hailstone.ini";
    Config c;
    c.set("display.lighting", "modern");
    c.set("display.msaa", "4");
    c.set("display.upscaler", "off");
    REQUIRE(c.save(shared_file));

    // --lighting classic, --msaa 2, --upscaler taa: read over hailstone.ini's.
    c.set_run_override("display.lighting", "classic");
    c.set_run_override("display.msaa", "2");
    c.set_run_override("display.upscaler", "taa");
    CHECK(c.get_string("display.lighting") == "classic");
    CHECK(c.get_int("display.msaa", 0) == 2);
    REQUIRE(c.load(shared_file)); // the file read again keeps them
    CHECK(c.get_int("display.msaa", 0) == 2);

    // Over a character's settings too, the shared keys and the character's alike.
    REQUIRE(c.attach_character(one));
    CHECK(c.get_string("display.lighting") == "classic");
    CHECK(c.get_string("display.upscaler") == "taa");

    // Never written: not to hailstone.ini, nor to the character's file, nor when another key's change is saved.
    c.set("display.bloom", "0.3");
    REQUIRE(c.save(shared_file));
    Config disk;
    REQUIRE(disk.load(shared_file));
    CHECK(disk.get_string("display.lighting") == "modern");
    CHECK(disk.get_string("display.msaa") == "4");
    CHECK(disk.get_string("display.upscaler") == "off");
    REQUIRE(disk.load(one));
    CHECK(disk.get_string("display.lighting") == "modern");
    CHECK(disk.get_string("display.bloom") == "0.3");

    // Another program's change to hailstone.ini merged in on save leaves the flag's value out too.
    Config other;
    REQUIRE(other.load(shared_file));
    other.set("update.skip", "1.0");
    REQUIRE(other.save(shared_file));
    c.set("display.msaa", "8"); // the player's own change in the Options window: it takes over, and is saved
    CHECK(c.get_int("display.msaa", 0) == 8);
    REQUIRE(c.save(shared_file));
    REQUIRE(disk.load(shared_file));
    CHECK(disk.get_string("display.msaa") == "8");
    CHECK(disk.get_string("display.lighting") == "modern");
    CHECK(disk.get_string("update.skip") == "1.0");
    CHECK(c.get_string("display.lighting") == "classic"); // the others still hold
    c.erase("display.lighting");
    CHECK_FALSE(c.get("display.lighting"));
}

TEST_CASE("Settings files: the keys a character's settings leave in hailstone.ini") {
    for (const char* key : {"assets.path", "addons.index", "update.skip", "login.profile", "login.last_world.x",
                            "window.x", "display.window_mode", "display.resolution", "display.msaa",
                            "display.upscaler", "ui.name",
                            "ui.scale", "audio.pregame_music", "plugins.autoload", "scripts.autoload", "news.last_read"})
        CHECK_MESSAGE(Config::is_shared_key(key), key);
    for (const char* key : {"display.render_scale", "display.shadows", "display.max_fps", "display.clip_plane",
                            "display.find_trail_style",
                            "audio.master", "mouse.sensitivity", "KEYMAPPING_JUMP_1", "gamepad.deadzone",
                            "combat.auto_target_xtarget", "ui.font_face", "game.fast_camp"})
        CHECK_MESSAGE(!Config::is_shared_key(key), key);
}

TEST_CASE("Settings files: Find trail choice is restored independently for each character") {
    test::ScratchDir tmp("find_trail_settings");
    const auto shared = tmp.path() / "hailstone.ini";
    const auto one = tmp.path() / "One" / "hailstone.ini";
    const auto two = tmp.path() / "Two" / "hailstone.ini";
    Config config;
    REQUIRE(config.save(shared));
    REQUIRE(config.attach_character(one));
    CHECK(config.get_int("display.find_trail_style", 0) == 0);
    config.set_int("display.find_trail_style", 1);
    REQUIRE(config.save(shared));
    REQUIRE(config.attach_character(two));
    CHECK(config.get_int("display.find_trail_style", 0) == 0);
    REQUIRE(config.attach_character(one));
    CHECK(config.get_int("display.find_trail_style", 0) == 1);
    config.detach_character();
    CHECK(config.get_int("display.find_trail_style", 0) == 0);
}

TEST_CASE("Settings files: the log keeps the earlier runs' files") {
    TempDir tmp("log");
    const fs::path file = tmp.path / "logs" / "test.log";
    for (int run = 1; run <= 5; ++run) {
        REQUIRE(log::open_file(file));
        CHECK(log::file_path() == file); // this process holds the lock: the file itself, every time
        log::info("run {}", run);
        log::close_file();
    }
    CHECK(read(file).find("run 5") != std::string::npos);
    CHECK(read(tmp.path / "logs" / "test.1.log").find("run 4") != std::string::npos);
    CHECK(read(tmp.path / "logs" / "test.2.log").find("run 3") != std::string::npos);
    CHECK(read(tmp.path / "logs" / "test.3.log").find("run 2") != std::string::npos);
    CHECK_FALSE(fs::exists(tmp.path / "logs" / "test.4.log"));
}

TEST_CASE("Settings files: a second client's old logs go, the newest and the recent ones stay") {
    TempDir tmp("log_pids");
    const fs::path logs = tmp.path / "logs";
    fs::create_directories(logs);
    const auto now = fs::file_time_type::clock::now();
    // Eight old ones (days ago, the newest first) and one written an hour ago; the earlier runs' files and others
    // of other names are not second clients' logs.
    for (int i = 0; i < 8; ++i) {
        const fs::path f = logs / ("test." + std::to_string(1000 + i) + ".log");
        std::ofstream(f) << "old";
        fs::last_write_time(f, now - std::chrono::hours(24 * (2 + i)));
    }
    std::ofstream(logs / "test.2000.log") << "recent";
    fs::last_write_time(logs / "test.2000.log", now - std::chrono::hours(1));
    for (const char* other : {"test.1.log", "test.2.log", "test.3.log", "viewer.1234.log", "test.x1.log"}) {
        std::ofstream(logs / other) << "other";
        fs::last_write_time(logs / other, now - std::chrono::hours(24 * 30));
    }
    REQUIRE(log::open_file(logs / "test.log"));
    log::close_file();
    // The newest five: the recent one and the four newest old ones.
    CHECK(fs::exists(logs / "test.2000.log"));
    for (int i = 0; i < 8; ++i)
        CHECK_MESSAGE(fs::exists(logs / ("test." + std::to_string(1000 + i) + ".log")) == (i < 4), i);
    for (const char* other : {"test.2.log", "test.3.log", "viewer.1234.log", "test.x1.log"})
        CHECK_MESSAGE(fs::exists(logs / other), other);
}

TEST_CASE("Settings files: the log file stops at its limit with a line saying so") {
    TempDir tmp("log_limit");
    const fs::path file = tmp.path / "logs" / "test.log";
    log::set_file_limit(300);
    REQUIRE(log::open_file(file));
    for (int i = 0; i < 100; ++i) log::info("line {} of a run that logs a lot", i);
    log::close_file();
    log::set_file_limit(0);
    const std::string text = read(file);
    CHECK(text.size() < 500);
    CHECK(text.find("line 0 ") != std::string::npos);
    CHECK(text.find("line 99 ") == std::string::npos);
    CHECK(text.find("reached its limit of 300 bytes") != std::string::npos);
    // The next open starts a new file with the limit back to the default.
    REQUIRE(log::open_file(file));
    for (int i = 0; i < 100; ++i) log::info("line {} again", i);
    log::close_file();
    CHECK(read(file).find("line 99 again") != std::string::npos);
}

TEST_CASE("Settings files: a shared key a character's file still holds moves to hailstone.ini when it has none") {
    test::ScratchDir tmp("shared_key_moves");
    const auto one = tmp.path() / "One" / "hailstone.ini";
    {
        Config old; // a character's file from before display.upscaler was shared
        old.set("display.upscaler", "fsr3");
        old.set("display.render_scale", "67");
        std::filesystem::create_directories(one.parent_path());
        REQUIRE(old.save(one));
    }
    Config config;
    REQUIRE(config.attach_character(one));
    CHECK(config.get_string("display.upscaler") == "fsr3");
    config.detach_character();
    CHECK(config.get_string("display.upscaler") == "fsr3"); // kept in the shared settings
    CHECK_FALSE(config.get("display.render_scale"));       // a character's own stays its own

    Config set_globally; // hailstone.ini's own value wins over a stale one in the character's file
    set_globally.set("display.upscaler", "taa");
    REQUIRE(set_globally.attach_character(one));
    CHECK(set_globally.get_string("display.upscaler") == "taa");
}
