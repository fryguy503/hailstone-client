// Headless launcher entry point. It uses the native release format and transaction, but the launcher owns
// the channel, trust root and ticket. No credentials or game files go through this process.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "app/update_install.h"
#include "app/updater.h"
#include "core/log.h"
#include "platform/http_get.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {
void line(const char* key, std::string_view value) {
    if (value.empty()) return;
    std::string safe(value);
    for (char& c : safe)
        if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    std::printf("%s=%s\n", key, safe.c_str());
    std::fflush(stdout);
}

int result(std::string_view status, std::string_view version = {}, std::string_view message = {},
           std::string_view current = {}) {
    line("version", version);
    line("current_version", current);
    line("message", message);
    line("status", status);
    return status == "error" ? 1 : 0;
}

fs::path utf8_path(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

bool safe_directory(const fs::path& dir) {
    if (!platform::ordinary_path(dir, true)) return false;
    std::error_code ec;
    const bool exists = fs::exists(dir, ec);
    return !ec && (!exists || fs::is_directory(dir, ec));
}

bool game_directory(const fs::path& dir) {
    for (auto candidate = dir; !candidate.empty();) {
        for (const auto* marker : {"eqgame.exe", "spells_us.txt", "eqclient.ini"}) {
            std::error_code ec;
            if (fs::exists(candidate / marker, ec) || ec) return true;
        }
        const auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return false;
}

bool safe_update_tree(const fs::path& dir) {
    if (!safe_directory(dir)) return false;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return !ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!platform::ordinary_path(it->path())) return false;
    }
    return !ec;
}

class Progress {
public:
    void report(int value) {
        if (value <= last_) return;
        last_ = value;
        line("progress", std::to_string(value));
    }
private:
    int last_ = -1;
};

int run(std::string_view operation, const fs::path& install, const fs::path& data,
        const std::string& feed, std::string_view channel, const Ed25519PublicKey& key, std::string current) {
    const auto state_dir = app::update_dir(install);
    if (install == install.root_path() || data == data.root_path() || !safe_directory(install) ||
        !safe_directory(data) || game_directory(install) || game_directory(data) || !safe_update_tree(state_dir) ||
        !safe_directory(data / "update") || !platform::ordinary_path(data / "update" / "feed-seen.txt") ||
        !platform::ordinary_path(install / "hailstone.exe"))
        return result("error", current, "Choose a local Hailstone folder outside your game files.");

    // Multiple launcher processes may check concurrently. Only a single one may write download or recovery state.
    std::optional<platform::ScopedFileLock> operation_lock;
    if (operation != "check") {
        std::error_code ec;
        fs::create_directories(state_dir, ec);
        if (ec) return result("error", current, "The Hailstone update folder is unavailable.");
        operation_lock = platform::ScopedFileLock::try_acquire(state_dir / "launcher.lock",
            platform::FileLockMode::Exclusive, platform::LockFolder::Ordinary);
        if (!operation_lock) return result("busy", current, "Another Hailstone update is in progress.", current);
    }

    if (operation == "prepare") {
        const auto prepared = app::prepare_managed_launch(install, current);
        if (prepared.busy) return result("busy", prepared.version, "Hailstone is finishing an update.", current);
        if (prepared.error) return result("error", prepared.version, prepared.message, current);
        if (prepared.rolled_back)
            return result("rolled_back", prepared.version, prepared.message, prepared.version);
        std::error_code ec;
        if (!fs::is_regular_file(install / "hailstone.exe", ec))
            return result("error", prepared.version, "Hailstone needs to be installed before it can start.");
        if (prepared.pending) line("pending", "1");
        return result("ready", prepared.version, {}, prepared.version);
    }

    std::error_code image_error;
    const auto executable = install / "hailstone.exe";
    const bool recognize = current == "0.0.0" && fs::is_regular_file(executable, image_error);
    if (recognize && operation != "check")
        return result("error", current, "Check this Hailstone installation before applying updates.");
    if (const auto inventory = app::read_installed_update(install); !recognize && !inventory.version.empty())
        current = inventory.version;
    if (!platform::http_available()) return result("error", current, "HTTPS is unavailable.", current);
    app::UpdaterEnv env;
    env.install_dir = install;
    env.data_dir = data;
    env.feed = feed;
    env.version = current;
    env.platform = "windows-x64";
    env.channel = channel;
    env.key = key;
    env.remember_feed = operation == "apply";
    // Only this run's verified manifest can select packages for installation, even if a previous feed left a
    // ready.txt with the same version. The package cache is still reused when its hashes match.
    env.reuse_staged = false;
    env.fetch = [](const std::string& url, const auto& sink, const auto& progress, const auto& cancelled) {
        platform::HttpGetOptions options;
        options.user_agent = "HailstoneUpdate";
        options.cancelled = cancelled;
        return platform::http_get(url, options, sink, progress);
    };
    app::Updater updater(std::move(env));
    updater.check();
    updater.wait();
    auto status = updater.status();
    if (status.state == app::UpdateState::FeedBehind)
        return result("error", current, "The Hailstone release feed returned an older release.", current);
    if (recognize) {
        if (status.state != app::UpdateState::Available || !status.has_package || !status.runtime_sha256 ||
            !app::runtime_file_matches(executable, *status.runtime_sha256))
            return result("error", current, "This installation does not match the current signed Hailstone release.");
        return result("recognized", status.version, {}, status.version);
    }
    if (status.state == app::UpdateState::UpToDate || status.skipped)
        return result("current", current, {}, current);
    if (status.state != app::UpdateState::Available || !status.has_package)
        return result("error", current, "Could not verify a Hailstone update for Windows x64.", current);
    if (operation == "check") return result("available", status.version, {}, current);

    Progress progress;
    progress.report(0);
    updater.download();
    while (updater.in_progress()) {
        const auto downloading = updater.status();
        if (downloading.total != 0)
            progress.report(static_cast<int>(70 * std::min(downloading.done, downloading.total) / downloading.total));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    updater.wait();
    status = updater.status();
    if (status.state != app::UpdateState::Ready || !status.runtime_sha256)
        return result("error", current, "Could not download and verify the Hailstone update.", current);
    progress.report(70);
    const app::VerifiedUpdate verified{status.version, std::move(status.verified_packages), *status.runtime_sha256};
    const auto installed = app::install_staged_update(install, current, [&](float fraction) {
        progress.report(70 + static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 29));
    }, &verified);
    if (installed.busy) return result("busy", current, installed.error, current);
    if (!installed.ok) return result("error", current, "The Hailstone update could not be installed.", current);
    progress.report(100);
    return result("installed", installed.version, {}, installed.version);
}
} // namespace

int main(int argc, char** argv) {
    log::set_console(false);
    if (argc != 7)
        return result("error", {}, "Expected an operation, install folder, data folder, feed, key and release.");
    const std::string_view operation = argv[1];
    if (operation != "check" && operation != "apply" && operation != "prepare")
        return result("error", {}, "Unknown Hailstone update operation.");
    const std::string feed = argv[4];
    const std::string_view channel = feed.ends_with("/preview/hailstone-update.txt") ? "preview"
                                   : feed.ends_with("/stable/hailstone-update.txt") ? "stable" : "";
    const auto key = hex_decode<32>(argv[5]);
    if (!feed.starts_with("https://") || channel.empty() || feed.find_first_of("?#\r\n\t ") != std::string::npos || !key ||
        !parse_version(argv[6]))
        return result("error", {}, "Invalid Hailstone update feed, key or release.");
    try {
        return run(operation, utf8_path(argv[2]), utf8_path(argv[3]), feed, channel, *key, argv[6]);
    } catch (const std::exception&) {
        return result("error", {}, "The Hailstone update could not finish.");
    }
}
