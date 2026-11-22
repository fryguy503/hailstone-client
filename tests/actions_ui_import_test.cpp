#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "app/actions_ui.h"
#include "core/atomic_file.h"
#include "core/config.h"
#include "scratch_dir.h"

namespace {
using namespace hailstone;
namespace fs = std::filesystem;

constexpr std::string_view kIncoming = "[Socials]\nPage1Button1Name=Ready\nPage1Button1Line1=/say Ready.\n";
constexpr std::string_view kOriginalSettings = "# Previous settings\r\n[Other]\r\nKeep=123\r\n";
constexpr game::CharacterImportOptions kSocials{.socials = true};

struct ImportFixture {
    test::ScratchDir dir{"action_import"};
    Config config;
    fs::path target = fs::canonical(dir.path()) / "settings" / "hotbuttons.ini";
    std::unique_ptr<app::ActionsUi> actions = std::make_unique<app::ActionsUi>(config, dir.path() / "hailstone.ini");
};

std::string contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

TEST_CASE("Character import refuses a missing target under a linked character directory") {
    ImportFixture fixture;
    const auto other_character = fixture.target.parent_path().parent_path() / "other";
    fs::create_directories(other_character);
    std::error_code ec;
    fs::create_directory_symlink(other_character, fixture.target.parent_path(), ec);
    if (ec) return;

    fixture.actions->enter_zone(fixture.target);
    const auto preview = fixture.actions->preview_import(kIncoming, kSocials);
    CHECK_FALSE(preview.plan.error.empty());
    CHECK_FALSE(fs::exists(other_character / "hotbuttons.ini"));
    CHECK_FALSE(fs::exists(other_character / "import-backups"));
}
}

TEST_CASE("Character import works when the path to the character directory passes through a link") {
    // A linked ancestor (a /home that is a link, a games folder on another disk) is not an escape.
    ImportFixture fixture;
    const auto real = fixture.target.parent_path().parent_path() / "real";
    fs::create_directories(real / "settings");
    std::error_code ec;
    fs::create_directory_symlink(real, fixture.target.parent_path().parent_path() / "linked", ec);
    if (ec) return;
    const auto target = fixture.target.parent_path().parent_path() / "linked" / "settings" / "hotbuttons.ini";
    REQUIRE(write_file_atomic(real / "settings" / "hotbuttons.ini", kOriginalSettings));

    fixture.actions->enter_zone(target);
    const auto preview = fixture.actions->preview_import(kIncoming, kSocials);
    CHECK(preview.plan.error.empty());
    CHECK(preview.plan.additions == 1);
    std::string error;
    CHECK(fixture.actions->apply_import(preview, kIncoming, kSocials, error));
    CHECK(error.empty());
    CHECK(fixture.actions->has_import_backup());
}

TEST_CASE("Character import preview is read-only and can restore a persisted revision after reload") {
    ImportFixture fixture;
    REQUIRE(write_file_atomic(fixture.target, kOriginalSettings));
    fixture.actions->enter_zone(fixture.target);

    const auto preview = fixture.actions->preview_import(kIncoming, kSocials);
    REQUIRE(preview.plan.error.empty());
    REQUIRE(preview.plan.additions == 1);
    CHECK(contents(fixture.target) == kOriginalSettings);
    CHECK(fixture.actions->hot_buttons().social(0).empty());
    CHECK_FALSE(fs::exists(fixture.target.parent_path() / "import-backups"));

    std::string error;
    REQUIRE_MESSAGE(fixture.actions->apply_import(preview, kIncoming, kSocials, error), error);
    CHECK(fixture.actions->hot_buttons().social(0).name == "Ready");
    CHECK(fixture.actions->current_social() < 0);
    CHECK(fixture.actions->char_ini_string("Other", "Keep") == "123");
    CHECK_FALSE(fixture.actions->apply_import(preview, kIncoming, kSocials, error));

    auto fresh = std::make_unique<app::ActionsUi>(fixture.config, fixture.dir.path() / "hailstone.ini");
    fresh->enter_zone(fixture.target);
    REQUIRE(fresh->has_import_backup());
    REQUIRE_MESSAGE(fresh->restore_import(error), error);
    CHECK(fresh->hot_buttons().social(0).empty());
    CHECK(contents(fixture.target) == kOriginalSettings);
    CHECK_FALSE(fresh->has_import_backup());
}

TEST_CASE("Character import refuses a changed revision or an unavailable backup location") {
    ImportFixture fixture;
    REQUIRE(write_file_atomic(fixture.target, kOriginalSettings));
    fixture.actions->enter_zone(fixture.target);
    const auto preview = fixture.actions->preview_import(kIncoming, kSocials);
    REQUIRE(preview.plan.additions == 1);

    std::string error;
    SUBCASE("settings edited after review") {
        fixture.actions->write_char_ini("Other", "New", "Saved");
        CHECK_FALSE(fixture.actions->apply_import(preview, kIncoming, kSocials, error));
        CHECK(fixture.actions->char_ini_string("Other", "New") == "Saved");
    }
    SUBCASE("backup path obstructed") {
        REQUIRE(write_file_atomic(fixture.target.parent_path() / "import-backups", "blocked"));
        CHECK_FALSE(fixture.actions->apply_import(preview, kIncoming, kSocials, error));
        CHECK(contents(fixture.target) == kOriginalSettings);
    }
    CHECK(fixture.actions->hot_buttons().social(0).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Character import protects unrecognized scripted social files and subsequent edits") {
    ImportFixture fixture;
    fixture.actions->enter_zone(fixture.target);
    const auto script = fixture.target.parent_path() / "socials" / "1.lua";
    REQUIRE(write_file_atomic(script, "malformed or unavailable script"));
    const auto blocked = fixture.actions->preview_import(kIncoming, kSocials);
    CHECK(blocked.plan.error.empty());
    CHECK(blocked.plan.additions == 0);

    constexpr std::string_view another_slot = "[Socials]\nPage1Button2Name=Ready\nPage1Button2Line1=/say Ready.\n";
    const auto preview = fixture.actions->preview_import(another_slot, kSocials);
    REQUIRE(preview.plan.additions == 1);
    std::string error;
    REQUIRE_MESSAGE(fixture.actions->apply_import(preview, another_slot, kSocials, error), error);
    CHECK(contents(script) == "malformed or unavailable script");
    REQUIRE(fixture.actions->has_import_backup());
    SUBCASE("a later settings edit") {
        fixture.actions->write_char_ini("Other", "New", "Saved");
        CHECK_FALSE(fixture.actions->has_import_backup());
        CHECK_FALSE(fixture.actions->restore_import(error));
        CHECK(fixture.actions->char_ini_string("Other", "New") == "Saved");
    }
    SUBCASE("a newly protected social slot") {
        const auto added_script = script.parent_path() / "2.lua";
        REQUIRE(write_file_atomic(added_script, "protected slot"));
        CHECK_FALSE(fixture.actions->restore_import(error));
        CHECK(fixture.actions->hot_buttons().social(1).name == "Ready");
        CHECK(contents(added_script) == "protected slot");
    }
}
