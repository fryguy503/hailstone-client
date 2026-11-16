-- Bazaar purchase confirmation: replaces the game's BazaarConfirmationWnd. The item (its icon, name and count),
-- the total as coins, and the two ways to take it as cards: straight to you for Expedient Delivery Vouchers (the
-- original's BZC_UseTokenButton, method 2: the server takes that many vouchers), or to your parcels for the
-- platinum fee the game asks (BZC_UsePlatButton, method 1); Cancel. The buttons are the original's (the purchase
-- goes out from its handler).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "BazaarConfirmationWnd"

-- "Item Price: 12p 3g" -> copper
local function total()
  return M.ParseCoins((W.Text(WINDOW, "BZC_TotalPrice"):gsub("^[^:]*:", "")))
end

local WINDOW_OPTS = {
  rollup = false, -- (a question to answer: kept whole)
  size = function() return 380, 230 end,
  resizable = { 320, 200 },
  keepTitle = true,
}
local function drawWindow(x, y, w, h, s)
  local lh, btnH = M.Heights(s)
  -- the item
  local icon = 44 * s
  local cx, cy = ui.GetCursorScreenPos()
  local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, "BZC_ItemIcon", "decal")
  T.Icon(cx, cy, icon, s, tex or nil, u0, v0, u1, v1)
  ui.SetCursorScreenPos(cx + icon + 10 * s, cy + 2 * s)
  ui.BeginGroup()
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, W.Text(WINDOW, "BZC_ItemLabel"))
  ui.PopFont()
  local cp = total()
  if cp then
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Total")
    ui.SameLine()
    M.Price(cp)
  else
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, W.Text(WINDOW, "BZC_TotalPrice"))
  end
  ui.EndGroup()
  ui.SetCursorScreenPos(cx, cy + icon + 8 * s)

  W.Heading("Delivery")
  -- two cards side by side: the button and what it costs
  local cardW = (ui.GetContentRegionAvail() - 8 * s) / 2
  local cardH = btnH + lh * 2 + 22 * s
  M.BeginPanel("##bzcme", cardW, cardH, s)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Into your inventory")
  -- "x 2": the vouchers it costs
  local vouchers = tonumber((W.Text(WINDOW, "BZC_TokenLabel"):match("%d+"))) or 0
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1,
    vouchers == 1 and "Costs 1 voucher" or ("Costs " .. vouchers .. " vouchers"))
  W.Button(WINDOW, "BZC_UseTokenButton", "To Me", { primary = true, width = -1, height = btnH,
    tooltip = "Pay with Expedient Delivery Vouchers to have the item delivered straight to you" })
  M.EndPanel()
  ui.SameLine(0, 8 * s)
  M.BeginPanel("##bzcparcel", cardW, cardH, s)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "To your parcels")
  local fee = M.ParseCoins(W.Text(WINDOW, "BZC_PlatLabel"))
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Fee")
  if fee then ui.SameLine(); M.Price(fee) end
  W.Button(WINDOW, "BZC_UsePlatButton", "To Parcels", { width = -1, height = btnH,
    tooltip = "Pay the platinum fee to have the item sent to your parcels (collect it at a parcel merchant)" })
  M.EndPanel()
  ui.Dummy(0, 2 * s)
  W.Button(WINDOW, "BZC_CancelButton", "Cancel", { width = -1, height = btnH })
end

ui.window("Bazaar Confirmation", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Confirm Purchase", WINDOW_OPTS, drawWindow)
end)
