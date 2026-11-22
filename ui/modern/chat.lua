-- Chat: replaces every game chat window (Chat1 .. Chat32) with a translucent panel: tabs along the top (the active one
-- underlined, unread ones marked), the output, and an input with the channel plain text goes to. It works as the
-- the Classic UI's chat did: the native tab strip and its menu share the chat manager with the API.
-- The lines are the original output box's (hs.ChatText, STML), typed lines
-- go through the original window (hs.ChatSubmit: channel, tells, history, commands), Enter and the chat keys move
-- what the game's input holds here (hs.ChatTakeFocus), item links open the item display, Up / Down walk the
-- history, Tab / Shift+Tab on a /tell line step through the players to reply to (hs.ChatCycleReply). A player's
-- name in a line is a link: a click opens a tell to them (their tell window with Use Tell Windows, else
-- "/tell <name> " in the input). Right-click: the chat menu (windows, tabs, Always Chat Here, filters, default channel,
-- font, tell windows, timestamps, clear).
-- The output's text can be selected with the mouse (a drag; a double click takes a word, a triple click the line,
-- Shift+click extends) and copied with Ctrl+C or the menu's Copy: a Hailstone addition (lib/textselect.lua).
-- A button at the strip's right end folds the window into the strip (the output and the input go, the tabs stay), as
-- the combat ability window folds into its title bar; the frame's menu has the same (Roll up).
-- Unlocked (any frame's menu), a chat window is moved by dragging and resized from its corner.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local stml = dofile("lib/stml.lua")
local textselect = dofile("lib/textselect.lua")
local T = hud.T
local C = T.color
stml.LINK = C.chatLink -- (this copy of stml: chat links in the colour made for the chat's page)

local chats = {} -- by visible window slot

-- Item and achievement links: the input shows labels; exact tags go back in when
-- the line is sent (lib/chatlinks.lua).
local chatlinks = dofile("lib/chatlinks.lua")
local function collapseLinks(c, text, offset) return chatlinks.Collapse(c.links, text, offset) end
local function expandLinks(c, text) return chatlinks.Expand(c.links, text) end

local SIZE = function() return 470, 250 end
local RESIZABLE = { 240, 120 }
local HEADER, INPUT = 26, 26 -- (the tab strip's and the input's heights, design pixels)

local function state(slot)
  local c = chats[slot]
  if not c then
    c = { version = -1, lines = {}, input = "", focus = 0, follow = false, links = {} }
    -- (what the window's widgets take, made once and not every frame)
    c.onLink = function(link) API.chat.Link(slot, link[1], link[2]) end
    c.inputOpts = {
      moveToEnd = false,
      history = function(older)
        local line = API.chat.History(slot, older)
        if line then c.links = {}; line = collapseLinks(c, line) end
        return line
      end,
      -- Tab / Shift+Tab on a "/tell" or "/rtarget" line: the next older / newer player from the reply list
      tab = function(text, back)
        local line = API.chat.CycleReply(expandLinks(c, text), back)
        if line then c.links = {} end
        return line
      end,
    }
    -- the output's selection (lib/textselect.lua; stml.DrawLines fills in the place under the mouse)
    c.sel = { rows = 0, clicks = {}, color = { 0, 0, 0, 0 } }
    c.panelOpts = { alpha = 0.7 }
    c.closeWindow = function() API.chat.CloseWindow(slot) end
    c.winOpts = { size = SIZE, resizable = RESIZABLE, fit = slot == 1, panelOpts = c.panelOpts, closable = false,
      rollup = true, titleHeight = HEADER }
    chats[slot] = c
  end
  return c
end

local function update(slot, c)
  -- (players' names as links: a click opens a tell; with Timestamps each line's time first; only the lines that
  -- came since the last look, unless the window's lines changed otherwise)
  local version, lines, reset, count = API.chat.Text(slot, c.version, true, ui.GetSetting("chat.timestamps"), true)
  if version == c.version or not lines then return end
  c.version = version
  if reset then stml.UpdateLines(c, lines, C.chatText) else stml.AppendLines(c, lines, count, C.chatText) end
  -- (a selection goes with lines numbered afresh, or once every line of it has gone)
  local _, e = textselect.Range(c.sel)
  if reset or (e and e.line <= (c.dropped or 0)) then c.sel.anchor, c.sel.caret, c.sel.dragging = nil, nil, false end
  c.lastLine = ui.GetTime()
end

local function copySelection(c)
  local s, e = textselect.Range(c.sel)
  if s then ui.SetClipboardText(textselect.Extract(c, s, e)) end
end

local function chatContext(slot, c)
  if ui.IsWindowHovered(ui.HoveredFlags.ChildWindows) and ui.IsMouseClicked(ui.Mouse.Right)
      and not ui.GetIO().alt and not T.RightClickTaken() then
    if API.menus.Contexts() then ui.OpenPopup("chatmenu") end
    T.TakeRightClick()
  end
  if ui.BeginPopup("chatmenu") then
    ui.ChatTabMenu(slot)
    -- (after the original's rows and Hailstone's, docs/dev-ui.md "Modern chat tabs")
    if textselect.Range(c.sel) then
      ui.Separator()
      if ui.MenuItem("Copy", false, true, "Ctrl+C") then copySelection(c) end
    end
    ui.EndPopup()
  end
end

-- ---------------------------------------------------------------- drawing
local FLASH = 1.0 -- seconds a tell window's flash takes to fade

-- The roll-up button at the right end of the tab strip: the window folds into the strip (the tabs stay, the output and
-- the input go), as the combat ability window folds into its title bar. `x`: the button's left edge.
local function collapseButton(slot, x, y, s, rolled)
  local size = 16 * s
  local bx, by = x, y + (HEADER * s - size) / 2
  local clicked = T.Hit("##chatcollapse" .. slot, bx, by, bx + size, by + size)
  local hovered = ui.IsItemHovered()
  local c = hovered and C.accent or C.muted
  local cx, cy = bx + size / 2, by + size / 2
  local d = 4 * s
  if rolled then
    ui.DrawTriangleFilled(cx - d, cy - d / 2, cx + d, cy - d / 2, cx, cy + d / 2, c[1], c[2], c[3], 1)
  else
    ui.DrawTriangleFilled(cx - d, cy + d / 2, cx + d, cy + d / 2, cx, cy - d / 2, c[1], c[2], c[3], 1)
  end
  if hovered then ui.SetTooltip(rolled and "Expand" or "Collapse") end
  if clicked then hud.SetRolled("Chat" .. slot, not rolled) end
end

-- The place under the mouse as stml.DrawLines found it: the row above it, else (the mouse above every line drawn)
-- the first line's start.
local function mousePlace(sel)
  if sel.hitLine then return { line = sel.hitLine, off = sel.hitOff } end
  if sel.firstLine then return { line = sel.firstLine, off = 0 } end
  return nil
end

-- Selecting the output's text, after its lines are drawn (in the output's child): a press starts a selection (not
-- on a link, which it follows, nor with Alt, which moves the frame), a drag moves its end and scrolls the output
-- when the mouse is above or below it, a release with nothing selected clears it. A double click takes the word, a
-- triple click the line; Shift+click moves the end of the selection there is.
local function selectWithMouse(c, hovered, top, bottom, s)
  local sel, io = c.sel, ui.GetIO()
  if hovered and ui.IsMouseClicked(ui.Mouse.Left) and not io.alt and not sel.overLink then
    local at = mousePlace(sel)
    if not at then return end
    local clicks = textselect.Clicks(sel.clicks, ui.GetTime(), sel.mx, sel.my, 0.35, 4 * s)
    if clicks == 1 then
      if io.shift and sel.anchor then sel.caret = at else sel.anchor, sel.caret = at, at end
      sel.dragging = true
    else
      local l = c.lines[at.line - (c.dropped or 0)]
      local text = l and textselect.LineText(l) or ""
      local a, b = 0, #text
      if clicks == 2 then a, b = textselect.Word(text, at.off) end
      sel.anchor, sel.caret, sel.dragging = { line = at.line, off = a }, { line = at.line, off = b }, false
    end
  elseif sel.dragging then
    if ui.IsMouseDown(ui.Mouse.Left) then
      sel.caret = mousePlace(sel) or sel.caret
      local d = (sel.my < top and sel.my - top) or (sel.my > bottom and sel.my - bottom) or 0
      if d ~= 0 then ui.SetScrollY(math.max(0, math.min(ui.GetScrollMaxY(), ui.GetScrollY() + d * 0.25))) end
    else
      sel.dragging = false
      if not textselect.Range(sel) then sel.anchor, sel.caret = nil, nil end
    end
  end
end

local function body(slot, c, w, x, y, width, height, s)
  local font, active, channel = API.chat.Info(slot)
  font = math.max(0, math.min(10, font or 3)) -- the chat font list (/chatfontsize 0..10)
  local pad = 8 * s
  -- rolled up: the tab strip is the title bar the window folds into, and all that is left of it (the frame's own:
  -- this frame's, as the frame is drawn, so a click does not fold it before the frame is smaller)
  local rolled = c.winOpts.rolled
  local right = (w.tells and 26 * s or 0) + 22 * s -- (the room kept free at the end: the tell window's close box, the button)
  if ui.ChatTabStrip(slot, x + 3 * s, y, width - right - 3 * s, HEADER * s, s,
      C.text, C.muted, C.accent, C.textShadow, rolled) then T.TakeRightClick() end
  collapseButton(slot, x + width - right, y, s, rolled)
  if rolled then chatContext(slot, c); return false end
  ui.DrawLine(x + pad, y + HEADER * s, x + width - pad, y + HEADER * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)

  -- the output
  local oy = y + HEADER * s + 4 * s
  local oh = height - HEADER * s - INPUT * s - 16 * s
  local wx, wy = ui.GetWindowPos()
  local cb = C.chatBg -- (a light palette's: a dark page under the lines, whose colours are the player's user colours)
  if cb[4] > 0 then ui.DrawRectFilled(x + pad - 4 * s, oy - 2 * s, x + width - pad + 4 * s, oy + math.max(oh, 10) + 2 * s, cb[1], cb[2], cb[3], cb[4], 6 * s) end
  ui.SetCursorPos(x - wx + pad, oy - wy)
  ui.PushChatFont(font)
  ui.PushStyleColor(ui.Col.ChildBg, 0, 0, 0, 0)
  local selected = w.tabs[w.tab] or {}
  local scrollFlags = selected.scrollbar == false and ui.WindowFlags.NoScrollbar or 0
  ui.BeginChild("output", width - 2 * pad + 4 * s, math.max(oh, 10), false, scrollFlags)
  local lw, viewH = ui.GetContentRegionAvail()
  -- (the lines' wraps and heights are kept while this stays: the width and the font, measured, as a UI scale
  -- changes the font's size)
  local pw, ph = ui.CalcTextSize("Wg il")
  local probe = pw * 4096 + ph
  if c.keyW ~= lw or c.keyFont ~= probe then c.keyW, c.keyFont, c.key = lw, probe, lw .. ":" .. probe end
  local sel = c.sel
  sel.mx, sel.my = ui.GetMousePos()
  sel.s, sel.e = textselect.Range(sel)
  local hc = C.accent
  sel.color[1], sel.color[2], sel.color[3], sel.color[4] = hc[1], hc[2], hc[3], 0.35
  sel.rows, sel.candRow, sel.hitLine, sel.firstLine, sel.overLink = 0, nil, nil, nil, false
  local _, top = ui.GetWindowPos()
  local left = ui.GetCursorScreenPos()
  -- (the lines, not the scroll bar beside them)
  local outHovered = ui.IsWindowHovered() and sel.mx <= left + lw
  local _, rowH = ui.CalcTextSize("Ag")
  sel.active, sel.lineH = outHovered or sel.dragging or sel.s ~= nil, rowH
  stml.DrawLines(c, lw, viewH, c.key, c.onLink, sel)
  local page = API.chat.Page(slot) -- the PAGEUP / PAGEDOWN keys: a page less 8 up or down
  ui.ScrollChatTab(slot, c.tab, page, viewH - 8 * s, c.follow)
  c.follow = false
  -- (after ScrollChatTab, which keeps an output scrolled to its end there: a drag scrolls it from there)
  selectWithMouse(c, outHovered, top, top + viewH, s)
  ui.EndChild()
  -- Ctrl+C copies the selection while the window has the focus and its input does not (which copies its own text);
  -- with a selection and Ctrl down the keys are the UI's, so a Ctrl+C key bind does not fire too
  if sel.s and ui.GetIO().ctrl and not ui.GetIO().typing and ui.IsWindowFocused(3) then
    ui.CaptureKeyboard()
    if ui.IsKeyPressed(ui.Key.C) then copySelection(c) end
  end
  ui.PopStyleColor()

  -- the input: the channel plain text goes to, then the field
  local iy = y + height - INPUT * s - pad
  local ix0, ix1 = x + pad, x + width - pad
  local chip = selected.tell and "Tell" or (channel or "Say")
  ui.PushFont(T.font.small)
  local cw = ui.CalcTextSize(chip) + 14 * s
  ui.PopFont()
  ui.DrawRectFilled(ix0, iy, ix1, iy + INPUT * s, C.inset[1], C.inset[2], C.inset[3], 0.85, 6 * s)
  ui.DrawRectFilled(ix0 + 3 * s, iy + 3 * s, ix0 + 3 * s + cw, iy + INPUT * s - 3 * s, C.accent[1], C.accent[2], C.accent[3], 0.16, 4 * s)
  local _, sh = T.TextSize("Ag", T.font.small)
  T.Text(ix0 + 3 * s + cw / 2, iy + (INPUT * s - sh) / 2, chip, C.accent, T.font.small, "center")

  -- (what is typed goes along, links as their tags: the loot window's Link All sends the lines that fill up and
  -- hands back what the input holds after)
  local typed = c.input
  if #c.links > 0 then -- (expanded again only when the input or its links changed)
    if c.expandedFrom ~= typed or c.expandedLinks ~= c.links or c.expandedN ~= #c.links then
      c.expandedFrom, c.expandedLinks, c.expandedN, c.expanded = typed, c.links, #c.links, expandLinks(c, typed)
    end
    typed = c.expanded
  end
  local taken, replace = API.chat.TakeFocus(slot, typed)
  if taken then
    if replace then c.input, c.links = "", {} end
    local offset = #c.input
    c.input = c.input .. collapseLinks(c, taken, offset)
    c.inputError = nil
    c.focus = 3
    c.grab = 30 -- (asked for until the input has the keys: a name link's click holds the mouse on the output)
  end
  local fx = ix0 + cw + 8 * s
  ui.SetCursorPos(fx - wx, iy - wy)
  local grab = (c.grab or 0) > 0 and not ui.IsMouseDown(ui.Mouse.Left)
  if grab then ui.SetKeyboardFocusHere(); c.focus = math.max(c.focus, 3) end
  ui.SetNextItemWidth(ix1 - fx - 4 * s)
  local _, lh = ui.CalcTextSize("Ag")
  ui.PushStyleColor(ui.Col.FrameBg, 0, 0, 0, 0)
  ui.PushStyleColor(ui.Col.FrameBgHovered, 0, 0, 0, 0)
  ui.PushStyleColor(ui.Col.FrameBgActive, 0, 0, 0, 0)
  ui.PushStyleVar(ui.StyleVar.FramePadding, 2 * s, math.max(0, (INPUT * s - lh) / 2))
  c.inputOpts.moveToEnd = c.focus > 0
  local text, enter = ui.InputText("##input", c.input, 2048, ui.InputTextFlags.EnterReturnsTrue, c.inputOpts)
  local editing = ui.IsItemActive()
  if editing then c.grab = 0 elseif grab then c.grab = c.grab - 1 end
  ui.PopStyleVar()
  ui.PopStyleColor(3)
  ui.PopFont()
  if text == "" and not editing then
    T.Text(fx + 2 * s, iy + (INPUT * s - sh) / 2, active and "Press Enter to chat" or "Click to chat here", C.faint, T.font.small)
  end
  if editing then
    ui.DrawRect(ix0, iy, ix1, iy + INPUT * s, C.accent[1], C.accent[2], C.accent[3], 0.6, 6 * s, 1)
  end
  if c.focus > 0 then c.focus = c.focus - 1 end
  if text ~= c.input then c.inputError = nil end
  c.input = text
  if text == "" and next(c.links) then c.links = {} end
  if enter then
    local expanded, reason = chatlinks.ForSubmit(c.links, text)
    if expanded then
      if expanded ~= "" then API.chat.Submit(slot, expanded) end
      c.input, c.links, c.inputError = "", {}, nil
      c.follow = true
    else c.inputError = reason end
  end
  if c.inputError then ui.SetTooltip(c.inputError) end

  if ui.IsWindowHovered(ui.HoveredFlags.ChildWindows) and ui.IsMouseClicked(ui.Mouse.Left) and not active then
    API.chat.Activate(slot)
  end
  chatContext(slot, c)
  return editing
end

-- The chat windows by slot, asked for once a frame: each of the 32 windows below used to build the whole list
-- (a table per window and per tab) every frame, though only a few exist.
-- The list and the table by slot are refilled each frame rather than made anew (hs.ChatWindows' `into`: its records
-- are not kept past the next call).
local listedFrame, bySlot, listed = -1, {}, {}
local function chatWindow(slot)
  local frame = ui.GetFrameCount()
  if frame ~= listedFrame then
    listedFrame = frame
    for k in pairs(bySlot) do bySlot[k] = nil end
    for _, w in ipairs(API.chat.Windows(listed)) do bySlot[w.slot] = w end
  end
  return bySlot[slot]
end

-- What is kept by slot (a half-typed line, its links) belongs to the character: it is dropped when the character
-- select (or creation) screen opens, or another character is seen in game. (Checked once a frame, before the
-- windows' own check: zoning shows no character for a while, and keeps it all.)
local checkedFrame, lastName, lastAtSelect = -1, nil, false
local function forgetOnCharacterChange()
  local frame = ui.GetFrameCount()
  if frame == checkedFrame then return end
  checkedFrame = frame
  local gs = API.session.GameState and API.session.GameState()
  local atSelect = gs == 1 or gs == 2
  local name = API.character.Name and API.character.Name()
  if (atSelect and not lastAtSelect) or (name and lastName and name ~= lastName) then
    for k in pairs(chats) do chats[k] = nil end
  end
  lastAtSelect = atSelect
  if atSelect then lastName = nil elseif name then lastName = name end
end

for slot = 1, 32 do
  local window = "Chat" .. slot
  ui.window("Chat " .. slot, function()
    forgetOnCharacterChange()
    if not (API.session.UiReady or API.session.InGame)() then return end
    local win = chatWindow(slot)
    if not win then chats[slot] = nil; return end
    local c = state(slot)
    local tab = (win.tabs[win.tab] or {}).slot or slot
    if c.tab ~= tab then c.version, c.tab = -1, tab end
    update(slot, c)
    local title = win.windowTitle or (win.tells and "Tells" or ("Chat " .. slot))
    -- quieter while nobody is using it
    local busy = c.editing or ui.GetTime() - (c.hoverTime or -100) < 2
    -- a tell window flashes once, a ring in the accent colour fading out over a second, when a tell comes in
    local glow = nil
    if win.tells then
      -- (a window a tell has just opened flashes too: it is first seen with a tell in it)
      if (win.tellsIn or 0) > (c.tellsIn or 0) then c.flash = ui.GetTime() end
      c.tellsIn = win.tellsIn or 0
      local k = c.flash and 1 - (ui.GetTime() - c.flash) / FLASH or 0
      if k > 0 then
        glow = c.glow or {}
        glow[1], glow[2], glow[3], glow[4] = C.accent[1], C.accent[2], C.accent[3], k
        c.glow = glow
      end
    end
    -- (fit: sized by the default layout until the player resizes it)
    c.panelOpts.alpha, c.panelOpts.glow = (busy or glow) and 1 or 0.7, glow
    -- a tell window can be put away (its X closes it with its tabs, as the menu's Close Window)
    c.winOpts.onClose = win.tells and c.closeWindow or nil
    c.win = win
    c.body = c.body or function(x, y, w, h, s)
      if ui.IsMouseHoveringRect(x, y, x + w, y + h) then c.hoverTime = ui.GetTime() end
      c.editing = body(slot, c, c.win, x, y, w, h, s)
    end
    hud.Window(window, title, c.winOpts, c.body)
  end)
end
