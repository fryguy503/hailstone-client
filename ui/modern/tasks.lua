-- Compact multi-quest journal. Objectives retain the server's order, individual hidden rows and real group
-- separators; no numbered phases or parent/child relationships are inferred. Tracking is shared with the HUD;
-- reward previews and abandonment remain the selected task's native actions.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local Q = dofile("lib/task_journal.lua")
local T = hud.T
local C = T.color
local WINDOW, TABS = "TaskWnd", "TaskSubwindows"
local PAGE_ACTIVE, PAGE_MEMBERS, PAGE_HISTORY = 0, 1, 2
local state = { category = "active", listW = 200 }
local KIND = { [0] = "Task", [1] = "Shared task", [2] = "Quest" }

local function setCategory(category)
  state.category, state.categoryPref = category, category
  ui.SetSetting("tasks.category", category)
  eq.SetTabPage(WINDOW, TABS, category == "history" and PAGE_HISTORY or PAGE_ACTIVE)
end

local function find(tasks, id)
  for _, task in ipairs(tasks) do if task.id == id then return task end end
end

local function pick(task)
  if task and eq.OpenTask(task.id) then
    state.selected, state.nativeId = task.id, task.id
    Q.invalidate()
  end
end

-- Follow a task opened by the tracker, without taking a history selection away while its page is open.
local function sync(tasks)
  local restoredCategory = false
  local requestedMembers = eq.TabPage(WINDOW, TABS) == PAGE_MEMBERS
  local native = find(tasks, tasks.selectedId)
  local category = ui.GetSetting("tasks.category")
  -- Earlier versions used Shared as a quest filter. The native page is a roster, not another quest list.
  if category == "shared" then category = "active"; ui.SetSetting("tasks.category", category) end
  if not state.loaded or category ~= state.categoryPref then
    state.categoryPref = category
    if category == "active" or category == "history" then
      state.category = category
      eq.SetTabPage(WINDOW, TABS, category == "history" and PAGE_HISTORY or PAGE_ACTIVE)
      if native and category == "active" then state.selected = native.id end
      restoredCategory = true
    end
  end
  if not state.loaded then
    state.loaded = true
    local test = tonumber(ui.GetSetting("test.tab." .. WINDOW .. "." .. TABS) or "")
    if test then eq.SetTabPage(WINDOW, TABS, test); restoredCategory = false end
  end
  local page = eq.TabPage(WINDOW, TABS) or PAGE_ACTIVE
  if requestedMembers or page == PAGE_MEMBERS then
    state.openMembers = true
    setCategory("active")
  elseif page == PAGE_HISTORY then
    state.category = "history"
  elseif native and not restoredCategory and (state.nativeId ~= native.id or state.category == "history") then
    state.selected = native.id
    state.category = "active"
  end
  state.nativeId = tasks.selectedId
  if page ~= PAGE_ACTIVE and page ~= PAGE_HISTORY then eq.SetTabPage(WINDOW, TABS, PAGE_ACTIVE) end
  if state.category ~= "history" then
    local task = find(tasks, state.selected)
    if not task then
      local selected
      for _, row in ipairs(tasks) do if Q.isTracked(row) then selected = row; break end end
      selected = selected or tasks[1]
      state.selected = selected and selected.id or nil
      pick(selected)
    end
  end
end

local function button(label, id, selected, s)
  if selected then ui.PushStyleColor(ui.Col.Button, C.accent[1], C.accent[2], C.accent[3], 0.24) end
  local hit = ui.Button(label .. "##" .. id, 0, 25 * s)
  if selected then ui.PopStyleColor() end
  return hit
end

local function categories(tasks, history, s)
  for i, tab in ipairs({ { "Active", "active", #tasks }, { "History", "history", #history } }) do
    if i > 1 then ui.SameLine(0, 5 * s) end
    if button(tab[1] .. "  " .. tab[3], "category." .. tab[2], state.category == tab[2], s) then
      setCategory(tab[2])
      if tab[2] ~= "history" then
        local selected = find(tasks, state.selected)
        if not selected then
          for _, task in ipairs(tasks) do if Q.isTracked(task) then selected = task; break end end
        end
        selected = selected or tasks[1]
        state.selected = selected and selected.id or nil
        pick(selected)
      end
    end
  end
  -- This roster belongs to the current shared task, independently of the selected quest or History page.
  local members = eq.ListInfo(WINDOW, "STASK_MemberList") or 0
  local label = "Shared members  " .. members
  local width = T.TextSize(label, T.font.normal) + 24 * s
  ui.SameLine(0, 8 * s)
  local x, y = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x + math.max(0, ui.GetContentRegionAvail() - width), y)
  if button(label, "sharedMembers", false, s) then state.openMembers = true end
  if ui.IsItemHovered() then ui.SetTooltip("View shared-task members and manage invitations") end
  ui.Dummy(0, 3 * s)
end

local function memberRow(rows, name)
  if name then for _, row in ipairs(rows) do if row.texts[1] == name then return row.row end end end
  return -1
end

local function sharedMembers(tasks, s)
  local popup = "##sharedMembers"
  if state.openMembers then
    state.openMembers, state.memberName = nil, nil
    eq.ListClick(WINDOW, "STASK_MemberList", -1, "left", 0)
    ui.OpenPopup(popup)
  end
  local io = ui.GetIO()
  ui.SetNextWindowSize(math.min(420 * s, (io.width or 1280) - 20 * s), 0, ui.Cond.Always)
  if not ui.BeginPopup(popup) then return end
  ui.PushFont(T.font.normal)
  ui.Text("Shared task members")
  local shared
  for _, task in ipairs(tasks) do if task.type == 1 then shared = task; break end end
  local sharedId = shared and shared.id or nil
  if state.memberTaskId ~= sharedId then state.memberName = nil; state.memberTaskId = sharedId end
  if shared then ui.TextWrapped(shared.title or "") end
  ui.Separator()

  -- Membership updates do not change the task revision. Re-read the native roster, and retain selection by
  -- name so a departing member cannot shift Remove/Make leader onto a different person at the same row index.
  local rows = K.Rows(WINDOW, "STASK_MemberList")
  local selected = memberRow(rows, state.memberName)
  eq.ListClick(WINDOW, "STASK_MemberList", selected, "left", 0)
  if #rows == 0 then
    state.memberName = nil
    K.Empty(shared and "Waiting for member details." or "You are not in a shared task.")
  else
    W.List(WINDOW, "STASK_MemberList", 0, math.min(210 * s, (28 + #rows * 22) * s), {
      hide = { [3] = true }, columns = { { "Member", 0 }, { "Role", 85 } },
    })
    local _, _, picked = eq.ListInfo(WINDOW, "STASK_MemberList")
    state.memberName = picked and rows[picked + 1] and rows[picked + 1].texts[1] or nil
  end
  ui.Separator()
  if eq.ChildState(WINDOW, "STASK_AddPlayerButton") == false then
    K.Muted("Only the shared-task leader can manage members.")
  end
  local h = 25 * s
  W.Button(WINDOW, "STASK_AddPlayerButton", "Add target", { height = h,
    tooltip = "Invite your current player target to the shared task" })
  ui.SameLine(0, 6 * s)
  -- Re-resolve immediately before the member actions; native selection is only an index.
  selected = memberRow(K.Rows(WINDOW, "STASK_MemberList"), state.memberName)
  eq.ListClick(WINDOW, "STASK_MemberList", selected, "left", 0)
  ui.BeginDisabled(selected < 0)
  W.Button(WINDOW, "STASK_RemovePlayerButton", "Remove member", { height = h, danger = true })
  ui.SameLine(0, 6 * s)
  W.Button(WINDOW, "STASK_MakeLeaderButton", "Make leader", { height = h })
  ui.EndDisabled()
  W.Button(WINDOW, "STASK_InviteRaidButton", "Invite raid", { height = h })
  ui.SameLine(0, 6 * s)
  W.Button(WINDOW, "STASK_InviteExpedButton", "Invite expedition", { height = h })
  ui.SameLine(0, 12 * s)
  if ui.Button("Close##closeSharedMembers", 0, h) then ui.CloseCurrentPopup() end
  ui.PopFont()
  ui.EndPopup()
end

local function card(task, s)
  local tracked = Q.isTracked(task)
  local done, total = Q.stats(task)
  local title = task.title or ""
  local clicked = K.Card("quest." .. task.id, task.id == state.selected, 43, function(x, y, w)
    local markW = tracked and 11 * s or 0
    if tracked then ui.DrawCircleFilled(x + 3 * s, y + 11 * s, 2.5 * s, C.accent[1], C.accent[2], C.accent[3], 1) end
    T.Text(x + markW, y + 3 * s, T.Fit(title, w - markW - 6 * s, T.font.small), C.text, T.font.small)
    local count = string.format("%d/%d", done, total)
    local countW = T.TextSize(count, T.font.small)
    local timed = (task.duration or 0) > 0
    local here = Q.inZone(task)
    local meta = timed and (Q.time(task.left) .. " left") or here and "In this zone" or (KIND[task.type] or "Task")
    if task.type == 1 and (timed or here) then meta = "Shared: " .. meta end
    local color = timed and ((task.left or 0) <= 120 and C.danger or C.gold) or here and C.accent or C.muted
    T.Text(x, y + 23 * s, T.Fit(meta, w - countW - 12 * s, T.font.small), color, T.font.small)
    T.Text(x + w - 6 * s, y + 23 * s, count, C.muted, T.font.small, "right")
  end)
  if ui.IsItemHovered() then ui.SetTooltip(title .. (tracked and "\nTracked" or "")) end
  if clicked then pick(task) end
end

local function taskList(tasks, needle, s)
  local tracked, other = {}, {}
  for _, task in ipairs(tasks) do
    if needle == "" or (task.title or ""):lower():find(needle, 1, true) then
      local group = Q.isTracked(task) and tracked or other
      group[#group + 1] = task
    end
  end
  if #tracked + #other == 0 then K.Empty(needle == "" and "No tasks here." or "No matching tasks."); return end
  if #tracked > 0 then
    K.Muted("Tracked", C.accent)
    for _, task in ipairs(tracked) do card(task, s) end
  end
  if #other > 0 then
    if #tracked > 0 then ui.Dummy(0, 4 * s) end
    K.Muted(#tracked > 0 and "Other active" or "Active quests")
    for _, task in ipairs(other) do card(task, s) end
  end
end

local function historyList(rows, selected, needle, s)
  local shown = 0
  for _, row in ipairs(rows) do
    local title = row.texts[2] or ""
    if needle == "" or title:lower():find(needle, 1, true) then
      shown = shown + 1
      local hit = K.Card("history." .. row.row, row.row == selected, 43, function(x, y, w)
        T.Text(x, y + 3 * s, T.Fit(title, w - 6 * s, T.font.small), C.text, T.font.small)
        T.Text(x, y + 23 * s, T.Fit(row.texts[3] or "Completed", w - 6 * s, T.font.small), C.muted, T.font.small)
      end)
      if ui.IsItemHovered() then ui.SetTooltip(title) end
      if hit then eq.ListClick(WINDOW, "QUEST_TaskList", row.row, "left", 1) end
    end
  end
  if shown == 0 then K.Empty(needle == "" and "No completed quests." or "No matching quests.") end
end

local function objectiveTable(rows, s)
  if #rows == 0 then K.Empty("No objectives to show."); return end
  local avail = ui.GetContentRegionAvail()
  local zoneW = math.max(85 * s, math.min(155 * s, avail * 0.29))
  ui.PushFont(T.font.small)
  ui.PushStyleVar(ui.StyleVar.CellPadding, 4 * s, 1 * s)
  if ui.BeginTable("##questObjectives", 3, ui.TableFlags.SizingFixedFit, 0, 0) then
    ui.TableSetupColumn("Objective", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("Count", ui.TableColumnFlags.WidthFixed, 42 * s)
    ui.TableSetupColumn("Zone", ui.TableColumnFlags.WidthFixed, zoneW)
    ui.TableHeadersRow()
    for _, e in ipairs(rows) do
      ui.TableNextRow()
      ui.TableNextColumn()
      if e.separator then
        ui.Dummy(0, 4 * s)
      else
        local color = e.hidden and C.muted or e.done and C.pet or C.text
        local x, y = ui.GetCursorScreenPos()
        local _, lh = ui.CalcTextSize("Ag")
        if not e.hidden then K.Tick(x, y + (lh - 9 * s) / 2, 9 * s, e.done) end
        ui.Dummy(11 * s, lh)
        ui.SameLine(0, 4 * s)
        local label = e.text or ""
        local width = ui.GetContentRegionAvail()
        ui.TextColored(color[1], color[2], color[3], 1, T.Fit(label, width, T.font.small))
        if ui.IsItemHovered() and T.TextSize(label, T.font.small) > width then ui.SetTooltip(label) end
        ui.TableNextColumn()
        K.Muted(e.hidden and "" or (e.status or ""), e.done and C.pet or C.muted)
        ui.TableNextColumn()
        local zone = e.zone or ""
        if zone == "ALL" then zone = "Anywhere" end
        K.Muted(T.Fit(zone, ui.GetContentRegionAvail(), T.font.small))
        if ui.IsItemHovered() then ui.SetTooltip(zone) end
      end
    end
    ui.EndTable()
  end
  ui.PopStyleVar()
  ui.PopFont()
end

local function historyObjectives(s)
  local rows = K.Rows(WINDOW, "QUEST_TaskElementList")
  local objectives = {}
  for _, row in ipairs(rows) do
    local label = row.texts[1] or ""
    local separator = label == "" and (row.texts[2] or "") == "" and (row.texts[3] or "") == ""
    objectives[#objectives + 1] = { separator = separator, text = label, status = row.texts[2], zone = row.texts[3],
      hidden = not separator and label:match("^[%s%?]*$") ~= nil, done = row.texts[2] == "Done" }
  end
  objectiveTable(objectives, s)
end

local SECTIONS = { { "Objectives", "objectives" }, { "Description", "description" }, { "Rewards", "rewards" } }
local function sectionTabs(key, s)
  local section = ui.GetSetting("tasks.section." .. key) or "objectives"
  if section ~= "description" and section ~= "rewards" then section = "objectives" end
  for i, tab in ipairs(SECTIONS) do
    if i > 1 then ui.SameLine(0, 5 * s) end
    if button(tab[1], "section." .. tab[2], section == tab[2], s) then
      section = tab[2]
      ui.SetSetting("tasks.section." .. key, section)
    end
  end
  ui.Dummy(0, 2 * s)
  return section
end

local function activeDetail(task, s)
  if not task then K.Empty("Select a task to see its objectives."); return end
  K.Muted(KIND[task.type] or "Task", C.gold)
  ui.SameLine(0, 0)
  local x, y = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x + math.max(0, ui.GetContentRegionAvail() - 85 * s), y - 3 * s)
  local tracked = Q.isTracked(task)
  if button(tracked and "Tracked" or "Track", "trackQuest", tracked, s) then Q.setTracked(task, not tracked) end
  if ui.IsItemHovered() then ui.SetTooltip("Show this quest in the independent Quest Tracker") end
  ui.PushFont(T.font.large)
  ui.PushTextWrapPos(0)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, task.title or "")
  ui.PopTextWrapPos()
  ui.PopFont()
  local done, total = Q.stats(task)
  local timed = (task.duration or 0) > 0
  local label = timed and ((task.left or 0) > 0 and Q.time(task.left) .. " left" or "Out of time") or "No time limit"
  local color = timed and (task.left or 0) <= 120 and C.danger or C.muted
  K.Muted(label, color)
  ui.SameLine(0, 12 * s)
  K.Muted(string.format("%d / %d complete", done, total))
  local bx, by = ui.GetCursorScreenPos()
  local bw = ui.GetContentRegionAvail()
  ui.DrawRectFilled(bx, by, bx + bw, by + 3 * s, C.inset[1], C.inset[2], C.inset[3], 1, 2 * s)
  if total > 0 and done > 0 then ui.DrawRectFilled(bx, by, bx + bw * done / total, by + 3 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s) end
  ui.Dummy(0, 6 * s)
  local section = sectionTabs(tostring(task.id), s)
  local _, lh = ui.CalcTextSize("Ag")
  local tasks = Q.tasks()
  local hasTimer = (tasks.requestTimerLeft or 0) > 0
  local footH = lh + 18 * s + (hasTimer and lh + 5 * s or 0)
  ui.BeginChild("##questContent", 0, -footH, false)
  if section == "objectives" then objectiveTable(task.objectives or {}, s)
  elseif section == "description" then
    if K.HasText(task.description) then K.Stml(WINDOW, "TASK_Description", task.description)
    else K.Empty("No description supplied.") end
  else
    if K.HasText(task.rewards) then K.Stml(WINDOW, "TASK_Description", task.rewards)
    elseif task.rewardSet then K.Empty("Reward details are available from the server.", "Use Preview Reward to view this quest's rewards.")
    else K.Empty("Reward preview unavailable.", "No reward details supplied for this quest.") end
  end
  ui.EndChild()
  ui.Separator()
  if hasTimer then
    K.Muted("Next request in " .. Q.time(tasks.requestTimerLeft))
  end
  W.Button(WINDOW, "TASK_PreviewRewardButton", "Preview Reward", { height = lh + 8 * s,
    tooltip = "Ask the server to preview the selected task's rewards" })
  ui.SameLine(0, 8 * s)
  W.Button(WINDOW, "TASK_AbandonButton", "Abandon...", { danger = true, height = lh + 8 * s,
    tooltip = "The game asks for confirmation before abandoning this task" })
end

local function historyDetail(rows, selected, s)
  local task
  for _, row in ipairs(rows) do if row.row == selected then task = row; break end end
  if not task then K.Empty("Select a completed quest to review it."); return end
  K.Muted("Completed", C.pet)
  ui.PushFont(T.font.large)
  ui.PushTextWrapPos(0)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, task.texts[2] or "")
  ui.PopTextWrapPos()
  ui.PopFont()
  K.Muted(task.texts[3] or "")
  local section = sectionTabs("history", s)
  ui.BeginChild("##questHistoryContent", 0, 0, false)
  if section == "objectives" then historyObjectives(s)
  elseif section == "description" then K.Empty("No description in completed-task history.")
  else K.Empty("No reward details in completed-task history.") end
  ui.EndChild()
end

local WINDOW_OPTS = {
  size = function() return 820, 560 end,
  resizable = { 620, 360 },
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 30 end,
}
local function drawWindow(_, _, _, _, s)
  local tasks = Q.tasks()
  sync(tasks)
  local history, historySelected = K.Rows(WINDOW, "QUEST_TaskList")
  categories(tasks, history, s)
  sharedMembers(tasks, s)
  local width, height = ui.GetContentRegionAvail()
  local listW = math.max(155 * s, math.min((tonumber(ui.GetSetting("tasks.listw")) or state.listW) * s, width * 0.38))
  ui.BeginChild("##questSidebar", listW, height, false)
  local needle = K.Search(state, "search", state.category == "history" and "Search history" or "Search quests")
  local _, lh = ui.CalcTextSize("Ag")
  ui.BeginChild("##questList", 0, -(lh + 18 * s), false)
  if state.category == "history" then historyList(history, historySelected, needle, s)
  else taskList(tasks, needle, s) end
  ui.EndChild()
  local visible = Q.trackerVisible() and (#Q.getTracked(tasks) > 0 or ui.GetSetting("tasks.tracker.visible") == "1")
  if ui.Button(visible and "Hide tracker" or "Show tracker", -1, lh + 8 * s) then Q.setTrackerVisible(not visible) end
  ui.EndChild()
  ui.SameLine(0, 0)
  local x, y = ui.GetCursorScreenPos()
  T.Hit("##questSplit", x + 2 * s, y, x + 10 * s, y + height)
  local hot = ui.IsItemHovered() or ui.IsItemActive()
  if ui.IsItemActive() then
    local mx = ui.GetMousePos()
    local wx = ui.GetWindowPos()
    state.listW = math.max(155, math.min((mx - wx - 10 * s) / s, width * 0.38 / s))
    ui.SetSetting("tasks.listw", tostring(math.floor(state.listW)))
  end
  ui.DrawLine(x + 6 * s, y, x + 6 * s, y + height, C.ink[1], C.ink[2], C.ink[3], hot and 0.30 or 0.10, 1)
  ui.SetCursorScreenPos(x + 14 * s, y)
  ui.BeginChild("##questDetail", 0, height, false)
  if state.category == "history" then historyDetail(history, historySelected, s)
  else activeDetail(find(tasks, state.selected), s) end
  ui.EndChild()
end

ui.window("Tasks", function()
  if not eq.InGame() then
    state.loaded, state.selected, state.nativeId, state.categoryPref = false, nil, nil, nil
    state.openMembers, state.memberName, state.memberTaskId = nil, nil, nil
    state.category, state.search = "active", ""
    Q.invalidate()
    return
  end
  hud.Dialog(WINDOW, "Quest Journal", WINDOW_OPTS, drawWindow)
end)
