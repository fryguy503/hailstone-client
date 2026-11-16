-- Colour picker: replaces the game's ColorPickerWnd (opened by the tint window's brush, the map's colour buttons, the
-- label window and the options' user colours). A large preview of the colour with its hex value, red / green / blue
-- as sliders with number boxes beside them, the window's fifteen preset swatches, Accept and Cancel. The sliders,
-- boxes and swatches are the window's own (hs.SetSlider, hs.SetWindowText, hs.ClickChild): the window keeps the
-- three values in step and Accept hands the colour to whoever opened it. Cancel is the close box.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "ColorPickerWnd"

local CHANNELS = {
  { "Red", "CPW_RedSlider", "CPW_RedSliderInput", { 0.95, 0.35, 0.35 } },
  { "Green", "CPW_GreenSlider", "CPW_GreenSliderInput", { 0.35, 0.85, 0.45 } },
  { "Blue", "CPW_BlueSlider", "CPW_BlueSliderInput", { 0.40, 0.60, 1.00 } },
}

local WINDOW_OPTS = {
  size = function() return 380, 236 end,
  resizable = { 300, 220 },
  place = K.Centered,
}
local function drawWindow(x, y, w, h, s)
  -- the colour now (the window's CPW_TC15Button shows it)
  local r, g, b = K.Tint(WINDOW, "CPW_TC15Button")
  local pv = 76 * s
  ui.BeginGroup()
  K.Swatch("##preview", r, g, b, pv, s, { tooltip = "The chosen color" })
  local hex = K.Hex(r, g, b)
  local tw = ui.CalcTextSize(hex)
  local cx, cy = ui.GetCursorPos()
  ui.SetCursorPos(cx + (pv - tw) / 2, cy)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, hex)
  ui.EndGroup()
  ui.SameLine(0, 12 * s)
  -- the three channels: name, slider, number box
  ui.BeginGroup()
  local boxW = 44 * s
  if ui.BeginTable("##channels", 3, ui.TableFlags.SizingFixedFit, 0, 0) then
    ui.TableSetupColumn("##name", ui.TableColumnFlags.WidthFixed, ui.CalcTextSize("Green") + 4 * s)
    ui.TableSetupColumn("##slider", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("##box", ui.TableColumnFlags.WidthFixed, boxW)
    for _, ch in ipairs(CHANNELS) do
      local c = ch[4]
      ui.TableNextRow()
      ui.TableNextColumn()
      local cx, cy = ui.GetCursorPos()
      ui.SetCursorPos(cx, cy + 3 * s)
      ui.TextColored(c[1], c[2], c[3], 1, ch[1])
      ui.TableNextColumn()
      ui.PushStyleColor(ui.Col.FrameBg, c[1], c[2], c[3], 0.16)
      ui.PushStyleColor(ui.Col.FrameBgHovered, c[1], c[2], c[3], 0.24)
      ui.PushStyleColor(ui.Col.FrameBgActive, c[1], c[2], c[3], 0.30)
      ui.PushStyleColor(ui.Col.SliderGrab, c[1], c[2], c[3], 0.90)
      ui.PushStyleColor(ui.Col.SliderGrabActive, c[1], c[2], c[3], 1)
      W.Slider(WINDOW, ch[2], { format = function() return "" end })
      ui.PopStyleColor(5)
      ui.TableNextColumn()
      W.Edit(WINDOW, ch[3], { max = 3 })
    end
    ui.EndTable()
  end
  ui.EndGroup()
  -- the presets
  ui.Dummy(0, 4 * s)
  K.Caption("Presets")
  local gap = 5 * s
  local sz = math.max(14 * s, math.min(24 * s, (ui.GetContentRegionAvail() - 14 * gap) / 15))
  local perRow = math.max(1, math.floor((ui.GetContentRegionAvail() + gap) / (sz + gap)))
  for i = 0, 14 do
    if i % perRow ~= 0 then ui.SameLine(0, gap) end
    local id = "CPW_TC" .. i .. "Button"
    local sr, sg, sb = K.Tint(WINDOW, id)
    if K.Swatch("##" .. id, sr, sg, sb, sz, s, { tooltip = K.Hex(sr, sg, sb),
        selected = K.Hex(sr, sg, sb) == hex }) then
      API.windows.ClickChild(WINDOW, id, "left")
    end
  end
  -- the buttons at the bottom
  local bh = K.ButtonH(s)
  local _, by0 = ui.GetCursorPos()
  local _, wh = ui.GetWindowSize()
  local by = wh - bh - 8 * s
  if by > by0 then ui.SetCursorPos(10 * s, by) end
  W.Button(WINDOW, "CPW_Accept_Button", "Accept", { primary = true, height = bh,
    width = (ui.GetContentRegionAvail() - 8) / 2, tooltip = "Use this color" })
  ui.SameLine()
  if ui.Button("Cancel##cpw", -1, bh) then API.windows.Show(WINDOW, false) end
end

ui.window("Color Picker", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Color", WINDOW_OPTS, drawWindow)
end)
