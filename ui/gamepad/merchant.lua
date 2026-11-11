-- Merchant (gamepad pack): stands in for MerchantWnd. Its pages, LB / RB to switch:
--   Buy      the merchant's goods; A buys the focused one (the stock Buy button: the quantity dialog asks how many)
--   Sell     your bags; A sells the focused item (selected by a click on its slot, as with the merchant open in the
--            game, then the stock Sell button and its quantity dialog)
--   Parcels  at a parcel merchant (EQEmu's): the parcels waiting, A retrieves the focused one, X claims them all;
--            Y sends one: To and Note on the keyboard, then A on an item of your bags sends it (the stock Send
--            button: a stack asks how many), or money: A on a coin of your purse (the inventory's coin buttons: the
--            quantity dialog asks how much) picks it, and Send money sends what is picked, else asks the platinum
--            on the keyboard (eq.SendParcelMoney)
-- Y inspects the focused item. On Buy, X switches "Usable only" (the stock MW_UsableButton, on to start). The price
-- and the merchant's rules are the stock window's.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local K = dofile("lib/keyboard.lua")
local X = dofile("lib/exchange.lua")
local C = T.C

local MW = "MerchantWnd"
local MANAGED = ui.ManagedDistribution and ui.ManagedDistribution() or false
local page = 1 -- 1 buy, 2 sell
local buyList = { offset = 0 }
local pendingBuy, pendingSell = nil, nil -- a row / slot selected this frame, its button pressed next frame
local parcelList = { offset = 0 }
local sendMode, pendingSend, parcelNote = false, nil, nil
-- the money picked with the purse's coins (eq.Parcels' money, by its serial), platinum first
local sendCoins, moneySerial = { 0, 0, 0, 0 }, nil
local kb, kbFor = K.new(), nil -- the keyboard, and what it types ("to", "note", "money")
-- The pad's focus: on the first row or slot when a page shows (the other page's item is gone), and on the slot now in
-- the sold item's place when it leaves the bags.
local sellFocus, sellRefocus, lastPage

local function coins(texts)
  local out = {}
  local units = { { 4, "pp" }, { 5, "gp" }, { 6, "sp" }, { 7, "cp" } }
  for _, u in ipairs(units) do
    local v = tonumber(texts[u[1]] or "") or 0
    if v > 0 then out[#out + 1] = v .. u[2] end
  end
  return #out > 0 and table.concat(out, " ") or "free"
end

local function tabs(x0, y0)
  local labels = { "Buy", "Sell" }
  if eq.Parcels() then labels[3] = "Parcels" end
  local x = x0
  T.font(22)
  for i, l in ipairs(labels) do
    local tw, th = ui.CalcTextSize(l)
    local w = tw + T.px(40)
    T.rect(x, y0, x + w, y0 + th + T.px(16), page == i and C.accent or C.raised, page == i and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + th + T.px(16), l, page == i and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + th + T.px(16)) and ui.IsMouseClicked(ui.Mouse.Left) then page = i end
    x = x + w + T.px(10)
  end
  local _, th = ui.CalcTextSize("Ag")
  T.unfont()
  return y0 + th + T.px(28)
end

ui.window("Merchant", function()
  if not W.shown(MW) then return end
  T.frame()
  X.frame() -- runs a purse coin's click once A is up (its quantity dialog)
  -- the stock button the previous frame asked for, once the selection is the game's
  if pendingBuy then eq.ClickChild(MW, "MW_Buy_Button", "left") pendingBuy = nil end
  if pendingSell then eq.ClickChild(MW, "MW_Sell_Button", "left") pendingSell = nil end
  if pendingSend then eq.ClickChild(MW, "MW_Send_Button", "left") pendingSend = nil end
  local parcelsHere = eq.Parcels() ~= nil
  if page == 3 and not parcelsHere then page = 1 end
  local title = eq.WindowTitle(MW) or ""
  local pp, gp, sp, cp = eq.Money()
  local hints = { { "a", page == 1 and "Buy" or "Sell" }, { "y", "Inspect" }, { "lb", "Previous page" }, { "rb", "Next page" }, { "b", "Close" } }
  local _, usableOnly = eq.ChildState(MW, "MW_UsableButton")
  if page == 1 then table.insert(hints, 3, { "x", usableOnly and "Show everything" or "Usable only" }) end
  if kb.open then
    hints = K.HINTS
  elseif page == 3 and sendMode then
    hints = { { "a", "Send" }, { "y", "Back to the parcels" }, { "b", "Back" } }
  elseif page == 3 then
    hints = MANAGED
      and { { "a", "Retrieve" }, { "y", "Send a parcel" }, { "lb", "Previous page" }, { "b", "Close" } }
      or { { "a", "Retrieve" }, { "x", "Claim all" }, { "y", "Send a parcel" }, { "lb", "Previous page" }, { "b", "Close" } }
  end
  if W.begin("Merchant", { w = 1500, h = 900, title = title ~= "" and title or "Merchant",
        subtitle = string.format("You have %dpp %dgp %dsp %dcp", pp or 0, gp or 0, sp or 0, cp or 0),
        hints = hints }) then
    if not kb.open and W.pressed("lb") then page = math.max(1, page - 1) end
    if not kb.open and W.pressed("rb") then page = math.min(parcelsHere and 3 or 2, page + 1) end
    if page ~= lastPage then
      if page == 1 then buyList.focusNext = 1 else sellRefocus = 1 end
      buyList.lastFocus = nil
      lastPage = page
    end
    -- the stock window's page: Purchase (Buy and Sell) or Parcels, where the purse's coins pick money to send
    eq.SetTabPage(MW, "MW_MerchantSubwindows", page == 3 and 2 or 0)
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local listX1 = x1 - T.px(420)
    local top = tabs(x0, y0)
    ui.SetCursorPos(x0 - wx, top - wy)
    local focusedItem, focusedWhere, focusedRow
    if page == 1 then
      if not kb.open and W.pressed("x") then
        eq.ClickChild(MW, "MW_UsableButton", "left")
        buyList.focusNext, buyList.lastFocus = 1, nil
      end
      local rows = eq.ListInfo(MW, "ItemList") or 0
      if rows == 0 then
        T.draw(x0, top, usableOnly and "Nothing here you can use. X shows everything." or "Nothing for sale.", C.dim)
      end
      local rowH = 64
      local visible = math.max(1, math.floor((y1 - top) / T.px(rowH + 10)))
      W.list("buy", buyList, rows, visible, rowH, listX1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
        local texts, _, _, _, tex, u0, v0, u1, v1 = eq.ListRow(MW, "ItemList", i - 1)
        if not texts then return end
        local s = ry1 - ry0 - T.px(12)
        if tex then T.icon(tex, u0, v0, u1, v1, rx0 + T.px(8), ry0 + T.px(6), rx0 + T.px(8) + s, ry0 + T.px(6) + s) end
        local qty = (texts[3] or "") ~= "" and (" x" .. texts[3]) or ""
        T.textIn(rx0 + s + T.px(24), ry0, rx1 - T.px(260), ry1, T.fit((texts[2] or "") .. qty, rx1 - rx0 - s - T.px(300)), C.text, 0)
        local lvl = (texts[8] or "") ~= "" and ("Lvl " .. texts[8] .. "   ") or ""
        T.textIn(rx1 - T.px(300), ry0, rx1 - T.px(14), ry1, lvl .. coins(texts), C.endurance, 1)
        if focused then
          focusedWhere = "Price: " .. coins(texts)
          focusedRow = { name = texts[2] or "", qty = texts[3] or "", price = coins(texts), tex = tex, uv = { u0, v0, u1, v1 } }
          if buyList.lastFocus ~= i then -- select it in the stock window: the price and the item to inspect
            buyList.lastFocus = i
            eq.ListClick(MW, "ItemList", i - 1, "left")
          end
          if W.pressed("y") then eq.ListClick(MW, "ItemList", i - 1, "right") end
        end
      end, function(i)
        eq.ListClick(MW, "ItemList", i - 1, "left")
        pendingBuy = true
      end)
    elseif page == 3 and kb.open then
      local r = K.draw(kb, pad, x0, top, math.min(listX1 - x0, T.px(1100)))
      if r == "ok" then
        local t = kb.text:match("^%s*(.-)%s*$") or ""
        if kbFor == "to" then eq.SetWindowText(MW, "MW_Send_To_Edit", t)
        elseif kbFor == "note" then eq.SetWindowText(MW, "MW_Note_Edit", t)
        elseif kbFor == "money" then
          local n = tonumber(t:match("%d+") or "")
          local to = eq.WindowText(MW, "MW_Send_To_Edit") or ""
          if n and n > 0 then
            parcelNote = eq.SendParcelMoney(to, eq.WindowText(MW, "MW_Note_Edit") or "", n, 0, 0, 0)
              or (n .. "pp sent to " .. to)
          end
        end
      end
    elseif page == 3 and not sendMode then
      -- the parcels waiting here
      local list, claiming = eq.Parcels()
      list = list or {}
      if #list == 0 then
        T.draw(x0, top, "No parcels waiting. Y sends one.", C.dim)
        ui.Dummy(1, 1)
      else
        local rowH = 64
        local visible = math.max(1, math.floor((y1 - top) / T.px(rowH + 10)))
        W.list("parcels", parcelList, #list, visible, rowH, listX1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
          local p = list[i]
          local it = p.item
          local s = ry1 - ry0 - T.px(12)
          local tex, u0, v0, u1, v1 = eq.ItemIcon(it.icon)
          if tex then T.icon(tex, u0, v0, u1, v1, rx0 + T.px(8), ry0 + T.px(6), rx0 + T.px(8) + s, ry0 + T.px(6) + s) end
          local name = it.name .. ((it.stackable and it.count > 1) and (" x" .. it.count) or "")
          local from = p.from ~= "" and ("From " .. p.from) or "From the server"
          T.textIn(rx0 + s + T.px(24), ry0, rx1 - T.px(300), ry1, T.fit(name, rx1 - rx0 - s - T.px(340)), C.text, 0)
          T.textIn(rx1 - T.px(300), ry0, rx1 - T.px(14), ry1, T.fit(from, T.px(286)), C.dim, 1)
          if focused then
            focusedItem = it
            focusedWhere = from .. (p.sent > 0 and (", " .. os.date("%b %d", p.sent)) or "") .. (p.note ~= "" and (": " .. p.note) or "")
          end
        end, function(i) eq.RetrieveParcel(list[i].slot) end)
      end
      if not MANAGED and not kb.open and W.pressed("x") and #list > 0 and claiming == 0 then eq.ClaimAllParcels() end
      if not kb.open and W.pressed("y") then sendMode, sellRefocus, parcelNote = true, 1, nil end
    elseif page == 3 then
      -- sending: To, Note and money, then your bags (A on an item sends it)
      local _, _, _, sending, _, money = eq.Parcels()
      if money and money.serial ~= moneySerial then
        moneySerial = money.serial
        sendCoins[money.coin + 1] = money.amount
      end
      local picked = {}
      for i, u in ipairs({ "pp", "gp", "sp", "cp" }) do
        if sendCoins[i] > 0 then picked[#picked + 1] = sendCoins[i] .. u end
      end
      local to = eq.WindowText(MW, "MW_Send_To_Edit") or ""
      local note = eq.WindowText(MW, "MW_Note_Edit") or ""
      local gap = T.px(10)
      local bh, bw = T.px(84), (listX1 - x0 - 2 * gap) / 3
      if sellRefocus == 1 then ui.SetKeyboardFocusHere() sellRefocus = nil end
      if T.button("##pto", "To", bw, bh, { sub = to ~= "" and to or "Pick a name", align = 0 }, pad) then
        kbFor = "to"
        K.open(kb, "Send the parcel to", to, 63, true)
      end
      ui.SameLine(0, gap)
      if T.button("##pnote", "Note", bw, bh, { sub = note ~= "" and note or "None", align = 0 }, pad) then
        kbFor = "note"
        K.open(kb, "A note with the parcel", note, 127)
      end
      ui.SameLine(0, gap)
      if T.button("##pmoney", "Send money", bw, bh,
          { sub = #picked > 0 and table.concat(picked, " ") or "Platinum", align = 0, disabled = to == "" or sending }, pad) then
        if #picked > 0 then
          local why = eq.SendParcelMoney(to, note, sendCoins[1], sendCoins[2], sendCoins[3], sendCoins[4])
          parcelNote = why or (table.concat(picked, " ") .. " sent to " .. to)
          if not why then sendCoins = { 0, 0, 0, 0 } end
        else
          kbFor = "money"
          K.open(kb, "Platinum to send to " .. to, "", 9)
        end
      end
      -- your purse: the inventory's coin buttons, which on this page pick money to send
      local mine = { eq.Money() }
      X.coinRow("pcoin", x0, listX1 - x0, function(i) return mine[i + 1] end,
        function(i) X.afterA(function() eq.ClickChild("InventoryWindow", "IW_Money" .. i, "left") end) end,
        { outlined = function(i) return select(2, eq.ChildState("InventoryWindow", "IW_Money" .. i)) == true end })
      local size = T.px(76)
      local per = math.max(1, math.floor((listX1 - x0 + gap) / (size + gap)))
      local n = 0
      local function cell(slot, bag, item, where)
        if n % per ~= 0 then ui.SameLine(0, gap) end
        n = n + 1
        local clicked, focused = T.slot("##send" .. slot .. "_" .. (bag or -1), item, size, {}, pad)
        if focused then focusedItem, focusedWhere = item, where end
        if clicked and item then
          if to == "" then
            parcelNote = "Pick who it is for first (To)."
          elseif not sending then
            eq.ClickSlot(slot, bag, "left") -- selects it for the merchant
            pendingSend = true
            parcelNote = item.name .. " sent to " .. to
          end
        end
      end
      for slot = 23, 32 do
        local it = eq.Item(slot)
        if it and (it.bagSlots or 0) == 0 then cell(slot, nil, it, eq.SlotName(slot)) end
      end
      for slot = 23, 32 do
        local bag = eq.Item(slot)
        if bag and (bag.bagSlots or 0) > 0 then
          for i = 0, bag.bagSlots - 1 do
            local it = eq.Item(slot, i)
            if it then cell(slot, i, it, bag.name) end
          end
        end
      end
      if not kb.open and W.pressed("y") then sendMode = false parcelList.focusNext = 1 end
    else
      -- your bags: the ten bag slots, then each bag's contents
      local size, gap = T.px(76), T.px(8)
      local per = math.max(1, math.floor((listX1 - x0 + gap) / (size + gap)))
      local n = 0
      local focusedAt
      local function cell(slot, bag, item, where)
        if n % per ~= 0 then ui.SameLine(0, gap) end
        n = n + 1
        if sellRefocus == n then ui.SetKeyboardFocusHere() sellRefocus = nil end
        local clicked, focused = T.slot("##sell" .. slot .. "_" .. (bag or -1), item, size, {}, pad)
        if focused then
          focusedItem, focusedWhere, focusedAt = item, where, n
          if W.pressed("y") and item then eq.ClickSlot(slot, bag, "rightheld") end
        end
        if clicked and item then
          eq.ClickSlot(slot, bag, "left") -- selects it for the merchant
          pendingSell = true
        end
      end
      for slot = 23, 32 do
        local it = eq.Item(slot)
        if it and (it.bagSlots or 0) == 0 then cell(slot, nil, it, eq.SlotName(slot)) end
      end
      for slot = 23, 32 do
        local bag = eq.Item(slot)
        if bag and (bag.bagSlots or 0) > 0 then
          for i = 0, bag.bagSlots - 1 do
            local it = eq.Item(slot, i)
            if it then cell(slot, i, it, bag.name) end
          end
        end
      end
      if n == 0 then T.draw(x0, top, "Nothing to sell.", C.dim) end
      if sellRefocus and sellRefocus > n then sellRefocus = n > 0 and n or nil end
      if focusedAt then
        sellFocus = focusedAt
      elseif sellFocus and W.focused() and n > 0 then -- the focused item was sold
        sellRefocus = math.min(sellFocus, n)
        sellFocus = nil
      end
    end
    -- the right column: the focused item, the stock window's selection
    if page >= 2 then
      I.summary(focusedItem, focusedWhere, listX1 + T.px(24), y0, x1, y1 - T.px(170))
    else -- the merchant's row: its icon, name and price (Y shows the item's details)
      local px0, py1 = listX1 + T.px(24), y1 - T.px(170)
      T.rect(px0, y0, x1, py1, C.sunken, 1, T.px(14))
      local p = T.px(18)
      if focusedRow then
        local s = T.px(72)
        T.rect(px0 + p, y0 + p, px0 + p + s, y0 + p + s, C.raised, 1, T.px(10))
        if focusedRow.tex then
          local uv = focusedRow.uv
          T.icon(focusedRow.tex, uv[1], uv[2], uv[3], uv[4], px0 + p + T.px(6), y0 + p + T.px(6), px0 + p + s - T.px(6), y0 + p + s - T.px(6))
        end
        T.font(24)
        local tx = px0 + p + s + T.px(14)
        T.draw(tx, y0 + p, T.fit(focusedRow.name, x1 - p - tx))
        T.unfont()
        T.font(20)
        local _, lh = ui.CalcTextSize("Ag")
        local y = y0 + p + s + T.px(14)
        T.draw(px0 + p, y, "Price: " .. focusedRow.price, C.endurance)
        if focusedRow.qty ~= "" then T.draw(px0 + p, y + lh + T.px(4), "In stock: " .. focusedRow.qty, C.dim) end
        T.unfont()
        T.font(18)
        T.hints(pad, { { "y", "Item details" } }, px0 + p, py1 - p - T.px(30), T.px(30))
        T.unfont()
      end
    end
    local sy = y1 - T.px(150)
    T.rect(listX1 + T.px(24), sy, x1, y1, C.sunken, 1, T.px(14))
    T.font(18)
    if page == 3 then
      local _, claiming, _, sending = eq.Parcels()
      T.draw(listX1 + T.px(42), sy + T.px(16), "PARCELS", C.dim)
      T.unfont()
      T.font(22)
      local status = (claiming or 0) > 0 and string.format("Claiming... %d left", claiming)
        or (sending and "Sending..." or parcelNote or (sendMode and "A on an item sends it" or "Y sends a parcel"))
      T.textIn(listX1 + T.px(42), sy + T.px(48), x1 - T.px(18), y1 - T.px(12), T.fit(status, x1 - listX1 - T.px(60)), C.text, 0)
      T.unfont()
    else
      T.draw(listX1 + T.px(42), sy + T.px(16), "SELECTED", C.dim)
      T.unfont()
      T.font(22)
      local selName = eq.WindowText(MW, "MW_SelectedItemLabel") or ""
      T.draw(listX1 + T.px(42), sy + T.px(48), T.fit(selName ~= "" and selName or "-", x1 - listX1 - T.px(60)))
      T.draw(listX1 + T.px(42), sy + T.px(88), eq.WindowText(MW, "MW_SelectedPriceLabel") or "", C.endurance)
      T.unfont()
    end
  end
  local close = W.back(W.finish(), nil, kb)
  if close and page == 3 and sendMode then
    sendMode, close = false, false
    parcelList.focusNext = 1
  end
  if close then eq.ShowWindow(MW, false) end
end)
