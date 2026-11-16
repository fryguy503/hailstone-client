-- The gamepad pack's pieces over a stock window's children (the dialogs, the readers, the pickers): each draws in
-- the pack's look at the layout cursor and acts through the original child, so the game does what its own button,
-- check box, combo box, list or edit box does.
--
--   local G = dofile("lib/stock.lua")
--   G.button(WIN, "OK", "OK", w, h, { primary = true })     -- the child's click (greyed while it is disabled)
--   G.check(WIN, "NoShowButton", "Don't show tips", w, h)   -- a check box: the box on the right, A toggles it
--   G.combo(WIN, "BugTypes", "Category", w, h)              -- A steps to the next choice (hs.ComboSelect)
--   G.list(WIN, "OptionsList", state, visible, rowH, w, drawRow)  -- a list child as a W.list (hs.ListClick)
--   G.edit(kb, WIN, "Entry", x0, x1, { max = 64 })          -- an edit box typed with the on-screen keys
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local KF = dofile("lib/keyfield.lua")
local C = T.C
local G = { KF = KF }

function G.text(win, id) return API.windows.Text(win, id) or "" end
function G.enabled(win, id) return API.windows.ChildState(win, id) ~= false end
function G.checked(win, id) return select(2, API.windows.ChildState(win, id)) == true end
function G.shown(win, id) return API.windows.ChildShown(win, id) ~= false end

-- STML to plain text (a prompt: colours and links are not needed).
function G.plain(stml)
  local s = (stml or ""):gsub("\r", ""):gsub("<[Bb][Rr]%s*/?>", "\n"):gsub("</?[Pp]>", "\n"):gsub("<[^>]*>", "")
  s = s:gsub("&lt;", "<"):gsub("&gt;", ">"):gsub("&quot;", "\""):gsub("&apos;", "'"):gsub("&nbsp;", " "):gsub("&amp;", "&")
  return s
end

-- Text word-wrapped to `width` px from x, y (at most `maxLines` drawn); returns the bottom (of every line, drawn or
-- not). A line is not measured again for each word: ui.CalcTextSize gives a width rounded to a whole pixel, so the
-- line with the next word is within a pixel or two of (line) + (" word") measured apart, and only a line too close to
-- `width` to tell that way is measured whole. The lines break where measuring each whole would break them.
function G.wrapped(text, x, y, width, c, maxLines)
  local _, lh = ui.CalcTextSize("Ag")
  local n = 0
  for para in (text .. "\n"):gmatch("(.-)\n") do
    local line, hi, lo = "", 0, 0 -- the line and bounds of its width
    for word in para:gmatch("%S+") do
      if line == "" then
        line = word
        hi = ui.CalcTextSize(word)
        lo = hi
      else
        local ws = ui.CalcTextSize(" " .. word)
        -- (each width is its float sum rounded up, and float sums of ~2000 px are not exact: a pixel either way, and
        -- one more below)
        local tryHi, tryLo = hi + ws + 1, lo + ws - 2
        local fits
        if tryHi <= width then fits = true
        elseif tryLo > width then fits = false
        else
          local tw = ui.CalcTextSize(line .. " " .. word)
          fits = tw <= width
          tryHi, tryLo = tw, tw
        end
        if fits then
          line, hi, lo = line .. " " .. word, tryHi, tryLo
        else
          if not maxLines or n < maxLines then T.draw(x, y, line, c) end
          y, n, line = y + lh, n + 1, word
          hi = ui.CalcTextSize(word)
          lo = hi
        end
      end
    end
    if not maxLines or n < maxLines then T.draw(x, y, line, c) end
    y, n = y + lh, n + 1
  end
  return y
end

-- A button for the child `id`: greyed while the window disables it, and hidden (nothing drawn, false) while the
-- window hides it unless opts.always. Returns clicked (the child has been clicked).
function G.button(win, id, label, w, h, opts)
  opts = opts or T.NO_OPTS
  if not opts.always and API.windows.ChildShown(win, id) == false then return false end
  local clicked = T.button("##" .. id, label, w, h, opts, pad, opts.disabled or not G.enabled(win, id))
  if clicked then API.windows.ClickChild(win, id, "left") end
  return clicked
end

-- A check box for the child `id` as a button: the label on the left, the box on the right (ticked while the child
-- is checked); A or a click toggles it (the child's click).
function G.check(win, id, label, w, h, opts)
  opts = opts or {}
  local checked = G.checked(win, id)
  local clicked, focused, x0, y0, x1, y1 = T.button("##" .. id, label, w, h,
    { align = 0, disabled = not G.enabled(win, id), sub = opts.sub }, pad)
  local bs = math.min(T.px(32), h - T.px(16))
  local bx1, by0 = x1 - T.px(16), (y0 + y1 - bs) / 2
  local bx0, by1 = bx1 - bs, by0 + bs
  if checked then
    T.rect(bx0, by0, bx1, by1, C.accent, 1, T.px(6))
    local c = C.onAccent
    local t = math.max(2, T.px(3.5))
    ui.DrawLine(bx0 + bs * 0.22, by0 + bs * 0.52, bx0 + bs * 0.42, by0 + bs * 0.72, c[1], c[2], c[3], 1, t)
    ui.DrawLine(bx0 + bs * 0.42, by0 + bs * 0.72, bx0 + bs * 0.78, by0 + bs * 0.30, c[1], c[2], c[3], 1, t)
  else
    T.rect(bx0, by0, bx1, by1, C.sunken, 1, T.px(6))
    T.outline(bx0, by0, bx1, by1, C.dim, 0.8, T.px(6), math.max(1, T.px(2)))
  end
  if clicked then API.windows.ClickChild(win, id, "left") end
  return clicked
end

-- A combo box child as a button: its caption over the current choice; A or a click steps to the next choice
-- (opts.back: to the previous one instead). Returns the choices and the selected index (0-based).
function G.combo(win, id, label, w, h, opts)
  opts = opts or {}
  local choices, sel = API.windows.ComboInfo(win, id)
  choices = choices or {}
  local cur = sel and choices[sel + 1] or ""
  local n = #choices
  local clicked = T.button("##" .. id, label, w, h,
    { sub = cur ~= "" and (cur .. (n > 1 and "   (" .. ((sel or 0) + 1) .. " / " .. n .. ")" or "")) or "-",
      align = 0, disabled = n < 2 or not G.enabled(win, id) }, pad)
  if clicked and n > 0 then API.windows.ComboSelect(win, id, ((sel or -1) + (opts.back and -1 or 1)) % n) end
  return choices, sel
end

-- A D-pad button held: true when it went down, then again and again while it stays down (an amount, a colour).
local held = {}
function G.repeated(b)
  if pad.menuPressed(b) then
    held[b] = ui.GetTime() + 0.4
    return true
  end
  if held[b] and pad.connected() and pad.mode() == "menu" and pad.down(b) then
    if ui.GetTime() >= held[b] then
      held[b] = ui.GetTime() + 0.06
      return true
    end
  else
    held[b] = nil
  end
  return false
end

-- A list child as a W.list: rows 1..count from hs.ListInfo / hs.ListRow, the window's selection marked, A or a
-- click selects the row in the original (hs.ListClick). drawRow(i, texts, x0, y0, x1, y1, focused) draws a row
-- (texts: the row's columns, plus color = { r, g, b } and icon = { tex, u0, v0, u1, v1 }). opts.onClick(i) after
-- the click. Returns the row count and the focused row.
function G.list(win, id, state, visible, rowH, width, drawRow, opts)
  opts = opts or {}
  local rows, _, sel = API.windows.ListInfo(win, id)
  rows = rows or 0
  state.selected = (sel and sel >= 0) and sel + 1 or nil
  state.shown = visible
  local focusedRow = W.list(win .. id, state, rows, visible, rowH, width, function(i, x0, y0, x1, y1, focused)
    local texts, r, g, b, tex, u0, v0, u1, v1 = API.windows.ListRow(win, id, i - 1)
    texts = texts or {}
    if r then texts.color = { r, g, b, 1 } end
    if type(tex) == "userdata" then texts.icon = { tex, u0, v0, u1, v1 } end
    drawRow(i, texts, x0, y0, x1, y1, focused)
  end, function(i)
    API.windows.ListClick(win, id, i - 1, "left")
    if opts.onClick then opts.onClick(i) end
  end)
  return rows, focusedRow
end

-- A colour swatch in r, g, b over x0..x1, y0..y1 (a hairline round it).
function G.swatch(x0, y0, x1, y1, r, g, b, radius)
  local rad = radius or T.px(8)
  ui.DrawRectFilled(x0, y0, x1, y1, r, g, b, 1, rad)
  T.outline(x0, y0, x1, y1, C.ink, 0.25, rad)
end

-- A child's decal tint (a tint or colour picker swatch), else white.
function G.tint(win, id)
  local r, g, b = API.windows.ChildColor(win, id, "decal")
  if not r then return 1, 1, 1 end
  return r, g, b
end

-- "#RRGGBB" for a colour of 0..1 parts.
function G.hex(r, g, b)
  local function p(v) return math.floor(math.max(0, math.min(1, v)) * 255 + 0.5) end
  return string.format("#%02X%02X%02X", p(r), p(g), p(b))
end

-- A field that shows the edit box child's text (a placeholder while it is empty) as a button: A or a click opens
-- the keys for it (the caller's editing state); opts.focus puts the pad's focus on it. Returns clicked.
function G.field(win, id, label, w, h, placeholder, opts)
  opts = opts or {}
  local text = G.text(win, id)
  if opts.focus then -- (the keys just closed: the pad's focus back on the field)
    ui.SetKeyboardFocusHere()
    pad.showCursor()
  end
  local clicked, focused, x0, y0, x1, y1 = T.button("##field" .. id, "", w, h, {}, pad)
  local labelW = opts.labelW or T.px(170)
  T.font(19)
  local _, lh = ui.CalcTextSize("Ag")
  local ly1 = opts.multiline and y0 + T.px(14) + lh or y1
  T.textIn(x0 + T.px(16), opts.multiline and y0 + T.px(14) or y0, x0 + labelW, ly1, label, focused and C.accent or C.dim, 0)
  T.unfont()
  if opts.multiline then
    local _, lh = ui.CalcTextSize("Ag")
    ui.PushClipRect(x0 + labelW, y0, x1 - T.px(12), y1 - T.px(8), true)
    local y = y0 + T.px(14)
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
      y = G.wrapped(line, x0 + labelW, y, x1 - x0 - labelW - T.px(20), C.text)
      if y > y1 then break end
    end
    ui.PopClipRect()
    if text == "" then T.draw(x0 + labelW, y0 + T.px(14), placeholder or "(empty)", C.faint) end
  else
    T.textIn(x0 + labelW, y0, x1 - T.px(16), y1, text ~= "" and T.fit(text, x1 - x0 - labelW - T.px(24)) or
      (placeholder or "(empty)"), text ~= "" and C.text or C.faint, 0)
  end
  return clicked
end

-- The keys typing into the edit box child `id` (the window keeps the text: hs.SetWindowText), from x0 to x1 at the
-- cursor: an input box over them for a real keyboard (Enter: done), then the pad's keys. opts.max (the most
-- characters), opts.multiline, opts.digits (only 0-9 kept). Returns "done" when Done (or Enter) was pressed.
function G.edit(kb, win, id, x0, x1, opts)
  opts = opts or {}
  local max = opts.max or 255
  local function get() return G.text(win, id) end
  local function set(t)
    if opts.digits then t = t:gsub("%D", "") end
    if #t > max then t = t:sub(1, max) end
    if t ~= get() then API.windows.SetText(win, id, t) end
  end
  local wx, wy = ui.GetWindowPos()
  local _, cy = ui.GetCursorScreenPos()
  ui.SetCursorPos(x0 - wx, cy - wy)
  local text = get()
  local result
  T.font(24)
  if opts.multiline then
    local t = ui.InputTextMultiline("##edit" .. id, text, max + 1, x1 - x0, T.px(opts.height or 150))
    if t ~= text then set(t) end
  else
    ui.SetNextItemWidth(x1 - x0)
    local t, enter = ui.InputText("##edit" .. id, text, max + 1, ui.InputTextFlags.EnterReturnsTrue, { moveToEnd = true })
    if t ~= text then set(t) end
    if enter then result = "done" end
  end
  T.unfont()
  ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(6) - wy)
  if KF.draw(kb, get, set, x0, x1, { newline = opts.multiline, keyHeight = opts.keyHeight }) == "done" then result = "done" end
  return result
end

-- The keys' hints (and what B does while they are up).
function G.editHints(bLabel)
  local h = {}
  for _, x in ipairs(KF.hints) do h[#h + 1] = x end
  h[#h + 1] = { "b", bLabel or "Done" }
  return h
end

-- Opens the keys (the focus goes to them next frame): kb.shift for a name's capital.
function G.openKeys(kb, capital)
  kb.shift = capital == true
  KF.focus(kb)
end

-- Testing: the pack's settings file (ui/settings/gamepad.txt) test.show = Screen[@seconds],... shows those stock
-- windows once, that many seconds (default 6) after zoning in (hs.ShowWindow; spell:<id> shows that spell's display,
-- hs.ShowSpell; read:<slot>[.<bag index>] right-clicks that possessions slot, as reading a note does), for screenshots of windows the game opens only on an event.
local testStart
function G.testShow()
  if GPSTATE.testShown or not API.session.InGame() then return end
  local list = ui.GetSetting("test.show") or ""
  if list == "" then GPSTATE.testShown = true return end
  testStart = testStart or ui.GetTime()
  GPSTATE.testShowDone = GPSTATE.testShowDone or {}
  local left = 0
  for item in list:gmatch("[^,]+") do
    local screen, at = item:match("^%s*([^@%s]+)@?(%d*)")
    if screen and not GPSTATE.testShowDone[item] then
      if ui.GetTime() - testStart >= (tonumber(at) or 6) then
        GPSTATE.testShowDone[item] = true
        if screen:match("^spell:%d+$") then API.spells.Show(tonumber(screen:sub(7)))
        elseif screen:match("^read:%d+") then -- a right click on that slot (a note or book is read)
          local slot, bag = screen:match("^read:(%d+)%.?(%d*)")
          API.inventory.ClickSlot(tonumber(slot), tonumber(bag), "rightup")
        else API.windows.Show(screen, true) end
        ui.Log("gamepad test.show: " .. screen)
      else
        left = left + 1
      end
    end
  end
  if left == 0 then GPSTATE.testShown = true end
end

return G
