#pragma once

// Spell data and the spell rules the client decides itself.
//
// Spell records come from the game's spells_us.txt: one line per spell, 237
// '^'-separated columns, loaded as the original's Spellmanager::LoadTextSpells
// @0x5c1c30 / EQ_Spell_InitFromLine @0x4ae710 do (lines that do not
// start with a digit are skipped; an id above 45000 stops the load). Column
// numbers follow the original's field map, which matches EQEmu's spells_new
// columns.
//
// The rules (cast checks, cast time, memorize speed, chat lines) follow the
// original's casting functions and CEverQuest::ReportSuccessfulHit @0x52ee40
// (the spell branch, type 231). The server decides fizzles, interrupts,
// resists, mana use and landing; the client only gates, times and reports.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/eq_strings.h"
#include "game/world_state.h"

namespace hailstone { class GameDir; }

namespace hailstone::game {

inline constexpr std::uint32_t kNoSpell = 0xffffffff;
inline constexpr std::uint32_t kMaxSpellId = 45000;  // IsValidSpellID: 1..45000
inline constexpr int kSpellGems = 12;                 // gems 0..11 (CastingSlot Gem1..Gem12)
inline constexpr int kItemGem = 12;                   // CastingSlot::Item
inline constexpr int kBaseSpellGems = 8;              // CanUseMemorizedSpellSlot: gems 0..7 always
inline constexpr int kSpellBookSize = 720;
inline constexpr int kLongBuffSlots = 42;             // RoF2 long buffs 0..41
inline constexpr int kShortBuffSlots = 20;            // songs 42..61
inline constexpr int kBuffSlots = kLongBuffSlots + kShortBuffSlots + 1; // + the discipline slot, the last (62)
inline constexpr int kMemorizeTicks = 92;             // StartSpellMemorization @0x75c106
inline constexpr std::uint32_t kGemLockMs = 10000;    // CastSpell @0x43c76b: gem/global lock until the server answers
inline constexpr std::uint32_t kBuffTickMs = 6000;    // ProcessAffects every 6 s

// One spells_us.txt record (the fields the client uses).
struct SpellData {
    std::uint32_t id = 0;                      // col 0
    std::string name;                          // 1
    std::string player_1;                      // 2  (the projectile/target model text)
    std::string teleport_zone;                 // 3
    std::string cast_by_me, cast_by_other;     // 4, 5 (bard begin-cast texts)
    std::string cast_on_you, cast_on_other;    // 6, 7 (landing texts)
    std::string wear_off;                      // 8
    float range = 0, ae_range = 0, push_back = 0, push_up = 0;  // 9..12
    std::uint32_t cast_time = 0;               // 13 ms
    std::uint32_t recovery_time = 0;           // 14 ms (the global recovery after the cast)
    std::uint32_t recast_time = 0;             // 15 ms (the gem's own refresh)
    std::uint32_t duration_formula = 0;        // 16
    std::uint32_t duration_cap = 0;            // 17 tics
    std::uint32_t ae_duration = 0;             // 18
    std::int32_t mana = 0;                     // 19
    std::array<std::int32_t, 12> base{};       // 20..31
    std::array<std::int32_t, 12> base2{};      // 32..43 (limit)
    std::array<std::int32_t, 12> max{};        // 44..55
    std::int32_t book_icon = 0;                // 56
    std::int32_t gem_icon = 0;                 // 57 (memicon)
    std::array<std::int32_t, 4> reagent_id{};  // 58..61
    std::array<std::int32_t, 4> reagent_count{};      // 62..65
    std::array<std::int32_t, 4> no_expend_reagent{};  // 66..69
    std::array<std::int32_t, 12> formula{};    // 70..81
    std::uint8_t light_type = 0;               // 82
    std::uint8_t spell_type = 0;               // 83: 0 detrimental, 1 beneficial, 2 beneficial group-only
    std::uint8_t activated = 0;                // 84
    std::uint8_t resist = 0;                   // 85
    std::array<std::int32_t, 12> effect{};     // 86..97 SPA ids, 254 = unused
    std::uint8_t target_type = 0;              // 98
    std::uint8_t cast_difficulty = 0;          // 99
    std::uint8_t skill = 0;                    // 100
    std::uint8_t zone_type = 0;                // 101
    std::uint8_t environment = 0;              // 102
    std::uint8_t time_of_day = 0;              // 103
    std::array<std::uint8_t, 16> class_level{};// 104..119 (class 1..16; 255 = cannot use)
    std::uint8_t casting_anim = 0;             // 120
    std::uint8_t target_anim = 0;              // 121
    std::uint8_t travel_type = 0;              // 122
    std::int32_t spa_index = 0;                // 123
    std::uint32_t deity_mask = 0;              // 125..141 (+0x15c: bit n when column 125 + n is set; 0 any deity)
    bool cancel_on_sit = false;                // 124
    std::int32_t spell_icon = 0;               // 144 (A_SpellIcons cell)
    std::int32_t spell_anim = 0;               // 145
    bool uninterruptable = false;              // 146
    std::int32_t resist_adj = 0;               // 147
    bool not_stackable_dot = false;            // 148 (+0x239 NotStackableDot)
    bool deletable = false;                    // 149 (+0x23a Deletable: the book says it can be deleted, 4027)
    bool duration_window = false;              // 154 (short buff box)
    std::int32_t recourse_link = 0;            // 150 (+0x178 Autocast: CastSpell refuses it on yourself, 210)
    std::int32_t description_id = 0;           // 155 (dbstr type 6)
    std::int32_t category = 0;                 // 156 (dbstr type 5)
    std::int32_t subcategory = 0;              // 157
    std::int32_t subcategory2 = 0;             // 158 (+0x184; the combat ability window files an ability under both)
    bool npc_no_los = false;                  // 159 (+0x230): a spell missile's local pulse may bypass NPC line of sight
    std::int32_t endurance_cost = 0;           // 166
    std::int32_t reuse_timer_index = 0;        // 167 (linked timer 1..19)
    bool is_skill = false;                     // 168
    std::int32_t endurance_upkeep = 0;         // 174 (+0x19c)
    std::int32_t hit_count_type = 0;           // 175 (+0x1a4)
    std::int32_t hit_count = 0;                // 176 (+0x1a8)
    std::int32_t global_group = 0;             // 180 (+0x1c8; 0x2e: GetHPRegen's sitting check)
    std::int32_t pcnpc_only = 0;               // 183
    bool cast_not_standing = false;            // 184
    bool no_dispell = false;                   // 186 (+0x242 NoDispell)
    bool sneak = false;                        // 196 (+0x4dc SneakAttack: needs hiding, keeps invisibility)
    bool not_focusable = false;                // 197 (+0x4dd)
    bool stacks_with_self = false;             // 203 (+0x4df StacksWithSelf)
    bool suspendable = false;                  // 200 (+0x279 CountdownHeld: held while the buff timers are on hold)
    std::int32_t spell_group = 0;              // 207
    bool bypass_regen_check = false;           // 212 (+0x23b: does not stop fast regen)
    std::int32_t spell_rank = 0;               // 208 (+0x1dc)
    std::int32_t req_association = 0;         // 211 (+0x1e8 SpellReqAssociationID: CastSpell asks the spell manager)
    bool can_cast_in_combat = true;            // 213
    bool can_cast_out_of_combat = true;        // 214
    std::int32_t max_targets = 0;              // 218 (EQEmu aoe_max_targets: an AE's target cap, 0 none)
    std::int32_t spell_class = 0;              // 221 (+0x1e0)
    std::int32_t spell_subclass = 0;           // 222 (+0x1e4)
    float distance_mod_start_x = 0;            // 227 (+0x21c)
    float distance_mod_start_mod = 0;          // 228 (+0x220)
    float distance_mod_end_x = 0;              // 229 (+0x224)
    float distance_mod_end_mod = 0;            // 230 (+0x228)
    // +0x4e9: (end_mod - start_mod) / (end_x - start_x), the divisor 1 when it lies strictly within FLT_EPSILON of 0
    // (the parser's range ratio, game/spell_record.cpp).
    float distance_mod_ratio = 0;
    float min_range = 0;                       // 231
    bool no_remove = false;                    // 232 (+0x4e3 NoRemove)
    bool only_during_fast_regen = false;       // 234
    std::int32_t spell_subgroup = 0;           // 236 (+0x1d8 SpellSubGroup: the rank swap's lookup)

    bool beneficial() const { return spell_type != 0; }
    // EQ_Spell::SpellAffects @0x4af770: any of the 12 slots.
    bool has_effect(int spa) const;
    // GetPCSpellAffect @0x433fd0's scan of a buff's spell: the slots before the first 254 (blank) only.
    bool has_effect_before_blank(int spa) const;
    // EQ_Spell::GetSpellLevelNeeded for one class (1..16); 255 when it cannot use the spell.
    std::uint8_t level_needed(std::uint8_t class_id) const;
};

// The loaded spell file.
// Whether spells_us.txt text is the live client's layout (165 carets a line, its texts moved to
// spells_us_str.txt) rather than RoF2's (236).
bool spells_text_is_live_layout(std::string_view text);
// A live spells_us.txt line as a RoF2 one (237 fields): its columns moved to RoF2's places, the five texts
// (you cast, other casts, cast on you, cast on other, fades) from spells_us_str.txt, the effects from the last field
// ("slot|spa|base|base2|formula|max" joined by '$'). Columns live has no copy of are 0 (deletable: 1).
std::string spell_line_from_live(std::string_view line, const std::array<std::string_view, 5>& texts);
// A whole live spells_us.txt with its spells_us_str.txt, as RoF2's text.
std::string spells_text_from_live(std::string_view spells, std::string_view strings);

class SpellTable {
public:
    SpellTable();
    // Reads spells_us.txt from the game directory (the server's assets first); false when it is missing. The live
    // client's layout (spells_text_is_live_layout) is read through spells_text_from_live.
    bool load(const GameDir& game);
    // Parses the file's text. Returns the number of records loaded.
    std::size_t parse(std::string_view text);
    // One line (without the line end); nullopt when it is not a spell record.
    static std::optional<SpellData> parse_line(std::string_view line);

    const SpellData* get(std::uint32_t id) const;
    // The spell named `name` (case-insensitive; the lowest id when several share it), or null.
    const SpellData* find_by_name(std::string_view name) const;
    // SpellManager::GetSpellByGroupAndRank @0x5c1ae0 over the
    // spells of SpellGroup `group` (> 0; the load hashes them by it): among those of SpellSubGroup `subgroup`, the
    // one of exactly `rank`; else, when `rank` is -1 or `below` for lower ranks, the highest rank seen. Null when
    // none. Of equal ranks the original takes the first in its hash chain; here the highest id.
    const SpellData* by_group_and_rank(std::int32_t group, std::int32_t subgroup, std::int32_t rank, bool below) const;
    std::size_t size() const { return spells_.size(); }
    bool empty() const { return spells_.empty(); }

    // SpellManager_GetSpellCRC @0x432560: EQ_Spell_CalcCRC of the spell's
    // record as the file gave it (game/spell_record.h), for ids 0..45000
    // (unsigned) the Unknown Spell's CRC when the file has no such line, and
    // above 45000 the Unknown Spell's too. OP_CastSpell carries it.
    std::uint32_t spell_crc(std::uint32_t id) const;
    // ClientSpellManager::LoadSpells' ComputeFileChecksum @0x808d90 of the
    // two files (__MemChecker1: CRC-32 of the whole file; 0 when it cannot be
    // read): OP_ZoneEntry +68 and +72.
    std::uint32_t file_crc() const { return file_crc_; }
    std::uint32_t assoc_file_crc() const { return assoc_file_crc_; }

private:
    std::unordered_map<std::uint32_t, SpellData> spells_;
    std::unordered_map<std::uint32_t, std::uint32_t> crcs_;
    // Built by parse from spells_, so find_by_name and by_group_and_rank do not scan the table (the merchant window
    // asks per rank per scroll every frame). Ids, not pointers, so a copied table stays valid.
    struct RankedId {
        std::int32_t rank;
        std::uint32_t id;
    };
    std::unordered_map<std::string, std::uint32_t> by_name_;               // lower-cased name -> lowest id
    std::unordered_map<std::uint64_t, std::vector<RankedId>> by_group_;   // (group, subgroup) -> by rank, then id
    void build_indices();
    std::uint32_t default_crc_ = 0;
    std::uint32_t file_crc_ = 0, assoc_file_crc_ = 0;
};

// EQEmu SpellTargetType values used by the client's checks.
enum TargetType : std::uint8_t {
    kTargetOptional = 1, kTargetAEClientV1 = 2, kTargetGroupTeleport = 3, kTargetAECaster = 4, kTargetSingle = 5,
    kTargetSelf = 6, kTargetAETarget = 8, kTargetAnimal = 9, kTargetUndead = 10, kTargetSummoned = 11,
    kTargetTap = 13, kTargetPet = 14, kTargetCorpse = 15, kTargetAEClientV2 = 40, kTargetGroup = 41,
    kTargetDirectional = 42,
};

// EQ_Spell_TargetTypeNeedsTarget @0x432500: the single-target types.
bool target_type_needs_target(std::uint8_t target_type);
// The target types for which CastSpell's switch (@0x43b747) refuses to cast
// without a target (eqstr 214): the single-target types it checks and the
// targeted AE types 8, 21, 22, 23, 28.
bool target_type_requires_target(std::uint8_t target_type);

// CharacterZoneClient::CastSpell's checks are game/mechanics/cast_spell.h.

// The lowest level at which any class in `class_mask` (bit c-1 = class c) can
// use the spell (NMS multiclass: the server accepts any unlocked class,
// zone/client_process.cpp OPMemorizeSpell); 255 when none can. A mask of 0
// uses `class_id` alone.
std::uint8_t level_needed(const SpellData& spell, std::uint32_t class_mask, std::uint8_t class_id);

// The cast time itself is mechanics::cast_time_ms (game/mechanics/character.h).
// CastSpell treats casts of 100 ms or less as instant (@0x43c471).
inline bool is_instant(std::uint32_t cast_ms) { return cast_ms <= 100; }

// Memorising (CSpellBookWnd::GetSpellMemTicksLeft) is game/mechanics/spell_gems.h.

// The chat line for a spell hit (OP_Damage type 231), as ReportSuccessfulHit
// builds it: on the player CastOnYou (with "You have been healed for N
// points." / "You have taken N points of damage." for non-zero damage), on
// others "<target name><CastOnAnother>". nullopt when the spell has no such
// text or the target is unknown.
std::optional<std::string> spell_hit_message(const EqStrings& strings, const WorldState& world,
                                             const SpellData& spell, std::uint32_t target, std::int32_t damage);

// Hailstone's spell hit modes are display choices, independent of the native melee categories. Callers keep the
// normal sentence for logging and plugin hooks, then use this metadata only for the text put on screen.
enum class SpellHitReport : std::uint8_t { DirectDamage, DamageOverTime, Healing, HealingOverTime };
using SpellHitModes = std::array<std::int32_t, 4>;
struct SpellHitLine {
    SpellHitReport kind = SpellHitReport::DirectDamage;
    std::string abbreviated;
    std::string amount;
};
// Recognised EQEmu formatted-message ids and their typed argument positions, never arbitrary chat text. Unknown
// messages or missing/malformed arguments return nullopt so their normal text remains intact.
std::optional<SpellHitLine> spell_message_hit_report(std::uint32_t eqstr, std::span<const std::string> args);
// Some servers carry complete English combat reports in GENERIC_STRING or OP_SpecialMesg instead of typed
// substitutions; stock EQEmu does so only for its caster-less "You have been healed for %d points of damage.".
// Recognise only complete hit/heal forms on combat message types; unrelated text stays intact.
std::optional<SpellHitLine> spell_text_hit_report(std::string_view text, std::uint32_t color);
// OP_Damage's native spell line contains an amount only on the local player. Other landing and zero-damage
// messages keep their normal presentation; the packet does not identify a periodic tick.
std::optional<SpellHitLine> spell_damage_hit_report(const WorldState& world, const SpellData& spell,
                                                  std::uint32_t target, std::int32_t damage);
// Mode 0 (and invalid modes) preserves normal; 1 is abbreviated, 2 is the amount alone.
std::string report_spell_hit_line(const SpellHitLine& line, std::string_view normal, std::int32_t mode);

// Buff time left: `tics` as of `received_ms` (the packet, or the client's
// last buff tick, game/passage_of_time.h).
// A negative count (and the permanent formula 50) never runs out: EQEmu sends -1 (duration formula 50), -4
// (formula 51, an aura's) and -1000 (PERMANENT_BUFF_DURATION, a script's); the original's own -2 is not traced
// (docs/unknowns.md).
bool buff_is_permanent(std::int32_t tics, const SpellData* spell);
// 0 for a negative count: test buff_is_permanent first.
std::uint64_t buff_ms_left(std::int32_t tics, std::int64_t received_ms, std::int64_t now_ms);
// Ticks left, as the original's signed count: `tics` less the whole 6 s ticks since `received_ms`, never below 0.
// A negative count (a permanent buff) is given as it is, never counted down.
std::int32_t buff_tics_left(std::int32_t tics, std::int64_t received_ms, std::int64_t now_ms);
// "333d", "1:05:00", "4:59", "12s".
std::string format_duration(std::uint64_t ms);

// ---- client-side state (owned by net::GameConnection) ---------------------

// A gem: the profile's memorised spell (+0x3060) and PlayerClient +0x2e8's ready time, absolute __EQGetTime ms
// (GameConnection::eq_time()), compared unsigned (game/mechanics/spell_gems.h).
struct SpellGem {
    std::uint32_t spell = kNoSpell;
    std::uint32_t ready = 0;
};

struct BuffSlot {
    std::uint32_t spell = kNoSpell;
    std::int32_t tics = 0;           // as received; negative for a permanent buff (buff_is_permanent)
    std::int64_t received_ms = 0;
    std::uint8_t level = 0;
    std::uint32_t caster_id = 0;
    std::string caster_name;
    std::uint32_t num_hits = 0;
    float modifier = 1.0f;           // bard instrument modifier (EQ_Affect +4)
    std::uint8_t effect_type = 2;    // EQ_Affect +0: 2 buff, 4 inverted
    // EQ_Affect +0x28: the per-slot data (including counters left). An all-zero array without known
    // counters does not mean the buff has run out; zone-in lists and the profile do not seed this array.
    std::array<std::int32_t, 12> slot_data{};
    bool counters_known = false;
    bool empty() const { return spell == kNoSpell || spell == 0; }
};

// The cast in progress (LaunchSpellData, PlayerZoneClient+0x444).
struct CastingData {
    std::int64_t eta_ms = 0;             // 0 when the bar is not running
    std::uint32_t spell = kNoSpell;      // -1 when not casting
    std::uint8_t slot = 0xff;            // gem
    std::uint32_t cast_time = 0;         // bar length
    std::uint32_t target = 0;
    // The cast item (+0x450 ItemGlobalIndex as CastSpell stores it: location, slot | sub << 16, aug; all -1 for a
    // gem) and its id (+0x460).
    std::array<std::uint32_t, 3> item_location{0xffffffffu, 0xffffffffu, 0x0000ffffu};
    std::uint32_t item_id = 0;
    std::uint32_t item_cast_type = 0; // +0x45c
    bool casting() const { return spell != kNoSpell; }
    // LaunchSpellData_IsItemCast @0x441e30: the location is 0..0x17 and the slot word is not -1.
    bool is_item_cast() const {
        return static_cast<std::int32_t>(item_location[0]) >= 0 && static_cast<std::int32_t>(item_location[0]) < 0x18 &&
               (item_location[1] & 0xffffu) != 0xffffu;
    }
};

// A memorization in progress (CSpellBookWnd +0x234..+0x23c); it ends when OP_MemorizeSpell goes out.
struct Memorizing {
    int gem = -1;
    int book_slot = -1;
    std::uint32_t spell = kNoSpell;
    int ticks_left = kMemorizeTicks;
};

// A scribe in progress (CSpellBookWnd ScribeBookSlot/SpellId/ScribeTicksLeft):
// 92 ticks, 3 per frame at most every 2 ms (GetSpellScribeTicksLeft @0x75e620, scribe_countdown).
struct Scribing {
    int book_slot = -1;
    std::uint32_t spell = kNoSpell;
    int ticks_left = kMemorizeTicks;
    bool sent = false;
};

// Somebody else's cast (OP_BeginCast), for the target window.
struct OtherCast {
    std::uint32_t spell = kNoSpell;
    std::int64_t started_ms = 0;
    std::uint32_t cast_time = 0;
};

struct SpellState {
    std::array<std::uint32_t, kSpellBookSize> book{};
    std::array<SpellGem, kSpellGems + 1> gems{}; // 12 = the item/AA slot
    std::uint32_t recovery_ready = 0;            // PlayerClient +0x3cc: the global recovery lock (__EQGetTime ms)
    CastingData casting;
    int pending = 0;                             // IsItemPending (requests the server has not answered)
    std::optional<Memorizing> memorizing;
    std::uint32_t memorize_last_tick = 0;        // 0xf749a8: the last memorise tick (0 after a memorise ends)
    int memorize_sent_gem = -1;                  // the gem of the OP_MemorizeSpell the server has not answered
    std::optional<Scribing> scribing;
    std::uint32_t scribe_last_tick = 0;          // 0xf749ac: the last scribe tick (0 after a scribe is sent)
    std::array<BuffSlot, kBuffSlots> buffs{};
    std::int64_t last_buff_tick_ms = 0;          // the character's 6 s tick timer (spawn +0x24c)
    std::int64_t last_upkeep_tick_ms = 0;        // its 1 s timer (spawn +0x2d8; 0: set at the next frame)
    std::uint32_t target_buffs_entity = 0;       // whose buffs target_buffs are (OP_TargetBuffs)
    std::vector<std::pair<std::uint32_t, BuffSlot>> target_buffs; // slot, buff
    std::unordered_map<std::uint32_t, OtherCast> others;           // spawn id → cast
    std::uint32_t hunger = 0, thirst = 0;        // the profile's, then OP_Stamina
    std::array<std::uint32_t, 20> linked_ready{}; // profile +0x6a0c: shared timers 1..19, Unix seconds (gem tint only)
    // profile +0x69bc: the combat ability (discipline) reuse timers 0..19 (PcBase_SetCombatAbilityTimer, read by
    // GetDisciplineReuseTimer), Unix seconds when each is ready; ids from 20 up are a server plugin's (NMS).
    std::array<std::uint32_t, 20> discipline_ready{};
    // The player spawn's +0x22d (CharacterZoneClient_AreBuffTimersOnHold): set by OP_SpawnAppearance 38 (value != 0),
    // by a SPA 0x1b4 buff landing or ticking; cleared with the spawn (PlayerZoneClient::ResetVariables), here at zone-in.
    bool buff_timers_on_hold = false;
    // OP_NewZone +876 (suspend_buffs), which the original copies with the rest of the packet to 0xddfc5c: the zone
    // holds every suspendable buff's countdown (EQEmu's zone.suspendbuffs; the server stops ticking them too).
    // Cleared on the way out of a zone and set again by the next zone's OP_NewZone.
    bool zone_suspends_buffs = false;

    SpellState() { book.fill(kNoSpell); }
    // CCastSpellWnd::HandleSpellLeftClick's check: now (__EQGetTime) past the gem's timer and the recovery lock.
    bool gem_castable(int gem, std::uint32_t now) const;
    // RefreshSpellGemButtons' readiness (the tint), without the casting and linked-timer parts: now at or past both.
    bool gem_timers_done(int gem, std::uint32_t now) const;
};

// Spell_IsBuffCountdownHeld @0x4dcf20: a suspendable spell (+0x279) in a zone that suspends buffs (0xddfc5c, OP_NewZone
// +876: SpellState::zone_suspends_buffs), or while the buff timers are on hold unless it has SPA 0x1b4 itself: the
// function tests 0xddfc5c before the SPA, so the exception covers only the hold. False without a spell.
bool buff_countdown_held(const SpellData* spell, bool timers_on_hold, bool zone_suspends_buffs);

// The time left on a buff as a display shows it: its ticks as received while its countdown is held
// (buff_countdown_held), else counted down since they arrived. 0 for a negative count: test buff_is_permanent first.
std::uint64_t buff_ms_left(const BuffSlot& b, const SpellData* spell, bool timers_on_hold, bool zone_suspends_buffs,
                           std::int64_t now_ms);

// CSpellBookWnd::GetSpellScribeTicksLeft @0x75e620's countdown, run when the book's scribe gauge is drawn: 3 ticks
// off when the display clock (`now`, CDisplay::TimeStamp ms) moved on more than 1 ms (unsigned) since `last`
// (0xf749ac), which then becomes now. Below 1 tick the scribe is due: the ticks and `last` go to 0 and it returns
// true (the caller sends OP_MemorizeSpell {book slot, spell, 0}).
bool scribe_countdown(Scribing& scribing, std::uint32_t& last, std::uint32_t now);

} // namespace hailstone::game
