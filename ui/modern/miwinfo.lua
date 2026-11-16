-- Mercenary info: replaces the game's MIW_InfoWnd (the Info button of the mercenary manager). Your mercenary's
-- purchase and upkeep as coins, its description and its current stance's description, from the original's children.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local WINDOW = "MIW_InfoWnd"

local OPTS = {
  size = function() return 320, 300 end,
  resizable = { 240, 200 },
}
local function body(x, y, w, h, s)
  local lh = M.Heights(s)
  M.BeginPanel("##miwcost", 0, lh * 2 + 20 * s, s)
  M.MercCosts(WINDOW, s)
  M.EndPanel()
  local rest = h - (lh * 2 + 20 * s) - lh - 12 * s
  M.BeginPanel("##miwinfo", 0, rest * 0.6, s)
  M.StmlOr(WINDOW, "InfoText", "No mercenary.")
  M.EndPanel()
  M.Caption("Stance")
  M.BeginPanel("##miwstance", 0, 0, s)
  M.StmlOr(WINDOW, "StanceInfoText", "")
  M.EndPanel()
end

ui.window("Mercenary Info", function()
  if not API.session.InGame() then return end
  M.TestOpen(WINDOW)
  hud.Dialog(WINDOW, "Mercenary Info", OPTS, body)
end)
