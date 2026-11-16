-- Tribute Trophies (gamepad pack): stands in for TributeTrophyWnd, answered through it (lib/model.lua,
-- app/lua_ui_fellowship.cpp; the game refuses it without the House of Thule expansion). The Personal and Guild views
-- (LB / RB, the stock window's two view buttons): the trophy benefits available and active (focusing one selects it:
-- its description shows), Upgrade (X), Downgrade (Y) and Modify, the favour, cost and time left, and in the guild view
-- the guild's opt in / out. EQEmu sends no trophy tribute, so the lists stay empty.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local P = dofile("lib/textpanel.lua")
local C = T.C

local TW = "TributeTrophyWnd"
local avail, active, desc = {}, {}, { scroll = 0 }

-- A pad shortcut of the current window, not in its first frames: the press that opened it is not one for it.
-- (GPSTATE.appeared holds the frame the window appeared in, lib/window.lua)
local function pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 15 and W.pressed(b)
end

-- The two views as tabs at x0, y0; returns the bottom.
local function tabs(guild, x0, y0)
  local x = x0
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  local h = th + T.px(16)
  for i, t in ipairs({ { "Personal", "TTW_ViewPersonal_Btn" }, { "Guild", "TTW_ViewGuild_Btn" } }) do
    local on = (i == 2) == guild
    local w = ui.CalcTextSize(t[1]) + T.px(40)
    T.rect(x, y0, x + w, y0 + h, on and C.accent or C.raised, on and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + h, t[1], on and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + h) and ui.IsMouseClicked(ui.Mouse.Left) then API.windows.ClickChild(TW, t[2], "left") end
    x = x + w + T.px(10)
  end
  T.unfont()
  return y0 + h + T.px(16)
end

ui.window("Tribute Trophies", function()
  if not W.shown(TW) then return end
  T.frame()
  local _, guild = M.state(TW, "TTW_ViewGuild_Btn")
  guild = guild == true
  if W.begin("Tribute Trophies", { w = 1600, h = 960, title = "Tribute Trophies",
        hints = { { "x", "Upgrade" }, { "y", "Downgrade" }, { "lb", "Personal" }, { "rb", "Guild" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if pressed("Tribute Trophies", "lb") then API.windows.ClickChild(TW, "TTW_ViewPersonal_Btn", "left") end
    if pressed("Tribute Trophies", "rb") then API.windows.ClickChild(TW, "TTW_ViewGuild_Btn", "left") end
    if pressed("Tribute Trophies", "x") then API.windows.ClickChild(TW, "TTW_UpgradeButton", "left") end
    if pressed("Tribute Trophies", "y") then API.windows.ClickChild(TW, "TTW_DowngradeButton", "left") end
    local top = tabs(guild, x0, y0)
    -- the favour, cost and time, as the view's labels show them
    local facts
    if guild then
      facts = { { "Guild favor", M.text(TW, "TTW_GuildCurrentPointsLabel"), C.endurance },
        { "Active cost", M.text(TW, "TTW_GuildCostLabel") }, { "Time left", M.text(TW, "TTW_GuildTimeLabel") },
        { "Guild", M.text(TW, "TTW_StatusText") } }
    else
      facts = { { "Current favor", M.text(TW, "TTW_CurrentPointsLabel"), C.endurance },
        { "Career favor", M.text(TW, "TTW_TotalPointsLabel") }, { "Active cost", M.text(TW, "TTW_CostLabel") },
        { "Time left", M.text(TW, "TTW_TimeLabel") } }
    end
    M.facts(facts, x0 + (x1 - x0) * 0.40, x1, y0 - T.px(6))
    local midX = x0 + (x1 - x0) * 0.5
    ui.SetCursorPos(x0 - wx, top - wy)
    T.heading("Available trophy benefits")
    local ly = select(2, ui.GetCursorScreenPos())
    M.list(TW, "TTW_AvailableTrophyBenefitList", avail, x0, ly, midX - T.px(20), y1, { focusSelects = true, empty = "None." })
    ui.SetCursorPos(midX - wx, top - wy)
    T.heading("Active trophy benefits")
    local ay1 = top + (y1 - top) * 0.38
    M.list(TW, "TTW_ActiveTrophyBenefitList", active, midX, ly, x1, ay1, { focusSelects = true, empty = "None." })
    local bh = T.px(70)
    local rows = guild and 2 or 1
    local by = y1 - rows * bh - (rows - 1) * T.px(10)
    P.draw("##ttdesc", desc, M.text(TW, "TTW_Description"), midX, ay1 + T.px(14), x1, by - T.px(14),
      { empty = "Focus a benefit to read about it." })
    ui.SetCursorPos(midX - wx, by - wy)
    M.buttons(TW, { { "TTW_UpgradeButton", "Upgrade", { glyph = "x" } }, { "TTW_DowngradeButton", "Downgrade", { glyph = "y" } } },
      midX, x1, bh)
    if guild then
      local _, opted = M.state(TW, "TTW_StatusButton")
      M.buttons(TW, { { "TTW_LockButton", nil, { primary = true } },
        { "TTW_StatusButton", opted and "Opt Out" or "Opt In" } }, midX, x1, bh)
    end
  end
  if W.finish() then API.windows.Show(TW, false) end
end)
