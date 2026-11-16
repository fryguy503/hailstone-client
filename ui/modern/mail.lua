-- Mail: replaces the game's MailWindow. A mail client's layout: Compose, the address book and the mailbox at the
-- top, the Inbox and Deleted tabs (with the unread count), a search over sender and subject, the messages beside
-- the selected one's text when the window is wide (above it when narrow), and the message's actions under it.
-- The lists, the message view (the game's own STML: From, To, Subject, Date, Status, then the body) and every
-- button are the window's: selecting a row is the list's click (which marks the message read), Reply / Reply All /
-- Forward open the composition window filled in by the game, Delete moves to the deleted items. The window's
-- Export, Report, Spam and options buttons are not drawn: it does not handle them.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local WINDOW = "MailWindow"

local tab = "inbox"
local search = ""

-- The window's two pages (its tab box has no ScreenID; this UI keeps the page).
local PAGES = {
  inbox = { list = "InboxList", body = "Body", empty = "Your inbox is empty.", emptyHint = "Messages other players send you arrive here.",
    actions = {
      { "ButtonReply", "Reply", { primary = true } },
      { "ButtonReplyAll", "Reply All", { tooltip = "Reply to the sender and everyone it was sent to" } },
      { "ButtonForward", "Forward" },
      { "ButtonDelete", "Delete", { danger = true, tooltip = "Move the message to Deleted" } },
    } },
  trash = { list = "TrashList", body = "TrashBody", empty = "No deleted messages.",
    actions = {
      { "ButtonTrashUndelete", "Restore", { primary = true, tooltip = "Move the message back to the inbox" } },
      { "ButtonTrashReply", "Reply" },
      { "ButtonTrashReplyAll", "Reply All" },
      { "ButtonTrashForward", "Forward" },
      { "ButtonTrashEmpty", "Empty", { danger = true, tooltip = "Delete every message here for good" } },
    } },
}

-- New and unread rows are the colour the game gives them (0xffc896).
local function unread(r, g, b) return r and r > 0.95 and g and g < 0.85 and g > 0.7 and b < 0.65 end

local function unreadCount()
  local rows = API.windows.ListInfo(WINDOW, "InboxList") or 0
  local n = 0
  for i = 0, rows - 1 do
    local _, r, g, b = API.windows.ListRow(WINDOW, "InboxList", i)
    if unread(r, g, b) then n = n + 1 end
  end
  return n, rows
end

-- "Wed Jun 30 21:49:08 1993" as "Jun 30 21:49" (this year) or "Jun 30 1993".
local function shortDate(t)
  local mon, day, hm, year = (t or ""):match("^%a+ (%a+) +(%d+) (%d+:%d+):%d+ (%d+)$")
  if not mon then return t or "" end
  if tonumber(year) == tonumber(os.date("%Y")) then return mon .. " " .. day .. " " .. hm end
  return mon .. " " .. day .. " " .. year
end

local function messageList(P, w, h, needle, s)
  local rows = API.windows.ListInfo(WINDOW, P.list) or 0
  if rows == 0 then
    ui.BeginChild("##mailempty", w, h, false)
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, ah, P.empty, P.emptyHint)
    ui.EndChild()
    return
  end
  W.List(WINDOW, P.list, w, h, {
    columns = { [1] = { "From", 110 }, [2] = { "Subject", 0 }, [3] = { "Date", 92 } },
    filter = needle ~= "" and function(_, texts) return S.Matches(texts, needle, { 1, 2 }) end or nil,
    onRow = function(row, texts)
      local _, r, g, b = API.windows.ListRow(WINDOW, P.list, row)
      local new = unread(r, g, b)
      local c = new and C.gold or C.text
      ui.TextColored(c[1], c[2], c[3], 1, texts[1] or "")
      ui.TableNextColumn()
      ui.TextColored(c[1], c[2], c[3], 1, texts[2] or "")
      ui.TableNextColumn()
      ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, shortDate(texts[3]))
      if new and ui.IsItemHovered() then ui.SetTooltip("Unread") end
      return true
    end,
  })
end

local function view(P, w, h, s)
  local _, _, sel = API.windows.ListInfo(WINDOW, P.list)
  local btnH = S.ButtonHeight(s)
  ui.BeginChild("##mailview", w, h, false)
  local _, ah = ui.GetContentRegionAvail()
  if (sel or -1) < 0 or W.Text(WINDOW, P.body) == "" then
    local aw = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(30 * s, ah - btnH - 8 * s), "No message selected.", "Pick one from the list to read it.")
  else
    local bx, by = ui.GetCursorScreenPos()
    local aw = ui.GetContentRegionAvail()
    ui.DrawRectFilled(bx, by, bx + aw, by + ah - btnH - 8 * s, C.ink[1], C.ink[2], C.ink[3], 0.025, 6 * s)
    ui.SetCursorScreenPos(bx + 8 * s, by + 6 * s)
    W.Stml(WINDOW, P.body, aw - 16 * s, ah - btnH - 20 * s)
    ui.SetCursorScreenPos(bx, by + ah - btnH - 2 * s)
  end
  W.ButtonRow(WINDOW, P.actions, btnH)
  ui.EndChild()
end

local WINDOW_OPTS = {
  size = function() return 820, 500 end,
  resizable = { 440, 320 },
  keepTitle = true,
}
local function drawWindow(x, y, w, h, s)
  S.TestClicks(WINDOW)
  -- the top line: Compose, the address book, the mailbox
  local btnH = S.ButtonHeight(s)
  W.Button(WINDOW, "ButtonCompose", "Compose", { primary = true, height = btnH, tooltip = "Write a new message" })
  ui.SameLine()
  W.Button(WINDOW, "AddressBookButton", "Address Book", { height = btnH, tooltip = "Names and lists you send to" })
  local choices = API.windows.ComboInfo(WINDOW, "CharacterBoxSelector")
  if choices and #choices > 0 then
    ui.SameLine()
    local aw = ui.GetContentRegionAvail()
    local cw = math.min(200 * s, aw)
    if aw > cw + 70 * s then
      local cx, cy = ui.GetCursorPos()
      ui.SetCursorPos(cx + aw - cw - 64 * s, cy)
      ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Mailbox")
      ui.SameLine()
    end
    W.Combo(WINDOW, "CharacterBoxSelector", { width = cw })
  end
  -- the pages
  local unreadN = unreadCount()
  local trashN = API.windows.ListInfo(WINDOW, "TrashList") or 0
  if ui.BeginTabBar("##mailtabs") then
    local tt = ui.GetSetting("test.mail.tab") -- testing: inbox | trash
    local function item(key, label)
      local flags = (tt == key and tab ~= key) and ui.TabItemFlags.SetSelected or 0
      if ui.BeginTabItem(label, flags) then
        tab = key
        ui.EndTabItem()
      end
    end
    item("inbox", (unreadN > 0 and ("Inbox (" .. unreadN .. " unread)") or "Inbox") .. "###inbox")
    item("trash", (trashN > 0 and ("Deleted (" .. trashN .. ")") or "Deleted") .. "###trash")
    ui.EndTabBar()
  end
  local P = PAGES[tab]
  local needle
  search, needle = S.Search("mailsearch", search, "Search sender or subject")
  local aw, ah = ui.GetContentRegionAvail()
  if (API.windows.ListInfo(WINDOW, P.list) or 0) == 0 then
    messageList(P, aw, ah, needle, s) -- nothing to read: the empty state alone
  elseif aw >= 640 * s then
    local lw = math.floor(aw * 0.46)
    messageList(P, lw, ah, needle, s)
    ui.SameLine(0, 10 * s)
    view(P, aw - lw - 10 * s, ah, s)
  else
    local lh = math.floor(ah * 0.42)
    messageList(P, aw, lh, needle, s)
    view(P, aw, ah - lh - 6 * s, s)
  end
end

ui.window("Messages", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Mail", WINDOW_OPTS, drawWindow)
end)
