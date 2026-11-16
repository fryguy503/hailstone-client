-- Group mentoring: replaces the game's GroupMentorWindow (the leadership window's Group Mentoring button). The
-- protege (None, or a member of the group), the share of your experience to give (a slider and its number box,
-- which the window keeps in step and holds to 100) and Accept (or Enter in the box), which sends the mentoring
-- and closes. The fields and Accept are the window's (app/lua_ui_progress.cpp), so what the server gets is its.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local WINDOW = "GroupMentorWindow"

local OPTS = {
  size = function() return 300, 180 end,
  resizable = { 240, 170 },
}
local function body(x, y, w, h, s)
  K.Caption("Protege")
  W.Combo(WINDOW, "GM_SelectPlayer")
  if ui.IsItemHovered() then ui.SetTooltip("The group member to mentor (None: stop mentoring)") end
  ui.Dummy(0, 2 * s)
  K.Caption("Share of your experience")
  local boxW = 56 * s
  W.Slider(WINDOW, "GM_Slider", { width = -(boxW + 8 * s), format = function(v) return v .. "%" end })
  ui.SameLine()
  local _, committed = W.Edit(WINDOW, "GM_SliderInput", { width = boxW, max = 4 })
  if committed and ui.IsKeyPressed(ui.Key.Enter) then API.windows.ClickChild(WINDOW, "GM_SliderInput", "left") end
  local bh = K.ButtonH(s)
  local _, cy = ui.GetCursorScreenPos()
  ui.SetCursorScreenPos(x, math.max(cy + 4 * s, y + h - bh))
  W.ButtonRow(WINDOW, {
    { "GM_AcceptButton", "Accept", { primary = true, tooltip = "Mentor the protege with this share of your experience" } },
  }, bh)
end

local function testOpen() API.windows.Show(WINDOW, true) end
ui.window("Group Mentoring", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  hud.Dialog(WINDOW, "Group Mentoring", OPTS, body)
end)
