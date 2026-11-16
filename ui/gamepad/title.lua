-- Titles (gamepad pack): stands in for TitleWnd. Your name as it shows now and as it would with the picks, then the
-- titles or the suffixes the server offers (LB / RB), Use and Remove, and the game's "tell me about new titles" box.
-- The rows are the original's lists (Title_AvailableTitles / Title_AvailableSuffixes): A picks in the game's list,
-- X uses the pick (its Change button), Y removes the title or suffix (its Clear button); the buttons are its own.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local G = dofile("lib/stock.lua")
local C = T.C
local TW = "TitleWnd"

local KINDS = {
  { label = "Titles", list = "Title_AvailableTitles", change = "Title_ChangeTitlePrefix", clear = "Title_ClearTitlePrefix",
    noun = "title" },
  { label = "Suffixes", list = "Title_AvailableSuffixes", change = "Title_ChangeTitleSuffix",
    clear = "Title_ClearTitleSuffix", noun = "suffix" },
}
local kind = 1
local lists = { { offset = 0 }, { offset = 0 } }
local side = {}

ui.window("Titles", function()
  G.testShow()
  if not W.shown(TW) then return end
  T.frame()
  local k = KINDS[kind]
  if W.begin("Titles", { w = 1400, h = 960, title = "Titles",
        hints = { { "lb", "Titles" }, { "rb", "Suffixes" }, { "a", "Pick" }, { "x", "Use" }, { "y", "Remove" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    if W.pressed("lb") then kind = 1 end
    if W.pressed("rb") then kind = 2 end
    k = KINDS[kind]
    if W.pressed("x") and G.enabled(TW, k.change) then API.windows.ClickChild(TW, k.change, "left") end
    if W.pressed("y") and G.enabled(TW, k.clear) then API.windows.ClickChild(TW, k.clear, "left") end
    -- the name now and with the picks
    local cur, new = G.text(TW, "Title_CurrentName"), G.text(TW, "Title_ProposedName")
    local changed = new ~= "" and new ~= cur
    local boxH = T.px(110)
    T.rect(x0, y0, x1, y0 + boxH, C.sunken, 1, T.px(14))
    T.font(16)
    T.draw(x0 + T.px(20), y0 + T.px(20), "NOW", C.dim)
    T.draw(x0 + T.px(20), y0 + T.px(64), "NEW", C.dim)
    T.unfont()
    T.font(24)
    T.draw(x0 + T.px(100), y0 + T.px(14), T.fit(cur, w - T.px(120)), C.text)
    T.draw(x0 + T.px(100), y0 + T.px(58), T.fit(changed and new or "Pick a title or suffix below", w - T.px(120)),
      changed and C.accent or C.faint)
    T.unfont()
    -- the tabs
    local ty = y0 + boxH + T.px(16)
    local tx = x0
    T.font(20)
    for i, kk in ipairs(KINDS) do
      local label = string.format("%s (%d)", kk.label, API.windows.ListInfo(TW, kk.list) or 0)
      local tw = ui.CalcTextSize(label) + T.px(40)
      T.rect(tx, ty, tx + tw, ty + T.px(48), kind == i and C.accent or C.raised, kind == i and 0.9 or 1, T.px(10))
      T.textIn(tx, ty, tx + tw, ty + T.px(48), label, kind == i and C.onAccent or C.text, 0.5)
      if ui.IsMouseHoveringRect(tx, ty, tx + tw, ty + T.px(48)) and ui.IsMouseClicked(ui.Mouse.Left) then kind = i end
      tx = tx + tw + T.px(10)
    end
    T.unfont()
    k = KINDS[kind]
    -- the list, the buttons beside it
    local ly = ty + T.px(64)
    local bh, rowH = T.px(64), 56
    local listW = w * 0.62
    local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(rowH) + T.px(10))))
    ui.SetCursorPos(x0 - wx, ly - wy)
    local state = lists[kind]
    local n, row = G.list(TW, k.list, state, visible, rowH, listW, function(i, t, rx0, ry0, rx1, ry1)
      T.font(22)
      T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(18), ry1, T.fit(t[1] or "", rx1 - rx0 - T.px(36)),
        i == state.selected and C.accent or C.text, 0)
      T.unfont()
    end)
    if n == 0 then T.draw(x0, ly + T.px(10), "The server has given you no " .. k.label:lower() .. " yet", C.faint) end
    W.toSide(side, row)
    local rx = x0 + listW + T.px(24)
    local rw = x1 - rx
    local actions = {
      function() return G.button(TW, k.change, "Use " .. k.noun, rw, bh, { primary = true, glyph = "x", always = true }) end,
      function() return G.button(TW, k.clear, "Remove " .. k.noun, rw, bh, { glyph = "y", always = true }) end,
      function() return G.check(TW, "Title_NotifyNewTitles", "Tell me about new titles", rw, T.px(84)) end,
    }
    for i, a in ipairs(actions) do
      ui.SetCursorPos(rx - wx, ly + (i - 1) * (bh + T.px(10)) - wy)
      W.sideBefore(side, i)
      a()
      W.sideAfter(side, i, #actions, state)
    end
  end
  if W.finish() then API.windows.Show(TW, false) end
end)
