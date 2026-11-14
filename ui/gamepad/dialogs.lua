-- The small dialogs (gamepad pack), each standing in for its stock window and answered through it, so the game
-- does what the stock buttons do:
--   Confirmation (ConfirmationDialogBox)   the question, Yes / No (or OK); A answers yes, B no
--   Quantity (QuantityWnd)                 how many: D-pad left / right one, LB / RB ten, X none, Y all; A accepts
--                                          (no ImGui navigation in it: the D-pad is the amount's)
--   Text prompt (TextEntryWnd)             the prompt and its text on the on-screen keys; Done is OK, B Cancel
--   Name changes (NameChangeWnd, NameChangePetWnd, NameChangeMercWnd)
--                                          the new name on the keys, Submit Name, Disable Reminder, Remind Me Later
--   Map label (EditLabelWnd)               the label's text (on the keys), its size and colour, Done and Cancel
--   Alarm (AlarmWnd)                       hours and minutes stepped, quick choices, Set Alarm and Cancel
--   Server plugins (PluginConsent)         a server's compiled plugins ask before they load (eq.PluginConsent):
--                                          Allow / Don't allow (the pad starts on Don't allow); B puts it away
--                                          until the next zone-in
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local C = T.C

-- STML to plain text lines (the dialog's text: colours and links are not needed for a question).
local function plain(stml)
  local s = (stml or ""):gsub("\r", ""):gsub("<[Bb][Rr]%s*/?>", "\n"):gsub("</?[Pp]>", "\n"):gsub("<[^>]*>", "")
  s = s:gsub("&lt;", "<"):gsub("&gt;", ">"):gsub("&quot;", "\""):gsub("&apos;", "'"):gsub("&nbsp;", " "):gsub("&amp;", "&")
  return s
end

-- Text word-wrapped to `width` px from x, y; returns the bottom.
local function wrapped(text, x, y, width, c)
  local _, lh = ui.CalcTextSize("Ag")
  for para in (text .. "\n"):gmatch("(.-)\n") do
    local line = ""
    for word in para:gmatch("%S+") do
      local try = line == "" and word or (line .. " " .. word)
      if ui.CalcTextSize(try) > width and line ~= "" then
        T.draw(x, y, line, c)
        y = y + lh
        line = word
      else
        line = try
      end
    end
    T.draw(x, y, line, c)
    y = y + lh
  end
  return y
end

-- ---------------------------------------------------------------- confirmation
local CONFIRM = "ConfirmationDialogBox"
ui.window("Confirm", function()
  if not W.shown(CONFIRM) then return end
  T.frame()
  local okOnly = eq.ChildShown(CONFIRM, "OK_Button") == true and eq.ChildShown(CONFIRM, "Yes_Button") == false
  local hints = okOnly and { { "a", "OK" } } or { { "a", "Select" }, { "b", "No" } }
  if W.begin("Confirm", { w = 760, h = 420, title = "Confirm", hints = hints, alwaysFocus = true }) then
    local x0, y0, x1, y1, w = W.area()
    local text = plain((eq.WindowText(CONFIRM, "TextOutput")))
    wrapped(text, x0, y0, w, C.text)
    local bw, bh = T.px(220), T.px(64)
    -- the buttons at the bottom right of the content area
    local wx, wy = ui.GetWindowPos()
    ui.SetCursorPos(x1 - wx - (okOnly and bw or 2 * bw + T.px(16)), y1 - wy - bh)
    if okOnly then
      if T.button("##ok", "OK", bw, bh, { primary = true, glyph = "a" }, pad) then eq.ClickChild(CONFIRM, "OK_Button", "left") end
    else
      if T.button("##yes", "Yes", bw, bh, { primary = true, glyph = "a" }, pad) then eq.ClickChild(CONFIRM, "Yes_Button", "left") end
      ui.SameLine(0, T.px(16))
      if T.button("##no", "No", bw, bh, { glyph = "b" }, pad) then eq.ClickChild(CONFIRM, "No_Button", "left") end
    end
  end
  -- B / the close button: No (the stock window's close answers its owner as No)
  if W.finish() then eq.ClickChild(CONFIRM, okOnly and "OK_Button" or "No_Button", "left") end
end)

-- ---------------------------------------------------------------- quantity
local QTY = "QuantityWnd"
ui.window("Quantity", function()
  if not W.shown(QTY) then return end
  T.frame()
  local _, _, value, count = eq.ChildState(QTY, "QTYW_Slider")
  value, count = value or 0, count or 1
  local maxv = math.max(0, count - 1)
  local function set(v) eq.SetSlider(QTY, "QTYW_Slider", math.max(0, math.min(maxv, v))) end
  -- (no ImGui navigation: the D-pad sets the amount, so it cannot also move between the buttons)
  if W.begin("Quantity", { w = 640, h = 400, title = "How many?", noNav = true,
        hints = { { "left", "-1" }, { "right", "+1" }, { "lb", "-10" }, { "rb", "+10" }, { "a", "Accept" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1, w = W.area()
    -- the amount, large
    T.font(56)
    local amount = eq.WindowText(QTY, "QTYW_SliderInput") or tostring(value)
    local _, th = ui.CalcTextSize("0")
    T.textIn(x0, y0, x1, y0 + th, amount, C.text, 0.5)
    T.unfont()
    -- the slider as a bar
    local by = y0 + th + T.px(16)
    T.bar(x0 + T.px(40), by, x1 - T.px(40), by + T.px(16), maxv > 0 and value / maxv or 1, C.accent)
    local accept = false
    if W.focused() then
      if pad.menuPressed("left") then set(value - 1) end
      if pad.menuPressed("right") then set(value + 1) end
      if pad.menuPressed("lb") then set(value - 10) end
      if pad.menuPressed("rb") then set(value + 10) end
      if pad.menuPressed("x") then set(0) end
      if pad.menuPressed("y") then set(maxv) end
      if pad.menuPressed("a") then accept = true end
    end
    -- buttons (the mouse; the pad's buttons are drawn on them)
    local wx, wy = ui.GetWindowPos()
    local bw, bh = T.px(130), T.px(60)
    ui.SetCursorPos(x0 - wx, y1 - wy - bh)
    if T.button("##min", "None", bw, bh, { glyph = "x" }, pad) then set(0) end
    ui.SameLine(0, T.px(10))
    if T.button("##minus", "-", T.px(60), bh, {}, pad) then set(value - 1) end
    ui.SameLine(0, T.px(10))
    if T.button("##plus", "+", T.px(60), bh, {}, pad) then set(value + 1) end
    ui.SameLine(0, T.px(10))
    if T.button("##max", "All", bw - T.px(10), bh, { glyph = "y" }, pad) then set(maxv) end
    ui.SameLine(0, T.px(10))
    local aw = x1 - (ui.GetCursorScreenPos())
    if T.button("##accept", "Accept", math.max(T.px(120), aw), bh, { primary = true, glyph = "a" }, pad) then accept = true end
    if accept then eq.ClickChild(QTY, "QTYW_Accept_Button", "left") end
  end
  if W.finish() then eq.ShowWindow(QTY, false) end
end)

-- ---------------------------------------------------------------- the prompts
local G = dofile("lib/stock.lua")

-- A prompt window typed with the on-screen keys (they are up from the start: typing is what it is for): the text,
-- the edit box child over the keys, and a row of buttons under them. p = { screen, title, prompt, edit, max,
-- capital, buttons = { { id, label, opts } }, ok (the child Done / Enter clicks), cancel (the child B clicks) }.
local function prompt(p)
  local kb = G.KF.new()
  local wasShown = false
  ui.window(p.title, function()
    G.testShow()
    if not W.shown(p.screen) then
      wasShown = false
      return
    end
    if not wasShown then
      wasShown = true
      G.openKeys(kb, p.capital)
    end
    T.frame()
    if W.begin(p.title, { w = 1200, h = 800, title = p.title, hints = G.editHints("Cancel"), alwaysFocus = true }) then
      local x0, y0, x1, y1, w = W.area()
      local wx, wy = ui.GetWindowPos()
      T.font(22)
      local text = G.plain(type(p.prompt) == "function" and p.prompt() or G.text(p.screen, p.prompt))
      local y = G.wrapped(text ~= "" and text or "Enter the text:", x0, y0, w, C.text, 3)
      T.unfont()
      ui.SetCursorPos(x0 - wx, y + T.px(10) - wy)
      if G.edit(kb, p.screen, p.edit, x0, x1, { max = p.max }) == "done" and G.enabled(p.screen, p.ok) then
        eq.ClickChild(p.screen, p.ok, "left")
      end
      -- the window's buttons along the bottom
      local shown = {}
      for _, b in ipairs(p.buttons) do if G.shown(p.screen, b[1]) then shown[#shown + 1] = b end end
      local bh, gap = T.px(64), T.px(12)
      local bw = (w - (#shown - 1) * gap) / math.max(1, #shown)
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      for i, b in ipairs(shown) do
        if i > 1 then ui.SameLine(0, gap) end
        local label = b[2]
        if label == true then label = G.text(p.screen, b[1]) end -- the window names it (Submit / Request Pending)
        G.button(p.screen, b[1], label, bw, bh, b[3])
      end
    end
    if W.finish() then eq.ClickChild(p.screen, p.cancel, "left") end
  end)
end

-- Text prompt (TextEntryWnd: a bandolier set's name, ...): Done (or Enter) is a click on its Entry box, which the
-- window takes as OK, as the stock edit box's Enter; B is its Cancel.
prompt({ screen = "TextEntryWnd", title = "Enter Text", prompt = "Prompt", edit = "Entry", max = 255,
  ok = "Entry", cancel = "Cancel",
  buttons = { { "Cancel", "Cancel", { glyph = "b" } }, { "OK", "OK", { primary = true } } } })

-- Name changes (NameChangeWnd, NameChangePetWnd, NameChangeMercWnd: the server asks for a new name): the prompt
-- the window shows (the reminder's or the manual one), the name (15 characters, a capital first), Submit Name
-- (greyed, "Request Pending", while a request is out), Disable Reminder (with the reminder) and the close button
-- ("Remind Me Later" or "Close"), all the window's own; Done is Enter in its box (Submit while it is enabled).
for _, n in ipairs({ { "NameChangeWnd", "NCW_", "Change Name" }, { "NameChangePetWnd", "NCPW_", "Change Pet Name" },
    { "NameChangeMercWnd", "NCMW_", "Change Mercenary Name" } }) do
  local pre = n[2]
  prompt({ screen = n[1], title = n[3], prompt = pre .. "Prompt", edit = pre .. "NameEdit", max = 15, capital = true,
    ok = pre .. "SubmitButton", cancel = pre .. "CloseButton",
    buttons = { { pre .. "DisableReminderButton", "Disable Reminder" }, { pre .. "CloseButton", true, { glyph = "b" } },
      { pre .. "SubmitButton", true, { primary = true } } } })
end

-- ---------------------------------------------------------------- map label
-- Map label (EditLabelWnd: a new map label, or the marked one edited, from the map toolbar): the label's text (A
-- types it with the keys), its size (the window's Small / Medium / Large, the chosen one lit), Color (the colour
-- picker on the label's colour), Done and Cancel; every piece is the window's child, so the map makes the label.
local LABEL = "EditLabelWnd"
local SIZES = { { "ELW_LabelSmallButton", "Small" }, { "ELW_LabelMediumButton", "Medium" }, { "ELW_LabelLargeButton", "Large" } }
local labelKb, labelEditing, labelShown, labelRefocus = G.KF.new(), false, false, false
ui.window("Map Label", function()
  if not W.shown(LABEL) then
    labelShown = false
    return
  end
  if not labelShown then -- opened: straight to the keys (a label is its text)
    labelShown, labelEditing = true, true
    G.openKeys(labelKb, true)
  end
  T.frame()
  if W.begin("Map Label", { w = 1200, h = labelEditing and 820 or 560, title = "Map Label",
        hints = labelEditing and G.editHints("Done") or { { "a", "Select" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    ui.SetCursorPos(x0 - wx, y0 - wy)
    if labelEditing then
      T.heading("Label text", w)
      if G.edit(labelKb, LABEL, "ELW_LabelTextEditbox", x0, x1, { max = 255 }) == "done" then
        labelEditing, labelRefocus = false, true
      end
    else
      local focus = labelRefocus
      labelRefocus = false
      if G.field(LABEL, "ELW_LabelTextEditbox", "Text", w, T.px(72), "Name this spot", { focus = focus }) then
        labelEditing = true
        G.openKeys(labelKb)
      end
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
      T.heading("Size and color", w)
      local gap = T.px(12)
      local bw = (w - 3 * gap) / 4
      for i, b in ipairs(SIZES) do
        if i > 1 then ui.SameLine(0, gap) end
        G.button(LABEL, b[1], b[2], bw, T.px(72), { primary = G.checked(LABEL, b[1]) })
      end
      ui.SameLine(0, gap)
      G.button(LABEL, "ELW_ChooseColorButton", "Color...", bw, T.px(72))
      local bh = T.px(64)
      local hw = (w - gap) / 2
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      G.button(LABEL, "ELW_CancelButton", "Cancel", hw, bh, { glyph = "b" })
      ui.SameLine(0, gap)
      G.button(LABEL, "ELW_DoneButton", "Done", hw, bh, { primary = true })
    end
  end
  if W.finish() then
    if labelEditing then labelEditing, labelRefocus = false, true else eq.ClickChild(LABEL, "ELW_CancelButton", "left") end
  end
end)

-- ---------------------------------------------------------------- alarm
-- Alarm (AlarmWnd, /alarm: a reminder after a time): hours and minutes stepped with their buttons, quick choices
-- that fill both, what the alarm comes to, Set Alarm and Cancel. The numbers are the window's own edit boxes
-- (eq.SetWindowText) and Set Alarm is its Set Time button, so the window starts the alarm as the stock one does.
local ALARM = "AlarmWnd"
local HOURS, MINUTES = "ALRW_HoursEditbox", "ALRW_MinutesEditbox"
local QUICK = { { "5 min", 0, 5 }, { "15 min", 0, 15 }, { "30 min", 0, 30 }, { "1 hour", 1, 0 }, { "2 hours", 2, 0 } }
ui.window("Alarm", function()
  if not W.shown(ALARM) then return end
  T.frame()
  if W.begin("Alarm", { w = 1100, h = 700, title = "Alarm", subtitle = "Remind me after",
        hints = { { "a", "Select" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local hrs = math.max(0, math.floor(tonumber(G.text(ALARM, HOURS)) or 0))
    local mins = math.max(0, math.floor(tonumber(G.text(ALARM, MINUTES)) or 0))
    local function set(h, m)
      eq.SetWindowText(ALARM, HOURS, tostring(math.max(0, math.min(99, h))))
      eq.SetWindowText(ALARM, MINUTES, tostring(math.max(0, math.min(59, m))))
    end
    local gap, bh = T.px(12), T.px(72)
    -- a number with its steps: the name and value on the left, the buttons on the right
    local function stepper(y, name, value, steps, apply)
      T.font(22)
      T.textIn(x0, y, x0 + T.px(180), y + bh, name, C.dim, 0)
      T.unfont()
      T.font(44)
      T.textIn(x0 + T.px(180), y, x0 + T.px(330), y + bh, tostring(value), C.text, 0.5)
      T.unfont()
      local sw = (x1 - x0 - T.px(350) - (#steps - 1) * gap) / #steps
      ui.SetCursorPos(x0 + T.px(350) - wx, y - wy)
      for i, d in ipairs(steps) do
        if i > 1 then ui.SameLine(0, gap) end
        if T.button("##" .. name .. i, (d > 0 and "+" or "") .. d, sw, bh, {}, pad) then apply(d) end
      end
    end
    stepper(y0, "Hours", hrs, { -1, 1 }, function(d) set(hrs + d, mins) end)
    stepper(y0 + bh + gap, "Minutes", mins, { -5, -1, 1, 5 }, function(d) set(hrs, mins + d) end)
    -- quick choices
    local qy = y0 + 2 * (bh + gap) + T.px(14)
    ui.SetCursorPos(x0 - wx, qy - wy)
    T.heading("Quick choices", w)
    local qw = (w - (#QUICK - 1) * gap) / #QUICK
    for i, q in ipairs(QUICK) do
      if i > 1 then ui.SameLine(0, gap) end
      if T.button("##quick" .. i, q[1], qw, T.px(64), {}, pad) then set(q[2], q[3]) end
    end
    -- what it comes to
    local total = hrs * 60 + mins
    local ay = select(2, ui.GetCursorScreenPos()) + T.px(10)
    T.font(24)
    if total > 0 then
      local parts = {}
      if total // 60 > 0 then parts[#parts + 1] = (total // 60) .. " h" end
      if total % 60 > 0 then parts[#parts + 1] = (total % 60) .. " min" end
      T.draw(x0, ay, "Alarm in " .. table.concat(parts, " "), C.accent)
    else
      T.draw(x0, ay, "Choose a time", C.faint)
    end
    T.unfont()
    local fh = T.px(64)
    local hw = (w - gap) / 2
    ui.SetCursorPos(x0 - wx, y1 - fh - wy)
    G.button(ALARM, "ALRW_CancelButton", "Cancel", hw, fh, { glyph = "b", always = true })
    ui.SameLine(0, gap)
    G.button(ALARM, "ALRW_OkButton", "Set Alarm", hw, fh, { primary = true, always = true, disabled = total == 0 })
  end
  if W.finish() then eq.ClickChild(ALARM, "ALRW_CancelButton", "left") end
end)

-- ---------------------------------------------------------------- server plugins
-- The question a server's compiled plugins ask before they load (docs/plugins.md "The server's compiled plugins ask
-- first"): the server, each plugin's name, file, size and the start of its SHA-256, the warning, Allow and Don't
-- allow (kept for those bytes on this server). It stands in for the client's own prompt ("PluginConsent").
local consentShown = nil -- the question's serial the pad's focus was put on Don't allow for
ui.window("Server Plugins", function()
  eq.WindowShown("PluginConsent")
  local q = eq.PluginConsent()
  if not q then
    consentShown = nil
    return
  end
  T.frame() -- (this file\'s copy of lib/theme.lua: its scale for this screen)
  local h = 470 + 60 * math.min(#q.plugins, 4)
  if W.begin("Server Plugins", { w = 900, h = h, title = "Allow this server's plugin?", alwaysFocus = true,
        hints = { { "a", "Select" }, { "b", "Not now" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local y = wrapped(string.format("%s wants to run %s on your computer:", q.server,
      #q.plugins == 1 and "a compiled plugin" or "compiled plugins"), x0, y0, w, C.text)
    for i, p in ipairs(q.plugins) do
      if i > 4 then
        y = wrapped(string.format("and %d more", #q.plugins - 4), x0, y, w, C.dim)
        break
      end
      local size = p.size >= 1024 * 1024 and string.format("%.1f MB", p.size / (1024 * 1024))
        or string.format("%.0f KB", p.size / 1024)
      y = wrapped("  " .. p.name .. "   " .. p.file .. ", " .. size .. ", sha256 " .. p.short, x0, y, w, C.dim)
    end
    y = wrapped(q.warning, x0, y + T.px(10), w, C.bad or C.text)
    wrapped("Your answer is kept for this server. The add-on list on the login screen can change it.", x0,
      y + T.px(6), w, C.faint)
    local bw, bh = T.px(260), T.px(64)
    local wx, wy = ui.GetWindowPos()
    ui.SetCursorPos(x1 - wx - 2 * bw - T.px(16), y1 - wy - bh)
    if T.button("##consentallow", "Allow", bw, bh, {}, pad) then eq.AnswerPluginConsent(true) end
    ui.SameLine(0, T.px(16))
    if consentShown ~= q.serial then
      ui.SetKeyboardFocusHere()
      consentShown = q.serial
    end
    if T.button("##consentdeny", "Don't allow", bw, bh, {}, pad) then eq.AnswerPluginConsent(false) end
  end
  -- B / the close button: not now (asked again at the next zone-in)
  if W.finish() then eq.AnswerPluginConsent(nil) end
end)
