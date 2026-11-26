-- Group frames: replace the game's GroupWindow. A solid panel with a title strip (drag it, or any empty spot, to
-- move the frame), a card per member (name, health, mana and endurance, their pet), clickable member and pet bars, and
-- the invite, disband, accept and decline buttons under the cards (the game's own buttons: they are enabled when
-- the game enables them).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local roles = dofile("lib/role_icons.lua")
local vitals = dofile("lib/vitals.lua")
local T = hud.T
local C = T.color
local WINDOW = "GroupWindow"
local ROLE_COLORS = { C.mana, C.accent, C.endurance }
local HP_DISPLAY = "group.hpDisplay"
local MANA_DISPLAY = "group.manaDisplay"
local ENDURANCE_DISPLAY = "group.enduranceDisplay"
local PET_DISPLAY = "group.petHpDisplay"
local READOUT_H, READOUT_STEP = 16, 19

-- Each slot's ids and its member record (filled in again every frame), made once.
local SLOTS = {}
for i = 1, 5 do
  SLOTS[i] = {
    gauge = "Gauge" .. i, hit = "##Gauge" .. i, hp = "group.hp" .. i, mana = "group.mana" .. i,
    endurance = "group.end" .. i, pet = "group.pet" .. i, petGauge = "PetGauge" .. i,
    m = { i = i, hpText = { font = T.font.tiny }, manaText = { font = T.font.tiny },
      enduranceText = { font = T.font.tiny }, petText = { font = T.font.tiny } },
    tint = { 0, 0, 0, 1 }, color = { 0, 0, 0, 1 },
  }
end

local function readout(opts, mode, gauge, fraction)
  opts.text, opts.right = nil, nil
  if fraction == nil then return false end
  opts.text, opts.right = vitals.Read(mode, gauge, fraction)
  return opts.text ~= nil or opts.right ~= nil
end

local function member(i)
  local frac, name, raw, tr, tg, tb = API.character.Gauge(10 + i)
  if raw == -1 or not name or name == "" then return nil end
  local slot = SLOTS[i]
  local m = slot.m
  m.hp, m.name, m.dead, m.away, m.hpKnown = frac, name:match("^[^\n]*"), raw == -3, raw == -2, raw >= 0
  m.tint, m.mana, m.endurance, m.pet, m.petName, m.color = nil, nil, nil, nil, nil, nil
  -- a special value's tint (-2: not in the zone, full in the game's dark red)
  if tr then
    local t = slot.tint
    t[1], t[2], t[3] = tr, tg, tb
    m.tint = t
  end
  local mf, _, mraw = API.character.Gauge(30 + i)
  if mraw ~= -1 then m.mana = mf end
  m.manaKnown = mraw >= 0
  local ef, _, eraw = API.character.Gauge(35 + i)
  if eraw ~= -1 then m.endurance = ef end
  m.enduranceKnown = eraw >= 0
  local pf, pname, praw = API.character.Gauge(16 + i)
  if praw ~= -1 and pname and pname ~= "" then m.pet, m.petName = pf, pname end
  local r, g, b = API.windows.ChildColor(WINDOW, slot.gauge) -- a leader's name in green
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
local COUNT = {} -- "n / 6" by n

local function cardH(m)
  return 47 + (m.hpText.text and READOUT_H - 10 or 0) + (m.manaReadout and READOUT_STEP or 0)
    + (m.enduranceReadout and READOUT_STEP or 0) + (m.pet and (m.petReadout and READOUT_H + 4 or 9) or 0)
end

-- What the options and body read, set each frame before hud.Window (they are made once, not every frame).
local members = {}
local frame = { invited = false, canInvite = false, grouped = false }
local INVITED_BUTTONS = { { "FollowButton", "Accept", true, "##FollowButton" }, { "DeclineButton", "Decline", nil, "##DeclineButton" } }
local GROUP_BUTTONS = { { "InviteButton", "Invite", nil, "##InviteButton" }, { "DisbandButton", "Disband", nil, "##DisbandButton" } }
local BUTTON_OPTS = {}

-- Exact values are requested only while hud.Window draws the group frame.
local function size()
  local hp = vitals.Mode(HP_DISPLAY, "percent")
  local mana = vitals.Mode(MANA_DISPLAY, "none")
  local endurance = vitals.Mode(ENDURANCE_DISPLAY, "none")
  local pet = vitals.Mode(PET_DISPLAY, "none")
  local total = HEAD + PAD
  for _, m in ipairs(members) do
    readout(m.hpText, hp, 10 + m.i, m.hpKnown and m.hp or nil)
    m.manaReadout = readout(m.manaText, mana, 30 + m.i, m.manaKnown and m.mana or nil)
    m.enduranceReadout = readout(m.enduranceText, endurance, 35 + m.i, m.enduranceKnown and m.endurance or nil)
    m.petReadout = readout(m.petText, pet, 16 + m.i, m.pet)
    total = total + cardH(m) + 4
  end
  if #members == 0 then total = total + 22 end
  return CARD_W, total + 28
end

local OPTS = {
  size = size,
  menu = function()
    vitals.Menu("Member HP", HP_DISPLAY, "percent")
    vitals.Menu("Member Mana", MANA_DISPLAY, "none")
    vitals.Menu("Member Endurance", ENDURANCE_DISPLAY, "none")
    vitals.Menu("Pet HP", PET_DISPLAY, "none")
    ui.TextDisabled("Exact values require server support.")
  end,
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
    -- a right click: the member's group role menu; with Alt (or when no menu opened) the frame's own menu
    if hovered and not ui.GetIO().alt and ui.IsMouseClicked(ui.Mouse.Right) then
      if API.windows.ClickChild(WINDOW, slot.gauge, "right") then T.TakeRightClick() end
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
    local status = (m.dead and "Dead") or (m.away and "Away") or (not m.hpText.text and m.hpText.right)
    if status then T.Text(x + w - pad, cy + 5 * s, status, m.dead and C.danger or C.muted, T.font.small, "right") end
    local by = cy + 5 * s + nh + 3 * s
    local hpHeight = (m.hpText.text and READOUT_H or 10) * s
    T.Bar(slot.hp, x + pad, by, x + w - pad, by + hpHeight, s, m.dead and 0 or m.hp,
      m.tint or ((m.hp or 1) < 0.25 and C.hpLow or C.hp), m.hpText.text and m.hpText or nil)
    by = by + hpHeight + 2 * s
    local half = (w - 2 * pad - 4 * s) / 2
    if m.mana and not m.manaReadout then
      T.Bar(slot.mana, x + pad, by, x + pad + half, by + 4 * s, s, m.mana, C.mana)
    end
    if m.endurance and not m.enduranceReadout then
      T.Bar(slot.endurance, x + pad + half + 4 * s, by, x + w - pad, by + 4 * s, s, m.endurance, C.endurance)
    end
    by = by + 7 * s
    if m.manaReadout then
      T.Bar(slot.mana, x + pad, by, x + w - pad, by + READOUT_H * s, s, m.mana, C.mana, m.manaText)
      by = by + READOUT_STEP * s
    end
    if m.enduranceReadout then
      T.Bar(slot.endurance, x + pad, by, x + w - pad, by + READOUT_H * s, s, m.endurance, C.endurance, m.enduranceText)
      by = by + READOUT_STEP * s
    end
    local child = slot.gauge
    if m.pet then
      local px0, px1, py1 = x + pad + 10 * s, x + w - pad, by + (m.petReadout and READOUT_H or 5) * s
      T.Bar(slot.pet, px0, by, px1, py1, s, m.pet, C.pet, m.petText)
      if clicked and ui.IsMouseHoveringRect(px0, by, px1, py1) then child = slot.petGauge end
    end
    if hovered and not roleHovered then
      ui.SetTooltip(m.name .. (m.roles ~= 0 and ("\n" .. roles.Labels[m.roles]) or "")
        .. (m.pet and ("\nPet: " .. m.petName) or "") .. "\nRight-click for group roles\nAlt+right-click for readout options")
    end
    if clicked then API.windows.ClickChild(WINDOW, child, "left") end
    cy = cy + ch + 4 * s
  end
  -- the buttons
  local by = cy + 2 * s
  local bh = 22 * s
  local buttons = frame.invited and INVITED_BUTTONS or GROUP_BUTTONS
  local bw = (w - (#buttons - 1) * 6 * s) / #buttons
  for k, b in ipairs(buttons) do
    local bx = x + (k - 1) * (bw + 6 * s)
    local enabled = API.windows.ChildState(WINDOW, b[1])
    local tip = API.windows.ChildTooltip(WINDOW, b[1])
    BUTTON_OPTS.enabled, BUTTON_OPTS.primary, BUTTON_OPTS.tooltip = enabled ~= false, b[3], tip ~= "" and tip or nil
    if T.Button(b[4], bx, by, bx + bw, by + bh, s, b[2], BUTTON_OPTS) then
      API.windows.ClickChild(WINDOW, b[1], "left")
    end
  end
end

ui.window("Group", function()
  if not API.session.InGame() then return end
  for k = #members, 1, -1 do members[k] = nil end
  for i = 1, 5 do
    local m = member(i)
    if m then members[#members + 1] = m end
  end
  frame.invited = API.windows.ChildShown(WINDOW, "FollowButton") == true
  frame.canInvite = API.windows.ChildState(WINDOW, "InviteButton") == true
  frame.grouped = API.windows.ChildState(WINDOW, "DisbandButton") == true
  hud.Window(WINDOW, "Group", OPTS, body)
end)
