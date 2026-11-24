-- A block of STML (a description: <BR> lines, <c> colours) word-wrapped in a box, scrolled with the right stick
-- while its window has the focus (or the mouse wheel over it). No item of its own: the pad's focus stays on the
-- window's list.
--
--   local TB = dofile("lib/textbox.lua")
--   TB.draw(stml, x0, y0, x1, y1, state)   -- state = {} kept by the caller (its scroll); a new text scrolls to the top
local T = dofile("lib/theme.lua")
local S = dofile("lib/stml.lua")
local pad = dofile("lib/pad.lua")
local TB = {}

function TB.draw(stml, x0, y0, x1, y1, state)
  T.frame() -- (the theme's scale)
  if state.text ~= stml then state.text, state.scroll = stml, 0 end
  local _, lh = ui.CalcTextSize("Ag")
  local rows = S.blockRows(stml, x1 - x0 - T.px(14)) -- (laid out again only when the text, width or font change)
  local total = #rows * lh
  local maxScroll = math.max(0, total - (y1 - y0))
  -- the right stick (menu mode) or the wheel scrolls it
  if ui.IsWindowFocused(3) and pad.mode() == "menu" then
    local ry = pad.axis("ry") or 0
    if math.abs(ry) > 0.2 then state.scroll = (state.scroll or 0) + ry * T.px(900) / math.max(20, ui.GetIO().fps or 60) end
  end
  if T.mouseOver(x0, y0, x1, y1) then
    local wheel = ui.GetIO().wheel or 0
    if wheel ~= 0 then state.scroll = (state.scroll or 0) - wheel * lh * 3 end
  end
  state.scroll = math.max(0, math.min(maxScroll, state.scroll or 0))
  ui.PushClipRect(x0, y0, x1, y1, true)
  local y = y0 - state.scroll
  for _, r in ipairs(rows) do
    if y + lh >= y0 and y <= y1 then S.drawRow(r, x0, y) end
    y = y + lh
  end
  ui.PopClipRect()
  if maxScroll > 0 then -- a thin scroll bar, and that the stick scrolls it
    local sx = x1 - T.px(6)
    local h = y1 - y0
    T.rect(sx, y0, x1, y1, T.C.sunken, 1, T.px(3))
    local th = math.max(T.px(30), h * h / total)
    local ty = y0 + (h - th) * (state.scroll / maxScroll)
    T.rect(sx, ty, x1, ty + th, T.C.dim, 0.8, T.px(3))
  end
  return maxScroll > 0
end

return TB
