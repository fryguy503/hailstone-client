-- Spell bar: replaces the game's CastSpellWnd. The memorized spells as icon tiles with their category colour,
-- the refresh shading over a gem that is not ready, and the spell book button at the end. A row or column by
-- default, with optional names and wrapping from the frame's menu.
-- The gems act as the game's: click casts (or memorizes the spell on the cursor), a quick right click forgets the
-- spell, holding the right button shows its display; releasing over it keeps it open. Dragging makes a hot button.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "CastSpellWnd"
local GEMS = 12
local VERTICAL = "hud.vertical." .. WINDOW
local NAMES = "hud.gemNames." .. WINDOW
local HIDE_BACKGROUND = "hud.gemHideBackground." .. WINDOW
local RESIZE = "hud.gemResize." .. WINDOW
local WRAP = "hud.gemWrap." .. WINDOW
local ICON_SIZE = "hud.icon." .. WINDOW
local DEFAULT_SIZE, GAP, PAD, NAME_SPACE = 40, 5, 8, 120

-- Find this spell's visible display, not an old ImGui rectangle left behind by a closed window.
local function overSpellDisplay(spell)
  local title = API.spells.Details(spell)
  if not title then return false end
  for n = 1, 6 do
    local window = n == 1 and "SpellDisplayWindow" or ("SpellDisplayWindow#" .. n)
    if API.windows.Shown(window) and API.windows.Title(window) == title then
      local x, y, w, h = ui.FindWindowRect("###modern." .. window)
      local mx, my = ui.GetMousePos()
      if x and mx >= x and mx < x + w and my >= y and my < y + h then return true end
    end
  end
  return false
end

-- Each gem's: its child, number, the record hs.GemInfo fills (its looks this frame), its tint ring and right button.
local all = {}
for i = 0, GEMS - 1 do
  local id = "CSPW_Spell" .. i
  local heldSpell
  all[i + 1] = {
    id = id, hit = "##" .. id, n = i + 1, info = {}, ring = { 1, 1, 1, 0.9 }, icon = { radius = 6 },
    -- (a quick right click forgets the spell only when let go on the gem: slid off, or the gem gone mid-press
    -- (T.RightPressFrame's outside), it does nothing, as a hot button's)
    onClick = function(phase, outside)
      if phase == "rightup" and outside then return end
      if phase == "rightheld" then
        local state = API.spells.BookState()
        heldSpell = state and state.gems and state.gems[i + 1]
      end
      -- Hailstone: releasing over the display keeps it; elsewhere it closes as in the original.
      if phase == "rightupheld" then
        local spell = heldSpell
        heldSpell = nil
        local keep = spell and spell ~= -1 and overSpellDisplay(spell)
        API.spells.GemClick(id, phase)
        if keep then API.spells.Show(spell) end
        return
      end
      API.spells.GemClick(id, phase)
    end,
  }
end
local gems, count = {}, 0 -- the gems the game shows this frame, and how many (opts.size reads them first)
local size, cellWidth, padding, tileScale, minWrap, maxWrap, perLine, columns, rows
local vert, names

local function collect()
  count = 0
  for _, g in ipairs(all) do
    if API.spells.GemInfo(WINDOW, g.id, g.info).shown ~= false then
      count = count + 1
      gems[count] = g
    end
  end
end

local opts, minimum = {}, {}

local function span(tiles, cell)
  return tiles * (cell + GAP) - GAP
end

local function fit(length, cell)
  return math.floor(((length - padding) / tileScale + GAP) / (cell + GAP) + 1e-6)
end

-- A small viewport at a large HUD scale might not fit any grid. Scale the tiles just enough to keep the last gem
-- and the book reachable, without changing the character's saved icon size or reducing the grip area.
local function scaleToFit(w, h, tiles)
  local best = 0
  for cols = 1, tiles do
    local nrows = math.ceil(tiles / cols)
    best = math.max(best, math.min((w - padding) / span(cols, cellWidth), (h - padding) / span(nrows, size)))
  end
  return best > 0 and best or 0.1
end

function opts.size(s)
  collect()
  vert = ui.GetSetting(VERTICAL) == "1"
  names = ui.GetSetting(NAMES) == "1"
  opts.panel = ui.GetSetting(HIDE_BACKGROUND) ~= "1" -- the original panel stays on by default
  local resize = ui.GetSetting(RESIZE) == "1"
  size = math.max(22, math.min(64, math.tointeger(tonumber(ui.GetSetting(ICON_SIZE))) or DEFAULT_SIZE))
  cellWidth = size + (names and NAME_SPACE or 0)
  -- The right and bottom edge padding must also fit the corner grip at small window scales.
  padding = PAD + (resize and math.max(14, math.ceil(12 / s)) or PAD)
  local tiles = count + 1 -- including the spell book
  local saved = math.tointeger(tonumber(ui.GetSetting(WRAP)))
  tileScale = 1
  minWrap, maxWrap = 1, tiles
  if names or resize or saved or size ~= DEFAULT_SIZE then
    local io = ui.GetIO()
    local w, h = io.width / s, io.height / s
    local cw, ch = math.max(0, fit(w, cellWidth)), math.max(0, fit(h, size))
    if cw * ch < tiles then
      tileScale = scaleToFit(w, h, tiles)
      cw, ch = math.max(1, fit(w, cellWidth)), math.max(1, fit(h, size))
    end
    local along, across = cw, ch
    if vert then along, across = ch, cw end
    maxWrap = math.min(tiles, along)
    minWrap = math.min(maxWrap, math.ceil(tiles / across))
  end
  local automatic = names and not vert and 4 or tiles
  perLine = math.max(minWrap, math.min(maxWrap, saved or automatic))
  columns, rows = perLine, math.ceil(tiles / perLine)
  if vert then columns, rows = rows, columns end
  minimum[1], minimum[2] = cellWidth * tileScale + padding, size * tileScale + padding
  opts.resizable = resize and minimum or nil
  return span(columns, cellWidth) * tileScale + padding, span(rows, size) * tileScale + padding
end

function opts.onResize(w, h, dx, dy)
  -- Choose the axis from the drag's origin, so a wrap changing the other axis cannot switch it mid-drag.
  local horizontal = math.abs(dx) >= math.abs(dy)
  local length, cell, current = w, cellWidth, columns
  if not horizontal then length, cell, current = h, size, rows end
  local nextCount = math.max(1, fit(length + (cell + GAP) * tileScale / 2, cell))
  if nextCount == current then return end
  if horizontal == vert then nextCount = math.ceil((count + 1) / nextCount) end
  nextCount = math.max(minWrap, math.min(maxWrap, nextCount))
  if nextCount ~= perLine then ui.SetSetting(WRAP, nextCount) end
end

function opts.onResetSize()
  ui.SetSetting(WRAP, "")
  ui.SetSetting(ICON_SIZE, "")
end

function opts.menu()
  if ui.MenuItem("Vertical", vert) then ui.SetSetting(VERTICAL, not vert) end
  ui.SetNextItemWidth(180)
  local v, changed = ui.SliderInt("Icon size", size, 22, 64)
  if changed then ui.SetSetting(ICON_SIZE, v) end
  ui.Separator()
  if ui.MenuItem("Show spell names", names) then ui.SetSetting(NAMES, not names) end
  if ui.MenuItem("Show window background", opts.panel) then ui.SetSetting(HIDE_BACKGROUND, opts.panel) end
  local resize = opts.resizable ~= nil
  if ui.MenuItem("Show resize bar", resize) then ui.SetSetting(RESIZE, not resize) end
end

local function body(x, y, w, h, s)
  local inset = PAD * s
  local fontScale
  if tileScale ~= 1 then
    fontScale = s / (ui.GetIO().uiScale or 1)
    s = s * tileScale
    ui.SetWindowFontScale(fontScale * tileScale)
  end
  local function at(k) -- the k-th tile's corner (0-based)
    local col, row = k % perLine, k // perLine
    if vert then col, row = row, col end
    return x + inset + col * (cellWidth + GAP) * s, y + inset + row * (size + GAP) * s
  end
  local sz = size * s
  local labelWidth = (NAME_SPACE - 12) * s
  local lineHeight
  if names then
    local _, lh = T.TextSize("Ag", T.font.small)
    lineHeight = lh
  end
  for k = 1, count do
    local g = gems[k]
    local info = g.info
    local gx, gy = at(k - 1)
    local tex = info.tex
    if type(tex) ~= "userdata" then tex = nil end
    local ring = nil
    local ia = 1
    if tex then
      ring = g.ring
      ring[1], ring[2], ring[3] = info.tintR or 1, info.tintG or 1, info.tintB or 1
      ia = info.tintA or 1
    end
    g.icon.alpha = ia
    T.Icon(gx, gy, sz, s, tex, info.u0, info.v0, info.u1, info.v1, ring, g.icon)
    if tex then
      -- the refresh: the game's cool-down rectangle, from `top` to the bottom (hs.GemOverlay only while there is
      -- one: its top is worked out from where the tile is)
      local top = info.overlay and API.spells.GemOverlay(WINDOW, g.id, gy, gy + sz)
      if top and top < gy + sz then
        ui.DrawRectFilled(gx, math.max(top, gy), gx + sz, gy + sz, 0, 0, 0, 0.62, 6 * s)
        ui.DrawLine(gx + 2 * s, math.max(top, gy), gx + sz - 2 * s, math.max(top, gy), C.accent[1], C.accent[2], C.accent[3], 0.9, 1.5 * s)
      end
    else
      T.Text(gx + sz / 2, gy + sz / 2 - 7 * s, tostring(g.n), C.faint, T.font.small, "center")
    end
    local tip = info.tooltip
    if names and tip and tip ~= "" then
      if g.nameSource ~= tip or g.nameWidth ~= labelWidth then
        g.nameSource, g.nameWidth = tip, labelWidth
        g.label = T.Fit(tip, labelWidth, T.font.small)
      end
      T.Text(gx + sz + 6 * s, gy + (sz - lineHeight) / 2, g.label, C.text, T.font.small)
    end
    local right = gx + cellWidth * s
    local clicked = T.Hit(g.hit, gx, gy, right, gy + sz)
    local key, hovered = ui.GetItemID(), ui.IsItemHovered()
    -- a spell dragged from the spell book (spellbook.lua) memorizes into this gem
    -- (this frame's only: a book closed mid-drag, or the character gone, leaves a stale one)
    local dragged = modern_spell_drag_frame == ui.GetFrameCount() and modern_spell_drag or nil
    -- (only while this bar is the window under the mouse: the book's own gems under it take the drop instead)
    if dragged and ui.IsMouseHoveringRect(gx, gy, right, gy + sz)
        and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows | ui.HoveredFlags.AllowWhenBlockedByActiveItem) then
      ui.DrawRect(gx - 2 * s, gy - 2 * s, gx + sz + 2 * s, gy + sz + 2 * s, C.accent[1], C.accent[2], C.accent[3], 1, 7 * s, 2 * s)
      if ui.IsMouseReleased(ui.Mouse.Left) and not dragged.gem then API.spells.BookMemorize(dragged.slot, g.n - 1) end
    end
    T.HoverFx(gx, gy, right, gy + sz, s, 6)
    if hovered and tip and tip ~= "" then ui.SetTooltip(tip) end
    if T.DragFrom(WINDOW, g.id) then clicked = false end
    if clicked then API.spells.GemClick(g.id, "left") end
    if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      T.RightPressStart(key, g.onClick)
      T.TakeRightClick()
    end
    T.RightPressFrame(key, g.onClick, hovered)
  end
  -- the spell book
  local bx, by = at(count)
  local book = vert and "CSPW_SpellBookV" or "CSPW_SpellBookH"
  local clicked = T.Hit("##book", bx, by, bx + sz, by + sz)
  ui.DrawRectFilled(bx, by, bx + sz, by + sz, C.ink[1], C.ink[2], C.ink[3], 0.05, 6 * s)
  ui.DrawRect(bx, by, bx + sz, by + sz, C.ink[1], C.ink[2], C.ink[3], 0.12, 6 * s, 1)
  -- a book glyph: two pages
  local cx, cy, pw, ph = bx + sz / 2, by + sz / 2, 11 * s, 15 * s
  ui.DrawRectFilled(cx - pw, cy - ph / 2, cx - 1 * s, cy + ph / 2, C.accent[1], C.accent[2], C.accent[3], 0.85, 2 * s)
  ui.DrawRectFilled(cx + 1 * s, cy - ph / 2, cx + pw, cy + ph / 2, C.accent[1], C.accent[2], C.accent[3], 0.85, 2 * s)
  T.HoverFx(bx, by, bx + sz, by + sz, s, 6)
  if ui.IsItemHovered() then ui.SetTooltip("Spell book\nRight-click: spell sets") end
  -- the book button's right click: the spell sets (CCastSpellWnd's loadout menu)
  if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
    ui.OpenPopup("##sets")
    T.TakeRightClick()
  end
  if ui.BeginPopup("##sets") then
    ui.TextDisabled("Spell sets")
    ui.Separator()
    local any = false
    for n, set in ipairs(API.spells.Sets() or {}) do
      if set.inUse then
        any = true
        if ui.MenuItem("Memorize " .. set.name .. "##set" .. n) then API.spells.MemorizeSet(n) end
      end
    end
    if not any then ui.TextDisabled("(none saved)") end
    ui.Separator()
    if ui.MenuItem("Save or manage sets...") then
      modern_book_tab = "sets"
      API.windows.ClickChild(WINDOW, book, "left")
    end
    ui.EndPopup()
  end
  if clicked then
    if not API.windows.ClickChild(WINDOW, book, "left") then API.windows.ClickChild(WINDOW, vert and "CSPW_SpellBookH" or "CSPW_SpellBookV", "left") end
  end
  if fontScale then ui.SetWindowFontScale(fontScale) end
end

ui.window("Spell Gems", function()
  if not API.session.InGame() then return end
  hud.Window(WINDOW, "Spell Gems", opts, body)
end)
