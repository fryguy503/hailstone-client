#pragma once

// Hailstone's item comparison (the original has none; docs/item-compare.md): what wearing an item instead of what
// the player wears now would change. The worn slot is swapped on a copy of the player's sheet and the copy is run
// through the same rules as the player (mechanics::Character: ProcessAllStats, Max_HP, CalcAC, the caps, the
// recommended-level scaling), so the numbers are the ones the inventory window would show after the swap. Only the
// verdict (Upgrade, Downgrade, ...) is Hailstone's own judgement: a weighting by class, a guide and not a rule.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hailstone::net {
struct Item;
}

namespace hailstone::game {

namespace mechanics {
struct CharacterSheet;
class Character;
} // namespace mechanics

// The numbers compared, in the order they are listed.
enum class CompareStat : std::uint8_t {
    Hp, Mana, Endurance, Ac, Attack, Haste,
    Str, Sta, Agi, Dex, Wis, Int, Cha,
    SvMagic, SvFire, SvCold, SvDisease, SvPoison, SvCorruption,
    HpRegen, ManaRegen, EnduranceRegen,
    HealAmount, SpellDamage, Clairvoyance,
    Shielding, SpellShield, Avoidance, Accuracy, StunResist, StrikeThrough, DotShielding, CombatEffects,
    DamageShield, DsMitigation,
    WeaponRatio, // a weapon slot's damage / delay x 100 (no character rule; the usual measure of a weapon)
    Count
};
inline constexpr int kCompareStatCount = static_cast<int>(CompareStat::Count);

// "HP", "Mana", ... and a short key for scripts ("hp", "mana", ...).
const char* compare_stat_name(CompareStat s);
const char* compare_stat_key(CompareStat s);

struct StatChange {
    CompareStat stat = CompareStat::Hp;
    std::int64_t before = 0, after = 0;
    double weight = 0.0; // the class weighting's weight for the player (0: does not count towards the verdict)
    std::int64_t delta() const { return after - before; }
};

enum class Verdict : std::uint8_t {
    Upgrade,   // better in something that counts for the class, worse in nothing that does
    Downgrade, // the reverse
    Mixed,     // both; `score` leans it one way
    Same,      // no change that counts for the class
    CantUse,   // the player cannot wear it (class, race, deity, level)
};
const char* verdict_name(Verdict v); // "upgrade", "downgrade", "mixed", "same", "cantuse"

struct WeaponInfo {
    std::int32_t damage = 0, delay = 0, bonus = 0; // bonus: the class and level's damage bonus (weapon_damage_bonus)
};

// One worn slot the item could go in.
struct SlotComparison {
    int slot = 0;                    // the possessions slot (0..22)
    std::vector<int> replaced;       // the slots emptied (the slot; a two-hander also the secondary)
    std::vector<std::string> current; // the names of the items taken off ("" none)
    std::vector<StatChange> changes; // the effective numbers that change, in CompareStat order
    std::optional<WeaponInfo> weapon_before, weapon_after; // primary, secondary and range only
    std::vector<std::string> gained, lost; // "Worn: Haste", "Focus: Improved Healing I", ...
    Verdict verdict = Verdict::Same;
    double score = 0.0;              // the weighted sum (> 0 better)
};

struct ItemComparison {
    bool usable = true;
    std::string why_not;              // "Your class cannot use this item", "Requires level 45", ...
    std::vector<std::string> notes;   // "Recommended level 45: its stats are reduced for you", ...
    std::vector<SlotComparison> slots; // the best swap first; empty when the item has no worn slot for the player
};

struct CompareOptions {
    std::uint32_t class_mask = 0;  // the player's classes, bit c-1 (0: the sheet's class only)
    bool can_use_all = false;      // a server extension answers CanUseItem true for everything
    bool dual_wield = false;       // the player has Dual Wield (skill 22)
    std::uint32_t slots_available = 0x7fffffu; // bit n: worn slot n is available (CharacterBase_IsInvSlotAvailable)
};

// Compares `item` against what `player` wears. `player` has run process_all_stats; `scratch` is reused storage for
// the copies (its old contents are replaced). Returns nullopt for an item that is not worn gear at all (no worn slot
// in its equip slots).
std::optional<ItemComparison> compare_item(const mechanics::Character& player, const net::Item& item,
                                           const CompareOptions& options, mechanics::CharacterSheet& scratch);

// The class weighting (Hailstone's): how much one point of `stat` counts for the classes in `class_mask` (bit c-1),
// the most of any of them. 0 for a stat the classes have no use for.
double compare_weight(CompareStat stat, std::uint32_t class_mask);

} // namespace hailstone::game
