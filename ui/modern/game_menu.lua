-- Menu button: replaces the game's EQMainWnd (the EQ button). A round button at the bottom right; a click opens or
-- closes the game menu (drawn by menus.lua, which opens it from this button: modern.menuAnchor).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "EQMainWnd"

local WINDOW_OPTS = {
  size = function() return 44, 44 end,
  panelOpts = { radius = 22 },
}
local function drawWindow(x, y, w, h, s)
  if modern then modern.menuAnchor = { x, y, x + w, y + h, s = s } end
  local _, open = API.windows.ChildState(WINDOW, "EQButton")
  local clicked = T.Hit("##menu", x, y, x + w, y + h)
  if hud.DragItem() then clicked = false end -- a drag moves the frame
  local hovered = ui.IsItemHovered()
  local c = (open or hovered) and C.accent or C.text
  if open then ui.DrawCircleFilled(x + w / 2, y + h / 2, w / 2 - 2 * s, c[1], c[2], c[3], 0.18) end
  local lw, gap = 16 * s, 5 * s
  for k = -1, 1 do
    local ly = y + h / 2 + k * gap
    ui.DrawLine(x + w / 2 - lw / 2, ly, x + w / 2 + lw / 2, ly, c[1], c[2], c[3], 1, 2 * s)
  end
  if hovered and not open then ui.SetTooltip("Menu") end
  if clicked then API.windows.ClickChild(WINDOW, "EQButton", "left") end
end

ui.window("EQ Button", function()
  if not API.session.InGame() then return end
  hud.Window(WINDOW, "Menu", WINDOW_OPTS, drawWindow)
end)
