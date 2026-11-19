local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")

local function centered(sw, sh, w, h)
  return (sw - w) / 2, (sh - h) / 2
end

local DETAILS = { "Stats", "Abilities", "Equipment" }
local detailColumns = {
  Stats = { { "Stat", 0 }, { "Value", 90 } },
  Abilities = { { "Ability", 0 }, { "Rank", 60 } },
  Equipment = { { "Item", 0 } },
}
local pickerOptions = {
  size = function() return 840, 510 end,
  resizable = { 600, 380 },
  place = centered,
  rollup = false,
}

local function picker(screen, title, accept, mission)
  local tab = DETAILS[1]
  local columns = { { "Category", 90 }, { "Form", 0 }, { "Lvl", 32 }, { "Availability", 85 } }
  if mission then columns[#columns + 1] = { "Assigned", 66 } end
  ui.window(title, function()
    if not API.session.InGame() then return end
    if not API.windows.Shown(screen) then tab = DETAILS[1]; return end
    hud.Dialog(screen, title, pickerOptions, function(_, _, _, _, s)
      local _, textH = ui.CalcTextSize("Ag")
      local actionH = textH + 12 * s
      local listWidth = math.floor(ui.GetContentRegionAvail() * (mission and 0.60 or 0.54))
      ui.BeginChild("##shroudCatalog", listWidth, -(actionH + 12 * s), false)
      if (API.windows.ListInfo(screen, "Forms") or 0) == 0 then K.Empty("The server offered no forms.")
      else
        W.List(screen, "Forms", 0, 0, { columns = columns })
      end
      ui.EndChild()
      ui.SameLine(0, 12 * s)
      ui.BeginChild("##shroudDetails", 0, -(actionH + 12 * s), false)
      ui.TextWrapped(W.Text(screen, "Description"))
      local status = W.Text(screen, "DetailStatus")
      if status ~= "" then K.Muted(status) end
      local requirement = W.Text(screen, "RoleRequirement")
      if requirement ~= "" then K.Muted(requirement) end
      ui.Separator()
      if ui.BeginTabBar("##shroudDetailTabs", 0) then
        for _, label in ipairs(DETAILS) do
          if ui.BeginTabItem(label) then tab = label; ui.EndTabItem() end
        end
        ui.EndTabBar()
      end
      if tab == "Abilities" then
        K.Muted("Select an ability to inspect it.")
        if W.Text(screen, "AbilityDescription") ~= "" then W.Stml(screen, "AbilityDescription", 0, 80 * s) end
      elseif tab == "Equipment" then
        K.Muted("Select an item to inspect it.")
      end
      W.List(screen, tab, 0, 0, { columns = detailColumns[tab] })
      ui.EndChild()
      ui.Separator()
      W.ButtonRow(screen, {
        { "Accept", accept, { primary = true } },
        { "Cancel", mission and "Close" or "Cancel" },
      }, actionH)
    end)
  end)
end

picker("ProgressionSelectionWnd", "Spirit Shroud Selection", "Select Shroud")
picker("TaskTemplateSelectWnd", "Monster Mission Selection", "Select Monster", true)

local PROGRESS = "ShroudProgressWnd"
local progressOptions = { size = function() return 510, 310 end, resizable = { 390, 240 }, place = centered }
ui.window("Spirit Shroud Progress", function()
  if not API.session.InGame() then return end
  hud.Dialog(PROGRESS, "Spirit Shroud Progress", progressOptions, function(_, _, _, _, s)
    W.List(PROGRESS, "Progress", 0, -99 * s, { columns = { { "Category", 0 }, { "Unlocked", 80 }, { "Next form", 150 } } })
    ui.Separator()
    ui.BeginChild("##progressDescription", 0, 0, false)
    local description = W.Text(PROGRESS, "Description")
    ui.TextWrapped(description ~= "" and description or "Choose a category to see its details.")
    ui.EndChild()
  end)
end)

local BANK = "AltStorageWnd"
local bankOptions = { size = function() return 415, 380 end, resizable = { 325, 280 }, place = centered }
ui.window("Shroud Bank", function()
  if not API.session.InGame() then return end
  hud.Dialog(BANK, "Shroud Bank", bankOptions, function(_, _, _, _, s)
    local count, timer = W.Text(BANK, "CountLabel"), W.Text(BANK, "TimerLabel")
    if count ~= "" then K.Muted("Items: " .. count) end
    if timer ~= "" then
      if count ~= "" then ui.SameLine(0, 12 * s) end
      K.Muted("Time remaining: " .. timer)
    end
    local status = W.Text(BANK, "Status")
    if status ~= "" then ui.TextWrapped(status) end
    W.List(BANK, "ItemList", 0, -44 * s, { columns = { { "Item", 0 }, { "Quantity", 65 } } })
    ui.Separator()
    W.ButtonRow(BANK, {
      { "GetButton", "Withdraw", { primary = true } }, { "InspectButton", "Inspect" }, { "DoneButton", "Done" },
    }, 29 * s)
  end)
end)
