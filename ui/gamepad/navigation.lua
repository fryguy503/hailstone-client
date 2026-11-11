-- The zone guide's windows (gamepad pack), standing in for the stock windows and answered through them
-- (lib/model.lua, app/lua_eq_zoneguide.cpp):
--   Find (eq.FindNpcs, Hailstone's Find window in place of the game's FindLocationWnd; app/lua_eq_find.cpp)
--                                the zone's merchants, bankers, guildmasters, other services and points of interest,
--                                nearest first, one kind or all: A on one targets it and starts the path to it (the
--                                window then closes, as the game's find window did); X walks there (the navigator)
--   Zone Path (ZonePathWnd)      the steps of the zone guide's active path (the game refuses to show it without one)
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local C = T.C
local managed = eq.ManagedDistribution and eq.ManagedDistribution() or false
local findHints = { { "a", "Show the path" }, { "b", "Close" } }
if not managed then table.insert(findHints, 2, { "x", "Walk there" }) end

-- ---------------------------------------------------------------- find (Hailstone's Find window, eq.FindNpcs)
local findList = { offset = 0 }
local findKind = 0 -- 0 all, else the index into eq.FindCategories()
local CATS = eq.FindCategories and eq.FindCategories() or {}

ui.window("Find", function()
  if not eq.InGame() or not eq.FindOpen() then return end
  T.frame()
  if W.begin("Find", { w = 1200, h = 860, title = "Find", subtitle = "Merchants, bankers, trainers and more in this zone",
        hints = findHints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(76)
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local kind = CATS[findKind]
    if T.button("##findkind", "Showing: " .. (kind and kind[2] or "Everything"), x1 - x0, bh, T.NO_OPTS, pad) then
      findKind = (findKind + 1) % (#CATS + 1)
      findList.offset = 0
    end
    local rows = {}
    for _, r in ipairs(eq.FindNpcs()) do
      if not kind or r.category == kind[1] then rows[#rows + 1] = r end
    end
    local top = y0 + bh + T.px(18)
    ui.SetCursorPos(x0 - wx, top - wy)
    if #rows == 0 then
      T.font(24)
      T.draw(x0, top + T.px(10), "Nothing to find here.", C.dim)
      T.unfont()
    else
      local rowH = 70
      local visible = math.max(1, math.floor((y1 - top + T.px(10)) / T.px(rowH + 10)))
      local focused = W.list("find", findList, #rows, visible, rowH, x1 - x0, function(i, rx0, ry0, rx1, ry1)
        local r = rows[i]
        local _, th = ui.CalcTextSize("Ag")
        local mid = (ry0 + ry1) / 2
        T.textIn(rx0 + T.px(20), mid - th, rx1 - T.px(160), mid, r.name, C.text, 0)
        T.textIn(rx0 + T.px(20), mid, rx1 - T.px(160), mid + th, r.subtitle ~= "" and r.subtitle or r.label, C.dim, 0)
        T.textIn(rx1 - T.px(150), ry0, rx1 - T.px(20), ry1, string.format("%d away", math.floor(r.distance)), C.dim, 1)
      end, function(i)
        -- as the original's find window: the path to the row, and the window goes
        eq.FindNpcTarget(rows[i].id)
        eq.FindNpcPath(rows[i].id)
        eq.FindOpen(false)
      end)
      -- X: walk there (the navigator; any movement stops it), the window closing
      if not managed and focused and W.pressed("x") then
        eq.FindNpcTarget(rows[focused].id)
        if eq.FindNpcWalk(rows[focused].id) then eq.FindOpen(false) end
      end
    end
  end
  if W.finish() then eq.FindOpen(false) end
end)

-- ---------------------------------------------------------------- the zone path (ZonePathWnd)
local ZW = "ZonePathWnd"
local steps = {}

ui.window("Zone Path", function()
  if not W.shown(ZW) then return end
  T.frame()
  if W.begin("Zone Path", { w = 1000, h = 800, title = "Zone Path", subtitle = "The zones on the way",
        hints = { { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    M.list(ZW, "ZPW_ZoneList", steps, x0, y0, x1, y1, { empty = "No path is set." })
  end
  if W.finish() then eq.ShowWindow(ZW, false) end
end)
