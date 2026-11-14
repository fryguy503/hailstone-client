-- Shared journal/tracker state. The authoritative snapshot is independent of TaskWnd's visibility and selection.
-- dofile is used by both windows, so the snapshot cache is shared, while watch flags live in character settings.
local Q = {}
modern_task_journal_cache = modern_task_journal_cache or { frame = -1 }
local cache = modern_task_journal_cache

function Q.invalidate() cache.frame = -1 end

function Q.tasks()
  local frame = ui.GetFrameCount()
  if cache.frame ~= frame then
    cache.value = eq.Tasks() or {}
    cache.frame = frame
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
