-- Achievement browser presentation. All progress is received state; drawing,
-- filtering and expanding categories never requests data or awards anything.
local API = hs.UI
local M = {}
function M.new(look)
  local B = {}
  local catalogCache, stateCache, catalogToken, stateRevision
  local category, search, statusFilter = nil, "", "All"
  local expanded, categorySets, categoryStats, byId, children = {}, {}, {}, {}, {}
  local actionMessage, serverMessage
  local matchCache = {} -- matching()'s last rows and what they were worked out from
  local colors = look.colors
  local function colored(text, color)
    ui.TextColored(color[1], color[2], color[3], color[4] or 1, text)
  end
  local function muted(text) colored(text, colors.muted) end
  local function title(text) look.heading(text) end
  local function statusName(value) return value == "Open" and "In progress" or (value or "Unavailable") end
  local function statusColor(value)
    return value == "Completed" and colors.complete or value == "Open" and colors.accent or colors.muted
  end
  local function number(value)
    local text = tostring(value or 0)
    local out = text:reverse():gsub("(%d%d%d)", "%1,"):reverse():gsub("^,", "")
    return out
  end
  local function summary(stats)
    return number(stats.completed) .. " / " .. number(stats.total) .. " completed"
  end
  function B.reset()
    catalogCache, stateCache, catalogToken, stateRevision = nil, nil, nil, nil
    category, search, statusFilter, actionMessage = nil, "", "All", nil
    serverMessage, matchCache = nil, {}
    expanded, categorySets, categoryStats, byId, children = {}, {}, {}, {}, {}
    if look.cancel then look.cancel() end
  end
  local function rebuildCatalog()
    byId, children, categorySets, categoryStats = {}, {}, {}, {}
    local sorted = {}
    for position, value in ipairs(catalogCache.categories or {}) do
      byId[value.id] = value
      sorted[#sorted + 1] = { value = value, position = position }
    end
    table.sort(sorted, function(a, b)
      local ao, bo = a.value.display_order or 0, b.value.display_order or 0
      return ao == bo and a.position < b.position or ao < bo
    end)
    for _, entry in ipairs(sorted) do
      local parent = byId[entry.value.parent_id] and entry.value.parent_id or false
      children[parent] = children[parent] or {}
      children[parent][#children[parent] + 1] = entry.value
    end
  end
  local record = {} -- the state's record, refilled each call rather than made anew (hs.Achievements' `into`)
  function B.readState()
    local state = API.achievements.List(catalogToken, stateRevision, record)
    if not state then B.reset(); return nil end
    if state.message ~= serverMessage then actionMessage = nil; serverMessage = state.message end
    if catalogToken ~= state.catalog_token then
      category, expanded, actionMessage = nil, {}, nil
    end
    if not state.catalog_unchanged then
      catalogCache = state.catalog or {}
      rebuildCatalog()
    end
    if not state.states_unchanged then stateCache = state.states; categoryStats = {} end
    catalogToken, stateRevision = state.catalog_token, state.state_revision
    state.catalog, state.states = catalogCache or {}, stateCache or {}
    return state
  end
  local function result(ok, reason)
    actionMessage = not ok and (reason or "This achievement is no longer available.") or nil
  end
  local function button(label, enabled, action, width)
    if look.button(label, enabled, width) and enabled then action() end
  end
  local function setFor(id)
    if not id then return nil end
    if categorySets[id] then return categorySets[id] end
    local ids, seen, pending = {}, {}, {id}
    while #pending > 0 do
      local current = table.remove(pending)
      if not seen[current] then
        seen[current] = true
        local value = byId[current]
        if value then
          for _, link in ipairs(value.associations or {}) do ids[link.achievement_id] = true end
          -- Use the received hierarchy; IDs are never indexes in a filtered list.
          for _, child in ipairs(children[current] or {}) do pending[#pending + 1] = child.id end
        end
      end
    end
    categorySets[id] = ids
    return ids
  end
  local function statsFor(state, id)
    local key = id or "all"
    if categoryStats[key] then return categoryStats[key] end
    local stats = {completed=0,total=0,points=0,total_points=0}
    local allowed = setFor(id)
    for _, definition in ipairs(state.catalog.definitions or {}) do
      local own = state.states[definition.index + 1]
      if own and own.known and own.status ~= "Hidden" and (not allowed or allowed[definition.id]) then
        stats.total = stats.total + 1
        stats.total_points = stats.total_points + (definition.points or 0)
        if own.status == "Completed" then
          stats.completed = stats.completed + 1
          stats.points = stats.points + (definition.points or 0)
        end
      end
    end
    categoryStats[key] = stats
    return stats
  end
  local function progress(stats, scale)
    local label = summary(stats)
    ui.ProgressBar(stats.total > 0 and stats.completed / stats.total or 0, -1, 18 * scale, label)
  end
  local function categoryRow(state, value, width, height, scale)
    local id, caption = value and value.id, value and value.name or "All achievements"
    local selected = category == id
    local x, y = ui.GetCursorScreenPos()
    local clicked, focused = look.row("category:" .. (id or "all"), selected, width, height)
    if clicked then category = id end
    local stats = statsFor(state, id)
    local count = number(stats.completed) .. " / " .. number(stats.total)
    local countWidth, textHeight = ui.CalcTextSize(count)
    local padding = 6 * scale
    local countX = x + width - countWidth - padding
    local artRight = math.max(x + padding, countX - 10 * scale)
    local texture, u0, v0, u1, v1, imageWidth, imageHeight
    if value and value.icon and value.icon ~= "" then
      -- These XML animations are wide badges with baked lettering. Resolve
      -- live handles each frame: the texture cache can retire an old handle.
      local suffix = selected and "Selected" or focused and "Hover" or ""
      texture, u0, v0, u1, v1, imageWidth, imageHeight = API.assets.Animation(value.icon .. suffix)
      if not texture and suffix ~= "" then
        texture, u0, v0, u1, v1, imageWidth, imageHeight = API.assets.Animation(value.icon)
      end
    end
    ui.PushClipRect(x, y, x + width, y + height, true)
    if texture and imageWidth and imageWidth > 0 and imageHeight and imageHeight > 0 then
      local zoom = math.min((look.categoryArtScale or 1) * scale,
        (height - 4 * scale) / imageHeight, math.max(0, artRight - x - padding) / imageWidth)
      local w, h = imageWidth * zoom, imageHeight * zoom
      ui.DrawImage(texture, x + padding, y + (height - h) / 2, x + padding + w, y + (height + h) / 2, u0, v0, u1, v1)
    else
      ui.PushClipRect(x + padding, y, artRight, y + height, true)
      ui.DrawText(x + padding, y + (height - textHeight) / 2,
        colors.text[1], colors.text[2], colors.text[3], 1, caption)
      ui.PopClipRect()
    end
    local color = stats.total > 0 and stats.completed == stats.total and colors.complete or colors.muted
    ui.DrawText(math.max(x + padding, countX), y + (height - textHeight) / 2, color[1], color[2], color[3], 1, count)
    ui.PopClipRect()
    if focused then
      ui.SetTooltip(caption .. "\n" .. summary(stats) .. "\n" .. number(stats.points) .. " / " .. number(stats.total_points) .. " points")
    end
  end
  local function categoryTree(state, scale)
    title("Categories")
    local width = ui.GetContentRegionAvail()
    local rowHeight = (look.categoryHeight or 36) * scale
    categoryRow(state, nil, width, rowHeight, scale)
    ui.Separator()
    local arrowWidth = (look.chevronWidth or 24) * scale
    local pending = {}
    local function push(values, depth)
      for index = #(values or {}), 1, -1 do pending[#pending + 1] = {value=values[index],depth=depth} end
    end
    push(children[false], 0)
    while #pending > 0 do
      local entry = table.remove(pending)
      local value, depth = entry.value, entry.depth
      local nested = children[value.id] and #children[value.id] > 0
      local x, y = ui.GetCursorPos()
      ui.SetCursorPos(x + math.min(depth, 8) * 8 * scale, y)
      local available = ui.GetContentRegionAvail()
      if nested then
        if look.select((expanded[value.id] and "-" or "+") .. "###expandcategory:" .. value.id,
            false, arrowWidth, rowHeight) then expanded[value.id] = not expanded[value.id] end
      else ui.Dummy(arrowWidth, rowHeight) end
      ui.SameLine(0, 3 * scale)
      categoryRow(state, value, math.max(20 * scale, available - arrowWidth - 3 * scale), rowHeight, scale)
      if nested and expanded[value.id] then push(children[value.id], depth + 1) end
    end
  end
  local function completionDate(value)
    if not value then return nil end
    local ok, date = pcall(os.date, "%b %d, %Y", value)
    return ok and date or nil
  end
  local function objectiveProgress(value, scale)
    if value.type == 3 then return end
    if value.satisfied_known then
      colored(value.satisfied and "Complete" or "Incomplete", value.satisfied and colors.complete or colors.muted)
    else muted("Status unavailable") end
    if value.count_known then
      local count = value.count_text or tostring(value.count)
      local required = value.required_count or 0
      local label = count .. " / " .. number(required)
      if required > 0 then
        -- The bar is a clamped visual ratio, never a completion decision.
        local fraction = math.max(0, math.min(1, (tonumber(count) or 0) / required))
        ui.ProgressBar(fraction, -1, 16 * scale, label)
        if ui.IsItemHovered() then ui.SetTooltip(label) end
      else muted("Count: " .. count) end
    else muted("Count unavailable") end
  end
  -- The objectives' positions in display order (by type, then display order, then as received): sorted again only
  -- when a component's type or display order differs from the last sort's.
  local objectiveCache = {n=-1,types={},orders={},positions={}}
  local function objectiveOrder(components)
    local c, n = objectiveCache, #components
    local same = c.n == n
    if same then
      for i = 1, n do
        local value = components[i]
        if c.types[i] ~= value.type or c.orders[i] ~= (value.display_order or 0) then same = false; break end
      end
    end
    if same then return c.positions end
    local types, orders, positions = {}, {}, {}
    for i = 1, n do
      types[i], orders[i], positions[i] = components[i].type, components[i].display_order or 0, i
    end
    table.sort(positions, function(a, b)
      if types[a] ~= types[b] then return types[a] < types[b] end
      return orders[a] == orders[b] and a < b or orders[a] < orders[b]
    end)
    c.n, c.types, c.orders, c.positions = n, types, orders, positions
    return positions
  end
  function B.detail(detail, scale, embedded)
    local own = detail.own or {}
    local other = detail.source == "own" and detail.comparison or nil
    local width, height = ui.GetContentRegionAvail()
    local footer = detail.source == "own" and (look.detailFooter or 58) * scale or 0
    ui.BeginChild(embedded and "##achievementinlinedetail" or "##achievementdetailbody", 0, math.max(60 * scale, height - footer), false)
    title(detail.name or "Achievement")
    colored(own.known and statusName(own.status) or "State unavailable", statusColor(own.status))
    ui.SameLine()
    colored(number(detail.points) .. " points", colors.accent)
    if detail.source == "link" then muted("Linked by " .. (detail.player_name or "player")) end
    local date = own.known and own.status == "Completed" and completionDate(own.completion_time)
    if date then muted("Completed " .. date) end
    local compact = height < 480 * scale
    if compact then
      if ui.CollapsingHeader("Description###achievementdescription") then ui.TextWrapped(detail.description or "") end
    else
      ui.Spacing(); ui.TextWrapped(detail.description or ""); ui.Spacing()
    end
    ui.Separator()
    if not compact then ui.Spacing() end
    title("Objectives")
    local components = own.components or {}
    local fulfilled, known = 0, 0
    for _, component in ipairs(components) do
      if component.type ~= 3 and component.satisfied_known then
        known = known + 1
        if component.satisfied then fulfilled = fulfilled + 1 end
      end
    end
    local rows = objectiveOrder(components)
    if known > 0 then muted(fulfilled .. " of " .. known .. " known objectives satisfied") end
    if #rows == 0 then muted("No objective information has been received.") end
    local comparison = {}
    for _, component in ipairs(other and other.components or {}) do
      comparison[component.type .. ":" .. component.id] = component
    end
    local columns = other and width >= 460 * scale
    if other then
      ui.Spacing()
      colored("Comparing with " .. (other.name or "player"), colors.accent)
      muted("Your status: " .. statusName(own.status) .. "  |  Their status: " .. statusName(other.status))
    end
    local tableOpen = columns and ui.BeginTable("##achievementobjectives", 2, ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerV)
    if tableOpen then
      ui.TableSetupColumn("Your progress")
      ui.TableSetupColumn(other.name or "Comparison")
      ui.TableHeadersRow()
    end
    for _, position in ipairs(rows) do
      local component = components[position]
      if tableOpen then ui.TableNextRow(); ui.TableNextColumn() end
      ui.Spacing()
      ui.PushTextWrapPos(0)
      colored(component.name or "Objective", component.type == 3 and colors.muted or colors.text)
      ui.PopTextWrapPos()
      if component.description and component.description ~= "" and component.description ~= component.name then ui.TextWrapped(component.description) end
      if component.type == 3 then muted("Additional information") else objectiveProgress(component, scale) end
      if other and component.type ~= 3 then
        if tableOpen then ui.TableNextColumn(); ui.Spacing() else muted(other.name or "Comparison") end
        local compared = comparison[component.type .. ":" .. component.id]
        if compared then objectiveProgress(compared, scale) else muted("Progress unavailable") end
      end
      if not tableOpen then ui.Separator() end
    end
    if tableOpen then ui.EndTable() end
    ui.EndChild()
    if detail.source == "own" then
      ui.Separator()
      button("View reward###achievementreward", detail.has_reward == true,
        function() result(API.achievements.Reward(detail.catalog_token, detail.definition_index)) end)
      ui.SameLine()
      button("Link###achievementlink", own.known == true,
        function() result(API.achievements.Link(detail.catalog_token, detail.definition_index)) end)
      if actionMessage then colored(actionMessage, colors.warning)
      elseif detail.has_reward then muted("Preview the reward before choosing it.")
      else muted("No reward is listed for this achievement.") end
    end
  end
  -- The rows the category, the search and the status filter keep: worked out again only when one of them, the
  -- catalog or the character's states change (the catalog and the states are kept tables until they do).
  local function matching(state)
    local m = matchCache
    if m.rows and m.catalog == state.catalog and m.states == state.states and m.ready == state.ready and
        m.category == category and m.search == search and m.filter == statusFilter then
      return m.rows
    end
    local allowed, rows = setFor(category), {}
    local needle = search:lower()
    for _, definition in ipairs(state.catalog.definitions or {}) do
      local own = state.states[definition.index + 1]
      if state.ready and own and own.known and own.status ~= "Hidden" and
          (statusFilter == "All" or own.status == statusFilter) and (not allowed or allowed[definition.id]) and
          (needle == "" or (definition.name or ""):lower():find(needle, 1, true) or
            (definition.description or ""):lower():find(needle, 1, true)) then
        rows[#rows + 1] = {definition=definition,state=own}
      end
    end
    m.rows, m.catalog, m.states, m.ready = rows, state.catalog, state.states, state.ready
    m.category, m.search, m.filter = category, search, statusFilter
    return rows
  end
  local function overview(state, scale)
    ui.BeginChild("##achievementoverview", 0, 0, false)
    title(category and byId[category].name or "Your achievements")
    local description = category and byId[category].description
    if description and description ~= "" then ui.TextWrapped(description) end
    ui.Spacing()
    local stats = statsFor(state, category)
    progress(stats, scale)
    colored(number(stats.points) .. " / " .. number(stats.total_points) .. " points", colors.accent)
    ui.Spacing(); ui.Separator(); ui.Spacing()
    ui.TextWrapped("Select an achievement to see its objectives, progress and reward.")
    if search ~= "" or statusFilter ~= "All" then muted("The list reflects your search and status filter.") end
    ui.EndChild()
  end
  local function list(state, rows, scale, height)
    ui.BeginChild("##achievementlist", 0, height, true)
    local flags = ui.TableFlags.RowBg | ui.TableFlags.SizingStretchProp
    if ui.BeginTable("##achievementrows", 3, flags) then
      ui.TableSetupColumn("Achievement", ui.TableColumnFlags.WidthStretch)
      ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, (look.statusWidth or 95) * scale)
      ui.TableSetupColumn("Points", ui.TableColumnFlags.WidthFixed, (look.pointsWidth or 48) * scale)
      ui.TableHeadersRow()
      for _, entry in ipairs(rows) do
        local definition, own = entry.definition, entry.state
        ui.TableNextRow(); ui.TableNextColumn()
        local selected = state.detail_source ~= "link" and state.selected_index == definition.index
        if look.select(definition.name .. "###achievement:" .. state.catalog_token .. ":" .. definition.index,
            selected, nil, 30 * scale, true) then
          result(API.achievements.Open(state.catalog_token, definition.index))
        end
        if ui.IsItemHovered() then ui.SetTooltip(definition.name .. "\n" .. (definition.description or "")) end
        ui.TableNextColumn(); colored(statusName(own.status), statusColor(own.status))
        ui.TableNextColumn(); ui.Text(number(definition.points))
      end
      ui.EndTable()
    end
    if not state.ready then ui.TextWrapped("Waiting for achievement definitions and character state.")
    elseif #rows == 0 then
      ui.Spacing()
      ui.TextWrapped("No achievements match these filters.")
      muted("Try another category, clear the search, or choose All.")
    end
    ui.EndChild()
  end
  function B.main(state, scale)
    local stats = statsFor(state)
    title(state.ready and (number(stats.points) .. " achievement points") or "Achievements")
    if state.ready then progress(stats, scale) end
    if not state.ready then muted("Waiting for achievement data.")
    elseif not state.progress_ready then muted("Objective counts have not arrived yet.") end
    ui.Spacing()
    local width = ui.GetContentRegionAvail()
    ui.Text("Search")
    ui.SameLine()
    local searchWidth = ui.GetContentRegionAvail()
    ui.SetNextItemWidth(math.max(60 * scale, searchWidth - (look.keyboard and 150 * scale or 0)))
    search = ui.InputText("###achievementsearch", search, 128)
    if look.keyboard then
      ui.SameLine()
      if look.button("Keyboard###achievementkeyboard", true) then look.keyboard(search) end
      local value, pause = look.editKeyboard()
      if value then search = value end
      if pause then return end
    end
    local used = 0
    for index, value in ipairs({"All","Open","Completed","Locked"}) do
      local filterWidth = math.max(75 * scale, ui.CalcTextSize(statusName(value)) + (look.filterPadding or 24) * scale)
      if index > 1 and used + 6 * scale + filterWidth <= width then ui.SameLine(0, 6 * scale)
      else used = 0 end
      if look.select(statusName(value) .. "###filter:" .. value, statusFilter == value, filterWidth, 28 * scale) then statusFilter = value end
      used = used + filterWidth + 6 * scale
    end
    ui.Spacing()
    width = ui.GetContentRegionAvail()
    local _, height = ui.GetContentRegionAvail()
    local bodyHeight = math.max(100 * scale, height - (look.footerHeight or 88) * scale)
    local categoryWidth = math.min((look.categoryWidth or 300) * scale, width * 0.32)
    local rightWidth = math.max(80 * scale, width - categoryWidth - 10 * scale)
    ui.BeginChild("##achievementcategories", categoryWidth, bodyHeight, true)
    categoryTree(state, scale)
    ui.EndChild(); ui.SameLine(0, 10 * scale)
    ui.BeginChild("##achievementbrowser", rightWidth, bodyHeight, false)
    if category and not byId[category] then category = nil end
    colored(category and byId[category].name or "All achievements", colors.text)
    local rows = matching(state)
    muted(#rows .. (#rows == 1 and " achievement" or " achievements"))
    ui.Spacing()
    local contentWidth, contentHeight = ui.GetContentRegionAvail()
    local wide = contentWidth >= 820 * scale
    if wide then
      local listWidth = math.floor(contentWidth * 0.47)
      ui.BeginChild("##achievementlistpane", listWidth, 0, false)
      list(state, rows, scale, 0)
      ui.EndChild(); ui.SameLine(0, 10 * scale)
      ui.BeginChild("##achievementdetailpane", 0, 0, true, ui.WindowFlags.NoScrollbar | ui.WindowFlags.NoScrollWithMouse)
    else
      list(state, rows, scale, math.max(70 * scale, math.floor(contentHeight * 0.25)))
      ui.Spacing()
      ui.BeginChild("##achievementdetailpane", 0, 0, true, ui.WindowFlags.NoScrollbar | ui.WindowFlags.NoScrollWithMouse)
    end
    local detail = API.achievements.Get()
    local visible = false
    if detail and detail.source == "own" and detail.catalog_token == state.catalog_token then
      for _, row in ipairs(rows) do if row.definition.index == detail.definition_index then visible = true; break end end
    end
    if visible then B.detail(detail, scale, true) else overview(state, scale) end
    ui.EndChild(); ui.EndChild()
    ui.Spacing()
    button(state.refresh_pending and "Refreshing...###refreshachievements" or "Refresh###refreshachievements",
      not state.refresh_pending, function() result(API.achievements.Refresh()) end)
    ui.SameLine()
    local compare = state.comparison or {}
    button("Compare target###compareachievements", state.ready and not compare.pending,
      function() result(API.achievements.Compare()) end)
    if compare.ready or compare.pending then
      ui.SameLine()
      button("Clear comparison###clearachievementcompare", true, function() result(API.achievements.ClearCompare()) end)
    end
    if compare.pending then muted("Waiting for the comparison result.")
    elseif compare.ready then muted("Comparing with " .. (compare.name or "player")) end
    if actionMessage then colored(actionMessage, colors.warning)
    elseif compare.message and compare.message ~= "" then muted(compare.message)
    elseif state.message and state.message ~= "" then muted(state.message)
    elseif not compare.pending and not compare.ready then
      muted("Select a nearby player, then Compare target to compare progress.")
    end
  end
  function B.closeMain()
    API.windows.Show("AchievementsWnd", false)
    local detail = API.achievements.Get()
    if detail and detail.source == "own" then API.windows.Show("AchievementWnd", false) end
  end
  function B.floating()
    local detail = API.achievements.Get()
    if not detail or not detail.shown then return nil end
    local state = B.readState()
    if state and state.shown and detail.source == "own" then return nil end
    return detail
  end
  return B
end
return M
