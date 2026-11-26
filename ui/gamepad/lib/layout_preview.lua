-- Read-only layout geometry. Keep the rendering shared in behavior between the shipped packs.
local V = {}

function V.prepare(profile)
  local view = { name = profile.name, width = profile.width, height = profile.height, boxes = {} }
  if not view.width or not view.height or view.width <= 0 or view.height <= 0 then return view end

  for _, r in ipairs(profile.rects or {}) do
    local x0, y0 = math.max(0, r.x), math.max(0, r.y)
    local x1, y1 = math.min(view.width, r.x + r.w), math.min(view.height, r.y + r.h)
    if x1 > x0 and y1 > y0 then
      view.boxes[#view.boxes + 1] = { name = r.name, x0 = x0, y0 = y0, x1 = x1, y1 = y1,
        area = (x1 - x0) * (y1 - y0) }
    end
  end
  table.sort(view.boxes, function(a, b)
    if a.area == b.area then return a.name < b.name end
    return a.area > b.area
  end)
  return view
end

function V.draw(view, width, height, background, border, accent, text)
  if #view.boxes == 0 then
    ui.TextWrapped("No window positions are available. Save this layout again to capture a visual preview.")
    return
  end
  if width < 1 or height < 1 then return end

  local x, y = ui.GetCursorScreenPos()
  local scale = math.min(width / view.width, height / view.height)
  local w, h = view.width * scale, view.height * scale
  x, y = x + (width - w) * 0.5, y + (height - h) * 0.5
  ui.Dummy(width, height)
  ui.DrawRectFilled(x, y, x + w, y + h, background[1], background[2], background[3], 1)
  ui.DrawRect(x, y, x + w, y + h, border[1], border[2], border[3], 1, 0, 1)
  ui.PushClipRect(x, y, x + w, y + h, true)

  local mx, my = ui.GetMousePos()
  local mouseOver = ui.IsWindowHovered()
  local hovered
  for _, box in ipairs(view.boxes) do
    local x0, y0 = x + box.x0 * scale, y + box.y0 * scale
    local x1, y1 = x + box.x1 * scale, y + box.y1 * scale
    ui.DrawRectFilled(x0, y0, x1, y1, accent[1], accent[2], accent[3], 0.15)
    ui.DrawRect(x0, y0, x1, y1, accent[1], accent[2], accent[3], 0.86, 0, 1)
    local tw, th = ui.CalcTextSize(box.name)
    if tw + 8 <= x1 - x0 and th + 6 <= y1 - y0 then
      ui.DrawText(x0 + 4, y0 + 3, text[1], text[2], text[3], 1, box.name)
    end
    if mouseOver and mx >= x0 and mx < x1 and my >= y0 and my < y1 then hovered = box.name end
  end
  ui.PopClipRect()
  if hovered then ui.SetTooltip(hovered) end
end

return V
