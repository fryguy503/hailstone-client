-- Give: replaces the game's GiveWnd (handing items and coin to an NPC). Who you are giving to, your four item slots
-- as tiles that rewrap to the width, your coin piles, then Give and Cancel.
-- Everything is the game's (hs.SlotItem / hs.ClickChild on GVW_MyItemSlot0..3, GVW_MyMoney0..3, GVW_Give_Button,
-- GVW_Cancel_Button): drop an item from your bags on a slot to add it, click a coin pile with coin on the cursor
-- to add that coin.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local TC = dofile("lib/trade_common.lua")
local T = hud.T
local C = T.color
local WINDOW = "GiveWnd"

local WINDOW_OPTS = {
  size = function() return 290, 210 end,
  resizable = { 200, 190 },
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Giving to")
  ui.SameLine()
  ui.PushFont(T.font.normal)
  local npc = W.Text(WINDOW, "GVW_NPCName")
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, npc ~= "" and npc or "...")
  ui.PopFont()
  ui.Separator()
  ui.BeginChild("##giveitems", 0, -(btnH + 10 * s), false)
  local cw = ui.GetContentRegionAvail()
  local gap = 6 * s
  local size = math.min(52 * s, math.max(36 * s, (cw - 3 * gap) / 4))
  local n = W.SlotGrid(WINDOW, "GVW_MyItemSlot", 0, 3, size, 0, gap)
  local any = false
  for i = 0, 3 do if API.inventory.SlotItem(WINDOW, "GVW_MyItemSlot" .. i) then any = true end end
  if not any and n > 0 then
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Drop items from your bags on the slots.")
  end
  ui.Dummy(0, 4 * s)
  TC.MoneyButtons(WINDOW, { "GVW_MyMoney0", "GVW_MyMoney1", "GVW_MyMoney2", "GVW_MyMoney3" }, s,
    "Pick up coin in your inventory, then click here to add it")
  ui.EndChild()
  ui.Separator()
  W.ButtonRow(WINDOW, {
    { "GVW_Give_Button", "Give", { primary = true, tooltip = "Hand the items and coin over" } },
    { "GVW_Cancel_Button", "Cancel", { tooltip = "Take everything back" } },
  }, btnH)
end

ui.window("Give", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Give", WINDOW_OPTS, drawWindow)
end)
