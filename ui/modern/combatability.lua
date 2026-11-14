-- Combat abilities: replaces the game's CombatAbilityWnd (the combat ability key; the melee and hybrid classes).
-- The active discipline at the top (its icon, a click on it ends the discipline, and the time it has left), "Skills"
-- for the list of every combat ability (CombatSkillSelectWnd), and the eight ability buttons as tiles that reflow
-- to the width: each shows its ability and its reuse time shaded, a click uses it (or, holding an ability on the
-- cursor, puts that one on the button), a right click picks the ability from the game's menu, a drag takes it off
-- onto the cursor (for a hot bar). Everything is the window's (app/lua_eq_abilities.cpp: eq.ClickChild,
-- eq.ButtonCoolDown, eq.PickUp), so the abilities and timers are the game's.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "CombatAbilityWnd"
local DRAW_KEY = "test.draw." .. WINDOW -- testing: draws the window though the game has not opened it
local BUTTONS = 8

local function cooldownTime(ms)
  local sec = math.ceil(ms / 1000)
  if sec >= 3600 then return string.format("%d:%02d:%02d", sec // 3600, (sec // 60) % 60, sec % 60) end
  return string.format("%d:%02d", sec // 60, sec % 60)
end

-- One ability button as a tile at the cursor (w x h): the click, right click and drag go to the window's button.
local function abilityTile(n, w, h, s)
  local id = "CAW_Button" .. n
  local name = W.Text(WINDOW, id)
  local empty = name == ""
  local x, y = ui.GetCursorScreenPos()
  local _, _, _, _, _, left, reuse = eq.ButtonCoolDown(WINDOW, id, y, y + h)
  local cooling = not empty and (left or 0) > 0
  local fraction = cooling and math.min(1, left / math.max(left, reuse or 0, 1)) or 0
  local clicked = T.Hit("##caw" .. n, x, y, x + w, y + h)
  local hovered, active = ui.IsItemHovered(), ui.IsItemActive()
  if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
    eq.ClickChild(WINDOW, id, "rightup") -- the game's ability menu, at this tile
    T.TakeRightClick()
  end
  local dragged = not empty and T.DragFrom(WINDOW, id)
  local rad = 6 * s
  local base = C.ink
  local fill = empty and 0.02 or 0.06
  if active then fill = fill + 0.10 elseif hovered then fill = fill + 0.06 end
  ui.DrawRectFilled(x, y, x + w, y + h, base[1], base[2], base[3], fill, rad)
  if cooling then
    ui.DrawRectFilled(x, y, x + w, y + h, C.danger[1], C.danger[2], C.danger[3], 0.08, rad)
    ui.DrawRectFilled(x, y, x + w * fraction, y + h, C.danger[1], C.danger[2], C.danger[3], 0.20, rad)
  end
  local edge = hovered and C.accent or (cooling and C.danger or C.ink)
  ui.DrawRect(x, y, x + w, y + h, edge[1], edge[2], edge[3], hovered and 0.6 or (cooling and 0.45 or 0.14), rad, hovered and 1.5 or 1)
  T.Text(x + 6 * s, y + 4 * s, tostring(n), C.faint, T.font.tiny)
  local font = T.font.small
  local _, th = T.TextSize("Ag", font)
  local label = empty and "Empty" or T.Fit(name, w - (cooling and 24 or 12) * s, font)
  if cooling then
    local _, timerH = T.TextSize("Ag", T.font.tiny)
    local top = y + math.max(s, (h - th - timerH - 2 * s) / 2)
    T.Text(x + w / 2 + 4 * s, top, label, C.muted, font, "center")
    T.Text(x + w / 2, top + th + 2 * s, cooldownTime(left), C.danger, T.font.tiny, "center")
  else
    T.Text(x + w / 2, y + (h - th) / 2, label, empty and C.faint or C.text, font, "center")
  end
  if hovered and not dragged then
    if empty then
      ui.SetTooltip("An empty button\nRight-click to choose an ability, or drop one here from Combat Skills")
    else
      ui.SetTooltip(name .. (cooling and ("\nReady in " .. cooldownTime(left)) or "") ..
        "\nClick to use it; right-click to choose another\nDrag onto an action bar to make a hot button")
    end
  end
  if clicked and not dragged then eq.ClickChild(WINDOW, id, "left") end
end

local function testOpen() eq.ShowWindow(WINDOW, true) end
local OPTS = {
  size = function() return 360, 190 end,
  resizable = { 200, 150 },
}
local function body(x, y, w, h, s)
  -- the active discipline
  local icon = 32 * s
  local active = eq.ChildShown(WINDOW, "CAW_CombatEffectButton") ~= false
  local ix, iy = ui.GetCursorScreenPos()
  local tex, u0, v0, u1, v1 = eq.ChildArt(WINDOW, "CAW_CombatEffectButton", "decal")
  if type(tex) ~= "userdata" then tex = nil end
  T.Icon(ix, iy, icon, s, active and tex or nil, u0, v0, u1, v1, active and C.accent or nil)
  if active then
    if T.Hit("##cawEffect", ix, iy, ix + icon, iy + icon) then eq.ClickChild(WINDOW, "CAW_CombatEffectButton", "left") end
    T.HoverFx(ix, iy, ix + icon, iy + icon, s)
    if ui.IsItemHovered() then ui.SetTooltip("Click to end this discipline") end
  end
  local skillsW = 64 * s
  local tx = ix + icon + 8 * s
  local textW = w - (tx - x) - skillsW - 8 * s
  local _, th = T.TextSize("Ag", T.font.small)
  local effect = W.Text(WINDOW, "CAW_CombatEffectLabel")
  local fraction, remaining
  if active then fraction, remaining = eq.Gauge(26) end
  local timerW = remaining and remaining ~= "" and (T.TextSize(remaining, T.font.small) + 8 * s) or 0
  local nameW = math.max(0, textW - timerW)
  local name = nameW >= T.TextSize("...", T.font.small) and T.Fit(effect, nameW, T.font.small) or ""
  T.Text(tx, iy + (active and 0 or (icon - th) / 2), name,
    active and C.text or C.muted, T.font.small)
  if active then
    T.Text(tx + textW, iy, remaining or "", C.muted, T.font.small, "right")
    T.Bar("cawEffectTime", tx, iy + th + 6 * s, tx + textW, iy + th + 14 * s, s,
      fraction or 0, C.accent, { noTrail = true })
  end
  ui.SetCursorScreenPos(x + w - skillsW, iy + (icon - (th + 10 * s)) / 2)
  W.Button(WINDOW, "CAW_OpenCombatSkillSelectWnd", "Skills", { width = skillsW,
    tooltip = "Every combat ability you know: drag one onto a button here or onto a hot bar" })
  ui.SetCursorScreenPos(x, iy + icon + 8 * s)
  -- the eight buttons
  local gap = 6 * s
  local cols = math.max(1, math.min(BUTTONS, math.floor((w + gap) / (82 * s + gap))))
  if cols > 4 and cols < 8 then cols = 4 end -- whole rows: 4 x 2 (or 8 x 1 when wide)
  local tw = math.floor((w - gap * (cols - 1)) / cols)
  local rows = math.ceil(BUTTONS / cols)
  local _, ah = ui.GetContentRegionAvail()
  local tileH = math.max(30 * s, math.min(56 * s, (ah - gap * (rows - 1)) / rows))
  local gx, gy = ui.GetCursorScreenPos()
  for n = 1, BUTTONS do
    local col, row = (n - 1) % cols, (n - 1) // cols
    ui.SetCursorScreenPos(gx + col * (tw + gap), gy + row * (tileH + gap))
    abilityTile(n, tw, tileH, s)
  end
  ui.SetCursorScreenPos(gx, gy)
  ui.Dummy(w, rows * tileH + (rows - 1) * gap)
end

ui.window("Combat Abilities", function()
  if not eq.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  OPTS.shown = T.Setting(DRAW_KEY) == "1" or nil
  hud.Dialog(WINDOW, "Combat Abilities", OPTS, body)
end)
