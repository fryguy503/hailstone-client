local API = hs.UI
local hud = dofile("lib/hud.lua")
local X = dofile("lib/item_exp_transfer.lua")
local WINDOW = "ItemExpTransferWnd"
local OPTIONS = {
  size = function() return 440, 330 end,
  resizable = { 320, 280 },
  keepTitle = true,
  place = function(sw, sh, w, h) return (sw - w) / 2, (sh - h) / 2 end,
}

-- The state's record, refilled each frame rather than made anew (hs.ItemExpTransfer's `into`).
local record = {}

ui.window("Evolving Item Transfer", function()
  if not API.session.InGame() then X.reset(); return end
  local state = API.evolving.State(record)
  X.sync(state)
  if not state.shown then return end
  hud.Dialog(WINDOW, "Evolving Item Experience Transfer", OPTIONS, function(_, _, _, _, s)
    ui.BeginChild("##evolve-content", 0, 0, false)
    X.draw(state, {
      width = -1, gap = 10 * s,
      button = function(id, caption, enabled)
        ui.BeginDisabled(not enabled)
        local clicked = ui.Button(caption .. id, -1, 0)
        ui.EndDisabled()
        return enabled and clicked
      end,
    })
    ui.EndChild()
  end)
end)
