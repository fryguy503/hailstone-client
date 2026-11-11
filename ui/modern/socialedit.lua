-- Edit social: replaces the game's SocialEditWnd (a right click on a social's hot button or Actions window button).
-- The boxes, colours and buttons are the window's (app/lua_eq_socialedit.cpp: eq.SetWindowText, eq.ClickChild), so
-- what is saved is the game's: a name of 15 letters, a colour, five lines of 255.
--
-- Laid out for making macros: the button as it will look, the social's place and whether it has unsaved changes, a
-- Commands / Lua switch (Lua saves the lines and opens them in the Lua social editor, luasocial.lua, as mq.cmd
-- calls). Each line shows what it is (a game command, chat, a server # command, a command the game does not know)
-- and has a wait box (its "/pause N", in seconds) and buttons to move it up or down or empty it; the cooldown box is
-- the social's "/timer N". The lines' parts are the press's own (eq.SocialLine, lib/social_lines.lua), and a
-- timeline under them shows when each line runs. Tab in a command box completes the game's slash commands (again:
-- the next match; a list of them shows while typing; eq.SlashCommands). Examples fill in a whole social.
-- The toolbar: Test (saves, then presses the social as its hot button does: the commands are really sent; "running"
-- and Stop while it runs), Put on bar (and dragging the preview: the social on the cursor, eq.SocialToCursor), Copy
-- and Paste share text (eq.LuaSocialExport / LuaSocialImport; a pasted Lua social goes to the Lua editor unsaved,
-- through the global hailstone_social_draft). Ctrl+Enter accepts.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local S = dofile("lib/social.lua")
local SL = dofile("lib/social_lines.lua")
local T = hud.T
local C = T.color
local WINDOW = "SocialEditWnd"
local MANAGED = ui.ManagedDistribution and ui.ManagedDistribution() or false
local NAME_MAX = 15
local LINE_MAX = 255
local HINTS = { "First command, e.g. /assist", "e.g. /attack on", "e.g. /pet attack", "e.g. /g Incoming!",
  "e.g. /stand" }

local EXAMPLES = {
  { "Assist and attack", "Assist", { "/assist", "/attack on" } },
  { "Pet attack (my target)", "Pet Attack", { "/pet attack" } },
  { "Assist, pet and me", "Engage", { "/assist", "/pause 5, /pet attack", "/attack on" } },
  { "Sit and tell the group", "Med", { "/sit", "/g Medding, back soon" } },
  { "Stop fighting", "Back Off", { "/attack off", "/pet back off" } },
  { "Camp out", "Camp", { "/sit", "/camp" } },
}

local function near(a, b) return math.abs(a - b) < 0.01 end
local function lineH() return select(2, ui.CalcTextSize("Ag")) end
local function lineId(i) return "SEW_Line" .. (i - 1) .. "Input" end

-- What a line is, for its badge: colour, tooltip.
local function describe(p)
  if p.raw == "" then return C.faint, "An empty line: skipped." end
  if p.command == "" then return C.muted, "Only waits (or starts the cooldown): sends nothing." end
  if p.kind == "command" then return C.accent, "Runs " .. (p.name or "a game command") .. "." end
  if p.kind == "chatserver" then return C.link, "Sent to the chat server." end
  if p.kind == "invalid" then
    return C.gold, "Not one of the game's commands. A plugin or script may take it; otherwise the game answers " ..
        "that it is not a valid command."
  end
  if p.command:match("^%s*#") then return C.good, "Sent as chat: the server reads it as a # command." end
  return C.good, "Said in your current chat channel."
end

-- "1.5" for 15 steps; "" for none.
local function seconds(n)
  if not n then return "" end
  local v = n / 10
  return v == math.floor(v) and string.format("%d", v) or string.format("%.1f", v)
end
-- Steps (tenths) for typed seconds, 1..max; nil for none.
local function steps(text, max)
  local v = tonumber(text)
  if not v or v <= 0 then return nil end
  return math.max(1, math.min(max, math.floor(v * 10 + 0.5)))
end

-- A small icon button drawn over an invisible one: "up", "down" or "clear".
local function iconButton(id, kind, size, s, enabled, tooltip)
  local x, y = ui.GetCursorScreenPos()
  ui.BeginDisabled(not enabled)
  local clicked = ui.InvisibleButton(id, size, size)
  ui.EndDisabled()
  local hovered = enabled and ui.IsItemHovered()
  local c = enabled and (hovered and C.text or C.muted) or C.faint
  if hovered then ui.DrawRectFilled(x, y, x + size, y + size, C.text[1], C.text[2], C.text[3], 0.08, 4 * s) end
  local cx, cy, r = x + size / 2, y + size / 2, size * 0.22
  if kind == "up" then
    ui.DrawTriangleFilled(cx - r, cy + r * 0.6, cx + r, cy + r * 0.6, cx, cy - r * 0.7, c[1], c[2], c[3], 1)
  elseif kind == "down" then
    ui.DrawTriangleFilled(cx - r, cy - r * 0.6, cx + r, cy - r * 0.6, cx, cy + r * 0.7, c[1], c[2], c[3], 1)
  else
    ui.DrawLine(cx - r, cy - r, cx + r, cy + r, c[1], c[2], c[3], 1, 1.6 * s)
    ui.DrawLine(cx - r, cy + r, cx + r, cy - r, c[1], c[2], c[3], 1, 1.6 * s)
  end
  if tooltip and ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then ui.SetTooltip(tooltip) end
  return clicked and enabled
end

-- A button in a role's colours ("primary", "danger" or nil).
local function roleButton(label, role, w, h, enabled)
  local base = role == "primary" and C.accent or role == "danger" and C.danger or nil
  if base then
    ui.PushStyleColor(ui.Col.Button, base[1], base[2], base[3], 0.28)
    ui.PushStyleColor(ui.Col.ButtonHovered, base[1], base[2], base[3], 0.45)
    ui.PushStyleColor(ui.Col.ButtonActive, base[1], base[2], base[3], 0.60)
  end
  ui.BeginDisabled(enabled == false)
  local clicked = ui.Button(label, w, h)
  ui.EndDisabled()
  if base then ui.PopStyleColor(3) end
  return clicked and enabled ~= false
end

-- The social as a hot button shows it: its name in its colour.
local function preview(name, r, g, b, bw, bh, s)
  local x, y = ui.GetCursorScreenPos()
  ui.DrawRectGradient(x, y, x + bw, y + bh, C.panelTop[1], C.panelTop[2], C.panelTop[3], 1,
    C.panel[1], C.panel[2], C.panel[3], 1)
  ui.DrawRect(x, y, x + bw, y + bh, C.borderHi[1], C.borderHi[2], C.borderHi[3], C.borderHi[4] or 1, 6 * s, 1)
  local label = name ~= "" and T.Fit(name, bw - 8 * s, T.font.small) or "(no name)"
  local tw, th = T.TextSize(label, T.font.small)
  local col = name ~= "" and { r or C.text[1], g or C.text[2], b or C.text[3], 1 } or C.faint
  T.Text(x + (bw - tw) / 2, y + (bh - th) / 2, label, col, T.font.small)
  ui.InvisibleButton("##sewpreview", bw, bh)
end

-- A tinted note across the width.
local function note(text, color, s)
  local x, y = ui.GetCursorScreenPos()
  local aw = ui.GetContentRegionAvail()
  local pad = 7 * s
  ui.PushFont(T.font.small)
  ui.SetCursorScreenPos(x + pad + 3 * s, y + pad * 0.6)
  ui.PushTextWrapPos((ui.GetCursorPos()) + aw - 2 * pad - 3 * s)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, text)
  ui.PopTextWrapPos()
  ui.PopFont()
  local _, y1 = ui.GetCursorScreenPos()
  y1 = y1 + pad * 0.4
  ui.DrawRectFilled(x, y, x + aw, y1, color[1], color[2], color[3], 0.12, 5 * s)
  ui.DrawRectFilled(x, y, x + 3 * s, y1, color[1], color[2], color[3], 0.9, 2 * s)
  ui.SetCursorScreenPos(x, y1 + 4 * s)
  ui.Dummy(0, 0)
end

-- The lines' timeline: a track with a numbered mark where each line runs; hovering one shows its command.
local function timeline(lines, tl, s)
  local x, y = ui.GetCursorScreenPos()
  local aw = ui.GetContentRegionAvail()
  local h = 20 * s
  local pad = 10 * s
  local total = math.max(tl.total, 0.2)
  local mid = y + h / 2
  ui.DrawLine(x + pad, mid, x + aw - pad, mid, C.border[1], C.border[2], C.border[3], C.border[4] or 1, 2 * s)
  local mx, my = ui.GetMousePos()
  local tip = nil
  for i = 1, #lines do
    local t = tl.at[i]
    if t then
      local p = SL.Parts(lines[i])
      local col = describe(p)
      local px = x + pad + (aw - 2 * pad) * (t / total)
      local r = 7 * s
      ui.DrawCircleFilled(px, mid, r, col[1], col[2], col[3], 1)
      local label = tostring(i)
      local tw, th = T.TextSize(label, T.font.tiny)
      T.Text(px - tw / 2, mid - th / 2, label, C.onAccent, T.font.tiny)
      if math.abs(mx - px) <= r + 2 * s and math.abs(my - mid) <= r + 2 * s then
        tip = string.format("Line %d at %.1f s: %s", i, t, p.command ~= "" and p.command or "(waits only)")
      end
    end
  end
  ui.Dummy(aw, h)
  if tip and ui.IsItemHovered() then ui.SetTooltip(tip) end
end

-- The first line with a cooldown, or nil.
local function cooldownLine(lines)
  for i = 1, #lines do
    if SL.Parts(lines[i]).timer then return i end
  end
  return nil
end

local function setLine(i, text) eq.SetWindowText(WINDOW, lineId(i), text:sub(1, LINE_MAX)) end

local function swap(lines, a, b)
  setLine(a, lines[b])
  setLine(b, lines[a])
end

-- The prefixes a command box had when typing in it began: while it is active ImGui keeps its own text, which may
-- hold a "/pause N, " typed by hand, so the line is put together from those, not from what was saved last frame.
local typing = {}

-- Tab completion of the command word: each Tab the next of the game's commands that start with what was typed
-- (Shift+Tab the one before); a single match gets a space after it.
local comp = {}     -- per line: {list, i, last}
local inputOpts = {} -- per line: ui.InputText's options (made once)
local function completer(i)
  return function(text, back)
    local lead, word = text:match("^(%s*)(/%S*)$")
    if not word then return nil end
    local c = comp[i]
    if c and c.last == text and #c.list > 1 then
      c.i = back and ((c.i - 2) % #c.list + 1) or (c.i % #c.list + 1)
    else
      local list = eq.SlashCommands(word)
      if not list or #list == 0 then return nil end
      c = { list = list, i = 1 }
      comp[i] = c
    end
    local out = lead .. c.list[c.i] .. (#c.list == 1 and " " or "")
    c.last = out
    return out
  end
end

-- The commands the word being typed may become, under its box (at most 8; the one Tab gave lit).
local matches = { word = nil, list = {} }
local function suggestions(i, text, x0, y1, s)
  local word = text:match("^%s*(/%S+)$")
  if not word then return end
  if matches.word ~= word then matches.word, matches.list = word, eq.SlashCommands(word) or {} end
  local list = matches.list
  if #list == 0 or (#list == 1 and list[1]:lower() == word:lower()) then return end
  local c = comp[i]
  local lit = c and c.last == text and c.list[c.i] or nil
  ui.SetNextWindowPos(x0, y1 + 2 * s, ui.Cond.Always)
  ui.BeginTooltip()
  for k = 1, math.min(8, #list) do
    local col = list[k] == lit and C.accent or (k == 1 and C.text or C.muted)
    ui.TextColored(col[1], col[2], col[3], 1, list[k])
  end
  if #list > 8 then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, string.format("... %d more", #list - 8)) end
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Tab: complete (again: the next)")
  ui.EndTooltip()
end

local function commandRow(i, lines, s, fh)
  local p = SL.Parts(lines[i])
  local col, tip = describe(p)
  ui.PushID("sewl" .. i)
  -- the badge: the line's number in what it is
  local x, y = ui.GetCursorScreenPos()
  local bw = 24 * s
  ui.InvisibleButton("##badge", bw, fh)
  if ui.IsItemHovered() then ui.SetTooltip(tip) end
  ui.DrawRectFilled(x, y + 2 * s, x + bw - 5 * s, y + fh - 2 * s, col[1], col[2], col[3], p.raw == "" and 0.10 or 0.24,
    4 * s)
  local label = tostring(i)
  local tw, th = T.TextSize(label, T.font.small)
  T.Text(x + (bw - 5 * s - tw) / 2, y + (fh - th) / 2, label, p.raw == "" and C.faint or col, T.font.small)
  -- the command: the line without its /pause and /timer
  ui.SameLine(0, 0)
  local icon = fh * 0.85
  local waitW = 56 * s
  local aw = ui.GetContentRegionAvail()
  ui.SetNextItemWidth(math.max(80 * s, aw - waitW - 3 * icon - 10 * s))
  local prefixLen = #SL.Compose(p.timer, p.pause, "x") - 1
  local held = typing[i] or p
  inputOpts[i] = inputOpts[i] or { tab = completer(i) }
  local cmd, changed = ui.InputText("##cmd", p.command, LINE_MAX + 1 - prefixLen, 0, inputOpts[i])
  if changed then setLine(i, SL.Compose(held.timer, held.pause, cmd)) end
  local active = ui.IsItemActive()
  local ix0, iy0, _, iy1 = ui.GetItemRect()
  if active then
    typing[i] = typing[i] or { timer = p.timer, pause = p.pause }
    suggestions(i, cmd, ix0, iy1, s)
  else
    typing[i] = nil
    if cmd == "" then -- the hint (ui.InputText has none)
      local f = C.faint
      ui.DrawText(ix0 + 5 * s, iy0 + (iy1 - iy0 - lineH()) / 2, f[1], f[2], f[3], f[4] or 1, HINTS[i] or "")
    end
  end
  if ui.IsItemHovered() and p.raw ~= "" then
    local extra = (p.timer or p.pause) and ("\nThe whole line: " .. p.raw) or ""
    ui.SetTooltip(string.format("%s\n%d of %d characters%s", tip, #p.raw, LINE_MAX, extra))
  end
  -- the wait after it
  ui.SameLine(0, 4 * s)
  ui.SetNextItemWidth(waitW)
  local wtext, wchanged = ui.InputTextWithHint("##wait", p.raw ~= "" and "wait s" or "", seconds(p.pause), 8,
    ui.InputTextFlags.CharsDecimal)
  if wchanged then setLine(i, SL.Compose(p.timer, steps(wtext, 600), p.command)) end
  if ui.IsItemHovered() then
    ui.SetTooltip("Seconds to wait after this line (/pause; at most 60). Empty: the next line follows a tenth " ..
      "of a second later.")
  end
  -- move and empty
  ui.SameLine(0, 4 * s)
  local last = #lines
  if iconButton("##up", "up", icon, s, i > 1 and (lines[i] ~= "" or lines[i - 1] ~= ""), "Move up") then
    swap(lines, i, i - 1)
  end
  ui.SameLine(0, 0)
  if iconButton("##down", "down", icon, s, i < last and (lines[i] ~= "" or lines[i + 1] ~= ""), "Move down") then
    swap(lines, i, i + 1)
  end
  ui.SameLine(0, 0)
  if iconButton("##clear", "clear", icon, s, lines[i] ~= "", "Empty this line") then setLine(i, "") end
  ui.PopID()
end

-- The social's cooldown: the first /timer of its lines; a new one goes on the first line that runs.
local function cooldownRow(lines, s)
  local ci = cooldownLine(lines)
  local current = ci and SL.Parts(lines[ci]).timer or nil
  K.Caption("Cooldown")
  ui.SameLine()
  ui.SetNextItemWidth(64 * s)
  local text, changed = ui.InputTextWithHint("##sewcool", "none", seconds(current), 10, ui.InputTextFlags.CharsDecimal)
  if ui.IsItemHovered() then
    ui.SetTooltip("Seconds the button stays dimmed after a press (/timer; at most 10 hours). It starts when its " ..
      "line runs" .. (ci and string.format(" (line %d)", ci) or "") .. ".")
  end
  ui.SameLine()
  K.Caption("s")
  if not changed then return end
  local v = steps(text, 360000)
  local at = ci
  if not at then
    at = 1
    for i = 1, #lines do if lines[i] ~= "" then at = i break end end
  end
  local p = SL.Parts(lines[at])
  if v or ci then setLine(at, SL.Compose(v, p.pause, p.command)) end
end

-- A short message in the toolbar, for a few seconds.
local flash = { text = "", at = -100 }
local function say(text) flash.text, flash.at = text, ui.GetTime() end

local dragged = false
local pickUp, toolbar

-- Saved (when changed: Accept, which closes the editor) and on the cursor, to drop on a hot button.
pickUp = function(n, dirty)
  if dirty then eq.ClickChild(WINDOW, "SEW_Accept_Button", "left") end
  eq.SocialToCursor(n)
end

-- Test, Put on bar, Copy, Paste, and "running" with Stop. True when the editor gave way to another.
toolbar = function(n, name, color, lines, anyLine, dirty, s)
  local named = n ~= nil and name ~= ""
  ui.BeginDisabled(not (named and anyLine))
  if ui.SmallButton("Test##sewtest") then
    if dirty then
      eq.ClickChild(WINDOW, "SEW_Accept_Button", "left")
      eq.OpenSocialEditor(n) -- (Accept closed it: back on the social, as saved)
    end
    eq.PressSocial(n)
    say(dirty and "Saved and pressed." or "Pressed.")
  end
  if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
    ui.SetTooltip("Save the social and press it, as its hot button would: the commands are really sent")
  end
  ui.SameLine()
  if ui.SmallButton("Put on bar##sewcursor") then pickUp(n, dirty) end
  if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
    ui.SetTooltip("Pick the social up (saving it), then click a hot button to place it. Or drag the button above")
  end
  ui.EndDisabled()
  if not MANAGED then
    ui.SameLine()
    ui.BeginDisabled(not named)
    if ui.SmallButton("Copy##sewcopy") then
      local t = eq.LuaSocialExport(n, { name = name, color = color or 0, lines = lines })
      if t then
        ui.SetClipboardText(t)
        say("The share text is on the clipboard.")
      end
    end
    if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
      ui.SetTooltip("Copy this social as one line of text to share (hailstone-social:1:...)")
    end
    ui.EndDisabled()
    ui.SameLine()
    ui.BeginDisabled(n == nil)
    if ui.SmallButton("Paste##sewpaste") then
      local t, why = eq.LuaSocialImport(ui.GetClipboardText() or "")
      if not t then
        say(why or "The clipboard holds no social.")
      elseif t.lua then
        -- a Lua social: to the Lua editor, unsaved (luasocial.lua takes the draft)
        hailstone_social_draft = { social = n, t = t }
        eq.ShowWindow(WINDOW, false)
        eq.OpenLuaSocialEditor(n)
        ui.EndDisabled()
        return true
      else
        if t.name ~= "" then eq.SetWindowText(WINDOW, "SEW_NameInput", t.name) end
        if t.color >= 0 and t.color < 20 then eq.ClickChild(WINDOW, "SEW_TC" .. t.color .. "Button", "left") end
        for i = 1, 5 do setLine(i, t.lines[i] or "") end
        say("Pasted: Accept to keep it.")
      end
    end
    if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
      ui.SetTooltip("Take a social someone shared from the clipboard (a Lua one opens in the Lua editor)")
    end
    ui.EndDisabled()
  end
  if n and eq.CurrentSocial() == n then
    ui.SameLine(0, 12 * s)
    S.Chip("running", C.good)
    ui.SameLine()
    if ui.SmallButton("Stop##sewstop") then eq.StopSocial() end
  end
  if flash.text ~= "" and ui.GetTime() - flash.at < 5 then
    ui.SameLine(0, 12 * s)
    K.Caption(flash.text)
  end
  ui.Dummy(0, 2 * s)
  return false
end

local OPTS = {
  size = function() return 560, 560 end,
  resizable = { 460, 480 },
}

local function body(x, y, w, h, s)
  local n = eq.SocialEditorSocial()
  local saved = n and eq.LuaSocial(n) or nil
  local colors = eq.SocialColors() or {}
  local nr, ng, nb = eq.ChildColor(WINDOW, "SEW_NameInput")
  local name = W.Text(WINDOW, "SEW_NameInput")
  local lines = {}
  for i = 1, 5 do lines[i] = W.Text(WINDOW, lineId(i)) end
  local color = nil
  for i, c in ipairs(colors) do
    if nr and near(c[1], nr) and near(c[2], ng) and near(c[3], nb) then color = i - 1 break end
  end
  local anyLine = false
  for i = 1, 5 do if lines[i] ~= "" then anyLine = true end end
  local dirty
  if saved then
    dirty = name ~= saved.name or (color ~= nil and color ~= saved.color)
    for i = 1, 5 do if lines[i] ~= (saved.lines and saved.lines[i] or "") then dirty = true end end
  else
    dirty = name ~= "" or anyLine
  end
  local _, lh = ui.CalcTextSize("Ag")
  local fh = lh + 6 * s

  -- the header: the button, its place, the mode
  local pw, ph = 78 * s, 46 * s
  local hx, hy = ui.GetCursorScreenPos()
  preview(name, nr, ng, nb, pw, ph, s)
  -- dragging the button puts the social on the cursor (saved first)
  if not ui.IsMouseDown(ui.Mouse.Left) then dragged = false end
  if n and name ~= "" and not dragged and ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left)
      and not eq.HoldingButton() then
    dragged = true
    pickUp(n, dirty)
  end
  if ui.IsItemHovered() and not dragged and n and name ~= "" then
    ui.SetTooltip(dirty and "Drag onto a hot bar (saves the social first)" or "Drag onto a hot bar")
  end
  ui.SetCursorScreenPos(hx + pw + 10 * s, hy + 1 * s)
  ui.BeginGroup()
  ui.TextColored(C.heading[1], C.heading[2], C.heading[3], 1, n and string.format("Social %d", n + 1) or "Social")
  if n then
    ui.SameLine()
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, string.format("page %d, button %d", n // 12 + 1, n % 12 + 1))
  end
  if dirty then
    S.Chip("unsaved changes", C.gold)
  elseif saved then
    S.Chip("saved", C.good)
  else
    S.Chip("empty", C.muted)
  end
  ui.EndGroup()
  local segW, segH = 156 * s, 26 * s
  ui.SetCursorScreenPos(x + w - segW, hy + (ph - segH) / 2)
  local mode = not MANAGED and K.Segmented("##sewmode", { "Commands", "Lua" }, 1, segW, segH, s, {
    "Five lines, run in turn when the button is pressed.",
    "Edit this social as Lua: the lines are saved, then open in the Lua social editor as mq.cmd calls. Lua can " ..
        "branch on Shift / Ctrl / Alt, read the game and draw the button's face.",
  }) or 1
  if mode == 2 and n then
    -- saved first (named "Lua" when it has lines but no name), so the Lua editor converts what is on screen
    local keep = name
    if keep == "" and anyLine then
      keep = "Lua"
      eq.SetWindowText(WINDOW, "SEW_NameInput", keep)
    end
    if keep ~= "" and (dirty or keep ~= name) then eq.ClickChild(WINDOW, "SEW_Accept_Button", "left") end
    eq.ShowWindow(WINDOW, false)
    eq.OpenLuaSocialEditor(n)
    return
  end
  ui.SetCursorScreenPos(x, hy + ph + 6 * s)
  ui.Dummy(0, 0)
  if toolbar(n, name, color, lines, anyLine, dirty, s) then return end

  -- the name, in its colour
  K.Caption("Name")
  ui.SameLine()
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, string.format("%d / %d", #name, NAME_MAX))
  ui.PushStyleColor(ui.Col.Text, nr or C.text[1], ng or C.text[2], nb or C.text[3], 1)
  W.Edit(WINDOW, "SEW_NameInput", { hint = "The button's label", max = NAME_MAX + 1 })
  ui.PopStyleColor(1)

  -- the colours: SEW_TC0Button .. SEW_TC19Button
  K.Caption("Colour")
  local aw = ui.GetContentRegionAvail()
  local gap = 4 * s
  local perRow = 10
  local sw = math.min(28 * s, (aw - gap * (perRow - 1)) / perRow)
  for i, c in ipairs(colors) do
    if (i - 1) % perRow ~= 0 then ui.SameLine(0, gap) end
    if K.Swatch("##sewc" .. i, c[1], c[2], c[3], sw * 0.75, s, { width = sw, selected = color == i - 1,
          tooltip = "Colour " .. (i - 1) }) then
      eq.ClickChild(WINDOW, "SEW_TC" .. (i - 1) .. "Button", "left")
    end
  end

  -- the lines
  ui.Dummy(0, 2 * s)
  K.Caption("Commands, run in turn")
  ui.SameLine()
  local ex = "Examples"
  local exW = ui.CalcTextSize(ex) + 30 * s
  local cx, cy = ui.GetCursorPos()
  ui.SetCursorPos(cx + math.max(0, ui.GetContentRegionAvail() - exW), cy)
  ui.SetNextItemWidth(exW)
  if ui.BeginCombo("##sewexamples", ex, 0) then
    for _, e in ipairs(EXAMPLES) do
      if ui.Selectable(e[1]) then
        eq.SetWindowText(WINDOW, "SEW_NameInput", e[2])
        for i = 1, 5 do setLine(i, e[3][i] or "") end
      end
      if ui.IsItemHovered() then ui.SetTooltip(table.concat(e[3], "\n")) end
    end
    ui.EndCombo()
  end
  if ui.IsItemHovered() then ui.SetTooltip("Fill in a ready-made social (replaces the name and the lines)") end
  for i = 1, 5 do commandRow(i, lines, s, fh) end

  -- the cooldown and when each line runs
  ui.Dummy(0, 2 * s)
  cooldownRow(lines, s)
  local tl = SL.Timeline(lines)
  ui.SameLine(0, 16 * s)
  if tl.runs == 0 then
    K.Caption("Nothing to send yet.")
  else
    K.Caption(string.format("%d command%s over about %.1f s", tl.runs, tl.runs == 1 and "" or "s", tl.total))
  end
  timeline(lines, tl, s)

  -- what to know before Accept
  if name == "" and anyLine then
    note("A social needs a name: without one, Accept clears it and the lines are lost.", C.danger, s)
  elseif name == "" and saved then
    note("Accept with no name clears this social from every hot bar it is on.", C.danger, s)
  end

  -- the buttons: Clear and Revert, Cancel and Accept
  local bh = K.ButtonH(s)
  local _, by = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x, math.max(by + 4 * s, y + h - bh))
  local bw = math.max(70 * s, (w - 3 * 8) / 4)
  if roleButton("Clear##sewclear", nil, bw, bh) then eq.ClickChild(WINDOW, "SEW_Clear_Button", "left") end
  if ui.IsItemHovered() then ui.SetTooltip("Empty the name and every line (nothing is saved until Accept)") end
  ui.SameLine()
  if roleButton("Revert##sewrevert", nil, bw, bh, dirty and saved ~= nil) then eq.OpenSocialEditor(n) end
  if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then ui.SetTooltip("Back to the social as it was saved") end
  ui.SameLine()
  if roleButton("Cancel##sewcancel", nil, bw, bh) then eq.ShowWindow(WINDOW, false) end
  if ui.IsItemHovered() then ui.SetTooltip("Close without saving") end
  ui.SameLine()
  local clears = name == "" and (saved ~= nil or anyLine)
  local accept = roleButton((clears and "Clear social" or "Accept") .. "##sewaccept", clears and "danger" or "primary",
    bw, bh)
  if ui.IsItemHovered() then
    ui.SetTooltip(clears and "Save the social empty: it leaves every hot bar it is on" or
      "Save the social (Ctrl+Enter)")
  end
  if not accept and ui.GetIO().ctrl and ui.IsKeyPressed(ui.Key.Enter) and ui.IsWindowFocused(1) then
    accept = true -- (1: ImGuiFocusedFlags_ChildWindows)
  end
  if accept then eq.ClickChild(WINDOW, "SEW_Accept_Button", "left") end
end

local function testOpen() eq.OpenSocialEditor(tonumber(ui.GetSetting("test.social") or "") or 0) end
ui.window("Edit Social", function()
  if not eq.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  hud.Dialog(WINDOW, "Edit Social", OPTS, body)
end)
