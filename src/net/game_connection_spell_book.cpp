// The spell book's open and close rules (game/spell_book.h) on the connection's state.
#include "net/game_connection.h"

#include "core/log.h"
#include "game/spell_book.h"
#include "net/spell_packets.h"

namespace hailstone::net {

bool GameConnection::open_spell_book() {
    if (state_ != ConnectionState::InZone) return false;
    if (spell_book_open_) return true;
    game::SpellBookState s;
    s.spellcaster = is_spellcaster(); // EQ_Character::IsSpellcaster (server_ext() may answer)
    s.profile = profile_.has_value();
    s.controlled_speed = speed_run_;
    s.stand_state = pstatus_.stand_state;
    s.auto_attack = auto_attack_;
    s.auto_fire = auto_fire_;
    std::vector<game::SpellBookCall> calls;
    bool ok = game::spell_book_about_to_show(s, calls);
    for (const auto& c : calls) {
        using K = game::SpellBookCall::Kind;
        switch (c.kind) {
        case K::Chat:
        case K::Chat2: chat(ChatEvent::Kind::System, strings_.format(c.value, {}), 0, c.color); break;
        case K::ChangePosition: set_stand_state(c.value); break;
        case K::SetAutoAttack: set_auto_attack(false); break;
        case K::SetAutoFire: set_auto_fire(false); break;
        default: break; // window focus and the cast window are the UI's
        }
    }
    if (!ok) return false;
    spell_book_open_ = true;
    book_swap_slot_ = -1; // ResetBookState (sub_0075c930)
    book_was_sitting_ = s.was_sitting;
    return true;
}

void GameConnection::spell_book_closed_by_game(bool stand) {
    if (!spell_book_open_) return;
    spell_book_open_ = false;
    // unk_CSpellBookWnd_HideWorker: a memorization in progress is aborted (12045), else a scribe whose request is
    // not yet sent (ScribeBookSlot +0x240, cleared when GetSpellScribeTicksLeft sends it) is aborted (12044).
    if (spells_.memorizing) {
        chat(ChatEvent::Kind::System, strings_.format(12045, {}));
        spells_.memorizing.reset();
    } else if (spells_.scribing && !spells_.scribing->sent) {
        chat(ChatEvent::Kind::System, strings_.format(12044, {}));
        spells_.scribing.reset();
    }
    set_mem_ = {}; // A queued gem must not start when the book next opens.
    // OnShow (hidden): stand up when the book sat the player down.
    game::SpellBookState s;
    s.stand_state = pstatus_.stand_state;
    s.was_sitting = book_was_sitting_;
    std::vector<game::SpellBookCall> calls;
    game::spell_book_on_show(s, false, calls);
    if (!stand) return;
    for (const auto& c : calls)
        if (c.kind == game::SpellBookCall::Kind::ChangePosition) set_stand_state(c.value);
}

void GameConnection::close_spell_book() { spell_book_closed_by_game(true); }

void GameConnection::spell_book_swap_click(int book_slot) {
    // Send_OP_SwapSpell @0x75ceb0 (the slot already has LeftPageNbr * 8 added): only sitting (StandState 'n') or
    // with the controlled spawn's SpeedRun at 0. WndNotification ignores every click while a scribe runs.
    if (spells_.scribing) return;
    if (!sitting() && speed_run_ != 0.0f) return;
    if (book_slot < 0 || book_slot >= game::kSpellBookSize) return;
    if (book_slot == book_swap_slot_) { // the picked slot again: put it back
        book_swap_slot_ = -1;
        return;
    }
    if (book_swap_slot_ == -1) {
        book_swap_slot_ = book_slot;
        system(12046, {}, 15); // Right click on another Scribe Slot in your Spell Book to swap this Spell position ...
        const std::uint32_t id = spells_.book[static_cast<std::size_t>(book_slot)];
        const game::SpellData* s = id != game::kNoSpell && spell_table_ ? spell_table_->get(id) : nullptr;
        if (s && s->deletable) system(4027, {}, 15); // ... can be permanently removed ... by pressing the delete key
        return;
    }
    // The pick stays until the server's echo; "Attempt to send message %d on a void connection." without a zone.
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode(SwapSpell{static_cast<std::uint32_t>(book_swap_slot_), static_cast<std::uint32_t>(book_slot)}));
}

} // namespace hailstone::net
