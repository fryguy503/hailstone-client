#include "app/updater.h"
#include "core/distribution.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "app/addon_install.h"
#include "app/update_install.h"
#include "core/config.h"
#include "core/log.h"
#include "core/version.h"
#include "platform/crash_log.h"

#ifndef HAILSTONE_UPDATE_FEED
#define HAILSTONE_UPDATE_FEED ""
#endif
#ifndef HAILSTONE_UPDATE_KEY
#define HAILSTONE_UPDATE_KEY ""
#endif

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMaxManifestBytes = 64 * 1024;

// The address of a file beside the manifest: GitHub's releases/latest/download/<name> is the same release's asset.
std::string beside(const std::string& feed, const std::string& file) {
    const auto slash = feed.rfind('/');
    return (slash == std::string::npos ? std::string() : feed.substr(0, slash + 1)) + file;
}

bool newer(const std::string& release, const std::string& running) {
    auto a = parse_version(release), b = parse_version(running);
    return a && b && compare_versions(*a, *b) > 0;
}

bool package_file_ok(const fs::path& path, const UpdatePackage& p) { return package_file_matches(path, p.size, p.sha256); }

} // namespace

UpdateSettings update_settings(const Config& config) {
    UpdateSettings s;
    if (distribution::managed()) {
        s.check = s.download = s.install_on_quit = false;
        return s;
    }
    s.check = config.get_bool("update.check", true);
    s.download = config.get_bool("update.download", false);
    s.install_on_quit = config.get_bool("update.install_on_quit", true);
    s.feed = config.get_string("update.feed");
    s.skip = config.get_string("update.skip");
    return s;
}

bool update_installs_at_quit(const UpdateSettings& s, bool asked) {
    return !distribution::managed() && s.install_on_quit && (asked || s.download);
}

std::string default_update_feed() { return HAILSTONE_UPDATE_FEED; }

std::optional<Ed25519PublicKey> update_release_key() { return hex_decode<32>(HAILSTONE_UPDATE_KEY); }

std::string default_addon_index() { return HAILSTONE_ADDON_INDEX; }

std::string default_project_page() { return HAILSTONE_PROJECT_PAGE; }

std::string updates_off_reason(const fs::path& install_dir) {
    if (distribution::managed()) return "updates are supplied by this managed distribution";
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
    if (distribution::managed()) {
        UpdateStatus status;
        status.state = UpdateState::Failed;
        status.error = "updates are supplied by this managed distribution";
        set(status);
        return;
    }
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
    if (s.has_package && staged_update_version(env_.install_dir) == s.version &&
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
            const fs::path part = dir / (p.file + ".part");
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out) {
                s.state = UpdateState::Failed;
                s.error = "cannot write " + part.string();
                set(s);
                return;
            }
            log::info("Update: downloading {} ({} bytes)", p.file, p.size);
            auto got = env_.fetch(
                beside(env_.feed, p.file),
                [&](std::span<const std::byte> piece) {
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
                s.error = got.ok ? p.file + " did not match the manifest" : got.error;
                log::warn("Update: {}", s.error);
                set(s);
                return;
            }
            fs::rename(part, file, ec);
            if (ec) {
                s.state = UpdateState::Failed;
                s.error = "cannot keep " + file.string() + ": " + ec.message();
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
    set(s);
}

} // namespace hailstone::app
