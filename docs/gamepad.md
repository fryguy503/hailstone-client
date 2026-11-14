# Gamepad support and the gamepad UI

Hailstone can be played with a gamepad (an Xbox, PlayStation, Nintendo or generic SDL3 gamepad) through
`ui/gamepad`, a UI pack built for it (`input=gamepad`). This is a **client extension**: RoF2 has no gamepad
scheme to follow. The scheme only *presses the client's key commands* (game/keymap.h, `KeyCommand`), so every
game rule stays the game's. Movement, targeting, hot buttons and attack go through the same `key_command` /
`movement_.command` paths as the keys do. The stick gives the digital forward / back / strafe commands, because
the keyboard commands have no analog speed.

The mouse and keyboard keep working alongside the pad, and with any UI pack. The gamepad pack is the one whose
windows are laid out for the pad: large text and targets, navigation by focus instead of a pointer, and
no window that needs typing to work.

## 1. Layers

| Layer | Where | What it does |
|---|---|---|
| Device | `platform/window.*` (SDL_INIT_GAMEPAD) | opens and closes pads on hot-plug and fills `InputState::pad` (buttons, axes, type) once per frame |
| Bindings | `game/gamepad_bindings.*` (pure, unit-tested) | maps pad buttons and chords to key commands, or to the pad's own actions (cross hotbar slot, mode switch, camera) |
| App | `app/client_app_gamepad.cpp` | in game mode, turns the bindings into key events, movement and camera; in menu and cursor modes, hands the pad to the windows |
| ImGui | `ui/imgui_layer.cpp` | `NavEnableGamepad`; menu mode lets the SDL3 backend drive ImGui navigation |
| Lua | `app/lua_eq_gamepad.cpp` (`eq.gamepad`), `ui/lua/lua_imgui.cpp` (navigation calls) | gives the windows the pad's state, its mode and its glyphs |
| UI pack | `ui/gamepad/` | the HUD, the cross hotbar, the main menu and the gamepad windows |

## 2. Modes

The pad is in one of three modes at a time. `eq.gamepad.mode()` reports it.

- **game**: the pad plays: it moves, turns the camera, targets and fires hot buttons. ImGui gets no pad input.
- **menu**: a gamepad window has the focus. The pad drives ImGui navigation (D-pad or left stick moves the focus,
  A activates, B goes back or closes, LB / RB switch tabs) and the game gets nothing from it. Opening the main
  menu (Start) or any gamepad window that asks for the focus enters menu mode. Closing the last one returns to
  game mode. A mouse click elsewhere takes ImGui's focus but not the pad: menu mode lasts while such a window is
  open, and the pad's next button gives the newest one its focus back. Windows that ask in the same frame all count,
  the last on top; when the top one closes (a Tip over the merchant), the one under it takes the focus at once. Windows the game restores when zoning in
  do not ask for the pad, so the pad starts out playing. The pad's items must be built with
  `ui.ButtonFlags.EnableNav`: ImGui 1.91 leaves invisible buttons out of navigation.
- **cursor**: for windows that are not built for the pad (the temporary C++ windows). The left
  stick moves the mouse pointer and the right stick scrolls. A is a left click, X a right click, B leaves cursor
  mode. Back toggles cursor mode from game mode. The real mouse moving or clicking ends it (the mouse takes over), and
  the main menu enters it only for a tile chosen with the pad.

The pad's commands act even while a window has the keyboard (typing in the chat): the pad is not the keyboard.

With the interface locked (/safelock) the pad loses what the mouse and the locked keys lose (__ProcessMouseEvents,
__ExecuteCmd): its key commands go through the keys' refusal, its hot buttons, interact, cancel, targeting and camera
do nothing, menu and cursor modes fall back to game mode, and `eq.gamepad.down` / `pressed` / `released` answer false,
all except for the chat input's windows (`pad_chat_window` in app/gamepad.h: the chat key still opens it, and its
on-screen keyboard works). So a pad alone unlocks it: Back + Start, then `/safelock <password>` typed on the keys (or
the quick-chat list's "/safelock ...", which opens the keys with "/safelock " typed).

Testing: `--input-script` (app/input_script.h) plays a virtual Xbox pad's buttons and sticks, keys, typed text and
mouse moves and clicks at set times after zoning in (or, for a time written `c<seconds>`, after the character select screen
appeared: `3.5 text /camp; c3 pad start` camps and enters the world again; `d<seconds>`, after a link death's disconnected
screen appeared). It also takes screenshots, logs the pad's mode and ImGui's focus, sends the window's close request
(`close`) and silences the links (`droplink`, a link death), so all of the above can be checked without a pad.

## 3. Default bindings (game mode)

Button names use Xbox labels. The UI shows each pad's own glyphs (`eq.gamepad.glyph`).

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

The **cross hotbar** has 3 sets of 8 slots: D-pad left, up, right and down, then X, Y, B and A. By default the
sets are hot bar 1 buttons 1–8 (LT), hot bar 2 buttons 1–8 (RT) and hot bar 3 buttons 1–8 (LT+RT). A slot fires
`ActionsUi::hot_button(bar, button)`, as a hot button key does. So the slots use the hot buttons the player already
set up, and the hot bar windows show and edit the same buttons.

A trigger counts as held past 0.5. The stick dead zone is 0.2 (radial). Every value is a setting.

## 4. Settings

The bindings are client-wide, in `hailstone.ini` (like the window and display settings):

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

`<action>` is a `KeyCommand` name (`game::command_by_name`; the `CMD_` prefix may be left out or added, so
`CMD_FORWARD` and `FORWARD` are the same), `hot:<bar>:<button>`, `none`, or a pad action (`menu`, `cursor`,
`camera_toggle`, `camera_iso` (the isometric camera on or off, docs/isometric.md), `interact`, `cancel`, `target_prev`, `target_next`, `target_group` (D-pad up's self and group
cycle), `hotpage_prev`, `hotpage_next` (D-pad left / right), `chat`). `<input>` is a button name, a trigger chord
(`lt+a`, `rt+up`, `lt+rt+x`) or `back+start`. Only the bindings that differ from the defaults are written. A chord
with no binding of its own gives its cross hotbar slot (D-pad and face buttons) or the plain button's action. The
gamepad pack's Options page edits them; the numbers are also Options window options (`eq.Option` /
`eq.SetOption`): `gamepad_<setting>` in whole steps (the thresholds and dead zone in percent, the camera speed in
tenths, booleans 0 / 1).

Interact (A) uses the target's right-click action when it is within 15 units, else USE on what is at the middle
of the screen (else the nearest door in front).

Outside the world (login, server select, character select and creation) the pad drives ImGui's navigation of the
screens in game mode too, and Back still gives cursor mode. The screens show the pad's buttons at the bottom
(`ui::set_screen_gamepad`). The navigation cursor starts on the first empty login field or on the picked server or
character, and the picked row follows it. A on a row plays that server or enters the world with that character.
Start does the screen's main action (Login, Play, Enter World, Create), and B goes back (not on the login screen).
A text field activated with the pad (A, or Y) opens an on-screen keyboard: A types the key under the cursor, X
deletes, Y types a space, LB is shift, Start or Done accepts, and B cancels. On the login screen, Start with a field
still empty opens the keyboard on it.

With the Gamepad UI the login, server select, character select and character creation screens are the pack's own
(`ui/gamepad/login.lua`, `serverselect.lua`, `charselect.lua`, `charcreate.lua` over `eq.LoginScreen` and the rest,
docs/dev-ui.md); the above is the client's
ImGui screens, which show with a pack that does not draw them. The pack's login starts on the character tiles of
Continue as (A plays one; one with no saved password asks for it first), LB / RB change the server, Y opens More
options (the box set, game files, the add-ons, About Hailstone with What's new, music, Quit), Start logs in (or opens the keys on the first empty field) and B goes
back from a page. At the server select A plays the focused world, Y marks it a favourite (or unmarks it) and B goes back; at character select the focused
character is the one the scene shows, A or Start enters the world, X creates, Y deletes (asked first) and B goes back.
At character creation the rows are stepped in place (gender, race, class, deity, start city: D-pad left / right, or
A); A on the name opens the keys, X asks the server for a random name, A on Stats opens a row per stat (left / right
takes a point back or spends one, X spends all it can take, LB resets them), Y opens Appearance, Start creates once
every point is spent and the name has 4 letters or more, and B cancels. The box under the rows shows the game's
description of the race, class, deity, city or stat focused.
Testing: `test.login = server | more | notes | addons | keys | servers` in `ui/settings/gamepad.txt` opens that page once.

## 5. Lua API (`eq.gamepad`)

This is the contract between the C++ side and `ui/gamepad`. Button names are `a b x y lb rb lt rt back start guide
ls rs up down left right touchpad`. Axis names are `lx ly rx ry lt rt` (-1..1 after the dead zone, triggers 0..1).

| Call | Returns |
|---|---|
| `eq.gamepad.connected()` | bool, plus the pad's name |
| `eq.gamepad.type()` | `"xbox"`, `"playstation"`, `"nintendo"` or `"generic"` |
| `eq.gamepad.down(b)` / `pressed(b)` / `released(b)` | bool: held / went down this frame / went up this frame |
| `eq.gamepad.axis(a)` | number |
| `eq.gamepad.mode()` / `set_mode(m)` | `"game"`, `"menu"` or `"cursor"` |
| `eq.gamepad.cross_set()` | 0 none, 1 LT, 2 RT, 3 LT+RT: the set being held |
| `eq.gamepad.cross_slot(set, slot)` | bar, button (1-based) of a cross hotbar slot (set 1..3, slot 1..8); nil out of range |
| `eq.gamepad.set_cross_slot(set, slot, bar, button)` | bool: the slot set and saved (`gamepad.cross.*`) |
| `eq.gamepad.glyph(b)` | the label for the connected pad: `"A"`/`"Cross"`/`"B"` ..., `"LB"`/`"L1"`/`"L"` ... |
| `eq.gamepad.binding(input)` / `set_binding(input, action)` | the action string for an input such as `"x"` or `"lt+a"`: `""` for a chord with no binding of its own (its cross slot applies), nil for an unknown input; `set_binding` returns bool, and `""` or nil puts the default back |
| `eq.gamepad.setting(name)` / `set_setting(name, value)` | the `gamepad.*` numbers above, by name with or without the `gamepad.` prefix (booleans 0 / 1; `set_setting` also takes true / false, clamps, saves and returns bool); nil for an unknown name |
| `eq.gamepad.rumble(low, high, ms)` | bool (0..1 strengths); false without a pad, rumble or with `gamepad.rumble = 0` |
| `eq.gamepad.request_focus(window)` | puts the pad in menu mode with ImGui's focus on that Lua window (its `ui.Begin` title), from the next frame once the window exists |
| `eq.gamepad.drop_cursor()` | bool: drops the cursor item on the ground as a click on the world with it does (`request_drop_cursor`, its confirmation when the game asks); false when nothing is held. The pad has no pointer to click the world with |
| `eq.gamepad.takes_b()` | a UI window will close itself on B this frame (game mode), so the client's B (stop casting, close the top window, clear the target) leaves the target alone, as Escape closes only the top window |

Glyphs: Xbox `A B X Y LB RB LT RT View Menu Guide LS RS`, PlayStation `Cross Circle Square Triangle L1 R1 L2 R2
Share Options PS L3 R3`, Nintendo `B A Y X L R ZL ZR - + Home LS RS` (its bottom face button is B), others
`Back` / `Start`; the D-pad is `Up Down Left Right`. Menu mode also ends by itself when no window has had the focus
for a few frames (the last gamepad window closed).

The pad's X is locked away from ImGui's navigation (ImGuiLayer::new_frame), which would otherwise open its window
switcher while X is held: the windows read X from `eq.gamepad`. Y is ImGui's "input" key, which presses the focused
button as A does; the pack's buttons ignore a press made with Y down (`pad.yActivated`), as Y has its own meaning.

The `ui` table adds the navigation calls that the gamepad windows need: `ui.IsItemFocused()`,
`ui.SetWindowFocus(name)`, `ui.IsWindowAppearing()`, `ui.GetIO().nav_active` and `ui.GetIO().nav_visible`.

## 6. The gamepad UI pack (`ui/gamepad`)

`ui.ini`: `name=Gamepad`, `input=gamepad,mouse`, `game_art=icons`. It uses no game UI art, like the other packs:
it draws shapes and uses only the item, spell and gem icon sheets. It is designed for 10-foot viewing: a base font
of at least 20 px at 1080p, targets of at least 48 px, and every action reachable from the pad.

- **HUD** (always on, no focus): player HP, mana and endurance (the panel pulses red while auto attack is on); the
  target with its con colour and distance; the group; the pet; the casting bar; short buff rows; the zone and the compass.
- **Minimap** (top right, a client extension; the game has only the map window): the zone map in a circle
  around the player (`eq.DrawMinimap`), north up or turning with the player (right click), with the zone name
  above, the game clock below, and + / - zoom buttons (or the wheel). A click opens the large map.
- **Map** (stands in for MapViewWnd): the zone fitted to a large window (`eq.DrawZoneMap`), north up, with the
  labels, the group, the /find path and the player. It follows the player until panned with a stick or by
  dragging. LB / RB zoom, Y centres on the player again, X shows or hides the labels, and RT turns Auto Z on or off
  (what is well above or below the player fades; `eq.MapAutoZ`). Both maps draw the map window's map (the same
  files, layers, height filter and Auto Z), and lighten its dark line colours for the dark
  background. Neither shows other players or NPCs, because the game's map does not.
- **Cross hotbar** (bottom centre): the three sets as two diamonds (D-pad, face buttons) each, shown in the pad's
  glyphs. The held set is enlarged and the others dimmed. Each slot shows its icon, its cooldown and whether it is
  usable.
- **Main menu** (Start, `mainmenu.lua`): a tiled, focus-driven menu with Inventory, Spells, Abilities, AA, Skills,
  Map, Group, Pet, Raid, Guild, Friends, Who, Bazaar, Quests, Mail, Chat, Options, All Windows (every window of the
  EQ menu), More (`system.lua`, the game's other windows: help, a bug report, the storyline, the NPC journal,
  adventures, expeditions, PvP, the zone guide, alerts, trophies, the display and the user interface, the notes, the
  Lua scripts, the web browser; Tracking for a character with the Tracking skill, which uses the skill as its hot
  button does (`eq.UseSkill(53)`: the skill opens the tracking window)), Hot Bars and Camp. Each tile opens the gamepad window, or, where there is none
  yet, the C++ window in cursor mode.
- **Gamepad windows**: inventory and equipment (a slot grid, A picks up and moves an item, Y shows its details;
  holding an item, X puts it away, LB destroys it and RB drops it on the ground; with nothing held LB puts the focused
  item on a hot button through the slot picker, a link that follows the item wherever it is moved; under the coins the alternate
  currencies: A claims one onto the cursor as items, and A holding its item reclaims it, every currency listed
  while something is held),
  spell gems and the spell book (Y in the book: the spell sets, A loads one and X edits it; the editor fills a
  set's twelve slots from the whole book, LB / RB its pages, without memorizing anything: A on a spell then A on a
  slot, or A on an empty slot then the spell; X empties a slot, Y saves; `test.book.view=sets|edit<n>|editnew`),
  loot (A loots the item, X takes it, Y its details, LB Loot All, RB Link All: every
  item's link in the chat input), item details (X links the item in the chat input, as a click on its icon
  does; B closes), merchant, the chat log (read only, with a quick-chat list and an on-screen
  keyboard: digits, letters, `' , . ?` and a row of `/ # @ % : - ! ( ) +` for slash commands, #commands, %t and the
  like; shift gives capitals and the keys' second characters, `! @ # $ % ^ & * ( )` over the digits; the windows'
  smaller keyboards, lib/keyboard.lua, have the same keys), confirmation dialogs (A / B), and Options > Gamepad (the bindings, dead zone, camera speed and
  inversion).
- **Abilities, skills and AAs** (abilities.lua, skills.lua, aa.lua): the active discipline above the combat ability
  list shows its remaining duration to the second and a draining bar; click its icon or select it with A to end it.
  Cooling entries have a faded red reuse fill and a seconds countdown, including every entry sharing the same timer.
  The Actions window's four pages (LB / RB; Y
  chooses an ability button's skill), the combat abilities (disciplines: icon, level, reuse time left and the
  spell's description), the skills (value / cap, the usable ones used with A and their reuse time) and the AA
  window (its tabs, Train with the game's confirmation, Use for an owned ability with a reuse time, the "Can
  purchase" filter and the AA experience percentage). X on an entry assigns it to a slot (slotpicker.lua): LB / RB
  go through the cross hotbar's three sets and the ten hot bars, A drops the entry's hotbutton there, as dragging it
  onto the bar does. The main menu's **Hot Bars** opens the same picker holding nothing: A opens the slot's own menu
  (the game's hotbutton menu, as the right button held on a hot button opens it: Assign Social, where a new social
  opens the social editor, Assign Label, Assign Icon, Change Size, Clear, Activate, Memorize ...); on a hot bar's page Y
  opens that bar's own menu (hotbutton sets, Clear current hotbuttons, Button Size, Show Key Map, Show Spinner, Open
  New Bar).
- **Quests, mail, tradeskills, tribute**: the quest journal (tasks, objectives with progress, time left, shared
  task members, history; X twice abandons the focused task) and a task giver's offers (A accepts, X declines); the
  mailbox (inbox and trash, read, delete, reply, mailbox switch) and composing with the on-screen keyboard
  (`lib/keyfield.lua`); the tradeskill window (recipes, search, favourites, components with what you carry of each, how many you can
  make, Combine and Combine All / Stop) and a combine
  container's slots beside the bags; the tribute master (benefits, modify / upgrade / downgrade / save, donations)
  and the tribute benefits window.
- **Dialogs, readers and pickers** (dialogs.lua, reading.lua, reports.lua, colors.lua, appearance.lua, title.lua,
  iconselection.lua; their text through `lib/stock.lua`'s `G.edit` on the on-screen keys): the text prompt, the
  name changes (character, pet, mercenary), the map label and the alarm; the context tip, notes, books (LB / RB
  turn the pages) and spell displays (the right stick scrolls them); the bug report, feedback, veteran reward
  claims and the respawn choices; the colour picker and armour dye (the D-pad picks a channel and changes it); set
  appearance (the D-pad steps the features), the item preview (the right stick turns it, LB / RB zoom), titles
  (LB / RB titles or suffixes, X uses, Y removes) and the hot button icon picker (the D-pad moves through the
  icons, LB / RB pages, LT the type, RT the page size, X types the number). Testing: `test.show =
  Screen[@seconds],...` in `ui/settings/gamepad.txt` shows those stock windows after zoning in (`spell:<id>` a
  spell's display, `read:<slot>[.<bag index>]` right-clicks that slot, as reading a note does).
- **Adventures, expeditions, PvP** (adventure.lua, expedition.lua): an adventure recruiter's window (the words, the
  time and progress of the current adventure, the kind and risk to ask for, Request, Accept / Decline, Leave asked
  first, View Stats), the adventure record and leaderboard, the PvP record and leaderboard (X refreshes), the
  expedition (members, Quit asked first, the leader's Add / Remove / Make Leader / Invite Raid / Invite Task, the
  lockouts) and the choice of expeditions a door leads into.
- **Help, story, journal, the zone guide** (help.lua, journal.lua, navigation.lua, trophy.lua): the help pages with
  their links as a list (A follows one, LB Back, X Contents), the storyline, the alert history, the NPC journal (the
  NPCs and what they said, X adds to the selection, Search, the category filter and the entries' category, Delete
  asked first, Save) and its categories (typed on the keys), Find and Zone Path, and the tribute trophies (LB / RB
  Personal / Guild, X / Y upgrade / downgrade).
- **Display, layout** (system.lua): Video Modes, Load Skin and Copy Layout. These windows draw over the stock windows'
  children through `lib/model.lua` (buttons, check boxes, lists in columns, a combo box's choices as a list A picks
  from), as the Classic UI's `lib/sidl.lua` did for the stock layouts. Testing: `test.show=<screen>,...` in
  `ui/settings/gamepad.txt` shows those stock windows once, `test.show.after` seconds (default 6) after zoning in.
- **More of the HUD** (hudextras.lua, hotbars.lua; none takes the pad's focus and B leaves them up): the air meter
  while short of air, the aggro meter under the target, the potion belt (bottom right), the cursor's item (at the
  pointer with the mouse; in menu mode, where the pad has no pointer, a "Holding ..." card) and hot bars 2..10 when opened, as rows above the cross hotbar (the mouse's,
  or the pad's in cursor mode; Hot Bars in the main menu edits them with the pad). A click on a bar's page number
  makes it vertical, a column on the right (left of the minimap) with its pager across the top, as the game's bar
  laid out taller than wide (CHotButtonWnd::OnCompleteMoveOrResize); `hotbar.vertical.<window>` in
  ui/settings/gamepad.txt.
- **Social and other windows** standing in for their stock windows child by child (lib/child.lua: buttons, check
  boxes as toggles, drop-downs as a list of their choices, edit boxes typed on the on-screen keyboard, lists as
  W.list): inspect, leadership and group mentoring, Looking for Guild (LB / RB pages), the social editor (each line's
  field says what its command is; Wait after picks its /pause; More: the cooldown, Copy / Paste share text, Edit as
  Lua; Test, which saves and presses it, Stop while it runs; Hot bar, through the slot picker) and
  Hailstone's Lua social editor (the code as lines: A types one with a row of code keys, X adds, Y deletes; or the
  text editor for a real keyboard), the address book and the ignore list (X adds a name to an open message's To;
  the mail windows' Address Book opens it), raid options, the combat abilities list (Make Hotkey goes through the
  slot picker), group search and its filters, the fellowship, the bandolier, blocked spells (yours and your pet's)
  and the NMS plugin's waypoint window. Testing: `test.show = <ScreenID>,...` in ui/settings/gamepad.txt shows those stock
  windows 10 s after zoning in, `test.luasocial = <n>` opens the Lua social editor on social n.
- **Markets and services** (bazaar.lua, barter.lua, pointmerchant.lua, mercenary.lua, guildbank.lua, train.lua,
  tracking.lua; shared pieces in `lib/markets.lua`): the bazaar trader (wares as slots, the price as coin buttons,
  begin / end trading, add / remove, the activity text), the bazaar search (filters as keyboard fields and pickers,
  results where A buys and Y inspects) and its purchase confirmation; the barter buyer (item, offer and buy-line
  pages), a buyer's window and the barter search; the point and adventure merchants (buy, Y inspects a good, and a sell
  page with your bags); the mercenary merchant, manager and info; the guild bank (bank and deposits pages, with your bags to
  deposit from); a guildmaster's training (X trains, Y changes the order); and tracking (con filters, options, A
  tracks). A stock edit box is a button that opens the on-screen keyboard, a drop-down a button that opens a list
  of its choices. The main menu's **Bazaar** tile opens the bazaar search. Testing: `test.show=<screen>,...`
  shows those stock windows ten seconds in; `test.draw=<screen>,...` draws the pack's window for windows only an
  NPC opens (empty), one at a time with `test.draw.every=<seconds>`.
- **The NMS server's windows and the notes** (the NMS plugin's `ui/gamepad/`, in its own repository: nmsloot.lua, nmsvault.lua,
  nmswaypoints.lua and a Pet window over the pack's lib/pet.lua, through
  `eq.Plugin("nms")`, the plugin's docs/nms.md; and the
  pack's playernotes.lua): Personal Loot, which takes the pad when loot
  needs a decision, on the first drop (LB / RB its tabs: Loot, Rules, History, Settings; on a drop A keeps it, X
  sells or destroys it, Y opens its menu: Always Keep / Always Sell now, a rule alone, inspect; RT loots all; the
  decided drop's place goes to the next, so A A A goes down the list; beside the list the selected drop's facts and
  the same actions as buttons, Sell All asked first; rules searched and added with the on-screen keys, A changes a
  rule, X removes it); the Dimensional Vault (its nine pages on LB / RB, A deposits the held item or withdraws, X
  withdraws one of a stack, Y opens a bag's contents below the page or inspects, your bags beside it to pick an
  item up, Bank and Merchant); and the notes (/note: the notes scrolled with the right stick or LB / RB, Edit
  on the keys, Add a dated note as /note does). The main menu's More opens the notes, and on the NMS server
  Personal Loot.
- Windows the pack does not have yet fall back to the C++ windows, as every pack's do (`lua::replaces`). They
  are used in cursor mode.
- **Colour palettes** (`lib/palettes.lua`, Options > Interface > Colour palette, the setting `palette` in
  `ui/settings/gamepad.txt`): Midnight (the default), Charcoal, Daylight (light), High Contrast and the accent
  palettes Ember, Verdant, Amethyst and Rose, as named tokens (window, raised, sunken, text, dim, accent, onAccent,
  ink for faint lines, chatBg and chatText for the chat over the world, the coins, ...). `lib/theme.lua` puts the
  pick into `T.C` in place, so every window follows at once; each palette keeps the WCAG contrast checks listed in
  the file (`tests/lua_scripts_test.cpp`). The pad's glyphs keep their dark face on every palette, as the
  pad's own buttons. Testing: `test.palette.cycle=<seconds>` shows each palette in turn (logged).
