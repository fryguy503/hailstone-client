-- Barter (gamepad pack): stands in for the barter windows.
--   Barter Buyer     BarterWnd (/buyer: your own buy lines), three pages (LB / RB) as the steps of a buy line:
--                    Item   the item to buy: its name typed on the keyboard, Search, the results (A picks one)
--                    Offer  the price each as four coin buttons (the game's amount dialog), how many, the items you
--                           give as well (your items: A picks one, Add; the offer's: Remove), Create Buy Line or
--                           Modify (the selected line)
--                    Lines  your buy lines (A selects one: its values go back into the offer to change them),
--                           Remove, Turn On / Off, the status text with List Lines and Clear, the welcome message
--                    Start / Stop Buyer (X) and the buying state head every page.
--   Barter Merchant  BarterMerchantWnd (a buyer's window, in the barter hall): the buyer's buy lines (focusing one
--                    selects it), what it gives besides coins, Sell (A: the game's amount dialog).
--   Barter Search    BarterSearchWnd (/barter: buyers for your items): your items (A puts the name in the search),
--                    the name typed, the buyers to ask (all or this zone's), Search (X), the matching buy lines
--                    (focusing one selects it, A sells to it), what it gives besides coins, Find Buyer, Greeting,
--                    Welcome and the inspect buttons the game shows.
-- Every list, field and button is the stock window's (hs.ListClick, hs.SetWindowText, hs.ComboSelect,
-- hs.ClickChild): the rules and messages are the game's.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local P = dofile("lib/textpanel.lua")
local M = dofile("lib/markets.lua")
local C = T.C

local GREEN = C.good

-- An item row: icon, name (texts[n]), "x<qty>" (texts[q]) on the right.
local function itemRow(n, q)
  return function(r, x0, y0, x1, y1)
    local qty = q and (r.texts[q] or ""):match("%d+")
    M.row(r, x0, y0, x1, y1, r.texts[n], qty and ("x" .. qty) or nil, { rightW = T.px(110) })
  end
end

-- ---------------------------------------------------------------- the buyer (BarterWnd)
local BW = "BarterWnd"
local F = M.form()
local page = 1
local search, inv, comp, lines = {}, {}, {}, {}
local status = { scroll = 0 }

local function startStop(x, y, w, h)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy)
  local buying = (API.windows.Text(BW, "StartStopButton") or ""):find("Stop") ~= nil
  M.button(BW, "StartStopButton", buying and "Stop Buyer" or "Start Buyer", w, h, { primary = not buying, glyph = "x" })
end

ui.window("Barter Buyer", function()
  if not M.visible(BW) then
    F.kb.open, F.pick = false, nil
    return
  end
  M.frame()
  local buying = (API.windows.Text(BW, "StartStopButton") or ""):find("Stop") ~= nil
  local hints = M.hints(F, { { "a", "Select" }, { "x", buying and "Stop Buyer" or "Start Buyer" },
    { "lb", "Previous" }, { "rb", "Next" }, { "b", "Close" } })
  if W.begin("Barter Buyer", { w = 1760, h = 1000, title = "Barter Buyer",
        subtitle = buying and "Sellers can fill your active buy lines" or "Set up buy lines, then start buying", hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(F, x0, y0, x1, y1) then
      if M.pressed("Barter Buyer", "lb") then page = math.max(1, page - 1) end
      if M.pressed("Barter Buyer", "rb") then page = math.min(3, page + 1) end
      if M.pressed("Barter Buyer", "x") then API.windows.ClickChild(BW, "StartStopButton", "left") end
      local top = M.tabs({ "1  Item", "2  Offer", "3  Buy lines" }, page, x0, y0, function(i) page = i end)
      local bh = T.px(64)
      local mid = x0 + (x1 - x0) * 0.5
      if page == 1 then
        -- the item to buy
        ui.SetCursorPos(x0 - wx, top - wy)
        local fw = mid - x0 - T.px(24)
        M.field(F, BW, "ItemSearchEdit", "Item name", fw * 0.7, bh, { hint = "Type the item to buy",
          done = function() API.windows.ClickChild(BW, "ItemSearchButton", "left") M.refresh(BW, "ItemSearchList") end })
        ui.SameLine(0, T.px(10))
        M.button(BW, "ItemSearchButton", "Search", fw * 0.3 - T.px(10), bh, { primary = true })
        local _, sel = M.list(BW, "ItemSearchList", search, x0, top + bh + T.px(16), mid - T.px(24), y1, {
          rowH = 60, draw = itemRow(2), empty = "Search for the item you want to buy." })
        -- the pick
        local rx0 = mid + T.px(24)
        M.caption(rx0, top, "Buying")
        T.font(26)
        T.draw(rx0, top + T.px(30), T.fit(sel and sel.texts[2] or "Pick the item to buy on the left.", x1 - rx0),
          sel and C.text or C.faint)
        T.unfont()
        T.font(19)
        T.draw(rx0, top + T.px(76), "Then set the price and how many on the Offer page (RB).", C.dim)
        T.unfont()
      elseif page == 2 then
        -- the price each, how many, and the items given as well
        M.caption(x0, top, "Price each")
        ui.SetCursorPos(x0 - wx, top + T.px(28) - wy)
        M.coinButtons(BW, { "CompensationPlat", "CompensationGold", "CompensationSilver", "CompensationCopper" }, x0, mid - T.px(24), T.px(72))
        ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(6) - wy)
        M.field(F, BW, "CountEdit", "How many", mid - T.px(24) - x0, bh, { hint = "1", max = 6 })
        local ly = select(2, ui.GetCursorScreenPos()) + T.px(14)
        M.caption(x0, ly, "Also give (up to 10 of your items)")
        M.list(BW, "CompensationList", comp, x0, ly + T.px(30), mid - T.px(24), y1 - 2 * bh - T.px(30),
          { rowH = 56, draw = itemRow(2, 3), empty = "Coins only." })
        ui.SetCursorPos(x0 - wx, y1 - 2 * bh - T.px(10) - wy)
        M.buttons(BW, { { "InventoryRemoveButton", "Remove the selected" } }, x0, mid - T.px(24), bh)
        M.buttons(BW, { { "BuyLineAddButton", "Create Buy Line", { primary = true } }, { "BuyLineModifyButton", "Modify" } },
          x0, mid - T.px(24), bh)
        local rx0 = mid + T.px(24)
        M.caption(rx0, top, "Your items")
        M.list(BW, "InventoryList", inv, rx0, top + T.px(30), x1, y1 - bh - T.px(14),
          { rowH = 56, draw = itemRow(2, 3), empty = "No tradeable items." })
        ui.SetCursorPos(rx0 - wx, y1 - bh - wy)
        M.buttons(BW, { { "InventoryAddButton", "Add the selected to the offer" }, { "InventoryRefreshButton", "Refresh" } }, rx0, x1, bh)
      else
        -- the buy lines: On ("X"), icon, name, count, offering
        M.list(BW, "BuyLineList", lines, x0, top, mid - T.px(24), y1 - bh - T.px(14), {
          rowH = 64, empty = "No buy lines yet.",
          draw = function(r, rx0, ry0, rx1, ry1)
            local on = r.texts[1] == "X"
            local offer = r.texts[5] or ""
            M.row(r, rx0, ry0, rx1, ry1, r.texts[3], on and "On" or "Off",
              { sub = "x" .. (r.texts[4] or "?") .. "   " .. offer, rightW = T.px(80), rightColor = on and GREEN or C.faint })
          end })
        ui.SetCursorPos(x0 - wx, y1 - bh - wy)
        M.buttons(BW, { { "BuyLineRemoveButton", "Remove" }, { "BuyLineActiveButton", "Turn On / Off" } }, x0, mid - T.px(24), bh)
        local rx0 = mid + T.px(24)
        M.caption(rx0, top, "Status")
        local wy0 = y1 - 2 * bh - T.px(60)
        P.draw("##btrstatus", status, API.windows.Text(BW, "StatusText") or "", rx0, top + T.px(30), x1, wy0 - bh - T.px(24),
          { empty = "Nothing yet.", size = 19 })
        ui.SetCursorPos(rx0 - wx, wy0 - bh - T.px(10) - wy)
        M.buttons(BW, { { "ListLinesButton", "List Lines" }, { "ClearStatusButton", "Clear" } }, rx0, x1, bh)
        M.caption(rx0, wy0 + T.px(4), "Welcome message")
        ui.SetCursorPos(rx0 - wx, wy0 + T.px(34) - wy)
        M.field(F, BW, "WelcomeEdit", "Shown to sellers", x1 - rx0, bh, { hint = "None", max = 128 })
        ui.SetCursorPos(rx0 - wx, y1 - bh - wy)
        M.buttons(BW, { { "UpdateWelcomeButton", "Update Welcome" } }, rx0, x1, bh)
      end
      -- the buying state and Start / Stop (drawn last: the pad's first focus is the page's first control)
      local cw = M.chip(x1 - T.px(560), y0 + T.px(4), buying and "Buying" or "Not buying", buying and GREEN or C.dim)
      startStop(x1 - T.px(560) + cw + T.px(16), y0 - T.px(4), x1 - (x1 - T.px(560) + cw + T.px(16)), T.px(56))
    end
  end
  if M.back(F, W.finish()) then API.windows.Show(BW, false) end
end)

-- ---------------------------------------------------------------- a buyer's window (BarterMerchantWnd)
local MW = "BarterMerchantWnd"
local mlines, mdetail = {}, {}

ui.window("Barter Merchant", function()
  if not M.visible(MW) then return end
  M.frame()
  local name = API.windows.Text(MW, "MerchantNameLabel") or ""
  if W.begin("Barter Merchant", { w = 1500, h = 900, title = name ~= "" and name or "Buyer", subtitle = "is buying",
        hints = { { "a", "Sell" }, { "y", "Inspect" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local mid = x0 + (x1 - x0) * 0.58
    local _, sel = M.list(MW, "BuyLineList", mlines, x0, y0, mid - T.px(24), y1, {
      rowH = 64, select = "focus", empty = "This buyer has no buy lines.",
      draw = function(r, rx0, ry0, rx1, ry1)
        M.row(r, rx0, ry0, rx1, ry1, r.texts[2], function(xr, a, b)
          local cp = M.parse(r.texts[4])
          if cp then M.price(cp, xr, a, b, 1) end
        end, { sub = "wants " .. (r.texts[3] or "?") })
      end,
      onA = function() M.afterA(function() API.windows.ClickChild(MW, "SellButton", "left") end) end })
    local rx0 = mid + T.px(24)
    M.caption(rx0, y0, sel and "Also gives, for each sale" or "Focus a buy line")
    local bh = T.px(72)
    M.list(MW, "DetailList", mdetail, rx0, y0 + T.px(30), x1, y1 - bh - T.px(60),
      { rowH = 56, draw = itemRow(2, 3), empty = sel and "Coins only." or "" })
    local can = M.enabled(MW, "SellButton")
    T.font(18)
    T.draw(rx0, y1 - bh - T.px(40), can and "Sell asks how many." or (sel and "You do not carry this item." or ""), C.dim)
    T.unfont()
    ui.SetCursorPos(rx0 - wx, y1 - bh - wy)
    M.button(MW, "SellButton", "Sell", x1 - rx0, bh, { primary = true, afterA = true, glyph = "a" })
  end
  if W.finish() then API.windows.Show(MW, false) end
end)

-- ---------------------------------------------------------------- the search for buyers (BarterSearchWnd)
local SW = "BarterSearchWnd"
local SF = M.form()
local sinv, slines, sdetail = {}, {}, {}
local searched, lastDrawn = false, -10

local function find()
  API.windows.ClickChild(SW, "SearchButton", "left")
  M.refresh(SW, "BuyLineList")
  searched = true
  slines.offset = 0
end

ui.window("Barter Search", function()
  if not M.visible(SW) then
    SF.kb.open, SF.pick = false, nil
    return
  end
  M.frame()
  local frame = ui.GetFrameCount()
  if frame - lastDrawn > 2 then searched = false end
  lastDrawn = frame
  local hints = M.hints(SF, { { "a", "Select / sell" }, { "y", "Inspect" }, { "x", "Search" }, { "b", "Close" } })
  if W.begin("Barter Search", { w = 1800, h = 1000, title = "Barter Search", subtitle = "Find buyers for your items",
        hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(SF, x0, y0, x1, y1) then
      if M.pressed("Barter Search", "x") then find() end
      local bh = T.px(64)
      -- your items: A puts the item's name in the search
      local c1 = x0 + T.px(440)
      M.caption(x0, y0, "Your items")
      M.list(SW, "InventoryList", sinv, x0, y0 + T.px(30), c1, y1 - bh - T.px(14), {
        rowH = 56, draw = itemRow(2, 3), empty = "Nothing a buyer could take.", inspect = false })
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      M.buttons(SW, { { "RefreshInventoryButton", "Refresh" } }, x0, c1, bh)
      -- the search and its buy lines
      local c2x0, c2x1 = c1 + T.px(24), x0 + (x1 - x0) * 0.70
      ui.SetCursorPos(c2x0 - wx, y0 - wy)
      local sw = c2x1 - c2x0
      M.field(SF, SW, "ItemSearchEdit", "Item to sell", sw * 0.45, bh, { hint = "Type or pick an item", done = find })
      ui.SameLine(0, T.px(10))
      M.combo(SF, SW, "PlayerCombo", "Buyers", sw * 0.33, bh)
      ui.SameLine(0, T.px(10))
      M.button(SW, "SearchButton", "Search", sw * 0.22 - T.px(20), bh, { primary = true, glyph = "x" })
      local _, sel = M.list(SW, "BuyLineList", slines, c2x0, y0 + bh + T.px(16), c2x1, y1, {
        rowH = 64, select = "focus",
        empty = searched and "No buyer wants that." or "Search for an item to see who is buying it.",
        draw = function(r, rx0, ry0, rx1, ry1)
          M.row(r, rx0, ry0, rx1, ry1, r.texts[2], function(xr, a, b)
            local cp = M.parse(r.texts[4])
            if cp then M.price(cp, xr, a, b, 1) end
          end, { sub = (r.texts[5] or "?") .. " wants " .. (r.texts[3] or "?") })
        end,
        -- Sell: the line's own button (the list's column 5, as CBarterSearchWnd puts one on each line)
        onA = function(r) M.afterA(function() API.windows.ListClick(SW, "BuyLineList", r.index, "left", 5) end) end })
      -- what the line gives, and the buttons
      local rx0 = c2x1 + T.px(24)
      M.caption(rx0, y0, sel and "Also gives, for each sale" or "Focus a buy line")
      local by = y1 - 3 * bh - 2 * T.px(10)
      M.list(SW, "DetailsList", sdetail, rx0, y0 + T.px(30), x1, by - T.px(14),
        { rowH = 56, draw = itemRow(2, 3), empty = sel and "Coins only." or "" })
      ui.SetCursorPos(rx0 - wx, by - wy)
      if T.button("##btssell", "Sell", (x1 - rx0 - T.px(10)) / 2, bh, { primary = true, disabled = sel == nil }, pad) and sel then
        local index = sel.index
        M.afterA(function() API.windows.ListClick(SW, "BuyLineList", index, "left", 5) end)
      end
      ui.SameLine(0, T.px(10))
      M.button(SW, "FindButton", "Find Buyer", (x1 - rx0 - T.px(10)) / 2, bh)
      M.buttons(SW, { { "GreetingButton", "Greeting" }, { "WelcomeButton", "Welcome" } }, rx0, x1, bh)
      M.buttons(SW, { { "BuyLineInspectButton", "Inspect" }, { "CompensationInspectButton", "Inspect Gift" },
        { "CompensationPreviewButton", "Preview" }, { "HideButton", "Hide" } }, rx0, x1, bh)
    end
  end
  if M.back(SF, W.finish()) then API.windows.Show(SW, false) end
end)
