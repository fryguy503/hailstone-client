-- Map editing: replaces the game's MapToolbarWnd (opened from the map's Edit button). The toolbar's eighteen tools
-- grouped as Lines, Labels, Colours and Map, each group a grid of tiles that reflows to the window's width. The
-- tools are the game's buttons: lines and points are set down where you stand, the colours open the colour picker,
-- Clear asks first, Auto Map stays lit while it runs, and Join At Intersect with Ctrl held moves only the first line.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local tiles = dofile("lib/hudx_tiles.lua")
local W = dofile("lib/widgets.lua")
local WINDOW = "MapToolbarWnd"

local function t(id, label, tip, o)
  o = o or {}
  o.tooltip = tip
  return { id, label, o }
end

local GROUPS = {
  { "Lines", {
    t("MTW_StartLineButton", "Start Line", "Set down a line's first point where you stand"),
    t("MTW_AddPointButton", "Add Point", "Add a point to the line where you stand"),
    t("MTW_EndLineButton", "End Line", "Set down the last point where you stand and finish the line"),
    t("MTW_AutoMapButton", "Auto Map", "Add a point each time you change direction (click again to stop)"),
    t("MTW_RemoveLineButton", "Remove Line", "Cut out the highlighted line"),
    t("MTW_JoinAtIntersectButton", "Join Lines", "Extend the two selected lines to meet where they cross\n" ..
      "Hold Ctrl to move only the first line"),
  } },
  { "Labels", {
    t("MTW_AddLabelButton", "Add Label", "Add a text label where you stand"),
    t("MTW_EditLabelButton", "Edit Label", "Change the selected label's text"),
    t("MTW_MoveLabelToMeButton", "Move To Me", "Move the selected label to where you stand"),
    t("MTW_RemoveLabelButton", "Remove Label", "Remove the highlighted label"),
  } },
  { "Colours", {
    t("MTW_SetCurrentColorButton", "New Lines", "The colour the next lines are drawn in"),
    t("MTW_ChangeLineColorButton", "Selected Line", "Recolour the highlighted line"),
    t("MTW_SetMyColorButton", "You", "Your own colour on the map"),
    t("MTW_SetGroupColorButton", "Group", "Your group's colour on the map"),
    t("MTW_SetPathColorButton", "Find Path", "The colour of a find path on the map"),
  } },
  { "Map", {
    t("MTW_SaveMapButton", "Save Map", "Save the map to its file", { primary = true }),
    t("MTW_ClearLayerButton", "Clear Layer", "Clear the active layer (asks first)", { danger = true }),
    t("MTW_ClearMapButton", "Clear Map", "Clear every line and label from the map (asks first)", { danger = true }),
  } },
}

local testShown = false

local WINDOW_OPTS = {
  size = function() return 300, 560 end,
  resizable = { 150, 200 },
  keepTitle = true,
}
local function drawWindow(x, y, w, h, s)
  ui.BeginChild("##mtools", 0, 0, false)
  for i, g in ipairs(GROUPS) do
    if i > 1 then ui.Dummy(0, 2 * s) end
    W.Heading(g[1])
    tiles.Grid(WINDOW, g[2], { minW = 90, h = 30 })
  end
  ui.EndChild()
end

ui.window("Map Toolbar", function()
  if not API.session.InGame() then return end
  if not testShown then -- testing: test.show.MapToolbarWnd=1 opens it once
    testShown = true
    if ui.GetSetting("test.show." .. WINDOW) == "1" then API.windows.Show(WINDOW, true) end
  end
  hud.Dialog(WINDOW, "Map Editing", WINDOW_OPTS, drawWindow)
end)
