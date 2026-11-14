-- Grid snapping for the modern UI's windows (lib/hud.lua: frames and dialogs alike), a setting of this UI's own
-- (ui-modern.txt: hud.snap, hud.snap.size): a dragged window lands with its left or right edge, and its top or
-- bottom edge, on the grid line nearest where the mouse put it, or flush with the screen's edge when that is nearer.
-- Shift held while dragging places it freely. The grid's size is in design pixels (times the UI scale on screen).
--
--   local snap = dofile("lib/snap.lua")
--   local g = snap.Grid()                     -- the grid in screen pixels, or nil (off, or Shift held)
--   x = snap.Axis(x, w, W, g)                 -- one axis: position, the window's size, the screen's
--   w = snap.Extent(x, w, dx, minW, s, W, g) -- a resize: the far edge from x moved dx screen pixels, the screen's
--
-- Axis and Extent are pure (tests/lua_scripts_test.cpp runs them without the ui table).
local snap = {}

snap.DEFAULT_SIZE, snap.MIN_SIZE, snap.MAX_SIZE = 10, 4, 64

local function nearest(v, g) return math.floor(v / g + 0.5) * g end

-- The snapped position on one axis: whichever of the nearest grid line for the near edge, the nearest one for the
-- far edge, and the screen's two edges is closest to raw (the first of them on a tie), in whole pixels.
function snap.Axis(raw, size, screen, grid)
  if not grid or grid <= 0 then return raw end
  local best, bestD = raw, math.huge
  for _, c in ipairs({ nearest(raw, grid), nearest(raw + size, grid) - size, 0, screen - size }) do
    local d = math.abs(c - raw)
    if d < bestD then best, bestD = c, d end
  end
  return math.floor(best + 0.5)
end

-- The resized size on one axis (design pixels) when a window's far edge is dragged: the window starts at origin
-- (screen pixels) and is size long at scale, and the drag has moved the edge delta screen pixels. With a grid, the
-- far edge goes to the grid line nearest where the mouse put it, or flush with the screen's far edge when that is
-- nearer (the grid line on a tie), as Axis places a moved window; never below minSize. The origin need not be on a
-- grid line.
function snap.Extent(origin, size, delta, minSize, scale, screen, grid)
  local extent = math.max(minSize, size + delta / scale)
  if not grid or grid <= 0 then return extent end
  local raw = origin + extent * scale
  local edge = nearest(raw, grid)
  if screen and math.abs(screen - raw) < math.abs(edge - raw) then edge = screen end
  return math.max(minSize, (edge - origin) / scale)
end

function snap.On() return ui.GetSetting("hud.snap") == "1" end

-- The grid's size in design pixels.
function snap.Size()
  local v = tonumber(ui.GetSetting("hud.snap.size") or "") or snap.DEFAULT_SIZE
  return math.max(snap.MIN_SIZE, math.min(snap.MAX_SIZE, math.floor(v)))
end

-- The grid in screen pixels while snapping applies to a drag, else nil.
function snap.Grid()
  if not snap.On() then return nil end
  local io = ui.GetIO()
  if io.shift then return nil end
  return snap.Size() * (io.uiScale or 1)
end

-- The grid's lines over the whole screen, faint, above every window: drawn once a frame while a window is dragged.
-- (modern is the table every window script's copy of lib/hud.lua shares.)
function snap.DrawGrid(grid, color)
  local frame = ui.GetFrameCount()
  if modern and modern.gridFrame == frame then return end
  if modern then modern.gridFrame = frame end
  local io = ui.GetIO()
  ui.Foreground(true)
  for x = grid, io.width - 1, grid do ui.DrawLine(x, 0, x, io.height, color[1], color[2], color[3], 0.12, 1) end
  for y = grid, io.height - 1, grid do ui.DrawLine(0, y, io.width, y, color[1], color[2], color[3], 0.12, 1) end
  ui.Foreground(false)
end

-- The right-click menus' rows: the switch, and the grid's size while it is on.
function snap.MenuItems()
  local on = snap.On()
  if ui.MenuItem("Snap to grid", on) then ui.SetSetting("hud.snap", on and "0" or "1") end
  if ui.IsItemHovered() then ui.SetTooltip("Dragged windows line up on a grid; hold Shift to place one freely") end
  if on then
    ui.SetNextItemWidth(180)
    local v, changed = ui.SliderInt("Grid size##snap", snap.Size(), snap.MIN_SIZE, snap.MAX_SIZE)
    if changed then ui.SetSetting("hud.snap.size", tostring(v)) end
  end
end

return snap
