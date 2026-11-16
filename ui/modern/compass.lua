-- Compass: replaces the game's CompassWindow with a heading band across the top of the screen: the four points,
-- ticks every 15 degrees and the heading in degrees under the centre mark. The expedition and adventure marks
-- (hs.CompassMarkers) are shown where the stock compass would put them.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color

local PX_PER_DEG = 1.4 -- on the band (the stock strip is 0.5)
local STOCK_CENTRE = 49  -- the stock compass's centre column, where hs.CompassMarkers' x are measured from

local NAMES = { [0] = "N", [45] = "NW", [90] = "W", [135] = "SW", [180] = "S", [225] = "SE", [270] = "E", [315] = "NE" }
local NO_MARKERS = {}

local OPTS = {
  size = function() return 300, 30 end,
  panelOpts = { radius = 15 },
}

local function body(x, y, w, h, s)
  local _, _, _, heading = API.compass.Strips()
  heading = heading or 0
  local cx = x + w / 2
  ui.PushClipRect(x + 10 * s, y, x + w - 10 * s, y + h, true)
  -- (the points' heights, measured once a frame for each of the two fonts)
  local _, thNormal = T.TextSize("N", T.font.normal)
  local _, thSmall = T.TextSize("N", T.font.small)
  for deg = 0, 345, 15 do
    local d = (deg - heading + 540) % 360 - 180 -- degrees to the left of the heading
    local px = cx - d * PX_PER_DEG * s
    local fade = math.max(0, 1 - math.abs(d) / 110)
    if NAMES[deg] then
      local cardinal = deg % 90 == 0
      local col = deg == 0 and C.danger or (cardinal and C.text or C.muted)
      local font = cardinal and T.font.normal or T.font.small
      local th = cardinal and thNormal or thSmall
      T.Text(px, y + (h - th) / 2, NAMES[deg], col, font, "center", fade)
    else
      ui.DrawLine(px, y + h * 0.36, px, y + h * 0.64, C.ink[1], C.ink[2], C.ink[3], 0.35 * fade, 1)
    end
  end
  -- the stock compass's marks, moved onto the band (they are pixels at 0.5 per degree from its centre)
  for _, mx in ipairs(API.compass.Markers() or NO_MARKERS) do
    local d = (STOCK_CENTRE - mx) / 0.5
    local px = cx - d * PX_PER_DEG * s
    ui.DrawTriangleFilled(px - 5 * s, y + 3 * s, px + 5 * s, y + 3 * s, px, y + 10 * s, C.pet[1], C.pet[2], C.pet[3], 1)
  end
  ui.PopClipRect()
  -- the centre mark
  ui.DrawTriangleFilled(cx - 5 * s, y + h, cx + 5 * s, y + h, cx, y + h - 6 * s, C.accent[1], C.accent[2], C.accent[3], 1)
  -- (no hit box: the whole band is the frame's drag handle)
  if ui.IsMouseHoveringRect(x, y, x + w, y + h) and ui.IsWindowHovered() then ui.SetTooltip(string.format("Heading %d\xC2\xB0", math.floor((360 - heading) % 360 + 0.5) % 360)) end
end

ui.window("Compass", function()
  if not API.session.InGame() then return end
  hud.Window("CompassWindow", "Compass", OPTS, body)
end)
