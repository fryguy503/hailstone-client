-- Inspect (gamepad pack): stands in for InspectWnd (another player's gear; /inspect, the target's right-click menu).
-- The worn slots as the inventory's grid with the weapons below, the focused slot's item by name beside it with
-- the whole gear listed, and the player's inspect message: yours is set with the on-screen keyboard (the game saves
-- it), another's is shown. The inspect answer has names and icons only, so a slot has nothing to press. Done is the
-- window's button (B too).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local C = T.C
local WIN = "InspectWnd"

-- InvSlot0..22 are the worn slots in hs.Item's numbering (0 charm .. 22 ammo).
local GRID = {
  { 1, 2, 3, 4, 5 },     -- ear, head, face, ear, neck
  { 6, 7, 8, 17, 20 },   -- shoulders, arms, back, chest, waist
  { 9, 10, 12, 15, 16 }, -- wrists, hands, fingers
  { 18, 19, 0, 21, 22 }, -- legs, feet, charm, power source, ammo
  { 13, 14, 11 },        -- primary, secondary, range
}
local NAME = {
  [0] = "Charm", [1] = "Ear", [2] = "Head", [3] = "Face", [4] = "Ear", [5] = "Neck", [6] = "Shoulders", [7] = "Arms",
  [8] = "Back", [9] = "Wrist", [10] = "Wrist", [11] = "Range", [12] = "Hands", [13] = "Primary", [14] = "Secondary",
  [15] = "Finger", [16] = "Finger", [17] = "Chest", [18] = "Legs", [19] = "Feet", [20] = "Waist", [21] = "Power Source",
  [22] = "Ammo",
}
local ORDER = { 2, 3, 5, 6, 17, 8, 7, 9, 10, 12, 20, 18, 19, 1, 4, 15, 16, 0, 21, 13, 14, 11, 22 }

local st = CH.new()
local wasShown = false

ui.window("Inspect", function()
  if not W.shown(WIN) then
    st.kb.open, st.pick, wasShown = false, nil, false
    return
  end
  T.frame()
  local appearing = not wasShown
  wasShown = true
  local title = API.windows.Title(WIN) or ""
  local own = CH.enabled(WIN, "INSW_Edit")
  if W.begin("Inspect", { w = 1500, h = 940, title = "Inspect", subtitle = title,
        hints = CH.hints(st, own and { { "a", "Edit message" }, { "b", "Close" } } or { { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(st, x0, y0, x1, y1) then
      -- the slots
      local size, gap = T.px(96), T.px(12)
      local focusedSlot
      for r, row in ipairs(GRID) do
        local rowW = #row * size + (#row - 1) * gap
        local sx = x0 + (5 * size + 4 * gap - rowW) / 2
        local sy = y0 + (r - 1) * (size + gap) + (r == 5 and T.px(14) or 0)
        for c, n in ipairs(row) do
          ui.SetCursorPos(sx + (c - 1) * (size + gap) - wx, sy - wy)
          if appearing and r == 1 and c == 1 then ui.SetKeyboardFocusHere() end
          local item = API.inventory.SlotItem(WIN, "InvSlot" .. n)
          local _, focused = T.slot("##ins" .. n, item, size, { label = NAME[n] }, pad)
          if focused then focusedSlot = n end
        end
      end
      -- the gear by name; the focused slot's on top
      local gx = x0 + 5 * size + 4 * gap + T.px(40)
      local gy = y0
      if focusedSlot then
        local item = API.inventory.SlotItem(WIN, "InvSlot" .. focusedSlot)
        T.font(18)
        T.draw(gx, gy, NAME[focusedSlot]:upper(), C.dim)
        T.unfont()
        T.font(26)
        T.draw(gx, gy + T.px(24), item and T.fit(item.name or "", x1 - gx) or "Nothing", item and (item.magic and C.accent or C.text) or C.faint)
        T.unfont()
      else
        T.font(20)
        T.draw(gx, gy + T.px(12), "Move over a slot to see its item.", C.faint)
        T.unfont()
      end
      gy = gy + T.px(76)
      ui.DrawLine(gx, gy, x1, gy, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
      gy = gy + T.px(10)
      T.font(19)
      local _, lh = ui.CalcTextSize("Ag")
      local maxY = y0 + 5 * (size + gap) + T.px(14)
      local any = false
      for _, n in ipairs(ORDER) do
        local item = API.inventory.SlotItem(WIN, "InvSlot" .. n)
        if item and gy + lh <= maxY then
          any = true
          T.draw(gx, gy, NAME[n], n == focusedSlot and C.accent or C.dim)
          T.draw(gx + T.px(170), gy, T.fit(item.name or "", x1 - gx - T.px(170)), item.magic and C.accent or C.text)
          gy = gy + lh + T.px(2)
        end
      end
      if not any then T.draw(gx, gy, "Nothing worn.", C.faint) end
      T.unfont()
      -- the message, and Done
      local my = y0 + 5 * (size + gap) + T.px(34)
      local bh = T.px(76)
      ui.SetCursorPos(x0 - wx, my - wy)
      local bw = T.px(240)
      if own then
        CH.field(st, WIN, "INSW_Edit", "Your inspect message", cw - bw - T.px(12), bh, { max = 254, prompt = "Your inspect message" })
      else
        local text = CH.text(WIN, "INSW_Edit")
        T.panel(x0, my, x0 + cw - bw - T.px(12), my + bh, 1, T.px(12), C.sunken)
        T.font(17)
        T.draw(x0 + T.px(14), my + T.px(8), "MESSAGE", C.dim)
        T.unfont()
        T.font(21)
        T.draw(x0 + T.px(14), my + T.px(34), text ~= "" and T.fit((text:gsub("\n", " ")), cw - bw - T.px(40)) or "No message.", text ~= "" and C.text or C.faint)
        T.unfont()
        ui.Dummy(cw - bw - T.px(12), bh)
      end
      ui.SameLine(0, T.px(12))
      CH.button(WIN, "DoneButton", "Done", bw, bh, { primary = true })
    end
  end
  if CH.back(st, W.finish()) then API.windows.ClickChild(WIN, "DoneButton", "left") end
end)
