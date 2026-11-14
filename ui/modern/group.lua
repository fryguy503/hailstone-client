-- Group frames: replace the game's GroupWindow. A solid panel with a title strip (drag it, or any empty spot, to
-- move the frame), a card per member (name, health, mana and endurance, their pet), a click targets the member, and
-- the invite, disband, accept and decline buttons under the cards (the game's own buttons: they are enabled when
-- the game enables them).
local hud = dofile("lib/hud.lua")
local roles = dofile("lib/role_icons.lua")
local T = hud.T
local C = T.color
local WINDOW = "GroupWindow"
local ROLE_COLORS = { C.mana, C.accent, C.endurance }

-- Each slot's ids and its member record (filled in again every frame), made once.
local SLOTS = {}
for i = 1, 5 do
  SLOTS[i] = {
    gauge = "Gauge" .. i, hit = "##Gauge" .. i, hp = "group.hp" .. i, mana = "group.mana" .. i,
    endurance = "group.end" .. i, pet = "group.pet" .. i,
    m = { i = i }, tint = { 0, 0, 0, 1 }, color = { 0, 0, 0, 1 },
  }
end

local function member(i)
  local frac, name, raw, tr, tg, tb = eq.Gauge(10 + i)
  if raw == -1 or not name or name == "" then return nil end
  local slot = SLOTS[i]
  local m = slot.m
  m.hp, m.name, m.dead, m.away = frac, name:match("^[^\n]*"), raw == -3, raw == -2
  m.tint, m.mana, m.endurance, m.pet, m.petName, m.color = nil, nil, nil, nil, nil, nil
  -- a special value's tint (-2: not in the zone, full in the game's dark red)
  if tr then
    local t = slot.tint
    t[1], t[2], t[3] = tr, tg, tb
    m.tint = t
  end
  local mf, _, mraw = eq.Gauge(30 + i)
  if mraw ~= -1 then m.mana = mf end
  local ef, _, eraw = eq.Gauge(35 + i)
  if eraw ~= -1 then m.endurance = ef end
  local pf, pname, praw = eq.Gauge(16 + i)
  if praw ~= -1 and pname and pname ~= "" then m.pet, m.petName = pf, pname end
  local r, g, b = eq.ChildColor(WINDOW, slot.gauge) -- a leader's name in green
  if r then
    local c = slot.color
    c[1], c[2], c[3] = r, g, b
    m.color = c
  end
  m.roles = roles.Mask(WINDOW, i)
  return m
end

local CARD_W = 210
local HEAD, PAD = 24, 6
local PERCENT = {} -- "n%" by n
local COUNT = {} -- "n / 6" by n

local function cardH(m) return 47 + (m.pet and 9 or 0) end

-- What the options and body read, set each frame before hud.Window (they are made once, not every frame).
local members = {}
local frame = { invited = false, canInvite = false, grouped = false, total = 0 }
local INVITED_BUTTONS = { { "FollowButton", "Accept", true, "##FollowButton" }, { "DeclineButton", "Decline", nil, "##DeclineButton" } }
local GROUP_BUTTONS = { { "InviteButton", "Invite", nil, "##InviteButton" }, { "DisbandButton", "Disband", nil, "##DisbandButton" } }
local BUTTON_OPTS = {}

local OPTS = {
  size = function() return CARD_W, frame.total end,
  panelOpts = { opaque = true },
  -- solo: shown only with an invitation to answer or a player to invite
  hideWhen = function() return #members == 0 and not frame.invited and not frame.canInvite and not frame.grouped end,
}

local function body(x0, y, w0, h, s)
  -- the title strip: empty, so a drag there moves the frame
  local _, th = T.TextSize("Ag", T.font.small)
  T.Text(x0 + 10 * s, y + (HEAD * s - th) / 2, "Group", C.muted, T.font.small)
  if #members > 0 then
    local n = #members + 1
    local count = COUNT[n]
    if not count then
      count = n .. " / 6"
      COUNT[n] = count
    end
    T.Text(x0 + w0 - 10 * s, y + (HEAD * s - th) / 2, count, C.faint, T.font.small, "right")
  end
  ui.DrawLine(x0 + 8 * s, y + HEAD * s, x0 + w0 - 8 * s, y + HEAD * s, C.ink[1], C.ink[2], C.ink[3], 0.07, 1)
  local x, w = x0 + PAD * s, w0 - 2 * PAD * s
  local cy = y + HEAD * s + 4 * s
  if #members == 0 then
    T.Text(x0 + w0 / 2, cy + 3 * s, "Not in a group", C.faint, T.font.small, "center")
    cy = cy + 22 * s
  end
  for _, m in ipairs(members) do
    local slot = SLOTS[m.i]
    local ch = cardH(m) * s
    local clicked = T.Hit(slot.hit, x, cy, x + w, cy + ch)
    if hud.DragItem() then clicked = false end -- a drag moves the frame
    local hovered = ui.IsItemHovered()
    if hovered and not ui.GetIO().alt and ui.IsMouseClicked(ui.Mouse.Right) then
      T.TakeRightClick()
      eq.ClickChild(WINDOW, slot.gauge, "right")
    end
    ui.DrawRectFilled(x, cy, x + w, cy + ch, C.ink[1], C.ink[2], C.ink[3], hovered and 0.07 or 0.035, 6 * s)
    if hovered then ui.DrawRect(x, cy, x + w, cy + ch, C.accent[1], C.accent[2], C.accent[3], 0.6, 6 * s, 1.5 * s) end
    local pad = 8 * s
    local _, nh = T.TextSize("Ag", T.font.small)
    local iconSize, iconGap = 14 * s, 3 * s
    local iconW = roles.Width(m.roles, iconSize, iconGap)
    local name = T.Fit(m.name, w - 60 * s - (iconW > 0 and iconW + 4 * s or 0), T.font.small)
    local nameW = T.TextSize(name, T.font.small)
    T.Text(x + pad, cy + 5 * s, name, m.color or C.text, T.font.small)
    local roleHovered = roles.Draw(m.roles, x + pad + nameW + 4 * s, cy + 5 * s + (nh - iconSize) / 2,
      iconSize, iconGap, ROLE_COLORS)
    local pct
    if m.dead then pct = "Dead" elseif m.away then pct = "Away" else
      local n = math.floor((m.hp or 0) * 100 + 0.5)
      pct = PERCENT[n]
      if not pct then
        pct = n .. "%"
        PERCENT[n] = pct
      end
    end
    T.Text(x + w - pad, cy + 5 * s, pct, m.dead and C.danger or C.muted, T.font.small, "right")
    local by = cy + 5 * s + nh + 3 * s
    T.Bar(slot.hp, x + pad, by, x + w - pad, by + 10 * s, s, m.dead and 0 or m.hp,
      m.tint or ((m.hp or 1) < 0.25 and C.hpLow or C.hp))
    by = by + 12 * s
    local half = (w - 2 * pad - 4 * s) / 2
    if m.mana then T.Bar(slot.mana, x + pad, by, x + pad + half, by + 4 * s, s, m.mana, C.mana) end
    if m.endurance then
      T.Bar(slot.endurance, x + pad + half + 4 * s, by, x + w - pad, by + 4 * s, s, m.endurance, C.endurance)
    end
    if m.pet then
      by = by + 7 * s
      T.Bar(slot.pet, x + pad + 10 * s, by, x + w - pad, by + 5 * s, s, m.pet, C.pet)
    end
    if hovered and not roleHovered then
      ui.SetTooltip(m.name .. (m.roles ~= 0 and ("\n" .. roles.Labels[m.roles]) or "")
        .. (m.pet and ("\nPet: " .. m.petName) or "") .. "\nRight-click for group roles")
    end
    if clicked then eq.ClickChild(WINDOW, slot.gauge, "left") end
    cy = cy + ch + 4 * s
  end
  -- the buttons
  local by = cy + 2 * s
  local bh = 22 * s
  local buttons = frame.invited and INVITED_BUTTONS or GROUP_BUTTONS
  local bw = (w - (#buttons - 1) * 6 * s) / #buttons
  for k, b in ipairs(buttons) do
    local bx = x + (k - 1) * (bw + 6 * s)
    local enabled = eq.ChildState(WINDOW, b[1])
    local tip = eq.ChildTooltip(WINDOW, b[1])
    BUTTON_OPTS.enabled, BUTTON_OPTS.primary, BUTTON_OPTS.tooltip = enabled ~= false, b[3], tip ~= "" and tip or nil
    if T.Button(b[4], bx, by, bx + bw, by + bh, s, b[2], BUTTON_OPTS) then
      eq.ClickChild(WINDOW, b[1], "left")
    end
  end
end

ui.window("Group", function()
  if not eq.InGame() then return end
  for k = #members, 1, -1 do members[k] = nil end
  for i = 1, 5 do
    local m = member(i)
    if m then members[#members + 1] = m end
  end
  frame.invited = eq.ChildShown(WINDOW, "FollowButton") == true
  frame.canInvite = eq.ChildState(WINDOW, "InviteButton") == true
  frame.grouped = eq.ChildState(WINDOW, "DisbandButton") == true
  local total = HEAD + PAD
  for _, m in ipairs(members) do total = total + cardH(m) + 4 end
  if #members == 0 then total = total + 22 end
  frame.total = total + 28
  hud.Window(WINDOW, "Group", OPTS, body)
end)
