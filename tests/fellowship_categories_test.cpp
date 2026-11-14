#include <doctest/doctest.h>

#include <fstream>
#include <set>
#include <span>
#include <string>

#include "core/game_dir.h"
#include "fuzz_support.h"
#include "game/fellowship_categories.h"
#include "game_files.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {
std::span<const std::byte> bytes(const std::string& text) { return std::as_bytes(std::span(text)); }
void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}
}

TEST_CASE("fellowship categories: external labels and bit identifiers are bounded and independent") {
    const std::string text = "# category fixture\r\nCATEGORY_ID^LABEL\n 1 ^ Fixture first \r\n31^Fixture top\n"
        "1^Duplicate\n0^Reserved\n32^Too large\n-1^Negative\n4294967296^Overflow\n2^\n3^Bad^field\n4^Bad\tcontrol\n";
    const auto categories = game::parse_fellowship_categories(bytes(text));
    REQUIRE(categories.size() == 2);
    CHECK(categories[0].id == 1);
    CHECK(categories[0].name == "Fixture first");
    CHECK(categories[0].mask() == 2u);
    CHECK(categories[1].id == 31);
    CHECK(categories[1].name == "Fixture top");
    CHECK(categories[1].mask() == 0x80000000u);
    CHECK(game::FellowshipCategory{}.mask() == 0);
    CHECK((game::FellowshipCategory{32, "Invalid"}.mask()) == 0);
    CHECK(game::parse_fellowship_categories(bytes(std::string("1^Before\0\n2^After", 18))).empty());
    CHECK(game::parse_fellowship_categories(bytes("2^" + std::string(256, 'x'))).empty());
    CHECK(game::parse_fellowship_categories(bytes(std::string(game::kMaxFellowshipCategoryBytes + 1, 'x'))).empty());
}

TEST_CASE("fellowship categories: layered files reload without retaining a previous server catalog") {
    test::ScratchDir scratch("fellowship_categories");
    const auto base = scratch.path() / "base";
    const auto overlay = scratch.path() / "overlay";
    write(base / "Resources" / "CampsiteCategories.txt", "1^Base fixture\n");
    write(overlay / "resources" / "campsitecategories.txt", "3^Overlay fixture\n");
    GameDir folder(base);
    auto result = game::load_fellowship_categories(folder);
    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "Base fixture");
    folder.set_overlay(overlay);
    result = game::load_fellowship_categories(folder);
    REQUIRE(result.size() == 1);
    CHECK(result[0].id == 3);
    CHECK(result[0].name == "Overlay fixture");
    write(overlay / "resources" / "campsitecategories.txt", std::string(game::kMaxFellowshipCategoryBytes + 1, 'x'));
    CHECK(game::load_fellowship_categories(folder).empty());
    folder.set_overlay(std::nullopt);
    CHECK(game::load_fellowship_categories(folder).size() == 1);
    CHECK(game::load_fellowship_categories(GameDir{}).empty());
}

TEST_CASE("fellowship categories: truncated and mutated metadata remains bounded") {
    const std::string text = "# fixture\n1^One\n2^Two\n31^Top\n";
    fuzz::Bytes seed(bytes(text).begin(), bytes(text).end());
    fuzz::Rng random(0xca7e90);
    for (std::size_t size = 0; size <= seed.size(); ++size)
        CHECK_NOTHROW(game::parse_fellowship_categories(std::span(seed).first(size)));
    for (int i = 0; i < 300; ++i) {
        const auto mutated = fuzz::mutate(seed, random);
        const auto parsed = game::parse_fellowship_categories(mutated);
        REQUIRE(parsed.size() <= 31);
        for (const auto& category : parsed) {
            CHECK(category.mask() != 0);
            CHECK(category.name.size() <= 255);
        }
    }
}

TEST_CASE("fellowship categories: installed catalog supplies the player's labels" * doctest::skip(!test::game_dir())) {
    const auto result = game::load_fellowship_categories(*test::game_dir());
    REQUIRE_FALSE(result.empty());
    CHECK(result.size() <= 31);
    std::set<std::uint32_t> identifiers;
    for (const auto& category : result) {
        CHECK(category.mask() != 0);
        CHECK_FALSE(category.name.empty());
        CHECK(identifiers.insert(category.id).second);
    }
}
