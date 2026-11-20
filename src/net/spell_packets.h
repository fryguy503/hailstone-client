#pragma once

// Typed RoF2 spell packets: casting, memorizing and scribing, buffs, spell
// hits and spell messages. Layouts follow the NMS server's RoF2 translator
// (common/patches/rof2.cpp ENCODE/DECODE, rof2_structs.h) and, for
// pass-through opcodes, common/eq_packet_structs.h; the client side is
// the original's casting code. Per-packet notes are in
// docs/protocol/rof2-packets.md ("Spells").

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/mechanics/cast_spell.h"
#include "net/packet.h"

namespace hailstone::net {

// RoF2 InventorySlot_Struct / the client's ItemGlobalIndex (12 bytes): i32
// location (EQEmu's i16 Type + i16 Unknown02), i16 slot, sub-index, augment,
// i16 padding. A gem cast carries an invalid index (all -1).
struct InventorySlot {
    std::int16_t type = -1;
    std::int16_t unknown02 = -1;
    std::int16_t slot = -1;
    std::int16_t sub_index = -1;
    std::int16_t aug_index = -1;
    std::int16_t unknown01 = 0;
};

// OP_CastSpell, client → zone (44 bytes, rof2_structs.h CastSpell_Struct):
// gem (0..11, 12 item, 13 discipline, 255 AA), spell id, inventory slot,
// target spawn id, the client's spell record CRC (EQEmu ignores it), u8
// free-target flag + 3 bytes, the free target (CastSpell @0x43c7c1: the
// position argument's three floats in the order given, which the original
// names y, x, z; EQEmu's y_pos, x_pos, z_pos). Checked byte for byte against
// the original's payloads (tests/cast_spell_vectors_test.cpp, CastSpell_Send).
struct CastSpell {
    std::uint32_t slot = 0;
    std::uint32_t spell_id = 0;
    InventorySlot inventory_slot;
    std::uint32_t target_id = 0;
    std::uint32_t spell_crc = 0;
    std::uint8_t free_target = 0;
    std::array<float, 3> position{}; // +0x20: y, x, z as the caller gave them
};
inline constexpr std::size_t kCastSpellSize = 44;
AppPacket encode(const CastSpell& v);
std::optional<CastSpell> decode_cast_spell(const AppPacket& p);
// The packet game::mechanics::cast::cast_spell built (its payload fields; the
// item location's three dwords as the InventorySlot's six halves).
CastSpell cast_spell_packet(const game::mechanics::cast::Packet& p);

// OP_MemorizeSpell (16 bytes, both directions, MemorizeSpell_Struct): slot
// (book slot when scribing, gem otherwise), spell id, scribing, reduction.
// scribing: 0 scribe, 1 memorize, 2 forget, 3 spell bar refresh after a cast
// (server → client; `reduction` is subtracted from the recast time).
struct MemorizeSpell {
    std::uint32_t slot = 0;
    std::uint32_t spell_id = 0;
    std::uint32_t scribing = 0;
    std::uint32_t reduction = 0;
};
inline constexpr std::uint32_t kMemScribe = 0, kMemMemorize = 1, kMemForget = 2, kMemSpellBar = 3;
AppPacket encode(const MemorizeSpell& v);
std::optional<MemorizeSpell> decode_memorize_spell(const AppPacket& p);

// OP_BeginCast, zone → client (10 bytes): u32 spell, u16 caster, u32 cast time ms.
struct BeginCast {
    std::uint32_t spell_id = 0;
    std::uint16_t caster_id = 0;
    std::uint32_t cast_time = 0;
};
AppPacket encode(const BeginCast& v); // server direction, for tests
std::optional<BeginCast> decode_begin_cast(const AppPacket& p);

// OP_InterruptCast, zone → client (InterruptCast_Struct): u32 spawn id, u32
// eqstr id, then optional NUL-terminated arguments (EQEmu sends the caster's
// name for the "others" message; none for the caster's own).
struct InterruptCast {
    std::uint32_t spawn_id = 0;
    std::uint32_t message_id = 0;
    std::vector<std::string> args;
};
AppPacket encode(const InterruptCast& v); // server direction, for tests
std::optional<InterruptCast> decode_interrupt_cast(const AppPacket& p);

// The client's interrupt request: an empty OP_ManaChange (/stopcast,
// StopSpellCast reason 0 @0x434a57; zone Handle_OP_ManaChange).
AppPacket encode_stop_cast_request();

// OP_Buff (100 bytes, both directions, SpellBuffPacket_Struct): entity id, an
// 88-byte SpellBuff_Struct, slot, fade. Server → client fade is 1 (remove) or
// 2 (refresh in place).
struct SpellBuff {
    std::uint8_t effect_type = 0; // 0 none, 2 buff
    std::uint8_t level = 0;
    std::uint8_t unknown002 = 0, unknown003 = 0;
    float bard_modifier = 1.0f;
    std::uint32_t spell_id = 0;
    std::int32_t duration = 0; // tics; negative for a permanent buff (EQEmu's signed ticsremaining)
    std::uint32_t caster_id = 0;
    std::uint32_t num_hits = 0;
    float y = 0, x = 0, z = 0;
    std::uint32_t unknown036 = 0;
    std::int32_t slot_data[12] = {};
};
struct BuffPacket {
    std::uint32_t entity_id = 0;
    SpellBuff buff;
    std::uint32_t slot = 0;
    std::uint32_t fade = 0;
};
inline constexpr std::size_t kBuffPacketSize = 100;
AppPacket encode(const BuffPacket& v);
std::optional<BuffPacket> decode_buff(const AppPacket& p);

// OP_BuffCreate and OP_TargetBuffs, zone → client (rof2.cpp ENCODE(OP_BuffCreate)):
// u32 entity, u32 tic timer, u8 all buffs (1 = the whole list), u16 count,
// count × {u32 slot, u32 spell (0xffffffff = removed), i32 tics (negative: permanent), u32 num hits,
// cstr caster}, u8 type (0 own window, 5 PC target, 7 NPC target).
struct BuffIconEntry {
    std::uint32_t slot = 0;
    std::uint32_t spell_id = 0;
    std::int32_t tics = 0; // negative for a permanent buff
    std::uint32_t num_hits = 0;
    std::string caster;
};
struct BuffIcons {
    std::uint32_t entity_id = 0;
    std::uint32_t tic_timer = 0;
    std::uint8_t all_buffs = 0;
    std::vector<BuffIconEntry> entries;
    std::uint8_t type = 0;
};
AppPacket encode(const BuffIcons& v, bool target_window); // server direction, for tests

// The most buff slots a spawn other than the player has in RoF2 (an NPC's or a pet's, EQEmu's RoF2 NPC_BUFFS):
// the target and pet windows ignore an OP_TargetBuffs / OP_PetBuffWindow entry for a slot past it, so a hostile
// packet cannot grow their lists past this many entries.
inline constexpr std::uint32_t kMaxSpawnBuffSlots = 97;
std::optional<BuffIcons> decode_buff_icons(const AppPacket& p);

// OP_BuffRemoveRequest, client → zone (8 bytes): u32 buff slot, u32 entity.
AppPacket encode_buff_remove_request(std::uint32_t slot, std::uint32_t entity_id);

// OP_Action, zone → client (56 bytes, rof2_structs.h ActionAlt_Struct). A
// spell is type 231; it comes once when the spell goes off and again with
// effect_flag 4 when it lands.
struct Action {
    std::uint16_t target = 0;
    std::uint16_t source = 0;
    std::uint16_t level = 0;
    std::uint32_t unknown06 = 0;
    float instrument_mod = 1.0f;
    float force = 0, hit_heading = 0, hit_pitch = 0;
    std::uint8_t type = 0;
    std::uint32_t damage = 0;
    std::uint16_t unknown31 = 0;
    std::uint32_t spell_id = 0;
    std::uint8_t spell_level = 0;
    std::uint8_t effect_flag = 0;
    std::uint8_t spell_gem = 0;
    InventorySlot slot;
    std::uint32_t item_cast_type = 0;
};
inline constexpr std::size_t kActionSize = 56;
AppPacket encode(const Action& v); // server direction, for tests
std::optional<Action> decode_action(const AppPacket& p);

// OP_DeleteSpell (8 bytes, both directions, DeleteSpell_Struct): i16 book
// slot, 2 bytes, u8 success (the server's echo: 1 deleted), 3 bytes.
struct DeleteSpell {
    std::int16_t book_slot = 0;
    std::uint8_t success = 0;
};
AppPacket encode(const DeleteSpell& v);
std::optional<DeleteSpell> decode_delete_spell(const AppPacket& p);

// OP_SwapSpell (8 bytes, both directions, SwapSpell_Struct): u32 from slot, u32 to slot (book slots). The client
// sends it from the spell book's second right click (Send_OP_SwapSpell @0x75ceb0); EQEmu swaps the two slots and
// echoes the packet, which swaps them in the client's book (Handle_OP_SwapSpell @0x75d380).
struct SwapSpell {
    std::uint32_t from_slot = 0;
    std::uint32_t to_slot = 0;
};
inline constexpr std::size_t kSwapSpellSize = 8;
AppPacket encode(const SwapSpell& v);
std::optional<SwapSpell> decode_swap_spell(const AppPacket& p);

// OP_ColoredText, zone → client: u32 chat colour, cstr text (buff fade
// messages, Client::Message with colour).
struct ColoredText {
    std::uint32_t color = 0;
    std::string text;
};
AppPacket encode(const ColoredText& v); // server direction, for tests
std::optional<ColoredText> decode_colored_text(const AppPacket& p);

// OP_Stamina, zone → client (8 bytes): u32 food, u32 water (0..32000 in EQEmu, lower = hungrier).
struct Stamina {
    std::uint32_t food = 0;
    std::uint32_t water = 0;
};
std::optional<Stamina> decode_stamina(const AppPacket& p);

// OP_LoadSpellSet, client → zone (16 bytes): u8 forget[12] (non-zero = forget
// that gem), u32 count (CSpellBookWnd::MemorizeSet via Send_OP_LoadSpellSet @0x646620).
AppPacket encode_load_spell_set(const std::vector<int>& forget_gems);

// OP_LinkedReuse, zone → client (12 bytes): u32 timer id (1..19), u32 end, u32 start (seconds).
struct LinkedReuse {
    std::uint32_t timer_id = 0;
    std::uint32_t end_time = 0;
    std::uint32_t start_time = 0;
};
std::optional<LinkedReuse> decode_linked_reuse(const AppPacket& p);

// OP_DisciplineTimer, zone → client (12 bytes, EQEmu DisciplineTimer_Struct): u32 timer id, u32 seconds left, u32.
struct DisciplineTimer {
    std::uint32_t timer_id = 0;
    std::uint32_t seconds = 0;
};
std::optional<DisciplineTimer> decode_discipline_timer(const AppPacket& p);

// OP_BlockedBuffs / OP_RemoveBlockedBuffs, both ways (128 bytes, RoF2 BlockedBuffs_Struct): 30 × i32 spell ids
// (-1 none), u32 count, u8 pet, u8 initialise, u16 flags. To the server: initialise 1 replaces the list (sent at
// zone-in: EQEmu keeps it per zone session), 0 adds `count` spells, OP_RemoveBlockedBuffs removes them; the server
// answers with the whole list (OP_BlockedBuffs, flags 0x54) or the spells removed. EQEmu's own list holds 20.
inline constexpr int kBlockedBuffSlots = 30;
struct BlockedBuffs {
    std::vector<std::int32_t> spells; // at most 30
    bool pet = false;
    bool initialise = false;
    std::uint16_t flags = 0;
    // The count sent when it is not the list's size (Cmd_blockspell sends its 30 slots, -1 where an argument was
    // not a number, and the count of the numbers).
    std::optional<std::uint32_t> count;
};
inline constexpr std::size_t kBlockedBuffsSize = 128;
AppPacket encode(const BlockedBuffs& v, bool remove = false);
std::optional<BlockedBuffs> decode_blocked_buffs(const AppPacket& p);
// /blockspell clear (Cmd_blockspell @0x4fc730): OP_ClearBlockedBuffs {u8 pet} (EQEmu clears the list and echoes it).
AppPacket encode_clear_blocked_buffs(bool pet);

// OP_UpdateAura (EQEmu zone/aura.cpp, eq_packet_structs.h AuraCreate_Struct): Z→C action 0 adds an aura (80 bytes:
// u32 0, u32 type, char[64] name, u32 the aura's spawn id, u32 icon), 1 removes one (8 bytes: u32 1, u32 spawn id;
// the client sends it to remove an aura, and the server echoes it), 2 clears them all (4 bytes, on camp or zoning).
struct AuraUpdate {
    std::uint32_t action = 0, type = 0, spawn_id = 0, icon = 0;
    std::string name;
};
inline constexpr std::uint32_t kAuraAdd = 0, kAuraRemove = 1, kAuraClear = 2;
std::optional<AuraUpdate> decode_aura_update(const AppPacket& p);
AppPacket encode(const AuraUpdate& v); // both directions (the remove), and the server's for tests

// OP_SpellEffect, zone → client (28 bytes, eq_packet_structs.h SpellEffect_Struct, passed through for RoF2): an
// effect of spellsnew.eff by number (not a spell) played on spawns. CEverQuest::HandleWorldMessage @0x4d0087: `add`
// (+0x19) set gives SpellEffectTimers_Add(caster, target, effect, stage1_ms, stage1_length_ms, stage2_length_ms,
// full); clear, SpellEffectTimers_Remove(caster, target, effect). EQEmu's Mob::SendSpellEffect sends a spell's
// nimbus (spells_new column 193) this way, {500, 0, 3000, 1, 1}.
struct SpellEffectPacket {
    std::uint32_t effect = 0;           // +0x00 spellsnew.eff index
    std::uint32_t caster = 0;           // +0x04 spawn id
    std::uint32_t target = 0;           // +0x08 spawn id
    std::uint32_t stage1_ms = 0;        // +0x0c EQEmu "Duration": when the projectile stage plays
    std::uint32_t stage1_length_ms = 0; // +0x10 "FinishDelay": the impact stage plays this long after it
    std::uint32_t stage2_length_ms = 0; // +0x14 the impact stage's duration
    bool full = false;                  // +0x18 the emitters at density and opacity 1 (SpawnStageEmitters' flag)
    bool add = false;                   // +0x19 0: remove
    std::uint16_t unknown = 0;          // +0x1a not read by the client
};
AppPacket encode(const SpellEffectPacket& v); // server direction, for tests
std::optional<SpellEffectPacket> decode_spell_effect(const AppPacket& p);

// OP_AddNimbusEffect / OP_RemoveNimbusEffect, zone → client (8 bytes, RemoveNimbusEffect_Struct for both): u32 spawn
// id, i32 spellsnew.eff index. Handle_OP_AddNimbusEffect @0x4bce70 plays the effect's impact stage on the spawn;
// Handle_OP_RemoveNimbusEffect @0x4b7a10 stops spawn << 16 | effect.
struct NimbusEffect {
    std::uint32_t spawn_id = 0;
    std::int32_t effect = 0;
};
AppPacket encode(const NimbusEffect& v, bool add); // server direction, for tests
std::optional<NimbusEffect> decode_nimbus_effect(const AppPacket& p);

} // namespace hailstone::net
