-- Story: replaces the game's StoryWnd (the news and story pages, the game's storyline\story*.txt). The stories,
-- newest first, beside the selected one's page, or above it when narrow; "Show new stories when I log in" is the
-- original's Automatic check box. The list, page and check box are the original's (app/lua_ui_story.cpp), so what
-- is marked read is the game's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "StoryWnd"

local WINDOW_OPTS = {
  size = function() return 680, 440 end,
  resizable = { 360, 260 },
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local footer = lh + 14 * s
  local wide = w >= 560 * s
  local rows = API.windows.ListInfo(WINDOW, "StoryList") or 0
  local lw = wide and math.floor((w - 12 * s) * 0.5) or w
  local _, restH = ui.GetContentRegionAvail()
  local lh2 = wide and -footer or math.floor(((restH or 300) - footer) * 0.35)
  local lx, ly = ui.GetCursorScreenPos()
  ui.BeginChild("##storylist", lw, lh2, false)
  W.List(WINDOW, "StoryList", 0, 0, { columns = { [1] = { "Date", 150 }, [2] = { "Title", 0 } } })
  ui.EndChild()
  if rows == 0 then T.Text(lx + 8 * s, ly + lh + 10 * s, "No stories in the game folder.", C.faint, T.font.small) end
  if wide then ui.SameLine(0, 12 * s) end
  K.TextBox(WINDOW, "Display", 0, -footer, { font = T.font.normal, empty = "Select a story to read it." })
  W.Check(WINDOW, "AutomaticButton", "Show new stories when I log in",
    { tooltip = "Open this window when a story newer than the last one read is there" })
end

ui.window("Story", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Story", WINDOW_OPTS, drawWindow)
end)
