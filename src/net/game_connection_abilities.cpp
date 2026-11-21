// GameConnection: the packets the skills and abilities code (app/actions_ui.cpp) reads.
#include "net/game_connection.h"

#include "game/merchant.h"
#include "game/spa_ids.h"
#include "net/ability_packets.h"
#include "net/notice_packets.h"

namespace hailstone::net {

void GameConnection::handle_ability(Op op, const AppPacket& p) {
    switch (op) {
    case Op::SkillUpdate:
        // Handle_OP_SkillUpdate @0x4be7c0 (net/game_connection_progress.cpp).
        handle_skill_update(p);
        break;
    case Op::Track:
        // HandleWorldMessage @0x4c623c: CTrackingWnd::SetTrackingList (game/tracking.h).
        handle_track_reply(p);
        break;
    case Op::Begging:
    case Op::PickPocket:
        server_coin(op, p);
        break;
    case Op::CancelSneakHide: {
        hidden_ = false;
        sneaking_ = false;
        const auto me = static_cast<std::uint16_t>(world_.player_id);
        // A cancellation can arrive while a regular invisibility spell still applies.
        const bool spell_invisible = own_effect_with(game::SPA_INVISIBILITY) >= 0 ||
                                     own_effect_with(game::SPA_IMPROVED_INVIS) >= 0;
        if (!spell_invisible) {
            if (invis_state_ > 0) invis_state_ = 0;
            if (auto player = world_.spawns.find(world_.player_id);
                player != world_.spawns.end() && player->second.invis_rank > 0)
                player->second.invis_rank = 0;
        }
        send_zone(encode_hide_cancel());
        if (me != 0) {
            // EQEmu relays the visibility update to observers. The server's Hide roll can
            // succeed independently of our local Hide flag.
            if (!spell_invisible) send_zone(encode(SpawnAppearance{me, kAppearanceInvisibility, 0}));
            send_zone(encode(SpawnAppearance{me, kAppearanceSneak, 0}));
        }
        break;
    }
    case Op::SpawnAppearance:
    case Op::Taunt:
    case Op::FeignDeath:
    case Op::Bind_Wound:
        break;
    default:
        return;
    }
    // Nobody may be reading (tools): keep only the latest few.
    if (ability_packets_.size() >= 64) ability_packets_.pop_front();
    ability_packets_.push_back(p);
}

void GameConnection::server_coin(Op op, const AppPacket& p) {
    // Coin the server adds without a coin update (AddMoneyToPP, no client update): begging (3 silver, 4 copper:
    // the amount in that coin) and pick pockets (1..4 platinum .. copper; a stolen item comes as an item packet).
    auto f = op == Op::Begging ? decode_begging(p) : decode_pick_pocket(p);
    if (!f || f->amount == 0) return;
    if (op == Op::Begging) {
        if (f->kind == 4) game::server_add_copper(coins_, f->amount);
        else if (f->kind == 3) game::server_add_copper(coins_, static_cast<std::uint64_t>(f->amount) * 10);
    } else if (f->kind == 1) {
        coins_.platinum += f->amount;
    } else if (f->kind == 2) {
        coins_.gold += f->amount;
    } else if (f->kind == 3) {
        coins_.silver += f->amount;
    } else if (f->kind == 4) {
        coins_.copper += f->amount;
    }
}

std::optional<AppPacket> GameConnection::poll_ability_packet() {
    if (ability_packets_.empty()) return std::nullopt;
    AppPacket p = std::move(ability_packets_.front());
    ability_packets_.pop_front();
    return p;
}

} // namespace hailstone::net
