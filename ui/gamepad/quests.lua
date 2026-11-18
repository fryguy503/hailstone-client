-- Quests (gamepad pack): stands in for TaskWnd (the quest journal) and TaskSelectWnd (a task giver's offers).
--   Quest Journal  three pages, LB / RB to switch:
--                  Tasks    the active tasks (title, time left); focusing one selects it, as a click on the stock
--                           list does: its objectives (instructions, progress, zones; done ones dimmed) and its
--                           description. Preview asks for the selected task's rewards; X abandons it (asked first).
--                  Shared   the shared task's members.
--                  History  the completed tasks; A asks for one's objectives (the stock list's click).
--   Task Offers    the offers (title, duration) and the focused one's description and objectives. Preview Reward
--                  asks for that offer's rewards; A accepts the focused offer, X declines them all (as does
--                  closing the window, the stock window's close).
-- Everything comes from the stock windows' lists and texts (hs.ListInfo / ListRow / WindowText) and acts through
-- their buttons (hs.ClickChild), so the rules are the game's.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local P = dofile("lib/textpanel.lua")
local C = T.C

-- Keeps the D-pad's up / down in a W.list column: ImGui's navigation may jump to a nearer item of the next column
-- (the objectives, the message), so a focused row hands the focus to its neighbour itself. (The last shown row's
-- down and the first's up scroll the list: W.list does those.)
local function stayInColumn(state, i, count, visible)
  local v = i - (state.offset or 0)
  if pad.menuPressed("down") and i < count and v < visible then state.focusNext = v + 1 end
  if pad.menuPressed("up") and v > 1 then state.focusNext = v - 1 end
end

-- A pad shortcut of the current window, not in its first moments: the press that opened it (X uses a bag in the
-- inventory, A opens a tile) is not one for it.
local function pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 8 and W.pressed(b) -- (appeared: by frames, as W.begin keeps it)
end

-- Tabs drawn at x0, y0 (LB / RB switch them; a click picks one); returns the y under them.
local function tabs(labels, page, x0, y0, set)
  local x = x0
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  local h = th + T.px(16)
  for i, l in ipairs(labels) do
    local tw = ui.CalcTextSize(l)
    local w = tw + T.px(40)
    T.rect(x, y0, x + w, y0 + h, page == i and C.accent or C.raised, page == i and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + h, l, page == i and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + h) and ui.IsMouseClicked(ui.Mouse.Left) then set(i) end
    x = x + w + T.px(10)
  end
  T.unfont()
  return y0 + h + T.px(16)
end

-- A list's rows that fit in `height` design-scaled px at `rowH` design px each.
local function fits(height, rowH) return math.max(1, math.floor((height + T.px(10)) / T.px(rowH + 10))) end

-- The rows of a stock list that are not its blank separators (CTaskWnd's rows between groups and steps): their
-- 0-based indexes. Read a row at a time, so kept per list for 0.25 s, until its row count changes, or until this
-- window clicks a list or closes (forgetRows).
local rowCache = {}
local function forgetRows() rowCache = {} end
local function realRows(screen, child)
  local now = ui.GetTime()
  local rows = API.windows.ListInfo(screen, child) or 0
  local key = child -- (the lists' names differ)
  local e = rowCache[key]
  if e and e.rows == rows and now - e.at < 0.25 and now >= e.at then return e.out end
  local out = {}
  for r = 0, rows - 1 do
    local texts = API.windows.ListRow(screen, child, r)
    local blank = true
    for _, t in ipairs(texts or {}) do if t ~= "" then blank = false end end
    if not blank then out[#out + 1] = r end
  end
  rowCache[key] = { rows = rows, at = now, out = out }
  return out
end

-- The objectives list of `screen`'s `child` (columns: instructions, status, zone) at the cursor, `width` wide.
local function objectives(screen, child, state, visible, width)
  local idx = realRows(screen, child)
  local rows = #idx
  if rows == 0 then
    local x0, y0 = ui.GetCursorScreenPos()
    T.draw(x0, y0, "No objectives.", C.faint)
    return
  end
  ui.BeginGroup() -- (the rows start at this x, not the window's left)
  W.list(child, state, rows, visible, 72, width, function(i, rx0, ry0, rx1, ry1, focused)
    local texts, r, g, b = API.windows.ListRow(screen, child, idx[i])
    if not texts then return end
    local col = { r or 1, g or 1, b or 1, 1 }
    local done = texts[2] == "Done" -- the stock list's status for a finished objective
    local p = T.px(16)
    -- progress on the right: "n/m" and a small bar
    local status = texts[2] or ""
    local n, m = status:match("^(%d+)/(%d+)$")
    local sw = T.px(150)
    T.font(21)
    local _, th = ui.CalcTextSize("Ag")
    T.textIn(rx0 + p, ry0 + T.px(6), rx1 - sw - p, ry0 + T.px(6) + th,
      T.fit(texts[1] or "", rx1 - rx0 - sw - 2 * p), done and C.dim or col, 0)
    T.unfont()
    T.font(17)
    local _, sh = ui.CalcTextSize("Ag")
    T.draw(rx0 + p, ry1 - sh - T.px(8), T.fit(texts[3] or "", rx1 - rx0 - sw - 2 * p), C.faint)
    T.unfont()
    if done then
      T.textIn(rx1 - sw, ry0, rx1 - p, ry1, status, C.good, 1)
    elseif n and m and tonumber(m) > 0 then
      T.font(20)
      T.textIn(rx1 - sw, ry0 + T.px(6), rx1 - p, ry0 + T.px(6) + th, status, done and C.good or C.text, 1)
      T.unfont()
      T.bar(rx1 - sw, ry1 - T.px(18), rx1 - p, ry1 - T.px(10), math.min(1, tonumber(n) / tonumber(m)), done and C.good or C.accent)
    elseif status ~= "" then
      T.textIn(rx1 - sw, ry0, rx1 - p, ry1, status, C.dim, 1)
    end
  end)
  ui.EndGroup()
end

-- ---------------------------------------------------------------- the quest journal (TaskWnd)
local TW = "TaskWnd"
local page = 1 -- 1 tasks, 2 shared, 3 history
local taskList, elemList, memberList, histList, histElems = { offset = 0 }, { offset = 0 }, { offset = 0 }, { offset = 0 }, { offset = 0 }
local desc = { scroll = 0 }

local journalShown = false

ui.window("Quest Journal", function()
  if not W.shown(TW) then
    journalShown = false
    if next(rowCache) then forgetRows() end
    return
  end
  if not journalShown then -- opened: the pad's focus on the first task
    journalShown, taskList.focusNext, taskList.offset = true, 1, 0
  end
  local taskIdx = realRows(TW, "TASK_TaskList")
  local count = #taskIdx
  local _, _, sel = API.windows.ListInfo(TW, "TASK_TaskList")
  local selTexts = sel and sel >= 0 and API.windows.ListRow(TW, "TASK_TaskList", sel) or nil
  local hints = { { "lb", "Page" }, { "rb", "Page" } }
  if page == 1 and selTexts and taskList.hasFocus then hints[#hints + 1] = { "x", "Abandon" } end
  if page == 3 then hints[#hints + 1] = { "a", "Show objectives" } end
  hints[#hints + 1] = { "b", "Close" }
  local sub = count == 1 and "1 active task" or (count .. " active tasks")
  if W.begin("Quest Journal", { w = 1640, h = 960, title = "Quest Journal", subtitle = sub, hints = hints }) then
    if pressed("Quest Journal", "lb") then page = (page + 1) % 3 + 1 end
    if pressed("Quest Journal", "rb") then page = page % 3 + 1 end
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local top = tabs({ "Tasks", "Shared", "History" }, page, x0, y0, function(i) page = i end)
    local leftX1 = x0 + (x1 - x0) * 0.36
    local rx0 = leftX1 + T.px(24)
    if page == 1 then
      -- the tasks
      ui.SetCursorPos(x0 - wx, top - wy)
      local taskRows = fits(y1 - top, 84)
      local focusedTask = W.list("tasks", taskList, count, taskRows, 84, leftX1 - x0, function(i, rx0_, ry0, rx1, ry1, focused)
        if focused then stayInColumn(taskList, i, count, taskRows) end
        local texts = API.windows.ListRow(TW, "TASK_TaskList", taskIdx[i])
        if not texts then return end
        local p = T.px(16)
        T.font(22)
        local _, th = ui.CalcTextSize("Ag")
        T.draw(rx0_ + p, ry0 + T.px(10), T.fit(texts[2] or "", rx1 - rx0_ - 2 * p), C.text)
        T.unfont()
        T.font(18)
        local left = texts[3] or ""
        if not left:match("^%d+:%d%d:%d%d$") then left = "" end -- "Unlimited"
        T.draw(rx0_ + p, ry0 + T.px(16) + th, left ~= "" and ("Time left " .. left) or "No time limit", left ~= "" and C.endurance or C.faint)
        T.unfont()
        if focused and taskList.lastFocus ~= i then -- select it in the stock window: its objectives and description
          taskList.lastFocus = i
          API.windows.ListClick(TW, "TASK_TaskList", taskIdx[i], "left")
          forgetRows()
          elemList.offset, desc.scroll = 0, 0
        end
      end, function(i)
        API.windows.ListClick(TW, "TASK_TaskList", taskIdx[i], "left")
        forgetRows()
      end)
      taskList.hasFocus = focusedTask ~= nil
      if count == 0 then T.draw(x0, top, "You have no active tasks.", C.faint) end
      -- the selected task: title, objectives, description
      if selTexts then
        T.font(26)
        local _, th = ui.CalcTextSize("Ag")
        T.draw(rx0, top, T.fit(selTexts[2] or "", x1 - rx0))
        T.unfont()
        local oy = top + th + T.px(14)
        T.font(18)
        T.draw(rx0, oy, "OBJECTIVES", C.dim)
        local _, hh = ui.CalcTextSize("Ag")
        T.unfont()
        oy = oy + hh + T.px(8)
        local descY = y1 - (y1 - top) * 0.40
        ui.SetCursorPos(rx0 - wx, oy - wy)
        objectives(TW, "TASK_TaskElementList", elemList, fits(descY - oy - T.px(20), 72), x1 - rx0)
        local previewH = T.px(60)
        P.draw("##taskdesc", desc, API.windows.Text(TW, "TASK_Description") or "", rx0, descY, x1, y1 - previewH - T.px(8), { empty = "No description." })
        ui.SetCursorPos(rx0 - wx, y1 - previewH - wy)
        local previewEnabled = API.windows.ChildState(TW, "TASK_PreviewRewardButton") == true
        local canSelectMonster = API.windows.ChildShown(TW, "TASK_MonsterButton") == true
        local actionWidth = canSelectMonster and (x1 - rx0 - T.px(10)) / 2 or x1 - rx0
        if T.button("##taskpreview", "Preview Reward", actionWidth, previewH, { disabled = not previewEnabled }, pad) then
          API.windows.ClickChild(TW, "TASK_PreviewRewardButton", "left")
        end
        if canSelectMonster then
          ui.SameLine(0, T.px(10))
          if T.button("##taskmonster", "Select Monster", actionWidth, previewH, {}, pad) then
            API.windows.ClickChild(TW, "TASK_MonsterButton", "left")
          end
        end
        -- X abandons the focused task (the list's row, which is the selected one) through the stock Remove, which
        -- asks first
        if focusedTask and pressed("Quest Journal", "x") then API.windows.ClickChild(TW, "TASK_AbandonButton", "left") end
      end
    elseif page == 2 then
      local n = API.windows.ListInfo(TW, "STASK_MemberList") or 0
      ui.SetCursorPos(x0 - wx, top - wy)
      T.heading("Shared task members")
      W.list("members", memberList, n, fits(y1 - top - T.px(40), 60), 60, (x1 - x0) * 0.5, function(i, rx0_, ry0, rx1, ry1)
        local texts = API.windows.ListRow(TW, "STASK_MemberList", i - 1)
        if not texts then return end
        T.textIn(rx0_ + T.px(16), ry0, rx1 - T.px(16), ry1, texts[1] or "", C.text, 0)
        local role = texts[2] or ""
        local monster = texts[3] or ""
        if monster ~= "" then role = role ~= "" and (role .. "  /  " .. monster) or monster end
        if role ~= "" then T.textIn(rx0_, ry0, rx1 - T.px(16), ry1, role, C.dim, 1) end
      end)
      if n == 0 then T.draw(x0, top + T.px(40), "You are not in a shared task.", C.faint) end
    else
      local n = API.windows.ListInfo(TW, "QUEST_TaskList") or 0
      local _, _, hsel = API.windows.ListInfo(TW, "QUEST_TaskList")
      ui.SetCursorPos(x0 - wx, top - wy)
      local histRows = fits(y1 - top, 60)
      W.list("history", histList, n, histRows, 60, leftX1 - x0, function(i, rx0_, ry0, rx1, ry1, focused)
        if focused then stayInColumn(histList, i, n, histRows) end
        local texts = API.windows.ListRow(TW, "QUEST_TaskList", i - 1)
        if not texts then return end
        T.textIn(rx0_ + T.px(16), ry0, rx1 - T.px(16), ry1, T.fit(texts[2] or "", rx1 - rx0_ - T.px(32)), C.text, 0)
      end, function(i)
        API.windows.ListClick(TW, "QUEST_TaskList", i - 1, "left") -- asks the server for its objectives
        forgetRows()
        histElems.offset = 0
      end)
      if n == 0 then T.draw(x0, top, "No completed tasks.", C.faint) end
      if hsel and hsel >= 0 then
        local texts = API.windows.ListRow(TW, "QUEST_TaskList", hsel)
        T.font(26)
        local _, th = ui.CalcTextSize("Ag")
        T.draw(rx0, top, T.fit(texts and texts[2] or "", x1 - rx0))
        T.unfont()
        ui.SetCursorPos(rx0 - wx, top + th + T.px(20) - wy)
        objectives(TW, "QUEST_TaskElementList", histElems, fits(y1 - top - th - T.px(20), 72), x1 - rx0)
      else
        T.draw(rx0, top, "Choose a completed task to see its objectives.", C.faint)
      end
    end
  end
  if W.finish() then API.windows.Show(TW, false) end
end)

-- ---------------------------------------------------------------- a task giver's offers (TaskSelectWnd)
local SW = "TaskSelectWnd"
local offerList = { offset = 0 }
local offerDesc = { scroll = 0 }

ui.window("Task Offers", function()
  if not W.shown(SW) then
    offerList.lastFocus = nil
    return
  end
  local count = API.windows.ListInfo(SW, "TaskList") or 0
  if W.begin("Task Offers", { w = 1500, h = 880, title = "Tasks Offered",
        subtitle = count == 1 and "1 task offered" or (count .. " tasks offered"),
        hints = { { "a", "Accept" }, { "x", "Decline" }, { "b", "Close (decline)" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local leftX1 = x0 + (x1 - x0) * 0.40
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local offerRows = fits(y1 - y0, 84)
    W.list("offers", offerList, count, offerRows, 84, leftX1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
      if focused then stayInColumn(offerList, i, count, offerRows) end
      local texts = API.windows.ListRow(SW, "TaskList", i - 1)
      if not texts then return end
      local p = T.px(16)
      T.font(22)
      local _, th = ui.CalcTextSize("Ag")
      T.draw(rx0 + p, ry0 + T.px(10), T.fit(texts[1] or "", rx1 - rx0 - 2 * p), C.text)
      T.unfont()
      T.font(18)
      local d = texts[2] or ""
      T.draw(rx0 + p, ry0 + T.px(16) + th, d ~= "" and ("Duration " .. d) or "No time limit", d ~= "" and C.endurance or C.faint)
      T.unfont()
      if focused and offerList.lastFocus ~= i then -- select it: its description shows
        offerList.lastFocus = i
        API.windows.ListClick(SW, "TaskList", i - 1, "left")
        offerDesc.scroll = 0
      end
    end, function(i)
      API.windows.ListClick(SW, "TaskList", i - 1, "left")
      API.windows.ClickChild(SW, "AcceptButton", "left")
    end)
    local rx0 = leftX1 + T.px(24)
    local _, _, sel = API.windows.ListInfo(SW, "TaskList")
    local texts = sel and sel >= 0 and API.windows.ListRow(SW, "TaskList", sel) or nil
    T.font(26)
    local _, th = ui.CalcTextSize("Ag")
    T.draw(rx0, y0, T.fit(texts and texts[1] or "", x1 - rx0))
    T.unfont()
    local previewH = T.px(60)
    P.draw("##offerdesc", offerDesc, API.windows.Text(SW, "Description") or "", rx0, y0 + th + T.px(14), x1, y1 - previewH - T.px(8))
    ui.SetCursorPos(rx0 - wx, y1 - previewH - wy)
    local previewEnabled = API.windows.ChildState(SW, "TSEL_PreviewRewardButton") == true
    if T.button("##offerpreview", "Preview Reward", x1 - rx0, previewH, { disabled = not previewEnabled }, pad) then
      API.windows.ClickChild(SW, "TSEL_PreviewRewardButton", "left")
    end
    if pressed("Task Offers", "x") then API.windows.ClickChild(SW, "DeclineButton", "left") end
  end
  if W.finish() then API.windows.Show(SW, false) end
end)
