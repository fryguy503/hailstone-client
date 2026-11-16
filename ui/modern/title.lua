-- Titles: replaces the game's TitleWnd. Your name as it shows now and as it would with the title and suffix picked,
-- then the titles or the suffixes the server offers (tabs) with a search, Change and Clear, and the game's "tell me
-- about new titles" box. The rows are the original's lists (Title_AvailableTitles / Title_AvailableSuffixes): a click
-- picks in the game's list, a double click also changes to it; the buttons are its own.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "TitleWnd"

-- The two kinds (the stock tab box has no page of its own in this client, so the tabs are this window's).
local KINDS = {
  { label = "Titles", list = "Title_AvailableTitles", change = "Title_ChangeTitlePrefix", clear = "Title_ClearTitlePrefix",
    changeLabel = "Use title", clearLabel = "Remove title", noun = "title" },
  { label = "Suffixes", list = "Title_AvailableSuffixes", change = "Title_ChangeTitleSuffix",
    clear = "Title_ClearTitleSuffix", changeLabel = "Use suffix", clearLabel = "Remove suffix", noun = "suffix" },
}
local kind, search = 1, ""

local WINDOW_OPTS = {
  size = function() return 340, 440 end,
  resizable = { 260, 300 },
}
local function drawWindow(x, y, w, h, s)
  -- the name now and with the picks
  local cur, new = W.Text(WINDOW, "Title_CurrentName"), W.Text(WINDOW, "Title_ProposedName")
  local boxH = 58 * s
  ui.DrawRectFilled(x, y, x + w, y + boxH, C.ink[1], C.ink[2], C.ink[3], 0.04, 8 * s)
  T.Text(x + 10 * s, y + 7 * s, "NOW", C.faint, T.font.tiny)
  T.Text(x + 58 * s, y + 5 * s, T.Fit(cur, w - 68 * s, T.font.small), C.text, T.font.small)
  local changed = new ~= "" and new ~= cur
  T.Text(x + 10 * s, y + 33 * s, "NEW", C.faint, T.font.tiny)
  T.Text(x + 58 * s, y + 31 * s, T.Fit(changed and new or "Pick a title or suffix below", w - 68 * s, T.font.small),
    changed and C.accent or C.faint, T.font.small)
  ui.SetCursorScreenPos(x, y + boxH + 8 * s)
  -- titles or suffixes
  if ui.BeginTabBar("##titletabs") then
    for i, k in ipairs(KINDS) do
      local rows = API.windows.ListInfo(WINDOW, k.list) or 0
      if ui.BeginTabItem(string.format("%s (%d)##%d", k.label, rows, i)) then kind = i; ui.EndTabItem() end
    end
    ui.EndTabBar()
  end
  local k = KINDS[kind]
  search = P.Search("##titlesearch", search, "Search " .. k.label:lower())
  local needle = search:lower()
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local foot = btnH + lh + 18 * s
  P.Rows(WINDOW, k.list, 0, -foot, {
    rowH = 24,
    filter = needle ~= "" and function(_, texts) return (texts[1] or ""):lower():find(needle, 1, true) ~= nil end or nil,
    empty = needle ~= "" and ("No " .. k.noun .. " matches") or ("No " .. k.label:lower() .. " to choose from"),
    emptySub = needle == "" and ("The server has given you no " .. k.label:lower() .. " yet") or nil,
    doubleClick = function()
      if W.State(WINDOW, k.change) then API.windows.ClickChild(WINDOW, k.change, "left") end
    end,
  })
  ui.SetCursorScreenPos(x, y + h - foot + 6 * s)
  W.ButtonRow(WINDOW, {
    { k.change, k.changeLabel, { primary = true, tooltip = "Show the selected " .. k.noun .. " with your name" } },
    { k.clear, k.clearLabel, { tooltip = "Show no " .. k.noun } },
  }, btnH)
  W.Check(WINDOW, "Title_NotifyNewTitles", "Tell me when new titles are available")
end

ui.window("Titles", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Titles", WINDOW_OPTS, drawWindow)
end)
