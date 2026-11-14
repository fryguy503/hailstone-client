-- Fellowship (gamepad pack): stands in for FellowshipWnd (the EQ menu's Fellowship). Its two pages, LB / RB:
--   Fellowship   the members (name, level, class, zone, last on, experience sharing; A selects one), the message of
--                the day (the keyboard types it; Update sends it) and the window's buttons: Create, Invite (your
--                target), Make Leader, Remove, Toggle XP, Leave, End Fellowship, enabled as the game has them (the
--                leader's are the leader's).
--   Camp         the current campsite, Create / Destroy Camp, the campsite kits (Refresh List asks for them, Show All
--                Possible, the kind filter) and the selected kit's description.
-- Every list, field and button is the stock window's (app/lua_eq_fellowship.cpp), so the requests are the game's.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local TB = dofile("lib/textbox.lua")
local C = T.C
local WIN = "FellowshipWnd"

local st = CH.new()
local page = tonumber(ui.GetSetting("test.tab.FellowshipWnd.Subwindows") or "") == 1 and 2 or 1
local members, kits, desc = {}, {}, {}
local side = {}
local wasShown = false

local function setPage(p)
  page = p
  eq.SetTabPage(WIN, "Subwindows", p - 1)
  if p == 1 then members.focusNext = 1 else kits.focusNext = 1 end
end

ui.window("Fellowship", function()
  CH.testOpen(WIN)
  if not W.shown(WIN) then
    st.kb.open, st.pick, wasShown = false, nil, false
    return
  end
  T.frame()
  if not wasShown then setPage(page) end
  wasShown = true
  local count = eq.ListInfo(WIN, "MemberList") or 0
  if W.begin("Fellowship", { w = 1700, h = 960, title = "Fellowship", subtitle = page == 1
          and (count .. (count == 1 and " member" or " members") .. " | L marks the leader") or "Campsite",
        hints = CH.hints(st, { { "lb", "Fellowship" }, { "rb", "Camp" }, { "a", "Select" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(st, x0, y0, x1, y1) then
      if W.pressed("lb") and page ~= 1 then setPage(1) end
      if W.pressed("rb") and page ~= 2 then setPage(2) end
      local listW = cw * 0.64
      local rx0 = x0 + listW + T.px(24)
      local rw = x1 - rx0
      local bh = T.px(68)
      local buttons
      if page == 1 then
        local fh = T.px(80)
        ui.SetCursorPos(x0 - wx, y0 - wy)
        CH.field(st, WIN, "MOTDInput", "Message of the day", listW - T.px(190), fh, { max = 1023, keep = true })
        ui.SameLine(0, T.px(10))
        CH.button(WIN, "UpdateMOTDButton", "Update", T.px(180), fh, { keep = true })
        T.font(17)
        T.draw(x0, y0 + fh + T.px(10), CH.plain(WIN, "FellowshipVitality"), C.dim)
        T.unfont()
        ui.SetCursorPos(x0 - wx, y0 + fh + T.px(45) - wy)
        local row = CH.list(WIN, "MemberList", members, listW, y1 - y0 - fh - T.px(45), { rowH = 56, font = 19,
          empty = "You are in no fellowship: Create starts one.",
          columns = { { 1, 0.22 }, { 2, 0.08, 1 }, { 3, 0.12 }, { 4, 0.06 }, { 5, 0.22 }, { 6, 0.15 }, { 7, 0.15 } } })
        W.toSide(side, row)
        buttons = {
          { "CreateFellowshipButton", "Create", "Start a fellowship" },
          { "InviteMemberButton", "Invite", "Your target" },
          { "MakeLeaderButton", "Make Leader", "The selected member" },
          { "RemoveMemberButton", "Remove", "The selected member" },
          { "ToggleExpSharingButton", "Toggle XP", "Sharing for the selected member" },
          { "LeaveFellowshipButton", "Leave", "Leave the fellowship" },
          { "EndFellowshipButton", "End Fellowship", "For everyone" },
        }
      else
        T.font(19)
        T.draw(x0, y0, CH.plain(WIN, "CurrentCampLoc"), C.dim)
        local viewer = CH.plain(WIN, "CampsiteViewer")
        T.draw(x0, y0 + T.px(28), viewer ~= "" and viewer or "No campsite.", viewer ~= "" and C.text or C.faint)
        T.draw(x0, y0 + T.px(56), CH.plain(WIN, "CampsiteTime"), C.dim)
        T.unfont()
        local fy = y0 + T.px(100)
        ui.SetCursorPos(x0 - wx, fy - wy)
        local half = (listW - T.px(10)) / 2
        CH.combo(st, WIN, "CampsiteFilter", "Kind", half, T.px(76), { keep = true })
        ui.SameLine(0, T.px(10))
        CH.toggle(WIN, "ShowAllButton", "Show All Possible", half, T.px(76), { keep = true })
        local ly = fy + T.px(90)
        local listH = (y1 - ly) * 0.6
        ui.SetCursorPos(x0 - wx, ly - wy)
        local row = CH.list(WIN, "CampsiteKitList", kits, listW, listH, { rowH = 52, font = 19,
          empty = "No campsite kits: Refresh List asks for them.", columns = { { 2, 1 } } })
        W.toSide(side, row)
        TB.draw(eq.WindowText(WIN, "CampsiteDescription") or "", x0, ly + listH + T.px(14), x0 + listW, y1, desc)
        buttons = {
          { "CreateCampsite", "Create Camp", "Here" },
          { "DestroyCampsite", "Destroy Camp", "The current one" },
          { "RefreshListButton", "Refresh List", "The campsite kits" },
        }
      end
      local by = y0
      for k, b in ipairs(buttons) do
        ui.SetCursorPos(rx0 - wx, by - wy)
        W.sideBefore(side, k)
        CH.button(WIN, b[1], b[2], rw, bh, { sub = b[3], align = 0, keep = true, primary = k == 1 })
        W.sideAfter(side, k, #buttons, page == 1 and members or kits)
        by = by + bh + T.px(10)
      end
    end
  end
  if CH.back(st, W.finish()) then eq.ShowWindow(WIN, false) end
end)
