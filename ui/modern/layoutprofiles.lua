local API = hs.UI
local Profiles = API.layoutProfiles
if not Profiles or not Profiles.List then return end

local hud = dofile("lib/hud.lua")
local K = dofile("lib/dialogs_kit.lua")
local Visual = dofile("lib/layout_preview.lua")
local C = hud.T.color
local WINDOW = "LayoutProfilesWindow"
local OPTS = {
  size = function() return 446, 328 end,
  resizable = { 326, 295 },
  place = K.Centered,
}

local names, selected, preview = {}, nil, nil
local name, status, overwrite = "", "", nil
local pendingApply, visual
local opened = false

local function selectProfile(picked)
  selected = picked
  local err
  preview, err = Profiles.Preview(picked)
  status = err or ""
end

local function refresh()
  names = Profiles.List() or {}
  for _, saved in ipairs(names) do
    if saved == selected then selectProfile(saved); return end
  end
  selected, preview = nil, nil
  if names[1] then selectProfile(names[1]) end
end

local function save(text, replace)
  local ok, err, reason = Profiles.Save(text, replace)
  if ok then
    selected, name = text, ""
    refresh()
    status = "Saved " .. (selected or text) .. "."
  elseif reason == "exists" and not replace then
    overwrite = text
    ui.OpenPopup("##replace-layout-profile")
  else
    status = err or "The profile could not be saved."
  end
end

local function apply(target, discardUnsaved)
  local ok, err, reason = Profiles.Apply(target, discardUnsaved)
  if reason == "unsaved" and not discardUnsaved then
    pendingApply = { target = target, name = name }
    ui.OpenPopup("##save-before-layout-apply")
  else
    status = ok and ("Applied " .. target .. ".") or (err or "The profile could not be applied.")
  end
end

local function finishApply(request)
  pendingApply = nil
  ui.CloseCurrentPopup()
  apply(request.target, true)
end

local function saveBeforeApply(request, replace)
  if request.name:lower() == request.target:lower() then
    request.error = "Choose a different name from the profile you are applying."
    return
  end
  local ok, err, reason = Profiles.Save(request.name, replace)
  if ok then
    names = Profiles.List() or {}
    name = ""
    finishApply(request)
  elseif reason == "exists" and not replace then
    request.replace = true
    request.error = nil
  else
    request.replace = false
    request.error = err or "The current layout could not be saved."
  end
end

local function drawSaveBeforeApply(s, buttonHeight)
  if not ui.BeginPopup("##save-before-layout-apply") then
    pendingApply = nil
    return
  end
  local request = pendingApply
  if request then
    K.Caption("Save Current Layout")
    ui.TextWrapped("Save the current layout before applying “" .. request.target .. "”?")
    if request.replace then
      ui.TextWrapped("Replace “" .. request.name .. "” with your current layout?")
      if ui.Button("Replace & Apply", 130 * s, buttonHeight) then saveBeforeApply(request, true) end
      ui.SameLine(0, 8 * s)
      if ui.Button("Use Another Name", 140 * s, buttonHeight) then request.replace = false end
    else
      ui.SetNextItemWidth(280 * s)
      local entered
      request.name, entered = ui.InputTextWithHint("##save-before-name", "Profile name", request.name, 49,
        ui.InputTextFlags.EnterReturnsTrue)
      local blank = request.name:match("^%s*$") ~= nil
      ui.BeginDisabled(blank)
      local clicked = ui.Button("Save & Apply", 112 * s, buttonHeight)
      ui.EndDisabled()
      if not blank and (clicked or entered) then saveBeforeApply(request, false) end
      ui.SameLine(0, 8 * s)
      if ui.Button("Apply Without Saving", 162 * s, buttonHeight) then finishApply(request) end
    end
    ui.SameLine(0, 8 * s)
    if ui.Button("Cancel", 70 * s, buttonHeight) then
      pendingApply = nil
      ui.CloseCurrentPopup()
    end
    if request.error then ui.TextWrapped(request.error) end
  end
  ui.EndPopup()
end

local function drawVisualPreview(s, buttonHeight)
  if not visual then return end
  local io = ui.GetIO()
  local width = math.max(1, math.min(840 * s, io.width - 40 * s))
  local height = math.max(1, math.min(480 * s, io.height - 190 * s))
  if visual.opening then
    local totalHeight = (#visual.boxes > 0 and height or 0) + 125 * s
    ui.SetNextWindowPos(math.max(0, (io.width - width) * 0.5 - 8 * s),
      math.max(0, (io.height - totalHeight) * 0.5), ui.Cond.Appearing)
    visual.opening = nil
  end
  if not ui.BeginPopup("##visual-layout-preview") then
    visual = nil
    return
  end
  ui.PushTextWrapPos(ui.GetCursorPos() + width)
  K.Caption("Visual Preview")
  ui.TextWrapped(visual.name)
  ui.TextWrapped("Read-only view of the saved window positions.")
  Visual.draw(visual, width, height, C.inset, C.frameBorder, C.accent, C.text)
  ui.PopTextWrapPos()
  if ui.Button("Close Preview", 120 * s, buttonHeight) then
    visual = nil
    ui.CloseCurrentPopup()
  end
  ui.EndPopup()
end

local function draw(_, _, _, _, s)
  K.Caption("Save current setup")
  local buttonHeight = K.ButtonH(s)
  local available = ui.GetContentRegionAvail()
  ui.SetNextItemWidth(math.max(1, available - 92 * s))
  local entered
  name, entered = ui.InputTextWithHint("##profile-name", "Profile name", name, 49, ui.InputTextFlags.EnterReturnsTrue)
  ui.SameLine(0, 8 * s)
  local blank = name:match("^%s*$") ~= nil
  ui.BeginDisabled(blank)
  local clicked = ui.Button("Save", 84 * s, buttonHeight)
  ui.EndDisabled()
  if not blank and (clicked or entered) then save(name, false) end

  ui.Dummy(0, 8 * s)
  K.Caption("Saved profiles")
  if #names == 0 then
    ui.TextDisabled("No profiles saved for this interface.")
  else
    ui.SetNextItemWidth(-1)
    if ui.BeginCombo("##saved-profile", preview and preview.name or selected or "Select a profile") then
      for i, saved in ipairs(names) do
        if ui.Selectable(saved .. "##layout" .. i, saved == selected) then selectProfile(saved) end
      end
      ui.EndCombo()
    end
  end
  if preview then
    ui.Dummy(0, 6 * s)
    K.Caption("Preview")
    if preview.width > 0 and preview.height > 0 then
      ui.Text(string.format("Saved at %d × %d", preview.width, preview.height))
      local io = ui.GetIO()
      if math.abs(io.width - preview.width) >= 1 or math.abs(io.height - preview.height) >= 1 then
        ui.TextWrapped("This profile was saved at a different window size.")
      end
    end
    ui.TextWrapped(string.format("%d saved window layouts, %d chat settings", preview.windows, preview.chat))
    ui.TextWrapped("Chat includes tabs, filters and colors.")
  end

  ui.Dummy(0, 8 * s)
  local actionWidth = math.max(1, (ui.GetContentRegionAvail() - 16 * s) / 3)
  ui.BeginDisabled(not preview)
  if ui.Button("Visual Preview", actionWidth, buttonHeight) then
    visual = Visual.prepare(preview)
    visual.opening = true
    ui.OpenPopup("##visual-layout-preview")
  end
  ui.SameLine(0, 8 * s)
  if ui.Button("Apply Profile", actionWidth, buttonHeight) then
    apply(selected, false)
  end
  ui.EndDisabled()
  ui.SameLine(0, 8 * s)
  ui.BeginDisabled(not Profiles.CanUndo())
  if ui.Button("Undo Apply", actionWidth, buttonHeight) then
    local ok, err = Profiles.Undo()
    status = ok and "Restored the previous setup." or (err or "The previous setup could not be restored.")
  end
  ui.EndDisabled()
  if status ~= "" then ui.TextWrapped(status) end

  if ui.BeginPopup("##replace-layout-profile") then
    ui.TextWrapped("Replace “" .. (overwrite or "") .. "” with your current setup?")
    if ui.Button("Cancel", 100 * s, buttonHeight) then
      overwrite = nil
      ui.CloseCurrentPopup()
    end
    ui.SameLine(0, 8 * s)
    if ui.Button("Replace", 100 * s, buttonHeight) then
      if overwrite then save(overwrite, true) end
      overwrite = nil
      ui.CloseCurrentPopup()
    end
    ui.EndPopup()
  end
  drawSaveBeforeApply(s, buttonHeight)
  drawVisualPreview(s, buttonHeight)
end

ui.window("Layout Profiles", function()
  if not API.session.InGame() or not API.windows.Shown(WINDOW) then
    if opened then
      names, selected, preview, name, status, overwrite, pendingApply, visual = {}, nil, nil, "", "", nil, nil, nil
    end
    opened = false
    return
  end
  if not opened then refresh(); opened = true end
  hud.Dialog(WINDOW, "Layout Profiles", OPTS, draw)
end)
