-- Map label: replaces the game's EditLabelWnd (a new map label, or the marked one edited, from the map toolbar). The
-- label's text, its size as three segments (the window's Small / Medium / Large buttons, the chosen one lit), the
-- colour button (it opens the colour picker on the label's colour), Cancel and Done. Every piece is the window's
-- child, so the label is made by the map's own handler.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "EditLabelWnd"

local SIZES = { { "ELW_LabelSmallButton", "Small" }, { "ELW_LabelMediumButton", "Medium" },
  { "ELW_LabelLargeButton", "Large" } }

local focus = true

local WINDOW_OPTS = {
  size = function() return 340, 168 end,
  resizable = { 260, 150 },
  place = K.Centered,
}
local function drawWindow(x, y, w, h, s)
  K.Caption("Label text")
  if focus then
    ui.SetKeyboardFocusHere()
    focus = false
  end
  W.Edit(WINDOW, "ELW_LabelTextEditbox", { hint = "Name this spot", max = 255 })
  if ui.IsItemHovered() then ui.SetTooltip("The text shown on the map") end
  ui.Dummy(0, 2 * s)
  K.Caption("Size")
  -- the sizes as one segmented row, then the colour button beside it
  local avail = ui.GetContentRegionAvail()
  local colorW = math.max(90 * s, avail * 0.32)
  local segW = (avail - colorW - 16 * s) / 3
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, 2 * s, 4 * s)
  for i, b in ipairs(SIZES) do
    if i > 1 then ui.SameLine() end
    W.Button(WINDOW, b[1], b[2], { width = segW, tooltip = b[2] .. " text" })
  end
  ui.PopStyleVar()
  ui.SameLine(0, 16 * s)
  W.Button(WINDOW, "ELW_ChooseColorButton", "Color...", { width = colorW, tooltip = "Choose the label's color" })
  -- the buttons at the bottom
  local bh = K.ButtonH(s)
  local _, cy = ui.GetCursorPos()
  local _, wh = ui.GetWindowSize()
  local by = wh - bh - 8 * s
  if by > cy then ui.SetCursorPos(10 * s, by) end
  W.ButtonRow(WINDOW, {
    { "ELW_DoneButton", "Done", { primary = true, tooltip = "Place the label" } },
    { "ELW_CancelButton", "Cancel" },
  }, bh)
end

ui.window("Edit Map Label", function()
  if not API.session.InGame() then return end
  if not API.windows.Shown(WINDOW) then focus = true return end
  hud.Dialog(WINDOW, "Map Label", WINDOW_OPTS, drawWindow)
end)
