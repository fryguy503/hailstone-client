-- Point merchants (gamepad pack): stand in for PointMerchantWnd (a merchant who sells for an alternate currency)
-- and NewPointMerchantWnd (adventure points, PvP points or crystals). Two pages, LB / RB:
--   Buy   the merchant's goods (the cost, a cost above what you hold in red, an adventure good's theme): focusing
--         one selects it, A buys it (the stock Purchase button)
--   Sell  your bags: A picks an item up (the game's slot logic), then Sell offers the held item to the merchant
--         (what it is worth is the Sell button's own text)
-- How much of the currency you hold heads the window; Done (or B) closes it as the stock Done button does.
-- The list and the buttons are the stock window's (hs.ListClick, hs.ClickChild).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local X = dofile("lib/exchange.lua")
local M = dofile("lib/markets.lua")
local C = T.C

local function cost(r) return tonumber(((r.texts[2] or ""):gsub(",", ""))) or 0 end

local function thousands(n)
  local s = tostring(math.floor(n))
  return s:reverse():gsub("(%d%d%d)", "%1,"):reverse():gsub("^,", "")
end

-- One merchant window: cfg = { window, name (ImGui), prefix, currency() -> name, theme (a third column) }.
local function merchant(cfg)
  local win, pre = cfg.window, cfg.prefix
  local page = 1
  local goods = {}
  local nav, bags = {}, {}
  ui.window(cfg.name, function()
    if not M.visible(win) then return end
    M.frame()
    local ctx = X.frame(nav, {})
    local have = tonumber(((API.windows.Text(win, pre .. "PointsAvailableValue") or ""):gsub(",", "")))
    local label = cfg.currency()
    if label == "" then label = "points" end
    local name = API.windows.Text(win, pre .. "NameLabel") or ""
    local hints = page == 1 and { { "a", "Buy" }, { "y", "Inspect" }, { "lb", "Buy" }, { "rb", "Sell" }, { "b", "Done" } }
      or { { "a", "Pick up / put down" }, { "x", "Put away" }, { "lb", "Buy" }, { "rb", "Sell" }, { "b", "Done" } }
    if W.begin(cfg.name, { w = 1500, h = 920, title = name ~= "" and name or "Merchant",
          subtitle = "You hold " .. (have and thousands(have) or "?") .. " " .. label, hints = hints }) then
      if M.pressed(cfg.name, "lb") then page = 1 end
      if M.pressed(cfg.name, "rb") then page = 2 end
      local x0, y0, x1, y1 = W.area()
      local wx, wy = ui.GetWindowPos()
      local top = M.tabs({ "Buy", "Sell" }, page, x0, y0, function(i) page = i end)
      local rx0 = x1 - T.px(440)
      local bh = T.px(64)
      if page == 1 then
        local function short(r) return have and cost(r) > have end
        local _, sel = M.list(win, pre .. "ItemList", goods, x0, top, rx0 - T.px(24), y1, {
          rowH = 60, select = "focus", empty = "This merchant has nothing for sale.",
          draw = function(r, a, b, c, d)
            M.row(r, a, b, c, d, r.texts[1], thousands(cost(r)) .. " " .. label,
              { rightColor = short(r) and C.bad or C.endurance, sub = cfg.theme and r.texts[3] or nil, rightW = T.px(300) })
          end,
          onA = function() API.windows.ClickChild(win, pre .. "PurchaseButton", "left") end })
        -- the selected good
        T.rect(rx0, top, x1, y1 - 2 * bh - T.px(40), C.sunken, 1, T.px(14))
        local p = T.px(18)
        if sel then
          T.font(24)
          local _, lh = ui.CalcTextSize("Ag")
          T.draw(rx0 + p, top + p, T.fit(sel.texts[1] or "", x1 - rx0 - 2 * p))
          T.unfont()
          T.font(21)
          T.draw(rx0 + p, top + p + lh + T.px(10), thousands(cost(sel)) .. " " .. label, short(sel) and C.bad or C.endurance)
          if short(sel) then T.draw(rx0 + p, top + p + 2 * lh + T.px(20), "You need " .. thousands(cost(sel) - have) .. " more.", C.dim) end
          if cfg.theme and (sel.texts[3] or "") ~= "" then T.draw(rx0 + p, top + p + 3 * lh + T.px(30), "Theme: " .. sel.texts[3], C.dim) end
          T.unfont()
        else
          T.font(20)
          T.draw(rx0 + p, top + p, "Focus a good to select it.", C.faint)
          T.unfont()
        end
        ui.SetCursorPos(rx0 - wx, y1 - 2 * bh - T.px(10) - wy)
        M.buttons(win, { { pre .. "PurchaseButton", "Purchase", { primary = true } } }, rx0, x1, bh)
        M.buttons(win, { { pre .. "DoneButton", "Done" } }, rx0, x1, bh)
      else
        -- your bags: pick up the item to sell
        local per = math.max(1, math.floor((rx0 - T.px(24) - x0 + T.px(X.GAP)) / T.px(X.SIZE + X.GAP)))
        ui.SetCursorPos(x0 - wx, top - wy)
        X.bags(ctx, bags, x0, rx0 - T.px(24) - x0, per)
        X.holding(ctx, rx0, top, x1, "Sell offers it to the merchant")
        local hy = top + X.holdingHeight(ctx) + T.px(14)
        local offer = API.windows.ChildTooltip and API.windows.ChildTooltip(win, pre .. "SellButton") or ""
        T.font(20)
        T.draw(rx0, hy, ctx.held and (offer ~= "" and ("Offer: " .. offer) or "The merchant may not take it.")
          or "Pick up an item this merchant takes.", C.dim)
        T.unfont()
        ui.SetCursorPos(rx0 - wx, y1 - 2 * bh - T.px(10) - wy)
        M.buttons(win, { { pre .. "SellButton", "Sell", { primary = true } } }, rx0, x1, bh)
        M.buttons(win, { { pre .. "DoneButton", "Done" } }, rx0, x1, bh)
      end
    end
    if W.finish() then API.windows.ClickChild(win, pre .. "DoneButton", "left") end
  end)
end

-- The alternate currency merchant: the point list's one row names the currency.
merchant({ window = "PointMerchantWnd", name = "Point Merchant", prefix = "PointMerchant_",
  currency = function()
    local row = API.windows.ListRow("PointMerchantWnd", "PointMerchant_PointList", 0)
    return row and row[1] or ""
  end })

-- The adventure merchant: its points label names them; each good has a theme.
merchant({ window = "NewPointMerchantWnd", name = "Adventure Merchant", prefix = "NewPointMerchant_", theme = true,
  currency = function() return API.windows.Text("NewPointMerchantWnd", "NewPointMerchant_PointsNameLabel") or "" end })
