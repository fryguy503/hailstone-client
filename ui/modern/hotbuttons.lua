-- Action bars: replace the game's HotButtonWnd .. HotButtonWnd10. Twelve tiles each, with the page and its arrows at
-- the left. The buttons are the game's: click uses what the button holds (an empty one opens its menu), a quick
-- right click is the game's (a social's editor), the right button held opens the button's menu (assign a social,
-- label, icon, clear ...), dragging a button off takes it on the cursor, and letting go of a held button over a tile
-- puts it there. A right click on the bar opens the bar's own menu (hotbutton sets, clear, Button Size, Show Key Map,
-- Show Spinner, Open New Bar); its settings (hs.HotBarSetting) size the tiles, show each tile's key and the pager.
-- As the game's bar (CHotButtonWnd::OnCompleteMoveOrResize), a bar laid out taller than wide (more rows than
-- columns: "Buttons per row" in the frame's menu) is vertical, with its pager across the top instead of at the left;
-- the key map text sits in each tile's top-right corner, right-aligned (0x5506b0).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local BUTTONS = 12
local IDS, HIT_IDS = {}, {}
for i = 1, BUTTONS do
  IDS[i] = "HB_Button" .. i
  HIT_IDS[i] = "##HB_Button" .. i
end

-- A text tile's label wrapped to the tile's width, kept in `cache` (the tile's) while the label, the width and the
-- font's measure of the label stay the same: the lines and their count, and the line height.
local function wrapLabel(cache, label, maxw)
  local full = T.TextSize(label, T.font.tiny)
  if cache.label == label and cache.maxw == maxw and cache.full == full then return cache.lines, cache.n, cache.lh end
  local lines, n, line = cache.lines or {}, 0, ""
  for wd in label:gmatch("%S+") do
    local try = line == "" and wd or (line .. " " .. wd)
    if T.TextSize(try, T.font.tiny) > maxw and line ~= "" then
      n = n + 1
      lines[n] = line
      line = wd
    else
      line = try
    end
  end
  if line ~= "" then
    n = n + 1
    lines[n] = line
  end
  local _, lh = T.TextSize("Ag", T.font.tiny)
  cache.label, cache.maxw, cache.full, cache.lines, cache.n, cache.lh = label, maxw, full, lines, n, lh
  return lines, n, lh
end

local function bar(n)
  local window = n == 1 and "HotButtonWnd" or ("HotButtonWnd" .. n)
  local name = n == 1 and "Hotbuttons" or ("Hotbuttons " .. n)
  local perRowKey = "hud.perRow." .. window
  -- each tile's: the record hs.HotButtonInfo fills, its wrapped label, text colour and icon options, and its right
  -- button: as the game takes it, a quick click on release (a social's stop or editor; an empty tile's menu: assign
  -- a social, a new one opening the social editor), held 500 ms the button's menu
  local tiles = {}
  for i = 1, BUTTONS do
    local id = IDS[i]
    tiles[i] = {
      info = {}, wrap = {}, tc = { 0, 0, 0, 1 }, icon = { radius = 6 },
      onRight = function(phase, outside)
        if phase == "rightheld" then API.hotbars.Click(window, id, "rightheld")
        elseif phase == "rightup" and not outside then API.hotbars.Click(window, id, "right") end
      end,
    }
  end
  -- the bar's Button Size (PercentSize, 40..175: CHotButton::SetButtonSize) and Show Spinner (the pager), and the
  -- layout they make: read by opts.size, which hud.Window calls first once the bar shows
  local size, gap, spinner, perRow, rows, vertical, pager, left, top
  local function settings()
    local percent = API.hotbars.Setting and API.hotbars.Setting(n, "PercentSize") or 100
    spinner = not API.hotbars.Setting or API.hotbars.Setting(n, "ShowSpinner") ~= false
    size, gap = 44 * percent // 100, 4
    perRow = math.max(1, math.min(BUTTONS, tonumber(ui.GetSetting(perRowKey)) or BUTTONS))
    rows = math.ceil(BUTTONS / perRow)
    vertical = rows > perRow
    pager = spinner and 22 or -6
    left, top = vertical and -6 or pager, vertical and pager or -6 -- where the pager goes
  end
  -- A corner or edge drag changes the shape: the columns from the dragged width, or the rows from its height,
  -- whichever the mouse moved further. The same shape as "Buttons per row", by dragging the bar instead.
  local function resizeTo(nw, nh)
    settings()
    local curW = left + 6 + perRow * (size + gap) - gap + 14
    local curH = top + 6 + rows * (size + gap) - gap + 14
    local cols = perRow
    if math.abs(nw - curW) >= math.abs(nh - curH) then
      cols = math.floor((nw - left - 20 + gap) / (size + gap) + 0.5)
    else
      local newRows = math.max(1, math.floor((nh - top - 20 + gap) / (size + gap) + 0.5))
      cols = math.ceil(BUTTONS / newRows)
    end
    cols = math.max(1, math.min(BUTTONS, cols))
    if cols ~= perRow then ui.SetSetting(perRowKey, cols) end
  end
  local opts = {
    size = function()
      settings()
      return left + 6 + perRow * (size + gap) - gap + 14, top + 6 + rows * (size + gap) - gap + 14
    end,
    resizable = { 40, 40 }, -- (the drag's floor; the shape snaps to whole buttons)
    onResize = resizeTo,
    menu = function()
      ui.SetNextItemWidth(180)
      local v, changed = ui.SliderInt("Buttons per row", perRow, 1, BUTTONS)
      if changed then ui.SetSetting(perRowKey, v) end
    end,
  }
  -- A page arrow: its hit box x0, y0 .. x1, y1 and its triangle round mx, my (half-width d), pointing up or down.
  -- Made once for the bar: defined inside body, a new function was made each frame for every bar.
  local function arrow(id, up, x0, y0, x1, y1, mx, my, d)
    local clicked = T.Hit("##" .. id, x0, y0, x1, y1)
    local c = ui.IsItemHovered() and C.accent or C.muted
    if up then
      ui.DrawTriangleFilled(mx - d, my + d / 2, mx + d, my + d / 2, mx, my - d / 2, c[1], c[2], c[3], 1)
    else
      ui.DrawTriangleFilled(mx - d, my - d / 2, mx + d, my - d / 2, mx, my + d / 2, c[1], c[2], c[3], 1)
    end
    if clicked then API.windows.ClickChild(window, id, "left") end
  end
  local function body(x, y, w, h, s)
    -- the page: up, number, down (with Show Spinner); a vertical bar's runs across the top
    local px0, px1 = x + 7 * s, x + (7 + pager) * s
    if spinner and vertical then
      local cx, py0, py1 = x + w / 2, y + 5 * s, y + (5 + pager) * s
      local cy = (py0 + py1) / 2
      local page = API.windows.Text(window, "HB_VerticalCurrentPageLabel")
      if not page or page == "" then page = API.windows.Text(window, "HB_HorizontalCurrentPageLabel") or "" end
      local _, th = T.TextSize("8", T.font.small)
      T.Text(cx, cy - th / 2, page, C.text, T.font.small, "center")
      local up, down = cx - 20 * s, cx + 20 * s
      arrow("HB_PageUpButton", true, up - 9 * s, py0, up + 9 * s, py1, up, cy, 5 * s)
      arrow("HB_PageDownButton", false, down - 9 * s, py0, down + 9 * s, py1, down, cy, 5 * s)
      ui.DrawLine(x + 8 * s, py1 + 3 * s, x + w - 8 * s, py1 + 3 * s, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
    elseif spinner then
      local cy = y + h / 2
      local page = API.windows.Text(window, "HB_HorizontalCurrentPageLabel")
      if not page or page == "" then page = API.windows.Text(window, "HB_VerticalCurrentPageLabel") or "" end
      local _, th = T.TextSize("8", T.font.small)
      T.Text((px0 + px1) / 2, cy - th / 2, page, C.text, T.font.small, "center")
      local mx, up, down = (px0 + px1) / 2, cy - 17 * s, cy + 17 * s
      arrow("HB_PageUpButton", true, px0, up - 9 * s, px1, up + 9 * s, mx, up, 5 * s)
      arrow("HB_PageDownButton", false, px0, down - 9 * s, px1, down + 9 * s, mx, down, 5 * s)
      ui.DrawLine(px1 + 3 * s, y + 8 * s, px1 + 3 * s, y + h - 8 * s, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
    end

    local holding = API.cursor.HoldingButton()
    local bx0 = vertical and x + 7 * s or px1 + 6 * s
    local by0 = vertical and y + (7 + top + 6) * s or y + 7 * s
    local sz = size * s
    for i = 1, BUTTONS do
      local id, t = IDS[i], tiles[i]
      local col, row = (i - 1) % perRow, (i - 1) // perRow
      local bx = bx0 + col * (size + gap) * s
      local by = by0 + row * (size + gap) * s
      -- what it holds; a social's colour and a Lua social's face (social); its check (on: auto attack / fire, a
      -- skill not ready) and cool-down (state); a gem's stage (iconAlpha)
      local info = API.hotbars.Info(window, id, t.info)
      local kind, label, tex = info.kind, info.label, info.tex
      local face = info.social
      t.icon.wellAlpha = kind == "empty" and 0.5 or 1
      t.icon.alpha = info.iconAlpha or 1
      T.Icon(bx, by, sz, s, tex, info.u0, info.v0, info.u1, info.v1, nil, t.icon)
      if kind ~= "empty" and not tex and label ~= "" then
        -- a text button (social, combat ability): its label, wrapped to the tile
        ui.PushClipRect(bx, by, bx + sz, by + sz, true)
        ui.DrawRectFilled(bx + 1, by + 1, bx + sz - 1, by + sz - 1, C.accent[1], C.accent[2], C.accent[3], 0.10, 6 * s)
        local lines, count, lh = wrapLabel(t.wrap, label, sz - 4 * s)
        local ty = by + (sz - math.min(count, 3) * lh) / 2
        local tc = C.text
        if face then
          tc = t.tc
          tc[1], tc[2], tc[3] = info.color[1], info.color[2], info.color[3]
        end
        if face and info.faceCooldown > 0 then
          ui.DrawRectFilled(bx, by + sz * (1 - info.faceCooldown), bx + sz, by + sz, 0, 0, 0, 0.5, 6 * s)
        end
        if face and not info.enabled then ui.DrawRectFilled(bx, by, bx + sz, by + sz, 0, 0, 0, 0.45, 6 * s) end
        for k = 1, math.min(count, 3) do
          T.Text(bx + sz / 2, ty + (k - 1) * lh, lines[k], tc, T.font.tiny, "center")
        end
        if face and info.overlay ~= "" then
          T.Text(bx + sz - 3 * s, by + sz - 13 * s, info.overlay, C.text, T.font.tiny, "right")
        end
        if face and info.running then
          ui.DrawRect(bx + 1, by + 1, bx + sz - 1, by + sz - 1, C.accent[1], C.accent[2], C.accent[3], 0.9, 6 * s, 2 * s)
        end
        ui.PopClipRect()
      elseif tex and label ~= "" and kind == "item" then
        local count = label:match("^%d+$")
        if count then T.Text(bx + sz - 3 * s, by + sz - 13 * s, count, C.text, T.font.tiny, "right") end
      end
      if info.cooldown then
        -- the time still to go, shaded: it shrinks toward the bottom as the gem bar's does
        ui.PushClipRect(bx, by, bx + sz, by + sz, true)
        ui.DrawRectFilled(bx, by + sz * info.cooldown, bx + sz, by + sz, 0, 0, 0, 0.55, 6 * s)
        ui.DrawLine(bx, by + sz * info.cooldown, bx + sz, by + sz * info.cooldown, C.accent[1], C.accent[2], C.accent[3], 0.8, 1.5 * s)
        ui.PopClipRect()
        if info.left >= 1000 then
          local secs = (info.left + 999) // 1000
          local left = secs >= 60 and string.format("%dm", (secs + 59) // 60) or tostring(secs)
          T.Text(bx + sz / 2, by + sz / 2 - 7 * s, left, C.text, T.font.small, "center")
        end
      end
      if info.checked then
        ui.DrawRectFilled(bx, by, bx + sz, by + sz, C.accent[1], C.accent[2], C.accent[3], 0.16, 6 * s)
        ui.DrawRect(bx + 1, by + 1, bx + sz - 1, by + sz - 1, C.accent[1], C.accent[2], C.accent[3], 0.95, 6 * s, 2 * s)
      end
      -- the key bound to it (Show Key Map: hs.WindowText of the button is its key, "" with the key map off)
      local keyText = API.windows.Text(window, id) or ""
      if keyText ~= "" then T.Text(bx + sz - 4 * s, by + 2 * s, keyText, C.faint, T.font.tiny, "right") end
      local clicked = T.Hit(HIT_IDS[i], bx, by, bx + sz, by + sz)
      local key, hovered = ui.GetItemID(), ui.IsItemHovered()
      T.HoverFx(bx, by, bx + sz, by + sz, s, 6)
      if ui.IsItemHovered() and face and info.tooltip ~= "" then ui.SetTooltip(info.tooltip)
      elseif ui.IsItemHovered() and label ~= "" then ui.SetTooltip(label) end
      T.DragFrom(window, id)
      if clicked then
        API.hotbars.Click(window, id, "left")
      elseif holding and ui.IsMouseReleased(ui.Mouse.Left) and ui.IsMouseHoveringRect(bx, by, bx + sz, by + sz)
          and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows | ui.HoveredFlags.AllowWhenBlockedByActiveItem) then
        API.hotbars.Click(window, id, "left")
      end
      if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
        T.RightPressStart(key, t.onRight)
        T.TakeRightClick()
      end
      T.RightPressFrame(key, t.onRight, hovered)
    end
  end
  ui.window(name, function()
    if not API.session.InGame() then return end
    hud.Window(window, name, opts, body)
  end)
end

for n = 1, 10 do bar(n) end
