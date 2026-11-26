-- STML (the game's text markup) for the gamepad pack: a chat line into coloured pieces, and drawing them
-- word-wrapped with the theme's text. <c "#rrggbb"> colours (nested), links are shown in the link colour; other
-- tags are dropped, and a "<" with no ">" after it is shown as text.
local T = dofile("lib/theme.lua")
local S = {}

-- links' colour: the palette's (a table lib/theme.lua changes in place); the chat sets its own copy's chatLink
S.LINK = T.C.link
local ENT = { lt = "<", gt = ">", amp = "&", quot = "\"", apos = "'", nbsp = " " }

local function entities(t) return (t:gsub("&(%a+);", function(e) return ENT[e:lower()] or ("&" .. e .. ";") end)) end
local function hex(h)
  if #h == 8 then h = h:sub(3) end
  local r, g, b = tonumber(h:sub(1, 2), 16), tonumber(h:sub(3, 4), 16), tonumber(h:sub(5, 6), 16)
  return r and g and b and { r / 255, g / 255, b / 255, 1 } or nil
end

-- line -> { { text, colour } }; color: the colour outside any <c> (the palette's text)
function S.parse(line, color, explicitLinkColor)
  local out, colors, link = {}, { color or T.C.text }, false
  local pos = 1
  while pos <= #line do
    local lt = line:find("<", pos, true)
    local text = line:sub(pos, (lt or #line + 1) - 1)
    local col = link and not explicitLinkColor and S.LINK or colors[#colors]
    if text ~= "" then out[#out + 1] = { entities(text), col } end
    if not lt then break end
    local gt = line:find(">", lt, true)
    if not gt then -- a "<" that opens no tag ("<3" in a chat line): the rest is text, not dropped
      out[#out + 1] = { entities(line:sub(lt)), col }
      break
    end
    local tag = line:sub(lt + 1, gt - 1)
    local name = (tag:match("^/?(%a+)") or ""):lower()
    if name == "c" and tag:sub(1, 1) ~= "/" then table.insert(colors, hex(tag:match("#(%x+)") or "") or colors[#colors])
    elseif name == "c" then if #colors > 1 then table.remove(colors) end
    elseif name == "a" then link = tag:sub(1, 1) ~= "/" end
    pos = gt + 1
  end
  return out
end

-- The plain text of a parsed line.
function S.plain(segments)
  local t = {}
  for _, s in ipairs(segments) do t[#t + 1] = s[1] end
  return table.concat(t)
end

-- A prefix that fits in a cell, ending at a UTF-8 character boundary. Keep one character even if it needs clipping.
local function fitWord(word, width)
  local lo, hi = 1, #word
  while lo < hi do
    local middle = (lo + hi + 1) // 2
    local finish = (word:find("[^\128-\191]", middle + 1) or #word + 1) - 1
    if ui.CalcTextSize(word:sub(1, finish)) <= width then lo = finish else hi = middle - 1 end
  end
  if lo == 1 then return (word:find("[^\128-\191]", 2) or #word + 1) - 1 end
  return lo
end

-- Lays a parsed line out in `width`: a list of rows, each { { text, colour, x } }. A new line in the text (a chat
-- line can hold several: a server's MOTD) ends the row; an empty one is an empty row; one at the end adds nothing.
-- In a table cell, split overlong words and record a clip edge for a single character wider than its column.
function S.layout(segments, width, tableCell)
  local rows, row, x = {}, {}, 0
  for _, seg in ipairs(segments) do
    local text, from = seg[1], 1
    while true do
      local nl = text:find("\n", from, true)
      for token, space in text:sub(from, (nl or #text + 1) - 1):gmatch("([^ ]*)( *)") do
        local word = token
        if word ~= "" or space ~= "" then
          local ww = ui.CalcTextSize(word)
          if x > 0 and x + ww > width then
            rows[#rows + 1] = row
            row, x = {}, 0
          end
          while tableCell and ww > width do
            local finish = fitWord(word, width)
            if finish == #word then break end
            local part = word:sub(1, finish)
            row[#row + 1] = { part, seg[2], x, ui.CalcTextSize(part) > width and width or nil }
            rows[#rows + 1] = row
            row, x = {}, 0
            word = word:sub(finish + 1)
            ww = ui.CalcTextSize(word)
          end
          row[#row + 1] = { word .. space, seg[2], x, tableCell and ww > width and width or nil }
          x = x + ui.CalcTextSize(word .. space)
        end
      end
      if not nl then break end
      rows[#rows + 1] = row
      row, x, from = {}, 0, nl + 1
    end
  end
  if #row > 0 or #rows == 0 then rows[#rows + 1] = row end
  return rows
end

-- A number for the font in use (what ui.CalcTextSize measures with): a kept layout is for one width and font.
function S.fontKey()
  local w, h = ui.CalcTextSize("Wg")
  return w * 4096 + h
end

-- S.layout(segments, width), kept on the parsed line until the width or the font (S.fontKey) change: a line drawn
-- every frame is laid out once.
function S.rows(segments, width, font)
  font = font or S.fontKey()
  if segments.rowsWidth ~= width or segments.rowsFont ~= font then
    segments.rows, segments.rowsWidth, segments.rowsFont = S.layout(segments, width), width, font
  end
  return segments.rows
end

local function lineBreaks(text)
  return (text:gsub("\r", ""):gsub("<[Bb][Rr]%s*/?>", "\n"):gsub("</?[Pp]>", "\n"))
end

-- A block of STML (<br> and <p> are line breaks) -> its lines.
function S.split(stml)
  local s = lineBreaks(stml or "")
  local out = {}
  for line in (s .. "\n"):gmatch("(.-)\n") do out[#out + 1] = line end
  return out
end

-- Every row of a block of STML laid out in `width`, one list: S.split's lines (or build(text, width)'s rows),
-- each parsed and laid out. The last few texts' rows are kept (by text, width, font and build), so a text drawn
-- every frame, or two drawn by turns, is not split, parsed and laid out again each time.
local blocks, blockOrder = {}, {}
local BLOCKS = 8
local function blockRows(text, width)
  local rows = {}
  for _, line in ipairs(S.split(text)) do
    for _, r in ipairs(S.layout(S.parse(line), width)) do rows[#rows + 1] = r end
  end
  return rows
end
function S.blockRows(text, width, build, font)
  text, build, font = text or "", build or blockRows, font or S.fontKey()
  local e = blocks[text]
  if e and e.width == width and e.font == font and e.build == build then return e.rows end
  if not e then
    e = {}
    blocks[text] = e
    blockOrder[#blockOrder + 1] = text
    if #blockOrder > BLOCKS then blocks[table.remove(blockOrder, 1)] = nil end
  end
  e.width, e.font, e.build, e.rows = width, font, build, build(text, width)
  return e.rows
end

-- Keep short columns at their natural widths and share the room left among the columns that need to wrap.
local function fitColumns(widths, available)
  local ordered = { table.unpack(widths) }
  table.sort(ordered)
  local limit
  for i, preferred in ipairs(ordered) do
    limit = available / (#ordered - i + 1)
    if preferred >= limit then break end
    available = available - preferred
  end
  for i, preferred in ipairs(widths) do widths[i] = math.min(preferred, limit) end
end

-- A table row occupies as many screen rows as its tallest cell. Merge those screen rows for the dialog's scroller.
local function appendTable(out, block, width)
  local widths, cells = {}, {}
  for i = 1, block.columns do widths[i] = T.px(8) end
  for i, row in ipairs(block.rows) do
    local parsed = {}
    for column, raw in ipairs(row) do
      local segments = S.parse(lineBreaks(raw))
      local lines = S.layout(segments, math.huge)
      local preferred = 0
      for _, line in ipairs(lines) do
        local last = line[#line]
        if last then preferred = math.max(preferred, last[3] + ui.CalcTextSize(last[1])) end
      end
      parsed[column] = { segments = segments, lines = lines, width = preferred }
      widths[column] = math.max(widths[column], preferred)
    end
    cells[i] = parsed
  end

  local gap = math.min(T.px(16), width / (2 * block.columns))
  fitColumns(widths, width - (block.columns - 1) * gap)
  for _, row in ipairs(cells) do
    local start, x = #out + 1, 0
    out[start] = {}
    for column, cell in ipairs(row) do
      local cellWidth = widths[column]
      local lines = cell.width <= cellWidth and cell.lines or S.layout(cell.segments, cellWidth, true)
      for i, line in ipairs(lines) do
        local index = start + i - 1
        local joined = out[index] or {}
        out[index] = joined
        for _, piece in ipairs(line) do
          joined[#joined + 1] = { piece[1], piece[2], x + piece[3], piece[4] and x + piece[4] }
        end
      end
      x = x + cellWidth + gap
    end
  end
end

local markup
local function popupRows(text, width)
  markup = markup or dofile("lib/stml_blocks.lua")
  local rows = {}
  for _, block in ipairs(markup.parse(text)) do
    if block.rows then
      appendTable(rows, block, width)
    else
      for _, row in ipairs(blockRows(block.text, width)) do rows[#rows + 1] = row end
    end
  end
  return rows
end

function S.popupRows(text, width)
  return S.blockRows(text, math.max(1, width), popupRows)
end

function S.drawRow(row, x, y, alpha)
  for _, p in ipairs(row) do
    if p[4] then
      local _, height = ui.CalcTextSize(p[1])
      ui.PushClipRect(x + p[3], y, x + p[4], y + height + T.px(2), true)
    end
    T.draw(x + p[3], y, p[1], p[2], alpha)
    if p[4] then ui.PopClipRect() end
  end
end

return S
