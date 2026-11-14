# Scripting and plugin reference

This page lists everything a script or plugin can use: the TLOs and their members, the spawn search words, the
commands, the Lua modules (`mq`, `hailstone`, `ImGui`, `ui`, `eq`) and the compiled plugin SDK
(`sdk/hailstone_plugin.h`). It is for people writing add-ons by hand.

Read docs/plugins.md first. It covers where scripts and plugins live, how they are loaded, what a server's
`hailstone.cfg` allows, the capabilities and the sandbox. This page repeats those rules only where a function needs
one. docs/addons.md covers packaging and releasing an add-on, and docs/navigation.md covers `/nav`, `/stick` and the
`Navigation` TLO.

Names follow MacroQuest, so an MQ reference mostly carries over. Where Hailstone differs, or has something MQ lacks,
this page says so.

## Contents

- [Conventions](#conventions)
- [Capabilities](#capabilities)
- [Top-level objects](#top-level-objects)
- [Data types](#data-types)
- [Spawn searches](#spawn-searches)
- [Inventory slots](#inventory-slots)
- [Commands for scripts](#commands-for-scripts)
- [The `mq` module](#the-mq-module)
- [The `hailstone` module (Lua plugins)](#the-hailstone-module-lua-plugins)
- [Windows: `ImGui`, `ui` and `eq`](#windows-imgui-ui-and-eq)
- [Compiled plugins: the C SDK](#compiled-plugins-the-c-sdk)

## Conventions

**Three ways to read a TLO.** One TLO evaluator (`src/mq/tlo.h`) answers all three, so they give the same values:

| Where | Example |
|---|---|
| `${...}` text in a command line, `mq.parse`, a plugin's `ParseMacroData` | `/echo ${Me.Buff[1].Duration.TotalSeconds}` |
| Lua's `mq.TLO` | `mq.TLO.Me.Buff(1).Duration.TotalSeconds()` |
| a compiled plugin's `ParseMacroData` | `ParseMacroData("${Target.PctHPs}", buf, sizeof buf)` |

- **Indexes.** `[...]` in text is a call with arguments in Lua: `Spawn[npc radius 50]` is `mq.TLO.Spawn('npc radius
  50')`. Several Lua arguments are joined with commas, so `mq.TLO.Select('a', 'a', 'b')` is `Select[a,a,b]`.
- **Plain values.** In Lua, call a value with no arguments to get its plain value: a number, a string, a boolean or
  `nil`. `mq.TLO.Target()` gives the target's name, and `mq.TLO.Target.Level()` its level. A TLO value also works
  with `tostring`, `..`, `==`, `<` and `<=`, which use its plain value.
- **NULL.** An unknown TLO or member, a member of nothing (`Target.Level` with no target) and a value that does not
  exist are NULL. That is `nil` in Lua and the text `NULL` in `${...}`. It is never an error.
- **Case.** TLO and member names are not case sensitive (`${me.pcthps}` works).
- **Text form.** In `${...}`, `TRUE`/`FALSE` stand for booleans and floats get two decimals.
- **Inheritance.** A member a type does not have is looked up on its parent: `character` → `spawn`, `buff` → `spell`,
  `groupmember` → `spawn`. If no type in the chain has the member, it is looked up on the plain value, so
  `Target.Name.Length` works.
- **Coordinates.** `X`, `Y`, `Loc` and the `loc` search word use MQ's coordinates, which are the server's (`/loc`
  order: Y first). The `hailstone` module and the C structs use the zone's own coordinates (`game/coords.h`), with x
  mirrored.
- **Timestamps** are milliseconds. A `timestamp` value's plain value is milliseconds too.
- **`${...}` is expanded once, innermost first.** Text that comes out of a value is never parsed again, so a spawn
  named `${Me.Name}` stays that text. This is a deliberate difference from MQ (docs/plugins.md "MQ commands").

## Capabilities

Each function below names the capability it needs. docs/plugins.md "Capabilities" has the full rules.

| Bit | Name | Covers |
|---|---|---|
| 0x001 | `read` | TLOs, `${...}`, `mq.parse`, `ParseMacroData`, spawn events, the `hailstone` read functions, `get_*` |
| 0x002 | `commands` | commands outside the groups below; `mq.bind`, `AddCommand` |
| 0x004 | `target` | `/target`, `/assist`, `/xtarget`, ... |
| 0x008 | `actions` | `/cast`, `/disc`, `/doability`, `/useitem`, `/attack`, the client actions in the `hailstone` module and host table |
| 0x010 | `move` | `/face`, movement `/keypress`, `/follow`, `/sit`, `/stand`, `/nav`, ... |
| 0x020 | `chat` | plain text, `/say`, `/tell`, channels, emotes |
| 0x040 | `events` | `mq.event`, `OnIncomingChat`, `OnFilterChat` |
| 0x080 | `ui` | `mq.imgui`, `OnUpdateImGui`, plugin windows, options, menu entries |
| 0x100 | `packets` | `on_packet`, `send_packet`, `OnPacket` |
| 0x200 | `extend` | hooks, `set_flag`, `set_*` server knowledge |
| 0x400 | `login` | running before the zone-in |
| 0x800 | `net` | `tcp_*` |
| 0x1000 | `web` | `mq.web`, `eq.Web*`, `web_*`, `/url`, `/www` |

The C names are `HAILSTONE_CAP_READ` ... `HAILSTONE_CAP_WEB`. A script asks for the bits it needs in its add-on
manifest's `caps` (docs/addons.md).

## Top-level objects

| TLO | Index | Gives | What |
|---|---|---|---|
| `Me` | | `character` | the player; NULL before the player is in a zone |
| `Target` | | `spawn` | the current target |
| `Spawn` | id, or a [search](#spawn-searches) | `spawn` | the spawn with that id, or the nearest that matches |
| `SpawnCount` | a search | int | how many spawns match (0 when not connected) |
| `NearestSpawn` | `n`, `n,search` or `search` | `spawn` | the nth nearest that matches (1 the nearest) |
| `Pet` | | `spawn` | the player's pet: the one the pet window shows, else the first pet the player owns |
| `Group` | | `group` | the player's group; NULL when not grouped |
| `Zone` | | `zone` | the current zone |
| `Spell` | id or exact name | `spell` | a spell from the spell file |
| `Cursor` | | `item` | the item on the cursor |
| `FindItem` | name, or `=exact name` | `item` | the first carried item whose name contains the text (worn, general slots, bags, cursor) |
| `FindItemCount` | name, or `=exact name` | int | how many such items are carried, stacks counted |
| `Window` | window name | `window` | a window, to ask whether it is open |
| `Plugin` | plugin name | `plugin` | a compiled plugin, to ask whether it is loaded |
| `EverQuest` | | `everquest` | the game state and server name |
| `Time` | | `time` | the computer's local time |
| `GameTime` | | `gametime` | Norrath's time of day; NULL before the server sent it |
| `Heading` | degrees | `heading` | a heading made from a number (wrapped to 0..360) |
| `LineOfSight` | `y,x,z:y,x,z` | bool | line of sight between two points through the zone's collision (below) |
| `Navigation` | | `navigation` | the client's navigation (docs/navigation.md) |
| `Stick` | | `stick` | `/stick`'s state (docs/navigation.md) |
| `Int` | number | int | the number cut toward zero; NULL for NaN, infinity or a number beyond 64 bits |
| `Float` | number | float | the number |
| `Bool` | text | bool | false for empty, `false`, `null` or `0`; true otherwise |
| `String` | text | string | the text |
| `Select` | `x,a,b,c,...` | int | the 1-based position of `x` among the rest (not case sensitive), 0 when absent |

`LineOfSight` is NULL before the zone's collision is loaded, and for a coordinate that is not a finite number below
1 000 000 in size.

A compiled plugin can add TLOs (`AddTopLevelObject`). It cannot replace one that exists. MQ TLOs that are missing:
`Raid`, `Mercenary`, `Corpse`, `Merchant`, `Ground`, `Macro`, `Math`, and other MQ plugins' TLOs (`Navigation` and
`Stick` are built in).

## Data types

### spawn

The value of `Target`, `Spawn[...]`, `NearestSpawn[...]`, `Pet` and members that give a spawn. Its plain value is
the clean name.

| Member | Gives | What |
|---|---|---|
| `ID` | int | spawn id |
| `Name` | string | the raw name (`a_rat01`) |
| `CleanName`, `DisplayName` | string | the name as shown (`a rat`) |
| `Surname`, `Title`, `Suffix` | string | |
| `Level` | int | |
| `Class` | `class` | |
| `Race` | `race` | |
| `Gender` | string | `male`, `female` or `neuter` |
| `Type` | string | `PC`, `NPC`, `Pet`, `Mercenary` or `Corpse` |
| `Deity`, `BodyType` | int | the body type is the spawn record's first property (11: untargetable) |
| `Guild` | int | guild id; NULL for none |
| `PctHPs` | int | health percent |
| `CurrentHPs`, `MaxHPs` | int | for others the server sends only a percent, so these are the percent and 100 (`Me` has real values) |
| `PctMana`, `PctEndurance` | int | known only for group members; NULL otherwise |
| `X`, `Y`, `Z` | float | MQ's (the server's) coordinates |
| `Loc` | string | `"Y, X, Z"` with two decimals |
| `LocYX` | string | `"Y, X"` rounded |
| `Heading` | `heading` | the direction it faces |
| `HeadingTo` | `heading` | the direction from the player to it |
| `Distance` | float | distance from the player, ignoring height |
| `Distance3D` | float | distance from the player |
| `DistanceZ` | float | height difference from the player |
| `MaxRangeTo` | float | its melee radius, at least 14 |
| `Height` | float | its size |
| `Moving` | bool | |
| `StandState` | int | 100 standing, 110 sitting, 111 ducking, 115 feigning |
| `Sitting`, `Standing`, `Ducking`, `Feigning` | bool | |
| `Dead` | bool | a corpse |
| `Targetable` | bool | its Targetable flag alone, as in MQ. The `targetable` search word also checks `BodyType` |
| `Named` | bool | MQ's test: an NPC whose name is not "a ..." or "an ..." and starts with a capital or `#` |
| `ConColor` | string | `GREY`, `GREEN`, `LIGHT BLUE`, `BLUE`, `WHITE`, `YELLOW` or `RED` |
| `Casting` | `spell` | the spell it is casting; NULL when it is not casting |
| `LineOfSight` | bool | the player's eyes to it through the zone's collision, the check the client's own abilities make; NULL before collision is loaded |
| `AFK`, `LFG`, `Linkdead`, `GM`, `Anonymous`, `Roleplaying`, `Trader`, `Buyer`, `Mercenary` | bool | |
| `Master`, `Owner` | `spawn` | a pet's owner |
| `Pet` | `spawn` | its pet (for the player, the pet the pet window shows) |
| `xtarget` | bool | on the player's extended target list |
| `BuffsPopulated` | bool | its buffs are known: always for the player, and for the target once the server sent OP_TargetBuffs |
| `BuffCount` | int | |
| `Buff[n\|name]` | `buff` | the nth buff, or one by name (exact first, else a part of it; `=name` exact only). With no index, the count |
| `MyBuff[n\|name]` | `buff` | the same, counting only buffs the player cast |
| `BuffDuration[n\|name]`, `MyBuffDuration[n\|name]` | `timestamp` | time left on that buff |
| `Mezzed`, `Charmed`, `Feared`, `Rooted`, `Snared`, `Slowed` | `buff` | the buff that puts it in that state; NULL when none does (below) |

A spawn's buffs are the player's own, or the target's as OP_TargetBuffs gave them. Stock EQEmu sends those only to
GMs, players with the Inspect Buffs AA or leadership, or when its `AlwaysSendTargetsBuffs` rule is on. Check
`BuffsPopulated` first. The states use the buff's spell effects (EQEmu's spdat.h): `Mezzed` SPA 31, `Charmed` 22,
`Feared` 23, `Rooted` 99, `Snared` SPA 3 with a negative value, `Slowed` SPA 11 below 100%.

### character

The value of `Me`. It has every `spawn` member, and these replace or add to them. Its plain value is the
character's name.

| Member | Gives | What |
|---|---|---|
| `Name`, `Level`, `Class`, `Race`, `Deity` | | from the player's profile |
| `PctHPs`, `CurrentHPs`, `MaxHPs` | int | real values, the items' HP included (as the windows show them) |
| `PctMana`, `CurrentMana`, `MaxMana` | int | |
| `PctEndurance`, `CurrentEndurance`, `MaxEndurance` | int | |
| `Exp`, `AAExp` | int | the raw values (0..330) |
| `PctExp`, `PctAAExp` | float | percent of the level or AA point |
| `AAPoints`, `AAPointsSpent` | int | |
| `Buff[n\|name]` | `buff` | the nth long buff, or one by name; with no index, the count |
| `Song[n\|name]` | `buff` | the same for short buffs (songs) |
| `CountBuffs`, `CountSongs` | int | |
| `MaxBuffSlots` | int | 42 |
| `FreeBuffSlots` | int | 42 minus the long buffs |
| `Gem[n]` | `spell` | the spell in gem n |
| `Gem[name]` | int | the gem that holds that spell |
| `NumGems` | int | how many gems the player can use |
| `Book[n]` | `spell` | the spell in book slot n |
| `Book[name]` | int | the book slot that holds that spell |
| `SpellReady[n\|name]` | bool | the gem has a spell, it is ready and the player is not casting |
| `Casting` | `spell` | the spell being cast |
| `Combat` | bool | auto attack is on |
| `CombatState` | string | `COMBAT`, `RESTING` or `ACTIVE` |
| `Sitting`, `Standing`, `Ducking`, `Feigning`, `Dead`, `Moving` | bool | |
| `Stunned`, `Invis`, `Sneaking` | bool | |
| `Hunger`, `Thirst` | int | |
| `Platinum`, `Gold`, `Silver`, `Copper` | int | carried coin |
| `Cash` | int | all carried coin, in copper |
| `Grouped` | bool | |
| `GroupSize` | int | the player included |
| `XTarget[n]` | `spawn` | the spawn in extended target slot n; with no index, how many slots hold a spawn |
| `Inventory[slot]` | `item` | the item in a slot, by [name or number](#inventory-slots) |
| `FreeInventory` | int | empty general slots plus empty bag slots |
| `Pet` | `spawn` | as the `Pet` TLO |
| `AbilityReady[name\|number]` | bool | a skill's reuse timer has run out |
| `CombatAbilityReady[name]` | bool | the same for a combat ability (a discipline); the name works without its "Rk. II" |
| `AltAbilityReady[name\|group]` | bool | the same for an activated AA |
| `AbilityTimer`, `CombatAbilityTimer`, `AltAbilityTimer` | `timestamp` | time left before it is ready |

"Ready" means only that the reuse timer has run out. Checking endurance and mana is up to the script. Ability names
are matched as the Ability Book shows them.

Not yet: the `AltAbility` type (`Me.AltAbility[...]`).

### buff

A buff slot. It has every `spell` member (the buff's spell), and these. Its plain value is the spell's name.

| Member | Gives | What |
|---|---|---|
| `ID` | int | the slot, 1-based |
| `Spell` | `spell` | |
| `Level` | int | the caster's level |
| `Caster` | string | the caster's name |
| `Counters`, `HitCount` | int | the hits or counters left |
| `Mod` | float | the modifier |
| `Duration`, `Dur` | `timestamp` | time left; NULL for a permanent buff |

`Spawn.BuffDuration` and `MyBuffDuration` are NULL for a permanent buff as well.

### spell

The value of `Spell[...]`, `Me.Gem[n]`, `Me.Casting` and the like. Its plain value is the name.

| Member | Gives | What |
|---|---|---|
| `ID`, `Name` | | |
| `Level` | int | the level the player's class gets it; NULL when the class cannot use it |
| `Mana` | int | |
| `Range`, `AERange` | float | |
| `CastTime`, `RecastTime`, `RecoveryTime` | float | seconds |
| `MyCastTime` | `timestamp` | the cast time (not adjusted for focus) |
| `Duration` | `timestamp` | the spell file's duration cap, in ticks of 6 s |
| `TargetType` | string | `Single`, `Self`, `Group v1`, `PB AE`, `Targeted AE`, `Line of Sight`, ... |
| `SpellType` | string | `Beneficial` or `Detrimental` |
| `Beneficial` | bool | |
| `ResistType` | string | `Magic`, `Fire`, `Cold`, `Poison`, `Disease`, `Chromatic`, `Prismatic`, `Physical`, `Corruption` or `Unresistable` |
| `ResistAdj`, `Skill`, `SpellIcon`, `GemIcon`, `Category` | int | raw spell file values |

### item

An item in a slot. Its plain value is the item's name.

| Member | Gives | What |
|---|---|---|
| `ID`, `Name`, `Icon` | | |
| `Lore` | bool | |
| `NoDrop`, `NoRent` | bool | |
| `Stack` | int | how many are in the stack |
| `Stackable` | bool | |
| `StackSize` | int | the largest stack |
| `Charges` | int | -1 for unlimited |
| `Weight` | float | |
| `Value` | int | in copper |
| `Type` | string | `Container`, `1H Slashing`, `Armor`, `Food`, `Scroll`, `Augmentation`, ... |
| `Container` | int | a bag's slot count; 0 for an item that is not a bag |
| `Items` | int | how many items a bag holds |
| `Item[n]` | `item` | the item in bag slot n (1-based); only for a bag in a top-level slot |
| `ItemSlot` | int | the [slot](#inventory-slots) it is in |
| `ItemSlot2` | int | its place in the bag (0-based); -1 when it is not in a bag |

### group and groupmember

`Group` is NULL when the player is not grouped. Its plain value is the number of other members.

| `group` member | Gives | What |
|---|---|---|
| `Members` | int | members besides the player |
| `GroupSize` | int | the player included |
| `Member[n]` | `groupmember` | member n; 0 is the player |
| `Member[name]` | int | that member's index |
| `Leader`, `MainTank`, `MainAssist`, `Puller` | `groupmember` | the member with that role |

A `groupmember` has every `spawn` member (of its spawn when it is in the zone), and these. Its plain value is the
name.

| `groupmember` member | Gives | What |
|---|---|---|
| `Name` | string | |
| `Index` | int | its place in the group, 0 the player |
| `Spawn` | `spawn` | NULL when it is not in this zone |
| `Level` | int | also known when it is in another zone |
| `Leader`, `MainTank`, `MainAssist`, `Puller` | bool | |
| `Mercenary`, `Offline` | bool | |
| `OtherZone` | bool | online but not in this zone |

### Small types

| Type | Plain value | Members |
|---|---|---|
| `class` | its name | `ID`, `Name`, `ShortName` (`WAR`, `CLR`, ...) |
| `race` | its name | `ID`, `Name` |
| `heading` | the compass point (`N`, `NNE`, ...) | `Degrees` (clockwise from north), `DegreesCCW`, `ShortName`, `Name` (`north by northeast`), `Clock` (1..12) |
| `timestamp` | milliseconds | `Raw` (ms), `TotalSeconds`, `TotalMinutes`, `Hours`, `Minutes` (0..59), `Seconds` (0..59), `Ticks` (6 s, rounded up), `TimeHMS` (`h:mm:ss`), `Time` (`m:ss`) |
| `zone` | its long name | `ID`, `Name`, `ShortName` |
| `everquest` | `EverQuest` | `GameState` (`INGAME`, `CHARSELECT` or `PRECHARSELECT`), `Server` (the server's name), `UIScale` (always 1.0) |
| `window` | `TRUE` or `FALSE` | `Open`, `Name` |
| `plugin` | its name when loaded, else NULL | `IsLoaded`, `Name` |
| `time` | `hh:mm:ss` | `Hour`, `Minute`, `Second`, `Day`, `Month`, `Year`, `DayOfWeek` (1 Sunday), `Date` (`MM/DD/YYYY`), `Time24`, `SecondsSinceMidnight`, `Night` (before 6:00 or from 19:00) |
| `gametime` | `hh:mm` | `Hour`, `Minute`, `Day`, `Month`, `Year`, `Night`, `Time24` |
| `navigation` | `Active` | `Active`, `Paused`, `MeshLoaded`, `MeshComplete`, `TilesBuilt`, `TilesTotal`, `Destination`, `Remaining`, `PathExists[dest]`, `PathLength[dest]` (-1 for no path); docs/navigation.md "The Navigation TLO" |
| `stick` | `ON` or `OFF` | `Active`, `Status` (`ON`, `PAUSED` or `OFF`), `Distance`, `StickTarget` (spawn id, 0 none), `StickTargetName` |

`Window[name]` asks the UI whether a window of that name is open. The names are the game's own window names, which
the UI packs use too (`InventoryWindow`, `LootWnd`, `BankWnd`, `GroupWindow`, ...).

### Pure types

Every number, string and boolean has these members, as in MQ.

| Type | Members |
|---|---|
| `int` | `Float`, `Hex` (`0x1f`), `Reverse` (byte order of the low 32 bits) |
| `float` | `Int` (cut toward zero; NULL for NaN, infinity or beyond 64 bits), `Deci`, `Centi`, `Milli` (1, 2 or 3 decimals, as text), `Precision[n]` (n decimals, 0..12), `Raw` (the 32-bit float's bits) |
| `string` | `Length`, `Upper`, `Lower`, `Left[n]`, `Right[n]` (a negative n counts from the other end), `Mid[start,n]` (1-based), `Find[text]` (1-based, not case sensitive; NULL when absent), `Equal[text]`, `NotEqual[text]` (not case sensitive), `EqualCS[text]`, `Arg[n,sep]` (the nth word, `sep` a space by default), `Token[n,sep]` (like `Arg`, but empty fields count), `Count[c]` (how many times character c appears) |
| `bool` | none |

## Spawn searches

`Spawn[...]`, `SpawnCount[...]`, `NearestSpawn[...]`, `/target`, `/face` and `/nav` take MQ's search words. The
matches are sorted nearest first. Any word that is not a keyword is part of the name.

| Kind | Matches |
|---|---|
| `pc`, `npc`, `pet`, `merc` / `mercenary` | that kind (pets and mercenaries are NPCs too) |
| `corpse`, `npccorpse`, `pccorpse` | corpses |
| `any` | everything (the default) |

| Filter | Matches |
|---|---|
| `id N`, `notid N` | that spawn id, or not it (`notid` can be repeated) |
| `radius R` | within R of the player (or of `loc`), ignoring height |
| `zradius R` | within R in height |
| `range MIN MAX`, `level N` | level between MIN and MAX, or exactly N |
| `class X`, `race X` | by name (`class wizard`, `class WIZ`) |
| `loc X Y [Z]` | measures `radius` and the sort from that point instead of the player (MQ's coordinates, X first) |
| `named` | `Spawn.Named` |
| `targetable` | the game's own test: the Targetable flag and a body type other than 11 |
| `group` | members of the player's group |
| `nopet` | not a pet |
| `gm`, `trader`, `buyer`, `afk`, `lfg`, `linkdead` | spawns with that flag |
| a name, `=Name` or `exact` | a part of the name (not case sensitive); `=` or `exact` asks for the whole name |

`next`, `prev` and `los` are accepted and ignored for now. MQ's alert list words (`alert N`, `noalert N`, ...) are
skipped, because there are no alert lists.

## Inventory slots

`Me.Inventory[...]` and the `hailstone` module take a slot's name or its number. These are RoF2's slot numbers,
which `/useitem` uses too.

| # | Name | # | Name | # | Name |
|---|---|---|---|---|---|
| 0 | `charm` | 12 | `hands` | 23..32 | `pack1` .. `pack10` |
| 1 | `leftear` | 13 | `mainhand` | 33 | `cursor` |
| 2 | `head` | 14 | `offhand` | | |
| 3 | `face` | 15 | `leftfinger` | | |
| 4 | `rightear` | 16 | `rightfinger` | | |
| 5 | `neck` | 17 | `chest` | | |
| 6 | `shoulder` | 18 | `legs` | | |
| 7 | `arms` | 19 | `feet` | | |
| 8 | `back` | 20 | `waist` | | |
| 9 | `leftwrist` | 21 | `powersource` | | |
| 10 | `rightwrist` | 22 | `ammo` | | |
| 11 | `range` | | | | |

## Commands for scripts

All of the game's commands work from a script (`mq.cmd`) or plugin (`DoCommand`), as long as its capabilities allow
them. These commands come from MacroQuest rather than the game:

| Command | What |
|---|---|
| `/echo text` | a line in the chat window |
| `/target id N` \| `clear` \| `mycorpse` \| a search | targets |
| `/face` [target] \| `id N` \| `heading H` \| `loc y,x` \| a search [`fast`] | turns to face |
| `/keypress <command> [hold\|release]` | presses a key command by the game's name: `forward`, `jump`, `hot1_1`, ... |
| `/squelch <cmd>` | runs the command without its chat output |
| `/docommand <cmd>` | runs the command (useful after `${...}` builds it) |
| `/noparse <cmd>` | runs the command without expanding `${...}` |
| `/lua run <name> [args]` \| `stop [name]` \| `pause [name]` \| `list` | scripts |
| `/plugin list` \| `/plugin <name> [load\|unload]` | compiled plugins |
| `/nav`, `/moveto`, `/stick` | navigation (docs/navigation.md "Commands") |

`${...}` is expanded in every command line, whether typed or sent by a script, except `/noparse` lines and the lines
the client's own windows send for a button.

## The `mq` module

The global `mq` (also `require('mq')`) in every script, Lua social and Lua plugin.

| Function | Needs | What |
|---|---|---|
| `mq.TLO.<Name>...` | `read` | the TLOs above |
| `mq.cmd(...)` | per command | runs a command line; its arguments are joined with spaces and `${...}` is expanded |
| `mq.cmd.<name>(args...)` | per command | `mq.cmd.target('id', 5)` runs `/target id 5` |
| `mq.cmdf(fmt, ...)` | per command | `mq.cmd(string.format(fmt, ...))` |
| `mq.delay(ms \| '2s' \| '500ms' \| '1m' [, cond])` | | waits until the time has passed or `cond()` is true (checked each frame). A wait beyond 9e15 ms (`math.huge`) is cut to that; NaN is an error |
| `mq.parse(text)` | `read` | `${...}` expanded |
| `mq.gettime()` | | milliseconds since an arbitrary start, for timing |
| `mq.exit()` | | ends the script |
| `mq.event(name, pattern, fn)` | `events` | calls `fn(line, arg1, ...)` for a chat line that matches the whole pattern (below) |
| `mq.unevent(name)` | | |
| `mq.doevents([name])` | | runs the handlers of the lines that matched since the last call |
| `mq.flushevents([name])` | | drops lines that matched but have not been handled |
| `mq.bind('/cmd', fn)` | `commands` | a command of the script's; `fn` gets the words and may `mq.delay` |
| `mq.unbind('/cmd')` | | |
| `mq.getAllSpawns()` | `read` | every spawn in the zone as `spawn` values, in an array sorted by id |
| `mq.getFilteredSpawns(fn)` | `read` | those for which `fn(spawn)` is true |
| `mq.imgui.init(name, fn)` | `ui` | `fn` is called every frame to draw with `ImGui` |
| `mq.imgui.destroy(name)`, `mq.imgui.exists(name)` | | |
| `mq.pickle(path, table)` | | writes the table as Lua to `path` under `mq.configDir`. Read it back with `dofile` / `loadfile`, as data: no functions |
| `mq.web.*` | `web` | the script's own browser tabs (below) |
| `mq.luaDir` | | the script folder the script was found in |
| `mq.configDir` | | the folder `mq.pickle` writes to: `scripts/config/`, or `scripts/config/<script>/` for a script without every capability |
| `print(...)`, `printf(fmt, ...)` | | a line in the chat window |
| `require(name)` | | `mq`, `ImGui`, `hailstone`, or a module file next to the script or in the script folders |

**Event patterns** (`mq/event_pattern.h`): the pattern must match the whole line. `#*#` matches any text, as much as
it can. `#1#`, `#2#`, ... match as little as they can and are passed to `fn` in their number's order. Neither
matches across a line break, and any other `#` is literal. A script can have at most 256 events. A pattern can be at
most 1024 bytes and a name 256, and all of a script's patterns together at most 32 KB. Lines that other scripts or
plugins write raise events too.

```lua
mq.event('tell', '#1# tells you, \'#2#\'', function(line, who, what)
    printf('%s said %s', who, what)
end)
while true do
    mq.doevents()
    mq.delay(100)
end
```

**`mq.bind`** cannot take one of the client's own commands (the game's, with their abbreviations, `/lua`, `/plugin`,
MQ's) or a name another script or plugin already has. A bind runs with the script's capabilities and those of
whoever ran the command, both together.

**`mq.web`**: `open(url [, w, h])` gives a tab id. The tab functions are `navigate(tab, url)`, `nav(tab, 'back' |
'forward' | 'reload' | 'stop')`, `close(tab)`, `eval(tab, js)`, `post(tab, text)` (calls the page's
`window.hailstone.onmessage`), `messages(tab)` (an array of what the page sent with `window.hailstone.post`),
`text(tab, text)` (typed at the page's cursor) and `view(tab, w, h)` (draws the page inside an `mq.imgui` window).
`state(tab)` gives `{url, title, loading, canBack, canForward, closed, why}`, `tabs()` lists the tab ids and
`available()` says whether the browser helper is installed. docs/plugins.md "Web pages" has the rules and limits.

**Not available:** `load`, `loadstring`, `string.dump`, `io`, and every `os` function except `os.clock`, `os.time`,
`os.date` and `os.difftime`. Tables cannot have a `__gc` finalizer.

**Limits:** each script has its own Lua 5.4 state. It is paused after 3 ms without yielding and stopped if it cannot
pause for 5 s. Its memory is capped at 256 MB.

## The `hailstone` module (Lua plugins)

The global `hailstone` (also `require('hailstone')`) lets a script do what a compiled plugin does, without native
code. A script that registers any of these stays loaded after its main chunk returns, until it is stopped.

### Reading the game (`read`)

| Function | Gives |
|---|---|
| `player()` | `{spawn_id, name, level, race, class, visible_class, class_mask, cur_hp, max_hp, cur_mana, max_mana, cur_endurance, max_endurance, target_id, zone_id, game_state, in_group, in_raid, spell_book_open, buff_slots, xtarget_slots, casting_spell_id, scribing_spell_id, stand_state, pet_id, filter_pc_spells, in_zone, aa_points, aa_spent}`; `cur_hp` / `max_hp` include the items' HP |
| `spawn(id)` | `{id, name, level, class, race, owner, guild, npc, corpse, x, y, z, heading, hp_percent, rider, mercenary, gm, body_type}`, or nil. x, y, z are the zone's coordinates and `heading` is 0..512 |
| `spawns()` | every spawn in the zone as such tables, in an array sorted by id |
| `spell(id)` | `{id, name, class_level (16 entries, 255 cannot use), timer_id, cast_time, recovery_time, recast_time (ms), mana, target_type, skill, is_skill, gem_icon, range, ae_range, push_back, duration_formula, duration_cap (ticks), spell_type, resist, resist_adj, spell_icon, category, subcategory, cast_on_you, cast_on_other, wear_off, effects}`. `effects` has 12 1-based `{effect, base, base2, max, formula}` entries. These are the raw record values |
| `item(slot [, sub])` or `item{type, slot, sub, aug}` | `{id, name, type, item_class, classes, races, icon, stack, charges, req_level, bag_slots, click_spell, scroll_spell}`. An item with a click effect adds `click_type, click_level, click_cast_time (ms), click_recast (s), click_recast_type, click_max_charges (-1 unlimited), click_ready_in (s, 0 ready)`. `sub` is the place in a bag, from 0 |
| `buff(i)` | `{spell_id, tics, level, caster_id}` for buff slot i (1-based), or nil when the slot is empty |
| `xtarget(i)` | `{type, status, spawn_id, name}` for extended target slot i (1-based) |
| `eqstr(id, ...)` | a game string (eqstr_us.txt) with `%1`, `%2`, ... filled; nil when there is none |
| `zone_name(id)` | a zone's short name |
| `server()` | `{name, host, port, world, world_host}`, the server this login is on; nil before the world login |
| `autoskill_list()` | the Ability Book's autoskill list in order: `{key, name, endurance, mana, target_hp_below, target_hp_above, self_hp_below, when, keep_buff}` |
| `alt_abilities()` | the AA window's abilities: `{id, group, name, tab, category, rank, max_rank, cost, level, maxed, can_train, activated, prereqs = {{group, rank, met}}}` |
| `server_rule(section, key)` | a setting from a section of the server's `hailstone.cfg` that the client does not use itself (`[autocombat] hunter`), as text; nil when it is not set |
| `server_dir()`, `character_dir()` | the script's folders for this server and this character (`plugin-config/<name>/`), made when first asked for |
| `max_camera_distance()` | the camera's zoom limit |

### Acting (`actions`)

| Function | What |
|---|---|
| `use_ability(kind, name)` | uses a skill (`"skill"`), combat ability (`"disc"`) or activated AA (`"aa"`) as its hot button does; false when the player has no such ability |
| `train_aa(group)` | buys the next rank of an AA as `/alternateadv buy` does, if the AA window's own check allows it; false (and nothing sent) otherwise |
| `hold_autoskill(on)` | stops the Ability Book's autoskill from using anything while held. The hold ends when the script stops |
| `right_click_item(slot [, class])` | the client's own right click on an item; `class` makes the casting check see that class |
| `close_spell_book()`, `clear_spell_recovery()` | |
| `set_window_pet(id)` | shows another of the player's pets in the pet window (only on this client; the server is not told) |
| `inspect_item(id [, augments])` | opens an item's display as a click on its link would |
| `set_max_camera_distance(d)` | |

### Chat, packets and windows

| Function | Needs | What |
|---|---|---|
| `chat(text [, color])` | | a chat line in one of the game's chat colours (15 yellow, 2 green, 6 grey, ...) |
| `on_packet(stream, direction, opcode, fn)` | `packets` | `fn(opcode, data)` for each matching packet; `stream` is `'world'` or `'zone'`, `direction` `'in'` or `'out'`, and opcode 0 matches every packet. Return `true` to drop the packet, or a string to replace its data. Use `string.pack` / `string.unpack` for the layout |
| `send_packet(stream, opcode, data)` | `packets` | sends a packet to the server |
| `ui_function(name, fn)` | `ui` | a function the script's windows call as `eq.Plugin("<script>").<name>(...)` |
| `option{name, label, min, max, get, set}` | `ui` | an option in the Options window's Plugins section; `max = 1` makes a switch, otherwise a slider |
| `menu(label, command)` | `ui` | an entry in the EQ menu's Plugins submenu that runs `command` |

### Server extensions (`extend`)

| Function | What |
|---|---|
| `hooks{name = fn, ...}` | answers the core's questions (below) |
| `set_active(on)` | the hooks, switches and vitals apply only while active (the default). Turn it off until the script has seen its server |
| `set_flag(name [, on])` | a rule switch: `wide_item_links`, `items_any_class`, `hide_any_class`, `aa_any_level`, `aa_train_all`, `augment_classes`, `research_classes`, `focus_classes`, `backstab_either_hand`, `solo_adventure` (as [`HAILSTONE_FLAG_*`](#constants)) |
| `set_player_classes(mask)` | the player's classes, bit c-1 for class c (multiclass) |
| `set_character_classes(name, mask)` | a character's classes at character select; 0 removes them |
| `set_pet_list{{spawn = id, class = c}, ...}` | the player's pets, when the server allows several |
| `set_vitals{max_hp, cur_hp, max_mana, cur_mana, max_endurance, cur_endurance}` | the server's vitals in place of the client's formulas (any subset); nil clears them |

### Hooks

`hailstone.hooks{...}` takes functions with the names of the compiled plugins' `HailstoneHooks` (below). A Lua hook
gets the same arguments without the `out` pointer and returns its answer. `nil` keeps the stock answer. Hooks run on
the script's own Lua state and must finish within 50 ms. An error in a hook ends the script.

| Hook | Arguments → answer |
|---|---|
| `zone_entry_retry` | () → true runs the zone-in's last step again |
| `game_command` | (handler, args) → true when handled |
| `who_lines` | (`{entries = {{name, guild, zone_name, account, flags, class, level, race, zone_id}}, line, title_string, footer_string, footer_count}`) → an array of lines to print instead |
| `cast_class` | () → the class the client's checks see; 0 the profile's |
| `is_spellcaster` | () → bool |
| `spell_level_needed` | (spell_id, class) → level |
| `song_blocked` | (bard_level) → true: never a song |
| `own_begin_cast_line` | (spell_id) → true prints "You begin casting ..." |
| `can_use_gem` | (gem) → bool |
| `ability_timer_id` | (spell_id) → timer id |
| `discipline_reuse` | (timer_id) → ready time in Unix seconds |
| `before_use_skill` | (skill) → true allows it, false refuses it |
| `before_combat_ability` | (spell_id) → true allows it, false refuses it |
| `after_combat_ability` | (spell_id, used) |
| `combat_effect_label` | () → text |
| `guild_member_classes` | (class_value) → class mask |
| `vitals_classes` | (class_id) → class mask |
| `skill_gate` | (skill, class_mask, profile_skill) → bool |
| `skill_spoof_class` | (skill, class_mask) → class |
| `tracking_allowed` | (class_id, class_mask, skill) → bool |
| `item_level_line` | (spell_id) → text for the item display's Level Needed line |
| `label` | (id, text) → text |
| `hide_xtarget_slot` | (slot) → true leaves the slot out |
| `item_right_click` | (`{type, slot, sub, aug}`) → true when taken |
| `list_level` | (item table) → text for the merchant list's Lvl column |
| `reuse_focus_dropped` | (spell_id) → true: no reuse focus |
| `map_spawn` | (spawn table) → true shows it as the defaults say, a table `{color, layer, label}` changes them, falsy leaves it off |
| `nameplate` | (spawn table, text) → the text for its nameplate. Lua only. The spawn table is reused for every plate, so copy what you need |

`command_line` is for compiled plugins only.

## Windows: `ImGui`, `ui` and `eq`

Three Lua APIs draw windows:

- **`ImGui`** is MQ's ImGui table, for an MQ-style script drawing in an `mq.imgui.init` function.
- **`ui`** is the client's own ImGui layer, which `ImGui` is built on. The UI packs, plugin windows and scripts use it.
- **`eq`** is the game's state and the stock windows' logic, for a UI pack's windows and the windows a plugin or Lua
  plugin ships in its `ui/<pack>/` folder (docs/plugins.md "Plugin windows (ABI 2)"). Scripts do not have it.

### `ImGui` (MQ scripts)

`ImGui = setmetatable({}, {__index = ui})`: every `ui.*` name above is reachable as `ImGui.*`. These are overridden
to take MQ's argument and return order:

- `ImGui.Begin(name, open, flags)` returns `still_open, visible` when `open` is given, else `visible`.
- `ImGui.Text`, `TextWrapped`, `TextDisabled` and `BulletText` take `(fmt, ...)` and string.format it.
- `ImGui.TextColored` takes `(r, g, b, a, fmt, ...)` or `(ImVec4, fmt, ...)`.
- `ImGui.Button(label, w, h | ImVec2)`, `ImGui.SetNextWindowSize(w, h, cond | ImVec2, cond)` and
  `ImGui.SetNextWindowPos(x, y, cond | ImVec2, cond)`.
- Globals: `ImVec2(x, y)` and `ImVec4(x, y, z, w)` (plain tables), plus `ImGuiWindowFlags`, `ImGuiCond`, `ImGuiCol`,
  `ImGuiStyleVar`, `ImGuiTableFlags`, `ImGuiTableColumnFlags`, `ImGuiInputTextFlags`, `ImGuiComboFlags`,
  `ImGuiSelectableFlags`, `ImGuiMouseButton` (= ui.Mouse) and `ImGuiHoveredFlags`. There are no aliases for
  ButtonFlags, Key, TabItemFlags or TabBarFlags.
- Any MQ ImGui function that is not in `ui` is nil, so check before porting a script that uses a rarer one.

### `ui`

Colours are `r, g, b, a` in 0..1. A value ImGui would change through a pointer comes back as a return value:
`value, changed = ui.Checkbox('Show', value)`. `ui` needs no capability, since it only draws, except the clipboard
functions, which raise an error unless the caller may do everything. A plugin's code sees `ui` read-only. Textures
(`tex`) are the light userdata that functions such as `eq.ItemIcon` and `eq.SpellIcon` return, with their UV
rectangle (`u0, v0, u1, v1`).

#### Host functions (lua_host.cpp)

| Function | Args → returns | What |
|---|---|---|
| window | (name, drawFn) → — | Register or replace a window drawn by calling drawFn each frame. A plugin's window covers the pack's of the same name |
| base_window | () → bool | From a draw function: draw the window this one covers (false = none) |
| GetSetting | (key [, default]) → string | A saved setting. Plugin code reads "plugin.<tag>.<key>" |
| SetSetting | (key, value) → bool | Save a setting (refused if it holds '=' or a newline) |
| DisableWindow | (name) → — | Turn a script window off (saved) |
| Log | (text) → — | Write a line to the client log |

#### Windows

| Function | Args → returns | What |
|---|---|---|
| Begin | (title [, open [, flags [, collapseRight]]]) → visible, open | Begin a window. open nil = no close button |
| End | () → — | |
| BeginChild | (id [, w, h [, border [, flags]]]) → bool | |
| EndChild | () → — | |
| SetNextWindowSize | (w, h [, cond=FirstUseEver]) → — | |
| SetNextWindowPos | (x, y [, cond=FirstUseEver]) → — | |
| SetNextWindowBgAlpha | (a) → — | |
| SetNextWindowCollapsed | (collapsed [, cond=Always]) → — | |
| SetNextWindowFocus | () → — | |
| IsWindowCollapsed | () → bool | |
| SetWindowFontScale | ([scale=1]) → — | |
| GetWindowPos | () → x, y | |
| GetWindowSize | () → w, h | |
| FindWindowRect | (name) → x, y, w, h \| nothing | Any window's place now (before Begin: after this frame's drag) |
| IsWindowFocused | ([flags]) → bool | |
| IsWindowHovered | ([flags]) → bool | |
| IsWindowAppearing | () → bool | false outside a window |
| SetWindowFocus | ([name]) → — | nil takes focus from every window |

#### Text

| Function | Args → returns | What |
|---|---|---|
| Text | (text) → — | Unformatted |
| TextColored | (r, g, b, a, text) → — | |
| TextDisabled | (text) → — | |
| TextWrapped | (text) → — | |
| SeparatorText | (text) → — | |
| LabelText | (label, text) → — | |
| BulletText | (text) → — | |
| CalcTextSize | (text) → w, h | |
| PushTextWrapPos | ([x=0]) → — | 0 = window edge |
| PopTextWrapPos | () → — | |

#### Widgets

| Function | Args → returns | What |
|---|---|---|
| Button | (label [, w, h]) → bool | |
| SmallButton | (label) → bool | |
| InvisibleButton | (id [, w=1, h=1 [, flags]]) → bool | flags ui.ButtonFlags (EnableNav, MouseButtonLeft/Right) |
| EdgeButton | (id, x0, y0, x1, y1) → bool | Invisible button at a screen rect, reaching to the window edge (resize handles) |
| ImageButton | (id, tex, w, h [, u0, v0, u1, v1]) → bool | A plain Button if tex is invalid |
| Selectable | (label [, selected [, flags [, w, h]]]) → bool | |
| Checkbox | (label, value) → value, changed | |
| SliderFloat | (label, value [, min=0, max=1]) → value, changed | |
| SliderInt | (label, value [, min=0, max=100 [, text]]) → value, changed | text replaces the shown number |
| InputText | (label [, text [, cap=256 [, flags [, {history, tab, moveToEnd}]]]]) → text, changed | history(older)→text, tab(text, shift)→text |
| InputTextWithHint | (label, hint [, text [, cap [, flags]]]) → text, changed | |
| InputTextMultiline | (label [, text [, cap=4096 [, w, h [, flags]]]]) → text, changed | flags limited to ReadOnly and AutoSelectAll |
| CodeEditor | (id [, text [, w, h [, {error_line, read_only, complete}]]]) → text, changed, cursor, active | Lua editor with colours. complete(before) → start, {names} |
| SetClipboardText | (text) → — | Only for a script that may do everything |
| GetClipboardText | () → string | Same restriction |
| SetKeyboardFocusHere | ([offset=0]) → — | |
| SetNavCursorVisible | (bool) → — | |
| BeginCombo | (label, preview [, flags]) → bool | |
| EndCombo | () → — | |
| SetItemDefaultFocus | () → — | |
| ProgressBar | (fraction [, w=-1, h [, overlay]]) → — | |
| CollapsingHeader | (label [, flags]) → bool | |
| TreeNode | (label) → bool | |
| TreePop | () → — | |
| Image | (tex, w, h [, u0, v0, u1, v1 [, r, g, b, a]]) → — | r, g, b, a tint it (white by default). Draws a Dummy if tex is invalid |

#### Layout and cursor

| Function | Args → returns | What |
|---|---|---|
| SameLine | ([offset [, spacing=-1]]) → — | |
| NewLine / Separator / Spacing | () → — | Three functions |
| Dummy | (w, h) → — | |
| Indent / Unindent | ([w]) → — | |
| BeginGroup / EndGroup | () → — | |
| PushID | (string\|number) → — | |
| PopID | () → — | |
| SetNextItemWidth | (w) → — | |
| GetContentRegionAvail | () → w, h | |
| GetCursorPos | () → x, y | Window-relative |
| SetCursorPos | (x, y) → — | |
| GetCursorScreenPos | () → x, y | |
| SetCursorScreenPos | (x, y) → — | |
| GetScrollY / GetScrollMaxY | () → number | |
| SetScrollY | (y) → — | |
| SetScrollHereY | ([ratio=0.5]) → — | |
| PushClipRect | (x0, y0, x1, y1 [, intersect]) → — | Screen pixels. Scripts can pop only their own pushes |
| PopClipRect | () → — | |
| BeginDisabled | ([disabled=true]) → — | |
| EndDisabled | () → — | |
| SetNextItemAllowOverlap | () → — | |

#### Tables

| Function | Args → returns | What |
|---|---|---|
| BeginTable | (id, columns(1..64) [, flags [, w, h]]) → bool | |
| EndTable | () → — | |
| TableNextRow | () → — | |
| TableNextColumn | () → bool | |
| TableSetupColumn | (label [, flags [, width]]) → — | |
| TableSetupScrollFreeze | (cols, rows) → — | |
| TableHeadersRow | () → — | |
| TableHeadersRowClicked | () → column\|-1 | Draws the header row. Gives the column clicked this frame |
| TableGetHoveredColumn | () → column\|-1 | |
| TableGetSortSpecs | () → column, ascending \| nothing | Only when the sort changed this frame |

#### Tabs, tooltips, popups, menus

| Function | Args → returns | What |
|---|---|---|
| BeginTabBar | (id [, flags]) → bool | |
| EndTabBar | () → — | |
| BeginTabItem | (label [, flags]) → bool | |
| EndTabItem | () → — | |
| BeginTooltip | () → bool | |
| EndTooltip | () → — | |
| SetTooltip | (text) → — | |
| OpenPopup | (id) → — | |
| BeginPopup | (id) → bool | |
| BeginPopupContextItem | ([id]) → bool | |
| BeginPopupContextWindow | ([id]) → bool | Right click on the window off any item |
| EndPopup | () → — | |
| CloseCurrentPopup | () → — | |
| BeginMenu | (label [, enabled=true]) → bool | |
| EndMenu | () → — | |
| MenuItem | (label [, selected [, enabled=true [, shortcut]]]) → bool | |

#### Item and input state

| Function | Args → returns | What |
|---|---|---|
| IsItemHovered | ([flags]) → bool | |
| IsItemClicked | ([button=0]) → bool | |
| IsItemActive | () → bool | |
| IsItemFocused | () → bool | Gamepad nav cursor is on it |
| IsItemDeactivatedAfterEdit | () → bool | |
| GetItemID | () → int | |
| GetItemRect | () → x0, y0, x1, y1 | |
| IsMouseClicked / IsMouseDoubleClicked / IsMouseReleased / IsMouseDown / IsMouseDragging | ([button=0]) → bool | Five functions. Button 0..4 |
| IsMouseHoveringRect | (x0, y0, x1, y1) → bool | |
| GetMousePos | () → x, y | |
| IsKeyDown | (key) → bool | ui.Key value, or Ctrl/Shift/Alt/Super modifiers |
| IsKeyPressed | (key) → bool | Went down this frame (no repeat) |
| CaptureKeyboard | () → — | Keyboard goes to the UI next frame |
| GetIO | () → {width, height, fps, ctrl, shift, alt, wheel, mouseDX, mouseDY, uiScale, nav_active, nav_visible, typing} | One shared table per frame. Read-only |
| GetTime | () → seconds | |
| GetFrameCount | () → int | |

#### Drag and drop (string payloads)

| Function | Args → returns | What |
|---|---|---|
| BeginDragDropSource | ([flags]) → bool | |
| SetDragDropPayload | (type, data) → bool | |
| EndDragDropSource | () → — | |
| BeginDragDropTarget | () → bool | |
| AcceptDragDropPayload | (type) → string\|nil | |
| EndDragDropTarget | () → — | |

#### Style and fonts

| Function | Args → returns | What |
|---|---|---|
| PushStyleColor | (idx, r, g, b, a) → — | idx from ui.Col |
| PopStyleColor | ([n=1]) → — | |
| PushStyleColors | ({idx,r,g,b,a, …}) → n | Many at once |
| PushStyleVar | (idx, x [, y]) → — | idx from ui.StyleVar |
| PopStyleVar | ([n=1]) → — | |
| PushStyleVars | ({idx,x,y\|false, …}) → n | |
| PushFont | ([eqFont=3]) → — | EQ UI font 1..6 |
| PushChatFont | ([index=2]) → — | Chat font 0..10. Pop with PopFont |
| PopFont | () → — | |
| ChatFontSize | ([index=2]) → px | |

#### Drawing (window draw list, or foreground)

| Function | Args → returns | What |
|---|---|---|
| Foreground | (on) → — | Draw* goes above every window this frame |
| DrawRect | (x0, y0, x1, y1, r, g, b, a [, rounding [, thickness=1]]) → — | |
| DrawRectFilled | (x0, y0, x1, y1, r, g, b, a [, rounding]) → — | |
| DrawRectGradient | (x0, y0, x1, y1, r, g, b, a, r2, g2, b2, a2) → — | Top colour to bottom colour |
| DrawLine | (x0, y0, x1, y1, r, g, b, a [, thickness=1]) → — | |
| DrawCircle | (x, y, radius, r, g, b, a [, thickness=1]) → — | |
| DrawCircleFilled | (x, y, radius, r, g, b, a) → — | |
| DrawTriangleFilled | (x1, y1, x2, y2, x3, y3, r, g, b, a) → — | |
| DrawText | (x, y, r, g, b, a, text [, size]) → — | |
| DrawImage | (tex, x0, y0, x1, y1 [, u0, v0, u1, v1 [, r, g, b, a]]) → — | Screen coordinates |

#### Constant tables on `ui` (not functions)

`WindowFlags`, `Cond`, `TableFlags`, `Col`, `StyleVar`, `TableColumnFlags`, `InputTextFlags`, `ComboFlags`,
`SelectableFlags`, `Mouse` (Left/Right/Middle), `ButtonFlags`, `Key` (Escape, Enter, arrows, Space, A..Z, "0".."9"),
`TabItemFlags`, `TabBarFlags`, `HoveredFlags`. Each holds a subset of ImGui's enum: see `register_ui_api`.

### `eq`

The tables below include `eq.gamepad.*`. The **Cap** column is what a plugin's code needs to call it; `—` means
nothing beyond `ui`. A UI pack's own code may call everything. A refused call does nothing and gives `nil`, with one
line in the log. The list is `src/app/lua_eq_caps.cpp`; a function missing from it is taken to act and needs
`actions`. `self` means the function checks the caller itself, as each group's note says.

Conventions: `[x]` is optional and `a | b` is one or the other. `name, child` is a stock window's name and a child's
XML name (`"InventoryWindow", "IW_Money0"`). `slot` is a [possessions slot](#inventory-slots) and `bagSlot` a place
in the bag at that slot, from 0. `eq.FirstWornSlot` (0), `LastWornSlot` (22), `FirstBagSlot` (23), `LastBagSlot` (32)
and `CursorSlot` (33) are fields, not functions.

The **item table** that `eq.Item`, `SlotItem`, `BagSlotItem`, `BankItem` and others return is `{name, id, icon,
count, charges, stackSize, stackable, bagSlots, weight, magic, lore, type, size, ac, hp, mana, category, sellable}`,
plus `cooldown` (0..1 of the recast left) on the slot readers while a recast runs.

#### Session and client state (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| InGame | () → bool | A character is loaded | — |
| UiReady | () → bool | In game, or at character select / creation | — |
| GameState | () → int\|nil | 1 char select, 2 char create, 5 in game; nil before | — |
| Name | () → string\|nil | Character name | — |
| SlotName | (slot) → string | Possessions slot name ("Primary", …) | — |
| Money | () → pp, gp, sp, cp | Carried coin | — |
| Label | (eqType) → text [, r, g, b] | Text a game label with that EQType shows | — |
| Gauge | (eqType) → fraction\|nil, text, value [, r, g, b] | EQType gauge: fraction 0..1, raw value (0..1000 or special negatives), tint for special values | — |
| AltCurrencies | ([showMissing]) → {{name, count, icon, maxStack}} | Alternate currencies in the InventoryWindow list's order | — |
| UiTexture | (file) → tex, w, h \| nil | Texture from uifiles/default | — |
| UiAnimation | (name) → tex, u0, v0, u1, v1, w, h \| nil | First frame of a named installed XML animation, with its authored dimensions | — |
| Option | (name) → int\|nil | Client option value (ui/<pack>/options.lua) | — |
| SetOption | (name, int) → — | Change, apply and save a client option | commands |
| OptionChoices | (name) → {string} | Choices the client lists for an option (e.g. "resolution") | — |
| UiIni | (section, key) → string\|nil | Character UI ini value (over defaults.ini) | — |
| SetUiIni | (section, key, value) → — | Write and save a UI ini value | commands |
| CoolDownColor | () → r, g, b, a | Colour of cool-down rectangles (user colour 0x15f, alpha 0x40) | — |
| DebugScreens | () → {{name, shown, replaced}} | Every stock window model | — |
| DebugTree | () → {} | Stub: always an empty table | — |
| VoiceChat | () → false | Stub: no voice chat | — |

#### Window replacement and layout (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| ReplaceWindow | (name [, sizable]) → shown, x, y, w, h, sized, place | Call every frame from a replacing window. Marks the stock window replaced and gives its INI place. A plugin cannot replace "PluginConsent" | — |
| WindowShown | (name) → bool | ReplaceWindow's `shown` alone (also counts as replaced), with no place worked out | — |
| WindowPlaced | (name, x, y, w, h [, sizable]) → — | Report where the window is this frame, stored in the UI ini | — |
| ShowWindow | (name, show) → — | Show or hide a game window | commands |
| WindowTitle | (name) → string | Title the game set ("" = the XML's) | — |
| WindowClient | (window [, frameW, frameH]) → w, h \| nil | Client size of a window that sizes itself in code | — |
| SizeWindow | (window, w, h [, frameW, frameH]) → — | The user resized such a window | — |
| WindowContextMenu | (name) → taken | Right click on a window: its context menu at the mouse | — |
| TakeWindowMenu | (name) → bool | A context menu's "Window" row asked for this window's menu (once) | — |
| TakeCloseTopWindow | () → bool | Escape (close top window) went down this frame (once) | — |
| ContextMenus | () → bool | Right click may open context menus | — |

#### Stock window children, lists and controls (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| WindowButton | (name, child [, message=1]) → ok | Run the window's handler for that child (3 = buff right-click removes) | actions |
| WindowText | (name, child) → text, set | Text the game set on a child; `set` = window set it | — |
| SetWindowText | (name, child, text) → ok | Type into an edit box | actions |
| WindowList | (name, child) → {{col texts}} | A list's rows as column texts | — |
| ChildShown | (name, child) → bool\|nil | Whether the game shows the child (nil = as XML) | — |
| ChildArt | (name, child, part) → tex,u0,v0,u1,v1 \| false \| animName \| nil | What the child shows now. part "background", "decal" or "spellicon" | — |
| ChildState | (name, child) → enabled, checked, value, count, action \| nil | Button/control state | — |
| ChildGauge | (name, child) → fraction\|nil, text [, r, g, b] | A gauge the window fills itself | — |
| ChildColor | (name, child [, "hover"\|"decal"]) → r, g, b [, a] \| nil | Colour the window set on the child | — |
| ChildTooltip | (name, child) → string | Tooltip the window set ("" none) | — |
| ChildUnder | (name, child) → bool | The window draws this child itself, under the others | — |
| ChildRect | (window, child) → x, y, w, h, relative [, right, bottom] \| nil | Where the window's code placed the child | — |
| ClickChild | (name, child [, button [, outside]]) → ok | Click: "left", "right", "rightdown", "rightheld", "rightup" or "rightupheld" | actions |
| ClickChildAt | (name, child, clientX, clientY [, button]) → ok | As ClickChild, at a client point | actions |
| StmlLink | (name, child, message, data) → ok | An STML `<a WndNotify>` link was clicked (-1 = href) | actions |
| TabPage | (name, child) → index\|nil | Tab box's page (0-based) | — |
| SetTabPage | (name, child, index) → — | Switch the tab page | actions |
| SetSlider | (window, child, value) → ok | Drag the slider to a value | actions |
| ComboInfo | (name, child) → {choices}, selected \| nil | Combo choices and the chosen one (0-based) | — |
| ComboSelect | (name, child, index) → ok | Pick a combo choice (0-based) | actions |
| ListInfo | (name, child) → rows, columns, selected, labels, widths \| nil | List shape | — |
| ListRow | (name, child, row) → texts, r, g, b [, tex,u0,v0,u1,v1] | Row (0-based). texts.altRight / .comboColumn / .iconColumn / .colors / .disabled (a line the window left disabled) | — |
| ListClick | (name, child, row [, "left"\|"right"\|"double" [, column [, mods]]]) → ok | Click a list row. mods: 1 shift, 2 ctrl | actions |
| ListHeaderClick | (name, child, column) → ok | Click a column header (0-based) | actions |

#### Active tasks and quest journal (lua_eq_progress.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| Tasks | () → tasks | all authoritative active tasks, independent of journal visibility or selection | — |
| OpenTask | (taskId) → bool | selects and opens an existing active task in the journal; false for an unknown ID | `commands` |

`Tasks` returns an array in native task/shared/quest and slot order, plus `selectedId` (0 if none), `revision`
and `requestTimerLeft` (seconds). Each task has `id`, `type` (0 task, 1 shared, 2 quest), `sequence`, `title`,
`duration`, `left` (seconds; 0 when untimed), `description`, `rewards`, `details` (the combined native STML),
`hasRewards`, `rewardSet`, `done`, `total` and `objectives`. It is empty outside the world.

Objectives preserve native group/slot order. A separator is `{separator=true, group}`. A normal entry has
`activityId`, `group`, `separator=false`, `hidden`, `optional`, `done`, `currentZone`, `text`, `status`, `zone`,
`count` and `goal`. Hidden entries have no `count` or `goal`; their undisclosed names and locations are not
available. `currentZone` is false for hidden objectives and those that count anywhere. `group` is the server's
list-group value, not a parent-objective ID or an invented numbered stage. Reading tasks does not select one or
send a request. `OpenTask` changes the journal selection and visibility, not task or tracking state.

#### Spell gems, buffs, cool-downs (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| GemTint | (name, child) → r, g, b, a | Spell gem tint by category | — |
| GemOverlay | (name, child, y0, y1) → top, r, g, b, a \| nil | Gem cool-down rectangle | — |
| GemInfo | (name, child [, into [, y0, y1]]) → t | All gem looks in one call: {shown, tooltip, tex,u0,v0,u1,v1, tintR..tintA, overlay, coolR..coolA, top} | — |
| ButtonCoolDown | (name, child, y0, y1) → top, r, g, b, a, remainingMs, reuseMs | A button's cool-down rectangle; first five answers unchanged (nil when idle), with remaining/focus-adjusted reuse milliseconds for combat ability buttons (both 0 while idle or without timing detail). Shared remaining time can exceed this ability's reuse | — |
| BuffSlot | (name, child [, overlays]) → spellId, msLeft, name, beneficial, fade [, counter,cr,cg,cb, limit,lr,lg,lb] \| nil | Buff on a buff button. msLeft < 0 = permanent | — |
| BuffOverlay | (name, child, "timer"\|"counter"\|"limit") → text, r, g, b \| nil | Overlay text on a buff button | — |
| SpellGemClick | (child [, "left"\|"right"]) → ok | Cast, or forget (right) | actions |

#### Hot buttons and the cursor (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| HotButton | (name, child) → kind, label [, tex,u0,v0,u1,v1] | kind "spell", "item", "button" or "empty" | — |
| HotButtonFace | (name, child) → {color={r,g,b}, lua, running, enabled, cooldown, overlay, tooltip} \| nil | A social's face extras | — |
| HotButtonChecked | (name, child) → bool | Drawn pressed (a pet command that is on) | — |
| HotButtonState | (name, child [, y0, y1]) → {checked, left, cooldown, top, color, tint} \| nil | Check and cool-down state | — |
| HotButtonInfo | (name, child [, into [, y0, y1]]) → t | HotButton + Face + State in one table (kind, label, tex…, social, color, lua, running, enabled, faceCooldown, overlay, tooltip, state, checked, left, cooldown, top, tint, iconAlpha) | — |
| ClickHotButton | (name, child [, "left"\|"right"\|"rightheld" [, x, y]]) → ok | Use the button, or open its menu (at x, y) | actions |
| PickUp | (window, child [, row]) → ok | Drag from a child: its hotbutton goes on the cursor | actions |
| PressButtonFrom | (window, child [, row]) → ok | Run the hotbutton a drag would make (a pad's "use"). Not for hot bars | actions |
| HoldingButton | () → bool | A hotbutton is on the cursor | — |
| ClearCursorButton | () → — | Drop the cursor's hotbutton | actions |
| CursorAttachment | () → {icon, iconRect, rect, quantity, font, spell, coin, …} \| nil | What the mouse carries. Images are {texture,u0,v0,u1,v1}, rects are {x,y,w,h}. Money on the cursor has `coin` (0 pp .. 3 cp) and its count in `quantity`, and no `icon` | — |

#### Items, bags, item jobs, item preview (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| Item | (slot [, bagSlot]) → item\|nil | Item table for a possessions slot | — |
| ItemIcon | (icon) → tex,u0,v0,u1,v1 \| nil | Item icon cell | — |
| ClickSlot | (slot, bagSlot\|nil, "left"\|"leftheld"\|"right"\|phase [, outside]) → ok | Game slot logic: pick up, drop, swap, split, use | actions |
| HasSlot | (slot, bagSlot\|nil) → bool | ClickSlot can act there now: in game, a possessions slot 0..33, and with bagSlot a place inside the bag that general slot holds (empty places count) | — |
| SlotItem | (name, child) → item\|nil | Item in a window's item slot (loot, bank, trade…; `MerchantWnd` `ItemList:<row>` a merchant's row, `ItemDisplayWindow` `Hailstone_Item` the shown item) | — |
| BagInfo | (bagWindow) → name, slots, combine, where, slot \| nil | "Bag1".."Bag36" or "WorldContainer". where: "inventory", "bank" or "other" | — |
| BagSlotItem | (bagWindow, index) → item\|nil | Item in a bag slot (0-based) | — |
| BagSlotClick | (bagWindow, index [, button [, outside]]) → ok | Slot logic on a bag slot | actions |
| PlaceClick | (place, "left"\|"open") → ok | Slot logic at a place string "i:29:3" / "b:0:2" | actions |
| FindItems | (where, text) → count, slots, {{item, place, bag}} | Search "inventory", "bank" or "all" by name | — |
| InventorySpace | () → free, total | Empty places in general slots and bags | — |
| SortItems | (where, mode [, {locked, filters}]) → err\|nil | "sort", "stack" or "tidy" an inventory, bank or bag | actions |
| SellJunk | ({itemId…}) → err\|nil | Sell those ids to the open merchant | actions |
| ItemJob | () → done, total, mode, where \| nil | The running item job | — |
| StopItemJob | () → — | Stop it | actions |
| ItemPreview | () → {open, show, name, onPlayer, serial} \| nil | Object preview window state | — |
| ItemPreviewImage | (w, h [, "marketplace"]) → tex\|nil | The preview's 3D view for this frame | — |
| ItemPreviewControl | (what [, x [, y [, view]]]) → — | "rotate", "pan", "zoom" or "reset" the preview camera | — |

#### Parcels (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| Parcels | () → parcels, claiming, busy, sending, picked, money \| nil | Parcels at the open parcel merchant: {{slot, item, from, note, sent}}. money {coin, amount, serial} | — |
| RetrieveParcel | (slot) → — | Take one parcel | actions |
| ClaimAllParcels | () → — | Take them all in turn | actions |
| SendParcel | (to, note, count [, place]) → err\|nil | Send the picked item (or the item at `place`) | actions |
| SendParcelMoney | (to, note, pp, gp, sp, cp) → err\|nil | Send money | actions |

#### Menus (lua_eq.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| CascadeMenus | () → {{x,y,w,h, items={{kind,text,rect,icon,decalW,decalH,textX,enabled,arrow,arrowRect,background}}}} | Open EQ-button menu and submenus | — |
| CascadeClick | (menu, item) → — | Click an item (1-based) | actions |
| CascadeHover | (menu, item) → — | Hover an item | — |
| CascadeClose | () → — | Close the cascade | — |
| GameMenus | ([scale]) → {{x,y,w,h, rows={{text,r,g,b,check,enabled,separator,submenu}}}} | Open context menus | — |
| GameMenuClick | (menu, row) → — | Menu 1-based, row 0-based | actions |
| GameMenuHover | (menu, row) → — | Hover a row | — |
| GameMenuClose | () → — | Close the menus | — |

#### Compass (lua_eq_misc.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| CompassStrips | () → left1, left2, w, degrees \| nil | Compass strip offsets and the shown heading | — |
| CompassMarkers | () → {x…} | Window x of the client's compass points | — |

#### Bank and trade (lua_eq_exchange.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| BankItem | (slot [, bagSlot [, shared]]) → item\|nil | Bank (or shared bank) slot item | — |
| BankSlotClick | (slot, bagSlot\|nil, button [, shared]) → ok | Slot logic in the open bank ("left", "rightup", "rightheld") | actions |
| CursorCoin | () → button, amount \| nil | Coin on the cursor (0 pp .. 3 cp) | — |
| TradeState | () → {npc, partner, requested, open, myReady, theirReady} \| nil | Open trade or give window | — |

#### Inventory manager (lua_eq_inventory_manager.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| AllItems | () → {{item, place, where, slot, sub, bag, kept, stale}} | Everything worn, carried, banked and shared. item has extra fields (value, noTrade, temporary, attuned, reqLevel, recLevel, equipSlots, tradeskill, augment, usable, tribute, noDestroy, stats). stale: a banked row from the bank as last seen (eq.BankSeen), not in reach | — |
| ItemsRevision | () → inv, keep | Change counters | — |
| BankSeen | () → live, age | live: the bank's rows are the bank as the server has it now; else they are the copy as last seen at a bank, age seconds ago (nil: never seen) | — |
| ItemKept | (id) → bool | Item is marked Keep | — |
| SetItemKept | (id, name, on) → — | Mark or unmark every copy as Keep | actions |
| SetItemsKept | ({{id, name}…}, on) → — | SetItemKept for many items at once, with one write (up to 4096) | actions |
| ItemStats | () → {{key, label}} (array pairs) | Stats the window filters and sorts by | — |
| BulkItems | (mode, {place…} [, dry]) → count, err\|nil | Bulk "sell", "destroy", "tribute" or "withdraw" (bank to your packs, never equipped) of the chosen items; Keep-marked items are refused | actions |
| TakeItemSearch | () → text\|nil | /finditem's search, once | — |

#### Abilities (lua_eq_abilities.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| Disciplines | () → {{index, spell, name, icon, level, endurance, mana, reuse, remaining, timer}} | Combat abilities list | — |
| UsableSkills | () → {{id, name, combat, remaining}} | Skills the Actions window takes | — |
| UseSkill | (id) → ok | Use a skill as its hot button does | actions |

#### Ability book (lua_eq_abilitybook.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| AbilityBook | () → {{key, kind, id, name, icon, spell, classes, level, rank, maxRank, endurance, mana, timerKind, timer, reuse, remaining, ready, description, combat, autoable, beneficial, buff, buffOn, favorite}} | Every usable ability (AA, disc, skill) | — |
| AbilityBookState | () → {classes={{id,name}}, autoskill={on, allowed, held, list={{key, endurance, mana, targetHp, targetHpAbove, selfHp, when, keepBuff}}}} | Book and autoskill state | — |
| AbilityBookUse | (key) → ok | Use the ability | actions |
| AbilityBookFavorite | (key, on) → — | Toggle favourite | commands |
| AbilityBookAutoskill | (key, on) → — | Add to or remove from the autoskill list | commands |
| AbilityBookAutoskillMove | (key, delta) → — | Move in the list (-1 / +1) | commands |
| AbilityBookAutoskillRule | (key, endurance, mana, targetHp, targetHpAbove, selfHp, when, keepBuff) → — | Set an entry's rule | commands |
| AbilityBookAutoskillEnable | (on) → — | Autoskill master switch | actions |

#### Achievements and rewards (lua_eq_achievements.cpp, lua_eq_rewards.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| Achievements | ([catalogToken, stateRevision]) → table\|nil | Received catalog, own state, totals and comparison status; matching tokens omit unchanged tables | — |
| Achievement | () → table\|nil | Current own or linked detail, including its revision | — |
| AchievementOpen | (catalogToken, definitionIndex) → accepted, reason | Select one of your achievements by its received zero-based index | commands |
| AchievementRefresh | () → accepted, reason | Explicitly request a fresh catalog and state | actions |
| AchievementCompare | () → accepted, reason | Compare the nearby targeted player | actions |
| AchievementClearCompare | () → accepted, reason | Clear the comparison; retire a reply still in flight | commands |
| AchievementReward | (catalogToken, definitionIndex) → accepted, reason | Open the selected achievement's server-provided rewards | actions |
| AchievementLink | (catalogToken, definitionIndex) → accepted, reason | Insert the selected achievement link into chat composition | chat |
| RewardSelection | ([pendingRevision, previewRevision]) → table\|nil | Separate pending and preview lanes; matching revisions omit unchanged tabs | — |
| RewardSelectionClaim | (revision, tabToken, wireOption) → accepted, reason | Confirm a received pending choice; previews cannot claim | actions |
| RewardSelectionInspect | (lane, revision, tabToken, wireOption, entryId, itemId) → accepted, reason | Explicitly request a received reward item's details | actions |
| RestorePendingRewards | () → accepted, reason | Request pending rewards without retrying a claim | actions |
| RewardSelectionClose | (lane) → accepted, reason | Close the pending or preview lane | commands |

Catalog, state, detail, lane-revision and tab tokens are opaque decimal strings: pass them back unchanged, never
through `tonumber`. Definition indices and wire ids are unsigned 32-bit integers. `lane` is `"pending"` or
`"preview"`. Getters and drawing are passive; an accepted action means it was admitted to transport, not that the
server granted a reward. The full table shapes and cache rules are in [docs/dev-ui.md](dev-ui.md), under the
achievement and reward-selection APIs.

#### Spell book and spell sets (lua_eq_spellbook.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| SpellBook | () → {{slot, id, name, icon, level, mana, endurance, castTime, recastTime, recoveryTime, range, aeRange, duration, beneficial, targetType, skill, skillName, category, categoryName, subcategory, subcategoryName, resist, gem}} | Every scribed spell | — |
| SpellBookState | () → {open, sitting, gems, usableGems, memorizing={gem,spell}, settingSet, scribing, cursorSpell, holdingItem, swapSlot} | Book state | — |
| SpellIcon | (icon) → tex,u0,v0,u1,v1 \| nil | Spell icon cell | — |
| SpellBookClick | (slot) → ok | Stock book click: scribe, or spell to cursor | actions |
| SpellBookSwap | (slot) → — | Quick right click: pick up or swap book slots | actions |
| SpellBookMemorize | (slot, gem) → — | Memorize into an empty gem (0-based) | actions |
| ShowSpell | (id [, temporary]) → — | Open the spell display window | commands |
| SpellDetails | (id) → title, stml \| nil | Spell display text | — |
| DisplaySpellId | (n) → spellId\|nil | Spell id of the nth open spell display (1-based) | — |
| DisplayItemData | (n) → {id, icon, name, lore, value, stackCount, damage, delay, itemType, favor} \| nil | nth open item display's data (`stack_count` and `item_type` are kept too) | — |
| SpellSets | () → {{name, inUse, spells={id\|-1 ×12}} ×10} | Spell sets | — |
| SaveSpellSet | (name [, spells]) → n\|nil | Save the gems (or the given spells) as a set | commands |
| EditSpellSet | (n, name, spells) → n\|nil | Rename a set and replace its spells | commands |
| MemorizeSpellSet | (n) → ok | Memorize set n (1-based) | actions |
| DeleteSpellSet | (n) → — | Delete set n | commands |

#### Character stats and /who (lua_eq_stats.cpp, lua_eq_social.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| CharacterStats | () → {hpRegen, manaRegen, enduranceRegen, hpRegenBase, manaRegenBase, enduranceRegenBase, maxHp, maxMana, maxEndurance, maxHpBase, maxManaBase, maxEnduranceBase, ac, defense, weight, maxAir, maxLevel} \| nil | Computed stats that no label shows | — |
| WhoResults | () → count, {{name, level, class, race, raceName, guild, zone, anonymous, roleplay, afk, linkdead, trader, buyer, offline, line}} | Last /who reply | — |

#### Chat windows (lua_eq_chat.cpp)

`slot` is a chat window, 1..32.

| Function | Args → returns | What | Cap |
|---|---|---|---|
| ChatWindows | () → {{slot, title, main, tells, tab, select, tellsIn, tabs={{title, unread, tell}}}} | Shown chat windows | — |
| ChatText | (slot [, version [, names [, stamps [, incremental]]]]) → version [, lines [, reset, count]] | STML lines of the tab that is up. The incremental form gives only the new lines | — |
| ChatInfo | (slot) → font, active, default, scrollbar, here | Tab font, active, default channel, scrollbar, Always Chat Here | — |
| ChatSubmit | (slot, text) → — | Send a typed line (goes through the command filter, so the caller's caps apply) | — |
| ChatTakeFocus | (slot [, typed]) → text, replace \| nil | Text for the input when a chat key fires (Link All too) | — |
| ChatHistory | (slot, older) → text\|nil | Step through lines sent before | — |
| ChatCycleReply | (text, back) → text\|nil | Tab / Shift+Tab reply cycling | — |
| ChatPage | (slot) → -1\|1\|nil | PageUp / PageDown for this window, once | — |
| ChatLink | (slot, message, data) → — | A link clicked in the output (27 item, 1001 name) | commands |
| ChatActivate | (slot) → — | Make it the active window | commands |
| ChatMenu | (slot) → — | The window's context menu at the mouse | — |
| ChatSetAlwaysHere | (slot, on) → — | Always Chat Here | commands |
| ChatNewWindow | ([title]) → slot\|nil | New chat window | commands |
| ChatCloseWindow | (slot) → — | Close a window and its tabs | commands |
| ChatNewTab | (slot [, title]) → tab\|nil | New tab in this window | commands |
| ChatCloseTab | (slot, tab) → — | Close a tab | commands |
| ChatSelectTab | (slot, tab) → — | Bring a tab up | commands |
| ChatSetTitle | (slot, title) → — | Rename the tab | commands |
| ChatFilters | (slot) → {{id, group, name, on, menu, parent}} | Route rows for the tab; menu and parent are stable group IDs | — |
| ChatFilterGroups | () → {{id, parent, name}} | Filter menu hierarchy; parent 0 is the root | — |
| ChatSetFilter | (slot, id\|groupName, on) → — | Route a channel here or back to main | commands |
| ChatSetFont | (slot, font) → — | Tab font index | commands |
| ChatDefaultChannels | () → {names} | Channel menu items (0-based order) | — |
| ChatSetDefault | (slot, index) → — | Default channel for the tab | commands |
| ChatClear | (slot) → — | Empty the tab | commands |
| ChatTellWindows | () → bool | Tells open a window per player | — |
| ChatSetTellWindows | (on) → — | Set that | commands |
| ChatHitModes | () → {{name, mode, modes={names}}} | Melee report categories | — |
| ChatSetHitMode | (index, mode) → — | 1-based index (7 = ALL) | commands |

#### Hot bars and icon selection (lua_eq_hotbar.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| IconSelectionIcons | () → first, last, selected | Icon selection window's page | — |
| IconSelectionIcon | (n) → tex,u0,v0,u1,v1 \| nothing | Icon n of the window's type | — |
| HotBarSetting | (bar, name) → value | Bar 1..10: "PercentSize", "ShowKeyMap" or "ShowSpinner" | commands |
| HotBarMenu | (bar [, x, y]) → ok | The bar's window menu | — |

#### Social editor (lua_eq_socialedit.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| OpenSocialEditor | (index) → — | Open the social editor on social 0..119 | commands |
| SocialEditorSocial | () → n\|nil | Social the editor is open on | — |
| SocialLine | (text) → {pause, timers, command, kind, name} | How a press runs a line | — |
| SlashCommands | (prefix) → {"/cmd"…} | Game commands that start with the prefix | — |
| PressSocial | (n) → ok | Press social n | actions |
| CurrentSocial | () → n\|nil | Social a press is running | — |
| StopSocial | () → — | /stopsocial | actions |

#### Lua socials (lua_eq_luasocial.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| LuaSocialRules | () → {mode, caps, press_commands, faces} | Server's rules for Lua socials | — |
| LuaSocial | (n) → {name, color, lines, lua, press, face, running, hash, rules} \| nil | A social | — |
| SetLuaSocial | (n, {name, color, press, face [, lines]}) → ok | Save a social | commands+actions |
| LuaSocialFace | (n) → {label, color, cooldown, overlay, enabled, tooltip, error} \| nil | Face of a Lua social | — |
| LuaSocialDryRun | (n [, t [, mods]]) → {ok, lines, error, mode} | Commands a press would send | commands+actions |
| PressLuaSocial | (n) → ok | Press it | actions |
| StopLuaSocial | (n) → ok | Stop it | commands |
| LuaSocialExport | (n [, t]) → text | Share text | — |
| LuaSocialImport | (text) → t \| nil, why | Parse share text | — |
| LuaComplete | (before) → start, {names} | Editor completion | — |
| LuaSocialEditRequest | () → n\|nil | A social the editor was asked to open | — |
| OpenLuaSocialEditor | (n) → — | Open the Lua social editor | commands |
| FreeSocial | () → n\|nil | First empty social | — |
| SocialToCursor | (n) → ok | Social as a hot button on the cursor | actions |
| SocialColors | () → {{r,g,b} ×20} | Social colours | — |

#### Map, atlas and zones (lua_eq_map.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| DrawMap | (x0, y0, x1, y1 [, hovered [, scale]]) → — | Draw the stock map into a rectangle | — |
| MapZoom | (steps) → — | Mouse wheel zoom | — |
| MapMouseDown | () → — | Left button down on the map (pan, edit, lasso) | — |
| MapMouseUp | () → — | Left button up | — |
| MapPanReset | () → — | Re-centre | — |
| MapRightClick | (x, y) → — | Right click: find path to that point | commands |
| DrawMinimap | (cx, cy, radius, range [, rotate]) → ndx, ndy [, px, py, scale] | Circular minimap | — |
| DrawZoneMap | (x0, y0, x1, y1, zoom [, cx, cy [, labels]]) → scale, cx, cy [, px, py] | North-up zone map | — |
| DrawAtlas | (x0, y0, x1, y1 [, scale]) → — | Atlas page with links | — |
| FindMapLocation | (x, y) → — | Find path to a map point | commands |
| FindPathShown | () → bool | A find path is shown | — |
| EndFind | () → — | End the find path | commands |
| ZoneName | () → long, short | Current zone | — |
| GameTime | () → hour, minute \| nothing | Game clock | — |
| MapAutoZ | ([on [, above [, below]]]) → on, above, below | Auto Z setting (get or set) | commands |
| AtlasIndex | () → {pages={{file,title,parent}}, zones={{zone,name,page,x0,y0,x1,y1,id,expansion,allowed}}} | Atlas index | — |
| AtlasPageData | (file) → {width, height, background, bgw, bgh, images, labels, links, errors} \| nil | One atlas page | — |
| AtlasTexture | (file) → tex, w, h \| nil | Atlas picture | — |
| ZoneLinks | () → server, zones, limit [, expansion] | Zone connectivity known (asks the server once) | — |
| ZoneRoute | (to [, from]) → {{id, zone, name, expansion}…, assumed} \| nil | Fewest-zone route | — |
| ZoneExits | () → {{id, zone, name, expansion, x, y, x0, y0, x1, y1}} | Current zone's zone lines | — |
| ZoneExitWalk | (zone) → x, y \| nil | /nav walk through the nearest line to `zone` | move |
| ShowZoneMap | ([zone]) → — | Map window shows another zone (or own) | — |
| MapZoneShown | () → long, short, own | Zone the map shows | — |

#### Item comparison (lua_eq_compare.cpp)

The item against what the player wears (docs/item-compare.md). `c` = {usable, whyNot, notes, slots = {{slot, slotName, current, verdict, score, changes = {{key, name, before, after, delta, weight}}, weaponBefore, weaponAfter, gained, lost}}}, the best swap first; nil for no comparison.

| Function | Args → returns | What | Cap |
|---|---|---|---|
| CompareItem | (slot [, bagSlot]) → c\|nil | An item carried (nil for a worn one) | — |
| CompareSlotItem | (window, child) → c\|nil | The item in a window's item slot (as SlotItem) | — |
| CompareBagSlotItem | (bagWindow, index) → c\|nil | The item in a bag window's slot | — |

#### Find window (lua_eq_find.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| FindNpcs | ([search]) → {{id, name, subtitle, category, label, x, y, z, distance, quests}} | Useful NPCs, nearest first | — |
| FindCategories | () → {{key, label}} | Categories in order | — |
| FindNpcTarget | (id) → ok | Target the NPC | target |
| FindNpcPath | (id) → ok | Guide path to it | commands |
| FindNpcWalk | (id) → ok, why | Walk there (/nav) | move |
| FindOpen | ([open]) → bool | Find window state (get or set) | commands |

#### Gamepad: `eq.gamepad.*` (lua_eq_gamepad.cpp)

| Function | Args → returns | What | Cap |
|---|---|---|---|
| gamepad.connected | () → bool, name | Pad connected | — |
| gamepad.type | () → "xbox"\|"playstation"\|"nintendo"\|"generic" | Pad family | — |
| gamepad.down | (button) → bool | Held | — |
| gamepad.pressed | (button) → bool | Went down this frame | — |
| gamepad.released | (button) → bool | Went up this frame | — |
| gamepad.axis | (name) → number | Stick -1..1, trigger 0..1 | — |
| gamepad.mode | () → "game"\|"menu"\|"cursor" | Current mode | — |
| gamepad.set_mode | (mode) → mode | Set the mode | commands |
| gamepad.cross_set | () → 0..3 | None, LT, RT, LT+RT | — |
| gamepad.cross_slot | (set, slot) → bar, button \| nil | Cross slot binding | — |
| gamepad.set_cross_slot | (set, slot, bar, button) → ok | Rebind a cross slot | commands |
| gamepad.glyph | (button) → string | Pad's label for a button | — |
| gamepad.binding | (input) → action\|""\|nil | Action bound to an input | — |
| gamepad.set_binding | (input [, action]) → ok | Bind (nil or "" = default) | commands |
| gamepad.setting | (name) → number\|nil | gamepad.* setting | — |
| gamepad.set_setting | (name, value) → ok | Set and save to hailstone.ini | commands |
| gamepad.rumble | (low, high, ms) → ok | Rumble | — |
| gamepad.request_focus | (window) → — | Menu mode, ImGui focus on that window | — |
| gamepad.drop_cursor | () → ok | Drop the cursor item on the ground | actions |
| gamepad.takes_b | () → — | A window closes itself on B this frame | — |

#### Plugins (lua_eq_plugins.cpp)

`self`: from a plugin's code these reach only that plugin's own entries. PluginConsent and AnswerPluginConsent are
refused to plugin code (nil / false).

| Function | Args → returns | What | Cap |
|---|---|---|---|
| Plugin | (name) → p\|nil | Plugin object: p.name, p.owner, p.f() calls, p.call('f') | self |
| PluginOptions | () → {{plugin, owner, name, label, min, max, value}} | Plugin options | self |
| SetPluginOption | (plugin, name, value) → — | Set a plugin option | self |
| PluginMenu | () → {{plugin, owner, label, command}} | Plugin menu entries | self |
| PluginMenuClick | (i) → — | Run entry i's command as its owner | self |
| PluginConsent | () → {server, serial, warning, plugins={{name, file, size, sha256, short}}} \| nil | Pending consent question for the server's compiled plugins | self |
| AnswerPluginConsent | (true\|false\|nil) → ok | Answer it (nil = ask later) | self |

#### Scripts (lua_eq_scripts.cpp)

`self`: Run, Stop, Pause and Autorun are refused to plugin code.

| Function | Args → returns | What | Cap |
|---|---|---|---|
| Scripts | ([rescan]) → {{name, status, error, found, allowed, why, server, kept, autorun, caps}}, {folders, autorun} | Scripts window list | self |
| ScriptRun | (name [, args]) → ok [, why] | Run a script | self |
| ScriptStop | (name) → ok | Stop | self |
| ScriptPause | (name) → ok | Pause | self |
| SetScriptAutorun | (name, on) → ok | Character autorun | self |

#### In-game browser (lua_eq_web.cpp)

`self`: plugin code needs the 'web' permission and reaches its own tabs only. WebOpenExternal works only from the
player's own windows.

| Function | Args → returns | What | Cap |
|---|---|---|---|
| WebAvailable | () → bool | Browser helper is usable | self |
| WebOpen | (url [, w, h]) → tab \| nil, why | New tab | self |
| WebTabs | () → {tab} | Caller's tabs | self |
| WebState | (tab) → {url, title, loading, canBack, canForward, closed, why} \| nil | Tab state | self |
| WebNavigate | (tab, url) → true \| false, why | Navigate | self |
| WebNav | (tab, "back"\|"forward"\|"reload"\|"stop") → — | Navigation | self |
| WebClose | (tab) → — | Close | self |
| WebView | (tab, w, h) → hovered | Draw the page, give it the input | self |
| WebText | (tab, text) → — | Type text into the page | self |
| WebEval | (tab, script) → ok | Run JS | self |
| WebPost | (tab, message) → ok | Call window.hailstone.onmessage | self |
| WebMessages | (tab) → {strings} | Page posts since the last call (plugin tabs only) | self |
| WebOpenExternal | (url) → ok | Open in the system browser (http(s)) | self |

#### Pre-game: login, server and character select, creation (lua_eq_pregame.cpp)

`self`: these give nil / false to plugin windows (`plugin_window()`). The views take a `[revision]` argument and
return `true` while nothing changed. Reading a view draws that screen in the client's place.

| Function | Args → returns | What | Cap |
|---|---|---|---|
| LoginScreen | ([revision]) → view\|true\|nil | Login view: revision, busy, status, servers, accounts, characters, gameFolders, addons{…} and more (the comment in src/app/lua_eq_pregame.cpp lists them) | self |
| LoginSet | (field, value) → — | Edit a login field ("name", "host", "port", "username", "password", "ui", "remember", …) | self |
| LoginDo | (action [, arg [, value]]) → ok | Login buttons: "login", "quit", "play", "select", "save", "delete", update* and addon* actions, … | self |
| ServerSelect | ([revision]) → {revision, busy, status, server, servers={{name, status, canJoin, favorite}}, selected, last, addonHint}\|true\|nil | Server select view | self |
| ServerSelectDo | (action [, i]) → ok | "select", "play", "favorite" or "back" | self |
| CharacterSelect | ([revision]) → {revision, busy, status, world, server, scene, selected, characters={{name, level, race, raceName, classes, className, classShort, zone, canGoHome, homeQuestion}}}\|true\|nil | Character select view | self |
| CharacterSelectDo | (action [, arg]) → ok | "select", "enter", "create", "delete", "home", "back" or "logout" | self |
| CharacterCreate | () → view\|nil | Creation view: name, gender, race/class/deity/city (+Name, +Index, lists), stats, pointsLeft, ready… | self |
| CharacterCreateHelp | (kind, id) → text, title | Description of a race, class, deity, city or stat | self |
| CharacterCreateDo | (action [, value]) → ok | "name", "randomName", "race", "class", "gender", "deity", "city", "statUp/Down/Max", "resetStats", "appearance", "create" or "cancel" | self |

## Compiled plugins: the C SDK

A plugin is a `.dll`, `.so` or `.dylib` built against `sdk/hailstone_plugin.h` (plain C, MIT licensed). docs/plugins.md
"Compiled plugins" has a full example and the build commands, and `plugins/example/MQ2Example.c` uses every part.
`sdk/hailstone_byte_io.h` (C++20, header only) gives the `ByteReader` and `ByteWriter` the client uses for packets.

```c
#include "hailstone_plugin.h"
HAILSTONE_PLUGIN   /* the two required exports and the `host` global */

static void hello(void* user, const char* args) { WriteChatf("Hello, %s!", *args ? args : "world"); }
HAILSTONE_API void InitializePlugin(void) { AddCommand("/hello", hello, 0); }
HAILSTONE_API void ShutdownPlugin(void) { RemoveCommand("/hello"); }
```

### Exports

All are optional except the two `HAILSTONE_PLUGIN` defines. Declare each with `HAILSTONE_API` so a C++ plugin
exports its C name.

| Export | Needs | When |
|---|---|---|
| `uint32_t hailstone_plugin_abi(void)` | | returns `HAILSTONE_PLUGIN_ABI` (3) |
| `void hailstone_plugin_set_host(const PluginHostApi*)` | | first, with the host table |
| `void InitializePlugin(void)`, `void ShutdownPlugin(void)` | | at load and unload |
| `void OnPulse(void)` | | every frame |
| `void SetGameState(int state)` | | the game state changed (`GAMESTATE_*`) |
| `void OnBeginZone(void)`, `void OnEndZone(void)`, `void OnZoned(void)` | | zoning |
| `void OnAddSpawn(uint32_t id)`, `void OnRemoveSpawn(uint32_t id)` | `read` | |
| `int OnIncomingChat(const char* line, uint32_t color)` | `events` | a chat line from the server or the client (not one a script or plugin wrote); the result is ignored |
| `int OnFilterChat(const char* line, uint32_t color)` | `events` | nonzero hides the line |
| `void OnUpdateImGui(void)` | `ui` | draw with Dear ImGui |
| `int OnPacket(int stream, int direction, HailstonePacket* p)` | `packets` | each packet in either direction; change it in place, nonzero drops it |
| `const HailstoneHooks* hailstone_plugin_hooks(void)` | `extend` | read once after `InitializePlugin` |

### Helpers

Defined in the header over the global `host`, with MacroQuest's names:

| Helper | What |
|---|---|
| `WriteChatf(fmt, ...)` | a chat line |
| `DoCommand(line)`, `DoCommandf(fmt, ...)` | runs a command line, held to the plugin's capabilities |
| `AddCommand(name, fn, user)`, `RemoveCommand(name)` | a command of the plugin's (`commands`); 0 when the name is one of the client's own |
| `ParseMacroData(text, out, size)` | `${...}` expanded; returns the full length, -1 without `read` |
| `AddTopLevelObject(name, fn, user)`, `RemoveTopLevelObject(name)` | a TLO of the plugin's; 0 when the name is already a TLO |

A plugin TLO's function is `int fn(void* user, const char* index, const char* member, const char* member_index,
HailstoneValue* out)`. It is called with `member` NULL for the TLO's own value (`${Name[index]}`) and with the member
for `${Name[index].Member[member_index]}`. It returns nonzero when it set `out`. Members of the returned value are
the [pure types'](#pure-types) members, so a plugin TLO is one level deep.

### The host table

Every function takes `host->host` first: `host->get_player(host->host, &p)`. Calls the plugin may not make return 0
or -1 and say so in the chat window. Only call these on the client's thread, from inside a callback.

| Function | Needs | What |
|---|---|---|
| `caps()`, `name()` | | the plugin's capabilities and name |
| `log(level, text)` | | the client's log, level 0 debug to 3 error |
| `write_chat(text, rgb)` | | a chat line in an RGB colour (0 the default) |
| `write_chat_color(text, color)` | | a chat line in one of the game's chat colours |
| `do_command(line)` | per command | |
| `add_command`, `remove_command` | `commands` | |
| `parse(text, out, size)` | `read` | |
| `add_tlo`, `remove_tlo` | | |
| `send_packet(stream, opcode, data, size)` | `packets` | to the server, through the other plugins' outgoing hooks |
| `resize_packet(p, size)` | `packets` | `OnPacket`'s packet at a new size (new bytes are zero) |
| `config_dir(out, size)` | | the plugin's own folder (for every server) |
| `server_config_dir`, `character_config_dir` | | `servers/<profile>/plugin-config/<name>/` and the character's; 0 before there is one |
| `imgui_context()` | `ui` | for `ImGui::SetCurrentContext` (build with the client's Dear ImGui version) |
| `set_active`, `set_flag`, `set_player_classes`, `set_character_classes`, `set_pet_list`, `set_vitals` | `extend` | as in the `hailstone` module |
| `get_player`, `get_spawn(id)`, `next_spawn(after_id)`, `get_spell(id)`, `get_item(slot)`, `get_buff(i)`, `get_xtarget(i)` | `read` | fill the [structs](#structs); `next_spawn` from 0 walks every spawn by id. Indexes are 0-based |
| `eqstr(id, args, count, out, size)` | `read` | a game string with `%1`.. filled; -1 when there is none |
| `zone_name(id, out, size)` | `read` | a zone's short name |
| `server_info(out)` | `read` | the server this login is on |
| `combat_ability(index)` | `read` | the spell id of the player's combat ability at 0..299, 0 none |
| `skill(skill)` | `read` | the profile's skill value, without bonuses |
| `aa_value(group)` | `read` | an owned AA's slot value |
| `hot_button(bar, page, button, out, size)` | `read` | a hot button as the character ini's string; bars, pages and buttons are 0-based (10 × 10 × 12) |
| `set_hot_button`, `hot_bar_page`, `set_hot_bar_page` | `actions` (`hot_bar_page` `read`) | |
| `right_click_item(slot, class_id)` | `actions` | the client's own right click on a slot |
| `close_spell_book`, `clear_spell_recovery`, `set_window_pet(id)`, `inspect_item(id, augments)` | `actions` | |
| `max_camera_distance`, `set_max_camera_distance(d)` | `actions` | |
| `open_bank(banker)` | `actions` | the bank window at a spawn (0 the player), without a banker's checks. Added later in ABI 3: test `HAILSTONE_HOST_HAS(host, open_bank)` first |
| `get_buff_data_bulk(out, size)`, `get_pet_buff_data_bulk(out, size)`, `get_spawns_data_bulk(out, size)` | `read` | every buff slot of the player (`size` must hold every buff and song slot, RoF2's 1798 bytes, or 0 comes back; one record more, 1827, brings the discipline slot, the last), the pet window's buffs, or every spawn in the zone (`size` at least 4096, or 0 comes back; 4112 for any record), as packed records in the machine's byte order (layout in docs/plugins.md "Buff and spawn records"); returns the bytes written. Added later in ABI 3: test `HAILSTONE_HOST_HAS(host, <field>)` first |
| `add_ui_function(name, fn, user)`, `remove_ui_function` | `ui` | a function the plugin's windows call as `eq.Plugin("<name>").<name>(...)` |
| `arg_count`, `arg(i, out)`, `arg_field(i, key, out)` | | a UI function's arguments |
| `out_value`, `out_begin_array`, `out_begin_table`, `out_key`, `out_end` | | a UI function's result |
| `add_option(name, label, min, max, get, set, user)` | `ui` | an option in the Options window |
| `add_menu(label, command)` | `ui` | an entry in the EQ menu's Plugins submenu |
| `tcp_open(address, port)`, `tcp_state`, `tcp_send`, `tcp_recv`, `tcp_error`, `tcp_close` | `net` | the plugin's own TCP connections (docs/plugins.md "Connections (ABI 2)") |
| `web_open(url, w, h)`, `web_navigate`, `web_nav`, `web_close`, `web_eval`, `web_post`, `web_text`, `web_message`, `web_state`, `web_view` | `web` | browser tabs (docs/plugins.md "Web pages") |

### Hook table

`hailstone_plugin_hooks` returns a `HailstoneHooks` with `size` set to `sizeof(HailstoneHooks)`. Each entry may be
NULL. "Answered" hooks return nonzero when they set `*out`, and zero for the stock answer. The names in brackets are
the original client's functions that a hook stands in for.

| Hook | What |
|---|---|
| `int zone_entry_retry(void)` | each frame in the zone: nonzero runs the zone-in's last step again |
| `int game_command(const char* handler, const char* args)` | one of the game's commands by its handler name (`loot`, `alt`, ...); nonzero = handled |
| `int who_lines(const HailstoneWho*, HailstoneLineFn add, void* ctx)` | a `/who` reply: call `add` for each line and return nonzero. Read entries with `HAILSTONE_WHO_ENTRY(who, i)` |
| `uint8_t cast_class(void)` | the class the client's checks see while nonzero |
| `int is_spellcaster(int* out)` | [EQ_Character::IsSpellcaster] |
| `int spell_level_needed(uint32_t spell, uint32_t cls, uint8_t* out)` | [EQ_Spell::GetSpellLevelNeeded] |
| `int song_blocked(uint8_t bard_level)` | [CharacterZoneClient::BardCastBard]: nonzero = never a song |
| `int own_begin_cast_line(uint32_t spell)` | nonzero prints "You begin casting %1." for the player's own cast |
| `int can_use_gem(int gem, int* out)` | [CharacterZoneClient::CanUseMemorizedSpellSlot] |
| `int ability_timer_id(uint32_t spell, int32_t* out)` | a combat ability's timer id |
| `int discipline_reuse(int32_t timer, uint32_t* out)` | [GetDisciplineReuseTimer]: the ready time in Unix seconds |
| `int before_combat_ability(uint32_t spell, int* allowed)`, `void after_combat_ability(uint32_t spell, int used)` | around [PcZoneClient::doCombatAbility] |
| `const char* combat_effect_label(void)` | the combat abilities window's effect label |
| `uint32_t guild_member_classes(uint32_t class_value)` | a guild member's classes as a mask |
| `uint32_t vitals_classes(uint8_t class_id)` | the classes whose vitals formulas the server takes the best of |
| `int skill_gate(int32_t skill, uint32_t mask, int32_t profile_skill, int* has)` | [CharacterZoneClient::HasSkill]; asked only with a class mask set |
| `uint32_t skill_spoof_class(int32_t skill, uint32_t mask)` | the class a skill value is read with |
| `int tracking_allowed(uint8_t cls, uint32_t mask, uint32_t skill, int* out)` | the tracking check |
| `const char* item_level_line(uint32_t spell)` | the item display's Level Needed line for a scroll |
| `const char* label(int id, const char* text)` | [__GetLabelFromEQ] |
| `int hide_xtarget_slot(int slot)` | leaves an extended target slot out of the window |
| `int item_right_click(const HailstoneItemSlot*)` | [CInvSlot::HandleRButtonUp] before the client's own; nonzero = taken |
| `const char* list_level(const HailstoneItem*)` | the merchant and bazaar lists' Lvl column |
| `int reuse_focus_dropped(uint32_t spell)` | [CharacterZoneClient::GetFocusReuseMod]: nonzero = no reuse focus |
| `int before_use_skill(uint32_t skill, int* allowed)` | before [CharacterZoneClient::UseSkill] |
| `const char* command_line(const char* line)` | a typed or scripted line after `${...}` parsing: a replacement line, or NULL to keep it |
| `int map_spawn(const HailstoneSpawn*, HailstoneMapSpawn* out)` | the game's maps show only the group by themselves: nonzero shows this spawn as `*out` says |

When several plugins extend the client, it asks them in load order and the first answer wins
(`src/game/server_ext_chain.h`).

### Structs

Set `size` to `sizeof` the struct before passing it. The client fills the fields that both it and the plugin know,
so a plugin built against an older SDK keeps working. Names are copied in, cut to fit and NUL-terminated.

| Struct | Fields |
|---|---|
| `HailstonePlayer` | `spawn_id, name[64], level, race, class_id, visible_class, class_mask, cur_hp, max_hp (int64), cur_mana, max_mana, cur_endurance, max_endurance, target_id, zone_id, game_state, in_group, in_raid, spell_book_open, buff_slots, xtarget_slots, casting_spell_id, scribing_spell_id, stand_state, pet_id, filter_pc_spells, in_zone` |
| `HailstoneSpawn` | `id, name[64], level, class_id, race, owner_id, guild_id, is_npc, is_corpse, x, y, z, heading (0..512), hp_percent, rider_id, is_mercenary, is_gm, in_group, con_color, body_types[4], body_type_count` |
| `HailstoneSpell` | `id, name[64], class_level[16] (255 cannot use), timer_id, cast_time_ms, recovery_time_ms, recast_time_ms, mana, target_type, skill, is_skill, gem_icon` |
| `HailstoneItem` | `id, name[64], item_type, item_class, classes, races, icon, stack_count, charges, click_spell, scroll_spell` |
| `HailstoneItemSlot` | `type` (0 the player's possessions), `slot`, `sub` (-1 the slot itself), `aug` (-1 none); no `size` |
| `HailstoneBuff` | `spell_id` (0 empty), `tics, level, caster_id` |
| `HailstoneXTarget` | `type, status, spawn_id, name[64]` |
| `HailstoneServer` | `name[128], host[128], port, world[128], world_host[128]` |
| `HailstoneMapSpawn` | `color` (0xRRGGBB), `layer` (0..3), `label[64]` |
| `HailstoneWho`, `HailstoneWhoEntry` | a `/who` reply (see the header) |
| `HailstoneVitals` | `has` (`HAILSTONE_VITAL_*` bits), `max_hp, cur_hp, max_mana, cur_mana, max_endurance, cur_endurance`; no `size` |
| `HailstonePet` | `spawn_id, class_id`; no `size` |
| `HailstoneWebState` | `size` (a `size_t`), `url[1024], title[256], loading, can_back, can_forward, closed` |
| `HailstoneValue` | `kind` (`HAILSTONE_NIL`, `_BOOL`, `_INT`, `_FLOAT`, `_STRING`, `_TABLE`), `i`, `f`, `s` |
| `HailstonePacket` | `opcode, data, size` |

### Constants

| Constant | Values |
|---|---|
| `HAILSTONE_PLUGIN_ABI` | 3 |
| `HAILSTONE_CAP_*` | `READ` 0x001, `COMMANDS` 0x002, `TARGET` 0x004, `ACTIONS` 0x008, `MOVE` 0x010, `CHAT` 0x020, `EVENTS` 0x040, `UI` 0x080, `PACKETS` 0x100, `EXTEND` 0x200, `LOGIN` 0x400, `NET` 0x800, `WEB` 0x1000 |
| `GAMESTATE_*` | `PRECHARSELECT` -1, `CHARSELECT` 1, `CHARCREATE` 2, `INGAME` 5, `LOGGINGIN` 253, `UNLOADING` 255 |
| `HAILSTONE_STREAM_*`, `HAILSTONE_IN` / `_OUT` | `WORLD` 0, `ZONE` 1; `IN` 0 (from the server), `OUT` 1 |
| `HAILSTONE_FLAG_*` | `WIDE_ITEM_LINKS` 0x01, `ITEMS_ANY_CLASS` 0x02, `HIDE_ANY_CLASS` 0x04, `AA_ANY_LEVEL` 0x08, `AA_TRAIN_ALL` 0x10, `AUGMENT_CLASSES` 0x20, `RESEARCH_CLASSES` 0x40, `FOCUS_CLASSES` 0x80, `BACKSTAB_EITHER_HAND` 0x100, `SOLO_ADVENTURE` 0x200 |
| `HAILSTONE_VITAL_*` | `MAX_HP` 0x01, `CUR_HP` 0x02, `MAX_MANA` 0x04, `CUR_MANA` 0x08, `MAX_ENDURANCE` 0x10, `CUR_ENDURANCE` 0x20 |
| `HAILSTONE_SLOT_CURSOR` | 33 |
| `HAILSTONE_TCP_MAX` | 8 |

docs/plugins.md "Server extensions (ABI 2)" explains what each flag does. "The names up to 0.6" there covers the old
`nmsc_*` names, which still build and load.
