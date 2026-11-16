-- More of the HUD (gamepad pack): pieces that never take the pad's focus and that B leaves up,
-- each standing in for its stock window so its key and the EQ menu still show and hide it:
--   Breath (BreathWindow)       the air left (EQType 8) as a bar over the casting bar while you are short of air,
--                               turning from cyan to red as it runs out and pulsing in its last quarter
--   Aggro (AggroMeterWnd)       under the target: your share of the target's hate (gauge 149, label 305) and the
--                               most hated besides you (labels 303 / 307), while the meter is on your target
--   Potion Belt (PotionBeltWnd) five tiles at the bottom right with how many of each item you carry; the game's
--                               slots: a click with an item on the cursor puts it on the belt, a right click uses it,
--                               the right button held shows it (the mouse, or the pad in cursor mode)
--   Cursor (CursorAttachment)   what the game carries on the cursor (an item, money, a spell, a hotbutton) drawn
--                               above every window: at the pointer with the mouse; in menu mode, where the pad has
--                               no pointer, as a card at the bottom right saying what is held
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua") -- (testing: CH.testOpen)
local C = T.C

-- A HUD window at x, y (design px; negative: from the right / bottom), w x h; inputs: the mouse may click in it.
local function hud(name, x, y, w, h, inputs, draw)
  T.frame()
  T.resetFonts()
  local px, py, pw, ph = T.px(x), T.px(y), T.px(w), T.px(h)
  if px < 0 then px = T.W + px - pw end
  if py < 0 then py = T.H + py - ph end
  ui.SetNextWindowPos(px, py, ui.Cond.Always)
  ui.SetNextWindowSize(pw, ph, ui.Cond.Always)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 0, 0)
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 0)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoBackground | ui.WindowFlags.NoSavedSettings
      | ui.WindowFlags.NoMove | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoBringToFrontOnFocus | W.NoNav
  if not inputs then flags = flags | ui.WindowFlags.NoInputs end
  if ui.Begin(name, nil, flags) then
    T.font(T.BASE)
    draw(px, py, px + pw, py + ph)
    T.unfont()
  end
  ui.End()
  ui.PopStyleVar(2)
end

-- ---------------------------------------------------------------- breath
local AIR = { 0.30, 0.85, 0.95, 1 }
local air = 0 -- the air left, for drawBreath (one function, not a new one each frame)
local function drawBreath(x0, y0, x1, y1)
  T.panel(x0, y0, x1, y1, 0.9, T.px(14))
  local col = AIR
  if air < 0.5 then -- toward red as it runs out
    local k = math.max(0, air / 0.5)
    col = { C.bad[1] + (AIR[1] - C.bad[1]) * k, C.bad[2] + (AIR[2] - C.bad[2]) * k, C.bad[3] + (AIR[3] - C.bad[3]) * k, 1 }
  end
  local alpha = 1
  if air < 0.25 then alpha = 0.65 + 0.35 * math.abs(math.sin(ui.GetTime() * 5)) end
  local p = T.px(12)
  T.bar(x0 + p, y0 + p, x1 - p, y1 - p, air, col, "Air", string.format("%d%%", math.floor(air * 100 + 0.5)), alpha)
end
ui.window("HUD Breath", function()
  CH.testOpen("BreathWindow")
  if not W.shown("BreathWindow") then return end
  air = API.character.Gauge(8) or 0
  hud("HUD Breath", 760, -410, 400, 62, false, drawBreath)
end)

-- ---------------------------------------------------------------- aggro
local SECOND = { 0.90, 0.55, 0.12, 1 }
local aggro = { mine = 0, other = "", otherPct = 0 } -- this frame's, for drawAggro (one function, not one a frame)
local function drawAggro(x0, y0, x1, y1)
  local mine, other, otherPct = aggro.mine, aggro.other, aggro.otherPct
  T.panel(x0, y0, x1, y1, 0.85, T.px(12))
  local p = T.px(12)
  T.font(17)
  local col = mine > 99 and C.bad or (mine > 75 and C.endurance or C.good) -- the game's 75 / 100 thresholds
  T.bar(x0 + p, y0 + p, x1 - p, y0 + p + T.px(26), math.min(1, mine / 100), col, "Your hate", mine .. "%")
  if other ~= "" then
    T.bar(x0 + p, y0 + p + T.px(32), x1 - p, y0 + p + T.px(58), math.min(1, otherPct / 100), SECOND, T.fit(other, x1 - x0 - T.px(140)), otherPct .. "%")
  end
  T.unfont()
end
ui.window("HUD Aggro", function()
  CH.testOpen("AggroMeterWnd")
  if not W.shown("AggroMeterWnd") then return end
  local frac, _, raw = API.character.Gauge(149)
  local _, target, traw = API.character.Gauge(6)
  if raw == nil or raw == -1 or traw == -1 or not target or target == "" then return end
  if (API.character.Label(302) or "") ~= target then return end -- the meter is on another spawn (a locked one)
  aggro.mine = tonumber((API.character.Label(305))) or math.floor((frac or 0) * 100 + 0.5)
  aggro.other = API.character.Label(303) or ""
  aggro.otherPct = tonumber((API.character.Label(307))) or 0
  hud("HUD Aggro", 660, 232, 600, aggro.other ~= "" and 92 or 58, false, drawAggro)
end)

-- ---------------------------------------------------------------- potion belt
local PB = "PotionBeltWnd"
local rightPress = nil
local SLOT_IDS, LABEL_IDS, NUMBERS = {}, {}, {} -- the belt's children and numbers, made once (it is up all the time)
for i = 0, 4 do
  SLOT_IDS[i] = "PotionSlot" .. i
  LABEL_IDS[i] = "PotionSlotLabel" .. i
  NUMBERS[i] = tostring(i + 1)
end
local size, gap = 64, 8
-- (one function, not a new one each frame)
local function drawBelt(x0, y0, x1, y1)
  T.panel(x0, y0, x1, y1, 0.85, T.px(14))
  local sz = T.px(size)
  local wx, wy = ui.GetWindowPos()
  for i = 0, 4 do
    local id = SLOT_IDS[i]
    local bx, by = x0 + T.px(12) + i * (sz + T.px(gap)), y0 + T.px(12)
    ui.SetCursorPos(bx - wx, by - wy)
    ui.PushID(i)
    local clicked = ui.InvisibleButton("##pb", sz, sz)
    local hovered = ui.IsItemHovered()
    ui.PopID()
    local name = API.windows.ChildTooltip(PB, id) or ""
    T.rect(bx, by, bx + sz, by + sz, name ~= "" and C.raised or C.sunken, 1, T.px(10))
    if name ~= "" then
      local tex, u0, v0, u1, v1 = API.windows.ChildArt(PB, id, "decal")
      if type(tex) == "userdata" then
        local m = math.floor(sz * 0.1)
        T.icon(tex, u0, v0, u1, v1, bx + m, by + m, bx + sz - m, by + sz - m)
      end
      -- the slot's recast (the belt button's cool-down): the part still to go, from below
      local top = API.spells.ButtonCoolDown(PB, id, by, by + sz)
      if top then ui.DrawRectFilled(bx, top, bx + sz, by + sz, 0, 0, 0, 0.55, T.px(10)) end
      local count = API.windows.Text(PB, LABEL_IDS[i]) or ""
      if count ~= "" then
        T.font(16)
        T.textIn(bx, by + sz - T.px(22), bx + sz - T.px(5), by + sz, count, count == "0" and C.bad or C.text, 1)
        T.unfont()
      end
    end
    T.font(13)
    T.draw(bx + T.px(5), by + T.px(3), NUMBERS[i], C.faint)
    T.unfont()
    T.outline(bx, by, bx + sz, by + sz, hovered and C.accent or C.edge, 1, T.px(10), hovered and T.px(2) or 1)
    if hovered then
      ui.SetTooltip(name ~= "" and (name .. "\nRight-click to use, hold the right button to inspect")
        or "An empty belt slot: click it with a potion on the cursor to put it here")
    end
    if clicked then API.windows.ClickChild(PB, id, "left") end
    -- the right button as the game takes it on a slot (down, held after 500 ms, up)
    if hovered and ui.IsMouseClicked(ui.Mouse.Right) then
      rightPress = { i = i, at = ui.GetTime() }
      API.windows.ClickChild(PB, id, "rightdown")
    end
    if rightPress and rightPress.i == i then
      if not ui.IsMouseDown(ui.Mouse.Right) then
        API.windows.ClickChild(PB, id, rightPress.held and "rightupheld" or "rightup", not hovered)
        rightPress = nil
      elseif not rightPress.held and ui.GetTime() - rightPress.at > 0.5 then
        rightPress.held = true
        API.windows.ClickChild(PB, id, "rightheld")
      end
    end
  end
end

ui.window("HUD Potion Belt", function()
  CH.testOpen(PB)
  if not W.shown(PB) then return end
  hud("HUD Potion Belt", -24, -24, 5 * size + 4 * gap + 24, size + 24, true, drawBelt)
end)

-- ---------------------------------------------------------------- cursor
-- (with the mouse: where the game puts the attachment, at the pointer)
local function image(t, r, a)
  if t and r and type(t[1]) == "userdata" then ui.DrawImage(t[1], r[1], r[2], r[1] + r[3], r[2] + r[4], t[2], t[3], t[4], t[5], 1, 1, 1, a or 1) end
end
-- money on the cursor (a.coin 0 platinum .. 3 copper): a coin in its colour with its letter, centred in x0, y0, sz
local COINS = { [0] = { "p", "Platinum", C.platinum }, { "g", "Gold", C.gold }, { "s", "Silver", C.silver }, { "c", "Copper", C.copper } }
local function coin(c, x0, y0, sz)
  local cx, cy, r = x0 + sz / 2, y0 + sz / 2, sz * 0.32
  local col = c[3]
  ui.DrawCircleFilled(cx, cy, r, col[1], col[2], col[3], 1)
  ui.DrawCircle(cx, cy, r, 0, 0, 0, 0.45, 1.5)
  local tw, th = ui.CalcTextSize(c[1])
  ui.DrawText(cx - tw / 2, cy - th / 2, 0.08, 0.08, 0.1, 0.85, c[1])
end

ui.window("Cursor Item", function()
  if not API.session.InGame() then return end
  local a = API.cursor.Attachment()
  if not a then return end
  T.frame()
  if pad.connected() and pad.mode() == "menu" then
    -- the pad has no pointer: what is held as a card at the bottom right (above the potion belt), over the windows
    hud("HUD Cursor Item", -24, -130, 520, 96, false, function(x0, y0, x1, y1)
      ui.Foreground(true)
      local w, h = x1 - x0, y1 - y0
      T.panel(x0, y0, x1, y1, 0.95, T.px(16), C.popup)
      local s = h - T.px(24)
      T.rect(x0 + T.px(12), y0 + T.px(12), x0 + T.px(12) + s, y0 + T.px(12) + s, C.raised, 1, T.px(10))
      local icon = a.icon or a.gem or a.bg
      local held = a.coin and COINS[a.coin]
      if icon and type(icon[1]) == "userdata" then
        T.icon(icon[1], icon[2], icon[3], icon[4], icon[5], x0 + T.px(16), y0 + T.px(16), x0 + T.px(8) + s, y0 + T.px(8) + s)
      elseif held then
        coin(held, x0 + T.px(12), y0 + T.px(12), s)
      end
      T.font(17)
      T.draw(x0 + s + T.px(30), y0 + T.px(14), "HOLDING", C.dim)
      T.unfont()
      T.font(23)
      local item = not a.button and API.inventory.Item(API.inventory.CursorSlot or 33) or nil
      local what = (item and item.name) or (held and held[2]) or ((a.text and a.text ~= "") and a.text) or (a.spell)
        or (a.button and "A hot button" or "A spell")
      if a.quantity then what = what .. "  x" .. a.quantity end
      T.draw(x0 + s + T.px(30), y0 + T.px(42), T.fit(what, w - s - T.px(50)), C.text)
      T.unfont()
      ui.Foreground(false)
    end)
    return
  end
  -- (with the mouse: where the game puts the attachment, at the pointer, above everything; no window of its own)
  ui.Foreground(true)
  local face = a.bgRect or a.iconRect or a.rect
  if face then
    local x0, y0, sz = face[1], face[2], face[3]
    ui.DrawRectFilled(x0 + 2, y0 + 3, x0 + sz + 2, y0 + sz + 4, 0, 0, 0, 0.35, 8) -- a soft shadow: it is lifted
    if a.button and not a.bg then
      T.rect(x0, y0, x0 + sz, y0 + sz, C.raised, 0.95, 8)
      T.outline(x0, y0, x0 + sz, y0 + sz, C.accent, 1, 8, 2)
    end
    image(a.bg, a.bgRect)
    image(a.icon, a.iconRect)
    image(a.gem, a.gemRect)
    if a.coin and COINS[a.coin] then coin(COINS[a.coin], x0, y0, sz) end
    ui.PushFont(math.max(1, math.min(6, a.font or 2)))
    if a.text and a.text ~= "" and a.bgRect and not a.icon then
      local tw, th = ui.CalcTextSize(a.text)
      ui.DrawText(x0 + (sz - tw) / 2, y0 + (sz - th) / 2, C.text[1], C.text[2], C.text[3], 1, a.text)
    end
    if a.quantity then
      local text = tostring(a.quantity)
      local tw, th = ui.CalcTextSize(text)
      local r = a.rect or face
      local px1, py1 = r[1] + r[3] + 2, r[2] + r[4] + 2
      T.rect(px1 - tw - 10, py1 - th - 2, px1, py1, C.popup, 0.95, (th + 2) / 2)
      ui.DrawText(px1 - tw - 5, py1 - th - 1, C.text[1], C.text[2], C.text[3], 1, text)
    end
    ui.PopFont()
  end
  ui.Foreground(false)
end)
