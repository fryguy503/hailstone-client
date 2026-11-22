-- Actual map presenters and extension helpers over strict, deterministic host/input surfaces.
-- Display callbacks are counted separately from explicit action dispatch. No server addon is loaded.
CLIENT_ROOT=CLIENT_ROOT or(arg and arg[1])or"."
local originalDofile=dofile
local checks=0
local function check(value,why)checks=checks+1;assert(value,why or"check failed")end
local function equal(a,b,why)check(a==b,(why or"values differ")..": "..tostring(a).." / "..tostring(b))end
local function strict(value,name)return setmetatable(value,{__index=function(_,key)error("Unexpected "..name.."."..tostring(key),2)end})end
local function nop()end

local function rig(pack)
 local s={frame=0,time=20,x=0,y=0,depth=0,disabled={},ids={},styles=0,windows={},actions={},mapActions={},displayCalls={},
  settings={["map.sidebar"]="0",["map.sideTab"]="markers",["map.view"]="zone",["map.labels"]="1",["map.showMarkers"]="1"},
  writes={},buttons={},popupButtons={},texts={},pad={},shown=true,inGame=true,allowed=true,present=true,apiAvailable=true,
  withActions=true,withSections=true,sectionsEnabled=true,generation=1,owner="plugin:map-fixture",zone="alpha",shownZone="alpha",own=true,
  zoneEntry=1,playerX=5,playerY=8,
  policy={custom_spawns=true,stock_npcs=false,npc_find=false,name="Map provider",revision="provider:1",context="context:1",source="source:1"},
  autoZ=true,pathShown=false,geometry={},focus=nil,popup=nil,popupDepth=0,io={fps=60,mouseDX=0,mouseDY=0,wheel=0,alt=false},
  mapPacks={"Default"},mapPack=0,combos={}}
 local function draw()check(s.depth>0,"Draw outside a presenter or popup")end
 local function mapAction(name,...)s.mapActions[#s.mapActions+1]={name,...};return true end
 local function isDisabled()for _,value in ipairs(s.disabled)do if value then return true end end;return false end
 local function identity(id)return table.concat(s.ids,"/").."|"..id end
 local function item(id,label,navInput)
  draw();local key=identity(id);s.last=key;s.lastRaw=id
  local entry={id=key,raw=id,label=label or id}
  s.buttons[#s.buttons+1]=entry
  if s.popupDepth>0 then s.popupButtons[#s.popupButtons+1]=entry end
  if s.requestFocus then s.focusQueued=key;s.requestFocus=false end
  return not isDisabled() and (s.click==id or s.click==label or s.click==key
   or ((s.pad.a or (navInput and s.pad.y)) and s.focus==key))
 end
 local map={
  Policy=function()local snapshot={};for key,value in pairs(s.policy)do snapshot[key]=value end;return snapshot end,
  ZoneName=function(withEntry)
   if not s.inGame then return "","" end
   if withEntry then return "Current zone",s.zone,s.zoneEntry end
   return "Current zone",s.zone
  end,
  ZoneShown=function()return"Shown zone",s.shownZone,s.own end,
  ShowZone=function(short)s.shownZone=short or s.zone;s.own=short==nil or short==s.zone;return mapAction("ShowZone",short)end,
  AutoZ=function(value)if value~=nil then s.autoZ=value;mapAction("AutoZ",value)end;return s.autoZ end,
  FindPathShown=function()return s.pathShown end,
  DrawZone=function(x0,y0,x1,y1,zoom,cx,cy,labels)
   draw();s.geometry[#s.geometry+1]={zone=s.shownZone,zoom=zoom,cx=cx,cy=cy,labels=labels,x0=x0,y0=y0,x1=x1,y1=y1}
   if s.noMap then return nil end
   if s.own then return 1,0,0,s.playerX,s.playerY end
   return 1,0,0
  end,
 }
 local function checkPackCombo(screen,id)
  equal(screen,"MapViewWnd");equal(id,"MVW_MapsDirCombobox")
 end
 local windows={Shown=function(screen)check(screen=="MapViewWnd","Only map visibility is queried");return s.shown end,
  Show=function(screen,value)if screen=="MapViewWnd"then s.shown=value end;return mapAction("ShowWindow",screen,value)end,
  ComboInfo=function(screen,id)checkPackCombo(screen,id);return s.mapPacks,s.mapPack end,
  ComboSelect=function(screen,id,index)
   checkPackCombo(screen,id);check(s.mapPacks[index+1],"Map pack index is zero-based")
   s.mapPack=index;return mapAction("ComboSelect",index)
  end}
 local find={Open=function(value)if value~=nil then s.findOpen=value;mapAction("FindOpen",value)end;return s.findOpen or false end,
  MapLocation=function(x,y)return mapAction("MapLocation",x,y)end,
  End=function()s.pathShown=false;return mapAction("End")end}
 local api={version={major=1,minor=5},map=strict(map,"API.map"),windows=strict(windows,"API.windows"),find=strict(find,"API.find"),
  session=strict({InGame=function()return s.inGame end},"API.session")}
 local function content()
  local result={sections={}}
  if not s.present then return result end
  if s.withActions then
   for _,row in ipairs({{"inspect","Inspect map",false},{"change","Change map setting",true}})do
    result[#result+1]={handle="extension:"..s.generation..":"..row[1],owner=s.owner,id=row[1],label=row[2],
      enabled=s.allowed,requiresReview=row[3],requiresSelection=false,priority=0}
   end
  end
  if s.withSections then
   for _,placement in ipairs({"before","after"})do
    result.sections[#result.sections+1]={handle="extension:"..s.generation..":"..placement,owner=s.owner,id=placement,
      placement=placement,enabled=s.allowed and s.sectionsEnabled,priority=0}
   end
  end
  return result
 end
 api.extensions=strict({
  ListActions=function(window,slot,context)
   -- The API 1.4 registry cannot contain arbitrary new Map/toolbar slots.
   if s.legacySlots then return{}end
   equal(window,"MapViewWnd");equal(slot,"controls");check(type(context)=="table");s.lastListed=context;return content()
  end,
  DrawSection=function(window,slot,placement,context)
   s.sectionCalls=(s.sectionCalls or 0)+1
   if s.legacySlots and not(window=="RaidWindow"and slot=="member_actions")then
    error("Unknown declared display point")
   end
   equal(window,"MapViewWnd");check(type(slot)=="string"and slot~="");check(type(context)=="table");draw()
   s.sectionVisits=s.sectionVisits or{};s.sectionVisits[#s.sectionVisits+1]=placement
   if placement=="replace"and s.replaceSlot==slot then
    if s.customReplacement then ui.Text("Custom map canvas")end
    return true
   end
   local count=0
   for _,section in ipairs(slot=="controls"and content().sections or{})do
    if section.enabled and section.placement==placement then count=count+1 end
   end
   if count>0 then s.displayCalls[#s.displayCalls+1]={placement=placement,context=context}end
   if placement=="replace"then return count>0 end;return count
  end,
  InvokeAction=function(handle,phase,context,token)
   check(s.allowed and s.present,"Disabled/removed actions never dispatch")
   s.actions[#s.actions+1]={handle=handle,phase=phase,context=context,token=token}
   if s.badResponse then return{ok="yes",message="invalid"}end
   if phase=="review"then return{ok=true,message="Review the map change",reviewToken="review-token"}end
   return{ok=true,message="Handled"}
  end,
 },"API.extensions")
 api.Supports=function(module,member)
  if module=="extensions"and not s.apiAvailable then return false end
  return rawget(api,module)~=nil and(not member or rawget(api[module],member)~=nil)
 end
 hs={UI=strict(api,"API")}
 ui={Mouse={Left=0,Right=1},ButtonFlags={EnableNav=1},StyleVar={FramePadding=1,ItemSpacing=2},WindowFlags={AlwaysAutoResize=1,NoSavedSettings=2},
  Cond={Always=1,Appearing=2},InputTextFlags={EnterReturnsTrue=1,AutoSelectAll=2}}
 ui.window=function(name,body)s.windows[name]=body end
 ui.GetFrameCount=function()return s.frame end;ui.GetTime=function()return s.time end
 ui.GetSetting=function(key,fallback)return s.settings[key]or fallback or""end
 ui.SetSetting=function(key,value)s.settings[key]=tostring(value);s.writes[#s.writes+1]={key,value};return true end
 ui.GetCursorScreenPos=function()return s.x,s.y end;ui.SetCursorScreenPos=function(x,y)s.x,s.y=x,y end
 ui.SetCursorPos=ui.SetCursorScreenPos;ui.GetWindowPos=function()return 0,0 end
 ui.GetContentRegionAvail=function()return 480,520 end
 ui.CalcTextSize=function(text)return #tostring(text)*7,14 end
 ui.GetIO=function()return s.io end;ui.GetMousePos=function()return 400,240 end
 ui.IsItemHovered=function()return s.hover==s.lastRaw end;ui.IsItemActive=function()return s.active==s.lastRaw end
 ui.IsItemFocused=function()return s.focus==s.last end
 ui.IsWindowFocused=function()return true end
 ui.IsMouseDragging=function()return s.drag==true end
 ui.IsMouseClicked=function(button)return button==s.mouseClick end
 ui.IsMouseDoubleClicked=function()return false end;ui.IsMouseReleased=function()return false end
 ui.IsMouseDown=function()return false end;ui.IsItemClicked=function()return false end
 ui.IsMouseHoveringRect=function()return false end
 ui.IsWindowAppearing=function()return s.popupAppearing==true end
 ui.SetKeyboardFocusHere=function(offset)if offset==-1 then s.focusQueued=s.last else s.requestFocus=true end end
 ui.Button=function(label)return item(label,label:match("^(.-)###")or label)end
 ui.SmallButton=ui.Button;ui.InvisibleButton=function(id)return item(id,id)end
 ui.PushID=function(id)s.ids[#s.ids+1]=tostring(id)end
 ui.PopID=function()check(#s.ids>0);table.remove(s.ids)end
 ui.PushStyleVar=function()s.styles=s.styles+1 end
 ui.PopStyleVar=function(count)s.styles=s.styles-(count or 1);check(s.styles>=0)end
 ui.BeginDisabled=function(value)s.disabled[#s.disabled+1]=value end
 ui.EndDisabled=function()check(#s.disabled>0);table.remove(s.disabled)end
 ui.OpenPopup=function(id)s.popup=id;s.popupAppearing=true;s.nativeFocusInit=true end
 ui.IsPopupOpen=function(id)return s.popup==id end
 ui.BeginPopup=function(id)
  if s.popup~=id then return false end
  s.depth=s.depth+1;s.popupDepth=s.popupDepth+1
  if s.nativeFocusInit then s.requestFocus=true;s.nativeFocusInit=false end
  return true
 end
 ui.EndPopup=function()s.depth=s.depth-1;s.popupDepth=s.popupDepth-1;check(s.popupDepth>=0)end
 ui.CloseCurrentPopup=function()s.popup=nil end
 ui.BeginCombo=function(id,preview)
  s.combos[id]=preview
  if item(id,id:match("^(.-)##")or id,true)then
   ui.OpenPopup(id);s.comboPopup=id
  end
  return ui.BeginPopup(id)
 end
 ui.EndCombo=ui.EndPopup
 ui.Selectable=function(label,selected)
  local clicked=item(label,label:match("^(.-)##")or label,true)
  s.buttons[#s.buttons].selected=selected
  if clicked then
   s.focusQueued=identity(s.comboPopup);s.comboPopup=nil
   ui.CloseCurrentPopup()
  end
  return clicked
 end
 ui.SetItemDefaultFocus=function()
  if s.comboPopup and s.popupAppearing then s.focusQueued=s.last end
 end
 ui.BeginChild=function()draw();s.depth=s.depth+1;return true end
 ui.EndChild=function()s.depth=s.depth-1 end
 ui.BeginMenu=function()return false end;ui.EndMenu=nop
 ui.MenuItem=function(label)return s.menuClick==label end
 ui.InputText=function(_,text)return text,false end
 ui.Text=function(value)draw();s.texts[tostring(value)]=true end;ui.TextDisabled=ui.Text;ui.TextWrapped=ui.Text
 ui.TextColored=ui.Text;ui.Separator=ui.Text;ui.Dummy=ui.Text
 ui.Spacing=draw
 ui.SameLine=nop;ui.NewLine=nop;ui.SetNextItemWidth=nop;ui.SetNextWindowSize=nop;ui.SetNextWindowPos=nop
 ui.SetTooltip=nop;ui.PushClipRect=nop;ui.PopClipRect=nop;ui.PushFont=nop;ui.PopFont=nop
 for _,name in ipairs({"DrawRectFilled","DrawRect","DrawCircleFilled","DrawCircle","DrawLine","DrawTriangleFilled"})do ui[name]=draw end
 ui=strict(ui,"ui")
 local color={text={1,1,1},muted={.6,.6,.6},faint={.4,.4,.4},accent={0,1,1},ink={0,0,0},mapBg={.1,.1,.1},
  dim={.5,.5,.5},edge={.3,.3,.3},raised={.2,.2,.2},sunken={.1,.1,.1},onAccent={0,0,0}}
 local theme={color=strict(color,"theme.color"),C=strict(color,"theme.C"),S=1,px=function(value)return value end,
  TextSize=ui.CalcTextSize,Text=function(_,_,value)ui.Text(value)end,Fit=function(value)return value end,Hit=function(id)return item(id,id)end,
  Button=function(id,_,_,_,_,_,label)return item(id,label)end,TakeRightClick=nop,
  rect=draw,outline=draw,draw=draw,unfont=nop,font=pack=="modern"and{small=1,tiny=1,normal=1}or nop}
 local pad={connected=function()return true end,axis=function(key)return s.axes and s.axes[key]or 0 end,
  menuPressed=function(key)return s.pad[key]or false end,navVisible=function()return true end,
  focused=function()return ui.IsItemFocused()end,yActivated=function(clicked)return clicked and s.pad.y==true end}
 local window={shown=function()return s.inGame and s.shown end,
  begin=function(_,opts)s.beginOpts=opts;s.depth=s.depth+1;return true end,
  area=function()return 0,0,1100,650 end,focused=function()return true end,
  pressed=function(key)return s.pad[key]or false end,
  finish=function()s.depth=s.depth-1;return s.beginOpts.close~=false and s.pad.b or false end}
 local hud={T=theme,AutoZControls=nop,Dialog=function(_,_,_,body)s.depth=s.depth+1;body(0,0,1100,650,1);s.depth=s.depth-1 end}
 local atlas={zoom=1,Opened=nop,ResetView=nop,Show=nop,DrawCrumbs=nop,DrawFinder=nop,DrawRouteBanner=nop,DrawExits=nop,
  DrawView=function(_,_,_,_,_,viewZone)
   draw()
   if s.atlasZone then viewZone(s.atlasZone);s.atlasZone=nil end
  end}
 local FL={state={},CATS={},SHORT={},rows=function()return{}end,marks=function()return false end,
  policy=function()return s.policy.stock_npcs~=false,s.policy.npc_find~=false end,
  current=function()return nil end,selected=function()return nil end}
 local E,MC
 GPSTATE={drawn={},closeAsked={},padSettleUntil=0}
 dofile=function(path)
  if path=="lib/extensions.lua"then E=E or originalDofile(CLIENT_ROOT.."/ui/"..pack.."/"..path);return E end
  if path=="lib/map_controls.lua"then MC=MC or originalDofile(CLIENT_ROOT.."/ui/"..pack.."/"..path);return MC end
  local helpers={["lib/theme.lua"]=theme,["lib/hud.lua"]=hud,["lib/window.lua"]=window,["lib/pad.lua"]=pad,
    ["lib/atlas.lua"]=atlas,["lib/find.lua"]=FL}
  return assert(helpers[path],"Unexpected helper "..path)
 end
 s.api=api;s.extensions=dofile("lib/extensions.lua");s.controls=dofile("lib/map_controls.lua")
 originalDofile(CLIENT_ROOT.."/ui/"..pack.."/map.lua")
 function s.draw(input,click)
  s.frame=s.frame+1;s.time=s.time+.016;s.pad=input or{};s.click=click;s.buttons={};s.popupButtons={};s.texts={};s.x,s.y=0,0
  s.combos={}
  if s.comboPopup and s.pad.b then
   -- Native gamepad cancel closes the combo before the map presenter draws.
   s.focus=identity(s.comboPopup);s.comboPopup=nil;s.popup=nil
  end
  if s.popup and(s.pad.down or s.pad.up)and s.previousPopupButtons then
   local at=1
   for i,row in ipairs(s.previousPopupButtons)do if row.id==s.focus then at=i;break end end
   at=math.max(1,math.min(#s.previousPopupButtons,at+(s.pad.down and 1 or -1)))
   s.focus=s.previousPopupButtons[at]and s.previousPopupButtons[at].id
  end
  assert(s.windows.Map,"Map presenter registered")()
  if s.focusQueued then s.focus=s.focusQueued;s.focusQueued=nil end
  s.previousPopupButtons=s.popupButtons;s.popupAppearing=false
  equal(s.depth,0,"Window/popup/child scopes balance");equal(s.popupDepth,0);equal(#s.disabled,0);equal(#s.ids,0);equal(s.styles,0)
 end
 function s.helperDraw(view,back)
  s.depth=1;s.controls.Draw(view or"zone",back);s.depth=0;equal(s.styles,0)
 end
 function s.button(label)
  for _,row in ipairs(s.buttons)do if row.label==label then return row end end
 end
 function s.focusLabel(label)
  for _=1,12 do
   local at,target=1,nil
   for i,row in ipairs(s.previousPopupButtons or{})do
    if row.id==s.focus then at=i;if row.label==label then return end end
    if row.label==label then target=i end
   end
   check(target~=nil,"Popup has a focusable "..label.." row")
   s.draw({[target>at and "down"or"up"]=true});s.draw()
  end
  error("D-pad could not reach "..label)
 end
 function s.restore()dofile=originalDofile end
 return s
end

for _,pack in ipairs({"modern","gamepad"})do
 local s=rig(pack);local E,MC=s.extensions,s.controls
 local context=E.MapContext("zone")
 equal(context.window,"MapViewWnd");equal(context.section,"controls");equal(context.zone,"alpha")
 equal(context.shown_zone,"alpha");check(context.own_zone);equal(context.view,"zone")
 check(context.policy~=s.policy,"Policy context is copied");context.policy.name="Local edit"
 equal(s.policy.name,"Map provider")
 check(MC.Sync("zone"));equal(#s.actions,0);equal(#s.displayCalls,0,"Sync is metadata-only")
 s.draw();equal(#s.actions,0);equal(#s.displayCalls,0,"Closed controls do not paint addon sections")
 MC.Toggle("zone");check(MC.Open());s.draw();check(#s.displayCalls>0);equal(#s.actions,0)
 s.draw({},"Inspect map");equal(#s.actions,1);equal(s.actions[1].phase,"invoke")
 s.draw({},"Change map setting");equal(s.actions[2].phase,"review")
 s.draw();equal(#s.actions,2,"Review paint never confirms")
 s.draw({},"Confirm");equal(s.actions[3].phase,"confirm");equal(s.actions[3].token,"review-token")
 s.draw({},"Confirm");equal(#s.actions,3,"Consumed confirmation is not reusable")
 s.draw({},"Change map setting");local before=#s.actions
 s.policy.revision="provider:2";s.draw();check(MC.Open(),"Content revision alone keeps the panel open")
 check(not E.HasReview());equal(#s.actions,before,"Revision retirement never cancels implicitly")
 local rows=E.Rows("MapViewWnd","controls",E.MapContext("zone"))
 s.policy.context="context:2";check(not E.Activate(rows[1],context));equal(#s.actions,before,"Activation rereads current map context")
 MC.Sync("zone");check(not MC.Open(),"Context replacement closes locally")
 for _,change in ipairs({function()s.zone="beta"end,function()s.shownZone="other";s.own=false end,function()s.policy.source="source:2"end,
    function()s.owner="plugin:replacement"end,function()s.generation=s.generation+1 end,function()s.present=false end})do
  s.present=true;MC.Toggle("zone");check(MC.Open());change();before=#s.actions;MC.Sync("zone")
  check(not MC.Open(),"Lifecycle/content ownership change closes the panel");equal(#s.actions,before)
 end
 s.present=true;MC.Toggle("zone");check(MC.Open());MC.Sync("world");check(not MC.Open(),"View changes close stale controls")
 s.withActions=false;s.withSections=true;s.sectionsEnabled=true;check(MC.Sync("zone"),"Display-only sections expose controls")
 MC.Toggle("zone");s.draw();check(MC.Open());equal(#s.actions,before)
 s.sectionsEnabled=false;check(not MC.Sync("zone"));check(not MC.Open(),"Disabled-only sections cannot keep an empty panel")
 s.withActions=true;s.sectionsEnabled=true;s.allowed=true;MC.Toggle("zone");s.draw({},"Change map setting")
 before=#s.actions;s.allowed=false;s.draw({},"Confirm");equal(#s.actions,before);check(not E.HasReview(),"Permission loss discards pending review")
 s.allowed=true;check(MC.Sync("zone"));check(not MC.Open(),"Permission restoration does not reopen controls implicitly")
 MC.Toggle("zone");s.draw({},"Change map setting");s.draw({},"Cancel");equal(s.actions[#s.actions].phase,"cancel")
 MC.Reset();check(not MC.Open());s.restore()
end

-- The same helper composes arbitrary source slots, preserves default returns,
-- and accepts a successful empty replacement without inventing another host API.
for _,pack in ipairs({"modern","gamepad"})do
 local s=rig(pack);local E=s.extensions;local defaults=0
 local function original()defaults=defaults+1;return 9,nil,"kept"end
 s.depth=1;s.sectionVisits={}
 local values=table.pack(E.Component("MapViewWnd","future.widget",E.MapContext("zone"),original))
 equal(values.n,3);equal(values[1],9);equal(values[2],nil);equal(values[3],"kept");equal(defaults,1)
 equal(table.concat(s.sectionVisits,","),"before,replace,after")
 s.replaceSlot="future.widget";s.sectionVisits={}
 values=table.pack(E.Component("MapViewWnd","future.widget",E.MapContext("zone"),original))
 equal(values.n,0,"Successful empty replacement hides the default");equal(defaults,1)
 equal(table.concat(s.sectionVisits,","),"before,replace,after")
 s.replaceSlot=nil;s.sectionVisits={};E.Component("MapViewWnd","future.widget",E.MapContext("zone"),original)
 equal(defaults,2,"A revoked/failed replacement restores default behavior")
 local apiIndex=getmetatable(s.api).__index
 -- Version is a known optional field; all other absent APIs remain strict errors.
 getmetatable(s.api).__index=function(t,key)if key=="version"then return nil end;return apiIndex(t,key)end
 s.legacySlots=true;s.replaceSlot="future.widget"
 for _,case in ipairs({{name="API 1.4",version={major=1,minor=4}},
   {name="unknown major",version={major=2,minor=99}},{name="missing version"}})do
  s.api.version=case.version;s.sectionVisits={};local called=s.sectionCalls or 0;local expected=defaults+1
  values=table.pack(E.Component("MapViewWnd","future.widget",E.MapContext("zone"),original))
  equal(defaults,expected,case.name.." retains its original component")
  equal(values.n,3);equal(values[1],9);equal(values[2],nil);equal(values[3],"kept")
  equal(s.sectionCalls or 0,called,case.name.." never probes an unsupported display point")
  equal(#s.sectionVisits,0)
 end
 s.depth=0;s.restore()
end

-- Replacing the canvas must not call or paint the absent default. Removing the
-- registration, and running on a host without replace, restore the normal map.
for _,pack in ipairs({"modern","gamepad"})do
 local s=rig(pack);s.draw();local geometry=#s.geometry
 s.replaceSlot="map.canvas";s.draw();equal(#s.geometry,geometry,"Empty replacement skips DrawZone")
 check(not s.texts["No map for this zone."],"An empty replacement does not paint stock no-map fallback")
 s.customReplacement=true;s.draw();equal(#s.geometry,geometry);check(s.texts["Custom map canvas"])
 check(not s.texts["No map for this zone."],"Custom canvas has no default fallback overlay")
 s.replaceSlot=nil;s.draw();equal(#s.geometry,geometry+1,"Removing replace restores DrawZone")
 s.api.version.minor=4;s.legacySlots=true;s.replaceSlot="map.canvas";local before=s.sectionCalls or 0;s.draw()
 equal(#s.geometry,geometry+2,"Older hosts draw the ordinary canvas")
 equal(s.sectionCalls or 0,before,"API 1.4 rejects new slots, so no presenter may probe one")
 s.api.version.minor=5;s.legacySlots=false;s.replaceSlot=nil
 if pack=="modern"then
  s.noMap=true;s.draw();check(s.texts["No map for this zone."],"Genuine backend absence retains the stock message")
  s.noMap=false;s.hover="##maparea";s.mouseClick=ui.Mouse.Right;s.draw();equal(s.popup,"##mapmenu")
  s.mouseClick=nil;s.menuClick="Find a path here";s.replaceSlot="map.canvas";local before=#s.mapActions
  s.draw();equal(#s.mapActions,before,"A retired canvas popup cannot issue a stale coordinate Find")
  check(s.popup~="##mapmenu","Canvas replacement retires its old stock popup")
 end
 s.restore()
end

local function lastMap(s)return s.geometry[#s.geometry]end

local function panAndZoom(s)
 s.hover="##maparea";s.io.wheel=1;s.draw();s.io.wheel=0;s.draw()
 local before=lastMap(s)
 s.active="##maparea";s.drag=true;s.io.mouseDX=27;s.io.mouseDY=-11;s.draw()
 s.drag=false;s.active=nil;s.io.mouseDX=0;s.io.mouseDY=0;s.draw()
 local view=lastMap(s)
 check(view.zoom>1,"The map is zoomed in")
 check(view.cx~=before.cx and view.cy~=before.cy,"The map was panned on both axes")
 return view
end

local function sameCamera(view,expected,why)
 equal(view.zoom,expected.zoom,why.." zoom")
 equal(view.cx,expected.cx,why.." horizontal center")
 equal(view.cy,expected.cy,why.." vertical center")
end

-- Camera state survives closing either presenter, but belongs to one zone entry.
for _,pack in ipairs({"modern","gamepad"})do
 local s=rig(pack);s.draw()
 local default=lastMap(s)
 local panned=panAndZoom(s)
 s.shown=false;s.draw();s.playerX=15;s.playerY=18
 s.shown=true;s.draw();sameCamera(lastMap(s),panned,pack.." reopen preserves")
 s.draw();sameCamera(lastMap(s),panned,pack.." reopen stays panned after the player moves")

 s.shown=false;s.draw()
 s.zone="beta";s.shownZone=s.zone;s.zoneEntry=s.zoneEntry+1
 s.draw();s.shown=true;s.draw()
 sameCamera(lastMap(s),default,pack.." zone change while closed resets")
 panAndZoom(s)
 s.zoneEntry=s.zoneEntry+1;s.draw()
 sameCamera(lastMap(s),default,pack.." new entry with the same zone short name resets")

 panAndZoom(s)
 s.inGame=false;s.draw()
 s.inGame=true;s.zoneEntry=s.zoneEntry+1;s.draw()
 sameCamera(lastMap(s),default,pack.." logout and re-entry into the same zone resets")

 s.hover="##maparea";s.io.wheel=1;s.draw();s.io.wheel=0;s.draw();s.draw()
 local following=lastMap(s)
 equal(following.cx,s.playerX);equal(following.cy,s.playerY)
 s.shown=false;s.draw();s.playerX=25;s.playerY=28
 s.shown=true;s.draw();s.draw()
 equal(lastMap(s).zoom,following.zoom,pack.." reopen preserves following zoom")
 equal(lastMap(s).cx,s.playerX,pack.." reopen still follows the player horizontally")
 equal(lastMap(s).cy,s.playerY,pack.." reopen still follows the player vertically")
 s.restore()
end

-- Modern restores a browsed map and its camera, and displays the player's map in the minimap while closed.
do
 local s=rig("modern");s.draw()
 local default=lastMap(s)
 s.draw({},"##vworld");s.atlasZone="remote";s.draw();s.draw()
 equal(lastMap(s).zone,"remote")
 local panned=panAndZoom(s)
 s.shown=false;s.draw();equal(s.shownZone,"alpha","Closing restores the native map for the minimap")
 s.shown=true;s.draw();equal(lastMap(s).zone,"remote","Reopening shows the map that was closed")
 sameCamera(lastMap(s),panned,"Reopening a browsed map preserves")
 s.shown=false;s.draw()
 s.zone="beta";s.shownZone=s.zone;s.zoneEntry=s.zoneEntry+1
 s.shown=true;s.draw();equal(lastMap(s).zone,"beta","Zoning clears a previously browsed map")
 sameCamera(lastMap(s),default,"Zoning after browsing resets")
 s.restore()
end

-- The real atlas keeps its camera on repeated opens and starts at the new zone after a reset.
do
 local s=rig("modern")
 s.api.map.ZoneLinks=function()return "server",0,0,0 end
 s.api.map.AtlasIndex=function()return{
  pages={{file="Alpha.xml",title="Alpha region"},{file="Beta.xml",title="Beta region"}},
  zones={{zone="alpha",id=1,name="Alpha",page="Alpha.xml",x0=0,y0=0,x1=20,y1=40},
   {zone="beta",id=2,name="Beta",page="Beta.xml",x0=40,y0=60,x1=60,y1=100}},
 }end
 local atlas=originalDofile(CLIENT_ROOT.."/ui/modern/lib/atlas.lua")
 atlas.Opened();equal(atlas.page,"Alpha.xml")
 local panned={zoom=3,cx=45,cy=65}
 atlas.zoom,atlas.cx,atlas.cy=panned.zoom,panned.cx,panned.cy
 atlas.Opened();sameCamera(atlas,panned,"Reopening World preserves")
 s.zone="beta";atlas.ResetView();atlas.Opened()
 equal(atlas.page,"Beta.xml")
 sameCamera(atlas,{zoom=1.6,cx=50,cy=80},"Opening World after zoning resets")
 s.restore()
end

-- The map window uses the same per-character choice as the Options window.
local MAP_PACK="Map Pack##map_pack"
local function hint(s,key)for _,h in ipairs(s.beginOpts.hints)do if h[1]==key then return h[2]end end end
do
 local s=rig("modern")
 s.mapPacks={"Default","Brewall","Good's"};s.mapPack=1
 s.draw();equal(s.combos[MAP_PACK],"Brewall","Preview reflects the existing choice")
 equal(#s.mapActions,0,"Drawing does not change the pack")
 s.draw({},MAP_PACK)
 equal(#s.popupButtons,3,"The dropdown lists every installed choice")
 check(s.button("Brewall").selected,"The current pack is selected in the dropdown")
 s.draw({},"Good's");equal(#s.mapActions,1)
 equal(s.mapActions[1][1],"ComboSelect");equal(s.mapActions[1][2],2)
 s.draw();equal(s.combos[MAP_PACK],"Good's","Preview updates after selection")
 s.mapPack=0;s.draw();equal(s.combos[MAP_PACK],"Default","Preview follows changes made elsewhere")
 s.draw({},"##vworld");s.draw()
 equal(s.combos[MAP_PACK],nil,"The World atlas has no map pack selector")
 s.restore()
end
do
 local s=rig("gamepad")
 s.mapPacks={"Default","Brewall","Good's"};s.mapPack=1
 s.draw();equal(s.combos[MAP_PACK],"Brewall")
 s.draw({rb=true});s.draw();s.draw()
 local centred=s.geometry[#s.geometry]
 s.axes={lx=1};s.draw();s.axes=nil;s.draw()
 check(s.geometry[#s.geometry].cx~=centred.cx,"The stick panned the map")
 s.focus=s.button("Map Pack").id
 s.draw({y=true});equal(#s.popupButtons,0,"Y does not open the focused map pack dropdown")
 s.draw();s.draw();equal(s.geometry[#s.geometry].cx,centred.cx,"Y still centres the map")
 equal(hint(s,"a"),"Map pack","A's hint names the focused dropdown")
 s.draw({a=true});equal(#s.popupButtons,3,"A opens the focused map pack dropdown")
 equal(#s.mapActions,0,"Opening the dropdown with A does not find a path")
 s.draw({b=true,rb=true,rt=true});check(s.shown,"B closes the dropdown without closing the map")
 equal(hint(s,"a"),"Select");equal(hint(s,"b"),"Back","The hints follow the open dropdown")
 s.draw();equal(s.geometry[#s.geometry].zoom,centred.zoom,"The dismiss-frame RB does not zoom")
 equal(#s.mapActions,0,"The dismiss-frame RT does not toggle Auto Z")
 s.draw({a=true});s.draw();s.focusLabel("Good's")
 s.draw({y=true});equal(#s.mapActions,0,"Y does not select a map pack")
 check(s.popup,"Y leaves the dropdown open")
 s.draw({a=true})
 equal(#s.mapActions,1,"Selecting a pack with A does not find a path")
 equal(s.mapActions[1][1],"ComboSelect");equal(s.mapActions[1][2],2)
 s.draw();equal(s.combos[MAP_PACK],"Good's")
 s.focus=s.button("##maparea").id
 s.draw({a=true});equal(s.mapActions[2][1],"MapLocation","A still finds a path from the map canvas")
 s.draw();equal(hint(s,"a"),"Find path to centre");equal(hint(s,"b"),"Close")
 s.restore()
end

-- Native map behavior remains reachable while the extension popup is closed.
do
 local s=rig("modern");s.draw();local zoom=s.geometry[#s.geometry].zoom
 s.draw({},"##zin");check(s.geometry[#s.geometry].zoom>zoom)
 s.draw({},"##labels");equal(s.settings["map.labels"],"0")
 s.draw({},"##autoz");equal(s.mapActions[#s.mapActions][1],"AutoZ")
 s.draw({},"##edit");equal(s.mapActions[#s.mapActions][2],"MapToolbarWnd")
 local opener
 for _,row in ipairs(s.buttons)do if row.label:lower():find("control",1,true)then opener=row;break end end
 check(opener,"Actual Modern map exposes the controls opener")
 s.draw({},opener.raw);check(s.controls.Open());s.restore()
end
do
 local s=rig("gamepad");s.draw();s.draw({rb=true});s.draw()
 s.draw({lt=true});s.draw();check(s.controls.Open())
 local before=#s.mapActions;local view=s.geometry[#s.geometry]
 s.axes={lx=1,ly=1,rx=1,ry=1};s.draw({lt=true,a=true,rb=true})
 check(not s.controls.Open(),"LT closes the controls popup")
 equal(#s.mapActions,before,"Closing controls cannot activate Find in the same frame")
 s.draw({a=true,rb=true})
 local resumed=s.geometry[#s.geometry]
 equal(resumed.zoom,view.zoom,"The close-frame RB cannot change zoom")
 equal(resumed.cx,view.cx,"The close-frame stick cannot pan horizontally")
 equal(resumed.cy,view.cy,"The close-frame stick cannot pan vertically")
 equal(#s.mapActions,before+1,"Find resumes on the next ordinary frame")
 equal(s.mapActions[#s.mapActions][1],"MapLocation")
 s.axes=nil;s.draw();local moved=s.geometry[#s.geometry]
 check(moved.zoom>view.zoom,"Zoom resumes after the close frame")
 check(moved.cx~=view.cx or moved.cy~=view.cy,"Stick panning resumes after the close frame")
 s.restore()
end
do
 local s=rig("gamepad");s.draw();s.draw({rb=true});s.draw();check(s.geometry[#s.geometry].zoom>1)
 s.draw({x=true});equal(s.settings["map.labels"],"false")
 s.draw({rt=true});equal(s.mapActions[#s.mapActions][1],"AutoZ")
 s.draw({a=true});equal(s.mapActions[#s.mapActions][1],"MapLocation")
 s.draw({lt=true});check(s.controls.Open(),"LT opens actual Gamepad map controls")
 s.draw();local before=#s.mapActions;local labels=s.settings["map.labels"];local zoom=s.geometry[#s.geometry].zoom
 s.axes={lx=1,ly=1,rx=1,ry=1};s.draw({a=true,x=true,y=true,rt=true,rb=true})
 equal(#s.mapActions,before,"Map shortcuts do not leak through the controls popup")
 equal(s.settings["map.labels"],labels);equal(s.geometry[#s.geometry].zoom,zoom)
 s.axes=nil;s.draw();s.focusLabel("Change map setting");s.draw({a=true})
 equal(s.actions[#s.actions].phase,"review","Native popup focus activates the review row")
 s.draw();s.focusLabel("Confirm");s.draw({a=true});equal(s.actions[#s.actions].phase,"confirm")
 s.draw();s.focusLabel("Change map setting");s.draw({a=true});s.draw();before=#s.actions
 s.draw({b=true});equal(#s.actions,before+1);equal(s.actions[#s.actions].phase,"cancel")
 check(s.controls.Open()and s.shown,"B cancels review before closing popup or map")
 s.draw();s.draw({b=true});check(not s.controls.Open()and s.shown,"A later B closes only controls")
 s.draw();s.draw({b=true});check(not s.shown,"B still closes the ordinary map once controls are closed")
 s.restore()
end
dofile=originalDofile
print("Map extension presenters: "..checks.." assertions passed")
