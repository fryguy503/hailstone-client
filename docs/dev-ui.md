# Developer UI

Lua interfaces (Modern and Gamepad) use the versioned, modular `hs.UI` contract for game data and actions. See
[UI API](ui-api.md) for feature discovery, pack inheritance and the author workflow, and
[UI API Reference](ui-api-reference.md) for every canonical method. Flat host bindings are also under `hs`.

`ui.WindowFlags` exposes all 23 public window flags of the pinned Dear ImGui, including `HorizontalScrollbar`,
`AlwaysVerticalScrollbar` and `AlwaysHorizontalScrollbar`, with Dear ImGui's values in ordinary and child windows.
Internal and obsolete flags are not exported; use the window, popup or child functions for those scopes.

The client uses [Dear ImGui](https://github.com/ocornut/imgui) (v1.93.0, fetched by `cmake/Dependencies.cmake`) for a minimal developer UI: log in, pick a server and a character, see basic in-game information. The Lua windows below replace it for play.

## Pieces

- `src/ui/imgui_layer.{h,cpp}`: the ImGui context and the SDL3 and Vulkan backends (our volk loader, dynamic rendering, recorded inside the colour + depth rendering `VkContext::begin_frame` started). Fonts follow the display's content scale: Liberation Sans or another common system font, else ImGui's built-in one.
  The font atlas includes Latin and Unicode General Punctuation for both chat layouts. A face missing a curly quote
  draws the corresponding straight quote glyph. The displayed glyph choice leaves the original chat text intact for
  copying and logging.
- `src/ui/dev_screens.{h,cpp}`: the screens. Each takes a view model and returns the player's action; none does networking:
  - `login_screen`
  - `server_select_screen`
  - `character_select_screen`
  - `loading_screen`
  - `in_game_overlay`: info panel, chat and spawn list
- `src/ui/spell_windows.{h,cpp}`: the spell gem bar, cast bar, spell book, buff and song windows and the target's buffs, with icons from the game's `spells0N.tga` sheets (A_SpellIcons: 40 × 40 cells, 6 per row, 36 per sheet). `src/app/spell_ui.{h,cpp}` builds their view models from `net::GameConnection` each frame.
- `src/game/eq_names.{h,cpp}`: race and class names. Multiclass characters show as `SHD/CLR/WIZ`.
- `src/core/config.{h,cpp}` and `src/platform/paths.{h,cpp}`: the `key=value` settings file `hailstone.ini` in the Hailstone folder beside the binary (or `~/.config/hailstone`, `%APPDATA%\hailstone` or `~/Library/Application Support/hailstone` when that is not writable; `docs/layout.md`). It names the last server used (`login.profile`).
- `src/core/server_profile.{h,cpp}`: the saved servers, each a folder `servers/<id>/` with a `server.ini` (name, address, port, username).
  - New, Save and Delete edit them; Login saves the picked one.
  - The username is kept when "Remember username" is ticked. The password never is.
  - The Assets row shows the server's own files folder (`servers/<id>/assets`, Open folder button). Its files replace the asset folder's files of the same name for that server (`GameDir::set_overlay`, applied at Login).
  - The old `login.host`, `login.port` and `login.username` keys become the first saved server.
  - Test logins (`--user` with `--host`/`--port`) use a transient server that is never saved. `--password` is visible in the process list to every user of the computer: use it only for a test account. `--server <id or name>` picks a saved one.
- `src/app/ui_demo.{h,cpp}`: the demo below.

## Demo

```sh
build/hailstone/hailstone --ui-demo login           # start on the login screen
build/hailstone/hailstone --ui-demo game --zone qeynos2 # in-game overlay over the zone viewer
build/hailstone/hailstone --ui-demo chars --frames 20 --screenshot chars.png   # to screenshots/chars.png
build/hailstone/hailstone --ui-demo login --ui-scale 1.5 # override the display scale
```

The screens are `login`, `servers`, `chars`, `nochars`, `loading`, `game` and `items` (a fake inventory and corpse). F1 to F7 switch screens; the buttons walk a fake flow (any username and password log in); typed chat is echoed.

While the UI has keyboard or mouse focus, the camera ignores that input; holding the right mouse button still turns it.

Text editing, modal dialogs, active keyboard or gamepad controls, and explicit requests (`ui.CaptureKeyboard`) can take the keyboard. A click or drag on a button, slider, title bar or window background leaves the keyboard with the game through mouse release (`ui::keyboard_for_ui`), so held movement keys stay held, as in the original.

## Using it from an app loop

```cpp
ui::ImGuiLayer ui(window, vk);           // after VkContext
while (window.poll_events()) {
    ui.new_frame();
    if (auto a = ui::login_screen(login_view); a == ui::LoginAction::Submit) { /* start login */ }
    auto f = vk.begin_frame(clear);
    if (!f) { ui.end_frame_without_render(); continue; }
    /* draw the world */
    ui.render(*f);                       // last, before end_frame
    vk.end_frame(*f);
}
```

### Modern chat tabs

Right-click any tab or the chat output for the chat menu: create, rename and close tabs and windows, detach, move
or reorder a tab, and combine windows into the one you clicked. **Switch Tab** lists that window's tabs, **Chat
Windows** every window's IDs and tabs. The menu also has the tab's scrollbar and highlight settings, chat routing and
the **Window** layout menu. An overflow button lists all tabs when the strip fills.

Moving a tab keeps its filters, output and scroll position. Background tabs flash in their colour every half second
for five seconds, then stay highlighted until selected. The overflow button takes the highlight colour of a hidden
tab during its highlighted phase. **Highlight New Messages** defaults to on in red. Turning it off for a tab leaves
the strip and overflow button visually unchanged when that tab receives new messages. Each tab can hide its
scrollbar. Groups, names, order, selection and tab settings are saved per character. The native model is shared by
the Modern controls, [`hs.UI.chat`](ui-api.md#chat-tabs-and-windows) and [`/chattabs`](commands.md).

The menu's first rows are the original's, in its order (Window, Scrollbar, New Chat Window, Rename Window, Always
Chat Here, the tell menus, Filters, Channel, Language, Hit Modes); the tab rows, highlight settings, Font Size, Use
Tell Windows and Timestamps are Hailstone's and follow. Rename Window renames the frame, Rename Tab the chat window it
shows; both take 15 characters, as the original's. Checked: CChatWindowManager::InitContextMenu @0x6513c0.

The output's text can be selected with the mouse and copied, a Hailstone addition (`ui/modern/lib/textselect.lua`):
drag to select, double-click for a word, triple-click for the line, Shift+click to extend. A plain click clears it;
a click on a link follows the link, and Alt+drag still moves the frame. Dragging above or below the output scrolls it.
**Ctrl+C** copies while the chat window has the focus and its input does not (the input copies its own text), and the
menu's last row, **Copy**, does the same. The text is what the lines show, timestamps and link names included, one
chat line to a line. The selection follows its lines as new ones arrive, and goes when the tab changes, the window
is cleared or its lines have all scrolled out of the buffer. While a selection exists, the window has the focus and
Ctrl is held, the keys are the UI's, so a Ctrl key bind does not also fire.

Tabs are kept in the character's `UI.ini` `[ChatManager]` beside the original's keys: `ChatWindow<n>_Group` (the
frame, 1..32), `_TabOrder`, `_Highlight` and `_HighlightColor` per chat window, and `ChatGroup<g>_Selected` and
`_Name` per frame. An older file (`ChatWindow<n>_Dock` and `_Up`) loads as it was: a docked window becomes a tab of
its host's frame, the tab that was up selected. Tell tabs are not saved, as in the original.

### Extended chat channels

Channels 57..107 and user colours 355..408 (docs/mechanics.md "Extended chat routing") are Hailstone's, used only
when a server sends those colours. The gamepad pack has no filter menu.

- `hs.ChatFilterGroups()` gives the Filters menu's submenus as `{id, parent, name}` (parent 0 is the Filters menu):
  Chat Channel, Melee and Spells, then Extended (server) and its submenus.
- `hs.ChatFilters(slot)` adds `menu` and `parent` to `id`, `group`, `name` and `on`.
- `hs.ChatSetFilter` takes a channel or a menu's name.
- Each filter menu ends in the original's **ALL** and Hailstone's **Hide All**, for that menu's own channels only.

They are kept as `ChannelMap0..107` in the character's UI ini and `User_1..153` in its `chat.ini`. An older file
(ChannelMap0..56, User_1..99) loads with the new ones on the main window at their defaults.

## Quest Journal and Quest Tracker

#### The journal

The modern journal (`tasks.lua`) has Active and History pages. Active lists every current task, shared ones included, tracked quests first. The selected quest has Objectives, Description and Rewards:

- Objectives keep the native group/slot order, with every hidden `? ? ?` row and blank separator the task model supplies; no invented steps or dependencies.
- Completed quests keep the native history request.
- Description and reward links use the `TASK_Description` STML handler. Abandon keeps its native confirmation.
- Preview Reward opens the server-owned reward preview when the native task model enables it (read through `hs.ChildState` in both packs); otherwise it stays disabled.

#### Task offers

TaskSelectWnd has Preview Reward in both packs (`TSEL_PreviewRewardButton`, the offer's received preview flag).

- Opening or drawing offers requests no rewards. A preview leaves the offers open.
- Its Description has the native starting-zone hint from element slot 0 (eqstr 6029), not a reward summary.
- Selecting an offer without a preview hides one shown, as the native handler does. A replaced offer list starts at its first row.
- Accept and Preview Reward report disabled (`hs.ChildState`) until the offers are listed (`hs.ListInfo`).

#### Shared tasks

The header's Shared members control shows the roster count and opens a management popup over the native `STASK_MemberList` (Member, Role). Add target, Remove member, Make leader, Invite raid and Invite expedition use the native controls and their permission checks.

#### The tracker

Track puts a quest in `questtracker.lua`, a `hud.Window` independent of `TaskWnd`: a Hailstone presentation feature, with no extra server requests or rules.

- Each tracked quest shows up to three revealed, unfinished objectives in native order, with counts and locations.
- Its title and All objectives open that quest in the journal.
- It has the ordinary HUD move, resize, lock, fade and roll-up.
- Hiding it keeps the tracking flags; the journal's Show tracker brings it back. With nothing tracked it starts hidden unless shown.

#### Shared state and settings

Both windows share `lib/task_journal.lua` and `hs.Tasks()`. Building that is not cheap, so the module reads it again only when `hs.TasksRevision()` changes, after a pick, and twice a second for countdowns. A hidden tracker reads nothing.

A quest that leaves the list forgets its tracking, collapsed state and section; a key back at its default is dropped. `hs.OpenTask(id)` opens and selects an active task. Character settings: `tasks.tracked.*`, `tasks.tracker.*` (visibility, collapsed quests), `tasks.category`, `tasks.section.*`. The frame's geometry and lock are ordinary `hud.*` settings; Copy layout copies placement, not tracked IDs or sections.

#### Journal tests

Covered by packet-driven bridge tests and headless Dear ImGui/Lua tests. Not covered: quest updates across zoning, shared roster updates and membership actions live, and an in-game mouse resize.

#### Syncing Lua-only edits

The client target copies the UI packs after linking. For a Lua-only edit that does not relink, run `cmake/SyncUiPacks.cmake` with absolute `SRC` (source UI folder) and `DST` (built client's UI folder). It copies changed scripts and leaves pack settings alone.

## The Lua windows

### UI packs

The windows are Lua scripts drawn with ImGui, grouped into user interfaces: every folder of `ui/` with a `ui.ini` (`src/ui/ui_packs.h`).

- The build copies only changed scripts next to the executable and deletes removed ones (`cmake/SyncUiPacks.cmake`); scripts reload when they change.
- Each saved server keeps its own (`servers/<id>/server.ini` `[Server] ui`), picked on the login screen's Interface list or in Options > Display > Interface > User Interface, applied at once.
- `hailstone.ini` `ui.name` is the one for a new server or a test login (`modern` when missing).
- Script settings live in `ui/settings/<name>.txt` of the Hailstone folder, not in the pack, which an update replaces.

A pack's `ui.ini` holds:

- `name` and `description`, shown in Options;
- `input`: `mouse`, `gamepad` or `touch`;
- `game_art`: `icons` loads only the item, spell and gem icon sheets of the game's `uifiles/default` (`UiTextures::set_all_art`); `all` every file;
- `base`: a pack whose windows and helpers fill in what this one lacks (docs/ui-api.md).

Scripts `dofile` paths relative to their UI's folder (`"lib/theme.lua"`). A new UI is a folder with a `ui.ini` and window scripts over the same `ui.*` and `hs.*` API; it may copy `lib/` from modern or gamepad.

A server or `hailstone.ini` that names `classic` (the removed Classic UI) gets Modern (`ui::find_ui_pack`).

### Modern look and palettes

`ui/modern` is a modern HUD over the same `hs.*` answers and clicks, with no game UI art (`game_art=icons`): translucent rounded panels, eased bars with a trail for lost health, icon tiles. Colours and drawing pieces are in `lib/theme.lua`; `lib/style.lua` restyles ImGui's own pieces through `ui.Col` / `ui.StyleVar`, skipping names the client lacks.

#### Colour palettes

The palette is Options > Display > Interface > Colour Palette (`palette` in `ui/settings/modern.txt`), applied at once.

- `lib/palettes.lua`: Midnight (default), Charcoal, Daylight (light), High Contrast, and accents Ember, Verdant, Amethyst and Rose. Each is a set of named tokens (window, frame, frameBorder, text, muted, heading, accent, onAccent, ink, chat and bar text, coins, ...).
- `lib/theme.lua` fills `T.color` from it in place, so every window and `lib/style.lua` follows a change.
- Each palette passes the WCAG checks in the file (text 4.5:1, control outlines 3:1; `tests/lua_scripts_test.cpp`).
- Chat lines sit on the palette's `chatBg`, checked alone over a white or black world, since the chat panel fades while idle.
- Dialogs and Options outline fields, check boxes, combos and buttons in `frameBorder`; Options section headings use `heading`.
- Testing: `test.palette.cycle=<seconds>` shows each palette in turn.

### Modern frames and dialogs

#### Default layout

Frames (`lib/hud.lua` `hud.Window`) are borderless and show when the game shows the window they replace (`hs.ReplaceWindow`).

Until moved, a frame sits on the modern UI's default layout (`lib/layout.lua`), worked out again when the screen size, UI or HUD scale or the shown frames change (a frame shows, resizes, or has not been drawn for more than two frames):

- from the bottom middle up: hot bars, spell gems, player and target side by side, air bar;
- chat at the bottom left, as wide as the room beside them allows (the middle stack moves right on a narrow screen);
- compass at the top middle;
- minimap top right, buffs and songs to its left (wrapping up to the compass, under it where fewer than eight icons fit), auras and extended targets under them;
- group at the left, pet beside the player;
- menu button bottom right.

Each frame takes the first of its places clear of those before it, else the clear place beside the nearest frame. The casting bar, a target, the first buff and a full group keep their room while hidden, so one appearing moves nothing. A frame the layout does not know (a plugin's) keeps its `opts.place`. `tests/lua_scripts_test.cpp` checks 17 screen sizes at 7 scales: nothing off screen, nothing overlapping from 1000 x 600 design pixels (new character) and 1280 x 720 (every frame up).

#### Moving and sizing frames

- Drag any spot that is not a button, slot or bar, or anywhere with Alt held. A drag on a piece covering its frame (group card, menu button, minimap) moves the frame (`hud.DragItem`).
- Scale is in the right-click menu. The chat's right and bottom edges and bottom-right corner resize it.

The right-click menu (Alt+right-click where the game has its own menu): Lock this frame / Lock all frames; Layout mode (frames that hide while empty, outlined); each frame's scale, opacity, reset position and size; Roll up or Expand, and hide; All frames (HUD scale, minimap, reset every position); Snap to grid.

Places, sizes and scales (`hud.pos.<window>`, `hud.size.<window>`, `hud.scale.<window>`, every `hud.*` key) are per character, in `ui-modern.txt` in the character's folder. Missing keys come from the shared `ui/settings/modern.txt`, so a new character starts from that layout. The layout copy window copies another character's.

#### Modern dialogs

Dialogs (`hud.Dialog`) have a title bar and close box and are always movable. Their background is opaque; Opacity fades the whole window.

- **Scale** (`hud.scale.<window>`): the right-click menu's Scale slider or Ctrl+mouse wheel.
- **Resize**: a dialog with `opts.resizable` resizes from its right and bottom edges and corner, through its own handles (`ui.EdgeButton`); ImGui's resize is off, since its grip would take the corner. The size is kept in `hud.size.<window>` in design pixels, so Scale zooms the whole window. A dialog sized to its contents has no grip.
- **Roll up**: a closable dialog has a roll-up button beside the close box (`ui.Begin`'s `collapseRight`); a title-bar double click or the menu's Roll up does the same. Kept in `hud.rolled.<window>`, across closing and reopening.
- **No roll-up**: questions to answer (confirmation, quantity, text entry, bazaar purchase, the server plugins' question) say `rollup = false`, as does any dialog with `closable = false`.

HUD frames have no title bar and do not roll up, unless their script gives them one: `opts.rollup` folds into the title bar the frame draws itself (`opts.titleHeight`, design pixels), kept in `hud.rolled.<window>`.

#### Close boxes

Every window that can be put away has a close box, which goes through the window's own close (`hs.ShowWindow(name, false)`): the merchant, bank, loot, trade and tradeskill windows end their sessions with the server. A window only the server closes (respawn choices) has none (`closable = false`). Modern frames have one only when they can be put away (`hud.Window` `opts.onClose`); gamepad windows have a round X next to B. A modern dialog's right-click menu has scale, opacity, Fades, Snap to grid, reset position and size, Roll up and Close.

#### Grid snapping

Grid snapping (`lib/snap.lua`, `hud.snap`) is off by default; it is in every frame's and dialog's right-click menu and in Options > Display > Windows.

- The grid (`hud.snap.size`, 4 to 64, default 10) is in design pixels, times the UI scale; it is drawn faintly while dragging. Shift places freely.
- **Moving**: the dragged window's nearest left/right and top/bottom edges land on the nearest grid line, or flush with the screen edge when nearer (`snap.Axis`).
- **Resizing**: the dragged right or bottom edge lands on the nearest line or screen edge, never below the minimum size (`snap.Extent`); left and top edges stay.
- A frame that lays itself out from the drag (`opts.onResize`, the hot bars) is not snapped, since its own rounding would leave it off the lines.
- A frame's drag tracks the unsnapped place, since each frame's mouse move alone would never reach the next line.
- A dialog is snapped from where ImGui's title drag put it before its `Begin` (`ui.FindWindowRect(name)`) and held with `ImGuiCond_Always`.
- Turning the grid on moves nothing until the next drag.

#### Transparency and fading

A window's transparency is its own Opacity (right-click menu) times Options' Window Transparency (`[Main] GlobalAlpha`).

With Fades ticked (`hud.fades.<window>`, off by default; the original's per-window Fades), it fades as CXWnd::OnProcessFrame does: to the Fade-to Transparency (`GlobalFadeToAlpha`) once the mouse has been away for the Fade Delay (`GlobalFadeDelay`), back while the mouse is over it, each over the Fade Duration (`GlobalFadeDuration`).

#### Close with Esc

Close with Esc (`lib/escape.lua`, `hud.esc.<window>`) is ticked per window in its right-click menu: frames that can be put away, dialogs and Options.

- On by default for the windows in `DEFAULT_ON`: inventory, AA, ability and spell books, bags, map, item and spell displays, and the reference and social windows (inspect, skills, tasks, guild, friends, bank, scripts, help, books and notes, tribute, leadership, titles, tracking, mail, group, bazaar and barter search).
- Off for the rest, such as prompts and the merchant, loot, trade, give and tradeskill windows, whose closing ends or cancels something.

The CLOSE_TOP_WINDOW key (Escape unless rebound) closes one ticked window per press: the one shown that was opened or clicked last. The client's own key windows go first (`ActionsUi::close_top_window`); a press nothing took is the UI's for that frame (`hs.TakeCloseTopWindow()`, true once). The key's other default command, CLEAR_TARGET, still runs.

#### Controls

Controls (`lib/widgets.lua`): buttons, check boxes, button rows, edit and combo boxes, sliders, gauges, lists, STML text, item slots and grids, tabs and coins. A list is a virtual table over `hs.ListInfo` / `hs.ListRow`, sortable and filterable.

Each control is bound to a game window's child by its ScreenID (the layout's `id`, not `name`): it reads what the child shows and hands clicks to it (`hs.ClickChild`, `hs.ListClick`, `hs.SetWindowText`, `hs.ComboSelect`, `hs.SetSlider`, `hs.SetTabPage`, `hs.PickUp`), so the window's own handler runs.

ImGui bindings they add beyond the first set: `InputTextWithHint`, `PushTextWrapPos` / `PopTextWrapPos`, `SetCursorScreenPos`, `TableSetupScrollFreeze`, `TableGetSortSpecs` and the `Sortable`, `BordersInner*`, `Hideable`, `Reorderable` table flags.

### Redone windows

Each entry names the window and what is particular to it; the `hs.*` calls behind them are in [scripting-reference.md](scripting-reference.md) and [ui-api-reference.md](ui-api-reference.md).

#### HUD windows

- **PlayerWindow**: level badge, health / mana / endurance, experience and AA experience, and the spell being cast (the game's CastingWindow). While auto attack is on: a pulsing red ring (`T.Panel`'s `glowWidth`), a red wash and a red level badge.
  In the Modern frame's right-click menu, **Mana readout** and **Endurance readout** each offer **None** (the default), **Percentage**, **Value** (current / maximum), and **Both**. The choices are saved separately in the Modern pack settings (`player.manaReadout` and `player.enduranceReadout`). Hovering either bar still shows its current and maximum values.
  The Modern frame's right-click menu has **Always show cast bar** (off by default). It keeps the empty cast bar visible between casts so the frame stays the same height. The choice is saved in the Modern pack settings (`player.alwaysShowCastBar`).

- **TargetWindow**: con colour, health, target's target, the aggro meter (AggroMeterWnd's gauge 149 and labels 302..307; `aggro.lua` keeps the game's window hidden, and the EQ menu has no Aggro Meter row), and buffs with Spell Info on a right click.

- **GroupWindow** has member cards and the invite, disband, accept and decline buttons. A left click on `Gauge<n>` selects the member. A left click on `PetGauge<n>`, used by the Modern card's green pet bar, selects their pet. Solo the window shows only with an invitation or a player to invite.

- **BuffWindow** and **ShortDurationBuffWindow**: the defaults are the compact horizontal icon trays with remaining time under each icon, a transparent background, and no title bar or drag resizing. Icons fill from right to left and pulse near expiry.
  - Right-click an effect for Spell Info, Add to Blocked Buffs, Remove, and **Window options**. Right-click the title bar or an empty spot to open the options directly, or Alt-right-click anywhere in the tray.
  - **Layout** chooses Horizontal or Vertical. Vertical fills down and continues in columns. **Show spell names**, **Show slot numbers**, and **Show remaining time** can be set independently. **Show empty slots** reserves gaps up to the last occupied slot. **Show slot numbers in tooltips** also works with the visible numbers off. Both kinds of numbering use the effect's one-based slot in its tray.
  - **Show window background** and **Show title bar** are independent. The frame menu's **Opacity** controls the whole tray.
  - Turn on **Allow window resizing** to drag the left edge and change the width. A horizontal tray wraps to fit. For vertical icons without names, drag the bottom edge to change how many icons fit in each column. The grip shows on hover. Turning resizing off keeps the size. **Reset size** restores automatic sizing. **Icons per row** also returns horizontal trays to fitting their contents.
  - The `hud.*` settings are saved per tray and character (`lib/bufftray.lua`).

- **CastSpellWnd** (Modern Spell Gems): gem tiles with refresh shading and a spell book button after the last gem. The default is a single row with the window background and the original icon size. The **Vertical** setting fills down first.
  - Alt-right-click the bar for **Show spell names**, **Show window background**, **Show resize bar**, and **Icon size**. Spell names appear beside their gems. Hover a gem for its full name.
  - With **Show resize bar** on, drag the right edge, bottom edge, or bottom-right corner to wrap the tiles into rows and columns. Hiding the grip keeps the chosen wrap. **Reset size** clears the wrap and restores the default icon size and frame scale.
  - The options are saved per character with the Modern layout. Gem clicks and drags keep their usual actions in every layout.

- **HotButtonWnd**..10, **CastingWindow**, **EQMainWnd**; **CompassWindow** (a heading band).

- **Chat windows**: tabs, the channel chip, the chat menu.
  - A button at the strip's right end folds the window into its tab strip.
  - Always Chat Here (`hs.ChatSetAlwaysHere`, `hs.ChatInfo`'s fifth value) picks the window Enter and '/' start a line in; the main window by default.
  - Right-click a chat window and open **Timestamps** to choose **Off**, 12-hour with seconds (`[09:07:43 PM]`),
    24-hour without seconds (`[21:07]`), or 24-hour with seconds (`[21:07:43]`). The Modern pack's setting
    (`chat.timestamps`) applies to all its chat windows, including their existing lines, using each line's local
    arrival time. Off is the default. Previously enabled timestamps keep the 24-hour format with seconds.
  - Each window and tab keeps its own last 1000 lines (Hailstone's limit).
  - A tell window flashes once when another player's tell comes in (`hs.ChatWindows`' `tellsIn`).
  - A window reads only the lines since its version (`hs.ChatText(slot, version, names, stamps, true)`), and everything again (`reset`) when its lines changed otherwise. Line wrap and height are cached until the width or font change. A word wider than the output (a long name or a run with no space) is broken where the row ends, at a character's start, as ImGui's own wrapped text breaks it.

- **InventoryWindow**: the worn grid, numbers by EQType, carried slots, coins. Testing: `test.inv.page=character|stats|bandolier|currency`.
  - An item or coin dropped between the worn slots goes to `IW_CharacterView`: CInventoryWnd::HandleLButtonUp's auto-equip (eqstr 5290 when it stays), or the coin back to the purse.
  - Stats page: every attribute and resist as total, base, heroic and cap, item bonuses, and `hs.CharacterStats`'s regeneration, unbuffed pools and defense (`app/lua_ui_stats.cpp`).
  - Bandolier page (Hailstone's: the stock window has none), drawn from `lib/bandolier.lua` over the hidden BandolierWnd.
  - Alt. Currency page: "Show all" (`AltCurr_DisplayMissingButton`), card click selects that row of `AltCurr_PointList`, Claim and Reclaim are `AltCurr_CreateItemButton` and `AltCurr_ReclaimButton`.

- **MapViewWnd** (`map.lua`, over `hs.DrawZoneMap`):
  - resizable; wheel zoom around the cursor (around the player while following); drag to pan;
  - the Zone and World views keep their zoom and centre when the map closes, until zoning or logging out;
  - Fit / Follow / Labels / Auto Z, and your own markers per zone in `map.markers.<zone>`;
  - right click: Find a path here (`hs.FindMapLocation`), the cursor's /loc and distance; Edit opens the map toolbar;
  - the side panel's Find page is Hailstone's Find window (`app/lua_ui_find.cpp`, docs/navigation.md "Find"); its NPCs button marks the listed NPCs on the map.

- **Bag1..36** (`bags.lua`): rows of up to five. Resizable (Hailstone's): a set size (`hud.size.Bag<n>`) fits as many slots to a row as its width holds.

- **ExtendedTargetWnd** (`lib/xtarget.lua`, which takes a plugin's part on each row, docs/plugins.md): filled / available slots, a row for every server slot (empty ones faint) with its role unless an auto hater, six rows tall until resized. Right-click adds Hailstone's Auto expand (`hud.expand.ExtendedTargetWnd`), which grows the frame down to the last filled slot.

- **PetInfoWindow**: right-click a buff for Spell Info or Add to Blocked Pet Spells. Removable beneficial buffs also offer **Remove**. The icon clears when the server updates the pet's buffs.

- **SpellBookWnd** (`spellbook.lua`, over `hs.SpellBook`): search, sort, filters, categories, list or grid, spell display text.
  - Spell sets: load, edit, delete, New set or From gems. The twelve-slot editor takes drags, double-clicks or the spell's menu and saves without memorizing; a spell dropped on a set's card joins that set.
  - A gem strip takes drags; only an empty gem takes a spell, as StartSpellMemorization without replace (`hs.SpellBookMemorize` refuses a full one). The spell bar takes them too; its book button's right click lists the sets.

- The **minimap** (`minimap.lua`): `hs.DrawMinimap`, zone name, clock, the Find window's NPC marks. A click opens the map.

#### Item slots

Slots act as classic's (`lib/items.lua`): the same button phases, and drags to hot buttons.

- Inventory and bag slots take the left click on release (CInvSlot::HandleLButtonUp).
- Holding the left button half a second, or dragging off, puts a hotbutton for it on the cursor (`hs.ClickSlot(slot, bag, "leftheld")`, `hs.BagSlotClick(window, i, "leftheld")`; CInvSlotWnd::HandleLButtonHeld, `game::inventory_hot_button`).
- That hotbutton is an item link (type 12) that follows the item, found by serial, else item id (`game::hot_button_item_slot`). With Shift or Ctrl, or on an empty slot, it is the slot itself (type 5, top-level slots only).

#### Group roles

In a group, right-click a member's name in the Group window, or your own in the Player window, for Main Tank, Main Assist and Puller toggles, in both packs (`hs.ClickChild("GroupWindow", "Gauge<n>", "right")` / `hs.ClickChild("PlayerWindow", "PlayerHP", "right")`, true when it took the click).

- Role icons beside the names, full names on hover. Icons and checks follow the server's replies.
- Only the leader may change them; a mercenary's Main Assist and Puller rows are greyed.
- The menu has no "Window" row. In the modern pack the layout menu is a right click elsewhere on the frame, Alt-right-click on the name, or a right click on the name when solo.
- In the gamepad pack, Y on a Group window row opens it (docs/gamepad.md).

Rules: docs/mechanics.md "Group roles". The controls use the GroupWindow / PlayerWindow models (`app/lua_ui_hud.cpp`) and the `OP_GroupRoles` sender `/grouproles` uses. `ui.StyleVar.WindowMinSize` is bound for windows under ImGui's 32 px default.

#### Group invite and Auto-Inventory

- **Group invite** (`groupinvite.lua`, Hailstone): a popup in the upper middle while an invitation waits, showing the inviter (GroupWindow's `Hailstone_Inviter`) with Accept and Decline (FollowButton, DeclineButton). Its X dismisses that invitation.
- **Auto-Inventory** (Hailstone, not the original's): a button in the inventory and bank windows (`Hailstone_AutoInventory` on InventoryWindow, BankWnd / BigBankWnd), enabled while the cursor holds an item. It runs `/autoinventory`: a worn slot that takes it, a free general slot, else a bag; "no open slots" otherwise.

The groups below are redone too, each window a resizable dialog (or frame) over its game window's children, with a helper library per group.

#### Items and trading

Helper: `lib/trade_common.lua`.

- **MerchantWnd**: sortable goods table with search, "Usable only" (MW_UsableButton) and "Affordable only"; coin prices dimmed when unaffordable; Buy or Sell and Inspect; Recover and Parcels pages.
- **TradeWnd**: both offers side by side (stacked when narrow); accepted sides framed, status from `hs.TradeState()`.
- **GiveWnd**. The native shortcut for offering the cursor item to the target is hidden for service NPCs that
  refuse direct hand-ins, including personal and guild tribute masters. Use Donate in the tribute master's window.
- **BankWnd** and **BigBankWnd** (`bank.lua`): rewrapping slots, a dimming search, shared bank, banked coin.
- **GuildBankWnd**: bank and deposit tables with the permission filter.
- **QuantityWnd**: number box, 1 / Half / All, where the game puts it.
- **ItemDisplayWindow**..#6: one scrolling card: flag chips (Collectible and No Destroy too), class / race / deity / slot lines, a stat grid that folds as it narrows, heroic bonuses, description.
- **LootWnd**: filled slots as tiles, Loot All.
- **SkillsWindow**: sortable, searchable list.

Testing: `HAILSTONE_COMMERCE_TEST=merchant|bank|give:<item id>` near the NPC.

#### Dialog windows

Helper: `lib/dialogs_kit.lua`.

- **ConfirmationDialogBox**: Yes is primary; Enter does not accept.
- **LargeDialogWindow**: the server's text, scrolled.
- **TextEntryWnd**, **EditLabelWnd**, **TipWindow** (earlier tips).
- **NoteWindow** and **BookWindow**: paper-toned pages, side by side or stacked.
- **AlarmWnd**, **BreathWindow** (a frame; the air bar pulses in its last quarter), **RespawnWnd**.
- **ColorPickerWnd**: preview, hex, red / green / blue, presets.
- **BodyTintWnd**: current and dye per slot, reagents needed and held.
- **BugReportWindow**, **ClaimWnd**.

#### Progress windows

Helper: `lib/progress.lua`; `P.Rows` draws a list child as custom rows, reading `hs.ListRow` only for rows in view.

**AAWindow** (`lib/aa.lua`, which takes a script's part on each row, in the details and under the header; docs/plugins.md):

- One list of all four tabs' abilities (AAWindow's `ModernList`): a deliberate difference, the stock window shows one tab at a time.
- Search, and tab, category and owned filters.
- Columns: tab, era (expansion; sorts by `ModernList`'s eighth column), active or passive, rank, cost. Text is a font size up.
- Details pane from AAWindow's `Modern*` children (`app/lua_ui_progress.cpp`): rank, next cost, required level, prerequisites, reuse; Train / Train all / Hotkey.

The AA list's columns:

- Sorted by name at first; a header click sorts by it (again reverses), a header drag moves it.
- A header's right edge resizes against the next column, with minimum widths. The ability column takes what the others leave.
- Narrow windows shrink columns to fit without changing what is kept.
- Kept as `aa.columns`, `aa.sort` and `aa.widths` (design pixels, never the ability column's).
- **Reset columns** restores default widths, order and name sort.
- Headers and rows use the dialog's scale and the same row bounds (scroll bar and a script's reserved area included); each cell clips its text.

The other progress windows:

- **LeadershipWindow**, **TitleWnd**.
- **TrainWindow**: search, sort, value bars, prices.
- **TradeskillWnd**: recipe search, favourites, container, components with what you carry (green or red) and how many you can make, Combine / Combine N / Combine All / Stop (Hailstone's `Combine*`, `ComponentList` and `MakeableLabel` children), Auto-Inventory / Experiment.
- **TrackingWnd**: con toggles, search. Toggles and sort are a ranger's only. They run `/trackfilter`, `/tracksort`, `/trackplayers`, `/trackpets` and `/trackmercs` as CTrackingWnd::WndNotification does, so they print and save. Auto update is the option `track_auto_update` (`[Defaults] TrackAutoUpdate` in options.ini).
- **SkillsSelectWindow**, **CombatSkillSelectWnd** and **SpellDisplayWindow**..#6.
- **AchievementsWnd** and **AchievementWnd**: `achievements.lua` in both packs (see `lua_ui_achievements.cpp` below).

Testing: `test.open.<screen>=<seconds>`, `test.select.<window>.<list>=<row>`.

#### Social windows

Helper: `lib/social.lua`.

- **FriendsWindow**: friends and ignored, add, Who / Remove.
- **RaidWindow**: twelve reflowing group cards, ungrouped players above, roles, search, the invitation banner.
  - A member dropped on a card, or "Move here", is selected in the game's list and that group's button pressed. The game enables those buttons only in a locked raid, so an unlocked raid's leader gets it locked first. A full group refuses the drop.
- **RaidOptionsWindow**, **LFGuildWnd**.
- **GuildManagementWnd**: roster, notes, message of the day, ranks.
- **InspectWnd**: worn grid and gear by name.
- **MailWindow**, **MailAddressBookWindow** and **MailCompositionWindow**.
- **GroupSearchWnd**: your post (as player or group) beside sortable, searchable results; a group's members and wanted classes under it.
- **GroupSearchFiltersWnd**, **GroupMentorWindow**.
- **FellowshipWnd**: members and campsite pages. Its tab box is the script's own (the model keeps no page): `test.tab.FellowshipWnd.Subwindows=n`.
- **SocialEditWnd**:
  - button preview, colour swatches;
  - each line's command with a kind badge, a wait box for its `/pause`, move / empty buttons;
  - a cooldown box for the `/timer`, and a timeline of when each line runs;
  - examples, Revert / Cancel, Ctrl+Enter to accept, a Commands / Lua switch;
  - Tab completes the game's slash commands;
  - toolbar: Test, Put on bar (or drag the preview), Copy and Paste share text, "running" with Stop;
  - a pasted Lua social goes to the Lua editor unsaved (global `hailstone_social_draft`);
  - line parsing is `lib/social_lines.lua`; the gamepad pack's copy is kept the same by a test.
- The **Lua Social editor** (`luasocial.lua`, `hud.pos.LuaSocialWnd`). Its Commands / Lua switch turns `mq.cmd` and `mq.delay` lines back into command lines.
- **IconSelectionWnd**.
- **CombatAbilityWnd**: the active discipline and eight ability tiles with reuse shading; right-click menu; drag to a bar. `test.draw.CombatAbilityWnd=1` draws it for a class the game refuses it to.

Testing: `test.click.<window>=child,list:row,tabs=page,edit~text`, `test.lfguild.page`, `test.mail.tab`.

#### Quest windows

Helpers: `lib/quests_kit.lua`, `lib/quests_board.lua`, `lib/task_journal.lua`.

- **TaskWnd** and the **Quest Tracker** (see "Quest Journal and Quest Tracker" above).
- **TaskSelectWnd**: offers as cards, the description's `[n,text]` steps numbered.
- **DynamicZoneWnd** and its lockouts, **DynamicZoneSwitchListWnd**.
- Adventures: Alt+V opens the status without a recruiter. Request/offer controls, a confirmed Leave, seven-column theme stats, a category/risk leaderboard with tied ranks.
- PvP stats and leaderboard.
- **TributeBenefitWnd** and **TributeMasterWnd**.

Testing: `test.tab.TaskWnd.TaskSubwindows=2`, `test.tab.TributeBenefitWnd=guild`.

#### Market windows

Helpers: `lib/markets.lua` (coin chips; `M.Table`, a sortable, filterable table over a copy of a list child's rows) and `lib/markets_points.lua`.

- **BazaarSearchWnd**: filters in a side column; results sortable by item, quantity, price, trader or a picked stat; double-click buys, right-click inspects.
- **BazaarWnd**, **BazaarConfirmationWnd**, **BarterWnd**, **BarterMerchantWnd**, **BarterSearchWnd**.
- **PointMerchantWnd** and **NewPointMerchantWnd**: right-click inspects, Shift or Ctrl+right-click previews.
- **MarketplaceWnd**: category tree, the shelf as grid, list or landing page, quantity, buy pages and 3D preview (`hs.ItemPreviewImage(w, h, "marketplace")`).
- Mercenary windows: MMTW_MerchantWnd, MMGW_ManageWnd, MIW_InfoWnd.

Testing: `test.open.<screen>=1`, `test.draw.<screen>=1`.

#### Information windows

- **JournalNPCWnd** and **JournalCatWnd** (`journal.lua`): NPCs beside their lines, category filter, search, keep a journal, Save; the category editor. Both lists multi-select with Shift / Ctrl and sort by header (`W.List` `opts.multi` / `opts.headerClick`).
- **HelpWindow** (`help.lua`): Back / Contents / Reset Tips / Customer Service; href links followed (`lib/stml.lua`).
- **StoryWnd**: stories beside the page, "show new stories"; links followed with no Back.
- **AlertHistoryWnd**: toggled by the alerts key (Shift+H by default).
- **ZonePathWnd** (`zoneguide.lua`). Find is the map's page.
- **TributeTrophyWnd**: personal or guild view, numbers as tiles, both benefit lists, Upgrade / Downgrade / Modify.

Testing: `test.open.<screen>=1` (JournalNPCWnd, JournalCatWnd, AlertHistoryWnd, TributeTrophyWnd); `test.draw.<screen>=1` (ZonePathWnd, which the game opens only with a zone guide path; TributeTrophyWnd).

#### HUD extras

Helpers: `lib/hudx_tiles.lua`, `lib/hudx_blocked.lua`.

- **ActionsWindow**: tabs, reflowing tiles; a tile dragged to a bar gives its hot button.
- **BandolierWnd**, **PotionBeltWnd** and **AuraWindow** (frames).
- **BlockedBuffWnd** and **BlockedPetBuffWnd**.
- **MapToolbarWnd**: the tools grouped.
- TargetOfTargetWindow is kept hidden (the target frame shows the target's target).

Testing: `test.tab.ActionsWindow.ACTW_ActionsSubwindows=n`, `test.show.MapToolbarWnd=1`.

#### Cursor and menus

`cursor.lua` and `menus.lua` redraw the cursor's item and the game's context menus in this UI's look.

The main button's cascade menu keeps the game's rows and commands, laid out by `menus.lua`: it opens from the menu button wherever it was moved (`game_menu.lua` keeps its rectangle in `modern.menuAnchor`), toward the screen's middle. Line icons are this UI's own (`lib/menuicons.lua`, by row text), covering every row the client can offer under other skins' and later clients' names too; the gamepad pack's All Windows list uses its copy.

#### The Options window

`options.lua` is this client's own tabbed Options window. Every setting is a client option by name (`hs.UI.settings.Option` / `SetOption`).

Its search field filters the open tab:

- A setting stays when every query word is in its label, tooltip or section title (on the Plugins tab, the plugin's name).
- Words may be loose (`lib/fuzzy.lua`): initials of consecutive words ("fov" for Field of View), a word's letters in order from its first (four letters or more), or one letter off (five or more).
- Key bindings, chat filters and colours match by name; the key list reads only rows in view without a search.
- Empty sections are left out whole, and section buttons hide while searching. Tests: `tests/lua_scripts_test.cpp`, `tests/ui_pack_windows_test.cpp`.

#### System windows

- **LoadskinWnd** (`/loadskin`): this client's user interfaces, Keep your layout.
- **LayoutCopyWindow** (`/copylayout`), **VideoModesWindow** (EQ menu; the modern Options' Resolution list does the same).
- **Import character settings** (`/import`) shows the source files, category selection, slot preview, and confirmation for apply or restore.
- **NameChangeWnd** / **NameChangePetWnd** / **NameChangeMercWnd** (`/changename`, `/changepetname`, `/changemercname`): one script, `namechange.lua`.

### Other shared windows

#### Gamepad panel bounds

Gamepad's `lib/window.lua` bounds each panel's size and position to the viewport, with a small margin, so titles and close controls stay reachable at narrow resolutions without changing the font or UI scale. Explicit positions and sizes that fit are kept; each window still scrolls its own body.

#### Item experience transfer

`ItemExpTransferWnd` (`itemexptransfer.lua`, both packs): an owned evolving item's display offers Transfer experience with that item as source. The selectors list evolving possessions (bags and cursor included) without moving them.

`hs.UI.evolving.State()` returns `shown`, `pending`, `submitted`, `hasDetails`, `compatible`, `compatibility` (percent), `maxTransferLevel`, `canTransfer`, `status`, string `source`/`destination` (empty when unselected), `revision` and `items`. Items and the optional `previewSource` / `previewDestination` have string `id`, `name`, `icon`, `level`, `maxLevel`, `progress`; previews are the server's resulting forms, never substituted for the selected originals.

1. `hs.UI.evolving.Select("source"|"destination", id)` selects an exact string ID (empty clears).
2. Transfer opens a review stage; Cancel sends nothing.
3. Confirm calls `hs.UI.evolving.Confirm(source, destination, revision)`, all strings. A changed pair, revision or original item invalidates it.

`hs.UI.evolving.CanTransferDisplayed(screen)` and `hs.UI.evolving.OpenFromDisplay(screen)` are the display entry. `hs.ShowWindow("ItemExpTransferWnd", false)` drops the selection. Gamepad Back cancels review first, then closes. Reads need no capability; selection, display entry and confirmation need Actions.

#### UI animations

`hs.UiAnimation(name)` resolves the first frame of a named `Ui2DAnimation` from the installed/default XML: `texture, u0, v0, u1, v1, width, height` (the authored frame size, not the atlas), or nil.

- It works in an icon-only pack without changing `hs.UiTexture`, `hs.ChildArt` or the art policy.
- Only a relative atlas filename and a contained positive crop are accepted.
- Resolve on each draw; handles do not survive texture-cache eviction.
- No game art or XML is bundled.

#### Spell sets

Spell sets (`game/spell_sets.h`, `net/game_connection_spell_sets.cpp`) are the character ini's `[SpellLoadouts]` (`hotbuttons.ini`, via `ActionsUi`), memorized as `CSpellBookWnd::MemorizeSet` does; `/memspellset`.

- Scripts: `hs.SpellSets`, `SaveSpellSet`, `MemorizeSpellSet`, `DeleteSpellSet`.
- The books' set editors (Hailstone's own): `hs.SaveSpellSet(name, spells)` and `hs.EditSpellSet(n, name, spells)` save without memorizing. The ini is the same, so the stock client reads the sets.
- Code: `app/lua_ui_spellbook.cpp`, with the book's calls.
- Testing: `test.book.tab=details|sets`, `test.book.select=<spell id>` and `test.book.edit=<set number>|new|gems` in `ui/settings/modern.txt`; `test.book.view=book|sets|edit<set number>|editnew` in `ui/settings/gamepad.txt`.

### Host code

#### The script host

`src/ui/lua/` holds the script host and the `ui.*` ImGui bindings. The host (`lua_host`) does:

- script loading, hot reload and the `ui.window` registry;
- `ui.GetSetting` / `SetSetting` in `ui/settings/<name>.txt`;
- window layout keys (`LuaUi::character_setting`: `hud.*`, `tasks.listw`) in the character's `ui-<name>.txt` once in the world (`LuaUi::set_character_settings`), falling back to the shared file (`docs/layout.md`);
- the "UI errors" and "Lua UI" windows.

The bindings (`lua_imgui.cpp`) take only ImGui's public window flags, refuse calls ImGui would only assert on, and draw images only from live client textures (`src/ui/texture_ids.h`). Additions:

- `ui.CodeEditor` (`lua_code_editor.cpp`): a Lua editor with colours from `src/ui/code_syntax.h`, an error line and completion. An editor not drawn for about a minute, or any not drawn last frame once more than 256 exist, is forgotten.
- `ui.SetClipboardText` / `GetClipboardText`; MQ scripts get them only with every capability (docs/plugins.md).
- `ui.EdgeButton(id, x0, y0, x1, y1)`: an invisible button reaching the window's outer edge.
- `ui.FindWindowRect(name)`: a window's place and size by name (nothing for one never drawn).
- `ui.Begin(title, open, flags, collapseRight)`: the collapse button beside the close box.
- `ui.SetNextWindowCollapsed(collapsed [, cond])` and `ui.IsWindowCollapsed()`.
- `ui.PushStyleColors(list)` / `ui.PushStyleVars(list)`: many pushes from flat lists `{ idx, r, g, b, a, ... }` / `{ idx, x, y or false, ... }`, returning the count pushed.
- A script's `PopStyleColor`, `PopStyleVar` and `PopFont` pop only what its own draw function pushed.

Also:

- A window registered after its script loaded that fails shows in "UI errors" on its own, with a retry button.
- Long string matches are held to the time limit by a check in the vendored Lua's pattern matching (`kMatchCheckKey`, THIRD_PARTY.md).
- `ui::lua::replaces(screen)` tells a C++ window whether a Lua window stands in for it.

#### Game art textures

`src/ui/ui_textures.{h,cpp}` makes the game's art into ImGui textures: any `uifiles/default` file, the first frame of any `<Ui2DAnimation>` in the EQUI_*.xml files, the icon grids, and the screens' `<Location>` / `<Size>`.

#### The hs table

`src/app/lua_ui.{h,cpp}` is the `hs` table; names, arguments and returns are in [scripting-reference.md](scripting-reference.md). Answers come from `GameConnection`, the character sheet and `UiHost` (filled by the client every frame).

- Windows are models keyed by XML screen name (`app/lua_ui_windows.h`).
- `hs.ReplaceWindow(name)` answers whether the game shows a window and where; `hs.WindowShown(name)` only the first (still counting as replaced). Use the latter for checks; a window the script draws calls `hs.ReplaceWindow`, which places it.
- A list (`hs.ListInfo`, `hs.ListRow`, `hs.WindowList`) is built once a frame per window and list, kept until an `hs` call that can change it.
- Each child answers by its ScreenID with what the game's window class sets on it; a click runs the client action the game's handler does.

The models are split over the files below. `hs.CursorAttachment` (`cursor.lua`) is in `lua_ui.cpp`.

### The lua_ui files

One entry per file: the windows it models and what is not obvious from the [function tables](scripting-reference.md).

#### `lua_ui_values.cpp`

Labels and gauges by EQType (`hs.Label`, `hs.Gauge`): the combat timer (label 137 "mm:ss", gauge 29 for the last 30 s, initialized by OP_PlayerProfile and updated by OP_RestState); title (74), ATK (23), combat regeneration (212..214); target of target (135); group and extended target mana and endurance (139..148, 171..210); pet buffs (86..115); blocked buffs (550..579, 650..679); aggro names and percents (302..333).

- A second argument `true` to `hs.Label` (1, 28) or `hs.Gauge` (1, 6, 27) adds the name plate's invisibility marks to the name: Hailstone's, the original's captions are plain.
- `hs.Gauge` and `hs.ChildGauge` return a special value's fill tint as CGaugeWnd::Draw gives it (`gauge_fill`: -2 full in dark red, -110..-10 purple, -2000..-1000 pink); the group frames use it for a member out of zone.
- `hs.BuffSlot`'s fifth answer is the fade under 19 s (`game::buff_fade`, CBuffWindow::OnProcessFrame).
- Pass true as `hs.BuffSlot`'s fourth argument to append the pet entity ID and server slot for a removable pet buff. Other buffs append two nils. `hs.WindowButton("PetInfoWindow", child, 3)` requests removal for the current buff at that child.
- The modern player frame shows the combat state (PW_CombatStateAnim by `hs.ChildArt`) as a pip on the level badge and the combat timer as a ring.

#### `lua_ui_hud.cpp`

PlayerWindow, TargetWindow, GroupWindow, BuffWindow, ShortDurationBuffWindow, CastSpellWnd, CastingWindow, HotButtonWnd..HotButtonWnd10.

- **Hot button menu** (`game/hot_button_menu.h`): clicking an empty slot or holding the right button opens the menu via `UiHost::open_menu`, drawn by `menus.lua`. With Lock layout on, empty slots have no menu and filled slots offer gameplay actions such as Activate and Inspect. Call: `hs.ClickHotButton(window, child, "left"|"right"|"rightheld" [, x, y])`; test key `HOTBTN:<screen>:<child>:rightheld`.
- An empty gem's right click pops the spell select menu (CCastSpellWnd::MakeSpellSelectMenu), each spell in its target type's colour (`game::spell_target_color`); `menus.lua` lifts a colour too close to the panel's brightness. On a gem's button it is the gem's own right click (`UiApi::click_gem`, HandleSpellRightClick).
- A right click on an AA button (a social of 120 + group) opens the AA window on that ability (`AAWindow`'s `activate`, CAAWnd::ShowAbility).
- Bars: key map text top-right. Modern bars go vertical when "Buttons per row" makes them taller than wide; dragging an edge or corner changes it. Gamepad bars go vertical when their page number is clicked (`hotbar.vertical.<window>`).
- Stackable items show their quantity at the bottom of the hotkey, including the Gamepad crossbar. Item links total the carried stacks of that item, including those in bags. Inventory-slot shortcuts show the current stack in that slot. The cursor and bank are excluded.
- **Lock layout** in a Modern bar's menu saves `hud.lockLayout.<window>` per character. It prevents accidental frame moves and resizes, drag and drop in both directions, slot edits, loading or clearing hotbuttons, and changes to button size and bar shape. Buttons still activate and pages still switch. **Lock this frame** and **Lock all frames** govern frame dragging and resizing independently. The row is Hailstone's: RoF2's window lock only stops a move or resize, and its hotbuttons can still be dragged in and out and cleared, as they are here with Lock layout off.
- The bar menu, settings and icon selection are in `lua_ui_hotbar.cpp` (scripts `ui/modern/iconselection.lua`, `ui/gamepad/iconselection.lua`).
- `hs.HotButtonChecked` is true for a pet command button ("Pet <command>") while that toggle is on (`game/pet_buttons.h`). `hs.HotButtonState` follows CHotButtonWnd_UpdateButtonStates (`game/hot_button_state.h`, `app/lua_ui_button_state.cpp`).
- The bars and gems read each slot with one call a frame (`hs.HotButtonInfo`, `hs.GemInfo`, `into` refilled in place), since the separate calls each work the slot out again.
- `hs.BuffSlot(window, child, true)` adds the counter and limit overlays, worked out only when asked. Item tables carry `cooldown` (CInvSlotWnd_Draw; colour `hs.CoolDownColor()`).
- **Item preview** (CObjectPreviewWnd, `lua_ui_itemdisplay.cpp`, `app/item_preview_view.h`): `hs.ItemPreview()`, `hs.ItemPreviewImage(w, h)`, `hs.ItemPreviewControl`; drawn by `ui/modern/objectpreview.lua` and the gamepad `appearance.lua`; test key `PREVIEW:<slot>[:<bag>]`.

#### `lua_ui_web.cpp`

BrowserWnd (`ui/modern/browser.lua`, `ui/gamepad/browser.lua`; docs/browser.md) over `hs.Web*`. `hs.WebOpen` adds `https://` to text without a scheme. `hs.WebView(tab, w, h)` draws the page and gives it the mouse, and the keyboard once clicked: game key binds stay off until a click elsewhere. In a plugin's window the calls act as that plugin ("plugin:<name>", its tabs, its `web` permission and the server's sites; docs/plugins.md "Web pages"); `hs.WebOpenExternal` is for the player's windows only.

#### `lua_ui_scripts.cpp`

ScriptsWnd (`ui/modern/scripts.lua`, `ui/gamepad/scripts.lua`; docs/plugins.md "The Scripts window") and the EQ menu's Lua Scripts row (`kMenuScripts`). `hs.Scripts([rescan])` reads the folders at most once a second.

#### `lua_ui_luasocial.cpp`

Lua socials for `ui/modern/luasocial.lua` and `ui/gamepad/luasocial.lua` (`hs.LuaSocial` and the rest). `LuaComplete` completes mq.TLO paths (`mq::Registry::complete`), `mq.` and `button.`. `SetLuaSocial`'s `lines` turn a social plain again; `LuaSocialExport(n [, t])` shares a plain social or an editor's unsaved `t`.

#### `lua_ui_abilities.cpp`

`hs.Disciplines`, `hs.UsableSkills` and `hs.UseSkill(id)` (as the skill's hot button; the gamepad Tracking tile). CombatAbilityWnd's `CAW_CombatEffectTimeRemainingGauge` and `hs.Gauge(26)` give the active discipline's remaining fraction and seconds; both packs let the active icon end it. Ability entries show a red reuse fill and countdown from their shared timer, which may exceed that ability's own reuse.

#### `lua_ui_misc.cpp`

- **PetInfoWindow**: button layout is `UiHost::pet_buttons` (`game::PetButtonLayout`). The right button down on Pet<n>_Button (`hs.ClickChild(window, child, "rightdown")`) pops the commands CanUsePetCommand allows; the choice is saved as the UI ini's `PetButtonState`. `hs.ChildState`'s fifth result is bAction (a disabled button that still takes the right button).
- **ActionsWindow**, **SpellBookWnd**.
- **CompassWindow**: `hs.CompassStrips` / `CompassMarkers` over the shown heading: the true one plus CCompassWnd's error below Sense Heading 200 (`game/compass.h`), reset for 5 s by a successful Sense Heading.
- **RespawnWnd**: server destination names; the marker cell as Handle_OP_RespawnWindow fills it ("*" default, "r" for a row whose final byte is set); unavailable rows `texts.disabled`; accepted hover resurrection enables the reserved final row locally; resurrection text red/green; Select needs an available destination and the packet delay; timer gauge EQType 28.

The original Respawn window stays drawn beside confirmation dialogs; the packs put a resurrection offer in front once, as it appears. ConfirmationDialogBox's `ResurrectionOffer` child (not in the XML) shows while the question is that offer.

#### `lua_ui_tint.cpp`

BodyTintWnd and ColorPickerWnd. CColorPickerWnd serves any caller (`UiHost::pick_color(rgb, done)`, notification 0x25): the tint brush, map toolbar colours, label window, Options' Colors page.

#### `lua_ui_itemdisplay.cpp`

ItemDisplayWindow (.. #6) over `game/item_display.h`, pieces placed where the game places them (`hs.ChildRect`, relative to the parent).

- The description has CreatePlaceableString's "This item is placeable in ..." line, the collection hint and a trophy's benefit lines. `net::Item::placeable_flags`, `collectible`, `no_destroy` and `trophy_benefit` read fields EQEmu's struct calls unknown; `ItemDisplayContext::trophy_benefit` gives the benefit.
- A left click on `IconButton` (`hs.ClickChild(window, "IconButton", "left")`) links the item into the active chat input (the Always Chat Here one when set), as Link All does (`request_chat_link_all`); not while `game::benefit_flag_max` is above 0.

#### `lua_ui_dialogs.cpp`

- **QuantityWnd**: stack splits and the windows' own dialogs.
- **OptionsWindow**: key bindings (a key bound to several commands in red, Hailstone's marking), chat filters and user colours for each pack's `options.lua`. Settings are client options by name (`hs.Option` / `hs.SetOption`, `app/client_app_options.cpp`).
- **SpellDisplayWindow** (.. #6) over `game/spell_display.h`. Unlike the original, the Modern spell bar keeps the display open when a held right click is released over it; released elsewhere it closes, and a quick right click still forgets the spell.

`hs.DisplaySpellId(n)` and `hs.DisplayItemData(n)` (`lua_ui_spellbook.cpp`; n 1..6) say what each display shows.

#### `lua_ui_social.cpp`

FriendsWindow, GuildManagementWnd, RaidWindow. A raid roster's `hs.ListClick("RaidWindow", list, row, "right")` or `RAID_MemberActionsButton` opens the member's shared menu (`menus.lua`), which rechecks membership and permissions when activated.

#### `lua_ui_socialedit.cpp`

SocialEditWnd (CSocialEditWnd), opened on a social by a right click on its hot button or Actions button (`UiHost::open_social_editor`, `hs.OpenSocialEditor(index)`). Accept writes name, lines and colour to the character's socials (`hotbuttons.ini`). `hs.SlashCommands(prefix)` lists only the game's commands, in the order it tries them; `hs.PressSocial(n)` is `ActionsUi::press_social`; `hs.SocialLine(text)` splits as a press does (`game::split_social_line`). The Actions window shows each social's name in its colour (GetSocialTextColor).

#### `lua_ui_rewards.cpp`

RewardSelectionWnd, opened by the reward key. Both packs have Pending and Preview pages, common rewards, explicit choice confirmation, item inspection and manual Restore. TaskWnd's Preview uses the selected task id.

- Choices are numbered cards summarising their grants; long captions fit without splitting UTF-8, full text on hover or focus. The details pane lists the selected choice's items, quantities and lore flags.
- Clicking an item's tile, name or Inspect item requests its stats. A lone choice still needs selection and confirmation.
- Preview stays read-only; Restore never unlocks an uncertain claim.
- Reward item IDs are not icon IDs: with no known icon the UI shows a neutral tile, never guessing or querying while drawing.

**Reward-selection API.** `hs.RewardSelection([pendingRevision, previewRevision])` is nil outside a character session, else `{shown, pending, preview}`.

- Each lane: `revision`, `shown`, `claim_pending`, `claim_uncertain`, `inspect_pending`, `message`, `unchanged`, `tabs`. A matching revision omits only `tabs`.
- Revision and tab `token` values are opaque decimal strings, never Lua numbers.
- Tab: `title`, `pending_id`, `set_id`, `multiplier`, `blocked`, `options`. Option: `wire_id`, `common`, `flags`, `label`, `entries`. Entry: `type`, `entry_id`, `string_id`, `description`, four raw `values`, `flag`, presentation labels, item rows `{id, quantity, name, lore, lore_group}`. Server strings are plain text.

**Reward-selection actions.** `hs.RewardSelectionClaim(revision, tabToken, wireOption)` and `hs.RewardSelectionInspect(lane, revision, tabToken, wireOption, entryId, itemId)` return `(accepted, reason)`.

- Ids are exact unsigned 32-bit integers; lane is `"pending"` or `"preview"`. Acceptance means admission to transport, not delivery. Preview tokens cannot claim.
- `hs.RestorePendingRewards()` requests the server's pending view and never retries a claim; only an explicit native-window open can request Restore.
- `hs.RewardSelectionClose(lane)` closes one lane, `hs.ShowWindow("RewardSelectionWnd", false)` both.
- Actions need Actions; closing needs Commands; the getter is read-only.

The protocol page documents uncertain claims and nonce-less inspection replies.

#### `lua_ui_achievements.cpp`

AchievementsWnd and AchievementWnd (`achievements.lua`, both packs; `lib/achievement_browser.lua`).

- The browser: a collapsible category tree (parent/child order, local expansion), name/description search, All / Open / Completed / Locked filters, completion and points totals, explicit Refresh / Compare target / Clear comparison. Hidden and unknown states are left out, and category totals deduplicate descendant associations.
- Wide windows put list and detail side by side; small ones stack them. Objective bars show known counts only and never decide completion. Reward and Link are explicit actions.
- The detail window shows objectives and comparison columns, or a linked snapshot labelled with its player. Closing the browser hides its own detail; linked snapshots stay independent.
- Category badges: a category's `icon` is a stock XML animation name, drawn through `hs.UiAnimation` (base, Hover or Selected, falling back to base, then the name). They are wide badges with baked lettering: aspect kept, no name drawn over them, counts in a right-aligned column, full name in the tooltip.
- The gamepad pack adds its search keyboard and B-close.

**Achievement summary API.** `hs.Achievements([catalogToken, stateRevision])` is passive and nil outside a character session.

- Metadata: `shown`, `detail_shown`, optional zero-based `selected_index`, `detail_source` (`"own"` or `"link"`), `ready` (full state received), `progress_ready` (objective counts), `refresh_pending`, `message`, `points`, `total_points`, `completed`, `total` (known, non-hidden own states only), and `comparison = {pending, ready, name, message}`.
- `catalog_token` and `state_revision` are opaque unsigned 64-bit decimal strings: pass them back unchanged, never through `tonumber`. A matching first argument sets `catalog_unchanged=true` and omits `catalog`; a matching second sets `states_unchanged=true` and omits `states`. Clear both caches outside the session.
- Catalog `categories`: `id`, `parent_id` (`4294967295` for a root), `name`, `description`, `icon`, `display_order`, `child_ids`, `associations = {{achievement_id, text, display_order}, ...}`.
- Catalog `definitions`: `index`, `id`, `name`, `description`, `icon_id`, `points`, `has_reward`, `persistent`, `version`. Keep the zero-based index through sorting: it differs from the achievement ID.
- `states[index + 1]`: `known`, `status` (`"Completed"`, `"Open"`, `"Locked"`, `"Hidden"` or `"Unknown"`), optional `completion_time`.

**Selected detail API.** Passive `hs.Achievement()` is nil without a selection, else `shown`, `revision` (opaque string), `catalog_token`, `definition_index`, `id`, `name`, `description`, `icon_id`, `points`, `has_reward`, `source`, `player_name`, and `own`: the selected source's `known`, `status`, optional `completion_time` and `components` (own state, or the linked player's snapshot). An own detail may add `comparison` (same fields plus `name`).

**Components**: `id`, numeric `type`, `name`, `description`, `required_count`, `display_order`, `satisfied_known`, `satisfied`, `count_known`, optional `count` and `count_text`, in bucket order 0, 1, 2, 3.

- `count_text` is the exact unsigned decimal, beyond signed Lua integers too: display it rather than `count`.
- Missing counts stay unavailable; a known zero is real. Satisfaction never implies counts or completion. Type 3 is descriptive only.
- Comparison rows match by bucket and component ID. Received text is plain.

**Achievement actions** return `(accepted, reason)`; indices are unsigned 32-bit integers, tokens exact decimal strings, and the connection rechecks both.

- `hs.AchievementOpen(catalogToken, index)` selects locally and opens AchievementWnd (Commands; sends nothing).
- `hs.AchievementRefresh()`, `hs.AchievementCompare()` (the nearby player target) and `hs.AchievementReward(catalogToken, index)` (via RewardSelectionWnd, carrying the definition index, not the ID) need Actions.
- `hs.AchievementClearCompare()` clears locally (Commands).
- `hs.AchievementLink(catalogToken, index)` (Chat) adds the own snapshot to the chat composition without sending.

**Closing and ownership.** Close with `hs.ShowWindow("AchievementsWnd", false)` / `hs.ShowWindow("AchievementWnd", false)`. Only an explicit main-window open may request a refresh; drawing or reading never does. A reopened detail keeps its source. Catalog or connection replacement invalidates indices and detail ownership. Comparisons and linked snapshots never replace own state, and linked details have no reward/share buttons. Acceptance means local selection or request admission, not a server grant.

**Chat composition** (`lib/chatlinks.lua`, both packs): item and achievement tags are stored intact while the editor shows readable labels; the expanded prefix goes back to `hs.ChatTakeFocus`, and Send restores the exact tags. Malformed achievement envelopes show a generic label. A tagged line expanding past ten tags or 512 raw bytes (the gamepad channel prefix included) is rejected intact with a visible error, so the network never truncates through a tag; untagged chat keeps its longer limit. The gamepad edit field holds the same 2048-byte buffer as Modern.

#### `lua_ui_map.cpp`

MapViewWnd, MapToolbarWnd and EditLabelWnd (`map.lua`, `maptoolbar.lua`, `editlabel.lua`) over `game/map_data.h`.

**Map files.** `maps/<zone>.txt` (layer 0) and `<zone>_1.txt`..`_3.txt`, with `L x1, y1, z1,  x2, y2, z2,  r, g, b` and `P x, y, z,  r, g, b,  size,  text_with_underscores` records, named by the zone's short name (EQG zones alike). Map coordinates are (−X, −Y, Z) of the game's spawn axes, i.e. (−world.y, −world.x, z) here.

**Drawing**, as MapViewMap::Draw: the 472 × 360 fit; zoom (wheel ×1.5, buttons ×1.1, kept across zones, 1000 at first); pan; layer, label and height filters; label placement (24 places) and search; the maps directory combo (`Main` `MapSkin`); lines, labels, the line being walked, group members, the player's arrow (Sense Heading over 190, else a plus) along the compass heading, and "Range = n". Other players, NPCs and the target are not drawn; the modern map's Find marks are a Hailstone addition.

**The toolbar** needs the Planes of Power bit: hover, marks, Ctrl-drag lasso, start / end line, add point, remove line, join at intersect, automap, add / edit / move / remove label, save. Colour buttons use `UiHost::pick_color`; Clear Map / Clear Layer confirm.

**Saving.** Saved layers go to `servers/<profile>/maps/[<dir>/]<zone>_<n>.txt`, read before the server's, global and game maps (docs/layout.md "Maps"). Window settings are the UI ini's `[MapViewWnd]` keys (CMapViewWnd::LoadIniInfo / StoreIniInfo).

**Auto Z** is a client extension beside the original's height filter (which hides).

- Lines, labels and plugin marks more than `AutoZAbove` above or `AutoZBelow` below the feet (each 1..1000, default 30) fade, to 15% at three times that range. A line spanning the feet stays whole.
- It applies to MapViewWnd, `hs.DrawMinimap` and `hs.DrawZoneMap`.
- `on, above, below = hs.MapAutoZ([on [, above [, below]]])` (`above` alone sets both). Kept as `AutoZ` / `AutoZAbove` / `AutoZBelow` in `[MapViewWnd]`; an older single `AutoZRange` is read as both.
- The stock layout has no button for it; the modern map (arrow beside Auto Z) and minimap (frame menu) have range sliders.

**Map overlays.** `hs.UI.map.RegisterOverlay` (`app/lua_ui_map_overlay.cpp`) is the client-owned drawing layer of
`hs.DrawZoneMap` and `hs.DrawMinimap`: the callbacks run at the end of each drawn view, after the client's own lines,
labels, marks and player and before the pack's own additions, so they draw in whichever add-on's window called the
method. The registry is kept per UI VM, as the named window extensions are; a view's geometry is one plain table
(screen rectangle or disc, scale, centre, player, zone). A callback runs as its registered source, under that source's
ceiling, and a failed one retires like a named extension's. The complete API is in `docs/ui-api.md` "Map overlays".

**Paths and the atlas.** The find path (`game/player_path.h`; `hs.FindMapLocation(x, y)` as the stock right click, `hs.FindPathShown()`, `hs.EndFind()`), the atlas page (`game/atlas.h`, `hs.DrawAtlas`), and `hs.DrawZoneMap` for another zone's map. The modern World view and zone finder (docs/navigation.md "Zone atlas"; `ui/modern/lib/atlas.lua`) use `hs.AtlasIndex()`, `hs.AtlasPageData(file)`, `hs.AtlasTexture(file)`, `hs.ZoneLinks()` (asks the server once a session), `hs.ZoneRoute(to [, from])`, `hs.ZoneExits()`, `hs.ZoneExitWalk(zone)` (a `/nav` walk to that zone's nearest line), `hs.ShowZoneMap([zone])` and `hs.MapZoneShown()`.

#### `lua_ui_player.cpp`

InspectWnd, ConfirmationDialogBox (the ground drop's, the connection's and windows' questions: `UiHost::confirm`), BookWindow (ProcessText's paging), NoteWindow, TextEntryWnd (`UiHost::text_entry`), BandolierWnd, LargeDialogWindow (server popups, OP_OnLevelMessage), TargetOfTargetWindow, ExtendedTargetWnd, PotionBeltWnd, the extended target menu, and the two CTipWnd windows on the TipWindow screen:

- **The context tips** (`TipWindow`, `UiApi::activate_tip`), from windows' OnShow (`context_tips_on_show` in `lua_ui.cpp`), the connection's events (`GameConnection::take_context_tips`: sitting, level 10, consider, faction, experience, group and guild invitations, consent, world containers, duel and death), the app (the red con tip on attacking, the interactive object click), and the Actions and player customization windows. The table keeps the original's 90 ids, voice macro and audio trigger entries included.
- **The tip of the day** (`TipWindow#2`): help\\tips\\totd.txt at each zone-in (`UiApi::activate_tip_of_day`).

#### `lua_ui_help.cpp`

HelpWindow (help\\help.html, href links with Back history (the last 64 pages), Contents, the Tips button's tip of the day, Customer Service's /petition) and `load_stml_page`. An STML link goes to its window: `hs.StmlLink(window, child, message, data)`, `StockWindow::stml_link` (message -1 for an href; `lib/stml.lua`).

#### `lua_ui_login.cpp`

Models for the stock character creation page (EQUI_CharacterCreate.xml) and server select page (EQLSUI_ServerSelectWnd.xml). No pack draws them; the client's own screens show.

- Creation: race and class portraits of world's combinations, gender, deity and city, stats with points, Random Name (OP_RandomNameGenerator), the dbstr_us.txt help box, Create with the expansion and points checks; the name goes through `UiHost::char_create_submit`. It covers the screen over the pre-game model player (app/client_app_pregame.cpp, `UiHost::model_player`, `char_create_look`); Appearance opens PlayerCustomizationWnd. Testing: `HAILSTONE_CHAR_CREATE=1` opens it at character select.
- The ImGui fallback (`ui::character_create_screen`): name, race, class, gender, Deity (GetDeityDesc) and Start city (zone long names) as FillDeityCombo / FillStartCityCombo list them; each stat from its base up to base + 25 and at most 150, Create once every point is spent.
- Server select (`UiHost::server_select`, `UiHost::server_select_action`), with quick connect to the world played last on that login server (hailstone.ini `login.last_world.<profile>`). Testing: `HAILSTONE_SERVER_SELECT=1` keeps a test login (`--user`) on it.

A pre-world window answers `StockWindow::pre_game()`.

#### `lua_ui_pregame.cpp`

Hailstone's own login, server select, character select and creation screens, drawn by each pack (`ui/<pack>/login.lua`, `serverselect.lua`, `charselect.lua`, `charcreate.lua`) from `hs.LoginScreen()`, `hs.ServerSelect()`, `hs.CharacterSelect()` and `hs.CharacterCreate()` (`ui::LoginView`, `ServerSelectView`, `CharacterSelectView`, `CharacterCreateView`). Each is nil while another screen shows; creation's also while customization is up.

- **Revisions**: the first three views carry `view.revision`. Passing it back gives `true` while nothing changed, so a window keeps its table; a script that writes into its held view must read the whole view next frame.
- **Actions** (`hs.LoginSet`, `hs.LoginDo`, `hs.ServerSelectDo`, `hs.CharacterSelectDo`, `hs.CharacterCreateDo`; lists 1-based; the file's comments list them) do what the ImGui screens do. The login view has `view.version` (`build_version()`, shown at the foot) and the updater's `view.update` with `"updateDownload"`, `"updateRestart"`, `"updateSkip"` and `"updateNotes"` (docs/updater.md).
- Worlds are sorted favourites first, then by name ignoring case (`ui::server_list_order`), a Hailstone difference (the original keeps the login server's order). `"favorite", i` toggles one, kept as hailstone.ini `login.favorite_worlds.<profile>`, names joined by `|`.
- Creation's rules are shared with the ImGui screen (`ui::create_normalize`, `create_set_deity`, `create_stat_up` and the rest in `src/ui/dev_screens.h`). `hs.CharacterCreateDo("randomName")` asks OP_RandomNameGenerator. `hs.CharacterCreateHelp(kind, id)` gives dbstr_us.txt's description (types 8, 9, 14, 15 and 10) and name.
- Reading a view in a frame replaces the client's screen (`UiApi::replaces("LoginScreen" | "serverselect" | "CharacterListWnd" | "CharacterCreateScreen")`); actions return next frame via `UiHost::login_action`, `server_select_action`, `char_select_action` and `char_create_action`.
- Only the pack's own windows see these screens: a plugin's window gets nil and its actions do nothing, since the login view holds the password. A pack without these scripts (or `ui.cpp_windows=1`) gets the ImGui screens of `src/ui/dev_screens.cpp`.
- **Interface per server**: the login screen switches at once to the picked server's interface. `hs.LoginSet("ui", id)` writes only `ui` to a saved server's `server.ini` at once; other fields wait for Save or Log in.

#### `lua_ui_layout.cpp`

LoadskinWnd (/loadskin) and LayoutCopyWindow (/copylayout).

- LayoutCopyWindow copies another character's UI.ini or defaults.ini, for a screen size its keys keep, over this character's windowed places; characters played last come first.
- Hailstone adds LC_CopyChatCheckBox / LC_CopyOptionsCheckBox (chat.ini, options.ini) and a first-entry offer: activate(0) at the zone-in of a character with no UI.ini, when hailstone.ini ui.offer_layout_copy (option offer_layout_copy, on by default) is on and another character has a layout. LC_FirstVisitText and LC_OfferCheckBox show only then.
- **Key Bindings** (`LC_CopyKeysCheckBox`) replaces the character's primary and alternate keyboard mappings with those from the selected character's `hailstone.ini`. The mappings include the source's default movement scheme and any keys it cleared. The copy takes effect immediately. The first-entry offer preselects this option when the source has saved client settings.

#### `lua_ui_names.cpp`

NameChangeWnd, NameChangePetWnd and NameChangeMercWnd over `net/game_connection_names.cpp` (CNameChangeWnd and its copies).

- The server's invitations (OP_InvokeNameChangeImmediate / Lazy, OP_InvokeChangePetName(Immediate), 0x63d7 / 0x4cd9 for the mercenary) open them with the reminder prompt unless `[Preferences] Disable*NameChangeReminder` is set; the immediate ones clear it.
- `/changename`, `/changepetname`, `/changemercname` with no name open the manual prompt.
- Submit Name (or Enter) checks 4..15 characters and confirms (0x7c / 0x8c / 0x8d); yes sends {new name, character name, 0} as OP_GMNameChange, OP_ChangePetName or 0x36db, showing "Request Pending" until the answer. The result prints and shows as an OK dialog; success closes the window.
- Disable Reminder asks, then saves the flag in the character ini, this client's `hotbuttons.ini` (`ActionsUi::char_ini_int` / `write_char_ini`, `GameConnection::set_character_ini`).

#### Other lua_ui files

- `lua_ui_items.cpp`: InventoryWindow, Bag1..Bag36, WorldContainer (the open tradeskill object, as a container or by Experiment; the plain fallback shows only without a pack's window), LootWnd, MerchantWnd, BankWnd / BigBankWnd, TradeWnd, GiveWnd.
- `lua_ui_chat.cpp`: the 32 chat windows, channel routing, the chat menu and [ChatManager] keys, tabs, in the eqclient.ini user colours.
- `lua_ui_menu.{h,cpp}`: the cascade menu (Hailstone's layout, or a server's `uifiles/default/CascadeMenu.txt`; docs/layout.md). EQMainWnd is in `lua_ui.cpp`.
- `lua_ui_progress.cpp`: SkillsWindow, TrainWindow, AAWindow, TaskWnd, TaskSelectWnd.
- `lua_ui_market.cpp`: BazaarWnd, BazaarSearchWnd, BazaarConfirmationWnd, BarterMerchantWnd, BarterSearchWnd, BarterWnd.
- `lua_ui_marketplace.cpp`: MarketplaceWnd. MKPW_CategoriesTree, MKPW_Thumbnails, MKPW_NewItems and MKPW_FeaturedItems read as lists; the file's header gives their columns.
- `lua_ui_mail.cpp`: MailWindow and MailCompositionWindow.
- `lua_ui_video.cpp`: VideoModesWindow (modes, full-screen refresh rates, Accept with the keep-it question).
- `lua_ui_zoneguide.cpp`: FindLocationWnd and ZonePathWnd (no zone guide data on EQEmu).
- `lua_ui_fellowship.cpp`: FellowshipWnd (up to 64 server-supplied members, invitations, MOTD, leadership, XP sharing, exact vitality, campsite filters and expiry; vitality gauges 147/148) and AlertHistoryWnd.
- `lua_ui_story.cpp`: StoryWnd (storyline\\story*.txt, newest first; links via `stml_href_path`). Settings: hailstone.ini news.last_read / news.automatic; it shows at the first zone-in when a story is new and Automatic is on.

### Shared host pieces

#### User colours

`user_color_rgb(i)` (app/lua_ui_windows.h) is CDisplay's table of 99 plus Hailstone's 54 extended ones, read from the character's `chat.ini` [TextColors] over the asset folder's eqclient.ini, with the game's defaults. The Colors page writes `chat.ini` (`servers/<profile>/characters/<world>/<name>/`, core/character_files.h); the asset folder is never written.

#### Context menus

A model's `context_menu(host, x, y)` calls `UiHost::open_menu(x, y, rows)` (rows: text, colour, check, enabled, separator, `act`, `submenu`), drawn by `menus.lua` (`hs.GameMenus` / `GameMenuHover` / `GameMenuClick` / `GameMenuClose`). `hs.GameMenus(scale)` measures at a scale: modern's `menus.scale`, 125 % by default (Options > Interface "Menu Size", or Ctrl+wheel over a menu). Right click on a Lua window opens the model's menu; Alt + right click (or a "Window" row, `UiHost::window_menu`) opens this client's window menu.

Submenus open beside the row that opened them. They start on the right, switch sides when there is more room on the other side and the current side cannot fit the next menu, and carry that direction to deeper levels. The gamepad pack passes its panel width and row heights in screen pixels, so it uses the same placement with its larger controls.

#### Dialog owners

- `open_quantity(host, QuantityOpen, done)` (`game::quantity_dialog_open`) accepts at once or shows the dialog at the caller's point; `done` gets the quantity.
- Yes / no: `UiHost::confirm`; text: `UiHost::text_entry`. `UiHost::click_rect` is the last clicked child's screen rectangle.
- Models can answer drop-downs (`StockWindow::combo` / `combo_select`, `hs.ComboInfo` / `hs.ComboSelect`) and move their window (`take_move`: centred dialogs).

#### The UI scale

Game fonts are built at the display's content scale (or `--ui-scale`), and the packs lay out at that scale (`ui.GetIO().uiScale`). Sizes stored in the UI ini stay unscaled. `ui.GetIO()` returns the same read-only table every call (one per Lua state), with that frame's values; a write raises an error.

#### Window places

`src/ui/ui_ini.{h,cpp}`: at world entry, windows are placed from `servers/<profile>/characters/<world>/<name>/UI.ini` over the game's `defaults.ini`. The game directory's `UI_<name>_<server>.ini` seeds it and is never written.

- **Place and size**: the XML rectangle, then `Width`/`Height` and `XPos`/`YPos` for 1280x1024 and `...Windowed`. Windowed values are stored for 4096 x 4096 and moved by `ConvertToRes`, whose sums wrap in 32 bits as the original's do.
- **Shown or not**: `Show` comes from the character's file only (defaults.ini too for HotButtonWnd), and only for a window whose IniFlags have bit 8, as CSidlScreenWnd::LoadIniInfo @0x85dda0 reads it. Without it a window starts shown when its IniFlags have bit 9 (player, target, group, buffs, songs, gems, compass, chat, EQ button, hot bar 1). Hailstone clears it for the aura window, which RoF2 starts shown.
  - It is read again at each entry into the world, also after a camp back to the same character, whose new connection starts with the Potion Belt closed. Changes are written only while in game, and once more as the layout is saved on leaving, so character select never writes its own state into the last character's file.
- **Chat windows**: a file with no `[ChatManager] NumWindows` takes defaults.ini's extra chat windows as tabs of the main window; after that only the character's file counts. A deliberate change: through defaults.ini, "Other Chat" came back at every start after the player closed it.
- Moves, resizes and show / hide are written back to the same keys.

The game's defaults.ini lays the HUD out for a large window: at 1280 x 720 player, hot bar, group and chat overlap; `--window-size 1920x1080` shows them apart.

### Developer tools

#### Developer windows

The info panel, spawn list and "Lua UI" control window are off unless `ui.dev_windows=1` in `hailstone.ini`; Ctrl+Shift+D toggles them.

#### The performance window

Ctrl+Shift+P, `/perf`, or `ui.perf_window=1`. It shows frame times, the main loop's phases (60-frame average beside the slowest of the last 300), renderer counts, memory and traffic.

**With frame generation** (docs/graphics.md "Frame generation"): the headline shows rendered and displayed rates (`app::frame_rates`); a section adds the share of frames with an in-between frame, totals and skip reasons (a cut or resize; no free swapchain image), present mode and image count, the last wait for the second image, and frame generation's GPU time where the device can time it. The graph and phases stay per real frame.

**Log hitches** (`ui.perf_log_hitches=1`) logs every frame over 2.5 times the typical frame time (20 ms at least) as `perf: hitch ...`, phases longest first, then sections of 1 ms or more (`; in it: ...`).

**Frame sections** are laps marked by the main loop and `LuaUi::frame` (`core/frame_sections.h`): the Update, UI and Record phases' main calls (Update: connection, automation, world objects, player, spawn smoothing), the Record phase's cover and water maps, each Lua window and the map views' parts, the script scan and Lua GC. The "Frame sections" table shows each one's 60-frame average and slowest frame, with a filter (`app::SectionAverages`).

- With `display.parallel_record` (docs/graphics.md "Worker threads"), per-renderer laps are "... queued" and time only set-up and queuing; recording is in "scene record (opaque)" and "scene record". Off, they are "... draw".
- The table is rebuilt twice a second (`app::kSectionsRefresh`) and on a filter change, drawing only rows in view: rebuilding every frame cost the time it measured.
- Sections nest: "Lua UI frame" holds every "Lua window ..." row.
- "KB/frame" is what the Lua state allocated in "Lua UI frame" and each window (`LuaUi::allocated_bytes`; `core::frame_sections::add_bytes`): the collector's future work. Short strings (40 bytes or less) are interned and free on reuse; per-frame tables, closures and long strings are what it counts.
- The Actors row counts instances not posed this frame.

**GPU passes.** The "GPU passes" table shows each pass's time, averaged the same way, with the total: sun cascades, cover and water maps, light cubes, sky, zone, objects, characters, translucent, AO, bloom, post, UI and frame generation.

- Each is timed between `VkContext::gpu_mark` timestamps (`render/gpu_timer.h`), written only while the window is open and read back a frame or two late, so nothing waits on the GPU.
- With `display.parallel_record` the scene shows as "scene opaque (worker-recorded)" and "scene (worker-recorded)"; `display.parallel_record 0` restores per-pass rows.
- The zone viewer's `--gpu-timing` (with `--frames`) logs each pass's mean as `gpu pass: ...`.

#### Lua garbage collection

The Lua UI's collector runs in generational mode; `LuaUi::frame`'s first collection after loading switches it and makes the scripts' state the old generation.

- **Minor collections** (each 20 % of heap growth) take the few hundred KB of per-frame garbage the Modern windows make.
- **Major collections** are the host's: between window draws, `LuaUi::frame` collects in full when the heap reaches three times what was live after the last one (`kGcMajorTrigger`), timed as "Lua GC major" and logged at 8 ms or more.
- Lua 5.5's own shift to major collections would run untimed inside window draws, so it is pushed to about eleven times the live heap (`LUA_GCPMINORMAJOR` 1000 %). If Lua shifts anyway (lgc.c `checkminormajor`), the next frame collects and returns to minor mode.
- "What was live" is Lua's `GCmajorminor`, read in `src/ui/lua/lua_gc_live.cpp`, the one file that includes Lua's internals.

This replaced an incremental collector whose catch-up paused frames up to 40 ms.

#### Settings

- Graphics settings (`display.msaa`, `display.tonemap`, `display.exposure`) are in `docs/graphics.md`.
- `ui.cpp_windows=1` in `hailstone.ini` keeps the C++ windows on beside the Lua ones.
- In `ui/settings/<name>.txt`, `enabled.<window>=0` turns a script window off and `test.pos.<screen>=x,y` pins a window (for screenshots).

#### Testing aids

- `test.tab.<window>.<tab box id>=n` in `ui/settings/<name>.txt` opens page n once (`W.Tabs`).
- `--auto-keys` also takes:
  - `UI:<window>:<child>`: a click on a child;
  - `LIST:<window>:<list>:<row>[:<column>]`;
  - `DLIST:<window>:<list>:<row>`: a double click;
  - `COMBO:<window>:<child>:<index>` (a list cell's box is `<list>:<row>`);
  - `MENU:<window>:<x>:<y>`: a right click, the context menu;
  - `MENUROW:<menu>:<row>` (0-based).
- `HAILSTONE_AUTO_DOOR_ID=n` makes `--auto-door` walk to door n; `HAILSTONE_AUTO_DOOR_SECONDS` lengthens the walk.
- `GameConnection::receive_zone_packet_for_test` feeds packets to windows in tests (`tests/ui_windows_test.cpp`).
- `--frames` counts login and loading frames, which run below the cap. Time a click late (or log `ui.GetTime()`) when a screenshot must show a self-closing dialog.

#### Checking

Log in to the local test server (`--host 127.0.0.1 --port 5997 --user ... --character NAME --frames 900 --screenshot FILE`, `XDG_CONFIG_HOME` set to a scratch directory, a fresh one for first-login layouts) and look for "ui script error" in the log.

- `--auto-keys INVENTORY,TOGGLE_OPTIONSWIN,...` presses window keys.
- `--show-items N` opens the inventory and bags (1), picks up the first item (2, shift-pickup 8), shows the primary weapon's item display (4), opens the main menu (16) or a spell display (32).
