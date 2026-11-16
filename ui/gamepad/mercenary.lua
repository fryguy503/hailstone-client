-- Mercenaries (gamepad pack): stands in for the three mercenary windows.
--   Mercenary Merchant  MMTW_MerchantWnd: the type (a picker), the mercenaries of that type (A picks one: the game
--                       asks for its details, as the stock list does; red needs a membership the account lacks),
--                       its stances (A picks one), on the right what it costs (purchase and upkeep), its
--                       description, the stance's, and Hire (X).
--   Manage Mercenaries  MMGW_ManageWnd, two pages (LB / RB) as the stock window's tabs: Manage (the time to the
--                       next upkeep, the stances: A switches to one, green the active one; Auto Assist, Set Active,
--                       Call Assist, Set Owner, Info, Suspend / Unsuspend, Dismiss) and Switch (your mercenaries:
--                       A selects one; Switch).
--   Mercenary Info      MIW_InfoWnd (the manager's Info): your mercenary's costs, description and stance.
-- The drop-down, lists and buttons are the stock windows' (hs.ComboSelect, hs.ListClick, hs.ClickChild), enabled
-- and labelled as the game sets them.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local P = dofile("lib/textpanel.lua")
local M = dofile("lib/markets.lua")
local C = T.C

-- A one-column list's row: its texts joined, in the row's colour.
local function plain(r, x0, y0, x1, y1, _, selected)
  local text = table.concat(r.texts, "  "):gsub("^%s+", "")
  M.row(r, x0, y0, x1, y1, text, selected and "Selected" or nil, { rightW = T.px(140), rightColor = C.accent })
end

-- A mercenary's purchase and upkeep (the PurchaseCost* / UpkeepCost* children; an alternate currency's amount as
-- well when the game shows it) as two lines at x0, y.
local function costs(win, x0, x1, y)
  local function line(caption, kind, ly)
    local pp = tonumber(API.windows.Text(win, kind .. "CostPlatText") or "") or 0
    local gp = tonumber(API.windows.Text(win, kind .. "CostGoldText") or "") or 0
    T.font(21)
    local _, th = ui.CalcTextSize("Ag")
    T.draw(x0, ly, caption, C.dim)
    local px = x0 + T.px(130)
    px = px + M.price(pp * 1000 + gp * 100, px, ly, ly + th, 0)
    if M.shown(win, "Alt" .. kind .. "CostText") then
      T.draw(px + T.px(14), ly, "or " .. (API.windows.Text(win, "Alt" .. kind .. "CostText") or ""), C.gold)
    end
    T.unfont()
    return th
  end
  local th = line("Purchase", "Purchase", y)
  line("Upkeep", "Upkeep", y + th + T.px(8))
  return 2 * th + T.px(8)
end

-- ---------------------------------------------------------------- the merchant (MMTW_MerchantWnd)
local MW = "MMTW_MerchantWnd"
local F = M.form()
local subs, stances = {}, {}
local info, sinfo = { scroll = 0 }, { scroll = 0 }

ui.window("Mercenary Merchant", function()
  if not M.visible(MW) then
    F.pick = nil
    return
  end
  M.frame()
  if W.begin("Mercenary Merchant", { w = 1600, h = 940, title = "Mercenary Merchant",
        hints = M.hints(F, { { "a", "Choose" }, { "x", "Hire" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(F, x0, y0, x1, y1) then
      if M.pressed("Mercenary Merchant", "x") then API.windows.ClickChild(MW, "HireButton", "left") end
      local mid = x0 + (x1 - x0) * 0.42
      local bh = T.px(64)
      ui.SetCursorPos(x0 - wx, y0 - wy)
      M.combo(F, MW, "TypeComboBox", "Type", mid - x0 - T.px(24), bh)
      local ly = y0 + bh + T.px(16)
      local split = ly + (y1 - ly) * 0.58
      M.caption(x0, ly, "Mercenary")
      M.list(MW, "SubtypeListBox", subs, x0, ly + T.px(28), mid - T.px(24), split - T.px(10),
        { rowH = 54, draw = plain, inspect = false, empty = "None for hire." })
      M.caption(x0, split + T.px(6), "Stance")
      M.list(MW, "StanceListBox", stances, x0, split + T.px(34), mid - T.px(24), y1,
        { rowH = 54, draw = plain, inspect = false, empty = "Pick a mercenary." })
      local rx0 = mid + T.px(24)
      local ch = costs(MW, rx0, x1, y0)
      local ty = y0 + ch + T.px(20)
      local by = y1 - bh
      local th = by - T.px(16) - ty
      P.draw("##mmtwinfo", info, API.windows.Text(MW, "InfoText") or "", rx0, ty, x1, ty + th * 0.6,
        { empty = "Pick a mercenary to read about it.", size = 19 })
      M.caption(rx0, ty + th * 0.6 + T.px(10), "Stance")
      P.draw("##mmtwstance", sinfo, API.windows.Text(MW, "StanceInfoText") or "", rx0, ty + th * 0.6 + T.px(38), x1, by - T.px(16),
        { empty = "Pick a stance to read about it.", size = 19 })
      ui.SetCursorPos(rx0 - wx, by - wy)
      M.button(MW, "HireButton", "Hire", x1 - rx0, bh, { primary = true, glyph = "x" })
    end
  end
  if M.back(F, W.finish()) then API.windows.Show(MW, false) end
end)

-- ---------------------------------------------------------------- the manager (MMGW_ManageWnd)
local GW = "MMGW_ManageWnd"
local page = 1
local mstances, owned = {}, {}

ui.window("Manage Mercenaries", function()
  if not M.visible(GW) then return end
  M.frame()
  if W.begin("Manage Mercenaries", { w = 1400, h = 900, title = "Mercenary",
        subtitle = page == 1 and ("Next upkeep in " .. (API.windows.Text(GW, "TimeLeftText") or "?")) or
          ("Slots: " .. (API.windows.Text(GW, "TotalSlotsText") or "?")),
        hints = { { "a", page == 1 and "Switch stance" or "Select" }, { "lb", "Manage" }, { "rb", "Switch" }, { "b", "Close" } } }) then
    if M.pressed("Manage Mercenaries", "lb") then page = 1 end
    if M.pressed("Manage Mercenaries", "rb") then page = 2 end
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local top = M.tabs({ "Manage", "Switch" }, page, x0, y0, function(i) page = i end)
    local mid = x0 + (x1 - x0) * 0.5
    local bh = T.px(68)
    local rx0 = mid + T.px(24)
    if page == 1 then
      M.caption(x0, top, "Stances (green: the active one)")
      M.list(GW, "StanceListBox", mstances, x0, top + T.px(28), mid - T.px(24), y1,
        { rowH = 56, draw = plain, inspect = false, empty = "No mercenary." })
      ui.SetCursorPos(rx0 - wx, top - wy)
      M.toggle(GW, "AssistModeCheckbox", "Auto Assist", x1 - rx0, bh, { off = "Off: it waits for Call Assist" })
      ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(6) - wy)
      M.buttons(GW, { { "SetActiveStanceButton", "Set Active" }, { "CallForAssistButton", "Call Assist" } }, rx0, x1, bh)
      M.buttons(GW, { { "SetOwnerButton", "Set Owner" }, { "InfoButton", "Info" } }, rx0, x1, bh)
      M.buttons(GW, { { "SuspendButton", nil }, { "DismissButton", "Dismiss" } }, rx0, x1, bh)
      local tip = API.windows.ChildTooltip and API.windows.ChildTooltip(GW, "SuspendButton") or ""
      if tip ~= "" then
        T.font(18)
        T.draw(rx0, select(2, ui.GetCursorScreenPos()) + T.px(6), T.fit(tip, x1 - rx0), C.dim)
        T.unfont()
      end
    else
      M.caption(x0, top, "Your mercenaries")
      M.list(GW, "OwnedListBox", owned, x0, top + T.px(28), mid - T.px(24), y1,
        { rowH = 56, draw = plain, inspect = false, empty = "You own no mercenary." })
      ui.SetCursorPos(rx0 - wx, top - wy)
      M.buttons(GW, { { "SwitchButton", "Switch", { primary = true } } }, rx0, x1, bh)
      T.font(18)
      T.draw(rx0, select(2, ui.GetCursorScreenPos()) + T.px(6), "Select one, then Switch to make it the active one.", C.dim)
      T.unfont()
    end
  end
  if W.finish() then API.windows.Show(GW, false) end
end)

-- ---------------------------------------------------------------- the info (MIW_InfoWnd)
local IW = "MIW_InfoWnd"
local iinfo, istance = { scroll = 0 }, { scroll = 0 }

ui.window("Mercenary Info", function()
  if not M.visible(IW) then return end
  M.frame()
  if W.begin("Mercenary Info", { w = 1100, h = 820, title = "Mercenary Info", hints = { { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local ch = costs(IW, x0, x1, y0)
    local ty = y0 + ch + T.px(20)
    local th = y1 - ty
    P.draw("##miwinfo", iinfo, API.windows.Text(IW, "InfoText") or "", x0, ty, x1, ty + th * 0.6, { empty = "No mercenary.", size = 19 })
    M.caption(x0, ty + th * 0.6 + T.px(10), "Stance")
    P.draw("##miwstance", istance, API.windows.Text(IW, "StanceInfoText") or "", x0, ty + th * 0.6 + T.px(38), x1, y1,
      { empty = "", size = 19 })
  end
  if W.finish() then API.windows.Show(IW, false) end
end)
