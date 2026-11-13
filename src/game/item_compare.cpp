#include "game/item_compare.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "game/inventory.h"
#include "game/item_display.h"
#include "game/mechanics/character.h"
#include "game/mechanics/item_stats.h"
#include "game/mechanics/shown_stats.h"
#include "game/spells.h"
#include "net/item_packets.h"

namespace hailstone::game {

namespace mech = mechanics;

namespace {

constexpr int kWornSlots = 23; // possessions 0..22
constexpr int kRange = 11, kPrimary = 13, kSecondary = 14;

struct StatInfo {
    const char* name;
    const char* key;
};
constexpr std::array<StatInfo, kCompareStatCount> kStats = {{
    {"HP", "hp"}, {"Mana", "mana"}, {"Endurance", "endurance"}, {"AC", "ac"}, {"ATK", "atk"}, {"Haste", "haste"},
    {"STR", "str"}, {"STA", "sta"}, {"AGI", "agi"}, {"DEX", "dex"}, {"WIS", "wis"}, {"INT", "int"}, {"CHA", "cha"},
    {"Magic", "svMagic"}, {"Fire", "svFire"}, {"Cold", "svCold"}, {"Disease", "svDisease"}, {"Poison", "svPoison"},
    {"Corruption", "svCorruption"},
    {"HP Regen", "hpRegen"}, {"Mana Regen", "manaRegen"}, {"Endurance Regen", "enduranceRegen"},
    {"Heal Amount", "healAmount"}, {"Spell Damage", "spellDamage"}, {"Clairvoyance", "clairvoyance"},
    {"Shielding", "shielding"}, {"Spell Shield", "spellShield"}, {"Avoidance", "avoidance"},
    {"Accuracy", "accuracy"}, {"Stun Resist", "stunResist"}, {"Strike Through", "strikeThrough"},
    {"DoT Shielding", "dotShielding"}, {"Combat Effects", "combatEffects"}, {"Damage Shield", "damageShield"},
    {"DS Mitigation", "dsMitigation"},
    {"Weapon Ratio", "weaponRatio"},
}};

// ---- the class weighting (Hailstone's; docs/item-compare.md "The verdict") ----

enum Role : std::uint8_t { Tank, Melee, Hybrid, Priest, Caster, kRoles };

// Classes 1..16: WAR CLR PAL RNG SHD DRU MNK BRD ROG SHM NEC WIZ MAG ENC BST BER.
constexpr std::array<Role, 16> kRole = {Tank,   Priest, Tank,   Hybrid, Tank,   Priest, Melee,  Hybrid,
                                        Melee,  Priest, Caster, Caster, Caster, Caster, Hybrid, Melee};
constexpr std::array<bool, 16> kHasMana = {false, true, true,  true, true, true, false, true,
                                           false, true, true,  true, true, true, true,  false};
// The stat their spells and mana go by: 1 WIS, 2 INT, 3 CHA (bards' songs), 0 none.
constexpr std::array<std::uint8_t, 16> kCastStat = {0, 1, 1, 1, 2, 1, 0, 3, 0, 1, 2, 2, 2, 2, 1, 0};

// Weight of one point, by role (Tank, Melee, Hybrid, Priest, Caster). HP and mana count as themselves; the stats
// that raise them (STA, WIS, INT) count a little on top, since the HP and mana changes already show their effect.
using RoleWeights = std::array<double, kRoles>;
constexpr std::array<RoleWeights, kCompareStatCount> kWeights = {{
    {1.5, 1.0, 1.0, 1.0, 1.0},  // Hp
    {0.3, 0.0, 0.5, 1.0, 1.0},  // Mana (only classes with mana)
    {0.3, 0.5, 0.3, 0.0, 0.0},  // Endurance (priests and casters hardly use it)
    {4.0, 2.0, 2.0, 1.5, 1.0},  // Ac
    {1.0, 2.0, 1.5, 0.2, 0.0},  // Attack
    {5.0, 6.0, 5.0, 1.0, 0.5},  // Haste (per %)
    {0.5, 0.8, 0.6, 0.1, 0.0},  // Str
    {0.2, 0.2, 0.2, 0.2, 0.2},  // Sta
    {0.6, 0.5, 0.3, 0.2, 0.2},  // Agi
    {0.4, 0.8, 0.6, 0.1, 0.1},  // Dex
    {0.0, 0.0, 0.0, 0.0, 0.0},  // Wis (the casting stat's, below)
    {0.0, 0.0, 0.0, 0.0, 0.0},  // Int (the casting stat's, below)
    {0.0, 0.0, 0.0, 0.0, 0.0},  // Cha (the casting stat's, below)
    {0.3, 0.3, 0.3, 0.3, 0.3},  // SvMagic
    {0.3, 0.3, 0.3, 0.3, 0.3},  // SvFire
    {0.3, 0.3, 0.3, 0.3, 0.3},  // SvCold
    {0.3, 0.3, 0.3, 0.3, 0.3},  // SvDisease
    {0.3, 0.3, 0.3, 0.3, 0.3},  // SvPoison
    {0.3, 0.3, 0.3, 0.3, 0.3},  // SvCorruption
    {4.0, 4.0, 4.0, 4.0, 4.0},  // HpRegen
    {2.0, 0.0, 4.0, 8.0, 8.0},  // ManaRegen (only classes with mana)
    {3.0, 3.0, 1.0, 0.0, 0.0},  // EnduranceRegen
    {0.1, 0.0, 0.2, 0.5, 0.0},  // HealAmount
    {0.1, 0.0, 0.2, 0.0, 0.5},  // SpellDamage
    {0.0, 0.0, 0.0, 0.2, 0.2},  // Clairvoyance
    {3.0, 2.0, 2.0, 1.0, 1.0},  // Shielding
    {2.0, 2.0, 2.0, 2.0, 2.0},  // SpellShield
    {3.0, 3.0, 2.0, 1.0, 1.0},  // Avoidance
    {2.0, 3.0, 2.0, 0.0, 0.0},  // Accuracy
    {2.0, 1.0, 1.0, 1.0, 1.0},  // StunResist
    {2.0, 3.0, 2.0, 0.0, 0.0},  // StrikeThrough
    {1.0, 1.0, 1.0, 1.0, 1.0},  // DotShielding
    {2.0, 3.0, 2.0, 0.5, 0.0},  // CombatEffects
    {1.0, 0.5, 0.5, 0.3, 0.3},  // DamageShield
    {0.5, 0.5, 0.5, 0.5, 0.5},  // DsMitigation
    {3.0, 4.0, 3.0, 0.5, 0.0},  // WeaponRatio (per 0.01)
}};

double class_weight(CompareStat stat, int cls) {
    if (cls < 1 || cls > 16) return 0.0;
    const auto i = static_cast<std::size_t>(cls - 1);
    const Role role = kRole[i];
    switch (stat) {
    case CompareStat::Mana:
    case CompareStat::ManaRegen:
        if (!kHasMana[i]) return 0.0;
        break;
    case CompareStat::Wis: return kCastStat[i] == 1 ? 0.5 : 0.0;
    case CompareStat::Int: return kCastStat[i] == 2 ? 0.5 : 0.0;
    case CompareStat::Cha: return kCastStat[i] == 3 || cls == 14 ? 0.5 : 0.0; // bards and enchanters
    default: break;
    }
    return kWeights[static_cast<std::size_t>(stat)][role];
}

// ---- what the character shows ----

using Readout = std::array<std::int64_t, kCompareStatCount>;

std::int64_t ratio_x100(const net::Item* w) {
    if (!w || w->delay == 0 || w->damage == 0) return 0;
    return static_cast<std::int64_t>(w->damage) * 100 / w->delay;
}

// The numbers the inventory window shows (app/lua_eq_values.cpp's cases), for a character that has run
// process_all_stats.
Readout read_out(const mech::Character& ch, int slot) {
    const auto& d = ch.stats();
    Readout r{};
    auto set = [&](CompareStat s, std::int64_t v) { r[static_cast<std::size_t>(s)] = v; };
    set(CompareStat::Hp, ch.max_hp(true));
    set(CompareStat::Mana, ch.max_mana(true));
    set(CompareStat::Endurance, ch.max_endurance(true));
    set(CompareStat::Ac, mech::shown_ac(ch));
    set(CompareStat::Attack, mech::shown_atk(ch, mech::has_spell_2434(ch)));
    std::int64_t haste = ch.total_effect(0xb, true, 0, true, true); // label case 0x49: 100 + %, 0 for none
    set(CompareStat::Haste, haste == 0 ? 0 : haste - 100);
    set(CompareStat::Str, d.stats.str);
    set(CompareStat::Sta, d.stats.sta);
    set(CompareStat::Agi, d.stats.agi);
    set(CompareStat::Dex, d.stats.dex);
    set(CompareStat::Wis, d.stats.wis);
    set(CompareStat::Int, d.stats.int_);
    set(CompareStat::Cha, d.stats.cha);
    set(CompareStat::SvMagic, d.stats.sv_magic);
    set(CompareStat::SvFire, d.stats.sv_fire);
    set(CompareStat::SvCold, d.stats.sv_cold);
    set(CompareStat::SvDisease, d.stats.sv_disease);
    set(CompareStat::SvPoison, d.stats.sv_poison);
    set(CompareStat::SvCorruption, d.stats.sv_corruption);
    // Label cases 0x4a..0x4c: the combat regeneration with the buffs' share.
    set(CompareStat::HpRegen, static_cast<std::int64_t>(ch.hp_regen(true, nullptr, true)) + ch.cached_buff(0, 0));
    set(CompareStat::ManaRegen,
        static_cast<std::int64_t>(ch.mana_regen(true, true)) + ch.cached_buff(15, 0) + ch.cached_buff(0x1a1, 0));
    set(CompareStat::EnduranceRegen,
        static_cast<std::int64_t>(ch.endurance_regen(true, true)) + ch.cached_buff(0xbd, 0));
    set(CompareStat::HealAmount, d.heal_amount);
    set(CompareStat::SpellDamage, d.spell_damage);
    set(CompareStat::Clairvoyance, d.clairvoyance);
    set(CompareStat::Shielding, d.shielding);
    set(CompareStat::SpellShield, d.spell_shield);
    set(CompareStat::Avoidance, d.avoidance);
    set(CompareStat::Accuracy, d.accuracy);
    set(CompareStat::StunResist, d.stun_resist);
    set(CompareStat::StrikeThrough, d.strike_through);
    set(CompareStat::DotShielding, d.dot_shielding);
    set(CompareStat::CombatEffects, d.combat_effects);
    set(CompareStat::DamageShield, d.damage_shield);
    set(CompareStat::DsMitigation, d.ds_mitigation);
    if (slot == kPrimary || slot == kSecondary || slot == kRange)
        set(CompareStat::WeaponRatio, ratio_x100(ch.sheet().item(slot)));
    return r;
}

// The player as a copy with its stats processed: the server extension's vitals left out so Max_HP, Max_Mana and
// Max_Endurance run the stock formulas on both sides of the comparison (as app/stats_ui.cpp's class comparison).
Readout process_copy(const mech::Character& player, mech::CharacterSheet& sheet, int slot) {
    sheet.vitals = {};
    mech::Character c(sheet, player.tables(), player.spells());
    c.process_all_stats();
    return read_out(c, slot);
}

bool two_handed(const net::Item* i) {
    return i && (i->item_type == 1 || i->item_type == 4 || i->item_type == 35); // 2HS, 2HB, 2HP
}

// The spell effects an item and its augments give: "Worn: <name>" and so on.
std::vector<std::string> effects_of(const net::Item& item, const mech::SpellLookup& spells) {
    static constexpr std::pair<net::ItemEffectKind, const char*> kKinds[] = {
        {net::kEffectWorn, "Worn"}, {net::kEffectFocus, "Focus"}, {net::kEffectClick, "Click"}, {net::kEffectProc, "Proc"}};
    std::vector<std::string> out;
    auto add = [&](const net::Item& i) {
        for (auto [kind, label] : kKinds) {
            const net::ItemEffect& e = i.effects[kind];
            if (!e.present()) continue;
            const SpellData* s = spells ? spells(e.effect) : nullptr;
            std::string name = s && !s->name.empty() ? s->name : !e.name.empty() ? e.name : std::format("spell {}", e.effect);
            out.push_back(std::format("{}: {}", label, name));
        }
    };
    add(item);
    for (const auto& c : item.contents)
        if (c.index < 6) add(c.item);
    return out;
}

std::vector<std::string> minus(const std::vector<std::string>& a, std::vector<std::string> b) {
    std::vector<std::string> out;
    for (const auto& s : a) {
        auto it = std::find(b.begin(), b.end(), s);
        if (it == b.end()) out.push_back(s);
        else b.erase(it); // each one only once: two items with the same worn effect still lose one
    }
    return out;
}

std::uint32_t class_mask_of(const mech::CharacterSheet& sheet, std::uint32_t mask) {
    if (mask) return mask;
    return sheet.class_id >= 1 && sheet.class_id <= 32 ? 1u << (sheet.class_id - 1) : 0u;
}

void judge(SlotComparison& s, bool usable) {
    double pos = 0.0, neg = 0.0;
    for (const auto& c : s.changes) {
        double v = c.weight * static_cast<double>(c.delta());
        if (v > 0) pos += v;
        else neg += v;
    }
    s.score = pos + neg;
    if (!usable) s.verdict = Verdict::CantUse;
    else if (pos == 0.0 && neg == 0.0) s.verdict = Verdict::Same;
    else if (neg == 0.0) s.verdict = Verdict::Upgrade;
    else if (pos == 0.0) s.verdict = Verdict::Downgrade;
    else s.verdict = Verdict::Mixed;
}

} // namespace

const char* compare_stat_name(CompareStat s) {
    auto i = static_cast<std::size_t>(s);
    return i < kStats.size() ? kStats[i].name : "";
}

const char* compare_stat_key(CompareStat s) {
    auto i = static_cast<std::size_t>(s);
    return i < kStats.size() ? kStats[i].key : "";
}

const char* verdict_name(Verdict v) {
    switch (v) {
    case Verdict::Upgrade: return "upgrade";
    case Verdict::Downgrade: return "downgrade";
    case Verdict::Mixed: return "mixed";
    case Verdict::Same: return "same";
    case Verdict::CantUse: return "cantuse";
    }
    return "";
}

double compare_weight(CompareStat stat, std::uint32_t class_mask) {
    double w = 0.0;
    for (int c = 1; c <= 16; ++c)
        if (class_mask & (1u << (c - 1))) w = std::max(w, class_weight(stat, c));
    return w;
}

std::optional<ItemComparison> compare_item(const mech::Character& player, const net::Item& item,
                                           const CompareOptions& options, mech::CharacterSheet& scratch) {
    const mech::CharacterSheet& sheet = player.sheet();
    const std::uint32_t worn = mech::item_equip_slots(item, false) & ((1u << kWornSlots) - 1);
    if (item.item_class != 0 || worn == 0) return std::nullopt;

    ItemComparison out;
    const std::uint32_t classes = class_mask_of(sheet, options.class_mask);
    // CanUseItem: the class (the click path's check, can_use_class), then level, race, deity and prestige.
    if (!options.can_use_all) {
        if (!can_use_class(item, classes)) {
            out.usable = false;
            out.why_not = "Your class cannot use this item";
        } else if (std::int32_t req = mech::item_required_level(item); req > sheet.level) {
            out.usable = false;
            out.why_not = std::format("Requires level {}", req);
        } else if (!player.can_use_item(item, false)) {
            out.usable = false;
            out.why_not = "Your race or deity cannot use this item";
        }
    }
    auto add_note = [&out](std::string note) {
        if (std::find(out.notes.begin(), out.notes.end(), note) == out.notes.end()) out.notes.push_back(std::move(note));
    };
    if (std::int32_t rec = mech::item_recommended_level(item); rec > sheet.level)
        add_note(std::format("Recommended level {}: its stats are reduced for you", rec));

    const net::Item* primary = sheet.item(kPrimary);
    const net::Item* secondary = sheet.item(kSecondary);
    const std::vector<std::string> new_effects = effects_of(item, player.spells());
    scratch = sheet; // copy-assigned: the last copy's storage is reused
    const Readout before_all = process_copy(player, scratch, -1);

    for (int slot = 0; slot < kWornSlots; ++slot) {
        if (!(worn & (1u << slot))) continue;
        const bool available = (options.slots_available >> slot) & 1u;
        if (!available) continue;
        // The original refuses a two-hander beside a secondary item and an item in the secondary beside a
        // two-hander (PcZoneClient_CanItemGoInEquipSlot); the comparison takes the other one off instead.
        std::vector<int> replaced{slot};
        if (!can_go_in_equip_slot(item, slot, primary, secondary, options.dual_wield)) {
            if (!can_go_in_equip_slot(item, slot, nullptr, nullptr, options.dual_wield)) continue;
            if (slot == kPrimary && secondary) replaced.push_back(kSecondary);
            else if (slot == kSecondary && primary) replaced.push_back(kPrimary);
            else continue;
        } else if (slot == kPrimary && two_handed(&item) && secondary) {
            replaced.push_back(kSecondary);
        }

        SlotComparison sc;
        sc.slot = slot;
        sc.replaced = replaced;
        std::vector<std::string> old_effects;
        for (int r : replaced) {
            const net::Item* cur = sheet.item(r);
            sc.current.push_back(cur ? cur->name : std::string());
            if (cur) {
                auto e = effects_of(*cur, player.spells());
                old_effects.insert(old_effects.end(), e.begin(), e.end());
                std::size_t augs = 0;
                for (const auto& c : cur->contents)
                    if (c.index < 6) ++augs;
                if (augs)
                    add_note(std::format("Your {} has {} augment{}, counted with it", cur->name, augs,
                                                    augs == 1 ? "" : "s"));
            }
        }

        Readout before = before_all;
        before[static_cast<std::size_t>(CompareStat::WeaponRatio)] =
            slot == kPrimary || slot == kSecondary || slot == kRange ? ratio_x100(sheet.item(slot)) : 0;
        scratch = sheet;
        for (int r : replaced) scratch.possessions[static_cast<std::size_t>(r)].reset();
        scratch.possessions[static_cast<std::size_t>(slot)] = item;
        const Readout after = process_copy(player, scratch, slot);

        for (int i = 0; i < kCompareStatCount; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (before[ui] == after[ui]) continue;
            const auto stat = static_cast<CompareStat>(i);
            sc.changes.push_back({stat, before[ui], after[ui], compare_weight(stat, classes)});
        }
        if (slot == kPrimary || slot == kSecondary || slot == kRange) {
            auto info = [&](const net::Item* w) -> std::optional<WeaponInfo> {
                if (!w || w->damage == 0) return std::nullopt;
                return WeaponInfo{static_cast<std::int32_t>(w->damage), w->delay,
                                  weapon_damage_bonus(sheet.level, sheet.class_id, *w)};
            };
            sc.weapon_before = info(sheet.item(slot));
            sc.weapon_after = info(&item);
        }
        sc.gained = minus(new_effects, old_effects);
        sc.lost = minus(old_effects, new_effects);
        judge(sc, out.usable);
        out.slots.push_back(std::move(sc));
    }
    // The best swap first (an empty slot or the weaker of a pair), then the slot order.
    std::stable_sort(out.slots.begin(), out.slots.end(),
                     [](const SlotComparison& a, const SlotComparison& b) { return a.score > b.score; });
    return out;
}

} // namespace hailstone::game
