# Item comparison

Code: `src/game/item_compare.{h,cpp}` (the comparison and the verdict), `src/game/mechanics/shown_stats.{h,cpp}` (the
AC and ATK the inventory window shows, shared with its labels), `src/app/lua_eq_compare.cpp` (`eq.CompareItem`,
`eq.CompareSlotItem`, `eq.CompareBagSlotItem`), `ui/<pack>/lib/compare_lines.lua` (the lines both packs show; the
gamepad pack's is a copy of the modern pack's), and the slots, tooltips and item displays of both packs.
Tests: `tests/item_compare_test.cpp`, and "the item display compares the item with what you wear" in
`tests/ui_pack_windows_test.cpp`.

The original client has no such thing. This is a Hailstone addition for players who cannot yet tell at a glance
whether an item is better than what they wear. It only reads: no rule, packet or number of the game changes.

## What it shows

For an item that can be worn (an equip slot among the worn slots 0..22), and that the player is not already wearing:

- what it would replace: the item in that slot, or the empty slot;
- the player's numbers that would change, before and after: maximum HP, mana and endurance, AC and ATK as the
  inventory window shows them, haste, the seven stats, the six resists, HP / mana / endurance regeneration, and the
  item bonuses (heal amount, spell damage, clairvoyance, shielding, spell shield, avoidance, accuracy, stun resist,
  strike through, DoT shielding, combat effects, damage shield, damage shield mitigation); for the primary, secondary
  and range slots also the weapon's damage and delay and the damage / delay ratio;
- the spell effects gained and lost (worn, focus, click and proc; by name, the item's augments' included);
- why the player cannot use it, when they cannot (class, required level, race or deity);
- a recommended level above the player's, and the augments in the item taken off (counted with it);
- a verdict, unless Options turns it off: Upgrade, Downgrade, Mixed (better or worse overall) or no change that
  matters for the class.

Where:

- Modern pack: the tooltip of every item slot (inventory, bags, the bank, loot, trade, give and the other windows'
  item slots) and of the merchant's rows, at most eight changes; the item display window (a linked item too) under
  the stats, every change, with a button per slot for an item that fits two (ears, wrists, rings). In a tooltip,
  Shift shows the other slot of a pair.
- Gamepad pack: the focused item's panel in the inventory, loot and merchant (buy and sell) screens, and the item
  details. A pair's other slot is named at the end ("Also fits your ...").
- Options: Compare Items (`item_compare`, `items.compare` in hailstone.ini) and Upgrade Verdict
  (`item_compare_verdict`, `items.compare_verdict`), both on by default; in the gamepad pack under Interface.

## How the numbers are found

The player's sheet (the `mechanics::CharacterSheet` the stats window builds each frame) is copied, the slot is
emptied and the item put in, and the copy runs through the same rules as the player: ProcessAllStats, Max_HP,
Max_Mana, Max_Endurance, CalcAC with ComputeDefense, the ATK of label case 0x16, the regeneration of label cases
0x4a..0x4c, TotalEffect's haste. So the stat caps, the recommended-level scaling, item haste taking the best item's,
and an item that does not count (a level, race, deity or prestige the player lacks: ProcessAllStats skips it) all
show as they would in game. Both sides are computed from copies with a server extension's HP / mana / endurance
answers left out (`CharacterSheet::vitals`, the NMS patch's), so the change is the formula's and not hidden by the
server's number.

The slot: every worn slot in the item's equip slots that the player has (`inv_slot_available`) and the item may go
in (`can_go_in_equip_slot`, with Dual Wield for the secondary). Where the original refuses because of the other hand
(a two-hander beside a secondary item, a secondary item beside a two-hander) the comparison takes that item off too
and names both. The slots are listed with the best swap first (the highest score), so of two rings the weaker
one's comparison comes first.

Results are kept for half a second per item and place, so a tooltip shown every frame does not rebuild the sheet
every frame.

## The verdict

The one part that is judgement, not a rule. Each change is weighted by what one point of it is worth to the player's
class (the most of any of a multiclass player's classes); the classes are grouped as tanks (WAR PAL SHD), melee (MNK
ROG BER), hybrids (RNG BRD BST), priests (CLR DRU SHM) and casters (NEC WIZ MAG ENC). HP and mana count as themselves;
the stats that raise them count a little on top, since their effect already shows in HP and mana. Mana and mana
regeneration count only for classes with mana; WIS, INT and CHA only as a class's casting stat (CHA for bards and
enchanters). The table is `kWeights` in `src/game/item_compare.cpp`.

- Upgrade: better in something that counts, worse in nothing that counts.
- Downgrade: the reverse.
- Mixed: both; "better overall" or "worse overall" by the weighted sum.
- No change that matters for your class: only changes weighted 0 (they are still listed, dimmed).
- An item the player cannot use has no verdict; the reason is shown instead.

## For scripts

```lua
local c = eq.CompareItem(slot [, bagSlot])     -- an item carried (nil for a worn one)
local c = eq.CompareSlotItem(window, child)    -- a window's item slot; "MerchantWnd", "ItemList:<row>" for a
                                               -- merchant's row, "ItemDisplayWindow", "Hailstone_Item" for a display
local c = eq.CompareBagSlotItem(bagWindow, i)  -- a bag window's slot
-- c = { usable, whyNot, notes = { ... }, slots = { { slot, slotName, current = { name or "" }, verdict, score,
--       changes = { { key, name, before, after, delta, weight } }, weaponBefore, weaponAfter, gained, lost } } }
```

nil out of the game, before the player's stats exist, for an empty slot and for an item that is not worn gear.

## Not done

- The item's own numbers against the worn item's (without the character): the item display already shows the
  item's.
- The bank rows of the Advanced Inventory have no comparison (its worn and carried rows do).
- Focus effects are compared by name only, not by strength.
- Bags (more slots, weight reduction) are not compared.
