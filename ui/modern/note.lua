-- Note: replaces the game's NoteWindow (a note or scroll read from the inventory). The note's text (STML) on a warm
-- paper-toned page that scrolls, and the window's buttons: Cast Spell and Scribe when the note teaches a spell (they
-- show only then, as on the stock window), and Done.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "NoteWindow"

local PAPER = { 0.115, 0.100, 0.080, 0.92 }
local INK = { 0.95, 0.90, 0.80, 1 }

local WINDOW_OPTS = {
  size = function() return 400, 380 end,
  resizable = { 260, 200 },
}
local function drawWindow(x, y, w, h, s)
  K.TextBox(WINDOW, "Note", 0, -K.FooterH(s), { font = T.font.normal, bg = PAPER, color = INK })
  W.ButtonRow(WINDOW, {
    { "CastSpellButton", "Cast Spell", { tooltip = "Cast the spell on this note" } },
    { "ScribeButton", "Scribe", { tooltip = "Scribe the spell into your spell book" } },
    { "DoneButton", "Done", { primary = true } },
  }, K.ButtonH(s))
end

ui.window("A Note", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Note", WINDOW_OPTS, drawWindow)
end)
