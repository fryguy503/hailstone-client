-- Compact host-owned container; addon sections own their controls and settings.
local API=hs.UI
local E=dofile("lib/extensions.lua")
local M={}
local opened,focus=false,false
local life,owners,context=nil,nil,nil
local POPUP="##mapcontrols"
local synced={} -- the frame and view of the last Sync: Draw does not repeat the window's own Sync of this frame
local function value(v)return type(v)..":"..#tostring(v)..":"..tostring(v)end
local function lifecycle(c)
  return table.concat({value(c.policy.context),value(c.policy.source),value(c.zone),value(c.shown_zone),
    value(c.own_zone),value(c.view)},"|")
end
function M.Reset()
  opened,focus,life,owners=false,false,nil,nil
  synced.frame=nil
  E.Reset()
end
function M.Open()return opened end
function M.Sync(view)
  -- No add-on with map controls (a stock server): nothing to build or show.
  if not E.Used("MapViewWnd","controls") then M.Reset();return false end
  context=E.MapContextNow(view)
  local _,metadata=E.Rows("MapViewWnd","controls",context)
  metadata=metadata or {}
  local present,ids=false,{}
  local function inspect(item)
    if item.enabled then
      present=true;ids[#ids+1]=value(item.handle)..value(item.owner)
    end
  end
  for _,item in ipairs(metadata)do inspect(item)end
  for _,item in ipairs(metadata.sections or {})do inspect(item)end
  table.sort(ids)
  local now,registered=lifecycle(context),table.concat(ids,"|")
  if opened and (not present or life~=now or owners~=registered)then M.Reset()end
  synced.frame,synced.view=ui.GetFrameCount(),view
  return present
end
function M.Toggle(view)
  if not M.Sync(view)then return false end
  if opened then M.Reset();return false end
  -- Capture identity, not policy content: editing a filter must not close its panel.
  local _,metadata=E.Rows("MapViewWnd","controls",context)
  local ids={}
  for _,list in ipairs({metadata or {},metadata and metadata.sections or {}})do
    for _,item in ipairs(list)do if item.enabled then ids[#ids+1]=value(item.handle)..value(item.owner)end end
  end
  table.sort(ids);life,owners=lifecycle(context),table.concat(ids,"|")
  opened,focus=true,true
  ui.OpenPopup(POPUP)
  return true
end
function M.Draw(view,back)
  if synced.frame~=ui.GetFrameCount() or synced.view~=view then M.Sync(view)end
  if ui.BeginPopup(POPUP)then
    if not opened then ui.CloseCurrentPopup()
    elseif back then
      if E.HasReview()then E.Back("MapViewWnd","controls",context);focus=true
      else M.Reset();ui.CloseCurrentPopup()end
    else
      ui.TextDisabled("Map controls");ui.Separator()
      local reviewing=E.HasReview()
      if focus and not reviewing then ui.SetKeyboardFocusHere()end
      E.Draw("MapViewWnd","controls",context,focus)
      focus=not reviewing and E.HasReview()
      M.Sync(view)
      if not opened then ui.CloseCurrentPopup()end
    end
    ui.EndPopup()
  elseif opened then M.Reset()end -- Escape or an outside click closes only this container.
  return opened
end
return M
