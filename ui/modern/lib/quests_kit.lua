-- Small pieces shared by the modern quest, expedition, adventure, PvP and tribute windows (tasks.lua,
-- taskselect.lua, dynamiczone.lua, adventure*.lua, pvp*.lua, tribute*.lua): empty states, chips, stat tiles,
-- search boxes, a game list read as rows, selectable cards and times. They draw in ImGui's flow, so the dialogs
-- that use them reflow when resized. Every click still goes to the original window (hs.ListClick, W.Button).
local API = hs.UI
local T = dofile("lib/theme.lua")
local C = T.color

local K = {}

local function scale() return ui.GetIO().uiScale or 1 end

-- ---------------------------------------------------------------- reading
-- Whether two hs.ListRow texts tables read the same (the texts, the cell colours, the icon's column).
local function sameTexts(t, u)
  if #t ~= #u or t.iconColumn ~= u.iconColumn then return false end
  for i = 1, #t do if t[i] ~= u[i] then return false end end
  local a, b = t.colors, u.colors
  if (a == nil) ~= (b == nil) then return false end
  if a then
    for k, c in pairs(a) do
      local d = b[k]
      if not d or c[1] ~= d[1] or c[2] ~= d[2] or c[3] ~= d[3] then return false end
    end
    for k in pairs(b) do if not a[k] then return false end end
  end
  return true
end
local function sameRow(e, r, texts, cr, cg, cb)
  if not e or e.row ~= r then return false end
  local c, t = e.color, e.texts
  if (c and c[1]) ~= cr or (c and c[2]) ~= cg or (c and c[3]) ~= cb then return false end
  return sameTexts(t, texts)
end
local lastRows = {} -- [win][id] = the rows K.Rows gave last call
-- Every row of a list child: { { row = 0-based, texts = { ... }, color = { r, g, b } }, ... }, and the selected
-- row (0-based, -1 none). For the short lists of these windows (a list of tasks, members, themes).
-- A row that reads as it did last call is last call's table (its texts and colour unchanged), so a list that stands
-- still makes no new tables but the list's own answers; the rows are the caller's to read, not to change.
function K.Rows(win, id)
  local rows, _, selected = API.windows.ListInfo(win, id)
  local out = {}
  local byId = lastRows[win]
  if not byId then byId = {}; lastRows[win] = byId end
  local last = byId[id]
  for r = 0, (rows or 0) - 1 do
    local texts, cr, cg, cb = API.windows.ListRow(win, id, r)
    if texts then
      if not cr then cg, cb = nil, nil end
      local e = last and last[#out + 1]
      if not sameRow(e, r, texts, cr, cg, cb) then
        e = { row = r, texts = texts, color = cr and { cr, cg, cb } or nil }
      end
      out[#out + 1] = e
    end
  end
  byId[id] = out
  return out, selected or -1
end

-- "key=value" lines -> a table.
function K.Fields(text)
  local t = {}
  for k, v in (text or ""):gmatch("([%w_]+)=([^\n]*)") do t[k] = v end
  return t
end

-- seconds -> "2h 05m", "4m 09s", "12s"; days when long.
function K.Time(sec)
  sec = math.max(0, math.floor(tonumber(sec) or 0))
  if sec >= 86400 then return string.format("%dd %dh", sec // 86400, (sec % 86400) // 3600) end
  if sec >= 3600 then return string.format("%dh %02dm", sec // 3600, (sec % 3600) // 60) end
  if sec >= 60 then return string.format("%dm %02ds", sec // 60, sec % 60) end
  return sec .. "s"
end

-- copper -> { pp, gp, sp, cp } for W.Coins
function K.Coins(copper)
  copper = math.floor(tonumber(copper) or 0)
  return { copper // 1000, (copper // 100) % 10, (copper // 10) % 10, copper % 10 }
end

-- ---------------------------------------------------------------- drawing
function K.Muted(text, color)
  local c = color or C.muted
  ui.TextColored(c[1], c[2], c[3], 1, text)
end

-- A section heading with a hairline under it, as W.Heading, and an optional muted note at its right end.
function K.Heading(text, note)
  local s = scale()
  ui.Dummy(0, 2 * s)
  local x = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, text)
  if note and note ~= "" then
    local nw = ui.CalcTextSize(note)
    ui.SameLine(0, 0)
    local cx = ui.GetCursorScreenPos()
    ui.SetCursorScreenPos(math.max(cx + 8 * s, x + w - nw), select(2, ui.GetCursorScreenPos()))
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, note)
  end
  local _, y = ui.GetCursorScreenPos()
  ui.DrawLine(x, y, x + w, y, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
  ui.Dummy(0, 3 * s)
end

-- Coins from copper as "3p 2g 5s" in the coins' colours, leaving out the empty ones (inline at the cursor).
local COIN = { { "p", C.platinum }, { "g", C.gold }, { "s", C.silver }, { "c", C.copper } }
function K.CoinLine(copper)
  local v = K.Coins(copper)
  local first = true
  for i = 1, 4 do
    if v[i] > 0 then
      if not first then ui.SameLine(0, 6) end
      first = false
      ui.TextColored(C.text[1], C.text[2], C.text[3], 1, T.Thousands(v[i]))
      ui.SameLine(0, 1)
      local c = COIN[i][2]
      ui.TextColored(c[1], c[2], c[3], 1, COIN[i][1])
    end
  end
  if first then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "none") end
end

-- A friendly empty state: a line of text (and a fainter hint under it), centred in the width, with air around it.
function K.Empty(text, hint)
  local s = scale()
  ui.Dummy(0, 14 * s)
  local avail = ui.GetContentRegionAvail()
  local function centred(t, c)
    local tw = ui.CalcTextSize(t)
    if tw < avail then
      local cx = ui.GetCursorPos()
      ui.SetCursorPos(cx + (avail - tw) / 2, select(2, ui.GetCursorPos()))
      ui.TextColored(c[1], c[2], c[3], 1, t)
    else
      ui.PushTextWrapPos(0)
      ui.TextColored(c[1], c[2], c[3], 1, t)
      ui.PopTextWrapPos()
    end
  end
  centred(text, C.muted)
  if hint then centred(hint, C.faint) end
  ui.Dummy(0, 14 * s)
end

-- A small rounded tag at the cursor (inline: call ui.SameLine() before it to put it after something).
function K.Chip(text, color, tooltip)
  local s = scale()
  color = color or C.accent
  ui.PushFont(T.font.small)
  local tw, th = ui.CalcTextSize(text)
  ui.PopFont()
  local x, y = ui.GetCursorScreenPos()
  local _, lh = ui.CalcTextSize("Ag")
  local h = math.max(th + 4 * s, lh)
  local w = tw + 12 * s
  ui.DrawRectFilled(x, y, x + w, y + h, color[1], color[2], color[3], 0.16, h / 2)
  ui.DrawRect(x, y, x + w, y + h, color[1], color[2], color[3], 0.45, h / 2, 1)
  ui.PushFont(T.font.small)
  ui.DrawText(x + 6 * s, y + (h - th) / 2, color[1], color[2], color[3], 1, text)
  ui.PopFont()
  ui.Dummy(w, h)
  if tooltip and ui.IsItemHovered() then ui.SetTooltip(tooltip) end
end

-- A stat tile: a muted caption over a large value, `w` wide (0: the rest of the line). Returns its height.
function K.Stat(caption, value, color, w, tooltip)
  local s = scale()
  local x, y = ui.GetCursorScreenPos()
  w = (w and w > 0) and w or ui.GetContentRegionAvail()
  local _, ch = T.TextSize("Ag", T.font.small)
  local _, vh = T.TextSize("Ag", T.font.large)
  local h = ch + vh + 14 * s
  ui.DrawRectFilled(x, y, x + w, y + h, C.inset[1], C.inset[2], C.inset[3], 0.55, 6 * s)
  ui.DrawRect(x, y, x + w, y + h, C.ink[1], C.ink[2], C.ink[3], 0.06, 6 * s, 1)
  T.Text(x + 9 * s, y + 6 * s, caption, C.muted, T.font.small)
  T.Text(x + 9 * s, y + 8 * s + ch, (value ~= nil and value ~= "") and tostring(value) or "-", color or C.text, T.font.large)
  ui.Dummy(w, h)
  if tooltip and ui.IsItemHovered() then ui.SetTooltip(tooltip) end
  return h
end

-- A row of stat tiles that share the width: { { caption, value [, color [, tooltip]] }, ... }; wraps to more rows
-- when the tiles would be narrower than minW (design pixels).
function K.StatRow(tiles, minW)
  local s = scale()
  local gap = 8 * s
  local avail = ui.GetContentRegionAvail()
  local per = math.max(1, math.min(#tiles, math.floor((avail + gap) / ((minW or 110) * s + gap))))
  local tw = (avail - gap * (per - 1)) / per
  for i, t in ipairs(tiles) do
    if (i - 1) % per ~= 0 then ui.SameLine(0, gap) end
    K.Stat(t[1], t[2], t[3], tw, t[4])
  end
end

-- A search box over the whole width; keeps its text in `state[key]`. Returns the lower-cased needle ("" none).
function K.Search(state, key, hint)
  ui.SetNextItemWidth(-1)
  state[key] = ui.InputTextWithHint("##" .. key, hint or "Search", state[key] or "", 64)
  return (state[key] or ""):lower()
end

-- A selectable card `h` design pixels tall at the cursor, the full width; draw(x, y, w, h) paints its content.
-- Returns clicked (left), rightClicked.
function K.Card(id, selected, h, draw)
  local s = scale()
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  h = h * s
  local clicked = ui.Selectable("##card" .. id, selected, 0, w, h)
  local right = ui.IsItemClicked(ui.Mouse.Right)
  if selected then
    ui.DrawRectFilled(x, y + 3 * s, x + 3 * s, y + h - 3 * s, C.accent[1], C.accent[2], C.accent[3], 1, 2 * s)
  end
  draw(x + 10 * s, y, w - 10 * s, h)
  return clicked, right
end

-- A check mark or an open ring (an objective's state), `size` screen pixels at x, y.
function K.Tick(x, y, size, done, color)
  local c = color or (done and C.pet or C.faint)
  local r = size / 2
  if done then
    ui.DrawCircleFilled(x + r, y + r, r, c[1], c[2], c[3], 0.22)
    ui.DrawCircle(x + r, y + r, r, c[1], c[2], c[3], 0.9, 1.5)
    ui.DrawLine(x + size * 0.28, y + size * 0.52, x + size * 0.44, y + size * 0.68, c[1], c[2], c[3], 1, 2)
    ui.DrawLine(x + size * 0.44, y + size * 0.68, x + size * 0.74, y + size * 0.34, c[1], c[2], c[3], 1, 2)
  else
    ui.DrawCircle(x + r, y + r, r, c[1], c[2], c[3], 0.9, 1.5)
  end
end

-- An STML child's text inline in the flow (wrapped; links go to the window), cached by its text.
local stml = dofile("lib/stml.lua")
local stmlCache = {}
function K.Stml(win, id, raw)
  raw = raw or (API.windows.Text(win, id) or "")
  local byId = stmlCache[win]
  if not byId then byId = {}; stmlCache[win] = byId end
  local c = byId[id]
  if not c or c.raw ~= raw then
    -- (the link handler is made with the lines, not once a line a frame)
    c = { raw = raw, lines = stml.Lines(raw, nil), onLink = function(link) API.windows.StmlLink(win, id, link[1], link[2]) end }
    byId[id] = c
  end
  local width = ui.GetContentRegionAvail()
  for _, line in ipairs(c.lines) do stml.DrawLine(line, width, c.onLink) end
  return #c.lines
end

-- A text the window escaped whole (stml_escape: the tribute descriptions) whose line breaks were meant as breaks:
-- "&lt;br&gt;" back to "<br>", so the lines break instead of showing the tag.
function K.Breaks(raw)
  return ((raw or ""):gsub("&lt;[Bb][Rr]%s*/?&gt;", "<br>"))
end

-- Whether the STML text has anything to show (tags and spaces aside).
function K.HasText(raw)
  return ((raw or ""):gsub("<[^>]*>", ""):gsub("%s", "")) ~= ""
end

return K
