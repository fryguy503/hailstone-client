# Item comparison

Code:

- `src/game/item_compare.{h,cpp}`: the comparison and the verdict;
- `src/game/mechanics/shown_stats.{h,cpp}`: the AC and ATK the inventory window shows, shared with its labels;
- `src/app/lua_ui_compare.cpp`: `hs.CompareItem`, `hs.CompareSlotItem`, `hs.CompareBagSlotItem`;
- `ui/<pack>/lib/compare_lines.lua`: the pieces and lines both packs show (the gamepad pack's is a copy of the
  modern pack's);
- the stat rows' keys: `ItemDisplayCell::key` in `src/game/item_display.cpp`, the window's `IDW_Row<r>Col<c>Key`
  child;
- the slots, tooltips and item displays of both packs.

Tests: `tests/item_compare_test.cpp`, `tests/item_display_test.cpp`, `tests/ui_pack_windows_test.cpp`.

A Hailstone addition (the original has none), for players who cannot yet tell at a glance whether an item beats what
they wear. It only reads: no rule, packet or game number changes.

## What it shows

For a wearable item (an equip slot among worn slots 0..22) the player is not wearing:

- what it would replace: the item in that slot, or the empty slot;
- the player's numbers that would change, before and after (listed below);
- spell effects gained and lost (worn, focus, click, proc; by name, augments included);
- why the player cannot use it, when they cannot (class, required level, race or deity);
- a recommended level above the player's, and the augments in the item taken off (counted with it);
- a verdict, unless Options turns it off: Upgrade, Downgrade, Mixed (better or worse overall) or no change that
  matters for the class.

The numbers compared:

- maximum HP, mana and endurance;
- AC and ATK as the inventory window shows them, and haste;
- the seven stats, the six resists, and HP / mana / endurance regeneration;
- the item bonuses: heal amount, spell damage, clairvoyance, shielding, spell shield, avoidance, accuracy, stun
  resist, strike through, DoT shielding, combat effects, damage shield, damage shield mitigation;
- for the primary, secondary and range slots, also the weapon's damage and delay and the damage / delay ratio.

Where:

- **Item display window** (linked items too), both packs: each change sits beside its stat in the grid, signed and
  coloured (green better, red worse, dimmed when it does not count for the class): `AC  20  +12`.
  - The modern pack puts the verdict (a chip) and "vs your X (Slot)" at the top of the Stats section, with a button
    per slot for an item that fits two (ears, wrists, rings); a row's tooltip gives the before and after.
  - Below the grid come only the changes it has no row for (a stat the worn item has and this one lacks, HP from
    STA), as "+5 STR", then the weapon, the effects and the notes.
  - A stat on several rows (haste from augments) carries its change on the first.
- **Modern pack, tooltips**: every item slot and the merchant's rows. The verdict under the name, the change beside
  the tooltip's AC, HP and mana, the other changes side by side as "+5 STR" (at most eight), and what it is compared
  with. Shift shows the other slot of a pair.
- **Gamepad pack**: the focused item's panel in the inventory, loot and merchant screens lists the changes; the item
  details put them beside the stats as above. A pair's other slot is named at the end ("Also fits your ...").
- **Options**: Compare Items (`item_compare`, `items.compare` in hailstone.ini) and Upgrade Verdict
  (`item_compare_verdict`, `items.compare_verdict`), both on by default; under Display (modern) or Interface
  (gamepad).

## How the numbers are found

The player's `mechanics::CharacterSheet` is copied with the item swapped in and run through the player's own
rules, so stat caps, recommended-level scaling, best-item haste and items that do not count (a level, race, deity or
prestige the player lacks) show as in game. Checked: ProcessAllStats, Max_HP, Max_Mana, Max_Endurance, CalcAC with
ComputeDefense, ATK label case 0x16, regeneration label cases 0x4a..0x4c, TotalEffect's haste.

Both sides leave out a server extension's HP / mana / endurance answers (`CharacterSheet::vitals`), so the change is
the formula's, not hidden by the server's number.

The slot is every worn slot of the item's that the player has (`inv_slot_available`) and the item may go in
(`can_go_in_equip_slot`, with Dual Wield for the secondary).

- Where the original refuses because of the other hand (a two-hander beside a secondary item, or the reverse), that
  item comes off too and both are named.
- Slots are listed best swap first (highest score): of two rings, the weaker one's comes first.

Results are cached half a second per item and place (inventory, window or bag slot), up to eight places, oldest
dropped first, so per-frame tooltips do not rebuild the sheet.

## The verdict

The verdict is judgement, not a rule. Each change is weighted by what one point is worth to the player's class (the
highest of a multiclass player's classes): `kWeights` in `src/game/item_compare.cpp`.

- The classes are grouped as tanks (WAR PAL SHD), melee (MNK ROG BER), hybrids (RNG BRD BST), priests (CLR DRU SHM)
  and casters (NEC WIZ MAG ENC).
- HP and mana count as themselves; the stats raising them count only a little more, since HP and mana already show
  their effect.
- Mana and mana regeneration count only for classes with mana.
- WIS, INT and CHA count only as a class's casting stat (CHA for bards and enchanters).

The verdicts:

- Upgrade: better in something that counts, worse in nothing that counts.
- Downgrade: the reverse.
- Mixed: both; "better overall" or "worse overall" by the weighted sum.
- No change that matters for your class: only changes weighted 0 (they are still listed, dimmed).
- An item the player cannot use has no verdict; the reason is shown instead.

## For scripts

```lua
local c = hs.CompareItem(slot [, bagSlot])     -- an item carried (nil for a worn one)
local c = hs.CompareSlotItem(window, child)    -- a window's item slot; "MerchantWnd", "ItemList:<row>" for a
                                               -- merchant's row, "ItemDisplayWindow", "Hailstone_Item" for a display
local c = hs.CompareBagSlotItem(bagWindow, i)  -- a bag window's slot
-- c = { usable, whyNot, notes = { ... }, slots = { { slot, slotName, current = { name or "" }, verdict, score,
--       changes = { { key, name, before, after, delta, weight } }, weaponBefore, weaponAfter, gained, lost } } }
```

nil out of the game, before the player's stats exist, for an empty slot and for an item that is not worn gear.

## Not done

- The item's own numbers against the worn item's, without the character (the item display shows the item's).
- The bank rows of the Advanced Inventory have no comparison (its worn and carried rows do).
- Focus effects are compared by name only, not by strength.
- Bags (more slots, weight reduction) are not compared.
