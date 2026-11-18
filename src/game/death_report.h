#pragma once

// OP_Death 0x6517 as the original handles it: Handle_OP_Death @0x532b50
// (CEverQuest::ReportDeath). Checked against
// the original's vectors (Handle_OP_Death).
//
// The record: +0 dead id, +4 killer id, +0x10 spell, +0x14 skill (byte),
// +0x18 damage.

#include <cstdint>
#include <string>
#include <vector>

namespace hailstone::game {

struct DeathInput {
    bool dead_known = true;        // the dead spawn exists (else nothing at all)
    bool dead_is_local = false;
    std::uint32_t dead_id = 0, killer_id = 0;
    bool killer_known = false;     // GetSpawnByID(killer) found it
    bool killer_is_local = false;  // ... and it is the local player
    std::uint32_t local_id = 0;    // the local player's id (and the character's own spawn, pc +0x2dd0)
    std::int32_t spell = 0;
    std::uint8_t skill = 0;        // 250 burned, 251 drowned, 252 fell
    std::int32_t damage = 0;
    std::uint8_t dead_type = 0;    // +0x125 (1 NPC)
    std::uint8_t dead_class = 0;   // +0xeb8 (62: an LDoN object gets no text)
    std::uint8_t dead_stand_state = 100;
    bool dead_is_duel_opponent = false; // 0xddf7e4 is its id
    bool dueling = false;               // 0xddf7ec
    bool dead_is_trade_partner = false, dead_is_merchant = false, dead_is_banker = false, dead_is_trainer = false;
    bool tracking_on = false, dead_is_tracked = false;
    bool dead_is_target = false;
    bool in_group = false, group_member = false;
    float distance = 0.0f;               // CDisplay::PlayerDistance(local, dead)
    // RoF2's 50; the connection passes hailstone.ini combat.report_range instead when it is set (extension).
    float report_range = 50.0f;
    // Names: +0xa4 (dead's and ours) and the displayed +0xe4 ones.
    std::string dead_name, local_name, dead_shown, killer_shown;
};

struct DeathChat {
    std::uint32_t eqstr = 0;
    bool built = false;       // BuildStringFromEQSTR (else the plain string)
    std::string p1, p2;       // its parameters ("" for none)
    std::uint32_t color = 0;
};

struct DeathCall {
    enum class Fn : std::uint8_t {
        SendDuelDecline,      // OP_DuelDecline {dead id, our id} (when connected)
        TradeCancelAndClose,  // CTradeWnd_CancelAndClose
        BankWindowClose,      // 0x6275f0(0)
        LootResetSession,     // our death
        SendTrackTarget,      // Send_OP_TrackTarget(-1, 0)
        SetAutoAttackOff,
        SetAutoRangeAttackOff,
        GroupGetMember,       // CGroup_GetMemberByPlayer(dead)
        TrackingRemove,       // CTrackingWnd::RemovePlayerFromTracking
        SpawnDied,            // PlayerZoneClient::IDied
    };
    Fn fn;
};

struct DeathResult {
    bool handled = false;
    std::vector<DeathCall> calls;
    std::vector<DeathChat> chat;
    bool screen_text = false;         // CTextOverlay::DisplayText(text, 10, 10, 0xc0, 1000, 1000, 4000)
    // ReportSuccessfulHit({dead id, killer id (u16), skill, spell, damage}, report, 0).
    bool hit = false;
    std::uint16_t hit_target = 0, hit_source = 0;
    std::uint8_t hit_type = 0;
    std::int32_t hit_spell = 0, hit_damage = 0;
    bool hit_report = false;
    bool local_died = false;          // 0x531730: auto attack off, level and max HP noted, tip 31 later
    bool merchant_window_closed = false, trainer_window_closed = false;
    bool duel_ended = false;          // 0xddf7e4 / e8 / ec cleared
    bool target_cleared = false;
    bool dead_hp_minus_11 = false;    // a group member's HP (+0x2e4) = -11
};

inline constexpr std::uint32_t kDuelChatColor = 0x10d, kTrackingChatColor = 0x111;
inline constexpr std::uint32_t kOwnDeathColor = 0x115, kOtherDeathColor = 0x116;

DeathResult report_death(const DeathInput& in);

} // namespace hailstone::game
