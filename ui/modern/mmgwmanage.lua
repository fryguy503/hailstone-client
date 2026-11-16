-- Mercenary manager: replaces the game's MMGW_ManageWnd. Two tabs as the stock window has: Manage (the time to the
-- next upkeep, the stances with the active one in green, Auto Assist, and the commands: Set Active, Call Assist,
-- Set Owner, Info, Suspend / Unsuspend, Dismiss) and Switch (the mercenaries you own, the current one marked, and
-- Switch). Lists, check box and buttons are the original's children: a stance row click changes the stance at once,
-- as on the stock list, and each button is enabled, labelled and explained (its tooltip) as the game sets it.
-- (The tabs are this window's own: the game fills both pages whichever is shown.)
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "MMGW_ManageWnd"

local page = 0

-- A list child as plain selectable rows in their colours (no header); `columns` texts joined.
local function list(id, h, empty, tip)
  local rows, selected = M.Rows(WINDOW, id)
  local enabled = W.State(WINDOW, id)
  ui.PushID(id)
  ui.BeginChild("##l", 0, h, true)
  ui.BeginDisabled(not enabled)
  for i, r in ipairs(rows) do
    local c = r.color or C.text
    local text = table.concat(r.texts, "  "):gsub("^%s+", "")
    ui.PushStyleColor(ui.Col.Text, c[1], c[2], c[3], 1)
    if ui.Selectable(text .. "##" .. i, i - 1 == selected) then API.windows.ListClick(WINDOW, id, i - 1, "left") end
    ui.PopStyleColor(1)
    if tip then M.Tip(tip) end
  end
  ui.EndDisabled()
  if #rows == 0 then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, empty) end
  ui.EndChild()
  ui.PopID()
end

local function tip(id, fallback)
  local t = API.windows.ChildTooltip(WINDOW, id)
  return (t and t ~= "") and t or fallback
end

local function managePage(w, h, s, lh, btnH)
  W.Field("Time left", W.Text(WINDOW, "TimeLeftText"))
  M.Tip(tip("TimeLeftText", "Until the next upkeep"))
  M.Caption("Stances")
  list("StanceListBox", -(btnH * 4 + 26 * s), "No mercenary.", "Click a stance to switch to it (green: the active one)")
  W.Check(WINDOW, "AssistModeCheckbox", "Auto Assist", { tooltip = "Your mercenary assists you on its own" })
  W.ButtonRow(WINDOW, {
    { "SetActiveStanceButton", "Set Active", { tooltip = "Use the selected stance" } },
    { "CallForAssistButton", "Call Assist", { tooltip = "Have your mercenary assist you now" } },
  })
  W.ButtonRow(WINDOW, {
    { "SetOwnerButton", "Set Owner", { tooltip = "Make this character the mercenary's owner" } },
    { "InfoButton", "Info", { tooltip = "The mercenary's costs and description" } },
  })
  W.ButtonRow(WINDOW, {
    { "SuspendButton", nil, { tooltip = tip("SuspendButton", "Suspend or unsuspend your mercenary") } },
    { "DismissButton", "Dismiss", { danger = true, tooltip = "Let your mercenary go" } },
  }, btnH)
end

local function switchPage(w, h, s, lh, btnH)
  W.Field("Slots", W.Text(WINDOW, "TotalSlotsText"))
  M.Tip("How many mercenaries you can own")
  M.Caption("Your mercenaries")
  list("OwnedListBox", -(btnH + 8 * s), "You own no mercenary.", "Select one, then Switch (marked: the current one)")
  W.Button(WINDOW, "SwitchButton", "Switch", { primary = true, width = -1, height = btnH,
    tooltip = "Make the selected mercenary your active one" })
end

local OPTS = {
  size = function() return 300, 420 end,
  resizable = { 240, 320 },
}
local function body(x, y, w, h, s)
  local lh, btnH = M.Heights(s)
  if ui.BeginTabBar("##mmgwtabs") then
    if ui.BeginTabItem("Manage") then page = 0; ui.EndTabItem() end
    if ui.BeginTabItem("Switch") then page = 1; ui.EndTabItem() end
    ui.EndTabBar()
  end
  local _, top = ui.GetCursorScreenPos()
  local rest = h - (top - y)
  if page == 0 then managePage(w, rest, s, lh, btnH) else switchPage(w, rest, s, lh, btnH) end
end

ui.window("Manage Mercenaries", function()
  if not API.session.InGame() then return end
  M.TestOpen(WINDOW)
  hud.Dialog(WINDOW, "Mercenary", OPTS, body)
end)
