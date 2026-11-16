-- Potion belt: replaces the game's PotionBeltWnd with a borderless bar of five tiles (a row, or a column: the
-- right-click menu), each with how many of its item you carry. The tiles are the game's slots: a left click with an
-- item on the cursor puts it on the belt, a right click uses it, holding the right button shows it. A clicky's
-- recast shades its tile from below as it runs. A slot beyond the ones you may use, or one with nothing on it, is
-- an empty well.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local tiles = dofile("lib/hudx_tiles.lua")
local T = hud.T
local C = T.color
local WINDOW = "PotionBeltWnd"
local SLOTS = 5
local SIZE, GAP, PAD = 40, 4, 6

local function vertical() return ui.GetSetting("hud.vertical." .. WINDOW) == "1" end

local LONG = SLOTS * (SIZE + GAP) - GAP + PAD * 2
local ICON_OPTS, WELL_OPTS = { radius = 6 }, { radius = 6, wellAlpha = 0.5 }
-- each slot's ids, number and right-button handler, made once (not every frame)
local SLOT = {}
for i = 0, SLOTS - 1 do
  local id = "PotionSlot" .. i
  SLOT[i] = { id = id, label = "PotionSlotLabel" .. i, hit = "##" .. id, number = tostring(i + 1),
    -- the right button as the game takes it on a slot (down, held after 500 ms, up)
    onClick = function(phase, outside) API.windows.ClickChild(WINDOW, id, phase, outside) end }
end

local vert = false -- vertical(), read each frame the window is drawn
local OPTS = {
  size = function() if vert then return SIZE + PAD * 2, LONG end return LONG, SIZE + PAD * 2 end,
  menu = function()
    if ui.MenuItem("Vertical", vert) then ui.SetSetting("hud.vertical." .. WINDOW, vert and "0" or "1") end
  end,
}
local function body(x, y, w, h, s)
  local sz = SIZE * s
  for i = 0, SLOTS - 1 do
    local slot = SLOT[i]
    local id = slot.id
    local off = (PAD + i * (SIZE + GAP)) * s
    local bx, by = vert and x + PAD * s or x + off, vert and y + off or y + PAD * s
    local name = API.windows.ChildTooltip(WINDOW, id) or ""
    if name ~= "" then
      tiles.Icon(WINDOW, id, bx, by, sz, s, nil, ICON_OPTS)
      -- the slot's recast cool-down (the game's belt button): the part still to go, from below
      local top = API.spells.ButtonCoolDown(WINDOW, id, by, by + sz)
      if top then
        ui.DrawRectFilled(bx, top, bx + sz, by + sz, 0, 0, 0, 0.55, 6 * s)
        ui.DrawLine(bx, top, bx + sz, top, C.accent[1], C.accent[2], C.accent[3], 0.8, 1.5 * s)
      end
      local count = API.windows.Text(WINDOW, slot.label) or ""
      if count ~= "" then
        local col = count == "0" and C.danger or C.text
        T.Text(bx + sz - 3 * s, by + sz - 13 * s, count, col, T.font.tiny, "right")
      end
    else
      T.Icon(bx, by, sz, s, nil, 0, 0, 1, 1, nil, WELL_OPTS)
    end
    T.Text(bx + 4 * s, by + 2 * s, slot.number, C.faint, T.font.tiny)
    T.Hit(slot.hit, bx, by, bx + sz, by + sz)
    local key, hovered = ui.GetItemID(), ui.IsItemHovered()
    T.HoverFx(bx, by, bx + sz, by + sz, s, 6)
    if hovered and not ui.IsMouseDown(ui.Mouse.Right) then
      if name ~= "" then
        ui.SetTooltip(name .. "\nRight-click to use, hold the right button to inspect")
      else
        ui.SetTooltip("An empty belt slot: click it with a potion on the cursor to put it here")
      end
    end
    if ui.IsItemClicked(ui.Mouse.Left) then API.windows.ClickChild(WINDOW, id, "left") end
    local onClick = slot.onClick
    if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      T.RightPressStart(key, onClick)
      T.TakeRightClick()
    end
    T.RightPressFrame(key, onClick, hovered)
  end
end

ui.window("Potion Belt", function()
  if not API.session.InGame() then return end
  vert = vertical()
  hud.Window(WINDOW, "Potion Belt", OPTS, body)
end)
