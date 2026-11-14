// GameConnection: LDoN adventures over EQEmu's packets (net/adventure_packets.h). The windows are
// app/lua_eq_adventure.cpp's; the world server chooses the adventure, checks the group and answers with the offer
// (OP_AdventureDetails) or a refusal (OP_AdventureInfo).

#include <algorithm>
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

bool GameConnection::adventure_recruiter_available() const {
    const auto it = world_.spawns.find(adventure_.recruiter);
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || !me || !it->second.is_npc ||
        it->second.is_corpse || it->second.class_id != kClassAdventureRecruiter)
        return false;
    const glm::vec3 at = motion_known_ ? position_ : me->position;
    return glm::distance(at, it->second.position) <= kRecruiterRange;
}

bool GameConnection::open_adventure_recruiter(std::uint32_t npc) {
    auto it = world_.spawns.find(npc);
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || !me || !it->second.is_npc ||
        it->second.is_corpse || it->second.class_id != kClassAdventureRecruiter)
        return false;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (!(glm::distance(at, it->second.position) <= kRecruiterRange)) return false;
    if (adventure_.recruiter != npc) adventure_answer(false);
    adventure_.recruiter = npc;
    adventure_.window_open = true;
    adventure_.npc_text.clear();
    recruiter_state_ = 0; // Send_OP_AdventureInfoRequest @0x420030: AdventureMgr_SetState(0) first
    send_zone(encode_adventure_info_request(npc));
    return true;
}

void GameConnection::set_adventure_window_open(bool open) {
    adventure_.window_open = open;
    if (!open) recruiter_state_ = 0;
    if (!open && !adventure_.data.assigned) {
        // CAdventureRequestWnd::Deactivate @0x618420 cancels the pending offer and recruiter interaction.
        adventure_answer(false);
        adventure_.recruiter = 0;
        adventure_.npc_text.clear();
    }
}

void GameConnection::close_adventure_recruiter() { set_adventure_window_open(false); }

bool GameConnection::adventure_request_ready() const {
    return adventure_recruiter_available() && !adventure_.data.assigned && adventure_.offer.empty() &&
           session_now_ms() >= adventure_.request_next_ms;
}

void GameConnection::adventure_request(std::uint32_t risk, std::uint32_t type) {
    if (!adventure_request_ready() || risk < 1 || risk > 2 || type > 4) return;
    if (!group_.active && !raid_.active && !server_ext().solo_adventure(*this)) {
        // EQEmu drops a request from a character in no group or raid without an answer; a server extension's
        // solo_adventure switch lets it go out (game/server_ext.h).
        adventure_.npc_text = "You must be in a group or raid to request an adventure."; // this client's line
        return;
    }
    adventure_.request_next_ms = session_now_ms() + 5000; // EQEmu's pTimerStartAdventureTimer
    adventure_.npc_text.clear();
    send_zone(encode_adventure_request(risk, adventure_.recruiter, type));
}

void GameConnection::adventure_answer(bool accept) {
    if (state_ != ConnectionState::InZone || adventure_.offer.empty()) return;
    if (accept && (adventure_.data.assigned || !adventure_recruiter_available())) return;
    adventure_.offer.clear();
    recruiter_state_ = 0; // Send_OP_LDoNButton (0x41fed0 / 0x41ff80 / 0x41fe10): AdventureMgr_SetState(0)
    send_zone(encode_ldon_button(accept));
}

void GameConnection::adventure_leave() {
    if (state_ == ConnectionState::InZone && adventure_.data.assigned) send_zone(make_empty(Op::LeaveAdventure));
}

void GameConnection::set_adventure_stats_open(bool open) {
    adventure_.stats_open = open;
    if (open) adventure_stats_request();
}

void GameConnection::set_adventure_leaderboard_open(bool open) {
    const bool was_open = adventure_.leaderboard_open;
    adventure_.leaderboard_open = open;
    if (open && !was_open) { // CAdventureLeaderboardWnd::Activate @0x617510 refreshes on hidden -> shown.
        const auto& s = adventure_.leaderboard_selection;
        adventure_leaderboard_request(s.order, s.theme, s.risk);
    }
}

bool GameConnection::adventure_stats_ready() const {
    return state_ == ConnectionState::InZone && session_now_ms() >= adventure_.stats_next_ms;
}

bool GameConnection::adventure_leaderboard_ready() const {
    return state_ == ConnectionState::InZone && !adventure_.leaderboard_inflight &&
           !adventure_.leaderboard_queued && session_now_ms() >= adventure_.leaderboard_next_ms;
}

std::int64_t GameConnection::adventure_elapsed_seconds() const {
    return std::max<std::int64_t>(0, session_now_ms() - adventure_.data_at) / 1000;
}

void GameConnection::adventure_stats_request() {
    // CAdventureStatsWnd::SendRequest @0x619570 gates requests for 10 s (EQEmu's own guard is 8 s).
    if (!adventure_stats_ready()) return;
    adventure_.stats_queued = false;
    adventure_.stats_next_ms = session_now_ms() + 10000;
    send_zone(encode_adventure_stats_request());
}

void GameConnection::adventure_leaderboard_request(std::uint32_t order, std::uint32_t theme, std::uint32_t risk) {
    if (state_ != ConnectionState::InZone || order > 1 || theme >= kAdventureThemes || risk > 2) return;
    const AdventureLeaderboardSelection selection{order, theme, risk};
    if (selection != adventure_.leaderboard_selection) adventure_.leaderboard.reset();
    adventure_.leaderboard_selection = selection;
    adventure_.leaderboard_failed = false;
    if (adventure_.leaderboard_inflight == selection) {
        adventure_.leaderboard_queued = false;
        return;
    }
    adventure_.leaderboard_queued = true;
    adventure_frame();
}

void GameConnection::adventure_frame() {
    if (state_ != ConnectionState::InZone) return;
    auto& a = adventure_;
    // Hailstone recovery: a missing or malformed reply must not lock Refresh forever. The wire has no request
    // id, so a reply delayed beyond this deadline cannot be distinguished from a later request's reply.
    if (a.leaderboard_inflight && session_now_ms() >= a.leaderboard_deadline_ms) {
        a.leaderboard_inflight.reset();
        a.leaderboard_failed = true;
    }
    if (a.stats_queued && adventure_stats_ready()) adventure_stats_request();
    // CAdventureLeaderboardWnd::SendRequest @0x6173e0 gates requests for 10 s; EQEmu also drops requests
    // inside its 4 s guard. EQEmu replies carry no category, so serialize them and keep the latest selection.
    if (!a.leaderboard_queued || a.leaderboard_inflight || session_now_ms() < a.leaderboard_next_ms) return;
    a.leaderboard_inflight = a.leaderboard_selection;
    a.leaderboard_queued = false;
    a.leaderboard_failed = false;
    a.leaderboard_next_ms = session_now_ms() + 10000;
    a.leaderboard_deadline_ms = session_now_ms() + 30000;
    const auto& s = *a.leaderboard_inflight;
    send_zone(encode_adventure_leaderboard_request(s.order, s.theme, s.risk));
}

void GameConnection::reset_adventure_for_zone() {
    auto& a = adventure_;
    recruiter_state_ = 0;
    a.recruiter = 0;
    a.npc_text.clear();
    a.offer.clear();
    a.data = {};
    a.count = {};
    a.data_at = 0;
    a.request_next_ms = a.stats_next_ms = a.leaderboard_next_ms = a.leaderboard_deadline_ms = 0;
    a.leaderboard_inflight.reset();
    a.leaderboard_failed = false;
    a.stats_queued = a.stats_open;
    a.leaderboard_queued = a.leaderboard_open;
    if (a.leaderboard_open) a.leaderboard.reset();
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
        if (auto v = decode_adventure_text(p)) {
            if (a.recruiter && !a.data.assigned) {
                a.offer = std::move(*v);
                a.window_open = true;
                set_recruiter_state(2); // AdventureMgr_HandleAdventureDetails @0x4201b0 starts the range clock.
            } else {
                // The window closed or the player zoned while the request was being answered.
                send_zone(encode_ldon_button(false));
                recruiter_state_ = 0;
            }
        }
        break;
    case Op::AdventureData:
        if (auto v = decode_adventure_data(p)) {
            if (!v->assigned || v->text != a.data.text) a.count = {};
            a.data = std::move(*v);
            a.data_at = session_now_ms();
            a.offer.clear();
            if (a.data.assigned) recruiter_state_ = 0; // AdventureMgr_HandleAdventureData @0x420320: states 3 / 4.
        }
        break;
    case Op::AdventureUpdate:
        if (auto v = decode_adventure_count(p); v && a.data.assigned && !a.data.time_to_enter) a.count = *v;
        break;
    case Op::AdventureFinish:
        if (auto v = decode_adventure_finish(p)) {
            // RoF2's OP_AdventureFinish branch @0x4206eb resets the assignment and hides the request window.
            // Stock EQEmu does not follow it with an empty OP_AdventureData.
            a.data = {};
            a.count = {};
            a.data_at = 0;
            a.offer.clear();
            a.npc_text.clear();
            a.recruiter = 0;
            recruiter_state_ = 0;
            a.window_open = false;
            // EQEmu sends no chat line with it; this client's lines.
            ui_message(v->won ? std::format("You have completed the adventure and earned {} points.", v->points)
                              : std::string("You have failed the adventure."));
        }
        break;
    case Op::AdventurePointsUpdate:
        if (auto v = decode_adventure_points(p)) {
            a.points = *v;
            // RoF2's points handler @0x4208f5 refreshes the stats as well. Preserve this refresh when an update
            // lands during the ten-second request guard, so completion results do not stay stale.
            a.stats_queued = true;
        }
        break;
    case Op::AdventureStatsReply:
        if (auto v = decode_adventure_stats(p)) a.stats = *v;
        break;
    case Op::AdventureLeaderboardReply:
        if (auto v = decode_adventure_leaderboard(p); v && a.leaderboard_inflight) {
            if (*a.leaderboard_inflight == a.leaderboard_selection) a.leaderboard = std::move(*v);
            a.leaderboard_inflight.reset();
            a.leaderboard_failed = false;
            adventure_frame();
        }
        break;
    default:
        break;
    }
}

} // namespace hailstone::net
