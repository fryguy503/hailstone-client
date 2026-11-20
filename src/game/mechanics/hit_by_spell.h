#pragma once

// EQ_Character::HitBySpell @0x43e8c0: what the client does when a spell
// lands on it (OP_Action type 231 -> PlayerZoneClient::IWasHit). In order:
//  1. validation (a spawn, launch data, IsValidSpellID, the spell, the caster);
//  2. Divine Aura: refused when the SPA 40 total is above 0, except spell 982,
//     a first SPA of 145 and spells with +0x240;
//  3. a bard song's level = packet level + the caster's SPA 272;
//  4. the stun / class, self-cast, level and PvP refusals, group-only
//     beneficial spells;
//  5. a DoSpellCheck failure sends CMD_EXITGAME;
//  6. a spell with type +0x26b == 0 removes the first SPA 12/28/29/314/315/316
//     buff, each in turn;
//  7. the duration: the override, else PvPSpellDuration (a detrimental spell
//     between two PCs or mercenaries), else SpellDuration;
//  8. SPA 123 (immunity): message 0x30a0;
//  9. an instant spell (+0x1c == 0) runs loop 1 (drunkenness, cures, bind,
//     gate/teleport, the target packet, mana, books, identify, counters, gem
//     timers, interrupts, shadow step, pull, fling) with the spell 87, 88/2774
//     and 742 level adjustments, the song value and the modifier;
// 10. a buff asks FindAffectSlot, fills the EQ_Affect and runs loop 2;
// 11. the spell's push: speed += table(heading) * push_back, z += push_up;
// 12. stats dirty; returns 1.
//
// Follows the original exactly: the objects are plain structs (PlayerClient, EQ_Spell,
// EQ_Affect fields at the original's offsets, named), every callee the
// original calls through a function or virtual pointer is a HitQueries call
// (the order and arguments are the original's), and the exe helpers it calls
// directly (IsValidSpellID, EQ_Spell::SpellAffects / IsEffectIndexUsed,
// the EQ_Affect slot-data helpers, the collision-info constructors, Vec3_Cross,
// Matrix3x3_TransformVec3, Ray_SetFromPoints) are ported here. Floats are
// kept as their bits; every x87 step runs at the FPU's precision
// (game/mechanics/fpu.h), a copy through the FPU quiets a signalling NaN.
// Checked against HitBySpell_Validation / _Instant / _Items / _Moves / _Buff
// (tests/hit_by_spell_vectors_test.cpp).

#include <array>
#include <cstdint>

#include "game/mechanics/fpu.h"

namespace hailstone::game::mechanics::hit {

// An object the callees know by address (a character, a PcClient, an item,
// a text), for the calls that only pass it on.
using Handle = std::uintptr_t;

struct Character;

// The PlayerClient fields HitBySpell reads and writes (float fields as bits).
struct Spawn {
    std::uint8_t type = 0;            // +0x125: 0 player
    std::uint8_t flag_208 = 0;        // +0x208
    std::uint32_t owner_38c = 0;      // +0x38c
    std::uint32_t spawn_id = 0;       // +0x148
    std::uint32_t floor_z = 0;        // +0x28
    std::uint32_t y = 0, x = 0, z = 0; // +0x64 / +0x68 / +0x6c
    std::uint32_t speed_y = 0, speed_x = 0, speed_z = 0; // +0x70 / +0x74 / +0x78
    std::uint32_t rotation_speed = 0; // +0x7c
    std::uint32_t heading = 0;        // +0x80
    std::uint32_t heading_speed = 0;  // +0x8c
    std::uint32_t model_height = 0;   // +0x138
    std::uint8_t class_id = 0;        // +0xeb8
    std::int32_t casting_spell = -1;  // +0x448
    std::uint8_t stand_state = 0;     // +0x35c
    std::uint8_t flag_428 = 0;        // +0x428
    Character* character = nullptr;   // +0x2cc
    std::array<char, 64> name{};      // +0xa4
};

// The EQ_Spell fields HitBySpell reads.
struct Spell {
    std::uint32_t range = 0;          // +0 (float)
    std::uint32_t push_back = 0;      // +8 (float)
    std::uint32_t push_up = 0;        // +0xc (float)
    std::uint32_t duration_formula = 0; // +0x1c: 0 instant
    std::array<std::int32_t, 12> base{};  // +0x2c
    std::array<std::int32_t, 12> base2{}; // +0x5c
    std::array<std::int32_t, 12> max{};   // +0x8c
    std::array<std::int32_t, 12> attrib{}; // +0x11c: SPA, 254 blank
    std::uint32_t recourse_178 = 0;   // +0x178
    std::uint32_t timer_id_198 = 0;   // +0x198: linked reuse timer
    std::uint8_t flag_240 = 0;        // +0x240
    std::uint8_t spell_type = 0;      // +0x26b: 0 detrimental, 1 beneficial
    std::uint8_t byte_26c = 0;        // +0x26c
    std::uint8_t target_type = 0;     // +0x26e: 0xd / 0x14 songs on oneself
    std::array<char, 64> name{};      // +0x27a
    std::array<char, 32> teleport_zone{}; // +0x2da
    // Read by FindAffectSlot and its stacking rules (game/mechanics/find_affect_slot.h), not by HitBySpell.
    std::int32_t id = 0;                     // +0x174
    std::array<std::int32_t, 12> calc{};     // +0xec: the effect formulas
    std::array<std::uint8_t, 36> class_level{}; // +0x246: ClassLevel[class] (index 0 unused)
    std::uint8_t short_buff = 0;        // +0x237 DurationWindow
    std::uint8_t not_stackable_dot = 0; // +0x239
    std::uint8_t no_dispell = 0;        // +0x242
    std::uint8_t is_skill = 0;          // +0x244
    std::uint8_t stacks_with_self = 0;  // +0x4df
    std::uint8_t no_remove = 0;         // +0x4e3
};

// An EQ_Affect (buff slot).
struct Affect {
    std::uint8_t type = 0;      // +0
    std::uint8_t level = 0;     // +1
    std::uint8_t byte_2 = 0;    // +2
    std::uint8_t byte_3 = 0;    // +3
    std::uint32_t modifier = 0; // +4 (float)
    std::int32_t spell_id = 0;  // +8
    std::int32_t duration = 0;  // +0xc
    std::int32_t caster_10 = 0; // +0x10
    std::int32_t dword_14 = 0;  // +0x14
    std::uint32_t y_18 = 0, x_1c = 0, z_20 = 0; // +0x18..+0x20 (floats; SPA 441)
    std::uint32_t flags_24 = 0; // +0x24: the slot-bit array's first dword
    std::array<std::int32_t, 12> slot_data{}; // +0x28
    bool counters_known = false; // Hailstone metadata, not part of EQ_Affect
};

// The CharacterBase part HitBySpell touches.
struct CharacterBase {
    std::uint32_t y = 0, x = 0, z = 0, heading = 0; // +0x2c..+0x38 (floats)
    std::uint8_t mesmerized = 0;  // +0xfc
    std::int32_t zone_id = 0;     // +0x100
};

// A CharacterZoneClient (the character a spell lands on, or a caster's).
struct Character {
    std::uint8_t cache_valid = 0;    // +0x1a4: the spell effect cache
    std::uint8_t aa_cache_valid = 0; // +0x1ac
    std::uint8_t stats_dirty = 0;    // +0xc
    Spawn* spawn = nullptr;          // +8
    CharacterBase* base = nullptr;   // the virtual base (+4 + vbtable[1])
};

// The profile fields (ProfileManager::GetCurrentProfile).
struct Profile {
    std::uint8_t class_id = 0;   // +0x3374
    std::int32_t weight = 0;     // +0x31a8
    std::int32_t mana = 0;       // +0x338c
    std::int32_t dword_3248 = 0; // +0x3248 (SPA 65 clears it)
    std::int32_t dword_3264 = 0; // +0x3264 (SPA 66)
};

// A PcClient: its +0x20a8 (the id a PC caster's buffs record) and its gem
// timers (+0x2dd0 -> +0x2e8..+0x314, or none).
struct Pc {
    std::int32_t id_20a8 = 0;
    std::int32_t* gem_timers = nullptr; // 12 entries
};
inline constexpr Handle kPcCharacter = 0x2dc8; // pc + 0x2dc8: its CharacterZoneClient part
inline constexpr Handle kPcBase = 0x108;        // pc + 8 + vbtable[1]: its PcBase

// OP_Action's record (the fields HitBySpell reads).
struct Hit {
    std::uint16_t caster_id = 0;   // +2
    std::uint16_t item_level = 0;  // +4
    std::uint32_t modifier = 0;    // +0xa (float): the instrument / focus modifier
    std::uint32_t push_heading = 0; // +0x12 (float)
    std::uint16_t word_1b = 0;     // +0x1b
    std::int32_t song_value = 0;   // +0x1d
    std::uint32_t spell_id = 0;    // +0x21
    std::uint8_t caster_level = 0; // +0x25
};

// LaunchSpellData.
struct Launch {
    std::uint8_t byte_32 = 0;   // +0x32: FindAffectSlot's last argument
    std::uint8_t item_flag = 0; // +0x40
};

// Globals HitBySpell reads and writes.
struct Globals {
    std::uint32_t ruleset = 0;    // 0xdcee70: the server's PvP ruleset
    std::uint8_t zone_type = 0;   // __ZoneType
    std::uint8_t bind_blocked = 0; // 0xddfc69
    bool connected = false;       // __gWorld
    std::uint8_t no_send_log = 0; // 0xddf654
    Spawn* target = nullptr;      // pinstTarget
    Spawn* local_player = nullptr;
    Spawn* controlled_player = nullptr;
    std::uint8_t local_pushed = 0;      // 0xddfd59
    std::uint8_t controlled_pushed = 0; // 0xddfd58
    std::int32_t save_reason = 0;       // 0xddfd74
    std::uint16_t packet_opcode = 0;    // 0xdd00e0 (the OP 0x56b1 send buffer)
    std::int32_t packet_target = 0;     // 0xdd00e2
    std::int32_t send_count = 0;        // 0xf99584
};

// CCollisionInfoBase (@0x441c90) fields the DLL's tests read.
struct CollisionInfo {
    std::uint32_t vtable = 0; // exe address
    std::array<std::uint32_t, 3> from{}, to{}; // +4, +0x10
    std::array<std::uint32_t, 3> delta{};      // +0x1c
    std::array<std::uint32_t, 3> direction{};  // +0x28 (normalised delta)
    std::uint32_t inverse_length = 0;          // +0x34
    std::uint32_t fraction = 0;                // +0x38 (1.0)
    std::array<std::uint32_t, 3> normal{};     // +0x3c (not written: the frame's old bytes)
    std::uint8_t moving = 0;                   // +0x68
    std::int32_t mask_64 = -1;                 // +0x64
};
// CCollisionInfoSphere (@0x441ea0): the base, then +0x74.. .
struct CollisionSphere {
    CollisionInfo info;
    std::uint32_t radius = 0, radius_sq = 0, inverse_radius = 0; // +0x74, +0x84, +0x88
    std::array<std::uint32_t, 3> centre{};  // +0x78
    std::uint32_t filter = 0, callback = 0; // +0x8c, +0x90 (exe addresses)
    std::uint32_t dword_94 = 0, dword_98 = 0; // +0x94, +0x98
    std::uint8_t byte_9c = 0;               // +0x9c
};
// CCollisionInfoLineSegmentVisibility: the base, then the ray copy.
struct CollisionLine {
    CollisionInfo info;
    std::array<std::uint32_t, 6> ray{}; // +0x74..+0x88: start, direction
    std::uint8_t byte_8c = 0;           // +0x8c
};

// OP_Fling's record for PlayerZoneClient_ApplyFling.
struct FlingRecord {
    std::array<std::uint32_t, 8> words{}; // +0..+0x1c
};

// The 0x20c bytes of the original's frame from esp+0x2e8 (DoSpellCheck's
// record, later message text and the SPA 379 sphere, whose normal is read
// back from here when the DLL's test wrote none).
using SpellCheckRecord = std::array<std::uint8_t, 0x20c>;

// Every callee, in the original's words. Pointers are the struct above or
// null; `level_dword` / `index_dword` are byte values the original pushes as
// whole dwords (only their low byte is defined; callees read the byte).
class HitQueries {
public:
    virtual ~HitQueries() = default;
    // Virtuals (the spell manager, the CharacterBase, spawns, EQGraphicsDX9).
    virtual const Spell* spell_by_id(std::uint32_t id) = 0;   // pinstSpellManager +0xc
    virtual Handle cb_pc() = 0;                               // CharacterBase vf4c: the PcClient, or 0
    virtual std::uint32_t cb_level() = 0;                     // CharacterBase vf68
    virtual std::uint32_t cb_max_mana() = 0;                  // CharacterBase vf6c
    virtual std::uint8_t spawn_vf_dc(Spawn* spawn) = 0;       // the spawn's vfdc
    virtual void pc_base_vf58(Handle pc_base) = 0;
    virtual std::uint8_t point_in_world(const std::array<std::uint32_t, 3>& p) = 0; // pinstSceneGraph +0x4c
    virtual std::uint8_t actor_collision_enabled() = 0;       // pinstCollision +0x48
    virtual void set_actor_collision(std::uint8_t flag) = 0;  // +0x50
    virtual std::uint8_t collide_sphere(CollisionSphere& sphere) = 0; // +0x08 (may write the normal)
    virtual std::uint8_t terrain_collision_enabled() = 0;     // +0x44
    virtual std::uint8_t collide_line(CollisionLine& line) = 0; // +0x00
    // CDisplay::GetPlayerFloorHeight(x, y, z, 1) on the spawn: the x87 value.
    virtual double floor_height(Spawn* spawn, std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;
    virtual double table_cos(std::uint32_t angle) = 0; // the sine table's +0xc: x87 value
    virtual double table_sin(std::uint32_t angle) = 0; // +0x8

    // Functions.
    virtual Spawn* spawn_by_id(std::uint32_t id) = 0;
    virtual void recache_spell_effects(Character* c) = 0;
    virtual std::int32_t cached_buff_effect(Character* c, std::int32_t spa) = 0;
    virtual std::int32_t cached_aa_effect(Character* c, std::int32_t spa) = 0;
    virtual std::int32_t cached_item_effect(Character* c, std::int32_t spa) = 0;
    virtual std::uint32_t bard_cast_bard(const Spell* spell, std::uint32_t class_id) = 0;
    virtual std::uint8_t sub_00433830(const Spell* spell, Character* caster_character) = 0;
    virtual Handle character_of(Spawn* spawn) = 0; // 0x59f730: the spawn's PcClient
    virtual void token_to_client(Handle to, std::uint32_t id, std::uint32_t color) = 0;
    virtual std::uint8_t is_item_cast(const Launch* launch) = 0;
    virtual std::uint8_t is_self_or_group_member(Handle pc_character, const char* name) = 0;
    // Not a call: the name a PcClient keeps (+8 vbtable, +0x44), for the PvP check.
    virtual const char* pc_name(Handle pc) = 0;
    virtual std::uint32_t do_spell_check(std::uint32_t spell_id, SpellCheckRecord& record) = 0;
    virtual void log_exit_game() = 0;  // DebugLog_Printf("*** DISCONNECTING: HitBySpell() ...", file, 0x395b)
    virtual void execute_cmd(std::uint32_t cmd) = 0; // __ExecuteCmd(cmd, 0, 0)
    virtual std::uint8_t max_effects(Character* c) = 0;
    virtual Affect* get_effect(Character* c, std::uint32_t index) = 0;
    virtual void remove_affect(Affect* a) = 0; // RemoveAffect(a, 1, 0)
    virtual void cur_hp(std::uint32_t type) = 0; // Cur_HP(type, 1)
    virtual std::uint8_t is_pc_or_mercenary(Spawn* s) = 0;
    virtual std::int32_t pvp_spell_duration(const Spell* spell, std::uint32_t level_dword) = 0;
    virtual std::int32_t spell_duration(const Spell* spell, std::uint32_t level_dword) = 0;
    // CalcAffectChange(spell, level, index, 0, -1, caster, 0, -1, flag).
    virtual std::int32_t calc_affect_change(const Spell* spell, std::uint32_t level_dword, std::uint32_t index_dword,
                                            Spawn* caster, std::uint32_t flag) = 0;
    // BuildStringFromEQSTR(out, id, arg, 0 ...): writes the text into `out`.
    virtual void build_string(char* out, std::uint32_t id, const char* arg) = 0;
    virtual void dsp_chat(const char* text, std::uint32_t color) = 0; // dsp_chat(text, color, 1, 1)
    virtual void dsp_chat_2(const char* text) = 0;
    virtual std::uint8_t get_level(Spawn* s) = 0; // PlayerZoneClient::GetLevel
    virtual Profile* current_profile() = 0;       // ProfileManager::GetCurrentProfile
    virtual void record_position_sample(Spawn* s, std::uint32_t kind) = 0; // (kind, 0, 0)
    virtual Spawn* vehicle_of(Spawn* s) = 0;      // 0x58cf90
    virtual Spawn* mount_spawn(Spawn* s) = 0;     // PlayerClient_GetMountSpawn
    // Loop 1.
    virtual std::int32_t get_drunkenness(Handle pc) = 0;
    virtual void set_drunkenness(Handle pc, std::int32_t value) = 0;
    virtual std::int32_t random_percent() = 0;    // RandomPercentRoll
    virtual std::uint8_t has_property(Spawn* s, std::uint32_t property) = 0; // (property, 0, 0)
    virtual std::uint32_t get_150(Spawn* s) = 0;  // unk_PlayerClient_Get150
    virtual std::uint32_t bind_spot_ok(Spawn* where) = 0; // EQ_Character::sub_00432e90
    virtual std::uint32_t expansion_zone(std::int32_t zone) = 0;
    virtual void set_bind_zone(Handle pc, std::uint32_t slot, std::int32_t zone) = 0;
    virtual void set_bind_y(Handle pc, std::uint32_t slot, std::uint32_t v) = 0;
    virtual void set_bind_x(Handle pc, std::uint32_t slot, std::uint32_t v) = 0;
    virtual void set_bind_z(Handle pc, std::uint32_t slot, std::uint32_t v) = 0;
    virtual void set_bind_heading(Handle pc, std::uint32_t slot, std::uint32_t v) = 0;
    virtual void save_pc() = 0;                   // CEverQuest::SavePC(0, 1, 0xe)
    virtual void set_150(Spawn* s) = 0;           // 0x8cfd10(0)
    virtual void play_sound(Spawn* s, std::uint32_t sound) = 0;
    virtual std::int32_t zone_index(const char* name) = 0; // EQWorldData::GetIndexFromZoneName
    // MoveToZone(zone, "portal spell", 0, 7, x, y, z, heading, -1, -1, 0).
    virtual void move_to_zone(std::int32_t zone, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                              std::int32_t heading) = 0;
    virtual void update_player_actor(Spawn* s) = 0; // SetCCreateCamera
    virtual void sub_00432bc0(std::uint32_t range) = 0;
    virtual std::uint16_t hton(std::uint32_t opcode) = 0;
    virtual std::uint8_t send_world(std::uint32_t size) = 0;  // UdpConnection::Send(4, buffer, size)
    virtual void close_world() = 0;                           // NetMgr_CloseConnection
    virtual void log_void_connection(std::uint32_t opcode) = 0; // DebugLog_Printf
    virtual const char* get_string(std::uint32_t id) = 0;
    virtual void set_endurance(std::int32_t v) = 0; // 0x441e50 on the CharacterBase
    virtual void cur_endurance() = 0;               // Cur_Endurance(1)
    virtual void cur_mana() = 0;                    // Cur_Mana(1)
    virtual void feign(std::uint32_t spell_id, std::int32_t v) = 0; // StopCastAndFeignDeath
    virtual Handle get_item(std::uint32_t slot) = 0; // unk_Container_GetItem(&ref, slot) on the CharacterBase
    virtual void release_item() = 0;                 // VePointer::~VePointer
    virtual const char* lore_name(Handle item) = 0;
    virtual std::int32_t item_type(Handle item) = 0;
    virtual std::uint32_t book_lang(Handle item) = 0;
    virtual const char* lang_desc(std::uint32_t lang) = 0;
    virtual std::uint8_t is_magic(Handle item) = 0; // IsMagic(1)
    virtual Affect* pc_spell_affect(std::int32_t spa) = 0; // GetPCSpellAffect(spa, 0, 0)
    virtual void remove_pc_affect(Affect* a, std::uint32_t caster_id) = 0; // RemovePCAffectex
    virtual std::int32_t eq_get_time() = 0;
    virtual std::int32_t memorized_spell(Handle pc_base, std::uint32_t gem) = 0;
    virtual std::int32_t spell_focus_pct(const Spell* gem_spell, const Spell* spell) = 0; // (.., 1, 0)
    virtual void set_spell_recast_timer(Handle pc, std::uint32_t gem) = 0; // (gem, 0)
    virtual std::uint32_t fast_time() = 0;
    virtual void set_linked_reuse(Handle pc, std::uint32_t timer, std::uint32_t when) = 0;
    virtual std::int32_t random_below(std::uint32_t n) = 0; // 0x402870
    virtual void stop_current_spell_cast() = 0;             // (1)
    virtual void apply_fling(Spawn* local, const FlingRecord& record) = 0;
    virtual std::uint8_t is_valid_teleport(Spawn* s, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                                           std::uint32_t heading, std::uint32_t distance) = 0;
    // The buff path.
    virtual std::uint8_t find_buff_level(std::uint32_t spell_id) = 0; // FindBuffLevelBySpellID(id, 0)
    // FindAffectSlot(id, caster, &slot, 0, level, 0, 0, launch_dword).
    virtual Affect* find_affect_slot(std::uint32_t spell_id, Spawn* caster, std::int32_t* slot,
                                     std::uint32_t level_dword, std::uint32_t launch_dword) = 0;
    virtual void set_buff_caster_id(std::uint32_t slot, std::uint32_t id) = 0;
    virtual std::uint8_t has_buff_flag(const Spell* spell) = 0; // 0x453520
    virtual void on_new_buff(std::uint32_t slot) = 0;            // 0x45b250
    virtual std::int32_t refused_mez(std::int32_t max, Spawn* caster, const Spell* spell) = 0; // 0x4432a0
    virtual std::int32_t total_effect(Character* c, std::int32_t spa) = 0; // TotalEffect(spa, 1, 0, 1, 1)
    virtual void root_aa(std::int32_t total) = 0; // EmptyFunction_00443570("POP_AA_EnhancedRootOnMe", total)
    virtual void movement_rate(std::int32_t v) = 0; // eqspa_movement_rate
    virtual std::uint32_t change_form(const Spell* spell, std::uint32_t id, std::int32_t form, Affect* a) = 0;
    virtual void lose_control_message(Spawn* s) = 0;
    virtual void update_all_visibility() = 0;
    virtual void set_view_actor(const char* name) = 0;
    virtual void spa87(std::int32_t v) = 0;   // 0x4940b0 on CDisplay
    virtual void spa57(std::int32_t v) = 0;   // 0x432b20
    virtual void update_visible_status() = 0; // UpdateMyVisibleStatus(0)
    virtual std::int32_t random_low_high(std::int32_t lo, std::int32_t hi) = 0;
    // CalcAffectChangeGeneric(spell, caster id, index, affect, base, 1).
    virtual std::int32_t calc_affect_change_generic(const Spell* spell, std::uint32_t caster_id,
                                                    std::uint32_t index_dword, Affect* a, std::int32_t base) = 0;
    virtual void add_physics_effect(Spawn* s, std::uint32_t spell_id, std::uint32_t caster_id, std::uint32_t slot,
                                    std::int32_t value) = 0;
    virtual void apply_post_effect(const Spell* spell, std::uint32_t index) = 0; // (.., 1)
    virtual void spa436() = 0; // 0x4325e0(1)
};

// The original's frame buffers that outlive a callee's use of them: the
// record at esp+0x2e8, the message text at esp+0x128 and the lore name at
// esp+0x234 (later texts overwrite earlier ones; the SPA 379 sphere reads its
// normal from the record).
struct HitFrame {
    SpellCheckRecord record{};
    std::array<char, 0x100> text{};
    std::array<char, 0x90> lore{};
};

// Everything HitBySpell reads and writes besides the callees.
struct HitState {
    Character* self = nullptr; // this (its spawn may be null)
    Globals* globals = nullptr;
    HitFrame* frame = nullptr; // optional: the frame to use (else a local one)
    bool rof2_single_slot_cures = false; // replay the original rather than applying cures to the buff total
    // Bits of counter SPAs faded by the server earlier in this network update; those cures are already applied.
    std::uint8_t faded_counter_spas = 0;
};

// EQ_Character::HitBySpell(hit, launch, duration): 1 when the spell landed,
// 0 when refused. `launch` may be null.
std::uint8_t hit_by_spell(HitState& state, const Hit& hit, const Launch* launch, std::int32_t duration,
                          HitQueries& q, const Fpu& fpu = Fpu{});

// The exe helpers, exposed for the client and the tests.
bool is_valid_spell_id(std::uint32_t id);                      // IsValidSpellID @0x4ad150: 1..45000
std::uint8_t spell_affects(const Spell* spell, std::int32_t spa); // EQ_Spell::SpellAffects: index + 1, or 0
bool effect_index_used(const Spell& spell, std::uint32_t index); // EQ_Spell::IsEffectIndexUsed @0x433f80

} // namespace hailstone::game::mechanics::hit
