-- Experience transfer uses the server's approval for one exact pair of evolving records. Record IDs and the
-- approval revision remain strings: converting either to a Lua number would lose large uint64 values.
local API = hs.UI
local X = {}
local confirmation

function X.reset() confirmation = nil end

function X.back()
  if not confirmation then return false end
  confirmation = nil
  return true
end

local function find(items, id)
  for _, item in ipairs(items) do if item.id == id then return item end end
end

local function current(state)
  return confirmation and state.canTransfer and not state.pending
    and confirmation.source == state.source and confirmation.destination == state.destination
    and confirmation.revision == state.revision
    and state.source ~= state.destination and find(state.items, state.source) ~= nil
    and find(state.items, state.destination) ~= nil
end

function X.sync(state)
  if not state or not state.shown or not current(state) then confirmation = nil end
end

local function wrapped(text)
  ui.PushTextWrapPos(0)
  ui.Text(text)
  ui.PopTextWrapPos()
end

local function preview(caption, item)
  if not item then return end
  wrapped(string.format("%s after transfer: %s (level %d/%d, %g%% experience)",
    caption, item.name, item.level, item.maxLevel, item.progress))
end

local function details(state)
  if not state.hasDetails then return end
  wrapped("Compatibility: " .. tostring(state.compatibility) .. "%")
  wrapped("Max transfer level: " .. tostring(state.maxTransferLevel))
  preview("Source", state.previewSource)
  preview("Destination", state.previewDestination)
end

local function selector(state, role, caption, other, width, changed)
  local item = find(state.items, state[role])
  ui.Text(caption)
  ui.SetNextItemWidth(width)
  ui.BeginDisabled(state.pending)
  if ui.BeginCombo("##evolve-" .. role, item and item.name or "Choose an evolving item") then
    if ui.Selectable("None###evolve-" .. role .. "-none", state[role] == "") and not state.pending then
      changed.value = API.evolving.Select(role, "") or changed.value
    end
    for _, candidate in ipairs(state.items) do
      if candidate.id ~= other then
        if ui.Selectable(candidate.name .. "###evolve-" .. role .. "-" .. candidate.id,
            candidate.id == state[role]) and not state.pending then
          changed.value = API.evolving.Select(role, candidate.id) or changed.value
        end
      end
    end
    ui.EndCombo()
  end
  ui.EndDisabled()
  if item then
    wrapped(string.format("Level %d/%d   %g%% experience", item.level, item.maxLevel, item.progress))
  end
end

-- look.button(id, caption, enabled) lets each pack keep its own focusable button style.
function X.draw(state, look)
  X.sync(state)
  local source, destination = find(state.items, state.source), find(state.items, state.destination)
  if confirmation then
    ui.Text("Confirm experience transfer")
    ui.Separator()
    wrapped("Are you sure you want to transfer experience between these two items?")
    wrapped("Source: " .. source.name)
    wrapped("Destination: " .. destination.name)
    details(state)
    if look.button("##evolve-cancel", "Cancel", true) then confirmation = nil end
    if look.button("##evolve-confirm", "Confirm transfer", current(state) and source ~= nil and destination ~= nil) then
      local approved = confirmation
      confirmation = nil
      API.evolving.Confirm(approved.source, approved.destination, approved.revision)
    end
    return
  end

  local changed = { value = false }
  selector(state, "source", "Source item", state.destination, look.width, changed)
  ui.Dummy(0, look.gap)
  selector(state, "destination", "Destination item", state.source, look.width, changed)
  ui.Dummy(0, look.gap)
  if #state.items == 0 then wrapped("No evolving items are available in your possessions.") end
  if state.status ~= "" then wrapped(state.status) end
  details(state)
  local enabled = not changed.value and state.canTransfer and not state.pending and source ~= nil and destination ~= nil
  if look.button("##evolve-transfer", "Transfer", enabled) then
    confirmation = { source = state.source, destination = state.destination, revision = state.revision }
  end
end

return X
