#pragma once

// What a server lets compiled plugins and MacroQuest Lua scripts do, from the hailstone.cfg in its client_files/
// (docs/plugins.md). A server without one sets no rules: everything runs with every capability. A server with one
// always runs its own plugins and scripts (those it sent into servers/<profile>/plugins and scripts, named by their
// SHA-256), with every capability; the player's own run only when the file allows them, with the capabilities it
// grants. The player's own typed commands are never limited; the capabilities check what plugins and scripts do on
// the player's behalf.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
class CallerScope {
public:
    CallerScope(std::uint32_t caps, std::string_view name);
    ~CallerScope();
    CallerScope(const CallerScope&) = delete;
    CallerScope& operator=(const CallerScope&) = delete;

private:
    bool active_ = false;
    std::uint32_t prev_caps_ = 0;
    std::string prev_name_;
};
// Whether this is the client's thread: the one that started the program (taken before main runs).
bool on_main_thread();

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

// The server's rules: its hailstone.cfg, or none.
struct Rules {
    bool configured = false; // false: no hailstone.cfg, no rules
    // Before the zone-in's check of the server's files (world login, character select): the rules as the last login
    // left them (net/server_files.h cached_server_rules). Only files whose capabilities include kCapLogin run; the
    // zone-in's rules replace these.
    bool provisional = false;
    std::filesystem::path dir; // servers/<profile>/ (empty: a login without one)
    // The player's own scripts and plugins: what they may do (0: they do not run).
    std::uint32_t player_script_caps = 0, player_plugin_caps = 0;
    bool player_assets = true; // false: of servers/<profile>/assets, only the server's files and textures count
    std::vector<std::string> run_scripts; // the server's scripts started at each zone-in ([scripts] run)
    ButtonMode social_mode = ButtonMode::Press;
    std::uint32_t social_caps = kCapRead | kCapTarget | kCapActions;
    int social_press_commands = 5;
    bool social_faces = true;
    bool autoskill = true; // [abilities] autoskill: the Ability Book's autoskill may run (game/ability_book.h)
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
    std::vector<ServerFile> server_files; // the server's scripts and plugins
    // Sections that are not the client's own, for scripts (hailstone.server_rule) to read as they like: names and
    // keys lower-cased, values as written (comments and quotes taken off).
    std::map<std::string, std::map<std::string, std::string>> script_sections;
};

// hailstone.cfg's text (an ini file: [scripts], [plugins], [assets], [socials], [abilities], [movement], [web], [addons]) as rules; what it does
// not say keeps the defaults above. Unknown keys and values are named in `warnings` and ignored; another section is
// kept for scripts (Rules::script_sections), and named in `warnings` too, in case it is a misspelt one.
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
};

class Policy {
public:
    void set(Rules rules);
    // The add-ons' ceilings (they stay across set() and clear(): they are the player's, not the server's). Their
    // sources are kept in addon_identity's form.
    void set_ceilings(std::vector<Ceiling> ceilings);
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
    std::uint32_t serial_ = 0;
};

} // namespace hailstone::plugin
