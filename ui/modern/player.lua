-- Player frame: replaces the game's PlayerWindow. A level badge, the name, and the health, mana and endurance bars
-- (the game's own values: eq.Gauge / eq.Label by EQType), experience and alternate advancement experience (the
-- inventory's gauges 4 and 5) under them, the spell being cast, and while auto attack is on a pulsing red ring with a
-- halo, a red wash over the frame and a red level badge (the pet's health is the Pet window's). The cast bar replaces
-- the game's CastingWindow: shown while the game shows it, the spell's name over a bar that fills as the cast
-- completes (the casting gauge counts the time left down from 1).
local hud = dofile("lib/hud.lua")
local roles = dofile("lib/role_icons.lua")
local T = hud.T
local C = T.color
local WINDOW = "PlayerWindow"
local ROLE_COLORS = { C.mana, C.accent, C.endurance }
local CAST_H = 24 -- the cast bar's row (design pixels)

-- The classes' short names, for a multiclass server's list when the full names do not fit (a level title the
-- table does not know shows its first three letters).
local SHORT = {
  Warrior = "WAR", Cleric = "CLR", Paladin = "PAL", Ranger = "RNG", ["Shadow Knight"] = "SHD", Shadowknight = "SHD",
  Druid = "DRU", Monk = "MNK", Bard = "BRD", Rogue = "ROG", Shaman = "SHM", Necromancer = "NEC", Wizard = "WIZ",
  Magician = "MAG", Enchanter = "ENC", Beastlord = "BST", Berserker = "BER",
}

-- The class label (one class a line on a multiclass server) as a list, its names joined, their short names joined
-- and the hover text; kept while the label reads the same.
local classLabel, classList, classFull, classShort, classTip = nil, {}, "", "", ""
local function classes()
  local label = eq.Label(3) or ""
  if label ~= classLabel then
    classLabel = label
    local list = {}
    for line in (label .. "\n"):gmatch("(.-)\n") do
      for raw in line:gmatch("[^/,]+") do
        local part = raw:match("^%s*(.-)%s*$")
        if part ~= "" then list[#list + 1] = part end
      end
    end
    local short = {}
    for i, c in ipairs(list) do short[i] = SHORT[c] or c:sub(1, 3):upper() end
    classList, classFull, classShort = list, table.concat(list, " / "), table.concat(short, " / ")
    classTip = table.concat(list, "\n")
  end
  return classList, classFull, classShort, classTip
end

-- The health bar's "hp / max" text, kept while the two labels read the same.
local hpKey, maxHpKey, hpBarText = nil, nil, nil
local function hpText(hp, maxHp)
  if hp ~= hpKey or maxHp ~= maxHpKey then
    hpKey, maxHpKey = hp, maxHp
    hpBarText = (hp and maxHp and hp ~= "") and (T.Thousands(hp) .. " / " .. T.Thousands(maxHp)) or nil
  end
  return hpBarText
end
local PERCENT = {} -- "n%" by n
local function percent(n)
  local t = PERCENT[n]
  if not t then
    t = n .. "%"
    PERCENT[n] = t
  end
  return t
end

-- The combat state (the game's PW_CombatStateAnim: in combat, the combat timer, debuffed, resting, standing) as a pip
-- on the level badge with the game's tooltip, and while the combat timer runs (gauge 29: up to the last 30 s of
-- OP_RestState's downtime) a ring around the badge that empties with it and the time left (label 137) under it.
local STATE = {
  A_PWCSInCombat = "danger", A_PWCSTimer = "endurance", A_PWCSDebuff = "bad", A_PWCSRegen = "good",
  A_PWCSStanding = "muted",
}
local function combatState(cx, cy, r, s)
  local timer = eq.Gauge(29)
  if timer then
    local col = C.endurance
    local n = 48
    local steps = math.floor(n * timer + 0.5)
    local rr = r + 3 * s
    for k = 0, steps - 1 do -- clockwise from the top
      local a0, a1 = -math.pi / 2 + 2 * math.pi * k / n, -math.pi / 2 + 2 * math.pi * (k + 1) / n
      ui.DrawLine(cx + rr * math.cos(a0), cy + rr * math.sin(a0), cx + rr * math.cos(a1), cy + rr * math.sin(a1),
        col[1], col[2], col[3], 0.95, 2.5 * s)
    end
    local left = eq.Label(137) or ""
    if left ~= "" then T.Text(cx, cy + rr + 1 * s, left, col, T.font.tiny, "center") end
  end
  local art = eq.ChildArt(WINDOW, "PW_CombatStateAnim", "decal")
  local tok = type(art) == "string" and STATE[art]
  if not tok then return end
  local col = C[tok] or C.muted
  local px, py, pr = cx + r * 0.72, cy + r * 0.72, 5 * s
  ui.DrawCircleFilled(px, py, pr + 1.5 * s, C.inset[1], C.inset[2], C.inset[3], 1)
  ui.DrawCircleFilled(px, py, pr, col[1], col[2], col[3], 1)
  if ui.IsMouseHoveringRect(px - pr, py - pr, px + pr, py + pr) and ui.IsWindowHovered() then
    local tip = eq.ChildTooltip(WINDOW, "PW_CombatStateAnim")
    if tip and tip ~= "" then ui.SetTooltip(tip) end
  end
end

-- What this frame's options and body read, set before hud.Window each frame (made once, not every frame).
local frame = { caster = false, casting = false, manaFrac = nil, attack = nil } -- attack: the pulse, 0..1, or nil
local GLOW = { 0, 0, 0, 0 }
local PANEL_OPTS = { glowWidth = 3 }
local NO_TRAIL = { noTrail = true }
local HP_BAR = {}
local NAME_COLOR = { 0, 0, 0, 1 }
local OPTS = {
  size = function() return 300, 104 + (frame.caster and 12 or 0) + (frame.casting and CAST_H or 0) end,
  panelOpts = PANEL_OPTS,
}

local function body(x, y, w, h, s)
  local caster, casting = frame.caster, frame.casting
  local pad = 10 * s
  local attack = frame.attack
  if attack then -- auto attack: a red wash over the frame, under its contents
    local d = C.danger
    ui.DrawRectFilled(x, y, x + w, y + h, d[1], d[2], d[3], 0.06 + 0.12 * attack, 7 * s)
  end
  -- the level badge (its ring red while auto attack is on)
  local r = 21 * s
  local cx, cy = x + pad + r, y + pad + r
  local ring = attack and C.danger or C.accent
  ui.DrawCircleFilled(cx, cy, r, C.inset[1], C.inset[2], C.inset[3], 0.9)
  ui.DrawCircle(cx, cy, r, ring[1], ring[2], ring[3], attack and 1 or 0.85, (attack and 3 or 2) * s)
  local level = eq.Label(2) or ""
  local _, lh = T.TextSize(level, T.font.large)
  T.Text(cx, cy - lh / 2, level, C.text, T.font.large, "center")
  combatState(cx, cy, r, s)

  local bx0, bx1 = cx + r + 10 * s, x + w - pad
  local hpFrac, hpLabel = eq.Gauge(1)
  local name = (hpLabel or eq.Name() or ""):match("^[^\n]*")
  local nameColor = C.text
  do
    local _, nr, ng, nb = eq.Label(1) -- green for a group leader
    if nr then
      NAME_COLOR[1], NAME_COLOR[2], NAME_COLOR[3] = nr, ng, nb
      nameColor = NAME_COLOR
    end
  end
  local _, nameH = T.TextSize("Ag", T.font.normal)
  local mask = roles.Mask(WINDOW)
  local iconSize, iconGap = 16 * s, 3 * s
  local iconW = roles.Width(mask, iconSize, iconGap)
  local fittedName = T.Fit(name, bx1 - bx0 - (iconW > 0 and iconW + 5 * s or 0), T.font.normal)
  local nameW = T.TextSize(fittedName, T.font.normal)
  local nameY = y + pad - 2 * s
  T.Text(bx0, nameY, fittedName, nameColor, T.font.normal)
  roles.Draw(mask, bx0 + nameW + 5 * s, nameY + (nameH - iconSize) / 2, iconSize, iconGap, ROLE_COLORS)
  -- a right click on the name: the group role menu (in a group); solo, or with Alt, the frame's own menu as anywhere
  -- else on the frame (the click is taken only when the role menu opened)
  if ui.IsWindowHovered() and ui.IsMouseHoveringRect(bx0, nameY, bx1, nameY + nameH)
      and not ui.GetIO().alt and ui.IsMouseClicked(ui.Mouse.Right) then
    if eq.ClickChild(WINDOW, "PlayerHP", "right") then T.TakeRightClick() end
  end
  -- the classes on their own line: the full names when they fit, else the short ones, the full list on hover
  local list, full, short, tip = classes()
  local text = full
  if T.TextSize(full, T.font.small) > bx1 - bx0 then
    text = T.Fit(short, bx1 - bx0, T.font.small)
  end
  local cy0 = y + pad + nameH - 3 * s
  local _, ch = T.TextSize("Ag", T.font.small)
  T.Text(bx0, cy0, text, C.muted, T.font.small)
  if #list > 0 and ui.IsMouseHoveringRect(bx0, cy0, bx1, cy0 + ch) and ui.IsWindowHovered() then
    ui.SetTooltip(tip)
  end

  local by = cy0 + ch + 3 * s
  local hp, maxHp = eq.Label(17), eq.Label(18)
  local pct = math.floor((hpFrac or 0) * 100 + 0.5)
  local hpColor = (hpFrac or 1) < 0.25 and C.hpLow or C.hp
  HP_BAR.text, HP_BAR.right, HP_BAR.font = hpText(hp, maxHp), percent(pct), T.font.small
  T.Bar("player.hp", bx0, by, bx1, by + 16 * s, s, hpFrac, hpColor, HP_BAR)
  if T.Hit("##hp", bx0, by, bx1, by + 16 * s) then eq.ClickChild(WINDOW, "PlayerHP", "left") end -- target yourself
  by = by + 19 * s
  if caster then
    local mf = frame.manaFrac -- (eq.Gauge(2), read once this frame)
    local mana, maxMana = eq.Label(124), eq.Label(125)
    T.Bar("player.mana", bx0, by, bx1, by + 9 * s, s, mf, C.mana)
    T.Hit("##mana", bx0, by, bx1, by + 9 * s)
    if ui.IsItemHovered() and mana then ui.SetTooltip("Mana " .. T.Thousands(mana) .. " / " .. T.Thousands(maxMana)) end
    by = by + 12 * s
  end
  local ef = eq.Gauge(3)
  T.Bar("player.end", bx0, by, bx1, by + 6 * s, s, ef, C.endurance)
  T.Hit("##end", bx0, by - 1 * s, bx1, by + 7 * s)
  if ui.IsItemHovered() then
    local e, maxE = eq.Label(126), eq.Label(127)
    if e then ui.SetTooltip("Endurance " .. T.Thousands(e) .. " / " .. T.Thousands(maxE)) end
  end
  -- experience and AA experience, across the frame
  by = by + 12 * s
  do
    local xp, aa = eq.Gauge(4) or 0, eq.Gauge(5) or 0
    local x0, x1 = x + pad, x + w - pad
    local split = x0 + (x1 - x0) * 0.64
    local _, th = T.TextSize("Ag", T.font.tiny)
    T.Text(x0, by, "XP", C.muted, T.font.tiny)
    T.Text(split - 6 * s, by, string.format("%.1f%%", xp * 100), C.text, T.font.tiny, "right")
    T.Text(split + 6 * s, by, "AA", C.muted, T.font.tiny)
    local banked = eq.Label(71) or "0" -- unspent AA points
    local aw = T.TextSize("AA ", T.font.tiny)
    T.Text(split + 6 * s + aw, by, banked, C.aa, T.font.tiny)
    T.Text(x1, by, string.format("%.1f%%", aa * 100), C.text, T.font.tiny, "right")
    local yb = by + th + 2 * s
    T.Bar("player.xp", x0, yb, split - 6 * s, yb + 5 * s, s, xp, C.xp, NO_TRAIL)
    for k = 1, 9 do -- a tick every tenth
      local tx = x0 + (split - 6 * s - x0) * k / 10
      ui.DrawLine(tx, yb + 1, tx, yb + 5 * s - 1, 0, 0, 0, 0.5, 1)
    end
    T.Bar("player.aa", split + 6 * s, yb, x1, yb + 5 * s, s, aa, C.aa, NO_TRAIL)
    T.Hit("##xp", x0, by, x1, yb + 6 * s)
    if ui.IsItemHovered() then
      ui.BeginTooltip()
      ui.Text("Level " .. level)
      ui.TextColored(C.xp[1], C.xp[2], C.xp[3], 1, string.format("Experience  %.2f%%", xp * 100))
      ui.TextColored(C.aa[1], C.aa[2], C.aa[3], 1,
        string.format("AA experience  %.2f%%  (%s unspent)", aa * 100, banked))
      ui.EndTooltip()
    end
  end
  if casting then
    local left, _, raw = eq.Gauge(7)
    local done = (left and raw ~= -1) and (1 - left) or 0
    local x0, x1 = x + pad, x + w - pad
    local cy = y + h - pad - (CAST_H - 2) * s
    local _, th = T.TextSize("Ag", T.font.tiny)
    T.Text(x0, cy, T.Fit(eq.Label(134) or "", x1 - x0, T.font.tiny), C.text, T.font.tiny)
    T.Bar("player.cast", x0, cy + th + 2 * s, x1, cy + th + 9 * s, s, done, C.cast, NO_TRAIL)
  end
end

ui.window("Player", function()
  if not eq.InGame() then return end
  local manaFrac, _, manaRaw = eq.Gauge(2)
  frame.caster, frame.manaFrac = manaRaw ~= -1, manaFrac
  frame.casting = eq.WindowShown("CastingWindow")
  local attacking = eq.ChildShown(WINDOW, "A_AttackIndicatorAnimTop") == true
  local glow, attack = nil, nil
  if attacking then
    attack = math.abs(math.sin(ui.GetTime() * 4))
    GLOW[1], GLOW[2], GLOW[3], GLOW[4] = C.danger[1], C.danger[2], C.danger[3], 0.55 + 0.45 * attack
    glow = GLOW
  end
  PANEL_OPTS.glow, frame.attack = glow, attack
  hud.Window(WINDOW, "Player", OPTS, body)
end)
