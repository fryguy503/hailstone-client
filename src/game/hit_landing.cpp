// A spell landing on the player (game/hit_landing.h).
#include "game/hit_landing.h"

#include <bit>
#include <cstring>
#include <deque>
#include <random>
#include <unordered_map>

#include "game/coords.h"
#include "game/distance.h"
#include "game/eq_strings.h"
#include "game/mechanics/character.h"
#include "game/mechanics/find_affect_slot.h"
#include "game/sine_table.h"
#include "game/spells.h"
#include "game/world_state.h"

namespace hailstone::game {

namespace hit = mechanics::hit;

namespace {

std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }
float as_float(std::uint32_t b) { return std::bit_cast<float>(b); }

template <std::size_t N> void copy_text(std::array<char, N>& to, const std::string& from) {
    std::size_t n = std::min(from.size(), N - 1);
    std::memcpy(to.data(), from.data(), n);
    to[n] = 0;
}

// The client's state behind HitBySpell's callees, and FindAffectSlot's.
class Landing final : public hit::HitQueries, public mechanics::affect::AffectQueries {
public:
    Landing(const HitLandingContext& ctx, HitLandingResult& out) : ctx_(ctx), out_(out), rng_(ctx.now_ms) {
        const WorldState& w = *ctx.world;
        if (const Spawn* p = w.player()) snapshot(*p, me_);
        me_.type = 0;
        me_.character = &self_;
        self_.cache_valid = 1;
        self_.spawn = &me_;
        self_.base = &cb_;
        cb_.y = me_.y;
        cb_.x = me_.x;
        cb_.z = me_.z;
        cb_.heading = me_.heading;
        glm::vec3 v = ctx.velocity;
        me_.speed_y = bits(v.x); // +0x70 moves +0x64, the world x (heading_cos)
        me_.speed_x = bits(v.y);
        me_.speed_z = bits(v.z);
        profile_.class_id = ctx.class_id;
        profile_.mana = ctx.mana;
        profile_.weight = ctx.weight;
        drunk_ = ctx.drunkenness;
        const SpellState& s = *ctx.spell_state;
        for (std::size_t i = 0; i < s.buffs.size(); ++i) {
            const BuffSlot& b = s.buffs[i];
            hit::Affect& a = buffs_[i];
            a.type = b.empty() ? 0 : b.effect_type;
            a.level = b.level;
            a.spell_id = b.empty() ? 0 : static_cast<std::int32_t>(b.spell);
            a.modifier = bits(b.modifier);
            a.duration = b.tics;
            caster_ids_[i] = b.empty() ? 0 : b.caster_id;
            a.slot_data = b.slot_data; // the counters left; a cure spends them (HitBySpell's counter case)
            a.counters_known = b.counters_known;
        }
    }

    // CDisplay::PlayerDistance(spawn, me, 0): the 2D distance of the snapshots' +0x64 / +0x68; none without a spawn.
    std::optional<float> distance_to_me(const hit::Spawn* s) const {
        if (!s) return std::nullopt;
        return true_distance({as_float(s->y), as_float(s->x), 0.0f}, {as_float(me_.y), as_float(me_.x), 0.0f}, 0.0f);
    }

    hit::Character& self() { return self_; }
    hit::Spawn& me() { return me_; }
    std::int32_t drunkenness() const { return drunk_; }

    // ---- virtuals
    const hit::Spell* spell_by_id(std::uint32_t id) override {
        if (auto it = spells_.find(id); it != spells_.end()) return &it->second;
        const SpellData* d = ctx_.spells ? ctx_.spells->get(id) : nullptr;
        if (!d) return nullptr;
        hit::Spell s;
        s.range = bits(d->range);
        s.push_back = bits(d->push_back);
        s.push_up = bits(d->push_up);
        s.duration_formula = d->duration_formula;
        s.base = d->base;
        s.base2 = d->base2;
        s.max = d->max;
        s.attrib = d->effect;
        s.timer_id_198 = static_cast<std::uint32_t>(d->reuse_timer_index);
        s.flag_240 = d->uninterruptable ? 1 : 0;
        s.spell_type = d->spell_type;
        s.target_type = d->target_type;
        s.id = static_cast<std::int32_t>(d->id);
        s.calc = d->formula;
        for (std::size_t c = 1; c <= d->class_level.size(); ++c) s.class_level[c] = d->class_level[c - 1];
        s.short_buff = d->duration_window ? 1 : 0;
        s.not_stackable_dot = d->not_stackable_dot ? 1 : 0;
        s.no_dispell = d->no_dispell ? 1 : 0;
        s.is_skill = d->is_skill ? 1 : 0;
        s.stacks_with_self = d->stacks_with_self ? 1 : 0;
        s.no_remove = d->no_remove ? 1 : 0;
        copy_text(s.name, d->name);
        copy_text(s.teleport_zone, d->teleport_zone);
        return &spells_.emplace(id, s).first->second;
    }
    hit::Handle cb_pc() override { return reinterpret_cast<hit::Handle>(&pc_); }
    std::uint32_t cb_level() override { return ctx_.level; }
    std::uint32_t cb_max_mana() override {
        return ctx_.character ? static_cast<std::uint32_t>(ctx_.character->max_mana()) : 0u;
    }
    std::uint8_t spawn_vf_dc(hit::Spawn*) override { return 0; }
    void pc_base_vf58(hit::Handle) override {}
    std::uint8_t point_in_world(const std::array<std::uint32_t, 3>&) override { return 0; }
    std::uint8_t actor_collision_enabled() override { return 0; }
    void set_actor_collision(std::uint8_t) override {}
    std::uint8_t collide_sphere(hit::CollisionSphere&) override { return 0; }
    std::uint8_t terrain_collision_enabled() override { return 0; }
    std::uint8_t collide_line(hit::CollisionLine&) override { return 0; }
    double floor_height(hit::Spawn*, std::uint32_t, std::uint32_t, std::uint32_t) override {
        return static_cast<double>(-1e27f);
    }
    double table_cos(std::uint32_t angle) override { return cmath_cos(as_float(angle)); }
    double table_sin(std::uint32_t angle) override { return cmath_sin(as_float(angle)); }

    // ---- functions
    hit::Spawn* spawn_by_id(std::uint32_t id) override {
        const WorldState& w = *ctx_.world;
        if (id == w.player_id) return &me_;
        auto it = w.spawns.find(id);
        if (it == w.spawns.end()) return nullptr;
        others_.emplace_back();
        snapshot(it->second, others_.back());
        return &others_.back();
    }
    void recache_spell_effects(hit::Character*) override {}
    std::int32_t cached_buff_effect(hit::Character* c, std::int32_t spa) override {
        return c == &self_ && ctx_.character ? ctx_.character->cached_buff(spa, 0) : 0;
    }
    std::int32_t cached_aa_effect(hit::Character* c, std::int32_t spa) override {
        return c == &self_ && ctx_.character ? ctx_.character->cached_aa(spa, 0) : 0;
    }
    std::int32_t cached_item_effect(hit::Character* c, std::int32_t spa) override {
        return c == &self_ && ctx_.character ? ctx_.character->cached_item(spa, 0) : 0;
    }
    std::uint32_t bard_cast_bard(const hit::Spell* s, std::uint32_t class_id) override {
        return mechanics::affect::bard_cast_bard(s, class_id);
    }
    std::uint8_t sub_00433830(const hit::Spell*, hit::Character*) override { return 0; }
    hit::Handle character_of(hit::Spawn* s) override {
        if (s == &me_) return reinterpret_cast<hit::Handle>(&pc_);
        return s && s->type == 0 ? reinterpret_cast<hit::Handle>(&other_pc_) : 0;
    }
    void token_to_client(hit::Handle to, std::uint32_t id, std::uint32_t color) override {
        // To this character (self, or its PcClient's character part): a chat line.
        if (to == reinterpret_cast<hit::Handle>(&self_) || to == reinterpret_cast<hit::Handle>(&pc_) + hit::kPcCharacter)
            say(id, {}, color);
    }
    std::uint8_t is_item_cast(const hit::Launch*) override { return 0; }
    std::uint8_t is_self_or_group_member(hit::Handle, const char*) override { return 1; }
    const char* pc_name(hit::Handle) override { return ""; }
    std::uint32_t do_spell_check(std::uint32_t, hit::SpellCheckRecord&) override { return 1; }
    void log_exit_game() override {}
    void execute_cmd(std::uint32_t) override {}
    std::uint8_t max_effects(hit::Character* c) override {
        return c == &self_ ? static_cast<std::uint8_t>(buffs_.size()) : 0;
    }
    hit::Affect* get_effect(hit::Character*, std::uint32_t i) override { return &buffs_[i % buffs_.size()]; }
    void remove_affect(hit::Affect* a) override {
        int slot = slot_of(a);
        if (slot >= 0) {
            *a = hit::Affect{};
            out_.removed_buffs.push_back(slot);
        }
    }
    void cur_hp(std::uint32_t) override {}
    std::uint8_t is_pc_or_mercenary(hit::Spawn* s) override { // Spawn_IsPCOrMercenary @0x4325b0
        if (!s) return 0;
        return s->type != 0 && s->flag_208 == 0 ? 0 : 1;
    }
    std::int32_t pvp_spell_duration(const hit::Spell* s, std::uint32_t level) override { return duration(s, level); }
    std::int32_t spell_duration(const hit::Spell* s, std::uint32_t level) override { return duration(s, level); }
    std::int32_t calc_affect_change(const hit::Spell* s, std::uint32_t level, std::uint32_t index, hit::Spawn* caster,
                                    std::uint32_t flag) override {
        const SpellData* d = data(s);
        index &= 0xff;
        if (!d || index >= 12) return 0;
        if (!ctx_.character) return d->base[index];
        std::int32_t v = ctx_.character->calc_affect_change(*d, level & 0xff, static_cast<int>(index), nullptr,
                                                            d->base[index], flag != 0);
        // CalcAffectChange @0x436670: scaled by the caster's distance; no caster (and buff index -1): unscaled.
        if (!caster) return v;
        return ctx_.character->scale_by_distance(*d, v, distance_to_me(caster));
    }
    void build_string(char* out, std::uint32_t id, const char* arg) override {
        std::string t = ctx_.strings ? ctx_.strings->format(id, std::vector<std::string>{arg ? arg : ""}) : "";
        std::size_t n = std::min<std::size_t>(t.size(), 0xff);
        std::memcpy(out, t.data(), n);
        out[n] = 0;
    }
    void dsp_chat(const char* text, std::uint32_t color) override {
        out_.chat.emplace_back(text ? text : "");
        out_.chat_colors.push_back(color);
    }
    void dsp_chat_2(const char* text) override {
        out_.chat.emplace_back(text ? text : "");
        out_.chat_colors.push_back(0);
    }
    std::uint8_t get_level(hit::Spawn* s) override { return s == &me_ ? ctx_.level : level_of(s); }
    hit::Profile* current_profile() override { return &profile_; }
    void record_position_sample(hit::Spawn* s, std::uint32_t kind) override {
        if (s == &me_) out_.position_samples.push_back(kind);
    }
    hit::Spawn* vehicle_of(hit::Spawn*) override { return nullptr; }
    hit::Spawn* mount_spawn(hit::Spawn*) override { return nullptr; }
    std::int32_t get_drunkenness(hit::Handle) override { return drunk_; }
    void set_drunkenness(hit::Handle, std::int32_t v) override { drunk_ = v; }
    std::int32_t random_percent() override { return 1 + static_cast<std::int32_t>(rng_() % 100); }
    std::uint8_t has_property(hit::Spawn*, std::uint32_t) override { return 0; }
    std::uint32_t get_150(hit::Spawn*) override { return 0; }
    std::uint32_t bind_spot_ok(hit::Spawn*) override { return 1; }
    std::uint32_t expansion_zone(std::int32_t) override { return 1; }
    void set_bind_zone(hit::Handle, std::uint32_t, std::int32_t) override {}
    void set_bind_y(hit::Handle, std::uint32_t, std::uint32_t) override {}
    void set_bind_x(hit::Handle, std::uint32_t, std::uint32_t) override {}
    void set_bind_z(hit::Handle, std::uint32_t, std::uint32_t) override {}
    void set_bind_heading(hit::Handle, std::uint32_t, std::uint32_t) override {}
    void save_pc() override {}
    void set_150(hit::Spawn*) override {}
    // PlayerClient::PlaySound: the Teleport wave (0x89) of SPA 83 / 145 and a valid SPA 379, at the player.
    void play_sound(hit::Spawn*, std::uint32_t sound) override { out_.sounds.push_back(static_cast<std::int32_t>(sound)); }
    std::int32_t zone_index(const char*) override { return self_.base->zone_id; } // stays: the server moves us
    void move_to_zone(std::int32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) override {}
    void update_player_actor(hit::Spawn*) override {}
    void sub_00432bc0(std::uint32_t) override {}
    std::uint16_t hton(std::uint32_t op) override { return static_cast<std::uint16_t>(op); }
    std::uint8_t send_world(std::uint32_t) override { return 1; }
    void close_world() override {}
    void log_void_connection(std::uint32_t) override {}
    const char* get_string(std::uint32_t id) override {
        texts_.push_back(ctx_.strings ? ctx_.strings->format(id, std::vector<std::string>{}) : "");
        return texts_.back().c_str();
    }
    void set_endurance(std::int32_t) override {}
    void cur_endurance() override {}
    void cur_mana() override {}
    // CharacterZoneClient_StopCastAndFeignDeath @0x433b80 (SPA 74) ignores both: with the spawn it clears the bard
    // twist, StopSpellCast(0, the casting spell) and ChangePosition(0x73). The connection does those.
    void feign(std::uint32_t, std::int32_t) override { out_.feigned = true; }
    hit::Handle get_item(std::uint32_t) override { return 0; }
    void release_item() override {}
    const char* lore_name(hit::Handle) override { return ""; }
    std::int32_t item_type(hit::Handle) override { return 0; }
    std::uint32_t book_lang(hit::Handle) override { return 0; }
    const char* lang_desc(std::uint32_t) override { return ""; }
    std::uint8_t is_magic(hit::Handle) override { return 0; }
    hit::Affect* pc_spell_affect(std::int32_t spa) override {
        for (auto& a : buffs_) {
            if (a.type == 0) continue;
            const hit::Spell* s = spell_by_id(static_cast<std::uint32_t>(a.spell_id));
            if (s && hit::spell_affects(s, spa)) return &a;
        }
        return nullptr;
    }
    void remove_pc_affect(hit::Affect* a, std::uint32_t) override { remove_affect(a); }
    std::int32_t eq_get_time() override { return static_cast<std::int32_t>(ctx_.now_ms); }
    std::int32_t memorized_spell(hit::Handle, std::uint32_t gem) override {
        return gem < ctx_.spell_state->gems.size() ? static_cast<std::int32_t>(ctx_.spell_state->gems[gem].spell) : -1;
    }
    std::int32_t spell_focus_pct(const hit::Spell*, const hit::Spell*) override { return 0; }
    void set_spell_recast_timer(hit::Handle, std::uint32_t) override {}
    std::uint32_t fast_time() override { return ctx_.now_ms; }
    void set_linked_reuse(hit::Handle, std::uint32_t, std::uint32_t) override {}
    std::int32_t random_below(std::uint32_t n) override { return n ? static_cast<std::int32_t>(rng_() % n) : 0; }
    void stop_current_spell_cast() override {}
    void apply_fling(hit::Spawn*, const hit::FlingRecord&) override {}
    std::uint8_t is_valid_teleport(hit::Spawn*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                   std::uint32_t) override {
        return 0;
    }
    std::uint8_t find_buff_level(std::uint32_t id) override {
        for (auto& a : buffs_)
            if (a.type != 0 && static_cast<std::uint32_t>(a.spell_id) == id) return a.level;
        return 0;
    }
    // FindAffectSlot(id, caster, &slot, 0, level, 0, 0, launch) (game/mechanics/find_affect_slot.h).
    hit::Affect* find_affect_slot(std::uint32_t id, hit::Spawn* caster, std::int32_t* slot, std::uint32_t level,
                                  std::uint32_t) override {
        hit::Affect* a = mechanics::affect::find_affect_slot(&self_, static_cast<std::int32_t>(id), caster, slot, 0,
                                                             level, *this);
        placed_ = a;
        return a;
    }
    void set_buff_caster_id(std::uint32_t slot, std::uint32_t id) override {
        if (slot < caster_ids_.size()) caster_ids_[slot] = id;
    }

    // ---- FindAffectSlot's callees (mechanics::affect::AffectQueries)
    const hit::Spell* spell_by_id(std::int32_t id) override {
        return id > 0 ? spell_by_id(static_cast<std::uint32_t>(id)) : nullptr;
    }
    hit::Affect* effect_at(std::int32_t slot) override {
        // The profile has 97 effects; the client keeps a player's 63 (GetMaxEffects), the rest stay empty.
        if (slot >= 0 && static_cast<std::size_t>(slot) < buffs_.size()) return &buffs_[static_cast<std::size_t>(slot)];
        unused_ = hit::Affect{};
        return &unused_;
    }
    std::uint32_t buff_caster_id(std::int32_t slot) override {
        return slot >= 0 && static_cast<std::size_t>(slot) < caster_ids_.size() ? caster_ids_[static_cast<std::size_t>(slot)] : 0;
    }
    std::uint32_t extra_buff_slots() override {
        return mechanics::affect::pc_extra_buff_slots(static_cast<std::int32_t>(ctx_.level));
    }
    std::int32_t calc_affect_change(const hit::Spell* s, std::uint32_t level, std::uint32_t slot,
                                    std::int32_t buff_index) override {
        // CalcAffectChange(spell, level, slot, 0, buff, 0, 0, -1, 1): CalcAffectChangeGeneric with the slot's base,
        // scaled by the distance of the buff's caster (BuffCasterIDs[buff] by GetSpawnByID; not found: the far
        // modifier).
        const SpellData* d = data(s);
        slot &= 0xff;
        if (!d || slot >= 12) return 0;
        if (!ctx_.character) return d->base[slot];
        std::int32_t v =
            ctx_.character->calc_affect_change(*d, level & 0xff, static_cast<int>(slot), nullptr, d->base[slot], true);
        if (buff_index == -1) return v;
        return ctx_.character->scale_by_distance(*d, v, distance_to_me(spawn_by_id(buff_caster_id(buff_index))));
    }
    // The buff FindAffectSlot gave HitBySpell, if any.
    const hit::Affect* placed() const { return placed_; }
    // By comparing addresses, not subtracting them: effect_at hands out &unused_ for a slot past the array, and
    // a pointer from outside the array may not be subtracted from one inside it.
    int slot_of(const hit::Affect* a) const {
        for (std::size_t i = 0; i < buffs_.size(); ++i)
            if (a == &buffs_[i]) return static_cast<int>(i);
        return -1;
    }
    std::uint32_t caster_id_of(int slot) const { return caster_ids_[static_cast<std::size_t>(slot)]; }
    // HitBySpell's counter case writes the counters left into the buff's EQ_Affect; the client's buff state keeps
    // them (a cure must lower the counters the icon shows, and a weaker cure must not look like an empty buff).
    void keep_slot_data() {
        for (std::size_t i = 0; i < buffs_.size(); ++i) {
            if (&buffs_[i] == placed_ && placed_->type != 0) continue; // carried by PlacedBuff instead
            ctx_.spell_state->buffs[i].slot_data = buffs_[i].slot_data;
        }
    }
    std::uint8_t has_buff_flag(const hit::Spell*) override { return 0; }
    void on_new_buff(std::uint32_t) override {}
    std::int32_t refused_mez(std::int32_t, hit::Spawn*, const hit::Spell*) override { return 0; }
    std::int32_t total_effect(hit::Character* c, std::int32_t spa) override {
        return c == &self_ && ctx_.character ? ctx_.character->total_effect(spa) : 0;
    }
    void root_aa(std::int32_t) override {}
    void movement_rate(std::int32_t) override {}
    std::uint32_t change_form(const hit::Spell*, std::uint32_t, std::int32_t, hit::Affect*) override { return 1; }
    void lose_control_message(hit::Spawn*) override {}
    void update_all_visibility() override {}
    void set_view_actor(const char*) override {}
    void spa87(std::int32_t) override {}
    void spa57(std::int32_t) override {}
    void update_visible_status() override {}
    std::int32_t random_low_high(std::int32_t lo, std::int32_t hi) override {
        return lo + static_cast<std::int32_t>(rng_() % static_cast<std::uint32_t>(hi - lo + 1));
    }
    std::int32_t calc_affect_change_generic(const hit::Spell*, std::uint32_t, std::uint32_t, hit::Affect*,
                                            std::int32_t base) override {
        return base;
    }
    void add_physics_effect(hit::Spawn*, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) override {}
    void apply_post_effect(const hit::Spell*, std::uint32_t) override {}
    void spa436() override { out_.buff_timers_on_hold = true; }

private:
    void snapshot(const Spawn& s, hit::Spawn& h) {
        h.type = s.is_npc ? 1 : (s.is_corpse ? 2 : 0);
        h.spawn_id = s.id;
        h.y = bits(s.position.x);
        h.x = bits(s.position.y);
        h.z = bits(s.position.z);
        h.heading = bits(s.heading);
        h.class_id = s.class_id;
        h.character = s.is_npc ? nullptr : &other_character_;
        copy_text(h.name, s.name);
        levels_[&h] = s.level;
    }
    std::uint8_t level_of(hit::Spawn* s) const {
        auto it = levels_.find(s);
        return it == levels_.end() ? 0 : it->second;
    }
    const SpellData* data(const hit::Spell* s) const {
        if (!s || !ctx_.spells) return nullptr;
        for (const auto& [id, sp] : spells_)
            if (&sp == s) return ctx_.spells->get(id);
        return nullptr;
    }
    std::int32_t duration(const hit::Spell* s, std::uint32_t level) const {
        const SpellData* d = data(s);
        if (!d || !ctx_.character) return d ? static_cast<std::int32_t>(d->duration_cap) : 0;
        return ctx_.character->spell_duration(*d, static_cast<std::uint8_t>(level));
    }
    void say(std::uint32_t id, const std::vector<std::string>& args, std::uint32_t color = 0) {
        if (!ctx_.strings) return;
        out_.chat.push_back(ctx_.strings->format(id, args));
        out_.chat_colors.push_back(color);
    }

    const HitLandingContext& ctx_;
    HitLandingResult& out_;
    std::minstd_rand rng_;
    hit::Character self_;
    hit::CharacterBase cb_;
    hit::Character other_character_;
    hit::Spawn me_;
    std::deque<hit::Spawn> others_;
    std::unordered_map<const hit::Spawn*, std::uint8_t> levels_;
    hit::Pc pc_, other_pc_;
    hit::Profile profile_;
    std::int32_t drunk_ = 0;
    std::array<hit::Affect, kBuffSlots> buffs_{};
    std::array<std::uint32_t, kBuffSlots> caster_ids_{}; // profile +0x2218
    hit::Affect unused_{};
    const hit::Affect* placed_ = nullptr;
    std::unordered_map<std::uint32_t, hit::Spell> spells_;
    std::deque<std::string> texts_;
};

} // namespace

hit::Hit hit_record(std::uint16_t source, std::uint16_t level, float modifier, float heading, std::uint32_t damage,
                    std::uint16_t unknown31, std::uint32_t spell_id, std::uint8_t spell_level) {
    hit::Hit h;
    h.caster_id = source;
    h.item_level = level;
    h.modifier = bits(modifier);
    h.push_heading = bits(heading);
    h.word_1b = static_cast<std::uint16_t>(damage);
    h.song_value = static_cast<std::int32_t>((damage >> 16) | (static_cast<std::uint32_t>(unknown31) << 16));
    h.spell_id = spell_id;
    h.caster_level = spell_level;
    return h;
}

HitLandingResult land_spell(const hit::Hit& h, const HitLandingContext& ctx) {
    HitLandingResult out;
    out.drunkenness = ctx.drunkenness;
    if (!ctx.world || !ctx.spell_state || !ctx.world->player()) return out;
    Landing q(ctx, out);
    hit::Globals g;
    g.connected = false; // the OP 0x56b1 target packet is not sent
    g.ruleset = static_cast<std::uint32_t>(ctx.pvp_rules);
    g.zone_type = ctx.zone_type;
    hit::Spawn* me = &q.me();
    g.local_player = me;
    g.controlled_player = me;
    hit::Launch launch;
    hit::HitState state;
    state.self = &q.self();
    state.globals = &g;
    state.faded_counter_spas = ctx.faded_counter_spas;
    const mechanics::Fpu fpu(mechanics::FpuPrecision::Single24);
    const glm::vec3 before = ctx.velocity;
    const std::uint32_t heading = me->heading;
    out.value = hit::hit_by_spell(state, h, &launch, -1, q, fpu);
    q.keep_slot_data();
    out.velocity_change = glm::vec3(as_float(me->speed_y), as_float(me->speed_x), as_float(me->speed_z)) - before;
    if (!std::isfinite(out.velocity_change.x) || !std::isfinite(out.velocity_change.y) ||
        !std::isfinite(out.velocity_change.z))
        out.velocity_change = glm::vec3(0.0f);
    out.heading_changed = me->heading != heading;
    out.heading = as_float(me->heading);
    out.drunkenness = q.drunkenness();
    if (const hit::Affect* a = q.placed(); a && a->type != 0) {
        if (int slot = q.slot_of(a); slot >= 0) {
            PlacedBuff b;
            b.slot = slot;
            b.spell = static_cast<std::uint32_t>(a->spell_id);
            b.tics = a->duration;
            b.level = a->level;
            b.modifier = as_float(a->modifier);
            b.effect_type = a->type;
            b.caster_id = q.caster_id_of(slot);
            b.slot_data = a->slot_data;
            b.counters_known = a->counters_known;
            out.placed_buffs.push_back(b);
        }
    }
    return out;
}

} // namespace hailstone::game
