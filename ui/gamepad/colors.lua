-- Colours (gamepad pack), each standing in for its stock window and acting through its children:
--   Color picker (ColorPickerWnd: the tint window's brush, the map's colour buttons, the label window)
--       a large preview with its hex value, red / green / blue as bars and the fifteen preset swatches. D-pad up /
--       down picks a row, left / right change its value by one (held: on and on) or, on the presets, pick the next
--       swatch; LB / RB change it by 16. The bars and swatches are the window's sliders and buttons (hs.SetSlider,
--       hs.ClickChild): it keeps the three values in step, and Accept (A) hands the colour to whoever opened it.
--       B cancels (the close box). (No ImGui navigation in it: the D-pad is the values'.)
--   Armor dye (BodyTintWnd: the inventory's tint button)
--       the brush (A opens the colour picker on it), then a row per tintable slot: its colour now (A takes it as
--       the brush) and its new dye (A paints the brush on it), marked when it will change; the reagents, Reset and
--       Save Dye (a Vial of Prismatic Dye per changed slot; the window enables it when you have enough).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local G = dofile("lib/stock.lua")
local C = T.C

-- ---------------------------------------------------------------- colour picker
local CP = "ColorPickerWnd"
local CHANNELS = {
  { "Red", "CPW_RedSlider", { 0.95, 0.35, 0.35, 1 } },
  { "Green", "CPW_GreenSlider", { 0.35, 0.85, 0.45, 1 } },
  { "Blue", "CPW_BlueSlider", { 0.40, 0.60, 1.00, 1 } },
}
local row, preset = 1, 0
ui.window("Color Picker", function()
  G.testShow()
  if not W.shown(CP) then return end
  T.frame()
  if W.begin("Color Picker", { w = 1200, h = 780, title = "Color", noNav = true,
        hints = { { "up", "Row" }, { "left", "-1" }, { "right", "+1" }, { "lb", "-16" }, { "rb", "+16" },
          { "a", "Accept" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local function value(ch)
      local _, _, v, count = API.windows.ChildState(CP, ch[2])
      return v or 0, math.max(1, (count or 256) - 1)
    end
    local function set(ch, v)
      local _, maxv = value(ch)
      API.windows.SetSlider(CP, ch[2], math.max(0, math.min(maxv, v)))
    end
    local function pick(i)
      preset = i % 15
      API.windows.ClickChild(CP, "CPW_TC" .. preset .. "Button", "left")
    end
    -- the pad
    local accept = false
    if W.focused() then
      if G.repeated("up") then row = math.max(1, row - 1) end
      if G.repeated("down") then row = math.min(4, row + 1) end
      local ch = CHANNELS[row]
      if ch then
        local v = value(ch)
        if G.repeated("left") then set(ch, v - 1) end
        if G.repeated("right") then set(ch, v + 1) end
        if G.repeated("lb") then set(ch, v - 16) end
        if G.repeated("rb") then set(ch, v + 16) end
      else
        if G.repeated("left") then pick(preset + 14) end
        if G.repeated("right") then pick(preset + 1) end
      end
      if T.settledPress(pad, "a") then accept = true end
    end
    -- the colour now (the window's CPW_TC15Button shows it)
    local r, g, b = G.tint(CP, "CPW_TC15Button")
    local pv = T.px(220)
    G.swatch(x0, y0, x0 + pv, y0 + pv, r, g, b, T.px(16))
    T.font(24)
    T.textIn(x0, y0 + pv + T.px(8), x0 + pv, y0 + pv + T.px(44), G.hex(r, g, b), C.dim, 0.5)
    T.unfont()
    -- the channels as bars: the mouse sets one by a click or a drag on it
    local cx0 = x0 + pv + T.px(40)
    local rh = T.px(64)
    for i, ch in ipairs(CHANNELS) do
      local y = y0 + (i - 1) * (rh + T.px(14))
      local v, maxv = value(ch)
      local focused = row == i and pad.connected()
      T.rect(cx0, y, x1, y + rh, C.raised, 1, T.px(12))
      T.font(22)
      T.textIn(cx0 + T.px(18), y, cx0 + T.px(130), y + rh, ch[1], ch[3], 0)
      T.textIn(x1 - T.px(100), y, x1 - T.px(18), y + rh, tostring(v), C.text, 1)
      T.unfont()
      local bx0, bx1 = cx0 + T.px(140), x1 - T.px(120)
      T.bar(bx0, y + rh / 2 - T.px(10), bx1, y + rh / 2 + T.px(10), v / maxv, ch[3])
      ui.SetCursorPos(bx0 - wx, y - wy)
      ui.InvisibleButton("##bar" .. i, bx1 - bx0, rh)
      if ui.IsItemActive() then
        local mx = ui.GetMousePos()
        set(ch, math.floor((mx - bx0) / (bx1 - bx0) * maxv + 0.5))
        row = i
      end
      if focused then T.focusRing(cx0, y, x1, y + rh, T.px(12)) end
    end
    -- the presets
    local py = y0 + 3 * (rh + T.px(14)) + T.px(10)
    T.font(18)
    T.draw(cx0, py, "PRESETS", C.dim)
    T.unfont()
    py = py + T.px(34)
    local gap = T.px(10)
    local sz = math.min(T.px(64), (x1 - cx0 - 14 * gap) / 15)
    local hex = G.hex(r, g, b)
    for i = 0, 14 do
      local id = "CPW_TC" .. i .. "Button"
      local sx = cx0 + i * (sz + gap)
      local sr, sg, sb = G.tint(CP, id)
      G.swatch(sx, py, sx + sz, py + sz, sr, sg, sb)
      if G.hex(sr, sg, sb) == hex then T.outline(sx - T.px(3), py - T.px(3), sx + sz + T.px(3), py + sz + T.px(3), C.text, 0.9, T.px(10), T.px(2)) end
      if row == 4 and preset == i and pad.connected() then T.focusRing(sx, py, sx + sz, py + sz, T.px(8)) end
      ui.SetCursorPos(sx - wx, py - wy)
      if ui.InvisibleButton("##preset" .. i, sz, sz) then
        row = 4
        pick(i)
      end
    end
    -- Cancel and Accept
    local bh = T.px(64)
    local hw = (w - T.px(12)) / 2
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    local cancel = T.button("##cpcancel", "Cancel", hw, bh, { glyph = "b" }, pad)
    ui.SameLine(0, T.px(12))
    G.button(CP, "CPW_Accept_Button", "Accept", hw, bh, { primary = true, glyph = "a", always = true })
    if accept then API.windows.ClickChild(CP, "CPW_Accept_Button", "left") end
    if cancel then API.windows.Show(CP, false) end
  end
  if W.finish() then API.windows.Show(CP, false) end
end)

-- ---------------------------------------------------------------- armour dye
local BT = "BodyTintWnd"
-- { the slot's name in the ScreenIDs, the name shown }
local SLOTS = { { "Head", "Head" }, { "Chest", "Chest" }, { "Arms", "Arms" }, { "ForeArm", "Wrists" },
  { "Hands", "Hands" }, { "Legs", "Legs" }, { "Feet", "Feet" } }

-- A swatch button w x h at the cursor for the child `id`: A or a click is its click.
local function swatchButton(id, w, h, sub)
  local r, g, b = G.tint(BT, id)
  local clicked, focused, x0, y0, x1, y1 = T.button("##" .. id, "", w, h, {}, pad)
  G.swatch(x0 + T.px(8), y0 + T.px(8), x1 - T.px(8), y1 - T.px(8), r, g, b)
  if sub then
    T.font(16)
    T.textIn(x0, y0, x1, y1, sub, (r + g + b) / 3 > 0.55 and { 0, 0, 0, 1 } or { 1, 1, 1, 1 }, 0.5, 0.8)
    T.unfont()
  end
  if focused then T.focusRing(x0, y0, x1, y1, T.px(12)) end
  if clicked then API.windows.ClickChild(BT, id, "left") end
  return r, g, b
end

ui.window("Armor Dye", function()
  if not W.shown(BT) then return end
  T.frame()
  if W.begin("Armor Dye", { w = 1100, h = 1000, title = "Armor Dye",
        hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    -- the brush
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local br, bg, bb = swatchButton("TINT_CurBrushBtn", T.px(200), T.px(76), "Brush")
    T.font(22)
    T.draw(x0 + T.px(220), y0 + T.px(8), "Brush " .. G.hex(br, bg, bb))
    T.unfont()
    T.font(18)
    T.draw(x0 + T.px(220), y0 + T.px(42), "A on it chooses its color; A on a dye paints it", C.dim)
    T.unfont()
    -- the slots: name, now, dye, changed
    local top = y0 + T.px(96)
    T.font(18)
    local nameW, colW = T.px(200), (w - T.px(200) - T.px(60) - 2 * T.px(12)) / 2
    T.draw(x0 + nameW, top, "NOW", C.dim)
    T.draw(x0 + nameW + colW + T.px(12), top, "NEW DYE", C.dim)
    T.unfont()
    local rh, gap = T.px(64), T.px(10)
    for i, slot in ipairs(SLOTS) do
      local y = top + T.px(30) + (i - 1) * (rh + gap)
      T.font(22)
      T.textIn(x0, y, x0 + nameW, y + rh, slot[2], C.text, 0)
      T.unfont()
      ui.SetCursorPos(x0 + nameW - wx, y - wy)
      local nr, ng, nb = swatchButton("TINT_O" .. slot[1] .. "ColorBtn", colW, rh)
      ui.SameLine(0, T.px(12))
      local dr, dg, db = swatchButton("TINT_" .. slot[1] .. "Btn", colW, rh)
      if G.hex(nr, ng, nb) ~= G.hex(dr, dg, db) then
        local gc = C.gold or C.accent
        ui.DrawCircleFilled(x1 - T.px(24), y + rh / 2, T.px(9), gc[1], gc[2], gc[3], 1)
      end
    end
    -- the reagents
    local ry = top + T.px(30) + #SLOTS * (rh + gap) + T.px(6)
    local need, have = G.text(BT, "TINT_ReagentsNeeded"), G.text(BT, "TINT_ReagentsLabel")
    T.font(20)
    T.draw(x0, ry, (need ~= "" and need or "Reagents Needed: 0") .. (have ~= "" and ("     " .. have) or ""), C.dim)
    T.unfont()
    local bh = T.px(64)
    local hw = (w - T.px(12)) / 2
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    G.button(BT, "TINT_ResetBodytTint", "Reset", hw, bh, { always = true })
    ui.SameLine(0, T.px(12))
    G.button(BT, "TINT_SaveTint", "Save Dye", hw, bh, { primary = true, always = true })
  end
  if W.finish() then API.windows.Show(BT, false) end
end)
