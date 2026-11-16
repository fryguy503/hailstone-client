-- Pieces the gamepad pack's market and service windows share (bazaar, barter, point and mercenary merchants, the
-- guild bank, training, tracking): a stock list as W.list rows, stock buttons, a stock window's edit boxes and
-- drop-downs as buttons (A types the text on the on-screen keyboard, or picks a choice from a list drawn in the
-- window's place), check buttons as On / Off buttons, coin buttons and prices. Every control is the stock window's
-- child: hs.ListClick, hs.ClickChild, hs.SetWindowText, hs.ComboSelect, so the rules and messages are the game's.
--
--   local M = dofile("lib/markets.lua")
--   local F = M.form()                       -- a window's keyboard and picker
--   M.frame()                                -- once a frame, first (runs clicks waiting for A to be released)
--   if not M.modal(F, x0, y0, x1, y1) then ... M.field(F, win, child, ...) ... end
--   if M.back(F, W.finish()) then close end
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local C = T.C
local M = {}

-- ---------------------------------------------------------------- money
M.COINS = { { "pp", C.platinum }, { "gp", C.gold }, { "sp", C.silver }, { "cp", C.copper } }

-- copper -> { pp, gp, sp, cp }
function M.split(copper)
  copper = math.max(0, math.floor(tonumber(copper) or 0))
  return { copper // 1000, copper % 1000 // 100, copper % 100 // 10, copper % 10 }
end

-- "12p 3g 5c" (the game's compact coin text) -> copper; nil when it is not one.
function M.parse(text)
  if not text or text == "" then return nil end
  local total, any = 0, false
  for n, unit in text:gmatch("(%d+)%s*([pgsc])") do
    any = true
    local v = tonumber(n) or 0
    total = total + (unit == "p" and v * 1000 or unit == "g" and v * 100 or unit == "s" and v * 10 or v)
  end
  return any and total or nil
end

-- Four list texts (platinum .. copper) from texts[first] -> copper.
function M.coinsOf(texts, first)
  local cp = 0
  for i = 0, 3 do cp = cp + (tonumber(texts[first + i] or "") or 0) * (10 ^ (3 - i)) end
  return math.floor(cp)
end

-- A price as coloured coins ending at x1 (align 1) or starting at x (align 0), vertically in y0 .. y1; returns its
-- width. Zero coins are left out ("0cp" for nothing).
function M.price(copper, x, y0, y1, align)
  local parts = M.split(copper)
  local pieces = {}
  for i = 1, 4 do
    if parts[i] ~= 0 or (i == 4 and #pieces == 0) then pieces[#pieces + 1] = { tostring(parts[i]), M.COINS[i] } end
  end
  local w = 0
  for k, p in ipairs(pieces) do
    w = w + ui.CalcTextSize(p[1]) + ui.CalcTextSize(p[2][1]) + (k > 1 and T.px(10) or 0)
  end
  local cx = align == 1 and x - w or x
  for k, p in ipairs(pieces) do
    if k > 1 then cx = cx + T.px(10) end
    cx = cx + T.textIn(cx, y0, cx + 400, y1, p[1], C.text, 0)
    cx = cx + T.textIn(cx, y0, cx + 400, y1, p[2][1], p[2][2], 0)
  end
  return w
end

-- ---------------------------------------------------------------- clicks after A
-- A click that opens the quantity dialog runs once A is released, so the A still held does not press the dialog.
-- Dropped if the window it was asked in closes first (W.deferred).
local waiting = {}
function M.afterA(fn) W.deferred(waiting, fn) end
function M.frame()
  T.frame()
  W.runDeferred(waiting)
end

-- A pad shortcut of the current window, not in its first moments (the press that opened it is not one for it).
function M.pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 10 and W.pressed(b)
end

-- ---------------------------------------------------------------- rows
-- A copy of a list child's rows: each frame a list is read it is built again (hs keeps it for the rest of that
-- frame) and each row read makes a table, so a long list (a bazaar search's results) is read whole only when its count or its first or last row changes (or after M.refresh), and
-- else a few rows a frame.
local cache = {}
local function readRow(win, id, r)
  local texts, cr, cg, cb, tex, u0, v0, u1, v1 = API.windows.ListRow(win, id, r)
  if not texts then return nil end
  return { texts = texts, color = cr and { cr, cg, cb, 1 } or nil, tex = tex, uv = { u0, v0, u1, v1 }, index = r }
end
-- Whether row r still is what `a` (a row read before) holds: the texts and the colour's red, as readRow would read
-- it (without making the row's tables).
local function sameAt(a, win, id, r)
  local texts, cr = API.windows.ListRow(win, id, r)
  if not a or not texts then return a == nil and texts == nil end
  if (a.color and a.color[1]) ~= (cr or nil) or #a.texts ~= #texts then return false end
  for i, t in ipairs(a.texts) do if texts[i] ~= t then return false end end
  return true
end

-- The rows ({ texts, color, tex, uv, index (0-based) }) and the selected row (0-based, -1 none).
function M.rows(win, id)
  local key = win .. "." .. id
  local n, _, selected = API.windows.ListInfo(win, id)
  if not n then return {}, -1 end
  local c = cache[key]
  local whole = not c or c.n ~= n or c.dirty
  -- a short list is read whole each frame below anyway: its first and last rows need no check
  local per = n > 0 and math.max(1, math.min(n, 400 // n)) or 0
  if not whole and n > 0 and per < n then
    whole = not sameAt(c.rows[1], win, id, 0) or not sameAt(c.rows[n], win, id, n - 1)
  end
  if whole then
    c = { n = n, rows = {}, next = 0 }
    for r = 0, n - 1 do c.rows[r + 1] = readRow(win, id, r) end
    cache[key] = c
  elseif n > 0 then
    for _ = 1, per do
      local r = c.next
      c.rows[r + 1] = readRow(win, id, r)
      c.next = (r + 1) % n
    end
  end
  return c.rows, selected or -1
end
function M.refresh(win, id) local c = cache[win .. "." .. id]; if c then c.dirty = true end end

-- A stock list as W.list rows in x0, y0 .. x1, y1. opts: rowH (design px, 60), draw(row, x0, y0, x1, y1, focused,
-- selected), filter(row), select = "focus" (focusing a row selects it in the game's list) or "a" (default: A does),
-- onA(row) (after the selection; e.g. Buy), inspect (false: Y does not right-click the row), empty (text).
-- state = {} kept by the caller. Returns the focused row, the selected row and the rows shown.
function M.list(win, id, state, x0, y0, x1, y1, opts)
  opts = opts or {}
  local rows, selected = M.rows(win, id)
  local order = rows
  if opts.filter then
    order = {}
    for _, r in ipairs(rows) do if opts.filter(r) then order[#order + 1] = r end end
  end
  local rowH = opts.rowH or 60
  local visible = math.max(1, math.floor((y1 - y0 + T.px(10)) / T.px(rowH + 10)))
  state.shown = visible
  state.selected = nil
  for i, r in ipairs(order) do if r.index == selected then state.selected = i end end
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y0 - wy)
  local focusedRow
  ui.BeginGroup() -- (the rows start at this x)
  W.list(win .. id, state, #order, visible, rowH, x1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
    local r = order[i]
    if not r then return end
    opts.draw(r, rx0, ry0, rx1, ry1, focused, r.index == selected)
    if focused then
      focusedRow = r
      if opts.select == "focus" and state.lastFocus ~= r.index then
        state.lastFocus = r.index
        if r.index ~= selected then API.windows.ListClick(win, id, r.index, "left") end
      end
      if opts.inspect ~= false and W.pressed("y") then API.windows.ListClick(win, id, r.index, "right") end
    end
  end, function(i)
    local r = order[i]
    if not r then return end
    if r.index ~= selected or opts.select ~= "focus" then API.windows.ListClick(win, id, r.index, "left") end
    if opts.onA then opts.onA(r) end
  end)
  ui.EndGroup()
  if #order == 0 and opts.empty then
    T.font(20)
    T.draw(x0 + T.px(14), y0 + T.px(16), opts.empty, C.faint)
    T.unfont()
  end
  return focusedRow, rows[selected + 1], #order
end

-- A row: the row's icon (when it has one), `name` in the row's colour, `right` (text, or a function(x1, y0, y1)
-- that draws it) at the right; `sub` a dim second line under the name.
function M.row(r, x0, y0, x1, y1, name, right, opts)
  opts = opts or {}
  local x = x0 + T.px(14)
  local s = y1 - y0 - T.px(14)
  if r.tex and opts.icon ~= false then
    T.icon(r.tex, r.uv[1], r.uv[2], r.uv[3], r.uv[4], x0 + T.px(8), y0 + T.px(7), x0 + T.px(8) + s, y0 + T.px(7) + s)
    x = x0 + s + T.px(20)
  end
  local rw = opts.rightW or T.px(260)
  if not right then rw = 0 end
  T.font(opts.size or 21)
  local col = opts.color or r.color or C.text
  if opts.sub and opts.sub ~= "" then
    local _, th = ui.CalcTextSize("Ag")
    local mid = (y0 + y1) / 2
    T.textIn(x, mid - th, x1 - rw - T.px(14), mid, T.fit(name or "", x1 - rw - x - T.px(14)), col, 0)
    T.unfont()
    T.font(17)
    T.textIn(x, mid, x1 - rw - T.px(14), mid + th, T.fit(opts.sub, x1 - rw - x - T.px(14)), C.dim, 0)
  else
    T.textIn(x, y0, x1 - rw - T.px(14), y1, T.fit(name or "", x1 - rw - x - T.px(14)), col, 0)
  end
  T.unfont()
  if type(right) == "function" then
    T.font(20)
    right(x1 - T.px(16), y0, y1)
    T.unfont()
  elseif right and right ~= "" then
    T.font(19)
    T.textIn(x1 - rw, y0, x1 - T.px(16), y1, T.fit(right, rw - T.px(16)), opts.rightColor or C.dim, 1)
    T.unfont()
  end
end

-- A grid of `n` cells (`per` across, `rows` shown) at the cursor from screen x: cell(i, k) draws cell i (1-based)
-- as the k-th focusable item and returns whether it has the focus. D-pad down on the last shown line (or up on the
-- first) moves the shown lines, as W.list does. state = {} kept by the caller.
function M.grid(state, n, per, rows, x, gap, cell)
  state.first = state.first or 0
  local lines = math.ceil(n / per)
  state.first = math.max(0, math.min(state.first, lines - rows))
  local wx, wy = ui.GetWindowPos()
  local y = select(2, ui.GetCursorScreenPos())
  local moved = false
  local from = state.first * per + 1
  local to = math.min(n, (state.first + rows) * per)
  for i = from, to do
    local k = i - from
    if k % per == 0 then
      ui.SetCursorPos(x - wx, y - wy)
    else
      ui.SameLine(0, gap)
    end
    if state.focusNext == i then
      ui.SetKeyboardFocusHere()
      pad.showCursor()
      state.focusNext = nil
    end
    local focused = cell(i, k)
    if k % per == per - 1 or i == to then y = select(2, ui.GetCursorScreenPos()) end
    if focused and not moved then
      local line = (i - 1) // per
      if line == state.first + rows - 1 and line < lines - 1 and pad.menuPressed("down") then
        state.first = state.first + 1
        state.focusNext = math.min(n, i + per)
        moved = true
      elseif line == state.first and line > 0 and pad.menuPressed("up") then
        state.first = state.first - 1
        state.focusNext = i - per
        moved = true
      end
    end
  end
  return lines > rows
end

-- ---------------------------------------------------------------- buttons
local function shown(win, child) return API.windows.ChildShown(win, child) ~= false end
M.shown = shown
local function enabled(win, child) return API.windows.ChildState(win, child) ~= false end
M.enabled = enabled

-- A stock button w x h at the cursor (left out when the game hides it): A clicks it (opts.afterA: once A is
-- released, for a button that opens the quantity dialog). opts: as T.button (primary, sub, glyph), label = nil
-- takes the button's own text. Returns clicked, focused.
function M.button(win, child, label, w, h, opts)
  opts = opts or {}
  local text = label
  if not text or opts.useText then
    local t = API.windows.Text(win, child)
    if t and t ~= "" then text = t end
  end
  local o = { primary = opts.primary, sub = opts.sub, glyph = opts.glyph, align = opts.align,
    disabled = opts.disabled or not enabled(win, child) }
  local clicked, focused = T.button("##" .. win .. child, text or child, w, h, o, pad)
  if clicked then
    if opts.afterA then M.afterA(function() API.windows.ClickChild(win, child, "left") end)
    else API.windows.ClickChild(win, child, "left") end
  end
  return clicked, focused
end

-- A row of stock buttons from x0 to x1 at the cursor's line, h tall: list = { { child, label, opts } }; the ones
-- the game hides are left out and the rest share the width.
function M.buttons(win, list, x0, x1, h)
  local wx, wy = ui.GetWindowPos()
  local show = {}
  for _, b in ipairs(list) do if b.always or shown(win, b[1]) then show[#show + 1] = b end end
  if #show == 0 then return end
  local gap = T.px(10)
  local bw = (x1 - x0 - gap * (#show - 1)) / #show
  ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
  for i, b in ipairs(show) do
    if i > 1 then ui.SameLine(0, gap) end
    M.button(win, b[1], b[2], bw, h, b[3])
  end
end

-- A check button as a button: its label and On / Off under it (opts.on / opts.off the words). Returns clicked.
function M.toggle(win, child, label, w, h, opts)
  opts = opts or {}
  local _, checked = API.windows.ChildState(win, child)
  local clicked = T.button("##" .. win .. child, label, w, h, { sub = checked and (opts.on or "On") or (opts.off or "Off"),
    primary = checked and not opts.plain, align = 0, disabled = not enabled(win, child) }, pad)
  if clicked then API.windows.ClickChild(win, child, "left") end
  return clicked
end

-- Four coin buttons (the stock window's money buttons ids[1..4], platinum .. copper; their text the amount) from x0
-- to x1 at the cursor's line: A opens the game's amount dialog (once A is released).
function M.coinButtons(win, ids, x0, x1, h)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
  local gap = T.px(8)
  local bw = (x1 - x0 - 3 * gap) / 4
  for i, id in ipairs(ids) do
    if i > 1 then ui.SameLine(0, gap) end
    local clicked, _, bx0, by0, bx1, by1 = T.button("##" .. win .. id, "", bw, h, { disabled = not enabled(win, id) }, pad)
    T.font(22)
    local v = tostring(tonumber(API.windows.Text(win, id) or "") or 0)
    T.textIn(bx0 + T.px(8), by0, bx1 - T.px(8), by0 + (by1 - by0) * 0.62, T.fit(v, bx1 - bx0 - T.px(16)), C.text, 0.5)
    T.unfont()
    T.font(15)
    T.textIn(bx0, by0 + (by1 - by0) * 0.55, bx1, by1 - T.px(4), M.COINS[i][1], M.COINS[i][2], 0.5)
    T.unfont()
    if clicked then M.afterA(function() API.windows.ClickChild(win, id, "left") end) end
  end
end

-- Tabs from x0 at y0 (the mouse can click them; LB / RB is the window's); returns the y under them.
function M.tabs(labels, page, x0, y0, set)
  local x = x0
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  local h = th + T.px(16)
  for i, l in ipairs(labels) do
    local w = ui.CalcTextSize(l) + T.px(40)
    T.rect(x, y0, x + w, y0 + h, page == i and C.accent or C.raised, page == i and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + h, l, page == i and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + h) and ui.IsMouseClicked(ui.Mouse.Left) then set(i) end
    x = x + w + T.px(10)
  end
  T.unfont()
  return y0 + h + T.px(16)
end

-- Label / value pairs across x0 .. x1 at y: { { label, value, colour } }.
function M.facts(list, x0, x1, y)
  local w = (x1 - x0) / #list
  for i, f in ipairs(list) do
    local fx = x0 + (i - 1) * w
    T.font(17)
    T.draw(fx, y, f[1]:upper(), C.dim)
    local _, lh = ui.CalcTextSize("Ag")
    T.unfont()
    T.font(24)
    T.draw(fx, y + lh + T.px(2), T.fit(f[2] or "", w - T.px(10)), f[3] or C.text)
    T.unfont()
  end
end

-- A small heading at x, y (screen).
function M.caption(x, y, text)
  T.font(17)
  T.draw(x, y, text:upper(), C.dim)
  T.unfont()
end

-- A state chip (a word in a coloured pill) at x, y; returns its width.
function M.chip(x, y, text, c)
  T.font(19)
  local tw, th = ui.CalcTextSize(text)
  local w, h = tw + T.px(28), th + T.px(10)
  T.rect(x, y, x + w, y + h, c, 0.18, h / 2)
  T.outline(x, y, x + w, y + h, c, 0.7, h / 2, T.px(2))
  T.textIn(x, y, x + w, y + h, text, c, 0.5)
  T.unfont()
  return w, h
end

-- ---------------------------------------------------------------- fields (the keyboard and the picker)
function M.form() return { kb = K.new() } end

-- Whether the keyboard or a picker is up.
function M.busy(F) return F.kb.open or F.pick ~= nil end

-- The hints for a window: the keyboard's while it is up, the picker's, else `normal`.
function M.hints(F, normal)
  if F.kb.open then return K.HINTS end
  if F.pick then return { { "a", "Choose" }, { "b", "Cancel" } } end
  return normal
end

local function focusBack(F, id)
  if F.refocus == id then
    ui.SetKeyboardFocusHere()
    pad.showCursor()
    F.refocus = nil
  end
end

-- An edit box of the stock window as a button w x h at the cursor: its label and text (opts.hint when empty); A
-- types it on the keyboard (opts.max characters), Done sets it (hs.SetWindowText) and calls opts.done.
function M.field(F, win, child, label, w, h, opts)
  opts = opts or {}
  local id = "##f" .. win .. child
  focusBack(F, id)
  local text = API.windows.Text(win, child) or ""
  local clicked, focused = T.button(id, label, w, h, { sub = text ~= "" and text or (opts.hint or "-"), align = 0,
    disabled = not enabled(win, child) }, pad)
  if clicked then
    K.open(F.kb, opts.prompt or label, text, opts.max or 64, opts.capital)
    F.target = { win = win, child = child, done = opts.done, id = id }
  end
  return clicked, focused
end

-- A drop-down of the stock window as a button w x h at the cursor: its label and choice; A shows its choices.
function M.combo(F, win, child, label, w, h)
  local id = "##c" .. win .. child
  focusBack(F, id)
  local choices, chosen = API.windows.ComboInfo(win, child)
  choices = choices or {}
  local clicked, focused = T.button(id, label, w, h, { sub = choices[(chosen or 0) + 1] or "-", align = 0,
    disabled = #choices == 0 or not enabled(win, child) }, pad)
  if clicked then
    F.pick = { win = win, child = child, label = label, choices = choices, chosen = chosen or 0, id = id,
      state = { offset = 0 }, at = ui.GetFrameCount() }
  end
  return clicked, focused
end

-- Draws the keyboard or the picker in x0, y0 .. x1, y1 in the window's place; true while one is up (the window
-- draws nothing else then).
function M.modal(F, x0, y0, x1, y1)
  if F.kb.open then
    local t = F.target
    local w = math.min(x1 - x0, T.px(1200))
    local r = K.draw(F.kb, pad, x0 + (x1 - x0 - w) / 2, y0, w)
    if r == "ok" and t then
      API.windows.SetText(t.win, t.child, F.kb.text)
      if t.done then t.done(F.kb.text) end
    end
    if r and t then F.refocus = t.id end
    return true
  end
  local p = F.pick
  if not p then return false end
  T.font(24)
  local _, th = ui.CalcTextSize("Ag")
  T.draw(x0, y0, p.label, C.text)
  T.unfont()
  local ly = y0 + th + T.px(16)
  local n = #p.choices
  local cols = n > 12 and 3 or (n > 6 and 2 or 1)
  local gap = T.px(12)
  local bw = (x1 - x0 - gap * (cols - 1)) / cols
  local bh = T.px(60)
  local perCol = math.max(1, math.floor((y1 - ly + gap) / (bh + gap)))
  local fits = perCol * cols
  -- a long list pages: the window of choices around the current one
  local first = 0
  if n > fits then first = math.max(0, math.min(n - fits, p.chosen - fits // 2)) end
  if p.first then first = p.first end
  p.first = first
  local wx, wy = ui.GetWindowPos()
  local done = false
  for k = 0, math.min(fits, n - first) - 1 do
    local i = first + k
    local col, rowN = k // perCol, k % perCol
    ui.SetCursorPos(x0 + col * (bw + gap) - wx, ly + rowN * (bh + gap) - wy)
    if not p.focused and i == p.chosen then
      ui.SetKeyboardFocusHere()
      pad.showCursor()
      p.focused = true
    elseif p.focusIndex == i then
      ui.SetKeyboardFocusHere()
      pad.showCursor()
      p.focusIndex = nil
    end
    local current = i == p.chosen
    local clicked, focused = T.button("##pick" .. i, p.choices[i + 1] or "", bw, bh,
      { primary = current, align = 0 }, pad)
    if focused then
      -- past the shown ones: the next page
      if k == math.min(fits, n - first) - 1 and i < n - 1 and pad.menuPressed("down") then
        p.first = math.min(n - fits, first + perCol)
        p.focusIndex = math.min(n - 1, i + 1)
      elseif k == 0 and i > 0 and pad.menuPressed("up") then
        p.first = math.max(0, first - perCol)
        p.focusIndex = i - 1
      end
    end
    if clicked and ui.GetFrameCount() - p.at > 10 then
      if not current then API.windows.ComboSelect(p.win, p.child, i) end
      done = true
    end
  end
  if ui.IsMouseHoveringRect(x0, ly, x1, y1) and n > fits then
    local wheel = ui.GetIO().wheel or 0
    if wheel ~= 0 then p.first = math.max(0, math.min(n - fits, first - math.floor(wheel * 3 + 0.5))) end
  end
  if done then
    F.refocus = p.id
    F.pick = nil
  end
  return true
end

-- W.finish's answer: B (or the close button) takes the keyboard or the picker down first; true when the window
-- itself is to close.
function M.back(F, close)
  if not close then return false end
  if F.kb.open then
    F.kb.open = false
    F.refocus = F.target and F.target.id
    return false
  end
  if F.pick then
    F.refocus = F.pick.id
    F.pick = nil
    return false
  end
  return true
end

-- ---------------------------------------------------------------- testing
-- The testing settings below, read once a frame for every window (GPSTATE) and split again only when one changes:
-- every closed market window asks each frame.
local function testSettings()
  local t, frame = GPSTATE.marketsTest, ui.GetFrameCount()
  if t and t.frame == frame then return t end
  if not t then
    t = {}
    GPSTATE.marketsTest = t
  end
  t.frame = frame
  local show, draw = ui.GetSetting("test.show") or "", ui.GetSetting("test.draw") or ""
  if t.showRaw ~= show then
    t.showRaw, t.show = show, {}
    for w in show:gmatch("[^,]+") do t.show[w] = true end
  end
  if t.drawRaw ~= draw then
    t.drawRaw, t.draw, t.drawSet = draw, {}, {}
    for w in draw:gmatch("[^,]+") do
      t.draw[#t.draw + 1] = w
      t.drawSet[w] = true
    end
  end
  return t
end

-- test.show = <screen>,<screen> in the pack's settings file (ui/settings/gamepad.txt) shows those stock windows once,
-- ten seconds in (hs.ShowWindow), for screenshots of windows only an NPC or a server opens; their children then
-- answer as for an empty window.
function M.testShow(screen, t)
  t = t or testSettings()
  if t.showRaw == "" or not API.session.InGame() then return end
  GPSTATE.marketsShown = GPSTATE.marketsShown or {}
  if GPSTATE.marketsShown[screen] then return end
  if not t.show[screen] then return end
  GPSTATE.marketsShowAt = GPSTATE.marketsShowAt or ui.GetTime()
  if ui.GetTime() - GPSTATE.marketsShowAt > 10 then
    GPSTATE.marketsShown[screen] = true
    API.windows.Show(screen, true)
  end
end

-- test.draw = <screen>,... draws the pack's window for those stock windows though the game has not opened them (a
-- trainer's, a merchant's: only an NPC opens them); their children answer as for a closed window (empty lists).
-- With test.draw.every = <seconds> they are drawn one at a time in turn, from ten seconds in (logged).
local function forced(screen, t)
  if t.drawRaw == "" then -- (no screen is drawn: with test.draw.every, list[i] is never one)
    return false
  end
  if not API.session.InGame() then return false end
  local list = t.draw
  local every = tonumber(ui.GetSetting("test.draw.every") or "")
  if not every then return t.drawSet[screen] == true end
  GPSTATE.testDrawAt = GPSTATE.testDrawAt or ui.GetTime()
  local tt = ui.GetTime() - GPSTATE.testDrawAt - 10
  if tt < 0 then return false end
  local i = math.floor(tt / every) + 1
  if list[i] ~= screen then return false end
  if GPSTATE.testDrawLogged ~= i then
    GPSTATE.testDrawLogged = i
    ui.Log("gamepad ui: test.draw " .. screen)
  end
  return true
end

-- W.shown for these windows, with the testing settings above.
function M.visible(screen)
  local t = testSettings()
  M.testShow(screen, t)
  local shown = W.shown(screen)
  return shown or forced(screen, t)
end

return M
