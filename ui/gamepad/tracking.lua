-- Tracking (gamepad pack): stands in for TrackingWnd. The con-colour filters as a row of coloured toggles (lit:
-- that colour is listed), the names in the game's colours (focusing one selects it, A tracks it), and on the right
-- the options: the sort and the players to list (pickers), pets, mercenaries and automatic update; Track (X) and
-- Done. Every control is the stock window's child: the toggles and boxes its check buttons, the pickers its combo
-- boxes, a row its list's row (TRW_TrackingList).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/markets.lua")
local C = T.C

local TW = "TrackingWnd"
local F = M.form()
local list = {}

-- The con filters: child, colour, name (the stock check button on hides that colour).
local CONS = {
  { "TRW_FilterRedButton", { 0.94, 0.25, 0.25, 1 }, "Red" },
  { "TRW_FilterYellowButton", { 0.94, 0.86, 0.25, 1 }, "Yellow" },
  { "TRW_FilterWhiteButton", { 0.92, 0.92, 0.92, 1 }, "White" },
  { "TRW_FilterBlueButton", { 0.30, 0.45, 1.00, 1 }, "Blue" },
  { "TRW_FilterLightBlueButton", { 0.35, 0.85, 0.95, 1 }, "Light blue" },
  { "TRW_FilterGreenButton", { 0.35, 0.78, 0.40, 1 }, "Green" },
  { "TRW_FilterGray", { 0.55, 0.57, 0.62, 1 }, "Grey" },
}

ui.window("Tracking", function()
  if not M.visible(TW) then
    F.pick = nil
    return
  end
  M.frame()
  local n = API.windows.ListInfo(TW, "TRW_TrackingList") or 0
  if W.begin("Tracking", { w = 1400, h = 960, title = "Tracking", subtitle = n == 1 and "1 nearby" or (n .. " nearby"),
        hints = M.hints(F, { { "a", "Track" }, { "x", "Track" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.modal(F, x0, y0, x1, y1) then
      if M.pressed("Tracking", "x") then API.windows.ClickChild(TW, "TRW_TrackButton", "left") end
      local rx0 = x1 - T.px(440)
      -- the con filters
      M.caption(x0, y0, "Show")
      local gap = T.px(8)
      local cw = (rx0 - T.px(24) - x0 - gap * (#CONS - 1)) / #CONS
      local ch = T.px(64)
      ui.SetCursorPos(x0 - wx, y0 + T.px(28) - wy)
      for i, con in ipairs(CONS) do
        if i > 1 then ui.SameLine(0, gap) end
        local enabled, hidden = API.windows.ChildState(TW, con[1]) -- a ranger's filters: disabled for anyone else
        local on = not hidden
        local c = con[2]
        local clicked, focused, bx0, by0, bx1, by1 = T.button("##" .. con[1], "", cw, ch, {}, pad)
        local k = enabled == false and 0.4 or 1
        T.rect(bx0 + T.px(3), by0 + T.px(3), bx1 - T.px(3), by1 - T.px(3), c, (on and 0.30 or 0.05) * k, T.px(9))
        ui.DrawCircleFilled((bx0 + bx1) / 2, by0 + T.px(22), T.px(9), c[1], c[2], c[3], (on and 1 or 0.3) * k)
        T.font(15)
        T.textIn(bx0, by0 + T.px(36), bx1, by1 - T.px(4), on and con[3] or "hidden", on and C.text or C.faint, 0.5)
        T.unfont()
        if clicked and enabled ~= false then API.windows.ClickChild(TW, con[1], "left") end
      end
      -- the names
      local top = y0 + T.px(28) + ch + T.px(16)
      M.list(TW, "TRW_TrackingList", list, x0, top, rx0 - T.px(24), y1, {
        rowH = 56, select = "focus", inspect = false,
        empty = "Nothing to track nearby (or the filters hide it).",
        draw = function(r, a, b, c, d) M.row(r, a, b, c, d, r.texts[1], r.texts[2], { rightW = T.px(200) }) end,
        onA = function() API.windows.ClickChild(TW, "TRW_TrackButton", "left") end })
      -- the options and the buttons
      local bh = T.px(64)
      local oy = y0
      local function at() ui.SetCursorPos(rx0 - wx, oy - wy) oy = oy + bh + T.px(10) end
      at() M.combo(F, TW, "TRW_TrackSortCombobox", "Sort", x1 - rx0, bh)
      at() M.combo(F, TW, "TRW_TrackPlayersCombobox", "Players", x1 - rx0, bh)
      at() M.toggle(TW, "TRW_TrackPetsCheckbox", "Pets", x1 - rx0, bh, { on = "Listed", off = "Not listed" })
      at() M.toggle(TW, "TRW_TrackMercsCheckbox", "Mercenaries", x1 - rx0, bh, { on = "Listed", off = "Not listed" })
      at() M.toggle(TW, "TRW_AutoUpdateButton", "Update automatically", x1 - rx0, bh)
      ui.SetCursorPos(rx0 - wx, y1 - 2 * bh - T.px(10) - wy)
      M.buttons(TW, { { "TRW_TrackButton", "Track", { primary = true, glyph = "x" } } }, rx0, x1, bh)
      M.buttons(TW, { { "DoneButton", "Done" } }, rx0, x1, bh)
    end
  end
  if M.back(F, W.finish()) then API.windows.Show(TW, false) end
end)
