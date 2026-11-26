local V = {}
local character = hs.UI.character

local choices = {
  { "none", "None" }, { "percent", "Percentage" }, { "value", "Value" }, { "both", "Both" },
}
local settings = {
  { "Group HP readout", "group.hpDisplay", "percent" },
  { "Group mana readout", "group.manaDisplay", "none" },
  { "Group endurance readout", "group.enduranceDisplay", "none" },
  { "Group pet HP readout", "group.petHpDisplay", "none" },
  { "Pet HP readout", "pet.hpDisplay", "percent" },
}

local function modeIndex(setting)
  local value = ui.GetSetting(setting[2], setting[3])
  for i, choice in ipairs(choices) do
    if choice[1] == value then return i end
  end
  return setting[3] == "percent" and 2 or 1
end

function V.modes()
  return choices[modeIndex(settings[1])][1], choices[modeIndex(settings[2])][1],
    choices[modeIndex(settings[3])][1], choices[modeIndex(settings[4])][1]
end

function V.petMode()
  return choices[modeIndex(settings[5])][1]
end

function V.addOptions(rows)
  for _, setting in ipairs(settings) do
    local i = modeIndex(setting)
    rows[#rows + 1] = { setting[1], choices[i][2], function(dir)
      ui.SetSetting(setting[2], choices[(i - 1 + dir) % #choices + 1][1])
    end }
  end
  rows[#rows + 1] = { note = "Current / maximum values require server support.", key = "vitals:server" }
end

function V.text(mode, gauge, petIndex)
  if mode == "none" then return "" end
  local exact = mode == "value" or mode == "both"
  local current, maximum, percent = character.GaugeValues(gauge, petIndex, exact)
  local value = exact and current and (current .. " / " .. maximum) or ""
  local pct = mode ~= "value" and percent and (percent .. "%") or ""
  if value ~= "" and pct ~= "" then return value .. "  " .. pct end
  return value .. pct
end

return V
