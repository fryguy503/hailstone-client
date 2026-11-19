-- Chat (gamepad pack): stands in for the chat windows (Chat1: the C++ chat stays off).
--   Chat ticker    the newest lines of the main chat window, bottom left of the HUD (read only, fades when quiet)
--   Chat Log       from the main menu (or Back + Start, or the chat keys: Enter, / ...): the chat windows' lines,
--                  read only, and ways to talk without a keyboard: quick-chat phrases and an on-screen keyboard.
--                  LB / RB pick the chat window, D-pad up / down scroll the log (on the log), X changes the channel.
-- What is sent goes through the stock chat window (hs.ChatSubmit): its history and the game's command interpreter.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local S = dofile("lib/stml.lua")
local K = dofile("lib/keyboard.lua")
local chatlinks = dofile("lib/chatlinks.lua")
local C = T.C
S.LINK = C.chatLink -- (this copy of stml: chat links in the colour made for the chat's page)

local cache = {} -- by slot: { version, lines = { segments } }
-- The window's lines, parsed; only the lines that came since the last look are asked for and parsed (hs.ChatText's
-- incremental form), unless the window's lines changed otherwise.
local function lines(slot)
  local c = cache[slot]
  if not c then
    c = { version = -1, lines = {} }
    cache[slot] = c
  end
  local version, raw, reset, count = API.chat.Text(slot, c.version, false, false, true)
  if version and raw and version ~= c.version then
    c.version, c.changed = version, ui.GetTime()
    if reset then c.lines = {} end
    local l = c.lines
    local n = #l
    for _, r in ipairs(raw) do
      n = n + 1
      l[n] = S.parse(r, C.chatText)
    end
    local drop = n - math.max(count or n, 0)
    if drop > 0 then
      table.move(l, drop + 1, n, 1)
      for i = n - drop + 1, n do l[i] = nil end
    end
  end
  return c
end

-- The keyboard's button ids, made once (not each frame).
local KEY_IDS = {}
for r, row in ipairs(K.KEYS) do
  KEY_IDS[r] = {}
  for k in ipairs(row) do KEY_IDS[r][k] = "##k" .. r .. "_" .. k end
end
local NO_OPTS = {} -- (T.button only reads its options)
local tickerRows = {}

-- The chat keys (Enter, '/', ...): the text they start with; opens the log with the keyboard on its input.
local typed = nil
local input, inputLinks, inputError = "", {}, nil
local slotIdx = 1

-- The chat windows, read once a frame (the ticker, the log and a send each want them).
local windowsFrame, windowsList = nil, nil
local function chatWindows()
  local f = ui.GetFrameCount()
  if f ~= windowsFrame or not windowsList then windowsFrame, windowsList = f, API.chat.Windows() end
  return windowsList
end
-- The input with its links' tags put back, worked out again only when the input or its links change (the ticker
-- offers it to the chat keys every frame). The links are only ever added to, or replaced by a new table.
local expandedText, expandedFor, expandedLinks, expandedCount = "", "", nil, 0
local function expandedInput()
  if input ~= expandedFor or not rawequal(inputLinks, expandedLinks) or #inputLinks ~= expandedCount then
    expandedFor, expandedLinks, expandedCount = input, inputLinks, #inputLinks
    expandedText = chatlinks.Expand(inputLinks, input)
  end
  return expandedText
end

-- ---------------------------------------------------------------- ticker
ui.window("Chat Ticker", function()
  if not API.session.InGame() then input, inputLinks, inputError, typed = "", {}, nil, nil; return end
  local shown = W.shown("Chat1")
  local main = 1
  local windows = chatWindows()
  for _, window in ipairs(windows) do
    if window.main then main = window.slot; break end
  end
  if main ~= 1 then shown = W.shown("Chat" .. main) end
  if not shown then return end
  local start, replace
  local text = expandedInput()
  for index, window in ipairs(windows) do
    start, replace = API.chat.TakeFocus(window.slot, text)
    if start then slotIdx = index; break end
  end
  if start then
    typed = { text = start, replace = replace }
    GPSTATE.chatOpen = true
    -- the pad's Back + Start (its "chat" binding presses the chat key) opens the on-screen keyboard, not a typed line
    GPSTATE.chatByKeyboard = not (pad.down("back") and pad.down("start"))
    GPSTATE.chatOpenedFrame = ui.GetFrameCount()
  end
  if GPSTATE.chatOpen then return end
  local c = lines(main)
  local n = #c.lines
  if n == 0 then return end
  T.frame()
  T.resetFonts()
  local w, h = T.px(640), T.px(250)
  local bottom = T.H - T.px(90)
  -- clear of the cross hotbar: on a narrow screen it comes up over the bar's left end (under the pet, if any)
  local bar, now = GPSTATE.crossBar, ui.GetTime()
  if bar and now - bar.at < 0.5 and T.px(24) + w > bar.x0 - T.px(12) then
    bottom = bar.y0 - T.px(12)
    local pet = GPSTATE.hudPet
    local top = (pet and now - pet.at < 0.5) and pet.y1 + T.px(12) or bottom - h
    h = math.max(T.px(100), math.min(h, bottom - top))
  end
  ui.SetNextWindowPos(T.px(24), bottom - h, ui.Cond.Always)
  ui.SetNextWindowSize(w, h, ui.Cond.Always)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
  -- a click on it opens the chat with the keyboard on its input (the pad's navigation passes it by)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoBackground | ui.WindowFlags.NoSavedSettings
      | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoBringToFrontOnFocus | W.NoNav
  if ui.Begin("Chat Ticker", nil, flags) then
    local x0, y0 = ui.GetWindowPos()
    ui.SetCursorPos(0, 0)
    if ui.InvisibleButton("##ticker", w, h) then
      typed = { text = "", replace = true }
      GPSTATE.chatOpen = true
      GPSTATE.chatByKeyboard = true
    end
    local quiet = ui.GetTime() - (c.changed or 0)
    local alpha = quiet < 15 and 1 or math.max(0.35, 1 - (quiet - 15) / 10)
    T.font(19)
    local _, lh = ui.CalcTextSize("Ag")
    -- the newest rows, wrapped, from the bottom up; the panel only as tall as they are
    local all, na = tickerRows, 0
    local font = S.fontKey()
    for i = n, math.max(1, n - 12), -1 do
      local rows = S.rows(c.lines[i], w - T.px(28), font)
      for r = #rows, 1, -1 do na = na + 1; all[na] = rows[r] end
      if na * lh > h then break end
    end
    for i = #all, na + 1, -1 do all[i] = nil end
    local used = math.min(h - T.px(12), #all * lh)
    T.rect(x0, y0 + h - used - T.px(20), x0 + w, y0 + h, C.chatBg, alpha, T.px(14))
    local y = y0 + h - T.px(10)
    ui.PushClipRect(x0, y0 + T.px(6), x0 + w, y0 + h, true)
    for _, row in ipairs(all) do
      y = y - lh
      S.drawRow(row, x0 + T.px(14), y, alpha)
      if y < y0 then break end
    end
    ui.PopClipRect()
    T.unfont()
  end
  ui.End()
  ui.PopStyleVar(2)
end)

-- ---------------------------------------------------------------- the log, quick chat and keyboard
local CHANNELS = { { "Say", "/say " }, { "Group", "/gsay " }, { "Guild", "/guildsay " }, { "Raid", "/rsay " },
  { "OOC", "/ooc " }, { "Shout", "/shout " }, { "Auction", "/auction " } }
local PHRASES = { "Hail", "Hello!", "Thanks!", "Ready", "Not ready", "Incoming!", "Help!", "Follow me", "On my way",
  "Need a break", "Back", "Looking for group", "Yes", "No", "Good fight!", "Bye!" }
-- Commands that need the rest typed: the keyboard opens with the text (/safelock: the locked interface leaves the pad
-- only this window, so its password is typed here).
local STARTS = { { "/safelock ...", "/safelock " } }
local channel = 1
local scroll = 0 -- lines up from the newest
local mode = "quick" -- "quick" or "keys"
local shift = false
-- The pad's focus goes to the first phrase or key: when the chat opens with the pad, and when Y switches between
-- the two (the focused button is gone then).
local focusFirst = false
local chatDrawn = -10
local function focusHere()
  if focusFirst then
    ui.SetKeyboardFocusHere()
    pad.showCursor()
    focusFirst = false
  end
end

local function send(text, composed)
  if text == "" then return true end
  local windows = chatWindows()
  local slot = windows[slotIdx] and windows[slotIdx].slot or 1
  local prefix = text:sub(1, 1) == "/" and "" or CHANNELS[channel][2]
  local expanded, reason = chatlinks.ForSubmit(composed and inputLinks or {}, text, prefix)
  if not expanded then inputError = reason; return false end
  API.chat.Submit(slot, expanded)
  inputError = nil
  return true
end

ui.window("Chat Log", function()
  if not API.session.InGame() then input, inputLinks, inputError, typed = "", {}, nil, nil; return end
  -- Back + Start: the chat input (here, the keyboard)
  -- (in game mode the client's own back+start binding opens it, through the chat key: the ticker above, this frame)
  if ((pad.down("back") and pad.pressed("start")) or (pad.down("start") and pad.pressed("back")))
      and GPSTATE.chatOpenedFrame ~= ui.GetFrameCount() then
    GPSTATE.chatOpen = not GPSTATE.chatOpen
    GPSTATE.chatByKeyboard = false
    if GPSTATE.chatOpen then mode = "keys" end
    GPSTATE.menuOpen = false
  end
  -- another window's tell or reply (W.chat): the keyboard with that text, the pad's focus on the window
  if GPSTATE.chatText then
    inputLinks = {}
    input, mode, GPSTATE.chatText = chatlinks.Collapse(inputLinks, GPSTATE.chatText), "keys", nil
    inputError = nil
    GPSTATE.chatOpen, GPSTATE.chatByKeyboard = true, false
  end
  if not GPSTATE.chatOpen then return end
  -- opened (from the menu, Back + Start, ...) for the pad: its focus on the first phrase or key, not the log
  local f = ui.GetFrameCount()
  if f - chatDrawn > 1 and not GPSTATE.chatByKeyboard then focusFirst = true end
  chatDrawn = f
  if typed then
    if typed.replace then input, inputLinks = "", {} end
    input = input .. chatlinks.Collapse(inputLinks, typed.text, #input)
    mode, typed, inputError = "keys", nil, nil
    GPSTATE.chatFocusInput = GPSTATE.chatByKeyboard -- (the pad uses the on-screen keys)
  end
  local windows = chatWindows()
  if slotIdx > #windows then slotIdx = 1 end
  local win = windows[slotIdx]
  local slot = win and win.slot or 1
  if W.begin("Chat Log", { w = 1600, h = 960, title = "Chat", subtitle = win and win.title or "",
        hints = { { "lb", "Window" }, { "rb", "Window" }, { "x", "Channel: " .. CHANNELS[channel][1] },
          { "y", mode == "quick" and "Keyboard" or "Quick chat" }, { "b", "Close" } } }) then
    if #windows > 1 then
      if W.pressed("lb") then slotIdx = (slotIdx - 2) % #windows + 1 scroll = 0 end
      if W.pressed("rb") then slotIdx = slotIdx % #windows + 1 scroll = 0 end
    end
    if W.pressed("x") then channel = channel % #CHANNELS + 1 end
    if W.pressed("y") then
      mode = mode == "quick" and "keys" or "quick"
      focusFirst = true
    end
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local logX1 = x0 + (x1 - x0) * 0.55
    -- the log: a focusable panel (D-pad up / down scrolls while it has the focus)
    ui.SetCursorPos(x0 - wx, y0 - wy)
    ui.InvisibleButton("##log", logX1 - x0, y1 - y0, ui.ButtonFlags.EnableNav)
    local logFocused = pad.focused()
    if logFocused then
      if pad.menuPressed("up") then scroll = scroll + 3 end
      if pad.menuPressed("down") then scroll = math.max(0, scroll - 3) end
    end
    if ui.IsItemHovered() then
      local wheel = ui.GetIO().wheel or 0
      if wheel ~= 0 then scroll = math.max(0, scroll + math.floor(wheel * 3 + 0.5)) end
    end
    T.rect(x0, y0, logX1, y1, C.sunken, 1, T.px(14))
    T.rect(x0, y0, logX1, y1, C.chatBg, 1, T.px(14)) -- (the chat's page: its user colours are made for a dark one)
    if logFocused then T.focusRing(x0, y0, logX1, y1, T.px(14)) end
    local c = lines(slot)
    T.font(21)
    local _, lh = ui.CalcTextSize("Ag")
    ui.PushClipRect(x0 + T.px(8), y0 + T.px(8), logX1 - T.px(8), y1 - T.px(8), true)
    local y = y1 - T.px(14)
    local n = #c.lines
    scroll = math.min(scroll, math.max(0, n - 1))
    local font = S.fontKey()
    for i = n - scroll, 1, -1 do
      local rows = S.rows(c.lines[i], logX1 - x0 - T.px(32), font)
      for r = #rows, 1, -1 do
        y = y - lh
        S.drawRow(rows[r], x0 + T.px(16), y)
      end
      if y < y0 then break end
    end
    ui.PopClipRect()
    if scroll > 0 then T.textIn(x0, y0 + T.px(8), logX1 - T.px(16), y0 + T.px(8) + lh, scroll .. " lines up", C.dim, 1) end
    T.unfont()
    -- the right side: what to say
    local rx0 = logX1 + T.px(24)
    local rw = x1 - rx0
    ui.SetCursorPos(rx0 - wx, y0 - wy)
    T.font(20)
    T.draw(rx0, y0, "To: " .. CHANNELS[channel][1], C.accent)
    T.unfont()
    ui.SetCursorPos(rx0 - wx, y0 + T.px(40) - wy)
    if mode == "quick" then
      local cols = 2
      local bw = (rw - T.px(10)) / cols
      for i, p in ipairs(PHRASES) do
        if (i - 1) % cols ~= 0 then ui.SameLine(0, T.px(10)) else ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) - wy) end
        if i == 1 then focusHere() end
        if T.button("##q" .. i, p, bw, T.px(56), {}, pad) then
          send(p)
        end
      end
      for i, c in ipairs(STARTS) do
        if (#PHRASES + i - 1) % cols ~= 0 then ui.SameLine(0, T.px(10)) else ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) - wy) end
        if T.button("##qs" .. i, c[1], bw, T.px(56), {}, pad) then
          input, mode, focusFirst = c[2], "keys", true
          inputLinks, inputError = {}, nil
        end
      end
    else
      -- the typed line (a keyboard types into it too)
      ui.SetNextItemWidth(rw)
      if GPSTATE.chatFocusInput then
        ui.SetKeyboardFocusHere()
        GPSTATE.chatFocusInput = false
      end
      local text, entered = ui.InputText("##chatinput", input, 2048, ui.InputTextFlags.EnterReturnsTrue, { moveToEnd = true })
      if text ~= input then inputError = nil end
      input = text
      if input == "" then inputLinks = {} end
      if entered then
        if send(text, true) then
          input, inputLinks = "", {}
          -- Enter sends and closes only after an intact line was accepted.
          if GPSTATE.chatByKeyboard then GPSTATE.chatOpen = false end
        end
      end
      if inputError then ui.TextWrapped(inputError) end
      ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
      local kw = (rw - 9 * T.px(8)) / 10
      for r, row in ipairs(K.KEYS) do
        ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
        for k, key in ipairs(row) do
          if k > 1 then ui.SameLine(0, T.px(8)) end
          local label = K.label(key, shift)
          if r == 2 and k == 1 then focusHere() end -- (q)
          if T.button(KEY_IDS[r][k], label, kw, T.px(60), NO_OPTS, pad) then
            input = input .. label
            shift = false
          end
        end
      end
      ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
      local third = (rw - 3 * T.px(8)) / 4
      if T.button("##shift", "Shift", third, T.px(60), { primary = shift }, pad) then shift = not shift end
      ui.SameLine(0, T.px(8))
      if T.button("##space", "Space", third, T.px(60), {}, pad) then input = input .. " " end
      ui.SameLine(0, T.px(8))
      if T.button("##back", "Delete", third, T.px(60), {}, pad) then input = T.dropChar(input) end
      ui.SameLine(0, T.px(8))
      if T.button("##send", "Send", third, T.px(60), { primary = true }, pad) then
        if send(input, true) then input, inputLinks = "", {} end
      end
    end
  end
  -- Escape closes it too while its input has the keyboard (the game does not see that key then)
  if GPSTATE.chatOpen and ui.IsKeyPressed(ui.Key.Escape) then GPSTATE.chatOpen = false end
  if W.finish() then GPSTATE.chatOpen = false end
  if not GPSTATE.chatOpen then GPSTATE.chatByKeyboard = false end
end)
