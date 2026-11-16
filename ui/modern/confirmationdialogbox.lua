-- Confirmation: replaces the game's ConfirmationDialogBox (the yes / no questions: dropping an item on the ground, a
-- guild invitation, the server's and the windows' own questions). The question in a readable box with a question
-- mark beside it, and the buttons the question has: Yes (the primary) and No, or OK alone. The answers are the
-- window's buttons (hs.ClickChild); the close box answers as No does, as the stock close box.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "ConfirmationDialogBox"

local WINDOW_OPTS = {
  rollup = false, -- (a question to answer: kept whole)
  size = function() return 380, 160 end,
  resizable = { 260, 120 },
  place = K.Centered,
}
local function drawWindow(x, y, w, h, s)
  local footer = K.FooterH(s)
  -- the question mark, then the question beside it
  local r = 13 * s
  local cx, cy = ui.GetCursorScreenPos()
  ui.DrawCircleFilled(cx + r, cy + r, r, C.accent[1], C.accent[2], C.accent[3], 0.18)
  ui.DrawCircle(cx + r, cy + r, r, C.accent[1], C.accent[2], C.accent[3], 0.70, 1.5)
  T.Text(cx + r, cy + r - 10 * s, "?", C.accent, T.font.large, "center")
  ui.Dummy(2 * r, 2 * r)
  ui.SameLine(0, 10 * s)
  K.TextBox(WINDOW, "TextOutput", 0, -footer, { font = T.font.normal, bg = { 0, 0, 0, 0 }, border = false })
  W.ButtonRow(WINDOW, {
    { "Yes_Button", "Yes", { primary = true } },
    { "No_Button", "No" },
    { "OK_Button", "OK", { primary = true } },
    { "Cancel_Button", "Cancel" },
  }, K.ButtonH(s))
end

ui.window("Confirmation Dialog Box", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Confirm", WINDOW_OPTS, drawWindow)
end)
