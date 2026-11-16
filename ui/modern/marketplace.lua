-- Marketplace: replaces the game's MarketplaceWnd (/marketplace, the EQ menu's Marketplace, the EQ button's coin
-- button). The store a server runs through the original client's marketplace packets; a server without one leaves
-- it empty. Three columns, the middle one taking the slack:
--   * the categories: the game's tree (New and Featured, Player Studio, All Items and the server's categories under
--     it); the arrow opens or closes a branch, a click shows that shelf;
--   * the shelf: the search box (Find or Enter), the sort, grid or list; New and Featured shows the landing page (the
--     item of the week, the new and the featured items);
--   * the selected item: its icon, name, maker, timers, description, price, quantity and Buy; Inspect and Preview;
--     the 3D preview on its own tab; the zip code, confirmation and results pages in turn while buying.
-- Your funds head the window. Every field, list and button is the game window's (app/lua_ui_marketplace.cpp), so
-- what is bought, and when the buttons work, is the game's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "MarketplaceWnd"

local TREE_W, DETAILS_W, TILE = 190, 290, 132 -- design pixels
local findActive = false

local function muted(text) ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, text) end
local function faint(text) ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, text) end

local function childIcon(id, size, s)
  local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, id, "decal")
  local x, y = ui.GetCursorScreenPos()
  if tex then T.Icon(x, y, size, s, tex, u0, v0, u1, v1, nil, { radius = 4 })
  else ui.DrawRectFilled(x, y, x + size, y + size, C.ink[1], C.ink[2], C.ink[3], 0.06, 4 * s) end
  ui.Dummy(size, size)
end

-- ---------------------------------------------------------------- the categories
local function tree(w, h, s)
  M.BeginPanel("##mkpwtree", w, h, s)
  local rows, selected = M.Rows(WINDOW, "MKPW_CategoriesTree")
  if #rows == 0 then faint("No categories yet.") end
  for _, r in ipairs(rows) do
    local depth = tonumber(r.texts[2]) or 0
    local branch, open = r.texts[3] == "1", r.texts[4] == "1"
    local cx, cy = ui.GetCursorPos()
    ui.SetCursorPos(cx + depth * 12 * s, cy)
    if branch then
      if ui.SmallButton((open and "v" or ">") .. "##arrow" .. r.index) then
        API.windows.ListClick(WINDOW, "MKPW_CategoriesTree", r.index, "left", 1)
        M.Refresh(WINDOW, "MKPW_CategoriesTree")
      end
      ui.SameLine(0, 4 * s)
    else
      local ax, ay = ui.GetCursorPos()
      ui.SetCursorPos(ax + 18 * s, ay)
    end
    if ui.Selectable(r.texts[1] .. "##cat" .. r.index, r.index == selected) then
      API.windows.ListClick(WINDOW, "MKPW_CategoriesTree", r.index, "left", 0)
      M.Refresh(WINDOW, "MKPW_CategoriesTree")
    end
  end
  M.EndPanel()
end

-- ---------------------------------------------------------------- tiles
-- One tile of a tile list (MKPW_Thumbnails, MKPW_NewItems, MKPW_FeaturedItems): icon, name, price, was, timer.
local function tile(list, r, selected, s)
  local t = r.texts
  local x, y = ui.GetCursorScreenPos()
  local tw, th = TILE * s, 112 * s
  local sel = r.index == selected
  ui.DrawRectFilled(x, y, x + tw, y + th, C.ink[1], C.ink[2], C.ink[3], sel and 0.14 or 0.05, 6 * s)
  if sel then ui.DrawRect(x, y, x + tw, y + th, C.accent[1], C.accent[2], C.accent[3], 0.8, 6 * s, 1.5) end
  local isz = 40 * s
  if r.tex then T.Icon(x + (tw - isz) / 2, y + 8 * s, isz, s, r.tex, r.uv[1], r.uv[2], r.uv[3], r.uv[4], nil, { radius = 4 }) end
  if t[6] == "1" then ui.DrawText(x + 6 * s, y + 6 * s, C.accent[1], C.accent[2], C.accent[3], 1, "New") end
  if (t[7] or "") ~= "" then ui.DrawText(x + 6 * s, y + 6 * s, C.muted[1], C.muted[2], C.muted[3], 1, "Studio") end
  local ty = y + 8 * s + isz + 4 * s
  ui.DrawText(x + 6 * s, ty, C.text[1], C.text[2], C.text[3], 1, T.Fit(t[1] or "", tw - 12 * s, nil))
  local _, lh = ui.CalcTextSize("Ag")
  ui.DrawText(x + 6 * s, ty + lh, C.accent[1], C.accent[2], C.accent[3], 1, t[2] or "")
  if (t[3] or "") ~= "" then
    local pw = ui.CalcTextSize(t[2] or "")
    ui.DrawText(x + 12 * s + pw, ty + lh, C.faint[1], C.faint[2], C.faint[3], 1, t[3])
  end
  if (t[5] or "") ~= "" then
    ui.DrawText(x + 6 * s, ty + 2 * lh, C.muted[1], C.muted[2], C.muted[3], 1,
      (t[4] == "sale" and "Sale ends " or "Leaves in ") .. t[5])
  end
  ui.InvisibleButton("##tile" .. list .. r.index, tw, th)
  if ui.IsItemClicked(ui.Mouse.Left) then API.windows.ListClick(WINDOW, list, r.index, "left") end
  if ui.IsItemHovered() then ui.SetTooltip(t[1] or "") end
end

local function tiles(list, w, s, empty)
  local rows, selected = M.Rows(WINDOW, list)
  if #rows == 0 then
    faint(empty)
    return
  end
  local perRow = math.max(1, math.floor((w + 8 * s) / ((TILE + 8) * s)))
  for i, r in ipairs(rows) do
    if (i - 1) % perRow ~= 0 then ui.SameLine(0, 8 * s) end
    tile(list, r, selected, s)
  end
end

-- ---------------------------------------------------------------- the shelf
local function landing(w, h, s)
  ui.BeginChild("##mkpwlanding", w, h, false)
  W.Heading("Item of the Week")
  if W.Shown(WINDOW, "MKPW_ItemOfWeekIcon") then
    childIcon("MKPW_ItemOfWeekIcon", 48 * s, s)
    ui.SameLine(0, 10 * s)
    ui.BeginGroup()
    W.Label(WINDOW, "MKPW_ItemOfWeekName", C.heading)
    if W.Shown(WINDOW, "MKPW_ItemOfWeekNew") then ui.SameLine(); ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, "New") end
    ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, W.Text(WINDOW, "MKPW_ItemOfWeekPrice"))
    if W.Shown(WINDOW, "MKPW_ItemOfWeekWas") then ui.SameLine(); faint(W.Text(WINDOW, "MKPW_ItemOfWeekWas")) end
    W.Button(WINDOW, "MKPW_ItemOfWeekBuyNowBtn", "Show me", { tooltip = "Go to this item" })
    ui.EndGroup()
    W.Stml(WINDOW, "MKPW_ItemOfWeekDescription", 0, 60 * s)
  else
    faint(W.Text(WINDOW, "MKPW_ItemOfWeekName"))
  end
  W.Heading("New")
  tiles("MKPW_NewItems", w, s, "Nothing new right now.")
  W.Heading("Featured")
  tiles("MKPW_FeaturedItems", w, s, "Nothing featured right now.")
  ui.EndChild()
end

local function find()
  API.windows.ClickChild(WINDOW, "MKPW_FindBtn", "left")
end

local function header(w, s, btnH)
  W.Edit(WINDOW, "MKPW_FindEditBox", { width = w * 0.42, hint = "Search item names" })
  local active = ui.IsItemActive()
  if findActive and not active and ui.IsKeyPressed(ui.Key.Enter) then find() end
  findActive = active
  ui.SameLine()
  W.Button(WINDOW, "MKPW_FindBtn", "Find", { height = btnH, tooltip = "Show the items whose name holds the text" })
  ui.SameLine(0, 12 * s)
  W.Combo(WINDOW, "MKPW_Sort_Combobox", { width = 150 * s })
  ui.SameLine(0, 12 * s)
  W.Button(WINDOW, "MKPW_IconBtn", "Grid", { height = btnH })
  ui.SameLine(0, 4 * s)
  W.Button(WINDOW, "MKPW_ListBtn", "List", { height = btnH })
end

local function shelf(w, h, s, lh, btnH)
  if W.Shown(WINDOW, "MKPW_Landing") then return landing(w, h, s) end
  header(w, s, btnH)
  ui.Dummy(0, 2 * s)
  local bodyH = h - btnH - 6 * s
  if W.Shown(WINDOW, "MKPW_ItemList") then
    M.Table(WINDOW, "MKPW_ItemList", w, bodyH, {
      { "", 16, text = 1, nosort = true },
      { "Item", 0, cell = M.IconName(3), sort = function(r) return r.texts[3] end },
      { "Price", 90, cell = M.Number(5), sort = function(r) return tonumber(r.texts[5]) or 0 end },
    }, { icon = true, empty = "Nothing on this shelf.", key = "mkpwlist" })
  else
    ui.BeginChild("##mkpwgrid", w, bodyH, false)
    tiles("MKPW_Thumbnails", w, s, "Nothing on this shelf.")
    ui.EndChild()
  end
end

-- ---------------------------------------------------------------- the selected item
local function timers()
  for _, p in ipairs({ { "MKPW_DetailsAvailableTimeLeft_Label", "Available for " },
                       { "MKPW_DetailsSaleTimeLeft_Label", "On sale for " } }) do
    if W.Shown(WINDOW, p[1]) then muted(p[2] .. W.Text(WINDOW, p[1])) end
  end
end

local function preview(w, h, s, btnH)
  local x, y = ui.GetCursorScreenPos()
  local vh = math.max(h - btnH - 8 * s, 16)
  if not W.Shown(WINDOW, "MKPW_PreviewRenderArea") then
    faint("No item preview available.")
    return
  end
  local tex = API.preview.Image(w, vh, "marketplace")
  ui.DrawRectFilled(x, y, x + w, y + vh, 0, 0, 0, 0.25, 6 * s)
  if tex then ui.DrawImage(tex, x, y, x + w, y + vh, 0, 0, 1, 1) end
  ui.InvisibleButton("##mkpwview", w, vh, ui.ButtonFlags.MouseButtonLeft + ui.ButtonFlags.MouseButtonRight)
  local io = ui.GetIO()
  if ui.IsItemHovered() and io.wheel ~= 0 then API.preview.Control("zoom", io.wheel > 0 and 0.9 or 1.1, 0, "marketplace") end
  if ui.IsItemActive() then
    if ui.IsMouseDown(ui.Mouse.Left) then
      API.preview.Control("rotate", io.mouseDX * 0.01, -io.mouseDY * 0.01, "marketplace")
    elseif ui.IsMouseDown(ui.Mouse.Right) then
      API.preview.Control("pan", -io.mouseDX / w, io.mouseDY / vh, "marketplace")
    end
  end
  if ui.Button("Reset view##mkpw", -1, btnH) then API.preview.Control("reset", 0, 0, "marketplace") end
  if W.Shown(WINDOW, "MKPW_PreviewPlayerStudioLabel") then muted(W.Text(WINDOW, "MKPW_PreviewPlayerStudioLabel")) end
end

local function mainPage(s, btnH)
  W.Field("Price", W.Text(WINDOW, "MKPW_BuyPrice"), C.accent)
  if W.Shown(WINDOW, "MKPW_WasPrice") then ui.SameLine(); faint("(was " .. W.Text(WINDOW, "MKPW_WasPrice") .. ")") end
  muted("Quantity")
  ui.SameLine()
  W.Button(WINDOW, "MKPW_QtyDownBtn", "-", { width = btnH, height = btnH })
  ui.SameLine(0, 4 * s)
  W.Edit(WINDOW, "MKPW_QtyEdit", { width = 44 * s, max = 4 })
  ui.SameLine(0, 4 * s)
  W.Button(WINDOW, "MKPW_QtyUpBtn", "+", { width = btnH, height = btnH })
  W.Field("Total", W.Text(WINDOW, "MKPW_BuyTotal"), C.accent)
  ui.Dummy(0, 4 * s)
  W.ButtonRow(WINDOW, { { "MKPW_BuyBtn", "Buy", { primary = true, hidden = false, tooltip = "Buy this item" } } }, btnH)
end

local function page(s, btnH)
  local p = API.windows.TabPage(WINDOW, "MKPW_Subwindows") or 0
  if p == 0 then return mainPage(s, btnH) end
  if p == 1 then
    W.Heading("Billing zip code")
    muted("Enter your 5 digit zip code to go on.")
    W.Edit(WINDOW, "MKPW_TaxInfoZipCodeEdit", { width = 90 * s, max = 6 })
    W.ButtonRow(WINDOW, { { "MKPW_TaxInfoNextButton", "Next", { primary = true, hidden = false } },
                          { "MKPW_CancelBtn", "Cancel" } }, btnH)
  elseif p == 2 then
    W.Heading("Confirm purchase")
    W.Label(WINDOW, "MKPW_ConfirmPurchase", C.heading)
    W.Field("Quantity", W.Text(WINDOW, "MKPW_ConfirmPurchase_Qty"))
    W.Field("Price each", W.Text(WINDOW, "MKPW_ConfirmPurchase_Price"), C.accent)
    W.Field("Total", W.Text(WINDOW, "MKPW_ConfirmPurchase_Total"), C.accent)
    ui.PushTextWrapPos(0)
    faint(W.Text(WINDOW, "MKPW_ConfirmPurchase_ItemRestrictionLabel"))
    ui.PopTextWrapPos()
    W.ButtonRow(WINDOW, { { "MKPW_ConfirmPurchase_ConfirmBtn", "Confirm", { primary = true, hidden = false } },
                          { "MKPW_CancelBtn", "Cancel" } }, btnH)
  else
    W.Heading("Purchase")
    W.Label(WINDOW, "MKPW_Results_Result", nil, true)
    W.ButtonRow(WINDOW, { { "MKPW_Results_OKBtn", "OK", { hidden = false } } }, btnH)
  end
end

local function details(w, h, s, lh, btnH)
  M.BeginPanel("##mkpwdetails", w, h, s)
  if W.Text(WINDOW, "MKPW_DetailsName") == "" then
    faint("Select an item.")
    M.EndPanel()
    return
  end
  childIcon("MKPW_DetailsIcon", 40 * s, s)
  if ui.IsItemClicked(ui.Mouse.Right) then API.windows.ClickChild(WINDOW, "MKPW_DetailsIcon", "right") end
  ui.SameLine(0, 8 * s)
  ui.BeginGroup()
  ui.PushTextWrapPos(0)
  W.Label(WINDOW, "MKPW_DetailsName", C.heading)
  ui.PopTextWrapPos()
  if W.Shown(WINDOW, "MKPW_DetailsPlayerStudioLabel") then muted(W.Text(WINDOW, "MKPW_DetailsPlayerStudioLabel")) end
  timers()
  ui.EndGroup()
  W.ButtonRow(WINDOW, { { "MKPW_DetailsInspect", "Inspect", { tooltip = "Show the item's details" } },
                        { "MKPW_DetailsPreview", "Preview", { tooltip = "Open the item preview window" } } }, btnH)
  -- the details' tab box (a good preview reply turns it to the preview by itself)
  local tab = W.Tabs(WINDOW, "MKPW_AdditionalDetails", { { "Details", 0 }, { "Preview", 1 } })
  local _, cy = ui.GetCursorScreenPos()
  local _, py = ui.GetWindowPos()
  local rest = h - (cy - py) - 6 * s
  if tab == 1 then
    preview(ui.GetContentRegionAvail(), rest, s, btnH)
  else
    local pageH = btnH * 5 + lh * 3
    W.Stml(WINDOW, "MKPW_DetailsDescription", 0, math.max(rest - pageH, 40 * s))
    page(s, btnH)
  end
  M.EndPanel()
end

local OPTS = {
  size = function() return 1000, 600 end,
  resizable = { 760, 420 },
}
local function body(x, y, w, h, s)
  local lh, btnH = M.Heights(s)
  muted("Funds")
  ui.SameLine()
  ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, W.Text(WINDOW, "MKPW_AvailableFundsUpper"))
  local bodyH = h - lh - 6 * s
  local treeW, detW = TREE_W * s, DETAILS_W * s
  tree(treeW, bodyH, s)
  ui.SameLine(0, 8 * s)
  local midW = w - treeW - detW - 16 * s
  ui.BeginGroup()
  shelf(midW, bodyH, s, lh, btnH)
  ui.EndGroup()
  ui.SameLine(0, 8 * s)
  details(detW, bodyH, s, lh, btnH)
end

ui.window("Marketplace", function()
  if not API.session.InGame() then return end
  M.TestOpen(WINDOW)
  OPTS.shown = M.Forced(WINDOW)
  hud.Dialog(WINDOW, "Marketplace", OPTS, body)
end)
