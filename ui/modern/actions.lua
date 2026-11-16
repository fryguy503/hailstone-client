-- Actions window: replaces the game's ActionsWindow. Its four pages as tabs (the game's own page: the Actions keys
-- that open a page open it here too), each a grid of tiles that reflows to the window's width:
--   * Main: who, invite or follow, disband, camp, sit or stand, run or walk (only the one of each pair that applies
--     now, as the game shows it);
--   * Abilities and Combat: the skills put on the window's buttons (a right click, or a click on an empty one, picks
--     the skill), melee attack lit while auto attack is on;
--   * Socials: the page's twelve socials, with the page arrows.
-- Every tile is the window's button: a click is its click, and dragging one puts its hot button on the cursor to
-- drop on an action bar (hs.PickUp), as on the stock window.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local tiles = dofile("lib/hudx_tiles.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color
local WINDOW = "ActionsWindow"
local TABS = "ACTW_ActionsSubwindows"

local ORD = { "First", "Second", "Third", "Fourth", "Fifth", "Sixth" }
local DRAG = "Drag onto an action bar to make a hot button"

local function hint(text)
  ui.PushFont(T.font.small)
  ui.PushTextWrapPos(0)
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, text)
  ui.PopTextWrapPos()
  ui.PopFont()
end

local function mainTile(id, label, tip) return { id, label, { drag = true, tooltip = tip .. "\n" .. DRAG } } end
local MAIN_TILES = {
  mainTile("AMP_SitButton", "Sit", "Sit down (regenerates faster)"),
  mainTile("AMP_StandButton", "Stand", "Stand up"),
  mainTile("AMP_RunButton", "Run", "Run"),
  mainTile("AMP_WalkButton", "Walk", "Walk"),
  mainTile("AMP_WhoButton", "Who", "Who is in this zone"),
  mainTile("AMP_InviteButton", "Invite", "Invite your target to your group"),
  mainTile("AMP_FollowButton", "Follow", "Accept the group invitation"),
  mainTile("AMP_DisbandButton", "Disband", "Leave your group (or remove your target from it)"),
  mainTile("AMP_FindButton", "Find", "Find merchants, bankers, trainers and more in this zone"),
  mainTile("AMP_EndFindButton", "End Find", "Stop showing the path"),
  mainTile("AMP_CampButton", "Camp", "Sit down and log out"),
}
local MAIN_GRID = { minW = 92, h = 32 }
local ABILITY_GRID = { minW = 120, h = 34 }
local COMBAT_GRID = { minW = 120, h = 40 }
local SOCIAL_GRID = { minW = 96, h = 30 }

local function mainPage(s)
  tiles.Grid(WINDOW, MAIN_TILES, MAIN_GRID)
end

-- A button's tile, kept while the name on it is the same (its tooltip is made from the name). make(id, name) -> tile.
local tileCache = {}
local function cachedTile(id, name, make)
  local c = tileCache[id]
  if not c or c.name ~= name then
    c = { name = name, tile = make(id, name) }
    tileCache[id] = c
  end
  return c.tile
end
local function abilityTile(id, name)
  if name ~= "" then
    return { id, name, { drag = true, right = true, tooltip = name .. "\nRight-click to choose another skill\n" .. DRAG } }
  end
  return { id, "+  Choose skill", { empty = true, right = true,
    tooltip = "An empty button: click to choose a skill for it" } }
end
local function socialTile(id, name)
  if name ~= "" then
    return { id, name, { drag = true, right = true, tooltip = name .. "\n" .. DRAG .. "\nRight-click to edit" } }
  end
  return { id, "", { empty = true, enabled = false, right = true, tooltip = "Right-click to make a social" } }
end

-- The ability buttons' ids by page prefix, and each page's tile list (filled in again every frame).
local ABILITY_IDS = { AAP_ = {}, ACP_ = {} }
for prefix, ids in pairs(ABILITY_IDS) do
  for i = 1, #ORD do ids[i] = prefix .. ORD[i] .. "AbilityButton" end
end
local abilityLists = { AAP_ = {}, ACP_ = {} }

-- The ability buttons of a page: the skill on each, or an empty tile that picks one.
local function abilityTiles(prefix, count)
  local list, ids = abilityLists[prefix], ABILITY_IDS[prefix]
  local filled = 0
  for i = 1, count do
    local id = ids[i]
    local name = W.Text(WINDOW, id)
    if name ~= "" then filled = filled + 1 end
    list[i] = cachedTile(id, name, abilityTile)
  end
  for i = count + 1, #list do list[i] = nil end
  tiles.Grid(WINDOW, list, ABILITY_GRID)
  return filled
end

local function abilitiesPage(s)
  local n = abilityTiles("AAP_", 6)
  ui.Dummy(0, 2 * s)
  hint(n == 0 and "Put your skills (Sneak, Hide, Forage, Begging...) on these buttons." or
    "Right-click a button to change its skill.")
end

local RANGE_TILE = { "ACP_RangeAttackButton", "Range Attack", { drag = true, tooltip = "Fire your ranged weapon\n" .. DRAG } }
local MELEE_TIP = "Turn auto attack on or off\n" .. DRAG
local COMBAT_ON = {
  { "ACP_MeleeAttackButton", "Melee Attack", { drag = true, sub = "On", tooltip = MELEE_TIP } }, RANGE_TILE,
}
local COMBAT_OFF = {
  { "ACP_MeleeAttackButton", "Melee Attack", { drag = true, tooltip = MELEE_TIP } }, RANGE_TILE,
}

local function combatPage(s)
  local _, melee = API.windows.ChildState(WINDOW, "ACP_MeleeAttackButton")
  tiles.Grid(WINDOW, melee and COMBAT_ON or COMBAT_OFF, COMBAT_GRID)
  ui.Dummy(0, 2 * s)
  W.Heading("Combat abilities")
  local n = abilityTiles("ACP_", 4)
  ui.Dummy(0, 2 * s)
  hint(n == 0 and "Put your combat skills (Kick, Bash, Taunt...) on these buttons." or
    "Right-click a button to change its skill.")
end

local SOCIAL_IDS = {}
for i = 1, 12 do SOCIAL_IDS[i] = "ASP_SocialButton" .. i end
local socialList = {}
local PREV_OPTS, NEXT_OPTS = { tooltip = "Previous page" }, { tooltip = "Next page" }
local PAGE_TEXT = {} -- "Page n of 10" by the page label

local function socialsPage(s)
  -- the page arrows and number
  local page = W.Text(WINDOW, "ASP_CurrentSocialPageLabel")
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  local bh, bw = 24 * s, 30 * s
  if T.Button("##socleft", x, y, x + bw, y + bh, s, "<", PREV_OPTS) then
    API.windows.ClickChild(WINDOW, "ASP_SocialPageLeftButton", "left")
  end
  if T.Button("##socright", x + w - bw, y, x + w, y + bh, s, ">", NEXT_OPTS) then
    API.windows.ClickChild(WINDOW, "ASP_SocialPageRightButton", "left")
  end
  local _, th = T.TextSize("Ag", T.font.small)
  local pageText = PAGE_TEXT[page]
  if not pageText then
    pageText = "Page " .. (page ~= "" and page or "1") .. " of 10"
    PAGE_TEXT[page] = pageText
  end
  T.Text(x + w / 2, y + (bh - th) / 2, pageText, C.text, T.font.small, "center")
  ui.SetCursorScreenPos(x, y)
  ui.Dummy(w, bh + 4 * s)
  local filled = 0
  for i = 1, 12 do
    local id = SOCIAL_IDS[i]
    local name = W.Text(WINDOW, id)
    if name ~= "" then filled = filled + 1 end
    socialList[i] = cachedTile(id, name, socialTile)
  end
  if filled == 0 then hint("No socials on this page.") end
  ui.Dummy(0, 2 * s)
  tiles.Grid(WINDOW, socialList, SOCIAL_GRID)
end

local PAGES = { [0] = mainPage, [1] = abilitiesPage, [2] = combatPage, [3] = socialsPage }
local TAB_PAGES = { { "Main", 0 }, { "Abilities", 1 }, { "Combat", 2 }, { "Socials", 3 } }
local testTab = false

local OPTS = {
  size = function() return 300, 290 end,
  resizable = { 200, 180 },
}

local function body(x, y, w, h, s)
  if not testTab then -- testing: test.tab.ActionsWindow.ACTW_ActionsSubwindows=n opens page n once
    testTab = true
    local t = tonumber(ui.GetSetting("test.tab." .. WINDOW .. "." .. TABS) or "")
    if t then API.windows.SetTabPage(WINDOW, TABS, t) end
  end
  local page = W.Tabs(WINDOW, TABS, TAB_PAGES)
  ui.BeginChild("##actionspage", 0, 0, false)
  ui.Dummy(0, 2 * s)
  local draw = PAGES[page] or mainPage
  draw(s)
  ui.EndChild()
end

ui.window("Actions", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Actions", OPTS, body)
end)
