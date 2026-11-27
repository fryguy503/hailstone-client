// CHANGELOG.md's section for a version (core/changelog.h): the login screen's What's new.

#include <doctest/doctest.h>

#include <fstream>
#include <random>
#include <sstream>

#include "app/updater.h"
#include "core/build_info.h"
#include "core/changelog.h"

using hailstone::changelog_heading;
using hailstone::changelog_section;

TEST_CASE("changelog_section takes the version's entries") {
    const char* text = "# Changelog\n"
                       "\n"
                       "## 0.2.0-alpha (2026-11-02)\n"
                       "\n"
                       "- Newest thing\n"
                       "- A `/nav` fix that goes on\n"
                       "  over a second line\r\n"
                       "\n"
                       "## 0.1.0-alpha (2026-11-01)\n"
                       "\n"
                       "- Older thing\n";
    const auto now = changelog_section(text, "0.2.0-alpha");
    REQUIRE(now.size() == 2);
    CHECK(now[0] == "Newest thing");
    CHECK(now[1] == "A /nav fix that goes on over a second line");
    const auto old = changelog_section(text, "0.1.0-alpha");
    REQUIRE(old.size() == 1);
    CHECK(old[0] == "Older thing");
}

TEST_CASE("changelog_section: no such version, a prefix of one, or no file") {
    const char* text = "## 0.2.0-alpha (2026-11-02)\n- a\n";
    CHECK(changelog_section(text, "0.2.0").empty()); // the heading's whole first word, not its start
    CHECK(changelog_section(text, "0.3.0-alpha").empty());
    CHECK(changelog_section(text, "").empty());
    CHECK(changelog_section("", "0.2.0-alpha").empty());
    CHECK(changelog_section("## 0.2.0-alpha", "0.2.0-alpha").empty());
}

TEST_CASE("changelog_section: the repository's CHANGELOG.md has this release's section") {
    // What a release ships (CMake installs CHANGELOG.md): a release whose section is missing would show no notes.
    std::ifstream in(HAILSTONE_SOURCE_DIR "/CHANGELOG.md", std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    CHECK_FALSE(hailstone::build_changelog(text.str(), hailstone::release_version(),
                                          hailstone::app::launcher_manages_updates()).empty());
}

TEST_CASE("changelog_heading: a nightly reads the unreleased section, a release its own") {
    CHECK(changelog_heading("0.7.3-beta") == "0.7.3-beta");
    CHECK(changelog_heading("1.0.0") == "1.0.0");
    CHECK(changelog_heading("0.7.3-nightly.20261126") == "Unreleased");
    CHECK(changelog_heading("0.7.3-beta.nightly") == "0.7.3-beta.nightly");
    const char* text = "## Unreleased\n- new\n## 0.7.3-beta (2026-11-23)\n- old\n";
    CHECK(changelog_section(text, changelog_heading("0.7.3-nightly.20261126")) == std::vector<std::string>{"new"});
    CHECK(hailstone::build_changelog(text, "0.7.4-bastion.1", true) == std::vector<std::string>{"new"});
    CHECK(hailstone::build_changelog(text, "0.7.3-beta", true) == std::vector<std::string>{"old"});
    CHECK(hailstone::build_changelog(text, "0.7.4-bastion.1", false).empty());
}

TEST_CASE("changelog_section: random and cut-off input reads without a crash") {
    std::mt19937 rng(7);
    const std::string base = "## 1.0.0 (x)\n- one `a`\n  two\n- three\n## 0.9.0\n- four\n";
    for (int i = 0; i < 2000; ++i) {
        std::string text = base.substr(0, rng() % (base.size() + 1));
        for (int k = 0, n = static_cast<int>(rng() % 4); k < n && !text.empty(); ++k)
            text[rng() % text.size()] = static_cast<char>(rng() % 256);
        const auto out = changelog_section(text, "1.0.0");
        CHECK(out.size() <= 10); // (at most a line each, a few made by the changed bytes)
    }
}
