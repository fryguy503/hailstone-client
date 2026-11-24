-- STML (the game's text markup, CStmlWnd), once the Classic UI's: parsing into coloured pieces and links, and drawing
-- them word-wrapped. Used by chat windows and every STMLbox (item and spell descriptions, tips, ...).
--   <c "#rrggbb"> ... </c>  colour (nested)        <a WndNotify="n,data"> ... </a>  link (n: message to the window)
--   <br>, <p>, new lines     line breaks            other tags are dropped; &lt; &gt; &amp; &quot; &apos; &nbsp;
--   a "<" with no ">" after it is text
local stml = {}

-- (the palette's link and text colours: tables lib/theme.lua changes in place, so parsed lines follow a new palette)
local C = dofile("lib/theme.lua").color
local textselect = dofile("lib/textselect.lua")
stml.LINK = C.link
local WHITE = C.text
local ENTITIES = { lt = "<", gt = ">", amp = "&", quot = "\"", apos = "'", nbsp = " " }

local function entities(text)
  return (text:gsub("&(%a+);", function(e) return ENTITIES[e:lower()] or ("&" .. e .. ";") end))
end

local function hexColor(h)
  if #h == 8 then h = h:sub(3) end -- #AARRGGBB
  local r, g, b = tonumber(h:sub(1, 2), 16), tonumber(h:sub(3, 4), 16), tonumber(h:sub(5, 6), 16)
  if not (r and g and b) then return nil end
  return { r / 255, g / 255, b / 255 }
end

-- One line of STML -> segments { text, color, link } (link = { message, data } for WndNotify links, { -1, target }
-- for href links (the help pages'), true for other links, which only change the colour). color: the colour outside any <c> (default the palette's text).
function stml.Parse(line, color)
  local segments, colors, link = {}, { color or WHITE }, false
  local pos = 1
  while pos <= #line do
    local lt = line:find("<", pos, true)
    local text = line:sub(pos, (lt or #line + 1) - 1)
    if text ~= "" then
      table.insert(segments, { entities(text), link and stml.LINK or colors[#colors], link })
    end
    if not lt then break end
    local gt = line:find(">", lt, true)
    if not gt then -- a "<" that opens no tag ("<3" in server text): the rest is text, not dropped
      table.insert(segments, { entities(line:sub(lt)), link and stml.LINK or colors[#colors], link })
      break
    end
    local tag = line:sub(lt + 1, gt - 1)
    local name = tag:match("^/?(%a+)")
    name = name and name:lower()
    if name == "c" and tag:sub(1, 1) ~= "/" then
      table.insert(colors, hexColor(tag:match("#(%x+)") or "") or colors[#colors])
    elseif name == "c" then
      if #colors > 1 then table.remove(colors) end
    elseif name == "a" then
      if tag:sub(1, 1) == "/" then
        link = false
      else
        local message, data = tag:match('[Ww]nd[Nn]otify="(%d+),([^"]*)"')
        -- (an unclosed quote ends at the tag's end: storyline pages written as href="file:///x.txt>, which the
        -- original's STML box follows)
        local href = tag:match('[Hh][Rr][Ee][Ff]%s*=%s*"([^"]*)"') or tag:match('[Hh][Rr][Ee][Ff]%s*=%s*"([^"]*)$')
        -- an href link: message -1, the target (the STML window follows it: hs.StmlLink)
        link = message and { tonumber(message), data } or (href and { -1, href }) or true
      end
    end
    pos = gt + 1
  end
  return segments
end

local MAX_CARRY = 32 -- open <c> tags carried from one line to the next (stml.Lines)

-- A whole STML text -> lines of segments. A colour opened on one line carries on to the next, as CStmlWnd draws it.
function stml.Lines(raw, color)
  raw = raw:gsub("\r", ""):gsub("<[Bb][Rr]%s*/?>", "\n"):gsub("</?[Pp]>", "\n")
  local out = {}
  local carry = ""
  for line in (raw .. "\n"):gmatch("(.-)\n") do
    local full = carry .. line
    out[#out + 1] = stml.Parse(full, color)
    -- the <c> tags still open at the end of the line (carried ones included)
    local open = {}
    for slash, tag in full:gmatch("<(/?)([Cc][%s>][^>]*)") do
      if slash == "" then open[#open + 1] = "<" .. tag .. ">" elseif #open > 0 then open[#open] = nil end
    end
    -- Only the innermost MAX_CARRY are carried: a text that never closes its tags would otherwise carry them all to
    -- every later line, reparsed each time. Text that nests no deeper than that is drawn the same.
    carry = table.concat(open, "", math.max(1, #open - MAX_CARRY + 1))
  end
  while #out > 0 and #out[#out] == 0 do out[#out] = nil end
  return out
end

-- A line of several pieces word-wrapped to width, the way stml.DrawLine draws it: into `out`, six entries a piece
-- (the text, its colour, its link, whether it starts a row, its byte offset in the line's text, its width). A row is started
-- by the wrap (true) or by a new line in the text ("hard"): a chat line can hold several (a server's MOTD). An empty
-- line is an empty piece, a row of its own; a new line at the end adds nothing. A word wider than a whole row (a long
-- name or a run with no space) is broken where the row ends, at a character's start, as ImGui's TextWrapped breaks it.

-- How many bytes of `word` fit in `room` pixels, ending at a UTF-8 character's end; at least its first character.
local function fit(word, room)
  local ends, i, n = {}, 1, #word
  while i <= n do
    local b = word:byte(i)
    local len = (b >= 0xF0 and 4) or (b >= 0xE0 and 3) or (b >= 0xC0 and 2) or 1
    i = math.min(i + len, n + 1)
    ends[#ends + 1] = i - 1
  end
  local lo, hi = 1, #ends
  while lo < hi do
    local mid = (lo + hi + 1) // 2
    if ui.CalcTextSize(word:sub(1, ends[mid])) <= room then lo = mid else hi = mid - 1 end
  end
  return ends[lo]
end

local function wrap(segments, width, out)
  local n, x, first, broken, base = 0, 0, true, false, 0
  local function put(piece, seg, row, off, w)
    out[n + 1], out[n + 2], out[n + 3], out[n + 4], out[n + 5], out[n + 6] = piece, seg[2], seg[3], row, off, w
    n = n + 6
    first = false
  end
  for _, seg in ipairs(segments) do
    local text, from = seg[1], 1
    while true do
      local nl = text:find("\n", from, true)
      for at, word, space in text:sub(from, (nl or #text + 1) - 1):gmatch("()([^ ]*)( *)") do
        if word ~= "" or space ~= "" then
          local row = broken
          local ww = ui.CalcTextSize(word)
          if not first and not row and x + ww > width and x > 0 then row = true end -- wrap: next row
          if row then x = 0 end
          local off, starts = base + from + at - 2, row and not first and (broken and "hard" or true)
          while ww > width do -- (too wide for any row: a row's worth at a time, each next piece on a row of its own)
            local k = fit(word, width)
            if k >= #word then break end
            local piece = word:sub(1, k)
            put(piece, seg, starts, off, ui.CalcTextSize(piece))
            word, off, starts, x = word:sub(k + 1), off + k, true, 0
            ww = ui.CalcTextSize(word)
          end
          local w = ui.CalcTextSize(word .. space)
          put(word .. space, seg, starts, off, w)
          x = x + w
          broken = false
        end
      end
      if not nl then break end
      if broken or first then put("", seg, not first and "hard", base + nl - 1, 0) end -- (an empty line)
      broken, from = true, nl + 1
    end
    base = base + #text
  end
  for i = #out, n + 1, -1 do out[i] = nil end
  return out
end

-- (the link pieces drawn this line, four entries each: left, bottom, right, the link; reused, not made every frame)
local rects = {}

local function measure(text) return (ui.CalcTextSize(text)) end

-- With a selection (stml.DrawLines' sel): a row starting at screen y `py` is the one under the mouse until a row
-- below it starts above the mouse (rows come top to bottom).
local function selectionRow(sel, line, off, py)
  sel.rows = sel.rows + 1
  sel.firstLine = sel.firstLine or line
  if sel.my >= py then sel.candRow, sel.hitLine, sel.hitOff = sel.rows, line, off end
end

-- A link's words are drawn as separate pieces (and on separate rows when it wraps), but it is one link: the mouse on
-- any piece underlines every piece of it, and a click on any piece follows it.
-- sel (optional): the selected part is drawn behind its pieces, and the place under the mouse is found (see
-- stml.DrawLines; only while sel.active); line is the line's number there.
local function drawPieces(p, onLink, sel, line)
  local n = #p
  if n == 0 then
    if sel and sel.active then selectionRow(sel, line, 0, select(2, ui.GetCursorScreenPos())) end
    ui.Text("")
    return
  end
  local m, hovered = 0, nil
  local track = sel and sel.active
  for i = 1, n, 6 do
    if i > 1 and not p[i + 3] then ui.SameLine(0, 0) end
    local piece, col = p[i], p[i + 1]
    if track then
      local px, py = ui.GetCursorScreenPos()
      local pw, ph, o = p[i + 5], sel.lineH, p[i + 4]
      if i == 1 or p[i + 3] then selectionRow(sel, line, o, py) end
      if sel.candRow == sel.rows and sel.mx >= px then
        sel.hitOff = o + (sel.mx < px + pw and textselect.CharAt(piece, sel.mx - px, measure) or #piece)
      end
      local lo, hi = textselect.Span(sel.s, sel.e, line, o, #piece)
      if lo then
        local x0 = lo > 0 and px + ui.CalcTextSize(piece:sub(1, lo)) or px
        local x1 = hi < #piece and px + ui.CalcTextSize(piece:sub(1, hi)) or px + pw
        local hc = sel.color
        ui.DrawRectFilled(x0, py, x1, py + ph, hc[1], hc[2], hc[3], hc[4])
      end
    end
    ui.TextColored(col[1], col[2], col[3], 1, piece)
    local link = p[i + 2]
    if type(link) == "table" then
      local x0, _, x1, y1 = ui.GetItemRect()
      rects[m + 1], rects[m + 2], rects[m + 3], rects[m + 4] = x0, y1, x1, link
      m = m + 4
      if ui.IsItemHovered() then
        hovered = link
        if track then sel.overLink = true end
        if ui.IsItemClicked(ui.Mouse.Left) and onLink then onLink(link) end
      end
    end
  end
  if hovered then
    for j = 1, m, 4 do
      if rects[j + 3] == hovered then
        ui.DrawRect(rects[j], rects[j + 1] - 1, rects[j + 2], rects[j + 1], stml.LINK[1], stml.LINK[2], stml.LINK[3], 1, 0, 1)
      end
    end
  end
  for j = m, 1, -1 do rects[j] = nil end
end

-- Draws one line, word-wrapped to width, each piece in its colour. A link is underlined while hovered; onLink(link)
-- is called when one is clicked. The line's wrap is kept on it (segments.wrap) and worked out again only when the
-- width or the font change, not every frame; key (optional): the font, else measured. sel, line: see stml.DrawLines.
function stml.DrawLine(segments, width, onLink, key, sel, line)
  if not sel and #segments == 1 and not segments[1][3] then
    local col = segments[1][2]
    ui.PushStyleColor(ui.Col.Text, col[1], col[2], col[3], 1)
    ui.TextWrapped(segments[1][1])
    ui.PopStyleColor()
    return
  end
  if key == nil then
    local w, h = ui.CalcTextSize("Wg il")
    key = w * 4096 + h
  end
  if segments.wrapKey ~= key or segments.wrapWidth ~= width then
    segments.wrap = wrap(segments, width, segments.wrap or {})
    segments.wrapKey, segments.wrapWidth = key, width
  end
  drawPieces(segments.wrap, onLink, sel, line)
end


-- An output's lines (hs.ChatText's STML strings) parsed into `view` (view.lines; view.raws, the strings), reusing
-- the last parse of a line still there (with its wrap and height): a new line costs one parse, not the whole output's.
-- The lines are numbered afresh (view.dropped: see stml.AppendLines).
function stml.UpdateLines(view, raws, color)
  local old = {}
  for i, raw in ipairs(view.raws or {}) do old[raw] = view.lines[i] end
  local lines = {}
  for i, raw in ipairs(raws) do
    local p = old[raw] or stml.Parse(raw, color)
    old[raw] = p
    lines[i] = p
  end
  view.raws, view.lines, view.cumKey, view.dropped = raws, lines, nil, 0
end

-- hs.ChatText's incremental form: `raws` added at the end, then the oldest dropped until `count` are left
-- (view.dropped counts them: a line's number, lib/textselect.lua's, is its index plus that).
function stml.AppendLines(view, raws, count, color)
  local lines, rs = view.lines or {}, view.raws or {}
  local n = #lines
  for _, raw in ipairs(raws) do
    n = n + 1
    rs[n], lines[n] = raw, stml.Parse(raw, color)
  end
  local drop = n - math.max(count or n, 0)
  if drop > 0 then
    table.move(lines, drop + 1, n, 1)
    table.move(rs, drop + 1, n, 1)
    for i = n - drop + 1, n do lines[i], rs[i] = nil, nil end
    view.dropped = (view.dropped or 0) + drop
  end
  view.raws, view.lines, view.cumKey = rs, lines, nil
end

-- Draws view's lines (stml.UpdateLines) in the scrolling child the caller began, word-wrapped to width; a line out
-- of sight (viewH: the child's height) is passed over by the height it had when last drawn with the same `key`
-- (the width and font), so a long output costs what is in view. Once every line has its height the lines' places
-- are summed (view.cum) and the first one in sight found by a binary search.
-- sel (optional; the chat output's): a text selection (lib/textselect.lua). Before the call the caller sets
-- sel.active (whether to draw a selection and look for the mouse: false costs nothing), sel.mx, sel.my (the mouse,
-- screen coordinates), sel.s, sel.e (textselect.Range, or nil), sel.color and sel.lineH (a row's height), and clears
-- sel.rows (0), sel.candRow, sel.hitLine, sel.firstLine and sel.overLink. The selected text is drawn on that colour,
-- and after the call sel.hitLine, sel.hitOff are the place under the mouse (the nearest row above it; nil above every
-- line drawn), sel.firstLine the first line drawn and sel.overLink whether a link is hovered. With sel every line is
-- drawn by its wrap, a single-coloured one too, so its places can be found.
function stml.DrawLines(view, width, viewH, key, onLink, sel)
  local lines = view.lines or {}
  local base = view.dropped or 0
  local n = #lines
  if n == 0 then return end
  local top = ui.GetScrollY()
  local x0, y0 = ui.GetCursorPos()
  if view.cumKey ~= key then
    local cum, y = view.cum or {}, 0
    for i = 1, n do
      local l = lines[i]
      if l.heightKey ~= key then cum = nil; break end
      cum[i] = y
      y = y + l.height
    end
    if cum then
      cum[n + 1] = y
      for i = #cum, n + 2, -1 do cum[i] = nil end
      view.cum, view.cumKey = cum, key
    end
  end
  if view.cumKey == key then
    -- (the lines' places known: the same lines drawn and passed over as below, without walking every line)
    local cum, lo, hi = view.cum, 1, n + 1
    while lo < hi do -- the first line not wholly above the view: y + h >= top - 40
      local mid = (lo + hi) // 2
      if y0 + cum[mid + 1] < top - 40 then lo = mid + 1 else hi = mid end
    end
    local i = lo
    if i > 1 then ui.SetCursorPos(x0, y0 + cum[i]) end
    local y = y0 + cum[i]
    while i <= n and y <= top + viewH + 40 do
      local l = lines[i]
      stml.DrawLine(l, width, onLink, key, sel, base + i)
      local _, ny = ui.GetCursorPos()
      if ny - y ~= l.height then l.height = ny - y; view.cumKey = nil end
      y = ny
      i = i + 1
    end
    if i <= n then
      ui.SetCursorPos(x0, y0 + cum[n + 1])
      ui.Dummy(0, 0)
    end
    return
  end
  local y = y0
  local skipped = false
  for i, l in ipairs(lines) do
    local h = l.heightKey == key and l.height
    if h and (y + h < top - 40 or y > top + viewH + 40) then
      y = y + h
      skipped = true
    else
      if skipped then ui.SetCursorPos(x0, y); skipped = false end
      stml.DrawLine(l, width, onLink, key, sel, base + i)
      local _, ny = ui.GetCursorPos()
      l.height, l.heightKey = ny - y, key
      y = ny
    end
  end
  if skipped then
    ui.SetCursorPos(x0, y)
    ui.Dummy(0, 0)
  end
end

return stml
