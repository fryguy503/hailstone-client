-- Server requirements can be put away like any other window; recovery stays the player's choice.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color

ui.window("Server add-ons", function()
  local shown = API.windows.Shown("AddonPolicy")
  if not API.session.InGame() or not shown then return end
  local q = API.addons.Policy()
  if not q or (not q.degraded and #q.required == 0) then return end
  hud.Window(nil, "Server add-ons", {
    shown = function() return true end,
    onClose = function() API.windows.Show("AddonPolicy", false) end,
    size = function() return 440, 130 + math.min(#q.required, 4) * 20 + (q.degraded and 32 or 0) end,
    place = function(W, H, w, h) return W - w - 24, 70 end,
  }, function(x, y, w, h, s)
    ui.SetCursorPos(10 * s, 8 * s)
    local c = q.degraded and C.danger or C.text
    ui.TextColored(c[1], c[2], c[3], 1, "Server add-ons")
    ui.PushTextWrapPos(w - 10 * s)
    ui.TextWrapped(q.message ~= "" and q.message or "This server requires these add-ons while you play.")
    ui.PopTextWrapPos()
    if #q.required > 0 then
      ui.BeginChild("##addonpolicyrequired", w - 20 * s, math.min(#q.required, 4) * 20 * s, false)
      for _, identity in ipairs(q.required) do ui.BulletText(identity) end
      ui.EndChild()
    end
    if q.degraded then
      if not q.can_retry then ui.BeginDisabled() end
      if ui.Button("Retry##addonpolicy", 120 * s, 25 * s) then API.addons.PolicyAction("retry", q.serial) end
      if not q.can_retry then ui.EndDisabled() end
      ui.SameLine()
      if ui.Button("Camp##addonpolicy", 120 * s, 25 * s) then API.addons.PolicyAction("camp", q.serial) end
    end
    if not q.can_stop then ui.BeginDisabled() end
    if ui.Button("Emergency stop required add-ons##addonpolicy", 0, 25 * s) then
      API.addons.PolicyAction("stop", q.serial)
    end
    if not q.can_stop then ui.EndDisabled() end
  end)
end)
