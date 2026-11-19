-- Who (gamepad pack), from the main menu (and the Friends window's Who): RoF2 has no who window, /who answers in the
-- chat window. This builds the /who line from a few filters (this zone or everywhere, a level band, a class, LFG,
-- a name typed on the keyboard), runs it through the chat window as typing it would (hs.ChatSubmit), and lists the
-- answer (hs.WhoResults: the reply the chat lines came from). A picked player can be sent a tell, invited to the
-- group (/invite name), targeted (/target name) or added as a friend (/friends name).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local C = T.C

-- The first word of each class's name: /who matches a class by the start of its name (Who_BuildAndSendRequest).
local CLASSES = { "Any", "Warrior", "Cleric", "Paladin", "Ranger", "Shadow", "Druid", "Monk", "Bard", "Rogue", "Shaman",
  "Necromancer", "Wizard", "Magician", "Enchanter", "Beastlord", "Berserker" }
-- A class's name as the client shows it (a server's own where its plugin named the class), else `fallback`.
local HAS_CLASS_NAMES = API.Supports and API.Supports("character", "ClassName") == true
local function className(id, fallback, short)
  return HAS_CLASS_NAMES and API.character.ClassName(id, short) or fallback
end
local LEVELS = { { "Any" }, { "Near you", near = 5 } }
for lo = 1, 91, 10 do LEVELS[#LEVELS + 1] = { lo .. "-" .. (lo + 9), lo, lo + 9 } end

local f = { all = true, level = 1, class = 1, lfg = false, name = "" }
local listState = {}
local kb = K.new()
local side = {} -- W.toSide
local asked, askedAt = nil, 0 -- the who_count when the search went out
local selected

local function myLevel()
  return tonumber(API.character.Label(2) or "") -- EQType 2: the player's level
end

-- The /who line for the filters.
local function command()
  local words = { "/who" }
  if f.all then words[#words + 1] = "all" elseif f.lfg then words[#words + 1] = "lfg" end
  if f.class > 1 then words[#words + 1] = CLASSES[f.class]:lower() end
  local l = LEVELS[f.level]
  if l.near then
    local me = myLevel()
    if me then words[#words + 1] = math.max(1, me - l.near) .. " " .. (me + l.near) end
  elseif l[2] then
    words[#words + 1] = l[2] .. " " .. l[3]
  end
  if f.name ~= "" then words[#words + 1] = f.name end
  return table.concat(words, " ")
end

-- The last reply (hs.WhoResults builds every player's chat line each call): read at most every 0.25 s, and its
-- players kept while the reply count is the same (a reply changes it), so each row's line is cut down once. Dropped
-- out of the game: the next connection's count starts over.
local REFRESH = 0.25
local whoCount, whoPlayers, whoAt = nil, nil, nil
local function results()
  local now = ui.GetTime()
  if not whoAt or now - whoAt >= REFRESH or now < whoAt then
    local count, players = API.character.WhoResults()
    if count ~= whoCount or not whoPlayers then whoCount, whoPlayers = count, players end
    whoAt = now
  end
  return whoCount, whoPlayers
end

-- The rest of the line the chat printed (the server's level, class, race, guild and zone), once per player.
local function infoOf(p)
  if p.info then return p.info end
  local info = p.line
  local at, to = info:find(p.name, 1, true)
  if at then info = (info:sub(1, at - 1):gsub("[%[%s]+$", "") .. info:sub(to + 1):gsub("^[%]%s%-]+", "")) end
  info = info:gsub("^%s+", "")
  p.info = info
  return info
end

-- A line typed into the main chat window (the game's command interpreter).
local function say(line)
  local windows = API.chat.Windows()
  API.chat.Submit(windows[1] and windows[1].slot or 1, line)
end

local function run(line)
  say(line)
  asked, askedAt = API.character.WhoResults(), ui.GetTime()
  whoAt = nil -- (read again next frame)
  selected = nil
  listState.offset = 0
end

-- The Friends window's Who (and others) open this window to show the answer: no search of its own then.
-- Testing: the pack's settings file test.open = ...,who opens it once, 6 s after
-- zoning in (closed first when the game restored it, so it takes the pad as a window opened in play does).
-- (the setting does not change in play: once it is read without who, the hook stays off, as M.testShow does)
local testAt, testOff
local function testOpen()
  if testOff or GPSTATE["test_who"] or not API.session.InGame() then return end
  if not (","..(ui.GetSetting("test.open") or "")..","):find(",who,", 1, true) then
    testOff = true
    return
  end
  testAt = testAt or ui.GetTime()
  if ui.GetTime() - testAt > 6 then
    GPSTATE["test_who"] = true
    GPSTATE.whoOpen = true
  elseif ui.GetTime() - testAt > 5 then
    GPSTATE.whoOpen = false
  end
end

ui.window("Who", function()
  testOpen()
  if not API.session.InGame() then
    GPSTATE.whoOpen = false
    whoCount, whoPlayers, whoAt = nil, nil, nil
    return
  end
  if not GPSTATE.whoOpen then
    kb.open = false
    whoAt = nil -- (read afresh when it opens)
    return
  end
  T.frame()
  local count, players = results()
  local waiting = asked ~= nil and count == asked and ui.GetTime() - askedAt < 5
  if count ~= asked then asked = nil end
  local sub = count == 0 and "Search for players" or (#players .. (#players == 1 and " player" or " players"))
  if waiting then sub = "Searching..." end
  if W.begin("Who", { w = 1720, h = 960, title = "Who", subtitle = sub,
        hints = kb.open and K.HINTS or { { "x", "Search" }, { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not kb.open and W.pressed("x") then run(command()) end
    -- the filters: one row of buttons that cycle on A
    local bh = T.px(84)
    local n = 6
    local bw = (cw - (n - 1) * T.px(10)) / n
    local function filter(id, label, value, opts)
      local o = opts or {}
      o.sub, o.align = value, 0
      local clicked = T.button(id, label, bw, bh, o, pad)
      ui.SameLine(0, T.px(10))
      return clicked
    end
    if filter("##scope", "Where", f.all and "Everywhere" or "This zone") then f.all = not f.all end
    if filter("##level", "Level", LEVELS[f.level][1]) then f.level = f.level % #LEVELS + 1 end
    if filter("##class", "Class", f.class > 1 and className(f.class - 1, CLASSES[f.class]) or CLASSES[f.class]) then f.class = f.class % #CLASSES + 1 end
    if filter("##lfg", "Looking for group", f.lfg and "Only" or "Anyone", { disabled = f.all }) then f.lfg = not f.lfg end
    if filter("##name", "Name", f.name ~= "" and f.name or "Anyone") then K.open(kb, "A name (or the start of one)", f.name, 64, true) end
    if T.button("##search", "Search", bw, bh, { primary = true, glyph = "x" }, pad) then run(command()) end
    local ly = y0 + bh + T.px(14)
    T.font(17)
    T.draw(x0, ly, command(), C.faint)
    T.unfont()
    ly = ly + T.px(34)
    local listW = cw * 0.66
    ui.SetCursorPos(x0 - wx, ly - wy)
    if kb.open then
      local r = K.draw(kb, pad, x0, ly, math.min(cw, T.px(1100)))
      if r == "ok" then f.name = kb.text:match("^%s*(.-)%s*$") or "" end
    elseif #players == 0 then
      T.draw(x0, ly + T.px(10), count == 0 and "Pick the filters, then Search." or "No players matched those filters.", C.dim)
      ui.Dummy(1, 1)
    else
      if selected and selected > #players then selected = nil end
      listState.selected = selected
      local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(64) + T.px(10))))
      listState.shown = visible
      local focusedRow = W.list("who", listState, #players, visible, 64, listW, function(i, rx0, ry0, rx1, ry1)
        local p = players[i]
        local col = i == selected and C.accent or C.text
        local w = rx1 - rx0
        T.font(23)
        T.textIn(rx0 + T.px(18), ry0, rx0 + w * 0.28, ry1, p.name, col, 0)
        T.unfont()
        T.font(19)
        T.textIn(rx0 + w * 0.29, ry0, rx1 - T.px(14), ry1, T.fit(infoOf(p), w * 0.7), C.dim, 0)
        T.unfont()
      end, function(i) selected = i end)
      W.toSide(side, focusedRow)
      -- the picked player: their line as the chat printed it, and what to do
      local rx0 = x0 + listW + T.px(30)
      local rw = x1 - rx0
      local p = selected and players[selected]
      T.font(18)
      -- (on the /who line's level: the buttons start level with the first row, so D-pad down from it goes down
      -- the list, not across)
      T.draw(rx0, ly - T.px(34), p and p.name:upper() or "SELECT A PLAYER ON THE LEFT", C.dim)
      T.unfont()
      local ry = ly
      local abh = T.px(76)
      local who = p and p.name or ""
      local actions = {
        { "##tell", "Send a tell", "Opens the chat with /tell", function() W.chat("/tell " .. who .. " ") end },
        { "##invite", "Invite to group", "/invite", function() say("/invite " .. who) end },
        { "##target", "Target", "In this zone", function() say("/target " .. who) end },
        { "##friend", "Add as a friend", "/friends", function() say("/friends " .. who) end },
      }
      for k, a in ipairs(actions) do
        ui.SetCursorPos(rx0 - wx, ry - wy)
        W.sideBefore(side, k)
        if T.button(a[1], a[2], rw, abh, { sub = a[3], disabled = p == nil, align = 0 }, pad) then a[4]() end
        W.sideAfter(side, k, #actions, listState)
        ry = ry + abh + T.px(10)
      end
    end
  end
  if W.back(W.finish(), nil, kb, listState) then GPSTATE.whoOpen = false end
end)
