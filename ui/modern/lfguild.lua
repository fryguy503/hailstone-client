-- Looking for Guild: replaces the game's LFGuildWnd. Its four pages as tabs:
--   * Find a Guild: your own entry (your level and class, time zone, a comment), whether it is posted, and the
--     window's Post / Remove / Update buttons.
--   * Recruit: your guild's entry: the classes it wants as toggle chips (coloured by role), the level range, a
--     minimum AA count, time zone and comment, and Post / Remove / Update (the game enables them in a guild).
--   * Search Guilds and Search Players: the search's fields, Search, the matches as a sortable table and the
--     selected match's comment under it.
-- Every field is the window's own (typing and picking change its edit boxes and drop-downs) and every button is
-- its button, so what is posted and searched is the game's. The window keeps no page of its own: the tabs here are
-- this UI's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local WINDOW = "LFGuildWnd"

local page = 0

-- A form: a caption column and a field column (ImGui table).
local function form(id, rows, s)
  if not ui.BeginTable("##form" .. id, 2, 0) then return end
  ui.TableSetupColumn("##cap", ui.TableColumnFlags.WidthFixed, 110 * s)
  ui.TableSetupColumn("##val", ui.TableColumnFlags.WidthStretch, 0)
  for _, r in ipairs(rows) do
    ui.TableNextRow()
    ui.TableNextColumn()
    S.Caption(r[1])
    ui.TableNextColumn()
    r[2]()
  end
  ui.EndTable()
end

local function number(id, s) W.Edit(WINDOW, id, { width = 60 * s, max = 8 }) end

-- The level range: two number fields with "to" between.
local function levels(minId, maxId, s)
  return function()
    number(minId, s)
    ui.SameLine()
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "to")
    ui.SameLine()
    number(maxId, s)
  end
end

-- The sixteen class check boxes (prefix GLFP_ or GSFP_) as toggle chips, with Select All / Select None.
local function classChips(prefix, s)
  local avail = ui.GetContentRegionAvail()
  local gap = 4 * s
  local cw = 56 * s
  local per = math.max(4, math.min(8, math.floor((avail + gap) / (cw + gap))))
  cw = (avail - (per - 1) * gap) / per
  local _, lh = ui.CalcTextSize("Ag")
  local n = 0
  for _, c in ipairs(S.CLASSES) do
    local id = prefix .. c[2] .. "Checkbox"
    local enabled, checked = W.State(WINDOW, id)
    local col = S.ROLE_COLOR[c[3]]
    if n % per ~= 0 then ui.SameLine(0, gap) end
    n = n + 1
    ui.PushStyleColor(ui.Col.Button, col[1], col[2], col[3], checked and 0.32 or 0.05)
    ui.PushStyleColor(ui.Col.ButtonHovered, col[1], col[2], col[3], checked and 0.42 or 0.16)
    ui.PushStyleColor(ui.Col.ButtonActive, col[1], col[2], col[3], 0.5)
    if checked then ui.PushStyleColor(ui.Col.Text, C.text[1], C.text[2], C.text[3], 1) else ui.PushStyleColor(ui.Col.Text, C.faint[1], C.faint[2], C.faint[3], 1) end
    ui.BeginDisabled(not enabled)
    if ui.Button(c[2] .. "##" .. id, cw, lh + 8 * s) and enabled then API.windows.ClickChild(WINDOW, id, "left") end
    ui.EndDisabled()
    ui.PopStyleColor(4)
    if ui.IsItemHovered() then ui.SetTooltip(c[1] .. " (" .. S.ROLE_NAME[c[3]] .. ")" .. (checked and "" or "  -  off")) end
  end
  W.Button(WINDOW, prefix .. "SelectAllRolesButton", "Select All")
  ui.SameLine()
  W.Button(WINDOW, prefix .. "DeselectAllRolesButton", "Select None")
end

-- Posted or not (the window's On button is checked while the entry is up), with when.
local function status(onId, expireId)
  local _, on = W.State(WINDOW, onId)
  S.Chip(on and "posted" or "not posted", on and C.good or C.faint)
  local when = W.Text(WINDOW, expireId)
  if when ~= "" then
    ui.SameLine()
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, when)
  end
end

-- Post, Update and Remove (the window's On, Update and Off buttons; On and Off are a pair whose checked one is
-- the current state, which the status chip shows, so they are drawn as plain buttons here).
local function postButtons(prefix, s, what)
  local btnH = S.ButtonHeight(s)
  local list = {
    { prefix .. "OnButton", "Post", true, "Post " .. what .. " (the server keeps it for a while)" },
    { prefix .. "UpdateButton", "Update", false, "Send the changes to " .. what },
    { prefix .. "OffButton", "Remove", false, "Take " .. what .. " down" },
  }
  local avail = ui.GetContentRegionAvail()
  local bw = (avail - 16) / 3
  for i, b in ipairs(list) do
    if i > 1 then ui.SameLine() end
    local enabled = API.windows.ChildState(WINDOW, b[1]) ~= false
    if b[3] then
      ui.PushStyleColor(ui.Col.Button, C.accent[1], C.accent[2], C.accent[3], 0.24)
      ui.PushStyleColor(ui.Col.ButtonHovered, C.accent[1], C.accent[2], C.accent[3], 0.40)
    end
    ui.BeginDisabled(not enabled)
    if ui.Button(b[2] .. "##" .. b[1], bw, btnH) and enabled then API.windows.ClickChild(WINDOW, b[1], "left") end
    ui.EndDisabled()
    if b[3] then ui.PopStyleColor(2) end
    if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then ui.SetTooltip(b[4]) end
  end
end

local function findGuildPage(s)
  W.Label(WINDOW, "PLFG_LevelClassLabel", C.text)
  status("PLFG_OnButton", "PLFG_ExpireLabel")
  ui.Dummy(0, 4 * s)
  form("plfg", {
    { "Time zone", function() W.Combo(WINDOW, "PLFG_TimeZoneCombo") end },
  }, s)
  S.Caption("About you (guilds see this)")
  local btnH = S.ButtonHeight(s)
  W.Edit(WINDOW, "PLFG_CommentsEdit", { multiline = true, height = math.max(60 * s, select(2, ui.GetContentRegionAvail()) - btnH - 10 * s), max = 256 })
  postButtons("PLFG_", s, "your entry")
end

local function recruitPage(s)
  local inGuild = API.windows.ChildState(WINDOW, "GLFP_UpdateButton") ~= false
  status("GLFP_OnButton", "GLFP_ExpireLabel")
  if not inGuild then
    ui.SameLine()
    ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, "Only a guild can post this.")
  end
  W.Heading("Classes wanted")
  classChips("GLFP_", s)
  ui.Dummy(0, 4 * s)
  form("glfp", {
    { "Levels", levels("GLFP_LevelRangeMinEditbox", "GLFP_LevelRangeMaxEditbox", s) },
    { "Minimum AA", function() number("GLFP_MinAACountEdit", s) end },
    { "Time zone", function() W.Combo(WINDOW, "GLFP_TimeZoneCombo") end },
  }, s)
  S.Caption("About the guild (players see this)")
  local btnH = S.ButtonHeight(s)
  W.Edit(WINDOW, "GLFP_CommentsEdit", { multiline = true, height = math.max(50 * s, select(2, ui.GetContentRegionAvail()) - btnH - 10 * s), max = 256 })
  postButtons("GLFP_", s, "the guild's entry")
end

-- The matches and the selected one's comment.
local function results(listId, commentId, s, emptyText)
  local _, ah = ui.GetContentRegionAvail()
  local commentH = math.max(50 * s, ah * 0.25)
  local rows = API.windows.ListInfo(WINDOW, listId) or 0
  if rows == 0 then
    local aw = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(30 * s, ah - commentH - 30 * s), emptyText, "Set the fields above and press Search.")
  else
    W.List(WINDOW, listId, 0, -(commentH + 26 * s), { sortable = true })
  end
  W.Heading("Comment")
  if W.Text(WINDOW, commentId) == "" then
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, rows > 0 and "Select a match to read its comment." or "")
  else
    W.Stml(WINDOW, commentId, 0, 0)
  end
end

local function searchGuildsPage(s)
  form("psfg", {
    { "Your level", function() number("PSFG_LevelEdit", s) end },
    { "Your AA", function() number("PSFG_AACountEdit", s) end },
    { "Class", function() W.Combo(WINDOW, "PSFG_ClassCombo") end },
    { "Time zone", function() W.Combo(WINDOW, "PSFG_TimeZoneCombo") end },
  }, s)
  W.Button(WINDOW, "PSFG_SearchButton", "Search", { primary = true, width = -1, tooltip = "Guilds recruiting a player like this" })
  results("PSFG_SearchResultList", "PSFG_Comment", s, "No guilds found yet.")
end

local function searchPlayersPage(s)
  classChips("GSFP_", s)
  ui.Dummy(0, 4 * s)
  form("gsfp", {
    { "Levels", levels("GSFP_LevelRangeMinEditbox", "GSFP_LevelRangeMaxEditbox", s) },
    { "Minimum AA", function() number("GSFP_MinAACountEdit", s) end },
    { "Time zone", function() W.Combo(WINDOW, "GSFP_TimeZoneCombo") end },
  }, s)
  W.Button(WINDOW, "GSFP_SearchButton", "Search", { primary = true, width = -1, tooltip = "Players looking for a guild" })
  results("GSFP_SearchResultList", "GSFP_Comment", s, "No players found yet.")
end

local PAGES = {
  { "Find a Guild", findGuildPage },
  { "Recruit", recruitPage },
  { "Search Guilds", searchGuildsPage },
  { "Search Players", searchPlayersPage },
}

local WINDOW_OPTS = {
  size = function() return 520, 560 end,
  resizable = { 420, 420 },
  keepTitle = true,
}
local function drawWindow(x, y, w, h, s)
  S.TestClicks(WINDOW)
  local tp = tonumber(ui.GetSetting("test.lfguild.page") or "") -- testing: 0..3
  if ui.BeginTabBar("##lfgtabs") then
    for i, p in ipairs(PAGES) do
      local flags = (tp == i - 1 and page ~= tp) and ui.TabItemFlags.SetSelected or 0
      if ui.BeginTabItem(p[1], flags) then
        page = i - 1
        ui.EndTabItem()
      end
    end
    ui.EndTabBar()
  end
  PAGES[page + 1][2](s)
end

ui.window("Looking for Guild", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Looking for Guild", WINDOW_OPTS, drawWindow)
end)
