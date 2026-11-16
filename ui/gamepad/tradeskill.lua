-- Tradeskills (gamepad pack): stands in for TradeskillWnd and for a tradeskill container's window (a sewing kit, a
-- mixing bowl, ...: the bag windows "Bag1".."Bag36" whose bag is a combine container; other bags keep their window).
--   Tradeskill  the container's recipes (the stock list: the search's or the favourites'); focusing one selects it
--               (its components show, each with what you carry of it: green with enough, red without, and how
--               many you can make), A combines it once (the server takes the components from the bags; the result
--               is put away). X searches by name (the on-screen keyboard), Y shows the favourites, LB experiments
--               (the container's own window); the buttons under the components also Combine All (until the
--               components run out or the inventory is full; Stop while it runs), add or remove the favourite and
--               Auto-Inventory.
--   Combine     the container's slots and your bags side by side: A picks up or puts down the focused item (the
--               game's slot logic), Y shows its details, X combines (the stock Combine button).
-- Everything acts through the stock windows (hs.ListClick, hs.ClickChild, hs.ClickSlot, hs.BagSlotClick).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local K = dofile("lib/keyfield.lua")
local C = T.C
local bagSlots = {} -- the bag slots' items, read once a frame (I.readBagSlots)

-- Keeps the D-pad's up / down in a W.list column: ImGui's navigation may jump to a nearer item of the next column
-- (the objectives, the message), so a focused row hands the focus to its neighbour itself. (The last shown row's
-- down and the first's up scroll the list: W.list does those.)
local function stayInColumn(state, i, count, visible)
  local v = i - (state.offset or 0)
  if pad.menuPressed("down") and i < count and v < visible then state.focusNext = v + 1 end
  if pad.menuPressed("up") and v > 1 then state.focusNext = v - 1 end
end

-- A pad shortcut of the current window, not in its first moments: the press that opened it (X uses a bag in the
-- inventory, A opens a tile) is not one for it.
local function pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 8 and W.pressed(b) -- (appeared: by frames, as W.begin keeps it)
end

-- ---------------------------------------------------------------- the recipe window (TradeskillWnd)
local TW = "TradeskillWnd"
local recipes = { offset = 0 }
local searching = false
local kb = K.new()
local wasShown = false

-- Search Recipes with the name typed; the pad's focus goes to the first recipe found.
local function search()
  API.windows.ClickChild(TW, "SearchButton", "left")
  searching, recipes.offset, recipes.lastFocus, recipes.focusNext = false, 0, nil, 1
end

ui.window("Tradeskill", function()
  if not W.shown(TW) then
    searching, wasShown, recipes.lastFocus = false, false, nil
    return
  end
  if not wasShown then -- opened: the list from the top (the game asks for the favourites)
    wasShown = true
    recipes.offset = 0
  end
  local name = API.windows.Text(TW, "ContainerName") or ""
  local hints = searching and { K.hints[1], K.hints[2], K.hints[3], K.hints[4], { "b", "Search" } }
    or { { "a", "Combine" }, { "x", "Search" }, { "y", "Favorites" }, { "lb", "Experiment" }, { "b", "Close" } }
  if W.begin("Tradeskill", { w = 1640, h = 960, title = name ~= "" and name or "Tradeskill",
        subtitle = API.windows.Text(TW, "SkillLabel") or "", hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local leftX1 = x0 + (x1 - x0) * 0.46
    local count, _, sel = API.windows.ListInfo(TW, "RecipeList")
    count = count or 0
    -- the recipes
    ui.SetCursorPos(x0 - wx, y0 - wy)
    T.heading(count == 1 and "1 recipe" or (count .. " recipes"))
    local top = select(2, ui.GetCursorScreenPos())
    local rowH = 60
    local shownRows = math.max(1, math.floor((y1 - top + T.px(10)) / T.px(rowH + 10)))
    W.list("recipes", recipes, count, shownRows, rowH, leftX1 - x0,
      function(i, rx0, ry0, rx1, ry1, focused)
        if focused then stayInColumn(recipes, i, count, shownRows) end
        local texts, r, g, b = API.windows.ListRow(TW, "RecipeList", i - 1)
        if not texts then return end
        local fav = b and b < 0.7 -- the stock list's favourite colour
        T.font(21)
        T.textIn(rx0 + T.px(16), ry0, rx1 - T.px(130), ry1, T.fit(texts[1] or "", rx1 - rx0 - T.px(150)), fav and { r, g, b, 1 } or C.text, 0)
        T.unfont()
        T.font(18)
        T.textIn(rx1 - T.px(130), ry0, rx1 - T.px(16), ry1, "Trivial " .. (texts[2] or ""), C.dim, 1)
        T.unfont()
        if focused and recipes.lastFocus ~= i then -- select it: the stock window asks for its components
          recipes.lastFocus = i
          API.windows.ListClick(TW, "RecipeList", i - 1, "left")
        end
      end,
      function(i)
        API.windows.ListClick(TW, "RecipeList", i - 1, "left")
        API.windows.ClickChild(TW, "CombineOneButton", "left")
      end)
    if count == 0 then
      T.font(20)
      T.draw(x0, top, "No recipes. Search (X) or show the favourites (Y).", C.faint)
      T.unfont()
    end
    local rx0 = leftX1 + T.px(24)
    if searching then
      -- the search: a name (part of it), then Search Recipes
      T.font(20)
      T.draw(rx0, y0, "SEARCH RECIPES", C.dim)
      T.unfont()
      ui.SetCursorPos(rx0 - wx, y0 + T.px(36) - wy)
      ui.SetNextItemWidth(x1 - rx0)
      local text = API.windows.Text(TW, "SearchTextEdit") or ""
      local t, entered = ui.InputText("##tsearch", text, 64, ui.InputTextFlags.EnterReturnsTrue, { moveToEnd = true })
      if t ~= text then API.windows.SetText(TW, "SearchTextEdit", t) end
      ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(10) - wy)
      local done = K.draw(kb, function() return API.windows.Text(TW, "SearchTextEdit") end,
        function(v) API.windows.SetText(TW, "SearchTextEdit", v) end, rx0, x1)
      if done == "done" or entered then search() end
    else
      -- the selected recipe and its components
      local texts = sel and sel >= 0 and API.windows.ListRow(TW, "RecipeList", sel) or nil
      T.font(26)
      local _, th = ui.CalcTextSize("Ag")
      T.draw(rx0, y0, T.fit(texts and texts[1] or "No recipe selected", x1 - rx0), texts and C.text or C.faint)
      T.unfont()
      local cy = y0 + th + T.px(18)
      T.font(18)
      T.draw(rx0, cy, "COMPONENTS", C.dim)
      local make = API.windows.Text(TW, "MakeableLabel") or ""
      if make ~= "" then
        local tw = ui.CalcTextSize("CAN MAKE " .. make:upper())
        T.draw(x1 - tw, cy, "CAN MAKE " .. make:upper(), make ~= "0" and C.good or C.bad)
      end
      T.unfont()
      cy = cy + T.px(32)
      local s, gap = T.px(64), T.px(10)
      local n = API.windows.ListInfo(TW, "ComponentList") or 0
      T.font(20)
      for k = 0, n - 1 do
        local ctexts, r, g, b, tex, u0, v0, u1, v1 = API.windows.ListRow(TW, "ComponentList", k)
        if ctexts then
          local col = k % 2
          local row = k // 2
          local ix = rx0 + col * (x1 - rx0) / 2
          local iy = cy + row * (s + gap)
          local cw = (x1 - rx0) / 2
          T.rect(ix, iy, ix + s, iy + s, C.raised, 1, T.px(10))
          if type(tex) == "userdata" then
            T.icon(tex, u0, v0, u1, v1, ix + T.px(6), iy + T.px(6), ix + s - T.px(6), iy + s - T.px(6))
          end
          T.textIn(ix + s + T.px(12), iy, ix + cw - T.px(100), iy + s,
            T.fit(ctexts[1] or "", cw - s - T.px(120)), C.text, 0)
          T.textIn(ix + cw - T.px(100), iy, ix + cw - T.px(12), iy + s, ctexts[2] or "", { r, g, b, 1 }, 1)
        end
      end
      if n == 0 then T.draw(rx0, cy, texts and "Asking for the components..." or "Focus a recipe to see what it needs.", C.faint) end
      T.unfont()
      -- the buttons
      local progress = API.windows.Text(TW, "CombineProgressLabel") or ""
      local all = progress ~= "" and { "Stop (" .. progress .. ")", "CombineStopButton" } or { "Combine All", "CombineAllButton", true }
      local buttons = { { "Combine", "CombineOneButton", true }, all, { "Add Favorite", "AddFavoriteButton" },
        { "Remove Favorite", "RemoveFavoriteButton" }, { "Experiment", "ExperimentButton" }, { "Auto-Inventory", "AutoInvButton" } }
      local bh = T.px(60)
      local bw = (x1 - rx0 - T.px(10)) / 2
      local by = y1 - 3 * bh - 2 * T.px(10)
      ui.SetCursorPos(rx0 - wx, by - wy)
      T.font(20)
      for i, b in ipairs(buttons) do
        if i % 2 == 0 then ui.SameLine(0, T.px(10)) else ui.SetCursorPos(rx0 - wx, select(2, ui.GetCursorScreenPos()) - wy) end
        local on = API.windows.ChildState(TW, b[2]) ~= false
        if T.button("##tb" .. b[2], b[1], bw, bh, { primary = b[3], disabled = not on }, pad) then API.windows.ClickChild(TW, b[2], "left") end
      end
      T.unfont()
      if pressed("Tradeskill", "x") then
        searching = true
        K.focus(kb)
      end
      if pressed("Tradeskill", "lb") then API.windows.ClickChild(TW, "ExperimentButton", "left") end
      if pressed("Tradeskill", "y") then
        API.windows.ClickChild(TW, "ShowFavoritesButton", "left")
        recipes.offset, recipes.lastFocus = 0, nil
      end
    end
  end
  if W.finish() then
    if searching then search() else API.windows.Show(TW, false) end
  end
end)

-- ---------------------------------------------------------------- a combine container (its bag window)
local bagState = { screen = nil }

-- The first open bag window whose bag is a combine container (looked for every frame: the names made once).
local BAG_SCREENS = {}
for n = 1, 36 do BAG_SCREENS[n] = "Bag" .. n end
local function combineBag()
  for n = 1, 36 do
    local screen = BAG_SCREENS[n]
    local name, slots, combine = API.inventory.BagInfo(screen)
    if not name then break end
    if combine then return screen, name, slots end
  end
end

ui.window("Combine", function()
  if not API.session.InGame() then return end
  local screen, name, slots = combineBag()
  if not screen or not W.shown(screen) then return end
  local held = API.inventory.Item(API.inventory.CursorSlot or 33)
  if W.begin("Combine", { w = 1500, h = 900, title = name, subtitle = slots .. " slots",
        hints = { { "a", held and "Put down" or "Pick up" }, { "x", "Combine" }, { "y", "Details" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local size, gap = T.px(84), T.px(10)
    local focusedItem, focusedWhere
    -- the container's slots
    ui.SetCursorPos(x0 - wx, y0 - wy)
    T.heading("In the " .. name)
    local per = 5
    ui.BeginGroup()
    for i = 0, slots - 1 do
      if i % per ~= 0 then ui.SameLine(0, gap) end
      local item = API.inventory.BagSlotItem(screen, i)
      local clicked, focused = T.slot("##cs" .. i, item, size, { label = tostring(i + 1) }, pad)
      if focused then
        focusedItem, focusedWhere = item, name .. ", slot " .. (i + 1)
        if pressed("Combine", "y") and item then API.inventory.BagSlotClick(screen, i, "rightheld") end
      end
      if clicked then API.inventory.BagSlotClick(screen, i, "left") end
    end
    ui.EndGroup()
    ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(16) - wy)
    T.font(22)
    if T.button("##combine", "Combine", per * size + (per - 1) * gap, T.px(64), { primary = true, glyph = "x" }, pad) then
      API.windows.ClickChild(screen, "Container_Combine", "left")
    end
    T.unfont()
    if pressed("Combine", "x") then API.windows.ClickChild(screen, "Container_Combine", "left") end
    -- your bags: the items to put in (the combine bag's own slots are above)
    local bx0 = x0 + per * (size + gap) + T.px(40)
    local listX1 = x1 - T.px(400)
    ui.SetCursorPos(bx0 - wx, y0 - wy)
    ui.BeginGroup()
    T.heading("Your bags")
    local bper = math.max(1, math.floor((listX1 - bx0 + gap) / (size + gap)))
    local n = 0
    local bagItem = API.inventory.BagSlotItem(screen, -1)
    local acted = false -- (a click may have moved an item: the bag slots are read again after it)
    local function cell(slot, bag, item, where)
      if n % bper ~= 0 then ui.SameLine(0, gap) end
      n = n + 1
      local clicked, focused = T.slot(I.cellId("inv", slot, bag), item, size, { label = where }, pad)
      if focused then
        focusedItem, focusedWhere = item, where
        if pressed("Combine", "y") and item then API.inventory.ClickSlot(slot, bag, "rightheld") end
      end
      if clicked then
        API.inventory.ClickSlot(slot, bag, "left")
        acted = true
      end
    end
    I.readBagSlots(bagSlots)
    for slot = 23, 32 do
      local it = bagSlots[slot]
      if not it or (it.bagSlots or 0) == 0 then cell(slot, nil, it, API.inventory.SlotName(slot)) end
    end
    for slot = 23, 32 do
      local bag = acted and API.inventory.Item(slot) or bagSlots[slot]
      if bag and (bag.bagSlots or 0) > 0 and not (bagItem and bag.id == bagItem.id and bag.name == bagItem.name) then
        for i = 0, bag.bagSlots - 1 do cell(slot, i, API.inventory.Item(slot, i), bag.name) end
      end
    end
    ui.EndGroup()
    -- the focused item and what is held
    local holdH = held and T.px(110) or 0
    I.summary(focusedItem, focusedWhere, listX1 + T.px(24), y0, x1, y1 - holdH - (held and T.px(12) or 0))
    if held then
      local hy = y1 - holdH
      T.rect(listX1 + T.px(24), hy, x1, y1, C.sunken, 1, T.px(14))
      T.itemIcon(held.icon, listX1 + T.px(40), hy + T.px(16), listX1 + T.px(40) + T.px(76), hy + T.px(92))
      T.font(18)
      T.draw(listX1 + T.px(130), hy + T.px(20), "HOLDING", C.dim)
      T.unfont()
      T.font(21)
      T.draw(listX1 + T.px(130), hy + T.px(50), T.fit(held.name or "", x1 - listX1 - T.px(150)))
      T.unfont()
    end
  end
  if W.finish() then API.windows.Show(screen, false) end
end)
