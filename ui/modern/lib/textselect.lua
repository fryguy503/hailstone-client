-- Text selection over STML lines (lib/stml.lua's views): what is selected, the text it holds, words, multiple clicks.
-- A Hailstone addition: the chat output's lines can be selected with the mouse and copied (chat.lua).
-- A position is { line = n, off = k }. line counts every line the view has held (view.dropped, the lines
-- stml.AppendLines let go, plus the line's index), so a selection stays on its text as lines come and the oldest go.
-- off is a byte offset into the line's text (its segments' texts joined), so it stays put when the box is resized
-- or the font changes. Nothing here draws or reads the mouse: lib/stml.lua and chat.lua do, and the tests run it as
-- plain Lua.
local textselect = {}

-- A line's text: its segments' texts joined (kept on the line, as its wrap is).
function textselect.LineText(segments)
  local t = segments.plain
  if t then return t end
  local parts = {}
  for i, seg in ipairs(segments) do parts[i] = seg[1] end
  t = table.concat(parts)
  segments.plain = t
  return t
end

function textselect.Before(a, b) return a.line < b.line or (a.line == b.line and a.off < b.off) end

-- The selection's start and end in order, or nil when nothing is selected.
function textselect.Range(sel)
  local a, b = sel.anchor, sel.caret
  if not (a and b) or (a.line == b.line and a.off == b.off) then return nil end
  if textselect.Before(b, a) then a, b = b, a end
  return a, b
end

-- The part of a piece selected: the piece starts at byte `o` of line `line` and is `n` bytes long; s, e from
-- textselect.Range. Returns the selected bytes within the piece as offsets from its start (lo, hi), or nil.
function textselect.Span(s, e, line, o, n)
  if not s or line < s.line or line > e.line then return nil end
  local from = line == s.line and s.off or 0
  local to = line == e.line and e.off or math.huge
  local lo, hi = math.max(from, o), math.min(to, o + n)
  if hi <= lo then return nil end
  return lo - o, hi - o
end

-- The selected text, one line of the view to a line of the text. Lines that have gone (dropped) are left out.
function textselect.Extract(view, s, e)
  local lines, base = view.lines or {}, view.dropped or 0
  local out = {}
  for abs = s.line, e.line do
    local l = lines[abs - base]
    if l then
      local t = textselect.LineText(l)
      local from = abs == s.line and s.off or 0
      local to = abs == e.line and math.min(e.off, #t) or #t
      out[#out + 1] = t:sub(from + 1, to)
    end
  end
  return table.concat(out, "\n")
end

-- The word around byte offset `off` of `text` (a run of anything but white space), as byte offsets a, b; a == b on
-- white space.
function textselect.Word(text, off)
  local n = #text
  off = math.max(0, math.min(off, n))
  local a, b = off, off
  while a > 0 and not text:sub(a, a):match("%s") do a = a - 1 end
  while b < n and not text:sub(b + 1, b + 1):match("%s") do b = b + 1 end
  return a, b
end

-- How many bytes of `piece` lie left of `dx` (the distance from the piece's left edge), at the nearest character
-- boundary: a UTF-8 sequence is never split. measure(text) -> width.
function textselect.CharAt(piece, dx, measure)
  local n = #piece
  if dx <= 0 then return 0 end
  local prev, prevW, i = 0, 0, 1
  while i <= n do
    local b = piece:byte(i)
    local len = (b >= 0xF0 and 4) or (b >= 0xE0 and 3) or (b >= 0xC0 and 2) or 1
    local k = math.min(i + len - 1, n) -- (a stray continuation byte counts as a character of its own)
    local w = measure(piece:sub(1, k))
    if w >= dx then
      if dx - prevW < w - dx then return prev end
      return k
    end
    prev, prevW, i = k, w, k + 1
  end
  return n
end

-- Counts a click as the first, second or third of a run (a fourth starts again): one within `interval` seconds of
-- the last and within `slop` pixels of it adds to the run. `state` keeps the run between calls.
function textselect.Clicks(state, t, x, y, interval, slop)
  local near = state.t and t - state.t <= interval and math.abs(x - state.x) <= slop and math.abs(y - state.y) <= slop
  state.count = (near and state.count < 3) and state.count + 1 or 1
  state.t, state.x, state.y = t, x, y
  return state.count
end

return textselect
