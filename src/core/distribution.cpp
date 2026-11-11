#include "core/distribution.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace hailstone::distribution {
namespace {
std::atomic_bool restricted{false};
std::once_flag configured;
struct ApprovedUi { std::string path; Sha256Digest digest; };
std::vector<ApprovedUi> approved_ui;
}

void enable_managed(std::span<const UiSource> sources) {
    std::call_once(configured, [&] {
        for (const auto& source : sources) approved_ui.push_back({std::string(source.path), source.digest});
        restricted.store(true, std::memory_order_release);
    });
}

bool managed() { return restricted.load(std::memory_order_acquire); }

bool ui_source_allowed(std::string_view path, std::string_view source) {
    if (!managed()) return true;
    const auto digest = sha256(source);
    return std::any_of(approved_ui.begin(), approved_ui.end(), [&](const auto& allowed) {
        return allowed.path == path && allowed.digest == digest;
    });
}

bool automation_option(std::string_view option) {
    if (option.starts_with("--auto-")) return true;
    for (const auto name : {"--input-script", "--watch", "--watch-turn", "--pick-test", "--melee-test",
                            "--abilities-test", "--move-test", "--show-items"})
        if (option == name) return true;
    return false;
}

} // namespace hailstone::distribution
