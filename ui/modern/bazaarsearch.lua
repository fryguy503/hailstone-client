-- Bazaar search: replaces the game's BazaarSearchWnd. The filters down the left (name, trader, slot, stat, race,
-- class, type, augment slot, glamours, levels, prices, results a trader) and the results as a sortable table: the
-- item with its icon, how many, the price each as coins, the trader, and the stat's value when a stat was picked.
-- A box over the results narrows what is shown (by item or trader) without asking again.
-- Everything is the game window's: the fields and drop-downs are its children (hs.SetWindowText, hs.ComboSelect),
-- Find Items, Reset, Inspect, Buy, Find Trader, Hide Traders and Welcome its buttons, a row click selects in its list
-- (a right or Shift click inspects, as on the stock list). Buy opens the game's quantity dialog and then the purchase confirmation.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "BazaarSearchWnd"
local LIST = "BZR_ItemList"

local SIDE_W = 230 -- the filters' column, design pixels

-- the drop-downs, in the order a buyer narrows by
local COMBOS = {
  { "BZR_PlayersCombobox", "Trader", "Search one trader's goods only" },
  { "BZR_ItemSlotCombobox", "Slot", "Items worn in this slot" },
  { "BZR_ItemTypeCombobox", "Type", "Weapon, armor or other item type" },
  { "BZR_StatSlotCombobox", "Stat", "Items with this stat (its value becomes a column)" },
  { "BZR_ClassSlotCombobox", "Class", "Items this class can use" },
  { "BZR_RaceSlotCombobox", "Race", "Items this race can use" },
  { "BZR_ItemAugmentCombobox", "Augment", "Augments that fit this augment slot type" },
  { "BZR_ItemPrestigeCombobox", "Glamours", "Include, exclude or only show glamour (prestige) items" },
}

local searched = false -- a search was asked for since the window opened (for the empty state)
local lastDrawn = 0
local nameActive = false -- the name field had the keyboard last frame (Enter there searches)

local function price(r)
  local t = r.texts
  return (tonumber(t[4]) or 0) * 1000 + (tonumber(t[5]) or 0) * 100 + (tonumber(t[6]) or 0) * 10 + (tonumber(t[7]) or 0)
end

local function find()
  if W.State(WINDOW, "BZR_QueryButton") then
    API.windows.ClickChild(WINDOW, "BZR_QueryButton", "left")
    M.Refresh(WINDOW, LIST)
    searched = true
  end
end

-- "min - max" edits on one line
local function range(caption, minId, maxId, tip, s)
  M.Caption(caption)
  M.Tip(tip)
  local avail = ui.GetContentRegionAvail()
  local dash = ui.CalcTextSize("-")
  local fw = (avail - dash - 16) / 2
  W.Edit(WINDOW, minId, { width = fw, hint = "min" })
  ui.SameLine(0, 4)
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "-")
  ui.SameLine(0, 4)
  W.Edit(WINDOW, maxId, { width = fw, hint = "max" })
end

local function filters(s, lh)
  W.Heading("Find")
  W.Edit(WINDOW, "BZR_ItemNameInput", { hint = "Item name (part of it)" })
  local active = ui.IsItemActive()
  if nameActive and not active and ui.IsKeyPressed(ui.Key.Enter) then find() end
  nameActive = active
  M.Tip("Press Enter to search")
  ui.Dummy(0, 2 * s)
  local capW = 62 * s
  for _, c in ipairs(COMBOS) do
    if API.windows.ComboInfo(WINDOW, c[1]) then
      local x, y = ui.GetCursorScreenPos()
      ui.DrawText(x, y + 3 * s, C.muted[1], C.muted[2], C.muted[3], 1, c[2])
      ui.SetCursorScreenPos(x + capW, y)
      W.Combo(WINDOW, c[1])
      M.Tip(c[3])
    end
  end
  ui.Dummy(0, 2 * s)
  range("Level", "BZR_MinLevelInput", "BZR_MaxLevelInput", "The item's required level", s)
  range("Price each (platinum)", "BZR_MinPriceInput", "BZR_MaxPriceInput", "Whole platinum pieces", s)
  M.Caption("Results per trader")
  W.Edit(WINDOW, "BZR_MaxResultsPerTraderInput", { hint = "200" })
end

local WINDOW_OPTS = {
  size = function() return 820, 560 end,
  resizable = { 600, 360 },
}
local function drawWindow(x, y, w, h, s)
  lastDrawn = ui.GetTime()
  local lh, btnH = M.Heights(s)
  local sideW = SIDE_W * s

  -- ---------------------------------------------------------------- filters
  M.BeginPanel("##bzsfilters", sideW, h, s)
  local footer = btnH * 2 + 14 * s
  ui.BeginChild("##bzsfscroll", 0, -footer, false)
  filters(s, lh)
  ui.EndChild()
  local searching = not W.State(WINDOW, "BZR_QueryButton")
  if W.Button(WINDOW, "BZR_QueryButton", searching and "Searching..." or "Find Items",
      { primary = true, width = -1, height = btnH, tooltip = "Search every trader in the bazaar" }) then
    M.Refresh(WINDOW, LIST)
    searched = true
  end
  W.Button(WINDOW, "BZR_Default", "Reset Filters", { width = -1, height = btnH, tooltip = "Back to the defaults" })
  M.EndPanel()

  -- ---------------------------------------------------------------- results
  ui.SameLine(0, 8 * s)
  ui.BeginGroup()
  local rw = w - sideW - 8 * s
  local n = tonumber(W.Text(WINDOW, "BZR_SearchResultLabel")) or 0
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, "Results")
  ui.PopFont()
  ui.SameLine()
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, n == 1 and "1 item" or (T.Thousands(n) .. " items"))
  ui.SameLine(0, 12 * s)
  local needle = M.Search("bzs", "Filter by item or trader", -1)
  local _, _, _, labels = API.windows.ListInfo(WINDOW, LIST)
  local stat = labels and labels[10] or "Stat"
  local cols = {
    { "Item", 0, cell = M.IconName(2), sort = function(r) return r.texts[2] end, default = "asc" },
    { "Qty", 44, cell = M.Number(3), sort = function(r) return tonumber(r.texts[3]) or 0 end, desc = true },
    { "Price each", 116, cell = M.PriceCell(price), sort = price },
    { "Trader", 110, text = 8 },
  }
  if stat ~= "" and stat ~= "Stat" then
    cols[#cols + 1] = { stat, 70, cell = M.Number(10), sort = function(r) return tonumber(r.texts[10]) or 0 end, desc = true }
  end
  local barH = btnH + 30 * s
  local shownRows, _, sel = M.Table(WINDOW, LIST, rw, h - lh - 8 * s - barH, cols, {
    key = "bzs" .. #cols,
    icon = true,
    filter = needle ~= "" and function(r)
      return (r.texts[2] or ""):lower():find(needle, 1, true) ~= nil or (r.texts[8] or ""):lower():find(needle, 1, true) ~= nil
    end or nil,
    filterKey = needle,
    empty = searching and "Searching..." or (searched and "Nothing matches these filters." or "Set the filters and press Find Items."),
    emptyFiltered = "No result matches \"" .. needle .. "\".",
    tooltip = function(r)
      return (r.texts[2] or "") .. "\nSold by " .. (r.texts[8] or "?") .. "\nDouble-click to buy, right-click to inspect"
    end,
    doubleClick = function() API.windows.ClickChild(WINDOW, "BZR_BuyButton", "left") end,
  })

  -- ---------------------------------------------------------------- the selected result
  ui.Dummy(0, 2 * s)
  if sel then
    local t = sel.texts
    local qty = tonumber(t[3]) or 1
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, t[2] or "")
    ui.SameLine()
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, (qty > 1 and ("x" .. qty .. "  ") or "") .. "from " .. (t[8] or "?") .. ":")
    ui.SameLine()
    M.Price(price(sel))
    ui.SameLine()
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, qty > 1 and "each" or "")
    if qty > 1 then
      ui.SameLine()
      ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "(all:")
      ui.SameLine(0, 4)
      M.Price(price(sel) * qty)
      ui.SameLine(0, 0)
      ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, ")")
    end
  else
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1,
      shownRows > 0 and "Select a result to buy or inspect it." or " ")
  end
  W.ButtonRow(WINDOW, {
    { "BZR_BuyButton", "Buy", { primary = true, hidden = false, tooltip = "Buy the selected item (asks how many, then confirms)" } },
    { "BZR_RequestItemButton", "Inspect", { tooltip = "Show the selected item's details" } },
    { "BZR_FindTraderButton", "Find Trader", { tooltip = "Show the way to the selected item's trader (in this zone)" } },
    { "BZR_WelcomeButton", "Welcome", { tooltip = "Show the bazaar's welcome message (how many traders and items)" } },
  }, btnH)
  W.Check(WINDOW, "BZR_HideTradersButton", "Hide other traders",
    { tooltip = "Hide every trader but the one Find Trader showed the way to" })
  ui.EndGroup()
end

ui.window("Bazaar Search", function()
  if not API.session.InGame() then return end
  if ui.GetTime() - lastDrawn > 1 then searched = false end -- (re)opened
  hud.Dialog(WINDOW, "Bazaar Search", WINDOW_OPTS, drawWindow)
end)
