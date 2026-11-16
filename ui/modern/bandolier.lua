-- Bandolier: replaces the game's BandolierWnd. The selected weapon set's four weapons on top (primary, secondary,
-- range, ammo: a green ring when you carry the item, red when you do not, as the game tints them), the sets below
-- as a list, then the game's buttons: Activate, Save (stores what you hold as the selected set, asking for a name),
-- Delete, Auto Swap (a click on a set swaps to it) and Hot Button. Selecting a row selects it in the game's list;
-- a double click activates it. The inventory's Bandolier page draws the same (lib/bandolier.lua).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local band = dofile("lib/bandolier.lua")

local WINDOW_OPTS = {
  size = function() return 300, 330 end,
  resizable = { 230, 260 },
}
local function drawWindow(x, y, w, h, s)
  band.Heading(s)
  band.Weapons(s)
  ui.Separator()
  band.List(0, -(band.ButtonsHeight(s) + 8 * s))
  band.Buttons(s)
end

ui.window("Bandolier", function()
  if not API.session.InGame() then return end
  hud.Dialog(band.WINDOW, "Bandolier", WINDOW_OPTS, drawWindow)
end)
