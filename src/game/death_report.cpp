#include "game/death_report.h"

namespace hailstone::game {

DeathResult report_death(const DeathInput& in) {
    using F = DeathCall::Fn;
    DeathResult r;
    if (!in.dead_known) return r; // NullDebugPrintf only
    r.handled = true;
    auto call = [&](F f) { r.calls.push_back({f}); };
    auto built = [&](std::uint32_t id, std::string p1, std::string p2, std::uint32_t color) {
        r.chat.push_back({id, true, std::move(p1), std::move(p2), color});
    };
    if (in.dead_is_duel_opponent) {
        if (in.killer_id == in.local_id && in.dueling) {
            built(12100, in.local_name, in.dead_name, kDuelChatColor);
            call(F::SendDuelDecline);
        } else {
            built(12101, in.dead_name, in.dead_name, kDuelChatColor);
        }
        r.duel_ended = true;
    }
    if (in.dead_is_trade_partner) call(F::TradeCancelAndClose);
    if (in.dead_is_merchant) r.merchant_window_closed = true;
    if (in.dead_is_banker) call(F::BankWindowClose);
    if (in.dead_is_local) call(F::LootResetSession);
    if (in.dead_is_trainer) r.trainer_window_closed = true;
    if (in.tracking_on && in.dead_is_tracked) {
        call(F::SendTrackTarget);
        r.chat.push_back({12102, false, {}, {}, kTrackingChatColor});
    }
    if (in.dead_is_target) {
        r.target_cleared = true;
        call(F::SetAutoAttackOff);
        call(F::SetAutoRangeAttackOff);
    }
    if (in.in_group) {
        call(F::GroupGetMember);
        if (in.group_member) r.dead_hp_minus_11 = true;
    }
    call(F::TrackingRemove);
    // Reported unless neither side is us and it died more than report_range away (RoF2: 50; NaN reports).
    bool report = true;
    if (!in.killer_is_local && in.dead_id != in.local_id && in.distance > in.report_range) report = false;
    r.hit = true;
    r.hit_target = static_cast<std::uint16_t>(in.dead_id);
    r.hit_source = static_cast<std::uint16_t>(in.killer_id);
    r.hit_type = in.skill;
    r.hit_spell = in.spell;
    r.hit_damage = in.damage;
    r.hit_report = report;
    DeathChat message;
    bool have_message = true;
    auto plain = [&](std::uint32_t id) { message = {id, false, {}, {}, 0}; };
    auto text = [&](std::uint32_t id, std::string p1, std::string p2) { message = {id, true, std::move(p1), std::move(p2), 0}; };
    if (in.dead_is_local) {
        r.local_died = true;
        if (in.skill == 0xfa) plain(12105);
        else if (in.skill == 0xfb) plain(12104);
        else if (in.skill == 0xfc) plain(12103);
        else if (in.killer_id == 0) plain(12106);
        else if (in.killer_known) text(12107, in.killer_shown, {});
        else text(12108, {}, {});
    } else {
        if (in.skill == 0xfa) text(12111, in.dead_shown, {});
        else if (in.skill == 0xfb) text(12110, in.dead_shown, {});
        else if (in.skill == 0xfc) text(12109, in.dead_shown, {});
        else if (in.killer_id == 0) text(12112, in.dead_shown, {});
        else if (in.killer_id == in.local_id) text(12113, in.dead_shown, {});
        else if (in.killer_known) {
            if (in.killer_id == in.dead_id && in.dead_type == 1) {
                report = false;
                have_message = false;
            } else {
                text(12114, in.dead_shown, in.killer_shown);
            }
        } else {
            text(12115, in.dead_shown, {});
        }
    }
    call(F::SpawnDied);
    if (in.dead_class == 62) return r;
    if (in.dead_is_local) {
        message.color = kOwnDeathColor;
        r.chat.push_back(message);
        r.screen_text = true;
    } else if (report && have_message) {
        message.color = kOtherDeathColor;
        r.chat.push_back(message);
    }
    return r;
}

} // namespace hailstone::game
