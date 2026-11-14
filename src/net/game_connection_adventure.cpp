// GameConnection: LDoN adventures over EQEmu's packets (net/adventure_packets.h). The windows are
// app/lua_eq_adventure.cpp's; the world server chooses the adventure, checks the group and answers with the offer
// (OP_AdventureDetails) or a refusal (OP_AdventureInfo).

#include <format>

#include "core/log.h"
#include "net/game_connection.h"
#include "net/packet_io.h"

namespace hailstone::net {

namespace {

constexpr std::uint8_t kClassAdventureRecruiter = 60;
// EQEmu checks none. AdventureMgr_Process @0x41fa80 keeps the window within PlayerClient_IsWithinInteractRange(npc,
// 0, 20.0); the right click's own check is not traced.
constexpr float kRecruiterRange = 20.0f;

} // namespace

bool GameConnection::open_adventure_recruiter(std::uint32_t npc) {
    auto it = world_.spawns.find(npc);
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || !me || !it->second.is_npc ||
        it->second.class_id != kClassAdventureRecruiter)
        return false;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, it->second.position) >= kRecruiterRange) return false;
    adventure_.recruiter = npc;
    adventure_.npc_text.clear();
    recruiter_state_ = 0; // Send_OP_AdventureInfoRequest @0x420030: AdventureMgr_SetState(0) first
    send_zone(encode_adventure_info_request(npc));
    return true;
}

void GameConnection::adventure_request(std::uint32_t risk, std::uint32_t type) {
    if (state_ != ConnectionState::InZone || !adventure_.recruiter) return;
    if (!group_.active && !raid_.active && !server_ext().solo_adventure(*this)) {
        // EQEmu drops a request from a character in no group or raid without an answer; a server extension's
        // solo_adventure switch lets it go out (game/server_ext.h).
        adventure_.npc_text = "You must be in a group or raid to request an adventure."; // this client's line
        return;
    }
    send_zone(encode_adventure_request(risk, adventure_.recruiter, type));
}

void GameConnection::adventure_answer(bool accept) {
    if (adventure_.offer.empty()) return;
    adventure_.offer.clear();
    recruiter_state_ = 0; // Send_OP_LDoNButton (0x41fed0 / 0x41ff80 / 0x41fe10): AdventureMgr_SetState(0)
    if (state_ == ConnectionState::InZone) send_zone(encode_ldon_button(accept));
}

void GameConnection::adventure_leave() {
    if (state_ == ConnectionState::InZone && adventure_.data.assigned) send_zone(make_empty(Op::LeaveAdventure));
}

void GameConnection::set_adventure_stats_open(bool open) {
    adventure_.stats_open = open;
    if (open) adventure_stats_request();
}

void GameConnection::set_adventure_leaderboard_open(bool open) {
    adventure_.leaderboard_open = open;
    if (open && !adventure_.leaderboard) adventure_leaderboard_request(0, 0, 1);
}

void GameConnection::adventure_stats_request() {
    // EQEmu answers at most once every 8 s.
    if (state_ == ConnectionState::InZone) send_zone(encode_adventure_stats_request());
}

void GameConnection::adventure_leaderboard_request(std::uint32_t order, std::uint32_t theme, std::uint32_t risk) {
    // EQEmu answers at most once every 4 s.
    if (state_ == ConnectionState::InZone) send_zone(encode_adventure_leaderboard_request(order, theme, risk));
}

void GameConnection::handle_adventure(Op op, const AppPacket& p) {
    auto& a = adventure_;
    switch (op) {
    case Op::AdventureInfo:
        if (auto v = decode_adventure_text(p)) {
            a.npc_text = std::move(*v);
            if (!a.recruiter) ui_message(a.npc_text); // a notice with no window open
            // AdventureMgr_HandleAdventureInfo @0x420130: state 1, its range-check clock started.
            else set_recruiter_state(1);
        }
        break;
    case Op::AdventureDetails:
        // AdventureMgr_HandleAdventureDetails @0x4201b0: state 2, its range-check clock started. The original also
        // shows the window when it was closed; this client's window needs its recruiter, so an offer for a closed
        // window is dropped (Hailstone).
        if (auto v = decode_adventure_text(p); v && a.recruiter) {
            a.offer = std::move(*v);
            set_recruiter_state(2);
        }
        break;
    case Op::AdventureData:
        if (auto v = decode_adventure_data(p)) {
            a.data = std::move(*v);
            a.data_at = now_ms();
            if (!a.data.assigned) a.count = {};
            else recruiter_state_ = 0; // AdventureMgr_HandleAdventureData @0x420320: states 3 / 4, not range-checked
        }
        break;
    case Op::AdventureUpdate:
        if (auto v = decode_adventure_count(p)) a.count = *v;
        break;
    case Op::AdventureFinish:
        if (auto v = decode_adventure_finish(p)) {
            // EQEmu sends no chat line with it; this client's lines.
            ui_message(v->won ? std::format("You have completed the adventure and earned {} points.", v->points)
                              : std::string("You have failed the adventure."));
        }
        break;
    case Op::AdventurePointsUpdate:
        if (auto v = decode_adventure_points(p)) a.points = *v;
        break;
    case Op::AdventureStatsReply:
        if (auto v = decode_adventure_stats(p)) a.stats = *v;
        break;
    case Op::AdventureLeaderboardReply:
        if (auto v = decode_adventure_leaderboard(p)) a.leaderboard = std::move(*v);
        break;
    default:
        break;
    }
}

} // namespace hailstone::net
