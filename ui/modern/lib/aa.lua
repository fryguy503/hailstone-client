-- Alternate Advancement: replaces the game's AAWindow. A header with the points to spend, the points spent, the AA
-- experience and the share of experience that goes to it (- / +); one list of every ability on the game's four tabs
-- (General, Archetype, Class, Special: AAWindow's ModernList, app/lua_eq_progress.cpp), with a search and filters over
-- all of it (the tab, the category, owned / not owned and the game's own "trainable only"), and columns for the tab,
-- the era (the expansion the ability came with), activated or passive, the rank and the next rank's cost (a head's
-- click sorts by it, its drag moves the column, and dragging its right edge resizes it; sorted by name to start
-- with, the text a size up from other windows'); and the selected ability at the side: its rank, the next rank's cost, the level and the abilities it
-- needs, its reuse time, the game's description, Train (with "all ranks": the server's buy all, else the client buys rank after rank) and Hotkey;
-- the selection stays on the ability as its ranks are bought.
-- Deliberate differences: the stock window shows one tab's list at a time, in the game's order. Rows are the game's
-- (eq.ListClick selects the ability, so Train and Hotkey act on it; dragging a row puts the ability's hot button on
-- the cursor, as on the stock list); an ability greyed is one the game will not train now. The requirements come from
-- the window's Modern* children.
--
-- A library (as lib/xtarget.lua is), so that a script's window named "Alternate Advancement" can be this one with
-- parts of its own: dofile("lib/aa.lua").window(opts) draws the whole window; opts (all optional):
--   row = {
--     width = n: unscaled pixels kept at the right of each row for the script's part (the name, ranks and cost stop
--       short of it, and so does the row's click and drag);
--     draw = function(r, x0, y0, x1, y1, s): draws that part (its own hit boxes take its clicks);
--     style = function(r): nil, or { ring = {r, g, b, a} (the row's outline), tag = { text, {r, g, b, a} } (a small
--       label after the name) },
--   }
--   details = function(sel, x, y, w, s): draws a part of the script's in the selected ability's panel, under its
--     requirements; returns the height it took (unscaled pixels times s, as drawn).
--   banner = function(x, y, w, s): draws a strip of the script's under the header; returns its height (0 for none).
-- r, a row: name, rank, max, cost (the next rank's, as text; "" at the maximum), category, trainable (the game
-- would train it now), maxed, page (its tab, 0-based). sel, the selected ability: id (its rank's), name, rank, max,
-- cost (text).
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
-- The window's text, one size up from the other windows' (EQ UI fonts 3 and 4: 13 and 14 pixels for 12 and 13).
local F = { tiny = T.font.tiny + 1, small = T.font.small + 1 }
local WINDOW = "AAWindow"
local TABS = { "General", "Archetype", "Class", "Special" } -- the stock tabs (the ability's type 1..4)
local TAB_KEYS = { "1", "2", "3", "4" }                      -- ModernList's tab column
local VIEWS = { "All abilities", "Owned", "Not owned", "Not maxed" }
local SIDE_W = 260      -- the details' width beside the list (design pixels)
local STACK_W = 600     -- narrower than this, the details go under the list
local LIST = "ModernList"

local search, view, category, tab = "", 1, nil, nil -- category: the Category column's text, tab: 1..4, nil any
local needle = ""                                     -- the search in lower case, this frame

local function split(text, sep)
  local out = {}
  for part in ((text or "") .. sep):gmatch("(.-)" .. sep) do out[#out + 1] = part end
  return out
end

-- split() of a child's text, kept while the text stays the same (the details read three a frame)
local splits = {}
local function splitKept(slot, text, sep)
  local c = splits[slot]
  if not c or c.text ~= text then
    c = { text = text, parts = split(text, sep) }
    splits[slot] = c
  end
  return c.parts
end

-- Names in lower case for the search, kept (a list's names come back as the same strings each frame).
local lowered, nLowered = {}, 0
local function lowerOf(text)
  local l = lowered[text]
  if not l then
    if nLowered >= 2048 then lowered, nLowered = {}, 0 end
    l = text:lower()
    lowered[text], nLowered = l, nLowered + 1
  end
  return l
end

local NO_TRAIL = { noTrail = true }

-- "cur/max" of the list's second column.
local function ranks(texts)
  local cur, max = (texts[2] or ""):match("(%-?%d+)/(%d+)")
  return tonumber(cur) or 0, tonumber(max) or 0
end

local function seconds(n)
  n = tonumber(n) or 0
  if n <= 0 then return "-" end
  if n >= 3600 then
    local h, m = n // 3600, (n % 3600) // 60
    return m > 0 and string.format("%dh %dm", h, m) or string.format("%dh", h)
  end
  if n >= 60 then
    local m, sec = n // 60, n % 60
    return sec > 0 and string.format("%dm %ds", m, sec) or string.format("%dm", m)
  end
  return n .. "s"
end

-- ---------------------------------------------------------------- the header
local POINTS_TILE = { color = C.gold, font = T.font.large, tooltip = "Points you can spend" }
local SPENT_TILE = {}
local LESS_BUTTON = { tooltip = "Less of your experience goes to AA" }
local MORE_BUTTON = { tooltip = "More of your experience goes to AA" }
local assignedText, spentTip = nil, nil

local function header(x, y, w, s)
  local h = 36 * s
  local tileW = 80 * s
  local points = W.Text(WINDOW, "CurrentCount")
  P.Tile("##aapts", x, y, tileW, h, s, "UNSPENT", points ~= "" and points or "0", POINTS_TILE)
  local assigned = W.Text(WINDOW, "AssignedCount")
  if assigned ~= assignedText then
    assignedText, spentTip = assigned, "Points spent on abilities (assigned: " .. assigned .. ")"
  end
  SPENT_TILE.tooltip = spentTip
  P.Tile("##aaspent", x + tileW + 6 * s, y, tileW, h, s, "SPENT", W.Text(WINDOW, "TotalCount"), SPENT_TILE)
  -- the AA experience, and at its right the share of experience going to it
  local gx = x + 2 * tileW + 16 * s
  local gw = w - (gx - x)
  local frac, text = eq.Gauge(5)
  T.Text(gx, y + 3 * s, "AA experience", C.muted, F.small)
  local ew = T.TextSize("AA experience", F.small)
  T.Text(gx + ew + 8 * s, y + 3 * s, frac and string.format("%.1f%%", frac * 100) or (text or ""), C.text,
    F.small)
  local bw, pctW = 22 * s, 42 * s
  local bx = gx + gw - (2 * bw + pctW)
  T.Text(bx - 6 * s, y + 3 * s, "To AA", C.faint, F.small, "right")
  ui.SetCursorScreenPos(bx, y + 1 * s)
  LESS_BUTTON.width, LESS_BUTTON.height = bw, 18 * s
  W.Button(WINDOW, "LessExpButton", "-", LESS_BUTTON)
  T.Text(bx + bw + pctW / 2, y + 3 * s, W.Text(WINDOW, "ExpCount"), C.aa, F.small, "center")
  ui.SetCursorScreenPos(bx + bw + pctW, y + 1 * s)
  MORE_BUTTON.width, MORE_BUTTON.height = bw, 18 * s
  W.Button(WINDOW, "MoreExpButton", "+", MORE_BUTTON)
  T.Bar("aa.exp", gx, y + 25 * s, gx + gw, y + 31 * s, s, frac or 0, C.aa, NO_TRAIL)
  ui.SetCursorScreenPos(x, y + h + 6 * s)
end

-- ---------------------------------------------------------------- the list
-- The categories in the tab's rows (seen: a set, cats: in order), worked out again only when a row's category
-- column changes.
local catRows, seen, cats = {}, {}, {}
local function categories(rows)
  local same = #rows == #catRows
  if same then
    for i, t in ipairs(rows) do
      if (t[4] or "") ~= catRows[i] then
        same = false
        break
      end
    end
  end
  if same then return seen, cats end
  catRows, seen, cats = {}, {}, {}
  for i, t in ipairs(rows) do
    local c = t[4] or ""
    catRows[i] = c
    if c ~= "" and not seen[c] then seen[c] = true; cats[#cats + 1] = c end
  end
  table.sort(cats)
  return seen, cats
end

local CAN_PURCHASE = { tooltip = "The game's filter: abilities you meet the requirements for and can afford" }
local CAN_PURCHASE_LABEL = "Can train now"

local function combo(id, w, current, items, none)
  ui.SetNextItemWidth(w)
  local picked = current
  if ui.BeginCombo(id, current and items[current] or none, ui.ComboFlags.HeightLarge) then
    if none and ui.Selectable(none, current == nil) then picked = nil end
    for i, v in ipairs(items) do
      if ui.Selectable(v, current == i) then picked = i end
    end
    ui.EndCombo()
  end
  return picked
end

-- The search and the filters, across the window over every tab's abilities.
local catIndex = {}
local function filters(w, s, rows)
  local seen, cats = categories(rows)
  if category and not seen[category] then category = nil end
  local checkW = T.TextSize(CAN_PURCHASE_LABEL, F.small) + 30 * s
  local gap = 6 * s
  local tabW, catW, viewW = math.min(100 * s, w * 0.16), math.min(130 * s, w * 0.2), math.min(110 * s, w * 0.17)
  local searchW = w - tabW - catW - viewW - checkW - 4 * gap
  local wrap = searchW < 120 * s -- too narrow for it all: the check box on a line of its own
  if wrap then searchW = searchW + checkW + gap end
  search = P.Search("##aasearch", search, "Search abilities", searchW)
  ui.SameLine(0, gap)
  tab = combo("##aatab", tabW, tab, TABS, "All tabs")
  ui.SameLine(0, gap)
  for i = #catIndex, 1, -1 do catIndex[i] = nil end
  local ci = nil
  for i, c in ipairs(cats) do
    catIndex[i] = c
    if c == category then ci = i end
  end
  ci = combo("##aacat", catW, ci, catIndex, "All categories")
  category = ci and catIndex[ci] or nil
  ui.SameLine(0, gap)
  view = combo("##aaview", viewW, view, VIEWS) or view
  if not wrap then ui.SameLine(0, gap) end
  W.Check(WINDOW, "CanPurchaseFilter", CAN_PURCHASE_LABEL, CAN_PURCHASE)
end

local opts = nil    -- this frame's window options (M.window)
local hookRow = {}  -- the row as opts.row's functions see it (kept)

-- ---------------------------------------------------------------- the columns
-- Each column: its head, its preferred width (design pixels; the name has none: it takes what the others leave), its
-- alignment and its sort key from a row's texts (one type per column). A head's click sorts by that column (again:
-- the other way), its drag moves the column, its right edge resizes it against the next column (settings aa.columns,
-- aa.sort, aa.widths; only the fixed columns' widths are kept, so the name still takes up a wider window).
-- Narrow, the category goes first, then the tab. The list starts sorted by name, A to Z.
local COLUMNS = {
  name = { head = "ABILITY", min = 80, tip = "Sort by name; drag to move the column" },
  category = { head = "CATEGORY", w = 110, min = 48, drop = 1 },
  tab = { head = "TAB", w = 62, min = 36, drop = 2 },
  era = { head = "ERA", w = 104, min = 44, tip = "Sort by expansion, in release order; drag to move the column" },
  kind = { head = "TYPE", w = 52, min = 44, tip = "Sort activated, then passive; drag to move the column" },
  rank = { head = "RANK", w = 44, min = 38, align = "center" },
  cost = { head = "COST", w = 38, min = 36, align = "right",
    tip = "Sort by the next rank's cost (maxed last); drag to move the column" },
}
local DEFAULT_ORDER = { "name", "category", "tab", "era", "kind", "rank", "cost" }
for i, key in ipairs(DEFAULT_ORDER) do
  local c = COLUMNS[key]
  c.key, c.home = key, i
  c.tip = c.tip or ("Sort by " .. c.head:lower() .. "; drag to move the column")
end

local order = nil         -- the columns' keys, left to right
local widths = {}        -- the fixed columns' preferred widths (design pixels); a smaller window shrinks them as drawn
local sortKey, sortAsc = "name", true
local orderVersion = 0    -- bumped when the order or widths change (the kept layout is then worked out again)

local function loadColumns()
  order = {}
  local have = {}
  for key in (ui.GetSetting("aa.columns") or ""):gmatch("[^,]+") do
    if COLUMNS[key] and not have[key] then order[#order + 1] = key; have[key] = true end
  end
  for _, key in ipairs(DEFAULT_ORDER) do -- (a column the setting does not name: at its default place)
    if not have[key] then table.insert(order, math.min(#order + 1, COLUMNS[key].home), key) end
  end
  for key, value in (ui.GetSetting("aa.widths") or ""):gmatch("(%w+)=([^,]+)") do
    local c, n = COLUMNS[key], tonumber(value)
    if c and c.w and n and n == n and math.abs(n) < math.huge then -- (not the name's: it takes what the rest leave)
      widths[key] = math.max(c.min, math.min(n, 2000))
    end
  end
  local k, dir = (ui.GetSetting("aa.sort") or ""):match("^(%w+):(%a+)$")
  if k and COLUMNS[k] then sortKey, sortAsc = k, dir ~= "desc" end
end

local function saveOrder()
  ui.SetSetting("aa.columns", table.concat(order, ","))
  orderVersion = orderVersion + 1
end

local function saveSort()
  ui.SetSetting("aa.sort", sortKey .. ":" .. (sortAsc and "asc" or "desc"))
end

local function saveWidths()
  local values = {}
  for _, key in ipairs(DEFAULT_ORDER) do
    if widths[key] then values[#values + 1] = string.format("%s=%.2f", key, widths[key]) end
  end
  ui.SetSetting("aa.widths", table.concat(values, ","))
end

-- The columns shown from x0 to x1 (the script's part already taken off), left to right in the chosen order; each
-- { def, x0, x1 }. Kept: the heads and every row read the same layout while the width and the order stay the same.
local lay = { n = 0 }
local layKey0, layKey1, layS, layVersion = nil, nil, nil, -1
local function columns(x0, x1, s)
  if x0 == layKey0 and x1 == layKey1 and s == layS and layVersion == orderVersion then return lay end
  layKey0, layKey1, layS, layVersion = x0, x1, s, orderVersion
  local pad, gap = 8 * s, 6 * s
  local hide1, hide2 = false, false
  -- A column is never drawn narrower than its head and the sort arrow (columnHeads), whatever its saved width: the
  -- head's font does not grow in step with the scale.
  local function least(c) return math.max(c.min * s, T.TextSize(c.head, F.tiny) + 11 * s + 1) end
  local function width(key, c) return math.max((widths[key] or c.w) * s, least(c)) end
  local function fixed()
    local t = 0
    for _, key in ipairs(order) do
      local c = COLUMNS[key]
      if c.w and not (c.drop == 1 and hide1) and not (c.drop == 2 and hide2) then
        t = t + width(key, c) + gap
      end
    end
    return t
  end
  local room = math.max(0, (x1 - pad) - (x0 + pad))
  if room - fixed() < 150 * s then hide1 = true end
  if room - fixed() < 130 * s then hide2 = true end
  local free = room - fixed()
  -- how far the name can shrink (design pixels) before the narrow layout would drop a column
  local keep = not hide1 and 150 * s or (not hide2 and 130 * s or COLUMNS.name.min * s)
  lay.slack = math.max(0, free - keep) / s
  local nameW = math.max(COLUMNS.name.min * s, free)
  local n, total, minimum = 0, 0, 0
  for _, key in ipairs(order) do
    local c = COLUMNS[key]
    if not (c.drop == 1 and hide1) and not (c.drop == 2 and hide2) then
      n = n + 1
      local e = lay[n]
      if not e then e = {}; lay[n] = e end
      e.def, e.w, e.min = c, c.w and width(key, c) or nameW, c.w and least(c) or c.min * s
      total, minimum = total + e.w, minimum + e.min
    end
  end
  for k = n + 1, #lay do lay[k] = nil end
  lay.n = n
  gap = math.min(gap, room / math.max(1, 2 * n))
  local available = math.max(0, room - (n - 1) * gap)
  -- Keep the requested widths when there is room. Below that, shrink toward each column's minimum together.
  local fraction = total > available and total > minimum and math.max(0, (available - minimum) / (total - minimum)) or 1
  local squeeze = minimum > available and available / minimum or 1
  local x = x0 + pad
  for i = 1, n do
    local e = lay[i]
    local w = (e.min + (e.w - e.min) * fraction) * squeeze
    e.x0, e.x1 = x, x + w
    x = x + w + gap
  end
  return lay
end

-- the x a column's text starts at (left), centres on or ends at (right)
local function anchor(e)
  local a = e.def.align
  if a == "center" then return (e.x0 + e.x1) / 2, a end
  if a == "right" then return e.x1, a end
  return e.x0, nil
end

-- A head: a click sorts by it, a drag moves it (dropped between the two heads nearest the mouse).
local dragging = nil -- the key of the column being dragged
local resizing = nil -- the edge being dragged: the fixed columns either side and their starting widths
local function columnHeads(x0, y, x1, s)
  local L = columns(x0, x1, s)
  local f, k = F.tiny, C.faint
  local _, hh = T.TextSize("Ag", f)
  local released = nil
  local resizeBy = nil
  for i = 1, L.n do
    local e = L[i]
    local c = e.def
    local right = L[i + 1]
    local edgeHovered, edgeActive = false, false
    if right then -- (the last column's right edge is the list's)
      ui.DrawLine(e.x1, y, e.x1, y + hh, C.ink[1], C.ink[2], C.ink[3], 0.16, 1 * s)
      T.Hit("##aaresize" .. c.key, e.x1 - 3 * s, y - 2 * s, e.x1 + 3 * s, y + hh + 2 * s)
      edgeHovered, edgeActive = ui.IsItemHovered(), ui.IsItemActive()
    end
    if edgeActive and not resizing then
      -- The fixed column on each side of the edge changes, from its preferred width; the name, the flexible column,
      -- takes up the difference and is never given a width of its own.
      local b = right.def
      resizing = { key = c.key, x = ui.GetMousePos(), applied = 0 }
      local lo, hi
      if c.w then
        resizing.left, resizing.leftWidth = c.key, widths[c.key] or c.w
        lo = c.min - resizing.leftWidth
      else
        lo = -L.slack
      end
      if b.w then
        resizing.right, resizing.rightWidth = b.key, widths[b.key] or b.w
        hi = resizing.rightWidth - b.min
      else
        hi = L.slack
      end
      resizing.lo, resizing.hi = math.min(0, lo), math.max(0, hi)
      dragging = nil
    end
    if edgeHovered or resizing and resizing.key == c.key then
      ui.DrawLine(e.x1, y - 2 * s, e.x1, y + hh + 2 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s)
      if not resizing then ui.SetTooltip("Drag to resize " .. c.head:lower()) end
    end
    if resizing and resizing.key == c.key then
      local mx = ui.GetMousePos()
      resizeBy = math.max(resizing.lo, math.min(resizing.hi, (mx - resizing.x) / s))
    end
    local clicked = T.Hit("##aahead" .. c.key, e.x0, y - 2 * s, e.x1 - 3 * s, y + hh + 2 * s)
    local hovered, active = ui.IsItemHovered(), ui.IsItemActive()
    if active and not resizing and ui.IsMouseDragging(ui.Mouse.Left) then dragging = c.key end
    if dragging == c.key and not active then released = c.key end
    if clicked and not resizing and dragging ~= c.key then
      if sortKey == c.key then sortAsc = not sortAsc else sortKey, sortAsc = c.key, true end
      saveSort()
    end
    if hovered and not dragging and not resizing then ui.SetTooltip(c.tip .. "; drag the right edge to resize") end
    local on = sortKey == c.key
    if hovered or dragging == c.key then
      ui.DrawRectFilled(e.x0 - 3 * s, y - 2 * s, e.x1 + 3 * s, y + hh + 2 * s, C.ink[1], C.ink[2], C.ink[3], 0.06,
        4 * s)
    end
    local ax, align = anchor(e)
    local tc = on and C.text or k
    -- the sort arrow beside the head's text, on the side away from the column's edge
    local aw = 7 * s
    local head = T.Fit(c.head, e.x1 - e.x0 - aw - 4 * s, f)
    local tw = T.TextSize(head, f)
    local tx = align == "right" and ax - aw - 3 * s or (align == "center" and ax - aw / 2 - 2 * s or ax)
    ui.PushClipRect(e.x0, y - 2 * s, e.x1, y + hh + 2 * s, true)
    T.Text(tx, y, head, tc, f, align)
    if on then
      local axl = align == "right" and ax - aw or (align == "center" and tx + tw / 2 + 3 * s or tx + tw + 4 * s)
      local my = y + hh / 2
      local a = C.accent
      local d = (sortAsc and 2.5 or -2.5) * s -- (up for A to Z, down the other way)
      ui.DrawTriangleFilled(axl, my + d, axl + aw, my + d, axl + aw / 2, my - d, a[1], a[2], a[3], 1)
    end
    ui.PopClipRect()
  end
  -- Apply changes after drawing: the rows and heads keep one layout for the whole frame.
  if resizing then
    if resizeBy and resizeBy ~= resizing.applied then
      if resizing.left then widths[resizing.left] = resizing.leftWidth + resizeBy end
      if resizing.right then widths[resizing.right] = resizing.rightWidth - resizeBy end
      resizing.applied, resizing.changed = resizeBy, true
      orderVersion = orderVersion + 1
    end
    if not ui.IsMouseDown(ui.Mouse.Left) then
      if resizing.changed then saveWidths() end
      resizing = nil
    end
  end
  if dragging then
    -- where it would go: the gap before the first head whose middle is right of the mouse
    local mx = ui.GetMousePos()
    local at = L.n + 1
    for i = 1, L.n do
      if mx < (L[i].x0 + L[i].x1) / 2 then at = i; break end
    end
    local gx = at <= L.n and L[at].x0 - 3 * s or L[L.n].x1 + 3 * s
    local a = C.accent
    ui.DrawLine(gx, y - 3 * s, gx, y + hh + 3 * s, a[1], a[2], a[3], 1, 2 * s)
    if released then
      -- the drop place in the whole order (a column hidden while narrow keeps its place before its neighbour)
      local target = at <= L.n and L[at].def.key or nil
      local from = nil
      for i, key in ipairs(order) do if key == released then from = i end end
      if from and target ~= released then
        table.remove(order, from)
        local to = #order + 1
        if target then
          for i, key in ipairs(order) do if key == target then to = i end end
        end
        table.insert(order, to, released)
        saveOrder()
      end
      dragging = nil
    elseif not ui.IsMouseDown(ui.Mouse.Left) then
      dragging = nil
    end
  end
end

local function resetColumns()
  order, widths = {}, {}
  for i, key in ipairs(DEFAULT_ORDER) do order[i] = key end
  sortKey, sortAsc, dragging, resizing = "name", true, nil, nil
  orderVersion = orderVersion + 1
  ui.SetSetting("aa.columns", "")
  ui.SetSetting("aa.widths", "")
  ui.SetSetting("aa.sort", "")
end

-- ---------------------------------------------------------------- the sort
-- Each listed row's key for the sort column and its name (lower case), worked out once a frame in the filter, so
-- that the sort compares only kept values. Ties go by name, then by the game's order.
local keys, names = {}, {}
local function keyOf(texts, cur, max)
  if sortKey == "name" then return lowerOf(texts[1] or "") end
  if sortKey == "category" then return lowerOf(texts[4] or "") end
  if sortKey == "tab" then return tonumber(texts[5]) or 0 end
  if sortKey == "era" then return tonumber(texts[8]) or 0 end
  if sortKey == "kind" then return texts[7] == "1" and 0 or 1 end
  if sortKey == "rank" then return math.max(cur, 0) * 1000 + max end
  if max > 0 and cur >= max then return math.huge end -- (cost: maxed after every price, either way)
  return tonumber(texts[3]) or 0
end

local function sortRows(a, b)
  local ka, kb = keys[a.row], keys[b.row]
  if ka ~= kb then
    if sortAsc then return ka < kb end
    return ka > kb
  end
  local na, nb = names[a.row], names[b.row]
  if na ~= nb then return na < nb end
  return a.row < b.row
end

local function accepts(row, texts)
  if tab and texts[5] ~= TAB_KEYS[tab] then return false end
  local lname = lowerOf(texts[1] or "")
  if needle ~= "" and not lname:find(needle, 1, true) then return false end
  if category and (texts[4] or "") ~= category then return false end
  local cur, max = ranks(texts)
  if view == 2 and cur <= 0 then return false end
  if view == 3 and cur > 0 then return false end
  if view == 4 and cur >= max then return false end
  keys[row], names[row] = keyOf(texts, cur, max), lname
  return true
end

-- the row as each cell sees it (kept)
local cell = {}

local CELLS = {}
function CELLS.name(e, texts, ty, tiny, s)
  local nameW = e.x1 - e.x0
  local name = texts[1] or ""
  local tag = cell.style and cell.style.tag
  local color = cell.trainable and C.text or C.muted
  if tag then
    local tw = T.TextSize(tag[1], F.tiny) + 10 * s
    local fitted = T.Fit(name, nameW - tw - 6 * s, F.small)
    T.Text(e.x0, ty, fitted, color, F.small)
    local nx = e.x0 + T.TextSize(fitted, F.small) + 6 * s
    local _, tt = T.TextSize("Ag", F.tiny)
    local tc = tag[2] or C.accent
    ui.DrawRectFilled(nx, tiny - 1 * s, nx + tw, tiny + tt + 1 * s, tc[1], tc[2], tc[3], 0.18, 4 * s)
    T.Text(nx + 5 * s, tiny, tag[1], tc, F.tiny)
  else
    T.Text(e.x0, ty, T.Fit(name, nameW, F.small), color, F.small)
  end
end
function CELLS.category(e, texts, _, tiny)
  T.Text(e.x0, tiny, T.Fit(texts[4] or "", e.x1 - e.x0, F.tiny), C.faint, F.tiny)
end
function CELLS.tab(e, _, _, tiny)
  T.Text(e.x0, tiny, T.Fit(TABS[cell.page + 1] or "", e.x1 - e.x0, F.tiny), C.muted, F.tiny)
end
function CELLS.era(e, texts, _, tiny)
  local era = texts[6] or ""
  T.Text(e.x0, tiny, era ~= "" and T.Fit(era, e.x1 - e.x0, F.tiny) or "-", C.faint, F.tiny)
end
function CELLS.kind(e, texts, _, tiny)
  local active = texts[7] == "1"
  T.Text(e.x0, tiny, active and "Active" or "Passive", active and C.accent or C.faint, F.tiny)
end
function CELLS.rank(e, _, _, tiny)
  T.Text((e.x0 + e.x1) / 2, tiny, string.format("%d/%d", math.max(cell.cur, 0), cell.max),
    cell.maxed and C.aa or C.muted, F.tiny, "center")
end
function CELLS.cost(e, texts, ty, tiny)
  local cost = texts[3] or ""
  if cell.maxed then
    T.Text(e.x1, tiny, "MAX", C.aa, F.tiny, "right")
  elseif cost ~= "" then
    T.Text(e.x1, ty, cost, cell.trainable and C.gold or C.faint, F.small, "right")
  end
end

local function drawRow(_, texts, x0, y0, x1, y1, s, st)
  local cur, max = ranks(texts)
  local trainable = st.color[1] > 0.9 and st.color[2] > 0.9 -- the game greys what it will not train
  local maxed = max > 0 and cur >= max
  local page = (tonumber(texts[5]) or 1) - 1
  local hook = opts and opts.row
  local style = nil
  if hook then
    local r = hookRow
    r.name, r.rank, r.max, r.cost, r.category, r.trainable, r.maxed, r.page = texts[1] or "", math.max(cur, 0), max,
      texts[3] or "", texts[4] or "", trainable, maxed, page
    if hook.style then style = hook.style(r) end
    if style and style.ring then
      local c = style.ring
      ui.DrawRect(x0 + 1 * s, y0 + 1 * s, x1 - 1 * s, y1 - 1 * s, c[1], c[2], c[3], c[4] or 1, 5 * s, 1.5 * s)
    end
    if hook.width and hook.width > 0 then
      local px = x1 - hook.width * s
      if hook.draw then hook.draw(r, px, y0, x1, y1, s) end
      x1 = px
    end
  end
  cell.cur, cell.max, cell.maxed, cell.trainable, cell.page, cell.style = cur, max, maxed, trainable, page, style
  local L = columns(x0, x1, s)
  local _, th = T.TextSize("Ag", F.small)
  local _, tt = T.TextSize("Ag", F.tiny)
  local ty = (y0 + y1 - th) / 2
  local tiny = (y0 + y1 - tt) / 2
  for i = 1, L.n do
    local e = L[i]
    ui.PushClipRect(e.x0, y0, e.x1, y1, true)
    CELLS[e.def.key](e, texts, ty, tiny, s)
    ui.PopClipRect()
  end
  cell.style = nil
end

-- ---------------------------------------------------------------- the selected ability
local function details(x, y, w, h, s)
  local sel = splitKept("selected", W.Text(WINDOW, "ModernSelected"), "\t")
  ui.DrawRectFilled(x, y, x + w, y + h, C.ink[1], C.ink[2], C.ink[3], 0.025, 8 * s)
  local pad = 10 * s
  local ix, iw = x + pad, w - 2 * pad
  if not sel[1] or sel[1] == "" then
    ui.SetCursorScreenPos(x, y)
    P.Empty(w, h, "Select an ability", "Its rank, cost and requirements show here")
    return
  end
  local name, cur, max, cost = sel[2] or "", tonumber(sel[3]) or 0, tonumber(sel[4]) or 0, sel[5] or ""
  local yy = y + pad
  ui.PushClipRect(x, y, x + w, y + h, true)
  -- name, kind
  local reuse = splitKept("reuse", W.Text(WINDOW, "ModernReuse"), "\t")
  local activated = reuse[1] ~= nil and reuse[1] ~= ""
  T.Text(ix, yy, T.Fit(name, iw, T.font.normal), C.text, T.font.normal)
  local _, nh = T.TextSize("Ag", T.font.normal)
  yy = yy + nh + 4 * s
  local cw = P.Chip(ix, yy, s, activated and "Activated" or "Passive", activated and C.accent or C.muted)
  if cur >= max and max > 0 then P.Chip(ix + cw + 4 * s, yy, s, "Maxed", C.aa)
  elseif cur > 0 then P.Chip(ix + cw + 4 * s, yy, s, "Owned", C.pet) end
  yy = yy + 22 * s
  -- rank
  T.Text(ix, yy, "Rank", C.muted, F.small)
  T.Text(ix + iw, yy, string.format("%d of %d", math.max(cur, 0), max), C.text, F.small, "right")
  yy = yy + 18 * s
  T.Bar("aa.selrank", ix, yy, ix + iw, yy + 6 * s, s, max > 0 and math.max(cur, 0) / max or 0, C.aa, NO_TRAIL)
  yy = yy + 14 * s
  -- cost, level, reuse
  local level = W.Text(WINDOW, "ModernLevel")
  local _, levelMet = W.State(WINDOW, "ModernLevel")
  local tw = (iw - 8 * s) / 3
  P.Tile("##aacost", ix, yy, tw, 36 * s, s, "NEXT RANK", cost ~= "" and (cost .. " pt") or "-",
    { color = cost ~= "" and C.gold or C.faint, font = F.small })
  P.Tile("##aalevel", ix + tw + 4 * s, yy, tw, 36 * s, s, "LEVEL", level ~= "" and level or "-",
    { color = (level ~= "" and levelMet == false) and C.danger or C.text, font = F.small,
      tooltip = level ~= "" and (levelMet == false and "You are below the level this rank needs"
        or "The level this rank needs") or nil })
  local reuseText = activated and seconds(reuse[1]) or "-"
  local left = tonumber(reuse[2]) or 0
  P.Tile("##aareuse", ix + 2 * (tw + 4 * s), yy, tw, 36 * s, s, left > 0 and "READY IN" or "REUSE",
    left > 0 and seconds(left) or reuseText, { color = left > 0 and C.hpLow or C.text, font = F.small,
      tooltip = activated and ("Reuse time " .. reuseText) or "A passive ability" })
  yy = yy + 42 * s
  -- prerequisites
  local prereqs = W.Text(WINDOW, "ModernPrereqs")
  if prereqs ~= "" then
    T.Text(ix, yy, "Requires", C.muted, F.small)
    yy = yy + 18 * s
    for line in (prereqs .. "\n"):gmatch("(.-)\n") do
      local p = split(line, "\t")
      local met = p[3] == "1"
      local c = met and C.pet or C.danger
      ui.DrawCircleFilled(ix + 4 * s, yy + 7 * s, 3 * s, c[1], c[2], c[3], 1)
      T.Text(ix + 14 * s, yy, T.Fit(p[1] or "", iw - 70 * s, F.small), met and C.text or C.muted, F.small)
      T.Text(ix + iw, yy, "rank " .. (p[2] or "1"), met and C.faint or C.danger, F.tiny, "right")
      yy = yy + 18 * s
    end
    yy = yy + 4 * s
  end
  if opts and opts.details then
    local hookSel = { id = tonumber(sel[1]), name = name, rank = math.max(cur, 0), max = max, cost = cost }
    yy = yy + (opts.details(hookSel, ix, yy, iw, s) or 0)
  end
  ui.PopClipRect()
  -- the game's description, then the buttons
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local trainAll = eq.ChildShown(WINDOW, "TrainAllButton") ~= false
  local foot = btnH + 8 * s + (trainAll and (lh + 8 * s) or 0)
  ui.DrawLine(ix, yy, ix + iw, yy, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  yy = yy + 6 * s
  local descH = math.max(30 * s, y + h - pad - foot - yy)
  ui.SetCursorScreenPos(ix, yy)
  ui.PushFont(F.small)
  W.Stml(WINDOW, "Description", iw, descH)
  ui.PopFont()
  ui.SetCursorScreenPos(ix, y + h - pad - foot + 4 * s)
  if trainAll then
    W.Check(WINDOW, "TrainAllButton", "Buy every rank I can",
      { tooltip = "Train buys every rank of this ability you can afford, one after another" })
  end
  ui.SetCursorScreenPos(ix, y + h - pad - btnH)
  ui.BeginGroup()
  local bw = (iw - 8 * s) / 2
  W.Button(WINDOW, "TrainButton", cost ~= "" and ("Train  (" .. cost .. " pt)") or "Train",
    { primary = true, width = bw, height = btnH, tooltip = "Buy the next rank of this ability" })
  ui.SameLine(0, 8 * s)
  W.Button(WINDOW, "HotButton", "Hotkey", { width = bw, height = btnH,
    tooltip = "Put this ability's hot button on the cursor (or drag its row)" })
  ui.EndGroup()
end

-- ---------------------------------------------------------------- the window
local TEST_CLASS = WINDOW .. ".class"
local function testClass() tab = 3 end

local DIALOG = {
  size = function() return 840, 500 end,
  resizable = { 420, 400 },
  keepTitle = true,
}

local ROWS = { rowH = 24, drag = true, filter = accepts, sort = sortRows, draw = drawRow }

local function body(x, y, w, h, s)
  -- The dialog's minimum is an outer size; at that minimum its padding leaves less room for the contents.
  local availableW, availableH = ui.GetContentRegionAvail()
  w, h = math.min(w, availableW), math.min(h, availableH)
  header(x, y, w, s)
  if opts and opts.banner then
    local bx, by = ui.GetCursorScreenPos()
    local bh = opts.banner(bx, by, w, s) or 0
    ui.SetCursorScreenPos(bx, by + (bh > 0 and bh + 6 * s or 0))
  end
  local rows = eq.WindowList(WINDOW, LIST) or {}
  filters(w, s, rows)
  needle = search:lower()
  local stacked = w < STACK_W * s
  local side = stacked and w or math.min(SIDE_W * s, w * 0.4)
  local cx, cy = ui.GetCursorScreenPos()
  cy = cy + 2 * s
  local bottom = y + h
  local listW = stacked and w or (w - side - 8 * s)
  local listBottom = stacked and math.max(cy + (bottom - cy) * 0.4, bottom - 320 * s) or bottom
  -- the column heads and the list
  ui.SetCursorScreenPos(cx, cy)
  ui.BeginChild("##aaleft", listW, listBottom - cy, false)
  local lx, ly = ui.GetCursorScreenPos()
  local reserve = opts and opts.row and opts.row.width or nil
  local _, hh = T.TextSize("Ag", F.tiny)
  ui.SetCursorScreenPos(lx, ly + hh + 4 * s)
  local _, lh = T.TextSize("Ag", F.tiny)
  ROWS.rows = rows -- (P.Rows reads the list itself where it does not take these)
  ROWS.reserve = reserve
  ROWS.scale = s
  ROWS.empty = #rows == 0 and "No abilities" or "No ability matches"
  ROWS.emptySub = #rows == 0 and "" or "Clear the search or the filters to see more"
  local total, _, shown = P.Rows(WINDOW, LIST, 0, -(lh + 8 * s), ROWS)
  -- The footer starts at the rows' bottom, not after EndChild's item spacing (which need not fit the space kept).
  local fx, _, _, fy = ui.GetItemRect()
  ui.SetCursorScreenPos(fx, fy)
  local footerW = ui.GetContentRegionAvail()
  -- The rows have measured this frame's scroll bar and child padding. Draw the fixed heads at those same bounds.
  columnHeads(ROWS.innerX, ly, ROWS.innerX + ROWS.innerW - (reserve or 0) * s, s)
  local count = shown == total and string.format("%d abilities", total)
      or string.format("%d of %d abilities", shown, total)
  local resetW = T.TextSize("Reset columns", F.tiny) + 16 * s
  T.Text(fx, fy + 2 * s, T.Fit(count .. "   -   drag a row to make a hot button", footerW - resetW - 8 * s, F.tiny),
    C.faint, F.tiny)
  ui.SetCursorScreenPos(math.floor(fx + footerW - resetW), fy)
  ui.PushFont(F.tiny)
  if ui.Button("Reset columns##aareset", resetW, lh + 4 * s) then resetColumns() end
  ui.PopFont()
  if ui.IsItemHovered() then ui.SetTooltip("Restore the default column widths, order and name sort") end
  ui.EndChild()
  -- the details
  local dx, dy = stacked and cx or (cx + listW + 8 * s), stacked and (listBottom + 6 * s) or cy
  details(dx, dy, side, bottom - dy, s)
  ui.SetCursorScreenPos(x, y)
  ui.Dummy(w, h)
end

local M = {}

function M.window(o)
  if not eq.InGame() then return end
  opts = o
  if not order then loadColumns() end
  -- testing: test.open.AAWindow.class=<s> shows the Class tab's abilities
  P.TestOpen(TEST_CLASS, testClass)
  hud.Dialog(WINDOW, "Alternate Advancement", DIALOG, body)
  opts = nil
end

return M
