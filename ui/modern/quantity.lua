-- Quantity: replaces the game's QuantityWnd (how many to pick up, buy, sell, split...). A number box and a slider
-- that follow each other, quick picks (1, half, all), Accept and Cancel. It opens where the game opens it (by the
-- slot or button that asked), each time.
-- The slider, the box and Accept are the original's (hs.SetSlider / hs.SetWindowText / hs.ClickChild on
-- QTYW_Slider, QTYW_SliderInput, QTYW_Accept_Button): Enter in the box accepts as Accept does, closing the window
-- cancels (CQuantityWnd).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local WINDOW = "QuantityWnd"

local openX, openY = nil, nil -- where the game put the dialog when it last opened
local base = 0                -- the slider's minimum (the box shows slider + minimum)
local wasShown = false
local focusBox, boxActive = false, false
-- Each opening's own id for the box: ImGui keeps a focused box's text as typed, so the box of a dialog opened again for
-- another request (Sell while Buy's was still up) would go on showing the old amount over the new request's.
local opening = 0

-- the quick picks (label, tooltip); their slider values are worked out each frame from the minimum and the count
local PICKS = { { "1##q", "One" }, { "Half##q", "Half of them" }, { "All##q", "All of them" } }
local sliderMax = 0 -- the largest amount, for the slider's text
local SLIDER_OPTS = { format = function(v) return tostring(v + base) .. " of " .. sliderMax end }

local WINDOW_OPTS = {
  rollup = false, -- (a question to answer: kept whole)
  size = function() return 250, 118 end,
  resizable = { 200, 110 },
  place = function(W_, H, w, h, s, x, y) return openX or x, openY or y end,
}
local function drawWindow(x, y, w, h, s)
  local _, _, value, count = API.windows.ChildState(WINDOW, "QTYW_Slider")
  value, count = value or 0, count or 1
  local max = base + count - 1
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  -- the number, focused (and selected) when the dialog opens: type and press Enter
  local bw = 46 * s
  if focusBox then
    ui.SetKeyboardFocusHere()
    focusBox = false
  end
  ui.PushID(opening)
  W.Edit(WINDOW, "QTYW_SliderInput", { hint = "Amount", width = -(3 * (bw + 6 * s)) })
  ui.PopID()
  local active = ui.IsItemActive()
  if ui.IsItemHovered() then ui.SetTooltip(string.format("%d to %d; Enter accepts", base, max)) end
  if boxActive and ui.IsKeyPressed(ui.Key.Enter) then
    boxActive = false
    API.windows.Button(WINDOW, "QTYW_SliderInput")
    return
  end
  boxActive = active
  for i, q in ipairs(PICKS) do
    local to = i == 1 and 1 - base or (i == 2 and (count - 1) // 2 or count - 1)
    ui.SameLine(0, 6 * s)
    if ui.Button(q[1], bw, 0) then API.windows.SetSlider(WINDOW, "QTYW_Slider", math.max(0, math.min(count - 1, to))) end
    if ui.IsItemHovered() then ui.SetTooltip(q[2]) end
  end
  -- the slider
  sliderMax = max
  W.Slider(WINDOW, "QTYW_Slider", SLIDER_OPTS)
  ui.Dummy(0, 2 * s)
  -- Accept, and Cancel (closing the window cancels, as the original's close box does)
  local half = (ui.GetContentRegionAvail() - 8) / 2
  W.Button(WINDOW, "QTYW_Accept_Button", "Accept", { primary = true, width = half, height = btnH })
  ui.SameLine()
  if ui.Button("Cancel##qty", half, btnH) then API.windows.Show(WINDOW, false) end
end

ui.window("Quantity", function()
  if not API.session.InGame() then return end
  -- The game moves the dialog to its caller each time it opens (hs.ReplaceWindow's `place`): forget the place it
  -- was dragged to, so it opens there.
  local shown, sx, sy, _, _, _, place = API.windows.Replace(WINDOW)
  if place then
    openX, openY = sx, sy
    ui.SetSetting("hud.pos." .. WINDOW, "")
  end
  if shown and (not wasShown or place) then
    -- the box starts at the slider's value plus the minimum (read again when the game opens it for another caller
    -- while it is still up: that one's minimum may differ)
    local _, _, value = API.windows.ChildState(WINDOW, "QTYW_Slider")
    base = (tonumber(W.Text(WINDOW, "QTYW_SliderInput")) or 0) - (value or 0)
    focusBox, boxActive = true, false
    opening = opening + 1
  end
  wasShown = shown
  hud.Dialog(WINDOW, "Quantity", WINDOW_OPTS, drawWindow)
end)
