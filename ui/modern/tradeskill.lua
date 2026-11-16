-- Tradeskills: replaces the game's TradeskillWnd, the recipe window a tradeskill container opens in. At the left
-- the recipe search (name, trivial range) and favourites, and the matching recipes as rows with their trivial
-- (in its colour; sortable); at the right the container, the selected recipe, the components it takes (each with
-- what you carry of it: green with enough, red without), how many you can make, the combine buttons (one, a count,
-- or all; each result is put away before the next, and it stops when the inventory is full), Auto-Inventory and
-- Experiment (the container's own window). The fields are the window's edit boxes (Search sends what they hold),
-- a row click selects in its list (the server sends the components); ComponentList, MakeableLabel and the
-- Combine* children are Hailstone's (src/app/lua_ui_items.cpp).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "TradeskillWnd"
local LIST = "RecipeList"
local SIDE_W = 230   -- the container side (design pixels)
local STACK_W = 520  -- narrower than this, the container side goes under the recipes

local sortBy = 1 -- 1 the server's order, 2 name, 3 trivial
local SORTS = { "Server order", "Name", "Trivial" }

-- Names in lower case for the name sort, kept (a list's names come back as the same strings each frame).
local lowered, nLowered = {}, 0
local function lowerOf(text)
  local l = lowered[text]
  if not l then
    if nLowered >= 2048 then lowered, nLowered = {}, 0 end
    l = text:lower()
    lowered[text], nLowered = l, nLowered + 1
  end
  return l
end

local function testOpen()
  -- testing (test.open.TradeskillWnd=1): a right click on the first carried tradeskill bag the game opens here
  for slot = 23, 32 do
    local it = API.inventory.Item(slot)
    if it and (it.bagSlots or 0) > 0 and (it.name or ""):find("Mixing Bowl", 1, true) then
      API.inventory.ClickSlot(slot, nil, "right")
      return
    end
  end
end

-- (the controls' options: made once, their sizes set each frame)
local NUMBER_EDIT = { MinTrivialEdit = { hint = "any" }, MaxTrivialEdit = { hint = "any" } }
local SEARCH_EDIT = { hint = "Recipe name contains...", width = -1,
  onCommit = function() if ui.IsKeyPressed(ui.Key.Enter) then API.windows.ClickChild(WINDOW, "SearchButton", "left") end end }
local SEARCH_BUTTON = { primary = true, width = -1,
  tooltip = "Find the recipes you know that match (Enter in the name field does too)" }
local SHOW_FAVORITES = { tooltip = "List your favourite recipes" }
local ADD_FAVORITE = { tooltip = "Add the selected recipe to your favourites" }
local REMOVE_FAVORITE = { tooltip = "Remove the selected recipe from your favourites" }

-- A labelled number field for an edit child (the trivial range).
local function numberField(id, caption, width, s)
  local x, y = ui.GetCursorScreenPos()
  T.Text(x, y + 4 * s, caption, C.muted, T.font.small)
  ui.SetCursorScreenPos(x + T.TextSize(caption, T.font.small) + 6 * s, y)
  local opts = NUMBER_EDIT[id]
  opts.width = width
  W.Edit(WINDOW, id, opts)
end

local function search(w, s)
  local _, lh = ui.CalcTextSize("Ag")
  W.Edit(WINDOW, "SearchTextEdit", SEARCH_EDIT)
  local fieldW = 44 * s
  numberField("MinTrivialEdit", "Trivial from", fieldW, s)
  ui.SameLine(0, 8 * s)
  numberField("MaxTrivialEdit", "to", fieldW, s)
  ui.SameLine(0, 8 * s)
  W.Button(WINDOW, "SearchButton", "Search", SEARCH_BUTTON)
  -- favourites
  local bw = (w - 16 * s) / 3
  SHOW_FAVORITES.width, SHOW_FAVORITES.height = bw, lh + 8 * s
  W.Button(WINDOW, "ShowFavoritesButton", "Favorites", SHOW_FAVORITES)
  ui.SameLine(0, 8 * s)
  ADD_FAVORITE.width, ADD_FAVORITE.height = bw, lh + 8 * s
  W.Button(WINDOW, "AddFavoriteButton", "+ Favorite", ADD_FAVORITE)
  ui.SameLine(0, 8 * s)
  REMOVE_FAVORITE.width, REMOVE_FAVORITE.height = bw, lh + 8 * s
  W.Button(WINDOW, "RemoveFavoriteButton", "- Favorite", REMOVE_FAVORITE)
end

-- a tie keeps the list's order: table.sort is not stable and the rows are sorted again every frame, so two rows of
-- one name would otherwise swap places from frame to frame
local function byName(a, b)
  local na, nb = lowerOf(a.texts[1] or ""), lowerOf(b.texts[1] or "")
  if na ~= nb then return na < nb end
  return a.row < b.row
end
local function byTrivial(a, b)
  local ta, tb = tonumber(a.texts[2]) or 0, tonumber(b.texts[2]) or 0
  if ta ~= tb then return ta < tb end
  return a.row < b.row
end
local SORT_BY = { false, byName, byTrivial }

local trivialColor = { 0, 0, 0, 1 } -- a row's trivial colour, filled for each row (T.Text does not keep it)
local ROWS = {
  rowH = 24,
  draw = function(_, texts, x0, y0, x1, y1, s_, state)
    local _, th = T.TextSize("Ag", T.font.small)
    local ty = (y0 + y1 - th) / 2
    local nx = x0 + 8 * s_
    T.Text(nx, ty, T.Fit(texts[1] or "", x1 - nx - 50 * s_, T.font.small), C.text, T.font.small)
    -- the trivial in the game's colour for trivial - skill (how hard the recipe is for the character)
    local tc = state.colors and state.colors[2] -- (the row's cell colours, from P.Rows' read of it)
    if tc then trivialColor[1], trivialColor[2], trivialColor[3] = tc[1], tc[2], tc[3] end
    T.Text(x1 - 8 * s_, ty, texts[2] or "", tc and trivialColor or C.muted, T.font.small, "right")
  end,
  tooltip = function(_, texts) return (texts[1] or "") .. "\nTrivial at skill " .. (texts[2] or "?") end,
  empty = "No recipes listed",
  emptySub = "Search above, or show your favourites",
}

local function recipes(s)
  local x, y = ui.GetCursorScreenPos()
  T.Text(x, y + 4 * s, "Recipes", C.muted, T.font.small)
  local comboW = 120 * s
  local avail = ui.GetContentRegionAvail()
  ui.SetCursorScreenPos(x + avail - comboW, y)
  ui.SetNextItemWidth(comboW)
  if ui.BeginCombo("##tssort", SORTS[sortBy]) then
    for i, v in ipairs(SORTS) do if ui.Selectable(v, sortBy == i) then sortBy = i end end
    ui.EndCombo()
  end
  ROWS.sort = SORT_BY[sortBy] or nil
  P.Rows(WINDOW, LIST, 0, 0, ROWS)
end

local COMBINE_ONE = { primary = true,
  tooltip = "Make the selected recipe once from the components you carry (the result is put away)" }
local COMBINE_ALL = { primary = true, tooltip = "Combine until the components run out or your inventory is full" }
local COUNT_EDIT = { hint = "1" }
local COMBINE_COUNT = { tooltip = "Combine this many times (it stops early when the "
  .. "components run out or your inventory is full)" }
local COMBINE_STOP = { danger = true, tooltip = "Stop combining after the combine under way" }
local AUTO_INV = { tooltip = "Put what is on your cursor into your inventory" }
local EXPERIMENT = { tooltip = "Open the container itself, to try combinations you do not know" }
local function sized(opts, w, h)
  opts.width, opts.height = w, h
  return opts
end

-- The container, the recipe picked and its components, and the actions.
local function container(x, y, w, h, s)
  ui.DrawRectFilled(x, y, x + w, y + h, C.ink[1], C.ink[2], C.ink[3], 0.025, 8 * s)
  local pad = 10 * s
  local ix, iw = x + pad, w - 2 * pad
  local yy = y + pad
  -- the container
  local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, "ContainerIcon", "decal")
  if type(tex) ~= "userdata" then tex = nil end
  T.Icon(ix, yy, 36 * s, s, tex, u0, v0, u1, v1)
  T.Text(ix + 44 * s, yy + 2 * s, "CONTAINER", C.faint, T.font.tiny)
  T.Text(ix + 44 * s, yy + 16 * s, T.Fit(W.Text(WINDOW, "ContainerName"), iw - 44 * s, T.font.small), C.text,
    T.font.small)
  yy = yy + 46 * s
  -- the container's tradeskill and the character's value in it (the game's skill label, "Baking (52)")
  T.Text(ix, yy, T.Fit(W.Text(WINDOW, "SkillLabel"), iw, T.font.tiny), C.muted, T.font.tiny)
  yy = yy + 16 * s
  -- the recipe picked: the list's selected row
  local name, trivial
  local _, _, selected = API.windows.ListInfo(WINDOW, LIST)
  if selected and selected >= 0 then
    local row = API.windows.ListRow(WINDOW, LIST, selected)
    if row then name, trivial = row[1], row[2] end
  end
  ui.DrawLine(ix, yy, ix + iw, yy, C.ink[1], C.ink[2], C.ink[3], 0.06, 1)
  yy = yy + 8 * s
  if name then
    T.Text(ix, yy, T.Fit(name, iw, T.font.normal), C.text, T.font.normal)
    T.Hit("##tsrecipe", ix, yy, ix + iw, yy + 18 * s)
    if ui.IsItemHovered() then ui.SetTooltip(name) end
    yy = yy + 20 * s
    T.Text(ix, yy, "Trivial at " .. trivial, C.muted, T.font.tiny)
    yy = yy + 18 * s
  else
    T.Text(ix, yy, "No recipe selected", C.faint, T.font.small)
    yy = yy + 22 * s
  end
  -- components: one row each, what you carry of it against what it takes
  T.Text(ix, yy, "COMPONENTS", C.faint, T.font.tiny)
  local make = W.Text(WINDOW, "MakeableLabel")
  if make ~= "" then
    local ok = make ~= "0"
    T.Text(ix + iw, yy, "CAN MAKE " .. make:upper(), ok and C.good or C.danger, T.font.tiny, "right")
  end
  yy = yy + 16 * s
  local rows = API.windows.ListInfo(WINDOW, "ComponentList") or 0
  local size, gap = 28 * s, 4 * s
  for i = 0, rows - 1 do
    local texts, r, g, b, ctex, cu0, cv0, cu1, cv1 = API.windows.ListRow(WINDOW, "ComponentList", i)
    if texts then
      local col = { r, g, b, 1 }
      if type(ctex) ~= "userdata" then ctex = nil end
      T.Icon(ix, yy, size, s, ctex, cu0, cv0, cu1, cv1, col)
      local _, th = T.TextSize("Ag", T.font.small)
      local ty = yy + (size - th) / 2
      local countW = T.TextSize(texts[2] or "", T.font.small) + 6 * s
      T.Text(ix + size + 8 * s, ty, T.Fit(texts[1] or "", iw - size - 8 * s - countW, T.font.small), C.text, T.font.small)
      T.Text(ix + iw, ty, texts[2] or "", col, T.font.small, "right")
      T.Hit("##tscomp" .. i, ix, yy, ix + iw, yy + size)
      if ui.IsItemHovered() then ui.SetTooltip((texts[1] or "") .. "\nYou carry " .. (texts[2] or "")) end
      yy = yy + size + gap
    end
  end
  if rows == 0 then
    T.Text(ix, yy, name and "Waiting for the components..." or "Pick a recipe to see what it takes", C.faint,
      T.font.small)
    yy = yy + 20 * s
  end
  -- actions, at the bottom
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local bgap = 6 * s
  local progress = W.Text(WINDOW, "CombineProgressLabel")
  local by = y + h - pad - 4 * btnH - 3 * bgap
  if by < yy then by = yy end
  local half = (iw - bgap) / 2
  ui.SetCursorScreenPos(ix, by)
  sized(COMBINE_ONE, half, btnH)
  W.Button(WINDOW, "CombineOneButton", "Combine", COMBINE_ONE)
  ui.SameLine(0, bgap)
  W.Button(WINDOW, "CombineAllButton", "Combine All", sized(COMBINE_ALL, half, btnH))
  ui.SetCursorScreenPos(ix, by + btnH + bgap)
  local editW = 48 * s
  COUNT_EDIT.width = editW
  W.Edit(WINDOW, "CombineCountEdit", COUNT_EDIT)
  ui.SameLine(0, bgap)
  local countText = W.Text(WINDOW, "CombineCountEdit")
  W.Button(WINDOW, "CombineCountButton", "Combine " .. (tonumber(countText) and countText or "?"),
    sized(COMBINE_COUNT, iw - editW - bgap, btnH))
  ui.SetCursorScreenPos(ix, by + 2 * (btnH + bgap))
  if progress ~= "" then
    W.Button(WINDOW, "CombineStopButton", "Stop  (" .. progress .. ")", sized(COMBINE_STOP, iw, btnH))
  else
    W.Button(WINDOW, "AutoInvButton", "Auto-Inventory", sized(AUTO_INV, iw, btnH))
  end
  ui.SetCursorScreenPos(ix, by + 3 * (btnH + bgap))
  W.Button(WINDOW, "ExperimentButton", "Experiment", sized(EXPERIMENT, iw, btnH))
end

local TEST_SEARCH = WINDOW .. ".search"
local function testSearch() API.windows.ClickChild(WINDOW, "SearchButton", "left") end

local DIALOG = {
  size = function() return 640, 560 end,
  resizable = { 320, 480 },
}

local function body(x, y, w, h, s)
  local stacked = w < STACK_W * s
  local side = stacked and w or math.min(SIDE_W * s, w * 0.45)
  local leftW = stacked and w or (w - side - 10 * s)
  local leftH = stacked and (h * 0.55) or h
  ui.BeginChild("##tsleft", leftW, leftH, false)
  search(leftW, s)
  ui.Dummy(0, 2 * s)
  recipes(s)
  ui.EndChild()
  local cx, cy = stacked and x or (x + leftW + 10 * s), stacked and (y + leftH + 8 * s) or y
  container(cx, cy, side, y + h - cy, s)
  ui.SetCursorScreenPos(x, y)
  ui.Dummy(w, h)
end

ui.window("Tradeskills", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  P.TestOpen(TEST_SEARCH, testSearch)
  hud.Dialog(WINDOW, "Tradeskills", DIALOG, body)
end)
