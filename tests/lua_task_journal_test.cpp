// Exercise journal interactions against the shipped Lua and its real widget/STML helpers. Rendering and native
// task-state integration are covered separately; this fixture supplies only ImGui calls and the model boundary.
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <lua.hpp>

TEST_CASE("UI quest journal: task identity, sections, tracking and native history actions") {
    const auto pack = std::filesystem::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    REQUIRE(std::filesystem::exists(pack / "tasks.lua"));
    const char* script = R"lua(
local base = ...
local real_dofile = dofile
local settings, texts, snapshotCalls = {}, {}, 0
local pending, hoveredText, lastText, linkPending
local frame, page, selectedHistory = 0, 0, -1
local inGame, nextSearch = true, nil
local opened, historyClicks, nativeAbandon, linked = {}, 0, 0, nil
local previewRequests = {}
local roster, memberSelected, leader = { {"Aster","Leader"}, {"Beryl",""}, {"Cyra",""} }, -1, false
local sharedActions, popup, pushedRow = {}, nil, nil
local buttonIds, headers = {}, {}
local disabled, disabledStack = false, {}
local draw, tableRows
local snapshot = {
  {id=10,type=2,title="Identical quest title",duration=0,left=0,done=0,total=3,rewardSet=true,
   description="First description",rewards='<a WndNotify="27,reward-link">RewardToken</a>',objectives={
     {text="First objective",status="0/1",zone="North Ro",count=0,goal=1,currentZone=true},
     {text="???",status="",zone="???",hidden=true},
     {separator=true},
     {text="Later objective",status="0/1",zone="Befallen",count=0,goal=1}}},
  {id=20,type=2,title="Identical quest title",duration=0,left=0,done=0,total=1,rewardSet=true,
   description="Second description",rewards="",objectives={
     {text="Second objective",status="0/1",zone="Najena",count=0,goal=1}}},
  {id=99,type=1,title="Shared expedition",duration=600,left=175,done=1,total=2,
   description="Shared description",rewards="Experience",objectives={
     {text="Shared complete",status="Done",zone="Upper Guk",count=1,goal=1,done=true},
     {text="Shared objective",status="2/4",zone="Upper Guk",count=2,goal=4}}},
  selectedId=10,requestTimerLeft=0,revision=1,
}

local function record(value) texts[tostring(value)] = true; lastText = tostring(value) end
local function hit(label)
  local id = label:match("##(.+)$") or label
  if pending == id or pending == label then pending=nil; return true end
  return false
end
local function noop() end
ui = {
  Col={Button=1,Text=2}, StyleVar={CellPadding=1},
  TableFlags={SizingFixedFit=1,RowBg=2,ScrollY=4,BordersInnerV=8,Resizable=16},
  TableColumnFlags={WidthStretch=1,WidthFixed=2}, Mouse={Left=0,Right=1},
  SelectableFlags={SpanAllColumns=1,AllowDoubleClick=2},
  Cond={Always=1},
  HoveredFlags={AllowWhenDisabled=1},
  GetFrameCount=function() return frame end,
  GetSetting=function(key) return settings[key] end,
  SetSetting=function(key,value) settings[key]=value end,
  GetIO=function() return {uiScale=1} end,
  GetContentRegionAvail=function() return 600,400 end,
  GetCursorScreenPos=function() return 0,0 end,
  GetCursorPos=function() return 0,0 end,
  GetWindowPos=function() return 0,0 end,
  GetWindowSize=function() return 600,400 end,
  GetScrollY=function() return 0 end,
  GetItemRect=function() return 0,0,80,13 end,
  CalcTextSize=function(value) return #tostring(value)*6,13 end,
  InputTextWithHint=function(_,_,value)
    if nextSearch~=nil then value=nextSearch;nextSearch=nil end
    return value,false
  end,
  Button=function(label)
    buttonIds[label:match("##(.+)$") or label]=true
    local clicked=hit(label)
    return clicked and not disabled
  end,
  Selectable=function(label)
    if label=="##row" and pushedRow~=nil then
      local member=roster[pushedRow+1]
      if member and pending=="member."..member[1] then pending=nil;return true end
    end
    return hit(label)
  end,
  InvisibleButton=function() return false end,
  TextColored=function(_,_,_,_,value) record(value) end,
  TextWrapped=record, Text=record,
  IsItemHovered=function() return linkPending and lastText==hoveredText end,
  IsItemClicked=function(mouse)
    if mouse==0 and linkPending and lastText==hoveredText then linkPending=false;return true end
    return false
  end,
  IsItemActive=function() return false end,
  IsMouseDoubleClicked=function() return false end,
  TableGetHoveredColumn=function() return 0 end,
  PushID=function(row) pushedRow=row end,
  PopID=function() pushedRow=nil end,
  TableSetupColumn=function(label) headers[label:match("^(.-)##") or label]=true end,
  OpenPopup=function(id) popup=id end,
  BeginPopup=function(id) return popup==id end,
  CloseCurrentPopup=function() popup=nil end,
  BeginDisabled=function(on) disabledStack[#disabledStack+1]=disabled;disabled=disabled or on end,
  EndDisabled=function() disabled=disabledStack[#disabledStack];disabledStack[#disabledStack]=nil end,
  BeginTable=function() tableRows=0;return true end,
  TableNextRow=function() tableRows=tableRows+1 end,
  window=function(name,fn) assert(name=="Tasks");draw=fn end,
}
for _,name in ipairs({"PushStyleColor","PopStyleColor","PushStyleVar","PopStyleVar","PushFont","PopFont",
  "PushTextWrapPos","PopTextWrapPos","SameLine","SetCursorScreenPos","SetCursorPos","SetNextItemAllowOverlap",
  "SetNextItemWidth","DrawRectFilled","DrawRect","DrawCircleFilled","DrawCircle","DrawLine","DrawText",
  "Dummy","Separator","BeginChild","EndChild","TableHeadersRow","TableNextColumn","TableSetupScrollFreeze",
  "EndTable","SetTooltip","EndPopup","SetNextWindowSize"}) do ui[name]=noop end

eq = {
  InGame=function() return inGame end,
  Tasks=function() snapshotCalls=snapshotCalls+1;return snapshot end,
  TabPage=function() return page end,
  SetTabPage=function(_,_,value) page=value end,
  OpenTask=function(id)
    for _,task in ipairs(snapshot) do
      if task.id==id then opened[#opened+1]=id;snapshot.selectedId=id;page=0;return true end
    end
    return false
  end,
  ListInfo=function(_,id)
    if id=="QUEST_TaskList" then return 1,3,selectedHistory end
    if id=="QUEST_TaskElementList" then return selectedHistory==0 and 1 or 0,3,-1 end
    if id=="STASK_MemberList" then return #roster,3,memberSelected,{"Member","Role","Template"},{130,100,200} end
    error("Unexpected list: "..id)
  end,
  ListRow=function(_,id,row)
    if id=="STASK_MemberList" then
      local member=roster[row+1]
      return member and {member[1],member[2],""} or nil
    end
    assert(row==0)
    if id=="QUEST_TaskList" then return {"Q","Completed quest","November 14"} end
    if id=="QUEST_TaskElementList" then return {"Historical objective","Done","North Ro"} end
  end,
  ListClick=function(_,id,row)
    if id=="STASK_MemberList" then memberSelected=row;return end
    assert(id=="QUEST_TaskList" and row==0)
    selectedHistory=row;historyClicks=historyClicks+1
  end,
  WindowText=function() return "" end,
  ChildShown=function() return true end,
  ChildState=function(_,id)
    if id:match("^STASK_") then return leader,false end
    if id=="TASK_PreviewRewardButton" then
      for _,task in ipairs(snapshot) do if task.id==snapshot.selectedId then return task.rewardSet==true,false end end
      return false,false
    end
    return true,false
  end,
  ClickChild=function(win,id,mouse)
    assert(win=="TaskWnd" and mouse=="left")
    if id=="TASK_PreviewRewardButton" then previewRequests[#previewRequests+1]=snapshot.selectedId;return end
    if id:match("^STASK_") then
      local member=roster[memberSelected+1]
      sharedActions[#sharedActions+1]={id=id,member=member and member[1]}
      return
    end
    assert(id=="TASK_AbandonButton")
    nativeAbandon=nativeAbandon+1
  end,
  StmlLink=function(win,child,message,data) linked={win,child,message,data} end,
}
local colors=setmetatable({}, {__index=function(t,key) local c={0.6,0.7,0.8,1};rawset(t,key,c);return c end})
local T={color=colors,font={small=3,normal=5,large=6}}
T.Text=function(_,_,value) record(value) end
T.TextSize=function(value) return #tostring(value)*6,13 end
T.Fit=function(value) return value end
T.Hit=function() return false end
T.DragFrom=function() return false end
local hud={T=T,Dialog=function(win,_,_,body) assert(win=="TaskWnd");body(0,0,820,560,1) end}
dofile=function(path)
  if path=="lib/hud.lua" then return hud end
  if path=="lib/theme.lua" then return T end
  return real_dofile(base.."/"..path)
end
dofile("tasks.lua")
local function render(click)
  frame=frame+1;texts={};buttonIds={};headers={};pending=click;tableRows=0;draw()
  assert(pending==nil,"The requested control was unreachable: "..tostring(pending))
end
)lua"
R"lua(
render()
assert(texts["First objective"] and texts["???"] and texts["Later objective"],"Each objective stays in its original position")
assert(tableRows==4,"The real group separator stays between objective rows")
assert(not texts["Step 1"] and not texts["Step 2"],"No fabricated phase headings")
assert(buttonIds["category.active"] and buttonIds["category.history"] and not buttonIds["category.shared"],
  "The journal has only Active and History; shared task objectives belong in Active")
render("Show tracker")
assert(settings["tasks.tracker.visible"]=="1","The initially empty tracker can be explicitly shown")
render("Hide tracker")
assert(settings["tasks.tracker.visible"]=="0","Its visibility is separate from the journal")

render("cardquest.20")
assert(opened[#opened]==20 and texts["Second objective"],"Duplicate titles must select by task identity")
render("section.rewards")
assert(texts["Reward details are available from the server."] and not texts["Reward preview unavailable."],
  "A server-owned reward set is previewable even without inline reward text")
render("TASK_PreviewRewardButton")
assert(#previewRequests==1 and previewRequests[1]==20,"Preview uses the selected task's native reward action")
render("section.description")
assert(texts["Second description"] and not texts["First description"],"Description follows selection")
render("cardquest.10")
assert(texts["First objective"],"Another task starts on Objectives")
render("cardquest.20")
assert(texts["Second description"],"Section choice is remembered per quest")
render("cardquest.10")
render("trackQuest")
assert(settings["tasks.tracked.10"]=="1" and settings["tasks.tracker.visible"]=="1","Tracking opens the independent tracker")
render("trackQuest")
assert(settings["tasks.tracked.10"]=="0","Untracking updates the shared watch flag")

render("section.rewards")
assert(texts["RewardToken"],"The actual supplied reward is shown")
hoveredText="RewardToken";linkPending=true
render()
assert(linked and linked[1]=="TaskWnd" and linked[2]=="TASK_Description" and linked[3]==27 and linked[4]=="reward-link",
  "Reward links preserve the native STML action")
render("TASK_PreviewRewardButton")
assert(#previewRequests==2 and previewRequests[2]==10,"Preview follows a changed selection and remains available alongside inline rewards")
render("TASK_AbandonButton")
assert(nativeAbandon==1,"Abandon goes to the game's confirmation handler")

snapshot.selectedId=99;page=0
render()
assert(texts["Shared objective"] and texts["Shared expedition"],"A tracker-opened task follows the authoritative selection")
render("TASK_PreviewRewardButton")
assert(#previewRequests==2,"The native reward-set flag disables preview when no server-owned set exists")
render("category.active")
assert(snapshot.selectedId==99 and texts["Shared objective"],"Active retains a selected shared task instead of filtering it out")
assert(not buttonIds["category.shared"],"Shared is not another quest category")

render("category.history")
assert(page==2,"History retains the native page")
render("cardhistory.0")
assert(historyClicks==1,"History requests the chosen native history row")
render()
assert(texts["Historical objective"],"Received history objectives are displayed")
render("section.rewards")
assert(texts["No reward details in completed-task history."],"History never invents unavailable reward data")
snapshot.selectedId=99;page=0
render()
assert(texts["Shared objective"],"Reopening the same native task still leaves History")
snapshot.selectedId=20;page=0
render()
assert(texts["Second description"],"Opening a tracked quest also leaves History and restores that quest's section")

nextSearch="no matching quest"
render()
assert(texts["No matching tasks."],"The outgoing character has a search filter")
inGame=false
render()
assert(next(texts)==nil,"The journal does not draw while logged out")
settings["tasks.category"]="shared"
settings["tasks.section.20"]="objectives"
snapshot.selectedId=20;page=0;inGame=true
render()
assert(settings["tasks.category"]=="active" and texts["Second objective"],"A legacy Shared preference migrates to Active without changing the selected quest")
assert(not texts["No matching tasks."],"The outgoing character's search does not hide the new character's quests")

inGame=false
render()
settings["tasks.category"]="history"
snapshot.selectedId=99;page=0;inGame=true
render()
assert(page==2 and texts["Completed quest"],"The next character's History preference selects the native history page")
snapshot.selectedId=99;page=0
render()
assert(texts["Shared objective"],"A direct OpenTask still leaves restored History for the same task ID")

local otherShared, onlyShared=snapshot[3],snapshot[2]
onlyShared.type=1
for i=#snapshot,1,-1 do snapshot[i]=nil end
snapshot[1]=onlyShared;snapshot.selectedId=20
settings["tasks.category"]="shared";settings["tasks.section.20"]="objectives"
render()
assert(texts["Second objective"],"The native task is the only shared quest")
render("category.active")
assert(texts["Second objective"] and not texts["No tasks here."],"Active includes a shared-only roster of quests")
eq.OpenTask(20)
settings["tasks.category"]="active";settings["tasks.section.20"]="objectives"
render()
assert(texts["Second objective"],"A tracker-opened shared task uses Active even when its native task ID is unchanged")

snapshot[1],snapshot[2]=otherShared,onlyShared
settings["tasks.tracked.99"]="1"
render("category.history")
eq.OpenTask(20)
settings["tasks.category"]="active"
render()
assert(texts["Second objective"] and not texts["Shared objective"],"An external category change follows the exact native task rather than the first tracked quest")

local taskRevision=snapshot.revision
render("sharedMembers")
assert(popup=="##sharedMembers" and texts["Aster"] and texts["Beryl"] and texts["Cyra"],"The header opens the native shared roster")
assert(headers.Member and headers.Role and not headers.Template,"The popup shows member and role without an unsupported template column")
assert(memberSelected==-1,"Opening the roster clears any previous member target")
for _,id in ipairs({"STASK_AddPlayerButton","STASK_RemovePlayerButton","STASK_MakeLeaderButton","STASK_InviteRaidButton","STASK_InviteExpedButton"}) do
  render(id)
end
assert(#sharedActions==0,"A nonleader cannot send any shared-task management action")
leader=true
render("STASK_RemovePlayerButton")
render("STASK_MakeLeaderButton")
assert(#sharedActions==0,"Even a leader must select a member before row actions")
for _,id in ipairs({"STASK_AddPlayerButton","STASK_InviteRaidButton","STASK_InviteExpedButton"}) do
  render(id)
  assert(sharedActions[#sharedActions].id==id,"The management control uses its matching native action")
end
render("member.Beryl")
render("STASK_RemovePlayerButton")
assert(sharedActions[#sharedActions].member=="Beryl","Remove uses the selected member")
roster={ {"Cyra",""}, {"Aster","Leader"}, {"Beryl",""}, {"Doran",""} }
render("STASK_RemovePlayerButton")
assert(sharedActions[#sharedActions].member=="Beryl" and texts["Doran"] and memberSelected==2,
  "A reordered or expanded roster preserves the selected name at its new native row")
assert(snapshot.revision==taskRevision,"Roster updates do not need a task revision bump")
local beforeRemoval=#sharedActions
roster={ {"Cyra",""}, {"Aster","Leader"} }
render("STASK_RemovePlayerButton")
render("STASK_MakeLeaderButton")
assert(#sharedActions==beforeRemoval and memberSelected==-1,"A departed selection never retargets the replacement row")
roster={ {"Cyra",""}, {"Beryl",""}, {"Aster","Leader"} }
render("STASK_RemovePlayerButton")
assert(#sharedActions==beforeRemoval,"A returning name is not selected automatically")
render("member.Cyra")
render("STASK_MakeLeaderButton")
assert(sharedActions[#sharedActions].id=="STASK_MakeLeaderButton" and sharedActions[#sharedActions].member=="Cyra",
  "Make leader uses an explicitly selected member")
render("closeSharedMembers")
assert(popup==nil,"Close dismisses the roster")

page=1
render()
assert(page==0 and popup=="##sharedMembers" and settings["tasks.category"]=="active",
  "A native Shared page request opens the roster popup and returns to Active")
assert(memberSelected==-1,"Reopening the popup clears the old target")
render("closeSharedMembers")
roster={}
for i=#snapshot,1,-1 do snapshot[i]=nil end
snapshot.selectedId=0
render("sharedMembers")
assert(texts["You are not in a shared task."],"An empty shared roster has an explanatory popup")
render("closeSharedMembers")
return true
)lua";

    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);
    int rc = luaL_loadstring(L, script);
    if (rc == LUA_OK) {
        const auto path = pack.generic_string();
        lua_pushlstring(L, path.data(), path.size());
        rc = lua_pcall(L, 1, 1, 0);
    }
    const std::string error = rc == LUA_OK ? "" : lua_tostring(L, -1);
    lua_close(L);
    CHECK_MESSAGE(rc == LUA_OK, error);
}
