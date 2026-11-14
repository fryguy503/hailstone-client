-- Friends: replaces the game's FriendsWindow. The friends and ignore lists on two tabs (the game's tab box: the
-- Muted page is voice chat, which this server does not have), each with an add field at the top (Enter adds), a
-- search over the names once the list is long, and the actions under it. Adding and removing are the window's
-- own buttons (/friends and /ignore toggle a name), so the lists are the game's.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local WINDOW = "FriendsWindow"

local search = { [0] = "", [1] = "" }
local SEARCH_FROM = 8 -- rows: the search box shows from this many

-- One page: { add field id, add button id, list id, remove button id, what the page holds }.
local PAGES = {
  [0] = { edit = "NameInput", add = "AddButton", list = "FriendsList", remove = "DeleteButton",
    hint = "Add a friend by name", empty = "No friends yet.", emptyHint = "Type a name above and press Enter.",
    removeTip = "Take the selected name off your friends list" },
  [1] = { edit = "IgnoreNameInput", add = "IgnoreAddButton", list = "IgnoreList", remove = "IgnoreDeleteButton",
    hint = "Ignore a player by name", empty = "You are not ignoring anyone.",
    emptyHint = "Their tells and chat stay hidden while they are on this list.",
    removeTip = "Stop ignoring the selected name" },
}

local function count(id)
  local rows = eq.ListInfo(WINDOW, id)
  return rows or 0
end

local function page(p, s)
  local P = PAGES[p]
  -- the add field: typing goes to the window's edit box, Enter (or Add) is its Add button
  local btnW = 64 * s
  local _, committed = W.Edit(WINDOW, P.edit, { hint = P.hint, width = -(btnW + 8 * s) })
  local enter = committed and ui.IsKeyPressed(ui.Key.Enter)
  ui.SameLine()
  local clicked = W.Button(WINDOW, P.add, "Add", { width = btnW, primary = true })
  if enter and not clicked and W.Text(WINDOW, P.edit) ~= "" then eq.ClickChild(WINDOW, P.add, "left") end

  local rows = count(P.list)
  local needle = ""
  if rows >= SEARCH_FROM then
    search[p], needle = S.Search("friendsearch" .. p, search[p], "Search " .. rows .. " names")
  end
  local btnH = S.ButtonHeight(s)
  local listH = -(btnH + 8 * s)
  if rows == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(40 * s, ah - btnH - 8 * s), P.empty, P.emptyHint)
  else
    W.List(WINDOW, P.list, 0, listH, {
      columns = { [1] = { "", 0 } },
      filter = needle ~= "" and function(_, texts) return S.Matches(texts, needle) end or nil,
    })
  end
  local _, _, selected = eq.ListInfo(WINDOW, P.list)
  local has = (selected or -1) >= 0
  ui.BeginDisabled(not has)
  if p == 0 then
    W.ButtonRow(WINDOW, {
      { "WhoButton", "Who", { tooltip = "Look the selected friend up (/who all)" } },
      { P.remove, "Remove", { tooltip = P.removeTip } },
    }, btnH)
  else
    W.ButtonRow(WINDOW, { { P.remove, "Unignore", { tooltip = P.removeTip } } }, btnH)
  end
  ui.EndDisabled()
end

local WINDOW_OPTS = {
  size = function() return 280, 400 end,
  resizable = { 220, 240 },
  keepTitle = true,
}
local function drawWindow(x, y, w, h, s)
  S.TestClicks(WINDOW)
  -- the game's tab box (its Muted page left out); the counts in the labels ("###": the tab keeps its id while
  -- the count changes)
  local shown = W.Tabs(WINDOW, "Subwindows", {
    { "Friends (" .. count("FriendsList") .. ")###friends", 0 },
    { "Ignored (" .. count("IgnoreList") .. ")###ignored", 1 },
  })
  if shown ~= 0 and shown ~= 1 then shown = 0 end
  page(shown, s)
end

ui.window("Friends", function()
  if not eq.InGame() then return end
  hud.Dialog(WINDOW, "Friends", WINDOW_OPTS, drawWindow)
end)
