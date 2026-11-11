-- Lua Scripts (gamepad pack): Hailstone's own ScriptsWnd (docs/plugins.md "The Scripts window"), from More. The
-- MacroQuest Lua scripts found in the script folders (eq.Scripts), one row each with its state: A runs or stops it
-- (/lua run, /lua stop), X pauses or resumes it, Y puts it on this character's autorun list or takes it off (started
-- at each login once the server's rules allow it). A script the rules do not allow is greyed with the reason; the
-- server's own scripts under its hailstone.cfg cannot be stopped.
if ui.ManagedDistribution and ui.ManagedDistribution() then return end
local T = dofile("lib/theme.lua")
local W = dofile("lib/window.lua")
local C = T.C
local WIN = "ScriptsWnd"

local listState = {}
local wasShown = false
local note = nil -- the last Run's refusal

local STATUS = { running = "Running", paused = "Paused", exited = "Ended", error = "Error" }

local function toggle(r)
  if r.status == "running" or r.status == "paused" then
    eq.ScriptStop(r.name)
  elseif r.allowed and r.found then
    local ok, why = eq.ScriptRun(r.name)
    note = not ok and why or nil
  end
end

ui.window("Lua Scripts", function()
  if not W.shown(WIN) then
    wasShown, note = false, nil
    return
  end
  T.frame()
  local list, info = eq.Scripts(not wasShown)
  if not wasShown then listState.focusNext = 1 end
  wasShown = true
  local running = 0
  for _, r in ipairs(list) do
    if r.status == "running" or r.status == "paused" then running = running + 1 end
  end
  local hints = { { "a", "Run / stop" }, { "x", "Pause" }, { "b", "Close" } }
  if info.autorun then table.insert(hints, 3, { "y", "Autorun" }) end
  if W.begin("Lua Scripts", { w = 1300, h = 900, title = "Lua Scripts",
        subtitle = string.format("%d found, %d running", #list, running), hints = hints }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    local foot = T.px(40)
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local rowH = 70
    local visible = math.max(1, math.floor((y1 - y0 - foot + T.px(10)) / (T.px(rowH) + T.px(10))))
    if #list == 0 then
      T.font(20)
      T.draw(x0, y0 + T.px(10), "No scripts found. Install one with the add-on manager on the login screen.", C.faint)
      T.unfont()
    else
      local row = W.list(WIN, listState, #list, visible, rowH, cw, function(i, rx0, ry0, rx1, ry1)
        local r = list[i]
        local live = r.status == "running" or r.status == "paused"
        T.font(22)
        T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(420), ry1 - T.px(22),
          T.fit(r.name .. (r.server and "  (server)" or ""), rx1 - rx0 - T.px(440)),
          (r.allowed or live) and C.text or C.faint, 0)
        T.unfont()
        T.font(15)
        local sub = r.error ~= "" and r.error or (not r.allowed and r.why ~= "" and r.why) or r.caps
        T.textIn(rx0 + T.px(18), ry0 + T.px(30), rx1 - T.px(420), ry1, T.fit(sub or "", rx1 - rx0 - T.px(440)), C.dim, 0)
        T.unfont()
        T.font(18)
        local st = STATUS[r.status] or (r.allowed and "Stopped" or "Not allowed")
        local sc = r.status == "error" and C.bad or (r.status == "running" and C.good or C.dim)
        T.textIn(rx1 - T.px(400), ry0, rx1 - T.px(200), ry1, st, sc, 0)
        T.textIn(rx1 - T.px(190), ry0, rx1 - T.px(20), ry1, r.autorun and "Autorun" or "", C.accent, 1)
        T.unfont()
      end, function(i) toggle(list[i]) end)
      if row then
        local r = list[row]
        if W.pressed("x") and (r.status == "running" or r.status == "paused") and not r.kept then
          eq.ScriptPause(r.name)
        end
        if W.pressed("y") and info.autorun then eq.SetScriptAutorun(r.name, not r.autorun) end
      end
    end
    T.font(15)
    T.draw(x0, y1 - foot + T.px(8), note or ("Script folders: " .. table.concat(info.folders or {}, "  ·  ")),
      note and C.bad or C.faint)
    T.unfont()
  end
  if W.finish() then eq.ShowWindow(WIN, false) end
end)
