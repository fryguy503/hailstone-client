-- Compose: replaces the game's MailCompositionWindow (its title is the game's: a new message, a reply or a
-- forward). From, To (with the address book beside it: a right-click on a name there adds it here), Subject, and
-- the message filling the rest of the window; Send and Cancel are the window's buttons, so sending is the game's
-- (names from the address book expand when sent). The window's address-book sync button is not drawn: it does not
-- handle it.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local WINDOW = "MailCompositionWindow"

local function caption(text, s)
  S.Caption(text)
  ui.SameLine(64 * s)
end

local WINDOW_OPTS = {
  size = function() return 520, 440 end,
  resizable = { 360, 280 },
}
local function drawWindow(x, y, w, h, s)
  S.TestClicks(WINDOW)
  local btnH = S.ButtonHeight(s)
  caption("From", s)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, W.Text(WINDOW, "FromAddressLabel"))
  caption("To", s)
  local abW = 110 * s
  W.Edit(WINDOW, "ToField", { hint = "Names, separated by commas", width = -(abW + 8 * s), max = 512 })
  ui.SameLine()
  W.Button(WINDOW, "AddressBookButton", "Address Book", { width = abW,
    tooltip = "Open the address book (right-click a name there to add it here)" })
  caption("Subject", s)
  W.Edit(WINDOW, "SubjectField", { width = -1, max = 128 })
  ui.Dummy(0, 2 * s)
  local _, ah = ui.GetContentRegionAvail()
  W.Edit(WINDOW, "TextField", { multiline = true, width = -1, height = math.max(60 * s, ah - btnH - 10 * s), max = 4096 })
  local to = W.Text(WINDOW, "ToField")
  W.ButtonRow(WINDOW, {
    { "SendButton", "Send", { primary = true, tooltip = to == "" and "Add a recipient first" or "Send the message" } },
    { "CancelButton", "Discard", { tooltip = "Close without sending" } },
  }, btnH)
end

ui.window("Compose New Message", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "New Message", WINDOW_OPTS, drawWindow)
end)
