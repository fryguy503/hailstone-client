-- Group search: replaces the game's GroupSearchWnd (the group window's LFG button, /lfg, the LFG window key). Two
-- halves, side by side when the window is wide, stacked when narrow:
--   * "I am": the game's two info pages as tabs: a player looking for a group (who you are, a comment, the group
--     levels wanted, LFG on / off and Update Info) or a group looking for players (the group, a comment, the
--     classes wanted as check boxes grouped by role, with Select All / None, the player levels, LFP on / off and
--     Update Info); each has Additional Filters (GroupSearchFiltersWnd);
--   * "Show me": the players or the groups found, as sortable tables with a search over the rows, Matches / All,
--     and Get Matches (the game counts down its lock in the button); a group's members (red when the filters drop
--     them) and the classes it wants show under the groups. A double click on a result starts a tell to it.
-- Every field, check box, list and button is the window's (app/lua_ui_group_search.cpp), so what is posted and
-- searched is the game's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local K = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "GroupSearchWnd"

-- The class check boxes (GIP_<abbr>Checkbox, labelled by GIP_<abbr>Label), grouped by role as lib/social.lua's
-- colours are.
local ROLES = {
  { "tank", { "War", "Pal", "Shd" } },
  { "healer", { "Clr", "Dru", "Shm" } },
  { "melee", { "Brd", "Bst", "Ber", "Mnk", "Ran", "Rog" } },
  { "caster", { "Enc", "Mag", "Nec", "Wiz" } },
}

-- the labels' text (EQUI_GroupSearchWnd.xml), for a label the window leaves empty
local ABBR = { War = "WAR", Pal = "PAL", Shd = "SHD", Clr = "CLR", Dru = "DRU", Shm = "SHM", Brd = "BRD", Bst = "BST",
  Ber = "BER", Mnk = "MNK", Ran = "RNG", Rog = "ROG", Enc = "ENC", Mag = "MAG", Nec = "NEC", Wiz = "WIZ" }

-- their class ids
local CLASS_ID = { War = 1, Clr = 2, Pal = 3, Ran = 4, Shd = 5, Dru = 6, Mnk = 7, Brd = 8, Rog = 9, Shm = 10, Nec = 11,
  Wiz = 12, Mag = 13, Enc = 14, Bst = 15, Ber = 16 }
-- A server's short name for a class where its plugin named the class (API.character.ClassName), else nil.
local HAS_CLASS_NAMES = API.Supports and API.Supports("character", "ClassName") == true
local function ownShort(abbr)
  if not HAS_CLASS_NAMES then return nil end
  local short = API.character.ClassName(CLASS_ID[abbr], true)
  return short ~= ABBR[abbr] and short or nil
end

local search = { [0] = "", [1] = "" }

local function levels(prefix, caption)
  K.Caption(caption)
  local bw = 52 * ui.GetIO().uiScale
  W.Edit(WINDOW, prefix .. "LevelRangeMinEditbox", { width = bw, max = 4 })
  ui.SameLine()
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "to")
  ui.SameLine()
  W.Edit(WINDOW, prefix .. "LevelRangeMaxEditbox", { width = bw, max = 4 })
end

local function comment(prefix)
  K.Caption("Comment")
  W.Edit(WINDOW, prefix .. "CommentsEditbox", { hint = "What you are after (optional)", max = 64 })
end

local function postButtons(s, update, post, remove, postLabel, removeLabel, postTip, removeTip, updateTip)
  local bh = K.ButtonH(s)
  W.ButtonRow(WINDOW, {
    { post, postLabel, { primary = true, tooltip = postTip } },
    { remove, removeLabel, { tooltip = removeTip } },
  }, bh)
  W.ButtonRow(WINDOW, { { update, "Update Info", { tooltip = updateTip } } }, bh)
end

local function playerInfo(s)
  W.Label(WINDOW, "PIP_PlayerInfoLabel", C.heading, true)
  ui.Dummy(0, 2 * s)
  comment("PIP_")
  levels("PIP_", "Group levels wanted")
  ui.Dummy(0, 2 * s)
  W.Button(WINDOW, "PIP_AdditionalFiltersButton", "Additional Filters...", { width = -1,
    tooltip = "Rules that trim out groups you don't want" })
  ui.Dummy(0, 4 * s)
  postButtons(s, "PIP_UpdateMyInfoButton", "PIP_PostMyInfoButton", "PIP_RemoveMyInfoButton", "LFG on", "LFG off",
    "Post your information so groups seeking players can see it",
    "Take your information down so groups no longer see it",
    "Update the information you have posted")
end

local function groupInfo(s)
  W.Label(WINDOW, "GIP_GroupInfoLabel", C.heading, true)
  ui.Dummy(0, 2 * s)
  comment("GIP_")
  K.Caption("Classes wanted")
  -- a column a role, its classes under it
  local aw = ui.GetContentRegionAvail()
  local colW = aw / #ROLES
  local x0, y0 = ui.GetCursorScreenPos()
  local _, lh = ui.CalcTextSize("Ag")
  local rowH = lh + 8 * s
  local most = 0
  for r, role in ipairs(ROLES) do
    local c = S.ROLE_COLOR[role[1]]
    T.Text(x0 + (r - 1) * colW, y0, S.ROLE_NAME[role[1]], c, T.font.tiny)
    for i, abbr in ipairs(role[2]) do
      ui.SetCursorScreenPos(x0 + (r - 1) * colW, y0 + 14 * s + (i - 1) * rowH)
      local label = W.Text(WINDOW, "GIP_" .. abbr .. "Label")
      ui.PushStyleColor(ui.Col.Text, c[1], c[2], c[3], 1)
      W.Check(WINDOW, "GIP_" .. abbr .. "Checkbox", ownShort(abbr) or (label ~= "" and label) or ABBR[abbr])
      ui.PopStyleColor(1)
    end
    most = math.max(most, #role[2])
  end
  ui.SetCursorScreenPos(x0, y0)
  ui.Dummy(aw, 14 * s + most * rowH)
  W.ButtonRow(WINDOW, {
    { "GIP_SelectAllRolesButton", "Select All" },
    { "GIP_DeselectAllRolesButton", "Select None" },
  })
  levels("GIP_", "Player levels wanted")
  ui.Dummy(0, 2 * s)
  W.Button(WINDOW, "GIP_AdditionalFiltersButton", "Additional Filters...", { width = -1,
    tooltip = "Rules that trim out players you don't want" })
  ui.Dummy(0, 4 * s)
  postButtons(s, "GIP_UpdateOurInfoButton", "GIP_PostOurInfoButton", "GIP_RemoveOurInfoButton", "LFP on", "LFP off",
    "Post your group's information so players seeking groups can see it",
    "Take your group's information down so players no longer see it",
    "Update the information your group has posted")
end

-- A results list with its empty state; h as for W.List.
local function resultList(id, w, h, empty, hint, needle)
  local rows = API.windows.ListInfo(WINDOW, id) or 0
  if rows == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    if w and w > 0 then aw = w end
    local eh = (h and h > 0) and h or (ah + (h or 0))
    ui.BeginGroup()
    S.Empty(aw, math.max(30, eh), empty, hint)
    ui.EndGroup()
    return
  end
  W.List(WINDOW, id, w or 0, h or 0, {
    sortable = true,
    filter = (needle and needle ~= "") and function(_, texts) return S.Matches(texts, needle) end or nil,
    filterKey = needle or "",
  })
end

-- The toolbar over a results page: the count, Matches / All and Get Matches.
local function resultBar(prefix, refresh, s)
  local bh = K.ButtonH(s)
  local seg = 70 * s
  local get = 120 * s
  W.Button(WINDOW, prefix .. "ShowMatchesCheckbox", "Matches", { width = seg, height = bh,
    tooltip = "Only the results that match what you posted" })
  ui.SameLine(0, 2 * s)
  W.Button(WINDOW, prefix .. "ShowAllCheckbox", "All", { width = seg, height = bh, tooltip = "Every result" })
  ui.SameLine()
  local aw = ui.GetContentRegionAvail()
  local count = W.Text(WINDOW, prefix .. "NumSearchResultsLabel")
  local cw = math.max(0, aw - get - 8 * s)
  local _, th = ui.CalcTextSize("Ag")
  local cx, cy = ui.GetCursorScreenPos()
  T.Text(cx, cy + (bh - th) / 2, T.Fit(count, cw), C.muted, nil)
  ui.SetCursorScreenPos(cx + aw - get, cy)
  W.Button(WINDOW, refresh, nil, { primary = true, width = get, height = bh,
    tooltip = "Search again with your posted information and filters" })
end

local function playerResults(s)
  resultBar("PLP_", "PLP_RefreshPlayerButton", s)
  search[0] = S.Search("gswsearch0", search[0], "Search the players found")
  resultList("PLP_PlayerSearchResultList", 0, 0, "No players found.", "Press Get Matches to search.", search[0]:lower())
end

local function groupResults(s)
  resultBar("GLP_", "GLP_RefreshGroupsButton", s)
  search[1] = S.Search("gswsearch1", search[1], "Search the groups found")
  local _, ah = ui.GetContentRegionAvail()
  local _, lh = ui.CalcTextSize("Ag")
  local detailH = math.max(90 * s, ah * 0.42)
  resultList("GLP_PlayerSearchResultList", 0, -(detailH + 6 * s), "No groups found.", "Press Get Matches to search.",
    search[1]:lower())
  local aw = ui.GetContentRegionAvail()
  local half = (aw - 8 * s) / 2
  ui.BeginGroup()
  K.Caption("Members of the selected group")
  resultList("GLP_GroupDetailList", half, detailH - lh - 4 * s, "Select a group.")
  ui.EndGroup()
  ui.SameLine(0, 8 * s)
  ui.BeginGroup()
  K.Caption("Classes it wants")
  resultList("GLP_ClassesWantedList", half, detailH - lh - 4 * s, "Select a group.")
  ui.EndGroup()
end

local OPTS = {
  size = function() return 820, 520 end,
  resizable = { 380, 420 },
}
local function body(x, y, w, h, s)
  local wide = w >= 700 * s
  local infoW = wide and math.min(340 * s, w * 0.4) or w
  local infoH = wide and h or math.min(h * 0.5, 300 * s)
  ui.BeginChild("##gswinfo", infoW, infoH, false)
  W.Heading("I am")
  local page = W.Tabs(WINDOW, "GSW_InfoPages", { { "Looking for a group", 0 }, { "A group looking for players", 1 } })
  if page == 1 then groupInfo(s) else playerInfo(s) end
  ui.EndChild()
  if wide then ui.SameLine(0, 12 * s) end
  ui.BeginChild("##gswresults", 0, wide and h or (h - infoH - 8 * s), false)
  W.Heading("Show me")
  local shown = W.Tabs(WINDOW, "GSW_SearchPages", { { "Players looking for a group", 0 }, { "Groups looking for players", 1 } })
  if shown == 1 then groupResults(s) else playerResults(s) end
  ui.EndChild()
end

local function testOpen() API.windows.Show(WINDOW, true) end
ui.window("Group Search", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  S.TestClicks(WINDOW)
  hud.Dialog(WINDOW, "Group Search", OPTS, body)
end)
