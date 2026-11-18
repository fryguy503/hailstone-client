#pragma once

// Combat rules the client decides itself: consider colours and messages,
// melee and death messages, targeting helpers and the animation clip for an
// OP_Animation action. The packets are in net/zone_packets.h; the state lives
// in net::GameConnection.
//
// Sources: the original's BuildConsiderMessage @0x522e20,
// PcClient::GetConLevel @0x577cb0, eqgame.exe 0x7be930/@0x7be770 (the
// consider table, read from the disassembly),
// CEverQuest::ReportSuccessfulHit @0x52ee40, SetDefaultDamageDescByRace
// @0x5143c0 and Handle_OP_Death. Message templates come
// from eqstr_us.txt.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "game/chat_text.h"
#include "game/eq_strings.h"
#include "game/world_state.h"

namespace hailstone::game {

// PcClient::GetConLevel's values.
enum class Con : std::uint8_t { None = 0, Gray, Green, LightBlue, DarkBlue, White, Yellow, Red };

// The standard consider colour of a spawn of level `theirs` for a player of
// level `mine` (0x7be930: a 106 × 106 table built by @0x7be770).
Con standard_con(int theirs, int mine);
// PcClient::GetConLevel @0x577cb0 over its inputs (vectors
// PcClient_GetConLevel.jsonl): 0 for a spawn of type 2+ (corpses), with its
// virtual +0x50 set, or with its virtual +0x98 set unless +0x1e60 is 1 and
// +0x122a is set; a non-player gets the standard colour; a player under PvP
// rule set (OP_LogServer +4, 0xdcee70) 0 the standard colour as is; rules 1,
// 2 and 4 give 1 / 5 / 7 at level differences below -4 / -8 / -5 and from
// 5 / 9 / 6; then (every rule but 0) levels up to 5 clamp: theirs < 6 with
// mine > 5 gives 1, theirs > 5 with mine < 6 gives 7, both < 6 gives 1.
struct ConLevelInput {
    std::int32_t pvp_rules = 0;     // 0xdcee70
    std::int32_t my_level = 0;      // the character's level (CharacterBase virtual +0x68)
    std::int32_t their_level = 0;   // PlayerZoneClient::GetLevel's byte
    std::uint32_t standard = 0;     // 0x7be930(theirs, mine): standard_con
    std::uint8_t spawn_type = 0;    // +0x125: 0 player, 1 NPC, 2+ corpses
    bool virtual_50 = false;        // spawn virtual +0x50 (set: con 0)
    bool virtual_98 = false;        // spawn virtual +0x98 (set: con 0 unless the next two hold)
    std::int32_t field_1e60 = 0;    // +0x1e60
    bool field_122a = false;        // +0x122a
};
std::uint32_t con_level_value(const ConLevelInput& in);
// PcClient::GetConLevel for a spawn and the server's PvP rule set.
Con con_level(const Spawn& target, int my_level, std::int32_t pvp_rules);
// The chat colour BuildConsiderMessage prints in (2 green, 18 light blue, 4
// blue, 10 white, 15 yellow, 13 red, 6 gray).
std::uint32_t con_chat_color(Con con);
// The server's OP_Consider level field (ConsiderColor) as a Con.
Con con_from_server(std::uint32_t level);
// Screen colour for UI (0xRRGGBB).
std::uint32_t con_rgb(Con con);
// The colour an NPC's name is drawn in when display.con_name_colour is on (a deliberate difference, docs/graphics.md
// "Name plates"): its consider colour. nullopt for everything else, and for every spawn while the option is off, so
// the caller keeps the stock colours.
std::optional<std::uint32_t> npc_name_colour(const Spawn& s, int my_level, std::int32_t pvp_rules, bool enabled);
// The GM name colour, 0x1da64b, as PlayerClient::SetNameSpriteTint @0x58bf00 gives it: a spawn with IsGM that is
// not an NPC (type 1) and is neither anonymous nor roleplaying (Anon 0). nullopt for every other spawn, and (a
// deliberate difference from the original) for every corpse, which keeps the ordinary corpse colour.
std::optional<std::uint32_t> gm_name_colour(const Spawn& s);
// A mercenary's name colour, 0xe1b511 (amber), as SetNameSpriteTint gives a live NPC with IsMercenary (+0x208).
// nullopt for every other spawn.
std::optional<std::uint32_t> mercenary_name_colour(const Spawn& s);

// BuildConsiderMessage: "<name> <standing> -- <difficulty>". faction is the
// server's (remapped) standing 1..9; short_form is Consider_Struct +16 (the
// original's info +0x10; 0 normally).
std::string consider_message(const EqStrings& strings, const Spawn& target, int my_level, std::uint32_t faction,
                             std::uint32_t short_form, std::int32_t pvp_rules);

// The verb of a melee hit (ReportSuccessfulHit's switch on the damage type,
// SetDefaultDamageDescByRace for hand to hand and unknown types). First
// person ("hit") when the attacker is the player or the attack missed.
std::string damage_verb(const EqStrings& strings, std::uint8_t type, std::uint32_t attacker_race, bool third_person);

// The chat line for an OP_Damage (melee; spell damage returns nullopt), as
// ReportSuccessfulHit builds it: "You hit a rat for 3 points of damage.",
// "A rat bites YOU for 1 point of damage.", "You try to hit a rat, but miss!",
// "A rat tries to bite YOU, but YOU block!". nullopt when a spawn is unknown.
std::optional<std::string> damage_message(const EqStrings& strings, const WorldState& world, std::uint32_t target,
                                          std::uint32_t source, std::uint8_t type, std::int32_t damage,
                                          std::uint32_t special);

// ReportSuccessfulHit's melee report: the full sentence (its first character
// upper-cased with the C locale's _toupper, whatever the display mode) and
// the parts the short forms use, by the six chat categories of
// CChatWindowManager (+0x224 your hits .. +0x238 others' misses).
enum class HitReport : std::uint8_t { YourHits, YourMisses, YouBeingHit, YouBeingMissed, OthersHits, OthersMisses };
struct HitLine {
    HitReport kind = HitReport::OthersHits;
    std::string text;     // the full sentence (mode 0)
    std::string verb;     // the original's a2 (the verb)
    std::string victim;   // a3 (the victim's name)
    std::string attacker; // a4 (the attacker's name)
    std::int32_t damage = 0;
    bool i_attack = false;       // the attacker is the player
    bool on_me = false;          // the target is the player
    bool npc_hit_itself = false; // an NPC (type 1) is its own attacker: a hit is not reported
};
std::optional<HitLine> damage_report(const EqStrings& strings, const WorldState& world, std::uint32_t target,
                                     std::uint32_t source, std::uint8_t type, std::int32_t damage,
                                     std::uint32_t special);
// ReportSuccessfulHit's chat filters (ChatFilters_GetMode, mode 1 hides) on a melee line, starting from its
// `report` argument: a hit by somebody else on somebody else under filter 15 (others' hits, @0x53092c) is shown
// exactly when the player's pet hit; an NPC hitting itself never shows; a miss on the player is
// hidden under filter 16 (@0x530768), the player's own under filter 13 (@0x5305ce), and somebody else's on
// somebody else under filter 14 (@0x53078b) is shown exactly when the player's pet missed.
// Deliberate change from RoF2 (a fix): an unreported line stays hidden, the pet exceptions above included; the
// original lets the pet's line through unreported.
// `by_my_pet`: the attacker is the local player's PetID, or a temporary pet (bTempPet +0x3dc) whose MasterID is
// the player's SpawnID (ReportSuccessfulHit @0x52efd3). Its lines on others are coloured 0x148.
bool hit_line_shown(const HitLine& line, bool by_my_pet, const ChatFilters& filters, bool report = true);
// The display modes per category (CChatWindowManager +0x224..+0x238): 0 the
// full sentence, 1 the short line, 2 the number alone, else nothing.
using HitModes = std::array<std::int32_t, 6>; // indexed by HitReport
// ReportHit_* @0x4f7d70..0x4f8270: the line a category's mode shows, or
// nullopt. Mode 1: hits eqstr 2130 "%1 %2 for %3" {verb, victim, damage}
// (yours), 2108 "%1 %2 for %3" {attacker, verb, damage} (on you), 2128 "%1 %2
// %3 for %4" {attacker, verb, victim, damage} (others); misses 15039 "missed
// %1" {victim}, 15040 "%1 missed" {attacker}, 15041 "%1 missed %2" {attacker,
// victim}. Mode 2: hits "%d" of the damage, misses eqstr 12156 "miss".
std::optional<std::string> report_hit_line(const EqStrings& strings, const HitLine& line, std::int32_t mode);

// Handle_OP_Death's line: "You have slain a rat!", "You have been slain by a
// rat!", "A rat has been slain by Bob!", "You died." ...; nullopt when the
// original prints nothing (an NPC killing itself).
std::optional<std::string> death_message(const EqStrings& strings, const WorldState& world, std::uint32_t victim,
                                         std::uint32_t killer, std::uint32_t attack_skill);

// CEverQuest::ReportSuccessfulHit @0x52ee40's animation on the target
// (@0x5312f0: its animation object's slot 0xc(anim, 1, 0, 1.0, 1, 0xff,
// 0)), for a target of type 0 or 1 (not a corpse): a spell hit (type 231)
// plays the spell's TargetAnim (1..0x4e) when the target stands (100 or
// 102); a hit with damage above 0 (a spell's only when detrimental) that set
// none plays D02 (0xd) unless the local player (not the target: the
// original reads pinstLocalPlayer) lies dead (115) or feigns (120). Misses
// play nothing. The environmental types 0xf3..0xfd (the switch's cases -0xd
// .. -3) set their own animation whatever the stand states: 0xf3..0xf9 D01
// (0xc), 0xfa and 0xfb D04 (0xf), 0xfc and 0xfd D03 (0xe); none when the
// damage is exactly 0. The other types above 231 play D02 like a melee hit.
struct HitAnimInput {
    std::uint8_t type = 0;          // the damage type (231 a spell)
    std::int32_t damage = 0;
    bool target_alive = true;       // the target's +0x125 below 2
    std::uint8_t target_stand_state = 100;
    std::uint8_t local_stand_state = 100;
    std::uint8_t spell_target_anim = 0; // the spell's TargetAnim (type 231)
    bool spell_detrimental = false;
};
std::optional<std::uint8_t> hit_animation(const HitAnimInput& in);

// ReportSuccessfulHit's shared tail (@0x5311ee) runs its landed-hit half when
// the target is not a corpse and the landed damage is above 0: the damage of a melee
// hit (types below 231) or of any type above 231, a spell's (231) only when
// it is detrimental. Then: on the local player the spell book closes and a
// kneeling (105), sitting (110) or mounted player stands (100); the target's
// Hit1..4 sound plays (one picked 1..4).
bool hit_landed(const HitAnimInput& in);

// The environmental hit types: 0xf3 mind burns, 0xf4 rotting, 0xf5 chilled,
// 0xf6 freezing, 0xf7 tormented, 0xf8 burned, 0xf9 thorns, 0xfa burning
// (lava), 0xfb drowning, 0xfc falling, 0xfd a trap.
inline bool is_environment_hit(std::uint8_t type) { return type >= 0xf3 && type <= 0xfd; }

// ReportSuccessfulHit's line and sound for an environmental hit (the cases
// -0xd .. -3, then the branch for types above 231): on the local player "YOU
// are burning!" ... (eqstr 9080, 5722, 12139, 12141, 12137, 12135, 12133,
// 12131; drowning 12128, or 12127 "suffocating" in zone 0xab; falling 12118
// with "YOU"; a trap 12121..12126 by the hit's spell field: 0x73 / 0x74 saw
// blade, 0x78 / 0x7d spear, 0x82 pendulum, 0x87 blade, 0x8c / 0x91 / 0x92 /
// 0x96 / 0x97 / 0x98 crushed, others "injured by a trap"), in colour 0x10a;
// on another the next id with its name ("%1 is burning."), none for
// 0xf3..0xfa when chat filter 0x11's mode is 1, in colour 0x149 (0xf3..0xfa)
// or 0x117. No line when the damage is exactly 0; the caller shows it only
// when the hit is reported (Handle_OP_Damage's 75-unit rule), its first
// letter upper-cased. fall_hit (0x73) plays on the victim for a fall and for
// a crushing trap on the local player, whatever the damage.
struct EnvHitInput {
    std::uint8_t type = 0;
    std::int32_t damage = 0;
    bool on_me = false;          // the victim is the local player
    std::string victim;          // the victim's name (another)
    std::uint32_t spell = 0;     // the hit's spell field (+5)
    std::int32_t zone_id = 0;
    std::int32_t filter_mode = 0; // ChatFilters_GetMode(0x11)
};
struct EnvHit {
    std::string text; // empty: none
    std::uint32_t color = 0;
    bool fall_hit_sound = false;
};
EnvHit environment_hit(const EqStrings& strings, const EnvHitInput& in);

// The clip code for an OP_Animation action (1 = C01 ...), or nullptr: the
// original's old-style names (0xb787a0, mechanics::old_anim_clip).
const char* animation_code(std::uint8_t action);

// The nameplate text of a spawn ("a rat", "a rat's corpse").
std::string spawn_label(const Spawn& s);
// PlayerClient::SetNameSpriteState @0x58e2d0's owner line under an NPC's name (not a corpse's): "(" + its last name
// + ")"; a pet's (MasterID set, its last name trimmed: "Soandso`s pet") only with PetOwnerNames (CDisplay +0x2d6b,
// which CDisplay_TogglePetNameSprites @0x490b40 sets). Empty without one.
std::string npc_owner_line(const Spawn& s, bool pet_owner_names);
// SetNameSpriteState's guild line under a player's name ("\n<" + CGuild::GetGuildName + ">"): /shownames 3 or 4,
// a guild id other than -1 (and 0, EQEmu's none in a spawn record), bGuildShowAnim (the spawn's guild_show) and the
// player not anonymous (Anon 0, or the roleplaying bit 2 set).
bool name_sprite_shows_guild(const Spawn& s, int show_names);
// SetNameSpriteState's words before a player's name: "Offline " (eqstr 766) with bOfflineMode, then "Trader " (5503)
// with the trader flag or else "Buyer " (6055) with the buyer flag, either of which takes the title's place. Nothing
// for an NPC or a corpse, as the original (an NPC the server marks as a trader keeps its plain name).
struct NameSpriteMarketWords {
    std::string prefix;          // empty, or the words with their trailing spaces
    bool replaces_title = false; // a trader's or buyer's title is not shown
};
NameSpriteMarketWords name_sprite_market_words(const Spawn& s, const EqStrings& strings);
// Whether a spawn has a name sprite by the name switches (unk_PlayerClient_UpdateNameSprite @0x58f050 skips PCs and
// NPCs whose name sprites are switched off, and the local player while its model is hidden in first person). The
// local player's has a switch of its own, Hailstone's (display.own_name; the original has none): pc_names and
// own_name both on.
struct NameSpriteSwitches {
    bool pc_names = true;  // CDisplay::ToggleCharacterNameSprites (/shownames off, display.pc_names)
    bool npc_names = true; // CDisplay::ToggleNpcNameSprites (display.npc_names); corpses too
    bool own_name = true;  // Hailstone: the player's own name over its head
};
bool name_sprite_switched_on(const Spawn& s, bool local_player, bool model_hidden, const NameSpriteSwitches& on);
// The leadership marks around a non-player's name (SetNameSpriteState's NPC branch, with Gates of Discord):
// "[>> ][mark - ]name[ - hp%][ <<]": ">> " / " <<" around a main assist's target (the group's or the raid's, OP_
// SetGroupTarget), the group mark (OP_MarkNPC) or the raid mark (OP_MarkRaidNPC, which wins) as "%d - ", and an NPC's
// HP percent while NPC Health (ability 1 or 0x11) is active and it is the target, an assist target or marked.
struct NameSpriteMarks {
    bool leadership = false; // the character's expansion flag 0x40
    bool npc_health = false; // Leadership_IsAbilityActive(1, 1) or (0x11, 1)
    std::uint32_t target_id = 0;
    std::array<std::uint32_t, 4> assist_targets{}; // GroupAssistNPC, RaidAssistNPC[3]
    std::array<std::uint32_t, 3> group_marks{}, raid_marks{};
};
std::string name_sprite_with_marks(const Spawn& s, const std::string& name, const NameSpriteMarks& m);

// Targeting. Heading (0..512) that faces `to` from `from`.
float heading_to(const glm::vec3& from, const glm::vec3& to);
// The nearest living, targetable NPC within max_distance whose direction is
// within 90° of `yaw` (radians), skipping `exclude` (0 = none).
std::uint32_t nearest_npc(const WorldState& world, const glm::vec3& from, float yaw, float max_distance,
                          std::uint32_t exclude);
// Cycles NPC targets nearest first in front of the player: the next one
// after `current`, wrapping around (Tab).
std::uint32_t next_npc(const WorldState& world, const glm::vec3& from, float yaw, float max_distance,
                       std::uint32_t current);
// The spawn whose body (a vertical cylinder from its feet up to its height)
// the ray hits first; skip = the player's own id.
std::optional<std::uint32_t> pick_spawn(const WorldState& world, const glm::vec3& origin, const glm::vec3& dir,
                                        float max_distance, std::uint32_t skip);

} // namespace hailstone::game
