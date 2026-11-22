#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string_view>

#include "core/character_import_source.h"
#include "platform/read_only_directory.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {

constexpr std::string_view kSettings = "[Socials]\nPage1Button1Name=Assist\n";

void write_source(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    REQUIRE(out.good());
}

} // namespace

TEST_CASE("Character import inventory groups recognized files by character and world") {
    test::ScratchDir scratch("import_inventory");
    const auto root = fs::canonical(scratch.path());
    fs::create_directory(root / "USERDATA");
    write_source(root / "HailstoneUser_Main.ini", kSettings);
    write_source(root / "ui_HailstoneUser_Main.INI", "[PlayerWindow]\n");
    write_source(root / "BZR_HailstoneUser_Main.ini", "");
    write_source(root / "USERDATA" / "BART_HailstoneUser_Main.ini", "");
    write_source(root / "USERDATA" / "HB_0_HailstoneUser_Main.ini", "");
    write_source(root / "USERDATA" / "HB_bad_HailstoneUser_Main.ini", "");
    write_source(root / "USERDATA" / "HailstoneUser_Overflow.ini", kSettings);
    write_source(root / "UI_HailstoneUser_Second Realm.ini", "");
    write_source(root / "HailstoneOther_Main.ini", "\xef\xbb\xbf  [HotButtons10]\r\n");
    write_source(root / "HailstoneOnly_More_Data.ini", "[Abilities]\n");
    write_source(root / "HailstoneUnknown_Main.ini", "[Unrelated]\n");
    write_source(root / "eqclient.ini", kSettings);

    const auto inventory = discover_character_import_sources(root);
    CHECK(inventory.error.empty());
    REQUIRE(inventory.sources.size() == 4);
    CHECK(inventory.sources[0].world == "More_Data");
    CHECK(inventory.sources[1].character == "HailstoneOther");
    const auto& main = inventory.sources[2];
    CHECK(main.character == "HailstoneUser");
    CHECK(main.world == "Main");
    REQUIRE(main.files.size() == 5);
    CHECK(main.files[0].kind == CharacterImportFileKind::Settings);
    CHECK(main.files[0].name == "HailstoneUser_Main.ini");
    CHECK(main.files[1].name == "ui_HailstoneUser_Main.INI");
    CHECK(main.files[2].kind == CharacterImportFileKind::Trader);
    CHECK(main.files[3].name == "USERDATA/BART_HailstoneUser_Main.ini");
    CHECK(main.files[4].name == "USERDATA/HB_0_HailstoneUser_Main.ini");
    CHECK(inventory.sources[3].world == "Second Realm");
    REQUIRE(inventory.sources[3].files.size() == 1);
    CHECK(inventory.sources[3].files[0].kind == CharacterImportFileKind::Layout);

    std::string error;
    const auto bytes = read_character_import_source(root, main, error);
    REQUIRE(bytes);
    CHECK(*bytes == kSettings);
    CHECK(error.empty());
    CHECK_FALSE(read_character_import_source(root, inventory.sources[3], error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Character import source rechecks the exact selected file and bounds reads") {
    test::ScratchDir scratch("import_source");
    const auto root = fs::canonical(scratch.path());
    const auto path = root / "HailstoneUser_Main.ini";
    write_source(path, kSettings);
    const auto inventory = discover_character_import_sources(root);
    REQUIRE(inventory.sources.size() == 1);
    const auto& source = inventory.sources[0];
    std::string error;

    write_source(path, "[HotButtons]\n");
    const auto changed = read_character_import_source(root, source, error);
    REQUIRE(changed);
    CHECK(*changed == "[HotButtons]\n");
    CHECK(error.empty());
    fs::resize_file(path, kCharacterImportMaxBytes + 1);
    CHECK_FALSE(read_character_import_source(root, source, error));
    CHECK_FALSE(error.empty());
    fs::remove(path);
    CHECK_FALSE(read_character_import_source(root, source, error));
    auto invalid = source;
    invalid.files[0].name = "../HailstoneUser_Main.ini";
    CHECK_FALSE(read_character_import_source(root, invalid, error));
    invalid = source;
    invalid.world = "Other";
    CHECK_FALSE(read_character_import_source(root, invalid, error));
}

TEST_CASE("Character import refuses link escapes and case-ambiguous settings files") {
    test::ScratchDir scratch("import_links");
    const auto root = fs::canonical(scratch.path());
    const auto game = root / "Data";
    const auto outside = root / "Outside";
    fs::create_directory(game);
    fs::create_directory(outside);
    const auto source_path = game / "HailstoneUser_Main.ini";
    write_source(source_path, kSettings);
    const auto original = discover_character_import_sources(game);
    REQUIRE(original.sources.size() == 1);
    const auto linked_file = outside / "HailstoneUser_Main.ini";
    write_source(linked_file, kSettings);
    write_source(outside / "BART_HailstoneOther_Main.ini", "");
    std::string error;
    std::error_code ec;

    const auto duplicate = game / "HAILSTONEUSER_MAIN.INI";
    if (!fs::exists(duplicate)) {
        write_source(duplicate, kSettings);
        CHECK(discover_character_import_sources(game).sources.empty());
        CHECK_FALSE(read_character_import_source(game, original.sources[0], error));
        fs::remove(duplicate);
    }

    fs::remove(source_path);
    fs::create_hard_link(linked_file, source_path, ec);
    if (!ec) {
        CHECK(discover_character_import_sources(game).sources.empty());
        CHECK_FALSE(read_character_import_source(game, original.sources[0], error));
        fs::remove(source_path);
    }
    ec.clear();
    fs::create_symlink(linked_file, source_path, ec);
    if (!ec) {
        CHECK(discover_character_import_sources(game).sources.empty());
        CHECK_FALSE(read_character_import_source(game, original.sources[0], error));
        fs::remove(source_path);
    }
    ec.clear();
    fs::create_directory_symlink(outside, game / "UserData", ec);
    if (!ec) {
        CHECK(discover_character_import_sources(game).sources.empty());
        // A link as the folder itself is the folder chosen, and is followed (a Wine dosdevices path, a linked /home).
        const auto linked_root = discover_character_import_sources(game / "UserData");
        CHECK(linked_root.error.empty());
        CHECK(linked_root.sources.size() == 1);
        CHECK(read_character_import_source(game / "UserData", original.sources[0], error));
    }
}

TEST_CASE("Read-only directory scans stop at their entry limit") {
    test::ScratchDir scratch("import_directory");
    const auto root = fs::canonical(scratch.path());
    write_source(root / "one.ini", "");
    write_source(root / "two.ini", "");
    auto directory = platform::ReadOnlyDirectory::open(root);
    REQUIRE(directory);
    CHECK_FALSE(directory->list(1));
    auto entries = directory->list(2);
    REQUIRE(entries);
    CHECK(entries->size() == 2);
    CHECK_FALSE(directory->read("../one.ini", 100));
    CHECK_FALSE(directory->child(".."));
}
