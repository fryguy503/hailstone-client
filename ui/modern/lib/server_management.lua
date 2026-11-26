-- Draft values are kept by cfg key. Editing one field leaves every other field on the server alone.
local M = {}

local groupLabels = {
  non_rof2 = "Extended commands",
  movement_automation = "Movement automation",
  command_helpers = "Command helpers",
  addon_commands = "Add-on commands",
}
M.groupLabels = groupLabels

function M.trim(text)
  return text:match("^%s*(.-)%s*$")
end

function M.boolean(value)
  if value == "on" or value == "yes" or value == "true" or value == "1" then return true end
  if value == "off" or value == "no" or value == "false" or value == "0" then return false end
end

function M.setCommands(draft, commands)
  local aliases = {}
  for _, command in ipairs(commands) do
    aliases[command.name:lower()] = command.name
    for _, alias in ipairs(command.aliases) do aliases[alias:lower()] = command.name end
  end
  draft.commands, draft.aliases = commands, aliases
end

function M.new(view, resolveCommand)
  local draft = { saved = {}, edits = {}, newAddons = {}, resolveCommand = resolveCommand, resolvedCommands = {} }
  for _, field in ipairs(view.fields) do
    draft.saved[field.section .. "." .. field.key] = field.value
  end
  M.setCommands(draft, view.commands)
  return draft
end

function M.rebase(draft, view, previousConflicts)
  local fresh = M.new(view, draft.resolveCommand)
  local conflicts = {}
  for id, value in pairs(draft.edits) do
    local current = fresh.saved[id]
    if current ~= value then
      fresh.edits[id] = value
      if draft.saved[id] ~= current or previousConflicts[id] then conflicts[id] = true end
    end
  end
  fresh.newAddons = draft.newAddons
  return fresh, conflicts
end

function M.get(draft, id, fallback)
  return draft.edits[id] or draft.saved[id] or fallback or ""
end

function M.set(draft, id, value, fallback)
  if value == (draft.saved[id] or fallback or "") then draft.edits[id] = nil
  else draft.edits[id] = value end
end

function M.changes(draft)
  local result = {}
  for id, value in pairs(draft.edits) do
    local section, key = id:match("^([^.]+)%.(.+)$")
    result[#result + 1] = { section = section, key = key, value = value, previous = draft.saved[id], id = id }
  end
  table.sort(result, function(a, b) return a.id < b.id end)
  return result
end

local function tokens(text, commaOnly)
  local list = {}
  for item in text:gmatch(commaOnly and "[^,]+" or "[^,%s]+") do
    local clean = M.trim(item)
    if clean ~= "" then list[#list + 1] = clean end
  end
  return list
end

local function has(list, match)
  for _, token in ipairs(list) do if match(token) then return true end end
  return false
end

-- Preserve the spelling and order of unrelated list entries, including names from newer clients.
local function listUpdate(draft, id, commaOnly, match, add, replace)
  local before = tokens(M.get(draft, id), commaOnly)
  local saved = tokens(draft.saved[id] or "", commaOnly)
  local after, found = {}, false
  for _, item in ipairs(before) do
    if match(item) then
      if add and not found then after[#after + 1] = replace and replace(item) or add end
      found = true
    else after[#after + 1] = item end
  end
  if add and not found then after[#after + 1] = add end
  if #before == #after and table.concat(before, "\n") == table.concat(after, "\n") then return end
  local nextValue = table.concat(after, ", ")
  if nextValue == table.concat(saved, ", ") then draft.edits[id] = nil
  else M.set(draft, id, nextValue) end
end

local function commandName(draft, name)
  name = name:lower()
  if draft.aliases[name] then return draft.aliases[name] end
  if name ~= ":" and name ~= "'" and name:sub(1, 1) ~= "/" then name = "/" .. name end
  if draft.aliases[name] then return draft.aliases[name] end
  local resolved = draft.resolvedCommands[name]
  if not resolved then
    resolved = (draft.resolveCommand and draft.resolveCommand(name)) or name
    draft.resolvedCommands[name] = resolved
  end
  return resolved
end

local DEFAULT_GM_MINIMUM = 200

local function gmEntry(token)
  -- Use the last colon so the stock ":" alias can also be written as "::200".
  local name, status = token:match("^(.+):([^:]*)$")
  return name or token, status or ""
end

local function gmMinimum(status)
  local number = status:match("^[0-9]+$") and tonumber(status)
  if number and number >= 1 and number <= 32767 then return number end
end

function M.validCommandGmList(value, original)
  local retained = {}
  for _, token in ipairs(tokens(original or "")) do retained[token] = (retained[token] or 0) + 1 end
  for _, token in ipairs(tokens(value)) do
    local name, status = gmEntry(token)
    if name == "/" or not gmMinimum(status) then
      if (retained[token] or 0) == 0 then return false end
      retained[token] = retained[token] - 1
    end
  end
  return true
end

function M.commandRules(draft)
  local state = { commands = {}, groups = {}, gm = {}, default = M.get(draft, "commands.default", "allow"):lower() }
  for _, rule in ipairs({ "allow", "block" }) do
    for _, token in ipairs(tokens(M.get(draft, "commands." .. rule))) do state.commands[commandName(draft, token)] = rule end
    for _, token in ipairs(tokens(M.get(draft, "commands." .. rule .. "_groups"))) do state.groups[token:lower()] = rule end
  end
  for _, token in ipairs(tokens(M.get(draft, "commands.gm"))) do
    local name, status = gmEntry(token)
    local canonical, minimum = commandName(draft, name), gmMinimum(status)
    local previous = state.gm[canonical]
    if not previous or (previous.minimum and (not minimum or minimum > previous.minimum)) then
      state.gm[canonical] = { status = status, minimum = minimum }
    end
    if state.commands[canonical] ~= "block" then state.commands[canonical] = "gm" end
  end
  return state
end

function M.commandEffective(state, command)
  local direct = state.commands[command.name]
  if direct == "gm" then
    local minimum = state.gm[command.name].minimum
    return direct, minimum and "Minimum account status required." or "This GM rule needs a valid minimum status.", minimum
  end
  if direct then return direct, "Explicit command rule" end
  for _, rule in ipairs({ "block", "allow" }) do
    for _, group in ipairs(command.groups) do
      if state.groups[group] == rule then return rule, groupLabels[group] or group end
    end
  end
  return state.default == "deny" and "block" or "allow", "Default policy"
end

local coreCommands = { ["/camp"] = true, ["/quit"] = true, ["/exit"] = true, ["/servermanagement"] = true }

function M.restrictedCoreCommands(draft, state)
  local restricted = {}
  for _, command in ipairs(draft.commands) do
    if coreCommands[command.name] and M.commandEffective(state, command) ~= "allow" then
      restricted[#restricted + 1] = command.name
    end
  end
  return restricted
end

local function keepSpelling(item) return item end

function M.setCommandRule(draft, name, rule, group, minimum)
  local canonical = group and name or commandName(draft, name)
  local match = group and function(item) return item:lower() == canonical end
    or function(item) return commandName(draft, item) == canonical end
  for _, policy in ipairs({ "allow", "block" }) do
    listUpdate(draft, "commands." .. policy .. (group and "_groups" or ""), false, match,
      rule == policy and canonical or nil, keepSpelling)
  end
  if not group then
    local suffix = ":" .. tostring(minimum or DEFAULT_GM_MINIMUM)
    listUpdate(draft, "commands.gm", false, function(item)
      local command = gmEntry(item)
      return commandName(draft, command) == canonical
    end, rule == "gm" and canonical .. suffix or nil, function(item)
      local spelling = gmEntry(item)
      return spelling .. suffix
    end)
  end
end

function M.filterCommands(draft, origin, pattern)
  local result = {}
  pattern = M.trim(pattern):lower()
  for _, command in ipairs(draft.commands) do
    if command.origin == origin then
      local matches = command.name:lower():find(pattern, 1, true) or command.description:lower():find(pattern, 1, true)
      if matches or has(command.aliases, function(alias) return alias:lower():find(pattern, 1, true) end) then
        result[#result + 1] = command
      end
    end
  end
  return result
end

function M.addonIdentity(text, required)
  text = M.trim(text)
  local minimum
  if required then
    local identity, version = text:match("^(.*[^:]):([^:/]+)$")
    if identity and identity:find("/", 1, true) then text, minimum = M.trim(identity), M.trim(version) end
  end
  text = text:lower()
  if text:match("^https?://") or text:find("github.com/", 1, true) == 1 or text:find("www.github.com/", 1, true) == 1 then
    local path = text:gsub("^https?://", ""):gsub("^www%.", "")
    text = path:match("^github%.com/([^/]+/[^/?#]+)") or ""
  else text = text:match("^[^@#]+") or "" end
  local owner, repo = text:match("^([%w-]+)/([%w_.-]+)$")
  if owner and #owner <= 39 and #repo <= 100 and not owner:match("^%-") and not owner:match("%-$")
      and not owner:find("--", 1, true) and repo ~= "." and repo ~= ".." then
    if #repo > 4 then repo = repo:gsub("%.git$", "") end
    return owner .. "/" .. repo, minimum
  end
end

local function versionNumber(text)
  if #text > 10 or not text:match("^[0-9]+$") or (#text > 1 and text:sub(1, 1) == "0") then return nil end
  local n = tonumber(text)
  if n and n <= 4294967295 then return n end
end

local function versionIdentifiers(text, pre)
  if text == "" or text:sub(1, 1) == "." or text:sub(-1) == "." or text:find("..", 1, true) then return nil end
  local result = {}
  for field in text:gmatch("[^.]+") do
    if not field:match("^[0-9A-Za-z-]+$") then return nil end
    if pre and field:match("^[0-9]+$") then
      local number = versionNumber(field)
      if not number then return nil end
      result[#result + 1] = number
    else result[#result + 1] = field end
  end
  return result
end

local function versionParts(text)
  text = M.trim(text):gsub("^[vV]", "")
  local core, build = text:match("^(.-)%+(.*)$")
  if core then
    if not versionIdentifiers(build, false) then return nil end
    text = core
  end
  local pre
  core, pre = text:match("^(.-)%-(.*)$")
  local major, minor, patch = (core or text):match("^([0-9]+)%.([0-9]+)%.([0-9]+)$")
  if not major then return nil end
  major, minor, patch = versionNumber(major), versionNumber(minor), versionNumber(patch)
  if not (major and minor and patch) then return nil end
  local identifiers = pre and versionIdentifiers(pre, true) or nil
  if pre and not identifiers then return nil end
  return { major, minor, patch }, identifiers or {}
end

-- Match the client's SemVer precedence for repeated pins. Keep the winning authored text, including build data.
local function higherMinimum(candidate, current)
  local a, ap = versionParts(candidate)
  local b, bp = versionParts(current)
  if not a then return false end
  if not b then return true end
  for i = 1, 3 do if a[i] ~= b[i] then return a[i] > b[i] end end
  if #ap == 0 or #bp == 0 then return #ap == 0 and #bp ~= 0 end
  for i = 1, math.min(#ap, #bp) do
    local x, y = ap[i], bp[i]
    if x ~= y then
      if type(x) ~= type(y) then return type(x) == "string" end
      if type(x) == "number" then return x > y end
      -- SemVer compares ASCII bytes regardless of the Lua runtime's active locale.
      for n = 1, math.min(#x, #y) do
        if x:byte(n) ~= y:byte(n) then return x:byte(n) > y:byte(n) end
      end
      return #x > #y
    end
  end
  return #ap > #bp
end

function M.addons(draft)
  local rows, index = {}, {}
  for id, source in pairs(draft.newAddons) do
    index[id] = { id = id, source = source }
  end
  for _, key in ipairs({ "suggest", "allow", "required", "block" }) do
    for _, token in ipairs(tokens(M.get(draft, "addons." .. key), key ~= "suggest")) do
      local id, minimum = M.addonIdentity(token, key == "required")
      if id then
        local row = index[id] or { id = id }
        index[id] = row
        if key == "suggest" then
          row.suggested, row.source = true, token
        else
          row.mode = key
          if minimum and (not row.minimum or higherMinimum(minimum, row.minimum)) then row.minimum = minimum end
        end
      end
    end
  end
  for _, row in pairs(index) do rows[#rows + 1] = row end
  table.sort(rows, function(a, b) return a.id < b.id end)
  return rows, index
end

function M.setAddonMode(draft, addon, mode, minimum)
  for _, key in ipairs({ "allow", "required", "block" }) do
    local token = addon.id
    if key == "required" and minimum and minimum ~= "" then token = token .. ":" .. M.trim(minimum) end
    listUpdate(draft, "addons." .. key, true, function(item) return M.addonIdentity(item, key == "required") == addon.id end,
      mode == key and token or nil)
  end
end

function M.setAddonMinimum(draft, addon, minimum)
  minimum = M.trim(minimum)
  local token = minimum == "" and addon.id or addon.id .. ":" .. minimum
  listUpdate(draft, "addons.required", true, function(item) return M.addonIdentity(item, true) == addon.id end, token)
end

function M.suggestAddon(draft, addon, on)
  listUpdate(draft, "addons.suggest", false, function(item) return M.addonIdentity(item) == addon.id end,
    on and (addon.source or addon.id) or nil)
end

return M
