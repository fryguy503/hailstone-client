-- Bazaar (gamepad pack): stands in for the bazaar's three windows.
--   Bazaar Trader   BazaarWnd (/trader): your wares (the Trader's Satchels' items) as slots, A selects one (Y
--                   inspects it); on the right the selected ware, its price as four coin buttons (the game's amount
--                   dialog), Set Price (X) and Clear, Begin / End Trader, Add, Remove and List Items, with the
--                   trading state as a chip; the window's activity text under the wares.
--   Bazaar Search   BazaarSearchWnd (/bazaar, the EQ menu's Bazaar Search): the filters on the left (the name typed
--                   on the keyboard, the drop-downs as pickers, levels, prices, results a trader), Find Items (X)
--                   and Reset; the results on the right (item, how many, the price each, the trader, the stat's
--                   value when a stat is picked): focusing one selects it, A buys it (the quantity dialog, then the
--                   confirmation), Y inspects it; Buy, Inspect, Preview and Welcome under them.
--   Confirm         BazaarConfirmationWnd: the item and the total, To Me (Expedient Delivery Vouchers), To Parcels
--                   (the platinum fee) or Cancel (B).
-- Every slot, field, drop-down, list and button is the stock window's (hs.ClickChild, hs.SetWindowText,
-- hs.ComboSelect, hs.ListClick): the rules and messages are the game's.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local P = dofile("lib/textpanel.lua")
local M = dofile("lib/markets.lua")
local C = T.C

local GREEN = C.good

-- ---------------------------------------------------------------- the trader (BazaarWnd)
local BW = "BazaarWnd"
local SLOTS = 200
local wares = {}
local log = { scroll = 0 }
-- The satchels' slots' ids, made once; the wares found in them ({ id, item, cell id, checked }) read again every
-- quarter second, the frame after a click, and when the window opens: 200 slots are a lot to read each frame.
local SLOT_IDS, CELL_IDS = {}, {}
for i = 0, SLOTS - 1 do
  SLOT_IDS[i] = "BZR_BazaarSlot" .. i
  CELL_IDS[i] = "##bz" .. SLOT_IDS[i]
end
local cellOpts = {}
local wareList, wareCount, wareSelected, wareReadAt, wareDirty = {}, 0, nil, -1, true
local function readWares()
  local n, selected = 0, nil
  for i = 0, SLOTS - 1 do
    local id = SLOT_IDS[i]
    local item = API.inventory.SlotItem(BW, id)
    if item then
      n = n + 1
      local w = wareList[n]
      if not w then
        w = {}
        wareList[n] = w
      end
      local _, checked = API.windows.ChildState(BW, id)
      w[1], w[2], w[3], w[4] = id, item, CELL_IDS[i], checked
      if checked then selected = item end
    end
  end
  for j = #wareList, n + 1, -1 do wareList[j] = nil end
  wareCount, wareSelected = n, selected
end

ui.window("Bazaar Trader", function()
  if not M.visible(BW) then
    wareDirty = true
    return
  end
  M.frame()
  local trading = M.shown(BW, "BZW_End_Button")
  if W.begin("Bazaar Trader", { w = 1640, h = 960, title = "Bazaar Trader",
        subtitle = trading and "Buyers can see the wares you have priced" or "Price your wares, then begin trading",
        hints = { { "a", "Select" }, { "y", "Inspect" }, { "x", "Set Price" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local rx0 = x1 - T.px(520)
    local lx1 = rx0 - T.px(24)
    if M.pressed("Bazaar Trader", "x") then API.windows.ClickChild(BW, "BZW_SetPrice_Button", "left") end
    -- the wares: the satchels' items, the selected one (the slot the game marks checked) outlined
    local now = ui.GetTime()
    if wareDirty or now - wareReadAt >= 0.25 or now < wareReadAt then
      readWares()
      wareReadAt, wareDirty = now, false
    end
    local selectedItem = wareSelected
    ui.SetCursorPos(x0 - wx, y0 - wy)
    T.heading("Your wares", lx1 - x0)
    local size, gap = T.px(84), T.px(8)
    local per = math.max(1, math.floor((lx1 - x0 + gap) / (size + gap)))
    local logY = y1 - T.px(230)
    local gy = select(2, ui.GetCursorScreenPos())
    local rows = math.max(1, math.floor((logY - T.px(20) - gy + gap) / (size + gap)))
    local focusedItem
    if wareCount == 0 then
      T.font(20)
      T.draw(x0, gy + T.px(10), "Nothing to sell: put items in a Trader's Satchel.", C.faint)
      T.unfont()
    else
      M.grid(wares, wareCount, per, rows, x0, gap, function(i)
        local w = wareList[i]
        local id, item, checked = w[1], w[2], w[4]
        cellOpts.selected = checked
        local clicked, focused = T.slot(w[3], item, size, cellOpts, pad)
        if focused then
          focusedItem = item
          if W.pressed("y") then API.windows.ClickChild(BW, id, "rightheld") end
        end
        if clicked then
          API.windows.ClickChild(BW, id, "left")
          wareDirty = true -- (the selection moves: read again next frame)
        end
        if ui.IsItemClicked(ui.Mouse.Right) then API.windows.ClickChild(BW, id, "rightheld") end
        return focused
      end)
    end
    -- the activity text
    M.caption(x0, logY, "Activity")
    P.draw("##bzlog", log, API.windows.Text(BW, "BZW_BazaarText") or "", x0, logY + T.px(28), lx1, y1,
      { empty = "Nothing yet.", size = 19 })

    -- the right column: the state, the selected ware and its price, the trader buttons
    M.chip(rx0, y0, trading and "Trading" or "Not trading", trading and GREEN or C.dim)
    local item = selectedItem
    if item or focusedItem then
      I.summary(item or focusedItem, item and "Selected" or "Press A to select it", rx0, y0 + T.px(50), x1, y0 + T.px(270))
    else
      T.rect(rx0, y0 + T.px(50), x1, y0 + T.px(270), C.sunken, 1, T.px(14))
      T.font(20)
      T.draw(rx0 + T.px(18), y0 + T.px(68), "Select a ware to price it.", C.faint)
      T.unfont()
    end
    M.caption(rx0, y0 + T.px(286), item and item.stackable and (item.count or 0) > 1 and "Price each (for one of the stack)" or "Price")
    ui.SetCursorPos(rx0 - wx, y0 + T.px(316) - wy)
    local bh = T.px(64)
    M.coinButtons(BW, { "BZW_Money0", "BZW_Money1", "BZW_Money2", "BZW_Money3" }, rx0, x1, T.px(72))
    M.buttons(BW, { { "BZW_SetPrice_Button", "Set Price", { primary = true, glyph = "x" } }, { "BZW_Clear_Button", "Clear Price" } },
      rx0, x1, bh)
    ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(14) - wy)
    M.caption(rx0, select(2, ui.GetCursorScreenPos()), "Trading")
    ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(28) - wy)
    if trading then
      M.buttons(BW, { { "BZW_End_Button", "End Trader" } }, rx0, x1, bh)
    else
      M.buttons(BW, { { "BZW_Start_Button", "Begin Trader", { primary = true } } }, rx0, x1, bh)
    end
    M.buttons(BW, { { "BZW_Add_Button", "Add" }, { "BZW_Remove_Button", "Remove" } }, rx0, x1, bh)
    M.buttons(BW, { { "BZW_List_Button", "List Items" } }, rx0, x1, bh)
  end
  if W.finish() then API.windows.Show(BW, false) end
end)

-- ---------------------------------------------------------------- the search (BazaarSearchWnd)
local SW = "BazaarSearchWnd"
local LIST = "BZR_ItemList"
local F = M.form()
local results = {}
local searched, lastDrawn = false, -10

local FILTERS = {
  { "combo", "BZR_PlayersCombobox", "Trader" },
  { "combo", "BZR_ItemSlotCombobox", "Slot" },
  { "combo", "BZR_ItemTypeCombobox", "Type" },
  { "combo", "BZR_StatSlotCombobox", "Stat" },
  { "combo", "BZR_ClassSlotCombobox", "Class" },
  { "combo", "BZR_RaceSlotCombobox", "Race" },
  { "combo", "BZR_ItemAugmentCombobox", "Augment slot" },
  { "combo", "BZR_ItemPrestigeCombobox", "Glamours" },
  { "edit", "BZR_MinLevelInput", "Level from", "any" },
  { "edit", "BZR_MaxLevelInput", "Level to", "any" },
  { "edit", "BZR_MinPriceInput", "Price from (pp)", "any" },
  { "edit", "BZR_MaxPriceInput", "Price to (pp)", "any" },
  { "edit", "BZR_MaxResultsPerTraderInput", "Results a trader", "200" },
}

local function price(t) return M.coinsOf(t, 4) end

local function find()
  if M.enabled(SW, "BZR_QueryButton") then
    API.windows.ClickChild(SW, "BZR_QueryButton", "left")
    M.refresh(SW, LIST)
    searched = true
    results.offset, results.focusNext = 0, nil
  end
end

ui.window("Bazaar Search", function()
  if not M.visible(SW) then
    F.kb.open, F.pick = false, nil
    return
  end
  M.frame()
  local frame = ui.GetFrameCount()
  if frame - lastDrawn > 2 then searched = false end -- (re)opened
  lastDrawn = frame
  local n = tonumber(API.windows.Text(SW, "BZR_SearchResultLabel") or "") or 0
  local searching = not M.enabled(SW, "BZR_QueryButton")
  local hints = M.hints(F, { { "a", "Buy / choose" }, { "y", "Inspect" }, { "x", "Find Items" }, { "b", "Close" } })
  if W.begin("Bazaar Search", { w = 1800, h = 1000, title = "Bazaar Search",
        subtitle = searching and "Searching..." or (n == 1 and "1 item found" or (n .. " items found")), hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(F, x0, y0, x1, y1) then
      if M.pressed("Bazaar Search", "x") then find() end
      -- the filters: the name across, then two a line
      local fx1 = x0 + T.px(560)
      local gap = T.px(10)
      local fh = T.px(64)
      local half = (fx1 - x0 - gap) / 2
      ui.SetCursorPos(x0 - wx, y0 - wy)
      M.field(F, SW, "BZR_ItemNameInput", "Item name", fx1 - x0, fh, { hint = "Any (type part of a name)", done = find })
      local col = 0
      for _, f in ipairs(FILTERS) do
        if f[1] == "edit" or API.windows.ComboInfo(SW, f[2]) then
          if col == 1 then ui.SameLine(0, gap) else ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy) end
          if f[1] == "combo" then M.combo(F, SW, f[2], f[3], half, fh)
          else M.field(F, SW, f[2], f[3], half, fh, { hint = f[4], max = 10 }) end
          col = 1 - col
        end
      end
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(8) - wy)
      M.buttons(SW, { { "BZR_QueryButton", searching and "Searching..." or "Find Items", { primary = true, glyph = "x" } },
        { "BZR_Default", "Reset" } }, x0, fx1, T.px(64))

      -- the results
      local rx0 = fx1 + T.px(28)
      local _, _, _, labels = API.windows.ListInfo(SW, LIST)
      local stat = labels and labels[10] or ""
      local hasStat = stat ~= "" and stat ~= "Stat"
      local bh = T.px(64)
      local selY = y1 - bh - T.px(70)
      local _, sel = M.list(SW, LIST, results, rx0, y0, x1, selY - T.px(14), {
        rowH = 60, select = "focus", inspect = true,
        empty = searching and "Searching..." or (searched and "Nothing matches these filters." or "Set the filters, then Find Items (X)."),
        draw = function(r, rx, ry, rx1, ry1)
          local t = r.texts
          local qty = tonumber(t[3] or "") or 1
          local sub = "from " .. (t[8] or "?") .. (qty > 1 and ("   x" .. qty) or "")
          if hasStat then sub = sub .. "   " .. stat .. " " .. (t[10] or "") end
          M.row(r, rx, ry, rx1, ry1, t[2], function(xr, a, b) M.price(price(t), xr, a, b, 1) end, { sub = sub })
        end,
        onA = function() M.afterA(function() API.windows.ClickChild(SW, "BZR_BuyButton", "left") end) end,
      })
      -- the selected result and the buttons
      if sel then
        local t = sel.texts
        local qty = tonumber(t[3] or "") or 1
        T.font(22)
        local w = T.text(rx0, selY, T.fit((t[2] or "") .. (qty > 1 and ("  x" .. qty) or ""), (x1 - rx0) * 0.55), C.text)
        T.unfont()
        T.font(20)
        local _, th = ui.CalcTextSize("Ag")
        local px = rx0 + w + T.px(20)
        px = px + T.text(px, selY + T.px(2), "from " .. (t[8] or "?") .. ":", C.dim) + T.px(12)
        px = px + M.price(price(t), px, selY, selY + th + T.px(4), 0)
        if qty > 1 then T.draw(px + T.px(10), selY + T.px(2), "each", C.dim) end
        T.unfont()
      else
        T.font(20)
        T.draw(rx0, selY, n > 0 and "Focus a result to select it." or " ", C.faint)
        T.unfont()
      end
      ui.SetCursorPos(rx0 - wx, y1 - bh - wy)
      M.buttons(SW, { { "BZR_BuyButton", "Buy", { primary = true, afterA = true } }, { "BZR_RequestItemButton", "Inspect" },
        { "BZR_RequestPreviewButton", "Preview" }, { "BZR_WelcomeButton", "Welcome" } }, rx0, x1, bh)
    end
  end
  if M.back(F, W.finish()) then API.windows.Show(SW, false) end
end)

-- ---------------------------------------------------------------- the purchase confirmation (BazaarConfirmationWnd)
local CW = "BazaarConfirmationWnd"

ui.window("Bazaar Confirmation", function()
  if not M.visible(CW) then
    GPSTATE.bzcFocused = nil -- (the next time it opens, the focus goes to Cancel again)
    return
  end
  M.frame()
  if W.begin("Bazaar Confirmation", { w = 1000, h = 560, title = "Confirm Purchase", alwaysFocus = true,
        hints = { { "a", "Choose" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    -- the item and the total
    local s = T.px(80)
    T.rect(x0, y0, x0 + s, y0 + s, C.raised, 1, T.px(12))
    local tex, u0, v0, u1, v1 = API.windows.ChildArt(CW, "BZC_ItemIcon", "decal")
    T.icon(tex, u0, v0, u1, v1, x0 + T.px(8), y0 + T.px(8), x0 + s - T.px(8), y0 + s - T.px(8))
    T.font(26)
    local _, th = ui.CalcTextSize("Ag")
    T.draw(x0 + s + T.px(20), y0 + T.px(4), T.fit(API.windows.Text(CW, "BZC_ItemLabel") or "", x1 - x0 - s - T.px(20)))
    T.unfont()
    local totalText = API.windows.Text(CW, "BZC_TotalPrice") or ""
    local total = M.parse((totalText:gsub("^[^:]*:", "")))
    T.font(22)
    local ty = y0 + T.px(4) + th + T.px(8)
    if total then
      local w = T.text(x0 + s + T.px(20), ty, "Total", C.dim)
      M.price(total, x0 + s + T.px(36) + w, ty, ty + th, 0)
    else
      T.draw(x0 + s + T.px(20), ty, totalText, C.dim)
    end
    T.unfont()
    -- the two ways to take it, as large buttons
    local cy = y0 + s + T.px(40)
    M.caption(x0, cy, "Delivery")
    local gap = T.px(16)
    local cw = (x1 - x0 - gap) / 2
    local ch = T.px(130)
    local vouchers = tonumber(((API.windows.Text(CW, "BZC_TokenLabel") or ""):match("%d+"))) or 0
    local fee = M.parse(API.windows.Text(CW, "BZC_PlatLabel") or "")
    ui.SetCursorPos(x0 - wx, cy + T.px(30) - wy)
    M.button(CW, "BZC_UseTokenButton", "To Me", cw, ch, { primary = true, align = 0,
      sub = "Into your inventory: " .. (vouchers == 1 and "1 voucher" or (vouchers .. " vouchers")) })
    ui.SameLine(0, gap)
    local feeText = fee and (function()
      local p = M.split(fee)
      local out = {}
      for i = 1, 4 do if p[i] > 0 then out[#out + 1] = p[i] .. M.COINS[i][1] end end
      return #out > 0 and table.concat(out, " ") or "free"
    end)() or (API.windows.Text(CW, "BZC_PlatLabel") or "")
    M.button(CW, "BZC_UsePlatButton", "To Parcels", cw, ch, { align = 0, sub = "Collect at a parcel merchant: fee " .. feeText })
    ui.SetCursorPos(x0 - wx, y1 - T.px(64) - wy)
    -- the focus starts on Cancel: an A carried over from the bazaar's Buy must not spend the money (as W.confirm)
    if not GPSTATE.bzcFocused then
      ui.SetKeyboardFocusHere()
      GPSTATE.bzcFocused = true
    end
    M.button(CW, "BZC_CancelButton", "Cancel", x1 - x0, T.px(64))
  end
  if W.finish() then API.windows.ClickChild(CW, "BZC_CancelButton", "left") end
end)
