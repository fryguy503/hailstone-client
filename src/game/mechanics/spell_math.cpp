// Spell math the client does itself: SpellDuration @0x434420, GetCastingTimeModifier @0x4336d0 and the SPA 127
// focus (GetFocusCastingTimeModifier @0x42c9a0 with the focus collectors and limit checks).
//
// Checked against the original's vectors: SpellDuration, GetCastingTimeModifier, CastTimeAfterReduction,
// GetBest*Focus, Collect*FocusEffects, GetFocusCastingTimeModifier, GetSpellFocusPct, GetAAFocusPct and
// CheckFocusLimit. From the disassembly: EQ_Character::IsSpellcaster_2 @0x4288e0,
// GetFocusLevelFalloffPct @0x428980, 0x4acd60 and the focus-type table 0x4acc50.
#include "game/mechanics/character.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "core/wrap_math.h"
#include "game/mechanics/item_stats.h"
#include "game/spa_ids.h"
#include "game/spells.h"

namespace hailstone::game::mechanics {

namespace {

std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::uint32_t u(std::int32_t v) { return static_cast<std::uint32_t>(v); }

constexpr float kPercent = 0.01f; // 0x9c4be4

// value * percent / 100 (a 32-bit product, divided toward zero).
std::int32_t scale(std::int32_t value, std::int32_t percent) { return core::wrapped_div(u(value) * u(percent), 100); }
// CastTimeShare: the spell's cast time times the value, over -100.
std::int32_t cast_time_share(const SpellData& spell, std::int32_t value) {
    return core::wrapped_div(spell.cast_time * u(value), -100);
}

// 0x4acc50: the focus effect types (an AA effect list's focus entries).
bool is_focus_type(std::int32_t type) {
    if (type == 0xa7 || (type >= 0x7c && type <= 0x85)) return true;
    switch (type) {
    case 235: case 286: case 287: case 296: case 297: case 302: case 303: case 310: case 335: case 339: case 383:
    case 392: case 393: case 394: case 395: case 396: case 398: case 399: case 413: case 420: case 421: case 433:
    case 434: case 435:
        return true;
    default:
        return false;
    }
}

// The limit types that must have a non-negative base to count (GetSpellFocusPct's count).
bool counts(std::int32_t type, std::int32_t base) {
    static constexpr std::array<std::int32_t, 10> kTypes = {135, 136, 137, 139, 385, 403, 404, 411, 414, 415};
    if (base < 0) return false;
    return std::find(kTypes.begin(), kTypes.end(), type) != kTypes.end();
}

struct LimitState {
    std::int32_t min = 100;
    bool flag = false;
    std::uint32_t pending[15] = {};
    std::uint32_t failed[15] = {};
    void step(std::int32_t result, bool counting, std::int32_t type) {
        std::uint32_t bit = 1u << (u(type) & 31);
        auto word = static_cast<std::size_t>(std::clamp(type / 32, 0, 14));
        if (result == 0 && counting) {
            if ((failed[word] & bit) == 0) pending[word] |= bit;
            return;
        }
        if (result < min) min = result;
        if (result > 0 && counting) {
            failed[word] |= bit;
            pending[word] &= ~bit;
        }
    }
    void note(std::int32_t type) { flag = flag || type == 0x19b; } // 0x4acd60
    void finish() {
        for (std::uint32_t w : pending)
            if (w != 0) {
                min = 0;
                return;
            }
    }
};

// EQ_Spell::GetSpellLevelNeeded @0x4af700 (classes 1..16 in spells_us.txt; other values read as 255 here).
std::int32_t level_needed(const SpellData& s, std::uint32_t cls) {
    if (cls - 1u > 34u) cls = 1;
    return cls <= 16 ? s.class_level[cls - 1] : 255;
}

bool match_or_exclude(std::int32_t value, std::int32_t base) {
    if (base > 0) return value == base;
    return static_cast<std::int32_t>(0u - u(value)) != base;
}

bool shielding_effect(std::int32_t e) { return e == 0x21 || e == 0x6a || e == 0x47; }

struct FocusPick {
    std::int32_t value = 0, percent = 100;
    const net::Item* item = nullptr; // item focus
    int kind = 0;                    // the item's spell slot (3 or 5)
    const SpellData* spell = nullptr;      // buff focus
    const SheetAAEffect* aa = nullptr;     // AA focus
};

// The collectors' output order: the negative values when there are any (most negative first), else the others
// (largest first); ties keep the earlier one.
const FocusPick* best_of(const std::vector<FocusPick>& all) {
    const FocusPick* best = nullptr;
    bool negatives = std::any_of(all.begin(), all.end(), [](const FocusPick& p) { return p.value < 0; });
    for (const FocusPick& p : all) {
        if (negatives != (p.value < 0)) continue;
        if (!best || (negatives ? p.value < best->value : p.value > best->value)) best = &p;
    }
    return best;
}

// SpellDuration's level formulas: the ticks at a caster level for types 1..15, nullopt for any other type.
std::optional<std::int32_t> duration_ticks(std::uint32_t type, std::int32_t level) {
    switch (type) {
    case 4: return 50;
    case 5: return 2;
    case 1: return std::max(level >> 1, 1);
    case 2: return std::max(level >> 1, 1) + 5;
    case 6: return (level >> 1) + 2;
    case 12: return std::max(level >> 2, 1);
    default: break;
    }
    // The rest grow linearly: per_level x level + at_zero.
    struct Linear {
        std::uint32_t type;
        std::int32_t per_level;
        std::int32_t at_zero;
    };
    static constexpr Linear kLinear[] = {{7, 1, 0},   {8, 1, 10},   {9, 2, 10}, {10, 3, 10}, {13, 4, 10},
                                         {14, 5, 10}, {15, 10, 100}, {3, 30, 0}, {11, 30, 90}};
    for (const Linear& f : kLinear)
        if (f.type == type) return f.per_level * level + f.at_zero;
    return std::nullopt;
}

} // namespace

std::int32_t Character::spell_duration(const SpellData& spell, std::uint8_t level, bool special) const {
    std::uint32_t type = spell.duration_formula;
    std::int32_t ticks = 0;
    if (type != 0) {
        if (type == 50) return -1;
        if (type == 51) return -4;
        if (auto by_level = duration_ticks(type, level)) {
            ticks = *by_level;
        } else {
            if (special) return -2;
            if (type < 200) return 0;
            ticks = static_cast<std::int32_t>(type); // 200 and up: the type is the duration
        }
        std::uint32_t cap = spell.duration_cap;
        if (cap != 0 && cap < static_cast<std::uint32_t>(ticks)) ticks = static_cast<std::int32_t>(cap);
    }
    for (std::int32_t calc : spell.formula)
        if (calc == 107 || calc == 108 || calc == 120 || calc == 122 || (calc > 1000 && calc < 2000)) return ticks;
    if (spell.spell_type != 0 && spell.duration_formula != 0 && sheet_.has_spawn &&
        std::find(spell.effect.begin(), spell.effect.end(), 58) != spell.effect.end() &&
        total_effect(238, true, 0, true, true) != 0)
        ticks = 10000;
    return ticks;
}

std::uint32_t cast_time_ms(const Character& ch, const SpellData& spell) {
    std::int64_t base = spell.cast_time;
    std::int64_t mod = static_cast<std::int64_t>(ch.casting_time_modifier(spell)) + ch.focus_casting_time_modifier(spell);
    std::int64_t t = (mod <= 0 && -mod >= base) ? 0 : base + mod;
    if (t < base / 2) t = base / 2;
    return static_cast<std::uint32_t>(t);
}

std::int32_t Character::casting_time_modifier(const SpellData& spell) const {
    if (!sheet_.has_spawn) return 0;
    std::uint32_t cast = spell.cast_time;
    if (sheet_.level > 50) {
        std::uint8_t cls = sheet_.class_id;
        bool hybrid = cls == 4 || cls == 5 || cls == 3 || cls == 15;
        if (hybrid && cast >= 3000 && spell.spell_type == 0) {
            // x87 CastTimeAfterReduction (@0x4337b7): min(n, 15.0f) * 3.0f, 100.0f - that, * 0.01f, times the cast
            // time loaded unsigned, truncated (chop) to 64 bits, the low dword kept.
            double n = static_cast<double>(sheet_.level - 50);
            if (!(15.0 >= n)) n = 15.0;
            double x = fpu_.mul(n, 3.0);
            x = fpu_.sub(100.0, x);
            x = fpu_.mul(x, static_cast<double>(0.01f));
            x = fpu_.mul(x, static_cast<double>(cast));
            cast = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::trunc(x))));
        }
    }
    return static_cast<std::int32_t>(cast - spell.cast_time);
}

// CharacterZoneClient::CheckFocusLimit @0x428d00.
std::int32_t Character::check_focus_limit(const SpellData& spell, std::int32_t limit, std::int32_t base,
                                          std::int32_t base2, std::uint32_t slots) const {
    bool pass = true;
    switch (limit) {
    case 0x86: { // max level, with a falloff per level over
        std::int32_t needed = level_needed(spell, sheet_.class_id);
        if (needed <= base) return 100;
        if (base2 == 0) return 0;
        std::uint32_t over = u(needed) - u(base);
        return static_cast<std::int32_t>(100u - over * u(base2));
    }
    case 0x8e: pass = level_needed(spell, sheet_.class_id) >= base; break;
    case 0x87: pass = spell.resist == base; break;
    case 0x88: pass = match_or_exclude(spell.target_type, base); break;
    case 0x89: { // has / has not the effect |base|; 0x21, 0x6a and 0x47 count as one another
        bool include = base >= 0;
        std::int32_t wanted = include ? base : static_cast<std::int32_t>(0u - u(base));
        bool found = false;
        for (std::int32_t e : spell.effect) {
            if (e == 0xfe) break;
            if (wanted == e || (shielding_effect(e) && shielding_effect(wanted))) {
                found = true;
                break;
            }
        }
        pass = found == include;
        break;
    }
    case 0x137: pass = (spell.is_skill ? 1 : 0) == (base != 0 ? 1 : 0); break; // the byte at +0x244 (IsSkill)
    case 0x8a: pass = spell.spell_type == base; break;
    case 0x8b: pass = match_or_exclude(static_cast<std::int32_t>(spell.id), base); break;
    case 0x181: pass = match_or_exclude(spell.spell_group, base); break;
    case 0x8c: pass = spell_duration(spell, static_cast<std::uint8_t>(sheet_.level), false) >= base; break;
    case 0x8d: pass = spell.duration_formula == 0; break;
    case 0x8f: pass = spell.cast_time >= u(base); break;
    case 0x90: pass = spell.cast_time <= u(base); break;
    case 0x15c: pass = spell.mana >= base; break;
    case 0x187: pass = spell.mana <= base; break;
    case 0x1b1: case 0x1b2: case 0x1b3: { // GetFocusLevelFalloffPct @0x428980
        if (base2 == 100) return 100;
        std::int32_t needed = level_needed(spell, sheet_.class_id);
        if (needed <= base) return 100;
        if (base2 == 0) return 0;
        std::int32_t pct = static_cast<std::int32_t>(100u - (u(needed) - u(base)) * u(base2));
        return pct < 0 ? 0 : pct;
    }
    case 0x193: pass = match_or_exclude(spell.spell_class, base); break;
    case 0x194: pass = match_or_exclude(spell.spell_subclass, base); break;
    case 0x19b: // SPA 411 limit: the player's class (bit 1 << class); with focus_classes any of them
        pass = (u(base) & (sheet_.focus_classes != 0 ? sheet_.focus_classes << 1 : 1u << (sheet_.class_id & 31))) != 0;
        break;
    case 0x19c: pass = (u(base) & (1u << (sheet_.race & 31))) != 0; break;
    case 0x19e: pass = spell.skill == base; break;
    case 0x19f: { // an item of item type base (-1 any) and subclass base2 (-1 any) in a slot of the mask
        pass = false;
        for (std::uint32_t slot = 0; slot < 0x20; ++slot) {
            if (!(slots & (1u << slot))) continue;
            const net::Item* item = sheet_.item(static_cast<int>(slot));
            if (item && (base == -1 || item->item_type == u(base)) && (base2 == -1 || item->sub_type == base2)) {
                pass = true;
                break;
            }
        }
        break;
    }
    case 0x1a6: pass = spell.hit_count >= base; break;
    case 0x1a7: pass = spell.hit_count_type == base; break;
    default: break;
    }
    return pass ? 100 : 0;
}

// GetSpellFocusPct @0x4292f0 + EQ_Character::IsSpellcaster_2 @0x4288e0.
std::int32_t Character::spell_focus_pct(const SpellData& cast, const SpellData& focus, bool bypass_class_check) const {
    LimitState state;
    for (std::size_t slot = 1; slot < 12; ++slot) {
        std::int32_t type = focus.effect[slot];
        if (type == 0xfe) break;
        std::int32_t base = focus.base[slot];
        std::int32_t result = check_focus_limit(cast, type, base, focus.base2[slot], u(focus.max[slot]));
        state.step(result, counts(type, base), type);
        state.note(type);
        if (state.min <= 0) break;
    }
    state.finish();
    // IsSpellcaster_2: pure melee classes and bards get no focus unless the focus has limit 0x19b, the spell is a
    // skill, or the caller asks.
    if (bypass_class_check || state.flag || cast.is_skill || sheet_.focus_classes != 0) return state.min;
    std::uint8_t cls = sheet_.class_id;
    if (cls == 8 || cls == 1 || cls == 9 || cls == 7) return 0;
    return state.min;
}

// GetBestItemFocus @0x42b6a0 over CollectItemFocusEffects @0x42a1a0: the 43 item lookups (worn slots 0..21 and
// the 21 extra items) the character may use, each item and then its children, plain and not a charm without a
// positive scale; their focus (3) and second focus (5) spells whose first effect is the focus type.
FocusFound Character::best_item_focus(const SpellData& cast, std::int32_t type) const {
    if (hooks.best_item_focus)
        if (auto v = hooks.best_item_focus(type)) return *v;
    FocusFound none;
    if (cast.not_focusable) return none;
    std::uint32_t negate = negate_mask(type);
    std::vector<FocusPick> picks;
    for (int k = 0; k < 0x16 + 21; ++k) {
        const net::Item* top = k < 0x16 ? sheet_.item(k) : walk_item(kWornSlots + (k - 0x16));
        if (!top || top->item_class != 0 || !can_use_item(*top, true)) continue;
        std::vector<const net::Item*> walk{top};
        int children = 0;
        for (const auto& bi : top->contents) children = std::max(children, static_cast<int>(bi.index) + 1);
        for (int c = 0; c < children; ++c)
            if (const net::Item* child = top->content(static_cast<std::uint32_t>(c))) walk.push_back(child);
        for (const net::Item* item : walk) {
            if (item->item_class != 0 || (item->charm_file_id != 0 && static_cast<std::int32_t>(item->scaled_value) <= 0))
                continue;
            for (int kind : {3, 5}) {
                const SpellData* effect = spell(item->effects[static_cast<std::size_t>(kind)].effect);
                if (!effect || effect->effect[0] != type || (negate & 2)) continue;
                std::int32_t pct = spell_focus_pct(cast, *effect, false);
                if (pct <= 0) continue;
                FocusPick p;
                p.value = scale(effect->base[0], pct);
                p.percent = pct;
                p.item = item;
                p.kind = kind;
                picks.push_back(p);
            }
        }
    }
    const FocusPick* best = best_of(picks);
    if (!best) return none;
    FocusFound r;
    r.found = true;
    r.percent = best->percent;
    r.extra = best->kind;
    last_focus_item_ = best->item;
    return r;
}

// GetBestBuffFocus @0x42aca0 over CollectBuffFocusEffects @0x429980: a buff whose spell's first effect is the type.
FocusFound Character::best_buff_focus(const SpellData& cast, std::int32_t type, bool buff_arg) const {
    if (hooks.best_buff_focus)
        if (auto v = hooks.best_buff_focus(type, buff_arg)) return *v;
    FocusFound none;
    if (cast.not_focusable) return none;
    std::uint32_t negate = negate_mask(type);
    std::vector<FocusPick> picks;
    for (int slot = 0; slot < max_effects() && slot < static_cast<int>(sheet_.buffs.size()); ++slot) {
        const SheetBuff& b = sheet_.buffs[static_cast<std::size_t>(slot)];
        if (b.type == 0 || !valid_spell(b.spell_id)) continue;
        const SpellData* effect = spell(b.spell_id);
        if (!effect || effect->effect[0] != type) continue;
        if (effect->spell_type != 0 && (negate & 1)) continue;
        std::int32_t pct = spell_focus_pct(cast, *effect, buff_arg);
        if (pct <= 0) continue;
        FocusPick p;
        p.value = scale(effect->base[0], pct);
        p.percent = pct;
        p.spell = effect;
        picks.push_back(p);
    }
    const FocusPick* best = best_of(picks);
    if (!best) return none;
    return {true, best->percent, 0, best->spell->base[0]};
}

// GetBestAAFocus @0x42ad90 over CollectAAFocusEffects @0x429d10: abilities whose first effect is a focus type;
// each effect of the type with the limits that follow it (up to the next focus-type effect; GetAAFocusPct
// @0x429620), the largest value first.
FocusFound Character::best_aa_focus(const SpellData& cast, std::int32_t type) const {
    if (hooks.best_aa_focus)
        if (auto v = hooks.best_aa_focus(type)) return *v;
    FocusFound none;
    if (!sheet_.is_pc() || cast.not_focusable || !sheet_.has_owner) return none;
    std::uint32_t negate = negate_mask(type);
    const SheetAAEffect* best = nullptr;
    std::int32_t best_value = 0, best_pct = 100;
    for (const SheetAbility& a : sheet_.abilities) {
        // AltAbility_IsClassAllowed for the class; with focus_classes for any of them.
        if (!(a.class_mask & ((1u << (sheet_.class_id & 31)) | (sheet_.focus_classes << 1)))) continue;
        if (a.required_level > sheet_.level) continue;
        if (a.effects.empty() || !is_focus_type(a.effects[0].spa)) continue;
        for (std::size_t i = 0; i < a.effects.size(); ++i) {
            const SheetAAEffect& e = a.effects[i];
            if (e.spa != type || (negate & 4)) continue;
            LimitState state;
            for (std::size_t j = i + 1; j < a.effects.size() && !is_focus_type(a.effects[j].spa); ++j) {
                const SheetAAEffect& l = a.effects[j];
                std::int32_t result = check_focus_limit(cast, l.spa, l.base, l.limit, 0xffffffffu);
                state.step(result, counts(l.spa, l.base), l.spa);
                state.note(l.spa);
                if (state.min <= 0) break;
            }
            state.finish();
            std::int32_t pct = state.min;
            if (!(state.flag || cast.is_skill || sheet_.focus_classes != 0)) {
                std::uint8_t cls = sheet_.class_id;
                if (cls == 8 || cls == 1 || cls == 9 || cls == 7) pct = 0;
            }
            if (pct <= 0) continue;
            std::int32_t value = scale(e.base, pct);
            if (!best || value > best_value) {
                best = &e;
                best_value = value;
                best_pct = pct;
            }
        }
    }
    if (!best) return none;
    return {true, best_pct, 0, best->base};
}

std::int32_t Character::focus_casting_time_modifier(const SpellData& cast, bool buff_arg) const {
    constexpr std::int32_t kType = 0x7f; // SPA 127 IncreaseSpellHaste
    std::int32_t total = 0;
    last_focus_item_ = nullptr;
    FocusFound item = best_item_focus(cast, kType);
    if (item.found) {
        // ItemClient::GetSpellID(item, extra), IsValidSpellID, then the spell manager's spell (may be null).
        std::pair<std::int32_t, bool> id{0, false};
        bool hooked = false;
        if (hooks.item_spell_id)
            if (auto v = hooks.item_spell_id(item.extra)) {
                id = *v;
                hooked = true;
            }
        if (!hooked && last_focus_item_ && item.extra >= 0 && item.extra < net::kEffectCount) {
            id.first = last_focus_item_->effects[static_cast<std::size_t>(item.extra)].effect;
            id.second = valid_spell(id.first);
        }
        if (id.second) {
            std::optional<std::pair<bool, std::int32_t>> base;
            if (hooks.spell_focus_base) base = hooks.spell_focus_base(id.first);
            if (!base) {
                const SpellData* s = spell(id.first);
                base = std::pair<bool, std::int32_t>{s != nullptr, s ? s->base[0] : 0};
            }
            if (base->first) total = cast_time_share(cast, scale(base->second, item.percent));
        }
    }
    FocusFound buff = best_buff_focus(cast, kType, buff_arg);
    if (buff.found) total = add(total, cast_time_share(cast, scale(buff.value, buff.percent)));
    FocusFound aa = best_aa_focus(cast, kType);
    if (aa.found) total = add(total, cast_time_share(cast, scale(aa.value, aa.percent)));
    return total;
}

// GetFocusRangeModifier @0x42cb80 (focus type 0x81, SPA 129): for the best item (its focus spell, null-checked),
// buff (4th argument 0) and AA, (int)(Range x the scaled base x 0.01f); each share is truncated, then added.
std::int32_t Character::focus_range_modifier(const SpellData& cast) const {
    constexpr std::int32_t kType = SPA_FOCUS_RANGE_MOD;
    auto share = [&](std::int32_t value, std::int32_t percent) {
        double v = fpu_.mul(static_cast<double>(cast.range), static_cast<double>(scale(value, percent)));
        return Fpu::ftol(fpu_.mul(v, static_cast<double>(kPercent)));
    };
    std::int32_t total = 0;
    last_focus_item_ = nullptr;
    FocusFound item = best_item_focus(cast, kType);
    if (item.found) {
        // ItemClient::GetSpellID(item, extra) and the spell manager's spell, as focus_casting_time_modifier.
        std::pair<std::int32_t, bool> id{0, false};
        bool hooked = false;
        if (hooks.item_spell_id)
            if (auto v = hooks.item_spell_id(item.extra)) {
                id = *v;
                hooked = true;
            }
        if (!hooked && last_focus_item_ && item.extra >= 0 && item.extra < net::kEffectCount) {
            id.first = last_focus_item_->effects[static_cast<std::size_t>(item.extra)].effect;
            id.second = valid_spell(id.first);
        }
        if (id.second) {
            std::optional<std::pair<bool, std::int32_t>> base;
            if (hooks.spell_focus_base) base = hooks.spell_focus_base(id.first);
            if (!base) {
                const SpellData* s = spell(id.first);
                base = std::pair<bool, std::int32_t>{s != nullptr, s ? s->base[0] : 0};
            }
            if (base->first) total = share(base->second, item.percent);
        }
    }
    FocusFound buff = best_buff_focus(cast, kType, false);
    if (buff.found) total = add(total, share(buff.value, buff.percent));
    FocusFound aa = best_aa_focus(cast, kType);
    if (aa.found) total = add(total, share(aa.value, aa.percent));
    return total;
}

std::int32_t Character::focus_reuse_mod(const SpellData& cast) const {
    constexpr std::int32_t kType = 0x136; // SPA 310 ReduceReuseTimer
    std::int32_t total = 0;
    last_focus_item_ = nullptr;
    FocusFound item = best_item_focus(cast, kType);
    if (item.found) {
        // ItemClient::GetSpellID(item, extra), IsValidSpellID, then the spell's first base (none: nothing).
        std::pair<std::int32_t, bool> id{0, false};
        bool hooked = false;
        if (hooks.item_spell_id)
            if (auto v = hooks.item_spell_id(item.extra)) {
                id = *v;
                hooked = true;
            }
        if (!hooked && last_focus_item_ && item.extra >= 0 && item.extra < net::kEffectCount) {
            id.first = last_focus_item_->effects[static_cast<std::size_t>(item.extra)].effect;
            id.second = valid_spell(id.first);
        }
        if (id.second) {
            std::optional<std::pair<bool, std::int32_t>> base;
            if (hooks.spell_focus_base) base = hooks.spell_focus_base(id.first);
            if (!base) {
                const SpellData* s = spell(id.first);
                base = std::pair<bool, std::int32_t>{s != nullptr, s ? s->base[0] : 0};
            }
            if (base->first) total = scale(base->second, item.percent);
        }
    }
    FocusFound buff = best_buff_focus(cast, kType, false);
    if (buff.found) total = add(total, scale(buff.value, buff.percent));
    FocusFound aa = best_aa_focus(cast, kType);
    if (aa.found) total = add(total, scale(aa.value, aa.percent));
    const auto recast = static_cast<std::int32_t>(cast.recast_time);
    return recast < total ? recast : total;
}

} // namespace hailstone::game::mechanics
