-- Item preview: replaces the game's ObjectPreviewWnd (an Alt + right click on an item opens it). A card with the
-- client's 3D view of the item on your character, or of the item alone (hs.ItemPreviewImage): drag with the left
-- button to turn it, with the right button to move it, the wheel zooms; the row below has the game's own turn, zoom
-- and Reset buttons (OPW_*). An item with nothing to show says so, as the game does.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color
local WINDOW = "ObjectPreviewWnd"
local lastSerial = nil

local BUTTONS = {
  { "OPW_RotateLeftButton", "<", "Turn left" }, { "OPW_RotateRightButton", ">", "Turn right" },
  { "OPW_RotateUpButton", "^", "Tilt up" }, { "OPW_RotateDownButton", "v", "Tilt down" },
  { "OPW_ZoomInButton", "+", "Zoom in" }, { "OPW_ZoomOutButton", "-", "Zoom out" },
  { "OPW_ResetButton", "Reset", "Fit the view again" },
}

local preview = nil -- this frame's hs.ItemPreview() (the body reads it)
local WINDOW_OPTS = {
  size = function() return 300, 380 end,
  resizable = { 200, 240 },
  place = function(sw, sh, w, h) return sw / 2 + 120, sh / 2 - h / 2 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local top = y
  local btnH = lh + 8 * s
  local vh = math.max(h - (top - y) - btnH - 8 * s, 16)
  local p = preview
  if p and p.show then
    local tex = API.preview.Image(w, vh)
    ui.DrawRectFilled(x, top, x + w, top + vh, 0, 0, 0, 0.25, 6 * s)
    if tex then ui.DrawImage(tex, x, top, x + w, top + vh, 0, 0, 1, 1) end
    ui.SetCursorScreenPos(x, top)
    ui.InvisibleButton("##previewview", w, vh, ui.ButtonFlags.MouseButtonLeft + ui.ButtonFlags.MouseButtonRight)
    local io = ui.GetIO()
    if ui.IsItemHovered() then
      T.TakeRightClick()
      if io.wheel ~= 0 then API.preview.Control("zoom", io.wheel > 0 and 0.9 or 1.1) end
    end
    if ui.IsItemActive() then
      if ui.IsMouseDown(ui.Mouse.Left) then
        API.preview.Control("rotate", io.mouseDX * 0.01, -io.mouseDY * 0.01)
      elseif ui.IsMouseDown(ui.Mouse.Right) then
        API.preview.Control("pan", -io.mouseDX / w, io.mouseDY / vh)
      end
    end
    ui.SetCursorScreenPos(x, top + vh + 6 * s)
    local unit = (w - 6 * (#BUTTONS - 1) * s) / (#BUTTONS + 1) -- Reset takes two
    for i, b in ipairs(BUTTONS) do
      if i > 1 then ui.SameLine(0, 6 * s) end
      W.Button(WINDOW, b[1], b[2], { width = i == #BUTTONS and unit * 2 or unit, height = btnH, tooltip = b[3] })
    end
  else
    local msg = W.Text(WINDOW, "OPW_NoItemPreviewAvailable_Label")
    if msg == "" then msg = "No item preview available." end
    local tw = ui.CalcTextSize(msg)
    T.Text(x + (w - tw) / 2, top + vh / 2, msg, C.faint, T.font.small)
  end
end

ui.window("Item Preview", function()
  if not API.session.InGame() then return end
  preview = API.preview.Draw()
  local p = preview
  if p and p.serial ~= lastSerial then -- a new answer: to the front (BringWndToTop)
    lastSerial = p.serial
    ui.SetNextWindowFocus()
  end
  hud.Dialog(WINDOW, "Item Preview", WINDOW_OPTS, drawWindow)
end)
