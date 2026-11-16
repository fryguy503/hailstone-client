-- Text tiles in a grid that reflows to the width (the Actions window's buttons, the map toolbar's tools), and an
-- icon well for a child's own icon (the bandolier's weapons, the potion belt, blocked spells). Every tile is a game
-- window's child: a click goes to it (hs.ClickChild), a drag picks up the hot button it makes where the window lets
-- it (hs.PickUp: the Actions window's buttons), so the window's own handler runs.
--
--   local tiles = dofile("lib/hudx_tiles.lua")
--   tiles.Grid(win, list, { minW = 96, h = 30 })   -- list = { { id, label, opts }, ... }
local API = hs.UI
local T = dofile("lib/theme.lua")
local C = T.color

local tiles = {}

-- One tile at the cursor, w x h screen pixels. opts: on (lit: a toggle that is set), enabled (default: the
-- child's), empty (a placeholder look, `label` drawn faint), danger, tooltip, right (a right click goes to the child
-- as "right"), drag (a drag picks up the child's hot button), sub (a second, small line under the label).
-- Returns "left" / "right" when it was clicked (the click already went to the game).
function tiles.Tile(win, id, label, w, h, s, opts)
  opts = opts or T.NO_OPTS
  local e, checked = API.windows.ChildState(win, id)
  local enabled = opts.enabled
  if enabled == nil then enabled = e ~= false end
  local on = opts.on
  if on == nil then on = checked == true end
  local x, y = ui.GetCursorScreenPos()
  ui.PushID(id)
  local clicked = ui.InvisibleButton("##tile", math.max(w, 1), math.max(h, 1))
  local hovered, active = ui.IsItemHovered(), ui.IsItemActive()
  local dragged = opts.drag and T.DragFrom(win, id) or false
  local rclick = ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt
  ui.PopID()
  local rad = 6 * s
  local base = (opts.danger and C.danger) or ((on or opts.primary) and C.accent) or C.ink
  local fill = (on and 0.24) or (opts.primary and 0.16) or (opts.danger and 0.10) or (opts.empty and 0.02 or 0.05)
  if enabled and active then fill = fill + 0.10 elseif enabled and hovered then fill = fill + 0.06 end
  if not enabled then fill = 0.025 end
  ui.DrawRectFilled(x, y, x + w, y + h, base[1], base[2], base[3], fill, rad)
  local edge = enabled and ((hovered and 0.60) or (on and 0.55) or 0.14) or 0.06
  if hovered and enabled and not on and not opts.danger then base = C.accent end
  ui.DrawRect(x, y, x + w, y + h, base[1], base[2], base[3], edge, rad, (on or hovered) and 1.5 or 1)
  if on then -- a lit toggle: a small dot at the left
    ui.DrawCircleFilled(x + 8 * s, y + h / 2, 3 * s, C.accent[1], C.accent[2], C.accent[3], 1)
  end
  -- the cool-down the game set on the child (CButtonWnd::SetCoolDown: the Actions window's skills and socials):
  -- the time still to go, shaded, shrinking toward the bottom
  local top = API.spells.ButtonCoolDown(win, id, y, y + h)
  if top then ui.DrawRectFilled(x, top, x + w, y + h, 0, 0, 0, 0.45, rad) end
  local color = (not enabled and C.faint) or (opts.empty and C.faint) or (opts.danger and C.danger) or C.text
  local font = T.font.small
  local text = T.Fit(label, w - (on and 22 or 10) * s, font)
  local _, th = T.TextSize("Ag", font)
  if opts.sub and opts.sub ~= "" then
    local _, sh = T.TextSize("Ag", T.font.tiny)
    local ty = y + (h - th - sh) / 2
    T.Text(x + w / 2, ty, text, color, font, "center")
    T.Text(x + w / 2, ty + th, T.Fit(opts.sub, w - 10 * s, T.font.tiny), C.muted, T.font.tiny, "center")
  else
    T.Text(x + w / 2 + (on and 5 * s or 0), y + (h - th) / 2, text, color, font, "center")
  end
  if hovered and opts.tooltip and not dragged then ui.SetTooltip(opts.tooltip) end
  if dragged then return nil end
  if clicked and enabled then
    API.windows.ClickChild(win, id, "left")
    return "left"
  end
  if rclick and opts.right then
    API.windows.ClickChild(win, id, "right")
    T.TakeRightClick()
    return "right"
  end
  return nil
end

-- Tiles in rows that share the width: as many columns as fit tiles opts.minW wide (at most opts.maxCols), each
-- opts.h high. list = { { id, label, tileOpts }, ... }; a child the window hides is left out (opts.keepHidden: kept).
-- Returns the id clicked and the button.
function tiles.Grid(win, list, opts)
  opts = opts or T.NO_OPTS
  local s = ui.GetIO().uiScale or 1
  local gap = (opts.gap or 6) * s
  local shown = {}
  for _, t in ipairs(list) do
    if opts.keepHidden or API.windows.ChildShown(win, t[1]) ~= false then shown[#shown + 1] = t end
  end
  local avail = ui.GetContentRegionAvail()
  local cols = math.max(1, math.floor((avail + gap) / ((opts.minW or 96) * s + gap)))
  if opts.maxCols then cols = math.min(cols, opts.maxCols) end
  cols = math.min(cols, math.max(#shown, 1))
  local tw = math.floor((avail - gap * (cols - 1)) / cols)
  local th = (opts.h or 30) * s
  local x0, y0 = ui.GetCursorScreenPos()
  local hit, button = nil, nil
  for i, t in ipairs(shown) do
    local col, row = (i - 1) % cols, (i - 1) // cols
    ui.SetCursorScreenPos(x0 + col * (tw + gap), y0 + row * (th + gap))
    local b = tiles.Tile(win, t[1], t[2], tw, th, s, t[3])
    if b then hit, button = t[1], b end
  end
  local rows = math.ceil(#shown / cols)
  ui.SetCursorScreenPos(x0, y0)
  ui.Dummy(avail, math.max(rows * th + (rows - 1) * gap, 0)) -- the grid's size, for the layout after it
  return hit, button
end

-- A child's icon (hs.ChildArt part "decal", tinted as the window tints it) in a modern well at x, y, size square;
-- ring: the well's ring colour. Returns whether the child shows an icon.
function tiles.Icon(win, id, x, y, size, s, ring, opts)
  local tex, u0, v0, u1, v1 = API.windows.ChildArt(win, id, "decal")
  if type(tex) ~= "userdata" then tex = nil end -- (false: nothing shown; a string: stock art, not drawn here)
  T.Icon(x, y, size, s, tex, u0, v0, u1, v1, ring, opts)
  return tex ~= nil
end

return tiles
