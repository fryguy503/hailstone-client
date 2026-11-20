#pragma once

// The client's updater (docs/updater.md), a Hailstone feature with no original counterpart: at start it fetches the
// release feed's signed manifest (core/update_manifest.h) on a worker thread; when the release is newer than this
// build it tells the login screen and, when the player asks (or update.download is on), downloads this platform's packages into update/download/ (each checked by size
// and SHA-256) and stages them (update/ready.txt). Nothing in the Hailstone folder changes until the client quits:
// app/update_install.h installs the staged update then, and the player can restart into it from the login screen.
// The settings are hailstone.ini's update.* keys (update_settings()).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "core/ed25519.h"
#include "core/update_manifest.h"
#include "platform/http_get.h"

namespace hailstone { class Config; }

namespace hailstone::app {

// hailstone.ini's update.* keys.
struct UpdateSettings {
    bool check = true;           // update.check: look for a new release at start
    bool download = false;       // update.download: download it without asking (only when the player sets it)
    bool install_on_quit = true; // update.install_on_quit: install a downloaded one when the client quits
    std::string feed;            // update.feed: the manifest's address ("" the build's, kUpdateFeed)
    std::string skip;            // update.skip: a release the player chose to skip
};
UpdateSettings update_settings(const Config& config);
// Whether a plain quit installs a downloaded update. Only one the player asked for: Download pressed in this run
// (`asked`), or update.download switched on. A package an earlier run left downloaded waits for Restart now.
bool update_installs_at_quit(const UpdateSettings& s, bool asked);

// The address of the build's manifest (CMake HAILSTONE_UPDATE_FEED) and its release key (HAILSTONE_UPDATE_KEY, the public key
// hex; nullopt when the build has none, and then there are no updates).
std::string default_update_feed();
std::optional<Ed25519PublicKey> update_release_key();
// The add-on index's address (CMake HAILSTONE_ADDON_INDEX, core/addon_index.h; "" none), signed with the release key.
std::string default_addon_index();
// Hailstone's page (CMake HAILSTONE_PROJECT_PAGE), which the login screen links to.
std::string default_project_page();

// Why updates are off for the Hailstone folder `install_dir` ("" when they are not): a development tree (the build
// folder's CMakeCache.txt beside it), or no release key in this build.
std::string updates_off_reason(const std::filesystem::path& install_dir);

// One GET; `cancelled` (safe from any thread: http_get's HttpGetOptions::cancelled) turns true when the updater
// stops, so quitting does not wait out a fetch stalled on the network.
using UpdateFetch = std::function<platform::HttpGetResult(
    const std::string& url, const std::function<bool(std::span<const std::byte>)>& sink,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress, const std::function<bool()>& cancelled)>;

struct UpdaterEnv {
    std::filesystem::path install_dir;
    std::string feed;           // the manifest's address
    std::string version;        // this build's (release_version())
    std::string platform;       // update_platform()
    Ed25519PublicKey key{};
    bool auto_download = false;
    std::string skip;           // a version not to offer
    UpdateFetch fetch;          // http_get, or the tests' own
    // Tests: called on the worker once a check shows Available, before it starts a download of its own.
    std::function<void()> on_available;
};

enum class UpdateState {
    Idle,        // not checked yet
    Checking,
    UpToDate,
    Available,   // a newer release; download() fetches it (manual or no package: only its notes)
    Downloading,
    Ready,       // downloaded and checked: installed at quit
    Failed,
};

struct UpdateStatus {
    UpdateState state = UpdateState::Idle;
    std::string version;        // the newer release's
    std::string notes;          // its page
    std::string error;          // Failed: why
    bool has_package = false;   // Available: this platform has a package (else the player downloads it by hand)
    bool skipped = false;       // Available: the player chose to skip this release (update.skip)
    std::uint64_t done = 0, total = 0; // Downloading: bytes so far, all of them (0 unknown)
};

class Updater {
public:
    explicit Updater(UpdaterEnv env);
    ~Updater(); // cancels a check or download and waits for it
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Starts a check on the worker thread (nothing while one runs). A release already staged in update/ready.txt
    // that the manifest still names is Ready at once.
    void check();
    // Downloads the Available release's packages (or tries a failed download again).
    void download();
    // The player chose not to take this release: it is offered again only when a newer one comes.
    void skip();
    UpdateStatus status() const;
    // Waits for the worker (tests).
    void wait();

private:
    void run_check();
    void run_download();
    void set(const UpdateStatus& s);
    void start(void (Updater::*job)());

    UpdaterEnv env_;
    mutable std::mutex mutex_;
    UpdateStatus status_;
    std::optional<UpdateManifest> manifest_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> busy_{false};
};

// The end of a run that has a downloaded update (app/client_app_update.cpp): installs it (app/update_install.h) and,
// with `restart`, starts the new version.
void finish_update_at_exit(bool restart);
// Starts the Hailstone folder's client (after an install or a rollback), left running as this one quits.
void start_installed_client();

// The argument a box set's other windows are started with (ClientApp::start_box_login): such a window runs from the
// folder the first window updates, so it neither checks for updates nor fetches add-ons. Two updaters would download
// into the same addons/ and download/ files, and one quitting first could install while the others run.
inline constexpr const char* kBoxChildArg = "--box-child";

// Whether a run has the updater and the add-on manager's fetching (main.cpp): a player's own run, not a test drive
// that stops after some frames or takes a screenshot, and not a box set's other window (kBoxChildArg).
inline bool run_has_updates(int frames, bool screenshot, bool box_child) {
    return frames == 0 && !screenshot && !box_child;
}

} // namespace hailstone::app
