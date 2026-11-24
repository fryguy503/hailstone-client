#include "game/mechanics/ticks.h"

namespace hailstone::game::mechanics {

std::int32_t apply_mana_regen_tick(std::int32_t stored, std::int32_t current, std::int32_t maximum,
                                   const std::function<std::int32_t()>& regen) {
    if (current >= maximum) return stored;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(stored) + static_cast<std::uint32_t>(regen()));
}

EnduranceUpkeep apply_endurance_upkeep(std::int32_t stored, std::int32_t upkeep,
                                       const std::function<std::int32_t(std::int32_t stored)>& endurance,
                                       bool player_spawn) {
    EnduranceUpkeep out;
    out.endurance = static_cast<std::int32_t>(static_cast<std::uint32_t>(stored) - static_cast<std::uint32_t>(upkeep));
    if (endurance(out.endurance) >= 0) return out;
    out.endurance = 0;
    out.drop_discipline = player_spawn;
    return out;
}

namespace {

std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrap_neg(std::int32_t a) { return static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(a)); }

constexpr std::int32_t kSpaHoldBuffs = 0x1b4;

// PoolTick: the buff's change this tick (removing a draining buff flagged
// +0x26b the pool cannot pay), negated for type 4, added to the pool.
void pool_tick(AffectQueries& q, const AffectSpell& spell, AffectBuff& buff, int slot, int index, bool mana,
               std::int32_t& pool) {
    auto current = [&] { return mana ? q.cur_mana() : q.cur_endurance(); };
    std::int32_t change = q.affect_change(spell, buff, slot, index);
    if (change < 0 && spell.drain_removes == 1 && current() < (change < 0 ? wrap_neg(change) : change))
        q.remove_affect(buff, 1, 0);
    if (buff.type == 4) change = wrap_neg(change);
    pool = wrap_add(pool, change);
    current();
}

} // namespace

std::int32_t process_affects(AffectState& st, AffectQueries& q) {
    std::int32_t hp_change = 0;
    bool dirty = false;
    int index = 0;
    if (q.max_effects() == 0) return 0;
    do {
        bool remove = false;
        AffectBuff& buff = q.effect(index);
        if (buff.type < 2) continue;
        const AffectSpell* spell = nullptr;
        if (q.valid_spell(buff.spell_id)) {
            spell = q.spell(buff.spell_id);
            if (spell && (!(spell->float_224 == 0.0f) || !(spell->float_21c == 0.0f))) {
                q.cache_buff_slot_effects(index);
                dirty = true;
            }
        }
        if (buff.duration == 0) remove = true;
        if (buff.duration == -1 && spell && spell->duration_formula != 0x32) remove = true;
        if (!q.countdown_held(spell) && !q.has_affect(kSpaHoldBuffs) && buff.duration > 0) --buff.duration;
        if (spell) {
            bool stop = false;
            for (int slot = 0; slot < 12; ++slot) {
                auto s = static_cast<std::size_t>(slot);
                if (q.tick_varying(spell->spa[s])) st.tick_cache_valid = 0;
                std::int32_t spa = spell->spa[s];
                bool check = true; // whether the slot ends with the spawn check
                switch (spa) {
                case 0xbd:
                    pool_tick(q, *spell, buff, slot, index, false, st.endurance);
                    break;
                case 0xf:
                case 0x1a1: {
                    std::uint8_t c = st.class_id;
                    if (c == 8 || c == 7 || c == 9 || c == 1 || c == 0x10) break;
                    pool_tick(q, *spell, buff, slot, index, true, st.mana);
                    break;
                }
                case 0x71:
                    if (q.spawn_present() && !q.has_mount() && q.find_summon(false)) q.ride_mount();
                    break;
                case 0x43:
                    if (q.find_summon(true)) {
                        // The controlled player stops (SpeedRun, SpeedHeading 0) and the eye takes over.
                        st.controlled_present = true;
                        st.controlled_is_eye = true;
                        q.view_eye();
                    }
                    break;
                case 3:
                case 0x63:
                    if (0.0f < st.speed_multiplier || (spell->base2[s] & 1) != 0 || q.recheck(buff.spell_id) != 0)
                        check = false;
                    break;
                case 0x39: {
                    if (buff.duration < 0 || buff.duration >= 2) break;
                    if (q.valid_spell(buff.spell_id)) {
                        const AffectSpell* found = q.spell(buff.spell_id);
                        if (q.spell_level_needed(found, 8) != 0xff) break;
                    }
                    q.chat(kStrLevitationFading);
                    break;
                }
                case 0x3a:
                case 0x9c:
                    if (buff.duration >= 0 && buff.duration < 2) q.chat(kStrEffectFading);
                    break;
                case 0x157: {
                    std::int32_t roll = q.rand() % 100;
                    if (roll >= spell->base[s] || !q.spawn_present() || st.casting_spell_id == -1) break;
                    const AffectSpell* casting = q.spell(st.casting_spell_id);
                    if (casting && !casting->uninterruptible) q.stop_cast();
                    break;
                }
                case 0x14e:
                    if (q.spawn_present() && st.spawn_type == 1 && 0.0f < st.speed_run) break;
                    [[fallthrough]];
                case 0:
                case 0x64: {
                    std::int32_t base2 = spell->base2[s];
                    if (base2 > 0 && q.spawn_present() && !q.hp_tick_allowed(base2)) break;
                    hp_change = wrap_add(hp_change, q.tick_hp_change(index, buff, slot, *spell));
                    break;
                }
                case 0x121:
                case 0x179:
                    remove = false;
                    break;
                case kSpaHoldBuffs:
                    if (!q.timers_on_hold() && q.spell_has_spa(*spell, kSpaHoldBuffs) && q.spawn_present())
                        st.hold_flag = 1;
                    break;
                default:
                    break;
                }
                if (check && !q.spawn_present()) {
                    stop = true;
                    break;
                }
            }
            if (stop || !q.spawn_present()) break;
        }
        if (remove) q.remove_affect(buff, 0, 0);
    } while (++index < q.max_effects());
    if (dirty) st.stats_dirty = true;
    if (hp_change == 0) return 0;
    if (hp_change < 0) {
        // A loss breaks the first mesmerize (SPA 0x1f) buff.
        for (int k = 0; k < q.max_effects(); ++k) {
            AffectBuff& buff = q.effect(k);
            if (buff.type == 0) continue;
            const AffectSpell* spell = q.spell(buff.spell_id);
            if (!spell) continue;
            bool mesmerized = false;
            for (std::int32_t spa : spell->spa) {
                if (spa == 0xfe) break;
                if (spa == 0x1f) {
                    mesmerized = true;
                    break;
                }
            }
            if (mesmerized) {
                q.remove_affect(buff, 1, 0);
                q.end_stun();
                break;
            }
        }
    }
    q.modify_cur_hp(hp_change);
    return 0;
}

} // namespace hailstone::game::mechanics
