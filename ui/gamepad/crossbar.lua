-- The cross hotbar (gamepad pack), bottom centre: stands in for hot bar 1 (HotButtonWnd), whose key shows and hides
-- it. Three sets of eight slots (docs/gamepad.md section 3): hold LT, RT or both and the D-pad (left, up, right,
-- down) and the face buttons (X, Y, B, A) fire that set's slots. Each set is drawn as two diamonds in the pad's own
-- glyphs; the set being held (hs.gamepad.cross_set) is enlarged and the others dimmed.
--
-- A slot is a hot button the player already has (hs.gamepad.cross_slot: bar and button; by default bars 1, 2 and 3,
-- buttons 1..8), shown and used through the stock hot bar windows (hs.HotButtonInfo / hs.ClickHotButton), so the slots
-- are the same buttons the classic hot bars show and edit. A spell's slot shows its gem's cool-down and fades as the
-- gem does (hs.GemOverlay / hs.GemTint of the gem holding that spell). The mouse works too: a click uses the slot
-- (an empty one opens its menu), a quick right click is the game's (a social's editor), the right button held opens
-- the slot's menu (assign a social, a label, clear), a hot button on the cursor is dropped onto it. With the pad the
-- menu opens from the main menu's Hot Bars (slotpicker.lua).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local C = T.C

local SLOT_BUTTONS = { "left", "up", "right", "down", "x", "y", "b", "a" }
local DIAMOND = { { -1, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 } } -- left, up, right, down / west, north, east, south

local SCREENS, CHILDREN = {}, {}
for b = 1, 10 do SCREENS[b] = b == 1 and "HotButtonWnd" or ("HotButtonWnd" .. b) end
for b = 1, 12 do CHILDREN[b] = "HB_Button" .. b end
local function screenOf(bar) return SCREENS[bar] or ("HotButtonWnd" .. bar) end
local function childOf(button) return CHILDREN[button] or ("HB_Button" .. button) end

local rightPress = nil -- the slot the right button went down on: { key, at, held }

-- Each slot's (set * 10 + n): the record hs.HotButtonInfo fills, its label's text colour and its fitted label.
local slots = {}

-- T.fit(label, w) in the current font, kept in `c` while the label, the width and the font's measure of the label
-- stay the same.
local function fitted(c, label, w)
  local full = ui.CalcTextSize(label)
  if c.label ~= label or c.w ~= w or c.full ~= full then
    c.label, c.w, c.full, c.text = label, w, full, T.fit(label, w)
  end
  return c.text
end

local function slot(set, n, cx, cy, size, alpha, held)
  local bar, button = pad.cross_slot(set, n)
  if not bar then return end
  local key = set * 10 + n
  local rec = slots[key]
  if not rec then
    rec = { info = {}, tc = { 0, 0, 0, 1 }, fit = {} }
    slots[key] = rec
  end
  local screen, child = screenOf(bar), childOf(button)
  -- what it holds; a social's colour and a Lua social's face (social); its check (on: auto attack / fire, a skill not
  -- ready) and cool-down, a gem's tint stage (state, iconAlpha)
  local info = API.hotbars.Info(screen, child, rec.info)
  local kind, label, tex = info.kind, info.label, info.tex
  local face = info.social
  local x0, y0 = cx - size / 2, cy - size / 2
  local x1, y1 = x0 + size, y0 + size
  local r = T.px(10)
  -- the slot is an item of the bar's window (mouse clicks)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y0 - wy)
  ui.PushID(key)
  local clicked = ui.InvisibleButton("##slot", size, size)
  local hovered = ui.IsItemHovered()
  ui.PopID()
  local pressed = held and pad.down(SLOT_BUTTONS[n])
  T.rect(x0, y0, x1, y1, kind == "empty" and C.sunken or C.raised, alpha, r)
  local iconA = alpha
  if info.iconAlpha then iconA = alpha * info.iconAlpha end
  if tex then
    local m = math.floor(size * 0.08)
    T.icon(tex, info.u0, info.v0, info.u1, info.v1, x0 + m, y0 + m, x1 - m, y1 - m, iconA)
  elseif label ~= "" then
    if face and info.faceCooldown > 0 then ui.DrawRectFilled(x0, y1 - size * info.faceCooldown, x1, y1, 0, 0, 0, 0.5 * alpha, r) end
    if face and not info.enabled then ui.DrawRectFilled(x0, y0, x1, y1, 0, 0, 0, 0.45 * alpha, r) end
    T.font(size >= T.px(64) and 15 or 13)
    local tc = C.text
    if face then
      tc = rec.tc
      tc[1], tc[2], tc[3] = info.color[1], info.color[2], info.color[3]
    end
    T.textIn(x0 + 3, y0, x1 - 3, y1, fitted(rec.fit, label, size - 6), tc, 0.5, alpha)
    if face and info.overlay ~= "" then T.textIn(x0 + 3, y1 - size * 0.4, x1 - 4, y1, info.overlay, C.text, 1, alpha) end
    T.unfont()
  end
  if face and info.running then T.outline(x0, y0, x1, y1, C.accent, alpha, r, T.px(2)) end
  -- the cool-down: the time still to go, shaded, shrinking toward the bottom as the gem bar's does
  if info.cooldown then
    local top = y0 + size * info.cooldown
    ui.DrawRectFilled(x0, top, x1, y1, 0, 0, 0, 0.55 * alpha, r)
    ui.DrawLine(x0, top, x1, top, C.accent[1], C.accent[2], C.accent[3], 0.8 * alpha, T.px(2))
    if info.left >= 1000 then
      local secs = (info.left + 999) // 1000
      T.font(size >= T.px(64) and 18 or 14)
      T.textIn(x0, y0, x1, y1, secs >= 60 and string.format("%dm", (secs + 59) // 60) or tostring(secs), C.text, 0.5, alpha)
      T.unfont()
    end
  end
  if info.checked then
    ui.DrawRectFilled(x0, y0, x1, y1, C.accent[1], C.accent[2], C.accent[3], 0.18 * alpha, r)
    T.outline(x0, y0, x1, y1, C.accent, alpha, r, T.px(3))
  end
  T.outline(x0, y0, x1, y1, pressed and C.accent or C.edge, alpha, r, pressed and T.px(3) or 1)
  if hovered then T.outline(x0, y0, x1, y1, C.accent, 0.8, r, T.px(2)) end
  -- the pad button that fires it, at the slot's outer corner
  local gs = math.max(T.px(18), size * 0.34)
  T.font(size >= T.px(64) and 15 or 12)
  T.glyph(pad, SLOT_BUTTONS[n], x1 - gs * 0.25, y1 - gs * 0.25, gs, alpha)
  T.unfont()
  if clicked then API.hotbars.Click(screen, child, "left") end
  -- the right button as the game takes it: a quick click on release, held 500 ms the slot's menu
  if hovered and ui.IsMouseClicked(ui.Mouse.Right) then rightPress = { key = key, at = ui.GetTime() } end
  if rightPress and rightPress.key == key then
    if not ui.IsMouseDown(ui.Mouse.Right) then
      if not rightPress.held and hovered then API.hotbars.Click(screen, child, "right") end
      rightPress = nil
    elseif not rightPress.held and ui.GetTime() - rightPress.at > 0.5 then
      rightPress.held = true
      API.hotbars.Click(screen, child, "rightheld")
    end
  end
  if hovered and face and info.tooltip ~= "" then ui.SetTooltip(info.tooltip)
  elseif hovered and label ~= "" then ui.SetTooltip(label) end
end

-- One set: the D-pad diamond and the face diamond around cx, cy.
local function set(s, cx, cy, size, alpha, held)
  local step = size * 1.08
  local half = step * 1.75 -- each diamond's centre from the set's centre
  for d = 1, 2 do
    local dx = d == 1 and cx - half or cx + half
    T.rect(dx - step * 1.6, cy - step * 1.6, dx + step * 1.6, cy + step * 1.6, C.bg, 0.55 * alpha, step * 1.6)
    for i, o in ipairs(DIAMOND) do slot(s, (d - 1) * 4 + i, dx + o[1] * step, cy + o[2] * step, size, alpha, held) end
  end
  -- which trigger holds it, between the diamonds
  T.font(16)
  local gy = cy
  local gs = T.px(26)
  if s == 1 then T.glyph(pad, "lt", cx, gy, gs, alpha)
  elseif s == 2 then T.glyph(pad, "rt", cx, gy, gs, alpha)
  else
    T.glyph(pad, "lt", cx, gy - gs * 0.6, gs, alpha)
    T.glyph(pad, "rt", cx, gy + gs * 0.6, gs, alpha)
  end
  T.unfont()
end

-- widths: a set spans two diamonds (3.2 steps each) and the gap between them
local function span(size) return size * 1.08 * (3.5 + 3.2) end
local sizes = {} -- each set's slot size this frame

ui.window("Cross Hotbar", function()
  if not W.shown("HotButtonWnd") then return end
  T.frame()
  T.resetFonts()
  local held = pad.cross_set() or 0
  local base, big = T.px(50), T.px(70)
  for s = 1, 3 do sizes[s] = (held == s) and big or base end
  local gap = T.px(40)
  local total = span(sizes[1]) + span(sizes[2]) + span(sizes[3]) + 2 * gap
  local h = T.px(300)
  local wx0 = math.max(0, (T.W - total) / 2 - T.px(20))
  ui.SetNextWindowPos(wx0, T.H - h, ui.Cond.Always)
  ui.SetNextWindowSize(math.min(T.W, total + T.px(40)), h, ui.Cond.Always)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoBackground | ui.WindowFlags.NoSavedSettings
      | ui.WindowFlags.NoMove | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoBringToFrontOnFocus | W.NoNav
  if ui.Begin("Cross Hotbar", nil, flags) then
    T.font(T.BASE)
    local cy = T.H - T.px(118)
    local x = (T.W - total) / 2
    -- where the bar is, for the chat ticker to keep clear of it (chat.lua)
    local bar = GPSTATE.crossBar or {}
    bar.x0, bar.y0, bar.x1, bar.at = x, cy - T.px(16) - big * 1.08 * 1.6, x + total, ui.GetTime()
    GPSTATE.crossBar = bar
    for s = 1, 3 do
      local sw = span(sizes[s])
      local alpha = (held == 0 or held == s) and 1 or 0.35
      set(s, x + sw / 2, held == s and cy - T.px(16) or cy, sizes[s], alpha, held == s)
      x = x + sw + gap
    end
    T.unfont()
  end
  ui.End()
  ui.PopStyleVar(2)
end)
