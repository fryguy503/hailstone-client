-- Raid Options (gamepad pack): stands in for RaidOptionsWindow (the raid window's Loot options). The current loot
-- type and what it means, Set the loot type (the window's drop-down: a list of the four types; the game enables it
-- for the raid leader and sends the pick), the looters as the game lists them (the leader, then the group leaders or
-- the looters the raid window's Add / Remove Looter set, or the entire raid), and the sixteen class buttons: each
-- opens the colour picker, and the colour picked is that class's in the raid window.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local C = T.C
local WIN = "RaidOptionsWindow"

-- The class buttons RAIDOPTIONS_ClassColor0..15, in the stock window's order.
local CLASSES = { "Bard", "Beastlord", "Cleric", "Druid", "Enchanter", "Magician", "Monk", "Necromancer", "Paladin",
  "Ranger", "Rogue", "Shadow Knight", "Shaman", "Warrior", "Wizard", "Berserker" }
-- their class ids
local CLASS_IDS = { 8, 15, 2, 6, 14, 13, 7, 11, 3, 4, 9, 5, 10, 1, 12, 16 }
-- A class's name as the client shows it (a server's own where its plugin named the class), else `fallback`.
local HAS_CLASS_NAMES = API.Supports and API.Supports("character", "ClassName") == true
local function className(id, fallback, short)
  return HAS_CLASS_NAMES and API.character.ClassName(id, short) or fallback
end
-- (the game shows the type as its number; the drop-down's choices are the XML's)
local TYPES = { "Leader Only", "Leader and Group Leaders", "Leader Selected", "Entire Raid" }
local MEANING = {
  ["Leader Only"] = "Only the raid leader loots.",
  ["Leader and Group Leaders"] = "The raid leader and the group leaders loot.",
  ["Leader Selected"] = "The raid leader and the players on the looter list loot.",
  ["Entire Raid"] = "Everyone in the raid loots.",
}

local st = CH.new()
local looters = {}
local wasShown = false

ui.window("Raid Options", function()
  CH.testOpen(WIN)
  if not W.shown(WIN) then
    st.pick, wasShown = nil, false
    return
  end
  T.frame()
  local appearing = not wasShown
  wasShown = true
  local current = TYPES[tonumber(CH.text(WIN, "RAIDOPTIONS_CurrentLootType")) or 0] or ""
  if W.begin("Raid Options", { w = 1500, h = 940, title = "Raid Options", subtitle = "Loot and class colours",
        hints = CH.hints(st, { { "a", "Select" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(st, x0, y0, x1, y1) then
      local leftW = cw * 0.45
      -- the loot type
      CH.caption(x0, y0, "Loot type")
      T.font(28)
      T.draw(x0, y0 + T.px(26), current ~= "" and current or "-", C.gold)
      T.unfont()
      T.font(19)
      T.draw(x0, y0 + T.px(66), MEANING[current] or "", C.dim)
      T.unfont()
      ui.SetCursorPos(x0 - wx, y0 + T.px(104) - wy)
      if appearing then ui.SetKeyboardFocusHere() end
      CH.combo(st, WIN, "RAIDOPTIONS_SetLootTypeCombo", CH.enabled(WIN, "RAIDOPTIONS_SetLootTypeCombo")
        and "Set the loot type" or "Set the loot type (the raid leader sets it)", leftW, T.px(84), { keep = true })
      -- the looters
      local ly = y0 + T.px(210)
      CH.caption(x0, ly, "Looters")
      ui.SetCursorPos(x0 - wx, ly + T.px(30) - wy)
      CH.list(WIN, "RAIDOPTIONS_LooterList", looters, leftW, y1 - ly - T.px(30), { rowH = 54,
        empty = "No looters: select a raid member in the raid window and Add Looter.", onA = function() end })
      -- the class colours: two columns of buttons, each name in its colour
      local rx0 = x0 + leftW + T.px(30)
      local rw = x1 - rx0
      CH.caption(rx0, y0, "Class colours in the raid window")
      local bw = (rw - T.px(10)) / 2
      local bh = T.px(70)
      for i = 0, 15 do
        local id = "RAIDOPTIONS_ClassColor" .. i
        ui.SetCursorPos(rx0 + (i % 2) * (bw + T.px(10)) - wx, y0 + T.px(30) + (i // 2) * (bh + T.px(8)) - wy)
        local clicked, _, bx0, by0, bx1, by1 = T.button("##" .. id, "", bw, bh, {}, pad)
        local r, g, b = API.windows.ChildColor(WIN, id)
        T.rect(bx0 + T.px(14), by0 + T.px(18), bx0 + T.px(40), by1 - T.px(18), { r or 1, g or 1, b or 1, 1 }, 1, T.px(6))
        T.font(21)
        T.textIn(bx0 + T.px(54), by0, bx1 - T.px(10), by1, className(CLASS_IDS[i + 1], CLASSES[i + 1]), { r or 1, g or 1, b or 1, 1 }, 0)
        T.unfont()
        if clicked then API.windows.ClickChild(WIN, id, "left") end
      end
    end
  end
  if CH.back(st, W.finish()) then API.windows.Show(WIN, false) end
end)
