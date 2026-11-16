-- Skills (gamepad pack): stands in for SkillsWindow. The stock list (hs.ListRow: name, rank, value / cap in the
-- stock colours: red under the trained value, green over it, yellow over the cap), then the languages. A uses a skill
-- the player can activate (the skills the Actions window's buttons take, with their reuse time left), as its hot
-- button does; X puts the skill on a hot button or cross hotbar slot (the stock Make Hotkey button's hotbutton).
local API = hs.UI
local W = dofile("lib/window.lua")
local T = W.T -- the window module's theme: the scale W.begin computed this frame
local pad = W.pad
local AS = dofile("lib/assign.lua")
local C = T.C

local SW = "SkillsWindow"
local list = { offset = 0 }

ui.window("Skills", function()
  if not W.shown(SW) then return end
  -- the skills that can be used, by name (the list's first column)
  local usable = {}
  for _, s in ipairs(API.abilities.UsableSkills()) do usable[s.name] = s end
  local rows = API.windows.ListInfo(SW, "SkillList") or 0
  if W.begin("Skills", { w = 1100, h = 900, title = "Skills", subtitle = rows .. " skills and languages",
        hints = { { "a", "Use" }, { "x", "Assign to slot" }, { "b", "Close" } } }) then
    local settled = AS.settled("Skills")
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    -- the column heads
    T.font(18)
    T.draw(x0 + T.px(20), y0, "SKILL", C.dim)
    T.textIn(x1 - T.px(470), y0, x1 - T.px(260), y0 + T.px(24), "RANK", C.dim, 0)
    T.textIn(x1 - T.px(260), y0, x1 - T.px(40), y0 + T.px(24), "VALUE", C.dim, 1)
    T.unfont()
    local top = y0 + T.px(34)
    ui.SetCursorPos(x0 - wx, top - wy)
    local rowH = 54
    local visible = math.max(1, math.floor((y1 - top + T.px(10)) / T.px(rowH + 10)))
    W.list("skills", list, rows, visible, rowH, x1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
      local texts, r, g, b = API.windows.ListRow(SW, "SkillList", i - 1)
      if not texts then return end
      local name = texts[1] or ""
      local u = usable[name]
      T.textIn(rx0 + T.px(20), ry0, rx1 - T.px(480), ry1, name, C.text, 0)
      T.textIn(rx1 - T.px(470), ry0, rx1 - T.px(270), ry1, texts[2] or "", C.dim, 0)
      T.textIn(rx1 - T.px(260), ry0, rx1 - T.px(30), ry1, texts[3] or "", { r or 1, g or 1, b or 1, 1 }, 1)
      if u then -- usable: a mark, and its reuse time while it recovers
        local tag = u.remaining > 0 and T.short(u.remaining) or (u.combat and "combat" or "use")
        T.font(16)
        local tw = ui.CalcTextSize(tag) + T.px(20)
        local tx = rx1 - T.px(480) - tw
        T.rect(tx, ry0 + T.px(12), tx + tw, ry1 - T.px(12), u.remaining > 0 and C.sunken or C.accent, u.remaining > 0 and 1 or 0.25, T.px(8))
        T.textIn(tx, ry0, tx + tw, ry1, tag, u.remaining > 0 and C.endurance or C.text, 0.5)
        T.unfont()
      end
      if focused then
        if W.pressed("x") then
          local row = i
          AS.start(name, function() return API.cursor.PickUp(SW, "SkillList", row) end, "Skills")
        end
      end
    end, function(i)
      if not settled then return end
      local texts = API.windows.ListRow(SW, "SkillList", i - 1)
      API.windows.ListClick(SW, "SkillList", i - 1, "left")
      if texts and usable[texts[1] or ""] then API.cursor.PressButtonFrom(SW, "SkillList", i) end
    end)
  end
  if W.finish() then API.windows.Show(SW, false) end
end)
