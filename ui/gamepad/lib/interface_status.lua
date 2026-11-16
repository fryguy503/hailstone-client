-- Presentation data for the Options interface status section. The host distinguishes the saved selection from
-- the interface actually running while a requested switch is pending or needs a fallback.
return function(status, width)
  local rows = {}
  local function add(label, value)
    if value == nil or value == "" then return end
    value = tostring(value)
    if not width then
      rows[#rows + 1] = { label, value }
      return
    end
    -- Controller rows have a fixed height. Split complete messages into readable rows instead of losing their
    -- ends to fitting; the mouse interface uses wrapped text in its value column.
    for line in (value .. "\n"):gmatch("(.-)\n") do
      repeat
        local cut = #line
        if cut > width then
          cut = line:sub(1, width):match("^.*()%s") or width
          if cut < width / 2 then cut = width end
          while cut > 0 and line:byte(cut + 1) and line:byte(cut + 1) >= 128 and line:byte(cut + 1) < 192 do
            cut = cut - 1
          end
        end
        rows[#rows + 1] = { label, line:sub(1, cut) }
        label, line = "", line:sub(cut + 1):gsub("^%s+", "")
      until line == ""
    end
  end
  local function title(pack)
    if not pack or not pack.id or pack.id == "" then return "None" end
    return pack.name and pack.name ~= "" and pack.name or pack.id
  end
  local selected, loaded = status.selected or {}, status.loaded or {}
  add("Selected UI", title(selected))
  add("Loaded UI", title(loaded))
  add("Interface ID", loaded.id)
  local states = { loading = "Loading", ready = "Ready", pending = "Waiting to load",
    fallback = "Using fallback", error = "Load error" }
  add("Load state", states[status.state] or status.state or "Unavailable")
  add("Source", loaded.id and (loaded.source == "addon" and ("Addon: " .. (loaded.addon or "")) or "Built in"))
  if selected.available == false then add("Unavailable", selected.reason) end
  if status.bases and #status.bases > 0 then add("Based on", table.concat(status.bases, " > ")) end
  if status.scriptCount ~= nil then
    add("Scripts", string.format("%d / %d loaded; %d errors", status.loadedScriptCount or 0,
      status.scriptCount, status.errorCount or 0))
  end
  if status.windowCount ~= nil then add("Lua windows", status.windowCount) end
  for i, message in ipairs(status.diagnostics or {}) do add("Details " .. i, message) end
  return rows
end
