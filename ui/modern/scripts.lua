-- Lua Scripts: Hailstone's own window (ScriptsWnd; docs/plugins.md "The Scripts window"), the EQ menu's Lua Scripts
-- row. The MacroQuest Lua scripts found in the script folders (eq.Scripts), each with its state and Run / Stop and
-- Pause / Resume, as /lua run, /lua stop and /lua pause do; Autorun puts it on this character's list, started at
-- each login once the server's rules allow it (and kept running across zones, as every script is). A script the
-- rules do not allow is greyed with the reason; the server's own scripts under its hailstone.cfg cannot be stopped.
if ui.ManagedDistribution and ui.ManagedDistribution() then return end
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "ScriptsWnd"

local args = ""         -- the arguments Run passes
local filter = ""
local wasShown = false

local STATUS = { running = "Running", paused = "Paused", exited = "Ended", error = "Error" }

local function colored(c, text) ui.TextColored(c[1], c[2], c[3], 1, text) end

local function tooltip(r)
  if not ui.IsItemHovered() then return end
  local lines = { r.name }
  if r.server then lines[#lines + 1] = "The server's script" end
  if r.caps ~= "" then lines[#lines + 1] = "May: " .. r.caps end
  if not r.allowed and r.why ~= "" then lines[#lines + 1] = "Cannot run: " .. r.why end
  if not r.found then lines[#lines + 1] = "Its file is no longer in the script folders" end
  if r.error ~= "" then lines[#lines + 1] = "Error: " .. r.error end
  if r.kept then lines[#lines + 1] = "This server keeps it running" end
  ui.SetTooltip(table.concat(lines, "\n"))
end

local WINDOW_OPTS = {
  size = function() return 520, 420 end,
  resizable = { 380, 260 },
}
local function drawWindow(x, y, w, h, s)
  local list, info = eq.Scripts(not wasShown) -- the folders read again when the window opens
  wasShown = true

  ui.SetNextItemWidth(160 * s)
  filter = ui.InputTextWithHint("##filter", "Find a script", filter, 64)
  ui.SameLine()
  ui.SetNextItemWidth(math.max(80 * s, w - 260 * s))
  args = ui.InputTextWithHint("##args", "Arguments for Run", args, 256)
  if ui.IsItemHovered() then ui.SetTooltip("Run passes these to the script, as /lua run <name> <arguments>") end
  ui.SameLine()
  if ui.SmallButton("Rescan") then list, info = eq.Scripts(true) end

  local _, lh = ui.CalcTextSize("Ag")
  local footer = 2 * lh + 10 * s
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
  local shown = 0
  if ui.BeginTable("##scripts", 4, flags, 0, -footer) then
    ui.TableSetupColumn("Script", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("State", ui.TableColumnFlags.WidthFixed, 70 * s)
    ui.TableSetupColumn("Autorun", ui.TableColumnFlags.WidthFixed, 60 * s)
    ui.TableSetupColumn("##do", ui.TableColumnFlags.WidthFixed, 130 * s)
    ui.TableHeadersRow()
    local f = filter:lower()
    for _, r in ipairs(list) do
      if f == "" or r.name:find(f, 1, true) then
        shown = shown + 1
        ui.PushID(r.name)
        ui.TableNextRow()
        ui.TableNextColumn()
        colored((r.allowed or r.status ~= "") and C.text or C.faint, r.name .. (r.server and "  (server)" or ""))
        tooltip(r)
        ui.TableNextColumn()
        local st = STATUS[r.status]
        if st then
          colored(r.status == "error" and C.danger or (r.status == "running" and C.good or C.muted), st)
        elseif not r.allowed then
          colored(C.faint, "Not allowed")
        else
          colored(C.faint, "Stopped")
        end
        tooltip(r)
        ui.TableNextColumn()
        ui.BeginDisabled(not info.autorun)
        local on, changed = ui.Checkbox("##autorun", r.autorun)
        if changed then eq.SetScriptAutorun(r.name, on) end
        ui.EndDisabled()
        if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
          ui.SetTooltip(info.autorun and "Start it each time this character logs in" or "No character yet")
        end
        ui.TableNextColumn()
        local live = r.status == "running" or r.status == "paused"
        if live then
          ui.BeginDisabled(r.kept)
          if ui.SmallButton("Stop") then eq.ScriptStop(r.name) end
          ui.SameLine()
          if ui.SmallButton(r.status == "paused" and "Resume" or "Pause") then eq.ScriptPause(r.name) end
          ui.EndDisabled()
        else
          ui.BeginDisabled(not r.allowed or not r.found)
          if ui.SmallButton("Run") then eq.ScriptRun(r.name, args) end
          ui.EndDisabled()
        end
        ui.PopID()
      end
    end
    ui.EndTable()
  end
  if #list == 0 then
    colored(C.muted, "No scripts found. Put <name>.lua or <name>/init.lua in a script folder, or install one with")
    colored(C.muted, "the add-on manager on the login screen.")
  elseif shown == 0 then
    colored(C.muted, "No script matches.")
  else
    local folders = info.folders or {}
    colored(C.faint, "Script folders: " .. table.concat(folders, "  ·  "))
  end
end

ui.window("Lua Scripts", function()
  if not eq.InGame() then
    wasShown = false
    return
  end
  hud.Dialog(WINDOW, "Lua Scripts", WINDOW_OPTS, drawWindow)
end)
