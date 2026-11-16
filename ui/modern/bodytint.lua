-- Armor dye: replaces the game's BodyTintWnd (the inventory's tint button). The brush (a click opens the colour
-- picker on it), then a row per tintable slot: the armour's colour now (a click takes it as the brush) and its new
-- dye (a click paints the brush on it), with a mark on the slots that will change; the reagents needed, Reset and
-- Save Dye (a Vial of Prismatic Dye per changed slot; the window enables it when you have enough). Every swatch is
-- the window's own button (hs.ClickChild), so painting and dyeing are the window's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "BodyTintWnd"

-- { the slot's name in the ScreenIDs, the name shown }
local SLOTS = { { "Head", "Head" }, { "Chest", "Chest" }, { "Arms", "Arms" }, { "ForeArm", "Wrists" },
  { "Hands", "Hands" }, { "Legs", "Legs" }, { "Feet", "Feet" } }

local WINDOW_OPTS = {
  size = function() return 300, 380 end,
  resizable = { 240, 320 },
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local _, lh = ui.CalcTextSize("Ag")
  -- the brush
  local br, bg, bb = K.Tint(WINDOW, "TINT_CurBrushBtn")
  if K.Swatch("##brush", br, bg, bb, 30 * s, s, { width = 54 * s, tooltip = "Choose the brush's color" }) then
    API.windows.ClickChild(WINDOW, "TINT_CurBrushBtn", "left")
  end
  ui.SameLine(0, 10 * s)
  ui.BeginGroup()
  ui.Text("Brush")
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, K.Hex(br, bg, bb))
  ui.EndGroup()
  ui.Dummy(0, 2 * s)
  -- the slots
  local rowH = math.max(lh + 8 * s, 22 * s)
  local listH = -(K.FooterH(s) + lh + 8 * s)
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
  if ui.BeginTable("##tints", 4, flags, 0, listH) then
    ui.TableSetupScrollFreeze(0, 1)
    ui.TableSetupColumn("Slot", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Now", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Dye", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("##changed", ui.TableColumnFlags.WidthFixed, 14 * s)
    ui.TableHeadersRow()
    for _, slot in ipairs(SLOTS) do
      local now, dye = "TINT_O" .. slot[1] .. "ColorBtn", "TINT_" .. slot[1] .. "Btn"
      ui.TableNextRow()
      ui.TableNextColumn()
      local cx, cy = ui.GetCursorPos()
      ui.SetCursorPos(cx, cy + (rowH - lh) / 2)
      ui.Text(slot[2])
      ui.TableNextColumn()
      local nr, ng, nb = K.Tint(WINDOW, now)
      if K.Swatch("##" .. now, nr, ng, nb, rowH - 4 * s, s,
          { width = ui.GetContentRegionAvail(), tooltip = "Its color now (" .. K.Hex(nr, ng, nb) .. "); click to use it as the brush" }) then
        API.windows.ClickChild(WINDOW, now, "left")
      end
      ui.TableNextColumn()
      local dr, dg, db = K.Tint(WINDOW, dye)
      if K.Swatch("##" .. dye, dr, dg, db, rowH - 4 * s, s,
          { width = ui.GetContentRegionAvail(), tooltip = "Its new dye (" .. K.Hex(dr, dg, db) .. "); click to paint it with the brush" }) then
        API.windows.ClickChild(WINDOW, dye, "left")
      end
      ui.TableNextColumn()
      if K.Hex(nr, ng, nb) ~= K.Hex(dr, dg, db) then
        local px, py = ui.GetCursorScreenPos()
        ui.DrawCircleFilled(px + 5 * s, py + (rowH - 4 * s) / 2, 3.5 * s, C.gold[1], C.gold[2], C.gold[3], 1)
        ui.Dummy(10 * s, rowH - 4 * s)
        if ui.IsItemHovered() then ui.SetTooltip("Will be dyed") end
      end
    end
    ui.EndTable()
  end
  -- reagents
  local need = W.Text(WINDOW, "TINT_ReagentsNeeded")
  K.Caption(need ~= "" and need or "Reagents Needed: 0")
  local have = W.Text(WINDOW, "TINT_ReagentsLabel")
  if have ~= "" then
    ui.SameLine(0, 14 * s)
    K.Caption(have)
  end
  W.ButtonRow(WINDOW, {
    { "TINT_ResetBodytTint", "Reset", { tooltip = "Forget the changes" } },
    { "TINT_SaveTint", "Save Dye", { primary = true, tooltip = "Dye the marked slots (a Vial of Prismatic Dye each)" } },
  }, bh)
end

ui.window("Body Tint", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Armor Dye", WINDOW_OPTS, drawWindow)
end)
