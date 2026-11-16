-- The extra hot bars (gamepad pack): stand in for HotButtonWnd2 .. HotButtonWnd10, whose keys (and a bar's Open New
-- Bar) show and hide them. The cross hotbar (crossbar.lua) plays bars 1..3 on the pad; a bar opened besides is a
-- row of its twelve buttons above the cross hotbar, the newest highest, with its page and arrows at the left. Like the
-- HUD, the bars never take the pad's focus and B leaves them up: they are the mouse's, and the
-- pad's in cursor mode (Back: the stick moves the pointer, A clicks, X right-clicks). The pad edits every bar's
-- buttons from the main menu's Hot Bars (slotpicker.lua), and any slot of them can go on the cross hotbar.
-- The buttons are the game's (hs.HotButtonInfo / hs.ClickHotButton, as the crossbar's): a click uses what the button
-- holds (an empty one opens its menu), a quick right click is the game's (a social's editor), the right button held
-- opens the button's menu, dragging one off takes it on the cursor and a hotbutton on the cursor is dropped onto the
-- button clicked. A right click on the page opens the bar's own menu (hs.HotBarMenu: sets, clear, Button Size,
-- Show Key Map, Show Spinner, Open New Bar); its Button Size and Show Spinner (hs.HotBarSetting) size the tiles and
-- show the pager. A left click on the page number turns the bar vertical (or back): as the game's bar laid out taller
-- than wide (CHotButtonWnd::OnCompleteMoveOrResize, rows > columns), a column of the twelve buttons with the page
-- across its top; vertical bars stand on the right, left of the minimap, the newest furthest left (the setting
-- hotbar.vertical.<window>).
local API = hs.UI
local T = dofile("lib/theme.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua") -- (testing: CH.testOpen)
local C = T.C
local BUTTONS = 12

local rightPress = nil -- the button the right button went down on: { key, at, held }
local order = {}       -- the bars shown, in the order they appeared (stacked upward)
local vorder = {}      -- the vertical bars shown, in the order they appeared (right to left)
local LISTS = { order, vorder }
local IDS = {}
for i = 1, BUTTONS do IDS[i] = "HB_Button" .. i end
-- the page's pieces: the arrows' children and the page number between them
local PAGE = { { "HB_PageUpButton", "up" }, { "", "page" }, { "HB_PageDownButton", "down" } }

-- Each tile's (n * 100 + i): the record hs.HotButtonInfo fills, its label's text colour and its fitted label.
local tiles = {}

-- T.fit(label, w) in the current font, kept in `c` while the label, the width and the font's measure of the label
-- stay the same.
local function fitted(c, label, w)
  local full = ui.CalcTextSize(label)
  if c.label ~= label or c.w ~= w or c.full ~= full then
    c.label, c.w, c.full, c.text = label, w, full, T.fit(label, w)
  end
  return c.text
end

local function tile(window, n, i, x0, y0, size)
  local id = IDS[i]
  local k = n * 100 + i
  local rec = tiles[k]
  if not rec then
    rec = { info = {}, tc = { 0, 0, 0, 1 }, fit = {} }
    tiles[k] = rec
  end
  -- what it holds; a social's colour and a Lua social's face (social); its check and cool-down, a gem's stage
  local info = API.hotbars.Info(window, id, rec.info)
  local kind, label, tex = info.kind, info.label, info.tex
  local face = info.social
  local x1, y1 = x0 + size, y0 + size
  local r = T.px(8)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y0 - wy)
  ui.PushID(k)
  local clicked = ui.InvisibleButton("##hb", size, size)
  local hovered, active = ui.IsItemHovered(), ui.IsItemActive()
  ui.PopID()
  T.rect(x0, y0, x1, y1, kind == "empty" and C.sunken or C.raised, 0.92, r)
  local iconA = info.iconAlpha or 1
  if tex then
    local m = math.floor(size * 0.08)
    T.icon(tex, info.u0, info.v0, info.u1, info.v1, x0 + m, y0 + m, x1 - m, y1 - m, iconA)
    if kind == "item" and label:match("^%d+$") then
      T.font(13)
      T.textIn(x0, y1 - T.px(18), x1 - T.px(4), y1, label, C.text, 1)
      T.unfont()
    end
  elseif label ~= "" then
    if face and info.faceCooldown > 0 then ui.DrawRectFilled(x0, y1 - size * info.faceCooldown, x1, y1, 0, 0, 0, 0.5, r) end
    if face and not info.enabled then ui.DrawRectFilled(x0, y0, x1, y1, 0, 0, 0, 0.45, r) end
    T.font(13)
    local tc = C.text
    if face then
      tc = rec.tc
      tc[1], tc[2], tc[3] = info.color[1], info.color[2], info.color[3]
    end
    T.textIn(x0 + 2, y0, x1 - 2, y1, fitted(rec.fit, label, size - 4), tc, 0.5)
    if face and info.overlay ~= "" then T.textIn(x0 + 2, y1 - size * 0.4, x1 - 3, y1, info.overlay, C.text, 1) end
    T.unfont()
  end
  if face and info.running then T.outline(x0, y0, x1, y1, C.accent, 1, r, T.px(2)) end
  if info.cooldown then
    local top = y0 + size * info.cooldown
    ui.DrawRectFilled(x0, top, x1, y1, 0, 0, 0, 0.55, r)
    ui.DrawLine(x0, top, x1, top, C.accent[1], C.accent[2], C.accent[3], 0.8, T.px(2))
    if info.left >= 1000 then
      local secs = (info.left + 999) // 1000
      T.font(15)
      T.textIn(x0, y0, x1, y1, secs >= 60 and string.format("%dm", (secs + 59) // 60) or tostring(secs), C.text, 0.5)
      T.unfont()
    end
  end
  if info.checked then
    ui.DrawRectFilled(x0, y0, x1, y1, C.accent[1], C.accent[2], C.accent[3], 0.18, r)
    T.outline(x0, y0, x1, y1, C.accent, 1, r, T.px(3))
  end
  T.outline(x0, y0, x1, y1, hovered and C.accent or C.edge, hovered and 0.9 or 1, r, hovered and T.px(2) or 1)
  -- the key bound to it (Show Key Map: the button's text is its key, "" with the key map off)
  local key = API.windows.Text(window, id) or ""
  if key ~= "" then
    T.font(12)
    T.draw(x1 - T.px(4) - ui.CalcTextSize(key), y0 + T.px(2), key, C.faint) -- top right, as the game's (0x5506b0)
    T.unfont()
  end
  -- dragging it off takes it on the cursor
  if active and ui.IsMouseDragging(ui.Mouse.Left) and not API.cursor.HoldingButton() then API.cursor.PickUp(window, id) end
  if clicked then
    API.hotbars.Click(window, id, "left")
  elseif API.cursor.HoldingButton() and hovered and ui.IsMouseReleased(ui.Mouse.Left) then
    API.hotbars.Click(window, id, "left") -- a dragged hotbutton let go over it
  end
  -- the right button as the game takes it: a quick click on release, held 500 ms the button's menu
  if hovered and ui.IsMouseClicked(ui.Mouse.Right) then rightPress = { key = k, at = ui.GetTime() } end
  if rightPress and rightPress.key == k then
    if not ui.IsMouseDown(ui.Mouse.Right) then
      if not rightPress.held and hovered then API.hotbars.Click(window, id, "right") end
      rightPress = nil
    elseif not rightPress.held and ui.GetTime() - rightPress.at > 0.5 then
      rightPress.held = true
      API.hotbars.Click(window, id, "rightheld")
    end
  end
  if hovered and face and info.tooltip ~= "" then ui.SetTooltip(info.tooltip)
  elseif hovered and label ~= "" then ui.SetTooltip(label) end
end

local function bar(n)
  local window = "HotButtonWnd" .. n
  local name = "Hot Bar " .. n
  local key = "hotbar.vertical." .. window
  ui.window(name, function()
    T.frame()
    CH.testOpen(window)
    local shown = W.shown(window)
    local vertical = shown and ui.GetSetting(key, "0") == "1"
    -- (a bar's place in the stack it is in: shown and of that orientation)
    for l = 1, 2 do
      local list = LISTS[l]
      local keep = shown and ((list == vorder) == vertical)
      for k, b in ipairs(list) do if b == n and not keep then table.remove(list, k) break end end
    end
    if not shown then return end
    local list = vertical and vorder or order
    local place
    for k, b in ipairs(list) do if b == n then place = k end end
    if not place then
      list[#list + 1] = n
      place = #list
    end
    T.resetFonts()
    local percent = API.hotbars.Setting and API.hotbars.Setting(n, "PercentSize") or 100
    local spinner = not API.hotbars.Setting or API.hotbars.Setting(n, "ShowSpinner") ~= false
    local size, gap = T.px(52 * percent / 100), T.px(5)
    local pager = spinner and T.px(40) or 0
    local p = T.px(8)
    -- the cross hotbar's top (GPSTATE.crossBar)
    local top = GPSTATE.crossBar and ui.GetTime() - GPSTATE.crossBar.at < 1 and GPSTATE.crossBar.y0 or T.H - T.px(270)
    local w, h, x0, y0
    if vertical then
      -- a column left of the minimap's (300 wide at the right edge), between the top and the cross hotbar, the
      -- buttons made smaller when the screen is too short for them
      local yTop, yBot = T.px(100), top - T.px(12)
      size = math.max(T.px(20), math.min(size, (yBot - yTop - p * 2 - pager - (BUTTONS - 1) * gap) / BUTTONS))
      w = p * 2 + size
      h = p * 2 + pager + BUTTONS * size + (BUTTONS - 1) * gap
      x0 = T.W - T.px(24 + 300 + 16) - place * w - (place - 1) * T.px(8)
      y0 = yTop + math.max(0, (yBot - yTop - h) / 2)
    else
      w = p * 2 + pager + BUTTONS * size + (BUTTONS - 1) * gap
      h = p * 2 + size
      -- above the cross hotbar, the first bar opened lowest
      x0, y0 = (T.W - w) / 2, top - T.px(12) - place * (h + T.px(8))
    end
    ui.SetNextWindowPos(x0, y0, ui.Cond.Always)
    ui.SetNextWindowSize(w, h, ui.Cond.Always)
    ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
    ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
    local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoBackground | ui.WindowFlags.NoSavedSettings
        | ui.WindowFlags.NoMove | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoBringToFrontOnFocus | W.NoNav
    if ui.Begin(name, nil, flags) then
      T.font(T.BASE)
      T.panel(x0, y0, x0 + w, y0 + h, 0.85, T.px(12))
      local bx, by = x0 + p, y0 + p
      if spinner then
        -- the page between its arrows (across the top of a vertical bar); a right click on it: the bar's menu, a
        -- left click on the number: horizontal / vertical
        local page = API.windows.Text(window, "HB_HorizontalCurrentPageLabel")
        if not page or page == "" then page = API.windows.Text(window, "HB_VerticalCurrentPageLabel") or "" end
        local wx, wy = ui.GetWindowPos()
        local third = size / 3
        for k, a in ipairs(PAGE) do
          local cx0, cy0, cw, chh
          if vertical then cx0, cy0, cw, chh = bx + (k - 1) * third, by, third, pager - T.px(6)
          else cx0, cy0, cw, chh = bx, by + (k - 1) * third, pager - T.px(6), third end
          ui.SetCursorPos(cx0 - wx, cy0 - wy)
          ui.PushID(a[2])
          local clicked = ui.InvisibleButton("##pg", cw, chh)
          local hov = ui.IsItemHovered()
          ui.PopID()
          if a[1] ~= "" then
            T.triangle(a[2], cx0 + cw / 2 - (vertical and 0 or T.px(3)), cy0 + chh / 2, T.px(6), hov and C.accent or C.dim, 1)
            if clicked then API.windows.ClickChild(window, a[1], "left") end
          else
            T.font(16)
            T.textIn(cx0, cy0, cx0 + cw, cy0 + chh, page, hov and C.accent or C.text, 0.5)
            T.unfont()
            if clicked then ui.SetSetting(key, vertical and "0" or "1") end
            if hov then ui.SetTooltip(vertical and "Lay the bar out across" or "Lay the bar out downward") end
          end
          if hov and ui.IsMouseClicked(ui.Mouse.Right) and API.hotbars.Menu then
            API.hotbars.Menu(n, cx0 + cw / 2, cy0 + chh / 2)
          end
        end
        if vertical then by = by + pager else bx = bx + pager end
      end
      for i = 1, BUTTONS do
        if vertical then tile(window, n, i, bx, by + (i - 1) * (size + gap), size)
        else tile(window, n, i, bx + (i - 1) * (size + gap), by, size) end
      end
      T.unfont()
    end
    ui.End()
    ui.PopStyleVar(2)
  end)
end

for n = 2, 10 do bar(n) end
