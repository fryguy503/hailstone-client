-- Item search, Sort, Stack and Tidy for the bag, inventory and bank windows, and the marks they keep (Hailstone;
-- eq.SortItems, eq.SellJunk, eq.ItemJob, eq.FindItems):
--   a bag's filter (a kind of item: Sort fills the bag with it first, Tidy moves it there; after looting too when
--   "Tidy after looting" is on), locked slots (Sort, Stack and Tidy leave them alone), Sell (items, by id, that the
--   merchant's Sell marked sells; "junk" in the settings and the code). They are kept per server and character in the
--   UI settings ("items.<name>.*", the character's own settings file).
--   Keep (items, by id, the client will not destroy, drop, trade, give, sell or send: eq.SetItemKept, kept by the
--   client in the character's kept_items.txt, game/kept_items.h). An item is marked Keep or Sell, not both.
-- The searches are kept in one global table so a bag window dims what the search of the inventory or bank it is in
-- does not match. Places are eq.FindItems' ("i:<slot>[:<sub>]" carried, "b:<slot>[:<sub>]" banked).
local T = dofile("lib/theme.lua")
local C = T.color

local M = {}
local MANAGED = ui.ManagedDistribution and ui.ManagedDistribution() or false

ITEM_TOOLS = ITEM_TOOLS or { search = { inventory = "", bank = "" }, note = nil, noteAt = 0, mode = nil, prefs = nil }
local S = ITEM_TOOLS
M.state = S

M.CATEGORIES = {
  { "", "Any item" }, { "food", "Food and drink" }, { "tradeskill", "Tradeskill items" },
  { "spells", "Spells and tomes" }, { "weapons", "Weapons and ammo" }, { "armor", "Armor and jewelry" },
  { "potions", "Potions and poisons" }, { "augments", "Augments" },
}
local SHORT = { food = "Food", tradeskill = "Tradeskill", spells = "Spells", weapons = "Weapons", armor = "Armor",
  potions = "Potions", augments = "Augments" }

-- ---------------------------------------------------------------- the character's marks
local function split(text, fn)
  for part in (text or ""):gmatch("[^,]+") do fn(part) end
end

-- (asked for per slot drawn: the character's name is read once a frame)
local function prefs()
  local frame = ui.GetFrameCount()
  if S.prefs and S.prefsFrame == frame then return S.prefs end
  S.prefsFrame = frame
  local who = (eq.Label(1)) or ""
  if S.prefs and S.prefs.who == who then return S.prefs end
  local key = "items." .. who .. "."
  local p = { who = who, key = key, locks = {}, filters = {}, junk = {},
    tidyLoot = ui.GetSetting(key .. "tidyLoot") == "1" }
  split(ui.GetSetting(key .. "locks"), function(v) p.locks[v] = true end)
  split(ui.GetSetting(key .. "junk"), function(v) p.junk[tonumber(v) or 0] = true end)
  split(ui.GetSetting(key .. "filters"), function(v)
    local place, cat = v:match("^(.-)=(.+)$")
    if place then p.filters[place] = cat end
  end)
  S.prefs = p
  return p
end

local function save(name)
  local p = prefs()
  local out = {}
  if name == "locks" then for k in pairs(p.locks) do out[#out + 1] = k end
  elseif name == "junk" then for k in pairs(p.junk) do out[#out + 1] = tostring(k) end
  elseif name == "filters" then for k, v in pairs(p.filters) do out[#out + 1] = k .. "=" .. v end end
  table.sort(out)
  ui.SetSetting(p.key .. name, table.concat(out, ","))
  if name == "junk" then S.junkStamp = (S.junkStamp or 0) + 1 end
end

local places = { b = {}, i = {} } -- [b|i][slot][sub or -1] = the place (made once, not per slot a frame)
function M.Place(where, slot, sub)
  local bank = where == "bank"
  local bySlot = places[bank and "b" or "i"]
  local bySub = bySlot[slot]
  if not bySub then bySub = {}; bySlot[slot] = bySub end
  local k = (sub and sub >= 0) and sub or -1
  local place = bySub[k]
  if not place then
    place = (bank and "b:" or "i:") .. slot .. (sub and sub >= 0 and (":" .. sub) or "")
    bySub[k] = place
  end
  return place
end

function M.Locked(place) return prefs().locks[place] == true end
function M.Junk(item) return item ~= nil and prefs().junk[item.id] == true end

-- The Keep marks, read again only when the client says they changed (eq.ItemsRevision's second count), at most once
-- a frame: asked for per slot drawn.
local kept = { frame = -1, rev = -1, ids = {} }
local function keptIds()
  local frame = ui.GetFrameCount()
  if kept.frame == frame then return kept.ids end
  kept.frame = frame
  local _, rev = eq.ItemsRevision()
  if rev ~= kept.rev then
    kept.rev, kept.ids = rev, {}
  end
  return kept.ids
end
function M.Kept(item)
  if not item then return false end
  local ids = keptIds()
  local v = ids[item.id]
  if v == nil then
    v = eq.ItemKept(item.id) == true
    ids[item.id] = v
  end
  return v
end
-- Keep and Sell: one mark or the other (on = false clears it).
function M.SetKeep(item, on)
  if not item then return end
  if on and M.Junk(item) then
    prefs().junk[item.id] = nil
    save("junk")
  end
  eq.SetItemKept(item.id, item.name or "", on)
  kept.frame = -1
end
function M.SetSell(item, on)
  if not item then return end
  if on and M.Kept(item) then M.SetKeep(item, false) end
  prefs().junk[item.id] = on or nil
  save("junk")
end
function M.ClearSell()
  prefs().junk = {}
  save("junk")
end
-- The marks' change count (a window's cache of filtered rows).
function M.MarksStamp() return S.junkStamp or 0 end
function M.Filter(bagPlace) return prefs().filters[bagPlace] or "" end
function M.SetFilter(bagPlace, cat)
  prefs().filters[bagPlace] = cat ~= "" and cat or nil
  save("filters")
end
function M.TidyAfterLoot() return not MANAGED and prefs().tidyLoot end
function M.JunkIds()
  local ids = {}
  for id in pairs(prefs().junk) do ids[#ids + 1] = id end
  return ids
end

-- How many items marked as junk you carry that a merchant buys (the general slots and their bags). Counted at most
-- four times a second (it reads every carried item; the merchant window asks each frame), and again at once when the
-- junk marks change.
local JUNK_EVERY = 0.25
function M.JunkCarried()
  local p, n = prefs(), 0
  if next(p.junk) == nil then return 0 end
  local now = ui.GetTime()
  if S.junkFor == p and S.junkForStamp == S.junkStamp and S.junkAt and now - S.junkAt < JUNK_EVERY
      and now >= S.junkAt then
    return S.junkCount
  end
  for slot = 23, 32 do
    local it = eq.Item(slot)
    if it and (it.bagSlots or 0) > 0 then
      for i = 0, it.bagSlots - 1 do
        local c = eq.Item(slot, i)
        if c and p.junk[c.id] and c.sellable then n = n + 1 end
      end
    elseif it and p.junk[it.id] and it.sellable then
      n = n + 1
    end
  end
  S.junkCount, S.junkAt, S.junkFor, S.junkForStamp = n, now, p, S.junkStamp
  return n
end

local function options()
  local p = prefs()
  local locked = {}
  for k in pairs(p.locks) do locked[#locked + 1] = k end
  return { locked = locked, filters = p.filters }
end

-- ---------------------------------------------------------------- the marking modes
-- While one is on, a left click on a slot marks instead of picking up: "lock" a bag slot, "junk" (Sell) or "keep" an
-- item.
function M.Mode() return S.mode end

-- From a slot's click: true when the mode took it.
function M.Intercept(place, item, button)
  if not S.mode or (button ~= "left" and button ~= "leftheld") then return false end
  if button == "leftheld" then return true end -- no hotbutton while marking: the release marks
  if S.mode == "lock" then
    if not place:find(":%d+:%d+$") then return true end -- a bag's slot only
    local p = prefs()
    p.locks[place] = (not p.locks[place]) or nil
    save("locks")
  elseif S.mode == "junk" and item then
    M.SetSell(item, not M.Junk(item))
  elseif S.mode == "keep" and item then
    M.SetKeep(item, not M.Kept(item))
  end
  return true
end

-- ---------------------------------------------------------------- slot overlays
function M.Search(where) return S.search[where] or "" end

local lowered = { text = {}, lower = {} } -- [where]: the search and its lower case (lowered once, not per slot)
function M.Matches(item, where)
  local text = M.Search(where)
  if text == "" then return true end
  local needle = lowered.lower[where]
  if lowered.text[where] ~= text then
    needle = text:lower()
    lowered.text[where], lowered.lower[where] = text, needle
  end
  return item ~= nil and (item.name or ""):lower():find(needle, 1, true) ~= nil
end

local function padlock(x, y, s)
  local c = C.gold
  ui.DrawRectFilled(x, y + 4 * s, x + 9 * s, y + 11 * s, c[1], c[2], c[3], 0.95, 1.5 * s)
  ui.DrawRect(x + 2 * s, y, x + 7 * s, y + 6 * s, c[1], c[2], c[3], 0.95, 2.5 * s, 1.5 * s)
end

-- A small shield (the Keep mark) with its top left at x, y, `h` high.
function M.Shield(x, y, h, color, alpha)
  local c = color or C.good
  local a = alpha or 1
  local w = h * 0.8
  local cx = x + w / 2
  ui.DrawRectFilled(x, y, x + w, y + h * 0.55, c[1], c[2], c[3], a, h * 0.12)
  ui.DrawTriangleFilled(x, y + h * 0.5, x + w, y + h * 0.5, cx, y + h, c[1], c[2], c[3], a)
end

-- Over a slot just drawn: its marks (locked, Sell, Keep).
function M.Marks(x, y, size, s, item, place)
  if place and M.Locked(place) then padlock(x + 3 * s, y + 3 * s, s) end
  if M.Junk(item) then
    ui.DrawRectFilled(x, y, x + size, y + size, C.danger[1], C.danger[2], C.danger[3], 0.18, 6 * s)
    T.Text(x + size - 3 * s, y + 2 * s, "sell", C.danger, T.font.tiny, "right")
  elseif M.Kept(item) then
    M.Shield(x + size - 11 * s, y + 3 * s, 10 * s, C.good)
  end
end

-- Over a slot just drawn: the search (`where`'s): dimmed when it does not match, ringed when it does or `found` (a bag
-- holding matches).
function M.SearchMark(x, y, size, s, item, where, found)
  if M.Search(where) == "" then return end
  if found or M.Matches(item, where) then
    ui.DrawRect(x - 1, y - 1, x + size + 1, y + size + 1, C.gold[1], C.gold[2], C.gold[3], 0.9, 6 * s, 2 * s)
  else
    ui.DrawRectFilled(x, y, x + size, y + size, C.window[1], C.window[2], C.window[3], 0.72, 6 * s)
  end
end

-- Both.
function M.Mark(x, y, size, s, item, where, found, place)
  M.Marks(x, y, size, s, item, place)
  M.SearchMark(x, y, size, s, item, where, found)
end

-- ---------------------------------------------------------------- running
local function note(err) if err then S.note, S.noteAt = err, ui.GetTime() end end

function M.Run(where, mode) if not MANAGED then note(eq.SortItems(where, mode, options())) end end
function M.SellJunk() if not MANAGED then note(eq.SellJunk(M.JunkIds())) end end

-- The last refusal (why a job did not start) for a few seconds after it, else nil.
function M.Note()
  if S.note and ui.GetTime() - S.noteAt < 4 then return S.note end
end

-- After the loot window closes: Tidy the inventory, when that is on (from a frame function run every frame).
local lootWasOpen = false
function M.WatchLoot()
  if MANAGED then return end
  local open = eq.WindowShown("LootWnd") == true
  if lootWasOpen and not open and M.TidyAfterLoot() and not eq.ItemJob() then
    eq.SortItems("inventory", "tidy", options())
  end
  lootWasOpen = open
end

-- ---------------------------------------------------------------- the toolbar
-- The width Toolbar takes (with the More button when `more`).
function M.ToolbarWidth(s, more) return MANAGED and 0 or (2 * 54 * s + 6 * s + (more and 30 * s or 0)) end

local JOB_WORD = { sort = "Sorting", stack = "Stacking", tidy = "Tidying", sell = "Selling" }

-- Stack and Sort buttons ending at x1 (24 high from y), with a More menu (the marking modes, Tidy, Tidy after
-- looting) when `more`; while a job runs, its progress and Stop. The last refusal shows to their left (from
-- `left`; else the window shows Note) for a few seconds. `where`: "inventory", "bank" or a bag window.
function M.Toolbar(id, x1, y, s, where, left, more)
  if MANAGED then return end
  local bw, h = 54 * s, 24 * s
  local done, total, mode = eq.ItemJob()
  local x0 = x1 - M.ToolbarWidth(s, more)
  local _, th = T.TextSize("Ag", T.font.small)
  if done then
    local text = string.format("%s %d / %d", JOB_WORD[mode] or "Sorting", done, total)
    T.Text(x1 - bw - 8 * s, y + (h - th) / 2, text, C.muted, T.font.small, "right")
    if T.Button(id .. "stop", x1 - bw, y, x1, y + h, s, "Stop") then eq.StopItemJob() end
    return
  end
  if T.Button(id .. "stack", x0, y, x0 + bw, y + h, s, "Stack",
      { tooltip = "Fill partial stacks of the same item from the later ones" }) then M.Run(where, "stack") end
  if T.Button(id .. "sort", x0 + bw + 6 * s, y, x0 + 2 * bw + 6 * s, y + h, s, "Sort",
      { tooltip = "Sort by item type, then name; a bag with a filter takes its kind first.\nBags, loose items and locked slots stay where they are" }) then
    M.Run(where, "sort")
  end
  if more then
    local mx0 = x1 - 24 * s
    local active = S.mode ~= nil
    if T.Button(id .. "more", mx0, y, x1, y + h, s, "...",
        { primary = active, tooltip = "Mark to sell or keep, lock slots, tidy" }) then ui.OpenPopup(id .. "menu") end
    if ui.BeginPopup(id .. "menu") then
      if ui.MenuItem("Mark to sell (click items)", S.mode == "junk") then S.mode = S.mode ~= "junk" and "junk" or nil end
      if ui.MenuItem("Mark to keep (click items)", S.mode == "keep") then S.mode = S.mode ~= "keep" and "keep" or nil end
      if ui.MenuItem("Lock slots (click bag slots)", S.mode == "lock") then S.mode = S.mode ~= "lock" and "lock" or nil end
      ui.Separator()
      if ui.MenuItem("Tidy: items into their filtered bags") then M.Run(where, "tidy") end
      local p = prefs()
      if ui.MenuItem("Tidy after looting", p.tidyLoot) then
        p.tidyLoot = not p.tidyLoot
        ui.SetSetting(p.key .. "tidyLoot", p.tidyLoot and "1" or "0")
      end
      ui.Separator()
      if ui.MenuItem("Clear sell marks") then M.ClearSell() end
      if ui.MenuItem("Clear locks") then p.locks = {}; save("locks") end
      ui.TextDisabled("A bag's filter: the button at its top left")
      ui.EndPopup()
    end
  end
  local n = M.Note()
  if n and left then
    T.Text(left, y + (h - th) / 2, T.Fit(n, x0 - left - 8 * s, T.font.small), C.danger, T.font.small)
  end
end

-- A bag window's filter: a chip from x (24 high from y) that opens the list of kinds. Returns its width.
function M.FilterChip(id, x, y, s, bagPlace)
  local cat = M.Filter(bagPlace)
  local label = cat == "" and "Filter" or SHORT[cat] or cat
  local w = T.TextSize(label, T.font.small) + 18 * s
  if T.Button(id .. "filter", x, y, x + w, y + 24 * s, s, label,
      { primary = cat ~= "", tooltip = "What this bag is for: Sort fills it with that kind first, Tidy moves it here" }) then
    ui.OpenPopup(id .. "filtermenu")
  end
  if ui.BeginPopup(id .. "filtermenu") then
    for _, c in ipairs(M.CATEGORIES) do
      if ui.MenuItem(c[2], c[1] == cat) then M.SetFilter(bagPlace, c[1]) end
    end
    ui.EndPopup()
  end
  return w
end

-- A banner while a marking mode is on, across x0..x1 at y (20 high); a click on it ends the mode. Returns its height.
function M.ModeBanner(id, x0, x1, y, s)
  if not S.mode then return 0 end
  local text = S.mode == "junk" and "Marking to sell: click items (again to unmark). Click here when done"
    or S.mode == "keep" and "Marking to keep: click items (again to unmark). Click here when done"
    or "Locking slots: click bag slots (again to unlock). Click here when done"
  if T.Hit(id .. "banner", x0, y, x1, y + 20 * s) then S.mode = nil end
  ui.DrawRectFilled(x0, y, x1, y + 20 * s, C.gold[1], C.gold[2], C.gold[3], 0.16, 5 * s)
  local _, th = T.TextSize("Ag", T.font.tiny)
  T.Text((x0 + x1) / 2, y + (20 * s - th) / 2, T.Fit(text, x1 - x0 - 8 * s, T.font.tiny), C.gold, T.font.tiny, "center")
  return 20 * s
end

return M
