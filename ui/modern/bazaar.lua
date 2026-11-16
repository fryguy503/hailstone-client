-- Bazaar trader: replaces the game's BazaarWnd (the trader's own window). The wares (the Trader's Satchels' items)
-- as tiles that reflow to the window's width, beside them the selected item's price as four coin buttons with Set
-- Price and Clear Price, the trader buttons (Begin / End Trader, Add, Remove, List Items) with the trading state as a
-- chip, and the window's activity text below.
-- The tiles are the game's slots (a click selects the item, a right click inspects it), each coin button is the
-- original's money button (it opens the game's amount dialog), every other button the original's: the rules and
-- messages are the game's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "BazaarWnd"
local SLOTS = 200
local SIDE_W = 250

local GREEN = C.pet -- (the palette's green)

-- The selected ware's slot id and item (the slot the game marks checked).
local function selectedWare()
  for i = 0, SLOTS - 1 do
    local id = "BZR_BazaarSlot" .. i
    local _, checked = W.State(WINDOW, id)
    if checked then return id, API.inventory.SlotItem(WINDOW, id) end
  end
  return nil, nil
end

local function chip(text, color, s)
  local x, y = ui.GetCursorScreenPos()
  local tw, th = ui.CalcTextSize(text)
  local px, py = 8 * s, 2 * s
  ui.DrawRectFilled(x, y, x + tw + 2 * px, y + th + 2 * py, color[1], color[2], color[3], 0.16, 9 * s)
  ui.DrawRect(x, y, x + tw + 2 * px, y + th + 2 * py, color[1], color[2], color[3], 0.55, 9 * s, 1)
  ui.DrawText(x + px, y + py, color[1], color[2], color[3], 1, text)
  ui.Dummy(tw + 2 * px, th + 2 * py)
end

local WINDOW_OPTS = {
  size = function() return 640, 440 end,
  resizable = { 480, 320 },
}
local function drawWindow(x, y, w, h, s)
  local lh, btnH = M.Heights(s)
  local trading = W.Shown(WINDOW, "BZW_End_Button")

  -- ---------------------------------------------------------------- header: the state and Begin / End Trader
  if trading then chip("Trading", GREEN, s) else chip("Not trading", C.muted, s) end
  ui.SameLine()
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1,
    trading and "Buyers can see the wares you have priced." or "Price your wares, then begin trading.")
  local bw = 130 * s
  ui.SameLine(math.max(0, w - bw))
  if trading then
    W.Button(WINDOW, "BZW_End_Button", "End Trader", { width = bw, danger = true, tooltip = "Stop trading" })
  else
    W.Button(WINDOW, "BZW_Start_Button", "Begin Trader", { width = bw, primary = true,
      tooltip = "Start trading: buyers see every priced ware" })
  end
  ui.Dummy(0, 2 * s)

  local logH = math.max(70 * s, h * 0.24)
  local topH = h - logH - (lh + btnH) - 8 * s
  local sideW = SIDE_W * s

  -- ---------------------------------------------------------------- the wares
  M.BeginPanel("##bzwares", w - sideW - 8 * s, topH, s)
  local size = 44 * s
  local gap = 6 * s
  local avail = ui.GetContentRegionAvail()
  local perRow = math.max(1, math.floor((avail + gap) / (size + gap)))
  local n = 0
  ui.BeginChild("##bzwaresscroll", 0, 0, false)
  for i = 0, SLOTS - 1 do
    local id = "BZR_BazaarSlot" .. i
    if API.inventory.SlotItem(WINDOW, id) then
      if n % perRow ~= 0 then ui.SameLine(0, gap) end
      W.Slot(WINDOW, id, size, nil, "Trader's Satchel")
      n = n + 1
    end
  end
  if n == 0 then
    M.Empty("Nothing to sell: put items in a Trader's Satchel.", 60 * s)
  end
  ui.EndChild()
  M.EndPanel()

  -- ---------------------------------------------------------------- the selected ware's price
  ui.SameLine(0, 8 * s)
  M.BeginPanel("##bzprice", sideW, topH, s)
  local _, item = selectedWare()
  W.Heading("Price")
  if item then
    ui.PushTextWrapPos(0)
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, item.name or "")
    ui.PopTextWrapPos()
    if item.stackable and (item.count or 0) > 1 then
      ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, item.count .. " in the stack (the price is for each)")
    end
  else
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Select a ware to price it.")
  end
  ui.Dummy(0, 2 * s)
  -- the four coin buttons, two a line
  local half = (ui.GetContentRegionAvail() - 8) / 2
  for i = 0, 3 do
    if i % 2 == 1 then ui.SameLine() end
    local id = "BZW_Money" .. i
    local v = tonumber(W.Text(WINDOW, id)) or 0
    local cc = M.COIN_COLOR[i + 1]
    ui.PushStyleColor(ui.Col.Text, cc[1], cc[2], cc[3], 1)
    W.Button(WINDOW, id, T.Thousands(v) .. " " .. M.COIN_NAME[i + 1]:sub(1, 1):lower() .. "p",
      { width = half, tooltip = M.COIN_NAME[i + 1] .. ": click to enter the amount" })
    ui.PopStyleColor(1)
  end
  ui.Dummy(0, 2 * s)
  W.ButtonRow(WINDOW, {
    { "BZW_SetPrice_Button", "Set Price", { primary = true, tooltip = "Keep this price for the selected ware" } },
    { "BZW_Clear_Button", "Clear", { tooltip = "Clear the selected ware's price" } },
  })
  W.Heading("Trading")
  W.ButtonRow(WINDOW, {
    { "BZW_Add_Button", "Add", { tooltip = "Offer the selected ware now (while trading)" } },
    { "BZW_Remove_Button", "Remove", { tooltip = "Stop offering the selected ware (while trading)" } },
  })
  W.ButtonRow(WINDOW, {
    { "BZW_List_Button", "List Items", { tooltip = "List what you are offering" } },
    { "BZW_OfflineMode_Button", "Offline Mode", { tooltip = "Keep trading after you log out (while trading)" } },
  })
  M.EndPanel()

  -- ---------------------------------------------------------------- the activity text
  ui.Dummy(0, 2 * s)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Activity")
  M.BeginPanel("##bzlog", 0, 0, s)
  W.Stml(WINDOW, "BZW_BazaarText", 0, 0)
  M.EndPanel()
end

ui.window("Bazaar Trader", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Bazaar Trader", WINDOW_OPTS, drawWindow)
end)
