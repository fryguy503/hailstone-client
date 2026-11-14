#include "game/mechanics/sheet_builder.h"

#include <algorithm>

#include "game/distance.h"
#include "game/inventory.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "net/tribute_packets.h"
#include "net/zone_packets.h"

namespace hailstone::game::mechanics {

SpellLookup spell_lookup(const SpellTable& table) {
    return [&table](std::int32_t id) -> const SpellData* {
        return id > 0 ? table.get(static_cast<std::uint32_t>(id)) : nullptr;
    };
}

CharacterSheet build_sheet(const net::PlayerProfile& p, const Inventory& inventory, const SpellState& spells,
                           const LiveState& live, const net::NewZone* zone, const WorldState* world) {
    CharacterSheet s;
    build_sheet_into(s, p, inventory, spells, live, {}, zone, world); // the caller sets the abilities
    return s;
}

void build_sheet_into(CharacterSheet& s, const net::PlayerProfile& p, const Inventory& inventory,
                      const SpellState& spells, const LiveState& live, const SheetAbilitiesInto& fill_abilities,
                      const net::NewZone* zone, const WorldState* world) {
    {
        // Everything back to its default, but the containers keep their storage.
        auto possessions = std::move(s.possessions);
        auto buffs = std::move(s.buffs);
        auto abilities = std::move(s.abilities);
        s = CharacterSheet{};
        s.possessions = std::move(possessions);
        s.buffs = std::move(buffs);
        s.abilities = std::move(abilities);
    }
    if (fill_abilities) fill_abilities(s.abilities);
    else s.abilities.clear();
    s.race = p.race;
    s.class_id = p.class_;
    s.class_mask = live.class_mask;
    s.focus_classes = live.focus_classes;
    s.skill_rules = live.skill_rules;
    s.vitals = live.vitals;
    s.leadership_active = (p.expansions & 0x40) != 0;
    s.leadership_hp_bonus = live.leadership.hp;
    s.leadership_mana_bonus = live.leadership.mana;
    s.leadership_hp_regen_bonus = live.leadership.hp_regen;
    s.leadership_offense_bonus = live.leadership.offense;
    s.level = live.level > 0 ? live.level : p.level;
    // PcBase_GetHighestLevel: the profile carries no separate value in EQEmu's RoF2 encoder; the current level.
    s.highest_level = s.level;
    s.deity = p.deity;
    s.heritage = static_cast<std::uint8_t>(p.drakkin_heritage);
    s.expansions = p.expansions;
    s.spawn_type = SpawnType::PC;
    s.has_spawn = true;
    s.spawn_hp = static_cast<std::int32_t>(live.hp);
    s.spawn_max_hp = static_cast<std::int32_t>(live.max_hp);
    // Profile +0x3394 BaseHP: the server's current HP as it came (SetSpawnHP @0x4bd0e0 stores OP_HPUpdate's value
    // for the local player; the zone-in profile's cur_hp before that). Left 0, Cur_HP read 0 and CalculateWeight's
    // low-health slowdown halved AGI (and Max_Endurance with it).
    s.stored_hp = static_cast<std::int32_t>(live.hp);

    s.base_str = static_cast<std::int32_t>(p.str);
    s.base_sta = static_cast<std::int32_t>(p.sta);
    s.base_cha = static_cast<std::int32_t>(p.cha);
    s.base_dex = static_cast<std::int32_t>(p.dex);
    s.base_int = static_cast<std::int32_t>(p.int_);
    s.base_agi = static_cast<std::int32_t>(p.agi);
    s.base_wis = static_cast<std::int32_t>(p.wis);
    s.base_save_magic = p.base_save_magic;
    s.base_save_fire = p.base_save_fire;
    s.base_save_cold = p.base_save_cold;
    s.base_save_disease = p.base_save_disease;
    s.base_save_poison = p.base_save_poison;
    s.base_save_physical = p.base_save_physical;
    s.base_save_corruption = p.base_save_corruption;
    s.own_base_hp = p.base_hp_total;
    s.own_base_endurance = p.base_endurance_total;
    s.own_base_mana = p.base_mana_total;
    s.own_hp_regen = p.profile_344c[2];
    s.mana_regen_sitting = p.profile_344c[3];
    s.mana_regen_standing = p.profile_344c[4];
    s.own_endurance_regen = p.base_endurance_regen;
    s.base_ac = p.profile_3464[0];
    s.fist_damage = p.profile_3464[2]; // PlayerProfileData +0x4fec / +0x4ff0 (ApplyBaseProfileData); EQEmu sends 0
    s.fist_delay = p.profile_3464[3];
    // Innate ability 10 (+0x3258; Troll and Iksar regeneration) of the 25 innate slots at +0x3230.
    s.innate_3258 = p.innate_skills.size() > 10 ? static_cast<std::int32_t>(p.innate_skills[10]) : 0xff;
    for (std::size_t i = 0; i < s.skills.size() && i < p.skills.size(); ++i)
        s.skills[i] = static_cast<std::int32_t>(p.skills[i]);
    s.mana = static_cast<std::int32_t>(live.mana);
    s.endurance = static_cast<std::int32_t>(live.endurance);
    s.food = static_cast<std::int32_t>(live.hunger);
    s.drink = static_cast<std::int32_t>(live.thirst);
    s.drunkenness = live.drunkenness.value_or(static_cast<std::int32_t>(p.intoxication));
    s.owner_status = live.owner_status;

    for (int slot = 0; slot < kPossessionSlots; ++slot) {
        auto& kept = s.possessions[static_cast<std::size_t>(slot)];
        if (const net::Item* item = inventory.at(net::ItemSlot::possessions(static_cast<std::int16_t>(slot)))) {
            if (kept) *kept = *item; // copy-assigned: its strings and bag contents reuse what they hold
            else kept.emplace(*item);
        } else {
            kept.reset();
        }
    }

    // EQEmu's RoF2 translator addresses personal tribute as type 6, slots 0..4 and guild tribute as type 8,
    // slots 0..1. Use those received items, including their normal stats and effects, rather than synthesizing
    // bonuses from the tribute catalog. The connection retires their slots on received tribute state changes.
    for (std::int16_t slot = 0; slot < net::kTributeSlots; ++slot)
        if (const net::Item* item = inventory.at({net::kInvTribute, slot, -1, -1}))
            s.extra_items[static_cast<std::size_t>(slot)] = *item;
    for (std::int16_t slot = 0; slot < net::kGuildTributeSlots; ++slot)
        if (const net::Item* item = inventory.at({net::kInvGuildTribute, slot, -1, -1}))
            s.extra_items[15 + static_cast<std::size_t>(slot)] = *item;

    s.buffs.assign(kPlayerBuffSlots, SheetBuff{});
    for (std::size_t i = 0; i < s.buffs.size() && i < spells.buffs.size(); ++i) {
        const BuffSlot& b = spells.buffs[i];
        if (b.empty()) continue;
        SheetBuff& out = s.buffs[i];
        out.type = b.effect_type ? b.effect_type : 2;
        // The caster level of buffs that arrived without one (OP_BuffCreate) is not known; the player's own
        // level stands in for it.
        out.level = b.level ? b.level : static_cast<std::uint8_t>(std::clamp(s.level, 0, 255));
        out.modifier = b.modifier;
        out.spell_id = static_cast<std::int32_t>(b.spell);
        // Ticks left: the server's count less the 6 s ticks since it arrived.
        std::int64_t elapsed = live.now_ms > b.received_ms ? (live.now_ms - b.received_ms) / kBuffTickMs : 0;
        out.duration = static_cast<std::int32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(b.tics) - elapsed));
        out.caster_id = b.caster_id;
        out.caster_is_self = b.caster_id != 0 && b.caster_id == live.player_id;
        if (world) {
            auto it = world->spawns.find(b.caster_id);
            out.caster_is_pc = out.caster_is_self || (it != world->spawns.end() && !it->second.is_npc);
            // CacheBuffSlotEffects' GetSpawnByID(BuffCasterIDs[slot]) and CDisplay::PlayerDistance(caster, me, 0).
            if (const Spawn* me = world->player(); me && it != world->spawns.end())
                out.caster_distance = player_distance(&it->second.position, &me->position, 0.0f);
        } else {
            out.caster_is_pc = out.caster_is_self;
        }
    }

    s.sitting = live.sitting;
    s.spawn_sitting = live.sitting;
    s.has_owner = true;
    s.owner_expansions = s.expansions;
    s.owner_food = s.food;
    s.owner_drink = s.drink;
    s.sitting_ms = live.sitting_ms;
    s.fast_regen_resting = live.fast_regen_resting;
    s.buff_timers_on_hold = spells.buff_timers_on_hold;
    s.attacking = live.attacking;
    s.running = live.running;
    if (zone) {
        s.fast_regen_hp_divisor = static_cast<std::int32_t>(zone->fast_regen_hp);
        s.fast_regen_mana_divisor = static_cast<std::int32_t>(zone->fast_regen_mana);
        s.fast_regen_endurance_divisor = static_cast<std::int32_t>(zone->fast_regen_endurance);
        s.zone_id = zone->zone_id;
        // EQWorldData_IsBazaarZone / IsBarterZone: zone flags 0x10000000 / 0x2000000 of the original's zone table:
        // bazaar (151) has both, barter (346) the second.
        s.bazaar_zone = zone->zone_id == 151;
        s.barter_zone = zone->zone_id == 151 || zone->zone_id == 346;
    }
    // The server's PvP rule set (global 0xdcee70, OP_LogServer +4): CacheBuffSlotEffects scales a player's
    // negative SPA 46..50 / 370 by 1.5 under any rule set.
    s.pvp_ruleset = live.pvp_rules != 0;
}

} // namespace hailstone::game::mechanics
