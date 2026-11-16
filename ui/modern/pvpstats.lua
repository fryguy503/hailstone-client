-- PvP record: replaces the game's PvPStatsWnd. Your numbers as tiles (points to spend, career points, kills, deaths,
-- infamy, vitality, the streaks), your last kill and last death as cards, and the kills of the last day as a sortable
-- table; Refresh / Leaderboard / Done. The numbers are the original's labels (PvpStats_*, whose text is the XML's
-- word and the number after it), the lists and buttons its own.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "PvPStatsWnd"

-- the number at the end of a label ("Kills: 12" -> "12"), "-" when it has none yet
local function value(id)
  local n = W.Text(WINDOW, id):match("(%-?[%d,]+)%s*$")
  return n or "-"
end

-- one entry of a one-row list (the last kill / death) as a small card
local function entry(caption, list, color)
  local rows = K.Rows(WINDOW, list)
  local r = rows[1]
  K.Muted(caption)
  if r then
    ui.TextColored(color[1], color[2], color[3], 1, r.texts[1] or "")
    ui.SameLine()
    K.Muted(string.format("level %s, %s points", r.texts[2] or "?", r.texts[3] or "0"), C.faint)
  else
    K.Muted("none yet", C.faint)
  end
end

local WINDOW_OPTS = {
  size = function() return 560, 500 end,
  resizable = { 400, 360 },
  keepTitle = true,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 40 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  K.StatRow({
    { "Points to spend", W.Text(WINDOW, "PvpStats_AvailableValue"), C.pet },
    { "Career points", value("PvpStats_TotalPointsLabel"), C.accent },
    { "Kills", value("PvpStats_YourKillsLabel"), C.text },
    { "Deaths", value("PvpStats_YourDeathsLabel"), C.text },
  }, 100)
  ui.Dummy(0, 2 * s)
  K.StatRow({
    { "Infamy", value("PvpStats_YourInfamyLevelLabel"), C.danger },
    { "Vitality", value("PvpStats_YourVitalityLabel"), C.text },
    { "Streak", value("PvpStats_YourCurrentStreakLabel"), C.gold, "Your current run of kills" },
    { "Best streak", value("PvpStats_YourKillStreakLabel"), C.gold },
    { "Worst run", value("PvpStats_YourDeathStreakLabel"), C.muted, "Your longest run of deaths" },
  }, 90)
  ui.Dummy(0, 4 * s)
  local half = (ui.GetContentRegionAvail() - 12 * s) / 2
  ui.BeginGroup()
  ui.BeginChild("##pvplastkill", half, lh * 2 + 8 * s, false)
  entry("Last kill", "PvpStats_LastKillList", C.text)
  ui.EndChild()
  ui.EndGroup()
  ui.SameLine(0, 12 * s)
  ui.BeginChild("##pvplastdeath", 0, lh * 2 + 8 * s, false)
  entry("Last death", "PvpStats_LastDeathList", C.text)
  ui.EndChild()
  local rows = API.windows.ListInfo(WINDOW, "PvpStats_KillsList") or 0
  K.Heading("Kills in the last day", rows == 1 and "1 kill" or (rows .. " kills"))
  if rows == 0 then
    ui.BeginChild("##pvpkillsempty", 0, -(btnH + 8 * s), false)
    K.Empty("No kills in the last day.")
    ui.EndChild()
  else
    W.List(WINDOW, "PvpStats_KillsList", 0, -(btnH + 8 * s), { sortable = true })
  end
  W.ButtonRow(WINDOW, {
    { "PvpStats_RefreshButton", "Refresh" },
    { "PvpStats_LeaderboardButton", "Leaderboard", { tooltip = "The best fighters by kills, points or infamy" } },
    { "PvpStats_DoneButton", "Done" },
  }, btnH)
end

ui.window("PvP Stats", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "PvP Record", WINDOW_OPTS, drawWindow)
end)
