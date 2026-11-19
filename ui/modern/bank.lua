-- Bank: replaces the game's BankWnd and BigBankWnd (the game uses one or the other by the account's bank size: 16
-- or 24 slots). The banker, how many slots are used, a search that dims what does not match (rings the bags holding a
-- match, and dims in the bank bags' windows too) and Stack / Sort of the bank's bags (Hailstone); the bank slots as
-- tiles that rewrap to the window's width; the shared bank's two slots and its platinum; your banked coin; then
-- Auto-Bank, Change and Done.
-- The slots and buttons are the game's (hs.SlotItem / hs.ClickChild on <prefix>BankSlot<n>, SharedBankSlot0..1,
-- Money0..3, SharedMoney0, AutoButton, ChangeButton, DoneButton): click to pick up, drop or swap, right-click a bag
-- to open it, hold the right button to see an item; a coin pile picks the coin up, or takes the coin on the cursor.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local TC = dofile("lib/trade_common.lua")
local tools = dofile("lib/itemtools.lua")
local T = hud.T
local C = T.color

local bottomH = {} -- window -> the height the part under the slots took last frame

-- Over the tile just placed: dimmed when the search does not match it, ringed when it is or holds a match (`found`:
-- hs.FindItems' counts by bank slot).
local function dim(win, id, size, found)
  local x0, y0 = ui.GetItemRect()
  tools.Mark(x0, y0, size, 1, API.inventory.SlotItem(win, id), "bank", found and (found[tonumber(id:match("%d+$"))] or 0) > 0)
end

local function slots(win, prefix, first, last, size, gap, found)
  local avail = ui.GetContentRegionAvail()
  local perRow = math.max(1, math.floor((avail + gap) / (size + gap)))
  local n = 0
  for i = first, last do
    local id = prefix .. i
    if n % perRow ~= 0 then ui.SameLine(0, gap) end
    W.Slot(win, id, size)
    dim(win, id, size, found)
    n = n + 1
  end
end

local function bank(win, p, count, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  -- header: the banker, the slots used, the search
  local used = 0
  for i = 0, count - 1 do if API.inventory.SlotItem(win, p .. "BankSlot" .. i) then used = used + 1 end end
  local banker = W.Text(win, p .. "BankerName")
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, banker ~= "" and banker or "Bank")
  ui.SameLine()
  local cap = string.format("%d / %d slots used", used, count)
  local cw = ui.GetContentRegionAvail()
  local tw = ui.CalcTextSize(cap)
  ui.SetCursorPos(select(1, ui.GetCursorPos()) + cw - tw, select(2, ui.GetCursorPos()))
  ui.TextColored(used >= count and C.danger[1] or C.faint[1], used >= count and C.danger[2] or C.faint[2],
    used >= count and C.danger[3] or C.faint[3], 1, cap)
  ui.SetNextItemWidth(ui.GetContentRegionAvail() - tools.ToolbarWidth(s) - 8 * s)
  tools.state.search.bank = ui.InputTextWithHint("##banksearch", tools.Note() or "Find an item in the bank", tools.state.search.bank, 64)
  local _, sy0, sx1 = ui.GetItemRect()
  tools.Toolbar("##bank", sx1 + 8 * s + tools.ToolbarWidth(s), sy0, s, "bank")
  local text = tools.Search("bank")
  local found = text ~= "" and select(2, API.inventory.FindItems("bank", text)) or nil

  -- the slots fill what is left above the shared bank, the coin and the buttons
  ui.BeginChild("##bankslots", 0, -(bottomH[win] or 200 * s), false)
  local gap = 6 * s
  slots(win, p .. "BankSlot", 0, count - 1, 44 * s, gap, found)
  ui.EndChild()
  local _, top = ui.GetCursorScreenPos()

  W.Heading("Shared bank")
  ui.BeginGroup()
  slots(win, p .. "SharedBankSlot", 0, 1, 40 * s, gap)
  ui.EndGroup()
  ui.SameLine(0, 14 * s)
  ui.BeginGroup()
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Shared platinum")
  TC.MoneyButton(win, p .. "SharedMoney0", 1, s, 110 * s,
    "Shared with every character of your account.\nClick to pick it up, or to put the platinum on your cursor here")
  ui.EndGroup()
  ui.Dummy(0, 2 * s)
  TC.MoneyButtons(win, { p .. "Money0", p .. "Money1", p .. "Money2", p .. "Money3" }, s,
    "Banked coin. Click to pick it up (Ctrl: one coin), or to bank the coin on your cursor")
  ui.Dummy(0, 2 * s)
  ui.Separator()
  W.ButtonRow(win, {
    { "AutoButton", "Auto-Bank", { primary = true,
      tooltip = "Bank what is on your cursor: coin, or the item (onto a matching stack, a free slot or a bag)" } },
    { "Hailstone_AutoInventory", "Auto-Inventory",
      { tooltip = "Put the item on your cursor into your inventory: a free slot or a bag" } },
    { "ChangeButton", "Change", { tooltip = "Exchange banked coin for larger coins" } },
    { "DoneButton", "Done" },
  }, btnH)
  if API.windows.ChildShown(win, "AltStorageButton") == true then
    W.Button(win, "AltStorageButton", "Shroud Bank", { height = btnH })
  end
  local _, bottom = ui.GetCursorScreenPos()
  bottomH[win] = bottom - top
end

for _, b in ipairs({ { "BankWnd", "BW_", 16, "Bank" }, { "BigBankWnd", "BIGB_", 24, "Bank" } }) do
  local win, p, count, title = b[1], b[2], b[3], b[4]
  -- (each bank's options and body made once, not every frame)
  local opts = {
    size = function() return 420, 400 end,
    resizable = { 260, 360 },
  }
  local function body(x, y, w, h, s) bank(win, p, count, s) end
  ui.window(b[1] == "BankWnd" and "Bank" or "Big Bank", function()
    if not API.session.InGame() then return end
    hud.Dialog(win, title, opts, body)
  end)
end
