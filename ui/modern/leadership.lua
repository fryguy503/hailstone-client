-- Leadership: replaces the game's LeadershipWindow. Group and raid leadership side by side at the top (the points
-- to spend and the experience toward the next one), the switch for earning leadership experience, then the
-- abilities of one kind (tabs) as rows with their rank and the next rank's cost, and the selected one's description
-- with Purchase and hotkey creation. Everything is the original's children: a row click selects in the game's
-- list (LEList1 / LEList2); buttons use the window's permissions, ranks and hotkey labels.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "LeadershipWindow"

local tab = 1 -- 1 group, 2 raid (both lists are always filled: the tabs are this window's own)

local function ranks(texts)
  local cur, max = (texts[2] or ""):match("(%d+)%s*/%s*(%d+)")
  return tonumber(cur) or 0, tonumber(max) or 0
end

-- A kind's card: the name, its points and the gauge toward the next point.
local function card(x, y, w, h, s, title, points, gauge, color)
  ui.DrawRectFilled(x, y, x + w, y + h, C.ink[1], C.ink[2], C.ink[3], 0.04, 8 * s)
  local pad = 10 * s
  T.Text(x + pad, y + 8 * s, title, C.muted, T.font.small)
  local p = W.Text(WINDOW, points)
  T.Text(x + w - pad, y + 4 * s, p ~= "" and p or "0", C.gold, T.font.large, "right")
  T.Text(x + w - pad - T.TextSize(p ~= "" and p or "0", T.font.large) - 6 * s, y + 10 * s, "points", C.faint,
    T.font.tiny, "right")
  local frac, text = eq.ChildGauge(WINDOW, gauge)
  T.Bar("lead." .. gauge, x + pad, y + h - 24 * s, x + w - pad, y + h - 16 * s, s, frac or 0, color, { noTrail = true })
  T.Text(x + pad, y + h - 14 * s, "Toward the next point", C.faint, T.font.tiny)
  if text and text ~= "" then T.Text(x + w - pad, y + h - 14 * s, text, C.muted, T.font.tiny, "right") end
end

local function drawRow(_, texts, x0, y0, x1, y1, s, st)
  local cur, max = ranks(texts)
  local pad = 10 * s
  local midY = (y0 + y1) / 2
  local _, th = T.TextSize("Ag", T.font.small)
  local owned = cur > 0
  T.Text(x0 + pad, midY - th / 2, T.Fit(texts[1] or "", x1 - x0 - 190 * s, T.font.small), owned and C.text or C.muted,
    T.font.small)
  -- rank pips
  local px = x1 - 150 * s
  for i = 1, max do
    local bx = px + (i - 1) * 14 * s
    if i <= cur then ui.DrawRectFilled(bx, midY - 2 * s, bx + 11 * s, midY + 2 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s)
    else ui.DrawRectFilled(bx, midY - 2 * s, bx + 11 * s, midY + 2 * s, C.ink[1], C.ink[2], C.ink[3], 0.12, 2 * s) end
  end
  local cost = texts[3] or "-"
  if cost == "-" or cost == "" then
    T.Text(x1 - pad, midY - 6 * s, max > 0 and cur >= max and "MAX" or "-", C.accent, T.font.tiny, "right")
  else
    T.Text(x1 - pad, midY - 5 * s, "pt", C.faint, T.font.tiny, "right")
    T.Text(x1 - pad - T.TextSize("pt", T.font.tiny) - 3 * s, midY - th / 2, cost, C.gold, T.font.small, "right")
  end
  local _ = st
end

local WINDOW_OPTS = {
  size = function() return 560, 520 end,
  resizable = { 400, 400 },
}
local function drawWindow(x, y, w, h, s)
  -- the two kinds
  local cardH = 64 * s
  local half = (w - 8 * s) / 2
  card(x, y, half, cardH, s, "Group leadership", "LEW_CurrentGroupCount", "LEW_GroupExpGauge", C.accent)
  card(x + half + 8 * s, y, half, cardH, s, "Raid leadership", "LEW_CurrentRaidCount", "LEW_RaidExpGauge", C.xp)
  -- earning leadership experience
  local ry = y + cardH + 8 * s
  T.Text(x, ry + 3 * s, "Earn leadership experience", C.muted, T.font.small)
  local bw = 52 * s
  ui.SetCursorScreenPos(x + w - 2 * bw - 4 * s, ry)
  W.Button(WINDOW, "LEW_OnButton", "On", { width = bw, tooltip = "Part of your experience goes to leadership" })
  ui.SameLine(0, 4 * s)
  W.Button(WINDOW, "LEW_OffButton", "Off", { width = bw, tooltip = "Leadership earns no experience" })
  ui.SetCursorScreenPos(x, ry + 28 * s)
  -- the abilities of one kind
  if ui.BeginTabBar("##leadtabs") then
    if ui.BeginTabItem("Group abilities") then tab = 1; ui.EndTabItem() end
    if ui.BeginTabItem("Raid abilities") then tab = 2; ui.EndTabItem() end
    ui.EndTabBar()
  end
  local list = tab == 1 and "LEList1" or "LEList2"
  local _, cy = ui.GetCursorScreenPos()
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local descH = math.max(64 * s, (y + h - cy) * 0.24)
  local listH = y + h - cy - descH - 2 * btnH - 24 * s
  P.Rows(WINDOW, list, 0, listH, {
    rowH = 28,
    draw = drawRow,
    empty = "No abilities",
  })
  -- the selected one
  local _, dy = ui.GetCursorScreenPos()
  ui.DrawRectFilled(x, dy + 4 * s, x + w, dy + 4 * s + descH, C.ink[1], C.ink[2], C.ink[3], 0.025, 8 * s)
  ui.SetCursorScreenPos(x + 10 * s, dy + 10 * s)
  if W.Text(WINDOW, "LEW_Description") == "" then
    P.Empty(w - 20 * s, descH - 12 * s, "Select an ability", "Its rank and the next rank's cost show here")
  else
    W.Stml(WINDOW, "LEW_Description", w - 20 * s, descH - 12 * s)
  end
  ui.SetCursorScreenPos(x, y + h - 2 * btnH - 8 * s)
  local hw = (w - 12 * s) / 3
  for n = 1, 3 do
    if n > 1 then ui.SameLine(0, 6 * s) end
    local id = "LEW_HotkeyButton" .. n
    W.Button(WINDOW, id, "Create Hotkey " .. n, { width = hw, height = btnH,
      tooltip = eq.ChildTooltip(WINDOW, id) or "Create a hotkey for the selected ability, then place it on a hot bar" })
  end
  ui.SetCursorScreenPos(x, y + h - btnH)
  local mw = 150 * s
  W.Button(WINDOW, "LEW_PurchaseButton", "Purchase", { primary = true, width = w - mw - 6 * s, height = btnH,
    tooltip = "Buy the next rank of the selected ability" })
  ui.SameLine(0, 6 * s)
  -- the game's Group Mentoring button: for the group's leader, it opens the mentoring window
  W.Button(WINDOW, "LEW_GroupMentoring", "Group Mentoring", { width = mw, height = btnH,
    tooltip = "Give part of your experience to a group member" })
end

ui.window("Leadership", function()
  if not eq.InGame() then return end
  hud.Dialog(WINDOW, "Leadership", WINDOW_OPTS, drawWindow)
end)
