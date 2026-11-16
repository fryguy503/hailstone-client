-- Tribute trophies: replaces the game's TributeTrophyWnd (House of Thule's trophy benefits). Personal or Guild (the
-- original's two radio buttons, as tabs of chips), the view's numbers as tiles (active cost, time left, favour,
-- career favour; the guild's with whether you are opted in), the available and active benefits side by side, or
-- stacked when narrow, the selected one's description, and Upgrade / Downgrade / Modify. Which numbers and buttons
-- show is the window's (the personal or guild ones); the numbers are its labels, else their EQTypes (297..300, 117,
-- 118, 122), and every list and button the original's (app/lua_ui_fellowship.cpp). EQEmu sends no trophy tribute,
-- so the lists stay empty there.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local D = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "TributeTrophyWnd"
local DRAW_KEY = "test.draw." .. WINDOW -- testing: draws the window though the game has not opened it

-- a label child's text (the window's), else its EQType's
local function value(id, eqtype)
  local t = W.Text(WINDOW, id)
  if t == "" and eqtype then t = API.character.Label(eqtype) or "" end
  return t
end

-- a view radio button as a chip-like toggle
local function view(id, label, tip)
  local _, on = W.State(WINDOW, id)
  if on then
    ui.PushStyleColor(ui.Col.Button, C.accent[1], C.accent[2], C.accent[3], 0.40)
  else
    ui.PushStyleColor(ui.Col.Button, C.ink[1], C.ink[2], C.ink[3], 0.06)
  end
  if ui.Button(label .. "##" .. id) and not on then API.windows.ClickChild(WINDOW, id, "left") end
  ui.PopStyleColor(1)
  if ui.IsItemHovered() then ui.SetTooltip(tip) end
end

local function benefits(id, caption, w, h, s, empty)
  ui.BeginGroup()
  K.Heading(caption, nil)
  local rows = API.windows.ListInfo(WINDOW, id) or 0
  local x, y = ui.GetCursorScreenPos()
  W.List(WINDOW, id, w, h, {})
  if rows == 0 then
    local _, lh = ui.CalcTextSize("Ag")
    T.Text(x + 8 * s, y + lh + 10 * s, empty, C.faint, T.font.small)
  end
  ui.EndGroup()
end

local function testOpen() API.windows.Show(WINDOW, true) end
local OPTS = {
  size = function() return 640, 500 end,
  resizable = { 380, 360 },
}
local function body(x, y, w, h, s)
  local bh = D.ButtonH(s)
  view("TTW_ViewPersonal_Btn", "Personal Trophies", "Your own trophy benefits")
  ui.SameLine()
  view("TTW_ViewGuild_Btn", "Guild Trophies", "Your guild's trophy benefits")
  if W.Shown(WINDOW, "TTW_StatusButton") then
    ui.SameLine(0, 16 * s)
    W.Check(WINDOW, "TTW_StatusButton", "Opted in", { tooltip = "Whether you take part in the guild's trophies" })
  end
  ui.Dummy(0, 2 * s)
  local guild = W.Shown(WINDOW, "TTW_GuildCostLabel")
  local tiles
  if guild then
    tiles = {
      { "Active cost", value("TTW_GuildCostLabel", 300), C.text, "Guild favour paid for each period" },
      { "Time left", value("TTW_GuildTimeLabel", 299), C.text, "Time until the next payment" },
      { "Guild favour", value("TTW_GuildCurrentPointsLabel", 122), C.gold, "The guild's favour to spend" },
    }
  else
    tiles = {
      { "Active cost", value("TTW_CostLabel", 298), C.text, "Favour paid for each period" },
      { "Time left", value("TTW_TimeLabel", 297), C.text, "Time until the next payment" },
      { "Favour", value("TTW_CurrentPointsLabel", 117), C.gold, "Tribute favour you can spend" },
      { "Career favour", value("TTW_TotalPointsLabel", 118), C.text, "All the favour you have earned" },
    }
  end
  K.StatRow(tiles, 100)
  ui.Dummy(0, 2 * s)

  local _, restH = ui.GetContentRegionAvail()
  local descH = 80 * s
  local wide = w >= 520 * s
  local _, lh = ui.CalcTextSize("Ag")
  local headH = lh + 8 * s
  local listsH = (restH or 300) - descH - bh - 24 * s
  if wide then
    local lw = (w - 12 * s) / 2
    benefits("TTW_AvailableTrophyBenefitList", "Available benefits", lw, listsH - headH, s, "No trophy benefits.")
    ui.SameLine(0, 12 * s)
    benefits("TTW_ActiveTrophyBenefitList", "Active benefits", lw, listsH - headH, s, "None active.")
  else
    local each = listsH / 2 - headH
    benefits("TTW_AvailableTrophyBenefitList", "Available benefits", 0, each, s, "No trophy benefits.")
    benefits("TTW_ActiveTrophyBenefitList", "Active benefits", 0, each, s, "None active.")
  end
  ui.Dummy(0, 2 * s)
  D.TextBox(WINDOW, "TTW_Description", 0, descH, { empty = "Select a benefit to read what it does." })
  W.ButtonRow(WINDOW, {
    { "TTW_UpgradeButton", "Upgrade", { primary = true, tooltip = "Activate or upgrade the selected benefit" } },
    { "TTW_DowngradeButton", "Downgrade", { tooltip = "Deactivate or downgrade the selected benefit" } },
    { "TTW_LockButton", nil, { tooltip = "Begin changing the active benefits (needs the guild's trophy permission)" } },
  }, bh)
end

ui.window("Tribute Trophies", function()
  P.TestOpen(WINDOW, testOpen) -- testing: test.open.TributeTrophyWnd=1
  if not API.session.InGame() then return end
  OPTS.shown = T.Setting(DRAW_KEY) == "1" or nil -- testing: test.draw.TributeTrophyWnd=1
  hud.Dialog(WINDOW, "Tribute Trophies", OPTS, body)
end)
