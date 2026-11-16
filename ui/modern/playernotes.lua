-- Notes: replaces the game's PlayerNotesWindow (/note with no text opens it; /note <text> adds a dated line). The
-- notes fill the window as one text the player edits; they are kept in notes.txt when the window closes.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local WINDOW = "PlayerNotesWindow"

local WINDOW_OPTS = {
  size = function() return 384, 354 end,
  resizable = { 240, 160 },
}
local function drawWindow(x, y, w, h, s)
  local _, ah = ui.GetContentRegionAvail()
  W.Edit(WINDOW, "PlayerNotesInput", { multiline = true, width = -1, height = math.max(60 * s, ah), max = 10000 })
end

ui.window("Notes Window", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Notes", WINDOW_OPTS, drawWindow)
end)
