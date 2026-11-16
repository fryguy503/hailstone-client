-- Load a skin: replaces the game's LoadskinWnd (/loadskin with no skin named). The skins are this client's user
-- interfaces (ui/<pack>), the running one selected; "Keep your layout" (the original's INIButton, checked at first)
-- and Load Skin, which the window enables once a row is selected and which runs /loadskin with them. The list, the
-- toggle and the button are the original's (app/lua_ui_layout.cpp); a double click on a row is Load Skin.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local WINDOW = "LoadskinWnd"

local WINDOW_OPTS = {
  size = function() return 320, 320 end,
  resizable = { 240, 220 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local _, lh = ui.CalcTextSize("Ag")
  W.List(WINDOW, "SkinList", 0, -(bh + lh + 20 * s), {
    columns = { [1] = { "Skin", 0 } },
    doubleClick = function() API.windows.ClickChild(WINDOW, "LoadButton", "left") end,
  })
  ui.Dummy(0, 2 * s)
  W.Check(WINDOW, "INIButton", "Keep your layout",
    { tooltip = "Keep this character's window places and sizes with the new skin" })
  W.ButtonRow(WINDOW, {
    { "LoadButton", "Load Skin", { primary = true, tooltip = "Switch to the selected skin" } },
  }, bh)
end

ui.window("Load Skin", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Load a Skin", WINDOW_OPTS, drawWindow)
end)
