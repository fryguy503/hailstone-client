-- Chat: replaces every game chat window (Chat1 .. Chat32) with a translucent panel: tabs along the top (the active one
-- underlined, unread ones marked), the output, and an input with the channel plain text goes to. It works as the
-- the Classic UI's chat did: the lines are the original output box's (hs.ChatText, STML), typed lines
-- go through the original window (hs.ChatSubmit: channel, tells, history, commands), Enter and the chat keys move
-- what the game's input holds here (hs.ChatTakeFocus), item links open the item display, Up / Down walk the
-- history, Tab / Shift+Tab on a /tell line step through the players to reply to (hs.ChatCycleReply). A player's
-- name in a line is a link: a click opens a tell to them (their tell window with Use Tell Windows, else
-- "/tell <name> " in the input). Right-click: the chat menu (windows, tabs, Always Chat Here, filters, default channel,
-- font, tell windows, timestamps, clear).
-- A button at the strip's right end folds the window into the strip (the output and the input go, the tabs stay), as
-- the combat ability window folds into its title bar; the frame's menu has the same (Roll up).
-- Unlocked (any frame's menu), a chat window is moved by dragging and resized from its corner.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local stml = dofile("lib/stml.lua")
local T = hud.T
local C = T.color
stml.LINK = C.chatLink -- (this copy of stml: chat links in the colour made for the chat's page)

local chats = {} -- by slot: { version, lines, input, focus, follow, links, tab, rename, renameOpen }

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
    c = { version = -1, lines = {}, input = "", focus = 0, follow = true, links = {} }
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
  local version, lines, reset, count = API.chat.Text(slot, c.version, true, ui.GetSetting("chat.timestamps") == "1", true)
  if version == c.version or not lines then return end
  c.version = version
  if reset then stml.UpdateLines(c, lines, C.chatText) else stml.AppendLines(c, lines, count, C.chatText) end
  c.lastLine = ui.GetTime()
end

-- ---------------------------------------------------------------- the chat menu (as classic's)
-- Filters: the chat window manager's channels by its menus (Filters, Chat Channel, Melee, Spells, and Hailstone's
-- Extended (server) with its submenus; hs.ChatFilterGroups): each goes to one window; a ticked one comes to this tab,
-- unticking sends it back to the main window. Show All is the menu's ALL (its own items only). A menu with no items
-- (Chat Channel before joining one) is left out.
local function filtersMenu(slot)
  local filters = API.chat.Filters(slot)
  if #filters == 0 then return end
  if not ui.BeginMenu("Filters") then return end
  local groups = API.chat.FilterGroups()
  local rootName = "Filters"
  for _, f in ipairs(filters) do
    if f.menu == 0 then rootName = f.group; break end
  end
  local function hasRows(id)
    for _, f in ipairs(filters) do if f.menu == id then return true end end
    for _, group in ipairs(groups) do
      if group.parent == id and hasRows(group.id) then return true end
    end
    return false
  end
  local function drawGroup(id, name)
    if ui.MenuItem("Show All##group" .. id) then API.chat.SetFilter(slot, name, true) end
    if ui.MenuItem("Hide All##group" .. id) then API.chat.SetFilter(slot, name, false) end
    ui.Separator()
    for _, f in ipairs(filters) do
      if f.menu == id and ui.MenuItem(f.name .. "##filter" .. f.id, f.on) then
        API.chat.SetFilter(slot, f.id, not f.on)
      end
    end
    for _, group in ipairs(groups) do
      if group.parent == id and hasRows(group.id) and ui.BeginMenu(group.name .. "##group" .. group.id) then
        drawGroup(group.id, group.name)
        ui.EndMenu()
      end
    end
  end
  drawGroup(0, rootName)
  ui.EndMenu()
end

local function hitModesMenu()
  if ui.BeginMenu("Hit Modes") then
    local modes = API.chat.HitModes()
    local function drawMode(h)
      local id = h.id
      if ui.BeginMenu(h.name .. "##hit" .. id) then
        for m, label in ipairs(h.modes) do
          if ui.MenuItem(label .. "##hitmode" .. id .. m, h.mode == m - 1) then API.chat.SetHitMode(id, m - 1) end
        end
        ui.EndMenu()
      end
    end
    for i, h in ipairs(modes) do if i ~= 7 then drawMode(h) end end
    if modes[7] then drawMode(modes[7]) end
    ui.EndMenu()
  end
end

local function menu(slot, c, w)
  if ui.BeginPopup("chatmenu") then
    if ui.MenuItem("New Chat Window") then API.chat.NewWindow() end
    if not w.tells and ui.MenuItem("New Tab") then API.chat.NewTab(slot) end
    if ui.MenuItem("Rename Tab...") then c.rename = w.title or ""; c.renameOpen = true end
    if #w.tabs > 1 and ui.MenuItem("Close Tab") then API.chat.CloseTab(slot, w.tab) end
    if not w.main and ui.MenuItem("Close Window") then API.chat.CloseWindow(slot) end
    -- (Enter and '/' start a line in this window, whichever was clicked last; the main window's by default)
    local _, _, _, _, here = API.chat.Info(slot)
    if ui.MenuItem("Always Chat Here", here) then API.chat.SetAlwaysHere(slot, not here) end
    ui.Separator()
    filtersMenu(slot)
    hitModesMenu()
    local _, _, current = API.chat.Info(slot)
    if not (w.tabs[w.tab] or {}).tell and ui.BeginMenu("Default Channel") then
      for i, name in ipairs(API.chat.DefaultChannels()) do
        if ui.MenuItem(name, name == current) then API.chat.SetDefault(slot, i - 1) end
      end
      ui.EndMenu()
    end
    local font = API.chat.Info(slot)
    if ui.BeginMenu("Font Size") then
      -- The chat font list (/chatfontsize 0..10), each by its height.
      for f = 0, 10 do
        if ui.MenuItem(string.format("%d  (%d px)##f%d", f, ui.ChatFontSize(f), f), f == font) then API.chat.SetFont(slot, f) end
      end
      ui.EndMenu()
    end
    local tells = API.chat.TellWindows()
    if ui.MenuItem("Use Tell Windows", tells) then API.chat.SetTellWindows(not tells) end
    -- Hailstone: each line's time, [hh:mm:ss], in every chat window
    local stamps = ui.GetSetting("chat.timestamps") == "1"
    if ui.MenuItem("Timestamps", stamps) then ui.SetSetting("chat.timestamps", stamps and "0" or "1") end
    ui.Separator()
    if ui.MenuItem("Clear Chat") then API.chat.Clear(slot) end
    ui.TextDisabled("Alt+right-click: frame options")
    ui.EndPopup()
  end
  if c.renameOpen then
    ui.OpenPopup("chatrename")
    c.renameOpen = false
  end
  if ui.BeginPopup("chatrename") then
    ui.Text("Tab title:")
    ui.SetNextItemWidth(200)
    local text, enter = ui.InputText("##title", c.rename or "", 64, ui.InputTextFlags.EnterReturnsTrue)
    c.rename = text
    if enter or ui.Button("OK") then
      if c.rename ~= "" then API.chat.SetTitle(slot, c.rename) end
      ui.CloseCurrentPopup()
    end
    ui.SameLine()
    if ui.Button("Cancel") then ui.CloseCurrentPopup() end
    ui.EndPopup()
  end
end

-- ---------------------------------------------------------------- drawing
local FLASH = 1.0 -- seconds a tell window's flash takes to fade

-- The tabs: text along the top, the one up underlined in the accent colour, one with unseen lines marked with a
-- dot. A click brings a tab up. `right`: the room kept free at the end (the close box).
local function tabs(slot, w, x, y, width, s, title, right)
  local list = w.tabs
  if #list == 0 then list = { { title = title } } end
  local tx = x + 10 * s
  local _, th = T.TextSize("Ag", T.font.small)
  for i, t in ipairs(list) do
    local label = (t.title and t.title ~= "") and t.title or title
    local tw = T.TextSize(label, T.font.small)
    local up = i == w.tab or #w.tabs == 0
    local x1 = tx + tw + 14 * s
    if x1 > x + width - (right or 6 * s) then break end
    local clicked = T.Hit("##tab" .. i, tx - 4 * s, y + 2 * s, x1 - 6 * s, y + HEADER * s)
    local hovered = ui.IsItemHovered()
    local col = up and C.text or (hovered and C.text or C.muted)
    T.Text(tx, y + (HEADER * s - th) / 2, label, col, T.font.small)
    if up then
      ui.DrawLine(tx, y + HEADER * s - 3 * s, tx + tw, y + HEADER * s - 3 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s)
    elseif t.unread then
      ui.DrawCircleFilled(tx + tw + 4 * s, y + (HEADER * s - th) / 2 + 2 * s, 2.5 * s, C.accent[1], C.accent[2], C.accent[3], 1)
    end
    if clicked and not up then API.chat.SelectTab(slot, i) end
    tx = x1
  end
end

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

local function body(slot, c, w, title, x, y, width, height, s)
  local font, active, channel = API.chat.Info(slot)
  font = math.max(0, math.min(10, font or 3)) -- the chat font list (/chatfontsize 0..10)
  local pad = 8 * s
  -- rolled up: the tab strip is the title bar the window folds into, and all that is left of it (the frame's own:
  -- this frame's, as the frame is drawn, so a click does not fold it before the frame is smaller)
  local rolled = c.winOpts.rolled
  local right = (w.tells and 26 * s or 0) + 22 * s -- (the room kept free at the end: the tell window's close box, the button)
  tabs(slot, w, x, y, width, s, title, right)
  collapseButton(slot, x + width - right, y, s, rolled)
  if rolled then return false end
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
  ui.BeginChild("output", width - 2 * pad + 4 * s, math.max(oh, 10), false)
  local atBottom = ui.GetScrollY() >= ui.GetScrollMaxY() - 2
  local lw, viewH = ui.GetContentRegionAvail()
  -- (the lines' wraps and heights are kept while this stays: the width and the font, measured, as a UI scale
  -- changes the font's size)
  local pw, ph = ui.CalcTextSize("Wg il")
  local probe = pw * 4096 + ph
  if c.keyW ~= lw or c.keyFont ~= probe then c.keyW, c.keyFont, c.key = lw, probe, lw .. ":" .. probe end
  stml.DrawLines(c, lw, viewH, c.key, c.onLink)
  local page = API.chat.Page(slot) -- the PAGEUP / PAGEDOWN keys: a page less 8 up or down
  if page then
    ui.SetScrollY(math.max(0, ui.GetScrollY() + page * (viewH - 8 * s)))
  elseif atBottom or c.follow then
    ui.SetScrollHereY(1.0)
  end
  c.follow = false
  ui.EndChild()
  ui.PopStyleColor()

  -- the input: the channel plain text goes to, then the field
  local iy = y + height - INPUT * s - pad
  local ix0, ix1 = x + pad, x + width - pad
  local chip = (w.tabs[w.tab] or {}).tell and "Tell" or (channel or "Say")
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

  if ui.IsWindowHovered(ui.HoveredFlags.ChildWindows) then
    if ui.IsMouseClicked(ui.Mouse.Left) and not active then API.chat.Activate(slot) end
    if ui.IsMouseClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      if API.menus.Contexts() then ui.OpenPopup("chatmenu") end
      T.TakeRightClick()
    end
  end
  menu(slot, c, w)
  return editing
end

-- The chat windows by slot, asked for once a frame: each of the 32 windows below used to build the whole list
-- (a table per window and per tab) every frame, though only a few exist.
local listedFrame, bySlot = -1, {}
local function chatWindow(slot)
  local frame = ui.GetFrameCount()
  if frame ~= listedFrame then
    listedFrame, bySlot = frame, {}
    for _, w in ipairs(API.chat.Windows()) do bySlot[w.slot] = w end
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
    if c.tab ~= win.tab then c.version = -1; c.follow = true; c.tab = win.tab end -- another tab's lines
    update(slot, c)
    local title = win.tells and "Tells" or ((win.title and win.title ~= "") and win.title or ("Chat " .. slot))
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
    c.win, c.title = win, title
    c.body = c.body or function(x, y, w, h, s)
      if ui.IsMouseHoveringRect(x, y, x + w, y + h) then c.hoverTime = ui.GetTime() end
      c.editing = body(slot, c, c.win, c.title, x, y, w, h, s)
    end
    hud.Window(window, title, c.winOpts, c.body)
  end)
end
