-- Adventures and PvP (gamepad pack), each standing in for its stock window and answered through it (lib/model.lua:
-- the original's children, app/lua_eq_adventure.cpp), so what may be requested, accepted or left is the game's:
--   Adventure (AdventureRequestWnd)          an adventure recruiter: their words (or the adventure offered, or the
--                                            current one's briefing) in a panel D-pad up / down scrolls; on an
--                                            adventure the time to enter / to finish and the progress; the kind and
--                                            the risk to ask for (A opens their choices), Request, Accept / Decline
--                                            an offer, Leave the current adventure (asked first), View Stats
--   Adventure Stats (AdventureStatsWnd)      the points to spend, each theme's wins, losses, ranks and points;
--                                            Refresh, Leaderboard, Done
--   Adventure Leaderboard                    the theme and the order (A opens their choices: a pick asks the server
--   (AdventureLeaderboardWnd)                for that board), your own standing, the board; Refresh, Done
--   PvP Stats (PvPStatsWnd)                  points, kills, deaths, infamy, vitality, the streaks, the last kill and
--                                            death, the kills of the last day; Refresh, Leaderboard, Done
--   PvP Leaderboard (PvpLeaderboardWnd)      what to rank by, your standing, the board; Refresh, Done
-- X refreshes where the window has Refresh. B closes (B with a choice list up takes it down).
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local P = dofile("lib/textpanel.lua")
local C = T.C

-- A pad shortcut of the current window, not in its first frames: the press that opened it is not one for it.
-- (GPSTATE.appeared holds the frame the window appeared in, lib/window.lua)
local function pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 15 and W.pressed(b)
end

-- ---------------------------------------------------------------- the recruiter (AdventureRequestWnd)
local AR = "AdventureRequestWnd"
local arPick, arText, arState = {}, { scroll = 0 }, {}

-- "3 / 10" -> 3, 10
local function progress(text)
  local a, b = (text or ""):match("(%d+)%s*/%s*(%d+)")
  return tonumber(a), tonumber(b)
end

ui.window("Adventure", function()
  M.testShow()
  if not W.shown(AR) then
    arPick, arText, arState = {}, { scroll = 0 }, {}
    return
  end
  T.frame()
  if not M.shown(AR, "AdvRqst_LeaveButton") then arState.confirm = nil end
  local npc = M.text(AR, "AdvRqst_NPCLabel")
  local offer = M.shown(AR, "AdvRqst_AcceptButton") and (M.state(AR, "AdvRqst_AcceptButton"))
  local hints = arPick.open and { { "a", "Choose" }, { "b", "Back" } } or { { "a", "Select" }, { "b", "Close" } }
  if W.begin("Adventure", { w = 1500, h = 900, title = "Adventure", subtitle = npc ~= "NPC Name" and npc or "",
        hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if M.picker(arPick, x0, y0, x1, y1) then
      -- (the kind or the risk is being chosen)
    elseif not W.confirm(arState) then
      local midX = x0 + (x1 - x0) * 0.56
      -- the current adventure: the times and the progress, over the words
      local enter, finish = M.text(AR, "AdvRqst_EnterTimeLeftLabel"), M.text(AR, "AdvRqst_CompleteTimeLeftLabel")
      local prog = M.text(AR, "AdvRqst_ProgressTextLabel")
      local ty = y0
      if enter ~= "" or finish ~= "" or prog ~= "" then
        local facts = {}
        if enter ~= "" then facts[#facts + 1] = { "Time to enter", enter, C.endurance } end
        if finish ~= "" then facts[#facts + 1] = { "Time to finish", finish, C.endurance } end
        if prog ~= "" then facts[#facts + 1] = { "Progress", prog } end
        ty = M.facts(facts, x0, midX - T.px(24), y0) + T.px(10)
        local done, total = progress(prog)
        if done and total and total > 0 then
          T.bar(x0, ty, midX - T.px(24), ty + T.px(14), done / total, C.accent)
          ty = ty + T.px(28)
        end
      end
      P.draw("##advtext", arText, M.text(AR, "AdvRqst_NPCText"), x0, ty, midX - T.px(24), y1,
        { empty = "Speak with an adventure recruiter to request an adventure." })
      -- the right column: the request form, the answer to an offer, leaving, the record
      local rx0 = midX
      local bh = T.px(66)
      ui.SetCursorPos(rx0 - wx, y0 - wy)
      ui.BeginGroup()
      local cw = x1 - rx0
      T.heading("Ask for an adventure")
      M.combo(AR, "AdvRqst_TypeCombobox", "Kind", cw, bh, arPick)
      M.combo(AR, "AdvRqst_RiskCombobox", "Risk", cw, bh, arPick, { none = "-" })
      M.button(AR, "AdvRqst_RequestButton", "Request Adventure", cw, bh, { primary = true })
      T.heading(offer and "The adventure offered" or "Adventure offer")
      M.buttons(AR, { { "AdvRqst_AcceptButton", "Accept", { primary = true } },
        { "AdvRqst_DeclineButton", "Decline" } }, rx0, x1, bh)
      if M.shown(AR, "AdvRqst_LeaveButton") then
        T.heading("Current adventure")
        if T.button("##advleave", "Leave Adventure", cw, bh, { sub = "Give up the current adventure" , align = 0 }, pad) then
          arState.confirm = { "Give up the current adventure?", function() eq.ClickChild(AR, "AdvRqst_LeaveButton", "left") end }
        end
      end
      T.heading("Your record")
      M.button(AR, "AdvRqst_ViewStatsButton", "View Stats", cw, bh, { sub = "Wins, losses and points by theme", align = 0 })
      ui.EndGroup()
    end
  end
  if M.back(W.finish(), arPick, arState) then eq.ShowWindow(AR, false) end
end)

-- ---------------------------------------------------------------- a leaderboard (adventure, PvP)
-- o: win, list, combos { { child, caption } }, you { { caption, child [, colour] } }, refresh, done, nameCol, me
local function board(name, o, pick, state)
  if not W.shown(o.win) then return end
  T.frame()
  local hints = pick.open and { { "a", "Choose" }, { "b", "Back" } }
    or { { "a", "Select" }, { "x", "Refresh" }, { "b", "Close" } }
  if W.begin(name, { w = 1400, h = 940, title = name, hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.picker(pick, x0, y0, x1, y1) then
      if pressed(name, "x") then eq.ClickChild(o.win, o.refresh, "left") end
      -- the choices, then Refresh and Done, along the top
      local bh = T.px(76)
      local n = #o.combos + 2
      local bw = (x1 - x0 - (n - 1) * T.px(10)) / n
      ui.SetCursorPos(x0 - wx, y0 - wy)
      for i, c in ipairs(o.combos) do
        if i > 1 then ui.SameLine(0, T.px(10)) end
        M.combo(o.win, c[1], c[2], bw, bh, pick)
      end
      ui.SameLine(0, T.px(10))
      M.button(o.win, o.refresh, "Refresh", bw, bh, { glyph = "x" })
      ui.SameLine(0, T.px(10))
      M.button(o.win, o.done, "Done", bw, bh)
      -- your standing
      local facts = {}
      for _, f in ipairs(o.you) do facts[#facts + 1] = { f[1], M.text(o.win, f[2]), f[3] or C.endurance } end
      local fy = y0 + bh + T.px(18)
      local by = M.facts(facts, x0, x1, fy) + T.px(18)
      local me = M.text(o.win, o.me)
      local status = o.status and M.text(o.win, o.status) or ""
      M.list(o.win, o.list, state, x0, by, x1, y1, {
        empty = status ~= "" and status or o.empty,
        -- your own row in the endurance colour, as the stock board's yellow line
        rowText = function(_, texts)
          if me ~= "" and texts[o.nameCol] == me then
            texts.colors = {}
            for c = 1, #texts do texts.colors[c] = C.endurance end
          end
          return texts
        end,
      })
    end
  end
  if M.back(W.finish(), pick, nil, nil, state) then eq.ShowWindow(o.win, false) end
end

local ALB = { win = "AdventureLeaderboardWnd", list = "AdvLeaderboard_LeaderList", nameCol = 2,
  combos = { { "AdvLeaderboard_SelectCombobox", "Category" }, { "AdvLeaderboard_RiskCombobox", "Risk" } },
  me = "AdvLeaderboard_YourNameLabel",
  you = { { "Your rank", "AdvLeaderboard_YourRankLabel" }, { "Name", "AdvLeaderboard_YourNameLabel" },
    { "Wins", "AdvLeaderboard_YourSuccessLabel" }, { "Losses", "AdvLeaderboard_YourFailureLabel" },
    { "Win rate", "AdvLeaderboard_YourPercentLabel" } },
  refresh = "AdvLeaderboard_RefreshButton", done = "AdvLeaderboard_DoneButton",
  status = "AdvLeaderboard_StatusLabel",
  empty = "No adventurers on this board yet." }
local albPick, albList = {}, {}
ui.window("Adventure Leaderboard", function() board("Adventure Leaderboard", ALB, albPick, albList) end)

local PLB = { win = "PvpLeaderboardWnd", list = "PvpLeaderboard_LeaderList", nameCol = 2,
  combos = { { "PvpLeaderboard_SelectCombobox", "Rank by" } },
  me = "PvpLeaderboard_YourNameLabel",
  you = { { "Your rank", "PvpLeaderboard_YourRankLabel" }, { "Name", "PvpLeaderboard_YourNameLabel" },
    { "Kills", "PvpLeaderboard_YourKillsLabel" }, { "Deaths", "PvpLeaderboard_YourDeathsLabel" },
    { "Points", "PvpLeaderboard_YourPointsLabel" }, { "Infamy", "PvpLeaderboard_YourInfamyLabel", C.danger } },
  refresh = "PvpLeaderboard_RefreshButton", done = "PvpLeaderboard_DoneButton",
  empty = "No one on this board yet." }
local plbPick, plbList = {}, {}
ui.window("PvP Leaderboard", function() board("PvP Leaderboard", PLB, plbPick, plbList) end)

-- ---------------------------------------------------------------- adventure stats (AdventureStatsWnd)
local AS = "AdventureStatsWnd"
local asList = {}
ui.window("Adventure Stats", function()
  if not W.shown(AS) then return end
  T.frame()
  if W.begin("Adventure Stats", { w = 1400, h = 860, title = "Adventure Stats",
        hints = { { "x", "Refresh" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if pressed("Adventure Stats", "x") then eq.ClickChild(AS, "AdvStats_RefreshButton", "left") end
    -- the totals: the points to spend and the list's final totals row
    local rows = M.rows(AS, "AdvStats_ThemeList")
    local all = rows[#rows] or {}
    local by = M.facts({ { "Points to spend", M.text(AS, "AdvStats_AvailableValue"), C.endurance },
      { "Wins", all[3] }, { "Losses", all[4] }, { "Win rate", all[5] }, { "Points", all[7], C.accent } },
      x0, x1, y0) + T.px(20)
    local bh = T.px(72)
    M.list(AS, "AdvStats_ThemeList", asList, x0, by, x1, y1 - bh - T.px(14),
      { empty = "Your record has not come yet: Refresh asks for it." })
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    M.buttons(AS, { { "AdvStats_RefreshButton", "Refresh", { glyph = "x" } },
      { "AdvStats_LeaderboardButton", "Leaderboard" }, { "AdvStats_DoneButton", "Done" } }, x0, x1, bh)
  end
  if W.finish() then eq.ShowWindow(AS, false) end
end)

-- ---------------------------------------------------------------- PvP stats (PvPStatsWnd)
local PS = "PvPStatsWnd"
local psList = {}
-- the number at the end of a label ("Kills: 12" -> "12"; the labels carry their XML words)
local function value(id)
  return M.text(PS, id):match("(%-?[%d,]+)%s*$") or "-"
end
-- one entry of a one-row list (the last kill / death): name, level, points
local function entry(list)
  local r = M.rows(PS, list, 1)[1]
  if not r or (r[1] or "") == "" then return "None yet" end
  return string.format("%s  (level %s, %s points)", r[1], r[2] or "?", r[3] or "0")
end

ui.window("PvP Stats", function()
  if not W.shown(PS) then return end
  T.frame()
  if W.begin("PvP Stats", { w = 1400, h = 940, title = "PvP Stats",
        hints = { { "x", "Refresh" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if pressed("PvP Stats", "x") then eq.ClickChild(PS, "PvpStats_RefreshButton", "left") end
    local y = M.facts({ { "Points to spend", M.text(PS, "PvpStats_AvailableValue"), C.endurance },
      { "Career points", value("PvpStats_TotalPointsLabel"), C.accent }, { "Kills", value("PvpStats_YourKillsLabel") },
      { "Deaths", value("PvpStats_YourDeathsLabel") }, { "Infamy", value("PvpStats_YourInfamyLevelLabel"), C.danger } },
      x0, x1, y0) + T.px(14)
    y = M.facts({ { "Vitality", value("PvpStats_YourVitalityLabel") },
      { "Current streak", value("PvpStats_YourCurrentStreakLabel") },
      { "Best kill streak", value("PvpStats_YourKillStreakLabel") },
      { "Worst death streak", value("PvpStats_YourDeathStreakLabel") } }, x0, x1, y) + T.px(14)
    y = M.facts({ { "Last kill", entry("PvpStats_LastKillList"), size = 21 },
      { "Last death", entry("PvpStats_LastDeathList"), size = 21 } }, x0, x1, y) + T.px(20)
    ui.SetCursorPos(x0 - wx, y - wy)
    T.heading("Kills in the last 24 hours")
    local ly = select(2, ui.GetCursorScreenPos())
    local bh = T.px(72)
    M.list(PS, "PvpStats_KillsList", psList, x0, ly, x1, y1 - bh - T.px(14), { empty = "No kills in the last day." })
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    M.buttons(PS, { { "PvpStats_RefreshButton", "Refresh", { glyph = "x" } },
      { "PvpStats_LeaderboardButton", "Leaderboard" }, { "PvpStats_DoneButton", "Done" } }, x0, x1, bh)
  end
  if W.finish() then eq.ShowWindow(PS, false) end
end)
