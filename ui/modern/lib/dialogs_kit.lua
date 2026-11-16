-- Small pieces the modern dialogs share (confirmations, the server's text dialogs, text entry, books and notes, the
-- alarm, tints and the colour picker, the bug report, claims): a text box over an STML or plain-text child, colour
-- swatches over a child's decal tint, a mode switch (K.Segmented), the footer's button height and a centred first place. Like lib/widgets.lua,
-- what they show is the original child's and their clicks go to it.
--
--   local K = dofile("lib/dialogs_kit.lua")
--   K.TextBox("LargeDialogWindow", "LDW_TextBox", 0, -K.FooterH(s))   -- the dialog's text, scrolled
local API = hs.UI
local T = dofile("lib/theme.lua")
local C = T.color
local stml = dofile("lib/stml.lua")

local K = {}

-- The height of a footer button, and of the footer (the button and the gap above it).
function K.ButtonH(s)
  local _, lh = ui.CalcTextSize("Ag")
  return lh + 10 * s
end
function K.FooterH(s) return K.ButtonH(s) + 8 * s end

-- opts.place for a dialog that opens in the middle of the screen (a question, a prompt).
function K.Centered(W, H, w, h) return W / 2 - w / 2, H * 0.42 - h / 2 end

-- A text box: an inset, padded, scrolled area with the child's text. opts.plain: the text as lines (a book's
-- page) rather than STML; opts.font; opts.color (the text's colour outside any <c>); opts.bg (the inset's colour);
-- opts.border = false (no hairline: text that sits on the window itself); opts.empty (shown while there is no text).
-- w, h as for ui.BeginChild (0: the rest; negative: leave that much). Links go to the window (hs.StmlLink). A new
-- text is shown from its top.
local cache = {}
function K.TextBox(win, id, w, h, opts)
  opts = opts or {}
  local raw = API.windows.Text(win, id) or ""
  local byId = cache[win]
  if not byId then byId = {}; cache[win] = byId end
  local c = byId[id]
  local changed = c and c.raw ~= raw
  if not c or c.raw ~= raw or c.plain ~= opts.plain then
    local lines = nil
    if not opts.plain then lines = stml.Lines(raw, opts.color) end
    -- (the link handler is made with the lines, not once a line a frame)
    c = { raw = raw, plain = opts.plain, lines = lines,
      onLink = function(link) API.windows.StmlLink(win, id, link[1], link[2]) end }
    byId[id] = c
  end
  local bg = opts.bg or C.inset
  ui.PushStyleColor(ui.Col.ChildBg, bg[1], bg[2], bg[3], bg[4] or 1)
  ui.PushStyleColor(ui.Col.Border, C.ink[1], C.ink[2], C.ink[3], opts.border == false and 0 or 0.06)
  local s = ui.GetIO().uiScale or 1
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 10 * s, 8 * s)
  ui.BeginChild("##text" .. id, w or 0, h or 0, true)
  ui.PopStyleVar()
  if changed then ui.SetScrollY(0) end -- a new text (a link followed to another page) starts at its top
  if opts.font then ui.PushFont(opts.font) end
  if opts.plain then
    local col = opts.color or C.text
    for line in (raw .. "\n"):gmatch("(.-)\n") do ui.TextColored(col[1], col[2], col[3], 1, line) end
  else
    local width = ui.GetContentRegionAvail()
    for _, line in ipairs(c.lines) do stml.DrawLine(line, width, c.onLink) end
  end
  if raw == "" and opts.empty then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, opts.empty) end
  if opts.font then ui.PopFont() end
  ui.EndChild()
  ui.PopStyleColor(2)
end

-- A colour swatch at the cursor: a rounded square in r, g, b with a hairline (a brighter ring while hovered or
-- `selected`). Returns clicked. opts.tooltip, opts.selected, opts.width (default: size).
function K.Swatch(id, r, g, b, size, s, opts)
  opts = opts or {}
  local x, y = ui.GetCursorScreenPos()
  local sw = opts.width or size
  local clicked = ui.InvisibleButton(id, sw, size)
  local hovered = ui.IsItemHovered()
  local rad = 4 * s
  ui.DrawRectFilled(x, y, x + sw, y + size, r, g, b, 1, rad)
  local ring = opts.selected and C.accent or (hovered and C.borderHi or C.border)
  ui.DrawRect(x, y, x + sw, y + size, ring[1], ring[2], ring[3], opts.selected and 1 or ring[4], rad,
    (opts.selected or hovered) and 2 or 1)
  if hovered and opts.tooltip then ui.SetTooltip(opts.tooltip) end
  return clicked
end

-- A child's decal tint (a tint or colour picker swatch), else white.
function K.Tint(win, id)
  local r, g, b = API.windows.ChildColor(win, id, "decal")
  if not r then return 1, 1, 1 end
  return r, g, b
end

-- "#RRGGBB" for a colour of 0..1 parts.
function K.Hex(r, g, b)
  local function p(v) return math.floor(math.max(0, math.min(1, v)) * 255 + 0.5) end
  return string.format("#%02X%02X%02X", p(r), p(g), p(b))
end

-- Two or more buttons as one control (a mode switch): `selected` lit; returns the index clicked, nil for none or the
-- selected one. tips: a tooltip for each.
function K.Segmented(id, items, selected, w, h, s, tips)
  local hit = nil
  local x, y = ui.GetCursorScreenPos()
  local bw = w / #items
  local a = C.accent
  ui.DrawRectFilled(x, y, x + w, y + h, C.inset[1], C.inset[2], C.inset[3], C.inset[4] or 1, 6 * s)
  for i, label in ipairs(items) do
    local bx = x + (i - 1) * bw
    ui.SetCursorScreenPos(bx, y)
    if ui.InvisibleButton(id .. i, bw, h) and i ~= selected then hit = i end
    local hovered = ui.IsItemHovered()
    if i == selected then
      ui.DrawRectFilled(bx + 2 * s, y + 2 * s, bx + bw - 2 * s, y + h - 2 * s, a[1], a[2], a[3], 0.35, 5 * s)
    elseif hovered then
      ui.DrawRectFilled(bx + 2 * s, y + 2 * s, bx + bw - 2 * s, y + h - 2 * s, C.text[1], C.text[2], C.text[3], 0.07,
        5 * s)
    end
    local tw, th = T.TextSize(label, T.font.small)
    T.Text(bx + (bw - tw) / 2, y + (h - th) / 2, label, i == selected and C.text or C.muted, T.font.small)
    if hovered and tips and tips[i] then ui.SetTooltip(tips[i]) end
  end
  return hit
end

-- A label in the muted caption colour.
function K.Caption(text)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, text)
end

return K
