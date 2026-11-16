-- Minimap: a frame of this UI's own (no stock window), from the gamepad UI's (ui/gamepad/hud.lua). The zone map in a
-- circle around the player (hs.DrawMinimap: the map window's lines, the group, a /find path; what the stock map shows,
-- no other players or NPCs), the zone's name above it and the game clock below. A click opens the map window, the
-- wheel or + / - zoom, a right click turns rotation with the player on or off; Alt+right-click the frame's menu (which also has Auto Z
-- and its ranges above and below you, hud.AutoZControls).
-- Shown or hidden from any frame's menu (All frames > Minimap: hud.minimap).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local FL = dofile("lib/find.lua")
local T = hud.T
local C = T.color

local RANGES = { 150, 300, 600, 1200, 2400 }
local NO_ROWS = {}

local function zoom() return math.max(1, math.min(#RANGES, tonumber(ui.GetSetting("minimap.zoom")) or 3)) end
local function setZoom(step) ui.SetSetting("minimap.zoom", math.max(1, math.min(#RANGES, zoom() + step))) end
local function rotating() return ui.GetSetting("minimap.rotate") == "1" end

local function clock()
  local h, m = API.session.GameTime()
  if not h then return nil end
  local h12 = h % 12
  if h12 == 0 then h12 = 12 end
  return string.format("%d:%02d %s", h12, m, h < 12 and "AM" or "PM")
end

-- A small round button on the ring; true when clicked.
local function ringButton(id, cx, cy, r, label, s)
  local clicked = T.Hit(id, cx - r, cy - r, cx + r, cy + r)
  local hov = ui.IsItemHovered()
  ui.DrawCircleFilled(cx, cy, r, C.panel[1], C.panel[2], C.panel[3], 0.95)
  ui.DrawCircle(cx, cy, r, C.accent[1], C.accent[2], C.accent[3], hov and 0.9 or 0.35, 1.5 * s)
  local _, th = T.TextSize(label, T.font.normal)
  T.Text(cx, cy - th / 2 - 1 * s, label, hov and C.accent or C.text, T.font.normal, "center")
  return clicked
end

local WINDOW_OPTS = {
  shown = function() return ui.GetSetting("hud.minimap") ~= "0" end,
  size = function() return 200, 248 end,
  panel = false,
  menu = function()
    if ui.MenuItem("Rotate with the player", rotating()) then ui.SetSetting("minimap.rotate", rotating() and "0" or "1") end
    ui.Separator()
    hud.AutoZControls(hud.Scale())
    ui.Separator()
    if ui.MenuItem("Hide") then ui.SetSetting("hud.minimap", "0") end
  end,
}
local function drawWindow(x, y, w, h, s)
  local cx = x + w / 2
  local r = 86 * s
  local cy = y + 30 * s + r
  -- the zone's name, on the world (no panel or drop shadow): light text with a dark shadow on every palette
  local zone = API.map.ZoneName() or ""
  if zone ~= "" then
    local name = T.Fit(zone, w - 24 * s, T.font.small)
    T.Text(cx, y + 4 * s, name, C.barText, T.font.small, "center", nil, C.barShadow)
  end
  -- a light backdrop under the map: the chat window's panel (T.Panel at its resting 0.7) as a disc
  local r0, g0, b0, a0 = T.rgba(C.shadow, 0.8 * 0.7)
  ui.DrawCircleFilled(cx + 1 * s, cy + 3 * s, r + 2 * s, r0, g0, b0, a0)
  r0, g0, b0, a0 = T.rgba(C.panel, 0.7)
  ui.DrawCircleFilled(cx, cy, r, r0, g0, b0, a0)
  r0, g0, b0, a0 = T.rgba(C.border, 0.7)
  ui.DrawCircle(cx, cy, r, r0, g0, b0, a0, 1)
  T.Hit("##minimap", cx - r, cy - r, cx + r, cy + r)
  local dragged = hud.DragItem() -- a drag moves the frame
  local mx, my = ui.GetMousePos()
  local hovered = ui.IsItemHovered() and (mx - cx) ^ 2 + (my - cy) ^ 2 <= r * r
  local nx, ny, pmx, pmy, k = API.map.DrawMinimap(cx, cy, r, RANGES[zoom()], rotating())
  local F, all = FL.state, FL.marks()
  -- Retire retained actor state promptly, without asking the host for NPCs when this minimap uses none.
  local rows = (all or F.sel or F.hover or F.menu or F.why) and FL.rows() or NO_ROWS
  -- the Find window's NPCs (lib/find.lua): as the map marks them, inside the ring
  if FL.policy() and pmx and k then
    if all or F.sel then
      local sn, cs = nx, -ny -- north's screen direction is (sin, -cos) of the turn
      for _, row in ipairs(rows) do
        local lit = FL.matches(row, F.sel, F.selToken) or FL.matches(row, F.hover, F.hoverToken)
        if all or lit then
          local dx, dy = (row.x - pmx) * k, (row.y - pmy) * k
          local sx, sy = dx * cs - dy * sn, dx * sn + dy * cs
          if sx * sx + sy * sy <= (r - 4 * s) ^ 2 then FL.drawMark(cx + sx, cy + sy, row, s * 0.85, lit, false) end
        end
      end
    end
  end
  ui.DrawCircle(cx, cy, r, C.accent[1], C.accent[2], C.accent[3], 0.35, 1.5 * s)
  -- north on the ring
  if nx then
    local px, py = cx + nx * r, cy + ny * r
    ui.DrawCircleFilled(px, py, 10 * s, C.panel[1], C.panel[2], C.panel[3], 1)
    ui.DrawCircle(px, py, 10 * s, C.danger[1], C.danger[2], C.danger[3], 0.9, 1.5 * s)
    local _, th = T.TextSize("N", T.font.small)
    T.Text(px, py - th / 2, "N", C.danger, T.font.small, "center")
  end
  if hovered then
    ui.SetTooltip(string.format("%s\nClick: the map   Wheel: zoom (range %d)\nRight-click: %s", zone, RANGES[zoom()],
      rotating() and "north up" or "turn with the player"))
    local wheel = ui.GetIO().wheel or 0
    if wheel ~= 0 then setZoom(wheel > 0 and -1 or 1) end
    if ui.IsMouseReleased(ui.Mouse.Left) and not dragged then API.windows.Show("MapViewWnd", true) end
    if ui.IsMouseClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      ui.SetSetting("minimap.rotate", rotating() and "0" or "1")
      T.TakeRightClick()
    end
  end
  -- zoom on the lower right of the ring, the clock below (on the world, like the zone's name)
  local a1, a2 = math.rad(35), math.rad(58)
  if ringButton("##zoomin", cx + math.cos(a1) * (r + 2 * s), cy + math.sin(a1) * (r + 2 * s), 12 * s, "+", s) then setZoom(-1) end
  if ringButton("##zoomout", cx + math.cos(a2) * (r + 2 * s), cy + math.sin(a2) * (r + 2 * s), 12 * s, "-", s) then setZoom(1) end
  local t = clock()
  if t then
    T.Text(cx, cy + r + 11 * s, t, C.barText, T.font.small, "center", nil, C.barShadow)
  end
end

ui.window("Minimap", function()
  FL.policy()
  if not API.session.InGame() then return end
  hud.Window(nil, "Minimap", WINDOW_OPTS, drawWindow)
end)
