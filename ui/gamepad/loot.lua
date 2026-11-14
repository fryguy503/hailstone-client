-- Loot (gamepad pack): stands in for LootWnd. The corpse's items in a grid, the focused one described beside it.
-- A loots the item (the slot's right click: auto-loot into your bags), X takes it onto the cursor (its left click),
-- Y shows the item's details (its held right button), LB loots everything, RB puts every item's link into the chat
-- input (Link All), B closes the corpse. Every action is a
-- click on the stock window's slot or button, so the game's loot rules (no drop, lore, confirmation) apply.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local C = T.C

local LOOT = "LootWnd"
local SLOTS = 34
-- The pad's focus when its item leaves the corpse (looted): it goes to the item now in that place (the last one), or
-- to Done once the corpse is empty, so A can go on looting without a D-pad press to find the focus again.
local lastFocus, refocus

ui.window("Loot", function()
  if not W.shown(LOOT) then
    lastFocus, refocus = nil, nil -- (the next corpse opens with the focus where the window puts it, not on Done)
    return
  end
  T.frame()
  local corpse = eq.WindowText(LOOT, "LW_CorpseName") or ""
  if W.begin("Loot", { w = 1180, h = 700, title = "Loot", subtitle = corpse,
        hints = { { "a", "Loot" }, { "x", "Take" }, { "y", "Details" }, { "lb", "Loot all" }, { "rb", "Link all" },
        { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local cols, size, gap = 8, T.px(84), T.px(10)
    -- the items present (the window's slots in order; empty ones are left out)
    local items = {}
    for i = 0, SLOTS - 1 do
      local it = eq.SlotItem(LOOT, "LW_LootSlot" .. i)
      if it then items[#items + 1] = { i = i, item = it } end
    end
    local focusedItem, focusedChild
    if #items == 0 then
      T.font(24)
      T.draw(x0, y0 + T.px(20), "Nothing left on this corpse.", C.dim)
      T.unfont()
    end
    local anyFocused, focusedAt = false, nil
    for n, e in ipairs(items) do
      if (n - 1) % cols ~= 0 then ui.SameLine(0, gap) end
      local child = "LW_LootSlot" .. e.i
      if refocus == n then ui.SetKeyboardFocusHere() refocus = nil end
      local clicked, focused = T.slot("##loot" .. e.i, e.item, size, {}, pad)
      if focused then
        focusedItem, anyFocused, focusedAt = e.item, true, n
        focusedChild = child
        if W.pressed("x") then eq.ClickChild(LOOT, child, "left") end
        if W.pressed("y") then eq.ClickChild(LOOT, child, "rightheld") end
      end
      if clicked then eq.ClickChild(LOOT, child, "rightup") end -- A / click: auto-loot
      if ui.IsItemClicked(ui.Mouse.Right) then eq.ClickChild(LOOT, child, "left") end
    end
    -- the focused item, on the right
    local gx1 = x0 + cols * size + (cols - 1) * gap
    I.summary(focusedItem, nil, gx1 + T.px(24), y0, x1, y1 - T.px(90),
      focusedChild and eq.CompareSlotItem(LOOT, focusedChild) or nil)
    -- Loot All, Link All and Done
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(64)
    ui.SetCursorPos(gx1 + T.px(24) - wx, y1 - bh - wy)
    local bw = (x1 - gx1 - T.px(24) - 2 * T.px(12)) / 3
    local enabled = eq.ChildState(LOOT, "LootAllButton")
    local clicked, focused = T.button("##lootall", "Loot All", bw, bh, { glyph = "lb", disabled = enabled == false or #items == 0 }, pad)
    if clicked then eq.ClickChild(LOOT, "LootAllButton", "left") end
    anyFocused = anyFocused or focused
    ui.SameLine(0, T.px(12))
    -- Link All (BroadcastButton): the game's handler links whatever the corpse holds
    clicked, focused = T.button("##linkall", "Link All", bw, bh, { glyph = "rb", disabled = #items == 0 }, pad)
    if clicked then eq.ClickChild(LOOT, "BroadcastButton", "left") end
    anyFocused = anyFocused or focused
    ui.SameLine(0, T.px(12))
    if refocus == 0 then ui.SetKeyboardFocusHere() refocus = nil end
    clicked, focused = T.button("##done", "Done", bw, bh, { glyph = "b" }, pad)
    if clicked then eq.ClickChild(LOOT, "DoneButton", "left") end
    anyFocused = anyFocused or focused
    if anyFocused then
      lastFocus = focusedAt or 0 -- 0: a button
    elseif lastFocus and W.focused() then -- the focused item was looted
      refocus = (lastFocus > 0 and #items > 0) and math.min(lastFocus, #items) or 0
      lastFocus = nil
    end
    if W.pressed("lb") and #items > 0 then eq.ClickChild(LOOT, "LootAllButton", "left") end
    if W.pressed("rb") and #items > 0 then eq.ClickChild(LOOT, "BroadcastButton", "left") end
  end
  if W.finish() then eq.ClickChild(LOOT, "DoneButton", "left") end
end)
