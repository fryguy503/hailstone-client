#pragma once

// What a server lets compiled plugins and MacroQuest Lua scripts do, from the hailstone.cfg in its client_files/
// (docs/plugins.md). A server without one sets no rules: everything runs with every capability. A server with one
// always runs its own plugins and scripts (those it sent into servers/<profile>/plugins and scripts, named by their
// SHA-256), with every capability; the player's own run only when the file allows them, with the capabilities it
// grants. The optional [commands] section is a server-opted extension to RoF2 behavior. It advises Hailstone's
// slash-command dispatch for players and add-ons. The game server still owns authorization for gameplay actions.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/ascii_case.h"
#include "core/sha256.h"
#include "core/version.h"

namespace hailstone::game { class EqStrings; }

namespace hailstone::plugin {

// Capability bits (the policy entries' caps).
enum Cap : std::uint32_t {
    kCapRead = 1u << 0,     // read game state: TLOs, the host API's queries
    kCapCommands = 1u << 1, // slash commands not in another group (windows, options, /who, plugin commands ...)
    kCapTarget = 1u << 2,   // change the target: /target, /assist, /xtarget, ...
    kCapActions = 1u << 3,  // act: cast, disciplines, abilities, items, attack, memorise spells
    kCapMove = 1u << 4,     // move: face, keypress movement, follow, sit / stand / duck, MQ /stick and /nav
    kCapChat = 1u << 5,     // speak: say, tells, group / guild / raid / channels, emotes
    kCapEvents = 1u << 6,   // see chat lines (mq.event, OnIncomingChat) and change or hide them
    kCapUi = 1u << 7,       // draw windows (mq.imgui, OnUpdateImGui)
    kCapPackets = 1u << 8,  // see, change, drop and send raw packets (compiled plugins)
    kCapExtend = 1u << 9,   // change the client's rules as a server's client does (server extension hooks)
    kCapLogin = 1u << 10,   // run before the zone-in: from the world login, at character select (provisional rules)
    kCapNet = 1u << 11,     // open the plugin's own TCP connections (a server's service beside the game)
    kCapWeb = 1u << 12,     // open web pages: /url, /www (core/web_url.h: http(s) only, [web] allow)
    kCapAll = (1u << 13) - 1,
};

// "read,commands,..." / the bits for such a list ("all" for every bit); parse_caps ignores unknown words.
std::string caps_text(std::uint32_t caps);
std::uint32_t parse_caps(std::string_view text);
// The same, but nullopt when a word is not a capability ("all" and "none" are taken): an add-on's manifest.
std::optional<std::uint32_t> parse_caps_strict(std::string_view text);
// What each capability in `caps` lets a script or plugin do, in plain words for the player ("read the game state").
std::vector<std::string> caps_described(std::uint32_t caps);

// The capability a command line needs when a plugin or script runs it (EQ's own commands by their table entry,
// game/slash_commands.h; plain text and the chat server's ';' lines are chat). Words the table does not know (MQ and
// plugin commands) need kCapCommands; their own handlers check anything further.
std::uint32_t command_caps(std::string_view line, const game::EqStrings* strings = nullptr);

// What the code running now may do: kCapAll while the player acts (typing, keys, buttons); a plugin's or script's
// capabilities while it runs (CallerScope). Commands check it (mq/commands.h), so a command a script runs through
// any path (mq.cmd, /docommand, a plugin's DoCommand) is held to the script's capabilities.
//
// The client's thread only: plugins and scripts are called there, and so must call back. On another thread (a
// thread a plugin started) caller_caps() is 0, caller_name() "another thread", and a CallerScope does nothing.
std::uint32_t caller_caps();
std::string_view caller_name(); // "" for the player

// A server TLO grant can be used only from code loaded from a verified installed add-on file. Runtime owners
// keep the proof alive. Deferrals hold a weak reference so unloading or invalidating the code revokes it.
// A new policy (Policy::set, set_ceilings) leaves every proof without an identity until its owner re-verifies
// its file and renews it: renewing keeps the same object, so the work it deferred (a /timed line, a queued
// event) uses the grant the new policy gives, if any. An owner whose file no longer verifies revokes it instead.
class VerifiedTloCaller {
public:
    std::string_view identity() const { return revoked_ || generation_.expired() ? std::string_view{} : source_; }
    bool same_addon(const VerifiedTloCaller& other) const { return source_ == other.source_; }
    void revoke() const { revoked_ = true; }
    // Takes `current`'s generation: a proof Policy::verified_tlo_caller has just made for the same add-on. Only
    // the Policy makes proofs, so this cannot lend an identity; a revoked proof stays revoked.
    void renew(const VerifiedTloCaller& current) const {
        if (same_addon(current)) generation_ = current.generation_;
    }

private:
    friend class Policy;
    VerifiedTloCaller(std::string_view source, const std::shared_ptr<const void>& generation)
        : source_(source), generation_(generation) {}
    std::string source_;
    mutable std::weak_ptr<const void> generation_;
    mutable bool revoked_ = false;
};
using TloCaller = std::shared_ptr<const VerifiedTloCaller>;
using WeakTloCaller = std::weak_ptr<const VerifiedTloCaller>;
// nullopt means no code is on the stack (player); an empty weak reference means code with no grant.
std::optional<WeakTloCaller> caller_tlo_proof();
bool may_inherit_tlo_proof(const TloCaller& proof);

class CallerScope {
public:
    CallerScope(std::uint32_t caps, std::string_view name, WeakTloCaller tlo = {});
    ~CallerScope();
    CallerScope(const CallerScope&) = delete;
    CallerScope& operator=(const CallerScope&) = delete;

private:
    friend std::optional<WeakTloCaller> caller_tlo_proof();
    bool active_ = false;
    std::uint32_t prev_caps_ = 0;
    std::string prev_name_;
    const CallerScope* prev_tlo_scope_ = nullptr;
    WeakTloCaller tlo_;
};
// While it lives, the code runs as the client itself: every capability, no add-on's name ("" is the player's).
// For a client pass that runs an add-on's retained callback while another add-on's code is what called in (the map
// overlays: one add-on draws the map, a callback of a second is drawn into it). CallerScope cannot lift a scope (it
// intersects with the previous one), so the pass raises it here and the callback's own CallerScope then narrows it.
class HostCallerScope {
public:
    HostCallerScope();
    ~HostCallerScope();
    HostCallerScope(const HostCallerScope&) = delete;
    HostCallerScope& operator=(const HostCallerScope&) = delete;

private:
    bool active_ = false;
    std::uint32_t prev_caps_ = 0;
    std::string prev_name_;
};
// Whether this is the client's thread: the one that started the program (taken before main runs), or a plugin's
// thread the client's thread is lent to for one host call (ThreadLent).
bool on_main_thread();
// While it lives, this thread acts as the client's: on_main_thread() is true here. For a host call from a plugin's
// own thread while the plugin lends it the client's thread (lend_thread, native_plugins.cpp), made under the lock
// that keeps every host call one at a time meanwhile.
class ThreadLent {
public:
    ThreadLent();
    ~ThreadLent();
    ThreadLent(const ThreadLent&) = delete;
    ThreadLent& operator=(const ThreadLent&) = delete;

private:
    bool prev_ = false;
};
// Whether a ThreadLent is live on this thread (on_main_thread() is true here, though it is not the client's own).
bool on_lent_thread();

enum class FileKind : std::uint8_t { Plugin = 1, Script = 2 };

// What Lua socials (game/lua_social.h) may do. Off: only the stock five-line socials run. Press: a social's Lua runs
// to its end on the key press, without waiting or chat events, and sends at most `press_commands` commands. Full:
// like a script (mq.delay, events), within `caps`.
enum class ButtonMode : std::uint8_t { Off = 0, Press = 1, Full = 2 };

struct ButtonRules {
    ButtonMode mode = ButtonMode::Off;
    std::uint32_t caps = 0;
    int press_commands = 0; // press mode: the commands one press may send (0: no limit)
    bool faces = false;     // button faces may read the game state
};

// A script or plugin file the server sent: its path in the server's folder (servers/<profile>/, "scripts/x.lua",
// "plugins/x/x.so") and its SHA-256.
struct ServerFile {
    FileKind kind = FileKind::Script;
    std::string path;
    Sha256Digest sha256{};
};

// The file's name in the server's client_files/ and in servers/<profile>/.
inline constexpr std::string_view kServerCfgName = "hailstone.cfg";

// Opt-in add-on identities ([addons] in hailstone.cfg, docs/plugins.md "Opt-in add-on policies"): GitHub
// repositories, "owner/repo" (a GitHub link to one is taken too), compared without case and without the installed
// release tag or branch. A bare manifest name is not an identity: any package of that name would satisfy it. These
// checks restrict identities; the script/plugin capabilities still apply. The server's own files are not add-ons and
// are never named or refused by them.
struct AddonRules {
    bool enabled = false;
    bool default_allow = true;
    std::vector<std::string> required, allow, block; // in normalize_addon_identity's form
    // `required = owner/repo:1.0.0`: the oldest installed manifest version that meets that requirement (SemVer
    // order, core/version.h). A repository named more than once keeps the highest. Absent: any version.
    std::map<std::string, Version> minimum_versions;
    std::string error; // an invalid enabled policy refuses every add-on until the server fixes it
};

// "owner/repo" lower-cased for a repository or a GitHub link to one; nullopt for anything else, a bare name included.
std::optional<std::string> normalize_addon_identity(std::string_view identity);
// normalize_addon_identity's form of an add-on's recorded source ("owner/repo@v1.2"), "" when it has none.
std::string addon_identity(std::string_view source);
bool addon_identity_matches(std::string_view identity, std::string_view source);
// By the add-on's recorded source (installed.txt); an add-on without one (put in by hand) is named by no list.
bool addon_required(const AddonRules& rules, std::string_view source);
bool addon_allowed(const AddonRules& rules, std::string_view source);

enum class CommandOrigin { Stock, Extended, Addon };

struct CommandCatalogEntry {
    std::string name, description;
    CommandOrigin origin = CommandOrigin::Stock;
    std::vector<std::string> aliases, groups;
    bool gm_only = false;
};

std::vector<CommandCatalogEntry> command_catalog(std::span<const std::string> registered = {});
std::string_view command_origin_text(CommandOrigin origin);
// The same spelling used by policy for aliases and built-in abbreviations. Unknown names are retained for add-ons.
std::string canonical_command_name(std::string_view name);
bool known_extended_command(std::string_view name);
bool known_command_group(std::string_view name);

inline constexpr std::size_t kCommandBlockedMessageMaxBytes = 240;
inline constexpr std::string_view kDefaultCommandBlockedMessage = "This command has been blocked by your server administrator.";

struct CommandDecision {
    bool allowed;
    std::string_view reason;
    std::optional<int> minimum_status;
};

struct CommandRules {
    bool enabled = false, default_allow = true;
    std::vector<std::string> allow, block, allow_groups, block_groups;
    // A malformed entry with a known spelling remains a denial until it is corrected.
    std::map<std::string, std::optional<int>, std::less<>> gm;
    std::string blocked_message;
    CommandDecision decision(std::string_view name, std::optional<int> account_status = std::nullopt) const;
    bool allowed(std::string_view name, std::optional<int> account_status = std::nullopt) const {
        return decision(name, account_status).allowed;
    }
    std::string_view denial_message() const {
        return blocked_message.empty() ? kDefaultCommandBlockedMessage : std::string_view(blocked_message);
    }
};

// Advisory client policy for named TLO lookups. The default preserves existing behavior.
struct TloRules {
    struct NameLess {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const { return icompare(a, b) < 0; }
    };
    bool default_allow = true;
    std::set<std::string, NameLess> allow, block;
    std::map<std::string, std::set<std::string, NameLess>, NameLess> addon_allow;

    bool allowed(std::string_view name, std::string_view verified_addon = {}) const {
        if (!block.contains(name) && (default_allow || allow.contains(name))) return true;
        const auto grant = addon_allow.find(verified_addon);
        return !verified_addon.empty() && grant != addon_allow.end() && grant->second.contains(name);
    }
};

// The server's rules: its hailstone.cfg, or none.
struct Rules {
    bool configured = false; // false: no hailstone.cfg, no rules
    // Before the zone-in's check of the server's files (world login, character select): the rules as the last login
    // left them (net/server_files.h cached_server_rules). Only files whose capabilities include kCapLogin run; the
    // zone-in's rules replace these.
    bool provisional = false;
    std::filesystem::path dir; // servers/<profile>/ (empty: a login without one)
    bool deny_client = false; // [client] deny: stop Hailstone before it completes the first zone-in
    int management_min_status = 200; // [management] min_status: the encounter authorizes with the account status
    // The player's own scripts and plugins: what they may do (0: they do not run).
    std::uint32_t player_script_caps = 0, player_plugin_caps = 0;
    bool player_assets = true; // false: of servers/<profile>/assets, only the server's files and textures count
    std::vector<std::string> run_scripts; // the server's scripts started at each zone-in ([scripts] run)
    ButtonMode social_mode = ButtonMode::Press;
    std::uint32_t social_caps = kCapRead | kCapTarget | kCapActions;
    int social_press_commands = 5;
    bool social_faces = true;
    bool autoskill = true; // [abilities] autoskill: the Ability Book's autoskill may run (game/ability_book.h)
    bool map_vanilla = false; // [map] vanilla: only the map background, without marks or overlays
    bool npc_collision = true; // [movement] npc_collision: live NPCs stop the player (game/npc_collision.h)
    // [movement] passable_ladders: walking into a ladder whose polygons are all passable climbs it
    // (CollisionMesh::touches_passable_ladder).
    bool passable_ladders = true;
    // [movement] solid_ladders: the player's movement meets a ladder as one solid slab, not its rails and rungs
    // (game/zone_ladders.h, CollisionMesh::set_solid_ladders).
    bool solid_ladders = true;
    // [web] allow: the hosts scripts and plugins may open (and those below them); empty: any (core/web_url.h).
    std::vector<std::string> web_allow;
    // [addons] suggest: add-ons the server suggests to its players (docs/addons.md "Suggested add-ons"), as
    // addon_source_text writes them; the login screen offers each one the player does not have.
    std::vector<std::string> suggest_addons;
    AddonRules addons;
    CommandRules commands;
    TloRules tlos;
    std::vector<ServerFile> server_files; // the server's scripts and plugins
    // Custom and formerly custom sections for hailstone.server_rule. Case and inline comments follow the
    // existing script lookup behavior.
    std::map<std::string, std::map<std::string, std::string>> script_sections;
};

// Read hailstone.cfg as rules, using the defaults above for omitted settings. Unknown keys and values are named
// in `warnings` and ignored. Unknown sections are warned about and kept for scripts (Rules::script_sections).
Rules parse_server_cfg(std::string_view text, std::vector<std::string>* warnings = nullptr);
// The rules for a server whose folder is `dir`, from the files it listed at the zone-in ("hailstone.cfg",
// "scripts/...", "plugins/..."; paths in the server's folder). Without hailstone.cfg among them: no rules. A
// hailstone.cfg that does not match its listed hash (changed on disk) gives the defaults.
struct ListedFile {
    std::string path;
    Sha256Digest sha256{};
};
Rules server_rules(const std::filesystem::path& dir, const std::vector<ListedFile>& files,
                   std::vector<std::string>* warnings = nullptr);

// An installed add-on's file (docs/addons.md) and the capabilities its manifest declared: a ceiling on what the file
// gets, whatever the server allows. A file the player changed has another hash and is the player's own again.
struct Ceiling {
    FileKind kind = FileKind::Script;
    Sha256Digest sha256{};
    std::uint32_t caps = 0;
    std::string addon_name{}, addon_source{};
    bool verified = true; // false: edited/extra bytes retain source identity only for opt-in blocking
    bool entry_point = true; // scripts: only init.lua; modules still carry identity/capability restrictions
    std::filesystem::path installed_path{}; // exact managed package file, absent on legacy/unverified rows
};

class Policy {
public:
    void set(Rules rules);
    // The add-ons' ceilings (they stay across set() and clear(): they are the player's, not the server's). Their
    // sources are kept in addon_identity's form.
    void set_ceilings(std::vector<Ceiling> ceilings);
    std::string_view verified_tlo_identity(FileKind kind, const Sha256Digest& hash, const std::filesystem::path& file,
                                           bool entry_point) const;
    TloCaller verified_tlo_caller(FileKind kind, const Sha256Digest& hash, const std::filesystem::path& file) const;
    // The ceiling on a file with this hash, nullopt none (a file not an add-on's).
    std::optional<std::uint32_t> ceiling(FileKind kind, const Sha256Digest& hash) const;
    void clear(); // nothing runs (out of the world, or the server's files not checked yet)
    bool received() const { return rules_.has_value(); }
    const std::optional<Rules>& current() const { return rules_; }
    // Changes whenever set() or clear() is called (holders re-check what they run).
    std::uint32_t serial() const { return serial_; }

    // The capabilities for a file with this hash; 0 = it may not run. An add-on's file gets no more than its ceiling.
    std::uint32_t caps(FileKind kind, const Sha256Digest& hash, std::string_view name = {}) const;
    // A required installed add-on's file whose hash still identifies it, even when another rule refuses it.
    // Autostart must check caps too; readiness must retain refused requirements so they cannot be skipped.
    bool required_file(FileKind kind, const Sha256Digest& hash) const;
    // Expected entrypoints of the required installed add-ons (from the registry), independent of disk discovery or
    // permission: a missing/shadowed runtime file must still fail readiness instead of disappearing from them.
    std::vector<std::pair<std::string, Sha256Digest>> required_entries(FileKind kind) const;
    // How a required add-on's file is named to the player: its repository (owner/repo, as the server's `required`
    // names it), else `name` (a file with no recorded source).
    std::string required_label(FileKind kind, const Sha256Digest& hash, std::string_view name) const;
    // One of the server's own files (under a hailstone.cfg: it always runs, and the player cannot stop it).
    bool server_file(FileKind kind, const Sha256Digest& hash) const;
    // Why a file with caps 0 may not run, for the player.
    std::string refusal(FileKind kind) const;
    ButtonRules button_rules() const;

private:
    bool addon_file_allowed(FileKind kind, const Sha256Digest& hash, std::string_view name) const;
    std::optional<Rules> rules_;
    std::vector<Ceiling> ceilings_;
    std::shared_ptr<const void> tlo_generation_;
    std::uint32_t serial_ = 0;
};

} // namespace hailstone::plugin
