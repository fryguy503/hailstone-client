-- Map (gamepad pack): stands in for MapViewWnd, the large map the minimap opens. The zone's map fitted to a large
-- window, north up (hs.DrawZoneMap: the stock map's lines, labels, group, /find path, compass points and player),
-- following the player until it is panned. A stick pans, LB / RB zoom out / in, Y centres on the player again,
-- X shows / hides the labels, RT turns Auto Z (hs.MapAutoZ: what is well above or below the player fades) on or off;
-- the Map Pack dropdown is reached with the D-pad. With the mouse, dragging pans and the wheel zooms.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local C = T.C
local E = dofile("lib/extensions.lua")
local MC = dofile("lib/map_controls.lua")

local MAP = "MapViewWnd"
local PACK_COMBO = "MVW_MapsDirCombobox"
local ZOOMS = { 1, 1.5, 2.25, 3.4, 5, 7.5, 11 }
local M = {
  zoom = 1,
  follow = true, -- centred on the player
  cx = nil, cy = nil, -- the centre when panned, in map units
  labels = ui.GetSetting("map.labels", "1") == "1",
  packOpen = false,
  packFocused = false, -- the Map Pack dropdown had the focus last frame (closed)
}

local function setZoom(step)
  M.zoom = math.max(1, math.min(#ZOOMS, M.zoom + step))
end

local function mapPack(packs, selected)
  ui.SetNextItemWidth(T.px(180))
  -- Y is also an ImGui activation key, but on the map it recentres the view.
  ui.BeginDisabled(pad.yActivated(true))
  local open = ui.BeginCombo("Map Pack##map_pack", packs[selected + 1] or packs[1])
  local focused = not open and ui.IsItemFocused()
  M.packOpen = open
  if open then
    for i, name in ipairs(packs) do
      local active = i - 1 == selected
      if ui.Selectable(name .. "###map_pack_" .. i, active) then
        M.packOpen = false
        if not active then API.windows.ComboSelect(MAP, PACK_COMBO, i - 1) end
      end
      if active then ui.SetItemDefaultFocus() end
    end
    ui.EndCombo()
  end
  ui.EndDisabled()
  return open, focused
end

ui.window("Map", function()
  if not W.shown(MAP) then
    MC.Reset()
    M.packOpen, M.packFocused = false, false
    return
  end
  local zone, short, entry = API.map.ZoneName(true)
  if M.zoneEntry ~= entry or M.playerZone ~= short then
    M.zoneEntry, M.playerZone = entry, short
    M.zoom, M.follow, M.cx, M.cy = 1, true, nil, nil
  end
  local hasControls = MC.Sync("zone")
  local packs, selected = API.windows.ComboInfo(MAP, PACK_COMBO)
  local hasPack = packs and #packs > 0
  local packWasOpen = M.packOpen
  M.packOpen = false
  local hints = { { "lb", "Zoom out" }, { "rb", "Zoom in" }, { "y", "Centre" },
    { "x", M.labels and "Hide labels" or "Labels" }, { "rt", API.map.AutoZ() and "Auto Z off" or "Auto Z" },
    { "a", packWasOpen and "Select" or M.packFocused and "Map pack"
      or API.map.FindPathShown() and "End find" or "Find path to centre" }, { "b", packWasOpen and "Back" or "Close" } }
  if hasControls then table.insert(hints,1,{ "lt", "Map controls" }) end
  if W.begin("Map", { w = 1500, h = 960, title = zone and zone ~= "" and zone or "Map", close = not (MC.Open() or packWasOpen),
        hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local context = E.MapContext("zone")
    local controlsWasOpen = MC.Open()
    local packOpen, packFocused = false, false
    if hasPack then
      ui.SetCursorScreenPos(x0, y0)
      packOpen, packFocused = mapPack(packs, selected)
    end
    M.packFocused = packFocused -- (the next frame's A and B hints)
    -- Native ImGui navigation handles the combo. A dismiss frame still belongs to the dropdown.
    local packInput = packWasOpen or packOpen
    if hasControls then
      if hasPack then ui.SameLine() end
      if E.Component(MAP, "toolbar.mapcontrols", context, function() return ui.Button("Map controls") end)
          or (not packInput and W.pressed("lt")) then
        MC.Toggle("zone")
      end
    end
    if hasPack or hasControls then y0 = y0 + T.px(44) end
    local mapInput = not (controlsWasOpen or MC.Open() or packInput)
    local wx, wy = ui.GetWindowPos()
    ui.SetCursorPos(x0 - wx, y0 - wy)
    ui.InvisibleButton("##maparea", x1 - x0, y1 - y0, ui.ButtonFlags.EnableNav)
    ui.SetItemDefaultFocus()
    local hovered = ui.IsItemHovered()
    T.rect(x0, y0, x1, y1, C.mapBg, 1, T.px(14))
    T.font(16)
    local cx, cy = M.cx, M.cy
    local canvasDefault = false
    local k, mx, my, px, py = E.Component(MAP,"map.canvas",context,function()
      canvasDefault=true
      return API.map.DrawZone(x0, y0, x1, y1, ZOOMS[M.zoom], cx, cy, M.labels)
    end)
    mapInput = mapInput and canvasDefault
    T.unfont()
    T.outline(x0, y0, x1, y1, C.edge, 1, T.px(14), 1)
    if k then
      -- following: the player in the middle once zoomed in (the whole zone at zoom 1)
      if M.follow then
        if M.zoom > 1 and px then M.cx, M.cy = px, py else M.cx, M.cy = mx, my end
      end
      -- panning: the mouse dragging, or a stick (map units per second at full deflection)
      local io = ui.GetIO()
      local dx, dy = 0, 0
      if mapInput and ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left) then
        dx, dy = -(io.mouseDX or 0) / k, -(io.mouseDY or 0) / k
      end
      if mapInput and W.focused() and pad.connected() then
        local sx = (pad.axis("lx") or 0) + (pad.axis("rx") or 0)
        local sy = (pad.axis("ly") or 0) + (pad.axis("ry") or 0)
        local speed = (x1 - x0) * 0.8 / k * (1 / math.max(io.fps or 60, 10))
        dx, dy = dx + sx * speed, dy + sy * speed
      end
      if dx ~= 0 or dy ~= 0 then
        M.follow = false
        M.cx, M.cy = (M.cx or mx) + dx, (M.cy or my) + dy
      end
      if mapInput and hovered and (io.wheel or 0) ~= 0 then setZoom(io.wheel > 0 and 1 or -1) end
    end
    if mapInput and W.pressed("lb") then setZoom(-1) end
    if mapInput and W.pressed("rb") then setZoom(1) end
    if mapInput and W.pressed("y") then M.follow = true end
    -- the find path (the stock map's right click) to the point in the middle, or ending the shown one
    if mapInput and not packFocused and W.pressed("a") then
      if API.map.FindPathShown() then API.find.End() elseif M.cx then API.find.MapLocation(M.cx, M.cy) end
    end
    if mapInput and W.pressed("rt") then API.map.AutoZ(not API.map.AutoZ()) end
    if mapInput and W.pressed("x") then
      M.labels = not M.labels
      ui.SetSetting("map.labels", M.labels)
    end
    -- the zoom, bottom left
    T.font(17)
    T.draw(x0 + T.px(14), y1 - T.px(32), string.format("x%g%s", ZOOMS[M.zoom], M.follow and "" or "  (panned)"), C.dim)
    T.unfont()
    MC.Draw("zone", MC.Open() and W.pressed("b"))
  end
  if W.finish() then API.windows.Show(MAP, false) end
end)
