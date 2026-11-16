-- Mail (gamepad pack): stands in for MailWindow and MailCompositionWindow.
--   Mail           two pages, LB / RB to switch: Inbox and Trash. A reads the focused message (the stock list's
--                  click: a new message becomes read), shown on the right with the stock window's header lines.
--                  X deletes it (to the trash; in the trash: undeletes it), Y replies; the buttons under the
--                  message do the rest (Compose, Reply All, Forward, Empty Trash, the Address Book). Back picks the next mailbox when
--                  the account has several (the stock mailbox drop-down).
--   Compose Mail   To, Subject and the message as rows; A on one opens the on-screen keyboard on it (a keyboard
--                  types into it too), RB sends (the stock Send button), B cancels (the stock Cancel); Address Book
--                  opens the book (mailbook.lua), whose X adds the focused name to To.
-- The texts, rows and buttons are the stock windows' (hs.ListRow, hs.WindowText, hs.SetWindowText, hs.ClickChild).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local P = dofile("lib/textpanel.lua")
local K = dofile("lib/keyfield.lua")
local C = T.C

-- Keeps the D-pad's up / down in a W.list column: ImGui's navigation may jump to a nearer item of the next column
-- (the objectives, the message), so a focused row hands the focus to its neighbour itself. (The last shown row's
-- down and the first's up scroll the list: W.list does those.)
local function stayInColumn(state, i, count, visible)
  local v = i - (state.offset or 0)
  if pad.menuPressed("down") and i < count and v < visible then state.focusNext = v + 1 end
  if pad.menuPressed("up") and v > 1 then state.focusNext = v - 1 end
end

-- A pad shortcut of the current window, not in its first moments: the press that opened it (X uses a bag in the
-- inventory, A opens a tile) is not one for it.
local function pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 8 and W.pressed(b) -- (appeared: by frames, as W.begin keeps it)
end

local MW, CW = "MailWindow", "MailCompositionWindow"

local function tabs(labels, page, x0, y0, set)
  local x = x0
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  local h = th + T.px(16)
  for i, l in ipairs(labels) do
    local tw = ui.CalcTextSize(l)
    local w = tw + T.px(40)
    T.rect(x, y0, x + w, y0 + h, page == i and C.accent or C.raised, page == i and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + h, l, page == i and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + h) and ui.IsMouseClicked(ui.Mouse.Left) then set(i) end
    x = x + w + T.px(10)
  end
  T.unfont()
  return y0 + h + T.px(16), x
end

local function enabled(screen, child)
  local e = API.windows.ChildState(screen, child)
  return e ~= false
end

-- ---------------------------------------------------------------- the mailbox (MailWindow)
local page = 1 -- 1 inbox, 2 trash
local lists = { { offset = 0 }, { offset = 0 } }
local bodies = { { scroll = 0 }, { scroll = 0 } }

local mailShown = false

-- The inbox's unread messages (the rows in the stock list's new / unread colour, 0xffc896), counted again every
-- 0.25 s, when the inbox's row count changes and when the window opens: it is a row at a time.
local unreadCount, unreadAt, unreadRows = 0, nil, -1
local function unreadMessages()
  local now = ui.GetTime()
  local rows = API.windows.ListInfo(MW, "InboxList") or 0
  if not unreadAt or now - unreadAt >= 0.25 or now < unreadAt or rows ~= unreadRows then
    local unread = 0
    for i = 0, rows - 1 do
      local _, r, g, b = API.windows.ListRow(MW, "InboxList", i)
      if r and b and b < 0.7 then unread = unread + 1 end
    end
    unreadCount, unreadAt, unreadRows = unread, now, rows
  end
  return unreadCount
end

ui.window("Mail", function()
  if not W.shown(MW) then
    mailShown = false
    unreadAt = nil
    return
  end
  local trash = page == 2
  local list = trash and "TrashList" or "InboxList"
  local rows, _, sel = API.windows.ListInfo(MW, list)
  rows = rows or 0
  local boxes, boxSel = API.windows.ComboInfo(MW, "CharacterBoxSelector")
  local unread = unreadMessages()
  local hints = { { "a", "Read" }, { "x", trash and "Undelete" or "Delete" } }
  if not trash then hints[#hints + 1] = { "y", "Reply" } end
  hints[#hints + 1] = { "lb", "Inbox" }
  hints[#hints + 1] = { "rb", "Trash" }
  if boxes and #boxes > 1 then hints[#hints + 1] = { "back", "Mailbox" } end
  hints[#hints + 1] = { "b", "Close" }
  local box = boxes and boxSel and boxes[boxSel + 1] or ""
  local sub = (box ~= "" and (box .. "   ") or "") .. (unread == 1 and "1 unread message" or (unread .. " unread messages"))
  if W.begin("Mail", { w = 1680, h = 960, title = "Mail", subtitle = sub, hints = hints }) then
    -- the pad's focus on the first message when the window opens or the page changes
    if not mailShown then lists[page].focusNext, mailShown = 1, true end
    if pressed("Mail", "lb") and page ~= 1 then page, lists[1].focusNext = 1, 1 end
    if pressed("Mail", "rb") and page ~= 2 then page, lists[2].focusNext = 2, 1 end
    if boxes and #boxes > 1 and pressed("Mail", "back") then API.windows.ComboSelect(MW, "CharacterBoxSelector", ((boxSel or 0) + 1) % #boxes) end
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local top = tabs({ "Inbox", "Trash" }, page, x0, y0, function(i) page = i end)
    local leftX1 = x0 + (x1 - x0) * 0.42
    local rowH = 78
    ui.SetCursorPos(x0 - wx, top - wy)
    local st = lists[page]
    local shownRows = math.max(1, math.floor((y1 - top + T.px(10)) / T.px(rowH + 10)))
    local focusedRow = W.list(list, st, rows, shownRows, rowH, leftX1 - x0,
      function(i, rx0, ry0, rx1, ry1, focused)
        if focused then stayInColumn(st, i, rows, shownRows) end
        local texts, r, g, b = API.windows.ListRow(MW, list, i - 1)
        if not texts then return end
        local new = b and b < 0.7
        local p = T.px(16)
        T.font(22)
        local _, th = ui.CalcTextSize("Ag")
        if new then ui.DrawCircleFilled(rx0 + p, ry0 + T.px(10) + th / 2, T.px(6), C.accent[1], C.accent[2], C.accent[3], 1) end
        local nx = rx0 + p + (new and T.px(20) or 0)
        T.draw(nx, ry0 + T.px(10), T.fit(texts[2] or "", rx1 - nx - p), new and { r, g, b, 1 } or C.text)
        T.unfont()
        T.font(17)
        local _, sh = ui.CalcTextSize("Ag")
        T.draw(nx, ry1 - sh - T.px(10), T.fit(texts[1] or "", (rx1 - nx) * 0.45), C.dim)
        T.textIn(nx, ry1 - sh - T.px(10), rx1 - p, ry1 - T.px(10), texts[3] or "", C.faint, 1)
        T.unfont()
      end,
      function(i) API.windows.ListClick(MW, list, i - 1, "left") bodies[page].scroll = 0 end)
    if rows == 0 then T.draw(x0, top, trash and "The trash is empty." or "No mail.", C.faint) end
    -- X and Y act on the focused message (selected first, as a click on it does)
    if focusedRow and pressed("Mail", "x") then
      API.windows.ListClick(MW, list, focusedRow - 1, "left")
      API.windows.ClickChild(MW, trash and "ButtonTrashUndelete" or "ButtonDelete", "left")
    end
    if focusedRow and not trash and pressed("Mail", "y") then
      API.windows.ListClick(MW, list, focusedRow - 1, "left")
      API.windows.ClickChild(MW, "ButtonReply", "left")
    end
    -- the selected message and the buttons
    local rx0 = leftX1 + T.px(24)
    local bh = T.px(60)
    local by = y1 - bh
    P.draw("##body" .. page, bodies[page], API.windows.Text(MW, trash and "TrashBody" or "Body") or "", rx0, top, x1, by - T.px(16),
      { empty = sel and sel >= 0 and "Loading..." or "Choose a message and press A to read it." })
    local buttons = trash
      and { { "Compose", "ButtonTrashCompose" }, { "Reply", "ButtonTrashReply" }, { "Reply All", "ButtonTrashReplyAll" },
        { "Forward", "ButtonTrashForward" }, { "Undelete", "ButtonTrashUndelete" }, { "Empty Trash", "ButtonTrashEmpty" } }
      or { { "Compose", "ButtonCompose" }, { "Reply", "ButtonReply" }, { "Reply All", "ButtonReplyAll" },
        { "Forward", "ButtonForward" }, { "Delete", "ButtonDelete" }, { "Addresses", "AddressBookButton" } }
    local gap = T.px(10)
    local bw = (x1 - rx0 - gap * (#buttons - 1)) / #buttons
    ui.SetCursorPos(rx0 - wx, by - wy)
    T.font(20)
    for i, b in ipairs(buttons) do
      if i > 1 then ui.SameLine(0, gap) end
      local on = enabled(MW, b[2])
      if T.button("##mb" .. b[2], b[1], bw, bh, { disabled = not on, primary = i == 1 }, pad) then API.windows.ClickChild(MW, b[2], "left") end
    end
    T.unfont()
  end
  if W.finish() then API.windows.Show(MW, false) end
end)

-- ---------------------------------------------------------------- composing (MailCompositionWindow)
local FIELDS = { { "To", "ToField" }, { "Subject", "SubjectField" }, { "Message", "TextField" } }
local editing = nil -- the field (index) the keyboard types into
local focusField = nil
local kb = K.new()
local wasShown = false

ui.window("Compose Mail", function()
  if not W.shown(CW) then
    editing, wasShown = nil, false
    return
  end
  if not wasShown then -- opened (Compose, Reply, ...): the fields first, no keyboard yet
    wasShown = true
    editing = nil
  end
  local title = API.windows.Title(CW) or ""
  local hints = editing
    and { K.hints[1], K.hints[2], K.hints[3], K.hints[4], { "b", "Done" }, { "rb", "Send" } }
    or { { "a", "Edit" }, { "rb", "Send" }, { "b", "Cancel" } }
  if W.begin("Compose Mail", { w = 1500, h = 980, title = title ~= "" and title or "Compose Mail",
        subtitle = "From " .. (API.windows.Text(CW, "FromAddressLabel") or ""), hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if pressed("Compose Mail", "rb") then API.windows.ClickChild(CW, "SendButton", "left") end
    -- the fields
    local labelW = T.px(150)
    for i, f in ipairs(FIELDS) do
      local text = API.windows.Text(CW, f[2]) or ""
      local h = T.px(i == 3 and (editing and 120 or 300) or 64)
      ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
      local fx0, fy0 = ui.GetCursorScreenPos()
      if editing == i then
        -- the field being typed into: an input box (a keyboard works too), the pad's keys below
        T.font(20)
        T.textIn(fx0, fy0, fx0 + labelW, fy0 + T.px(64), f[1], C.accent, 0)
        T.unfont()
        ui.SetCursorPos(fx0 + labelW - wx, fy0 - wy)
        local t
        if i == 3 then
          t = ui.InputTextMultiline("##mailfield" .. i, text, 4000, x1 - fx0 - labelW, h)
        else
          ui.SetNextItemWidth(x1 - fx0 - labelW)
          t = ui.InputText("##mailfield" .. i, text, 256, 0, { moveToEnd = true })
          ui.Dummy(1, h - T.px(60))
        end
        if t ~= text then API.windows.SetText(CW, f[2], t) end
      else
        if focusField == i then -- the keyboard just closed: the pad's focus back on its field
          ui.SetKeyboardFocusHere()
          focusField = nil
        end
        local clicked, focused, bx0, by0, bx1, by1 = T.button("##field" .. i, "", x1 - x0, h, {}, pad)
        T.font(20)
        T.draw(bx0 + T.px(16), by0 + T.px(16), f[1], focused and C.accent or C.dim)
        T.unfont()
        T.font(22)
        local _, lh = ui.CalcTextSize("Ag")
        if i == 3 then
          local y = by0 + T.px(16)
          ui.PushClipRect(bx0 + labelW, by0, bx1 - T.px(12), by1 - T.px(8), true)
          for line in (text .. "\n"):gmatch("([^\n]*)\n") do
            T.draw(bx0 + labelW, y, line)
            y = y + lh
            if y > by1 then break end
          end
          ui.PopClipRect()
          if text == "" then T.draw(bx0 + labelW, by0 + T.px(16), "(empty)", C.faint) end
        else
          T.draw(bx0 + labelW, by0 + (h - lh) / 2, text ~= "" and T.fit(text, bx1 - bx0 - labelW - T.px(16)) or "(empty)", text ~= "" and C.text or C.faint)
        end
        T.unfont()
        if clicked then
          editing = i
          K.focus(kb)
        end
      end
    end
    -- the keyboard while a field is edited, else Send / Cancel
    ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(6) - wy)
    if editing then
      local f = FIELDS[editing]
      local done = K.draw(kb, function() return API.windows.Text(CW, f[2]) end, function(t) API.windows.SetText(CW, f[2], t) end,
        x0, x1, { newline = editing == 3 })
      if done == "done" then editing, focusField = nil, editing end
    else
      local bw = (x1 - x0 - 2 * T.px(10)) / 3
      if T.button("##send", "Send", bw, T.px(64), { primary = true, glyph = "rb" }, pad) then API.windows.ClickChild(CW, "SendButton", "left") end
      ui.SameLine(0, T.px(10))
      if T.button("##addresses", "Address Book", bw, T.px(64), {}, pad) then API.windows.ClickChild(CW, "AddressBookButton", "left") end
      ui.SameLine(0, T.px(10))
      if T.button("##cancel", "Cancel", bw, T.px(64), { glyph = "b" }, pad) then API.windows.ClickChild(CW, "CancelButton", "left") end
    end
  end
  if W.finish() then
    -- B: leaves the keyboard first, then cancels (the stock Cancel: "Message cancelled and not sent.")
    if editing then editing, focusField = nil, editing else API.windows.ClickChild(CW, "CancelButton", "left") end
  end
end)
