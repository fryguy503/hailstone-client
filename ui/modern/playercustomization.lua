-- Set appearance: replaces the game's PlayerCustomizationWnd (/facepick, the inventory's Face button). A row per
-- feature with its two step buttons (greyed where the window disables them for your race and model), the heritage's
-- name, and Random, Cancel and Accept. Every button is the window's own (hs.ClickChild): the features are stepped
-- and tried on your model by the game's code, and Accept sends them.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "PlayerCustomizationWnd"

-- { the row's ScreenID stem, the name shown }
local ROWS = {
  { "Face", "Face" }, { "Hair", "Hair style" }, { "Hair_Color", "Hair color" }, { "Eyes", "Eyes" },
  { "Beard", "Beard" }, { "Beard_Color", "Beard color" }, { "TattooIndex", "Tattoo" },
  { "FacialAttachmentIndex", "Facial detail" },
}

local WINDOW_OPTS = {
  size = function() return 260, 360 end,
  resizable = { 220, 300 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local heritage = W.Text(WINDOW, "ParentId_Text")
  if heritage ~= "" then
    W.Field("Heritage", heritage)
    ui.Dummy(0, 2 * s)
  end
  local _, lh = ui.CalcTextSize("Ag")
  local rowH = math.max(lh + 8 * s, 24 * s)
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
  if ui.BeginTable("##features", 3, flags, 0, -(K.FooterH(s) + bh + 6 * s)) then
    ui.TableSetupColumn("##down", ui.TableColumnFlags.WidthFixed, rowH + 6 * s)
    ui.TableSetupColumn("##name", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("##up", ui.TableColumnFlags.WidthFixed, rowH + 6 * s)
    for _, row in ipairs(ROWS) do
      local down, up, text = row[1] .. "_Down_Button", row[1] .. "_Up_Button", row[1] .. "_Text"
      local on = W.State(WINDOW, text)
      ui.TableNextRow()
      ui.TableNextColumn()
      W.Button(WINDOW, down, "<", { width = rowH, height = rowH, tooltip = "Previous " .. row[2]:lower() })
      ui.TableNextColumn()
      local cx, cy = ui.GetCursorPos()
      local tw = ui.CalcTextSize(row[2])
      ui.SetCursorPos(cx + math.max(0, (ui.GetContentRegionAvail() - tw) / 2), cy + (rowH - lh) / 2)
      local c = on and C.text or C.faint
      ui.TextColored(c[1], c[2], c[3], 1, row[2])
      ui.TableNextColumn()
      W.Button(WINDOW, up, ">", { width = rowH, height = rowH, tooltip = "Next " .. row[2]:lower() })
    end
    ui.EndTable()
  end
  W.ButtonRow(WINDOW, { { "Random_Button", "Random", { tooltip = "Random features for your face" } } }, bh)
  ui.Dummy(0, 2 * s)
  W.ButtonRow(WINDOW, {
    { "Cancel_Button", "Cancel", { tooltip = "Put your features back" } },
    { "Accept_Button", "Accept", { primary = true, tooltip = "Keep these features" } },
  }, bh)
end

ui.window("Set Appearance", function()
  if not API.session.InGame() and API.session.GameState() ~= 2 then return end -- in game, or at character creation
  hud.Dialog(WINDOW, "Set Appearance", WINDOW_OPTS, drawWindow)
end)
