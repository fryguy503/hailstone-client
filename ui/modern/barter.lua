-- Barter buyer: replaces the game's BarterWnd (/buyer: the player's own buy lines). Three columns read left to
-- right as the steps of a buy line: the item to buy (a search and its results), the offer (the price each as four
-- coin buttons, how many, and up to ten of your items given as well), and the buy lines with the status text and
-- the welcome message. The buyer state (a chip) and Start / Stop Buyer head the window.
-- Every list, field and button is the original's child (hs.ListClick, hs.SetWindowText, hs.ClickChild): the coin
-- buttons open the game's amount dialog, a buy line's click puts its values back into the offer to change them.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "BarterWnd"

local GREEN = C.pet -- (the palette's green)
local COINS = { "CompensationPlat", "CompensationGold", "CompensationSilver", "CompensationCopper" }
local ICON_ITEM = { { "Item", 0, cell = M.IconName(2), default = "asc" } }
local ICON_QTY = { { "Item", 0, cell = M.IconName(2), default = "asc" },
  { "Qty", 40, cell = M.Number(3), sort = function(r) return tonumber(r.texts[3]) or 0 end } }

local function chip(text, color, s)
  local x, y = ui.GetCursorScreenPos()
  local tw, th = ui.CalcTextSize(text)
  local px, py = 8 * s, 2 * s
  ui.DrawRectFilled(x, y, x + tw + 2 * px, y + th + 2 * py, color[1], color[2], color[3], 0.16, 9 * s)
  ui.DrawRect(x, y, x + tw + 2 * px, y + th + 2 * py, color[1], color[2], color[3], 0.55, 9 * s, 1)
  ui.DrawText(x + px, y + py, color[1], color[2], color[3], 1, text)
  ui.Dummy(tw + 2 * px, th + 2 * py)
end

local function step(n, text)
  ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, n)
  ui.SameLine(0, 6)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, text)
end

-- An edit and a button on one line.
local function editButton(edit, hint, button, label, tip)
  local bw = ui.CalcTextSize(label) + 24
  W.Edit(WINDOW, edit, { hint = hint, width = ui.GetContentRegionAvail() - bw - 8 })
  ui.SameLine()
  W.Button(WINDOW, button, label, { width = bw, tooltip = tip })
end

local function itemColumn(cw, ch, s, lh, btnH)
  M.BeginPanel("##btritem", cw, ch, s)
  step("1", "Item to buy")
  editButton("ItemSearchEdit", "Item name", "ItemSearchButton", "Search", "Find items by name")
  local _, _, sel = M.Table(WINDOW, "ItemSearchList", 0, -(lh + 6 * s), ICON_ITEM, {
    icon = true, key = "btr.search",
    empty = "Search for the item you want to buy.",
  })
  if sel then
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Buying:")
    ui.SameLine()
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, sel.texts[2] or "")
  else
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Pick the item to buy.")
  end
  M.EndPanel()
end

local function offerColumn(cw, ch, s, lh, btnH)
  M.BeginPanel("##btroffer", cw, ch, s)
  step("2", "Your offer")
  M.Caption("Price each")
  local q = (ui.GetContentRegionAvail() - 3 * 4) / 4
  for i, id in ipairs(COINS) do
    if i > 1 then ui.SameLine(0, 4) end
    local cc = M.COIN_COLOR[i]
    ui.PushStyleColor(ui.Col.Text, cc[1], cc[2], cc[3], 1)
    W.Button(WINDOW, id, T.Thousands(tonumber(W.Text(WINDOW, id)) or 0) .. M.COIN_NAME[i]:sub(1, 1):lower(),
      { width = q, tooltip = M.COIN_NAME[i] .. ": click to enter the amount" })
    ui.PopStyleColor(1)
  end
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "How many")
  ui.SameLine()
  W.Edit(WINDOW, "CountEdit", { hint = "1" })
  M.Caption("Also give (up to 10 of your items)")
  -- the two lists share what is left above the Create / Modify row
  local _, availH = ui.GetContentRegionAvail()
  local listH = math.max(50 * s, (availH - btnH * 2 - 30 * s) / 2)
  M.Table(WINDOW, "InventoryList", 0, listH, ICON_QTY, { icon = true, key = "btr.inv", empty = "No tradeable items." })
  W.ButtonRow(WINDOW, {
    { "InventoryAddButton", "Add", { tooltip = "Give one of the selected item with each sale" } },
    { "InventoryRemoveButton", "Remove", { tooltip = "Take the selected item out of the offer" } },
  })
  M.Table(WINDOW, "CompensationList", 0, listH, ICON_QTY, { icon = true, key = "btr.comp", empty = "Coins only." })
  ui.Dummy(0, 2 * s)
  W.ButtonRow(WINDOW, {
    { "BuyLineAddButton", "Create Buy Line", { primary = true, tooltip = "A new buy line with this item and offer" } },
    { "BuyLineModifyButton", "Modify", { tooltip = "Change the selected buy line to this offer" } },
  }, btnH)
  M.EndPanel()
end

-- CBarterWnd's BuyLineList: On ("X" when active), icon, Item Name, Cnt, Offering (the price, "+ items" with
-- compensation).
local LINE_COLS = {
  { "Item", 0, cell = M.IconName(3), default = "asc" },
  { "Qty", 36, cell = M.Number(4), sort = function(r) return tonumber(r.texts[4]) or 0 end },
  { "Price", 86, cell = M.PriceCell(function(r) return M.ParseCoins(r.texts[5]) end),
    sort = function(r) return M.ParseCoins(r.texts[5]) or 0 end },
  { "On", 34, cell = function(r, c)
      local on = r.texts[1] == "X"
      local col = on and GREEN or C.faint
      ui.DrawText(c.x, c.y + (c.h - c.textH) / 2, col[1], col[2], col[3], 1, on and "On" or "Off")
    end },
}

local function linesColumn(cw, ch, s, lh, btnH)
  M.BeginPanel("##btrlines", cw, ch, s)
  step("3", "Buy lines")
  local statusH = math.max(60 * s, ch * 0.28)
  M.Table(WINDOW, "BuyLineList", 0, -(btnH + statusH + lh * 2 + btnH * 2 + 30 * s), LINE_COLS, {
    icon = true, key = "btr.lines",
    empty = "No buy lines yet.",
    tooltip = function() return "Click to load this line into the offer (then Modify)" end,
  })
  W.ButtonRow(WINDOW, {
    { "BuyLineRemoveButton", "Remove", { tooltip = "Delete the selected buy line" } },
    { "BuyLineActiveButton", "Turn On/Off", { tooltip = "Pause or resume the selected buy line" } },
  })
  M.Caption("Status")
  M.BeginPanel("##btrstatus", 0, statusH, s)
  W.Stml(WINDOW, "StatusText", 0, 0)
  M.EndPanel()
  W.ButtonRow(WINDOW, {
    { "ListLinesButton", "List Lines", { tooltip = "List your buy lines in chat" } },
    { "ClearStatusButton", "Clear", { tooltip = "Clear the status text" } },
  })
  M.Caption("Welcome message")
  editButton("WelcomeEdit", "Shown to sellers", "UpdateWelcomeButton", "Update", "Set the message sellers see")
  M.EndPanel()
end

local WINDOW_OPTS = {
  size = function() return 900, 500 end,
  resizable = { 700, 400 },
}
local function drawWindow(x, y, w, h, s)
  local lh, btnH = M.Heights(s)
  local buying = W.Text(WINDOW, "StartStopButton"):find("Stop") ~= nil
  if buying then chip("Buying", GREEN, s) else chip("Not buying", C.muted, s) end
  ui.SameLine()
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1,
    buying and "Sellers can fill your active buy lines." or "Set up buy lines, then start buying.")
  local bw = 130 * s
  ui.SameLine(math.max(0, w - 2 * bw - 8 * s))
  W.Button(WINDOW, "OfflineModeButton", "Offline Mode",
    { width = bw, tooltip = "Keep buying after you log out (while buying)" })
  ui.SameLine()
  W.Button(WINDOW, "StartStopButton", buying and "Stop Barter" or "Start Barter",
    { width = bw, primary = not buying, danger = buying })
  ui.Dummy(0, 2 * s)
  local _, top = ui.GetCursorScreenPos()
  local ch = h - (top - y)
  local gap = 8 * s
  local c1 = math.floor((w - 2 * gap) * 0.30)
  local c2 = math.floor((w - 2 * gap) * 0.33)
  local c3 = w - 2 * gap - c1 - c2
  itemColumn(c1, ch, s, lh, btnH)
  ui.SameLine(0, gap)
  offerColumn(c2, ch, s, lh, btnH)
  ui.SameLine(0, gap)
  linesColumn(c3, ch, s, lh, btnH)
end

ui.window("Barter Buyer", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Barter Buyer", WINDOW_OPTS, drawWindow)
end)
