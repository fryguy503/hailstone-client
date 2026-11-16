-- Trade: replaces the game's TradeWnd (a trade with another player). Two offers, theirs and yours, as cards side by
-- side (one above the other when the window is narrow): the eight item slots of each (tiles that rewrap to the
-- width), their coin and your coin piles, then Trade and Cancel.
-- The slots and buttons are the game's (hs.SlotItem / hs.ClickChild on TRDW_TradeSlot0..15, TRDW_MyMoney0..3,
-- TRDW_Trade_Button, TRDW_Cancel_Button): drop an item from your bags on one of your slots to offer it, click your
-- coin piles with coin on the cursor to add it, hold the right button on any item to see it.
-- Who has accepted is hs.TradeState()'s myReady / theirReady (the stock window's flags; any change to an offer
-- clears both): an accepted offer's card is framed and tagged "Accepted", a line above the buttons says where the
-- trade stands, and the partner's accept flashes the frame for a moment so it is not missed.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local TC = dofile("lib/trade_common.lua")
local T = hud.T
local C = T.color
local WINDOW = "TradeWnd"

local SLOT_IDS = {}
for i = 0, 15 do SLOT_IDS[i] = "TRDW_TradeSlot" .. i end
local MY_MONEY = { "TRDW_MyMoney0", "TRDW_MyMoney1", "TRDW_MyMoney2", "TRDW_MyMoney3" }

local function filled(first, last)
  local n = 0
  for i = first, last do if API.inventory.SlotItem(WINDOW, SLOT_IDS[i]) then n = n + 1 end end
  return n
end

local FLASH = 1.2 -- seconds the frame flashes when the partner accepts
local theirSince = nil -- ui.GetTime() when theirReady last turned on

-- A small filled tag after the cursor, on the title line.
local function chip(text, c, s)
  local x, y = ui.GetCursorScreenPos()
  local tw, th = T.TextSize(text, T.font.tiny)
  local pw, ph = 6 * s, 2 * s
  local _, lh = ui.CalcTextSize("Ag")
  local oy = math.max(0, (lh - th - 2 * ph) / 2)
  ui.DrawRectFilled(x, y + oy, x + tw + 2 * pw, y + oy + th + 2 * ph, c[1], c[2], c[3], 0.22, 8 * s)
  T.Text(x + pw, y + oy + ph, text, c, T.font.tiny)
  ui.Dummy(tw + 2 * pw, lh)
end

-- the partner's coin (TRDW_HisMoney0..3), read into one table each frame (TC.Amount reads it and keeps nothing)
local HIS_MONEY = { "TRDW_HisMoney0", "TRDW_HisMoney1", "TRDW_HisMoney2", "TRDW_HisMoney3" }
local hisCoins = { 0, 0, 0, 0 }
local function theirCoins()
  for i = 1, 4 do hisCoins[i] = tonumber(W.Text(WINDOW, HIS_MONEY[i])) or 0 end
  return hisCoins
end

-- One side's card: a title, the slots, the coin. mine: your side (the coin piles take clicks). ready: that side
-- has accepted (the card is framed and tagged); flash: 0..1, how bright the partner's fresh accept still is.
local function offer(title, who, first, last, mine, s, width, ready, flash)
  -- the card sits inset by m whether or not it is framed, so accepting moves nothing
  local m = 5 * s
  local x, y = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x + m, y + m)
  width = width - 2 * m
  ui.BeginGroup()
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, who ~= "" and who or title)
  ui.PopFont()
  ui.SameLine()
  local n = filled(first, last)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, n == 0 and title or string.format("%s  \xC2\xB7  %d item%s",
    title, n, n == 1 and "" or "s"))
  if ready then
    ui.SameLine()
    chip("Accepted", C.good, s)
  end
  ui.Dummy(0, 2 * s)
  -- the slots: four across when they fit (the stock window's rows), else as many as fit
  local gap = 6 * s
  local size = math.min(46 * s, math.max(34 * s, (width - 3 * gap) / 4))
  local perRow = math.max(1, math.floor((width + gap) / (size + gap)))
  if perRow >= 4 then perRow = 4 * math.max(1, math.floor(perRow / 4)) end
  W.SlotGrid(WINDOW, "TRDW_TradeSlot", first, last, size, perRow, gap)
  ui.Dummy(0, 4 * s)
  if mine then
    TC.MoneyButtons(WINDOW, MY_MONEY, s,
      "Pick up coin in your inventory, then click here to offer it", width)
  else
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Coin")
    ui.SameLine()
    TC.Amount(theirCoins())
  end
  ui.EndGroup()
  local _, y1 = ui.GetCursorScreenPos()
  y1 = y1 + m
  if ready then
    -- a border only: a fill drawn now would lie over the item icons
    local g = C.good
    local th = (1.5 + 1.5 * flash) * s
    ui.DrawRect(x + th / 2, y + th / 2, x + width + 2 * m - th / 2, y1 - th / 2, g[1], g[2], g[3],
      0.7 + 0.3 * flash, 8 * s, th)
  end
  ui.SetCursorScreenPos(x, y1)
  ui.Dummy(0, 0)
  return x, y, y1
end

-- the buttons before and after we accept (made once, not every frame)
local CANCEL = { "TRDW_Cancel_Button", "Cancel", { tooltip = "End the trade; your items come back" } }
local BUTTONS = {
  [false] = { { "TRDW_Trade_Button", "Trade", { primary = true,
    tooltip = "Accept the trade as it stands; it completes once both of you have accepted" } }, CANCEL },
  [true] = { { "TRDW_Trade_Button", "Accepted", { primary = true,
    tooltip = "You have accepted; a change to either offer clears it" } }, CANCEL },
}

local OPTS = {
  size = function() return 470, 292 end,
  resizable = { 240, 290 },
}
local function body(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local partner = W.Text(WINDOW, "TRDW_HisName")
  local me = W.Text(WINDOW, "TRDW_MyName")
  local t = API.trade.State and API.trade.State() or {}
  local mine, theirs = t.myReady == true, t.theirReady == true
  local now = ui.GetTime()
  if theirs then theirSince = theirSince or now else theirSince = nil end
  local flash = theirSince and math.max(0, 1 - (now - theirSince) / FLASH) or 0
  if flash > 0 then flash = flash * (0.5 + 0.5 * math.cos((now - theirSince) * 4 * math.pi)) end
  local who = partner ~= "" and partner or "They"
  local status, sc
  if mine and theirs then status, sc = "Both accepted.", C.good
  elseif theirs then status, sc = who .. " has accepted. Click Trade to complete it.", C.good
  elseif mine then status, sc = "You accepted. Waiting for " .. (partner ~= "" and partner or "them") .. "...", C.text
  else status, sc = "Neither of you has accepted yet.", C.muted end
  ui.BeginChild("##offers", 0, -(btnH + lh + 14 * s), false)
  local cw = ui.GetContentRegionAvail()
  local gap = 16 * s
  local wide = cw >= 2 * (4 * 34 * s + 3 * 6 * s) + gap
  local colW = wide and (cw - gap) / 2 or cw
  -- theirs
  local x0, y0 = ui.GetCursorScreenPos()
  offer("Their offer", partner, 8, 15, false, s, colW, theirs, flash)
  if wide then
    ui.DrawLine(x0 + colW + gap / 2, y0, x0 + colW + gap / 2, select(2, ui.GetCursorScreenPos()), C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
    ui.SetCursorScreenPos(x0 + colW + gap, y0)
  else
    ui.Dummy(0, 4 * s)
    local sx, sy = ui.GetCursorScreenPos()
    ui.DrawLine(sx, sy, sx + cw, sy, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
    ui.Dummy(0, 6 * s)
  end
  offer("Your offer", me, 0, 7, true, s, colW, mine, 0)
  ui.EndChild()
  ui.Separator()
  ui.TextColored(sc[1], sc[2], sc[3], 1, status)
  W.ButtonRow(WINDOW, BUTTONS[mine], btnH)
end

ui.window("Trade", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Trade", OPTS, body)
end)
