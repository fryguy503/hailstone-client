#pragma once

// Social state and the pure rules behind the social slash commands: /who's
// filter words and its result lines, /random's numbers, /played and /time
// texts, tells and replies, friends and ignore lists, guild and raid state.
// Sources are the original's handlers (addresses
// below); the network side is net/game_connection_social.cpp.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/guild_ranks.h"

namespace hailstone::net { struct WhoResponse; struct WhoRequest; }

namespace hailstone::game {

class EqStrings;

// ---------------------------------------------------------------- /who

struct WhoContext {
    std::uint32_t own_guild_id = 0xffffffff;
    // A guild's id by name (CGuild::GetGuildIndex), nullopt when unknown.
    std::function<std::optional<std::uint32_t>(const std::string&)> guild_by_name;
    // True when the word is a zone's short name (stops race matching).
    std::function<bool(const std::string&)> is_zone_short_name;
};

struct WhoParse {
    std::optional<std::uint32_t> error; // eqstr id to show instead of sending
    bool friends = false;               // /who friends (OP_FriendsWho; not sent yet)
    std::uint32_t name_set = 0;
    std::string name;
    std::uint32_t race = 0xffffffff, class_ = 0xffffffff;
    std::uint32_t level_low = 0xffffffff, level_high = 0xffffffff;
    std::uint32_t gm = 0xffffffff;
    std::uint32_t guild = 0xffffffff;
    std::uint32_t type = 0;
};

// Who_BuildAndSendRequest @0x524400: up to six words. "all" (eqstr 13205)
// makes it a world /who (type 3), "corpse" type 2, "ripoff" type 4; "GM" /
// "NONGM" the GM filter; "friend(s)" the friends list; "LFG" guild -3,
// "Trader" -4 and "Buyer" -5 (not with all); "guild" the player's guild
// (12336 without one), "guild<n>" guild n; then a class (1..35) or race
// name prefix; numbers are the level range (one number: that level); a
// guild name the guild; anything else the name.
WhoParse parse_who_args(const std::string& args, const WhoContext& ctx, const EqStrings& strings);

// HandleWhoAllResponse @0x4bc010: the lines to print (colour 0x119), in
// order: the title and rule line, one per player, the footer.
// `multiclass_masks`: the class field is a bit mask of classes (NMS).
std::vector<std::string> format_who_response(const net::WhoResponse& r, const EqStrings& strings,
                                             bool multiclass_masks);

// EQ_Character::KunarkClass @0x4465a0 for a class and level: the eqstr id of the class name (levels below 51,
// or `base_name`) or of its level title (51..54, 55..59, 60..64, 65..69, 70..74, 75 and up; classes 1..16).
// Classes 20..35 (guildmasters), 40 (banker) and 41 (merchant) have names but no titles; other classes are
// eqstr 1484.
std::uint32_t kunark_class_eqstr(std::int32_t class_id, std::int32_t level, bool base_name);
// 0x44d420 (the /who class column): KunarkClass, and above level 50 " (" the base
// class name ")": "Champion (Warrior)".
std::string who_class_text(const EqStrings& strings, std::int32_t class_id, std::int32_t level);

// ---------------------------------------------------------------- spawn names

// CEverQuest::trimName's copy (0x8d3740): digits and '#' dropped, '_' turned into a space (the suffix
// localisation that follows leaves these names alone).
std::string trim_name(std::string_view name);
// PlayerManagerClient::GetPlayerFromPartialName @0x599710: among `names` (the spawn list's order) the one whose
// trimmed name starts with the trimmed `word` (ignoring case, as many characters as it has), the shortest raw
// name winning (the first of equal length), `exclude` skipped. nullopt for an empty trimmed word or no match.
std::optional<std::size_t> player_from_partial_name(std::string_view word, const std::vector<std::string>& names,
                                                    std::optional<std::size_t> exclude = std::nullopt);

// ---------------------------------------------------------------- /random

// Cmd_random @0x4df6c0: "/random", "/random N" or "/random A B". Returns the
// range to send (smaller first) or the eqstr id of the error.
struct RandomRange {
    std::uint32_t low = 0, high = 0;
    std::optional<std::uint32_t> error;
};
RandomRange parse_random_args(const std::string& args);

// ---------------------------------------------------------------- /played, /time

// The duration formatter @0x4f1600: "1 day, 2 hours, 3 minutes, and 4
// seconds" (eqstr 12969..12974, 6686..6691); zero parts are left out.
std::string format_duration(const EqStrings& strings, std::uint64_t seconds);

// MSVC's "%#x" (long date) and "%#c" (long date and time) for a calendar time.
std::string long_date(int year, int month, int day);
std::string long_date_time(int year, int month, int day, int hour, int minute, int second);

// CEverQuest's /time @0x5207a0: "Game Time: <%#x - %#I %p of the game date>"
// (eqstr 6706). EQ hours run 1..24 (OP_TimeOfDay); hour h is h-1 o'clock.
std::string format_game_time(const EqStrings& strings, int year, int month, int day, int hour);

// ---------------------------------------------------------------- chat

// msg_new_text @0x4c0080: the eqstr template and the dev chat kind for an
// OP_ChannelMessage channel. Language is shown as eqstr 1411 " in %1,".
struct ChannelFormat {
    std::uint32_t eqstr = 1410;
    bool has_language_slot = true; // the template's %2 is the language part
};
ChannelFormat channel_format(std::uint32_t channel);

// The eqstr echo for the player's own chat (send_chat 12344, send_gsay
// 12346, send_shout 12348, send_auction 12350, send_ooc 12352, guild 12263,
// raid 5109, fellowship 4574), or 0.
std::uint32_t own_chat_echo(std::uint32_t channel);

// ---------------------------------------------------------------- social emotes

// One of the 88 socials of CEverQuest::send_social @0x526910 (game/socials.inc,
// generated from the original's two jump tables by tools/gen_socials.py).
struct SocialEmote {
    int id = 0;
    int animation = -1; // OP_Animation action, -1 none
    std::uint32_t others_untargeted = 0, others_targeted = 0; // eqstr ids; 0 none
    const char* others_untargeted_slots = "";
    const char* others_targeted_slots = "";
    std::uint32_t echo_untargeted = 0, echo_targeted = 0;
    const char* echo_untargeted_slots = "";
    const char* echo_targeted_slots = "";
    bool needs_standing = false; // /frown /wave /cheer /gesture (eqstr 13069 otherwise)
};
// The social a command handler runs (Cmd_smile ...), or null.
const SocialEmote* social_for_handler(std::string_view handler);

// The texts send_social builds: what goes to the server as OP_Emote (the
// server puts the player's name in front; empty: nothing is sent) and the
// player's own line. `extra`: the text after the target word ("/smile Bob
// is here" → " is here." and "You emote 'is here'"). Genders: 0 male,
// 1 female, else neuter (the spawn's +0xeb9).
struct SocialText {
    std::string others;
    std::string echo;
};
SocialText build_social(const EqStrings& strings, const SocialEmote& e, const std::string* target_name,
                        int own_gender, int target_gender, const std::string& extra);

// ---------------------------------------------------------------- guilds

struct GuildMemberInfo {
    std::string name;
    std::uint32_t level = 0, class_ = 0, rank = 0, last_on = 0;
    std::uint16_t zone_id = 0;
    bool banker = false, alt = false;
    std::string public_note;
    // Guild tribute (OP_GuildMemberList, then OP_GuildOptInOut for the member; the original's member +0x3a0, +0x3a1,
    // +0x194, +0x198 and +0x10): opted in to the tribute and the trophy tribute, favour donated, the last donation
    // (a 32-bit time, 0 none), and the flag that shows the tribute cells white (yellow when clear).
    bool tribute_on = false, trophy_on = false, tribute_flag = true;
    std::uint32_t donations = 0, last_donation = 0;
    bool online() const { return zone_id != 0; }
};

struct GuildState {
    std::unordered_map<std::uint32_t, std::string> names; // OP_GuildsList
    std::uint32_t guild_id = 0xffffffff;                 // the player's (profile, SpawnAppearance 22)
    std::uint32_t rank = 0;                              // RoF2 ranks: 1 leader .. 8 recruit
    // PcBase::GuildStatus: the profile's rank, then only SpawnAppearance 23 (Handle_OP_SpawnAppearance case 0x17
    // compares it for "You are now the rank of %1."; the roster and OP_SetGuildRank leave it).
    std::uint32_t status = 0;
    std::string motd, motd_set_by;
    std::vector<GuildMemberInfo> members;                // OP_GuildMemberList
    GuildPermissions permissions;                        // the rank table (+0x9318; OP_GuildUpdate action 5)
    // A pending invitation (Handle_OP_GuildInvite @0x4b9410).
    std::string inviter;
    std::uint32_t invite_guild = 0, invite_rank = 0;
    std::int64_t invite_ms = 0;
    bool invite_world = false; // from another zone (OP_HailstoneGuildReply): answered with OP_HailstoneGuild
    // OP_GuildUpdate actions 0 and 1 (the guild's web page and chat channel) and 4 (a rank's name, 0x7d9520).
    std::string url, channel;
    std::array<std::string, 9> rank_names;

    bool in_guild() const { return guild_id != 0xffffffff && guild_id != 0; }
    std::string name_of(std::uint32_t id) const;
    std::optional<std::uint32_t> id_of(const std::string& name) const; // case-insensitive
    // 0x7d89e0: the name the guild gave the rank, else its default.
    std::string rank_name(std::uint32_t rank) const;
};
// 0x7ff350: the ranks' default names (table 0xb617d0, 1..8; "" otherwise).
std::string guild_rank_name(std::uint32_t rank);

// The chat line Handle_OP_SpawnAppearance @0x4b4b60 prints (colour 0x111) for a guild id (case 0x16) or rank (0x17)
// change; eqstr 0 for none.
struct GuildAppearanceLine {
    int eqstr = 0;
    std::vector<std::string> args;
};
// Case 0x16, before the spawn takes `id`. Our own spawn, when our guild (PcBase::GuildID) is not `id`: leaving (id
// negative as an int) 12880 with the old guild's name when CGuild::ValidGuildName knows it, else 12879; joining
// 12878 with the new guild's name. Another spawn whose guild was not `id`: 12881 "%1 has joined your guild." when
// `id` is ours (and not negative), else 12882 when its old guild was ours.
GuildAppearanceLine guild_id_appearance_line(const GuildState& g, bool own, std::uint32_t spawn_guild,
                                             std::uint32_t id, const std::string& spawn_name);
// Case 0x17, before the spawn takes `rank`; only ranks 1..8 print. Our own: 7655 with the rank's name when
// GuildStatus differs. Another spawn of our guild whose rank differs: 7656 (name, rank name).
GuildAppearanceLine guild_rank_appearance_line(const GuildState& g, bool own, std::uint32_t spawn_guild,
                                               std::uint32_t spawn_rank, std::uint32_t rank,
                                               const std::string& spawn_name);

// ---------------------------------------------------------------- raids

struct RaidMember {
    std::string name;
    std::uint32_t group = 0xffffffff; // 0..11, 0xffffffff ungrouped
    std::uint8_t class_ = 0, level = 0;
    bool group_leader = false;
    // RaidMember +0x84 / +0x85 / +0x86 (CRaid_SetMemberMainAssist / Marker / Waypointer, from OP_RaidDelegateAbility):
    // the raid window's Role column shows "A" for a main assist, then "M" for a marker (CRaidWnd_
    // SetMemberAssistMarkerFlags).
    bool main_assist = false, marker = false, waypointer = false;
    // RaidMember +0x88 RaidNote (64 bytes): the leader's note on the member (OP_RaidUpdate 0x24), shown in the raid
    // window's note list.
    std::string note;
};

struct RaidState {
    bool active = false;
    std::string leader;
    bool locked = false;
    std::string motd;
    std::vector<RaidMember> members;
    // A pending invitation (CRaid_HandleInvite @0x47dc30).
    std::string inviter;
    // The raid target member (CRaid +0x10c, CRaid::SetTargetRaidPlayer: a click in the raid window's lists), the
    // member the leader's buttons and /addraidlooter, /removeraidlooter act on; empty for none.
    std::string target;
    // The loot type (CRaid +0x3198: 1 leader only, 2 group leaders, 3 looters, 4 everyone; CRaid::ResetRaid sets 1)
    // and the raid looters (+0x319c, 19 names; OP_RaidUpdate 0x21 / 0x22).
    std::uint32_t loot_type = 1;
    std::vector<std::string> looters;
    // CRaid +0x2c00 raidId: set from OP_RaidUpdate 8 (+0x44, CRaid::RaidCreated) and the invitation's +0x44 when it is
    // accepted (CRaid::SendInviteResponse); the client's raid commands carry it at +0x44.
    std::uint32_t id = 0;
    // Local lifetime token for UI actions; never serialized. Stock servers may leave the wire raid ID at zero.
    std::uint64_t generation = 0;

    RaidMember* find(const std::string& name);
    bool is_member(const std::string& name) const;
    // Adds or updates a member (OP_RaidUpdate action 0). Returns true when new.
    bool add(const RaidMember& m);
    bool remove(const std::string& name);
    void clear();
};

// ---------------------------------------------------------------- social state

// ---------------------------------------------------------------- anonymity

// The anonymity flags (PlayerClient +0x2b8; Options/Anonymous 0xde0ba8): 1 anonymous, 2 roleplaying.
inline constexpr int kAnonFlag = 1, kRoleplayFlag = 2;

// What /anonymous or /roleplay does to the flags: the new flags and the line it prints (0: the flags stay, no
// packet; else the eqstr id, then the spawn appearance 0x15 with the flags goes out and the flags are saved).
struct AnonChange {
    int flags = 0;
    std::uint32_t eqstr = 0;
    bool changed = false; // the flags are sent and saved
};

// Utf8StrNICmp(args, word, count) == 0: the first `count` characters, case-insensitive (the word's end counts).
bool starts_with_word(std::string_view args, std::string_view word, std::size_t count);

// Cmd_anonymous @0x4f60e0: roleplaying refuses (13235); "off" (3 characters) clears, "on" (2) or no flags set 1,
// else clears; 13236 / 13237.
AnonChange anonymous_command(int flags, std::string_view args, std::string_view off, std::string_view on);
// Cmd_roleplay @0x4e83d0: "off" clears the roleplay bit; "on" or not roleplaying sets it, unless anonymous and not
// roleplaying (8873, nothing sent); else clears it; 13233 / 13234.
AnonChange roleplay_command(int flags, std::string_view args, std::string_view off, std::string_view on);

// LastTell_Add @0x4e1ef0: the name to the front of the 50 names (once, ignoring case).
void add_last_tell(std::vector<std::string>& names, const std::string& name);
struct SocialState;
// The same on the state's list, and the reply keys' index back to the newest name.
void add_last_tell(SocialState& s, const std::string& name);

// AFK. The original client has no idle timer of its own: the local AFK flag (PlayerZoneClient +AFK, vtable +0xd4)
// changes only with Cmd_afk @0x4e7690 and Handle_OP_SpawnAppearance type 0x18 for our own spawn, and while it is set
// a tell gets the away reply. EQEmu's zone puts a player AFK by itself (Character:EnableAutoAFK,
// Client::CheckAutoIdleAFK: "You are now AFK." once an OP_ClientUpdate finds the player standing still more than
// SecondsBeforeAFK{Combat,NonCombat}Zone, 1800 / 600 s, after the last reset). Only casting, a chat message, an item
// move, auto attack and OP_SpawnAppearance AFK reset that timer; walking does not, so a player who only moves and
// clicks for half an hour goes AFK the moment he stops, with any client.
// Hailstone's enhancement (docs/mechanics.md "AFK"): while the user gives input (keys, mouse, pad, typing) the zone
// is told so with OP_SpawnAppearance {me, 24, 0} at most once a minute (the zone's ResetAFKTimer), and an AFK the
// zone set by itself (not /afk) is cleared by the next input the same way.
struct AfkKeepalive {
    std::int64_t last_input_ms = -1;     // the user's last input
    std::int64_t last_sent_ms = -1;      // the last keepalive (-1: none yet, the first input sends one)
    std::int64_t zone_afk_since_ms = -1; // the zone put us AFK by itself at this time; -1 not
};
inline constexpr std::int64_t kAfkKeepaliveMs = 60000;
enum class AfkAction { None, Keepalive, Clear };
// One step at `now_ms`: `afk` the local AFK flag, `manual` set by /afk. Keepalive: send {me, 24, 0}; Clear: the
// same, and the local flag goes off ("You are no longer A.F.K.").
AfkAction afk_keepalive_step(AfkKeepalive& k, std::int64_t now_ms, bool afk, bool manual);

struct SocialState {
    std::string last_tell_from;        // /reply, /rtarget
    std::vector<std::string> last_tells; // __LastTell: who told us last, newest first (50; the Reply to... menu)
    int last_tell_index = 0;           // 0xde21d8: the reply keys' place in it (game::cycle_last_tell)
    std::vector<std::string> friends;  // the chat server's buddy list (OP_Buddy)
    std::vector<std::string> ignored;  // OP_Ignore
    bool afk = false, lfg = false;
    bool attack_on_assist = false;     // ini AttackOnAssist
    std::uint32_t follow_id = 0;       // the spawn /follow follows, 0 none
    std::int64_t session_start_ms = 0; // /played's "This session"
};

} // namespace hailstone::game
