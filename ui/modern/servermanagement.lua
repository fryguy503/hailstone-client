-- The server authorizes each management snapshot and save. The window edits only changed cfg keys.
local API = hs.UI
local manager = API.serverManagement
local hud = dofile("lib/hud.lua")
local K = dofile("lib/dialogs_kit.lua")
local M = dofile("lib/server_management.lua")
local T, C = hud.T, hud.T.color
local WINDOW = "ServerManagement"
local RULES = { { "", "Inherit" }, { "allow", "Allow" }, { "block", "Block" } }
local COMMAND_RULES = { RULES[1], RULES[2], { "block", "Deny" }, { "gm", "GM" } }
local DEFAULT = { { "allow", "Allow" }, { "deny", "Block" } }
local SWITCH = { { "on", "On" }, { "off", "Off" } }
local ADDON_MODE = { { "", "Inherit" }, { "allow", "Allow" }, { "block", "Block" }, { "required", "Required" } }
local GROUPS = { "non_rof2", "movement_automation", "command_helpers", "addon_commands" }
local ORIGINS = { "stock", "extended", "addon" }
local ORIGIN_NAMES = { "Stock (RoF2)", "Extended", "Add-on" }
local PAGE_SIZE = 7

local function toggle(id, label, fallback, description)
  return { id = id, label = label, fallback = fallback, description = description, choices = SWITCH, boolean = true }
end

local function input(id, label, fallback, description, capacity, hint, decimal)
  return { id = id, label = label, fallback = fallback, description = description, capacity = capacity, hint = hint,
    flags = decimal and ui.InputTextFlags.CharsDecimal or 0 }
end

local GAMEPLAY = {
  { id = "client.deny", label = "Hailstone access", fallback = "off", description = "Block Hailstone at login.",
    choices = { { "off", "Allowed" }, { "on", "Blocked" } }, boolean = true },
  input("management.min_status", "Minimum GM status", "200", "Minimum account status to open or save this window.", 5, nil, true),
  toggle("abilities.autoskill", "Ability Book autoskill", "on", "Allow automatic ability use."),
  toggle("movement.npc_collision", "NPC collision", "on", "Live NPCs stop player movement."),
  toggle("movement.passable_ladders", "Passable ladders", "on", "Walk or swim into passable ladders to climb them."),
  toggle("movement.solid_ladders", "Solid ladders", "on", "Treat ladder faces as a single surface for movement."),
}
local SCRIPTS = {
  toggle("scripts.player", "Player Lua scripts", "off", "Allow players' own Lua scripts."),
  input("scripts.player_caps", "Player script capabilities", "all", "Capabilities granted to allowed player scripts.", 256, "read, ui"),
  input("scripts.run", "Server scripts to start", "", "Comma-separated script names for zone-in.", 8192, "hud, welcome"),
  toggle("plugins.player", "Player compiled plugins", "off", "Allow players' own compiled plugins."),
  input("plugins.player_caps", "Player plugin capabilities", "all", "Capabilities granted to allowed player plugins.", 256, "read, ui"),
}
local SOCIALS = {
  { id = "socials.mode", label = "Lua social mode", fallback = "press", description = "Choose how Lua social buttons run.",
    choices = { { "off", "Off" }, { "press", "Press" }, { "full", "Full" } } },
  input("socials.caps", "Social capabilities", "read,target,actions", "Capabilities granted to Lua socials.", 256, "read, target, actions"),
  input("socials.press_commands", "Commands per press", "5", "0 means unlimited in Press mode.", 11, nil, true),
  toggle("socials.faces", "Lua button faces", "on", "Allow Lua socials to draw their button faces."),
}
local ASSETS = {
  toggle("assets.player", "Player assets", "on", "Allow player-provided assets for this server."),
  input("web.allow", "Allowed web hosts", "", "Comma-separated hostnames. Blank allows any host.", 8192, "wiki.example.com"),
}
local ADDONS = {
  toggle("addons.enforce", "Enforce add-on policy", "off", "Apply the required, allowed, and blocked lists."),
  { id = "addons.default", label = "Default for unlisted packages", fallback = "allow", choices = DEFAULT },
}
local COMMAND_LISTS = {
  input("commands.allow", "Allow by name", "", "Comma-separated command names, including commands from unloaded add-ons.", 8192, "/echo, /customcommand"),
  input("commands.block", "Block by name", "", "Comma-separated command names. An explicit block takes priority over GM and allow rules.", 8192, "/nav, /follow"),
  input("commands.gm", "GM by name", "", "Comma-separated command:status entries. Minimum account status is 1 to 32767.", 8192, "/nav:200, /customcommand:150"),
}
local ADDON_LISTS = {
  input("addons.allow", "Allow", "", "Comma-separated repository IDs.", 8192, "example/hud"),
  input("addons.block", "Block", "", "Comma-separated repository IDs.", 8192, "example/auto-actions"),
  input("addons.required", "Require", "", "Repository IDs with optional minimum versions.", 8192, "example/hud:1.2.0"),
  input("addons.suggest", "Suggest", "", "Repository IDs, release URLs, or repo@tag.", 8192, "example/hud@v1.2.0"),
}

local active, draft, serial, catalogSerial, minimumStatus
local section, origin, page, search = 1, 2, 1, ""
local results, filteredText, filteredOrigin
local reviewing, confirmClose, addPackage, localMessage
local conflicts = {}
local fieldsById = {}
for _, collection in ipairs({ GAMEPLAY, SCRIPTS, SOCIALS, ASSETS, ADDONS }) do
  for _, field in ipairs(collection) do fieldsById[field.id] = field end
end
local changeLabels = {
  ["commands.default"] = "Default command policy", ["commands.blocked_message"] = "Blocked command message",
  ["commands.allow"] = "Allowed commands", ["commands.block"] = "Blocked commands", ["commands.gm"] = "GM command rules",
  ["commands.allow_groups"] = "Allowed command groups", ["commands.block_groups"] = "Blocked command groups",
  ["addons.allow"] = "Allowed add-ons", ["addons.block"] = "Blocked add-ons",
  ["addons.required"] = "Required add-ons and minimum versions", ["addons.suggest"] = "Suggested add-ons",
}

local function colored(text, color)
  ui.TextColored(color[1], color[2], color[3], color[4] or 1, text)
end

local function wrap(text, color)
  ui.PushTextWrapPos(0)
  if color then colored(text, color) else ui.Text(text) end
  ui.PopTextWrapPos()
end

local function selectValue(id, choices, current)
  local label = current
  for _, choice in ipairs(choices) do if choice[1] == current then label = choice[2] break end end
  if not ui.BeginCombo(id, label) then return nil end
  local selected
  for _, choice in ipairs(choices) do
    if ui.Selectable(choice[2], choice[1] == current) and choice[1] ~= current then selected = choice[1] end
    if choice[1] == current then ui.SetItemDefaultFocus() end
  end
  ui.EndCombo()
  return selected
end

local function heading(title, description)
  ui.PushFont(T.font.large)
  colored(title, C.heading)
  ui.PopFont()
  if description then wrap(description, C.muted) end
  ui.Spacing()
end

local function fieldValue(field)
  local value = M.get(draft, field.id, field.fallback)
  if field.boolean then
    local on = M.boolean(value:lower())
    if on == nil then return field.fallback end
    return on and "on" or "off"
  end
  return field.choices and value:lower() or value
end

local function drawField(field)
  local value, nextValue = fieldValue(field)
  if field.choices then nextValue = selectValue("##" .. field.id, field.choices, value)
  else
    local changed
    nextValue, changed = ui.InputTextWithHint("##" .. field.id, field.hint or "", value, field.capacity, field.flags or 0)
    if not changed then nextValue = nil end
  end
  if nextValue then
    M.set(draft, field.id, nextValue, field.fallback)
    localMessage = nil
  end
end

local function drawFields(fields)
  if not ui.BeginTable("##smfields", 2, ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerH) then return end
  ui.TableSetupColumn("Setting", ui.TableColumnFlags.WidthStretch, 1.2)
  ui.TableSetupColumn("Value", ui.TableColumnFlags.WidthStretch, 1)
  for _, field in ipairs(fields) do
    ui.TableNextRow()
    ui.TableNextColumn()
    ui.Text(field.label)
    if field.description then wrap(field.description, C.muted) end
    ui.TableNextColumn()
    ui.SetNextItemWidth(-1)
    drawField(field)
  end
  ui.EndTable()
end

local function commandGroups(state)
  if not ui.CollapsingHeader("Command groups") then return end
  if not ui.BeginTable("##smgroups", 2, ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerH) then return end
  ui.TableSetupColumn("Group", ui.TableColumnFlags.WidthStretch, 2)
  ui.TableSetupColumn("Rule", ui.TableColumnFlags.WidthStretch, 1)
  for _, group in ipairs(GROUPS) do
    ui.TableNextRow()
    ui.TableNextColumn()
    ui.Text(M.groupLabels[group])
    ui.TableNextColumn()
    ui.SetNextItemWidth(-1)
    local value = selectValue("##smgroup" .. group, RULES, state.groups[group] or "")
    if value then M.setCommandRule(draft, group, value, true) localMessage = nil end
  end
  ui.EndTable()
end

local function commands(s)
  heading("Command permissions", "Set what players can run on this server.")
  ui.Text("Default policy")
  ui.SameLine()
  ui.SetNextItemWidth(105 * s)
  drawField({ id = "commands.default", choices = DEFAULT, fallback = "allow" })
  ui.Text("Blocked command message")
  ui.SetNextItemWidth(-1)
  drawField(input("commands.blocked_message", "", "", nil, 240, "Use Hailstone's default message"))
  K.Caption("Players see this text in chat when a command is blocked.")
  ui.Spacing()

  local width = ui.GetContentRegionAvail()
  local tabs = math.min(width, 275 * s)
  local nextOrigin = K.Segmented("##smorigin", ORIGIN_NAMES, origin, tabs, K.ButtonH(s), s)
  if nextOrigin then origin, page = nextOrigin, 1 end
  if width - tabs >= 155 * s then ui.SameLine(0, 12 * s) end
  ui.SetNextItemWidth(-1)
  search = ui.InputTextWithHint("##smsearch", "Search names, aliases, or descriptions", search, 96)
  if filteredText ~= search or filteredOrigin ~= origin then
    results, page = M.filterCommands(draft, ORIGINS[origin], search), 1
    filteredText, filteredOrigin = search, origin
  elseif not results then
    results = M.filterCommands(draft, ORIGINS[origin], search)
    page = math.min(page, math.max(1, math.ceil(#results / PAGE_SIZE)))
  end

  commandGroups(M.commandRules(draft))
  if ui.CollapsingHeader("Command lists") then drawFields(COMMAND_LISTS) end
  local state = M.commandRules(draft)
  if #results == 0 then
    ui.Spacing()
    K.Caption(search ~= "" and "No matching commands." or "No add-on commands are registered in this session.")
    return
  end
  local first = (page - 1) * PAGE_SIZE + 1
  local last = math.min(first + PAGE_SIZE - 1, #results)
  local flags = ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerH | ui.TableFlags.RowBg
  ui.PushStyleVar(ui.StyleVar.CellPadding, 6 * s, 5 * s)
  if ui.BeginTable("##smcommands", 4, flags) then
    ui.TableSetupColumn("Command", ui.TableColumnFlags.WidthStretch, 1.05)
    ui.TableSetupColumn("Description", ui.TableColumnFlags.WidthStretch, 2.15)
    ui.TableSetupColumn("Rule", ui.TableColumnFlags.WidthFixed, 83 * s)
    ui.TableSetupColumn("Effective", ui.TableColumnFlags.WidthFixed, 82 * s)
    ui.TableHeadersRow()
    for i = first, last do
      local command = results[i]
      ui.TableNextRow()
      ui.TableNextColumn()
      wrap(command.name, C.accent)
      if #command.aliases > 0 then
        local text = table.concat(command.aliases, ", ")
        wrap(#text <= 26 and text or (#command.aliases .. " aliases"), C.muted)
        if ui.IsItemHovered() then ui.SetTooltip(text) end
      end
      ui.TableNextColumn()
      wrap(command.description ~= "" and command.description or "No description available.", C.muted)
      ui.TableNextColumn()
      ui.SetNextItemWidth(-1)
      local nextValue = selectValue("##smcommand" .. command.name, COMMAND_RULES, state.commands[command.name] or "")
      if nextValue then
        M.setCommandRule(draft, command.name, nextValue)
        state = M.commandRules(draft)
        localMessage = nil
      end
      if state.commands[command.name] == "gm" then
        ui.SetNextItemWidth(-1)
        local rank, changed = ui.InputTextWithHint("##smgm" .. command.name, "Min. status",
          state.gm[command.name].status, 5, ui.InputTextFlags.CharsDecimal)
        if ui.IsItemHovered() then ui.SetTooltip("Minimum GM account status (1 to 32767)") end
        if changed then
          M.setCommandRule(draft, command.name, "gm", false, rank)
          state = M.commandRules(draft)
          localMessage = nil
        end
      end
      ui.TableNextColumn()
      local effective, reason, threshold = M.commandEffective(state, command)
      if effective == "gm" then
        colored(threshold and string.format("GM %d+", threshold) or "GM invalid", threshold and C.gold or C.danger)
      else
        local blocked = effective == "block"
        colored(blocked and "Blocked" or "Allowed", blocked and C.danger or C.pet)
      end
      if ui.IsItemHovered() then ui.SetTooltip(reason) end
    end
    ui.EndTable()
  end
  ui.PopStyleVar()
  ui.Spacing()
  K.Caption(string.format("%d-%d of %d", first, last, #results))
  ui.SameLine()
  ui.BeginDisabled(page <= 1)
  if ui.Button("Previous##smpage") then page = page - 1 end
  ui.EndDisabled()
  ui.SameLine()
  ui.BeginDisabled(last >= #results)
  if ui.Button("Next##smpage") then page = page + 1 end
  ui.EndDisabled()
end

local function addons(s)
  heading("Add-on policy", "Manage packages by repository identity.")
  drawFields(ADDONS)
  ui.Spacing()
  local rows, index = M.addons(draft)
  local flags = ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerH | ui.TableFlags.RowBg
  ui.PushStyleVar(ui.StyleVar.CellPadding, 6 * s, 5 * s)
  if #rows > 0 and ui.BeginTable("##smaddons", 4, flags) then
    ui.TableSetupColumn("Package", ui.TableColumnFlags.WidthStretch, 1)
    ui.TableSetupColumn("Access", ui.TableColumnFlags.WidthFixed, 89 * s)
    ui.TableSetupColumn("Min. version", ui.TableColumnFlags.WidthFixed, 86 * s)
    ui.TableSetupColumn("Suggest", ui.TableColumnFlags.WidthFixed, 49 * s)
    ui.TableHeadersRow()
    for _, row in ipairs(rows) do
      ui.PushID(row.id)
      ui.TableNextRow()
      ui.TableNextColumn()
      wrap(row.id, C.accent)
      if row.source and ui.IsItemHovered() then ui.SetTooltip(row.source) end
      ui.TableNextColumn()
      ui.SetNextItemWidth(-1)
      local mode = selectValue("##smaddonmode", ADDON_MODE, row.mode or "")
      if mode then M.setAddonMode(draft, row, mode, row.minimum) row.mode = mode localMessage = nil end
      ui.TableNextColumn()
      ui.SetNextItemWidth(-1)
      ui.BeginDisabled(row.mode ~= "required")
      local minimum, changed = ui.InputTextWithHint("##smaddonmin", "Any", row.minimum or "", 64)
      ui.EndDisabled()
      if changed then M.setAddonMinimum(draft, row, minimum) localMessage = nil end
      ui.TableNextColumn()
      local suggest, toggleSuggest = ui.Checkbox("##smaddonsuggest", row.suggested == true)
      if toggleSuggest then M.suggestAddon(draft, row, suggest) localMessage = nil end
      ui.PopID()
    end
    ui.EndTable()
  else
    K.Caption("No packages listed.")
  end
  ui.PopStyleVar()
  ui.Spacing()
  ui.SetNextItemWidth(math.max(90 * s, ui.GetContentRegionAvail() - 112 * s))
  addPackage = ui.InputTextWithHint("##smaddpackage", "owner/repo", addPackage, 400)
  ui.SameLine()
  if ui.Button("Add package##sm") then
    local id = M.addonIdentity(addPackage)
    if not id then localMessage = "Enter a GitHub repository as owner/repo."
    elseif index[id] then localMessage = "That repository is already listed."
    else draft.newAddons[id], addPackage, localMessage = M.trim(addPackage), "", nil end
  end
  if ui.CollapsingHeader("Add-on lists") then drawFields(ADDON_LISTS) end
end

local PAGES = {
  { label = "Commands", draw = commands },
  { label = "Client & gameplay", title = "Client & gameplay", description = "Set client access, movement rules, and ability behavior.", fields = GAMEPLAY },
  { label = "Add-ons", draw = addons },
  { label = "Scripts & plugins", title = "Scripts & plugins", description = "Manage player extensions and server autorun scripts.", fields = SCRIPTS },
  { label = "Socials", title = "Lua socials", description = "Set how scripts attached to social buttons can run.", fields = SOCIALS },
  { label = "Assets & web", title = "Assets & web", description = "Control local assets and the browser's host list.", fields = ASSETS },
}

local function selectPage(index)
  if index ~= section then section, reviewing, localMessage = index, false, nil end
end

local function review(changes)
  heading("Review changes", "Saved policy takes effect on players' next login.")
  for _, change in ipairs(changes) do
    local field = fieldsById[change.id]
    local previous = change.previous or (field and field.fallback) or "(unset)"
    colored(field and field.label or changeLabels[change.id] or change.id, C.heading)
    wrap((previous ~= "" and previous or "(empty)") .. " -> " ..
      (change.value ~= "" and change.value or "(empty)"), C.muted)
    if conflicts[change.id] then
      wrap("The server value changed after you opened the editor.", C.gold)
      if not (active.reload_required or active.reloading or active.saving) then
        if ui.Button("Use server value##" .. change.id) then
          draft.edits[change.id], conflicts[change.id] = nil, nil
        end
        ui.SameLine()
        if ui.Button("Keep my edit##" .. change.id) then conflicts[change.id] = nil end
      end
    end
    if change.id == "client.deny" and change.value == "on" then
      wrap("Blocking Hailstone access also blocks this window after you log in again.", C.danger)
    elseif change.id == "management.min_status" then
      wrap("Your GM account must still meet the new status to reopen this window.", C.gold)
    end
    ui.Separator()
  end
end

local function primary(label)
  ui.PushStyleColor(ui.Col.Button, T.rgba(C.accent, 0.82))
  ui.PushStyleColor(ui.Col.ButtonHovered, T.rgba(C.accent))
  ui.PushStyleColor(ui.Col.ButtonActive, T.rgba(C.accent, 0.95))
  ui.PushStyleColor(ui.Col.Text, T.rgba(C.onAccent))
  local clicked = ui.Button(label)
  ui.PopStyleColor(4)
  return clicked
end

local function body(_, _, _, _, s)
  local width, height = ui.GetContentRegionAvail()
  colored("HAILSTONE", C.accent)
  ui.SameLine(math.max(106 * s, width - 115 * s))
  colored(string.format("GM access / %d+", minimumStatus), C.gold)
  ui.Separator()
  local layoutWidth, layoutHeight = ui.GetContentRegionAvail()
  local contentHeight = math.max(68 * s, math.min(height, layoutHeight) - 65 * s)
  local side = layoutWidth >= 560 * s
  if side then
    ui.PushStyleColor(ui.Col.ChildBg, T.rgba(C.titleActive))
    ui.BeginChild("##smnav", 151 * s, contentHeight, true)
    ui.PopStyleColor()
    ui.BeginDisabled(confirmClose or active.saving)
    for index, entry in ipairs(PAGES) do
      if ui.Selectable(entry.label, index == section, 0, 0, 26 * s) then selectPage(index) end
    end
    ui.EndDisabled()
    ui.EndChild()
    ui.SameLine()
  end
  ui.BeginChild("##smpane", 0, contentHeight, false)
  if not side then
    ui.SetNextItemWidth(-1)
    if ui.BeginCombo("##smsection", PAGES[section].label) then
      for index, entry in ipairs(PAGES) do
        if ui.Selectable(entry.label, index == section) and not confirmClose and not active.saving then selectPage(index) end
      end
      ui.EndCombo()
    end
  end
  if confirmClose then
    heading("Discard unsaved changes?", "Closing this window discards your draft.")
  elseif reviewing then
    review(M.changes(draft))
  else
    local entry = PAGES[section]
    if entry.draw then entry.draw(s)
    else heading(entry.title, entry.description) drawFields(entry.fields) end
  end
  ui.EndChild()
  ui.Separator()
  local changes = M.changes(draft)
  for id in pairs(conflicts) do
    if draft.edits[id] == nil then conflicts[id] = nil end
  end
  local hasConflicts = next(conflicts) ~= nil
  local gmEdits = draft.edits["commands.gm"]
  local invalidGm = gmEdits ~= nil and not M.validCommandGmList(gmEdits)
  local footerTable = width >= 585 * s and ui.BeginTable("##smfooter", 2, ui.TableFlags.SizingStretchProp)
  if footerTable then
    ui.TableSetupColumn("Policy", ui.TableColumnFlags.WidthStretch, 1)
    ui.TableSetupColumn("Actions", ui.TableColumnFlags.WidthFixed, 230 * s)
    ui.TableNextRow()
    ui.TableNextColumn()
  end
  local version = active.file_revision and active.file_revision ~= "" and "Revision " .. active.file_revision
    or "No saved revision"
  wrap("client_files/hailstone.cfg / " .. version .. " / Applies on next login", C.muted)
  local serverMessage = active.message ~= "" and active.message
  local status = (active.reloading and "Reloading policy...") or localMessage or
    (active.reload_required and (serverMessage or "Reload the policy before saving.")) or
    (hasConflicts and "Resolve the changed server values before saving.") or
    (invalidGm and "GM rules require command:status with a status from 1 to 32767.") or serverMessage or
    (active.saving and "Saving policy..." or (#changes == 0 and "No unsaved changes" or
      string.format("%d unsaved %s", #changes, #changes == 1 and "change" or "changes")))
  local attention = not active.reloading and (active.reload_required or hasConflicts or invalidGm or localMessage or
    (reviewing and active.message ~= "" and not active.saving))
  wrap(status, attention and C.danger or C.muted)
  if footerTable then ui.TableNextColumn() end
  ui.BeginDisabled(active.saving or active.reloading)
  if confirmClose then
    if ui.Button("Keep editing##smclose") then confirmClose = false end
    ui.SameLine()
    if primary("Discard and close##sm") then manager.Close() end
  elseif active.reload_required or active.reloading then
    if primary("Reload policy##sm") and not active.reloading then
      localMessage = not manager.Open() and "Could not request the current policy. Try again." or nil
    end
  elseif reviewing then
    if ui.Button("Keep editing##smreview") then reviewing, localMessage = false, nil end
    ui.SameLine()
    local canSave = not invalidGm and not hasConflicts and #changes > 0
    ui.BeginDisabled(not canSave)
    if primary("Save policy##sm") and canSave then
      if manager.Save(active.revision, changes) then localMessage = nil
      else localMessage = "Could not send policy changes. Check the server connection and try again." end
    end
    ui.EndDisabled()
  else
    ui.BeginDisabled(#changes == 0 and not next(draft.newAddons))
    if ui.Button("Discard##sm") then draft.edits, draft.newAddons, conflicts, localMessage = {}, {}, {}, nil end
    ui.EndDisabled()
    ui.SameLine()
    ui.BeginDisabled(#changes == 0 or invalidGm)
    if primary("Review changes##sm") and #changes > 0 and not invalidGm then reviewing, localMessage = true, nil end
    ui.EndDisabled()
  end
  ui.EndDisabled()
  if footerTable then ui.EndTable() end
end

local opts = {
  keepTitle = true, rollup = false, resizable = { 380, 310 }, place = K.Centered,
  size = function(s)
    local io = ui.GetIO()
    return math.min(920, io.width / s - 24), math.min(650, io.height / s - 28)
  end,
  onClose = function()
    if active.saving then return end
    if next(draft.edits) then confirmClose = true
    else manager.Close() end
  end,
}

ui.window("Server Management", function()
  active = manager and API.session.InGame() and manager.View(serial or 0, catalogSerial or 0) or nil
  if not active then draft, serial, catalogSerial = nil, nil, nil return end
  if active.serial ~= serial then
    if active.reloaded and draft then
      draft, conflicts = M.rebase(draft, active, conflicts)
      reviewing = next(draft.edits) ~= nil
      results = nil
    else
      draft, conflicts = M.new(active, manager.CanonicalCommandName), {}
      filteredText, filteredOrigin, addPackage = nil, nil, ""
      reviewing = false
    end
    serial, minimumStatus = active.serial, active.minimum_status
    confirmClose, localMessage = false, nil
    ui.SetNextWindowFocus()
  elseif active.catalog_serial ~= catalogSerial then
    M.setCommands(draft, active.commands)
    results = nil
  end
  catalogSerial = active.catalog_serial
  opts.shown = true
  hud.Dialog(WINDOW, "Server Management", opts, body)
end)
