-- Barter merchant: replaces the game's BarterMerchantWnd (a buyer's window, opened on a buyer in the barter hall).
-- The buyer's name, the buy lines as a sortable table with a filter (item, how many wanted, the price each as
-- coins), what the selected line gives besides coins, and Sell. The lists and Sell are the original's children: a
-- row click selects the line in the game's window, Sell is enabled only when you carry the item and asks how many
-- through the game's amount dialog.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "BarterMerchantWnd"

local LINE_COLS = {
  { "Wants", 0, cell = M.IconName(2), default = "asc" },
  { "Qty", 40, cell = M.Number(3), sort = function(r) return tonumber(r.texts[3]) or 0 end, desc = true },
  { "Pays each", 96, cell = M.PriceCell(function(r) return M.ParseCoins(r.texts[4]) end),
    sort = function(r) return M.ParseCoins(r.texts[4]) or 0 end, desc = true },
}
local DETAIL_COLS = {
  { "Item", 0, cell = M.IconName(2) },
  { "Qty", 40, cell = M.Number(3), sort = function(r) return tonumber(r.texts[3]) or 0 end },
}

local WINDOW_OPTS = {
  size = function() return 400, 480 end,
  resizable = { 300, 320 },
}
local function drawWindow(x, y, w, h, s)
  local lh, btnH = M.Heights(s)
  local name = W.Text(WINDOW, "MerchantNameLabel")
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, name ~= "" and name or "Buyer")
  ui.PopFont()
  ui.SameLine()
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "is buying")
  local needle = M.Search("btrm", "Filter buy lines")
  local detailH = math.max(70 * s, h * 0.24)
  local _, _, sel = M.Table(WINDOW, "BuyLineList", 0, -(detailH + lh + btnH + 18 * s), LINE_COLS, {
    icon = true, key = "btrm.lines",
    filter = needle ~= "" and function(r) return (r.texts[2] or ""):lower():find(needle, 1, true) ~= nil end or nil,
    filterKey = needle,
    empty = "This buyer has no buy lines.",
    emptyFiltered = "No buy line matches.",
  })
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, sel and "Also gives, for each sale" or "Select a buy line")
  M.Table(WINDOW, "DetailList", 0, detailH, DETAIL_COLS, {
    icon = true, key = "btrm.detail",
    empty = sel and "Coins only." or "",
  })
  ui.Dummy(0, 2 * s)
  local can = W.State(WINDOW, "SellButton")
  W.Button(WINDOW, "SellButton", "Sell", { primary = true, width = -1, height = btnH,
    tooltip = can and "Sell to this buy line (asks how many)"
      or (sel and "Needs the item in your inventory" or "Select a buy line first") })
end

ui.window("Barter Merchant", function()
  if not API.session.InGame() then return end
  WINDOW_OPTS.shown = M.Forced(WINDOW)
  hud.Dialog(WINDOW, "Buyer", WINDOW_OPTS, drawWindow)
end)
