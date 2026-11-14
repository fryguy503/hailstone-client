-- Raid (gamepad pack): stands in for RaidWindow (the raid key, the EQ menu's Raid; an invitation opens it too). The
-- raid's buttons along the top (accept or decline an invitation, invite the target, leave, lock, the loot options,
-- the message of the day), the members by group on the left, and the leader's actions for the picked member on the
-- right: open their member menu, send a tell, move them to a group, or set their note (shown in their row).
-- Right-clicking a member or pressing Y on one opens that same menu. Each is the stock window's own button, shown
-- and enabled as the game
-- has it (eq.ChildShown, eq.ChildState); a note goes through the window's note list, note input and Update.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local C = T.C

local RW = "RaidWindow"
local listState = {}
local wasShown = false
local state = {} -- the question (W.confirm)
local kb = K.new()
local side = {} -- W.toSide
local pickFocused, pickAt = nil, 0
local picking = nil -- the member "Move to a group" picks a group for (a 12-button picker over the window)
local kbNote = nil -- the member the keyboard types a note for (nil: the message of the day)

local function shown(child) return eq.ChildShown(RW, child) ~= false end
local function enabled(child) return eq.ChildState(RW, child) ~= false end

-- By group (none last), then by name.
local function byGroup(a, b)
  local ga, gb = a.group or 99, b.group or 99
  if ga ~= gb then return ga < gb end
  return a.name < b.name
end

-- Both stock lists (in groups, then not in one) as one: { list, row (0-based), group, name, level, class, rank }.
local function readMembers()
  local out = {}
  for _, l in ipairs({ "RAID_PlayerList", "RAID_NotInGroupPlayerList" }) do
    local rows = eq.ListInfo(RW, l)
    for i = 0, (rows or 0) - 1 do
      local r = eq.ListRow(RW, l, i)
      if r then
        out[#out + 1] = { list = l, row = i, group = tonumber(r[1] or ""), name = r[2] or "", level = r[3] or "",
          class = r[4] or "", role = r[5] or "", rank = r[6] or "" }
      end
    end
  end
  -- the members' notes (the note list: group, name, note)
  local notes = {}
  for i = 0, (eq.ListInfo(RW, "RAID_NoteList") or 0) - 1 do
    local r = eq.ListRow(RW, "RAID_NoteList", i)
    if r and r[2] then notes[r[2]] = { row = i, note = r[3] or "" } end
  end
  for _, m in ipairs(out) do
    local n = notes[m.name]
    m.note, m.noteRow = n and n.note or "", n and n.row
  end
  table.sort(out, byGroup)
  return out
end

-- The members, read again every 0.25 s, when either list's row count changes, and when the window opens: reading
-- them is a row at a time from the stock lists. (Who is selected is read every frame, below.)
local REFRESH = 0.25
local memberList, membersAt, memberRows = {}, nil, -1
local function members(rowsIn, rowsOut)
  local now = ui.GetTime()
  local rows = (rowsIn or 0) * 1000 + (rowsOut or 0)
  if not membersAt or now - membersAt >= REFRESH or now < membersAt or rows ~= memberRows then
    memberList, membersAt, memberRows = readMembers(), now, rows
  end
  return memberList
end

-- The message of the day without its markup, on one line, shortened to `w` (kept while the text and the size are).
local motdRaw, motdW, motdS, motdPlain, motdFit = nil, nil, nil, "", ""
local function motdLine(w)
  local raw = eq.WindowText(RW, "MOTDViewer") or ""
  if raw ~= motdRaw then
    motdRaw, motdW = raw, nil
    motdPlain = (raw:gsub("<[^>]*>", ""):gsub("%s+", " "))
  end
  if w ~= motdW or T.S ~= motdS then
    motdW, motdS = w, T.S
    motdFit = T.fit("MOTD: " .. (motdPlain ~= "" and motdPlain or "(none)"), w)
  end
  return motdPlain, motdFit
end

-- Resolve the named member in the current stock lists before selecting them or opening their menu: cached row
-- numbers can have changed. The picker also acts on the member it was opened for, even if selection moved.
local function selectMember(name, button)
  for _, l in ipairs({ "RAID_PlayerList", "RAID_NotInGroupPlayerList" }) do
    local rows, _, sel = eq.ListInfo(RW, l)
    for i = 0, (rows or 0) - 1 do
      local r = eq.ListRow(RW, l, i)
      if r and r[2] == name then
        if button == "right" or sel ~= i then eq.ListClick(RW, l, i, button or "left") end
        return true
      end
    end
  end
  return false
end

local function targetName()
  local _, name, raw = eq.Gauge(6)
  if raw == -1 or not name or name == "" then return nil end
  return name
end

-- Testing: the pack's settings file test.open = ...,raid opens it once, 6 s after
-- zoning in (closed first when the game restored it, so it takes the pad as a window opened in play does).
-- (the setting does not change in play: once it is read without raid, the hook stays off, as M.testShow does)
local testAt, testOff
local function testOpen()
  if testOff or GPSTATE["test_raid"] or not eq.InGame() then return end
  if not (","..(ui.GetSetting("test.open") or "")..","):find(",raid,", 1, true) then
    testOff = true
    return
  end
  testAt = testAt or ui.GetTime()
  if ui.GetTime() - testAt > 6 then
    GPSTATE["test_raid"] = true
    eq.ShowWindow(RW, true)
  elseif ui.GetTime() - testAt > 5 then
    eq.ShowWindow(RW, false)
  end
end

ui.window("Raid", function()
  testOpen()
  if not W.shown(RW) then
    kb.open, state.confirm = false, nil
    picking, pickFocused = nil, nil -- (the picker is for the member picked while it was open)
    wasShown = false
    membersAt = nil -- (read afresh when it opens)
    return
  end
  T.frame()
  -- the pad's focus starts on the list
  if not wasShown then listState.focusNext = 1 end
  wasShown = true
  local rowsIn, _, selIn = eq.ListInfo(RW, "RAID_PlayerList")
  local rowsOut, _, selOut = eq.ListInfo(RW, "RAID_NotInGroupPlayerList")
  local list = members(rowsIn, rowsOut)
  local selected
  for i, m in ipairs(list) do
    local sel = m.list == "RAID_PlayerList" and selIn or selOut
    if sel == m.row then selected = i end
  end
  local count = eq.WindowText(RW, "RAID_PlayerCountLabel") or "0"
  local avg = eq.WindowText(RW, "RAID_LevelAverageLabel") or "0"
  local inRaid = #list > 0
  local contextMenus = eq.ContextMenus() ~= false
  local hints = { { "a", "Select" }, { "b", (state.confirm or picking) and "Cancel" or "Close" } }
  if contextMenus and inRaid and not state.confirm and not picking then table.insert(hints, 2, { "y", "Member actions" }) end
  if W.begin("Raid", { w = 1720, h = 960, title = "Raid",
        subtitle = inRaid and (count .. " players, average level " .. avg) or "Not in a raid",
        hints = kb.open and K.HINTS or hints }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if state.confirm then
      W.confirm(state)
    elseif picking then
      -- the twelve groups (the stock RAID_Group1Button .. RAID_Group12Button), 4 x 3
      T.font(24)
      T.draw(x0, y0, "Move " .. picking .. " to group", C.text)
      T.unfont()
      local gw, gh = (cw - 3 * T.px(14)) / 4, T.px(110)
      for g = 1, 12 do
        if (g - 1) % 4 ~= 0 then ui.SameLine(0, T.px(14)) else ui.SetCursorPos(x0 - wx, y0 + T.px(56) + (g - 1) // 4 * (gh + T.px(14)) - wy) end
        if g == 1 and picking ~= pickFocused then
          ui.SetKeyboardFocusHere()
          pickFocused = picking
        end
        local child = "RAID_Group" .. g .. "Button"
        -- (not in the first moment: the A that opened the picker is still down)
        if T.button("##g" .. g, "Group " .. g, gw, gh, { disabled = not enabled(child) }, pad) and ui.GetTime() - pickAt > 0.4 then
          if selectMember(picking) then eq.ClickChild(RW, child, "left") end
          picking, pickFocused = nil, nil
          listState.focusNext = 1
        end
      end
    else
      -- the raid's buttons
      local target = targetName()
      local top = {}
      if shown("RAID_AcceptButton") then top[#top + 1] = { "RAID_AcceptButton", "Accept", "Join the raid", true } end
      if shown("RAID_DeclineButton") then top[#top + 1] = { "RAID_DeclineButton", "Decline", "Turn it down", true } end
      if shown("RAID_InviteButton") then top[#top + 1] = { "RAID_InviteButton", "Invite", target or "your target" } end
      if shown("RAID_DisbandButton") and inRaid then
        top[#top + 1] = { "RAID_DisbandButton", "Leave", "Leave the raid", false, "Leave the raid?" }
      end
      if inRaid and shown("RAID_LockButton") then top[#top + 1] = { "RAID_LockButton", "Lock", "No more invitations" } end
      if inRaid and shown("RAID_UnlockButton") then top[#top + 1] = { "RAID_UnlockButton", "Unlock", "Allow invitations" } end
      if inRaid then top[#top + 1] = { "RAID_OptionsButton", "Loot options", "Loot type, looters", false, nil, true } end
      if inRaid then top[#top + 1] = { "##motd", "Set MOTD", "Message of the day" } end
      if inRaid then
        top[#top + 1] = { "RAID_InviteExpedButton", "Invite Expedition", "Expedition members to raid" }
        top[#top + 1] = { "RAID_InviteTaskButton", "Invite Task", "Shared task members to raid" }
      end
      local cols = 5
      local topRows = math.max(1, math.ceil(#top / cols))
      local bw = (cw - (cols - 1) * T.px(10)) / cols
      local bh = T.px(84)
      for i, b in ipairs(top) do
        if (i - 1) % cols ~= 0 then ui.SameLine(0, T.px(10))
        else ui.SetCursorPos(x0 - wx, y0 + ((i - 1) // cols) * (bh + T.px(10)) - wy) end
        local child = b[1]
        local on = child == "##motd" and enabled("UpdateMOTDButton") or (child ~= "##motd" and enabled(child))
        if T.button("##top" .. i, b[2], bw, bh, { sub = b[3], align = 0, disabled = not on, primary = b[4] == true }, pad) then
          if child == "##motd" then
            local motd = (eq.WindowText(RW, "MOTDViewer") or ""):gsub("<[^>]*>", "")
            kbNote = nil
            K.open(kb, "The raid's message of the day", motd, 255)
          elseif b[5] then
            state.confirm = { b[5], function() eq.ClickChild(RW, child, "left") end }
          else
            eq.ClickChild(RW, child, "left")
            if b[6] and pad.navVisible() then pad.set_mode("cursor") end -- the raid options are not a pad window
          end
        end
      end
      local ly = y0 + topRows * (bh + T.px(10)) + T.px(6)
      -- the message of the day
      if inRaid then
        T.font(19)
        local motd, line = motdLine(cw * 0.6)
        T.draw(x0, ly, line, motd ~= "" and C.text or C.faint)
        T.unfont()
        ly = ly + T.px(38)
      end
      local listW = cw * 0.6
      ui.SetCursorPos(x0 - wx, ly - wy)
      if kb.open then
        local r = K.draw(kb, pad, x0, ly, math.min(cw, T.px(1100)))
        if r == "ok" and kbNote then
          -- the member's note row selected, its text in the note input, then Update
          for _, m in ipairs(list) do
            if m.name == kbNote and m.noteRow then
              eq.ListClick(RW, "RAID_NoteList", m.noteRow, "left")
              eq.SetWindowText(RW, "NoteEdit", kb.text)
              eq.ClickChild(RW, "NoteUpdateButton", "left")
            end
          end
        elseif r == "ok" then
          eq.SetWindowText(RW, "MOTDInput", kb.text)
          eq.ClickChild(RW, "UpdateMOTDButton", "left")
        end
      elseif not inRaid then
        T.draw(x0, ly + T.px(10), "You are not in a raid. Target a player and Invite to start one.", C.dim)
        ui.Dummy(1, 1)
      else
        listState.selected = selected
        local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(56) + T.px(10))))
        listState.shown = visible
        local focusedRow = W.list("raid", listState, #list, visible, 56, listW, function(i, rx0, ry0, rx1, ry1, focused)
          local m = list[i]
          if contextMenus and ((ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt) or (focused and W.pressed("y"))) then
            selectMember(m.name, "right")
          end
          local w = rx1 - rx0
          local col = i == selected and C.accent or C.text
          -- the group as a badge, alternate groups tinted so they read as blocks
          local gx1 = rx0 + T.px(70)
          T.rect(rx0 + T.px(8), ry0 + T.px(8), gx1, ry1 - T.px(8), m.group and ((m.group % 2 == 0) and C.mana or C.exp) or C.sunken,
            m.group and 0.35 or 1, T.px(8))
          T.font(20)
          T.textIn(rx0 + T.px(8), ry0, gx1, ry1, m.group and ("G" .. m.group) or "-", C.text, 0.5)
          T.unfont()
          T.font(22)
          T.textIn(gx1 + T.px(14), ry0, rx0 + w * 0.42, ry1, m.name, col, 0)
          T.textIn(rx0 + w * 0.42, ry0, rx0 + w * 0.72, ry1, m.level .. " " .. m.class, col, 0)
          local role = (m.role:find("A", 1, true) and "MA " or "") .. (m.role:find("M", 1, true) and "MK " or "")
          local rank = role .. m.rank
          if m.note ~= "" then rank = (rank ~= "" and (rank .. "  ") or "") .. "\"" .. m.note .. "\"" end
          T.textIn(rx0 + w * 0.72, ry0, rx1 - T.px(12), ry1, T.fit(rank, rx1 - T.px(12) - rx0 - w * 0.72), C.dim, 0)
          T.unfont()
        end, function(i) selectMember(list[i].name) end)
        W.toSide(side, focusedRow)
        -- The picked member's menu and the shortcuts that use the pack's picker or keyboard.
        local rx0 = x0 + listW + T.px(30)
        local rw = x1 - rx0
        local m = selected and list[selected]
        T.font(18)
        -- (the heading on the MOTD's line: the buttons start level with the first row, so D-pad down from it
        -- goes down the list, not across; ImGui scores a button just below a row's middle as "down")
        T.draw(rx0, ly - T.px(38), m and (m.name:upper() .. (m.group and ("  GROUP " .. m.group) or "  NO GROUP")) or "SELECT A MEMBER ON THE LEFT", C.dim)
        T.unfont()
        ui.SetCursorPos(rx0 - wx, ly - wy)
        ui.BeginChild("##raidactions", rw, y1 - ly, false)
        local ax, ay = ui.GetCursorScreenPos()
        local actionW = ui.GetContentRegionAvail()
        local ry = ay
        local abh = T.px(72)
        local who = m and m.name or ""
        local actions = {
          { "##memberactions", "Member actions", "Leadership, marking, looters and groups", m ~= nil and enabled("RAID_MemberActionsButton"),
            function() if selectMember(who) then eq.ClickChild(RW, "RAID_MemberActionsButton", "left") end end },
          { "##tell", "Send a tell", "Opens the chat with /tell", m ~= nil, function() W.chat("/tell " .. who .. " ") end },
          -- (the game moves members only in a locked raid, CRaidWnd::UpdateButtons: for the leader of an unlocked
          -- one this locks it, and the picker's groups light up once the lock is back)
          { "##move", "Move to a group",
            enabled("RAID_Group1Button") and "Pick one of the 12" or (enabled("RAID_LockButton") and "Locks the raid first" or ""),
            m ~= nil and (enabled("RAID_Group1Button") or enabled("RAID_LockButton")), function()
              if not enabled("RAID_Group1Button") then eq.ClickChild(RW, "RAID_LockButton", "left") end
              picking, pickAt = who, ui.GetTime()
            end },
          -- (the leader's: the window shows the MOTD's Update to the leader only)
          { "##note", "Set note", m and m.note ~= "" and m.note or "Seen by the whole raid",
            m ~= nil and m.noteRow ~= nil and shown("UpdateMOTDButton"),
            function()
              kbNote = who
              K.open(kb, "Note for " .. who, m.note, 63)
            end },
        }
        if not contextMenus then
          -- Turning off context menus keeps the same native actions reachable as direct controls.
          table.remove(actions, 1)
          local function memberButton(child)
            return function() if selectMember(who) then eq.ClickChild(RW, child, "left") end end
          end
          local direct = {
            { "##assist", "Main Assist", "Delegate to the selected member", m ~= nil and enabled("RAID_MainAssistButton"),
              memberButton("RAID_MainAssistButton") },
            { "##marker", "Mark NPC", "Delegate target marking", m ~= nil and enabled("RAID_MarkNPCButton"),
              memberButton("RAID_MarkNPCButton") },
            { "##findpc", "Find PC", "Find a path to this member", m ~= nil and enabled("RAID_FindPCButton"),
              memberButton("RAID_FindPCButton") },
            { "##nogroup", "Out of the groups", "Not in any group", m ~= nil and enabled("RAID_NoGroupButton"),
              memberButton("RAID_NoGroupButton") },
            { "##leader", "Make raid leader", "Hand the raid over", m ~= nil and enabled("RAID_MakeLeaderButton"), function()
              state.confirm = { "Make " .. who .. " the raid leader?", memberButton("RAID_MakeLeaderButton") }
            end },
            { "##removeleader", "Remove group leader", "Remove this member's group leadership", m ~= nil and enabled("RAID_RemoveLeaderButton"),
              memberButton("RAID_RemoveLeaderButton") },
            { "##addlooter", "Add looter", "May loot for the raid", m ~= nil and enabled("RAID_AddLooterButton"),
              memberButton("RAID_AddLooterButton") },
            { "##removelooter", "Remove looter", "", m ~= nil and enabled("RAID_RemoveLooterButton"),
              memberButton("RAID_RemoveLooterButton") },
          }
          for i, action in ipairs(direct) do table.insert(actions, i, action) end
        end
        for k, a in ipairs(actions) do
          ui.SetCursorScreenPos(ax, ry)
          W.sideBefore(side, k)
          if T.button(a[1], a[2], actionW, abh, { sub = a[3] ~= "" and a[3] or nil, disabled = not a[4], align = 0 }, pad) then a[5]() end
          W.sideAfter(side, k, #actions, listState)
          ry = ry + abh + T.px(10)
        end
        ui.EndChild()
      end
    end
  end
  local close = W.finish()
  if close and picking then
    close, picking, pickFocused = false, nil, nil
    listState.focusNext = 1
  end -- B leaves the picker
  if W.back(close, state, kb, listState) then eq.ShowWindow(RW, false) end
end)
