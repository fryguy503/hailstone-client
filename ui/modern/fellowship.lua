-- Fellowship: replaces the game's FellowshipWnd (the fellowship key). The game's two pages as tabs:
--   * Fellowship: the message of the day with Update, the members (name, level, class, leader, zone, last on,
--     experience sharing), and the actions: Create, Invite (your target), Make Leader, Remove, Toggle XP, Leave and
--     End Fellowship (the game enables each as you lead, belong or neither);
--   * Camp: your campsite (where it is, Create Camp and Destroy Camp), the campsite kits with their filter and
--     "Show all possible", the selected kit's description, and Refresh List.
-- Every field, list and button uses the shared model (app/lua_eq_fellowship.cpp). The server decides membership,
-- permissions and campsite availability; the member list scrolls through rosters of up to 64 players.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local K = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "FellowshipWnd"
-- Testing: test.tab.FellowshipWnd.Subwindows=n in ui/settings/modern.txt opens page n once.
local testTab = tonumber(ui.GetSetting("test.tab.FellowshipWnd.Subwindows") or "")

local function fellowshipPage(s)
  local bh = K.ButtonH(s)
  K.Caption("Message of the day")
  local updW = 72 * s
  local _, committed = W.Edit(WINDOW, "MOTDInput", { width = -(updW + 8 * s), max = 1023,
    hint = "Your fellowship's message of the day" })
  local enter = committed and ui.IsKeyPressed(ui.Key.Enter)
  ui.SameLine()
  local clicked = W.Button(WINDOW, "UpdateMOTDButton", "Update", { width = updW,
    tooltip = "Set this as your fellowship's message of the day" })
  if enter and not clicked and W.State(WINDOW, "UpdateMOTDButton") then eq.ClickChild(WINDOW, "UpdateMOTDButton", "left") end
  ui.Dummy(0, 2 * s)
  W.Label(WINDOW, "FellowshipVitality", C.muted, true)
  local count = eq.ListInfo(WINDOW, "MemberList") or 0
  K.Caption(count > 0 and ("Members (" .. count .. ")") or "Members")
  local below = bh * 2 + 12 * s
  if count == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(40 * s, ah - below), "You are not in a fellowship.",
      "Create one, then invite players by targeting them.")
  else
    W.List(WINDOW, "MemberList", 0, -below, { sortable = true })
  end
  W.ButtonRow(WINDOW, {
    { "CreateFellowshipButton", "Create", { primary = true, tooltip = "Start a fellowship (you lead it)" } },
    { "InviteMemberButton", "Invite", { tooltip = "Invite your target to the fellowship" } },
    { "MakeLeaderButton", "Make Leader", { tooltip = "Make the selected member the leader" } },
    { "RemoveMemberButton", "Remove", { danger = true, tooltip = "Remove the selected member" } },
  }, bh)
  W.ButtonRow(WINDOW, {
    { "ToggleExpSharingButton", "Toggle XP", { tooltip = "Turn experience sharing on or off for the selected member" } },
    { "LeaveFellowshipButton", "Leave", { tooltip = "Leave the fellowship" } },
    { "EndFellowshipButton", "End Fellowship", { danger = true, tooltip = "Disband the fellowship for everyone" } },
  }, bh)
end

local function campPage(s)
  local bh = K.ButtonH(s)
  W.Heading("Your campsite")
  if W.Text(WINDOW, "CurrentCampLoc") ~= "" then W.Label(WINDOW, "CurrentCampLoc", C.muted, true) end
  local where = W.Text(WINDOW, "CampsiteViewer")
  if where ~= "" then
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, where)
  else
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "No campsite.")
  end
  if W.Text(WINDOW, "CampsiteTime") ~= "" then W.Label(WINDOW, "CampsiteTime", C.muted, true) end
  W.ButtonRow(WINDOW, {
    { "CreateCampsite", "Create Camp", { primary = true, tooltip = "Set up a campsite here" } },
    { "DestroyCampsite", "Destroy Camp", { tooltip = "Take your campsite down" } },
  }, bh)
  W.Heading("Campsite kits")
  W.Combo(WINDOW, "CampsiteFilter", { width = -(150 * s) })
  if ui.IsItemHovered() then ui.SetTooltip("The kinds of kit to list") end
  ui.SameLine()
  W.Check(WINDOW, "ShowAllButton", "Show all possible", { tooltip = "Also list the kits you cannot use yet" })
  local descH = 70 * s
  local below = descH + bh + 14 * s
  if (eq.ListInfo(WINDOW, "CampsiteKitList") or 0) == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(30 * s, ah - below), "No campsite kits listed.", "Press Refresh List to ask for them.")
  else
    W.List(WINDOW, "CampsiteKitList", 0, -below, { columns = { [1] = { "", 16 }, [2] = { "Campsite kit", 0 } } })
  end
  K.TextBox(WINDOW, "CampsiteDescription", 0, descH, { empty = "Select a kit to read about it." })
  ui.Dummy(0, 2 * s)
  W.ButtonRow(WINDOW, { { "RefreshListButton", "Refresh List", { tooltip = "Ask for the campsite kits again" } } }, bh)
end

local OPTS = {
  size = function() return 780, 520 end,
  resizable = { 560, 360 },
}
local function body(x, y, w, h, s)
  -- the tab box is the window's own (its model keeps no page): a tab bar of this script's
  if ui.BeginTabBar("##fpTabs") then
    local first = testTab
    testTab = nil
    if ui.BeginTabItem("Fellowship", first == 0 and ui.TabItemFlags.SetSelected or 0) then
      fellowshipPage(s)
      ui.EndTabItem()
    end
    if ui.BeginTabItem("Camp", first == 1 and ui.TabItemFlags.SetSelected or 0) then
      campPage(s)
      ui.EndTabItem()
    end
    ui.EndTabBar()
  end
end

local function testOpen() eq.ShowWindow(WINDOW, true) end
ui.window("Fellowship", function()
  if not eq.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  S.TestClicks(WINDOW)
  hud.Dialog(WINDOW, "Fellowship", OPTS, body)
end)
