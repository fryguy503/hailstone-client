local API = hs.UI
local Profiles = API.layoutProfiles
if not Profiles or not Profiles.List then return end

local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local Visual = dofile("lib/layout_preview.lua")
local T, pad = W.T, W.pad
local WINDOW = "LayoutProfilesWindow"
local HINTS = { { "a", "Select" }, { "b", "Close" } }
local PREVIEW_HINTS = { { "a", "Close Preview" }, { "b", "Back" } }
local keyboard = K.new()
local names, list, state, side = {}, {}, {}, {}
local preview, selected, status
local pendingApply, visual
local opened = false

local function selectProfile(i)
  selected, list.selected = names[i], i
  preview, status = Profiles.Preview(selected)
end

local function refresh()
  names = Profiles.List() or {}
  for i, saved in ipairs(names) do
    if saved == selected then selectProfile(i); return end
  end
  selected, preview, list.selected = nil, nil, nil
  if names[1] then selectProfile(1) end
end

local function save(text, replace)
  local ok, err, reason = Profiles.Save(text, replace)
  if ok then
    selected = text
    refresh()
    if list.selected then list.offset = math.max(0, list.selected - 1) end
    status = "Saved " .. (selected or text) .. "."
  elseif reason == "exists" and not replace then
    state.confirm = { "Replace “" .. text .. "” with your current setup?", function() save(text, true) end }
  else
    status = err or "The profile could not be saved."
  end
end

local function apply(target, discardUnsaved)
  local ok, err, reason = Profiles.Apply(target, discardUnsaved)
  if reason == "unsaved" and not discardUnsaved then
    pendingApply = { target = target, name = "", focus = true }
  else
    status = ok and ("Applied " .. target .. ".") or (err or "The profile could not be applied.")
  end
end

local function closeApplyPrompt()
  pendingApply = nil
  list.focusNext = 1
end

local function finishApply(request)
  closeApplyPrompt()
  apply(request.target, true)
end

local function saveBeforeApply(request, replace)
  if request.name:lower() == request.target:lower() then
    request.error = "Choose a different name from the profile you are applying."
    return
  end
  local ok, err, reason = Profiles.Save(request.name, replace)
  if ok then
    names, list.selected = Profiles.List() or {}, nil
    for i, saved in ipairs(names) do
      if saved == selected then list.selected = i; break end
    end
    finishApply(request)
  elseif reason == "exists" and not replace then
    request.error = nil
    state.confirm = { "Replace “" .. request.name .. "” with your current layout?", function() saveBeforeApply(request, true) end }
  else
    request.error = err or "The current layout could not be saved."
  end
end

local function drawSaveBeforeApply(request, x, y, width)
  ui.SetCursorScreenPos(x, y)
  T.heading("Save Current Layout")
  ui.TextWrapped("Save the current layout before applying “" .. request.target .. "”?")
  if request.focus then
    ui.SetKeyboardFocusHere()
    request.focus = false
  end
  local bh = T.px(54)
  local label = request.name == "" and "Enter Profile Name" or request.name
  if T.button("##save-before-name", label, width, bh, {}, pad) then
    K.open(keyboard, "Profile name", request.name, 48, request.name == "")
  end
  if T.button("##save-before-apply", "Save & Apply", width, bh,
      { primary = true, disabled = request.name:match("^%s*$") ~= nil }, pad) then
    saveBeforeApply(request, false)
  end
  if T.button("##apply-without-saving", "Apply Without Saving", width, bh, {}, pad) then finishApply(request) end
  if T.button("##cancel-layout-apply", "Cancel", width, bh, {}, pad) then closeApplyPrompt() end
  if request.error then ui.TextWrapped(request.error) end
end

local function closeVisualPreview()
  visual = nil
  side.want, side.at = 2, ui.GetTime()
end

local function drawVisualPreview(x, y, bottom, width)
  ui.SetCursorScreenPos(x, y)
  T.heading("Visual Preview")
  ui.TextWrapped(visual.name)
  ui.TextWrapped("Read-only view of the saved window positions.")
  local _, top = ui.GetCursorScreenPos()
  local bh = T.px(54)
  local height = bottom - top - bh - T.px(10)
  local C = T.C
  Visual.draw(visual, width, height, C.sunken, C.edge, C.accent, C.text)
  ui.SetCursorScreenPos(x, bottom - bh)
  if visual.opening then
    ui.SetKeyboardFocusHere()
    visual.opening = nil
  end
  if T.button("##close-visual-preview", "Close Preview", width, bh, {}, pad) then closeVisualPreview() end
end

local function drawProfiles(x, y, right, bottom, width)
  local gap, rowH, bh = T.px(24), 52, T.px(54)
  local leftW = math.floor((width - gap) * 0.46)
  local rightX, rightW = x + leftW + gap, width - leftW - gap
  ui.SetCursorScreenPos(x, y)
  T.heading("Saved profiles")
  local listY = select(2, ui.GetCursorScreenPos())
  local visible = math.max(1, math.floor((bottom - listY + T.px(10)) / T.px(rowH + 10)))
  list.shown = visible
  local row
  if #names > 0 then
    row = W.list("##profile-list", list, #names, visible, rowH, leftW, function(i, x0, y0, x1, y1)
      T.textIn(x0 + T.px(16), y0, x1 - T.px(16), y1, names[i], T.C.text, 0)
    end, selectProfile, function(i) return names[i] end)
  else
    ui.PushTextWrapPos(rightX - gap - ui.GetWindowPos())
    ui.TextWrapped("No profiles saved for this interface.")
    ui.PopTextWrapPos()
  end
  W.toSide(side, row)

  ui.SetCursorScreenPos(rightX, y)
  W.sideBefore(side, 1)
  if T.button("##profile-save", "Save Current Setup", rightW, bh, {}, pad) then
    K.open(keyboard, "Profile name", "", 48, true)
  end
  W.sideAfter(side, 1, 4, list)
  W.sideBefore(side, 2)
  if T.button("##profile-visual-preview", "Visual Preview", rightW, bh, { disabled = not preview }, pad) then
    visual = Visual.prepare(preview)
    visual.opening = true
  end
  W.sideAfter(side, 2, 4, list)
  W.sideBefore(side, 3)
  if T.button("##profile-apply", "Apply Profile", rightW, bh, { primary = true, disabled = not preview }, pad) then
    apply(selected, false)
  end
  W.sideAfter(side, 3, 4, list)
  W.sideBefore(side, 4)
  if T.button("##profile-undo", "Undo Apply", rightW, bh, { disabled = not Profiles.CanUndo() }, pad) then
    local ok, err = Profiles.Undo()
    status = ok and "Restored the previous setup." or (err or "The previous setup could not be restored.")
  end
  W.sideAfter(side, 4, 4, list)

  ui.PushTextWrapPos(right - ui.GetWindowPos())
  if preview then
    ui.Dummy(0, T.px(8))
    T.heading("Preview")
    ui.TextWrapped(preview.name)
    if preview.width > 0 and preview.height > 0 then
      ui.TextWrapped(string.format("Saved at %d × %d", preview.width, preview.height))
      local io = ui.GetIO()
      if math.abs(io.width - preview.width) >= 1 or math.abs(io.height - preview.height) >= 1 then
        ui.TextWrapped("This profile was saved at a different window size.")
      end
    end
    ui.TextWrapped(string.format("%d saved window layouts, %d chat settings", preview.windows, preview.chat))
    ui.TextWrapped("Chat includes tabs, filters and colors.")
  end
  if status and status ~= "" then ui.TextWrapped(status) end
  ui.PopTextWrapPos()
end

ui.window("Layout Profiles", function()
  if not W.shown(WINDOW) then
    if opened then
      names, list, state, side = {}, {}, {}, {}
      preview, selected, status, pendingApply, visual = nil, nil, nil, nil, nil
      K.cancel(keyboard)
    end
    opened = false
    return
  end
  if not opened then refresh(); opened = true end
  if W.begin("Layout Profiles", { w = 1260, h = 820, title = "Layout Profiles",
        hints = keyboard.open and K.HINTS or (visual and PREVIEW_HINTS or HINTS) }) then
    local x, y, right, bottom, width = W.area()
    if keyboard.open then
      if K.draw(keyboard, pad, x, y, width) == "ok" then
        if pendingApply then
          pendingApply.name, pendingApply.error = keyboard.text, nil
        else
          save(keyboard.text, false)
        end
      end
    elseif not W.confirm(state) then
      if pendingApply then drawSaveBeforeApply(pendingApply, x, y, width)
      elseif visual then drawVisualPreview(x, y, bottom, width)
      else drawProfiles(x, y, right, bottom, width) end
    end
  end
  if W.back(W.finish(), state, keyboard, list) then
    if pendingApply then closeApplyPrompt()
    elseif visual then closeVisualPreview()
    else API.windows.Show(WINDOW, false) end
  end
end)
