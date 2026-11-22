-- Player frame: replaces the game's PlayerWindow. A level badge, the name, and the health, mana and endurance bars
-- (the game's own values: hs.Gauge / hs.Label by EQType), experience and alternate advancement experience (the
-- inventory's gauges 4 and 5) under them, the spell being cast, and while auto attack is on a pulsing red ring with a
-- halo, a red wash over the frame and a red level badge (the pet's health is the Pet window's). The cast bar replaces
-- the game's CastingWindow: the spell's name over a bar that fills as the cast completes (the casting gauge
-- counts the time left down from 1). The frame menu can keep the empty bar visible between casts.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local roles = dofile("lib/role_icons.lua")
local T = hud.T
local C = T.color
local WINDOW = "PlayerWindow"
local ROLE_COLORS = { C.mana, C.accent, C.endurance }
local CAST_H = 24 -- the cast bar's row (design pixels)
local CAST_BAR_SETTING = "player.alwaysShowCastBar"
local MANA_READOUT = "player.manaReadout"
local ENDURANCE_READOUT = "player.enduranceReadout"
local MANA_H, ENDURANCE_H, READOUT_H = 9, 6, 16
local READOUT_CHOICES = {
  { "none", "None" }, { "percent", "Percentage" }, { "value", "Value" }, { "both", "Both" },
}
local READOUT_HEIGHT = { percent = READOUT_H, value = READOUT_H, both = READOUT_H }

local function readoutMenu(label, key, mode)
  if ui.BeginMenu(label) then
    for _, choice in ipairs(READOUT_CHOICES) do
      if ui.MenuItem(choice[2], mode == choice[1]) then ui.SetSetting(key, choice[1]) end
    end
    ui.EndMenu()
  end
end

-- The classes' short names, for a multiclass server's list when the full names do not fit (a level title the
-- table does not know shows its first three letters).
local SHORT = {
  Warrior = "WAR", Cleric = "CLR", Paladin = "PAL", Ranger = "RNG", ["Shadow Knight"] = "SHD", Shadowknight = "SHD",
  Druid = "DRU", Monk = "MNK", Bard = "BRD", Rogue = "ROG", Shaman = "SHM", Necromancer = "NEC", Wizard = "WIZ",
  Magician = "MAG", Enchanter = "ENC", Beastlord = "BST", Berserker = "BER",
}
-- A class's short name: the server's where its plugin named the class (API.character.ClassName), else the table's.
local HAS_CLASS_NAMES = API.Supports and API.Supports("character", "ClassName") == true
local function shortName(c)
  if HAS_CLASS_NAMES then
    for id = 1, 16 do
      if API.character.ClassName(id) == c then return API.character.ClassName(id, true) end
    end
  end
  return SHORT[c] or c:sub(1, 3):upper()
end

-- The class label (one class a line on a multiclass server) as a list, its names joined, their short names joined
-- and the hover text; kept while the label reads the same.
local classLabel, classList, classFull, classShort, classTip = nil, {}, "", "", ""
local function classes()
  local label = API.character.Label(3) or ""
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
    for i, c in ipairs(list) do short[i] = shortName(c) end
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

local function readoutMode(key)
  local mode = T.Setting(key)
  return READOUT_HEIGHT[mode] and mode or "none"
end

-- "current / max" for a pair of labels, made again only when they change (drawn every frame, as hpText is)
local VALUE_CACHE = {} -- by the current label's id: { current, max, text }
local function resourceValue(currentId, maxId)
  local current, max = API.character.Label(currentId), API.character.Label(maxId)
  local c = VALUE_CACHE[currentId]
  if not c then
    c = {}
    VALUE_CACHE[currentId] = c
  end
  if c[1] ~= current or c[2] ~= max then
    c[1], c[2] = current, max
    c[3] = (current and max and current ~= "" and max ~= "") and (T.Thousands(current) .. " / " .. T.Thousands(max))
      or nil
  end
  return c[3]
end

local function resourceReadout(opts, mode, fraction, currentId, maxId)
  opts.text = (mode == "value" or mode == "both") and resourceValue(currentId, maxId) or nil
  opts.right = (mode == "percent" or mode == "both") and fraction and percent(math.floor(fraction * 100 + 0.5)) or nil
end

-- The combat state (the game's PW_CombatStateAnim: in combat, the combat timer, debuffed, resting, standing) as a pip
-- on the level badge with the game's tooltip, and while the combat timer runs (gauge 29: up to the last 30 s of
-- OP_RestState's downtime) a ring around the badge that empties with it and the time left (label 137) under it.
local STATE = {
  A_PWCSInCombat = "danger", A_PWCSTimer = "endurance", A_PWCSDebuff = "bad", A_PWCSRegen = "good",
  A_PWCSStanding = "muted",
}
local function combatState(cx, cy, r, s)
  local timer = API.character.Gauge(29)
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
    local left = API.character.Label(137) or ""
    if left ~= "" then T.Text(cx, cy + rr + 1 * s, left, col, T.font.tiny, "center") end
  end
  local art = API.windows.ChildArt(WINDOW, "PW_CombatStateAnim", "decal")
  local tok = type(art) == "string" and STATE[art]
  if not tok then return end
  local col = C[tok] or C.muted
  local px, py, pr = cx + r * 0.72, cy + r * 0.72, 5 * s
  ui.DrawCircleFilled(px, py, pr + 1.5 * s, C.inset[1], C.inset[2], C.inset[3], 1)
  ui.DrawCircleFilled(px, py, pr, col[1], col[2], col[3], 1)
  if ui.IsMouseHoveringRect(px - pr, py - pr, px + pr, py + pr) and ui.IsWindowHovered() then
    local tip = API.windows.ChildTooltip(WINDOW, "PW_CombatStateAnim")
    if tip and tip ~= "" then ui.SetTooltip(tip) end
  end
end

-- What this frame's options and body read, set before hud.Window each frame (made once, not every frame).
local frame = { caster = false, casting = false, showCastBar = false, manaFrac = nil, attack = nil }
local GLOW = { 0, 0, 0, 0 }
local PANEL_OPTS = { glowWidth = 3 }
local NO_TRAIL = { noTrail = true }
local pctXp, pctXpText, pctAa, pctAaText = nil, "", nil, "" -- the XP and AA percentages as last drawn
local HP_BAR = {}
local MANA_BAR, ENDURANCE_BAR = {}, {}
local NAME_COLOR = { 0, 0, 0, 1 }
local OPTS = {
  size = function()
    local manaRow = frame.caster and frame.manaHeight + 3 or 0
    return 300, 104 - ENDURANCE_H + frame.enduranceHeight + manaRow + (frame.showCastBar and CAST_H or 0)
  end,
  panelOpts = PANEL_OPTS,
  menu = function()
    local always = T.Setting(CAST_BAR_SETTING) == "1"
    if ui.MenuItem("Always show cast bar", always) then ui.SetSetting(CAST_BAR_SETTING, always and "0" or "1") end
    readoutMenu("Mana readout", MANA_READOUT, frame.manaReadout)
    readoutMenu("Endurance readout", ENDURANCE_READOUT, frame.enduranceReadout)
  end,
}

local function body(x, y, w, h, s)
  local caster = frame.caster
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
  local level = API.character.Label(2) or ""
  local _, lh = T.TextSize(level, T.font.large)
  T.Text(cx, cy - lh / 2, level, C.text, T.font.large, "center")
  combatState(cx, cy, r, s)

  local bx0, bx1 = cx + r + 10 * s, x + w - pad
  local hpFrac, hpLabel = API.character.Gauge(1, true)
  local name = (hpLabel or API.character.Name() or ""):match("^[^\n]*")
  local nameColor = C.text
  do
    local _, nr, ng, nb = API.character.Label(1) -- green for a group leader
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
    if API.windows.ClickChild(WINDOW, "PlayerHP", "right") then T.TakeRightClick() end
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
  local hp, maxHp = API.character.Label(17), API.character.Label(18)
  local pct = math.floor((hpFrac or 0) * 100 + 0.5)
  local hpColor = (hpFrac or 1) < 0.25 and C.hpLow or C.hp
  HP_BAR.text, HP_BAR.right, HP_BAR.font = hpText(hp, maxHp), percent(pct), T.font.small
  T.Bar("player.hp", bx0, by, bx1, by + 16 * s, s, hpFrac, hpColor, HP_BAR)
  if T.Hit("##hp", bx0, by, bx1, by + 16 * s) then API.windows.ClickChild(WINDOW, "PlayerHP", "left") end -- target yourself
  by = by + 19 * s
  if caster then
    local mf = frame.manaFrac -- (hs.Gauge(2), read once this frame)
    local bottom = by + frame.manaHeight * s
    resourceReadout(MANA_BAR, frame.manaReadout, mf, 124, 125)
    T.Bar("player.mana", bx0, by, bx1, bottom, s, mf, C.mana, MANA_BAR)
    T.Hit("##mana", bx0, by, bx1, bottom)
    if ui.IsItemHovered() then
      local value = MANA_BAR.text or resourceValue(124, 125)
      if value then ui.SetTooltip("Mana " .. value) end
    end
    by = bottom + 3 * s
  end
  local ef = API.character.Gauge(3)
  local bottom = by + frame.enduranceHeight * s
  resourceReadout(ENDURANCE_BAR, frame.enduranceReadout, ef, 126, 127)
  T.Bar("player.end", bx0, by, bx1, bottom, s, ef, C.endurance, ENDURANCE_BAR)
  T.Hit("##end", bx0, by - 1 * s, bx1, bottom + s)
  if ui.IsItemHovered() then
    local value = ENDURANCE_BAR.text or resourceValue(126, 127)
    if value then ui.SetTooltip("Endurance " .. value) end
  end
  -- experience and AA experience, across the frame (their percentages made again only when the gauges move)
  by = bottom + 6 * s
  do
    local xp, aa = API.character.Gauge(4) or 0, API.character.Gauge(5) or 0
    local x0, x1 = x + pad, x + w - pad
    local split = x0 + (x1 - x0) * 0.64
    local _, th = T.TextSize("Ag", T.font.tiny)
    T.Text(x0, by, "XP", C.muted, T.font.tiny)
    if xp ~= pctXp then pctXp, pctXpText = xp, string.format("%.1f%%", xp * 100) end
    T.Text(split - 6 * s, by, pctXpText, C.text, T.font.tiny, "right")
    T.Text(split + 6 * s, by, "AA", C.muted, T.font.tiny)
    local banked = API.character.Label(71) or "0" -- unspent AA points
    local aw = T.TextSize("AA ", T.font.tiny)
    T.Text(split + 6 * s + aw, by, banked, C.aa, T.font.tiny)
    if aa ~= pctAa then pctAa, pctAaText = aa, string.format("%.1f%%", aa * 100) end
    T.Text(x1, by, pctAaText, C.text, T.font.tiny, "right")
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
  if frame.showCastBar then
    local x0, x1 = x + pad, x + w - pad
    local cy = y + h - pad - (CAST_H - 2) * s
    local _, th = T.TextSize("Ag", T.font.tiny)
    local done
    if frame.casting then
      local left, _, raw = API.character.Gauge(7)
      done = (left and raw ~= -1) and (1 - left) or 0
      T.Text(x0, cy, T.Fit(API.character.Label(134) or "", x1 - x0, T.font.tiny), C.text, T.font.tiny)
    end
    T.Bar("player.cast", x0, cy + th + 2 * s, x1, cy + th + 9 * s, s, done, C.cast, NO_TRAIL)
  end
end

ui.window("Player", function()
  if not API.session.InGame() then return end
  local manaFrac, _, manaRaw = API.character.Gauge(2)
  frame.caster, frame.manaFrac = manaRaw ~= -1, manaFrac
  frame.manaReadout, frame.enduranceReadout = readoutMode(MANA_READOUT), readoutMode(ENDURANCE_READOUT)
  frame.manaHeight = READOUT_HEIGHT[frame.manaReadout] or MANA_H
  frame.enduranceHeight = READOUT_HEIGHT[frame.enduranceReadout] or ENDURANCE_H
  frame.casting = API.windows.Shown("CastingWindow")
  frame.showCastBar = frame.casting or T.Setting(CAST_BAR_SETTING) == "1"
  local attacking = API.windows.ChildShown(WINDOW, "A_AttackIndicatorAnimTop") == true
  local glow, attack = nil, nil
  if attacking then
    attack = math.abs(math.sin(ui.GetTime() * 4))
    GLOW[1], GLOW[2], GLOW[3], GLOW[4] = C.danger[1], C.danger[2], C.danger[3], 0.55 + 0.45 * attack
    glow = GLOW
  end
  PANEL_OPTS.glow, frame.attack = glow, attack
  hud.Window(WINDOW, "Player", OPTS, body)
end)
