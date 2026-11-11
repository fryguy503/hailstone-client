#pragma once

#include <span>
#include <string_view>

#include "core/sha256.h"

namespace hailstone::distribution {

// A managed executable enables this before it parses options or loads code.
// This is a one-way process policy, separate from editable settings and server rules.
struct UiSource {
    std::string_view path;
    Sha256Digest digest;
};
void enable_managed(std::span<const UiSource> approved_ui_sources);
bool managed();

// Executable UI sources in a managed distribution must match its compiled manifest.
// Styles, window positions and other data settings remain editable.
bool ui_source_allowed(std::string_view logical_path, std::string_view source);
// Developer input/command drivers are not part of a managed player's executable interface.
bool automation_option(std::string_view option);

} // namespace hailstone::distribution
