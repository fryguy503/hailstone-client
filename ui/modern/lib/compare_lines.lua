-- Item comparison (Hailstone; docs/item-compare.md): the lines both packs show for eq.CompareItem /
-- eq.CompareSlotItem / eq.CompareBagSlotItem's table, as { text, kind } with kind one of "good", "bad", "mixed",
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

local function signed(key, d)
  local s = key == "weaponRatio" and string.format("%.2f", math.abs(d) / 100) or tostring(math.abs(d))
  if key == "haste" then s = s .. "%" end
  return (d > 0 and "+" or "-") .. s
end

-- "+50 HP (1200 to 1250)"
function M.change(c)
  return string.format("%s %s (%s to %s)", signed(c.key, c.delta), c.name, value(c.key, c.before), value(c.key, c.after))
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

-- The lines for c.slots[index or 1]. opts: verdict (show the verdict line), full (every change, not M.MAX_CHANGES).
function M.lines(c, index, opts)
  opts = opts or {}
  local out = {}
  local function add(text, kind) out[#out + 1] = { text, kind } end
  if not c.usable then add("Can't use: " .. c.whyNot, "bad") end
  local s = c.slots[index or 1]
  if not s then
    add("It does not fit a slot you can wear", "muted")
    return out
  end
  local names = joinNames(s.current)
  if names == "" then add("Compared with your empty " .. s.slotName .. " slot", "muted")
  else add("Compared with your " .. names .. " (" .. s.slotName .. ")", "muted") end
  if opts.verdict and s.verdict ~= "cantuse" then add(M.verdict(s)) end
  local changes = M.ordered(s.changes)
  local shown = opts.full and #changes or math.min(#changes, M.MAX_CHANGES)
  for i = 1, shown do
    local ch = changes[i]
    add(M.change(ch), ch.weight == 0 and "muted" or ch.delta > 0 and "good" or "bad")
  end
  if shown < #changes then add(string.format("... and %d more", #changes - shown), "muted") end
  if #changes == 0 then add("No change to your stats", "muted") end
  local wa, wb = s.weaponAfter, s.weaponBefore
  if wa then
    local text = string.format("Weapon %d dmg / %d delay", wa.damage, wa.delay)
    if wb then text = text .. string.format(" (now %d / %d)", wb.damage, wb.delay) end
    add(text, "text")
  end
  for _, g in ipairs(s.gained) do add("Gains " .. g, "good") end
  for _, l in ipairs(s.lost) do add("Loses " .. l, "bad") end
  for _, n in ipairs(c.notes) do add(n, "muted") end
  return out
end

-- The slot the other comparison is for (rings, ears, wrists: the second of the pair), or nil.
function M.otherSlotName(c, index)
  local other = c.slots[(index or 1) == 1 and 2 or 1]
  return other and other.slotName or nil
end

return M
