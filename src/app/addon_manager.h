#pragma once

// The add-on manager (docs/addons.md), a Hailstone feature with no original counterpart: the player pastes a GitHub
// link on the login screen, the manager fetches the add-on's manifest (core/addon_manifest.h) and shows what it is
// and what it asks to do; on the player's yes it downloads the package, checks it (app/addon_install.h) and installs
// it. At start it looks for newer versions of the installed add-ons and installs those that ask for nothing more.
// Fetching, checking and unpacking happen on a worker thread; the files move into place only on the main thread,
// through install_ready(), which the client calls on the login screen (nothing runs there: docs/plugins.md), so a
// script or plugin never changes under a running one.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/addon_install.h"
#include "core/addon_index.h"
#include "core/addon_manifest.h"
#include "platform/http_get.h"
#include "plugin/consent.h"

namespace hailstone { class Config; }

namespace hailstone::app {

// hailstone.ini's addons.* keys.
struct AddonSettings {
    bool check = true;          // addons.check: look for newer versions at start
    int check_hours = 6;        // addons.check_hours: at most this often (0: every start)
    bool allow_unsigned = true; // addons.allow_unsigned: install add-ons whose manifest is not signed
};
AddonSettings addon_settings(const Config& config);

// One GET (platform::http_get, or the tests' own); the manager keeps every download on GitHub
// (addon_download_allowed) through `allow_url`. `keep_going` turns false when the manager stops: the transfer ends.
// It may be asked from any thread (http_get's HttpGetOptions::cancelled, which ends a call waiting on the network).
using AddonFetch = std::function<platform::HttpGetResult(
    const std::string& url, const std::function<bool(std::string_view)>& allow_url,
    const std::function<bool(std::span<const std::byte>)>& sink, const std::function<bool()>& keep_going)>;

struct AddonsEnv {
    std::filesystem::path data_dir;
    std::string client_version; // release_version()
    std::string platform;       // update_platform()
    AddonFetch fetch;
    AddonSettings settings;
    // The add-on index (core/addon_index.h): its address ("" none) and the key it is signed with (the release key;
    // nullopt: no index).
    std::string index_url;
    std::optional<Ed25519PublicKey> index_key;
    std::function<std::int64_t()> now; // seconds (the check interval); empty: the system clock
};

// A manifest fetched for a link, waiting for the player's yes: the install card.
struct AddonOffer {
    std::string link;          // as pasted
    std::string source;        // addon_source_text
    std::string page;          // the repository's page
    std::string manifest_url;  // the address that answered
    AddonManifest manifest;
    std::uint32_t caps = 0;    // what it asks for
    std::string installed_version; // "" a new add-on; else this replaces that version
    bool older = false;        // older than the installed version: install_offer needs older_ok
    std::string blocked;       // why it cannot be installed ("" it can)
    bool suggested = false;    // opened from a suggestion (docs/addons.md "Suggested add-ons")
};

// What a suggested add-on's manifest must be (inspect()): the add-on index's name and key for it. A manifest that
// is another add-on, or signed with another key (or none), is blocked.
struct AddonExpect {
    std::string name;
    Ed25519PublicKey key{};
};

enum class AddonState : std::uint8_t {
    Idle,          // not checked this run
    Checking,
    UpToDate,
    Available,     // a newer version, and auto-update is off: update() takes it
    Downloading,   // a newer version, or the first install
    Ready,         // downloaded and checked: installed at the login screen
    NeedsApproval, // a newer version asks for more capabilities: update() approves them
    Modified,      // a newer version, but the player changed files of this one: update() overwrites them
    Blocked,       // a newer version that cannot be taken (another key, another add-on, a newer Hailstone needed)
    Failed,
};
std::string_view addon_state_word(AddonState state); // "idle", "checking", ... as the UI packs see it

struct AddonStatus {
    InstalledAddon addon;     // as installed (for an add-on being installed the first time: from its manifest)
    bool installed = true;    // false: its first install is under way
    AddonState state = AddonState::Idle;
    std::string new_version;  // a newer version (Available .. Blocked)
    std::uint32_t new_caps = 0; // NeedsApproval: what the newer version asks for
    std::vector<std::string> modified; // the files the player changed (any state)
    std::string error;        // Failed and Blocked: why
};

// An add-on of the add-on index, as the store shows it (docs/addons.md "Finding add-ons").
struct AddonCatalogEntry {
    AddonIndexAddon addon;                 // the index's record: name, source, key
    std::vector<std::string> worlds;       // the worlds the index names it for (none: any server)
    std::optional<AddonManifest> manifest; // its manifest, held to the index's name and key (nullopt: problem says why)
    std::string problem;                   // why it cannot be installed here ("" it can)
};

struct AddonsStatus {
    bool inspecting = false;          // a link's manifest is being fetched
    std::optional<AddonOffer> offer;  // the install card
    std::string message;              // the last thing that happened, for the player
    bool message_error = false;
    std::uint32_t message_serial = 0; // changes with each message (the same words may come twice)
    std::vector<AddonStatus> addons;  // installed (and being installed), by name
    std::uint32_t serial = 0;         // changes with installed.txt (the ceilings change with it)
    std::optional<AddonIndex> index;  // the add-on index, fetched or cached (nullopt: none yet)
    // The store (browse()): each add-on the index lists, with its manifest.
    bool browsing = false;
    bool browsed = false;             // the list was made this run (catalog_error: why it is empty)
    std::string catalog_error;
    std::vector<AddonCatalogEntry> catalog;
};

class AddonManager {
public:
    explicit AddonManager(AddonsEnv env);
    ~AddonManager(); // stops the worker (a download in flight is dropped)
    AddonManager(const AddonManager&) = delete;
    AddonManager& operator=(const AddonManager&) = delete;

    // Fetches the manifest a link names: the offer, or a message why not. `expect`: a suggested add-on's, from the
    // add-on index.
    void inspect(std::string link, std::optional<AddonExpect> expect = std::nullopt);
    void cancel_offer();
    // The player said yes to the offer: download, check and stage it (installed by install_ready()). An offer older
    // than the installed version (AddonOffer::older) needs `older_ok`, the player's second yes; without it the offer
    // stays and the message says why.
    void install_offer(bool older_ok = false);
    // Looks for newer versions of the add-ons with auto-update on (`now`: whatever the interval says, and every
    // add-on).
    void check_updates(bool now);
    // Updates one add-on now: approves more capabilities and overwrites the player's changes when it must. What the
    // player saw and approved: the capabilities (at most these: a version that asks for more is not taken, and must be
    // reviewed again) and the version ("" any).
    void update(std::string name, std::uint32_t approved_caps, std::string approved_version);
    void set_auto_update(std::string_view name, bool on);
    bool set_enabled(std::string_view name, bool on);
    // Reads the cached add-on index (addons/index.txt), then fetches it again when the cached one is older than
    // addons.check_hours (`now`: whatever its age). Nothing without env.index_url and env.index_key.
    void refresh_index(bool now);
    // The store: fetches the manifest of each add-on the index lists (once a run; `again`: once more). It runs after
    // refresh_index()'s job, so a fresh index is used.
    void browse(bool again);
    // Main thread, on the login screen: takes the add-on out (its own files; the player's stay).
    bool remove(std::string_view name, std::string* error);
    // Main thread, on the login screen: installs what is downloaded and checked. The names installed.
    std::vector<std::string> install_ready();
    bool has_ready() const;

    AddonsStatus status() const;
    // status().serial without copying the rest (the index, the store's manifests): what the world asks each frame.
    std::uint32_t serial() const;
    // Moves on whenever status() may have changed (each change the manager makes, and some looks that change
    // nothing): the login screen copies the status and rebuilds its add-on view only when it does.
    std::uint32_t changes() const { return changes_.load(std::memory_order_acquire); }
    std::vector<InstalledAddon> installed() const;
    // Waits until the worker has nothing to do (tests).
    void wait_idle();

private:
    struct Staged {
        InstalledAddon addon;
        std::filesystem::path package; // addons/download/<sha256>.zip, removed once installed
        // An update taken because auto-update was on (not the player's click): not installed if they turn it off
        // before install_ready().
        bool automatic = false;
    };

    // The lock taken to change status_ (or anything status() shows): changes() moves on as it is let go.
    struct Changing {
        explicit Changing(AddonManager& m) : manager(m), lock(m.mutex_) {}
        ~Changing() { manager.changes_.fetch_add(1, std::memory_order_acq_rel); }
        Changing(const Changing&) = delete;
        Changing& operator=(const Changing&) = delete;
        AddonManager& manager;
        std::lock_guard<std::mutex> lock;
    };

    void post(std::function<void()> job);
    void run();
    void say(std::string text, bool error);
    AddonStatus* row(std::string_view name); // under mutex_
    void set_row(const AddonStatus& s);
    void set_row_locked(const AddonStatus& s); // under mutex_
    std::optional<std::string> fetch_text(const std::string& url, std::string* error);
    // The manifest of a source: the first of its addresses that answers, checked; nullopt with *error.
    std::optional<std::pair<AddonManifest, std::string>> fetch_manifest(std::string_view source, std::string* error);
    // What is wrong with a manifest whatever it is for ("" nothing): the client's version, the platform, the
    // capabilities, the signature rules.
    std::string manifest_problem(const AddonManifest& m, std::uint32_t* caps) const;
    // Downloads and unpacks; the staged record, or nullopt with *error.
    std::optional<Staged> download(const AddonManifest& m, const std::string& manifest_url, const std::string& source,
                                   std::uint32_t caps, bool auto_update, std::string* error);
    // Returns 1 when a newer version was found (downloaded, or waiting for the player), else 0.
    // force: the player approved; `approved`: the capabilities they saw (nullopt: none, no forcing), and the version
    // ("" any).
    struct Approval {
        std::uint32_t caps = 0;
        std::string version;
    };
    int check_one(const InstalledAddon& a, const std::optional<Approval>& approved);
    void reload_rows();

    AddonsEnv env_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool running_job_ = false;
    bool stop_ = false;
    std::atomic<bool> cancel_{false}; // stop_, readable without the lock (the transfers ask it)
    AddonsStatus status_;
    std::atomic<std::uint32_t> changes_{0};
    std::vector<Staged> staged_;
    // Lower-case names of the add-ons removed this run: a check that read installed.txt before the removal does not
    // bring them back. A new install of the same name (install_offer) takes its name out.
    std::set<std::string> removed_;
    std::thread worker_;
};

// The scripts and plugins a server sent (its servers/<profile>/scripts and plugins, as its last login recorded
// them): shown in the add-on list as the server's, which the player cannot remove or stop.
struct ServerAddon {
    std::string name;
    AddonKind kind = AddonKind::Script;
    // A compiled plugin: the player's word on the bytes the server sent (plugin/consent.h), and those bytes' hashes
    // (the files of that name it listed), which the player's word is taken back or forgotten for.
    plugin::Consent consent = plugin::Consent::Unasked;
    std::vector<Sha256Digest> hashes;
};
std::vector<ServerAddon> server_addons(const std::filesystem::path& profile_dir);

// The scripts and plugins in the player's folders that no add-on installed (put in by hand, or shipped with
// Hailstone): `dirs` are the folders' parents in search order (the data folder, then the install's).
struct ManualAddon {
    std::string name;
    AddonKind kind = AddonKind::Script;
    std::filesystem::path path; // the file, or its folder
};
std::vector<ManualAddon> manual_addons(const std::vector<std::filesystem::path>& dirs,
                                       const std::vector<InstalledAddon>& installed);

} // namespace hailstone::app
