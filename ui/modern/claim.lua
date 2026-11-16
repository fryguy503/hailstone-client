-- Claim rewards: replaces the game's ClaimWnd (/claim: the account's veteran rewards). The rewards that may be
-- claimed (greyed when none are left) and, beside them or below when the window is narrow, the items the selected
-- one gives; Refresh, Cancel and Claim (the window enables it for a reward that can be claimed). The lists and
-- buttons are the window's (hs.ListClick, hs.ClickChild), so the claim the server gets is the window's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "ClaimWnd"

local WINDOW_OPTS = {
  size = function() return 620, 340 end,
  resizable = { 320, 240 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local _, lh = ui.CalcTextSize("Ag")
  local footer = K.FooterH(s)
  local listH = h - footer - lh - 4
  local wide = w >= 520 * s
  local lw = wide and (w - 12 * s) * 0.55 or w
  local ih = wide and listH or (listH - lh - 8 * s) / 2
  ui.BeginGroup()
  K.Caption("Rewards")
  local lx, ly = ui.GetCursorScreenPos()
  local rows = W.List(WINDOW, "ClaimFeatureList", lw, wide and listH or ih,
    { columns = { [1] = { "Reward", 0 }, [2] = { "Left", 50 } } })
  if rows == 0 then T.Text(lx + 8 * s, ly + lh + 10 * s, "No rewards to claim", C.faint, T.font.small) end
  ui.EndGroup()
  if wide then ui.SameLine(0, 12 * s) end
  ui.BeginGroup()
  K.Caption("Items in the selected reward")
  W.List(WINDOW, "ClaimItemList", wide and -1 or w, ih,
    { columns = { [1] = { "Item", 0 }, [2] = { "Charges", 60 } } })
  ui.EndGroup()
  W.ButtonRow(WINDOW, {
    { "ClaimRefreshButton", "Refresh", { tooltip = "Ask the server for the rewards again" } },
    { "ClaimCancelButton", "Cancel" },
    { "ClaimClaimButton", "Claim", { primary = true, tooltip = "Claim the selected reward on this character" } },
  }, bh)
end

ui.window("Claim Rewards", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Claim Rewards", WINDOW_OPTS, drawWindow)
end)
