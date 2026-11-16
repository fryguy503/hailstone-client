-- Shared journal/tracker state. The authoritative snapshot is independent of TaskWnd's visibility and selection.
-- dofile is used by both windows, so the snapshot cache is shared, while watch flags live in character settings.
local API = hs.UI
local Q = {}
modern_task_journal_cache = modern_task_journal_cache or { frame = -1 }
local cache = modern_task_journal_cache
-- hs.Tasks() builds every quest's text and objectives, too much to redo every frame: it is read again when the
-- task list or the selection changes (hs.TasksRevision, cheap), after invalidate(), and every REFRESH seconds for
-- the countdowns it carries.
local REFRESH = 0.5

function Q.invalidate() cache.frame = -1 end

-- A quest's per-character keys at their defaults; the host drops a key set to its default from the file.
local function forget(id)
  ui.SetSetting("tasks.tracked." .. id, "0")
  ui.SetSetting("tasks.tracker.collapsed." .. id, "0")
  ui.SetSetting("tasks.section." .. id, "objectives")
end

-- A quest that leaves the list while in game (completed, abandoned, failed) forgets its flags, so taking it again
-- does not bring back its old tracking. The list is kept across zoning; a new connection starts it afresh (its
-- revision restarts, and out of game the list is empty), so only a later revision of the same session prunes.
local function prune(old, new)
  if not (old.revision and new.revision) or new.revision <= old.revision then return end
  local present = {}
  for _, t in ipairs(new) do present[t.id] = true end
  for _, t in ipairs(old) do
    if t.id and not present[t.id] then forget(tostring(t.id)) end
  end
end

function Q.tasks()
  local frame = ui.GetFrameCount()
  if cache.frame == frame then return cache.value end
  local revision, selected
  if API.tasks.Revision then revision, selected = API.tasks.Revision() end
  local now = ui.GetTime and ui.GetTime() or 0
  local fresh = cache.frame == -1 or not API.tasks.Revision or revision ~= cache.revision or
    selected ~= cache.selected or now - (cache.time or 0) >= REFRESH or now < (cache.time or 0)
  cache.frame = frame
  if fresh then
    local old = cache.value
    cache.value = API.tasks.List() or {}
    cache.time, cache.revision, cache.selected = now, revision, selected
    if old and old.revision and old.revision > 0 then prune(old, cache.value) end
  end
  return cache.value
end

local function idOf(task)
  return type(task) == "table" and task.id or task
end

function Q.isTracked(task)
  local id = idOf(task)
  return id ~= nil and ui.GetSetting("tasks.tracked." .. tostring(id)) == "1"
end

function Q.trackerVisible() return ui.GetSetting("tasks.tracker.visible") ~= "0" end
function Q.setTrackerVisible(on) ui.SetSetting("tasks.tracker.visible", on and "1" or "0") end

function Q.setTracked(task, on)
  local id = idOf(task)
  if not id then return end
  ui.SetSetting("tasks.tracked." .. tostring(id), on and "1" or "0")
  if on then Q.setTrackerVisible(true) end
end

function Q.getTracked(tasks)
  local out = {}
  for _, task in ipairs(tasks or Q.tasks()) do
    if Q.isTracked(task) then out[#out + 1] = task end
  end
  return out
end

function Q.stats(task)
  if task.done ~= nil and task.total ~= nil then return task.done, task.total end
  local done, total = 0, 0
  for _, e in ipairs(task.objectives or {}) do
    if not e.separator then
      total = total + 1
      if e.done and not e.hidden then done = done + 1 end
    end
  end
  return done, total
end

function Q.time(seconds)
  local n = math.max(0, math.floor(tonumber(seconds) or 0))
  if n >= 86400 then return string.format("%dd %dh", n // 86400, n % 86400 // 3600) end
  if n >= 3600 then return string.format("%dh %02dm", n // 3600, n % 3600 // 60) end
  if n >= 60 then return string.format("%dm %02ds", n // 60, n % 60) end
  return n .. "s"
end

function Q.inZone(task)
  for _, e in ipairs(task.objectives or {}) do
    if not e.hidden and not e.done and e.currentZone then return true end
  end
  return false
end

return Q
