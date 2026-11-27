-- Pieces the trading windows share (merchant, trade, give, bank, guild bank, quantity): coin piles bound to a
-- window's money buttons, coin amounts, empty states and small captions.
--
--   local TC = dofile("lib/trade_common.lua")
--   TC.MoneyButtons("BankWnd", { "BW_Money0", "BW_Money1", "BW_Money2", "BW_Money3" }, s)   -- platinum first
local API = hs.UI
local T = dofile("lib/theme.lua")
local C = T.color

local TC = {}

-- The coins' colours and letters, platinum first (the money buttons count platinum first: Money0 .. Money3).
TC.COIN = {
  { "pp", "Platinum", C.platinum },
  { "gp", "Gold", C.gold },
  { "sp", "Silver", C.silver },
  { "cp", "Copper", C.copper },
}

-- copper -> { platinum, gold, silver, copper }
function TC.Split(copper)
  copper = math.floor(tonumber(copper) or 0)
  return { copper // 1000, copper // 100 % 10, copper // 10 % 10, copper % 10 }
end

function TC.Copper(pp, gp, sp, cp)
  return (tonumber(pp) or 0) * 1000 + (tonumber(gp) or 0) * 100 + (tonumber(sp) or 0) * 10 + (tonumber(cp) or 0)
end

-- The coins the character carries, in copper.
function TC.Purse()
  local p, g, s_, c = API.character.Money()
  return TC.Copper(p, g, s_, c)
end

-- A coin amount inline: "12p 3g" in the coins' colours (zero coins left out; nothing at all: "0"). color: one colour
-- for every number (a price you cannot afford).
function TC.Amount(values, color)
  local first = true
  for i, v in ipairs(values) do
    v = tonumber(v) or 0
    if v ~= 0 then
      if not first then ui.SameLine(0, 5) end
      first = false
      local c = color or C.text
      ui.TextColored(c[1], c[2], c[3], 1, T.Thousands(v))
      ui.SameLine(0, 1)
      local k = TC.COIN[i][3]
      ui.TextColored(k[1], k[2], k[3], color and 0.6 or 1, TC.COIN[i][1]:sub(1, 1))
    end
  end
  if first then
    local c = color or C.faint
    ui.TextColored(c[1], c[2], c[3], 1, "0")
  end
end

local FITTED_LABEL_X, FITTED_RIGHT_INSET, FITTED_TEXT_GAP = 15, 5, 7

-- One coin pile as a button bound to a money child: its count (the child's text), the coin's colour; a click is
-- the child's (pick the pile up, or drop the coin on the cursor). A fitted pile supplies the count measured for
-- layout, so drawing and measuring use the same text. Returns clicked.
function TC.MoneyButton(win, id, coin, s, width, tooltip, pile)
  local enabled = select(1, API.windows.ChildState(win, id)) ~= false
  local n = pile and pile.count or tonumber((API.windows.Text(win, id))) or 0
  local amount = pile and pile.amount or T.Thousands(n)
  local labelX = pile and FITTED_LABEL_X or 17
  local rightInset = pile and FITTED_RIGHT_INSET or 7
  local x, y = ui.GetCursorScreenPos()
  local _, th = T.TextSize("Ag", T.font.small)
  local h = th + 10 * s
  local clicked = ui.InvisibleButton("##money" .. id, width, h)
  local hov, act = ui.IsItemHovered(), ui.IsItemActive()
  local k = TC.COIN[coin][3]
  ui.DrawRectFilled(x, y, x + width, y + h, k[1], k[2], k[3], act and 0.20 or hov and 0.14 or 0.07, 5 * s)
  ui.DrawRect(x, y, x + width, y + h, k[1], k[2], k[3], hov and 0.7 or 0.25, 5 * s, 1)
  ui.DrawCircleFilled(x + (labelX - 7) * s, y + h / 2, 4 * s, k[1], k[2], k[3], 1)
  T.Text(x + labelX * s, y + (h - th) / 2, TC.COIN[coin][1]:sub(1, 1), k, T.font.small)
  T.Text(x + width - rightInset * s, y + (h - th) / 2, amount, n > 0 and C.text or C.faint, T.font.small, "right")
  if hov and tooltip then ui.SetTooltip(TC.COIN[coin][2] .. ": " .. amount .. "\n" .. tooltip) end
  if clicked and enabled then
    API.windows.ClickChild(win, id, "left")
    return true
  end
  return false
end

-- The four money buttons of a window in a row that shares the width (ids platinum first; width: the row's, else
-- the rest of the line).
function TC.MoneyButtons(win, ids, s, tooltip, width)
  local avail = width or ui.GetContentRegionAvail()
  local gap = 6 * s
  local bw = (avail - gap * (#ids - 1)) / #ids
  for i, id in ipairs(ids) do
    if i > 1 then ui.SameLine(0, gap) end
    TC.MoneyButton(win, id, i, s, bw, tooltip)
  end
end

-- Trade offer columns can be narrow even in a wide window. Size each column for its longest exact amount and
-- wrap the four piles to two columns (or one) when the label and the amount would otherwise collide.
function TC.FittedMoneyButtons(win, ids, s, tooltip, width)
  -- The extra pixel covers T.Text rounding its screen position before drawing.
  local padding = (FITTED_LABEL_X + FITTED_TEXT_GAP + FITTED_RIGHT_INSET) * s + 1
  local piles = {}
  for i, id in ipairs(ids) do
    local count = tonumber((API.windows.Text(win, id))) or 0
    local amount = T.Thousands(count)
    local label = TC.COIN[i][1]:sub(1, 1)
    local textWidth = T.TextSize(label, T.font.small) + T.TextSize(amount, T.font.small)
    piles[i] = { count = count, amount = amount, width = textWidth + padding }
  end

  local gap = 6 * s
  for _, columns in ipairs({ 4, 2, 1 }) do
    local widths = {}
    for i, pile in ipairs(piles) do
      local column = (i - 1) % columns + 1
      widths[column] = math.max(widths[column] or 0, pile.width)
    end
    local needed = gap * (columns - 1)
    for _, w in ipairs(widths) do needed = needed + w end
    if needed <= width or columns == 1 then
      local extra = math.max(0, width - needed) / columns
      for i, id in ipairs(ids) do
        local column = (i - 1) % columns + 1
        if column > 1 then ui.SameLine(0, gap) end
        TC.MoneyButton(win, id, i, s, widths[column] + extra, tooltip, piles[i])
      end
      return
    end
  end
end

-- A muted line centred in the space left (or `h` high): what an empty list or page means.
function TC.Empty(text, h)
  local w, ah = ui.GetContentRegionAvail()
  h = h or ah
  local tw, th = T.TextSize(text, T.font.small)
  local x, y = ui.GetCursorScreenPos()
  T.Text(x + math.max(0, (w - tw) / 2), y + math.max(0, (h - th) / 2), text, C.faint, T.font.small)
  ui.Dummy(w, math.max(h, th))
end

-- A small muted caption with an optional value at the right edge of the line.
function TC.Caption(text, right, rightColor)
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(x, y, text, C.muted, T.font.small)
  if right and right ~= "" then T.Text(x + w, y, right, rightColor or C.muted, T.font.small, "right") end
  ui.Dummy(w, th + 2)
end

return TC
