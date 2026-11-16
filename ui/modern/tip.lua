-- Tips: replaces the game's TipWindow (the context tips the game shows the first time a window or event comes up).
-- The tip's title, its page (STML) in a readable box that scrolls, the earlier tips in a drop-down (the window's
-- History combo), the two "don't show" check boxes (the window writes them to the UI ini's [Tips]) and Close.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "TipWindow"

local WINDOW_OPTS = {
  size = function() return 460, 360 end,
  resizable = { 300, 220 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  -- the earlier tips
  local choices = API.windows.ComboInfo(WINDOW, "History")
  if W.Shown(WINDOW, "History") and choices and #choices > 1 then
    K.Caption("Earlier tips")
    ui.SameLine()
    W.Combo(WINDOW, "History")
    if ui.IsItemHovered() then ui.SetTooltip("Show a tip seen before") end
  end
  local _, lh = ui.CalcTextSize("Ag")
  local checks = lh + 8 * s
  K.TextBox(WINDOW, "Display", 0, -(K.FooterH(s) + checks + 4 * s), { font = T.font.normal })
  W.Check(WINDOW, "NoShowThisButton", "Don't show this tip again")
  ui.SameLine(0, 18 * s)
  W.Check(WINDOW, "NoShowButton", "Don't show any tips", { tooltip = "Turn every context tip off" })
  W.ButtonRow(WINDOW, {
    { "NextTipButton", "Show Another Tip" },
    { "CloseButton", "Close", { primary = true } },
  }, bh)
end

ui.window("Tip of the Day", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Tip", WINDOW_OPTS, drawWindow)
end)
