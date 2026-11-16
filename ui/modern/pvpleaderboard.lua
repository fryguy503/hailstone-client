-- PvP leaderboard: replaces the game's PvpLeaderboardWnd. What to rank by as a drop-down (the original's combo box:
-- a pick asks the server), your own standing as tiles, the board as a sortable table with a name search (your row
-- in gold), Refresh / Done (lib/quests_board.lua). All of it the original's PvpLeaderboard_* children.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local B = dofile("lib/quests_board.lua")
local WINDOW = "PvpLeaderboardWnd"

local WINDOW_OPTS = {
  size = function() return 500, 480 end,
  resizable = { 380, 340 },
  keepTitle = true,
  place = function(sw, sh, w, h) return sw / 2 - w / 2 + 120, sh / 2 - h / 2 - 20 end,
}
local function drawWindow(x, y, w, h, s)
  B.Draw({
    s = s, win = WINDOW, list = "PvpLeaderboard_LeaderList", nameCol = 2,
    combos = { { "PvpLeaderboard_SelectCombobox", "Rank by" } },
    meName = "PvpLeaderboard_YourNameLabel",
    you = {
      { "Rank", "PvpLeaderboard_YourRankLabel" },
      { "Kills", "PvpLeaderboard_YourKillsLabel" },
      { "Deaths", "PvpLeaderboard_YourDeathsLabel" },
      { "Points", "PvpLeaderboard_YourPointsLabel", hud.T.color.pet },
      { "Infamy", "PvpLeaderboard_YourInfamyLabel", hud.T.color.danger },
    },
    refresh = "PvpLeaderboard_RefreshButton", done = "PvpLeaderboard_DoneButton",
    empty = "No one on this board yet.",
  })
end

ui.window("PvP Leaderboard", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "PvP Leaderboard", WINDOW_OPTS, drawWindow)
end)
