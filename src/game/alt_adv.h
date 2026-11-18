#pragma once

// Alternate advancement as the client knows it: the ability ranks the server
// sends (OP_SendAATable, one rank per packet, AltAbility_Unserialize
// @0x4a2de0 into AltAdvManager), the ranks the player owns and the points
// (OP_RespondAA @0x4bec60, OP_AAExpUpdate @0x4b70c0, the profile), the reuse
// timers (OP_AAAction from the server, Handle_OP_AAAction @0x4b5bd0) and the
// original's rules for what may be trained and when an ability is ready.
// docs/protocol/rof2-packets.md "Alternate advancement".
// Pure state and rules; the packets are handled in net/game_connection_progress.cpp.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game/mechanics/character_sheet.h"

namespace hailstone::game {

// One effect of a rank (AA_Ability: SPA, base, limit, slot).
struct AltAbilityEffect {
    std::int32_t spa = 0, base = 0, limit = 0, slot = 0;
};

// One rank of an ability, in AltAbility_Unserialize's order (the original's
// object offsets in the comments).
struct AltAbility {
    std::uint32_t id = 0;            // +0x00: the rank id (what the profile and OP_RespondAA list)
    std::uint8_t unknown04 = 1;      // +0x04
    std::int32_t hotkey_sid = -1;    // +0x08: the hotbutton label's dbstr id (upper line)
    std::int32_t hotkey_sid2 = -1;   // +0x0c: (lower line)
    std::int32_t title_sid = -1;     // +0x10: dbstr type 1 (the name; cmd_alternateadv list)
    std::int32_t desc_sid = -1;      // +0x14: dbstr type 4 (the description)
    std::int32_t level_req = 0;      // +0x18
    std::int32_t cost = 0;           // +0x1c
    std::int32_t group = 0;          // +0x20: the ability id all its ranks share (EQEmu "seq")
    std::int32_t rank = 0;           // +0x24: this rank's number, 1 = the first (EQEmu current_level)
    std::vector<std::int32_t> prereq_groups; // +0x28: abilities required (< 0: legacy category points)
    std::vector<std::int32_t> prereq_ranks;  // +0x38: the rank each needs
    std::int32_t type = 0;           // +0x48: the AA window tab (1 general, 2 archetype, 3 class, 4 special, ...)
    std::int32_t spell = -1;         // +0x4c: the spell an activated ability casts
    std::vector<std::int32_t> timers;// +0x54: reuse timer ids (EQEmu sends one: spell_type)
    std::int32_t refresh = 0;        // +0x64: reuse time in seconds
    std::uint32_t classes = 0;       // +0x68: bit (1 << class)
    std::int32_t max_rank = 0;       // +0x6c
    std::int32_t total_cost = 0;     // +0x70: points spent on the ability up to this rank
    std::int32_t prev_id = -1;       // +0x74: the previous rank's id
    std::int32_t next_id = -1;       // +0x78: the next rank's id (-1: last, or charges)
    std::uint8_t unknown50 = 0;      // +0x50
    std::uint8_t grant_only = 0;     // +0x7c: only granted (quests), never trained
    std::uint8_t ignores_level = 0;  // +0x84
    std::uint32_t charges = 0;       // +0x80
    std::int32_t expansion = 0;      // +0x88: bit (expansion - 1) of the account's expansions
    std::int32_t category = 0;       // +0x8c
    std::array<std::uint8_t, 4> flags{}; // +0x90 (monster only), +0x91..+0x93
    std::vector<AltAbilityEffect> effects; // +0x98

    bool monster_only() const { return flags[0] != 0; } // AltAbility_IsMonsterOnly @0x4a1f80
    // AltAbility_IsClassAllowed @0x4a1f50: bit (1 << class) for classes 1..16.
    bool class_allowed(std::uint32_t cls) const;
    bool activated() const { return spell > 0; }
};

// One owned rank (the profile's 300 slots at +0x36a4 / OP_RespondAA). The client keeps
// every owned rank the server lists, with no cap: see kOwnedAbilities.
struct OwnedAbility {
    std::uint32_t id = 0;      // the rank id (0 = empty)
    std::uint32_t value = 0;   // EQEmu: the points spent (OP_RespondAA) or the rank count (profile)
    std::uint32_t charges = 0;
};

inline constexpr int kAATimers = 100;     // PcBase_SetAATimer's bound
// The slots a stock server sends in OP_RespondAA (EQEmu's MAX_PP_AA_ARRAY, the
// original's reader stops there too). Deliberate difference: the client reads
// every slot the packet holds, since some servers send more than 300; the
// packet's size (net/session_codec.h kMaxFragmentedPayload) bounds the list.
inline constexpr int kOwnedAbilities = 300;

// What CanTrainAbility needs about the player.
struct AATrainer {
    std::uint8_t class_id = 0;     // the profile class (+0x3374)
    std::uint32_t class_mask = 0;  // NMS multiclass: bit c-1 = class c (0 = class_id only)
    std::int32_t level = 0;        // the profile level (+0x3388)
    std::uint32_t expansions = 0;  // the account's expansion bits (virtual +0x5c)
    std::int32_t spend_limit = -1; // the profile virtual's limit on points spent (-1 none)
    bool monster_profile = false;  // PcZoneClient_IsPlayingMonsterProfile @0x4a4960
    // The NMS patch NOPs CanTrainAbility's level test (0x4a4589 jl -> 6 x nop): AAs from level 1.
    bool ignore_level = false;
};

// The AA window's filter (CAAWnd +0x22c): the "trainable only" check box (+0x2bc), the kind combo (+0x2c4: 0 "None",
// 1 "Can Activate", 2 "Passive"), the expansion combo (+0x2c0) and the display category combo (+0x2c8); 0 or less
// is any.
struct AAFilter {
    bool trainable_only = false; // +0
    std::int32_t kind = 0;       // +4
    std::int32_t expansion = 0;  // +8
    std::int32_t category = 0;   // +0xc
};

// CAAWnd_FilterAccepts @0x6085b0 (vector CAAWnd_FilterAccepts): whether the list shows
// `a` (true for none). In order: trainable only needs CanTrainAbility (`can_train`, asked only then); kind 1 needs
// a spell (+0x4c > 0); kind 2 needs no spell or a first persistent effect (AltAbility_FirstPersistentEffect
// @0x42e1a0: the effect list's head value, 0 for an empty list); a positive category must equal +0x8c, a positive
// expansion +0x88; a quest-only ability (+0x7c) must be held (`has`: PcZoneClient::HasAlternateAbility(its id)).
// `first_persistent_effect` is the ability's first persistent effect (non-zero when it has one).
template <class CanTrain, class Has>
bool aa_filter_accepts(const AAFilter& f, const AltAbility* a, bool first_persistent_effect, CanTrain&& can_train,
                       Has&& has) {
    if (!a) return true;
    if (f.trainable_only && !can_train()) return false;
    if (f.kind == 1) {
        if (a->spell <= 0) return false;
    } else if (f.kind == 2) {
        if (a->spell > 0 && !first_persistent_effect) return false;
    }
    if (f.category > 0 && a->category != f.category) return false;
    if (f.expansion > 0 && a->expansion != f.expansion) return false;
    if (a->grant_only != 0 && !has()) return false;
    return true;
}

struct AAReadiness {
    bool ready = true;
    std::int32_t remaining = 0; // seconds until ready (0 when ready)
    std::int32_t total = 0;     // the reuse time after reductions
};

struct AltAdvState {
    std::map<std::uint32_t, AltAbility> table; // AltAdvManager: by rank id
    std::vector<OwnedAbility> owned;           // the profile's list, in order
    std::uint32_t points = 0;                  // unspent (profile +0x8900, OP_AAExpUpdate dword 1)
    std::uint32_t spent = 0;                   // profile +0x9914 (OP_RespondAA dword 0)
    std::array<std::uint32_t, 5> assigned{};   // PcBase_SetAAPointsAssigned 0..4 (EQEmu: total, then 0)
    std::uint32_t exp = 0;                     // PcBase.AAExp on the 0..330 gauge scale
    std::uint8_t percent = 0;                  // PcBase.PercentEXPtoAA
    std::array<std::int64_t, kAATimers> timers{}; // PcBase_SetAATimer: when each timer last started (time() s)
    bool timers_seen = false;

    // OP_SendAATable (0x4a3c00 adds the rank; a known id is replaced).
    void add(AltAbility a) { table[a.id] = std::move(a); }
    const AltAbility* get(std::uint32_t id) const;
    // PcZoneClient::HasAlternateAbility @0x582670: the rank id is in the owned list.
    bool has(std::uint32_t id) const;
    // 0x4a3d70: the owned rank of an ability (by group id), or null.
    const AltAbility* owned_rank(std::int32_t group) const;
    // The owned rank of the ability `a` belongs to, 0 when none.
    std::int32_t owned_rank_number(const AltAbility& a) const;
    // AltAdvManager_MeetsPrerequisites @0x4a4230.
    bool meets_prerequisites(const AltAbility& a) const;
    // AltAdvManager::CanTrainAbility @0x4a44c0 (pc, ability, 0, 0, 0): the AA window's train button
    // and /alternateadv buy.
    bool can_train(const AltAbility& a, const AATrainer& who) const;
    // AltAdvManager::IsAbilityReady @0x4a3880 with GetCalculatedTimer @0x4a3840: `reduction`
    // is TotalEffect(SPA 264, items, the ability's group, AAs, buffs); `now` is time() in seconds.
    AAReadiness readiness(const AltAbility& a, std::int32_t reduction, std::int64_t now) const;
    // Handle_OP_AAAction @0x4b5bd0: timer `index` (0..99) started at the server's `begin`, with
    // the server's clock at `server_now`; stored on this client's clock (`now`).
    void set_timer(std::uint32_t index, std::uint32_t begin, std::uint32_t server_now, std::int64_t now);
    // OP_RespondAA: the spent count, the five assigned values and the owned list.
    void set_owned(std::uint32_t spent_points, const std::array<std::uint32_t, 5>& assigned_points,
                   std::vector<OwnedAbility> list);
    // The ranks the abilities' effects come from, for CharacterZoneClient_CacheAAEffects @0x4385d0
    // (one SheetAbility per owned rank the table knows).
    std::vector<mechanics::SheetAbility> sheet_abilities() const;
    // The same into `out`, reusing its elements' storage (the per-frame character: no allocation while the owned
    // list stays as it is).
    void sheet_abilities_into(std::vector<mechanics::SheetAbility>& out) const;
    // AltAdvManager::CanSeeAbility @0x4a40f0: the first rank of an ability not
    // owned, the rank after an owned one, or the owned last rank.
    bool can_see(const AltAbility& a, std::uint8_t class_id, bool monster_profile = false) const;
    // The visible ranks of one AA window tab (type; 0 = all), in ascending rank id order (CAAWnd's list fill
    // 0x608ac0 walks the ids 0..49998), each one the filter accepts; `who` is CanTrainAbility's player.
    std::vector<const AltAbility*> visible(std::int32_t type, std::uint8_t class_id) const;
    std::vector<const AltAbility*> visible(std::int32_t type, std::uint8_t class_id, const AAFilter& filter,
                                           const AATrainer& who) const;
    void clear_owned() { owned.clear(); spent = 0; assigned = {}; } // OP_ClearAA
};

} // namespace hailstone::game
