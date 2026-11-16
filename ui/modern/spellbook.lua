-- Spell book: replaces the game's SpellBookWnd (shown and closed by the game: the book key, /book, standing up). The
-- whole book at once instead of pages of eight:
--   * search (name or category), sort (level, name, category, mana, cast time, book order), beneficial /
--     detrimental, memorized only, usable at your level; a list or a grid of icons;
--   * categories and subcategories at the left, with counts;
--   * the spell's details at the right (the game's own spell display text: hs.SpellDetails);
--   * spell sets (the game's sets: hotbuttons.ini [SpellLoadouts], /memspellset): load (memorize), edit or delete
--     one; a new set is made empty or from the gems. The editor takes spells dragged from the book onto its twelve
--     slots (or a double-click: the slot picked, else the first empty one), moves them between slots, and saves
--     without memorizing anything (Hailstone's own: hs.SaveSpellSet(name, spells) / hs.EditSpellSet); a spell
--     dropped on a set's card goes straight into that set;
--   * the gems along the bottom: drag a spell onto a gem (or double-click it: the selected gem, else the first
--     empty one) to memorize it, right-click a gem to forget it; the memorize under way shows on its gem.
-- Memorizing and scribing follow the game's rules (sitting, one at a time, the level, the gems you can use); a
-- scroll on the cursor is scribed into the first empty page slot ("Scribe").
local API = hs.UI
local hud = dofile("lib/hud.lua")
local stml = dofile("lib/stml.lua")
local T = hud.T
local C = T.color
local WINDOW = "SpellBookWnd"

local W, H = 980, 640
local SIDE_W, DETAIL_W = 200, 330
local TOOL_H, GEMS_H = 40, 84
local ROW_H, GRID = 34, 52

local SORTS = { "Level", "Name", "Category", "Mana", "Cast time", "Book order" }

-- ---------------------------------------------------------------- state
local book, byId, bySlot = {}, {}, {}
local lastRead = -1
local sel = nil          -- the selected spell's id
local filterCat, filterSub = nil, nil
local search = ""
local targetGem = nil     -- the gem picked in the strip (0-based)
local drag = nil          -- the entry being dragged
local dropTaken = false   -- the book's own gem strip memorized the drag this frame: the spell bar is not offered it
local details = { id = nil }
local tab = "details"
local confirmDelete = nil
-- the set being edited: { n = its number (nil: a new set), name, spells = { id or -1 x12 }, saved = the spells and
-- name it started with, pick = the slot picked (1..12) }
local editor = nil
local slotDrag = nil      -- the editor slot (1..12) being dragged
local flash = nil         -- { text, until }: the sets tab's last word ("Saved Healing")
local SET_SLOTS = 12
local testEdit = nil      -- the last test.book.edit applied
local bookWho = nil       -- the character the state above is for (nil: not in the game)
local bodyFrame = -1      -- the frame the book last drew

local function setting(key, default)
  local v = ui.GetSetting("book." .. key)
  if v == nil then return default end
  if type(default) == "boolean" then return v == "1" end
  if type(default) == "number" then return tonumber(v) or default end
  return v
end
local function setSetting(key, v) ui.SetSetting("book." .. key, type(v) == "boolean" and (v and "1" or "0") or v) end

local function lower(s) return (s or ""):lower() end

-- Tables T.Icon reads and does not keep, filled for each call rather than made: a ring colour, and the tiles' options.
local RING = { 0, 0, 0, 1 }
local function ringOf(c, a)
  RING[1], RING[2], RING[3], RING[4] = T.rgba(c, a)
  return RING
end
local GRID_ICON, ROW_ICON, DETAIL_ICON = { radius = 6, alpha = 1 }, { radius = 4, alpha = 1 }, { radius = 8 }
local SLOT_ICON, SET_ICON = { radius = 7, wellAlpha = 1, alpha = 1 }, { radius = 3 }
local GEM_ICON = { radius = 8, wellAlpha = 1 }
local GEM_IDS = setmetatable({}, { __index = function(t, g) local id = "##gem" .. g; t[g] = id; return id end })

-- The book's settings, read once a frame after the toolbar (which changes them): matches() and sorted() run over
-- every spell, and a setting is a string concat and a lookup.
local cfg = {}
local function readSettings()
  cfg.kind = setting("kind", "all")
  cfg.memorized = setting("memorized", false)
  cfg.usable = setting("usable", false)
  cfg.sort = setting("sort", "Level")
  cfg.desc = setting("desc", false)
  cfg.grid = setting("grid", false)
end

-- Whether a read of the book holds what the last one did: the same spells in the same slots, memorized in the same
-- gems (and read with the same level and names: the rest of an entry follows from its spell).
local function sameBook(a, b)
  if #a ~= #b then return false end
  for i = 1, #a do
    local x, y = a[i], b[i]
    if x.slot ~= y.slot or x.id ~= y.id or x.gem ~= y.gem or x.level ~= y.level or x.name ~= y.name
        or x.categoryName ~= y.categoryName or x.subcategoryName ~= y.subcategoryName then
      return false
    end
  end
  return true
end

local bookStamp = 0 -- counts the changes to the book: the filtered lists below are kept until it (or a filter) changes
local function refresh()
  if ui.GetTime() - lastRead < 0.25 then return end
  lastRead = ui.GetTime()
  local read = API.spells.Book() or {}
  if sameBook(read, book) then return end -- nothing changed: the entries, lists and categories made are kept
  book = read
  bookStamp = bookStamp + 1
  byId, bySlot = {}, {}
  for _, e in ipairs(book) do
    byId[e.id] = e
    bySlot[e.slot] = e
    -- what the search and the sorts compare, and the list's ids, once a read rather than once a frame
    e._lname, e._lcat, e._lsub = lower(e.name), lower(e.categoryName), lower(e.subcategoryName)
    e._catKey = e._lcat .. "\1" .. e._lsub
    e._rowId, e._gridId = "##r" .. e.slot, "##g" .. e.slot
  end
end

local function seconds(ms)
  if not ms or ms <= 0 then return "Instant" end
  local s = ms / 1000
  if s >= 60 then return string.format("%dm %02ds", math.floor(s / 60), math.floor(s % 60)) end
  if s == math.floor(s) then return string.format("%ds", s) end
  return string.format("%.1fs", s)
end

-- q: the search in lower case ("" none); cfg read this frame
local function matches(e, myLevel, q)
  if filterCat and e.categoryName ~= filterCat then return false end
  if filterSub and e.subcategoryName ~= filterSub then return false end
  local kind = cfg.kind
  if kind == "beneficial" and not e.beneficial then return false end
  if kind == "detrimental" and e.beneficial then return false end
  if cfg.memorized and not e.gem then return false end
  if cfg.usable and e.level > myLevel then return false end
  if q ~= "" then
    if not (e._lname:find(q, 1, true) or e._lcat:find(q, 1, true) or e._lsub:find(q, 1, true)) then
      return false
    end
  end
  return true
end

local SORT_KEY = {
  Level = function(e) return e.level end,
  Name = function(e) return e._lname end,
  Category = function(e) return e._catKey end,
  Mana = function(e) return e.mana end,
  ["Cast time"] = function(e) return e.castTime end,
  ["Book order"] = function(e) return e.slot end,
}
local sortKey, sortDesc = SORT_KEY.Level, false
local function byKey(a, b)
  local ka, kb = sortKey(a), sortKey(b)
  if ka ~= kb then
    if sortDesc then return ka > kb end
    return ka < kb
  end
  if a.level ~= b.level then return a.level < b.level end
  return a._lname < b._lname
end

local function sorted(list)
  sortKey, sortDesc = SORT_KEY[cfg.sort] or SORT_KEY.Level, cfg.desc
  table.sort(list, byKey)
  return list
end

-- The spells the list shows, filtered and sorted: kept while the book's read, the search, the filters and the sort
-- stay the same.
local listCache = { list = {} }
local function shownSpells(myLevel)
  local c = listCache
  if c.stamp ~= bookStamp or c.search ~= search or c.cat ~= filterCat or c.sub ~= filterSub or c.kind ~= cfg.kind
      or c.memorized ~= cfg.memorized or c.usable ~= cfg.usable or c.level ~= myLevel or c.sort ~= cfg.sort
      or c.desc ~= cfg.desc then
    c.stamp, c.search, c.cat, c.sub, c.kind = bookStamp, search, filterCat, filterSub, cfg.kind
    c.memorized, c.usable, c.level, c.sort, c.desc = cfg.memorized, cfg.usable, myLevel, cfg.sort, cfg.desc
    local q = lower(search)
    local list = {}
    for _, e in ipairs(book) do
      if matches(e, myLevel, q) then list[#list + 1] = e end
    end
    c.list = sorted(list)
  end
  return c.list
end

local function spellIcon(id)
  local e = byId[id]
  if not e then return nil end
  local tex, u0, v0, u1, v1 = API.spells.Icon(e.icon)
  return tex, u0, v0, u1, v1
end

-- Whether a gem that holds a spell takes another (Hailstone's replace_gem_spell, UiHost::gem_takes_book_spell); the
-- game memorizes only into an empty gem.
local function replaces()
  return API.settings.Option("replace_gem_spell") ~= 0
end

-- The gem a double-click or the Memorize button uses: the one picked, while it is empty or replace_gem_spell lets a
-- full one take the spell; else the first empty usable one.
local function gemFor(st)
  if targetGem and targetGem < st.usableGems and ((st.gems[targetGem + 1] or -1) == -1 or replaces()) then
    return targetGem
  end
  for g = 0, st.usableGems - 1 do
    if (st.gems[g + 1] or -1) == -1 then return g end
  end
  return nil
end

local function memorize(e, gem, st)
  if not e or not gem then return end
  if e.gem then return end
  API.spells.BookMemorize(e.slot, gem)
end

-- ---------------------------------------------------------------- spell sets
local function copySpells(t)
  local c = {}
  for k = 1, SET_SLOTS do c[k] = (t and t[k]) or -1 end
  return c
end

-- `id` into slot k of `spells`; a set memorizes a spell once (MemorizeSet keeps the later gem), so it leaves any
-- other slot it was in.
local function putSpell(spells, k, id)
  for j = 1, SET_SLOTS do
    if j ~= k and spells[j] == id then spells[j] = -1 end
  end
  spells[k] = id
end

-- The slot a spell added to `spells` goes to: `pick` when given, else the first empty one of the gems you can use,
-- else the first empty one; nil when all twelve are full.
local function slotFor(spells, pick, usable)
  if pick then return pick end
  for k = 1, math.min(usable or SET_SLOTS, SET_SLOTS) do
    if spells[k] == -1 then return k end
  end
  for k = 1, SET_SLOTS do
    if spells[k] == -1 then return k end
  end
  return nil
end

local function editorAdd(id, st)
  if not editor then return end
  local k = slotFor(editor.spells, editor.pick, st and st.usableGems)
  if k then
    putSpell(editor.spells, k, id)
    editor.pick = nil
  end
end

local function openEditor(n, set, fromGems, st)
  local spells = set and copySpells(set.spells) or copySpells(fromGems and st and st.gems or nil)
  editor = { n = n, name = set and set.name or "", spells = spells, pick = nil }
  editor.saved = { name = editor.name, spells = copySpells(spells) }
  editor.focusName = n == nil
end

local function editorDirty()
  if not editor then return false end
  if editor.name ~= editor.saved.name then return true end
  for k = 1, SET_SLOTS do
    if editor.spells[k] ~= editor.saved.spells[k] then return true end
  end
  return false
end

local function trim(t) return (t or ""):match("^%s*(.-)%s*$") end

local function firstEmptyBookSlot()
  for slot = 0, 719 do
    if not bySlot[slot] then return slot end
  end
  return nil
end

-- ---------------------------------------------------------------- pieces
local function chip(id, x, y, label, on, s)
  local tw = T.TextSize(label, T.font.small)
  local w = tw + 18 * s
  local clicked = T.Hit(id, x, y, x + w, y + 24 * s)
  local hov = ui.IsItemHovered()
  local a = C.accent
  if on then
    ui.DrawRectFilled(x, y, x + w, y + 24 * s, a[1], a[2], a[3], 0.22, 12 * s)
    ui.DrawRect(x, y, x + w, y + 24 * s, a[1], a[2], a[3], 0.7, 12 * s, 1)
  else
    ui.DrawRectFilled(x, y, x + w, y + 24 * s, C.ink[1], C.ink[2], C.ink[3], hov and 0.08 or 0.04, 12 * s)
  end
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(x + w / 2, y + (24 * s - th) / 2, label, (on or hov) and C.text or C.muted, T.font.small, "center")
  return clicked, w
end

local KINDS = { { "all", "All", "##kindall" }, { "beneficial", "Beneficial", "##kindbeneficial" },
  { "detrimental", "Detrimental", "##kinddetrimental" } }
local DESC_TIP, ASC_TIP = { tooltip = "Descending" }, { tooltip = "Ascending" }

local function toolbar(x, y, w, s, st)
  local wx, wy = ui.GetWindowPos()
  -- search
  ui.SetCursorPos(x - wx, y - wy + 2 * s)
  ui.SetNextItemWidth(240 * s)
  ui.PushStyleVar(ui.StyleVar.FramePadding, 8 * s, 5 * s)
  ui.PushStyleVar(ui.StyleVar.FrameRounding, 12 * s)
  local text = ui.InputText("##search", search, 64)
  local editing = ui.IsItemActive()
  ui.PopStyleVar(2)
  search = text
  if search == "" and not editing then
    local _, th = T.TextSize("Ag", T.font.small)
    T.Text(x + 10 * s, y + 2 * s + (26 * s - th) / 2, "Search spells or categories", C.faint, T.font.small)
  end
  -- sort
  local cx = x + 252 * s
  T.Text(cx, y + 7 * s, "Sort", C.muted, T.font.small)
  ui.SetCursorPos(cx + 36 * s - wx, y - wy + 2 * s)
  ui.SetNextItemWidth(120 * s)
  local mode = setting("sort", "Level")
  if ui.BeginCombo("##sort", mode) then
    for _, m in ipairs(SORTS) do
      if ui.Selectable(m, m == mode) then setSetting("sort", m) end
    end
    ui.EndCombo()
  end
  local desc = setting("desc", false)
  if T.Button("##desc", cx + 162 * s, y + 2 * s, cx + 190 * s, y + 28 * s, s, desc and "v" or "^",
      desc and DESC_TIP or ASC_TIP) then
    setSetting("desc", not desc)
  end
  -- filters
  local fx = cx + 204 * s
  local kind = setting("kind", "all")
  for _, k in ipairs(KINDS) do
    local clicked, cw = chip(k[3], fx, y + 3 * s, k[2], kind == k[1], s)
    if clicked then setSetting("kind", k[1]) end
    fx = fx + cw + 4 * s
  end
  fx = fx + 8 * s
  local mem = setting("memorized", false)
  local clicked, cw = chip("##mem", fx, y + 3 * s, "Memorized", mem, s)
  if clicked then setSetting("memorized", not mem) end
  fx = fx + cw + 4 * s
  local usable = setting("usable", false)
  clicked, cw = chip("##usable", fx, y + 3 * s, "My level", usable, s)
  if clicked then setSetting("usable", not usable) end
  if ui.IsItemHovered() then ui.SetTooltip("Only spells you can memorize at your level") end
  -- view
  local grid = setting("grid", false)
  local vx = x + w - 104 * s
  local c1 = chip("##list", vx, y + 3 * s, "List", not grid, s)
  local c2 = chip("##grid", vx + 50 * s, y + 3 * s, "Grid", grid, s)
  if c1 then setSetting("grid", false) end
  if c2 then setSetting("grid", true) end
end

-- category -> { count, subs = { name -> count }, subOrder, id, subIds }, the names in order, and the total: counted
-- with every filter but the category's own, kept while the book's read, the search and the filters stay the same
local catCache = {}
local function categories(myLevel)
  local c = catCache
  if c.stamp == bookStamp and c.search == search and c.kind == cfg.kind and c.memorized == cfg.memorized
      and c.usable == cfg.usable and c.level == myLevel then
    return c.cats, c.order, c.total
  end
  c.stamp, c.search, c.kind, c.memorized, c.usable, c.level = bookStamp, search, cfg.kind, cfg.memorized, cfg.usable,
      myLevel
  local cats, order = {}, {}
  local total = 0
  local q = lower(search)
  local savedCat, savedSub = filterCat, filterSub
  filterCat, filterSub = nil, nil -- count with every filter but the category's own
  for _, e in ipairs(book) do
    if matches(e, myLevel, q) then
      total = total + 1
      local name = e.categoryName ~= "" and e.categoryName or "Other"
      local cat = cats[name]
      if not cat then
        cat = { count = 0, subs = {}, subOrder = {}, id = "##c" .. name .. "0", subIds = {} }
        cats[name] = cat
        order[#order + 1] = name
      end
      cat.count = cat.count + 1
      local sub = e.subcategoryName ~= "" and e.subcategoryName or nil
      if sub then
        if not cat.subs[sub] then
          cat.subOrder[#cat.subOrder + 1] = sub
          cat.subIds[sub] = "##c" .. sub .. "14"
        end
        cat.subs[sub] = (cat.subs[sub] or 0) + 1
      end
    end
  end
  filterCat, filterSub = savedCat, savedSub
  table.sort(order)
  for _, name in ipairs(order) do table.sort(cats[name].subOrder) end
  c.cats, c.order, c.total = cats, order, total
  return cats, order, total
end

local function catRow(id, label, count, on, indent, rw, s)
  local cx, cy = ui.GetCursorScreenPos()
  local clicked = ui.InvisibleButton(id, rw, 24 * s)
  local hov = ui.IsItemHovered()
  if on then
    ui.DrawRectFilled(cx, cy, cx + rw, cy + 24 * s, C.accent[1], C.accent[2], C.accent[3], 0.16, 6 * s)
    ui.DrawRectFilled(cx, cy + 4 * s, cx + 3 * s, cy + 20 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s)
  elseif hov then
    ui.DrawRectFilled(cx, cy, cx + rw, cy + 24 * s, C.ink[1], C.ink[2], C.ink[3], 0.05, 6 * s)
  end
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(cx + (10 + indent) * s, cy + (24 * s - th) / 2, T.Fit(label, rw - (44 + indent) * s, T.font.small),
    on and C.text or (indent > 0 and C.muted or C.text), T.font.small)
  T.Text(cx + rw - 8 * s, cy + (24 * s - th) / 2, tostring(count), C.faint, T.font.small, "right")
  return clicked
end

local function sidebar(x, y, w, h, s, myLevel)
  local cats, order, total = categories(myLevel)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy)
  ui.BeginChild("##cats", w, h, false)
  local rw = ui.GetContentRegionAvail()
  if catRow("##cAll spells0", "All spells", total, filterCat == nil, 0, rw, s) then filterCat, filterSub = nil, nil end
  ui.Dummy(1, 4 * s)
  for _, name in ipairs(order) do
    local c = cats[name]
    local catKey = name == "Other" and "" or name
    if catRow(c.id, name, c.count, filterCat == catKey and filterSub == nil, 0, rw, s) then
      if filterCat == catKey and filterSub == nil then filterCat = nil else filterCat, filterSub = catKey, nil end
    end
    if filterCat == catKey then
      for _, sub in ipairs(c.subOrder) do
        if catRow(c.subIds[sub], sub, c.subs[sub], filterSub == sub, 14, rw, s) then
          filterSub = (filterSub == sub) and nil or sub
        end
      end
    end
  end
  ui.EndChild()
end

local function rowTooltip(e)
  ui.BeginTooltip()
  ui.TextColored(e.beneficial and C.good[1] or C.bad[1], e.beneficial and C.good[2] or C.bad[2],
    e.beneficial and C.good[3] or C.bad[3], 1, e.name)
  ui.TextDisabled(string.format("Level %d  ·  %s", e.level, e.categoryName ~= "" and e.categoryName or "Other"))
  if editor and tab == "sets" then
    ui.TextDisabled("Double-click or drag onto a slot to add it to the set")
  else
    ui.TextDisabled("Double-click or drag onto a gem to memorize")
  end
  ui.EndTooltip()
end

local function entryInput(e, st, hovered)
  if ui.IsItemClicked(ui.Mouse.Left) then sel = e.id end
  if hovered and ui.IsMouseDoubleClicked(ui.Mouse.Left) then
    if editor and tab == "sets" then editorAdd(e.id, st) else memorize(e, gemFor(st), st) end
  end
  if ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left) and not drag then drag = e end
  if ui.IsItemClicked(ui.Mouse.Right) then
    sel = e.id
    -- a spell picked up to move (the stock book's right click, hs.SpellBookSwap): this one swaps places with it
    -- (itself puts it back)
    if st.swapSlot then API.spells.BookSwap(e.slot) else ui.OpenPopup("##spellmenu") end
    T.TakeRightClick()
  end
  if hovered and not drag and not ui.IsMouseDown(ui.Mouse.Left) then rowTooltip(e) end
end

-- Lays out the entries `per` to a line (each line's height lineH) and calls draw(i, e) only for the lines near the
-- view [top, bottom] (screen y). The lines out of view were invisible buttons alone, drawn and read by nothing: the
-- cursor jumps over them instead, to where they would have put it (lines 1 and 2 are laid out to measure the step,
-- which ImGui keeps in whole pixels), and the last line's place is reserved with a Dummy of the same size, so the
-- scrolled height is the same.
local function clippedLines(list, per, lineW, lineH, top, bottom, draw)
  local n = #list
  local lines = math.ceil(n / per)
  local function line(l)
    for i = (l - 1) * per + 1, math.min(n, l * per) do draw(i, list[i]) end
  end
  if lines <= 3 then
    for l = 1, lines do line(l) end
    return
  end
  line(1)
  local x2, y2 = ui.GetCursorScreenPos()
  line(2)
  local _, y3 = ui.GetCursorScreenPos()
  local step = y3 - y2
  if step <= 0 then
    for l = 3, lines do line(l) end
    return
  end
  local first = math.max(3, math.floor((top - lineH - y2) / step) + 2)
  local last = math.min(lines, math.ceil((bottom - y2) / step) + 2)
  if first <= last then
    ui.SetCursorScreenPos(x2, y2 + (first - 2) * step)
    for l = first, last do line(l) end
  end
  if last < lines then
    ui.SetCursorScreenPos(x2, y2 + (lines - 2) * step)
    ui.Dummy(lineW, lineH)
  end
end

-- (listView's per-frame state for its entry drawers, which are file-level functions)
local lv = {}

local function gridEntry(i, e)
  local s, st = lv.s, lv.st
  if (i - 1) % lv.per ~= 0 then ui.SameLine(0, 0) end
  local px, py = ui.GetCursorScreenPos()
  ui.InvisibleButton(e._gridId, GRID * s, GRID * s)
  if py + GRID * s >= lv.top and py <= lv.bottom then
    local hovered = ui.IsItemHovered()
    local tex, u0, v0, u1, v1 = API.spells.Icon(e.icon)
    local pad = 5 * s
    GRID_ICON.alpha = e.level > lv.myLevel and 0.35 or 1
    T.Icon(px + pad, py + pad, GRID * s - 2 * pad, s, tex, u0, v0, u1, v1,
      sel == e.id and C.accent or ringOf(e.beneficial and C.good or C.bad, 0.35), GRID_ICON)
    if e.gem then
      ui.DrawCircleFilled(px + GRID * s - 10 * s, py + 10 * s, 7 * s, C.accent[1], C.accent[2], C.accent[3], 1)
      T.Text(px + GRID * s - 10 * s, py + 3 * s, tostring(e.gem + 1), C.onAccent, T.font.tiny, "center")
    end
    if hovered then T.HoverFx(px + pad, py + pad, px + GRID * s - pad, py + GRID * s - pad, s, 6) end
    if st.swapSlot == e.slot then
      ui.DrawRect(px + pad, py + pad, px + GRID * s - pad, py + GRID * s - pad, C.gold[1], C.gold[2], C.gold[3], 0.9,
        6 * s, 2 * s)
    end
    entryInput(e, st, hovered)
  end
end

local function listEntry(_, e)
  local s, st, cw, th = lv.s, lv.st, lv.cw, lv.th
  local px, py = ui.GetCursorScreenPos()
  ui.InvisibleButton(e._rowId, cw, ROW_H * s)
  if py + ROW_H * s >= lv.top and py <= lv.bottom then
    local hovered = ui.IsItemHovered()
    local on = sel == e.id
    if on then
      ui.DrawRectFilled(px, py, px + cw, py + ROW_H * s, C.accent[1], C.accent[2], C.accent[3], 0.16, 6 * s)
    elseif hovered then
      ui.DrawRectFilled(px, py, px + cw, py + ROW_H * s, C.ink[1], C.ink[2], C.ink[3], 0.05, 6 * s)
    end
    if st.swapSlot == e.slot then
      ui.DrawRect(px, py, px + cw, py + ROW_H * s, C.gold[1], C.gold[2], C.gold[3], 0.9, 6 * s, 2 * s)
    end
    local dim = e.level > lv.myLevel
    local tex, u0, v0, u1, v1 = API.spells.Icon(e.icon)
    local isz = (ROW_H - 6) * s
    ROW_ICON.alpha = dim and 0.4 or 1
    T.Icon(px + 4 * s, py + 3 * s, isz, s, tex, u0, v0, u1, v1, ringOf(e.beneficial and C.good or C.bad, 0.5), ROW_ICON)
    local nx = px + isz + 12 * s
    local ty = py + (ROW_H * s - th) / 2
    -- columns: name, level, mana, cast time, category
    local colLevel = px + cw * 0.44
    local colMana = px + cw * 0.53
    local colCast = px + cw * 0.62
    local colCat = px + cw * 0.72
    T.Text(nx, ty, T.Fit(e.name, colLevel - nx - 8 * s, T.font.small), dim and C.faint or C.text, T.font.small)
    T.Text(colLevel, ty, tostring(e.level), dim and C.danger or C.muted, T.font.small)
    T.Text(colMana, ty, e.mana > 0 and tostring(e.mana) or (e.endurance > 0 and (e.endurance .. " end") or "-"),
      C.muted, T.font.small)
    T.Text(colCast, ty, seconds(e.castTime), C.muted, T.font.small)
    T.Text(colCat, ty, T.Fit(e.subcategoryName ~= "" and e.subcategoryName or e.categoryName, px + cw - colCat - 30 * s,
      T.font.small), C.faint, T.font.small)
    if e.gem then
      local gx = px + cw - 14 * s
      ui.DrawCircleFilled(gx, py + ROW_H * s / 2, 9 * s, C.accent[1], C.accent[2], C.accent[3], 0.9)
      T.Text(gx, ty, tostring(e.gem + 1), C.onAccent, T.font.small, "center")
    end
    entryInput(e, st, hovered)
  end
end

local function listView(list, x, y, w, h, s, st, myLevel)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy)
  ui.BeginChild("##spells", w, h, false)
  local cw = ui.GetContentRegionAvail()
  local _, cy0 = ui.GetWindowPos()
  local top, bottom = cy0, cy0 + h
  if #list == 0 then
    T.Text(select(1, ui.GetCursorScreenPos()) + 10 * s, select(2, ui.GetCursorScreenPos()) + 10 * s,
      #book == 0 and "Your spell book is empty." or "No spells match.", C.muted, T.font.small)
  end
  lv.s, lv.st, lv.myLevel, lv.top, lv.bottom, lv.cw = s, st, myLevel, top, bottom, cw
  if cfg.grid then
    local per = math.max(1, math.floor(cw / (GRID * s)))
    lv.per = per
    clippedLines(list, per, GRID * s, GRID * s, top, bottom, gridEntry)
  else
    local _, th = T.TextSize("Ag", T.font.small)
    lv.th = th
    clippedLines(list, 1, cw, ROW_H * s, top, bottom, listEntry)
  end
  -- the spell's menu
  if ui.BeginPopup("##spellmenu") then
    local e = byId[sel]
    if e then
      ui.TextDisabled(e.name)
      ui.Separator()
      if e.gem then
        ui.TextDisabled("Memorized in gem " .. (e.gem + 1))
      elseif ui.BeginMenu("Memorize into") then
        for g = 0, st.usableGems - 1 do
          local cur = st.gems[g + 1]
          local label = string.format("Gem %d", g + 1) .. (cur ~= -1 and byId[cur] and ("  (" .. byId[cur].name .. ")") or "  (empty)")
          if ui.MenuItem(label .. "##gem" .. g) then memorize(e, g, st) end
        end
        ui.EndMenu()
      end
      if editor and ui.MenuItem("Add to the set being edited") then editorAdd(e.id, st) end
      if ui.MenuItem("Pick up (click a gem to memorize)") then API.spells.BookClick(e.slot) end
      if ui.MenuItem("Move in the book (right-click another spell)") then API.spells.BookSwap(e.slot) end
      if ui.MenuItem("Spell display window") then API.spells.Show(e.id) end
    end
    ui.EndPopup()
  end
  ui.EndChild()
  -- the header of the list's columns
end

local HEADER_COLS = { { 0, "Spell" }, { 0.44, "Level" }, { 0.53, "Mana" }, { 0.62, "Cast" }, { 0.72, "Category" } }
local function columnHeader(x, y, w, s)
  if cfg.grid then return end
  for _, c in ipairs(HEADER_COLS) do
    local cx = x + (c[1] == 0 and (ROW_H + 6) * s or w * c[1])
    T.Text(cx, y, c[2], C.faint, T.font.tiny)
  end
end

local TILE_LABELS, TILE_VALUES = { "Mana", "Cast", "Recast", "Range" }, {}
local function detailsTab(x, y, w, h, s, st)
  local e = byId[sel]
  if not e then
    T.Text(x, y + 6 * s, "Select a spell to see its details.", C.muted, T.font.small)
    return
  end
  local tex, u0, v0, u1, v1 = API.spells.Icon(e.icon)
  T.Icon(x, y, 52 * s, s, tex, u0, v0, u1, v1, e.beneficial and C.good or C.bad, DETAIL_ICON)
  local _, th = T.TextSize("Ag", T.font.large)
  T.Text(x + 62 * s, y + 2 * s, T.Fit(e.name, w - 62 * s, T.font.large), C.text, T.font.large)
  local sub = "Level " .. e.level .. (e.skillName and e.skillName ~= "" and ("  \xC2\xB7  " .. e.skillName) or "")
  T.Text(x + 62 * s, y + th + 6 * s, sub, C.muted, T.font.small)
  local cat = e.categoryName ~= "" and (e.categoryName .. (e.subcategoryName ~= "" and (" / " .. e.subcategoryName) or "")) or ""
  T.Text(x + 62 * s, y + th + 22 * s, T.Fit(cat, w - 62 * s, T.font.small), C.faint, T.font.small)
  -- the numbers in tiles
  local ty = y + 64 * s
  local tiles = TILE_VALUES
  tiles[1] = e.mana > 0 and tostring(e.mana) or "-"
  tiles[2] = seconds(e.castTime)
  tiles[3] = e.recastTime > 0 and seconds(e.recastTime) or "-"
  tiles[4] = e.range > 0 and tostring(math.floor(e.range)) or "-"
  local tw = (w - 3 * 6 * s) / 4
  for k, label in ipairs(TILE_LABELS) do
    local tx = x + (k - 1) * (tw + 6 * s)
    ui.DrawRectFilled(tx, ty, tx + tw, ty + 40 * s, C.ink[1], C.ink[2], C.ink[3], 0.04, 6 * s)
    T.Text(tx + tw / 2, ty + 4 * s, label, C.faint, T.font.tiny, "center")
    T.Text(tx + tw / 2, ty + 18 * s, tiles[k], C.text, T.font.small, "center")
  end
  -- buttons
  local by = ty + 48 * s
  local bw = (w - 6 * s) / 2
  local gem = gemFor(st)
  local canMem = not e.gem and gem ~= nil
  local label = "No free gem"
  if e.gem then
    label = "In gem " .. (e.gem + 1)
  elseif gem and (st.gems[gem + 1] or -1) ~= -1 then
    label = "Replace gem " .. (gem + 1)
  elseif gem then
    label = "Memorize (gem " .. (gem + 1) .. ")"
  end
  if T.Button("##memorize", x, by, x + bw, by + 26 * s, s, label, { primary = canMem, enabled = canMem }) then memorize(e, gem, st) end
  if T.Button("##show", x + bw + 6 * s, by, x + w, by + 26 * s, s, "Spell window") then API.spells.Show(e.id) end
  -- the game's spell display text
  local dy = by + 36 * s
  if details.id ~= e.id then
    local _, raw = API.spells.Details(e.id)
    details = { id = e.id, lines = stml.Lines(raw or "", C.text) }
  end
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, dy - wy)
  ui.BeginChild("##details", w, y + h - dy, false)
  ui.PushFont(T.font.small)
  local lw = ui.GetContentRegionAvail()
  for _, l in ipairs(details.lines or {}) do stml.DrawLine(l, lw) end
  ui.PopFont()
  ui.EndChild()
end

local function spellName(id)
  local e = byId[id]
  return e and e.name or ("spell " .. id .. " (not in your book)")
end

-- The set editor in place of the list: the name, the twelve slots (spells dragged from the book land there, a slot
-- dragged onto another swaps the two, one dragged off or right-clicked is emptied), the slots' names, and Save.
local function setEditor(x, y, w, h, s, st, sets)
  local ed = editor
  local wx, wy = ui.GetWindowPos()
  local usable = st.usableGems or 8
  local wasDragging = slotDrag ~= nil or drag ~= nil
  T.Text(x, y, ed.n and "Edit spell set" or "New spell set", C.text, T.font.small)
  if editorDirty() then T.Text(x + w, y + 2 * s, "Not saved", C.gold, T.font.tiny, "right") end
  -- the name
  local ny = y + 22 * s
  ui.SetCursorPos(x - wx, ny - wy)
  ui.SetNextItemWidth(w)
  if ed.focusName then
    ui.SetKeyboardFocusHere()
    ed.focusName = false
  end
  ui.PushStyleVar(ui.StyleVar.FramePadding, 8 * s, 5 * s)
  local text, enter = ui.InputTextWithHint("##setname", "Set name", ed.name, 24, ui.InputTextFlags.EnterReturnsTrue)
  ui.PopStyleVar()
  ed.name = text
  local name = trim(ed.name)
  local clash, free = nil, false
  for n, set in ipairs(sets) do
    if not set.inUse then
      free = true
    elseif n ~= ed.n and lower(set.name) == lower(name) then
      clash = set.name
    end
  end
  -- the slots: two rows of six, gem 1 first
  local gy = ny + 38 * s
  local gap = 6 * s
  local size = (w - 5 * gap) / 6
  local released = ui.IsMouseReleased(ui.Mouse.Left)
  local dropped = false
  local hoverSlot = nil
  for k = 1, SET_SLOTS do
    local sx, sy = x + ((k - 1) % 6) * (size + gap), gy + ((k - 1) // 6) * (size + gap)
    local id = ed.spells[k]
    local canUse = k <= usable
    local tex, u0, v0, u1, v1
    if id ~= -1 then tex, u0, v0, u1, v1 = spellIcon(id) end
    SLOT_ICON.wellAlpha, SLOT_ICON.alpha = canUse and 1 or 0.4, slotDrag == k and 0.3 or (canUse and 1 or 0.55)
    T.Icon(sx, sy, size, s, tex, u0, v0, u1, v1, ed.pick == k and C.accent or nil, SLOT_ICON)
    if id == -1 then
      T.Text(sx + size / 2, sy + size / 2 - 7 * s, tostring(k), C.faint, T.font.small, "center")
    else
      if not byId[id] then T.Text(sx + size / 2, sy + size / 2 - 7 * s, "?", C.danger, T.font.small, "center") end
      T.Text(sx + 4 * s, sy + 2 * s, tostring(k), C.text, T.font.tiny)
    end
    local clicked = T.Hit("##slot" .. k, sx, sy, sx + size, sy + size)
    local over = ui.IsMouseHoveringRect(sx, sy, sx + size, sy + size)
    if id ~= -1 and not wasDragging and ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left) then slotDrag = k end
    if (drag or slotDrag) and over then
      hoverSlot = k
      ui.DrawRect(sx - 2 * s, sy - 2 * s, sx + size + 2 * s, sy + size + 2 * s, C.accent[1], C.accent[2], C.accent[3], 1,
        8 * s, 2 * s)
      if released then
        if drag then
          putSpell(ed.spells, k, drag.id)
        elseif slotDrag ~= k then
          ed.spells[k], ed.spells[slotDrag] = ed.spells[slotDrag], ed.spells[k]
        end
        dropped = true
      end
    elseif ui.IsItemHovered() and not wasDragging then
      T.HoverFx(sx, sy, sx + size, sy + size, s, 7)
      local tip
      if id ~= -1 then
        tip = string.format("Gem %d: %s\nDrag to another slot to move it; right-click to take it out", k, spellName(id))
      else
        tip = string.format("Gem %d (empty)\nDrop a spell from the book here, or click to pick this slot", k)
      end
      if not canUse then tip = tip .. string.format("\nYou cannot use gem %d yet: loading the set skips it", k) end
      ui.SetTooltip(tip)
    end
    if clicked and not wasDragging then ed.pick = (ed.pick == k) and nil or k end
    if ui.IsItemClicked(ui.Mouse.Right) then
      ed.spells[k] = -1
      T.TakeRightClick()
    end
  end
  -- a slot dragged off the slots leaves the set
  if slotDrag and released and not dropped then ed.spells[slotDrag] = -1 end
  -- the slots' spells by name, two columns of six (a click picks the slot)
  local ly = gy + 2 * size + gap + 12 * s
  local lineH, colW = 18 * s, (w - 10 * s) / 2
  for k = 1, SET_SLOTS do
    local lx, lyk = x + ((k - 1) // 6) * (colW + 10 * s), ly + ((k - 1) % 6) * lineH
    local id = ed.spells[k]
    local hot = ed.pick == k or hoverSlot == k
    if T.Hit("##slotname" .. k, lx, lyk, lx + colW, lyk + lineH) then ed.pick = (ed.pick == k) and nil or k end
    if hot or ui.IsItemHovered() then
      ui.DrawRectFilled(lx, lyk, lx + colW, lyk + lineH, C.accent[1], C.accent[2], C.accent[3], hot and 0.16 or 0.08, 4 * s)
    end
    T.Text(lx + 4 * s, lyk + 2 * s, tostring(k), C.faint, T.font.small)
    local label, col = "-", C.faint
    if id ~= -1 then label, col = spellName(id), byId[id] and (k <= usable and C.text or C.muted) or C.danger end
    T.Text(lx + 22 * s, lyk + 2 * s, T.Fit(label, colW - 26 * s, T.font.small), col, T.font.small)
  end
  -- the buttons
  local by = y + h - 50 * s
  local bw = (w - 3 * 6 * s) / 4
  local function bx(i) return x + (i - 1) * (bw + 6 * s) end
  if T.Button("##edgems", bx(1), by, bx(1) + bw, by + 24 * s, s, "From gems",
      { tooltip = "Put the spells on your gems into the set" }) then
    ed.spells = copySpells(st.gems)
  end
  if T.Button("##edclear", bx(2), by, bx(2) + bw, by + 24 * s, s, "Clear", { tooltip = "Empty every slot" }) then
    ed.spells, ed.pick = copySpells(nil), nil
  end
  if T.Button("##edcancel", bx(3), by, bx(3) + bw, by + 24 * s, s, "Cancel") then
    editor, slotDrag = nil, nil
    return
  end
  local canSave = name ~= "" and (ed.n ~= nil or clash ~= nil or free)
  if T.Button("##edsave", bx(4), by, bx(4) + bw, by + 24 * s, s, clash and "Replace" or "Save",
      { primary = canSave, enabled = canSave }) or (enter and canSave) then
    local at
    if ed.n then at = API.spells.EditSet(ed.n, name, ed.spells) else at = API.spells.SaveSet(name, ed.spells) end
    if at then
      flash = { "Saved " .. name, ui.GetTime() + 3 }
      editor, slotDrag = nil, nil
      return
    end
  end
  local note, col = "Nothing is memorized until you load the set.", C.faint
  if name == "" then
    note = "Give the set a name to save it."
  elseif clash then
    note, col = "Saving replaces your set '" .. clash .. "'.", C.gold
  elseif not ed.n and not free then
    note, col = "All 10 sets are in use: delete one first.", C.danger
  elseif ed.pick then
    note, col = "Double-click a spell in the book to put it in gem " .. ed.pick .. ".", C.accent
  end
  T.Text(x, by + 31 * s, T.Fit(note, w, T.font.tiny), col, T.font.tiny)
  -- the slot being dragged follows the mouse
  if slotDrag then
    local mx, my = ui.GetMousePos()
    local tex, u0, v0, u1, v1 = spellIcon(ed.spells[slotDrag])
    ui.Foreground(true)
    if tex then ui.DrawImage(tex, mx - 18 * s, my - 18 * s, mx + 18 * s, my + 18 * s, u0, v0, u1, v1, 1, 1, 1, 0.85) end
    ui.Foreground(false)
    if not ui.IsMouseDown(ui.Mouse.Left) then slotDrag = nil end
  end
end

-- The sets as cards: name, the twelve spells, Load / Edit / Delete. A spell dragged from the book onto one of a card's
-- spells takes that gem, onto the rest of the card the first empty one.
local function setList(x, y, w, h, s, st, sets)
  local wx, wy = ui.GetWindowPos()
  local used = 0
  for _, set in ipairs(sets) do
    if set.inUse then used = used + 1 end
  end
  local full = used >= #sets and #sets > 0
  T.Text(x, y + 4 * s, string.format("%d of 10 sets", used), C.muted, T.font.small)
  local bw = 78 * s
  if T.Button("##newset", x + w - 2 * bw - 6 * s, y, x + w - bw - 6 * s, y + 24 * s, s, "New set",
      { primary = not full, enabled = not full, tooltip = "An empty set to fill from the book" }) then
    openEditor(nil, nil, false, st)
  end
  if T.Button("##gemset", x + w - bw, y, x + w, y + 24 * s, s, "From gems",
      { enabled = not full, tooltip = "A new set of the spells on your gems" }) then
    openEditor(nil, nil, true, st)
  end
  if flash and ui.GetTime() < flash[2] then
    T.Text(x, y + 32 * s, T.Fit(flash[1], w, T.font.tiny), C.good, T.font.tiny)
  else
    flash = nil
    T.Text(x, y + 32 * s, "Drag a spell from the book onto a set to add it.", C.faint, T.font.tiny)
  end
  local ly = y + 52 * s
  local listH = y + h - 20 * s - ly
  ui.SetCursorPos(x - wx, ly - wy)
  ui.BeginChild("##setlist", w, listH, false)
  local cx, cy = ui.GetWindowPos()
  local cw = ui.GetContentRegionAvail()
  local scroll = ui.GetScrollY()
  local cardH, rowGap = 62 * s, 6 * s
  local released = ui.IsMouseReleased(ui.Mouse.Left)
  local shown, openDelete = 0, false
  for n, set in ipairs(sets) do
    if set.inUse then
      local ry = cy - scroll + shown * (cardH + rowGap)
      shown = shown + 1
      local over = drag and ui.IsMouseHoveringRect(cx, ry, cx + cw, ry + cardH)
      ui.DrawRectFilled(cx, ry, cx + cw, ry + cardH, C.ink[1], C.ink[2], C.ink[3], 0.035, 6 * s)
      if over then ui.DrawRect(cx, ry, cx + cw, ry + cardH, C.accent[1], C.accent[2], C.accent[3], 0.9, 6 * s, 1.5 * s) end
      -- the card under the rest: a double-click edits it
      T.Hit("##card" .. n, cx, ry, cx + cw, ry + cardH)
      if ui.IsItemHovered() and ui.IsMouseDoubleClicked(ui.Mouse.Left) then openEditor(n, set, false, st) end
      T.Text(cx + 8 * s, ry + 6 * s, T.Fit(set.name, cw - 170 * s, T.font.small), C.text, T.font.small)
      local spells = copySpells(set.spells)
      local dropAt = nil
      local ix = cx + 8 * s
      for k = 1, SET_SLOTS do
        local id = spells[k]
        local iy = ry + 30 * s
        local hit = ui.IsMouseHoveringRect(ix, iy, ix + 20 * s, iy + 20 * s)
        if id ~= -1 then
          local tex, u0, v0, u1, v1 = spellIcon(id)
          T.Icon(ix, iy, 20 * s, s, tex, u0, v0, u1, v1, nil, SET_ICON)
          if not tex then T.Text(ix + 10 * s, iy + 3 * s, "?", C.danger, T.font.tiny, "center") end
        else
          ui.DrawRect(ix, iy, ix + 20 * s, iy + 20 * s, C.ink[1], C.ink[2], C.ink[3], 0.08, 3 * s, 1)
        end
        if over and hit then
          dropAt = k
          ui.DrawRect(ix - 1 * s, iy - 1 * s, ix + 21 * s, iy + 21 * s, C.accent[1], C.accent[2], C.accent[3], 1, 4 * s, 2 * s)
        end
        T.Hit("##si" .. n .. "_" .. k, ix, iy, ix + 20 * s, iy + 20 * s)
        if ui.IsItemHovered() and not drag then
          ui.SetTooltip(id ~= -1 and string.format("Gem %d: %s", k, spellName(id)) or string.format("Gem %d (empty)", k))
        end
        ix = ix + 22 * s
      end
      if over and released then
        local k = dropAt or slotFor(spells, nil, st.usableGems)
        if k then
          putSpell(spells, k, drag.id)
          if API.spells.EditSet(n, set.name, spells) then
            flash = { string.format("%s is in gem %d of %s", drag.name, k, set.name), ui.GetTime() + 3 }
          end
        else
          flash = { set.name .. " is full: drop the spell on one of its gems to replace it", ui.GetTime() + 4 }
        end
      end
      if T.Button("##load" .. n, cx + cw - 154 * s, ry + 4 * s, cx + cw - 104 * s, ry + 24 * s, s, "Load",
          { primary = true, tooltip = "Memorize this set" }) then
        API.spells.MemorizeSet(n)
      end
      if T.Button("##edit" .. n, cx + cw - 100 * s, ry + 4 * s, cx + cw - 58 * s, ry + 24 * s, s, "Edit") then
        openEditor(n, set, false, st)
      end
      if T.Button("##del" .. n, cx + cw - 54 * s, ry + 4 * s, cx + cw - 6 * s, ry + 24 * s, s, "Delete") then
        confirmDelete = n
        openDelete = true
      end
    end
  end
  if shown == 0 then
    T.Text(cx + 4 * s, cy + 4 * s, "No spell sets yet.", C.faint, T.font.small)
    T.Text(cx + 4 * s, cy + 22 * s, "New set starts an empty one; From gems saves your gems.", C.faint, T.font.tiny)
  end
  ui.EndChild()
  T.Text(x, y + h - 16 * s, "/memspellset <number or name> loads a set too.", C.faint, T.font.tiny)
  if openDelete then ui.OpenPopup("##confirmdelete") end
  if ui.BeginPopup("##confirmdelete") then
    local set = confirmDelete and sets[confirmDelete]
    ui.Text("Delete the spell set '" .. (set and set.name or "") .. "'?")
    if ui.Button("Delete") then
      if confirmDelete then API.spells.DeleteSet(confirmDelete) end
      confirmDelete = nil
      ui.CloseCurrentPopup()
    end
    ui.SameLine()
    if ui.Button("Cancel") then
      confirmDelete = nil
      ui.CloseCurrentPopup()
    end
    ui.EndPopup()
  end
end

local function setsTab(x, y, w, h, s, st)
  local sets = API.spells.Sets() or {}
  if editor and editor.n and not (sets[editor.n] and sets[editor.n].inUse) then editor = nil end -- deleted meanwhile
  if editor then setEditor(x, y, w, h, s, st, sets) else setList(x, y, w, h, s, st, sets) end
end

local function gemStrip(x, y, w, s, st)
  local size, gap = 46 * s, 8 * s
  local count = 12
  local total = count * size + (count - 1) * gap
  local gx0 = x + (w - total) / 2
  local memFrac = API.character.Gauge(9) -- the memorize gauge: the ticks left over 92
  for g = 0, count - 1 do
    local gx = gx0 + g * (size + gap)
    local id = st.gems[g + 1] or -1
    local usable = g < st.usableGems
    local tex, u0, v0, u1, v1
    if id ~= -1 then tex, u0, v0, u1, v1 = spellIcon(id) end
    local ring = nil
    if targetGem == g then ring = C.accent end
    GEM_ICON.wellAlpha = usable and 1 or 0.35
    T.Icon(gx, y, size, s, tex, u0, v0, u1, v1, ring, GEM_ICON)
    if not usable then
      T.Text(gx + size / 2, y + size / 2 - 7 * s, "-", C.faint, T.font.small, "center")
    elseif id == -1 then
      T.Text(gx + size / 2, y + size / 2 - 7 * s, tostring(g + 1), C.faint, T.font.small, "center")
    end
    if st.memorizing and st.memorizing.gem == g then
      local left = memFrac or 1
      ui.DrawRectFilled(gx, y, gx + size, y + size * left, 0, 0, 0, 0.55, 8 * s)
      local mt = st.memorizing.spell
      local mtex, mu0, mv0, mu1, mv1 = spellIcon(mt)
      if mtex then ui.DrawImage(mtex, gx + 3 * s, y + 3 * s, gx + size - 3 * s, y + size - 3 * s, mu0, mv0, mu1, mv1, 1, 1, 1, 0.35) end
      ui.DrawRect(gx, y, gx + size, y + size, C.gold[1], C.gold[2], C.gold[3], 0.9, 8 * s, 1.5 * s)
    end
    local hit = T.Hit(GEM_IDS[g], gx, y, gx + size, y + size)
    local hovered = ui.IsItemHovered() or (drag and ui.IsMouseHoveringRect(gx, y, gx + size, y + size))
    if usable and hovered then T.HoverFx(gx, y, gx + size, y + size, s, 8) end
    -- (only while the book is the window under the mouse: the spell bar over it takes the drop instead)
    if drag and usable and ui.IsMouseHoveringRect(gx, y, gx + size, y + size)
        and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows | ui.HoveredFlags.AllowWhenBlockedByActiveItem) then
      ui.DrawRect(gx - 2 * s, y - 2 * s, gx + size + 2 * s, y + size + 2 * s, C.accent[1], C.accent[2], C.accent[3], 1, 9 * s, 2 * s)
      if ui.IsMouseReleased(ui.Mouse.Left) then
        memorize(drag, g, st)
        dropTaken = true
      end
    end
    if hit and usable then
      if st.cursorSpell then
        API.spells.GemClick("CSPW_Spell" .. g, "left") -- the spell picked up in the book goes here
      else
        targetGem = (targetGem == g) and nil or g
      end
    end
    if ui.IsItemClicked(ui.Mouse.Right) and id ~= -1 then
      API.spells.GemClick("CSPW_Spell" .. g, "rightup") -- forget it
      T.TakeRightClick()
    end
    if ui.IsItemHovered() and not drag then
      if id ~= -1 and byId[id] then
        local replace = usable and replaces() -- Hailstone: a drop memorizes over it
        ui.SetTooltip(string.format("Gem %d: %s\n%sRight-click to forget", g + 1, byId[id].name,
          replace and "Drop a spell here, or click to pick this gem, to replace it\n" or ""))
      elseif usable then
        ui.SetTooltip(string.format("Gem %d (empty)\nDrop a spell here, or click to pick this gem", g + 1))
      else
        ui.SetTooltip(string.format("Gem %d is not available to you yet", g + 1))
      end
    end
  end
  -- the status line
  local line, col = nil, C.muted
  if st.settingSet then
    line, col = "Memorizing a spell set...", C.gold
  elseif st.memorizing then
    local e = byId[st.memorizing.spell]
    line, col = string.format("Memorizing %s...  %d%%", e and e.name or "", math.floor((1 - (memFrac or 1)) * 100)), C.gold
  elseif st.scribing then
    line, col = "Scribing...", C.gold
  elseif st.swapSlot and bySlot[st.swapSlot] then
    line, col = "Right-click another spell to swap places with " .. bySlot[st.swapSlot].name, C.accent
  elseif st.cursorSpell and bySlot[st.cursorSpell] then
    line, col = "Click a gem to memorize " .. bySlot[st.cursorSpell].name, C.accent
  elseif not st.sitting then
    line = "Sit down to memorize or scribe spells."
  elseif not (editor and tab == "sets") and not gemFor(st) then
    -- every usable gem holds a spell: a double-click has nowhere to go
    if replaces() then
      line = "Your gems are full: drag a spell onto a gem to replace it, or click a gem, then double-click the spell."
    else
      line = "Your gems are full: right-click a gem to forget its spell first."
    end
  elseif editor and tab == "sets" then
    line = "Double-click a spell to add it to the set; drag it onto a slot, or onto a gem to memorize it."
  else
    line = "Drag a spell onto a gem, or double-click it."
  end
  T.Text(x + w / 2, y + size + 8 * s, line, col, T.font.small, "center")
end

-- ---------------------------------------------------------------- window
local TABS = { { "details", "Details", "##tabdetails" }, { "sets", "Spell sets", "##tabsets" } }

local DIALOG = {
  size = function() return W, H end,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 30 end,
  keepTitle = true,
}

local function body(x, y, w, h, s)
  refresh()
  if modern_book_tab then tab, modern_book_tab = modern_book_tab, nil end -- the spell bar's "manage sets"
  -- testing: test.book.tab=details|sets, test.book.select=<spell id>, test.book.gem=<gem 1..12> (picked in the
  -- strip) and test.book.edit=<set number>|new|gems (the set editor) in ui/settings/modern.txt
  local tt, tsel = ui.GetSetting("test.book.tab"), tonumber(ui.GetSetting("test.book.select"))
  if tt then tab = tt end
  if tsel and not sel then sel = tsel end
  local tgem = tonumber(ui.GetSetting("test.book.gem"))
  if tgem and not targetGem then targetGem = tgem - 1 end
  local st = API.spells.BookState() or { gems = {}, usableGems = 8 }
  local myLevel = tonumber((API.character.Label(2))) or 1
  if targetGem and targetGem >= (st.usableGems or 8) then targetGem = nil end
  local te = ui.GetSetting("test.book.edit")
  if te and te ~= testEdit then
    testEdit, tab = te, "sets"
    local n = tonumber(te)
    openEditor(n, n and (API.spells.Sets() or {})[n], te == "gems", st)
  end
  toolbar(x, y, w, s, st)
  readSettings()
  local by = y + TOOL_H * s
  local bodyH = h - TOOL_H * s - GEMS_H * s - 8 * s
  sidebar(x, by, SIDE_W * s, bodyH, s, myLevel)
  local lx = x + (SIDE_W + 12) * s
  local lw = w - (SIDE_W + 12 + DETAIL_W + 14) * s
  local list = shownSpells(myLevel)
  columnHeader(lx + 4 * s, by, lw, s)
  local hy = cfg.grid and 0 or 16 * s
  listView(list, lx, by + hy, lw, bodyH - hy, s, st, myLevel)
  -- the right column: details / spell sets
  local rx = x + w - DETAIL_W * s
  ui.DrawRectFilled(rx - 8 * s, by, rx - 7 * s, by + bodyH, C.ink[1], C.ink[2], C.ink[3], 0.06)
  local tx = rx
  for _, t in ipairs(TABS) do
    local tw = T.TextSize(t[2], T.font.small)
    local clicked = T.Hit(t[3], tx - 4 * s, by, tx + tw + 4 * s, by + 22 * s)
    local up = tab == t[1]
    T.Text(tx, by + 2 * s, t[2], (up or ui.IsItemHovered()) and C.text or C.muted, T.font.small)
    if up then ui.DrawLine(tx, by + 21 * s, tx + tw, by + 21 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s) end
    if clicked then tab = t[1] end
    tx = tx + tw + 20 * s
  end
  local cy = by + 32 * s
  if tab == "details" then detailsTab(rx, cy, DETAIL_W * s, bodyH - 32 * s, s, st)
  else setsTab(rx, cy, DETAIL_W * s, bodyH - 32 * s, s, st) end
  -- a scroll on the cursor: scribe it into the first empty slot
  if st.holdingScroll then
    local slot = firstEmptyBookSlot()
    if slot and T.Button("##scribe", x + w - 150 * s, y + h - GEMS_H * s + 4 * s, x + w, y + h - GEMS_H * s + 30 * s, s,
        "Scribe the scroll", { primary = true, enabled = st.sitting }) then
      API.spells.BookClick(slot)
    end
  end
  ui.DrawLine(x, y + h - GEMS_H * s, x + w, y + h - GEMS_H * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  gemStrip(x, y + h - GEMS_H * s + 10 * s, w, s, st)
  -- the spell being dragged follows the mouse; let go anywhere else and it is dropped
  if drag then
    local mx, my = ui.GetMousePos()
    local tex, u0, v0, u1, v1 = API.spells.Icon(drag.icon)
    ui.Foreground(true)
    if tex then ui.DrawImage(tex, mx - 18 * s, my - 18 * s, mx + 18 * s, my + 18 * s, u0, v0, u1, v1, 1, 1, 1, 0.85) end
    ui.Foreground(false)
    -- shared with the spell bar (spellgems.lua, drawn after this), which takes the drop on the release frame: only
    -- this frame's (modern_spell_drag_frame), so a book closed mid-drag leaves nothing to memorize
    -- (a drop the book's own gems took is not offered to it as well: one release memorizes once)
    if dropTaken then
      modern_spell_drag, dropTaken = nil, false
    else
      modern_spell_drag, modern_spell_drag_frame = drag, ui.GetFrameCount()
    end
    if not ui.IsMouseDown(ui.Mouse.Left) then drag = nil end
  elseif modern_spell_drag and not ui.IsMouseReleased(ui.Mouse.Left) then
    modern_spell_drag = nil
  end
  bodyFrame = ui.GetFrameCount()
end

-- What belongs to the character the book was open for (the set being edited, a drag, the picks): camping, or another
-- character's name, clears it, so Save cannot write into the next character's set of that number.
local function forCharacter()
  local who = API.session.InGame() and (API.character.Label(1) or "") or nil
  if who == bookWho then return end
  bookWho = who
  editor, slotDrag, drag, confirmDelete, flash, targetGem, sel = nil, nil, nil, nil, nil, nil, nil
  modern_spell_drag = nil
  lastRead = -1
end

ui.window("My Spell Book", function()
  forCharacter()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Spell Book", DIALOG, body)
  if bodyFrame ~= ui.GetFrameCount() then -- the book is closed: a drag from it ends
    drag = nil
    modern_spell_drag = nil
  end
end)
