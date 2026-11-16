-- Expedition choice: replaces the game's DynamicZoneSwitchListWnd (a door or switch that leads into more than one
-- of your expeditions). The expeditions as a table (name, leader); a double click or Enter goes in, as OK does.
-- The list, OK and Cancel are the original's (DZSwitchList_*), so going in is the game's; closing the window is
-- Cancel.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "DynamicZoneSwitchListWnd"

local WINDOW_OPTS = {
  size = function() return 420, 280 end,
  resizable = { 300, 200 },
  keepTitle = true,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 60 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  K.Muted("This way leads into more than one expedition. Pick one:", C.muted)
  ui.Dummy(0, 2 * s)
  local rows = API.windows.ListInfo(WINDOW, "DZSwitchList_List") or 0
  if rows == 0 then
    ui.BeginChild("##dzswitchempty", 0, -(btnH + 8 * s), false)
    K.Empty("No expeditions to enter here.")
    ui.EndChild()
  else
    W.List(WINDOW, "DZSwitchList_List", 0, -(btnH + 8 * s), {
      doubleClick = function() API.windows.ClickChild(WINDOW, "DZSwitchList_OK", "left") end,
    })
  end
  if ui.IsKeyPressed and ui.Key and ui.Key.Enter and ui.IsKeyPressed(ui.Key.Enter)
      and ui.IsWindowFocused(ui.HoveredFlags.ChildWindows) and W.State(WINDOW, "DZSwitchList_OK") then
    API.windows.ClickChild(WINDOW, "DZSwitchList_OK", "left")
  end
  W.ButtonRow(WINDOW, {
    { "DZSwitchList_OK", "Enter", { primary = true, tooltip = "Go into the selected expedition" } },
    { "DZSwitchList_Cancel", "Cancel" },
  }, btnH)
end

ui.window("DZ Switch List", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Choose an Expedition", WINDOW_OPTS, drawWindow)
end)
