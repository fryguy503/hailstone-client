-- Inspect: replaces the game's InspectWnd (another player's gear; the title is the game's "You are inspecting ...").
-- The worn slots as the inventory's grid (the weapons apart), the gear as a list of slot and item names beside it
-- when the window is wide enough (the inspect answer has names and icons only), and the player's inspect message:
-- yours is editable here, as in the stock window (the game saves it). Done is the window's button.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local items = dofile("lib/items.lua")
local T = hud.T
local C = T.color
local WINDOW = "InspectWnd"

-- The window's slots InvSlot0..22 are the worn slots in hs.Item's numbering (0 charm .. 22 ammo).
local GRID = {
  { 1, 2, 3, 4, 5 },     -- ear, head, face, ear, neck
  { 6, 7, 8, 17, 20 },   -- shoulders, arms, back, chest, waist
  { 9, 10, 12, 15, 16 }, -- wrists, hands, fingers
  { 18, 19, 0, 21, 22 }, -- legs, feet, charm, power source, ammo
}
local WEAPONS = { 13, 14, 11 } -- primary, secondary, range
local SHORT = {
  [0] = "Charm", [1] = "Ear", [2] = "Head", [3] = "Face", [4] = "Ear", [5] = "Neck", [6] = "Shoulder", [7] = "Arms",
  [8] = "Back", [9] = "Wrist", [10] = "Wrist", [11] = "Range", [12] = "Hands", [13] = "Primary", [14] = "Offhand",
  [15] = "Ring", [16] = "Ring", [17] = "Chest", [18] = "Legs", [19] = "Feet", [20] = "Waist", [21] = "Power",
  [22] = "Ammo",
}
-- The list's order: armour top to bottom, then jewellery, then weapons.
local ORDER = { 2, 3, 5, 6, 17, 8, 7, 9, 10, 12, 20, 18, 19, 1, 4, 15, 16, 0, 21, 13, 14, 11, 22 }

local SLOT, GAP = 46, 6
local GRID_W = 5 * SLOT + 4 * GAP

local function slot(n, x, y, size, s)
  ui.SetCursorScreenPos(x, y)
  local id = "InvSlot" .. n
  local item = API.inventory.SlotItem(WINDOW, id)
  items.Slot("##ins" .. n, x, y, size, s, item, SHORT[n], API.inventory.SlotName(n) or SHORT[n],
    function(button, outside) API.windows.ClickChild(WINDOW, id, button, outside) end)
end

local function gearList(w, h, s)
  ui.BeginChild("##gear", w, h, false)
  local filled = 0
  for _, n in ipairs(ORDER) do
    local item = API.inventory.SlotItem(WINDOW, "InvSlot" .. n)
    if item then
      filled = filled + 1
      ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, SHORT[n])
      ui.SameLine(70 * s)
      local c = item.magic and C.magic or C.text
      ui.TextColored(c[1], c[2], c[3], 1, item.name or "")
    end
  end
  if filled == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, ah, "Nothing worn.")
  end
  ui.EndChild()
end

local WINDOW_OPTS = {
  size = function() return 560, 440 end,
  resizable = { GRID_W + 24, 380 },
}
local function drawWindow(x, y, w, h, s)
  S.TestClicks(WINDOW)
  local sz, gap = SLOT * s, GAP * s
  local x0, y0 = ui.GetCursorScreenPos()
  for r, row in ipairs(GRID) do
    for c, n in ipairs(row) do slot(n, x0 + (c - 1) * (sz + gap), y0 + (r - 1) * (sz + gap), sz, s) end
  end
  local gw = GRID_W * s
  local wy = y0 + 4 * (sz + gap) + 10 * s
  ui.DrawLine(x0, wy - 6 * s, x0 + gw, wy - 6 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  local wx = x0 + (gw - (3 * sz + 2 * gap)) / 2
  for k, n in ipairs(WEAPONS) do slot(n, wx + (k - 1) * (sz + gap), wy, sz, s) end
  local gridH = wy + sz - y0
  -- the gear by name, beside the grid when there is room
  local listW = w - gw - 16 * s
  if listW >= 160 * s then
    ui.SetCursorScreenPos(x0 + gw + 16 * s, y0)
    ui.DrawLine(x0 + gw + 8 * s, y0, x0 + gw + 8 * s, y0 + gridH, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
    gearList(listW, gridH, s)
  end
  ui.SetCursorScreenPos(x0, y0 + gridH + 8 * s)
  -- the inspect message (editable for your own)
  local own = API.windows.ChildState(WINDOW, "INSW_Edit") ~= false
  W.Heading(own and "Your inspect message" or "Message")
  local btnH = S.ButtonHeight(s)
  local _, left = ui.GetContentRegionAvail()
  local mh = math.max(40 * s, left - btnH - 8 * s)
  if own then
    W.Edit(WINDOW, "INSW_Edit", { multiline = true, height = mh, max = 254 })
  else
    local text = W.Text(WINDOW, "INSW_Edit")
    ui.BeginChild("##insmsg", 0, mh, false)
    if text == "" then
      ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "No message.")
    else
      ui.PushTextWrapPos(0)
      ui.TextColored(C.text[1], C.text[2], C.text[3], 1, text)
      ui.PopTextWrapPos()
    end
    ui.EndChild()
  end
  W.ButtonRow(WINDOW, { { "DoneButton", "Done" } }, btnH)
end

ui.window("Inspect", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Inspect", WINDOW_OPTS, drawWindow)
end)
