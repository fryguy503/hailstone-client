-- A stock window's children as this pack's widgets, for the windows the pack draws over a C++ window model (the
-- adventure, journal, help windows ...): each widget asks the original child what it shows
-- (hs.WindowText, hs.ChildState, hs.ListInfo / hs.ListRow, hs.ComboInfo) and hands the pad's presses and the mouse's
-- clicks to it (hs.ClickChild, hs.ListClick, hs.ComboSelect), as the classic pack's lib/sidl.lua does for the stock
-- layouts, so what happens is the game's.
--
--   local M = dofile("lib/model.lua")
--   M.button(WIN, "OKButton", "OK", w, h, { primary = true })      -- greyed while the original is disabled
--   M.toggle(WIN, "AutoButton", "Show at login", w, h)              -- a check box: On / Off under its label
--   M.combo(WIN, "Chooser", "Category", w, h, pick)                 -- A opens a list of its choices (M.picker)
--   M.list(WIN, "List", state, x0, y0, x1, y1, opts)                -- the rows in columns, A clicks one
--   if M.picker(pick, W.area()) then ... (the choices are up: draw nothing else) end
--   if M.back(W.finish(), pick) then hs.ShowWindow(WIN, false) end -- B takes the choices down first
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local C = T.C
local M = {}

-- ---------------------------------------------------------------- reading the original
-- The child's text, "" when it has none.
function M.text(win, id) return (API.windows.Text(win, id)) or "" end
-- Whether the window shows the child (true when it says nothing).
function M.shown(win, id) return API.windows.ChildShown(win, id) ~= false end
-- enabled, checked (enabled true and checked nil when the child has no state of its own).
function M.state(win, id)
  local e, c = API.windows.ChildState(win, id)
  if e == nil then return true, nil end
  return e ~= false, c
end

-- The rows of a list child as { texts, ... } (short lists: every row is read), and the selected one (1-based or nil).
-- limit: read only the first that many rows (a window that wants the first row).
function M.rows(win, id, limit)
  local rows, _, selected = API.windows.ListInfo(win, id)
  local out = {}
  local last = (rows or 0) - 1
  if limit then last = math.min(last, limit - 1) end
  for r = 0, last do out[#out + 1] = API.windows.ListRow(win, id, r) or {} end
  return out, (selected and selected >= 0) and selected + 1 or nil
end

-- ---------------------------------------------------------------- buttons
-- A button for the child at the cursor: greyed and inert while the original is disabled, accent while it is checked
-- (a toggle) or opts.primary. The press goes to the game; returns true when it did. opts: primary, glyph, sub,
-- align, disabled (also off), label (else the child's text).
function M.button(win, id, label, w, h, opts)
  opts = opts or {}
  local enabled, checked = M.state(win, id)
  label = label or M.text(win, id)
  local on = enabled and not opts.disabled
  if T.button("##" .. win .. "." .. id, label, w, h,
        { primary = opts.primary or checked == true, disabled = not on, glyph = opts.glyph, sub = opts.sub,
          align = opts.align }, pad) then
    API.windows.ClickChild(win, id, "left")
    return true
  end
  return false
end

-- A check box child as a button: its label, "On" / "Off" under it, accent while checked; A toggles the original.
function M.toggle(win, id, label, w, h, opts)
  opts = opts or {}
  local enabled, checked = M.state(win, id)
  if T.button("##" .. win .. "." .. id, label, w, h,
        { primary = checked == true, disabled = not enabled, sub = checked and (opts.on or "On") or (opts.off or "Off"),
          align = opts.align or 0 }, pad) then
    API.windows.ClickChild(win, id, "left")
    return true
  end
  return false
end

-- A row of child buttons from x0 to x1 at the cursor's line, sharing the width: { { id, label [, opts] } }; the hidden
-- ones are left out. Returns the id pressed.
function M.buttons(win, list, x0, x1, h)
  local shown = {}
  for _, b in ipairs(list) do if M.shown(win, b[1]) then shown[#shown + 1] = b end end
  if #shown == 0 then return nil end
  local wx, wy = ui.GetWindowPos()
  local gap = T.px(10)
  local bw = (x1 - x0 - gap * (#shown - 1)) / #shown
  ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
  local hit
  for i, b in ipairs(shown) do
    if i > 1 then ui.SameLine(0, gap) end
    if M.button(win, b[1], b[2], bw, h, b[3]) then hit = b[1] end
  end
  return hit
end

-- ---------------------------------------------------------------- combo boxes
-- A combo child as a button: its caption, the choice under it; A opens the choices (pick, a table the window keeps:
-- M.picker draws them). Returns nothing.
function M.combo(win, id, label, w, h, pick, opts)
  opts = opts or {}
  local choices, selected = API.windows.ComboInfo(win, id)
  local enabled = API.windows.ChildState(win, id) ~= false and choices ~= nil and #choices > 0
  local current = choices and choices[(selected or -1) + 1] or ""
  if pick.refocus == win .. "." .. id then -- the choices just went down: the focus comes back here
    pick.refocus = nil
    ui.SetKeyboardFocusHere()
    pad.showCursor()
  end
  if T.button("##" .. win .. "." .. id, label, w, h,
        { sub = current ~= "" and current or (opts.none or "-"), disabled = not enabled, align = 0 }, pad) then
    pick.open = { win = win, id = id, label = label, focus = true }
    pick.list = { offset = math.max(0, (selected or 0) - 2) }
  end
end

-- The open combo's choices in the window's content area x0, y0 .. x1, y1 (W.area), in place of its body: the window
-- draws nothing else while this returns true. A picks the focused choice (hs.ComboSelect), B (M.back) goes back
-- without one.
function M.picker(pick, x0, y0, x1, y1)
  local o = pick.open
  if not o then return false end
  local choices, selected = API.windows.ComboInfo(o.win, o.id)
  if not choices then
    pick.open = nil
    return false
  end
  local cw = x1 - x0
  local wx, wy = ui.GetWindowPos()
  T.font(22)
  T.draw(x0, y0, o.label, C.dim)
  local _, th = ui.CalcTextSize("Ag")
  T.unfont()
  local ly = y0 + th + T.px(12)
  local w = math.min(cw, T.px(900))
  local lx = x0 + (cw - w) / 2
  ui.SetCursorPos(lx - wx, ly - wy)
  local st = pick.list
  st.selected = (selected or -1) + 1
  if o.focus then
    -- the focus starts on the chosen row
    local v = math.max(1, st.selected - st.offset)
    st.focusNext = v
    o.focus = false
  end
  local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / T.px(60)))
  st.shown = visible
  ui.BeginGroup() -- (the rows start at this x)
  W.list("pick" .. o.id, st, #choices, visible, 50, w, function(i, rx0, ry0, rx1, ry1, focused)
    T.font(21)
    T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(18), ry1, T.fit(choices[i], rx1 - rx0 - T.px(36)),
      i == st.selected and C.accent or C.text, 0)
    T.unfont()
  end, function(i)
    if i - 1 ~= selected then API.windows.ComboSelect(o.win, o.id, i - 1) end
    pick.open = nil
    pick.refocus = o.win .. "." .. o.id
  end)
  ui.EndGroup()
  return true
end

-- W.finish's answer for a window with a picker (and a W.confirm question in `state`, a keyboard `kb`): B takes them
-- down first; true when the window itself is to close.
function M.back(close, pick, state, kb, list)
  if close and pick and pick.open then
    pick.refocus = pick.open.win .. "." .. pick.open.id
    pick.open = nil
    return false
  end
  return W.back(close, state, kb, list)
end

-- ---------------------------------------------------------------- lists
-- The columns' x ranges from x0 to x1 for the list's widths (design px; 0 stretches), with `only` (column numbers) to
-- show some of them.
local function columnsOf(widths, columns, x0, x1, only)
  local cols = {}
  if only then cols = only else for c = 1, math.max(columns, 1) do cols[#cols + 1] = c end end
  local fixed, stretch = 0, 0
  for _, c in ipairs(cols) do
    local wpx = (widths[c] or 0)
    if wpx > 0 then fixed = fixed + wpx else stretch = stretch + 1 end
  end
  local total = x1 - x0
  -- the stock widths scaled to the row; stretched columns share what they leave (at least a fifth of the row)
  local scale = total / math.max(1, fixed + (stretch > 0 and math.max(total / T.S * 0.2, 120) * stretch or 0))
  local out, x = {}, x0
  for _, c in ipairs(cols) do
    local wpx = (widths[c] or 0)
    local w = wpx > 0 and wpx * scale or (total - fixed * scale) / math.max(1, stretch)
    out[#out + 1] = { c, x, x + w }
    x = x + w
  end
  return out
end

-- A list child in x0, y0 .. x1, y1 (screen): a header line with the column names (a click on one is the original's
-- header click: sorting), then the rows (W.list), each column's text in the row's colour. A (or a click) clicks the
-- row in the original; the selected row (or rows, in a list that selects several) is marked.
-- state = {} kept by the window. opts:
--   rowH          row height (design px, default 54)
--   only          the columns to show ({ 1, 3 })
--   labels        column names in place of the original's ({ [n] = "Name" })
--   focusSelects  focusing a row clicks it (its details show as the pad moves)
--   multi         X toggles the focused row in the selection (the original's ctrl click)
--   activate(i)   Y on a row / a double click (i 1-based), e.g. the original's double click
--   empty         the text shown without rows
--   rowText(i, texts) -> texts   change what a row shows
-- Returns the focused row (1-based) or nil, and the row count.
local rowColor = {} -- (a row's colour, used while the row is drawn)
function M.list(win, id, state, x0, y0, x1, y1, opts)
  opts = opts or {}
  local rows, columns, selected, labels, widths, multi = API.windows.ListInfo(win, id)
  rows = rows or 0
  labels, widths = labels or {}, widths or {}
  local wx, wy = ui.GetWindowPos()
  local cols = columnsOf(widths, columns or 1, x0 + T.px(18), x1 - T.px(30), opts.only)
  local header = false
  for _, c in ipairs(cols) do
    local l = (opts.labels and opts.labels[c[1]]) or labels[c[1]] or ""
    if l ~= "" then header = true end
  end
  local top = y0
  if header then
    T.font(16)
    local _, th = ui.CalcTextSize("Ag")
    for _, c in ipairs(cols) do
      local l = (opts.labels and opts.labels[c[1]]) or labels[c[1]] or ""
      T.textIn(c[2], y0, c[3] - T.px(8), y0 + th, T.fit(l:upper(), c[3] - c[2] - T.px(8)), C.dim, 0)
      if l ~= "" and ui.IsMouseHoveringRect(c[2], y0, c[3], y0 + th) and ui.IsMouseClicked(ui.Mouse.Left) then
        API.windows.ListHeaderClick(win, id, c[1] - 1)
      end
    end
    T.unfont()
    top = y0 + th + T.px(8)
  end
  ui.SetCursorPos(x0 - wx, top - wy)
  if rows == 0 then
    T.draw(x0, top + T.px(6), opts.empty or "Nothing here.", C.faint)
    ui.Dummy(1, 1)
    return nil, 0
  end
  local rowH = opts.rowH or 54
  local visible = math.max(1, math.floor((y1 - top + T.px(10)) / T.px(rowH + 10)))
  state.shown = visible
  state.selected = (selected and selected >= 0) and selected + 1 or nil
  ui.BeginGroup() -- (the rows start at this x)
  local focusedRow = W.list(win .. id, state, rows, visible, rowH, x1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
    local texts, cr, cg, cb = API.windows.ListRow(win, id, i - 1)
    if not texts then return end
    if opts.rowText then texts = opts.rowText(i, texts) or texts end
    if multi and multi[i - 1] then T.rect(rx0, ry0, rx1, ry1, C.accent, 0.22, T.px(10)) end
    local base = C.text
    if cr then
      rowColor[1], rowColor[2], rowColor[3], rowColor[4] = cr, cg, cb, 1
      base = rowColor
    end
    -- (the stock lists' white text is the palette's text: legible on a light palette)
    if cr and cr > 0.95 and cg > 0.95 and cb > 0.95 then base = C.text end
    T.font(opts.size or 20)
    for k, c in ipairs(cols) do
      local t = texts[c[1]] or ""
      local cc = (texts.colors and texts.colors[c[1]]) or base
      if cc and cc[1] and cc[1] > 0.95 and cc[2] > 0.95 and cc[3] > 0.95 then cc = C.text end
      T.textIn(c[2], ry0, c[3] - T.px(8), ry1, T.fit(t, c[3] - c[2] - T.px(8)), k == 1 and cc or (cc == C.text and C.dim or cc), 0)
    end
    T.unfont()
    if focused then
      if opts.focusSelects and state.lastFocus ~= i then
        state.lastFocus = i
        if state.selected ~= i then API.windows.ListClick(win, id, i - 1, "left") end
      end
      if opts.multi and pad.menuPressed("x") then API.windows.ListClick(win, id, i - 1, "left", nil, 2) end
      if opts.activate and pad.menuPressed("y") then opts.activate(i) end
    end
    if opts.activate and ui.IsMouseHoveringRect(rx0, ry0, rx1, ry1) and ui.IsMouseDoubleClicked(ui.Mouse.Left) then
      opts.activate(i)
    end
  end, function(i) API.windows.ListClick(win, id, i - 1, "left") end)
  ui.EndGroup()
  return focusedRow, rows
end

-- ---------------------------------------------------------------- labels
-- Caption / value pairs in a row from x0 to x1 at y (the tribute window's facts): { { caption, value [, colour] } }.
-- Returns the bottom.
function M.facts(list, x0, x1, y)
  if #list == 0 then return y end
  local w = (x1 - x0) / #list
  local bottom = y
  for i, f in ipairs(list) do
    local fx = x0 + (i - 1) * w
    T.font(16)
    local _, lh = ui.CalcTextSize("Ag")
    T.draw(fx, y, T.fit(f[1]:upper(), w - T.px(10)), C.dim)
    T.unfont()
    T.font(f.size or 26)
    local _, vh = ui.CalcTextSize("Ag")
    local v = f[2]
    if v == nil or v == "" then v = "-" end
    T.draw(fx, y + lh + T.px(2), T.fit(v, w - T.px(10)), f[3] or C.text)
    T.unfont()
    bottom = math.max(bottom, y + lh + T.px(2) + vh)
  end
  return bottom
end

-- Opens a stock window once, `after` seconds in the world, when the pack's settings file has test.show=<screen>,...
-- (testing: screenshots of windows only the game opens, docs/gamepad.md).
local testShown, testAt = false, nil
function M.testShow()
  if testShown or not API.session.InGame() then return end
  local list = ui.GetSetting("test.show") or ""
  if list == "" then
    testShown = true
    return
  end
  testAt = testAt or ui.GetTime()
  if ui.GetTime() - testAt < tonumber(ui.GetSetting("test.show.after") or "6") then return end
  testShown = true
  for s in list:gmatch("[^,]+") do API.windows.Show(s, true) end
end

return M
