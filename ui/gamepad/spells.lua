-- Spells (gamepad pack):
--   Spell gems (CastSpellWnd)   a column of the twelve gems on the right edge of the HUD: icon, cool-down and
--                               fade as the stock gems draw them; a click casts (the pad casts from the cross
--                               hotbar or the CAST keys)
--   Spell book (SpellBookWnd)   the book's two pages and the gems side by side. A on a spell takes it (the book's
--                               own click: you must be sitting, as in the game), then A on a gem memorizes it there;
--                               X on a gem forgets its spell; LB / RB turn the pages. B closes the book.
--   Spell sets (Y in the book)  the character's sets as cards: A loads one onto the gems, X edits it; New set and
--                               From gems make one. The editor puts spells from the whole book into the set's
--                               twelve slots without memorizing anything; Y saves, B steps back.
-- The book and gems are clicks on the stock windows' children, so memorizing, scribing and casting follow the game;
-- the sets are hs.SpellSets and friends (src/app/lua_ui_spellbook.cpp).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local C = T.C

local GEMS = "CastSpellWnd"
local BOOK = "SpellBookWnd"
local GEM_IDS, gemInfo = {}, {} -- each gem's child, and the record hs.GemInfo fills for it
for g = 0, 11 do
  GEM_IDS[g] = "CSPW_Spell" .. g
  gemInfo[g] = {}
end
local tintEdge = { 1, 1, 1, 1 } -- a gem's tint, as its edge's colour

-- One gem at x0, y0 (size): its holder, icon, cool-down (hs.GemInfo: the gem's art, tint, cool-down and tooltip).
-- Returns the spell name ("" when empty).
local function drawGem(g, x0, y0, size, alpha)
  local x1, y1 = x0 + size, y0 + size
  local r = T.px(10)
  local info = API.spells.GemInfo(GEMS, GEM_IDS[g], gemInfo[g], y0, y1)
  local tex = info.tex
  local has = type(tex) == "userdata"
  T.rect(x0, y0, x1, y1, has and C.raised or C.sunken, alpha, r)
  if has then
    local tr, ia = info.tintR, info.tintA
    if tr then
      tintEdge[1], tintEdge[2], tintEdge[3] = tr, info.tintG, info.tintB
      T.outline(x0, y0, x1, y1, tintEdge, 0.9 * (alpha or 1), r, T.px(3))
    end
    local m = math.floor(size * 0.1)
    T.icon(tex, info.u0, info.v0, info.u1, info.v1, x0 + m, y0 + m, x1 - m, y1 - m, (ia or 1) * (alpha or 1))
    local top = info.top
    if top then ui.DrawRectFilled(x0, top, x1, y1, info.coolR, info.coolG, info.coolB, info.coolA or 0.6, r) end
  else
    T.font(14)
    T.textIn(x0, y0, x1, y1, tostring(g + 1), C.faint, 0.5)
    T.unfont()
  end
  T.outline(x0, y0, x1, y1, C.edge, alpha, r)
  return has and (info.tooltip or "") or ""
end

-- ---------------------------------------------------------------- the HUD's gem column
local hudGems = {} -- the gems the column shows this frame

ui.window("HUD Spell Gems", function()
  if not W.shown(GEMS) then return end
  T.frame()
  T.resetFonts()
  local size, gap = T.px(46), T.px(6)
  -- any spell memorized, and the gems the character may use (CanUseMemorizedSpellSlot: 8, then the SPA 326 slots;
  -- the stock window hides the rest)
  local any, count = false, 0
  for g = 0, 11 do
    local info = API.spells.GemInfo(GEMS, GEM_IDS[g], gemInfo[g])
    if type(info.tex) == "userdata" then any = true end
    if info.shown ~= false then
      count = count + 1
      hudGems[count] = g
    end
  end
  if not any then return end -- no spells memorized (a melee class): no column
  local h = count * size + (count - 1) * gap + T.px(20)
  local w = size + T.px(20)
  -- under the minimap and its clock (hud.lua: 20 + 352 design px), on the right edge
  ui.SetNextWindowPos(T.W - w - T.px(16), math.min(T.px(380), T.H - h - T.px(16)), ui.Cond.Always)
  ui.SetNextWindowSize(w, h, ui.Cond.Always)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoBackground | ui.WindowFlags.NoSavedSettings
      | ui.WindowFlags.NoMove | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoBringToFrontOnFocus | W.NoNav
  if ui.Begin("HUD Spell Gems", nil, flags) then
    local wx, wy = ui.GetWindowPos()
    T.panel(wx, wy, wx + w, wy + h, 0.7, T.px(16))
    for k = 1, count do
      local g = hudGems[k]
      local x0, y0 = wx + T.px(10), wy + T.px(10) + (k - 1) * (size + gap)
      ui.SetCursorPos(x0 - wx, y0 - wy)
      ui.PushID(g)
      local clicked = ui.InvisibleButton("##gem", size, size)
      local hovered = ui.IsItemHovered()
      ui.PopID()
      local name = drawGem(g, x0, y0, size)
      if hovered then
        T.outline(x0, y0, x0 + size, y0 + size, C.accent, 1, T.px(10), T.px(2))
        if name ~= "" then ui.SetTooltip(name) end
        if ui.IsMouseClicked(ui.Mouse.Right) then API.spells.GemClick(GEM_IDS[g], "right") end
      end
      if clicked then API.spells.GemClick(GEM_IDS[g], "left") end
    end
  end
  ui.End()
  ui.PopStyleVar(2)
end)

-- ---------------------------------------------------------------- the spell book
-- The pad's left / right between the gems and the pages, and between the pages, are made here: ImGui's navigation
-- does not cross from the gems to the pages. { gem = g } or { spell = m } (and the sets' { card = k }, { new = 1 },
-- the editor's { slot = k }, { espell = i }): focused next frame.
local jump = nil
local function jumpHere(kind, i)
  if jump and jump[kind] == i then
    ui.SetKeyboardFocusHere()
    pad.showCursor()
    jump = nil
  end
end

-- ---------------------------------------------------------------- spell sets
-- The book's Y page: the game's spell sets (hotbuttons.ini [SpellLoadouts], /memspellset) as cards, A loads one, X
-- edits it. The editor (Hailstone's own: hs.SaveSpellSet(name, spells) / hs.EditSpellSet) puts spells into a set's
-- twelve slots without memorizing them: A on a spell then A on a slot (or A on an empty slot, then the spell),
-- X empties a slot, A on a full slot then on another swaps the two; Y saves.
local SLOTS = 12
local view = "book"     -- "book", "sets" or "edit"
local ed = nil          -- the set being edited: { n (nil: new), name, spells, saved, held = { id } / { slot }, target }
local state = {}        -- the question (W.confirm)
local kb = K.new()
local flash = nil       -- { text, until }: the sets page's last word
local book, bookRead = {}, -1
local byId = {}
local edPage = 0
local lastDrawn = -1
local testView = nil    -- the last test.book.view applied

local function refreshBook()
  if ui.GetTime() - bookRead < 0.25 then return end
  bookRead = ui.GetTime()
  book, byId = API.spells.Book() or {}, {}
  for _, e in ipairs(book) do byId[e.id] = e end
  table.sort(book, function(a, b)
    if a.level ~= b.level then return a.level < b.level end
    return a.name:lower() < b.name:lower()
  end)
end

local function copySpells(t)
  local c = {}
  for k = 1, SLOTS do c[k] = (t and t[k]) or -1 end
  return c
end

-- A set memorizes a spell once (MemorizeSet keeps the later gem): placed in slot k it leaves any other.
local function putSpell(spells, k, id)
  for j = 1, SLOTS do
    if j ~= k and spells[j] == id then spells[j] = -1 end
  end
  spells[k] = id
end

local function spellName(id)
  local e = byId[id]
  return e and e.name or ("Spell " .. id .. " (not in your book)")
end

local function iconOf(id)
  local e = byId[id]
  if not e then return nil end
  return API.spells.Icon(e.icon)
end

local function openEditor(n, set, fromGems)
  local st = API.spells.BookState() or {}
  local spells = set and copySpells(set.spells) or copySpells(fromGems and st.gems or nil)
  ed = { n = n, name = set and set.name or "", spells = spells }
  ed.saved = { name = ed.name, spells = copySpells(spells) }
  view, edPage = "edit", 0
  if n == nil then K.open(kb, "Name the new spell set", "", 24, true) else jump = { slot = 1 } end
end

local function dirty()
  if not ed then return false end
  if ed.name ~= ed.saved.name then return true end
  for k = 1, SLOTS do
    if ed.spells[k] ~= ed.saved.spells[k] then return true end
  end
  return false
end

local function trim(t) return (t or ""):match("^%s*(.-)%s*$") end

-- What saving would do: canSave, the set it replaces (another of that name), whether a new one has room.
local function saveCheck(sets)
  local name = trim(ed.name)
  local clash, free = nil, false
  for n, set in ipairs(sets) do
    if not set.inUse then free = true
    elseif n ~= ed.n and set.name:lower() == name:lower() then clash = set.name end
  end
  return name ~= "" and (ed.n ~= nil or clash ~= nil or free), clash, free
end

local function save(sets)
  if not saveCheck(sets) then return end
  local name = trim(ed.name)
  local at
  if ed.n then at = API.spells.EditSet(ed.n, name, ed.spells) else at = API.spells.SaveSet(name, ed.spells) end
  if at then
    flash = { "Saved " .. name, ui.GetTime() + 3 }
    ed, view = nil, "sets"
    jump = { card = at }
  end
end

-- A row of a set's spell icons from x0, y0, `size` each.
local function setIcons(spells, x0, y0, size, gap)
  for k = 1, SLOTS do
    local ix = x0 + (k - 1) * (size + gap)
    local id = spells[k] or -1
    T.rect(ix, y0, ix + size, y0 + size, id ~= -1 and C.raised or C.sunken, 1, T.px(6))
    if id ~= -1 then
      local tex, u0, v0, u1, v1 = iconOf(id)
      if tex then
        T.icon(tex, u0, v0, u1, v1, ix + 2, y0 + 2, ix + size - 2, y0 + size - 2)
      else
        T.textIn(ix, y0, ix + size, y0 + size, "?", C.bad, 0.5)
      end
    end
  end
end

local function drawSets(x0, y0, x1, y1, wx, wy)
  local sets = API.spells.Sets() or {}
  local used = {}
  for n, set in ipairs(sets) do
    if set.inUse then used[#used + 1] = n end
  end
  local full = #used >= #sets and #sets > 0
  ui.SetCursorPos(x0 - wx, y0 - wy)
  jumpHere("new", 1)
  if T.button("##newset", "New set", T.px(300), T.px(64), { primary = not full, disabled = full }, pad) then
    openEditor(nil, nil, false)
  end
  ui.SameLine(0, T.px(16))
  if T.button("##gemset", "From gems", T.px(300), T.px(64), { disabled = full }, pad) then openEditor(nil, nil, true) end
  T.font(20)
  local note, col = string.format("%d of 10 sets", #used), C.dim
  if flash and ui.GetTime() < flash[2] then note, col = flash[1], C.good else flash = nil end
  T.textIn(x0 + T.px(640), y0, x1, y0 + T.px(64), note, col, 1)
  T.unfont()
  local cy0 = y0 + T.px(84)
  if #used == 0 then
    T.font(22)
    T.draw(x0, cy0, "No spell sets yet. New set starts an empty one; From gems saves the spells on your gems.", C.dim)
    T.unfont()
    return
  end
  local gap = T.px(14)
  local cardW = (x1 - x0 - gap) / 2
  local cardH = math.min(T.px(132), (y1 - cy0 - 4 * gap) / 5)
  for k, n in ipairs(used) do
    local set = sets[n]
    local col2, row = (k - 1) % 2, (k - 1) // 2
    local cx0, ry0 = x0 + col2 * (cardW + gap), cy0 + row * (cardH + gap)
    ui.SetCursorPos(cx0 - wx, ry0 - wy)
    ui.PushID("set" .. n)
    jumpHere("card", n)
    local clicked = T.settle(ui.InvisibleButton("##card", cardW, cardH, ui.ButtonFlags.EnableNav), pad)
    if pad.yActivated(clicked) then clicked = false end
    local focused = pad.focused()
    local rightClicked = ui.IsItemClicked(ui.Mouse.Right)
    ui.PopID()
    local r = T.px(12)
    T.rect(cx0, ry0, cx0 + cardW, ry0 + cardH, C.raised, focused and 1 or 0.8, r)
    T.font(22)
    local _, th = ui.CalcTextSize("Ag")
    T.draw(cx0 + T.px(16), ry0 + T.px(12), T.fit(set.name, cardW - T.px(32)), C.text)
    T.unfont()
    local isz = math.min(T.px(48), (cardW - T.px(32) - 11 * T.px(6)) / 12)
    setIcons(set.spells, cx0 + T.px(16), ry0 + T.px(20) + th, isz, T.px(6))
    if focused then
      T.focusRing(cx0, ry0, cx0 + cardW, ry0 + cardH, r)
      if W.pressed("x") then openEditor(n, set, false) end
    end
    if rightClicked then openEditor(n, set, false) end
    if clicked and view == "sets" then
      API.spells.MemorizeSet(n)
      view = "book" -- to watch the gems fill
    end
  end
end

local function drawEditor(x0, y0, x1, y1, wx, wy)
  local sets = API.spells.Sets() or {}
  local st = API.spells.BookState() or {}
  local usable = st.usableGems or 8
  refreshBook()
  local L = T.px(440)
  -- the name
  ui.SetCursorPos(x0 - wx, y0 - wy)
  local shown = trim(ed.name) ~= "" and ed.name or "(no name yet)"
  if T.button("##edname", shown, L, T.px(76), { sub = "Set name: A to type it", align = 0 }, pad) then
    K.open(kb, "Spell set name", ed.name, 24, true)
  end
  -- the twelve slots, four by three
  local gap = T.px(12)
  local size = (L - 3 * gap) / 4
  local gy = y0 + T.px(76) + T.px(16)
  for k = 1, SLOTS do
    local col, row = (k - 1) % 4, (k - 1) // 4
    local sx0, sy0 = x0 + col * (size + gap), gy + row * (size + gap)
    ui.SetCursorPos(sx0 - wx, sy0 - wy)
    ui.PushID("slot" .. k)
    jumpHere("slot", k)
    local clicked = T.settle(ui.InvisibleButton("##slot", size, size, ui.ButtonFlags.EnableNav), pad)
    if pad.yActivated(clicked) then clicked = false end
    local focused = pad.focused()
    local rightClicked = ui.IsItemClicked(ui.Mouse.Right)
    ui.PopID()
    local id = ed.spells[k]
    local canUse = k <= usable
    local r = T.px(10)
    T.rect(sx0, sy0, sx0 + size, sy0 + size, id ~= -1 and C.raised or C.sunken, canUse and 1 or 0.45, r)
    if id ~= -1 then
      local tex, u0, v0, u1, v1 = iconOf(id)
      local m = math.floor(size * 0.1)
      if tex then
        T.icon(tex, u0, v0, u1, v1, sx0 + m, sy0 + m, sx0 + size - m, sy0 + size - m,
          (ed.held and ed.held.slot == k) and 0.35 or (canUse and 1 or 0.5))
      else
        T.textIn(sx0, sy0, sx0 + size, sy0 + size, "?", C.bad, 0.5)
      end
      T.font(14)
      T.draw(sx0 + T.px(6), sy0 + T.px(4), tostring(k), C.text)
      T.unfont()
    else
      T.font(18)
      T.textIn(sx0, sy0, sx0 + size, sy0 + size, canUse and tostring(k) or "-", C.faint, 0.5)
      T.unfont()
    end
    T.outline(sx0, sy0, sx0 + size, sy0 + size, C.edge, 1, r)
    if ed.target == k then T.outline(sx0 + 1, sy0 + 1, sx0 + size - 1, sy0 + size - 1, C.endurance, 1, r, T.px(3)) end
    if focused then
      T.focusRing(sx0, sy0, sx0 + size, sy0 + size, r)
      ed.focus = { slot = k }
      if W.pressed("x") then
        ed.spells[k] = -1
        if ed.held and ed.held.slot == k then ed.held = nil end
      end
      if col == 3 and W.pressed("right") then jump = { espell = edPage * 16 + math.floor(row * 8 / 3) } end
    end
    if rightClicked then ed.spells[k] = -1 end
    if clicked then
      if ed.held and ed.held.id then
        putSpell(ed.spells, k, ed.held.id)
        ed.held = nil
      elseif ed.held and ed.held.slot then
        local from = ed.held.slot
        ed.spells[k], ed.spells[from] = ed.spells[from], ed.spells[k]
        ed.held = nil
      elseif id ~= -1 then
        ed.held = { slot = k }
      else
        ed.target = k -- then a spell for it
        jump = { espell = edPage * 16 }
      end
    end
  end
  -- the buttons under them
  local canSave, clash, free = saveCheck(sets)
  local by = gy + 3 * size + 2 * gap + T.px(20)
  local bw = (L - gap) / 2
  ui.SetCursorPos(x0 - wx, by - wy)
  if T.button("##edsave", clash and "Replace" or "Save", bw, T.px(60), { primary = canSave, disabled = not canSave,
      glyph = "y" }, pad) then
    save(sets)
    return
  end
  ui.SameLine(0, gap)
  if T.button("##edcancel", "Cancel", bw, T.px(60), {}, pad) then
    ed, view = nil, "sets"
    return
  end
  ui.SetCursorPos(x0 - wx, by + T.px(60) + gap - wy)
  if T.button("##edgems", "From gems", bw, T.px(60), {}, pad) then ed.spells = copySpells(st.gems) end
  ui.SameLine(0, gap)
  if T.button("##edclear", "Clear", bw, T.px(60), {}, pad) then ed.spells, ed.held, ed.target = copySpells(nil), nil, nil end
  if ed.n then
    ui.SetCursorPos(x0 - wx, by + 2 * (T.px(60) + gap) - wy)
    if T.button("##eddelete", "Delete set", L, T.px(60), {}, pad) then
      local n, name = ed.n, ed.name
      state.confirm = { "Delete the spell set " .. name .. "?", function()
        API.spells.DeleteSet(n)
        ed, view = nil, "sets"
        jump = { new = 1 }
      end }
    end
  end
  -- what is going on, under the buttons
  local ny = by + 3 * (T.px(60) + gap)
  local note, col = "Nothing is memorized until you load the set.", C.dim
  if ed.held and ed.held.id then
    note, col = "Choose a slot for " .. spellName(ed.held.id), C.accent
  elseif ed.held and ed.held.slot then
    note, col = "Choose a slot to swap gem " .. ed.held.slot .. " with", C.accent
  elseif ed.target then
    note, col = "Choose a spell for gem " .. ed.target, C.accent
  elseif trim(ed.name) == "" then
    note = "Give the set a name to save it."
  elseif clash then
    note, col = "Saving replaces your set " .. clash, C.gold
  elseif not ed.n and not free then
    note, col = "All 10 sets are in use: delete one first.", C.bad
  elseif ed.focus and ed.focus.slot and ed.spells[ed.focus.slot] ~= -1 then
    local k = ed.focus.slot
    note = "Gem " .. k .. ": " .. spellName(ed.spells[k]) .. (k > usable and " (you cannot use this gem yet)" or "")
  end
  T.font(18)
  ui.PushTextWrapPos(x0 + L)
  ui.SetCursorPos(x0 - wx, ny - wy)
  ui.PushStyleColor(ui.Col.Text, col[1], col[2], col[3], 1)
  ui.TextWrapped(note)
  ui.PopStyleColor(1)
  ui.PopTextWrapPos()
  T.unfont()
  -- the book: two columns of eight, every spell scribed by level
  local px0 = x0 + L + T.px(40)
  local pageW = (x1 - px0 - T.px(30)) / 2
  local rowH = (y1 - y0 - T.px(10)) / 8 - T.px(10)
  local pages = math.max(1, (#book + 15) // 16)
  if edPage >= pages then edPage = pages - 1 end
  local inSet = {}
  for k = 1, SLOTS do
    if ed.spells[k] ~= -1 then inSet[ed.spells[k]] = k end
  end
  for page = 0, 1 do
    for i = 0, 7 do
      local m = page * 8 + i
      local e = book[edPage * 16 + m + 1]
      local rx0, ry0 = px0 + page * (pageW + T.px(30)), y0 + i * (rowH + T.px(10))
      ui.SetCursorPos(rx0 - wx, ry0 - wy)
      ui.PushID("espell" .. m)
      jumpHere("espell", edPage * 16 + m)
      local clicked = T.settle(ui.InvisibleButton("##s", pageW, rowH, ui.ButtonFlags.EnableNav), pad)
      if pad.yActivated(clicked) then clicked = false end
      local focused = pad.focused()
      ui.PopID()
      local rx1, ry1 = rx0 + pageW, ry0 + rowH
      T.rect(rx0, ry0, rx1, ry1, e and C.raised or C.sunken, 0.9, T.px(10))
      if e then
        if ed.held and ed.held.id == e.id then T.rect(rx0, ry0, rx1, ry1, C.accent, 0.25, T.px(10)) end
        local is = rowH - T.px(12)
        local tex, u0, v0, u1, v1 = API.spells.Icon(e.icon)
        if tex then T.icon(tex, u0, v0, u1, v1, rx0 + T.px(6), ry0 + T.px(6), rx0 + T.px(6) + is, ry0 + T.px(6) + is) end
        local badge = inSet[e.id]
        local right = badge and T.px(56) or T.px(10)
        T.textIn(rx0 + is + T.px(20), ry0, rx1 - right - T.px(60), ry1, T.fit(e.name, pageW - is - right - T.px(90)), C.text, 0)
        T.font(16)
        T.textIn(rx1 - right - T.px(60), ry0, rx1 - right - T.px(6), ry1, "L" .. e.level, C.dim, 1)
        T.unfont()
        if badge then
          local bx, byy = rx1 - T.px(30), (ry0 + ry1) / 2
          ui.DrawCircleFilled(bx, byy, T.px(16), C.accent[1], C.accent[2], C.accent[3], 1)
          T.font(16)
          T.textIn(bx - T.px(16), byy - T.px(16), bx + T.px(16), byy + T.px(16), tostring(badge), C.onAccent, 0.5)
          T.unfont()
        end
      end
      if focused then
        T.focusRing(rx0, ry0, rx1, ry1, T.px(10))
        ed.focus = { spell = e and e.id }
        local base = edPage * 16
        if page == 0 and W.pressed("left") then jump = { slot = math.min(2, math.floor(i * 3 / 8)) * 4 + 4 } end
        if page == 0 and W.pressed("right") then jump = { espell = base + m + 8 } end
        if page == 1 and W.pressed("left") then jump = { espell = base + m - 8 } end
      end
      if clicked and e then
        if ed.target then
          putSpell(ed.spells, ed.target, e.id)
          jump = { slot = ed.target }
          ed.target, ed.held = nil, nil
        else
          ed.held = { id = e.id }
          local k = nil -- the first empty gem you can use, else gem 1
          for j = 1, math.min(usable, SLOTS) do
            if ed.spells[j] == -1 then k = j break end
          end
          jump = { slot = k or 1 }
        end
      end
    end
  end
  if W.pressed("lb") and edPage > 0 then edPage = edPage - 1 end
  if W.pressed("rb") and edPage < pages - 1 then edPage = edPage + 1 end
  if W.pressed("y") then save(sets) end
end

local function drawBook(x0, y0, x1, y1, wx, wy, holding, holdingSpell)
  -- the gems on the left, two columns of six
  local gsize, ggap = T.px(76), T.px(12)
  T.heading("Gems")
  local gy = select(2, ui.GetCursorScreenPos())
  for g = 0, 11 do
    if API.windows.ChildShown(GEMS, "CSPW_Spell" .. g) == false then goto nextgem end -- a gem the character cannot use
    local col, row = g // 6, g % 6
    local gx0, gy0 = x0 + col * (gsize + ggap), gy + row * (gsize + ggap)
    ui.SetCursorPos(gx0 - wx, gy0 - wy)
    ui.PushID("gem" .. g)
    jumpHere("gem", g)
    local clicked = ui.InvisibleButton("##g", gsize, gsize, ui.ButtonFlags.EnableNav)
    if pad.yActivated(clicked) then clicked = false end -- (Y is not a press)
    local focused = pad.focused()
    ui.PopID()
    local name = drawGem(g, gx0, gy0, gsize)
    if focused then
      T.focusRing(gx0, gy0, gx0 + gsize, gy0 + gsize, T.px(10))
      if W.pressed("x") then API.spells.GemClick("CSPW_Spell" .. g, "right") end
      if col == 1 and W.pressed("right") then jump = { spell = row } end
    end
    if clicked then API.spells.GemClick("CSPW_Spell" .. g, "left") end
    if ui.IsItemClicked(ui.Mouse.Right) then API.spells.GemClick("CSPW_Spell" .. g, "right") end
    ::nextgem::
  end
  local gx1 = x0 + 2 * gsize + ggap
  -- the memorize / scribe progress, under the gems
  if API.windows.ChildShown(BOOK, "SBW_Memorize_Gauge") == true then
    -- (the memorize gauge's EQType 9 reads 0, not nothing, while a scroll is scribed: the state picks the gauge)
    local scribing = (API.spells.BookState() or {}).scribing
    local frac = API.character.Gauge(scribing and 10 or 9)
    local by = gy + 6 * (gsize + ggap) + T.px(10)
    T.font(18)
    T.bar(x0, by, gx1, by + T.px(28), frac, C.cast, scribing and "Scribing" or "Memorizing")
    T.unfont()
  end
  -- the two pages: eight spells each
  local px0 = gx1 + T.px(40)
  local pageW = (x1 - px0 - T.px(30)) / 2
  local rowH = (y1 - y0 - T.px(10)) / 8 - T.px(10)
  for page = 0, 1 do
    for i = 0, 7 do
      local m = page * 8 + i
      local rx0, ry0 = px0 + page * (pageW + T.px(30)), y0 + i * (rowH + T.px(10))
      ui.SetCursorPos(rx0 - wx, ry0 - wy)
      local name = API.windows.Text(BOOK, "SBW_SpellName" .. m) or ""
      ui.PushID("spell" .. m)
      jumpHere("spell", m)
      local clicked = ui.InvisibleButton("##s", pageW, rowH, ui.ButtonFlags.EnableNav)
      if pad.yActivated(clicked) then clicked = false end -- (Y is not a press)
      local focused = pad.focused()
      ui.PopID()
      local rx1, ry1 = rx0 + pageW, ry0 + rowH
      T.rect(rx0, ry0, rx1, ry1, name ~= "" and C.raised or C.sunken, 0.9, T.px(10))
      local tex, u0, v0, u1, v1 = API.windows.ChildArt(BOOK, "SBW_Spell" .. m, "decal")
      local is = rowH - T.px(12)
      if type(tex) == "userdata" then T.icon(tex, u0, v0, u1, v1, rx0 + T.px(6), ry0 + T.px(6), rx0 + T.px(6) + is, ry0 + T.px(6) + is) end
      T.textIn(rx0 + is + T.px(20), ry0, rx1 - T.px(10), ry1, name ~= "" and T.fit(name, pageW - is - T.px(30)) or "", C.text, 0)
      if focused then
        T.focusRing(rx0, ry0, rx1, ry1, T.px(10))
        if page == 0 and W.pressed("left") then jump = { gem = 6 + math.min(i, 5) } end
        if page == 0 and W.pressed("right") then jump = { spell = m + 8 } end
        if page == 1 and W.pressed("left") then jump = { spell = m - 8 } end
      end
      if clicked and name ~= "" then API.windows.ClickChild(BOOK, "SBW_Spell" .. m, "left") end
    end
  end
  -- (the book's PageUp arrow turns forward and PageDown back, as the game has them: LB back, RB forward)
  if W.pressed("lb") then API.windows.ClickChild(BOOK, "SBW_PageDown_Button", "left") end
  if W.pressed("rb") then API.windows.ClickChild(BOOK, "SBW_PageUp_Button", "left") end
  if holdingSpell then -- under the gems (and their memorize bar): the spell held, waiting for a gem
    T.font(20)
    local _, lh = ui.CalcTextSize("Ag")
    local ty = gy + 6 * (gsize + ggap) + T.px(50)
    T.draw(x0, ty, "Choose a gem for", C.accent)
    T.draw(x0, ty + lh, T.fit(holding.spell, px0 - x0 - T.px(10)), C.text)
    T.unfont()
  end
  if W.pressed("y") then
    view = "sets"
    jump = { new = 1 }
  end
end

-- The editor, the sets page's word and the question belong to the character they were made for: camping (or another
-- character's name) clears them, so Save cannot write into the next character's set of that number.
local edWho = nil
local function forCharacter()
  local who = API.session.InGame() and (API.character.Label(1) or "") or nil
  if who == edWho then return end
  edWho = who
  ed, view, state.confirm, flash, jump, edPage = nil, "book", nil, nil, nil, 0
  kb = K.new()
  book, byId, bookRead = {}, {}, -1
end

ui.window("Spell Book", function()
  forCharacter()
  if not W.shown(BOOK) then return end
  -- shown again: back at the pages, unless a set was being edited
  local frame = ui.GetFrameCount()
  if frame - lastDrawn > 1 and view == "sets" then view = "book" end
  lastDrawn = frame
  -- testing: test.book.view=book|sets|edit<set number>|editnew in ui/settings/gamepad.txt
  local tv = ui.GetSetting("test.book.view")
  if tv and tv ~= testView then
    testView = tv
    if tv == "book" or tv == "sets" then
      view = tv
    elseif tv:match("^edit") then
      local n = tonumber(tv:match("%d+"))
      openEditor(n, n and (API.spells.Sets() or {})[n], false)
    end
  end
  if view == "edit" and not ed then view = "sets" end
  local holding = API.cursor.Attachment and API.cursor.Attachment()
  local holdingSpell = holding and holding.spell ~= nil
  local title, subtitle, hints
  if view == "book" then
    title = "Spell Book"
    subtitle = "Pages " .. (API.windows.Text(BOOK, "SBW_LeftPageNum") or "") .. " - " .. (API.windows.Text(BOOK, "SBW_RightPageNum") or "")
    hints = { { "a", holdingSpell and "Memorize here" or "Take spell" }, { "x", "Forget" }, { "lb", "Page" },
      { "rb", "Page" }, { "y", "Spell sets" }, { "b", "Close" } }
  elseif view == "sets" then
    title, subtitle = "Spell Sets", "Load a set onto your gems, or edit one without memorizing it"
    hints = { { "a", "Load" }, { "x", "Edit" }, { "y", "Spell book" }, { "b", "Back" } }
  else
    refreshBook()
    title = ed.n and "Edit Spell Set" or "New Spell Set"
    subtitle = string.format("Page %d of %d", edPage + 1, math.max(1, (#book + 15) // 16))
    local busy = ed.held or ed.target
    hints = { { "a", busy and "Place" or "Choose" }, { "x", "Remove" }, { "lb", "Page" }, { "rb", "Page" },
      { "y", "Save" }, { "b", busy and "Cancel" or "Back" } }
  end
  if kb.open then hints = K.HINTS end
  if W.begin("Spell Book", { w = 1500, h = 900, title = title, subtitle = subtitle, hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if state.confirm then
      W.confirm(state)
    elseif kb.open then
      local w = math.min(x1 - x0, T.px(900))
      local r = K.draw(kb, pad, x0 + (x1 - x0 - w) / 2, y0 + T.px(40), w)
      if r == "ok" and ed then ed.name = T.cutBytes(kb.text, 24) end
      if r then jump = { slot = 1 } end
      if r == "cancel" and ed and ed.n == nil and trim(ed.name) == "" and not dirty() then ed, view = nil, "sets" end
    elseif view == "book" then
      drawBook(x0, y0, x1, y1, wx, wy, holding, holdingSpell)
    elseif view == "sets" then
      drawSets(x0, y0, x1, y1, wx, wy)
    elseif ed then
      drawEditor(x0, y0, x1, y1, wx, wy)
    end
  end
  if W.back(W.finish(), state, kb) then
    -- B steps back: a pick, the editor (asking first when it has changes), the sets, then the book closes
    if view == "edit" and ed then
      if ed.held or ed.target then
        ed.held, ed.target = nil, nil
      elseif dirty() then
        state.confirm = { "Leave without saving the set?", function() ed, view = nil, "sets" end }
      else
        ed, view = nil, "sets"
        jump = { new = 1 }
      end
    elseif view ~= "book" then
      view = "book"
    else
      API.windows.Show(BOOK, false)
    end
  end
end)
