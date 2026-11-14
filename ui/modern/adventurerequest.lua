-- Adventure window: opened by the adventure key or a right click on an adventure recruiter. The
-- recruiter's words (or the adventure offered, or the current one's briefing) in a panel; while on an adventure a
-- status card with the time to get in or to finish and the progress as a bar; the request form keeps the kind and
-- risk visible while its actions are unavailable; an offer gets Accept / Decline; View Stats opens your record.
-- Every value, choice and button is the original's (AdvRqst_*), so what may be requested, accepted or left is the
-- game's; leaving the current adventure is a separate action and asks first.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "AdventureRequestWnd"

-- "3 / 10" -> 3, 10
local function progress(text)
  local a, b = (text or ""):match("(%d+)%s*/%s*(%d+)")
  return tonumber(a), tonumber(b)
end

local function status(s, lh)
  local enter = W.Text(WINDOW, "AdvRqst_EnterTimeLeftLabel")
  local finish = W.Text(WINDOW, "AdvRqst_CompleteTimeLeftLabel")
  local prog = W.Text(WINDOW, "AdvRqst_ProgressTextLabel")
  if enter == "" and finish == "" and prog == "" then return false end
  K.Heading("Current adventure")
  local tiles = {}
  if enter ~= "" then tiles[#tiles + 1] = { "Time to enter", enter, C.gold, "Get inside before this runs out" } end
  if finish ~= "" then tiles[#tiles + 1] = { "Time to finish", finish, C.gold, "Complete the objective before this runs out" } end
  if #tiles > 0 then K.StatRow(tiles, 120) end
  local done, total = progress(prog)
  if done and total and total > 0 then
    ui.Dummy(0, 2 * s)
    K.Muted("Progress")
    ui.SameLine()
    local x, y = ui.GetCursorScreenPos()
    local bw = ui.GetContentRegionAvail()
    T.Bar("adv.progress", x, y + (lh - 12 * s) / 2, x + bw, y + (lh + 12 * s) / 2, s, done / total, C.pet,
      { center = prog, font = T.font.tiny, noTrail = true })
    ui.Dummy(bw, lh)
  end
  return true
end

local WINDOW_OPTS = {
  size = function() return 460, 500 end,
  resizable = { 360, 420 },
  keepTitle = true,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 40 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local offer = W.State(WINDOW, "AdvRqst_AcceptButton") -- an adventure on offer (Accept is live)
  local canLeave = W.Shown(WINDOW, "AdvRqst_LeaveButton")
  -- Keep the request form visible even when opened away from a recruiter; the model disables unavailable actions.
  local footH = btnH * 4 + lh + 36 * s
  ui.BeginChild("##advtext", 0, -footH, false)
  local onAdventure = status(s, lh)
  if onAdventure then K.Heading("Briefing") end
  if offer then K.Heading("Adventure offered") end
  local text = W.Text(WINDOW, "AdvRqst_NPCText")
  if K.HasText(text) then K.Stml(WINDOW, "AdvRqst_NPCText", text)
  else K.Empty("No adventure is assigned.", "Speak with an adventure recruiter to request one.") end
  ui.EndChild()
  ui.Separator()
  local half = (ui.GetContentRegionAvail() - 8 * s) / 2
  local cx = ui.GetCursorPos()
  K.Muted("Kind")
  ui.SameLine(cx + half + 8 * s)
  K.Muted("Risk")
  W.Combo(WINDOW, "AdvRqst_TypeCombobox", { width = half })
  ui.SameLine(0, 8 * s)
  W.Combo(WINDOW, "AdvRqst_RiskCombobox", { width = half })
  ui.Dummy(0, 2 * s)
  W.Button(WINDOW, "AdvRqst_RequestButton", "Request Adventure",
    { primary = true, width = -1, height = btnH, tooltip = "Ask the recruiter for an adventure of this kind and risk" })
  W.ButtonRow(WINDOW, {
    { "AdvRqst_AcceptButton", "Accept", { primary = true, tooltip = "Take the adventure offered" } },
    { "AdvRqst_DeclineButton", "Decline", { tooltip = "Turn the offer down" } },
  }, btnH)
  if canLeave then
    ui.PushStyleColor(ui.Col.Button, C.danger[1], C.danger[2], C.danger[3], 0.18)
    ui.PushStyleColor(ui.Col.ButtonHovered, C.danger[1], C.danger[2], C.danger[3], 0.35)
    ui.PushStyleColor(ui.Col.ButtonActive, C.danger[1], C.danger[2], C.danger[3], 0.50)
    if ui.Button("Leave Adventure##advleave", half, btnH) then ui.OpenPopup("##advleaveask") end
    ui.PopStyleColor(3)
    if ui.BeginPopup("##advleaveask") then
      ui.Text("Give up the current adventure?")
      ui.Dummy(0, 4 * s)
      if W.Button(WINDOW, "AdvRqst_LeaveButton", "Leave", { danger = true, width = 100 * s }) then
        ui.CloseCurrentPopup()
      end
      ui.SameLine()
      if ui.Button("Stay", 100 * s, 0) then ui.CloseCurrentPopup() end
      ui.EndPopup()
    end
    ui.SameLine(0, 8 * s)
  end
  W.Button(WINDOW, "AdvRqst_ViewStatsButton", "View Stats",
    { width = canLeave and half or -1, height = btnH, tooltip = "Your adventure record and points" })
end

ui.window("Adventure", function()
  if not eq.InGame() then return end
  hud.Dialog(WINDOW, "Adventure", WINDOW_OPTS, drawWindow)
end)
