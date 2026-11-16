-- Pieces the character-progress windows share (aa, train, leadership, tradeskill, title, tracking, the skill
-- pickers): a list child of the game window drawn as rows of our own (P.Rows), a search box, an empty state, small
-- stat tiles. Like lib/widgets.lua they read the original child and hand clicks to it (hs.ListClick, hs.PickUp), so
-- the window's own handler runs.
--
--   local P = dofile("lib/progress.lua")
--   P.Rows("TitleWnd", "Title_AvailableTitles", 0, -40, { rowH = 22, filter = function(row, texts) ... end })
local API = hs.UI
local T = dofile("lib/theme.lua")
local C = T.color

local P = {}

-- ---------------------------------------------------------------- testing
-- ui/settings/modern.txt test.open.<screen>=1: `open(...)` runs once, 6 s after zoning in (or =<n>: n s after), for
-- screenshots of windows the game opens only from the world (a tradeskill bag, a right click on the Actions window).
-- Without `open` the window `screen` is shown (hs.ShowWindow(screen, true)). Arguments after `open` are passed to it,
-- so a caller drawing every frame need not make a closure: P.TestOpen(WINDOW, hs.ClickChild, "ActionsWindow", "X",
-- "right"). Once the setting is found missing the call returns at once.
modern_progress_test = modern_progress_test or { done = {}, since = nil }
local test = modern_progress_test
test.noSelect = test.noSelect or {} -- [win][list] = true: no test.select setting for that list
function P.TestOpen(screen, open, ...)
  if test.done[screen] then return end
  if not API.session.InGame() then test.since = nil return end
  local delay = tonumber(ui.GetSetting("test.open." .. screen) or "")
  if not delay then test.done[screen] = true return end
  test.since = test.since or ui.GetTime()
  if ui.GetTime() - test.since < math.max(delay, 6) then return end
  test.done[screen] = true
  if open then open(...) else API.windows.Show(screen, true) end
end

-- ---------------------------------------------------------------- small pieces
local function rgb(c, a) return c[1], c[2], c[3], a or c[4] or 1 end

-- A search field across the width (or `width`); returns the text. The hint shows while it is empty.
function P.Search(id, text, hint, width)
  ui.SetNextItemWidth(width or -1)
  return ui.InputTextWithHint(id, hint or "Search", text or "", 64)
end

-- A message centred in a w x h area at the cursor (an empty list): a line and an optional quieter one below.
function P.Empty(w, h, text, sub)
  local x, y = ui.GetCursorScreenPos()
  local tw, th = T.TextSize(text, T.font.small)
  local cy = y + math.max(0, h / 2 - th)
  T.Text(x + w / 2, cy, text, C.muted, T.font.small, "center")
  if sub and sub ~= "" then
    T.Text(x + w / 2, cy + th + 4, sub, C.faint, T.font.tiny, "center")
  end
  ui.Dummy(math.max(w, 1), math.max(h, 1))
end

-- A stat tile: a caption above a value, w x h screen pixels at (x, y). opts: color, font, tooltip (a hit box).
function P.Tile(id, x, y, w, h, s, caption, value, opts)
  opts = opts or {}
  ui.DrawRectFilled(x, y, x + w, y + h, C.ink[1], C.ink[2], C.ink[3], 0.04, 6 * s)
  local _, ch = T.TextSize("Ag", T.font.tiny)
  local font = opts.font or T.font.normal
  local _, vh = T.TextSize("Ag", font)
  local gap = 2 * s
  local top = y + (h - ch - vh - gap) / 2
  T.Text(x + w / 2, top, caption, C.faint, T.font.tiny, "center")
  T.Text(x + w / 2, top + ch + gap, value, opts.color or C.text, font, "center")
  if opts.tooltip then
    T.Hit(id, x, y, x + w, y + h)
    if ui.IsItemHovered() then ui.SetTooltip(opts.tooltip) end
  end
end

-- A rounded label chip at (x, y); returns its width.
function P.Chip(x, y, s, text, color, font)
  font = font or T.font.tiny
  local tw, th = T.TextSize(text, font)
  local w, h = tw + 10 * s, th + 4 * s
  ui.DrawRectFilled(x, y, x + w, y + h, color[1], color[2], color[3], 0.16, h / 2)
  T.Text(x + 5 * s, y + 2 * s, text, color, font)
  return w, h
end

-- The selected-row look P.Rows uses, for rows drawn elsewhere.
function P.RowBack(x0, y0, x1, y1, s, selected, hovered)
  if selected then
    ui.DrawRectFilled(x0, y0, x1, y1, C.accent[1], C.accent[2], C.accent[3], 0.16, 5 * s)
    ui.DrawRectFilled(x0, y0 + 3 * s, x0 + 2.5 * s, y1 - 3 * s, rgb(C.accent))
  elseif hovered then
    ui.DrawRectFilled(x0, y0, x1, y1, C.ink[1], C.ink[2], C.ink[3], 0.05, 5 * s)
  end
end

-- ---------------------------------------------------------------- rows over a list child
-- P.Rows(win, id, w, h, opts): the list child's rows (a copy of their texts, kept as below) as a
-- scrolled column of rows drawn by opts.draw, only those in view (each asks hs.ListRow for its colour). A click
-- selects the row in the original (hs.ListClick, so the window's handler runs), a right click is the original's,
-- a drag picks up the hot button the row makes where the original lets it (opts.drag).
--   w, h            the area (0: the rest; negative h: leave that much below)
--   opts.rows       the list's rows when the caller has them already (hs.WindowList's answer this frame; not kept)
--   opts.rowH       a row's height in design pixels (default: a line of small text and padding)
--   opts.scale      the containing dialog's effective scale (default: the global UI scale)
--   opts.filter(row, texts) -> bool   leaves rows out (row 0-based)
--   opts.filterKey   any value that changes when the filter's answers would (the search text): the filtered rows
--                    are kept while it, the rows and the sort stay the same, instead of filtered again each frame
--   opts.sort(a, b) -> bool           orders rows (each { row, texts }); the same function from frame to frame
--                                     sorts again only when the rows shown change
--   opts.draw(row, texts, x0, y0, x1, y1, s, state)   state = { selected, hovered, color = { r, g, b }, colors (the
--                                     row's cell colours, hs.ListRow's texts.colors, or nil) }; default: the first
--                                     column's text. (state and its tables are reused: read them in the call.)
--   opts.drag        a drag picks the row up (hs.PickUp)
--   opts.empty, opts.emptySub         the text shown when no row is left
--   opts.doubleClick(row)             a double click on a row (after its click)
--   opts.tooltip(row, texts)          a tooltip for a hovered row
--   opts.reserve     unscaled pixels at each row's right that its click and drag leave alone (a part drawn there
--                                     takes its own clicks)
-- Sets opts.innerX and opts.innerW to this frame's row bounds (less the scroll bar), for column heads drawn after it.
-- Returns the original's row count, the selected row (0-based, -1 none) and how many rows are shown.
local rowPools = {} -- [win][id] = { entries = { { row, texts } ... }, order = {}, state = {...} } (reused each frame)
local function rowPool(win, id)
  local byId = rowPools[win]
  if not byId then byId = {}; rowPools[win] = byId end
  local p = byId[id]
  if not p then
    p = { entries = {}, order = {}, cand = {}, state = { color = { 1, 1, 1, 1 } } }
    byId[id] = p
  end
  return p
end

-- testing: test.select.<window>.<list>=<row> selects that row once
local function testSelect(win, id, count)
  local none = test.noSelect[win]
  if none and none[id] then return end
  local pick = not test.done[win .. "." .. id] and tonumber(ui.GetSetting("test.select." .. win .. "." .. id) or "")
  if pick and count <= pick then return end -- (not yet listed)
  if pick then
    test.done[win .. "." .. id] = true
    API.windows.ListClick(win, id, pick, "left")
  end
  -- (none set, or done: do not ask again)
  if not none then none = {}; test.noSelect[win] = none end
  none[id] = true
end

-- The list's rows are kept as a copy (hs.WindowList reads and makes every row's texts, and the sort ran over them,
-- each frame): read whole when the row count, the first or the last row changes (or after a click on the list),
-- else a few rows a frame, and the rows in view each frame, as lib/widgets.lua's sorted lists do. The rows shown are
-- filtered again each frame over the copy (opts.filterKey, any value that changes when the filter would, keeps the
-- filtered rows instead), and sorted again only when the copy, the filter's rows or the sort changes.
local ROW_READS = 8 -- rows read a frame besides the first, the last and those in view

local function sameTexts(a, b)
  if not a or not b then return (not a) == (not b) end
  local n = #a
  if n ~= #b then return false end
  for i = 1, n do if a[i] ~= b[i] then return false end end
  return true
end

local function rowCopy(win, id, pool, n)
  local c = pool.copy
  if not c then
    c = { n = -1, rows = {}, next = 0, version = 0 }
    pool.copy = c
  end
  local whole = c.n ~= n or c.dirty
  if not whole and n > ROW_READS then -- (a shorter list is read whole below anyway)
    whole = not sameTexts(c.rows[1], API.windows.ListRow(win, id, 0) or false)
      or not sameTexts(c.rows[n], API.windows.ListRow(win, id, n - 1) or false)
  end
  if whole then
    local rows = c.rows
    for r = 0, n - 1 do rows[r + 1] = API.windows.ListRow(win, id, r) or false end
    for r = #rows, n + 1, -1 do rows[r] = nil end
    c.n, c.next, c.dirty, c.version = n, 0, false, c.version + 1
  elseif n > 0 then
    for _ = 1, math.min(n, ROW_READS) do
      local r = c.next
      local texts = API.windows.ListRow(win, id, r) or false
      if not sameTexts(c.rows[r + 1], texts) then
        c.rows[r + 1] = texts
        c.version = c.version + 1
      end
      c.next = (r + 1) % n
    end
  end
  return c
end

-- The rows shown, in pool.order (entries { row, texts }): rebuilt only when they would come out different.
local function rowOrder(pool, c, opts, given)
  local filter, fkey, sort = opts.filter, opts.filterKey, opts.sort
  local kept = not given and pool.version == c.version and pool.sort == sort and pool.hadFilter == (filter ~= nil)
  if kept and fkey ~= nil and pool.filterKey == fkey then return end
  -- the rows the filter keeps (1-based), compared with last frame's
  local cand = pool.cand
  local k = 0
  local rows = c.rows
  for r = 1, c.n do
    local texts = rows[r]
    if texts and (not filter or filter(r - 1, texts)) then
      k = k + 1
      if cand[k] ~= r then cand[k] = r; kept = false end
    end
  end
  if #cand ~= k then
    for i = #cand, k + 1, -1 do cand[i] = nil end
    kept = false
  end
  pool.filterKey = fkey
  if kept then return end
  pool.version, pool.sort, pool.hadFilter = c.version, sort, filter ~= nil
  local entries, order = pool.entries, pool.order
  for i = 1, k do
    local e = entries[i]
    if not e then e = {}; entries[i] = e end
    local r = cand[i]
    e.row, e.texts = r - 1, rows[r]
    order[i] = e
  end
  for i = #order, k + 1, -1 do order[i] = nil end
  for i = k + 1, #entries do entries[i].texts = nil end -- (let go of rows no longer listed)
  if sort then table.sort(order, sort) end
end

local NO_OPTS = {}
function P.Rows(win, id, w, h, opts)
  opts = opts or NO_OPTS
  local s = opts.scale or ui.GetIO().uiScale or 1
  local count, _, selected = API.windows.ListInfo(win, id)
  selected = selected or -1
  local pool = rowPool(win, id)
  local c
  if opts.rows then
    c = { n = #opts.rows, rows = opts.rows, version = -1 }
  else
    c = rowCopy(win, id, pool, count or 0)
  end
  local total = c.n
  testSelect(win, id, total)
  rowOrder(pool, c, opts, opts.rows ~= nil)
  local order = pool.order
  local _, th = T.TextSize("Ag", T.font.small)
  local rowH = opts.rowH and opts.rowH * s or (th + 10 * s)
  ui.BeginChild("##rows" .. id, w or 0, h or 0, false)
  local cw, ch = ui.GetContentRegionAvail()
  local ox, oy = ui.GetCursorScreenPos()
  if opts ~= NO_OPTS then opts.innerX, opts.innerW = ox, cw end
  if #order == 0 then
    P.Empty(cw, math.max(ch, 40 * s), opts.empty or "Nothing to show.", opts.emptySub)
    ui.EndChild()
    return total, selected, 0
  end
  local scroll = ui.GetScrollY()
  local first = math.max(1, math.floor(scroll / rowH))
  local last = math.min(#order, first + math.ceil(ch / rowH) + 1)
  local io = ui.GetIO()
  local state = pool.state
  local color = state.color
  for k = first, last do
    local e = order[k]
    local y0 = oy + (k - 1) * rowH
    local x1 = ox + cw
    local clicked = T.Hit("##r" .. e.row, ox, y0, x1 - (opts.reserve or 0) * s, y0 + rowH - 1 * s)
    local hovered = ui.IsItemHovered()
    if opts.drag and T.DragFrom(win, id, e.row + 1) then clicked = false end
    if clicked then
      API.windows.ListClick(win, id, e.row, "left")
      if opts.doubleClick and ui.IsMouseDoubleClicked(ui.Mouse.Left) then opts.doubleClick(e.row) end
    end
    if ui.IsItemClicked(ui.Mouse.Right) and not io.alt then
      API.windows.ListClick(win, id, e.row, "right")
      T.TakeRightClick()
      clicked = true
    end
    if clicked and pool.copy == c then c.dirty = true end -- (a click may rebuild the list: read it whole next frame)
    -- a row in view is drawn from what the list answers now, and brings the copy up to date
    local full, r, g, b = API.windows.ListRow(win, id, e.row)
    if full and pool.copy == c and not sameTexts(c.rows[e.row + 1], full) then
      c.rows[e.row + 1] = full
      c.version = c.version + 1
    end
    local texts = full or e.texts
    if hovered and opts.tooltip then
      local tip = opts.tooltip(e.row, texts)
      if tip and tip ~= "" then ui.SetTooltip(tip) end
    end
    state.selected, state.hovered, state.colors = e.row == selected, hovered, full and full.colors or nil
    color[1], color[2], color[3], color[4] = r or 1, g or 1, b or 1, 1
    P.RowBack(ox, y0, x1, y0 + rowH - 1 * s, s, state.selected, hovered)
    if opts.draw then
      opts.draw(e.row, texts, ox, y0, x1, y0 + rowH - 1 * s, s, state)
    else
      T.Text(ox + 8 * s, y0 + (rowH - th) / 2, T.Fit(texts[1] or "", cw - 16 * s, T.font.small), state.color,
        T.font.small)
    end
  end
  state.colors = nil
  -- the whole column's height, so the child scrolls over every row
  ui.SetCursorScreenPos(ox, oy)
  ui.Dummy(cw, #order * rowH)
  ui.EndChild()
  return total, selected, #order
end

return P
