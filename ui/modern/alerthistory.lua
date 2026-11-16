-- Alert history: replaces the game's AlertHistoryWnd (the alerts you have closed). The alerts as a table (title and
-- text) and Open Alert, which the window enables while one is selected; a double click opens it too. The list and
-- button are the original's (app/lua_ui_fellowship.cpp). EQEmu sends no alerts, so the list stays empty there.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local D = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local WINDOW = "AlertHistoryWnd"

local OPTS = {
  size = function() return 520, 300 end,
  resizable = { 300, 200 },
}
local function body(x, y, w, h, s)
  local bh = D.ButtonH(s)
  local rows = API.windows.ListInfo(WINDOW, "AH_Closed_Alert_List") or 0
  if rows == 0 then
    ui.BeginChild("##alertsempty", 0, -D.FooterH(s), false)
    K.Empty("No closed alerts.", "Alerts you close are kept here to open again.")
    ui.EndChild()
  else
    W.List(WINDOW, "AH_Closed_Alert_List", 0, -D.FooterH(s), {
      columns = { [1] = { "Title", 140 }, [2] = { "Alert", 0 } }, hide = { [3] = true },
      doubleClick = function() API.windows.ClickChild(WINDOW, "AH_Open_Alert_Btn", "left") end,
    })
  end
  W.ButtonRow(WINDOW, {
    { "AH_Open_Alert_Btn", "Open Alert", { primary = true, tooltip = "Open the selected alert again" } },
  }, bh)
end

local function testOpen() API.windows.Show(WINDOW, true) end
ui.window("Alert History", function()
  P.TestOpen(WINDOW, testOpen) -- testing: test.open.AlertHistoryWnd=1
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Alert History", OPTS, body)
end)
