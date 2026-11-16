-- Target of target: in this UI the target's target is part of the target frame (target.lua: its name and health
-- under the target's), so this keeps the game's TargetOfTargetWindow from being drawn on its own as well (the game
-- still keeps it, so switching back to Classic finds it as it was).
local API = hs.UI
ui.window("Target of Target", function()
  if not API.session.InGame() then return end
  API.windows.Shown("TargetOfTargetWindow")
end)
