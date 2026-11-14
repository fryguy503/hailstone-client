-- Item slots in the modern style: an icon tile with the stack count, a quick tooltip, and the game's slot logic
-- (the same clicks classic's slots deliver): left click picks up, drops or swaps (Shift: the whole stack, Ctrl:
-- one), a quick right click uses the item (inspect, open a bag, cast a clicky), holding the right button shows the
-- item's display until it is let go. A slot of the character's own (opts.held) takes its left click on the release,
-- as the game's CInvSlot::HandleLButtonUp does: holding the left button on it half a second (HandleLButtonHeld), or
-- dragging off it, puts a hotbutton for it on the cursor instead ("leftheld"), to be let go of over a hot bar.
local T = dofile("lib/theme.lua")
local tools = dofile("lib/itemtools.lua")
local CL = dofile("lib/compare_lines.lua")
local C = T.color

local items = {}

local MAGIC = C.magic
local HELD = 0.5 -- seconds, as the right button's hold

-- The left press on a slot with a held action: { key, start, seen, tried, made }.
local leftPress = nil

local function leftPressFrame(key, onClick, hovered)
  local press = leftPress
  if not press then return end
  if press.key ~= key then
    if ui.GetTime() - press.seen > 0.25 then leftPress = nil end -- its slot is no longer drawn: no click
    return
  end
  press.seen = ui.GetTime()
  if ui.IsMouseDown(ui.Mouse.Left) then
    -- held inside the slot (the game's HandleLButtonHeld needs the mouse there) or dragged off it: once a press
    if not press.tried and (ui.IsMouseDragging(ui.Mouse.Left) or (hovered and ui.GetTime() - press.start > HELD)) then
      press.tried = true
      press.made = onClick("leftheld") == true
    end
  else
    leftPress = nil
    if not press.made then onClick("left") end -- nothing went on the cursor: the press is the click
  end
end

-- Hailstone's item comparison under the item's lines (docs/item-compare.md): `compare` is eq.Compare*'s table.
-- Shift shows the other slot of a pair (ears, wrists, rings).
local KIND = { good = "good", bad = "bad", mixed = "gold" } -- the palette's colour names (read as drawn: it may change)
local function comparison(compare)
  if not compare or eq.Option("item_compare") == 0 then return end
  ui.Separator()
  local index = (#compare.slots > 1 and ui.GetIO().shift) and 2 or 1
  for _, line in ipairs(CL.lines(compare, index, { verdict = eq.Option("item_compare_verdict") ~= 0 })) do
    local text, kind = line[1], line[2]
    local col = KIND[kind] and C[KIND[kind]]
    if col then ui.TextColored(col[1], col[2], col[3], 1, text)
    elseif kind == "muted" then ui.TextDisabled(text)
    else ui.Text(text) end
  end
  local other = CL.otherSlotName(compare, index)
  if other then ui.TextDisabled((index == 1 and "Hold" or "Let go of") .. " Shift to compare with your " .. other) end
end

-- A tooltip of a name, the comparison (when there is one) and a footer: for rows that are not slots (the merchant's
-- goods). nil compare: just the name and footer.
function items.NameTooltip(name, compare, footer)
  ui.BeginTooltip()
  ui.Text(name)
  comparison(compare)
  if footer then ui.TextDisabled(footer) end
  ui.EndTooltip()
end

-- items.Tooltip(item, where [, compare]): compare is eq.CompareItem's (or CompareSlotItem's ...) table or nil.
function items.Tooltip(item, where, compare)
  ui.BeginTooltip()
  if item.magic then ui.TextColored(MAGIC[1], MAGIC[2], MAGIC[3], 1, item.name) else ui.Text(item.name) end
  local tags = {}
  if item.magic then tags[#tags + 1] = "Magic" end
  if item.lore then tags[#tags + 1] = "Lore" end
  if where and where ~= "" then tags[#tags + 1] = where end
  if #tags > 0 then ui.TextDisabled(table.concat(tags, "  \xC2\xB7  ")) end
  local stats = {}
  if item.ac and item.ac ~= 0 then stats[#stats + 1] = "AC " .. item.ac end
  if item.hp and item.hp ~= 0 then stats[#stats + 1] = "HP " .. item.hp end
  if item.mana and item.mana ~= 0 then stats[#stats + 1] = "Mana " .. item.mana end
  if #stats > 0 then ui.Text(table.concat(stats, "   ")) end
  ui.TextDisabled(string.format("Weight %.1f", item.weight or 0))
  if item.stackable then ui.TextDisabled(string.format("Stack %d / %d", item.count, item.stackSize)) end
  if item.bagSlots and item.bagSlots > 0 then ui.TextDisabled(item.bagSlots .. " slot container") end
  comparison(compare)
  ui.TextDisabled("Hold right-click for details")
  ui.EndTooltip()
end

-- items.Slot(id, x, y, size, s, item, placeholder, where, onClick [, opts])
--   item: eq.Item(...) or nil; placeholder: a short name drawn in an empty slot ("Head"); where: the tooltip's place
--   onClick(button [, outside]): "left", "leftheld" (with opts.held; true when a hotbutton went on the cursor) or a
--   right-button phase. opts: selected (a ring the window sets), held (the left click on release, the hold or a drag
--   "leftheld"), compare (a function giving eq.Compare*'s table for the slot's item, asked only while hovered)
-- items.HELD and items.SELECTED: opts tables for a caller to pass rather than a new one each slot each frame.
items.HELD = { held = true }
items.SELECTED = { selected = true }
local NO_OPTS = {}
local MAGIC_RING = { MAGIC[1], MAGIC[2], MAGIC[3], 0.45 }
local ICON_FULL, ICON_EMPTY = { radius = 6, wellAlpha = 1 }, { radius = 6, wellAlpha = 0.7 }
function items.Slot(id, x, y, size, s, item, placeholder, where, onClick, opts)
  opts = opts or NO_OPTS
  local tex, u0, v0, u1, v1
  if item then tex, u0, v0, u1, v1 = eq.ItemIcon(item.icon) end
  local ring = nil
  if item and item.magic then ring = MAGIC_RING end
  if opts.selected then ring = C.gold end
  T.Icon(x, y, size, s, tex, u0, v0, u1, v1, ring, item and ICON_FULL or ICON_EMPTY)
  if not item and placeholder and placeholder ~= "" then
    ui.PushClipRect(x, y, x + size, y + size, true)
    local _, th = T.TextSize("Ag", T.font.tiny)
    T.Text(x + size / 2, y + (size - th) / 2, placeholder, C.faint, T.font.tiny, "center")
    ui.PopClipRect()
  end
  if item and item.cooldown then -- a clicky's recast (the game's slot cool-down): the part still to go, from below
    local top = y + size * item.cooldown
    ui.DrawRectFilled(x, top, x + size, y + size, 0, 0, 0, 0.55, 6 * s)
    ui.DrawLine(x, top, x + size, top, C.accent[1], C.accent[2], C.accent[3], 0.8, 1.5 * s)
  end
  if item and item.stackable and item.count >= 1 then -- a single one of a stackable shows its 1 too: it stacks
    T.Text(x + size - 3 * s, y + size - 15 * s, tostring(item.count), C.text, T.font.tiny, "right")
  end
  T.Hit(id, x, y, x + size, y + size)
  local key, hovered = ui.GetItemID(), ui.IsItemHovered()
  T.HoverFx(x, y, x + size, y + size, s, 6)
  if ui.IsItemClicked(ui.Mouse.Left) then
    if opts.held then
      leftPress = { key = key, start = ui.GetTime(), seen = ui.GetTime(), tried = false, made = false }
    else
      onClick("left")
    end
  elseif ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
    T.RightPressStart(key, onClick)
    T.TakeRightClick()
  elseif ui.IsItemClicked(ui.Mouse.Right) then
    onClick("rightup") -- Alt: the item preview, not the window menu
    T.TakeAltRightClick()
  end
  T.RightPressFrame(key, onClick, hovered)
  if opts.held then leftPressFrame(key, onClick, hovered) end
  if hovered and not ui.IsMouseDown(ui.Mouse.Right) then
    if item then items.Tooltip(item, where, opts.compare and opts.compare())
    elseif where and where ~= "" then ui.SetTooltip(where) end
  end
end

-- A slot of the character's inventory (eq.Item / eq.ClickSlot); while a marking mode is on (lib/itemtools.lua) a
-- click marks instead, and its marks show. Returns the slot's item (eq.Item(slot, bagSlot)), nil when empty.
-- Each slot keeps one record (its id, place and click handler, made the first time it draws); the handler reads the
-- item the slot drew last, as a handler made for that frame would.
local invSlots = {} -- [slot][bagSlot or -1] = { id, place, item, onClick }
local function invSlot(slot, bagSlot)
  local bySub = invSlots[slot]
  if not bySub then bySub = {}; invSlots[slot] = bySub end
  local k = bagSlot or -1
  local rec = bySub[k]
  if rec then return rec end
  rec = { id = "##inv" .. slot .. "." .. tostring(bagSlot), place = tools.Place("inventory", slot, bagSlot) }
  rec.onClick = function(button, outside)
    if tools.Intercept(rec.place, rec.item, button) then return end
    return eq.ClickSlot(slot, bagSlot, button, outside)
  end
  rec.opts = { held = true, compare = function() return eq.CompareItem(slot, bagSlot) end }
  bySub[k] = rec
  return rec
end
function items.InvSlot(slot, bagSlot, x, y, size, s, placeholder)
  local item = eq.Item(slot, bagSlot)
  local where = eq.SlotName(slot) or ""
  local rec = invSlot(slot, bagSlot)
  rec.item = item
  items.Slot(rec.id, x, y, size, s, item, placeholder, where, rec.onClick, rec.opts)
  if item then tools.Marks(x, y, size, s, item, rec.place) end
  return item
end

return items
