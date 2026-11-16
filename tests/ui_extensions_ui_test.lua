-- Actual Raid presenters and extension helpers, with strict synthetic host/input surfaces.
-- No server commands or native-addon binary are used here.
CLIENT_ROOT=CLIENT_ROOT or (arg and arg[1]) or "."
local realDofile=dofile
local checks=0
local function check(value,why)checks=checks+1;assert(value,why or "check failed")end
local function eq(a,b,why)check(a==b,(why or "values differ")..": "..tostring(a).." / "..tostring(b))end
local function strict(t,label)return setmetatable(t,{__index=function(_,key)error("Unexpected "..label.."."..tostring(key),2)end})end
local function nop()end
local function rig(pack)
  local S={calls={},labels={},lines={},windows={},frame=0,time=5,x=0,y=0,last=nil,focus=nil,focusQueued=nil,
    pad={},inGame=true,shown=true,allowed=true,present=true,generation=1,children=0,disabled={},click=nil,
    rows={RAID_PlayerList={{"1","Grouped", "60","Cleric","",""}},
      RAID_NotInGroupPlayerList={{"","Offzone", "60","Wizard","",""}},RAID_NoteList={}},
    selectedList="RAID_NotInGroupPlayerList",selected=0,width=900,buttons={},events={},ids={},styles={},actionLabels={}}
  local function disabled()for _,v in ipairs(S.disabled)do if v then return true end end;return false end
  local windows={
    ListInfo=function(_,list)local data=assert(S.rows[list],list);return #data,6,list==S.selectedList and S.selected or -1 end,
    ListRow=function(_,list,row)return assert(S.rows[list],list)[row+1]end,
    ChildShown=function()return false end,ChildState=function()return false end,
    ClickChild=function()error("No native Raid action was requested")end,
    ListClick=function(_,list,row)S.selectedList,S.selected=list,row;return true end,
    Text=function(_,id)return id=="RAID_PlayerCountLabel" and "2" or "60"end,
    SetText=function()error("No native text edit was requested")end,
    Show=function(_,shown)S.shown=shown end,Shown=function()return S.shown end,
  }
  local function nativeActions(context)
    if not S.present then return {}end
    local out={}
    for _,item in ipairs({{"list","List Raid Managers",false},{"grant","Grant Raid Manager",true},{"revoke","Revoke Raid Manager",true}})do
      out[#out+1]={handle="extension:"..S.generation..":"..item[1],id=item[1],owner="plugin:test",label=S.actionLabels[item[1]] or item[2],
        enabled=S.allowed and (not item[3] or context.selection~=nil),requiresReview=item[3]}
    end
    return out
  end
  local api={windows=strict(windows,"API.windows"),session={InGame=function()return S.inGame end},
    character={Gauge=function()return nil,"",-1 end},menus={Contexts=function()return true end}}
  api.extensions={ListActions=function(_,_,context)return nativeActions(context)end,
    DrawSection=function(_,_,placement)S.sections=(S.sections or 0)+1;S.events[#S.events+1]=placement end,
    InvokeAction=function(handle,phase,context,token)
      S.calls[#S.calls+1]={handle=handle,phase=phase,name=context.selection and context.selection.name,token=token,revision=context.revision}
      if S.badResponse then return {ok="yes",message="Invalid"}end
      if phase=="review"then return {ok=true,message="Review "..(context.selection and context.selection.name or ""),reviewToken="one-use-token"}end
      return {ok=true,message=phase=="confirm" and "Request sent; server response pending" or "Done"}
    end}
  api.Supports=function(module,member)return api[module]~=nil and (not member or rawget(api[module],member)~=nil)end
  hs={UI=strict(api,"API")}
  ui={Mouse={Left=0,Right=1},ButtonFlags={EnableNav=1},StyleVar={FramePadding=1,ItemSpacing=2}}
  ui.window=function(name,draw)S.windows[name]=draw end
  ui.GetTime=function()return S.time end;ui.GetFrameCount=function()return S.frame end
  ui.GetSetting=function()return ""end;ui.GetIO=function()return {alt=false}end
  ui.GetCursorScreenPos=function()return S.x,S.y end;ui.GetWindowPos=function()return 0,0 end
  ui.SetCursorScreenPos=function(x,y)S.x,S.y=x,y end;ui.SetCursorPos=ui.SetCursorScreenPos
  ui.GetContentRegionAvail=function()return S.width-S.x,700 end
  ui.CalcTextSize=function(s)
    local width,lines=0,0
    for line in (tostring(s).."\n"):gmatch("(.-)\n")do width=math.max(width,#line*7);lines=lines+1 end
    return width,lines*14
  end
  ui.SetKeyboardFocusHere=function()S.setFocus=true end
  ui.IsItemFocused=function()return S.last==S.focus end
  ui.IsWindowFocused=function()return true end
  ui.InvisibleButton=function(id)
    S.last=id
    if S.setFocus then S.focusQueued=id;S.setFocus=false end
    return not disabled() and ((S.pad.a or S.pad.y) and S.focus==id or S.click==id)
  end
  ui.Button=function(label,width)
    local text=label:match("^(.-)###") or label
    local id=table.concat(S.ids,"/").."|"..(label:match("###(.*)$") or label)
    local tw,th=ui.CalcTextSize(text)
    local button={label=text,id=id,x=S.x,y=S.y,w=width or tw+20,h=th+10}
    S.buttons[#S.buttons+1]=button;S.lastRect=button
    S.labels[#S.labels+1]=text;S.events[#S.events+1]="button:"..text;S.last=id
    if S.setFocus then S.focusQueued=id;S.setFocus=false end
    S.x,S.y=0,button.y+button.h+6
    return not disabled() and (S.click==text or S.click==id or S.pad.a and S.focus==id)
  end
  ui.BeginDisabled=function(off)S.disabled[#S.disabled+1]=off end
  ui.EndDisabled=function()check(#S.disabled>0,"disabled scope balanced");table.remove(S.disabled)end
  ui.PushID=function(id)S.ids[#S.ids+1]=tostring(id)end
  ui.PopID=function()check(#S.ids>0,"ID scope balanced");table.remove(S.ids)end
  ui.PushStyleVar=function(key,x,y)S.styles[#S.styles+1]={key,x,y}end
  ui.PopStyleVar=function(count)for _=1,count or 1 do check(#S.styles>0,"style scope balanced");table.remove(S.styles)end end
  ui.BeginChild=function()S.children=S.children+1;return true end
  ui.EndChild=function()S.children=S.children-1;check(S.children>=0,"child scope balanced")end
  ui.TextWrapped=function(s)S.lines[#S.lines+1]=s;S.y=S.y+28 end
  ui.TextDisabled=function(text)S.lines[#S.lines+1]=text;S.events[#S.events+1]="heading:"..text;S.y=S.y+20 end
  ui.TextColored=nop;ui.Separator=function()S.events[#S.events+1]="separator";S.y=S.y+4 end
  ui.Spacing=function()S.y=S.y+4 end
  ui.Dummy=function(_,h)S.y=S.y+(h or 0)end
  ui.SameLine=function(_,gap)if S.lastRect then S.x=S.lastRect.x+S.lastRect.w+(gap or 8);S.y=S.lastRect.y end end
  ui.NewLine=nop;ui.Foreground=nop
  ui.DrawRect=nop;ui.DrawRectFilled=nop;ui.DrawCircle=nop;ui.DrawCircleFilled=nop;ui.DrawLine=nop
  ui.SetTooltip=nop;ui.GetMousePos=function()return -100,-100 end
  ui.IsItemHovered=function()return false end;ui.IsItemActive=function()return false end
  ui.IsItemClicked=function()return false end;ui.IsMouseDown=function()return false end
  ui.IsMouseDragging=function()return false end;ui.IsMouseHoveringRect=function()return false end
  ui=strict(ui,"ui")
  local color={text={1,1,1},muted={.5,.5,.5},faint={.5,.5,.5},ink={0,0,0},accent={0,1,1},gold={1,1,0},
    mana={0,0,1},exp={1,0,1},sunken={0,0,0},dim={.5,.5,.5},heading={1,1,1},good={0,1,0},danger={1,0,0}}
  local T={C=color,color=color,S=1,px=function(x)return x end,frame=nop,font={small=2,tiny=1,normal=3},
    TextSize=ui.CalcTextSize,Text=function(_,_,text)return #tostring(text)*7 end,Fit=function(s)return s end,
    Panel=nop,Hit=function()return false end,Button=function()return false end,TakeRightClick=nop,
    draw=nop,textIn=nop,rect=nop,unfont=nop,fit=function(s)return s end}
  local pad={navVisible=function()return true end,yActivated=function(clicked)return clicked and S.pad.y end,
    focused=function()return ui.IsItemFocused()end,menuPressed=function(key)return S.pad[key] or false end,set_mode=nop}
  local W,K,E
  GPSTATE={drawn={},closeAsked={},padSettleUntil=0}
  dofile=function(path)
    if path=="lib/extensions.lua"then
      if not E then E=realDofile(CLIENT_ROOT.."/ui/"..pack.."/lib/extensions.lua")end
      return E
    end
    if path=="lib/theme.lua"then return T end
    if path=="lib/pad.lua"then return pad end
    if path=="lib/window.lua"or path=="lib/widgets.lua"then return W end
    if path=="lib/keyboard.lua"then return K end
    if path=="lib/hud.lua"then return {T=T,Dialog=function(_,_,_,body)if S.shown then body(0,0,900,700,1)end end}end
    if path=="lib/social.lua"then return {
      Rows=function(_,list)local rows={};for row,data in ipairs(S.rows[list])do rows[#rows+1]={row=row-1,texts=data}end;return rows,list==S.selectedList and S.selected or -1 end,
      ClassAbbr=function(s)return s end,ClassColor=function()return {1,1,1}end,ButtonHeight=function()return 24 end,
      Chip=nop,Search=function(_,s)return s end,TestClicks=nop,Empty=nop}end
    error("Unexpected helper "..path)
  end
  if pack=="gamepad"then
    -- Execute the real theme/button and side navigation, replacing only drawing
    -- and outer-layout methods irrelevant to this synthetic controller fixture.
    local themeFile=assert(io.open(CLIENT_ROOT.."/ui/gamepad/lib/theme.lua"))
    local themeSource=themeFile:read("*a");themeFile:close()
    local first=assert(themeSource:find("function T.settle(",1,true))
    local last=assert(themeSource:find("-- An item slot",first,true))
    assert(load("local T,C=".."...\n"..themeSource:sub(first,last-1),"theme-buttons","t",_G))(T,color)
    T.px=function(x)return x end;T.font=nop;T.outline=nop;T.focusRing=nop
    W=realDofile(CLIENT_ROOT.."/ui/gamepad/lib/window.lua")
    W.begin=function()return true end;W.area=function()return 0,0,900,700,900 end
    W.finish=function()return S.pad.b or false end
    W.list=function(_,state,_,_,_,_,draw)
      if state.focusNext then S.focus="memberlist:1";state.focusNext=nil end
      state.offset,state.selected=0,1
      draw(1,0,0,500,56,S.focus=="memberlist:1")
      return S.focus=="memberlist:1" and 1 or nil
    end
    K={new=function()return {}end,draw=nop,open=function()error("No keyboard was requested")end}
  else
    W={Text=windows.Text,ButtonRow=nop,Tabs=function()return 0 end,Heading=nop,List=nop,Edit=nop,Button=nop,Stml=nop}
  end
  S.helper=dofile("lib/extensions.lua")
  realDofile(CLIENT_ROOT.."/ui/"..pack.."/raid.lua")
  function S.draw(input,click)
    S.frame=S.frame+1;S.time=S.time+.016;S.pad=input or {};S.click=click
    S.x,S.y=0,0;S.labels={};S.lines={};S.buttons={};S.events={};S.lastRect=nil
    assert(S.windows.Raid)()
    if S.focusQueued then S.focus=S.focusQueued;S.focusQueued=nil end
    eq(S.children,0);eq(#S.disabled,0);eq(#S.ids,0);eq(#S.styles,0)
  end
  function S.drawHelper(focus)
    S.x,S.y=0,0;S.labels={};S.lines={};S.buttons={};S.events={};S.lastRect=nil
    E.Draw("RaidWindow","member_actions",E.RaidContext(),focus)
    if S.focusQueued then S.focus=S.focusQueued;S.focusQueued=nil end
    eq(#S.disabled,0);eq(#S.ids,0);eq(#S.styles,0)
  end
  return S
end
-- The shared native-button layout wraps at the actual child width, without
-- cropping labels or changing the item's ID as line breaks change.
for _,pack in ipairs({"modern","gamepad"})do
  local S=rig(pack);local E=S.helper
  S.drawHelper();eq(#S.buttons,3);eq(S.events[1],"before");eq(S.events[#S.events],"after")
  eq(S.lines[1],"Addon actions");eq(#S.lines,1);eq(#S.calls,0)
  local first=S.buttons[1]
  for index,button in ipairs(S.buttons)do
    eq(button.y,first.y,"wide child lays actions out on one row")
    check(button.x+button.w<=S.width,"action stays inside available width")
    if index>1 then check(button.x>=S.buttons[index-1].x+S.buttons[index-1].w+8,"buttons have a clear gap")end
  end
  S.width=320;S.drawHelper()
  eq(S.buttons[1].y,S.buttons[2].y);check(S.buttons[3].y>S.buttons[2].y);eq(S.buttons[3].x,0)
  S.width=180;S.drawHelper()
  for index,button in ipairs(S.buttons)do
    eq(button.x,0);check(button.w<=S.width)
    if index>1 then check(button.y>=S.buttons[index-1].y+S.buttons[index-1].h,"narrow rows never overlap")end
  end
  S.actionLabels.grant="Grant a carefully reviewed exceptionallylongunspacedactionname"
  S.width=900;S.drawHelper();local stableId=S.buttons[2].id
  S.width=180;S.drawHelper();local long=S.buttons[2]
  eq(long.id,stableId,"line wrapping keeps the native item ID stable")
  eq(long.label:gsub("%s",""),S.actionLabels.grant:gsub("%s",""),"all action text remains readable")
  check(long.label:find("\n",1,true));check(long.h>24);check(long.w<=S.width)
  S.actionLabels.grant=string.rep(utf8.char(0xe9),30);S.width=90;S.drawHelper()
  check(utf8.len(S.buttons[2].label)~=nil,"wrapping never splits a UTF-8 character")
  eq(S.buttons[2].label:gsub("%s",""),S.actionLabels.grant)
  S.actionLabels.grant=nil
  S.width=900;local context=E.RaidContext();E.Activate(E.Rows("RaidWindow","member_actions",context)[2],context)
  S.drawHelper(true);eq(#S.buttons,5);check(S.buttons[4].y>S.buttons[3].y,"review controls get their own row")
  eq(S.buttons[4].y,S.buttons[5].y);check(S.focus:find(":cancel:",1,true),"review focus still defaults to Cancel")
  S.pad={a=true};S.drawHelper();eq(S.calls[#S.calls].phase,"cancel","native focus can activate review Cancel")
  S.pad={};S.present=false;S.drawHelper();eq(#S.buttons,0);eq(#S.lines,0,"no action heading for display-only slots")
  eq(S.events[1],"before");eq(S.events[#S.events],"after")
end
for _,pack in ipairs({"modern","gamepad"})do
  local S=rig(pack);local E=S.helper
  S.draw();eq(#S.calls,0,"painting and listing never invokes an action")
  check(table.concat(S.lines,"|"):find("Addon actions",1,true),"actual Raid separates addon actions from stock controls")
  local context=E.RaidContext();eq(context.selection.name,"Offzone");eq(context.selection.group,nil)
  local rows=E.Rows("RaidWindow","member_actions",context)
  eq(#rows,3);check(rows[1].enabled and rows[2].enabled)
  E.Activate(rows[2],context);eq(#S.calls,1);eq(S.calls[1].phase,"review");eq(S.calls[1].name,"Offzone")
  S.draw();eq(#S.calls,1,"review paint does not confirm")
  rows=E.Rows("RaidWindow","member_actions",E.RaidContext());eq(#rows,5);eq(rows[4].kind,"confirm");eq(rows[5].kind,"cancel")
  E.Activate(rows[4],context);eq(#S.calls,2);eq(S.calls[2].phase,"confirm");eq(S.calls[2].token,"one-use-token")
  E.Activate(rows[4],context);eq(#S.calls,2,"review token cannot be reused")
  rows=E.Rows("RaidWindow","member_actions",E.RaidContext());E.Activate(rows[2],E.RaidContext())
  local before=#S.calls;S.rows.RAID_PlayerList[1][6]="Changed roster rank"
  S.draw();check(not E.HasReview());eq(#S.calls,before,"roster changes discard locally, never cancel from paint")
  E.Activate(rows[2],context);eq(#S.calls,before,"a stale action row cannot invoke against a new context")
  rows=E.Rows("RaidWindow","member_actions",E.RaidContext());E.Activate(rows[2],E.RaidContext())
  S.allowed=false;before=#S.calls;S.draw();check(not E.HasReview());eq(#S.calls,before,"live permission loss is callback-free")
  S.allowed=true;rows=E.Rows("RaidWindow","member_actions",E.RaidContext());S.allowed=false
  E.Activate(rows[2],E.RaidContext());eq(#S.calls,before,"activation rechecks a permission lost after paint")
  S.allowed=true;rows=E.Rows("RaidWindow","member_actions",E.RaidContext());E.Activate(rows[2],E.RaidContext())
  S.generation=2;before=#S.calls;S.draw();check(not E.HasReview());eq(#S.calls,before,"owner replacement drops pending review")
  rows=E.Rows("RaidWindow","member_actions",E.RaidContext());E.Activate(rows[2],E.RaidContext())
  S.selectedList,S.selected="RAID_PlayerList",0;before=#S.calls;S.draw();check(not E.HasReview());eq(#S.calls,before)
  rows=E.Rows("RaidWindow","member_actions",E.RaidContext());E.Activate(rows[2],E.RaidContext())
  S.shown=false;before=#S.calls;S.draw();check(not E.HasReview());eq(#S.calls,before,"window close does not call cancel")
  S.shown=true;S.draw();rows=E.Rows("RaidWindow","member_actions",E.RaidContext())
  S.badResponse=true;E.Activate(rows[2],E.RaidContext());check(not E.HasReview(),"truthy nonboolean ok cannot start a review")
  S.badResponse=false
  rows=E.Rows("RaidWindow","member_actions",E.RaidContext());E.Activate(rows[2],E.RaidContext())
  before=#S.calls;E.Back("RaidWindow","member_actions",E.RaidContext());eq(#S.calls,before+1);eq(S.calls[#S.calls].phase,"cancel")
  rows=E.Rows("RaidWindow","member_actions",E.RaidContext());E.Activate(rows[2],E.RaidContext())
  before=#S.calls;S.present=false;S.draw();check(not E.HasReview());eq(#S.calls,before,"owner removal clears locally")
end
-- Dispatch is also exercised through the two actual Raid paint callbacks, not
-- only by calling helper methods directly. No Raid Bars API exists in this rig.
for _,pack in ipairs({"modern","gamepad"})do
  local S=rig(pack)
  local grant=pack=="modern" and "Grant Raid Manager" or "##extension:extension:1:grant"
  local confirm=pack=="modern" and "Confirm" or "##extension:confirm:extension:1:grant"
  S.draw({},grant);eq(#S.calls,1);eq(S.calls[1].phase,"review")
  S.draw({},confirm);eq(#S.calls,2);eq(S.calls[2].phase,"confirm")
  S.draw({},confirm);eq(#S.calls,2,"the presenter cannot dispatch a consumed confirmation")
end
-- Actual Gamepad Raid row list: right enters its native actions, down reaches
-- Grant, A reviews, focus defaults Cancel, up reaches Confirm, and B cancels.
do
  local S=rig("gamepad")
  local function step(key)S.draw(key and {[key]=true}or{});S.draw()end
  S.draw();step("right")
  for _=1,5 do step("down")end
  check(S.focus:find(":grant",1,true),"D-pad must reach Grant through all native and extension rows")
  local before=#S.calls;GPSTATE.padSettleUntil=S.time+1;S.draw({a=true});eq(#S.calls,before,"opening-A settling is preserved")
  GPSTATE.padSettleUntil=0;S.draw({y=true});eq(#S.calls,before,"Y activation is suppressed")
  S.draw({a=true});eq(S.calls[#S.calls].phase,"review");S.draw();S.draw()
  check(S.focus:find(":cancel:",1,true),"review defaults focus to Cancel")
  step("up");check(S.focus:find(":confirm:",1,true))
  S.draw({a=true});eq(S.calls[#S.calls].phase,"confirm")
  S.draw();step("right");for _=1,5 do step("down")end
  S.draw({a=true});eq(S.calls[#S.calls].phase,"review")
  S.draw();S.draw();S.draw({a=true});eq(S.calls[#S.calls].phase,"cancel","A activates the default Cancel row")
  S.draw();step("right");for _=1,5 do step("down")end
  S.draw({a=true});eq(S.calls[#S.calls].phase,"review")
  S.draw();S.draw();S.draw({b=true});eq(S.calls[#S.calls].phase,"cancel");check(S.shown,"B cancels review before closing Raid")
end
dofile=realDofile
print("UI extensions presenters: "..checks.." assertions passed")
