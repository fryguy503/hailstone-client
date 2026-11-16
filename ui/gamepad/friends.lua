-- Friends (gamepad pack): stands in for FriendsWindow (the friends key, the EQ menu's Friends/Ignore). LB / RB switch
-- between the friends and the ignore list; A picks a name, and the buttons on the right act on it: a tell (the chat
-- window's keyboard with "/tell name "), /who, remove (asked first). Names are added from the target or typed on an
-- on-screen keyboard. Every action is the stock window's own: its name box and buttons (hs.SetWindowText,
-- hs.ClickChild), so /friends and /ignore run as they do from the classic window.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local C = T.C

local FW = "FriendsWindow"
local PAGES = {
  { title = "Friends", list = "FriendsList", input = "NameInput", add = "AddButton", remove = "DeleteButton",
    empty = "Your friends list is empty.", noun = "friend" },
  { title = "Ignore", list = "IgnoreList", input = "IgnoreNameInput", add = "IgnoreAddButton",
    remove = "IgnoreDeleteButton", empty = "You are not ignoring anyone.", noun = "ignored player" },
}
local page = 1
local lists = { {}, {} } -- W.list states
local state = {}         -- the question (W.confirm)
local kb = K.new()
local side = {} -- W.toSide

-- The target's name, when there is one.
local function targetName()
  local _, name, raw = API.character.Gauge(6)
  if raw == -1 or not name or name == "" then return nil end
  return name
end

-- The names of a list (the stock window's rows) and its selected row (1-based, nil for none).
local function names(p)
  local rows, _, selected = API.windows.ListInfo(FW, p.list)
  local out = {}
  for i = 0, (rows or 0) - 1 do
    local r = API.windows.ListRow(FW, p.list, i)
    out[#out + 1] = r and r[1] or ""
  end
  return out, selected and selected >= 0 and selected + 1 or nil
end

-- Adds `name` as the stock window does: its name box, then its Add button.
local function add(p, name)
  if not name or name == "" then return end
  API.windows.SetText(FW, p.input, name)
  API.windows.ClickChild(FW, p.add, "left")
end

-- Testing: the pack's settings file test.open = ...,friends opens it once, 6 s after
-- zoning in (closed first when the game restored it, so it takes the pad as a window opened in play does).
-- (the setting does not change in play: once it is read without friends, the hook stays off, as M.testShow does)
local testAt, testOff
local function testOpen()
  if testOff or GPSTATE["test_friends"] or not API.session.InGame() then return end
  if not (","..(ui.GetSetting("test.open") or "")..","):find(",friends,", 1, true) then
    testOff = true
    return
  end
  testAt = testAt or ui.GetTime()
  if ui.GetTime() - testAt > 6 then
    GPSTATE["test_friends"] = true
    API.windows.Show(FW, true)
  elseif ui.GetTime() - testAt > 5 then
    API.windows.Show(FW, false)
  end
end

ui.window("Friends", function()
  testOpen()
  if not W.shown(FW) then
    kb.open, state.confirm = false, nil
    return
  end
  T.frame()
  local p = PAGES[page]
  local list, selected = names(p)
  local sel = selected and list[selected]
  if W.begin("Friends", { w = 1400, h = 860, title = "Friends and Ignore",
        subtitle = p.title .. " (" .. #list .. ")",
        hints = kb.open and K.HINTS or { { "lb", "Friends" }, { "rb", "Ignore" }, { "a", "Select" }, { "b", "Close" } } }) then
    if not kb.open and not state.confirm and (W.pressed("lb") or W.pressed("rb")) then
      page = page == 1 and 2 or 1
      API.windows.SetTabPage(FW, "Subwindows", page - 1)
      p = PAGES[page]
      list, selected = names(p)
      sel = selected and list[selected]
      -- (the pad's focus to the new page: its first name, else Add a name)
      if #list > 0 then lists[page].focusNext = 1 else side.want, side.at = 2, ui.GetTime() end
    end
    local x0, y0, x1, y1, cw, ch = W.area()
    local wx, wy = ui.GetWindowPos()
    if state.confirm then
      W.confirm(state)
    else
      -- the page tabs (they show which list LB / RB give)
      local tw = T.px(200)
      for i, pg in ipairs(PAGES) do
        local tx = x0 + (i - 1) * (tw + T.px(10))
        T.rect(tx, y0, tx + tw, y0 + T.px(44), i == page and C.accent or C.raised, i == page and 0.9 or 1, T.px(10))
        T.font(20)
        T.textIn(tx, y0, tx + tw, y0 + T.px(44), pg.title, i == page and C.onAccent or C.text, 0.5)
        T.unfont()
      end
      local listW = cw * 0.42
      local ly = y0 + T.px(60)
      ui.SetCursorPos(x0 - wx, ly - wy)
      if #list == 0 then
        T.draw(x0, ly + T.px(10), p.empty, C.dim)
        ui.Dummy(1, 1)
      else
        local ls = lists[page]
        ls.selected = selected
        local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(60) + T.px(10))))
        ls.shown = visible
        local focusedRow = W.list("fr" .. page, ls, #list, visible, 60, listW, function(i, rx0, ry0, rx1, ry1)
          T.font(24)
          T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(18), ry1, list[i], i == selected and C.accent or C.text, 0)
          T.unfont()
        end, function(i) API.windows.ListClick(FW, p.list, i - 1, "left") end)
        W.toSide(side, focusedRow)
      end
      -- the right side: the actions, or the keyboard
      local rx0 = x0 + listW + T.px(30)
      local rw = x1 - rx0
      if kb.open then
        local r = K.draw(kb, pad, rx0, y0 + T.px(60), rw)
        if r == "ok" then add(p, kb.text) end
      else
        local bh = T.px(84)
        local function at(y) ui.SetCursorPos(rx0 - wx, y - wy) end
        local y = y0 + T.px(60)
        local target = targetName()
        at(y)
        W.sideBefore(side, 1)
        if T.button("##addtarget", page == 1 and "Add your target" or "Ignore your target", rw, bh,
              { sub = target or "No target", disabled = not target, align = 0 }, pad) then add(p, target) end
        W.sideAfter(side, 1, page == 1 and 5 or 3, lists[page])
        y = y + bh + T.px(12)
        at(y)
        W.sideBefore(side, 2)
        if T.button("##addname", page == 1 and "Add a name" or "Ignore a name", rw, bh,
              { sub = "Type it on the keyboard", align = 0 }, pad) then
          K.open(kb, page == 1 and "Add a friend" or "Ignore a player", "", 64, true)
        end
        W.sideAfter(side, 2, page == 1 and 5 or 3, lists[page])
        y = y + bh + T.px(24)
        T.font(18)
        T.draw(rx0, y, (sel and sel ~= "") and ("SELECTED: " .. sel:upper()) or "SELECT A NAME ON THE LEFT", C.dim)
        T.unfont()
        y = y + T.px(34)
        local none = not sel or sel == ""
        if page == 1 then
          at(y)
          W.sideBefore(side, 3)
          if T.button("##tell", "Send a tell", rw, bh, { sub = "Opens the chat with /tell", disabled = none, align = 0 }, pad) then
            W.chat("/tell " .. sel .. " ")
          end
          W.sideAfter(side, 3, page == 1 and 5 or 3, lists[page])
          y = y + bh + T.px(12)
          at(y)
          W.sideBefore(side, 4)
          if T.button("##who", "Who", rw, bh, { sub = "Where they are (/who all)", disabled = none, align = 0 }, pad) then
            API.windows.ClickChild(FW, "WhoButton", "left")
            GPSTATE.whoOpen = true
          end
          W.sideAfter(side, 4, page == 1 and 5 or 3, lists[page])
          y = y + bh + T.px(12)
        end
        at(y)
        W.sideBefore(side, (page == 1 and 5 or 3))
        if T.button("##remove", "Remove", rw, bh, { sub = page == 1 and "Off your friends list" or "Stop ignoring",
              disabled = none, align = 0 }, pad) then
          local who, list_, button = sel, p.list, p.remove
          state.confirm = { "Remove " .. who .. " from your " .. (page == 1 and "friends" or "ignore") .. " list?", function()
            -- the stock Remove acts on its selected row: select the name again (the list may have changed)
            local again = names(p)
            for i, n in ipairs(again) do
              if n == who then API.windows.ListClick(FW, list_, i - 1, "left") end
            end
            API.windows.ClickChild(FW, button, "left")
          end }
        end
        W.sideAfter(side, (page == 1 and 5 or 3), page == 1 and 5 or 3, lists[page])
      end
    end
  end
  if W.back(W.finish(), state, kb, lists[page]) then API.windows.Show(FW, false) end
end)
