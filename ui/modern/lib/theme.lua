-- The modern UI's look: colours, and the drawing pieces every window is made of (panels, bars, icons, text with a
-- shadow, hit boxes). Everything is drawn in screen coordinates; sizes are design pixels times the window's scale s.
--
--   local T = dofile("lib/theme.lua")
--   T.Panel(x0, y0, x1, y1, s)
--   T.Bar("player.hp", x0, y0, x1, y1, s, fraction, T.color.hp, { text = "1200 / 1500" })

local API = hs.UI
local T = {}

-- ---------------------------------------------------------------- colours
-- { r, g, b, a } each, from the palette the player picked (lib/palettes.lua: the tokens and what each is for). Bars use
-- a colour and a darker shade of it for the gradient's bottom.
-- One table for every script (each dofile of this file makes a new copy of the rest): a palette is put into it in
-- place, every colour table keeping its identity, so windows that hold T.color.text and the like see the change at
-- once, without a restart.
local PALETTES = dofile("lib/palettes.lua")
modern_colors = modern_colors or {}
T.color = modern_colors
local C = T.color

-- An empty options table, for the pieces called without one (none of them keeps or changes it).
local NO_OPTS = {}
T.NO_OPTS = NO_OPTS

-- ---------------------------------------------------------------- settings
-- T.Setting(key): ui.GetSetting(key), read once a frame for every script (each window asks for the same few keys
-- every frame, and each read is a call into the host and a lookup there). ui.SetSetting is wrapped, once, so that a
-- write drops the key it writes: a value set mid-frame is read back at once, as it was without the cache. (A script
-- that kept its own reference to ui.SetSetting from before the wrap is seen from the next frame.)
-- The tables are kept from frame to frame, each key stamped with the frame it was read in: a table made anew each
-- frame grew again as the windows asked for their keys, about 6 KB of garbage a frame. Past MAX_SETTING_KEYS keys
-- (a script asking for ever new ones) they start afresh.
local MAX_SETTING_KEYS = 1024
modern_settings = modern_settings or { values = {} }
local settings = modern_settings
settings.seen = settings.seen or {} -- key -> the frame its value was read in
settings.count = settings.count or 0
if ui.SetSetting ~= settings.set then
  local SetSetting = ui.SetSetting
  settings.set = function(key, value)
    SetSetting(key, value)
    if settings.seen[key] then settings.seen[key] = false end -- (read again; still counted)
  end
  ui.SetSetting = settings.set
end

function T.Setting(key)
  local frame = ui.GetFrameCount()
  local seen = settings.seen
  local at = seen[key]
  if at ~= frame then
    if at == nil then
      if settings.count >= MAX_SETTING_KEYS then
        seen = {}
        settings.seen, settings.values, settings.count = seen, {}, 0
      end
      settings.count = settings.count + 1
    end
    settings.values[key] = ui.GetSetting(key) or false -- (false: no value; nil is not kept in a table)
    seen[key] = frame
  end
  return settings.values[key] or nil
end

-- The colours of palette `id` (its base's, then its own).
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
  modern_palette = id
  modern_palette_gen = (modern_palette_gen or 0) + 1 -- what was worked out from the colours (lib/style.lua) is redone
end

-- The palette in use, the list to pick from, and picking one (saved as the setting `palette`; Options shows these
-- through the global ui_palette, which every pack's theme sets up the same way).
T.palettes = PALETTES
function T.Palette() return modern_palette end
function T.SetPalette(id)
  ui.SetSetting("palette", id)
  applyPalette(id)
end
if not modern_palette then applyPalette(ui.GetSetting("palette") or PALETTES.default) end
ui_palette = ui_palette or {
  list = function() return PALETTES.list end,
  current = function() return modern_palette end,
  set = function(id) T.SetPalette(id) end,
}

-- Testing: test.palette.cycle=<seconds> in ui/settings/modern.txt shows each palette in turn for that long (for
-- screenshots of every palette in one run); each change is logged.
function T.TestPaletteCycle()
  local every = tonumber(T.Setting("test.palette.cycle") or "")
  if not every or every <= 0 then return end
  local n = #PALETTES.list
  local id = PALETTES.list[math.floor(ui.GetTime() / every) % n + 1].id
  if id ~= modern_palette then
    applyPalette(id)
    ui.Log("modern ui: palette " .. id)
  end
end

local function unpack4(c, a) return c[1], c[2], c[3], (a or 1) * (c[4] or 1) end
T.rgba = unpack4

-- The mouse's state, one for every script (each dofile of this file makes a new copy of the rest).
modern_mouse = modern_mouse or { rightTaken = false, rightPress = nil, pickedUp = false }
local mouse = modern_mouse

-- A right click a piece used for itself (a buff removed, a gem forgotten): the window's menu does not open.
function T.TakeRightClick() mouse.rightTaken = true end
function T.RightClickTaken() return mouse.rightTaken end
function T.ResetRightClick() mouse.rightTaken, mouse.altRightTaken = false, false end
-- An Alt right click a piece used for itself (a list row whose window takes it): not even Alt opens the frame menu.
function T.TakeAltRightClick() mouse.rightTaken, mouse.altRightTaken = true, true end
function T.AltRightClickTaken() return mouse.altRightTaken == true end

-- clicked = T.Hit(id, x0, y0, x1, y1 [, overlap])   -- an invisible button over a screen rectangle (then
-- ui.IsItemHovered, ...). overlap (default true) lets a later item lie over this one. A piece that lies over the
-- frame's own drag handle (a buff icon) passes false: ImGui hovers an overlapping item only once the frame before
-- hovered it too, so with true the first click on it would go to the frame.
-- (Hit boxes are ImGui items, placed relative to the current window.)
function T.Hit(id, x0, y0, x1, y1, overlap)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y0 - wy + ui.GetScrollY()) -- (the cursor position is in scrolled window coordinates)
  if overlap ~= false then ui.SetNextItemAllowOverlap() end
  return ui.InvisibleButton(id, math.max(x1 - x0, 1), math.max(y1 - y0, 1))
end

-- ---------------------------------------------------------------- text
-- The EQ UI fonts (1..6: 10, 12, 13, 14, 16, 20 pixels at UI scale 1); the window's font scale applies.
T.font = { tiny = 2, small = 3, normal = 5, large = 6, huge = 6 }

function T.TextSize(text, font)
  ui.PushFont(font or T.font.normal)
  local w, h = ui.CalcTextSize(text)
  ui.PopFont()
  return w, h
end

-- Text with a soft shadow, readable over bars and the world. align: "left" (default), "center", "right" (x is
-- then the centre or the right edge); shadow: its colour (the palette's textShadow). Returns its width and height.
-- (T.Text in the font pushed now)
local function drawText(x, y, text, color, align, alpha, shadow)
  local w, h = ui.CalcTextSize(text)
  if align == "center" then x = x - w / 2 elseif align == "right" then x = x - w end
  x, y = math.floor(x + 0.5), math.floor(y + 0.5)
  local a = alpha or 1
  local sh = shadow or C.textShadow
  ui.DrawText(x + 1, y + 1, sh[1], sh[2], sh[3], sh[4] * a, text)
  local r, g, b, ca = unpack4(color or C.text, a)
  ui.DrawText(x, y, r, g, b, ca, text)
  return w, h
end

function T.Text(x, y, text, color, font, align, alpha, shadow)
  if not text or text == "" then return 0, 0 end
  ui.PushFont(font or T.font.normal)
  local w, h = drawText(x, y, text, color, align, alpha, shadow)
  ui.PopFont()
  return w, h
end

-- The text cut to `width` with "...": the longest start of it that fits with the dots, cut between characters (a
-- UTF-8 character is never split), at least its first character. Found by halving: a cut's width only grows with
-- its length.
local function continuation(b) return b ~= nil and b >= 0x80 and b < 0xC0 end
function T.Fit(text, width, font)
  ui.PushFont(font or T.font.normal)
  local w = ui.CalcTextSize(text)
  if w > width then
    local len = #text
    local first = 1 -- the end of the first character: the shortest cut
    while first < len and continuation(text:byte(first + 1)) do first = first + 1 end
    -- the longest n in first + 1 .. len - 1 whose cut (n back to a character's end) fits, else first
    local best, lo, hi = first, first + 1, len - 1
    while lo <= hi do
      local mid = (lo + hi) // 2
      local n = mid
      while n > first and continuation(text:byte(n + 1)) do n = n - 1 end
      if ui.CalcTextSize(text:sub(1, n) .. "...") <= width then
        best, lo = n, mid + 1
      else
        hi = mid - 1
      end
    end
    text = text:sub(1, best) .. "..."
  end
  ui.PopFont()
  return text
end

-- ---------------------------------------------------------------- panels
-- A rounded translucent panel with a soft shadow, a lighter top edge and a hairline border. opts: alpha, radius,
-- glow = { r, g, b, a } (an outer ring: auto attack, the target), glowWidth (the ring's width in design pixels,
-- default 1.5; wider adds a soft halo outside it), opaque (the dialogs' solid fill).
function T.Panel(x0, y0, x1, y1, s, opts)
  opts = opts or NO_OPTS
  local a = opts.alpha or 1
  local rad = (opts.radius or 7) * s
  local sh, ink = C.shadow, C.ink
  ui.DrawRectFilled(x0 + 1 * s, y0 + 3 * s, x1 + 1 * s, y1 + 4 * s, sh[1], sh[2], sh[3], 0.8 * sh[4] * a, rad + 2 * s)
  local r, g, b, pa = unpack4(C.panel, a)
  if opts.opaque then pa = 0.97 * a end
  ui.DrawRectFilled(x0, y0, x1, y1, r, g, b, pa, rad)
  ui.DrawRectFilled(x0 + 1, y0 + 1, x1 - 1, y0 + math.min(16 * s, (y1 - y0) / 2), ink[1], ink[2], ink[3], 0.03 * a, rad)
  r, g, b, pa = unpack4(C.border, a)
  ui.DrawRect(x0, y0, x1, y1, r, g, b, pa, rad, 1)
  if opts.glow then
    r, g, b, pa = unpack4(opts.glow, a)
    local gw = (opts.glowWidth or 1.5) * s
    ui.DrawRect(x0 - gw, y0 - gw, x1 + gw, y1 + gw, r, g, b, pa, rad + gw, gw)
    if gw > 1.5 * s then
      local ho = gw * 2.5 -- a 2 * gw stroke flush outside the ring (strokes straddle their line)
      ui.DrawRect(x0 - ho, y0 - ho, x1 + ho, y1 + ho, r, g, b, pa * 0.3, rad + ho, gw * 2)
    end
  end
end

-- ---------------------------------------------------------------- bars
-- Displayed values ease toward the real one, and a lost part lingers as a pale "trail" before it drains, so a hit
-- is easy to read. Keyed by the bar's id.
local shown, trail, trailHold = {}, {}, {}

local function eased(key, target)
  local now = ui.GetTime()
  local v = shown[key]
  if v == nil or math.abs(v - target) > 0.5 then v = target end
  local fps = ui.GetIO().fps
  local dt = math.min(fps > 0 and 1 / fps or 0.016, 0.1)
  v = v + (target - v) * math.min(1, dt * 14)
  if math.abs(v - target) < 0.001 then v = target end
  shown[key] = v
  local t = trail[key] or target
  if target >= t then
    t, trailHold[key] = target, nil
  else
    trailHold[key] = trailHold[key] or now + 0.45
    if now > trailHold[key] then t = math.max(target, t - dt * 0.6) end
  end
  trail[key] = t
  return v, t
end

-- T.Bar(key, x0, y0, x1, y1, s, fraction, color, opts)   fraction nil: an empty track
-- opts: text (left, inside), right (right, inside), center, font, noTrail, radius, alpha
function T.Bar(key, x0, y0, x1, y1, s, fraction, color, opts)
  opts = opts or NO_OPTS
  local a = opts.alpha or 1
  local h = y1 - y0
  local rad = math.min(opts.radius and opts.radius * s or h / 2, h / 2)
  ui.DrawRectFilled(x0, y0, x1, y1, C.inset[1], C.inset[2], C.inset[3], C.inset[4] * a, rad)
  if fraction then
    fraction = math.max(0, math.min(1, fraction))
    local v, t = eased(key, fraction)
    if opts.noTrail then v, t = fraction, fraction end
    local w = x1 - x0
    if t > v + 0.002 then
      ui.DrawRectFilled(x0, y0, x0 + math.max(w * t, rad * 2), y1, 1, 0.92, 0.85, 0.55 * a, rad)
    end
    if v > 0.001 then
      local fx = x0 + math.max(w * v, math.min(rad * 2, w))
      local r, g, b = color[1], color[2], color[3]
      ui.DrawRectFilled(x0, y0, fx, y1, r * 0.62, g * 0.62, b * 0.62, a, rad)
      ui.DrawRectFilled(x0, y0, fx, y0 + h * 0.62, r, g, b, a, rad)
      ui.DrawRectFilled(x0 + rad * 0.5, y0 + 1, fx - rad * 0.5, y0 + math.max(1, h * 0.22), 1, 1, 1, 0.22 * a, rad)
    end
  end
  ui.DrawRect(x0, y0, x1, y1, C.ink[1], C.ink[2], C.ink[3], 0.07 * a, rad, 1)
  local font = opts.font or T.font.small
  if opts.text or opts.right or opts.center then
    ui.PushFont(font) -- (one push for the line's height and the texts)
    local _, th = ui.CalcTextSize("Ag")
    local ty = y0 + (h - th) / 2
    ui.PushClipRect(x0, y0, x1, y1, true)
    -- (in the palette's bar text colour, light on every palette: the fills are saturated)
    local tc, sh = opts.textColor or C.barText, C.barShadow
    local t, r, c = opts.text, opts.right, opts.center
    if t and t ~= "" then drawText(x0 + 6 * s, ty, t, tc, nil, a, sh) end
    if r and r ~= "" then drawText(x1 - 6 * s, ty, r, tc, "right", a, sh) end
    if c and c ~= "" then drawText((x0 + x1) / 2, ty, c, tc, "center", a, sh) end
    ui.PopClipRect()
    ui.PopFont()
  end
end

-- ---------------------------------------------------------------- icons
-- An icon tile: a dark rounded well, the icon, a ring in `ring` colour. Returns whether an icon was drawn.
function T.Icon(x0, y0, size, s, tex, u0, v0, u1, v1, ring, opts)
  opts = opts or NO_OPTS
  local x1, y1 = x0 + size, y0 + size
  local rad = (opts.radius or 5) * s
  local a = opts.alpha or 1
  ui.DrawRectFilled(x0, y0, x1, y1, C.inset[1], C.inset[2], C.inset[3], 0.85 * (opts.wellAlpha or 1), rad)
  if tex then
    local p = 1.5 * s
    ui.DrawImage(tex, x0 + p, y0 + p, x1 - p, y1 - p, u0, v0, u1, v1, 1, 1, 1, a)
  end
  if ring then
    ui.DrawRect(x0, y0, x1, y1, ring[1], ring[2], ring[3], (ring[4] or 1) * a, rad, 1.5 * s)
  else
    ui.DrawRect(x0, y0, x1, y1, C.ink[1], C.ink[2], C.ink[3], 0.10, rad, 1)
  end
  return tex ~= nil
end

-- Hover and press feedback over a tile or button rectangle (after its hit box).
function T.HoverFx(x0, y0, x1, y1, s, radius)
  local rad = (radius or 5) * s
  if ui.IsItemActive() then
    ui.DrawRectFilled(x0, y0, x1, y1, 0, 0, 0, 0.25, rad)
  elseif ui.IsItemHovered() then
    ui.DrawRectFilled(x0, y0, x1, y1, C.ink[1], C.ink[2], C.ink[3], 0.08, rad)
    ui.DrawRect(x0, y0, x1, y1, C.accent[1], C.accent[2], C.accent[3], 0.9, rad, 1.5 * s)
  end
end

-- A small rounded button with a label; returns clicked. opts: enabled (default true), primary, danger (the danger
-- colour, for what cannot be undone), width.
function T.Button(id, x0, y0, x1, y1, s, label, opts)
  opts = opts or NO_OPTS
  local enabled = opts.enabled ~= false
  local clicked = T.Hit(id, x0, y0, x1, y1)
  local hovered, active = ui.IsItemHovered(), ui.IsItemActive()
  local rad = 5 * s
  local base = (opts.danger and C.danger) or (opts.primary and C.accent) or C.ink
  local fill = (opts.primary or opts.danger) and 0.22 or 0.06
  if enabled and active then fill = fill + 0.10 elseif enabled and hovered then fill = fill + 0.06 end
  if not enabled then fill = 0.03 end
  ui.DrawRectFilled(x0, y0, x1, y1, base[1], base[2], base[3], fill, rad)
  ui.DrawRect(x0, y0, x1, y1, base[1], base[2], base[3], enabled and (hovered and 0.55 or 0.22) or 0.08, rad, 1)
  ui.PushFont(T.font.small) -- (one push for the line's height and the label)
  local _, th = ui.CalcTextSize("Ag")
  if label and label ~= "" then
    drawText((x0 + x1) / 2, y0 + (y1 - y0 - th) / 2, label, enabled and C.text or C.faint, "center")
  end
  ui.PopFont()
  if opts.tooltip and hovered then ui.SetTooltip(opts.tooltip) end
  return clicked and enabled
end

-- ---------------------------------------------------------------- formats
function T.Duration(ms)
  if not ms or ms < 0 or ms >= 100 * 3600000 then return "" end -- permanent, or long enough to be (an aura)
  local sec = math.floor(ms / 1000)
  if sec >= 3600 then return string.format("%dh", sec // 3600) end
  if sec >= 60 then return string.format("%dm", sec // 60) end
  return sec .. "s"
end

function T.LongDuration(ms)
  if not ms or ms < 0 then return "Permanent" end
  local sec = math.floor(ms / 1000)
  if sec >= 3600 then return string.format("%dh %02dm", sec // 3600, (sec % 3600) // 60) end
  if sec >= 60 then return string.format("%dm %02ds", sec // 60, sec % 60) end
  return sec .. "s"
end

-- 1234567 -> "1,234,567"
function T.Thousands(n)
  local s = tostring(math.floor(tonumber(n) or 0))
  local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
  return (out:gsub("^,", ""):gsub("^%-,", "-"))
end

-- ---------------------------------------------------------------- mouse
-- The right button on a slot, as the game takes it (__ProcessMouseEvents): "rightdown"; after 500 ms "rightheld"
-- (the item or spell display opens); on release "rightupheld" (it closes) or, for a quick click, "rightup".
-- outside: released off the slot. (The same phases classic's lib/sidl.lua delivers.)
function T.RightPressFrame(key, onClick, hovered)
  local press = mouse.rightPress
  if not press then return end
  if press.key ~= key then
    if ui.GetTime() - press.seen > 0.25 then -- its slot is no longer drawn
      mouse.rightPress = nil
      press.onClick(press.held and "rightupheld" or "rightup", true)
    end
    return
  end
  press.onClick, press.seen = onClick, ui.GetTime()
  if ui.IsMouseDown(ui.Mouse.Right) then
    if not press.held and ui.GetTime() - press.start > 0.5 then
      press.held = true
      onClick("rightheld")
    end
  else
    mouse.rightPress = nil
    onClick(press.held and "rightupheld" or "rightup", not hovered)
  end
end

function T.RightPressStart(key, onClick)
  mouse.rightPress = { key = key, onClick = onClick, start = ui.GetTime(), seen = ui.GetTime(), held = false }
  onClick("rightdown")
end

-- Dragging the item just drawn puts the hotbutton it makes on the cursor (hs.PickUp), once per press; that press's
-- click is then not a click.
function T.DragFrom(window, id, row)
  if not ui.IsMouseDown(ui.Mouse.Left) and not ui.IsMouseReleased(ui.Mouse.Left) then mouse.pickedUp = false end
  if window and id and not mouse.pickedUp and ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left)
      and not API.cursor.HoldingButton() then
    mouse.pickedUp = API.cursor.PickUp(window, id, row)
  end
  return mouse.pickedUp
end

return T
