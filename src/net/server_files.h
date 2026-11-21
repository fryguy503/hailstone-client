#pragma once

// Keeps a server profile's folder in step with the server's client_files/ (net/server_files_packets.h,
// docs/layout.md "Server files"). The server's client_files/ holds its hailstone.cfg (plugin/policy.h), the files it
// produces (spells_us.txt, Resources/SkillCaps.txt ...), and assets/, plugins/ and scripts/; they land in
// servers/<profile>/ as hailstone.cfg, assets/ (client_files/assets/ and every other file), plugins/ and scripts/
// (server_file_destination). At the first zone-in of a login the connection holds the zone-in (it neither asks for
// the zone's spawns nor loads the zone) while this:
//   1. asks the zone for the server's list of files (path, size, SHA-256);
//   2. compares it with the profile's folder: a file is current when its size and hash match. What this put there
//      is recorded in servers/<profile>/server_files.txt, so its files are not hashed again;
//   3. downloads the others, a piece at a time with several pieces asked ahead, into servers/<profile>/.download/,
//      and checks each one's hash. A file already there and complete (an earlier, broken-off download) is kept;
//   4. once all have come, moves them into place and deletes the files it put there earlier that the server no
//      longer lists. Files the player added by hand that the server does not list stay.
// Nothing changes until every file has come, so a failed or broken-off download leaves the old files.
// A server without Hailstone's script does not answer, and after kListTimeout the zone-in goes on as it was (after
// kKnownListTimeout for a server that sent its files before: its zone may be hashing new files).
// Paths are checked (core rules in is_safe_server_file_path); programs are refused, except Lua in scripts/ and in a
// plugin's ui/, and this platform's libraries in plugins/.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/sha256.h"
#include "net/packet.h"
#include "net/server_files_packets.h"
#include "plugin/policy.h"

namespace hailstone::net {

// A path the server may send: relative, '/' separators, no empty, "." or ".." parts, no part starting with '.', no
// ':' or '\\' or control characters, and not a program or script (.exe, .dll, .so, .lua ...): data only.
bool is_safe_server_file_path(std::string_view path);
// The same rules on a path's shape alone, whatever kind of file it is (an add-on tool's package, docs/addons.md).
bool is_safe_file_path_shape(std::string_view path);

// This build's plugin platform folder ("linux-x64", "windows-x64", "macos-arm64" ...).
std::string_view plugin_platform();

// Where a file the server lists goes in servers/<profile>/, or nullopt when this client does not take it:
//   hailstone.cfg                  -> hailstone.cfg
//   scripts/<path>                 -> scripts/<path>: Lua and data, no other programs
//   plugins/[<platform>/]<path>    -> plugins/<path>: this platform's libraries (.so, .dll or .dylib), data, and
//                                     Lua in a plugin's ui/ (plugins/<name>/ui/...), no other programs. A platform folder (plugin_platform()) not this build's, or
//                                     another platform's library, is skipped (`other_platform` set), not refused.
//   assets/<path>, any other path  -> assets/<path>: data only.
std::optional<std::string> server_file_destination(std::string_view path, bool* other_platform = nullptr);

// With [assets] player = off in the server's hailstone.cfg (plugin/policy.h): the files of servers/<profile>/assets/
// that count, as GameDir names (normalized, relative to assets/), the server's own. nullopt: every file counts (no
// hailstone.cfg, or it allows the player's). Textures always count (asset_counts).
std::optional<std::set<std::string>> server_asset_only(const std::filesystem::path& profile_dir);

// The server's rules as its last login left them in `profile_dir` (its hailstone.cfg and the files recorded with
// their hashes), marked provisional (plugin/policy.h): what may run before this login's zone-in has checked the
// server's files. No rules when the folder has none (a server never visited, or without Hailstone's script).
plugin::Rules cached_server_rules(const std::filesystem::path& profile_dir);
// Whether a file of the server's assets/ counts under server_asset_only's set: one of them, or a texture (.dds, .bmp,
// .tga, .png, .jpg), which changes no play.
bool asset_counts(const std::set<std::string>& only, std::string_view normalized);

// What a server may make this write (Hailstone's bounds; hailstone.ini's assets.server_files_max_file_mb and
// assets.server_files_max_total_mb, docs/layout.md "Server files"). A file larger than max_file is refused (not
// downloaded, like an unsafe name); when what is to come adds up to more than max_total the sync fails before
// anything is written. The defaults are far above the largest server measured (1.1 GB in all).
struct ServerFileLimits {
    std::uint64_t max_file = std::uint64_t{2} << 30;   // 2 GiB
    std::uint64_t max_total = std::uint64_t{16} << 30; // 16 GiB
};

class ServerFileSync {
public:
    using Clock = std::chrono::steady_clock;
    using Send = std::function<void(AppPacket)>;
    // Verifying: the list came, and the files already in the profile's folder (or left complete in .download/) that
    // the record does not vouch for are being hashed, kHashBytesPerUpdate a call, so the loading screen keeps drawing.
    enum class State { Idle, Listing, Verifying, Downloading, Done, NoAnswer, Failed };

    static constexpr std::chrono::seconds kListTimeout{5};
    static constexpr std::chrono::seconds kKnownListTimeout{30};
    static constexpr std::chrono::seconds kChunkTimeout{30};
    // A piece that does not come is asked for again: at once when one asked after it comes (the server answers in
    // order, so it skipped this one: an older Hailstone script dropped requests over its limit), or when nothing has
    // come for kChunkRetry. At most kChunkRetries times: a piece skipped once more then fails the sync, while a
    // silence past the last request waits for kChunkTimeout.
    static constexpr std::chrono::seconds kChunkRetry{3};
    static constexpr int kChunkRetries = 5;
    // Pieces asked ahead. Measured on one machine (2026-10-26): 16 gave 8 MB/s, 32 16 MB/s, 64 23 MB/s, but 64 (2 MB
    // in one burst) overflowed a 128 KiB socket buffer and stalled on EQEmu's backed-off resends, and Linux caps
    // the buffer platform/udp_socket.cpp asks for at net.core.rmem_max (often ~200 KiB).
    static constexpr int kWindow = 32;
    // The bytes hashed in one update() call while verifying, or while downloading a file whose pieces came out of
    // order (a few tens of ms); handle() hashes only plan()'s first share, once a list.
    static constexpr std::uint64_t kHashBytesPerUpdate = std::uint64_t{16} << 20;

    using Limits = ServerFileLimits;
    enum class Scope { All, ConfigOnly };

    // ConfigOnly uses a separate folder so a transient login leaves the full download's files and record alone.
    ServerFileSync(std::filesystem::path profile_dir, Send send, Limits limits = {}, Scope scope = Scope::All);
    ~ServerFileSync();

    void start(Clock::time_point now);
    void handle(const ServerFilesReply& reply, Clock::time_point now);
    void update(Clock::time_point now);

    State state() const { return state_; }
    // Done, NoAnswer or Failed: the zone-in may go on.
    bool finished() const { return state_ == State::Done || state_ == State::NoAnswer || state_ == State::Failed; }
    // Files in assets/ were added, replaced or deleted.
    bool changed() const { return changed_; }
    // The server's files now in the profile's folder, by their place there ("hailstone.cfg", "scripts/x.lua") and
    // hash: what it sent when Done, what an earlier login left when Failed.
    struct Installed {
        std::string path;
        Sha256Digest sha256{};
    };
    std::vector<Installed> installed();
    const std::string& error() const { return error_; }
    // For the loading screen: what it is doing, and 0..1 (< 0 while it lists).
    std::string status() const;
    float progress() const;
    std::size_t files_needed() const { return needed_.size(); }
    std::uint64_t bytes_needed() const { return bytes_total_; }
    // Tests: a smaller verifying budget than kHashBytesPerUpdate.
    void set_hash_budget(std::uint64_t bytes) { hash_budget_ = bytes; }

private:
    struct Incoming {
        std::ofstream out;
        std::uint32_t received = 0;
        std::uint32_t next = 0; // the offset the next piece should have, to hash as they come
        bool in_order = true;
        Sha256 hash;
        // Per kServerFilesChunkMax piece: the number of its request in asks_ while asked for and not yet come, else
        // 0. A piece that comes unasked or twice is ignored, so it neither counts twice toward `received` nor takes
        // outstanding_ below what is in flight.
        std::vector<std::uint64_t> pending;
    };

    std::filesystem::path staging_dir() const { return dir_ / ".download"; }
    // What is in the folders the server's files go to, by lower-cased place in the profile's folder.
    std::unordered_map<std::string, std::filesystem::path> present_files() const;
    std::filesystem::path record_file() const { return dir_ / "server_files.txt"; }
    void fail(std::string message);
    void plan(Clock::time_point now);
    // Hashes what plan() left to check, at most hash_budget_ bytes; true when all are checked.
    bool verify();
    void finish_plan(Clock::time_point now);
    void ask_more();
    void ask(std::uint32_t index, std::uint32_t offset, std::uint32_t length, int tries);
    void ask_again(std::uint64_t number);
    void received_file(std::uint32_t index);
    // A downloaded file's hash against the list's: ready, or the sync fails.
    void checked_file(std::uint32_t index, const std::optional<Sha256Digest>& got);
    // Hashes rehash_'s files from .download/, at most hash_budget_ bytes.
    void rehash();
    // Hashes `path` into hashing_, at most `budget` bytes of it; true when done, with `got` (empty: unreadable).
    bool hash_more(const std::filesystem::path& path, std::uint64_t& budget, std::vector<char>& buf,
                   std::optional<Sha256Digest>& got);
    void commit();
    void load_record();
    void save_record() const;

    std::filesystem::path dir_;
    Send send_;
    Limits limits_;
    Scope scope_;
    State state_ = State::Idle;
    bool changed_ = false;
    std::string error_;
    Clock::time_point last_reply_{};
    Clock::duration list_timeout_ = kListTimeout;
    Clock::time_point download_started_{};
    int restarts_ = 0;

    // The server's list as it comes.
    std::uint32_t list_id_ = 0;
    std::uint32_t total_ = 0;
    std::vector<ServerFileEntry> list_;
    std::vector<std::string> dest_; // by index into list_: its place in the profile's folder (server_file_destination)

    // What this put in the profile's folder (lower-cased place -> size and hash).
    struct Recorded {
        std::string path;
        std::uint32_t size = 0;
        Sha256Digest sha256{};
    };
    std::map<std::string, Recorded> record_;

    std::vector<std::uint32_t> needed_;  // indexes into list_, in order
    std::vector<std::uint32_t> ready_;   // in staging_dir(), checked
    std::vector<bool> staged_;           // by index into list_: in ready_
    std::vector<std::string> remove_;    // lower-cased paths in record_ the server no longer lists
    // plan()'s files to hash: the copy in the profile's folder (when its size matches but the record does not vouch
    // for it), then the one in .download/ (when its size matches). Checked in order, kHashBytesPerUpdate a call.
    struct Check {
        std::uint32_t index = 0;
        std::string key;              // lower-cased place
        std::filesystem::path present; // empty: none to hash
        std::filesystem::path staged;  // empty: none to hash
        int step = 0;                  // 0 the present copy, 1 the staged one, 2 decided
    };
    std::vector<Check> checks_;
    std::size_t next_check_ = 0;
    struct Hashing {
        std::ifstream in;
        Sha256 hash;
    };
    std::unique_ptr<Hashing> hashing_; // the file being hashed across calls (verify(), or rehash() while downloading)
    // Files whose pieces all came but out of order (one asked for again): hashed from disk by update(), in order.
    std::vector<std::uint32_t> rehash_;
    std::unordered_map<std::string, bool> listed_; // plan(): the places the list names
    std::size_t refused_ = 0, skipped_ = 0;
    std::uint64_t hash_budget_ = kHashBytesPerUpdate;
    std::size_t next_file_ = 0;          // into needed_: the file pieces are asked from
    std::uint32_t next_offset_ = 0;
    int outstanding_ = 0;
    // The pieces asked for and not yet come, by the order they were asked in.
    struct Ask {
        std::uint32_t index = 0, offset = 0, length = 0;
        int tries = 1;
    };
    std::map<std::uint64_t, Ask> asks_;
    std::uint64_t ask_number_ = 0;
    Clock::time_point last_piece_{}; // a piece came, or the silent ones were asked for again
    std::unordered_map<std::uint32_t, std::unique_ptr<Incoming>> incoming_;
    std::uint64_t bytes_total_ = 0, bytes_done_ = 0;
    std::string current_;
    // Download speed: bytes over the last second, smoothed.
    Clock::time_point rate_since_{};
    std::uint64_t rate_bytes_ = 0;
    double rate_ = 0;
};

} // namespace hailstone::net
