-- Menus (gamepad pack):
--   Game menus     the game's context menus (a hot button's, an item's, ...), drawn large where the game opened
--                  them; A chooses a row (a real click on that row of the game's menu), a submenu row opens its
--                  submenu, B or a click elsewhere closes them (eq.GameMenus)
--   All Windows    the EQ button's menu as a focus list (eq.CascadeMenus): every window the game's menu reaches,
--                  whether this pack has it or not, each with its line icon (lib/menuicons.lua, the modern pack's);
--                  A opens a submenu or runs the entry, B goes back
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local icons = dofile("lib/menuicons.lua")
local C = T.C

-- A game row's colour made readable on this UI's panel: one too close to the panel's brightness (the spell menu's
-- black and dark green target-type colours on a dark panel, its yellow on a light one) is blended toward the text
-- colour until it stands out. A look change, not the game's: its menus are on its own art.
local function lum(c) return 0.2126 * c[1] + 0.7152 * c[2] + 0.0722 * c[3] end
local function readable(c, panel, text)
  local lp, lc, lt = lum(panel), lum(c), lum(text)
  local need = 0.2
  if math.abs(lc - lp) >= need or math.abs(lt - lc) < 1e-3 then return c end
  local goal = lt > lp and lp + need or lp - need
  local t = math.min(1, math.max(0, (goal - lc) / (lt - lc)))
  return { c[1] + (text[1] - c[1]) * t, c[2] + (text[2] - c[2]) * t, c[3] + (text[3] - c[3]) * t }
end
icons.PushFont = function() T.font(15) end
icons.PopFont = T.unfont

local function menuStyle(begin)
  if begin then
    ui.PushStyleVar(ui.StyleVar.WindowPadding, T.px(10), T.px(10))
    ui.PushStyleVar(ui.StyleVar.WindowRounding, T.px(14))
    ui.PushStyleVar(ui.StyleVar.ItemSpacing, T.px(4), T.px(4))
    ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
    ui.PushStyleColor(ui.Col.WindowBg, T.C.window[1], T.C.window[2], T.C.window[3], 0.97)
  else
    ui.PopStyleColor()
    ui.PopStyleVar(4)
  end
end

-- ---------------------------------------------------------------- the game's context menus
local hovered = {}
local shownBefore = false
local lastCount = 0

ui.window("Game Menus", function()
  if not (eq.UiReady or eq.InGame)() then return end
  local menus = eq.GameMenus()
  if #menus == 0 then
    if shownBefore or lastCount ~= 0 or next(hovered) then hovered, shownBefore, lastCount = {}, false, 0 end
    return
  end
  T.frame()
  T.resetFonts()
  local overAny = false
  local rowH = T.px(52)
  for m, menu in ipairs(menus) do
    -- the rows' width: the widest text at this size
    T.frame()
    local w = T.px(320)
    local count = 0
    for _, row in ipairs(menu.rows) do count = count + (row.separator and 0 or 1) end
    ui.SetNextWindowPos(math.min(menu.x, T.W - w - T.px(10)), math.min(menu.y, T.H - count * (rowH + T.px(4)) - T.px(30)), ui.Cond.Always)
    ui.SetNextWindowSize(w, 0, ui.Cond.Always)
    local newest = m == #menus
    if newest and #menus ~= lastCount then ui.SetNextWindowFocus() end -- the newest menu has the focus (and the pad)
    menuStyle(true)
    local name = "##gamemenu" .. m
    if ui.Begin(name, nil, ui.WindowFlags.NoDecoration | ui.WindowFlags.NoMove | ui.WindowFlags.NoSavedSettings
          | ui.WindowFlags.AlwaysAutoResize) then
      if newest and #menus ~= lastCount then pad.focus(name) end
      T.font(20)
      for r, row in ipairs(menu.rows) do
        if row.separator then
          local x0, y0 = ui.GetCursorScreenPos()
          ui.DrawLine(x0, y0 + T.px(3), x0 + w - T.px(20), y0 + T.px(3), C.ink[1], C.ink[2], C.ink[3], 0.1, 1)
          ui.Dummy(w - T.px(20), T.px(6))
        else
          ui.PushID(r)
          local label = (row.check and "* " or "") .. row.text .. (row.submenu and "   >" or "")
          local clicked, focused = T.button("##row", "", w - T.px(20), rowH, { disabled = not row.enabled }, pad)
          local x0, y0 = ui.GetItemRect()
          T.textIn(x0 + T.px(14), y0, x0 + w - T.px(34), y0 + rowH, label,
            row.enabled and readable({ row.r or 1, row.g or 1, row.b or 1, 1 }, C.window, C.text) or C.faint, 0)
          if focused and row.submenu and hovered[m] ~= r then
            hovered[m] = r
            eq.GameMenuHover(m, r - 1)
          end
          if clicked and row.enabled then eq.GameMenuClick(m, r - 1) end
          ui.PopID()
        end
      end
      T.unfont()
      overAny = overAny or ui.IsWindowHovered(ui.HoveredFlags.AllowWhenBlockedByActiveItem)
      if newest and W.pressed("b") then eq.GameMenuClose() end
    end
    ui.End()
    menuStyle(false)
  end
  lastCount = #menus
  if shownBefore and not overAny and (ui.IsMouseClicked(ui.Mouse.Left) or ui.IsMouseClicked(ui.Mouse.Right)) then
    eq.GameMenuClose()
  end
  shownBefore = true
end)

-- ---------------------------------------------------------------- the EQ menu as a list
local path = {} -- the submenu rows opened, from the top menu
local lists = {} -- each level's list state

-- Makes the game's open menus match `path` (opening the EQ menu first).
local function sync()
  local menus = eq.CascadeMenus()
  if #menus == #path + 1 then return menus end
  if #menus > 0 then eq.CascadeClose() end
  eq.ClickChild("EQMainWnd", "EQButton", "left")
  for level, i in ipairs(path) do eq.CascadeHover(level, i) end
  return eq.CascadeMenus()
end

ui.window("All Windows", function()
  if not eq.InGame() then return end
  if not GPSTATE.eqMenuOpen then
    if #path > 0 or GPSTATE.eqMenuWasOpen then
      path = {}
      GPSTATE.eqMenuWasOpen = false
      if #eq.CascadeMenus() > 0 then eq.CascadeClose() end
    end
    return
  end
  GPSTATE.eqMenuWasOpen = true
  local menus = sync()
  local level = #path + 1
  local menu = menus[level]
  if not menu then
    GPSTATE.eqMenuOpen = false
    return
  end
  local back = false
  if W.begin("All Windows", { w = 760, h = 900, title = "All Windows", subtitle = #path > 0 and "Submenu" or "The EQ menu",
        hints = { { "a", "Open" }, { "b", #path > 0 and "Back" or "Close" } } }) then
    local _, _, _, _, cw, ch = W.area()
    local items = {}
    for i, item in ipairs(menu.items) do
      if item.kind ~= "separator" then items[#items + 1] = { i = i, item = item } end
    end
    lists[level] = lists[level] or { offset = 0 }
    local visible = math.max(1, math.floor((ch + T.px(10)) / T.px(64)))
    W.list("eq" .. level, lists[level], #items, visible, 54, cw, function(n, x0, y0, x1, y1, focused)
      local item = items[n].item
      local enabled = item.enabled ~= false
      local d = T.px(28)
      icons.Draw(item.text, x0 + T.px(18) + d / 2, (y0 + y1) / 2, d,
        (not enabled and C.faint) or (focused and C.accent) or C.dim, enabled and 1 or 0.5)
      T.textIn(x0 + T.px(30) + d, y0, x1 - T.px(18), y1, item.text, enabled and C.text or C.faint, 0)
      if item.kind == "submenu" then T.triangle("right", x1 - T.px(28), (y0 + y1) / 2, T.px(8), C.dim, 1) end
    end, function(n)
      local e = items[n]
      if e.item.enabled == false then return end
      if e.item.kind == "submenu" then
        path[#path + 1] = e.i
        lists[level + 1] = { offset = 0, focusNext = 1 } -- (the pad's focus on its first row: the old rows are gone)
        lists[level].backTo = n - lists[level].offset
      else
        eq.CascadeClick(level, e.i)
        path = {}
        GPSTATE.eqMenuOpen = false
        -- a window this pack does not have opens as it is: chosen with the pad, the pad's pointer comes up to use
        -- it (a window of the pack takes the pad's focus itself)
        if pad.navVisible() then pad.set_mode("cursor") end
      end
    end)
    if W.pressed("b") and #path > 0 then back = true end
  end
  local closed = W.finish()
  if back then
    table.remove(path)
    local up = lists[#path + 1]
    if up then up.focusNext = up.backTo or 1 end -- (back on the submenu's row)
  elseif closed then
    GPSTATE.eqMenuOpen = false
  end
end)
