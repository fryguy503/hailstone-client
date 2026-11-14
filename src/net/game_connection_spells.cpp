// GameConnection: spells. Casting, memorizing, buffs and spell messages, as
// the original client does them online:
// the client gates a cast, starts its cast bar, locks the gems for 10 s and
// sends OP_CastSpell; the server decides everything else and ends every cast
// with OP_ManaChange, then sends OP_MemorizeSpell(3) with the real recast.

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <optional>

#include "core/log.h"
#include "game/appearance.h"
#include "game/coords.h"
#include "net/game_connection.h"
#include "game/hit_landing.h"
#include "game/mechanics/character.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/spell_gems.h"

#include "game/distance.h"
#include "net/packet_io.h"
#include "net/spell_packets.h"
#include "net/ability_packets.h"
#include "game/race_models.h"

namespace hailstone::net {

namespace {

std::string spell_name(const game::SpellTable& table, std::uint32_t id) {
    const auto* s = table.get(id);
    return s ? s->name : "spell " + std::to_string(id);
}

// How long after its OP_BeginCast's cast time a spell's OP_Action still counts as that cast's landing (the server's
// tick and the latency), and how long a proc's added cast stage covers its other targets' landings.
constexpr std::int64_t kCastLandingSlackMs = 3000;
constexpr std::int64_t kProcLandingMs = 500;

// A bard song's skill (brass, singing, stringed, percussion, wind): its pulses land without a cast of their own.
bool is_song_skill(std::uint8_t skill) {
    return skill == 12 || skill == 41 || skill == 49 || skill == 54 || skill == 70;
}

} // namespace

std::int64_t GameConnection::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::uint8_t GameConnection::visible_class() const {
    if (std::uint8_t c = server_ext().cast_class(*this)) return c;
    return profile_ ? profile_->class_ : 0;
}

std::uint8_t GameConnection::spell_level_needed(const game::SpellData& spell, std::uint32_t class_id) const {
    if (auto v = server_ext().spell_level_needed(*this, spell, class_id)) return *v;
    // EQ_Spell::GetSpellLevelNeeded reads the class's level byte (classes outside 1..16: the warrior byte, as the
    // memorise port reads it).
    return spell.class_level[(class_id >= 1 && class_id <= 16 ? class_id : 1) - 1];
}

std::uint32_t GameConnection::player_class_mask() const {
    std::uint32_t mask = server_class_mask();
    if (mask == 0 && profile_ && profile_->class_ >= 1 && profile_->class_ <= 16) mask = 1u << (profile_->class_ - 1);
    return mask;
}

void GameConnection::spell_message(std::uint32_t eqstr, std::vector<std::string> args, std::uint32_t color) {
    chat(ChatEvent::Kind::Spell, strings_.format(eqstr, args), 0, color);
}

void GameConnection::init_spells_from_profile() {
    if (!profile_) return;
    const auto now = now_ms();
    const std::uint32_t eq_now = eq_time();
    // The profile's food and drink until OP_Stamina updates them (0 is starving / dehydrated, not "unknown").
    spells_.hunger = profile_->hunger;
    spells_.thirst = profile_->thirst;
    spells_.book.fill(game::kNoSpell);
    for (std::size_t i = 0; i < profile_->spell_book.size() && i < spells_.book.size(); ++i)
        spells_.book[i] = profile_->spell_book[i];
    for (auto& g : spells_.gems) g = {};
    for (std::size_t i = 0; i < profile_->mem_spells.size() && i < static_cast<std::size_t>(game::kSpellGems); ++i) {
        spells_.gems[i].spell = profile_->mem_spells[i];
        // spellSlotRefresh is the gem's remaining refresh in ms: the profile load makes it absolute (+ __EQGetTime)
        // and zone-in copies it into PlayerClient +0x2e8.
        if (i < profile_->gem_refresh.size()) spells_.gems[i].ready = profile_->gem_refresh[i] + eq_now;
    }
    spells_.recovery_ready = 0;
    item_recast_timers_.fill(0);
    for (std::size_t i = 0; i < profile_->item_recast_timers.size() && i < item_recast_timers_.size(); ++i)
        item_recast_timers_[i] = profile_->item_recast_timers[i];
    // PcClient_ApplyBaseProfileData @0x5789b0: the discipline and linked spell timers 0..19 as they are.
    spells_.discipline_ready.fill(0);
    for (std::size_t i = 0; i < profile_->combat_ability_timers.size() && i < spells_.discipline_ready.size(); ++i)
        spells_.discipline_ready[i] = profile_->combat_ability_timers[i];
    spells_.linked_ready.fill(0);
    for (std::size_t i = 0; i < profile_->recast_timers.size() && i < spells_.linked_ready.size(); ++i)
        spells_.linked_ready[i] = profile_->recast_timers[i];
    spells_.casting = {};
    spells_.pending = 0;
    spells_.memorizing.reset();
    spells_.memorize_sent_gem = -1;
    for (auto& b : spells_.buffs) b = {};
    spells_.last_buff_tick_ms = now;
    spells_.last_upkeep_tick_ms = 0; // set at the next frame (DoPassageOfTime @0x45ee13)
    spells_.buff_timers_on_hold = false; // a new player spawn (PlayerZoneClient::ResetVariables @0x5a01be)
    for (std::size_t i = 0; i < profile_->buffs.size() && i < spells_.buffs.size(); ++i) {
        const auto& pb = profile_->buffs[i];
        if (pb.spell_id == 0 || pb.spell_id == 0xffffffffu || pb.spell_id == 0xffffu) continue;
        game::BuffSlot& b = spells_.buffs[i];
        b.spell = pb.spell_id;
        b.tics = pb.duration;
        b.received_ms = now;
        b.level = pb.level;
        b.caster_id = pb.caster_id;
        b.num_hits = pb.num_hits;
        b.modifier = pb.instrument_mod;
        if (pb.effect_type != 0) b.effect_type = pb.effect_type;
    }
    std::size_t book = std::count_if(spells_.book.begin(), spells_.book.end(),
                                     [](std::uint32_t s) { return s != game::kNoSpell && s != 0; });
    log::info("spells: {} in the book, gems {}", book, [&] {
        std::string g;
        for (int i = 0; i < game::kSpellGems; ++i) {
            auto id = spells_.gems[static_cast<std::size_t>(i)].spell;
            if (id != game::kNoSpell) g += std::to_string(i + 1) + ":" + spell_name(*spell_table_, id) + " ";
        }
        return g.empty() ? std::string("empty") : g;
    }());
}

void GameConnection::update_spells() {
    if (!game_steps_) {
        update_spells_at(now_ms());
        return;
    }
    for (std::int64_t t : *game_steps_) update_spells_at(t);
}

void GameConnection::update_spells_at(std::int64_t now) {
    if (pstatus_.dead && spells_.casting.casting()) spells_.casting = {};
    // DoPassageOfTime's 1 s endurance upkeep and 6 s buff and regen tick (game_connection_ticks.cpp).
    passage_of_time(now);
    // Memorizing: CSpellBookWnd::GetSpellMemTicksLeft runs when the book's memorise gauge (EQType 9) is drawn,
    // once a frame while the book is shown (game/mechanics/spell_gems.h).
    if (spell_book_open_) {
        set_mem_frame(); // CSpellBookWnd::OnProcessFrame: a spell set's next gem
        memorize_tick(now);
    }
    // CanUseMemorizedSpellSlot for gems 0..11 (CCastSpellWnd::RefreshSpellGemButtons asks it every refresh).
    update_usable_gems();
    // Scribing: 92 ticks, 3 per frame at most every 2 ms (GetSpellScribeTicksLeft @0x75e620, the book's EQType 10
    // gauge).
    if (auto& sc = spells_.scribing; sc && !sc->sent && spell_book_open_) {
        if (game::scribe_countdown(*sc, spells_.scribe_last_tick, static_cast<std::uint32_t>(now))) {
            MemorizeSpell req;
            req.slot = static_cast<std::uint32_t>(sc->book_slot);
            req.spell_id = sc->spell;
            req.scribing = kMemScribe;
            send_zone(encode(req));
            ++spells_.pending;
            sc->sent = true;
        }
    }
    // The book only works sitting: standing up ends a memorization in progress.
    if (spells_.memorizing && !sitting()) spells_.memorizing.reset();
    if (spells_.scribing && !spells_.scribing->sent && !sitting()) spells_.scribing.reset();
    for (auto it = spells_.others.begin(); it != spells_.others.end();) {
        if (now - it->second.started_ms > static_cast<std::int64_t>(it->second.cast_time) + 1000) it = spells_.others.erase(it);
        else ++it;
    }
}

void GameConnection::with_character(const std::function<void(game::mechanics::Character&)>& fn) const {
    if (!profile_) return;
    static const game::mechanics::GameTables no_tables; // the effect totals read no tables
    game::mechanics::LiveState live;
    live.leadership = leadership_bonuses();
    live.drunkenness = drunkenness_;
    live.owner_status = pstatus_.owner_status;
    live.pvp_rules = pvp_rules_;
    live.level = pstatus_.level;
    live.player_id = world_.player_id;
    live.now_ms = now_ms();
    live.vitals = server_ext().character_vitals(*this);
    live.focus_classes = focus_class_mask();
    // Built every frame (update_usable_gems), so into the last call's sheet; a call from inside `fn` uses its own.
    std::optional<game::mechanics::CharacterSheet> nested;
    game::mechanics::CharacterSheet* sheet = nullptr;
    if (character_sheet_busy_) {
        sheet = &nested.emplace();
    } else {
        if (!character_sheet_) character_sheet_ = std::make_unique<game::mechanics::CharacterSheet>();
        sheet = character_sheet_.get();
    }
    struct Busy {
        bool& flag;
        bool was;
        ~Busy() { flag = was; }
    } busy{character_sheet_busy_, character_sheet_busy_};
    character_sheet_busy_ = true;
    game::mechanics::build_sheet_into(
        *sheet, *profile_, inventory_, spells_, live,
        [this](std::vector<game::mechanics::SheetAbility>& out) { alt_adv_.sheet_abilities_into(out); }, nullptr,
        &world_);
    game::mechanics::Character ch(*sheet, no_tables, game::mechanics::spell_lookup(*spell_table_));
    fn(ch);
}

namespace {

// GetSpellMemTicksLeft's callees on the connection's state.
class MemorizeCallees final : public game::mechanics::gems::MemorizeQueries {
public:
    using Send = std::function<bool(const game::mechanics::gems::MemorizePacket&)>;
    using LevelNeeded = std::function<std::uint8_t(const game::SpellData&, std::uint32_t)>;
    // `stat`: 0 CalcDEX, 1 CalcINT, 2 the meditate skill (GetAdjustedSkill(31)).
    MemorizeCallees(const game::SpellTable& spells, std::int32_t level, std::function<std::int32_t(int)> stat,
                    bool aa286, Send send, LevelNeeded level_needed)
        : spells_(spells), level_(level), stat_(std::move(stat)), aa286_(aa286), send_(std::move(send)),
          level_needed_(std::move(level_needed)) {}
    std::uint8_t spell_level_needed(std::int32_t id, std::uint8_t class_id) override {
        // EQ_Spell::GetSpellLevelNeeded (a missing record: 255), through the NMS patch's detour on an NMS server:
        // GetSpellMemTicksLeft is not hooked, but its first step asks for the profile class's level, which the
        // detour answers with the mask's minimum.
        const game::SpellData* s = id >= 0 ? spells_.get(static_cast<std::uint32_t>(id)) : nullptr;
        if (!s) return 0xff;
        return level_needed_(*s, class_id);
    }
    std::int32_t level() override { return level_; }
    std::int32_t calc_dex() override { return stat_(0); }
    std::int32_t calc_int() override { return stat_(1); }
    std::int32_t adjusted_skill(std::int32_t) override { return stat_(2); }
    bool has_alternate_ability(std::int32_t) override { return aa286_; }
    void push_pending_reason() override {}
    bool send(const game::mechanics::gems::MemorizePacket& p) override { return send_(p); }
    void close_connection() override {}
    void debug_log() override {}
    void cursor_attachment_vf94() override {}

private:
    const game::SpellTable& spells_;
    std::int32_t level_;
    std::function<std::int32_t(int)> stat_;
    bool aa286_;
    Send send_;
    LevelNeeded level_needed_;
};

} // namespace

// ---------------------------------------------------------------- blocked buffs

void GameConnection::block_buff(bool pet, std::int32_t spell) {
    auto& list = blocked_buffs_[pet ? 1 : 0];
    if (state_ != ConnectionState::InZone || spell <= 0 || std::find(list.begin(), list.end(), spell) != list.end()) return;
    if (list.size() >= static_cast<std::size_t>(kBlockedBuffSlots)) return;
    BlockedBuffs b;
    b.spells = {spell};
    b.pet = pet;
    send_zone(encode(b)); // the server answers with the list as it took it (beneficial, blockable spells only)
}

void GameConnection::unblock_buff(bool pet, std::int32_t spell) {
    if (state_ != ConnectionState::InZone || spell <= 0) return;
    BlockedBuffs b;
    b.spells = {spell};
    b.pet = pet;
    send_zone(encode(b, true));
}

void GameConnection::resend_blocked_buffs() {
    for (int k = 0; k < 2; ++k) {
        if (blocked_buffs_[static_cast<std::size_t>(k)].empty()) continue;
        BlockedBuffs b;
        b.spells = blocked_buffs_[static_cast<std::size_t>(k)];
        b.pet = k == 1;
        b.initialise = true;
        send_zone(encode(b));
    }
}

void GameConnection::memorize_tick(std::int64_t now) {
    auto& m = spells_.memorizing;
    if (!m || state_ != ConnectionState::InZone) return;
    namespace gems = game::mechanics::gems;
    gems::MemorizeBook book{m->gem, static_cast<std::int32_t>(m->spell), m->ticks_left};
    gems::MemorizeWorld w;
    w.class_id = profile_ ? profile_->class_ : 0;
    w.now = static_cast<std::uint32_t>(now); // eq_time()'s clock
    w.last_tick = spells_.memorize_last_tick;
    w.item_pending = spells_.pending;
    // DEX, INT and meditate from one sheet, built only on a tick whose interval has passed (the others ask for none).
    // Meditate is read before ProcessAllStats, as it was on a sheet of its own; all three see the same state.
    std::optional<std::array<std::int32_t, 3>> stats;
    auto stat = [this, &stats](int which) {
        if (!stats) {
            auto& v = stats.emplace(std::array<std::int32_t, 3>{});
            with_character([&](game::mechanics::Character& ch) {
                v[2] = ch.adjusted_skill(31);
                ch.process_all_stats(); // CalcDEX / CalcINT read the item totals ProcessAllStats keeps
                v[0] = ch.calc_dex(true);
                v[1] = ch.calc_int(true);
            });
        }
        return (*stats)[static_cast<std::size_t>(which)];
    };
    MemorizeCallees q(*spell_table_, pstatus_.level, stat, alt_adv_.has(gems::kAaMemorizeBoost),
                      [this](const gems::MemorizePacket& p) {
                          MemorizeSpell req;
                          req.slot = static_cast<std::uint32_t>(p.slot);
                          req.spell_id = static_cast<std::uint32_t>(p.spell_id);
                          req.scribing = static_cast<std::uint32_t>(p.scribing);
                          send_zone(encode(req));
                          return true;
                      },
                      [this](const game::SpellData& s, std::uint32_t c) { return spell_level_needed(s, c); });
    const int gem = m->gem;
    const std::int32_t before = book.ticks_left;
    gems::get_spell_mem_ticks_left(book, w, q);
    if (book.ticks_left != before || book.gem == -1)
        log::debug("memorize tick at {} ms: {} -> {} ticks left", w.now, before, book.gem == -1 ? 0 : book.ticks_left);
    spells_.memorize_last_tick = w.last_tick;
    spells_.pending = w.item_pending;
    if (book.gem == -1) { // sent: the book's memorise is over; the server's OP_MemorizeSpell(1) answers it
        spells_.memorize_sent_gem = gem;
        m.reset();
        return;
    }
    m->ticks_left = book.ticks_left;
}

void GameConnection::update_usable_gems() {
    namespace gems = game::mechanics::gems;
    std::int32_t aa = 0, buff = 0, item = 0;
    bool totals_known = false;
    // The SPA 326 totals, built only when CanUseMemorizedSpellSlot asks for them (a server extension that answers
    // every gem needs no character).
    auto known = [&] {
        if (totals_known) return;
        totals_known = true;
        with_character([&](game::mechanics::Character& ch) {
            aa = ch.cached_aa(gems::kSpaSpellSlots, 0);
            buff = ch.cached_buff(gems::kSpaSpellSlots, 0);
            item = ch.cached_item(gems::kSpaSpellSlots, 0);
        });
    };
    gems::SpellSlotTotals totals{[&] { known(); return aa; }, [&] { known(); return buff; },
                                 [&] { known(); return item; }};
    int n = 0;
    // CanUseMemorizedSpellSlot, or the server extension's (the NMS patch's detour: gem < 12, from level 1).
    const game::ServerExtension& ext = server_ext();
    bool stock = false; // a gem the server extension did not answer
    auto usable = [&](int gem) {
        if (auto v = ext.can_use_gem(*this, gem)) return *v;
        stock = true;
        return gems::can_use_memorized_spell_slot(profile_.has_value(), 0, gem, totals);
    };
    while (n < game::kSpellGems && usable(n)) ++n;
    if (n != usable_gems_ && !stock)
        log::info("spells: {} gems usable (the server's rule)", n);
    else if (n != usable_gems_ && totals_known && profile_)
        log::info("spells: {} gems usable (SPA 326: AA {}, buffs {}, items {})", n, aa, buff, item);
    else if (n != usable_gems_)
        log::info("spells: {} gems usable", n);
    usable_gems_ = n;
}

namespace {

// Handle_OP_MemorizeSpell's callees on the connection's state. The cast item's recast is not kept by the client (no
// cast item is ever valid here) and the cast window always exists. A bard's twist goes on through `twist`.
class MemorizeHandler final : public game::mechanics::gems::MemorizeHandlerQueries {
public:
    using Fn = std::function<void(std::int32_t, std::int32_t)>;
    MemorizeHandler(game::SpellState& st, const game::SpellTable& spells, std::uint8_t class_id, Fn scribed, Fn memorized,
                    Fn forgotten, std::function<std::int32_t(std::int32_t)> total, GameConnection& twist)
        : st_(st), spells_(spells), class_id_(class_id), scribed_(std::move(scribed)), memorized_(std::move(memorized)),
          forgotten_(std::move(forgotten)), total_(std::move(total)), twist_(twist) {}
    void pop_pending_reason() override {}
    bool book_window() override { return true; }
    bool cast_window() override { return true; }
    void finish_scribing(std::int32_t slot, std::int32_t spell) override { scribed_(slot, spell); }
    void finish_memorizing(std::int32_t gem, std::int32_t spell) override { memorized_(gem, spell); }
    void forget_memorized_spell(std::int32_t gem) override { forgotten_(gem, -1); }
    bool local_pc() override { return true; }
    bool local_player() override { return true; }
    void set_memorized_spell(std::int32_t gem, std::int32_t spell) override { forgotten_(gem, spell); }
    // The cast item's own recast timer (+0x34): the original adds the recast seconds to CDisplay +0x154 (EQ time,
    // ms) here, not to Util::FastTime as everywhere else; ported as it is (docs/unknowns.md).
    bool is_cast_item_valid() override { return twist_.valid_cast_item() != nullptr; }
    void get_cast_item() override {
        item_ = twist_.cast_item();
    }
    std::int32_t item_cast_type() override { return static_cast<std::int32_t>(st_.casting.item_cast_type); }
    std::int32_t item_recast_time(std::int32_t) override {
        return item_ ? static_cast<std::int32_t>(item_->effects[kEffectClick].recast_delay()) : 0;
    }
    std::uint32_t item_ready_time() override { return item_ ? item_->last_cast_time : 0; }
    void set_item_ready_time(std::uint32_t ready, bool) override {
        if (item_) item_->last_cast_time = ready;
    }
    bool is_valid_spell_id(std::int32_t id) override { return id >= 1 && id <= 45000; }
    std::optional<std::pair<std::int32_t, std::int32_t>> spell_times(std::int32_t id) override {
        const game::SpellData* s = spells_.get(static_cast<std::uint32_t>(id));
        if (!s) return std::nullopt;
        return std::make_pair(static_cast<std::int32_t>(s->recast_time), static_cast<std::int32_t>(s->recovery_time));
    }
    std::int32_t total_effect(std::int32_t spa) override { return total_(spa); }
    void set_gem_timer(std::int32_t gem, std::uint32_t ready) override {
        if (gem >= 0 && gem <= game::kItemGem) st_.gems[static_cast<std::size_t>(gem)].ready = ready;
    }
    void set_cast_slot(std::uint8_t slot) override { st_.casting.slot = slot; }
    void set_recovery(std::uint32_t ready) override { st_.recovery_ready = ready; }
    std::uint8_t class_id() override { return class_id_; }
    std::int32_t next_bard_twist_gem() override { return twist_.next_bard_twist_gem(); }
    void stop_current_spell_cast(std::int32_t) override { twist_.stop_current_spell_cast(); }
    void cast_gem(std::int32_t gem) override { twist_.cast_gem(gem); }

private:
    game::SpellState& st_;
    const game::SpellTable& spells_;
    std::uint8_t class_id_;
    Fn scribed_, memorized_, forgotten_;
    std::function<std::int32_t(std::int32_t)> total_;
    GameConnection& twist_;
    Item* item_ = nullptr; // LaunchSpellData_GetCastItem's
};

// CSpellBookWnd::FinishMemorizing's callees (a memorise from the book: mode 0, the cast window refreshes).
class FinishCallees final : public game::mechanics::gems::FinishQueries {
public:
    FinishCallees(game::SpellState& st, const game::SpellTable& spells,
                  std::function<void(std::uint32_t, std::int32_t, std::uint32_t)> say,
                  std::function<void()> continue_set = {}, std::function<void()> hide_book = {})
        : st_(st), spells_(spells), say_(std::move(say)), continue_set_(std::move(continue_set)),
          hide_book_(std::move(hide_book)) {}
    void set_memorized(std::uint32_t gem, std::int32_t spell) override {
        if (gem < static_cast<std::uint32_t>(game::kSpellGems)) st_.gems[gem].spell = static_cast<std::uint32_t>(spell);
    }
    void say(std::uint32_t eqstr, std::int32_t spell, std::uint32_t color) override { say_(eqstr, spell, color); }
    std::int32_t recast_time(std::int32_t id) override {
        const game::SpellData* s = spells_.get(static_cast<std::uint32_t>(id));
        return s ? static_cast<std::int32_t>(s->recast_time) : 0;
    }
    void set_gem_timer(std::uint32_t gem, std::uint32_t ready) override {
        if (gem <= static_cast<std::uint32_t>(game::kItemGem)) st_.gems[gem].ready = ready;
    }
    void hide_book() override {
        if (hide_book_) hide_book_();
    }
    void continue_set_mem() override {
        if (continue_set_) continue_set_();
    }
    bool cast_window() override { return true; }
    void update_spell_gems(std::int32_t) override {}

private:
    game::SpellState& st_;
    const game::SpellTable& spells_;
    std::function<void(std::uint32_t, std::int32_t, std::uint32_t)> say_;
    std::function<void()> continue_set_, hide_book_;
};

} // namespace

void GameConnection::handle_memorize_spell(const MemorizeSpell& m) {
    namespace gems = game::mechanics::gems;
    const std::uint32_t now = eq_time();
    auto scribed = [&](std::int32_t slot, std::int32_t spell) {
        // CSpellBookWnd::FinishScribing.
        if (slot >= 0 && slot < game::kSpellBookSize) spells_.book[static_cast<std::size_t>(slot)] = static_cast<std::uint32_t>(spell);
        spells_.scribing.reset();
        spell_message(12006, {spell_name(*spell_table_, static_cast<std::uint32_t>(spell))}); // You have finished scribing %1.
    };
    auto memorized = [&](std::int32_t gem, std::int32_t spell) {
        // CSpellBookWnd +0x254: 2 while a spell set is memorized (the next gem follows), else 0 (a memorise from the
        // book: the gems refresh)
        FinishCallees q(
            spells_, *spell_table_,
            [&](std::uint32_t eqstr, std::int32_t id, std::uint32_t color) {
                spell_message(eqstr, {spell_name(*spell_table_, static_cast<std::uint32_t>(id))}, color);
            },
            [this] { continue_set_mem(); }, [this] { close_spell_book(); });
        gems::finish_memorizing(set_mem_.mode, set_mem_.keep_open, static_cast<std::uint32_t>(gem), spell, now, q);
        if (spells_.memorize_sent_gem == gem) spells_.memorize_sent_gem = -1;
    };
    auto forgotten = [&](std::int32_t gem, std::int32_t) {
        // CCastSpellWnd::ForgetMemorizedSpell: the gem empties. The server forgets the old spell before memorising
        // a new one into the same gem; only a forget the player asked for gets the message.
        if (gem < 0 || gem >= game::kSpellGems) return;
        auto& g = spells_.gems[static_cast<std::size_t>(gem)];
        std::uint32_t old = g.spell;
        g = {};
        bool replacing = spells_.memorize_sent_gem == gem || (spells_.memorizing && spells_.memorizing->gem == gem);
        // ForgetMemorizedSpell @0x647d50 names the gem's own spell, and only a valid one (an empty gem a spell set
        // clears says nothing)
        if (!replacing && old != game::kNoSpell && spell_table_->get(old))
            spell_message(12496, {spell_name(*spell_table_, old)}); // You forget %1.
    };
    auto total = [this](std::int32_t spa) {
        std::int32_t v = 0;
        with_character([&](game::mechanics::Character& ch) { v = ch.total_effect(spa, true, 0, true, true); });
        return v;
    };
    MemorizeHandler q(spells_, *spell_table_, profile_ ? profile_->class_ : 0, scribed, memorized, forgotten, total,
                      *this);
    const auto scribing = static_cast<std::int32_t>(m.scribing);
    gems::handle_op_memorize_spell(scribing, static_cast<std::int32_t>(m.slot), static_cast<std::int32_t>(m.spell_id),
                                   static_cast<std::int32_t>(m.reduction), now, spells_.pending, q);
    if (scribing == -1) { // refused
        spells_.memorize_sent_gem = -1;
        spells_.scribing.reset();
    }
}

void GameConnection::stop_spell_cast(std::uint32_t spell) {
    // StopSpellCast @0x4348a0 for reason 3 (the server ended the cast), on the packet's spell (else the one cast);
    // a bard's song ends as game_connection_ability_commands.cpp has it (the twist goes on).
    const game::SpellData* s = spell_table_->get(spell != game::kNoSpell ? spell : spells_.casting.spell);
    spells_.pending = std::max(0, spells_.pending - 1);
    spells_.casting = {};
    if (s && bard_song(*s)) bard_song_stopped(3);
}

// ---------------------------------------------------------------- rest state and visibility

std::uint32_t GameConnection::rest_downtime_left() const {
    // vtable +0x84 @0x577dd0: downtime - (FastTime - start), seconds; 0 once it ran out.
    if (rest_since_ms_ == 0) return 0;
    std::int64_t left = static_cast<std::int64_t>(rest_downtime_) - (now_ms() / 1000 - rest_since_ms_ / 1000);
    return left > 0 ? static_cast<std::uint32_t>(left) : 0;
}

bool GameConnection::resting_for_fast_regen() const {
    // No active detrimental effect that stops regen (spell +0x23b BypassRegenCheck), out of combat, no
    // downtime left. (The server rule 0xddfd36 / 0xddfd38 is not known to the client: off.)
    for (const auto& b : spells_.buffs) {
        if (b.empty()) continue;
        const auto* s = spell_table_->get(b.spell);
        if (s && !s->beneficial() && !s->bypass_regen_check) return false;
    }
    return rest_downtime_left() == 0 && !in_combat_;
}

int GameConnection::own_effect_with(std::int32_t spa) const {
    for (int slot = 0; slot < game::kBuffSlots; ++slot) {
        const auto& b = spells_.buffs[static_cast<std::size_t>(slot)];
        if (b.empty()) continue;
        if (const auto* s = spell_table_->get(b.spell); s && s->has_effect(spa)) return slot;
    }
    return -1;
}

void GameConnection::remove_own_effect(int slot) {
    // CharacterZoneClient_RemoveAffect(effect, notify 1): the slot is cleared and OP_Buff {me, the effect with
    // its spell id, slot} goes to the server (EQEmu fades the spell by id). The fade dword is left unset by
    // the original; 0 here.
    auto& b = spells_.buffs[static_cast<std::size_t>(slot)];
    if (b.empty()) return;
    BuffPacket bp;
    bp.entity_id = world_.player_id;
    bp.buff.effect_type = b.effect_type;
    bp.buff.level = b.level;
    bp.buff.bard_modifier = b.modifier;
    bp.buff.spell_id = b.spell;
    bp.buff.duration = b.tics;
    bp.buff.caster_id = b.caster_id;
    bp.buff.num_hits = b.num_hits;
    bp.slot = static_cast<std::uint32_t>(slot);
    send_zone(encode(bp));
    b = {};
    reconcile_eye_control();
}

bool GameConnection::invisible() const {
    if (invis_state_ != 0 || hidden_) return true;
    for (std::int32_t spa : {12, 28, 29, 314, 315, 316})
        if (own_effect_with(spa) >= 0) return true;
    return false;
}

std::int32_t GameConnection::see_invisible_rank() const {
    if (const auto* player = world_.player(); player && player->gm) return 3001;
    std::int32_t rank = 0;
    with_character([&](game::mechanics::Character& ch) { rank = ch.total_effect(13, true, 0, true, true); });
    return rank;
}

bool GameConnection::spawn_invisible(const game::Spawn& s, std::int32_t rank) const {
    if (market_hidden(s)) return true;
    const auto* player = world_.player();
    return player && game::spawn_invisible(*player, s, rank, group_.has_member(s.name));
}

void GameConnection::make_me_visible(bool appearance) {
    if (state_ != ConnectionState::InZone || !invisible()) return;
    invis_state_ = 0;
    if (auto player = world_.spawns.find(world_.player_id); player != world_.spawns.end()) player->second.invis_rank = 0;
    for (std::int32_t spa : {0xc, 0x1c, 0x1d, 0x13a, 0x13b, 0x13c, 0x5e})
        if (int slot = own_effect_with(spa); slot >= 0) remove_own_effect(slot);
    SpawnAppearance a;
    a.spawn_id = static_cast<std::uint16_t>(world_.player_id);
    a.type = 3;
    a.parameter = 0;
    send_zone(encode(a));
    // Send_OP_Hide @0x52b9e0.
    if (hidden_) {
        hidden_ = false;
        visibility_cleared_.hide = true;
        send_zone(encode_hide_cancel());
        system(12337);
    }
    // Send_OP_SpawnAppearance_0052bae0: sneaking ends.
    if (appearance && sneaking_) {
        sneaking_ = false;
        visibility_cleared_.sneak = true;
        system(12338);
        a.type = 15;
        send_zone(encode(a));
    }
}

void GameConnection::stop_cast() {
    if (state_ != ConnectionState::InZone) return;
    // Cmd_stopcast @0x4e8970: non-bards ask the server and wait for its OP_ManaChange.
    if (!spells_.casting.casting() && spells_.pending == 0) return;
    send_zone(encode_stop_cast_request());
}

void GameConnection::memorize(int book_slot, int gem) {
    if (state_ != ConnectionState::InZone || book_slot < 0 || book_slot >= game::kSpellBookSize || gem < 0 ||
        gem >= usable_gems())
        return;
    // The book's click handler (@0x75e790) only works sitting (or mounted).
    if (!sitting() || spells_.casting.casting() || !can_start_memming(book_slot)) return;
    const std::uint32_t id = spells_.book[static_cast<std::size_t>(book_slot)];
    const game::SpellData* spell = spell_table_->get(id);
    if (!spell) return;
    // The memorise runs while the book's gauge is drawn: the book must be shown (a memorise starts from it).
    if (!spell_book_open_ && !open_spell_book()) return;
    // StartSpellMemorization @0x75bf30: 92 ticks to go; GetSpellMemTicksLeft counts them down (memorize_tick).
    game::Memorizing m;
    m.gem = gem;
    m.book_slot = book_slot;
    m.spell = id;
    m.ticks_left = game::kMemorizeTicks;
    spells_.memorizing = m;
    spell_message(12053, {spell->name}); // Beginning to memorize %1...
}

bool GameConnection::can_start_memming(int book_slot) {
    // Nothing pending, hands empty (CCursorAttachment::RemoveAttachment refuses only while an item or money is
    // held), no memorize or scribe under way.
    if (state_ != ConnectionState::InZone || book_slot < 0 || book_slot >= game::kSpellBookSize || spells_.pending > 0 ||
        inventory_.cursor() || spells_.memorizing || spells_.scribing)
        return false;
    const std::uint32_t id = spells_.book[static_cast<std::size_t>(book_slot)];
    const game::SpellData* spell = id == game::kNoSpell ? nullptr : spell_table_->get(id);
    if (!spell) return false;
    bool ok = true;
    for (const auto& g : spells_.gems)
        if (g.spell == id) { spell_message(12052); ok = false; } // once per gem holding it, as the original
    const std::uint8_t need = spell_level_needed(*spell, profile_ ? profile_->class_ : 0);
    if (need > pstatus_.level) { spell_message(12054); return false; }
    return ok;
}

std::uint32_t GameConnection::cursor_scroll_spell() const {
    const Item* it = inventory_.cursor();
    if (!it || it->item_type != 20) return 0; // ItemClient::GetItemType == 0x14
    std::int32_t id = it->effects[kEffectScroll].effect;
    return id > 0 ? static_cast<std::uint32_t>(id) : 0;
}

void GameConnection::scribe_cursor(int book_slot) {
    if (state_ != ConnectionState::InZone || !sitting() || spells_.scribing || spells_.pending > 0) return;
    const Item* item = inventory_.cursor();
    std::uint32_t id = cursor_scroll_spell();
    const game::SpellData* spell = id ? spell_table_->get(id) : nullptr;
    if (!item || !spell) return;
    // CSpellBookWnd::StartSpellScribe @0x75ddf0.
    for (std::uint32_t known : spells_.book)
        if (known == id) { spell_message(12050, {spell->name}); return; } // You already have scribed the %1 in your book.
    const std::uint8_t need = spell_level_needed(*spell, profile_ ? profile_->class_ : 0);
    if (need == 255) { spell_message(12047, {spell->name}); return; }                              // Your class can never use the %1.
    if (need > pstatus_.level) { spell_message(12048, {std::to_string(need), spell->name}); return; } // achieve level %1 ...
    if (item->stackable() && item->charges >= 2) { spell_message(6192); return; } // a stack of spells
    if (book_slot < 0)
        for (int i = 0; i < game::kSpellBookSize && book_slot < 0; ++i)
            if (spells_.book[static_cast<std::size_t>(i)] == game::kNoSpell || spells_.book[static_cast<std::size_t>(i)] == 0)
                book_slot = i;
    if (book_slot < 0 || book_slot >= game::kSpellBookSize) return;
    auto have = spells_.book[static_cast<std::size_t>(book_slot)];
    if (have != game::kNoSpell && have != 0) { spell_message(12051); return; } // You already have a spell in that slot.
    if (!spell_book_open_ && !open_spell_book()) return; // the scribe gauge is the book's
    game::Scribing sc;
    sc.book_slot = book_slot;
    sc.spell = id;
    spells_.scribing = sc;
    spell_message(12049, {spell->name}); // Beginning to scribe %1...
}

void GameConnection::forget_gem(int gem) {
    if (state_ != ConnectionState::InZone || gem < 0 || gem >= game::kSpellGems) return;
    const std::uint32_t id = spells_.gems[static_cast<std::size_t>(gem)].spell;
    if (id == game::kNoSpell) return;
    if (spells_.memorizing && spells_.memorizing->gem == gem) spells_.memorizing.reset();
    // CCastSpellWnd::Unmemorize @0x648ac0.
    MemorizeSpell req;
    req.slot = static_cast<std::uint32_t>(gem);
    req.spell_id = id;
    req.scribing = kMemForget;
    send_zone(encode(req));
    ++spells_.pending;
}

void GameConnection::remove_buff(int slot) {
    if (state_ != ConnectionState::InZone || slot < 0 || slot >= game::kBuffSlots) return;
    const auto& b = spells_.buffs[static_cast<std::size_t>(slot)];
    if (b.empty()) return;
    // Detrimental effects cannot be clicked off (the server refuses them too).
    if (const auto* s = spell_table_->get(b.spell); s && !s->beneficial()) return;
    send_zone(encode_buff_remove_request(static_cast<std::uint32_t>(slot), world_.player_id));
}

void GameConnection::clear_buff_locally(int slot) {
    if (slot < 0 || slot >= game::kBuffSlots) return;
    spells_.buffs[static_cast<std::size_t>(slot)] = {};
    reconcile_eye_control();
}

std::vector<int> GameConnection::buff_slots_with_spa(std::int32_t spa) const {
    std::vector<int> out;
    for (int i = 0; i < game::kBuffSlots; ++i) {
        const auto& b = spells_.buffs[static_cast<std::size_t>(i)];
        if (b.empty()) continue;
        // GetPCSpellAffect @0x433fd0 stops at the spell's first blank slot.
        if (const auto* s = spell_table_->get(b.spell); s && s->has_effect_before_blank(spa)) out.push_back(i);
    }
    return out;
}

void GameConnection::handle_spell_action(const Action& a, std::int64_t now, bool from_server) {
    if (a.type != 231) return;
    log::debug("action: spell {} {} -> {} flag {}", a.spell_id, a.source, a.target, a.effect_flag);
    // The caster's animation (PlayerZoneClient::IWasHit -> TriggerSpellEffect); visuals later. Only a landing the
    // server sent ends the source's cast: spell 859's local presentation says nothing about it.
    if (from_server) spells_.others.erase(a.source);
    SoundCue c;
    c.kind = SoundCue::Kind::SpellLanded;
    c.source = a.source;
    c.target = a.target;
    c.spell = a.spell_id;
    sound_cues_.push_back(c);
    // Hailstone: a spell landing with no OP_BeginCast before it (EQEmu sends one for a buff's proc but not for an
    // item, weapon or augment proc, Mob::TryWeaponProc) gets the cast stage at its source as a cast would. The
    // original plays only the impact here (EQPlayer::TriggerSpellEffect @0x593d10). Only the first of a landing's
    // two OP_Actions (flag 4 marks the second), not a bard song's pulse, and only for one the server sent: spell
    // 859's local landing plays only the impact, and never sends an animation.
    if (const game::SpellData* sp = spell_table_->get(a.spell_id);
        from_server && sp && a.source != 0 && (a.effect_flag & 4) == 0 && !is_song_skill(sp->skill)) {
        // Hailstone: a spell without a casting animation plays CastHeals or CastNuke (option
        // cast_animation_fallback, game::spell_cast_animation).
        const std::uint8_t anim = spell_cast_animation(sp->casting_anim, sp->spell_type, cast_animation_fallback_);
        play_animation(a.source, anim);
        // Our own: the zone is told of the animation too, as the server does not pass it on to players with
        // some chat filters on; a second one does not interrupt a playing animation and is range limited.
        if (a.source == world_.player_id) {
            Animation animation;
            animation.spawn_id = static_cast<std::uint16_t>(world_.player_id);
            animation.action = anim;
            animation.speed = 10;
            send_zone(encode(animation));
        }
        auto shown = cast_stage_shown_.find(a.source);
        if (shown == cast_stage_shown_.end() || shown->second.first != a.spell_id || now > shown->second.second) {
            SpellEffectCue cue{0, a.source, 0, a.spell_id, 0};
            cue.proc = true;
            effect_cues_.push_back(cue);
            // An area proc's other targets, landing with it, share this one.
            cast_stage_shown_[a.source] = {a.spell_id, now + kProcLandingMs};
        }
    }
    // The impact stage: the source as its caster, the struck spawn as its target (app/spell_particles.h).
    effect_cues_.push_back({2, a.target, a.target, a.spell_id, 0, a.source});
    // PlayerZoneClient::IWasHit @0x5a07b0: a spell on us with packet flag 4 goes through EQ_Character::HitBySpell
    // (game/hit_landing.h).
    if (a.target == world_.player_id && (a.effect_flag & 4) != 0 && profile_ && spell_table_)
        land_spell_on_me(a, now);
}

void GameConnection::spell_missile_hit(const game::Missile& missile) {
    // EQMissile's destruction case for spell 859 calls IWasHit with flag zero only inside 40 horizontal units.
    // It is a local presentation: no OP_Action is sent, no success flag is manufactured, and no buff is applied.
    const auto* me = world_.player();
    const auto* spell = spell_table_->get(859);
    if (!me || !spell || !missile.has_actor || missile.packet.item_type != game::kMissileItemSpell ||
        missile.packet.spell != 859 ||
        glm::distance(glm::vec2(me->position), glm::vec2(missile.position)) >= 40.0f)
        return;
    Action action;
    action.target = static_cast<std::uint16_t>(me->id);
    action.source = static_cast<std::uint16_t>(missile.packet.source_id);
    action.level = static_cast<std::uint16_t>(missile.packet.item_extra);
    action.instrument_mod = 1.0f;
    action.force = spell->push_back;
    action.hit_heading = missile.heading;
    action.hit_pitch = missile.tilt;
    action.type = 231;
    action.spell_id = 859;
    action.spell_level = pstatus_.level; // the local player's GetLevel, as the original's (@0x501342) is
    handle_spell_action(action, now_ms(), false);
}

void GameConnection::handle_spell(Op op, const AppPacket& p) {
    const std::uint32_t me = world_.player_id;
    const auto now = now_ms();
    auto set_buff = [&](game::BuffSlot& b, std::uint32_t spell, std::uint32_t tics, std::uint32_t num_hits) {
        if (b.spell != spell) { // another spell in the slot: its caster level and modifier are not known yet
            b.level = 0;
            b.modifier = 1.0f;
            b.effect_type = 2;
            b.slot_data = {};
        }
        b.spell = spell;
        b.tics = tics;
        b.received_ms = now;
        b.num_hits = num_hits;
    };
    switch (op) {
    case Op::RestState: {
        // PcClient_HandleRestState @0x578530: u8 in combat; out of combat a u32 downtime (seconds) from now.
        ByteReader r(p.data);
        if (r.remaining() >= 1) in_combat_ = r.le<std::uint8_t>() != 0;
        if (in_combat_) {
            rest_downtime_ = 0;
            rest_since_ms_ = 0;
        } else {
            if (r.remaining() >= 4) rest_downtime_ = r.le<std::uint32_t>();
            rest_since_ms_ = now;
        }
        break;
    }
    case Op::ManaChange: {
        // Handle_OP_ManaChange @0x4bbb50: the spell bar enable.
        auto m = decode_mana_change(p);
        if (!m) break;
        int gem = m->slot != -1 ? m->slot : spells_.casting.slot;
        if (gem >= 0 && gem < game::kSpellGems) {
            spells_.gems[static_cast<std::size_t>(gem)].ready = eq_time();
            spells_.recovery_ready = eq_time();
        }
        if (m->keep_casting) {
            spells_.pending = std::max(0, spells_.pending - 1);
            break;
        }
        stop_spell_cast(m->spell_id); // StopSpellCast(3, spell), or StopCurrentSpellCast(3) for -1
        break;
    }
    case Op::BlockedBuffs:
        // The server's list (EQEmu: the whole list after a change, flags 0x54).
        if (auto b = decode_blocked_buffs(p)) blocked_buffs_[b->pet ? 1 : 0] = std::move(b->spells);
        break;
    case Op::ClearBlockedBuffs:
        // EQEmu echoes /blockspell clear's packet once it has cleared the list.
        if (!p.data.empty()) blocked_buffs_[p.data[0] != std::byte{0} ? 1 : 0].clear();
        break;
    case Op::RemoveBlockedBuffs:
        if (auto b = decode_blocked_buffs(p)) {
            auto& list = blocked_buffs_[b->pet ? 1 : 0];
            for (auto id : b->spells) std::erase(list, id);
        }
        break;
    case Op::MemorizeSpell: {
        auto m = decode_memorize_spell(p);
        if (!m) break;
        handle_memorize_spell(*m);
        break;
    }
    case Op::BeginCast:
        if (auto b = decode_begin_cast(p)) {
            // CEverQuest::StartCasting @0x522670. The line depends on the
            // caster spawn's class: bards print the spell's own texts
            // (CastByMe; "%1%2" with CastByOther), everyone else 12205/12206.
            // NMS shows multiclass players as bards and sends 12205/12206 itself
            // (server_ext().own_begin_cast_line: where its filter drops our own).
            auto it = world_.spawns.find(b->caster_id);
            const game::SpellData* sp = spell_table_->get(b->spell_id);
            if (sp) {
                SoundCue c;
                c.kind = SoundCue::Kind::BeginCast;
                c.source = b->caster_id;
                c.spell = b->spell_id;
                sound_cues_.push_back(c);
                // The cast stage's particles, for the cast time.
                effect_cues_.push_back({0, b->caster_id, 0, b->spell_id, static_cast<std::int32_t>(b->cast_time)});
                cast_stage_shown_[b->caster_id] = {b->spell_id, now + b->cast_time + kCastLandingSlackMs};
            }
            bool bard = it != world_.spawns.end() && it->second.class_id == 8;
            if (b->caster_id == me) {
                if (bard && sp && server_ext().own_begin_cast_line(*this, *sp)) {
                    spell_message(12205, {spell_name(*spell_table_, b->spell_id)});
                } else if (bard) {
                    if (sp && !sp->cast_by_me.empty()) chat(ChatEvent::Kind::Spell, sp->cast_by_me);
                } else {
                    spell_message(12205, {spell_name(*spell_table_, b->spell_id)}); // You begin casting %1.
                }
                if (spells_.casting.eta_ms == 0 && !spells_.casting.casting() && b->cast_time > 0) {
                    spells_.casting.spell = b->spell_id;
                    spells_.casting.eta_ms = now + b->cast_time;
                    spells_.casting.cast_time = b->cast_time;
                }
            } else {
                spells_.others[b->caster_id] = {b->spell_id, now, b->cast_time};
                if (it != world_.spawns.end() && b->spell_id != 0xffffffffu) {
                    std::string who = game::display_name(it->second.name);
                    std::string text;
                    if (!bard) {
                        const std::array<std::string, 1> args{who};
                        text = strings_.format(12206, args); // %1 begins to cast a spell.
                    } else if (sp && !sp->cast_by_other.empty()) {
                        const std::array<std::string, 2> args{who, sp->cast_by_other};
                        text = strings_.format(12207, args);
                    }
                    // StartCasting @0x522b76: Spell Awareness identifies another caster's spell through five
                    // levels above the player. Group (8) or raid (23) suffices; a GM sees it independently.
                    if (sp) {
                        const game::Spawn* player = world_.player();
                        if (player_is_gm()) text += " (" + sp->name + ")";
                        else if (player && static_cast<int>(it->second.level) <= static_cast<int>(player->level) + 5 &&
                                 (leadership_ability_active(8, 1) || leadership_ability_active(0x17, 1)))
                            text += " <" + sp->name + ">";
                    }
                    if (!text.empty()) chat(ChatEvent::Kind::Spell, std::move(text));
                }
            }
        }
        break;
    case Op::InterruptCast:
        if (auto ic = decode_interrupt_cast(p)) {
            // Message only; the OP_ManaChange that comes with it ends the cast.
            spells_.others.erase(ic->spawn_id);
            chat(ChatEvent::Kind::Spell, strings_.format(ic->message_id, ic->args), 0, 289); // spell failures
        }
        break;
    case Op::Buff:
        if (auto b = decode_buff(p); b && b->entity_id == me && b->slot < static_cast<std::uint32_t>(game::kBuffSlots)) {
            auto& slot = spells_.buffs[b->slot];
            log::debug("OP_Buff: slot {} held {}, now spell {} (fade {})", b->slot,
                       slot.empty() ? std::int64_t{-1} : std::int64_t{slot.spell}, b->buff.spell_id, b->fade);
            if (b->fade == 1 || b->buff.spell_id == 0xffffffffu) {
                slot = {};
            } else {
                set_buff(slot, b->buff.spell_id, b->buff.duration, b->buff.num_hits);
                slot.level = b->buff.level;
                slot.caster_id = b->buff.caster_id;
                slot.modifier = b->buff.bard_modifier;
                if (b->buff.effect_type != 0) slot.effect_type = b->buff.effect_type;
                std::copy(std::begin(b->buff.slot_data), std::end(b->buff.slot_data), slot.slot_data.begin());
            }
        }
        break;
    case Op::BuffCreate:
    case Op::TargetBuffs:
        if (auto bi = decode_buff_icons(p)) {
            if (op == Op::BuffCreate && bi->entity_id == me) {
                if (bi->all_buffs)
                    for (auto& b : spells_.buffs) b = {};
                for (const auto& e : bi->entries) {
                    if (e.slot >= static_cast<std::uint32_t>(game::kBuffSlots)) continue;
                    auto& slot = spells_.buffs[e.slot];
                    if (e.spell_id == 0xffffffffu) {
                        slot = {};
                        continue;
                    }
                    set_buff(slot, e.spell_id, e.tics, e.num_hits);
                    slot.caster_name = e.caster;
                }
            } else if (bi->entity_id != me || op == Op::TargetBuffs) {
                // The target window's buffs (OP_TargetBuffs is OP_BuffCreate's layout).
                if (spells_.target_buffs_entity != bi->entity_id || bi->all_buffs) spells_.target_buffs.clear();
                spells_.target_buffs_entity = bi->entity_id;
                for (const auto& e : bi->entries) {
                    auto it = std::find_if(spells_.target_buffs.begin(), spells_.target_buffs.end(),
                                           [&](const auto& t) { return t.first == e.slot; });
                    if (e.spell_id == 0xffffffffu) {
                        if (it != spells_.target_buffs.end()) spells_.target_buffs.erase(it);
                        continue;
                    }
                    game::BuffSlot b;
                    set_buff(b, e.spell_id, e.tics, e.num_hits);
                    b.caster_name = e.caster;
                    if (it != spells_.target_buffs.end()) it->second = b;
                    else spells_.target_buffs.emplace_back(e.slot, b);
                }
            }
        }
        break;
    case Op::Action:
        if (auto action = decode_action(p)) handle_spell_action(*action, now, true);
        break;
    case Op::SpellEffect:
        // CEverQuest::HandleWorldMessage @0x4d0087: +0x19 set adds a timed effect, clear removes it.
        if (auto e = decode_spell_effect(p))
            effect_packets_.push_back({e->add ? EffectPacketCue::Kind::Add : EffectPacketCue::Kind::Remove, e->effect,
                                       e->caster, e->target, e->stage1_ms, e->stage1_length_ms, e->stage2_length_ms,
                                       e->full});
        break;
    case Op::AddNimbusEffect:
    case Op::RemoveNimbusEffect:
        if (auto n = decode_nimbus_effect(p))
            effect_packets_.push_back({op == Op::AddNimbusEffect ? EffectPacketCue::Kind::AddNimbus
                                                                 : EffectPacketCue::Kind::RemoveNimbus,
                                       static_cast<std::uint32_t>(n->effect), n->spawn_id, n->spawn_id});
        break;
    case Op::DeleteSpell:
        if (auto d = decode_delete_spell(p); d && d->success && d->book_slot >= 0 && d->book_slot < game::kSpellBookSize)
            spells_.book[static_cast<std::size_t>(d->book_slot)] = game::kNoSpell;
        break;
    case Op::SwapSpell:
        // Handle_OP_SwapSpell @0x75d380: both slots non-negative (and in the book), 1388, the two entries swapped,
        // the book's pick dropped.
        if (auto w = decode_swap_spell(p); w && static_cast<std::int32_t>(w->from_slot) >= 0 &&
                                           static_cast<std::int32_t>(w->to_slot) >= 0 &&
                                           w->from_slot < static_cast<std::uint32_t>(game::kSpellBookSize) &&
                                           w->to_slot < static_cast<std::uint32_t>(game::kSpellBookSize)) {
            system(1388, {}, 15); // Swapping Spell Book Scribe slots.
            std::swap(spells_.book[w->from_slot], spells_.book[w->to_slot]);
            book_swap_slot_ = -1;
        }
        break;
    case Op::ColoredText:
        if (auto t = decode_colored_text(p); t && !t->text.empty()) chat(ChatEvent::Kind::Spell, t->text, 0, t->color);
        break;
    case Op::Stamina:
        if (auto s = decode_stamina(p)) {
            spells_.hunger = s->food;
            spells_.thirst = s->water;
            consume_pending_ = false;
            auto_consume();
        }
        break;
    case Op::LinkedReuse:
        // @0x4cf2eb: PcZoneClient_SetLinkedSpellReuseTimer(id, FastTime() - start + end).
        if (auto l = decode_linked_reuse(p))
            game::mechanics::gems::set_linked_spell_reuse_timer(
                spells_.linked_ready, l->timer_id,
                game::mechanics::gems::linked_reuse_ready(static_cast<std::uint32_t>(std::time(nullptr)), l->end_time,
                                                          l->start_time));
        break;
    case Op::DisciplineTimer:
        // @0x4cf2b1: SetDisciplineReuseTimer(id, FastTime() + seconds); the stock array keeps ids 0..19.
        if (auto d = decode_discipline_timer(p); d && d->timer_id < spells_.discipline_ready.size())
            spells_.discipline_ready[d->timer_id] = static_cast<std::uint32_t>(std::time(nullptr)) + d->seconds;
        break;
    default:
        break;
    }
    reconcile_eye_control();
}

void GameConnection::land_spell_on_me(const Action& a, std::int64_t now) {
    static const game::mechanics::GameTables no_tables;
    game::mechanics::LiveState live;
    live.leadership = leadership_bonuses();
    live.drunkenness = drunkenness_;
    live.owner_status = pstatus_.owner_status;
    live.pvp_rules = pvp_rules_;
    live.level = pstatus_.level;
    live.player_id = world_.player_id;
    live.now_ms = now;
    live.vitals = server_ext().character_vitals(*this); // CharacterBase vf6c = Max_Mana: the NMS patch's detour
    live.focus_classes = focus_class_mask();
    game::mechanics::CharacterSheet sheet = game::mechanics::build_sheet(*profile_, inventory_, spells_, live, nullptr, &world_);
    game::mechanics::Character ch(sheet, no_tables, game::mechanics::spell_lookup(*spell_table_));
    game::HitLandingContext ctx;
    ctx.spells = spell_table_.get();
    ctx.world = &world_;
    ctx.spell_state = &spells_;
    ctx.character = &ch;
    ctx.strings = &strings_;
    ctx.class_id = profile_->class_;
    ctx.level = pstatus_.level;
    ctx.mana = static_cast<std::int32_t>(profile_->mana);
    ctx.drunkenness = drunkenness_;
    ctx.velocity = velocity_;
    ctx.now_ms = static_cast<std::uint32_t>(now);
    ctx.pvp_rules = pvp_rules_;
    if (new_zone_) ctx.zone_type = new_zone_->time_type;
    game::HitLandingResult r = game::land_spell(
        game::hit_record(a.source, a.level, a.instrument_mod, a.hit_heading, a.damage, a.unknown31, a.spell_id,
                         a.spell_level),
        ctx);
    log::debug("HitBySpell: spell {} from {} -> {}", a.spell_id, a.source, r.value);
    for (std::size_t i = 0; i < r.chat.size(); ++i)
        chat(ChatEvent::Kind::Spell, std::move(r.chat[i]), 0, i < r.chat_colors.size() ? r.chat_colors[i] : 0);
    for (std::int32_t wave : r.sounds) {
        SoundCue c;
        c.kind = SoundCue::Kind::SpawnWave;
        c.target = world_.player_id;
        c.spell = static_cast<std::uint32_t>(wave);
        sound_cues_.push_back(c);
    }
    for (int slot : r.removed_buffs) spells_.buffs[static_cast<std::size_t>(slot)] = {};
    if (r.buff_timers_on_hold) spells_.buff_timers_on_hold = true;
    for (const game::PlacedBuff& pb : r.placed_buffs) {
        // The EQ_Affect HitBySpell filled in FindAffectSlot's slot; the server's OP_Buff refreshes it.
        game::BuffSlot& b = spells_.buffs[static_cast<std::size_t>(pb.slot)];
        b = {};
        b.spell = pb.spell;
        b.tics = pb.tics;
        b.received_ms = now;
        b.level = pb.level;
        b.caster_id = pb.caster_id;
        b.modifier = pb.modifier;
        b.effect_type = pb.effect_type;
        log::debug("HitBySpell: spell {} placed in buff slot {}", pb.spell, pb.slot);
    }
    drunkenness_ = r.drunkenness;
    for (std::uint32_t kind : r.position_samples) record_position_sample(kind);
    landing_.velocity_change += r.velocity_change;
    if (r.heading_changed) landing_.heading = r.heading;
    if (r.feigned) {
        // CharacterZoneClient_StopCastAndFeignDeath @0x433b80 (SPA 74: Imitate Death, Death Peace and the
        // like): the bard twist ends, StopSpellCast(0, the casting spell), then ChangePosition(0x73), which lies
        // the player down, stops auto attack and sends OP_SpawnAppearance {me, 14, 115}. EQEmu's SE_FeignDeath
        // only marks the client feigned and sends no appearance, so without this the player stays standing.
        clear_bard_twist();
        stop_current_spell_cast();
        set_stand_state(kStandLying);
    }
}

} // namespace hailstone::net
