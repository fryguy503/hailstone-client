# Gamepad support and the gamepad UI

Hailstone can be played with an Xbox, PlayStation, Nintendo or generic SDL3 gamepad through `ui/gamepad`, a UI
pack built for it (`input=gamepad`). This is a **client extension**: RoF2 has no gamepad scheme.

The pad only *presses the client's key commands* (game/keymap.h, `KeyCommand`), through the same `key_command` /
`movement_.command` paths as the keys, so every game rule stays the game's. The stick gives the digital forward /
back / strafe commands, since the keyboard commands have no analog speed.

Mouse and keyboard keep working alongside the pad, with any UI pack. The gamepad pack lays its windows out for the
pad: large text and targets, focus navigation instead of a pointer, no window that needs typing.

## 1. Layers

| Layer | Where | What it does |
|---|---|---|
| Device | `src/platform/window.*` (SDL_INIT_GAMEPAD) | opens and closes pads on hot-plug and fills `InputState::pad` (buttons, axes, type) once per frame |
| Bindings | `src/game/gamepad_bindings.*` (pure, unit-tested) | maps pad buttons and chords to key commands, or to the pad's own actions (cross hotbar slot, mode switch, camera) |
| App | `src/app/client_app_gamepad.cpp` | in game mode, turns the bindings into key events, movement and camera; in menu and cursor modes, hands the pad to the windows |
| ImGui | `src/ui/imgui_layer.cpp` | `NavEnableGamepad`; menu mode lets the SDL3 backend drive ImGui navigation |
| Lua | `src/app/lua_ui_gamepad.cpp` (`hs.gamepad`), `src/ui/lua/lua_imgui.cpp` (navigation calls) | gives the windows the pad's state, its mode and its glyphs |
| UI pack | `ui/gamepad/` | the HUD, the cross hotbar, the main menu and the gamepad windows |

## 2. Modes

The pad is in one of three modes (`hs.gamepad.mode()`).

**game**: the pad moves, turns the camera, targets and fires hot buttons. ImGui gets no pad input.

**menu**: a gamepad window has the focus. The pad drives ImGui navigation (D-pad or left stick moves, A activates,
B goes back or closes, LB / RB switch tabs); the game gets nothing.

- Start (the main menu), or any gamepad window that asks for the focus, enters it; closing the last one leaves it.
- A mouse click elsewhere takes ImGui's focus but not the pad: the pad's next button gives the newest window its
  focus back.
- Windows asking in the same frame all count, the last on top; when the top one closes (a Tip over the merchant),
  the one under it takes the focus.
- Windows restored at zone-in do not ask, so the pad starts out playing.
- Pad items must use `ui.ButtonFlags.EnableNav`: ImGui 1.91 leaves invisible buttons out of navigation.
- It also ends when no window has had the focus for a few frames.

**cursor**: for windows not built for the pad (the temporary C++ windows). The left stick moves the pointer, the
right stick scrolls; A is a left click, X a right click, B leaves. Back toggles it from game mode; the real mouse
moving or clicking ends it. The main menu enters it only for a tile chosen with the pad.

The pad's commands act even while a window has the keyboard: the pad is not the keyboard.

### Interface lock

With the interface locked (/safelock) the pad loses what the mouse and locked keys lose:

- its key commands go through the keys' refusal;
- its hot buttons, interact, cancel, targeting and camera do nothing;
- menu and cursor modes fall back to game mode;
- `hs.gamepad.down` / `pressed` / `released` answer false.

The chat input's windows are exempt (`pad_chat_window` in app/gamepad.h), so a pad alone unlocks it: Back + Start,
then `/safelock <password>` on the keys (or the quick-chat list's "/safelock ..."). Checked: __ProcessMouseEvents,
__ExecuteCmd.

### Input scripts

Testing: `--input-script` (app/input_script.h) plays a virtual Xbox pad's buttons and sticks, keys, typed text and
mouse input at set times after zoning in, so all of the above can be checked without a pad.

- `c<seconds>` counts from when character select appeared (`3.5 text /camp; c3 pad start` camps and re-enters);
  `d<seconds>` from a link death's disconnected screen.
- It also takes screenshots, logs the pad's mode and ImGui's focus, sends the close request (`close`) and silences
  the links (`droplink`).

## 3. Default bindings (game mode)

Button names use Xbox labels. The UI shows each pad's own glyphs (`hs.gamepad.glyph`).

| Input | Action | Key command |
|---|---|---|
| Left stick | move: forward / back past 0.35, strafe left / right past 0.5 | CMD_FORWARD, CMD_BACK, CMD_STRAFE_LEFT/RIGHT (held) |
| L3 (click) | autorun toggle | CMD_AUTORUN |
| Right stick | camera: sideways turns (like mouse look), up / down tilts | (yaw_, pitch_, as update_mouse_look) |
| R3 (click) | first person / camera behind the player | (camera_distance_ 0 / last) |
| A | use / interact: the target's right-click action when it is in range, else USE (doors, objects) | USE, or the spawn's right click |
| B | cancel: stop casting, else close the top window, else clear the target | CMD_STOP_CAST, CLOSE_TOP_WINDOW, CLEAR_TARGET |
| X | auto attack toggle | AUTOPRIM |
| Y | jump | CMD_JUMP |
| LB / RB | previous / next NPC target, nearest first | CYCLENPCTARGETS (RB), its reverse (LB) |
| D-pad up / down | cycle self and group members (F1..F6) / sit or stand | TARGETME, PARTY1..5 / SIT_STAND |
| D-pad left / right | hot bar 1 previous / next page | HOTPAGE1_* |
| LT held, RT held, LT+RT held | **cross hotbar** sets 1, 2 and 3: while the trigger is held, D-pad and face buttons fire that set's 8 slots instead of their usual actions | hot buttons (below) |
| Start | main menu (menu mode) | |
| Back | cursor mode toggle | |
| Back + Start | toggle the chat input (an on-screen keyboard in the gamepad pack) | |

The **cross hotbar** has 3 sets of 8 slots: D-pad left, up, right, down, then X, Y, B, A. By default they are hot
bars 1, 2 and 3, buttons 1–8 (LT, RT, LT+RT). A slot fires `ActionsUi::hot_button(bar, button)` as a hot button key
does, so the hot bar windows show and edit the same buttons.

A trigger counts as held past 0.5; the stick dead zone is 0.2 (radial). Every value is a setting.

## 4. Settings

The bindings are client-wide, in `hailstone.ini`:

```
gamepad.enabled = 1
gamepad.deadzone = 0.2
gamepad.trigger_threshold = 0.5
gamepad.camera_speed = 2.5        ; radians per second at full right-stick deflection
gamepad.invert_y = 0
gamepad.rumble = 1
gamepad.move_threshold = 0.35     ; the left stick's forward / back
gamepad.strafe_threshold = 0.5    ; and strafe
gamepad.cursor_speed = 900        ; cursor mode: pixels per second at full left-stick deflection (100% UI scale)
gamepad.bind.<input> = <action>   ; e.g. gamepad.bind.x = AUTOPRIM, gamepad.bind.lt+a = hot:1:8
gamepad.cross.<set>.<slot> = <bar>:<button>
```

`<action>` is one of:

- a `KeyCommand` name (`game::command_by_name`), with or without `CMD_` (`CMD_FORWARD` = `FORWARD`);
- `hot:<bar>:<button>`, or `none`;
- a pad action: `menu`, `cursor`, `camera_toggle`, `camera_iso` (the isometric camera on or off,
  docs/isometric.md), `interact`, `cancel`, `target_prev`, `target_next`, `target_group` (D-pad up's self and group
  cycle), `hotpage_prev`, `hotpage_next` (D-pad left / right), `chat`.

`<input>` is a button name, a trigger chord (`lt+a`, `rt+up`, `lt+rt+x`) or `back+start`. A chord with no binding
of its own gives its cross hotbar slot (D-pad and face buttons) or the plain button's action. Only the bindings that
differ from the defaults are written.

The gamepad pack's Options page edits them. The numbers are also client options (`hs.Option` / `hs.SetOption`):
`gamepad_<setting>` in whole steps (thresholds and dead zone in percent, camera speed in tenths, booleans 0 / 1).

Interact (A) uses the target's right-click action within 15 units, else USE on what is at the screen's middle, else
the nearest door in front.

### The client's pre-game screens

Outside the world, the client's ImGui screens (shown with a pack that does not draw its own) take pad navigation
in game mode too; Back still gives cursor mode.

- The screens show the pad's buttons at the bottom (`ui::set_screen_gamepad`).
- The cursor starts on the first empty login field or the picked server or character; the picked row follows it.
- A on a row plays that server or enters the world with that character.
- Start does the screen's main action (Login, Play, Enter World, Create). B goes back (not on the login screen).
- A text field activated with the pad (A, or Y) opens an on-screen keyboard: A types the key under the cursor, X
  deletes, Y types a space, LB is shift, Start or Done accepts, and B cancels.
- On the login screen, Start with a field still empty opens the keyboard on it.

### The gamepad pack's pre-game screens

The Gamepad UI draws its own pre-game screens (`ui/gamepad/login.lua`, `serverselect.lua`, `charselect.lua`,
`charcreate.lua` over `hs.LoginScreen` and the rest, docs/dev-ui.md).

- **Login** starts on Continue as's character tiles: A plays one (asking for a password not saved). LB / RB change
  the server. Y opens More options (box set, game files, add-ons, About Hailstone with What's new, music, Quit).
  Start logs in (or opens the keys on the first empty field). B goes back from a page.
- **Server select**: A plays the focused world, Y marks it a favourite (or unmarks it), B goes back.
- **Character select**: the focused character is the one the scene shows. A or Start enters the world, X creates,
  Y deletes (asked first), B goes back.
- **Character creation**:
  - the rows are stepped in place (gender, race, class, deity, start city: D-pad left / right, or A);
  - A on the name opens the keys, and X asks the server for a random name;
  - A on Stats opens a row per stat: left / right returns or spends a point, X spends all it can, LB resets;
  - Y opens Appearance;
  - Start creates once every point is spent and the name has 4 letters or more; B cancels;
  - the box under the rows describes the focused race, class, deity, city or stat.

Testing: `test.login = server | more | notes | addons | keys | servers` in `ui/settings/gamepad.txt` opens that page once.

## 5. Lua API (`hs.gamepad`)

The contract between the C++ side and `ui/gamepad`. Button names are `a b x y lb rb lt rt back start guide
ls rs up down left right touchpad`. Axis names are `lx ly rx ry lt rt` (-1..1 after the dead zone, triggers 0..1).

| Call | Returns |
|---|---|
| `hs.gamepad.connected()` | bool, plus the pad's name |
| `hs.gamepad.type()` | `"xbox"`, `"playstation"`, `"nintendo"` or `"generic"` |
| `hs.gamepad.down(b)` / `pressed(b)` / `released(b)` | bool: held / went down this frame / went up this frame |
| `hs.gamepad.axis(a)` | number |
| `hs.gamepad.mode()` / `set_mode(m)` | `"game"`, `"menu"` or `"cursor"` |
| `hs.gamepad.cross_set()` | 0 none, 1 LT, 2 RT, 3 LT+RT: the set being held |
| `hs.gamepad.cross_slot(set, slot)` | bar, button (1-based) of a cross hotbar slot (set 1..3, slot 1..8); nil out of range |
| `hs.gamepad.set_cross_slot(set, slot, bar, button)` | bool: the slot set and saved (`gamepad.cross.*`) |
| `hs.gamepad.glyph(b)` | the label for the connected pad: `"A"`/`"Cross"`/`"B"` ..., `"LB"`/`"L1"`/`"L"` ... |
| `hs.gamepad.binding(input)` / `set_binding(input, action)` | the action string for an input such as `"x"` or `"lt+a"` (note 1) |
| `hs.gamepad.setting(name)` / `set_setting(name, value)` | the `gamepad.*` numbers above, by name (note 2) |
| `hs.gamepad.rumble(low, high, ms)` | bool (note 3) |
| `hs.gamepad.request_focus(window)` | puts the pad in menu mode with ImGui's focus on that Lua window (note 4) |
| `hs.gamepad.drop_cursor()` | bool: drops the cursor item on the ground (note 5) |
| `hs.gamepad.takes_b()` | a UI window will close itself on B this frame (game mode) (note 6) |

Notes:

1. `binding` gives `""` for a chord with no binding of its own (its cross slot applies), and nil for an unknown
   input. `set_binding` returns bool; `""` or nil puts the default back.
2. With or without the `gamepad.` prefix; booleans 0 / 1. `set_setting` also takes true / false, clamps, saves and
   returns bool (false for NaN or infinity). Both give nil for an unknown name. A NaN or infinite value in
   `hailstone.ini` loads as the default.
3. Strengths are 0..1, clamped; `ms` is at most 5000. It returns false without a pad, without rumble, with
   `gamepad.rumble = 0`, or for a NaN or infinite strength.
4. Named by its `ui.Begin` title; focus moves from the next frame once the window exists. As with `set_mode`, a
   plugin needs `commands` (docs/plugins.md "What plugin windows may do"), else its window does not take the focus.
5. As a click on the world with it (`request_drop_cursor`, with the game's confirmation); false when nothing is held.
6. The client's B then leaves the target alone, as Escape closes only the top window.

Glyphs:

- Xbox: `A B X Y LB RB LT RT View Menu Guide LS RS`;
- PlayStation: `Cross Circle Square Triangle L1 R1 L2 R2 Share Options PS L3 R3`;
- Nintendo: `B A Y X L R ZL ZR - + Home LS RS` (its bottom face button is B);
- others: `Back` / `Start`;
- the D-pad is `Up Down Left Right`.

The pad's X is kept from ImGui's navigation (ImGuiLayer::new_frame), whose window switcher it would open; windows
read X from `hs.gamepad`. Y is ImGui's "input" key, which presses the focused button as A does; the pack's buttons
ignore a press made with Y down (`pad.yActivated`).

Navigation calls the gamepad windows use: `ui.IsItemFocused()`,
`ui.SetWindowFocus(name)`, `ui.IsWindowAppearing()`, `ui.GetIO().nav_active` and `ui.GetIO().nav_visible`.

## 6. The gamepad UI pack (`ui/gamepad`)

`ui.ini`: `name=Gamepad`, `input=gamepad,mouse`, `game_art=icons` (only the item, spell and gem icon sheets). It is
built for 10-foot viewing: a base font of at least 20 px at 1080p, targets of at least 48 px, every action reachable
from the pad.

Windows the pack lacks fall back to the C++ windows (`lua::replaces`), used in cursor mode.

### HUD

Always on, with no focus:

- player HP, mana and endurance, pulsing red while auto attack is on. No mouse input, except in a group its name
  row: a click targets the player, a right click opens the group role menu;
- the target with its con colour and distance;
- the group, the pet, the casting bar, short buff rows, the zone and the compass.

### Minimap and map

**Minimap** (top right, a client extension): the zone map in a circle around the player (`hs.DrawMinimap`), north
up or turning with the player (right click), zone name above, clock below, + / - zoom (or the wheel). A click opens
the map.

**Map** (stands in for MapViewWnd): the zone fitted to a large window (`hs.DrawZoneMap`), north up, with labels,
group, the /find path and the player. It follows the player until panned (stick or drag). LB / RB zoom, Y recentres,
X toggles labels, RT toggles Auto Z (`hs.MapAutoZ`). Use the D-pad to focus **Map Pack**, A to open it or select a
pack, and B to dismiss the dropdown.
Reopening the map keeps its zoom and centre until zoning or logging out.

Both draw the map window's map (same files, layers, height filter and Auto Z) with dark line colours lightened.
Neither shows other players or NPCs, as the game's map does not.

### Cross hotbar

Bottom centre: each set as two diamonds (D-pad, face buttons) in the pad's glyphs, the held set enlarged and the
others dimmed. Each slot shows its icon, cooldown and usability.

### Group roles

group.lua, lib/role_icons.lua. The Group window (main menu, Group) lists the player, then the members, with icons
for the roles the server confirmed.

- Y on a row opens the member's role menu (Main Tank, Main Assist, Puller), as a right click on a name does
  (docs/dev-ui.md). A toggles a role, B closes; rows are greyed for all but the leader.
- The HUD's group rows and player name open it with a right click too.

### Main menu

Start opens `mainmenu.lua`, a tiled menu. Each tile opens the gamepad window, or the C++ window in cursor mode where
there is none:

- Inventory, Spells, Abilities, AA, Skills, Map, Group, Pet, Raid, Guild, Friends, Who, Bazaar, Quests, Mail, Chat,
  Options;
- All Windows: every window of the EQ menu;
- More (`system.lua`), the game's other windows: help, a bug report, the storyline, the NPC journal, adventures,
  expeditions, PvP, the zone guide, alerts, trophies, the display and the user interface, the notes, the Lua
  scripts, the web browser, and Tracking for a character with the skill (`hs.UseSkill(53)`);
- Hot Bars and Camp.

### Gamepad windows

**Inventory and equipment**: a slot grid. A picks up and moves an item, Y shows its details.

- Holding an item: X puts it away, RB drops it, LB destroys it with the game's confirmation (with Fast Item Destroy
  on, the window's own Yes / No comes first, since LB pages elsewhere).
- With nothing held, LB puts the focused item on a hot button through the slot picker, as a link that follows it.
- Alternate currencies under the coins: A claims one onto the cursor, A holding its item reclaims it. All are listed
  while something is held.

**Spell gems and the spell book**: Y in the book opens the spell sets; A loads one and X edits it.

- The editor fills a set's twelve slots from the whole book, LB / RB its pages, without memorizing anything.
- A on a spell then A on a slot, or A on an empty slot then the spell. X empties a slot, Y saves.
- Testing: `test.book.view=sets|edit<n>|editnew`.

**Loot**: A loots the item, X takes it, Y its details, LB Loot All, RB Link All (every item's link in the chat input).

**Item details**: X links the item in the chat input, as a click on its icon does; B closes.

**Merchant**.

**The chat log**: read only, with a quick-chat list and an on-screen keyboard.

- Keys: digits, letters, `' , . ?` and a row of `/ # @ % : - ! ( ) +`. Shift gives capitals and
  `! @ # $ % ^ & * ( )` over the digits. The windows' smaller keyboards (lib/keyboard.lua) match.

**Confirmation dialogs**: A / B; the focus starts on No.

**Options > Gamepad**: the bindings, dead zone, camera speed and inversion.

### Raid

raid.lua. Right-click a member, press Y on the row, or use Member actions for the shared member menu (Main Assist,
Mark NPC delegation, Find PC, leadership and looter actions); A chooses, B closes. Group picker, tell and note
controls sit beside the roster, raid-wide controls (Invite Expedition, Invite Task among them) above.

### Abilities, skills and AAs

abilities.lua, skills.lua, aa.lua.

- **Active discipline**: above the list, remaining seconds and a draining bar. Its icon or LB ends it; the icon is
  out of pad navigation, so A never ends it.
- **Cooldowns**: a faded red reuse fill and countdown, on every entry sharing the timer.
- **The Actions window's four pages** (LB / RB). Y chooses an ability button's skill.
- **The combat abilities** (disciplines): icon, level, reuse time left and the spell's description.
- **The skills**: value / cap, the usable ones used with A, and their reuse time.
- **The AA window**: its tabs, Train with the game's confirmation, Use for an owned ability with a reuse time, the
  "Can purchase" filter and the AA experience percentage.

**The slot picker** (slotpicker.lua): X on an entry assigns it. LB / RB step through the three cross sets and ten hot
bars; A drops the hotbutton there, as a drag does.

The main menu's **Hot Bars** opens the picker holding nothing:

- A opens the slot's hotbutton menu (as the right button held on a hot button): Assign Social (a new one opens the
  social editor), Assign Label, Assign Icon, Change Size, Clear, Activate, Memorize ...
- Y on a bar's page opens the bar menu: hotbutton sets, Clear current hotbuttons, Button Size, Show Key Map, Show
  Spinner, Open New Bar.

### Quests, mail, tradeskills, tribute

- **The quest journal**: tasks, objectives with progress, time left, shared task members, history. X twice abandons
  the focused task.
- **A task giver's offers**: A accepts, X declines.
- **The mailbox**: inbox and trash, read, delete, reply, mailbox switch, and composing with the on-screen keyboard
  (`lib/keyfield.lua`).
- **The tradeskill window**: recipes, search, favourites, components carried, how many you can make, Combine and
  Combine All / Stop; a container's slots beside the bags.
- **The tribute master** (benefits, modify / upgrade / downgrade / save, donations) and the tribute benefits window.

### Dialogs, readers and pickers

dialogs.lua, reading.lua, reports.lua, colors.lua, appearance.lua, title.lua, iconselection.lua. Their text goes
through `lib/stock.lua`'s `G.edit` on the on-screen keys.

- The text prompt, the name changes (character, pet, mercenary), the map label and the alarm.
- The context tip, notes, books (LB / RB turn the pages) and spell displays (the right stick scrolls them).
- The bug report, feedback, veteran reward claims and the respawn choices.
- The colour picker and armour dye: the D-pad picks a channel and changes it.
- Set appearance: the D-pad steps the features.
- The item preview: the right stick turns it, LB / RB zoom.
- Titles: LB / RB titles or suffixes, X uses, Y removes.
- The hot button icon picker: the D-pad moves through the icons, LB / RB pages, LT the type, RT the page size, X
  types the number.

Testing: `test.show = Screen[@seconds],...` in `ui/settings/gamepad.txt` shows those stock windows after zoning in
(`spell:<id>` a spell display; `read:<slot>[.<bag index>]` right-clicks that slot to read it).

### Adventures, expeditions, PvP

adventure.lua, expedition.lua.

- **An adventure recruiter's window**: text, the current adventure's time and progress, kind and risk, Request,
  Accept / Decline, Leave (asked first), View Stats.
- The adventure record and leaderboard, and the PvP record and leaderboard (X refreshes).
- **The expedition**: members, Quit (asked first), the leader's Add / Remove / Make Leader / Invite Raid / Invite
  Task, the lockouts.
- The choice of expeditions a door leads into.

### Help, story, journal, the zone guide

help.lua, journal.lua, navigation.lua, trophy.lua.

- **The help pages**, their links as a list: A follows one, LB Back, X Contents.
- The storyline and the alert history.
- **The NPC journal**: NPCs and their lines, X adds to the selection, Search, category filter and entry category
  (typed on the keys), Delete (asked first), Save.
- Find and Zone Path.
- **The tribute trophies**: LB / RB Personal / Guild, X / Y upgrade / downgrade.

### Display, layout

system.lua: Video Modes, Load Skin and Copy Layout, over the stock windows' children through `lib/model.lua` (a combo
box's choices as a list A picks from).

Testing: `test.show=<screen>,...` in `ui/settings/gamepad.txt` shows those stock windows once, `test.show.after`
seconds (default 6) after zoning in.

### More of the HUD

hudextras.lua, hotbars.lua. None takes the pad's focus, and B leaves them up.

- The air meter while short of air.
- The aggro meter under the target.
- The potion belt (bottom right).
- The cursor's item: at the pointer, or in menu mode a "Holding ..." card.
- Hot bars 2..10 when opened, as rows above the cross hotbar, for the mouse or cursor mode; the main menu's Hot Bars
  edits them with the pad.

A click on a bar's page number makes it a column left of the minimap with its pager on top, as the game's bar laid
out taller than wide (CHotButtonWnd::OnCompleteMoveOrResize); `hotbar.vertical.<window>` in ui/settings/gamepad.txt.

### Social and other windows

These stand in for their stock windows child by child (lib/child.lua): check boxes as toggles, drop-downs as lists,
edit boxes on the on-screen keys, lists as W.list.

- Inspect, leadership and group mentoring, Looking for Guild (LB / RB pages).
- **The social editor**: each line shows its command's kind; Wait after picks its /pause. More has the cooldown,
  Copy / Paste share text and Edit as Lua. Test saves and presses it (Stop while running); Hot bar uses the slot
  picker.
- **Hailstone's Lua social editor**: the code as lines (A types one with a row of code keys, X adds, Y deletes), or
  the text editor for a real keyboard.
- **The address book and the ignore list** (from the mail windows' Address Book): X adds a name to an open message's
  To.
- Raid options, the combat abilities list (Make Hotkey goes through the slot picker), group search and its filters,
  the fellowship, the bandolier and blocked spells (yours and your pet's).

Testing: `test.show = <ScreenID>,...` in ui/settings/gamepad.txt shows those stock windows 10 s after zoning in;
`test.luasocial = <n>` opens the Lua social editor on social n.

### Markets and services

bazaar.lua, barter.lua, pointmerchant.lua, mercenary.lua, guildbank.lua, train.lua, tracking.lua; shared pieces in
`lib/markets.lua`.

- **The bazaar trader**: wares as slots, the price as coin buttons, begin / end trading, add / remove, the activity
  text.
- **The bazaar search** (the main menu's **Bazaar** tile): filters as keyboard fields and pickers; A buys, Y inspects;
  the purchase confirmation's focus starts on Cancel.
- **The barter buyer** (item, offer and buy-line pages), a buyer's window and the barter search.
- **The point and adventure merchants**: buy, Y inspects a good, and a sell page with your bags.
- The mercenary merchant, manager and info.
- **The guild bank**: bank and deposits pages, with your bags to deposit from.
- **A guildmaster's training**: X trains, Y changes the order.
- **Tracking**: con filters, options; A tracks.

Testing: `test.show=<screen>,...` shows those stock windows ten seconds in. `test.draw=<screen>,...` draws the pack's
window for windows only an NPC opens (empty), one at a time with `test.draw.every=<seconds>`.

### The notes

playernotes.lua; /note, or the main menu's More. The right stick or LB / RB scrolls, Edit is on the keys, Add makes
a dated note as /note does.

A server plugin may bring gamepad windows of its own (its `ui/gamepad/`, documented in its own repository). Its
entries join More through `hs.PluginMenu` (docs/plugins.md "Plugin windows").

### Colour palettes

`lib/palettes.lua`; Options > Interface > Colour palette, the setting `palette` in `ui/settings/gamepad.txt`.

The same eight palettes as Modern (docs/dev-ui.md "Colour palettes"), with this pack's tokens (window, raised,
sunken, text, dim, accent, onAccent, ink, chatBg, chatText, coins, ...). `lib/theme.lua` puts the pick into `T.C` in
place. Each keeps the WCAG checks in the file (`tests/lua_scripts_test.cpp`). The pad's glyphs keep their dark face
on every palette. Testing: `test.palette.cycle=<seconds>`.
