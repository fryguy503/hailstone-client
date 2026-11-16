-- Barter search: replaces the game's BarterSearchWnd (/barter: find buyers for your items). Your sellable items
-- down the left (a click puts the item's name in the search), the search with the buyers to ask (all or this
-- zone's), the matching buy lines as a sortable table with a filter (item, how many wanted, the price each as coins,
-- the buyer), what the selected line gives besides coins, Sell and Find Buyer.
-- Every list, field and button is the original's child: Sell is the selected line's own Sell button (the list's
-- column 5, as CBarterSearchWnd puts one on each line), which asks how many through the game's amount dialog; Find
-- Buyer gives the path to a buyer in this zone; Greeting asks the buyer's welcome text, Welcome the hall's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "BarterSearchWnd"

local SIDE_W = 230

local INV_COLS = {
  { "Your items", 0, cell = M.IconName(2), default = "asc" },
  { "Qty", 40, cell = M.Number(3), sort = function(r) return tonumber(r.texts[3]) or 0 end },
}
local function price(r) return M.ParseCoins(r.texts[4]) or 0 end
local LINE_COLS = {
  { "Wanted", 0, cell = M.IconName(2), default = "asc" },
  { "Qty", 40, cell = M.Number(3), sort = function(r) return tonumber(r.texts[3]) or 0 end, desc = true },
  { "Pays each", 96, cell = M.PriceCell(price), sort = price, desc = true },
  { "Buyer", 110, text = 5 },
}
local DETAIL_COLS = {
  { "Item", 0, cell = M.IconName(2) },
  { "Qty", 40, cell = M.Number(3), sort = function(r) return tonumber(r.texts[3]) or 0 end },
}

local searchActive = false
local searched = false
local lastDrawn = 0

local function search()
  API.windows.ClickChild(WINDOW, "SearchButton", "left")
  M.Refresh(WINDOW, "BuyLineList")
  searched = true
end

local WINDOW_OPTS = {
  size = function() return 780, 480 end,
  resizable = { 560, 340 },
}
local function drawWindow(x, y, w, h, s)
  lastDrawn = ui.GetTime()
  local lh, btnH = M.Heights(s)
  local sideW = SIDE_W * s

  -- ---------------------------------------------------------------- your items
  M.BeginPanel("##btsinv", sideW, h, s)
  local needle = M.Search("btsinv", "Filter your items")
  M.Table(WINDOW, "InventoryList", 0, -(lh + 6 * s), INV_COLS, {
    icon = true, key = "bts.inv",
    filter = needle ~= "" and function(r) return (r.texts[2] or ""):lower():find(needle, 1, true) ~= nil end or nil,
    filterKey = needle,
    empty = "Nothing a buyer could take.",
    emptyFiltered = "No item matches.",
    tooltip = function(r) return "Search buyers for " .. (r.texts[2] or "") end,
  })
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Click an item to search for it")
  M.EndPanel()

  -- ---------------------------------------------------------------- the search and its buy lines
  ui.SameLine(0, 8 * s)
  ui.BeginGroup()
  local rw = w - sideW - 8 * s
  local comboW = 130 * s
  local bw = 90 * s
  W.Edit(WINDOW, "ItemSearchEdit", { hint = "Item to sell", width = rw - comboW - bw - 16 })
  local active = ui.IsItemActive()
  if searchActive and not active and ui.IsKeyPressed(ui.Key.Enter) then search() end
  searchActive = active
  ui.SameLine()
  W.Combo(WINDOW, "PlayerCombo", { width = comboW })
  M.Tip("Ask every buyer, or only those in this zone")
  ui.SameLine()
  if W.Button(WINDOW, "SearchButton", "Search", { primary = true, width = bw }) then
    M.Refresh(WINDOW, "BuyLineList")
    searched = true
  end
  local detailH = math.max(70 * s, h * 0.22)
  local _, _, sel = M.Table(WINDOW, "BuyLineList", rw, h - btnH - detailH - lh * 2 - btnH - 26 * s, LINE_COLS, {
    icon = true, key = "bts.lines",
    empty = searched and "No buyer wants that." or "Search for an item to see who is buying it.",
    tooltip = function(r) return (r.texts[5] or "?") .. " pays " .. (r.texts[4] or "?") .. " each" end,
  })
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, sel and "Also gives, for each sale" or "Select a buy line")
  M.Table(WINDOW, "DetailsList", rw, detailH, DETAIL_COLS, {
    icon = true, key = "bts.detail",
    empty = sel and "Coins only." or "",
  })
  ui.Dummy(0, 2 * s)
  local bw4 = (ui.GetContentRegionAvail() - 3 * 8) / 4
  ui.PushStyleColor(ui.Col.Button, C.accent[1], C.accent[2], C.accent[3], 0.24)
  ui.BeginDisabled(sel == nil)
  if ui.Button("Sell##btssell", bw4, btnH) and sel then API.windows.ListClick(WINDOW, "BuyLineList", sel.index, "left", 5) end
  ui.EndDisabled()
  ui.PopStyleColor(1)
  if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
    ui.SetTooltip(sel and "Sell to the selected buy line (asks how many)" or "Select a buy line first")
  end
  ui.SameLine()
  W.Button(WINDOW, "FindButton", "Find Buyer", { width = bw4, height = btnH, tooltip = "A path to the buyer (when in this zone)" })
  ui.SameLine()
  W.Button(WINDOW, "GreetingButton", "Greeting", { width = bw4, height = btnH, tooltip = "The buyer's welcome text" })
  ui.SameLine()
  W.Button(WINDOW, "WelcomeButton", "Welcome", { width = bw4, height = btnH, tooltip = "The barter hall's welcome" })
  W.Check(WINDOW, "HideButton", "Hide other buyers", { tooltip = "Hide every buyer but the one Find Buyer showed the way to" })
  ui.EndGroup()
end

ui.window("Barter Search", function()
  if not API.session.InGame() then return end
  if ui.GetTime() - lastDrawn > 1 then searched = false end
  hud.Dialog(WINDOW, "Barter Search", WINDOW_OPTS, drawWindow)
end)
