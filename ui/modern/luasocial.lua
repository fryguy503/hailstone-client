-- Lua social: Hailstone's editor for Lua socials (game/lua_social.h; not a stock window), in this UI's look. A
-- social's Lua runs on its hot button instead of five command lines; "Press" runs on the key press, "Face"
-- (optional) says how the button looks. What it may do is the server's choice (eq.LuaSocialRules): a note under the
-- name says what, and a dry run lists the commands a press would send without sending them.
--
-- It opens on a right click on a Lua social, from the social editor's socials when they are Lua, from the social
-- editor's Commands / Lua switch, and with /luasocial [1..120 | new] (eq.LuaSocialEditRequest). A plain social's
-- lines become mq.cmd calls, their waits mq.delay (lib/social_lines.lua); the same switch here turns a press made
-- only of those back into command lines and opens the social editor on them. A resizable dialog: the social's place and whether it is
-- running (with Stop), the name and its colour (the twenty social colours as swatches), the server's rules, the
-- Press and Face code on tabs (ui.CodeEditor: Tab takes a completion, Ctrl+Up / Ctrl+Down choose one; a Lua
-- error's line is marked), then Save / Dry run with the Shift, Ctrl and Alt a press is tried with, Revert, the
-- examples, Put on cursor, Copy share text / Paste and Delete (asks first), and the dry run's commands below.
-- eq.*: app/lua_eq_luasocial.cpp.
if ui.ManagedDistribution and ui.ManagedDistribution() then return end
local hud = dofile("lib/hud.lua")
local S = dofile("lib/social.lua")
local K = dofile("lib/dialogs_kit.lua")
local SL = dofile("lib/social_lines.lua")
local T = hud.T
local C = T.color
local KEY = "LuaSocialWnd" -- (no game window: the place and size are kept under this name)

local TEMPLATES = {
  { "Heal by health (Shift: yourself)", [[
-- Shift: heal yourself. Otherwise heal the target: gem 1 under 50% health, gem 2 above.
if button.shift then mq.cmd('/target id ' .. mq.TLO.Me.ID()) end
if (mq.TLO.Target.PctHPs() or 100) < 50 then
    mq.cmd('/cast 1')
else
    mq.cmd('/cast 2')
end]], [[
-- The target's health on the button, dimmed without a target.
local hp = mq.TLO.Target.PctHPs()
return {label = hp and (hp .. '%') or 'Heal', enabled = hp ~= nil}]] },
  { "Rotation: the next spell each press", [[
-- Each press casts the next gem of the list (state keeps the place between presses).
local gems = {1, 2, 3}
state.i = (state.i or 0) % #gems + 1
mq.cmdf('/cast %d', gems[state.i])]], [[
-- The gem the next press casts, in the corner.
local gems = {1, 2, 3}
return {overlay = tostring(gems[(state.i or 0) % #gems + 1])}]] },
  { "Assist, then attack", [[
-- Assist the target's target and turn attack on; Ctrl: attack off.
if button.ctrl then
    mq.cmd('/attack off')
else
    mq.cmd('/assist')
    mq.cmd('/attack on')
end]], [[
return {label = mq.TLO.Me.Combat() and 'Fighting' or 'Assist'}]] },
  { "Spell ready face", [[
mq.cmd('/cast 1')]], [[
-- Dimmed while gem 1 is not ready.
return {enabled = mq.TLO.Me.SpellReady(1)() == true}]] },
  { "Med to full (full mode: waits)", [[
-- Sits, waits until mana is full (at most ten minutes) and stands up. Needs a server that allows full mode;
-- press it again to stop.
mq.cmd('/sit')
mq.delay('10m', function() return (mq.TLO.Me.PctMana() or 100) >= 100 end)
mq.cmd('/stand')]], "" },
}

local open = false
local social = -1
local ed = nil      -- {name, color, press, face, saved = {...}}
local result = nil  -- the last dry run
local mods = { shift = false, ctrl = false, alt = false }
local errors = { press = 0, face = 0 }
local message = ""

local function load(n)
  social = n
  local s = eq.LuaSocial(n)
  ed = {
    name = s and s.name or "Lua", color = s and s.color or 0,
    press = s and s.press or "", face = s and s.face or "",
  }
  if s and not s.lua then
    -- A plain social becomes a Lua one: its lines as mq.cmd calls, their waits as mq.delay.
    ed.press = SL.ToLua(s.lines or {})
  end
  ed.saved = { name = ed.name, color = ed.color, press = ed.press, face = ed.face }
  result, message = nil, ""
  errors = { press = 0, face = 0 }
  -- a Lua social pasted in the social editor (socialedit.lua): in the editor, not saved
  local d = hailstone_social_draft
  hailstone_social_draft = nil
  if d and d.social == n then
    ed.name = d.t.name ~= "" and d.t.name or ed.name
    ed.color, ed.press, ed.face = d.t.color, d.t.press, d.t.face
    message = "Pasted: save to keep it."
  end
  open = true
end

local function dirty()
  local v = ed.saved
  return ed.name ~= v.name or ed.color ~= v.color or ed.press ~= v.press or ed.face ~= v.face
end

-- The line of a Lua error in the press or face part ("social 'X' (press):3: ...").
local function errorLines(text)
  errors = { press = 0, face = 0 }
  if not text then return end
  local part, line = text:match("%((%a+)%):(%d+):")
  if part and errors[part] then errors[part] = tonumber(line) end
end

local function save()
  if ed.name == "" then ed.name = "Lua" end
  eq.SetLuaSocial(social, { name = ed.name, color = ed.color, press = ed.press, face = ed.face })
  ed.saved = { name = ed.name, color = ed.color, press = ed.press, face = ed.face }
  message = "Saved."
end

-- What the server lets a Lua social do, and the note's colour.
local function rulesText(r)
  if r.mode == "off" then
    return "This server does not run Lua socials. You can still write them and dry-run them.", C.danger
  end
  local t = r.mode == "full" and "Full mode: a social may wait and see chat events, like a script."
      or "Press mode: a social runs to its end on the press, without waiting."
  t = t .. " It may: " .. (r.caps ~= "" and r.caps or "nothing") .. "."
  if r.mode == "press" and r.press_commands > 0 then t = t .. " At most " .. r.press_commands .. " commands a press." end
  t = t .. (r.faces and " Button faces are on." or " Button faces are off.")
  return t, C.accent
end

-- A tinted note box across the width.
local function note(text, color, s)
  local x, y = ui.GetCursorScreenPos()
  local aw = ui.GetContentRegionAvail()
  local pad = 8 * s
  ui.PushFont(T.font.small)
  ui.SetCursorScreenPos(x + pad, y + pad * 0.75)
  ui.PushTextWrapPos((ui.GetCursorPos()) + aw - 2 * pad)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, text)
  ui.PopTextWrapPos()
  ui.PopFont()
  local _, y1 = ui.GetCursorScreenPos()
  y1 = y1 + pad * 0.5
  ui.DrawRectFilled(x, y, x + aw, y1, color[1], color[2], color[3], 0.10, 6 * s)
  ui.DrawRectFilled(x, y, x + 3 * s, y1, color[1], color[2], color[3], 0.9, 2 * s)
  ui.SetCursorScreenPos(x, y1 + 4 * s)
  ui.Dummy(0, 0)
end

-- Back to command lines (a press of mq.cmd and mq.delay lines only): saved as such, then the social editor on it.
local function toCommands()
  local lines, why = SL.FromLua(ed.press, ed.face)
  if not lines then
    message = why
    return
  end
  eq.SetLuaSocial(social, { name = ed.name ~= "" and ed.name or "Lua", color = ed.color, press = "", face = "",
    lines = lines })
  open = false
  eq.OpenSocialEditor(social)
end

local function header(s, x, w)
  local st = eq.LuaSocial(social)
  local _, hy = ui.GetCursorScreenPos()
  ui.TextColored(C.heading[1], C.heading[2], C.heading[3], 1, string.format("Social %d", social + 1))
  ui.SameLine()
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1,
    string.format("page %d, button %d", social // 12 + 1, social % 12 + 1))
  if st and st.running then
    ui.SameLine()
    S.Chip("running", C.good)
    ui.SameLine()
    if ui.SmallButton("Stop") then eq.StopLuaSocial(social) end
  end
  local _, why = SL.FromLua(ed.press, ed.face)
  local segW, segH = 156 * s, 24 * s
  local after = { ui.GetCursorScreenPos() }
  ui.SetCursorScreenPos(x + w - segW, hy - 2 * s)
  if K.Segmented("##lsmode", { "Commands", "Lua" }, 2, segW, segH, s, {
        why and ("Cannot go back to command lines: " .. why) or
            "Turn this social back into command lines (saved now) and open the social editor on them.",
        "Lua: runs the press code, and the face draws the button.",
      }) == 1 then
    toCommands()
  end
  ui.SetCursorScreenPos(after[1], after[2])
  ui.Dummy(0, 0)
  return st
end

local function nameAndColour(s)
  local colors = eq.SocialColors()
  local c = colors[ed.color + 1] or { 1, 1, 1 }
  K.Caption("Name")
  ui.SameLine()
  ui.SetNextItemWidth(150 * s)
  ui.PushStyleColor(ui.Col.Text, c[1], c[2], c[3], 1)
  local name, changed = ui.InputText("##lsname", ed.name, 15)
  ui.PopStyleColor(1)
  if changed then ed.name = name end
  ui.SameLine(0, 14 * s)
  K.Caption("Colour")
  ui.SameLine()
  local aw = ui.GetContentRegionAvail()
  local gap = 2 * s
  local per = math.max(1, math.min(#colors, math.floor((aw + gap) / (14 * s + gap))))
  local sw = math.min(18 * s, (aw - gap * (per - 1)) / per)
  local x0 = ui.GetCursorPos()
  for i, col in ipairs(colors) do
    if i > 1 then
      if (i - 1) % per == 0 then ui.SetCursorPos(x0, select(2, ui.GetCursorPos())) else ui.SameLine(0, gap) end
    end
    if K.Swatch("##lsc" .. i, col[1], col[2], col[3], sw, s, { selected = ed.color == i - 1,
          tooltip = "Colour " .. (i - 1) }) then
      ed.color = i - 1
    end
  end
end

local function code(s, codeH)
  if ui.BeginTabBar("##lsparts") then
    for _, part in ipairs({ "press", "face" }) do
      local label = part == "press" and "Press" or "Face"
      if errors[part] > 0 then label = label .. "  (error, line " .. errors[part] .. ")" end
      if ui.BeginTabItem(label .. "##" .. part) then
        local t, ch = ui.CodeEditor("##ls" .. part, ed[part], 0, codeH,
          { complete = eq.LuaComplete, error_line = errors[part] })
        if ch then ed[part] = t end
        ui.EndTabItem()
      end
    end
    ui.EndTabBar()
  end
end

local function primaryButton(label, primary)
  local a = C.accent
  if primary then
    ui.PushStyleColor(ui.Col.Button, a[1], a[2], a[3], 0.30)
    ui.PushStyleColor(ui.Col.ButtonHovered, a[1], a[2], a[3], 0.45)
    ui.PushStyleColor(ui.Col.ButtonActive, a[1], a[2], a[3], 0.60)
  end
  local clicked = ui.Button(label)
  if primary then ui.PopStyleColor(3) end
  return clicked
end

local function actions(s)
  if primaryButton(dirty() and "Save *##lssave" or "Save##lssave", dirty()) then save() end
  if ui.IsItemHovered() then ui.SetTooltip("Keep the social (its hot buttons run it from now on)") end
  ui.SameLine()
  if ui.Button("Dry run") then
    result = eq.LuaSocialDryRun(social, { press = ed.press, face = ed.face }, mods)
    errorLines(result and result.error)
    message = ""
  end
  if ui.IsItemHovered() then ui.SetTooltip("Run the press without sending anything: lists what it would send") end
  ui.SameLine(0, 12 * s)
  K.Caption("as if held")
  for _, m in ipairs({ "shift", "ctrl", "alt" }) do
    ui.SameLine()
    local v = ui.Checkbox(m:sub(1, 1):upper() .. m:sub(2) .. "##lsmod" .. m, mods[m])
    mods[m] = v
  end

  if ui.Button("Revert") then load(social) end
  if ui.IsItemHovered() then ui.SetTooltip("Back to the social as it was saved") end
  ui.SameLine()
  ui.SetNextItemWidth(200 * s)
  if ui.BeginCombo("##lstemplates", "Examples", 0) then
    for _, t in ipairs(TEMPLATES) do
      if ui.Selectable(t[1]) then
        ed.press, ed.face = t[2], t[3]
        if ed.name == "" or ed.name == "Lua" then ed.name = (t[1]:sub(1, 15):gsub("%s+$", "")) end
      end
    end
    ui.EndCombo()
  end
  ui.SameLine()
  if ui.Button("Put on cursor") then
    if dirty() then save() end
    eq.SocialToCursor(social)
  end
  if ui.IsItemHovered() then ui.SetTooltip("Pick the social up to drop it on a hot button") end

  if ui.Button("Copy share text") then
    if dirty() then save() end
    local t = eq.LuaSocialExport(social)
    if t then ui.SetClipboardText(t) message = "The share text is on the clipboard." end
  end
  ui.SameLine()
  if ui.Button("Paste") then
    local t, why = eq.LuaSocialImport(ui.GetClipboardText())
    if t then
      ed.name, ed.color, ed.press, ed.face = t.name ~= "" and t.name or ed.name, t.color, t.press, t.face
      if not t.lua then ed.press = SL.ToLua(t.lines or {}) end -- a plain social's lines, as Lua
      message = "Pasted: save to keep it."
    else
      message = why or "The clipboard holds no social."
    end
  end
  if ui.IsItemHovered() then ui.SetTooltip("Take a social someone shared from the clipboard") end
  ui.SameLine()
  local d = C.danger
  ui.PushStyleColor(ui.Col.Button, d[1], d[2], d[3], 0.18)
  ui.PushStyleColor(ui.Col.ButtonHovered, d[1], d[2], d[3], 0.35)
  ui.PushStyleColor(ui.Col.ButtonActive, d[1], d[2], d[3], 0.50)
  if ui.Button("Delete") then ui.OpenPopup("##lsdelete") end
  ui.PopStyleColor(3)
  if ui.BeginPopup("##lsdelete") then
    ui.Text("Delete this social?")
    if ui.Button("Delete##lsyes") then
      eq.SetLuaSocial(social, { name = "" })
      open = false
      ui.CloseCurrentPopup()
    end
    ui.SameLine()
    if ui.Button("Keep") then ui.CloseCurrentPopup() end
    ui.EndPopup()
  end
  if message ~= "" then
    ui.SameLine(0, 12 * s)
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, message)
  end
end

-- The last dry run: what a press would send (a blocked command in the warning colour), or the error.
local function dryRun(h)
  ui.PushStyleColor(ui.Col.ChildBg, C.inset[1], C.inset[2], C.inset[3], C.inset[4] or 1)
  ui.BeginChild("##lsresult", 0, h, true)
  ui.PopStyleColor(1)
  if result.ok then
    K.Caption(#result.lines == 0 and "A press would send nothing." or "A press would send:")
  else
    ui.PushTextWrapPos(0)
    ui.TextColored(C.danger[1], C.danger[2], C.danger[3], 1, "Error: " .. result.error)
    ui.PopTextWrapPos()
    if #result.lines > 0 then K.Caption("Before it, a press would send:") end
  end
  for _, l in ipairs(result.lines) do
    if l:find("(blocked:", 1, true) then ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, "  " .. l)
    else ui.TextColored(C.text[1], C.text[2], C.text[3], 1, "  " .. l) end
  end
  ui.EndChild()
end

local WINDOW_OPTS = {
  keepTitle = true,
  size = function() return 700, 600 end,
  resizable = { 520, 420 },
  place = K.Centered,
  onClose = function() open = false end,
}
local function drawWindow(x, y, w, h, s)
  local st = header(s, x, w)
  if not open then return end
  nameAndColour(s)
  ui.Dummy(0, 2 * s)
  local text, color = rulesText(st and st.rules or eq.LuaSocialRules())
  note(text, color, s)
  local _, cy = ui.GetCursorScreenPos()
  local _, lh = ui.CalcTextSize("Ag")
  local actionsH = 3 * (lh + 10 * s) + 16 * s
  local resultH = result and math.max(70 * s, math.min(160 * s, (y + h - cy) * 0.28)) or 0
  local codeH = math.max(100 * s, (y + h) - cy - actionsH - resultH - lh - 24 * s)
  code(s, codeH)
  actions(s)
  if result then
    ui.Dummy(0, 2 * s)
    dryRun(math.max(40 * s, (y + h) - select(2, ui.GetCursorScreenPos())))
  end
end

ui.window("Lua Social", function()
  local req = eq.LuaSocialEditRequest()
  if req then load(req) end
  if not open or not ed or not eq.InGame() then return end
  WINDOW_OPTS.shown = open
  hud.Dialog(KEY, "Lua Social", WINDOW_OPTS, drawWindow)
end)
