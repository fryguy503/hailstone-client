# Modular UI API

Every interface (Modern, Gamepad, any other pack) draws its own presentation over the same client-owned `hs.UI`
contract. The client owns game state, window models, actions and permission checks; an interface does not duplicate
game rules, packets or server-specific logic.

```lua
local API = hs.UI
assert(API.version.major == 1, "This interface requires UI API 1")

if API.session.InGame() then
  local name = API.character.Name()
  local hpFraction, hpText = API.character.Gauge(1)
  -- Draw these values using this interface's widgets.
end
```

Use dot calls. Methods are grouped by service (`session`, `character`, `inventory`, `windows`, `chat`, `spells`,
`tasks`, `achievements`, `map`, `scripts`, `plugins`, `pregame`, `gamepad`, ...). The full generated mapping is in
[UI API Reference](ui-api-reference.md).

## Chat tabs and windows

`hs.UI.chat.Windows([into])` lists visible chat windows in slot order: `slot` identifies the window, `tab` is the
selected position in `tabs`, `title` the selected tab's label, `windowTitle` the window's title. A tab's `slot` stays
with it as it moves; slots may be reused after closing. Tab arguments are the current one-based position in `tabs`.
A per-frame caller can pass `into` to reuse the list and records; copy what must survive the next fill.

| Method | Behavior |
| --- | --- |
| `SplitTab(windowSlot, tab)` | Opens a separate window with the tab; returns its slot, or nil. |
| `JoinTab(windowSlot, tab, targetWindowSlot)` | Appends and selects the tab in another window. An empty source window closes. |
| `ReorderTab(windowSlot, tab, delta)` | Moves and selects the tab within its window (negative left, positive right). |
| `CombineTabs(targetWindowSlot)` | Appends tabs from other windows; returns the number moved. |
| `SetWindowTitle(windowSlot, title)` | Renames the window independently of its tab labels. |
| `SetTabFlash(windowSlot, tab, enabled)` | Whether background messages highlight the tab. |
| `SetTabFlashColor(windowSlot, tab[, rgb])` | A `0xRRGGBB` highlight; nil or omitted restores red. |
| `SetTabScrollbar(windowSlot, tab, enabled)` | Whether the tab shows an output scrollbar (`scrollbar` on the record). |

`NewTab`, `SelectTab` and `CloseTab` use the same tab order; `CloseWindow` closes a window and its tabs. The window
holding Main Chat cannot close. `SetTitle(windowSlot, title[, tab])` and `Clear(windowSlot[, tab])` act on the
selected tab when the position is omitted. Moving a tab keeps its lines, routing and preferences; temporary tell tabs
can move during the session.

Tab records expose `unread` (also with flashing off), `flash`, `flashColor` (nil for red) and `highlighted` (draw the
highlight this frame). A background message flashes the label every half second for five seconds, then leaves it
highlighted until selected. Mutations need `commands`. Feature-check with `hs.UI.Supports('chat', 'SplitTab')`.

## Optional world nameplate presentations

`hs.UI.nameplates` is a generic drawing service, also `require('hailstone').nameplates` for ordinary add-on scripts;
it needs no full interface. The UI facade publishes `Version`, the script module `version`; both are 1. Script
consumers need `read`, `ui` and `extend`; server policy and installed file ceilings apply.

`Register(name, callback)` returns an opaque owner handle, or nil and a reason; the name is local to the caller.
`Unregister(handle)` removes only the caller's own. A replacement, permission loss, script stop, file reload,
interface reload or VM shutdown retires its claims and click regions; saved functions cannot recapture authority from
an expired source file.

The callback runs in the world-plate pass. Its frame has `serial`, `now_ms`, `dt_ms` (monotonic, no shared epoch),
`font_size`, `font`, `viewport={x,y,w,h}` and `candidates`. Each candidate has a current-frame `token`, projected head
`x,y`, axial `depth`, `native_scale`, a read-only spawn snapshot (numeric id and incarnation) and `context` (the
assembled caption, target/self/group/raid state, distance, and only genuinely known health/resources/cast/aggro/marker
values). Tokens are drawing claims, not target-action handles.

`context.invisible` and `context.invisible_undead` flag observed regular and undead invisibility; `context.concealed`
also covers an untyped concealment update, Hide and invisibility versus animals. The host knows only your own and the
current target's server-supplied buffs: false means no positive evidence. Older hosts omit these fields.

The host applies its awareness, hidden-actor, range and frustum gates before publishing candidates. The add-on owns
appearance, text, bars, shapes, icons, animation, placement and stacking. The callback returns an array of models:

```lua
{token=candidate.token, bounds={x=-60,y=-18,w=120,h=18},
 offset_x=0, offset_y=0, through_walls=false, clickable=true,
 commands={{kind='RectFilled',x0=-60,y0=-18,x1=60,y1=0,color=0x344255,alpha=1}}}
```

Coordinates are window pixels relative to the projected head; positive offsets move right/down. Bounds and viewport
clip every command. Kinds: `Rect`, `RectFilled`, `Gradient`, `Line`, `Circle`, `CircleFilled`, `Triangle`,
`TriangleFilled`, `Text`, `Image`; the host has no health-bar, badge or preset recipes. Rectangle/line/image use
`x0,y0,x1,y1`, triangles add `x2,y2`, circles/text use `x,y`. Colors are integer RGB with separate 0..1 alpha. Text
adds `text,size,font`; images use a live texture and UVs; Gradient adds `color2,alpha2`.

`MeasureText(text,size,font=0)` returns advance `width,height` and glyph-ink bounds `ink_x0,ink_y0,ink_x1,ink_y1`
(font 0 the plate font, 1..6 shared UI fonts), from the current draw pass. Script `SpellIcon(icon)` returns a live
texture plus `u0,v0,u1,v1`, or nil; interface code can use `hs.UI.spells.Icon`.

An omitted token, nil answer, invalid command list or failed callback keeps the ordinary caption. `{token=...,hide=true}`
hides the whole caption. A valid nonempty model replaces the whole caption (secondary lines and target decoration
included) for that frame only. No registration changes global name switches. Without depth support, ordinary captions
stay rather than an untested overlay.

Limits:

- 32 registered owners and 1,024 candidates;
- 512 commands per model and 32,768 commands per frame;
- 65,536 estimated vertices per model and 262,144 per frame;
- text limited to 1,024 bytes per command; every parameter must be finite and bounded.

Models over the frame's geometry budget fall back individually, target first. Rounded rectangles use fixed
tessellation. Image handles are checked again at draw time.

`clickable` enables manual selection only. The host validates the displayed frame, owner, connection, zone and entity
incarnation, respects click-through masks and self-view rules, and keeps UI/modal, cursor and drag handling. With
`through_walls=false` the clicked ray must be clear of the scene and visible actors. Through-wall drawing bypasses no
awareness or server action rules.

## Named window extensions

API 1.5 makes `hs.UI.extensions` slots generic. A window and section are nonempty ASCII identifiers of at most 128
bytes (letters, digits, `_`, `.`, `:`, `-`). UI authors emit slots in Lua; a registration for an un-emitted slot does
nothing, and new slots need no client API update. Current presenters:

| Window | Section | Context |
| --- | --- | --- |
| `RaidWindow` | `member_actions` | The current selected raid member, including members outside the zone or without a group |
| `MapViewWnd` | `controls` | The current world/zone map view and copied presentation policy; no member selection |

Invalid identifiers are rejected. Selection, leader flags and map policy grant no authority. Every slot follows the
owner, capability and lifetime rules below.

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

- The capability floor is `read` plus `ui`; `requiredCaps` adds existing names such as `chat`, needed at registration
  and still live at execution.
- `priority` is an integer from -10,000 to 10,000; ties order by owner, id and handle.
- At most 32 registrations per owner and 128 per UI VM, display callbacks included.

Presenters keep add-on actions apart from stock controls, in compact wrapping rows with stable focus; Confirm and
Cancel get a review row. Gamepad Raid uses its full-width scrolling menu with an add-on actions heading. `priority`
sets the order; no other presentation metadata is needed.

`ListActions(window,section,context)` returns metadata only (`handle`, `owner`, `id`, `label`, `enabled`,
`requiresSelection`, `requiresReview`, `priority`), never calling a callback. API 1.5 adds a `sections` array
(`handle`, `owner`, `id`, `placement`, `enabled`, `priority`) listing visible display registrations, so a presenter
can find a display-only slot; older hosts omit it.

`InvokeAction(handle,phase,context[,reviewToken])` returns `{ok=boolean,message=string[,reviewToken=string]}`.

- An ordinary action accepts `invoke`.
- With `requiresReview=true` it accepts `review`, then `confirm` or `cancel` with the returned token. A successful
  review must give a nonempty message and token.
- `ok` must be a Lua boolean. Messages are at most 4,096 bytes, tokens 1,024.

The add-on validates its token, server, roster and permissions before acting; the registry confers no server
authority.

Context is a bounded data table. The raid presenters supply `selection={name,group,...}` and an advisory roster
`revision`; treat other fields as optional. Review compares the selection and context again before confirming; a
stale selection, owner or permission clears the pending UI without a callback. Only explicit user activation
dispatches phases.

The map presenters supply:

- `window='MapViewWnd'`, `section='controls'`, `view='world'|'zone'`;
- the current short zone name `zone`, the map's short `shown_zone`, and boolean `own_zone` (unavailable zone names
  are empty strings);
- a fresh copied `policy` from `hs.UI.map.Policy()`;
- `revision`: a length-framed concatenation of the view/zone values and the policy's context, source, revision, flags
  and name. Compare for equality only; never parse, persist or treat it as authority or a token.

Map callbacks get no `selection`, so `requiresSelection=true` actions stay disabled. The presenters re-read context on
activation and clear stale rows and reviews without callbacks. Connection/source/zone/view changes or retired
registrations close the panel; content revisions and preference edits refresh it and invalidate stale reviews.

The map's **Map controls** panel holds registered actions and before/after display callbacks. In Gamepad, LT enters
it; B returns (or cancels a review) before closing the map. While it owns input, map pan/zoom and Find shortcuts are
suspended.

Only the display points the interfaces offer take actions and sections; any other is an error (`RegisterAction`,
`RegisterSection`, `ListActions` and `DrawSection` alike). The offered points:

- `RaidWindow` `member_actions`;
- `MapViewWnd` `controls`;
- `MapViewWnd` `map.canvas`;
- the map toolbar's buttons, `MapViewWnd` `toolbar.<button>` (`toolbar.zin`, `toolbar.mapcontrols`, ...).

`RegisterSection(window,section,'before'|'after'|'replace',id,callback[,options])` registers a drawing callback
receiving `window,section,placement,context`. `DrawSection(window,section,placement,context)` is the presenter entry;
for `before`/`after` it returns the count of successful callbacks (zero outside a drawing frame).

API 1.5 adds `replace`, returning a boolean:

- Only the first eligible, visible registration runs (lower priority first, then owner, id, handle).
- A completed callback returns true even when it draws nothing, hiding the default.
- Missing, revoked or failed replacements return false, and a failing winner does not fall through to a second one:
  the presenter draws its default.
- Feature-check API version 1.5 before using `replace`; method discovery cannot tell.

Section callbacks use the same capability floor and `requiredCaps`/`priority`; action callbacks never run while
drawing. Each callback has its own widget-ID namespace and cannot pop the parent's window, table, tab, popup or style
scopes. Parent font scale, pending window/item settings and last-item state are restored afterwards; cursor advance
is kept.

UI authors emit before, replacement/default and after content with `E.Component(window,section,context,defaultDraw)`.

- The default function stays private to the presenter.
- An add-on removes an element with an empty successful `replace` callback; removing the handle restores it.
- This is cooperative composition, not access to internal windows or gameplay actions; un-emitted components are
  untouched.
- On older hosts or an unknown API major, `E.Component` calls its default directly.

A UI author emits a caption inside its window draw callback:

```lua
local Slots = dofile('lib/extensions.lua')
local function drawCaption()
  ui.Text('My window')
end
-- Inside the window's draw callback:
Slots.Component('MyWindow', 'caption', {title='My window'}, drawCaption)
```

An add-on hides it and later restores the default:

```lua
local E = hs.UI.extensions
local handle = E.RegisterSection('MyWindow','caption','replace','hide-caption',function() end)
local function restoreCaption()
  if handle then E.Remove(handle); handle=nil end
end
```

An existing element without a slot needs a Lua anchor or presenter override, not a new C++ method.

`Remove(handle)` removes only the owner's registration. `IsRegistered(handle)` (owner and trusted root UI) stays true
through temporary permission loss or missing selection, while metadata shows disabled and invocation is denied.

Handles retire on source reload, source failure, owner removal, VM/host destruction, or a native owner's authority
growing past the captured ceiling. Idle maintenance releases retired captures. A failed Lua callback follows the
source-file error policy: that file's callbacks retire; the built-in window and other files stay usable.

Callbacks run as their registered source, under the intersection of captured and live permissions.

- Only trusted root UI or the same owner may invoke; foreign plugins cannot borrow another owner's authority,
  root UI's included.
- Native callbacks are refused when their live host authority exceeds the captured or caller scope.
- Dispatch nesting is limited to eight.
- A saved function or proxy identity is not a lifecycle token: keep handles and use `IsRegistered`.

## Color controls for interface and addon settings

The `ui` library has `ColorButton(label,r,g,b[,a,flags,width,height]) -> pressed` and `ColorPicker3` / `ColorEdit3`
`(label,r,g,b[,flags]) -> r,g,b,changed`, with 0..1 components. The default picker is a hue wheel with RGB integer
editing; `ui.ColorEditFlags` lists the options. A swatch can sit in a settings row with the picker in a popup.
Nonfinite values and calls outside an ImGui frame are refused; unsupported or conflicting flags are normalized, alpha
editing is off, and global picker preferences are unchanged.

## Interface discovery and loading

Options lists built-in interfaces and installed `kind ui` add-ons together; installing one registers its interfaces.
A selection is saved per server and applied between frames, after the current UI callback returns.

`API.interfaces.List()` returns entries in selector order: `id`, `name`, `description`, `input`, `source` (`builtin`
or `addon`), `addon`, `base`, `bases`, `runtime`, `available`, `reason`. Use `id` for identity (names need not be
unique). A missing backend shows as `available=false` with a reason.

`API.interfaces.Status()` describes the current load: `selected` (requested) and `loaded` (effective) entries,
`state` (`loading`, `ready`, `pending`, `fallback`, `error`), `bases` (effective chain), `scriptCount`,
`loadedScriptCount`, `errorCount`, `windowCount` and `diagnostics`. An unavailable selection can stay selected while
another provides the fallback. Modern and Gamepad show this in Options.

UI add-ons replace the trusted client interface and have full access to the host UI API, account and game actions
included; their install card says so. They are not capability-limited plugin or script windows: never describe
`caps none` on a UI add-on as a sandbox.

## Drawing image assets

`API.assets.Image(relativeGamePath)` returns an opaque texture handle, width and height in pixels, or a single nil. It
reads DDS, TGA, PNG, BMP and JPEG from the indexed game-asset layers, the server's overlay included. Paths are
relative and case-insensitive, with either separator; absolute paths, traversal, ambiguous components, control
characters and other file types are refused.

```lua
local texture, width, height = API.assets.Image("images/interface-atlas.png")
if texture then
  ui.DrawImage(texture, x, y, x + width, y + height, 0, 0, 1, 1)
end
```

The caller supplies layout and UVs; the API reads no layout metadata, writes nothing and exposes no arbitrary files.
Explicit image requests ignore `game_art`; `assets.Texture` keeps its default-art behaviour. Handles use the bounded
texture cache: reacquire each draw, since an evicted handle stops drawing safely.

For custom widget art:

- `ui.IsItemDisabled()` reports the last widget's disabled state, also after `ui.EndDisabled()`, without changing its
  identity or input.
- `ui.GetStyleAlpha()` returns the current style opacity, a disabled scope's factor included; `ui.Draw*` already
  applies it.
- `ui.GetItemRect()` returns the total item bounds, or with true the interaction frame (without a slider's external
  label). Both keep the last item's ID, value and disabled state.

## Data and actions

The API aliases the client's existing functions: arguments, returns, nil/false results, caching, list invalidation and
action checks keep their meanings. Flat `hs.*` bindings stay for low-level integrations; packs use `hs.UI`.

`eq`, the name up to 0.7, is a permanent alias of the same guarded `hs` view everywhere (built-in packs, add-on
interfaces, plugin and script UI folders, the player's files). It grants no extra permissions, mutable host tables or
callback identity; source-bound callbacks and saved functions keep their reload and owner checks. New code uses `hs`.

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

These are game-window operations, not state setters: actions run the window's handler, and buying, trading,
destroying, casting and the like keep their permission, state and server checks.

Some queries have deliberate effects: `windows.Shown` keeps `hs.WindowShown`'s claim of a Lua replacement, and
`windows.Replace` claims the window too. Follow the [window contract](dev-ui.md) rather than treating every
zero-capability call as a pure getter. `requiredCaps = 0` means no outer gate; functions may still check caller or
screen state.

## Server add-ons warning

API 1.6 adds `addons.Policy()` and `addons.PolicyAction(action, serial)` (flat `hs.AddonPolicy`,
`hs.AddonPolicyAction`).

- `Policy()` is nil unless a server's required add-on has failed once in the world; then
  `{serial, message, can_retry}`, the same table until the failure changes (read-only).
- `PolicyAction("retry", serial)` retries the failed required add-ons once and returns whether it did.
- Both are for the client's own interfaces: add-on or hand-placed interfaces and plugin or script code get nil and
  false, and cannot replace the "AddonPolicy" window (docs/plugins.md "Opt-in add-on policies").

## Class names

API 1.7 adds `character.ClassName(id[, short])` (flat `hs.ClassName`) and `character.ClassNamesRevision()`
(`hs.ClassNamesRevision`).

`ClassName` is the class's displayed name, or short name with `short`: the server's where its plugin or script named
it (`set_class_name`, docs/plugins.md), else the stock one. Text the client hands over (labels, rows, `className` at
character select) already carries these. An interface mapping text back to a class, or with its own class list, asks
`ClassName` by id and keys caches on `ClassNamesRevision()`.

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

`Supports(module[, member])` reports whether a module or member exists in this host. It grants no permission and
promises neither server data nor a currently available action; use returned state and enabled/visible flags for
those. An unsupported optional feature should leave the rest of a UI usable.

`Describe()` returns the version and modules in deterministic order, each with `name` and `methods`; each member has
`name`, `binding` path, `kind` (`function` or `constant`) and `requiredCaps` mask. `Describe(module)` and
`Describe(module, member)` narrow it; unknown ones return nil. Constants use the same lookup (`API.inventory.CursorSlot`).

## Compatibility policy

`API.version` has independent `major` and `minor` numbers. Additions may extend a major; existing arguments, results
and behaviour stay compatible within it. Ignore unused fields and feature-check optional methods. A breaking change
needs a new major and an explicit migration.

The API version is independent of the Hailstone release and the plugin SDK ABI, and describes host support, not a
server's. Flat `hs` bindings use the same guarded closures as the namespaced methods.

## Service policy and observed identities

API 1.4 adds read-only `hs.UI.map.Policy()`: `custom_spawns`, `stock_npcs`, `npc_find`, `name`, `revision`,
`context`.

- `revision` and `context` are opaque decimal strings: compare for equality only; never parse, persist or treat as
  authorization.
- `revision` tracks provider identity, content and eligibility (same-name replacements too); `context` changes across
  connection, zone and player-incarnation boundaries. Clear retained rows, hover, selection and menus when either
  changes. A display name is not identity.
- Without an active policy the values are `false`, `true`, `true` and `""`.

The flags:

- `custom_spawns` selects an exclusive extension-marker provider; the others suppress stock NPC presentation and NPC
  Find independently.
- Stock presenters re-read policy and row eligibility before showing Find NPC rows, markers and controls, and keep
  saved preferences.
- PC group/raid Find, coordinates, static labels, user pins, Tracking, nameplates and raw spawn reads keep their own
  rules.
- Tracking invalidates source-owned work on provider lifecycle changes, not map-only content changes.

API 1.5 adds opaque string `source`, the provider lifecycle epoch: it changes on provider replacement and
activation/permission transitions, not on content revisions or preference edits. Treat it as optional on older hosts
(the map presenter falls back to `"0"`); never infer authorization from it. No native SDK record field is added.

`hs.UI.find.Npcs()` rows include opaque `context`, `incarnation` and `token` strings.

- Keep a row's token with its id and pass it to `NpcTarget(id[, token])`, `NpcPath(id[, token])` or
  `NpcWalk(id[, token])`.
- A stale token fails closed if its actor, player, context or policy source changed, even with a reused id. Tokens are
  short-lived observations, not authority.
- Calls without a token stay compatible, as fresh intent against the eligible actor. Every action checks live policy.

`Supports` describes the host's structure, not a permission, eligibility or server guarantee.

### Shared service invariants

Use the same observation/lifecycle pattern for a new service. Extend existing records with optional fields before
adding a window-specific adapter; unknown fields may be ignored and absent methods detected with `Supports`. A new
host feature still needs its own implementation and contract.

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

A policy applies to its declared intent domain: a map ownership claim does not forbid targeting, spawn data, tracking
or nameplates. Keep authorization out of display caches, never persist transient tokens as preferences, never use a
display name as identity, and keep saved preferences so ordinary behaviour returns when a provider goes.

## UI contributions

API 1.8 adds `hs.UI.contributions`. Packs can use `hs.UI.Supports("contributions", "List")` to detect the service.
`List(window)` returns plain records
for the window's current contributions. Each record has `owner`, `plugin`, `id`, `target`, `text`, `disabled` and
`action_label`. They are ordered by owner and then ID. An add-on's own Lua window can read its own contributions.
The pack draws these records at the anchors it supports. An empty `target` is the window's notice area.

Use stable target IDs for sections and controls. Show all applicable notices with their source. A control is
unavailable when any record for it sets `disabled`. Make the control non-interactive in this case and keep its
reasons and any actions usable. The setting value is still owned by the existing settings service.

On an explicit action click, call `Activate(window, owner, id)`. The host resolves the current registration and
checks its owner's current permissions before dispatch. It returns false if the action is unavailable.

Stock Modern and Gamepad Options use `OptionsWindow` with a window notice area, `section:display`,
`section:graphics`, `section:nameplates` and `setting:<id>` for supported option rows. Future presenters can use
the same API with their own window and target IDs. See [plugin windows](plugins.md#adding-to-a-packs-window) for
registration APIs.

## Owned method hooks

API 1.3 adds `hs.UI.hooks.Register(method, phase, callback[, options])`, `Remove(handle)` and `List([method])` within
the UI VM.

- `method` is the canonical name without `hs.UI.` (`"character.Gauge"`).
- Register returns an opaque handle, or nil when the method, permissions or budget refuse; registry refusals add a
  reason, an outer permission gate may return nil alone. Remove returns a boolean.
- List returns the caller's own records (handle, id, method, phase, priority, order, owner, enabled,
  unavailableReason); a method query adds hookable, policy and reason.
- `API.Describe(module, member)` exposes hookable, hookPolicy and hookReason regardless of permissions.

Options: optional `id` (1..64 printable bytes) and integer `priority` (-1000..1000, default 0). Lower priorities run
first, then registration order. The same source, method, phase and id replaces a registration. A dispatch takes a
snapshot: new registrations start on the next call, removed ones stop at once.

- `before(args)` gets a copied packed argument table (`args.n` keeps nil positions); returns and mutations are
  ignored.
- `after(results, args)` gets copied packed results and the original arguments; returns and mutations are ignored.
- `around(next, ...)` gets copied arguments and returns replacement results. It may call `next(...)` once to continue
  the chain to the original guarded method. `next` is synchronous: retaining, repeating, yielding or calling it as
  another owner cannot perform a later or duplicate action.

#### Hook order and errors

Before observers run first, then the around chain in priority order, then after observers on the resulting values.

- Original errors, nil or false payloads included, stay errors and are never retried.
- A failing observer is retired and leaves the call unchanged.
- A failing around hook uses its downstream result if `next` ran; otherwise the rest of the chain gets the original
  arguments.
- No error path repeats the original.

#### Hook data limits

Arguments and results must be acyclic plain data (nil, booleans, numbers, strings, tables with string/number keys and
no metatable): at most 64 values, 4096 nodes, 64 KiB of strings/keys and 16 table levels.

- Data past these limits bypasses observation and reaches the caller unchanged; an around callback that catches a
  result-limit error cannot conceal or truncate the original result. A budget skip does not retire the registration.
- Unmodified pass-through keeps the original table identities; modifications become new data.
- Without hooks, the original closure is called directly.

#### Hookable methods

The catalog opts in audited plain-data methods: character.Gauge, character.Label, character.Name, tasks.Open and
tasks.Revision. A capability mask alone never makes a method hookable. Pregame/account access, imports, add-on
management, owner-specific plugin services, one-shot requests and all other unaudited methods stay closed; enabling
more needs an argument/result and caller-guard audit (the UI_HOOK_POLICY annotations).

#### Hook permissions

Hook owners need UI, Read and Extend plus the method's permissions (existing policy bits, no new consent or SDK
fields).

- The original caller is checked before anything is exposed.
- Each callback runs with the intersection of the caller, the owner's captured ceiling and its live permissions.
- Source reload/removal, permission revocation, a closed VM or a destroyed host retires registrations.
- Root-UI callbacks need the root UI role, which no set of capability bits grants a plugin.
- Distinct add-on owners cannot trigger each other's callbacks.
- Native-owner callbacks also need all their live native authority within the effective ceiling.
- A narrower caller skips the callback without retiring it; List reports why.
- Kept aliases see current registrations while keeping their original guarded closure.

Limits: 128 registrations per VM, 16 per source, 16 per method. After 8 nested hooked calls, calls use the original
method. Hooks do not cross independent script/plugin Lua states.

## Adding a new client feature

1. Implement its authoritative model and public Lua binding in the client, with tests. Server-specific
   implementations stay in their add-ons.
2. Register the binding in its registrar, then add its module, member and capability declaration to
   `src/app/lua_ui_api.inc`, sorted by module and member.
3. Document arguments, results, state transitions and unavailable/denied results. Regenerate the reference with
   `python tools/gen_ui_api_docs.py` and the test fixture with `python tools/migrate_ui_api.py --mock-adapter`.
4. Any interface can then call `API.<module>.<Member>(...)` or check `API.Supports`, with no adapter.

The catalog owns capability metadata and public names; registration rejects missing, unlisted, duplicate or mistyped
entries, and tests check the descriptor against the registered functions, constants and gates.

Aliases are published **after** capability gates and cache-invalidation wrappers are installed. Never register a raw C
function again as an alias: that can bypass its wrapper or split shared state. Aliases keep the original closure,
upvalues and caller scope.

A new method does not design its screen: UI authors choose the presentation, and pack inheritance supplies the base
interface's until a derived one overrides it.

## Starting another UI

Create `ui/my-interface/ui.ini` in the Hailstone folder ([layout](layout.md)), or ship the interface as a UI add-on
([add-ons](addons.md#ui-packs)):

```ini
name=My Interface
description=My presentation over the shared Hailstone UI API.
base=modern
input=mouse
```

An optional `base=modern` or `base=gamepad` inherits windows and helper libraries.

- A top-level Lua file of the same relative name replaces that window; a helper such as `lib/theme.lua` can be
  overridden too.
- The derived pack's files win, the rest come from its base chain; removing an override restores the inherited file.
- Settings stay the selected interface's.
- Cycles, invalid parents and chains longer than 32 packs are rejected.
- Base folders must stay within approved roots: the installed `ui/` tree, or an installed UI add-on's
  `ui-addons/<addon>/ui/` tree in the data folder.
- Within a managed bundle, unqualified bases resolve locally before built-ins; `builtin:modern` or
  `addon.<addon>.<pack>` are explicit.
- Descriptors with an unsupported runtime stay visible as unavailable when their base chain is valid. Only Lua is
  supported.

A helper can decorate its inherited version through `API.interfaces.LoadBase(relativeLuaFile)`: `lib/theme.lua` can
import the base theme, wrap its drawing functions and return it, and inherited presenters (later client additions
included) get the decorated helper through their usual `dofile("lib/theme.lua")`.

```lua
local API = hs.UI
local theme = API.interfaces.LoadBase("lib/theme.lua")
-- Decorate the inherited drawing functions here.
return theme
```

The import searches only approved bases, starting after the caller's pack (a selected-pack helper starts at its
nearest base), so chained helpers never import themselves.

- All return values, nil positions included, are kept.
- Files run as text in the current UI VM, with the caller's environment, capabilities and presenter ownership.
- Plugin callers keep their own globals and permissions and cannot select another plugin's folder.
- Kept imports stop working when their owner is removed.
- Absolute paths, traversal, escaped links, selected-root files and recursive imports are refused.
- Errors clear import state before propagating.

Shared presentation modules can live under `<Hailstone data>/ui-data/<stable interface ID>/`.
`API.interfaces.LoadData(relativeLuaFile[, baseOnly])` runs a text Lua module there and returns all its values,
searching the selected interface's data, then its approved bases in order (true skips the selected ID). A sparse
derived interface thus inherits a base's data without copying it.

These roots hold shared, non-secret material such as styles and layouts, apart from credentials, private settings and
plugin folders.

- Modules keep the caller's environment, capability checks, owner lifetime and presenter ownership.
- `LoadBase`'s path, text-only, recursion and protected-error rules apply, including escaped data folders.
- An ID directory cannot redirect to another ID, even inside the data root.
- `LoadData` interprets no schema and writes nothing.
- A cached returned table needs an interface/window reload after an edit.
- Core updates keep `ui-data/`; add-on updates and removal leave it for a future reinstall.

The registry refreshes about once a second and after add-on manager changes.

- A changed `ui.ini` resolves metadata and the base chain again, and queues a reload if the active interface changed.
- Top-level window additions and replacements follow the usual script scan.
- Manual helper-only edits need a window or full UI reload, since scripts keep the helper functions they loaded.
- Package updates compare the content generations of the selected bundle and its bases, so helper-only updates
  reload even with `ui.ini` unchanged.
- A derived window that replaces a whole base file owns later changes to that window.

The example under [sdk/ui/example](../sdk/ui/example/) overrides only the player window and keeps Modern's others. It
is a starting point, not an installed interface choice.

Each pack keeps its own presentation helpers (Modern's HUD/widgets, Gamepad's navigation/window helpers) over the same
`hs.UI` methods. Inheritance reuses them through approved roots; `dofile` cannot escape those roots through `..`,
links or sibling folders.

## Migration and checks

```sh
python tools/migrate_ui_api.py --pack modern --pack gamepad --check
python tools/migrate_ui_api.py --mock-adapter --check
python tools/gen_ui_api_docs.py --check
```

The migration tool reads the same catalog and changes Lua code tokens, leaving comments and strings alone. Unknown
APIs need a deliberate review; it never fakes support for a missing binding.

Run the whole test suite after UI changes. The Lua/ImGui pack tests run Modern and Gamepad against the host API;
contract tests check closure identity, discovery, denied actions and caller/lifetime limits. Fixture adapters belong
in tests only, never as production fallbacks.
