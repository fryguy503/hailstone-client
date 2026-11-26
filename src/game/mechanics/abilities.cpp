// Skills and combat abilities (abilities.h). Addresses and sources are in the header; each function names the
// original it follows.
#include "game/mechanics/abilities.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>

#include "game/eq_strings.h"
#include "game/mechanics/character.h"
#include "game/mechanics/item_stats.h"
#include "game/mechanics/tables.h"
#include "game/mechanics/use_skill.h"
#include "game/server_ext.h"

namespace hailstone::game::mechanics {

namespace {

constexpr SkillTableEntry kSkillTable[100] = {
#define HAILSTONE_SKILL_TIMER(id, reuse, damage, combat) {reuse, damage, combat},
#include "game/mechanics/skill_timers.inc"
#undef HAILSTONE_SKILL_TIMER
};
constexpr SkillTableEntry kNoSkill{};

// eqstr ids (NMS/eqstr_us.txt).
constexpr std::uint32_t kMovedUnhidden = 12682;    // 0x318a You have moved and are no longer hidden!!

constexpr std::uint32_t kColorSkill = 0x10e;

void say(SkillResult& out, std::uint32_t eqstr, std::uint32_t color = kColorSkill, std::vector<std::string> args = {}) {
    out.messages.push_back({eqstr, std::move(args), color});
}

void send(SkillResult& out, SkillSend::Kind kind, std::uint32_t target = 0, std::uint32_t skill = 0,
          std::uint32_t value = 0) {
    out.sends.push_back({kind, target, skill, value});
    out.used = true;
}

std::int32_t profile_skill(const SkillContext& ctx, std::uint32_t skill) {
    return skill < kSkills ? ctx.ch->sheet().skills[skill] : 0;
}

// CDisplay::PlayerDistance(a, b, 0) @0x48c450: the 2D distance (TrueDistance with no z scale).
float player_distance(const SkillActor& a, const SkillActor& b) {
    double dx = static_cast<double>(a.x) - b.x, dy = static_cast<double>(a.y) - b.y;
    return std::sqrt(static_cast<float>(dx * dx + dy * dy));
}

// CharacterZoneClient_FindItemByItemType: the first possession (worn, general, then bag contents) of an item
// type; slot and bag slot (-1 when not in a bag).
struct FoundItem {
    int slot = -1, bag = -1;
};
std::optional<FoundItem> find_item_type(const CharacterSheet& sheet, std::uint8_t type) {
    for (int s = 0; s < 33; ++s) {
        const net::Item* it = sheet.item(s);
        if (!it) continue;
        if (it->item_type == type && !it->is_bag()) return FoundItem{s, -1};
    }
    for (int s = 23; s < 33; ++s) {
        const net::Item* bag = sheet.item(s);
        if (!bag || !bag->is_bag()) continue;
        for (std::uint32_t i = 0; i < bag->bag_slots; ++i)
            if (const net::Item* in = bag->content(i); in && in->item_type == type) return FoundItem{s, static_cast<int>(i)};
    }
    return std::nullopt;
}

} // namespace

const SkillTableEntry& skill_table_entry(std::uint32_t skill) { return skill < 100 ? kSkillTable[skill] : kNoSkill; }

namespace {

// GetAdjustedSkill, but a skill the server's profile gives a value is had: the caps of the game's SkillCaps.txt
// (missing, or another server's: a class it never gives the skill, a level below its minimum) would refuse it
// silently, while the server, which decides the attack, says the character has it.
std::int32_t server_adjusted_skill(const Character& ch, std::uint32_t skill) {
    std::int32_t v = ch.adjusted_skill(skill);
    if (v > 0 || skill >= kSkills) return v;
    return std::max<std::int32_t>(v, ch.sheet().skills[skill]);
}

} // namespace

std::int32_t adjusted_skill(const Character& ch, std::uint32_t skill) {
    const CharacterSheet& sheet = ch.sheet();
    if (sheet.class_mask == 0 || !sheet.skill_rules || skill >= kSkills) return server_adjusted_skill(ch, skill);
    // The server extension's HasSkill rules (the NMS patch's detour, see the NMS plugin's repository: a profile skill
    // of -1 and Tracking without Ranger, Druid or Bard have no skill; otherwise the original runs with the class
    // GetSpoofClassForSkill picks for the skill (Rogue for Backstab / Hide / Sneak ..., Warrior / Monk / ... for Kick,
    // ...), else the profile's).
    const SkillRules& rules = *sheet.skill_rules;
    if (rules.gate)
        if (auto gate = rules.gate(rules.ctx, static_cast<std::int32_t>(skill), sheet.class_mask, sheet.skills[skill]))
            return *gate ? 1 : 0;
    if (rules.spoof_class)
        if (std::uint32_t spoof = rules.spoof_class(rules.ctx, static_cast<std::int32_t>(skill), sheet.class_mask))
            if (std::int32_t v = ch.adjusted_skill_as(skill, spoof); v > 0) return v;
    return server_adjusted_skill(ch, skill);
}

std::uint32_t modify_attack_speed(const Character& ch, std::uint32_t delay) {
    if (delay == 0) return 0;
    return melee::modify_attack_speed(delay, 0, ch.total_effect(11, true, 0, true, true), nullptr, ch.fpu());
}

std::int32_t skill_reuse_time(const Character& ch, std::uint32_t skill) {
    std::int32_t spa = ch.total_effect(0xe3, true, static_cast<std::int32_t>(skill), true, true);
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(skill_table_entry(skill).reuse_ms) -
                                     static_cast<std::uint32_t>(spa) * 1000u);
}

// ---- SkillTimers ----

SkillTimers::SkillTimers() {
    can_use_.fill(true);
    combat_ready_.fill(true);
}

bool SkillTimers::uses_combat_timer(std::uint32_t skill) const {
    return skill == skill::kSlam || (skill < 100 && skill_combat_type(skill) == 2);
}

std::uint32_t SkillTimers::last_used(std::uint32_t skill) const {
    if (uses_combat_timer(skill)) return combat_last_[static_cast<std::size_t>(combat_timer_index(skill))];
    if (skill - 1 < 99) return last_[skill];
    return 0;
}

std::uint32_t SkillTimers::raw_duration(std::uint32_t skill) const {
    if (uses_combat_timer(skill)) return combat_duration_[static_cast<std::size_t>(combat_timer_index(skill))];
    if (skill - 1 < 99) return duration_[skill];
    return 0;
}

std::uint32_t SkillTimers::timer_duration(const Character& ch, std::uint32_t skill) const {
    if (uses_combat_timer(skill)) return modify_attack_speed(ch, raw_duration(skill));
    return raw_duration(skill);
}

bool SkillTimers::is_available(std::uint32_t skill) const {
    if (skill > 99) {
        if (skill == skill::kSlam) return combat_ready_[0];
        // 114..116 (LDoN inspect, open, reveal) are always available; the others read a spell gem timer that the
        // clean client does not model.
        return skill == 0x72 || skill == 0x73 || skill == 0x74;
    }
    if (skill_combat_type(skill) == 2) return combat_ready_[static_cast<std::size_t>(combat_timer_index(skill))];
    return can_use_[skill];
}

void SkillTimers::set_last_used(std::uint32_t skill, std::uint32_t when) {
    if (uses_combat_timer(skill)) combat_last_[static_cast<std::size_t>(combat_timer_index(skill))] = when;
    else if (skill - 1 < 99) last_[skill] = when;
}

void SkillTimers::set_timer_duration(std::uint32_t skill, std::uint32_t ms) {
    if (uses_combat_timer(skill)) combat_duration_[static_cast<std::size_t>(combat_timer_index(skill))] = ms;
    else if (skill - 1 < 99) duration_[skill] = ms;
}

void SkillTimers::set_can_use(std::uint32_t skill, bool can) {
    // Enabling clears the duration first (@0x5badb0..).
    if (can) set_timer_duration(skill, 0);
    if (uses_combat_timer(skill)) combat_ready_[static_cast<std::size_t>(combat_timer_index(skill))] = can;
    else if (skill - 1 < 99) can_use_[skill] = can;
}

void SkillTimers::update(const Character& ch, std::uint32_t now, bool sneaking, bool hidden) {
    for (std::uint32_t i = 0; i < 0x4a; ++i) {
        std::uint32_t last = last_used(i);
        std::uint32_t elapsed = (last == 0xffffffffu || last < now) ? now - last : 0;
        if (timer_duration(ch, i) < elapsed && !is_available(i)) {
            if (i == skill::kSneak && sneaking) continue;
            if (i == skill::kHide && hidden) continue;
            set_can_use(i, true);
        }
    }
    for (std::size_t i = 0; i < 2; ++i)
        if (modify_attack_speed(ch, combat_duration_[i]) <= now - combat_last_[i]) combat_ready_[i] = true;
}

std::uint32_t SkillTimers::remaining(const Character& ch, std::uint32_t skill, std::uint32_t now) const {
    if (is_available(skill)) return 0;
    std::uint32_t d = timer_duration(ch, skill), elapsed = now - last_used(skill);
    return elapsed >= d ? 0 : d - elapsed;
}

// ---- melee checks (game/mechanics/melee.h) ----

namespace {

melee::Spawn melee_spawn(const SkillActor& a, std::uint8_t stand_state) {
    melee::Spawn s;
    s.type = a.type;
    s.y = a.y;
    s.x = a.x;
    s.z = a.z;
    s.heading = a.heading;
    s.height = a.avatar_height;
    s.melee_radius = a.melee_radius;
    s.scale = a.scale;
    s.body_wet = a.body_wet;
    s.stand_state = stand_state;
    return s;
}

// ModifyAttackSpeed's quiver search over the sheet's bag slots.
class SheetBags final : public melee::BagQueries {
public:
    explicit SheetBags(const CharacterSheet& sheet) : sheet_(sheet) {}
    bool slot_available(std::int32_t) override { return true; } // CharacterBase_IsInvSlotAvailable: every bag slot
    bool has_item(std::int32_t slot) override { return sheet_.item(slot) != nullptr; }
    std::int32_t item_class(std::int32_t slot) override { return sheet_.item(slot)->item_class; }
    std::int32_t container_type(std::int32_t slot) override { return sheet_.item(slot)->bag_type; }
    std::int32_t weight_reduction(std::int32_t slot) override { return sheet_.item(slot)->weight_reduction; }

private:
    const CharacterSheet& sheet_;
};

// The two rolls' callees from the character.
class CharacterRolls final : public melee::RollQueries {
public:
    explicit CharacterRolls(const SkillContext& ctx) : ctx_(ctx) {}
    std::int32_t adjusted_skill(std::int32_t skill) override {
        // GetAdjustedSkill: not hooked by the NMS patch (an NMS character's caps are the Bard ones of NMS's
        // SkillCaps.txt).
        return ctx_.ch->adjusted_skill(static_cast<std::uint32_t>(skill));
    }
    std::int32_t skill(std::int32_t skill) override {
        return ctx_.ch->skill_with_item_mods(static_cast<std::uint32_t>(skill), true);
    }
    std::int32_t level() override { return ctx_.ch->sheet().level; }
    std::int32_t total_effect(std::int32_t spa) override { return ctx_.ch->total_effect(spa, true, 0, true, true); }
    std::int32_t random() override { return ctx_.random ? ctx_.random() : 0; }

private:
    const SkillContext& ctx_;
};

// DoAttack's and CanIHit's callees answered from a SkillContext; messages and the combat ability packet go to the
// SkillResult, the swings to SkillResult::swings. An item reference is the sheet slot holding the item
// (melee::kFistItem the fist, whose definition is state.attack.fist).
class ContextAttack final : public melee::AttackQueries {
public:
    ContextAttack(const SkillContext& ctx, SkillState& state, SkillResult& out) : ctx_(ctx), st_(state), out_(out) {}
    void set_me(const melee::Spawn* me) { me_ = me; }
    void set_slam(bool slam) { slam_ = slam; }

    bool has_character() override { return ctx_.ch != nullptr; }
    std::int32_t total_effect(std::int32_t spa) override { return ctx_.ch->total_effect(spa, true, 0, true, true); }
    void chat(std::uint32_t eqstr, std::int32_t color) override { say(out_, eqstr, static_cast<std::uint32_t>(color)); }
    void token(std::uint32_t id, std::int32_t color) override { say(out_, id, static_cast<std::uint32_t>(color)); }
    bool allowed_to_attack(const melee::Spawn&) override {
        // PlayerZoneClient::AllowedToAttack(target, -1) @0x5a2360 (game/mechanics/attack_rules.h); without it the
        // old approximation (NPCs always, players both flagged within 4 levels at 6+).
        if (ctx_.allowed_to_attack) return *ctx_.allowed_to_attack;
        const SkillActor& t = *ctx_.target;
        if (t.type >= 2) return false;
        if (t.type == 0 && !t.is_pet) {
            int diff = static_cast<int>(t.level) - static_cast<int>(ctx_.me.level);
            return ctx_.me.pvp_flag && t.pvp_flag && diff <= 4 && diff >= -4 && t.level >= 6 && ctx_.me.level >= 6;
        }
        return true;
    }
    bool can_see(const melee::Spawn&) override { return ctx_.can_see_target; }
    std::uint32_t tick_count() override { return ctx_.tick_count; }
    std::uint32_t now() override { return ctx_.now; }
    bool attack_blocked() override { return ctx_.attack_locked; }
    std::uint8_t is_player_spawn() override { return ctx_.me.type == 0 ? 1 : 0; }
    std::uint8_t can_melee_attack_now(const melee::Spawn*) override {
        melee::MeleeNowInput in;
        in.casting = ctx_.casting;
        in.class_id = ctx_.is_bard ? 8 : 0;
        in.has_character = ctx_.ch != nullptr;
        in.type = ctx_.me.type;
        in.char_state = ctx_.char_state;
        in.corpse = ctx_.me.type >= 2;
        return melee::can_melee_attack_now(in, *this) ? 1 : 0;
    }
    // GetPCSpellAffect(spa, 0, 0): the next buff slot holding the effect (as slot + 1; 0 for none), skipping the
    // ones RemovePCAffectex already took.
    std::uint32_t spell_affect(std::int32_t spa) override {
        if (!affects_) affects_ = ctx_.buff_slots_with_spa ? ctx_.buff_slots_with_spa(spa) : std::vector<int>{};
        return removed_ < affects_->size() ? static_cast<std::uint32_t>((*affects_)[removed_] + 1) : 0;
    }
    void remove_affect(std::uint32_t affect) override {
        out_.removed_buffs.push_back(static_cast<int>(affect) - 1);
        ++removed_;
    }
    void vf_c8() override {}
    std::uint8_t can_use_combat_skill(std::int32_t slot, std::int32_t skill) override {
        if (skill > 99) return 0; // IsSkillIdBelow100
        if (slot != 100) return 1;
        return mechanics::can_use_combat_skill(ctx_, static_cast<std::uint32_t>(skill), slam_) ? 1 : 0;
    }
    melee::ItemRef item(std::int32_t slot) override { return ctx_.ch->sheet().item(slot) ? slot : melee::kNoItem; }
    melee::ItemRef weapon_ref_item(std::int32_t slot, bool fist, bool) override {
        return fist ? melee::kFistItem : item(slot);
    }
    // ItemBase_GetIDFile's model stands for the sheet slot holding the item; the caller looks the model up.
    std::int32_t id_file(melee::ItemRef item) override { return item; }
    void set_held_model(std::int32_t slot, std::int32_t model) override {
        out_.held_models.push_back({static_cast<std::uint8_t>(slot), model == melee::kNoModel ? -1 : model,
                                    out_.sends.size()});
    }
    bool auto_fire() override { return ctx_.autofire; }
    void set_auto_range_attack(bool on) override {
        if (!on) out_.stop_auto_fire = true;
    }
    // GetSkillBaseDamage: the skill table's base damage (the character's own adjustments are not ported).
    std::int32_t base_damage(std::uint32_t skill, const melee::Spawn*) override {
        return skill_table_entry(skill).base_damage;
    }
    void fist_set_definition() override {}
    bool backstab_either_hand() override { return ctx_.backstab_either_hand; }
    std::int32_t item_type(melee::ItemRef item) override {
        if (item == melee::kFistItem) return st_.attack.fist.item_type;
        const net::Item* it = sheet_item(item);
        return it ? it->item_type : 0;
    }
    // ItemDefinition_IsWeapon @0x7f68a0: not ammunition (types 0x1b, 0x1c, 0x2b, 0x2c) and damage above 0.
    std::uint8_t is_weapon(melee::ItemRef item) override {
        std::int32_t type = item_type(item);
        if (type == 0x1b || type == 0x1c || type == 0x2b || type == 0x2c) return 0;
        if (item == melee::kFistItem) return st_.attack.fist.damage > 0 ? 1 : 0;
        const net::Item* it = sheet_item(item);
        return it && static_cast<std::int32_t>(it->damage) > 0 ? 1 : 0;
    }
    std::uint32_t item_delay(melee::ItemRef item) override {
        if (item == melee::kFistItem) return st_.attack.fist.delay;
        const net::Item* it = sheet_item(item);
        return it ? it->delay : 0;
    }
    std::int32_t item_range(melee::ItemRef item) override {
        const net::Item* it = sheet_item(item);
        return it ? it->range : 0;
    }
    std::int32_t item_stack(melee::ItemRef item) override {
        const net::Item* it = sheet_item(item);
        return it ? static_cast<std::int32_t>(it->stack_count) : 0;
    }
    std::int32_t item_id(melee::ItemRef item) override {
        const net::Item* it = sheet_item(item);
        return it ? static_cast<std::int32_t>(it->id) : 0;
    }
    std::uint8_t can_use_item(melee::ItemRef item) override {
        if (item == melee::kFistItem) return 1;
        const net::Item* it = sheet_item(item);
        return it && ctx_.ch->can_use_item(*it) ? 1 : 0;
    }
    melee::AttackProfile profile() override {
        // The profile's +0x346c / +0x3470 (EQEmu sends 0) and +0x30f0, Skill[20].
        melee::AttackProfile p;
        p.fist_damage = ctx_.ch->sheet().fist_damage;
        p.fist_delay = ctx_.ch->sheet().fist_delay;
        p.double_attack = ctx_.ch->sheet().skills[20];
        return p;
    }
    std::int32_t reuse_time(std::uint32_t skill) override { return skill_table_entry(skill).reuse_ms; }
    void hand_to_hand_delay(std::int32_t& delay, melee::ItemRef hands) override {
        const CharacterSheet& sheet = ctx_.ch->sheet();
        melee::HandToHandInput in;
        in.skill = ctx_.ch->skill_with_item_mods(0x1c, true);
        const net::Item* h = sheet_item(hands);
        in.hands_item = h != nullptr;
        in.hands_item_id = h ? static_cast<std::int32_t>(h->id) : 0;
        in.level = sheet.level;
        in.profile_3370 = static_cast<std::int32_t>(sheet.race); // Iksar (0x80) adds one to the base
        if (auto d = melee::hand_to_hand_delay(in)) delay = *d;
    }
    std::uint32_t modify_attack_speed(std::uint32_t delay, bool bow) override {
        SheetBags bags(ctx_.ch->sheet());
        std::int32_t haste = delay ? ctx_.ch->total_effect(11, true, 0, true, true) : 0;
        return melee::modify_attack_speed(delay, bow ? 1u : 0u, haste, &bags, ctx_.ch->fpu());
    }
    std::uint32_t combat_timer_index(std::uint32_t skill) override {
        return static_cast<std::uint32_t>(mechanics::combat_timer_index(skill));
    }
    std::uint32_t skill_reuse_time(std::uint32_t skill) override {
        // A deliberate change from RoF2 (a fixed bug): the original's DoAttack stores GetSkillReuseTime unclamped
        // in the unsigned combat timer, so SPA 227 taking more seconds off than the skill's reuse wraps it and
        // locks the skill for ~49 days. Hailstone clamps it at 0, as the original's UseSkill does for the
        // non-combat skills. Not switchable: no server relies on the lock.
        const std::int32_t reuse = mechanics::skill_reuse_time(*ctx_.ch, skill);
        return reuse < 0 ? 0u : static_cast<std::uint32_t>(reuse);
    }
    void set_skill_timer_duration(std::uint32_t skill, std::uint32_t ms) override {
        st_.timers.set_timer_duration(skill, ms);
    }
    void chat2(std::uint32_t eqstr) override { say(out_, eqstr, 0); }
    double player_distance(const melee::Spawn*) override {
        return ctx_.target ? mechanics::player_distance(ctx_.me, *ctx_.target) : 0.0;
    }
    bool hit_anyway() override { return ctx_.gm; }
    std::uint8_t can_i_hit(const melee::Spawn* target, float range) override {
        return me_ && melee::can_i_hit(*me_, target, range, *this, ctx_.ch->fpu()) ? 1 : 0;
    }
    void debug_cant_hit() override {}
    void send_attack(const melee::Spawn* target, melee::ItemRef item, std::int32_t slot, std::uint32_t skill) override {
        if (slot == 100) {
            // SendCombatAbilityAttack @0x4fb3d0: OP_CombatAbility for a living target.
            if (target && ctx_.target && ctx_.target->type < 2)
                send(out_, SkillSend::Kind::CombatAbility, ctx_.target->id, skill);
            return;
        }
        if (slot == 11) {
            // SendCombatAbilityAttack for the range slot (DoRangedAttack): OP_CombatAbility {target, 11, the item's
            // skill} for a living target, the skill as @0x4fb55d works it out (item type 9 231, 0x16 the item's
            // own (GetDelay), else ItemTypeToSkill: archery 7 for arrows, throwing 51). Its AimAtTarget(target, 0)
            // @0x4fb42f changes nothing: AimAtLocation @0x59e6c0 writes the heading only with a missile.
            if (!target || !ctx_.target || ctx_.target->type >= 2 || ctx_.me.type >= 2) return;
            const std::int32_t type = item_type(item);
            std::uint32_t s = type == 9      ? 0xe7u
                              : type == 0x16 ? item_delay(item)
                                             : item_type_to_skill(static_cast<std::uint32_t>(type));
            send(out_, SkillSend::Kind::RangedAttack, ctx_.target->id, s & 0xff);
            return;
        }
        out_.swings.push_back({static_cast<std::uint8_t>(slot), static_cast<std::uint8_t>(item_type(item)),
                               item == melee::kFistItem});
    }
    std::uint8_t check_double_attack(const melee::Spawn*, std::int32_t) override {
        CharacterRolls rolls(ctx_);
        melee::DoubleAttackInput in;
        in.class_id = static_cast<std::uint8_t>(ctx_.ch->sheet().class_id);
        return melee::check_double_attack(in, rolls) ? 1 : 0;
    }
    std::uint8_t check_dual_wield(const melee::Spawn*) override {
        CharacterRolls rolls(ctx_);
        melee::DualWieldInput in;
        if (const net::Item* p = ctx_.ch->sheet().item(13)) {
            in.primary = true;
            in.primary_class = p->item_class;
            in.primary_type = p->item_type;
        }
        return melee::check_dual_wield(in, rolls) ? 1 : 0;
    }
    void ranged_attack(const melee::Spawn* target) override {
        if (me_) melee::do_ranged_attack(*me_, target, *this);
    }

private:
    const net::Item* sheet_item(melee::ItemRef item) const {
        return item >= 0 ? ctx_.ch->sheet().item(item) : nullptr;
    }
    const SkillContext& ctx_;
    SkillState& st_;
    SkillResult& out_;
    const melee::Spawn* me_ = nullptr;
    bool slam_ = false;
    std::optional<std::vector<int>> affects_;
    std::size_t removed_ = 0;
};

// The AttackState DoAttack works on: the player's timers, with the combat skill timers SkillTimers keeps.
melee::AttackState& attack_state(SkillState& state) {
    std::copy(state.timers.combat_ticks.begin(), state.timers.combat_ticks.end(), state.attack.combat_ticks.begin());
    std::copy(state.timers.combat_used.begin(), state.timers.combat_used.end(), state.attack.combat_used.begin());
    state.attack.last_intimidate = state.last_intimidate;
    return state.attack;
}
void store_attack_state(SkillState& state) {
    std::copy_n(state.attack.combat_ticks.begin(), state.timers.combat_ticks.size(), state.timers.combat_ticks.begin());
    std::copy_n(state.attack.combat_used.begin(), state.timers.combat_used.size(), state.timers.combat_used.begin());
    state.last_intimidate = state.attack.last_intimidate;
}

} // namespace

double bearing(double y1, double x1, double y2, double x2) {
    return melee::bearing(static_cast<float>(y1), static_cast<float>(x1), static_cast<float>(y2),
                          static_cast<float>(x2), Fpu{});
}

float melee_range(const SkillActor* a, const SkillActor* b) {
    melee::RangeSpawn ra, rb;
    if (a) ra = {a->melee_radius, a->scale, a->avatar_height};
    if (b) rb = {b->melee_radius, b->scale, b->avatar_height};
    return melee::melee_range(a ? &ra : nullptr, b ? &rb : nullptr, Fpu{});
}

bool can_i_hit(const SkillContext& ctx, float extra_range, SkillResult& out) {
    if (!ctx.ch) return false;
    SkillState scratch;
    ContextAttack q(ctx, scratch, out);
    melee::Spawn me = melee_spawn(ctx.me, ctx.stand_state);
    std::optional<melee::Spawn> target;
    if (ctx.target && !ctx.target_is_me) target = melee_spawn(*ctx.target, 100);
    const melee::Spawn* t = ctx.target_is_me ? &me : target ? &*target : nullptr;
    return melee::can_i_hit(me, t, extra_range, q, ctx.ch->fpu());
}

bool can_use_combat_skill(const SkillContext& ctx, std::uint32_t skill, bool slam) {
    if (skill > 99) return false; // IsSkillIdBelow100
    if (skill_table_entry(skill).base_damage < 1) return false;
    const CharacterSheet& sheet = ctx.ch->sheet();
    // Bash without the skill or a shield for races 9, 10 and 2 only: the original compares the spawn's GetRace
    // (vtable +0x88, 0x58b4e0, the raw +0xeb4 an illusion's SetRace writes) with those three, so the variant races
    // 90, 92 and 93, whose innate Slam slot InitInnates also sets, do not pass here.
    const std::uint32_t race = ctx.form_race ? ctx.form_race : sheet.race;
    if (skill == skill::kBash && (race == 9 || race == 10 || race == 2)) return true;
    if (adjusted_skill(*ctx.ch, skill) <= 0) return false; // CharacterZoneClient::HasSkill @0x44a1b0
    if (skill != skill::kBash || ctx.me.type != 0) return true;
    // A deliberate change to a RoF2 rule (docs/mechanics.md "Slam"): Slam's attack needs no shield. The original
    // asks the races 90, 92 and 93, which have the innate Slam but not the race pass above, for the shield as for
    // Bash; Hailstone asks only for the Bash skill, which EQEmu's OPCombatAbility wants from them too (it bypasses
    // the skill only for races 2, 9 and 10, by GetRace, and checks no shield).
    if (slam) return true;
    // Bash: a shield in the secondary slot, one of three primary items, or a two-hander with SPA 226.
    bool allowed = true;
    const net::Item* secondary = sheet.item(14);
    if (!secondary || secondary->item_class != 0 || secondary->item_type != 8) {
        const net::Item* primary = sheet.item(13);
        if (!primary || (primary->id != 11050 && primary->id != 10099 && primary->id != 14383)) allowed = false;
    }
    // The original also requires the character's +8, its own player spawn (UseSkill reads the same field as the
    // spawn), which is always there while the player acts.
    const net::Item* primary = sheet.item(13);
    if (primary && ctx.ch->total_effect(0xe2, true, 0, true, true) != 0 &&
        (primary->item_type == 1 || primary->item_type == 4 || primary->item_type == 35))
        return true;
    return allowed;
}

bool do_attack(const SkillContext& ctx, SkillState& state, std::uint8_t slot, std::uint32_t skill, SkillResult& out,
               bool slam) {
    if (!ctx.ch) return false;
    ContextAttack q(ctx, state, out);
    melee::Spawn me = melee_spawn(ctx.me, ctx.stand_state);
    q.set_me(&me);
    q.set_slam(slam);
    std::optional<melee::Spawn> target;
    if (ctx.target && !ctx.target_is_me) target = melee_spawn(*ctx.target, 100);
    const melee::Spawn* t = ctx.target_is_me ? &me : target ? &*target : nullptr;
    melee::AttackState& as = attack_state(state);
    bool done = melee::do_attack(as, me, t, slot, skill, q, ctx.ch->fpu());
    store_attack_state(state);
    return done;
}

bool do_attack_skill(const SkillContext& ctx, SkillState& state, std::uint32_t skill, SkillResult& out) {
    return do_attack(ctx, state, 100, skill, out);
}

void attack_on_reset(const SkillContext& ctx, SkillState& state, bool attack_on) {
    if (!ctx.ch) return;
    SkillResult out;
    ContextAttack q(ctx, state, out);
    melee::attack_on_reset(attack_state(state), attack_on, q);
    store_attack_state(state);
}

void swing_ready(const SkillContext& ctx, SkillState& state) {
    if (!ctx.ch) return;
    SkillResult out;
    ContextAttack q(ctx, state, out);
    melee::swing_ready(attack_state(state), ctx.now, q);
    store_attack_state(state);
}

std::optional<SkillMessage> check_hide_moved(const SkillContext& ctx, SkillState& state, std::uint8_t class_id) {
    if (!state.hidden && state.activity != static_cast<std::int32_t>(skill::kHide)) return std::nullopt;
    // Truncated as save_position (use_skill.cpp) stored them, through __ftol2 (NaN and out of range: INT_MIN), and
    // the z difference taken in 32 bits with its wrap and a two's-complement magnitude (INT_MIN stays negative), so a
    // corrupt position is defined behaviour. For any position in the int range this is the plain cast and abs.
    const std::int32_t x = Fpu::ftol(static_cast<double>(ctx.me.y)), y = Fpu::ftol(static_cast<double>(ctx.me.x));
    const std::int32_t z = Fpu::ftol(static_cast<double>(ctx.me.z));
    const auto dz = static_cast<std::uint32_t>(state.hide_z) - static_cast<std::uint32_t>(z);
    const std::uint32_t sign = dz >> 31 ? 0xffffffffu : 0u;
    const auto z_moved = static_cast<std::int32_t>((dz ^ sign) - sign);
    bool moved = state.hide_x != x || state.hide_y != y || z_moved > 5;
    if (!moved || (class_id == 9 && state.sneaking)) return std::nullopt;
    if (state.activity == static_cast<std::int32_t>(skill::kHide)) state.activity = -1;
    state.hidden = false;
    return SkillMessage{kMovedUnhidden, {}, kColorSkill};
}

std::string format_skill_message(const EqStrings& strings, const SkillMessage& m) {
    std::vector<std::string> args = m.args;
    for (std::size_t i = 0; i < args.size() && i < m.arg_eqstr.size(); ++i)
        if (m.arg_eqstr[i]) args[i] = strings.format(*m.arg_eqstr[i], {});
    return strings.format(m.eqstr, args);
}

namespace {

// UseSkill's callees answered from a SkillContext (use_skill.h): the timers are SkillTimers, DoAttack(100, ...) is
// do_attack_skill and CanIHit can_i_hit; messages and packets go to the SkillResult. A message is kept as its eqstr
// id with its arguments: getString(id) hands out "\0@<id>" and build_token_string "\0@<id>\x1f<parameter>", which
// dsp_chat turns back into a SkillMessage. The leading NUL keeps those references apart from a name: a name comes
// off the wire as a C string and cannot hold one, so a spawn called "@5" stays the text "@5".
class ContextQueries final : public useskill::Queries {
public:
    ContextQueries(const SkillContext& ctx, SkillState& state, SkillResult& out, useskill::Spawn& me,
                   useskill::Spawn* target)
        : ctx_(ctx), st_(state), out_(out), me_(me), target_(target) {}

    std::int32_t cur_hp(std::int32_t, std::int32_t) override { return ctx_.cur_hp; }
    std::int32_t get_adjusted_skill(std::int32_t skill) override {
        // GetAdjustedSkill (not hooked by the NMS patch), with the server's profile value as the last word.
        return skill < 0 ? 0 : server_adjusted_skill(*ctx_.ch, static_cast<std::uint32_t>(skill));
    }
    std::int32_t total_effect(std::int32_t spa, std::uint8_t a1, std::int32_t a2, std::uint8_t a3,
                              std::uint8_t a4) override {
        return ctx_.ch->total_effect(spa, a1 != 0, a2, a3 != 0, a4 != 0);
    }
    std::string get_string(std::uint32_t id, std::int32_t) override { return ref(id); }
    void chat(const std::string& text, std::int32_t color, std::uint8_t, std::uint8_t) override {
        SkillMessage m;
        m.color = static_cast<std::uint32_t>(color);
        const std::size_t sep = text.find('\x1f');
        m.eqstr = ref_id(text.substr(0, sep)).value_or(0);
        if (sep != std::string::npos) {
            std::string param = text.substr(sep + 1);
            if (auto id = ref_id(param)) {
                m.args.emplace_back();
                m.arg_eqstr.emplace_back(*id);
            } else {
                m.args.push_back(std::move(param));
            }
        }
        out_.messages.push_back(std::move(m));
    }
    useskill::Handle get_pc_spell_affect(useskill::Obj, std::int32_t spa, std::int32_t* slot, bool) override {
        // The first buff slot with the effect that is still there (removed ones are gone).
        int found = -1;
        if (ctx_.buff_slots_with_spa)
            for (int s : ctx_.buff_slots_with_spa(spa))
                if (std::find(out_.removed_buffs.begin(), out_.removed_buffs.end(), s) == out_.removed_buffs.end()) {
                    found = s;
                    break;
                }
        if (slot) *slot = found;
        return found < 0 ? 0 : static_cast<useskill::Handle>(found + 1);
    }
    std::int32_t get_skill_combat_type(std::uint32_t skill) override { return skill_combat_type(skill); }
    bool is_available(std::uint32_t skill) override {
        // CSkillMgr::IsAvailable; the server extension's gate first (the NMS patch's detour refuses a profile skill
        // of -1 and Tracking without Ranger, Druid or Bard; its class spoof does not change the timer test).
        const CharacterSheet& sheet = ctx_.ch->sheet();
        if (sheet.class_mask && sheet.skill_rules && sheet.skill_rules->gate && skill < kSkills)
            if (auto gate = sheet.skill_rules->gate(sheet.skill_rules->ctx, static_cast<std::int32_t>(skill),
                                                    sheet.class_mask, sheet.skills[skill]))
                return *gate;
        return st_.timers.is_available(skill);
    }
    std::int32_t get_skill_reuse_time(std::uint32_t skill) override { return skill_reuse_time(*ctx_.ch, skill); }
    void set_skill_timer_duration(std::uint32_t skill, std::int32_t ms) override {
        st_.timers.set_timer_duration(skill, static_cast<std::uint32_t>(ms));
    }
    void set_skill_last_used(std::uint32_t skill, std::int32_t time) override {
        st_.timers.set_last_used(skill, static_cast<std::uint32_t>(time));
    }
    void set_skill_can_use(std::uint32_t skill, std::int32_t can) override {
        st_.timers.set_can_use(skill, can != 0);
        if (!can) out_.used = true;
    }
    bool send(std::int32_t, std::uint16_t opcode, const std::vector<std::uint8_t>& payload) override {
        SkillSend s;
        s.kind = kind_of(opcode, payload);
        s.raw = true;
        s.opcode = opcode;
        s.payload = payload;
        out_.sends.push_back(std::move(s));
        out_.used = true;
        return true;
    }
    void close_connection() override {}
    void debug_log_void_connection(std::uint32_t) override {}
    void send_ldon_inspect(std::int32_t, std::int32_t) override { push(SkillSend::Kind::LDoNInspect); }
    void send_ldon_open(std::int32_t, std::int32_t) override { push(SkillSend::Kind::LDoNOpen); }
    void cmd_reveal(std::int32_t, std::int32_t) override { push(SkillSend::Kind::Reveal); }
    bool pc_get_hide(useskill::Obj) override { return st_.hidden; }
    void pc_set_hide(useskill::Obj, std::uint8_t hide) override {
        st_.hidden = hide != 0;
        out_.visibility_changed = true;
    }
    void pc_set_sneak(useskill::Obj, std::uint8_t sneak) override { st_.sneaking = sneak != 0; }
    void get_current_profile() override {}
    useskill::Handle get_item(std::int32_t slot) override {
        const net::Item* item = ctx_.ch->sheet().item(slot);
        if (!item && slot == 0x21 && !ctx_.cursor_has_item) return 0;
        if (!item && slot != 0x21) return 0;
        items_.push_back(item); // the cursor may hold something the sheet does not list
        return items_.size();
    }
    void item_ref_release() override {}
    bool can_use_item(useskill::Handle item, std::int32_t, std::int32_t) override {
        const net::Item* it = item_of(item);
        return it && ctx_.ch->can_use_item(*it);
    }
    std::int32_t get_item_type(useskill::Handle item) override {
        const net::Item* it = item_of(item);
        return it ? it->item_type : 0;
    }
    std::int32_t get_item_class(useskill::Handle item) override {
        const net::Item* it = item_of(item);
        return it ? it->item_class : 0;
    }
    std::int32_t get_item_id(useskill::Handle item) override {
        const net::Item* it = item_of(item);
        return it ? static_cast<std::int32_t>(it->id) : 0;
    }
    bool do_attack(useskill::Spawn&, std::int32_t slot, std::uint32_t skill, useskill::Spawn*) override {
        if (slot != 100) return false;
        bool done = mechanics::do_attack(ctx_, st_, 100, skill, out_, slam);
        for (std::size_t i = 0; i < 2; ++i) me_.combat_skill_used[i] = static_cast<std::int32_t>(st_.timers.combat_used[i]);
        return done;
    }
    std::string trim_name(const std::string& name) override { return name; }
    std::string build_token_string(std::uint32_t id, const std::string& p1, std::int32_t) override {
        return ref(id) + '\x1f' + p1;
    }
    void change_position(useskill::Spawn&, std::uint8_t stand_state) override { out_.stand_state = stand_state; }
    void set_auto_attack(std::uint8_t on) override {
        if (!on) out_.stop_attack = true;
    }
    void set_auto_range_attack(std::uint8_t on) override {
        if (!on) out_.stop_auto_fire = true;
    }
    void update_my_visible_status(useskill::Obj, std::uint8_t) override { out_.visibility_changed = true; }
    void set_activity(std::int32_t kind, std::int32_t) override { st_.activity = kind; }
    std::int32_t rand_nbr() override { return ctx_.random ? ctx_.random() : 0; }
    std::int32_t random_low_high(std::int32_t low, std::int32_t high) override {
        return ctx_.random_range ? ctx_.random_range(low, high) : low;
    }
    void compass_reset() override { out_.compass_reset = true; }
    void send_track() override { out_.track_request = true; }
    std::int32_t not_on_same_deity(useskill::Spawn& self, useskill::Spawn& other, std::int32_t) override {
        // SkillActor::same_deity_ok: the target is on the player's side (PvP rule set 4).
        const SkillActor* t = &self == &me_ ? actor(&other) : actor(&self);
        return t && t->same_deity_ok ? 0 : 1;
    }
    std::int32_t get_level(useskill::Spawn& spawn) override {
        const SkillActor* a = actor(&spawn);
        return a ? a->level : 0;
    }
    void push_pending_reason(std::int32_t) override {}
    void push_interact_reason(std::int32_t) override {}
    void make_me_visible(useskill::Obj, std::int32_t, std::uint8_t) override {
        st_.hidden = false;
        out_.visibility_changed = true;
    }
    bool can_i_hit(useskill::Spawn&, useskill::Spawn*, std::uint32_t extra) override {
        return mechanics::can_i_hit(ctx_, std::bit_cast<float>(extra), out_);
    }
    void remove_pc_affect(useskill::Obj, useskill::Handle affect, std::int32_t) override {
        if (affect) out_.removed_buffs.push_back(static_cast<int>(affect) - 1);
    }
    void notify_pc_affect_change(useskill::Obj, std::int32_t, std::uint8_t) override {}
    void player_per_frame_update(useskill::Spawn*) override {}
    void token_message(useskill::Obj, std::int32_t id, std::int32_t color, std::int32_t) override {
        out_.messages.push_back({static_cast<std::uint32_t>(id), {}, static_cast<std::uint32_t>(color)});
    }
    bool find_item_by_item_type(std::int32_t type, std::int32_t* slot, std::int32_t* bag) override {
        auto found = find_item_type(ctx_.ch->sheet(), static_cast<std::uint8_t>(type));
        if (slot) *slot = found ? found->slot : -1;
        if (bag) *bag = found ? found->bag : -1;
        return found.has_value();
    }
    std::string get_id_file(useskill::Handle, std::int32_t) override { return {}; }
    void update_item_slot(useskill::Spawn&, std::uint8_t, const std::string&, std::int32_t, std::int32_t) override {}
    useskill::Handle get_animation(useskill::Spawn&) override { return 0; }
    void animation_vf0c(useskill::Handle, std::int32_t, std::int32_t, std::int32_t, std::uint32_t, std::int32_t,
                        std::int32_t, std::int32_t) override {}
    std::int32_t get_time() override { return static_cast<std::int32_t>(ctx_.now); }
    void chat_template(useskill::Obj, std::uint32_t id, std::int32_t color, const std::string& p1,
                       std::int32_t) override {
        out_.messages.push_back({id, {p1}, static_cast<std::uint32_t>(color)});
    }
    std::int32_t get_base_skill(std::int32_t skill, std::int32_t) override {
        return ctx_.ch->skill_with_item_mods(static_cast<std::uint32_t>(skill), true);
    }
    bool get_pc_client() override { return true; }
    bool is_interactive_object(useskill::Spawn& s) override {
        const SkillActor* a = actor(&s);
        return a && a->is_trap_or_box;
    }
    bool is_vehicle_race(useskill::Spawn&) override { return false; }
    bool is_gm(useskill::Spawn& s) override {
        const SkillActor* a = actor(&s);
        return a && a->gm;
    }
    void set_hide_mode(useskill::Spawn& s, std::int32_t mode) override {
        s.hide_mode = mode;
        if (&s == &me_) st_.hidden = mode != 0;
    }
    void tracking_window_show(std::uint8_t, std::uint8_t, std::uint8_t) override { out_.open_tracking_window = true; }
    std::uint32_t player_distance(useskill::Spawn& a, useskill::Spawn& b, std::uint32_t) override {
        const SkillActor *x = actor(&a), *y = actor(&b);
        return x && y ? std::bit_cast<std::uint32_t>(mechanics::player_distance(*x, *y)) : 0;
    }
    std::uint32_t get_melee_range(useskill::Spawn& a, useskill::Spawn& b) override {
        return std::bit_cast<std::uint32_t>(melee_range(actor(&a), actor(&b)));
    }

private:
    static std::string ref(std::uint32_t id) { return std::string(1, '\0') + '@' + std::to_string(id); }
    // The id of a whole "\0@<id>" reference; nothing for any other text.
    static std::optional<std::uint32_t> ref_id(const std::string& text) {
        if (text.size() < 3 || text[0] != '\0' || text[1] != '@') return std::nullopt;
        std::uint32_t id = 0;
        for (std::size_t i = 2; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') return std::nullopt;
            id = id * 10 + static_cast<std::uint32_t>(text[i] - '0');
        }
        return id;
    }
    static SkillSend::Kind kind_of(std::uint16_t opcode, const std::vector<std::uint8_t>& payload) {
        using K = SkillSend::Kind;
        switch (opcode) {
        case 0x71da: return K::RemoveTrap;
        case 0x9be3: return K::PickLock;
        case 0x78bf: return K::DisarmTraps;
        case 0x2703: return K::Taunt;
        case 0x52fa: return K::FeignDeath;
        case 0x640e: return K::InstillDoubt;
        case 0x6703: return K::Begging;
        case 0x39e8: return K::PickPocket;
        case 0x0ecf: return K::Mend;
        case 0x02af: return K::SenseTraps;
        case 0x0971: return payload.size() > 2 && payload[2] == 3 ? K::Unhide : K::StopSneak;
        case 0x5ec8: return K::Disarm;
        case 0x4577: return K::TrackUnknown;
        case 0x5d55: return K::Sneak;
        case 0x67fe: return K::Hide;
        case 0x260a: return K::SenseHeading;
        case 0x5306: return K::Forage;
        case 0x0386: return K::BindWound;
        case 0x7177: return K::FishAnimation;
        default: return K::Fishing; // 0x1e2a
        }
    }
    void push(SkillSend::Kind kind) {
        SkillSend s;
        s.kind = kind;
        out_.sends.push_back(s);
        out_.used = true;
    }
    const net::Item* item_of(useskill::Handle h) const { return h && h <= items_.size() ? items_[h - 1] : nullptr; }
    const SkillActor* actor(const useskill::Spawn* s) const {
        if (s == &me_) return &ctx_.me;
        if (s && s == target_ && ctx_.target) return &*ctx_.target;
        return nullptr;
    }

    const SkillContext& ctx_;
    SkillState& st_;
    SkillResult& out_;
    useskill::Spawn& me_;
    useskill::Spawn* target_;
    std::vector<const net::Item*> items_;

public:
    bool slam = false; // this UseSkill runs Slam (111): its DoAttack(100, 10) needs no shield (can_use_combat_skill)
};

useskill::Spawn spawn_of(const SkillActor& a) {
    useskill::Spawn s;
    s.type = a.type;
    s.spawn_id = a.id;
    s.pvp = a.pvp_flag ? 1 : 0;
    s.mercenary = a.mercenary ? 1 : 0;
    s.y = std::bit_cast<std::uint32_t>(a.y);
    s.x = std::bit_cast<std::uint32_t>(a.x);
    s.z = std::bit_cast<std::uint32_t>(a.z);
    s.heading = std::bit_cast<std::uint32_t>(a.heading);
    s.body_wet = a.body_wet;
    s.name = s.displayed_name = a.name;
    return s;
}

} // namespace

SkillResult use_skill(const SkillContext& ctx, SkillState& state, std::uint32_t id) {
    SkillResult out;
    if (!ctx.ch) return out;
    // Racial Slam follows the current appearance, including illusions. Route Bash through the
    // shieldless Slam path when the current form supplies it, even if the base race cannot Slam.
    // Under a 90, 92 or 93 form Slam still needs the Bash skill (can_use_combat_skill), so a character without it
    // sends nothing, which EQEmu would drop.
    if (id == skill::kBash && ctx.form_slam != 0xff) id = skill::kSlam;
    useskill::Spawn me = spawn_of(ctx.me);
    me.stand_state = ctx.stand_state;
    me.has_mount = ctx.mounted;
    me.sneak = state.sneaking ? 1 : 0;
    me.hide_mode = state.hidden ? 1 : 0;
    me.fishing_event = state.fishing ? 1 : 0;
    me.fishing_eta = static_cast<std::int32_t>(state.fishing_eta);
    me.combat_skill_used = {static_cast<std::int32_t>(state.timers.combat_used[0]),
                            static_cast<std::int32_t>(state.timers.combat_used[1])};
    me.next_intimidate_time = state.next_intimidate_time;
    me.intimidate_count = state.intimidate_count;
    me.speed_y = std::bit_cast<std::uint32_t>(ctx.speed_y);
    me.speed_x = std::bit_cast<std::uint32_t>(ctx.speed_x);
    me.speed_z = std::bit_cast<std::uint32_t>(ctx.speed_z);
    std::optional<useskill::Spawn> target;
    if (ctx.target && !ctx.target_is_me) target = spawn_of(*ctx.target);
    useskill::State s;
    s.me = &me;
    s.target = ctx.target_is_me ? &me : target ? &*target : nullptr;
    s.local_player = s.controlled_player = &me;
    s.status = ctx.char_state;
    s.stand_state = ctx.stand_state;
    s.skill_disarm = profile_skill(ctx, skill::kDisarm);
    s.skill_pick_pockets = profile_skill(ctx, skill::kPickPockets);
    s.skill_begging = profile_skill(ctx, skill::kBegging);
    s.innate_slam = ctx.form_slam;
    s.attack = ctx.attacking;
    s.autofire = ctx.autofire;
    s.ruleset = ctx.pvp_ruleset;
    s.activity = state.activity;
    s.display_time = static_cast<std::int32_t>(ctx.now);
    s.tracking_window_visible = ctx.tracking_window_open;
    s.compass_window = ctx.compass_window;
    s.hide_position = {state.hide_x, state.hide_y, state.hide_z};
    s.precision = ctx.ch->fpu().precision();
    ContextQueries q(ctx, state, out, me, target ? &*target : nullptr);
    q.slam = id == skill::kSlam;
    useskill::use_skill(s, id, q);
    state.sneaking = me.sneak != 0;
    state.fishing = me.fishing_event != 0;
    state.fishing_eta = static_cast<std::uint32_t>(me.fishing_eta);
    state.next_intimidate_time = me.next_intimidate_time;
    state.intimidate_count = me.intimidate_count;
    state.hide_x = s.hide_position[0];
    state.hide_y = s.hide_position[1];
    state.hide_z = s.hide_position[2];
    return out;
}

std::vector<std::uint32_t> usable_skills(const Character& ch, bool combat, std::uint32_t current_race) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t s = 0; s < 100; ++s) {
        if (!skill_activated(s) || adjusted_skill(ch, s) < 1) continue;
        std::int32_t type = skill_combat_type(s);
        if ((type == 1 || type == 2) == combat) out.push_back(s);
    }
    // Slam (111) comes from the current form, not necessarily the profile race.
    // The list updates as an illusion starts or expires.
    const auto race = current_race ? current_race : ch.sheet().race;
    if (combat && slam_innate(race) != 0xff) out.push_back(skill::kSlam);
    return out;
}

std::array<std::int32_t, 25> init_innates(std::uint32_t race, std::uint32_t class_id) {
    std::array<std::int32_t, 25> slots;
    slots.fill(0xff);
    slots[14] = 0; // +0x3268
    slots[15] = 0; // +0x326c
    auto has = [&](std::size_t slot) { slots[slot] = 0; };
    switch (race) {
    case 1: case 44: case 55: case 67: case 71: break;          // Human and its variants: none
    case 2: case 90: has(kInnateSlam); break;                     // Barbarian
    case 3: case 78: has(8); break;                               // Erudite
    case 5: case 106: has(kInnateInfravision); has(8); break;     // High Elf
    case 6: case 77: case 98: has(kInnateUltravision); break;     // Dark Elf
    case 9: case 92:                                              // Troll
        has(10);
        has(kInnateSlam);
        has(kInnateInfravision);
        break;
    case 10: case 93:                                             // Ogre
        has(kInnateInfravision);
        has(kInnateSlam);
        has(9);
        has(2);
        break;
    case 12: has(kInnateInfravision); has(8); break;              // Gnome
    case 27: case 32: case 33: case 60: case 65: case 69: case 70: case 85: case 117: case 118: case 122: case 123:
    case 330: case 367:                                           // Froglok and others
        has(kInnateUltravision);
        break;
    case 128: has(10); has(kInnateInfravision); break;            // Iksar
    default: has(kInnateInfravision); break; // Wood Elf 4/112, Half Elf, Dwarf 8/94, Halfling 11/81, Vah Shir 130...
    }
    // The class's (the second argument).
    switch (class_id) {
    case 4: has(1); break;            // Ranger
    case 6: has(4); break;            // Druid
    case 7: has(12); has(1); break;   // Monk
    case 8: has(16); break;           // Bard
    case 9: has(12); has(16); break;  // Rogue
    default: break;
    }
    return slots;
}

} // namespace hailstone::game::mechanics
