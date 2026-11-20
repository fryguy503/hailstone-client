// The user interfaces (ui/ui_packs.h): the folders with a ui.ini, their fields, and which one ui.name picks.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "ui/ui_packs.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using namespace hailstone;

TEST_CASE("UI packs: folders with a ui.ini, chosen by name with modern as the fallback") {
    hailstone::test::ScratchDir dir_scratch("ui_packs_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "modern");
    fs::create_directories(dir / "pad");
    fs::create_directories(dir / "not_a_ui");
    std::ofstream(dir / "modern" / "ui.ini") << "# comment\nname=Modern\ndescription=New\ninput=mouse\ngame_art=icons\n";
    std::ofstream(dir / "pad" / "ui.ini") << "name=Gamepad\ninput=gamepad,touch\ngame_art=all\n";
    std::ofstream(dir / "not_a_ui" / "readme.txt") << "x";

    auto packs = ui::list_ui_packs(dir);
    REQUIRE(packs.size() == 2);
    CHECK(packs[1].id == "modern"); // by name: Gamepad, Modern
    CHECK(packs[1].description == "New");
    CHECK_FALSE(packs[1].game_art);
    CHECK(packs[0].name == "Gamepad");
    CHECK(packs[0].input == "gamepad,touch");
    CHECK(packs[0].game_art);

    CHECK(ui::find_ui_pack(packs, "pad")->id == "pad");
    CHECK(ui::find_ui_pack(packs, "missing")->id == "modern");
    CHECK(ui::find_ui_pack({}, "modern") == nullptr);
    CHECK(ui::list_ui_packs(dir / "nowhere").empty());
    // the shipped UIs: modern and gamepad (the Classic UI is gone; a name of it gets the default)
    auto shipped = ui::list_ui_packs(fs::path(__FILE__).parent_path().parent_path() / "ui");
    CHECK(ui::find_ui_pack(shipped, "modern")->id == "modern");
    CHECK(ui::find_ui_pack(shipped, "classic")->id == "modern");
    fs::remove_all(dir);
}

TEST_CASE("UI packs: declared bases inherit metadata and reject invalid chains") {
    test::ScratchDir scratch("ui_pack_bases");
    const auto root = scratch.path() / "ui";
    auto pack = [&](const char* id, const char* ini) {
        fs::create_directories(root / id);
        std::ofstream(root / id / "ui.ini") << ini;
    };
    pack("modern", "name=Modern\ninput=mouse\ngame_art=all\n");
    pack("middle", "base=modern\ninput=gamepad\n");
    pack("leaf", "base=middle\n");
    pack("explicit", "base=leaf\ninput=touch\ngame_art=icons\n");
    pack("missing", "base=absent\n");
    pack("cycle-a", "base=cycle-b\n");
    pack("cycle-b", "base=cycle-a\n");
    pack("escape", "base=../modern\n");
    std::vector<std::string> diagnostics;
    const auto packs = ui::list_ui_packs(root, &diagnostics);
    REQUIRE(packs.size() == 4);
    CHECK(diagnostics.size() == 4);
    const auto* leaf = ui::find_ui_pack(packs, "leaf");
    REQUIRE(leaf);
    REQUIRE(leaf->id == "leaf");
    CHECK(leaf->input == "gamepad");
    CHECK(leaf->game_art);
    CHECK(leaf->base_dirs == std::vector<fs::path>{root / "middle", root / "modern"});
    const auto* own = ui::find_ui_pack(packs, "explicit");
    REQUIRE(own);
    REQUIRE(own->id == "explicit");
    CHECK(own->input == "touch");
    CHECK_FALSE(own->game_art);
    CHECK(own->name == "explicit");
    CHECK(own->description.empty());

    const auto plugin = scratch.path() / "plugin";
    fs::create_directories(plugin / "ui" / "modern");
    CHECK(ui::plugin_ui_folder(plugin, *leaf) == plugin / "ui" / "modern");
    fs::create_directories(plugin / "ui" / "middle");
    CHECK(ui::plugin_ui_folder(plugin, *leaf) == plugin / "ui" / "middle");
    fs::create_directories(plugin / "ui" / "leaf"); // an empty selected folder is intentional
    CHECK(ui::plugin_ui_folder(plugin, *leaf) == plugin / "ui" / "leaf");
}

TEST_CASE("UI packs: bounded chains leave shorter valid bases selectable") {
    test::ScratchDir scratch("ui_pack_base_limit");
    for (int i = 0; i < 34; ++i) {
        const auto dir = scratch.path() / ("pack" + std::to_string(i));
        fs::create_directories(dir);
        std::ofstream ini(dir / "ui.ini");
        if (i != 33) ini << "base=pack" << i + 1 << '\n';
    }
    std::vector<std::string> diagnostics;
    const auto packs = ui::list_ui_packs(scratch.path(), &diagnostics);
    CHECK(packs.size() == 32);
    CHECK(diagnostics.size() == 2);
    const auto* first_valid = ui::find_ui_pack(packs, "pack2");
    REQUIRE(first_valid);
    REQUIRE(first_valid->id == "pack2");
    CHECK(first_valid->base_dirs.size() == 31);
    CHECK(std::none_of(packs.begin(), packs.end(), [](const ui::UiPack& pack) {
        return pack.id == "pack0" || pack.id == "pack1";
    }));
}

TEST_CASE("UI packs: installed bundles have owned unique IDs and approved cross-root bases") {
    test::ScratchDir scratch("ui_pack_managed");
    const auto builtin = scratch.path() / "ui", managed = scratch.path() / "ui-addons";
    auto pack = [&](const fs::path& dir, const char* ini) {
        fs::create_directories(dir);
        std::ofstream(dir / "ui.ini") << ini;
    };
    pack(builtin / "modern", "name=Modern\ninput=mouse\ngame_art=all\n");
    pack(managed / "one/ui/modern", "base=builtin:modern\ninput=gamepad\n");
    pack(managed / "one/ui/leaf", "base=modern\n");
    pack(managed / "two/ui/leaf", "base=addon.one.leaf\ngame_art=icons\n");
    pack(managed / "not-installed/ui/hidden", "base=modern\n");
    std::vector<std::string> diagnostics;
    const auto packs = ui::list_ui_packs(builtin, managed, {"one", "two"}, &diagnostics);
    REQUIRE(packs.size() == 4);
    CHECK(diagnostics.empty());
    const auto* leaf = ui::find_ui_pack(packs, "addon.two.leaf");
    REQUIRE(leaf);
    REQUIRE(leaf->id == "addon.two.leaf");
    CHECK(leaf->source == "addon");
    CHECK(leaf->addon == "two");
    CHECK(leaf->local_id == "leaf");
    CHECK(leaf->input == "gamepad");
    CHECK_FALSE(leaf->game_art);
    CHECK(leaf->base_ids == std::vector<std::string>{"addon.one.leaf", "addon.one.modern", "modern"});
    CHECK(leaf->base_dirs == std::vector<fs::path>{managed / "one/ui/leaf", managed / "one/ui/modern", builtin / "modern"});
    CHECK(leaf->id.find_first_of("/\\:") == std::string::npos); // also the selected pack's settings-file key
    const auto plugin = scratch.path() / "plugin";
    fs::create_directories(plugin / "ui/addon.one.leaf");
    CHECK(ui::plugin_ui_folder(plugin, *leaf) == plugin / "ui/addon.one.leaf");
    fs::create_directories(plugin / "ui/addon.two.leaf");
    CHECK(ui::plugin_ui_folder(plugin, *leaf) == plugin / "ui/addon.two.leaf");

    const auto removed = ui::list_ui_packs(builtin, managed, {"two"}, &diagnostics);
    REQUIRE(removed.size() == 1); // an uninstalled owner's folders authorize neither it nor a dependent pack
    CHECK(removed.front().id == "modern");
    CHECK(ui::find_ui_pack(removed, "addon.two.leaf")->id == "modern");
}

TEST_CASE("UI packs: managed names cannot shadow builtins or escape registered owners") {
    test::ScratchDir scratch("ui_pack_managed_invalid");
    const auto builtin = scratch.path() / "ui", managed = scratch.path() / "ui-addons";
    auto pack = [&](const fs::path& dir, const char* ini) {
        fs::create_directories(dir);
        std::ofstream(dir / "ui.ini") << ini;
    };
    pack(builtin / "modern", "name=Modern\n");
    pack(builtin / "addon.one.leaf", "name=Spoofed owner\n");
    pack(managed / "one/ui/leaf", "base=modern\n");
    pack(managed / "one/ui/cycle", "base=addon.two.cycle\n");
    pack(managed / "two/ui/cycle", "base=addon.one.cycle\n");
    pack(managed / "one/ui/bad.name", "base=modern\n");
    pack(managed / "one/ui/escape", "base=../modern\n");
    std::vector<std::string> diagnostics;
    const auto packs = ui::list_ui_packs(builtin, managed, {"one", "two", "../outside"}, &diagnostics);
    REQUIRE(packs.size() == 2);
    CHECK(ui::find_ui_pack(packs, "addon.one.leaf")->source == "addon");
    CHECK(diagnostics.size() == 6);
    const auto duplicate = ui::list_ui_packs(builtin, managed, {"one", "one"});
    REQUIRE(duplicate.size() == 1);
    CHECK(duplicate.front().id == "modern");
}

TEST_CASE("UI packs: unsupported runtimes remain unavailable and require valid bases") {
    test::ScratchDir scratch("ui_pack_unavailable_runtime");
    const auto builtin = scratch.path() / "ui", managed = scratch.path() / "ui-addons";
    fs::create_directories(builtin / "modern");
    fs::create_directories(managed / "bundle/ui/future");
    fs::create_directories(managed / "bundle/ui/missingbase");
    std::ofstream(builtin / "modern/ui.ini") << "name=Modern\n";
    std::ofstream(managed / "bundle/ui/future/ui.ini") << "name=Future interface\nbase=builtin:modern\nruntime=future-runtime\n";
    std::ofstream(managed / "bundle/ui/missingbase/ui.ini") << "base=builtin:absent\nruntime=future-runtime\n";
    std::vector<std::string> diagnostics;
    const auto packs = ui::list_ui_packs(builtin, managed, {"bundle"}, &diagnostics);
    REQUIRE(packs.size() == 2);
    const auto* future = ui::find_ui_pack_exact(packs, "addon.bundle.future");
    REQUIRE(future);
    CHECK_FALSE(future->available);
    CHECK(future->runtime == "future-runtime");
    CHECK(future->unavailable_reason == "Unsupported interface runtime: future-runtime");
    CHECK(future->base_dirs == std::vector<fs::path>{builtin / "modern"});
    CHECK(future->base_ids == std::vector<std::string>{"modern"});
    CHECK(ui::find_ui_pack(packs, future->id)->id == "modern");
    CHECK(ui::find_ui_pack_exact(packs, "addon.bundle.missingbase") == nullptr);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().find("missing or invalid base") != std::string::npos);
    CHECK(ui::find_ui_pack_exact(packs, "absent") == nullptr);
    CHECK(ui::find_ui_pack({*future}, future->id) == nullptr);
}

TEST_CASE("UI packs: a registered managed owner cannot link its root outside the managed tree") {
    test::ScratchDir scratch("ui_pack_managed_owner_link");
    const auto builtin = scratch.path() / "ui", managed = scratch.path() / "ui-addons", outside = scratch.path() / "outside";
    fs::create_directories(builtin / "modern");
    fs::create_directories(managed);
    fs::create_directories(outside / "ui/escape");
    std::ofstream(builtin / "modern/ui.ini") << "name=Modern\n";
    std::ofstream(outside / "ui/escape/ui.ini") << "base=modern\n";
    std::error_code ec;
    fs::create_directory_symlink(outside, managed / "linked", ec);
    // (Wine makes something that is not a link and reports success.)
    if (ec || !fs::is_symlink(managed / "linked")) { MESSAGE("Directory link creation is unavailable on this host"); return; }
    std::vector<std::string> diagnostics;
    const auto packs = ui::list_ui_packs(builtin, managed, {"linked"}, &diagnostics);
    REQUIRE(packs.size() == 1);
    CHECK(packs.front().id == "modern");
    CHECK_FALSE(diagnostics.empty());
}

TEST_CASE("UI packs: directory links cannot approve an outside base or plugin folder") {
    test::ScratchDir scratch("ui_pack_base_links");
    const auto root = scratch.path() / "ui", outside = scratch.path() / "outside";
    fs::create_directories(root / "modern");
    fs::create_directories(outside);
    std::ofstream(root / "modern" / "ui.ini") << "name=Modern\n";
    std::ofstream(outside / "ui.ini") << "name=Outside\n";
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "linked", ec);
    // (Wine makes something that is not a link and reports success.)
    if (ec || !fs::is_symlink(root / "linked")) { MESSAGE("Directory link creation is unavailable on this host"); return; }
    std::vector<std::string> diagnostics;
    const auto packs = ui::list_ui_packs(root, &diagnostics);
    REQUIRE(packs.size() == 1);
    CHECK(packs.front().id == "modern");
    CHECK_FALSE(diagnostics.empty());

    ui::UiPack derived;
    derived.id = "leaf";
    derived.base_dirs = {root / "modern"};
    const auto plugin = scratch.path() / "plugin";
    fs::create_directories(plugin / "ui" / "modern");
    fs::create_directory_symlink(outside, plugin / "ui" / "leaf", ec);
    REQUIRE_FALSE(ec);
    CHECK(ui::plugin_ui_folder(plugin, derived) == plugin / "ui" / "modern");
}
