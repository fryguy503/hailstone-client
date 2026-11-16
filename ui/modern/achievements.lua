local API = hs.UI
local hud = dofile("lib/hud.lua")
local T, C = hud.T, hud.T.color
local browser = dofile("lib/achievement_browser.lua").new({
  colors = {text=C.text,muted=C.muted,accent=C.gold,complete=C.pet,warning=C.danger or C.gold},
  heading = function(text)
    ui.PushFont(T.font.large); ui.TextWrapped(text); ui.PopFont()
  end,
  row = function(id, selected, width, height)
    local clicked = ui.Selectable("###" .. id, selected, 0, width, height)
    return clicked, ui.IsItemHovered() or ui.IsItemFocused()
  end,
  select = function(label, selected, width, height, span)
    return ui.Selectable(label, selected, span and ui.SelectableFlags.SpanAllColumns or 0, width or 0, height)
  end,
  button = function(label, enabled, width)
    ui.BeginDisabled(not enabled)
    local clicked = ui.Button(label, width or 0)
    ui.EndDisabled()
    return clicked
  end,
})
local function size(scale)
  local io = ui.GetIO()
  return math.min(1280, (io.width - 60) / scale), math.min(760, (io.height - 70) / scale)
end
-- (the dialogs' options and bodies are made once, not each frame)
local MAIN_OPTS = {shown=true,size=size,resizable={760,580},
  place=function(sw,sh,w,h) return (sw-w)/2,(sh-h)/2 end,
  onClose=function() browser.closeMain() end}
local mainState
local function mainBody(_,_,_,_,scale) browser.main(mainState,scale) end
ui.window("Achievements", function()
  if not API.session.InGame() then browser.reset(); return end
  API.windows.Replace("AchievementsWnd") -- every frame, hidden too: the achievement key toggles it only while claimed
  local state = browser.readState()
  if not state or not state.shown then return end
  mainState = state
  hud.Dialog("AchievementsWnd", "Achievements", MAIN_OPTS, mainBody)
  mainState = nil
end)
local DETAIL_OPTS = {shown=true,size=function() return 680,580 end,
  resizable={480,350},onClose=function() API.windows.Show("AchievementWnd",false) end}
local shownDetail
local function detailBody(_,_,_,_,scale) browser.detail(shownDetail,scale,false) end
ui.window("Achievement", function()
  if not API.session.InGame() then browser.reset(); return end
  local detail = browser.floating()
  if not detail then return end
  shownDetail = detail
  hud.Dialog("AchievementWnd", "Achievement", DETAIL_OPTS, detailBody)
  shownDetail = nil
end)
