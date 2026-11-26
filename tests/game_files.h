#pragma once

// Tests that need the real game files run only when EQ_GAME_DIR points at a
// RoF2 installation; otherwise they are skipped.
//
// Any game folder may be set: a test whose file is missing skips (a MESSAGE and
// a return), never fails. Some tests pin exact values (counts, texture names,
// string text) taken from one folder, the NMS server's client folder; those
// checks run only when reference_folder() recognises it, and every folder gets
// the general checks (non-empty, well-formed, in range).

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "core/game_dir.h"

namespace hailstone::test {

inline const GameDir* game_dir() {
    static std::unique_ptr<GameDir> dir = [] {
        const char* env = std::getenv("EQ_GAME_DIR");
        return env && *env ? std::make_unique<GameDir>(env) : nullptr;
    }();
    return dir.get();
}

// True when EQ_GAME_DIR is the folder the exact values in some tests were taken
// from: the NMS server's client folder, recognised by the equipment archive only
// that server ships (gequip_nms.s3d). A stock RoF2 or live install has no such file.
inline bool reference_folder() {
    static const bool yes = game_dir() && game_dir()->exists("gequip_nms.s3d");
    return yes;
}

// The game file `relative`, or nullopt with a MESSAGE when the folder has none: the caller returns, and the test
// passes without checking it. A stock live install lacks many classic archives (bazaar.s3d, velketor_chr.s3d).
inline std::optional<std::filesystem::path> game_file(std::string_view relative) {
    auto path = game_dir() ? game_dir()->find(relative) : std::nullopt;
    if (!path) MESSAGE("EQ_GAME_DIR has no " << std::string(relative) << ": the check was skipped");
    return path;
}

// Whether to run checks of values taken from the reference folder; when not, says so with a MESSAGE.
inline bool pinned_values(std::string_view what) {
    if (reference_folder()) return true;
    MESSAGE("EQ_GAME_DIR is not the reference folder: " << std::string(what) << " not checked");
    return false;
}

} // namespace hailstone::test
