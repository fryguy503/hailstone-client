-- Item displays: replace the game's ItemDisplayWindow (and its copies ItemDisplayWindow#2..#6: the game shows
-- several at once). The item's information as one scrolling card: the icon and the name in the colour the game
-- gives it (green: you can equip it, red: you cannot), the flags as chips, the class / race / deity / slot lines as
-- captioned rows, the stat grid in its own columns (they fold into fewer as the window narrows, the heroic bonuses
-- beside their values, the game's tooltips on the names), and the description with its effects. Everything is the
-- game's text (CItemDisplayWnd's children: the info lines, IDW_Row<r>Col<c>Stat / Value, IDW_Heroic<n>, the
-- description STML); the Modified switch is the window's own button. The augment sockets are the window's socket
-- rows (UpdateAugmentSlots) with its buttons.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local stml = dofile("lib/stml.lua")
local CL = dofile("lib/compare_lines.lua")
local T = hud.T
local C = T.color
local INSTANCES = 6
local ROWS, INFO_LINES = 26, 12
local RADIUS5, RADIUS7 = { radius = 5 }, { radius = 7 }

-- The children's ids, made once (not concatenated again for every cell every frame).
local INFO_IDS, HEROIC_IDS, CELL_IDS = {}, {}, {}
for n = 1, INFO_LINES do INFO_IDS[n] = "IDW_ItemInfo" .. n end
for n = 1, 13 do HEROIC_IDS[n] = "IDW_Heroic" .. n end
for c = 1, 3 do
  CELL_IDS[c] = {}
  for r = 1, ROWS do CELL_IDS[c][r] = { "IDW_Row" .. r .. "Col" .. c .. "Stat", "IDW_Row" .. r .. "Col" .. c .. "Value" } end
end
local SOCKET_IDS = {}
for n = 1, 6 do
  local p = "IDW_Socket_Slot_" .. n
  SOCKET_IDS[n] = { screen = p .. "_Screen", item = p .. "_Item", desc = p .. "_Description" }
end

-- ---------------------------------------------------------------- reading the window
local function trim(s) return (s or ""):match("^%s*(.-)%s*$") end

local function childColor(win, id, default)
  local r, g, b = eq.ChildColor(win, id)
  return r and { r, g, b, 1 } or default
end

-- The info lines the game filled: { text, color } (line 1 the name, line 2 the flags, then the rest).
local function infoLines(win)
  local out = {}
  for n = 1, INFO_LINES do
    local id = INFO_IDS[n]
    if eq.ChildShown(win, id) == false then break end
    out[#out + 1] = { W.Text(win, id), childColor(win, id, C.text) }
  end
  return out
end

-- Captions for the lines after the flags: "Class: ..." and "Race: ..." name themselves (a second line of
-- abbreviations continues the one before it); a line of slot names is the slots; any other is the deities.
local SLOT_WORDS = {}
for w in ("Charm Ear Head Face Neck Shoulder Shoulders Arms Back Wrist Range Hands Primary Secondary Finger Fingers "
    .. "Chest Legs Feet Waist Power Source Ammo"):gmatch("%S+") do SLOT_WORDS[w] = true end

local function captioned(lines)
  local rows = {}
  for i = 3, #lines do
    local text, color = trim(lines[i][1]), lines[i][2]
    if text ~= "" then
      local cap, rest = text:match("^([%a ]+):%s*(.*)$")
      local last = rows[#rows]
      local abbrevs = text:match("^[%u%s]+$") ~= nil
      if cap then
        rows[#rows + 1] = { cap, rest, color, list = true }
      elseif last and last.list and abbrevs then
        last[2] = last[2] .. " " .. text
      else
        local slots = true
        for w in text:gmatch("%S+") do if not SLOT_WORDS[w] then slots = false end end
        rows[#rows + 1] = { slots and "Slots" or "Deity", text, color }
      end
    end
  end
  return rows
end

-- The stat grid by its columns: { { stat, value, color, tooltip, heroic } or false (a blank row between two
-- blocks) ... } per column, blanks only between cells.
local function statColumns(win)
  -- the heroic labels sit beside their values (eq.ChildRect: x by column, y by row, 16 pixels a row)
  local heroic = {}
  local _, y1 = eq.ChildRect(win, "IDW_Row1Col1Stat")
  for n = 1, 13 do
    local id = HEROIC_IDS[n]
    if eq.ChildShown(win, id) == false then break end
    local x, y = eq.ChildRect(win, id)
    if x and y1 then
      local col = x >= 360 and 3 or x >= 240 and 2 or 1
      local row = math.floor((y - y1) / 16 + 0.5) + 1
      heroic[row * 4 + col] = { trim(W.Text(win, id)), childColor(win, id, C.gold) }
    end
  end
  local cols = {}
  for c = 1, 3 do
    local list, gap = {}, false
    for r = 1, ROWS do
      local ids = CELL_IDS[c][r]
      local sid, vid = ids[1], ids[2]
      local stat = eq.ChildShown(win, sid) ~= false and trim(W.Text(win, sid)) or ""
      if stat ~= "" then
        if gap and #list > 0 then list[#list + 1] = false end
        gap = false
        list[#list + 1] = { stat:gsub(":$", ""), trim(W.Text(win, vid)), childColor(win, vid, C.text),
          eq.ChildTooltip(win, sid) or "", heroic[r * 4 + c] }
      else
        gap = true
      end
    end
    if #list > 0 then cols[#cols + 1] = list end
  end
  return cols
end

-- ---------------------------------------------------------------- drawing
local function chip(text, color, s)
  local tw, th = T.TextSize(text, T.font.small)
  local x, y = ui.GetCursorScreenPos()
  local pw, ph = 7 * s, 2 * s
  ui.DrawRectFilled(x, y, x + tw + 2 * pw, y + th + 2 * ph, color[1], color[2], color[3], 0.16, 9 * s)
  ui.DrawRect(x, y, x + tw + 2 * pw, y + th + 2 * ph, color[1], color[2], color[3], 0.45, 9 * s, 1)
  T.Text(x + pw, y + ph, text, color, T.font.small)
  ui.Dummy(tw + 2 * pw, th + 2 * ph)
end

local FLAG_COLOR = { Magic = C.accent, Lore = C.gold, ["No Trade"] = C.danger, Temporary = C.danger,
  Artifact = C.xp, Quest = C.pet, Heirloom = C.gold, Augmentation = C.xp, Summoned = C.muted,
  Collectible = C.xp, ["No Destroy"] = C.danger }

local function flags(text, s, width)
  local x0 = ui.GetCursorScreenPos()
  local used = 0
  for part in text:gmatch("[^,]+") do
    local f = trim(part)
    if f ~= "" then
      local tw = T.TextSize(f, T.font.small) + 14 * s
      if used > 0 and used + 6 * s + tw <= width then
        ui.SameLine(0, 6 * s)
      else
        if used > 0 then ui.SetCursorScreenPos(x0, select(2, ui.GetCursorScreenPos()) + 3 * s) end
        used = 0
      end
      chip(f, FLAG_COLOR[f] or C.muted, s)
      used = used + tw + 6 * s
    end
  end
  return x0
end

-- One "name  value +heroic" line of a stat column, `w` wide.
local function statLine(cell, w, s)
  local x, y = ui.GetCursorScreenPos()
  local _, th = T.TextSize("Ag", T.font.small)
  ui.Dummy(w, th + 3 * s)
  local hov = ui.IsItemHovered()
  if hov then ui.DrawRectFilled(x - 3 * s, y, x + w + 3 * s, y + th + 3 * s, C.ink[1], C.ink[2], C.ink[3], 0.05, 4 * s) end
  T.Text(x, y + 1 * s, cell[1], C.muted, T.font.small)
  local right = x + w
  if cell[5] and cell[5][1] ~= "" then
    local hw = T.Text(right, y + 1 * s, cell[5][1], cell[5][2], T.font.small, "right")
    right = right - hw - 4 * s
  end
  T.Text(right, y + 1 * s, cell[2], cell[3], T.font.small, "right")
  if hov and cell[4] ~= "" then ui.SetTooltip(cell[4]) end
end

local function statGrid(cols, s, width)
  if #cols == 0 then return end
  local gap = 18 * s
  local fit = math.max(1, math.min(#cols, math.floor((width + gap) / (150 * s + gap))))
  local groups = cols
  if fit < #cols then
    -- fewer columns than the game's: its columns one after the other, split evenly (a block keeps its gap)
    local flat = {}
    for i, col in ipairs(cols) do
      if i > 1 then flat[#flat + 1] = false end
      for _, cell in ipairs(col) do flat[#flat + 1] = cell end
    end
    groups = {}
    local per = math.ceil(#flat / fit)
    for i, cell in ipairs(flat) do
      local g = math.min(fit, (i - 1) // per + 1)
      groups[g] = groups[g] or {}
      local list = groups[g]
      if cell or #list > 0 then list[#list + 1] = cell end
    end
  end
  local n = #groups
  local cw = (width - gap * (n - 1)) / n
  local x0, y0 = ui.GetCursorScreenPos()
  local bottom = y0
  local _, th = T.TextSize("Ag", T.font.small)
  for g, list in ipairs(groups) do
    local gx = x0 + (g - 1) * (cw + gap)
    ui.SetCursorScreenPos(gx, y0)
    for i, cell in ipairs(list) do
      if cell then statLine(cell, cw, s)
      elseif i < #list then ui.Dummy(cw, th * 0.5) end
      ui.SetCursorScreenPos(gx, select(2, ui.GetCursorScreenPos()))
    end
    local _, y = ui.GetCursorScreenPos()
    bottom = math.max(bottom, y)
    if g > 1 then ui.DrawLine(gx - gap / 2, y0 + 2 * s, gx - gap / 2, y - 2 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1) end
  end
  ui.SetCursorScreenPos(x0, bottom)
  ui.Dummy(width, 0)
end

local stmlCache = {}
local function description(win, raw, width)
  local c = stmlCache[win]
  if not c or c.raw ~= raw then
    c = { raw = raw, lines = stml.Lines(raw, C.text) }
    stmlCache[win] = c
  end
  for _, line in ipairs(c.lines) do stml.DrawLine(line, width, function(link) eq.StmlLink(win, "ItemDescription", link[1], link[2]) end) end
  return #c.lines
end

-- ---------------------------------------------------------------- augment sockets
-- The window's socket rows (IDW_Socket_Slot_<n>_Screen / _Item / _Description, the appearance socket): the socket
-- button with the augment's icon and the game's tooltip, and the row's text (gold when the item on the cursor fits,
-- the augment's name a link to its display). Clicks go to the window as the stock buttons do: left inserts,
-- removes or swaps (Alt: shows the augment), right opens the socket menu, holding right shows the augment.
local socketCache = {}
local function socketText(win, id, width)
  local raw = W.Text(win, id)
  local key = win .. "\0" .. id
  local c = socketCache[key]
  if not c or c.raw ~= raw then
    c = { raw = raw, lines = stml.Lines(raw, C.text) }
    socketCache[key] = c
  end
  for _, line in ipairs(c.lines) do stml.DrawLine(line, width, function(link) eq.StmlLink(win, id, link[1], link[2]) end) end
end

local function socketButton(win, id, size, s)
  local x, y = ui.GetCursorScreenPos()
  ui.InvisibleButton("##sock" .. id, size, size)
  local key, hov = ui.GetItemID(), ui.IsItemHovered()
  local tex, u0, v0, u1, v1 = eq.ChildArt(win, id, "decal")
  if tex == false or type(tex) == "string" then tex = nil end
  T.Icon(x, y, size, s, tex, u0, v0, u1, v1, hov and C.accent or nil, RADIUS5)
  local function send(phase, outside) eq.ClickChild(win, id, phase, outside) end
  if ui.IsItemClicked(ui.Mouse.Left) then
    send("left")
  elseif ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
    T.RightPressStart(key, send)
  end
  T.RightPressFrame(key, send, hov)
  if hov then
    local tip = eq.ChildTooltip(win, id)
    if tip and tip ~= "" then ui.SetTooltip(tip) end
  end
end

local function socketRow(win, item, desc, s, width)
  local size = 22 * s
  local x, y = ui.GetCursorScreenPos()
  socketButton(win, item, size, s)
  ui.SetCursorScreenPos(x + size + 8 * s, y + 3 * s)
  ui.BeginGroup()
  socketText(win, desc, width - size - 8 * s)
  ui.EndGroup()
  local _, ny = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x, math.max(ny, y + size) + 3 * s)
end

local function sockets(win, s, width)
  local any = W.Shown(win, "IDW_Appearance_Socket_Screen")
  for n = 1, 6 do any = any or W.Shown(win, SOCKET_IDS[n].screen) end
  if not any then return end
  W.Heading("Augments")
  ui.PushFont(T.font.small)
  if W.Shown(win, "IDW_Appearance_Socket_Screen") then
    socketRow(win, "IDW_Appearance_Socket_Item", "IDW_Appearance_Socket_Description", s, width)
    if W.Shown(win, "IDW_Appearance_Socket_Buy_Button") then
      W.Button(win, "IDW_Appearance_Socket_Buy_Button", "Buy Now")
    end
  end
  for n = 1, 6 do
    local ids = SOCKET_IDS[n]
    if W.Shown(win, ids.screen) then
      socketRow(win, ids.item, ids.desc, s, width)
    end
  end
  ui.PopFont()
end

-- The info lines and the stat grid as read and parsed from the window: some 300 calls into the game and a table for
-- every line and cell, so they are read again only when what the body reads every frame anyway changes (the item's
-- icon, the Modified label, the description) or its name line does, when the Modified box is clicked here, and
-- otherwise every REREAD seconds (the game builds a shown item's display again at most once a second).
local REREAD = 0.25
local parsed = {}
local function readItem(win, mod, desc, tex, u0, v0)
  local name = eq.WindowText(win, INFO_IDS[1])
  local now = ui.GetTime()
  local p = parsed[win]
  if p and p.name == name and p.mod == mod and p.desc == desc and p.tex == tex and p.u0 == u0 and p.v0 == v0
      and now - p.at < REREAD and now >= p.at then
    return p
  end
  local lines = infoLines(win)
  p = { name = name, mod = mod, desc = desc, tex = tex, u0 = u0, v0 = v0, at = now, lines = lines,
    info = captioned(lines), cols = statColumns(win) }
  parsed[win] = p
  return p
end

-- ---------------------------------------------------------------- comparison
-- Hailstone: the item against what you wear (eq.CompareSlotItem over the window's Hailstone_Item;
-- docs/item-compare.md), every change, with a button per slot when it fits two (ears, wrists, rings).
local compareIndex = {} -- [win] = the slot shown
local CMP_KIND = { good = "good", bad = "bad", mixed = "gold" }
local function comparison(win)
  if eq.Option("item_compare") == 0 then return end
  local c = eq.CompareSlotItem(win, "Hailstone_Item")
  if not c then return end
  W.Heading("Compared with what you wear")
  local index = compareIndex[win] or 1
  if index > #c.slots then index = 1 end
  if #c.slots > 1 then
    for k, slot in ipairs(c.slots) do
      if k > 1 then ui.SameLine() end
      local label = slot.slotName .. "##cmp" .. k
      if ui.Selectable(label, k == index, 0, (ui.CalcTextSize(slot.slotName)), 0) then compareIndex[win] = k end
    end
  end
  ui.PushFont(T.font.small)
  ui.PushTextWrapPos(0)
  for _, line in ipairs(CL.lines(c, index, { verdict = eq.Option("item_compare_verdict") ~= 0, full = true })) do
    local text, kind = line[1], line[2]
    local col = CMP_KIND[kind] and C[CMP_KIND[kind]] or (kind == "muted" and C.muted or C.text)
    ui.TextColored(col[1], col[2], col[3], 1, text)
  end
  ui.PopTextWrapPos()
  ui.PopFont()
end

local function body(win, w, h, s)
  local tex, u0, v0, u1, v1 = eq.ChildArt(win, "IconButton", "decal")
  local mod = trim(W.Text(win, "IDW_ModButtonLabel"))
  local descRaw = W.Text(win, "ItemDescription")
  local item = readItem(win, mod, descRaw, tex, u0, v0)
  local lines = item.lines
  -- header: the icon, the name in the game's colour, the flags
  local x, y = ui.GetCursorScreenPos()
  local icon = 44 * s
  if tex == false then tex = nil end
  T.Icon(x, y, icon, s, tex, u0, v0, u1, v1, nil, RADIUS7)
  -- the stock window's icon button: a click links the item into the chat window you last used (or the one set to
  -- Always Chat Here)
  ui.SetCursorScreenPos(x, y)
  if ui.InvisibleButton("##linkicon", icon, icon) then eq.ClickChild(win, "IconButton", "left") end
  T.HoverFx(x, y, x + icon, y + icon, s, 7)
  if ui.IsItemHovered() then ui.SetTooltip("Click to link this item in chat") end
  local name = lines[1] or { eq.WindowTitle(win) or "", C.text }
  local tx = x + icon + 10 * s
  ui.SetCursorScreenPos(tx, y)
  ui.PushFont(T.font.large)
  ui.PushTextWrapPos(0)
  ui.TextColored(name[2][1], name[2][2], name[2][3], 1, trim(name[1]))
  ui.PopTextWrapPos()
  ui.PopFont()
  if ui.IsItemHovered() then
    ui.SetTooltip("Green: you can equip it.  Red: you cannot.  Purple: only the prestige check stops you.")
  end
  local flagText = lines[2] and trim(lines[2][1]) or ""
  if flagText ~= "" then
    ui.SetCursorScreenPos(tx, select(2, ui.GetCursorScreenPos()) + 2 * s)
    flags(flagText, s, w - icon - 10 * s)
  end
  local _, cy = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x, math.max(cy, y + icon) + 6 * s)

  -- the rest scrolls
  ui.BeginChild("##idbody", 0, 0, false)
  local cw = ui.GetContentRegionAvail()
  local info = item.info
  if #info > 0 then
    local capW = 0
    for _, r in ipairs(info) do capW = math.max(capW, T.TextSize(r[1], T.font.small)) end
    capW = capW + 12 * s
    for _, r in ipairs(info) do
      local rx, ry = ui.GetCursorScreenPos()
      T.Text(rx, ry + 1 * s, r[1], C.muted, T.font.small)
      ui.SetCursorScreenPos(rx + capW, ry)
      ui.PushFont(T.font.small)
      ui.PushTextWrapPos(0)
      ui.TextColored(r[3][1], r[3][2], r[3][3], 1, r[2])
      ui.PopTextWrapPos()
      ui.PopFont()
      local _, ny = ui.GetCursorScreenPos()
      ui.SetCursorScreenPos(rx, ny + 1 * s)
    end
  end
  local cols = item.cols
  if #cols > 0 then
    W.Heading("Stats")
    statGrid(cols, s, cw)
  end
  if mod ~= "" and eq.ChildShown(win, "IDW_ModButton") ~= false then
    -- the window's Modified button: its label says which values the grid shows
    ui.Dummy(0, 2 * s)
    local _, changed = ui.Checkbox("Modified values##mod", mod:lower() == "modified")
    if changed then
      eq.ClickChild(win, "IDW_ModButton", "left")
      parsed[win] = nil
    end
    if ui.IsItemHovered() then
      ui.SetTooltip("Off: the item's own values.  On: the values as they apply to you (a level below the item's\n"
        .. "recommended level lowers them).")
    end
  end
  sockets(win, s, cw)
  comparison(win)
  if trim(descRaw) ~= "" then
    W.Heading("Description")
    ui.PushFont(T.font.small)
    description(win, descRaw, cw)
    ui.PopFont()
  end
  ui.Dummy(0, 4 * s)
  ui.EndChild()
end

local function size() return 380, 440 end
for i = 1, INSTANCES do
  local window = i == 1 and "ItemDisplayWindow" or ("ItemDisplayWindow#" .. i)
  local title = "Item Display" .. (i == 1 and "" or (" " .. i))
  -- made once per window, not every frame
  local opts = {
    key = window,
    size = size,
    resizable = { 240, 180 },
    place = function(W_, H, w, h, s, x, y) return x + (i - 1) * 28 * s, y + (i - 1) * 28 * s end,
  }
  local function draw(x, y, w, h, s) body(window, w, h, s) end
  ui.window(title, function()
    if not eq.InGame() then return end
    hud.Dialog(window, title, opts, draw)
  end)
end
