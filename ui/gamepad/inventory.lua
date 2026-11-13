-- Inventory (gamepad pack): stands in for InventoryWindow, so the inventory key (I) and the EQ menu open it.
-- Equipment, the ten bag slots and the contents of the bag last focused, the character's numbers, and the focused
-- item described on the right (nothing is hover-only). Every slot goes through the game's slot logic (eq.ClickSlot):
--   A   pick up / put down / swap (the left click: the item goes on the cursor, shown here as "Holding")
--   X   use the item (the quick right click: open a bag, read a book, cast a clicky); holding one: put it away
--   Y   the item's full details (the held right button: the item display)
--   LB  holding an item: destroy it (the stock Destroy button: its confirmation unless Fast Item Destroy is on);
--       else put the focused item on a hot button (the slot picker; the left button held on the slot: a link that
--       follows the item wherever it is moved)
--   RB  holding an item: drop it on the ground (a click on the world with it: the game's drop rules and question)
-- Focusing a bag in the bag slots shows its contents below them. Under the coins, the alternate currencies (the stock
-- Alt. Currency page's AltCurr_PointList, a row selecting its row there): A on one with nothing held claims it onto
-- the cursor as items (Create Item: the quantity dialog, up to a stack), A holding its item puts it back (Reclaim;
-- both OP_AltCurrencyReclaim). While something is held every currency is listed (Display Missing), so one claimed
-- down to 0 still takes its item back.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local AS = dofile("lib/assign.lua")
local C = T.C

local INV = "InventoryWindow"
local WORN_ROWS = {
  { 0, 1, 2, 3, 4, 5, 6, 8 },
  { 7, 9, 10, 12, 15, 16, 17, 20 },
  { 18, 19, 13, 14, 11, 22, 21 },
}
local openBag = nil -- the bag slot (23..32) whose contents are shown
local CURRENCY_LIST = "AltCurr_PointList"
local currencies = {} -- W.list state
local currencyFocused = false -- last frame's focus was on a currency row (the hints)

local function label(t) return eq.Label(t) or "" end

-- The slots' ImGui ids and names (fixed), made once rather than every frame.
local SLOT_IDS, SLOT_NAMES = {}, {}
for _, row in ipairs(WORN_ROWS) do
  for _, slot in ipairs(row) do SLOT_IDS[slot] = "##worn" .. slot end
end
for slot = 23, 32 do SLOT_IDS[slot] = "##gen" .. slot end
local function slotName(slot)
  local n = SLOT_NAMES[slot]
  if not n then
    n = eq.SlotName(slot)
    SLOT_NAMES[slot] = n
  end
  return n
end
local bagIds = {} -- [bag slot][i]: "##bag<slot>_<i>"
local function bagId(slot, i)
  local ids = bagIds[slot]
  if not ids then
    ids = {}
    bagIds[slot] = ids
  end
  local id = ids[i]
  if not id then
    id = "##bag" .. slot .. "_" .. i
    ids[i] = id
  end
  return id
end

local function stat(x, y, w, name, value, c)
  T.draw(x, y, name, C.dim)
  local tw = ui.CalcTextSize(value)
  T.draw(x + w - tw, y, value, c)
end

-- The character's numbers, in x0 .. x1 from y.
local function sheet(x0, y, x1)
  local name = (label(1)):gsub("\n.*", "")
  T.font(28)
  local _, h = T.text(x0, y, T.fit(name, x1 - x0))
  T.unfont()
  y = y + h
  T.font(18)
  local _, sh = T.text(x0, y, "Level " .. label(2) .. "  " .. label(3), C.dim)
  T.unfont()
  y = y + sh + T.px(14)
  T.font(20)
  local _, lh = ui.CalcTextSize("Ag")
  lh = lh + T.px(4)
  local w = x1 - x0
  stat(x0, y, w, "HP", label(17) .. " / " .. label(18)) y = y + lh
  if (tonumber(label(125)) or 0) > 0 then stat(x0, y, w, "Mana", label(124) .. " / " .. label(125)) y = y + lh end
  stat(x0, y, w, "Endurance", label(126) .. " / " .. label(127)) y = y + lh
  stat(x0, y, w, "AC", label(22)) y = y + lh
  local atk = label(23)
  if atk ~= "" then stat(x0, y, w, "ATK", atk) y = y + lh end
  local title = label(74) -- IW_CharTitle: the AA title prefix
  if title ~= "" then stat(x0, y, w, "Title", title) y = y + lh end
  y = y + T.px(10)
  local half = (w - T.px(20)) / 2
  local STATS = { { "STR", 5 }, { "STA", 6 }, { "AGI", 8 }, { "DEX", 7 }, { "WIS", 9 }, { "INT", 10 }, { "CHA", 11 } }
  local RES = { { "Poison", 12 }, { "Disease", 13 }, { "Fire", 14 }, { "Cold", 15 }, { "Magic", 16 }, { "Corrupt", 136 } }
  local ys = y
  for i, s in ipairs(STATS) do
    local v, r, g, b = eq.Label(s[2])
    stat(x0, ys + (i - 1) * lh, half, s[1], v or "", r and { r, g, b, 1 } or nil)
  end
  for i, s in ipairs(RES) do
    local v, r, g, b = eq.Label(s[2])
    stat(x0 + half + T.px(20), ys + (i - 1) * lh, half, s[1], v or "", r and { r, g, b, 1 } or nil)
  end
  y = ys + #STATS * lh + T.px(10)
  local wv, wr, wg, wb = eq.Label(24)
  stat(x0, y, w, "Weight", (wv or "") .. " / " .. label(25), wr and { wr, wg, wb, 1 } or nil) y = y + lh
  T.unfont()
  -- money
  y = y + T.px(10)
  local pp, gp, sp, cp = eq.Money()
  T.font(20)
  local coins = { { pp, "pp", C.platinum }, { gp, "gp", C.gold }, { sp, "sp", C.silver }, { cp, "cp", C.copper } }
  local x = x0
  for _, c in ipairs(coins) do
    local tw = T.text(x, y, tostring(c[1] or 0), C.text)
    x = x + tw + T.px(4)
    local uw = T.text(x, y, c[2], c[3])
    x = x + uw + T.px(14)
  end
  T.unfont()
  local _, ch = ui.CalcTextSize("Ag")
  return y + ch
end

-- The currencies from y in x0 .. x1, down to y1.
local function currencyList(x0, y, x1, y1, holding)
  local _, missing = eq.ChildState(INV, "AltCurr_DisplayMissingButton")
  if (missing == true) ~= holding then eq.ClickChild(INV, "AltCurr_DisplayMissingButton", "left") end
  local list = eq.AltCurrencies(holding) or {}
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y - wy)
  T.heading("Currencies")
  if #list == 0 then
    T.font(18)
    T.draw(x0, select(2, ui.GetCursorScreenPos()), "None", C.faint)
    T.unfont()
    return false
  end
  local rowH = 56
  local _, ly = ui.GetCursorScreenPos()
  local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(rowH) + T.px(10))))
  local focused = W.list("##currency", currencies, #list, visible, rowH, x1 - x0, function(i, rx0, ry0, rx1, ry1)
    local c = list[i]
    local none = (c.count or 0) == 0
    local s = ry1 - ry0 - T.px(12)
    T.itemIcon(c.icon or 0, rx0 + T.px(8), ry0 + T.px(6), rx0 + T.px(8) + s, ry0 + T.px(6) + s, none and 0.4 or 1)
    T.font(19)
    local count = tostring(c.count or 0)
    local cw = ui.CalcTextSize(count)
    T.textIn(rx0 + s + T.px(18), ry0, rx1 - cw - T.px(24), ry1, T.fit(c.name or "", rx1 - rx0 - s - cw - T.px(44)),
      none and C.faint or C.text, 0)
    T.textIn(rx1 - cw - T.px(14), ry0, rx1 - T.px(14), ry1, count, none and C.faint or C.gold, 1)
    T.unfont()
  end, function(i)
    eq.ListClick(INV, CURRENCY_LIST, i - 1, "left")
    eq.ClickChild(INV, holding and "AltCurr_ReclaimButton" or "AltCurr_CreateItemButton", "left")
  end)
  return focused ~= nil
end

ui.window("Inventory", function()
  if not W.shown(INV) then return end
  T.frame()
  local held = eq.Item(eq.CursorSlot or 33)
  local holding = held ~= nil
  local hints = holding and { { "a", currencyFocused and "Reclaim" or "Place" }, { "x", "Put away" }, { "lb", "Destroy" }, { "rb", "Drop" }, { "y", "Details" }, { "b", "Close" } }
      or currencyFocused and { { "a", "Claim" }, { "b", "Close" } }
      or { { "a", "Pick up" }, { "x", "Use" }, { "y", "Details" }, { "lb", "Assign to slot" }, { "b", "Close" } }
  if W.begin("Inventory", { w = 1700, h = 980, title = "Inventory", hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local leftW, rightW = T.px(400), T.px(380)
    local midX = x0 + leftW + T.px(40)
    local sheetY = sheet(x0, y0, x0 + leftW)
    currencyFocused = currencyList(x0, sheetY + T.px(16), x0 + leftW, y1, held ~= nil)
    local focusedItem, focusedWhere, focusedSlot, focusedBag
    if held and W.pressed("lb") then eq.ClickChild(INV, "IW_Destroy", "left") end
    if held and W.pressed("rb") and eq.gamepad and eq.gamepad.drop_cursor then eq.gamepad.drop_cursor() end
    local size, gap = T.px(84), T.px(10)
    local bagName -- (a slot in the open bag is named "<bag>, slot <n>" only when it has the focus)
    local function slotAt(id, slot, bag, item, where, sz)
      local clicked, focused, sx0, sy0, sx1 = T.slot(id, item, sz or size, { label = bag == nil and where or nil }, pad)
      if focused then
        focusedItem, focusedWhere = item, where or (bagName .. ", slot " .. (bag + 1))
        focusedSlot, focusedBag = slot, bag
        if bag == nil and slot >= 23 and slot <= 32 and item and (item.bagSlots or 0) > 0 then openBag = slot end
        if W.pressed("x") then
          if held then eq.ChatSubmit(1, "/autoinventory") else eq.ClickSlot(slot, bag, "rightup") end
        end
        if W.pressed("y") and item then eq.ClickSlot(slot, bag, "rightheld") end
        -- the item on a hot button (the game's left button held on the slot: an item link that follows the item)
        if not held and item and W.pressed("lb") then
          AS.start(item.name, function() return eq.ClickSlot(slot, bag, "leftheld") end, "Inventory")
        end
        if held then -- what A would put here: the held item's icon at the slot's corner
          T.itemIcon(held.icon, sx1 - T.px(30), sy0 - T.px(14), sx1 + T.px(10), sy0 + T.px(26))
        end
      end
      if clicked then eq.ClickSlot(slot, bag, "left") end
      if ui.IsItemClicked(ui.Mouse.Right) then eq.ClickSlot(slot, bag, "rightup") end
    end
    -- equipment
    ui.SetCursorPos(midX - wx, y0 - wy)
    T.heading("Equipment")
    for _, row in ipairs(WORN_ROWS) do
      ui.SetCursorPos(midX - wx, select(2, ui.GetCursorScreenPos()) - wy)
      for i, slot in ipairs(row) do
        if i > 1 then ui.SameLine(0, gap) end
        slotAt(SLOT_IDS[slot], slot, nil, eq.Item(slot), slotName(slot))
      end
    end
    -- the bag slots
    ui.SetCursorPos(midX - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
    T.heading("Bags")
    ui.SetCursorPos(midX - wx, select(2, ui.GetCursorScreenPos()) - wy)
    local bsize = T.px(76)
    for slot = 23, 32 do
      if slot > 23 then ui.SameLine(0, T.px(8)) end
      slotAt(SLOT_IDS[slot], slot, nil, eq.Item(slot), slotName(slot), bsize)
    end
    -- the open bag's contents (the first bag until one is focused); read after the slots' clicks above
    local bag = openBag and eq.Item(openBag)
    if not (bag and (bag.bagSlots or 0) > 0) then
      openBag = nil
      for slot = 23, 32 do
        local it = eq.Item(slot)
        if it and (it.bagSlots or 0) > 0 then
          openBag, bag = slot, it
          break
        end
      end
    end
    if bag and (bag.bagSlots or 0) > 0 then
      ui.SetCursorPos(midX - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
      T.heading(bag.name)
      bagName = bag.name
      local per = 10
      for i = 0, bag.bagSlots - 1 do
        if i % per == 0 then ui.SetCursorPos(midX - wx, select(2, ui.GetCursorScreenPos()) - wy) else ui.SameLine(0, T.px(8)) end
        slotAt(bagId(openBag, i), openBag, i, eq.Item(openBag, i), nil, bsize)
      end
    else
      openBag = nil
    end
    -- the right column: the focused item, what is held
    local rx0 = x1 - rightW
    local holdH = held and T.px(110) or 0
    I.summary(focusedItem, focusedWhere, rx0, y0, x1, y1 - holdH - (held and T.px(12) or 0),
      focusedItem and focusedSlot and eq.CompareItem(focusedSlot, focusedBag) or nil)
    if held then
      local hy0 = y1 - holdH
      T.rect(rx0, hy0, x1, y1, C.accent, 0.16, T.px(14))
      T.outline(rx0, hy0, x1, y1, C.accent, 0.6, T.px(14), T.px(2))
      local s = T.px(64)
      T.itemIcon(held.icon, rx0 + T.px(16), hy0 + (holdH - s) / 2, rx0 + T.px(16) + s, hy0 + (holdH + s) / 2)
      T.font(17)
      T.draw(rx0 + T.px(96), hy0 + T.px(18), "HOLDING", C.accent)
      T.unfont()
      T.font(21)
      T.draw(rx0 + T.px(96), hy0 + T.px(44), T.fit(held.name or "", x1 - rx0 - T.px(110)))
      T.unfont()
      T.font(16)
      T.draw(rx0 + T.px(96), hy0 + T.px(76), "A place  X put away  LB destroy  RB drop", C.dim)
      T.unfont()
    end
  end
  if W.finish() then eq.ShowWindow(INV, false) end
end)
