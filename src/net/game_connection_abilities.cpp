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
        // HandleWorldMessage @0x4c7623 calls @0x5319e0: Send_OP_Hide @0x52b9e0 (only while the local hide flag is
        // set: the flag clears, OP_Hide {0} goes out, UpdateMyVisibleStatus @0x459510 sends SpawnAppearance {me, 3,
        // level} when the level changed, eqstr 12337), then @0x52bae0 (only while sneaking: the flag clears,
        // eqstr 12338, SpawnAppearance {me, 15, 0}).
        // Hailstone change (a fixed RoF2 gap): the replies go out even when the local flags are clear. Stock EQEmu
        // sends OP_CancelSneakHide only while it holds the player hidden (Client::CancelSneakHide), its Hide roll
        // is independent of the client's, and it clears its hidden flag and tells observers only on the client's
        // SpawnAppearance 3 with 0. Answering on the local flag alone left observers' hidden marks in place.
        const bool was_hidden = hidden_, was_sneaking = sneaking_;
        hidden_ = false;
        sneaking_ = false;
        const auto me = static_cast<std::uint16_t>(world_.player_id);
        // Regular invisibility that outlasts Hide: a known spell, or a level the server set for the player itself
        // (SpawnAppearance 3 to the player; a successful Hide's goes to the others only), e.g. a worn item's.
        // SpawnAppearance 3 with 0 would make EQEmu zero that invisibility (Handle_OP_SpawnAppearance).
        const bool still_invisible = invis_state_ > 0 || own_effect_with(game::SPA_INVISIBILITY) >= 0 ||
                                     own_effect_with(game::SPA_IMPROVED_INVIS) >= 0;
        send_zone(encode_hide_cancel());
        if (me != 0 && !still_invisible) send_zone(encode(SpawnAppearance{me, kAppearanceInvisibility, 0}));
        if (was_hidden) system(12337);
        if (was_sneaking) system(12338);
        if (me != 0) send_zone(encode(SpawnAppearance{me, kAppearanceSneak, 0}));
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
