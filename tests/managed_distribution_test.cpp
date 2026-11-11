#include <doctest/doctest.h>

#include "core/distribution.h"

TEST_CASE("managed distribution cannot be relaxed by a second initialization") {
    using namespace hailstone;
    REQUIRE(distribution::managed());
    const distribution::UiSource edited{"modern/edited.lua", sha256("unapproved replacement UI")};
    distribution::enable_managed(std::span(&edited, 1));
    CHECK_FALSE(distribution::ui_source_allowed("modern/edited.lua", "unapproved replacement UI"));
}

TEST_CASE("managed distribution distinguishes developer drivers from stock command names") {
    using hailstone::distribution::automation_option;
    for (auto option : {"--auto-cast", "--auto-commands", "--auto-walk", "--input-script", "--melee-test", "--move-test"})
        CHECK(automation_option(option));
    for (auto option : {"--server", "--character", "--game-dir", "/melody", "/autofire", "/follow", "/pause", "/doability"})
        CHECK_FALSE(automation_option(option));
}
