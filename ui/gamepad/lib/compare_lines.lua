-- Item comparison (Hailstone; docs/item-compare.md): the lines both packs show for hs.CompareItem /
-- hs.CompareSlotItem / hs.CompareBagSlotItem's table, as { text, kind } with kind one of "good", "bad", "mixed",
-- "muted" and "text" (each pack draws them in its own colours). Nothing here draws, so the packs share it: the
-- gamepad pack's lib/compare_lines.lua is a copy of this file, kept the same (tests/item_compare_test.cpp).
local M = {}

M.MAX_CHANGES = 8 -- the tooltip's; the item display's section shows them all

local VERDICTS = {
  upgrade = { "Upgrade", "good" },
  downgrade = { "Downgrade", "bad" },
  same = { "No change that matters for your class", "muted" },
}

-- The verdict of one slot's comparison: text, kind.
function M.verdict(slot)
  if slot.verdict == "mixed" then
    if slot.score > 0 then return "Mixed: better overall", "mixed" end
    if slot.score < 0 then return "Mixed: worse overall", "mixed" end
    return "Mixed", "mixed"
  end
  local v = VERDICTS[slot.verdict] or VERDICTS.same
  return v[1], v[2]
end

local function value(key, v)
  if key == "weaponRatio" then return string.format("%.2f", v / 100) end
  if key == "haste" then return v .. "%" end
  return tostring(v)
end

-- A change's amount with its sign: "+50", "-2", "+10%", "+0.42".
function M.signed(key, d)
  local s = key == "weaponRatio" and string.format("%.2f", math.abs(d) / 100) or tostring(math.abs(d))
  if key == "haste" then s = s .. "%" end
  return (d > 0 and "+" or "-") .. s
end

-- The kind a change is drawn in: "good" or "bad" when it counts for the class, else "muted".
function M.kind(ch)
  if ch.weight == 0 then return "muted" end
  return ch.delta > 0 and "good" or "bad"
end

-- "+50 HP (1200 to 1250)"
function M.change(c)
  return string.format("%s %s (%s to %s)", M.signed(c.key, c.delta), c.name, value(c.key, c.before), value(c.key, c.after))
end

-- "+50 HP"
function M.short(c)
  return M.signed(c.key, c.delta) .. " " .. c.name
end

-- A slot's changes by their key (hs.Compare*'s keys: "hp", "str", "svFire", ...): for a window that shows each
-- beside the item's own value.
function M.byKey(slot)
  local out = {}
  if slot then for _, ch in ipairs(slot.changes) do out[ch.key] = ch end end
  return out
end

-- The changes in the order to show them: those that count for the class first, the biggest weighted change first,
-- then the others in the comparison's order.
function M.ordered(changes)
  local counted, other = {}, {}
  for i, c in ipairs(changes) do
    if c.weight > 0 then counted[#counted + 1] = { c, math.abs(c.delta * c.weight), i }
    else other[#other + 1] = c end
  end
  table.sort(counted, function(a, b)
    if a[2] ~= b[2] then return a[2] > b[2] end
    return a[3] < b[3]
  end)
  local out = {}
  for _, c in ipairs(counted) do out[#out + 1] = c[1] end
  for _, c in ipairs(other) do out[#out + 1] = c end
  return out
end

local function joinNames(names)
  local shown = {}
  for _, n in ipairs(names) do if n ~= "" then shown[#shown + 1] = n end end
  return table.concat(shown, " and ")
end

-- The pieces of c.slots[index or 1]'s comparison, for a pack to lay out as it likes:
--   cantUse: "Can't use: ..." or nil; slot: the slot's table, nil when the item fits none you can wear
--   heading: "Compared with your X (Slot)" (or the empty slot); against: "your X (Slot)" (the short form)
--   verdict: { text, kind } or nil (an item you cannot use has none)
--   changes: the changes to list, M.ordered, without those whose key is in opts.skip (shown elsewhere), at most
--     M.MAX_CHANGES unless opts.full; more: how many more there were; none: true when nothing changes at all
--   extra: the weapon, gains, losses and notes as { text, kind }
function M.parts(c, index, opts)
  opts = opts or {}
  local p = { changes = {}, more = 0, extra = {} }
  if not c.usable then p.cantUse = "Can't use: " .. c.whyNot end
  local s = c.slots[index or 1]
  p.slot = s
  if not s then
    p.heading = "It does not fit a slot you can wear"
    for _, n in ipairs(c.notes) do p.extra[#p.extra + 1] = { n, "muted" } end
    return p
  end
  local names = joinNames(s.current)
  if names == "" then
    p.heading = "Compared with your empty " .. s.slotName .. " slot"
    p.against = "your empty " .. s.slotName .. " slot"
  else
    p.heading = "Compared with your " .. names .. " (" .. s.slotName .. ")"
    p.against = "your " .. names .. " (" .. s.slotName .. ")"
  end
  if s.verdict ~= "cantuse" then p.verdict = { M.verdict(s) } end
  local skip = opts.skip
  local list = {}
  for _, ch in ipairs(M.ordered(s.changes)) do
    if not (skip and skip[ch.key]) then list[#list + 1] = ch end
  end
  local shown = opts.full and #list or math.min(#list, M.MAX_CHANGES)
  for i = 1, shown do p.changes[i] = list[i] end
  p.more = #list - shown
  p.none = #s.changes == 0
  local wa, wb = s.weaponAfter, s.weaponBefore
  if wa then
    local text = string.format("Weapon %d dmg / %d delay", wa.damage, wa.delay)
    if wb then text = text .. string.format(" (now %d / %d)", wb.damage, wb.delay) end
    p.extra[#p.extra + 1] = { text, "text" }
  end
  for _, g in ipairs(s.gained) do p.extra[#p.extra + 1] = { "Gains " .. g, "good" } end
  for _, l in ipairs(s.lost) do p.extra[#p.extra + 1] = { "Loses " .. l, "bad" } end
  for _, n in ipairs(c.notes) do p.extra[#p.extra + 1] = { n, "muted" } end
  return p
end

-- The lines for c.slots[index or 1], as { text, kind }. opts: verdict (show the verdict line), full (every change,
-- not M.MAX_CHANGES), skip (keys not to list), short (a change as "+50 HP", not with its before and after).
function M.lines(c, index, opts)
  opts = opts or {}
  local p = M.parts(c, index, opts)
  local out = {}
  local function add(text, kind) out[#out + 1] = { text, kind } end
  if p.cantUse then add(p.cantUse, "bad") end
  add(p.heading, "muted")
  if not p.slot then
    for _, e in ipairs(p.extra) do add(e[1], e[2]) end
    return out
  end
  if opts.verdict and p.verdict then add(p.verdict[1], p.verdict[2]) end
  for _, ch in ipairs(p.changes) do add(opts.short and M.short(ch) or M.change(ch), M.kind(ch)) end
  if p.more > 0 then add(string.format("... and %d more", p.more), "muted") end
  if p.none then add("No change to your stats", "muted") end
  for _, e in ipairs(p.extra) do add(e[1], e[2]) end
  return out
end

-- The slot the other comparison is for (rings, ears, wrists: the second of the pair), or nil.
function M.otherSlotName(c, index)
  local other = c.slots[(index or 1) == 1 and 2 or 1]
  return other and other.slotName or nil
end

return M
