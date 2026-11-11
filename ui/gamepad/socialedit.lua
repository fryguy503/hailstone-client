-- Edit Social (gamepad pack): stands in for SocialEditWnd (a hot button's Assign Social with a new social, a right
-- click on a social). The social's name and its five command lines as fields the on-screen keyboard types into
-- (the window's edit boxes: 15 and 255 characters), its colour as a row of the window's twenty colour buttons, and
-- the window's Clear and Accept (Accept saves it, as the stock button does). A Lua social opens the Lua social
-- editor instead (luasocial.lua), as in every pack. As the classic pack's editor has it (lib/social_lines.lua): a
-- line's field holds its command, its caption says what the command is, and "Wait after" picks its /pause; More
-- picks the cooldown (/timer), copies or pastes share text (a pasted Lua social goes to the Lua editor unsaved) and
-- edits the social as Lua (saved first, its lines as mq.cmd calls); Test saves and presses it (Stop while it runs);
-- Hot bar saves it and picks the slot to put it on (lib/assign.lua).
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local AS = dofile("lib/assign.lua")
local SL = dofile("lib/social_lines.lua")
local C = T.C
local WIN = "SocialEditWnd"
local MANAGED = ui.ManagedDistribution and ui.ManagedDistribution() or false

local st = CH.new()
local wasShown = false
local message, messageAt = "", -100
local function say(text) message, messageAt = text, ui.GetTime() end
local function setLine(i, text) eq.SetWindowText(WIN, "SEW_Line" .. (i - 1) .. "Input", text:sub(1, 255)) end

-- The waits a line may have (/pause, in tenths of a second) and the cooldowns (/timer), as pickers.
local WAITS = { 0, 5, 10, 15, 20, 30, 50, 100, 200, 300, 600 }
local COOLDOWNS = { 0, 50, 100, 300, 600, 1200, 1800, 3000, 6000, 18000, 36000 }
local function seconds(t)
  if t >= 600 and t % 600 == 0 then return string.format("%d min", t // 600) end
  local v = t / 10
  return (v == math.floor(v) and string.format("%d", v) or string.format("%.1f", v)) .. " s"
end
local function waitText(t) return t and seconds(t) or "None" end
local function coolText(t) return t and seconds(t) or "none" end
local WAIT_NAMES, COOLDOWN_NAMES = {}, {}
for i, v in ipairs(WAITS) do WAIT_NAMES[i] = v == 0 and "None (the next line a tenth of a second later)" or seconds(v) end
for i, v in ipairs(COOLDOWNS) do COOLDOWN_NAMES[i] = v == 0 and "None" or seconds(v) end

-- What a line is, after its number in the field's caption.
local function what(p)
  if p.raw == "" then return "" end
  if p.command == "" then return ": waits only" end
  if p.kind == "command" then return ": runs " .. (p.name or "a command") end
  if p.kind == "chatserver" then return ": to the chat server" end
  if p.kind == "invalid" then return ": not a game command" end
  if p.command:match("^%s*#") then return ": a server # command" end
  return ": said in chat"
end

-- The colour button whose colour the name box has (the social's colour; -1 when none matches).
local function currentColour(colours)
  local r, g, b = eq.ChildColor(WIN, "SEW_NameInput")
  if not r then return -1 end
  for i, c in ipairs(colours) do
    if math.abs(c[1] - r) < 0.02 and math.abs(c[2] - g) < 0.02 and math.abs(c[3] - b) < 0.02 then return i - 1 end
  end
  return -1
end

ui.window("Edit Social", function()
  CH.testOpen(WIN)
  if not W.shown(WIN) then
    st.kb.open, st.pick, wasShown = false, nil, false
    return
  end
  T.frame()
  local appearing = not wasShown
  wasShown = true
  if W.begin("Edit Social", { w = 1400, h = 1000, title = "Edit Social", subtitle = "A hot button that types its lines",
        hints = CH.hints(st, { { "a", "Edit" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(st, x0, y0, x1, y1) then
      local fh = T.px(84)
      ui.SetCursorPos(x0 - wx, y0 - wy)
      if appearing then ui.SetKeyboardFocusHere() end
      CH.field(st, WIN, "SEW_NameInput", "Name", cw, fh, { max = 15, capital = true, prompt = "The social's name", keep = true })
      -- the colours: ten a row, each the colour it gives the name
      local colours = eq.SocialColors and eq.SocialColors() or {}
      local cur = currentColour(colours)
      local cy = y0 + fh + T.px(16)
      local th = CH.caption(x0, cy, "Colour")
      cy = cy + th + T.px(6)
      local n, gap = 10, T.px(10)
      local sw = (cw - (n - 1) * gap) / n
      local sh = T.px(56)
      local name = CH.text(WIN, "SEW_NameInput")
      for i = 0, 19 do
        local id = "SEW_TC" .. i .. "Button"
        ui.SetCursorPos(x0 + (i % n) * (sw + gap) - wx, cy + (i // n) * (sh + gap) - wy)
        local clicked, focused, bx0, by0, bx1, by1 = T.button("##" .. id, "", sw, sh, {}, pad)
        local c = colours[i + 1] or { 1, 1, 1 }
        T.font(20)
        T.textIn(bx0 + T.px(6), by0, bx1 - T.px(6), by1, T.fit(name ~= "" and name or "Social", sw - T.px(12)), { c[1], c[2], c[3], 1 }, 0.5)
        T.unfont()
        if i == cur then T.outline(bx0 + T.px(2), by0 + T.px(2), bx1 - T.px(2), by1 - T.px(2), C.accent, 1, T.px(10), T.px(3)) end
        if clicked then eq.ClickChild(WIN, id, "left") end
      end
      -- the five lines: each the command (its /pause and /timer kept apart), what it is, and its wait
      local social = eq.SocialEditorSocial and eq.SocialEditorSocial()
      local lines, anyLine = {}, false
      for i = 1, 5 do
        lines[i] = CH.text(WIN, "SEW_Line" .. (i - 1) .. "Input")
        if lines[i] ~= "" then anyLine = true end
      end
      local tl = SL.Timeline(lines)
      local ly = cy + 2 * (sh + gap) + T.px(10)
      th = CH.caption(x0, ly, tl.runs == 0 and "Lines (each is typed as a chat line, in turn)" or
        string.format("Lines, in turn: %d command%s over about %.1f s", tl.runs, tl.runs == 1 and "" or "s", tl.total))
      ly = ly + th + T.px(6)
      local lh = T.px(70)
      local waitW = T.px(210)
      for i = 1, 5 do
        local id = "SEW_Line" .. (i - 1) .. "Input"
        local p = SL.Parts(lines[i])
        local rowY = ly + (i - 1) * (lh + T.px(8))
        ui.SetCursorPos(x0 - wx, rowY - wy)
        local prefixLen = #SL.Compose(p.timer, p.pause, "x") - 1
        CH.edit(st, WIN .. id, "Line " .. i .. what(p), p.command,
          function(t) setLine(i, SL.Compose(p.timer, p.pause, t)) end, cw - waitW - T.px(12), lh,
          { max = 255 - prefixLen, keep = true, prompt = "Line " .. i .. " (a chat line or a /command)" })
        ui.SetCursorPos(x0 + cw - waitW - wx, rowY - wy)
        CH.refocus(st, "sewwait" .. i)
        if T.button("##sewwait" .. i, "Wait after", waitW, lh, { sub = waitText(p.pause), align = 0 }, pad) then
          local sel = 1
          for k, v in ipairs(WAITS) do if v == (p.pause or 0) then sel = k end end
          CH.choose(st, "Wait after line " .. i, WAIT_NAMES, sel, function(k)
            setLine(i, SL.Compose(p.timer, WAITS[k], p.command))
          end, "sewwait" .. i)
        end
      end
      -- Clear, More (cooldown, share text, Lua), Test, Put on a hot bar, Accept
      local saved = social and eq.LuaSocial(social) or nil
      local dirty = not saved and (name ~= "" or anyLine)
      if saved then
        dirty = name ~= saved.name or (cur >= 0 and cur ~= saved.color)
        for i = 1, 5 do if lines[i] ~= (saved.lines[i] or "") then dirty = true end end
      end
      local function save()
        if dirty then
          eq.ClickChild(WIN, "SEW_Accept_Button", "left")
          eq.OpenSocialEditor(social) -- (Accept closed it: back on the social, as saved)
        end
      end
      local ready = social ~= nil and name ~= ""
      local bh = T.px(72)
      local by = y1 - bh
      local linesEnd = ly + 5 * (lh + T.px(8))
      if message ~= "" and ui.GetTime() - messageAt < 5 and by - T.px(34) >= linesEnd then
        T.font(20)
        T.draw(x0, by - T.px(34), message, C.dim)
        T.unfont()
      end
      ui.SetCursorPos(x0 - wx, by - wy)
      local bgap = T.px(12)
      local bw = (cw - 4 * bgap) / 5
      CH.button(WIN, "SEW_Clear_Button", "Clear", bw, bh, { sub = "Name and lines", align = 0, keep = true })
      ui.SameLine(0, bgap)
      CH.refocus(st, "sewmore")
      if T.button("##sewmore", "More", bw, bh, { sub = MANAGED and "Cooldown" or "Cooldown, share, Lua", align = 0, disabled = social == nil },
            pad) and social then
        local ci = nil
        for i = 1, 5 do if not ci and SL.Parts(lines[i]).timer then ci = i end end
        local cool = ci and SL.Parts(lines[ci]).timer or nil
        CH.choose(st, "More", MANAGED and { "Cooldown: " .. coolText(cool) }
          or { "Cooldown: " .. coolText(cool), "Copy share text", "Paste share text", "Edit as Lua" }, 1,
          function(k)
            if k == 1 then
              local sel = 1
              for j, v in ipairs(COOLDOWNS) do if v == (cool or 0) then sel = j end end
              CH.choose(st, "Cooldown (the button dims after a press)", COOLDOWN_NAMES, sel, function(j)
                local v = COOLDOWNS[j] > 0 and COOLDOWNS[j] or nil
                local at = ci
                if not at then
                  at = 1
                  for i = 1, 5 do if lines[i] ~= "" then at = i break end end
                end
                local q = SL.Parts(lines[at])
                if v or ci then setLine(at, SL.Compose(v, q.pause, q.command)) end
              end, "sewmore")
            elseif k == 2 then
              local t = name ~= "" and eq.LuaSocialExport(social, { name = name, color = math.max(cur, 0), lines = lines })
              if t then ui.SetClipboardText(t) say("The share text is on the clipboard.")
              else say("Give the social a name first.") end
            elseif k == 3 then
              local t, why = eq.LuaSocialImport(ui.GetClipboardText() or "")
              if not t then
                say(why or "The clipboard holds no social.")
              elseif t.lua then
                hailstone_social_draft = { social = social, t = t } -- (luasocial.lua takes it, unsaved)
                eq.ShowWindow(WIN, false)
                eq.OpenLuaSocialEditor(social)
              else
                if t.name ~= "" then eq.SetWindowText(WIN, "SEW_NameInput", t.name) end
                if t.color >= 0 and t.color < 20 then eq.ClickChild(WIN, "SEW_TC" .. t.color .. "Button", "left") end
                for i = 1, 5 do setLine(i, t.lines[i] or "") end
                say("Pasted: Accept to keep it.")
              end
            else
              if anyLine and name == "" then eq.SetWindowText(WIN, "SEW_NameInput", "Lua") end
              if CH.text(WIN, "SEW_NameInput") ~= "" and (dirty or name == "") then
                eq.ClickChild(WIN, "SEW_Accept_Button", "left")
              end
              eq.ShowWindow(WIN, false)
              eq.OpenLuaSocialEditor(social)
            end
          end, "sewmore")
      end
      ui.SameLine(0, bgap)
      local running = social ~= nil and eq.CurrentSocial() == social
      if T.button("##sewtest", running and "Stop" or "Test", bw, bh, { sub = running and "It is running" or
            "Saves, then presses it", align = 0, disabled = not (running or (ready and anyLine)) }, pad) then
        if running then
          eq.StopSocial()
        elseif ready and anyLine then
          save()
          eq.PressSocial(social)
          say(dirty and "Saved and pressed." or "Pressed.")
        end
      end
      ui.SameLine(0, bgap)
      if T.button("##sewbar", "Hot bar", bw, bh, { sub = "Pick a slot", align = 0, disabled = not ready }, pad)
          and ready then
        save()
        AS.start(name, function() return eq.SocialToCursor(social) == true end, "Edit Social")
      end
      ui.SameLine(0, bgap)
      CH.button(WIN, "SEW_Accept_Button", "Accept", bw, bh, { primary = true, keep = true })
    end
  end
  if CH.back(st, W.finish()) then eq.ShowWindow(WIN, false) end
end)
