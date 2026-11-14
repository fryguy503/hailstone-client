-- Small, original line icons for server-confirmed group roles. Kept the same in both UI packs.
local R = {}
R.Labels = { [0] = "", "Main Tank", "Main Assist", "Main Tank / Main Assist", "Puller",
  "Main Tank / Puller", "Main Assist / Puller", "Main Tank / Main Assist / Puller" }
local NAMES = { "Main Tank", "Main Assist", "Puller" }

function R.Mask(window, suffix)
  suffix = suffix or ""
  return (eq.ChildShown(window, "GroupRoleTank" .. suffix) == true and 1 or 0)
    | (eq.ChildShown(window, "GroupRoleAssist" .. suffix) == true and 2 or 0)
    | (eq.ChildShown(window, "GroupRolePuller" .. suffix) == true and 4 or 0)
end

function R.Width(mask, size, gap)
  local n = ((mask & 1) ~= 0 and 1 or 0) + ((mask & 2) ~= 0 and 1 or 0) + ((mask & 4) ~= 0 and 1 or 0)
  return n > 0 and n * size + (n - 1) * gap or 0
end

function R.Draw(mask, x, y, size, gap, colors)
  local hovered = false
  for role = 1, 3 do
    if (mask & (1 << (role - 1))) ~= 0 then
      local c = colors[role]
      local a, t = c[4] or 1, math.max(1, size * 0.09)
      local function line(x0, y0, x1, y1)
        ui.DrawLine(x + x0 * size, y + y0 * size, x + x1 * size, y + y1 * size, c[1], c[2], c[3], a, t)
      end
      if role == 1 then -- shield
        ui.DrawRectFilled(x + size * 0.2, y + size * 0.15, x + size * 0.8, y + size * 0.5,
          c[1], c[2], c[3], 0.2 * a, 0)
        ui.DrawTriangleFilled(x + size * 0.2, y + size * 0.5, x + size * 0.8, y + size * 0.5,
          x + size * 0.5, y + size * 0.9, c[1], c[2], c[3], 0.2 * a)
        line(0.2, 0.15, 0.8, 0.15)
        line(0.8, 0.15, 0.8, 0.5)
        line(0.8, 0.5, 0.65, 0.75)
        line(0.65, 0.75, 0.5, 0.9)
        line(0.5, 0.9, 0.35, 0.75)
        line(0.35, 0.75, 0.2, 0.5)
        line(0.2, 0.5, 0.2, 0.15)
      elseif role == 2 then -- crosshair
        ui.DrawCircle(x + size * 0.5, y + size * 0.5, size * 0.24, c[1], c[2], c[3], a, t)
        line(0.5, 0.05, 0.5, 0.32)
        line(0.5, 0.68, 0.5, 0.95)
        line(0.05, 0.5, 0.32, 0.5)
        line(0.68, 0.5, 0.95, 0.5)
      else -- forward arrow
        line(0.12, 0.5, 0.85, 0.5)
        line(0.53, 0.18, 0.85, 0.5)
        line(0.85, 0.5, 0.53, 0.82)
      end
      if ui.IsWindowHovered() and ui.IsMouseHoveringRect(x, y, x + size, y + size) then
        ui.SetTooltip(NAMES[role])
        hovered = true
      end
      x = x + size + gap
    end
  end
  return hovered
end

return R
