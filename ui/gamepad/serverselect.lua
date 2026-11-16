-- The server select (gamepad pack): hs.ServerSelect stands in for the client's own (ui/dev_screens.cpp
-- server_select_screen). A Hailstone screen, not the original's. The world played last here comes first as a tile of
-- its own (the focus starts on it), then every world the login server lists, each with whether it can be joined (no
-- player count is shown). A plays the focused world, Start the selected one, B goes back to the login screen. With the
-- mouse a click selects a world and a double click plays it; Enter plays the selected one. Y (or a click on a row's
-- star) marks the world a favourite: the favourites are listed first, then the other worlds, each by name (the client
-- sorts them).
local API = hs.UI
local T = dofile("lib/theme.lua")
local W = dofile("lib/window.lua")
local P = dofile("lib/pregame.lua")
local C = T.C

local NAME = "Servers"
local ROW_H = 78
local st = { list = { offset = 0 }, frame = -10, clickAt = 0, clickRow = 0, note = nil, noteAt = 0, stars = {} }
local F = P.focus()

local function px(n) return T.px(n) end

-- The world's status on the right of a row ending at x1: the pill. Returns where it starts.
local function status(s, x1, cy)
  local c = s.status == "Locked" and C.gold or ((s.canJoin and s.status ~= "Down") and C.good or C.bad)
  local text = s.status ~= "" and s.status or (s.canJoin and "Up" or "Down")
  return x1 - P.pill(x1, cy, text, c, 1) - px(14)
end

-- A five-pointed star around (cx, cy): filled for a favourite, else its outline.
local pts = {} -- (its points, the tables kept from star to star)
for k = 1, 10 do pts[k] = { 0, 0 } end
local function star(cx, cy, r, filled, c, a)
  for k = 0, 9 do
    local ang = -math.pi / 2 + k * math.pi / 5
    local rr = k % 2 == 0 and r or r * 0.42
    local p = pts[k + 1]
    p[1], p[2] = cx + rr * math.cos(ang), cy + rr * math.sin(ang)
  end
  for k = 1, 10 do
    local p, q = pts[k], pts[k % 10 + 1]
    if filled then
      ui.DrawTriangleFilled(cx, cy, p[1], p[2], q[1], q[2], c[1], c[2], c[3], a)
    else
      ui.DrawLine(p[1], p[2], q[1], q[2], c[1], c[2], c[3], a, px(2))
    end
  end
end

local function play(v, i)
  if not API.pregame.ServerSelectDo("play", i) then
    local s = v.servers[i]
    if s and not s.canJoin then st.note, st.noteAt = s.name .. " cannot be joined now.", ui.GetTime() end
  end
end

-- A row's choice: with the pad A plays it; with the mouse a click selects it and a second click on it soon after plays.
local function chosen(v, i)
  local r = st.stars[i]
  if not P.padDrives() and r and ui.IsMouseHoveringRect(r[1], r[2], r[3], r[4]) then
    API.pregame.ServerSelectDo("favorite", i)
    st.clickRow = 0
    return
  end
  if P.padDrives() then
    play(v, i)
    return
  end
  local now = ui.GetTime()
  if st.clickRow == i and now - st.clickAt < 0.45 then
    st.clickRow = 0
    play(v, i)
  else
    API.pregame.ServerSelectDo("select", i)
    st.clickRow, st.clickAt = i, now
  end
end

-- (kept from frame to frame: tables made each frame are garbage the collector works through)
local OPTS = { w = 1040, subtitle = "Choose a world to play on", close = false, alwaysFocus = true,
  hints = { { "a", "Play" }, { "y", "Favorite" }, { "start", "Play selected" }, { "b", "Back" } } }
local held = nil -- the view drawn last frame: hs.ServerSelect gives true while it still holds (its revision)

ui.window(NAME, function()
  local v = API.pregame.ServerSelect and API.pregame.ServerSelect(held and held.revision)
  if v == true then v = held else held = v end
  if not v then
    st.frame = -10
    return
  end
  local frame = ui.GetFrameCount()
  local appearing = frame - st.frame > 1
  st.frame = frame
  local n = #v.servers
  if appearing then
    st.note, st.follow = nil, nil
    -- the focus starts on the world played last, else on the selected one
    if v.last > 0 then
      F:want("##last")
    else
      local at = v.selected > 0 and v.selected or 1
      st.list.offset = math.max(0, at - 4)
      st.list.focusNext = at - st.list.offset
    end
  end
  local title = v.server ~= "" and v.server or "Servers"
  -- (as tall as its worlds need, up to most of the screen)
  local rows = math.max(1, math.min(n, 8))
  local h = 110 + (v.last > 0 and 152 or 0) + 36 + rows * (ROW_H + 10) + 136 + 48 + 24 + 10
  OPTS.h, OPTS.title = math.min(1000, h), title
  if W.begin(NAME, OPTS) then
    local x0, y0, x1, y1, w = W.area()
    local busy = v.busy
    -- (the keyboard's Enter and Escape: only while this window has the focus and nothing is being typed)
    local keys = W.focused() and not ui.GetIO().typing
    if P.press("b") or (keys and ui.IsKeyPressed(ui.Key.Escape)) then API.pregame.ServerSelectDo("back") end
    if P.press("y") and v.selected > 0 and not st.follow then
      local sel = v.servers[v.selected]
      -- the list is sorted again with the world's new mark: the focus goes with the world to its new row
      if API.pregame.ServerSelectDo("favorite", v.selected) then
        st.follow = { name = sel.name, fav = sel.favorite, untilF = frame + 30 }
      end
    end
    if not busy and (P.press("start") or (keys and ui.IsKeyPressed(ui.Key.Enter))) then
      if v.selected > 0 then play(v, v.selected) end
    end
    local y = y0
    -- the world played last: one press away
    local last = v.servers[v.last]
    if last then
      P.at(x0, y)
      T.heading("Last played", w)
      y = P.y()
      P.at(x0, y)
      local clicked, _, bx0, by0, bx1, by1 = P.button(F, "##last", "", w, px(96), { disabled = busy })
      T.rect(bx0, by0 + px(16), bx0 + px(5), by1 - px(16), C.accent, 1, px(3))
      local sx = status(last, bx1 - px(20), (by0 + by1) / 2)
      T.font(30)
      T.textIn(bx0 + px(26), by0, sx - px(10), by1, T.fit(last.name, sx - bx0 - px(40)), C.text, 0, busy and 0.45 or 1)
      T.unfont()
      if clicked then
        if P.padDrives() then play(v, v.last) else chosen(v, v.last) end
      end
      y = by1 + px(20)
    end

    P.at(x0, y)
    T.heading("Worlds", w)
    y = P.y()
    -- as many rows as fit above the status line and the buttons
    local footer = px(50) + px(14) + px(72) -- (the status: two lines)
    local visible = math.max(1, math.floor((y1 - y - footer + px(10)) / (px(ROW_H) + px(10))))
    P.at(x0, y)
    if n == 0 then
      T.font(22)
      T.text(x0 + px(8), y + px(10), "No worlds listed.", C.dim)
      T.unfont()
    else
      local row = W.list("worlds", st.list, n, math.min(visible, n), ROW_H, w, function(i, rx0, ry0, rx1, ry1, focused)
        local s = v.servers[i]
        local sx = status(s, rx1 - px(18), (ry0 + ry1) / 2)
        -- the favourite star at the left (a click on it marks or unmarks the world)
        local sr = px(13)
        local cx, cy = rx0 + px(20) + sr, (ry0 + ry1) / 2
        st.stars[i] = { rx0, ry0, cx + sr + px(10), ry1 }
        local hot = not P.padDrives() and ui.IsMouseHoveringRect(rx0, ry0, cx + sr + px(10), ry1)
        star(cx, cy, sr, s.favorite, (s.favorite or hot) and C.gold or C.faint, busy and 0.45 or 1)
        local nx = cx + sr + px(14)
        T.font(25)
        T.textIn(nx, ry0, sx - px(10), ry1, T.fit(s.name, sx - nx - px(10)), s.canJoin and C.text or C.dim, 0,
          busy and 0.45 or 1)
        T.unfont()
      end, function(i)
        if not busy then chosen(v, i) end
      end)
      -- a world just marked or unmarked with Y: once the list holds its new mark, the focus moves to its row
      local f = st.follow
      if f then
        local at
        for k, s in ipairs(v.servers) do
          if s.name == f.name then at = k end
        end
        if not at or frame > f.untilF then
          st.follow = nil
        elseif v.servers[at].favorite ~= f.fav then
          if row == at then
            st.follow = nil
          elseif not f.moved then
            local shown = math.min(visible, n)
            if at <= st.list.offset then st.list.offset = at - 1
            elseif at > st.list.offset + shown then st.list.offset = at - shown end
            st.list.focusNext = at - st.list.offset
            f.moved = true
          end
        end
      end
      -- with the pad the selection follows the focus
      if row and P.padDrives() and row ~= v.selected and not busy and not st.follow then
        API.pregame.ServerSelectDo("select", row)
      end
      st.list.selected = v.selected
    end

    -- the status line and the buttons at the bottom
    local by = y1 - px(72)
    -- (a world listed with an add-on the player does not have: its hint, while the status line has nothing else)
    local note = st.note and ui.GetTime() - st.noteAt < 4 and { text = st.note, error = true }
      or ((v.status.text or "") == "" and (v.addonHint or "") ~= "" and { text = v.addonHint, error = false })
      or v.status
    P.status(note, busy, x0, by - px(12), x1, { bottom = true, lines = 2 })
    local bw = px(240)
    P.at(x1 - 2 * bw - px(12), by)
    if P.button(F, "##back", "Back", bw, px(72), { glyph = "b" }) then API.pregame.ServerSelectDo("back") end
    ui.SameLine(0, px(12))
    local sel = v.servers[v.selected]
    local canPlay = not busy and sel ~= nil and sel.canJoin
    if P.button(F, "##play", "Play", bw, px(72), { primary = canPlay, disabled = not canPlay, glyph = "start" }) then
      play(v, v.selected)
    end
    F:tick()
  end
  W.finish()
end)
