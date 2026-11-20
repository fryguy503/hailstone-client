-- Item details (gamepad pack): stands in for ItemDisplayWindow (Y on an item, or the held right button). The text
-- is the stock window's, as the game builds it (CItemDisplayWnd: the info lines, the stat grid, the description),
-- laid out in one readable column. X links the item into chat (the stock icon button); B closes it.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local CL = dofile("lib/compare_lines.lua")
local C = T.C

local IDW = "ItemDisplayWindow"

local function shown(id) return API.windows.ChildShown(IDW, id) ~= false end
local function textOf(id)
  local t = API.windows.Text(IDW, id)
  return t or ""
end
local function colourOf(id)
  local r, g, b = API.windows.ChildColor(IDW, id)
  return r and { r, g, b, 1 } or nil
end

-- The lines of `text` word-wrapped to `width`, kept per `key` (a child) while its text, the width and the scale are:
-- the window draws the same item's text every frame.
local wrapped = {}
local function wrapLines(key, text, width)
  local e = wrapped[key]
  if e and e.text == text and e.width == width and e.S == T.S then return e.lines end
  local lines = {}
  for para in (text .. "\n"):gmatch("(.-)\n") do
    local line = ""
    for word in para:gmatch("%S+") do
      local try = line == "" and word or (line .. " " .. word)
      if ui.CalcTextSize(try) > width and line ~= "" then
        lines[#lines + 1] = line
        line = word
      else
        line = try
      end
    end
    if line ~= "" then lines[#lines + 1] = line end
  end
  wrapped[key] = { text = text, width = width, S = T.S, lines = lines }
  return lines
end

-- Word-wrapped text from x, y in `width`; returns the bottom.
local function wrap(key, text, x, y, width, c)
  local _, lh = ui.CalcTextSize("Ag")
  for _, line in ipairs(wrapLines(key, text, width)) do
    T.draw(x, y, line, c)
    y = y + lh
  end
  return y
end

-- The children's ids, made once.
local INFO_IDS, HEROIC_IDS, STAT_IDS, VALUE_IDS, KEY_IDS = {}, {}, {}, {}, {}
for n = 1, 12 do
  INFO_IDS[n] = "IDW_ItemInfo" .. n
  HEROIC_IDS[n] = "IDW_Heroic" .. n
end
for r = 1, 26 do
  STAT_IDS[r], VALUE_IDS[r], KEY_IDS[r] = {}, {}, {}
  for c = 1, 3 do
    STAT_IDS[r][c] = "IDW_Row" .. r .. "Col" .. c .. "Stat"
    VALUE_IDS[r][c] = "IDW_Row" .. r .. "Col" .. c .. "Value"
    KEY_IDS[r][c] = "IDW_Row" .. r .. "Col" .. c .. "Key" -- Hailstone's: the row's comparison key
  end
end
local HINTS = { { "x", "Link in chat" }, { "b", "Close" } }

ui.window("Item Details", function()
  if not W.shown(IDW) then return end
  T.frame()
  local title = API.windows.Title(IDW) or ""
  if W.begin("Item Details", { w = 1000, h = 860, title = title ~= "" and title or "Item", hints = HINTS }) then
    local x0, y0, x1, y1, w = W.area()
    -- The description below is plain text in this pack; keep its evolution link reachable as a pad button.
    local final = "Hailstone_EvolvingFinalResult"
    if API.windows.ChildShown(IDW, final) == true then
      local bh = T.px(56)
      ui.SetCursorScreenPos(x0, y1 - bh)
      T.font(20)
      if T.button("##evolvingfinal", textOf(final), w, bh,
          { disabled = API.windows.ChildState(IDW, final) == false }, pad) then
        API.windows.ClickChild(IDW, final, "left")
      end
      T.unfont()
      y1 = y1 - bh - T.px(12)
    end
    ui.PushClipRect(x0, y0, x1, y1, true)
    local y = y0
    -- the icon (the stock window's icon button)
    local tex, u0, v0, u1, v1 = API.windows.ChildArt(IDW, "IconButton", "decal")
    local s = T.px(80)
    if type(tex) == "userdata" then
      T.rect(x0, y, x0 + s, y + s, C.raised, 1, T.px(12))
      T.icon(tex, u0, v0, u1, v1, x0 + T.px(6), y + T.px(6), x0 + s - T.px(6), y + s - T.px(6))
      ui.SetCursorScreenPos(x0, y)
      if ui.InvisibleButton("##linkicon", s, s) then API.windows.ClickChild(IDW, "IconButton", "left") end
    end
    if W.pressed("x") then API.windows.ClickChild(IDW, "IconButton", "left") end
    T.font(20)
    local _, lh = ui.CalcTextSize("Ag")
    -- the info lines (flags, class / race, slots, ...) beside the icon
    local iy = y
    for n = 1, 12 do
      local id = INFO_IDS[n]
      if API.windows.ChildShown(IDW, id) == true then
        local t = textOf(id)
        if t ~= "" then iy = wrap(id, t, x0 + s + T.px(18), iy, w - s - T.px(18), colourOf(id)) end
      end
    end
    y = math.max(y + s, iy) + T.px(16)
    if API.evolving.CanTransferDisplayed(IDW) then
      ui.SetCursorScreenPos(x0, y)
      if T.button("##evolve-transfer-entry", "Transfer evolving experience", w, T.px(54), nil, pad) then
        API.evolving.OpenFromDisplay(IDW)
      end
      y = y + T.px(66)
    end
    -- Hailstone: the item against what you wear (docs/item-compare.md); each stat's change for you beside its value
    local compare = API.settings.Option("item_compare") ~= 0 and API.compare.SlotItem(IDW, "Hailstone_Item") or nil
    local deltas = compare and CL.byKey(compare.slots[1]) or nil
    local shownKeys = {}
    -- the stat grid: three columns of name / value
    local colW = (w - T.px(40)) / 3
    local any = false
    for r = 1, 26 do
      local rowAny = false
      for c = 1, 3 do
        local stat = STAT_IDS[r][c]
        local value = VALUE_IDS[r][c]
        if API.windows.ChildShown(IDW, stat) == true then
          local name, v = textOf(stat), textOf(value)
          if name ~= "" or v ~= "" then
            local cx = x0 + (c - 1) * (colW + T.px(20))
            T.draw(cx, y, name, C.dim)
            local right = cx + colW
            local key = deltas and textOf(KEY_IDS[r][c]) or ""
            local ch = key ~= "" and not shownKeys[key] and deltas[key]
            if ch then
              shownKeys[key] = true
              local d = CL.signed(ch.key, ch.delta)
              local dw = ui.CalcTextSize(d)
              T.draw(right - dw, y, d, I.compareColor(CL.kind(ch)))
              right = right - dw - T.px(10)
            end
            local vw = ui.CalcTextSize(v)
            T.draw(right - vw, y, v, colourOf(value))
            rowAny = true
          end
        end
      end
      if rowAny then
        y = y + lh + T.px(2)
        any = true
      end
    end
    if any then y = y + T.px(12) end
    -- heroic bonuses
    for n = 1, 12 do
      local id = HEROIC_IDS[n]
      if API.windows.ChildShown(IDW, id) == true then
        local t = textOf(id)
        if t ~= "" then
          T.draw(x0, y, t, colourOf(id) or C.endurance)
          y = y + lh
        end
      end
    end
    -- the rest of the comparison: the verdict, what it is compared with, the changes the grid has no row for
    if compare then
      ui.DrawLine(x0, y, x1, y, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
      y = I.comparison(compare, x0, y + T.px(6), x1, y1, shownKeys) + T.px(6)
    end
    -- the description
    local desc = textOf("ItemDescription"):gsub("<[^>]*>", "")
    if desc ~= "" then
      ui.DrawLine(x0, y, x1, y, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
      y = wrap("ItemDescription", desc, x0, y + T.px(12), w, C.text)
    end
    T.unfont()
    ui.PopClipRect()
    ui.Dummy(1, 1)
  end
  if W.finish() then API.windows.Show(IDW, false) end
end)
