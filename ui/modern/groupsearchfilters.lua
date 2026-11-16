-- Group search filters: replaces the game's GroupSearchFiltersWnd (the group search window's Additional Filters
-- buttons). Who the results leave out: your ignore list, or only your friends (one at a time, as the game keeps
-- them); guilds to exclude or the only guilds to include (one at a time), each a drop-down of the guilds the game
-- knows with Add, Remove and Remove All over its list; and whether only players who match your posted filters may
-- find you. Every check box, list and button is the window's (app/lua_ui_group_search.cpp), which saves each change.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local K = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local WINDOW = "GroupSearchFiltersWnd"

-- The labels' text (EQUI_GroupSearchWnd.xml), for a label the window leaves empty.
local LABELS = {
  GSFW_UseIgnoreListLabel = "Use my ignore list",
  GSFW_UseFriendsListLabel = "Only show my friends (uses your friends list)",
  GSFW_ExcludedGuildsLabel = "Do not include these guilds in my search results",
  GSFW_DesiredGuildsLabel = "Only include these guilds in my search results",
  GSFW_ExclusionSearchLabel = "Only players who match my posted filters can query me",
}

local function check(id, labelId)
  local text = W.Text(WINDOW, labelId)
  W.Check(WINDOW, id, text ~= "" and text or LABELS[labelId])
end

-- One guild list: its check box, the guild drop-down with Add, the list, Remove and Remove All.
local function guilds(kind, combo, h, s)
  local on = select(2, W.State(WINDOW, "GSFW_" .. kind .. "GuildsCheckbox")) == true
  check("GSFW_" .. kind .. "GuildsCheckbox", "GSFW_" .. kind .. "GuildsLabel")
  ui.Indent(22 * s)
  local addW = 64 * s
  W.Combo(WINDOW, combo, { width = -(addW + 8 * s) })
  ui.SameLine()
  W.Button(WINDOW, "GSFW_Add" .. kind .. "GuildButton", "Add",
    { width = addW, primary = on, tooltip = "Add the guild picked here to the list" })
  local list = "GSFW_" .. kind .. "GuildsList"
  local bh = K.ButtonH(s)
  local listH = math.max(40 * s, h - bh * 2 - 16 * s)
  if (API.windows.ListInfo(WINDOW, list) or 0) == 0 then
    local aw = ui.GetContentRegionAvail()
    S.Empty(aw, listH, "No guilds on this list.")
  else
    W.List(WINDOW, list, 0, listH, { columns = { [1] = { "", 0 } } })
  end
  W.ButtonRow(WINDOW, {
    { "GSFW_Remove" .. kind .. "GuildButton", "Remove", { tooltip = "Take the selected guild off the list" } },
    { "GSFW_RemoveAll" .. kind .. "GuildButton", "Remove All", { tooltip = "Clear this list" } },
  }, bh)
  ui.Unindent(22 * s)
end

local function testOpen() API.windows.Show(WINDOW, true) end
local WINDOW_OPTS = {
  size = function() return 420, 560 end,
  resizable = { 320, 420 },
}
local function drawWindow(x, y, w, h, s)
  W.Heading("Players")
  check("GSFW_UseIgnoreListCheckbox", "GSFW_UseIgnoreListLabel")
  check("GSFW_UseFriendsListCheckbox", "GSFW_UseFriendsListLabel")
  W.Heading("Guilds")
  local _, top = ui.GetCursorScreenPos()
  local _, lh = ui.CalcTextSize("Ag")
  local footer = lh * 2 + 30 * s -- the last heading and check box
  local each = ((y + h) - top - footer) / 2 - lh - 10 * s
  guilds("Excluded", "GSFW_GuildNamesToExcludeSelectionCombo", each, s)
  ui.Dummy(0, 4 * s)
  guilds("Desired", "GSFW_GuildNamesToDesireSelectionCombo", each, s)
  W.Heading("Being found")
  check("GSFW_ExclusionSearchCheckbox", "GSFW_ExclusionSearchLabel")
  if ui.IsItemHovered() then ui.SetTooltip("Players whose own posts your filters drop can't find you") end
end

ui.window("Group Search Filters", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  hud.Dialog(WINDOW, "Group Search Filters", WINDOW_OPTS, drawWindow)
end)
