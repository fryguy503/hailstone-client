#pragma once

// The packets CharacterZoneClient::UseSkill @0x45b2d0 and SendCombatAbilityAttack @0x4fb3d0 send, byte for byte as
// the original builds them in its send buffer (0xdd00e0: the opcode, then the fields), checked
// against EQEmu's structs (common/eq_packet_structs.h, patches/rof2.cpp) and its handlers (zone/client_packet.cpp).
// docs/protocol/rof2-packets.md "Skills and abilities".

#include <cstdint>
#include <optional>

#include "game/mechanics/abilities.h"
#include "net/packet.h"

namespace hailstone::net {

// OP_CombatAbility (12 bytes, CombatAbility_Struct {m_target, m_atk, m_skill}); atk is the attack slot (100 for a
// combat skill, 11 for a ranged attack).
AppPacket encode_combat_ability(std::uint32_t target, std::uint32_t atk, std::uint32_t skill);
// OP_Bind_Wound (8 bytes): the target's spawn id as a u32 (BindWound_Struct to + unknown2), then 0 (type, unknown6).
AppPacket encode_bind_wound(std::uint32_t target);
// OP_Disarm (16 bytes, Disarm_Struct): source, target, the disarm skill value, then 0xff.
AppPacket encode_disarm(std::uint32_t source, std::uint32_t target, std::uint32_t skill_value);
// OP_PickPocket (18 bytes, PickPocket_Struct): to, from, the skill value (u16 myskill, type 0, unknown 0),
// coin = 0xff, 2 zero bytes. OP_Begging has the same 18 bytes.
AppPacket encode_pick_pocket(std::uint32_t target, std::uint32_t me, std::uint32_t skill_value);
AppPacket encode_begging(std::uint32_t target, std::uint32_t me, std::uint32_t skill_value);
// OP_FeignDeath (12 bytes): the spawn id, the global 0xddf708 (unknown; 0), 0.
AppPacket encode_feign_death(std::uint32_t me, std::uint32_t unknown = 0);
// OP_Taunt / OP_RemoveTrap (4 bytes, ClientTarget_Struct): the target's spawn id.
AppPacket encode_taunt(std::uint32_t target);
AppPacket encode_remove_trap(std::uint32_t target);
// OP_Animation (4 bytes, RoF2 Animation_Struct {u16 spawnid, u8 action, u8 speed}).
AppPacket encode_animation(std::uint16_t spawn_id, std::uint8_t action, std::uint8_t speed);
// OP_Hide with 4 zero bytes: Send_OP_Hide @0x52b9e0, the acknowledgement for the server's OP_CancelSneakHide.
AppPacket encode_hide_cancel();

// OP_SkillUpdate (RoF2 SkillUpdate_Struct, 12 bytes: skill id, value, 4 unknown bytes): Handle_OP_SkillUpdate
// @0x4be7c0 stores the value in the profile's skills.
struct SkillUpdate {
    std::uint32_t skill = 0;
    std::uint32_t value = 0;
    std::uint8_t announce = 1; // byte 8: print eqstr 12091 (EQEmu always sends 1)
};
std::optional<SkillUpdate> decode_skill_update(const AppPacket& p);
AppPacket encode_skill_update(const SkillUpdate& v); // server direction, for tests

// The packet for one of UseSkill's sends: the bytes the UseSkill port built (`raw`), else the kind's packet (empty
// packets for hide, sneak, forage, mend, ...).
std::optional<AppPacket> encode_skill_send(const game::mechanics::SkillSend& s);

// OP_Track, zone -> client: the reply's first u32 (none for a packet under 4 bytes). game::kTrackReplyTimer there
// means the reply is the tracking timer, not a list; any other value starts the list (game::set_tracking_list).
std::optional<std::uint32_t> decode_track_reply_header(const AppPacket& p);

} // namespace hailstone::net
