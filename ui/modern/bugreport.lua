-- Bug report: replaces the game's BugReportWindow (/bugreport). The category, the report in a large text box, the
-- location the report is about (taken when the window opens; Update Location takes it again), the optional
-- information as check boxes (with the target's name beside its box), Cancel and Send (the window enables it once
-- there is text). Every piece is the window's child (hs.ComboSelect, hs.SetWindowText, hs.ClickChild), so the
-- report the server gets is the window's. After Send the window shows the report saved for the player to post in
-- the bug report forum (bug-reports/<file>.zip): Open Folder, Copy Summary (the forum post) and Done.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "BugReportWindow"

-- A saved report (BugReportWindow after Send, CrashReportWindow): its file, size, what was left out, and how to post
-- it. Returns the height it took.
local function savedReport(win, s, intro)
  ui.TextWrapped(intro)
  ui.Dummy(0, 4 * s)
  K.Caption("File")
  ui.SameLine(70 * s)
  ui.Text(W.Text(win, "SavedName"))
  if ui.IsItemHovered() then ui.SetTooltip(W.Text(win, "SavedFolder")) end
  K.Caption("Size")
  ui.SameLine(70 * s)
  ui.Text(W.Text(win, "SavedInfo"))
  local left = W.Text(win, "SavedLeftOut")
  if left ~= "" then
    ui.SameLine()
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "(" .. left .. ")")
  end
  ui.Dummy(0, 4 * s)
  ui.TextWrapped("To post it: open the bug report forum on Discord, start a post, paste the summary (Copy Summary) "
    .. "and attach the file (Open Folder).")
  ui.TextWrapped("The file holds the report, your logs, hailstone.ini and screenshots. What was said in chat, your "
    .. "account names and your home folder are taken out; anything else in your logs or screenshots is posted too.")
end

local WINDOW_OPTS = {
  size = function() return 440, 400 end,
  resizable = { 320, 320 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  if W.Text(WINDOW, "SavedName") ~= "" then -- sent and saved
    savedReport(WINDOW, s, "Thanks! Your report was saved with your logs and screenshots.")
    ui.Dummy(0, 8 * s)
    W.ButtonRow(WINDOW, {
      { "OpenFolderButton", "Open Folder", { tooltip = "Show the report file in your file manager", hidden = false } },
      { "CopySummaryButton", "Copy Summary", { tooltip = "Copy a forum post describing the report", hidden = false } },
      { "DoneButton", "Done", { primary = true, hidden = false } },
    }, bh)
    return
  end
  local _, lh = ui.CalcTextSize("Ag")
  K.Caption("Category")
  ui.SameLine(90 * s)
  W.Combo(WINDOW, "BugTypes")
  if ui.IsItemHovered() then ui.SetTooltip("What kind of problem this is") end
  -- the location
  K.Caption("Location")
  ui.SameLine(90 * s)
  ui.Text(string.format("%s, %s, %s", W.Text(WINDOW, "LocX"), W.Text(WINDOW, "LocY"), W.Text(WINDOW, "LocZ")))
  ui.SameLine()
  W.Button(WINDOW, "LocationButton", "Update", { tooltip = "Use where you stand now as the report's location" })
  ui.Dummy(0, 2 * s)
  -- the report
  K.Caption("What happened? (what you did, what you expected, what you saw)")
  local below = K.FooterH(s) + 2 * (lh + 10 * s) + lh + 12 * s
  local _, avail = ui.GetContentRegionAvail()
  local ex, ey = ui.GetCursorScreenPos()
  W.Edit(WINDOW, "Edit", { multiline = true, height = math.max(60 * s, avail - below), max = 4096 })
  if W.Text(WINDOW, "Edit") == "" and not ui.IsItemActive() then -- a hint while empty
    T.Text(ex + 6 * s, ey + 4 * s, "Type the report here", C.faint, T.font.small)
  end
  -- the optional information
  W.Heading("Include")
  local col = math.max(150 * s, ui.GetContentRegionAvail() / 2)
  local x0 = ui.GetCursorPos()
  W.Check(WINDOW, "DupButton", "Can be repeated", { tooltip = "You can make it happen again" })
  ui.SameLine(x0 + col)
  W.Check(WINDOW, "CrashButton", "Crash", { tooltip = "The game crashed" })
  W.Check(WINDOW, "FlagsButton", "Character flags", { tooltip = "Send your character's flags with the report" })
  ui.SameLine(x0 + col)
  local target = W.Text(WINDOW, "TargetName")
  W.Check(WINDOW, "TargetButton", target ~= "" and ("Target: " .. target) or "Target info",
    { tooltip = "Send your current target's information with the report" })
  W.ButtonRow(WINDOW, {
    { "CancelButton", "Cancel", { tooltip = "Close without sending" } },
    { "SendButton", "Send", { primary = true, tooltip = "Send the report to the server and save it with your logs and screenshots in bug-reports/" } },
  }, bh)
end

ui.window("Bug Report", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Bug Report", WINDOW_OPTS, drawWindow)
end)

-- Crash report: shown at the start after Hailstone crashed (CrashReportWindow, also at the login screen). The report
-- is already saved; a note on what the player was doing can be added to it (Save Note writes it again).
local CRASH = "CrashReportWindow"
local WINDOW_OPTS2 = {
  size = function() return 460, 330 end,
  resizable = { 360, 300 },
  place = function(W_, H_, w, h) return K.Centered(W_, H_, w, h) end,
}
local function drawWindow2(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local _, lh = ui.CalcTextSize("Ag")
  savedReport(CRASH, s, "Hailstone closed unexpectedly last time. A crash report was saved with the log from "
    .. "that session.")
  K.Caption("What were you doing when it crashed? (optional)")
  local _, avail = ui.GetContentRegionAvail()
  local ex, ey = ui.GetCursorScreenPos()
  W.Edit(CRASH, "Edit", { multiline = true, height = math.max(50 * s, avail - K.FooterH(s) - lh - 8 * s), max = 4096 })
  if W.Text(CRASH, "Edit") == "" and not ui.IsItemActive() then
    T.Text(ex + 6 * s, ey + 4 * s, "e.g. zoning into the Commonlands while my pet was fighting", C.faint, T.font.small)
  end
  local note = W.Text(CRASH, "NoteSaved")
  if note ~= "" then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, note) else ui.Dummy(0, lh) end
  W.ButtonRow(CRASH, {
    { "SaveNoteButton", "Save Note", { tooltip = "Add your note to the report", hidden = false } },
    { "OpenFolderButton", "Open Folder", { tooltip = "Show the report file in your file manager", hidden = false } },
    { "CopySummaryButton", "Copy Summary", { tooltip = "Copy a forum post describing the crash", hidden = false } },
    { "CloseButton", "Close", { primary = true, hidden = false } },
  }, bh)
end

ui.window("Crash Report", function()
  hud.Dialog(CRASH, "Hailstone Crashed", WINDOW_OPTS2, drawWindow2)
end)
