-- Abilities (gamepad pack):
--   Actions (ActionsWindow)        the stock window's four pages (LB / RB): Main (who, invite, camp, sit, run ...),
--                                  Abilities (its six skill buttons), Combat (melee, range and four combat skill
--                                  buttons, and the disciplines) and Socials. A presses the focused button as a click
--                                  does; on a skill button Y chooses its skill (the stock right click) and X puts it
--                                  on a hot button / cross hotbar slot (the stock drag).
--   Select Skill (SkillsSelectWindow)   the skill chooser a skill button of either page opens: A picks.
--   Combat Abilities (CombatAbilityWnd)   the disciplines (eq.Disciplines: icon, level, endurance, reuse time left)
--                                  with the focused one's description; A uses it as its hot button does
--                                  (doCombatAbility), X assigns it to a slot.
-- Everything goes through the stock windows' models, so the game's rules apply.
local W = dofile("lib/window.lua")
local T = W.T -- the window module's theme: the scale W.begin computed this frame
local pad = W.pad
local AS = dofile("lib/assign.lua")
local TB = dofile("lib/textbox.lua")
local C = T.C

local AW = "ActionsWindow"
local PAGES = { "Main", "Abilities", "Combat", "Socials" }
local ORD = { "First", "Second", "Third", "Fourth", "Fifth", "Sixth" }
local message, messageAt
local settled = true -- (AS.settled) A presses act
local focusFirst = false -- a page was switched: the pad's focus onto its first button

-- The stock buttons' own text (EQUI_ActionsWindow.xml), for those the window's code leaves as they are.
local XML_TEXT = {
  AMP_WhoButton = "Who", AMP_FindButton = "Find", AMP_EndFindButton = "End Find", AMP_InviteButton = "Invite",
  AMP_FollowButton = "Follow", AMP_DisbandButton = "Disband", AMP_CampButton = "Camp (Exit)", AMP_SitButton = "Sit",
  AMP_StandButton = "Stand", AMP_RunButton = "Run", AMP_WalkButton = "Walk", ACP_MeleeAttackButton = "Melee Attack",
  ACP_RangeAttackButton = "Range Attack",
}

local function say(text) message, messageAt = text, ui.GetTime() end

local function tabs(x0, y0, page, names, onClick)
  local x = x0
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  for i, l in ipairs(names) do
    local w = ui.CalcTextSize(l) + T.px(40)
    local on = page == i - 1
    T.rect(x, y0, x + w, y0 + th + T.px(16), on and C.accent or C.raised, on and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + th + T.px(16), l, on and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + th + T.px(16)) and ui.IsMouseClicked(ui.Mouse.Left) then onClick(i - 1) end
    x = x + w + T.px(10)
  end
  T.unfont()
  return y0 + th + T.px(28)
end

-- The usable skills by name, with their reuse time left (eq.UsableSkills).
local function usableByName()
  local out = {}
  for _, s in ipairs(eq.UsableSkills()) do out[s.name] = s end
  return out
end

-- One of the Actions window's buttons, `w` x `h` at the cursor. kind: "skill" (an ability / combat skill button),
-- else a plain button. Returns whether it is focused.
local function actionButton(id, kind, w, h, usable)
  if eq.ChildShown(AW, id) == false then return false end
  local text, set = eq.WindowText(AW, id)
  text = text or ""
  if not set and text == "" and kind ~= "skill" then text = XML_TEXT[id] or "" end
  local enabled, checked = eq.ChildState(AW, id)
  local label, sub = text, nil
  if kind == "skill" then
    if text == "" then label, sub = "Empty", "Y: choose a skill"
    else
      local u = usable[text]
      sub = u and (u.remaining > 0 and ("Ready in " .. T.duration(u.remaining)) or "Ready") or nil
    end
  elseif checked then
    sub = "On"
  end
  if focusFirst then
    ui.SetKeyboardFocusHere()
    focusFirst = false
  end
  local clicked, focused = T.button("##" .. id, label, w, h, { sub = sub, disabled = enabled == false, align = 0 }, pad)
  if clicked and settled then eq.ClickChild(AW, id, "left") end
  if focused and kind == "skill" then
    if W.pressed("y") then eq.ClickChild(AW, id, "right") end
    if W.pressed("x") then
      if text == "" or not AS.start(text, function() return eq.PickUp(AW, id) end, "Actions") then say("Nothing to assign.") end
    end
  elseif focused and W.pressed("x") then
    if not AS.start(label, function() return eq.PickUp(AW, id) end, "Actions") then say("Nothing to assign.") end
  end
  return focused
end

-- Buttons in a grid of `cols` columns from the cursor.
local function grid(ids, kind, cols, x0, x1, h, usable)
  local gap = T.px(14)
  local w = (x1 - x0 - (cols - 1) * gap) / cols
  local n = 0
  for _, id in ipairs(ids) do
    if eq.ChildShown(AW, id) ~= false then
      if n % cols ~= 0 then ui.SameLine(0, gap) end
      n = n + 1
      actionButton(id, kind, w, h, usable)
    end
  end
end

-- The pages' buttons (fixed).
local GENERAL_IDS = { "AMP_WhoButton", "AMP_FindButton", "AMP_EndFindButton", "AMP_InviteButton", "AMP_FollowButton",
  "AMP_DisbandButton", "AMP_CampButton", "AMP_SitButton", "AMP_StandButton", "AMP_RunButton", "AMP_WalkButton" }
local ABILITY_IDS, COMBAT_IDS, SOCIAL_IDS = {}, {}, {}
for i = 1, 6 do ABILITY_IDS[i] = "AAP_" .. ORD[i] .. "AbilityButton" end
for i = 1, 4 do COMBAT_IDS[i] = "ACP_" .. ORD[i] .. "AbilityButton" end
for i = 1, 12 do SOCIAL_IDS[i] = "ASP_SocialButton" .. i end
local ATTACK_IDS = { "ACP_MeleeAttackButton", "ACP_RangeAttackButton" }

-- How many disciplines are known (the Disciplines button's text), read every 0.25 s: eq.Disciplines builds them all.
local discCount, discCountAt = 0, nil
local function disciplineCount()
  local now = ui.GetTime()
  if not discCountAt or now - discCountAt >= 0.25 or now < discCountAt then
    discCount, discCountAt = #eq.Disciplines(), now
  end
  return discCount
end

ui.window("Actions", function()
  if not W.shown(AW) then
    discCountAt = nil
    return
  end
  local page = eq.TabPage(AW, "ACTW_ActionsSubwindows") or 0
  if W.begin("Actions", { w = 1200, h = 800, title = "Actions", subtitle = PAGES[page + 1],
        hints = { { "a", "Use" }, { "y", "Choose skill" }, { "x", "Assign to slot" }, { "lb", "Page" }, { "rb", "Page" }, { "b", "Close" } } }) then
    settled = AS.settled("Actions")
    local function setPage(p)
      eq.SetTabPage(AW, "ACTW_ActionsSubwindows", p)
      focusFirst = true
    end
    if W.pressed("lb") then setPage((page + 3) % 4) end
    if W.pressed("rb") then setPage((page + 1) % 4) end
    page = eq.TabPage(AW, "ACTW_ActionsSubwindows") or page
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local top = tabs(x0, y0, page, PAGES, setPage)
    ui.SetCursorPos(x0 - wx, top - wy)
    local usable = usableByName()
    local h = T.px(84)
    if page == 0 then
      grid(GENERAL_IDS, "plain", 3, x0, x1, h, usable)
    elseif page == 1 then
      grid(ABILITY_IDS, "skill", 2, x0, x1, h, usable)
    elseif page == 2 then
      grid(ATTACK_IDS, "plain", 2, x0, x1, h, usable)
      grid(COMBAT_IDS, "skill", 2, x0, x1, h, usable)
      -- the disciplines: the combat abilities window (CMD_TOGGLE_COMBAT_ABILITY_WIN), for the classes that have it
      local n = disciplineCount()
      if T.button("##disc", "Disciplines", x1 - x0, h, { sub = n > 0 and (n .. " combat abilities") or "None known",
            disabled = n == 0, align = 0 }, pad) and settled then
        eq.ShowWindow("CombatAbilityWnd", true)
      end
    else
      local gap = T.px(14)
      local bw = T.px(84)
      if T.button("##prev", "<", bw, T.px(60), {}, pad) then eq.ClickChild(AW, "ASP_SocialPageLeftButton", "left") end
      ui.SameLine(0, gap)
      local cx, cy = ui.GetCursorScreenPos()
      T.font(22)
      T.textIn(cx, cy, cx + T.px(160), cy + T.px(60), "Page " .. (eq.WindowText(AW, "ASP_CurrentSocialPageLabel") or ""), C.text, 0.5)
      T.unfont()
      ui.Dummy(T.px(160), T.px(60))
      ui.SameLine(0, gap)
      if T.button("##next", ">", bw, T.px(60), {}, pad) then eq.ClickChild(AW, "ASP_SocialPageRightButton", "left") end
      grid(SOCIAL_IDS, "plain", 3, x0, x1, T.px(70), usable)
    end
    if message and ui.GetTime() - messageAt < 3 then
      T.font(20)
      T.textIn(x0, y1 - T.px(30), x1, y1, message, C.endurance, 0.5)
      T.unfont()
    end
  end
  if W.finish() then eq.ShowWindow(AW, false) end
end)

-- ---------------------------------------------------------------- the skill choosers
local function chooser(screen, title, listId, makeHotkey)
  local state = { offset = 0 }
  ui.window(title, function()
    if not W.shown(screen) then return end
    if W.begin(title, { w = 760, h = 820, title = title, subtitle = "For the Actions window's button",
          hints = { { "a", "Choose" }, { "b", "Cancel" } } }) then
      local x0, y0, x1, y1 = W.area()
      local rows = eq.ListInfo(screen, listId) or 0
      local rowH = 54
      local visible = math.max(1, math.floor((y1 - y0 + T.px(10)) / T.px(rowH + 10)))
      W.list(listId, state, rows, visible, rowH, x1 - x0, function(i, rx0, ry0, rx1, ry1)
        local texts = eq.ListRow(screen, listId, i - 1)
        T.textIn(rx0 + T.px(20), ry0, rx1 - T.px(20), ry1, texts and texts[1] or "", C.text, 0)
      end, function(i)
        eq.ListClick(screen, listId, i - 1, "left")
        if makeHotkey then eq.ClickChild(screen, makeHotkey, "left") end
        pad.focus("Actions")
      end)
    end
    if W.finish() then
      eq.ShowWindow(screen, false)
      pad.focus("Actions")
    end
  end)
end
chooser("SkillsSelectWindow", "Select Skill", "SKSW_SkillSelectorList")

-- ---------------------------------------------------------------- the combat abilities (disciplines)
local CW = "CombatAbilityWnd"
local discList = { offset = 0 }
local discDesc = {}

local function cooldownTime(sec)
  sec = math.ceil(sec)
  if sec >= 3600 then return string.format("%d:%02d:%02d", sec // 3600, (sec // 60) % 60, sec % 60) end
  return string.format("%d:%02d", sec // 60, sec % 60)
end

ui.window("Combat Abilities", function()
  if not W.shown(CW) then return end
  local discs = eq.Disciplines()
  local focusedDisc
  if W.begin("Combat Abilities", { w = 1600, h = 900, title = "Combat Abilities", subtitle = #discs .. " disciplines",
        hints = { { "a", "Use" }, { "x", "Assign to slot" }, { "y", "Actions" }, { "b", "Close" } } }) then
    local discSettled = AS.settled("Combat Abilities")
    if W.pressed("y") then -- the Actions window's combat page (the COMBAT key)
      eq.SetTabPage(AW, "ACTW_ActionsSubwindows", 2)
      eq.ShowWindow(AW, true)
    end
    local x0, y0, x1, y1 = W.area()
    if eq.ChildShown(CW, "CAW_CombatEffectButton") == true then
      local icon = T.px(64)
      ui.SetCursorScreenPos(x0, y0)
      if T.button("##cawEffect", "", icon, icon, nil, pad) and discSettled then
        eq.ClickChild(CW, "CAW_CombatEffectButton", "left")
      end
      if ui.IsItemHovered() then ui.SetTooltip("End this discipline") end
      local tex, u0, v0, u1, v1 = eq.ChildArt(CW, "CAW_CombatEffectButton", "decal")
      T.icon(tex, u0, v0, u1, v1, x0, y0, x0 + icon, y0 + icon)
      local tx = x0 + icon + T.px(16)
      local fraction, remaining = eq.Gauge(26)
      T.font(22)
      local timerW = remaining and remaining ~= "" and (ui.CalcTextSize(remaining) + T.px(16)) or 0
      local name = eq.WindowText(CW, "CAW_CombatEffectLabel") or ""
      T.textIn(tx, y0, x1 - timerW, y0 + T.px(32), T.fit(name, math.max(0, x1 - tx - timerW)), C.text, 0)
      T.textIn(tx, y0, x1, y0 + T.px(32), remaining or "", C.dim, 1)
      T.unfont()
      T.bar(tx, y0 + T.px(44), x1, y0 + T.px(54), fraction or 0, C.accent)
      y0 = y0 + T.px(80)
      ui.SetCursorScreenPos(x0, y0)
    end
    local listX1 = x1 - T.px(600)
    local rowH = 72
    local visible = math.max(1, math.floor((y1 - y0 + T.px(10)) / T.px(rowH + 10)))
    W.list("disc", discList, #discs, visible, rowH, listX1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
      local d = discs[i]
      if d.remaining > 0 then
        local fraction = math.min(1, d.remaining / math.max(d.reuse, d.remaining, 1))
        T.rect(rx0, ry0, rx1, ry1, C.bad, 0.08, T.px(10))
        T.rect(rx0, ry0, rx0 + (rx1 - rx0) * fraction, ry1, C.bad, 0.20, T.px(10))
        T.rect(rx0 + T.px(4), ry1 - T.px(5), rx0 + T.px(4) + (rx1 - rx0 - T.px(8)) * fraction,
          ry1 - T.px(2), C.bad, 0.75, T.px(2))
      end
      local s = ry1 - ry0 - T.px(12)
      local tex, u0, v0, u1, v1 = eq.SpellIcon(d.icon)
      if tex then T.icon(tex, u0, v0, u1, v1, rx0 + T.px(8), ry0 + T.px(6), rx0 + T.px(8) + s, ry0 + T.px(6) + s, d.remaining > 0 and 0.45 or 1) end
      T.textIn(rx0 + s + T.px(24), ry0, rx1 - T.px(330), ry1, T.fit(d.name, rx1 - rx0 - s - T.px(360)),
        d.remaining > 0 and C.dim or C.text, 0)
      T.font(18)
      if d.level > 0 and d.level < 255 then T.textIn(rx1 - T.px(320), ry0, rx1 - T.px(200), ry1, "Lvl " .. d.level, C.dim, 0) end
      T.textIn(rx1 - T.px(200), ry0, rx1 - T.px(16), ry1,
        d.remaining > 0 and cooldownTime(d.remaining) or (d.endurance > 0 and (d.endurance .. " end") or "Ready"),
        d.remaining > 0 and C.bad or C.good, 1)
      T.unfont()
      if focused then
        focusedDisc = d
        if W.pressed("x") then
          if not AS.start(d.name, function() return eq.PickUp(CW, "Discipline", d.index + 1) end, "Combat Abilities") then
            message, messageAt = "Nothing to assign.", ui.GetTime()
          end
        end
      end
    end, function(i)
      if not discSettled then return end
      local d = discs[i]
      if d then eq.PressButtonFrom(CW, "Discipline", d.index + 1) end
    end)
    if #discs == 0 then T.draw(x0, y0, "You have no combat abilities.", C.dim) end
    -- the focused discipline: its icon, name and the spell display's text
    local rx0 = listX1 + T.px(24)
    T.rect(rx0, y0, x1, y1, C.sunken, 1, T.px(14))
    if focusedDisc then
      local px, py = rx0 + T.px(18), y0 + T.px(16)
      local is = T.px(64)
      local tex, u0, v0, u1, v1 = eq.SpellIcon(focusedDisc.icon)
      if tex then T.icon(tex, u0, v0, u1, v1, px, py, px + is, py + is) end
      T.font(24)
      T.textIn(px + is + T.px(14), py, x1 - T.px(18), py + is, T.fit(focusedDisc.name, x1 - px - is - T.px(40)), C.text, 0)
      T.unfont()
      py = py + is + T.px(14)
      T.font(18)
      local reuse = focusedDisc.reuse > 0 and ("Reuse " .. T.duration(focusedDisc.reuse)) or "No reuse time"
      T.draw(px, py, reuse .. (focusedDisc.remaining > 0 and ("   (ready in " .. cooldownTime(focusedDisc.remaining) .. ")") or ""), C.dim)
      T.unfont()
      py = py + T.px(34)
      local _, stml = eq.SpellDetails(focusedDisc.spell)
      T.font(19)
      TB.draw(stml or "", px, py, x1 - T.px(14), y1 - T.px(14), discDesc)
      T.unfont()
    end
    if message and ui.GetTime() - messageAt < 3 then
      T.font(20)
      T.textIn(x0, y1 - T.px(30), listX1, y1, message, C.endurance, 0.5)
      T.unfont()
    end
  end
  if W.finish() then eq.ShowWindow(CW, false) end
end)
