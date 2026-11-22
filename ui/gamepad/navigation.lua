-- The zone guide's windows (gamepad pack), standing in for the stock windows and answered through them
-- (lib/model.lua, app/lua_ui_zoneguide.cpp):
--   Find (hs.FindNpcs, Hailstone's Find window in place of the game's FindLocationWnd; app/lua_ui_find.cpp)
--                                the zone's merchants, bankers, guildmasters, other services and points of interest,
--                                nearest first, one kind or all: A on one targets it and starts the path to it (the
--                                window then closes, as the game's find window did); X walks there (the navigator)
--   Zone Path (ZonePathWnd)      the steps of the zone guide's active path (the game refuses to show it without one)
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local C = T.C

-- ---------------------------------------------------------------- find (Hailstone's Find window, hs.FindNpcs)
local findList = { offset = 0 }
local findKind = 0 -- 0 all, else the index into hs.FindCategories()
local CATS = API.find.Categories and API.find.Categories() or {}
local findPolicy, findServerOnly, findPolicyName, findPolicyCustom, findPolicyStock
local findRevision, findContext, findZone = nil, nil, nil
local pathHints = { { "a", "Show the path" }, { "b", "Close" } }
local walkHints = { { "a", "Show the path" }, { "x", "Walk there" }, { "b", "Close" } }
local function npcFindPolicy()
  local query = API.map.Policy
  local policy = type(query) == "function" and query() or nil
  local allowed = type(policy) ~= "table" or policy.npc_find ~= false
  local serverOnly = type(policy) == "table" and policy.npc_find_server_only == true
  local name = type(policy) == "table" and policy.name or ""
  local custom = type(policy) == "table" and policy.custom_spawns == true
  local stock = type(policy) ~= "table" or policy.stock_npcs ~= false
  local revision = type(policy) == "table" and policy.revision or nil
  local context = type(policy) == "table" and policy.context or nil
  local zone = select(2, API.map.ZoneName())
  if findPolicy ~= allowed or findPolicyName ~= name or findPolicyCustom ~= custom or findPolicyStock ~= stock
      or findServerOnly ~= serverOnly
      or findRevision ~= revision or findContext ~= context or findZone ~= zone or not allowed then
    findList = { offset = 0 }
    findPolicy, findPolicyName, findPolicyCustom, findPolicyStock = allowed, name, custom, stock
    findServerOnly = serverOnly
    findRevision, findContext, findZone = revision, context, zone
  end
  return allowed, serverOnly
end

ui.window("Find", function()
  if not API.session.InGame() or not API.find.Open() then return end -- (before the policy: shut, it costs nothing)
  local allowed, serverOnly = npcFindPolicy()
  if not allowed then return end
  T.frame()
  if W.begin("Find", { w = 1200, h = 860, title = "Find",
        subtitle = serverOnly and "NPCs the server marks as findable" or "Merchants, bankers, trainers and more in this zone",
        hints = serverOnly and pathHints or walkHints }) then
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
    for _, r in ipairs(API.find.Npcs()) do
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
        local row = rows[i]
        if API.find.NpcTarget(row.id, row.token) ~= false and API.find.NpcPath(row.id, row.token) ~= false then
          API.find.Open(false)
        end
      end)
      -- X: walk there (the navigator; any movement stops it), the window closing
      if not serverOnly and focused and W.pressed("x") then
        local row = rows[focused]
        if API.find.NpcTarget(row.id, row.token) ~= false and API.find.NpcWalk(row.id, row.token) then API.find.Open(false) end
      end
    end
  end
  if W.finish() then API.find.Open(false) end
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
  if W.finish() then API.windows.Show(ZW, false) end
end)
