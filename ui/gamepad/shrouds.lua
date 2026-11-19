local API = hs.UI
local W = dofile("lib/window.lua")
local P = dofile("lib/textpanel.lua")
local T, pad = W.T, W.pad
local C = T.C

local DETAIL_TABS = { "Stats", "Abilities", "Equipment" }
local function fits(height, rowHeight)
  return math.max(1, math.floor((height + T.px(10)) / T.px(rowHeight + 10)))
end

local function picker(screen, title, accept, close)
  local catalog, detail = {}, {}
  local abilityDescription = {}
  local tab = 1
  ui.window(title, function()
    if not W.shown(screen) then catalog, detail, abilityDescription, tab = {}, {}, {}, 1; return end
    if W.begin(title, { w = 1540, h = 900, title = title, alwaysFocus = true,
        hints = { { "a", "Select" }, { "lb", "Details" }, { "rb", "Details" }, { "b", close } } }) then
      if W.pressed("lb") then tab, detail = (tab + 1) % #DETAIL_TABS + 1, {} end
      if W.pressed("rb") then tab, detail = tab % #DETAIL_TABS + 1, {} end
      local x0, y0, x1, y1, w = W.area()
      local wx, wy = ui.GetWindowPos()
      local split = x0 + w * 0.44
      local detailsX = split + T.px(16)
      local footer = y1 - T.px(62)
      local n, _, row = API.windows.ListInfo(screen, "Forms")
      catalog.selected = row and row + 1 or 0
      ui.SetCursorPos(x0 - wx, y0 - wy)
      if not n or n == 0 then
        ui.TextWrapped("The server offered no forms.")
      else
        W.list("shroudForms", catalog, n, fits(footer - y0, 66), 66, split - x0,
          function(i, rx0, ry0, rx1, ry1)
            local v = API.windows.ListRow(screen, "Forms", i - 1)
            if not v then return end
            local p = T.px(12)
            local width = rx1 - rx0 - 2 * p
            local title = v[2] or ""
            if (v[3] or "") ~= "" then title = title .. "  ·  Level " .. v[3] end
            T.draw(rx0 + p, ry0 + T.px(8), T.fit(title, width))
            T.font(18)
            local details = (v[1] or "") .. "  ·  " .. (v[4] or "")
            if (v[5] or "") ~= "" then details = details .. "  ·  " .. v[5] .. " assigned" end
            T.draw(rx0 + p, ry0 + T.px(37), T.fit(details, width), C.dim)
            T.unfont()
          end,
          function(i)
            API.windows.ListClick(screen, "Forms", i - 1, "left")
            detail, abilityDescription = {}, {}
          end)
      end

      local width = x1 - detailsX
      ui.SetCursorPos(detailsX - wx, y0 - wy)
      local tabWidth = (width - T.px(20)) / #DETAIL_TABS
      for i, label in ipairs(DETAIL_TABS) do
        if i > 1 then ui.SameLine(0, T.px(10)) end
        local active, height = i == tab, T.px(47)
        local hit, _, bx, by = T.button("##shroudTab" .. i, active and "" or label, tabWidth, height, { primary = active }, pad)
        if active then
          local tw, th = ui.CalcTextSize(label)
          ui.DrawText(bx + (tabWidth - tw) / 2, by + (height - th) / 2, C.onAccent[1], C.onAccent[2], C.onAccent[3], 1, label)
        end
        if hit and not active then tab, detail = i, {} end
      end

      ui.SetCursorPos(detailsX - wx, y0 + T.px(56) - wy)
      ui.BeginChild("##shroudDescription", width, T.px(122), false)
      ui.TextWrapped(API.windows.Text(screen, "Description") or "")
      local status = API.windows.Text(screen, "DetailStatus")
      if status and status ~= "" then ui.TextWrapped(status) end
      local requirement = API.windows.Text(screen, "RoleRequirement")
      if requirement and requirement ~= "" then ui.TextWrapped(requirement) end
      ui.EndChild()

      local id = DETAIL_TABS[tab]
      local detailTop = y0 + T.px(188)
      local count, _, detailRow = API.windows.ListInfo(screen, id)
      detail.selected = detailRow and detailRow + 1 or 0
      local description = id == "Abilities" and (API.windows.Text(screen, "AbilityDescription") or "") or ""
      ui.SetCursorPos(detailsX - wx, detailTop - wy)
      if description ~= "" then
        P.draw("##shroudAbilityDescription", abilityDescription, description, detailsX, detailTop, x1,
          detailTop + T.px(90), { size = 19 })
        detailTop = detailTop + T.px(100)
        ui.SetCursorPos(detailsX - wx, detailTop - wy)
      end
      ui.BeginGroup()
      W.list("shroud" .. id, detail, count or 0, fits(footer - detailTop, 44), 44, width,
        function(i, rx0, ry0, rx1, ry1)
          local v = API.windows.ListRow(screen, id, i - 1)
          if not v then return end
          local p = T.px(12)
          T.textIn(rx0 + p, ry0, rx1 - p, ry1, T.fit(v[1] or "", (rx1 - rx0) * 0.7), C.text, 0)
          if v[2] then T.textIn(rx0, ry0, rx1 - p, ry1, v[2], C.dim, 1) end
        end,
        id ~= "Stats" and function(i)
          API.windows.ListClick(screen, id, i - 1, "left")
          abilityDescription = {}
        end or nil)
      ui.EndGroup()

      ui.SetCursorPos(detailsX - wx, footer - wy)
      local enabled = API.windows.ChildState(screen, "Accept") == true
      if T.button("##acceptShroud", accept, width, T.px(52), { disabled = not enabled }, pad) then
        API.windows.ClickChild(screen, "Accept", "left")
      end
    end
    if W.finish() then API.windows.Show(screen, false) end
  end)
end

picker("ProgressionSelectionWnd", "Spirit Shroud Selection", "Select Shroud", "Cancel")
picker("TaskTemplateSelectWnd", "Monster Mission Selection", "Select Monster", "Close")

local PROGRESS, progressState = "ShroudProgressWnd", {}
ui.window("Spirit Shroud Progress", function()
  if not W.shown(PROGRESS) then progressState = {}; return end
  if W.begin("Spirit Shroud Progress", { w = 1150, h = 660, title = "Spirit Shroud Progress",
      hints = { { "a", "Details" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, w, h = W.area()
    local wx, wy = ui.GetWindowPos()
    local listWidth = w * 0.50
    local count, _, row = API.windows.ListInfo(PROGRESS, "Progress")
    progressState.selected = row and row + 1 or 0
    ui.SetCursorPos(x0 - wx, y0 - wy)
    W.list("shroudProgress", progressState, count or 0, fits(h, 66), 66, listWidth,
      function(i, rx0, ry0, rx1)
        local v = API.windows.ListRow(PROGRESS, "Progress", i - 1)
        if not v then return end
        local p = T.px(12)
        T.draw(rx0 + p, ry0 + T.px(8), T.fit(v[1] .. "  ·  " .. v[2], rx1 - rx0 - 2 * p))
        T.font(18)
        T.draw(rx0 + p, ry0 + T.px(37), T.fit(v[3] or "", rx1 - rx0 - 2 * p), C.dim)
        T.unfont()
      end,
      function(i) API.windows.ListClick(PROGRESS, "Progress", i - 1, "left") end)
    local description = API.windows.Text(PROGRESS, "Description") or ""
    ui.SetCursorPos(x0 + listWidth - wx, y0 - wy)
    ui.BeginChild("##progressDescription", x1 - x0 - listWidth, y1 - y0, false)
    ui.TextWrapped(description ~= "" and description or "Choose a category to see its details.")
    ui.EndChild()
  end
  if W.finish() then API.windows.Show(PROGRESS, false) end
end)

local BANK, bankState = "AltStorageWnd", {}
ui.window("Shroud Bank", function()
  if not W.shown(BANK) then bankState = {}; return end
  local timer = API.windows.Text(BANK, "TimerLabel") or ""
  if W.begin("Shroud Bank", { w = 920, h = 730, title = "Shroud Bank", alwaysFocus = true,
      subtitle = timer ~= "" and "Time remaining: " .. timer or nil,
      hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, width = W.area()
    local wx, wy = ui.GetWindowPos()
    ui.SetCursorPos(x0 - wx, y0 - wy)
    local status = API.windows.Text(BANK, "Status") or ""
    if status ~= "" then ui.TextWrapped(status) end
    local _, y = ui.GetCursorScreenPos()
    local count, _, selected = API.windows.ListInfo(BANK, "ItemList")
    bankState.selected = selected and selected + 1 or 0
    local footer = y1 - T.px(62)
    W.list("shroudBank", bankState, count or 0, fits(footer - y, 60), 60, width,
      function(i, rx0, ry0, rx1, ry1)
        local v, _, _, _, tex, u0, v0, u1, v1 = API.windows.ListRow(BANK, "ItemList", i - 1)
        if not v then return end
        local padX = T.px(12)
        if tex then
          local icon = T.px(40)
          local y0i = (ry0 + ry1 - icon) / 2
          ui.DrawImage(tex, rx0 + padX, y0i, rx0 + padX + icon, y0i + icon, u0, v0, u1, v1)
          padX = padX + icon + T.px(10)
        end
        T.textIn(rx0 + padX, ry0, rx1, ry1, T.fit(v[1] or "", rx1 - rx0 - padX - T.px(85)), C.text, 0)
        T.textIn(rx0, ry0, rx1 - T.px(12), ry1, "×" .. (v[2] or ""), C.dim, 1)
      end,
      function(i) API.windows.ListClick(BANK, "ItemList", i - 1, "left") end)
    ui.SetCursorPos(x0 - wx, footer - wy)
    local half = (x1 - x0 - T.px(10)) / 2
    for i, action in ipairs({ { "GetButton", "Withdraw" }, { "InspectButton", "Inspect" } }) do
      if i > 1 then ui.SameLine(0, T.px(10)) end
      local enabled = API.windows.ChildState(BANK, action[1]) == true
      if T.button("##shroudBank" .. i, action[2], half, T.px(52), { disabled = not enabled }, pad) then
        API.windows.ClickChild(BANK, action[1], "left")
      end
    end
  end
  if W.finish() then API.windows.Show(BANK, false) end
end)
