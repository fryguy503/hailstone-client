-- Server add-ons (Hailstone): a required add-on that failed once the character is in the world (hs.AddonPolicy;
-- docs/plugins.md "Opt-in add-on policies"). It shows only while there is such a failure, says the player can keep
-- playing, and offers Retry when the failed add-on can be tried again. Its close box puts it away until the next
-- failure. It stands in for the client's own window ("AddonPolicy"), which stays off while this runs. From an idea
-- in fryguy503's PR #93.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "AddonPolicy"
local closed = nil -- the failure's serial the player closed the window on
local q = nil -- this frame's failure (hs.AddonPolicy), read by the options and the body made once below

local OPTS = {
  keepTitle = true,
  rollup = false,
  size = function() return 420, 120 end,
  place = function(W, H, w, h) return W - w - 24, 70 end,
  onClose = function() if q then closed = q.serial end end,
}
local function body(x, y, w, h, s)
  local cx = ui.GetCursorPos()
  ui.PushTextWrapPos(cx + w)
  ui.TextColored(C.danger[1], C.danger[2], C.danger[3], 1, q.message)
  ui.TextWrapped("You can keep playing on this server.")
  ui.PopTextWrapPos()
  if q.can_retry then
    ui.Spacing()
    if ui.Button("Retry##addonpolicy", 120 * s, K.ButtonH(s)) then API.addons.PolicyAction("retry", q.serial) end
  end
end

ui.window("Server add-ons", function()
  q = API.session.InGame() and API.addons.Policy() or nil
  OPTS.shown = q ~= nil and q.serial ~= closed
  hud.Dialog(WINDOW, "Server add-ons", OPTS, body)
end)
