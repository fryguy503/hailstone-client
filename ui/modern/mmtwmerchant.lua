-- Mercenary merchant: replaces the game's MMTW_MerchantWnd. The choice on the left (the type, then the mercenaries
-- of that type, a membership the account lacks in red, then the chosen one's stances), on the right what it costs
-- (purchase and upkeep as coins), its description, the picked stance's description, and Hire.
-- The drop-down, lists and Hire are the original's children (hs.ComboSelect, hs.ListClick, hs.ClickChild): picking a
-- mercenary asks the game for its details as the stock window does.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = hud.T
local C = T.color
local WINDOW = "MMTW_MerchantWnd"


-- A one-column list child as a plain list (no header).
local function list(id, h, empty, tip)
  local rows, selected = M.Rows(WINDOW, id)
  ui.PushID(id)
  ui.BeginChild("##l", 0, h, true)
  for i, r in ipairs(rows) do
    local c = r.color or C.text
    ui.PushStyleColor(ui.Col.Text, c[1], c[2], c[3], 1)
    if ui.Selectable((r.texts[1] or "") .. "##" .. i, i - 1 == selected) then API.windows.ListClick(WINDOW, id, i - 1, "left") end
    ui.PopStyleColor(1)
    if tip then M.Tip(tip) end
  end
  if #rows == 0 then ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, empty) end
  ui.EndChild()
  ui.PopID()
end

local OPTS = {
  size = function() return 600, 400 end,
  resizable = { 460, 300 },
}
local function body(x, y, w, h, s)
  local lh, btnH = M.Heights(s)
  local leftW = math.floor(w * 0.40)

  -- ---------------------------------------------------------------- the choice
  M.BeginPanel("##mmtwpick", leftW, h, s)
  M.Caption("Type")
  W.Combo(WINDOW, "TypeComboBox")
  M.Caption("Mercenary")
  local _, avail = ui.GetContentRegionAvail()
  list("SubtypeListBox", math.max(60 * s, (avail - lh * 2) * 0.6), "None for hire.",
    "Red: needs a membership this account lacks")
  M.Caption("Stance")
  list("StanceListBox", 0, "Pick a mercenary.", "Its description shows on the right")
  M.EndPanel()

  -- ---------------------------------------------------------------- the chosen one
  ui.SameLine(0, 8 * s)
  ui.BeginGroup()
  local rw = w - leftW - 8 * s
  M.BeginPanel("##mmtwcost", rw, lh * 2 + 20 * s, s)
  M.MercCosts(WINDOW, s)
  M.EndPanel()
  local infoH = h - (lh * 2 + 20 * s) - btnH - lh - 24 * s
  M.BeginPanel("##mmtwinfo", rw, infoH * 0.62, s)
  M.StmlOr(WINDOW, "InfoText", "Pick a mercenary to read about it.")
  M.EndPanel()
  M.Caption("Stance")
  M.BeginPanel("##mmtwstance", rw, infoH * 0.38, s)
  M.StmlOr(WINDOW, "StanceInfoText", "Pick a stance to read about it.")
  M.EndPanel()
  W.Button(WINDOW, "HireButton", "Hire", { primary = true, width = rw, height = btnH,
    tooltip = "Hire the selected mercenary" })
  ui.EndGroup()
end

ui.window("Mercenary Merchant", function()
  if not API.session.InGame() then return end
  M.TestOpen(WINDOW)
  hud.Dialog(WINDOW, "Mercenary Merchant", OPTS, body)
end)
