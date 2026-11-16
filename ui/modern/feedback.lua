-- Send feedback: replaces the game's FeedbackWindow (/feedback). The character's name, the feedback in a large text
-- box, Cancel and Send. Every piece is the window's child (hs.SetWindowText, hs.ClickChild), so what the server gets
-- is the window's (the position and heading go with it).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "FeedbackWindow"

local WINDOW_OPTS = {
  size = function() return 400, 240 end,
  resizable = { 300, 180 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  K.Caption(W.Text(WINDOW, "FBKW_UserName"))
  local _, avail = ui.GetContentRegionAvail()
  local ex, ey = ui.GetCursorScreenPos()
  W.Edit(WINDOW, "FBKW_FeedbackInput", { multiline = true, height = math.max(60 * s, avail - K.FooterH(s)), max = 1023 })
  if W.Text(WINDOW, "FBKW_FeedbackInput") == "" and not ui.IsItemActive() then -- a hint while empty
    T.Text(ex + 6 * s, ey + 4 * s, "Type your feedback here", C.faint, T.font.small)
  end
  W.ButtonRow(WINDOW, {
    { "FBKW_CancelButton", "Cancel", { tooltip = "Close without sending" } },
    { "FBKW_SendButton", "Send", { primary = true, tooltip = "Send the feedback" } },
  }, bh)
end

ui.window("Send Feedback", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Send Feedback", WINDOW_OPTS, drawWindow)
end)
