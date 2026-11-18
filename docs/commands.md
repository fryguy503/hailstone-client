# Commands and shortcuts

Every slash command, key binding and mouse or window shortcut Hailstone has, in one place. The game's own commands
follow the RoF2 client; the ones Hailstone adds are marked as Hailstone's. For the gamepad, see docs/gamepad.md.

## How a typed line is read

- **Any start of a command name works.** `/wh` runs `/who` and `/g` runs `/gsay`. When two commands start the same
  way, the one listed first in the original client's table wins, so a short form may not be the command you expect.
  Upper and lower case do not matter.
- **A line without a slash** goes to the chat window's channel (say, unless the window has another one).
- **`'` and `:`** at the start of a line are `/say` and `/emote`.
- **`;` and `[`** send the line to the chat server (`;` sends the text after it, `[` the whole line), as the
  original did.
- **`#` commands** (`#help`, `#zone` and the rest) are EQEmu's server commands. Hailstone sends them on as said text,
  and the server answers. What a character may use depends on its status on the server, not on the client.
- **While dead**, only the chat commands (say, tell, group, guild, raid, channels and the like), `/consent`, `/deny`,
  `/ignore`, `/friends`, `/petition`, `/help`, `/location`, `/marketplace` and the mercenary commands work. Anything else answers
  "You may not use that command while dead."
- **Locked interface.** With the interface locked (`/safelock`), only `/safelock` gets through.
- **Plugins and scripts** can add commands of their own; they take a line before the game's commands do (see
  [Add-on and script commands](#add-on-and-script-commands)).
- A command the original has but Hailstone does not run answers "/name is not supported by this client yet." A word
  that is no command at all answers "That is not a valid command. Please use /help."
- `/help` lists the commands by class; `/help normal`, `/help emote`, `/help guild` and `/help chat` list one class.

## Chat and communication

| Command | What it does |
|---|---|
| `/say <text>`, `'<text>` | Say to everyone nearby. |
| `/tell <name> <text>`, `/msg`, `/send` | Tell one player. |
| `/ttell <text>` | Tell your target. |
| `/reply <text>` | Reply to the last tell you got. |
| `/rtarget [name]` | Target the named player, or the last player who sent you a tell. |
| `/gsay <text>` | Group chat. |
| `/rsay <text>` | Raid chat. |
| `/guildsay <text>` | Guild chat. |
| `/fsay <text>` | Fellowship chat. |
| `/shout <text>` | Shout to the zone. |
| `/ooc <text>` | Out-of-character chat to the zone. |
| `/auction <text>` | Auction chat to the zone. |
| `/emote <text>`, `/me`, `:<text>` | A free-text emote. |
| `/yell` | Call for help. |
| `/channel <channel> [name]` | Set the chat window's channel. |
| `/chat <text>` | Send a command to the chat server. |
| `/join <channel>[:password][,channel...]` | Join chat server channels. |
| `/leave [channel]`, `/leaveall` | Leave one chat channel, or all of them. |
| `/list [channel]` | List your channels, or the players in one. |
| `/1 <text>` .. `/10 <text>` | Talk in your chat server channel 1 to 10. |
| `/announce [on\|off]` | Channel join and leave announcements. |
| `/autojoin <channels>` | Channels to join every time you enter the world. |
| `/language [number]` | List the languages you know, or speak one. |
| `/afk [message]` | Away from keyboard, with an optional auto-reply. |
| `/anonymous`, `/roleplay` | Hide your details in `/who`; flag yourself as role-playing. |
| `/ignore [name]`, `/friends [name]` | The ignore and friends lists. |
| `/consent <name>`, `/deny <name>` | Let a player drag your corpse, or take that back. |
| `/usercolor` | Chat colours: no argument shows the help page. |
| `/chatfontsize <0..10>` | The active chat window's font size. |
| `/clearchat`, `/clearallchat` | Empty the active chat window, or all of them. |
| `/log [on\|off] [name]` | Log chat to a file in the character's folder. |
| `/secondslog [on\|off]` | Kept as a setting; nothing reads it, as in the original. |

## Information

| Command | What it does |
|---|---|
| `/who [filters]`, `/whotarget` | Who is online or in the zone; who your target is. |
| `/consider` | Consider your target. |
| `/location`, `/loc` | Your coordinates. |
| `/time` | The game time. |
| `/played` | Time played on this character. |
| `/random [low] [high]` | Roll a random number. |
| `/charinfo` | Your bind points. |
| `/motd` | The guild message of the day. |
| `/uptime` | How long the world and the zone have been up. |
| `/netstats` | Connection statistics. |
| `/groupleader` | Who leads your group. |
| `/grouproles` | Your group's roles. |
| `/findpc <name>` | Look for a player. |
| `/inspectbuffs` | Use the Inspect Buffs ability. |
| `/keys` | Your key ring. |
| `/outputfile <guild \| guildbank \| guildhall \| raid \| spellbook \| inventory \| realestate> [file]` | Write a list to a file in the character's folder. |
| `/help [class]` | The command list. |

## Targeting and combat

| Command | What it does |
|---|---|
| `/target <name \| id <#> \| clear \| mycorpse \| spawn search>` | Target by name or MacroQuest's spawn search (Hailstone's script layer answers `/target`; a plain name skips you). |
| `/assist [name]` | Target what your target, or the named player, is fighting. |
| `/attack [on\|off]` | Auto attack. |
| `/autofire` | Auto fire (ranged). |
| `/cast <1..12>` | Cast the spell in a gem. |
| `/stopcast` | Stop casting. |
| `/melody <gems>`, `/stopsong` | Twist songs; stop singing. |
| `/discipline <name>`, `/stopdisc` | Use a discipline; end the active one. |
| `/doability <n \| name>` | Use an ability or skill: button 1-10 of the Actions window, as RoF2; or (Hailstone's) the player's skill, discipline or activated AA of that name, in any case, as its Ability Book row uses it. A name more than one kind has is the skill's first, then the discipline's, then the AA's; a discipline's name may leave out its rank. |
| `/pet <command>` | Pet commands (attack, back off, follow, guard, sit and the rest). |
| `/shield`, `/shieldgroup` | Shield your target. |
| `/targetgroupbuff [on\|off]`, `/tgb` | Group spells land on the target's group. |
| `/xtarget [n \| show \| target \| set \| add \| remove \| auto]` | The extended target window: `/xtarget` alone prints the usage. |
| `/aggrometer`, `/aggrolock` | The aggro meter and its lock. |
| `/marknpc <1..3>`, `/gmarknpc`, `/rmarknpc`, `/clearmarks` | Group and raid NPC marks. |
| `/indicator [on\|off]` | The target indicator, for this session. |
| `/blockspell <display\|add\|remove\|clear> <me\|pet> [spell ids]` | Blocked buffs. The lists are kept in the character ini and sent to the server at each zone-in; a spell the server takes as newly blocked comes off the player (or the pet), with the effects of its spell group. |
| `/sheathe` | Sheathe or draw your weapons. |
| `/duel`, `/decline` | Challenge your target to a duel; decline one. |

## Movement and position

| Command | What it does |
|---|---|
| `/sit [off]`, `/stand` | Sit or stand. |
| `/follow` | Follow your target. |
| `/dismount` | Get off your mount. |
| `/rewind` | Ask the server to move you back to your last good position when stuck. |
| `/camp [desktop \| server]` | Camp to character select, the desktop or the server list. |
| `/quit`, `/exit` | Quit the game. |
| `/offlinemode` | Offline trader or buyer mode. |
| `/corpse`, `/corpsedrag`, `/corpsedrop` | Pull your targeted corpse to you, drag a corpse, drop it. |
| `/decaycorpse` | Let your targeted corpse decay, after a question. |
| `/hidecorpses [all \| allbutgroup \| looted \| npc]` | Hide corpses; another word shows them again. |
| `/setstartcity <name>` | Set your start city (the server decides). |
| `/servertransfer` | Ask the server for its transfer list. |

## Items, loot and trade

| Command | What it does |
|---|---|
| `/loot` | Loot your target. |
| `/lootnodrop [always \| usable \| never]` | Ask before looting No Drop items. |
| `/autoinventory` | Put the item on your cursor into your inventory. |
| `/useitem <slot> [bag index]` | Use an item by slot number. |
| `/split <pp> <gp> <sp> <cp>`, `/autosplit` | Split coin with the group; split it always. |
| `/bandolier <add \| delete \| activate> <name or number>` | Weapon sets. |
| `/potionbelt` | The potion belt. |
| `/fastdrop [on \| off \| never]` | Drop items without asking. |
| `/bazaar`, `/trader`, `/barter`, `/buyer` | The bazaar search, trader, barter search and buyer windows. |
| `/marketplace` | The marketplace window. |
| `/acompare`, `/aclearcompare` | Compare your target's gear; clear the comparison. |
| `/inspect`, `/open` | Inspect and open an adventure chest. |
| `/reveal` | Ask the server to reveal your target, within 75 units. Stock EQEmu does not answer it. |
| `/toggleinspect` | Allow or refuse being inspected. |
| `/invwinlabels [on\|off]` | Labels in the inventory window. |

## Spells, abilities and progression

| Command | What it does |
|---|---|
| `/book [close]` | The spell book. |
| `/memspellset <name>` | Memorize a saved spell set. |
| `/spellscribe` | Scribe the spell scrolls you carry. |
| `/alternateadv on <percent> \| off \| list \| buy <id> \| activate <id>` | AA experience split, and listing, buying or using abilities. |
| `/resetaas` | Ask the server to reset your AAs (the server decides). |
| `/stoptracking`, `/trackplayers`, `/trackpets`, `/trackmercs`, `/tracksort`, `/trackfilter` | Tracking options. |
| `/surname <name>` | Set your surname. |
| `/changename`, `/changepetname`, `/changemercname` | Rename, where the server allows it. |
| `/claim`, `/veteranReward` | The claim window; claim a veteran reward. |

## Group, raid, guild and tasks

| Command | What it does |
|---|---|
| `/invite [name]`, `/disband`, `/makeleader [name]` | Group invite, leave, hand over leadership. |
| `/lfgroup [on\|off]` | Looking for a group; no argument opens the group finder. |
| `/raidinvite [name]`, `/raidaccept`, `/raiddecline`, `/raiddisband` | Raid invitations; leave the raid. |
| `/raidwindow`, `/makeraidleader [name]` | The raid window; hand over the raid. |
| `/setloottype <1..4>`, `/addraidlooter`, `/removeraidlooter` | Raid loot rules (the raid leader). |
| `/guildinvite`, `/guildremove`, `/guilddelete`, `/guildstatus`, `/guildleader`, `/guildwar`, `/guildpeace`, `/guildcreate`, `/guildpromote`, `/guilddemote` | Guild management. |
| `/guildmotd <text>`, `/getguildmotd` | Set or show the guild message of the day. |
| `/lfguild` | The looking-for-guild window. |
| `/taskaddplayer`, `/taskremoveplayer`, `/taskplayerlist`, `/taskmakeleader`, `/tasktimers`, `/taskquit`, `/taskhelp` | Shared tasks. |
| `/dzaddplayer`, `/dzremoveplayer`, `/dzswapplayer`, `/dzmakeleader`, `/dzplayerlist`, `/dzlisttimers`, `/dzquit`, `/dzhelp` | Expeditions (dynamic zones). |
| `/comparetaskraid`, `/compareexpedraid` | Who is in the task or expedition but not the raid, and the other way round. |
| `/mercwindows`, `/stance <name>`, `/mercassist [on\|off]`, `/mercswitch <n>`, `/merclog` | Mercenaries. |

## Windows, display and sound

| Command | What it does |
|---|---|
| `/map` | The map. |
| `/note [text]` | The notes window, or add a dated line to your notes. |
| `/filter` | Chat filters. |
| `/hotbutton <name> [colour] <text>` | Make a hot button from a line and put it on the cursor. |
| `/loadskin <name> [keep]` | Load a UI skin. |
| `/copylayout` | Copy another character's layout. An ini no character or defaults file names is looked for beside the current layout file by its bare name; a path is not found. |
| `/fontface <name>` | The UI font. |
| `/viewport reset`, `/viewport <x> <y> <w> <h> [scale]` | The 3D view's area on screen. |
| `/fullscreen [on \| off \| toggle]` | Full screen or windowed. |
| `/shownames [1..6 \| off]` | Name plates above heads. |
| `/showgrass [on\|off]`, `/shadows [on\|off]`, `/dynamiclights [on\|off]` | Grass, shadows, dynamic lights. |
| `/particledensity <0..3 \| off \| low \| med \| high>`, `/showspelleffects [on\|off]` | Spell effects. |
| `/bodytint` | The body tint window. |
| `/facepick` | Change your face (player customization). |
| `/clickthrough` | Click through windows. |
| `/mousespeed <n>` | The pointer speed, 0..10 (a number past either end is held to it: the original kept it as typed). |
| `/combatmusic [on\|off]`, `/mixahead <1..100>`, `/reversesound <normal\|reverse>` | Sound settings. |
| `/feedback`, `/bugreport`, `/report <name>`, `/petition <text>`, `/guidehelp <text>`, `/survey` | Feedback, bug report, reporting a player, petitions. |
| `/url <address>`, `/www <address>` | Open a web address in your system's browser (`/www` adds `http://`). |
| `/realestate <items \| manage \| plot>`, `/leaverealestate` | Housing windows. |
| `/safelock [password]` | Lock or unlock the interface. |
| `/alarm` | The alarm window. |
| `/stopsocial` | Stop a running social. |
| `/reverb`, `/resetwindows`, `/paperdoll`, `/popmail`, `/vreport` | Accepted and do nothing, as in the original. |

## Emotes

Each plays an animation and a line to the people around you, with or without a target:

`/agree` `/amaze` `/apologize` `/applaud` `/bird` `/bite` `/bleed` `/blink` `/blush` `/boggle` `/bonk` `/bored`
`/bounce` `/bow` `/brb` `/burp` `/bye` `/cackle` `/calm` `/cheer` `/chuckle` `/clap` `/comfort` `/congratulate`
`/cough` `/cringe` `/cry` `/curious` `/dance` `/drool` `/duck` `/eye` `/fidget` `/finger` `/flex` `/flipoff`
`/frown` `/gasp` `/gesture` `/giggle` `/glare` `/grin` `/groan` `/grovel` `/hail` `/happy` `/hug` `/hungry`
`/introduce` `/jk` `/kiss` `/kneel` `/laugh` `/lost` `/massage` `/moan` `/mourn` `/nod` `/nudge` `/panic` `/pat`
`/peer` `/plead` `/point` `/poke` `/ponder` `/purr` `/puzzle` `/raise` `/ready` `/roar` `/rofl` `/rude` `/salute`
`/shiver` `/shrug` `/sigh` `/smack` `/smile` `/smirk` `/snarl` `/snicker` `/stare` `/tap` `/tease` `/thank`
`/thirsty` `/veto` `/wave` `/welcome` `/whine` `/whistle` `/yawn`

`/hail` also hails your target, which is how you start talking to an NPC.

## Hailstone's own commands

These are not in the original client.

| Command | What it does |
|---|---|
| `/abilitybook [auto [on\|off]]` | The Ability Book; `auto` turns autoskill on or off. |
| `/finditem [text]`, `/advi [text]` | The Advanced Inventory, searching everything you have for the text. |
| `/destroy` | Destroy the item on your cursor (the inventory's Destroy button). |
| `/destroycursor <id>` | Destroy the item on your cursor without asking, only if its item id is `<id>`; never coin or a bag. For plugins (docs/plugins.md). |
| `/stowcursor <id>` | Put the item on your cursor into your packs (never a worn slot), only if its item id is `<id>`. For plugins (docs/plugins.md). |
| `/web [address]` | The in-game browser, on a new tab (docs/browser.md). |
| `/luasocial [1..120 \| new]` | The Lua social editor. |
| `/lighting [modern \| smooth \| classic]` | Switch the lighting (docs/graphics.md); without a mode, between modern and classic. |
| `/screenshot` | Take a screenshot. |
| `/perf` | The performance window. |

## Add-on and script commands

The client's MacroQuest-style layer adds these (docs/plugins.md, docs/navigation.md). When a script or plugin runs
one, the server's add-on rules decide whether it may.

| Command | What it does |
|---|---|
| `/lua run <script> [args...]`, `/lua stop [script]`, `/lua pause [script]`, `/lua list` | Lua scripts. |
| `/plugin list`, `/plugin <name> [load\|unload]` | Compiled plugins. |
| `/nav ...` | Walk along the navigation mesh: `/nav target`, `/nav id <#>`, `/nav loc <y> <x> <z>`, `/nav door <name> [click]`, `/nav stop`, `/nav status`, `/nav option ...`, `/nav help`. |
| `/moveto loc <y> <x> [z] \| id [#] \| mdist <n> \| off` | Walk in a straight line, without the mesh. |
| `/stick [<distance>] [behind] [id <#>]`, `/stick off` | Stay with the target or a spawn. |
| `/target ...`, `/face ...` | Target or face by MacroQuest's spawn search (`/target` is above). |
| `/keypress <command> [hold\|release]` | Press a key command by name. |
| `/echo <text>` | Print a line to chat. |
| `/docommand <line>`, `/noparse <line>` | Run a line; run it without expanding `${...}`. |
| `/squelch <line>` | Run a line without the chat lines it prints. |

In a social's lines, `/pause` waits and `/timer` sets the button's cooldown (docs/dev-ui.md).

## GM-only commands

These need a GM account and are not commands at all for other players, as in the original. On EQEmu, most of what a
GM does goes through the server's `#` commands instead.

- `/find <name>` (Hailstone: a path to the named spawn or the target. For everyone, the Find window does this: Ctrl+F.)
- Not run by Hailstone: `/shownpcnames`, `/private`, `/goto`, `/becomenpc`, `/searchcorpse`, `/emoteworld`,
  `/emotezone`, `/summon`, `/summoncorpse`, `/broadcast`, `/lastname`, `/zone`, `/kick`, `/kill`, `/delcorpse`,
  `/hideme`, `/servers`, `/height`, `/selfkill`, `/toggletell`, `/inquire`, `/praise`, `/warn`, `/approval`,
  `/adventure`.

## Commands not supported

These are in the original client's table but answer "not supported by this client yet". Most are the publisher's
own services or test commands with nothing behind them on an EQEmu server.

`/adriver` `/sdriver` `/aggressive` `/beta` `/betabuff` `/dopropertinting` `/eqplayersguildupdate`
`/eqplayersupdate` `/facebook` `/fb` `/gems` `/kickplayers` `/mcicontrol` `/racechange` `/resetCreationServer`
`/share` `/system` `/testbuffme` `/testcopy` `/tweet` `/twitter` `/voicechat` `/webupdate` `/welcomescreen`

Left out on purpose, so they answer as unknown commands: the voice macros (`/vplay`, `/vtell`, `/vgroup`,
`/vraid`), `/achievements`, `/legendsofnorrath` and `/tcg`.

## Keys

Every key command can be rebound in **Options > Keys**, two keys each, with or without Shift, Ctrl and Alt. The
middle mouse button and up to five side buttons bind like keys (MOUS MID, MOUS AUX1..AUX5). A key bound to more than
one command is shown in red there and runs each of them. Bindings are saved in `hailstone.ini`, for every
character.

The defaults below are the original's. Movement uses WASD unless `UseWASDDefault=0` is set in `hailstone.ini`; then
the number pad moves you and **D** ducks.

### Movement

| Action | Key | Second key |
|---|---|---|
| Forward / back | W / S (Num 8 / Num 2) | Up / Down |
| Turn left / right | A / D (Num 4 / Num 6) | Left / Right |
| Jump | Space | |
| Duck | X (D) | |
| Auto run | Num Lock | |
| Move down (swimming, flying) | End | |
| Look up / down | Num 9 / Num 3 | Page Up / Page Down |
| Center view | Num 5 | Home |
| Sit / stand | Ctrl+S | |
| Run / walk | Ctrl+R | |
| Mouse look | F12 | |

### Targeting

| Action | Key | Second key |
|---|---|---|
| Target yourself | F1 | |
| Target group member 1..5 | F2..F6 | |
| Target nearest player | F7 | |
| Target nearest NPC | Tab | F8 |
| Consider | C | |
| Hail | H | |
| Auto attack | Q | |
| Use (doors, objects) | U | |

**Tab is Hailstone's change:** the original put Tab on "toggle target" and F8 on the nearest NPC. Tab and F8 skip
NPCs you cannot see; Options > Interface "Tab Needs Line of Sight" turns that off.

### Hot buttons, spells and abilities

| Action | Key |
|---|---|
| Hot button 1..12 of bar 1 | 1..0, -, = |
| Hot bar page 1..10 | Shift+1..Shift+0 |
| Cast spell gem 1..12 | Alt+1..Alt+0, Alt+-, Alt+= |
| Combat ability 1..8 | Ctrl+1..Ctrl+8 |
| Stop casting | Shift+S |

The other bars' buttons, pages, potion belt slots and extended target slots have keys to bind but none by default.

### Chat

| Action | Key |
|---|---|
| Start typing | Enter |
| Start a command | / |
| Start a chat server line | ; or [ |
| Say / emote mode | ' / Shift+; |
| Tell | T |
| Reply | R |
| Cycle the players who told you | Tab / Shift+Tab (on a `/tell` or `/rtarget` line) |
| Target the last player who told you | Ctrl+T |
| Earlier / later lines you sent | Shift+Up / Shift+Down (Shift+Num 8 / Shift+Num 2) |
| Scroll chat a page | Shift+Page Up / Shift+Page Down (Shift+Num 9 / Shift+Num 3) |

### Camera

| Action | Key | Second key |
|---|---|---|
| Next camera | F9 | |
| Zoom in / out | Num 7 / Num 1 | Insert / Delete |
| Camera forward / back | Alt+Num 8 / Alt+Num 2 | Alt+Up / Alt+Down |
| Camera left / right | Alt+Num 4 / Alt+Num 6 | Alt+Left / Alt+Right |
| Camera pitch up / down | Alt+Num 9 / Alt+Num 3 | Alt+Page Up / Alt+Page Down |
| View distance nearer / farther | Shift+- / Shift+= | |
| Screenshot | Num - | |

Each camera (first person, overhead, chase, user 1, user 2, tether) also has a key to bind.

### Windows and actions

| Window or action | Key | | Window or action | Key |
|---|---|---|---|---|
| Inventory | I | | Spell book | Ctrl+B |
| Open or close all bags | Shift+B | | Spell gems | Alt+S |
| Map | Backspace, M | | Alternate advancement | V |
| Find window | Ctrl+F | | Leadership | L |
| EQ menu | Alt+M | | Combat abilities | Alt+C |
| Options | Alt+O | | Auras | Shift+A |
| Player | Alt+Y | | Bandolier | B |
| Group | Alt+P | | Potion belt | Shift+P |
| Target | Alt+T | | Titles | Shift+T |
| Buffs | Alt+B | | Tasks | Alt+Q |
| Short buffs | Alt+E | | Expeditions | Alt+Z |
| Hot bar 1 | Alt+H | | Tribute benefits | Alt+U |
| Compass | Alt+D | | Raid | Alt+R |
| Friends | Alt+F | | Guild management | Alt+G |
| Pet | Alt+F1 | | Journal | Alt+J |
| Mail | Ctrl+E | | Story | Alt+N |
| Bazaar (trader) | Alt+I | | PvP stats | Ctrl+P |
| Bazaar search | Alt+A | | Housing items | Shift+I |
| Actions: abilities / combat / socials / main | Ctrl+A / Ctrl+C / Ctrl+O / Ctrl+M | | Who | Ctrl+W |
| Invite or follow | Ctrl+I | | Disband | Ctrl+D |
| Camp | Ctrl+X | | | |

Housing item placement has its keys on Shift+Z, Shift+Q, Shift+W, Shift+E, Shift+R, Shift+D, Shift+F and Shift+G.

F10 toggles all in-game UI presentations, including addon windows, captions and selection/navigation overlays. Press it again to restore the interface; its window visibility and layout settings are retained. Remap **Toggle UI Display** in Options > Keys > Windows (either binding). Loading and pregame screens remain visible.

Bound by default but doing nothing in Hailstone yet: F11 (network statistics), Alt+W (selector
window), Alt+K (music player), Alt+V (adventure request window), Shift+Alt+R (release the mouse cursor), and the voice
keys Num + and Ctrl+Tab (Hailstone has no voice chat).

### Fixed shortcuts

These are not in Options > Keys.

| Key | What it does |
|---|---|
| Print Screen | Take a screenshot, unless you bound Print Screen to another command. |
| Alt+Enter | Center the mouse pointer. |
| Ctrl+Shift+D | The developer windows (info panel, spawn list, Plugins & Lua). |
| Ctrl+Shift+P | The performance window. |
| Up / Down, Enter, Esc | Character select: next or previous character, enter the world, quit. |

### Modern UI

| Shortcut | What it does |
|---|---|
| Drag a frame, or Alt+drag | Move a HUD frame. |
| Right-click a frame | Its menu: lock, layout mode, scale, opacity, reset, hide. |
| Alt+right-click | The client's window menu, where the game has a menu of its own. |
| Ctrl+mouse wheel | Scale the window or menu under the pointer. |
| Double-click a title bar | Roll the window up or down. |
| Ctrl+Enter | Accept in the social editor. |
| Tab | Complete a slash command in the social editor. |
| Ctrl+A, Ctrl+click, Shift+click | Select rows in the Advanced Inventory and the journal. |
| Enter | Run the search in the bazaar, barter, marketplace and tradeskill windows. |

## Mouse

- **Left click** a spawn to target it. Left click picks up and puts down items.
- **Right button held** is mouse look; while it is held, the turn keys strafe.
- **Mouse wheel** zooms between first person and the camera behind you.
- **Right click** a player to open a menu (Hailstone's: the original opened a trader's shop, a buyer's barter
  window, or inspected the player); right click an item to use it.
- **Hold the left button** on an inventory or bag item, or drag it off the slot, and let go over a hot bar to make
  a hot button for it. With Shift or Ctrl held, the button is for the slot instead of the item.
- In the map: wheel to zoom, drag to pan, right click for "Find a path here".
