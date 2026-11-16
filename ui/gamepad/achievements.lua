local API = hs.UI
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local T, pad = W.T, W.pad
local keyboard = K.new()
local C = T.C
local browser = dofile("lib/achievement_browser.lua").new({
  footerHeight = 110, detailFooter = 90,
  statusWidth = 145, pointsWidth = 65,
  filterPadding = 40, chevronWidth = 40,
  categoryWidth = 340, categoryHeight = 42, categoryArtScale = 1.2,
  colors = {text=C.text,muted=C.dim,accent=C.accent,complete=C.good or C.accent,warning=C.warn or C.accent},
  -- W.begin supplies the scaled body font. Changing a child's window scale
  -- through T.font would multiply its inherited scale at every nested pane.
  heading = function(text) ui.TextWrapped(text) end,
  row = function(id, selected, width, height)
    local clicked, focused = T.button("##" .. id, "", width, height, {primary=selected}, pad)
    return clicked, focused or ui.IsItemHovered()
  end,
  select = function(label, selected, width, height)
    local caption,id = label:match("^(.*)###(.*)$")
    return T.button("##"..(id or label),caption or label,width or ui.GetContentRegionAvail(),
      math.max(T.px(38),height or 0),{primary=selected,align=width and width<=T.px(44) and 0.5 or 0},pad)
  end,
  button = function(label, enabled, width)
    local caption,id = label:match("^(.*)###(.*)$")
    return T.button("##"..(id or label),caption or label,
      width or (ui.CalcTextSize(caption or label)+T.px(36)),T.px(42),{disabled=not enabled},pad)
  end,
  cancel = function() K.cancel(keyboard) end,
  keyboard = function(text) K.open(keyboard,"Find achievement",text,127) end,
  editKeyboard = function()
    if not keyboard.open then return nil,false end
    local x,y = ui.GetCursorScreenPos()
    local done = K.draw(keyboard,pad,x,y,ui.GetContentRegionAvail())
    if done=="ok" then local text=keyboard.text; K.cancel(keyboard); return text,false end
    if done=="cancel" then K.cancel(keyboard); return nil,false end
    return nil,true
  end,
})
ui.window("Achievements", function()
  if not API.session.InGame() then browser.reset(); return end
  local open = W.shown("AchievementsWnd") -- every frame, hidden too: the achievement key toggles it only while claimed
  local state = browser.readState()
  if not state or not state.shown or not open then K.cancel(keyboard); return end
  T.frame()
  local height = math.max(780, (ui.GetIO().height - 60) / T.S)
  if W.begin("Achievements",{w=1780,h=height,title="Achievements",
      hints=keyboard.open and K.HINTS or {{"a","Select / activate"},{"b","Close"}}}) then
    local x,y,_,_,width,height = W.area()
    ui.SetCursorScreenPos(x,y)
    ui.BeginChild("##achievementcontents",width,height,false)
    browser.main(state,T.S)
    ui.EndChild()
  end
  if W.finish() then
    if keyboard.open then K.cancel(keyboard) else browser.closeMain() end
  end
end)
ui.window("Achievement", function()
  if not API.session.InGame() then browser.reset(); return end
  local detail = browser.floating()
  if not detail or not W.shown("AchievementWnd") then return end
  if W.begin("Achievement",{w=1320,h=940,title="Achievement",hints={{"a","Activate"},{"b","Close"}}}) then
    local x,y,_,_,width,height = W.area()
    ui.SetCursorScreenPos(x,y)
    ui.BeginChild("##achievementdetailcontents",width,height,false)
    browser.detail(detail,T.S,false)
    ui.EndChild()
  end
  if W.finish() then API.windows.Show("AchievementWnd",false) end
end)
