-- The map window's World view and zone finder (docs/navigation.md "Zone atlas"; Hailstone's, not the original's
-- atlas page). map.lua draws it; this file keeps the state.
--   * the atlas: the game folder's Atlas pages (eq.AtlasIndex, eq.AtlasPageData), zoomed with the wheel and panned
--     by dragging, a breadcrumb back to the world; a region click opens it, a zone click selects it, a double click
--     shows its map; your zone pulses, the selected zone and a route's zones are marked;
--   * the finder: every atlas zone by name (the zone table's long name or the atlas' own), the zones of the page
--     shown when nothing is typed; the selected zone's card with its route from here (eq.ZoneRoute) and buttons to
--     find it on the atlas, view its map and follow the route;
--   * a route followed (ui/settings/modern.txt, atlas.route): a banner over your zone's map with the next zone and
--     "Walk there" (eq.ZoneExitWalk: a /nav walk through that zone line), and the exits marked on the map.
-- Routes come from the server's zone list (Hailstone's server script) or, without it, from the zone lines of the
-- zones you have been to on this server (eq.ZoneLinks). Only the zones the server has are listed and routed through
-- (its current expansion and content flags, from the script; else the account's expansions); the atlas dims the rest.
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local managed = eq.ManagedDistribution and eq.ManagedDistribution() or false

local A = {
  page = "Default.xml", zoom = 1, cx = nil, cy = nil, sel = nil, search = "", lastClick = 0, scrollTo = nil,
}

-- The expansions as the zone table numbers them (game::zone_expansion).
local EXPANSIONS = {
  [0] = "Classic", "Kunark", "Velious", "Luclin", "Planes of Power", "Ykesha", "Lost Dungeons", "Gates of Discord",
  "Omens of War", "Dragons of Norrath", "Depths of Darkhollow", "Prophecy of Ro", "Serpent's Spine", "Buried Sea",
  "Secrets of Faydwer", "Seeds of Destruction", "Underfoot", "House of Thule", "Veil of Alaris", "Rain of Fear",
}
function A.ExpansionName(n) return EXPANSIONS[n or -1] end

-- ---------------------------------------------------------------- data
local index, byZone, byPage, pageTitles, pageParents = nil, nil, nil, nil, nil
local indexKey = nil
local allowedCount = 0

-- What decides which zones the server has; the index is read again when it changes.
local function limitKey()
  local server, _, limit, expansion = eq.ZoneLinks()
  return tostring(server) .. ":" .. tostring(limit) .. ":" .. tostring(expansion)
end

-- The atlas' zones, one entry per zone (its first page), with what the finder searches.
local function loadIndex()
  local key = limitKey()
  if index and key == indexKey then return end
  indexKey = key
  allowedCount = 0
  local raw = eq.AtlasIndex and eq.AtlasIndex() or { pages = {}, zones = {} }
  index, byZone, byPage, pageTitles, pageParents = {}, {}, {}, {}, {}
  for _, p in ipairs(raw.pages) do
    pageTitles[p.file:lower()] = p.title
    pageParents[p.file:lower()] = p.parent
  end
  for _, z in ipairs(raw.zones) do
    local key = z.zone:lower()
    byPage[z.page:lower()] = byPage[z.page:lower()] or {}
    table.insert(byPage[z.page:lower()], z)
    if not byZone[key] then
      local e = {
        zone = z.zone, id = z.id, page = z.page, region = pageTitles[z.page:lower()] or z.page,
        name = z.name, expansion = z.id ~= 0 and z.expansion or nil, allowed = z.allowed ~= false,
      }
      if e.allowed then allowedCount = allowedCount + 1 end
      e.find = (e.name .. " " .. e.zone):lower()
      byZone[key] = e
      index[#index + 1] = e
    end
  end
  table.sort(index, function(a, b) return a.name:lower() < b.name:lower() end)
end
function A.Zone(short) loadIndex(); return short and byZone[short:lower()] end

local pages = {}
local function pageData(file)
  local key = file:lower()
  if pages[key] == nil then pages[key] = eq.AtlasPageData and eq.AtlasPageData(file) or false end
  return pages[key] or nil
end

-- The routes from your zone, by destination, kept until your zone or what is known changes.
local routeCache = { key = nil, routes = {} }
local function routeTo(short)
  if not short then return nil end
  local _, here = eq.ZoneName()
  local server, known, limit, expansion = eq.ZoneLinks()
  local key = (here or "") .. ":" .. tostring(server) .. ":" .. known .. ":" .. tostring(limit) .. ":" .. tostring(expansion)
  if routeCache.key ~= key then routeCache.key, routeCache.routes = key, {} end
  local k = short:lower()
  local r = routeCache.routes[k]
  if r == nil then
    r = eq.ZoneRoute(short) or false
    routeCache.routes[k] = r
  end
  return r or nil
end
A.RouteTo = routeTo

function A.Target()
  local t = ui.GetSetting("atlas.route")
  return t ~= nil and t ~= "" and t or nil
end
function A.SetTarget(short) ui.SetSetting("atlas.route", short or "") end

-- The next zone on the route followed, and the route; nil when none is followed or no way is known.
function A.NextHop()
  local target = A.Target()
  local r = routeTo(target)
  if not r or #r < 2 then return nil, r end
  return r[2], r
end

local function zoneName(short)
  local e = A.Zone(short)
  return e and e.name or short
end
A.ZoneName = zoneName

-- ---------------------------------------------------------------- opening a page or a zone
local function openPage(file)
  A.page, A.zoom, A.cx, A.cy = file, 1, nil, nil
end

-- The atlas turned to the zone's page and centred on it.
function A.Show(short)
  loadIndex()
  local e = byZone[(short or ""):lower()]
  if not e then return false end
  A.sel = e.zone
  openPage(e.page)
  for _, z in ipairs(byPage[e.page:lower()] or {}) do
    if z.zone:lower() == e.zone:lower() then
      A.cx, A.cy, A.zoom = (z.x0 + z.x1) / 2, (z.y0 + z.y1) / 2, 1.6
      break
    end
  end
  return true
end

-- Each time the World view opens: your zone's page, when nothing was chosen yet.
function A.Opened()
  local _, here = eq.ZoneName()
  if not A.sel and here and A.Show(here) then A.sel = nil end
end

-- ---------------------------------------------------------------- the atlas view
local function pulse(speed) return 0.5 + 0.5 * math.sin((ui.GetTime() or 0) * (speed or 3)) end

-- A badge: a filled circle with a number or a letter.
local function badge(x, y, r, text, fill, s)
  ui.DrawCircleFilled(x + 1, y + 1.5, r + 1, 0, 0, 0, 0.45)
  ui.DrawCircleFilled(x, y, r, fill[1], fill[2], fill[3], 1)
  ui.DrawCircle(x, y, r, 1, 1, 1, 0.85, 1.5 * s)
  local _, th = T.TextSize("0", T.font.tiny)
  T.Text(x, y - th / 2, text, C.onAccent or { 0, 0, 0, 1 }, T.font.tiny, "center", 1, { 0, 0, 0, 0 })
end

-- onView(short): the map window shows that zone's map (map.lua switches to the Zone view).
function A.DrawView(x0, y0, x1, y1, s, onView)
  loadIndex()
  local page = pageData(A.page)
  ui.DrawRectFilled(x0, y0, x1, y1, C.mapBg[1], C.mapBg[2], C.mapBg[3], 0.95, 8 * s)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y0 - wy)
  ui.InvisibleButton("##atlasarea", math.max(x1 - x0, 1), math.max(y1 - y0, 1))
  local hovered, active = ui.IsItemHovered(), ui.IsItemActive()
  if not page or (page.width or 0) <= 0 then
    T.Text((x0 + x1) / 2, (y0 + y1) / 2 - 8 * s, "No atlas in the game folder (Atlas\\Default).", C.muted,
      T.font.small, "center")
    return
  end
  local vw, vh = x1 - x0, y1 - y0
  local fit = math.min(vw / page.width, vh / page.height)
  local k = fit * A.zoom
  local cx, cy = A.cx or page.width / 2, A.cy or page.height / 2
  local vcx, vcy = (x0 + x1) / 2, (y0 + y1) / 2
  local function P(px, py) return vcx + (px - cx) * k, vcy + (py - cy) * k end
  local mx, my = ui.GetMousePos()

  ui.PushClipRect(x0, y0, x1, y1, true)
  -- the pictures (the page's tiled backdrop under them)
  if page.background and (page.bgw or 0) > 0 then
    local tex = eq.AtlasTexture(page.background)
    if tex then
      local tw, th = page.bgw * k, page.bgh * k
      local sx0, sy0 = P(0, 0)
      sx0 = sx0 - math.ceil((sx0 - x0) / tw) * tw
      sy0 = sy0 - math.ceil((sy0 - y0) / th) * th
      for ty = sy0, y1, th do
        for tx = sx0, x1, tw do ui.DrawImage(tex, tx, ty, tx + tw, ty + th, 0, 0, 1, 1, 1, 1, 1, 0.55) end
      end
    end
  end
  for _, im in ipairs(page.images) do
    local tex, tw, th = eq.AtlasTexture(im.file)
    if tex and tw > 0 and th > 0 then
      local ax, ay = P(im.x0, im.y0)
      local bx, by = P(im.x1, im.y1)
      ui.DrawImage(tex, ax, ay, bx, by, 0, 0, math.min(1, (im.x1 - im.x0) / tw), math.min(1, (im.y1 - im.y0) / th))
    end
  end
  -- the page's labels, in the pack's font
  local labelFont = k >= 1.4 and T.font.normal or T.font.small
  for _, l in ipairs(page.labels) do
    local lx, ly = P(l.x, l.y)
    T.Text(lx, ly, l.text, { 1, 1, 1, 0.92 }, labelFont, "left", 1, { 0, 0, 0, 0.9 })
  end

  -- the route's zones on this page, in order
  local _, here = eq.ZoneName()
  here = (here or ""):lower()
  local target = A.Target()
  local route = routeTo(target)
  local onRoute = {}
  if route then
    for i, z in ipairs(route) do onRoute[z.zone:lower()] = i end
  end
  local centres = {}
  for _, l in ipairs(page.links) do
    if l.action == 2 then
      local key = l.target:lower()
      local ax, ay = P(l.x0, l.y0)
      local bx, by = P(l.x1, l.y1)
      centres[key] = centres[key] or { (ax + bx) / 2, (ay + by) / 2 }
    end
  end
  if route then
    local prev = nil
    for _, z in ipairs(route) do
      local c = centres[z.zone:lower()]
      if c and prev then
        ui.DrawLine(prev[1], prev[2] + 1.5, c[1], c[2] + 1.5, 0, 0, 0, 0.5, 4 * s)
        ui.DrawLine(prev[1], prev[2], c[1], c[2], C.accent[1], C.accent[2], C.accent[3], 0.9, 2.5 * s)
      end
      prev = c or nil
    end
  end

  -- the links: the last one under the mouse takes it (as the original's)
  local hover = nil
  if hovered then
    for i = #page.links, 1, -1 do
      local l = page.links[i]
      local ax, ay = P(l.x0, l.y0)
      local bx, by = P(l.x1, l.y1)
      if mx >= ax and my >= ay and mx < bx and my < by then
        hover = l
        break
      end
    end
  end
  for _, l in ipairs(page.links) do
    local ax, ay = P(l.x0, l.y0)
    local bx, by = P(l.x1, l.y1)
    local key = l.target:lower()
    local r = 6 * s
    local e = l.action == 2 and byZone[key] or nil
    if e and not e.allowed then
      -- not on this server: dimmed
      ui.DrawRectFilled(ax, ay, bx, by, 0, 0, 0, l == hover and 0.45 or 0.55, r)
    elseif l.action == 2 then
      local sel = A.sel and key == A.sel:lower()
      if l == hover then
        ui.DrawRectFilled(ax, ay, bx, by, C.accent[1], C.accent[2], C.accent[3], 0.16, r)
        ui.DrawRect(ax, ay, bx, by, C.accent[1], C.accent[2], C.accent[3], 0.75, r, 1.5 * s)
      end
      if sel then
        ui.DrawRect(ax - 3 * s, ay - 3 * s, bx + 3 * s, by + 3 * s, C.accent[1], C.accent[2], C.accent[3],
          0.25 + 0.25 * pulse(4), r + 3 * s, 4 * s)
        ui.DrawRect(ax, ay, bx, by, C.accent[1], C.accent[2], C.accent[3], 1, r, 2 * s)
      end
      if key == here then
        local c = centres[key]
        local p = pulse(3)
        ui.DrawCircle(c[1], c[2], (10 + 10 * p) * s, C.gold[1], C.gold[2], C.gold[3], 0.9 * (1 - p), 2.5 * s)
        ui.DrawCircleFilled(c[1], c[2], 6 * s, C.gold[1], C.gold[2], C.gold[3], 1)
        ui.DrawCircle(c[1], c[2], 6 * s, 0, 0, 0, 0.7, 1.5 * s)
        local tw, th = T.TextSize("You are here", T.font.tiny)
        local bx0, by0 = c[1] - tw / 2 - 6 * s, c[2] - 26 * s - th
        ui.DrawRectFilled(bx0, by0, bx0 + tw + 12 * s, by0 + th + 6 * s, 0, 0, 0, 0.72, 4 * s)
        T.Text(c[1], by0 + 3 * s, "You are here", C.gold, T.font.tiny, "center")
      end
      local step = onRoute[key]
      if step and key ~= here then
        local c = centres[key]
        badge(c[1], c[2], 8 * s, tostring(step - 1), C.accent, s)
      end
    elseif l == hover then
      ui.DrawRectFilled(ax, ay, bx, by, 1, 1, 1, 0.10, r)
      ui.DrawRect(ax, ay, bx, by, 1, 1, 1, 0.45, r, 1.5 * s)
    end
  end
  ui.PopClipRect()
  ui.DrawRect(x0, y0, x1, y1, C.ink[1], C.ink[2], C.ink[3], 0.08, 8 * s, 1)

  -- the hovered link's tip, and its clicks
  local hoverZone = hover and hover.action == 2 and byZone[hover.target:lower()] or nil
  if hoverZone and not hoverZone.allowed then
    ui.BeginTooltip()
    ui.Text(hoverZone.name)
    local exp = A.ExpansionName(hoverZone.expansion)
    ui.TextDisabled("Not on this server" .. (exp and (" (" .. exp .. ")") or ""))
    ui.EndTooltip()
    hover = nil
  end
  if hover then
    if hover.action == 2 then
      local e = byZone[hover.target:lower()]
      ui.BeginTooltip()
      ui.Text((e and e.name) or hover.tip or hover.target)
      local exp = e and A.ExpansionName(e.expansion)
      if exp then ui.TextDisabled(exp) end
      local r = routeTo(hover.target)
      if r then ui.TextDisabled(#r == 1 and "You are here" or string.format("%d zone%s from here", #r - 1, #r == 2 and "" or "s")) end
      ui.TextDisabled("Click: select   Double-click: its map")
      ui.EndTooltip()
    else
      ui.SetTooltip(hover.tip ~= "" and hover.tip or (pageTitles[hover.target:lower()] or hover.target))
    end
    if ui.IsMouseDoubleClicked(ui.Mouse.Left) and hover.action == 2 then
      if onView then onView(hover.target) end
    elseif ui.IsMouseReleased(ui.Mouse.Left) and not ui.IsMouseDragging(ui.Mouse.Left) and not A.dragged then
      if hover.action == 1 then
        openPage(hover.target)
      else
        A.sel = hover.target
        A.scrollTo = hover.target
      end
    end
  end

  -- pan, zoom around the cursor
  local io = ui.GetIO()
  if ui.IsMouseClicked(ui.Mouse.Left) then A.dragged = false end
  if active and ui.IsMouseDragging(ui.Mouse.Left) and ((io.mouseDX or 0) ~= 0 or (io.mouseDY or 0) ~= 0) then
    A.dragged = true
    A.cx, A.cy = cx - (io.mouseDX or 0) / k, cy - (io.mouseDY or 0) / k
  end
  if hovered and (io.wheel or 0) ~= 0 then
    local ax, ay = cx + (mx - vcx) / k, cy + (my - vcy) / k
    local z = math.max(1, math.min(5, A.zoom * (io.wheel > 0 and 1.2 or 1 / 1.2)))
    local k2 = fit * z
    A.zoom = z
    if z <= 1.0001 then
      A.cx, A.cy = nil, nil
    else
      A.cx, A.cy = ax - (mx - vcx) / k2, ay - (my - vcy) / k2
    end
  end
  -- kept on the page
  if A.cx then
    A.cx = math.max(0, math.min(page.width, A.cx))
    A.cy = math.max(0, math.min(page.height, A.cy))
  end
end

-- The breadcrumb from the world to the page shown (map.lua's toolbar in the World view). Returns its width.
function A.DrawCrumbs(x, y, s, maxW)
  loadIndex()
  local chain, f = {}, A.page
  for _ = 1, 16 do
    if not f or f == "" then break end
    table.insert(chain, 1, f)
    f = pageParents[f:lower()]
  end
  local tx = x
  local _, th = T.TextSize("Ag", T.font.small)
  for i, file in ipairs(chain) do
    local title = pageTitles[file:lower()] or file
    local last = i == #chain
    local tw = T.TextSize(title, last and T.font.normal or T.font.small)
    if tx + tw > x + maxW then break end
    if last then
      local _, nh = T.TextSize("Ag", T.font.normal)
      T.Text(tx, y + (26 * s - nh) / 2, title, C.text, T.font.normal)
    else
      local clicked = T.Hit("##crumb" .. i, tx - 4 * s, y, tx + tw + 4 * s, y + 26 * s)
      local hov = ui.IsItemHovered()
      if hov then ui.DrawRectFilled(tx - 4 * s, y, tx + tw + 4 * s, y + 26 * s, C.ink[1], C.ink[2], C.ink[3], 0.07, 5 * s) end
      T.Text(tx, y + (26 * s - th) / 2, title, hov and C.text or C.muted, T.font.small)
      if clicked then openPage(file) end
      tx = tx + tw + 6 * s
      T.Text(tx, y + (26 * s - th) / 2, ">", C.faint, T.font.small)
      tx = tx + T.TextSize(">", T.font.small) + 6 * s
    end
    tx = tx + (last and tw or 0)
  end
  return tx - x
end

-- ---------------------------------------------------------------- the finder
local function matches(q)
  loadIndex()
  local out = {}
  if q == "" then
    for _, z in ipairs(byPage[A.page:lower()] or {}) do
      local e = byZone[z.zone:lower()]
      if e and e.allowed and not out[e] then
        out[e] = true
        out[#out + 1] = e
      end
    end
    table.sort(out, function(a, b) return a.name:lower() < b.name:lower() end)
    return out
  end
  local first, rest = {}, {}
  for _, e in ipairs(index) do
    local at = e.allowed and e.find:find(q, 1, true)
    if at == 1 or (at and e.name:lower():find("%f[%w]" .. q:gsub("%p", "%%%0"))) then
      first[#first + 1] = e
    elseif at then
      rest[#rest + 1] = e
    end
  end
  for _, e in ipairs(rest) do first[#first + 1] = e end
  return first
end

local resultsKey, results = nil, {}

local function routeLines(x, y, w, s, route, target)
  local _, th = T.TextSize("Ag", T.font.small)
  local ly = y
  for i, z in ipairs(route) do
    local last = i == #route
    local cxp = x + 9 * s
    if not last then
      ui.DrawLine(cxp, ly + 9 * s, cxp, ly + 26 * s, C.accent[1], C.accent[2], C.accent[3], 0.5, 2 * s)
    end
    if i == 1 then
      ui.DrawCircleFilled(cxp, ly + 8 * s, 5 * s, C.gold[1], C.gold[2], C.gold[3], 1)
    else
      ui.DrawCircleFilled(cxp, ly + 8 * s, last and 6 * s or 4.5 * s, C.accent[1], C.accent[2], C.accent[3], 1)
    end
    T.Text(x + 22 * s, ly + 8 * s - th / 2, T.Fit(i == 1 and (z.name .. "  (here)") or z.name, w - 24 * s, T.font.small),
      last and C.text or C.muted, T.font.small)
    ly = ly + 18 * s
  end
  return ly - y
end

-- onView(short): show that zone's map. Returns nothing.
function A.DrawFinder(x, y, w, h, s, onView)
  loadIndex()
  local wx, wy = ui.GetWindowPos()
  local server, known, limit, expansion = eq.ZoneLinks()
  T.Text(x, y, "Zones", C.text, T.font.normal)
  T.Text(x + w, y + 3 * s, allowedCount > 0 and string.format("%d on this server", allowedCount) or "", C.faint, T.font.tiny, "right")
  ui.SetCursorPos(x - wx, y + 24 * s - wy)
  ui.SetNextItemWidth(w)
  A.search = ui.InputTextWithHint("##zonesearch", "Find a zone", A.search, 64)
  local q = A.search:lower():gsub("^%s+", ""):gsub("%s+$", "")
  local key = q .. "|" .. A.page .. "|" .. tostring(indexKey)
  if key ~= resultsKey then resultsKey, results = key, matches(q) end

  -- the selected zone's card at the bottom
  local sel = A.sel and byZone[A.sel:lower()]
  if sel and not sel.allowed then sel, A.sel = nil, nil end
  local route = sel and routeTo(sel.zone)
  local cardH = 0
  if sel then
    cardH = 112 * s
    if route and #route > 1 then cardH = cardH + math.min(#route, 8) * 18 * s + 6 * s end
  end
  local footer = (limit and 34 or 20) * s
  local ly = y + 56 * s
  T.Text(x, ly, q == "" and ("In " .. (pageTitles[A.page:lower()] or "this region")) or
    string.format("%d found", #results), C.faint, T.font.tiny)
  ly = ly + 16 * s
  local listH = math.max(y + h - ly - cardH - footer - 8 * s, 30 * s)
  ui.SetCursorPos(x - wx, ly - wy)
  ui.BeginChild("##zonelist", w, listH, false)
  local rw = ui.GetContentRegionAvail()
  local _, th = T.TextSize("Ag", T.font.small)
  local _, here = eq.ZoneName()
  here = (here or ""):lower()
  if #results == 0 then
    local rx, ry = ui.GetCursorScreenPos()
    T.Text(rx, ry + 4 * s, q == "" and "No zones on this page." or "No zone by that name.", C.faint, T.font.small)
  end
  for i, e in ipairs(results) do
    local rx, ry = ui.GetCursorScreenPos()
    local rh = 38 * s
    ui.InvisibleButton("##zr" .. i, rw, rh)
    local hov, on = ui.IsItemHovered(), A.sel and A.sel:lower() == e.zone:lower()
    if A.scrollTo and A.scrollTo:lower() == e.zone:lower() then
      ui.SetScrollHereY(0.5)
      A.scrollTo = nil
    end
    if on then
      ui.DrawRectFilled(rx, ry, rx + rw, ry + rh, C.accent[1], C.accent[2], C.accent[3], 0.15, 6 * s)
    elseif hov then
      ui.DrawRectFilled(rx, ry, rx + rw, ry + rh, C.ink[1], C.ink[2], C.ink[3], 0.05, 6 * s)
    end
    local isHere = e.zone:lower() == here
    local dot = isHere and C.gold or (A.Target() and A.Target():lower() == e.zone:lower()) and C.accent or C.faint
    ui.DrawCircleFilled(rx + 10 * s, ry + rh / 2, 4 * s, dot[1], dot[2], dot[3], 1)
    T.Text(rx + 22 * s, ry + 4 * s, T.Fit(e.name, rw - 28 * s, T.font.small), C.text, T.font.small)
    local sub = e.region
    local exp = A.ExpansionName(e.expansion)
    if exp then sub = sub .. "  ·  " .. exp end
    if isHere then sub = "You are here" end
    T.Text(rx + 22 * s, ry + 6 * s + th, T.Fit(sub, rw - 28 * s, T.font.tiny), isHere and C.gold or C.faint, T.font.tiny)
    if ui.IsItemClicked(ui.Mouse.Left) then
      A.sel = e.zone
      if q ~= "" then A.Show(e.zone) end
    end
    if hov and ui.IsMouseDoubleClicked(ui.Mouse.Left) and onView then onView(e.zone) end
  end
  ui.EndChild()

  -- the card
  if sel then
    local cy = y + h - footer - cardH
    ui.DrawRectFilled(x, cy, x + w, cy + cardH - 4 * s, C.ink[1], C.ink[2], C.ink[3], 0.05, 8 * s)
    local px = x + 10 * s
    local iw = w - 20 * s
    T.Text(px, cy + 8 * s, T.Fit(sel.name, iw, T.font.normal), C.text, T.font.normal)
    local sub = sel.region
    local exp = A.ExpansionName(sel.expansion)
    if exp then sub = sub .. "  ·  " .. exp end
    T.Text(px, cy + 28 * s, T.Fit(sub, iw, T.font.tiny), C.faint, T.font.tiny)
    local ry = cy + 46 * s
    local isHere = sel.zone:lower() == here
    if isHere then
      T.Text(px, ry, "You are here.", C.gold, T.font.small)
      ry = ry + 20 * s
    elseif route then
      local n = #route - 1
      T.Text(px, ry, string.format("%d zone%s away%s", n, n == 1 and "" or "s", route.assumed and " (some lines seen one way only)" or ""),
        C.muted, T.font.small)
      ry = ry + 20 * s
      local shown = route
      if #route > 8 then
        shown = {}
        for i = 1, 7 do shown[i] = route[i] end
        shown[8] = route[#route]
        shown[7] = { name = string.format("... %d more", #route - 8) }
      end
      ry = ry + routeLines(px, ry, iw, s, shown) + 6 * s
    else
      T.Text(px, ry, server and "The server lists no way there from here." or
        (known > 0 and "No known way yet: routes fill in as you travel." or "No routes known yet: they fill in as you travel."),
        C.faint, T.font.tiny)
      ry = ry + 20 * s
    end
    local bw = (iw - 8 * s) / 3
    local by = cy + cardH - 36 * s
    if T.Button("##zatlas", px, by, px + bw, by + 26 * s, s, "On atlas") then A.Show(sel.zone) end
    if T.Button("##zmap", px + bw + 4 * s, by, px + 2 * bw + 4 * s, by + 26 * s, s, "Its map") and onView then onView(sel.zone) end
    local following = A.Target() and A.Target():lower() == sel.zone:lower()
    if following then
      if T.Button("##zroute", px + 2 * bw + 8 * s, by, px + iw, by + 26 * s, s, "Stop route") then A.SetTarget(nil) end
    elseif T.Button("##zroute", px + 2 * bw + 8 * s, by, px + iw, by + 26 * s, s, "Go here",
      { primary = true, enabled = route ~= nil and not isHere, tooltip = "Follow the route: the map shows the way out of each zone" }) then
      A.SetTarget(sel.zone)
    end
  end

  -- which zones are listed, and where the routes come from
  local lim
  if limit == "server" then
    local exp = expansion ~= nil and expansion >= 0 and A.ExpansionName(expansion)
    lim = exp and ("Zones up to " .. exp .. ", the server's expansion") or "Zones the server has"
  elseif limit == "account" then
    lim = "Zones of your account's expansions"
  end
  local src = server and "Routes from the server's zone list" or
    (known > 0 and string.format("Routes from zone lines seen in %d zones", known) or "Routes: none known yet")
  if lim then T.Text(x, y + h - 28 * s, T.Fit(lim, w, T.font.tiny), C.faint, T.font.tiny) end
  T.Text(x, y + h - 14 * s, T.Fit(src, w, T.font.tiny), C.faint, T.font.tiny)
end

-- ---------------------------------------------------------------- over your zone's map
-- The banner of the route followed, at the top of the map area. onView(short) opens the World view on a zone.
function A.DrawRouteBanner(x0, y0, x1, s, onWorld)
  local target = A.Target()
  if not target then return end
  local nextHop, route = A.NextHop()
  local _, here = eq.ZoneName()
  local h = 34 * s
  local bx0, bx1 = x0 + 10 * s, x1 - 10 * s
  local by0 = y0 + 8 * s
  ui.DrawRectFilled(bx0 + 1, by0 + 2, bx1 + 1, by0 + h + 2, 0, 0, 0, 0.35, 8 * s)
  ui.DrawRectFilled(bx0, by0, bx1, by0 + h, C.window and C.window[1] or 0.08, C.window and C.window[2] or 0.1,
    C.window and C.window[3] or 0.14, 0.92, 8 * s)
  ui.DrawRect(bx0, by0, bx1, by0 + h, C.accent[1], C.accent[2], C.accent[3], 0.55, 8 * s, 1)
  local _, th = T.TextSize("Ag", T.font.small)
  local ty = by0 + (h - th) / 2
  local tx = bx0 + 12 * s
  local btnW = 0
  local text, color
  if here and here:lower() == target:lower() then
    text, color = "You have arrived in " .. zoneName(target) .. ".", C.gold
  elseif not route then
    text, color = "No known way to " .. zoneName(target) .. " from here.", C.muted
  else
    local n = #route - 1
    text = string.format("To %s  ·  %d zone%s  ·  next: %s", zoneName(target), n, n == 1 and "" or "s", nextHop.name)
    color = C.text
  end
  -- buttons from the right: close, then (on the way) Walk there
  local cx1 = bx1 - 6 * s
  if T.Button("##routeend", cx1 - 26 * s, by0 + 4 * s, cx1, by0 + h - 4 * s, s, "x", { tooltip = "Stop following the route" }) then
    A.SetTarget(nil)
  end
  cx1 = cx1 - 30 * s
  if T.Button("##routeworld", cx1 - 54 * s, by0 + 4 * s, cx1, by0 + h - 4 * s, s, "Route", { tooltip = "The route on the atlas" }) and onWorld then
    onWorld(target)
  end
  cx1 = cx1 - 58 * s
  if nextHop and not managed then
    local canWalk = A.ExitTo(nextHop.zone) ~= nil
    if T.Button("##routewalk", cx1 - 90 * s, by0 + 4 * s, cx1, by0 + h - 4 * s, s, "Walk there",
      { primary = true, enabled = canWalk, tooltip = canWalk and ("Walk to the zone line to " .. nextHop.name .. " and through it (/nav)") or
        "No zone line to it found here (it may be a teleporter or a boat)" }) then
      eq.ZoneExitWalk(nextHop.zone)
    end
    cx1 = cx1 - 94 * s
  end
  btnW = bx1 - cx1
  ui.DrawCircleFilled(tx, by0 + h / 2, 4 * s, C.accent[1], C.accent[2], C.accent[3], 1)
  T.Text(tx + 10 * s, ty, T.Fit(text, bx1 - bx0 - btnW - 34 * s, T.font.small), color, T.font.small)
end

-- The exit to a zone in your zone (eq.ZoneExits), or nil.
local exitsCache = { frame = -1, list = {} }
local function exits()
  local f = ui.GetFrameCount()
  if exitsCache.frame ~= f then exitsCache.frame, exitsCache.list = f, eq.ZoneExits and eq.ZoneExits() or {} end
  return exitsCache.list
end
function A.ExitTo(short)
  for _, e in ipairs(exits()) do
    if e.zone:lower() == short:lower() then return e end
  end
  return nil
end

-- Your zone's exits on its map: toScreen(mapX, mapY) -> screen. The next zone of the route followed stands out.
function A.DrawExits(toScreen, s)
  local nextHop = A.NextHop()
  local nextKey = nextHop and nextHop.zone:lower()
  local p = pulse(3.5)
  for _, e in ipairs(exits()) do
    local isNext = e.zone:lower() == nextKey
    local ax, ay = toScreen(e.x0, e.y0)
    local bx, by = toScreen(e.x1, e.y1)
    local c = isNext and C.accent or C.gold
    -- the lines' box, at least a few pixels across
    if math.abs(bx - ax) < 4 * s then ax, bx = (ax + bx) / 2 - 2 * s, (ax + bx) / 2 + 2 * s end
    if math.abs(by - ay) < 4 * s then ay, by = (ay + by) / 2 - 2 * s, (ay + by) / 2 + 2 * s end
    ui.DrawRectFilled(math.min(ax, bx), math.min(ay, by), math.max(ax, bx), math.max(ay, by), c[1], c[2], c[3],
      isNext and (0.25 + 0.25 * p) or 0.18, 2 * s)
    local sx, sy = toScreen(e.x, e.y)
    local label = e.name
    local tw, th = T.TextSize(label, T.font.tiny)
    local lx0, ly0 = sx - tw / 2 - 6 * s, sy - th / 2 - 3 * s
    ui.DrawRectFilled(lx0, ly0, lx0 + tw + 12 * s, ly0 + th + 6 * s, 0, 0, 0, 0.7, 4 * s)
    ui.DrawRect(lx0, ly0, lx0 + tw + 12 * s, ly0 + th + 6 * s, c[1], c[2], c[3], isNext and 1 or 0.6, 4 * s, isNext and 2 * s or 1)
    T.Text(sx, ly0 + 3 * s, label, isNext and C.text or c, T.font.tiny, "center")
  end
end

return A
