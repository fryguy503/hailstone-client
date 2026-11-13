-- Merchant: replaces the game's MerchantWnd. The merchant's goods as a table (icon, name, how many the merchant
-- has, the price in coins, the level) with a search box, sorting by any column (the price by its whole value),
-- "Usable only" (the game's MW_UsableButton, on to start) and "Affordable only"; prices you cannot pay are dimmed. Below it the item being considered (the merchant's, or
-- one of yours picked in your bags to sell) with its price, Buy or Sell, Inspect, and your purse.
-- The rows, the selection and the buttons are the game's (eq.ListRow / eq.ListClick / eq.ClickChild on ItemList,
-- MW_Buy_Button, MW_Sell_Button, ...): a row click selects in the original, a right click inspects, Buy and Sell
-- open the game's quantity dialog. The Recover and Parcels pages are the window's tab pages, with its buttons.
-- Hailstone: Sell junk sells every carried item marked as junk (lib/itemtools.lua), one after the other; the
-- Parcels page lists the parcels waiting, with Retrieve and Claim all, and sends an item or money by parcel.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local TC = dofile("lib/trade_common.lua")
local tools = dofile("lib/itemtools.lua")
local items = dofile("lib/items.lua")
local T = hud.T
local C = T.color
local WINDOW = "MerchantWnd"
local LIST = "ItemList"

local search = ""
local affordable = false
local sortCol, sortAsc = nil, true -- nil: the merchant's own order
local consideredH = nil             -- the height the strip under the table took last frame

-- The merchant's rows: { row, name, lname (lower case), qty, copper, level, tex, uv }. A row's record is kept while
-- everything it was made from reads the same (the cells and the icon), so a frame makes no tables and no lower-case
-- names for rows that did not change.
local rowCache, rowList = {}, {}
local function same(e, t, tex, u0, v0, u1, v1)
  return e.src2 == t[2] and e.src3 == t[3] and e.src4 == t[4] and e.src5 == t[5] and e.src6 == t[6]
    and e.src7 == t[7] and e.src8 == t[8] and e.tex == tex and e.uv[1] == u0 and e.uv[2] == v0 and e.uv[3] == u1
    and e.uv[4] == v1
end
local function readRows()
  local out = rowList
  local n = 0
  local rows = eq.ListInfo(WINDOW, LIST) or 0
  for r = 0, rows - 1 do
    local t, _, _, _, tex, u0, v0, u1, v1 = eq.ListRow(WINDOW, LIST, r)
    if t then
      local e = rowCache[r]
      if not (e and same(e, t, tex, u0, v0, u1, v1)) then
        local name = t[2] or ""
        e = { row = r, name = name, lname = name:lower(), qty = t[3] or "",
          copper = TC.Copper(t[4], t[5], t[6], t[7]), level = t[8] or "", tex = tex, uv = { u0, v0, u1, v1 },
          src2 = t[2], src3 = t[3], src4 = t[4], src5 = t[5], src6 = t[6], src7 = t[7], src8 = t[8] }
        rowCache[r] = e
      end
      n = n + 1
      out[n] = e
    end
  end
  for k = n + 1, #out do out[k] = nil end
  for r in pairs(rowCache) do if r >= rows then rowCache[r] = nil end end
  return out
end

local function down(dy)
  local cx, cy = ui.GetCursorPos()
  ui.SetCursorPos(cx, cy + dy)
end

local KEYS = { function(e) return e.lname end, function(e) return tonumber(e.qty) or math.huge end,
  function(e) return e.copper end, function(e) return tonumber(e.level) or 0 end }

-- the sort's comparator, made once: the key and direction are this frame's
local sortKey = KEYS[1]
local function byKey(a, b)
  local x, y = sortKey(a), sortKey(b)
  if x ~= y then if sortAsc then return x < y else return x > y end end
  return a.row < b.row
end
local view = {}

local function goods(s, h)
  local _, selected = eq.ListInfo(WINDOW, LIST)
  local all = readRows()
  local purse = TC.Purse()
  local needle = search:lower()
  local n = 0
  for _, e in ipairs(all) do
    if (needle == "" or e.lname:find(needle, 1, true)) and (not affordable or e.copper <= purse) then
      n = n + 1
      view[n] = e
    end
  end
  for k = n + 1, #view do view[k] = nil end
  if sortCol and KEYS[sortCol] then
    sortKey = KEYS[sortCol]
    table.sort(view, byKey)
  end
  if #all == 0 then
    local _, usableOnly = W.State(WINDOW, "MW_UsableButton")
    TC.Empty(usableOnly and "Nothing here you can use. Untick Usable only to see everything."
      or "This merchant has nothing for sale.", h)
    return
  end
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.BordersInnerV | ui.TableFlags.Sortable
      | ui.TableFlags.SizingFixedFit
  if not ui.BeginTable("##goods", 4, flags, 0, h) then return end
  ui.TableSetupScrollFreeze(0, 1)
  ui.TableSetupColumn("Item", ui.TableColumnFlags.WidthStretch, 0)
  ui.TableSetupColumn("Qty", ui.TableColumnFlags.WidthFixed, 34 * s)
  ui.TableSetupColumn("Price", ui.TableColumnFlags.WidthFixed, 96 * s)
  ui.TableSetupColumn("Lvl", ui.TableColumnFlags.WidthFixed, 36 * s)
  ui.TableHeadersRow()
  local col, asc = ui.TableGetSortSpecs()
  if col then sortCol, sortAsc = col + 1, asc end
  local _, lh = ui.CalcTextSize("Ag")
  local icon = math.floor(lh + 6 * s)
  for _, e in ipairs(view) do
    ui.TableNextRow()
    ui.TableNextColumn()
    ui.PushID(e.row)
    local x, y = ui.GetCursorScreenPos()
    if ui.Selectable("##row", e.row == selected, ui.SelectableFlags.SpanAllColumns, 0, icon) then
      eq.ListClick(WINDOW, LIST, e.row, "left")
    end
    if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      eq.ListClick(WINDOW, LIST, e.row, "right") -- the item's display
      T.TakeRightClick()
    end
    if ui.IsItemHovered() and not ui.IsMouseDown(ui.Mouse.Right) then
      -- Hailstone: the row's item against what you wear (docs/item-compare.md)
      items.NameTooltip(e.name, eq.CompareSlotItem(WINDOW, LIST .. ":" .. e.row), "Right-click for the item's details")
    end
    if e.tex then ui.DrawImage(e.tex, x, y, x + icon, y + icon, e.uv[1], e.uv[2], e.uv[3], e.uv[4]) end
    local tooMuch = e.copper > purse
    T.Text(x + icon + 6 * s, y + (icon - lh) / 2, e.name, tooMuch and C.muted or C.text, T.font.normal)
    ui.TableNextColumn()
    down((icon - lh) / 2)
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, e.qty)
    ui.TableNextColumn()
    down((icon - lh) / 2)
    TC.Amount(TC.Split(e.copper), tooMuch and C.danger or nil)
    ui.TableNextColumn()
    down((icon - lh) / 2)
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, e.level)
    ui.PopID()
  end
  if #view == 0 then
    ui.TableNextRow()
    ui.TableNextColumn()
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "No goods match.")
  end
  ui.EndTable()
end

-- The item being considered, its price and the buttons that act on it.
local function considered(s, btnH)
  local name = W.Text(WINDOW, "MW_SelectedItemLabel")
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  local size = 40 * s
  local tex, u0, v0, u1, v1 = eq.ChildArt(WINDOW, "MW_SelectedItem", "decal")
  if tex == false then tex = nil end
  ui.DrawRectFilled(x - 4 * s, y - 4 * s, x + w + 4 * s, y + size + 4 * s, C.ink[1], C.ink[2], C.ink[3], 0.035, 7 * s)
  T.Icon(x, y, size, s, tex, u0, v0, u1, v1, nil, { radius = 6 })
  local selling = eq.ChildShown(WINDOW, "MW_Sell_Button") == true
  local tx = x + size + 10 * s
  local bw = 74 * s
  local textW = w - size - 10 * s - 2 * (bw + 6 * s)
  if name == "" then
    T.Text(tx, y + 3 * s, "Nothing selected", C.muted, T.font.normal)
    T.Text(tx, y + 21 * s, T.Fit("Pick an item to buy, or click one in your bags to sell it", textW, T.font.small),
      C.faint, T.font.small)
  else
    T.Text(tx, y + 2 * s, T.Fit(name, textW, T.font.normal), C.text, T.font.normal)
    -- the game's price ("2 platinum, 5 gold") as coins; its own words if they do not read as coins
    local price = W.Text(WINDOW, "MW_SelectedPriceLabel")
    local coins, read = { 0, 0, 0, 0 }, price == ""
    for n, word in price:gmatch("(%d+)%s+(%a+)") do
      local k = ({ platinum = 1, gold = 2, silver = 3, copper = 4 })[word:lower()]
      if k then coins[k], read = tonumber(n), true end
    end
    local caption = selling and "Sells for" or "Costs"
    local cw = T.Text(tx, y + 21 * s, caption, C.muted, T.font.small)
    if read then
      ui.SetCursorScreenPos(tx + cw + 6 * s, y + 21 * s)
      ui.PushFont(T.font.small)
      TC.Amount(coins)
      ui.PopFont()
    else
      T.Text(tx + cw + 6 * s, y + 21 * s, T.Fit(price, textW - cw - 6 * s, T.font.small), C.gold, T.font.small)
    end
  end
  -- the buttons, at the right
  ui.SetCursorScreenPos(x + w - 2 * bw - 6 * s, y + (size - btnH) / 2)
  W.Button(WINDOW, "MW_Inspect_Button", "Inspect", { width = bw, height = btnH, tooltip = "Show the item's details" })
  ui.SameLine(0, 6 * s)
  if selling then
    W.Button(WINDOW, "MW_Sell_Button", "Sell", { width = bw, height = btnH, primary = true,
      tooltip = "Sell it (a stack asks how many)" })
  else
    W.Button(WINDOW, "MW_Buy_Button", "Buy", { width = bw, height = btnH, primary = true,
      tooltip = "Buy it (a stackable item asks how many)" })
  end
  ui.SetCursorScreenPos(x, y + size + 10 * s)
  ui.Dummy(w, 0)
end

local function purchasePage(s, h, btnH)
  -- search and filter
  local fw = T.TextSize("Usable only", T.font.normal) + T.TextSize("Affordable only", T.font.normal) + 68 * s
  ui.SetNextItemWidth(-fw)
  search = ui.InputTextWithHint("##merchantsearch", "Search goods", search, 64)
  ui.SameLine()
  W.Check(WINDOW, "MW_UsableButton", "Usable only", { hidden = false,
    tooltip = "Hide what your class or race cannot use, and spell scrolls you already know" })
  ui.SameLine()
  local _, ch = ui.Checkbox("Affordable only##aff", affordable)
  if ch then affordable = not affordable end
  if ui.IsItemHovered() then ui.SetTooltip("Hide what costs more than you carry") end
  local _, availH = ui.GetContentRegionAvail()
  goods(s, math.max(60 * s, availH - (consideredH or 60 * s)))
  local _, top = ui.GetCursorScreenPos()
  ui.Dummy(0, 2 * s)
  considered(s, btnH)
  local _, bottom = ui.GetCursorScreenPos()
  consideredH = bottom - top
end

-- The parcels waiting here (EQEmu's: eq.Parcels): a row picks one for Retrieve; Claim all takes every one, one after
-- another (Hailstone). Above the game's own Send fields.
local pickedParcel = nil
local function parcels(s, btnH)
  local list, claiming, busy = eq.Parcels()
  list = list or {}
  local _, availH = ui.GetContentRegionAvail()
  local listH = math.max(40 * s, availH - 6 * (btnH + 8 * s) - 16 * s) -- (room for the buttons and the Send fields)
  if #list == 0 then
    TC.Empty("No parcels waiting.", listH)
  else
    ui.BeginChild("##parcels", 0, listH, false)
    local cx, cy = ui.GetCursorScreenPos()
    local cw = ui.GetContentRegionAvail()
    local _, th = T.TextSize("Ag", T.font.small)
    local rh, isz = 38 * s, 30 * s
    local found = false
    for k, p in ipairs(list) do
      local y = cy + (k - 1) * rh
      local it = p.item
      local hit = T.Hit("##parcel" .. k, cx, y, cx + cw, y + rh - 2 * s)
      local hov = ui.IsItemHovered()
      local picked = pickedParcel == p.slot
      if picked then found = true end
      if picked or hov then
        local a = picked and 0.16 or 0.06
        local col = picked and C.accent or C.ink
        ui.DrawRectFilled(cx, y, cx + cw, y + rh - 2 * s, col[1], col[2], col[3], a, 5 * s)
      end
      local tex, u0, v0, u1, v1 = eq.ItemIcon(it.icon)
      T.Icon(cx + 4 * s, y + (rh - isz) / 2 - 1 * s, isz, s, tex, u0, v0, u1, v1, nil, { radius = 5 })
      local nx = cx + isz + 14 * s
      local name = it.name .. ((it.stackable and it.count > 1) and ("  x" .. it.count) or "")
      T.Text(nx, y + 4 * s, T.Fit(name, cw - isz - 24 * s, T.font.small), it.magic and C.magic or C.text, T.font.small)
      local from = (p.from ~= "" and ("From " .. p.from) or "From the server") ..
        (p.sent > 0 and ("  \xC2\xB7  " .. os.date("%b %d", p.sent)) or "")
      T.Text(nx, y + 4 * s + th, T.Fit(from, cw - isz - 24 * s, T.font.tiny), C.faint, T.font.tiny)
      if hov and p.note ~= "" then ui.SetTooltip(p.note) end
      if hit then pickedParcel = p.slot end
    end
    if not found then pickedParcel = nil end
    ui.Dummy(cw, #list * rh)
    ui.EndChild()
  end
  ui.Dummy(0, 2 * s)
  local w = ui.GetContentRegionAvail()
  local bw = (w - 8 * s) / 2
  local x, y = ui.GetCursorScreenPos()
  if T.Button("##retrieve", x, y, x + bw, y + btnH, s, "Retrieve",
      { enabled = pickedParcel ~= nil and not busy, tooltip = "Take the parcel picked above" }) then
    eq.RetrieveParcel(pickedParcel)
  end
  local label = claiming > 0 and string.format("Claiming... %d left", claiming)
    or (#list > 0 and ("Claim all (" .. #list .. ")") or "Claim all")
  if T.Button("##claimall", x + bw + 8 * s, y, x + w, y + btnH, s, label,
      { primary = true, enabled = #list > 0 and claiming == 0, tooltip = "Take every parcel here into your bags" }) then
    eq.ClaimAllParcels()
  end
  ui.Dummy(w, btnH + 6 * s)
end

-- Sending a parcel (Hailstone over EQEmu's OP_ShopSendParcel): an item picked in the bags (a click on it while
-- the merchant is open), or money typed in or picked with the inventory's coin buttons (the game's quantity dialog
-- asks how much; that coin's box takes it); To and Note are the window's boxes. An item's Send is the window's
-- (a stack asks how many); money goes by eq.SendParcelMoney. The server's answer comes in the chat.
local sendMoney, coins, sendNote, moneySerial = false, { "", "", "", "" }, nil, nil
local COIN_NAMES = { "pp", "gp", "sp", "cp" }
local function sendParcel(s, btnH)
  local _, _, _, sending, picked, money = eq.Parcels()
  -- a pick with the inventory's coin buttons: that coin for sending, the Money side shown
  if money and money.serial ~= moneySerial then
    moneySerial = money.serial
    coins[money.coin + 1] = tostring(money.amount)
    sendMoney = true
  end
  TC.Caption("Send a parcel")
  local w = ui.GetContentRegionAvail()
  local x, y = ui.GetCursorScreenPos()
  local segW = 70 * s
  if T.Button("##sendItem", x, y, x + segW, y + btnH, s, "Item", { primary = not sendMoney }) then sendMoney = false end
  if T.Button("##sendMoney", x + segW + 4 * s, y, x + 2 * segW + 4 * s, y + btnH, s, "Money", { primary = sendMoney }) then
    sendMoney = true
  end
  local rx = x + 2 * segW + 14 * s
  if sendMoney then
    local p = { eq.Money() }
    local cw = (x + w - rx - 3 * 6 * s) / 4
    for i = 1, 4 do
      ui.SetCursorScreenPos(rx + (i - 1) * (cw + 6 * s), y + 1 * s)
      ui.SetNextItemWidth(cw)
      local t, changed = ui.InputTextWithHint("##coin" .. i, COIN_NAMES[i] .. " (" .. (p[i] or 0) .. ")", coins[i], 12)
      if changed then coins[i] = t:gsub("%D", "") end
    end
  elseif picked then
    local tex, u0, v0, u1, v1 = eq.ItemIcon(picked.icon)
    local isz = btnH
    T.Icon(rx, y, isz, s, tex, u0, v0, u1, v1, nil, { radius = 4 })
    local name = picked.name .. ((picked.stackable and picked.count > 1) and ("  x" .. picked.count) or "")
    T.Text(rx + isz + 8 * s, y + (btnH - select(2, T.TextSize("Ag"))) / 2, T.Fit(name, x + w - rx - isz - 8 * s), C.text)
  else
    T.Text(rx, y + (btnH - select(2, T.TextSize("Ag", T.font.small))) / 2, "Click an item or coin in your inventory",
      C.faint, T.font.small)
  end
  ui.SetCursorScreenPos(x, y + btnH + 6 * s)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "To")
  ui.SameLine(60 * s)
  local to = W.Edit(WINDOW, "MW_Send_To_Edit", { hint = "The character's name", max = 64 })
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Note")
  ui.SameLine(60 * s)
  local note = W.Edit(WINDOW, "MW_Note_Edit", { hint = "A note with the parcel (optional)", max = 128 })
  ui.Dummy(0, 2 * s)
  local bx, by = ui.GetCursorScreenPos()
  local bw = 120 * s
  if sendMoney then
    local n = {}
    for i = 1, 4 do n[i] = tonumber(coins[i]) or 0 end
    local any = n[1] + n[2] + n[3] + n[4] > 0
    if T.Button("##sendmoneybtn", bx, by, bx + bw, by + btnH, s, sending and "Sending..." or "Send money",
        { primary = true, enabled = any and to ~= "" and not sending, tooltip = "Send this money to " .. (to ~= "" and to or "...") }) then
      sendNote = eq.SendParcelMoney(to, note, n[1], n[2], n[3], n[4])
      if not sendNote then coins = { "", "", "", "" } end
    end
  else
    ui.SetCursorScreenPos(bx, by)
    W.Button(WINDOW, "MW_Send_Button", sending and "Sending..." or "Send",
      { width = bw, height = btnH, primary = true,
        tooltip = picked and ("Send " .. picked.name .. " to " .. (to ~= "" and to or "...")) or "Pick an item in your bags first" })
  end
  if sendNote then
    T.Text(bx + bw + 10 * s, by + (btnH - select(2, T.TextSize("Ag", T.font.small))) / 2, T.Fit(sendNote, w - bw - 10 * s, T.font.small),
      C.danger or C.faint, T.font.small)
  end
  ui.SetCursorScreenPos(bx, by)
  ui.Dummy(w, btnH + 4 * s)
end

local function otherPage(s, btnH, empty, buttons)
  local _, availH = ui.GetContentRegionAvail()
  TC.Empty(empty, math.max(40 * s, availH - btnH - 50 * s))
  W.ButtonRow(WINDOW, buttons, btnH)
end

local PAGES = { { "Buy & Sell", 0 }, { "Recover", 1 }, { "Parcels", 2 } }
local RECOVER = { { "MW_Recover_Button", "Recover" } }
local SELLING_OPTS = { tooltip = "Click to stop" }

local OPTS = {
  size = function() return 440, 520 end,
  resizable = { 340, 320 },
}
local function body(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local page = W.Tabs(WINDOW, "MW_MerchantSubwindows", PAGES)
  local footer = btnH + 12 * s
  ui.BeginChild("##merchantpage", 0, -footer, false)
  if page == 1 then
    otherPage(s, btnH, "Nothing to buy back.", RECOVER)
  elseif page == 2 then
    parcels(s, btnH)
    sendParcel(s, btnH)
  else
    purchasePage(s, h, btnH)
  end
  ui.EndChild()
  -- footer: your purse and Done
  ui.Separator()
  local fx, fy = ui.GetCursorScreenPos()
  local fw = ui.GetContentRegionAvail()
  ui.SetCursorScreenPos(fx, fy + (btnH - lh) / 2)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "You have")
  ui.SameLine()
  local p, g, sv, c = eq.Money()
  TC.Amount({ p, g, sv, c })
  local done, total, mode = eq.ItemJob()
  local junkW = 130 * s
  local jx = fx + fw - 90 * s - 8 * s - junkW
  if done and mode == "sell" then
    if T.Button("##selljunk", jx, fy, jx + junkW, fy + btnH, s, string.format("Selling %d / %d", done, total),
        SELLING_OPTS) then eq.StopItemJob() end
  else
    local n = tools.JunkCarried()
    if T.Button("##selljunk", jx, fy, jx + junkW, fy + btnH, s, n > 0 and ("Sell marked (" .. n .. ")") or "Sell marked",
        { enabled = n > 0, tooltip = tools.Note() or (n > 0 and "Sell every item you marked to sell"
          or "Mark items to sell in the Advanced Inventory first") }) then tools.SellJunk() end
  end
  ui.SetCursorScreenPos(fx + fw - 90 * s, fy)
  W.Button(WINDOW, "MW_Done_Button", "Done", { width = 90 * s, height = btnH })
end

ui.window("Merchant", function()
  if not eq.InGame() then return end
  hud.Dialog(WINDOW, "Merchant", OPTS, body)
end)
