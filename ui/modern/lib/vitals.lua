-- Readout choices for the Modern Group and Pet frames.
local API = hs.UI
local T = dofile("lib/theme.lua")
local M = {}
local CHOICES = {
  { "none", "None" }, { "percent", "Percentage" }, { "value", "Value" }, { "both", "Both" },
}
local MODES = { none = true, percent = true, value = true, both = true }
local EXTRA_PETS = { [2] = "pet2", [3] = "pet3" }
local PERCENT, VALUES = {}, {}

function M.Mode(key, default)
  local mode = T.Setting(key)
  return MODES[mode] and mode or default
end

function M.Menu(label, key, default)
  if ui.BeginMenu(label) then
    local mode = M.Mode(key, default)
    for _, choice in ipairs(CHOICES) do
      if ui.MenuItem(choice[2], mode == choice[1]) then ui.SetSetting(key, choice[1]) end
    end
    ui.EndMenu()
  end
end

local function valueText(key, current, max)
  if current == nil or max == nil then return nil end
  -- One cache entry per gauge or extra pet, refreshed only when its numbers change.
  local entry = VALUES[key]
  if not entry or entry.current ~= current or entry.max ~= max then
    entry = { current = current, max = max, text = T.Thousands(current) .. " / " .. T.Thousands(max) }
    VALUES[key] = entry
  end
  return entry.text
end

function M.Read(mode, gauge, fraction, petIndex)
  if mode == "none" then return nil, nil end
  local requestExact = mode ~= "percent"
  local current, max, percent
  if type(API.character.GaugeValues) == "function" then
    current, max, percent = API.character.GaugeValues(gauge, petIndex, requestExact)
  elseif fraction ~= nil then
    percent = math.floor(fraction * 100 + 0.5)
  end
  local value = requestExact and valueText(EXTRA_PETS[petIndex] or gauge, current, max) or nil
  if mode == "value" or percent == nil then return value, nil end
  local pct = PERCENT[percent]
  if not pct then
    pct = percent .. "%"
    PERCENT[percent] = pct
  end
  return value, pct
end

return M
