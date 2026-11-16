-- The pieces the gamepad pack's pre-game screens share (login.lua, serverselect.lua, charselect.lua): the pad's
-- buttons outside the world, focus aimed from code, the status line, and the rows those screens are built from.
-- The screens are the client's (app/lua_ui_pregame.cpp), not the original's: nothing here follows eqmain.dll.
--
--   local P = dofile("lib/pregame.lua")
--   local f = P.focus()                  -- one per window: f:want(id) aims the pad's focus at an item next frame
--   f:aim(id)                            -- before the item `id`: takes the focus when it was wanted
--   P.press("start")                     -- a pad button for the screen (not while the pad drives the pointer)
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local C = T.C
local P = {}

-- A button that went down this frame for the focused window. Outside the world the pad drives the windows in game
-- mode too (ClientApp::update_gamepad: there is nothing to play), so this does not ask for menu mode as
-- pad.menuPressed does; cursor mode (Back) is the pointer's.
function P.press(b)
  return ui.IsWindowFocused(3) and pad.connected() and pad.mode() ~= "cursor" and pad.pressed(b)
end

-- Whether the pad (not the mouse) moves the focus now: its cursor shows. A click hides it.
function P.padDrives() return pad.navVisible() end

-- ---------------------------------------------------------------- focus from code
-- A focus wanted for an item by id: the item calls aim(id) before it is built. A want lasts a few frames (a page
-- that changes this frame builds its items the next), then lapses, so a stale one never steals the focus later.
local Focus = {}
Focus.__index = Focus
function P.focus() return setmetatable({ id = nil, frames = 0, last = nil }, Focus) end
function Focus:want(id)
  self.id, self.frames = id, 3
end
function Focus:aim(id)
  if self.id == id then
    ui.SetKeyboardFocusHere()
    pad.showCursor()
    self.id = nil
  end
end
-- Once a frame, after the items: lapses an unmet want.
function Focus:tick()
  if self.id then
    self.frames = self.frames - 1
    if self.frames <= 0 then self.id = nil end
  end
end
-- Notes the focused item (track(id, focused) after it), so B, which drops ImGui's focus in a root window, can put
-- it back: f:want(f.last).
function Focus:track(id, focused)
  if focused and pad.navVisible() then self.last = id end
end

-- ---------------------------------------------------------------- status
-- A turning ring of dots (the screen is waiting on the server).
function P.spinner(cx, cy, r)
  local head = (ui.GetTime() * 10) % 8 -- the brightest dot goes round
  for i = 0, 7 do
    local a = i * math.pi / 4
    local behind = (head - i) % 8
    ui.DrawCircleFilled(cx + math.cos(a) * r, cy + math.sin(a) * r, math.max(1.5, r * 0.22),
      C.accent[1], C.accent[2], C.accent[3], 0.2 + 0.8 * (1 - behind / 8))
  end
end

-- Text word-wrapped to `w` pixels, at most `max` lines (the last shortened with "..."). A server's message may carry
-- STML line breaks and tags: breaks become spaces, tags are dropped.
local function wrap(text, w, max)
  text = text:gsub("<[Bb][Rr]%s*/?>", " "):gsub("<[^>]*>", ""):gsub("%s+", " ")
  local lines, line = {}, ""
  for word in text:gmatch("%S+") do
    local try = line == "" and word or (line .. " " .. word)
    if line ~= "" and ui.CalcTextSize(try) > w then
      lines[#lines + 1] = line
      line = word
    else
      line = try
    end
  end
  if line ~= "" then lines[#lines + 1] = line end
  if #lines > max then
    lines[max] = T.fit(table.concat(lines, " ", max), w)
    for i = #lines, max + 1, -1 do lines[i] = nil end
  end
  return lines
end

-- The status from x0 to x1 (its text red when it is an error, a spinner before it while busy), wrapped to at most
-- opts.lines lines (3); its top at y, or its bottom with opts.bottom. Returns its height.
function P.status(status, busy, x0, y, x1, opts)
  opts = opts or {}
  T.font(19)
  local _, th = ui.CalcTextSize("Ag")
  local x = x0
  if busy then x = x + th + T.px(10) end
  local text = status and status.text or ""
  if text == "" and busy then text = "Working..." end
  local lines = text ~= "" and wrap(text, x1 - x, opts.lines or 3) or { "" }
  local h = #lines * th
  if opts.bottom then y = y - h end
  if busy then P.spinner(x0 + th / 2, y + th / 2, th * 0.38) end
  local c = (status and status.error) and C.bad or C.dim
  for i, line in ipairs(lines) do T.draw(x, y + (i - 1) * th, line, c) end
  T.unfont()
  return h
end

-- ---------------------------------------------------------------- rows
-- The rows are buttons (lib/theme.lua T.button) drawn over: a label on the left and its value on the right.
local LABEL_W = 210

-- A field: label, value (placeholder when empty), A or a click opens what it edits. opts: disabled, placeholder,
-- chevron (it opens a list), labelW (design px), note (a dim line under the value). Returns clicked, focused.
function P.field(f, id, label, value, w, h, opts)
  opts = opts or {}
  f:aim(id)
  local clicked, focused, x0, y0, x1, y1 = T.button(id, "", w, h, { disabled = opts.disabled }, pad)
  f:track(id, focused)
  local fa = opts.disabled and 0.45 or 1
  local lw = T.px(opts.labelW or LABEL_W)
  T.font(19)
  T.textIn(x0 + T.px(18), y0, x0 + lw, y1, label, focused and C.accent or C.dim, 0, fa)
  T.unfont()
  local right = x1 - T.px(18)
  if opts.chevron then
    T.triangle("right", right - T.px(6), (y0 + y1) / 2, T.px(7), C.dim, fa)
    right = right - T.px(26)
  end
  local empty = value == nil or value == ""
  local text = empty and (opts.placeholder or "") or value
  if opts.note and opts.note ~= "" then
    local _, th = ui.CalcTextSize("Ag")
    local mid = (y0 + y1) / 2
    T.textIn(x0 + lw, mid - th, right, mid, T.fit(text, right - x0 - lw), empty and C.faint or C.text, 0, fa)
    T.font(17)
    T.textIn(x0 + lw, mid, right, mid + th, T.fit(opts.note, right - x0 - lw), C.dim, 0, fa)
    T.unfont()
  else
    T.textIn(x0 + lw, y0, right, y1, T.fit(text, right - x0 - lw), empty and C.faint or C.text, 0, fa)
  end
  return clicked and not opts.disabled, focused
end

-- A choice stepped in place: "< value >"; A or a click steps forward, the D-pad's left / right while it has the
-- focus steps either way. Returns the step (-1, 1) or nil, and focused. opts as P.field's.
function P.cycle(f, id, label, value, w, h, opts)
  opts = opts or {}
  f:aim(id)
  local clicked, focused, x0, y0, x1, y1 = T.button(id, "", w, h, { disabled = opts.disabled }, pad)
  f:track(id, focused)
  local fa = opts.disabled and 0.45 or 1
  local lw = T.px(opts.labelW or LABEL_W)
  T.font(19)
  T.textIn(x0 + T.px(18), y0, x0 + lw, y1, label, focused and C.accent or C.dim, 0, fa)
  T.unfont()
  local cy = (y0 + y1) / 2
  local arrow = focused and C.accent or C.dim
  T.triangle("left", x0 + lw + T.px(8), cy, T.px(7), arrow, fa)
  T.triangle("right", x1 - T.px(24), cy, T.px(7), arrow, fa)
  local vx0, vx1 = x0 + lw + T.px(26), x1 - T.px(42)
  if opts.note and opts.note ~= "" then
    local _, th = ui.CalcTextSize("Ag")
    T.textIn(vx0, cy - th, vx1, cy, T.fit(value, vx1 - vx0), C.text, 0.5, fa)
    T.font(17)
    T.textIn(vx0, cy, vx1, cy + th, T.fit(opts.note, vx1 - vx0), C.dim, 0.5, fa)
    T.unfont()
  else
    T.textIn(vx0, y0, vx1, y1, T.fit(value, vx1 - vx0), C.text, 0.5, fa)
  end
  if opts.disabled then return nil, focused end
  local step
  if clicked then step = 1 end
  if focused and ui.IsWindowFocused(3) and pad.connected() then
    if pad.pressed("left") then step = -1 elseif pad.pressed("right") then step = 1 end
  end
  return step, focused
end

-- An on / off switch: label on the left, the switch on the right; A or a click flips it. opts: disabled, note (a
-- dim line under the label: why it is off). Returns clicked (only when enabled), focused.
function P.toggle(f, id, label, on, w, h, opts)
  opts = opts or {}
  f:aim(id)
  local clicked, focused, x0, y0, x1, y1 = T.button(id, "", w, h, { disabled = opts.disabled }, pad)
  f:track(id, focused)
  local fa = opts.disabled and 0.45 or 1
  local sw, sh = T.px(56), T.px(30)
  local sx1, sy0 = x1 - T.px(18), (y0 + y1 - sh) / 2
  local sx0 = sx1 - sw
  local textR = sx0 - T.px(12)
  if opts.note and opts.note ~= "" then
    local _, th = ui.CalcTextSize("Ag")
    local mid = (y0 + y1) / 2
    T.textIn(x0 + T.px(18), mid - th, textR, mid, T.fit(label, textR - x0 - T.px(18)), C.text, 0, fa)
    T.font(16)
    local _, nh = ui.CalcTextSize("Ag")
    T.textIn(x0 + T.px(18), mid, textR, mid + nh, T.fit(opts.note, textR - x0 - T.px(18)), C.dim, 0, fa)
    T.unfont()
  else
    T.textIn(x0 + T.px(18), y0, textR, y1, T.fit(label, textR - x0 - T.px(18)), C.text, 0, fa)
  end
  T.rect(sx0, sy0, sx1, sy0 + sh, on and C.accent or C.sunken, (on and 0.95 or 1) * fa, sh / 2)
  T.outline(sx0, sy0, sx1, sy0 + sh, C.edge, fa, sh / 2)
  local kr = sh / 2 - T.px(4)
  local kx = on and (sx1 - sh / 2) or (sx0 + sh / 2)
  local k = on and C.onAccent or C.dim
  ui.DrawCircleFilled(kx, sy0 + sh / 2, kr, k[1], k[2], k[3], fa)
  return clicked and not opts.disabled, focused
end

-- A plain button with the focus helpers (T.button's opts). Returns clicked (only when enabled), focused, its rect.
function P.button(f, id, label, w, h, opts)
  opts = opts or {}
  f:aim(id)
  local clicked, focused, x0, y0, x1, y1 = T.button(id, label, w, h, opts, pad)
  f:track(id, focused)
  return clicked and not opts.disabled, focused, x0, y0, x1, y1
end

-- A small rounded label (a world's status, "Last played"): text in colour c over a tint of it, its right edge at
-- x1 (ax 1) or its left at x0 (ax 0), centred on cy. Returns its width.
function P.pill(x, cy, text, c, ax)
  T.font(16)
  local tw, th = ui.CalcTextSize(text)
  local w, h = tw + T.px(20), th + T.px(8)
  local px0 = ax == 1 and x - w or x
  T.rect(px0, cy - h / 2, px0 + w, cy + h / 2, c, 0.18, h / 2)
  T.outline(px0, cy - h / 2, px0 + w, cy + h / 2, c, 0.55, h / 2)
  T.textIn(px0, cy - h / 2, px0 + w, cy + h / 2, text, c, 0.5)
  T.unfont()
  return w
end

-- The cursor to x, y (screen) in the current window.
function P.at(x, y)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy)
end

-- The cursor's screen y.
function P.y() return select(2, ui.GetCursorScreenPos()) end

return P
