// Add-ons on disk (app/addon_install.h, docs/addons.md): what a package may hold, the Lua and library checks,
// installed.txt, an install and an update that keep the player's own files, a removal, a cut-off install undone at
// start, the names that are taken, the capability ceilings, and the lists of the server's and hand-installed ones.

#include <doctest/doctest.h>

#include <chrono>

#include "addon_test_util.h"
#include "app/addon_install.h"
#include "app/update_install.h"
#include "plugin/policy.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::app;
using namespace hailstone::addon_test;
namespace fs = std::filesystem;

namespace {

AddonManifest manifest(const std::string& name, AddonKind kind, const std::string& version = "1.0.0") {
    const std::string kind_word = kind == AddonKind::Script ? "script" : "plugin";
    const std::string plat = kind == AddonKind::Script ? "any" : this_platform();
    auto m = parse_addon_manifest(manifest_for(name, kind_word, version, "read,ui", plat, "x", "p.zip"));
    REQUIRE(m);
    return *m;
}

// Unpacks `files` as `m`'s package; the files, or nullopt with *error.
std::optional<std::vector<AddonFile>> unpack(const fs::path& dir, const AddonManifest& m,
                                             const std::map<std::string, std::string>& files, std::string* error) {
    const fs::path zip = dir / "pkg.zip";
    put_file(zip, zip_of(files));
    return unpack_addon(dir, m, zip, this_platform(), error);
}

InstalledAddon record(const AddonManifest& m, std::vector<AddonFile> files) {
    InstalledAddon a;
    a.name = m.name;
    a.kind = m.kind;
    a.source = "owner/" + m.name;
    a.version = m.version_text;
    a.caps = plugin::kCapRead | plugin::kCapUi;
    a.files = std::move(files);
    return a;
}

// Installs `files` as version `version` of the script `name`.
void install_script(const fs::path& dir, const std::string& name, const std::string& version,
                    const std::map<std::string, std::string>& files) {
    const AddonManifest m = manifest(name, AddonKind::Script, version);
    std::string why;
    auto unpacked = unpack(dir, m, files, &why);
    REQUIRE_MESSAGE(unpacked, why);
    REQUIRE_MESSAGE(apply_addon(dir, record(m, *unpacked), &why), why);
}

} // namespace

TEST_CASE("addon_install: managed activation persists independently of updates and verifies all installed files") {
    test::ScratchDir dir("addon_managed");
    auto m = manifest("managed", AddonKind::Script);
    m.autoload = true;
    std::string why;
    auto files = unpack(dir.path(), m, {{"init.lua", "print('managed')\n"}, {"helper.lua", "return 1\n"}}, &why);
    REQUIRE_MESSAGE(files, why);
    auto a = record(m, *files);
    a.autoload = true;
    a.enabled = true;
    REQUIRE_MESSAGE(apply_addon(dir.path(), a, &why), why);
    auto specs = managed_addon_scripts(dir.path());
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].enabled);
    CHECK(specs[0].verified);
    CHECK(specs[0].hash == sha256(std::string_view("print('managed')\n")));
    CHECK(set_addon_enabled(dir.path(), "MANAGED", false));
    CHECK_FALSE(managed_addon_scripts(dir.path())[0].enabled);
    put_file(addon_folder(dir.path(), AddonKind::Script, "managed") / "helper.lua", "return 2\n");
    CHECK_FALSE(managed_addon_scripts(dir.path())[0].verified);
    m = manifest("managed", AddonKind::Script, "1.0.1");
    m.autoload = true;
    files = unpack(dir.path(), m, {{"init.lua", "print('managed v2')\n"}}, &why);
    REQUIRE(files);
    a = record(m, *files); a.autoload = true; a.enabled = true;
    REQUIRE(apply_addon(dir.path(), a, &why));
    specs = managed_addon_scripts(dir.path());
    REQUIRE(specs.size() == 1);
    CHECK_FALSE(specs[0].enabled); // a download never reverses the user's disabled switch
    CHECK(specs[0].verified);
    CHECK(set_addon_enabled(dir.path(), "managed", true));
    CHECK(managed_addon_scripts(dir.path())[0].enabled);
    CHECK_FALSE(set_addon_enabled(dir.path(), "missing", true));
    REQUIRE(remove_addon(dir.path(), "managed", &why));
    CHECK(managed_addon_scripts(dir.path()).empty());
}

TEST_CASE("addon_install: an update that newly opts into managed loading requires player activation") {
    test::ScratchDir dir("addon_managed_optin");
    install_script(dir.path(), "managed", "1.0.0", {{"init.lua", "print('ordinary')\n"}});
    auto m = manifest("managed", AddonKind::Script, "1.0.1");
    m.autoload = true;
    std::string why;
    auto files = unpack(dir.path(), m, {{"init.lua", "print('managed')\n"}}, &why);
    REQUIRE(files);
    auto a = record(m, *files); a.autoload = true; a.enabled = true;
    REQUIRE(apply_addon(dir.path(), a, &why));
    const auto specs = managed_addon_scripts(dir.path());
    REQUIRE(specs.size() == 1);
    CHECK_FALSE(specs[0].enabled);
    CHECK(set_addon_enabled(dir.path(), "managed", true));
}

TEST_CASE("addon_install: dropping a manifest activation flag cannot bypass an installed disabled switch") {
    test::ScratchDir dir("addon_managed_downgrade");
    auto m = manifest("managed", AddonKind::Script);
    m.autoload = true;
    std::string why;
    auto files = unpack(dir.path(), m, {{"init.lua", "return 1\n"}}, &why);
    REQUIRE_MESSAGE(files, why);
    auto a = record(m, *files);
    a.autoload = true;
    a.enabled = false;
    REQUIRE_MESSAGE(apply_addon(dir.path(), a, &why), why);
    m = manifest("managed", AddonKind::Script, "1.0.1"); // new installs of this release are ordinary Lua
    CHECK_FALSE(m.autoload);
    files = unpack(dir.path(), m, {{"init.lua", "return 2\n"}}, &why);
    REQUIRE_MESSAGE(files, why);
    REQUIRE_MESSAGE(apply_addon(dir.path(), record(m, *files), &why), why);
    const auto specs = managed_addon_scripts(dir.path());
    REQUIRE(specs.size() == 1);
    CHECK_FALSE(specs[0].enabled);
    CHECK(specs[0].verified);
    const auto installed = read_installed_addons(dir.path());
    REQUIRE(installed.size() == 1);
    CHECK(installed[0].autoload);
    CHECK_FALSE(installed[0].enabled);
    CHECK(set_addon_enabled(dir.path(), "managed", true));
    REQUIRE(remove_addon(dir.path(), "managed", &why));
    files = unpack(dir.path(), m, {{"init.lua", "return 2\n"}}, &why);
    REQUIRE_MESSAGE(files, why);
    REQUIRE_MESSAGE(apply_addon(dir.path(), record(m, *files), &why), why);
    CHECK(managed_addon_scripts(dir.path()).empty()); // explicit removal/reinstallation resets the opt-in
}

TEST_CASE("addon_install: a script's package, checked and unpacked") {
    test::ScratchDir dir("addon_script");
    const AddonManifest m = manifest("autoloot", AddonKind::Script);
    std::string why;
    auto files = unpack(dir.path(), m,
                        {{"init.lua", "local x = require('autoloot.rules')\nprint(x)\n"},
                         {"rules.lua", "return {}\n"},
                         {"README.md", "# autoloot\n"},
                         {"data/items.txt", "Rusty Sword\n"}},
                        &why);
    REQUIRE_MESSAGE(files, why);
    CHECK(files->size() == 4);
    const fs::path out = addons_dir(dir.path()) / "new" / "autoloot";
    CHECK(read_file(out / "rules.lua") == "return {}\n");
    CHECK(read_file(out / "data" / "items.txt") == "Rusty Sword\n");
    for (const auto& f : *files)
        if (f.path == "README.md") CHECK(f.sha256 == sha256(std::string_view("# autoloot\n")));
}

TEST_CASE("addon_install: UI bundles install and update transactionally without running presentation Lua") {
    test::ScratchDir dir("addon_ui_bundle");
    auto parsed = parse_addon_manifest(manifest_for("interfaces", "ui", "1.0.0", "none", "any", "x", "p.zip"));
    REQUIRE(parsed);
    std::string why;
    auto files = unpack(dir.path(), *parsed,
        {{"ui/first/ui.ini", "name=First\nbase=builtin:modern\n"},
         {"ui/first/panel.lua", "error('install must not execute UI code')\n"},
         {"ui/first/lib/a.lua", "return 1\n"}, {"ui/first/lib/b.lua", "return 2\n"},
         {"ui/second/ui.ini", "name=Second\nbase=first\n"}, {"LICENSE", "original fixture license"}}, &why);
    REQUIRE_MESSAGE(files, why);
    auto installed = record(*parsed, *files);
    installed.caps = 0;
    REQUIRE_MESSAGE(apply_addon(dir.path(), installed, &why), why);
    const auto folder = dir.path() / "ui-addons/interfaces";
    CHECK(fs::is_regular_file(folder / "ui/first/ui.ini"));
    CHECK(fs::is_regular_file(folder / "ui/first/lib/a.lua"));
    CHECK(fs::is_regular_file(folder / "ui/first/lib/b.lua"));
    CHECK_FALSE(fs::exists(dir.path() / "scripts/interfaces"));
    CHECK(addon_owns_folder(dir.path(), "ui-addons/interfaces"));
    const auto registry = read_addon_registry(dir.path());
    REQUIRE(registry.addons.size() == 1);
    CHECK(registry.addons.front().kind == AddonKind::Ui);
    CHECK(registry.addons.front().caps == 0);
    CHECK(addon_ceilings(registry.addons).empty());
    const auto presentation = dir.path() / "ui-data/addon.interfaces.first/lib/layout.lua";
    put_file(presentation, "return {kind='player presentation'}\n");
    CHECK_FALSE(addon_owns_folder(dir.path(), "ui-data/addon.interfaces.first"));
    put_file(folder / "notes.txt", "player-owned data");
    parsed->version_text = "2.0.0";
    files = unpack(dir.path(), *parsed, {{"ui/first/ui.ini", "name=Updated\nbase=builtin:modern\n"}}, &why);
    REQUIRE_MESSAGE(files, why);
    installed = record(*parsed, *files);
    installed.caps = 0;
    REQUIRE_MESSAGE(apply_addon(dir.path(), installed, &why), why);
    CHECK_FALSE(fs::exists(folder / "ui/second/ui.ini"));
    CHECK(fs::is_regular_file(folder / "notes.txt"));
    CHECK(read_file(presentation) == "return {kind='player presentation'}\n");
    REQUIRE_MESSAGE(remove_addon(dir.path(), "interfaces", &why), why);
    CHECK(fs::is_regular_file(folder / "notes.txt"));
    CHECK_FALSE(fs::exists(folder / "ui/first/ui.ini"));
    CHECK(read_file(presentation) == "return {kind='player presentation'}\n");
    CHECK(read_installed_addons(dir.path()).empty());
    CHECK_FALSE(addon_name_taken(dir.path(), {}, AddonKind::Ui, "INTERFACES").empty());
}

TEST_CASE("addon_install: invalid UI bundles are rejected before writing files") {
    const auto parsed = parse_addon_manifest(manifest_for("interfaces", "ui", "1.0.0", "none", "any", "x", "p.zip"));
    REQUIRE(parsed);
    for (const std::map<std::string, std::string>& files : {
            std::map<std::string, std::string>{{"init.lua", "error('not a script')"}},
            {{"ui/first/panel.lua", "return true"}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/second/panel.lua", "return true"}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/FIRST/panel.lua", "return true"}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/first/run.exe", "program"}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/first/lib.so.1", "library"}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/first/panel.lua", "local = broken"}},
            {{"ui/first/ui.ini", "name=First"}, {"../escape.txt", "escape"}},
            {{"ui/bad.name/ui.ini", "name=Bad"}},
            {{"ui/first/ui.ini", ""}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/first/UI.ini", "name=Duplicate"}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/first/lib/a.lua", "return 1"},
             {"ui/first/Lib/b.lua", "return 2"}},
            {{"ui/first/ui.ini", "name=First"}, {"ui/first/lib", "data"}, {"ui/first/lib/a.lua", "return 1"}},
        }) {
        test::ScratchDir dir("addon_ui_refused");
        std::string why;
        CHECK_FALSE(unpack(dir.path(), *parsed, files, &why));
        CHECK_FALSE(why.empty());
        CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "new/interfaces"));
        CHECK_FALSE(fs::exists(dir.path() / "escape.txt"));
    }
}

TEST_CASE("addon_install: what a script's package may not hold") {
    test::ScratchDir dir("addon_script_bad");
    const AddonManifest m = manifest("autoloot", AddonKind::Script);
    struct Case {
        std::map<std::string, std::string> files;
        const char* why;
    };
    const std::string init = "print('hi')\n";
    for (const Case& c : std::vector<Case>{
             {{{"main.lua", init}}, "init.lua"},
             {{{"init.lua", "print('hi'"}}, "does not compile"},
             {{{"init.lua", "\x1bLua\x54\x00 precompiled"}}, "does not compile"},
             {{{"init.lua", init}, {"helper.exe", "MZ"}}, "may hold"},
             {{{"init.lua", init}, {"lib/x.so", "x"}}, "may hold"},
             {{{"init.lua", init}, {"run.sh", "rm -rf /"}}, "may hold"},
             {{{"init.lua", init}, {".hidden", "x"}}, "may hold"},
             // A device name is refused by the zip reader itself (core/windows_names.h).
             {{{"init.lua", init}, {"con.txt", "x"}}, "unsafe name"},
             {{{"init.lua", init}, {"Init.lua", init}}, "twice"},
         }) {
        std::string names;
        for (const auto& [n, _] : c.files) names += n + " ";
        CAPTURE(names);
        std::string why;
        CHECK_FALSE(unpack(dir.path(), m, c.files, &why));
        CHECK_MESSAGE(why.find(c.why) != std::string::npos, why);
    }
    // A name that leaves the folder refuses the whole zip (core/zip_read).
    std::string why;
    CHECK_FALSE(unpack(dir.path(), m, {{"init.lua", init}, {"../escape.lua", init}}, &why));
    CHECK_FALSE(fs::exists(dir.path() / "escape.lua"));
    // Not a zip at all.
    put_file(dir.path() / "junk.zip", "not a zip");
    CHECK_FALSE(unpack_addon(dir.path(), m, dir.path() / "junk.zip", this_platform(), &why));
}

TEST_CASE("addon_install: a plugin's package: one library of this platform, named after it") {
    if (this_platform().empty()) return; // a platform with no plugin builds
    test::ScratchDir dir("addon_plugin");
    const AddonManifest m = manifest("MQ2Hud", AddonKind::Plugin);
    const std::string lib = library_for(this_platform());
    std::string why;
    auto files = unpack(dir.path(), m,
                        {{"MQ2Hud" + lib_ext(), lib},
                         {"ui/modern/hud.lua", "ui.window('Hud', function() end)\n"},
                         {"MQ2Hud.ini", "[Settings]\n"}},
                        &why);
    REQUIRE_MESSAGE(files, why);
    CHECK(files->size() == 3);

    struct Case {
        std::map<std::string, std::string> files;
        const char* why;
    };
    const std::string other = this_platform().starts_with("linux") ? "windows-x64" : "linux-x64";
    for (const Case& c : std::vector<Case>{
             {{{"MQ2Hud.ini", "x"}}, "no library"},
             {{{"Other" + lib_ext(), lib}}, "named after"},
             {{{"bin/MQ2Hud" + lib_ext(), lib}}, "top"},
             {{{"MQ2Hud" + lib_ext(), lib}, {"libMQ2Hud" + lib_ext(), lib}}, "more than one"},
             {{{"MQ2Hud" + lib_ext(), "not a library at all"}}, "not a library"},
             {{{"MQ2Hud" + lib_ext(), library_for(other)}}, "not a library"},
             {{{"MQ2Hud" + lib_ext(), lib}, {"helper.lua", "x = 1"}}, "may hold"},
             {{{"MQ2Hud" + lib_ext(), lib}, {"ui/modern/hud.lua", "x = "}}, "does not compile"},
         }) {
        std::string names;
        for (const auto& [n, _] : c.files) names += n + " ";
        CAPTURE(names);
        CHECK_FALSE(unpack(dir.path(), m, c.files, &why));
        CHECK_MESSAGE(why.find(c.why) != std::string::npos, why);
    }
}

TEST_CASE("addon_install: install, update and remove keep the player's own files") {
    test::ScratchDir dir("addon_apply");
    const fs::path folder = addon_folder(dir.path(), AddonKind::Script, "autoloot");
    install_script(dir.path(), "autoloot", "1.0.0", {{"init.lua", "-- v1\n"}, {"old.lua", "return 1\n"}});
    CHECK(read_file(folder / "init.lua") == "-- v1\n");
    auto all = read_installed_addons(dir.path());
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "autoloot");
    CHECK(all[0].version == "1.0.0");
    CHECK(all[0].files.size() == 2);
    CHECK(modified_addon_files(dir.path(), all[0]).empty());
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "new" / "autoloot"));
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "journal.txt"));

    // The player's own file in the folder, and a change to one of the add-on's.
    put_file(folder / "my-notes.txt", "mine\n");
    put_file(folder / "old.lua", "return 2 -- edited\n");
    CHECK(modified_addon_files(dir.path(), read_installed_addons(dir.path())[0]) == std::vector<std::string>{"old.lua"});

    // Version 2 drops old.lua and adds new.lua: old.lua goes, the player's file stays.
    install_script(dir.path(), "autoloot", "2.0.0", {{"init.lua", "-- v2\n"}, {"sub/new.lua", "return 3\n"}});
    CHECK(read_file(folder / "init.lua") == "-- v2\n");
    CHECK(read_file(folder / "sub" / "new.lua") == "return 3\n");
    CHECK_FALSE(fs::exists(folder / "old.lua"));
    CHECK(read_file(folder / "my-notes.txt") == "mine\n");
    all = read_installed_addons(dir.path());
    REQUIRE(all.size() == 1);
    CHECK(all[0].version == "2.0.0");
    // The registry saved before installed.txt was rewritten is gone once the update is done.
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "installed.prev.txt"));
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "journal.txt"));

    // Removal takes the add-on's files and its record; the player's file and the folder holding it stay.
    std::string why;
    REQUIRE_MESSAGE(remove_addon(dir.path(), "AUTOLOOT", &why), why);
    CHECK(read_installed_addons(dir.path()).empty());
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "installed.prev.txt"));
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "journal.txt"));
    CHECK_FALSE(fs::exists(folder / "init.lua"));
    CHECK_FALSE(fs::exists(folder / "sub"));
    CHECK(read_file(folder / "my-notes.txt") == "mine\n");
    CHECK_FALSE(remove_addon(dir.path(), "autoloot", &why));

    // A folder left empty goes with it.
    install_script(dir.path(), "tidy", "1.0.0", {{"init.lua", "-- x\n"}});
    REQUIRE(remove_addon(dir.path(), "tidy", &why));
    CHECK_FALSE(fs::exists(addon_folder(dir.path(), AddonKind::Script, "tidy")));
}

// A part that was a folder and is a file now: the folder's files go to previous/, the emptied folder is taken out,
// and the file goes in its place (it once failed at every try, moving the empty folder onto previous/'s).
TEST_CASE("addon_install: an update that turns a folder into a file of its name installs") {
    test::ScratchDir dir("addon_dir_to_file");
    const fs::path folder = addon_folder(dir.path(), AddonKind::Script, "foo");
    install_script(dir.path(), "foo", "1.0.0", {{"init.lua", "-- v1\n"}, {"lib/util.lua", "return 1\n"}});
    REQUIRE(fs::is_regular_file(folder / "lib" / "util.lua"));
    install_script(dir.path(), "foo", "1.1.0", {{"init.lua", "-- v2\n"}, {"lib", "data\n"}});
    CHECK(fs::is_regular_file(folder / "lib"));
    CHECK(read_file(folder / "lib") == "data\n");
    CHECK(read_installed_addons(dir.path())[0].version == "1.1.0");
    // A folder holding the player's own file is theirs: the update stops and the old version stays.
    install_script(dir.path(), "foo", "1.2.0", {{"init.lua", "-- v3\n"}, {"lib/util.lua", "return 3\n"}});
    put_file(folder / "lib" / "mine.txt", "mine\n");
    const AddonManifest m = manifest("foo", AddonKind::Script, "1.3.0");
    std::string why;
    auto unpacked = unpack(dir.path(), m, {{"init.lua", "-- v4\n"}, {"lib", "data\n"}}, &why);
    REQUIRE_MESSAGE(unpacked, why);
    CHECK_FALSE(apply_addon(dir.path(), record(m, *unpacked), &why));
    CHECK(read_file(folder / "lib" / "mine.txt") == "mine\n");
    CHECK(read_file(folder / "lib" / "util.lua") == "return 3\n");
    CHECK(read_installed_addons(dir.path())[0].version == "1.2.0");
}

// An update takes out only the folders its files left empty or that are in a new file's way: an empty folder a
// script made at run time (its saves) or the player made stays.
TEST_CASE("addon_install: an update keeps empty folders it did not empty") {
    test::ScratchDir dir("addon_keep_empty");
    const fs::path folder = addon_folder(dir.path(), AddonKind::Script, "foo");
    install_script(dir.path(), "foo", "1.0.0", {{"init.lua", "-- v1\n"}, {"old/deep/a.lua", "return 1\n"}});
    fs::create_directories(folder / "saves");
    fs::create_directories(folder / "logs" / "today");
    install_script(dir.path(), "foo", "1.1.0", {{"init.lua", "-- v2\n"}, {"new/b.lua", "return 2\n"}});
    CHECK(fs::is_directory(folder / "saves"));
    CHECK(fs::is_directory(folder / "logs" / "today"));
    CHECK_FALSE(fs::exists(folder / "old")); // the earlier version's, emptied by this update
    CHECK(read_file(folder / "new" / "b.lua") == "return 2\n");
    // An empty folder in a new file's way still goes.
    fs::create_directories(folder / "data" / "empty");
    install_script(dir.path(), "foo", "1.2.0", {{"init.lua", "-- v3\n"}, {"data", "x\n"}});
    CHECK(read_file(folder / "data") == "x\n");
    CHECK(fs::is_directory(folder / "saves"));
    CHECK_FALSE(fs::exists(folder / "new")); // 1.1.0's, emptied
}

TEST_CASE("addon_install: an install cut off is undone at the next start") {
    test::ScratchDir dir("addon_journal");
    const fs::path folder = addon_folder(dir.path(), AddonKind::Script, "autoloot");
    install_script(dir.path(), "autoloot", "1.0.0", {{"init.lua", "-- v1\n"}});
    const std::string registry = read_file(addons_dir(dir.path()) / "installed.txt");

    // As apply_addon would leave it if the client died between its moves: the old file in previous/, the new one in
    // place, the journal without "done".
    const fs::path prev = addons_dir(dir.path()) / "previous" / "autoloot";
    put_file(prev / "init.lua", "-- v1\n");
    put_file(folder / "init.lua", "-- v2\n");
    put_file(addons_dir(dir.path()) / "new" / "autoloot" / "extra.lua", "-- staged\n");
    put_file(addons_dir(dir.path()) / "journal.txt",
             "move\tscripts/autoloot/init.lua\taddons/previous/autoloot/init.lua\n"
             "move\taddons/new/autoloot/init.lua\tscripts/autoloot/init.lua\n");
    addons_at_start(dir.path());
    CHECK(read_file(folder / "init.lua") == "-- v1\n");
    CHECK(read_file(addons_dir(dir.path()) / "installed.txt") == registry);
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "journal.txt"));
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "previous"));
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "new"));

    // Cut off after installed.txt was rewritten for the new version: the old registry comes back with the old files.
    put_file(prev / "init.lua", "-- v1\n");
    put_file(folder / "init.lua", "-- v2\n");
    put_file(addons_dir(dir.path()) / "installed.prev.txt", registry);
    put_file(addons_dir(dir.path()) / "installed.txt", "hailstone-addons 1\n\naddon autoloot script\nversion 2.0.0\n");
    put_file(addons_dir(dir.path()) / "journal.txt",
             "move\tscripts/autoloot/init.lua\taddons/previous/autoloot/init.lua\n"
             "move\taddons/new/autoloot/init.lua\tscripts/autoloot/init.lua\n"
             "registry\tsaved\n");
    addons_at_start(dir.path());
    CHECK(read_file(folder / "init.lua") == "-- v1\n");
    CHECK(read_file(addons_dir(dir.path()) / "installed.txt") == registry);
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "installed.prev.txt"));
    // The first add-on's install, cut off the same way: there was no registry before it.
    const std::string with_v1 = read_file(addons_dir(dir.path()) / "installed.txt");
    put_file(addons_dir(dir.path()) / "journal.txt", "registry\tnone\n");
    addons_at_start(dir.path());
    CHECK_FALSE(fs::exists(addons_dir(dir.path()) / "installed.txt"));
    put_file(addons_dir(dir.path()) / "installed.txt", with_v1);

    // One that finished ("done") is left as it is.
    put_file(folder / "init.lua", "-- v2\n");
    put_file(prev / "init.lua", "-- v1\n");
    put_file(addons_dir(dir.path()) / "journal.txt",
             "move\tscripts/autoloot/init.lua\taddons/previous/autoloot/init.lua\ndone\n");
    addons_at_start(dir.path());
    CHECK(read_file(folder / "init.lua") == "-- v2\n");

    // A damaged journal moves nothing outside the folder.
    put_file(addons_dir(dir.path()) / "journal.txt", "move\t../outside\tscripts/autoloot/init.lua\n");
    addons_at_start(dir.path());
    CHECK(fs::exists(folder / "init.lua"));
    CHECK_FALSE(fs::exists(dir.path().parent_path() / "outside"));
}

// The journal names a batch of moves in one synced write before any of them is made (the old files out, then the new
// ones in): a cut anywhere in a batch leaves lines for moves never made, which find nothing where they point and are
// skipped, while the moves made are put back.
TEST_CASE("addon_install: an install cut off inside a batch of moves is undone") {
    test::ScratchDir dir("addon_journal_batch");
    const fs::path folder = addon_folder(dir.path(), AddonKind::Script, "autoloot");
    install_script(dir.path(), "autoloot", "1.0.0", {{"init.lua", "-- v1\n"}, {"lib.lua", "-- lib v1\n"}});
    const fs::path addons = addons_dir(dir.path());
    const std::string registry = read_file(addons / "installed.txt");
    const std::string outs = "move\tscripts/autoloot/init.lua\taddons/previous/autoloot/init.lua\n"
                             "move\tscripts/autoloot/lib.lua\taddons/previous/autoloot/lib.lua\n";
    const std::string ins = "move\taddons/new/autoloot/init.lua\tscripts/autoloot/init.lua\n"
                            "move\taddons/new/autoloot/lib.lua\tscripts/autoloot/lib.lua\n";

    // Cut inside the first batch: one old file moved out, the other not yet.
    fs::create_directories(addons / "previous" / "autoloot");
    fs::rename(folder / "init.lua", addons / "previous" / "autoloot" / "init.lua");
    put_file(addons / "new" / "autoloot" / "init.lua", "-- v2\n");
    put_file(addons / "new" / "autoloot" / "lib.lua", "-- lib v2\n");
    put_file(addons / "journal.txt", outs);
    addons_at_start(dir.path());
    CHECK(read_file(folder / "init.lua") == "-- v1\n");
    CHECK(read_file(folder / "lib.lua") == "-- lib v1\n");
    CHECK(read_file(addons / "installed.txt") == registry);

    // Cut after the second batch's lines were written, before (and then part way through) its moves.
    for (int made = 0; made <= 1; ++made) {
        fs::create_directories(addons / "previous" / "autoloot");
        fs::rename(folder / "init.lua", addons / "previous" / "autoloot" / "init.lua");
        fs::rename(folder / "lib.lua", addons / "previous" / "autoloot" / "lib.lua");
        put_file(addons / "new" / "autoloot" / "init.lua", "-- v2\n");
        put_file(addons / "new" / "autoloot" / "lib.lua", "-- lib v2\n");
        if (made) fs::rename(addons / "new" / "autoloot" / "init.lua", folder / "init.lua");
        put_file(addons / "journal.txt", outs + ins);
        addons_at_start(dir.path());
        CHECK(read_file(folder / "init.lua") == "-- v1\n");
        CHECK(read_file(folder / "lib.lua") == "-- lib v1\n");
        CHECK(read_file(addons / "installed.txt") == registry);
        CHECK_FALSE(fs::exists(addons / "journal.txt"));
        CHECK_FALSE(fs::exists(addons / "previous"));
    }

    // And a whole install still goes through, its journal gone.
    install_script(dir.path(), "autoloot", "2.0.0", {{"init.lua", "-- v2\n"}, {"lib.lua", "-- lib v2\n"}});
    CHECK(read_file(folder / "lib.lua") == "-- lib v2\n");
    CHECK_FALSE(fs::exists(addons / "journal.txt"));
}

// The journal is written ahead of each move (on the disk before it); should it still come back without a line, a
// file left in previous/ after the undo may be the only copy of a replaced one: it is kept aside, not deleted.
TEST_CASE("addon_install: files in previous/ an unfinished journal does not name are kept aside") {
    test::ScratchDir dir("addon_kept");
    const fs::path addons = addons_dir(dir.path());
    install_script(dir.path(), "autoloot", "1.0.0", {{"init.lua", "-- v1\n"}});
    CHECK_FALSE(fs::exists(addons / "journal.txt")); // a finished install removes its journal
    put_file(addons / "previous" / "autoloot" / "init.lua", "-- the only copy\n");
    put_file(addons / "journal.txt", "");
    addons_at_start(dir.path());
    CHECK_FALSE(fs::exists(addons / "previous"));
    std::vector<fs::path> kept;
    for (const auto& e : fs::directory_iterator(addons))
        if (e.path().filename().string().starts_with("kept-")) kept.push_back(e.path());
    REQUIRE(kept.size() == 1);
    CHECK(read_file(kept[0] / "autoloot" / "init.lua") == "-- the only copy\n");

    // A finished journal's previous/ is the old version's, removed as before.
    put_file(addons / "previous" / "autoloot" / "init.lua", "-- v0\n");
    put_file(addons / "journal.txt", "done\n");
    addons_at_start(dir.path());
    CHECK_FALSE(fs::exists(addons / "previous"));
}

TEST_CASE("addon_install: installed.txt reads back, a damaged record is taken from its backup or refused") {
    test::ScratchDir dir("addon_registry");
    InstalledAddon a;
    a.name = "MQ2Hud";
    a.kind = AddonKind::Plugin;
    a.source = "owner/repo@v2";
    a.version = "2.0.0-beta.1";
    a.caps = plugin::kCapRead | plugin::kCapUi;
    a.key = Keys().pub;
    a.auto_update = true;
    a.author = "Some One";
    a.description = "Draws a hud.";
    a.notes = "https://github.com/owner/repo/releases/tag/v2";
    a.files = {{"MQ2Hud.so", 10, sha256(std::string_view("a"))}, {"ui/modern/my hud.lua", 3, sha256(std::string_view("b"))}};
    InstalledAddon b;
    b.name = "autoloot";
    b.source = "owner/autoloot";
    b.version = "1.0.0";
    b.caps = plugin::kCapRead;
    REQUIRE(write_installed_addons(dir.path(), {a, b}));
    auto back = read_installed_addons(dir.path());
    REQUIRE(back.size() == 2);
    CHECK(back[0].name == "MQ2Hud");
    CHECK(back[0].kind == AddonKind::Plugin);
    CHECK(back[0].source == a.source);
    CHECK(back[0].version == a.version);
    CHECK(back[0].caps == a.caps);
    CHECK(back[0].key == a.key);
    CHECK(back[0].auto_update);
    CHECK(back[0].author == a.author);
    CHECK(back[0].description == a.description);
    CHECK(back[0].notes == a.notes);
    REQUIRE(back[0].files.size() == 2);
    CHECK(back[0].files[1].path == "ui/modern/my hud.lua");
    CHECK(back[0].files[1].sha256 == a.files[1].sha256);
    CHECK_FALSE(back[1].auto_update); // off unless the player switched it on
    CHECK(find_addon(back, "mq2hud") == &back[0]);

    // Written with its backup, which stands in for a damaged record.
    const fs::path reg = addons_dir(dir.path()) / "installed.txt", bak = addons_dir(dir.path()) / "installed.bak.txt";
    CHECK(read_file(bak) == read_file(reg));
    {
        // An older client's "auto 1" was written for every install without asking the player: it reads as off.
        std::string old = read_file(reg);
        REQUIRE(old.find("autoupdate 1\n") != std::string::npos);
        old.replace(old.find("autoupdate 1\n"), 13, "auto 1\n");
        put_file(reg, old);
        CHECK_FALSE(read_installed_addons(dir.path())[0].auto_update);
        REQUIRE(write_installed_addons(dir.path(), {a, b}));
    }
    std::string text = read_file(reg);
    text.replace(text.find("ui/modern/my hud.lua"), 20, "../../escape.lua");
    put_file(reg, text);
    AddonRegistry r = read_addon_registry(dir.path());
    REQUIRE(r.addons.size() == 2);
    const InstalledAddon* hud = find_addon(r.addons, "MQ2Hud");
    REQUIRE(hud);
    CHECK(hud->files.size() == 2);
    CHECK(r.from_backup);
    CHECK(r.unreadable.empty());
    CHECK(r.writable());
    // Without the backup the damaged record is not read, and is named as unreadable (its files get nothing).
    fs::remove(bak);
    r = read_addon_registry(dir.path());
    REQUIRE(r.addons.size() == 1);
    CHECK(r.addons[0].name == "autoloot");
    REQUIRE(r.unreadable.size() == 1);
    CHECK(r.unreadable[0].name == "MQ2Hud");
    CHECK(r.unreadable[0].kind == AddonKind::Plugin);
    CHECK_FALSE(r.writable());
    CHECK_FALSE(write_installed_addons(dir.path(), {b})); // it would drop MQ2Hud's record
    CHECK(read_file(reg) == text);
    // A newer Hailstone's registry (an update rolled back): nothing reads, each add-on it names is unreadable, and
    // it is never written over.
    put_file(reg, "hailstone-addons 99\naddon x script\naddon y something\n");
    r = read_addon_registry(dir.path());
    CHECK(r.addons.empty());
    CHECK(r.newer);
    REQUIRE(r.unreadable.size() == 2);
    CHECK(r.unreadable[0].kind == AddonKind::Script);
    CHECK_FALSE(r.unreadable[1].kind);
    CHECK_FALSE(write_installed_addons(dir.path(), {}));
    CHECK(read_installed_addons(dir.path()).empty());
}

TEST_CASE("addon_install: names that are taken") {
    test::ScratchDir dir("addon_names");
    std::vector<InstalledAddon> none;
    CHECK(addon_name_taken(dir.path(), none, AddonKind::Script, "autoloot").empty());
    CHECK_FALSE(addon_name_taken(dir.path(), none, AddonKind::Script, "config").empty());
    CHECK_FALSE(addon_name_taken(dir.path(), none, AddonKind::Script, "../x").empty());
    put_file(dir.path() / "scripts" / "byhand" / "init.lua", "x");
    CHECK(addon_name_taken(dir.path(), none, AddonKind::Script, "byhand").find("by hand") != std::string::npos);
    put_file(dir.path() / "scripts" / "single.lua", "x");
    CHECK_FALSE(addon_name_taken(dir.path(), none, AddonKind::Script, "Single").empty());
    fs::create_directories(dir.path() / "scripts" / "empty");
    CHECK(addon_name_taken(dir.path(), none, AddonKind::Script, "empty").empty());
    put_file(dir.path() / "plugins" / "shipped" / ("shipped" + lib_ext()), "x");
    CHECK_FALSE(addon_name_taken(dir.path(), none, AddonKind::Plugin, "shipped").empty());
    // A script and a compiled plugin never share a name (plugin::UiBridge tells their windows apart, the player
    // would not).
    CHECK(addon_name_taken(dir.path(), none, AddonKind::Script, "shipped").find("share a name") != std::string::npos);
    CHECK(addon_name_taken(dir.path(), none, AddonKind::Plugin, "byhand").find("share a name") != std::string::npos);
    // Only settings left behind (an update took out a plugin an older release shipped): not taken.
    put_file(dir.path() / "plugins" / "leftover" / "leftover.ini", "[x]\n");
    fs::create_directories(dir.path() / "plugins" / "leftover" / "ui");
    if (!lib_ext().empty()) {
        CHECK(addon_name_taken(dir.path(), none, AddonKind::Plugin, "leftover").empty());
        // The install goes in beside them and keeps them.
        const AddonManifest m = manifest("leftover", AddonKind::Plugin);
        std::string why;
        auto files = unpack(dir.path(), m,
                            {{"leftover" + lib_ext(), library_for(this_platform())}, {"ui/modern/w.lua", "x = 1\n"}},
                            &why);
        REQUIRE_MESSAGE(files, why);
        REQUIRE_MESSAGE(apply_addon(dir.path(), record(m, *files), &why), why);
        const fs::path folder = addon_folder(dir.path(), AddonKind::Plugin, "leftover");
        CHECK(fs::exists(folder / ("leftover" + lib_ext())));
        CHECK(read_file(folder / "leftover.ini") == "[x]\n");
    }
    put_file(dir.path() / "scripts" / "notes" / "notes.txt", "x"); // a script's folder is taken by any file
    CHECK_FALSE(addon_name_taken(dir.path(), none, AddonKind::Script, "notes").empty());
    if (!lib_ext().empty()) {
        put_file(dir.path() / "plugins" / ("libLoose" + lib_ext()), "x");
        CHECK_FALSE(addon_name_taken(dir.path(), none, AddonKind::Plugin, "Loose").empty());
    }
    InstalledAddon a;
    a.name = "Mine";
    CHECK_FALSE(addon_name_taken(dir.path(), {a}, AddonKind::Plugin, "mine").empty());

    // The core updater leaves an add-on's folder alone.
    REQUIRE(write_installed_addons(dir.path(), {a}));
    CHECK(addon_owns_folder(dir.path(), "scripts/mine"));
    CHECK_FALSE(addon_owns_folder(dir.path(), "plugins/mine"));
    CHECK(is_player_path("addons/installed.txt"));
}

TEST_CASE("addon_install: the ceilings are the add-on's Lua files and its library") {
    InstalledAddon s;
    s.name = "autoloot";
    s.source = "owner/autoloot@v1";
    s.caps = plugin::kCapRead;
    s.files = {{"init.lua", 1, sha256(std::string_view("i"))}, {"data.txt", 1, sha256(std::string_view("d"))}};
    InstalledAddon p;
    p.name = "MQ2Hud";
    p.source = "owner/hud";
    p.kind = AddonKind::Plugin;
    p.caps = plugin::kCapUi;
    p.files = {{"MQ2Hud.so", 1, sha256(std::string_view("l"))},
               {"MQ2Hud.ini", 1, sha256(std::string_view("n"))},
               {"ui/modern/hud.lua", 1, sha256(std::string_view("u"))}};
    const auto c = addon_ceilings(std::vector<InstalledAddon>{s, p});
    REQUIRE(c.size() == 2);
    CHECK(c[0].kind == plugin::FileKind::Script);
    CHECK(c[0].sha256 == sha256(std::string_view("i")));
    CHECK(c[0].caps == plugin::kCapRead);
    CHECK(c[0].addon_name == "autoloot");
    CHECK(c[0].addon_source == s.source);
    CHECK(c[1].kind == plugin::FileKind::Plugin);
    CHECK(c[1].sha256 == sha256(std::string_view("l")));
    CHECK(c[1].caps == plugin::kCapUi);
    CHECK(c[1].addon_name == "MQ2Hud");
    CHECK(c[1].addon_source == p.source);
}

TEST_CASE("addon_install: every script module carries its source identity and changed bytes fail a closed policy") {
    test::ScratchDir dir("addon_identity");
    install_script(dir.path(), "needed", "1.0.0",
                   {{"init.lua", "require('helper')\n"}, {"helper.lua", "return 42\n"}});
    const auto ceilings = addon_ceilings(dir.path());
    REQUIRE(ceilings.size() == 2);
    plugin::Policy policy;
    policy.set_ceilings(ceilings);
    policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                       "required = owner/needed\n"));
    for (const auto& c : ceilings) {
        CHECK(c.addon_name == "needed");
        CHECK(c.addon_source == "owner/needed");
        CHECK(policy.caps(c.kind, c.sha256, "needed") != 0);
        CHECK(policy.required_file(c.kind, c.sha256));
    }
    const auto entries = policy.required_entries(plugin::FileKind::Script);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].first == "needed");
    CHECK(entries[0].second == sha256(std::string_view("require('helper')\n")));
    put_file(dir.path() / "scripts/needed/init.lua", "print('changed')\n");
    CHECK(policy.caps(plugin::FileKind::Script, sha256(std::string_view("print('changed')\n")), "needed") == 0);
}

TEST_CASE("addon_install: source blocks retain changed and extra managed files without changing legacy caps") {
    test::ScratchDir dir("addon_changed_identity");
    install_script(dir.path(), "needed", "1.0.0", {{"init.lua", "print('original')\n"}});
    put_file(dir.path() / "scripts/needed/init.lua", "print('changed')\n");
    put_file(dir.path() / "scripts/needed/extra.lua", "return 'extra'\n");
    plugin::Policy policy;
    // Legacy startup/install reads only verified registry hashes, without attributing changed or extra code.
    const auto legacy = addon_ceilings(dir.path());
    REQUIRE(legacy.size() == 1);
    CHECK(legacy.front().verified);
    CHECK(legacy.front().sha256 == sha256(std::string_view("print('original')\n")));
    const auto explicit_legacy = addon_ceilings(dir.path(), false);
    REQUIRE(explicit_legacy.size() == legacy.size());
    CHECK(explicit_legacy.front().sha256 == legacy.front().sha256);
    const auto ceilings = addon_ceilings(dir.path(), true);
    REQUIRE(ceilings.size() == 3);
    policy.set_ceilings(ceilings);
    for (const auto& c : ceilings)
        if (!c.verified) {
            CHECK(c.addon_name == "needed");
            CHECK(c.addon_source == "owner/needed");
        }
    const auto changed = sha256(std::string_view("print('changed')\n"));
    const auto extra = sha256(std::string_view("return 'extra'\n"));
    policy.set(plugin::Rules{});
    CHECK(policy.caps(plugin::FileKind::Script, changed, "needed") == plugin::kCapAll);
    CHECK(policy.caps(plugin::FileKind::Script, extra, "needed") == plugin::kCapAll);
    auto closed = plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\n"
                                          "required = owner/needed\n");
    policy.set(closed);
    CHECK(policy.caps(plugin::FileKind::Script, changed, "needed") == 0);
    CHECK(policy.caps(plugin::FileKind::Script, extra, "needed") == 0);
    CHECK_FALSE(policy.required_file(plugin::FileKind::Script, changed));
    policy.set(plugin::parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = allow\n"
                                       "block = owner/needed\n"));
    CHECK(policy.caps(plugin::FileKind::Script, changed, "needed") == 0);
    CHECK(policy.caps(plugin::FileKind::Script, extra, "needed") == 0);
}

// A lost or damaged installed.txt fails closed: an add-on's files never become hand-installed ones (every
// capability on a server without hailstone.cfg). The backup brings it back when it can.
TEST_CASE("addon_install: an add-on whose record cannot be read gets no capabilities") {
    test::ScratchDir dir("addon_fail_closed");
    install_script(dir.path(), "autoloot", "1.0.0", {{"init.lua", "print(1)\n"}, {"lib/rules.lua", "return {}\n"}});
    const Sha256Digest init = sha256(std::string_view("print(1)\n")), rules = sha256(std::string_view("return {}\n"));
    const fs::path reg = addons_dir(dir.path()) / "installed.txt", bak = addons_dir(dir.path()) / "installed.bak.txt";
    REQUIRE(fs::exists(bak));
    plugin::Rules open; // a server without hailstone.cfg
    auto caps_of = [&](const Sha256Digest& h) {
        plugin::Policy p;
        p.set_ceilings(addon_ceilings(dir.path()));
        p.set(open);
        return p.caps(plugin::FileKind::Script, h);
    };
    CHECK(caps_of(init) == (plugin::kCapRead | plugin::kCapUi));

    SUBCASE("installed.txt lost: read from the backup, and written again at start") {
        fs::remove(reg);
        CHECK(read_addon_registry(dir.path()).from_backup);
        CHECK(caps_of(init) == (plugin::kCapRead | plugin::kCapUi));
        addons_at_start(dir.path());
        REQUIRE(fs::exists(reg));
        const AddonRegistry r = read_addon_registry(dir.path());
        CHECK(r.problem.empty());
        CHECK(r.addons.size() == 1);
    }
    SUBCASE("both a newer Hailstone's: the files it names may do nothing, and nothing installs") {
        const std::string newer = "hailstone-addons 2\naddon autoloot script\nsomething new\n";
        put_file(reg, newer);
        put_file(bak, newer);
        CHECK(caps_of(init) == 0u);
        CHECK(caps_of(rules) == 0u);
        CHECK(caps_of(sha256(std::string_view("a script of the player's"))) == plugin::kCapAll);
        addons_at_start(dir.path());
        CHECK(read_file(reg) == newer); // not written over
        std::string why;
        CHECK_FALSE(remove_addon(dir.path(), "autoloot", &why));
        CHECK(why.find("newer") != std::string::npos);
        CHECK(fs::exists(dir.path() / "scripts" / "autoloot" / "init.lua"));
        CHECK(addon_owns_folder(dir.path(), "scripts/autoloot"));
    }
    SUBCASE("garbage and no backup: the add-on it still names is refused") {
        put_file(reg, "\x01\x02garbage\naddon autoloot script\nfile zz\n");
        fs::remove(bak);
        CHECK(caps_of(init) == 0u);
        CHECK(caps_of(rules) == 0u);
        CHECK_FALSE(read_addon_registry(dir.path()).writable());
    }
    SUBCASE("an empty installed.txt with a good backup: the backup's") {
        put_file(reg, "");
        CHECK(caps_of(init) == (plugin::kCapRead | plugin::kCapUi));
        addons_at_start(dir.path());
        CHECK(read_addon_registry(dir.path()).problem.empty());
    }
}

TEST_CASE("addon_install: a pinned add-on is the one from its repository with its key") {
    Keys keys, other(9);
    InstalledAddon a;
    a.name = "texstudio";
    a.kind = AddonKind::Tool;
    a.source = "ProjectHailstone/hailstone-texstudio@v1.0.0";
    a.key = keys.pub;
    const std::vector<InstalledAddon> all = {a};
    CHECK(find_pinned_addon(all, "texstudio", AddonKind::Tool, "projecthailstone/hailstone-texstudio", keys.pub));
    CHECK_FALSE(find_pinned_addon(all, "texstudio", AddonKind::Tool, "projecthailstone/hailstone-texstudio", other.pub));
    CHECK_FALSE(find_pinned_addon(all, "texstudio", AddonKind::Tool, "someone/hailstone-texstudio", keys.pub));
    CHECK_FALSE(find_pinned_addon(all, "texstudio", AddonKind::Script, "projecthailstone/hailstone-texstudio", keys.pub));
    auto unsigned_one = all;
    unsigned_one[0].key.reset();
    CHECK_FALSE(find_pinned_addon(unsigned_one, "texstudio", AddonKind::Tool, "projecthailstone/hailstone-texstudio",
                                  keys.pub));
}

TEST_CASE("addon_install: lua_compiles takes text only") {
    std::string why;
    CHECK(lua_compiles("return 1", "x", &why));
    CHECK_FALSE(lua_compiles("return (", "x", &why));
    CHECK(why.find("x:") != std::string::npos);
    CHECK_FALSE(lua_compiles(std::string("\x1bLua", 4), "x", &why));
}

TEST_CASE("addon_manager: the server's own and the hand-installed, listed beside the add-ons") {
    test::ScratchDir dir("addon_lists");
    put_file(dir.path() / "scripts" / "hand.lua", "x");
    put_file(dir.path() / "scripts" / "folder" / "init.lua", "x");
    put_file(dir.path() / "scripts" / "config" / "init.lua", "x");
    put_file(dir.path() / "scripts" / "notascript" / "readme.txt", "x");
    put_file(dir.path() / "scripts" / "mine" / "init.lua", "x");
    put_file(dir.path() / "plugins" / "Dev" / ("Dev" + lib_ext()), "x");
    InstalledAddon mine;
    mine.name = "mine";
    const auto manual = manual_addons({dir.path()}, {mine});
    std::vector<std::string> names;
    for (const auto& m : manual) names.push_back(m.name + ":" + std::string(addon_kind_word(m.kind)));
    std::vector<std::string> want = {"folder:script", "hand:script"};
    if (!lib_ext().empty()) want.insert(want.begin(), "Dev:plugin");
    CHECK(names == want);

    // A server's files as its last login recorded them (net/server_files.h's record).
    const fs::path profile = dir.path() / "servers" / "one";
    CHECK(server_addons(profile).empty());
    put_file(profile / "scripts" / "hud.lua", "x");
    put_file(profile / "scripts" / "loot" / "init.lua", "x");
    put_file(profile / "plugins" / "MQ2Srv" / ("MQ2Srv" + lib_ext()), "x");
    std::string record = "# hailstone server files 2\n";
    for (const char* p : {"scripts/hud.lua", "scripts/loot/init.lua"})
        record += to_hex(sha256(std::string_view("x"))) + " 1 " + p + "\n";
    record += to_hex(sha256(std::string_view("x"))) + " 1 plugins/MQ2Srv/MQ2Srv" + lib_ext() + "\n";
    put_file(profile / "server_files.txt", record);
    const auto server = server_addons(profile);
    names.clear();
    for (const auto& s : server) names.push_back(s.name + ":" + std::string(addon_kind_word(s.kind)));
    std::vector<std::string> want_server = {"hud:script", "loot:script"};
    if (!lib_ext().empty()) want_server.push_back("MQ2Srv:plugin");
    CHECK(names == want_server);
    // The player's word on the server's plugin, for the bytes it sent (plugin/consent.h).
    if (!lib_ext().empty()) {
        CHECK(server.back().consent == plugin::Consent::Unasked);
        CHECK(server.back().hashes == std::vector<Sha256Digest>{sha256(std::string_view("x"))}); // its bytes now
        REQUIRE(plugin::set_plugin_consent(profile, "MQ2Srv", sha256(std::string_view("x")), true));
        CHECK(server_addons(profile).back().consent == plugin::Consent::Allowed);
        REQUIRE(plugin::revoke_plugin_consent(profile, "MQ2Srv"));
        CHECK(server_addons(profile).back().consent == plugin::Consent::Denied);
        REQUIRE(plugin::set_plugin_consent(profile, "MQ2Srv", sha256(std::string_view("other bytes")), true));
        CHECK(server_addons(profile).back().consent == plugin::Consent::Unasked); // a yes for other bytes
    }
}

TEST_CASE("addon_install: file hashes are remembered by size and time, and read again when either changes") {
    test::ScratchDir dir("addon_file_hash");
    const fs::path file = dir.path() / "provider.bin";
    const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(1);
    put_file(file, "first bytes");
    fs::last_write_time(file, old_time);
    REQUIRE(addon_file_sha256(file) == sha256("first bytes"));
    CHECK(addon_file_matches(file, 11, sha256("first bytes")));
    CHECK_FALSE(addon_file_matches(file, 12, sha256("first bytes")));

    // Same size and the same time put back: the remembered hash answers, so the file was not read again.
    put_file(file, "other bytes");
    fs::last_write_time(file, old_time);
    CHECK(addon_file_sha256(file) == sha256("first bytes"));
    // Another time, or another size: read again.
    fs::last_write_time(file, old_time + std::chrono::minutes(1));
    CHECK(addon_file_sha256(file) == sha256("other bytes"));
    put_file(file, "longer other bytes");
    fs::last_write_time(file, old_time + std::chrono::minutes(1));
    CHECK(addon_file_sha256(file) == sha256("longer other bytes"));

    // A file written just now is read each time: a rewrite of the same size within the clock's tick still shows.
    const fs::path fresh = dir.path() / "fresh.bin";
    put_file(fresh, "aaaa");
    CHECK(addon_file_sha256(fresh) == sha256("aaaa"));
    put_file(fresh, "bbbb");
    CHECK(addon_file_sha256(fresh) == sha256("bbbb"));
    CHECK_FALSE(addon_file_sha256(dir.path() / "missing.bin"));
}
