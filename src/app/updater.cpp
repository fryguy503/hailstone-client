#include "app/updater.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "app/addon_install.h"
#include "app/update_install.h"
#include "core/atomic_file.h"
#include "core/config.h"
#include "core/log.h"
#include "core/path_utf8.h"
#include "core/version.h"
#include "platform/crash_log.h"
#include "platform/durable_file.h"
#include "platform/private_file.h"

#ifndef HAILSTONE_UPDATE_FEED
#define HAILSTONE_UPDATE_FEED ""
#endif
#ifndef HAILSTONE_UPDATE_KEY
#define HAILSTONE_UPDATE_KEY ""
#endif
#ifndef HAILSTONE_MANAGED_UPDATES
#define HAILSTONE_MANAGED_UPDATES 0
#endif
#ifndef HAILSTONE_ADDON_INDEX_KEY
#define HAILSTONE_ADDON_INDEX_KEY HAILSTONE_UPDATE_KEY
#endif

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMaxManifestBytes = 64 * 1024;
constexpr std::string_view kBastionPreviousStableFeed = "https://patcher.bastiongame.com/hailstone/stable/hailstone-update.txt";

bool bastion_release_version(std::string_view version, const Version& parsed) {
    return parsed.pre.size() == 2 && parsed.pre[0] == "bastion" && parsed.pre[1].front() != '0' &&
           parsed.pre[1].find_first_not_of("0123456789") == std::string::npos && parsed.to_string() == version;
}

bool same_update_feed(std::string_view a, std::string_view b) {
    // Carry the Production record across the move to GitHub without sharing it with Preview.
    return a == b || (a == kBastionGitHubFeed && b == kBastionPreviousStableFeed) ||
           (b == kBastionGitHubFeed && a == kBastionPreviousStableFeed);
}

bool newer(const std::string& release, const std::string& running) {
    auto a = parse_version(release), b = parse_version(running);
    return a && b && compare_versions(*a, *b) > 0;
}

constexpr std::size_t kMaxSeenFeeds = 16;

fs::path seen_file(const fs::path& data_dir) { return data_dir / "update" / "feed-seen.txt"; }

// feed-seen.txt's lines, "<version> <feed address>", one a feed (the newest first).
std::vector<std::pair<std::string, std::string>> read_seen(const fs::path& data_dir) {
    std::vector<std::pair<std::string, std::string>> out;
    auto text = read_text_file(seen_file(data_dir));
    if (!text) return out;
    std::size_t at = 0;
    while (at < text->size() && out.size() < kMaxSeenFeeds) {
        std::size_t nl = text->find('\n', at);
        if (nl == std::string::npos) nl = text->size();
        std::string_view line(text->data() + at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t space = line.find(' ');
        if (space == std::string_view::npos || space + 1 >= line.size()) continue;
        const std::string_view version = line.substr(0, space);
        if (!parse_version(version)) continue; // (a damaged line counts as never seen)
        out.emplace_back(std::string(version), std::string(line.substr(space + 1)));
    }
    return out;
}

bool package_file_ok(const fs::path& path, const UpdatePackage& p) { return package_file_matches(path, p.size, p.sha256); }

struct SetupStaging {
    fs::path directory;
    fs::path partial;
    ~SetupStaging() {
        std::error_code ec;
        fs::remove(partial, ec);
        fs::remove(directory, ec);
    }
};

} // namespace

std::string_view managed_update_channel(std::string_view feed) {
    if (!feed.starts_with("https://") || feed.find_first_of("?#\\") != std::string_view::npos ||
        std::any_of(feed.begin(), feed.end(), [](unsigned char c) { return c <= ' ' || c == 127; }))
        return {};
    if (feed == kBastionGitHubFeed || feed.ends_with("/stable/hailstone-update.txt")) return "stable";
    return feed.ends_with("/preview/hailstone-update.txt") ? "preview" : "";
}

std::string update_asset_url(const std::string& feed, const std::string& version, const std::string& file) {
    if (feed == kBastionGitHubFeed) {
        const auto parsed = parse_version(version);
        // The asset name has already passed the signed manifest parser. Keep this URL construction closed to paths.
        if (!parsed || !bastion_release_version(version, *parsed) || file.empty() || file.front() == '.' ||
            file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos)
            return {};
        return std::string(kBastionGitHubAssets) + version + "/" + file;
    }
    const auto slash = feed.rfind('/');
    return (slash == std::string::npos ? std::string() : feed.substr(0, slash + 1)) + file;
}

bool bastion_github_asset_redirect_allowed(std::string_view url) {
    constexpr std::string_view hosts[] = {
        "https://release-assets.githubusercontent.com/", "https://objects.githubusercontent.com/"};
    return std::any_of(std::begin(hosts), std::end(hosts), [url](std::string_view host) {
               return url.size() > host.size() && url.starts_with(host) && url[host.size()] != '?';
           }) &&
           std::none_of(url.begin(), url.end(), [](unsigned char c) { return c <= ' ' || c == 127 || c == '\\' || c == '#'; });
}

std::string download_bastion_setup(const fs::path& scratch, const std::string& feed, const std::string& version,
                                   const BastionSetupPackage& setup, const UpdateFetch& fetch,
                                   const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    const std::string url = update_asset_url(feed, version, setup.file);
    if (url.empty()) return "Hailstone Setup has an invalid release download address.";
    const fs::path staging = scratch / ".setup-download";
    std::error_code ec;
    if (!platform::ordinary_path(scratch) || !fs::is_empty(scratch, ec) || ec || !platform::make_private_dir(staging))
        return "The Hailstone Setup download folder is unavailable.";
    const SetupStaging temporary{staging, staging / "Setup.part"};
    std::ofstream out(temporary.partial, std::ios::binary);
    if (!out) return "Could not save the Hailstone Setup download.";
    std::uint64_t written = 0;
    bool oversized = false;
    const auto got = fetch(
        url,
        [&](std::span<const std::byte> piece) {
            if (piece.size() > setup.size - written) {
                oversized = true;
                return false;
            }
            out.write(reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
            written += piece.size();
            return static_cast<bool>(out);
        },
        [&](std::uint64_t received, std::uint64_t) {
            return received <= setup.size && (!progress || progress(received, setup.size));
        }, {});
    out.close();
    if (!out) return "Could not save the Hailstone Setup download.";
    if (got.status == 404)
        return feed == kBastionGitHubFeed ? "Hailstone Setup is missing from the GitHub release."
                                        : "Hailstone Setup is missing from the release server.";
    if (oversized || (got.ok && !package_file_matches(temporary.partial, setup.size, setup.sha256)))
        return "The Hailstone Setup download does not match its signed release.";
    if (!got.ok) return "Could not download Hailstone Setup. Try again.";
    if (!platform::sync_file(temporary.partial) || !platform::create_file_synced(temporary.partial, scratch / "Hailstone-Setup.exe", ec))
        return "Could not save the verified Hailstone Setup download.";
    return {};
}

UpdateSettings update_settings(const Config& config) {
    UpdateSettings s;
    s.check = config.get_bool("update.check", true);
    s.download = config.get_bool("update.download", false);
    s.install_on_quit = config.get_bool("update.install_on_quit", true);
    s.feed = config.get_string("update.feed");
    s.skip = config.get_string("update.skip");
    return s;
}

bool update_installs_at_quit(const UpdateSettings& s, bool asked) { return s.install_on_quit && (asked || s.download); }

std::string default_update_feed() { return HAILSTONE_UPDATE_FEED; }

std::optional<Ed25519PublicKey> update_release_key() { return hex_decode<32>(HAILSTONE_UPDATE_KEY); }

bool launcher_manages_updates() { return HAILSTONE_MANAGED_UPDATES != 0; }

std::string default_addon_index() { return HAILSTONE_ADDON_INDEX; }

std::optional<Ed25519PublicKey> addon_index_key() { return hex_decode<32>(HAILSTONE_ADDON_INDEX_KEY); }

std::string default_project_page() { return HAILSTONE_PROJECT_PAGE; }

std::string newest_seen_release(const fs::path& data_dir, const std::string& feed) {
    if (data_dir.empty()) return {};
    std::string newest;
    for (const auto& [version, address] : read_seen(data_dir))
        if (same_update_feed(address, feed) && (newest.empty() || newer(version, newest))) newest = version;
    return newest;
}

bool remember_seen_release(const fs::path& data_dir, const std::string& feed, const std::string& version) {
    if (data_dir.empty() || feed.empty() || feed.find_first_of(" \r\n") != std::string::npos || !parse_version(version))
        return false;
    auto seen = read_seen(data_dir);
    for (const auto& [v, address] : seen)
        if (same_update_feed(address, feed) && !newer(version, v)) return true; // as new, or newer, is kept already
    std::erase_if(seen, [&](const auto& e) { return e.second == feed; });
    seen.insert(seen.begin(), {version, feed});
    if (seen.size() > kMaxSeenFeeds) seen.resize(kMaxSeenFeeds);
    std::string text;
    for (const auto& [v, address] : seen) text += v + " " + address + "\n";
    return write_file_atomic(seen_file(data_dir), text);
}

std::string updates_off_reason(const fs::path& install_dir) {
    std::error_code ec;
    if (fs::exists(install_dir.parent_path() / "CMakeCache.txt", ec)) return "a development build";
    if (!update_release_key()) return "this build has no release key";
    return {};
}

Updater::Updater(UpdaterEnv env) : env_(std::move(env)) {}

Updater::~Updater() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void Updater::wait() {
    if (worker_.joinable()) worker_.join();
}

UpdateStatus Updater::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void Updater::set(const UpdateStatus& s) {
    std::lock_guard lock(mutex_);
    status_ = s;
}

void Updater::start(void (Updater::*job)()) {
    if (busy_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, job] {
        platform::CrashStack crash_stack;
        (this->*job)();
        busy_ = false;
    });
}

void Updater::check() { start(&Updater::run_check); }

void Updater::download() {
    {
        std::lock_guard lock(mutex_);
        if ((status_.state != UpdateState::Available && status_.state != UpdateState::Failed) ||
            !status_.has_package || !manifest_)
            return;
    }
    start(&Updater::run_download);
}

void Updater::skip() {
    std::lock_guard lock(mutex_);
    // (env_ is the worker's: the setting, update.skip, is the client's to write.)
    if (status_.state == UpdateState::Available) status_.skipped = true;
}

void Updater::run_check() {
    UpdateStatus s;
    s.state = UpdateState::Checking;
    set(s);
    std::string text;
    auto got = env_.fetch(
        env_.feed,
        [&](std::span<const std::byte> piece) {
            if (text.size() + piece.size() > kMaxManifestBytes) return false;
            text.append(reinterpret_cast<const char*>(piece.data()), piece.size());
            return !cancel_;
        },
        [&](std::uint64_t, std::uint64_t) { return !cancel_.load(); }, [this] { return cancel_.load(); });
    if (cancel_) return;
    if (!got.ok) {
        s.state = UpdateState::Failed;
        s.fetch_failure_status = got.status;
        s.error = got.status == 404 ? "no release has an update manifest yet" : got.error;
        log::warn("Update check: {}", s.error);
        set(s);
        return;
    }
    std::string why;
    auto m = parse_update_manifest(text, env_.key, &why);
    if (!m) {
        s.state = UpdateState::Failed;
        s.error = why;
        log::warn("Update check: {}", why);
        set(s);
        return;
    }
    if (!env_.channel.empty() && (m->bastion_channel != env_.channel || !m->bastion_runtime_sha256)) {
        s.state = UpdateState::Failed;
        s.error = "the update manifest does not identify a runtime for this release channel";
        set(s);
        return;
    }
    if (env_.feed == kBastionGitHubFeed && !bastion_release_version(m->version_text, m->version)) {
        s.state = UpdateState::Failed;
        s.error = "the update manifest does not identify a tagged Bastion release";
        set(s);
        return;
    }
    s.runtime_sha256 = m->bastion_runtime_sha256;
    s.bastion_setup = m->bastion_setup;
    // A feed that went back: a signed manifest older than one it showed before (an old release's manifest served
    // again). newer() would refuse it anyway; what this adds is that the corner does not say "Up to date".
    if (const std::string seen = newest_seen_release(env_.data_dir, env_.feed); newer(seen, m->version_text)) {
        log::warn("Update check: the feed went back to {}, older than {} which it showed before; nothing is offered "
                  "(delete {} if that release was withdrawn on purpose)",
                  m->version_text, seen, seen_file(env_.data_dir).string());
        s.state = UpdateState::FeedBehind;
        s.version = m->version_text;
        s.seen = seen;
        set(s);
        return;
    }
    if (env_.remember_feed) remember_seen_release(env_.data_dir, env_.feed, m->version_text);
    if (!newer(m->version_text, env_.version)) {
        log::info("Update check: {} is the newest release", env_.version);
        s.state = UpdateState::UpToDate;
        s.version = m->version_text;
        set(s);
        return;
    }
    const std::string bad = failed_update_version(env_.install_dir);
    s.version = m->version_text;
    s.notes = m->notes;
    const auto packages = packages_for(*m, env_.platform);
    s.has_package = std::any_of(packages.begin(), packages.end(), [](const auto& p) { return p.component == "core"; });
    s.skipped = m->version_text == env_.skip || m->version_text == bad;
    {
        std::lock_guard lock(mutex_);
        manifest_ = *m;
    }
    log::info("Update check: {} is out (this is {}){}", s.version, env_.version,
              s.has_package ? "" : ", with no package for " + env_.platform);
    if (env_.reuse_staged && s.has_package && staged_update_version(env_.install_dir) == s.version &&
        staged_update_ok(env_.install_dir)) {
        s.state = UpdateState::Ready;
        set(s);
        return;
    }
    s.state = UpdateState::Available;
    set(s);
    if (!s.has_package || s.skipped || !env_.auto_download) return;
    if (env_.on_available) env_.on_available();
    {
        // The player may have pressed Skip since Available showed: their word wins. Past this point skip() finds the
        // download under way and changes nothing.
        std::lock_guard lock(mutex_);
        if (status_.skipped) return;
        status_.state = UpdateState::Downloading;
    }
    run_download();
}

void Updater::run_download() {
    UpdateStatus s = status();
    std::optional<UpdateManifest> m;
    {
        std::lock_guard lock(mutex_);
        m = manifest_;
    }
    if (!m) return;
    const InstalledUpdate installed = read_installed_update(env_.install_dir);
    std::vector<UpdatePackage> wanted;
    for (const auto& p : packages_for(*m, env_.platform)) {
        std::error_code ec;
        if (!p.needs.empty() && !fs::is_directory(env_.install_dir / fs::path(p.needs), ec)) continue;
        // A folder an add-on installed (docs/addons.md) is the add-on manager's: an optional package of the same
        // name would write into another plugin.
        if (!p.needs.empty() && addon_owns_folder(env_.install_dir, p.needs)) continue;
        // A component this folder already has at this package's hash needs nothing.
        if (auto it = installed.components.find(p.component);
            it != installed.components.end() && it->second == p.sha256)
            continue;
        wanted.push_back(p);
    }
    s.state = UpdateState::Downloading;
    s.fetch_failure_status.reset();
    s.done = 0;
    s.total = 0;
    for (const auto& p : wanted) s.total += p.size;
    set(s);

    const fs::path dir = update_dir(env_.install_dir) / "download";
    std::error_code ec;
    fs::create_directories(dir, ec);
    clear_staged_update(env_.install_dir); // a staged older release is replaced
    std::uint64_t before = 0;
    for (const auto& p : wanted) {
        const fs::path file = dir / p.file;
        if (!package_file_ok(file, p)) {
            const std::string url = update_asset_url(env_.feed, m->version_text, p.file);
            if (url.empty()) {
                s.state = UpdateState::Failed;
                s.error = "the update manifest has an invalid release download address";
                set(s);
                return;
            }
            const fs::path part = dir / (p.file + ".part");
            // Replacing scrubbed names instead of truncating an old partial also avoids following a hard link.
            fs::remove(part, ec);
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out) {
                s.state = UpdateState::Failed;
                s.error = "cannot write " + path_utf8(part);
                set(s);
                return;
            }
            log::info("Update: downloading {} ({} bytes)", p.file, p.size);
            std::uint64_t written = 0;
            bool too_large = false;
            auto got = env_.fetch(
                url,
                [&](std::span<const std::byte> piece) {
                    // The manifest's size is the limit, checked as the bytes come (the progress callback runs only
                    // about once a second): a server sending more stops at once, before it fills the disk.
                    if (piece.size() > p.size - written) {
                        too_large = true;
                        return false;
                    }
                    written += piece.size();
                    out.write(reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
                    return static_cast<bool>(out) && !cancel_;
                },
                [&](std::uint64_t received, std::uint64_t) {
                    if (received > p.size) return false;
                    UpdateStatus progress = s;
                    progress.done = before + received;
                    set(progress);
                    return !cancel_.load();
                },
                [this] { return cancel_.load(); });
            out.close();
            if (cancel_) return;
            if (!got.ok || !package_file_ok(part, p)) {
                fs::remove(part, ec);
                s.state = UpdateState::Failed;
                if (!got.ok) s.fetch_failure_status = got.status;
                s.error = got.ok      ? p.file + " did not match the manifest"
                          : too_large ? p.file + " is larger than the manifest says"
                                      : got.error;
                log::warn("Update: {}", s.error);
                set(s);
                return;
            }
            platform::replace_file_synced(part, file, ec);
            if (ec) {
                s.state = UpdateState::Failed;
                s.error = "cannot keep " + path_utf8(file) + ": " + ec.message();
                set(s);
                return;
            }
        }
        before += p.size;
    }
    std::string why;
    if (!stage_update(env_.install_dir, m->version_text, wanted, &why)) {
        s.state = UpdateState::Failed;
        s.error = why;
        set(s);
        return;
    }
    log::info("Update: {} is downloaded; it is installed when Hailstone quits", m->version_text);
    s.state = UpdateState::Ready;
    s.done = s.total;
    s.verified_packages = std::move(wanted);
    set(s);
}

} // namespace hailstone::app
