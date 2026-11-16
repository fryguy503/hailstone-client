#pragma once

// Installing and removing add-ons (docs/addons.md): a Lua script into scripts/<name>/, a compiled plugin into
// plugins/<name>/ or a tool (a program of its own) into tools/<name>/ of the Hailstone folder, from a package the add-on manager (app/addon_manager.h) downloaded and
// checked against its manifest. A Hailstone feature with no original counterpart. Everything of its own lives in
// <data>/addons/:
//   installed.txt   each add-on: where it came from, its version, key, capabilities, and every file it put in with
//                   its SHA-256 (so an update replaces only those, and a file the player changed is seen)
//   new/<name>/     a package unpacked and checked, before anything in its folder changes
//   previous/<name>/ the files an install or a removal is moving out, until it has finished
//   journal.txt     an install's or a removal's moves, one a line, so a cut-off one is undone at the next start
//   download/       the downloaded packages
// Nothing a package holds is trusted: what it may hold is what a server may send into the same folder
// (net::server_file_destination), a script needs an init.lua, a plugin exactly one library of this platform named
// after it (read from its header, never loaded), and every Lua file must compile as text (no precompiled chunks). A
// tool holds one program of this platform named after it at its top (read from its header, never run), may hold
// this platform's libraries beside it and data anywhere, and nothing else that runs.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/addon_manifest.h"
#include "core/ed25519.h"
#include "core/sha256.h"
#include "plugin/policy.h"
#include "mq/lua_mq.h"

namespace hailstone::app {

struct AddonFile {
    std::string path; // relative to the add-on's folder, '/' separators
    std::uint64_t size = 0;
    Sha256Digest sha256{};
};

struct InstalledAddon {
    std::string name;
    AddonKind kind = AddonKind::Script;
    std::string source;   // core/addon_source.h addon_source_text
    std::string version;
    std::uint32_t caps = 0; // the capabilities the player accepted (plugin/policy.h): the ceiling
    std::optional<Ed25519PublicKey> key; // pinned at the first install: later versions must carry the same
    bool auto_update = false; // only when the player switches it on: nothing updates unasked
    bool autoload = false, enabled = false; // managed Lua add-on; the player controls activation independently of updates
    std::string author, description, notes;
    std::vector<AddonFile> files;
};

std::filesystem::path addons_dir(const std::filesystem::path& data_dir);
// scripts/<name>, plugins/<name>, tools/<name> or ui-addons/<name> under the data folder.
std::filesystem::path addon_folder(const std::filesystem::path& data_dir, AddonKind kind, std::string_view name);
std::string_view addon_kind_word(AddonKind kind); // "script", "plugin", "tool", "upscaler", "ui"
// A tool's program in its package and its folder for `platform`: <name>.exe on Windows, <name> elsewhere.
std::string addon_tool_program(std::string_view name, std::string_view platform);

// An add-on installed.txt names whose record does not read: its files get no capabilities (addon_ceilings), since
// what it was allowed is not known. Fails closed: a lost or damaged registry must not turn an add-on's files into
// hand-installed ones, which get every capability on a server without hailstone.cfg.
struct UnreadableAddon {
    std::string name;
    std::optional<AddonKind> kind; // nullopt: its kind did not read either (every kind's folder of the name)
};

// installed.txt as read, with installed.bak.txt (the copy written beside it each time) standing in for a record of
// it that is lost or does not read.
struct AddonRegistry {
    std::vector<InstalledAddon> addons;     // the records that read
    std::vector<UnreadableAddon> unreadable; // named, but neither installed.txt nor the backup has it whole
    bool from_backup = false;                // some or all of `addons` are the backup's
    bool newer = false;                      // installed.txt is a newer Hailstone's format: never written over
    std::string problem;                     // what is wrong with installed.txt ("" nothing)
    // Whether installed.txt may be written (an install, an update, a removal, a switch): not over a newer
    // Hailstone's, and not while that would drop an add-on that does not read.
    bool writable() const { return !newer && unreadable.empty(); }
};
AddonRegistry read_addon_registry(const std::filesystem::path& data_dir);

// read_addon_registry's records that read.
std::vector<InstalledAddon> read_installed_addons(const std::filesystem::path& data_dir);
// Managed activation records, including disabled/modified ones so they cannot fall back to manual autorun.
std::vector<mq::ManagedScript> managed_addon_scripts(const std::filesystem::path& data_dir);
bool set_addon_enabled(const std::filesystem::path& data_dir, std::string_view name, bool enabled);
// Writes installed.txt and its backup, installed.bak.txt; false (written nowhere) when it cannot, or may not be
// written now (AddonRegistry::writable).
bool write_installed_addons(const std::filesystem::path& data_dir, const std::vector<InstalledAddon>& addons);
// The add-on named `name` (any case), or null.
const InstalledAddon* find_addon(const std::vector<InstalledAddon>& addons, std::string_view name);
// The add-on named `name` of `kind` when it comes from the repository `repo` (owner/repo, whatever tag or branch)
// and is signed with `key`, else null. For the client's own buttons that start one add-on they know by name
// (Options' Texture Studio...): another add-on that took the name, or one not signed by its author, is not taken for
// it.
const InstalledAddon* find_pinned_addon(const std::vector<InstalledAddon>& addons, std::string_view name,
                                        AddonKind kind, std::string_view repo, const Ed25519PublicKey& key);

// The files of an installed add-on that are gone or no longer what it shipped (the player changed them).
std::vector<std::string> modified_addon_files(const std::filesystem::path& data_dir, const InstalledAddon& addon);
// A file's SHA-256, remembered by its path, size and modification time so that an unchanged file is not read again
// (the opted-in add-on policy checks the required packages and the running upscaler at each zone-in, on the client's
// thread). nullopt when it cannot be read. Any thread.
std::optional<Sha256Digest> addon_file_sha256(const std::filesystem::path& file);
// The file is there with this size and SHA-256 (by addon_file_sha256).
bool addon_file_matches(const std::filesystem::path& file, std::uint64_t size, const Sha256Digest& sha256);

// Why `name` cannot be installed as a new add-on of this kind ("" it can): a reserved name, another add-on's, or a
// folder already there that is not an add-on's (installed by hand, or by Hailstone's own updates).
std::string addon_name_taken(const std::filesystem::path& data_dir, const std::vector<InstalledAddon>& addons,
                             AddonKind kind, std::string_view name);

// Whether `folder` (relative to the data folder, "plugins/x") is an installed add-on's.
bool addon_owns_folder(const std::filesystem::path& data_dir, std::string_view folder);

// Checks the package zip at `zip` for `manifest` on `platform` (update_platform()'s name) and unpacks it into
// addons/new/<name>/. The files it holds, or nullopt with *error why the whole package is refused.
std::optional<std::vector<AddonFile>> unpack_addon(const std::filesystem::path& data_dir, const AddonManifest& manifest,
                                                   const std::filesystem::path& zip, std::string_view platform,
                                                   std::string* error);

// Whether `text` compiles as a Lua chunk (text only, not run); *error the compiler's message when it does not.
bool lua_compiles(std::string_view text, std::string_view chunk_name, std::string* error);

// Moves addons/new/<name>/ into the add-on's folder and records `addon` (its files are the unpacked ones) in
// installed.txt in place of any earlier version. Files the earlier version shipped and this one does not are taken
// out; the player's other files there (a plugin's settings) stay. On failure the folder is put back as it was. An
// earlier version's auto-update switch is kept.
bool apply_addon(const std::filesystem::path& data_dir, const InstalledAddon& addon, std::string* error);
// Takes out the files the add-on shipped and its record; the player's other files in its folder stay (the folder
// goes when it is left empty).
bool remove_addon(const std::filesystem::path& data_dir, std::string_view name, std::string* error);

// The capability ceilings of the installed add-ons' files (plugin::Policy::set_ceilings): a script's Lua files and a
// plugin's library, each with the capabilities the player accepted for its add-on.
std::vector<plugin::Ceiling> addon_ceilings(const std::vector<InstalledAddon>& addons);
// The ceilings of the data folder's add-ons: addon_ceilings of the records that read, and a ceiling of no
// capabilities on each Lua file and library now in the folder of an add-on whose record does not
// (AddonRegistry::unreadable), each logged.
// Only an opted-in server needs the additional scan of readable add-ons for changed/extra code identities.
std::vector<plugin::Ceiling> addon_ceilings(const std::filesystem::path& data_dir, bool scan_for_enforcement = false);

// First thing at start: an install or a removal that was cut off is undone, and what it left in new/ and previous/
// goes. A lost or damaged installed.txt is written again from its backup when that has every add-on it names; else
// the log says what is wrong.
void addons_at_start(const std::filesystem::path& data_dir);

} // namespace hailstone::app
