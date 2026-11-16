-- Tribute (gamepad pack): stands in for TributeMasterWnd (a tribute master's window) and TributeBenefitWnd.
--   Tribute Master  two pages, LB / RB to switch:
--                   Benefits  the available benefits and your chosen ones; focusing one selects it (its description
--                             shows). Modify Benefits (Back) starts changing them, X upgrades the selected one (adds an
--                             available benefit, raises a chosen one's tier), Y downgrades it, and Save Benefits sends
--                             the change; the status button turns tribute on or off.
--                   Donate    your bags: A picks up an item (the game's slot logic), Donate gives the held item, or
--                             the platinum chosen with the Platinum button (the stock quantity dialog).
--   Tribute         the Personal and Guild pages (LB / RB): the active benefits, a focused one's description, the
--                   favour, cost and time left, Activate / Deactivate (and the guild's Opt In / Out).
-- The lists, texts and buttons are the stock windows' (hs.ListRow, hs.WindowText, hs.ChildState, hs.ClickChild); the
-- favour labels are their EQTypes (hs.Label).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local P = dofile("lib/textpanel.lua")
local C = T.C
local bagSlots = {} -- the bag slots' items, read once a frame (I.readBagSlots)

-- A pad shortcut of the current window, not in its first moments: the press that opened it is not one for it.
local function pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 8 and W.pressed(b) -- (appeared: by frames, as W.begin keeps it)
end

local function tabs(labels, page, x0, y0, set)
  local x = x0
  T.font(22)
  local _, th = ui.CalcTextSize("Ag")
  local h = th + T.px(16)
  for i, l in ipairs(labels) do
    local tw = ui.CalcTextSize(l)
    local w = tw + T.px(40)
    T.rect(x, y0, x + w, y0 + h, page == i and C.accent or C.raised, page == i and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + h, l, page == i and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + h) and ui.IsMouseClicked(ui.Mouse.Left) then set(i) end
    x = x + w + T.px(10)
  end
  T.unfont()
  return y0 + h + T.px(16)
end

-- Label / value pairs in a row from x0 to x1 at y.
local function facts(list, x0, x1, y)
  local w = (x1 - x0) / #list
  for i, f in ipairs(list) do
    local fx = x0 + (i - 1) * w
    T.font(17)
    T.draw(fx, y, f[1]:upper(), C.dim)
    local _, lh = ui.CalcTextSize("Ag")
    T.unfont()
    T.font(24)
    T.draw(fx, y + lh + T.px(2), f[2] or "", f[3] or C.text)
    T.unfont()
  end
end

-- A benefit list of `screen` at the cursor: one line a row (the stock list's first column, then its others dimmed).
-- Focusing a row selects it (the stock list's click: its description). Returns the focused row.
local function benefits(screen, child, state, visible, width)
  local rows = API.windows.ListInfo(screen, child) or 0
  ui.BeginGroup() -- (the rows start at this x)
  local f = W.list(child, state, rows, visible, 54, width, function(i, rx0, ry0, rx1, ry1, focused)
    local texts = API.windows.ListRow(screen, child, i - 1)
    if not texts then return end
    local extra = {}
    for k = 2, #texts do if (texts[k] or "") ~= "" then extra[#extra + 1] = texts[k] end end
    local ew = #extra > 0 and T.px(170) or 0
    T.font(20)
    T.textIn(rx0 + T.px(14), ry0, rx1 - ew - T.px(14), ry1, T.fit(texts[1] or "", rx1 - rx0 - ew - T.px(28)),
      texts[1] == "(none)" and C.faint or C.text, 0)
    T.unfont()
    if #extra > 0 then
      T.font(17)
      T.textIn(rx1 - ew, ry0, rx1 - T.px(14), ry1, (#extra == 2 and ("Cost " .. extra[1] .. "  Lvl " .. extra[2])) or table.concat(extra, "  "), C.dim, 1)
      T.unfont()
    end
    if focused and state.lastFocus ~= i then
      state.lastFocus = i
      API.windows.ListClick(screen, child, i - 1, "left")
    end
  end, function(i) API.windows.ListClick(screen, child, i - 1, "left") end)
  ui.EndGroup()
  if rows == 0 then
    local x, y = ui.GetCursorScreenPos()
    T.draw(x, y, "None.", C.faint)
  end
  return f
end

-- A tribute description: the server's text, whose "<br>" line breaks the window's text carries escaped.
local function described(text) return ((text or ""):gsub("&lt;[Bb][Rr]&gt;", "<BR>")) end

local function fits(height, rowH) return math.max(1, math.floor((height + T.px(10)) / T.px(rowH + 10))) end

-- A row of stock buttons (label, child, primary) at the cursor from x0 to x1.
local function buttons(screen, list, x0, x1, h)
  local wx, wy = ui.GetWindowPos()
  local gap = T.px(10)
  local bw = (x1 - x0 - gap * (#list - 1)) / #list
  ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
  T.font(20)
  for i, b in ipairs(list) do
    if i > 1 then ui.SameLine(0, gap) end
    local text = API.windows.Text(screen, b[2])
    local on = API.windows.ChildState(screen, b[2]) ~= false
    if T.button("##" .. screen .. b[2], (text and text ~= "" and b.useText) and text or b[1], bw, h,
          { primary = b[3], disabled = not on }, pad) then
      API.windows.ClickChild(screen, b[2], "left")
    end
  end
  T.unfont()
end

-- ---------------------------------------------------------------- the tribute master (TributeMasterWnd)
local MW = "TributeMasterWnd"
local page = 1
local avail, active = { offset = 0 }, { offset = 0 }
local desc = { scroll = 0 }

ui.window("Tribute Master", function()
  if not W.shown(MW) then return end
  local title = API.windows.Title(MW) or ""
  local lock = API.windows.Text(MW, "TMW_LockButton") or ""
  local hints = page == 1 and { { "back", lock ~= "" and lock or "Modify Benefits" }, { "x", "Upgrade" }, { "y", "Downgrade" },
    { "lb", "Benefits" }, { "rb", "Donate" }, { "b", "Close" } }
    or { { "a", "Pick up / put down" }, { "lb", "Benefits" }, { "rb", "Donate" }, { "b", "Close" } }
  if W.begin("Tribute Master", { w = 1680, h = 980, title = title ~= "" and title or "Tribute Master",
        subtitle = "Tribute is " .. (API.windows.Text(MW, "TMW_StatusText") or ""), hints = hints }) then
    if pressed("Tribute Master", "lb") then page = 1 end
    if pressed("Tribute Master", "rb") then page = 2 end
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local top = tabs({ "Benefits", "Donate" }, page, x0, y0, function(i) page = i end)
    local guild = (API.windows.Text(MW, "TMW_GuildCurrentPointsLabel") or "") ~= ""
    -- the favour and the cost, as the stock window's labels show them
    if guild then
      facts({ { "Guild favor", API.windows.Text(MW, "TMW_GuildCurrentPointsLabel"), C.endurance },
        { "Cost", (API.windows.Text(MW, "TMW_GuildCostLabel")) }, { "Time left", (API.windows.Text(MW, "TMW_GuildTimeLabel")) } },
        x0 + (x1 - x0) * 0.45, x1, y0 - T.px(6))
    else
      facts({ { "Favor", (API.character.Label(117)), C.endurance }, { "Career", (API.character.Label(118)) }, { "Cost", (API.character.Label(119)) },
        { "Time left", (API.character.Label(116)) } }, x0 + (x1 - x0) * 0.40, x1, y0 - T.px(6))
    end
    local midX = x0 + (x1 - x0) * 0.42
    if page == 1 then
      ui.SetCursorPos(x0 - wx, top - wy)
      T.heading("Available")
      local ly = select(2, ui.GetCursorScreenPos())
      benefits(MW, "TMW_AvailableBenefitList", avail, fits(y1 - ly, 54), midX - x0)
      local rx0 = midX + T.px(24)
      ui.SetCursorPos(rx0 - wx, top - wy)
      ui.BeginGroup()
      T.heading("Chosen")
      benefits(MW, "TMW_ActiveBenefitList", active, 5, x1 - rx0)
      ui.EndGroup()
      local dy = select(2, ui.GetCursorScreenPos()) + T.px(10)
      local bh = T.px(58)
      local by = y1 - 2 * bh - T.px(10)
      P.draw("##tmdesc", desc, described(API.windows.Text(MW, "TMW_Description")), rx0, dy, x1, by - T.px(14),
        { empty = "Focus a benefit to read about it." })
      ui.SetCursorPos(rx0 - wx, by - wy)
      buttons(MW, { { "Modify Benefits", "TMW_LockButton", true, useText = true }, { "Upgrade", "TMW_UpgradeButton" },
        { "Downgrade", "TMW_DowngradeButton" } }, rx0, x1, bh)
      local _, st = API.windows.ChildState(MW, "TMW_StatusButton")
      buttons(MW, { { st and "Turn Tribute Off" or "Turn Tribute On", "TMW_StatusButton" } }, rx0, x1, bh)
      if pressed("Tribute Master", "back") then API.windows.ClickChild(MW, "TMW_LockButton", "left") end
      if pressed("Tribute Master", "x") then API.windows.ClickChild(MW, "TMW_UpgradeButton", "left") end
      if pressed("Tribute Master", "y") then API.windows.ClickChild(MW, "TMW_DowngradeButton", "left") end
    else
      -- your bags: pick up the item to donate
      local size, gap = T.px(84), T.px(8)
      local listX1 = x1 - T.px(460)
      ui.SetCursorPos(x0 - wx, top - wy)
      ui.BeginGroup()
      T.heading("Your bags")
      local per = math.max(1, math.floor((listX1 - x0 + gap) / (size + gap)))
      local n = 0
      local focusedItem, focusedWhere
      local acted = false -- (a click may have moved an item: the bag slots are read again after it)
      local function cell(slot, bag, item, where)
        if n % per ~= 0 then ui.SameLine(0, gap) end
        n = n + 1
        local clicked, focused = T.slot(I.cellId("tm", slot, bag), item, size, { label = where }, pad)
        if focused then
          focusedItem, focusedWhere = item, where
          if pressed("Tribute Master", "y") and item then API.inventory.ClickSlot(slot, bag, "rightheld") end
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
        if bag and (bag.bagSlots or 0) > 0 then
          for i = 0, bag.bagSlots - 1 do cell(slot, i, API.inventory.Item(slot, i), bag.name) end
        end
      end
      ui.EndGroup()
      -- the right column: the held item, its tribute value, Donate and Platinum
      local rx0 = listX1 + T.px(24)
      local held = API.inventory.Item(API.inventory.CursorSlot or 33)
      I.summary(held or focusedItem, held and "Holding" or focusedWhere, rx0, top, x1, top + T.px(330))
      facts({ { "Tribute value", API.windows.Text(MW, "TMW_ValueLabel"), C.endurance },
        { "Platinum", (API.windows.Text(MW, "TMW_CoinButton")) } }, rx0, x1, top + T.px(350))
      ui.SetCursorPos(rx0 - wx, y1 - 2 * T.px(64) - T.px(10) - wy)
      buttons(MW, { { held and "Donate the held item" or "Donate", "TMW_DonateButton", true } }, rx0, x1, T.px(64))
      buttons(MW, { { "Platinum...", "TMW_CoinButton" } }, rx0, x1, T.px(64))
    end
  end
  if W.finish() then API.windows.Show(MW, false) end
end)

-- ---------------------------------------------------------------- the benefits window (TributeBenefitWnd)
local BW = "TributeBenefitWnd"
local bpage = 1
local mine, guildList = { offset = 0 }, { offset = 0 }
local bdesc, gdesc = { scroll = 0 }, { scroll = 0 }

ui.window("Tribute", function()
  if not W.shown(BW) then return end
  if W.begin("Tribute", { w = 1400, h = 860, title = "Tribute",
        hints = { { "lb", "Personal" }, { "rb", "Guild" }, { "b", "Close" } } }) then
    if pressed("Tribute", "lb") then bpage = 1 end
    if pressed("Tribute", "rb") then bpage = 2 end
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local top = tabs({ "Personal", "Guild" }, bpage, x0, y0, function(i) bpage = i end)
    local midX = x0 + (x1 - x0) * 0.45
    local bh = T.px(60)
    if bpage == 1 then
      facts({ { "Favor", (API.character.Label(117)), C.endurance }, { "Career", (API.character.Label(118)) }, { "Cost", (API.character.Label(119)) },
        { "Time left", (API.character.Label(116)) } }, midX + T.px(24), x1, top)
      ui.SetCursorPos(x0 - wx, top - wy)
      T.heading("Active benefits")
      local ly = select(2, ui.GetCursorScreenPos())
      benefits(BW, "ActiveBenefitList", mine, fits(y1 - ly, 54), midX - x0)
      P.draw("##tbdesc", bdesc, described(API.windows.Text(BW, "Description")), midX + T.px(24), top + T.px(80), x1, y1 - bh - T.px(14),
        { empty = "Focus a benefit to read about it." })
      ui.SetCursorPos(midX + T.px(24) - wx, y1 - bh - wy)
      buttons(BW, { { "Activate", "ActivateButton", true, useText = true } }, midX + T.px(24), x1, bh)
    else
      facts({ { "Guild favor", (API.character.Label(122)), C.endurance }, { "Cost", (API.character.Label(123)) }, { "Time left", (API.character.Label(121)) },
        { "Opted in", (API.windows.Text(BW, "GuildStatusLabel")) } }, midX + T.px(24), x1, top)
      ui.SetCursorPos(x0 - wx, top - wy)
      T.heading("Guild benefits")
      local ly = select(2, ui.GetCursorScreenPos())
      benefits(BW, "GuildBenefitList", guildList, fits(y1 - ly, 54), midX - x0)
      P.draw("##tgdesc", gdesc, described(API.windows.Text(BW, "GuildDescription")), midX + T.px(24), top + T.px(80), x1, y1 - bh - T.px(14),
        { empty = "Focus a benefit to read about it." })
      ui.SetCursorPos(midX + T.px(24) - wx, y1 - bh - wy)
      buttons(BW, { { "Opt In", "OptOutButton", false, useText = true }, { "Activate", "GuildActivateButton", true, useText = true } },
        midX + T.px(24), x1, bh)
    end
  end
  if W.finish() then API.windows.Show(BW, false) end
end)
