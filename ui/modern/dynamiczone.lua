-- Expedition: replaces the game's DynamicZoneWnd. The expedition you are in (its name, leader and head count as
-- tiles, the members as a sortable table, Quit), the leader's tools while you lead it (the target, Add / Remove /
-- Make Leader / Invite Raid / Invite Task, shown only to the leader as the game shows DZLeaderSection), and your
-- lockouts, searchable, filling the rest. Every value and button is the original's child (DZ_*), so the rules
-- (who may be added, the head count) are the game's; Quit asks first.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "DynamicZoneWnd"

local state = {}

local function leaderTools(s, btnH)
  K.Heading("Leader", "You lead this expedition")
  local target = W.Text(WINDOW, "DZ_Leader_TargetValue")
  K.Muted("Target")
  ui.SameLine()
  if target ~= "" then ui.TextColored(C.text[1], C.text[2], C.text[3], 1, target)
  else K.Muted("none (target a player to add them)", C.faint) end
  W.ButtonRow(WINDOW, {
    { "DZ_Leader_Add", "Add", { primary = true, tooltip = "Add your target to the expedition" } },
    { "DZ_Leader_Remove", "Remove", { tooltip = "Remove the selected member (else your target)" } },
    { "DZ_Leader_MakeLeader", "Make Leader", { tooltip = "Hand the lead to the selected member (else your target)" } },
    { "DZ_Leader_InviteRaid", "Invite Raid", { tooltip = "Invite everyone in your raid who is not in the expedition" } },
    { "DZ_Leader_InviteTask", "Invite Task",
      { tooltip = "Invite everyone in your shared task who is not in the expedition" } },
  }, btnH)
end

local function quitButton(s, btnH)
  local enabled = W.State(WINDOW, "DZ_Quit")
  ui.PushStyleColor(ui.Col.Button, C.danger[1], C.danger[2], C.danger[3], 0.18)
  ui.PushStyleColor(ui.Col.ButtonHovered, C.danger[1], C.danger[2], C.danger[3], 0.35)
  ui.PushStyleColor(ui.Col.ButtonActive, C.danger[1], C.danger[2], C.danger[3], 0.50)
  ui.BeginDisabled(not enabled)
  if ui.Button("Quit Expedition##dzquit", 150 * s, btnH) then ui.OpenPopup("##dzquitask") end
  ui.EndDisabled()
  ui.PopStyleColor(3)
  if ui.BeginPopup("##dzquitask") then
    ui.Text("Leave \"" .. W.Text(WINDOW, "DZ_CurrentDZValue") .. "\"?")
    ui.Dummy(0, 4 * s)
    if W.Button(WINDOW, "DZ_Quit", "Quit", { danger = true, width = 100 * s }) then ui.CloseCurrentPopup() end
    ui.SameLine()
    if ui.Button("Stay", 100 * s, 0) then ui.CloseCurrentPopup() end
    ui.EndPopup()
  end
end

local WINDOW_OPTS = {
  size = function() return 540, 520 end,
  resizable = { 400, 320 },
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 40 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local name = W.Text(WINDOW, "DZ_CurrentDZValue")
  if name ~= "" then
    ui.PushFont(T.font.large)
    ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, name)
    ui.PopFont()
    local n, max = W.Text(WINDOW, "DZ_NumPlayersValue"), W.Text(WINDOW, "DZ_MaxPlayersValue")
    K.StatRow({
      { "Leader", W.Text(WINDOW, "DZ_LeaderValue"), C.text },
      { "Members", n .. " / " .. max, C.text, "In the expedition / the most it can hold" },
    }, 140)
    ui.Dummy(0, 2 * s)
    -- the members (a click selects one for the leader's buttons) and Quit
    local rows = API.windows.ListInfo(WINDOW, "DZ_MemberList") or 0
    local membersH = math.min(math.max(rows, 2) * (lh + 4 * s) + lh + 12 * s, 170 * s)
    W.List(WINDOW, "DZ_MemberList", 0, membersH, { sortable = true })
    quitButton(s, btnH)
    if API.windows.ChildShown(WINDOW, "DZLeaderSection") == true then
      leaderTools(s, btnH)
    end
  else
    K.Empty("You are not in an expedition.", "Join one to see its members and leader here.")
  end
  -- the lockouts fill the rest
  local timers = K.Rows(WINDOW, "DZ_TimerList")
  K.Heading("Lockouts", #timers == 1 and "1 timer" or (#timers .. " timers"))
  if #timers == 0 then
    K.Empty("No lockouts.", "Expeditions you cannot enter again yet are listed here.")
  else
    local needle = #timers > 6 and K.Search(state, "dzsearch", "Search lockouts") or ""
    W.List(WINDOW, "DZ_TimerList", 0, 0, {
      sortable = true,
      filter = needle ~= "" and function(_, texts)
        return ((texts[1] or "") .. " " .. (texts[2] or "")):lower():find(needle, 1, true) ~= nil
      end or nil,
      filterKey = needle,
    })
  end
end

ui.window("Expedition", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Expedition", WINDOW_OPTS, drawWindow)
end)
