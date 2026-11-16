-- The gamepad pack's windows: a centred panel with a title, the pad hints along its bottom, and focus for the pad.
--
--   local W = dofile("lib/window.lua")
--   ui.window("Loot", function()
--     if not W.shown("LootWnd") then return end            -- hs.WindowShown: the game has it shown
--     if W.begin("Loot", { w = 900, h = 640, title = "Loot", hints = {...} }) then ... end
--     if W.finish() then hs.ShowWindow("LootWnd", false) end -- B (or the close button) asked to close it
--   end)
--
-- A window takes the pad's focus when it appears (hs.gamepad.request_focus: menu mode), and B closes it while it
-- has the focus. Sizes are design pixels (lib/theme.lua).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = { T = T, pad = pad }

GPSTATE = GPSTATE or { drawn = {}, closeAsked = {} }
local S = GPSTATE
-- Testing: the pack's settings file (ui/settings/gamepad.txt) test.open = menu,chat,group,eqmenu opens those once,
-- inspect<slot> / pickup<slot> clicks that inventory slot (item display, quantity dialog) a few seconds in
-- (screenshots of windows the pad opens).
if not S.testOpened then
  S.testOpened = true
  for w in (ui.GetSetting("test.open") or ""):gmatch("[^,]+") do
    if w == "menu" then S.menuOpen = true elseif w == "chat" then S.chatOpen = true
    elseif w == "group" then S.groupOpen = true elseif w == "eqmenu" then S.eqMenuOpen = true
    elseif w:match("^inspect%d+$") then S.testSlot = { tonumber(w:match("%d+")), "rightheld" }
    elseif w:match("^pickup%d+$") then S.testSlot = { tonumber(w:match("%d+")), "left" } end
  end
end

-- ImGui window flags the Lua table may not name (ImGui 1.91: NoNavInputs 1 << 16, NoNavFocus 1 << 17).
W.NoNav = (1 << 16) | (1 << 17)

-- Whether the game shows the stock window `screen` (and so this pack's window for it); turns the C++ stand-in off.
function W.shown(screen, sizable)
  S.own = S.own or {}
  S.own[screen] = true -- drawn by this pack
  if not API.session.InGame() then
    S.inGameAt = nil
    return false
  end
  S.inGameAt = S.inGameAt or ui.GetTime()
  local shown = API.windows.Shown(screen)
  return shown
end

local current

-- Begins the window `name` (also its ImGui name, which hs.gamepad.request_focus is given). opts: w, h (design px),
-- x, y (design px from the top left; default centred), title, subtitle, hints ({ { button, text } }), close
-- (false: no close button / B), noFocus (does not take the pad's focus when it appears), alwaysFocus (takes it even
-- just after zoning in), noNav (no ImGui navigation: the window reads the pad's buttons itself), alpha.
function W.begin(name, opts)
  T.frame()
  T.resetFonts()
  -- by frames, not seconds: at a low frame rate (a window in the background) every frame would look new
  local now, frame = ui.GetTime(), ui.GetFrameCount()
  local last = S.drawn[name]
  local appearing = not last or frame - last > 1
  S.drawn[name] = frame
  S.currentWindow = name -- (between W.begin and W.finish: the libraries' copies of this file have no `current`)
  S.appeared = S.appeared or {}
  if appearing then S.appeared[name] = frame end
  -- The theme keeps text readable at small resolutions, so scaled design sizes can exceed the viewport.
  local margin = T.px(12)
  local w = math.min(math.max(1, T.px(opts.w or 900)), math.max(1, T.W - 2 * margin))
  local h = math.min(math.max(1, T.px(opts.h or 600)), math.max(1, T.H - 2 * margin))
  local x = opts.x and T.px(opts.x) or (T.W - w) / 2
  local y = opts.y and T.px(opts.y) or (T.H - h) / 2
  x = math.max(0, math.min(x, math.max(0, T.W - w)))
  y = math.max(0, math.min(y, math.max(0, T.H - h)))
  ui.SetNextWindowPos(x, y, ui.Cond.Always)
  ui.SetNextWindowSize(w, h, ui.Cond.Always)
  -- (on top of the HUD; the client has no keyboard navigation, so the focus never takes the game's keys)
  if appearing and not opts.noFocus then ui.SetNextWindowFocus() end
  local pd = T.px(opts.padding or 24)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, pd, pd)
  ui.PushStyleVar(ui.StyleVar.WindowRounding, T.px(18))
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, T.px(10), T.px(10))
  ui.PushStyleVar(ui.StyleVar.FramePadding, T.px(12), T.px(8))
  ui.PushStyleVar(ui.StyleVar.FrameRounding, T.px(10))
  ui.PushStyleColor(ui.Col.WindowBg, 0, 0, 0, 0)
  ui.PushStyleColor(ui.Col.Text, T.C.text[1], T.C.text[2], T.C.text[3], 1)
  ui.PushStyleColor(ui.Col.FrameBg, T.C.raised[1], T.C.raised[2], T.C.raised[3], 1)
  ui.PushStyleColor(ui.Col.FrameBgHovered, T.C.hover[1], T.C.hover[2], T.C.hover[3], 1)
  ui.PushStyleColor(ui.Col.FrameBgActive, T.C.selected[1], T.C.selected[2], T.C.selected[3], 1)
  ui.PushStyleColor(ui.Col.Button, T.C.raised[1], T.C.raised[2], T.C.raised[3], 1)
  ui.PushStyleColor(ui.Col.ButtonHovered, T.C.selected[1], T.C.selected[2], T.C.selected[3], 1)
  ui.PushStyleColor(ui.Col.ButtonActive, T.C.accent[1], T.C.accent[2], T.C.accent[3], 1)
  ui.PushStyleColor(ui.Col.Header, T.C.selected[1], T.C.selected[2], T.C.selected[3], 1)
  ui.PushStyleColor(ui.Col.PopupBg, T.C.popup[1], T.C.popup[2], T.C.popup[3], T.C.popup[4])
  ui.PushStyleColor(ui.Col.Border, T.C.ink[1], T.C.ink[2], T.C.ink[3], 0.08)
  local flags = ui.WindowFlags.NoTitleBar | ui.WindowFlags.NoResize | ui.WindowFlags.NoMove
      | ui.WindowFlags.NoSavedSettings | ui.WindowFlags.NoScrollbar | ui.WindowFlags.NoScrollWithMouse
      | ui.WindowFlags.NoCollapse
  if opts.noFocus then flags = flags | ui.WindowFlags.NoFocusOnAppearing end
  -- (noNav: the D-pad and A are the window's own (W.pressed), not ImGui's navigation between its buttons)
  if opts.noNav then flags = flags | (1 << 16) end
  local visible = ui.Begin(name, nil, flags)
  -- gaining the focus starts the pad's settling time (T.settle)
  S.wasFocused = S.wasFocused or {}
  local hasFocus = ui.IsWindowFocused(3)
  if hasFocus and (appearing or not S.wasFocused[name]) then S.padSettleUntil = now + 0.25 end
  S.wasFocused[name] = hasFocus
  current = { name = name, opts = opts, x = x, y = y, w = w, h = h, visible = visible, close = false, pd = pd }
  -- (windows the game restores at zoning in stay as they were: the pad starts out playing)
  -- (opts.alwaysFocus: a window the server opens, never one restored)
  if appearing and not opts.noFocus and (opts.alwaysFocus or ui.GetTime() - (S.inGameAt or 0) > 3) then pad.focus(name) end
  if not visible then return false end
  T.font(T.BASE)
  -- body
  T.panel(x, y, x + w, y + h, opts.alpha or 1, T.px(18), T.C.window)
  -- title bar: the title, a subtitle under it, the close button on the right
  local top = y + pd
  if opts.title then
    T.font(30)
    local _, th = ui.CalcTextSize("Ag")
    T.draw(x + pd, top, opts.title)
    T.unfont()
    top = top + th
    if opts.subtitle and opts.subtitle ~= "" then
      T.font(18)
      local _, sh = ui.CalcTextSize("Ag")
      T.draw(x + pd, top + T.px(2), opts.subtitle, T.C.dim)
      T.unfont()
      top = top + sh + T.px(2)
    end
    top = top + T.px(12)
    ui.DrawLine(x + pd, top, x + w - pd, top, T.C.ink[1], T.C.ink[2], T.C.ink[3], 0.08, 1)
    top = top + T.px(14)
  end
  if opts.close ~= false then
    -- a round close button (mouse); B does the same with the pad
    local cs = T.px(40)
    local cx0, cy0 = x + w - pd - cs, y + pd
    ui.SetCursorPos(cx0 - x, cy0 - y)
    if ui.InvisibleButton("##close", cs, cs) then current.close = true end
    local hov = ui.IsItemHovered()
    ui.DrawCircleFilled(cx0 + cs / 2, cy0 + cs / 2, cs / 2, hov and 0.85 or 0.18, hov and 0.30 or 0.20, hov and 0.30 or 0.24, 0.95)
    local s = cs * 0.18
    ui.DrawLine(cx0 + cs / 2 - s, cy0 + cs / 2 - s, cx0 + cs / 2 + s, cy0 + cs / 2 + s, 1, 1, 1, 0.9, T.px(3))
    ui.DrawLine(cx0 + cs / 2 - s, cy0 + cs / 2 + s, cx0 + cs / 2 + s, cy0 + cs / 2 - s, 1, 1, 1, 0.9, T.px(3))
    if ui.IsWindowFocused(3) and pad.menuPressed("b") then current.close = true end
    -- in game mode B closes only the C++ windows: the newest open window of this pack closes itself
    if pad.connected() and pad.mode() == "game" and W.newest() == name then
      pad.takes_b() -- (the client's B then leaves the target alone)
      if pad.pressed("b") then current.close = true end
    end
  end
  -- hints along the bottom
  local hintH = 0
  if opts.hints and #opts.hints > 0 then
    hintH = T.px(30) + T.px(18)
    local hy = y + h - pd - T.px(30)
    ui.DrawLine(x + pd, hy - T.px(12), x + w - pd, hy - T.px(12), T.C.ink[1], T.C.ink[2], T.C.ink[3], 0.08, 1)
    T.font(18)
    T.hints(pad, opts.hints, x + pd, hy, T.px(30))
    T.unfont()
  end
  current.top, current.bottom = top, y + h - pd - hintH
  ui.SetCursorPos(pd, top - y)
  return true
end

-- The pack's window that opened last among those open now.
function W.newest()
  local frame, best, at = ui.GetFrameCount(), nil, -1
  for n, t in pairs(S.drawn) do
    local a = S.appeared and S.appeared[n] or 0
    if frame - t <= 1 and a > at then best, at = n, a end
  end
  return best
end

-- The content area of the current window: x0, y0, x1, y1 (screen) and its width and height.
function W.area()
  local c = current
  return c.x + c.pd, c.top, c.x + c.w - c.pd, c.bottom, c.w - 2 * c.pd, c.bottom - c.top
end

-- Ends the window; true when it was asked to close (its close button, or B while it has the focus).
function W.finish()
  local c = current
  if c and c.visible then T.unfont() end
  ui.End()
  ui.PopStyleColor(11)
  ui.PopStyleVar(6)
  current = nil
  S.currentWindow = nil
  return c and c.close
end

-- A click to run once A is released (a click that opens the quantity dialog, so the A still held does not press the
-- dialog): kept for the window being drawn, and dropped if that window is gone (not drawn last frame) before then,
-- so it never fires into another window. W.deferred(slot, fn) keeps it in the table `slot`; W.runDeferred(slot)
-- runs or drops it, each frame.
function W.deferred(slot, fn)
  slot.fn, slot.owner = fn, S.currentWindow
end
function W.runDeferred(slot)
  local fn = slot.fn
  if not fn then return end
  local owner = slot.owner
  if owner and (S.drawn[owner] or -1) < ui.GetFrameCount() - 1 then
    slot.fn, slot.owner = nil, nil
    return
  end
  if pad.down("a") then return end
  slot.fn, slot.owner = nil, nil
  fn()
end

-- Whether the window `name` (this pack's) has the focus, to give it the pad's shortcuts (Y, X, LB / RB, ...).
function W.focused() return ui.IsWindowFocused(3) end

-- A pad shortcut for the current window: the button went down while the window has the focus (menu mode).
function W.pressed(b) return ui.IsWindowFocused(3) and pad.menuPressed(b) end

-- ---------------------------------------------------------------- lists
-- A list that scrolls by moving its window of rows: `visible` rows of `rowH` design px at the cursor, rows
-- 1..count, drawRow(i, x0, y0, x1, y1, focused) draws one and returns nothing; onClick(i) on A / a click.
-- state = { offset = 0 } kept by the caller. D-pad down on the last shown row (or up on the first) scrolls it, as
-- does the mouse wheel over it. Returns the focused row (or nil).
local listRowIds = {} -- by list id: its rows' ImGui ids ("<id>:<v>"), made once
function W.list(id, state, count, visible, rowH, width, drawRow, onClick)
  local rowIds = listRowIds[id]
  if not rowIds then
    rowIds = {}
    listRowIds[id] = rowIds
  end
  state.offset = state.offset or 0
  local maxOff = math.max(0, count - visible)
  if state.offset > maxOff then state.offset = maxOff end
  if state.offset < 0 then state.offset = 0 end
  local h = T.px(rowH)
  local gap = T.px(10) -- the window's ItemSpacing
  local x0, y0 = ui.GetCursorScreenPos()
  local focusedRow
  local scrolled = false
  for v = 1, math.min(visible, count) do
    local i = state.offset + v
    local rowId = rowIds[v]
    if not rowId then
      rowId = id .. ":" .. v
      rowIds[v] = rowId
    end
    ui.PushID(rowId)
    if state.focusNext == v then
      ui.SetKeyboardFocusHere()
      pad.showCursor()
      state.focusNext = nil
    end
    local rx0, ry0 = ui.GetCursorScreenPos()
    local clicked = T.settle(ui.InvisibleButton("##row", width - T.px(14), h, ui.ButtonFlags.EnableNav), pad)
    if pad.yActivated(clicked) then clicked = false end
    local focused = pad.focused()
    local rx1, ry1 = rx0 + width - T.px(14), ry0 + h
    local r = T.px(10)
    T.rect(rx0, ry0, rx1, ry1, T.C.raised, focused and 1 or 0.7, r)
    if state.selected == i then T.rect(rx0, ry0, rx1, ry1, T.C.accent, 0.22, r) end
    drawRow(i, rx0, ry0, rx1, ry1, focused)
    if focused then
      T.focusRing(rx0, ry0, rx1, ry1, r)
      focusedRow = i
      if not scrolled and v == math.min(visible, count) and i < count and pad.menuPressed("down") then
        state.offset = state.offset + 1
        state.focusNext = v
        scrolled = true
      elseif not scrolled and v == 1 and i > 1 and pad.menuPressed("up") then
        state.offset = state.offset - 1
        state.focusNext = 1
        scrolled = true
      end
    end
    if clicked and onClick then onClick(i) end
    ui.PopID()
  end
  -- the scroll bar on the right
  local listH = visible * h + (visible - 1) * gap
  if count > visible then
    local sx = x0 + width - T.px(6)
    T.rect(sx, y0, sx + T.px(6), y0 + listH, T.C.sunken, 1, T.px(3))
    local th = math.max(T.px(30), listH * visible / count)
    local ty = y0 + (listH - th) * (state.offset / maxOff)
    T.rect(sx, ty, sx + T.px(6), ty + th, T.C.dim, 0.8, T.px(3))
  end
  if ui.IsMouseHoveringRect(x0, y0, x0 + width, y0 + listH) then
    local wheel = ui.GetIO().wheel or 0
    if wheel ~= 0 then state.offset = math.max(0, math.min(maxOff, state.offset - math.floor(wheel * 3 + 0.5))) end
  end
  if count == 0 then ui.Dummy(width, h) end
  return focusedRow
end

-- Opens the chat window's keyboard with `text` typed (a tell: "/tell name "), as a chat key does.
function W.chat(text) GPSTATE.chatText = text end

-- A Yes / No question over the current window (removing a friend, promoting a member): state.confirm =
-- { text, action } asks it; while it is up the window draws W.confirm(state) in place of its body (it takes the
-- pad's focus on No), and gives W.finish's answer to W.back. Returns true while it is up.
function W.confirm(state)
  local q = state.confirm
  if not q then return false end
  local c = current
  local w, h = math.min(c.w - 2 * c.pd, T.px(760)), T.px(230)
  local x0, y0 = c.x + (c.w - w) / 2, c.y + (c.h - h) / 2
  ui.DrawRectFilled(c.x, c.y, c.x + c.w, c.y + c.h, 0, 0, 0, 0.55, T.px(18))
  T.panel(x0, y0, x0 + w, y0 + h, 1, T.px(16), T.C.raised)
  T.font(24)
  T.textIn(x0 + T.px(24), y0 + T.px(20), x0 + w - T.px(24), y0 + T.px(90), T.fit(q[1], w - T.px(48)), T.C.text, 0.5)
  T.unfont()
  local bw = (w - T.px(24) * 2 - T.px(16)) / 2
  ui.SetCursorPos(x0 + T.px(24) - c.x, y0 + h - T.px(24) - T.px(72) - c.y)
  if T.button("##confirmyes", "Yes", bw, T.px(72), { primary = true }, pad) then
    state.confirm = nil
    q[2]()
  end
  ui.SameLine(0, T.px(16))
  if q.focus ~= false then
    ui.SetKeyboardFocusHere()
    q.focus = false
  end
  if T.button("##confirmno", "No", bw, T.px(72), {}, pad) then state.confirm = nil end
  return true
end

-- W.finish's answer for a window with a question or a keyboard up: B (or the close button) takes them down first;
-- true when the window itself is to close. `list` (a W.list state): the pad's focus goes back to its first shown
-- row when they are gone (their buttons had it).
function W.back(close, state, kb, list)
  local holder = state or kb
  if holder then
    local modal = (state and state.confirm ~= nil) or (kb and kb.open) or false
    if close then modal = false end
    if holder.wasModal and not modal and list then list.focusNext = 1 end
    holder.wasModal = modal
  end
  if not close then return false end
  if state and state.confirm then
    state.confirm = nil
    return false
  end
  if kb and kb.open then
    kb.open = false
    return false
  end
  return true
end

-- Between a W.list and the column of buttons beside it: ImGui scores a button half a row lower as nearer than the
-- next row (it all but ignores the sideways distance), so D-pad right, up / down in the column and left back are
-- moved by the window itself. side = {} kept by the window; W.toSide(side, row) after the list (row: W.list's
-- answer), and around the column's k-th of n buttons W.sideBefore(side, k) and W.sideAfter(side, k, n, list).
-- (A move is asked again until the button has the focus: ImGui's own move for the same press can win a frame.)
function W.toSide(side, row)
  if row and pad.menuPressed("right") then side.want, side.at = 1, ui.GetTime() end
end
function W.sideBefore(side, k)
  if side.want == k and ui.GetTime() - side.at < 0.5 then ui.SetKeyboardFocusHere() end
end
function W.sideAfter(side, k, n, list)
  if not (ui.IsItemFocused and ui.IsItemFocused()) then return end
  if side.want == k then side.want = nil end
  if pad.menuPressed("down") and k < n then side.want, side.at = k + 1, ui.GetTime()
  elseif pad.menuPressed("up") and k > 1 then side.want, side.at = k - 1, ui.GetTime()
  elseif pad.menuPressed("left") then
    local v = list.selected and list.selected - (list.offset or 0) or 1
    list.focusNext = (v >= 1 and v <= (list.shown or v)) and v or 1
  end
end

return W
