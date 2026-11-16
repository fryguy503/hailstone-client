-- Tracking: replaces the game's TrackingWnd. The con-colour filters as a row of coloured toggles, a search over the
-- names, the list in the game's colours, Track (or a double click), and the game's options (sort, players, pets,
-- mercenaries, automatic update) folded under the list. Every control is the original's child: the toggles and
-- boxes are its check buttons, the drop-downs its combo boxes, a row click selects in its list (TRW_TrackingList).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "TrackingWnd"

-- The con filters: child, the colour, what the colour means.
local CONS = {
  { "TRW_FilterRedButton", { 0.94, 0.25, 0.25, 1 }, "Red: much stronger than you" },
  { "TRW_FilterYellowButton", { 0.94, 0.86, 0.25, 1 }, "Yellow: stronger than you" },
  { "TRW_FilterWhiteButton", { 0.92, 0.92, 0.92, 1 }, "White: an even match" },
  { "TRW_FilterBlueButton", { 0.30, 0.45, 1.00, 1 }, "Blue: weaker than you" },
  { "TRW_FilterLightBlueButton", { 0.35, 0.85, 0.95, 1 }, "Light blue: much weaker" },
  { "TRW_FilterGreenButton", { 0.35, 0.78, 0.40, 1 }, "Green: far weaker" },
  { "TRW_FilterGray", { 0.55, 0.57, 0.62, 1 }, "Grey: no challenge" },
}

local search, options = "", false

-- One con toggle: a coloured dot, lit while that colour is listed (the original's check button is the filter: on
-- hides the colour; /trackfilter). Only a ranger filters: for anyone else the game disables the buttons.
local function conToggle(x, y, size, s, con)
  local enabled, hidden = W.State(WINDOW, con[1])
  local on = not hidden
  local c = con[2]
  local clicked = T.Hit("##" .. con[1], x, y, x + size, y + size)
  local hovered = ui.IsItemHovered()
  local k = enabled and 1 or 0.4
  ui.DrawRectFilled(x, y, x + size, y + size, c[1], c[2], c[3], (on and 0.28 or 0.06) * k, 6 * s)
  ui.DrawRect(x, y, x + size, y + size, c[1], c[2], c[3], (on and 0.9 or (hovered and 0.5 or 0.18)) * k, 6 * s, 1.5 * s)
  ui.DrawCircleFilled(x + size / 2, y + size / 2, size * 0.2, c[1], c[2], c[3], (on and 1 or 0.35) * k)
  if hovered then
    ui.SetTooltip(con[3] .. (not enabled and "\nOnly rangers filter by colour"
      or on and "\nListed: click to hide" or "\nHidden: click to list"))
  end
  if clicked and enabled then API.windows.ClickChild(WINDOW, con[1], "left") end
end

local OPTS = {
  size = function() return 300, 520 end,
  resizable = { 240, 320 },
}
local function body(x, y, w, h, s)
  -- con filters
  T.Text(x, y + 2 * s, "Show", C.muted, T.font.small)
  local n = #CONS
  local size = math.min(26 * s, (w - 46 * s - (n - 1) * 4 * s) / n)
  for i, con in ipairs(CONS) do conToggle(x + w - (n - i + 1) * (size + 4 * s) + 4 * s, y, size, s, con) end
  ui.SetCursorScreenPos(x, y + size + 8 * s)
  search = P.Search("##tracksearch", search, "Search names")
  local needle = search:lower()
  -- the list, the options under it, the buttons
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local optH = options and (4 * (lh + 10 * s) + 6 * s) or 0
  local foot = btnH + lh + 18 * s + optH
  local total, _, shown = P.Rows(WINDOW, "TRW_TrackingList", 0, -foot, {
    rowH = 22,
    filter = needle ~= "" and function(_, texts) return (texts[1] or ""):lower():find(needle, 1, true) ~= nil end or nil,
    draw = function(_, texts, x0, y0, x1, y1, s_, st)
      local _, th = T.TextSize("Ag", T.font.small)
      T.Text(x0 + 8 * s_, (y0 + y1 - th) / 2, T.Fit(texts[1] or "", x1 - x0 - 16 * s_, T.font.small), st.color,
        T.font.small)
    end,
    doubleClick = function() API.windows.ClickChild(WINDOW, "TRW_TrackButton", "left") end,
    empty = needle ~= "" and "No name matches" or "Nothing to track nearby",
    emptySub = needle == "" and "Turn on more colours above, or move somewhere busier" or nil,
  })
  local fx, fy = ui.GetCursorScreenPos()
  T.Text(fx, fy + 2 * s, shown == total and string.format("%d tracked", total)
    or string.format("%d of %d tracked", shown, total), C.faint, T.font.tiny)
  T.Text(x + w, fy + 2 * s, options and "Hide options" or "Options", C.accent, T.font.tiny, "right")
  local ow = T.TextSize("Hide options", T.font.tiny)
  if T.Hit("##trackopts", x + w - ow, fy, x + w, fy + lh) then options = not options end
  ui.SetCursorScreenPos(x, fy + lh + 6 * s)
  if options then
    local labelW = 60 * s
    local function combo(caption, id)
      local _, py = ui.GetCursorScreenPos()
      T.Text(x, py + 4 * s, caption, C.muted, T.font.small)
      ui.SetCursorScreenPos(x + labelW, py)
      W.Combo(WINDOW, id, { width = w - labelW })
    end
    combo("Sort", "TRW_TrackSortCombobox")
    combo("Players", "TRW_TrackPlayersCombobox")
    W.Check(WINDOW, "TRW_TrackPetsCheckbox", "Pets")
    ui.SameLine(0, 16 * s)
    W.Check(WINDOW, "TRW_TrackMercsCheckbox", "Mercenaries")
    W.Check(WINDOW, "TRW_AutoUpdateButton", "Update automatically", { tooltip = "Refresh the list every few seconds" })
  end
  ui.SetCursorScreenPos(x, y + h - btnH)
  W.ButtonRow(WINDOW, {
    { "TRW_TrackButton", "Track", { primary = true, tooltip = "Track the selected one" } },
    { "DoneButton", "Close" },
  }, btnH)
end

local function testOpen() API.windows.Show(WINDOW, true) end
ui.window("Tracking", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen) -- testing: test.open.TrackingWnd=1
  hud.Dialog(WINDOW, "Tracking", OPTS, body)
end)
