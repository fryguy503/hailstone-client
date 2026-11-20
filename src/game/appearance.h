#pragma once

// Spawn appearance rules that do not depend on the model files: which
// dressing path a spawn uses and how armour updates change it. Sources:
// the original's OP_WearChange handling, EQPlayer::UnSerializeSpawn and
// PlayerClient::ApplyBodyTexture, RefreshEquipSlot and SetHead.

#include <cstdint>
#include <string>

#include "game/world_state.h"
#include "net/player_packets.h"

namespace hailstone::game {

// Visible equipment slots (WearChange / spawn order).
enum EquipSlot : std::uint8_t {
    kSlotHead = 0,
    kSlotChest = 1,
    kSlotArms = 2,
    kSlotWrists = 3,
    kSlotHands = 4,
    kSlotLegs = 5,
    kSlotFeet = 6,
    kSlotPrimary = 7,
    kSlotSecondary = 8,
};
inline constexpr std::uint8_t kNoTexture = 0xff; // Appearance::texture: per-slot armour

// PlayerZoneClient::LegalPlayerRace @0049deb0: races that wear armour per
// slot (< 13, 128, 130, 330, 522).
bool is_pc_model_race(std::uint32_t race);
// Remote signed HP is unavailable; the server percentage gates living targets.
bool spawn_invisible(const Spawn& viewer, const Spawn& target, std::int32_t see_invisible_rank, bool group_member);

// The marks round an invisible spawn's name (GameConnection::name_invisibility): normal is the original's "(Name)"
// (PlayerClient::SetNameSpriteState @0x58e2d0), undead Hailstone's "[Name]"; both make "([Name])".
struct NameInvisibility {
    bool normal = false;
    bool undead = false;
};

void apply_name_invisibility(std::string& name, NameInvisibility invisibility);

// PlayerClient_ApplyNewArmorFilter @0x595de0 (re/src/game/spawn/armor_filter.cpp): the ActorNewArmorFilter bits
// that show a spawn's hero's forge armour, by its Type (PlayerClient +0x125: 0 PC, 1 NPC, 2+ corpse). An NPC
// has 0x10; a player 8, with 1 when it is the local player, 2 when it is the local player or a group member
// (IsSelfOrGroupMember) and 4 when it is a raid member; a corpse none (its flag stays 1, hero's forge hidden).
inline constexpr std::uint8_t kNewArmorAllBits = 0x1f;
std::uint8_t new_armor_bits(std::uint8_t spawn_type, bool local_player, bool group_member, bool raid_member);
// ActorClient +0x1ac: hero's forge armour is not shown under `filter`.
bool appearance_new_armor_filtered(const Appearance& a, std::uint32_t filter);
// Collapses Appearance::new_armor_bits to what `filter` makes of them (kNewArmorAllBits shown, 0 hidden), so two
// appearances the filter draws alike compare equal and share a dressing.
void resolve_new_armor_filter(Appearance& a, std::uint32_t filter);

// The unserializer's fold (@00591acb): for a PC-model race a TextureType
// >= 0 becomes the material of slots 1-6 and TextureType becomes -1.
void normalize_appearance(Appearance& a, std::uint32_t race);

// The appearance a spawn of `race` is drawn with. A race outside
// is_pc_model_race shows no armour colours (slots 0-6): seen in the RoF2
// client with a player under a skeleton illusion in dyed armour, whose bones
// stay undyed although the server re-sends every slot's tint after
// OP_Illusion. Where the original drops them is not found
// (docs/unknowns.md); SwapBodyMaterial itself has no race rule.
Appearance drawn_appearance(const Appearance& a, std::uint32_t race);

// OP_WearChange. Returns false when the client ignores it: a body
// slot of a spawn with a fixed body texture, or a slot >= 9.
bool apply_wear_change(Appearance& a, std::uint8_t slot, std::uint32_t material, std::uint32_t hero_forge,
                       std::uint32_t tint, std::uint32_t variation = 0, std::uint32_t elite_material = 0,
                       std::uint32_t new_armor_type = 0);

// Material numbers. 10..16 on the chest are robes (the arms and wrists then
// show the chest's texture); robe textures are CLK04..CLK10.
inline bool is_robe(std::uint32_t chest_material) { return chest_material >= 10 && chest_material <= 16; }

// The material a body slot (0-6) is drawn with: the slot's own material, the
// chest's for arms and wrists under a robe, else the body texture for NPCs.
// The head is bare unless show_helm (SwapHead @00594210).
std::uint32_t effective_material(const Appearance& a, int slot);

// PlayerClient::HandleMaterialEx @0x594e50 (RefreshEquipSlot) on a Luclin
// model: under a robe (chest +0xf44 10..16) the arms and wrists, and a
// Froglok's (330) legs, are dressed with the chest's material, its tint
// (+0xf00) and its hero's forge flag. The slot whose armour body slot `slot`
// (1-6) is dressed with.
int luclin_armor_slot(const Appearance& a, std::uint32_t race, int slot);

// 0x40a5b0 (ActorBase, @0x40a5b0): the head material ids 0xf0 / 0xf1 of
// the old models by the actor's race and gender (female: gender 1): 0xf0 is a
// per-race helm item, 0xf1 the head 3 (Ogres a helm, Barbarians the 0xf0 one);
// any other id, or a race outside 1-12 and 128, stays as it is.
std::uint32_t map_head_material(std::uint32_t race, std::uint8_t gender, std::uint32_t id);

// What PlayerClient::SwapHead @0x594210 puts on an old (classic) model for
// head material `material`: the head mesh
// <code>HE<variant>, plus an item IT<item> held on the head point when
// item != 0. 0xf0..0xff map through map_head_material; a result still
// >= 0xf0 is that item with HE00. 7 is 2 (3 for Iksar). Vah Shir (130) wear
// helmets as items only (1..3 and 0xf0 -> IT0x295.. / IT0x29a..).
struct HeadModel {
    std::uint32_t variant = 0;
    std::uint32_t item = 0;
};
HeadModel head_model(std::uint32_t race, std::uint8_t gender, std::uint32_t material, std::uint32_t hero_forge = 0);

// CDisplay::IsShield @0x489d70: whether a secondary's item model IT<n> is a shield, which PlayerClient::SetHeldModel
// shows on bone slot 5 (<code>SHIELD_POINT_DAG / ARML_SHLD) instead of slot 4 (L_POINT / ARML_WEAP). A fixed list of
// model numbers; a model named like a shield but not on it (the Haunting Nightmares ornament IT101064) is held in the
// hand. The original asks Resources\ActorTagInfo.txt's table first when that file loaded; RoF2 ships none.
// Hailstone (a deliberate difference, display.extra_shields, on by default): with `extra_shields` set, also the
// models of later shields the list lacks (shield_models.inc, tools/gen_shield_models.py), which RoF2 holds in the
// hand like a weapon, and of shield ornaments no other item uses (IT101064 among them).
bool is_shield_model(std::uint32_t item_model);
bool is_stock_shield_model(std::uint32_t item_model);
// display.extra_shields, set once at start (dressed models are cached).
void set_extra_shields(bool on);

// PlayerClient_UpdateHoldingAnimation @0x58d780: whether a spawn's holding column (+0x334) is other than 0, which
// on a tunic-point model plays its variant B tracks (assets::CharacterModel::held_aliases). The column is 1..7 when
// either hand's model kind (+0x119c / +0x11a0, 0x489ec0) is set, and a kind is 0 only for no model.
inline bool holds_item(const Appearance& a) {
    return a.material[kSlotPrimary] != 0 || a.material[kSlotSecondary] != 0;
}

inline bool holds_instrument(const Spawn& s) { return (s.player_state & net::kPlayerStateInstrument) != 0; }

} // namespace hailstone::game
