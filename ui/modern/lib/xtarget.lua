-- Extended targets (extendedtarget.lua) as a library, so a plugin's or script's window can be the pack's with its own
-- part on each row (a window named "Extended Target" covers the pack's: dofile("lib/xtarget.lua") finds this one).
-- xtarget.window(opts) draws the whole window; opts (all optional):
--   width = the window's (unscaled; 210)
--   row = {
--     width = n: unscaled pixels kept at the right of each filled row for the plugin's part (the name, the numbers
--       and the bars stop short of it, and so does the row's click);
--     draw = function(r, x0, y0, x1, y1, s): draws that part (its own hit boxes take its clicks);
--     style = function(r): nil, or { alpha = n (the row drawn faded), ring = {r, g, b, a} (its outline),
--       strike = true (a line through the name), tag = { text, {r, g, b, a} } (a small label after the name) },
--   }
-- r, a filled or empty slot (draw and style are called for filled ones only): n (0-based slot; hailstone.xtarget(n + 1) in a script), id (the
-- game's gauge child), name, filled, hp (0..1), con ({r, g, b, a} or nil), role (nil for an auto hater), aggro,
-- mana, endurance.
-- The window: replaces the game's ExtendedTargetWnd with a compact list at the right, shown while the game
-- shows the window: a solid panel with a title strip (drag it to move the frame) with the
-- filled and the available slots (the server's count: 5 + the Extended Target AA's, less the slots the server
-- module hides), one row per slot, an empty one too (faint, "(empty)" or the name the slot keeps), a filled one's name
-- in its con colour over its health, and the slot's role when it is not an auto hater (the game's role button tool
-- tip). A click targets it; a right click opens the game's menu for that slot (its type, Current Target, auto-add
-- haters, mana / endurance bars, target sets, and Hailstone's Auto expand), so an empty slot can be given a type. The
-- frame is as tall as its first six rows until the player drags its right or bottom edge; the rows past its height
-- scroll inside it. With Auto expand on (hud.expand.ExtendedTargetWnd: that menu's row, or the frame's own menu) it
-- grows past that height to show every row down to the last filled one, and shrinks back as they empty, never taller
-- than the screen.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "ExtendedTargetWnd"
local MAX_SLOTS = 20
local EXPAND = "hud.expand." .. WINDOW
local DEFAULT_ROWS = 6 -- the rows a frame without a size of its own is tall enough for

local M = {}

-- The children's names and the rows' ids, made once (slot n is entry n + 1).
local IDS = {}
for n = 0, MAX_SLOTS - 1 do
  IDS[n + 1] = { gauge = "ETW_Gauge" .. n, hpLabel = "ETW_HPLabel" .. n, mana = "ETW_ManaGauge" .. n,
    stamina = "ETW_STAGauge" .. n, aggro = "ETW_AggroPct" .. n, role = "ETW_Role" .. n, hit = "##ETW_Gauge" .. n,
    hp = "xt.hp" .. n, manaBar = "xt.mana" .. n, endBar = "xt.end" .. n }
end

-- This frame's window, worked out in size() (hud.Window asks for it only while the window is drawn): the rows (the
-- first nRows of rows; each slot's row table is kept from frame to frame), the slots the game shows, how many hold
-- a spawn, the target's name, and the window's height for its first DEFAULT_ROWS rows and down to its last filled row.
local opts, extra = nil, nil
local rows, nRows, slots, filledCount, target, base, full = {}, 0, 0, 0, "", 0, 0
local rowOf, conOf = {}, {} -- slot -> its row table, its con colour table
local NO_OPTS = {}
local ROW, HEAD, PAD = 30, 24, 6
-- Under the rows: room for hud.Window's corner grip (14 pixels a side, at least 12 on screen), with 2 to spare, so the
-- scrolling area (a child window, which takes the mouse wherever it lies) leaves the bottom edge and the grip free.
local FOOT = 16

local function rowH(r) return ROW + ((r.mana or r.endurance) and 5 or 0) end

local function gather()
  nRows, slots = 0, 0
  for n = 0, MAX_SLOTS - 1 do
    local ids = IDS[n + 1]
    local id = ids.gauge
    -- a row the game does not show: past the server's count, or a slot a server's plugin leaves out (NMS: your own
    -- pets, which the server puts in the last slots; Options "Hide My Pets in Extended Target")
    local shown = API.windows.ChildShown(WINDOW, id) ~= false
    if shown then slots = slots + 1 end
    local frac, name, raw = API.character.Gauge(42 + n)
    local filled = shown and API.windows.ChildShown(WINDOW, ids.hpLabel) == true
    if shown then -- every slot the game shows, an empty one too: its row is where its right click picks it
      local r, g, b = API.windows.ChildColor(WINDOW, id)
      local row = rowOf[n]
      if not row then
        row = {}
        rowOf[n] = row
      end
      local con = nil
      if r then
        con = conOf[n]
        if not con then
          con = {}
          conOf[n] = con
        end
        con[1], con[2], con[3], con[4] = r, g, b, 1
      end
      -- an empty slot: the name the slot keeps (a named player not in the zone), else "(empty)"
      if not filled then name = (name and name ~= "") and name or "(empty)" end
      row.n, row.id, row.hp, row.name, row.filled, row.raw, row.con = n, id, frac, name, filled, raw, con
      row.mana, row.endurance, row.aggro, row.role = nil, nil, nil, nil
      if API.windows.ChildShown(WINDOW, ids.mana) == true then row.mana = API.character.Gauge(62 + n) end
      if API.windows.ChildShown(WINDOW, ids.stamina) == true then row.endurance = API.character.Gauge(82 + n) end
      if API.windows.ChildShown(WINDOW, ids.aggro) == true then row.aggro = API.windows.Text(WINDOW, ids.aggro) end
      local role = API.windows.ChildShown(WINDOW, ids.role) ~= false and API.windows.ChildTooltip(WINDOW, ids.role) or ""
      if role ~= "" and role ~= "Auto Hater" then row.role = role end
      nRows = nRows + 1
      rows[nRows] = row
    end
  end
  target = API.character.Label(28) or ""
  base = HEAD + 4 + FOOT
  full = base
  filledCount = 0
  local h = base
  for i = 1, nRows do
    local r = rows[i]
    h = h + rowH(r) + 3
    if i <= DEFAULT_ROWS then base = h end
    if r.filled then
      filledCount = filledCount + 1
      full = h
    end
  end
  if nRows == 0 then base = base + 24 end
end

local function size()
  gather()
  return opts.width or 210, base
end

-- Auto expand: tall enough for the rows down to the last filled one, within the screen (its corner grip stays on it).
local function grow(dw, dh, s)
  if ui.GetSetting(EXPAND) ~= "1" then return dw, dh end
  return dw, math.max(dh, math.min(full, ui.GetIO().height / s))
end

local function menu()
  local on = ui.GetSetting(EXPAND) == "1"
  if ui.MenuItem("Auto expand", on) then ui.SetSetting(EXPAND, on and "0" or "1") end
  if ui.IsItemHovered() then
    ui.SetTooltip("Grow the frame to show every slot down to the last one with a target, and shrink it back as they clear")
  end
end

local ring = {}    -- (a row's outline colour, filled for each row)
local barOpts = {} -- (T.Bar's options, filled for each bar: T.Bar keeps nothing of them)

local function body(x0, y, w0, h, s)
  -- the title strip: empty, so a drag there moves the frame
  local _, hh = T.TextSize("Ag", T.font.small)
  T.Text(x0 + 10 * s, y + (HEAD * s - hh) / 2, "Extended Target", C.muted, T.font.small)
  if slots > 0 then T.Text(x0 + w0 - 10 * s, y + (HEAD * s - hh) / 2, filledCount .. " / " .. slots, C.faint, T.font.small, "right") end
  ui.DrawLine(x0 + 8 * s, y + HEAD * s, x0 + w0 - 8 * s, y + HEAD * s, C.ink[1], C.ink[2], C.ink[3], 0.07, 1)
  local top = y + HEAD * s + 4 * s
  if nRows == 0 then
    T.Text(x0 + w0 / 2, top + 4 * s, "No extended targets", C.faint, T.font.small, "center")
    return
  end
  -- The rows scroll under the title strip when the player sizes the frame shorter than they are (the wheel, or the
  -- scroll bar). The area stays off the frame's right and bottom edges and its corner grip (hud.Window's resize
  -- handles: 6 pixels at least 5, the grip 14 at least 12), which a child window over them would hide from the
  -- mouse. Sized by size() it is a little taller than the rows, so no scroll bar shows for rounding.
  local side = math.max(PAD * s, 7)
  local listH = y + h - top - math.max(14 * s, 12)
  if listH < 1 then return end
  ui.SetCursorScreenPos(x0 + side, top)
  ui.BeginChild("##xtrows", w0 - 2 * side, listH, false)
  local x, cy = ui.GetCursorScreenPos() -- (the area's top left, scrolled)
  local w = ui.GetContentRegionAvail() -- (less the scroll bar while it shows)
  local _, th = T.TextSize("Ag", T.font.small)
  for i = 1, nRows do
    local r = rows[i]
    local ids = IDS[r.n + 1]
    local rh = rowH(r) * s
    local look = extra and extra.style and r.filled and extra.style(r) or nil
    local a = look and look.alpha or 1
    local keep = (extra and extra.draw and r.filled and (extra.width or 0) or 0) * s
    local clicked = T.Hit(ids.hit, x, cy, x + w - keep, cy + rh)
    if hud.DragItem() then clicked = false end -- a drag moves the frame
    local hovered = ui.IsItemHovered()
    local isTarget = r.filled and r.name == target and target ~= ""
    ui.DrawRectFilled(x, cy, x + w, cy + rh, C.ink[1], C.ink[2], C.ink[3], (hovered and 0.07 or 0.035) * (r.filled and 1 or 0.6), 6 * s)
    local rc = look and look.ring
    if not rc and isTarget then
      ring[1], ring[2], ring[3], ring[4] = C.ink[1], C.ink[2], C.ink[3], 0.7
      rc = ring
    elseif not rc and hovered then
      ring[1], ring[2], ring[3], ring[4] = C.accent[1], C.accent[2], C.accent[3], 0.6
      rc = ring
    end
    if rc then ui.DrawRect(x, cy, x + w, cy + rh, rc[1], rc[2], rc[3], rc[4], 6 * s, 1.5 * s) end
    local pad = 7 * s
    local pct = r.filled and (math.floor((r.hp or 0) * 100 + 0.5) .. "%") or ""
    local wr = w - keep -- the stock part of the row
    local tag = look and look.tag
    local tagW = tag and (T.TextSize(tag[1], T.font.tiny) + 14 * s) or 0
    local name = T.Fit(r.name or "", wr - (r.role and 120 or 50) * s - tagW, T.font.small)
    local nameColor = r.filled and (r.con or C.text) or C.faint
    T.Text(x + pad, cy + 3 * s, name, nameColor, T.font.small, nil, a)
    local nw = T.TextSize(name, T.font.small)
    if look and look.strike then
      ui.DrawLine(x + pad, cy + 3 * s + th / 2, x + pad + nw, cy + 3 * s + th / 2, nameColor[1], nameColor[2],
        nameColor[3], 0.9 * a, 1.2 * s)
    end
    if tag then
      local _, tt = T.TextSize("Ag", T.font.tiny)
      local tc = tag[2] or C.accent
      local tx = x + pad + nw + 5 * s
      local ty = cy + 3 * s + (th - tt) / 2 - 1 * s
      ui.DrawRectFilled(tx, ty, tx + tagW - 5 * s, ty + tt + 2 * s, tc[1], tc[2], tc[3], 0.18, (tt + 2 * s) / 2)
      T.Text(tx + 4.5 * s, ty + 1 * s, tag[1], tc, T.font.tiny)
    end
    local right = r.aggro and r.aggro ~= "" and (r.aggro .. "  " .. pct) or pct
    if r.role then right = r.role .. "  " .. right end
    T.Text(x + wr - pad, cy + 3 * s, right, C.muted, T.font.small, "right", a)
    local by = cy + 3 * s + th + 3 * s
    barOpts.alpha = a
    T.Bar(ids.hp, x + pad, by, x + wr - pad, by + 6 * s, s, r.filled and r.hp or nil, C.hp, barOpts)
    if r.mana or r.endurance then
      local half = (wr - 2 * pad - 4 * s) / 2
      by = by + 8 * s
      if r.mana then T.Bar(ids.manaBar, x + pad, by, x + pad + half, by + 3 * s, s, r.mana, C.mana, barOpts) end
      if r.endurance then
        T.Bar(ids.endBar, x + wr - pad - half, by, x + wr - pad, by + 3 * s, s, r.endurance, C.endurance, barOpts)
      end
    end
    if keep > 0 then extra.draw(r, x + wr, cy, x + w, cy + rh, s) end
    if clicked and r.filled then API.windows.ClickChild(WINDOW, r.id, "left") end
    if hovered and ui.IsMouseClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      API.windows.ClickChild(WINDOW, ids.role, "rightup") -- this slot's menu
      T.TakeRightClick()
    end
    cy = cy + rh + 3 * s
  end
  ui.EndChild()
end

-- Six rows tall until the player drags an edge; then that size is kept. The rows past it scroll, unless Auto expand
-- grows the frame to them.
local HUD_OPTS = { size = size, grow = grow, menu = menu, panelOpts = { opaque = true }, resizable = { 160, 70 } }

function M.window(o)
  opts = o or NO_OPTS
  extra = opts.row
  if not API.session.InGame() then return end
  hud.Window(WINDOW, "Extended Target", HUD_OPTS, body)
end

return M
