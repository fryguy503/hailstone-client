-- Point merchant: replaces the game's PointMerchantWnd (a merchant who sells for an alternate currency): the
-- merchant and how much of the currency you hold (with its icon), the goods as a sortable table with a search, a
-- cost above what you hold in red, Purchase, Sell (the item on your cursor; its offer on hover) and Done
-- (lib/markets_points.lua). The list and buttons are the original's children.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local M = dofile("lib/markets.lua")
local P = dofile("lib/markets_points.lua")
local WINDOW = "PointMerchantWnd"

-- The merchant's currency: the point list's one row names it ("Currency <n>", as hs.AltCurrencies does).
local function currency()
  local row = API.windows.ListRow(WINDOW, "PointMerchant_PointList", 0)
  return row and row[1] or ""
end
-- The currency's icon number, by its name: found once in hs.AltCurrencies (a new list a call) rather than each frame;
-- a name not found yet is looked for again at most once a second. The texture itself is asked for each frame.
local iconOf, lookedAt = {}, {}
local function icon()
  local name = currency()
  local n = iconOf[name]
  if not n then
    local now = ui.GetTime()
    if lookedAt[name] and now >= lookedAt[name] and now - lookedAt[name] < 1 then return nil end
    lookedAt[name] = now
    for _, c in ipairs(API.character.AltCurrencies(true)) do
      if c.name == name and c.icon and c.icon > 0 then n = c.icon; break end
    end
    if not n then return nil end
    iconOf[name] = n
  end
  return API.assets.ItemIcon(n)
end

local CFG = { window = WINDOW, prefix = "PointMerchant_", currency = currency, icon = icon }

local OPTS = {
  size = function() return 420, 460 end,
  resizable = { 320, 300 },
}
local function body(x, y, w, h, s) P.Draw(CFG, w, h, s) end

ui.window("Point Merchant", function()
  if not API.session.InGame() then return end
  OPTS.shown = M.Forced(WINDOW)
  hud.Dialog(WINDOW, "Point Merchant", OPTS, body)
end)
