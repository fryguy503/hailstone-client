-- Real estate (player housing and guild plots): replaces the game's five real estate windows, on a server that has
-- housing (a stock EQEmu server has none, so they never open there).
--   Plot           RealEstatePurchaseWnd: the plot's address, owner, kind and capacities, the price and upkeep
--                  choices, Purchase, Sell, Transfer (to your target), the opt-out box, and Items / Manage
--   Manage         RealEstateManageWnd: who may do what on your plot (seven groups of permissions, the member list)
--                  and its escrow (balances, upkeep, time left; Deposit the coin on your cursor, Withdraw, Add Day /
--                  Week, Withdraw All)
--   Items          RealEstateItemsWnd: the items of the yard, house and crate you stand in; move them to your
--                  inventory, the closet, shed or crate, inspect them; a right click asks the server for the item's
--                  menu (enter a house, store, ...)
--   Neighborhoods  RealEstateNeighborhoodWnd: the neighborhoods with their free plots, a row's players and guilds, Go
--   Plot Search    RealEstatePlotSearchWnd: search plots by owner, Go travels to the one picked
-- Everything is the stock window's (hs.ClickChild, hs.ListClick, hs.ComboSelect, hs.SetWindowText); the windows grey
-- what you may not do.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color

local function btnH()
  local _, lh = ui.CalcTextSize("Ag")
  return lh + 10 * (ui.GetIO().uiScale or 1)
end

local function field(win, caption, id)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, caption)
  ui.SameLine(130 * (ui.GetIO().uiScale or 1))
  W.Label(win, id)
end

-- ---------------------------------------------------------------- the plot
local PW = "RealEstatePurchaseWnd"
local PLOT_OPTS = { size = function() return 440, 460 end, resizable = { 360, 380 } }
local PLOT_BUTTONS = {
  { "PurchaseButton", "Purchase", { primary = true, tooltip = "Buy the plot at the chosen price and upkeep" } },
  { "SellButton", "Sell", { danger = true, tooltip = "Give the plot up (asks first)" } },
  { "TransferButton", "Transfer", { tooltip = "Give the plot to your target (asks first)" } },
}
local PLOT_PAGES = {
  { "ItemsButton", "Items", { hidden = false } },
  { "ManageButton", "Manage", { hidden = false } },
}
local function plotBody(x, y, w, h, s)
  W.Heading("Plot")
  field(PW, "Address", "AddressLabel")
  field(PW, "Name", "NameLabel")
  field(PW, "Kind", "TypeLabel")
  field(PW, "Owner", "OwnerLabel")
  if W.Shown(PW, "NeighborhoodGuildLabel") then field(PW, "Neighborhood", "NeighborhoodGuildLabel") end
  field(PW, "Category", "CategoryLabel")
  W.Heading("Capacity")
  field(PW, "Items", "CapacityLabel")
  field(PW, "Vault", "VaultLabel")
  field(PW, "Pets", "PetLabel")
  W.Heading("Cost")
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Price")
  W.Combo(PW, "PriceCombo")
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Upkeep")
  W.Combo(PW, "UpkeepCombo")
  W.Check(PW, "OptOutCheck", "Opt out of neighborhood votes")
  W.Label(PW, "DescriptionLabel", C.muted, true)
  ui.Dummy(0, 4 * s)
  W.ButtonRow(PW, PLOT_BUTTONS, btnH())
  W.ButtonRow(PW, PLOT_PAGES, btnH())
end
ui.window("Real Estate Plot", function()
  if not API.session.InGame() then return end
  hud.Dialog(PW, "Plot", PLOT_OPTS, plotBody)
end)

-- ---------------------------------------------------------------- manage
local MW = "RealEstateManageWnd"
local GROUPS = { "Everyone", "Visitor", "Acquaintance", "Friend", "Trustee", "Co-Owner", "Reserved" }
local BITS = { "Receive buffs", "Enter", "Withdraw upkeep", "Remove crate", "Add crate", "Remove house", "Add house",
  "Mail item", "Remove item", "Add item", "Move item", "Interact", "Manage" }

local MEMBER_BUTTONS = {
  { "AddMemberButton", "Add Member", { hidden = false, tooltip = "Add a player by name (as a visitor)" } },
  { "RemoveMemberButton", "Remove", { hidden = false } },
}
local PERM_CHECK = { hidden = false }
local PERM_IDS = {} -- PERM_IDS[b][g]: "Perm_<g>_<b>" (0-based)
for b = 0, #BITS - 1 do
  PERM_IDS[b] = {}
  for g = 0, #GROUPS - 1 do PERM_IDS[b][g] = string.format("Perm_%d_%d", g, b) end
end

local function accessPage(s)
  local _, avail = ui.GetContentRegionAvail()
  local listH = math.max(80 * s, avail * 0.35)
  W.List(MW, "MemberList", 0, listH)
  W.Combo(MW, "MemberGroupCombo")
  W.ButtonRow(MW, MEMBER_BUTTONS, btnH())
  W.Heading("Permissions")
  if ui.BeginTable("##reperm", #GROUPS + 1, ui.TableFlags.ScrollY | ui.TableFlags.RowBg, 0, -1) then
    ui.TableSetupColumn("", 0, 0)
    for _, g in ipairs(GROUPS) do ui.TableSetupColumn(g, 0, 0) end
    ui.TableHeadersRow()
    for b = 0, #BITS - 1 do
      ui.TableNextRow()
      ui.TableNextColumn()
      ui.Text(BITS[b + 1])
      for g = 0, #GROUPS - 1 do
        ui.TableNextColumn()
        W.Check(MW, PERM_IDS[b][g], "", PERM_CHECK)
      end
    end
    ui.EndTable()
  end
end

local ESCROW_BUTTONS = {
  { "DepositButton", "Deposit", { primary = true, hidden = false, tooltip = "Put the coin on your cursor into the escrow" } },
  { "WithdrawButton", "Withdraw", { hidden = false, tooltip = "Take some of the selected balance (asks how much)" } },
  { "WithdrawAllButton", "Withdraw All", { hidden = false } },
}
local UPKEEP_BUTTONS = {
  { "AddDayButton", "Add Day", { hidden = false, tooltip = "Pay one more day of upkeep" } },
  { "AddWeekButton", "Add Week", { hidden = false, tooltip = "Pay one more week of upkeep" } },
  { "TransferButton", "Transfer", { hidden = false, tooltip = "Give the plot to your target (asks first)" } },
}

local function escrowPage(s)
  field(MW, "Time left", "TimeLeftLabel")
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Upkeep plan")
  W.Combo(MW, "UpkeepCombo")
  local _, avail = ui.GetContentRegionAvail()
  local bh = btnH()
  W.Heading("Balance")
  W.List(MW, "BalanceList", 0, math.max(60 * s, (avail - 3 * bh) * 0.45))
  W.Heading("Daily upkeep")
  W.List(MW, "UpkeepList", 0, -(2 * bh + 16 * s))
  W.ButtonRow(MW, ESCROW_BUTTONS, bh)
  W.ButtonRow(MW, UPKEEP_BUTTONS, bh)
end

local MANAGE_OPTS = { size = function() return 640, 560 end, resizable = { 480, 420 } }
local function manageBody(x, y, w, h, s)
  if ui.BeginTabBar("##remanage") then
    if ui.BeginTabItem("Access") then
      accessPage(s)
      ui.EndTabItem()
    end
    if ui.BeginTabItem("Escrow") then
      escrowPage(s)
      ui.EndTabItem()
    end
    ui.EndTabBar()
  end
end
ui.window("Real Estate Manage", function()
  if not API.session.InGame() then return end
  hud.Dialog(MW, "Manage Plot", MANAGE_OPTS, manageBody)
end)

-- ---------------------------------------------------------------- items
local IW = "RealEstateItemsWnd"
local ITEMS_OPTS = { size = function() return 520, 520 end, resizable = { 380, 360 } }
local SORTABLE = { sortable = true }
local STORE_BUTTONS = {
  { "InventoryButton", "Inventory", { hidden = false, tooltip = "Move the item to your inventory" } },
  { "ClosetButton", "Closet", { hidden = false, tooltip = "Store it in the house's closet" } },
  { "ShedButton", "Shed", { hidden = false, tooltip = "Store it in the yard's shed" } },
  { "CrateButton", "Crate", { hidden = false, tooltip = "Store it in the moving crate" } },
  { "InspectButton", "Inspect", { hidden = false } },
}
local PLACE_BUTTONS = {
  { "PlaceButton", "Place", { hidden = false, tooltip = "Placing items is not available yet" } },
  { "StowButton", "Store All", { hidden = false, tooltip = "Move every placed item into storage (asks first)" } },
  { "PackButton", "Pack", { hidden = false } },
  { "UnpackButton", "Unpack", { hidden = false } },
}
local function itemsBody(x, y, w, h, s)
  local bh = btnH()
  W.List(IW, "ItemList", 0, -(2 * bh + 14 * s), SORTABLE)
  if ui.IsItemHovered() then ui.SetTooltip("Right click an item for its menu") end
  W.ButtonRow(IW, STORE_BUTTONS, bh)
  W.ButtonRow(IW, PLACE_BUTTONS, bh)
end
ui.window("Real Estate Items", function()
  if not API.session.InGame() then return end
  hud.Dialog(IW, "Real Estate Items", ITEMS_OPTS, itemsBody)
end)

-- ---------------------------------------------------------------- neighborhoods
local NW = "RealEstateNeighborhoodWnd"
local NEIGHBORHOOD_OPTS = { size = function() return 600, 480 end, resizable = { 420, 360 } }
local NEIGHBORHOOD_BUTTONS = {
  { "GoButton", "Go", { primary = true, hidden = false, tooltip = "Travel to the selected neighborhood" } },
  { "MyPlotsButton", "My Plots", { hidden = false } },
  { "MyItemsButton", "My Items", { hidden = false } },
}
local function neighborhoodBody(x, y, w, h, s)
  local bh = btnH()
  local _, avail = ui.GetContentRegionAvail()
  W.List(NW, "NeighborhoodList", 0, math.max(80 * s, avail * 0.5), SORTABLE)
  local half = (ui.GetContentRegionAvail() - 8 * s) / 2
  ui.BeginChild("##reh_players", half, -(bh + 10 * s), false)
  W.List(NW, "PlayerList", 0, 0)
  ui.EndChild()
  ui.SameLine()
  ui.BeginChild("##reh_guilds", 0, -(bh + 10 * s), false)
  W.List(NW, "GuildList", 0, 0)
  ui.EndChild()
  W.ButtonRow(NW, NEIGHBORHOOD_BUTTONS, bh)
end
ui.window("Real Estate Neighborhoods", function()
  if not API.session.InGame() then return end
  hud.Dialog(NW, "Neighborhoods", NEIGHBORHOOD_OPTS, neighborhoodBody)
end)

-- ---------------------------------------------------------------- plot search
local SW = "RealEstatePlotSearchWnd"
local SEARCH_OPTS = { size = function() return 560, 460 end, resizable = { 400, 320 } }
local NAME_EDIT = { hint = "Owner name (empty: every plot)",
  onCommit = function() API.windows.ClickChild(SW, "SearchButton", "left") end }
local SEARCH_BUTTON = { hidden = false }
local PLOT_LIST = { sortable = true, doubleClick = function() API.windows.ClickChild(SW, "GoButton", "left") end }
local GO_BUTTONS = { { "GoButton", "Go", { primary = true, hidden = false, tooltip = "Go to the selected plot" } } }
local function searchBody(x, y, w, h, s)
  local bh = btnH()
  local bw = 100 * s
  NAME_EDIT.width = -(bw + 8 * s)
  W.Edit(SW, "NameEdit", NAME_EDIT)
  ui.SameLine()
  SEARCH_BUTTON.width = bw
  W.Button(SW, "SearchButton", "Search", SEARCH_BUTTON)
  W.List(SW, "PlotList", 0, -(bh + 10 * s), PLOT_LIST)
  W.ButtonRow(SW, GO_BUTTONS, bh)
end
ui.window("Real Estate Plot Search", function()
  if not API.session.InGame() then return end
  hud.Dialog(SW, "Plot Search", SEARCH_OPTS, searchBody)
end)
