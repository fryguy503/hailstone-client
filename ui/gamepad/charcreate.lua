-- Character creation (gamepad pack): hs.CharacterCreate stands in for the client's own (ui/dev_screens.cpp
-- character_create_screen). A Hailstone screen, not CCharacterCreation's page. With the scene up the new
-- character's model stands in the middle of the screen, so the panel keeps to the left of it.
--   Main    the name (A types it on the keys; X asks the server for a random one), then gender, race, class, deity
--           and start city, each stepped in place with the D-pad's left / right (or A), the stats (A opens them),
--           Appearance (Y: the customization window on the model), and the game's description of the row focused
--   Stats   a row per stat: left / right takes a point back or spends one (A spends one), X spends as many as it
--           can take, LB resets them all; B or Done goes back
-- Start creates once every point is spent and the name has 4 letters or more; B cancels (back to the character
-- select) or leaves the keys or the stats. The rules are the client's (a race, class or deity resets the stats).
local API = hs.UI
local T = dofile("lib/theme.lua")
local W = dofile("lib/window.lua")
local KF = dofile("lib/keyfield.lua")
local P = dofile("lib/pregame.lua")
local C = T.C

local NAME = "Character Creation"
local ROW_H, GAP = 64, 10
local st = { page = "main", edit = nil, kb = KF.new(), frame = -10, about = nil }
local F = P.focus()

local function px(n) return T.px(n) end

-- The row of `list` (choices {id, name}) `step` from `index`, wrapping round.
local function stepped(list, index, step)
  local n = #list
  if n == 0 then return nil end
  local i = ((math.max(index, 1) - 1 + step) % n) + 1
  return list[i]
end

-- The game's description (dbstr_us.txt) word-wrapped into the box from x0 to x1, at most `lines` lines (the last
-- one kept while the text, the width, the lines and the scale are the same: the box draws it every frame).
local wrapped = {}
local function wrap(text, w, lines)
  local c = wrapped
  if c.text == text and c.w == w and c.lines == lines and c.S == T.S then return c.out end
  local raw = text
  text = text:gsub("<[Bb][Rr]%s*/?>", " "):gsub("<[^>]*>", ""):gsub("%s+", " ")
  local out, line = {}, ""
  for word in text:gmatch("%S+") do
    local try = line == "" and word or (line .. " " .. word)
    if line ~= "" and ui.CalcTextSize(try) > w then
      out[#out + 1] = line
      line = word
    else
      line = try
    end
  end
  if line ~= "" then out[#out + 1] = line end
  if #out > lines then
    out[lines] = T.fit(table.concat(out, " ", lines), w)
    for i = #out, lines + 1, -1 do out[i] = nil end
  end
  wrapped = { text = raw, w = w, lines = lines, S = T.S, out = out }
  return out
end

local function drawAbout(x0, y0, x1, y1)
  local a = st.about
  if not a then return end
  local text, title = API.pregame.CharacterCreateHelp(a.kind, a.id)
  T.rect(x0, y0, x1, y1, C.sunken, 1, px(12))
  T.font(20)
  local _, th = ui.CalcTextSize("Ag")
  T.draw(x0 + px(16), y0 + px(10), T.fit(title ~= "" and title or "About", x1 - x0 - px(32)), C.accent)
  T.unfont()
  T.font(17)
  local _, lh = ui.CalcTextSize("Ag")
  local lines = math.max(1, math.floor((y1 - y0 - px(20) - th - px(6)) / lh))
  if text == "" then
    T.draw(x0 + px(16), y0 + px(16) + th, "No description in the game files.", C.faint)
  else
    for i, line in ipairs(wrap(text, x1 - x0 - px(32), lines)) do
      T.draw(x0 + px(16), y0 + px(16) + th + (i - 1) * lh, line, C.dim)
    end
  end
  T.unfont()
end

-- ---------------------------------------------------------------- the keys
local function clean(t)
  t = t:gsub("[^%a]", "") -- (the client keeps the letters alone)
  if #t > 64 then t = t:sub(1, 64) end
  return t
end

local function openEdit(v, byPad)
  st.edit = { buf = v.name }
  st.kb.shift = v.name == ""
  if byPad or P.padDrives() then KF.focus(st.kb) else st.edit.typeFocus = true end
end

local function closeEdit(accept)
  local e = st.edit
  st.edit = nil
  F:want("##name")
  if accept then API.pregame.CharacterCreateDo("name", e.buf) end
end

local function drawEdit(x0, y0, x1)
  local e = st.edit
  T.font(24)
  local _, th = ui.CalcTextSize("Ag")
  T.draw(x0, y0, "Name", C.dim)
  T.unfont()
  P.at(x0, y0 + th + px(8))
  ui.SetNextItemWidth(x1 - x0)
  if e.typeFocus then
    ui.SetKeyboardFocusHere()
    e.typeFocus = false
  end
  T.font(26)
  local t, enter = ui.InputText("##createname", e.buf, 65, ui.InputTextFlags.EnterReturnsTrue, { moveToEnd = true })
  T.unfont()
  if t ~= e.buf then
    e.buf = clean(t)
    -- the first letter a capital, as the client makes it on Create
    if #e.buf == 1 then st.kb.shift = false end
  end
  T.font(17)
  T.draw(x0, P.y() + px(4), "4 to 15 letters; the first is made a capital.", C.faint)
  T.unfont()
  P.at(x0, P.y() + px(34))
  local done = KF.draw(st.kb, function() return e.buf end, function(s)
    e.buf = clean(s)
    if #e.buf >= 1 then st.kb.shift = false end
  end, x0, x1, { keyHeight = 58 })
  if enter or done == "done" or P.press("start") then closeEdit(true)
  elseif P.press("b") or ui.IsKeyPressed(ui.Key.Escape) then closeEdit(false) end
end

-- ---------------------------------------------------------------- the main page
local function choiceRow(v, id, label, list, index, kind, w, h)
  local cur = list[index]
  local step, focused = P.cycle(F, id, label, cur and cur.name or "-", w, h, { disabled = v.busy })
  -- (the gender has no description)
  if focused then st.about = cur and kind ~= "gender" and { kind = kind, id = cur.id } or nil end
  if step and #list > 1 then
    local nxt = stepped(list, index, step)
    if nxt then API.pregame.CharacterCreateDo(kind, nxt.id) end
  end
  return focused
end

local function mainPage(v, x0, y0, x1, y1, w)
  local rh, gap = px(ROW_H), px(GAP)
  local y = y0
  local function next() y = y + rh + gap; P.at(x0, y) end
  P.at(x0, y)
  local nameShown = v.namePending and "Asking the server..." or v.name
  local clicked, focused = P.field(F, "##name", "Name", nameShown, w, rh,
    { disabled = v.busy, placeholder = "Choose a name", note = (#v.name > 0 and #v.name < 4) and "At least 4 letters" or nil })
  if clicked then openEdit(v, P.padDrives()) end
  if focused then st.about = nil end
  next()
  local genders = { { id = 0, name = "Male" }, { id = 1, name = "Female" } }
  choiceRow(v, "##gender", "Gender", genders, v.gender + 1, "gender", w, rh)
  next()
  choiceRow(v, "##race", "Race", v.races, v.raceIndex, "race", w, rh)
  next()
  choiceRow(v, "##class", "Class", v.classes, v.classIndex, "class", w, rh)
  next()
  choiceRow(v, "##deity", "Deity", v.deities, v.deityIndex, "deity", w, rh)
  next()
  choiceRow(v, "##city", "Start city", v.cities, v.cityIndex, "city", w, rh)
  next()
  local left = v.pointsLeft > 0 and string.format("%d point%s to spend", v.pointsLeft, v.pointsLeft == 1 and "" or "s")
      or "All points spent"
  local sclicked, sfocused = P.field(F, "##stats", "Stats", left, w, rh, { chevron = true, disabled = v.busy })
  if sfocused then st.about = nil end
  if sclicked then
    st.page = "stats"
    F:want("##stat1")
  end
  local by = y + rh + gap
  if v.scene then
    P.at(x0, by)
    local aclicked, afocused = P.button(F, "##appearance", "Appearance", w, rh, { glyph = "y", disabled = v.busy })
    if afocused then st.about = nil end
    if aclicked then API.pregame.CharacterCreateDo("appearance") end
    by = by + rh + gap
  end
  -- the description of the row focused, down to the footer
  if y1 - by > px(90) then drawAbout(x0, by, x1, y1) end
end

-- ---------------------------------------------------------------- the stats page
local function statsPage(v, x0, y0, x1, y1, w)
  local rh, gap = px(ROW_H), px(GAP)
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  local left = string.format("%d point%s to spend", v.pointsLeft, v.pointsLeft == 1 and "" or "s")
  T.draw(x0, y0, left, v.pointsLeft > 0 and C.accent or C.good)
  T.unfont()
  local y = y0 + th + px(12)
  for i, s in ipairs(v.stats) do
    P.at(x0, y)
    local note = string.format("%d to %d%s", s.base, s.cap, s.favored and "  ·  your class favours it" or "")
    local step, focused = P.cycle(F, "##stat" .. i, s.name, tostring(s.value), w, rh, { note = note, disabled = v.busy })
    if focused then
      st.about = { kind = "stat", id = i }
      if P.press("x") then API.pregame.CharacterCreateDo("statMax", i) end
    end
    if step == 1 then API.pregame.CharacterCreateDo("statUp", i)
    elseif step == -1 then API.pregame.CharacterCreateDo("statDown", i) end
    y = y + rh + gap
  end
  if P.press("lb") then API.pregame.CharacterCreateDo("resetStats") end
  local bw = (w - gap) / 2
  P.at(x0, y)
  if P.button(F, "##reset", "Reset", bw, rh, { glyph = "lb", disabled = v.busy }) then API.pregame.CharacterCreateDo("resetStats") end
  ui.SameLine(0, gap)
  if P.button(F, "##statsdone", "Done", bw, rh, { glyph = "b" }) then
    st.page = "main"
    F:want("##stats")
  end
  y = y + rh + gap
  if y1 - y > px(90) then drawAbout(x0, y, x1, y1) end
end

ui.window(NAME, function()
  local v = API.pregame.CharacterCreate and API.pregame.CharacterCreate()
  if not v then
    st.frame = -10
    return
  end
  local frame = ui.GetFrameCount()
  local appearing = frame - st.frame > 1
  st.frame = frame
  if appearing then
    -- (also back from the Appearance window, which hides this one)
    st.page, st.edit = "main", nil
    F:want("##name")
  end
  local hints
  if st.edit then
    hints = { KF.hints[1], KF.hints[2], KF.hints[3], KF.hints[4], { "start", "Done" }, { "b", "Back" } }
  elseif st.page == "stats" then
    hints = { { "a", "Spend" }, { "x", "Spend all" }, { "lb", "Reset" }, { "b", "Back" } }
  else
    hints = { { "a", "Select" }, { "x", "Random name" }, { "y", "Appearance" }, { "start", "Create" }, { "b", "Cancel" } }
  end
  local opts = { w = v.scene and 720 or 920, h = 1000, x = v.scene and 70 or nil, close = false, alwaysFocus = true,
    title = st.page == "stats" and "Stats" or "New character",
    subtitle = v.raceName ~= "" and (v.raceName .. " " .. v.className) or "", hints = hints }
  if W.begin(NAME, opts) then
    local x0, y0, x1, y1, w = W.area()
    local busy = v.busy
    local gap, bh = px(GAP), px(72)
    local footer = bh + px(50) + px(14)
    if st.edit then
      drawEdit(x0, y0, x1)
    else
      if not busy then
        if st.page == "main" then
          if P.press("start") and v.ready then API.pregame.CharacterCreateDo("create") end
          if P.press("x") then API.pregame.CharacterCreateDo("randomName") end
          if P.press("y") and v.scene then API.pregame.CharacterCreateDo("appearance") end
        end
      end
      if P.press("b") then
        if st.page == "stats" then
          st.page = "main"
          F:want("##stats")
        elseif not busy then
          API.pregame.CharacterCreateDo("cancel")
        end
      end
      if st.page == "stats" then statsPage(v, x0, y0, x1, y1 - footer, w)
      else mainPage(v, x0, y0, x1, y1 - footer - gap, w) end

      -- Cancel and Create under the page, the status over them
      local by = y1 - bh
      local cw = (w - gap) / 2
      P.at(x0, by)
      if P.button(F, "##cancel", "Cancel", cw, bh, { glyph = "b", disabled = busy }) then
        API.pregame.CharacterCreateDo("cancel")
      end
      ui.SameLine(0, gap)
      if P.button(F, "##create", "Create", cw, bh, { primary = v.ready, disabled = not v.ready, glyph = "start" }) then
        API.pregame.CharacterCreateDo("create")
      end
      local status = v.status
      if (not status or status.text == "") and not busy then
        if v.pointsLeft > 0 then status = { text = "Spend the points left in Stats to create.", error = false }
        elseif #v.name < 4 then status = { text = "Choose a name of 4 letters or more to create.", error = false } end
      end
      P.status(status, busy, x0, by - px(12), x1, { bottom = true, lines = 2 })
    end
    F:tick()
  end
  W.finish()
end)
