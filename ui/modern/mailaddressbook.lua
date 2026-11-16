-- Address book: replaces the game's MailAddressBookWindow. The names at the left (the book's entries, then the
-- built-in guild and group lists in grey) with a search, and the selected entry's fields at the right: Name,
-- Addresses, Private, Note, and Save (the window's Add / Modify), Delete and New. A click on a name shows it in the
-- fields; a right-click adds it to an open message's To field (the list's own right-click). The fields and buttons
-- are the window's, so the book is the game's (saved with the character). Its Ignore button opens the ignore list
-- (MailIgnoreListWindow), the same window over the second book the game keeps: names whose mail you do not want.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local search = {}

local function fields(WINDOW, s)
  local btnH = S.ButtonHeight(s)
  S.Caption("Name")
  W.Edit(WINDOW, "NameField", { hint = "What you type in To", width = -1, max = 64 })
  S.Caption("Addresses")
  W.Edit(WINDOW, "FieldOfAddresses", { multiline = true, width = -1, height = 70 * s, max = 1024 })
  if ui.IsItemHovered() then ui.SetTooltip("Character names, separated by commas") end
  W.Check(WINDOW, "NameFieldIsPrivateCheckbox", "Private", { tooltip = "Keep this entry to yourself" })
  S.Caption("Note")
  local ignore = API.windows.ChildShown(WINDOW, "IgnoreListButton") == true -- the address book's only
  local _, ah = ui.GetContentRegionAvail()
  local below = (ignore and 2 or 1) * (btnH + 8 * s)
  W.Edit(WINDOW, "NoteField", { multiline = true, width = -1, height = math.max(40 * s, ah - below), max = 256 })
  local label = W.Text(WINDOW, "AddModifyButton")
  W.ButtonRow(WINDOW, {
    { "AddModifyButton", label == "Modify" and "Save" or "Add", { primary = true,
      tooltip = label == "Modify" and "Save the changes to this entry" or "Add this entry to the book" } },
    { "DeleteButton", "Delete", { danger = true, tooltip = "Delete this entry" } },
    { "ClearButton", "New", { tooltip = "Clear the fields for a new entry" } },
  }, btnH)
  if ignore then
    W.Button(WINDOW, "IgnoreListButton", "Ignore List", { width = -1, height = btnH,
      tooltip = "Open the ignore list" })
  end
end

-- Each book's dialog options and body, made once (not every frame).
local books = {}
local function book(WINDOW, title, empty)
  local b = books[WINDOW]
  if not b then
    b = { opts = {
      size = function() return 520, 380 end,
      resizable = { 380, 300 },
    } }
    b.body = function(x, y, w, h, s)
      S.TestClicks(WINDOW)
      local aw, ah = ui.GetContentRegionAvail()
      local lw = math.max(140 * s, math.floor(aw * 0.38))
      ui.BeginChild("##abnames" .. WINDOW, lw, ah, false)
      local needle
      search[WINDOW], needle = S.Search("absearch" .. WINDOW, search[WINDOW] or "", "Search names")
      local rows = API.windows.ListInfo(WINDOW, "NameList") or 0
      if rows == 0 then
        local cw, chh = ui.GetContentRegionAvail()
        S.Empty(cw, chh, empty, "Fill in the fields and press Add.")
      else
        W.List(WINDOW, "NameList", 0, -(select(2, ui.CalcTextSize("Ag")) + 6 * s), {
          columns = { [1] = { "", 0 } },
          filter = needle ~= "" and function(_, texts) return S.Matches(texts, needle) end or nil,
        })
        ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Right-click: add to To")
      end
      ui.EndChild()
      ui.SameLine(0, 12 * s)
      ui.BeginChild("##abfields" .. WINDOW, 0, ah, false)
      fields(WINDOW, s)
      ui.EndChild()
    end
    books[WINDOW] = b
  end
  hud.Dialog(WINDOW, title, b.opts, b.body)
end

ui.window("Address Book", function()
  if not API.session.InGame() then return end
  book("MailAddressBookWindow", "Address Book", "No entries yet.")
end)

ui.window("Ignore List", function()
  if not API.session.InGame() then return end
  book("MailIgnoreListWindow", "Ignore List", "Nobody is ignored.")
end)
