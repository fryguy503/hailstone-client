-- Air meter: replaces the game's BreathWindow (shown by the game while you are short of air: under water). A small
-- borderless frame like the cast bar: "Air" and the percent left over a bar (the game's gauge, EQType 8) that turns
-- from cyan to red as the air runs out, pulsing in the last quarter. Moves by dragging; its scale is the right-click menu's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color

local AIR = { 0.30, 0.85, 0.95, 1 }

local WINDOW_OPTS = {
  size = function() return 240, 38 end,
}
local function drawWindow(x, y, w, h, s)
  local frac = API.character.Gauge(8) or 0
  local pad = 8 * s
  local col = AIR
  if frac < 0.5 then -- toward red as it runs out
    local k = math.max(0, frac / 0.5)
    col = { C.danger[1] + (AIR[1] - C.danger[1]) * k, C.danger[2] + (AIR[2] - C.danger[2]) * k,
      C.danger[3] + (AIR[3] - C.danger[3]) * k, 1 }
  end
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(x + pad, y + 4 * s, "Air", C.muted, T.font.small)
  T.Text(x + w - pad, y + 4 * s, string.format("%d%%", math.floor(frac * 100 + 0.5)), col, T.font.small, "right")
  local by = y + 4 * s + th + 3 * s
  local alpha = 1
  if frac < 0.25 then alpha = 0.65 + 0.35 * math.abs(math.sin(ui.GetTime() * 5)) end
  T.Bar("breath", x + pad, by, x + w - pad, math.min(by + 9 * s, y + h - 5 * s), s, frac, col,
    { noTrail = true, alpha = alpha })
end

ui.window("Breath", function()
  if not API.session.InGame() then return end
  hud.Window("BreathWindow", "Air", WINDOW_OPTS, drawWindow)
end)
