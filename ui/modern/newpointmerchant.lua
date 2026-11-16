-- Adventure merchant: replaces the game's NewPointMerchantWnd (adventure points, PvP points or crystals): the
-- merchant and how much of its currency you hold, the goods as a sortable table with a search (cost, and the
-- adventure theme a good belongs to), a cost above what you hold in red, Purchase, Sell (the item on your cursor;
-- its worth on hover) and Done (lib/markets_points.lua). The list and buttons are the original's children.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local P = dofile("lib/markets_points.lua")
local WINDOW = "NewPointMerchantWnd"

local CFG = {
  window = WINDOW, prefix = "NewPointMerchant_",
  currency = function() return W.Text(WINDOW, "NewPointMerchant_PointsNameLabel") end,
  columns = { { "Theme", 100, cell = M.TextCell(3, hud.T.color.muted) } },
}

local WINDOW_OPTS = {
  size = function() return 480, 460 end,
  resizable = { 360, 300 },
}
local function drawWindow(x, y, w, h, s) P.Draw(CFG, w, h, s) end

ui.window("Adventure Merchant", function()
  if not API.session.InGame() then return end
  WINDOW_OPTS.shown = M.Forced(WINDOW)
  hud.Dialog(WINDOW, "Merchant", WINDOW_OPTS, drawWindow)
end)
