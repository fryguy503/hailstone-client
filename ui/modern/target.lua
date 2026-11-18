-- Target frame: replaces the game's TargetWindow. The target's name in its con colour, its health, its target (the
-- target of target, when the server sends it), the aggro meter (the AggroMeterWnd's values: our share of the
-- target's hate, gauge 149 / label 305, and the most hated besides us, labels 303 / 307; aggro.lua keeps the stock
-- window from showing) and its buffs under the frame, each with the buff's menu on a right click (lib/buffmenu.lua:
-- Spell Info). Hidden while nothing is targeted.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local buffmenu = dofile("lib/buffmenu.lua")
local T = hud.T
local C = T.color
local WINDOW = "TargetWindow"
local MAX_BUFFS = 85

-- The con colour the game tints the target box with (nil: the target does not con, yourself or a corpse).
local function conColor()
  if API.windows.ChildShown(WINDOW, "A_TargetBoxStaticAnimTop") ~= true then return nil end
  local r, g, b = API.windows.ChildColor(WINDOW, "A_TargetBoxStaticAnimTop", "decal")
  if not r then r, g, b = API.windows.ChildColor(WINDOW, "A_TargetBoxStaticAnimTop") end
  if r then return { r, g, b, 1 } end
  return nil
end

-- The target's buffs (a record each, kept from frame to frame), n of them; ids and hit ids built once.
local IDS, HITS = {}, {}
for i = 0, MAX_BUFFS - 1 do
  IDS[i + 1] = "Buff" .. i
  HITS[i + 1] = "##tbBuff" .. i
end
local buffs, nBuffs = {}, 0

local function targetBuffs()
  nBuffs = 0
  for i = 1, MAX_BUFFS do
    local spell, ms, name, good = API.buffs.Slot(WINDOW, IDS[i])
    if spell then
      nBuffs = nBuffs + 1
      local b = buffs[nBuffs]
      if not b then
        b = {}
        buffs[nBuffs] = b
      end
      b.id, b.hit, b.spell, b.ms, b.name, b.good = IDS[i], HITS[i], spell, ms, name, good
      -- the time left on it (T.Duration), formatted again only when what it shows changes
      local key = (not ms or ms < 0 or ms >= 100 * 3600000) and -1 or math.floor(ms / 1000)
      if b.durKey ~= key then b.durKey, b.duration = key, T.Duration(ms) end
    end
  end
end

local ICON, GAP, PER_ROW = 22, 3, 12

-- What the frame shows this frame, read once it is known to show (hideWhen and size: hud.Window calls them first).
local name, frame = "", -1
local tot, totText, totRaw, hasTot, aggro, aggroOn, top, rows

local function read()
  local f = ui.GetFrameCount()
  if f == frame then return end
  frame = f
  name = API.character.Label(28) or ""
  if name ~= "" then targetBuffs() else nBuffs = 0 end
  tot, totText, totRaw = API.character.Gauge(27, true)
  hasTot = totRaw ~= nil and totRaw ~= -1 and totText and totText ~= ""
  rows = math.ceil(nBuffs / PER_ROW)
  -- the aggro meter, while it is on this target (the server sends it for the player's target or the locked one)
  local a, _, aggroRaw = API.character.Gauge(149)
  aggro = a
  aggroOn = name ~= "" and aggroRaw ~= nil and aggroRaw ~= -1 and (API.character.Label(302) or "") == name
  top = (64 + (hasTot and 20 or 0) + (aggroOn and 32 or 0))
end

local opts = {
  size = function()
    read()
    return 300, top + (rows > 0 and (rows * (ICON + GAP) + 6) or 0)
  end,
  hideWhen = function()
    read()
    return name == ""
  end,
  panel = false,
}

-- the drawing calls' options and colours, made once and filled each frame (the calls read them and keep nothing)
local GLOW = { 0, 0, 0, 0.55 }
local PANEL_OPTS = { glow = nil }
local HP_BAR, TOT_BAR = { center = nil, font = nil }, { right = nil, font = nil }
local NO_TRAIL = { noTrail = true }
local BUFF_ICON = { radius = 4 }

local function body(x, y, w, h, s)
  local con = conColor()
  local frameH = top * s
  if con then GLOW[1], GLOW[2], GLOW[3] = con[1], con[2], con[3] end
  PANEL_OPTS.glow = con and GLOW or nil
  T.Panel(x, y, x + w, y + frameH, s, PANEL_OPTS)
  local pad = 10 * s
  local frac, _, raw = API.character.Gauge(6)
  local caption = API.character.Label(28, true) or name
  local pct = API.character.Label(29) or ""
  -- name and the con dot
  local nx = x + pad
  if con then
    ui.DrawCircleFilled(nx + 5 * s, y + pad + 8 * s, 5 * s, con[1], con[2], con[3], 1)
    nx = nx + 15 * s
  end
  local _, nh = T.TextSize("Ag", T.font.normal)
  T.Text(nx, y + pad - 2 * s, T.Fit(caption, x + w - pad - 52 * s - nx, T.font.normal), con or C.text, T.font.normal)
  if pct ~= "" then T.Text(x + w - pad, y + pad - 3 * s, pct .. "%", C.text, T.font.large, "right") end
  local by = y + pad + nh + 4 * s
  local dead = raw == -3
  HP_BAR.center, HP_BAR.font = dead and "Corpse" or nil, T.font.small
  T.Bar("target.hp", x + pad, by, x + w - pad, by + 18 * s, s, dead and 0 or frac, C.hp, HP_BAR)
  if T.Hit("##hp", x + pad, by, x + w - pad, by + 18 * s) then API.windows.ClickChild(WINDOW, "TargetHP", "left") end
  by = by + 22 * s
  if hasTot then
    local tf, tname = tot, totText
    local tpct = API.character.Label(120) or ""
    T.Text(x + pad, by - 1 * s, "\xC2\xBB", C.muted, T.font.small) -- the target's target
    local tx = x + pad + 12 * s
    local tw = (x + w - pad) - tx
    T.Text(tx, by - 2 * s, T.Fit(tname, tw * 0.5, T.font.small), C.muted, T.font.small)
    TOT_BAR.right, TOT_BAR.font = tpct ~= "" and (tpct .. "%") or nil, T.font.tiny
    T.Bar("target.tot", tx + tw * 0.52, by + 2 * s, x + w - pad, by + 10 * s, s, tf, C.hp, TOT_BAR)
  end
  if aggroOn then
    by = by + (hasTot and 18 or 0) * s
    ui.DrawLine(x + pad, by - 3 * s, x + w - pad, by - 3 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
    local mine = tonumber((API.character.Label(305))) or math.floor((aggro or 0) * 100 + 0.5)
    -- green while others hold it, yellow nearing, red at the top
    local col = mine > 99 and C.danger or (mine > 75 and C.endurance or C.pet) -- the game's 75 / 100 thresholds
    T.Text(x + pad, by, "Aggro", C.muted, T.font.tiny)
    local bx0 = x + pad + 44 * s
    T.Bar("target.aggro", bx0, by + 2 * s, x + w - pad - 40 * s, by + 10 * s, s, math.min(1, mine / 100), col,
      NO_TRAIL)
    T.Text(x + w - pad, by, mine .. "%", col, T.font.tiny, "right")
    local other, otherPct = API.character.Label(303) or "", API.character.Label(307) or ""
    if other ~= "" then
      T.Text(bx0, by + 13 * s, T.Fit("Top: " .. other, w * 0.6, T.font.tiny), C.faint, T.font.tiny)
      if otherPct ~= "" then T.Text(x + w - pad, by + 13 * s, otherPct .. "%", C.faint, T.font.tiny, "right") end
    end
    T.Hit("##aggro", x + pad, by, x + w - pad, by + 26 * s)
    if ui.IsItemHovered() then
      ui.SetTooltip(string.format("Your share of %s's hate: %d%%%s", name, mine,
        other ~= "" and string.format("\nMost hated besides you: %s (%s%%)", other, otherPct) or ""))
    end
  end
  -- buffs, under the frame
  local iy = y + frameH + 6 * s
  for i = 1, nBuffs do
    local b = buffs[i]
    local col, row = (i - 1) % PER_ROW, (i - 1) // PER_ROW
    local ix = x + col * (ICON + GAP) * s
    local yy = iy + row * (ICON + GAP) * s
    local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, b.id, "decal")
    if type(tex) ~= "userdata" then tex = nil end
    T.Icon(ix, yy, ICON * s, s, tex, u0, v0, u1, v1, b.good and C.good or C.bad, BUFF_ICON)
    T.Hit(b.hit, ix, yy, ix + ICON * s, yy + ICON * s, false)
    if ui.IsItemHovered() then
      ui.SetTooltip(b.name .. "\n" .. T.LongDuration(b.ms) .. "\n" .. buffmenu.Hint())
    end
    if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      buffmenu.Open(WINDOW, b)
      T.TakeRightClick()
    end
    if b.ms and b.ms >= 0 and b.ms < 60000 then
      T.Text(ix + ICON * s / 2, yy + ICON * s - 9 * s, b.duration, C.text, T.font.tiny, "center")
    end
  end
  buffmenu.Draw(WINDOW)
end

ui.window("Target", function()
  if not API.session.InGame() then return end
  hud.Window(WINDOW, "Target", opts, body)
end)
