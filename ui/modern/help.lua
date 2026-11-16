-- Help: replaces the game's HelpWindow (the game's help\ pages). A tool row (Back, Contents, the page's title, Reset
-- Tips, Customer Service) over the page, which scrolls; its links are followed by the window (hs.StmlLink), with
-- Back through its history (the window enables it while there is one). The buttons are the original's
-- (app/lua_ui_help.cpp): Reset Tips brings up the tip of the day, Customer Service files a /petition.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "HelpWindow"

local WINDOW_OPTS = {
  size = function() return 620, 480 end,
  resizable = { 360, 260 },
}
local function drawWindow(x, y, w, h, s)
  W.Button(WINDOW, "BackButton", "< Back", { tooltip = "The page before" })
  ui.SameLine()
  W.Button(WINDOW, "Contents", "Contents", { tooltip = "The help's first page" })
  local title = W.Text(WINDOW, "Title")
  local right = ui.CalcTextSize("Reset Tips") + ui.CalcTextSize("Customer Service") + 48 * s
  ui.SameLine(0, 12 * s)
  local cx, cy = ui.GetCursorPos()
  local sx, sy = ui.GetCursorScreenPos()
  local avail = ui.GetContentRegionAvail()
  if title ~= "" and avail - right > 40 * s then -- the page's title, between the two groups of buttons
    T.Text(sx, sy + 5 * s, title, C.heading, T.font.normal)
  end
  ui.SetCursorPos(avail > right and cx + avail - right or cx, cy)
  W.Button(WINDOW, "TipsButton", "Reset Tips", { tooltip = "Bring up the tip of the day" })
  ui.SameLine()
  W.Button(WINDOW, "CustomerServiceButton", "Customer Service", { tooltip = "Ask a guide for help (/petition)" })
  ui.Dummy(0, 2 * s)
  K.TextBox(WINDOW, "Display", 0, 0, { font = T.font.normal, empty = "The help pages are not in the game folder." })
end

ui.window("Help", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Help", WINDOW_OPTS, drawWindow)
end)
