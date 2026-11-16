-- Spell displays: replace the game's SpellDisplayWindow. The game shows one for a spell gem held with the right
-- button (the Modern bar keeps it if released over it), the book's "Spell window", a link, ...; several can be open at once, as
-- SpellDisplayWindow, SpellDisplayWindow#2, ... (hs.ReplaceWindow numbers windows that share a screen name). Each is
-- a small resizable dialog: the spell's icon and name over the game's own description (SDW_SpellDescription: level,
-- mana, cast and recast times, duration, the effects), which scrolls when the window is short. Opening and closing
-- stay the game's: the close box closes that display.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local INSTANCES = 6
local ICON_OPTS = { radius = 6 }

-- testing (test.open.SpellDisplayWindow=1): the first gem's spell, else Complete Heal
local function testOpen()
  local st = API.spells.BookState()
  local id = st and st.gems and st.gems[1] or -1
  API.spells.Show(id ~= -1 and id or 13)
end
local function size() return 360, 300 end

for i = 1, INSTANCES do
  local window = i == 1 and "SpellDisplayWindow" or ("SpellDisplayWindow#" .. i)
  -- made once per window, not every frame
  local opts = {
    key = window,
    keepTitle = true,
    size = size,
    resizable = { 240, 140 },
    place = function(W_, H, w, h, s, x, y) return x + (i - 1) * 24, y + (i - 1) * 24 end,
  }
  local function body(x, y, w, h, s)
    local name = API.windows.Title(window) or ""
    local tex, u0, v0, u1, v1 = API.windows.ChildArt(window, "SDW_IconButton", "decal")
    if type(tex) ~= "userdata" then tex = nil end
    local size = 40 * s
    T.Icon(x, y, size, s, tex, u0, v0, u1, v1, nil, ICON_OPTS)
    local _, nh = T.TextSize("Ag", T.font.normal)
    T.Text(x + size + 10 * s, y + (size - nh) / 2, T.Fit(name ~= "" and name or "Spell", w - size - 10 * s,
      T.font.normal), C.text, T.font.normal)
    local ly = y + size + 8 * s
    ui.DrawLine(x, ly, x + w, ly, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
    ui.SetCursorScreenPos(x, ly + 6 * s)
    ui.PushFont(T.font.small)
    W.Stml(window, "SDW_SpellDescription", w, y + h - ly - 6 * s)
    ui.PopFont()
  end
  ui.window("Spell Display" .. (i == 1 and "" or (" " .. i)), function()
    if not API.session.InGame() then return end
    if i == 1 then P.TestOpen(window, testOpen) end
    hud.Dialog(window, "Spell", opts, body)
  end)
end
