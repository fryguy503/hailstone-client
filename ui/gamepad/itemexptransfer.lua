local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local X = dofile("lib/item_exp_transfer.lua")
local WINDOW = "ItemExpTransferWnd"

ui.window("Evolving Item Transfer", function()
  if not API.session.InGame() then X.reset(); return end
  local state = API.evolving.State()
  X.sync(state)
  if not state.shown or not W.shown(WINDOW) then X.reset(); return end
  if W.begin("Evolving Item Transfer", {
      w = 920, h = 760, title = "Evolving Item Experience Transfer",
      hints = { { "a", "Choose / confirm" }, { "b", "Back / close" } }, alwaysFocus = true,
    }) then
    local _, _, _, _, width, height = W.area()
    ui.BeginChild("##evolve-content", width, height, false)
    X.draw(state, {
      width = -1, gap = T.px(18),
      button = function(id, caption, enabled)
        local available = ui.GetContentRegionAvail()
        return T.button(id, caption, available, T.px(58), { disabled = not enabled }, pad)
      end,
    })
    ui.EndChild()
  end
  if W.finish() and not X.back() then X.reset(); API.windows.Show(WINDOW, false) end
end)
