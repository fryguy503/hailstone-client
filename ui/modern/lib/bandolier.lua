-- The bandolier, over the game's BandolierWnd: drawn by the Bandolier window (bandolier.lua) and the inventory's
-- Bandolier page (inventory.lua). Both act on the same game window, so its list selection, Auto Swap and buttons
-- are one state whichever draws them; the game window need not be shown for them to work.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local tiles = dofile("lib/hudx_tiles.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color

local M = {}
M.WINDOW = "BandolierWnd"
local WINDOW = M.WINDOW
local SLOTS = { "Primary", "Secondary", "Range", "Ammo" }

-- The selected set's name, "" when none is selected.
function M.SetName()
  local rows, _, selected = API.windows.ListInfo(WINDOW, "WeaponSetList")
  if not rows or not selected or selected < 0 then return "" end
  local texts = API.windows.ListRow(WINDOW, "WeaponSetList", selected)
  return texts and texts[1] or ""
end

-- The selected set's name as a heading.
function M.Heading(s)
  local name = M.SetName()
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, name ~= "" and name or "No set selected")
  ui.PopFont()
  ui.Dummy(0, 2 * s)
end

-- The selected set's weapons: a tile per slot with its name under it (a green ring when you carry the item, red
-- when you do not, as the game tints them).
function M.Weapons(s)
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  local gap = 8 * s
  local size = math.min(48 * s, (w - gap * 3) / 4)
  local cellW = (w - gap * 3) / 4
  local _, th = T.TextSize("Ag", T.font.tiny)
  for i = 0, 3 do
    local id = "DisplayButton" .. i
    local cx = x + i * (cellW + gap)
    local ix = cx + (cellW - size) / 2
    local name = API.windows.ChildTooltip(WINDOW, id) or ""
    local r, g = API.windows.ChildColor(WINDOW, id, "decal")
    local ring = nil
    if name ~= "" and r then
      ring = (g or 0) > (r or 0) and C.pet or C.danger
    end
    if name ~= "" then
      tiles.Icon(WINDOW, id, ix, y, size, s, ring, { radius = 6 })
    else
      T.Icon(ix, y, size, s, nil, 0, 0, 1, 1, nil, { radius = 6, wellAlpha = 0.5 })
    end
    T.Hit("##bw" .. i, ix, y, ix + size, y + size)
    if ui.IsItemHovered() then
      if name == "" then
        ui.SetTooltip(SLOTS[i + 1] .. ": nothing in this set")
      else
        local have = ring and ring ~= C.danger
        ui.SetTooltip(name .. "\n" .. (have and "You carry it" or "Not in your possessions"))
      end
    end
    T.Text(cx + cellW / 2, y + size + 3 * s, SLOTS[i + 1], name ~= "" and C.muted or C.faint, T.font.tiny, "center")
  end
  ui.SetCursorScreenPos(x, y)
  ui.Dummy(w, size + th + 6 * s)
end

-- The sets, w x h as W.List takes them; a double click activates one.
local LIST_OPTS = { doubleClick = function() API.windows.ClickChild(WINDOW, "UseButton", "left") end }
function M.List(w, h)
  W.List(WINDOW, "WeaponSetList", w, h, LIST_OPTS)
end

-- The height M.Buttons takes.
function M.ButtonsHeight(s)
  local _, lh = ui.CalcTextSize("Ag")
  return (lh + 10 * s) * 2 + 8 * s
end

-- The game's buttons: Activate, Save, Delete; then Auto Swap and Hot Button.
function M.Buttons(s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  W.ButtonRow(WINDOW, {
    { "UseButton", "Activate", { primary = true, tooltip = "Equip the selected set's weapons" } },
    { "AddButton", "Save", { tooltip = "Store the weapons you hold now as the selected set (asks for a name)" } },
    { "DeleteButton", "Delete", { tooltip = "Delete the selected set" } },
  }, btnH)
  W.Check(WINDOW, "AutoSwapButton", "Auto swap", { tooltip = "Swap to a set as soon as you click it in the list" })
  ui.SameLine()
  local avail = ui.GetContentRegionAvail()
  local bw = math.min(avail, 110 * s)
  local cx, cy = ui.GetCursorPos()
  ui.SetCursorPos(cx + avail - bw, cy)
  W.Button(WINDOW, "MkHotButton", "Hot Button", { width = bw, tooltip = "A hot button that activates the selected set" })
end

return M
