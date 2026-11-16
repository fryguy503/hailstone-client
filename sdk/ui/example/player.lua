-- SPDX-License-Identifier: MIT
-- An original presentation over the public UI contract. Other windows come from base=modern.
local API = assert(type(hs) == "table" and hs.UI, "Example Interface requires Hailstone UI API 1")
assert(API.version and API.version.major == 1, "Example Interface requires Hailstone UI API 1")
local hud = dofile("lib/hud.lua")
local T = hud.T
local OPTIONS = { size = function() return 260, 72 end }

ui.window("Player", function()
  if not API.session.InGame() then return end
  hud.Window("PlayerWindow", "Player", OPTIONS, function(x, y, w, h, s)
    local name = API.character.Name() or "Player"
    T.Text(x + 10 * s, y + 8 * s, name, T.color.text, T.font.normal)
    local fraction, text = API.character.Gauge(1)
    T.Bar("example.hp", x + 10 * s, y + 36 * s, x + w - 10 * s, y + h - 10 * s,
      s, fraction or 0, T.color.hp, { text = text or "" })
  end)
end)
