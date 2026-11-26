#pragma once

// CHANGELOG.md, shipped beside the client, as the login screen's What's new shows it (docs/updater.md "What the
// player sees"). A Hailstone feature with no original counterpart. The file's sections are "## <version> (<date>)",
// each a list of "- " entries; an entry may go on over indented lines.

#include <string>
#include <string_view>
#include <vector>

namespace hailstone {

// The entries of `version`'s section (its heading's first word is `version`), one a string, without the "- ", the
// lines of an entry joined with spaces and Markdown's backticks taken out. Empty when there is no such section.
std::vector<std::string> changelog_section(std::string_view text, std::string_view version);

// The heading a build's What's new reads: its version, or "Unreleased" for a nightly build (a pre-release label
// starting "nightly", tools/nightly.sh), whose changes since the last release are that section's.
std::string_view changelog_heading(std::string_view version);

// The file's name in the Hailstone folder.
inline constexpr std::string_view kChangelogName = "CHANGELOG.md";

} // namespace hailstone
