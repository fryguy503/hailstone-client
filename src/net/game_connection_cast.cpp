// GameConnection: casting a memorized spell. CCastSpellWnd::HandleSpellLeftClick @0x6488f0 gates the gem, then
// CharacterZoneClient::CastSpell @0x43a830 runs as game/mechanics/cast_spell.h implements it, with this file's
// CastQueries as its live adapter: each callee the original calls is answered from the client's state, and the
// ones the client has no state for are answered neutrally (docs/unknowns.md, "Spells / the cast checks").

#include <algorithm>
#include <bit>
#include <ctime>
#include <format>
#include <limits>
#include <map>

#include "core/log.h"
#include "game/coords.h"
#include "game/distance.h"
#include "game/mechanics/attack_rules.h"
#include "game/mechanics/cast_spell.h"
#include "game/mechanics/character.h"
#include "game/mechanics/combat_ability.h"
#include "game/mechanics/sheet_builder.h"
#include "game/race_models.h"
#include "net/game_connection.h"
#include "net/spell_packets.h"

namespace hailstone::net {

namespace cast = game::mechanics::cast;

namespace {

std::string base_name(std::string n) {
    while (!n.empty() && n.back() >= '0' && n.back() <= '9') n.pop_back();
    return n;
}

std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }

} // namespace

class CastQueries final : public cast::Queries {
public:
    CastQueries(GameConnection& c, cast::State& st, std::uint8_t) : c_(c), st_(st) {
        if (c.profile_) {
            game::mechanics::LiveState live;
            live.leadership = c.leadership_bonuses();
            live.drunkenness = c.drunkenness_;
            live.owner_status = c.pstatus_.owner_status;
            live.level = c.pstatus_.level;
            live.pvp_rules = c.pvp_rules_;
            live.player_id = c.world_.player_id;
            live.now_ms = GameConnection::now_ms();
            live.vitals = c.server_ext().character_vitals(c); // the NMS patch's Cur_Mana / Cur_Endurance detours
            live.focus_classes = c.focus_class_mask();
            sheet_ = game::mechanics::build_sheet(*c.profile_, c.inventory_, c.spells_, live, nullptr, &c.world_);
            sheet_->abilities = c.alt_adv_.sheet_abilities(); // the AA foci (SPA 127)
            ch_.emplace(*sheet_, no_tables_, game::mechanics::spell_lookup(*c.spell_table_));
        }
        for (std::size_t i = 0; i < affects_.size(); ++i) {
            const auto& b = c.spells_.buffs[i];
            affects_[i].active = b.empty() ? 0 : 1;
            affects_[i].spell_id = b.empty() ? 0 : static_cast<std::int32_t>(b.spell);
        }
    }

    // The caster: the player's spawn as CastSpell reads it.
    cast::Spawn* me() {
        auto [it, fresh] = spawns_.try_emplace(c_.world_.player_id);
        cast::Spawn& s = it->second;
        if (fresh) {
            const game::Spawn* p = c_.world_.player();
            s.type = 0;
            s.spawn_id = c_.world_.player_id;
            s.stand_state = c_.pstatus_.stand_state;
            s.hide_mode = c_.hidden_ ? std::max<std::int32_t>(1, c_.invis_state_) : c_.invis_state_;
            s.hp_max = static_cast<std::int32_t>(c_.own_hp().max); // Max_HP: the items' HP included
            // FloorHeight and +0x11b4 for the mount's room check (SPA 113, 4067): the movement's last floor test and
            // the ceiling its CanIFitHere last found room under; without them (no collision, the open sky) it passes.
            s.floor_height = 0;
            s.ceiling = bits(std::numeric_limits<float>::infinity());
            float ceiling = 0.0f, floor = 0.0f;
            if (c_.mount_queries().ceiling_and_floor(ceiling, floor)) {
                s.floor_height = bits(floor);
                s.ceiling = bits(ceiling);
            }
            s.height = bits(p ? p->heights.avatar_height : 0.0f);
            glm::vec3 server = game::world_to_server(c_.position_);
            s.y = bits(server.y);
            s.x = bits(server.x);
            s.z = bits(server.z);
            s.casting.spell_id =
                c_.spells_.casting.casting() ? static_cast<std::int32_t>(c_.spells_.casting.spell) : -1;
            for (std::size_t g = 0; g < s.gem_eta.size(); ++g) s.gem_eta[g] = c_.spells_.gems[g].ready; // +0x2e8
            s.cooldown_eta = c_.spells_.recovery_ready;                                                  // +0x3cc
            positions_[&s] = c_.position_;
            race_[&s] = p ? p->race : 0;
        }
        return &s;
    }

    // Another spawn by id (null when the client does not know it).
    cast::Spawn* spawn(std::uint32_t id) {
        if (id == 0) return nullptr;
        if (id == c_.world_.player_id) return me();
        auto w = c_.world_.spawns.find(id);
        if (w == c_.world_.spawns.end()) return nullptr;
        auto [it, fresh] = spawns_.try_emplace(id);
        cast::Spawn& s = it->second;
        if (fresh) {
            const game::Spawn& g = w->second;
            s.type = g.is_corpse ? 2 : (g.is_npc ? 1 : 0);
            s.spawn_id = id;
            s.stand_state = g.stand_state;
            s.master_id = g.pet_owner_id;
            s.summoned = g.is_npc && g.pet_owner_id != 0 ? 1 : 0;
            s.has_character = false; // only the own spawn has a pCharacter here
            s.height = bits(g.heights.avatar_height);
            s.z = bits(g.position.z);
            positions_[&s] = g.position;
            race_[&s] = g.race;
            names_[&s] = g.name;
            body_[&s] = g.body_type;
        }
        return &s;
    }

    // The client's spell as CastSpell reads it.
    const cast::Spell* spell(std::int32_t id) {
        const game::SpellData* d = id > 0 ? c_.spell_table_->get(static_cast<std::uint32_t>(id)) : nullptr;
        if (!d) return nullptr;
        auto [it, fresh] = spells_.try_emplace(id);
        cast::Spell& s = it->second;
        if (fresh) {
            s.id = static_cast<std::int32_t>(d->id);
            s.range = bits(d->range);
            s.min_range = bits(d->min_range);
            s.cast_time = d->cast_time;
            s.mana = d->mana;
            s.base0 = d->base[0];
            s.no_self = d->recourse_link;
            s.endurance = d->endurance_cost;
            s.pcnpc_only = d->pcnpc_only;
            s.group = d->spell_group;
            s.subgroup = d->spell_subgroup;
            s.rank = d->spell_rank;
            s.req_association = d->req_association;
            s.can_cast_in_combat = d->can_cast_in_combat;
            s.can_cast_out_of_combat = d->can_cast_out_of_combat;
            s.only_during_fast_regen = d->only_during_fast_regen;
            s.cast_not_standing = d->cast_not_standing;
            s.is_skill = d->is_skill;
            // GetSpellLevelNeeded per class as the client answers it: the stock bytes, or the server extension's
            // (the NMS patch's detour: the mask's minimum for the visible class and the mask's classes, 255 for the
            // rest).
            s.class_level.fill(0xff);
            for (std::uint8_t k = 1; k <= 16; ++k) s.class_level[k] = c_.spell_level_needed(*d, k);
            raw_bard_level_[&s] = d->class_level[8 - 1]; // the record's own Bard byte
            s.spell_type = d->spell_type;
            s.target_type = d->target_type;
            s.travel_type = d->travel_type;
            s.sneak = d->sneak;
            for (std::size_t k = 0; k < 12; ++k) s.spa[k] = d->effect[k];
            data_[&s] = d;
        }
        return &s;
    }

    bool is_valid_spell_id(std::int32_t id) override { return id > 0 && id <= static_cast<std::int32_t>(game::kMaxSpellId); }
    const cast::Spell* spell_by_id(std::int32_t id) override { return spell(id); }
    // The rank swap: PcClient's CharacterBase vtable +0x90 (@0x580300) is the player spawn's GM flag (its vtable
    // +0xdc); +0 is PcClient::GetGameFeature_ @0x5784e0, the membership's limit (feature 3, the spell tier; -1 none,
    // as EQEmu's gold membership sends).
    bool character_virtual_90() override { return c_.player_is_gm(); }
    std::int32_t character_rank(std::int32_t feature) override { return c_.game_feature(feature); }
    const cast::Spell* spell_by_group_and_rank(std::int32_t group, std::int32_t subgroup, std::int32_t rank,
                                               std::int32_t below) override {
        const game::SpellData* d = c_.spell_table_->by_group_and_rank(group, subgroup, rank, below != 0);
        return d ? spell(static_cast<std::int32_t>(d->id)) : nullptr;
    }
    // CEverQuest::IsOkToTransact (game/trade_rules.h) over the connection's windows.
    bool ok_to_transact() override { return c_.ok_to_transact(); }
    void pending_request_report(std::int32_t reason) override { log::debug("cast: pending request report {}", reason); }
    void dsp_chat(std::uint32_t eqstr, std::int32_t color, std::uint8_t, std::uint8_t) override { say(eqstr, color); }
    void change_position(cast::Spawn& who, std::uint8_t state) override {
        if (state == 100) c_.set_sitting(false);
        who.stand_state = c_.pstatus_.stand_state;
        st_.state_104 = c_.pstatus_.stand_state;
    }
    void message(std::uint32_t eqstr, std::int32_t color, std::int32_t) override { say(eqstr, color); }
    void recache_spell_effects() override {}
    std::int32_t cached_buff_effect(std::int32_t spa, std::int32_t sub) override { return ch_ ? ch_->cached_buff(spa, sub) : 0; }
    std::int32_t cached_aa_effect(std::int32_t spa, std::int32_t sub) override { return ch_ ? ch_->cached_aa(spa, sub) : 0; }
    std::int32_t cached_item_effect(std::int32_t spa, std::int32_t sub) override { return ch_ ? ch_->cached_item(spa, sub) : 0; }
    cast::Handle pc_zone() override { return 1; }
    bool in_combat() override { return c_.in_combat_; }
    bool resting_for_fast_regen() override { return c_.resting_for_fast_regen(); }
    // Item casts (cast_item): the item's click block (net::ItemEffect, ItemBase_GetSpellRecastTime / Type(0): the
    // definition's +0x298 / +0x29c, the recast delay in seconds and the recast type) and the item recast timers
    // (GameConnection::item_recast_timer), all in Util::FastTime seconds (@0x808b50 is time(0)).
    std::int32_t item_spell_recast_time(std::int32_t) override {
        return item_ ? static_cast<std::int32_t>(click().recast_delay()) : 0;
    }
    std::int32_t item_recast_timer() override { return static_cast<std::int32_t>(c_.item_recast_timer(item_)); }
    std::int32_t fast_time() override { return static_cast<std::int32_t>(std::time(nullptr)); }
    void create_target_indicator(std::uint8_t, const cast::Spell& s, std::int32_t) override {
        // CEverQuest::CreateTargetIndicator: the client has no free-target tracker yet (docs/unknowns.md).
        log::info("cast: spell {} needs a free target (target type 45), which the client cannot place yet", s.id);
    }
    std::uint8_t max_effects() override {
        int n = ch_ ? ch_->max_effects() : game::kBuffSlots;
        return static_cast<std::uint8_t>(std::min(n, game::kBuffSlots));
    }
    cast::Affect* effect(std::int32_t i) override {
        return &affects_[static_cast<std::size_t>(std::clamp(i, 0, game::kBuffSlots - 1))];
    }
    void remove_affect(cast::Affect& a, std::int32_t, std::int32_t) override {
        c_.remove_own_effect(static_cast<int>(&a - affects_.data()));
        a.active = 0;
    }
    void make_me_visible(std::int32_t, std::int32_t appearance) override { c_.make_me_visible(appearance != 0); }
    std::int32_t item_type() override { return item_ ? item_->item_type : 0; }
    std::int32_t bard_cast_bard(const cast::Spell* s, std::uint8_t class_id) override {
        // A server's client may refuse the song by the record's own Bard byte (+0x24e; the NMS patch: 255).
        if (s) {
            auto it = raw_bard_level_.find(s);
            if (it != raw_bard_level_.end() && c_.server_ext().song_blocked(c_, it->second)) return 0;
        }
        return cast::bard_cast_bard(s, class_id) ? 1 : 0;
    }
    std::int32_t item_spell_required_level(std::int32_t) override { return item_ ? click().level2 : 0; }
    std::int32_t character_level() override { return c_.pstatus_.level; }
    std::int32_t item_is_attuneable() override { return item_ && item_->attuneable ? 1 : 0; }
    std::int32_t item_spell_effect_type(std::int32_t) override { return item_ ? static_cast<std::int32_t>(click().type) : 0; }
    const cast::Item* inventory_item(std::int32_t slot) override {
        // unk_Container_GetItem on the possessions (0x21 = the cursor); the cast item itself where it is.
        const ItemSlot at = ItemSlot::possessions(static_cast<std::int16_t>(slot));
        if (item_ && cast_item_ && item_slot_.same(at)) return cast_item_;
        return c_.inventory_.at(at) ? &some_item_ : nullptr;
    }
    // CharacterZoneClient::CanUseItem(item, 1, loud) for click types 4 and 5: class, deity, level, race; item casts
    // are never loud, so quietly (GameConnection::can_use_item_full, the server extension's answer first).
    bool can_use_item(std::int32_t, std::uint8_t) override { return !item_ || c_.can_use_item_full(*item_); }
    const cast::Item* pc_item(std::int32_t) override { return c_.inventory_.cursor() ? &some_item_ : nullptr; }
    std::int32_t character_virtual_34() override { return 0; }
    cast::Spawn* pet_spawn() override { return spawn(c_.pets_.focused); }
    cast::Spawn* spawn_by_id(std::uint32_t id) override { return spawn(id); }
    bool can_see_position(cast::Spawn& s, const std::array<std::uint32_t, 3>& p) override {
        if (!c_.line_of_sight_) return true;
        glm::vec3 to = game::server_to_world({std::bit_cast<float>(p[1]), std::bit_cast<float>(p[0]),
                                              std::bit_cast<float>(p[2])});
        return c_.line_of_sight_(eye(s), to);
    }
    bool can_see(cast::Spawn& s, cast::Spawn& other) override {
        // PlayerBase::CanSee: the segment from the caster (z + 1 for races 8, 11 and 12) to the target.
        if (!c_.line_of_sight_) return true;
        const glm::vec3 from = eye(s), to = positions_[&other];
        const bool seen = c_.line_of_sight_(from, to);
        log::debug("cast: line of sight ({:.1f}, {:.1f}, {:.1f}) -> spawn {} ({:.1f}, {:.1f}, {:.1f}): {}", from.x, from.y,
                   from.z, other.spawn_id, to.x, to.y, to.z, seen ? "clear" : "blocked");
        return seen;
    }
    // PlayerBase vtable +0xdc is not identified; no spawn has it (docs/unknowns.md).
    bool spawn_virtual_dc(cast::Spawn&) override { return false; }
    // PlayerZoneClient::NotOnSameDeity (with a spell): the deity teams of table 0x59e9dc (PvP rule set 4).
    std::int32_t not_on_same_deity(cast::Spawn& self, cast::Spawn& other, const cast::Spell&) override {
        auto deity = [&](const cast::Spawn& s) -> std::uint32_t {
            auto it = c_.world_.spawns.find(s.spawn_id);
            return it == c_.world_.spawns.end() ? 0 : it->second.deity;
        };
        game::mechanics::AttackSpawn a, b;
        a.deity = deity(self);
        b.deity = deity(other);
        return game::mechanics::not_on_same_deity(&a, &b, true, false) ? 1 : 0;
    }
    // Cur_Mana(1) / Cur_Endurance(1): the server's values (the sheet has no class tables to clamp them with), or a
    // server's client patch's answer (the NMS patch's detours: OP_ServerAuthStats keys 3 / 4).
    std::int32_t cur_mana(std::int32_t arg) override {
        if (ch_ && sheet_->vitals.cur_mana) return ch_->cur_mana(arg != 0);
        return static_cast<std::int32_t>(c_.pstatus_.mana);
    }
    std::int32_t cur_endurance(std::int32_t arg) override {
        if (ch_ && sheet_->vitals.cur_endurance) return ch_->cur_endurance(arg != 0);
        return static_cast<std::int32_t>(c_.pstatus_.endurance);
    }
    // SpellReqAssociationID (SpellManager vtable +8): the client has no requirement data (docs/unknowns.md).
    bool spell_allowed(cast::Spawn*, std::int32_t) override { return true; }
    // GetFocusRangeModifier: not ported yet (docs/unknowns.md).
    std::int32_t focus_range_modifier(const cast::Spell&) override { return 0; }
    std::uint32_t player_distance(cast::Spawn& from, cast::Spawn& to, std::uint32_t) override {
        // CDisplay::PlayerDistance with no z scale: the 2-D distance.
        if (&from == &to) return 0;
        glm::vec3 a = positions_[&from], b = positions_[&to];
        return bits(game::player_distance(&a, &b, 0.0f));
    }
    cast::Handle spawn_character_pc(cast::Spawn& s) override { return s.spawn_id == c_.world_.player_id ? 1 : 0; }
    // 0x59ed50 (target type 12, a race list) is not ported (docs/unknowns.md): it passes.
    bool target_type_12_check(cast::Spawn&) override { return true; }
    bool is_self_or_group_member(cast::Handle pc, const cast::Spawn* who) override {
        if (!who) return pc == 1; // the caster's own name in the target's PcClient: the target is the caster
        if (who->spawn_id == c_.world_.player_id) return true;
        auto n = names_.find(who);
        return n != names_.end() && c_.group_.has_member(base_name(n->second));
    }
    bool has_property(cast::Spawn& s, std::int32_t property, std::int32_t, std::int32_t) override {
        // PlayerBase::HasProperty for the body-type properties: the spawn's body type.
        auto b = body_.find(&s);
        return b != body_.end() && static_cast<std::int32_t>(b->second) == property;
    }
    std::int32_t item_base_type() override { return item_ ? item_->item_class : 0; }
    std::int32_t item_spell_id(std::int32_t) override { return item_ ? click().effect : 0; }
    std::int32_t largest_cached_effect(std::int32_t, std::int32_t) override { return 0; } // the result is unused
    std::int32_t casting_time_modifier(const cast::Spell& s) override {
        return ch_ ? ch_->casting_time_modifier(*data_[&s]) : 0;
    }
    std::int32_t focus_casting_time_modifier(const cast::Spell& s, std::int32_t arg3) override {
        return ch_ ? ch_->focus_casting_time_modifier(*data_[&s], arg3 != 0) : 0;
    }
    void start_casting(std::int32_t, std::uint16_t, std::uint32_t) override {}
    void launch_spell_data_reset() override { c_.spells_.casting = {}; }
    std::int32_t item_id() override { return item_ ? static_cast<std::int32_t>(item_->id) : 0; }
    std::int32_t item_spell_cast_time(std::int32_t) override { return item_ ? click().cast_time() : 0; }
    std::int32_t item_spell_recast_type(std::int32_t) override { return item_ ? click().recast_type() : 0; }
    void set_item_recast_timer(std::int32_t time, std::int32_t type) override {
        c_.set_item_recast_timer(item_ ? c_.inventory_.at(item_slot_) : nullptr, time, type);
    }

    // The item a cast comes from (cast_item): its record, where it is, and CastSpell's view of it.
    void set_item(const Item* item, const ItemSlot& at, const cast::Item* view) {
        item_ = item;
        item_slot_ = at;
        cast_item_ = view;
    }
    std::uint32_t spell_crc(std::int32_t id) override { return c_.spell_table_->spell_crc(static_cast<std::uint32_t>(id)); }
    void hton(std::uint32_t, std::int32_t) override {}
    bool send(std::int32_t, const cast::Packet& p, std::int32_t) override {
        c_.send_zone(encode(cast_spell_packet(p)));
        return true;
    }
    void operator_delete(std::int32_t) override {}
    void close_connection() override {}
    void debug_log(std::uint32_t opcode) override { log::warn("attempt to send message {} on a void connection", opcode); }
    void push_interact_reason(std::int32_t) override {}
    void push_pending_reason(std::int32_t) override {}

private:
    void say(std::uint32_t eqstr, std::int32_t color) {
        auto index = static_cast<std::uint32_t>(color); // dsp_chat's colour (0x121 spell failures, ...)
        if (color == 0x121) c_.spell_message(eqstr, {}, index);
        else c_.system(eqstr, {}, index);
    }
    glm::vec3 eye(const cast::Spawn& s) {
        glm::vec3 from = positions_[&s];
        std::uint16_t race = race_[&s];
        if (race == 8 || race == 11 || race == 12) from.z += 1.0f;
        return from;
    }

    GameConnection& c_;
    cast::State& st_;
    std::map<const cast::Spell*, std::uint8_t> raw_bard_level_; // the record's own Bard byte (server_ext().song_blocked)
    static inline const game::mechanics::GameTables no_tables_{}; // the checks read no tables
    std::optional<game::mechanics::CharacterSheet> sheet_;
    std::optional<game::mechanics::Character> ch_;
    std::map<std::uint32_t, cast::Spawn> spawns_;
    std::map<std::int32_t, cast::Spell> spells_;
    std::map<const cast::Spawn*, glm::vec3> positions_;
    std::map<const cast::Spawn*, std::uint16_t> race_;
    std::map<const cast::Spawn*, std::string> names_;
    std::map<const cast::Spawn*, std::uint32_t> body_;
    std::map<const cast::Spell*, const game::SpellData*> data_;
    std::array<cast::Affect, game::kBuffSlots> affects_{};
    cast::Item some_item_{};
    const Item* item_ = nullptr;
    ItemSlot item_slot_ = ItemSlot::invalid();
    const cast::Item* cast_item_ = nullptr;
    const ItemEffect& click() const { return item_->effects[kEffectClick]; }
};

void GameConnection::cast_gem(int gem) {
    if (state_ != ConnectionState::InZone || pstatus_.dead || gem < 0 || gem >= game::kSpellGems) return;
    const std::uint32_t id = spells_.gems[static_cast<std::size_t>(gem)].spell;
    if (id == game::kNoSpell || !spell_table_->get(id)) return;
    // HandleSpellLeftClick @0x6488f0.
    if (!spells_.gem_castable(gem, eq_time()) || spells_.casting.casting()) {
        // A bard song stays cast while it plays (the server's OP_ManaChange keeps it going): a click on any gem
        // stops it (ClearBardTwist, StopCurrentSpellCast(0)) and the next click casts.
        const game::SpellData* playing =
            spells_.casting.casting() ? spell_table_->get(spells_.casting.spell) : nullptr;
        if (playing && bard_song(*playing)) {
            clear_bard_twist();
            stop_current_spell_cast();
            return;
        }
        spell_message(12029); // You haven't recovered yet...
        return;
    }
    if (spells_.pending > 0) {
        // The original refuses silently and reports the pending requests (opcode 0x57c6).
        log::debug("cast refused: {} request(s) pending", spells_.pending);
        return;
    }
    cast_spell_slot(gem, id);
}

// The callees of doCombatAbility and Cmd_discipline over the connection.
class CombatAbilityQueries final : public game::mechanics::combat_ability::Queries {
public:
    explicit CombatAbilityQueries(GameConnection& c) : c_(c) {}

    const game::mechanics::combat_ability::Spell* spell_by_id(std::int32_t id) override {
        const game::SpellData* d = id > 0 ? c_.spell_table_->get(static_cast<std::uint32_t>(id)) : nullptr;
        if (!d) return nullptr;
        auto [it, fresh] = spells_.try_emplace(id);
        auto& s = it->second;
        if (fresh) {
            s.id = static_cast<std::int32_t>(d->id);
            s.name = d->name;
            s.recast_time = d->recast_time;
            s.duration_formula = static_cast<std::int32_t>(d->duration_formula);
            s.deity_mask = d->deity_mask;
            s.timer = c_.combat_ability_timer(*d);
            s.group = d->spell_group;
            s.subgroup = d->spell_subgroup;
            s.rank = d->spell_rank;
            s.is_skill = d->is_skill ? 1 : 0;
            s.target_type = d->target_type;
            data_[&s] = d;
        }
        return &s;
    }
    // The rank swap, as in CastSpell (the GM flag, GetGameFeature_(3), GetSpellByGroupAndRank).
    const game::mechanics::combat_ability::Spell* spell_by_group_and_rank(std::int32_t group, std::int32_t subgroup,
                                                                          std::int32_t rank,
                                                                          std::int32_t below) override {
        const game::SpellData* d = c_.spell_table_->by_group_and_rank(group, subgroup, rank, below != 0);
        return d ? spell_by_id(static_cast<std::int32_t>(d->id)) : nullptr;
    }
    std::int32_t combat_ability(std::int32_t index) override {
        return static_cast<std::int32_t>(c_.combat_ability(index));
    }
    bool character_virtual_90() override { return c_.player_is_gm(); }
    std::int32_t character_rank(std::int32_t feature) override { return c_.game_feature(feature); }
    std::uint8_t spell_level_needed(const game::mechanics::combat_ability::Spell& s, std::uint32_t class_id) override {
        return c_.spell_level_needed(*data_.at(&s), class_id);
    }
    std::uint32_t class_id() override { return c_.visible_class(); }
    std::int32_t level() override { return c_.pstatus_.level; }
    std::uint32_t deity() override { return c_.profile_ ? c_.profile_->deity : 0; }
    // AltAttack (PlayerZoneClient +0x340): the original's code only clears it (here, DoAttack) and copies it
    // (0x598480, the spawn copy); nothing sets it, so it is always 0.
    bool take_alt_attack() override { return false; }
    std::uint32_t combat_ability_timer(std::int32_t id) override {
        return id >= 0 && id < game::mechanics::combat_ability::kTimers
                   ? c_.spells_.discipline_ready[static_cast<std::size_t>(id)]
                   : 0;
    }
    std::uint32_t discipline_reuse_timer(std::int32_t id) override { return c_.discipline_reuse_timer(id); }
    std::uint32_t fast_time() override { return static_cast<std::uint32_t>(std::time(nullptr)); }
    // GetOpenEffectSlot(0, song 1, -1) == -1: a player's song search covers the last slot only (62, the
    // discipline's).
    bool discipline_active() override { return !c_.spells_.buffs[game::kBuffSlots - 1].empty(); }
    void eqstr_template(std::uint32_t eqstr, std::int32_t color, const std::vector<std::string>& args) override {
        c_.system(eqstr, args, static_cast<std::uint32_t>(color));
    }
    void message(std::uint32_t eqstr, std::int32_t color) override {
        c_.system(eqstr, {}, static_cast<std::uint32_t>(color));
    }
    void dsp_chat_2(std::uint32_t eqstr) override { c_.system(eqstr); }
    void cast_spell(std::int32_t spell_id) override {
        c_.cast_spell_slot(static_cast<int>(game::mechanics::combat_ability::kCastSlot),
                           static_cast<std::uint32_t>(spell_id));
    }
    // /discipline goes through GameConnection::do_combat_ability, so the server extension's detour runs around it.
    void use_combat_ability(std::int32_t spell_id) override {
        c_.do_combat_ability(static_cast<std::uint32_t>(spell_id), true);
    }

private:
    GameConnection& c_;
    std::map<std::int32_t, game::mechanics::combat_ability::Spell> spells_;
    std::map<const game::mechanics::combat_ability::Spell*, const game::SpellData*> data_;
};

bool GameConnection::do_combat_ability(std::uint32_t spell, bool rank) {
    if (state_ != ConnectionState::InZone || !profile_) return false;
    const game::SpellData* d = spell_table_->get(spell);
    // The server extension's detour runs around the original (NMS: the reuse timers from 20 up).
    if (d)
        if (auto answer = server_ext().before_combat_ability(*this, *d)) return *answer;
    CombatAbilityQueries q(*this);
    const bool used = game::mechanics::combat_ability::do_combat_ability(static_cast<std::int32_t>(spell), rank, q);
    if (d) server_ext().after_combat_ability(*this, *d, used);
    return used;
}

void GameConnection::discipline_command(const std::string& args) {
    if (state_ != ConnectionState::InZone || !profile_) return;
    CombatAbilityQueries q(*this);
    game::mechanics::combat_ability::discipline_command(args, q);
}

std::int32_t GameConnection::combat_ability_timer(const game::SpellData& s) const {
    return server_ext().combat_ability_timer(*this, s);
}

std::uint32_t GameConnection::discipline_reuse_timer(std::int32_t id) const {
    if (auto t = server_ext().discipline_reuse_timer(*this, id)) return *t;
    // GetDisciplineReuseTimer @0x57f810: ids 0..19 (unsigned), else 0.
    return static_cast<std::uint32_t>(id) < spells_.discipline_ready.size()
               ? spells_.discipline_ready[static_cast<std::size_t>(id)]
               : 0;
}

void GameConnection::cast_spell_slot(int gem, std::uint32_t id, const ItemSlot* item_slot, bool keep_target,
                                     bool send) {
    const auto now = now_ms();
    // CharacterZoneClient::CastSpell(gem, spell, no item, ..., send, no position).
    cast::State st;
    // The class CastSpell reads (the profile's +0x3374): the extension's while it spoofs it (NMS: the cast spoof).
    const std::uint8_t class_id = visible_class();
    CastQueries q(*this, st, class_id);
    st.caster = q.me();
    st.local_player = st.caster;
    st.target = q.spawn(world_.target_id);
    st.is_local_pc = true;
    st.local_block = attack_locked_ ? 1 : 0; // 0xddf70c
    st.auto_stand = client_option(game::ClientOption::AllowAutoStand) ? 1 : 0; // 0xde0d5b
    st.now = static_cast<std::uint32_t>(now);
    st.effects_cached = 1;
    st.stunned = stunned() ? 1 : 0;
    st.state_104 = pstatus_.stand_state;
    st.class_id = class_id;
    st.zone_type = pvp_rules_; // 0xdcee70: rule set 4 refuses beneficial spells on the other deity team (210)
    cast::Args a;
    a.slot = static_cast<std::uint8_t>(gem);
    a.spell_id = static_cast<std::int32_t>(id);
    a.item_location = {0xffffffffu, 0xffffffffu, 0x0000ffffu}; // no item: the gem's invalid index (all -1)
    a.send_packet = send ? 1 : 0;
    cast::Item view{};
    if (item_slot) {
        if (const Item* item = inventory_.at(*item_slot)) {
            // ItemGlobalIndex as the InventorySlot it goes out as: u32 location, slot | sub << 16, aug.
            const ItemSlot& s = *item_slot;
            a.item_location = {static_cast<std::uint32_t>(static_cast<std::uint16_t>(s.type)),
                               static_cast<std::uint32_t>(static_cast<std::uint16_t>(s.slot)) |
                                   static_cast<std::uint32_t>(static_cast<std::uint16_t>(s.sub)) << 16,
                               static_cast<std::uint32_t>(static_cast<std::uint16_t>(s.aug))};
            view.charges = static_cast<std::int32_t>(item->charges);
            view.attuned = static_cast<std::int32_t>(item->attuned);
            a.item = &view;
            q.set_item(item, s, &view);
        }
    }
    cast::Spawn* const target_before = st.target;
    std::optional<cast::Packet> sent;
    const std::uint8_t ok = cast::cast_spell(st, a, q, &sent);

    // pinstTarget: the original retargets pet and owner spells.
    if (st.target != target_before && !keep_target) set_target(st.target ? st.target->spawn_id : 0);
    if (!ok) {
        log::debug("cast {} from {} {}: refused", id, item_slot ? "item slot" : "gem", item_slot ? item_slot->slot : gem + 1);
        return;
    }
    const cast::Spawn& me = *st.caster;
    if (me.casting.spell_id != -1 && me.casting.eta != 0) {
        const std::uint32_t cast_ms = me.casting.eta - st.now;
        spells_.casting = {};
        spells_.casting.eta_ms = now + cast_ms;
        spells_.casting.spell = static_cast<std::uint32_t>(me.casting.spell_id);
        spells_.casting.slot = me.casting.slot;
        spells_.casting.cast_time = cast_ms;
        spells_.casting.target = me.casting.target_id;
        spells_.casting.item_location = me.casting.item_location;
        spells_.casting.item_id = me.casting.item_id;
        spells_.casting.item_cast_type = me.casting.item_cast_type;
    }
    // CastSpell's lock until the server answers (@0x43c76b): the gem timer and the recovery lock at now + 10000.
    for (std::size_t g = 0; g < me.gem_eta.size(); ++g) spells_.gems[g].ready = me.gem_eta[g];
    spells_.recovery_ready = me.cooldown_eta;
    spells_.pending += st.item_pending; // IsItemPending: the server's OP_ManaChange answers it
    if (sent)
        log::info("cast {} from {} {} on {} ({} ms, spell crc {:#010x})", id, item_slot ? "item slot" : "gem",
                  item_slot ? item_slot->slot : gem + 1, sent->target_id,
                  spells_.casting.casting() ? spells_.casting.cast_time : 0, sent->crc);
    else
        log::debug("cast {}: started, nothing sent", id);
}

} // namespace hailstone::net
