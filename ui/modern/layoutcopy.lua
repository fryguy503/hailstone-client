-- Copy a layout: replaces the game's LayoutCopyWindow (/copylayout with no arguments). The layouts that can be
-- copied over the current one (another character's, or the default; File and Mode), what to copy with the windows
-- (Hot Buttons, Loadouts, Socials) and Copy, which asks first. The list, the check boxes and Copy are the
-- original's (app/lua_ui_layout.cpp), so the copy is the game's. Hailstone adds Chat and Options (this client keeps
-- them per character) and opens the window at a character's first entry into the world, with a line saying why and
-- a box to stop offering it.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "LayoutCopyWindow"

local WINDOW_OPTS = {
  size = function() return 460, 400 end,
  resizable = { 320, 280 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local _, lh = ui.CalcTextSize("Ag")
  local first = API.windows.ChildShown(WINDOW, "LC_FirstVisitText") == true
  if first then
    ui.TextWrapped(W.Text(WINDOW, "LC_FirstVisitText"))
    ui.Dummy(0, 4 * s)
  end
  K.Caption("Source layouts")
  local lx, ly = ui.GetCursorScreenPos()
  local below = bh + lh * 4 + 40 * s + (first and lh + 8 * s or 0)
  local rows = W.List(WINDOW, "LC_LayoutsList", 0, -below)
  if rows == 0 then T.Text(lx + 8 * s, ly + lh + 10 * s, "No other layouts found", C.faint, T.font.small) end
  ui.Dummy(0, 2 * s)
  K.Caption("Also copy")
  W.Check(WINDOW, "LC_CopyHotButtonCheckBox", "Hot Buttons")
  ui.SameLine(0, 16 * s)
  W.Check(WINDOW, "LC_CopyHotButtonLoadoutsCheckBox", "Loadouts")
  ui.SameLine(0, 16 * s)
  W.Check(WINDOW, "LC_CopySocialsCheckBox", "Socials")
  W.Check(WINDOW, "LC_CopyChatCheckBox", "Chat Filters and Colors",
    { tooltip = "Hailstone: the chat filters and your own chat colors" })
  ui.SameLine(0, 16 * s)
  W.Check(WINDOW, "LC_CopyOptionsCheckBox", "Game Options",
    { tooltip = "Hailstone: the Options window's game settings (not Anonymous or Roleplaying)" })
  if first then
    W.Check(WINDOW, "LC_OfferCheckBox", "Offer this for new characters",
      { tooltip = "Hailstone: also in Options, General" })
  end
  W.ButtonRow(WINDOW, {
    { "LC_CopyButton", "Copy", { primary = true, tooltip = "Copy the selected layout over this character's" } },
  }, bh)
end

ui.window("Copy Layout", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Copy Layout", WINDOW_OPTS, drawWindow)
end)
