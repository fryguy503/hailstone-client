-- Real estate (gamepad pack): stands in for the game's five real estate windows, on a server that has housing.
--   Plot           the plot's facts, the price and upkeep pickers, Purchase / Sell / Transfer, the opt-out toggle,
--                  Items and Manage
--   Manage         two pages (LB / RB): Access (a group's thirteen permissions as toggles, the group picked across the
--                  top; the members with Add / Remove and their group) and Escrow (balances, upkeep, time left; Deposit
--                  the held coin, Withdraw the selected balance, Add Day / Week, Withdraw All)
--   Items          the items where you stand: A selects, Y opens the item's menu (the server's), and the moves
--   Neighborhoods  the neighborhoods (A selects one, showing its players and guilds), Go, My Plots, My Items
--   Plot Search    an owner name (the keyboard), Search, the plots (A selects), Go
-- Everything is the stock window's (hs.ClickChild, hs.ListClick, hs.ComboSelect, hs.SetWindowText).
local API = hs.UI
local T = dofile("lib/theme.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/markets.lua")
local C = T.C

local function plain(r, x0, y0, x1, y1)
  M.row(r, x0, y0, x1, y1, r.texts[1] or "", r.texts[2])
end

-- ---------------------------------------------------------------- the plot
local PW = "RealEstatePurchaseWnd"
local PF = M.form()
ui.window("Real Estate Plot", function()
  if not M.visible(PW) then
    PF.pick = nil
    return
  end
  M.frame()
  if W.begin("Real Estate Plot", { w = 1100, h = 900, title = "Plot", hints = M.hints(PF, { { "a", "Choose" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(PF, x0, y0, x1, y1) then
      local t = function(id) return API.windows.Text(PW, id) or "" end
      M.facts({ { "Address", t("AddressLabel") }, { "Owner", t("OwnerLabel") }, { "Kind", t("TypeLabel") } }, x0, x1, y0)
      M.facts({ { "Items", t("CapacityLabel") }, { "Vault", t("VaultLabel") }, { "Pets", t("PetLabel") },
        { "Category", t("CategoryLabel") } }, x0, x1, y0 + T.px(80))
      local bh = T.px(64)
      ui.SetCursorPos(x0 - wx, y0 + T.px(170) - wy)
      M.combo(PF, PW, "PriceCombo", "Price", x1 - x0, bh)
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
      M.combo(PF, PW, "UpkeepCombo", "Upkeep", x1 - x0, bh)
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
      M.toggle(PW, "OptOutCheck", "Opt out of votes", x1 - x0, bh)
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(20) - wy)
      M.buttons(PW, { { "PurchaseButton", "Purchase", { primary = true } }, { "SellButton", "Sell" },
        { "TransferButton", "Transfer" } }, x0, x1, bh)
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
      M.buttons(PW, { { "ItemsButton", "Items", always = true }, { "ManageButton", "Manage", always = true } }, x0, x1, bh)
    end
  end
  if M.back(PF, W.finish()) then API.windows.Show(PW, false) end
end)

-- ---------------------------------------------------------------- manage
local MW = "RealEstateManageWnd"
local MF = M.form()
local GROUPS = { "Everyone", "Visitor", "Acquaint.", "Friend", "Trustee", "Co-Owner", "Reserved" }
local BITS = { "Buffs", "Enter", "Withdraw upkeep", "Remove crate", "Add crate", "Remove house", "Add house",
  "Mail item", "Remove item", "Add item", "Move item", "Interact", "Manage" }
local mpage, group = 1, 1
local members, balances, upkeep = {}, {}, {}

ui.window("Real Estate Manage", function()
  if not M.visible(MW) then
    MF.pick = nil
    return
  end
  M.frame()
  if W.begin("Real Estate Manage", { w = 1500, h = 960, title = "Manage Plot",
    hints = M.hints(MF, { { "a", "Choose" }, { "lb", "Access" }, { "rb", "Escrow" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(MF, x0, y0, x1, y1) then
      if M.pressed("Real Estate Manage", "lb") then mpage = 1 end
      if M.pressed("Real Estate Manage", "rb") then mpage = 2 end
      local top = M.tabs({ "Access", "Escrow" }, mpage, x0, y0, function(i) mpage = i end)
      local bh = T.px(60)
      local mid = x0 + (x1 - x0) * 0.5
      if mpage == 1 then
        local gt = M.tabs(GROUPS, group, x0, top, function(i) group = i end)
        -- the group's permissions: two columns of toggles
        local colW = (mid - x0 - T.px(34)) / 2
        for b = 1, #BITS do
          local col = (b - 1) % 2
          local row = math.floor((b - 1) / 2)
          ui.SetCursorPos(x0 + col * (colW + T.px(10)) - wx, gt + row * (bh + T.px(8)) - wy)
          M.toggle(MW, string.format("Perm_%d_%d", group - 1, b - 1), BITS[b], colW, bh)
        end
        M.caption(mid, gt, "Members")
        M.list(MW, "MemberList", members, mid, gt + T.px(30), x1, y1 - 2 * bh - T.px(30), {
          rowH = 54, empty = "Only you.", draw = plain })
        ui.SetCursorPos(mid - wx, y1 - 2 * bh - T.px(14) - wy)
        M.combo(MF, MW, "MemberGroupCombo", "Group", x1 - mid, bh)
        ui.SetCursorPos(mid - wx, y1 - bh - wy)
        M.buttons(MW, { { "AddMemberButton", "Add", always = true }, { "RemoveMemberButton", "Remove", always = true } }, mid, x1, bh)
      else
        local t = API.windows.Text(MW, "TimeLeftLabel") or ""
        M.facts({ { "Time left", t, t == "Expired" and C.bad or nil } }, x0, mid, top)
        M.caption(x0, top + T.px(80), "Balance")
        M.list(MW, "BalanceList", balances, x0, top + T.px(110), mid - T.px(20), y1 - 2 * bh - T.px(30), {
          rowH = 54, empty = "Nothing in escrow.", draw = plain })
        M.caption(mid, top + T.px(80), "Daily upkeep")
        M.list(MW, "UpkeepList", upkeep, mid, top + T.px(110), x1, y1 - 3 * bh - T.px(40), {
          rowH = 54, empty = "No upkeep.", draw = function(r, rx0, ry0, rx1, ry1)
            M.row(r, rx0, ry0, rx1, ry1, (r.texts[1] or "") .. "  " .. (r.texts[2] or ""), r.texts[3])
          end })
        ui.SetCursorPos(mid - wx, y1 - 3 * bh - T.px(24) - wy)
        M.combo(MF, MW, "UpkeepCombo", "Upkeep plan", x1 - mid, bh)
        ui.SetCursorPos(x0 - wx, y1 - 2 * bh - T.px(10) - wy)
        M.buttons(MW, { { "DepositButton", "Deposit", { primary = true }, always = true },
          { "WithdrawButton", "Withdraw", { afterA = true }, always = true },
          { "WithdrawAllButton", "Withdraw All", always = true } }, x0, x1, bh)
        ui.SetCursorPos(x0 - wx, y1 - bh - wy)
        M.buttons(MW, { { "AddDayButton", "Add Day", always = true }, { "AddWeekButton", "Add Week", always = true },
          { "TransferButton", "Transfer", always = true } }, x0, x1, bh)
      end
    end
  end
  if M.back(MF, W.finish()) then API.windows.Show(MW, false) end
end)

-- ---------------------------------------------------------------- items
local IW = "RealEstateItemsWnd"
local IF = M.form()
local items = {}
ui.window("Real Estate Items", function()
  if not M.visible(IW) then return end
  M.frame()
  if W.begin("Real Estate Items", { w = 1400, h = 900, title = "Real Estate Items",
    hints = M.hints(IF, { { "a", "Select" }, { "y", "Item menu" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(60)
    local rx0 = x1 - T.px(420)
    -- Y right-clicks the focused row: the server's item menu.
    M.list(IW, "ItemList", items, x0, y0, rx0 - T.px(24), y1, {
      rowH = 60, empty = "No items here.", draw = function(r, ax0, ay0, ax1, ay1)
        M.row(r, ax0, ay0, ax1, ay1, r.texts[2] or "", (r.texts[3] or "") .. " · " .. (r.texts[4] or ""))
      end })
    ui.SetCursorPos(rx0 - wx, y0 - wy)
    for _, b in ipairs({ { "InventoryButton", "To Inventory" }, { "ClosetButton", "To Closet" }, { "ShedButton", "To Shed" },
      { "CrateButton", "To Crate" }, { "InspectButton", "Inspect" }, { "StowButton", "Store All" },
      { "PackButton", "Pack Crate" }, { "UnpackButton", "Unpack Crate" } }) do
      M.buttons(IW, { { b[1], b[2], always = true } }, rx0, x1, bh)
      ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(6) - wy)
    end
  end
  if W.finish() then API.windows.Show(IW, false) end
end)

-- ---------------------------------------------------------------- neighborhoods
local NW = "RealEstateNeighborhoodWnd"
local hoods, nplayers, nguilds = {}, {}, {}
ui.window("Real Estate Neighborhoods", function()
  if not M.visible(NW) then return end
  M.frame()
  if W.begin("Real Estate Neighborhoods", { w = 1400, h = 900, title = "Neighborhoods",
    hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(60)
    local mid = x0 + (x1 - x0) * 0.55
    M.list(NW, "NeighborhoodList", hoods, x0, y0, mid - T.px(20), y1 - bh - T.px(20), {
      rowH = 60, inspect = false, empty = "No neighborhoods listed.", draw = function(r, ax0, ay0, ax1, ay1)
        M.row(r, ax0, ay0, ax1, ay1, r.texts[1] or "", "P " .. (r.texts[3] or "") .. "  G " .. (r.texts[4] or ""),
          { sub = r.texts[2] })
      end })
    local half = y0 + (y1 - y0 - bh) * 0.5
    M.caption(mid, y0, "Players")
    M.list(NW, "PlayerList", nplayers, mid, y0 + T.px(30), x1, half - T.px(10), { rowH = 48, inspect = false, draw = plain })
    M.caption(mid, half, "Guilds")
    M.list(NW, "GuildList", nguilds, mid, half + T.px(30), x1, y1 - bh - T.px(20), { rowH = 48, inspect = false, draw = plain })
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    M.buttons(NW, { { "GoButton", "Go", { primary = true }, always = true }, { "MyPlotsButton", "My Plots", always = true },
      { "MyItemsButton", "My Items", always = true } }, x0, x1, bh)
  end
  if W.finish() then API.windows.Show(NW, false) end
end)

-- ---------------------------------------------------------------- plot search
local SW = "RealEstatePlotSearchWnd"
local SF = M.form()
local plots = {}
ui.window("Real Estate Plot Search", function()
  if not M.visible(SW) then
    SF.pick = nil
    return
  end
  M.frame()
  if W.begin("Real Estate Plot Search", { w = 1300, h = 900, title = "Plot Search",
    hints = M.hints(SF, { { "a", "Select" }, { "x", "Go" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(SF, x0, y0, x1, y1) then
      local bh = T.px(64)
      local bw = T.px(260)
      ui.SetCursorPos(x0 - wx, y0 - wy)
      M.field(SF, SW, "NameEdit", "Owner", x1 - x0 - bw - T.px(10), bh, { hint = "Every plot",
        done = function() API.windows.ClickChild(SW, "SearchButton", "left") end })
      ui.SameLine(0, T.px(10))
      M.button(SW, "SearchButton", "Search", bw, bh)
      M.list(SW, "PlotList", plots, x0, y0 + bh + T.px(16), x1, y1 - bh - T.px(20), {
        rowH = 60, inspect = false, empty = "No plots found.", draw = function(r, ax0, ay0, ax1, ay1)
          M.row(r, ax0, ay0, ax1, ay1, r.texts[4] or "", r.texts[3], { sub = r.texts[2] })
        end, onA = function() end })
      if W.pressed("x") then API.windows.ClickChild(SW, "GoButton", "left") end
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      M.buttons(SW, { { "GoButton", "Go", { primary = true, glyph = "x" }, always = true } }, x0, x1, bh)
    end
  end
  if M.back(SF, W.finish()) then API.windows.Show(SW, false) end
end)
