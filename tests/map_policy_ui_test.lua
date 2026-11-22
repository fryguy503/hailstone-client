-- Actual map/Find scripts over bounded presentation primitives. No server or addon code is used.
CLIENT_ROOT=CLIENT_ROOT or(arg and arg[1])or"."
local originalDofile=dofile
local checks=0
local function check(value,why)checks=checks+1;assert(value,why)end
local function equal(a,b,why)check(a==b,(why or"values differ")..": "..tostring(a).." / "..tostring(b))end
local function nop()end
local function rig(pack)
 local s={time=20,frameCount=1,zone="fixture",windows={},settings={["map.npcMarks"]="1",["find.hidden"]="banker",["map.sideTab"]="find",
  ["map.sidebar"]="1",["map.markers.fixture"]="My pin|5|8|1"},writes={},actions={},texts={},hits={},triangles=0,
  policy=nil,policyReads=0,npcReads=0,geometry=0,depth=0,x=0,y=0,findOpen=false,shown=true,accepted={},menuHits={},
  rows={{id=44,name="Old NPC",category="merchant",x=3,y=4,distance=5,subtitle="Trader",label="Merchant"}}}
 FIND_STATE=nil
 local function draw()check(s.depth>0,"Presentation drew into an implicit root/debug window")end
 local function text(value)draw();s.texts[tostring(value)]=true end
 local function hit(id)draw();s.lastHit=id;s.hits[id]=true;return s.click==id end
 local function act(name,... )s.actions[#s.actions+1]={name,...};return true,""end
 local function npcAction(name,id,token)
  act(name,id,token)
  if s.beforeAction then s.beforeAction(name,id,token) end
  for _,row in ipairs(s.rows)do
   if row.id==id and row.token==token then
    s.accepted[#s.accepted+1]={name,id,token};return true,""
   end
  end
  return false,"NPC identity changed"
 end
 local map={Policy=function()s.policyReads=s.policyReads+1;return s.policy end,
  ZoneName=function()return"Fixture",s.zone end,ZoneShown=function()return"Fixture",s.zone,true end,
  ShowZone=function(...)return act("ShowZone",...)end,FindPathShown=function()return s.pathShown==true end,
  AutoZ=function(...)if select("#",...)>0 then return act("AutoZ",...)end;return true end,
  DrawZone=function()draw();s.geometry=s.geometry+1;return 1,0,0,0,0 end,
  DrawMinimap=function()draw();s.geometry=s.geometry+1;return 0,1,0,0,1 end}
 local find={Categories=function()return{{"merchant","Merchants"},{"banker","Bankers"}}end,
  Npcs=function(search)s.npcReads=s.npcReads+1;s.lastSearch=search;return s.rows end,
  Open=function(value)if value~=nil then s.findOpen=value;return act("FindOpen",value)end;return s.findOpen end,
  NpcTarget=function(id,token)return npcAction("NpcTarget",id,token)end,
  NpcPath=function(id,token)return npcAction("NpcPath",id,token)end,
   CurrentTargetPath=function(show)
     if not s.targetReady then return false end
     if show==false then return true end
     return act("CurrentTargetPath")
   end,
  NpcWalk=function(id,token)return npcAction("NpcWalk",id,token)end,End=function()return act("End")end,
  MapLocation=function(...)return act("MapLocation",...)end}
 hs={UI={map=map,find=find,session={InGame=function()return true end,GameTime=function()return nil end},
  windows={Shown=function()return s.shown end,Show=function(_,value)s.shown=value;return act("ShowWindow",value)end,
   ComboInfo=function()return{"Default"},0 end}}}
 hs.UI.Supports=function(module,member)return hs.UI[module]and type(hs.UI[module][member])=="function"or false end
 ui={Mouse={Left=0,Right=1},InputTextFlags={EnterReturnsTrue=1,AutoSelectAll=2}}
 ui.window=function(name,body)s.windows[name]=body end
 ui.GetTime=function()return s.time end
 ui.GetFrameCount=function()return s.frameCount end
 ui.GetSetting=function(key)return s.settings[key]or""end
 ui.SetSetting=function(key,value)s.writes[#s.writes+1]={key,value};s.settings[key]=tostring(value)end
 ui.GetIO=function()return{alt=false,wheel=0,mouseDX=0,mouseDY=0}end
 ui.GetWindowPos=function()return 0,0 end
 ui.GetCursorScreenPos=function()return s.x,s.y end
 ui.SetCursorPos=function(x,y)s.x,s.y=x,y end
 ui.SetCursorScreenPos=ui.SetCursorPos
 ui.GetContentRegionAvail=function()return 240,400 end
 ui.GetMousePos=function()return-100,-100 end
 ui.CalcTextSize=function(value)return #tostring(value)*7,14 end
 ui.IsItemHovered=function()return s.hoverId~=nil and s.lastHit==s.hoverId end;ui.IsItemActive=function()return false end
 ui.IsItemClicked=function(button)return s.lastHit==s.itemId and button==s.itemButton end
 ui.IsMouseClicked=function(button)return s.mouseClick==button end
 ui.IsMouseDoubleClicked=function()return false end;ui.IsMouseDragging=function()return false end
 ui.IsMouseReleased=function()return false end;ui.IsWindowAppearing=function()return false end
 ui.InvisibleButton=function(id)hit(id);return false end
 ui.Button=hit;ui.MenuItem=function(label)s.menuHits[label]=true;return s.menuClick==label end;ui.BeginMenu=function()return false end
 ui.BeginPopup=function(name)return s.popup==name end;ui.OpenPopup=function(name)s.popup=name end
 ui.BeginCombo=function()draw();return false end
 ui.CloseCurrentPopup=function()s.popup=nil;s.popupClosed=true end
 ui.EndPopup=nop;ui.EndMenu=nop
 ui.BeginChild=function()draw();s.depth=s.depth+1;return true end
 ui.EndChild=function()s.depth=s.depth-1 end
 ui.InputTextWithHint=function(_,_,value)draw();return value end
 ui.InputText=function(_,value)draw();return value,false end
 ui.DrawTriangleFilled=function()draw();s.triangles=s.triangles+1 end
 for _,key in ipairs({"DrawCircle","DrawCircleFilled","DrawRect","DrawRectFilled","Separator"})do ui[key]=draw end
 for _,key in ipairs({"SetNextItemWidth","SetKeyboardFocusHere","SetTooltip","PushClipRect","PopClipRect","PushFont","PopFont"})do ui[key]=nop end
 ui.Text=text;ui.TextDisabled=text
 local color=setmetatable({},{__index=function()return{.5,.6,.7,1}end})
 local theme={color=color,C=color,font={small=1,tiny=1,normal=1},NO_OPTS={},S=1}
 theme.Text=function(_,_,value)text(value)end;theme.TextSize=ui.CalcTextSize
 theme.Fit=function(value)return value end;theme.Hit=hit
 theme.Button=function(id,x0,y0,x1,y1,scale,label,opts)return hit(id)and(not opts or opts.enabled~=false)end
 theme.TakeRightClick=nop;theme.rgba=function(c,a)return c[1],c[2],c[3],a end
 theme.frame=nop;theme.px=function(n)return n end;theme.draw=function(_,_,value)text(value)end
 theme.font=function()end;theme.unfont=nop
 theme.textIn=function(_,_,_,_,value)text(value)end
 theme.button=function(id,label,w,h,opts,pad,off)text(label);return hit(id)and not off end
 -- Modern's font table and Gamepad's font method are separate pack APIs.
 if pack=="modern"then theme.font={small=1,tiny=1,normal=1}end
 local hud={T=theme,Scale=function()return 1 end,DragItem=function()return false end,AutoZControls=nop}
 local function panel(drawBody)s.depth=s.depth+1;drawBody(0,0,1100,650,1);s.depth=s.depth-1 end
 hud.Dialog=function(_,_,_,body)panel(body)end
 hud.Window=function(_,_,_,body)panel(body)end
 local atlas={Opened=nop,ResetView=nop,Show=nop,DrawExits=nop,DrawRouteBanner=nop,DrawCrumbs=nop,DrawFinder=nop,DrawView=nop,zoom=1}
 local window={shown=function()return false end,begin=function(_,options)
  s.depth=s.depth+1
  if options.subtitle then text(options.subtitle)end
  for _,hint in ipairs(options.hints or{})do text(hint[2])end
  return true
 end,
  area=function()return 0,0,1100,650 end,finish=function()s.depth=s.depth-1;return false end,
  pressed=function(key)return s.pressed==key end,
  list=function(_,state,count,_,_,_,render,activate)
   s.listOffset=state.offset;s.listCount=count
   if s.seedOffset then state.offset=s.seedOffset;s.seedOffset=nil end
   for i=1,count do render(i,0,(i-1)*80,900,i*80)end
   if s.activate then s.activate=false;activate(1)end
   return 1
  end}
 dofile=function(path)
  if path=="lib/hud.lua"then return hud end
  if path=="lib/atlas.lua"then return atlas end
  if path=="lib/theme.lua"then return theme end
  if path=="lib/window.lua"then return window end
  if path=="lib/pad.lua"then return{}end
  if path=="lib/model.lua"then return{list=nop}end
  if path=="lib/extensions.lua"or path=="lib/map_controls.lua"then
   return assert(loadfile(CLIENT_ROOT.."/ui/"..pack.."/"..path))()
  end
  check(path=="lib/find.lua","Unexpected presentation dependency "..path)
  return assert(loadfile(CLIENT_ROOT.."/ui/modern/"..path))()
 end
 function s.load(file)assert(loadfile(CLIENT_ROOT.."/ui/"..pack.."/"..file))()end
 function s.frame(name)
  s.frameCount=s.frameCount+1;s.texts={};s.hits={};s.triangles=0;s.lastHit=nil;s.menuHits={}
  assert(s.windows[name],name)()
  equal(s.depth,0,"Unbalanced presentation window/child scope")
 end
 function s.preferences()
  equal(s.settings["map.npcMarks"],"1","Policy changed the saved NPC mark preference")
  equal(s.settings["find.hidden"],"banker","Policy changed the saved Find category preference")
 end
 function s.restore()dofile=originalDofile end
 return s
end

do
 local s=rig("modern");local first=dofile("lib/find.lua");local second=dofile("lib/find.lua")
 equal(first.rows()[1].id,44,"Initial native Find rows missing")
 local queried=s.npcReads
 FIND_STATE.sel,FIND_STATE.hover,FIND_STATE.menu,FIND_STATE.why=44,44,s.rows[1],"old reason"
 s.time=s.time+.01;s.policy={custom_spawns=true,stock_npcs=false,npc_find=false,name="Synthetic map owner"}
 equal(#second.rows(),0,"Policy transition exposed the shared 200ms cache")
 check(FIND_STATE.sel==nil and FIND_STATE.hover==nil and FIND_STATE.menu==nil and FIND_STATE.why==nil and FIND_STATE.rowsCache==nil,
  "Policy retirement retained shared selected/hovered/menu/cache state")
 equal(s.npcReads,queried,"Blocked Find queried native NPC rows")
 check(not first.marks(),"Saved mark preference bypassed map ownership")
 s.preferences();equal(#s.writes,0,"Policy retirement wrote presentation preferences")
 s.rows={{id=901,name="Fresh NPC",category="merchant",x=7,y=8,distance=10,subtitle="",label="Merchant"}}
 s.policy=nil;s.time=s.time+.01
 equal(first.rows()[1].id,901,"Restoration reused an old cache before its 200ms expiry")
 equal(s.npcReads,queried+1,"Restoration did not ask for fresh native-filtered rows")
 s.preferences();s.restore()
end

do
 local s=rig("modern");local FL=dofile("lib/find.lua")
 local merchant=s.rows[1]
 s.rows={merchant,{id=45,name="Banker",category="banker"}}
 equal(#FL.rows(),1,"Saved category filter stopped applying to fresh host rows")
 FIND_STATE.search="merchant search"
 equal(#FL.rows(),1,"Changing search changed category filtering")
 equal(s.lastSearch,"merchant search","Changed search did not reach the host in the same frame")
 equal(s.npcReads,2,"Changed search reused a same-frame cache")
 FIND_STATE.sel,FIND_STATE.menu=44,merchant
 FL.setHidden("merchant",true)
 equal(#FL.rows(),0,"Changed local category filter reused a same-frame cache")
 check(FIND_STATE.sel==nil and FIND_STATE.menu==nil,"Filtered actor retained actionable selection/menu state")
 equal(FIND_STATE.search,"merchant search","Category filter changed the search")
 equal(#s.writes,1,"Explicit category selection did not write exactly its preference")
 s.policy={npc_find=false};FL.rows()
 equal(#s.writes,1,"Policy retirement changed the user's category preference")
 s.restore()
end

for _,name in ipairs({"Map","Minimap"})do
 local s=rig("modern");s.load(name=="Map"and"map.lua"or"minimap.lua")
 s.frame(name);check(s.triangles>0,"Stock NPC marks were absent before ownership")
 local queried=s.npcReads
 FIND_STATE.sel,FIND_STATE.hover,FIND_STATE.menu=44,44,s.rows[1]
 s.policy={custom_spawns=true,stock_npcs=false,npc_find=false,name="Synthetic map owner"};s.time=s.time+.01
 s.click="##npcs";s.frame(name)
 equal(s.triangles,0,"Owned map drew selected/hovered stock NPC marks")
 equal(s.npcReads,queried,"Owned map read stale or native Find NPCs")
 check(FIND_STATE.sel==nil and FIND_STATE.hover==nil and FIND_STATE.menu==nil and FIND_STATE.rowsCache==nil,"Map did not retire shared Find state immediately")
 check(not s.texts["Old NPC"]and not s.hits["##npcs"]and not s.hits["##tabfind"]and not s.hits["##findpath"],"Owned map exposed conflicting NPC controls or names")
 check(s.geometry>=2,"Ownership suppressed native map geometry")
 if name=="Map"then check(s.texts["My pin"]and s.hits["##edit"]and s.hits["##labels"]and s.hits["##marks"],"Ownership hid personal pins or stock map editing")end
 s.preferences();equal(#s.actions,0,"Ownership transition called a Find action")
 equal(#s.writes,0,"Ownership transition changed saved UI preferences")
 s.click=nil;s.policy=nil;s.rows={{id=902,name="New visible NPC",category="merchant",x=5,y=6,distance=8,subtitle="",label="Merchant"}};s.time=s.time+.01
 s.frame(name);check(s.triangles>0,"Restoring stock policy failed to restore saved NPC marks")
 equal(s.npcReads,queried+1,"Map restoration reused a stale filtered NPC list")
 if name=="Map"then check(s.hits["##npcs"]and s.hits["##tabfind"]and s.texts["New visible NPC"],"Find page/control did not return from fresh data")end
 s.preferences();equal(#s.actions,0,"Restoration issued an implicit NPC action")
 s.policy={custom_spawns=true,stock_npcs=false,npc_find=true,name="Overlay-only owner"};s.time=s.time+.01;s.frame(name)
 FIND_STATE.sel,FIND_STATE.hover=902,902;s.frame(name)
 equal(s.triangles,0,"Selected/hovered stock marks escaped an overlay-only ownership policy")
 if name=="Map"then check(s.hits["##tabfind"]and not s.hits["##npcs"],"Independent NPC Find policy was not respected")end
 s.restore()
end

do
 local s=rig("modern");s.settings["map.npcMarks"]="0";s.load("minimap.lua");s.frame("Minimap")
 equal(s.npcReads,0,"A minimap with no NPC marks/state initiated an NPC read")
 s.restore()
end

do
 local s=rig("modern");hs.UI.map.Policy=nil;s.load("map.lua");s.frame("Map")
 check(s.hits["##npcs"]and s.hits["##tabfind"]and s.triangles>0,"An older host without Policy lost stock behavior")
 s.preferences();s.restore()
end

do
 local s=rig("gamepad");s.findOpen=true;s.seedOffset=99;s.load("navigation.lua");s.frame("Find")
 equal(s.listCount,1,"Gamepad stock Find rows missing")
 local queried=s.npcReads
 s.policy={custom_spawns=true,stock_npcs=false,npc_find=false,name="Synthetic map owner"};s.activate=true;s.frame("Find")
 equal(s.npcReads,queried,"Gamepad owned Find read native NPC rows")
 equal(#s.actions,0,"Gamepad ownership transition targeted, pathed, walked or closed Find")
 check(next(s.texts)==nil,"Gamepad owned Find still painted its conflicting NPC window")
 s.preferences();equal(#s.writes,0,"Gamepad policy retirement changed preferences")
 s.rows={{id=903,name="Fresh Gamepad NPC",category="merchant",distance=6,subtitle="",label="Merchant"}};s.policy=nil;s.frame("Find")
 equal(s.listOffset,0,"Gamepad restoration retained stale list navigation")
 equal(s.actions[1][1],"NpcTarget","Gamepad fresh activation lost targeting")
 equal(s.actions[1][2],903,"Gamepad restoration activated a stale row")
 equal(s.actions[2][1],"NpcPath","Gamepad fresh activation lost path request")
 equal(s.actions[2][2],903,"Gamepad restoration pathed to a stale row")
 equal(s.actions[3][1],"FindOpen","Gamepad explicit activation did not close Find")
 s.preferences();s.restore()
end

-- The rows are read again every 200ms, as before map policies, and at once on a policy, zone or search change; an
-- action is authorized by its token when it is taken. Helper reloads share the state.
do
 local s=rig("modern");local first=dofile("lib/find.lua");local second=dofile("lib/find.lua")
 local old=s.rows[1];old.token="actor:first"
 s.policy={stock_npcs=true,npc_find=true,custom_spawns=false,name="Provider",revision="r1",context="connection:1"}
 FIND_STATE.search="kept search"
 equal(first.rows()[1].token,"actor:first","Token-bearing actor missing")
 equal(second.rows()[1],old,"Helpers did not share one authorized frame")
 equal(s.npcReads,1,"One frame queried authorization more than once")
 equal(s.lastSearch,"kept search","Search was not passed to the host")
 FIND_STATE.sel,FIND_STATE.selToken,FIND_STATE.hover,FIND_STATE.hoverToken=44,old.token,44,old.token
 FIND_STATE.menu,FIND_STATE.why=old,"old failure"
 s.rows={};s.frameCount=s.frameCount+1;s.time=s.time+.25
 equal(#first.rows(),0,"A hidden row survived past the 200ms re-read")
 check(FIND_STATE.sel==nil and FIND_STATE.hover==nil and FIND_STATE.menu==nil and FIND_STATE.why==nil,
  "Revoked actor retained selected/hovered/menu/reason state")
 equal(s.npcReads,2,"The re-read did not ask the client again")
 s.rows={old};s.frameCount=s.frameCount+1;s.time=s.time+.25
 equal(first.rows()[1],old,"Authorized actor was not restored")
 FIND_STATE.sel,FIND_STATE.selToken,FIND_STATE.menu=44,old.token,old
 s.policy.unknown_future_field={anything=true}
 second.policy()
 equal(FIND_STATE.sel,44,"An unknown additive policy field invalidated a valid actor")
 s.policy.revision="r2"
 second.policy()
 check(FIND_STATE.sel==nil and FIND_STATE.menu==nil and FIND_STATE.rowsCache==nil,"Same-name provider revision retained old state")
 first.rows();FIND_STATE.menu=old;FIND_STATE.why="old reason"
 s.policy.context="connection:2";s.time=s.time+.016 -- same zone, same display name, same numeric actor id; a frame later
 first.rows()
 check(FIND_STATE.menu==nil and FIND_STATE.why==nil,"Connection context change retained a menu")
 FIND_STATE.sel,FIND_STATE.selToken,FIND_STATE.hover,FIND_STATE.hoverToken=44,old.token,44,old.token
 FIND_STATE.menu,FIND_STATE.why=old,"old reason"
 local replacement={id=44,token="actor:replacement",name="Replacement",category="merchant",x=9,y=8}
 s.rows={replacement};s.frameCount=s.frameCount+1;s.time=s.time+.25
 equal(first.rows()[1],replacement,"Same-id replacement was not read")
 check(FIND_STATE.sel==nil and FIND_STATE.hover==nil and FIND_STATE.menu==nil and FIND_STATE.why==nil,
  "Same-zone actor incarnation reused old UI state")
 FIND_STATE.sel,FIND_STATE.selToken,FIND_STATE.menu=44,nil,{id=44,name="Untokened old row"}
 s.frameCount=s.frameCount+1;s.time=s.time+.25;first.rows()
 check(FIND_STATE.sel==nil and FIND_STATE.menu==nil,"A missing saved token matched a token-bearing actor")
 FIND_STATE.menu,FIND_STATE.why=replacement,"old reason";s.zone="other";first.rows()
 check(FIND_STATE.menu==nil and FIND_STATE.why==nil,"Zone transition retained old menu data")
 equal(FIND_STATE.search,"kept search","Policy or identity retirement changed the user's search")
 s.preferences();equal(#s.writes,0,"Read-only retirement wrote settings")
 local before=s.npcReads;first.rows();second.rows()
 equal(s.npcReads,before,"Two reads within 200ms asked the client twice")
 s.restore()
end

for _,name in ipairs({"Map","Minimap"})do
 local s=rig("modern");s.load(name=="Map"and"map.lua"or"minimap.lua");s.frame(name)
 FIND_STATE.sel,FIND_STATE.hover,FIND_STATE.menu,FIND_STATE.why=44,44,s.rows[1],"stale reason"
 s.rows={};s.time=s.time+.25;s.popup="##findmenu";s.menuClick="Target"
 s.frame(name)
 equal(s.triangles,0,"A newly hidden NPC retained its map/minimap mark")
 check(FIND_STATE.sel==nil and FIND_STATE.hover==nil and FIND_STATE.menu==nil and FIND_STATE.why==nil,
  "Fresh empty rows did not retire shared actor state")
 check(not s.texts["Old NPC"],"A hidden NPC name remained in the popup")
 equal(#s.actions,0,"A hidden popup dispatched an action")
 if name=="Map"then check(s.popupClosed,"The retired NPC popup was not closed locally")end
 s.preferences();s.restore()
end

-- Each explicit action forwards the opaque identity of the displayed row. A host-side replacement at the
-- action boundary rejects that old identity instead of acting on a same-id newcomer.
do
 local s=rig("modern");s.rows[1].token="manual:actor";s.load("map.lua");s.frame("Map")
 for _,label in ipairs({"Target","Show the path","Walk there"})do
  FIND_STATE.menu=s.rows[1];s.popup="##findmenu";s.menuClick=label;s.frame("Map")
  equal(s.accepted[#s.accepted][3],"manual:actor","Modern popup omitted the expected actor token")
 end
 equal(#s.accepted,3,"Modern popup actions did not admit the current actor")
 s.popup=nil;s.menuClick=nil;s.itemId="##fn1";s.itemButton=ui.Mouse.Left;s.frame("Map")
 equal(FIND_STATE.selToken,"manual:actor","Manual row selection did not retain identity")
 s.itemId=nil;s.itemButton=nil;s.click="##findpath";s.frame("Map")
 equal(s.accepted[#s.accepted][1],"NpcPath","Selected path action was not admitted")
 equal(s.accepted[#s.accepted][3],"manual:actor","Selected path omitted identity")
 s.click="##findwalk";s.frame("Map")
 equal(s.accepted[#s.accepted][1],"NpcWalk","Selected walk action was not admitted")
 equal(s.accepted[#s.accepted][3],"manual:actor","Selected walk omitted identity")
 s.click=nil;FIND_STATE.menu=s.rows[1];s.popup="##findmenu";s.menuClick="Target"
 local admitted=#s.accepted
 s.beforeAction=function()s.rows={{id=44,token="new:actor",name="New actor",category="merchant",x=1,y=2,distance=3,subtitle="",label="Merchant"}}end
 s.frame("Map")
 equal(#s.accepted,admitted,"A TOCTOU replacement inherited an old popup action")
 equal(s.actions[#s.actions][3],"manual:actor","TOCTOU action did not carry the displayed token")
 s.beforeAction=nil;s.menuClick=nil;s.time=s.time+.25;s.frame("Map") -- (the rows re-read after 200ms)
 check(FIND_STATE.menu==nil and FIND_STATE.sel==nil,"Replacement retained the stale popup or selection")
 s.preferences();s.restore()
end

for _,key in ipairs({"activate","pressed"})do
 local s=rig("gamepad");s.findOpen=true;s.rows[1].token="pad:actor"
 s.policy={npc_find=true,name="Provider",revision="r1",context="c1"};s.seedOffset=99
 s.load("navigation.lua");s.frame("Find")
 s.policy.revision="r2";s.frame("Find")
 equal(s.listOffset,0,"Same-name provider revision retained Gamepad navigation")
 s.seedOffset=77;s.frame("Find");s.policy.context="c2";s.frame("Find")
 equal(s.listOffset,0,"Same-zone connection change retained Gamepad navigation")
 if key=="activate"then s.activate=true else s.pressed="x"end
 s.frame("Find")
 equal(s.accepted[1][3],"pad:actor","Gamepad target omitted identity")
 equal(s.accepted[2][1],key=="activate"and"NpcPath"or"NpcWalk","Gamepad did not admit the selected action")
 equal(s.accepted[2][3],"pad:actor","Gamepad path/walk omitted identity")
 check(not s.findOpen,"Successful Gamepad activation did not close Find")
 s.findOpen=true;s.pressed=nil;s.actions={};s.accepted={};s.activate=true
 s.beforeAction=function()s.rows={{id=44,token="pad:replacement",name="Replacement",category="merchant",distance=3,subtitle="",label="Merchant"}}end
 s.frame("Find")
 equal(#s.accepted,0,"A Gamepad action accepted a same-id replacement")
 check(s.findOpen,"Rejected Gamepad activation closed Find")
 equal(#s.actions,1,"Rejected Gamepad targeting dispatched a second action")
 s.preferences();s.restore()
end

do
 local s=rig("modern");s.load("map.lua");s.load("minimap.lua");s.frame("Map")
 check(s.hits["##findwalk"]and s.texts["Old NPC"],"Stock Find did not show its original rows and Walk action")
 s.policy={custom_spawns=true,stock_npcs=false,npc_find=true,npc_find_server_only=true,name="Server map"}
 s.rows={{id=904,token="server:npc",name="Server-listed NPC",category="merchant",x=5,y=6,distance=8,subtitle="",label="Merchant"}}
 s.time=s.time+.01;s.frame("Map")
 check(s.hits["##tabfind"]and s.texts["Server-listed NPC"]and not s.texts["Old NPC"],"Server policy did not show fresh native Find rows")
 check(s.hits["##findpath"]and not s.hits["##findwalk"]and not s.hits["##npcs"],"Server Find exposed stock marks or Walk controls")
 s.itemId="##fn1";s.itemButton=ui.Mouse.Left;s.frame("Map")
 equal(s.accepted[1][1],"NpcTarget","Server Find lost row selection")
 equal(s.triangles,0,"Server Find drew stock NPC marks on selection")
 s.itemId=nil;s.itemButton=nil;s.click="##findpath";s.frame("Map")
 equal(s.accepted[2][1],"NpcPath","Server Find lost the selected path action")
 equal(s.accepted[2][3],"server:npc","Server Find path omitted the row's identity")
 s.click=nil;FIND_STATE.menu=s.rows[1];s.popup="##findmenu";s.menuClick="Walk there";s.frame("Map")
 check(s.menuHits["Show the path"]and not s.menuHits["Walk there"],"Server Find popup exposed Walk or hid the path action")
 equal(#s.actions,2,"Unavailable Walk menu sent an action")
 s.menuClick="Show the path";s.frame("Map")
 equal(s.accepted[3][1],"NpcPath","Server Find lost the menu path action")
 s.popup=nil;s.menuClick=nil;s.pathShown=true;s.click="##findend";s.frame("Map")
 equal(s.actions[4][1],"End","Server Find lost the End path action")
 s.click=nil;s.frame("Minimap")
 equal(s.triangles,0,"Server Find drew stock NPC marks on the minimap")
 s.preferences();equal(#s.writes,0,"Server Find policy changed saved UI preferences");s.restore()
end

do
 local s=rig("gamepad");s.findOpen=true;s.seedOffset=99
 s.policy={custom_spawns=true,stock_npcs=false,npc_find=true,name="Server map"}
 s.load("navigation.lua");s.frame("Find")
 check(s.texts["Walk there"],"Gamepad stock Find lost its Walk hint")
 s.policy.npc_find_server_only=true
 s.rows={{id=905,token="server:pad",name="Server-listed Gamepad NPC",category="merchant",distance=8,subtitle="",label="Merchant"}}
 s.pressed="x";s.frame("Find")
 equal(s.listOffset,0,"Server-only policy retained stale Gamepad navigation")
 check(s.texts["Server-listed Gamepad NPC"]and s.texts["Show the path"]and not s.texts["Walk there"],"Gamepad server Find showed incorrect rows or controls")
 equal(#s.actions,0,"Gamepad Walk input dispatched an action under server Find")
 s.pressed=nil;s.activate=true;s.frame("Find")
 equal(s.accepted[1][1],"NpcTarget","Gamepad server Find lost targeting")
 equal(s.accepted[2][1],"NpcPath","Gamepad server Find lost the path action")
 equal(s.accepted[2][3],"server:pad","Gamepad server Find path omitted the row's identity")
 check(not s.findOpen,"Successful Gamepad server Find path did not close Find")
 s.preferences();equal(#s.writes,0,"Gamepad server Find policy changed saved UI preferences");s.restore()
end

-- NPC Find ownership does not disable a manually selected coordinate path or personal pins.
do
 local s=rig("modern");s.policy={custom_spawns=true,stock_npcs=false,npc_find=false,name="Owner"}
 s.load("map.lua");s.hoverId="##maparea";s.mouseClick=ui.Mouse.Right;s.menuClick="Find a path here"
 s.frame("Map")
 equal(s.actions[1][1],"MapLocation","NPC policy blocked an explicit coordinate path")
 check(s.texts["My pin"]~=nil,"NPC policy hid a personal map pin")
 equal(s.npcReads,0,"Coordinate path queried forbidden NPC data")
 s.preferences();s.restore()
end

for _,pack in ipairs({"modern","gamepad"})do
  local s=rig(pack);s.findOpen=true
  local modern=pack=="modern"
  local window=modern and "Map" or "Find"
  s.policy={stock_npcs=false,npc_find=true,npc_find_server_only=true}
  s.load(modern and "map.lua" or "navigation.lua")
  s.frame(window)
  check(not s.hits["##findtargetpath"],pack.." showed the actor guide without provider opt-in")
  s.policy.actor_guides=true
  s.click="##findtargetpath";s.frame(window)
  check(s.hits["##findtargetpath"],pack.." did not show the current-target button")
  equal(#s.actions,0,pack.." requested a guide with no current target")
  s.targetReady=true;s.frame(window)
  equal(s.actions[1][1],"CurrentTargetPath",pack.." did not request the current-target guide")
  equal(s.actions[1][2],nil,pack.." passed an NPC row or actor ID to the current-target guide")
  s.preferences();s.restore()
end
print("Map policy presentation: "..checks.." checks passed")
