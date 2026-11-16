-- Auras: replaces the game's AuraWindow (a window the player opens and closes: the EQ menu's Auras, its key, the X
-- in the corner; the state is kept as the stock window's). A borderless frame with each aura's icon and name; a
-- right click on one removes it (the game's list takes the click). Its height follows the number of auras; its
-- scale is the right-click menu's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "AuraWindow"
local LIST = "AuraList"
local ROW, PAD, HEAD = 26, 6, 16

local RADIUS = { radius = 4 }
-- The number of auras, read by size() (hud.Window calls it only while the window shows, before the body), so a
-- closed window reads nothing.
local rows = 0
local OPTS = {
  size = function()
    rows = API.windows.ListInfo(WINDOW, LIST) or 0
    return 190, HEAD + PAD * 2 + math.max(rows, 1) * ROW
  end,
  onClose = function() API.windows.Show(WINDOW, false) end,
}
local function body(x, y, w, h, s)
  T.Text(x + PAD * s + 2 * s, y + 4 * s, "AURAS", C.faint, T.font.tiny)
  local ry = y + (HEAD + PAD - 2) * s
  if rows == 0 then
    T.Text(x + PAD * s + 2 * s, ry + 5 * s, "No auras", C.faint, T.font.small)
    return
  end
  local isz = (ROW - 4) * s
  for r = 0, rows - 1 do
    local texts, _, _, _, tex, u0, v0, u1, v1 = API.windows.ListRow(WINDOW, LIST, r)
    local name = texts and texts[1] or ""
    local rx0, rx1 = x + PAD * s, x + w - PAD * s
    local y0 = ry + r * ROW * s
    T.Hit("##aura" .. r, rx0, y0, rx1, y0 + ROW * s)
    local hov = ui.IsItemHovered()
    if hov then ui.DrawRectFilled(rx0, y0, rx1, y0 + ROW * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 5 * s) end
    T.Icon(rx0 + 2 * s, y0 + 2 * s, isz, s, tex, u0, v0, u1, v1, C.good, RADIUS)
    local _, th = T.TextSize("Ag", T.font.small)
    local tx = rx0 + isz + 9 * s
    T.Text(tx, y0 + (ROW * s - th) / 2, T.Fit(name, rx1 - tx - 4 * s, T.font.small), C.text, T.font.small)
    if hov then ui.SetTooltip(name .. "\nRight-click to remove it") end
    if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      API.windows.ListClick(WINDOW, LIST, r, "right")
      T.TakeRightClick()
    end
  end
end

ui.window("Auras", function()
  if not API.session.InGame() then return end
  hud.Window(WINDOW, "Auras", OPTS, body)
end)
