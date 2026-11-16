-- Combat Abilities list (gamepad pack): stands in for CombatSkillSelectWnd (the list the combat ability window's
-- "S" opens). The character's combat abilities (disciplines) with their level and reuse, the focused one's
-- description, and Make Hotkey: A selects an ability (the stock list's click), X (or Make Hotkey) puts the selected
-- one on the cursor as the stock button does and opens the slot picker (slotpicker.lua) to drop it on a hot button.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local TB = dofile("lib/textbox.lua")
local AS = dofile("lib/assign.lua")
local C = T.C
local WIN = "CombatSkillSelectWnd"
local LIST = "CSSW_SkillSelectorList"

local listState = {}
local desc = {}
local side = {}
local wasShown = false

-- The selected ability's hotbutton onto a slot of the pad's choosing.
local function assign(name)
  AS.start(name or "Combat ability", function()
    API.windows.ClickChild(WIN, "CSSW_MakeHotkeyButton", "left")
    return true
  end, "Combat Skills")
end

local function testOpener() API.windows.ClickChild("CombatAbilityWnd", "CAW_OpenCombatSkillSelectWnd", "left") end

ui.window("Combat Skills", function()
  CH.testOpen(WIN, testOpener)
  if not W.shown(WIN) then
    wasShown = false
    return
  end
  T.frame()
  if not wasShown then listState.focusNext = 1 end
  wasShown = true
  if W.begin("Combat Skills", { w = 1500, h = 920, title = "Combat Abilities", subtitle = "Pick one for a hot button",
        hints = { { "a", "Select" }, { "x", "Make hotkey" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    local listW = cw * 0.55
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local row, texts = CH.list(WIN, LIST, listState, listW, y1 - y0, { rowH = 58, empty = "You have no combat abilities.",
      columns = { { 1, 0.62 }, { 2, 0.14, 1 }, { 7, 0.24, 1 } } })
    if row and W.pressed("x") and AS.settled("Combat Skills") then
      API.windows.ListClick(WIN, LIST, row, "left")
      assign(texts and texts[1])
    end
    W.toSide(side, row)
    local rx0 = x0 + listW + T.px(24)
    local bh = T.px(80)
    local text = API.windows.Text(WIN, "CSSW_SkillDescriptionStmlBox") or ""
    if text == "" then
      T.font(20)
      T.draw(rx0, y0, "Select an ability to read about it.", C.faint)
      T.unfont()
    else
      TB.draw(text, rx0, y0, x1, y1 - bh - T.px(16), desc)
    end
    ui.SetCursorPos(rx0 - wx, y1 - bh - wy)
    W.sideBefore(side, 1)
    local _, _, sel = API.windows.ListInfo(WIN, LIST)
    if T.button("##makehotkey", "Make Hotkey", x1 - rx0, bh, { primary = true, glyph = "x",
          disabled = not CH.enabled(WIN, "CSSW_MakeHotkeyButton") or (sel or -1) < 0, sub = "Put it on a hot button", align = 0 }, pad) then
      local t = sel and API.windows.ListRow(WIN, LIST, sel)
      assign(t and t[1])
    end
    W.sideAfter(side, 1, 1, listState)
  end
  if W.finish() then API.windows.Show(WIN, false) end
end)
