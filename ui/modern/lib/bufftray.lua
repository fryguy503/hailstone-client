-- Buffs and Songs share the same slot order, spell actions, icon rings and countdowns. The default is the
-- frameless, right-aligned icon tray. Each character can change the two trays independently.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local buffmenu = dofile("lib/buffmenu.lua")
local T = hud.T
local C = T.color

local tray = {}
local MENU = { block = "buff", remove = true, windowOptions = true }
local GAP, LABEL, TITLE = 4, 12, 24
local NAME_FONT = T.font.small

local function toggle(key, label, invert)
  local enabled = (ui.GetSetting(key) == "1") ~= (invert == true)
  if ui.MenuItem(label, enabled) then ui.SetSetting(key, enabled == (invert == true)) end
end

local function fitNames(b, width, scale, multiline)
  local text = b.name
  if b.fitSource ~= text or b.fitWidth ~= width or b.fitScale ~= scale or b.fitMultiline ~= multiline then
    local first, second = text, nil
    if multiline and T.TextSize(text, NAME_FONT) > width then
      local split
      for space in text:gmatch("() +") do
        if T.TextSize(text:sub(1, space - 1), NAME_FONT) > width then break end
        split = space
      end
      if split then
        first = text:sub(1, split - 1)
        second = T.Fit(text:sub(split + 1):match("^%s*(.*)"), width, NAME_FONT)
      end
    end
    b.fitFirst, b.fitSecond = T.Fit(first, width, NAME_FONT), second
    b.fitSource, b.fitWidth, b.fitScale, b.fitMultiline = text, width, scale, multiline
  end
  return b.fitFirst, b.fitSecond
end

-- tray.Register(scriptName, window, slots, opts)  opts: icon, perRow (at most: the tray wraps to the room the default
-- layout gives it at the top right, lib/layout.lua trayRoom, unless the player set Icons per row)
function tray.Register(name, window, slots, opts)
  local ids, hits, badges = {}, {}, {}
  for i = 0, slots - 1 do
    ids[i + 1] = "Buff" .. i
    hits[i + 1] = "##Buff" .. i
    badges[i + 1] = "#" .. (i + 1)
  end
  local iconKey, perRowKey = "hud.icon." .. window, "hud.perRow." .. window
  local emptySlotsKey, slotNumbersKey = "hud.showEmptySlots." .. window, "hud.showSlotNumbers." .. window
  local layoutKey, namesKey, badgesKey = "hud.buffLayout." .. window, "hud.buffNames." .. window, "hud.buffSlotBadges." .. window
  local timeKey, backgroundKey, titleKey = "hud.buffHideTime." .. window, "hud.buffBackground." .. window, "hud.buffTitleBar." .. window
  local resizeKey, sizeKey = "hud.buffResize." .. window, "hud.size." .. window
  local winOpts = { frameMenu = true }
  local resizeMinimum = { 12, 10 } -- the layout below clamps to whole, visible tiles
  local list, n, frame = {}, 0, -1
  local icon, perRow, columns, rows, cellWidth, cellHeight, width, height, cells
  local hasNames, hasBadges, hasTime, vertical, titleHeight, badgeGutter, showEmptySlots, showSlotNumbers
  local leftPad, bottomPad, savedWidth, savedHeight

  local function saveSize(w, h)
    ui.SetSetting(sizeKey, (w > 0 or h > 0) and string.format("%d,%d", w, h) or "")
  end

  local function measure(scale)
    local io = ui.GetIO()
    vertical = ui.GetSetting(layoutKey) == "vertical"
    hasNames, hasBadges = ui.GetSetting(namesKey) == "1", ui.GetSetting(badgesKey) == "1"
    hasTime = ui.GetSetting(timeKey) ~= "1"
    winOpts.panel = ui.GetSetting(backgroundKey) == "1"
    titleHeight = ui.GetSetting(titleKey) == "1" and TITLE or 0
    local resize = ui.GetSetting(resizeKey) == "1"
    winOpts.resizable = resize and resizeMinimum or nil
    winOpts.resizeEdge = vertical and not hasNames and "bottom" or "left"
    local sw, sh = (ui.GetSetting(sizeKey) or ""):match("^(%d+),(%d+)$")
    savedWidth, savedHeight = math.tointeger(tonumber(sw)) or 0, math.tointeger(tonumber(sh)) or 0
    leftPad, bottomPad = 6, 5
    if winOpts.resizeEdge == "left" and (resize or savedWidth > 0) then leftPad = math.max(leftPad, 5 / scale) end
    if winOpts.resizeEdge == "bottom" and (resize or savedHeight > 0) then bottomPad = math.max(bottomPad, 5 / scale) end
    local padX, padY = leftPad + 6, 5 + bottomPad + titleHeight
    local titleWidth = titleHeight > 0 and 74 or 0
    -- At small icon sizes the badge hangs farther to the left so the counter and charge overlays keep their space.
    badgeGutter = hasBadges and not (hasNames and vertical) and math.max(5, 49 - icon) or 0
    if hasNames then
      cellWidth = vertical and math.max(150, icon + (hasBadges and 122 or 100))
        or math.max(170, icon + badgeGutter + 112)
      cellHeight = math.max(icon, vertical and 16 or (hasTime and 38 or 26)) + 8
    else
      cellWidth, cellHeight = icon + badgeGutter, icon + (hasTime and LABEL or 0)
    end

    local minWidth = math.max(titleWidth, cellWidth + padX)
    local maxWidth = math.max(minWidth, math.floor(io.width / scale))
    local maxHeight = math.max(cellHeight + padY, io.height / scale)
    local maxColumns = math.max(1, math.floor((maxWidth - padX + GAP) / (cellWidth + GAP)))
    if vertical then
      local limit = savedHeight > 0 and not hasNames and math.min(savedHeight, maxHeight) or maxHeight
      local fits = math.floor((limit - padY + GAP) / (cellHeight + GAP))
      rows = math.max(1, math.ceil(cells / maxColumns), math.min(cells, fits))
      columns = math.max(1, math.ceil(cells / rows))
      width = math.max(titleWidth, columns * (cellWidth + GAP) - GAP + padX)
      if hasNames then
        local preferred = savedWidth > 0 and savedWidth or columns * (math.max(268, cellWidth) + GAP) - GAP + padX
        width = math.max(width, math.min(preferred, maxWidth))
        cellWidth = (width - padX - (columns - 1) * GAP) / columns
      end
    else
      local room = hud.Layout().trayRoom
      local fits = room and math.floor((room / scale - padX + GAP) / (cellWidth + GAP)) or opts.perRow
      perRow = math.max(1, math.floor(tonumber(ui.GetSetting(perRowKey)) or math.min(opts.perRow, fits)))
      if hasNames or hasBadges or savedWidth > 0 then
        local maxRows = math.max(1, math.floor((maxHeight - padY + GAP) / (cellHeight + GAP)))
        local minColumns = math.min(maxColumns, math.max(1, math.ceil(cells / maxRows)))
        if savedWidth > 0 then
          width = math.max(minWidth, minColumns * (cellWidth + GAP) - GAP + padX, math.min(savedWidth, maxWidth))
          perRow = math.floor((width - padX + GAP) / (cellWidth + GAP))
        else
          perRow = math.max(minColumns, math.min(perRow, maxColumns))
        end
      end
      columns = math.max(1, math.min(perRow, cells))
      rows = math.max(1, math.ceil(cells / perRow))
      if savedWidth == 0 then width = math.max(titleWidth, columns * (cellWidth + GAP) - GAP + padX) end
    end
    height = rows * (cellHeight + GAP) - GAP + padY
  end

  -- Read the settings and occupied slots once per frame, before hud.Window places the frame.
  local function collect()
    local f = ui.GetFrameCount()
    if f == frame then return end
    frame = f
    n = 0
    local lastSlot = 0
    for i = 1, slots do
      -- the spell and the counters the game puts on the button (hits left, counters): hs.BuffSlot's overlays
      local spell, ms, spellName, good, fade, counter, cr, cg, cb, limit = API.buffs.Slot(window, ids[i], true)
      if spell then
        n = n + 1
        lastSlot = i
        local b = list[n]
        if not b then
          b = { counterColor = { 0, 0, 0, 1 }, icon = {} }
          list[n] = b
        end
        b.id, b.hit, b.spell, b.ms, b.name, b.good, b.fade = ids[i], hits[i], spell, ms, spellName or "", good, fade or 1
        b.slot = i
        b.counter, b.limit = counter, limit
        if counter then b.counterColor[1], b.counterColor[2], b.counterColor[3] = cr, cg, cb end
        -- the time left under it (T.Duration), formatted again only when what it shows changes
        local key = (not ms or ms < 0 or ms >= 100 * 3600000) and -1 or math.floor(ms / 1000)
        if b.durKey ~= key then b.durKey, b.duration = key, T.Duration(ms) end
      end
    end
    icon = math.max(18, math.min(64, tonumber(ui.GetSetting(iconKey)) or opts.icon))
    showEmptySlots = ui.GetSetting(emptySlotsKey) == "1"
    showSlotNumbers = ui.GetSetting(slotNumbersKey) == "1"
    cells = showEmptySlots and lastSlot or n
    measure(hud.WindowScale(window))
  end

  function winOpts.size()
    collect()
    return width, height
  end

  function winOpts.hideWhen()
    collect()
    return n == 0
  end

  function winOpts.onResize(w, h)
    if winOpts.resizeEdge == "bottom" then
      h = math.floor(h + 0.5)
      if h ~= savedHeight then saveSize(savedWidth, h) end
    else
      w = math.floor(w + 0.5)
      if w ~= savedWidth then saveSize(w, savedHeight) end
    end
  end

  function winOpts.menu()
    if ui.BeginMenu("Layout") then
      if ui.MenuItem("Horizontal", not vertical) then ui.SetSetting(layoutKey, "horizontal") end
      if ui.MenuItem("Vertical", vertical) then ui.SetSetting(layoutKey, "vertical") end
      ui.EndMenu()
    end
    if not vertical then
      ui.SetNextItemWidth(180)
      local v, changed = ui.SliderInt("Icons per row", perRow, 1, 20)
      if changed then
        ui.SetSetting(perRowKey, v)
        if savedWidth > 0 then saveSize(0, savedHeight) end
      end
    end
    ui.SetNextItemWidth(180)
    local v, changed = ui.SliderInt("Icon size", icon, 18, 64)
    if changed then ui.SetSetting(iconKey, v) end
    ui.Separator()
    toggle(namesKey, "Show spell names")
    toggle(badgesKey, "Show slot numbers")
    toggle(timeKey, "Show remaining time", true)
    toggle(emptySlotsKey, "Show empty slots")
    toggle(slotNumbersKey, "Show slot numbers in tooltips")
    ui.Separator()
    toggle(backgroundKey, "Show window background")
    toggle(titleKey, "Show title bar")
    toggle(resizeKey, "Allow window resizing")
  end

  local function drawTitle(x, y, w, s)
    local c = C.title
    ui.DrawRectFilled(x + s, y + s, x + w - s, y + titleHeight * s, c[1], c[2], c[3], c[4], 5 * s)
    local _, line = T.TextSize("Ag", NAME_FONT)
    local ty = y + (titleHeight * s - line) / 2
    T.Text(x + 8 * s, ty, name, C.text, NAME_FONT)
    local x0, x1 = x + w - 26 * s, x + w - 4 * s
    local clicked = T.Hit("##trayoptions", x0, y + 2 * s, x1, y + (titleHeight - 2) * s, false)
    T.HoverFx(x0, y + 2 * s, x1, y + (titleHeight - 2) * s, s)
    T.Text((x0 + x1) / 2, ty - s, "...", C.muted, NAME_FONT, "center")
    if clicked then hud.OpenFrameMenu() end
  end

  local function drawBadge(slot, x, y, s)
    local text = badges[slot]
    local tw, th = T.TextSize(text, T.font.tiny)
    local c = C.window
    ui.DrawRectFilled(x, y, x + tw + 4 * s, y + th + 2 * s, c[1], c[2], c[3], c[4], 3 * s)
    T.Text(x + 2 * s, y + s, text, C.text, T.font.tiny)
  end

  local function drawName(b, slot, x, y, iconRight, right, s, nameLine, timerLine)
    local left = iconRight + 6 * s
    local timer = hasTime and b and b.duration or ""
    local tc = b and b.ms and b.ms >= 0 and b.ms < 60000 and C.gold or C.muted
    local textLimit = right - 5 * s
    if vertical and timer ~= "" then textLimit = math.max(left, textLimit - T.TextSize(timer, T.font.tiny) - 6 * s) end
    local textWidth = math.max(1, textLimit - left)
    local first, second = "Empty slot", nil
    if b then first, second = fitNames(b, textWidth, s, not vertical) end
    local total = nameLine * (second and 2 or 1) + (not vertical and timer ~= "" and timerLine or 0)
    local ty = y + (cellHeight * s - total) / 2
    if vertical and hasBadges then T.Text(x + 5 * s, ty, badges[slot], C.muted, T.font.tiny) end
    ui.PushClipRect(left, y, textLimit, y + cellHeight * s, true)
    T.Text(left, ty, first, b and C.text or C.faint, NAME_FONT)
    if second then T.Text(left, ty + nameLine, second, C.text, NAME_FONT) end
    ui.PopClipRect()
    if timer ~= "" then
      if vertical then
        T.Text(right - 5 * s, y + (cellHeight * s - timerLine) / 2, timer, tc, T.font.tiny, "right")
      else
        T.Text(left, ty + total - timerLine, timer, tc, T.font.tiny)
      end
    end
  end

  local function body(x, y, w, h, s)
    if titleHeight > 0 and not hud.Unlocked() then drawTitle(x, y, w, s) end
    local namesLine, timerLine
    if hasNames then
      local _, nl = T.TextSize("Ag", NAME_FONT)
      local _, tl = T.TextSize("Ag", T.font.tiny)
      namesLine, timerLine = nl, tl
    end
    local showGaps = showEmptySlots and (vertical or hasNames or hasBadges)
    local buffIndex = 1
    -- Horizontal trays fill from the right. Vertical trays fill down, then continue in a column to the left.
    for i = 1, showGaps and cells or n do
      local b = buffIndex <= n and list[buffIndex] or nil
      local slot = showGaps and i or b.slot
      if b and b.slot == slot then buffIndex = buffIndex + 1 else b = nil end
      local index = (showEmptySlots and slot or i) - 1
      local col, row
      if vertical then col, row = index // rows, index % rows
      else col, row = index % perRow, index // perRow end
      local cx = x + w - (6 + (col + 1) * cellWidth + col * GAP) * s
      local cy = y + (5 + titleHeight + row * (cellHeight + GAP)) * s
      local right = cx + cellWidth * s
      local size = icon * s
      local ix, iy = cx + badgeGutter * s, cy
      if hasNames then
        ix = ix + (vertical and hasBadges and 33 or 5) * s
        iy = iy + (cellHeight * s - size) / 2
        if winOpts.panel and (not vertical or index % 2 == 0) then
          local c = C.ink
          ui.DrawRectFilled(cx, cy, right, cy + cellHeight * s, c[1], c[2], c[3], 0.04, 5 * s)
        end
      end
      if b then
        local tex, u0, v0, u1, v1 = API.windows.ChildArt(window, b.id, "decal")
        if type(tex) ~= "userdata" then tex = nil end
        b.icon.alpha = 0.2 + 0.8 * b.fade
        T.Icon(ix, iy, size, s, tex, u0, v0, u1, v1, b.good and C.good or C.bad, b.icon)
        T.Hit(b.hit, cx, cy, right, cy + cellHeight * s, false)
        if hasNames then T.HoverFx(cx, cy, right, cy + cellHeight * s, s)
        else T.HoverFx(ix, iy, ix + size, iy + size, s) end
        if ui.IsItemHovered() then
          ui.BeginTooltip()
          local title = showSlotNumbers and (b.name .. " - Slot " .. b.slot) or b.name
          local c = b.good and C.good or C.bad
          ui.TextColored(c[1], c[2], c[3], 1, title)
          ui.TextDisabled(T.LongDuration(b.ms))
          ui.TextDisabled(buffmenu.Hint(MENU))
          ui.EndTooltip()
        end
        if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
          buffmenu.Open(window, b, MENU)
          T.TakeRightClick()
        end
        if b.counter then T.Text(ix + size - 2 * s, iy + s, b.counter, b.counterColor, T.font.tiny, "right") end
        if b.limit then T.Text(ix + 2 * s, iy + size - 10 * s, b.limit, C.gold, T.font.tiny) end
      else
        local c = C.borderHi
        ui.DrawRect(ix, iy, ix + size, iy + size, c[1], c[2], c[3], c[4], 5 * s, 1)
      end
      if badgeGutter > 0 then drawBadge(slot, cx, cy, s) end
      if hasNames then
        drawName(b, slot, cx, cy, ix + size, right, s, namesLine, timerLine)
      elseif hasTime and b then
        local tc = b.ms and b.ms >= 0 and b.ms < 60000 and C.gold or C.muted
        T.Text(ix + size / 2, iy + size + s, b.duration, tc, T.font.tiny, "center")
      end
    end
    if buffmenu.Draw(window) then hud.OpenFrameMenu() end
  end
  ui.window(name, function()
    if not API.session.InGame() then return end
    hud.Window(window, name, winOpts, body)
  end)
end

return tray
