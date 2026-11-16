-- The zone guide's window: ZonePathWnd (the zones on the way to the zone guide's destination; app/lua_ui_zoneguide.cpp).
-- EQEmu sends no zone guide, so it never opens (the game refuses without a path). Find is a page of the map
-- (map.lua).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local PATH = "ZonePathWnd"

-- testing: test.draw.<screen>=1 draws the window though the game has not opened it (Zone Path without a path)
local drawKeys = {}
local function forced(win)
  local key = drawKeys[win]
  if not key then key = "test.draw." .. win; drawKeys[win] = key end
  return T.Setting(key) == "1" or nil
end

local PATH_OPTS = {
  size = function() return 280, 300 end,
  resizable = { 200, 160 },
}
local function pathBody(x, y, w, h, s)
  K.Muted("The zones on the way to your destination:", C.muted)
  ui.Dummy(0, 2 * s)
  local rows = API.windows.ListInfo(PATH, "ZPW_ZoneList") or 0
  if rows == 0 then
    ui.BeginChild("##pathempty", 0, 0, false)
    K.Empty("No path.", "Find a place in another zone to get one.")
    ui.EndChild()
  else
    W.List(PATH, "ZPW_ZoneList", 0, 0, {})
  end
end

ui.window("Zone Path", function()
  if not API.session.InGame() then return end
  PATH_OPTS.shown = forced(PATH)
  hud.Dialog(PATH, "Zone Path", PATH_OPTS, pathBody)
end)
