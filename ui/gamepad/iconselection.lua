-- Icon selection (gamepad pack): stands in for IconSelectionWnd (a hot button's Assign Icon). The page's icons as a
-- grid: the D-pad moves the selection (a click on that icon in the original, ISW_Icon:<n>), LB / RB turn the page
-- (its arrows), LT steps the icon type and RT how many a page shows (its two combo boxes), X types the icon's number
-- (its ID box, on the on-screen keys), A is Accept (the hot button takes the icon) and B closes it. The mouse clicks
-- icons and the buttons, and the wheel scrolls a long page. (No ImGui navigation while the grid has the pad.)
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local G = dofile("lib/stock.lua")
local C = T.C
local ISW = "IconSelectionWnd"

local kb, typing = G.KF.new(), false
local top = 0 -- the first grid row in view

ui.window("Icon Selection", function()
  G.testShow()
  if not W.shown(ISW) then
    typing = false
    return
  end
  T.frame()
  local first, last, selected = API.hotbars.IconSelectionIcons()
  first, last, selected = first or 0, last or -1, selected or -1
  local hints = typing and G.editHints("Done")
      or { { "up", "Move" }, { "lb", "Page" }, { "lt", "Type" }, { "rt", "Per page" }, { "x", "Number" }, { "a", "Accept" }, { "b", "Close" } }
  if W.begin("Icon Selection", { w = 1500, h = 1000, title = "Choose an Icon",
        subtitle = G.text(ISW, "ISW_PageRange_Label"), noNav = not typing, hints = hints }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local gap = T.px(12)
    -- the selected icon, its number and the page
    local sel = T.px(96)
    T.rect(x0, y0, x0 + sel, y0 + sel, C.sunken, 1, T.px(12))
    local tex, u0, v0, u1, v1 = API.hotbars.IconSelectionIcon(selected)
    if tex then T.icon(tex, u0, v0, u1, v1, x0 + T.px(8), y0 + T.px(8), x0 + sel - T.px(8), y0 + sel - T.px(8)) end
    local cx = x0 + sel + T.px(20)
    local cw = (x1 - cx - 3 * gap) / 4
    local bh = T.px(84)
    ui.SetCursorPos(cx - wx, y0 + (sel - bh) / 2 - wy)
    local function pick(id, back)
      local choices, cur = API.windows.ComboInfo(ISW, id)
      if choices and #choices > 0 then API.windows.ComboSelect(ISW, id, ((cur or -1) + (back and -1 or 1)) % #choices) end
    end
    if typing then
      T.font(24)
      T.textIn(cx, y0, x1, y0 + sel, "Type the icon's number, then Done", C.dim, 0)
      T.unfont()
    else
      G.combo(ISW, "ISW_Type_Combobox", "Type (LT)", cw, bh)
      ui.SameLine(0, gap)
      G.combo(ISW, "ISW_PerPage_Combobox", "Per page (RT)", cw, bh)
      ui.SameLine(0, gap)
      G.button(ISW, "ISW_PageLeft_Button", "Page " .. G.text(ISW, "ISW_PageIndex_Label"), cw / 2 - gap / 2, bh, { glyph = "lb", always = true })
      ui.SameLine(0, gap)
      G.button(ISW, "ISW_PageRight_Button", "Next", cw / 2 - gap / 2, bh, { glyph = "rb", always = true })
      ui.SameLine(0, gap)
      if T.button("##iswnumber", "Icon " .. G.text(ISW, "ISW_Slot_EditBox"), cw, bh, { glyph = "x", align = 0 }, pad) then
        typing = true
        G.openKeys(kb)
      end
    end
    local gy = y0 + sel + T.px(20)
    local fh = T.px(64)
    if typing then
      ui.SetCursorPos(x0 - wx, gy - wy)
      if G.edit(kb, ISW, "ISW_Slot_EditBox", x0, x1, { max = 5, digits = true }) == "done" then typing = false end
    else
      -- the grid
      local cell = T.px(76)
      local gx1 = x1 - T.px(14)
      local perRow = math.max(1, math.floor((gx1 - x0) / cell))
      local count = math.max(0, last - first + 1)
      local rows = (count + perRow - 1) // perRow
      local gy1 = y1 - fh - T.px(16)
      local shownRows = math.max(1, math.floor((gy1 - gy) / cell))
      -- the pad
      if W.focused() then
        local idx = selected - first
        if idx < 0 or idx >= count then idx = -1 end
        local function go(i)
          i = math.max(0, math.min(count - 1, i))
          API.windows.ClickChild(ISW, "ISW_Icon:" .. (first + i), "left")
          local r = i // perRow
          if r < top then top = r elseif r >= top + shownRows then top = r - shownRows + 1 end
        end
        if count > 0 then
          if G.repeated("left") then go(idx < 0 and 0 or idx - 1) end
          if G.repeated("right") then go(idx < 0 and 0 or idx + 1) end
          if G.repeated("up") then go(idx < 0 and 0 or idx - perRow) end
          if G.repeated("down") then go(idx < 0 and 0 or idx + perRow) end
        end
        if pad.menuPressed("lb") then API.windows.ClickChild(ISW, "ISW_PageLeft_Button", "left") top = 0 end
        if pad.menuPressed("rb") then API.windows.ClickChild(ISW, "ISW_PageRight_Button", "left") top = 0 end
        if pad.menuPressed("lt") then pick("ISW_Type_Combobox") top = 0 end
        if pad.menuPressed("rt") then pick("ISW_PerPage_Combobox") top = 0 end
        if pad.menuPressed("x") then
          typing = true
          G.openKeys(kb)
        end
        if T.settledPress(pad, "a") then API.windows.ClickChild(ISW, "ISW_Accept_Button", "left") end
      end
      if ui.IsMouseHoveringRect(x0, gy, x1, gy1) then
        local wheel = ui.GetIO().wheel or 0
        if wheel ~= 0 then top = top - math.floor(wheel * 2 + 0.5) end
      end
      top = math.max(0, math.min(math.max(0, rows - shownRows), top))
      T.rect(x0, gy, x1, gy1, C.sunken, 1, T.px(14))
      for r = top, math.min(rows - 1, top + shownRows - 1) do
        for c = 0, perRow - 1 do
          local n = first + r * perRow + c
          if n > last then break end
          local ix, iy = x0 + T.px(6) + c * cell, gy + T.px(6) + (r - top) * cell
          ui.SetCursorPos(ix - wx, iy - wy)
          if ui.InvisibleButton("##icon" .. n, cell - T.px(4), cell - T.px(4)) then API.windows.ClickChild(ISW, "ISW_Icon:" .. n, "left") end
          local hovered = ui.IsItemHovered()
          local t2, a0, b0, a1, b1 = API.hotbars.IconSelectionIcon(n)
          if t2 then T.icon(t2, a0, b0, a1, b1, ix + T.px(4), iy + T.px(4), ix + cell - T.px(8), iy + cell - T.px(8)) end
          if n == selected then T.focusRing(ix, iy, ix + cell - T.px(4), iy + cell - T.px(4), T.px(8))
          elseif hovered then T.outline(ix, iy, ix + cell - T.px(4), iy + cell - T.px(4), C.text, 0.6, T.px(8)) end
          if hovered then ui.SetTooltip(tostring(n)) end
        end
      end
      if rows > shownRows then -- a scroll bar on the right
        local th = math.max(T.px(30), (gy1 - gy) * shownRows / rows)
        local sy = gy + (gy1 - gy - th) * (top / (rows - shownRows))
        T.rect(x1 - T.px(10), sy, x1 - T.px(4), sy + th, C.dim, 0.8, T.px(3))
      end
      local hw = (w - gap) / 2
      ui.SetCursorPos(x0 - wx, y1 - fh - wy)
      if T.button("##iswclose", "Close", hw, fh, { glyph = "b" }, pad) then API.windows.Show(ISW, false) end
      ui.SameLine(0, gap)
      G.button(ISW, "ISW_Accept_Button", "Accept", hw, fh, { primary = true, glyph = "a", always = true })
    end
  end
  if W.finish() then
    if typing then typing = false else API.windows.Show(ISW, false) end
  end
end)
