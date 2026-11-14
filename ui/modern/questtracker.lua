-- A HUD frame of this UI's own: watch flags and its visibility are independent of TaskWnd. The task snapshot is
-- read directly from the game's state, so closing the journal or selecting another task never changes this list.
local hud = dofile("lib/hud.lua")
local Q = dofile("lib/task_journal.lua")
local T = hud.T
local C = T.color
local KEY = "Quest Tracker"
local HEAD, FOOT, PAD = 28, 25, 9
local watched = {}

local function openJournal(task)
  if not task then eq.ShowWindow("TaskWnd", true); return end
  if eq.OpenTask(task.id) then
    ui.SetSetting("tasks.section." .. tostring(task.id), "objectives")
    ui.SetSetting("tasks.category", "active")
    Q.invalidate()
  end
end

local function collapseKey(task) return "tasks.tracker.collapsed." .. tostring(task.id) end

local function link(id, x, y, w, h, s, label, tooltip)
  local clicked = T.Hit(id, x, y, x + w, y + h)
  if hud.DragItem() then clicked = false end
  local over = ui.IsItemHovered()
  local _, th = T.TextSize(label, T.font.small)
  T.Text(x, y + (h - th) / 2, label, over and C.text or C.accent, T.font.small)
  if over then ui.SetTooltip(tooltip or label) end
  return clicked
end

local function title(task, x, y, w, s)
  local done, total = Q.stats(task)
  local count = tostring(done) .. "/" .. tostring(total)
  local cw = T.TextSize(count, T.font.tiny) + 7 * s
  local collapsed = ui.GetSetting(collapseKey(task)) == "1"
  local clicked = T.Hit("##collapse" .. task.id, x, y, x + 17 * s, y + 23 * s)
  local over = ui.IsItemHovered()
  T.Text(x + 6 * s, y + 4 * s, collapsed and ">" or "v", over and C.text or C.muted, T.font.small)
  if over then ui.SetTooltip(collapsed and "Expand objectives" or "Collapse objectives") end
  if clicked then
    collapsed = not collapsed
    ui.SetSetting(collapseKey(task), collapsed and "1" or "0")
  end
  local available = math.max(1, w - 18 * s - cw)
  local name = T.Fit(task.title or "Quest", available, T.font.small)
  if link("##quest" .. task.id, x + 18 * s, y, available, 23 * s, s, name, task.title or "Open quest") then
    openJournal(task)
  end
  T.Text(x + w, y + 5 * s, count, C.muted, T.font.tiny, "right")
  return collapsed
end

local function objective(e, x, w, s)
  local _, y = ui.GetCursorScreenPos()
  local count = ""
  if (tonumber(e.goal) or 0) > 0 then count = tostring(e.count or 0) .. "/" .. tostring(e.goal) end
  local cw = count ~= "" and (T.TextSize(count, T.font.tiny) + 8 * s) or 0
  ui.SetCursorScreenPos(x + 8 * s, y)
  ui.PushTextWrapPos(ui.GetCursorPos() + math.max(1, w - 8 * s - cw))
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, e.text or "")
  ui.PopTextWrapPos()
  ui.DrawCircleFilled(x + 2 * s, y + 7 * s, 1.5 * s, C.faint[1], C.faint[2], C.faint[3], 1)
  if count ~= "" then T.Text(x + w, y + 1 * s, count, C.muted, T.font.tiny, "right") end
  local _, after = ui.GetCursorScreenPos()
  if e.zone and e.zone ~= "" then
    ui.SetCursorScreenPos(x + 8 * s, after)
    ui.PushFont(T.font.tiny)
    ui.PushTextWrapPos(ui.GetCursorPos() + math.max(1, w - 8 * s))
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, e.zone)
    ui.PopTextWrapPos()
    ui.PopFont()
  end
  local _, bottom = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x, bottom + 3 * s)
end

local function quest(task, s)
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  local collapsed = title(task, x, y, w, s)
  ui.SetCursorScreenPos(x, y + 24 * s)
  if collapsed then return end

  if (task.duration or 0) > 0 and task.left ~= nil then
    ui.SetCursorScreenPos(x + 18 * s, select(2, ui.GetCursorScreenPos()))
    ui.TextColored(C.heading[1], C.heading[2], C.heading[3], 1, Q.time(task.left) .. " remaining")
  end

  -- This is a deliberately compact view, not another progression list: only the first three known unfinished
  -- objectives in the game's ordering. The journal retains every hidden row and the server's group boundaries.
  local shown = 0
  for _, e in ipairs(task.objectives or {}) do
    if not e.separator and not e.hidden and not e.done then
      objective(e, x + 18 * s, math.max(1, w - 18 * s), s)
      shown = shown + 1
      if shown == 3 then break end
    end
  end
  if shown == 0 then
    ui.SetCursorScreenPos(x + 18 * s, select(2, ui.GetCursorScreenPos()))
    ui.PushTextWrapPos(0)
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Open the journal for quest details.")
    ui.PopTextWrapPos()
  end
  local _, bottom = ui.GetCursorScreenPos()
  local _, total = Q.stats(task)
  local label = "All objectives (" .. total .. ")"
  if link("##all" .. task.id, x + 18 * s, bottom, w - 18 * s, 21 * s, s, label, "Open this quest in the journal") then
    openJournal(task)
  end
  ui.SetCursorScreenPos(x, bottom + 25 * s)
end

local OPTS = {
  shown = function()
    return Q.trackerVisible() and (#watched > 0 or ui.GetSetting("tasks.tracker.visible") == "1")
  end,
  size = function() return 300, 350 end,
  place = function(W, H, w, h, s)
    return math.max(0, W - w - 18 * s), math.max(0, math.min(280 * s, H - h - 18 * s))
  end,
  resizable = { 220, 100 },
  rollup = true,
  titleHeight = HEAD,
  onClose = function() Q.setTrackerVisible(false) end,
  menu = function()
    if ui.MenuItem("Quest Journal") then openJournal() end
    if ui.MenuItem("Hide tracker") then Q.setTrackerVisible(false) end
  end,
}

local function draw(x, y, w, h, s)
  local count = #watched
  local label = T.Fit("Objectives (" .. count .. ")", w - 104 * s, T.font.small)
  T.Text(x + PAD * s, y + 6 * s, label, C.heading, T.font.small)
  local ownLock = ui.GetSetting("hud.lock." .. KEY) == "1"
  local allLocked = ui.GetSetting("hud.locked") == "1"
  if T.Button("##trackerlock", x + w - 91 * s, y + 4 * s, x + w - 45 * s, y + 23 * s, s,
      ownLock and "Unlock" or "Lock", {
        enabled = not allLocked,
        tooltip = allLocked and "All frames are locked; use the frame menu to unlock them"
          or (ownLock and "Unlock tracker position and size" or "Lock tracker position and size"),
      }) then
    ui.SetSetting("hud.lock." .. KEY, ownLock and "0" or "1")
  end
  if T.Button("##trackerroll", x + w - 42 * s, y + 4 * s, x + w - 24 * s, y + 23 * s, s,
      OPTS.rolled and "+" or "-", { tooltip = OPTS.rolled and "Expand tracker" or "Roll up tracker" }) then
    hud.SetRolled(KEY, not OPTS.rolled)
  end
  if OPTS.rolled then return end
  ui.DrawLine(x + PAD * s, y + HEAD * s, x + w - PAD * s, y + HEAD * s, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)

  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx + PAD * s, y - wy + (HEAD + 4) * s)
  ui.PushStyleColor(ui.Col.ChildBg, 0, 0, 0, 0)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.BeginChild("##trackedquests", math.max(1, w - PAD * 2 * s), math.max(1, h - (HEAD + FOOT + 10) * s), false)
  ui.PopStyleVar()
  ui.PushFont(T.font.small)
  if count == 0 then
    ui.PushTextWrapPos(0)
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "No tracked quests. Use Track in the Quest Journal to add one.")
    ui.PopTextWrapPos()
  else
    for i, task in ipairs(watched) do
      if i > 1 then ui.Separator() end
      quest(task, s)
    end
  end
  ui.PopFont()
  ui.EndChild()
  ui.PopStyleColor()

  local fy = y + h - FOOT * s
  ui.DrawLine(x + PAD * s, fy - 3 * s, x + w - PAD * s, fy - 3 * s, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
  if link("##managequests", x + PAD * s, fy, w - 30 * s, 21 * s, s, "Quest Journal", "Manage tracked quests") then
    openJournal()
  end
end

ui.window("Quest Tracker", function()
  if not eq.InGame() then return end
  watched = Q.getTracked(Q.tasks())
  hud.Window(nil, KEY, OPTS, draw)
end)
