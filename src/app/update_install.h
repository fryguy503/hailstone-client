#pragma once

// Installing a staged update into the Hailstone folder, and taking it back out (docs/updater.md). Everything lives
// in <install>/update/:
//   download/<package>.zip  the packages app/updater.h downloaded (each checked by size and SHA-256)
//   ready.txt               the staged update: its version, then each package's component, size, hash and file
//   installed.txt           what the updates put in: each component's package hash and its files, so the next
//                           update removes the files a component no longer ships and skips unchanged components
//   new/                    the packages unpacked, before anything in the folder is touched
//   previous/               the files the last install replaced or removed, until the new version has started
//   journal.txt             the install's moves, one a line, so a cut-off install or a failed start is undone
//   state.txt               "installing <new> <old>" while files move; "pending <new> <old> <starts>" until the
//                           new version reaches the login screen
//   failed.txt              a release that failed to start twice and was rolled back (not offered again)
//   note.txt                what a rollback has to tell the version it starts (StartNote::message)
//   install.lock            held (exclusively) by the one process that installs, rolls back or ends a pending update
//   running.lock            held (shared) by every Hailstone running from the folder; taken exclusively only to see
//                           that no other is running (an install, a rollback, a start's count)
// The install runs when the client quits (nothing else of it is running then): each file is first unpacked into
// new/, then the old one is moved into previous/ and the new one moved into its place. Both are renames on one
// volume, which Windows allows for a running program and its loaded libraries, so the quitting client can do it
// itself. The player's own files (settings, servers, logs, maps, scripts ...: kPlayerPaths) are never written.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/sha256.h"
#include "core/update_manifest.h"
#include "platform/shared_file.h"

namespace hailstone::app {

std::filesystem::path update_dir(const std::filesystem::path& install_dir);

// Whether the file at `path` has this size and SHA-256 (a downloaded package).
bool package_file_matches(const std::filesystem::path& path, std::uint64_t size, const Sha256Digest& sha256);
bool runtime_file_matches(const std::filesystem::path& path, const Sha256Digest& sha256);

struct InstalledUpdate {
    std::string version; // release recorded by the last successful runtime change, or empty for older inventories
    std::map<std::string, Sha256Digest> components;              // component -> its package's hash
    std::map<std::string, std::vector<std::string>> files;       // component -> the files it put in
};
InstalledUpdate read_installed_update(const std::filesystem::path& install_dir);

// Whether `path` (relative, '/' separators) is one of the player's: an update never writes or removes it.
bool is_player_path(std::string_view path);

// Records `packages` (already in update/download/) as the staged update of `version`.
bool stage_update(const std::filesystem::path& install_dir, const std::string& version,
                  const std::vector<UpdatePackage>& packages, std::string* error);
// The staged update's version ("" none), and whether its packages are all there and match.
std::string staged_update_version(const std::filesystem::path& install_dir);
bool staged_update_ok(const std::filesystem::path& install_dir);
// Forgets the staged update (its downloaded packages stay, to be used again when they match).
void clear_staged_update(const std::filesystem::path& install_dir);

// A release that failed to start after its install and was rolled back ("" none).
std::string failed_update_version(const std::filesystem::path& install_dir);

struct InstallResult {
    bool ok = false;
    bool busy = false;
    std::string version; // the version installed
    std::string error;
};
struct VerifiedUpdate {
    std::string version;
    std::vector<UpdatePackage> packages;
    Sha256Digest runtime_sha256{};
};
// Installs the staged update over the folder; `running` is this build's version. On any failure the folder is put
// back as it was. `progress` (may be empty) gets 0..1. Nothing is installed (an error) while another Hailstone runs
// from the folder (running.lock): the caller lets go of its own claim (claim_running) first.
InstallResult install_staged_update(const std::filesystem::path& install_dir, const std::string& running,
                                    const std::function<void(float)>& progress = {},
                                    const VerifiedUpdate* verified = nullptr);

// This process's claim on the folder (a shared lock on update/running.lock), held while it runs from the folder's
// files: no other Hailstone installs or rolls back an update under it. nullopt when it cannot be had (a folder that
// cannot be written, or another process installing at this moment).
std::optional<platform::ScopedFileLock> claim_running(const std::filesystem::path& install_dir);

struct StartNote {
    std::string message; // for the player and the log ("" nothing happened)
    std::string version; // release effective after recovery, for an external launcher
    bool busy = false;
    bool rolled_back = false;
    bool error = false;
    bool pending = false; // an installed release still needs an actual healthy start
    bool restart = false; // the folder now holds another version than this one: start it and quit
    // This process's claim (claim_running), taken before the install lock is let go so that no other start counts
    // or rolls back between the two. Kept for as long as the process runs.
    std::optional<platform::ScopedFileLock> running_claim;
};
// First thing at start. Only while no other Hailstone runs from the folder: a cut-off install is undone; a pending
// update's start is counted, and a third start of it that never reached the login screen rolls it back (failed.txt).
// While another one runs, none of that happens (its start was counted, and it runs from these files). Another version
// than the pending one (the player put one in by hand) ends the pending state.
StartNote update_at_start(const std::filesystem::path& install_dir, const std::string& running);
// Launcher preparation without running the client or creating a ticket. Uses native inventory when a launcher
// receipt was interrupted. A startup is counted by the client itself after the launcher obtains a ticket.
StartNote prepare_managed_launch(const std::filesystem::path& install_dir, const std::string& running);
// A ticketed managed client is already starting. Take its running claim and count the attempt without restarting
// or rolling back under a live ticket. On failure, the client stops before sending the ticket.
StartNote record_managed_start(const std::filesystem::path& install_dir, const std::string& running);
// The login screen is up: a pending update of this version started fine, and previous/ goes. False (nothing done)
// while another process holds the install lock: call it again later.
bool update_started_fine(const std::filesystem::path& install_dir, const std::string& running);

// A file an earlier release shipped that later ones do not: path ('/'-separated, from the folder), size, SHA-256.
struct RetiredFile {
    std::string_view path;
    std::uint64_t size = 0;
    std::string_view sha256; // hex
};
// Removes the files of `retired` (by default the server plugins 0.1.0-alpha and 0.2.0-alpha shipped, add-ons since)
// that are there byte for byte as shipped: a folder updated by unpacking a release over it, or by an updater that kept
// no list of its files, still has them, and they would stop the add-on of the same name from installing. A file
// changed since stays, and so do the plugin's settings beside them; a plugin folder whose library is another build
// (the add-on's, or one built from the plugin's repository) keeps every file, its unchanged window files too; folders
// left empty go. Does nothing while an update is pending (a rollback needs them): call it once the login screen is up,
// after update_started_fine. Does nothing either while another process holds the install lock (the next start does
// it). Returns the files removed.
const std::vector<RetiredFile>& retired_release_files();
std::vector<std::string> retire_old_release_files(const std::filesystem::path& install_dir);
std::vector<std::string> retire_old_release_files(const std::filesystem::path& install_dir,
                                                  const std::vector<RetiredFile>& retired);

} // namespace hailstone::app
