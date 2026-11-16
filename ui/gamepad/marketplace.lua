-- Marketplace (gamepad pack): stands in for MarketplaceWnd (/marketplace, the EQ menu's Marketplace), the store a
-- server runs through the original client's marketplace packets. Three columns:
--   Categories   the game's tree as a list (A shows that shelf, X opens or closes a branch)
--   Items        the shelf: focusing an item selects it; on New and Featured the new and the featured items; the
--                search (A types it, then it searches) and the sort head the column
--   Item         the selected item: price, quantity (- / +), total and Buy, then the game's pages in turn: the zip
--                code, the confirmation, the result; Y inspects the item
-- Your funds head the window. The lists and buttons are the stock window's (app/lua_ui_marketplace.cpp).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/markets.lua")
local C = T.C
local WIN = "MarketplaceWnd"
local F = M.form()
local cats, items, news, feats = {}, {}, {}, {}

local function text(id) return API.windows.Text(WIN, id) or "" end
local function shown(id) return API.windows.ChildShown(WIN, id) ~= false end

local function find() API.windows.ClickChild(WIN, "MKPW_FindBtn", "left") end

local function itemRow(r, a, b, c, d, nameCol, priceCol)
  M.row(r, a, b, c, d, r.texts[nameCol], r.texts[priceCol], { rightColor = C.endurance, rightW = T.px(140) })
end

local function details(x0, y0, x1, y1)
  local wx, wy = ui.GetWindowPos()
  local p = T.px(18)
  local bh = T.px(60)
  T.rect(x0, y0, x1, y1, C.sunken, 1, T.px(14))
  if text("MKPW_DetailsName") == "" then
    T.font(20)
    T.draw(x0 + p, y0 + p, "Focus an item to select it.", C.faint)
    T.unfont()
    return
  end
  T.font(24)
  local _, lh = ui.CalcTextSize("Ag")
  T.draw(x0 + p, y0 + p, T.fit(text("MKPW_DetailsName"), x1 - x0 - 2 * p))
  T.unfont()
  local y = y0 + p + lh + T.px(8)
  T.font(19)
  local _, sh = ui.CalcTextSize("Ag")
  local function line(t, col)
    if t == "" then return end
    T.draw(x0 + p, y, T.fit(t, x1 - x0 - 2 * p), col or C.dim)
    y = y + sh + T.px(6)
  end
  if shown("MKPW_DetailsPlayerStudioLabel") then line(text("MKPW_DetailsPlayerStudioLabel")) end
  if shown("MKPW_DetailsAvailableTimeLeft_Label") then line("Available for " .. text("MKPW_DetailsAvailableTimeLeft_Label")) end
  if shown("MKPW_DetailsSaleTimeLeft_Label") then line("On sale for " .. text("MKPW_DetailsSaleTimeLeft_Label")) end
  local page = API.windows.TabPage(WIN, "MKPW_Subwindows") or 0
  if page == 0 then
    line("Price " .. text("MKPW_BuyPrice") .. (shown("MKPW_WasPrice") and ("  (was " .. text("MKPW_WasPrice") .. ")") or ""), C.endurance)
    line("Quantity " .. text("MKPW_QtyEdit") .. "    Total " .. text("MKPW_BuyTotal"), C.text)
    T.unfont()
    ui.SetCursorPos(x0 + p - wx, y1 - 3 * bh - T.px(30) - wy)
    M.buttons(WIN, { { "MKPW_QtyDownBtn", "-", always = true }, { "MKPW_QtyUpBtn", "+", always = true } }, x0 + p, x1 - p, bh)
    M.buttons(WIN, { { "MKPW_DetailsInspect", "Inspect" }, { "MKPW_DetailsPreview", "Preview" } }, x0 + p, x1 - p, bh)
    M.buttons(WIN, { { "MKPW_BuyBtn", "Buy", { primary = true }, always = true } }, x0 + p, x1 - p, bh)
    return
  end
  if page == 1 then
    line("Enter your 5 digit zip code to go on.", C.text)
    T.unfont()
    ui.SetCursorPos(x0 + p - wx, y - wy)
    M.field(F, WIN, "MKPW_TaxInfoZipCodeEdit", "Zip code", x1 - x0 - 2 * p, bh, { max = 5 })
    ui.SetCursorPos(x0 + p - wx, y1 - bh - T.px(20) - wy)
    M.buttons(WIN, { { "MKPW_TaxInfoNextButton", "Next", { primary = true }, always = true }, { "MKPW_CancelBtn", "Cancel" } },
      x0 + p, x1 - p, bh)
  elseif page == 2 then
    line("Confirm: " .. text("MKPW_ConfirmPurchase_Qty") .. " x " .. text("MKPW_ConfirmPurchase"), C.text)
    line("Total " .. text("MKPW_ConfirmPurchase_Total"), C.endurance)
    line(text("MKPW_ConfirmPurchase_ItemRestrictionLabel"), C.faint)
    T.unfont()
    ui.SetCursorPos(x0 + p - wx, y1 - bh - T.px(20) - wy)
    M.buttons(WIN, { { "MKPW_ConfirmPurchase_ConfirmBtn", "Confirm", { primary = true }, always = true },
                     { "MKPW_CancelBtn", "Cancel" } }, x0 + p, x1 - p, bh)
  else
    ui.PushTextWrapPos(x1 - p - wx)
    ui.SetCursorPos(x0 + p - wx, y - wy)
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, text("MKPW_Results_Result"))
    ui.PopTextWrapPos()
    T.unfont()
    ui.SetCursorPos(x0 + p - wx, y1 - bh - T.px(20) - wy)
    M.buttons(WIN, { { "MKPW_Results_OKBtn", "OK", { primary = true }, always = true } }, x0 + p, x1 - p, bh)
  end
end

ui.window("Marketplace", function()
  if not M.visible(WIN) then return end
  M.frame()
  local hints = M.hints(F, { { "a", "Choose" }, { "x", "Open / close" }, { "y", "Inspect" }, { "b", "Close" } })
  if W.begin("Marketplace", { w = 1760, h = 960, title = "Marketplace",
        subtitle = "Funds: " .. text("MKPW_AvailableFundsUpper"), hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    if not M.modal(F, x0, y0, x1, y1) then
      local wx, wy = ui.GetWindowPos()
      local gap = T.px(24)
      local cx1 = x0 + T.px(380)
      local dx0 = x1 - T.px(520)
      -- the categories
      M.list(WIN, "MKPW_CategoriesTree", cats, x0, y0, cx1, y1, {
        rowH = 54, inspect = false, empty = "No categories yet.",
        draw = function(r, a, b, c, d, focused)
          local depth = tonumber(r.texts[2]) or 0
          local mark = r.texts[3] == "1" and (r.texts[4] == "1" and "- " or "+ ") or "  "
          M.row(r, a + T.px(24) * depth, b, c, d, mark .. r.texts[1], nil, { icon = false })
          if focused and W.pressed("x") and r.texts[3] == "1" then API.windows.ListClick(WIN, "MKPW_CategoriesTree", r.index, "left", 1) end
        end })
      -- the shelf
      local sx0, sx1 = cx1 + gap, dx0 - gap
      local fh = T.px(64)
      local top = y0
      if not shown("MKPW_Landing") then
        local half = (sx1 - sx0 - T.px(10)) / 2
        ui.SetCursorPos(sx0 - wx, y0 - wy)
        M.field(F, WIN, "MKPW_FindEditBox", "Search", half, fh, { hint = "Item names", done = find })
        ui.SameLine(0, T.px(10))
        M.combo(F, WIN, "MKPW_Sort_Combobox", "Sort", half, fh)
        top = y0 + fh + T.px(14)
        M.list(WIN, "MKPW_ItemList", items, sx0, top, sx1, y1, {
          rowH = 60, select = "focus", empty = "Nothing on this shelf.",
          draw = function(r, a, b, c, d) itemRow(r, a, b, c, d, 3, 5) end,
          onA = function() API.windows.ClickChild(WIN, "MKPW_BuyBtn", "left") end })
      else
        local mid = (y0 + y1) / 2
        M.caption(sx0, y0, "New")
        M.list(WIN, "MKPW_NewItems", news, sx0, y0 + T.px(36), sx1, mid - T.px(8), {
          rowH = 60, select = "focus", inspect = false, empty = "Nothing new right now.",
          draw = function(r, a, b, c, d) itemRow(r, a, b, c, d, 1, 2) end })
        M.caption(sx0, mid, "Featured")
        M.list(WIN, "MKPW_FeaturedItems", feats, sx0, mid + T.px(36), sx1, y1, {
          rowH = 60, select = "focus", inspect = false, empty = "Nothing featured right now.",
          draw = function(r, a, b, c, d) itemRow(r, a, b, c, d, 1, 2) end })
      end
      -- the selected item
      if W.pressed("y") then API.windows.ClickChild(WIN, "MKPW_DetailsInspect", "left") end
      details(dx0, y0, x1, y1)
    end
  end
  if M.back(F, W.finish()) then API.windows.Show(WIN, false) end
end)
