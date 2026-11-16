-- Training (gamepad pack): stands in for TrainWindow, a guildmaster's. Your practice points and money on top, the
-- skills the trainer teaches as rows (the skill, its rank, a bar of its value toward the trainer's cap, the price
-- of a point); A selects one (the trainer answers in chat, as the stock list's click does), X trains the selected
-- one (the stock Train button: enabled when the game allows it), Y changes the order (the trainer's, by name, most
-- room to train, by price). Done (or B) is the stock Done button. The trainer's name is the window's title.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/markets.lua")
local C = T.C

local TW = "TrainWindow"
local SORTS = { "Trainer's order", "Name", "Most room to train", "Price" }
local sortBy = 1
local skills = {}

local function value(t)
  local v, cap = (t[3] or ""):match("(%d+)%s*/%s*(%d+)")
  return tonumber(v) or 0, tonumber(cap) or 0
end

-- The sort keys of the orders after the trainer's own.
local BY = {
  [2] = function(r) return (r.texts[1] or ""):lower() end,
  [3] = function(r) local v, c = value(r.texts) return -(c - v) end,
  [4] = function(r) return M.coinsOf(r.texts, 4) end,
}
local function sorter(by)
  return function(a, b)
    local x, y = by(a), by(b)
    if x ~= y then return x < y end
    return a.index < b.index
  end
end
local SORTERS = { [2] = sorter(BY[2]), [3] = sorter(BY[3]), [4] = sorter(BY[4]) }

-- The rows in the chosen order (W.list over a sorted copy; clicks go to the original's row). The order (the rows'
-- positions) is sorted again every 0.25 s and when the order or the row count changes; each frame takes the rows'
-- current texts in it.
local order, positions, sortedAt, sortedBy, sortedN = {}, {}, nil, nil, -1
local function ordered()
  local rows = M.rows(TW, "SkillList")
  local n, now = #rows, ui.GetTime()
  if sortBy == 1 then
    for i = 1, n do order[i] = rows[i] end
  elseif not sortedAt or now - sortedAt >= 0.25 or now < sortedAt or sortedBy ~= sortBy or sortedN ~= n then
    for i = 1, n do order[i] = rows[i] end
    for i = #order, n + 1, -1 do order[i] = nil end -- (before the sort: it takes the whole table)
    table.sort(order, SORTERS[sortBy])
    local at = {}
    for i, r in ipairs(rows) do at[r] = i end
    for i = 1, n do positions[i] = at[order[i]] end
    sortedAt, sortedBy, sortedN = now, sortBy, n
  else
    for i = 1, n do order[i] = rows[positions[i]] end
  end
  for i = #order, n + 1, -1 do order[i] = nil end
  return order
end

ui.window("Training", function()
  if not M.visible(TW) then
    sortedAt = nil -- (sorted afresh when it opens)
    return
  end
  M.frame()
  local title = API.windows.Title(TW) or ""
  if W.begin("Training", { w = 1500, h = 960, title = title ~= "" and title or "Training",
        subtitle = "Order: " .. SORTS[sortBy],
        hints = { { "a", "Select" }, { "x", "Train" }, { "y", "Order" }, { "b", "Done" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if M.pressed("Training", "x") then API.windows.ClickChild(TW, "TrainButton", "left") end
    if M.pressed("Training", "y") then sortBy = sortBy % #SORTS + 1 end
    -- practice points and money
    local pp = tonumber(API.windows.Text(TW, "PracticeCount") or "") or 0
    M.facts({ { "Practice points", tostring(pp), pp > 0 and C.gold or C.faint },
      { "Platinum", API.windows.Text(TW, "CoinCount0") or "0", C.platinum }, { "Gold", API.windows.Text(TW, "CoinCount1") or "0", C.gold },
      { "Silver", API.windows.Text(TW, "CoinCount2") or "0", C.silver }, { "Copper", API.windows.Text(TW, "CoinCount3") or "0", C.copper } },
      x0, x1, y0)
    local top = y0 + T.px(80)
    local bh = T.px(64)
    local order = ordered()
    local _, _, selected = API.windows.ListInfo(TW, "SkillList")
    selected = selected or -1
    skills.shown = math.max(1, math.floor((y1 - bh - T.px(14) - top + T.px(10)) / T.px(70)))
    skills.selected = nil
    for i, r in ipairs(order) do if r.index == selected then skills.selected = i end end
    ui.SetCursorPos(x0 - wx, top - wy)
    W.list("train", skills, #order, skills.shown, 60, x1 - x0, function(i, rx0, ry0, rx1, ry1)
      local r = order[i]
      if not r then return end
      local t = r.texts
      local v, cap = value(t)
      local maxed = cap > 0 and v >= cap
      local nameW = (rx1 - rx0) * 0.36
      T.font(22)
      local _, th = ui.CalcTextSize("Ag")
      local mid = (ry0 + ry1) / 2
      T.textIn(rx0 + T.px(16), mid - th, rx0 + nameW, mid, T.fit(t[1] or "", nameW - T.px(20)), maxed and C.dim or (r.color or C.text), 0)
      T.unfont()
      T.font(17)
      T.textIn(rx0 + T.px(16), mid, rx0 + nameW, mid + th, t[2] or "", C.faint, 0)
      T.unfont()
      local bx0, bx1 = rx0 + nameW + T.px(10), rx1 - T.px(260)
      T.font(18)
      T.bar(bx0, mid - T.px(14), bx1, mid + T.px(14), cap > 0 and v / cap or 0, maxed and C.faint or C.accent,
        string.format("%d / %d", v, cap))
      T.unfont()
      T.font(20)
      local cp = M.coinsOf(t, 4)
      if cp == 0 then T.textIn(rx1 - T.px(240), ry0, rx1 - T.px(16), ry1, "free", C.dim, 1)
      else M.price(cp, rx1 - T.px(16), ry0, ry1, 1) end
      T.unfont()
    end, function(i)
      local r = order[i]
      if r then API.windows.ListClick(TW, "SkillList", r.index, "left") end
    end)
    if #order == 0 then
      T.font(20)
      T.draw(x0 + T.px(14), top + T.px(16), "This trainer has nothing to teach you.", C.faint)
      T.unfont()
    end
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    M.buttons(TW, { { "TrainButton", "Train", { primary = true, glyph = "x" } }, { "DoneButton", "Done" } }, x0, x1, bh)
  end
  if W.finish() then API.windows.ClickChild(TW, "DoneButton", "left") end
end)
