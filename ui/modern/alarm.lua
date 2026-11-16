-- Alarm: replaces the game's AlarmWnd (/alarm: a reminder after a time). Hours and minutes as number boxes, a row of
-- quick choices that fill them, what the alarm will be ("in 1 h 30 min"), Set Alarm and Cancel. The boxes are the
-- window's own edit boxes (hs.SetWindowText; a quick choice types into both), and Set Alarm is its Set Time button,
-- so the window starts the alarm as the stock one does.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "AlarmWnd"
local HOURS, MINUTES = "ALRW_HoursEditbox", "ALRW_MinutesEditbox"

local QUICK = { { "5 min", 0, 5 }, { "15 min", 0, 15 }, { "30 min", 0, 30 }, { "1 hour", 1, 0 }, { "2 hours", 2, 0 } }

-- A number box for one of the edit boxes.
local function number(id, width)
  local current = W.Text(WINDOW, id)
  ui.SetNextItemWidth(width)
  local text = ui.InputText("##" .. id, current, 4, ui.InputTextFlags.CharsDecimal | ui.InputTextFlags.AutoSelectAll)
  if text ~= current then API.windows.SetText(WINDOW, id, text) end
  return math.max(0, math.floor(tonumber(text) or 0))
end

local WINDOW_OPTS = {
  size = function() return 340, 176 end,
  resizable = { 280, 160 },
  place = K.Centered,
}
local function drawWindow(x, y, w, h, s)
  K.Caption("Remind me after")
  local avail = ui.GetContentRegionAvail()
  local bw = math.min(70 * s, (avail - 100 * s) / 2)
  local hrs = number(HOURS, bw)
  ui.SameLine()
  ui.Text("hours")
  ui.SameLine(0, 16 * s)
  local mins = number(MINUTES, bw)
  ui.SameLine()
  ui.Text("minutes")
  -- quick choices, as many to a row as fit
  ui.Dummy(0, 2 * s)
  local lineW = 0
  for i, q in ipairs(QUICK) do
    local qw = ui.CalcTextSize(q[1]) + 14 * s
    if i > 1 and lineW + 6 * s + qw <= avail then
      ui.SameLine(0, 6 * s)
      lineW = lineW + 6 * s + qw
    else
      lineW = qw
    end
    if ui.Button(q[1] .. "##quick", qw, 0) then
      API.windows.SetText(WINDOW, HOURS, tostring(q[2]))
      API.windows.SetText(WINDOW, MINUTES, tostring(q[3]))
    end
    if ui.IsItemHovered() then ui.SetTooltip("Fill in " .. q[1]) end
  end
  -- what it comes to
  local total = hrs * 60 + mins
  ui.Dummy(0, 2 * s)
  if total > 0 then
    local hh, mm = total // 60, total % 60
    local parts = {}
    if hh > 0 then parts[#parts + 1] = hh .. " h" end
    if mm > 0 then parts[#parts + 1] = mm .. " min" end
    W.Field("Alarm in", table.concat(parts, " "), C.accent)
  else
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Choose a time")
  end
  local bh = K.ButtonH(s)
  local _, cy = ui.GetCursorPos()
  local _, wh = ui.GetWindowSize()
  local by = wh - bh - 8 * s
  if by > cy then ui.SetCursorPos(10 * s, by) end
  W.ButtonRow(WINDOW, {
    { "ALRW_OkButton", "Set Alarm", { primary = true, tooltip = "Start the alarm" } },
    { "ALRW_CancelButton", "Cancel" },
  }, bh)
end

ui.window("Alarm", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Alarm", WINDOW_OPTS, drawWindow)
end)
