-- Metadata reads never invoke action callbacks. Reviews live only in this presenter;
-- addon callbacks still validate authoritative roster identity and one-use tokens.
local API=hs.UI
local E={}
local pending,message=nil,nil
local supported
local function available() -- asked once: the catalog does not change while the interface runs
  if supported==nil then supported=(API.Supports and API.Supports("extensions","ListActions"))==true end
  return supported
end
local function value(v)
  local text=v==nil and "" or tostring(v)
  return type(v)..":"..#text..":"..text
end
-- value() of a string or an integer, kept: the raid context reads seven of them a row each frame, and the same names
-- and numbers come back frame after frame. Other values (a float's text tells 1 from 1.0) are made each time. The
-- table is dropped when it grows large (names come and go over a session).
local valueMemo,valueMemoSize={},0
local function memoValue(v)
  if type(v)~="string" and math.type(v)~="integer" then return value(v) end
  local hit=valueMemo[v]
  if hit then return hit end
  if valueMemoSize>=4096 then valueMemo,valueMemoSize={},0 end
  hit=value(v)
  valueMemo[v],valueMemoSize=hit,valueMemoSize+1
  return hit
end
local function signature(context)
  if context.window=="MapViewWnd" then return context.revision or "" end
  local selected=context.selection or {}
  return table.concat({value(context.revision),value(selected.name),value(selected.group),
    value(selected.level),value(selected.class),value(selected.list),value(selected.row)},"|")
end
-- Whether any add-on has an action or section at this point: read once a frame, so a window with none (every
-- window against a stock server) builds no context for it.
local registered={}
function E.Used(window,section)
  if not available() then return false end
  local frame,key=ui.GetTime(),window.."|"..section
  local seen=registered[key]
  if seen and seen.frame==frame then return seen.used end
  local actions=API.extensions.ListActions(window,section,{})
  local used=#actions>0 or #(actions.sections or {})>0
  registered[key]={frame=frame,used=used}
  return used
end
-- A context is built at most once a frame (both the presenter and its fresh() read it).
local built={}
local function once(key,build)
  local frame=ui.GetTime() -- the frame's time: the same throughout one
  local hit=built[key]
  if hit and hit.frame==frame then return hit.context end
  local context=build()
  built[key]={frame=frame,context=context}
  return context
end
local function raidContext()
  local context,revision={},{}
  -- Exact framed row contents are a presentation change hint, not an authority
  -- token: the addon owns stronger roster/incarnation checks at confirmation.
  for _,list in ipairs({"RAID_PlayerList","RAID_NotInGroupPlayerList"}) do
    local rows,_,selected=API.windows.ListInfo("RaidWindow",list)
    rows=rows or 0
    revision[#revision+1]=memoValue(list);revision[#revision+1]=memoValue(rows)
    for row=0,rows-1 do
      local data=API.windows.ListRow("RaidWindow",list,row)
      revision[#revision+1]=memoValue(row)
      for column=1,6 do revision[#revision+1]=memoValue(data and data[column]) end
      if row==selected and data and type(data[2])=="string" and data[2]~="" then
        context.selection={name=data[2],group=tonumber(data[1]),level=data[3],class=data[4],list=list,row=row}
      end
    end
  end
  context.revision=table.concat(revision,"|")
  return context
end
function E.RaidContext()
  if not E.Used("RaidWindow","member_actions") then return {} end
  return once("raid",raidContext)
end
-- Presentation equality only; callbacks still validate their current domain authority.
local function mapContext(view)
  local getter=API.map.Policy
  local policy=type(getter)=="function" and getter() or {}
  if type(policy)~="table" then policy={} end
  local _,zone=API.map.ZoneName()
  local _,shown,own=API.map.ZoneShown()
  local context={window="MapViewWnd",section="controls",view=view=="world" and "world" or "zone",
    zone=zone or "",shown_zone=shown or "",own_zone=own==true,policy=policy}
  local parts={value(context.view),value(context.zone),value(context.shown_zone),value(context.own_zone)}
  for _,key in ipairs({"context","source","revision","custom_spawns","stock_npcs","npc_find","name"})do
    parts[#parts+1]=value(policy[key])
  end
  context.revision=table.concat(parts,"|")
  return context
end
function E.MapContext(view)
  view=view=="world" and "world" or "zone"
  return once("map:"..view,function() return mapContext(view) end)
end
-- Built now, for a check that must see a change made earlier in the same frame (the map controls' lifecycle).
function E.MapContextNow(view) return mapContext(view=="world" and "world" or "zone") end
-- live: built now, not the frame's copy (an action rechecks the context it acts on).
local function fresh(window,section,context,live)
  if window=="RaidWindow" and section=="member_actions" then return live and raidContext() or E.RaidContext() end
  if window=="MapViewWnd" and section=="controls" then
    local view=context and context.view
    return live and mapContext(view=="world" and "world" or "zone") or E.MapContext(view)
  end
  return context or {}
end
local function find(actions,handle)
  for _,action in ipairs(actions)do if action.handle==handle then return action end end
end
local function sync(window,section,context,actions)
  if message then
    local current=find(actions,message.action.handle)
    if message.window~=window or message.section~=section or not current or not current.enabled or
        current.owner~=message.action.owner or message.signature~=signature(context) then message=nil end
  end
  if not pending then return end
  local current=find(actions,pending.action.handle)
  if pending.window~=window or pending.section~=section or not current or not current.enabled or
      current.owner~=pending.action.owner or not current.requiresReview or pending.signature~=signature(context) then
    pending,message=nil,nil -- stale paint never calls invoke, confirm or cancel
  end
end
local function dispatch(action,phase,context,token)
  local result=API.extensions.InvokeAction(action.handle,phase,context,token or "")
  if type(result)~="table" or type(result.ok)~="boolean" or
      result.message~=nil and type(result.message)~="string" or
      result.reviewToken~=nil and type(result.reviewToken)~="string" then
    return {ok=false,message="The extension returned an invalid response."}
  end
  return result
end
function E.Reset() pending,message=nil,nil end
function E.HasReview() return pending~=nil end
function E.Before(window,section,context)
  if available() then API.extensions.DrawSection(window,section,"before",context) end
end
function E.After(window,section,context)
  if available() then API.extensions.DrawSection(window,section,"after",context) end
end
function E.Rows(window,section,context)
  if not available() then E.Reset();return {} end
  context=context or {}
  local actions=API.extensions.ListActions(window,section,context)
  sync(window,section,context,actions)
  local rows={}
  for _,action in ipairs(actions)do
    rows[#rows+1]={id="##extension:"..action.handle, label=action.label,enabled=action.enabled==true,
      kind="action",action=action,window=window,section=section,signature=signature(context)}
  end
  if pending then
    for _,kind in ipairs({"confirm","cancel"})do
      rows[#rows+1]={id="##extension:"..kind..":"..pending.action.handle,label=kind=="confirm" and "Confirm" or "Cancel",
        enabled=true,kind=kind,action=pending.action,window=window,section=section,review=pending,signature=pending.signature}
    end
  end
  return rows,actions
end
function E.Message()
  local note=pending and pending.message or message and message.text
  if note and note~="" then ui.Separator();ui.TextWrapped(note) end
end
function E.Activate(row,context)
  if not available() then E.Reset();return false end
  context=fresh(row.window,row.section,context,true)
  local actions=API.extensions.ListActions(row.window,row.section,context)
  sync(row.window,row.section,context,actions)
  local current=find(actions,row.action.handle)
  if not current or not current.enabled or current.owner~=row.action.owner or signature(context)~=row.signature then
    E.Reset();return false
  end
  local phase,token
  if row.kind=="action" then
    phase=current.requiresReview and "review" or "invoke"
    pending=nil
  else
    if row.review~=pending then return false end
    phase,token=row.kind,pending.token
    pending=nil -- explicit activation consumes this presenter's token before dispatch
  end
  local result=dispatch(current,phase,context,token)
  message={text=result.message,action=current,window=row.window,section=row.section,signature=signature(context)}
  if phase=="review" and result.ok and type(result.reviewToken)=="string" and result.reviewToken~="" and
      type(result.message)=="string" and result.message~="" then
    pending={action=current,window=row.window,section=row.section,token=result.reviewToken,message=result.message,signature=signature(context)}
    return true,"review"
  end
  return true,phase=="review" and "failed" or phase
end
function E.Back(window,section,context)
  if not pending then return false end
  local rows=E.Rows(window,section,fresh(window,section,context))
  for _,row in ipairs(rows)do if row.kind=="cancel" then E.Activate(row,context);return true end end
  return true
end
local function wrappedLabel(label,width)
  if ui.CalcTextSize(label)<=width then return label end
  local lines,line={},""
  -- Keep long action names readable even in a narrow child, without shortening them.
  for word in label:gmatch("%S+")do
    local nextLine=line=="" and word or line.." "..word
    if line~="" and ui.CalcTextSize(nextLine)>width then lines[#lines+1]=line;line="" end
    if ui.CalcTextSize(word)>width then
      for character in word:gmatch("[%z\1-\127\194-\244][\128-\191]*")do
        if line~="" and ui.CalcTextSize(line..character)>width then lines[#lines+1]=line;line="" end
        line=line..character
      end
    else
      line=line=="" and word or line.." "..word
    end
  end
  if line~="" then lines[#lines+1]=line end
  return table.concat(lines,"\n")
end
function E.Draw(window,section,context,focus)
  if not E.Used(window,section) then E.Reset();return end
  context=fresh(window,section,context)
  E.Before(window,section,context)
  local rows=E.Rows(window,section,context)
  if #rows>0 then
    ui.Spacing();ui.Separator();ui.Spacing()
    ui.TextDisabled("Addon actions");ui.Spacing()
  end
  E.Message()
  local width=math.max(1,(ui.GetContentRegionAvail()))
  local _,textHeight=ui.CalcTextSize("Ag")
  local scale=textHeight/14
  local gap,padX,padY=8*scale,10*scale,5*scale
  ui.PushStyleVar(ui.StyleVar.FramePadding,padX,padY)
  ui.PushStyleVar(ui.StyleVar.ItemSpacing,gap,6*scale)
  local used,review=0,false
  for _,row in ipairs(rows)do
    if row.kind~="action" and not review then used,review=0,true;ui.Spacing() end
    local label=wrappedLabel(row.label,math.max(1,width-2*padX))
    local buttonWidth=math.min(width,ui.CalcTextSize(label)+2*padX)
    if used>0 and used+gap+buttonWidth<=width then ui.SameLine(0,gap);used=used+gap
    else used=0 end
    ui.PushID(row.id);ui.BeginDisabled(not row.enabled)
    if focus and row.enabled and ((pending and row.kind=="cancel") or (not pending and row.kind=="action")) then
      ui.SetKeyboardFocusHere();focus=false
    end
    -- The hidden ID is stable when resizing changes the visible line breaks.
    local clicked=ui.Button(label.."###action",buttonWidth)
    ui.EndDisabled()
    if clicked and row.enabled then E.Activate(row,context) end
    ui.PopID()
    used=used+buttonWidth
  end
  ui.PopStyleVar(2)
  E.After(window,section,context)
end
-- The default closure stays inside the interface; addon replacements receive plain context only.
-- An empty successful replacement hides the component. Revocation/errors restore its default.
function E.Component(window,id,context,drawDefault)
  local v=API.version or {}
  -- Earlier hosts reject new slot IDs even for before/after; do not probe them by drawing.
  if not available() or v.major~=1 or (v.minor or 0)<5 then return drawDefault() end
  E.Before(window,id,context)
  local replaced=API.extensions.DrawSection(window,id,"replace",context)==true
  local result
  if not replaced then result=table.pack(drawDefault()) end
  E.After(window,id,context)
  if result then return table.unpack(result,1,result.n) end
end
return E
