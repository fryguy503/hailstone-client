-- Forms and choices the game sends to the server (gamepad pack), each standing in for its stock window: every piece
-- is the window's child (eq.ComboSelect, eq.SetWindowText, eq.ListClick, eq.ClickChild), so what the server gets is
-- the window's. Text is typed with the on-screen keys (A on the text field; B leaves them).
--   Bug report (BugReportWindow, /bug)   category (A steps it), the location (Update takes where you stand now), the
--                                        report, what to include (repeatable, crash, flags, the target), Cancel, Send
--                                        (the window enables it once there is text; RB); then the report saved for
--                                        the bug report forum: Open Folder, Copy Summary, Done
--   Crash report (CrashReportWindow)     at the start after a crash: the saved report, a note (Save Note, RB),
--                                        Open Folder, Copy Summary, Close
--   Feedback (FeedbackWindow, /feedback) your name, the feedback, Cancel and Send (RB)
--   Claim (ClaimWnd, /claim)             the veteran rewards that can be claimed and the selected one's items;
--                                        Refresh, Cancel, Claim (the window enables it for a reward with any left)
--   Respawn (RespawnWnd)                 where to return after death: the server's choices (its default and
--                                        unavailable choices marked), the timer, and Respawn after its delay;
--                                        only the server closes it (no B)
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local G = dofile("lib/stock.lua")
local C = T.C

-- A row of buttons across x0..x1 at y; `list` = { { id, label, opts } } (those the window hides left out).
local function buttons(win, list, x0, x1, y, h)
  local shown = {}
  for _, b in ipairs(list) do if G.shown(win, b[1]) then shown[#shown + 1] = b end end
  local gap = T.px(12)
  local bw = (x1 - x0 - (#shown - 1) * gap) / math.max(1, #shown)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y - wy)
  for i, b in ipairs(shown) do
    if i > 1 then ui.SameLine(0, gap) end
    G.button(win, b[1], b[2], bw, h, b[3])
  end
end

-- A saved report (app/bug_report.h) from y0: what it is, its file and size, how to post it. Returns the y below it.
local function savedReport(win, x0, y0, w, intro)
  local y = G.wrapped(intro, x0, y0, w, C.text, 3) + T.px(16)
  T.draw(x0, y, "File: " .. G.text(win, "SavedName") .. "   (" .. G.text(win, "SavedInfo") .. ")", C.text)
  y = y + T.px(34)
  local left = G.text(win, "SavedLeftOut")
  if left ~= "" then
    T.draw(x0, y, left, C.dim)
    y = y + T.px(34)
  end
  y = G.wrapped("Post it in the bug report forum on Discord: paste the summary (Copy Summary) and attach the file "
    .. "from the bug-reports folder (Open Folder).", x0, y + T.px(8), w, C.dim, 3) + T.px(8)
  return G.wrapped("It holds the report, your logs, settings and screenshots. Chat, your account names and your home "
    .. "folder are taken out.", x0, y, w, C.dim, 2) + T.px(16)
end

-- A form with one long text: the fields drawn by body() (it returns the text field's clicked), the keys in their
-- place while the text is typed, Cancel and Send at the bottom (RB sends). p.saved(): the form's report is saved,
-- and p.savedBody draws that instead (with its own buttons).
local function form(p)
  local kb, editing, refocus = G.KF.new(), false, false
  ui.window(p.title, function()
    G.testShow()
    if not W.shown(p.screen) then
      editing = false
      return
    end
    T.frame()
    if p.saved and p.saved() then
      if W.begin(p.title, { w = 1400, h = 700, title = p.title, hints = { { "a", "Select" }, { "b", "Done" } } }) then
        p.savedBody(W.area())
      end
      if W.finish() then eq.ClickChild(p.screen, p.done, "left") end
      return
    end
    local hints = editing and G.editHints("Done")
      or { { "a", "Select" }, { "rb", p.sendLabel or "Send" }, { "b", p.cancelLabel or "Cancel" } }
    if W.begin(p.title, { w = 1400, h = 960, title = p.title, subtitle = p.subtitle and p.subtitle() or nil, hints = hints }) then
      local x0, y0, x1, y1, w = W.area()
      local wx, wy = ui.GetWindowPos()
      ui.SetCursorPos(x0 - wx, y0 - wy)
      if editing then
        T.heading(p.label, w)
        if G.edit(kb, p.screen, p.edit, x0, x1, { max = p.max, multiline = true, height = 200, keyHeight = 56 }) == "done" then
          editing, refocus = false, true
        end
      else
        local focus = refocus
        refocus = false
        if p.body(x0, y0, x1, y1, w, focus) then
          editing = true
          G.openKeys(kb, true)
        end
        if W.pressed("rb") and G.enabled(p.screen, p.send) then eq.ClickChild(p.screen, p.send, "left") end
        local bh = T.px(64)
        local list = { { p.cancel, p.cancelLabel or "Cancel", { glyph = "b", always = true } } }
        for _, b in ipairs(p.extra or {}) do list[#list + 1] = b end
        list[#list + 1] = { p.send, p.sendLabel or "Send", { primary = true, glyph = "rb", always = true } }
        buttons(p.screen, list, x0, x1, y1 - bh, bh)
      end
    end
    if W.finish() then
      if editing then editing, refocus = false, true else eq.ClickChild(p.screen, p.cancel, "left") end
    end
  end)
end

-- ---------------------------------------------------------------- bug report
local BUG = "BugReportWindow"
local OPEN = { "OpenFolderButton", "Open Folder", { always = true } }
local COPY = { "CopySummaryButton", "Copy Summary", { always = true } }
form({ screen = BUG, title = "Bug Report", edit = "Edit", max = 2049, send = "SendButton", cancel = "CancelButton",
  saved = function() return G.text(BUG, "SavedName") ~= "" end, done = "DoneButton",
  savedBody = function(x0, y0, x1, y1, w)
    savedReport(BUG, x0, y0, w, "Thanks! Your report was saved with your logs and screenshots.")
    local bh = T.px(64)
    buttons(BUG, { OPEN, COPY, { "DoneButton", "Done", { primary = true, glyph = "b", always = true } } }, x0, x1, y1 - bh, bh)
  end,
  label = "What happened? (what you did, what you expected, what you saw)",
  body = function(x0, y0, x1, y1, w, focus)
    local wx, wy = ui.GetWindowPos()
    local gap, rh = T.px(12), T.px(80)
    local half = (w - gap) / 2
    G.combo(BUG, "BugTypes", "Category", half, rh)
    ui.SameLine(0, gap)
    local loc = string.format("%s, %s, %s", G.text(BUG, "LocX"), G.text(BUG, "LocY"), G.text(BUG, "LocZ"))
    G.button(BUG, "LocationButton", "Location", half, rh, { sub = loc .. "   (A: where you stand now)", align = 0, always = true })
    ui.SetCursorPos(x0 - wx, y0 + rh + gap - wy)
    local checksH = 2 * T.px(64) + gap + T.px(34)
    local fh = (y1 - T.px(64) - gap) - (y0 + rh + gap) - checksH - gap
    local clicked = G.field(BUG, "Edit", "Report", w, fh, "What you did, what you expected, what you saw",
      { multiline = true, focus = focus })
    ui.SetCursorPos(x0 - wx, y0 + rh + gap + fh + gap - wy)
    T.heading("Include", w)
    local target = G.text(BUG, "TargetName")
    G.check(BUG, "DupButton", "Can be repeated", half, T.px(64))
    ui.SameLine(0, gap)
    G.check(BUG, "CrashButton", "The game crashed", half, T.px(64))
    G.check(BUG, "FlagsButton", "Character flags", half, T.px(64))
    ui.SameLine(0, gap)
    G.check(BUG, "TargetButton", target ~= "" and ("Target: " .. target) or "Target info", half, T.px(64))
    return clicked
  end })

-- ---------------------------------------------------------------- crash report
local CRASH = "CrashReportWindow"
form({ screen = CRASH, title = "Hailstone Crashed", edit = "Edit", max = 4096, send = "SaveNoteButton",
  sendLabel = "Save Note", cancel = "CloseButton", cancelLabel = "Close", extra = { OPEN, COPY },
  label = "What were you doing when it crashed?",
  body = function(x0, y0, x1, y1, w, focus)
    local wx, wy = ui.GetWindowPos()
    local y = savedReport(CRASH, x0, y0, w, "Hailstone closed unexpectedly last time. A crash report was saved with "
      .. "the log from that session.")
    ui.SetCursorPos(x0 - wx, y - wy)
    local note = G.text(CRASH, "NoteSaved")
    local clicked = G.field(CRASH, "Edit", "Note", w, y1 - y - T.px(64) - T.px(60), "What were you doing? (optional)",
      { multiline = true, focus = focus })
    if note ~= "" then T.draw(x0, y1 - T.px(64) - T.px(44), note, C.dim) end
    return clicked
  end })

-- ---------------------------------------------------------------- feedback
local FB = "FeedbackWindow"
form({ screen = FB, title = "Send Feedback", edit = "FBKW_FeedbackInput", max = 1023, send = "FBKW_SendButton",
  cancel = "FBKW_CancelButton", label = "Your feedback", subtitle = function() return G.text(FB, "FBKW_UserName") end,
  body = function(x0, y0, x1, y1, w, focus)
    return G.field(FB, "FBKW_FeedbackInput", "Feedback", w, y1 - y0 - T.px(64) - T.px(14), "Type your feedback here",
      { multiline = true, focus = focus })
  end })

-- ---------------------------------------------------------------- claim
local CLAIM = "ClaimWnd"
local rewards, items, side = { offset = 0 }, { offset = 0 }, {}
ui.window("Claim Rewards", function()
  if not W.shown(CLAIM) then return end
  T.frame()
  if W.begin("Claim Rewards", { w = 1500, h = 900, title = "Claim Rewards",
        hints = { { "a", "Select" }, { "x", "Refresh" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    if W.pressed("x") then eq.ClickChild(CLAIM, "ClaimRefreshButton", "left") end
    local gap, bh, rowH = T.px(24), T.px(64), 60
    local lw = (w - gap) * 0.55
    local iw = w - gap - lw
    local listTop = y0 + T.px(34)
    local visible = math.max(1, math.floor((y1 - bh - T.px(20) - listTop + T.px(10)) / (T.px(rowH) + T.px(10))))
    ui.SetCursorPos(x0 - wx, y0 - wy)
    T.heading("Rewards", lw)
    ui.SetCursorPos(x0 - wx, listTop - wy)
    local n, row = G.list(CLAIM, "ClaimFeatureList", rewards, visible, rowH, lw, function(i, t, rx0, ry0, rx1, ry1)
      local left = tonumber(t[2] or "") or 0
      local c = left > 0 and (i == rewards.selected and C.accent or C.text) or C.faint
      T.font(22)
      T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(110), ry1, T.fit(t[1] or "", rx1 - rx0 - T.px(130)), c, 0)
      T.textIn(rx1 - T.px(110), ry0, rx1 - T.px(18), ry1, (t[2] or "") .. " left", C.dim, 1)
      T.unfont()
    end)
    if n == 0 then T.draw(x0, listTop + T.px(10), "No rewards to claim", C.faint) end
    -- the selected reward's items
    local ix = x0 + lw + gap
    ui.SetCursorPos(ix - wx, y0 - wy)
    T.heading("Items in the selected reward", iw)
    ui.SetCursorPos(ix - wx, listTop - wy)
    local m = G.list(CLAIM, "ClaimItemList", items, visible, rowH, iw, function(i, t, rx0, ry0, rx1, ry1)
      T.font(22)
      T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(110), ry1, T.fit(t[1] or "", rx1 - rx0 - T.px(130)), C.text, 0)
      T.textIn(rx1 - T.px(110), ry0, rx1 - T.px(18), ry1, t[2] or "", C.dim, 1)
      T.unfont()
    end)
    if m == 0 then T.draw(ix, listTop + T.px(10), "Pick a reward on the left", C.faint) end
    buttons(CLAIM, { { "ClaimRefreshButton", "Refresh", { glyph = "x" } }, { "ClaimCancelButton", "Cancel", { glyph = "b" } },
      { "ClaimClaimButton", "Claim", { primary = true } } }, x0, x1, y1 - bh, bh)
  end
  if W.finish() then eq.ClickChild(CLAIM, "ClaimCancelButton", "left") end
end)

-- ---------------------------------------------------------------- respawn
local RESPAWN = "RespawnWnd"
local choices = { offset = 0 }
local respawnQuestionShown = false
ui.window("Respawn", function()
  if not W.shown(RESPAWN) then respawnQuestionShown = false; return end
  local question = W.shown("ConfirmationDialogBox")
  local returnFocus = respawnQuestionShown and not question
  respawnQuestionShown = question
  T.frame()
  if W.begin("Respawn", { w = 1200, h = 820, title = "Respawn", close = false, alwaysFocus = true, noFocus = question,
        hints = { { "a", "Select" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local ready = G.enabled(RESPAWN, "SelectButton")
    T.font(22)
    T.draw(x0, y0, "Choose an available destination", C.dim)
    T.unfont()
    local frac, text = eq.ChildGauge(RESPAWN, "TimeGauge")
    if frac == nil then frac, text = eq.Gauge(28) end
    T.bar(x0, y0 + T.px(40), x1, y0 + T.px(68), frac or 0, C.hp, text)
    local bh, rowH = T.px(72), 64
    local top = y0 + T.px(90)
    local visible = math.max(1, math.floor((y1 - bh - T.px(20) - top + T.px(10)) / (T.px(rowH) + T.px(10))))
    ui.SetCursorPos(x0 - wx, top - wy)
    G.list(RESPAWN, "OptionsList", choices, visible, rowH, w, function(i, t, rx0, ry0, rx1, ry1)
      local unavailable = t[1] == "unavailable"
      local c = (t.colors and t.colors[2]) or (unavailable and C.dim or (i == choices.selected and C.accent or C.text))
      local name = t[2] or ""
      T.font(23)
      T.textIn(rx0 + T.px(18), ry0, rx0 + (rx1 - rx0) * 0.6, ry1, name, c, 0)
      local nw = ui.CalcTextSize(name)
      T.unfont()
      -- A tag after the name: the server's availability flag is not an option kind.
      local tag = unavailable and "UNAVAILABLE" or (t[1] == "*" and "DEFAULT") or nil
      if tag then
        local tx = math.min(rx0 + T.px(34) + nw, rx0 + (rx1 - rx0) * 0.6 - T.px(10))
        T.font(16)
        local tw, th = ui.CalcTextSize(tag)
        local tc = unavailable and C.dim or C.accent
        local cy = (ry0 + ry1) / 2
        T.rect(tx, cy - th / 2 - T.px(4), tx + tw + T.px(16), cy + th / 2 + T.px(4), tc, 0.22, T.px(6))
        T.textIn(tx, ry0, tx + tw + T.px(16), ry1, tag, tc, 0.5)
        T.unfont()
      end
      T.font(20)
      T.textIn(rx0 + (rx1 - rx0) * 0.6, ry0, rx1 - T.px(18), ry1, t[3] or "", (t.colors and t.colors[3]) or C.dim, 1)
      T.unfont()
    end)
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    G.button(RESPAWN, "SelectButton", ready and "Respawn" or "Respawn (unavailable)", w, bh,
      { primary = true, always = true })
  end
  W.finish()
  -- The offer stays in front while the original choices and countdown remain drawn underneath it.
  if question then ui.SetWindowFocus("Confirm")
  elseif returnFocus then ui.SetWindowFocus("Respawn"); pad.focus("Respawn") end
end)
