-- Spell bar: replaces the game's CastSpellWnd. The memorized spells as a row (or column) of icon tiles with their
-- category colour, the refresh shading over a gem that is not ready, and the spell book button at the end.
-- The gems act as the game's: click casts (or memorizes the spell on the cursor), a quick right click forgets the
-- spell, holding the right button shows its display; releasing over it keeps it open. Dragging makes a hot button.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "CastSpellWnd"
local GEMS = 12
local VERTICAL = "hud.vertical." .. WINDOW

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
local size, gap = 40, 5
local vert = false

local function collect()
  count = 0
  for _, g in ipairs(all) do
    if API.spells.GemInfo(WINDOW, g.id, g.info).shown ~= false then
      count = count + 1
      gems[count] = g
    end
  end
end

local opts = {
  size = function()
    collect()
    vert = ui.GetSetting(VERTICAL) == "1"
    local long = (count + 1) * (size + gap) - gap + 16 -- and the book
    if vert then return size + 16, long end
    return long, size + 16
  end,
  menu = function()
    if ui.MenuItem("Vertical", vert) then ui.SetSetting(VERTICAL, vert and "0" or "1") end
  end,
}

local function body(x, y, w, h, s)
  local function at(k) -- the k-th tile's corner (0-based)
    local d = 8 * s + k * (size + gap) * s
    if vert then return x + 8 * s, y + d end
    return x + d, y + 8 * s
  end
  local sz = size * s
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
    local clicked = T.Hit(g.hit, gx, gy, gx + sz, gy + sz)
    local key, hovered = ui.GetItemID(), ui.IsItemHovered()
    -- a spell dragged from the spell book (spellbook.lua) memorizes into this gem
    -- (this frame's only: a book closed mid-drag, or the character gone, leaves a stale one)
    local dragged = modern_spell_drag_frame == ui.GetFrameCount() and modern_spell_drag or nil
    -- (only while this bar is the window under the mouse: the book's own gems under it take the drop instead)
    if dragged and ui.IsMouseHoveringRect(gx, gy, gx + sz, gy + sz)
        and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows | ui.HoveredFlags.AllowWhenBlockedByActiveItem) then
      ui.DrawRect(gx - 2 * s, gy - 2 * s, gx + sz + 2 * s, gy + sz + 2 * s, C.accent[1], C.accent[2], C.accent[3], 1, 7 * s, 2 * s)
      if ui.IsMouseReleased(ui.Mouse.Left) and not dragged.gem then API.spells.BookMemorize(dragged.slot, g.n - 1) end
    end
    T.HoverFx(gx, gy, gx + sz, gy + sz, s, 6)
    if hovered then
      local tip = info.tooltip
      if tip and tip ~= "" then ui.SetTooltip(tip) end
    end
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
end

ui.window("Spell Gems", function()
  if not API.session.InGame() then return end
  hud.Window(WINDOW, "Spell Gems", opts, body)
end)
