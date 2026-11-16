-- Bandolier (gamepad pack): stands in for BandolierWnd (the bandolier key, the EQ menu's Bandolier). The weapon
-- sets as a list (A selects one, X activates it: the window's Use), the selected set's four weapons (primary,
-- secondary, range, ammo; green when you carry the item, red when you do not, as the game tints them), and the
-- window's buttons: Activate, Save (stores what you hold now as the selected set; the game asks for a name), Delete,
-- Auto Swap (a set is swapped to as soon as it is selected) and Hot Button, which puts the set's hotbutton on the
-- cursor and opens the slot picker to drop it on a hot button.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local AS = dofile("lib/assign.lua")
local C = T.C
local WIN = "BandolierWnd"
local SLOTS = { "Primary", "Secondary", "Range", "Ammo" }

local listState = {}
local side = {}
local wasShown = false

ui.window("Bandolier", function()
  CH.testOpen(WIN)
  if not W.shown(WIN) then
    wasShown = false
    return
  end
  T.frame()
  if not wasShown then listState.focusNext = 1 end
  wasShown = true
  local _, _, selected = API.windows.ListInfo(WIN, "WeaponSetList")
  local name = ""
  if selected and selected >= 0 then
    local t = API.windows.ListRow(WIN, "WeaponSetList", selected)
    name = t and t[1] or ""
  end
  if W.begin("Bandolier", { w = 1400, h = 860, title = "Bandolier", subtitle = name ~= "" and name or "No set selected",
        hints = { { "a", "Select" }, { "x", "Activate" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    local listW = cw * 0.45
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local row = CH.list(WIN, "WeaponSetList", listState, listW, y1 - y0, { rowH = 60,
      empty = "No weapon sets: hold the weapons and Save." })
    if row and W.pressed("x") then
      API.windows.ListClick(WIN, "WeaponSetList", row, "left")
      API.windows.ClickChild(WIN, "UseButton", "left")
    end
    W.toSide(side, row)
    -- the selected set's weapons
    local rx0 = x0 + listW + T.px(30)
    local rw = x1 - rx0
    local size = math.min(T.px(120), (rw - 3 * T.px(16)) / 4)
    local gap = (rw - 4 * size) / 3
    for i = 0, 3 do
      local id = "DisplayButton" .. i
      local sx = rx0 + i * (size + gap)
      local item = API.windows.ChildTooltip(WIN, id) or ""
      T.rect(sx, y0, sx + size, y0 + size, item ~= "" and C.raised or C.sunken, 1, T.px(12))
      if item ~= "" then
        local tex, u0, v0, u1, v1 = API.windows.ChildArt(WIN, id, "decal")
        if type(tex) == "userdata" then
          local m = math.floor(size * 0.1)
          T.icon(tex, u0, v0, u1, v1, sx + m, y0 + m, sx + size - m, y0 + size - m)
        end
        local r, g = API.windows.ChildColor(WIN, id, "decal")
        if r then T.outline(sx, y0, sx + size, y0 + size, (g or 0) > (r or 0) and C.good or C.bad, 1, T.px(12), T.px(3)) end
      end
      T.font(17)
      T.textIn(sx - gap / 2, y0 + size + T.px(4), sx + size + gap / 2, y0 + size + T.px(30), SLOTS[i + 1], C.dim, 0.5)
      T.textIn(sx - gap / 2, y0 + size + T.px(30), sx + size + gap / 2, y0 + size + T.px(54), T.fit(item, size + gap), C.text, 0.5)
      T.unfont()
    end
    -- the buttons
    local by = y0 + size + T.px(76)
    local bh = T.px(70)
    local buttons = {
      { "UseButton", "Activate", "Equip the selected set" },
      { "AddButton", "Save", "What you hold now, as the selected set" },
      { "DeleteButton", "Delete", "The selected set" },
      { "AutoSwapButton", "Auto Swap", nil, true },
      { "MkHotButton", "Hot Button", "Put the set on a hot button" },
    }
    for k, b in ipairs(buttons) do
      ui.SetCursorPos(rx0 - wx, by - wy)
      W.sideBefore(side, k)
      if b[4] then
        CH.toggle(WIN, b[1], b[2], rw, bh, { on = "On: selecting a set swaps to it", off = "Off", keep = true })
      elseif b[1] == "MkHotButton" then
        if T.button("##bandhot", b[2], rw, bh, { sub = b[3], align = 0, disabled = not CH.enabled(WIN, b[1]) or name == "" }, pad) then
          AS.start(name, function() API.windows.ClickChild(WIN, "MkHotButton", "left") return true end, "Bandolier")
        end
      else
        CH.button(WIN, b[1], b[2], rw, bh, { sub = b[3], align = 0, primary = k == 1, keep = true })
      end
      W.sideAfter(side, k, #buttons, listState)
      by = by + bh + T.px(10)
    end
  end
  if W.finish() then API.windows.Show(WIN, false) end
end)
