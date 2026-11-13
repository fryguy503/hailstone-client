-- The HUD (gamepad pack): always on, never takes the pad's focus. Each part stands in for a stock window, so its
-- key and the EQ menu still show and hide it, and the C++ window stays off:
--   Player (PlayerWindow)      name, level and class, HP / mana / endurance, experience
--   Target (TargetWindow)      name in its con colour, HP, its buffs, the target's target
--   Group (GroupWindow)        the members' HP / mana / endurance (a click targets one, as F2..F6)
--   Pet (PetInfoWindow)        the pet's name and HP
--   Extended Target (ExtendedTargetWnd)   the slots holding a spawn: name, HP, role (a click targets one)
--   Casting (CastingWindow)    the spell being cast and its bar
--   Buffs (BuffWindow, ShortDurationBuffWindow)   icons with the time left
--   Compass (CompassWindow)    a heading strip
--   Minimap (a client extension, no stock window): the zone map around the player, top right; a click opens the
--                              map window (MapViewWnd), the wheel or + / - zoom, a right click turns rotation on/off
-- Every value is the game's (eq.Label / eq.Gauge by EQType, the stock windows' children); nothing is computed here.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local A = dofile("lib/actions.lua")
local C = T.C
local ROLE_LABELS = { [0] = "", "Main Tank", "Main Assist", "Main Tank / Main Assist", "Puller",
  "Main Tank / Puller", "Main Assist / Puller", "Main Tank / Main Assist / Puller" }

-- A HUD window at x, y (design px), w x h; inputs: the mouse may click in it. draw(x0, y0, x1, y1).
local function hud(name, x, y, w, h, inputs, draw)
  T.frame()
  T.resetFonts()
  local px, py, pw, ph = T.px(x), T.px(y), T.px(w), T.px(h)
  if px < 0 then px = T.W + px - pw end -- negative x: from the right edge
  if py < 0 then py = T.H + py - ph end
  ui.SetNextWindowPos(px, py, ui.Cond.Always)
  ui.SetNextWindowSize(pw, ph, ui.Cond.Always)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoBackground | ui.WindowFlags.NoSavedSettings
      | ui.WindowFlags.NoMove | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoBringToFrontOnFocus | W.NoNav
  if not inputs then flags = flags | ui.WindowFlags.NoInputs end
  if ui.Begin(name, nil, flags) then
    T.font(T.BASE)
    draw(px, py, px + pw, py + ph)
    T.unfont()
  end
  ui.End()
  ui.PopStyleVar(2)
end

local function num(type)
  local t = eq.Label(type)
  return t or ""
end

-- ---------------------------------------------------------------- player
ui.window("HUD Player", function()
  if not W.shown("PlayerWindow") then return end
  local mana, _, mraw = eq.Gauge(2)
  hud("HUD Player", 32, 28, 470, mraw ~= -1 and 176 or 148, true, function(x0, y0, x1, y1)
    if ui.InvisibleButton("##player", x1 - x0, y1 - y0) then eq.ClickChild("PlayerWindow", "PlayerHP", "left") end
    if ui.IsItemHovered() and not ui.GetIO().alt and ui.IsMouseClicked(ui.Mouse.Right) then
      eq.WindowContextMenu("PlayerWindow")
    end
    T.panel(x0, y0, x1, y1, 0.9)
    -- auto attack (PlayerWindow's A_AttackIndicatorAnim*): a pulsing red wash and a thick red ring
    if eq.ChildShown("PlayerWindow", "A_AttackIndicatorAnimTop") == true then
      local pulse = math.abs(math.sin(ui.GetTime() * 4))
      T.rect(x0, y0, x1, y1, C.bad, 0.06 + 0.12 * pulse, T.px(14))
      local t = T.px(4)
      T.outline(x0 - t / 2, y0 - t / 2, x1 + t / 2, y1 + t / 2, C.bad, 0.55 + 0.45 * pulse, T.px(14) + t / 2, t)
    end
    local p = T.px(16)
    local name = (eq.Label(1) or ""):gsub("\n.*", "")
    local lvl, cls = num(2), num(3)
    T.font(26)
    local _, nh = T.text(x0 + p, y0 + p - T.px(2), name)
    T.unfont()
    T.font(18)
    local level = (lvl ~= "" and ("Level " .. lvl) or "") .. (cls ~= "" and ("  " .. cls) or "")
    T.textIn(x0 + p, y0 + p, x1 - p, y0 + p + nh, level, C.dim, 1)
    -- the combat timer (label 137: OP_RestState's downtime left), left of the level while it runs
    local timer = eq.Label(137) or ""
    if timer ~= "" then
      local lw = ui.CalcTextSize(level)
      T.textIn(x0 + p, y0 + p, x1 - p - lw - T.px(14), y0 + p + nh, timer, C.endurance, 1)
    end
    T.unfont()
    local y = y0 + p + nh + T.px(8)
    local bh = T.px(30)
    local hp = eq.Gauge(1)
    T.bar(x0 + p, y, x1 - p, y + bh, hp, C.hp, num(17) .. " / " .. num(18), num(19) .. "%")
    y = y + bh + T.px(6)
    local small = T.px(22)
    T.font(17)
    if mraw ~= -1 then
      T.bar(x0 + p, y, x1 - p, y + small, mana, C.mana, num(124) .. " / " .. num(125), num(20) .. "%")
      y = y + small + T.px(5)
    end
    local en = eq.Gauge(3)
    T.bar(x0 + p, y, x1 - p, y + small, en, C.endurance, num(126) .. " / " .. num(127), num(21) .. "%")
    T.unfont()
    -- experience: a thin line along the bottom edge
    local xp = eq.Gauge(4) or 0
    local ey = y1 - T.px(7)
    T.rect(x0 + p, ey, x1 - p, ey + T.px(4), C.sunken, 1, T.px(2))
    if xp > 0 then T.rect(x0 + p, ey, x0 + p + (x1 - x0 - 2 * p) * math.min(1, xp), ey + T.px(4), C.exp, 1, T.px(2)) end
  end)
end)

-- ---------------------------------------------------------------- target
local conRgba, conBar = { 0, 0, 0, 1 }, { 0, 0, 0, 1 } -- the target's con colour, and its bar's (darker)
local function conColour()
  if eq.ChildShown("TargetWindow", "A_TargetBoxStaticAnimFill") == false then return nil end
  local r, g, b = eq.ChildColor("TargetWindow", "A_TargetBoxStaticAnimFill", "decal")
  if r then
    conRgba[1], conRgba[2], conRgba[3] = r, g, b
    return conRgba
  end
  return nil
end

local TARGET_BUFFS = {} -- TargetWindow's Buff0..Buff41
for i = 0, 41 do TARGET_BUFFS[i + 1] = "Buff" .. i end
local targetBuffs = {} -- the slots holding a buff this frame, in order

ui.window("HUD Target", function()
  if not W.shown("TargetWindow") then return end
  local frac, name, raw = eq.Gauge(6)
  if raw == -1 or not name or name == "" then return end
  -- a second row only for the target's buffs or its target (eq.Gauge(27))
  local tfrac, tname, traw = eq.Gauge(27)
  local nBuffs = 0
  for i = 1, #TARGET_BUFFS do
    if eq.BuffSlot("TargetWindow", TARGET_BUFFS[i]) then
      nBuffs = nBuffs + 1
      targetBuffs[nBuffs] = TARGET_BUFFS[i]
    end
  end
  local extra = (traw ~= -1 and (tname or "") ~= "") or nBuffs > 0
  hud("HUD Target", 660, 74, 600, extra and 150 or 110, false, function(x0, y0, x1, y1)
    T.panel(x0, y0, x1, y1, 0.9)
    local con = conColour()
    local p = T.px(16)
    if con then -- the con colour as the panel's left edge and the name
      T.rect(x0, y0 + T.px(14), x0 + T.px(6), y1 - T.px(14), con, 1, T.px(3))
    end
    T.font(26)
    local _, nh = ui.CalcTextSize("Ag")
    T.textIn(x0 + p + T.px(4), y0 + p - T.px(2), x1 - p - T.px(90), y0 + p + nh, name, con or C.text, 0)
    T.unfont()
    -- the distance (a client that gives it: eq.TargetDistance)
    if eq.TargetDistance then
      local d = eq.TargetDistance()
      if d then
        T.font(18)
        T.textIn(x1 - p - T.px(140), y0 + p, x1 - p, y0 + p + nh, string.format("%d ft", math.floor(d + 0.5)), C.dim, 1)
        T.unfont()
      end
    end
    local y = y0 + p + nh + T.px(6)
    local bh = T.px(30)
    local pct = eq.Label(29) or ""
    if con then conBar[1], conBar[2], conBar[3] = con[1] * 0.85, con[2] * 0.85, con[3] * 0.85 end
    T.bar(x0 + p + T.px(4), y, x1 - p, y + bh, raw == -3 and 0 or frac, con and conBar or C.hp,
      raw == -3 and "Corpse" or "", pct ~= "" and (pct .. "%") or "")
    y = y + bh + T.px(8)
    -- the target's buffs: small icons
    local s = T.px(28)
    local x = x0 + p + T.px(4)
    for i = 1, nBuffs do
      local tex, u0, v0, u1, v1 = eq.ChildArt("TargetWindow", targetBuffs[i], "decal")
      if type(tex) == "userdata" then
        T.icon(tex, u0, v0, u1, v1, x, y, x + s, y + s)
        x = x + s + T.px(3)
        if x + s > x1 - p - T.px(180) then break end
      end
    end
    -- the target's target
    if traw ~= -1 and tname and tname ~= "" then
      T.font(16)
      local tx0 = x1 - p - T.px(170)
      T.bar(tx0, y + T.px(2), x1 - p, y + s - T.px(2), tfrac, C.hp, T.fit(tname, T.px(150)))
      T.unfont()
    end
  end)
end)

-- ---------------------------------------------------------------- group
local MEMBER_IDS = {} -- each row's button and the group window's gauge for it
for i = 1, 5 do MEMBER_IDS[i] = { button = "##member" .. i, gauge = "Gauge" .. i } end
local members = {} -- the members shown this frame (records kept from frame to frame)

ui.window("HUD Group", function()
  if not W.shown("GroupWindow") then return end
  local count = 0
  for i = 1, 5 do
    local frac, name, raw, tr, tg, tb = eq.Gauge(10 + i)
    -- tr, tg, tb: a special value's tint (-2, not in the zone: full in the game's dark red)
    if raw ~= -1 and name and name ~= "" then
      count = count + 1
      local m = members[count]
      if not m then
        m = { rgba = { 0, 0, 0, 1 } }
        members[count] = m
      end
      m.i, m.name, m.hp = i, name, frac
      local roles = (eq.ChildShown("GroupWindow", "GroupRoleTank" .. i) and 1 or 0)
        | (eq.ChildShown("GroupWindow", "GroupRoleAssist" .. i) and 2 or 0)
        | (eq.ChildShown("GroupWindow", "GroupRolePuller" .. i) and 4 or 0)
      m.roles = ROLE_LABELS[roles]
      if tr then
        m.tint = m.rgba
        m.tint[1], m.tint[2], m.tint[3] = tr, tg, tb
      else
        m.tint = nil
      end
    end
  end
  if count == 0 then return end
  local rowH = 80
  hud("HUD Group", 32, 214, 400, 16 + count * (rowH + 6), true, function(x0, y0, x1, y1)
    local y = y0
    for k = 1, count do
      local m = members[k]
      local ry1 = y + T.px(rowH)
      ui.SetCursorPos(0, y - y0)
      local clicked = ui.InvisibleButton(MEMBER_IDS[m.i].button, x1 - x0, T.px(rowH))
      if clicked then eq.ClickChild("GroupWindow", MEMBER_IDS[m.i].gauge, "left") end -- targets the member (F2..F6)
      local hovered = ui.IsItemHovered()
      if hovered and not ui.GetIO().alt and ui.IsMouseClicked(ui.Mouse.Right) then
        eq.ClickChild("GroupWindow", MEMBER_IDS[m.i].gauge, "right")
      end
      T.panel(x0, y, x1, ry1, hovered and 1 or 0.85, T.px(12))
      local p = T.px(12)
      T.font(18)
      local _, th = ui.CalcTextSize("Ag")
      T.draw(x0 + p, y + T.px(6), T.fit(m.name, x1 - x0 - 2 * p - T.px(60)))
      local pct = eq.Label(34 + m.i) or ""
      T.textIn(x0 + p, y + T.px(6), x1 - p, y + T.px(6) + th, pct ~= "" and (pct .. "%") or "", C.dim, 1)
      T.unfont()
      local by = y + T.px(10) + th
      T.bar(x0 + p, by, x1 - p, by + T.px(12), m.hp, m.tint or C.hp)
      local mana, _, mraw = eq.Gauge(30 + m.i)
      local en = eq.Gauge(35 + m.i)
      local half = (x1 - x0 - 2 * p - T.px(6)) / 2
      by = by + T.px(16)
      if mraw ~= -1 and mana then T.bar(x0 + p, by, x0 + p + half, by + T.px(7), mana, C.mana) end
      if en then T.bar(x1 - p - half, by, x1 - p, by + T.px(7), en, C.endurance) end
      if m.roles ~= "" then
        T.font(15)
        T.draw(x0 + p, by + T.px(10), T.fit(m.roles, x1 - x0 - 2 * p), C.accent)
        T.unfont()
      end
      y = ry1 + T.px(6)
    end
  end)
end)

-- ---------------------------------------------------------------- extended target
-- The slots that hold a spawn (the server's auto haters: NPCs with aggro on you or your group; the roles the game's
-- menu or /xtarget set), under the minimap: name in its con colour, HP, the role when it is not an auto hater. The
-- count of slots is the server's (5 + the Extended Target AA's) less the slots the server module hides. A click targets the slot's spawn.
local XT_IDS = {} -- each slot's children
for n = 0, 19 do
  XT_IDS[n] = { gauge = "ETW_Gauge" .. n, hpLabel = "ETW_HPLabel" .. n, role = "ETW_Role" .. n, button = "##xtarget" .. n }
end
local xtRows = {} -- the slots holding a spawn this frame (records kept from frame to frame)
ui.window("HUD Extended Target", function()
  if not W.shown("ExtendedTargetWnd") then return end
  local WINDOW = "ExtendedTargetWnd"
  local nRows, slots = 0, 0
  for n = 0, 19 do
    local ids = XT_IDS[n]
    -- a slot the game shows: in the server's count and not left out by a server's plugin (NMS: your own pets)
    -- (a hidden slot can sit between shown ones: all twenty are asked)
    local shown = eq.ChildShown(WINDOW, ids.gauge) ~= false
    if shown then slots = slots + 1 end
    if shown and eq.ChildShown(WINDOW, ids.hpLabel) == true then
      local frac, name = eq.Gauge(42 + n)
      local r, g, b = eq.ChildColor(WINDOW, ids.gauge)
      local role = eq.ChildTooltip(WINDOW, ids.role) or ""
      nRows = nRows + 1
      local row = xtRows[nRows]
      if not row then
        row = { conRgba = { 0, 0, 0, 1 }, conBar = { 0, 0, 0, 1 } }
        xtRows[nRows] = row
      end
      row.n, row.name, row.hp, row.pct = n, name or "", frac, eq.Label(151 + n) or ""
      row.role = role ~= "" and role ~= "Auto Hater" and role or nil
      if r then
        row.con = row.conRgba
        row.con[1], row.con[2], row.con[3] = r, g, b
        row.conBar[1], row.conBar[2], row.conBar[3] = r * 0.85, g * 0.85, b * 0.85
      else
        row.con = nil
      end
    end
  end
  if nRows == 0 then return end
  local rows = xtRows
  local rowH = 52
  hud("HUD Extended Target", -24, 390, 300, 34 + nRows * (rowH + 4), true, function(x0, y0, x1, y1)
    T.font(16)
    local _, th = ui.CalcTextSize("Ag")
    T.textIn(x0 + T.px(10), y0 + T.px(4), x1 - T.px(10), y0 + T.px(4) + th, "Extended Target", C.dim, 0)
    T.textIn(x0 + T.px(10), y0 + T.px(4), x1 - T.px(10), y0 + T.px(4) + th, nRows .. " / " .. slots, C.dim, 1)
    T.unfont()
    local y = y0 + T.px(30)
    for k = 1, nRows do
      local r = rows[k]
      local ry1 = y + T.px(rowH)
      ui.SetCursorPos(0, y - y0)
      if ui.InvisibleButton(XT_IDS[r.n].button, x1 - x0, T.px(rowH)) then eq.ClickChild(WINDOW, XT_IDS[r.n].gauge, "left") end
      T.panel(x0, y, x1, ry1, ui.IsItemHovered() and 1 or 0.85, T.px(10))
      local p = T.px(10)
      T.font(17)
      local _, h = ui.CalcTextSize("Ag")
      T.textIn(x0 + p, y + T.px(5), x1 - p - T.px(r.role and 110 or 50), y + T.px(5) + h, T.fit(r.name, x1 - x0 - 2 * p - T.px(r.role and 110 or 50)), r.con or C.text, 0)
      local right = r.pct ~= "" and (r.pct .. "%") or ""
      if r.role then right = r.role .. "  " .. right end
      T.textIn(x0 + p, y + T.px(5), x1 - p, y + T.px(5) + h, right, C.dim, 1)
      T.unfont()
      local by = y + T.px(9) + h
      T.bar(x0 + p, by, x1 - p, by + T.px(10), r.hp, r.con and r.conBar or C.hp)
      y = ry1 + T.px(4)
    end
  end)
end)

-- ---------------------------------------------------------------- pet
ui.window("HUD Pet", function()
  if not W.shown("PetInfoWindow") then return end
  local frac, name, raw = eq.Gauge(16)
  if raw == -1 or not name or name == "" then return end
  hud("HUD Pet", 32, -350, 400, 70, false, function(x0, y0, x1, y1)
    local hp = GPSTATE.hudPet or {} -- (the chat ticker stays under it)
    hp.y1, hp.at = y1, ui.GetTime()
    GPSTATE.hudPet = hp
    T.panel(x0, y0, x1, y1, 0.85, T.px(12))
    local p = T.px(12)
    T.font(18)
    local _, th = ui.CalcTextSize("Ag")
    T.draw(x0 + p, y0 + T.px(6), "Pet: " .. T.fit(name, x1 - x0 - T.px(140)))
    local pct = eq.Label(69) or ""
    T.textIn(x0 + p, y0 + T.px(6), x1 - p, y0 + T.px(6) + th, pct ~= "" and (pct .. "%") or "", C.dim, 1)
    T.unfont()
    T.bar(x0 + p, y0 + T.px(12) + th, x1 - p, y0 + T.px(26) + th, frac, C.good)
  end)
end)

-- ---------------------------------------------------------------- casting
ui.window("HUD Casting", function()
  if not W.shown("CastingWindow") then return end
  local frac, _, raw = eq.Gauge(7)
  if raw == -1 then return end
  hud("HUD Casting", 710, -330, 500, 64, false, function(x0, y0, x1, y1)
    T.panel(x0, y0, x1, y1, 0.9, T.px(14))
    local p = T.px(12)
    T.bar(x0 + p, y0 + p, x1 - p, y1 - p, frac, C.cast, eq.Label(134) or "")
  end)
end)

-- ---------------------------------------------------------------- buffs
local BUFF_IDS = {} -- Buff0..Buff41
for i = 0, 41 do BUFF_IDS[i] = "Buff" .. i end
local blocks = { BuffWindow = { n = 0 }, ShortDurationBuffWindow = { n = 0 } } -- each window's buffs this frame

-- The buffs of `screen`'s count slots, packed: its block (n of them, each a record kept from frame to frame).
local function buffBlock(screen, count)
  local list = blocks[screen]
  local n = 0
  for i = 0, count - 1 do
    local spell, ms, name, _, fade = eq.BuffSlot(screen, BUFF_IDS[i])
    if spell then
      n = n + 1
      local b = list[n]
      if not b then
        b = {}
        list[n] = b
      end
      b.id, b.ms, b.name, b.fade = BUFF_IDS[i], ms, name, fade or 1
      -- the time left under it (T.short), formatted again only when the whole second changes
      if ms and ms > 0 then
        local sec = math.floor(ms / 1000)
        if b.sec ~= sec then b.sec, b.time = sec, T.short(ms / 1000) end
      end
    end
  end
  list.n = n
  return list
end

local function drawBuffs(screen, list, x0, y0, x1, size)
  local gap = T.px(6)
  local x, y = x1 - size, y0
  T.font(14)
  local _, th = ui.CalcTextSize("0")
  for k = 1, list.n do
    local b = list[k]
    local tex, u0, v0, u1, v1 = eq.ChildArt(screen, b.id, "decal")
    T.rect(x - 2, y - 2, x + size + 2, y + size + 2, C.sunken, 1, T.px(6))
    T.icon(tex, u0, v0, u1, v1, x, y, x + size, y + size, 0.2 + 0.8 * b.fade) -- the game's fade under 19 s
    if b.ms and b.ms > 0 then
      local t = b.time
      local tw = ui.CalcTextSize(t)
      T.draw(x + (size - tw) / 2, y + size + T.px(1), t, b.ms < 30000 and C.endurance or C.dim)
    end
    x = x - size - gap
    if x < x0 then
      x = x1 - size
      y = y + size + th + T.px(8)
    end
  end
  T.unfont()
  return y + size + th + T.px(8)
end

ui.window("HUD Buffs", function()
  local long = W.shown("BuffWindow")
  local short = W.shown("ShortDurationBuffWindow")
  if not long and not short then return end
  local lb = long and buffBlock("BuffWindow", 42) or nil
  local sb = short and buffBlock("ShortDurationBuffWindow", 20) or nil
  local nl, ns = lb and lb.n or 0, sb and sb.n or 0
  if nl == 0 and ns == 0 then return end
  hud("HUD Buffs", -344, 24, 300, 420, false, function(x0, y0, x1, y1)
    local size = T.px(40)
    local y = y0
    if nl > 0 then y = drawBuffs("BuffWindow", lb, x0, y, x1, size) end
    if ns > 0 then drawBuffs("ShortDurationBuffWindow", sb, x0, y + T.px(6), x1, T.px(34)) end
  end)
end)

-- ---------------------------------------------------------------- compass
local CARDINALS = { { 0, "N" }, { 45, "NW" }, { 90, "W" }, { 135, "SW" }, { 180, "S" }, { 225, "SE" }, { 270, "E" }, { 315, "NE" } }

ui.window("HUD Compass", function()
  if not W.shown("CompassWindow") then return end
  local _, _, _, heading = eq.CompassStrips()
  if not heading then return end
  hud("HUD Compass", 760, 22, 400, 44, false, function(x0, y0, x1, y1)
    T.panel(x0, y0, x1, y1, 0.8, T.px(22))
    local cx = (x0 + x1) / 2
    local k = (x1 - x0 - T.px(40)) / 180 -- 180 degrees across the strip
    ui.PushClipRect(x0 + T.px(14), y0, x1 - T.px(14), y1, true)
    for deg = 0, 345, 15 do
      local off = ((heading - deg + 180) % 360) - 180 -- a heading to the left is larger
      local x = cx + off * k
      if deg % 45 ~= 0 then ui.DrawLine(x, y1 - T.px(12), x, y1 - T.px(6), C.ink[1], C.ink[2], C.ink[3], 0.3, 1) end
    end
    T.font(18)
    for _, c in ipairs(CARDINALS) do
      local off = ((heading - c[1] + 180) % 360) - 180
      local x = cx + off * k
      local tw, th = ui.CalcTextSize(c[2])
      T.draw(x - tw / 2, (y0 + y1 - th) / 2, c[2], c[2] == "N" and C.accent or (#c[2] == 1 and C.text or C.dim))
    end
    T.unfont()
    ui.PopClipRect()
    T.triangle("down", cx, y0 + T.px(5), T.px(5), C.accent, 1)
  end)
end)

-- ---------------------------------------------------------------- minimap
-- The zone map in a circle around the player (eq.DrawMinimap: the map window's lines, group, /find path and compass
-- points), its zone name above and the game clock below. The range and rotation are kept in the pack's settings.
local RANGES = { 150, 300, 600, 1200, 2400 }
local mini = {
  zoom = tonumber(ui.GetSetting("minimap.zoom", "3")) or 3,
  rotate = ui.GetSetting("minimap.rotate", "0") == "1",
}

local function miniZoom(step)
  mini.zoom = math.max(1, math.min(#RANGES, mini.zoom + step))
  ui.SetSetting("minimap.zoom", mini.zoom)
end

local function clock()
  local h, m = eq.GameTime()
  if not h then return nil end
  local h12 = h % 12
  if h12 == 0 then h12 = 12 end
  return string.format("%d:%02d %s", h12, m, h < 12 and "AM" or "PM")
end

-- A small round button on the ring: returns true when clicked.
local function ringButton(id, cx, cy, r, label)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(cx - r - wx, cy - r - wy)
  local clicked = ui.InvisibleButton(id, r * 2, r * 2)
  local hov = ui.IsItemHovered()
  ui.DrawCircleFilled(cx, cy, r, C.raised[1], C.raised[2], C.raised[3], 0.95)
  ui.DrawCircle(cx, cy, r, C.edge[1], C.edge[2], C.edge[3], hov and 0.6 or 0.25, T.px(1.5))
  T.font(20)
  local tw, th = ui.CalcTextSize(label)
  T.draw(cx - tw / 2, cy - th / 2 - T.px(1), label, hov and C.accent or C.text)
  T.unfont()
  return clicked
end

ui.window("HUD Minimap", function()
  if not eq.InGame() then return end
  hud("HUD Minimap", -24, 20, 300, 352, true, function(x0, y0, x1, y1)
    local cx, r = (x0 + x1) / 2, T.px(128)
    local cy = y0 + T.px(44) + r
    -- the zone name
    local zone = eq.ZoneName() or ""
    if zone ~= "" then
      T.font(19)
      zone = T.fit(zone, x1 - x0 - T.px(24))
      local tw, th = ui.CalcTextSize(zone)
      T.panel(cx - tw / 2 - T.px(14), y0, cx + tw / 2 + T.px(14), y0 + th + T.px(10), 0.85, T.px(12))
      T.draw(cx - tw / 2, y0 + T.px(5), zone, C.text)
      T.unfont()
    end
    -- the disc, the map and the ring
    ui.DrawCircleFilled(cx, cy, r + T.px(5), C.bg[1], C.bg[2], C.bg[3], 0.92)
    ui.DrawCircleFilled(cx, cy, r, C.mapBg[1], C.mapBg[2], C.mapBg[3], C.mapBg[4])
    local wx, wy = ui.GetWindowPos()
    ui.SetCursorPos(cx - r - wx, cy - r - wy)
    ui.InvisibleButton("##minimap", r * 2, r * 2)
    local hovered = ui.IsItemHovered()
    local nx, ny = eq.DrawMinimap(cx, cy, r, RANGES[mini.zoom], mini.rotate)
    ui.DrawCircle(cx, cy, r, C.edge[1], C.edge[2], C.edge[3], 0.35, T.px(2))
    ui.DrawCircle(cx, cy, r + T.px(4), 0, 0, 0, 0.5, T.px(3))
    -- north on the ring
    if nx then
      local px, py = cx + nx * r, cy + ny * r
      ui.DrawCircleFilled(px, py, T.px(13), C.bg[1], C.bg[2], C.bg[3], 1)
      ui.DrawCircle(px, py, T.px(13), C.accent[1], C.accent[2], C.accent[3], 0.8, T.px(1.5))
      T.font(17)
      local tw, th = ui.CalcTextSize("N")
      T.draw(px - tw / 2, py - th / 2, "N", C.accent)
      T.unfont()
    end
    if hovered then
      ui.SetTooltip(string.format("%s\nClick: map  Wheel: zoom (range %d)\nRight click: %s", zone,
        RANGES[mini.zoom], mini.rotate and "north up" or "rotate with the player"))
      local wheel = ui.GetIO().wheel or 0
      if wheel ~= 0 then miniZoom(wheel > 0 and -1 or 1) end
      if ui.IsMouseClicked(ui.Mouse.Left) then A.toggle("MapViewWnd") end
      if ui.IsMouseClicked(ui.Mouse.Right) then
        mini.rotate = not mini.rotate
        ui.SetSetting("minimap.rotate", mini.rotate)
      end
    end
    -- zoom buttons on the lower right of the ring (as WoW's), the clock below
    local a1, a2 = math.rad(35), math.rad(60)
    if ringButton("##minizoomin", cx + math.cos(a1) * (r + T.px(2)), cy + math.sin(a1) * (r + T.px(2)), T.px(16), "+") then
      miniZoom(-1)
    end
    if ringButton("##minizoomout", cx + math.cos(a2) * (r + T.px(2)), cy + math.sin(a2) * (r + T.px(2)), T.px(16), "-") then
      miniZoom(1)
    end
    local t = clock()
    if t then
      T.font(18)
      local tw, th = ui.CalcTextSize(t)
      local ty = cy + r + T.px(10)
      T.panel(cx - tw / 2 - T.px(12), ty, cx + tw / 2 + T.px(12), ty + th + T.px(8), 0.85, T.px(10))
      T.draw(cx - tw / 2, ty + T.px(4), t, C.dim)
      T.unfont()
    end
  end)
end)

-- ---------------------------------------------------------------- the menu button (mouse; Start on the pad)
ui.window("HUD Menu Button", function()
  if not eq.InGame() then return end
  local open = GPSTATE and GPSTATE.menuOpen
  if open then return end
  hud("HUD Menu Button", 24, -24, 210, 52, true, function(x0, y0, x1, y1)
    ui.SetCursorPos(0, 0)
    local clicked = ui.InvisibleButton("##menu", x1 - x0, y1 - y0)
    local hov = ui.IsItemHovered()
    T.panel(x0, y0, x1, y1, hov and 1 or 0.75, T.px(26))
    T.font(18)
    local gw = T.glyph(pad, "start", x0 + T.px(14) + T.px(30), (y0 + y1) / 2, T.px(30))
    T.textIn(x0 + T.px(14) + T.px(30) + gw / 2 + T.px(10), y0, x1 - T.px(10), y1, "Menu", C.text, 0)
    T.unfont()
    if clicked then GPSTATE.menuOpen = true end
  end)
end)
