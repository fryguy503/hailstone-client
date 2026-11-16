-- Pad controls bound to a stock window's children (by ScreenID), for the gamepad windows that stand in for a stock
-- window child by child (inspect, leadership, group search, the address book ...). Each asks the original child what
-- it shows (hs.WindowText, hs.ChildState, hs.ComboInfo, hs.ListInfo ...) and hands the press to it (hs.ClickChild,
-- hs.ComboSelect, hs.SetWindowText, hs.ListClick ...), so the window's own handler runs and the mechanics are the
-- game's. Every control is a focusable item at the layout cursor (A presses it, the mouse clicks it):
--
--   local CH = dofile("lib/child.lua")
--   local st = CH.new()                                   -- the window's keyboard and picker, kept by the window
--   if W.begin(...) then
--     if not CH.modal(st, x0, y0, x1, y1) then            -- the keyboard or a picker takes the body while up
--       CH.button(WIN, "DoneButton", "Done", w, h)
--       CH.field(st, WIN, "NameField", "Name", w, h)     -- A: the on-screen keyboard types the edit box's text
--       CH.combo(st, WIN, "TimeZoneCombo", "Time zone", w, h)   -- A: a list of the choices
--     end
--   end
--   if CH.back(st, W.finish()) then hs.ShowWindow(WIN, false) end
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local C = T.C
local CH = {}

function CH.new() return { kb = K.new(), pick = nil, edit = nil, list = {} } end

-- The control a keyboard or picker was opened from takes the pad's focus back when it closes.
local function refocus(st, key)
  if st.refocus == key and not st.kb.open and not st.pick then
    ui.SetKeyboardFocusHere()
    pad.showCursor()
    st.refocus = nil
  end
end

function CH.shown(win, id) return API.windows.ChildShown(win, id) ~= false end
function CH.enabled(win, id) return API.windows.ChildState(win, id) ~= false end
function CH.checked(win, id) return select(2, API.windows.ChildState(win, id)) == true end
function CH.text(win, id) return API.windows.Text(win, id) or "" end
-- STML / a label's text as plain text (no tags).
function CH.plain(win, id)
  local s = CH.text(win, id):gsub("<[Bb][Rr]%s*/?>", " "):gsub("<[^>]*>", "")
  return (s:gsub("&nbsp;", " "):gsub("&amp;", "&"))
end

-- The button for a child: greyed while the original is disabled, nothing at all while it is hidden (opts.keep draws
-- it greyed instead). opts as T.button's (primary, sub, glyph, align). Returns true when the click went to the game.
function CH.button(win, id, label, w, h, opts)
  opts = opts or T.NO_OPTS
  local shown = CH.shown(win, id)
  if not shown and not opts.keep then return false end
  if T.button("##" .. win .. id, label or CH.text(win, id), w, h, opts, pad,
        opts.disabled or not shown or not CH.enabled(win, id)) then
    API.windows.ClickChild(win, id, "left")
    return true
  end
  return false
end

-- A check box child as a toggle: the label, "On" / "Off" (or opts.on / opts.off) under it, lit while checked.
function CH.toggle(win, id, label, w, h, opts)
  opts = opts or {}
  if not CH.shown(win, id) and not opts.keep then return false end
  local on = CH.checked(win, id)
  local o = { sub = on and (opts.on or "On") or (opts.off or "Off"), align = opts.align or 0, primary = on,
    disabled = not CH.enabled(win, id) }
  if T.button("##" .. win .. id, label, w, h, o, pad) then
    API.windows.ClickChild(win, id, "left")
    return true
  end
  return false
end

-- A text field: its caption and text; A opens the keyboard on it, Done gives set(text) what was typed. opts: max
-- (characters), capital (shift on first), prompt, disabled, keys (extra keys under the keyboard: { label, text }),
-- multiline (Done keeps line breaks typed with the keys' "\n" key). off: disabled as well (CH.field's child).
function CH.edit(st, id, caption, text, set, w, h, opts, off)
  opts = opts or T.NO_OPTS
  text = text or ""
  local enabled = not (opts.disabled or off)
  local shown = text ~= "" and text:gsub("\n", "  ") or "(empty)"
  refocus(st, "edit" .. id)
  local clicked, focused, x0, y0, x1, y1 = T.button("##edit" .. id, "", w, h, T.NO_OPTS, pad, not enabled)
  local a = enabled and 1 or 0.45
  T.font(17)
  local _, ch = ui.CalcTextSize("Ag")
  T.draw(x0 + T.px(14), y0 + T.px(8), caption, focused and C.accent or C.dim, a)
  T.unfont()
  T.font(21)
  ui.PushClipRect(x0 + T.px(14), y0, x1 - T.px(12), y1, true)
  T.draw(x0 + T.px(14), y0 + T.px(10) + ch, T.fit(shown, x1 - x0 - T.px(30)), text ~= "" and C.text or C.faint, a)
  ui.PopClipRect()
  T.unfont()
  if clicked and enabled then
    CH.type(st, opts.prompt or caption, text, set, opts)
    st.from = "edit" .. id
    return true
  end
  return false
end

-- Opens the keyboard with `text` typed; Done gives set(text) what was typed (opts as CH.edit's).
function CH.type(st, prompt, text, set, opts)
  opts = opts or {}
  st.edit = { set = set, keys = opts.keys }
  st.pick, st.from = nil, opts.from
  K.open(st.kb, prompt, text or "", opts.max or 256, opts.capital)
end

-- An edit box child as a field (CH.edit over the edit box's text: Done puts the text in it, as typing it does).
function CH.field(st, win, id, caption, w, h, opts)
  opts = opts or T.NO_OPTS
  if not CH.shown(win, id) and not opts.keep then return false end
  return CH.edit(st, win .. id, caption, CH.text(win, id), function(t) API.windows.SetText(win, id, t) end, w, h, opts,
    opts.disabled or not CH.enabled(win, id))
end

-- A combo box child as a button showing its choice; A opens the list of its choices (A picks one there).
function CH.combo(st, win, id, caption, w, h, opts)
  opts = opts or {}
  if not CH.shown(win, id) and not opts.keep then return false end
  local choices, selected = API.windows.ComboInfo(win, id)
  choices = choices or {}
  local current = choices[(selected or -1) + 1] or "-"
  local enabled = CH.enabled(win, id) and #choices > 0
  refocus(st, "combo" .. win .. id)
  if T.button("##" .. win .. id, caption, w, h, { sub = current, align = 0, disabled = not enabled }, pad) and enabled then
    local sel = math.max(0, selected or 0)
    st.pick = { win = win, id = id, caption = caption, list = { offset = math.max(0, sel - 1) },
      choices = choices, selected = selected, colors = opts.colors, at = ui.GetTime() }
    st.pick.list.focusNext = sel - st.pick.list.offset + 1
    st.edit = nil
    st.from = "combo" .. win .. id
    return true
  end
  return false
end

-- A picker of choices this window makes itself (not a combo child): choices = { text, ... }; onPick(i) (1-based).
-- CH.refocus(st, key) before the button that opened it gives that button the focus back when it closes.
function CH.refocus(st, key) refocus(st, key) end
function CH.choose(st, caption, choices, selected, onPick, from)
  st.pick = { caption = caption, choices = choices, selected = (selected or 1) - 1, onPick = onPick,
    list = { offset = 0, focusNext = 1 }, at = ui.GetTime() }
  st.edit = nil
  st.from = from
end

-- The keyboard or a picker over the window's body (x0..y1, the content area): true while one is up (the window
-- draws nothing else then). The keyboard's Done puts its text in the edit box (or st.edit.set(text)).
function CH.modal(st, x0, y0, x1, y1)
  local wx, wy = ui.GetWindowPos()
  if st.kb.open and st.edit then
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local kw = math.min(x1 - x0, T.px(1200))
    local e = st.edit
    local r = K.draw(st.kb, pad, x0, y0, kw)
    -- extra keys (code: braces, operators ...) in a row under the keyboard
    if e.keys and st.kb.open then
      local n = #e.keys
      local gap = T.px(8)
      local bw = (kw - (n - 1) * gap) / n
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(8) - wy)
      for i, k in ipairs(e.keys) do
        if i > 1 then ui.SameLine(0, gap) end
        if T.button("##xkey" .. i, k[1], bw, T.px(60), {}, pad) and #st.kb.text < st.kb.max then
          st.kb.text = st.kb.text .. k[2]
        end
      end
    end
    if r == "ok" then e.set(st.kb.text) end
    if not st.kb.open then st.edit, st.refocus = nil, st.from end
    return true
  end
  local p = st.pick
  if not p then return false end
  T.font(24)
  T.draw(x0, y0, p.caption, C.text)
  T.unfont()
  local ly = y0 + T.px(46)
  ui.SetCursorPos(x0 - wx, ly - wy)
  local rowH = 60
  local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(rowH) + T.px(10))))
  p.list.selected = (p.selected or -1) + 1
  local picked
  W.list("pick" .. (p.id or p.caption), p.list, #p.choices, visible, rowH, math.min(x1 - x0, T.px(900)),
    function(i, rx0, ry0, rx1, ry1)
      local c = p.colors and p.colors[i] or (i == p.list.selected and C.accent or C.text)
      T.font(22)
      T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(14), ry1, T.fit(p.choices[i], rx1 - rx0 - T.px(32)), c, 0)
      T.unfont()
    end, function(i) if ui.GetTime() - (p.at or 0) > 0.3 then picked = i end end) -- (not the A that opened it)
  if picked then
    st.pick, st.refocus = nil, st.from
    if p.onPick then p.onPick(picked)
    elseif picked - 1 ~= p.selected then API.windows.ComboSelect(p.win, p.id, picked - 1) end
  end
  return true
end

-- W.finish's answer with the keyboard or a picker up: B takes them down first; true when the window is to close.
function CH.back(st, close)
  if not close then return false end
  if st.kb.open then
    st.kb.open, st.edit, st.refocus = false, nil, st.from
    return false
  end
  if st.pick then
    st.pick, st.refocus = nil, st.from
    return false
  end
  return true
end

function CH.busy(st) return st.kb.open or st.pick ~= nil end

-- The window's hints: the keyboard's while it is up, the picker's, else `hints`.
function CH.hints(st, hints)
  if st.kb.open then return K.HINTS end
  if st.pick then return { { "a", "Pick" }, { "b", "Back" } } end
  return hints
end

-- A list child as a W.list: its rows (hs.ListInfo / hs.ListRow, 0-based there), the original's selection, A (or a
-- click) selects the row in the original (hs.ListClick; opts.onA(row, texts) instead). opts: rowH (design px,
-- default 58), columns = { { column (1-based), width fraction [, align] }, ... } (default: the first column),
-- draw(row, texts, x0, y0, x1, y1, focused, rowColor) instead, filter(row, texts), color(row, texts) the text colour (default:
-- the row's colour from the list), empty (a text shown when there are no rows). Returns the focused row (0-based)
-- and its texts.
local DEFAULT_COLUMNS = { { 1, 1 } }
-- Without opts.filter only the rows in view are read (hs.ListRow builds each row's texts), once a frame before they
-- are drawn, into tables the list's state keeps (state.rowBuf: the colours are reused).
local function readRows(win, id, state, rows, filter, visible)
  local buf = state.rowBuf
  if not buf then
    buf = { shown = {}, colors = {} }
    state.rowBuf = buf
  end
  local shown, colors = buf.shown, buf.colors
  local n, first, last = 0, 0, rows - 1
  if not filter then
    -- the rows W.list will draw: its offset clamped as it clamps it
    local off = state.offset or 0
    off = math.max(0, math.min(off, math.max(0, rows - visible)))
    first, last = off, math.min(rows, off + visible) - 1
    n = off -- (rows before the view are counted, not read)
  end
  for r = first, last do
    local texts, cr, cg, cb = API.windows.ListRow(win, id, r)
    texts = texts or {}
    if not filter or filter(r, texts) then
      n = n + 1
      local s = shown[n]
      if not s then
        s = {}
        shown[n] = s
      end
      s.row, s.texts = r, texts
      if cr then
        local c = colors[n]
        if not c then
          c = {}
          colors[n] = c
        end
        c[1], c[2], c[3], c[4] = cr, cg, cb, 1
        s.color = c
      else
        s.color = nil
      end
    end
  end
  if not filter then n = rows end
  return shown, n
end

function CH.list(win, id, state, width, height, opts)
  opts = opts or T.NO_OPTS
  local rows, _, selected = API.windows.ListInfo(win, id)
  rows = rows or 0
  local rowH = opts.rowH or 58
  local visible = math.max(1, math.floor((height + T.px(10)) / (T.px(rowH) + T.px(10))))
  local shown, count = readRows(win, id, state, rows, opts.filter, visible)
  local x0, y0 = ui.GetCursorScreenPos()
  if count == 0 then
    T.font(20)
    T.draw(x0 + T.px(6), y0 + T.px(10), opts.empty or "Nothing here.", C.faint)
    T.unfont()
    ui.Dummy(width, T.px(rowH))
    return nil
  end
  state.shown = visible
  state.selected = nil
  if opts.filter then
    for i = 1, count do if shown[i].row == selected then state.selected = i end end
  elseif selected and selected >= 0 and selected < rows then
    state.selected = selected + 1
  end
  local focusedRow
  local f = W.list(win .. id, state, count, visible, rowH, width, function(i, rx0, ry0, rx1, ry1, focused)
    local s = shown[i]
    if opts.draw then return opts.draw(s.row, s.texts, rx0, ry0, rx1, ry1, focused, s.color) end
    local col = opts.color and opts.color(s.row, s.texts) or s.color or C.text
    if i == state.selected then col = C.accent end
    local cols = opts.columns or DEFAULT_COLUMNS
    local w = rx1 - rx0 - T.px(32)
    local x = rx0 + T.px(18)
    T.font(opts.font or 21)
    for k, c in ipairs(cols) do
      local cw = w * c[2]
      local t = s.texts[c[1]] or ""
      T.textIn(x, ry0, x + cw - T.px(8), ry1, T.fit(t, cw - T.px(8)), k == 1 and col or C.dim, c[3] or 0)
      x = x + cw
    end
    T.unfont()
  end, function(i)
    local s = shown[i]
    if opts.onA then opts.onA(s.row, s.texts) else API.windows.ListClick(win, id, s.row, "left") end
  end)
  if f then focusedRow = shown[f] end
  return focusedRow and focusedRow.row, focusedRow and focusedRow.texts
end

-- A line typed into the main chat window (the game's command interpreter), as who.lua does.
function CH.say(line)
  local windows = API.chat.Windows()
  API.chat.Submit(windows[1] and windows[1].slot or 1, line)
end

-- A caption (dim, small capitals) at x, y; returns its height.
function CH.caption(x, y, text)
  T.font(17)
  local _, th = ui.CalcTextSize("Ag")
  T.draw(x, y, text:upper(), C.dim)
  T.unfont()
  return th
end

-- Testing: the pack's settings file (ui/settings/gamepad.txt) test.show = <ScreenID> shows that stock window once,
-- 10 s after zoning in (hidden first a second before, so it takes the pad as a window opened in play does; opener()
-- instead of hs.ShowWindow where the game opens it another way); test.click.<ScreenID> = child,list:row,tabbox=page,
-- edit~text clicks its children 2 s after it shows (screenshots of the window's states).
-- (Every window asks each frame before it knows it is shown: with test.show empty once in the world, as it is in
-- play, nothing is asked again, as M.testShow and G.testShow do; the list is split once for every window, GPSTATE.)
local testAt, tested = {}, {}
function CH.testOpen(screen, opener)
  if GPSTATE.testOpenOff or not API.session.InGame() then return end
  local list = ui.GetSetting("test.show") or ""
  if list == "" then
    GPSTATE.testOpenOff = true
    return
  end
  local parsed = GPSTATE.testOpenList
  if not parsed or parsed.raw ~= list then
    parsed = { raw = list, names = {} }
    for name in list:gmatch("[^,]+") do parsed.names[name] = true end
    GPSTATE.testOpenList = parsed
  end
  if not parsed.names[screen] or GPSTATE["test_show_" .. screen] then return end
  testAt[screen] = testAt[screen] or ui.GetTime()
  local t = ui.GetTime() - testAt[screen]
  if t > 10 then
    GPSTATE["test_show_" .. screen] = true
    if opener then opener() else API.windows.Show(screen, true) end
  elseif t > 9 and not opener then
    API.windows.Show(screen, false)
  end
end
function CH.testClicks(win)
  if tested[win] == true then return end
  tested[win] = tested[win] or ui.GetTime()
  if ui.GetTime() - tested[win] < 2 then return end
  tested[win] = true
  local list = ui.GetSetting("test.click." .. win)
  if not list or list == "" then return end
  for item in list:gmatch("[^,]+") do
    local id, row = item:match("^(.-):(%d+)$")
    local box, pg = item:match("^(.-)=(%d+)$")
    local edit, text = item:match("^(.-)~(.*)$")
    if edit then API.windows.SetText(win, edit, text)
    elseif box then API.windows.SetTabPage(win, box, tonumber(pg))
    elseif id then API.windows.ListClick(win, id, tonumber(row), "left") else API.windows.ClickChild(win, item, "left") end
  end
end

return CH
