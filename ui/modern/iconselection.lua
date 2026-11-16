-- Icon selection: replaces the game's IconSelectionWnd (a hot button's Assign Icon). A toolbar with the icon type,
-- the icons a page and the page arrows (with the page and the range of icons it holds), then the page's icons in a
-- grid that reflows to the width (a click selects one, the selected one ringed), and at the bottom the selected icon,
-- its number (type one to jump to it) and Accept, which gives the hot button that icon. The toolbar, the number box
-- and Accept are the window's children, the icons hs.IconSelectionIcons / hs.IconSelectionIcon with clicks as
-- "ISW_Icon:<n>" (app/lua_ui_hotbar.cpp), so the icon set is the game's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "IconSelectionWnd"

-- The page's icons in a scrolled grid, w x h (only the rows in view are drawn).
local function icons(w, h, s)
  local first, last, selected = API.hotbars.IconSelectionIcons()
  ui.PushStyleColor(ui.Col.ChildBg, C.inset[1], C.inset[2], C.inset[3], C.inset[4] or 1)
  ui.BeginChild("##iswicons", w, h, true)
  ui.PopStyleColor(1)
  if not first then
    ui.EndChild()
    return
  end
  local cell, icon, pad = 44 * s, 38 * s, 3 * s
  local aw = ui.GetContentRegionAvail()
  local perRow = math.max(1, math.floor(aw / cell))
  local count = math.max(0, last - first + 1)
  local rows = (count + perRow - 1) // perRow
  local ox, oy = ui.GetCursorScreenPos()
  local scroll = ui.GetScrollY()
  local firstRow = math.max(0, math.floor(scroll / cell) - 1)
  local lastRow = math.min(rows - 1, math.floor((scroll + h) / cell) + 1)
  for row = firstRow, lastRow do
    for col = 0, perRow - 1 do
      local n = first + row * perRow + col
      if n > last then break end
      local x0, y0 = ox + col * cell, oy + row * cell
      ui.SetCursorScreenPos(x0, y0)
      if ui.InvisibleButton("##icon" .. n, cell, cell) then API.windows.ClickChild(WINDOW, "ISW_Icon:" .. n, "left") end
      local hovered = ui.IsItemHovered()
      local tex, u0, v0, u1, v1 = API.hotbars.IconSelectionIcon(n)
      local ring = (n == selected and C.accent) or (hovered and C.borderHi) or nil
      T.Icon(x0 + pad, y0 + pad, icon, s, tex, u0, v0, u1, v1, ring, { radius = 4 })
      if n == selected then
        ui.DrawRect(x0 + 1, y0 + 1, x0 + cell - 1, y0 + cell - 1, C.accent[1], C.accent[2], C.accent[3], 1, 6 * s, 2 * s)
      end
      if hovered then ui.SetTooltip("Icon " .. n) end
    end
  end
  ui.SetCursorScreenPos(ox, oy + rows * cell)
  ui.Dummy(1, 1) -- the scroll range
  ui.EndChild()
end

local OPTS = {
  size = function() return 420, 460 end,
  resizable = { 300, 300 },
}
local function body(x, y, w, h, s)
  local bh = K.ButtonH(s)
  -- the toolbar
  K.Caption("Type")
  ui.SameLine()
  W.Combo(WINDOW, "ISW_Type_Combobox", { width = 110 * s })
  ui.SameLine(0, 14 * s)
  K.Caption("Per page")
  ui.SameLine()
  W.Combo(WINDOW, "ISW_PerPage_Combobox", { width = 70 * s })
  -- the pages
  local arrow = 28 * s
  W.Button(WINDOW, "ISW_PageLeft_Button", "<", { width = arrow, tooltip = "The page before" })
  ui.SameLine()
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, "Page " .. W.Text(WINDOW, "ISW_PageIndex_Label"))
  ui.SameLine()
  W.Button(WINDOW, "ISW_PageRight_Button", ">", { width = arrow, tooltip = "The page after" })
  ui.SameLine(0, 14 * s)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, W.Text(WINDOW, "ISW_PageRange_Label"))
  -- the icons
  local well = 44 * s
  local footer = math.max(well, bh) + 10 * s
  local _, ah = ui.GetContentRegionAvail()
  icons(0, math.max(60 * s, ah - footer), s)
  -- the selected icon, its number and Accept
  ui.Dummy(0, 4 * s)
  local fx, fy = ui.GetCursorScreenPos()
  local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, "ISW_SelectedItem_Button", "decal")
  if type(tex) ~= "userdata" then tex = nil end
  T.Icon(fx, fy, well, s, tex, u0, v0, u1, v1, C.accent, { radius = 5 })
  local _, lh = ui.CalcTextSize("Ag")
  local my = fy + (well - lh - 6 * s) / 2
  ui.SetCursorScreenPos(fx + well + 10 * s, my + 3 * s)
  K.Caption("Icon")
  ui.SameLine()
  ui.SetCursorScreenPos(ui.GetCursorScreenPos(), my)
  W.Edit(WINDOW, "ISW_Slot_EditBox", { width = 70 * s, max = 6 })
  if ui.IsItemHovered() then ui.SetTooltip("Type an icon's number to select it") end
  local accW = 100 * s
  ui.SetCursorScreenPos(x + w - accW, fy + (well - bh) / 2)
  W.Button(WINDOW, "ISW_Accept_Button", "Accept", { primary = true, width = accW, height = bh,
    tooltip = "Give the hot button this icon" })
  ui.SetCursorScreenPos(fx, fy)
  ui.Dummy(w, well)
end

local function testOpen() API.windows.Show(WINDOW, true) end
ui.window("Icon Selection", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  hud.Dialog(WINDOW, "Icon Selection", OPTS, body)
end)
