-- The Find window's state and look (hs.FindNpcs; Hailstone's Find window, the map's Find page), shared by the map
-- (map.lua) and the minimap (minimap.lua), so both mark the same NPCs: those of the kinds not left out that match the
-- search.
--
--   local FL = dofile("lib/find.lua")
--   FL.state             -- { search, sel (the selected NPC's id), hover (the hovered row's id), why, menu }
--   FL.CATS              -- hs.FindCategories(): { { key, label }, ... }
--   FL.rows()            -- the rows, nearest first, read again every 0.2 s (shared: do not change it)
--   FL.current(id, token) -- the same currently authorized actor, or nil
--   FL.hidden()          -- the kinds left out, as a set of keys (find.hidden in ui/settings/modern.txt)
--   FL.setHidden(key, hide)
--   FL.marks()           -- whether the maps mark every listed NPC (map.npcMarks; the selected and hovered always)
--   FL.drawMark(sx, sy, row, s, lit, label, T) -- a diamond in the kind's colour, named (with T, lib/theme) when `label`
local API = hs.UI
local FL = {}
local NO_ROWS = {} -- shared, read-only empty result while NPC Find is unavailable

FIND_STATE = FIND_STATE or { search = "", sel = nil, hover = nil, why = nil, menu = nil }
FL.state = FIND_STATE

local function clearState()
  FIND_STATE.sel, FIND_STATE.selToken, FIND_STATE.hover, FIND_STATE.hoverToken = nil, nil, nil, nil
  FIND_STATE.menu, FIND_STATE.why, FIND_STATE.rowsCache = nil, nil, nil
end

-- Policy is live state, not a saved display preference. The two map scripts share the same cache and selection.
function FL.policy()
  local query = API.map.Policy
  local policy = type(query) == "function" and query() or nil
  local stock = type(policy) ~= "table" or policy.stock_npcs ~= false
  local find = type(policy) ~= "table" or policy.npc_find ~= false
  local serverOnly = type(policy) == "table" and policy.npc_find_server_only == true
  local custom = type(policy) == "table" and policy.custom_spawns == true
  local name = type(policy) == "table" and policy.name or ""
  local revision = type(policy) == "table" and policy.revision or nil
  local context = type(policy) == "table" and policy.context or nil
  if FIND_STATE.policyStock ~= stock or FIND_STATE.policyFind ~= find or FIND_STATE.policyCustom ~= custom
      or FIND_STATE.policyServerOnly ~= serverOnly
      or FIND_STATE.policyName ~= name or FIND_STATE.policyRevision ~= revision or FIND_STATE.policyContext ~= context then
    clearState()
    FIND_STATE.policyStock, FIND_STATE.policyFind, FIND_STATE.policyCustom, FIND_STATE.policyName = stock, find, custom, name
    FIND_STATE.policyServerOnly = serverOnly
    FIND_STATE.policyRevision, FIND_STATE.policyContext = revision, context
  end
  if not find then clearState() end
  return stock, find, serverOnly
end
-- FL.policy once a frame (ui.GetTime is the frame's) for the rows and the marks, which both maps ask several times.
local policyTime, policyStock, policyFind
local function framePolicy()
  local now = ui.GetTime()
  if now ~= policyTime then policyTime, policyStock, policyFind = now, FL.policy() end
  return policyStock, policyFind
end

FL.CATS = API.find.Categories and API.find.Categories() or {}
FL.SHORT = { merchant = "Merchants", banker = "Bankers", guildmaster = "Guildmasters", special = "Special",
  service = "Services", quest = "Quests", poi = "Points" }
FL.COLORS = {
  merchant = { 1.00, 0.80, 0.36 }, banker = { 0.40, 0.90, 0.55 }, guildmaster = { 0.48, 0.78, 1.00 },
  special = { 1.00, 0.55, 0.30 }, service = { 0.80, 0.55, 1.00 }, quest = { 1.00, 0.92, 0.25 },
  poi = { 0.92, 0.92, 0.92 },
}
local TEXT = {}
for key, c in pairs(FL.COLORS) do TEXT[key] = { c[1], c[2], c[3], 1 } end
FL.TEXT = TEXT

-- parsed again only when the setting changes
local hiddenRaw, hiddenSet = nil, {}
function FL.hidden()
  local raw = ui.GetSetting("find.hidden") or ""
  if raw ~= hiddenRaw then
    hiddenRaw, hiddenSet = raw, {}
    for key in raw:gmatch("[^,]+") do hiddenSet[key] = true end
  end
  return hiddenSet
end

function FL.setHidden(key, hide)
  local set, parts = FL.hidden(), {}
  for _, c in ipairs(FL.CATS) do
    local h = set[c[1]]
    if c[1] == key then h = hide end
    if h then parts[#parts + 1] = c[1] end
  end
  ui.SetSetting("find.hidden", table.concat(parts, ","))
end

-- The rows are read again every ROWS_REREAD seconds, as before map policies, or at once when the zone, the search,
-- the kinds left out or the server's map policy (FL.policy clears the cache) change. Acting on a row is checked by
-- the client against its token then (hs.UI.find), so a row kept for a moment cannot act on a replaced NPC.
local ROWS_REREAD = 0.2
function FL.matches(row, id, token)
  return row ~= nil and row.id == id and row.token == token
end

function FL.rows()
  local _, allowed = framePolicy()
  if not allowed then return NO_ROWS end
  -- a new zone: its spawn ids are its own, so the old selection would name another NPC
  local zone = select(2, API.map.ZoneName()) or ""
  if zone ~= FIND_STATE.zone then
    clearState()
    FIND_STATE.zone = zone
  end
  local hidden = FL.hidden()
  local now = ui.GetTime()
  local c = FIND_STATE.rowsCache
  if c and c.zone == zone and c.search == FIND_STATE.search and c.hidden == hiddenRaw
      and now - c.at < ROWS_REREAD and now >= c.at then
    return c.rows
  end
  local out, byId = {}, {}
  for _, r in ipairs(API.find.Npcs and API.find.Npcs(FIND_STATE.search) or {}) do
    if not hidden[r.category] then out[#out + 1], byId[r.id] = r, r end
  end
  if FIND_STATE.sel ~= nil and not FL.matches(byId[FIND_STATE.sel], FIND_STATE.sel, FIND_STATE.selToken) then
    FIND_STATE.sel, FIND_STATE.selToken, FIND_STATE.why = nil, nil, nil
  end
  if FIND_STATE.hover ~= nil and not FL.matches(byId[FIND_STATE.hover], FIND_STATE.hover, FIND_STATE.hoverToken) then
    FIND_STATE.hover, FIND_STATE.hoverToken = nil, nil
  end
  local menu = FIND_STATE.menu
  if menu then
    local current = byId[menu.id]
    if FL.matches(current, menu.id, menu.token) then FIND_STATE.menu = current
    else FIND_STATE.menu, FIND_STATE.why = nil, nil end
  end
  FIND_STATE.rowsCache = { zone = zone, search = FIND_STATE.search, hidden = hiddenRaw, at = now, rows = out }
  return out
end

function FL.current(id, token)
  if id == nil then return nil end
  for _, row in ipairs(FL.rows()) do if FL.matches(row, id, token) then return row end end
end

function FL.marks() return framePolicy() and ui.GetSetting("map.npcMarks") == "1" end

function FL.drawMark(sx, sy, r, s, lit, label, T)
  local col = FL.COLORS[r.category] or FL.COLORS.poi
  local d = (lit and 6 or 4.5) * s
  ui.DrawTriangleFilled(sx, sy - d - 1, sx - d - 1, sy + 1, sx + d + 1, sy + 1, 0, 0, 0, 0.55)
  ui.DrawTriangleFilled(sx, sy + d + 1, sx - d - 1, sy - 1, sx + d + 1, sy - 1, 0, 0, 0, 0.55)
  ui.DrawTriangleFilled(sx, sy - d, sx - d, sy, sx + d, sy, col[1], col[2], col[3], 1)
  ui.DrawTriangleFilled(sx, sy + d, sx - d, sy, sx + d, sy, col[1], col[2], col[3], 1)
  if label and T then T.Text(sx + d + 3 * s, sy - 8 * s, r.name, TEXT[r.category] or TEXT.poi, T.font.small) end
end

return FL
