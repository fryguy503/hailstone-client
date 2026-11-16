-- Server dialog: replaces the game's LargeDialogWindow (the server's large text popups: the welcome message, quest
-- and event texts). The server's title, its text (STML: colours and links) in a padded box at a readable size that
-- scrolls when long, and its buttons: OK, or Yes and No with the labels the server gave them. The window's sound
-- controls (a voice file's volume, Replay, Mute) show only when the dialog has them, as on the stock window.
-- Everything goes through the window's children (hs.ClickChild, hs.SetSlider), so the answer is the game's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "LargeDialogWindow"

-- The button's label: the server's when it set one, else the stock one.
local function label(id, default)
  local t, set = API.windows.Text(WINDOW, id)
  if set and t and t ~= "" then return t end
  return default
end

ui.window("Large Dialog", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Message", {
    size = function() return 520, 400 end,
    resizable = { 300, 180 },
    place = K.Centered,
  }, function(x, y, w, h, s)
    local bh = K.ButtonH(s)
    local sound = API.windows.ChildShown(WINDOW, "LDW_ReplayButton") == true
    local below = K.FooterH(s) + (sound and (bh + 6 * s) or 0)
    K.TextBox(WINDOW, "LDW_TextBox", 0, -below, { font = T.font.normal })
    if sound then
      W.Button(WINDOW, "LDW_ReplayButton", "Replay", { height = bh, tooltip = "Play the message's voice again" })
      ui.SameLine()
      W.Button(WINDOW, "LDW_MuteButton", "Mute", { height = bh })
      ui.SameLine()
      local vol = W.Text(WINDOW, "LDW_SoundVolumeValueLabel")
      local tw = ui.CalcTextSize(vol)
      W.Slider(WINDOW, "LDW_SoundVolumeSlider", {
        width = math.max(60 * s, ui.GetContentRegionAvail() - tw - 12 * s),
        format = function(v) return "" end,
      })
      ui.SameLine()
      ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, vol)
    end
    W.ButtonRow(WINDOW, {
      { "LDW_YesButton", label("LDW_YesButton", "Yes"), { primary = true } },
      { "LDW_NoButton", label("LDW_NoButton", "No") },
      { "LDW_OkButton", label("LDW_OkButton", "OK"), { primary = true } },
    }, bh)
  end)
end)
