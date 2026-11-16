-- Looks (gamepad pack), each standing in for its stock window and acting through its children:
--   Set appearance (PlayerCustomizationWnd: /facepick, the inventory's Face button; also at character creation)
--       a row per feature: D-pad up / down picks it, left / right step it (its two step buttons, greyed where the
--       window disables them for your race and model); X Random, A Accept, B Cancel. The features are stepped and
--       tried on your model by the game's code, and Accept sends them. (No ImGui navigation: the D-pad is the rows'.)
--   Item preview (ObjectPreviewWnd: Alt + a right click on an item)
--       the client's 3D view of the item on your character, or of the item alone (hs.ItemPreviewImage): the right
--       stick turns it, LB / RB zoom, Y fits it again; the row below has the game's own turn, zoom and Reset
--       buttons (OPW_*). The mouse drags (left: turn, right: move) and the wheel zooms.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local G = dofile("lib/stock.lua")
local C = T.C

-- ---------------------------------------------------------------- set appearance
local PC = "PlayerCustomizationWnd"
-- { the row's ScreenID stem, the name shown }
local ROWS = {
  { "Face", "Face" }, { "Hair", "Hair style" }, { "Hair_Color", "Hair color" }, { "Eyes", "Eyes" },
  { "Beard", "Beard" }, { "Beard_Color", "Beard color" }, { "TattooIndex", "Tattoo" },
  { "FacialAttachmentIndex", "Facial detail" },
}
local feature = 1
ui.window("Set Appearance", function()
  G.testShow()
  -- (in game, or at character creation: hs.GameState 2)
  local shown = W.shown(PC) or (API.session.GameState and API.session.GameState() == 2 and API.windows.Shown(PC))
  if not shown then return end
  T.frame()
  local heritage = G.text(PC, "ParentId_Text")
  -- (on the left: the model being changed stays in view)
  if W.begin("Set Appearance", { w = 720, h = 1000, x = 80, title = "Set Appearance", subtitle = heritage ~= "" and heritage or nil,
        noNav = true, hints = { { "up", "Feature" }, { "left", "Change" }, { "x", "Random" }, { "a", "Accept" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local function step(i, dir)
      local id = ROWS[i][1] .. (dir < 0 and "_Down_Button" or "_Up_Button")
      if G.enabled(PC, id) then API.windows.ClickChild(PC, id, "left") end
    end
    local accept = false
    if W.focused() then
      if G.repeated("up") then feature = math.max(1, feature - 1) end
      if G.repeated("down") then feature = math.min(#ROWS, feature + 1) end
      if G.repeated("left") then step(feature, -1) end
      if G.repeated("right") then step(feature, 1) end
      if pad.menuPressed("x") then API.windows.ClickChild(PC, "Random_Button", "left") end
      if T.settledPress(pad, "a") then accept = true end
    end
    local rh, gap = T.px(72), T.px(10)
    for i, r in ipairs(ROWS) do
      local y = y0 + (i - 1) * (rh + gap)
      local on = G.enabled(PC, r[1] .. "_Text")
      T.rect(x0, y, x1, y + rh, C.raised, 1, T.px(12))
      ui.SetCursorPos(x0 - wx, y - wy)
      G.button(PC, r[1] .. "_Down_Button", "<", rh, rh, { always = true })
      T.font(23)
      T.textIn(x0 + rh, y, x1 - rh, y + rh, r[2], on and C.text or C.faint, 0.5)
      T.unfont()
      ui.SetCursorPos(x1 - rh - wx, y - wy)
      G.button(PC, r[1] .. "_Up_Button", ">", rh, rh, { always = true })
      if feature == i and pad.connected() then T.focusRing(x0, y, x1, y + rh, T.px(12)) end
    end
    local bh = T.px(64)
    ui.SetCursorPos(x0 - wx, y1 - 2 * bh - T.px(12) - wy)
    G.button(PC, "Random_Button", "Random", w, bh, { glyph = "x", always = true })
    local hw = (w - T.px(12)) / 2
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    G.button(PC, "Cancel_Button", "Cancel", hw, bh, { glyph = "b", always = true })
    ui.SameLine(0, T.px(12))
    G.button(PC, "Accept_Button", "Accept", hw, bh, { primary = true, glyph = "a", always = true })
    if accept then API.windows.ClickChild(PC, "Accept_Button", "left") end
  end
  if W.finish() then API.windows.ClickChild(PC, "Cancel_Button", "left") end
end)

-- ---------------------------------------------------------------- item preview
local OP = "ObjectPreviewWnd"
local BUTTONS = {
  { "OPW_RotateLeftButton", "Turn left" }, { "OPW_RotateRightButton", "Turn right" },
  { "OPW_RotateUpButton", "Tilt up" }, { "OPW_RotateDownButton", "Tilt down" },
  { "OPW_ZoomInButton", "Zoom in" }, { "OPW_ZoomOutButton", "Zoom out" }, { "OPW_ResetButton", "Reset" },
}
local lastSerial
ui.window("Item Preview", function()
  if not W.shown(OP) then return end
  T.frame()
  local p = API.preview.Draw()
  local fresh = p and p.serial ~= lastSerial
  if fresh then lastSerial = p.serial end -- a new item: the window takes the pad again (below)
  local title = p and p.show and p.name ~= "" and p.name or "Item Preview"
  if W.begin("Item Preview", { w = 1100, h = 1000, title = title, alwaysFocus = true,
        hints = { { "rs", "Turn" }, { "lb", "Zoom out" }, { "rb", "Zoom in" }, { "y", "Reset" }, { "b", "Close" } } }) then
    if fresh then pad.focus("Item Preview") end
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(64)
    local vy1 = y1 - bh - T.px(14)
    if p and p.show then
      T.rect(x0, y0, x1, vy1, C.sunken, 1, T.px(14))
      local tex = API.preview.Image(x1 - x0, vy1 - y0)
      if tex then ui.DrawImage(tex, x0, y0, x1, vy1, 0, 0, 1, 1) end
      -- the mouse over the view
      ui.SetCursorPos(x0 - wx, y0 - wy)
      ui.InvisibleButton("##previewview", x1 - x0, vy1 - y0, ui.ButtonFlags.MouseButtonLeft + ui.ButtonFlags.MouseButtonRight)
      local io = ui.GetIO()
      if ui.IsItemHovered() and io.wheel ~= 0 then API.preview.Control("zoom", io.wheel > 0 and 0.9 or 1.1) end
      if ui.IsItemActive() then
        if ui.IsMouseDown(ui.Mouse.Left) then
          API.preview.Control("rotate", io.mouseDX * 0.01, -io.mouseDY * 0.01)
        elseif ui.IsMouseDown(ui.Mouse.Right) then
          API.preview.Control("pan", -io.mouseDX / (x1 - x0), io.mouseDY / (vy1 - y0))
        end
      end
      -- the pad: the right stick turns it, LB / RB zoom, Y fits it again
      if W.focused() and pad.mode() == "menu" then
        local dt = 1 / math.max(20, io.fps or 60)
        local rx, ry = pad.axis("rx") or 0, pad.axis("ry") or 0
        if math.abs(rx) > 0.15 or math.abs(ry) > 0.15 then API.preview.Control("rotate", rx * 3 * dt, -ry * 3 * dt) end
        if pad.down("lb") then API.preview.Control("zoom", 1 + dt) end
        if pad.down("rb") then API.preview.Control("zoom", 1 - dt) end
        if W.pressed("y") then API.windows.ClickChild(OP, "OPW_ResetButton", "left") end
      end
      local gap = T.px(10)
      local bw = (w - (#BUTTONS - 1) * gap) / #BUTTONS
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      for i, b in ipairs(BUTTONS) do
        if i > 1 then ui.SameLine(0, gap) end
        G.button(OP, b[1], b[2], bw, bh, { always = true, glyph = b[1] == "OPW_ResetButton" and "y" or nil })
      end
    else
      local msg = G.text(OP, "OPW_NoItemPreviewAvailable_Label")
      if msg == "" then msg = "No item preview available." end
      T.font(24)
      T.textIn(x0, y0, x1, vy1, msg, C.faint, 0.5)
      T.unfont()
    end
  end
  if W.finish() then API.windows.Show(OP, false) end
end)
