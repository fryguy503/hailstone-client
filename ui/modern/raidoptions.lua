-- Raid options: replaces the game's RaidOptionsWindow (the raid window's Options button). The current loot
-- type, the drop-down that sets it (the game enables it for the raid leader; picking a type is the window's own
-- choice, which sends it), the looters as the game lists them (the leader, then the group leaders or the looters
-- set from the raid window's Add / Remove Looter, or the entire raid), and the sixteen class buttons: each opens
-- the colour picker, and the colour picked is that class's in the raid window (kept in the UI ini).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local WINDOW = "RaidOptionsWindow"

-- What each of the game's loot types means, for the tooltip of the drop-down.
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

local WINDOW_OPTS = {
  size = function() return 360, 480 end,
  resizable = { 240, 220 },
  keepTitle = true,
}
local function drawWindow(x, y, w, h, s)
  S.TestClicks(WINDOW)
  local current = TYPES[tonumber(W.Text(WINDOW, "RAIDOPTIONS_CurrentLootType")) or 0] or ""
  W.Field("Loot type", current ~= "" and current or "-", C.gold)
  if MEANING[current] then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, MEANING[current]) end
  W.Heading("Set the loot type")
  W.Combo(WINDOW, "RAIDOPTIONS_SetLootTypeCombo")
  if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
    local lines = {}
    for _, k in ipairs(TYPES) do
      lines[#lines + 1] = k .. ": " .. MEANING[k]
    end
    if API.windows.ChildState(WINDOW, "RAIDOPTIONS_SetLootTypeCombo") == false then lines[#lines + 1] = "(The raid leader sets it.)" end
    ui.SetTooltip(table.concat(lines, "\n"))
  end
  W.Heading("Looters")
  local classH = 4 * (S.ButtonHeight(s) + 4 * s) + 34 * s -- the class colours below
  local rows = API.windows.ListInfo(WINDOW, "RAIDOPTIONS_LooterList") or 0
  if rows == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(30 * s, ah - classH), "No looters.", "Select a raid member and press Add Looter.")
  else
    W.List(WINDOW, "RAIDOPTIONS_LooterList", 0, -classH, { columns = { [1] = { "", 0 } } })
  end
  -- the class colours: four buttons a row, each in its colour; a click opens the colour picker
  W.Heading("Class colours in the raid window")
  local aw = ui.GetContentRegionAvail()
  local bw = (aw - 3 * 6 * s) / 4
  for i = 0, 15 do
    local id = "RAIDOPTIONS_ClassColor" .. i
    if i % 4 ~= 0 then ui.SameLine(0, 6 * s) end
    local r, g, b = API.windows.ChildColor(WINDOW, id)
    ui.PushStyleColor(ui.Col.Text, r or 1, g or 1, b or 1, 1)
    W.Button(WINDOW, id, className(CLASS_IDS[i + 1], CLASSES[i + 1]), { width = bw, tooltip = "Choose this class's colour" })
    ui.PopStyleColor(1)
  end
end

ui.window("Raid Options", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Raid Options", WINDOW_OPTS, drawWindow)
end)
