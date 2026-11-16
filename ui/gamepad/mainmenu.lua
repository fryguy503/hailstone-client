-- The main menu (gamepad pack): Start on the pad (or the HUD's Menu button) opens a grid of tiles, moved through
-- with the D-pad or the left stick, A opens one, B or Start closes the menu. A tile opens its window the way its key
-- or the EQ menu does (lib/actions.lua): this pack's window where the pack has one, else the game's own window,
-- which is then used in cursor mode (the left stick moves the pointer).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local A = dofile("lib/actions.lua")
local AS = dofile("lib/assign.lua")
local C = T.C

-- ---------------------------------------------------------------- pictograms (shapes, no game art)
local function line(x0, y0, x1, y1, c, t) ui.DrawLine(x0, y0, x1, y1, c[1], c[2], c[3], 1, t) end
local P = {}
function P.bag(x, y, s, c, t)
  ui.DrawRect(x - s * 0.7, y - s * 0.35, x + s * 0.7, y + s * 0.85, c[1], c[2], c[3], 1, s * 0.2, t)
  ui.DrawRect(x - s * 0.35, y - s * 0.8, x + s * 0.35, y - s * 0.35, c[1], c[2], c[3], 1, s * 0.15, t)
  line(x - s * 0.7, y + s * 0.2, x + s * 0.7, y + s * 0.2, c, t)
end
function P.book(x, y, s, c, t)
  ui.DrawRect(x - s * 0.8, y - s * 0.7, x - s * 0.04, y + s * 0.75, c[1], c[2], c[3], 1, s * 0.1, t)
  ui.DrawRect(x + s * 0.04, y - s * 0.7, x + s * 0.8, y + s * 0.75, c[1], c[2], c[3], 1, s * 0.1, t)
  for i = 0, 2 do line(x - s * 0.6, y - s * 0.35 + i * s * 0.3, x - s * 0.25, y - s * 0.35 + i * s * 0.3, c, t * 0.6) end
end
function P.sword(x, y, s, c, t)
  line(x - s * 0.7, y + s * 0.7, x + s * 0.7, y - s * 0.7, c, t * 1.3)
  line(x - s * 0.65, y + s * 0.15, x - s * 0.15, y + s * 0.65, c, t)
end
function P.star(x, y, s, c, t)
  local pts = {}
  for i = 0, 9 do
    local a = -math.pi / 2 + i * math.pi / 5
    local r = (i % 2 == 0) and s * 0.85 or s * 0.38
    pts[#pts + 1] = { x + math.cos(a) * r, y + math.sin(a) * r }
  end
  for i = 1, 10 do
    local p, q = pts[i], pts[i % 10 + 1]
    line(p[1], p[2], q[1], q[2], c, t)
  end
end
function P.bars(x, y, s, c, t)
  for i = 0, 2 do
    local h = s * (0.5 + i * 0.4)
    ui.DrawRectFilled(x - s * 0.7 + i * s * 0.5, y + s * 0.75 - h, x - s * 0.35 + i * s * 0.5, y + s * 0.75, c[1], c[2], c[3], 1, s * 0.06)
  end
end
function P.map(x, y, s, c, t)
  local xs = { x - s * 0.8, x - s * 0.27, x + s * 0.27, x + s * 0.8 }
  local ys = { y - s * 0.6, y - s * 0.75, y - s * 0.6, y - s * 0.75 }
  for i = 1, 3 do
    line(xs[i], ys[i], xs[i + 1], ys[i + 1], c, t)
    line(xs[i], ys[i] + s * 1.4, xs[i + 1], ys[i + 1] + s * 1.4, c, t)
  end
  for i = 1, 4 do line(xs[i], ys[i], xs[i], ys[i] + s * 1.4, c, t) end
end
function P.people(x, y, s, c, t)
  ui.DrawCircle(x - s * 0.35, y - s * 0.35, s * 0.28, c[1], c[2], c[3], 1, t)
  ui.DrawCircle(x + s * 0.4, y - s * 0.25, s * 0.24, c[1], c[2], c[3], 1, t)
  ui.DrawRect(x - s * 0.8, y + s * 0.1, x + s * 0.1, y + s * 0.8, c[1], c[2], c[3], 1, s * 0.3, t)
  ui.DrawRect(x + s * 0.05, y + s * 0.15, x + s * 0.8, y + s * 0.75, c[1], c[2], c[3], 1, s * 0.25, t)
end
function P.shield(x, y, s, c, t)
  line(x - s * 0.7, y - s * 0.7, x + s * 0.7, y - s * 0.7, c, t)
  line(x - s * 0.7, y - s * 0.7, x - s * 0.6, y + s * 0.2, c, t)
  line(x + s * 0.7, y - s * 0.7, x + s * 0.6, y + s * 0.2, c, t)
  line(x - s * 0.6, y + s * 0.2, x, y + s * 0.85, c, t)
  line(x + s * 0.6, y + s * 0.2, x, y + s * 0.85, c, t)
end
function P.heart(x, y, s, c, t)
  ui.DrawCircle(x - s * 0.33, y - s * 0.25, s * 0.36, c[1], c[2], c[3], 1, t)
  ui.DrawCircle(x + s * 0.33, y - s * 0.25, s * 0.36, c[1], c[2], c[3], 1, t)
  line(x - s * 0.68, y - s * 0.12, x, y + s * 0.75, c, t)
  line(x + s * 0.68, y - s * 0.12, x, y + s * 0.75, c, t)
end
function P.scroll(x, y, s, c, t)
  ui.DrawRect(x - s * 0.6, y - s * 0.75, x + s * 0.6, y + s * 0.75, c[1], c[2], c[3], 1, s * 0.12, t)
  for i = 0, 3 do line(x - s * 0.35, y - s * 0.4 + i * s * 0.27, x + s * 0.35, y - s * 0.4 + i * s * 0.27, c, t * 0.6) end
end
function P.chat(x, y, s, c, t)
  ui.DrawRect(x - s * 0.8, y - s * 0.65, x + s * 0.8, y + s * 0.4, c[1], c[2], c[3], 1, s * 0.3, t)
  line(x - s * 0.3, y + s * 0.4, x - s * 0.5, y + s * 0.8, c, t)
  line(x - s * 0.5, y + s * 0.8, x + s * 0.05, y + s * 0.4, c, t)
end
function P.gear(x, y, s, c, t)
  ui.DrawCircle(x, y, s * 0.5, c[1], c[2], c[3], 1, t)
  ui.DrawCircle(x, y, s * 0.18, c[1], c[2], c[3], 1, t)
  for i = 0, 7 do
    local a = i * math.pi / 4
    line(x + math.cos(a) * s * 0.5, y + math.sin(a) * s * 0.5, x + math.cos(a) * s * 0.8, y + math.sin(a) * s * 0.8, c, t * 1.3)
  end
end
function P.grid(x, y, s, c, t)
  for i = 0, 1 do
    for j = 0, 1 do
      ui.DrawRect(x - s * 0.75 + i * s * 0.85, y - s * 0.75 + j * s * 0.85, x - s * 0.1 + i * s * 0.85,
        y - s * 0.1 + j * s * 0.85, c[1], c[2], c[3], 1, s * 0.1, t)
    end
  end
end
function P.slots(x, y, s, c, t) -- a row of three hot buttons
  for i = -1, 1 do
    local cx = x + i * s * 0.62
    ui.DrawRect(cx - s * 0.27, y - s * 0.27, cx + s * 0.27, y + s * 0.27, c[1], c[2], c[3], 1, s * 0.08, t)
  end
  line(x - s * 0.9, y + s * 0.6, x + s * 0.9, y + s * 0.6, c, t * 0.6)
end
function P.moon(x, y, s, c, t)
  ui.DrawCircle(x, y, s * 0.7, c[1], c[2], c[3], 1, t)
  ui.DrawCircleFilled(x + s * 0.32, y - s * 0.22, s * 0.6, T.C.raised[1], T.C.raised[2], T.C.raised[3], 1)
end
function P.mail(x, y, s, c, t)
  ui.DrawRect(x - s * 0.8, y - s * 0.55, x + s * 0.8, y + s * 0.6, c[1], c[2], c[3], 1, s * 0.1, t)
  line(x - s * 0.8, y - s * 0.5, x, y + s * 0.1, c, t)
  line(x + s * 0.8, y - s * 0.5, x, y + s * 0.1, c, t)
end
function P.paw(x, y, s, c, t)
  ui.DrawCircle(x, y + s * 0.35, s * 0.38, c[1], c[2], c[3], 1, t)
  for i = 0, 3 do
    local a = math.pi * (1.15 + i * 0.233)
    ui.DrawCircle(x + math.cos(a) * s * 0.72, y + s * 0.2 + math.sin(a) * s * 0.72, s * 0.17, c[1], c[2], c[3], 1, t)
  end
end
function P.crowd(x, y, s, c, t)
  for i = -1, 1 do
    local cx, top = x + i * s * 0.55, i == 0 and -0.15 or 0
    ui.DrawCircle(cx, y + (top - 0.4) * s, s * 0.2, c[1], c[2], c[3], 1, t)
    ui.DrawRect(cx - s * 0.27, y + (top - 0.1) * s, cx + s * 0.27, y + s * 0.75, c[1], c[2], c[3], 1, s * 0.2, t)
  end
end
function P.dots(x, y, s, c, t) -- more
  for i = -1, 1 do ui.DrawCircleFilled(x + i * s * 0.6, y, s * 0.17, c[1], c[2], c[3], 1) end
end
function P.search(x, y, s, c, t)
  ui.DrawCircle(x - s * 0.15, y - s * 0.15, s * 0.5, c[1], c[2], c[3], 1, t)
  line(x + s * 0.22, y + s * 0.22, x + s * 0.75, y + s * 0.75, c, t * 1.4)
end

function P.scales(x, y, s, c, t)
  line(x, y - s * 0.75, x, y + s * 0.7, c, t)
  line(x - s * 0.75, y - s * 0.45, x + s * 0.75, y - s * 0.45, c, t)
  line(x - s * 0.4, y + s * 0.7, x + s * 0.4, y + s * 0.7, c, t)
  for _, d in ipairs({ -1, 1 }) do
    local cx = x + d * s * 0.6
    line(cx, y - s * 0.45, cx - s * 0.3, y + s * 0.1, c, t * 0.7)
    line(cx, y - s * 0.45, cx + s * 0.3, y + s * 0.1, c, t * 0.7)
    line(cx - s * 0.32, y + s * 0.1, cx + s * 0.32, y + s * 0.1, c, t)
  end
end

-- ---------------------------------------------------------------- tiles
-- A window this pack does not have opens as it is: chosen with the pad, the pad's pointer comes up to use it; chosen
-- with the mouse, the mouse stays as it is.
local function cursorMode()
  if pad.navVisible() then pad.set_mode("cursor") end
end

-- { title, pictogram, description, action }; action returns true when it did something.
local TILES = {
  { "Inventory", "bag", "Equipment and bags", function() return A.open("Inventory", "InventoryWindow") end },
  { "Spells", "book", "Spell book and gems", function() return A.open("Spell Book", "SpellBookWnd") end },
  { "Abilities", "sword", "Disciplines, skills, actions", function() -- (abilities.lua) the disciplines where the class has them, else the ABILITIES key's page
    if #API.abilities.Disciplines() > 0 then return A.open("Combat Abilities", "CombatAbilityWnd") end
    API.windows.SetTabPage("ActionsWindow", "ACTW_ActionsSubwindows", 1) API.windows.Show("ActionsWindow", true) return true end },
  { "AA", "star", "Alternate advancement", function() return A.open("Alternate Advancement", "AAWindow") end },
  { "Skills", "bars", "Skill levels", function() return A.open("Skills", "SkillsWindow") end },
  { "Map", "map", "The zone map", function() A.toggle("MapViewWnd") return true end },
  { "Group", "people", "Invite, follow, disband", function() GPSTATE.groupOpen = true return true end },
  { "Pet", "paw", "Pet commands and buffs", function() return GPSTATE.openPet and GPSTATE.openPet() end },
  { "Raid", "crowd", "Raid groups and invites", function() return A.open("Raid", "RaidWindow") end },
  { "Guild", "shield", "Guild management", function() return A.open("Guild", "GuildManagementWnd") end },
  { "Friends", "heart", "Friends and ignore list", function() return A.open("Friends/Ignore", "FriendsWindow") end },
  { "Who", "search", "Find players (/who)", function() GPSTATE.whoOpen = true return true end },
  { "Bazaar", "scales", "Search the traders' wares", function() return A.open("Bazaar Search", "BazaarSearchWnd") end },
  { "Quests", "scroll", "Quest journal", function() return A.open("Quest Journal", "TaskWnd") end },
  { "Mail", "mail", "Inbox and compose", function() A.toggle("MailWindow") return true end },
  { "Chat", "chat", "Chat log and quick chat", function() GPSTATE.chatOpen = true return true end },
  { "Options", "gear", "Gamepad and game settings", function() return A.open("Options", "OptionsWindow") end },
  { "All Windows", "grid", "Every window of the EQ menu", function() GPSTATE.eqMenuOpen = true return true end },
  { "More", "dots", "Help, journal, display", function() GPSTATE.moreOpen = true return true end }, -- (system.lua)
  { "Hot Bars", "slots", "Edit hot buttons, socials", function() return AS.edit("Main Menu") end },
  { "Camp", "moon", "Camp out (sit for 30 s)", function() return A.menu("Camp & Quit") end },
}

-- (wide enough that every row fits a 1080p screen)
local COLS = #TILES > 20 and 6 or 5
local message, messageAt = nil, 0
-- The menu opens on the tile chosen last (ImGui's own memory of the focus depends on which window had it before).
local lastTile, frame, drawnFrame, focusFrames = 1, 0, -10, 0

ui.window("Main Menu", function()
  frame = frame + 1 -- (frames, not seconds: a slow frame must not look like the menu opening again)
  if not API.session.InGame() then
    if GPSTATE.wasInGame then -- left the world (camped): the pad's own windows close
      GPSTATE.menuOpen, GPSTATE.chatOpen, GPSTATE.groupOpen, GPSTATE.eqMenuOpen = false, false, false, false
    end
    GPSTATE.wasInGame = false
    return
  end
  GPSTATE.wasInGame = true
  if GPSTATE.testSlot then -- testing (lib/window.lua test.open)
    GPSTATE.testAt = GPSTATE.testAt or ui.GetTime()
    if ui.GetTime() - GPSTATE.testAt > 8 then
      API.inventory.ClickSlot(GPSTATE.testSlot[1], nil, GPSTATE.testSlot[2])
      GPSTATE.testSlot = nil
    end
  end
  -- Start opens the menu (from the game) and closes it
  if pad.pressed("start") and not pad.down("back") then GPSTATE.menuOpen = not GPSTATE.menuOpen end
  if not GPSTATE.menuOpen then return end
  local tileW, tileH, gap = 240, 150, 18
  local rows = math.ceil(#TILES / COLS)
  local w = COLS * tileW + (COLS - 1) * gap + 48
  local h = rows * tileH + (rows - 1) * gap + 48 + 70 + 66
  local name = API.character.Name() or ""
  if frame - drawnFrame > 1 then focusFrames = 3 end -- (a few frames: the window takes the focus after it appears)
  drawnFrame = frame
  if W.begin("Main Menu", { w = w, h = h, title = "Menu", subtitle = name,
        hints = { { "a", "Open" }, { "b", "Close" }, { "start", "Close" } } }) then
    for i, t in ipairs(TILES) do
      if (i - 1) % COLS ~= 0 then ui.SameLine(0, T.px(gap)) end
      if focusFrames > 0 and i == lastTile then
        ui.SetKeyboardFocusHere()
        pad.showCursor()
      end
      local clicked, focused, x0, y0, x1, y1 = T.button("##tile" .. i, "", T.px(tileW), T.px(tileH), {}, pad)
      if focusFrames > 0 and i == lastTile and ui.IsItemFocused and ui.IsItemFocused() then focusFrames = 0 end -- (landed)
      local col = focused and C.accent or C.text
      P[t[2]](x0 + T.px(48), y0 + T.px(52), T.px(26), col, math.max(1.5, T.px(3)))
      T.font(26)
      local _, th = ui.CalcTextSize("Ag")
      T.draw(x0 + T.px(18), y1 - T.px(18) - th - T.px(24), t[1], focused and C.text or C.text)
      T.unfont()
      T.font(16)
      T.draw(x0 + T.px(18), y1 - T.px(18) - T.px(20), T.fit(t[3], x1 - x0 - T.px(30)), C.dim)
      T.unfont()
      if clicked then
        lastTile = i
        if t[4]() then
          GPSTATE.menuOpen = false
        else
          message, messageAt = t[1] .. ": not available here", ui.GetTime()
        end
      end
    end
    focusFrames = math.max(0, focusFrames - 1)
    if message and ui.GetTime() - messageAt < 3 then
      local x0, _, x1, y1 = W.area()
      T.textIn(x0, y1 - T.px(30), x1, y1, message, C.endurance, 1)
    end
  end
  if W.finish() then GPSTATE.menuOpen = false end
end)
