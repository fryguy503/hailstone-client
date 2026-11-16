-- Menus: the game's context menus (a chat window's, a hot button's, an item's, ...) and the EQ button's cascading
-- menu, drawn in this UI's look above every window. The rows and what a choice does are the game's, as in the
-- classic menus.lua: hs.GameMenus / hs.CascadeMenus give the open menus (text, check marks, disabled rows,
-- separators, submenus), a choice is a real click on that row of the game's menu (hs.GameMenuClick /
-- hs.CascadeClick), hovering a submenu row opens it, a click anywhere else closes them. What changes is the look:
-- rounded panels with a shadow, rows lit in the accent colour, chevrons for submenus; and the game menu opens from
-- the menu button wherever it was moved, with line icons of this UI's own.
local API = hs.UI
local style = dofile("lib/style.lua")
local T = dofile("lib/theme.lua")
local C = T.color

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

local PAD_X, PAD_Y = 6, 5 -- the menus' padding (GameMenus measures the rows with the style in force when it runs)

-- The context menus' zoom (menus.scale, 0.75..2; Options > Interface "Menu Size", or Ctrl+wheel over a menu): the
-- text and padding at that size, a little larger than the EQ font by default. GameMenus measures at the same scale.
local MENU_SCALE_MIN, MENU_SCALE_MAX, MENU_SCALE_DEFAULT = 0.75, 2.0, 1.25
local function menuScale()
  local v = tonumber(T.Setting("menus.scale"))
  if not v then return MENU_SCALE_DEFAULT end
  return math.max(MENU_SCALE_MIN, math.min(MENU_SCALE_MAX, v))
end

local function shadowPanel(x0, y0, x1, y1)
  ui.DrawRectFilled(x0 + 2, y0 + 4, x1 + 2, y1 + 6, 0, 0, 0, 0.30, 9)
end

-- A small chevron pointing right, centred at cx, cy.
local function chevron(cx, cy, d, c, a)
  ui.DrawLine(cx - d / 2, cy - d, cx + d / 2, cy, c[1], c[2], c[3], a, 1.5)
  ui.DrawLine(cx + d / 2, cy, cx - d / 2, cy + d, c[1], c[2], c[3], a, 1.5)
end

-- ---------------------------------------------------------------- context menus
local hovered = {}  -- menu number -> row whose hover was last sent to the game
local shownBefore = false

local function drawMenus(menus, s)
  local overAny = false
  local io = ui.GetIO()
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoMove | ui.WindowFlags.NoSavedSettings
  for m, menu in ipairs(menus) do
    ui.SetNextWindowPos(menu.x, menu.y, ui.Cond.Always)
    ui.SetNextWindowSize(math.max(menu.w, 40), math.max(menu.h, 10), ui.Cond.Always)
    ui.SetNextWindowFocus() -- above every other window, like the game's menus
    if ui.Begin("##gamemenu" .. m, nil, flags) then
      ui.PushFont(2)
      ui.SetWindowFontScale(s)
      for r, row in ipairs(menu.rows) do
        if row.separator then
          ui.Separator()
        else
          ui.PushID(r)
          local col = row.enabled and { row.r, row.g, row.b } or nil
          -- the game's white rows read as this UI's text colour; coloured ones keep their colour
          if col and col[1] > 0.95 and col[2] > 0.95 and col[3] > 0.95 then col = C.text end
          if col then col = readable(col, C.window, C.text) end
          if col then ui.PushStyleColor(ui.Col.Text, col[1], col[2], col[3], 1) end
          local clicked = ui.MenuItem(row.text, row.check, row.enabled, nil)
          if col then ui.PopStyleColor() end
          if row.submenu then
            local x0, y0, x1, y1 = ui.GetItemRect()
            local open = hovered[m] == r
            chevron(x1 - 6 * s, (y0 + y1) / 2, 3.5 * s, open and C.accent or C.muted, 1)
          end
          -- every row tells the game when the mouse gets to it (a submenu row opens its submenu; another row
          -- closes the submenus of this menu), as the stock list does
          if ui.IsItemHovered() and hovered[m] ~= r then
            hovered[m] = r
            API.menus.GameMenuHover(m, r - 1)
          end
          if clicked and row.enabled then API.menus.GameMenuClick(m, r - 1) end
          ui.PopID()
        end
      end
      ui.SetWindowFontScale(1)
      ui.PopFont()
      overAny = overAny or ui.IsWindowHovered()
      -- Ctrl+wheel over a menu zooms the menus, as it zooms a dialog (the next frame measures at the new size)
      if io.ctrl and (io.wheel or 0) ~= 0 and ui.IsWindowHovered() then
        local ns = math.max(MENU_SCALE_MIN, math.min(MENU_SCALE_MAX, s * 1.1 ^ io.wheel))
        ui.SetSetting("menus.scale", string.format("%.3f", ns))
      end
    end
    ui.End()
  end
  -- A click outside every menu closes them (not on the frame they appeared: that click opened them).
  if shownBefore and not overAny and (ui.IsMouseClicked(ui.Mouse.Left) or ui.IsMouseClicked(ui.Mouse.Right)) then
    API.menus.GameMenuClose()
  end
  shownBefore = true
end

ui.window("Game Menus", function()
  if not (API.session.UiReady or API.session.InGame)() then return end
  -- (the padding first: GameMenus measures the rows with it; nothing of style.Push changes what it measures)
  local s = menuScale()
  ui.PushStyleVar(ui.StyleVar.WindowPadding, PAD_X * s, PAD_Y * s)
  local got, menus = pcall(API.menus.Game, s)
  if not got then
    ui.PopStyleVar()
    error(menus, 0)
  end
  if #menus == 0 then -- none open: nothing to draw, the look not pushed
    ui.PopStyleVar()
    if shownBefore or next(hovered) then hovered, shownBefore = {}, false end
    return
  end
  style.Push()
  local ok, err = pcall(drawMenus, menus, s)
  style.Pop()
  ui.PopStyleVar()
  if not ok then error(err, 0) end
end)

-- ---------------------------------------------------------------- the game menu
-- The EQ button's cascade menu, laid out by this UI. The rows, their order, what a row does and which submenu it
-- opens are the game's (hs.CascadeMenus; hs.CascadeHover / hs.CascadeClick by row), but the panels are placed here:
-- the menu opens from the menu button (game_menu.lua keeps its rectangle in modern.menuAnchor) toward the middle of
-- the screen, each submenu beside the row that opened it, and every row has one of this UI's line icons
-- (lib/menuicons.lua) in place of the game's.
local icons = dofile("lib/menuicons.lua")
local cascadeHovered = {} -- menu number -> its row last hovered (the one whose submenu is open)
local cascadeShownBefore = false
local openedAt = nil

local function inRect(mx, my, r) return r and mx >= r[1] and mx < r[3] and my >= r[2] and my < r[4] end

ui.window("EQ Menu", function()
  if not (API.session.UiReady or API.session.InGame)() then return end
  local menus = API.menus.Cascades()
  if #menus == 0 then
    if cascadeShownBefore or openedAt or next(cascadeHovered) then
      cascadeHovered, cascadeShownBefore, openedAt = {}, false, nil
    end
    return
  end
  local now = ui.GetTime()
  openedAt = openedAt or now
  local fade = math.min(1, (now - openedAt) / 0.12)
  local io = ui.GetIO()
  local SW, SH = io.width, io.height
  local anchor = modern and modern.menuAnchor
  local s = anchor and anchor.s or 1
  local ROW, SEP, ICON, PADX, PADY, GAP = 30 * s, 9 * s, 18 * s, 8 * s, 6 * s, 8 * s
  -- the button's rectangle (or, with no button, the place the game chose) and the way the menus open from it
  local a = anchor and { anchor[1], anchor[2], anchor[3], anchor[4] }
      or { menus[1].x, menus[1].y, menus[1].x, menus[1].y }
  local dirX = (a[1] + a[3]) / 2 > SW / 2 and -1 or 1
  local dirY = (a[2] + a[4]) / 2 > SH / 2 and -1 or 1

  local mx, my = ui.GetMousePos()
  local overAny = inRect(mx, my, a)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoMove | ui.WindowFlags.NoSavedSettings
      | ui.WindowFlags.NoBackground
  local placed = {}
  style.Push()
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.PushFont(T.font.normal)
  for m, menu in ipairs(menus) do
    -- measure
    local textW = 0
    local h = 2 * PADY
    for _, item in ipairs(menu.items) do
      if item.kind == "separator" then h = h + SEP
      else
        textW = math.max(textW, (ui.CalcTextSize(item.text or "")))
        h = h + ROW
      end
    end
    local w = math.max(200 * s, PADX * 2 + 10 * s + ICON + 12 * s + textW + 26 * s)
    -- place: the top menu beside the button, a submenu beside its row, both kept on screen
    local x0, y0
    local parent = placed[m - 1]
    local row = parent and parent.rows[cascadeHovered[m - 1] or 0]
    if not parent then
      x0 = dirX < 0 and a[3] - w or a[1]
      y0 = dirY < 0 and a[2] - GAP - h or a[4] + GAP
    elseif row then
      x0 = dirX < 0 and parent.x0 - 3 * s - w or parent.x1 + 3 * s
      if x0 < 0 or x0 + w > SW then x0 = dirX < 0 and parent.x1 + 3 * s or parent.x0 - 3 * s - w end
      y0 = dirY < 0 and row[4] + PADY - h or row[2] - PADY
    else
      x0, y0 = menu.x, menu.y
    end
    x0 = math.max(4 * s, math.min(x0, SW - w - 4 * s))
    y0 = math.max(4 * s, math.min(y0, SH - h - 4 * s))
    y0 = y0 + (1 - fade) * 6 * s * -dirY -- a short slide as the menu appears
    local x1, y1 = x0 + w, y0 + h
    local here = { x0 = x0, y0 = y0, x1 = x1, y1 = y1, rows = {} }
    placed[m] = here

    ui.SetNextWindowPos(x0 - 6 * s, y0 - 6 * s, ui.Cond.Always)
    ui.SetNextWindowSize(w + 12 * s, h + 14 * s, ui.Cond.Always) -- (room for the shadow)
    ui.SetNextWindowFocus()
    if ui.Begin("##eqmenu" .. m, nil, flags) then
      local wx, wy = ui.GetWindowPos()
      ui.DrawRectFilled(x0 + 1 * s, y0 + 4 * s, x1 + 1 * s, y1 + 6 * s, 0, 0, 0, 0.35 * fade, 10 * s)
      ui.DrawRectFilled(x0, y0, x1, y1, C.popup[1], C.popup[2], C.popup[3], 0.98 * fade, 9 * s)
      ui.DrawRectFilled(x0 + 1, y0 + 1, x1 - 1, y0 + 18 * s, C.ink[1], C.ink[2], C.ink[3], 0.03 * fade, 9 * s)
      ui.DrawRect(x0, y0, x1, y1, C.ink[1], C.ink[2], C.ink[3], 0.10 * fade, 9 * s, 1)
      local y = y0 + PADY
      for i, item in ipairs(menu.items) do
        if item.kind == "separator" then
          local ly = math.floor(y + SEP / 2) + 0.5
          ui.DrawLine(x0 + PADX + 4 * s, ly, x1 - PADX - 4 * s, ly, C.ink[1], C.ink[2], C.ink[3], 0.08 * fade, 1)
          y = y + SEP
        else
          local rx0, ry0, rx1, ry1 = x0 + PADX / 2, y, x1 - PADX / 2, y + ROW
          here.rows[i] = { rx0, ry0, rx1, ry1 }
          ui.SetCursorPos(rx0 - wx, ry0 - wy)
          ui.PushID(i)
          ui.InvisibleButton("##item", rx1 - rx0, ry1 - ry0)
          local hov = ui.IsItemHovered()
          local clicked = ui.IsItemClicked(ui.Mouse.Left)
          ui.PopID()
          local enabled = item.enabled ~= false
          local open = item.kind == "submenu" and cascadeHovered[m] == i and menus[m + 1] ~= nil
          local lit = enabled and (hov or open)
          if lit then
            ui.DrawRectFilled(rx0, ry0 + 1, rx1, ry1 - 1, C.accent[1], C.accent[2], C.accent[3], (hov and 0.18 or 0.11) * fade, 6 * s)
            ui.DrawRectFilled(rx0, ry0 + 7 * s, rx0 + 3 * s, ry1 - 7 * s, C.accent[1], C.accent[2], C.accent[3], fade, 2 * s)
          end
          local ic = (not enabled and C.faint) or (lit and C.accent) or C.muted
          icons.Draw(item.text, rx0 + PADX / 2 + 6 * s + ICON / 2, (ry0 + ry1) / 2, ICON, ic, (enabled and 1 or 0.5) * fade)
          local c = (not enabled and C.faint) or (lit and C.text) or C.text
          local _, th = ui.CalcTextSize("Ag")
          T.Text(rx0 + PADX / 2 + 6 * s + ICON + 12 * s, ry0 + (ROW - th) / 2, item.text or "", c, T.font.normal, nil, fade)
          if item.kind == "submenu" then
            chevron(rx1 - 12 * s, (ry0 + ry1) / 2, 4 * s, lit and C.accent or C.faint, (enabled and 1 or 0.4) * fade)
          end
          -- hovering a row tells the game: a submenu row opens its submenu, another row closes this menu's
          if hov and enabled and cascadeHovered[m] ~= i then
            cascadeHovered[m] = i
            for k = m + 1, #menus do cascadeHovered[k] = nil end
            API.menus.CascadeHover(m, i)
          end
          if clicked and enabled then API.menus.CascadeClick(m, i) end
          y = y + ROW
        end
      end
      overAny = overAny or ui.IsWindowHovered()
    end
    ui.End()
  end
  ui.PopFont()
  ui.PopStyleVar()
  style.Pop()
  -- a click anywhere else, or Escape, closes the menus (the button's own click toggles them)
  if cascadeShownBefore and not overAny and (ui.IsMouseClicked(ui.Mouse.Left) or ui.IsMouseClicked(ui.Mouse.Right)) then
    API.menus.CascadeClose()
  elseif ui.IsKeyPressed(ui.Key.Escape) then
    API.menus.CascadeClose()
  end
  cascadeShownBefore = true
end)
