-- Pieces shared by the windows that move items and coin between you and someone else (bank.lua, trade.lua): a slot
-- that takes the pad's item buttons, your bags with the focused bag's contents, a row of coin buttons, and the
-- "Holding" panel. Every click is the game's (the caller's click function: hs.ClickSlot, hs.ClickChild, ...).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local C = T.C
local X = {}

X.COINS = { { "pp", T.C.platinum }, { "gp", T.C.gold }, { "sp", T.C.silver }, { "cp", T.C.copper } }
X.SIZE, X.GAP = 76, 8 -- slot size and gap, design px

function X.isBag(item) return item and (item.bagSlots or 0) > 0 end

-- A frame's state: what the pad holds and what is focused. ctx = X.frame(nav, sections); ctx.focusedItem,
-- ctx.focusedWhere. nav (kept by the window) and sections (names in order) give LB / RB, which move the focus to the
-- previous / next section (X.section marks where each begins): a grid of slots is a long way across with the D-pad.
local deferred = {}
-- Runs fn once A is released: a click that opens the quantity dialog, so the A still held does not keep the focus
-- (and the dialog behind) in this window. Dropped if the window it was asked in closes first (W.deferred).
function X.afterA(fn) W.deferred(deferred, fn) end

function X.frame(nav, sections)
  W.runDeferred(deferred)
  local ctx = { held = API.inventory.Item(API.inventory.CursorSlot or 33), nav = nav, sections = sections }
  if API.cursor.Coin then ctx.coinType, ctx.coinAmount = API.cursor.Coin() end
  return ctx
end

-- The section `name` begins with the next item: LB / RB's jump lands on it. (The buttons are read at the first
-- section, inside the window: W.pressed needs it focused.)
function X.section(ctx, name)
  local nav, sections = ctx.nav, ctx.sections
  if nav and sections and #sections > 0 and not ctx.navRead then
    ctx.navRead = true
    local step = (W.pressed("rb") and 1 or 0) - (W.pressed("lb") and 1 or 0)
    if step ~= 0 then
      local at = 0 -- (none yet: RB goes to the first, LB to the last)
      for i, n in ipairs(sections) do if n == nav.at then at = i end end
      local i = (at == 0 and step < 0) and #sections or at + step
      nav.jump = sections[(i - 1) % #sections + 1]
    end
  end
  ctx.drawing = name
  if ctx.nav and ctx.nav.jump == name then
    ui.SetKeyboardFocusHere()
    ctx.nav.jump = nil
  end
end

-- The item just drawn has the focus: its section is where the pad is.
local function focusedIn(ctx)
  if ctx and ctx.nav and ctx.drawing then ctx.nav.at = ctx.drawing end
end

-- An item slot at the cursor: A click("left"), X with an item held puts it away (/autoinventory) or calls
-- opts.put (a trade: into the offer), else uses the item (click("rightup"), not for a bag), Y click("rightheld").
-- Returns whether it has the focus.
local acted = false -- a cell clicked or put something away (X.bags reads its slots again)
local slotOpts = {}
function X.cell(ctx, id, item, where, click, opts)
  opts = opts or T.NO_OPTS
  slotOpts.label, slotOpts.selected = opts.label, opts.selected
  local clicked, focused, sx0, sy0, sx1 = T.slot(id, item, T.px(X.SIZE), slotOpts, pad)
  if focused then
    focusedIn(ctx)
    ctx.focusedItem, ctx.focusedWhere = item, where
    if W.pressed("x") then
      acted = true
      if ctx.held then
        if opts.put then opts.put() else API.chat.Submit(1, "/autoinventory") end
      elseif item and not X.isBag(item) and not opts.noUse then
        click("rightup")
      end
    end
    if W.pressed("y") and item then
      acted = true
      click("rightheld")
    end
    if ctx.held then T.itemIcon(ctx.held.icon, sx1 - T.px(30), sy0 - T.px(14), sx1 + T.px(10), sy0 + T.px(26)) end
  end
  if clicked then
    acted = true
    if item and item.stackable and (item.count or 1) > 1 and not ctx.held then
      X.afterA(function() click("left") end) -- a stack asks how many
    else
      click("left")
    end
  end
  if ui.IsItemClicked(ui.Mouse.Right) then
    acted = true
    click("rightup")
  end
  return focused
end

-- Starts slot `i` of a grid of `per` across at screen x (window at wx, wy).
function X.row(x, wx, wy, i, per)
  if i % per == 0 then ui.SetCursorPos(x - wx, select(2, ui.GetCursorScreenPos()) - wy) else ui.SameLine(0, T.px(X.GAP)) end
end

-- The bag slots are read once a call (hs.Item, hs.SlotName) and read again only after a cell acted (it may have
-- moved an item); the slots' ids and click functions are made once.
local bagItems = {}
local bagCells, innerCells = {}, {}
for slot = 23, 32 do
  bagCells[slot] = { id = "##inv" .. slot, click = function(b) API.inventory.ClickSlot(slot, nil, b) end }
end
local function innerCell(slot, i)
  local bySlot = innerCells[slot]
  if not bySlot then
    bySlot = {}
    innerCells[slot] = bySlot
  end
  local cell = bySlot[i]
  if not cell then
    cell = { id = "##ib" .. slot .. "_" .. i, click = function(b) API.inventory.ClickSlot(slot, i, b) end }
    bySlot[i] = cell
  end
  return cell
end
local function bagItem(slot, stale)
  if stale then return API.inventory.Item(slot) end
  return bagItems[slot]
end
local cellOpts = {}
-- Your ten bag slots (`per` across) and the contents of the bag last focused (state.bag, else the first bag), from
-- the cursor at screen x, `w` wide. opts.put: what X does with an item held (see X.cell).
function X.bags(ctx, state, x, w, per, opts)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, select(2, ui.GetCursorScreenPos()) - wy)
  T.heading("Your bags", w)
  X.section(ctx, "bags")
  local put = opts and opts.put
  cellOpts.put = put
  acted = false
  for slot = 23, 32 do
    X.row(x, wx, wy, slot - 23, per)
    local item = API.inventory.Item(slot)
    bagItems[slot] = item
    local name = API.inventory.SlotName(slot)
    local cell = bagCells[slot]
    cellOpts.label = name
    if X.cell(ctx, cell.id, item, name, cell.click, cellOpts) and X.isBag(item) then
      state.bag = slot
    end
  end
  cellOpts.label = nil
  -- (after a cell acted a slot may hold another item: read again, as each was read after the cells before)
  local stale = acted
  if not (state.bag and X.isBag(bagItem(state.bag, stale))) then
    state.bag = nil
    for slot = 23, 32 do
      if X.isBag(bagItem(slot, stale)) then state.bag = slot break end
    end
  end
  local bag = state.bag and bagItem(state.bag, stale)
  if not X.isBag(bag) then return end
  ui.SetCursorPos(x - wx, select(2, ui.GetCursorScreenPos()) + T.px(8) - wy)
  T.heading(bag.name, w)
  local slot = state.bag
  for i = 0, bag.bagSlots - 1 do
    X.row(x, wx, wy, i, per)
    local cell = innerCell(slot, i)
    X.cell(ctx, cell.id, API.inventory.Item(slot, i), bag.name .. ", slot " .. (i + 1), cell.click, cellOpts)
  end
end

-- Four coin buttons (platinum .. copper) across `w` from screen x, on the cursor's line: amount(i) the pile (i 0..3),
-- click(i) on A; opts.outlined(i): the button is outlined (the game's pick).
function X.coinRow(id, x, w, amount, click, opts)
  opts = opts or {}
  if opts.section then X.section(opts.ctx, opts.section) end
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, select(2, ui.GetCursorScreenPos()) - wy)
  local gap = T.px(8)
  local bw = (w - 3 * gap) / 4
  for i = 0, 3 do
    if i > 0 then ui.SameLine(0, gap) end
    local clicked, focused, x0, y0, x1, y1 = T.button("##" .. id .. i, "", bw, T.px(64), { disabled = opts.disabled }, pad)
    if focused then focusedIn(opts.ctx) end
    T.font(20)
    local v = tostring(amount(i) or 0)
    T.textIn(x0 + T.px(8), y0, x1 - T.px(8), y0 + (y1 - y0) * 0.62, T.fit(v, x1 - x0 - T.px(16)), C.text, 0.5)
    T.unfont()
    T.font(15)
    T.textIn(x0, y0 + (y1 - y0) * 0.55, x1, y1 - T.px(4), X.COINS[i + 1][1], X.COINS[i + 1][2], 0.5)
    T.unfont()
    if opts.outlined and opts.outlined(i) then T.outline(x0, y0, x1, y1, C.accent, 0.9, T.px(10), T.px(3)) end
    if clicked then click(i) end
  end
end

-- Your purse's coin buttons (the inventory's, CInventoryWnd::ClickedMoneyButton): coin held drops into the purse,
-- else the quantity dialog asks how much comes onto the cursor.
function X.purse(ctx, x, w, section)
  local pp, gp, sp, cp = API.character.Money()
  local mine = { pp, gp, sp, cp }
  X.coinRow("purse", x, w, function(i) return mine[i + 1] end,
    function(i) X.afterA(function() API.windows.ClickChild("InventoryWindow", "IW_Money" .. i, "left") end) end,
    { ctx = ctx, section = section })
end

-- The height X.holding draws (0 when nothing is held).
function X.holdingHeight(ctx) return (ctx.held or ctx.coinType) and T.px(100) or 0 end

-- What the cursor holds, in x0, y0 .. x1 (an item or coin); `tip` the line under it.
function X.holding(ctx, x0, y0, x1, tip)
  local h = X.holdingHeight(ctx)
  if h == 0 then return end
  local y1 = y0 + h
  T.rect(x0, y0, x1, y1, C.accent, 0.16, T.px(14))
  T.outline(x0, y0, x1, y1, C.accent, 0.6, T.px(14), T.px(2))
  local s = T.px(60)
  if ctx.held then T.itemIcon(ctx.held.icon, x0 + T.px(16), y0 + (h - s) / 2, x0 + T.px(16) + s, y0 + (h + s) / 2) end
  T.font(17)
  T.draw(x0 + T.px(92), y0 + T.px(14), "HOLDING", C.accent)
  T.unfont()
  T.font(21)
  local what = ctx.held and (ctx.held.name or "") or (ctx.coinAmount .. " " .. X.COINS[ctx.coinType + 1][1])
  T.draw(x0 + T.px(92), y0 + T.px(40), T.fit(what, x1 - x0 - T.px(106)))
  T.unfont()
  T.font(16)
  T.draw(x0 + T.px(92), y0 + T.px(70), T.fit(tip or "", x1 - x0 - T.px(106)), C.dim)
  T.unfont()
end

return X
