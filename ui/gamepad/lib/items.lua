-- Item helpers for the gamepad windows: the summary panel shown beside a slot grid for the focused item (no
-- hover tooltips on a couch: what the focus is on is always described), from the hs.Item table.
local API = hs.UI
local T = dofile("lib/theme.lua")
local CL = dofile("lib/compare_lines.lua")
local I = {}
local C = T.C

-- The facts hs.Item gives, as short lines.
local function facts(item)
  local out = {}
  local tags = {}
  if item.magic then tags[#tags + 1] = "MAGIC" end
  if item.lore then tags[#tags + 1] = "LORE" end
  if #tags > 0 then out[#out + 1] = { table.concat(tags, "  "), C.accent } end
  if (item.ac or 0) ~= 0 then out[#out + 1] = { "AC " .. item.ac } end
  if (item.hp or 0) ~= 0 then out[#out + 1] = { "HP " .. string.format("%+d", item.hp) } end
  if (item.mana or 0) ~= 0 then out[#out + 1] = { "Mana " .. string.format("%+d", item.mana) } end
  if item.stackable then out[#out + 1] = { string.format("Stack %d / %d", item.count or 1, item.stackSize or 1) } end
  if (item.bagSlots or 0) > 0 then out[#out + 1] = { item.bagSlots .. " slots" } end
  if (item.charges or 0) > 0 and not item.stackable then out[#out + 1] = { "Charges " .. item.charges } end
  out[#out + 1] = { string.format("Weight %.1f", item.weight or 0), C.dim }
  return out
end

-- Hailstone's item comparison (docs/item-compare.md): hs.Compare*'s table as lines from y down to y1 at most, between
-- x and x1; the best slot's comparison (a pair's other slot is named at the end). Returns the y below. Nothing when
-- the Options turn it off. skip: the keys of changes already shown beside their stats (the item display's grid).
local KIND = { good = "good", bad = "bad", mixed = "gold", muted = "dim" }
function I.compareColor(kind) return C[KIND[kind] or "text"] or C.text end
function I.comparison(compare, x, y, x1, y1, skip)
  if not compare or API.settings.Option("item_compare") == 0 then return y end
  T.font(18)
  local _, h = ui.CalcTextSize("Ag")
  local lines = CL.lines(compare, 1, { verdict = API.settings.Option("item_compare_verdict") ~= 0, skip = skip })
  local other = CL.otherSlotName(compare, 1)
  if other then lines[#lines + 1] = { "Also fits your " .. other, "muted" } end
  y = y + T.px(6)
  for _, line in ipairs(lines) do
    if y + h > y1 then break end
    T.draw(x, y, T.fit(line[1], x1 - x), C[KIND[line[2]] or "text"] or C.text)
    y = y + h + T.px(3)
  end
  T.unfont()
  return y
end

-- The focused item's panel in x0, y0 .. x1, y1: icon, name and facts; `where` (the slot's name) under the name;
-- `compare` (hs.Compare*'s table, or nil) the item against what the player wears, below the facts.
function I.summary(item, where, x0, y0, x1, y1, compare)
  T.rect(x0, y0, x1, y1, C.sunken, 1, T.px(14))
  local p = T.px(18)
  if not item then
    T.font(20)
    T.textIn(x0 + p, y0 + p, x1 - p, y0 + p + T.px(30), where and where ~= "" and (where .. " (empty)") or "Empty", C.faint, 0)
    T.unfont()
    return
  end
  local s = T.px(72)
  T.rect(x0 + p, y0 + p, x0 + p + s, y0 + p + s, C.raised, 1, T.px(10))
  T.itemIcon(item.icon, x0 + p + T.px(6), y0 + p + T.px(6), x0 + p + s - T.px(6), y0 + p + s - T.px(6))
  local tx = x0 + p + s + T.px(14)
  T.font(24)
  -- the name, wrapped on two lines at most
  local name = item.name or ""
  local _, lh = ui.CalcTextSize("Ag")
  local width = x1 - p - tx
  local first, rest = name, ""
  if ui.CalcTextSize(name) > width then
    local cut = #name
    while cut > 1 and (name:sub(cut, cut) ~= " " or ui.CalcTextSize(name:sub(1, cut - 1)) > width) do cut = cut - 1 end
    if cut > 1 then first, rest = name:sub(1, cut - 1), name:sub(cut + 1) end
  end
  T.draw(tx, y0 + p, T.fit(first, width))
  if rest ~= "" then T.draw(tx, y0 + p + lh, T.fit(rest, width)) end
  T.unfont()
  local y = y0 + p + s + T.px(14)
  if where and where ~= "" then
    T.font(17)
    T.draw(x0 + p, y, where, C.dim)
    local _, h = ui.CalcTextSize("Ag")
    y = y + h + T.px(8)
    T.unfont()
  end
  T.font(20)
  local _, h = ui.CalcTextSize("Ag")
  for _, f in ipairs(facts(item)) do
    if y + h > y1 - p then break end
    T.draw(x0 + p, y, f[1], f[2])
    y = y + h + T.px(4)
  end
  T.unfont()
  I.comparison(compare, x0 + p, y, x1 - p, y1 - p)
end

-- The ImGui id "##<prefix><slot>_<bag>" of a cell of a bag grid (bag nil: the bag slot itself), made once, not each
-- frame.
local cellIds = {}
function I.cellId(prefix, slot, bag)
  local byPrefix = cellIds[prefix]
  if not byPrefix then
    byPrefix = {}
    cellIds[prefix] = byPrefix
  end
  local bySlot = byPrefix[slot]
  if not bySlot then
    bySlot = {}
    byPrefix[slot] = bySlot
  end
  local b = bag or -1
  local id = bySlot[b]
  if not id then
    id = "##" .. prefix .. slot .. "_" .. b
    bySlot[b] = id
  end
  return id
end

-- The ten bag slots' items read into `into` (slots 23..32), once a frame for the windows that walk them twice (the
-- loose items, then each bag's contents).
function I.readBagSlots(into)
  for slot = 23, 32 do into[slot] = API.inventory.Item(slot) end
  return into
end

return I
