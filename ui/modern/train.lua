-- Training: replaces the game's TrainWindow, the guildmaster's. Your practice points and money at the top, the
-- skills the trainer teaches as rows (the skill, its rank, a bar of its value toward the trainer's cap and the price
-- of a point), with a search and a sort, and Train for the selected one. The trainer's name is the window's title.
-- Rows are the original's list (a click selects in it, and the trainer answers in chat as the game does); Train is
-- its button, enabled when the game allows the training (points, money, level).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "TrainWindow"
local SORTS = { "Trainer's order", "Name", "Most room to train", "Price" }

local search, sortBy = "", 1
local needle = "" -- the search in lower case, this frame

-- Names in lower case for the search and the name sort, kept (a list's names come back as the same strings each
-- frame).
local lowered, nLowered = {}, 0
local function lowerOf(text)
  local l = lowered[text]
  if not l then
    if nLowered >= 2048 then lowered, nLowered = {}, 0 end
    l = text:lower()
    lowered[text], nLowered = l, nLowered + 1
  end
  return l
end

-- A skill's value and the trainer's cap ("12 / 50"), read from the cell's text once a text rather than in each of
-- the sort's comparisons (kept as lowerOf keeps its names).
local valueOf, capOf, nValues = {}, {}, 0
local function value(texts)
  local text = texts[3] or ""
  local v = valueOf[text]
  if v then return v, capOf[text] end
  if nValues >= 2048 then valueOf, capOf, nValues = {}, {}, 0 end
  local tv, tc = text:match("(%d+)%s*/%s*(%d+)")
  v = tonumber(tv) or 0
  local cap = tonumber(tc) or 0
  valueOf[text], capOf[text], nValues = v, cap, nValues + 1
  return v, cap
end

-- The price of a point in copper (the platinum, gold, silver and copper columns).
local function price(texts)
  return (tonumber(texts[4]) or 0) * 1000 + (tonumber(texts[5]) or 0) * 100 + (tonumber(texts[6]) or 0) * 10
      + (tonumber(texts[7]) or 0)
end

local COIN = { { "p", C.platinum }, { "g", C.gold }, { "s", C.silver }, { "c", C.copper } }

-- A price as coins, right-aligned at x1 (copper the rightmost); returns its width.
local function coinsAt(x1, y, texts)
  local x, drawn = x1, false
  for i = 4, 1, -1 do
    local n = tonumber(texts[3 + i]) or 0
    if n > 0 then
      drawn = true
      local coin, digits = COIN[i], tostring(n)
      T.Text(x, y + 2, coin[1], coin[2], T.font.tiny, "right")
      x = x - T.TextSize(coin[1], T.font.tiny) - 1
      T.Text(x, y, digits, C.text, T.font.small, "right")
      x = x - T.TextSize(digits, T.font.small) - 6
    end
  end
  if not drawn then
    T.Text(x1, y, "free", C.muted, T.font.small, "right")
    return T.TextSize("free", T.font.small)
  end
  return x1 - x
end

local NO_TRAIL = { noTrail = true }

local function drawRow(_, texts, x0, y0, x1, y1, s, st)
  local v, cap = value(texts)
  local pad = 10 * s
  local _, th = T.TextSize("Ag", T.font.small)
  local nameW = (x1 - x0) * 0.42
  local maxed = cap > 0 and v >= cap
  T.Text(x0 + pad, y0 + 4 * s, T.Fit(texts[1] or "", nameW, T.font.small), maxed and C.muted or st.color, T.font.small)
  T.Text(x0 + pad, y0 + 6 * s + th, texts[2] or "", C.faint, T.font.tiny)
  -- the value toward the trainer's cap
  local bx0, bx1 = x0 + pad + nameW + 8 * s, x1 - 96 * s
  if bx1 - bx0 > 30 * s then
    T.Text(bx0, y0 + 4 * s, string.format("%d / %d", v, cap), maxed and C.faint or C.muted, T.font.tiny)
    T.Bar("train." .. (texts[1] or ""), bx0, y0 + 20 * s, bx1, y0 + 25 * s, s, cap > 0 and v / cap or 0,
      maxed and C.faint or C.accent, NO_TRAIL)
  end
  coinsAt(x1 - pad, (y0 + y1 - th) / 2, texts)
end

-- a tie keeps the list's order: table.sort is not stable and the rows are sorted again every frame, so two rows of
-- one name would otherwise swap places from frame to frame
local function byName(a, b)
  local na, nb = lowerOf(a.texts[1] or ""), lowerOf(b.texts[1] or "")
  if na ~= nb then return na < nb end
  return a.row < b.row
end
local function byRoom(a, b)
  local av, ac = value(a.texts)
  local bv, bc = value(b.texts)
  if ac - av ~= bc - bv then return ac - av > bc - bv end
  return a.row < b.row
end
local function byPrice(a, b)
  local pa, pb = price(a.texts), price(b.texts)
  if pa ~= pb then return pa < pb end
  return a.row < b.row
end
local SORT_BY = { false, byName, byRoom, byPrice }

local function matchesSearch(_, texts) return lowerOf(texts[1] or ""):find(needle, 1, true) ~= nil end

local PRACTICE_TILE = { font = T.font.large, tooltip = "Each point trains a skill one step" }
local coins = {}
local ROWS = { rowH = 34, draw = drawRow }
local BUTTONS = {
  { "TrainButton", "Train", { primary = true,
    tooltip = "Spend a practice point and the price to raise the selected skill" } },
  { "DoneButton", "Done" },
}

local DIALOG = {
  size = function() return 520, 460 end,
  resizable = { 380, 300 },
}

local function body(x, y, w, h, s)
  -- practice points and money
  local tileH = 46 * s
  local practice = W.Text(WINDOW, "PracticeCount")
  PRACTICE_TILE.color = (tonumber(practice) or 0) > 0 and C.gold or C.faint
  P.Tile("##trnpts", x, y, 110 * s, tileH, s, "PRACTICE POINTS", practice ~= "" and practice or "0", PRACTICE_TILE)
  local mx = x + 118 * s
  ui.DrawRectFilled(mx, y, x + w, y + tileH, C.ink[1], C.ink[2], C.ink[3], 0.04, 6 * s)
  T.Text(mx + 10 * s, y + 6 * s, "YOUR MONEY", C.faint, T.font.tiny)
  ui.SetCursorScreenPos(mx + 10 * s, y + 22 * s)
  coins[1], coins[2], coins[3], coins[4] = W.Text(WINDOW, "CoinCount0"), W.Text(WINDOW, "CoinCount1"),
    W.Text(WINDOW, "CoinCount2"), W.Text(WINDOW, "CoinCount3")
  W.Coins(coins)
  ui.SetCursorScreenPos(x, y + tileH + 8 * s)
  -- search and sort
  local comboW = math.min(170 * s, w * 0.4)
  search = P.Search("##trnsearch", search, "Search skills", w - comboW - 8 * s)
  ui.SameLine()
  ui.SetNextItemWidth(comboW)
  if ui.BeginCombo("##trnsort", SORTS[sortBy]) then
    for i, v in ipairs(SORTS) do if ui.Selectable(v, sortBy == i) then sortBy = i end end
    ui.EndCombo()
  end
  needle = search:lower()
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  ROWS.sort = SORT_BY[sortBy] or nil
  ROWS.filter = needle ~= "" and matchesSearch or nil
  ROWS.empty = needle ~= "" and "No skill matches" or "This trainer has nothing to teach you"
  P.Rows(WINDOW, "SkillList", 0, -(btnH + 8 * s), ROWS)
  ui.SetCursorScreenPos(x, y + h - btnH)
  W.ButtonRow(WINDOW, BUTTONS, btnH)
end

ui.window("Training", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Training", DIALOG, body)
end)
