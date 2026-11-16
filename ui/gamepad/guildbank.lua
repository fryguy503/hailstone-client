-- Guild bank (gamepad pack): stands in for GuildBankWnd. Two pages, LB / RB:
--   Bank      the bank's items (the window's filter as a picker, the free slots): focusing one selects it, A
--             withdraws it, Y views it; Withdraw, View, Split (the game's quantity dialog), Merge, the item's
--             permission (a picker: Single Member asks for the name) and Change Member
--   Deposits  the deposits waiting for a banker (A selects one, Put in Bank moves it into the bank) and your
--             bags: A picks an item up, Deposit (X) gives the held item
-- One selection across both lists, as the window keeps it; buttons a member may not use are greyed by the game.
-- Everything is the stock window's (hs.ListClick, hs.ComboSelect, hs.ClickChild).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local X = dofile("lib/exchange.lua")
local M = dofile("lib/markets.lua")
local C = T.C

local GW = "GuildBankWnd"
local F = M.form()
local page = 1
local items, deposits = {}, {}
local nav, bags = {}, {}

local function row(r, x0, y0, x1, y1)
  local qty = (r.texts[3] or ""):match("%d+")
  M.row(r, x0, y0, x1, y1, (r.texts[2] or "") .. (qty and ("  x" .. qty) or ""), r.texts[4], { rightW = T.px(300) })
end

local function free(x1, y, id)
  local t = API.windows.Text(GW, id) or ""
  local n = tonumber(t:match("(%d+)"))
  T.font(19)
  local tw = ui.CalcTextSize(t)
  T.draw(x1 - tw, y, t, n == 0 and C.bad or C.dim)
  T.unfont()
end

ui.window("Guild Bank", function()
  if not M.visible(GW) then
    F.pick = nil
    return
  end
  M.frame()
  local ctx = X.frame(nav, {})
  local hints = page == 1 and { { "a", "Withdraw" }, { "y", "View" }, { "lb", "Bank" }, { "rb", "Deposits" }, { "b", "Close" } }
    or { { "a", "Select / pick up" }, { "x", "Deposit" }, { "lb", "Bank" }, { "rb", "Deposits" }, { "b", "Close" } }
  if W.begin("Guild Bank", { w = 1640, h = 960, title = "Guild Bank", hints = M.hints(F, hints) }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(F, x0, y0, x1, y1) then
      if M.pressed("Guild Bank", "lb") then page = 1 end
      if M.pressed("Guild Bank", "rb") then page = 2 end
      local top = M.tabs({ "Bank", "Deposits" }, page, x0, y0, function(i) page = i end)
      local rx0 = x1 - T.px(460)
      local bh = T.px(64)
      if page == 1 then
        free(rx0 - T.px(24), y0 + T.px(8), "BankCountLabel")
        local _, sel = M.list(GW, "ItemList", items, x0, top, rx0 - T.px(24), y1, {
          rowH = 60, select = "focus", inspect = false, empty = "The bank is empty (or the filter hides it all).",
          draw = row,
          onA = function() API.windows.ClickChild(GW, "WithdrawButton", "left") end })
        if sel and W.pressed("y") then API.windows.ClickChild(GW, "ViewItemButton", "left") end
        ui.SetCursorPos(rx0 - wx, top - wy)
        M.combo(F, GW, "FilterCombo", "Show", x1 - rx0, bh)
        ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
        M.caption(rx0, select(2, ui.GetCursorScreenPos()), "Selected")
        T.font(22)
        T.draw(rx0, select(2, ui.GetCursorScreenPos()) + T.px(28), T.fit(sel and sel.texts[2] or "-", x1 - rx0), sel and C.text or C.faint)
        T.unfont()
        ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(70) - wy)
        M.buttons(GW, { { "WithdrawButton", "Withdraw", { primary = true } } }, rx0, x1, bh)
        M.buttons(GW, { { "ViewItemButton", "View" }, { "SplitButton", "Split", { afterA = true } }, { "MergeButton", "Merge" } }, rx0, x1, bh)
        ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
        M.combo(F, GW, "PermissionCombo", "Permission", x1 - rx0, bh)
        ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
        M.buttons(GW, { { "ChangeTargetButton", "Change Member" } }, rx0, x1, bh)
      else
        free(x0 + (x1 - x0) * 0.45 - T.px(24), y0 + T.px(8), "DepositCountLabel")
        local mid = x0 + (x1 - x0) * 0.45
        M.list(GW, "DepositList", deposits, x0, top, mid - T.px(24), y1 - bh - T.px(14), {
          rowH = 60, empty = "No deposits waiting.", draw = row })
        ui.SetCursorPos(x0 - wx, y1 - bh - wy)
        M.buttons(GW, { { "PromoteButton", "Put in Bank" } }, x0, mid - T.px(24), bh)
        -- your bags: the item to deposit
        local bx0 = mid + T.px(24)
        local per = math.max(1, math.floor((x1 - bx0 + T.px(X.GAP)) / T.px(X.SIZE + X.GAP)))
        ui.SetCursorPos(bx0 - wx, top - wy)
        X.bags(ctx, bags, bx0, x1 - bx0, per, { put = function() API.windows.ClickChild(GW, "DepositButton", "left") end })
        local hy = y1 - bh - T.px(14) - X.holdingHeight(ctx)
        X.holding(ctx, bx0, hy, x1, "Deposit (X) gives it to the guild")
        ui.SetCursorPos(bx0 - wx, y1 - bh - wy)
        M.buttons(GW, { { "DepositButton", ctx.held and "Deposit the held item" or "Deposit", { primary = true, glyph = "x" } } }, bx0, x1, bh)
      end
    end
  end
  if M.back(F, W.finish()) then API.windows.Show(GW, false) end
end)
