-- Expeditions (gamepad pack), standing in for the stock windows and answered through them (lib/model.lua: the
-- original's DZ_* / DZSwitchList_* children, app/lua_ui_adventure.cpp), so who may be added and the head count are
-- the game's:
--   Expedition (DynamicZoneWnd)        the expedition you are in (its name, leader and head count), its members (A
--                                      selects one for the leader's buttons), Quit (asked first), the leader's tools
--                                      while you lead it (the target, Add, Remove, Make Leader, Invite Raid, Invite
--                                      Task: shown only to the leader, as the game shows DZLeaderSection), and your
--                                      lockouts on the right
--   Choose an Expedition               a door or switch that leads into more than one of your expeditions: A picks
--   (DynamicZoneSwitchListWnd)         one, Y (or Enter) goes in as OK does; B is Cancel
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local C = T.C

-- ---------------------------------------------------------------- the expedition (DynamicZoneWnd)
local DZ = "DynamicZoneWnd"
local members, timers, state = {}, {}, {}

ui.window("Expedition", function()
  if not W.shown(DZ) then return end
  T.frame()
  local name = M.text(DZ, "DZ_CurrentDZValue")
  if W.begin("Expedition", { w = 1640, h = 960, title = "Expedition", subtitle = name ~= "" and name or "Not in an expedition",
        hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not W.confirm(state) then
      local midX = x0 + (x1 - x0) * 0.52
      local lx1 = midX - T.px(24)
      local leader = M.shown(DZ, "DZLeaderSection") and API.windows.ChildShown(DZ, "DZLeaderSection") == true
      local bh = T.px(70)
      local y = M.facts({ { "Leader", M.text(DZ, "DZ_LeaderValue") },
        { "Players", M.text(DZ, "DZ_NumPlayersValue") }, { "Most players", M.text(DZ, "DZ_MaxPlayersValue") } },
        x0, lx1, y0) + T.px(16)
      ui.SetCursorPos(x0 - wx, y - wy)
      T.heading("Members")
      local ly = select(2, ui.GetCursorScreenPos())
      -- under the members: Quit, then the leader's tools (two rows of buttons and the target)
      local foot = bh + T.px(14) + (leader and (2 * bh + T.px(10) + T.px(54)) or 0)
      M.list(DZ, "DZ_MemberList", members, x0, ly, lx1, y1 - foot, { empty = "You are not in an expedition." })
      local fy = y1 - foot + T.px(14)
      ui.SetCursorPos(x0 - wx, fy - wy)
      local enabled = M.state(DZ, "DZ_Quit")
      if T.button("##dzquit", "Quit Expedition", lx1 - x0, bh, { disabled = not enabled }, pad) then
        state.confirm = { "Leave the expedition \"" .. name .. "\"?", function() API.windows.ClickChild(DZ, "DZ_Quit", "left") end }
      end
      if leader then
        local target = M.text(DZ, "DZ_Leader_TargetValue")
        local ty = fy + bh + T.px(14)
        T.font(19)
        T.draw(x0, ty, "LEADER   Target: " .. (target ~= "" and target or "none (target a player to add them)"),
          target ~= "" and C.text or C.dim)
        T.unfont()
        ui.SetCursorPos(x0 - wx, ty + T.px(40) - wy)
        M.buttons(DZ, { { "DZ_Leader_Add", "Add", { primary = true } }, { "DZ_Leader_Remove", "Remove" },
          { "DZ_Leader_MakeLeader", "Make Leader" } }, x0, lx1, bh)
        M.buttons(DZ, { { "DZ_Leader_InviteRaid", "Invite Raid" }, { "DZ_Leader_InviteTask", "Invite Task" } }, x0, lx1, bh)
      end
      -- the lockouts
      ui.SetCursorPos(midX - wx, y0 - wy)
      T.heading("Lockouts")
      local ty = select(2, ui.GetCursorScreenPos())
      M.list(DZ, "DZ_TimerList", timers, midX, ty, x1, y1,
        { empty = "No lockouts: expeditions you cannot enter again yet are listed here." })
    end
  end
  if W.back(W.finish(), state, nil, members) then API.windows.Show(DZ, false) end
end)

-- ---------------------------------------------------------------- the expedition choice (DynamicZoneSwitchListWnd)
local SW = "DynamicZoneSwitchListWnd"
local choices = {}

ui.window("Choose an Expedition", function()
  if not W.shown(SW) then return end
  T.frame()
  if W.begin("Choose an Expedition", { w = 1100, h = 760, title = "Choose an Expedition", alwaysFocus = true,
        subtitle = "This way leads into more than one expedition",
        hints = { { "a", "Select" }, { "y", "Enter" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(72)
    local function go() API.windows.ClickChild(SW, "DZSwitchList_OK", "left") end
    M.list(SW, "DZSwitchList_List", choices, x0, y0, x1, y1 - bh - T.px(14), {
      empty = "No expeditions to enter here.",
      activate = function(i)
        API.windows.ListClick(SW, "DZSwitchList_List", i - 1, "left")
        go()
      end,
    })
    if ui.IsKeyPressed and ui.Key and ui.Key.Enter and ui.IsKeyPressed(ui.Key.Enter) and W.focused() then go() end
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    M.buttons(SW, { { "DZSwitchList_OK", "Enter", { primary = true, glyph = "y" } }, { "DZSwitchList_Cancel", "Cancel" } }, x0, x1, bh)
  end
  if W.finish() then API.windows.ClickChild(SW, "DZSwitchList_Cancel", "left") end
end)
