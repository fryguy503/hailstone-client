-- Pieces the social windows share (friends, guild, raid, looking for guild, inspect, mail): a search box, an empty
-- state, class colours and abbreviations, a toolbar line, and reading a list child's rows. Display only: every
-- action still goes to the game window's children through lib/widgets.lua.
local API = hs.UI
local T = dofile("lib/theme.lua")
local C = T.color

local S = {}

-- ---------------------------------------------------------------- classes
-- The class names the game's lists show (game/eq_names.cpp), their abbreviations and a colour per role (a tank,
-- a healer, a melee or hybrid damage dealer, a caster): the colours are this UI's, for reading a roster at a glance.
local ROLE_COLOR = {
  tank = { 0.45, 0.70, 1.00, 1 },
  healer = { 0.42, 0.88, 0.55, 1 },
  melee = { 1.00, 0.66, 0.36, 1 },
  caster = { 0.78, 0.60, 1.00, 1 },
}
local CLASSES = {
  { "Warrior", "WAR", "tank" }, { "Cleric", "CLR", "healer" }, { "Paladin", "PAL", "tank" },
  { "Ranger", "RNG", "melee" }, { "Shadow Knight", "SHD", "tank" }, { "Druid", "DRU", "healer" },
  { "Monk", "MNK", "melee" }, { "Bard", "BRD", "melee" }, { "Rogue", "ROG", "melee" },
  { "Shaman", "SHM", "healer" }, { "Necromancer", "NEC", "caster" }, { "Wizard", "WIZ", "caster" },
  { "Magician", "MAG", "caster" }, { "Enchanter", "ENC", "caster" }, { "Beastlord", "BST", "melee" },
  { "Berserker", "BER", "melee" },
}
local byName, byAbbr = {}, {}
for i, c in ipairs(CLASSES) do
  local e = { name = c[1], abbr = c[2], role = c[3], bit = i - 1 }
  byName[c[1]:lower()] = e
  byAbbr[c[2]] = e
end
-- A server's names for its classes (API.character.ClassName, set by its plugin): entries as the stock ones, with
-- its name and short name, made again when ClassNamesRevision moves. The lists show those names.
local HAS_OWN = API.Supports and API.Supports("character", "ClassNamesRevision") == true
local ownRevision, ownByName, ownByAbbr = nil, {}, {}
local function ownNames()
  if not HAS_OWN then return ownByName, ownByAbbr end
  local r = API.character.ClassNamesRevision()
  if r ~= ownRevision then
    ownRevision, ownByName, ownByAbbr = r, {}, {}
    for i, c in ipairs(CLASSES) do
      local name, abbr = API.character.ClassName(i), API.character.ClassName(i, true)
      if name ~= c[1] or abbr ~= c[2] then
        local e = { name = name, abbr = abbr, role = c[3], bit = i - 1 }
        ownByName[name:lower()] = e
        ownByAbbr[abbr:upper()] = e
      end
    end
  end
  return ownByName, ownByAbbr
end
S.CLASSES = CLASSES
S.ROLE_COLOR = ROLE_COLOR
S.ROLE_NAME = { tank = "Tank", healer = "Healer", melee = "Melee", caster = "Caster" }

-- The class entry for a list's class text ("Shadow Knight", "SHD", or a multiclass "SHD/CLR": its first class); a
-- server's name for a class first, as the lists show those.
function S.Class(text)
  if not text or text == "" then return nil end
  local ownName, ownAbbr = ownNames()
  local e = ownName[text:lower()] or ownAbbr[text:upper()] or byName[text:lower()] or byAbbr[text:upper()]
  if e then return e end
  local first = text:match("^([^/]+)/")
  if first then
    return ownAbbr[first:upper()] or ownName[first:lower()] or byAbbr[first:upper()] or byName[first:lower()]
  end
  return nil
end

function S.ClassColor(text)
  local e = S.Class(text)
  return e and ROLE_COLOR[e.role] or C.text
end

function S.ClassAbbr(text)
  local e = S.Class(text)
  return e and e.abbr or (text or "")
end

-- ---------------------------------------------------------------- pieces
-- A search box filling the line (or `width`); returns the text and its lower-case needle ("" for none).
function S.Search(id, text, hint, width)
  ui.SetNextItemWidth(width or -1)
  text = ui.InputTextWithHint("##" .. id, hint or "Search", text or "", 64)
  return text, text:lower()
end

-- Whether any of the row's texts contains the needle.
function S.Matches(texts, needle, columns)
  if needle == "" then return true end
  if columns then
    for _, c in ipairs(columns) do
      if (texts[c] or ""):lower():find(needle, 1, true) then return true end
    end
    return false
  end
  for _, t in ipairs(texts) do
    if type(t) == "string" and t:lower():find(needle, 1, true) then return true end
  end
  return false
end

-- A centred empty state inside the rectangle the cursor is at (w x h): a line and an optional hint under it.
function S.Empty(w, h, text, hint)
  local x, y = ui.GetCursorScreenPos()
  local _, th = T.TextSize("Ag", T.font.small)
  local total = th + (hint and th + 4 or 0)
  local cy = y + math.max(0, (h - total) / 2)
  T.Text(x + w / 2, cy, text, C.muted, T.font.small, "center")
  if hint then T.Text(x + w / 2, cy + th + 4, hint, C.faint, T.font.tiny, "center") end
  ui.Dummy(w, h)
end

-- A muted caption over a field (the form layout the social windows use).
function S.Caption(text)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, text)
end

-- A small rounded chip at the cursor (a count, a status), on the current line.
function S.Chip(text, color, tooltip)
  local x, y = ui.GetCursorScreenPos()
  local tw, th = T.TextSize(text, T.font.tiny)
  local s = ui.GetIO().uiScale or 1
  local pw, ph = 6 * s, 2 * s
  local c = color or C.muted
  local _, lh = ui.CalcTextSize("Ag")
  local oy = math.max(0, (lh - th - 2 * ph) / 2)
  ui.DrawRectFilled(x, y + oy, x + tw + 2 * pw, y + oy + th + 2 * ph, c[1], c[2], c[3], 0.16, 8 * s)
  T.Text(x + pw, y + oy + ph, text, c, T.font.tiny)
  ui.Dummy(tw + 2 * pw, lh)
  if tooltip and ui.IsItemHovered() then ui.SetTooltip(tooltip) end
end

-- Whether two hs.ListRow texts tables read the same (the texts, the cell colours, the icon's column).
local function sameTexts(t, u)
  if #t ~= #u or t.iconColumn ~= u.iconColumn then return false end
  for i = 1, #t do if t[i] ~= u[i] then return false end end
  local a, b = t.colors, u.colors
  if (a == nil) ~= (b == nil) then return false end
  if a then
    for k, c in pairs(a) do
      local d = b[k]
      if not d or c[1] ~= d[1] or c[2] ~= d[2] or c[3] ~= d[3] then return false end
    end
    for k in pairs(b) do if not a[k] then return false end end
  end
  return true
end
local function sameRow(e, r, texts, cr, cg, cb)
  if not e or e.row ~= r then return false end
  local c, t = e.color, e.texts
  return c[1] == cr and c[2] == cg and c[3] == cb and sameTexts(t, texts)
end
local lastRows = {} -- [win][id] = the rows S.Rows gave last call
-- The rows of a list child: { { row = 0-based, texts = {...}, color = { r, g, b } }, ... }, selected (0-based).
-- A row that reads as it did last call is last call's table (its texts and colour unchanged), so a list that stands
-- still makes no new tables but the list's own answers; the rows are the caller's to read, not to change.
function S.Rows(win, id)
  local rows, _, selected = API.windows.ListInfo(win, id)
  local out = {}
  if not rows then return out, -1 end
  local byId = lastRows[win]
  if not byId then byId = {}; lastRows[win] = byId end
  local last = byId[id]
  for r = 0, rows - 1 do
    local texts, cr, cg, cb = API.windows.ListRow(win, id, r)
    if texts then
      cr, cg, cb = cr or 1, cg or 1, cb or 1
      local e = last and last[#out + 1]
      if not sameRow(e, r, texts, cr, cg, cb) then e = { row = r, texts = texts, color = { cr, cg, cb } } end
      out[#out + 1] = e
    end
  end
  byId[id] = out
  return out, selected or -1
end

-- The height of a line of buttons (the text and the frame padding), for leaving room under a list.
function S.ButtonHeight(s)
  local _, lh = ui.CalcTextSize("Ag")
  return lh + 10 * s
end

-- Testing: test.click.<window>=child,child,... in ui/settings/modern.txt clicks those children of the window
-- once, two seconds after it first draws (its lists filled) ("list:row" selects a list's row: "RAID_NotInGroupPlayerList:1"; "tabs=page" shows
-- a tab box's page: "Subwindows=1"; "edit~text" types into an edit box: "ToField~Nmscuinine").
local tested = {}
function S.TestClicks(win)
  if tested[win] == true then return end
  tested[win] = tested[win] or ui.GetTime()
  if ui.GetTime() - tested[win] < 2 then return end
  tested[win] = true
  local list = ui.GetSetting("test.click." .. win)
  if not list or list == "" then return end
  for item in list:gmatch("[^,]+") do
    local id, row = item:match("^(.-):(%d+)$")
    local box, pg = item:match("^(.-)=(%d+)$")
    local edit, text = item:match("^(.-)~(.*)$")
    if edit then API.windows.SetText(win, edit, text)
    elseif box then API.windows.SetTabPage(win, box, tonumber(pg))
    elseif id then API.windows.ListClick(win, id, tonumber(row), "left") else API.windows.ClickChild(win, item, "left") end
  end
end

return S
