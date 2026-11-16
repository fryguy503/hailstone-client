-- Pieces the market windows share (bazaar, barter, point and mercenary merchants): prices as coin chips, a search
-- box, empty states, and M.Table, a list child of the game window as a sortable, filterable table whose columns
-- the script draws (an icon beside the name, a price as coins, a cost the player cannot pay in red).
--
-- M.Table keeps a copy of the list's rows: a list child rebuilds every row each time one is asked for, so reading a
-- long list (a bazaar search's results) every frame would cost rows x rows. The copy is read again whole when the
-- row count, the first or the last row changes (or after M.Refresh), else a few rows a frame, so a long list
-- stays current within a few seconds.
-- Clicks still go to the original (hs.ListClick with the original's row), so the window's own handler selects,
-- inspects or picks up.
local API = hs.UI
local T = dofile("lib/theme.lua")
local W = dofile("lib/widgets.lua")
local C = T.color

local M = {}

-- ---------------------------------------------------------------- money
local COINS = { { "p", C.platinum }, { "g", C.gold }, { "s", C.silver }, { "c", C.copper } }
M.COIN_COLOR = { COINS[1][2], COINS[2][2], COINS[3][2], COINS[4][2] }
M.COIN_NAME = { "Platinum", "Gold", "Silver", "Copper" }

-- copper -> { pp, gp, sp, cp }
function M.Split(copper)
  copper = math.max(0, math.floor(tonumber(copper) or 0))
  return { copper // 1000, copper % 1000 // 100, copper % 100 // 10, copper % 10 }
end

-- "12p 3g 5c" (the game's compact coin text) -> copper; nil when it is not one.
function M.ParseCoins(text)
  if not text or text == "" then return nil end
  local total, any = 0, false
  for n, unit in text:gmatch("(%d+)%s*([pgsc])") do
    any = true
    local v = tonumber(n) or 0
    total = total + (unit == "p" and v * 1000 or unit == "g" and v * 100 or unit == "s" and v * 10 or v)
  end
  return any and total or nil
end

-- A price as coin chips on the current line ("12p 3g"; zero coins left out, "0c" for nothing). opts: dim (greyed),
-- color (one colour for the numbers: a price the player cannot pay).
function M.Price(copper, opts)
  opts = opts or {}
  local parts = M.Split(copper)
  local first = true
  for i = 1, 4 do
    if parts[i] ~= 0 or (i == 4 and first) then
      if not first then ui.SameLine(0, 5) end
      first = false
      local t = opts.color or (opts.dim and C.faint or C.text)
      ui.TextColored(t[1], t[2], t[3], 1, T.Thousands(parts[i]))
      ui.SameLine(0, 1)
      local c = opts.dim and C.faint or COINS[i][2]
      ui.TextColored(c[1], c[2], c[3], 1, COINS[i][1])
    end
  end
end

-- The width M.Price takes (to right-align it in a cell).
function M.PriceWidth(copper)
  local parts = M.Split(copper)
  local w, first = 0, true
  for i = 1, 4 do
    if parts[i] ~= 0 or (i == 4 and first) then
      if not first then w = w + 5 end
      first = false
      w = w + ui.CalcTextSize(T.Thousands(parts[i])) + 1 + ui.CalcTextSize(COINS[i][1])
    end
  end
  return w
end

-- ---------------------------------------------------------------- text
-- A faint line centred in the space left (an empty list's message). h: the height to centre in (0: one line).
function M.Empty(text, h)
  local w = ui.GetContentRegionAvail()
  local tw, th = ui.CalcTextSize(text)
  local x, y = ui.GetCursorScreenPos()
  local yy = y + math.max(0, ((h or 0) - th) / 2)
  ui.DrawText(x + math.max(0, (w - tw) / 2), yy, C.faint[1], C.faint[2], C.faint[3], 1, text)
  ui.Dummy(w, math.max(h or 0, th))
end

-- A muted caption on its own line, above a field.
function M.Caption(text)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, text)
end

function M.Text(text, color)
  local c = color or C.text
  ui.TextColored(c[1], c[2], c[3], c[4] or 1, text or "")
end

-- A hover help line for the item just drawn.
function M.Tip(text)
  if text and text ~= "" and ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then ui.SetTooltip(text) end
end

-- A search box of the script's own (it filters what is shown; the game is not asked). Returns the lowered needle.
local searches = {}
function M.Search(key, hint, width)
  ui.SetNextItemWidth(width or -1)
  local v = ui.InputTextWithHint("##search" .. key, hint or "Filter", searches[key] or "", 64)
  searches[key] = v
  return v:lower()
end

-- The line height of the body font and a button's height.
function M.Heights(s)
  local _, lh = ui.CalcTextSize("Ag")
  return lh, lh + 10 * s
end

-- ---------------------------------------------------------------- rows
local cache = {}

local function readRow(win, id, r)
  local texts, cr, cg, cb, tex, u0, v0, u1, v1 = API.windows.ListRow(win, id, r)
  if not texts then return nil end
  return { texts = texts, color = cr and { cr, cg, cb, 1 } or nil, tex = tex, uv = { u0, v0, u1, v1 }, index = r }
end

local function same(a, b)
  if not a or not b then return a == b end
  if (a.color and a.color[1]) ~= (b.color and b.color[1]) or #a.texts ~= #b.texts then return false end
  for i, t in ipairs(a.texts) do if b.texts[i] ~= t then return false end end
  return true
end

-- A row read again: kept (brought up to date in place) when its texts and colour are the same, so a list read a few
-- rows a frame does not make a row's tables each time; true when it changed (a new row was made).
local function reread(c, win, id, r)
  local texts, cr, cg, cb, tex, u0, v0, u1, v1 = API.windows.ListRow(win, id, r)
  local old = c.rows[r + 1]
  if old and texts and (old.color and old.color[1]) == cr and #old.texts == #texts then
    local ot = old.texts
    local equal = true
    for i = 1, #texts do if ot[i] ~= texts[i] then equal = false; break end end
    if equal then
      -- (the parts the comparison leaves out, as a new row would have them)
      old.texts, old.tex = texts, tex
      local uv = old.uv
      uv[1], uv[2], uv[3], uv[4] = u0, v0, u1, v1
      if old.color then old.color[2], old.color[3] = cg, cb end
      return false
    end
  end
  c.rows[r + 1] = texts and
    { texts = texts, color = cr and { cr, cg, cb, 1 } or nil, tex = tex, uv = { u0, v0, u1, v1 }, index = r } or nil
  return not (old == nil and texts == nil)
end

-- The list's rows (the copy; see the header), its selected row (0-based, -1 none), the column labels and a
-- version that changes with the rows.
function M.Rows(win, id)
  local n, _, selected, labels = API.windows.ListInfo(win, id)
  if not n then return {}, -1, {}, 0 end
  local byId = cache[win]
  if not byId then byId = {}; cache[win] = byId end
  local c = byId[id]
  -- read whole: a new count, asked for, or the first or last row changed (a new search with as many results)
  local whole = not c or c.n ~= n or c.dirty
  if not whole and n > 0 then
    whole = not same(c.rows[1], readRow(win, id, 0)) or not same(c.rows[n], readRow(win, id, n - 1))
  end
  if whole then
    c = { n = n, rows = {}, next = 0, version = ((c and c.version) or 0) + 1 }
    for r = 0, n - 1 do c.rows[r + 1] = readRow(win, id, r) end
    byId[id] = c
  elseif n > 0 then
    -- a few rows a frame (about n * per <= 400 row reads): short lists whole, long ones within a few seconds
    for _ = 1, math.max(1, math.min(n, 400 // n)) do
      local r = c.next
      if reread(c, win, id, r) then c.version = c.version + 1 end
      c.next = (r + 1) % n
    end
  end
  return c.rows, selected or -1, labels or {}, c.version
end

-- Read the list again whole next frame (after a click that changes it).
function M.Refresh(win, id) local c = cache[win] and cache[win][id]; if c then c.dirty = true end end

-- ---------------------------------------------------------------- tables
-- M.Table(win, id, w, h, cols, opts): the list child as a table.
--   cols = { { label, width (design px; 0 stretch), cell = function(row, ctx), sort = function(row) -> number|string,
--              align = "right", tip = header tooltip, default = "asc"|"desc" (the first sort) } ... }
--     row = { texts = {...}, color, tex, uv, index (0-based in the original) }; without `cell` the column draws
--     texts[col.text]; without `sort` it sorts on that text.
--   opts.filter(row) -> bool     leave rows out
--   opts.empty = "text"          shown when no row is (opts.emptyFiltered when the filter hid them all)
--   opts.icon = true             rows are icon-tall (an item icon in a cell)
--   opts.tooltip(row)            a hover tooltip for the row
--   opts.doubleClick(row), opts.rightClick(row) (default: the original's right click)
--   opts.key                     the sort's key (several tables of one list)
-- Returns the number of rows shown, the selected row (0-based in the original, -1 none) and its row.
local sorts = {}
local orders = {}
function M.Table(win, id, w, h, cols, opts)
  opts = opts or {}
  local key = opts.key or (win .. "." .. id)
  local rows, selected, _, version = M.Rows(win, id)
  local s = ui.GetIO().uiScale or 1
  local _, textH = ui.CalcTextSize("Ag")
  local rowH = opts.icon and math.floor(textH + 6 * s) or textH
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingFixedFit
      | ui.TableFlags.BordersInnerV | ui.TableFlags.Resizable | ui.TableFlags.Sortable
  if not ui.BeginTable("##mt" .. key, #cols, flags, w or 0, h or 0) then return 0, selected end
  ui.TableSetupScrollFreeze(0, 1)
  for i, col in ipairs(cols) do
    local cw = col.width or col[2] or 0
    local f = cw > 0 and ui.TableColumnFlags.WidthFixed or ui.TableColumnFlags.WidthStretch
    if col.default then f = f | ui.TableColumnFlags.DefaultSort end
    if col.default == "desc" or col.desc then f = f | ui.TableColumnFlags.PreferSortDescending end
    if col.nosort then f = f | ui.TableColumnFlags.NoSort end
    ui.TableSetupColumn((col[1] or "") .. "##c" .. i, f, cw * s)
  end
  ui.TableHeadersRow()
  local scol, sasc = ui.TableGetSortSpecs()
  if scol then sorts[key] = { scol + 1, sasc } end

  -- the order rows are shown in (sorted and filtered; again only when the rows, the sort or the filter changed)
  local st = sorts[key]
  local o = orders[key]
  local fsig = opts.filterKey or ""
  if not o or o.version ~= version or o.n ~= #rows or o.sort ~= st or o.fsig ~= fsig
      or (opts.filter and opts.filterKey == nil) then
    local order = {}
    for _, r in ipairs(rows) do
      if not opts.filter or opts.filter(r) then order[#order + 1] = r end
    end
    if st then
      local col, asc = cols[st[1]], st[2]
      local by = col and (col.sort or function(r) return r.texts[col.text or st[1]] or "" end)
      if by then
        local keys = {}
        for _, r in ipairs(order) do
          local k = by(r)
          if type(k) == "string" then
            local n = tonumber((k:gsub(",", "")))
            k = n or k:lower()
          end
          keys[r] = k or ""
        end
        table.sort(order, function(a, b)
          local x, y = keys[a], keys[b]
          if type(x) ~= type(y) then x, y = type(x) == "number" and 0 or 1, type(y) == "number" and 0 or 1 end
          if x ~= y then if asc then return x < y else return x > y end end
          return a.index < b.index
        end)
      end
    end
    o = { order = order, version = version, n = #rows, sort = st, fsig = fsig }
    orders[key] = o
  end
  local order = o.order
  local selRow = rows[selected + 1]

  local lineH = rowH + 4 -- the selectable plus 2 px of cell padding a side
  local first = math.max(0, math.floor(ui.GetScrollY() / lineH) - 1)
  local _, view = ui.GetWindowSize()
  local last = math.min(#order - 1, first + math.ceil(view / lineH) + 2)
  if first > 0 then ui.TableNextRow(); ui.TableNextColumn(); ui.Dummy(1, first * lineH - 4) end
  for i = first, last do
    local r = order[i + 1]
    ui.TableNextRow()
    ui.TableNextColumn()
    ui.PushID(r.index)
    local column = ui.TableGetHoveredColumn()
    local picked = ui.Selectable("##row", r.index == selected,
      ui.SelectableFlags.SpanAllColumns | ui.SelectableFlags.AllowDoubleClick, 0, rowH)
    if opts.drag and T.DragFrom(win, id, r.index + 1) then picked = false end
    if picked then
      API.windows.ListClick(win, id, r.index, "left", column)
      if opts.onClick then opts.onClick(r) end
      if opts.doubleClick and ui.IsMouseDoubleClicked(ui.Mouse.Left) then opts.doubleClick(r) end
    end
    if ui.IsItemHovered() and opts.tooltip then
      local tip = opts.tooltip(r)
      if tip and tip ~= "" then ui.SetTooltip(tip) end
    end
    if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      if opts.rightClick then opts.rightClick(r) else API.windows.ListClick(win, id, r.index, "right", column) end
      T.TakeRightClick()
    end
    ui.SameLine(0, 0)
    for k, col in ipairs(cols) do
      if k > 1 then ui.TableNextColumn() end
      local x, y = ui.GetCursorScreenPos()
      local cw = ui.GetContentRegionAvail()
      if col.cell then
        col.cell(r, { x = x, y = y, w = cw, h = rowH, s = s, textH = textH })
      else
        local t = r.texts[col.text or k] or ""
        local c = r.color or C.text
        local tx = x
        if col.align == "right" then tx = x + cw - ui.CalcTextSize(t) end
        ui.DrawText(tx, y + (rowH - textH) / 2, c[1], c[2], c[3], 1, t)
      end
    end
    ui.PopID()
  end
  if last < #order - 1 then ui.TableNextRow(); ui.TableNextColumn(); ui.Dummy(1, (#order - 1 - last) * lineH - 4) end
  ui.EndTable()
  if #order == 0 and (opts.empty or opts.emptyFiltered) then
    -- the message over the empty table's body
    local x0, y0, x1, y1 = ui.GetItemRect()
    local msg = (#rows > 0 and opts.emptyFiltered) or opts.empty or ""
    local tw = ui.CalcTextSize(msg)
    local cy = y0 + (y1 - y0) / 2
    ui.DrawText(x0 + math.max(4, (x1 - x0 - tw) / 2), cy, C.faint[1], C.faint[2], C.faint[3], 1, msg)
  end
  return #order, selected, selRow
end

-- Cells for M.Table.
-- An item icon (the row's) and its name (texts[n]) in the row's colour.
function M.IconName(n, color)
  return function(r, c)
    local x = c.x
    if r.tex then
      local sz = c.h
      T.Icon(x, c.y, sz, c.s, r.tex, r.uv[1], r.uv[2], r.uv[3], r.uv[4], nil, { radius = 3 })
      x = x + sz + 6 * c.s
    end
    local col = color or r.color or C.text
    local t = r.texts[n] or ""
    ui.DrawText(x, c.y + (c.h - c.textH) / 2, col[1], col[2], col[3], 1, T.Fit(t, c.x + c.w - x, nil))
  end
end

-- A number (texts[n]) right-aligned, muted when zero.
function M.Number(n, colorOf)
  return function(r, c)
    local t = r.texts[n] or ""
    local col = (colorOf and colorOf(r)) or ((t == "0" or t == "") and C.faint or C.text)
    local tw = ui.CalcTextSize(t)
    ui.DrawText(c.x + c.w - tw - 4 * c.s, c.y + (c.h - c.textH) / 2, col[1], col[2], col[3], 1, t)
  end
end

-- A price (copperOf(row)) as coins, right-aligned. colorOf(row): one colour for the numbers.
function M.PriceCell(copperOf, colorOf)
  return function(r, c)
    local cp = copperOf(r)
    if not cp then return end
    local pw = M.PriceWidth(cp)
    ui.SetCursorScreenPos(c.x + math.max(0, c.w - pw - 4 * c.s), c.y + (c.h - c.textH) / 2)
    M.Price(cp, { color = colorOf and colorOf(r) or nil })
  end
end

-- Plain text (texts[n]) in a colour (default the row's).
function M.TextCell(n, color)
  return function(r, c)
    local col = color or r.color or C.text
    ui.DrawText(c.x, c.y + (c.h - c.textH) / 2, col[1], col[2], col[3], 1, T.Fit(r.texts[n] or "", c.w, nil))
  end
end

-- ---------------------------------------------------------------- mercenary costs
-- A mercenary's purchase and upkeep (the merchant's and the info window's PurchaseCost* / UpkeepCost* children:
-- platinum and gold, or an alternate currency's amount as well) as two captioned lines of coins.
function M.MercCosts(win, s)
  local function line(caption, kind)
    local pp = tonumber(API.windows.Text(win, kind .. "CostPlatText") or "") or 0
    local gp = tonumber(API.windows.Text(win, kind .. "CostGoldText") or "") or 0
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, caption)
    ui.SameLine(80 * (s or 1))
    M.Price(pp * 1000 + gp * 100)
    if API.windows.ChildShown(win, "Alt" .. kind .. "CostText") ~= false then
      ui.SameLine()
      ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "or")
      ui.SameLine()
      ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, API.windows.Text(win, "Alt" .. kind .. "CostText") or "")
      M.Tip("An alternate currency")
    end
  end
  line("Purchase", "Purchase")
  M.Tip("Paid once, when hired")
  line("Upkeep", "Upkeep")
  M.Tip("Paid at each upkeep interval while the mercenary is active")
end

-- ---------------------------------------------------------------- testing
-- test.open.<screen>=1 in ui/settings/modern.txt shows the window once in game (hs.ShowWindow), for screenshots of
-- windows a test cannot reach (a mercenary window without a mercenary).
-- The setting is read the first time (the windows ask each frame): set it before the UI starts.
local testOpened = {}
function M.TestOpen(win)
  if testOpened[win] then return end
  testOpened[win] = true
  if ui.GetSetting("test.open." .. win) == "1" then API.windows.Show(win, true) end
end

-- test.draw.<screen>=1: draw the window though the game has not opened it (hud.Dialog's opts.shown), for windows
-- only a particular NPC or player opens; its children answer as for a closed window (empty lists). nil otherwise.
-- Read the first time, as test.open.
local forced = {} -- [win] = true, or false when not set
function M.Forced(win)
  local f = forced[win]
  if f == nil then
    f = ui.GetSetting("test.draw." .. win) == "1"
    forced[win] = f
  end
  return f or nil
end

-- An STML child's text, or a faint line when it has none.
function M.StmlOr(win, id, empty)
  if (API.windows.Text(win, id) or "") == "" then
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, empty)
  else
    W.Stml(win, id, 0, 0)
  end
end

-- ---------------------------------------------------------------- panels
-- A child region with a faint inset background and hairline (a column of a window). Returns what ui.BeginChild
-- does. (A bordered child, for its padding.)
function M.BeginPanel(id, w, h, s)
  ui.PushStyleColor(ui.Col.ChildBg, C.ink[1], C.ink[2], C.ink[3], 0.025)
  ui.PushStyleColor(ui.Col.Border, C.ink[1], C.ink[2], C.ink[3], 0.06)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 8 * s, 6 * s)
  ui.PushStyleVar(ui.StyleVar.ChildRounding, 6 * s)
  local v = ui.BeginChild(id, w, h, true, ui.WindowFlags.NoScrollbar)
  ui.PopStyleVar(2)
  ui.PopStyleColor(2)
  return v
end
function M.EndPanel() ui.EndChild() end

return M
