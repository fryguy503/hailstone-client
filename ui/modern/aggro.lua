-- Aggro meter: in this UI the meter is part of the target frame (target.lua); this keeps the game's AggroMeterWnd
-- from being drawn on its own (the game still shows and fills it, so its values stay current).
local API = hs.UI
ui.window("Aggro Meter", function()
  if not API.session.InGame() then return end
  API.windows.Shown("AggroMeterWnd")
end)
