// EQ_Character::HitBySpell (game/mechanics/hit_by_spell.h), checked against
// the original's vectors; the exe helpers from the disassembly of 0x441bb0..0x441f24
// (Vec3_Normalize, Vec3_Cross, Ray_SetFromPoints, the CCollisionInfoBase and
// CCollisionInfoSphere constructors, the matrix helpers). Addresses in the
// comments are the original's.
#include "game/mechanics/hit_by_spell.h"

#include "core/wrap_math.h"
#include "game/counter_spas.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace hailstone::game::mechanics::hit {

namespace {

// Exe constants.
constexpr std::uint32_t kNoFloor = 0xec4ecb8f;      // 0x9c5760: -1e27
constexpr std::uint32_t kFive = 0x40a00000;         // 0x9c58a0
constexpr std::uint32_t kTwo = 0x40000000;          // 0x9c58e8
constexpr std::uint32_t kTwoHundred = 0x43480000;   // 0x9c58e4
constexpr std::uint32_t kDegToHeading = 0x3b360b61; // 0x9c58e0: 1/360
constexpr std::uint32_t kHeadingScale = 0x43ff8000; // 0x9c58dc: 511
constexpr std::uint32_t kWeightScale = 0x3ca3d70a;  // 0x9c58fc: 0.02
constexpr std::uint32_t kOne = 0x3f800000;          // 0x9c3390
constexpr std::uint32_t kMinusEpsilon = 0xb4000000; // 0x9c338c: -2^-23
constexpr std::uint32_t kEpsilon = 0x34000000;      // 0x9c3388
constexpr std::uint32_t kSphereVtable = 0x9c5868;
constexpr std::uint32_t kLineVtable = 0xa1edbc;     // CCollisionInfoLineSegmentVisibility
constexpr std::uint32_t kFloorActorFilter = 0x476280;
constexpr std::uint32_t kFloorCallback = 0x475f40;  // CollsnCallbackForFloor

float as_float(std::uint32_t bits) { return std::bit_cast<float>(bits); }

// The x87 helpers. A memory operand keeps its signalling-NaN flag; a value
// already loaded (or a result) is quiet.
struct Ops {
    const Fpu& f;
    static X87 mem(std::uint32_t bits) { return X87(as_float(bits)); }
    static X87 st(double v) { return X87(v); }
    static X87 ld(std::uint32_t bits) { return X87(static_cast<double>(as_float(bits))); } // quiet on load
    static std::uint32_t store(double v) { return std::bit_cast<std::uint32_t>(static_cast<float>(v)); }
    // A copy through the FPU: a signalling NaN comes out quiet.
    static std::uint32_t q(std::uint32_t bits) {
        bool snan = (bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0 && !(bits & 0x00400000u);
        return snan ? bits | 0x00400000u : bits;
    }
    static std::uint32_t fild(std::int32_t v) { return store(static_cast<double>(v)); }
    double add(const X87& a, const X87& b) const { return x87_add(f, a, b); }
    double sub(const X87& a, const X87& b) const { return x87_sub(f, a, b); }
    double mul(const X87& a, const X87& b) const { return x87_mul(f, a, b); }
    double div(const X87& a, const X87& b) const { return x87_div(f, a, b); }
    // a (loaded) + b (a memory operand), stored as a float.
    std::uint32_t fadd(std::uint32_t a, std::uint32_t b) const { return store(add(ld(a), mem(b))); }
    // The integer a + b (a memory operand), stored as a float.
    std::uint32_t fiadd(std::int32_t a, std::uint32_t b) const {
        return store(add(st(static_cast<double>(a)), mem(b)));
    }
    double sqrt(double v) const {
        if (std::isnan(v)) return v;
        if (v < 0) return -std::numeric_limits<double>::quiet_NaN();
        double r = std::sqrt(v);
        if (f.precision() == FpuPrecision::Double53 || r == 0 || !std::isfinite(r)) return r;
        int e = 0;
        std::frexp(r, &e);
        double scale = std::ldexp(1.0, 1 - e);
        return static_cast<double>(static_cast<float>(r * scale)) / scale;
    }
};

std::int32_t ftol(double v) { return Fpu::ftol(v); }

// The inlined effect total (0x43e9d5): buffs + AAs (+ worn items for a PC, or
// a spawn with +0x38c or +0x208 set).
std::int32_t cached_total(Character* c, std::int32_t spa, HitQueries& q) {
    if (c->cache_valid == 0) q.recache_spell_effects(c);
    std::int32_t total = q.cached_buff_effect(c, spa);
    total = static_cast<std::int32_t>(static_cast<std::uint32_t>(total) +
                                      static_cast<std::uint32_t>(q.cached_aa_effect(c, spa)));
    Spawn* s = c->spawn;
    if (s != nullptr && (s->type == 0 || s->owner_38c != 0 || s->flag_208 != 0))
        total = static_cast<std::int32_t>(static_cast<std::uint32_t>(total) +
                                          static_cast<std::uint32_t>(q.cached_item_effect(c, spa)));
    return total;
}

// The first active buff whose spell has `spa` before its first blank (254)
// effect, or null.
Affect* find_buff_with(Character* self, std::int32_t spa, HitQueries& q) {
    for (std::uint32_t i = 0; i < q.max_effects(self); ++i) {
        Affect* e = q.get_effect(self, i);
        if (e->type == 0) continue;
        const Spell* s = q.spell_by_id(static_cast<std::uint32_t>(e->spell_id));
        if (s == nullptr) continue;
        for (int k = 0; k < 12; ++k) {
            if (s->attrib[static_cast<std::size_t>(k)] == 0xfe) break;
            if (s->attrib[static_cast<std::size_t>(k)] == spa) return e;
        }
    }
    return nullptr;
}

// EQ_Affect helpers (0x7e3db0..0x7e3e20).
void slot_set(Affect* a, std::uint32_t index, std::int32_t v) {
    if (index <= 0xb) a->slot_data[index] = v;
}
std::int32_t slot_get(const Affect* a, std::uint32_t index) { return index > 0xb ? 0 : a->slot_data[index]; }
void slot_add(Affect* a, std::uint32_t index, std::int32_t v) {
    // The original does not range-check; HitBySpell's indexes are 0..11.
    a->slot_data[index % 12] = static_cast<std::int32_t>(static_cast<std::uint32_t>(a->slot_data[index % 12]) +
                                                         static_cast<std::uint32_t>(v));
}

// A cure spends against the remaining counter data of one buff: EQEmu keeps one pool per buff
// (zone/spell_effects.cpp), so all the counter effects of that buff count towards the total, with the
// cure's own type drained first. Like RoF2's HitBySpell, only the first matching buff is touched; any
// surplus is lost rather than carried to a later buff.
// A deliberate change to a RoF2 rule: HitBySpell @0x43e8c0 spends only the first counter slot of the first buff
// GetPCSpellAffect @0x433fd0 finds and removes that buff itself (RemovePCAffectex) once the slot drops below 1.
// Against EQEmu, whose OP_Buff carries no slot_data, that removed debuffs the server kept. The original rule stays
// behind HitState::rof2_single_slot_cures (the recorded-vector replay).
void cure_buff_counters(Character* self, HitQueries& q, std::int32_t spa, std::int32_t val,
                        std::uint8_t faded_counter_spas) {
    // EQEmu sends OP_Buff fade=1 for the cured buff before the cure's OP_Action, in the same network update. The
    // slot it removed is already gone, so the first matching buff left in the client's list is a different one.
    // Spending this cure against it would drain a debuff the server did not cure. When a buff of this counter type
    // faded this update, the server has applied the cure: leave the remaining buffs' counters untouched.
    const int index = counter_spa_index(spa);
    if (index >= 0 && (faded_counter_spas & (1u << index)) != 0) return;
    std::int64_t cure = -static_cast<std::int64_t>(val);
    const auto slots = q.max_effects(self);
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        Affect* buff = q.get_effect(self, slot);
        if (buff == nullptr || buff->type == 0) continue;
        const Spell* other = q.spell_by_id(static_cast<std::uint32_t>(buff->spell_id));
        if (other == nullptr) continue;
        bool matches = false;
        for (std::uint32_t i = 0; i < other->attrib.size(); ++i) {
            if (!is_counter_spa(other->attrib[i])) continue;
            if (other->attrib[i] == spa && other->base[i] > 0) matches = true;
        }
        if (!matches) continue;
        // Counters from zone-in are unknown: leave removal to the server instead of emptying the icon.
        if (!buff->counters_known) break;
        // The server owns buff removal: EQEmu sends OP_Buff fade=1 before the cure's OP_Action, so by the time the
        // client computes the cure the slot is normally gone already. A local remove here would only fire on a
        // mispredicted over-cure (the client's counters are lower than the server's) and would then hide a buff
        // the server kept. Spend the counters locally instead - the server sends slot_data as zeros - and leave
        // the buff standing for the server's fade to clear. Every slot is capped at zero by the subtraction.
        for (bool matching : {true, false}) {
            for (std::uint32_t i = 0; i < other->attrib.size() && cure > 0; ++i) {
                const auto effect = other->attrib[i];
                if (!is_counter_spa(effect) || (effect == spa) != matching) continue;
                const auto count = slot_get(buff, i);
                if (count <= 0) continue;
                const auto spent = static_cast<std::int32_t>(cure < count ? cure : count);
                slot_set(buff, i, count - spent);
                cure -= spent;
            }
        }
        return;
    }
}

// Vec3_Normalize @0x441bb0 on v; returns the length left on the x87 stack (or 0).
double normalize(std::array<std::uint32_t, 3>& v, const Ops& o) {
    // loads y (+4), x (+0), z (+8): x*x into x's slot, y*y into y's, y*y + x*x, + z*z.
    double xx = o.mul(Ops::ld(v[0]), Ops::ld(v[0]));
    double yy = o.mul(Ops::ld(v[1]), Ops::ld(v[1]));
    double s = o.add(Ops::st(yy), Ops::st(xx));
    double zz = o.mul(Ops::ld(v[2]), Ops::ld(v[2]));
    double len = o.sqrt(o.add(Ops::st(s), Ops::st(zz)));
    // Normalised when -eps >= len or unordered.
    bool unordered = std::isnan(len);
    double lo = as_float(kMinusEpsilon), hi = as_float(kEpsilon);
    bool normalise = unordered || lo >= len;
    if (!normalise) normalise = !(hi > len); // eps < len or eps == len
    if (!normalise) {
        v = {0, 0, 0};
        return 0.0;
    }
    double inv = o.div(Ops::st(1.0), Ops::st(len));
    v[0] = Ops::store(o.mul(Ops::ld(v[0]), Ops::st(inv)));
    v[1] = Ops::store(o.mul(Ops::st(inv), Ops::mem(v[1])));
    v[2] = Ops::store(o.mul(Ops::st(inv), Ops::mem(v[2])));
    return len;
}

// CCollisionInfoBase::CCollisionInfoBase @0x441c90 (from, to).
void info_ctor(CollisionInfo& c, const std::array<std::uint32_t, 3>& from, const std::array<std::uint32_t, 3>& to,
               const Ops& o) {
    c.vtable = 0xa1e99c;
    for (int i = 0; i < 3; ++i) {
        c.from[static_cast<std::size_t>(i)] = Ops::q(from[static_cast<std::size_t>(i)]);
        c.to[static_cast<std::size_t>(i)] = Ops::q(to[static_cast<std::size_t>(i)]);
    }
    // to.y - from.y, to.z - from.z, to.x - from.x (to loaded, from a memory operand).
    c.delta[1] = Ops::store(o.sub(Ops::ld(to[1]), Ops::mem(from[1])));
    c.delta[2] = Ops::store(o.sub(Ops::ld(to[2]), Ops::mem(from[2])));
    c.delta[0] = Ops::store(o.sub(Ops::ld(to[0]), Ops::mem(from[0])));
    c.mask_64 = -1;
    c.fraction = kOne;
    c.direction = {Ops::q(c.delta[0]), Ops::q(c.delta[1]), Ops::q(c.delta[2])};
    double len = normalize(c.direction, o);
    // 0 >= len or unordered -> +0x34 = 0, not moving.
    bool moving = !std::isnan(len) && 0.0 < len;
    c.inverse_length = moving ? Ops::store(o.div(Ops::st(1.0), Ops::st(len))) : 0u;
    c.moving = moving ? 1 : 0;
}

// Vec3_Cross @0x441c10: out = a x b in the original's operand order.
std::array<std::uint32_t, 3> cross(const std::array<std::uint32_t, 3>& a, const std::array<std::uint32_t, 3>& b,
                                   const Ops& o) {
    std::array<std::uint32_t, 3> out{};
    out[0] = Ops::store(o.sub(Ops::st(o.mul(Ops::ld(a[1]), Ops::mem(b[2]))),
                              Ops::st(o.mul(Ops::ld(b[1]), Ops::mem(a[2])))));
    out[1] = Ops::store(o.sub(Ops::st(o.mul(Ops::ld(a[2]), Ops::mem(b[0]))),
                              Ops::st(o.mul(Ops::ld(a[0]), Ops::mem(b[2])))));
    out[2] = Ops::store(o.sub(Ops::st(o.mul(Ops::ld(b[1]), Ops::mem(a[0]))),
                              Ops::st(o.mul(Ops::ld(a[1]), Ops::mem(b[0])))));
    return out;
}

// Matrix3x3_TransformVec3 @0x441dd0: m (9 floats) times v.
std::array<std::uint32_t, 3> transform(const std::array<std::uint32_t, 9>& m, const std::array<std::uint32_t, 3>& v,
                                       const Ops& o) {
    std::array<std::uint32_t, 3> out{};
    double a = o.mul(Ops::ld(m[3]), Ops::mem(v[1]));
    double b = o.mul(Ops::ld(v[0]), Ops::mem(m[0]));
    double s = o.add(Ops::st(a), Ops::st(b));
    s = o.add(Ops::st(s), Ops::st(o.mul(Ops::ld(m[6]), Ops::mem(v[2]))));
    out[0] = Ops::store(s);
    a = o.mul(Ops::ld(m[1]), Ops::mem(v[0]));
    b = o.mul(Ops::ld(m[4]), Ops::mem(v[1]));
    s = o.add(Ops::st(a), Ops::st(b));
    s = o.add(Ops::st(s), Ops::st(o.mul(Ops::ld(m[7]), Ops::mem(v[2]))));
    out[1] = Ops::store(s);
    a = o.mul(Ops::ld(m[2]), Ops::mem(v[0]));
    b = o.mul(Ops::ld(m[5]), Ops::mem(v[1]));
    s = o.add(Ops::st(a), Ops::st(b));
    s = o.add(Ops::st(s), Ops::st(o.mul(Ops::ld(m[8]), Ops::mem(v[2]))));
    out[2] = Ops::store(s);
    return out;
}

void read_bits(const SpellCheckRecord& r, std::size_t at, std::array<std::uint32_t, 3>& v) {
    std::memcpy(v.data(), r.data() + at, 12);
}
void write_bits(SpellCheckRecord& r, std::size_t at, const std::array<std::uint32_t, 3>& v) {
    std::memcpy(r.data() + at, v.data(), 12);
}

// The local (and not controlled) or the controlled player was pushed (0x440963).
void flag_pushed(Spawn* me, Globals& g) {
    if (me == g.local_player && g.local_player != g.controlled_player) g.local_pushed = 1;
    else if (me == g.controlled_player) g.controlled_pushed = 1;
}

// SPA 160 (0x43f612): drunkenness += |value| / (weight * 0.02 + 1), at most 200.
void drunk(std::int32_t value, HitQueries& q, const Ops& o) {
    Handle pc = q.cb_pc();
    std::int32_t weight = q.current_profile()->weight;
    std::int32_t magnitude =
        value < 0 ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(value)) : value;
    double d = o.mul(Ops::st(static_cast<double>(weight)), Ops::mem(kWeightScale));
    d = o.add(Ops::st(d), Ops::mem(kOne));
    std::int32_t amount = ftol(o.div(Ops::st(static_cast<double>(magnitude)), Ops::st(d)));
    std::int32_t now = q.get_drunkenness(pc);
    q.set_drunkenness(pc, static_cast<std::int32_t>(static_cast<std::uint32_t>(now) +
                                                    static_cast<std::uint32_t>(amount)));
    if (q.get_drunkenness(pc) > 0xc8) q.set_drunkenness(pc, 0xc8);
}

void set_bind(Handle pc, std::uint32_t slot, Character* self, Spawn* where, HitQueries& q) {
    q.set_bind_zone(pc, slot, self->base->zone_id);
    q.set_bind_y(pc, slot, Ops::q(where->y));
    q.set_bind_x(pc, slot, Ops::q(where->x));
    q.set_bind_z(pc, slot, Ops::q(where->z));
    q.set_bind_heading(pc, slot, Ops::q(where->heading));
}

// CDisplay::GetPlayerFloorHeight; unless it answers exactly -1e27 the spawn
// gets FloorHeight = floor and z = floor + its height (0x4403f5 / 0x44069b).
void floor_snap(Spawn* s, HitQueries& q, const Ops& o) {
    double floor = q.floor_height(s, Ops::q(s->y), Ops::q(s->x), Ops::q(s->z));
    if (floor == static_cast<double>(as_float(kNoFloor))) return;
    s->floor_z = Ops::store(floor);
    s->z = Ops::store(o.add(Ops::st(floor), Ops::mem(s->model_height)));
}

bool is_song_on_self(const Spell* spell, Spawn* caster, Spawn* me) {
    return (spell->target_type == 0xd || spell->target_type == 0x14) && caster == me;
}

} // namespace

bool is_valid_spell_id(std::uint32_t id) { return id - 1 <= 0xafc7; }

std::uint8_t spell_affects(const Spell* spell, std::int32_t spa) {
    if (!spell) return 0;
    for (int i = 0; i < 12; ++i)
        if (spell->attrib[static_cast<std::size_t>(i)] == spa) return static_cast<std::uint8_t>(i + 1);
    return 0;
}

bool effect_index_used(const Spell& spell, std::uint32_t index) {
    std::int32_t spa = spell.attrib[index % 12];
    if (spa == 0xfe) return false;
    return !(spa == 0xa && spell.base[index % 12] == 0);
}

std::uint8_t hit_by_spell(HitState& state, const Hit& hit, const Launch* launch, std::int32_t duration,
                          HitQueries& q, const Fpu& fpu) {
    const Ops o{fpu};
    Character* self = state.self;
    Globals& g = *state.globals;
    auto me_spawn = [&] { return self->spawn; };
    HitFrame local_frame;
    HitFrame& frame = state.frame ? *state.frame : local_frame;
    SpellCheckRecord& record = frame.record; // esp+0x2e8
    char* const text128 = frame.text.data(); // esp+0x128
    constexpr std::size_t kText = sizeof(HitFrame::text);

    if (me_spawn() == nullptr || launch == nullptr) return 0;
    const std::uint32_t spell_id = hit.spell_id;
    if (!is_valid_spell_id(spell_id)) return 0;
    const Spell* spell = q.spell_by_id(spell_id);
    if (spell == nullptr) return 0;
    Spawn* caster = q.spawn_by_id(hit.caster_id);
    if (caster == nullptr) return 0;
    Character* caster_character = nullptr;
    std::uint8_t me_flag = 0;
    {
        bool portal = spell->attrib[0] == 0x91;
        me_flag = me_spawn()->flag_208;
        caster_character = caster->character;
        std::int32_t divine = cached_total(self, 0x28, q);
        if (divine > 0 && spell_id != 0x3d6 && !portal && spell->flag_240 == 0) return 0;
    }
    // The caster level byte (esp+0x98), pushed as a dword with stale upper bytes.
    std::uint8_t level = hit.caster_level;
    if (q.bard_cast_bard(spell, caster->class_id) != 0) {
        if (Character* c = caster->character)
            level = static_cast<std::uint8_t>(level + static_cast<std::uint8_t>(cached_total(c, 0x110, q)));
    }
    if (q.sub_00433830(spell, caster->character)) {
        if (caster->class_id == 8) return 0;
        Handle pc = q.character_of(caster);
        q.token_to_client(pc != 0 ? pc + kPcCharacter : 0, 0xc5, 0x121);
        return 0;
    }
    if (q.is_item_cast(launch) && hit.item_level != 0) level = static_cast<std::uint8_t>(hit.item_level);
    if (caster == me_spawn() && spell->attrib[0] == 0x17) return 0;
    if (spell_id == 0x29a || spell_id == 0x2a2) {
        if (static_cast<std::int32_t>(q.cb_level()) < 0x2e) {
            Spawn* me = me_spawn();
            if (me == nullptr || me->type != 0) return 0;
            q.token_to_client(reinterpret_cast<Handle>(self), 0xfc, 0x121);
            return 0;
        }
    }
    if (spell->target_type == 0x14 || (spell->recourse_178 != 0 && spell->spell_type == 0)) {
        Spawn* me = me_spawn();
        if (me != nullptr && me->type == 0 && caster->type == 0) {
            Handle theirs = q.character_of(caster);
            Handle mine = q.character_of(me_spawn());
            if (theirs != mine) {
                if (g.ruleset != 0) {
                    if (theirs != 0 && mine != 0) {
                        // The PcClient's name (+8 vbtable, +0x44): the character's own.
                        if (q.is_self_or_group_member(theirs + kPcCharacter, q.pc_name(mine))) {
                            q.token_to_client(theirs + kPcCharacter, 0x8c2, 0x121);
                            return 0;
                        }
                    }
                } else {
                    if (theirs == 0) return 0;
                    q.token_to_client(theirs + kPcCharacter, 0xd2, 0x121);
                    return 0;
                }
            }
        }
    }
    if (q.do_spell_check(spell_id, record) == 0) {
        q.log_exit_game();
        q.execute_cmd(0x1e5);
    }
    if (hit.word_1b != 0) {
        if (const Spell* other = q.spell_by_id(hit.word_1b))
            if (other->recourse_178 != 0) q.spell_by_id(other->recourse_178);
    }
    {
        bool check = !is_song_on_self(spell, caster, me_spawn());
        if (check && spell->spell_type == 1 && caster->type == 0 && me_spawn()->type == 0) {
            const std::int32_t first = spell->attrib[0];
            bool group = first == 0xc || first == 0x1c || first == 0x1d || first == 0x13a || first == 0x13b ||
                         first == 0x13c || spell_id == 0x628 || spell_id == 0xe6e;
            if (!group && spell_affects(spell, 0x1b) && !q.spawn_vf_dc(caster)) group = true;
            if (group) {
                Handle pc = q.cb_pc();
                if (!q.is_self_or_group_member(pc + kPcCharacter, caster->name.data())) return 0;
            }
        }
    }
    if (spell->spell_type == 0) {
        static constexpr std::int32_t kCleared[] = {0xc, 0x1c, 0x1d, 0x13a, 0x13b, 0x13c};
        for (std::int32_t cleared : kCleared) q.remove_affect(find_buff_with(self, cleared, q));
    }
    Spawn* const me_at_start = me_spawn(); // esp+0x68
    q.cur_hp(me_at_start->type);
    std::int32_t dur = 0;
    if (duration > 0) {
        dur = duration;
    } else if (spell->spell_type == 0 && q.is_pc_or_mercenary(me_spawn()) && q.is_pc_or_mercenary(caster)) {
        dur = q.pvp_spell_duration(spell, level);
    } else {
        dur = q.spell_duration(spell, level);
    }
    if (spell->spell_type == 0 && spell_affects(spell, 0x7b)) {
        std::uint8_t at = spell_affects(spell, 0x7b);
        std::int32_t total = cached_total(self, 0x7b, q);
        std::uint32_t cb_level = q.cb_level();
        std::int32_t change = q.calc_affect_change(spell, cb_level, static_cast<std::uint8_t>(at - 1), caster, 1);
        if (static_cast<std::int32_t>(static_cast<std::uint32_t>(change) + static_cast<std::uint32_t>(total)) >= 0) {
            q.build_string(reinterpret_cast<char*>(record.data()), 0x30a0, spell->name.data());
            q.dsp_chat(reinterpret_cast<const char*>(record.data()), 0x121);
            return 0;
        }
    }

    if (spell->duration_formula == 0) {
        // ---- loop 1: an instant spell
        Handle pc_for_bind = 0;
        if (me_spawn()->type == 0) pc_for_bind = q.cb_pc();
        for (std::uint32_t index = 0; index < 12; ++index) {
            if (me_spawn() == nullptr) return 0;
            if (!effect_index_used(*spell, index)) continue;
            std::int32_t val = q.calc_affect_change(spell, level, index, caster,
                                                    spell->attrib[index] != 0xc1 ? 1u : 0u);
            if (spell_id != 0xad7) {
                if (caster_character != nullptr && spell_id == 0x57) {
                    if (q.get_level(caster) > 0x28) {
                        std::int32_t total = cached_total(caster_character, 0x11c, q);
                        if (total > 0) {
                            val = total;
                        } else {
                            std::int32_t x = (static_cast<std::int32_t>(q.get_level(caster)) - 0x28) * 0x2c;
                            val = static_cast<std::int32_t>(static_cast<std::uint32_t>(val) +
                                                            static_cast<std::uint32_t>(x < 0 ? 0 : x)); // add: wraps
                        }
                    }
                } else {
                    bool done = false;
                    if ((spell_id == 0x58 || spell_id == 0xad6) && q.get_level(caster) > 0x28) {
                        done = true;
                        std::int32_t lv = q.get_level(caster);
                        std::int32_t x = (lv * 5 - 0xc8) * 4;
                        x = x < 0 ? 0 : (x > 0x1f4 ? 0x1f4 : x);
                        Character* c = caster->character;
                        val = static_cast<std::int32_t>(static_cast<std::uint32_t>(val) - static_cast<std::uint32_t>(x));
                        if (c != nullptr) {
                            std::int32_t n = q.max_effects(c);
                            for (std::int32_t j = 0; j < n; ++j) {
                                Affect* e = q.get_effect(c, static_cast<std::uint32_t>(j));
                                if (e->spell_id == 0x11a8) {
                                    val = core::wrapped_div(static_cast<std::uint32_t>(val) * 6u, 4);
                                    break;
                                }
                            }
                        }
                    }
                    if (!done && spell_id == 0x2e6 && q.get_level(caster) > 0x2d) {
                        std::int32_t lv = q.get_level(caster);
                        std::int32_t x = (lv * 5 - 0xe1) * 2;
                        x = x < 0 ? 0 : (x > 0xc8 ? 0xc8 : x);
                        val = static_cast<std::int32_t>(static_cast<std::uint32_t>(val) - static_cast<std::uint32_t>(x));
                    }
                }
            }
            if (is_song_on_self(spell, caster, me_spawn())) {
                val = hit.song_value;
            } else if (spell->attrib[index] != 0x1b && as_float(hit.modifier) > 0.0f) {
                // 0x43f562: value * modifier, truncated.
                val = ftol(o.mul(Ops::st(static_cast<double>(val)), Ops::mem(hit.modifier)));
            }
            if (me_spawn() == nullptr) return 0;
            const std::int32_t spa = spell->attrib[index];
            switch (spa) {
            case 160: {
                Spawn* me = me_spawn();
                if (me == nullptr || me->type != 0) break;
                drunk(val, q, o);
                break;
            }
            case 20: { // a negative value may cure one blindness buff
                if (val >= 0) break;
                Affect* e = find_buff_with(self, 0x14, q);
                if (e == nullptr) break;
                std::int32_t chance = (static_cast<std::int32_t>(level) - e->level) * 5 + 0x32;
                chance = chance < 5 ? 5 : (chance > 0x5f ? 0x5f : chance);
                if (chance >= q.random_percent()) q.remove_affect(e);
                break;
            }
            case 68: { // the target's properties (answers unused)
                Spawn* t = g.target;
                if (t == nullptr || t->type != 1 || me_flag != 0 || t->owner_38c != me_spawn()->spawn_id) break;
                if (q.has_property(t, 0x1b)) break;
                if (q.has_property(g.target, 8)) break;
                q.has_property(g.target, 0x1c);
                break;
            }
            case 25: { // bind affinity
                if (me_spawn()->type != 0 || pc_for_bind == 0) return 0;
                std::uint32_t slot = static_cast<std::uint32_t>(val) - 1u;
                if (static_cast<std::int32_t>(slot) < 0 || static_cast<std::int32_t>(slot) >= 5) slot = 0;
                if (q.get_150(me_spawn()) != 0) {
                    q.token_to_client(reinterpret_cast<Handle>(self), 0x1954, 0x121);
                    return 0;
                }
                if (g.bind_blocked != 0) {
                    q.token_to_client(reinterpret_cast<Handle>(self), 0x69, 0x121);
                    return 0;
                }
                Spawn* where = me_at_start;
                auto zone_allows = [&] { return g.zone_type == 2 || g.zone_type == 3 || g.zone_type == 4; };
                if (caster != me_spawn() && !q.spawn_vf_dc(caster)) {
                    if (!zone_allows() && q.bind_spot_ok(where) == 0) {
                        q.token_to_client(reinterpret_cast<Handle>(self), 0x103, 0x121);
                        return 0;
                    }
                    set_bind(pc_for_bind, slot, self, where, q);
                    return 0;
                }
                std::uint8_t expansion;
                if (g.ruleset == 6) {
                    expansion = 1;
                } else if (g.zone_type == 1) {
                    expansion = 0;
                } else {
                    expansion = q.expansion_zone(self->base->zone_id) != 0;
                    if (g.ruleset == 4) expansion = 1;
                }
                if (!q.spawn_vf_dc(caster) && expansion != 0) {
                    if (!zone_allows() && q.bind_spot_ok(where) == 0) {
                        q.token_to_client(reinterpret_cast<Handle>(self), 0x103, 0x121);
                        return 0;
                    }
                }
                set_bind(pc_for_bind, slot, self, where, q);
                g.save_reason = 7;
                q.save_pc();
                break;
            }
            case 83:
            case 145: { // teleport / gate to a zone
                if (me_spawn()->type != 0 || pc_for_bind == 0) return 0;
                if (spell->teleport_zone[0] == 0) break;
                q.pc_base_vf58(pc_for_bind + kPcBase);
                Spawn* me = me_at_start;
                q.set_150(me);
                std::uint32_t x = Ops::fild(spell->base[0]);
                std::uint32_t y = Ops::fild(spell->base[1]);
                std::uint32_t z = Ops::fild(spell->base[2]);
                std::int32_t heading = spell->base[3];
                q.play_sound(me, 0x89);
                std::int32_t zone = q.zone_index(spell->teleport_zone.data());
                if (self->base->zone_id != zone) {
                    q.move_to_zone(zone, Ops::q(x), Ops::q(y), Ops::q(z), heading);
                    break;
                }
                me->y = Ops::q(x);
                me->x = Ops::q(y);
                me->z = Ops::q(z);
                q.record_position_sample(me, 3);
                self->base->y = Ops::q(x);
                self->base->x = Ops::q(y);
                self->base->z = Ops::q(z);
                std::uint32_t h = Ops::fild(heading);
                me->heading = h;
                self->base->heading = h;
                if (spell_id != 0x5c4) floor_snap(me, q, o);
                q.update_player_actor(me);
                break;
            }
            case 42:
                q.sub_00432bc0(Ops::q(spell->range));
                break;
            case 72: { // tell the server about the target
                Spawn* t = g.target;
                if (t == nullptr || t->type < 2) {
                    q.dsp_chat(q.get_string(0x306f), 0x111);
                    break;
                }
                if (g.connected) {
                    g.packet_opcode = q.hton(0x56b1);
                    g.packet_target = static_cast<std::int32_t>(g.target->spawn_id);
                    std::uint8_t sent = q.send_world(6);
                    --g.send_count;
                    if (!sent) q.close_world();
                } else if (g.no_send_log == 0) {
                    q.log_void_connection(0x56b1);
                }
                q.build_string(text128, 0x30a1, g.target->name.data());
                q.dsp_chat(text128, 0x111);
                break;
            }
            case 189:
                q.set_endurance(val);
                q.cur_endurance();
                break;
            case 74:
                q.feign(spell_id, val);
                break;
            case 61:
            case 62: { // read a book
                std::strncpy(text128, q.get_string(0x30a4), kText - 1);
                bool has = q.get_item(0x21) != 0;
                q.release_item();
                if (!has) break;
                char* const lore = frame.lore.data();
                std::strncpy(lore, q.lore_name(q.get_item(0x21)), sizeof(HitFrame::lore) - 1);
                q.release_item();
                auto special = [](char c) { return c == 0x7e || c == 0x2a || c == 0x23 || c == 0x26 || c == 0x21; };
                if (!special(lore[0])) {
                    const char* name = q.lore_name(q.get_item(0x21));
                    q.build_string(text128, 0x1a6d, name);
                    q.release_item();
                } else if (!special(lore[1])) {
                    q.build_string(text128, 0x1a6d, lore + 1);
                } else {
                    q.build_string(text128, 0x1a6d, lore + 2);
                }
                bool book = q.item_type(q.get_item(0x21)) == 2;
                q.release_item();
                if (book) {
                    std::uint32_t lang = q.book_lang(q.get_item(0x21));
                    const char* text = q.lang_desc(lang);
                    q.build_string(reinterpret_cast<char*>(record.data()), 0x30a2, text);
                    q.release_item();
                    q.dsp_chat_2(reinterpret_cast<const char*>(record.data()));
                }
                q.dsp_chat(text128, 0x108);
                break;
            }
            case 56: { // stop turning
                me_spawn()->heading = 0;
                me_spawn()->heading_speed = 0;
                if (Spawn* m = q.mount_spawn(me_spawn())) {
                    m->heading = 0;
                    m->heading_speed = 0;
                }
                break;
            }
            case 38: { // identify
                std::strncpy(text128, q.get_string(0x30a4), kText - 1);
                bool has = q.get_item(0x21) != 0;
                q.release_item();
                if (has) {
                    std::strncpy(text128, q.get_string(0x61a), kText - 1);
                    bool plain = q.item_type(q.get_item(0x21)) == 0;
                    q.release_item();
                    if (plain) {
                        std::uint8_t magic = q.is_magic(q.get_item(0x21));
                        q.release_item();
                        if (magic) std::strncpy(text128, q.get_string(0x30a3), kText - 1);
                    }
                }
                q.dsp_chat_2(text128);
                break;
            }
            case 358:
            case 15:
            case 417: { // mana; SPA 15 / 417 not for warriors, monks, rogues, bards, berserkers
                bool change = spa == 358 && q.current_profile()->class_id != 8;
                if (!change) {
                    if (q.current_profile()->class_id == 8 || q.current_profile()->class_id == 7 ||
                        q.current_profile()->class_id == 9 || q.current_profile()->class_id == 1 ||
                        q.current_profile()->class_id == 0x10)
                        break;
                }
                if (val < 0) {
                    if (static_cast<std::int32_t>(q.cb_max_mana() + static_cast<std::uint32_t>(val)) < 0)
                        val = static_cast<std::int32_t>(0u - q.cb_max_mana());
                }
                Profile* p = q.current_profile();
                p->mana = static_cast<std::int32_t>(static_cast<std::uint32_t>(p->mana) + static_cast<std::uint32_t>(val));
                q.cur_mana();
                break;
            }
            case kSpaDiseaseCounter:
            case kSpaPoisonCounter:
            case kSpaCurseCounter:
            case kSpaCorruptionCounter: { // remove counters from a matching buff
                if (val >= 0) break;
                if (!state.rof2_single_slot_cures) {
                    cure_buff_counters(self, q, spa, val, state.faded_counter_spas);
                    break;
                }
                // Original @0x43e8c0, for vector replay.
                Affect* e = q.pc_spell_affect(spa);
                if (e == nullptr) break;
                const Spell* other = q.spell_by_id(static_cast<std::uint32_t>(e->spell_id));
                if (other == nullptr) break;
                std::uint8_t k = spell_affects(other, spa);
                if (k == 0) break;
                --k;
                q.calc_affect_change(other, e->level, k, caster, 1);
                slot_add(e, k, val);
                if (slot_get(e, k) < 1) q.remove_pc_affect(e, caster->spawn_id);
                break;
            }
            case 389:
            case 390: { // refresh / delay spell gem timers by focus
                if (q.cb_pc() == 0) break;
                Handle pc = q.cb_pc();
                if (pc == 0) break;
                for (std::uint32_t gem = 0; gem < 12; ++gem) {
                    std::int32_t now = q.eq_get_time();
                    std::int32_t when = spa == 389 ? now : static_cast<std::int32_t>(static_cast<std::uint32_t>(val) +
                                                                                     static_cast<std::uint32_t>(now));
                    std::int32_t id = q.memorized_spell(pc + kPcBase, gem);
                    if (id > 0) {
                        const Spell* gem_spell = q.spell_by_id(static_cast<std::uint32_t>(id));
                        // (The original reads a null spell's +0x198 and faults; skipped here.)
                        if (q.spell_focus_pct(gem_spell, spell) && gem_spell != nullptr) {
                            if (std::int32_t* timers = reinterpret_cast<Pc*>(pc)->gem_timers) timers[gem] = when;
                            if (spa == 389) q.set_spell_recast_timer(pc, gem);
                            std::uint32_t linked = gem_spell->timer_id_198;
                            if (linked <= 0x13) {
                                std::uint32_t t = q.fast_time();
                                q.set_linked_reuse(pc, linked, spa == 389 ? t : t + static_cast<std::uint32_t>(val));
                            }
                        }
                    }
                }
                break;
            }
            case 343: { // interrupt casting
                if (q.random_below(100) >= spell->base[index]) break;
                Spawn* me = me_spawn();
                if (me == nullptr || me->casting_spell == -1) break;
                const Spell* cast = q.spell_by_id(static_cast<std::uint32_t>(me->casting_spell));
                if (cast == nullptr || cast->flag_240 != 0) break;
                q.stop_current_spell_cast();
                break;
            }
            case 379: { // shadow step: the farthest valid point along the heading, up to the distance
                q.record_position_sample(me_spawn(), 9);
                Spawn* s = me_spawn();
                // 0x4403a6: from (x, y, z + 2), to (x, y, z - 200).
                std::array<std::uint32_t, 3> from{Ops::q(s->y), Ops::q(s->x), 0};
                std::array<std::uint32_t, 3> to{Ops::q(s->y), Ops::q(s->x), 0};
                from[2] = Ops::store(o.add(Ops::ld(s->z), Ops::mem(kTwo)));
                to[2] = Ops::store(o.sub(Ops::ld(s->z), Ops::mem(kTwoHundred)));
                if (!q.point_in_world(from)) break;
                std::uint8_t actor_on = q.actor_collision_enabled();
                q.set_actor_collision(1);
                // CCollisionInfoSphere(from, 1.0, to, FloorActorFilter, CollsnCallbackForFloor, 0, 0, 1) at the
                // record: its normal is whatever the record holds there.
                CollisionSphere sphere;
                read_bits(record, 0x3c, sphere.info.normal);
                info_ctor(sphere.info, from, to, o);
                sphere.info.vtable = kSphereVtable;
                sphere.radius = kOne;
                sphere.centre = {Ops::q(from[0]), Ops::q(from[1]), Ops::q(from[2])};
                sphere.filter = kFloorActorFilter;
                sphere.callback = kFloorCallback;
                sphere.radius_sq = Ops::store(o.mul(Ops::ld(kOne), Ops::st(as_float(kOne))));
                sphere.byte_9c = 1;
                sphere.inverse_radius = Ops::store(o.div(Ops::st(1.0), Ops::ld(kOne)));
                q.collide_sphere(sphere);
                write_bits(record, 0x3c, sphere.info.normal);
                q.set_actor_collision(actor_on);
                // The floor's frame: n, n x (1,0,0), (n x (1,0,0)) x n.
                std::array<std::uint32_t, 3> n{Ops::q(sphere.info.normal[0]), Ops::q(sphere.info.normal[1]),
                                               Ops::q(sphere.info.normal[2])};
                std::array<std::uint32_t, 3> ex{kOne, 0, 0};
                std::array<std::uint32_t, 3> side = cross(n, ex, o);
                std::array<std::uint32_t, 3> fwd = cross(side, n, o);
                std::array<std::uint32_t, 9> m{};
                for (int i = 0; i < 3; ++i) {
                    m[static_cast<std::size_t>(i)] = Ops::q(fwd[static_cast<std::size_t>(i)]);
                    m[static_cast<std::size_t>(3 + i)] = Ops::q(side[static_cast<std::size_t>(i)]);
                    m[static_cast<std::size_t>(6 + i)] = Ops::q(n[static_cast<std::size_t>(i)]);
                }
                // 0x44052b: heading + base2 * (1/360) * 511, truncated, % 512.
                double turn = o.mul(Ops::st(static_cast<double>(spell->base2[index])), Ops::mem(kDegToHeading));
                turn = o.mul(Ops::st(turn), Ops::mem(kHeadingScale));
                std::int32_t heading = ftol(o.add(Ops::ld(me_spawn()->heading), Ops::st(turn))) % 512;
                std::array<std::uint32_t, 3> start{Ops::q(me_spawn()->y), Ops::q(me_spawn()->x),
                                                   o.fadd(me_spawn()->z, kTwo)};
                // 0x440567: the distance, while above 5, less 2 each step.
                std::int32_t base = spell->base[index];
                std::uint32_t distance = Ops::fild(base);
                bool more = static_cast<double>(base) > static_cast<double>(as_float(kFive));
                if (more) {
                    Spawn* me = me_at_start;
                    const std::uint32_t angle = Ops::fild(heading);
                    do {
                        std::uint32_t c = Ops::store(o.mul(Ops::st(q.table_cos(Ops::q(angle))), Ops::mem(distance)));
                        std::uint32_t sn = Ops::store(o.mul(Ops::st(q.table_sin(Ops::q(angle))), Ops::mem(distance)));
                        std::array<std::uint32_t, 3> step{Ops::q(c), sn, 0};
                        std::array<std::uint32_t, 3> p = transform(m, step, o);
                        p[0] = o.fadd(me_spawn()->y, p[0]);
                        p[1] = o.fadd(me_spawn()->x, p[1]);
                        p[2] = o.fadd(me_spawn()->z, p[2]);
                        // Ray_SetFromPoints(start, p) @0x441c50: the start and the direction p - start.
                        std::array<std::uint32_t, 6> ray{};
                        for (int i = 0; i < 3; ++i) ray[static_cast<std::size_t>(i)] = Ops::q(start[static_cast<std::size_t>(i)]);
                        ray[3] = Ops::store(o.sub(Ops::ld(p[0]), Ops::mem(ray[0])));
                        ray[4] = Ops::store(o.sub(Ops::ld(p[1]), Ops::mem(ray[1])));
                        ray[5] = Ops::store(o.sub(Ops::ld(p[2]), Ops::ld(start[2])));
                        std::array<std::uint32_t, 3> end{o.fadd(ray[3], ray[0]), o.fadd(ray[4], ray[1]),
                                                         o.fadd(ray[5], ray[2])};
                        CollisionLine line;
                        info_ctor(line.info, {ray[0], ray[1], ray[2]}, end, o);
                        line.byte_8c = 0;
                        for (int i = 0; i < 6; ++i) line.ray[static_cast<std::size_t>(i)] = Ops::q(ray[static_cast<std::size_t>(i)]);
                        line.info.vtable = kLineVtable;
                        line.info.mask_64 = 0;
                        bool blocked = false;
                        if (q.terrain_collision_enabled()) blocked = q.collide_line(line) != 0;
                        if (!blocked && q.is_valid_teleport(me_spawn(), Ops::q(p[0]), Ops::q(p[1]), Ops::q(p[2]),
                                                            Ops::q(angle), Ops::q(distance))) {
                            q.play_sound(me, 0x89);
                            Spawn* sp = me_spawn();
                            std::uint32_t x = Ops::q(p[0]), y = Ops::q(p[1]), z = Ops::q(p[2]);
                            sp->y = x;
                            sp->x = y;
                            sp->z = z;
                            self->base->y = x;
                            self->base->x = y;
                            self->base->z = z;
                            floor_snap(me_spawn(), q, o);
                            q.update_player_actor(me_spawn());
                        }
                        // 0x440860: d - 2, stored as a float, then compared with 5 unrounded.
                        double next = o.sub(Ops::ld(distance), Ops::mem(kTwo));
                        const std::uint32_t before = distance;
                        distance = Ops::store(next);
                        more = next > static_cast<double>(as_float(kFive));
                        // A deliberate difference: from 2^25 up a float's d - 2 can round back to d, and the
                        // original then loops here for ever (the distance alone decides the loop). Stop instead;
                        // every base that ends in the original ends here at the same step (docs/mechanics.md).
                        if (distance == before) more = false;
                    } while (more);
                }
                break;
            }
            case 380: { // pull toward the caster
                q.record_position_sample(me_spawn(), 7);
                Spawn* s = me_spawn();
                // 0x4408a2: dx, dy as floats; the distance from the unrounded squares.
                double dx = o.sub(Ops::ld(s->y), Ops::mem(caster->y));
                double dy = o.sub(Ops::ld(s->x), Ops::mem(caster->x));
                std::uint32_t fdx = Ops::store(dx), fdy = Ops::store(dy);
                double dyy = o.mul(Ops::st(dy), Ops::st(dy));
                double dxx = o.mul(Ops::st(dx), Ops::st(dx));
                std::uint32_t dist = Ops::store(o.sqrt(o.add(Ops::st(dxx), Ops::st(dyy))));
                const double b = static_cast<double>(spell->base[index]);
                std::uint32_t py = Ops::store(o.mul(Ops::st(o.div(Ops::ld(fdx), Ops::mem(dist))), Ops::st(b)));
                std::uint32_t px = Ops::store(o.mul(Ops::st(o.div(Ops::ld(fdy), Ops::mem(dist))), Ops::st(b)));
                Spawn* v = q.vehicle_of(me_spawn());
                Spawn* target = v != nullptr ? v : me_spawn();
                target->speed_y = o.fadd(py, target->speed_y);
                target->speed_x = o.fadd(px, target->speed_x);
                target->speed_z = o.fiadd(spell->base2[index], target->speed_z);
                flag_pushed(me_spawn(), g);
                break;
            }
            case 381: { // fling to the caster
                q.record_position_sample(me_spawn(), 7);
                FlingRecord r;
                r.words[0] = 1; // +0 byte 1
                r.words[2] = 1; // +8 byte 1
                r.words[3] = Ops::fild(spell->base[index]);
                r.words[4] = Ops::q(caster->y);
                r.words[5] = Ops::q(caster->x);
                r.words[6] = Ops::q(caster->z);
                r.words[7] = me_at_start->spawn_id;
                q.apply_fling(g.local_player, r);
                break;
            }
            default:
                break;
            }
        }
    } else {
        // ---- a buff: FindAffectSlot, the EQ_Affect, loop 2
        q.find_buff_level(spell_id);
        std::int32_t slot = 0;
        Affect* affect = q.find_affect_slot(spell_id, caster, &slot, level, launch->byte_32);
        if (affect != nullptr) {
            std::uint32_t modifier_used = kOne; // esp+0x18
            for (auto& v : affect->slot_data) v = 0; // EQ_Affect::FillSlotData(0)
            affect->counters_known = true;
            q.set_buff_caster_id(static_cast<std::uint32_t>(slot), caster->spawn_id);
            if (launch->item_flag != 0 && (spell_affects(spell, 0) || spell_affects(spell, 0x64)))
                affect->flags_24 |= 1u; // SetSlotBit(0)
            else
                affect->flags_24 &= ~1u; // ClearSlotBit(0)
            affect->type = is_song_on_self(spell, caster, me_spawn()) ? 4 : 2;
            affect->spell_id = static_cast<std::int32_t>(spell_id);
            affect->modifier = kOne;
            affect->level = level;
            affect->duration = dur;
            affect->byte_3 = spell->byte_26c;
            affect->caster_10 = 0;
            affect->dword_14 = 0;
            if (caster->type == 0 && caster->character != nullptr)
                affect->caster_10 = reinterpret_cast<Pc*>(q.character_of(caster))->id_20a8;
            else
                affect->caster_10 = hit.caster_id;
            if (q.has_buff_flag(spell)) q.on_new_buff(static_cast<std::uint32_t>(slot));
            self->cache_valid = 0;
            for (std::uint32_t index = 0; index < 12; ++index) {
                if (me_spawn() == nullptr) return 0;
                if (!effect_index_used(*spell, index)) continue;
                std::int32_t val = q.calc_affect_change(spell, level, index, caster, 1);
                std::int32_t spa = spell->attrib[index];
                if (spa != 0x3a && spa != 0x9c && spa != 0x1e && spa != 0x11 && spa != 0x12 && spa != 0x56 &&
                    as_float(hit.modifier) > 0.0f) {
                    modifier_used = Ops::q(hit.modifier);
                    // 0x440ec9: modifier * value, truncated.
                    val = ftol(o.mul(Ops::ld(hit.modifier), Ops::st(static_cast<double>(val))));
                }
                spa = spell->attrib[index];
                auto apply_modifier = [&] {
                    if (!(as_float(modifier_used) == 1.0f)) {
                        affect->modifier = Ops::q(hit.modifier);
                        self->cache_valid = 0;
                    }
                };
                switch (spa) {
                case 198:
                    q.set_endurance(val);
                    q.cur_endurance();
                    break;
                case 160: {
                    Spawn* me = me_spawn();
                    if (me == nullptr || me->type != 0) break;
                    drunk(val, q, o);
                    break;
                }
                case 31: { // mesmerize
                    std::int32_t refused = q.refused_mez(spell->max[index], caster, spell);
                    if (refused != 0) {
                        if (caster->type != 0) break;
                        Handle pc = q.character_of(caster);
                        if (pc == 0) break;
                        q.token_to_client(pc + kPcCharacter, refused == 1 ? 0xef : 0xf0, 0x121);
                        break;
                    }
                    if (Spawn* me = me_spawn()) {
                        me->rotation_speed = 0;
                        if (Spawn* m = q.mount_spawn(me_spawn())) m->rotation_speed = 0;
                    }
                    self->base->mesmerized = 1;
                    break;
                }
                case 20:
                case 34:
                    me_spawn()->flag_428 = 1;
                    break;
                case 99: { // root
                    if (Character* c = caster->character) q.root_aa(q.total_effect(c, 0xf4));
                    q.movement_rate(val);
                    break;
                }
                case 3: { // movement speed
                    apply_modifier();
                    if (q.total_effect(self, 0x63) == 0) q.movement_rate(q.total_effect(self, 3));
                    break;
                }
                case 55:
                case 78:
                case 163:
                    slot_set(affect, index, val);
                    break;
                case 58: { // illusion
                    std::uint8_t k = spell_affects(spell, 0x3a);
                    std::int32_t form = spell->base[static_cast<std::size_t>(k - 1) % 12];
                    if (q.change_form(spell, spell_id, form, affect) == 0) return 0;
                    break;
                }
                case 22: {
                    Spawn* me = me_spawn();
                    if (me == caster) return 0;
                    q.lose_control_message(me);
                    break;
                }
                case 23:
                    q.lose_control_message(me_spawn());
                    break;
                case 65:
                    q.current_profile()->dword_3248 = 0;
                    break;
                case 66:
                    q.current_profile()->dword_3264 = 0;
                    break;
                case 13:
                    q.update_all_visibility();
                    break;
                case 73: { // bind sight
                    Spawn* t = g.target;
                    if (t == nullptr) {
                        q.token_to_client(reinterpret_cast<Handle>(self), 0xd6, 0x121);
                        affect->modifier = kOne;
                        affect->spell_id = -1;
                        affect->type = 0;
                        affect->level = 0;
                        affect->duration = 0;
                        affect->byte_2 = 0;
                        slot_set(affect, index, 0);
                        self->cache_valid = 0;
                        return 0;
                    }
                    q.set_view_actor(t->name.data());
                    break;
                }
                case 87:
                    q.spa87(val);
                    break;
                case 64: // reads a base value it does not use
                    break;
                case 57:
                    q.spa57(spell->base2[index]);
                    break;
                case 74:
                    q.feign(spell_id, val);
                    break;
                case 12:
                case 28:
                case 29:
                case 314:
                case 315:
                case 316:
                    q.update_visible_status();
                    break;
                case kSpaDiseaseCounter:
                case kSpaPoisonCounter:
                case kSpaCurseCounter:
                case kSpaCorruptionCounter: { // counters
                    if (val > 0) {
                        slot_set(affect, index, val);
                        break;
                    }
                    if (val == 0) break;
                    if (!state.rof2_single_slot_cures) {
                        cure_buff_counters(self, q, spa, val, state.faded_counter_spas);
                        break;
                    }
                    if (q.random_low_high(1, 100) > 10) break;
                    Affect* e = q.pc_spell_affect(spell->attrib[index]);
                    if (e == nullptr) break;
                    const Spell* other = q.spell_by_id(static_cast<std::uint32_t>(e->spell_id));
                    if (other == nullptr) break;
                    std::int32_t change = q.calc_affect_change(other, e->level, index, caster, 1);
                    if (change <= 0 ||
                        static_cast<std::int32_t>(static_cast<std::uint32_t>(change) + static_cast<std::uint32_t>(val)) >= 7)
                        break;
                    slot_add(e, index, val);
                    if (slot_get(e, index) < 1) {
                        q.remove_pc_affect(e, 0);
                        Spawn* me = me_spawn();
                        if (me != nullptr && me->type == 0)
                            q.token_to_client(reinterpret_cast<Handle>(self), 0x109, 0x111);
                    }
                    break;
                }
                case 161:
                case 162:
                case 450:
                case 451:
                case 452:
                    slot_set(affect, index, spell->max[index]);
                    break;
                case 382: {
                    std::int32_t mask = spell->base[index];
                    if (mask == 0 || (spell->base[index] & 4) != 0) self->aa_cache_valid = 0;
                    break;
                }
                case 424: { // gravity / physics effect
                    if (me_spawn() == nullptr) break;
                    std::uint32_t id = caster->spawn_id;
                    std::int32_t generic = q.calc_affect_change_generic(spell, id, index, affect, spell->base[index]);
                    q.add_physics_effect(me_spawn(), spell_id, id, static_cast<std::uint32_t>(slot), generic);
                    break;
                }
                case 430:
                    q.apply_post_effect(spell, index);
                    break;
                case 436:
                    q.spa436();
                    break;
                case 441:
                    affect->y_18 = Ops::q(me_spawn()->y);
                    affect->x_1c = Ops::q(me_spawn()->x);
                    affect->z_20 = Ops::q(me_spawn()->z);
                    break;
                case 156:
                    break;
                default:
                    apply_modifier();
                    break;
                }
            }
        }
    }

    // ---- the push, and done
    if (!(as_float(spell->push_back) == 0.0f) || !(as_float(spell->push_up) == 0.0f)) {
        q.record_position_sample(me_spawn(), 7);
        Spawn* v = q.vehicle_of(me_spawn());
        // table->vf(angle) * push_back + the field (0x441106).
        auto push = [&](Spawn* s) {
            double c = o.mul(Ops::st(q.table_cos(Ops::q(hit.push_heading))), Ops::mem(spell->push_back));
            s->speed_y = Ops::store(o.add(Ops::st(c), Ops::mem(s->speed_y)));
            Spawn* s2 = v != nullptr ? v : me_spawn(); // re-read after the call
            double sn = o.mul(Ops::st(q.table_sin(Ops::q(hit.push_heading))), Ops::mem(spell->push_back));
            s2->speed_x = Ops::store(o.add(Ops::st(sn), Ops::mem(s2->speed_x)));
            Spawn* s3 = v != nullptr ? v : me_spawn();
            s3->speed_z = o.fadd(spell->push_up, s3->speed_z);
        };
        push(v != nullptr ? v : me_spawn());
        flag_pushed(me_spawn(), g);
    }
    self->stats_dirty = 1;
    return 1;
}

} // namespace hailstone::game::mechanics::hit
