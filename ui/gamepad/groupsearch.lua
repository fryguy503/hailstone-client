-- The group finder (gamepad pack): stands in for GroupSearchWnd (/lfg, the LFG window key, the group window's LFG)
-- and GroupSearchFiltersWnd (its Additional Filters).
--   Group Search   on the left what you post: as a player looking for a group (your info, a comment, the levels you
--                  want) or as a group looking for players (also the classes wanted, Select All / None), with the
--                  window's Update Info, LFG / LFP on and off and Additional Filters; on the right the matches
--                  (LB / RB: players or groups), Show Matches / Show All and Get Matches. A selects a match (a group
--                  shows its members and the classes it wants below), Y sends it a tell (the list's double click),
--                  X sorts by the next column (the list's header click).
--   Filters        use my ignore list, only my friends, the guilds to leave out or keep to (a drop-down to pick one,
--                  Add, Remove, Remove All) and "only players who match my filters can query me".
-- Each page, field, check box and button is the stock windows' own (hs.SetTabPage, hs.SetWindowText,
-- hs.ClickChild, hs.ComboSelect), so what is posted and searched is the game's.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local C = T.C
local WIN = "GroupSearchWnd"

local CLASSES = { "War", "Clr", "Pal", "Ran", "Shd", "Dru", "Mnk", "Brd", "Rog", "Shm", "Nec", "Wiz", "Mag", "Enc",
  "Bst", "Ber" }
local LABEL = { Ran = "RNG" }
-- A class's name as the client shows it (a server's own where its plugin named the class), else `fallback`.
local HAS_CLASS_NAMES = API.Supports and API.Supports("character", "ClassName") == true
local function className(id, fallback, short)
  return HAS_CLASS_NAMES and API.character.ClassName(id, short) or fallback
end

local st = CH.new()
local lists = { players = {}, groups = {}, detail = {}, wanted = {} }
local sortCol = { players = 0, groups = 0 }
local wasShown = false

-- Controls side by side at y across w; each f(w, h) at the cursor. Returns the next y.
local function row(x0, y, w, h, fs, wx, wy)
  local gap = T.px(10)
  local bw = (w - (#fs - 1) * gap) / #fs
  for i, f in ipairs(fs) do
    ui.SetCursorPos(x0 + (i - 1) * (bw + gap) - wx, y - wy)
    f(bw, h)
  end
  return y + h + T.px(10)
end
local function btn(id, label, opts) return function(w, h) CH.button(WIN, id, label, w, h, opts or { keep = true }) end end
local function field(id, caption, max) return function(w, h) CH.field(st, WIN, id, caption, w, h, { max = max or 8, keep = true }) end end

-- What you post (the info pages): x0..x1 from y.
local function infoPage(group, x0, y, x1, y1, wx, wy)
  local cw = x1 - x0
  local p = group and "GIP_" or "PIP_"
  T.font(20)
  T.draw(x0, y, T.fit(CH.plain(WIN, group and "GIP_GroupInfoLabel" or "PIP_PlayerInfoLabel"), cw), C.text)
  T.unfont()
  y = y + T.px(34)
  y = row(x0, y, cw, T.px(76), { field(p .. "CommentsEditbox", "Comments", 128) }, wx, wy)
  y = row(x0, y, cw, T.px(76), {
    field(p .. "LevelRangeMinEditbox", group and "Want player levels from" or "Want group levels from"),
    field(p .. "LevelRangeMaxEditbox", "to"),
    btn(p .. "AdditionalFiltersButton", "Additional Filters"),
  }, wx, wy)
  if group then
    CH.caption(x0, y, "Want classes")
    y = y + T.px(26)
    for r = 0, 3 do
      local fs = {}
      for c = 1, 4 do
        local cl = CLASSES[r * 4 + c]
        local label = className(r * 4 + c, LABEL[cl] or cl:upper(), true)
        fs[#fs + 1] = function(w, h) CH.toggle(WIN, "GIP_" .. cl .. "Checkbox", label, w, h, { on = "wanted", off = "-", keep = true }) end
      end
      y = row(x0, y, cw, T.px(58), fs, wx, wy)
    end
    y = row(x0, y, cw, T.px(54), { btn("GIP_SelectAllRolesButton", "Select All"), btn("GIP_DeselectAllRolesButton", "Select None") }, wx, wy)
  end
  row(x0, y1 - T.px(80), cw, T.px(80), group and {
    btn("GIP_PostOurInfoButton", "LFP on", { primary = true, sub = "Post the group", align = 0, keep = true }),
    btn("GIP_UpdateOurInfoButton", "Update Info", { sub = "Send the changes", align = 0, keep = true }),
    btn("GIP_RemoveOurInfoButton", "LFP off", { sub = "Take it down", align = 0, keep = true }),
  } or {
    btn("PIP_PostMyInfoButton", "LFG on", { primary = true, sub = "Post yourself", align = 0, keep = true }),
    btn("PIP_UpdateMyInfoButton", "Update Info", { sub = "Send the changes", align = 0, keep = true }),
    btn("PIP_RemoveMyInfoButton", "LFG off", { sub = "Take it down", align = 0, keep = true }),
  }, wx, wy)
end

-- The matches (the search pages).
local function searchPage(groups, x0, y, x1, y1, wx, wy)
  local cw = x1 - x0
  local p = groups and "GLP_" or "PLP_"
  local listId = p .. "PlayerSearchResultList"
  T.font(19)
  T.draw(x0, y, CH.plain(WIN, p .. "NumSearchResultsLabel"), C.dim)
  T.unfont()
  y = y + T.px(30)
  y = row(x0, y, cw, T.px(70), {
    function(w, h) CH.toggle(WIN, p .. "ShowMatchesCheckbox", "Show Matches", w, h, { on = "shown", off = "-", keep = true }) end,
    function(w, h) CH.toggle(WIN, p .. "ShowAllCheckbox", "Show All", w, h, { on = "shown", off = "-", keep = true }) end,
    btn(groups and "GLP_RefreshGroupsButton" or "PLP_RefreshPlayerButton", "Get Matches", { primary = true, keep = true }),
  }, wx, wy)
  local key = groups and "groups" or "players"
  local listH = groups and (y1 - y) * 0.55 or (y1 - y)
  ui.SetCursorPos(x0 - wx, y - wy)
  local r = CH.list(WIN, listId, lists[key], cw, listH, { rowH = 52, font = 19,
    empty = "No matches: Get Matches asks the server.",
    columns = groups and { { 1, 0.28 }, { 2, 0.08 }, { 3, 0.14 }, { 4, 0.22 }, { 5, 0.28 } }
      or { { 1, 0.25 }, { 2, 0.1 }, { 3, 0.15 }, { 4, 0.22 }, { 5, 0.28 } } })
  if r and W.pressed("y") then API.windows.ListClick(WIN, listId, r, "double") end -- a tell
  if W.pressed("x") then
    sortCol[key] = (sortCol[key] + 1) % 5
    API.windows.ListHeaderClick(WIN, listId, sortCol[key])
  end
  if groups then
    -- the selected group's members and the classes it wants
    local dy = y + listH + T.px(14)
    local half = (cw - T.px(16)) * 0.62
    CH.caption(x0, dy, "Members")
    CH.caption(x0 + half + T.px(16), dy, "Classes wanted")
    ui.SetCursorPos(x0 - wx, dy + T.px(26) - wy)
    local d = CH.list(WIN, "GLP_GroupDetailList", lists.detail, half, y1 - dy - T.px(26), { rowH = 44, font = 18,
      empty = "Select a group.", columns = { { 1, 0.5 }, { 2, 0.3 }, { 3, 0.2, 1 } } })
    if d and W.pressed("y") then API.windows.ListClick(WIN, "GLP_GroupDetailList", d, "double") end
    ui.SetCursorPos(x0 + half + T.px(16) - wx, dy + T.px(26) - wy)
    CH.list(WIN, "GLP_ClassesWantedList", lists.wanted, cw - half - T.px(16), y1 - dy - T.px(26), { rowH = 44, font = 18,
      empty = "-", onA = function() end })
  end
end

ui.window("Group Search", function()
  CH.testOpen(WIN)
  if not W.shown(WIN) then
    st.kb.open, st.pick, wasShown = false, nil, false
    return
  end
  T.frame()
  local appearing = not wasShown
  wasShown = true
  local info = API.windows.TabPage(WIN, "GSW_InfoPages") or 0
  local search = API.windows.TabPage(WIN, "GSW_SearchPages") or 0
  if W.begin("Group Search", { w = 1800, h = 1010, title = "Group Search",
        subtitle = info == 1 and "Your group looks for players" or "You look for a group",
        hints = CH.hints(st, { { "lb", "Players" }, { "rb", "Groups" }, { "a", "Select" }, { "y", "Tell" }, { "x", "Sort" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(st, x0, y0, x1, y1) then
      if W.pressed("lb") then API.windows.SetTabPage(WIN, "GSW_SearchPages", 0) end
      if W.pressed("rb") then API.windows.SetTabPage(WIN, "GSW_SearchPages", 1) end
      local leftW = cw * 0.44
      local rx0 = x0 + leftW + T.px(30)
      -- "I am a" and "Show me" as tab buttons over each side
      ui.SetCursorPos(x0 - wx, y0 - wy)
      local tw = (leftW - T.px(10)) / 2
      if appearing then ui.SetKeyboardFocusHere() end
      if T.button("##gsiam0", "I am a player", tw, T.px(56), { primary = info == 0 }, pad) then API.windows.SetTabPage(WIN, "GSW_InfoPages", 0) end
      ui.SameLine(0, T.px(10))
      if T.button("##gsiam1", "We are a group", tw, T.px(56), { primary = info == 1 }, pad) then API.windows.SetTabPage(WIN, "GSW_InfoPages", 1) end
      local sw = (x1 - rx0 - T.px(10)) / 2
      ui.SetCursorPos(rx0 - wx, y0 - wy)
      if T.button("##gsshow0", "Players looking for a group", sw, T.px(56), { primary = search == 0, glyph = "lb" }, pad) then API.windows.SetTabPage(WIN, "GSW_SearchPages", 0) end
      ui.SameLine(0, T.px(10))
      if T.button("##gsshow1", "Groups looking for players", sw, T.px(56), { primary = search == 1, glyph = "rb" }, pad) then API.windows.SetTabPage(WIN, "GSW_SearchPages", 1) end
      ui.DrawLine(rx0 - T.px(15), y0 + T.px(70), rx0 - T.px(15), y1, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
      infoPage(info == 1, x0, y0 + T.px(72), x0 + leftW, y1, wx, wy)
      searchPage(search == 1, rx0, y0 + T.px(72), x1, y1, wx, wy)
    end
  end
  if CH.back(st, W.finish()) then API.windows.Show(WIN, false) end
end)

-- ---------------------------------------------------------------- additional filters
local FW = "GroupSearchFiltersWnd"
local fst = CH.new()
local guildLists = { {}, {} }
local fShown = false

local function guilds(x0, y, w, y1, wx, wy, k, what)
  local p = k == 1 and "Excluded" or "Desired"
  y = row(x0, y, w, T.px(70), {
    function(bw, h) CH.toggle(FW, "GSFW_" .. p .. "GuildsCheckbox", what, bw, h, { keep = true }) end,
  }, wx, wy)
  ui.SetCursorPos(x0 - wx, y - wy)
  local listH = y1 - y - 2 * (T.px(76) + T.px(10))
  CH.list(FW, "GSFW_" .. p .. "GuildsList", guildLists[k], w, listH, { rowH = 48, font = 19, empty = "No guilds." })
  y = y + listH + T.px(10)
  y = row(x0, y, w, T.px(76), {
    function(bw, h) CH.combo(fst, FW, k == 1 and "GSFW_GuildNamesToExcludeSelectionCombo" or "GSFW_GuildNamesToDesireSelectionCombo", "Guild", bw, h, { keep = true }) end,
  }, wx, wy)
  row(x0, y, w, T.px(76), {
    function(bw, h) CH.button(FW, "GSFW_Add" .. p .. "GuildButton", "Add", bw, h, { primary = true, keep = true }) end,
    function(bw, h) CH.button(FW, "GSFW_Remove" .. p .. "GuildButton", "Remove", bw, h, { keep = true }) end,
    function(bw, h) CH.button(FW, "GSFW_RemoveAll" .. p .. "GuildButton", "Remove All", bw, h, { keep = true }) end,
  }, wx, wy)
end

ui.window("Group Search Filters", function()
  CH.testOpen(FW)
  if not W.shown(FW) then
    fst.kb.open, fst.pick, fShown = false, nil, false
    return
  end
  T.frame()
  local appearing = not fShown
  fShown = true
  if W.begin("Group Search Filters", { w = 1600, h = 960, title = "Group Search: Additional Filters",
        hints = CH.hints(fst, { { "a", "Select" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(fst, x0, y0, x1, y1) then
      if appearing then
        ui.SetCursorPos(x0 - wx, y0 - wy)
        ui.SetKeyboardFocusHere()
      end
      local y = row(x0, y0, cw, T.px(76), {
        function(w, h) CH.toggle(FW, "GSFW_UseIgnoreListCheckbox", "Use my ignore list", w, h, { keep = true }) end,
        function(w, h) CH.toggle(FW, "GSFW_UseFriendsListCheckbox", "Only show my friends", w, h, { keep = true }) end,
      }, wx, wy)
      y = row(x0, y, cw, T.px(76), {
        function(w, h) CH.toggle(FW, "GSFW_ExclusionSearchCheckbox", "Only players who match my posted filters can query me", w, h, { keep = true }) end,
      }, wx, wy)
      local half = (cw - T.px(30)) / 2
      guilds(x0, y + T.px(10), half, y1, wx, wy, 1, "Leave out these guilds")
      guilds(x0 + half + T.px(30), y + T.px(10), half, y1, wx, wy, 2, "Only these guilds")
    end
  end
  if CH.back(fst, W.finish()) then API.windows.Show(FW, false) end
end)
