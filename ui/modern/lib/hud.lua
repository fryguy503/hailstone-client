-- The modern UI's windows: borderless HUD frames that draw their own panels, tied to the game window they replace.
--
--   local hud = dofile("lib/hud.lua")
--   ui.window("Player", function()
--     hud.Window("PlayerWindow", "Player", {
--       size = function(s) return 300, 86 end,                         -- design pixels (times s on screen)
--     }, function(x, y, w, h, s) ... end)                              -- draws into that screen rectangle
--   end)
--
-- Until the player moves a frame it sits where the default layout (lib/layout.lua) puts it for this screen, among
-- the frames shown now; a frame that layout does not know gives its own opts.place.
--
-- The game decides whether a window is shown (eq.ReplaceWindow: its keys, /commands and close buttons work as
-- they do for the stock window). Places are the modern UI's own (ui/settings/modern.txt, hud.pos.<window>), not
-- the character's UI ini, so switching back to Classic finds the stock layout untouched.
--
-- Moving and sizing, with no title bar: drag a frame by any spot that is not a button, slot or bar (or anywhere
-- with Alt held); a frame that can change size (the chat) does so from its right and bottom edges and its
-- bottom-right corner. The scale is the right-click menu's.
-- "Lock" (one frame, or all of them) stops both. Layout mode (the right-click menu) also shows the frames that
-- hide while empty (target, group, extended targets, buffs), outlined, so they can be placed too. "Snap to grid"
-- (lib/snap.lua) lines up the frames and dialogs a drag places. A window that can be closed has "Close with Esc" in
-- its right-click menu (lib/escape.lua; some windows have it ticked from the start).

local T = dofile("lib/theme.lua")
local style = dofile("lib/style.lua")
local snap = dofile("lib/snap.lua")
local esc = dofile("lib/escape.lua")
local hud = { T = T }

-- Shared by every window script (each dofile makes its own copy of this file).
modern = modern or { windows = {}, resetAll = false }
local shared = modern

local Setting = T.Setting -- (ui.GetSetting, read once a frame: lib/theme.lua)

local function num(key, default)
  local v = tonumber(Setting(key))
  if v == nil then return default end
  return v
end

-- A window's setting keys, made once (window -> { scale = "hud.scale.<window>", ... }).
shared.keys = shared.keys or {}
local function keysOf(window)
  local k = shared.keys[window]
  if not k then
    k = { scale = "hud.scale." .. window, alpha = "hud.alpha." .. window, pos = "hud.pos." .. window,
      size = "hud.size." .. window, lock = "hud.lock." .. window, fades = "hud.fades." .. window,
      rolled = "hud.rolled." .. window,
      test = "test.pos." .. window, id = "###modern." .. window }
    shared.keys[window] = k
  end
  return k
end

-- A window's ImGui name: its title and its key (made again only when the title changes).
local function windowName(k, title)
  if k.title ~= title then k.title, k.name = title, title .. k.id end
  return k.name
end

-- Layout mode (the setting kept its first name)
function hud.Unlocked() return Setting("hud.unlocked") == "1" end
function hud.SetUnlocked(on) ui.SetSetting("hud.unlocked", on and "1" or "0") end
function hud.Scale() return num("hud.scale", 1.0) end
function hud.Locked(window)
  return Setting("hud.locked") == "1" or Setting(keysOf(window).lock) == "1"
end

-- Whether a frame is rolled up (only its title bar is drawn) and how to roll it (hud.rolled.<window>, kept until it
-- is expanded, closing and opening the frame again included).
function hud.Rolled(window) return Setting(keysOf(window).rolled) == "1" end
function hud.SetRolled(window, on) ui.SetSetting(keysOf(window).rolled, on and "1" or "") end

-- The scale a window draws at: the display's UI scale (the EQ fonts already carry it), the HUD scale and the
-- window's own.
function hud.WindowScale(window)
  return (ui.GetIO().uiScale or 1) * hud.Scale() * num(keysOf(window).scale, 1.0)
end

-- Whether the game shows `screen` this frame: the one place hud.Window and hud.Dialog ask. Asking also takes the
-- game window over (its keys, /commands and close buttons work as the stock window's do, and the stock window is
-- not drawn). It may also return where the stock window sits (hud.Dialog asks eq.ReplaceWindow when it does not).
local function gameShown(screen) return eq.WindowShown(screen) end

-- The default layout (lib/layout.lua) over the frames drawn in the last frames: window -> { w, h, frame, fit, rolled }
-- in shared.frames, the solve kept in shared.solved for as long as nothing it was worked out from changes: a frame
-- shows or changes size (noteFrame), one it placed goes unseen for more than 2 frames (its room is given back), the
-- screen's size or the HUD's scale changes.
local defaults = dofile("lib/layout.lua")
shared.frames = shared.frames or {}

local function noteFrame(key, w, h, fit, rolled)
  local f, frame = shared.frames[key], ui.GetFrameCount()
  if not f then
    f = {}
    shared.frames[key] = f
    shared.solved = nil
  elseif frame - f.frame > 2 or f.w ~= w or f.h ~= h or f.fit ~= fit or f.rolled ~= rolled then
    shared.solved = nil
  end
  f.w, f.h, f.frame, f.fit, f.rolled = w, h, frame, fit, rolled
end

-- The default layout's rectangles on this screen (layout.Solve), and its trayRoom.
function hud.Layout()
  local io, frame = ui.GetIO(), ui.GetFrameCount()
  local s = (io.uiScale or 1) * hud.Scale()
  local c = shared.solved
  if c and c.W == io.width and c.H == io.height and c.s == s then
    if c.checked == frame then return c.rects end
    local live = true
    for _, f in pairs(c.frames) do -- (the frames' own tables, kept up to date by noteFrame)
      if frame - f.frame > 2 then
        live = false
        break
      end
    end
    if live then
      c.checked = frame
      return c.rects
    end
  end
  local frames = {}
  for k, f in pairs(shared.frames) do
    if frame - f.frame <= 2 then frames[k] = f end
  end
  local rects = defaults.Solve(io.width, io.height, s, frames)
  shared.solved = { checked = frame, W = io.width, H = io.height, s = s, frames = frames, rects = rects }
  return rects
end

local pending = {} -- window -> "reset": back to the default place next frame
-- window -> whether the game had it shown last frame, and whether the player opened it (hidden, then shown: a window
-- key or menu entry) since: an opened frame is drawn even when its hideWhen says there is nothing to show
local wasShown, opened = {}, {}

-- window -> the frame as drawn this frame (its place, size, the screen's size, whether it is locked) and whether it
-- moved; kept from frame to frame. current: the frame being drawn (for hud.DragItem); pressMoved: whether this press
-- has moved it.
local frameStates = {}
local current = nil
local pressMoved = false
local dialogResizes = {}

local function dialogExtent(key, id, origin, size, coord, minSize, scale, grid)
  local byId = dialogResizes[key]
  if not byId then byId = {}; dialogResizes[key] = byId end
  local drag = byId[id]
  if not drag then drag = { coord = coord, size = size }; byId[id] = drag end
  return snap.Extent(origin, drag.size, coord - drag.coord, minSize, scale, grid)
end

local function endDialogResize(key, id)
  local byId = dialogResizes[key]
  if byId then byId[id] = nil end
end

-- window -> where the drag has taken it before snapping ({ x, y }), while one goes on with the grid on: each
-- frame's mouse move is too small to reach the next grid line on its own
local drags = {}

-- Moves frame st (a frameStates entry) by dx, dy: its hud.pos setting, on the grid when snapping.
local function moveBy(st, dx, dy)
  st.moving = true
  if dx == 0 and dy == 0 then return end
  local key = st.key
  local g = snap.Grid()
  if not g then
    drags[key] = nil
    ui.SetSetting(st.keys.pos, string.format("%d,%d", math.floor(st.x + dx + 0.5), math.floor(st.y + dy + 0.5)))
    return
  end
  local d = drags[key] or { x = st.x, y = st.y }
  d.x, d.y = d.x + dx, d.y + dy
  drags[key] = d
  ui.SetSetting(st.keys.pos, string.format("%d,%d", snap.Axis(d.x, st.w, st.W, g), snap.Axis(d.y, st.h, st.H, g)))
end

-- After a piece's hit box (T.Hit) on a frame whose pieces cover it (group cards, the menu button): dragging the
-- piece moves the frame. True while this press has moved it (the piece's click is then not a click).
function hud.DragItem()
  if not ui.IsMouseDown(ui.Mouse.Left) and not ui.IsMouseReleased(ui.Mouse.Left) then pressMoved = false end
  if not current or current.locked then return pressMoved end
  if ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left) then
    local io = ui.GetIO()
    moveBy(current, io.mouseDX, io.mouseDY)
    pressMoved = true
  end
  return pressMoved
end

-- The Options window's window sliders (the UI ini's [Main] GlobalAlpha, GlobalFadeToAlpha, GlobalFadeDelay,
-- GlobalFadeDuration: CXWndManager::SetGlobal*), read once a frame for every script: alphas 0..1, times in seconds.
shared.fadeGlobals = shared.fadeGlobals or { frame = -1 }
local globals = shared.fadeGlobals
local function ini(key, default) return tonumber(eq.UiIni("Main", key) or "") or default end
local function fadeSettings()
  local frame = ui.GetFrameCount()
  if globals.frame ~= frame then
    globals.frame = frame
    globals.alpha = math.max(0, math.min(255, ini("GlobalAlpha", 255))) / 255
    globals.to = math.max(0, math.min(255, ini("GlobalFadeToAlpha", 128))) / 255
    globals.delay = math.max(0, ini("GlobalFadeDelay", 2000)) / 1000
    globals.duration = math.max(0, ini("GlobalFadeDuration", 500)) / 1000
  end
  return globals
end

-- window -> its fade: the alpha it fades from and to, since when, the last time the mouse was over it and its rectangle
local fades = {}

-- A window's opacity this frame: its own Opacity setting times the Options window's Window Transparency, or with
-- the window's Fades box ticked (hud.fades.<window>) the Fade-to Transparency once the mouse has been away for the
-- Fade Delay, eased over the Fade Duration either way, as CXWnd::OnProcessFrame fades a window with Fades on.
local function windowAlpha(key, k, layout)
  local g, now = fadeSettings(), ui.GetTime()
  local f = fades[key]
  if not f then
    f = { from = g.alpha, to = g.alpha, since = now, over = now }
    fades[key] = f
  end
  local target = g.alpha
  if Setting(k.fades) == "1" and not layout then
    local r = f.rect
    local mx, my = ui.GetMousePos()
    if not r or (mx >= r[1] and mx < r[3] and my >= r[2] and my < r[4]) then
      f.over = now
    elseif now - f.over > g.delay then
      target = g.to
    end
  else
    f.over = now -- the delay starts afresh when Fades is ticked
  end
  local t = g.duration > 0 and math.min(1, (now - f.since) / g.duration) or 1
  local current = f.from + (f.to - f.from) * t
  if target ~= f.to then f.from, f.to, f.since, current = current, target, now, current end
  return num(k.alpha, 1.0) * current
end

-- Where a window was drawn this frame, for its fade's mouse-over test next frame.
local function noteRect(key, x, y, w, h)
  local f = fades[key]
  if not f then return end
  local r = f.rect
  if not r then
    r = {}
    f.rect = r
  end
  r[1], r[2], r[3], r[4] = x, y, x + w, y + h
end

-- (from its right-click menu, while open: the window stays in full view)
local function fadesMenuItem(key)
  if fades[key] then fades[key].over = ui.GetTime() end
  local on = ui.GetSetting("hud.fades." .. key) == "1"
  if ui.MenuItem("Fades", on) then ui.SetSetting("hud.fades." .. key, on and "0" or "1") end
  if ui.IsItemHovered() then
    ui.SetTooltip("Fade to the Options window's Fade-to Transparency when the mouse leaves (Options > Display > Windows)")
  end
end

local function menu(window, title, opts)
  ui.TextDisabled(title)
  ui.Separator()
  local own = ui.GetSetting("hud.lock." .. window) == "1"
  if ui.MenuItem("Lock this frame", own) then ui.SetSetting("hud.lock." .. window, own and "0" or "1") end
  local all = ui.GetSetting("hud.locked") == "1"
  if ui.MenuItem("Lock all frames", all) then ui.SetSetting("hud.locked", all and "0" or "1") end
  local layout = hud.Unlocked()
  if ui.MenuItem("Layout mode (show every frame)", layout) then hud.SetUnlocked(not layout) end
  snap.MenuItems()
  ui.Separator()
  ui.SetNextItemWidth(180)
  local v, changed = ui.SliderFloat("Scale##win", num("hud.scale." .. window, 1.0), 0.5, 2.5)
  if changed then ui.SetSetting("hud.scale." .. window, string.format("%.2f", v)) end
  ui.SetNextItemWidth(180)
  v, changed = ui.SliderFloat("Opacity##win", num("hud.alpha." .. window, 1.0), 0.15, 1.0)
  if changed then ui.SetSetting("hud.alpha." .. window, string.format("%.2f", v)) end
  fadesMenuItem(window)
  if opts.menu then
    ui.Separator()
    opts.menu()
  end
  ui.Separator()
  if ui.MenuItem("Reset position") then pending[window] = "reset" end
  if ui.MenuItem("Reset size") then
    ui.SetSetting("hud.size." .. window, "")
    ui.SetSetting("hud.scale." .. window, "")
  end
  if opts.rollup then
    if hud.Rolled(window) then
      if ui.MenuItem("Expand") then hud.SetRolled(window, false) end
    elseif ui.MenuItem("Roll up") then hud.SetRolled(window, true) end
  end
  if opts.onClose then
    if ui.MenuItem("Close") then opts.onClose() end
  elseif opts.closable ~= false and opts.screen and ui.MenuItem("Close") then
    eq.ShowWindow(opts.screen, false)
  end
  if opts.onClose or (opts.closable ~= false and opts.screen) then esc.MenuItem(window) end
  if ui.BeginMenu("All frames") then
    ui.SetNextItemWidth(180)
    local hs, hc = ui.SliderFloat("HUD scale", hud.Scale(), 0.5, 2.5)
    if hc then ui.SetSetting("hud.scale", string.format("%.2f", hs)) end
    local mm = ui.GetSetting("hud.minimap") ~= "0"
    if ui.MenuItem("Minimap", mm) then ui.SetSetting("hud.minimap", mm and "0" or "1") end
    if ui.MenuItem("Reset every position") then shared.resetAll = ui.GetTime() end
    ui.EndMenu()
  end
  ui.Separator()
  ui.TextDisabled("Drag an empty spot, or Alt+drag, to move")
  if opts.resizable then ui.TextDisabled("Drag an edge or the bottom-right corner to resize") end
  ui.TextDisabled("Alt+right-click always opens this menu")
end

-- Auto Z's switch and its two ranges (eq.MapAutoZ), for a menu or popup: how far above (+) and below (-) the
-- player's feet the map's lines and labels stay whole before they fade. Shared by the map and the minimap.
local AUTOZ_MAX = 300
function hud.AutoZControls(s)
  local on, above, below = eq.MapAutoZ()
  if ui.MenuItem("Auto Z", on) then eq.MapAutoZ(not on) end
  if ui.IsItemHovered() then ui.SetTooltip("Fade what is well above or below you") end
  if not on then ui.BeginDisabled() end
  ui.SetNextItemWidth(180 * (s or 1))
  local a, ca = ui.SliderInt("##autozabove", above, 1, AUTOZ_MAX, string.format("+%d above you", above))
  ui.SetNextItemWidth(180 * (s or 1))
  local b, cb = ui.SliderInt("##autozbelow", below, 1, AUTOZ_MAX, string.format("-%d below you", below))
  if not on then ui.EndDisabled() end
  if ca or cb then eq.MapAutoZ(on, a, b) end
end

-- An X button of this UI's own (a frame's close box): size x size at x, y; onClose() when it is clicked.
function hud.CloseX(id, x, y, size, onClose)
  local clicked = T.Hit(id, x, y, x + size, y + size)
  local hovered, active = ui.IsItemHovered(), ui.IsItemActive()
  local c = (hovered or active) and T.color.text or T.color.faint
  if hovered then ui.DrawRectFilled(x, y, x + size, y + size, 0.85, 0.30, 0.30, active and 0.9 or 0.6, size / 4) end
  local m = size * 0.3
  ui.DrawLine(x + m, y + m, x + size - m, y + size - m, c[1], c[2], c[3], 1, 1.5)
  ui.DrawLine(x + m, y + size - m, x + size - m, y + m, c[1], c[2], c[3], 1, 1.5)
  if clicked then onClose() end
end

-- A resizable frame's right (right = true) or bottom edge: a drag changes its size (design pixels); `setSize`
-- applies the new size (hud.size.<window>, or the frame's own reflow).
local function frameEdge(st, id, x0, y0, x1, y1, right, s, minW, minH, setSize)
  T.Hit(id, x0, y0, x1, y1)
  local active, over = ui.IsItemActive(), ui.IsItemHovered()
  local wx, wy, w, h = st.wx, st.wy, st.w, st.h
  if active or over then
    local c = T.color.accent
    local a = active and 1 or 0.6
    if right then ui.DrawLine(wx + w - 2 * s, wy + 6 * s, wx + w - 2 * s, y1, c[1], c[2], c[3], a, 2 * s)
    else ui.DrawLine(wx + 6 * s, wy + h - 2 * s, x1, wy + h - 2 * s, c[1], c[2], c[3], a, 2 * s) end
  end
  if active and not st.resize then
    local mx, my = ui.GetMousePos()
    st.resize = { x = mx, y = my, w = st.w, h = st.h }
  end
  if active and ui.IsMouseDragging(ui.Mouse.Left) then
    local mx, my = ui.GetMousePos()
    local grid = snap.Grid()
    if grid then snap.DrawGrid(grid, T.color.accent) end
    local nw = right and snap.Extent(wx, st.resize.w / s, mx - st.resize.x, minW, s, grid)
      or math.max(minW, st.w / s)
    local nh = right and math.max(minH, st.h / s)
      or snap.Extent(wy, st.resize.h / s, my - st.resize.y, minH, s, grid)
    setSize(nw, nh, grid)
  end
end

-- hud.Window(screen, title, opts, body)
--   screen: the game window this replaces (eq.ReplaceWindow), or nil for a frame of this UI's own (opts.shown()).
--   opts.size(s) -> w, h        the frame's size in design pixels (may change with its contents); called only while
--                                the frame is drawn, so work for the frame alone can go there
--   opts.place(W, H, w, h, s) -> x, y   the default place on a W x H screen (w, h on screen), for a frame the
--                                default layout (lib/layout.lua) does not place; neither: the screen's middle
--   opts.fit = true              (with resizable) the layout sizes the frame until the player does (the chat)
--   opts.panel = false           no panel behind the body (the body draws its own pieces)
--   opts.menu()                  extra rows for the right-click menu
--   opts.inputs = false          the frame never takes the mouse (moved in layout mode only)
--   opts.hideWhen() -> true      not drawn this frame unless in layout mode (an empty target frame)
--   opts.resizable = { minW, minH }   its right and bottom edges and its bottom-right corner change its size
--                                (hud.size.<window>); a frame without it has no grip (its scale is the menu's)
--   opts.onResize(nw, nh)        with resizable: the drag changes the frame's own layout instead of hud.size (the hot
--                                bars' buttons per row); opts.size must then give the frame its size from that layout
--   opts.onClose()               a frame that can be put away (not a HUD piece): an X in its top right corner calls it
--   opts.rollup = true           the frame rolls up into its title bar (hud.rolled.<window>): rolled up, only the
--                                title bar is drawn and the frame is as tall as it (opts.titleHeight, design pixels);
--                                the body draws the title bar itself (the frame does not draw one)
--   opts.titleHeight = 26        the title bar's height in design pixels (a rolled up frame's height)
--   body(x, y, w, h, s)          draws into the frame's screen rectangle
function hud.Window(screen, title, opts, body)
  opts.screen = screen
  local key = screen or title
  local shown
  if screen then shown = gameShown(screen) else shown = (opts.shown == nil) or opts.shown() end
  if shown and wasShown[key] == false then opened[key] = true end
  wasShown[key] = shown and true or false
  if not shown then
    opened[key] = nil
    return
  end
  local layout = hud.Unlocked()
  if opts.hideWhen and not layout and not opened[key] and opts.hideWhen() then return end
  local k = keysOf(key)
  local locked = hud.Locked(key)
  local rolled = opts.rollup and hud.Rolled(key) or false
  opts.rolled = rolled -- (the body's own: this frame's, as the frame is drawn)

  local io = ui.GetIO()
  local hudScale, ownScale = hud.Scale(), num(k.scale, 1.0)
  local s = (io.uiScale or 1) * hudScale * ownScale -- (hud.WindowScale)
  local dw, dh = opts.size(s)
  local fit = false
  if opts.resizable and not opts.onResize then
    local sz = Setting(k.size)
    local sw, sh = (sz or ""):match("([%d.]+),([%d.]+)")
    if sw then
      dw, dh = math.max(tonumber(sw), opts.resizable[1]), math.max(tonumber(sh), opts.resizable[2])
    elseif opts.fit then
      -- no size of its own yet: the layout's, from the room this screen has
      fit = true
      local r = hud.Layout()[key]
      if r then dw, dh = r.w / s, r.h / s end
    end
  end
  if rolled then dh = opts.titleHeight or 26 end -- (the title bar only)
  local w, h = math.floor(dw * s + 0.5), math.floor(dh * s + 0.5)
  local W, H = io.width, io.height
  shared.windows[key] = true
  noteFrame(key, w, h, fit, rolled)

  -- the place: saved, else the default; kept on screen when the resolution shrinks
  local saved = Setting(k.pos)
  if pending[key] == "reset" or (shared.resetAll and (shared.resetDone or {})[key] ~= shared.resetAll) then
    ui.SetSetting(k.pos, "")
    saved = nil
    shared.resetDone = shared.resetDone or {}
    shared.resetDone[key] = shared.resetAll
  end
  pending[key] = nil
  local x, y
  if saved and saved ~= "" then
    local sx, sy = saved:match("(-?[%d.]+),(-?[%d.]+)")
    x, y = tonumber(sx), tonumber(sy)
  end
  if not x then x, y = defaults.Place(hud.Layout(), key, w, h, (io.uiScale or 1) * hudScale) end
  if not x and opts.place then x, y = opts.place(W, H, w, h, s) end
  if not x then x, y = W / 2 - w / 2, H / 2 - h / 2 end
  x = math.max(0, math.min(x, W - math.min(w, W)))
  y = math.max(0, math.min(y, H - math.min(h, H)))
  local testPos = Setting(k.test)
  if testPos then
    local tx, ty = testPos:match("(-?%d+),(-?%d+)")
    if tx then x, y = tonumber(tx), tonumber(ty) end
  end

  -- The frame sits where its setting says; moving it (below) changes the setting.
  ui.SetNextWindowPos(math.floor(x), math.floor(y), ui.Cond.Always)
  ui.SetNextWindowSize(w, h, ui.Cond.Always)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoBackground | ui.WindowFlags.NoSavedSettings
      | ui.WindowFlags.NoScrollWithMouse | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoMove
  if opts.inputs == false and not layout then flags = flags | ui.WindowFlags.NoInputs end
  -- a rolled up frame is a title bar, and shorter than ImGui's smallest window (its WindowMinSize): it may be one
  -- (its size is set here every frame, so the auto-resize has nothing to do)
  if rolled then flags = flags | ui.WindowFlags.AlwaysAutoResize end

  local alpha = windowAlpha(key, k, layout)
  noteRect(key, x, y, w, h)
  style.Push() -- menus and tooltips
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
  ui.PushStyleVar(ui.StyleVar.Alpha, alpha)
  local failure = nil -- an error in the body: raised once the window is closed and the style popped
  local visible = ui.Begin(windowName(k, title), nil, flags)
  ui.PopStyleVar(3)
  local closeNow = false -- Esc closes it (lib/escape.lua): once its Begin and End are done
  if opts.onClose or (opts.closable ~= false and screen) then closeNow = esc.Frame(key) end
  if visible then
    local wx, wy = ui.GetWindowPos()
    ui.SetWindowFontScale(hudScale * ownScale)
    T.ResetRightClick()
    ui.PushStyleVar(ui.StyleVar.Alpha, alpha)
    if not ui.IsMouseDown(ui.Mouse.Left) then drags[key] = nil end
    -- this frame, for moveBy and hud.DragItem
    local st = frameStates[key]
    if not st then
      st = { key = key, keys = k }
      frameStates[key] = st
    end
    st.x, st.y, st.w, st.h, st.W, st.H, st.wx, st.wy = x, y, w, h, W, H, wx, wy
    st.moving, st.locked = false, locked and not layout
    if not ui.IsMouseDown(ui.Mouse.Left) then st.resize = nil end
    -- the frame's own drag handle, under its pieces: the empty spots move it
    if not locked or layout then
      T.Hit("##move", wx, wy, wx + w, wy + h)
      if ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left) then moveBy(st, io.mouseDX, io.mouseDY) end
    end
    current = st
    if layout then ui.BeginDisabled(true) end -- in layout mode the pieces let go of the mouse: a drag moves the frame
    if opts.panel ~= false then T.Panel(wx, wy, wx + w, wy + h, s, opts.panelOpts) end
    local ok, err = pcall(body, wx, wy, w, h, s)
    if not ok then failure = err end
    if opts.onClose and not layout then hud.CloseX("##hudclose", wx + w - 20 * s, wy + 4 * s, 16 * s, opts.onClose) end
    if layout then
      ui.EndDisabled()
      local c = T.color.accent
      ui.DrawRectFilled(wx, wy, wx + w, wy + h, c[1], c[2], c[3], 0.10, 6 * s)
      ui.DrawRect(wx, wy, wx + w, wy + h, c[1], c[2], c[3], 0.95, 6 * s, 1.5)
      T.Text(wx + 6 * s, wy + 4 * s, title, c, T.font.small)
    end
    if not locked or layout then
      -- Alt held: one handle over the whole frame, on top of its pieces
      if io.alt and ui.IsMouseHoveringRect(wx, wy, wx + w, wy + h) then
        T.Hit("##altmove", wx, wy, wx + w, wy + h)
        if ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left) then moveBy(st, io.mouseDX, io.mouseDY) end
      end
      if st.moving then
        local c = T.color.accent
        ui.DrawRect(wx, wy, wx + w, wy + h, c[1], c[2], c[3], 0.9, 6 * s, 1.5)
        local g = snap.Grid()
        if g then snap.DrawGrid(g, c) end
      end
      if opts.resizable and not rolled then
        -- the right and bottom edges and the corner grip (shown while the frame is hovered) change its size in design
        -- pixels; the scale is the right-click menu's, or opts.onResize changes the frame's own layout
        local g = math.max(12, 14 * s)
        local hovered = ui.IsMouseHoveringRect(wx, wy, wx + w, wy + h)
        local e = math.max(5, 6 * s)
        local minW, minH = opts.resizable[1], opts.resizable[2]
        local setSize = function(nw, nh, grid)
          if opts.onResize then opts.onResize(nw, nh)
          else ui.SetSetting(k.size, string.format(grid and "%.3f,%.3f" or "%.0f,%.0f", nw, nh)) end
        end
        frameEdge(st, "##resizeright", wx + w - e, wy, wx + w, wy + h - g, true, s, minW, minH, setSize)
        frameEdge(st, "##resizebottom", wx, wy + h - e, wx + w - g, wy + h, false, s, minW, minH, setSize)
        T.Hit("##resize", wx + w - g, wy + h - g, wx + w, wy + h)
        local active, over = ui.IsItemActive(), ui.IsItemHovered()
        if hovered or active then
          local c = T.color.accent
          local a = (active or over) and 1 or 0.45
          for n = 1, 3 do
            local d = n * 3.5 * s
            ui.DrawLine(wx + w - d - 3 * s, wy + h - 3 * s, wx + w - 3 * s, wy + h - d - 3 * s, c[1], c[2], c[3], a, 1.5)
          end
        end
        if over and not active then ui.SetTooltip("Drag to resize") end
        if active and not st.resize then
          local mx, my = ui.GetMousePos()
          st.resize = { x = mx, y = my, w = st.w, h = st.h }
        end
        if active and ui.IsMouseDragging(ui.Mouse.Left) then
          local mx, my = ui.GetMousePos()
          local grid = snap.Grid()
          if grid then snap.DrawGrid(grid, T.color.accent) end
          local nw = snap.Extent(wx, st.resize.w / s, mx - st.resize.x, minW, s, grid)
          local nh = snap.Extent(wy, st.resize.h / s, my - st.resize.y, minH, s, grid)
          setSize(nw, nh, grid)
        end
      end
    end
    ui.PopStyleVar()
    ui.SetWindowFontScale(1)

    -- right click: the game window's own menu (eq.WindowContextMenu) unless a piece took the click or Alt is held;
    -- else this UI's frame menu
    if ((io.alt and not T.AltRightClickTaken()) or not T.RightClickTaken()) and ui.IsMouseClicked(ui.Mouse.Right)
        and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows) then
      if io.alt or layout or not (screen and eq.WindowContextMenu(screen)) then ui.OpenPopup("##hudmenu") end
    end
    if screen and eq.TakeWindowMenu(screen) then ui.OpenPopup("##hudmenu") end
    if ui.BeginPopup("##hudmenu") then
      menu(key, title, opts)
      ui.EndPopup()
    end
  end
  ui.End()
  style.Pop()
  if closeNow then
    if opts.onClose then opts.onClose() else eq.ShowWindow(screen, false) end
  end
  if failure then error(failure, 0) end
end

-- ---------------------------------------------------------------- dialogs
-- A resizable dialog's right (right = true) or bottom edge, with the window's corner at gx, gy: lit while hovered or
-- held; true while dragged.
local function dialogEdge(id, x0, y0, x1, y1, right, gx, gy, s, c)
  ui.EdgeButton(id, x0, y0, x1, y1)
  local active, over = ui.IsItemActive(), ui.IsItemHovered()
  if active or over then
    local lx0, ly0 = right and gx - 2 * s or x0 + 8 * s, right and y0 or gy - 2 * s
    local lx1, ly1 = right and gx - 2 * s or x1, right and y1 or gy - 2 * s
    ui.DrawLine(lx0, ly0, lx1, ly1, c[1], c[2], c[3], active and 1 or 0.6, 2 * s)
  end
  return active, active and ui.IsMouseDragging(ui.Mouse.Left)
end

-- hud.Dialog(screen, title, opts, body): a window with a title bar and a close box (inventory, bags, ...), always
-- movable (its place is kept as the frames' are), sized to its contents.
--   opts.size(s) -> w, h         the contents' size in design pixels
--   opts.place(W, H, w, h, s, x, y) -> x, y   the first place (x, y: where the stock window sits)
--   opts.onClose()               instead of hiding the game window
--   opts.closable = false        no close box (a window only the server closes: the respawn choices), and no roll-up
--   opts.rollup = false          no roll-up button (a question to answer: a confirmation, a quantity)
--   opts.key                     the place's key when one script draws several windows of one screen name
--   opts.resizable = { minW, minH }   resized from its right and bottom edges (hud.size.<window>, design pixels);
--                                body gets the current contents' size
-- A resizable dialog's bottom-right corner resizes it as its edges do; its scale (hud.scale.<window>, text and all)
-- is the right-click menu's slider, or Ctrl+mouse wheel over it. A dialog with a close box also
-- has a roll-up button beside it (and a double click on its title bar): rolled up, only the title bar is drawn
-- (hud.rolled.<window>, kept until it is unrolled, closing and opening it again included).
--   body(x, y, w, h, s)          draws the contents at that screen rectangle
function hud.Dialog(screen, title, opts, body)
  local key = opts.key or screen
  local shown, sx, sy = gameShown(screen)
  if opts.shown ~= nil then shown = opts.shown end
  if not shown then return end
  if sx == nil then _, sx, sy = eq.ReplaceWindow(screen) end -- (where the stock window sits)
  local k = keysOf(key)
  local io = ui.GetIO()
  local ownScale = num(k.scale, 1.0)
  local s = (io.uiScale or 1) * ownScale
  local dw, dh = opts.size(s)
  local w, h = math.floor(dw * s + 0.5), math.floor(dh * s + 0.5)
  local saved = Setting(k.pos)
  if pending[key] == "reset" then
    ui.SetSetting(k.pos, "")
    saved = nil
  end
  local reset = pending[key] == "reset"
  pending[key] = nil
  local x, y
  if saved and saved ~= "" then
    local px, py = saved:match("(-?[%d.]+),(-?[%d.]+)")
    x, y = tonumber(px), tonumber(py)
  end
  if not x then
    if opts.place then x, y = opts.place(io.width, io.height, w, h, s, sx or 100, sy or 100)
    else x, y = sx or 100, sy or 100 end
  end
  -- on screen as a whole where it fits (its title bar's close box too), else its top left corner
  local outerW, outerH = w + 20 * s, h + 40 * s -- (the padding and title bar)
  if opts.resizable then
    local a, b = (Setting(k.size) or ""):match("([%d.]+),([%d.]+)")
    outerW, outerH = (tonumber(a) or dw) * s, (tonumber(b) or dh) * s
  end
  x = math.max(0, math.min(x, io.width - math.min(outerW, io.width)))
  y = math.max(0, math.min(y, io.height - math.min(outerH, io.height)))
  local testPos = Setting(k.test)
  if testPos then
    local tx, ty = testPos:match("(-?%d+),(-?%d+)")
    if tx then x, y = tonumber(tx), tonumber(ty) end
  end
  -- With the grid on, the dialog sits where its setting says, as a frame does: where ImGui's title bar drag put it
  -- this frame (before Begin) is snapped and kept. ImGui works the drag out from the mouse each frame, not from the
  -- last place, so the snapping does not hold it back.
  local snapped = false
  local g = snap.Grid()
  if g and not reset and not testPos then
    snapped = true
    local rx, ry, rw, rh = ui.FindWindowRect(k.id)
    if rx and (math.abs(rx - x) >= 1 or math.abs(ry - y) >= 1) then
      x, y = snap.Axis(rx, rw, io.width, g), snap.Axis(ry, rh, io.height, g)
      x = math.max(0, math.min(x, io.width - math.min(rw, io.width)))
      y = math.max(0, math.min(y, io.height - math.min(rh, io.height)))
      ui.SetSetting(k.pos, string.format("%d,%d", math.floor(x), math.floor(y)))
      snap.DrawGrid(g, T.color.accent)
    end
  end
  ui.SetNextWindowPos(math.floor(x), math.floor(y),
    (reset or testPos or snapped) and ui.Cond.Always or ui.Cond.Appearing)
  local savedW, savedH
  if opts.resizable then
    local sz = Setting(k.size) or ""
    local a, b = sz:match("([%d.]+),([%d.]+)")
    savedW, savedH = tonumber(a), tonumber(b)
    savedW = savedW and math.max(savedW, opts.resizable[1])
    savedH = savedH and math.max(savedH, opts.resizable[2])
    -- its size is kept in design pixels and set every frame: a new scale (the menu's Scale, Ctrl+wheel, the UI
    -- scale) zooms the whole window, not only what is in it; its edges and corner (below) change the size. It is
    -- drawn no bigger than the screen (the saved size is kept): a large scale left its foot below the screen's edge.
    ui.SetNextWindowSize(math.min((savedW or dw) * s, io.width), math.min((savedH or dh) * s, io.height),
      ui.Cond.Always)
  end
  local alpha = windowAlpha(key, k, false)
  style.Push()
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 10 * s, 8 * s)
  ui.PushStyleVar(ui.StyleVar.Alpha, alpha)
  local flags = ui.WindowFlags.NoSavedSettings | ui.WindowFlags.NoResize
      | ui.WindowFlags.NoScrollbar | ui.WindowFlags.NoScrollWithMouse
  if not opts.resizable then flags = flags | ui.WindowFlags.AlwaysAutoResize end
  -- the roll-up: ImGui's collapse button beside the close box, the state kept in the setting (applied every frame,
  -- so a click or a double click on the title bar, which ImGui applies after it, still toggles it)
  local rollable = opts.closable ~= false and opts.rollup ~= false
  local rolled = false
  if rollable then
    rolled = Setting(k.rolled) == "1"
    ui.SetNextWindowCollapsed(rolled, ui.Cond.Always)
  else
    flags = flags | ui.WindowFlags.NoCollapse
  end
  local gameTitle = eq.WindowTitle(screen)
  if gameTitle and gameTitle ~= "" and not opts.keepTitle then title = gameTitle end
  local failure = nil
  local visible, open = ui.Begin(windowName(k, title), opts.closable ~= false or nil, flags, rollable)
  if opts.closable == false then open = true
  elseif esc.Frame(key) then open = false end
  ui.PopStyleVar(2)
  if rollable and ui.IsWindowCollapsed() ~= rolled then ui.SetSetting(k.rolled, rolled and "" or "1") end
  -- its place and rectangle (the fade's mouse-over test), rolled up too: only the title bar is there then
  local wx, wy = ui.GetWindowPos()
  do
    local ww, wh = ui.GetWindowSize()
    noteRect(key, wx, wy, ww, wh)
  end
  if math.abs(wx - x) >= 1 or math.abs(wy - y) >= 1 then
    ui.SetSetting(k.pos, string.format("%d,%d", math.floor(wx), math.floor(wy)))
  end
  if visible then
    do -- an opaque layer of the window colour: the scene, and the text of windows behind, showed through the window
      -- colour alone; the Opacity setting fades it with the rest
      local ww, wh = ui.GetWindowSize()
      local wc = T.color.window
      ui.DrawRectFilled(wx, wy, wx + ww, wy + wh, wc[1], wc[2], wc[3], alpha, 8)
    end
    -- fields, check boxes, combo boxes and buttons outlined in the palette's frameBorder (3:1 against the window), the
    -- window's own edge keeping its hairline
    local fb = T.color.frameBorder
    ui.PushStyleColor(ui.Col.Border, fb[1], fb[2], fb[3], fb[4])
    ui.PushStyleVar(ui.StyleVar.FrameBorderSize, 1)
    ui.PushStyleVar(ui.StyleVar.Alpha, alpha) -- the contents fade with the window (not its menu, below)
    ui.SetWindowFontScale(ownScale)
    T.ResetRightClick()
    local cx, cy = ui.GetCursorScreenPos()
    local cpx, cpy = ui.GetCursorPos()
    if opts.resizable then
      -- the contents' size from the window's
      w, h = ui.GetContentRegionAvail()
      w = math.max(w, opts.resizable[1] * s)
      h = math.max(h, opts.resizable[2] * s)
    end
    local ok, err = pcall(body, cx, cy, w, h, s)
    if not ok then failure = err end
    ui.SetCursorPos(cpx, cpy)
    local c = T.color.accent
    local windowHovered = ui.IsWindowHovered(ui.HoveredFlags.ChildWindows)
    if opts.resizable then
      -- the right and bottom edges, below the title bar and short of the corner, and the corner grip change its size
      -- (design pixels); a dialog sized to its contents has no grip
      local g = math.max(12, 14 * s)
      local ww, wh = ui.GetWindowSize()
      local gx, gy = wx + ww, wy + wh
      local e = math.max(5, 6 * s)
      local curW, curH = savedW or dw, savedH or dh
      local minW, minH = opts.resizable[1], opts.resizable[2]
      local rightActive, rightDragging = dialogEdge("##dlgright", gx - e, cy, gx, gy - g, true, gx, gy, s, c)
      if rightActive then
        local mx = ui.GetMousePos()
        local grid = snap.Grid()
        local nw = dialogExtent(key, "right", wx, ww / s, mx, minW, s, grid)
        if rightDragging then
          if grid then snap.DrawGrid(grid, c) end
          local nh = math.max(minH, curH)
          ui.SetSetting(k.size, string.format(grid and "%.3f,%.3f" or "%.0f,%.0f", nw, nh))
        end
      else
        endDialogResize(key, "right")
      end
      local bottomActive, bottomDragging = dialogEdge("##dlgbottom", wx, gy - e, gx - g, gy, false, gx, gy, s, c)
      if bottomActive then
        local _, my = ui.GetMousePos()
        local grid = snap.Grid()
        local nh = dialogExtent(key, "bottom", wy, wh / s, my, minH, s, grid)
        if bottomDragging then
          if grid then snap.DrawGrid(grid, c) end
          local nw = math.max(minW, curW)
          ui.SetSetting(k.size, string.format(grid and "%.3f,%.3f" or "%.0f,%.0f", nw, nh))
        end
      else
        endDialogResize(key, "bottom")
      end
      ui.EdgeButton("##dlgresize", gx - g, gy - g, gx, gy)
      local active, over = ui.IsItemActive(), ui.IsItemHovered()
      gx, gy = gx - 3 * s, gy - 3 * s -- (the lines clear of the rounded corner)
      if windowHovered or active then
        local a = (active or over) and 1 or 0.35
        for n = 1, 3 do
          local d = n * 3.5 * s
          ui.DrawLine(gx - d, gy, gx, gy - d, c[1], c[2], c[3], a, 1.5)
        end
      end
      if over and not active then ui.SetTooltip("Drag to resize") end
      if active then
        local mx, my = ui.GetMousePos()
        local grid = snap.Grid()
        local nw = dialogExtent(key, "corner-x", wx, ww / s, mx, minW, s, grid)
        local nh = dialogExtent(key, "corner-y", wy, wh / s, my, minH, s, grid)
        if ui.IsMouseDragging(ui.Mouse.Left) then
          if grid then snap.DrawGrid(grid, c) end
          ui.SetSetting(k.size, string.format(grid and "%.3f,%.3f" or "%.0f,%.0f", nw, nh))
        end
      else
        endDialogResize(key, "corner-x")
        endDialogResize(key, "corner-y")
      end
    end
    ui.SetCursorPos(cpx, cpy)
    if not opts.resizable then ui.Dummy(w, h) end -- the contents' size, for the auto-resize
    ui.SetWindowFontScale(1)
    ui.PopStyleVar() -- the contents' Alpha
    -- Ctrl+wheel over the window zooms it, as the menu's Scale does
    if io.ctrl and (io.wheel or 0) ~= 0 and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows) then
      local scale = math.max(0.6, math.min(2.0, num(k.scale, 1.0) * 1.1 ^ io.wheel))
      ui.SetSetting(k.scale, string.format("%.3f", scale))
    end
    if ((io.alt and not T.AltRightClickTaken()) or not T.RightClickTaken()) and ui.IsMouseClicked(ui.Mouse.Right)
        and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows) then
      if io.alt or not eq.WindowContextMenu(screen) then ui.OpenPopup("##dialogmenu") end
    end
    if eq.TakeWindowMenu(screen) then ui.OpenPopup("##dialogmenu") end
    if ui.BeginPopup("##dialogmenu") then
      ui.TextDisabled(title)
      ui.Separator()
      ui.SetNextItemWidth(180)
      local v, changed = ui.SliderFloat("Scale##dlg", num("hud.scale." .. key, 1.0), 0.6, 2.0)
      if changed then ui.SetSetting("hud.scale." .. key, string.format("%.2f", v)) end
      ui.SetNextItemWidth(180)
      v, changed = ui.SliderFloat("Opacity##dlg", num("hud.alpha." .. key, 1.0), 0.3, 1.0)
      if changed then ui.SetSetting("hud.alpha." .. key, string.format("%.2f", v)) end
      fadesMenuItem(key)
      if opts.menu then
        ui.Separator()
        opts.menu()
      end
      ui.Separator()
      snap.MenuItems()
      if ui.MenuItem("Reset position") then pending[key] = "reset" end
      if ui.MenuItem("Reset size") then
        ui.SetSetting("hud.scale." .. key, "")
        if opts.resizable then ui.SetSetting("hud.size." .. key, "") end
      end
      if opts.closable ~= false then
        ui.Separator()
        if rollable and ui.MenuItem("Roll up") then ui.SetSetting(k.rolled, "1") end
        if ui.MenuItem("Close") then open = false end
        esc.MenuItem(key)
      end
      ui.Separator()
      if opts.resizable then ui.TextDisabled("Drag an edge or the bottom-right corner to resize") end
      ui.TextDisabled("Ctrl+mouse wheel to zoom")
      ui.EndPopup()
    end
    ui.PopStyleVar()
    ui.PopStyleColor()
  end
  ui.End()
  style.Pop()
  if failure then error(failure, 0) end
  if not open then
    if opts.onClose then opts.onClose() else eq.ShowWindow(screen, false) end
  end
end

return hud
