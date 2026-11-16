-- Video modes: replaces the game's VideoModesWindow (the EQ menu or its key; Options > Display sets the
-- resolution itself). This client's display modes: the resolutions as a list, the refresh rate and bit depth of the one selected (filled in full
-- screen only, as the original's) and Accept, which sets the mode (and in full screen asks to keep it). All of it
-- is the original's (app/lua_ui_video.cpp); a double click on a resolution is Accept.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local WINDOW = "VideoModesWindow"

local WINDOW_OPTS = {
  size = function() return 360, 360 end,
  resizable = { 260, 240 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local _, fh = ui.CalcTextSize("Ag")
  local fieldH = fh + 8 * s
  W.List(WINDOW, "VIDW_VideoResolutionsList", 0, -(bh + fieldH * 2 + 24 * s), {
    columns = { [1] = { "Resolution", 0 } },
    doubleClick = function() API.windows.ClickChild(WINDOW, "VIDW_AcceptButton", "left") end,
  })
  ui.Dummy(0, 2 * s)
  local cw = 100 * s
  local function field(caption, id)
    local cx = ui.GetCursorPos()
    K.Caption(caption)
    ui.SameLine(cx + cw)
    local choices = API.windows.ComboInfo(WINDOW, id)
    if choices and #choices > 0 then W.Combo(WINDOW, id) else K.Caption("-") end
  end
  field("Refresh rate", "VIDW_RefreshRateCombobox")
  field("Bit depth", "VIDW_BitDepthCombobox")
  W.ButtonRow(WINDOW, {
    { "VIDW_AcceptButton", "Accept", { primary = true, tooltip = "Switch to the selected mode" } },
  }, bh)
end

ui.window("Video Modes", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Video Modes", WINDOW_OPTS, drawWindow)
end)
