// The Hailstone folder layout: platform/paths.h (the data folder and its parts), core/server_profile.h (the saved
// servers), core/game_dir.h (the asset folder with a server's overlay) and core/asset_folder.h (the picker's check).
#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/asset_folder.h"
#include "core/old_ui_settings.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/server_profile.h"
#include "game_files.h"
#include "platform/paths.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {
// A fresh folder under the system temp folder, removed again at the end of the test.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (std::string("hailstone_layout_") + tag + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string text(const std::optional<std::vector<std::byte>>& bytes) {
    if (!bytes) return "<missing>";
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}
} // namespace

TEST_CASE("Layout: the data folder and the named parts under it") {
    TempDir tmp("paths");
    platform::set_data_dir_for_tests(tmp.path);
    CHECK(platform::data_dir() == tmp.path);
    CHECK(platform::config_file() == tmp.path / "hailstone.ini");
    CHECK(platform::imgui_ini() == tmp.path / "hailstone-windows.ini");
    CHECK(platform::ui_settings_dir() == tmp.path / "ui" / "settings");
    CHECK(platform::ui_settings_file("classic") == tmp.path / "ui" / "settings" / "classic.txt");
    CHECK(platform::plugins_dir() == tmp.path / "plugins");
    CHECK(platform::scripts_dir() == tmp.path / "scripts");
    CHECK(platform::scripts_config_dir() == tmp.path / "scripts" / "config");
    CHECK(platform::textures_dir() == tmp.path / "textures");
    CHECK(platform::servers_dir() == tmp.path / "servers");
    CHECK(platform::logs_dir() == tmp.path / "logs");
    CHECK(platform::log_file() == tmp.path / "logs" / "hailstone.log");
    CHECK(platform::screenshots_dir() == tmp.path / "screenshots");
    CHECK(platform::cache_dir() == tmp.path / "cache");
    CHECK(platform::plugin_cache_dir() == tmp.path / "cache" / "plugins");
    CHECK_FALSE(fs::exists(tmp.path / "servers")); // nothing is created
    // the shipped parts stay beside the binary
    CHECK(platform::shaders_dir() == platform::install_dir() / "shaders");
    CHECK(platform::ui_packs_dir() == platform::install_dir() / "ui");

    CHECK(platform::is_writable_dir(tmp.path));
    CHECK_FALSE(platform::is_writable_dir(tmp.path / "nowhere"));
    CHECK(fs::is_empty(tmp.path)); // the probe is gone

    platform::set_data_dir_for_tests({});
    CHECK(platform::data_dir() != tmp.path);
    CHECK_FALSE(platform::data_dir().empty());
}

TEST_CASE("Layout: the first start makes the data folder's layout") {
    test::ScratchDir tmp("layout_dirs");
    const fs::path root = tmp.path() / "hailstone"; // a data folder that does not exist yet
    platform::set_data_dir_for_tests(root);
    CHECK(platform::create_data_dirs().empty());
    for (const char* dir : {"ui/settings", "plugins", "scripts", "scripts/config", "textures", "textures/default",
                            "maps", "servers", "logs", "screenshots", "bug-reports", "cache"})
        CHECK_MESSAGE(fs::is_directory(root / dir), dir);
    CHECK_FALSE(fs::exists(root / "hailstone.ini")); // folders only

    // A second start keeps what is there.
    write(root / "plugins" / "mine.txt", "kept");
    CHECK(platform::create_data_dirs().empty());
    CHECK(fs::is_regular_file(root / "plugins" / "mine.txt"));

    // A file where a folder belongs is reported, not replaced.
    fs::remove_all(root / "maps");
    write(root / "maps", "not a folder");
    auto failed = platform::create_data_dirs();
    REQUIRE(failed.size() == 1);
    CHECK(failed[0] == root / "maps");
    CHECK(fs::is_regular_file(root / "maps"));

    platform::set_data_dir_for_tests({});
}

TEST_CASE("Layout: the data folder is the Hailstone folder, or a problem to report") {
    TempDir tmp("choose");
    const fs::path bundle_data = tmp.path / "bundle-data";
    auto choose = [&](const fs::path& install) { return platform::choose_data_dir(install, bundle_data); };

    // A fresh writable folder: portable, with nothing left of the write test.
    const fs::path fresh = tmp.path / "fresh";
    fs::create_directories(fresh);
    CHECK(choose(fresh).dir == fresh);
    CHECK(choose(fresh).reason == "portable");
    CHECK(choose(fresh).problem.empty());
    CHECK_FALSE(fs::exists(fresh / ".hailstone-write-test"));

    // Settings in a per-user folder never pull the choice there: it is always the Hailstone folder.
    write(bundle_data / "hailstone.ini", "layout.version=1\n");
    CHECK(choose(fresh).dir == fresh);

    // Unpacked into an EverQuest folder (the asset folder, never written): a problem, without even the write test's
    // probe file.
    const fs::path hs = tmp.path / "game-assets";
    write(hs / "eqgame.exe", "exe");
    CHECK(choose(hs).dir == hs);
    CHECK(choose(hs).problem.find("EverQuest folder") != std::string::npos);
    CHECK_FALSE(fs::exists(hs / ".hailstone-write-test"));
    const fs::path ui2 = tmp.path / "RoF2b";
    write(ui2 / "Spells_US.txt", "spells");
    CHECK_FALSE(choose(ui2).problem.empty());

    // A folder that cannot be written: a problem that names it (where the system lets a test take the permission away;
    // an administrator can write anywhere).
    const fs::path locked = tmp.path / "locked";
    fs::create_directories(locked);
    std::error_code ec;
    fs::permissions(locked, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    if (!platform::is_writable_dir(locked)) {
        CHECK(choose(locked).dir == locked);
        CHECK(choose(locked).problem.find("cannot write") != std::string::npos);
    }
    fs::permissions(locked, fs::perms::owner_all, fs::perm_options::add, ec);

    // A macOS app bundle: the one case outside the folder (the bundle cannot hold the player's files).
    const fs::path bundle = tmp.path / "Hailstone.app" / "Contents" / "Resources";
    fs::create_directories(bundle);
    CHECK(choose(bundle).dir == bundle_data);
    CHECK(choose(bundle).reason.find("app bundle") != std::string::npos);
    CHECK(choose(bundle).problem.empty());
}

TEST_CASE("Layout: the Vulkan driver shipped beside the binary is MoltenVK on macOS only") {
    test::ScratchDir tmp("vulkan_lib");
    // No file: the system's Vulkan, everywhere.
    CHECK(platform::bundled_vulkan_library(tmp.path()).empty());
    write(tmp.path() / "libMoltenVK.dylib", "dylib");
#if defined(__APPLE__)
    CHECK(platform::bundled_vulkan_library(tmp.path()) == tmp.path() / "libMoltenVK.dylib");
    // A folder by that name is not a library.
    test::ScratchDir dir_named("vulkan_dir");
    fs::create_directories(dir_named.path() / "libMoltenVK.dylib");
    CHECK(platform::bundled_vulkan_library(dir_named.path()).empty());
#else
    // Elsewhere the graphics driver brings Vulkan, and a stray file changes nothing.
    CHECK(platform::bundled_vulkan_library(tmp.path()).empty());
#endif
}

TEST_CASE("Layout: file names and profile ids made safe") {
    CHECK(sanitize_file_name("Soandso") == "Soandso");
    CHECK(sanitize_file_name("a/b\\c:d*e") == "a_b_c_d_e");
    CHECK(sanitize_file_name("  .hidden. ") == "hidden");
    CHECK(sanitize_file_name("..") == "_");
    CHECK(sanitize_file_name("") == "_");
    CHECK(sanitize_file_name("CON") == "_CON");
    CHECK(sanitize_file_name("com1.txt") == "_com1.txt");
    CHECK(sanitize_file_name("console") == "console");

    TempDir tmp("ids");
    fs::path servers = tmp.path / "servers";
    CHECK(make_profile_id("My Server!", servers) == "my-server"); // no servers folder yet
    CHECK(make_profile_id("  ***  ", servers) == "server");
    CHECK(make_profile_id("Nul", servers) == "_nul");
    fs::create_directories(servers / "My-Server");
    fs::create_directories(servers / "my-server-2");
    CHECK(make_profile_id("my server", servers) == "my-server-3"); // taken in any case
}

TEST_CASE("Layout: the window title names the server") {
    CHECK(client_window_title("") == "Hailstone");
    CHECK(client_window_title("", "Bob") == "Hailstone - Bob");
    CHECK(client_window_title("NMS Test") == "NMS Test");
    CHECK(client_window_title("NMS Test", "Nmscanimc [Clr]") == "NMS Test - Nmscanimc [Clr]");
}

TEST_CASE("Layout: server profiles saved, listed and found") {
    TempDir tmp("profiles");
    fs::path servers = tmp.path / "servers";

    ServerProfile p;
    p.id = make_profile_id("Test Server", servers);
    p.name = "Test Server";
    p.host = "192.0.2.10";
    p.port = 5998;
    p.username = "player";
    REQUIRE(save_profile(p, servers));
    CHECK(fs::is_directory(profile_assets_dir(p.id, servers)));
    CHECK(profile_ini(p.id, servers) == servers / "test-server" / "server.ini");
    CHECK(profile_maps_dir(p.id, servers) == servers / "test-server" / "maps");
    CHECK(character_dir(p.id, "Test World", "Soandso", servers) ==
          servers / "test-server" / "characters" / "Test World" / "Soandso");
    CHECK(character_dir(p.id, "../w", "../x", servers) == servers / "test-server" / "characters" / "_w" / "_x");
    CHECK(world_folder_name("NMS: Dev/Test") == "NMS_ Dev_Test");
    CHECK(world_folder_name("Nul") == "_Nul");

    auto back = load_profile(p.id, servers);
    REQUIRE(back);
    CHECK(back->name == "Test Server");
    CHECK(back->host == "192.0.2.10");
    CHECK(back->port == 5998);
    CHECK(back->username == "player");
    CHECK(back->remember);

    // keys it does not know survive a save (an older build's [NMS], which the NMS plugin reads); not remembering
    // drops the username
    {
        std::ofstream(profile_ini(p.id, servers), std::ios::app) << "[Extra]\nkept=1\n[NMS]\nauth_key=secret\n";
    }
    p.remember = false;
    REQUIRE(save_profile(p, servers));
    IniFile ini;
    REQUIRE(ini.load(profile_ini(p.id, servers)));
    CHECK(ini.get_string("Extra", "kept") == "1");
    CHECK(ini.get_string("NMS", "auth_key") == "secret");
    CHECK_FALSE(ini.get("Server", "username"));

    // a bare server.ini: the id is the name, the defaults fill the rest
    write(servers / "alpha" / "server.ini", "[Server]\nhost=a.example\nmodule=bogus\n");
    fs::create_directories(servers / "not-a-profile");
    ServerProfile empty_id;
    CHECK_FALSE(save_profile(empty_id, servers));

    auto all = list_profiles(servers);
    REQUIRE(all.size() == 2);
    CHECK(all[0].id == "alpha");
    CHECK(all[0].name == "alpha");
    CHECK(all[0].port == 5997);
    CHECK(all[1].id == "test-server");
    CHECK(find_profile(all, "A.EXAMPLE", 5997) == &all[0]);
    CHECK(find_profile(all, "a.example", 5998) == nullptr);
    CHECK(find_profile(all, "Test-Server") == &all[1]);

    // worlds.ini: what the client keeps about each world, beside server.ini.
    CHECK(profile_worlds_ini(p.id, servers) == servers / "test-server" / "worlds.ini");
    // A transient profile's folder gets worlds.ini but no server.ini, so it is not listed.
    REQUIRE(set_listed_worlds("_transient-x", {"W"}, servers));
    CHECK(fs::exists(profile_worlds_ini("_transient-x", servers)));

    CHECK_FALSE(remove_profile("../servers", servers));
    CHECK(remove_profile("alpha", servers));
    CHECK_FALSE(fs::exists(servers / "alpha"));
    CHECK(list_profiles(servers).size() == 1);
    // Kept in servers/.deleted/<id>-<date>-<time>/, whole; a second deletion of the same id gets its own folder.
    std::vector<fs::path> deleted;
    for (const auto& e : fs::directory_iterator(deleted_profiles_dir(servers))) deleted.push_back(e.path());
    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0].filename().string().starts_with("alpha-"));
    CHECK(fs::exists(deleted[0] / "server.ini"));
    write(servers / "alpha" / "server.ini", "[Server]\nhost=b.example\n");
    CHECK(remove_profile("alpha", servers));
    CHECK(std::distance(fs::directory_iterator(deleted_profiles_dir(servers)), fs::directory_iterator()) == 2);
    CHECK_FALSE(remove_profile("alpha", servers)); // nothing left to delete
    CHECK(list_profiles(servers).size() == 1);
    CHECK(list_profiles(tmp.path / "nowhere").empty());
}

TEST_CASE("Layout: a world or character name from the server cannot write lines of its own into server.ini") {
    hailstone::test::ScratchDir tmp("profile-inject");
    const fs::path servers = tmp.path() / "servers";
    ServerProfile p;
    p.id = "inject";
    p.name = "Inject";
    p.host = "login.example";
    p.port = 5999;
    SavedAccount a;
    a.username = "player";
    // A world listed as "X\n[Server]\nhost=evil.example\n;" and a character name with '=' and a line break.
    a.characters.push_back({"Bob", "X\n[Server]\nhost=evil.example\n;", 50, "WAR"});
    a.characters.push_back({"Al=ice\r\n[Account:x]", "W", 10, ""});
    p.accounts.push_back(a);
    REQUIRE(save_profile(p, servers));

    auto back = load_profile(p.id, servers);
    REQUIRE(back);
    CHECK(back->host == "login.example");
    CHECK(back->port == 5999);
    REQUIRE(back->accounts.size() == 1);
    CHECK(back->accounts[0].username == "player");
    REQUIRE(back->accounts[0].characters.size() == 2);
    CHECK(back->accounts[0].characters[0].name == "Bob");
    CHECK(back->accounts[0].characters[0].world == "X_[Server]_host_evil.example_;");
    CHECK(back->accounts[0].characters[0].level == 50);
    CHECK(back->accounts[0].characters[1].name == "Al_ice__[Account:x]");
    CHECK(back->accounts[0].characters[1].level == 10);
    IniFile ini;
    REQUIRE(ini.load(profile_ini(p.id, servers)));
    CHECK(ini.sections() == std::vector<std::string>{"Server", "Account:player"});
}

TEST_CASE("IniFile: keys and section names stay on their own line") {
    IniFile ini;
    ini.set("A]\n[B", "k\n[C]\nx", "v");
    ini.set("S", "a=b", "1");
    ini.set("S", ";not a comment", "2");
    ini.set("S", "[not a section", "3");
    // the same names find the cleaned entries again
    CHECK(ini.get_string("A]\n[B", "k\n[C]\nx") == "v");
    CHECK(ini.get_string("S", "a=b") == "1");
    IniFile back;
    back.parse(ini.to_string());
    CHECK(back.sections() == std::vector<std::string>{"A__[B", "S"});
    CHECK(back.keys("A__[B") == std::vector<std::string>{"k_[C]_x"});
    CHECK(back.get_string("S", "a_b") == "1");
    CHECK(back.get_string("S", "_not a comment") == "2");
    CHECK(back.get_string("S", "_not a section") == "3");
    back.erase("S", "a=b");
    CHECK_FALSE(back.get("S", "a_b"));
    back.erase_section("A]\n[B");
    CHECK(back.sections() == std::vector<std::string>{"S"});
}

// parse() trims the names and each line's ends; set() and get() trim names the same way, so a name set with spaces
// around it is found before and after a save and a load. A value's trailing spaces are written as given and read
// back trimmed, as GetPrivateProfileString reads them.
TEST_CASE("IniFile: names with spaces around them read the same after a reload") {
    IniFile ini;
    ini.set(" Sect\t", "  key ", "  two words  ");
    ini.set("Sect", ";k ", "x");
    CHECK(ini.get_string("Sect", "key") == "  two words  ");
    CHECK(ini.get_string(" Sect ", " key\t") == "  two words  ");
    CHECK(ini.get_string("Sect", "_k") == "x");
    CHECK(ini.to_string() == "[Sect]\nkey=  two words  \n_k=x\n");
    IniFile back;
    back.parse(ini.to_string());
    CHECK(back.sections() == std::vector<std::string>{"Sect"});
    CHECK(back.keys("Sect") == std::vector<std::string>{"key", "_k"});
    CHECK(back.get_string(" Sect", "key ") == "  two words");
    CHECK(back.get_string("Sect", "_k") == "x");
    back.erase(" Sect ", " key ");
    CHECK_FALSE(back.get("Sect", "key"));
}

TEST_CASE("Layout: a server's overlay over the asset folder") {
    TempDir tmp("gamedir");
    fs::path base = tmp.path / "base";
    fs::path over = tmp.path / "over";
    write(base / "spells_us.txt", "base spells");
    write(base / "Resources" / "BaseData.txt", "base data");
    write(base / "snd1.pfs", "s1");
    write(base / "uifiles" / "default" / "EQUI_Animations.xml", "anims");
    write(base / "uifiles" / "default" / "EQUI_Inventory.xml", "base inv");
    write(base / "maps" / "Brewall" / "qeynos_1.txt", "x");
    write(over / "SPELLS_US.TXT", "server spells");
    write(over / "newzone.s3d", "new zone");
    write(over / "uifiles" / "default" / "NMS_Extra.xml", "extra");
    write(over / "uifiles" / "default" / "eqUI_inventory.xml", "server inv");
    write(over / "Maps" / "brewall" / "qeynos_2.txt", "y");

    GameDir game(base);
    CHECK(game.root() == base);
    CHECK(game.base_root() == base);
    CHECK_FALSE(game.overlay());
    CHECK(game.roots() == std::vector<fs::path>{base});
    CHECK(text(game.read("spells_us.txt")) == "base spells");
    CHECK_FALSE(game.exists("newzone.s3d"));

    const GameDir& held = game; // what the holders keep
    game.set_overlay(over);
    CHECK(held.root() == base); // root() stays the base
    CHECK(held.roots() == std::vector<fs::path>{over, base});
    CHECK(text(held.read("Spells_Us.txt")) == "server spells"); // whole-file replacement
    CHECK(held.from_overlay("spells_us.txt"));
    CHECK(text(read_file(*held.find_base("spells_us.txt"))) == "base spells");
    CHECK(text(held.read("newzone.s3d")) == "new zone"); // only in the overlay
    CHECK(text(held.read("resources\\basedata.txt")) == "base data"); // falls through
    CHECK_FALSE(held.from_overlay("Resources/BaseData.txt"));

    auto ui = held.list("uifiles/default/");
    REQUIRE(ui.size() == 3);
    CHECK(ui[0].name == "EQUI_Animations.xml");
    CHECK(ui[1].name == "eqUI_inventory.xml"); // one entry per name, the overlay's
    CHECK(text(read_file(ui[1].path)) == "server inv");
    CHECK(ui[1].relative == "uifiles/default/eqUI_inventory.xml");
    CHECK(ui[2].name == "NMS_Extra.xml");
    auto top = held.list("");
    CHECK(top.size() == 3); // newzone.s3d, snd1.pfs, spells_us.txt
    CHECK(held.list("\\UIFILES\\Default").size() == 3);
    CHECK(held.list("nowhere").empty());
    CHECK(held.list_dirs("maps") == std::vector<std::string>{"brewall"}); // the overlay's spelling
    CHECK(held.list_dirs("") == std::vector<std::string>{"Maps", "Resources", "uifiles"});
    CHECK(held.list("maps/BREWALL").size() == 2);

    // an overlay filter ([assets] player = off): the overlay files it rejects fall through to the base, or vanish
    game.set_overlay_filter([](std::string_view name) { return name == "spells_us.txt"; });
    CHECK(text(held.read("spells_us.txt")) == "server spells");
    CHECK_FALSE(held.exists("newzone.s3d"));
    CHECK(text(held.read("uifiles/default/EQUI_Inventory.xml")) == "base inv");
    game.set_overlay_filter({});
    CHECK(held.exists("newzone.s3d"));

    // a file added later is seen after a rescan
    write(over / "late.txt", "late");
    CHECK_FALSE(held.exists("late.txt"));
    game.rescan();
    CHECK(held.exists("late.txt"));

    // clearing the overlay goes back to the base alone
    game.set_overlay(std::nullopt);
    CHECK(text(held.read("spells_us.txt")) == "base spells");
    CHECK_FALSE(held.exists("newzone.s3d"));

    // an overlay folder that is not there yet is an empty layer
    game.set_overlay(tmp.path / "later");
    CHECK(held.roots().size() == 2);
    CHECK(text(held.read("spells_us.txt")) == "base spells");

    // copies keep their own layers
    GameDir copy = game;
    game.set_overlay(over);
    CHECK(text(copy.read("spells_us.txt")) == "base spells");
    CHECK(text(game.read("spells_us.txt")) == "server spells");
    GameDir both(base, over);
    CHECK(both.from_overlay("newzone.s3d"));

    CHECK_THROWS(GameDir(tmp.path / "missing"));
}

TEST_CASE("Layout: an empty GameDir for the asset-folder picker") {
    TempDir tmp("picker");
    write(tmp.path / "eqstr_us.txt", "x");
    GameDir game;
    CHECK_FALSE(game.has_base());
    CHECK(game.root().empty());
    CHECK(game.roots().empty());
    CHECK_FALSE(game.exists("eqstr_us.txt"));
    CHECK(game.list("").empty());
    CHECK_THROWS(game.set_base(tmp.path / "missing"));
    CHECK_FALSE(game.has_base());
    game.set_base(tmp.path);
    CHECK(game.has_base());
    CHECK(game.root() == tmp.path);
    CHECK(game.exists("EQSTR_US.TXT"));
}

TEST_CASE("Layout: the asset-folder check") {
    TempDir tmp("assets");
    fs::path dir = tmp.path / "rof2";

    auto none = validate_asset_folder(dir);
    CHECK_FALSE(none.ok);
    CHECK_FALSE(none.is_directory);
    CHECK(describe(none) == "not a folder");

    fs::create_directories(dir);
    auto bare = validate_asset_folder(dir);
    CHECK_FALSE(bare.ok);
    CHECK(bare.is_directory);
    CHECK(bare.missing.size() == asset_folder_required_files().size() + 1); // + snd*.pfs
    CHECK(bare.missing.back() == "snd*.pfs");
    CHECK(bare.missing_rof2.size() == asset_folder_rof2_files().size());
    CHECK(bare.warnings == std::vector<std::string>{"eqgame.exe"});

    // the required files, in another case than the list's
    for (const auto& f : asset_folder_required_files()) {
        std::string upper = f;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        write(dir / upper, "x");
    }
    write(dir / "Snd17.PFS", "x");
    auto older = validate_asset_folder(dir);
    CHECK(older.missing.empty());
    CHECK_FALSE(older.ok);
    CHECK(describe(older).starts_with("not RoF2 or a current EverQuest install (missing Resources/OnDemandResources.txt"));

    for (const auto& f : asset_folder_rof2_files()) write(dir / f, "x");
    auto good = validate_asset_folder(dir);
    CHECK(good.ok);
    CHECK(describe(good).empty());
    CHECK(good.warnings.size() == 1); // no eqgame.exe, still usable
    write(dir / "eqgame.exe", "x");
    CHECK(validate_asset_folder(dir).warnings.empty());

    // through a GameDir: a file the overlay adds counts for the merged view
    fs::remove(dir / "SPELLS_US.TXT");
    CHECK(validate_asset_folder(dir).missing == std::vector<std::string>{"spells_us.txt"});
    GameDir game(dir);
    CHECK_FALSE(validate_asset_folder(game).ok);
    write(tmp.path / "over" / "spells_us.txt", "x");
    game.set_overlay(tmp.path / "over");
    CHECK(validate_asset_folder(game).ok);
    CHECK_FALSE(validate_asset_folder(GameDir{}).is_directory);
}

TEST_CASE("Layout: the asset-folder check passes on the real install") {
    // CI sets EQ_GAME_DIR to empty: no game files there, so nothing to check (as tests/game_files.h does).
    const auto* game = hailstone::test::game_dir();
    if (!game) return;
    auto check = validate_asset_folder(*game);
    INFO(describe(check));
    CHECK(check.ok);
}

TEST_CASE("Layout: a classic folder under the asset folder") {
    TempDir tmp("classic");
    fs::path base = tmp.path / "live", classic = tmp.path / "eql", over = tmp.path / "over";
    write(base / "spells_us_str.txt", "live marker");
    write(base / "everfrost.s3d", "live everfrost");
    write(base / "nektulos.s3d", "live nektulos");
    write(base / "qeynos.s3d", "live qeynos");
    write(classic / "commons.s3d", "eql commons");
    write(classic / "EVERFROST.s3d", "eql everfrost");
    write(classic / "nektulos.s3d", "eql nektulos");
    write(classic / "qeynos.s3d", "eql qeynos");
    write(over / "commons.s3d", "server commons");

    GameDir game(base);
    game.set_classic(classic, classic_preferred_file);
    CHECK(text(game.read("commons.s3d")) == "eql commons");     // only in the classic folder: filled in
    CHECK(text(game.read("everfrost.s3d")) == "eql everfrost"); // preferred over the live base
    CHECK(text(game.read("nektulos.s3d")) == "live nektulos");  // the base's, not preferred
    CHECK(text(game.read("qeynos.s3d")) == "live qeynos");
    CHECK(game.from_classic("commons.s3d"));
    CHECK_FALSE(game.from_classic("qeynos.s3d"));
    CHECK(game.roots() == std::vector<fs::path>{base, classic});

    game.set_overlay(over); // the server's own files still come first
    CHECK(text(game.read("commons.s3d")) == "server commons");
    CHECK(game.roots() == std::vector<fs::path>{over, base, classic});

    GameDir plain(base);
    plain.set_classic(classic); // no rule: only fills in
    CHECK(text(plain.read("everfrost.s3d")) == "live everfrost");
    CHECK(text(plain.read("commons.s3d")) == "eql commons");
    plain.set_classic(std::nullopt);
    CHECK_FALSE(plain.exists("commons.s3d"));

    GameDir opened = open_game_dir(base, classic); // a live base: the rule applies
    CHECK(text(opened.read("everfrost.s3d")) == "eql everfrost");

    ClassicFolderCheck check = check_classic_folder(classic);
    CHECK(check.ok);
    CHECK(check.has == std::vector<std::string>{"commons.s3d", "everfrost.s3d"});
    CHECK_FALSE(check_classic_folder(base / "nowhere").ok);
    CHECK(classic_preferred_file("lavastorm.emt"));
    CHECK(classic_preferred_file("everfrost_obj.s3d"));
    CHECK_FALSE(classic_preferred_file("everfrostx.s3d"));
    CHECK_FALSE(classic_preferred_file("nektulos.s3d"));
    CHECK_FALSE(classic_preferred_file("maps/everfrost.txt"));
}

// The UI settings move from ui-settings/ to ui/settings/ at start-up: a file already there wins, the old folder goes
// once empty, and a second start has nothing to do.
TEST_CASE("Layout: ui-settings/ moves under ui/") {
    TempDir tmp("ui_settings_move");
    platform::set_data_dir_for_tests(tmp.path);
    write(tmp.path / "ui-settings" / "modern.txt", "palette=Ember\n");
    write(tmp.path / "ui-settings" / "gamepad.txt", "old gamepad\n");
    write(tmp.path / "ui" / "settings" / "gamepad.txt", "new gamepad\n");
    CHECK(move_old_ui_settings() == 1);
    CHECK(read(tmp.path / "ui" / "settings" / "modern.txt") == "palette=Ember\n");
    CHECK(read(tmp.path / "ui" / "settings" / "gamepad.txt") == "new gamepad\n");
    CHECK(read(tmp.path / "ui-settings" / "gamepad.txt") == "old gamepad\n"); // left, not overwritten

    fs::remove(tmp.path / "ui-settings" / "gamepad.txt");
    CHECK(move_old_ui_settings() == 0);
    CHECK_FALSE(fs::exists(tmp.path / "ui-settings"));
    CHECK(move_old_ui_settings() == 0);
    platform::set_data_dir_for_tests({});
}

// The client's windows were "##nmsc..." up to 0.6: their saved places are renamed to "##hailstone...", only in the
// [Window] sections, and only in a file no newer client has written.
TEST_CASE("Layout: old window names in hailstone-windows.ini are renamed") {
    const std::string old_ini = "[Window][Inventory##nmsc_inv]\nPos=10,20\nSize=300,400\n\n"
                                "[Window][Bag##nmsc_bag3]\nPos=1,2\n\n"
                                "[Window][Debug##Default]\nPos=60,60\n\n"
                                "[Table][0x1234]\nColumn 0  Width=40 ##nmsc\n";
    const std::string want = "[Window][Inventory##hailstone_inv]\nPos=10,20\nSize=300,400\n\n"
                             "[Window][Bag##hailstone_bag3]\nPos=1,2\n\n"
                             "[Window][Debug##Default]\nPos=60,60\n\n"
                             "[Table][0x1234]\nColumn 0  Width=40 ##nmsc\n";
    CHECK(rename_old_window_ids(old_ini) == want);
    CHECK(rename_old_window_ids(want) == want);
    const std::string both = old_ini + "[Window][Inventory##hailstone_inv]\nPos=5,5\n";
    CHECK(rename_old_window_ids(both) == both); // a newer client wrote it: left as it is
    CHECK(rename_old_window_ids("[Window][A##nmsc][Window]##nmsc") == "[Window][A##hailstone][Window]##hailstone");

    TempDir tmp("window_ids_rename");
    write(tmp.path / "hailstone-windows.ini", old_ini);
    CHECK(rename_old_window_ids_in(tmp.path / "hailstone-windows.ini"));
    CHECK(read(tmp.path / "hailstone-windows.ini") == want);
    CHECK_FALSE(rename_old_window_ids_in(tmp.path / "hailstone-windows.ini"));
    CHECK_FALSE(rename_old_window_ids_in(tmp.path / "missing.ini"));
}
