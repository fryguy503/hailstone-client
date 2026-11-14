#pragma once

// Typed RoF2 zone-server packets: the zone-in handshake, the player profile,
// spawns, movement, chat, zoning and camping. Layouts follow the NMS server's
// EQEmu RoF2 translator (common/patches/rof2.cpp ENCODE/DECODE, rof2_structs.h)
// and, for pass-through opcodes, common/eq_packet_structs.h; per-packet notes
// are in docs/protocol/rof2-packets.md.
//
// Positions are EQ units: x/y/z in world units, heading 0..512 (a full turn),
// deltas in units per tick as the server stores them. The wire uses fixed
// point: coords ×8 (19-bit), deltas ×64 (13-bit), heading ×4 (12-bit),
// delta heading ×20 (10-bit) (common/misc_functions.cpp FloatToEQ*).
//
// Handshake packets without payload (OP_ReqClientSpawn, OP_SendExpZonein,
// OP_WorldObjectsSent, OP_ClientReady, OP_SendAATable, OP_UpdateAA,
// OP_Logout, OP_WorldClientReady) are built with make_empty() from
// net/packet_io.h.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "net/packet.h"

namespace hailstone::net {

// ---------------------------------------------------------------- shared

struct Motion {
    float x = 0, y = 0, z = 0;
    float heading = 0; // 512ths; received headings are signed (-256..256)
    float delta_x = 0, delta_y = 0, delta_z = 0;
    float delta_heading = 0;
    std::int32_t animation = 0; // raw 10-bit value (signed)
    float pitch = 0;            // CameraAngle (received: 12 bits x 0.25)
    float speed_run = 0;        // received: the animation field x 0.025 (the original's SpeedRun)
};

struct EquipTexture {
    std::uint32_t material = 0, unknown1 = 0, elite_material = 0, hero_forge_model = 0, material2 = 0;
};

// ---------------------------------------------------------------- client → zone

// OP_ZoneEntry, client → zone (76 bytes, ClientZoneEntry_Struct). EQEmu
// identifies a RoF2 zone stream by this being the first packet with 76 bytes.
// The original client fills +0 with __MemChecker0 of the name's 32-byte
// buffer (eq_crc32(name) here: the bytes after the name are its stack) and
// +68/+72 with the spell files' CRC-32 (SpellTable::file_crc /
// assoc_file_crc); EQEmu reads only the name.
struct ZoneEntryRequest {
    std::string name;
    std::uint32_t name_crc = 0;  // 0 = compute eq_crc32(name)
    std::uint32_t spell_crc = 0; // +68
    std::uint32_t spell_assoc_crc = 0; // +72
};
inline constexpr std::size_t kZoneEntryRequestSize = 76;
AppPacket encode(const ZoneEntryRequest& v);
std::optional<ZoneEntryRequest> decode_zone_entry_request(const AppPacket& p);

// OP_ClientUpdate, client → zone (46 bytes, PlayerPositionUpdateClient_Struct;
// passed through untranslated).
struct ClientPositionUpdate {
    std::uint16_t sequence = 0; // +1 per packet
    std::uint16_t spawn_id = 0;
    std::uint16_t vehicle_id = 0;
    Motion motion;
};
inline constexpr std::size_t kClientPositionUpdateSize = 46;
AppPacket encode(const ClientPositionUpdate& v);
std::optional<ClientPositionUpdate> decode_client_position_update(const AppPacket& p);

// OP_Camp: u32 kind (0 = character select, 1 = desktop, 2 = server); EQEmu
// ignores the payload and starts its 29 s camp timer.
AppPacket encode_camp(std::uint32_t kind);

// OP_Save (0x4a39, Save @0x52ae60) and OP_SaveOnZoneReq (0x600d, SavePCForce @0x51d4d0 with argument 0), C->Z,
// 484 bytes each: u32 __MemChecker0 of the 476 bytes from +5, u8 kind (Save's third argument: 0x21 Send_OP_Camp,
// 0x22 / 0x23 /quit and /exit; SavePCForce 0), then f32 y, x, z (server order), i32 heading (truncated), 22 x 20-byte
// armour records (PcBase_GetArmorProperties: the profile's, the nine worn ones from the actor first), u8 on a
// vehicle (CharacterBase +0xbc, the vehicle name, non-empty) and 3 bytes, u32 minutes played (PcBase +0x2074),
// i32 primary and secondary actor (the IT number of the items in slots 13 / 14), i32 air supply (+0x2084), and 3
// zero bytes. EQEmu (Handle_OP_Save, Handle_OP_SaveOnZoneReq) reads nothing of it and saves the character.
struct SaveRequest {
    std::uint8_t kind = 0;
    float y = 0, x = 0, z = 0;
    std::int32_t heading = 0;
    std::array<EquipTexture, 22> armor{};
    bool vehicle = false;
    std::uint32_t minutes_played = 0;
    std::int32_t primary = 0, secondary = 0, air_supply = 0;
};
inline constexpr std::size_t kSaveRequestSize = 484;
AppPacket encode_save(const SaveRequest& v);         // OP_Save
AppPacket encode_save_on_zone(const SaveRequest& v); // OP_SaveOnZoneReq
std::optional<SaveRequest> decode_save(const AppPacket& p);

// ---------------------------------------------------------------- zone → client

// Spawns arrive one per packet with opcode OP_ZoneEntry (the server sends
// OP_ZoneSpawns/OP_NewSpawn as one OP_ZoneEntry each, rof2.cpp
// ENCODE(OP_ZoneSpawns)); OP_NewSpawn is accepted too. The first one after
// the client's OP_ZoneEntry is the player's own spawn.
struct InteractiveObject {
    std::string model, name2, string;
    std::uint32_t appearance = 0, animation_id = 0;
    std::array<std::uint32_t, 10> sound_ids{};
    std::uint8_t collidable = 0;
    std::uint32_t object_type = 0;
};
struct Spawn {
    std::string name;
    std::uint32_t spawn_id = 0;
    std::uint8_t level = 0;
    float melee_radius = 0; // EQEmu sends size - 0.7
    std::uint8_t npc = 0;   // 0 PC, 1 NPC, 2/3 corpse
    // Spawn_Struct_Bitfields (one dword).
    std::uint32_t bitfield = 0;
    std::uint8_t gender = 0;
    bool is_pet = false, afk = false, gm = false, sneak = false, lfg = false, linkdead = false, show_helm = false;
    std::uint8_t anon = 0;
    std::uint16_t invis = 0;
    bool trader = false, targetable = false, targetable_with_hotkey = false, show_name = false;
    std::uint8_t other_data = 0; // bit0 buyer, bit1 offline mode, bit2 interactive/LDoN, bit4 title, bit5 suffix
    float emitter_scaling_radius = -1;
    std::uint32_t default_emitter_id = 0;
    std::optional<InteractiveObject> interactive;
    std::vector<std::uint32_t> properties; // CharacterPropertyHash; [0] is the body type
    std::uint32_t body_type = 0;
    std::uint8_t hp_percent = 0;
    std::uint8_t hair_color = 0, beard_color = 0, eye_color1 = 0, eye_color2 = 0, hair_style = 0, beard = 0;
    std::uint32_t drakkin_heritage = 0, drakkin_tattoo = 0, drakkin_details = 0;
    std::uint8_t texture_type = 0; // equip_chest2 (0xff = per-slot armor)
    std::uint8_t material = 0, variation = 0;
    std::uint8_t head_type = 0; // helm
    float size = 0;
    std::uint8_t face = 0;
    float walk_speed = 0, run_speed = 0;
    std::uint32_t race = 0;
    std::uint8_t holding = 0;
    std::uint32_t deity = 0;
    std::uint32_t guild_id = 0xffffffff, guild_rank = 0;
    std::uint8_t class_ = 0;
    std::uint8_t pvp = 0, stand_state = 0, light = 0, fly_mode = 0;
    std::string last_name;
    std::uint32_t aa_title = 0;
    std::uint8_t guild_show = 0, temp_pet = 0;
    std::uint32_t pet_owner_id = 0;
    std::uint8_t find_bits = 0;
    std::uint32_t player_state = 0, npc_tint_id = 0, primary_tint = 0, secondary_tint = 0;
    std::uint32_t weapon_tint1 = 0xffffffff, weapon_tint2 = 0xffffffff;
    // Long form (PCs and PC-model races): 9 tints and 9 textures. Short form
    // (other NPC races): textures for slot 0 (zero), primary and secondary only.
    bool long_equipment = true;
    std::array<std::uint32_t, 9> equipment_tint{};
    std::array<EquipTexture, 9> equipment{};
    Motion motion;
    std::string title, suffix;
    std::uint32_t spline1 = 0, spline2 = 0;
    std::uint8_t is_mercenary = 0;
    std::string real_estate_guid;
    std::uint32_t real_estate_id = 0xffffffff, real_estate_item_id = 0xffffffff;
    std::uint8_t always_show_aura = 0;
    std::vector<std::array<std::uint32_t, 4>> physics_effects; // spell, caster, effect, base
    std::array<std::uint32_t, 6> spawn_status{};
};
// True for the races that use the long equipment block (rof2.cpp and the
// client's PlayerZoneClient::LegalPlayerRace: < 13, 128, 130, 330, 522).
bool is_pc_model_race(std::uint32_t race);
AppPacket encode(const Spawn& v); // server direction (OP_ZoneEntry), for tests
std::optional<Spawn> decode_spawn(const AppPacket& p);

// OP_ClientUpdate, zone → client (24 bytes, PlayerPositionUpdateServer_Struct,
// passed through untranslated).
struct SpawnPositionUpdate {
    std::uint16_t spawn_id = 0;
    std::uint16_t vehicle_id = 0;
    Motion motion;
};
inline constexpr std::size_t kSpawnPositionUpdateSize = 24;
AppPacket encode(const SpawnPositionUpdate& v); // server direction, for tests
std::optional<SpawnPositionUpdate> decode_spawn_position_update(const AppPacket& p);
// OP_NPCMoveUpdate, zone → client: the same update as a bit stream
// (Handle_OP_NPCMoveUpdate, game/mechanics/remote_spawns.h); any length.
std::optional<SpawnPositionUpdate> decode_npc_move_update(const AppPacket& p);

// OP_PlayerProfile (variable, ~25 KB): rof2.cpp ENCODE(OP_PlayerProfile)
// writes it field by field with count-prefixed arrays.
struct Bind {
    std::uint32_t zone_id = 0;
    float x = 0, y = 0, z = 0, heading = 0;
};
struct ProfileBuff {
    float instrument_mod = 0;
    std::uint32_t caster_id = 0;
    std::uint32_t counters = 0;
    std::uint32_t duration = 0;
    std::uint8_t level = 0;
    std::uint32_t spell_id = 0;
    std::uint8_t effect_type = 0;
    std::uint32_t num_hits = 0;
};
// One weapon or potion of a bandolier set / the potion belt (BandolierItem_Struct:
// the name, then the item id and icon; id 0 = empty).
struct ProfileBeltItem {
    std::string name;
    std::uint32_t id = 0, icon = 0;
};
struct ProfileBandolier {
    std::string name; // empty = unused set
    std::array<ProfileBeltItem, 4> items; // primary, secondary, range, ammo
};
struct PlayerProfile {
    std::uint32_t checksum = 0;      // eq_crc32 of bytes [8, size - 1)
    std::uint32_t checksum_size = 0; // size - 9
    std::uint8_t gender = 0;
    std::uint32_t race = 0;
    std::uint8_t class_ = 0; // NMS multiclass characters are created as Bard (16)
    std::uint8_t level = 0;
    std::vector<Bind> binds; // [0] bind point, [4] home
    std::uint32_t deity = 0;
    std::uint32_t intoxication = 0;
    std::vector<EquipTexture> equipment; // 22 on the wire; the first 9 are worn slots
    std::vector<std::uint32_t> equipment_tint;
    std::uint8_t hair_color = 0, beard_color = 0, eye_color1 = 0, eye_color2 = 0, hair_style = 0, beard = 0, face = 0;
    std::uint32_t drakkin_heritage = 0, drakkin_tattoo = 0, drakkin_details = 0;
    float height = 0;
    std::uint32_t points = 0; // unspent skill/practice points
    std::uint32_t mana = 0;
    std::uint32_t cur_hp = 0;
    std::uint32_t str = 0, sta = 0, cha = 0, dex = 0, int_ = 0, agi = 0, wis = 0;
    std::vector<std::array<std::uint32_t, 3>> aa; // AA id, value, charges
    std::vector<std::uint32_t> skills;
    std::vector<std::uint32_t> innate_skills;
    std::vector<std::uint32_t> disciplines;
    // The three timer arrays land in PlayerProfileData +0x188c / +0x18dc / +0x192c, which
    // PcClient_ApplyBaseProfileData @0x5789b0 copies as they are into PcBase_SetCombatAbilityTimer (profile +0x69bc,
    // the discipline reuse timers), PcBase_SetLinkedSpellTimer (+0x6a0c) and PcBase_SetItemRecastTimer (+0x6a5c), all
    // Unix seconds. EQEmu sends zeros for the first and its recastTimers (the spell reuse timers) as the second.
    std::vector<std::uint32_t> combat_ability_timers;
    std::vector<std::uint32_t> recast_timers; // the linked spell timers
    std::vector<std::uint32_t> item_recast_timers; // by recast type; EQEmu's "Timestamp2", 100 zeros
    std::vector<std::uint32_t> spell_book;  // 0xffffffff = empty
    std::vector<std::uint32_t> mem_spells;  // 0xffffffff = empty
    std::vector<std::uint32_t> gem_refresh;
    std::vector<ProfileBuff> buffs;
    std::uint32_t platinum = 0, gold = 0, silver = 0, copper = 0;
    std::uint32_t platinum_cursor = 0, gold_cursor = 0, silver_cursor = 0, copper_cursor = 0;
    std::uint32_t thirst = 0, hunger = 0;
    std::uint32_t aa_points_spent = 0, aa_points = 0;
    std::vector<ProfileBandolier> bandoliers; // 20 sets on RoF2
    std::vector<ProfileBeltItem> potion_belt; // 5 slots
    // Personal tribute (ENCODE(OP_PlayerProfile)): favour, career favour, and the chosen benefits {tribute id
    // (0xffffffff none), tier} (RoF2: 5).
    std::uint32_t tribute_points = 0, career_tribute_points = 0;
    // RoF2's counted LDoN block: unknown slot 0 followed by the five themes, then the spendable total.
    std::array<std::uint32_t, 5> adventure_points{};
    std::uint32_t adventure_points_available = 0;
    // Leadership (ENCODE(OP_PlayerProfile)): experience toward the next group and raid point (EQEmu: 1000 and 2000 a
    // point), the unspent points, the abilities' ranks by EQEmu's index (0..15 group, 16..31 raid), and whether
    // leadership experience is earned.
    double group_leadership_exp = 0, raid_leadership_exp = 0;
    std::uint32_t group_leadership_points = 0, raid_leadership_points = 0;
    std::vector<std::uint32_t> leadership_ranks;
    std::int32_t air_supply = 0; // the air left (PcBase +0x2084, ApplyProfileData's +0x57e0)
    bool leadership_exp_on = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tributes;
    // The block before the endurance, which the original copies to PcProfile +0x3424..+0x3470
    // (PcClient_ApplyBaseProfileData @0x5789b0). EQEmu sends fixed values.
    std::int32_t base_hp_total = 0, base_endurance_total = 0, base_mana_total = 0; // +0x3424 / +0x3428 / +0x342c
    std::int32_t base_save_cold = 0, base_save_fire = 0, base_save_magic = 0, base_save_disease = 0,
                 base_save_poison = 0, base_save_physical = 0, base_save_corruption = 0; // +0x3438 +0x3434 +0x3430 ...
    std::array<std::int32_t, 5> profile_344c{}; // +0x344c..: [2] HP regen +0x3454, [3] sitting mana regen +0x3458, [4] standing +0x345c
    std::int32_t base_endurance_regen = 0;      // +0x3460
    std::array<std::int32_t, 4> profile_3464{}; // [0] base AC +0x3464, [2] fist damage +0x346c, [3] fist delay +0x3470
    std::uint32_t endurance = 0;
    std::string name;
    std::string last_name;
    std::uint32_t birthday = 0, last_login = 0, time_played_min = 0, expansions = 0;
    // The slots the character may use (PcBase_SetInventorySlotBitmask, CharacterBase +0x341c): bit n for slot n.
    std::uint32_t inventory_slot_mask = 0xffffffffu;
    std::vector<std::uint8_t> languages;
    std::uint16_t zone_id = 0, zone_instance = 0;
    float x = 0, y = 0, z = 0, heading = 0;
    std::uint8_t pvp = 0, gm = 0;
    std::uint32_t guild_id = 0;
    std::uint8_t guild_rank = 0;
    std::uint64_t exp = 0;
    std::uint32_t platinum_bank = 0, gold_bank = 0, silver_bank = 0, copper_bank = 0, platinum_shared = 0;
    std::uint32_t character_id = 0;
    std::uint32_t aa_exp = 0;
    std::uint32_t last_fellowship_join = 0;
    std::uint64_t fellowship_vitality_normal = 0;
    std::uint32_t fellowship_vitality_aa = 0;
    std::array<std::uint8_t, 3> auto_consent{}; // group, raid, guild (PcClient +0x2921..+0x2923)
    std::uint8_t show_helm = 0;
    std::uint32_t rest_timer = 0;
    std::size_t trailing_bytes = 0; // bytes left after the last known field (0 for EQEmu)
};
std::optional<PlayerProfile> decode_player_profile(const AppPacket& p);

// OP_NewZone (948 bytes, NewZone_Struct).
struct NewZone {
    std::string char_name;
    std::string zone_short_name;
    std::string zone_long_name;
    std::uint8_t zone_type = 0xff; // ztype
    std::array<std::uint8_t, 4> fog_red{}, fog_green{}, fog_blue{};
    std::array<float, 4> fog_minclip{}, fog_maxclip{};
    float gravity = 0;
    std::uint8_t time_type = 0; // 0 indoor dungeon, 1 outdoor, 2 outdoor city, ...
    std::array<std::uint8_t, 4> rain_chance{}, rain_duration{}, snow_chance{}, snow_duration{};
    std::uint8_t sky = 0;
    float zone_exp_multiplier = 0;
    float safe_y = 0, safe_x = 0, safe_z = 0;
    float max_z = 0; // +604 (EQEmu writes max_z here; +600 is unused)
    std::uint32_t old_model_scale = 0; // +692 (0x2b4): the client's 0xddfba4 (game/spawn_heights.h)
    float underworld = 0;
    float minclip = 0, maxclip = 0;
    std::string zone_short_name2;
    std::uint16_t zone_id = 0, zone_instance = 0;
    std::uint32_t suspend_buffs = 0;
    std::uint32_t fast_regen_hp = 0, fast_regen_mana = 0, fast_regen_endurance = 0;
    std::uint32_t underworld_teleport_index = 0;
    std::uint32_t lava_damage = 0, min_lava_damage = 0;
    std::uint8_t no_bind = 0, no_attack = 0, fall_damage_off = 0;
    float fog_density = 0;
    std::uint32_t no_levitate = 0;
};
inline constexpr std::size_t kNewZoneSize = 948;
AppPacket encode(const NewZone& v); // server direction, for tests
std::optional<NewZone> decode_new_zone(const AppPacket& p);

// OP_TimeOfDay (8 bytes).
struct TimeOfDay {
    std::uint8_t hour = 0, minute = 0, day = 0, month = 0;
    std::uint32_t year = 0;
};
AppPacket encode(const TimeOfDay& v);
std::optional<TimeOfDay> decode_time_of_day(const AppPacket& p);
// EQWorldData::AdvanceTime @0x7dbff0: the game date `elapsed_s` whole real seconds (Util::FastTime's) after `ref`
// came. Twenty game seconds a real second (one game minute per 3 s), the 32-bit product wrapping as the original's; the whole
// minutes, hours, days, months and years are added to `ref` with its byte arithmetic and carries (hours 1..24, days
// 1..28, months 1..12), and a year past 9999 becomes 0.
TimeOfDay advance_time_of_day(const TimeOfDay& ref, std::uint32_t elapsed_s);

// OP_Weather (12 bytes, or 8: EQEmu's weather changes and #set weather send only the two words the original reads):
// kind (0 rain, 1 snow, 0xff stop), intensity (the server's 1..10, 0 fades out), a third word the original does not
// read. The zone-in one is {0xff, 0x31, 0}.
struct Weather {
    std::int32_t kind = 0xff, intensity = 0;
    std::uint32_t mode = 0;
};
AppPacket encode(const Weather& v);
std::optional<Weather> decode_weather(const AppPacket& p);

// OP_DeleteSpawn: the server's (5 bytes: u32 id, u8 1) and the client's own (4 bytes: its spawn id; MoveToZone
// @0x541f0a and DoMainLoop's camp exit send PlayerClient +0x148), which EQEmu answers by dropping the link.
AppPacket encode_delete_spawn(std::uint32_t spawn_id);
AppPacket encode_delete_own_spawn(std::uint32_t spawn_id);
std::optional<std::uint32_t> decode_delete_spawn(const AppPacket& p);

// OP_SpawnAppearance (8 bytes, both directions). Size changes arrive as
// OP_ChangeSize instead (rof2.cpp ENCODE(OP_SpawnAppearance)).
struct SpawnAppearance {
    std::uint16_t spawn_id = 0; // 0 = me
    std::uint16_t type = 0;
    std::uint32_t parameter = 0;
};
AppPacket encode(const SpawnAppearance& v);
std::optional<SpawnAppearance> decode_spawn_appearance(const AppPacket& p);

// OP_ChangeSize (16 bytes).
struct ChangeSize {
    std::uint32_t spawn_id = 0;
    float size = 0;
    float view_offset = 0; // +8: ChangeHeight's view offset (EQEmu: 0)
    float anim_scale = 1;  // +0xc: ChangeHeight's animation scale (EQEmu: 1.0)
};
std::optional<ChangeSize> decode_change_size(const AppPacket& p);

// OP_HPUpdate (10 bytes): i16 spawn id, u32 current, i32 max.
struct HpUpdate {
    std::int16_t spawn_id = 0;
    std::uint32_t cur_hp = 0;
    std::int32_t max_hp = 0;
};
AppPacket encode(const HpUpdate& v); // server direction, for tests
std::optional<HpUpdate> decode_hp_update(const AppPacket& p);

// OP_MobHealth (3 bytes): u16 spawn id, u8 percent.
struct MobHealth {
    std::uint16_t spawn_id = 0;
    std::uint8_t percent = 0;
};
std::optional<MobHealth> decode_mob_health(const AppPacket& p);

// OP_ChannelMessage (both directions): sender, target, u32 0, language,
// channel, 5 unknown bytes, language skill, message, then 15 zero bytes.
struct ChannelMessage {
    std::string sender;
    std::string target; // tell recipient
    std::uint32_t language = 0;
    std::uint32_t channel = 0; // 0 guild, 2 group, 3 shout, 4 auction, 5 ooc, 7 tell, 8 say, ...
    std::uint32_t language_skill = 0;
    std::string message;
    bool spam = false; // the byte after the message (ChatMessage +0x9c): filter 31 hides it
    std::uint32_t color = 0; // ChatMessage +0xa4 (the dword 5 bytes past the message): channel 20's colour
};
AppPacket encode(const ChannelMessage& v);
// Opcodes 0x150e / 0x1f65 (not in EQEmu's table): the player's name; set / clear the chat block.
inline constexpr std::uint16_t kOpChatBlockOn = 0x150e, kOpChatBlockOff = 0x1f65;
std::optional<ChannelMessage> decode_channel_message(const AppPacket& p);

// OP_SpecialMesg (zone → client).
struct SpecialMessage {
    std::uint8_t speak_mode = 0, journal_mode = 0, language = 0;
    std::uint32_t type = 0; // chat colour (Chat::*)
    std::uint32_t target_spawn_id = 0;
    std::string sayer;
    std::array<std::uint32_t, 3> loc{};
    std::string message;
};
std::optional<SpecialMessage> decode_special_message(const AppPacket& p);

// OP_FormattedMessage (zone → client): eqstr id plus up to 9 arguments.
struct FormattedMessage {
    std::uint32_t unknown0 = 0;
    std::uint32_t string_id = 0;
    std::uint32_t type = 0;
    std::vector<std::string> args;
};
std::optional<FormattedMessage> decode_formatted_message(const AppPacket& p);

// OP_SimpleMessage (zone → client, 12 bytes, SimpleMessage_Struct): an eqstr
// id without arguments, chat colour, unknown (1 = from world). EQEmu's
// MessageString without arguments ("You gain experience!!").
struct SimpleMessage {
    std::uint32_t string_id = 0;
    std::uint32_t color = 0;
    std::uint32_t unknown8 = 0;
};
AppPacket encode(const SimpleMessage& v); // server direction, for tests
std::optional<SimpleMessage> decode_simple_message(const AppPacket& p);

// OP_ZoneChange (100 bytes, both directions). Client → zone with success 0;
// zone → client with success 1 (go: send OP_DeleteSpawn, wait for the drop,
// re-authenticate to world with LoginInfo::zoning) or a negative error code
// (EQEmu subtracts one more from its own negatives).
struct ZoneChange {
    std::string char_name;
    std::uint16_t zone_id = 0;
    std::uint16_t instance_id = 0;
    std::uint32_t unknown068 = 0, unknown072 = 0;
    float y = 0, x = 0, z = 0;
    std::uint32_t zone_reason = 0;
    std::int32_t success = 0;
    std::uint32_t unknown096 = 0;
};
AppPacket encode(const ZoneChange& v);
std::optional<ZoneChange> decode_zone_change(const AppPacket& p);

// OP_RequestClientZoneChange (176 bytes, zone → client): the server asks the
// client to zone (e.g. #zone, gate, death); the client answers with OP_ZoneChange.
struct RequestClientZoneChange {
    std::uint16_t zone_id = 0;
    std::uint16_t instance_id = 0;
    std::uint32_t unknown004 = 0xffffffffu;
    float y = 0, x = 0, z = 0, heading = 0;
    std::uint32_t type = 0;
};
std::optional<RequestClientZoneChange> decode_request_client_zone_change(const AppPacket& p);

// OP_SendZonepoints (zone → client during zone-in): the zone's zone points,
// u32 count then count × 32-byte entries, plus one unused trailing entry
// (EQEmu rof2_structs.h ZonePoint_Entry, rof2.cpp ENCODE(OP_SendZonepoints),
// zone/client.cpp Client::SendZonePoints). Each entry holds a TARGET: where
// zone point `number` (zone_points.number) leads. Server coordinates; a
// coordinate of 999999 keeps the player's own, a heading of 999 keeps the
// player's heading. The original client keeps the table only when
// count < 128 (Handle_OP_SendZonepoints @0x5133e0) and looks entries up by
// number (@0x513420), copying the zone id and instance as one dword.
struct ZonePoint {
    std::uint32_t number = 0;       // "iterator": zone_points.number
    float y = 0, x = 0, z = 0;      // target position, server coordinates
    float heading = 0;              // target heading (0..512, 999 = keep)
    std::uint16_t zone_id = 0;      // target zone
    std::uint16_t instance_id = 0;  // target instance
    std::uint32_t unknown024 = 0;   // the client sends OP 0xcadf (teleport by index) when non-zero; EQEmu sends 0
    std::uint32_t unknown028 = 0;
};
std::optional<std::vector<ZonePoint>> decode_zone_points(const AppPacket& p);

// ---------------------------------------------------------------- combat

// OP_TargetMouse / OP_TargetCommand (4 bytes, ClientTarget_Struct, both
// passed through): u32 target spawn id, 0 = no target. The original sends
// OP_TargetMouse whenever its target changes (CharacterZoneClient::
// DoPassageOfTime @0x45daf0); the server echoes OP_TargetCommand when it
// accepts a /target and sends it for pets.
AppPacket encode_target(bool command, std::uint32_t spawn_id);
std::optional<std::uint32_t> decode_target(const AppPacket& p);

// OP_AutoAttack and OP_AutoAttack2 (u32 each: 1 on, 0 off). The original
// sends both whenever __Attack changes; EQEmu reads byte 0 of OP_AutoAttack
// and ignores OP_AutoAttack2.
AppPacket encode_auto_attack(bool second, bool on);
std::optional<bool> decode_auto_attack(const AppPacket& p);
// OP_AutoFire (u8: 1 on, 0 off): DoPassageOfTime @0x45daf0 sends it whenever __Autofire differs from the last sent
// value. EQEmu then shoots from the range slot on its own timer (Client::Process, RangedAttack / ThrowingAttack).
AppPacket encode_auto_fire(bool on);

// OP_Consider (20 bytes, both directions, rof2_structs.h Consider_Struct).
// Client → zone: player id, target id, the rest 0. Zone → client: faction
// standing 1..9 (already remapped by the server for the eqstr order 12212..),
// level = the server's con colour (2 green, 4 dark blue, 6 gray, 10 white,
// 13 red, 15 yellow, 18 light blue), pvp flag.
struct Consider {
    std::uint32_t player_id = 0;
    std::uint32_t target_id = 0;
    std::uint32_t faction = 0;
    std::uint32_t level = 0;
    std::uint8_t pvp_con = 0;
};
inline constexpr std::size_t kConsiderSize = 20;
AppPacket encode(const Consider& v);
std::optional<Consider> decode_consider(const AppPacket& p);

// OP_Damage (30 bytes, rof2_structs.h CombatDamage_Struct). damage > 0 is a
// hit; 0 a miss; -1 block, -2 parry, -3 riposte, -4 dodge, -5 invulnerable,
// -6 rune (zone/common.h DMG_*). type is the damage type of
// eq_constants.h SkillDamageTypes (0 crush, 1 slash, 4 hand to hand, 7
// archery, 8 backstab, 10 bash, 21/23 strike, 30 kick, 36 pierce, 51 throw,
// 74 frenzy, 231 spell).
struct CombatDamage {
    std::uint16_t target = 0;
    std::uint16_t source = 0;
    std::uint8_t type = 0;
    std::uint32_t spell_id = 0;
    std::int32_t damage = 0;
    float force = 0, hit_heading = 0, hit_pitch = 0;
    std::uint8_t secondary = 0;
    std::uint32_t special = 0; // 1 wild rampage, 2 rampage
};
inline constexpr std::size_t kCombatDamageSize = 30;
AppPacket encode(const CombatDamage& v); // also the client's own hit report (ApplyDamage)
std::optional<CombatDamage> decode_combat_damage(const AppPacket& p);

// OP_Death (32 bytes). rof2.cpp has no ENCODE for it, so the server's own
// Death_Struct goes out: spawn, killer, corpse id, bind zone, spell, attack
// skill (a damage type, SkillDamageTypes; 0xfc/0xfb/0xfa = fell, choked,
// burned as the client reads it), damage, unknown. The original reads
// +16 spell, +20 skill, +24 damage (Handle_OP_Death in HandleWorldMessage).
struct Death {
    std::uint32_t spawn_id = 0;
    std::uint32_t killer_id = 0;
    std::uint32_t corpse_id = 0;
    std::uint32_t bind_zone_id = 0;
    std::uint32_t spell_id = 0xffffffff;
    std::uint32_t attack_skill = 0;
    std::uint32_t damage = 0;
};
inline constexpr std::size_t kDeathSize = 32;
AppPacket encode(const Death& v); // server direction, for tests
std::optional<Death> decode_death(const AppPacket& p);

// OP_ManaChange (20 bytes zone → client, ManaChange_Struct): new mana,
// stamina (endurance), spell id, keep casting, slot. The client sends an
// empty OP_ManaChange to interrupt a cast (not used yet).
struct ManaChange {
    std::uint32_t mana = 0;
    std::uint32_t endurance = 0;
    std::uint32_t spell_id = 0;
    std::uint8_t keep_casting = 0;
    std::int32_t slot = -1;
};
AppPacket encode(const ManaChange& v); // server direction, for tests
std::optional<ManaChange> decode_mana_change(const AppPacket& p);

// OP_ManaUpdate / OP_EnduranceUpdate (10 bytes, ManaUpdate_Struct /
// EnduranceUpdate_Struct): u32 current, u32 maximum, u16 spawn id. Sent to
// the player for itself and to those who have it on extended target.
struct PointsUpdate {
    std::uint32_t current = 0;
    std::uint32_t maximum = 0;
    std::uint16_t spawn_id = 0;
};
AppPacket encode_points_update(bool endurance, const PointsUpdate& v); // server direction, for tests
std::optional<PointsUpdate> decode_points_update(const AppPacket& p);

// OP_ExpUpdate (8 bytes): experience into the level on the 0..330 gauge
// scale, then a tip flag (client: Handle_OP_ExpUpdate @0x4b7060). The second
// dword is not AA experience: the original only compares it with 2 to show
// the experience tip, and AA experience comes in OP_AAExpUpdate alone.
struct ExpUpdate {
    std::uint32_t exp = 0;
    std::uint32_t tip = 0;
};
AppPacket encode(const ExpUpdate& v); // server direction, for tests
std::optional<ExpUpdate> decode_exp_update(const AppPacket& p);

// OP_LevelUpdate (12 bytes): new level, old level, experience.
struct LevelUpdate {
    std::uint32_t level = 0;
    std::uint32_t old_level = 0;
    std::uint32_t exp = 0;
};
AppPacket encode(const LevelUpdate& v); // server direction, for tests
std::optional<LevelUpdate> decode_level_update(const AppPacket& p);

// OP_Animation (4 bytes, rof2_structs.h Animation_Struct): u16 spawn id,
// u8 action (1.. = the clip list C01..C11, D01..D05, L01..L09, O01,
// S01..S05, P01..P07, T01..T09, S06..S28, P08, O02, O03), u8 speed.
// The server's own struct has speed before action; rof2.cpp swaps them.
struct Animation {
    std::uint16_t spawn_id = 0;
    std::uint8_t action = 0;
    std::uint8_t speed = 0;
};
AppPacket encode(const Animation& v);
std::optional<Animation> decode_animation(const AppPacket& p);

// OP_ZonePlayerToBind (zone → client, ENCODE(OP_ZonePlayerToBind)): u16 bind
// zone (0 when the bind is in this zone: the server then expects an
// OP_ZoneChange with zone 0 and re-zones the player through world), u16
// instance, f32 x, y, z, heading, cstr zone name, u8 save items, u32 hp,
// mana, endurance. Sent after the player dies (Client::GoToDeath).
struct ZonePlayerToBind {
    std::uint16_t zone_id = 0;
    std::uint16_t instance_id = 0;
    float x = 0, y = 0, z = 0, heading = 0;
    std::string zone_name;
};
AppPacket encode(const ZonePlayerToBind& v); // server direction, for tests
std::optional<ZonePlayerToBind> decode_zone_player_to_bind(const AppPacket& p);

// OP_RespawnWindow, zone → client (with Character:RespawnFromHover):
// u32 default selection, u32 time remaining (ms), u32 the Select delay (ms; EQEmu sends 0), u32 count, then per
// option u32 id, u32 zone, f32 x, y, z, heading, cstr name, u8 validity (0 available,
// 1 unavailable). The flag does not identify resurrection choices. EQEmu pads count + 1 zero bytes (OpenEQ
// DEATH_RECOVERY_PLAN.md, zone/client.cpp SendRespawnBinds). Client → zone:
// u32 chosen option id (Handle_OP_RespawnWindow).
struct RespawnOption {
    std::uint32_t id = 0;
    std::uint32_t zone_id = 0;
    float x = 0, y = 0, z = 0, heading = 0;
    std::string name;
    std::uint8_t validity = 0;
    bool available() const { return validity == 0; }
    bool operator==(const RespawnOption&) const = default;
};
struct RespawnWindow {
    std::uint32_t selected = 0;
    std::uint32_t time_remaining_ms = 0;
    std::uint32_t delay_ms = 0; // +8: Select waits until the corpse is this old (CRespawnWnd)
    std::vector<RespawnOption> options;
};
std::optional<RespawnWindow> decode_respawn_window(const AppPacket& p);
AppPacket encode_respawn_selection(std::uint32_t option_id);

// SpawnAppearance types used so far (common/eq_constants.h AppearanceType).
inline constexpr std::uint16_t kAppearanceAnimation = 14; // parameter: kStand*
inline constexpr std::uint16_t kAppearancePet = 25;       // parameter: the owner's spawn id (0 = none)
inline constexpr std::uint32_t kStandStanding = 100, kStandFreeze = 102, kStandLooting = 105, kStandSitting = 110,
                               kStandCrouching = 111, kStandLying = 115;

// OP_WearChange, both directions (27 bytes, WearChange_Struct): one visible
// slot's armour material or held item (IT number) and its tint 0xAARRGGBB.
struct WearChange {
    std::uint16_t spawn_id = 0;
    EquipTexture armor;
    std::uint32_t tint = 0;
    std::uint8_t slot = 0;
};
std::optional<WearChange> decode_wear_change(const AppPacket& p);
AppPacket encode(const WearChange& v);

// OP_Illusion, zone -> client (336 bytes, Illusion_Struct / the client's
// chngForm): the appearance fields this client uses.
struct Illusion {
    std::uint32_t spawn_id = 0;
    std::uint16_t race = 0;
    std::uint8_t gender = 0, texture = 0, helm = 0;
    std::uint32_t face = 0;
    std::uint8_t hair_style = 0, hair_color = 0, beard = 0, beard_color = 0;
    float size = 0;
    std::uint32_t drakkin_heritage = 0, drakkin_tattoo = 0, drakkin_details = 0;
};
std::optional<Illusion> decode_illusion(const AppPacket& p);

// TODO: OP_SpawnDoor, OP_GroundSpawn, OP_Action, OP_BeginCast. Items,
// inventory and loot are in net/item_packets.h.

} // namespace hailstone::net
