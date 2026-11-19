-- Bank (gamepad pack): stands in for the bank window (BigBankWnd, or BankWnd for a 16 slot bank), which the game
-- opens at a banker (A on the targeted banker: its right click). The bank and the shared bank on the left, your bags
-- in the middle, the focused item, what is held and the coin on the right. Every slot is the game's slot logic (the
-- bank window's slot click, hs.BankSlotClick inside a bank bag as its container window's slots, hs.ClickSlot in your
-- bags), so moving an item is the stock window's pick up and put down:
--   A   pick up / put down / swap (the left click: the item goes on the cursor, shown as "Holding")
--   X   holding an item: put it away in your bags (/autoinventory); else use it (the quick right click)
--   Y   the item's full details (the held right button)
--   LB / RB  the previous / next section (the bank, your bags, the coin)
-- Focusing a bag shows its contents under it. The coin buttons are the stock ones: a bank pile comes onto the cursor
-- whole, your purse's through the quantity dialog, and coin held drops on the pile A is pressed on.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local X = dofile("lib/exchange.lua")
local C = T.C

local SCREENS = { { "BigBankWnd", "BIGB_", 24 }, { "BankWnd", "BW_", 16 } }
local openBank = nil -- { slot, shared } of the bank bag whose contents are shown
local mine = {}      -- the bag shown in your bags (X.bags)
local nav = {}       -- LB / RB between the sections (X.frame)
local SECTIONS = { "bank", "bags", "coin" }

ui.window("Bank", function()
  local BW, P, size
  for _, s in ipairs(SCREENS) do
    if W.shown(s[1]) then BW, P, size = s[1], s[2], s[3] break end
  end
  if not BW then
    openBank, mine.bag = nil, nil
    return
  end
  local banker = API.windows.Text(BW, P .. "BankerName") or ""
  local ctx = X.frame(nav, SECTIONS)
  if W.begin("Bank", { w = 1840, h = 1000, title = "Bank", subtitle = banker,
        hints = { { "a", "Pick up / place" }, { "x", ctx.held and "Put away" or "Use" }, { "y", "Details" }, { "lb", "" }, { "rb", "Section" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local rightW = T.px(470)
    local leftW = T.px(8 * X.SIZE + 7 * X.GAP)
    local midX = x0 + leftW + T.px(40)
    local rx0 = x1 - rightW
    local function bankCell(id, child, where, label, shared, n)
      local item = API.inventory.SlotItem(BW, child)
      if X.cell(ctx, id, item, where, function(b) API.windows.ClickChild(BW, child, b) end, { label = label }) and X.isBag(item) then
        openBank = { n, shared }
      end
    end
    -- the bank
    ui.SetCursorPos(x0 - wx, y0 - wy)
    T.heading("Bank", leftW)
    X.section(ctx, "bank")
    for n = 0, size - 1 do
      X.row(x0, wx, wy, n, 8)
      bankCell("##bank" .. n, P .. "BankSlot" .. n, "Bank slot " .. (n + 1), tostring(n + 1), false, n)
    end
    ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(8) - wy)
    T.heading("Shared bank", leftW)
    for n = 0, 1 do
      X.row(x0, wx, wy, n, 8)
      bankCell("##shared" .. n, P .. "SharedBankSlot" .. n, "Shared bank slot " .. (n + 1), "S" .. (n + 1), true, n)
    end
    -- the bank bag last focused (the first bank bag until one is)
    if not (openBank and X.isBag(API.inventory.BankItem(openBank[1], nil, openBank[2]))) then
      openBank = nil
      for n = 0, size - 1 do
        if X.isBag(API.inventory.BankItem(n)) then openBank = { n, false } break end
      end
    end
    local bag = openBank and API.inventory.BankItem(openBank[1], nil, openBank[2])
    if X.isBag(bag) then
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(8) - wy)
      T.heading(bag.name, leftW)
      local slot, shared = openBank[1], openBank[2]
      for i = 0, bag.bagSlots - 1 do
        X.row(x0, wx, wy, i, 8)
        X.cell(ctx, "##bb" .. (shared and "s" or "") .. slot .. "_" .. i, API.inventory.BankItem(slot, i, shared),
          bag.name .. ", slot " .. (i + 1), function(b) API.inventory.BankSlotClick(slot, i, b, shared) end)
      end
    end
    -- your bags
    ui.SetCursorPos(midX - wx, y0 - wy)
    X.bags(ctx, mine, midX, rx0 - midX - T.px(40), 5)
    -- the right column: the focused item, what is held, the coin
    local canOpenShroudBank = API.windows.ChildShown(BW, "AltStorageButton") == true
    local coinH = T.px(canOpenShroudBank and 360 or 300)
    local holdH = X.holdingHeight(ctx)
    local sumY1 = y1 - coinH - (holdH > 0 and holdH + T.px(12) or 0) - T.px(12)
    I.summary(ctx.focusedItem, ctx.focusedWhere, rx0, y0, x1, sumY1)
    X.holding(ctx, rx0, sumY1 + T.px(12), x1, ctx.held and "A: place   X: put away" or "A on a coin pile: drop it there")
    local cw = x1 - rx0
    ui.SetCursorPos(rx0 - wx, y1 - coinH - wy)
    T.heading("Bank coin", cw)
    X.coinRow("bankcoin", rx0, cw, function(i) return tonumber((API.windows.Text(BW, P .. "Money" .. i))) end,
      function(i) API.windows.ClickChild(BW, P .. "Money" .. i, "left") end, { ctx = ctx, section = "coin" })
    ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
    T.heading("Your coin", cw)
    X.purse(ctx, rx0, cw)
    -- Auto-Bank (the cursor's item into the bank) and the shared bank's platinum
    ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(4) - wy)
    local bw = (cw - T.px(10)) / 2
    if T.button("##auto", "Auto-Bank", bw, T.px(60), { disabled = API.windows.ChildState(BW, "AutoButton") == false }, pad) then
      API.windows.ClickChild(BW, "AutoButton", "left")
    end
    ui.SameLine(0, T.px(10))
    local shared = API.windows.Text(BW, P .. "SharedMoney0") or "0"
    if T.button("##sharedcoin", "Shared: " .. shared .. " pp", bw, T.px(60), {}, pad) then
      API.windows.ClickChild(BW, P .. "SharedMoney0", "left")
    end
    if canOpenShroudBank and T.button("##openShroudBank", "Shroud Bank", cw, T.px(50), {}, pad) then
      API.windows.ClickChild(BW, "AltStorageButton", "left")
    end
  end
  if W.finish() then API.windows.ClickChild(BW, "DoneButton", "left") end
end)
