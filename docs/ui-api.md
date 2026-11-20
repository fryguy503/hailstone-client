# Modular UI API

Every interface uses the same client-owned `hs.UI` contract. Modern, Gamepad,
and additional UI packs draw their own presentations over that contract. The
client owns game state, window models, actions and permission checks. An interface
does not duplicate game rules, packets or server-specific logic.

```lua
local API = hs.UI
assert(API.version.major == 1, "This interface requires UI API 1")

if API.session.InGame() then
  local name = API.character.Name()
  local hpFraction, hpText = API.character.Gauge(1)
  -- Draw these values using this interface's widgets.
end
```

Use dot calls. Methods are grouped by the service they belong to, such as
`session`, `character`, `inventory`, `windows`, `chat`, `spells`, `tasks`,
`achievements`, `map`, `scripts`, `plugins`, `pregame` and `gamepad`.
The complete generated mapping is in [UI API Reference](ui-api-reference.md).

## Chat tabs and windows

`hs.UI.chat.Windows([into])` lists visible chat windows in slot order. `slot` identifies the window,
`tab` is the selected position in `tabs`, `title` is the selected tab's label, and `windowTitle` is
the containing window's title. Each tab's `slot` stays with that tab as it moves. Window and tab
slots may be reused after closing. Use the current one-based position in `tabs` for tab arguments.
Scripts that query each frame can pass a table as `into` to reuse the list and its records. Copy any
values that need to survive the next fill.

| Method | Behavior |
| --- | --- |
| `SplitTab(windowSlot, tab)` | Opens a separate window with the same tab and returns its window slot, or nil if it cannot split. |
| `JoinTab(windowSlot, tab, targetWindowSlot)` | Appends and selects the tab in another window. An empty source window closes. |
| `ReorderTab(windowSlot, tab, delta)` | Moves and selects the tab within its window. A negative offset moves left and a positive offset moves right. |
| `CombineTabs(targetWindowSlot)` | Appends tabs from other windows and returns the number moved. |
| `SetWindowTitle(windowSlot, title)` | Renames the containing window independently of its tab labels. |
| `SetTabFlash(windowSlot, tab, enabled)` | Sets whether background messages highlight the tab. |
| `SetTabFlashColor(windowSlot, tab[, rgb])` | Sets a `0xRRGGBB` highlight or restores the default red with nil or omission. |
| `SetTabScrollbar(windowSlot, tab, enabled)` | Sets whether the tab shows an output scrollbar. Read `scrollbar` on the tab record. |

`NewTab`, `SelectTab` and `CloseTab` use the same tab order. `CloseTab` closes one tab, and `CloseWindow`
closes the window and its tabs. The window containing Main Chat is protected from closing.
`SetTitle(windowSlot, title[, tab])` and `Clear(windowSlot[, tab])` act on the selected tab when
the position is omitted. Moving a tab keeps its lines, channel routing and preferences. Temporary
tell tabs can be moved during the current session.

Tab records expose `unread`, the `flash` preference, the `flashColor` override (nil for red), and
`highlighted`, which indicates whether to draw the highlight this frame. When enabled, a background
message flashes the label at half-second intervals for five seconds, then leaves it highlighted
until selected. `unread` also tracks new messages with flashing disabled. Mutations use the existing
`commands` permission. Check feature availability with `hs.UI.Supports('chat', 'SplitTab')`.

## Optional world nameplate presentations

`hs.UI.nameplates` is a generic drawing service, also exposed to ordinary addon scripts as
`require('hailstone').nameplates`. It does not require selecting a complete interface.
The UI facade publishes `Version`; the script module publishes `version`. Both are 1.
Script consumers need `read`, `ui` and `extend`; ordinary server policy and installed file ceilings apply.

`Register(name, callback)` returns an opaque owner handle, or nil with an unavailable reason. The name is
local to the caller. `Unregister(handle)` removes only a registration owned by that caller. A replacement,
permission loss, script stop, file reload, interface reload or VM shutdown retires its old claims and click regions.
Saved functions cannot recapture authority from an expired source file.

The callback runs during the current world-plate pass. Its frame includes `serial`, `now_ms`, `dt_ms`,
`font_size`, `font`, `viewport={x,y,w,h}` and `candidates`. Time is monotonic, with no shared-epoch promise
with other clocks. Each candidate has a current-frame `token`, projected head `x,y`, axial `depth`,
`native_scale`, a read-only spawn snapshot and `context`. The snapshot includes numeric id and incarnation;
the context includes the assembled caption, target/self/group/raid state, distance and only genuinely known
health/resources/cast/aggro/marker values. Tokens are opaque drawing claims, not target-action handles.

`context.invisible` and `context.invisible_undead` identify observed regular and undead invisibility effects.
`context.concealed` also covers an untyped spawn concealment update, Hide and observed invisibility versus animals.
The host can identify effects from your own buffs and current-target buffs supplied by the server. A false flag
means the host has no positive evidence for it. Older hosts omit these fields.

The host applies its normal awareness, hidden-actor, range and frustum gates before publishing candidates.
The addon owns its appearance, text, resource bars, shapes, icons, animation, placement and stacking.
The callback returns an array of models:

```lua
{token=candidate.token, bounds={x=-60,y=-18,w=120,h=18},
 offset_x=0, offset_y=0, through_walls=false, clickable=true,
 commands={{kind='RectFilled',x0=-60,y0=-18,x1=60,y1=0,color=0x344255,alpha=1}}}
```

Coordinates are local window pixels relative to the projected head. Positive offsets move right/down.
The explicit bounds and viewport clip every command. Generic command kinds are `Rect`, `RectFilled`,
`Gradient`, `Line`, `Circle`, `CircleFilled`, `Triangle`, `TriangleFilled`, `Text` and `Image`.
There are no health-bar, badge, skin or preset recipes in the host. Rectangle/line/image coordinates use
`x0,y0,x1,y1`; triangles add `x2,y2`; circles/text use `x,y`. Colors are integer RGB with separate 0..1 alpha.
Text adds `text,size,font`; images use a live opaque texture and UVs. Gradient adds `color2,alpha2`.

`MeasureText(text,size,font=0)` returns advance `width,height` and relative glyph-ink bounds
`ink_x0,ink_y0,ink_x1,ink_y1`. Font 0 is the current plate font; 1..6 select shared UI fonts.
Measurement is supplied by the current draw pass. Script `SpellIcon(icon)` returns a live texture plus
`u0,v0,u1,v1`, or nil; interface code can also use `hs.UI.spells.Icon`.

An omitted token, nil answer, invalid command list or failed callback leaves the ordinary caption active.
`{token=...,hide=true}` explicitly suppresses the whole caption. A valid nonempty model replaces the complete
caption, including its secondary lines and target decoration, for that frame only. No registration changes
global name switches. Missing depth support preserves ordinary captions rather than drawing an untested overlay.

Limits are 32 registered owners, 1,024 candidates, 512 commands per model, 32,768 commands per frame,
65,536 estimated vertices per model and 262,144 per frame. Parameters must be finite and bounded; text is
limited to 1,024 bytes per command. Models exceeding the aggregate geometry budget fall back independently,
with target priority. Rounded rectangles use fixed tessellation. Image handles are checked again at draw time.

`clickable` enables real manual selection only. The host validates the displayed frame, owner, connection,
zone and entity incarnation, respects native click-through masks and self-view rules, and preserves UI/modal,
cursor and drag handling. With `through_walls=false`, the actual clicked ray must remain clear against the
scene and visible actors. Through-wall drawing is explicit and does not bypass awareness or server action rules.

## Named window extensions

API 1.5 makes `hs.UI.extensions` slots generic. A window and section are nonempty ASCII identifiers, at most 128
bytes each, containing only letters, digits, `_`, `.`, `:` or `-`. UI authors emit slots in Lua; registrations do not
inject into an un-emitted slot. Adding another slot does not require a client API update. Current presenters include:

| Window | Section | Context |
| --- | --- | --- |
| `RaidWindow` | `member_actions` | The current selected raid member, including members outside the zone or without a group |
| `MapViewWnd` | `controls` | The current world/zone map view and copied presentation policy; no member selection |

Any valid window/section pair may be registered. Invalid identifiers are rejected. Selection, leader flags and map
policy metadata do not grant authority. Every slot uses the same owner, capability and source-lifetime rules below.

```lua
local E = hs.UI.extensions
local handle, reason = E.RegisterAction('RaidWindow', 'member_actions', 'inspect', 'Inspect member',
  {requiresSelection=true, requiredCaps={'read'}, priority=0},
  function(window, id, phase, context, reviewToken)
    -- Validate the current context before performing this action.
    return {ok=true, message='Done'}
  end)
```

`RegisterAction(window,section,id,label,options,callback)` returns an opaque handle, or nil and a reason.
The callback receives five arguments as shown above. The mandatory capability floor is `read` plus `ui`;
`requiredCaps` adds existing capability names such as `chat`. Required permissions must exist when registering
and remain live when executing. `priority` is an integer from -10,000 to 10,000. Ordering then uses owner, id
and handle. There are at most 32 registrations per owner and 128 per UI VM, including display callbacks.

The shared presenters separate addon actions from stock controls. Buttons use compact rows that wrap to
the available width, retaining long labels and stable keyboard focus as the window resizes. Confirm and
Cancel occupy a separate review row. Gamepad Raid retains its full-width scrolling controller menu with
an addon-actions heading. Set `priority` to choose the action order; no extra presentation metadata is required.

`ListActions(window,section,context)` returns only metadata: `handle`, `owner`, `id`, `label`, `enabled`,
`requiresSelection`, `requiresReview` and `priority`. Listing or painting an action never calls its callback.
The action array is retained; API 1.5 adds a `sections` metadata array with `handle`, `owner`, `id`, `placement`,
`enabled` and `priority`. It includes visible display registrations without invoking them, so a presenter can
discover a display-only slot. Missing `sections` on an older host means this metadata is unavailable.
`InvokeAction(handle,phase,context[,reviewToken])` returns `{ok=boolean,message=string[,reviewToken=string]}`.
An ordinary action accepts `invoke`. With `requiresReview=true`, it instead accepts `review`, then explicit
`confirm` or `cancel` with the returned token. A successful review must supply a nonempty message and token.
`ok` must be an actual Lua boolean. Messages are limited to 4,096 bytes and tokens to 1,024 bytes.
The addon remains responsible for validating its token, the current server, roster and permissions before
performing a game action; the client registry does not confer server authority.

Context is a bounded data table. The built-in raid presenters supply `selection={name,group,...}` and a
current advisory roster `revision`; authors should treat additional fields as optional. Review UI compares
the current selection and context again before confirmation. Stale selection, owner or permissions clears
pending UI locally without invoking a callback. Only explicit user activation dispatches action phases.

The map presenters supply `window='MapViewWnd'`, `section='controls'`, `view='world'|'zone'`, the current short
zone name `zone`, the map's short `shown_zone`, boolean `own_zone`, a fresh copied `policy` from `hs.UI.map.Policy()`,
and `revision`. Unavailable zone names are empty strings. This revision is a length-framed concatenation of typed
view/zone/shown-zone/own-zone values and the policy's context, source, revision, flags and name. Compare it only for equality;
do not parse, persist or treat it as authority or an actor/action token. Map callbacks receive no `selection`, so
actions using `requiresSelection=true` remain disabled. The presenters re-read map context on activation and clear
stale rows and reviews without invoking an action callback for automatic cleanup. Connection/source/zone/view
changes or retired registrations close the panel. Map content revisions and preference edits refresh its contents
and invalidate stale reviews while keeping the panel open.

The map's compact **Map controls** panel contains registered actions and before/after display callbacks. In Gamepad,
LT enters this panel; B returns or explicitly cancels a review before closing the map. While the panel owns input,
map pan/zoom and Find shortcuts are suspended. Outside the panel, ordinary map controls keep their behavior.

Only the display points the interfaces offer take actions and sections; any other window or part is an error
(`RegisterAction`, `RegisterSection`, `ListActions` and `DrawSection` alike), so an add-on cannot claim a part of a
window that was not offered: `RaidWindow` `member_actions`, `MapViewWnd` `controls`, `MapViewWnd` `map.canvas` and
the map toolbar's buttons, `MapViewWnd` `toolbar.<button>` (`toolbar.zin`, `toolbar.mapcontrols`, ...).

`RegisterSection(window,section,'before'|'after'|'replace',id,callback[,options])` registers a drawing callback receiving
`window,section,placement,context`. `DrawSection(window,section,placement,context)` is the presenter entry point
and, for `before`/`after`, retains its numeric count of successful callbacks, zero outside an active drawing frame.
API 1.5 adds `replace`, which returns an actual boolean. It invokes only the first eligible, visible registration in
the existing deterministic order: lower priority values first, then owner, id and handle. A completed callback returns
true even when it draws nothing, intentionally hiding the default. Missing/revoked/failed replacements return false.
A failing winner does not fall through to a second replacement during the same dispatch; the presenter draws its default.
Feature-check API version 1.5 before using `replace`; method discovery alone cannot distinguish this new placement.
Section callbacks use the same capability floor and optional `requiredCaps`/`priority`; action callbacks are not
invoked while drawing sections. Each callback has an isolated widget-ID namespace and cannot pop the parent
window, table, tab, popup or style scopes. Parent font scale, pending window/item settings and last-item
observations are restored after callback drawing; normal cursor advancement is retained.

UI authors can use their presentation helper `E.Component(window,section,context,defaultDraw)` to emit before,
replacement/default and after content. The default function remains private to the presenter and is never passed
to addon callbacks. An addon removes an element by registering an empty successful `replace` callback for that
emitted slot; removing its handle restores the default. This is cooperative presentation composition, not arbitrary
access to an internal window or gameplay action. Components the UI has not emitted remain unchanged. On older hosts, or an unknown API major, `E.Component` calls its default directly without probing new slots.

For example, a UI author emits a caption inside its existing window draw callback:

```lua
local Slots = dofile('lib/extensions.lua')
local function drawCaption()
  ui.Text('My window')
end
-- Inside the window's draw callback:
Slots.Component('MyWindow', 'caption', {title='My window'}, drawCaption)
```

An addon can hide that caption and later restore its private default from the same owner:

```lua
local E = hs.UI.extensions
local handle = E.RegisterSection('MyWindow','caption','replace','hide-caption',function() end)
local function restoreCaption()
  if handle then E.Remove(handle); handle=nil end
end
```

An existing element without a slot needs a Lua anchor or window presenter override, requiring no new C++ API method.
Slots address emitted Lua components; a registration for an un-emitted slot has no effect.

`Remove(handle)` removes only the current owner's registration. `IsRegistered(handle)` is callback-free
lifecycle discovery visible to the owner and trusted root UI. It remains true across temporary permission
loss or missing selection, while metadata becomes disabled and invocation is denied. Source reload, source
failure, owner removal or VM/host destruction retires its handles; a native owner's authority expanding beyond
the registration's captured ceiling also retires it. Idle lifecycle maintenance releases retired callback
captures without an action query. A failed Lua callback follows the existing source-file error policy:
callbacks from that file retire, while the calling built-in window and other source files remain usable.

Callbacks run as their registered source under the intersection of captured and live permissions. Invocation
is limited to trusted root UI or the same owner; foreign plugins cannot borrow another owner's authority,
including root UI services. Native callbacks are refused when their live host authority exceeds the captured
or current caller scope. Dispatch nesting is limited to eight. Saved API function or table proxy identity is
not a lifecycle token: retain opaque handles and use `IsRegistered` to repair retired registrations.

## Color controls for interface and addon settings

The shared `ui` drawing library provides `ColorButton(label,r,g,b[,a,flags,width,height]) -> pressed`
and `ColorPicker3` / `ColorEdit3` `(label,r,g,b[,flags]) -> r,g,b,changed`. Components are normalized
0..1 values. The default picker is a hue wheel with RGB integer editing; `ui.ColorEditFlags` exposes the
supported options. Swatches can remain in the main settings row while a popup holds the full picker.
Nonfinite values and calls outside an active ImGui frame are refused. Unsupported/conflicting flags are
normalized, alpha editing is disabled for these three-component controls, and global picker preferences are unchanged.

## Interface discovery and loading

Options lists built-in interfaces and installed `kind ui` addons together. Installing a UI addon registers its
interfaces automatically; players select one in Options rather than starting a Lua script. A selection is saved
for the current server and applied between frames, after the current UI callback returns.

`API.interfaces.List()` returns the registry entries in selector order. Each entry includes `id`, `name`,
`description`, `input`, `source` (`builtin` or `addon`), `addon`, `base`, `bases`, `runtime`, `available` and `reason`.
Use the stable `id` for identity, since display names need not be unique. A missing backend leaves the entry
visible with `available=false` and an explanation.

`API.interfaces.Status()` describes the current load. `selected` is the saved/requested entry and `loaded` is the
effective frontend, each with the registry metadata above. `state` is `loading`, `ready`, `pending`, `fallback` or
`error`. `bases` lists the effective base chain. `scriptCount`, `loadedScriptCount`, `errorCount` and `windowCount`
report the loader's actual results, and `diagnostics` contains its messages. An unavailable requested interface
can remain selected while an available interface provides the fallback. Modern and Gamepad show this data in Options.

UI addons replace the trusted client interface and have full access to the host UI API, including its account and
game actions. Their install card identifies that access. They are different from capability-limited plugin or
script windows; authors must not describe `caps none` on a UI addon as a sandbox or a lack of access.

## Drawing image assets

`API.assets.Image(relativeGamePath)` returns an opaque texture handle, pixel width
and pixel height, or a single nil when the image is unavailable. It reads raster
images from the indexed game-asset layers, including the active server's asset
overlay. Supported formats are DDS, TGA, PNG, BMP and JPEG. Paths are relative,
case-insensitive and may use either separator. Absolute paths, traversal,
ambiguous path components, control characters and other file types are refused.

```lua
local texture, width, height = API.assets.Image("images/interface-atlas.png")
if texture then
  ui.DrawImage(texture, x, y, x + width, y + height, 0, 0, 1, 1)
end
```

The caller supplies its own layout and atlas UV coordinates. The API does not
read layout metadata, write to the game folder or expose arbitrary files.
Explicit image requests are independent of `game_art`; `assets.Texture` retains
its existing default-art behavior. Image handles use the client's existing
bounded texture cache and deferred GPU retirement. Reacquire the handle when
drawing: an evicted or retired handle stops drawing safely.

API 1.7 adds `API.assets.InterfaceImage(relativeUiPath)` for raster artwork
shipped with an interface. It returns the same texture, width and height, or a
single nil. In the managed UI VM it searches the selected interface folder and
its approved bases in order; a plugin window first searches its own active
folder. It never searches another plugin, `ui-data` or arbitrary disk folders.
The path uses the same raster formats and restrictions as `Image`, but preserves
case, so use the package's exact spelling on every platform. Links outside an
approved folder are refused. Saved plugin closures retain their owner and stop
resolving files when that owner is removed.

```lua
if API.Supports("assets", "InterfaceImage") then
  local texture, width, height = API.assets.InterfaceImage("assets/atlas.png")
  if texture then
    ui.DrawImage(texture, x, y, x + width, y + height, 0, 0, 1, 1)
  end
end
```

This lets an addon include its own artwork without an extra local install.
Images share the bounded texture cache, keyed by their resolved package file;
replacing a file with a changed size or modification time refreshes it on the
next request. Each file is limited to 64 MiB, the UI archive's unpacked limit.
Reacquire handles when drawing as with `Image`. Outside the managed UI VM, or
without an artwork provider, `InterfaceImage` returns nil.

For custom widget artwork, `ui.IsItemDisabled()` observes the last submitted
widget's disabled state, including after `ui.EndDisabled()`. It preserves that
widget's identity and input behavior, like the other item-state queries.
`ui.GetStyleAlpha()` returns the current style opacity, including a disabled
scope's factor. The `ui.Draw*` bindings already apply that opacity through
ImGui's color conversion.
`ui.GetItemRect()` retains the total item bounds. Passing true returns the native
interaction frame instead, which excludes the external label on a slider.
Both forms preserve the last item's ID, value and disabled state.

## Data and actions

The API aliases the client's existing finalized functions. Arguments, return
values, nil/false results, caching, list invalidation and action checks keep
their existing meanings. Flat `hs.*` bindings remain available for low-level
host integrations. Modern, Gamepad and new UI packs use `hs.UI`.

`eq`, the name up to 0.7, stays as a permanent alias of the same guarded `hs`
view everywhere: in the built-in Modern and Gamepad packs, in add-on interfaces,
in compiled-plugin and script UI folders and in the player's own files. Code
written against `eq` keeps working with no end date; it grants no additional
permissions, mutable host tables or callback identity, and source-bound callback
factories and saved functions keep their reload and owner checks. `hs` is the
name new code uses (`hs.UI`).

Examples inside a window's draw callback:

```lua
local API = hs.UI

local shown = API.windows.Shown("InventoryWindow")
if not shown and ui.Button("Open inventory") then
  API.windows.Show("InventoryWindow", true)
end

local enabled, checked = API.windows.ChildState("InventoryWindow", "IW_DoneButton")
if shown and enabled and ui.Button("Done") then
  API.windows.ClickChild("InventoryWindow", "IW_DoneButton", "left")
end

local tasks = API.tasks.List()
local revision = API.tasks.Revision()
-- Open an existing quest using its received task ID:
-- API.tasks.Open(task.id)
```

These are game-window operations, not arbitrary setters for game state.
Actions still run the existing window handler. Buying, trading, destroying,
casting and similar operations retain their permission, state and server checks.

Some presentation queries have deliberate effects. In particular,
`windows.Shown` retains `hs.WindowShown`'s claim of a Lua replacement where that
is applicable; `windows.Replace` claims the window too. Follow the current
[window contract](dev-ui.md), rather than treating every zero-capability call
as a side-effect-free getter. `requiredCaps = 0` means no outer capability gate;
individual functions can still check caller identity or screen state themselves.

## Server add-ons warning

API 1.6 adds `addons.Policy()` and `addons.PolicyAction(action, serial)` (flat `hs.AddonPolicy`,
`hs.AddonPolicyAction`). `Policy()` is nil unless a server's required add-on has failed once the character is in the
world; then it is `{serial, message, can_retry}`, the same table until the failure changes (read it, do not change
it). `PolicyAction("retry", serial)` tries the failed required add-ons again once and returns whether it did. Both
are the client's own interfaces' alone: an add-on or hand-placed interface, and plugin or script code, get nil and
false, and cannot replace the client's "AddonPolicy" window (docs/plugins.md "Opt-in add-on policies").

## Class names

API 1.7 adds `character.ClassName(id[, short])` (flat `hs.ClassName`) and `character.ClassNamesRevision()`
(`hs.ClassNamesRevision`). `ClassName` is the class's name as the client shows it, or its short name with `short`: a
server's own where its plugin or script named the class (`set_class_name`, docs/plugins.md), else the stock one.
Text the client hands an interface (labels, list rows, `className` at character select) already carries those names;
an interface that maps such text back to a class, or has its own list of class names, asks `ClassName` by id and
keys any cache on `ClassNamesRevision()`, which changes whenever the names may have.

## Inventory placement

UI API 1.8 adds `inventory.CanPlace(place)` (`hs.CanPlace`). It previews whether
the item on the cursor fits an empty carried, personal bank or shared bank slot
(`i:26:0`, `b:3`, `s:0:5`). Bank slots require an open bank. The query applies
the native bag and bank restrictions, and the slot action validates the move
again at the time of the click. Item records from `inventory.Item`,
`inventory.AllItems`, and other inventory slot readers include the server's
`questItem` flag. Bag records also include `bagType` and `bagSize`. Tradeskill
storage bags use type 58. The fit check uses the material's tradeskill flag.

## Discovering optional features

```lua
local API = hs.UI

if API.Supports("achievements", "List") then
  local state = API.achievements.List()
  -- The received state determines what this server currently offers.
end

local description = API.Describe("tasks", "Open")
-- {name="Open", binding="OpenTask", kind="function", requiredCaps=...}
```

`Supports(module)` reports whether a module exists.
`Supports(module, member)` reports whether that member exists in this host.
It does not grant permission, establish that the current server supplies data,
or promise that an action is available in the present game state.
Use the method's returned state and existing enabled/visible flags for those
decisions. An unsupported optional feature should leave the rest of a UI usable.

`Describe()` returns the version and modules in deterministic order. Each module
has a `name` and `methods` array. Each member records its public `name`, `binding`
path, `kind` (`function` or `constant`) and `requiredCaps` bit mask.
`Describe(module)` returns one module; `Describe(module, member)` returns one
member. Unknown descriptions return nil. Constants use the same module lookup
as methods, for example `API.inventory.CursorSlot`.

## Compatibility policy

`API.version` contains independent `major` and `minor` contract numbers.
Additive methods and fields can extend a major version; existing method
arguments, results and behavior remain compatible within that major.
Interfaces should ignore fields they do not use and feature-check optional
methods. A breaking change needs a new major contract and an explicit migration.

This API version is independent of the Hailstone release version and the plugin
SDK ABI. It also describes host support, not the capabilities of a particular
server. Flat host bindings under `hs` use the same guarded closures as the
namespaced methods.

## Service policy and observed identities

API 1.4 adds read-only `hs.UI.map.Policy()`, returning `custom_spawns`, `stock_npcs`, `npc_find`, `name`,
`revision` and `context`. The last two are opaque decimal strings: compare them only for equality; do not parse,
persist or treat them as authorization. `revision` tracks provider identity, relevant content and eligibility
changes, including same-name replacements. `context` changes across connection, zone and player-incarnation
boundaries. Clear retained rows, hover, selection and menus when either changes; a display name is not identity.
Without an active policy the presentation values are `false`, `true`, `true` and `""`. Stock presenters re-read
policy and current row eligibility before displaying Find NPC rows, markers and controls, and preserve saved
preferences. `custom_spawns` selects an exclusive extension-marker provider; the other flags suppress stock NPC
presentation and NPC Find independently. PC group/raid Find, coordinates, static map labels, user pins, Tracking,
nameplates and raw spawn reads keep their separate rules.
Tracking invalidates source-owned work on provider lifecycle changes, without retiring it for map-only content changes.
API 1.5 adds opaque decimal-string `source`, the provider lifecycle epoch independently of map content revisions.
It distinguishes provider replacement and activation/permission transitions without treating a preference edit as
a new source. Treat a missing `source` on an older host as an optional unavailable field (the map presenter uses
`"0"` as its fallback); never infer authorization from it. No native SDK record field is added for this UI metadata.

`hs.UI.find.Npcs()` rows include opaque `context`, `incarnation` and `token` strings. Retain the row's token with
its id, then pass it to `NpcTarget(id[, token])`, `NpcPath(id[, token])` or `NpcWalk(id[, token])` when acting on
that observed row. A stale token fails closed if its actor, player, context or policy source has changed, even
when a numeric id is reused. These tokens are short-lived observations, not authority. Calls without a token
remain compatible as fresh intent against the currently eligible actor; every action still checks live policy.
`Supports` describes the host's structural contract, not a current permission, eligibility or server guarantee.

### Shared service invariants

Use the same observation/lifecycle pattern when adding another service. Extend existing records with optional
fields before inventing another window-specific adapter. Unknown fields may be ignored, while an absent method
can be detected with `Supports`. A new host feature still needs its own implementation and contract; interfaces
can adopt it through the shared API or inherit its base presentation.

| Service area | Observation and invalidation | Activation boundary |
| --- | --- | --- |
| Map, minimap and Find | Policy revision/context, frame-fresh eligibility and actor tokens; cache geometry separately | Reconcile retained menus/selection and validate the observed actor again |
| Guides and navigation | Actor references and origin/goal/context request leases | Replacements, stops, timeouts and uncorrelated replies cannot activate another goal; local/manual intent stays separate |
| Tracking | Its own current skill/provider admission, source context and received actor identities | Apply the same gate to skill, window, refresh, selection and delayed replies; map presentation policy is independent |
| Group and raid actions | Roster revision, owner/member incarnations and current selection | Revalidate explicit actions and reviews; server-specific authority stays in the addon |
| Item and loot presentation | Current inventory/received-item identity, provider revision and escaped text | Preserve current item/action checks and retire stale provider-derived presentation |
| Nameplates and world geometry | Owner, frame/epoch and current entity claims | Bound submissions and validate live ownership; invalid claims retain the stock presentation |
| Interface and addon lifecycle | Source generations, approved roots and captured capability ceilings | Reload or removal retires captured work; switching occurs outside the running callback |
| Native and script providers | The same policy fields and intent domains, with source/content revisions | Enforce live permissions and preserve older size-checked SDK prefixes, including legacy alignment |

Policies apply to their declared intent domain. A map ownership claim is not a global targeting, spawn-data,
tracking or nameplate prohibition. Keep authorization out of display-only caches, do not persist transient
tokens as preferences, and do not use a provider's display name as identity. Retain saved user preferences so
ordinary behavior returns when an optional provider is no longer active.

## Owned method hooks

API 1.3 adds `hs.UI.hooks.Register(method, phase, callback[, options])`,
`Remove(handle)` and `List([method])` within the UI VM. `method` is the exact
canonical name, for example `"character.Gauge"`, without `hs.UI.`. A registration
returns an opaque handle, or nil when the method, permissions or registration
budget do not allow it. Registry refusals also return a reason; an outer
permission gate may return nil alone. Remove returns a boolean. List returns
data-only records owned by the registering source, with handle, id, method,
phase, priority, order, owner, enabled and unavailableReason. A method query also has hookable, policy and
reason fields. `API.Describe(module, member)` exposes hookable, hookPolicy and
hookReason independently of current permissions.

Options are a plain table with optional `id` (1..64 printable bytes) and integer
`priority` (-1000..1000, default 0). Lower priorities run first, then registration
order. The same source registering the same method, phase and id replaces that
registration. A dispatch takes a snapshot; a new registration starts on the
next call, while a removed registration stops immediately.

- `before(args)` receives a copied packed argument table (`args.n` preserves nil
  positions). Its returns and mutations are ignored.
- `after(results, args)` receives copied packed return values and original
  arguments. Its returns and mutations are ignored.
- `around(next, ...)` receives copied arguments as varargs and returns replacement
  results as ordinary Lua multiple returns. It may call `next(...)` once to pass
  arguments to the remaining around hooks and original guarded method. `next`
  is synchronous: retaining, repeating, yielding or calling it as another owner
  cannot perform a later or duplicate action.

Eligible before observers run first, eligible around hooks form a chain in the same priority
order, and after observers run last on the resulting values. Original errors,
including nil or false payloads, remain errors and are never retried. A failing
observer is retired and leaves the call unchanged. A failing around hook uses
its already-produced downstream result if next ran; otherwise the remaining
chain receives the original arguments. No error path repeats the original.

Arguments/results must be acyclic plain data: nil, booleans, numbers, strings
and tables with string/number keys and no metatable. At most 64 arguments or
results, 4096 nodes, 64 KiB of strings/keys and 16 table levels are copied. Data
outside these limits bypasses observation and reaches the original caller
unchanged. Even if an around callback catches a result-limit error, its
replacement cannot conceal or truncate the original result. A budget skip does
not retire a valid registration. Unmodified pass-through copies recover the
original argument/result table identities; explicit modifications become new
data. Without hooks, the original closure is called directly.

The catalog explicitly opts in audited plain-data methods: initially
character.Gauge, character.Label, character.Name, tasks.Open and tasks.Revision.
A capability mask alone never makes a method hookable. Pregame/account access,
imports, addon management, owner-specific plugin services, one-shot requests and
all other unaudited methods stay closed. UI_HOOK_POLICY annotations require an
argument/result and caller-guard audit before additional methods are enabled.

Hook owners need the existing UI, Read and Extend permissions plus the method's
permissions. These are existing policy bits, not new consent or SDK fields.
The original caller is checked before any arguments/results are exposed. Each
callback runs with the intersection of that caller, the owner's captured
registration ceiling and its live permissions. Source reload/removal, permission
revocation, a closed VM or a destroyed host retires registrations.
Root-UI callbacks require the root UI role; sharing every capability bit never
grants that role to a plugin. Distinct addon owners cannot trigger each other's
callbacks. Native-owner callbacks also require all their live native authority
to fit the effective caller/registration ceiling, since native host queries
can otherwise use owner-wide permissions. A narrower caller skips the callback
without retiring it; List reports the current unavailable reason. Retained
aliases see current registrations while retaining their original guarded host
closure. Limits are 128 registrations per VM, 16 per source and 16 per method;
after 8 nested hooked calls, further calls use the original guarded method.
This service does not install hooks across independent script/plugin Lua states.

## Adding a new client feature

1. Implement its authoritative model and public Lua binding in the client, with
   its normal tests. Keep server-specific implementations in their addons.
2. Register the binding in its existing registrar, then add its public module,
   member and capability declaration to `src/app/lua_ui_api.inc`, keeping entries
   sorted by module and member.
3. Document arguments, results, state transitions and unavailable/denied results.
   Regenerate the reference with `python tools/gen_ui_api_docs.py` and its
   descriptor-derived test fixture with `python tools/migrate_ui_api.py --mock-adapter`.
4. Any interface can immediately call `API.<module>.<Member>(...)` or check it
   with `API.Supports`. It needs no additional adapter in that UI pack.

The catalog owns both capability metadata and the public names. Registration
rejects missing, unlisted, duplicate or mistyped entries instead of silently
publishing a partially defined API. Tests check the descriptor against the real
registered functions, constants and permission gates.

The client publishes aliases **after** capability gates and cache invalidation
wrappers are installed. Do not register a raw C function again to create an
alias: that can bypass its existing wrapper or separate state that should be
shared. Aliases must retain the original closure, upvalues and caller scope.

A data/action method becoming available does not automatically design its
feature's screen. UI authors choose its presentation. Pack inheritance supplies
the base interface's presentation when a derived interface has not overridden it.

## Starting another UI

Create `ui/my-interface/ui.ini`:

```ini
name=My Interface
description=My presentation over the shared Hailstone UI API.
base=modern
input=mouse
```

An optional `base=modern` or `base=gamepad` inherits existing windows and helper
libraries. Add a top-level Lua file with the same relative name to replace that
window, or override a helper such as `lib/theme.lua`. Files in the derived pack
take precedence; other files come from its declared base chain. Removing an
override restores the inherited file. Settings remain the selected interface's.
Cycles, invalid parents and chains longer than 32 packs are rejected. Base
folders must stay within approved registered roots: the installed `ui/` tree
or an installed UI addon's owned `ui-addons/<addon>/ui/` tree in the data folder.
Within a managed bundle, unqualified bases resolve locally before built-ins;
use `builtin:modern` or `addon.<addon>.<pack>` for an explicit reference.
Descriptors with an unsupported runtime remain visible as unavailable when
their declared base chain is valid. Only the Lua runtime is supported.

A helper can decorate its inherited implementation through
`API.interfaces.LoadBase(relativeLuaFile)`. For example, `lib/theme.lua` can
import the base theme, wrap its drawing functions and return that theme table.
Inherited presenters then use the decorated helper through their normal
`dofile("lib/theme.lua")` call, including presenters added by later client updates.

```lua
local API = hs.UI
local theme = API.interfaces.LoadBase("lib/theme.lua")
-- Decorate the inherited drawing functions here.
return theme
```

The import searches only approved bases, starting after the caller's source pack;
a selected-pack helper starts with its nearest base. This allows several derived
helpers to decorate one another without importing themselves. All return values,
including nil positions, are preserved. Files run as text in the current UI VM,
with the caller's environment, capabilities and presenter-file ownership.
Plugin callers retain their own globals and permissions and cannot select another
plugin's folder. Retained imports stop working when their owner is removed.
Absolute paths, traversal, escaped links, selected-root files and recursive
file imports are refused. Errors clear import state before propagating to Lua.

Shared presentation modules can live outside manager-owned code, under
`<Hailstone data>/ui-data/<stable interface ID>/`. Use
`API.interfaces.LoadData(relativeLuaFile[, baseOnly])` to execute a text Lua
module there and receive all of its return values. The default search is the
selected interface's data, followed by its approved base IDs in order; true skips
the selected ID. A sparse derived interface can inherit a base's presentation
data without copying it into the manager's installed code files.

These roots contain shared, non-secret presentation material such as styles and
layouts. They are separate from credentials, private settings and plugin folders.
Modules retain the caller's environment, capability checks, owner lifetime and
presenter ownership. The same path, text-only, recursion and protected-error
rules as `LoadBase` apply, including checks against escaped data folders.
An ID directory cannot redirect to another ID, even inside the shared data root.
`LoadData` does not interpret a data schema or write any files. Modules load when
called; a cached returned table needs an interface/window reload after an edit.
Core updates preserve `ui-data/`, and add-on updates/removal leave this player
data available for a future reinstall.

The registry refreshes about once a second and after add-on manager changes.
Changing `ui.ini` resolves the metadata and base chain again and queues a reload
when it changes the active interface. Top-level window additions and replacements
follow the usual script scan. Manual helper-only edits need a window or full UI
reload because scripts retain the helper functions they already loaded. Installed
package updates also compare the recorded content generations of the selected
bundle and its approved bases, so helper-only updates trigger a reload even when
`ui.ini` is unchanged. A derived window that replaces a whole base file also owns
subsequent changes to that window's presentation.

The original example under [sdk/ui/example](../sdk/ui/example/) overrides only
the player window while retaining Modern's remaining windows. It is a starting
point for an interface, not another automatically installed interface choice.

Each pack retains its own presentation helpers. Modern's HUD/widgets and
Gamepad's navigation/window helpers serve different input styles. Both use the
same `hs.UI` data and action methods. Declared inheritance lets a derived pack
reuse those helpers through approved pack roots. `dofile` cannot escape those
roots through `..` traversal or links, or access arbitrary sibling folders.

## Migration and checks

```sh
python tools/migrate_ui_api.py --pack modern --pack gamepad --check
python tools/migrate_ui_api.py --mock-adapter --check
python tools/gen_ui_api_docs.py --check
```

The migration tool reads the same catalog, changes Lua code tokens, and leaves
comments and quoted strings alone. Unknown APIs require a deliberate review.
It does not synthesize fake support for a missing binding.

Run the whole client test suite after UI changes. The real Lua/ImGui pack tests
exercise Modern and Gamepad against the host API; contract tests check closure
identity, discovery, denied actions and caller/lifetime restrictions. Fixture
adapters belong in tests only and must never become production fallbacks.
