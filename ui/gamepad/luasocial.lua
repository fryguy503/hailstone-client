-- Lua Social (gamepad pack): Hailstone's editor for Lua socials (game/lua_social.h; not a stock window), as the
-- classic pack's luasocial.lua has it, laid out for the pad. A social's Lua runs on its hot button instead of five
-- command lines: "Press" on the key press, "Face" (optional) how the button looks. What it may do is the server's
-- choice (eq.LuaSocialRules), said at the top; a dry run lists the commands a press would send without sending them.
--
-- The code is a list of lines (LB / RB: Press or Face): A types the focused line on the on-screen keyboard (with a
-- row of code keys: braces, operators, quotes, an indent, mq.cmd), X adds a line under it, Y deletes it; the line a
-- Lua error names is marked. "Edit with the keyboard" swaps the list for the text editor (ui.CodeEditor) for a real
-- keyboard. The examples fill in a whole social. The buttons on the right: Save, Dry run (with Shift / Ctrl / Alt as
-- the press's modifiers), Put on a hot bar (the slot picker, as an ability's X), Examples, Revert, Copy and Paste the
-- share text (the clipboard), Delete (asks first) and Stop while it runs.
--
-- It opens on a right click on a Lua social, from the stock social editor's socials when they are Lua, and with
-- /luasocial [1..120 | new]. eq.*: app/lua_eq_luasocial.cpp.
if ui.ManagedDistribution and ui.ManagedDistribution() then return end
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local AS = dofile("lib/assign.lua")
local SL = dofile("lib/social_lines.lua")
local C = T.C

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

-- The code keys under the keyboard (what the pad's keyboard lacks for Lua).
local CODE_KEYS = { { "{", "{" }, { "}", "}" }, { "=", " = " }, { "==", " == " }, { "~=", " ~= " }, { "..", " .. " },
  { "<", "<" }, { ">", ">" }, { "Indent", "    " }, { "mq.cmd('')", "mq.cmd('')" } }

local open = false
local social = -1
local ed = nil      -- { name, color, press, face, saved = {...} }
local result = nil  -- the last dry run
local mods = { shift = false, ctrl = false, alt = false }
local part = "press"
local errors = { press = 0, face = 0 }
local message = ""
local st = CH.new()
local state = {}    -- W.confirm
local lines = { press = {}, face = {} } -- the W.list states
local side = {}
local typing = false -- the text editor instead of the line list
local lastLine = nil -- the line the keyboard typed: the pad's focus back on it after

local function load(n)
  social = n
  local s = eq.LuaSocial(n)
  ed = { name = s and s.name or "Lua", color = s and s.color or 0, press = s and s.press or "", face = s and s.face or "" }
  if s and not s.lua then
    -- A plain social becomes a Lua one: its lines as mq.cmd calls, their waits as mq.delay (lib/social_lines.lua).
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
  lines.press.focusNext, lines.press.offset = 1, 0
end

local function dirty()
  local v = ed.saved
  return ed.name ~= v.name or ed.color ~= v.color or ed.press ~= v.press or ed.face ~= v.face
end

-- The line of a Lua error in the press or face part ("social 'X' (press):3: ...").
local function errorLines(text)
  errors = { press = 0, face = 0 }
  if not text then return end
  local p, line = text:match("%((%a+)%):(%d+):")
  if p and errors[p] then errors[p] = tonumber(line) end
end

local function save()
  if ed.name == "" then ed.name = "Lua" end
  eq.SetLuaSocial(social, { name = ed.name, color = ed.color, press = ed.press, face = ed.face })
  ed.saved = { name = ed.name, color = ed.color, press = ed.press, face = ed.face }
  message = "Saved."
end

local function rulesText(r)
  if r.mode == "off" then
    return "This server does not run Lua socials. You can still write them and dry-run them.", C.bad
  end
  local t = r.mode == "full" and "Full mode: a social may wait and see chat events, like a script."
      or "Press mode: a social runs to its end on the press, without waiting."
  t = t .. " It may: " .. (r.caps ~= "" and r.caps or "nothing") .. "."
  if r.mode == "press" and r.press_commands > 0 then t = t .. " At most " .. r.press_commands .. " commands a press." end
  t = t .. (r.faces and " Button faces are on." or " Button faces are off.")
  return t, C.dim
end

-- The part's code as lines and back.
local function split(text)
  local out = {}
  for l in ((text or "") .. "\n"):gmatch("([^\n]*)\n") do out[#out + 1] = l end
  if #out > 1 and out[#out] == "" and not (text or ""):find("\n$") then out[#out] = nil end
  return out
end
local function join(t) return table.concat(t, "\n") end

-- Text word-wrapped from x, y within `width`; returns the bottom.
local function wrapped(text, x, y, width, c, maxY)
  local _, lh = ui.CalcTextSize("Ag")
  local line = ""
  for word in text:gmatch("%S+") do
    local try = line == "" and word or (line .. " " .. word)
    if ui.CalcTextSize(try) > width and line ~= "" then
      if maxY and y + lh > maxY then return y end
      T.draw(x, y, line, c)
      y = y + lh
      line = word
    else
      line = try
    end
  end
  if line ~= "" and not (maxY and y + lh > maxY) then
    T.draw(x, y, line, c)
    y = y + lh
  end
  return y
end

-- Testing: the pack's settings file test.luasocial = <n> opens social n (1..120) 10 s after zoning in.
-- (the setting does not change in play: once it is read unset, the hook stays off, as M.testShow does)
local testAt, testOff
local function testOpen()
  if testOff then return end
  local n = tonumber(ui.GetSetting("test.luasocial") or "")
  if not n then
    testOff = true
    return
  end
  if GPSTATE.test_luasocial or not eq.InGame() then return end
  testAt = testAt or ui.GetTime()
  if ui.GetTime() - testAt > 10 then
    GPSTATE.test_luasocial = true
    load(n - 1)
  end
end

ui.window("Lua Social", function()
  testOpen()
  local req = eq.LuaSocialEditRequest()
  if req then load(req) end
  if not open or not ed or not eq.InGame() then
    st.kb.open, st.pick, state.confirm = false, nil, nil
    return
  end
  T.frame()
  local s = eq.LuaSocial(social)
  local hints
  if st.kb.open or st.pick then hints = CH.hints(st, {})
  elseif state.confirm then hints = { { "a", "Select" }, { "b", "No" } }
  else hints = { { "lb", "Press" }, { "rb", "Face" }, { "a", "Edit line" }, { "x", "New line" }, { "y", "Delete line" }, { "b", "Close" } } end
  local sub = string.format("Social %d (page %d, button %d)%s", social + 1, social // 12 + 1, social % 12 + 1,
    s and s.running and "  -  running" or "")
  if W.begin("Lua Social", { w = 1760, h = 1000, title = "Lua Social", subtitle = sub, hints = hints, alwaysFocus = true }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if state.confirm then
      W.confirm(state)
    elseif not CH.modal(st, x0, y0, x1, y1) then
      if not typing then
        if W.pressed("lb") and part ~= "press" then part = "press" lines.press.focusNext = 1 end
        if W.pressed("rb") and part ~= "face" then part = "face" lines.face.focusNext = 1 end
      end
      local leftW = cw * 0.64
      -- the name and colour
      local colours = eq.SocialColors()
      local fh = T.px(76)
      ui.SetCursorPos(x0 - wx, y0 - wy)
      CH.edit(st, "lsname", "Name", ed.name, function(t) ed.name = t end, leftW * 0.6, fh, { max = 15, capital = true })
      ui.SameLine(0, T.px(10))
      local names = {}
      for i = 1, #colours do names[i] = "Colour " .. (i - 1) end
      local cols = {}
      for i, c in ipairs(colours) do cols[i] = { c[1], c[2], c[3], 1 } end
      CH.refocus(st, "lscolour")
      local cclicked, _, bx0, by0, bx1, by1 = T.button("##lscolour", "", leftW * 0.4 - T.px(10), fh, {}, pad)
      local cc = colours[ed.color + 1] or { 1, 1, 1 }
      T.font(17)
      T.draw(bx0 + T.px(14), by0 + T.px(8), "Colour", C.dim)
      T.unfont()
      T.font(21)
      T.draw(bx0 + T.px(14), by0 + T.px(34), T.fit(ed.name, bx1 - bx0 - T.px(28)), { cc[1], cc[2], cc[3], 1 })
      T.unfont()
      if cclicked then
        CH.choose(st, "The name's colour", names, ed.color + 1, function(i) ed.color = i - 1 end, "lscolour")
        st.pick.colors = cols
      end
      -- what the server allows
      local rules = s and s.rules or eq.LuaSocialRules()
      local rt, rc = rulesText(rules)
      T.font(18)
      local ry = wrapped(rt, x0, y0 + fh + T.px(10), leftW, rc, y0 + fh + T.px(70))
      T.unfont()
      -- Press / Face
      local ty = math.max(ry, y0 + fh + T.px(60)) + T.px(8)
      ui.SetCursorPos(x0 - wx, ty - wy)
      local tw = T.px(200)
      for i, p in ipairs({ { "press", "Press", "lb" }, { "face", "Face", "rb" } }) do
        if i > 1 then ui.SameLine(0, T.px(10)) end
        if T.button("##lspart" .. p[1], p[2], tw, T.px(56), { primary = part == p[1], glyph = p[3] }, pad) then part = p[1] end
      end
      ui.SameLine(0, T.px(10))
      if T.button("##lstyping", typing and "Edit by lines" or "Edit with the keyboard", leftW - 2 * tw - T.px(20), T.px(56), {}, pad) then
        typing = not typing
      end
      local cy = ty + T.px(56) + T.px(12)
      local bottom = y1 - (result and T.px(200) or T.px(40))
      ui.SetCursorPos(x0 - wx, cy - wy)
      local focusedRow
      if typing then
        local t, ch = ui.CodeEditor("##code" .. part, ed[part], leftW, bottom - cy,
          { complete = eq.LuaComplete, error_line = errors[part] })
        if ch then ed[part] = t end
      else
        local code = split(ed[part])
        if #code == 0 then code = { "" } end
        local ls = lines[part]
        if lastLine and not CH.busy(st) then ls.focusNext = math.max(1, lastLine - ls.offset) lastLine = nil end
        local rowH = 44
        local visible = math.max(1, math.floor((bottom - cy + T.px(10)) / (T.px(rowH) + T.px(10))))
        ls.shown = visible
        focusedRow = W.list("lscode" .. part, ls, #code, visible, rowH, leftW, function(i, rx0, ry0, rx1, ry1)
          T.font(16)
          T.textIn(rx0 + T.px(8), ry0, rx0 + T.px(52), ry1, tostring(i), errors[part] == i and C.bad or C.faint, 1)
          T.unfont()
          T.font(19)
          local l = code[i]:gsub("\t", "    ")
          T.textIn(rx0 + T.px(66), ry0, rx1 - T.px(10), ry1, T.fit(l, rx1 - rx0 - T.px(80)),
            errors[part] == i and C.bad or (l:match("^%s*%-%-") and C.dim or C.text), 0)
          T.unfont()
          if errors[part] == i then T.outline(rx0, ry0, rx1, ry1, C.bad, 0.8, T.px(10), T.px(2)) end
        end, function(i)
          lastLine = i
          CH.type(st, (part == "press" and "Press" or "Face") .. ", line " .. i, code[i], function(t)
            code[i] = t
            ed[part] = join(code)
          end, { max = 512, keys = CODE_KEYS })
        end)
        if focusedRow then
          if W.pressed("x") then
            table.insert(code, focusedRow + 1, (code[focusedRow]:match("^%s*") or ""))
            ed[part] = join(code)
            ls.focusNext = math.min(focusedRow + 1 - ls.offset, visible)
            if focusedRow + 1 - ls.offset > visible then ls.offset = ls.offset + 1 end
          elseif W.pressed("y") and #code > 1 then
            table.remove(code, focusedRow)
            ed[part] = join(code)
          elseif W.pressed("y") then
            ed[part] = ""
          end
        end
        W.toSide(side, focusedRow)
      end
      -- the dry run's answer under the code
      if result then
        local dy = bottom + T.px(10)
        T.panel(x0, dy, x0 + leftW, y1, 1, T.px(12), C.sunken)
        T.font(18)
        local _, lh = ui.CalcTextSize("Ag")
        local y = dy + T.px(10)
        if result.ok then
          T.draw(x0 + T.px(14), y, #result.lines == 0 and "A press would send nothing." or "A press would send:", C.text)
        else
          y = wrapped("Error: " .. (result.error or ""), x0 + T.px(14), y, leftW - T.px(28), C.bad, y1 - lh) - lh
          if #result.lines > 0 then T.draw(x0 + T.px(14), y + lh, "Before it, a press would send:", C.text) y = y + lh end
        end
        y = y + lh
        for _, l in ipairs(result.lines) do
          if y + lh > y1 - T.px(6) then break end
          T.draw(x0 + T.px(30), y, T.fit(l, leftW - T.px(50)), l:find("(blocked:", 1, true) and C.bad or C.dim)
          y = y + lh
        end
        T.unfont()
      end
      -- the buttons
      local rx0 = x0 + leftW + T.px(24)
      local rw = x1 - rx0
      local bh = T.px(66)
      local half = (rw - T.px(10)) / 2
      local actions = {
        { dirty() and "Save *" or "Save", function() save() end, primary = true },
        { "Dry run", function()
          result = eq.LuaSocialDryRun(social, { press = ed.press, face = ed.face }, mods)
          errorLines(result and result.error)
          message = ""
        end },
        { "mods" },
        { "Put on a hot bar", function()
          if dirty() then save() end
          AS.start(ed.name, function() eq.SocialToCursor(social) return true end, "Lua Social")
        end, sub = "Pick a slot" },
        { "Examples", function(k)
          local names = {}
          for i, t in ipairs(TEMPLATES) do names[i] = t[1] end
          CH.choose(st, "Replace the code with an example", names, 1, function(i)
            local t = TEMPLATES[i]
            ed.press, ed.face = t[2], t[3]
            if ed.name == "" or ed.name == "Lua" then ed.name = (t[1]:sub(1, 15):gsub("%s+$", "")) end
            lines.press.offset, lines.face.offset = 0, 0
          end, "lsact" .. k)
        end },
        { "Revert", function() load(social) end, sub = "Back to the saved social" },
        { "Copy share text", function()
          if dirty() then save() end
          local t = eq.LuaSocialExport(social)
          if t then ui.SetClipboardText(t) message = "The share text is on the clipboard." end
        end },
        { "Paste share text", function()
          local t, why = eq.LuaSocialImport(ui.GetClipboardText())
          if t then
            ed.name, ed.color, ed.press, ed.face = t.name ~= "" and t.name or ed.name, t.color, t.press, t.face
            if not t.lua then ed.press = SL.ToLua(t.lines or {}) end -- a plain social's lines, as Lua
            message = "Pasted: save to keep it."
          else
            message = why or "The clipboard holds no social."
          end
        end },
        { "Delete", function()
          state.confirm = { "Delete this social?", function()
            eq.SetLuaSocial(social, { name = "" })
            open = false
          end }
        end },
      }
      if s and s.running then table.insert(actions, 1, { "Stop", function() eq.StopLuaSocial(social) end, sub = "It is running" }) end
      local by = y0
      local n = #actions
      for k, a in ipairs(actions) do
        ui.SetCursorPos(rx0 - wx, by - wy)
        W.sideBefore(side, k)
        if a[1] == "mods" then
          -- the press's modifiers for the dry run
          local mw = (rw - 2 * T.px(10)) / 3
          for i, m in ipairs({ "shift", "ctrl", "alt" }) do
            if i > 1 then ui.SameLine(0, T.px(10)) end
            if T.button("##lsmod" .. m, m:sub(1, 1):upper() .. m:sub(2), mw, bh, { primary = mods[m], sub = mods[m] and "held" or "not held", align = 0 }, pad) then
              mods[m] = not mods[m]
            end
            if i == 1 then W.sideAfter(side, k, n, lines[part]) end
          end
        else
          CH.refocus(st, "lsact" .. k)
          if T.button("##lsact" .. k, a[1], rw, bh, { primary = a.primary, sub = a.sub, align = 0 }, pad) then a[2](k) end
          W.sideAfter(side, k, n, lines[part])
        end
        by = by + bh + T.px(8)
      end
      if message ~= "" then
        T.font(18)
        wrapped(message, rx0, by + T.px(6), rw, C.dim)
        T.unfont()
      end
    end
  end
  local close = W.finish()
  if state.confirm and close then state.confirm = nil close = false end
  if CH.back(st, close) then open = false end
end)
