// Spell sets: CSpellBookWnd::MemorizeSet / ContinueSetMem / OnProcessFrame's set step / StartSpellMemorization, the
// spell gem menu's set items (CCastSpellWnd::WndNotification: SaveLoadout, memorize, delete) and /memspellset.
#include "net/game_connection.h"

#include <string>

#include "net/spell_packets.h"

namespace hailstone::net {

namespace {

std::int32_t as_id(std::uint32_t spell) { return spell == game::kNoSpell ? -1 : static_cast<std::int32_t>(spell); }

} // namespace

bool GameConnection::memorize_set(const std::array<std::int32_t, game::kLoadoutGems>& spells) {
    if (state_ != ConnectionState::InZone) return false;
    set_mem_.at = -1;
    // CCursorAttachment::RemoveAttachment(1): something held stays held, and the set is not memorized.
    if (inventory_.cursor()) {
        spell_message(3345, {}, 0xf);
        return false;
    }
    // The book must be shown. AboutToShow seats a player on foot and leaves a rider mounted.
    set_mem_.keep_open = spell_book_open_;
    if (!spell_book_open_ && !open_spell_book()) return false;
    std::array<std::uint8_t, 12> flags{};
    set_mem_.mode = 2;
    auto gem_spell = [&](int i) { return as_id(spells_.gems[static_cast<std::size_t>(i)].spell); };
    auto& pending = set_mem_.spells;
    const int count = game::kLoadoutGems;
    for (int i = 0; i < count; ++i) {
        if (gem_spell(i) == spells[static_cast<std::size_t>(i)]) {
            pending[static_cast<std::size_t>(i)] = -1;
            continue;
        }
        if (spells[static_cast<std::size_t>(i)] != -1) flags[static_cast<std::size_t>(i)] = 1;
        pending[static_cast<std::size_t>(i)] = spells[static_cast<std::size_t>(i)];
        for (int j = 0; j < i; ++j) { // a spell wanted twice keeps the later gem
            if (pending[static_cast<std::size_t>(j)] == pending[static_cast<std::size_t>(i)]) {
                pending[static_cast<std::size_t>(j)] = -1;
                flags[static_cast<std::size_t>(j)] = 0;
            }
        }
    }
    set_mem_.count = count;
    // A gem holding a spell the set wants on another gem is forgotten too.
    for (int j = 0; j < count; ++j)
        for (int i = 0; i < 12; ++i)
            if (j != i && gem_spell(i) == pending[static_cast<std::size_t>(j)] && !flags[static_cast<std::size_t>(i)])
                flags[static_cast<std::size_t>(i)] = 1;
    std::vector<int> forget;
    for (int i = 0; i < 12; ++i)
        if (flags[static_cast<std::size_t>(i)]) forget.push_back(i);
    send_zone(encode_load_spell_set(forget));
    continue_set_mem();
    return true;
}

void GameConnection::memorize_from_cursor(std::uint32_t spell, int gem) {
    if (state_ != ConnectionState::InZone) return;
    // CCursorAttachment::RemoveAttachment(1): something held stays held
    if (inventory_.cursor()) {
        spell_message(3345, {}, 0xf);
        return;
    }
    int slot = -1; // 0x75bc00
    for (int i = 0; i < game::kSpellBookSize; ++i)
        if (spells_.book[static_cast<std::size_t>(i)] == spell) {
            slot = i;
            break;
        }
    if (!can_start_memming(slot)) return;
    set_mem_.keep_open = spell_book_open_; // +0x258
    if (!spell_book_open_) open_spell_book(); // vtable +0x90 (Activate)
    if (!spell_book_open_) return;
    set_mem_.mode = 1;
    set_mem_.book_index = slot;
    set_mem_.target_gem = gem;
    set_mem_.next_time = eq_time() + 500;
}

void GameConnection::continue_set_mem() {
    ++set_mem_.at;
    while (set_mem_.at < set_mem_.count) {
        std::int32_t id = set_mem_.spells[static_cast<std::size_t>(set_mem_.at)];
        if (id != -1 && spell_table_->get(static_cast<std::uint32_t>(id))) {
            int index = -1; // SpellBook_FindSpellIndex
            for (int i = 0; i < game::kSpellBookSize; ++i)
                if (as_id(spells_.book[static_cast<std::size_t>(i)]) == id) {
                    index = i;
                    break;
                }
            // CharacterZoneClient::CanUseMemorizedSpellSlot (gems 8.. need SPA 326; update_usable_gems)
            if (index != -1 && set_mem_.at < usable_gems_) {
                set_mem_.book_index = index;
                set_mem_.target_gem = set_mem_.at;
                set_mem_.next_time = eq_time() + 1000;
                return;
            }
        }
        ++set_mem_.at;
    }
    set_mem_.count = 0;
    if (!set_mem_.keep_open) close_spell_book();
    set_mem_.mode = 0;
}

void GameConnection::set_mem_frame() {
    // OnProcessFrame @0x75ce2b: a scheduled gem starts once its time has passed and nothing is pending.
    if (set_mem_.target_gem == -1) return;
    if (eq_time() <= set_mem_.next_time || spells_.pending > 0) return;
    if (can_start_memming(set_mem_.book_index)) {
        start_spell_memorization(set_mem_.book_index, set_mem_.target_gem, false);
        set_mem_.target_gem = -1;
        return;
    }
    set_mem_.target_gem = -1;
    if (set_mem_.mode == 2) continue_set_mem();
}

bool GameConnection::start_spell_memorization(int book_index, int gem, bool replace) {
    if (book_index < 0 || book_index >= game::kSpellBookSize || gem < 0 || gem >= game::kSpellGems) return false;
    if (spells_.pending > 0 || spells_.memorizing || spells_.scribing) return false;
    const std::uint32_t id = spells_.book[static_cast<std::size_t>(book_index)];
    const std::uint32_t current = spells_.gems[static_cast<std::size_t>(gem)].spell;
    if (replace) {
        if (current == id) return false;
    } else if (current != game::kNoSpell && spell_table_->get(current)) {
        return false;
    }
    for (int g = 0; g < game::kSpellGems; ++g) {
        if (spells_.gems[static_cast<std::size_t>(g)].spell == id) {
            spell_message(12052); // (0x2f14) already memorized
            return false;
        }
    }
    const game::SpellData* spell = spell_table_->get(id);
    spell_message(12053, {spell ? spell->name : std::string()}); // Beginning to memorize %1...
    game::Memorizing m;
    m.gem = gem;
    m.book_slot = book_index;
    m.spell = id;
    m.ticks_left = game::kMemorizeTicks;
    spells_.memorizing = m;
    return true;
}

bool GameConnection::memorize_loadout(int n) {
    if (n < 0 || n >= game::kSpellLoadouts) return false;
    const game::SpellLoadout& s = spell_loadouts_.at(n);
    if (!s.in_use || !memorize_set(s.spells)) return false;
    spell_message(8014, {s.name}, 0xf); // (0x1f4e)
    return true;
}

void GameConnection::save_loadout(std::string_view name) {
    std::array<std::int32_t, game::kLoadoutGems> gems{};
    for (int k = 0; k < game::kLoadoutGems; ++k) gems[static_cast<std::size_t>(k)] = as_id(spells_.gems[static_cast<std::size_t>(k)].spell);
    int at = -1;
    if (spell_loadouts_.save_set(name, gems, &at) == game::SpellLoadouts::SaveResult::Full) {
        spell_message(3235, {"10"}, 0xd); // (0xca3)
        return;
    }
    spell_message(3236, {spell_loadouts_.at(at < 0 ? 0 : at).name}, 0xf); // (0xca4)
}

void GameConnection::delete_loadout(int n) { spell_loadouts_.remove(n); }

void GameConnection::memspellset_command(std::string_view args) {
    // (The profile's +0x340c, which stops it as it stops the other gem commands, is not kept here.)
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    while (!args.empty() && args.back() == ' ') args.remove_suffix(1);
    int n = spell_loadouts_.find(args);
    if (n < 0) {
        spell_message(3342, {}, 0x108); // (0xd0e) the usage
        return;
    }
    memorize_set(spell_loadouts_.at(n).spells);
}

} // namespace hailstone::net
