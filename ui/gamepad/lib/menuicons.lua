-- Line icons for the game menu, drawn with ImGui's shapes (this UI's own, no game art). Each icon is drawn in a box
-- of `d` pixels centred at cx, cy, in units of -1..1. menuicons.Draw(text, cx, cy, d, color, alpha) draws the icon
-- for a menu row's text (a generic dot for a row it does not know). The same file as the modern pack's
-- (ui/modern/lib/menuicons.lua); menus.lua sets M.PushFont / M.PopFont to this pack's fonts.
local M = {}
-- the font of a hot button bar's number
function M.PushFont() ui.PushFont(2) end
function M.PopFont() ui.PopFont() end

local cx, cy, k, cr, cg, cb, ca, lw -- the icon being drawn: centre, half size, colour, line width

local function P(x, y) return cx + x * k, cy + y * k end
local function line(x0, y0, x1, y1)
  local ax, ay = P(x0, y0)
  local bx, by = P(x1, y1)
  ui.DrawLine(ax, ay, bx, by, cr, cg, cb, ca, lw)
end
local function poly(pts, closed)
  for i = 1, #pts - 2, 2 do line(pts[i], pts[i + 1], pts[i + 2], pts[i + 3]) end
  if closed then line(pts[#pts - 1], pts[#pts], pts[1], pts[2]) end
end
-- poly over its points given as arguments (x0, y0, x1, y1, ...): no table each time an icon draws
local function polyv(closed, ...)
  local n = select("#", ...)
  for i = 1, n - 2, 2 do
    local x0, y0, x1, y1 = select(i, ...)
    line(x0, y0, x1, y1)
  end
  if closed then
    local xl, yl = select(n - 1, ...)
    local xf, yf = ...
    line(xl, yl, xf, yf)
  end
end
local function circle(x, y, r) local px, py = P(x, y) ui.DrawCircle(px, py, r * k, cr, cg, cb, ca, lw) end
local function disc(x, y, r) local px, py = P(x, y) ui.DrawCircleFilled(px, py, r * k, cr, cg, cb, ca) end
local function rect(x0, y0, x1, y1, round)
  local ax, ay = P(x0, y0)
  local bx, by = P(x1, y1)
  ui.DrawRect(ax, ay, bx, by, cr, cg, cb, ca, (round or 0.15) * k, lw)
end
local function fill(x0, y0, x1, y1, round)
  local ax, ay = P(x0, y0)
  local bx, by = P(x1, y1)
  ui.DrawRectFilled(ax, ay, bx, by, cr, cg, cb, ca, (round or 0) * k)
end
local function tri(x0, y0, x1, y1, x2, y2)
  local ax, ay = P(x0, y0)
  local bx, by = P(x1, y1)
  local qx, qy = P(x2, y2)
  ui.DrawTriangleFilled(ax, ay, bx, by, qx, qy, cr, cg, cb, ca)
end
-- The unit points of the arcs and the star, worked out once (the menu draws its icons every frame).
local arcs = {} -- [a0][a1] = { cos, sin, cos, sin, ... }
local function unitArc(a0, a1)
  local byStart = arcs[a0]
  if not byStart then
    byStart = {}
    arcs[a0] = byStart
  end
  local u = byStart[a1]
  if not u then
    u = {}
    local n = math.max(4, math.floor(math.abs(a1 - a0) / 15))
    for i = 0, n do
      local a = math.rad(a0 + (a1 - a0) * i / n)
      u[#u + 1] = math.cos(a)
      u[#u + 1] = math.sin(a)
    end
    byStart[a1] = u
  end
  return u
end
-- a polyline through unit points u scaled by r (or r * inner[i] for point i) about x, y
local function unitPoly(u, x, y, r, inner, closed)
  local px, py
  local fx, fy
  for i = 1, #u, 2 do
    local f = inner and inner[(i + 1) // 2]
    local rr = f and r * f or r
    local qx, qy = x + u[i] * rr, y + u[i + 1] * rr
    if px then line(px, py, qx, qy) else fx, fy = qx, qy end
    px, py = qx, qy
  end
  if closed and px then line(px, py, fx, fy) end
end
-- an arc from angle a0 to a1 (degrees, 0 = right, 90 = down)
local function arc(x, y, r, a0, a1)
  unitPoly(unitArc(a0, a1), x, y, r)
end
local function person(x, y, sc)
  circle(x, y - 0.38 * sc, 0.26 * sc)
  arc(x, y + 0.62 * sc, 0.5 * sc, 180, 360)
end
local STAR, STAR_INNER = {}, {}
for i = 0, 9 do
  local a = math.rad(-90 + i * 36)
  STAR[#STAR + 1] = math.cos(a)
  STAR[#STAR + 1] = math.sin(a)
  STAR_INNER[i + 1] = (i % 2 == 1) and 0.45 or false
end
local function star(x, y, r)
  unitPoly(STAR, x, y, r, STAR_INNER, true)
end
local function upArrow(x, y, sc)
  line(x, y + 0.45 * sc, x, y - 0.45 * sc)
  polyv(false, x - 0.3 * sc, y - 0.15 * sc, x, y - 0.45 * sc, x + 0.3 * sc, y - 0.15 * sc)
end
local function book(x0)
  rect(-0.75, -0.75, 0.75, 0.8, 0.12)
  line(-0.45 + (x0 or 0), -0.75, -0.45 + (x0 or 0), 0.8)
end
local function noEntry(x, y, r)
  circle(x, y, r)
  line(x - r * 0.7, y + r * 0.7, x + r * 0.7, y - r * 0.7)
end

local GLOBE = {} -- the globe's meridian
for i = 0, 23 do
  local a = i * math.pi / 12
  GLOBE[#GLOBE + 1] = 0.4 * math.cos(a)
  GLOBE[#GLOBE + 1] = 0.85 * math.sin(a)
end
local BUG_LEGS, SCALE_PANS, AGGRO_TICKS = { -0.1, 0.25, 0.6 }, { -1, 1 }, { 225, 270, 315 }

local ICONS = {
  compass = function()
    circle(0, 0, 0.85)
    tri(0, -0.62, 0.22, 0, -0.22, 0)
    polyv(false, 0.22, 0, 0, 0.62, -0.22, 0)
  end,
  find = function()
    circle(-0.18, -0.18, 0.52)
    line(0.2, 0.2, 0.8, 0.8)
  end,
  map = function()
    polyv(true, -0.85, -0.6, -0.3, -0.85, 0.3, -0.6, 0.85, -0.85, 0.85, 0.6, 0.3, 0.85, -0.3, 0.6, -0.85, 0.85)
    line(-0.3, -0.85, -0.3, 0.6)
    line(0.3, -0.6, 0.3, 0.85)
  end,
  group = function()
    person(-0.38, 0.05, 0.75)
    person(0.38, 0.05, 0.75)
  end,
  players = function()
    person(-0.5, 0.15, 0.6)
    person(0.5, 0.15, 0.6)
    person(0, -0.05, 0.75)
  end,
  person = function() person(0, 0.05, 1) end,
  guild = function()
    polyv(true, -0.7, -0.8, 0.7, -0.8, 0.7, 0.05, 0, 0.88, -0.7, 0.05)
    line(0, -0.8, 0, 0.88)
  end,
  raid = function()
    line(-0.6, -0.9, -0.6, 0.9)
    tri(-0.6, -0.85, 0.8, -0.45, -0.6, -0.05)
  end,
  friends = function()
    disc(-0.3, -0.25, 0.36)
    disc(0.3, -0.25, 0.36)
    tri(-0.64, -0.12, 0.64, -0.12, 0, 0.72)
  end,
  swords = function()
    line(-0.75, -0.75, 0.6, 0.6)
    line(0.75, -0.75, -0.6, 0.6)
    line(0.25, 0.65, 0.65, 0.25)
    line(-0.25, 0.65, -0.65, 0.25)
  end,
  target = function()
    circle(0, 0, 0.6)
    line(0, -0.95, 0, -0.35) line(0, 0.35, 0, 0.95)
    line(-0.95, 0, -0.35, 0) line(0.35, 0, 0.95, 0)
    disc(0, 0, 0.12)
  end,
  xtarget = function()
    circle(-0.2, 0, 0.5)
    disc(-0.2, 0, 0.12)
    line(0.45, -0.55, 0.9, -0.55) line(0.45, 0, 0.9, 0) line(0.45, 0.55, 0.9, 0.55)
  end,
  hotkeys = function()
    rect(-0.8, -0.8, -0.1, -0.1, 0.12) rect(0.1, -0.8, 0.8, -0.1, 0.12)
    rect(-0.8, 0.1, -0.1, 0.8, 0.12) rect(0.1, 0.1, 0.8, 0.8, 0.12)
  end,
  bandolier = function()
    line(-0.9, 0.55, 0.9, -0.55)
    for i = -1, 1 do fill(i * 0.45 - 0.12, -i * 0.28 - 0.25, i * 0.45 + 0.12, -i * 0.28 + 0.2, 0.05) end
  end,
  bolt = function()
    tri(0.25, -0.95, -0.55, 0.15, 0.05, 0.1)
    tri(-0.05, -0.1, 0.55, -0.15, -0.25, 0.95)
  end,
  alarm = function()
    circle(0, 0.08, 0.72)
    line(0, 0.08, 0, -0.38) line(0, 0.08, 0.32, 0.25)
    line(-0.85, -0.6, -0.55, -0.88) line(0.85, -0.6, 0.55, -0.88)
  end,
  expedition = function()
    polyv(true, -0.95, 0.75, -0.3, -0.35, 0.05, 0.2, 0.4, -0.7, 0.95, 0.75)
    polyv(false, 0.2, -0.38, 0.4, -0.2, 0.6, -0.38)
  end,
  journal = function()
    book()
    line(-0.15, -0.35, 0.45, -0.35) line(-0.15, 0, 0.45, 0) line(-0.15, 0.35, 0.3, 0.35)
  end,
  quests = function()
    circle(0, 0, 0.85)
    fill(-0.09, -0.55, 0.09, 0.2, 0.05)
    disc(0, 0.45, 0.11)
  end,
  card = function()
    rect(-0.9, -0.65, 0.9, 0.65, 0.15)
    circle(-0.4, -0.12, 0.2)
    arc(-0.4, 0.5, 0.35, 180, 360)
    line(0.1, -0.25, 0.65, -0.25) line(0.1, 0.05, 0.65, 0.05) line(0.1, 0.35, 0.5, 0.35)
  end,
  pet = function()
    disc(0, 0.38, 0.38)
    disc(-0.55, -0.12, 0.18) disc(-0.2, -0.5, 0.18) disc(0.2, -0.5, 0.18) disc(0.55, -0.12, 0.18)
  end,
  gem = function()
    polyv(true, -0.45, -0.7, 0.45, -0.7, 0.85, -0.2, 0, 0.85, -0.85, -0.2)
    line(-0.85, -0.2, 0.85, -0.2)
    polyv(false, -0.3, -0.2, 0, 0.85, 0.3, -0.2)
  end,
  spellbook = function()
    book()
    star(0.15, 0.02, 0.38)
  end,
  aa = function() star(0, 0.05, 0.9) end,
  buffs = function()
    circle(0, 0, 0.85)
    upArrow(0, 0, 1)
  end,
  shortbuffs = function()
    polyv(true, -0.6, -0.85, 0.6, -0.85, -0.6, 0.85, 0.6, 0.85)
    tri(-0.3, 0.6, 0.3, 0.6, 0, 0.2)
  end,
  leadership = function()
    polyv(true, -0.85, 0.6, -0.85, -0.5, -0.4, -0.05, 0, -0.75, 0.4, -0.05, 0.85, -0.5, 0.85, 0.6)
  end,
  skills = function()
    fill(-0.8, 0.1, -0.4, 0.85, 0.06)
    fill(-0.2, -0.3, 0.2, 0.85, 0.06)
    fill(0.4, -0.8, 0.8, 0.85, 0.06)
  end,
  blocked = function() noEntry(0, 0, 0.82) end,
  blockedpet = function()
    noEntry(0, 0, 0.82)
    disc(0, 0.18, 0.2)
  end,
  auras = function()
    disc(0, 0, 0.2)
    circle(0, 0, 0.5)
    ca = ca * 0.55
    circle(0, 0, 0.88)
  end,
  character = function()
    person(0, 0.05, 1)
  end,
  inventory = function()
    rect(-0.75, -0.35, 0.75, 0.85, 0.25)
    arc(0, -0.35, 0.38, 180, 360)
    line(-0.75, 0.15, 0.75, 0.15)
  end,
  advinventory = function() -- the bag, and a lens over its corner (the Advanced Inventory)
    rect(-0.85, -0.35, 0.45, 0.75, 0.25)
    arc(-0.2, -0.35, 0.34, 180, 360)
    circle(0.42, 0.42, 0.3)
    line(0.63, 0.63, 0.9, 0.9)
  end,
  options = function()
    circle(0, 0, 0.45)
    disc(0, 0, 0.12)
    for i = 0, 7 do
      local a = math.rad(i * 45)
      line(math.cos(a) * 0.55, math.sin(a) * 0.55, math.cos(a) * 0.9, math.sin(a) * 0.9)
    end
  end,
  camp = function()
    arc(0, 0.08, 0.75, -50, 230)
    line(0, -0.9, 0, -0.05)
  end,
  globe = function() -- the web browser
    circle(0, 0, 0.85)
    line(-0.85, 0, 0.85, 0)
    poly(GLOBE, true)
  end,
  bug = function()
    rect(-0.42, -0.35, 0.42, 0.85, 0.42) -- the body
    circle(0, -0.58, 0.24)              -- the head
    line(0, -0.35, 0, 0.85)
    for _, y in ipairs(BUG_LEGS) do
      line(-0.42, y, -0.85, y - 0.15)
      line(0.42, y, 0.85, y - 0.15)
    end
  end,
  abilitybook = function()
    book()
    line(-0.1, 0.5, 0.55, -0.4)  -- a sword on the page
    line(-0.08, 0.17, 0.18, 0.37)
  end,
  aggro = function()
    arc(0, 0.35, 0.8, 180, 360)
    line(-0.8, 0.35, 0.8, 0.35)
    for _, a in ipairs(AGGRO_TICKS) do
      local r = math.rad(a)
      line(math.cos(r) * 0.6, 0.35 + math.sin(r) * 0.6, math.cos(r) * 0.8, 0.35 + math.sin(r) * 0.8)
    end
    line(0, 0.35, 0.38, -0.2)
    disc(0, 0.35, 0.12)
  end,
  help = function()
    circle(0, 0, 0.85)
    arc(0, -0.2, 0.27, 180, 405)
    line(0.19, -0.01, 0, 0.18)
    disc(0, 0.45, 0.1)
  end,
  mail = function()
    rect(-0.85, -0.6, 0.85, 0.6, 0.12)
    polyv(false, -0.85, -0.55, 0, 0.1, 0.85, -0.55)
  end,
  mercenary = function() -- a helmet
    arc(0, 0.05, 0.72, 180, 360)
    polyv(false, -0.72, 0.05, -0.72, 0.55, -0.3, 0.88)
    polyv(false, 0.72, 0.05, 0.72, 0.55, 0.3, 0.88)
    line(-0.55, 0.2, -0.15, 0.2) line(0.15, 0.2, 0.55, 0.2)
    fill(-0.07, -0.05, 0.07, 0.62, 0.04)
  end,
  npcjournal = function()
    book()
    rect(-0.2, -0.5, 0.55, 0.1, 0.18) -- a speech bubble on the page
    polyv(false, -0.05, 0.1, -0.1, 0.38, 0.15, 0.1)
  end,
  pvp = function() -- a trophy
    line(-0.55, -0.8, 0.55, -0.8)
    arc(0, -0.8, 0.55, 0, 180)
    arc(-0.55, -0.55, 0.22, 90, 270)
    arc(0.55, -0.55, 0.22, -90, 90)
    line(0, -0.25, 0, 0.45)
    rect(-0.45, 0.45, 0.45, 0.78, 0.1)
  end,
  story = function() -- a scroll
    rect(-0.55, -0.62, 0.55, 0.62, 0.02)
    rect(-0.75, -0.88, 0.75, -0.62, 0.5)
    rect(-0.75, 0.62, 0.75, 0.88, 0.5)
    line(-0.3, -0.3, 0.3, -0.3) line(-0.3, 0, 0.3, 0) line(-0.3, 0.3, 0.15, 0.3)
  end,
  tribute = function() -- a stack of coins
    rect(-0.75, 0.42, 0.65, 0.82, 0.5)
    rect(-0.6, -0.02, 0.8, 0.38, 0.5)
    rect(-0.75, -0.46, 0.65, -0.06, 0.5)
    rect(-0.6, -0.9, 0.8, -0.5, 0.5)
  end,
  bazaar = function() -- scales
    line(0, -0.75, 0, 0.7)
    line(-0.75, -0.5, 0.75, -0.5)
    line(-0.4, 0.7, 0.4, 0.7)
    for _, d in ipairs(SCALE_PANS) do
      local x = d * 0.58
      line(x, -0.5, x - 0.28, 0.08) line(x, -0.5, x + 0.28, 0.08)
      arc(x, 0.08, 0.3, 0, 180)
    end
  end,
  barter = function() -- two arrows, one each way
    line(-0.8, -0.3, 0.75, -0.3)
    polyv(false, 0.45, -0.6, 0.8, -0.3, 0.45, 0)
    line(0.8, 0.35, -0.75, 0.35)
    polyv(false, -0.45, 0.05, -0.8, 0.35, -0.45, 0.65)
  end,
  lfgroup = function()
    person(-0.25, 0.05, 0.85)
    line(0.6, -0.4, 0.6, 0.1) line(0.35, -0.15, 0.85, -0.15)
  end,
  lfguild = function()
    polyv(true, -0.85, -0.85, 0.15, -0.85, 0.15, -0.15, -0.35, 0.5, -0.85, -0.15)
    circle(0.35, 0.25, 0.3)
    line(0.56, 0.46, 0.85, 0.75)
  end,
  marketplace = function() -- a shop front under a scalloped awning
    polyv(false, -0.85, -0.45, -0.7, -0.85, 0.7, -0.85, 0.85, -0.45)
    for i = 0, 3 do arc(-0.6375 + i * 0.425, -0.45, 0.2125, 0, 180) end
    line(-0.7, -0.24, -0.7, 0.85) line(0.7, -0.24, 0.7, 0.85)
    line(-0.85, 0.85, 0.85, 0.85)
    rect(-0.5, 0.05, -0.1, 0.45, 0.06)
    rect(0.1, 0.2, 0.45, 0.85, 0.06)
  end,
  house = function() -- real estate
    polyv(false, -0.9, -0.02, 0, -0.85, 0.9, -0.02)
    polyv(false, -0.65, -0.25, -0.65, 0.85, 0.65, 0.85, 0.65, -0.25)
    rect(-0.18, 0.3, 0.18, 0.85, 0.05)
  end,
  key = function() -- managing a plot
    circle(-0.45, 0, 0.32)
    line(-0.13, 0, 0.9, 0)
    line(0.55, 0, 0.55, 0.32) line(0.8, 0, 0.8, 0.25)
  end,
  pin = function() -- a plot search: a map pin over the ground
    arc(0, -0.3, 0.45, 150, 390)
    line(-0.39, -0.075, 0, 0.6) line(0.39, -0.075, 0, 0.6)
    circle(0, -0.3, 0.15)
    line(-0.65, 0.82, 0.65, 0.82)
  end,
  chair = function() -- the items placed on a plot
    line(-0.45, -0.85, -0.45, 0.85)
    line(-0.45, -0.4, 0.05, -0.4)
    fill(-0.45, 0.02, 0.6, 0.18, 0.04)
    line(0.5, 0.18, 0.5, 0.85)
  end,
  plug = function() -- the plugins' submenu
    rect(-0.45, -0.38, 0.45, 0.32, 0.15)
    line(-0.2, -0.38, -0.2, -0.85) line(0.2, -0.38, 0.2, -0.85)
    line(0, 0.32, 0, 0.9)
  end,
  script = function() -- the Lua scripts window: a console with a prompt
    rect(-0.85, -0.7, 0.85, 0.7, 0.15)
    polyv(false, -0.5, -0.3, -0.15, 0, -0.5, 0.3)
    line(0.05, 0.32, 0.5, 0.32)
  end,
  misc = function()
    disc(-0.55, 0, 0.14) disc(0, 0, 0.14) disc(0.55, 0, 0.14)
  end,
  dot = function() disc(0, 0, 0.22) end,
}

-- menu row text -> icon
local BY_TEXT = {
  ["Compass"] = "compass", ["Find"] = "find", ["Navigation"] = "map",
  ["Group Status"] = "group", ["Guild"] = "guild", ["Raid"] = "raid", ["Friends/Ignore"] = "friends",
  ["Players"] = "players",
  ["Combat Abilities"] = "swords", ["Target"] = "target", ["Extended Target"] = "xtarget",
  ["Bandolier"] = "bandolier", ["Actions"] = "bolt",
  ["Alarm"] = "alarm",
  ["Expeditions"] = "expedition", ["Quest Journal"] = "journal", ["Quests"] = "quests",
  ["Player Information"] = "card", ["Pet"] = "pet", ["Spell Gems"] = "gem", ["Spell Book"] = "spellbook",
  ["Alternate Advancement"] = "aa", ["Buffs"] = "buffs", ["Short Duration Buffs"] = "shortbuffs",
  ["Leadership"] = "leadership", ["Skills"] = "skills", ["Blocked Buffs"] = "blocked",
  ["Blocked Pet Buffs"] = "blockedpet", ["Auras"] = "auras", ["Character"] = "character",
  ["Inventory"] = "inventory", ["Advanced Inventory"] = "advinventory", ["Options"] = "options", ["Camp & Quit"] = "camp",
  ["Layout Profiles"] = "hotkeys",
  ["Report a Bug"] = "bug", ["Web Browser"] = "globe", ["Lua Scripts"] = "script", ["Ability Book"] = "abilitybook", ["Aggro Meter"] = "aggro",
  ["Help"] = "help", ["Everquest Help"] = "help", ["Mail"] = "mail", ["Mercenaries"] = "mercenary",
  ["NPC Journal"] = "npcjournal", ["PvP Stats"] = "pvp", ["Storyline"] = "story", ["Tribute"] = "tribute",
  ["Bazaar Search"] = "bazaar", ["Barter Search"] = "barter", ["Looking for Group"] = "lfgroup",
  ["Looking for Guild"] = "lfguild", ["Map"] = "map", ["Atlas/Map"] = "map", ["Miscellaneous"] = "misc",
  ["Marketplace"] = "marketplace", ["Real Estate"] = "house", ["Manage"] = "key", ["Plot Search"] = "pin",
  ["Real Estate Items"] = "chair", ["Plugins"] = "plug",
  -- the same windows under other skins' and later clients' row texts
  ["Actions Window"] = "bolt", ["Alarm Window"] = "alarm", ["Group Window"] = "group",
  ["Player Info Window"] = "card", ["Buffs (Short Duration)"] = "shortbuffs", ["Instance Information"] = "expedition",
}

function M.Name(text)
  if BY_TEXT[text] then return BY_TEXT[text] end
  if text and text:match("^Hotkeys") then return "hotkeys" end
  return "dot"
end

-- a hot button bar's row: a key cap with the bar's number
local function keycap(n)
  rect(-0.9, -0.8, 0.9, 0.8, 0.3)
  M.PushFont()
  local tw, th = ui.CalcTextSize(n)
  ui.DrawText(cx - tw / 2, cy - th / 2, cr, cg, cb, ca, n)
  M.PopFont()
end

function M.Draw(text, x, y, d, color, alpha)
  cx, cy, k = x, y, d / 2
  cr, cg, cb, ca = color[1], color[2], color[3], alpha or 1
  lw = math.max(1.2, d / 12)
  local bar = text and text:match("^Hotkeys (%d+)$")
  if bar then return keycap(bar) end
  ICONS[M.Name(text)]()
end

return M
