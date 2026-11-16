-- The modern UI's default layout: where each HUD frame (lib/hud.lua hud.Window) sits until the player moves it, on
-- any screen size and UI scale. Pure Lua (no ui.* or hs.*), so tests/lua_scripts_test.cpp runs it at many sizes.
--
--   local layout = dofile("lib/layout.lua")
--   local rects = layout.Solve(W, H, s, frames)
--     W, H     the screen, pixels
--     s        the HUD's scale (the display's UI scale times the HUD scale): margins and gaps grow with it
--     frames   window -> { w = , h = } the frames drawn now, on screen (pixels); a frame not listed is not shown
--              (layout.reserve lists frames that keep their room anyway). Chat1 with fit = true is sized here.
--   rects[window] = { x =, y =, w =, h = } for every frame listed that the layout knows; rects.trayRoom is the
--   width the buff trays have at the top right (lib/bufftray.lua wraps its icons to fit).
--
-- The frames go in a fixed order, each to the first of its candidate places that is on screen and clear of the
-- frames placed before it (none clear: the clear place beside a placed frame nearest its first; none of those: the
-- candidate that covers the least). The layout ignores where the player has moved frames: "Reset position" always
-- brings a frame back to the same place, and a moved frame leaves its room empty. From the bottom middle up: the hot
-- bars, the spell gems, the player and target frames side by side, the air bar. The chat at the bottom
-- left beside them (the middle stack moves right when the screen is too narrow for both); the compass at the top
-- middle; the minimap at the top right with the buffs and songs to its left and the extended targets under it; the
-- group at the left; the menu button in the bottom right corner. Frames added by plugins or later scripts that are
-- not listed here keep the place their script gives (hud.Window's opts.place).
local L = {}

L.MARGIN, L.GAP = 12, 8 -- design pixels: from the screen's edges, between frames

-- Frames that count as their size here (design pixels) when smaller or not shown, so that one appearing (a target,
-- the first buff, a fifth group member) does not move the frames placed after it.
L.reserve = {
  TargetWindow = { 300, 64 }, -- target.lua, without the target's target, aggro or buffs
  BuffWindow = { 38, 56 }, -- one icon of lib/bufftray.lua at its 34 pixels
  GroupWindow = { 210, 358 }, -- group.lua: five members, each with a pet
}

L.CHAT_W, L.CHAT_H, L.CHAT_MIN_W = 470, 250, 300

local HOTBARS = { "HotButtonWnd" }
for n = 2, 10 do HOTBARS[n] = "HotButtonWnd" .. n end

local function overlap(a, b)
  local w = math.min(a.x + a.w, b.x + b.w) - math.max(a.x, b.x)
  local h = math.min(a.y + a.h, b.y + b.h) - math.max(a.y, b.y)
  if w <= 0 or h <= 0 then return 0 end
  return w * h
end

function L.Overlap(a, b) return overlap(a, b) end

function L.Solve(W, H, s, frames)
  local m, g = L.MARGIN * s, L.GAP * s
  local out, placed = {}, {}

  -- a frame's size for the layout: as drawn, at least its reserve; nil when it neither shows nor keeps its room
  local function size(key)
    local f, r = frames[key], L.reserve[key]
    if not f and not r then return nil end
    local w, h = f and f.w or 0, f and f.h or 0
    if r then w, h = math.max(w, r[1] * s), math.max(h, r[2] * s) end
    return w, h
  end

  local function put(key, x, y, w, h)
    local r = { x = x, y = y, w = w, h = h }
    out[key] = r
    placed[#placed + 1] = r
    return r
  end

  -- how much of the frames placed so far a w x h frame at x, y covers (on screen: 0 is clear), and the place clamped
  -- on screen (a clamped place is not clear)
  local function cover(x, y, w, h)
    local cx, cy = math.max(0, math.min(x, W - w)), math.max(0, math.min(y, H - h))
    local r, c = { x = cx, y = cy, w = w, h = h }, 0
    for _, p in ipairs(placed) do c = c + overlap(r, p) end
    if cx ~= x or cy ~= y then c = c + 0.5 end
    return c, cx, cy
  end

  -- the first candidate {x, y} that is clear; else the clear place beside a placed frame (at its sides, top or
  -- bottom, aligned with either edge) nearest the first candidate; else the candidate covering the least
  local function choose(key, w, h, candidates)
    local best, bx, by = math.huge, nil, nil
    for _, c in ipairs(candidates) do
      local k, x, y = cover(c[1], c[2], w, h)
      if k == 0 then return put(key, x, y, w, h) end
      if k < best then best, bx, by = k, x, y end
    end
    if not bx then return nil end
    local px, py = candidates[1][1], candidates[1][2]
    local near, nx, ny = math.huge, nil, nil
    for _, p in ipairs(placed) do
      for _, c in ipairs({
        { p.x + p.w + g, p.y }, { p.x + p.w + g, p.y + p.h - h }, { p.x - g - w, p.y }, { p.x - g - w, p.y + p.h - h },
        { p.x, p.y + p.h + g }, { p.x + p.w - w, p.y + p.h + g }, { p.x, p.y - g - h }, { p.x + p.w - w, p.y - g - h },
      }) do
        local d = (c[1] - px) ^ 2 + (c[2] - py) ^ 2
        if d < near and cover(c[1], c[2], w, h) == 0 then near, nx, ny = d, c[1], c[2] end
      end
    end
    if nx then return put(key, nx, ny, w, h) end
    return put(key, bx, by, w, h)
  end

  -- under the frames of the top right corner placed so far over the columns x0..x1
  local corner = { "Minimap", "BuffWindow", "ShortDurationBuffWindow", "AuraWindow", "ExtendedTargetWnd" }
  local function below(x0, x1)
    local y = m
    for _, key in ipairs(corner) do
      local p = out[key]
      if p and p.x < x1 and x0 < p.x + p.w then y = math.max(y, p.y + p.h + g) end
    end
    return y
  end

  -- the middle stack ------------------------------------------------------------------------------------------------
  local stack = {} -- bottom up: { key, w, h }
  for _, key in ipairs(HOTBARS) do
    local w, h = size(key)
    if w then stack[#stack + 1] = { key, w, h } end
  end
  local gw, gh = size("CastSpellWnd")
  local gemsVertical = gw and gh > gw
  if gw and not gemsVertical then stack[#stack + 1] = { "CastSpellWnd", gw, gh } end
  local pw, ph = size("PlayerWindow")
  local tw, th = size("TargetWindow")
  local half = ((pw or 0) + g + tw) / 2
  for _, f in ipairs(stack) do half = math.max(half, f[2] / 2) end

  -- the menu button first: the stack keeps clear of the bottom right corner
  local mw, mh = size("EQMainWnd")
  local menuW = 0
  if mw then
    put("EQMainWnd", W - m - mw, H - m - mh, mw, mh)
    menuW = mw + g
  end

  -- centred, unless the chat would not fit beside it: then as far right as the menu button lets it go
  local cx = W / 2
  local chatW = frames.Chat1 and (frames.Chat1.fit and L.CHAT_MIN_W * s or frames.Chat1.w) or 0
  if frames.Chat1 then
    local low, high = m + chatW + g + half, W - m - menuW - half
    if cx < low and low <= high then cx = low end
  end
  cx = math.floor(cx + 0.5)

  local y = H - m
  for _, f in ipairs(stack) do
    put(f[1], math.floor(cx - f[2] / 2 + 0.5), y - f[3], f[2], f[3])
    y = y - f[3] - g
  end
  local unitBottom = y
  local unitTop = unitBottom
  if pw then
    local r = put("PlayerWindow", cx - g / 2 - pw, unitBottom - ph, pw, ph)
    unitTop = math.min(unitTop, r.y)
  end
  local tr = put("TargetWindow", cx + g / 2, unitBottom - th, tw, th)
  unitTop = math.min(unitTop, tr.y)
  local stackLeft, stackTop = W, unitTop
  for _, p in ipairs(placed) do
    if p ~= out.EQMainWnd then stackLeft = math.min(stackLeft, p.x) end
  end

  -- the chat: bottom left, as wide as the room beside the stack allows ---------------------------------------------
  if frames.Chat1 then
    local w, h = frames.Chat1.w, frames.Chat1.h
    if frames.Chat1.fit then
      h = math.min(L.CHAT_H * s, H * 0.36)
      -- a chat rolled up (its tab strip only) keeps just that room; the rest of its height is the screen's again
      if frames.Chat1.rolled then h = frames.Chat1.h end
      w = math.max(L.CHAT_MIN_W * s, math.min(L.CHAT_W * s, stackLeft - g - m))
    end
    choose("Chat1", w, h, { { m, H - m - h }, { m, stackTop - g - h }, { m, H / 2 - h / 2 } })
  end

  -- the top ---------------------------------------------------------------------------------------------------------
  local right = W - m -- the right edge of the column under the top right corner's frames
  local fw, fh = size("CompassWindow")
  if fw then choose("CompassWindow", fw, fh, { { W / 2 - fw / 2, m } }) end
  fw, fh = size("Minimap")
  if fw then
    local r = choose("Minimap", fw, fh, { { W - m - fw, m } })
    if r.x > W / 2 then right = r.x - g end
  end
  -- the buff trays: right-aligned left of the minimap, as wide as the room up to the compass; where fewer than eight
  -- of the buffs' icons fit there (lib/bufftray.lua: 34 pixels, 4 apart, 12 of padding), under the compass with the
  -- room for fourteen
  local compass = out.CompassWindow
  local left = compass and (compass.x + compass.w + g) or (W / 2)
  local trayTop = m
  out.trayRoom = math.max(0, right - left)
  if out.trayRoom < (8 * 38 + 8) * s then
    out.trayRoom = math.max((8 * 38 + 8) * s, math.min(right - m, (14 * 38 + 8) * s))
    if compass then trayTop = compass.y + compass.h + g end
  end
  local trayRight = right
  fw, fh = size("BuffWindow")
  if fw then
    local r = choose("BuffWindow", fw, fh, { { right - fw, trayTop }, { W - m - fw, below(W - m - fw, W - m) } })
    trayRight = r.x + r.w
  end
  fw, fh = size("ShortDurationBuffWindow")
  if fw then
    choose("ShortDurationBuffWindow", fw, fh, { { trayRight - fw, below(trayRight - fw, trayRight) },
      { W - m - fw, below(W - m - fw, W - m) } })
  end
  fw, fh = size("AuraWindow")
  if fw then
    choose("AuraWindow", fw, fh, { { trayRight - fw, below(trayRight - fw, trayRight) },
      { W - m - fw, below(W - m - fw, W - m) }, { W - m - fw, H / 2 - fh / 2 } })
  end
  fw, fh = size("ExtendedTargetWnd")
  if fw then
    choose("ExtendedTargetWnd", fw, fh, { { W - m - fw, below(W - m - fw, W - m) }, { W - m - fw, H * 0.34 },
      { trayRight - fw, below(trayRight - fw, trayRight) } })
  end

  -- the left ----------------------------------------------------------------------------------------------------------
  fw, fh = size("GroupWindow")
  if fw then choose("GroupWindow", fw, fh, { { m, math.max(m, math.floor(H * 0.12)) }, { m, m } }) end

  -- beside the stack --------------------------------------------------------------------------------------------------
  -- over the frames placed so far in the screen's lower two thirds over the columns x0..x1
  local function above(x0, x1)
    local y = H - m
    for _, p in ipairs(placed) do
      if p.x < x1 and x0 < p.x + p.w and p.y + p.h > H / 3 then y = math.min(y, p.y - g) end
    end
    return y
  end
  fw, fh = size("BreathWindow")
  if fw then
    local x = cx - fw / 2
    choose("BreathWindow", fw, fh, { { x, unitTop - g - fh }, { x, above(x, x + fw) - fh } })
  end
  local player = out.PlayerWindow
  fw, fh = size("PetInfoWindow")
  if fw then
    local c = {}
    if player then
      c[#c + 1] = { player.x - g - fw, unitBottom - fh }
      if out.Chat1 then c[#c + 1] = { player.x - g - fw, math.min(unitBottom, out.Chat1.y - g) - fh } end
      c[#c + 1] = { player.x, above(player.x, player.x + fw) - fh }
    end
    c[#c + 1] = { stackLeft - g - fw, unitBottom - fh }
    choose("PetInfoWindow", fw, fh, c)
  end
  if gemsVertical then
    choose("CastSpellWnd", gw, gh, { { stackLeft - g - gw, unitBottom - gh }, { m, H / 2 - gh / 2 } })
  end
  fw, fh = size("PotionBeltWnd")
  if fw then
    local c = {}
    for _, key in ipairs({ "HotButtonWnd", "CastSpellWnd", "TargetWindow" }) do
      local r = out[key]
      if r then c[#c + 1] = { r.x + r.w + g, r.y + r.h - fh } end
    end
    c[#c + 1] = { tr.x, unitTop - g - fh }
    choose("PotionBeltWnd", fw, fh, c)
  end

  for _, p in pairs(out) do
    if type(p) == "table" then p.x, p.y = math.floor(p.x + 0.5), math.floor(p.y + 0.5) end
  end
  return out
end

-- The default place of a w x h frame from a solve, or nil for a frame the layout does not place. A chat window after
-- the first goes on the first's, each a little up and to the right (bottom edges in line).
function L.Place(rects, key, w, h, s)
  local r = rects[key]
  if r then return r.x, r.y end
  local slot = tonumber(key:match("^Chat(%d+)$"))
  if slot and slot > 1 and rects.Chat1 then
    local d = (slot - 1) * 28 * s
    return rects.Chat1.x + d, rects.Chat1.y + rects.Chat1.h - h - d
  end
  return nil
end

return L
