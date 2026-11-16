-- The Pet window (pet.lua) as a library, so a plugin's window can be the pack's with its own part added
-- (hs.Plugin windows: dofile("lib/pet.lua") finds this one). pet.window(opts) draws the whole window; opts (all
-- optional):
--   extra = { height = function(s) ... end, draw = function(x, y, w, s) ... end }: drawn between the pets' health
--     and the command buttons; height is unscaled (the window's size), draw returns the y below it
--   width = the window's (unscaled; 240)
--   fold = "<setting key>": the command buttons fold under a "Pet commands" header the player opens and closes
--     (kept in the UI settings under that key; folded at first)
-- The pet's buffs open the buff's menu on a right click (lib/buffmenu.lua: Spell Info and Add to Blocked Pet Spells).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local buffmenu = dofile("lib/buffmenu.lua")
local T = hud.T
local C = T.color
local M = {}
M.WINDOW = "PetInfoWindow"
local WINDOW = M.WINDOW
local MENU = { block = "pet" } -- (the pet's buffs: the Blocked Pet Buffs window)
local BUTTONS, PER_ROW = 14, 4
local MAX_BUFFS = 85
local ICON = 22
local FOLD_H = 22
local DRAG = "Drag onto an action bar to make a hot button"
local CHOOSE = "Right-click to choose its command"

-- The children's names, made once, and each one's hit box id.
local BUTTON_IDS, BUFF_IDS, HIT = {}, {}, {}
for i = 0, BUTTONS - 1 do BUTTON_IDS[i + 1] = "Pet" .. i .. "_Button" end
for i = 0, MAX_BUFFS - 1 do BUFF_IDS[i + 1] = "PetBuff" .. i end
local OTHER_IDS = { "Pet2HPGauge", "Pet3HPGauge" }
for _, list in ipairs({ BUTTON_IDS, BUFF_IDS, OTHER_IDS }) do
  for _, id in ipairs(list) do HIT[id] = "##" .. id end
end

-- The window's contents this frame (gather, run only while the window is drawn), in tables kept from frame to frame:
-- the first n of each list are this frame's.
local buttons, buffs, others = {}, {}, {}
local nButtons, nBuffs, nOthers = 0, 0, 0

local function entry(list, n)
  local e = list[n]
  if not e then
    e = {}
    list[n] = e
  end
  return e
end

local function gather()
  nButtons, nBuffs, nOthers = 0, 0, 0
  for i = 1, BUTTONS do
    local id = BUTTON_IDS[i]
    local text = API.windows.Text(WINDOW, id)
    if text and text ~= "" and API.windows.ChildShown(WINDOW, id) ~= false then
      nButtons = nButtons + 1
      local b = entry(buttons, nButtons)
      b.id, b.text = id, text
    end
  end
  for i = 1, MAX_BUFFS do
    local id = BUFF_IDS[i]
    local spell, ms, name, good = API.buffs.Slot(WINDOW, id)
    if spell then
      nBuffs = nBuffs + 1
      local b = entry(buffs, nBuffs)
      b.id, b.spell, b.ms, b.name, b.good = id, spell, ms, name, good
    end
  end
  for _, id in ipairs(OTHER_IDS) do
    if API.windows.ChildShown(WINDOW, id) == true then
      local frac, name = API.windows.ChildGauge(WINDOW, id)
      if frac then
        nOthers = nOthers + 1
        local o = entry(others, nOthers)
        o.id, o.frac, o.name = id, frac, name
      end
    end
  end
end

-- A command button's tool tip, by its words (made once for each).
local tooltips = {}
local function tooltip(text)
  local t = tooltips[text]
  if not t then
    t = text .. "\n" .. DRAG .. "\n" .. CHOOSE
    tooltips[text] = t
  end
  return t
end

local ICON_OPTS = { radius = 4 }
local buttonOpts = {} -- (T.Button's options, filled for each button: T.Button keeps nothing of them)

local function draw_buttons(x, by, w, pad, s)
  local bw = (w - 2 * pad - (PER_ROW - 1) * 4 * s) / PER_ROW
  for k = 1, nButtons do
    local b = buttons[k]
    local col, row = (k - 1) % PER_ROW, (k - 1) // PER_ROW
    local bx = x + pad + col * (bw + 4 * s)
    local byy = by + row * 28 * s
    local enabled, checked = API.windows.ChildState(WINDOW, b.id)
    local on = enabled ~= false
    buttonOpts.primary, buttonOpts.enabled = checked == true, on
    buttonOpts.tooltip = on and tooltip(b.text) or CHOOSE
    local clicked = T.Button(HIT[b.id], bx, byy, bx + bw, byy + 24 * s, s, b.text, buttonOpts)
    -- the right button (a "none" button's too) picks the command the button holds: the game's menu of the
    -- commands the pet takes
    if ui.IsItemClicked(ui.Mouse.Right) then API.windows.ClickChild(WINDOW, b.id, "rightdown") end
    -- a drag makes the command's hot button ("Pet <command>"), as the stock window's does
    local dragged = on and T.DragFrom(WINDOW, b.id)
    if clicked and not dragged then API.windows.ClickChild(WINDOW, b.id, "left") end
  end
end

-- This frame's window: its opts (M.window's), whether the commands are unfolded, the rows of commands and of buffs.
local opts, open, rows, buffRows = nil, true, 0, 0
local NO_OPTS = {}

-- (hud.Window asks for the size only while the window is drawn: the contents are read then)
local function size()
  gather()
  open = not opts.fold or ui.GetSetting(opts.fold, "0") == "1"
  rows = math.ceil(nButtons / PER_ROW)
  buffRows = math.ceil(nBuffs / 9)
  local extra = opts.extra
  local hb = open and rows * 28 or 0
  if opts.fold then hb = hb + FOLD_H end
  return opts.width or 240, 70 + nOthers * 18 + hb + (extra and extra.height(1) or 0) + (buffRows > 0 and buffRows * (ICON + 3) + 6 or 0)
end

local function body(x, y, w, h, s)
  local extra = opts.extra
  local pad = 9 * s
  local frac, name, raw = API.character.Gauge(16)
  local _, th = T.TextSize("Ag", T.font.small)
  local has = raw ~= nil and raw ~= -1
  T.Text(x + pad, y + pad - 2 * s, has and name or "No pet", has and C.text or C.muted, T.font.small)
  if has then T.Text(x + w - pad, y + pad - 2 * s, math.floor((frac or 0) * 100 + 0.5) .. "%", C.muted, T.font.small, "right") end
  local by = y + pad + th + 2 * s
  T.Bar("pet.hp", x + pad, by, x + w - pad, by + 10 * s, s, has and frac or nil, C.pet)
  if T.Hit("##pethp", x + pad, by, x + w - pad, by + 10 * s) then API.windows.ClickChild(WINDOW, "PetHPGauge", "left") end
  by = by + 14 * s
  local tf, tname, traw = API.character.Gauge(41)
  local ttext = (traw ~= nil and traw ~= -1 and tname and tname ~= "") and tname or "No target"
  T.Text(x + pad, by, "\xC2\xBB " .. T.Fit(ttext, w * 0.55, T.font.tiny), C.muted, T.font.tiny)
  T.Bar("pet.target", x + w * 0.6, by + 4 * s, x + w - pad, by + 9 * s, s, (traw ~= -1) and tf or nil, C.hp)
  by = by + 18 * s
  -- the other pets
  for n = 1, nOthers do
    local o = others[n]
    T.Text(x + pad, by, T.Fit(o.name, w * 0.45, T.font.tiny), C.muted, T.font.tiny)
    T.Text(x + w * 0.5, by, math.floor(o.frac * 100 + 0.5) .. "%", C.muted, T.font.tiny)
    T.Bar("pet." .. o.id, x + w * 0.6, by + 4 * s, x + w - pad, by + 9 * s, s, o.frac, C.pet)
    if T.Hit(HIT[o.id], x + pad, by, x + w - pad, by + 14 * s) then API.windows.ClickChild(WINDOW, o.id, "left") end
    by = by + 18 * s
  end
  if extra then by = extra.draw(x, by, w, s) end
  -- the commands, under a header that folds them when asked
  if opts.fold then
    local label = (open and "\xE2\x96\xBE " or "\xE2\x96\xB8 ") .. "Pet commands"
    T.Text(x + pad, by + 2 * s, label, C.muted, T.font.tiny)
    if T.Hit("##petfold", x + pad, by, x + w - pad, by + (FOLD_H - 4) * s) then
      open = not open
      ui.SetSetting(opts.fold, open)
    end
    by = by + FOLD_H * s
  end
  if open then draw_buttons(x, by, w, pad, s) end
  -- buffs
  local iy = by + (open and rows * 28 * s or 0) + 4 * s
  for k = 1, nBuffs do
    local b = buffs[k]
    local col, row = (k - 1) % 9, (k - 1) // 9
    local ix = x + pad + col * (ICON + 3) * s
    local yy = iy + row * (ICON + 3) * s
    local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, b.id, "decal")
    if type(tex) ~= "userdata" then tex = nil end
    T.Icon(ix, yy, ICON * s, s, tex, u0, v0, u1, v1, b.good and C.good or C.bad, ICON_OPTS)
    T.Hit(HIT[b.id], ix, yy, ix + ICON * s, yy + ICON * s, false)
    if ui.IsItemHovered() then
      ui.SetTooltip(b.name .. "\n" .. T.LongDuration(b.ms) .. "\n" .. buffmenu.Hint(MENU))
    end
    if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
      buffmenu.Open(WINDOW, b, MENU)
      T.TakeRightClick()
    end
  end
  buffmenu.Draw(WINDOW)
end

local HUD_OPTS = { size = size }

function M.window(o)
  if not API.session.InGame() then return end
  opts = o or NO_OPTS
  hud.Window(WINDOW, "Pet", HUD_OPTS, body)
end

return M
