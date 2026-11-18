-- Alternate Advancement (gamepad pack): stands in for AAWindow. LB / RB switch the stock tabs (General, Archetype,
-- Class, Special); the list is the stock window's (title, rank cur/max, cost, category, grey when it cannot be
-- trained), and the focused row is the stock selection, so the right column shows its description and reuse timer.
--   A   Train: the stock Train button (the game asks to confirm with the cost unless AANoConfirm is set)
--   Y   Use: an owned ability with a reuse time (the stock Hotkey button's hotbutton, pressed as a hot button)
--   X   Assign it to a hot button / cross hotbar slot (the Hotkey button's hotbutton)
-- Under the description: the points, "Can Purchase" (the stock filter) and the AA experience percentage (- / +, the
-- stock buttons: 10 at a time).
local API = hs.UI
local W = dofile("lib/window.lua")
local T = W.T -- the window module's theme: the scale W.begin computed this frame
local pad = W.pad
local AS = dofile("lib/assign.lua")
local TB = dofile("lib/textbox.lua")
local C = T.C

local AW = "AAWindow"
local TABS = { "General", "Archetype", "Class", "Special" }
local lists = { {}, {}, {}, {} }
local desc = {}
local rowCol = { 1, 1, 1, 1 } -- a row's colour, filled in for each row (not a table a row each frame)
local message, messageAt

local function tabs(x0, y0, page)
  local x = x0
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  for i, l in ipairs(TABS) do
    local tw = ui.CalcTextSize(l)
    local w = tw + T.px(40)
    local on = page == i - 1
    T.rect(x, y0, x + w, y0 + th + T.px(16), on and C.accent or C.raised, on and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + th + T.px(16), l, on and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + th + T.px(16)) and ui.IsMouseClicked(ui.Mouse.Left) then
      API.windows.SetTabPage(AW, "Subwindows", i - 1)
    end
    x = x + w + T.px(10)
  end
  T.unfont()
  return y0 + th + T.px(28)
end

local function label(id) return (API.windows.Text(AW, id)) or "" end

ui.window("Alternate Advancement", function()
  if not W.shown(AW) then return end
  local page = API.windows.TabPage(AW, "Subwindows") or 0
  local timer = label("Timer")
  local canUse = timer ~= ""
  local pointsLabel = API.windows.ChildShown(AW, "MonsterLabel") == true and "Monster points" or "AA points"
  if W.begin("Alternate Advancement", { w = 1700, h = 940, title = "Alternate Advancement",
        subtitle = pointsLabel .. ": " .. label("CurrentCount") .. "   Spent: " .. label("TotalCount"),
        hints = { { "a", "Train" }, { "y", canUse and "Use" or "Use (owned, with a timer)" }, { "x", "Assign to slot" },
          { "lb", "Tab" }, { "rb", "Tab" }, { "b", "Close" } } }) then
    local settled = AS.settled("Alternate Advancement")
    local switched
    if W.pressed("lb") then switched = (page + 3) % 4 end
    if W.pressed("rb") then switched = (page + 1) % 4 end
    if switched then
      API.windows.SetTabPage(AW, "Subwindows", switched)
      lists[switched + 1].focusNext = 1 -- the pad's focus onto the new tab's list
      lists[switched + 1].lastFocus = nil
    end
    page = API.windows.TabPage(AW, "Subwindows") or page
    local listId = "List" .. (page + 1)
    local state = lists[page + 1]
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local listX1 = x1 - T.px(560)
    local top = tabs(x0, y0, page)
    -- the stock filter and the AA experience - / + on the tab row, right-aligned over the list
    local bh = top - y0 - T.px(12)
    local cx = listX1 - T.px(14)
    local function control(id, text, w, opts)
      cx = cx - w
      ui.SetCursorPos(cx - wx, y0 - wy)
      cx = cx - T.px(10)
      return T.button(id, text, w, bh, opts or {}, pad)
    end
    T.font(20)
    if control("##more", "+", T.px(60), { disabled = API.windows.ChildState(AW, "MoreExpButton") == false }) then API.windows.ClickChild(AW, "MoreExpButton", "left") end
    local pw = T.px(170)
    cx = cx - pw
    T.textIn(cx, y0, cx + pw, y0 + bh, "AA exp " .. label("ExpCount"), C.exp, 0.5)
    cx = cx - T.px(10)
    if control("##less", "-", T.px(60), { disabled = API.windows.ChildState(AW, "LessExpButton") == false }) then API.windows.ClickChild(AW, "LessExpButton", "left") end
    local _, checked = API.windows.ChildState(AW, "CanPurchaseFilter")
    if control("##filter", (checked and "[x] " or "[  ] ") .. "Can purchase", T.px(230)) then API.windows.ClickChild(AW, "CanPurchaseFilter", "left") end
    T.unfont()
    -- the column heads
    T.font(18)
    T.draw(x0 + T.px(20), top, "ABILITY", C.dim)
    T.textIn(listX1 - T.px(470), top, listX1 - T.px(360), top + T.px(24), "RANK", C.dim, 0.5)
    T.textIn(listX1 - T.px(350), top, listX1 - T.px(270), top + T.px(24), "COST", C.dim, 0.5)
    T.draw(listX1 - T.px(250), top, "CATEGORY", C.dim)
    T.unfont()
    top = top + T.px(34)
    ui.SetCursorPos(x0 - wx, top - wy)
    local rows = API.windows.ListInfo(AW, listId) or 0
    local rowH = 54
    local visible = math.max(1, math.floor((y1 - top + T.px(10)) / T.px(rowH + 10)))
    W.list("aa", state, rows, visible, rowH, listX1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
      local texts, r, g, b = API.windows.ListRow(AW, listId, i - 1)
      if not texts then return end
      local col = rowCol
      col[1], col[2], col[3] = r or 1, g or 1, b or 1
      T.textIn(rx0 + T.px(20), ry0, rx1 - T.px(480), ry1, T.fit(texts[1] or "", rx1 - rx0 - T.px(500)), col, 0)
      T.textIn(rx1 - T.px(470), ry0, rx1 - T.px(360), ry1, texts[2] or "", col, 0.5)
      T.textIn(rx1 - T.px(350), ry0, rx1 - T.px(270), ry1, texts[3] or "", C.endurance, 0.5)
      T.font(18)
      T.textIn(rx1 - T.px(250), ry0, rx1 - T.px(16), ry1, T.fit(texts[4] or "", T.px(230)), C.dim, 0)
      T.unfont()
      if focused then
        if state.lastFocus ~= i then -- the stock selection follows the focus: the description and timer
          state.lastFocus = i
          API.windows.ListClick(AW, listId, i - 1, "left")
        end
        if W.pressed("y") then
          if canUse then API.cursor.PressButtonFrom(AW, "HotButton")
          else message, messageAt = "Only an owned ability with a reuse time can be used.", ui.GetTime() end
        end
        if W.pressed("x") then
          local name = texts[1] or ""
          if not AS.start(name, function() return API.cursor.PickUp(AW, listId, i) end, "Alternate Advancement") then
            message, messageAt = "This ability makes no hot button.", ui.GetTime()
          end
        end
      end
    end, function(i)
      if not settled then return end
      API.windows.ListClick(AW, listId, i - 1, "left")
      local enabled = API.windows.ChildState(AW, "TrainButton")
      if enabled == false then message, messageAt = "You cannot train this ability now.", ui.GetTime()
      else API.windows.ClickChild(AW, "TrainButton", "left") end
    end)
    -- the right column: the selected ability's description and timer, then the points and the settings
    local rx0 = listX1 + T.px(24)
    T.rect(rx0, y0, x1, y1, C.sunken, 1, T.px(14))
    local px = rx0 + T.px(18)
    local py = y0 + T.px(14)
    if canUse then
      T.font(20)
      T.draw(px, py, "Reuse: " .. timer, timer:find(":") and C.endurance or C.good)
      T.unfont()
      py = py + T.px(34)
    end
    local bottomH = T.px(150)
    T.font(20)
    TB.draw(API.windows.Text(AW, "Description") or "", px, py, x1 - T.px(14), y1 - bottomH, desc)
    T.unfont()
    local by = y1 - bottomH + T.px(14)
    ui.DrawLine(px, by - T.px(8), x1 - T.px(18), by - T.px(8), C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
    T.font(20)
    local _, lh = ui.CalcTextSize("Ag")
    T.draw(px, by, pointsLabel .. ": " .. label("CurrentCount"), C.text)
    T.draw(px, by + lh + T.px(8), "Spent: " .. label("TotalCount") .. "    Assigned: " .. label("AssignedCount"), C.dim)
    T.draw(px, by + 2 * (lh + T.px(8)), "Experience to AA: " .. label("ExpCount"), C.exp)
    T.unfont()
    if message and ui.GetTime() - messageAt < 3 then
      T.font(20)
      T.textIn(x0, y1 - T.px(30), listX1, y1, message, C.endurance, 0.5)
      T.unfont()
    end
  end
  if W.finish() then API.windows.Show(AW, false) end
end)
