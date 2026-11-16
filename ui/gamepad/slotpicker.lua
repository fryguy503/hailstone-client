-- The slot picker (gamepad pack): where a window's "assign" (X) puts an ability, a skill, an AA or a discipline
-- (lib/assign.lua). LB / RB go through the cross hotbar's three sets (LT, RT, LT + RT: the slots the triggers fire,
-- docs/gamepad.md section 3) and the ten hot bars; A puts it on the focused slot, B cancels. A slot is a hot button
-- of the bar's current page, so the hotbutton goes there as a drop from the cursor does (hs.ClickHotButton with it on
-- the cursor), and whatever the slot held is replaced.
-- Opened from the main menu's Hot Bars (AS.edit) it holds nothing: A opens the focused slot's own menu, the game's
-- hotbutton menu the right button held on it opens (hs.ClickHotButton "rightheld" at the slot; menus.lua takes the
-- pad): Assign Social (a new social opens the social editor), Assign Label, Assign Icon, Change Size, Clear,
-- Activate ...; on a hot bar's page Y opens that bar's own menu (hs.HotBarMenu: hotbutton sets, Clear current
-- hotbuttons, Button Size, Show Key Map, Show Spinner, Open New Bar), as a right click on the bar does.
local API = hs.UI
local W = dofile("lib/window.lua")
local T = W.T -- the window module's theme: the scale W.begin computed this frame
local pad = W.pad
local C = T.C

local SLOT_BUTTONS = { "left", "up", "right", "down", "x", "y", "b", "a" }
local PAGES = 3 + 10 -- the cross sets, then hot bars 1..10
local page = 1
local focusFirst = false -- a page was switched: the pad's focus onto its first slot

local function screenOf(bar) return bar == 1 and "HotButtonWnd" or ("HotButtonWnd" .. bar) end

local function pageTitle(p)
  if p == 1 then return "Cross hotbar: hold LT" elseif p == 2 then return "Cross hotbar: hold RT"
  elseif p == 3 then return "Cross hotbar: hold LT + RT" end
  return "Hot bar " .. (p - 3)
end

-- The slots of a page: { bar, button, glyph } (cross slots name the pad button that fires them).
local function slots(p)
  local out = {}
  if p <= 3 then
    for n = 1, 8 do
      local bar, button = pad.cross_slot(p, n)
      if bar then out[#out + 1] = { bar, button, SLOT_BUTTONS[n] } end
    end
  else
    for n = 1, 12 do out[#out + 1] = { p - 3, n } end
  end
  return out
end

local function close(a)
  GPSTATE.assign = nil
  if not a.edit and API.cursor.HoldingButton() then API.cursor.ClearButton() end -- cancelled, or what the slot held
  if a and a.from then pad.focus(a.from) end
end

ui.window("Assign To Slot", function()
  local a = GPSTATE.assign
  if not a then return end
  if not API.session.InGame() or (not a.edit and not API.cursor.HoldingButton()) then GPSTATE.assign = nil return end -- (the mouse dropped it)
  local title = a.edit and "Hot Bars" or ("Assign: " .. (a.label or ""))
  local hints = a.edit and page > 3 and { { "a", "Edit" }, { "y", "Bar menu" }, { "lb", "Previous" }, { "rb", "Next" }, { "b", "Close" } }
      or a.edit and { { "a", "Edit" }, { "lb", "Previous" }, { "rb", "Next" }, { "b", "Close" } }
      or { { "a", "Put it here" }, { "lb", "Previous" }, { "rb", "Next" }, { "b", "Cancel" } }
  if W.begin("Assign To Slot", { w = 1240, h = 440, title = title, subtitle = pageTitle(page), hints = hints }) then
    if W.pressed("lb") then page, focusFirst = (page + PAGES - 2) % PAGES + 1, true end
    if W.pressed("rb") then page, focusFirst = page % PAGES + 1, true end
    local x0, y0, x1, y1 = W.area()
    if a.edit and page > 3 and W.pressed("y") then API.hotbars.Menu(page - 3, (x0 + x1) / 2, (y0 + y1) / 2) end
    local list = slots(page)
    local size, gap = T.px(128), T.px(14)
    local perRow = page <= 3 and 8 or 6
    local rowW = perRow * size + (perRow - 1) * gap
    local sx = x0 + (x1 - x0 - rowW) / 2
    local wx, wy = ui.GetWindowPos()
    local focusedLabel
    for i, s in ipairs(list) do
      local col, row = (i - 1) % perRow, (i - 1) // perRow
      local cx0, cy0 = sx + col * (size + gap), y0 + T.px(10) + row * (size + gap + T.px(30))
      ui.SetCursorPos(cx0 - wx, cy0 - wy)
      ui.PushID(i)
      if focusFirst and i == 1 then
        ui.SetKeyboardFocusHere()
        focusFirst = false
      end
      local clicked = ui.InvisibleButton("##slot", size, size, ui.ButtonFlags.EnableNav)
      local focused = pad.focused()
      ui.PopID()
      local screen, child = screenOf(s[1]), "HB_Button" .. s[2]
      local kind, label, tex, u0, v0, u1, v1 = API.hotbars.Button(screen, child)
      local cx1, cy1 = cx0 + size, cy0 + size
      local r = T.px(12)
      T.rect(cx0, cy0, cx1, cy1, kind == "empty" and C.sunken or C.raised, 1, r)
      if tex then
        local m = math.floor(size * 0.12)
        T.icon(tex, u0, v0, u1, v1, cx0 + m, cy0 + m, cx1 - m, cy1 - m)
      elseif label and label ~= "" then
        T.font(16)
        T.textIn(cx0 + T.px(6), cy0, cx1 - T.px(6), cy1, T.fit(label, size - T.px(12)), C.text, 0.5)
        T.unfont()
      else
        T.font(15)
        T.textIn(cx0, cy0, cx1, cy1, "empty", C.faint, 0.5)
        T.unfont()
      end
      T.outline(cx0, cy0, cx1, cy1, C.edge, 1, r)
      -- what fires it: the pad button (a cross slot) or the bar's button number, under the slot
      if s[3] then
        T.font(16)
        T.glyph(pad, s[3], (cx0 + cx1) / 2, cy1 + T.px(18), T.px(28))
        T.unfont()
      else
        T.font(18)
        T.textIn(cx0, cy1, cx1, cy1 + T.px(32), tostring(s[2]), C.dim, 0.5)
        T.unfont()
      end
      if focused then
        T.focusRing(cx0, cy0, cx1, cy1, r)
        if a.edit then focusedLabel = (label and label ~= "") and label or "Empty slot"
        else focusedLabel = (label and label ~= "") and ("Replaces " .. label) or "Empty slot" end
      end
      if clicked and a.edit then
        API.hotbars.Click(screen, child, "rightheld", cx0 + size / 2, cy0 + size / 2) -- the slot's menu
      elseif clicked then
        -- the drop; what the slot held comes back on the cursor and is let go
        API.hotbars.Click(screen, child, "left")
        close(a)
      end
    end
    if focusedLabel then
      T.font(20)
      T.textIn(x0, y1 - T.px(36), x1, y1, focusedLabel, C.dim, 0.5)
      T.unfont()
    end
  end
  if W.finish() then close(a) end
end)
