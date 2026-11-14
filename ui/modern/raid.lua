-- Raid: replaces the game's RaidWindow. The raid as twelve group cards side by side (they reflow to the window's
-- width), the players in no group in a strip above them, a summary line (players, average level, the leader,
-- locked or open) with a search that picks players out, member actions in a right-click menu, and raid-wide buttons
-- under the cards. An invitation
-- shows as a banner with Accept and Decline.
--
-- Everything goes through the game's window: the members are its two lists (RAID_PlayerList, the grouped players,
-- and RAID_NotInGroupPlayerList), a click on a member selects that row of its list (so Make Leader, the looter
-- buttons and the group moves act on it), and moving a player is the window's group buttons: "Move here" on a
-- card, or dragging a member onto a card, selects the member and presses that group's button (RAID_Group1Button
-- .. RAID_Group12Button, RAID_NoGroupButton for no group).
--
-- The game enables the group buttons for the leader of a locked raid only (CRaidWnd::UpdateButtons). So that the
-- raid leader can always drag, a move asked for while the raid is unlocked presses Lock first and makes the move
-- once the lock has come back from the server; a strip over the cards says so and holds Lock / Unlock. A drop on a
-- full group (six) is refused here rather than sent (EQEmu ignores it). Leadership and invitation controls follow
-- the window's permissions and ability ranks. The Notes page has the members' notes and the message of the day.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local WINDOW = "RaidWindow"
local GROUPS, PER_GROUP = 12, 6
local GROUPED, UNGROUPED = "RAID_PlayerList", "RAID_NotInGroupPlayerList"
local TEST_CLICKS = "test.click." .. WINDOW
local PENDING_FOR = 5 -- seconds a move waits for the lock it asked for

local search = ""
local drag = nil -- the member being dragged: { name, class, group, classColor }
local pending = {} -- moves waiting for the raid's lock: { name, group, at }
local bodyFrame = -1 -- the frame the members page or the notes last drew

-- The raid as the window's lists give it: members by group (1..12), the ungrouped, the selected name, the leader.
local function readRaid()
  local raid = { groups = {}, ungrouped = {}, count = 0, leader = nil, selected = nil }
  for g = 1, GROUPS do raid.groups[g] = {} end
  local function add(list, r, sel)
    -- columns: group, name, level, class, role, raid rank
    local t = r.texts
    local m = { list = list, row = r.row, group = tonumber(t[1] or ""), name = t[2] or "", level = t[3] or "",
      class = t[4] or "", role = t[5] or "", rank = t[6] or "" }
    -- the class colour picked in Raid Options (the game's class cell colour); white (the default) keeps this pack's
    local cc = t.colors and t.colors[4]
    if cc and not (cc[1] >= 0.999 and cc[2] >= 0.999 and cc[3] >= 0.999) then m.classColor = cc end
    if m.rank == "Raid Leader" then raid.leader = m.name end
    if r.row == sel then raid.selected = m end
    raid.count = raid.count + 1
    return m
  end
  local rows, sel = S.Rows(WINDOW, GROUPED)
  for _, r in ipairs(rows) do
    local m = add(GROUPED, r, sel)
    local g = m.group
    if g and raid.groups[g] then table.insert(raid.groups[g], m) else table.insert(raid.ungrouped, m) end
  end
  rows, sel = S.Rows(WINDOW, UNGROUPED)
  for _, r in ipairs(rows) do table.insert(raid.ungrouped, add(UNGROUPED, r, sel)) end
  return raid
end

-- The raid leader (the window shows the MOTD's Update to the leader only), and whether the game takes moves now
-- (its group buttons: the leader of a locked raid).
local function isLeader() return eq.ChildShown(WINDOW, "UpdateMOTDButton") == true end
local function movesOpen() return eq.ChildState(WINDOW, "RAID_Group1Button") == true end

-- The group button for card g (0: no group).
local function groupButton(g) return g == 0 and "RAID_NoGroupButton" or ("RAID_Group" .. g .. "Button") end

-- The member named `name` in the lists as read this frame (a drag keeps the name: the rows may have moved since).
local function findMember(raid, name)
  for _, m in ipairs(raid.ungrouped) do
    if m.name == name then return m end
  end
  for g = 1, GROUPS do
    for _, m in ipairs(raid.groups[g]) do
      if m.name == name then return m end
    end
  end
  return nil
end

-- Whether group g (0: none) has room for a member now in `from`.
local function hasRoom(raid, g, from)
  return g == 0 or (from or 0) == g or #raid.groups[g] < PER_GROUP
end

-- Select the member in the game's list, then press the group's button (the game checks who may move whom).
local function moveTo(m, g)
  eq.ListClick(WINDOW, m.list, m.row, "left")
  local id = groupButton(g)
  if eq.ChildState(WINDOW, id) ~= false then eq.ClickChild(WINDOW, id, "left") end
end

-- A move the player asked for: made now in a locked raid; in an unlocked one, the raid is locked first and the move
-- waits for the lock (made by flushPending).
local function requestMove(raid, m, g)
  if (m.group or 0) == g or not hasRoom(raid, g, m.group) then return end
  if movesOpen() then
    moveTo(m, g)
  elseif isLeader() and eq.ChildState(WINDOW, "RAID_LockButton") == true then
    for i = #pending, 1, -1 do
      if pending[i].name == m.name then table.remove(pending, i) end -- (the last ask for a member wins)
    end
    if #pending == 0 then eq.ClickChild(WINDOW, "RAID_LockButton", "left") end
    pending[#pending + 1] = { name = m.name, group = g, at = ui.GetTime() }
  end
end

local function flushPending(raid)
  if #pending == 0 then return end
  local now = ui.GetTime()
  if movesOpen() then
    local moves = pending
    pending = {}
    for _, p in ipairs(moves) do
      local m = findMember(raid, p.name) -- (gone from the raid meanwhile: nothing)
      if m and (m.group or 0) ~= p.group and hasRoom(raid, p.group, m.group) then moveTo(m, p.group) end
    end
  elseif not isLeader() or now - pending[1].at > PENDING_FOR or now < pending[1].at then
    pending = {} -- the lock did not come (or the lead went elsewhere)
  end
end

local function pendingFor(name)
  for _, p in ipairs(pending) do
    if p.name == name then return p end
  end
  return nil
end

local function matches(m, needle)
  if needle == "" then return true end
  return m.name:lower():find(needle, 1, true) or m.class:lower():find(needle, 1, true)
    or S.ClassAbbr(m.class):lower():find(needle, 1, true)
end

-- The member's note (the note list's third column), read only for a tooltip.
local function noteOf(name)
  local rows = eq.ListInfo(WINDOW, "RAID_NoteList") or 0
  for r = 0, rows - 1 do
    local t = eq.ListRow(WINDOW, "RAID_NoteList", r)
    if t and t[2] == name then return t[3] or "" end
  end
  return ""
end

-- A small rounded tag (RL, GL, MA, MK) ending at x1; returns its width with its gap.
local function tag(x1, y, text, col, s, a)
  local cw, ch = T.TextSize(text, T.font.tiny)
  local x0 = x1 - cw - 8 * s
  ui.DrawRectFilled(x0, y, x1, y + ch + 1 * s, col[1], col[2], col[3], 0.18 * a, 6 * s)
  T.Text(x0 + 4 * s, y, text, col, T.font.tiny, nil, a)
  return cw + 10 * s
end

-- The height of a member line.
local function lineHeight(s)
  local _, th = T.TextSize("Ag", T.font.small)
  return th + 8 * s
end

-- A member line inside a card: a class stripe, the name, the rank and role tags, level and class at the right.
-- Returns the line's height.
local function memberLine(m, x, y, w, s, raid, needle, drags)
  local _, th = T.TextSize("Ag", T.font.small)
  local lh = lineHeight(s)
  local bh = lh - 2 * s -- (the backing leaves a gap between lines)
  local selected = raid.selected and raid.selected.name == m.name
  local hit = T.Hit("##m" .. m.list .. m.row, x, y, x + w, y + bh)
  local hovered = ui.IsItemHovered()
  local dragged = drag and drag.name == m.name
  local waiting = pendingFor(m.name)
  local dim = (needle ~= "" and not matches(m, needle)) or dragged or waiting
  local cc = m.classColor or S.ClassColor(m.class)
  local ink = C.ink
  -- the line's own backing, so a card reads as its slots
  ui.DrawRectFilled(x, y, x + w, y + bh, ink[1], ink[2], ink[3], 0.035, 4 * s)
  if selected then
    ui.DrawRectFilled(x, y, x + w, y + bh, C.accent[1], C.accent[2], C.accent[3], 0.20, 4 * s)
    ui.DrawRect(x, y, x + w, y + bh, C.accent[1], C.accent[2], C.accent[3], 0.7, 4 * s, 1)
  elseif needle ~= "" and not dim then
    ui.DrawRectFilled(x, y, x + w, y + bh, C.gold[1], C.gold[2], C.gold[3], 0.14, 4 * s)
  elseif hovered and not drag then
    ui.DrawRectFilled(x, y, x + w, y + bh, ink[1], ink[2], ink[3], 0.06, 4 * s)
  end
  local a = dim and 0.35 or 1
  ui.DrawRectFilled(x + 3 * s, y + 4 * s, x + 6 * s, y + bh - 4 * s, cc[1], cc[2], cc[3], 0.9 * a, 1)
  local ty = y + (bh - th) / 2
  local right = T.Text(x + w - 6 * s, ty, S.ClassAbbr(m.class), cc, T.font.small, "right", a)
  local lvW = T.Text(x + w - 12 * s - right, ty, m.level, C.muted, T.font.small, "right", a)
  local tx = x + w - 18 * s - right - lvW
  local _, tth = T.TextSize("Ag", T.font.tiny)
  local tagY = y + (bh - tth) / 2
  if m.rank == "Raid Leader" then tx = tx - tag(tx, tagY, "RL", C.gold, s, a)
  elseif m.rank == "Group Leader" then tx = tx - tag(tx, tagY, "GL", C.accent, s, a) end
  -- the Role column: "A" a main assist, "M" a marker (CRaidWnd_SetMemberAssistMarkerFlags)
  if m.role:find("A", 1, true) then tx = tx - tag(tx, tagY, "MA", C.danger, s, a) end
  if m.role:find("M", 1, true) then tx = tx - tag(tx, tagY, "MK", C.magic or C.accent, s, a) end
  local nameW = tx - (x + 12 * s) - 4 * s
  T.Text(x + 12 * s, ty, T.Fit(m.name, nameW, T.font.small), C.text, T.font.small, nil, a)

  -- a click selects the row in the game's list; right-click is the list's own; a drag carries the member
  if hit and not drag then eq.ListClick(WINDOW, m.list, m.row, "left") end
  if not drag and ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
    eq.ListClick(WINDOW, m.list, m.row, "right")
    T.TakeRightClick()
  end
  if drags and ui.IsItemActive() and ui.IsMouseDragging(ui.Mouse.Left) and not drag then
    drag = { name = m.name, class = m.class, group = m.group, classColor = m.classColor }
  end
  if hovered and not drag then
    local where = m.group and ("Group " .. m.group) or "Not in a group"
    local lines = { m.name, "Level " .. m.level .. " " .. m.class, where }
    if m.rank ~= "" then lines[#lines + 1] = m.rank end
    if m.role:find("A", 1, true) then lines[#lines + 1] = "Raid main assist" end
    if m.role:find("M", 1, true) then lines[#lines + 1] = "Raid marker" end
    local note = noteOf(m.name)
    if note ~= "" then lines[#lines + 1] = "Note: " .. note end
    if waiting then lines[#lines + 1] = "Moving once the raid is locked..." end
    if eq.ContextMenus() then lines[#lines + 1] = "Right-click for member actions" end
    if drags then lines[#lines + 1] = "Drag onto another group to move" end
    ui.SetTooltip(table.concat(lines, "\n"))
  end
  return lh
end

-- An empty slot in a card (the lines under its members): a faint outline, lit as a drop target while dragging.
local function emptySlot(x, y, w, s, lit)
  local lh = lineHeight(s)
  local bh = lh - 2 * s
  local ink = C.ink
  if lit then
    ui.DrawRectFilled(x, y, x + w, y + bh, C.accent[1], C.accent[2], C.accent[3], 0.08, 4 * s)
    ui.DrawRect(x, y, x + w, y + bh, C.accent[1], C.accent[2], C.accent[3], 0.35, 4 * s, 1)
  else
    ui.DrawRect(x, y, x + w, y + bh, ink[1], ink[2], ink[3], 0.06, 4 * s, 1)
  end
  return lh
end

-- The "Move here" button ending at x1 in a card's header, while the card is hovered and the selected member is
-- elsewhere (for the raid leader: in an unlocked raid the button locks it first). Returns whether it drew.
local function moveHere(id, x1, y, s, raid, g, rect)
  local sel = raid.selected
  if not sel or drag or not isLeader() then return false end
  if not ui.IsMouseHoveringRect(rect[1], rect[2], rect[3], rect[4]) then return false end
  if (sel.group or 0) == g or not hasRoom(raid, g, sel.group) then return false end
  local tw = T.TextSize("Move here", T.font.tiny)
  local bw, bh = tw + 14 * s, 18 * s
  local tip = "Move " .. sel.name .. " here"
  if not movesOpen() then tip = tip .. " (locks the raid first: the game moves players only in a locked raid)" end
  if T.Button(id, x1 - bw, y, x1, y + bh, s, "Move here", { primary = true, tooltip = tip }) then
    requestMove(raid, sel, g)
  end
  return true
end

-- The six pips of a group's head count, ending at x1, centred on y.
local function pips(x1, y, n, s)
  local r = 2.5 * s
  local step = 7 * s
  local c = n >= PER_GROUP and C.gold or C.accent
  for i = 1, PER_GROUP do
    local cx = x1 - (PER_GROUP - i) * step - r
    if i <= n then ui.DrawCircleFilled(cx, y, r, c[1], c[2], c[3], 0.9)
    else ui.DrawCircle(cx, y, r, C.ink[1], C.ink[2], C.ink[3], 0.25, 1) end
  end
end

-- The glow of a card under a drag: the accent where the member can go, the danger colour on a full group.
local function dropGlow(raid, g, x, y, w, h)
  if not drag or not ui.IsMouseHoveringRect(x, y, x + w, y + h) then return nil end
  if (drag.group or 0) == g then return nil end
  return hasRoom(raid, g, drag.group) and C.accent or C.danger
end

-- One group's card at (x, y), w x h, with room for `lines` lines.
local function groupCard(g, list, x, y, w, h, lines, s, raid, needle, drags)
  local glow = dropGlow(raid, g, x, y, w, h)
  T.Panel(x, y, x + w, y + h, s, { radius = 6, glow = glow, glowWidth = glow and 2 or nil })
  local pad = 6 * s
  local _, th = T.TextSize("Ag", T.font.small)
  local n = #list
  -- the header: the group's number, its average level, its head count as pips (or Move here)
  local hw = T.Text(x + pad + 2 * s, y + pad, "Group " .. g, C.heading or C.accent, T.font.small)
  if n > 0 then
    local sum = 0
    for _, m in ipairs(list) do sum = sum + (tonumber(m.level) or 0) end
    T.Text(x + pad + 8 * s + hw, y + pad + 1 * s, "avg " .. math.floor(sum / n), C.faint, T.font.tiny)
  end
  if not moveHere("##here" .. g, x + w - pad, y + pad - 2 * s, s, raid, g, { x, y, x + w, y + h }) then
    pips(x + w - pad - 2 * s, y + pad + th / 2, n, s)
  end
  local ly = y + pad + th + 6 * s
  ui.DrawLine(x + pad, ly - 3 * s, x + w - pad, ly - 3 * s, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  local lx, lw = x + pad - 2 * s, w - 2 * pad + 4 * s
  for _, m in ipairs(list) do ly = ly + memberLine(m, lx, ly, lw, s, raid, needle, drags) end
  for _ = n + 1, lines do ly = ly + emptySlot(lx, ly, lw, s, glow == C.accent) end
  if glow == C.danger then
    local _, tth = T.TextSize("Ag", T.font.tiny)
    T.Text(x + w / 2, y + h - pad - tth, "Group is full", C.danger, T.font.tiny, "center")
  end
end

local function summary(raid, s)
  -- players, average level, the leader, locked or open
  S.Chip(raid.count .. (raid.count == 1 and " player" or " players"), C.text)
  ui.SameLine(0, 6 * s)
  S.Chip("avg level " .. W.Text(WINDOW, "RAID_LevelAverageLabel"), C.muted, "The raid's average level")
  if raid.leader then
    ui.SameLine(0, 6 * s)
    S.Chip("leader " .. raid.leader, C.gold)
  end
  -- (locked or open: the window tells only its leader, through Lock / Unlock)
  if isLeader() then
    ui.SameLine(0, 6 * s)
    local locked = eq.ChildShown(WINDOW, "RAID_UnlockButton") == true
    S.Chip(locked and "locked" or "open", locked and C.danger or C.good,
      locked and "No one can join while the raid is locked" or "Players can be invited")
  end
  ui.SameLine(0, 10 * s)
  local avail = ui.GetContentRegionAvail()
  if avail > 120 * s then
    search = S.Search("raidsearch", search, "Find a player or class", -1)
  else
    ui.NewLine()
  end
end

-- The leader's strip over the cards: how moving works, and Lock / Unlock.
local function leaderStrip(s)
  if not isLeader() then return end
  local open = movesOpen()
  local x, y = ui.GetCursorScreenPos()
  local aw = ui.GetContentRegionAvail()
  local btnH = S.ButtonHeight(s)
  local h = btnH + 8 * s
  local c = open and C.good or C.gold
  ui.DrawRectFilled(x, y, x + aw, y + h, c[1], c[2], c[3], 0.08, 6 * s)
  ui.DrawRect(x, y, x + aw, y + h, c[1], c[2], c[3], 0.30, 6 * s, 1)
  local text
  if #pending > 0 then
    text = "Locking the raid to move " .. pending[1].name
      .. (#pending > 1 and (" and " .. (#pending - 1) .. " more") or "") .. "..."
  elseif open then
    text = "Raid locked: drag players between groups, or select one and use Move here. No one can join until you unlock."
  else
    text = "The game moves players only while the raid is locked. Dragging a player locks it for you."
  end
  local bw = 90 * s
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(x + 10 * s, y + (h - th) / 2, T.Fit(text, aw - bw - 30 * s, T.font.small), C.text, T.font.small)
  ui.SetCursorScreenPos(x + aw - bw - 4 * s, y + 4 * s)
  if open then
    W.Button(WINDOW, "RAID_UnlockButton", "Unlock", { width = bw, height = btnH, tooltip = "Let players join again" })
  else
    W.Button(WINDOW, "RAID_LockButton", "Lock", { width = bw, height = btnH, primary = true,
      tooltip = "Stop new players joining, and allow moving players between groups" })
  end
  ui.SetCursorScreenPos(x, y + h + 6 * s)
end

local ACTIONS = {
  { "RAID_MemberActionsButton", "Member actions", { tooltip = "Open the selected member's actions; right-clicking a member opens the same menu" } },
  { "RAID_InviteExpedButton", "Invite Expedition", { tooltip = "Invite expedition members who are missing from this raid" } },
  { "RAID_InviteTaskButton", "Invite Task", { tooltip = "Invite shared task members who are missing from this raid" } },
  { "RAID_InviteButton", "Invite", { primary = true, tooltip = "Invite your target to the raid" } },
  { "RAID_OptionsButton", "Options", { tooltip = "The raid's loot type, looters and class colours" } },
  { "RAID_DumpButton", "Dump", { tooltip = "Write the raid roster with its notes to a file" } },
  { "RAID_DisbandButton", "Leave", { danger = true, tooltip = "Leave the raid" } },
}
-- Context menus can be switched off globally. Keep the member controls usable in that mode.
local DIRECT_ACTIONS = {
  { "RAID_MainAssistButton", "Main Assist", { tooltip = "Delegate raid main assist to the selected member" } },
  { "RAID_MarkNPCButton", "Mark NPC", { tooltip = "Delegate raid target marking to the selected member" } },
  { "RAID_FindPCButton", "Find PC", { tooltip = "Find a path to the selected raid member" } },
  { "RAID_MakeLeaderButton", "Make Leader", { tooltip = "Hand the raid lead to your target" } },
  { "RAID_RemoveLeaderButton", "Remove Leader", { tooltip = "Remove the selected member's group leadership" } },
  { "RAID_AddLooterButton", "Add Looter", { tooltip = "Let the selected player loot (Raid Looters loot type)" } },
  { "RAID_RemoveLooterButton", "Remove Looter", { tooltip = "Take the selected player off the looters" } },
  { "RAID_NoGroupButton", "Ungroup", { tooltip = "Take the selected player out of their group" } },
}
for i = 2, #ACTIONS do DIRECT_ACTIONS[#DIRECT_ACTIONS + 1] = ACTIONS[i] end
local function actionList() return eq.ContextMenus() and ACTIONS or DIRECT_ACTIONS end
local function actionColumns(w, s)
  return w >= 680 * s and 4 or (w >= 510 * s and 3 or 2)
end

local function actions(raid, s)
  local btnH = S.ButtonHeight(s)
  local sel = raid.selected
  if sel then
    ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Selected")
    ui.SameLine()
    local cc = sel.classColor or S.ClassColor(sel.class)
    ui.TextColored(cc[1], cc[2], cc[3], 1, sel.name)
    ui.SameLine()
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1,
      sel.level .. " " .. sel.class .. "  -  " .. (sel.group and ("Group " .. sel.group) or "not in a group"))
  end
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1,
    eq.ContextMenus() and "Right-click a member for actions." or "Select a member, then choose an action.")
  local choices = actionList()
  local cols = actionColumns(ui.GetContentRegionAvail(), s)
  for first = 1, #choices, cols do
    local row = {}
    for i = first, math.min(first + cols - 1, #choices) do row[#row + 1] = choices[i] end
    W.ButtonRow(WINDOW, row, btnH)
  end
end

local function membersPage(raid, s)
  local needle = search:lower()
  leaderStrip(s)
  local btnH = S.ButtonHeight(s)
  local aw, ah = ui.GetContentRegionAvail()
  local rows = math.ceil(#actionList() / actionColumns(aw, s))
  local below = math.min(ah * 0.60, rows * (btnH + 8 * s) + 30 * s + select(2, ui.CalcTextSize("Ag")) + 4 * s +
    (raid.selected and select(2, ui.CalcTextSize("Ag")) + 4 * s or 0))
  ui.BeginChild("##raidcards", 0, -below, false)
  local x0, y0 = ui.GetCursorScreenPos()
  local aw = ui.GetContentRegionAvail()
  local gap = 8 * s
  local drags = isLeader()
  local cards = {} -- { group, x, y, w, h } for drops
  local _, th = T.TextSize("Ag", T.font.small)
  local lh = lineHeight(s)
  local head = 6 * s + th + 9 * s
  local y = y0
  -- the players in no group: a strip of name pills that wraps (for the leader always, as the drop for no group)
  if #raid.ungrouped > 0 or drags then
    local perRow = math.max(1, math.floor((aw + gap) / (170 * s + gap)))
    local colW = (aw - 12 * s - (perRow - 1) * gap) / perRow
    local rows = math.max(1, math.ceil(#raid.ungrouped / perRow))
    local h = head + rows * lh + 6 * s
    local glow = dropGlow(raid, 0, x0, y, aw, h)
    T.Panel(x0, y, x0 + aw, y + h, s, { radius = 6, glow = glow, glowWidth = glow and 2 or nil })
    T.Text(x0 + 8 * s, y + 6 * s, "Not in a group", C.heading or C.accent, T.font.small)
    if not moveHere("##here0", x0 + aw - 6 * s, y + 4 * s, s, raid, 0, { x0, y, x0 + aw, y + h }) then
      T.Text(x0 + aw - 8 * s, y + 6 * s, tostring(#raid.ungrouped), C.muted, T.font.tiny, "right")
    end
    if #raid.ungrouped == 0 then
      T.Text(x0 + aw / 2, y + head + (lh - th) / 2, "Drop a player here to take them out of their group", C.faint,
        T.font.tiny, "center")
    end
    for i, m in ipairs(raid.ungrouped) do
      local c = (i - 1) % perRow
      local r = math.floor((i - 1) / perRow)
      memberLine(m, x0 + 6 * s + c * (colW + gap), y + head + r * lh, colW, s, raid, needle, drags)
    end
    cards[#cards + 1] = { 0, x0, y, aw, h }
    y = y + h + gap
  end
  -- the twelve groups, as many across as fit
  local minW = 170 * s
  local perRow = math.max(1, math.min(GROUPS, math.floor((aw + gap) / (minW + gap))))
  if perRow > 6 then perRow = 6 elseif perRow > 4 and perRow < 6 then perRow = 4 end -- 12 = 6x2, 4x3, 3x4, 2x6
  local cw = (aw - (perRow - 1) * gap) / perRow
  -- a row of cards is as tall as its fullest group with a free slot to drop into (two lines at least, six at most)
  local cy = y
  for first = 1, GROUPS, perRow do
    local lines = 2
    for g = first, math.min(GROUPS, first + perRow - 1) do lines = math.max(lines, #raid.groups[g] + 1) end
    lines = math.min(lines, PER_GROUP)
    local ch = head + lines * lh + 6 * s
    for g = first, math.min(GROUPS, first + perRow - 1) do
      local cx = x0 + (g - first) * (cw + gap)
      groupCard(g, raid.groups[g], cx, cy, cw, ch, lines, s, raid, needle, drags)
      cards[#cards + 1] = { g, cx, cy, cw, ch }
    end
    cy = cy + ch + gap
  end
  local total = cy - gap - y0
  ui.SetCursorScreenPos(x0, y0)
  ui.Dummy(aw, total)

  -- a drag: the member follows the mouse with where it would go; let go over a card to move there
  if drag then
    local mx, my = ui.GetMousePos()
    local over = nil
    for _, cd in ipairs(cards) do
      if mx >= cd[2] and mx < cd[2] + cd[4] and my >= cd[3] and my < cd[3] + cd[5] then
        over = cd[1]
        break
      end
    end
    local where = nil
    if over and over ~= (drag.group or 0) then
      if not hasRoom(raid, over, drag.group) then where = "full"
      else where = over == 0 and "to no group" or ("to Group " .. over) end
    end
    ui.Foreground(true)
    local tw, tht = T.TextSize(drag.name, T.font.small)
    local ww = where and (T.TextSize(where, T.font.tiny) + 8) or 0
    local cc = drag.classColor or S.ClassColor(drag.class)
    local x1 = mx + 30 + tw + ww
    ui.DrawRectFilled(mx + 12, my + 6, x1, my + 14 + tht, C.popup[1], C.popup[2], C.popup[3], 0.95, 5)
    ui.DrawRect(mx + 12, my + 6, x1, my + 14 + tht, cc[1], cc[2], cc[3], 0.6, 5, 1)
    ui.DrawRectFilled(mx + 16, my + 10, mx + 19, my + 10 + tht, cc[1], cc[2], cc[3], 0.9, 1)
    T.Text(mx + 24, my + 10, drag.name, cc, T.font.small)
    if where then T.Text(mx + 30 + tw, my + 11, where, where == "full" and C.danger or C.muted, T.font.tiny) end
    ui.Foreground(false)
    if not ui.IsMouseDown(ui.Mouse.Left) then
      if over then
        local m = findMember(raid, drag.name)
        if m then requestMove(raid, m, over) end -- (gone from the raid meanwhile: nothing)
      end
      drag = nil
    end
  end
  ui.EndChild()
  ui.Dummy(0, 2 * s)
  -- A small resized window scrolls its controls rather than drawing them beyond the dialog.
  ui.BeginChild("##raidactions", 0, 0, false)
  actions(raid, s)
  ui.EndChild()
end

local function notesPage(s)
  local btnH = S.ButtonHeight(s)
  local leader = isLeader()
  -- the members' notes (the leader's; everyone sees them): select a member, then the leader edits its note
  W.Heading("Member notes")
  local _, ah = ui.GetContentRegionAvail()
  W.List(WINDOW, "RAID_NoteList", 0, math.max(80 * s, ah * 0.45), {
    columns = { [1] = { "Grp", 40 }, [2] = { "Name", 130 }, [3] = { "Note", 0 } },
  })
  local btnW = 80 * s
  W.Edit(WINDOW, "NoteEdit", { hint = leader and "Select a member, then type its note" or "The raid leader sets the notes",
    width = -(btnW + 8 * s), max = 63 })
  ui.SameLine()
  W.Button(WINDOW, "NoteUpdateButton", "Update", { width = btnW, tooltip = "Set this note for the selected member" })
  W.Heading("Message of the day")
  local below = leader and (btnH + 40 * s) or 0
  if W.Text(WINDOW, "MOTDViewer") == "" then
    local aw, ah2 = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(30 * s, ah2 - below), "No message of the day.",
      leader and "Set one below; every raid member sees it." or "The raid leader sets it.")
  else
    W.Stml(WINDOW, "MOTDViewer", 0, -below)
  end
  if leader then
    W.Heading("Set a new message")
    W.Edit(WINDOW, "MOTDInput", { hint = "The message every raid member sees", width = -(btnW + 8 * s) })
    ui.SameLine()
    W.Button(WINDOW, "UpdateMOTDButton", "Update", { width = btnW, primary = true })
  end
end

local OPTS = {
  size = function() return 860, 600 end,
  resizable = { 400, 300 },
  keepTitle = true,
}
local function body(x, y, w, h, s)
  local raid = readRaid()
  if raid.count > 0 then
    S.TestClicks(WINDOW) -- (after the lists are read: a row click selects by the rows just listed)
    -- read again only when there are test clicks (test.click.RaidWindow), the only way the call above changes them
    if (ui.GetSetting(TEST_CLICKS) or "") ~= "" then raid = readRaid() end
  end
  flushPending(raid)
  local btnH = S.ButtonHeight(s)
  -- an invitation to answer
  if eq.ChildShown(WINDOW, "RAID_AcceptButton") == true then
    local bx, by = ui.GetCursorScreenPos()
    local aw = ui.GetContentRegionAvail()
    local bh = btnH + 16 * s
    ui.DrawRectFilled(bx, by, bx + aw, by + bh, C.accent[1], C.accent[2], C.accent[3], 0.12, 6 * s)
    ui.DrawRect(bx, by, bx + aw, by + bh, C.accent[1], C.accent[2], C.accent[3], 0.45, 6 * s, 1)
    ui.SetCursorScreenPos(bx + 10 * s, by + (bh - select(2, ui.CalcTextSize("Ag"))) / 2)
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, "You have been invited to join a raid.")
    ui.SameLine()
    local rw = 190 * s
    ui.SetCursorScreenPos(bx + aw - rw - 8 * s, by + 8 * s)
    W.Button(WINDOW, "RAID_AcceptButton", "Accept", { primary = true, width = rw / 2 - 4 * s, height = btnH })
    ui.SetCursorScreenPos(bx + aw - rw / 2 - 4 * s, by + 8 * s)
    W.Button(WINDOW, "RAID_DeclineButton", "Decline", { width = rw / 2 - 4 * s, height = btnH })
    ui.SetCursorScreenPos(bx, by + bh + 6 * s)
  end
  if raid.count == 0 then
    -- not in a raid: what to do, and the buttons that start one
    pending = {}
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(40 * s, ah - btnH - 10 * s), "You are not in a raid.",
      "Target a player and press Invite to start one.")
    W.ButtonRow(WINDOW, {
      { "RAID_InviteButton", "Invite", { primary = true, tooltip = "Invite your target to a raid" } },
    }, btnH)
    return
  end
  summary(raid, s)
  local shown = W.Tabs(WINDOW, "Subwindows", { { "Members", 0 }, { "Notes", 1 } })
  if shown == 1 then
    drag = nil -- (a drag is the members page's)
    notesPage(s)
  else
    membersPage(raid, s)
  end
  bodyFrame = ui.GetFrameCount()
end

ui.window("Raid", function()
  if not eq.InGame() then
    drag, pending = nil, {}
    return
  end
  hud.Dialog(WINDOW, "Raid", OPTS, body)
  if bodyFrame ~= ui.GetFrameCount() then drag = nil end -- closed (or no raid): a drag ends
end)
