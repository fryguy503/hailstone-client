-- Ability Book (Hailstone's own window, AbilityBookWnd: the EQ menu's "Ability Book", /abilitybook): every ability
-- you can activate apart from spells, in one list, in the spell book's look:
--   * the activated alternate abilities, the combat abilities (disciplines) and the activated skills
--     (eq.AbilityBook; each is used as its own hot button is, so the game's and the server's rules hold);
--   * search (name or description), sort (name, kind, timer group, reuse, time left, level, cost, and class when
--     you have more than one), filters (kind, ready now, favourites, on autoskill); passive AAs are not listed;
--   * the ability's details at the right: its costs, reuse and timer group (with what shares the timer), its
--     description (a combat ability's is the game's spell display text: eq.SpellDetails);
--   * double-click to use, drag onto a hot bar, right-click for its menu;
--   * Autoskill along the bottom: its list in order (the arrows move one), the switch, and the selected entry's
--     conditions (a summary; the Conditions popup sets them). While on it uses the first ready ability of the list,
--     one at a time, while you fight (auto attack on an NPC in melee range); a beneficial one may be set to any time
--     or out of combat, and one with a buff to only when the buff has worn off. A server can turn it off (hailstone.cfg [abilities] autoskill = off).
local hud = dofile("lib/hud.lua")
local stml = dofile("lib/stml.lua")
local T = hud.T
local C = T.color
local WINDOW = "AbilityBookWnd"
local MANAGED = ui.ManagedDistribution and ui.ManagedDistribution() or false

local W, H = 960, 620
local DETAIL_W = 320
local TOOL_H, AUTO_H = 40, 92
local ROW_H, HEAD_H = 34, 24

local KINDS = { aa = "Alternate ability", disc = "Combat ability", skill = "Skill" }
local KIND_SHORT = { aa = "AA", disc = "Combat", skill = "Skill" }
local KIND_ORDER = { aa = 1, disc = 2, skill = 3 } -- (a kind the client does not know yet sorts after these: 4)
local WHEN = { fighting = "While you fight", always = "Any time", resting = "Out of combat" }

-- ---------------------------------------------------------------- state
local list, byKey = {}, {}
local st = { classes = {}, autoskill = { on = false, allowed = true, held = false, list = {} } }
local lastRead = -1
local sel = nil         -- the selected ability's key
local search = ""
local details = { key = nil }
local autoSel = nil     -- the autoskill entry whose conditions show
local toolW = W         -- the toolbar's width at scale 1, as last drawn (the window widens to it)

local function setting(key, default)
  local v = ui.GetSetting("abilitybook." .. key)
  if v == nil then return default end
  if type(default) == "boolean" then return v == "1" end
  if type(default) == "number" then return tonumber(v) or default end
  return v
end
local function setSetting(key, v)
  ui.SetSetting("abilitybook." .. key, type(v) == "boolean" and (v and "1" or "0") or tostring(v))
end

local function lower(s) return (s or ""):lower() end

local stamp = 0        -- counts the reads: the filtered and sorted rows are kept until it (or a filter) changes
local autoIndex = {}   -- key -> its place on the autoskill list (the first, as a scan of the list finds)
local mask = 0         -- the player's classes as a mask (bit c-1)
local function refresh(force)
  if not force and ui.GetTime() - lastRead < 0.25 then return end
  lastRead = ui.GetTime()
  list = eq.AbilityBook() or {}
  st = eq.AbilityBookState() or st
  stamp = stamp + 1
  byKey = {}
  for _, e in ipairs(list) do
    byKey[e.key] = e
    -- what the search and the name sort compare, and the row's id, once a read rather than once a frame
    e._lname, e._ldesc, e._rowId = lower(e.name), lower(e.description), "##r" .. e.key
  end
  autoIndex = {}
  for i, r in ipairs(st.autoskill.list) do
    if not autoIndex[r.key] then autoIndex[r.key] = i end
  end
  mask = 0
  for _, c in ipairs(st.classes) do mask = mask | (1 << (c.id - 1)) end
end

local function seconds(n)
  if not n or n <= 0 then return "-" end
  if n >= 3600 then return string.format("%dh %02dm", n // 3600, n % 3600 // 60) end
  if n >= 60 then return string.format("%dm %02ds", n // 60, n % 60) end
  return string.format("%ds", n)
end

local function onAutoskill(key)
  local i = autoIndex[key]
  if i then return i, st.autoskill.list[i] end
  return nil
end

-- The player's classes as a mask (bit c-1) and the class an ability counts under: "Shared" when every one of the
-- player's classes has it (or any class may use it), else the first of the player's classes that has it. A class
-- filter shows that class's abilities and the shared ones; "Shared" shows only those.
local function classOf(e)
  local mine = mask
  local has = e.classes & mine
  if e.classes == 0 or has == mine or has == 0 then return "Shared", 0 end
  for _, c in ipairs(st.classes) do
    if has & (1 << (c.id - 1)) ~= 0 then return c.name, c.id end
  end
  return "Shared", 0
end
local function multiclass() return #st.classes > 1 end

local function timerLabel(e)
  if e.kind == "skill" then return "Own timer" end
  if e.timer < 0 then return "No timer" end
  return (e.kind == "aa" and "AA timer " or "Combat timer ") .. e.timer
end
local function timerGroup(e)
  if e.kind == "skill" or e.timer < 0 then return nil end
  return e.kind .. ":" .. e.timer
end

local function cost(e)
  local parts = {}
  if e.endurance > 0 then parts[#parts + 1] = e.endurance .. " end" end
  if e.mana > 0 then parts[#parts + 1] = e.mana .. " mana" end
  return #parts > 0 and table.concat(parts, ", ") or "-"
end

-- The book's settings, read once a frame after the toolbar (which changes them).
local cfg = {}
local function readSettings()
  cfg.kind = setting("kind", "all")
  cfg.ready = setting("ready", false)
  cfg.favorites = setting("favorites", false)
  cfg.auto = not MANAGED and setting("auto", false)
  cfg.multi = multiclass()
  cfg.class = cfg.multi and setting("class", 0) or 0
  cfg.sort = setting("sort", "Name")
  cfg.desc = setting("desc", false)
end

-- q: the search in lower case ("" none)
local function matches(e, q)
  local kind = cfg.kind
  if kind ~= "all" and e.kind ~= kind then return false end
  if cfg.ready and not e.ready then return false end
  if cfg.favorites and not e.favorite then return false end
  if cfg.auto and not onAutoskill(e.key) then return false end
  if cfg.multi then
    local cls = cfg.class
    if cls ~= 0 then
      local _, id = classOf(e)
      if cls == -1 then
        if id ~= 0 then return false end
      elseif id ~= 0 and e.classes & (1 << (cls - 1)) == 0 then
        return false
      end
    end
  end
  if q ~= "" then
    if not (e._lname:find(q, 1, true) or e._ldesc:find(q, 1, true)) then return false end
  end
  return true
end

local SORTS = { "Name", "Kind", "Timer group", "Reuse", "Time left", "Level", "Cost" }
local SORTS_MULTI = { "Name", "Kind", "Class", "Timer group", "Reuse", "Time left", "Level", "Cost" }
local function sorts() return multiclass() and SORTS_MULTI or SORTS end

-- The sort key, and the group header a row falls under ("" none).
local function sortKey(e, mode)
  if mode == "Kind" then
    return KIND_ORDER[e.kind] or 4, KINDS[e.kind] and (KINDS[e.kind] .. "s") or "Other abilities"
  end
  if mode == "Class" then
    local name, id = classOf(e)
    return id == 0 and 99 or id, name
  end
  if mode == "Timer group" then
    local g = timerGroup(e)
    if not g then return 99999, "No shared timer" end
    return (KIND_ORDER[e.kind] or 4) * 1000 + e.timer, timerLabel(e)
  end
  if mode == "Reuse" then return e.reuse, "" end
  if mode == "Time left" then return e.remaining, "" end
  if mode == "Level" then return e.level, "" end
  if mode == "Cost" then return e.endurance + e.mana, "" end
  return e._lname, ""
end

local keys, sortDesc = {}, false
local function byKeys(a, b)
  local ka, kb = keys[a.key], keys[b.key]
  if ka ~= kb then
    if sortDesc then return ka > kb end
    return ka < kb
  end
  return a._lname < b._lname
end

local function sorted(rows)
  local mode = cfg.sort
  local valid = false
  for _, m in ipairs(sorts()) do valid = valid or m == mode end
  if not valid then mode = "Name" end
  keys, sortDesc = {}, cfg.desc
  for _, e in ipairs(rows) do keys[e.key] = sortKey(e, mode) end
  table.sort(rows, byKeys)
  return rows, mode
end

-- The rows the list shows, filtered and sorted, with each one's group header (sortKey's second value): kept while
-- the read, the search, the filters and the sort stay the same.
local rowCache = { rows = {}, groups = {} }
local function shownRows()
  local c = rowCache
  if c.stamp ~= stamp or c.search ~= search or c.kind ~= cfg.kind or c.ready ~= cfg.ready
      or c.favorites ~= cfg.favorites or c.auto ~= cfg.auto or c.multi ~= cfg.multi or c.class ~= cfg.class
      or c.sort ~= cfg.sort or c.desc ~= cfg.desc then
    c.stamp, c.search, c.kind, c.ready, c.favorites = stamp, search, cfg.kind, cfg.ready, cfg.favorites
    c.auto, c.multi, c.class, c.sort, c.desc = cfg.auto, cfg.multi, cfg.class, cfg.sort, cfg.desc
    local q = lower(search)
    local rows = {}
    for _, e in ipairs(list) do
      if matches(e, q) then rows[#rows + 1] = e end
    end
    c.rows, c.mode = sorted(rows)
    c.groups = {}
    for i, e in ipairs(c.rows) do
      local _, g = sortKey(e, c.mode)
      c.groups[i] = g
    end
  end
  return c.rows, c.groups
end

local function icon(e, x, y, size, s, ring, alpha)
  local tex, u0, v0, u1, v1
  if e.icon and e.icon >= 0 then tex, u0, v0, u1, v1 = eq.SpellIcon(e.icon) end
  T.Icon(x, y, size, s, tex, u0, v0, u1, v1, ring, { radius = 5, alpha = alpha or 1 })
  if not tex then
    T.Text(x + size / 2, y + size / 2 - 7 * s, e.name:sub(1, 2), C.muted, T.font.small, "center")
  end
  -- the reuse still to go, shaded from the bottom up
  if e.remaining > 0 and e.reuse > 0 then
    local f = math.min(1, e.remaining / e.reuse)
    ui.DrawRectFilled(x, y + size * (1 - f), x + size, y + size, 0, 0, 0, 0.5, 5 * s)
  end
end

local function use(e)
  if e then
    eq.AbilityBookUse(e.key)
    lastRead = -1
  end
end

-- ---------------------------------------------------------------- pieces
local function chip(id, x, y, label, on, s, tip)
  local tw = T.TextSize(label, T.font.small)
  local w = tw + 18 * s
  local clicked = T.Hit(id, x, y, x + w, y + 24 * s)
  local hov = ui.IsItemHovered()
  local a = C.accent
  if on then
    ui.DrawRectFilled(x, y, x + w, y + 24 * s, a[1], a[2], a[3], 0.22, 12 * s)
    ui.DrawRect(x, y, x + w, y + 24 * s, a[1], a[2], a[3], 0.7, 12 * s, 1)
  else
    ui.DrawRectFilled(x, y, x + w, y + 24 * s, C.ink[1], C.ink[2], C.ink[3], hov and 0.08 or 0.04, 12 * s)
  end
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(x + w / 2, y + (24 * s - th) / 2, label, (on or hov) and C.text or C.muted, T.font.small, "center")
  if hov and tip then ui.SetTooltip(tip) end
  return clicked, w
end

local KIND_CHIPS = { { "all", "All", "##kindall" }, { "aa", "AAs", "##kindaa" }, { "disc", "Combat", "##kinddisc" },
  { "skill", "Skills", "##kindskill" } }
local FILTER_CHIPS = {
  { "ready", "Ready", "Only abilities ready to use", "##fready" },
  { "favorites", "Favorites", "Only your favorites", "##ffavorites" },
  not MANAGED and { "auto", "Autoskill", "Only abilities on the autoskill list", "##fauto" } or nil,
}
local DESC_TIP, ASC_TIP = { tooltip = "Descending" }, { tooltip = "Ascending" }

local function toolbar(x, y, w, s)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy + 2 * s)
  ui.SetNextItemWidth(200 * s)
  ui.PushStyleVar(ui.StyleVar.FramePadding, 8 * s, 5 * s)
  ui.PushStyleVar(ui.StyleVar.FrameRounding, 12 * s)
  local text = ui.InputText("##search", search, 64)
  local editing = ui.IsItemActive()
  ui.PopStyleVar(2)
  search = text
  if search == "" and not editing then
    local _, th = T.TextSize("Ag", T.font.small)
    T.Text(x + 10 * s, y + 2 * s + (26 * s - th) / 2, "Search abilities", C.faint, T.font.small)
  end
  -- sort
  local cx = x + 210 * s
  T.Text(cx, y + 7 * s, "Sort", C.muted, T.font.small)
  ui.SetCursorPos(cx + 34 * s - wx, y - wy + 2 * s)
  ui.SetNextItemWidth(118 * s)
  local mode = setting("sort", "Name")
  if ui.BeginCombo("##sort", mode) then
    for _, m in ipairs(sorts()) do
      if ui.Selectable(m, m == mode) then setSetting("sort", m) end
    end
    ui.EndCombo()
  end
  local desc = setting("desc", false)
  if T.Button("##desc", cx + 156 * s, y + 2 * s, cx + 184 * s, y + 28 * s, s, desc and "v" or "^",
      desc and DESC_TIP or ASC_TIP) then
    setSetting("desc", not desc)
  end
  -- kind
  local fx = cx + 196 * s
  local kind = setting("kind", "all")
  for _, k in ipairs(KIND_CHIPS) do
    local clicked, cw = chip(k[3], fx, y + 3 * s, k[2], kind == k[1], s)
    if clicked then setSetting("kind", k[1]) end
    fx = fx + cw + 4 * s
  end
  fx = fx + 8 * s
  for _, f in ipairs(FILTER_CHIPS) do
    local on = setting(f[1], false)
    local clicked, cw = chip(f[4], fx, y + 3 * s, f[2], on, s, f[3])
    if clicked then setSetting(f[1], not on) end
    fx = fx + cw + 4 * s
  end
  -- class (only with more than one), after the chips; the window widens to fit it
  if multiclass() then
    local cls = setting("class", 0)
    local name = "All classes"
    if cls == -1 then name = "Shared" end
    for _, c in ipairs(st.classes) do if c.id == cls then name = c.name end end
    fx = fx + 8 * s
    ui.SetCursorPos(fx - wx, y - wy + 2 * s)
    ui.SetNextItemWidth(130 * s)
    if ui.BeginCombo("##class", name) then
      if ui.Selectable("All classes", cls == 0) then setSetting("class", 0) end
      if ui.Selectable("Shared", cls == -1) then setSetting("class", -1) end
      for _, c in ipairs(st.classes) do
        if ui.Selectable(c.name, c.id == cls) then setSetting("class", c.id) end
      end
      ui.EndCombo()
    end
    fx = fx + 130 * s
  end
  toolW = (fx - x) / s
end

local function abilityMenu(e)
  ui.TextDisabled(e.name)
  ui.Separator()
  if ui.MenuItem("Use") then use(e) end
  if ui.MenuItem(e.favorite and "Remove from favorites" or "Add to favorites") then
    eq.AbilityBookFavorite(e.key, not e.favorite)
    lastRead = -1
  end
  if e.autoable and not MANAGED then
    local on = onAutoskill(e.key) ~= nil
    if ui.MenuItem(on and "Remove from autoskill" or "Add to autoskill") then
      eq.AbilityBookAutoskill(e.key, not on)
      lastRead = -1
    end
  end
end

local function rowTooltip(e)
  ui.BeginTooltip()
  ui.Text(e.name)
  ui.TextDisabled((KINDS[e.kind] or "Ability") .. "  \xC2\xB7  " .. timerLabel(e))
  ui.TextDisabled("Double-click to use; drag onto a hot bar")
  ui.EndTooltip()
end

local function listView(rows, groups, x, y, w, h, s)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, y - wy)
  ui.BeginChild("##abilities", w, h, false)
  local cw = ui.GetContentRegionAvail()
  local _, top = ui.GetWindowPos()
  local bottom = top + h
  local _, th = T.TextSize("Ag", T.font.small)
  if #rows == 0 then
    local px, py = ui.GetCursorScreenPos()
    T.Text(px + 10 * s, py + 10 * s, #list == 0 and "You have no abilities to activate yet." or "No abilities match.",
      C.muted, T.font.small)
  end
  local header = nil
  local menuFor = nil
  for i, e in ipairs(rows) do
    local g = groups[i]
    if g ~= "" and g ~= header then
      header = g
      local px, py = ui.GetCursorScreenPos()
      ui.Dummy(cw, HEAD_H * s)
      T.Text(px + 4 * s, py + 6 * s, g:upper(), C.faint, T.font.tiny)
      ui.DrawLine(px, py + HEAD_H * s - 3 * s, px + cw, py + HEAD_H * s - 3 * s, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
    end
    local px, py = ui.GetCursorScreenPos()
    ui.InvisibleButton(e._rowId, cw, ROW_H * s)
    local hovered = ui.IsItemHovered()
    if ui.IsItemClicked(ui.Mouse.Left) then sel = e.key end
    if hovered and ui.IsMouseDoubleClicked(ui.Mouse.Left) then use(e) end
    local dragged = T.DragFrom(WINDOW, e.key)
    if ui.IsItemClicked(ui.Mouse.Right) then
      sel = e.key
      menuFor = e
      T.TakeRightClick()
    end
    if py + ROW_H * s >= top and py <= bottom then
      if sel == e.key then
        ui.DrawRectFilled(px, py, px + cw, py + ROW_H * s, C.accent[1], C.accent[2], C.accent[3], 0.16, 6 * s)
      elseif hovered then
        ui.DrawRectFilled(px, py, px + cw, py + ROW_H * s, C.ink[1], C.ink[2], C.ink[3], 0.05, 6 * s)
      end
      local isz = (ROW_H - 6) * s
      icon(e, px + 4 * s, py + 3 * s, isz, s)
      local nx = px + isz + 12 * s
      local ty = py + (ROW_H * s - th) / 2
      local colKind = px + cw * 0.46
      local colTimer = px + cw * 0.58
      local colReuse = px + cw * 0.76
      local colLeft = px + cw * 0.86
      local name = e.name .. (e.maxRank > 1 and (" " .. e.rank) or "")
      T.Text(nx, ty, T.Fit(name, colKind - nx - 26 * s, T.font.small), C.text, T.font.small)
      if e.favorite then T.Text(colKind - 14 * s, ty, "*", C.gold, T.font.small, "center") end
      T.Text(colKind, ty, KIND_SHORT[e.kind], C.faint, T.font.small)
      T.Text(colTimer, ty, T.Fit(timerLabel(e), colReuse - colTimer - 6 * s, T.font.small), C.muted, T.font.small)
      T.Text(colReuse, ty, seconds(e.reuse), C.muted, T.font.small)
      T.Text(colLeft, ty, e.ready and "Ready" or seconds(e.remaining), e.ready and C.good or C.cast, T.font.small)
      if onAutoskill(e.key) then
        local ax = px + cw - 12 * s
        ui.DrawCircleFilled(ax, py + ROW_H * s / 2, 4 * s, C.accent[1], C.accent[2], C.accent[3], 1)
      end
      if hovered and not dragged and not ui.IsMouseDown(ui.Mouse.Left) then rowTooltip(e) end
    end
  end
  if menuFor then ui.OpenPopup("##abilitymenu") end
  if ui.BeginPopup("##abilitymenu") then
    local e = byKey[sel]
    if e then abilityMenu(e) end
    ui.EndPopup()
  end
  ui.EndChild()
end

local HEADER_COLS = { { 0, "Ability" }, { 0.46, "Kind" }, { 0.58, "Timer" }, { 0.76, "Reuse" }, { 0.86, "Ready" } }
local function columnHeader(x, y, w, s)
  for _, c in ipairs(HEADER_COLS) do
    local cx = x + (c[1] == 0 and (ROW_H + 6) * s or w * c[1])
    T.Text(cx, y, c[2], C.faint, T.font.tiny)
  end
end

local shares = {}
local function detailsPane(x, y, w, h, s)
  local e = byKey[sel]
  if not e then
    T.Text(x, y + 6 * s, "Select an ability to see its details.", C.muted, T.font.small)
    return
  end
  icon(e, x, y, 52 * s, s, C.accent)
  local _, th = T.TextSize("Ag", T.font.large)
  T.Text(x + 62 * s, y + 2 * s, T.Fit(e.name, w - 62 * s, T.font.large), C.text, T.font.large)
  local sub = KINDS[e.kind] or "Ability"
  if e.maxRank > 1 then sub = sub .. string.format("  \xC2\xB7  rank %d of %d", e.rank, e.maxRank) end
  if e.level > 0 then sub = sub .. "  \xC2\xB7  level " .. e.level end
  T.Text(x + 62 * s, y + th + 6 * s, T.Fit(sub, w - 62 * s, T.font.small), C.muted, T.font.small)
  if multiclass() then
    T.Text(x + 62 * s, y + th + 22 * s, classOf(e), C.faint, T.font.small)
  end
  -- the numbers in tiles
  local ty = y + 64 * s
  local tiles = {
    { "Cost", cost(e) },
    { "Reuse", seconds(e.reuse) },
    { "Timer", e.kind == "skill" and "Own" or (e.timer >= 0 and tostring(e.timer) or "-") },
    { "Ready", e.ready and "Now" or seconds(e.remaining) },
  }
  local tw = (w - 3 * 6 * s) / 4
  for k, t in ipairs(tiles) do
    local tx = x + (k - 1) * (tw + 6 * s)
    ui.DrawRectFilled(tx, ty, tx + tw, ty + 40 * s, C.ink[1], C.ink[2], C.ink[3], 0.04, 6 * s)
    T.Text(tx + tw / 2, ty + 4 * s, t[1], C.faint, T.font.tiny, "center")
    T.Text(tx + tw / 2, ty + 18 * s, T.Fit(t[2], tw - 6 * s, T.font.small), C.text, T.font.small, "center")
  end
  -- buttons
  local by = ty + 48 * s
  local bw = MANAGED and ((w - 6 * s) / 2) or ((w - 12 * s) / 3)
  if T.Button("##use", x, by, x + bw, by + 26 * s, s, "Use", { primary = e.ready }) then use(e) end
  if T.Button("##fav", x + bw + 6 * s, by, x + 2 * bw + 6 * s, by + 26 * s, s, e.favorite and "Unfavorite" or "Favorite") then
    eq.AbilityBookFavorite(e.key, not e.favorite)
    lastRead = -1
  end
  local onAuto = onAutoskill(e.key) ~= nil
  if not MANAGED and T.Button("##auto", x + 2 * bw + 12 * s, by, x + w, by + 26 * s, s, onAuto and "Off autoskill" or "To autoskill",
      { enabled = e.autoable, tooltip = (not e.autoable) and "Only combat skills, combat abilities and activated AAs" or nil }) then
    eq.AbilityBookAutoskill(e.key, not onAuto)
    if not onAuto then autoSel = e.key end
    lastRead = -1
  end
  -- what shares its timer
  local dy = by + 36 * s
  local g = timerGroup(e)
  if g then
    local sh = shares
    if sh.stamp ~= stamp or sh.key ~= e.key then
      -- the others on its timer, kept while the read and the selection stay the same
      sh.stamp, sh.key = stamp, e.key
      local others = {}
      for _, o in ipairs(list) do
        if o.key ~= e.key and timerGroup(o) == g then others[#others + 1] = o.name end
      end
      table.sort(others)
      sh.n, sh.line, sh.tip = #others, table.concat(others, ", "), table.concat(others, "\n")
    end
    if sh.n > 0 then
      T.Text(x, dy, "Shares its timer with", C.faint, T.font.tiny)
      T.Text(x, dy + 14 * s, T.Fit(sh.line, w, T.font.small), C.muted, T.font.small)
      if ui.IsMouseHoveringRect(x, dy + 14 * s, x + w, dy + 30 * s) then ui.SetTooltip(sh.tip) end
      dy = dy + 38 * s
    end
  end
  -- the description
  if details.key ~= e.key then
    local raw = e.description
    if e.kind == "disc" and e.spell > 0 then
      local _, text = eq.SpellDetails(e.spell)
      raw = text or ""
    end
    if raw == "" then raw = "No description." end
    details = { key = e.key, lines = stml.Lines(raw, C.text) }
  end
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x - wx, dy - wy)
  ui.BeginChild("##details", w, y + h - dy, false)
  ui.PushFont(T.font.small)
  local lw = ui.GetContentRegionAvail()
  for _, l in ipairs(details.lines or {}) do stml.DrawLine(l, lw) end
  ui.PopFont()
  ui.EndChild()
end

-- Autoskill along the bottom: the switch, its list as tiles in order, and the picked entry's conditions.
local function autoskillStrip(x, y, w, s)
  local a = st.autoskill
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(x, y, "AUTOSKILL", C.faint, T.font.tiny)
  local label = not a.allowed and "Not allowed here" or (a.on and (a.held and "On (held)" or "On") or "Off")
  if T.Button("##autoswitch", x, y + 16 * s, x + 110 * s, y + 44 * s, s, label, {
    primary = a.on, enabled = a.allowed,
    tooltip = not a.allowed and "This server turned autoskill off"
      or a.held and "A running script (Auto Combat) is using your abilities; autoskill waits until it stops"
      or "Uses the list's first ready ability, one at a time, while you fight\n" ..
        "(auto attack on an NPC in melee range); beneficial ones can be set to any time or out of combat.\n" ..
        "/abilitybook auto",
  }) then
    eq.AbilityBookAutoskillEnable(not a.on)
    lastRead = -1
  end
  -- the list
  local lx = x + 122 * s
  local condW = 270 * s
  local lw = w - (lx - x) - condW - 12 * s
  local tile = 40 * s
  if #a.list == 0 then
    T.Text(lx, y + 16 * s + (28 * s - th) / 2, "Add abilities with \"To autoskill\" or the right-click menu.", C.muted,
      T.font.small)
  end
  local per = math.max(1, math.floor(lw / (tile + 6 * s)))
  for i, r in ipairs(a.list) do
    if i > per then
      T.Text(lx + per * (tile + 6 * s), y + 28 * s, "+" .. (#a.list - per), C.faint, T.font.small)
      break
    end
    local tx = lx + (i - 1) * (tile + 6 * s)
    local ty = y + 16 * s
    local e = byKey[r.key]
    if e then
      icon(e, tx, ty, tile, s, autoSel == r.key and C.accent or nil)
    else
      T.Icon(tx, ty, tile, s, nil, 0, 0, 0, 0, nil, { radius = 5, wellAlpha = 0.4 })
    end
    T.Text(tx + 3 * s, ty + 1 * s, tostring(i), C.faint, T.font.tiny)
    if T.Hit("##auto" .. i, tx, ty, tx + tile, ty + tile) then autoSel = r.key end
    if ui.IsItemHovered() then
      ui.SetTooltip((e and e.name or (r.key .. " (not available now)")) .. "\nClick to set its conditions")
    end
  end
  -- the picked entry: its order and conditions
  local cx = x + w - condW
  local i, r = nil, nil
  if autoSel then i, r = onAutoskill(autoSel) end
  if not r then
    T.Text(cx, y + 24 * s, "Pick an entry to set its conditions.", C.faint, T.font.small)
    return
  end
  local e = byKey[r.key]
  T.Text(cx, y, T.Fit((i .. ". " .. (e and e.name or r.key)), condW - 70 * s, T.font.small), C.text, T.font.small)
  if T.Button("##up", cx + condW - 64 * s, y - 2 * s, cx + condW - 44 * s, y + 16 * s, s, "<", { tooltip = "Earlier" }) then
    eq.AbilityBookAutoskillMove(r.key, -1)
    lastRead = -1
  end
  if T.Button("##down", cx + condW - 42 * s, y - 2 * s, cx + condW - 22 * s, y + 16 * s, s, ">", { tooltip = "Later" }) then
    eq.AbilityBookAutoskillMove(r.key, 1)
    lastRead = -1
  end
  if T.Button("##remove", cx + condW - 20 * s, y - 2 * s, cx + condW, y + 16 * s, s, "x", { tooltip = "Take it off" }) then
    eq.AbilityBookAutoskill(r.key, false)
    autoSel = nil
    lastRead = -1
    return
  end
  -- a summary, and the conditions in a popup
  local when = (e and not e.beneficial) and "fighting" or (r.when or "fighting")
  local parts = { WHEN[when] }
  if r.keepBuff and e and e.buff then parts[#parts + 1] = "when its buff is off" end
  if r.endurance > 0 then parts[#parts + 1] = "end " .. r.endurance .. "%+" end
  if r.mana > 0 then parts[#parts + 1] = "mana " .. r.mana .. "%+" end
  if r.selfHp > 0 then parts[#parts + 1] = "you < " .. r.selfHp .. "%" end
  if r.targetHp > 0 then parts[#parts + 1] = "target < " .. r.targetHp .. "%" end
  if r.targetHpAbove > 0 then parts[#parts + 1] = "target > " .. r.targetHpAbove .. "%" end
  local summary = table.concat(parts, ", ")
  T.Text(cx, y + 22 * s, T.Fit(summary, condW, T.font.small), C.muted, T.font.small)
  if T.Button("##conditions", cx, y + 44 * s, cx + 120 * s, y + 70 * s, s, "Conditions") then
    ui.OpenPopup("##autoconditions")
  end
  if ui.BeginPopup("##autoconditions") then
    local changed = false
    local w2 = 260 * s
    ui.TextDisabled(e and e.name or r.key)
    ui.Separator()
    if e and e.beneficial then
      ui.SetNextItemWidth(w2)
      if ui.BeginCombo("##when", WHEN[when]) then
        for _, k in ipairs({ "fighting", "always", "resting" }) do
          if ui.Selectable(WHEN[k], k == when) then when, changed = k, true end
        end
        ui.EndCombo()
      end
    else
      ui.TextDisabled("Only while you fight (it is not beneficial)")
    end
    local keep = r.keepBuff
    if e and e.buff then
      local v, ch = ui.Checkbox("Only when its buff has worn off##keepbuff", keep)
      if ch then keep, changed = v, true end
    end
    local function slider(id, v, fmt, none)
      ui.SetNextItemWidth(w2)
      local nv, ch = ui.SliderInt("##cond" .. id, v, 0, 100, v == 0 and none or string.format(fmt, v))
      if ch then changed = true end
      return nv
    end
    local endu = slider("end", r.endurance, "Endurance at least %d%%", "Any endurance")
    local mana = slider("mana", r.mana, "Mana at least %d%%", "Any mana")
    local mine = slider("self", r.selfHp, "Your health below %d%%", "Any health of yours")
    local below = slider("below", r.targetHp, "Target health below %d%%", "Any target health")
    local above = slider("above", r.targetHpAbove, "Target health above %d%%", "No target minimum")
    if changed then
      eq.AbilityBookAutoskillRule(r.key, endu, mana, below, above, mine, when, keep)
      r.endurance, r.mana, r.targetHp, r.targetHpAbove, r.selfHp, r.when, r.keepBuff =
        endu, mana, below, above, mine, when, keep
    end
    ui.EndPopup()
  end
end

-- ---------------------------------------------------------------- window
local DIALOG = {
  size = function() return math.max(W, math.ceil(toolW)), H end,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 30 end,
  keepTitle = true,
}

local function body(x, y, w, h, s)
  refresh()
  -- testing: test.abilitybook.select=<key> in ui/settings/modern.txt
  local tsel = ui.GetSetting("test.abilitybook.select")
  if tsel and not sel then sel = tsel end
  if sel and not byKey[sel] then sel = nil end
  toolbar(x, y, w, s)
  readSettings()
  local by = y + TOOL_H * s
  local bodyH = h - TOOL_H * s - (MANAGED and 0 or AUTO_H * s) - 8 * s
  local lw = w - (DETAIL_W + 14) * s
  local rows, groups = shownRows()
  columnHeader(x + 4 * s, by, lw, s)
  listView(rows, groups, x, by + 16 * s, lw, bodyH - 16 * s, s)
  local rx = x + w - DETAIL_W * s
  ui.DrawRectFilled(rx - 8 * s, by, rx - 7 * s, by + bodyH, C.ink[1], C.ink[2], C.ink[3], 0.06)
  detailsPane(rx, by, DETAIL_W * s, bodyH, s)
  if not MANAGED then
    ui.DrawLine(x, y + h - AUTO_H * s, x + w, y + h - AUTO_H * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
    autoskillStrip(x, y + h - AUTO_H * s + 10 * s, w, s)
  end
end

ui.window("Ability Book", function()
  if not eq.InGame() then return end
  hud.Dialog(WINDOW, "Ability Book", DIALOG, body)
end)
