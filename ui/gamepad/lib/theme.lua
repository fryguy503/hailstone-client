-- The gamepad pack's look: dark translucent panels with rounded corners, one accent colour, large legible text and
-- bars, sized for a screen across the room (10-foot UI). No game UI art is drawn (ui.ini game_art = icons): every
-- shape here is drawn, and only the item, spell and gem icon sheets are used for pictures.
--
-- Sizes are design pixels at 1920 x 1080; T.px scales them to the screen (T.S), so the base text is 22 px at 1080p
-- and grows and shrinks with the display. Fonts: the client builds the EQ fonts 1..6 (6 is 20 px times the UI
-- scale); T.font(px) picks font 6 and scales the window's font to that many design pixels.
--
-- One table for the whole pack (every file's dofile gets the same one): T.frame() sets the screen's scale once a frame
-- for every window, so no file's T.px stays at the scale it was loaded with.
local API = hs.UI
if GPTHEME then return GPTHEME end
local T = {}

-- ---------------------------------------------------------------- palette
-- The colours of the palette the player picked (lib/palettes.lua: the tokens and what each is for), put into T.C in
-- place: every colour table keeps its identity, so windows that hold T.C.text and the like follow a change at once.
local PALETTES = dofile("lib/palettes.lua")
T.C = {}
local C = T.C

local function paletteColors(id)
  local byId = {}
  for _, p in ipairs(PALETTES.list) do byId[p.id] = p end
  local function resolve(p, out)
    if not p then return out end
    if p.base then resolve(byId[p.base], out) end
    for k, v in pairs(p.colors or {}) do out[k] = v end
    return out
  end
  return resolve(byId[id] or byId[PALETTES.default], {})
end

local function applyPalette(id)
  for k, v in pairs(paletteColors(id)) do
    local c = C[k]
    if not c then c = {} C[k] = c end
    c[1], c[2], c[3], c[4] = v[1], v[2], v[3], v[4] or 1
  end
  T.palette = id
end
applyPalette(ui.GetSetting("palette") or PALETTES.default)

-- The palettes to pick from and picking one (saved as the setting `palette`); also the global ui_palette, which
-- every pack's theme sets up the same way.
T.palettes = PALETTES
function T.setPalette(id)
  ui.SetSetting("palette", id)
  applyPalette(id)
end
ui_palette = {
  list = function() return PALETTES.list end,
  current = function() return T.palette end,
  set = function(id) T.setPalette(id) end,
}

T.BASE = 22 -- body text, design px (>= 20 px at 1080p)

-- ---------------------------------------------------------------- scale
T.S, T.U, T.W, T.H = 1, 1, 1920, 1080
-- Call once per frame before laying out (each window does). Every window calls it, so the frame's first call does the
-- work and the others only take the UI scale again (an option can change it within the frame, ui.GetIO).
function T.frame()
  local frame = ui.GetFrameCount()
  if T.frameDone == frame then
    T.U = ui.GetIO().uiScale or 1
    return
  end
  T.frameDone = frame
  -- testing: test.palette.cycle=<seconds> in ui/settings/gamepad.txt shows each palette in turn (logged)
  local every = tonumber(ui.GetSetting("test.palette.cycle") or "")
  if every and every > 0 then
    local id = PALETTES.list[math.floor(ui.GetTime() / every) % #PALETTES.list + 1].id
    if id ~= T.palette then
      applyPalette(id)
      ui.Log("gamepad ui: palette " .. id)
    end
  end
  local io = ui.GetIO()
  T.W, T.H = io.width or 1920, io.height or 1080
  T.U = io.uiScale or 1
  T.S = math.max(0.5, math.min(T.W / 1920, T.H / 1080))
end
function T.px(n) return math.floor(n * T.S + 0.5) end

-- ---------------------------------------------------------------- fonts
local fontStack = {}
-- Text of `size` design pixels from here on (in the current window), until T.unfont().
function T.font(size)
  local scale = (size or T.BASE) * T.S / (20 * T.U)
  table.insert(fontStack, scale)
  ui.PushFont(6)
  ui.SetWindowFontScale(scale)
end
function T.unfont()
  ui.PopFont()
  table.remove(fontStack)
  ui.SetWindowFontScale(fontStack[#fontStack] or 1)
end
-- Resets the stack (a new window).
function T.resetFonts()
  fontStack = {}
end

-- ---------------------------------------------------------------- drawing
function T.rect(x0, y0, x1, y1, c, a, r)
  ui.DrawRectFilled(x0, y0, x1, y1, c[1], c[2], c[3], (a or 1) * (c[4] or 1), r or 0)
end
function T.outline(x0, y0, x1, y1, c, a, r, t)
  ui.DrawRect(x0, y0, x1, y1, c[1], c[2], c[3], (a or 1) * (c[4] or 1), r or 0, t or 1)
end

-- A panel: the translucent rounded body with a hairline edge.
function T.panel(x0, y0, x1, y1, alpha, radius, colour)
  local r = radius or T.px(14)
  T.rect(x0, y0, x1, y1, colour or C.bg, alpha, r)
  T.outline(x0, y0, x1, y1, C.edge, alpha, r, 1)
end

-- Text with a soft shadow (legible over the world), at x, y (top left); draws only (T.text also measures it).
local function draw(x, y, text, c, a)
  c = c or C.text
  local o = math.max(1, T.px(2))
  local sh = C.textShadow
  ui.DrawText(x + o * 0.5, y + o, sh[1], sh[2], sh[3], sh[4] * (a or 1), text)
  ui.DrawText(x, y, c[1], c[2], c[3], (a or 1) * (c[4] or 1), text)
end
T.draw = draw
-- T.draw, then its width and height (a call that does not use them is cheaper with T.draw).
function T.text(x, y, text, c, a)
  draw(x, y, text, c, a)
  return ui.CalcTextSize(text)
end
-- Aligned in a box: ax 0 left, 0.5 centre, 1 right (vertical centre); clipped to the box. Returns its width and height.
function T.textIn(x0, y0, x1, y1, text, c, ax, a)
  local tw, th = ui.CalcTextSize(text)
  local x = x0 + ((x1 - x0) - tw) * (ax or 0)
  ui.PushClipRect(x0, y0, x1, y1, true)
  draw(x, y0 + ((y1 - y0) - th) / 2, text, c, a)
  ui.PopClipRect()
  return tw, th
end
-- Shortens text with "..." to fit `w` pixels: the longest start that fits with the dots after it (at least its first
-- character). A binary search over the text's length (a start is never narrower than a shorter one), cut only between
-- UTF-8 characters.
local function charEnd(text, n) -- n, moved back to the end of a character (never before the first one's end)
  local b = text:byte(n + 1)
  while b and b >= 0x80 and b < 0xC0 and n > 1 do
    n = n - 1
    b = text:byte(n + 1)
  end
  return n
end
function T.fit(text, w)
  if ui.CalcTextSize(text) <= w then return text end
  local len = #text
  local lo = 1 -- the first character's end: the shortest start kept
  while lo < len do
    local b = text:byte(lo + 1)
    if b < 0x80 or b >= 0xC0 then break end
    lo = lo + 1
  end
  local hi = len
  while lo < hi do
    local mid = (lo + hi + 1) // 2
    if ui.CalcTextSize(text:sub(1, charEnd(text, mid)) .. "...") <= w then lo = mid else hi = mid - 1 end
  end
  return text:sub(1, charEnd(text, lo)) .. "..."
end

-- UTF-8 edits: text cut, shortened or started only between characters (a name or a link label typed or pasted in can
-- hold them). A byte that is not part of a well-formed character (Latin-1 text) counts as one character.
local function isCont(b) return b ~= nil and b >= 0x80 and b < 0xC0 end
-- The longest start of text of at most n bytes that does not end inside a character.
function T.cutBytes(text, n)
  if #text <= n then return text end
  if n <= 0 then return "" end
  if isCont(text:byte(n + 1)) then
    -- back to the lead byte of the character it splits (at most three bytes back), the cut before it
    for i = n, math.max(1, n - 2), -1 do
      local b = text:byte(i)
      if b >= 0xC0 then return text:sub(1, i - 1) end
      if b < 0x80 then break end
    end
  end
  return text:sub(1, n)
end
-- text without its last character (Delete on the keys).
function T.dropChar(text)
  local n = #text
  local i = n
  while i > 1 and i > n - 3 and isCont(text:byte(i)) do i = i - 1 end
  if i < n and text:byte(i) >= 0xC0 then return text:sub(1, i - 1) end
  return text:sub(1, n - 1)
end
-- i (a byte of text), moved on to the first byte of a character.
function T.charStart(text, i)
  local stop = math.min(#text + 1, i + 3)
  local j = i
  while j < stop and isCont(text:byte(j)) do j = j + 1 end
  if j == stop and isCont(text:byte(j)) then return i end -- (not UTF-8: bytes as they are)
  return j
end

-- A bar: fraction 0..1 of colour c, with a text centred on it (and an optional right-hand text).
function T.bar(x0, y0, x1, y1, frac, c, text, right, alpha)
  local h = y1 - y0
  local r = math.min(h / 2, T.px(6))
  T.rect(x0, y0, x1, y1, C.sunken, alpha, r)
  if frac and frac > 0 then
    local fx = x0 + (x1 - x0) * math.min(1, frac)
    T.rect(x0, y0, math.max(fx, x0 + 2 * r), y1, c, alpha, r)
    -- a lighter top half: the bar reads as lit, not flat
    ui.DrawRectFilled(x0 + r * 0.5, y0 + 1, math.max(fx, x0 + 2 * r) - r * 0.5, y0 + h * 0.45, 1, 1, 1, 0.12 * (alpha or 1), r)
  end
  if text and text ~= "" then T.textIn(x0 + T.px(10), y0, x1 - T.px(10), y1, text, C.text, 0, alpha) end
  if right and right ~= "" then T.textIn(x0 + T.px(10), y0, x1 - T.px(10), y1, right, C.text, 1, alpha) end
end

-- The focus ring around an item: the accent, thick.
function T.focusRing(x0, y0, x1, y1, r)
  local o = T.px(3)
  T.outline(x0 - o, y0 - o, x1 + o, y1 + o, C.accent, 1, (r or T.px(10)) + o, T.px(3))
end

-- An icon from a sheet (hs.ItemIcon / hs.ChildArt: texture, u0, v0, u1, v1).
function T.icon(tex, u0, v0, u1, v1, x0, y0, x1, y1, a)
  if tex and type(tex) == "userdata" then ui.DrawImage(tex, x0, y0, x1, y1, u0, v0, u1, v1, 1, 1, 1, a or 1) end
end
function T.itemIcon(icon, x0, y0, x1, y1, a)
  if not icon then return end
  local tex, u0, v0, u1, v1 = API.assets.ItemIcon(icon)
  T.icon(tex, u0, v0, u1, v1, x0, y0, x1, y1, a)
end

-- ---------------------------------------------------------------- button glyphs
-- The pad's buttons, drawn as shapes in the connected pad's style (hs.gamepad.type / glyph).
local FACE = {
  xbox = { a = { 0.36, 0.78, 0.30 }, b = { 0.92, 0.32, 0.30 }, x = { 0.27, 0.56, 0.96 }, y = { 0.96, 0.80, 0.26 } },
  playstation = { a = { 0.50, 0.66, 0.98 }, b = { 0.95, 0.38, 0.42 }, x = { 0.92, 0.52, 0.80 }, y = { 0.32, 0.86, 0.72 } },
}
FACE.generic, FACE.nintendo = FACE.xbox, FACE.xbox
local GLYPH_INK = { 0.94, 0.95, 0.98, 1 } -- a glyph's label: the glyph face (glyphBg) is dark on every palette, as a pad's buttons are
local function triangle(dir, cx, cy, s, c, a)
  if dir == "up" then ui.DrawTriangleFilled(cx, cy - s, cx + s, cy + s * 0.7, cx - s, cy + s * 0.7, c[1], c[2], c[3], a)
  elseif dir == "down" then ui.DrawTriangleFilled(cx - s, cy - s * 0.7, cx + s, cy - s * 0.7, cx, cy + s, c[1], c[2], c[3], a)
  elseif dir == "left" then ui.DrawTriangleFilled(cx - s, cy, cx + s * 0.7, cy - s, cx + s * 0.7, cy + s, c[1], c[2], c[3], a)
  else ui.DrawTriangleFilled(cx - s * 0.7, cy - s, cx + s, cy, cx - s * 0.7, cy + s, c[1], c[2], c[3], a) end
end
T.triangle = triangle

local ROUND = { a = true, b = true, x = true, y = true, up = true, down = true, left = true, right = true }
-- The width T.glyph draws button `b` at for a `size` tall glyph (pills are wider).
function T.glyphWidth(pad, b, size)
  if ROUND[b] then return size end
  return math.max(size, ui.CalcTextSize(pad.label(b)) + size * 0.6)
end

-- Draws button `b` ("a", "lb", "up", "start", ...) centred at cx, cy, `size` pixels tall; returns its width.
function T.glyph(pad, b, cx, cy, size, alpha)
  local a = alpha or 1
  local kind = pad.kind()
  local r = size / 2
  if b == "a" or b == "b" or b == "x" or b == "y" then
    local col = (FACE[kind] or {})[b]
    local gb = C.glyphBg
    ui.DrawCircleFilled(cx, cy, r, gb[1], gb[2], gb[3], 0.95 * a)
    if kind == "playstation" then -- the shapes, in their colours
      local s, t = r * 0.45, math.max(1.5, r * 0.16)
      if b == "a" then
        ui.DrawLine(cx - s, cy - s, cx + s, cy + s, col[1], col[2], col[3], a, t)
        ui.DrawLine(cx - s, cy + s, cx + s, cy - s, col[1], col[2], col[3], a, t)
      elseif b == "b" then ui.DrawCircle(cx, cy, s * 1.05, col[1], col[2], col[3], a, t)
      elseif b == "x" then ui.DrawRect(cx - s, cy - s, cx + s, cy + s, col[1], col[2], col[3], a, 0, t)
      else
        local h = s * 1.1
        ui.DrawLine(cx, cy - h, cx + h, cy + h * 0.7, col[1], col[2], col[3], a, t)
        ui.DrawLine(cx + h, cy + h * 0.7, cx - h, cy + h * 0.7, col[1], col[2], col[3], a, t)
        ui.DrawLine(cx - h, cy + h * 0.7, cx, cy - h, col[1], col[2], col[3], a, t)
      end
    else
      if col then ui.DrawCircle(cx, cy, r - 1, col[1], col[2], col[3], 0.9 * a, math.max(1.5, r * 0.12)) end
      local label = pad.label(b)
      local tw, th = ui.CalcTextSize(label)
      local c = col or GLYPH_INK
      ui.DrawText(cx - tw / 2, cy - th / 2, c[1], c[2], c[3], a, label)
    end
    return size
  elseif b == "up" or b == "down" or b == "left" or b == "right" then
    ui.DrawRectFilled(cx - r, cy - r, cx + r, cy + r, C.glyphBg[1], C.glyphBg[2], C.glyphBg[3], 0.95 * a, r * 0.35)
    triangle(b, cx, cy, r * 0.5, GLYPH_INK, a)
    return size
  end
  -- shoulders, triggers, sticks, menu buttons: a pill with the pad's label
  local label = pad.label(b)
  local tw, th = ui.CalcTextSize(label)
  local w = math.max(size, tw + size * 0.6)
  ui.DrawRectFilled(cx - w / 2, cy - r, cx + w / 2, cy + r, C.glyphBg[1], C.glyphBg[2], C.glyphBg[3], 0.95 * a, r)
  ui.DrawRect(cx - w / 2, cy - r, cx + w / 2, cy + r, 1, 1, 1, 0.18 * a, r, 1)
  ui.DrawText(cx - tw / 2, cy - th / 2, GLYPH_INK[1], GLYPH_INK[2], GLYPH_INK[3], a, label)
  return w
end

-- A row of hints: { { "a", "Select" }, { "b", "Back" } } from x, y (left to right); returns the end x.
function T.hints(pad, hints, x, y, size)
  size = size or T.px(30)
  for _, h in ipairs(hints) do
    local w = T.glyphWidth(pad, h[1], size)
    T.glyph(pad, h[1], x + w / 2, y + size / 2, size)
    x = x + w + T.px(8)
    local tw = T.textIn(x, y, x + 1000, y + size, h[2], C.dim, 0)
    x = x + tw + T.px(26)
  end
  return x
end
function T.hintsWidth(pad, hints, size)
  size = size or T.px(30)
  local x = 0
  for _, h in ipairs(hints) do
    local w = T.glyphWidth(pad, h[1], size)
    x = x + w + T.px(8) + ui.CalcTextSize(h[2]) + T.px(26)
  end
  return x - T.px(26)
end

-- ---------------------------------------------------------------- focusable widgets
-- Every widget is an ImGui item at the layout cursor (so pad navigation and the mouse both reach it), drawn here.

-- A pad press that closed one window (a dialog, a picker) or opened this one must not also press what the focus lands
-- on: for a moment after a window of the pack gains the focus (lib/window.lua), the pad's activations are dropped.
-- Mouse clicks are not (the navigation cursor is hidden then).
function T.settle(clicked, pad)
  if clicked and pad and pad.navVisible() and (GPSTATE and GPSTATE.padSettleUntil or 0) > ui.GetTime() then
    return false
  end
  return clicked
end
-- A pad button a window reads itself (a dialog's A to accept, not an ImGui item's), dropped in the same settling time
-- whether or not the navigation cursor shows (a window without ImGui navigation never shows it).
function T.settledPress(pad, b)
  return pad.menuPressed(b) and (GPSTATE and GPSTATE.padSettleUntil or 0) <= ui.GetTime()
end

-- A button w x h: returns clicked, focused, x0, y0, x1, y1. opts: primary (accent face), disabled, glyph (a pad
-- button drawn on its left), sub (a second, dimmer line), align (0 left, 0.5 centre). off: disabled as well (a wrapper
-- that disables the caller's opts passes it here rather than copying them).
local NO_OPTS = {}
T.NO_OPTS = NO_OPTS -- (an empty opts to pass without making one; never written to)
function T.button(id, label, w, h, opts, pad, off)
  opts = opts or NO_OPTS
  local disabled = opts.disabled or off
  local x0, y0 = ui.GetCursorScreenPos()
  local clicked = T.settle(ui.InvisibleButton(id, math.max(1, w), math.max(1, h), ui.ButtonFlags.EnableNav), pad)
  if pad and pad.yActivated(clicked) then clicked = false end
  local focused = pad and pad.focused() or ui.IsItemHovered()
  local active = ui.IsItemActive()
  local x1, y1 = x0 + w, y0 + h
  local r = math.min(T.px(12), h / 2)
  local face = opts.primary and C.accent or C.raised
  local fa = disabled and 0.45 or 1
  if opts.primary then
    T.rect(x0, y0, x1, y1, face, (active and 0.75 or 0.92) * fa, r)
  else
    T.rect(x0, y0, x1, y1, face, fa, r)
    if focused and not disabled then T.rect(x0, y0, x1, y1, C.accent, active and 0.30 or 0.18, r) end
  end
  T.outline(x0, y0, x1, y1, C.edge, 1, r)
  if focused then T.focusRing(x0, y0, x1, y1, r) end
  local tx = x0 + T.px(16)
  if opts.glyph and pad then
    local gs = math.min(h - T.px(12), T.px(32))
    local gw = T.glyphWidth(pad, opts.glyph, gs)
    T.glyph(pad, opts.glyph, tx + gw / 2, (y0 + y1) / 2, gs, fa)
    tx = tx + gw + T.px(12)
  end
  local col = opts.primary and C.onAccent or C.text
  if opts.sub then
    local _, th = ui.CalcTextSize("Ag")
    T.textIn(tx, y0 + h * 0.5 - th, x1 - T.px(12), y0 + h * 0.5, label, col, opts.align or 0, fa)
    T.textIn(tx, y0 + h * 0.5, x1 - T.px(12), y0 + h * 0.5 + th, opts.sub, C.dim, opts.align or 0, fa)
  else
    local ax = opts.align or (opts.glyph and 0 or 0.5)
    T.textIn(ax == 0.5 and x0 + T.px(8) or tx, y0, x1 - T.px(8), y1, label, col, ax, fa)
  end
  if disabled then clicked = false end
  return clicked, focused, x0, y0, x1, y1
end

-- An item slot `size` square: the item's icon and stack count; returns clicked, focused, x0, y0, x1, y1.
-- opts: label (shown dim when empty: the slot's name), selected, dim.
function T.slot(id, item, size, opts, pad)
  opts = opts or NO_OPTS
  local x0, y0 = ui.GetCursorScreenPos()
  local clicked = T.settle(ui.InvisibleButton(id, size, size, ui.ButtonFlags.EnableNav), pad)
  if pad and pad.yActivated(clicked) then clicked = false end
  local focused = pad and pad.focused() or ui.IsItemHovered()
  local x1, y1 = x0 + size, y0 + size
  local r = T.px(10)
  T.rect(x0, y0, x1, y1, item and C.raised or C.sunken, 1, r)
  T.outline(x0, y0, x1, y1, C.edge, 1, r)
  if item then
    local m = math.floor(size * 0.12)
    T.itemIcon(item.icon, x0 + m, y0 + m, x1 - m, y1 - m, opts.dim and 0.4 or 1)
    if item.cooldown then -- a clicky's recast (the game's slot cool-down): the part still to go, from below
      ui.DrawRectFilled(x0, y0 + size * item.cooldown, x1, y1, 0, 0, 0, 0.5, r)
    end
    if item.stackable and (item.count or 1) >= 1 then -- a single one too: it says the item stacks
      T.font(16)
      local t = tostring(item.count)
      local tw, th = ui.CalcTextSize(t)
      draw(x1 - tw - T.px(6), y1 - th - T.px(3), t)
      T.unfont()
    end
  elseif opts.label then
    T.font(13)
    T.textIn(x0 + 2, y0, x1 - 2, y1, T.fit(opts.label, size - 4), C.faint, 0.5)
    T.unfont()
  end
  if opts.selected then T.outline(x0 + 1, y0 + 1, x1 - 1, y1 - 1, C.endurance, 1, r, T.px(3)) end
  if focused then T.focusRing(x0, y0, x1, y1, r) end
  return clicked, focused, x0, y0, x1, y1
end

-- A section heading at the cursor.
function T.heading(text, w)
  local x0, y0 = ui.GetCursorScreenPos()
  T.font(18)
  local _, th = ui.CalcTextSize("Ag")
  draw(x0, y0, text:upper(), C.dim)
  T.unfont()
  ui.Dummy(w or 1, th + T.px(4))
end

-- Seconds in one short unit: "3h", "12m", "42s" (under an icon).
function T.short(sec)
  sec = math.floor(sec)
  if sec >= 3600 then return (sec // 3600) .. "h" end
  if sec >= 60 then return (sec // 60) .. "m" end
  return sec .. "s"
end

-- Seconds as "1h 02m", "3m 05s", "42s".
function T.duration(sec)
  sec = math.floor(sec)
  if sec >= 3600 then return string.format("%dh %02dm", sec // 3600, (sec % 3600) // 60) end
  if sec >= 60 then return string.format("%dm %02ds", sec // 60, sec % 60) end
  return sec .. "s"
end

GPTHEME = T
return T
