-- Loot window: replaces the game's LootWnd. The corpse's items as tiles that reflow to the window's width (only the
-- filled slots: the stock window's 34 places are the corpse's slots, which keep their numbers), Loot All, Link All
-- and Done.
-- The slots are the game's (hs.SlotItem / hs.ClickChild): left click puts the item on the cursor, a right click
-- loots it straight into the inventory, holding the right button shows it.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color
local WINDOW = "LootWnd"
local SLOTS = 34
local SLOT_IDS = {}
for i = 0, SLOTS - 1 do SLOT_IDS[i] = "LW_LootSlot" .. i end
local BUTTONS = {
  { "LootAllButton", "Loot All", { primary = true, tooltip = "Loot every item into your inventory" } },
  { "BroadcastButton", "Link All", { tooltip = "Put every item's link into the chat input" } },
  { "DoneButton", "Done" },
}
local filledIds = {} -- this frame's filled slots, in order (one pass over the 34 slots, not two)

local OPTS = {
  size = function() return 330, 230 end,
  resizable = { 220, 150 },
  place = function(W_, H, w, h, s, x, y) return x, y end,
}
local function body(x, y, w, h, s)
  local corpse = W.Text(WINDOW, "LW_CorpseName")
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, corpse ~= "" and corpse or "Corpse")
  ui.PopFont()
  local filled = 0
  for i = 0, SLOTS - 1 do
    local id = SLOT_IDS[i]
    if API.inventory.SlotItem(WINDOW, id) then
      filled = filled + 1
      filledIds[filled] = id
    end
  end
  for k = filled + 1, #filledIds do filledIds[k] = nil end
  ui.SameLine()
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, filled == 1 and "1 item" or (filled .. " items"))
  ui.Separator()
  -- the items, in a scrolled area that leaves room for the buttons
  local _, btnH = ui.CalcTextSize("Ag")
  btnH = btnH + 10 * s
  ui.BeginChild("##lootitems", 0, -(btnH + 8 * s), false)
  local size = 44 * s
  local gap = 6 * s
  local avail = ui.GetContentRegionAvail()
  local perRow = math.max(1, math.floor((avail + gap) / (size + gap)))
  local n = 0
  for k = 1, filled do
    if n % perRow ~= 0 then ui.SameLine(0, gap) end
    W.Slot(WINDOW, filledIds[k], size)
    n = n + 1
  end
  if n == 0 then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Nothing left to loot.") end
  ui.EndChild()
  W.ButtonRow(WINDOW, BUTTONS, btnH)
end

ui.window("Loot", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Loot", OPTS, body)
end)
