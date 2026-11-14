-- Leadership (gamepad pack): stands in for LeadershipWindow (group and raid leadership abilities; the EQ menu's
-- Leadership) and GroupMentorWindow (its Group Mentoring button).
--   Leadership   group and raid leadership points and the experience toward the next one at the top, the switch
--                for earning leadership experience (the window's On / Off), the abilities of one kind (LB / RB: the
--                group's or the raid's list, LEList1 / LEList2) with their rank and the next rank's cost, and the
--                selected one's description with Purchase (X too), hotkey creation, Group Mentoring and Done.
--   Mentoring    the protege (the window's drop-down), the share of experience to give (D-pad left / right on it,
--                LB / RB ten; the window's slider) and Accept.
-- Rows, buttons, the drop-down and the slider are the stock windows' children, so the game decides what each does.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local AS = dofile("lib/assign.lua")
local TB = dofile("lib/textbox.lua")
local C = T.C

-- ---------------------------------------------------------------- leadership
local LW = "LeadershipWindow"
local tab = 1 -- 1 group, 2 raid (both lists are always filled: the tabs are this window's own)
local lists = { {}, {} }
local desc = {}
local side = {}
local wasShown = false

local function ranks(texts)
  local cur, max = (texts[2] or ""):match("(%d+)%s*/%s*(%d+)")
  return tonumber(cur) or 0, tonumber(max) or 0
end

-- A kind's card: its name, the points to spend and the gauge toward the next point.
local function card(x0, y0, x1, y1, title, points, gauge, colour)
  T.panel(x0, y0, x1, y1, 1, T.px(14), C.raised)
  local p = T.px(16)
  T.font(18)
  T.draw(x0 + p, y0 + p, title:upper(), C.dim)
  T.unfont()
  T.font(34)
  local pts = CH.text(LW, points)
  T.textIn(x0 + p, y0 + p - T.px(4), x1 - p, y0 + p + T.px(40), (pts ~= "" and pts or "0") .. " pt", C.text, 1)
  T.unfont()
  local frac, text = eq.ChildGauge(LW, gauge)
  T.font(17)
  T.bar(x0 + p, y1 - p - T.px(26), x1 - p, y1 - p, frac or 0, colour, "Toward the next point", text or "")
  T.unfont()
end

ui.window("Leadership", function()
  CH.testOpen(LW)
  if not W.shown(LW) then
    wasShown = false
    return
  end
  T.frame()
  local list = tab == 1 and "LEList1" or "LEList2"
  if not wasShown then lists[tab].focusNext = 1 end
  wasShown = true
  if W.begin("Leadership", { w = 1600, h = 960, title = "Leadership",
        subtitle = tab == 1 and "Group abilities" or "Raid abilities",
        hints = { { "lb", "Group" }, { "rb", "Raid" }, { "a", "Select" }, { "x", "Purchase" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if W.pressed("lb") and tab ~= 1 then tab = 1 lists[1].focusNext = 1 end
    if W.pressed("rb") and tab ~= 2 then tab = 2 lists[2].focusNext = 1 end
    if W.pressed("x") and CH.enabled(LW, "LEW_PurchaseButton") then eq.ClickChild(LW, "LEW_PurchaseButton", "left") end
    -- the two kinds
    local ch = T.px(130)
    local half = (cw - T.px(16)) / 2
    card(x0, y0, x0 + half, y0 + ch, "Group leadership", "LEW_CurrentGroupCount", "LEW_GroupExpGauge", C.accent)
    card(x0 + half + T.px(16), y0, x1, y0 + ch, "Raid leadership", "LEW_CurrentRaidCount", "LEW_RaidExpGauge", C.exp)
    -- the abilities
    local ly = y0 + ch + T.px(20)
    local listW = cw * 0.6
    ui.SetCursorPos(x0 - wx, ly - wy)
    local focusedRow = CH.list(LW, list, lists[tab], listW, y1 - ly, { rowH = 62, empty = "No abilities.",
      draw = function(_, texts, rx0, ry0, rx1, ry1, focused)
        local cur, max = ranks(texts)
        local p = T.px(18)
        T.font(22)
        T.textIn(rx0 + p, ry0, rx1 - T.px(260), ry1, T.fit(texts[1] or "", rx1 - rx0 - T.px(290)), cur > 0 and C.text or C.dim, 0)
        T.unfont()
        -- the ranks as pips, the next rank's cost on the right
        local midY = (ry0 + ry1) / 2
        local px = rx1 - T.px(250)
        for i = 1, max do
          local bx = px + (i - 1) * T.px(22)
          T.rect(bx, midY - T.px(4), bx + T.px(16), midY + T.px(4), i <= cur and C.accent or C.sunken, 1, T.px(3))
        end
        local cost = texts[3] or ""
        T.font(20)
        T.textIn(rx0, ry0, rx1 - p, ry1, (cost == "" or cost == "-") and ((max > 0 and cur >= max) and "MAX" or "-") or (cost .. " pt"), C.gold or C.accent, 1)
        T.unfont()
      end })
    W.toSide(side, focusedRow)
    -- the selected one, and the buttons
    local rx0 = x0 + listW + T.px(24)
    local rw = x1 - rx0
    local bh = T.px(76)
    local buttons = {
      { "LEW_PurchaseButton", "Purchase", "The next rank of the selected ability", true },
      { "LEW_HotkeyButton1", "Create Hotkey 1", "Place the selected ability on a hot bar" },
      { "LEW_HotkeyButton2", "Create Hotkey 2", "Place the selected ability on a hot bar" },
      { "LEW_HotkeyButton3", "Create Hotkey 3", "Place the selected ability on a hot bar" },
      { "LEW_GroupMentoring", "Group Mentoring", "Give experience to a group member" },
      { "LEW_OnButton", "Earn leadership experience", "Part of your experience goes to leadership" },
      { "LEW_OffButton", "Stop earning it", "Leadership earns no experience" },
      { "LEW_DoneButton", "Done", nil },
    }
    local by = ly + math.min(T.px(170), (y1 - ly) * 0.28)
    TB.draw(eq.WindowText(LW, "LEW_Description") or "", rx0, ly, x1, by - T.px(16), desc)
    ui.SetCursorPos(rx0 - wx, by - wy)
    -- Navigation scrolls the selected action into view, even on a smaller display.
    ui.BeginChild("##leadershipactions", rw, y1 - by, false)
    local ax, ay = ui.GetCursorScreenPos()
    local actionW = ui.GetContentRegionAvail()
    by = ay
    for k, b in ipairs(buttons) do
      ui.SetCursorScreenPos(ax, by)
      W.sideBefore(side, k)
      local on = CH.checked(LW, b[1])
      local tip = eq.ChildTooltip(LW, b[1]) or ""
      local opts = { sub = on and "(now)" or (tip ~= "" and tip or b[3]), align = 0, primary = b[4] or on, keep = true }
      if b[1]:find("LEW_HotkeyButton", 1, true) == 1 then
        opts.disabled = not CH.enabled(LW, b[1])
        if T.button("##" .. LW .. b[1], b[2], actionW, bh, opts, pad) then
          AS.start(tip ~= "" and tip or b[2], function() eq.ClickChild(LW, b[1], "left") return true end, "Leadership")
        end
      else
        CH.button(LW, b[1], b[2], actionW, bh, opts)
      end
      W.sideAfter(side, k, #buttons, lists[tab])
      by = by + bh + T.px(10)
    end
    ui.EndChild()
  end
  if W.finish() then eq.ShowWindow(LW, false) end
end)

-- ---------------------------------------------------------------- group mentoring
local GM = "GroupMentorWindow"
local gst = CH.new()

ui.window("Group Mentoring", function()
  CH.testOpen(GM)
  if not W.shown(GM) then
    gst.pick, gst.kb.open = nil, false
    return
  end
  T.frame()
  local _, _, value, count = eq.ChildState(GM, "GM_Slider")
  value, count = value or 0, count or 1
  local maxv = math.max(0, count - 1)
  local function set(v) eq.SetSlider(GM, "GM_Slider", math.max(0, math.min(maxv, v))) end
  if W.begin("Group Mentoring", { w = 900, h = 560, title = "Group Mentoring", subtitle = "Share your experience with a protege",
        hints = CH.hints(gst, { { "a", "Select" }, { "left", "Less" }, { "right", "More" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(gst, x0, y0, x1, y1) then
      ui.SetCursorPos(x0 - wx, y0 - wy)
      CH.combo(gst, GM, "GM_SelectPlayer", "Protege", cw, T.px(84), { keep = true })
      -- the share: the slider's value as text, a bar, and - / + (D-pad left / right while it is focused)
      local sy = y0 + T.px(104)
      local amount = CH.text(GM, "GM_SliderInput")
      if amount == "" then amount = tostring(value) end
      ui.SetCursorPos(x0 - wx, sy - wy)
      local _, focused, bx0, by0, bx1, by1 = T.button("##gmshare", "", cw, T.px(120), {}, pad)
      T.font(18)
      T.draw(bx0 + T.px(16), by0 + T.px(12), "GIVE % EXPERIENCE", focused and C.accent or C.dim)
      T.unfont()
      T.font(40)
      T.textIn(bx0, by0 + T.px(8), bx1 - T.px(16), by0 + T.px(60), amount .. "%", C.text, 1)
      T.unfont()
      T.bar(bx0 + T.px(16), by1 - T.px(36), bx1 - T.px(16), by1 - T.px(20), maxv > 0 and value / maxv or 0, C.accent)
      if focused then
        if pad.menuPressed("left") then set(value - 1) end
        if pad.menuPressed("right") then set(value + 1) end
        if pad.menuPressed("lb") then set(value - 10) end
        if pad.menuPressed("rb") then set(value + 10) end
      end
      local bh = T.px(72)
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      if T.button("##gmminus", "-", T.px(100), bh, {}, pad) then set(value - 1) end
      ui.SameLine(0, T.px(10))
      if T.button("##gmplus", "+", T.px(100), bh, {}, pad) then set(value + 1) end
      ui.SameLine(0, T.px(10))
      CH.button(GM, "GM_AcceptButton", "Accept", cw - T.px(220), bh, { primary = true, keep = true })
    end
  end
  if CH.back(gst, W.finish()) then eq.ShowWindow(GM, false) end
end)
