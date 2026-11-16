-- Offered tasks: replaces the game's TaskSelectWnd (an NPC's task offer). The offers as cards (title, how long it
-- runs), the selected one's description and first objectives at the right, Accept, Preview Reward and Decline All.
-- The cards are the original's TaskList (a click selects there: hs.ListClick), the text its Description, the buttons
-- its own, so accepting is the game's (closing the window declines, as the stock window does).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "TaskSelectWnd"

local WINDOW_OPTS = {
  size = function() return 560, 380 end,
  resizable = { 380, 260 },
  keepTitle = true,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 60 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local rows, selected = K.Rows(WINDOW, "TaskList")
  local listW = math.max(160 * s, math.min(220 * s, w * 0.42))
  local _, availH = ui.GetContentRegionAvail()
  local bodyH = availH - btnH - 10 * s
  ui.BeginChild("##offers", listW, bodyH, false)
  K.Muted(#rows == 1 and "1 task offered" or (#rows .. " tasks offered"), C.faint)
  for _, r in ipairs(rows) do
    local dur = r.texts[2] or ""
    if K.Card("offer" .. r.row, r.row == selected, dur ~= "" and 44 or 30, function(cx, cy, cw, ch)
      local _, th = T.TextSize("Ag", T.font.normal)
      local ty = dur ~= "" and cy + 6 * s or cy + (ch - th) / 2
      T.Text(cx, ty, T.Fit(r.texts[1] or "", cw - 8 * s), C.text)
      if dur ~= "" then T.Text(cx, ty + th + 2 * s, "Time limit " .. dur, C.muted, T.font.small) end
    end) then
      API.windows.ListClick(WINDOW, "TaskList", r.row, "left", 0)
    end
    if ui.IsItemHovered() then ui.SetTooltip(r.texts[1] or "") end
  end
  if #rows == 0 then K.Empty("Nothing on offer.") end
  ui.EndChild()
  ui.SameLine(0, 12 * s)
  local dx, dy = ui.GetCursorScreenPos()
  ui.DrawLine(dx - 6 * s, dy, dx - 6 * s, dy + bodyH, C.ink[1], C.ink[2], C.ink[3], 0.07, 1)
  ui.BeginChild("##offer", 0, bodyH, false)
  local picked
  for _, r in ipairs(rows) do if r.row == selected then picked = r end end
  if picked then
    ui.PushFont(T.font.large)
    ui.PushTextWrapPos(0)
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, picked.texts[1] or "")
    ui.PopTextWrapPos()
    ui.PopFont()
    if (picked.texts[2] or "") ~= "" then K.Chip("Time limit " .. picked.texts[2], C.gold)
    else K.Chip("No time limit", C.faint) end
    -- the description's step sections ("[2,Turn in ...]": the text the journal shows at step 2) as a numbered
    -- list of their own; the rest as written
    local steps = {}
    local desc = W.Text(WINDOW, "Description"):gsub("%[(%d[%d,]*),([^%]]*)%]", function(nums, text)
      steps[#steps + 1] = { tonumber(nums:match("^%d+")) or (#steps + 1), text }
      return ""
    end)
    if K.HasText(desc) then
      K.Heading("Details")
      K.Stml(WINDOW, "Description", desc)
    elseif #steps == 0 then
      K.Heading("Details")
      K.Muted("No description given.", C.faint)
    end
    if #steps > 0 then
      K.Heading("Steps", #steps == 1 and "1 step" or (#steps .. " steps"))
      for _, st in ipairs(steps) do
        ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, st[1] .. ".")
        ui.SameLine()
        ui.PushTextWrapPos(0)
        ui.TextColored(C.text[1], C.text[2], C.text[3], 1, st[2])
        ui.PopTextWrapPos()
      end
    end
  else
    K.Empty("Select a task to read about it.")
  end
  ui.EndChild()
  ui.Dummy(0, 4 * s)
  W.ButtonRow(WINDOW, {
    { "AcceptButton", "Accept", { primary = true, tooltip = "Take on the selected task" } },
    { "TSEL_PreviewRewardButton", "Preview Reward", { tooltip = "View the selected task's rewards" } },
    { "DeclineButton", "Decline All", { tooltip = "Turn down every task offered" } },
  }, btnH)
end

ui.window("Task Window", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Offered Tasks", WINDOW_OPTS, drawWindow)
end)
