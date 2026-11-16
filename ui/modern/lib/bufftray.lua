-- A buff tray: the buffs of a buff window (BuffWindow, ShortDurationBuffWindow) as a grid of icons, packed (empty
-- slots left out), with a ring for beneficial / detrimental, the time left under each and the game's fade under 19
-- seconds left (hs.BuffSlot's fade: out once a second; a bard's own short songs do not fade); hover shows the spell
-- and time. Right-click: the buff's menu (lib/buffmenu.lua: Spell Info, Add to Blocked Buffs and Remove).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local buffmenu = dofile("lib/buffmenu.lua")
local T = hud.T
local C = T.color

local tray = {}
local MENU = { block = "buff", remove = true } -- (the player's own buffs)

-- tray.Register(scriptName, window, slots, opts)  opts: icon, perRow (at most: the tray wraps to the room the default
-- layout gives it at the top right, lib/layout.lua trayRoom, unless the player set Icons per row)
function tray.Register(name, window, slots, opts)
  local ids, hits = {}, {}
  for i = 0, slots - 1 do
    ids[i + 1] = "Buff" .. i
    hits[i + 1] = "##Buff" .. i
  end
  local iconKey, perRowKey = "hud.icon." .. window, "hud.perRow." .. window
  -- The buffs shown, packed (n of them; each a record kept from frame to frame), and the tray's layout: read once a
  -- frame, when the frame shows (hideWhen and size, which hud.Window calls first).
  local list, n, frame = {}, 0, -1
  local icon, perRow, cols, rows
  local gap, label = 4, 12
  local function collect()
    local f = ui.GetFrameCount()
    if f == frame then return end
    frame = f
    n = 0
    for i = 1, slots do
      -- the spell and the counters the game puts on the button (hits left, counters): hs.BuffSlot's overlays
      local spell, ms, spellName, good, fade, counter, cr, cg, cb, limit = API.buffs.Slot(window, ids[i], true)
      if spell then
        n = n + 1
        local b = list[n]
        if not b then
          b = { counterColor = { 0, 0, 0, 1 }, icon = {} }
          list[n] = b
        end
        b.id, b.hit, b.spell, b.ms, b.name, b.good, b.fade = ids[i], hits[i], spell, ms, spellName, good, fade or 1
        b.counter, b.limit = counter, limit
        if counter then b.counterColor[1], b.counterColor[2], b.counterColor[3] = cr, cg, cb end
        -- the time left under it (T.Duration), formatted again only when what it shows changes
        local key = (not ms or ms < 0 or ms >= 100 * 3600000) and -1 or math.floor(ms / 1000)
        if b.durKey ~= key then b.durKey, b.duration = key, T.Duration(ms) end
      end
    end
    icon = tonumber(ui.GetSetting(iconKey)) or opts.icon
    local room = hud.Layout().trayRoom
    local fits = room and math.floor((room / hud.WindowScale(window) - 12 + gap) / (icon + gap)) or opts.perRow
    perRow = math.max(1, math.floor(tonumber(ui.GetSetting(perRowKey)) or math.max(1, math.min(opts.perRow, fits))))
    cols = math.max(1, math.min(perRow, n))
    rows = math.max(1, math.ceil(n / perRow))
  end
  local winOpts = {
    size = function()
      collect()
      return cols * (icon + gap) - gap + 12, rows * (icon + label + gap) - gap + 10
    end,
    panel = false,
    hideWhen = function()
      collect()
      return n == 0
    end,
    menu = function()
      ui.SetNextItemWidth(180)
      local v, changed = ui.SliderInt("Icons per row", perRow, 1, 20)
      if changed then ui.SetSetting(perRowKey, v) end
      ui.SetNextItemWidth(180)
      v, changed = ui.SliderInt("Icon size", icon, 18, 64)
      if changed then ui.SetSetting(iconKey, v) end
    end,
  }
  local function body(x, y, w, h, s)
    -- right-aligned from the top-right corner, like most trays
    for i = 1, n do
      local b = list[i]
      local col, row = (i - 1) % perRow, (i - 1) // perRow
      local ix = x + w - 6 * s - (col + 1) * icon * s - col * gap * s
      local iy = y + 5 * s + row * (icon + label + gap) * s
      local size = icon * s
      local tex, u0, v0, u1, v1 = API.windows.ChildArt(window, b.id, "decal")
      if type(tex) ~= "userdata" then tex = nil end
      b.icon.alpha = 0.2 + 0.8 * b.fade -- never quite gone, so the tray keeps its shape
      T.Icon(ix, iy, size, s, tex, u0, v0, u1, v1, b.good and C.good or C.bad, b.icon)
      T.Hit(b.hit, ix, iy, ix + size, iy + size + label * s, false)
      T.HoverFx(ix, iy, ix + size, iy + size, s)
      if ui.IsItemHovered() then
        ui.BeginTooltip()
        ui.TextColored(b.good and C.good[1] or C.bad[1], b.good and C.good[2] or C.bad[2],
          b.good and C.good[3] or C.bad[3], 1, b.name)
        ui.TextDisabled(T.LongDuration(b.ms))
        ui.TextDisabled(buffmenu.Hint(MENU))
        ui.EndTooltip()
      end
      if ui.IsItemClicked(ui.Mouse.Right) and not ui.GetIO().alt then
        buffmenu.Open(window, b, MENU)
        T.TakeRightClick()
      end
      -- the counters the game puts on the button (hits left, counters), top right
      if b.counter then T.Text(ix + size - 2 * s, iy + 1 * s, b.counter, b.counterColor, T.font.tiny, "right") end
      if b.limit then T.Text(ix + 2 * s, iy + size - 10 * s, b.limit, C.gold, T.font.tiny) end
      local tc = (b.ms and b.ms >= 0 and b.ms < 60000) and C.gold or C.muted
      T.Text(ix + size / 2, iy + size + 1 * s, b.duration, tc, T.font.tiny, "center")
    end
    buffmenu.Draw(window)
  end
  ui.window(name, function()
    if not API.session.InGame() then return end
    hud.Window(window, name, winOpts, body)
  end)
end

return tray
