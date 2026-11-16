-- The Pet window (pet.lua) in pieces, so a plugin's window can be the pack's with its own part added (hs.Plugin
-- windows: dofile("lib/pet.lua") finds this one):
--   P.visible() -> shown, name    whether the window shows this frame (the stock PetInfoWindow and the Pet tile;
--                                 call it once a frame), and the pet's name (nil: no pet)
--   P.HINTS                       the pad hints for the stock parts
--   P.status(x0, width, name) -> y         the pet's HP, its target and the other pets, from the cursor down
--   P.commands(x0, y, width, name, settled) -> y   the stock command buttons under a "Commands" heading
--   P.buffs(rx0, y0, x1, y1)      the pet's buffs: icon, name, time left
--   P.window()                    the pack's window: status and commands on the left, buffs on the right
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local AS = dofile("lib/assign.lua")
local C = T.C

local P = {}
local PW = "PetInfoWindow"
P.WINDOW = PW
local BUTTONS = 14 -- Pet0_Button .. Pet13_Button
local BUFFS = 85   -- PetBuff0 .. PetBuff84
local BUFF_IDS = {}
for i = 0, BUFFS - 1 do BUFF_IDS[i] = "PetBuff" .. i end
P.HINTS = { { "a", "Command" }, { "x", "Assign" }, { "y", "Choose command" }, { "b", "Close" } }

-- The main menu's Pet tile: shows the stock window if it is hidden (the pet window key's toggle), and opens this.
-- Set once, by the pack's own copy of this file: a plugin's Pet window that runs this file too would otherwise put its
-- own copy there, which runs as the plugin and stops working when it unloads (docs/plugins.md "Plugin windows").
GPSTATE.openPet = GPSTATE.openPet or function()
  if not API.windows.Shown(PW) then API.windows.Show(PW, true) end
  GPSTATE.petOpen = true
  return true
end

local was = { shown = false, pet = false }

local function petName()
  local _, name, raw = API.character.Gauge(16)
  if raw == -1 or not name or name == "" then return nil end
  return name
end

function P.visible()
  local shown = W.shown(PW)
  local name = petName()
  -- the stock window shown again by its key or the EQ menu (not by a summon: the pet was there before)
  if shown and not was.shown and was.pet then GPSTATE.petOpen = true end
  was.shown, was.pet = shown, name ~= nil
  if not shown then
    GPSTATE.petOpen = false
    return false
  end
  return GPSTATE.petOpen == true, name
end

function P.status(x0, leftW, name)
  local wx, wy = ui.GetWindowPos()
  local frac = API.character.Gauge(16)
  local pct = API.character.Label(69) or ""
  local clicked, _, bx0, by0, bx1, by1 = T.button("##pethp", "", leftW, T.px(92), { disabled = name == nil }, pad)
  T.font(24)
  T.textIn(bx0 + T.px(18), by0 + T.px(6), bx1 - T.px(18), by0 + T.px(46), name or "No Pet", C.text, 0)
  T.textIn(bx0 + T.px(18), by0 + T.px(6), bx1 - T.px(18), by0 + T.px(46), pct ~= "" and (pct .. "%") or "", C.dim, 1)
  T.unfont()
  if name then T.bar(bx0 + T.px(18), by1 - T.px(34), bx1 - T.px(18), by1 - T.px(16), frac, C.good) end
  if clicked then API.windows.ClickChild(PW, "PetHPGauge", "left") end -- targets the pet
  local tfrac, tname, traw = API.character.Gauge(41)
  local ty = by1 + T.px(14)
  T.rect(x0, ty, x0 + leftW, ty + T.px(80), C.sunken, 1, T.px(12))
  T.font(18)
  T.draw(x0 + T.px(18), ty + T.px(8), "PET'S TARGET", C.dim)
  T.unfont()
  T.font(22)
  local hasTarget = name and traw ~= -1 and traw ~= -2 and tname and tname ~= ""
  T.draw(x0 + T.px(18), ty + T.px(32), hasTarget and T.fit(tname, leftW * 0.45) or "No Target", hasTarget and C.text or C.faint)
  T.unfont()
  if hasTarget and tfrac then T.bar(x0 + leftW * 0.5, ty + T.px(40), x0 + leftW - T.px(18), ty + T.px(58), tfrac, C.hp) end
  -- the other pets (Pet2HPGauge / Pet3HPGauge: shown only while the server has such a pet)
  local oy = ty + T.px(80)
  for _, id in ipairs({ "Pet2HPGauge", "Pet3HPGauge" }) do
    if API.windows.ChildShown(PW, id) == true then
      local ofrac, oname = API.windows.ChildGauge(PW, id)
      if ofrac then
        ui.SetCursorPos(x0 - wx, oy + T.px(10) - wy)
        local oc, _, ox0, oy0, ox1, oy1 = T.button("##" .. id, "", leftW, T.px(56), {}, pad)
        T.font(20)
        T.textIn(ox0 + T.px(18), oy0, ox0 + leftW * 0.45, oy1, T.fit(oname or "", leftW * 0.4), C.text, 0)
        T.textIn(ox0 + leftW * 0.45, oy0, ox0 + leftW * 0.5 - T.px(8), oy1,
          math.floor(ofrac * 100 + 0.5) .. "%", C.dim, 1)
        T.unfont()
        T.bar(ox0 + leftW * 0.5, oy0 + T.px(20), ox1 - T.px(18), oy0 + T.px(36), ofrac, C.good)
        if oc then API.windows.ClickChild(PW, id, "left") end -- targets that pet
        oy = oy1
      end
    end
  end
  return oy
end

function P.commands(x0, y, leftW, name, settled, cols)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y - wy)
  T.heading("Commands", leftW)
  cols = cols or 3
  local gap = T.px(10)
  local bw = (leftW - (cols - 1) * gap) / cols
  local n = 0
  local bottom = y
  for i = 0, BUTTONS - 1 do
    local id = "Pet" .. i .. "_Button"
    local label = API.windows.Text(PW, id) or ""
    if label ~= "" and API.windows.ChildShown(PW, id) ~= false then
      if n % cols ~= 0 then ui.SameLine(0, gap) end
      n = n + 1
      local enabled, checked = API.windows.ChildState(PW, id)
      local none = label == "none"
      local text = none and "(none)" or (label:sub(1, 1):upper() .. label:sub(2))
      local c2, focused, cx0, cy0, cx1, cy1 = T.button("##" .. id, text, bw, T.px(76),
        { disabled = enabled == false or name == nil, primary = checked == true }, pad)
      bottom = cy1
      if checked then -- a state that is on (sit, follow, guard, taunt, ...)
        T.font(15)
        T.textIn(cx0, cy0 + T.px(4), cx1 - T.px(10), cy0 + T.px(24), "ON", C.onAccent, 1)
        T.unfont()
      end
      if c2 and settled then API.windows.ClickChild(PW, id, "left") end
      -- X: the hot button the stock button's drag makes (it needs no pet, as the drag does not)
      if focused and enabled ~= false and W.pressed("x") then
        AS.start("Pet " .. label, function() return API.cursor.PickUp(PW, id) end, "Pet")
      end
      -- Y: the command the button holds (the stock button's right button: the game's menu of the commands the pet
      -- takes, then "none"), a "none" button's too
      if focused and W.pressed("y") then API.windows.ClickChild(PW, id, "rightdown") end
    end
  end
  return bottom
end

function P.buffs(rx0, y0, x1, y1)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(rx0 - wx, y0 - wy)
  T.heading("Buffs", x1 - rx0)
  local _, by = ui.GetCursorScreenPos()
  local size = T.px(44)
  local rowH = size + T.px(10)
  local count = 0
  T.font(19)
  local _, th = ui.CalcTextSize("Ag")
  for i = 0, BUFFS - 1 do
    local spell, ms, bname = API.buffs.Slot(PW, BUFF_IDS[i])
    if spell then
      if by + rowH > y1 then break end
      count = count + 1
      local tex, u0, v0, u1, v1 = API.windows.ChildArt(PW, BUFF_IDS[i], "decal")
      T.rect(rx0 - 2, by - 2, rx0 + size + 2, by + size + 2, C.raised, 1, T.px(6))
      T.icon(tex, u0, v0, u1, v1, rx0, by, rx0 + size, by + size)
      local tx = rx0 + size + T.px(14)
      local time = (ms and ms > 0) and T.duration(ms / 1000) or ""
      local tw = ui.CalcTextSize(time)
      T.draw(tx, by + (size - th) / 2, T.fit(bname or "", x1 - tx - tw - T.px(20)))
      T.draw(x1 - tw, by + (size - th) / 2, time, (ms and ms > 0 and ms < 30000) and C.endurance or C.dim)
      by = by + rowH
    end
  end
  if count == 0 then T.draw(rx0, by, "No buffs.", C.faint) end
  T.unfont()
end

function P.window()
  local open, name = P.visible()
  if not open then return end
  if W.begin("Pet", { w = 1240, h = 860, title = "Pet", subtitle = name or "You have no pet.", hints = P.HINTS }) then
    local settled = AS.settled("Pet") -- (no A press just after the slot picker closed)
    local x0, y0, x1, y1 = W.area()
    local leftW = T.px(720)
    local oy = P.status(x0, leftW, name)
    P.commands(x0, oy + T.px(24), leftW, name, settled)
    P.buffs(x0 + leftW + T.px(40), y0, x1, y1)
  end
  if W.finish() then GPSTATE.petOpen = false end
end

return P
