-- Inventory: replaces the game's InventoryWindow. The worn slots as a grid with the weapons apart, the character's
-- numbers beside them (the game's own labels by EQType, in the colours the game gives them), the ten carried slots,
-- coins, and the game's buttons, with Advanced Inventory (Hailstone: inventorymanager.lua, where the search of
-- everything you have, Stack / Sort and the item marks are). The Stats page has every number the game gives (each attribute and resist as
-- total, base, heroic and cap; the item bonuses; regeneration and the pools without buffs: eq.CharacterStats), the
-- Bandolier page the game's BandolierWnd (Hailstone: the stock inventory has no such page; lib/bandolier.lua, as the
-- Bandolier window draws it), the Alt. Currency page the alternate currencies. Slots are the game's (lib/items.lua).
local hud = dofile("lib/hud.lua")
local items = dofile("lib/items.lua")
local tools = dofile("lib/itemtools.lua")
local band = dofile("lib/bandolier.lua")
local T = hud.T
local C = T.color
local WINDOW = "InventoryWindow"

-- Worn slots (eq.Item numbers), as rows of the grid.
local WORN = {
  { 1, 2, 3, 4, 5 },        -- ear, head, face, ear, neck
  { 6, 7, 8, 17, 20 },      -- shoulders, arms, back, chest, waist
  { 9, 10, 12, 15, 16 },    -- wrists, hands, fingers
  { 18, 19, 0, 21, 22 },    -- legs, feet, charm, power source, ammo
}
local WEAPONS = { 13, 14, 11 } -- primary, secondary, range
local SHORT = { -- names to show in an empty slot (eq.SlotName's are long)
  [0] = "Charm", [1] = "Ear", [2] = "Head", [3] = "Face", [4] = "Ear", [5] = "Neck", [6] = "Shoulder", [7] = "Arms",
  [8] = "Back", [9] = "Wrist", [10] = "Wrist", [11] = "Range", [12] = "Hands", [13] = "Primary", [14] = "Offhand",
  [15] = "Ring", [16] = "Ring", [17] = "Chest", [18] = "Legs", [19] = "Feet", [20] = "Waist", [21] = "Power",
  [22] = "Ammo",
}
local GENERAL_FIRST = 23

local SLOT, GAP = 46, 6
local LEFT_W = 5 * SLOT + 4 * GAP
local STATS_W = 220
local W = LEFT_W + 18 + STATS_W
local HEADER_H, TABS_H = 46, 28
local BODY_H = 4 * (SLOT + GAP) + 10 + SLOT + 14
local FOOT_H = SLOT + 12 + 34 + 2 * 32 + 2
local H = HEADER_H + TABS_H + BODY_H + FOOT_H

local page = "character" -- character, stats, bandolier, currency
-- the game's tab box pages (it fills some only while shown); the Bandolier page is Hailstone's, the game's Character
-- page stays up behind it
local GAME_PAGE = { character = 0, stats = 1, bandolier = 0, currency = 4 }
local showMissing = false

local function label(t) local text, r, g, b = eq.Label(t) return text or "", r and { r, g, b, 1 } or nil end

-- A label type the stock client has none of (eq.Label gives "Unknown\tLabel<n>" for it) is filled in only by a
-- server's own client (the NMS plugin's mitigation 6668 and avoidance 6667), so its row is drawn only when that
-- server's client gives a value; nil otherwise.
local function serverLabel(t)
  local v, col = label(t)
  if v == "" or v:find("^Unknown") then return nil end
  return v, col
end

-- A "name   value" row; value from EQType t (its colour if the game gives one).
local function statRow(x, y, w, name, t, s, font)
  font = font or T.font.small
  local v, col = label(t)
  T.Text(x, y, name, C.muted, font)
  T.Text(x + w, y, v, col or C.text, font, "right")
  local _, th = T.TextSize("Ag", font)
  return th + 3 * s
end

local function pool(x, y, w, s, name, cur, max, color)
  local c, m = tonumber((eq.Label(cur))) or 0, tonumber((eq.Label(max))) or 0
  T.Text(x, y, name, C.muted, T.font.small)
  T.Text(x + w, y, T.Thousands(c) .. " / " .. T.Thousands(m), C.text, T.font.small, "right")
  local _, th = T.TextSize("Ag", T.font.small)
  T.Bar("inv." .. name, x, y + th + 2 * s, x + w, y + th + 6 * s, s, m > 0 and c / m or 0, color, { noTrail = true })
  return th + 12 * s
end

local function characterPage(x, y, s)
  local sz, gap = SLOT * s, GAP * s
  -- the doll: the worn grid and the weapons stand for the game's paper doll (IW_CharacterView). An item or coin held
  -- on the cursor and dropped between the slots goes to the window (CInventoryWnd::HandleLButtonUp: the item is
  -- equipped or put away, eqstr 5290 when it stays; coin goes back to the purse). The slots, drawn after it, keep
  -- their own drops.
  if eq.Item(eq.CursorSlot) or eq.CursorCoin() then
    local dx1, dy1 = x + LEFT_W * s, y + 4 * (sz + gap) + 10 * s + sz
    if T.Hit("##doll", x - 4 * s, y - 4 * s, dx1 + 4 * s, dy1 + 4 * s) then
      eq.ClickChild(WINDOW, "IW_CharacterView", "left")
    end
    if ui.IsItemHovered() then
      ui.DrawRect(x - 4 * s, y - 4 * s, dx1 + 4 * s, dy1 + 4 * s, C.accent[1], C.accent[2], C.accent[3], 0.6, 6 * s, 1.5 * s)
    end
  end
  -- worn grid
  for r, row in ipairs(WORN) do
    for c, slot in ipairs(row) do
      items.InvSlot(slot, nil, x + (c - 1) * (sz + gap), y + (r - 1) * (sz + gap), sz, s, SHORT[slot])
    end
  end
  -- weapons, centred under the grid
  local wy = y + 4 * (sz + gap) + 10 * s
  local wx = x + (LEFT_W * s - (3 * sz + 2 * gap)) / 2
  ui.DrawLine(x, wy - 6 * s, x + LEFT_W * s, wy - 6 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  for k, slot in ipairs(WEAPONS) do items.InvSlot(slot, nil, wx + (k - 1) * (sz + gap), wy, sz, s, SHORT[slot]) end

  -- the numbers
  local sx = x + (LEFT_W + 18) * s
  local sw = STATS_W * s
  ui.DrawRectFilled(sx - 9 * s, y - 2 * s, sx - 8 * s, y + BODY_H * s - 14 * s, C.ink[1], C.ink[2], C.ink[3], 0.06)
  local yy = y
  yy = yy + pool(sx, yy, sw, s, "Health", 17, 18, C.hp)
  local _, _, manaRaw = eq.Gauge(2)
  if manaRaw ~= -1 then yy = yy + pool(sx, yy, sw, s, "Mana", 124, 125, C.mana) end
  yy = yy + pool(sx, yy, sw, s, "Endurance", 126, 127, C.endurance)
  yy = yy + 4 * s
  local half = (sw - 14 * s) / 2
  local col2 = sx + half + 14 * s
  local y0 = yy
  yy = yy + statRow(sx, yy, half, "AC", 22, s)
  yy = yy + statRow(sx, yy, half, "ATK", 23, s)
  yy = yy + statRow(sx, yy, half, "Haste", 211, s)
  local y2 = y0
  if serverLabel(6668) then y2 = y2 + statRow(col2, y2, half, "Mitigation", 6668, s) end
  if serverLabel(6667) then y2 = y2 + statRow(col2, y2, half, "Avoidance", 6667, s) end
  yy = math.max(yy, y2) + 6 * s
  ui.DrawLine(sx, yy - 3 * s, sx + sw, yy - 3 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  local attrs = { { "STR", 5 }, { "STA", 6 }, { "AGI", 8 }, { "DEX", 7 }, { "WIS", 9 }, { "INT", 10 }, { "CHA", 11 } }
  local resists = { { "Poison", 12 }, { "Magic", 16 }, { "Disease", 13 }, { "Fire", 14 }, { "Cold", 15 }, { "Corrupt", 136 } }
  local ya, yr = yy + 2 * s, yy + 2 * s
  for _, a in ipairs(attrs) do ya = ya + statRow(sx, ya, half, a[1], a[2], s) end
  for _, r in ipairs(resists) do yr = yr + statRow(col2, yr, half, r[1], r[2], s) end
  local wv, wc = label(24)
  local wl = label(25)
  T.Text(col2, yr + 4 * s, "Weight", C.muted, T.font.small)
  T.Text(col2 + half, yr + 4 * s, wv .. " / " .. wl, wc or C.text, T.font.small, "right")
end

-- The cards stand for the game's AltCurr_PointList (the same rows in the same order while Show all matches the
-- game's Display Missing): a click selects one there. Claim is the game's Create Item (the selected currency out
-- onto the cursor as items, the quantity dialog up to a stack), Reclaim puts the currency item held back into the
-- count (both OP_AltCurrencyReclaim).
local CURRENCY_LIST = "AltCurr_PointList"
local function currencyPage(x, y, s)
  local _, gameMissing = eq.ChildState(WINDOW, "AltCurr_DisplayMissingButton")
  if (gameMissing == true) ~= showMissing then eq.ClickChild(WINDOW, "AltCurr_DisplayMissingButton", "left") end
  local list = eq.AltCurrencies(showMissing) or {}
  local _, th = T.TextSize("Ag", T.font.small)
  local held = 0
  for _, c in ipairs(list) do if (c.count or 0) > 0 then held = held + 1 end end
  T.Text(x, y, string.format("%d held", held), C.muted, T.font.small)
  local tw = T.TextSize("Show all", T.font.small)
  if T.Button("##missing", x + W * s - tw - 30 * s, y - 3 * s, x + W * s, y + 21 * s, s, "Show all",
      { primary = showMissing, tooltip = "Also the currencies you have none of" }) then
    showMissing = not showMissing
  end
  if #list == 0 then
    T.Text(x, y + 34 * s, "No alternate currencies.", C.faint, T.font.small)
    return
  end
  local _, _, sel = eq.ListInfo(WINDOW, CURRENCY_LIST)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y + 30 * s - wy)
  ui.BeginChild("##currencies", W * s, (BODY_H - 30 - 54) * s, false)
  local cw = ui.GetContentRegionAvail()
  local cols = 2
  local cardW = (cw - (cols - 1) * 8 * s) / cols
  local cardH = 44 * s
  for i, c in ipairs(list) do
    if (i - 1) % cols ~= 0 then ui.SameLine(0, 8 * s) end
    local cx, cy = ui.GetCursorScreenPos()
    if ui.InvisibleButton("##cur" .. i, cardW, cardH) then eq.ListClick(WINDOW, CURRENCY_LIST, i - 1, "left") end
    local hov = ui.IsItemHovered()
    local picked = sel == i - 1
    local none = (c.count or 0) == 0
    ui.DrawRectFilled(cx, cy, cx + cardW, cy + cardH, C.ink[1], C.ink[2], C.ink[3],
      (none and 0.02 or 0.045) + (hov and 0.03 or 0), 8 * s)
    if picked then
      ui.DrawRect(cx, cy, cx + cardW, cy + cardH, C.accent[1], C.accent[2], C.accent[3], 0.8, 8 * s, 1.5 * s)
    end
    local tex, u0, v0, u1, v1 = eq.ItemIcon(c.icon or 0)
    T.Icon(cx + 6 * s, cy + 6 * s, 32 * s, s, tex, u0, v0, u1, v1, nil, { radius = 5, alpha = none and 0.4 or 1 })
    T.Text(cx + 46 * s, cy + 6 * s, T.Fit(c.name or "", cardW - 140 * s, T.font.small), none and C.faint or C.text, T.font.small)
    T.Text(cx + cardW - 10 * s, cy + (cardH - th) / 2, T.Thousands(c.count or 0), none and C.faint or C.gold, T.font.normal, "right")
    if c.maxStack and c.maxStack > 0 then
      T.Text(cx + 46 * s, cy + 8 * s + th, "Stacks to " .. T.Thousands(c.maxStack), C.faint, T.font.tiny)
    end
  end
  ui.EndChild()
  -- Claim and Reclaim, enabled as the game's buttons are; clear of the line over the search field (ty0 - 8)
  local by = y + (BODY_H - 46) * s
  local bw = 120 * s
  local actions = {
    { "AltCurr_CreateItemButton", "Claim", "Take the selected currency out onto the cursor as items" },
    { "AltCurr_ReclaimButton", "Reclaim", "Put the currency item you hold back into its count" },
  }
  for k, a in ipairs(actions) do
    local bx1 = x + W * s - (#actions - k) * (bw + 6 * s)
    local enabled = eq.ChildState(WINDOW, a[1])
    if T.Button("##" .. a[1], bx1 - bw, by, bx1, by + 26 * s, s, a[2], { enabled = enabled == true, tooltip = a[3] }) then
      eq.ClickChild(WINDOW, a[1], "left")
    end
  end
  if not sel or sel < 0 then T.Text(x, by + 5 * s, "Select a currency", C.faint, T.font.small) end
end

-- ---------------------------------------------------------------- bandolier page
-- The sets on the left, the selected set's weapons and the game's buttons on the right; clear of the line over the
-- search field (ty0 - 8).
local function bandolierPage(x, y, s)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy)
  ui.BeginChild("##bandolier", W * s, (BODY_H - 16) * s, false)
  local cw, ch = ui.GetContentRegionAvail()
  local listW = math.floor(cw * 0.4)
  band.List(listW, ch)
  ui.SameLine(0, 12 * s)
  ui.BeginChild("##bandolierset", 0, ch, false)
  band.Heading(s)
  band.Weapons(s)
  local _, left = ui.GetContentRegionAvail()
  local bh = band.ButtonsHeight(s)
  if left > bh then ui.Dummy(0, left - bh) end
  band.Buttons(s)
  ui.EndChild()
  ui.EndChild()
end

-- ---------------------------------------------------------------- stats page
local STATS = { { "Strength", 5, 238, 251, 264 }, { "Stamina", 6, 239, 252, 265 }, { "Dexterity", 7, 240, 253, 266 },
  { "Agility", 8, 241, 254, 267 }, { "Wisdom", 9, 242, 255, 268 }, { "Intelligence", 10, 243, 256, 269 },
  { "Charisma", 11, 244, 257, 270 } }
local RESISTS = { { "Poison", 12, 245, 258, 271 }, { "Disease", 13, 246, 259, 272 }, { "Fire", 14, 247, 260, 273 },
  { "Cold", 15, 248, 261, 274 }, { "Magic", 16, 249, 262, 275 }, { "Corruption", 136, 250, 263, 276 } }
local BONUSES = { { "Spell Shielding", 215 }, { "Combat Effects", 216 }, { "Shielding", 217 }, { "Damage Shield", 218 },
  { "DoT Shielding", 219 }, { "DS Mitigation", 220 }, { "Avoidance", 221 }, { "Accuracy", 222 },
  { "Stun Resist", 223 }, { "Strikethrough", 224 }, { "Heal Amount", 225 }, { "Spell Damage", 226 },
  { "Clairvoyance", 227 } }

local function section(x, y, w, title, s)
  T.Text(x, y, title, C.accent, T.font.small)
  local _, th = T.TextSize("Ag", T.font.small)
  ui.DrawLine(x, y + th + 3 * s, x + w, y + th + 3 * s, C.accent[1], C.accent[2], C.accent[3], 0.25, 1)
  return y + th + 8 * s
end

-- name, value [, colour [, tooltip]] rows; returns the next y. The hover areas are numbered per frame: the same name
-- shows in more than one section (Health under pools and regeneration), and equal IDs are an ImGui error.
local kvHits = 0
local function kv(x, y, w, rows, s)
  local _, th = T.TextSize("Ag", T.font.small)
  for i, r in ipairs(rows) do
    if i % 2 == 0 then ui.DrawRectFilled(x - 4 * s, y - 1 * s, x + w + 4 * s, y + th + 2 * s, C.ink[1], C.ink[2], C.ink[3], 0.025, 3 * s) end
    T.Text(x, y, r[1], C.muted, T.font.small)
    T.Text(x + w, y, r[2] or "", r[3] or C.text, T.font.small, "right")
    if r[4] then
      kvHits = kvHits + 1
      T.Hit("##kv" .. kvHits, x, y, x + w, y + th)
      if ui.IsItemHovered() then ui.SetTooltip(r[4]) end
    end
    y = y + th + 4 * s
  end
  return y
end

-- Stat | Total | Base | Heroic | Cap
local function statTable(x, y, w, rows, s)
  local _, th = T.TextSize("Ag", T.font.small)
  local cols = { 0.46, 0.62, 0.79, 1.0 }
  local heads = { "Total", "Base", "Heroic", "Cap" }
  for c, hd in ipairs(heads) do T.Text(x + w * cols[c], y, hd, C.faint, T.font.tiny, "right") end
  y = y + th
  for i, r in ipairs(rows) do
    if i % 2 == 0 then ui.DrawRectFilled(x - 4 * s, y - 1 * s, x + w + 4 * s, y + th + 2 * s, C.ink[1], C.ink[2], C.ink[3], 0.025, 3 * s) end
    T.Text(x, y, r[1], C.muted, T.font.small)
    local total, tc = label(r[2])
    local base, bc = label(r[3])
    local heroic = label(r[4])
    local cap = label(r[5])
    T.Text(x + w * cols[1], y, total, tc or C.text, T.font.small, "right")
    T.Text(x + w * cols[2], y, base, bc or C.muted, T.font.small, "right")
    T.Text(x + w * cols[3], y, heroic ~= "0" and heroic or "-", heroic ~= "0" and C.gold or C.faint, T.font.small, "right")
    T.Text(x + w * cols[4], y, cap, C.faint, T.font.small, "right")
    y = y + th + 4 * s
  end
  return y
end

local function statsPage(x, y, s)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy)
  ui.BeginChild("##stats", W * s, BODY_H * s, false)
  local cx, cy = ui.GetCursorScreenPos()
  local cw = ui.GetContentRegionAvail()
  local colW = (cw - 24 * s) / 2
  local lx, rx = cx + 4 * s, cx + colW + 20 * s
  local st = eq.CharacterStats() or {}
  kvHits = 0
  local function n(v) return v and T.Thousands(v) or "-" end
  -- left: pools, regeneration, defense and offense, progress
  local yl = section(lx, cy, colW, "Health, mana and endurance", s)
  local hp, maxHp = label(17), label(18)
  local mana, maxMana = label(124), label(125)
  local en, maxEn = label(126), label(127)
  local _, _, manaRaw = eq.Gauge(2)
  local rows = {
    { "Health", T.Thousands(hp) .. " / " .. T.Thousands(maxHp), nil,
      st.maxHpBase and ("Without buffs: " .. T.Thousands(st.maxHpBase)) or nil },
  }
  if manaRaw ~= -1 then
    rows[#rows + 1] = { "Mana", T.Thousands(mana) .. " / " .. T.Thousands(maxMana), nil,
      st.maxManaBase and ("Without buffs: " .. T.Thousands(st.maxManaBase)) or nil }
  end
  rows[#rows + 1] = { "Endurance", T.Thousands(en) .. " / " .. T.Thousands(maxEn), nil,
    st.maxEnduranceBase and ("Without buffs: " .. T.Thousands(st.maxEnduranceBase)) or nil }
  yl = kv(lx, yl, colW, rows, s) + 6 * s
  yl = section(lx, yl, colW, "Regeneration (per tick)", s)
  -- the tooltip adds the game's combat regeneration (labels 212..214: no resting bonus, with the buffs' share)
  local function regenTip(base, eqtype)
    local lines = {}
    if base then lines[#lines + 1] = "Without items: " .. n(base) end
    local combat = label(eqtype)
    if combat ~= "" then lines[#lines + 1] = "In combat: " .. combat end
    return #lines > 0 and table.concat(lines, "\n") or nil
  end
  local regen = { { "Health", n(st.hpRegen), C.hp, regenTip(st.hpRegenBase, 212) } }
  if manaRaw ~= -1 then
    regen[#regen + 1] = { "Mana", n(st.manaRegen), C.mana, regenTip(st.manaRegenBase, 213) }
  end
  regen[#regen + 1] = { "Endurance", n(st.enduranceRegen), C.endurance, regenTip(st.enduranceRegenBase, 214) }
  yl = kv(lx, yl, colW, regen, s) + 6 * s
  yl = section(lx, yl, colW, "Defense and offense", s)
  local ac, acc = label(22)
  local haste = tonumber((label(211))) or 100
  local fight = { { "Armor class", ac, acc } }
  for _, m in ipairs({ { "Mitigation", 6668 }, { "Avoidance", 6667 } }) do
    local v, col = serverLabel(m[2])
    if v then fight[#fight + 1] = { m[1], v, col } end
  end
  fight[#fight + 1] = { "Defense", n(st.defense), nil, "ComputeDefense: the skill and agility part of AC" }
  fight[#fight + 1] = { "Haste", (haste - 100) .. "%", haste > 100 and C.good or nil }
  local atk = label(23)
  if atk ~= "" then table.insert(fight, #fight - 1, { "Attack", atk }) end
  yl = kv(lx, yl, colW, fight, s) + 6 * s
  yl = section(lx, yl, colW, "Progress", s)
  local xp, aa = eq.Gauge(4) or 0, eq.Gauge(5) or 0
  local wv, wc = label(24)
  local progress = {
    { "Level", label(2) .. (st.maxLevel and (" of " .. st.maxLevel) or "") },
    { "Experience", string.format("%.2f%%", xp * 100), C.xp },
    { "AA experience", string.format("%.2f%%", aa * 100), C.aa },
    { "AA points", label(71) },
    { "Weight", wv .. " / " .. label(25), wc },
  }
  local title = label(74) -- IW_CharTitle: the AA title prefix
  if title ~= "" then table.insert(progress, 2, { "Title", title }) end
  yl = kv(lx, yl, colW, progress, s)
  -- right: attributes, resists, item bonuses
  local yr = section(rx, cy, colW, "Attributes", s)
  yr = statTable(rx, yr, colW, STATS, s) + 6 * s
  yr = section(rx, yr, colW, "Resists", s)
  yr = statTable(rx, yr, colW, RESISTS, s) + 6 * s
  yr = section(rx, yr, colW, "Item bonuses", s)
  local half = (colW - 16 * s) / 2
  local b1, b2 = {}, {}
  for i, b in ipairs(BONUSES) do
    local v, c = label(b[2])
    local row = { b[1], v, (v ~= "0" and v ~= "") and (c or C.text) or C.faint }
    if i <= math.ceil(#BONUSES / 2) then b1[#b1 + 1] = row else b2[#b2 + 1] = row end
  end
  local y1 = kv(rx, yr, half, b1, s)
  local y2 = kv(rx + half + 16 * s, yr, half, b2, s)
  ui.SetCursorPos(0, 0)
  ui.Dummy(cw, math.max(yl, y1, y2) - cy + 8 * s)
  ui.EndChild()
end

local PAGES = { { "character", "Character" }, { "stats", "Stats" }, { "bandolier", "Bandolier" },
  { "currency", "Alt. Currency" } }
-- In two rows of three. Advanced Inventory is Hailstone's (InventoryManagerWnd).
local BUTTONS = {
  { "IW_Skills", "Skills" }, { "IW_AltAdvBtn", "Alt. Advancement" },
  { "Hailstone_AdvancedInventory", "Advanced Inventory", window = "InventoryManagerWnd",
    tooltip = "Everything you have, worn, carried and banked: search, filters, and Keep and Sell marks" },
  { "Hailstone_AutoInventory", "Auto-Inventory", tooltip = "Put the item on your cursor into your inventory" },
  { "IW_Destroy", "Destroy", danger = true }, { "IW_DoneButton", "Done", primary = true },
}
local BUTTONS_PER_ROW = 3
local NO_TRAIL = { noTrail = true }
local SLOT_NUMBERS = {}
for i = 0, 9 do SLOT_NUMBERS[i] = tostring(i + 1) end

local COINS = {
  { "IW_Money0", "pp", C.platinum },
  { "IW_Money1", "gp", C.gold },
  { "IW_Money2", "sp", C.silver },
  { "IW_Money3", "cp", C.copper },
}

local function hasVitality()
  local _, normal = eq.Gauge(147)
  local _, aa = eq.Gauge(148)
  return normal ~= "" or ((tonumber((eq.Label(2))) or 0) >= 51 and aa ~= "")
end

local function vitality(x, y, w, s, gauge, name, color)
  local fill, amount = eq.Gauge(gauge)
  if not amount or amount == "" then return end
  T.Text(x, y, name, C.muted, T.font.small)
  T.Text(x + w, y, string.format("%.1f%%", (fill or 0) * 100), C.text, T.font.small, "right")
  -- Amounts arrive as strings: a 64-bit vitality pool must not pass through Lua's floating-point formatter.
  T.Text(x, y + 15 * s, T.Fit(amount, w, T.font.tiny), C.faint, T.font.tiny)
  T.Bar("inv.vitality" .. gauge, x, y + 29 * s, x + w, y + 33 * s, s, fill, color, { noTrail = true })
  T.Hit("##vitality" .. gauge, x, y, x + w, y + 34 * s)
  if ui.IsItemHovered() then ui.SetTooltip(name .. ": " .. amount) end
end

local OPTS = {
  size = function() return W, H + (hasVitality() and 40 or 0) end,
  place = function(sw, sh, w, h, s, x, y) return sw - w - 60 * s, sh / 2 - h / 2 end,
  keepTitle = true,
}
local function body(x, y, w, h, s)
  -- header: name, level, class, deity
  local name = label(1)
  local last = label(73)
  T.Text(x, y, name .. (last ~= "" and (" " .. last) or ""), C.text, T.font.large)
  local lvl, cls, deity = label(2), label(3):gsub("%s*\n%s*", " / "), label(4)
  local _, th = T.TextSize("Ag", T.font.large)
  local sub = "Level " .. lvl .. "  \xC2\xB7  " .. cls .. (deity ~= "" and ("  \xC2\xB7  " .. deity) or "")
  T.Text(x, y + th + 2 * s, sub, C.muted, T.font.small)
  -- experience at the right of the header
  local xp, aa = eq.Gauge(4) or 0, eq.Gauge(5) or 0
  local bw = 150 * s
  local bx = x + w - bw
  T.Text(bx, y + 2 * s, "XP", C.muted, T.font.tiny)
  T.Text(bx + bw, y + 2 * s, string.format("%.1f%%", xp * 100), C.text, T.font.tiny, "right")
  T.Bar("inv.xp", bx, y + 18 * s, bx + bw, y + 23 * s, s, xp, C.xp, NO_TRAIL)
  T.Text(bx, y + 25 * s, "AA", C.muted, T.font.tiny)
  T.Text(bx + bw, y + 25 * s, string.format("%.1f%%", aa * 100), C.text, T.font.tiny, "right")
  T.Bar("inv.aa", bx, y + 41 * s, bx + bw, y + 45 * s, s, aa, C.aa, NO_TRAIL)

  -- pages
  local ty = y + HEADER_H * s
  local tx = x
  for k, p in ipairs(PAGES) do
    local tw = T.TextSize(p[2], T.font.small)
    local up = (page == p[1])
    local clicked = T.Hit("##page" .. k, tx - 4 * s, ty, tx + tw + 4 * s, ty + 22 * s)
    T.Text(tx, ty + 3 * s, p[2], (up or ui.IsItemHovered()) and C.text or C.muted, T.font.small)
    if up then ui.DrawLine(tx, ty + 21 * s, tx + tw, ty + 21 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s) end
    if clicked then page = p[1] end
    tx = tx + tw + 22 * s
  end
  ui.DrawLine(x, ty + 22 * s, x + w, ty + 22 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  -- the game fills some pages only while its tab box shows them (alternate currencies)
  local tp = ui.GetSetting("test.inv.page") -- testing: test.inv.page=character|stats|bandolier|currency
  if tp then page = tp end
  local gamePage = GAME_PAGE[page] or 0
  if eq.TabPage(WINDOW, "IW_Subwindows") ~= gamePage then eq.SetTabPage(WINDOW, "IW_Subwindows", gamePage) end
  local by = ty + TABS_H * s + 4 * s
  tools.ModeBanner("##inv", math.max(tx, x + 260 * s), x + w, ty, s) -- after the tabs
  if page == "stats" then statsPage(x, by, s)
  elseif page == "bandolier" then bandolierPage(x, by, s)
  elseif page == "currency" then currencyPage(x, by, s)
  else characterPage(x, by, s) end

  -- the carried slots
  local fy = y + (HEADER_H + TABS_H + BODY_H) * s
  ui.DrawLine(x, fy - 8 * s, x + w, fy - 8 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  local sz = SLOT * s
  local gap = (w - 10 * sz) / 9
  for i = 0, 9 do
    items.InvSlot(GENERAL_FIRST + i, nil, x + i * (sz + gap), fy, sz, s, SLOT_NUMBERS[i])
  end
  local free, total = eq.InventorySpace()
  local low = free <= 5
  local note = tools.Note() -- (a refusal of Sort / Stack started from a bag's window or the Advanced Inventory)
  T.Text(x + w, fy + sz + 1 * s, note or string.format("%d / %d free", free, total),
    (note or low) and C.danger or C.faint, T.font.tiny, "right")
  -- coins, then the buttons
  local cy = fy + sz + 12 * s
  local plat, gold, silver, copper = eq.Money()
  local amounts = { plat, gold, silver, copper }
  local cx = x
  -- the merchant's Parcels page is up: a coin picks money to send there (the game outlines the coin picked)
  local parcels = eq.Parcels() ~= nil and eq.TabPage("MerchantWnd", "MW_MerchantSubwindows") == 2
  for k, coin in ipairs(COINS) do
    local text = T.Thousands(amounts[k] or 0)
    local tw = T.TextSize(text, T.font.small)
    local cw = tw + 34 * s
    local clicked = T.Hit("##" .. coin[1], cx, cy, cx + cw, cy + 24 * s)
    local hov = ui.IsItemHovered()
    ui.DrawRectFilled(cx, cy, cx + cw, cy + 24 * s, C.ink[1], C.ink[2], C.ink[3], hov and 0.08 or 0.04, 12 * s)
    if select(2, eq.ChildState(WINDOW, coin[1])) then
      ui.DrawRect(cx, cy, cx + cw, cy + 24 * s, C.gold[1], C.gold[2], C.gold[3], 0.9, 12 * s, 1.5 * s)
    end
    ui.DrawCircleFilled(cx + 12 * s, cy + 12 * s, 6 * s, coin[3][1], coin[3][2], coin[3][3], 1)
    ui.DrawCircle(cx + 12 * s, cy + 12 * s, 6 * s, 0, 0, 0, 0.4, 1)
    local _, sh = T.TextSize("Ag", T.font.small)
    T.Text(cx + 24 * s, cy + (24 * s - sh) / 2, text, C.text, T.font.small)
    if hov then
      ui.SetTooltip(parcels and ("Send " .. coin[2] .. " by parcel (Shift: all)") or ("Pick up " .. coin[2] .. " (Shift: all)"))
    end
    if clicked then eq.ClickChild(WINDOW, coin[1], "left") end
    cx = cx + cw + 6 * s
  end
  local cursor = eq.Item(eq.CursorSlot)
  if cursor then
    T.Text(x + w, cy + 4 * s, T.Fit("Holding: " .. cursor.name, w - (cx - x) - 8 * s, T.font.small), C.gold, T.font.small, "right")
  end

  local btnY = cy + 34 * s
  if hasVitality() then
    local half = (w - 16 * s) / 2
    vitality(x, btnY, half, s, 147, "XP vitality", C.xp)
    if (tonumber((eq.Label(2))) or 0) >= 51 then
      vitality(x + half + 16 * s, btnY, half, s, 148, "AA vitality", C.aa)
    end
    btnY = btnY + 40 * s
  end
  local per = BUTTONS_PER_ROW
  local bw = (w - (per - 1) * 6 * s) / per
  for k, b in ipairs(BUTTONS) do
    local col, rowN = (k - 1) % per, (k - 1) // per
    local bx0 = x + col * (bw + 6 * s)
    local by0 = btnY + rowN * 32 * s
    local enabled = b.window and true or eq.ChildState(WINDOW, b[1])
    if b.danger and not cursor then enabled = false end
    local open = b.window and eq.WindowShown(b.window) == true
    if T.Button("##" .. b[1], bx0, by0, bx0 + bw, by0 + 26 * s, s, b[2],
        { enabled = enabled ~= false, primary = b.primary or open, tooltip = b.tooltip }) then
      if b.window then eq.ShowWindow(b.window, not open)
      elseif b[1] == "IW_DoneButton" then eq.ShowWindow(WINDOW, false)
      else eq.ClickChild(WINDOW, b[1], "left") end
    end
  end
end

ui.window("Inventory", function()
  if not eq.InGame() then return end
  hud.Dialog(WINDOW, "Inventory", OPTS, body)
end)
