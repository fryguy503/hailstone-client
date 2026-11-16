-- "Close with Esc": a window's right-click menu item (hud.esc.<window>). The key bound to the client's
-- CLOSE_TOP_WINDOW (Escape unless rebound) then closes the frontmost window that has it ticked: the one opened or
-- clicked last. One press closes one window. It is ticked from the start for the windows in DEFAULT_ON (the ones
-- opened to look at something and put away again), off until ticked for the rest; unticking one saves "0".
--
-- A Hailstone choice, per window: the original's escapable windows are not traced (docs/unknowns.md).
--
--   local esc = dofile("lib/escape.lua")
--   -- after the window's Begin, every frame it is shown:
--   if esc.Frame(key) then <close it> end
--   -- in its right-click menu:
--   esc.MenuItem(key)

-- Shared by every window script (each dofile makes its own copy of this file).
local API = hs.UI
modern = modern or { windows = {}, resetAll = false }
local shared = modern
shared.esc = shared.esc or { seen = {}, front = {}, frame = -1 }
local S = shared.esc

local E = {}

local function settingKey(key) return "hud.esc." .. key end

-- Ticked until the player unticks them: the windows opened to look at something and put away again. The inventory,
-- the AA window, the ability and spell books, every bag (inventory, bank and world containers), the zone map (not the
-- minimap, which is a frame of the HUD), the item and spell displays, and the reference and social windows. Not the
-- ones whose closing does something in the game (merchant, loot, trade, give, tradeskill: closing ends or cancels
-- it) nor prompts (a press would answer them).
local DEFAULT_ON = {}
for _, key in ipairs({
  "InventoryWindow", "AAWindow", "AbilityBookWnd", "SpellBookWnd", "MapViewWnd", "WorldContainer",
  "InspectWnd", "SkillsWindow", "TaskWnd", "GuildManagementWnd", "FriendsWindow", "BankWnd", "BigBankWnd",
  "ScriptsWnd", "HelpWindow", "BookWindow", "NoteWindow", "TributeBenefitWnd", "TributeTrophyWnd",
  "LeadershipWindow", "TitleWnd", "TrackingWnd", "MailWindow", "GroupSearchWnd", "BazaarSearchWnd",
  "BarterSearchWnd",
}) do DEFAULT_ON[key] = true end
for i = 1, 36 do DEFAULT_ON["Bag" .. i] = true end
DEFAULT_ON.ItemDisplayWindow, DEFAULT_ON.SpellDisplayWindow = true, true
for i = 2, 6 do
  DEFAULT_ON["ItemDisplayWindow#" .. i], DEFAULT_ON["SpellDisplayWindow#" .. i] = true, true
end

function E.On(key)
  local v = ui.GetSetting(settingKey(key))
  if v == nil or v == "" then return DEFAULT_ON[key] == true end
  return v == "1"
end

function E.MenuItem(key)
  local on = E.On(key)
  if ui.MenuItem("Close with Esc", on) then ui.SetSetting(settingKey(key), on and "0" or "1") end
  if ui.IsItemHovered() then ui.SetTooltip("Esc closes this window (the one opened or clicked last first)") end
end

-- The window `key` this frame, called after its Begin: notes it shown (and in front when it has just opened or was
-- clicked) and returns true when the key closes it now.
function E.Frame(key)
  local frame, now = ui.GetFrameCount(), ui.GetTime()
  local last = S.seen[key]
  if last == nil or frame - last > 1 then S.front[key] = now end -- (opened again: in front)
  S.seen[key] = frame
  if ui.IsWindowHovered(ui.HoveredFlags.ChildWindows)
      and (ui.IsMouseClicked(ui.Mouse.Left) or ui.IsMouseClicked(ui.Mouse.Right)) then
    S.front[key] = now
  end
  -- The first window drawn each frame asks for the key; the choice is among the windows shown this frame and the
  -- last (a window drawn later this frame was last seen in the last one).
  if S.frame ~= frame then
    S.frame = frame
    if API.menus.TakeCloseTopWindow and API.menus.TakeCloseTopWindow() then
      local best, bestAt
      for k, f in pairs(S.seen) do
        if frame - f <= 1 and E.On(k) and (bestAt == nil or S.front[k] > bestAt) then best, bestAt = k, S.front[k] end
      end
      S.target, S.at = best, frame
    end
  end
  if S.target == key and frame - S.at <= 1 then
    S.target = nil
    return true
  end
  return false
end

return E
