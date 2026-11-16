-- Exercise the actual pack scripts. Getters return copied data and omit cached
-- tables; unknown APIs and attempted background requests fail the fixture.
CLIENT_ROOT = CLIENT_ROOT or (arg and arg[1]) or "."
local attachUI = assert(loadfile(CLIENT_ROOT .. "/tests/lua_ui_api_mock.lua"))()
local checks = 0
local function check(value, why) checks = checks + 1; assert(value, why) end
local function copy(value)
  if type(value) ~= "table" then return value end
  local result = {}; for field, child in pairs(value) do result[field] = copy(child) end
  return result
end
local function key(label) return label:match("^.*###(.*)$") or label:match("^##(.*)$") or label end

for _, pack in ipairs({ "modern", "gamepad" }) do
  local windows, clicks, labels, labelOrder, lines, actions, edits = {}, {}, {}, {}, {}, {}, {}
  local lineScopes, labelScopes, children, tables, dialogs = {}, {}, {}, {}, {}
  local rowDraws, imageRequests, clips, imageSerial = {}, {}, {}, 0
  local artAvailable, variantsAvailable = true, true
  local availableWidth, availableHeight = 1000, 650
  local contentWidth, contentHeight, childStack, tableStack, lastWidget = availableWidth,availableHeight,{},{},nil
  local currentChild
  local hovered,tooltips
  local fontWidth,ioHeight,sameLinePending=8,1080,false
  local labelSizes,headerOpen,windowOptions={},{},{}
  local failure
  local inGame, closeWindow, currentWindow = true, nil, nil
  local catalogReads, stateReads, getterCalls = 0, 0, 0
  local token, revision = "18446744073709551615", "9007199254740993"
  local longDescription=string.rep("Literal category <text> & /cast is descriptive only. ",160)
  local function definition(index, id, name, reward)
    return { index = index, id = id, name = name, description = "Literal <description> & /cast", points = index + 1, has_reward = reward }
  end
  local state = { shown = true, ready = true, progress_ready = true, refresh_pending = false, message = "",
    catalog_token = token, state_revision = revision, selected_index = nil,
    points = 2, total_points = 15, completed = 1, total = 3, comparison = { pending = false, ready = false },
    catalog = { categories = {
      {id=55,parent_id=4294967295,name="First family",icon="FixtureWide",description=longDescription,display_order=2,
        associations={{achievement_id=100},{achievement_id=100},{achievement_id=300},{achievement_id=400},{achievement_id=4294967295}},child_ids={66}},
      {id=66,parent_id=55,name="Child family",icon="FixtureLeaf",description="Child description",display_order=0,
        associations={{achievement_id=4294967295},{achievement_id=4294967295}},child_ids={}},
      {id=77,parent_id=4294967295,name="Other family",icon="MissingImage",description="",display_order=1,associations={{achievement_id=200}},child_ids={}},
    }, definitions = {
      definition(0,100,"First open",true), definition(1,4294967295,"Completed <literal>",true),
      definition(2,200,"Locked entry",false), definition(3,300,"Hidden secret",true), definition(4,400,"Unknown secret",true),
    } },
    states = { {known=true,status="Open"}, {known=true,status="Completed",completion_time=12345},
      {known=true,status="Locked"}, {known=true,status="Hidden"}, {known=false} },
  }
  local function component(id, bucket, name, countKnown, count)
    return {id=id,type=bucket,name=name,description="Objective <literal> & /cast",required_count=10,
      satisfied_known=bucket~=3,satisfied=bucket==1,count_known=countKnown,count=count}
  end
  local function detailFor(index)
    local def = state.catalog.definitions[index + 1]
    return { shown = true, source = "own", revision = "18446744073709551614", catalog_token = state.catalog_token,
      definition_index=index,id=def.id,name=def.name,description=def.description,points=def.points,has_reward=def.has_reward,
      own={known=true,status="Completed",completion_time=12345,components={
        component(8,1,"Known objective",true,4),component(9,2,"Unknown count",false),component(10,3,"Presentation text",false)}},
      comparison={name="Other player",known=true,status="Open",components={
        component(8,1,"Known objective",false),component(9,2,"Unknown count",true,0),component(10,3,"Presentation text",false)}},
    }
  end
  local detail
  local function record(action, ...)
    actions[#actions+1] = {action,...}
    if failure then local reason=failure;failure=nil;return false,reason end
    return true
  end
  hs = setmetatable({
    InGame=function() return inGame end,
    UiAnimation=function(name)
      imageRequests[#imageRequests+1]=name
      if not artAvailable then return nil end
      local valid=name=="FixtureWide" or name=="FixtureLeaf" or variantsAvailable and
        (name=="FixtureWideHover" or name=="FixtureWideSelected")
      if not valid then return nil end
      imageSerial=imageSerial+1
      return {serial=imageSerial,name=name},0.1,0.2,0.5,0.8,name=="FixtureLeaf" and 80 or 128,name=="FixtureLeaf" and 20 or 32
    end,
    Achievements=function(cachedCatalog,cachedState)
      getterCalls=getterCalls+1
      local result=copy(state)
      result.detail_shown=detail and detail.shown or false
      result.detail_source=detail and detail.source or "own"
      if cachedCatalog==state.catalog_token then result.catalog=nil;result.catalog_unchanged=true else catalogReads=catalogReads+1 end
      if cachedState==state.state_revision then result.states=nil;result.states_unchanged=true else stateReads=stateReads+1 end
      return result
    end,
    Achievement=function() return copy(detail) end,
    AchievementOpen=function(currentToken,index)
      record("open",currentToken,index)
      if currentToken~=state.catalog_token then return false,"Catalog changed" end
      detail=detailFor(index);state.selected_index=index;return true
    end,
    AchievementRefresh=function() return record("refresh") end,
    AchievementCompare=function() return record("compare") end,
    AchievementClearCompare=function() return record("clearcompare") end,
    AchievementReward=function(...) return record("reward",...) end,
    AchievementLink=function(...) return record("link",...) end,
    ReplaceWindow=function() return false end,
    ShowWindow=function(screen,shown)
      record("window",screen,shown)
      if screen=="AchievementsWnd" then state.shown=shown elseif detail then detail.shown=shown end
    end,
  },{__index=function(_,name) error("Unexpected game/action API: "..name) end})
  attachUI(hs)
  local function click(label,caption,width,height)
    labels[key(label)]=caption or label
    labelScopes[key(label)]=currentWindow
    lastWidget=key(label)
    labelSizes[lastWidget]={width=width,height=height,caption=caption or label:match("^(.*)###") or label,
      sameLine=sameLinePending,parentWidth=contentWidth}
    sameLinePending=false
    labelOrder[#labelOrder+1]=key(label)
    local pressed=clicks[key(label)];clicks[key(label)]=nil
    return pressed==true
  end
  local function line(text)
    sameLinePending=false
    lines[#lines+1]=text
    lineScopes[#lineScopes+1]={text=text,window=currentWindow,after=lastWidget,child=currentChild}
  end
  ui = {
    window=function(name,fn) windows[name]=fn end,
    BeginDisabled=function() end,EndDisabled=function() end,
    Button=function(label,width,height) return click(label,nil,width,height) end,SmallButton=function(label) return click(label) end,
    Selectable=function(label,_,_,width,height) return click(label,nil,width,height) end,
    InputText=function(label,value) sameLinePending=false;local id=key(label);local nextValue=edits[id];edits[id]=nil;return nextValue or value end,
    SameLine=function()sameLinePending=true end,BeginChild=function(id,width,height,_,flags)
      children[#children+1]={id=id,width=width or 0,height=height or 0,flags=flags or 0,window=currentWindow,
        parentWidth=contentWidth,parentHeight=contentHeight}
      sameLinePending=false
      childStack[#childStack+1]={contentWidth,contentHeight,currentChild};currentChild=id
      contentWidth=width and width>0 and width or math.max(1,contentWidth+(width or 0))
      contentHeight=height and height>0 and height or math.max(1,contentHeight+(height or 0))
      return true
    end,EndChild=function()
      local parent=table.remove(childStack);assert(parent,"Unbalanced child panes");contentWidth,contentHeight,currentChild=parent[1],parent[2],parent[3]
    end,
    BeginTable=function(id,columns)
      local value={id=id,columns=columns,window=currentWindow,headings={}}
      tables[#tables+1]=value;tableStack[#tableStack+1]=value;return true
    end,EndTable=function()assert(table.remove(tableStack),"Unbalanced table")end,
    TableNextRow=function()local value=tableStack[#tableStack];value.rows=(value.rows or 0)+1 end,TableNextColumn=function() end,
    TableSetupColumn=function(label)local value=tableStack[#tableStack];value.headings[#value.headings+1]=label end,
    TableHeadersRow=function()for _,label in ipairs(tableStack[#tableStack].headings)do line(label)end end,
    Text=line,TextWrapped=line,TextDisabled=line,TextColored=function(_,_,_,_,text)line(text)end,
    PushClipRect=function(x0,y0,x1,y1)clips[#clips+1]={x0=x0,y0=y0,x1=x1,y1=y1}end,
    PopClipRect=function()assert(table.remove(clips),"Unbalanced drawing clip")end,
    DrawText=function(x,y,_,_,_,_,text)
      rowDraws[#rowDraws+1]={kind="text",text=text,x=x,y=y,after=lastWidget,clip=clips[#clips]}
      line(text)
    end,
    DrawImage=function(texture,x0,y0,x1,y1,u0,v0,u1,v1)
      rowDraws[#rowDraws+1]={kind="image",texture=texture,x=x0,y=y0,x1=x1,y1=y1,u0=u0,v0=v0,u1=u1,v1=v1,after=lastWidget,clip=clips[#clips]}
    end,
    Separator=function() end,SeparatorText=line,Spacing=function() end,Dummy=function() end,
    Indent=function() end,Unindent=function() end,PushID=function() end,PopID=function() end,
    SetNextItemWidth=function() end,SetItemDefaultFocus=function() end,SetScrollHereY=function() end,
    IsItemFocused=function()return false end,IsItemHovered=function()return hovered~=nil and lastWidget==hovered end,
    SetTooltip=function(text)tooltips[#tooltips+1]=text end,
    CollapsingHeader=function(label)
      local id=tostring(currentWindow)..":"..key(label)
      if click(label)then headerOpen[id]=not headerOpen[id]end
      return headerOpen[id]==true
    end,
    ProgressBar=function(_,_,_,text)if text then line(text)end end,
    PushFont=function()end,PopFont=function()end,
    PushTextWrapPos=function()end,PopTextWrapPos=function()end,
    CalcTextSize=function(text) return #text*fontWidth,16 end,GetContentRegionAvail=function() return contentWidth,contentHeight end,
    GetCursorPos=function()return 0,0 end,SetCursorPos=function()end,
    GetCursorScreenPos=function() return 0,0 end,SetCursorScreenPos=function() end,
    GetIO=function()return {width=1920,height=ioHeight,uiScale=1}end,
    WindowFlags={NoScrollbar=1,NoScrollWithMouse=2},
    TableFlags={RowBg=1,BordersInnerH=2,SizingStretchProp=4,ScrollY=8,Resizable=16,BordersInnerV=32},
    TableColumnFlags={WidthFixed=1,WidthStretch=2},SelectableFlags={SpanAllColumns=1},
  }
  local keyboard, keyboardResult
  local K = { HINTS={},new=function() keyboard={open=false,text=""};return keyboard end,
    open=function(kb,_,text) kb.open=true;kb.text=text end,cancel=function(kb) kb.open=false end,
    draw=function(kb) if keyboardResult then local result=keyboardResult;keyboardResult=nil;kb.text="locked";return result end end }
  local color={1,1,1,1}
  local colors={text=color,dim=color,muted=color,accent=color,good=color,warn=color,faint=color,gold=color,pet=color,danger=color}
  local T
  T={S=1,px=function(n)return math.floor(n*T.S+0.5) end,button=function(id,caption,width,height,opts)
      local pressed=click(id,caption,width,height);labelSizes[key(id)].align=opts and opts.align;return pressed,false
    end,
    C=colors,font=function()end,unfont=function()end,frame=function()end}
  local original=dofile
  dofile=function(path)
    if pack=="modern" then
      if path=="lib/hud.lua" then
        return {T={color=colors,font={large=1}},Dialog=function(screen,_,opts,body)
          currentWindow=screen;dialogs[#dialogs+1]=screen
          if closeWindow==screen then closeWindow=nil;opts.onClose() else body(0,0,availableWidth,availableHeight,1) end
        end}
      end
      return original(CLIENT_ROOT.."/ui/"..pack.."/"..path)
    end
    if path=="lib/keyboard.lua" then return K end
    if path~="lib/window.lua"then return original(CLIENT_ROOT.."/ui/"..pack.."/"..path)end
    return {T=T,pad={},shown=function(screen) return screen=="AchievementsWnd" and state.shown or screen=="AchievementWnd" and detail and detail.shown end,
      begin=function(name,opts) currentWindow=name=="Achievements" and "AchievementsWnd" or "AchievementWnd";dialogs[#dialogs+1]=currentWindow;windowOptions[currentWindow]=opts;return true end,
      area=function()return 0,0,availableWidth,availableHeight,availableWidth,availableHeight end,
      finish=function()local close=closeWindow==currentWindow;if close then closeWindow=nil end;return close end}
  end
  assert(loadfile(CLIENT_ROOT.."/ui/"..pack.."/achievements.lua"))();dofile=original
  check(type(windows.Achievements)=="function" and type(windows.Achievement)=="function","Both stock achievement windows were not registered")
  local function frame(name)
    lines={};labels={};labelOrder={};lineScopes={};labelScopes={};children={};tables={};dialogs={};currentWindow=nil
    rowDraws={};imageRequests={};clips={}
    tooltips={};labelSizes={};sameLinePending=false
    contentWidth,contentHeight,childStack,tableStack,lastWidget=availableWidth,availableHeight,{},{},nil
    currentChild=nil
    windows[name or "Achievements"]()
    clicks={}
    check(#childStack==0 and #tableStack==0 and #clips==0,"A browser path left child/table/drawing scopes unbalanced")
  end
  local function activate(id,name) clicks[id]=true;frame(name) end
  local function saw(text) for _,line in ipairs(lines) do if line==text then return true end end;return false end
  local function sawIn(text,window)
    for _,line in ipairs(lineScopes)do if line.text==text and line.window==window then return true end end
    return false
  end
  local function dialogShown(window)for _,screen in ipairs(dialogs)do if screen==window then return true end end;return false end
  local function hasChild(id)for _,value in ipairs(children)do if value.id==id then return true end end;return false end
  local function sawInChild(text,id)
    for _,value in ipairs(lineScopes)do if value.text==text and value.child==id then return true end end
    return false
  end
  local function categoryCount(id,caption)
    for _,value in ipairs(lineScopes)do if value.after=="category:"..id and value.text==caption then return true end end
    return false
  end
  local function rowDraw(id,kind,text)
    for _,draw in ipairs(rowDraws)do
      if draw.after=="category:"..id and draw.kind==kind and (not text or draw.text==text)then return draw end
    end
  end
  local function requested(name)for _,value in ipairs(imageRequests)do if value==name then return true end end;return false end
  local function listed(index) return labels["achievement:"..state.catalog_token..":"..index]~=nil end
  frame()
  check(#actions==0,"Drawing sent an achievement request")
  check(listed(0) and listed(1) and listed(2) and not listed(3) and not listed(4),"Hidden/unknown state leaked or a known state was omitted")
  check(labels["filter:Open"] and labels["filter:Open"]:find("In progress",1,true),
    "The player-facing open filter did not use the clear In progress caption")
  check(saw("1 / 3 completed") and saw("2 achievement points"),"Known non-hidden completion/points summaries were omitted")
  check(categoryCount(55,"1 / 2") and categoryCount(77,"0 / 1"),
    "Category rollups counted duplicate associations, hidden/unknown states, or omitted descendants")
  local image,count=rowDraw(55,"image"),rowDraw(55,"text","1 / 2")
  check(image and count and image.texture.name=="FixtureWide" and image.u0==0.1 and image.v1==0.8,
    "Category did not resolve its received named XML animation and authored crop")
  check(math.abs((image.x1-image.x)/(image.y1-image.y)-4)<0.000001 and image.x1<count.x and
    math.abs((image.y+image.y1)/2-(count.y+8))<0.000001,
    "Category artwork was distorted or the count no longer shared its row and vertical center")
  check(not rowDraw(55,"text","First family"),"The badge's baked label was duplicated with overlapping category text")
  local fallback=rowDraw(77,"text","Other family")
  local fallbackCount=rowDraw(77,"text","0 / 1")
  check(fallback and fallbackCount and fallback.y==fallbackCount.y and fallback.clip.x1<fallbackCount.x,
    "Missing category art hid its name or allowed fallback text to overlap its same-row count")
  local previousHandle=image.texture.serial
  frame()
  check(rowDraw(55,"image").texture.serial~=previousHandle and requested("FixtureWide") and #actions==0,
    "The browser retained a possibly retired texture handle or sent a background action")
  artAvailable=false;frame()
  check(not rowDraw(55,"image") and rowDraw(55,"text","First family") and rowDraw(55,"text","1 / 2"),
    "A missing installed UI resource did not retain its readable name and count")
  artAvailable=true;frame()
  hovered="category:55";frame()
  check(tooltips[1]=="First family\n1 / 2 completed\n2 / 3 points",
    "Category tooltip totals did not deduplicate the full received descendant set")
  check(rowDraw(55,"image").texture.name=="FixtureWideHover","A hovered category did not use the XML hover badge")
  hovered=nil
  local compactRows=false
  for _,value in ipairs(tables)do
    if value.id=="##achievementrows"then compactRows=value.columns==3 and value.rows==3 end
  end
  check(compactRows,"Achievement results did not use one compact table row per visible name/status/points record")
  local positions={};for index,id in ipairs(labelOrder)do positions[id]=index end
  check(positions["category:77"]<positions["category:55"] and not labels["category:66"],
    "Category roots were not ordered or their descendants started expanded")
  local beforeExpand=#actions
  activate("expandcategory:55")
  positions={};for index,id in ipairs(labelOrder)do positions[id]=index end
  check(labels["category:66"] and positions["category:55"]<positions["category:66"] and #actions==beforeExpand,
    "The explicit chevron failed to reveal children locally in their parent hierarchy")
  activate("expandcategory:55");activate("category:55")
  check(not labels["category:66"] and listed(0) and listed(1) and not listed(2) and #actions==beforeExpand,
    "Selecting a collapsed parent expanded it, sent a request, or lost descendant results")
  frame();check(rowDraw(55,"image").texture.name=="FixtureWideSelected","The selected category did not use its XML selected badge")
  variantsAvailable=false;frame()
  check(requested("FixtureWideSelected") and rowDraw(55,"image").texture.name=="FixtureWide",
    "A missing selected badge did not fall back to the base XML animation")
  variantsAvailable=true;frame()
  local overview
  for _,child in ipairs(children)do if child.id=="##achievementoverview"then overview=child end end
  local noScroll=ui.WindowFlags.NoScrollbar|ui.WindowFlags.NoScrollWithMouse
  check(overview and (overview.flags&noScroll)==0 and sawInChild(longDescription,"##achievementoverview") and
    sawInChild("2 / 3 points","##achievementoverview") and
    sawInChild("Select an achievement to see its objectives, progress and reward.","##achievementoverview") and #actions==beforeExpand,
    "A long category description left its progress and selection prompt outside the accessible overview scroller")
  activate("category:all")
  frame();check(catalogReads==1 and stateReads==1 and listed(1),"Two unchanged caches were not retained independently")
  state.state_revision="9007199254740994";state.states[1].status="Completed";hovered="category:55";frame()
  check(catalogReads==1 and stateReads==2 and categoryCount(55,"2 / 2") and tooltips[1]=="First family\n2 / 2 completed\n3 / 3 points",
    "A state-only update retransmitted the catalog or left category completion cached")
  hovered=nil
  activate("filter:Open");check(not listed(0) and not listed(1) and not listed(2),"Open filter ignored received state")
  state.states[1].status="Open";state.state_revision="9007199254740995";frame();check(listed(0),"State revision did not refresh a filtered row")
  activate("filter:Completed");check(listed(1) and not listed(0),"Completed filter selected the wrong records")
  activate("filter:Locked");check(listed(2) and not listed(1),"Locked filter selected the wrong records")
  activate("filter:All");activate("category:55")
  check(listed(0) and listed(1) and not listed(2),"Parent category omitted descendant associations")
  activate("expandcategory:55")
  activate("category:66");check(listed(1) and not listed(0),"Child category did not filter associations")
  beforeExpand=#actions;activate("expandcategory:55")
  check(not labels["category:66"] and listed(1) and not listed(0) and #actions==beforeExpand,
    "Collapsing a selected descendant changed its active filter or sent a request")
  activate("expandcategory:55")
  activate("achievement:"..token..":1")
  local action=actions[#actions]
  check(action[1]=="open" and action[2]==token and type(action[2])=="string" and action[3]==1,
    "Filtered selection lost the received definition index or full-width token")
  local before=#actions;frame()
  check(#actions==before and sawIn("Literal <description> & /cast","AchievementsWnd") and
    sawIn("Objective <literal> & /cast","AchievementsWnd"),"Selected own objectives were not inline or drawing interpreted server literals/sent a request")
  check(saw("Your progress") and saw("Other player") and saw("Complete") and saw("4 / 10") and
    saw("Count unavailable") and saw("Incomplete") and saw("0 / 10") and saw("Additional information"),
    "Comparison lost known zero/unknown counts or invented type3 requirements")
  check(saw("Completed "..os.date("%b %d, %Y",12345)),"Completion timestamp was omitted instead of formatting the received time")
  detail.own.components[1].count_text="18446744073709551615";frame()
  check(saw("18446744073709551615 / 10") and saw("Complete"),"The exact received count text was rounded or used to infer completion")
  detail.own.components[1].count_text=nil
  detail.comparison.components[1],detail.comparison.components[2]=detail.comparison.components[2],detail.comparison.components[1]
  frame();check(not saw("Progress unavailable") and saw("0 / 10") and saw("Count unavailable"),
    "Comparison objectives were matched by row position instead of received bucket/component identity")
  detail.comparison.components[1],detail.comparison.components[2]=detail.comparison.components[2],detail.comparison.components[1]
  detail.own.components[3].count_known=true;detail.own.components[3].count=9999999;detail.own.components[3].required_count=99999
  detail.own.components[3].satisfied_known=true;detail.own.components[3].satisfied=false;frame()
  check(saw("1 of 2 known objectives satisfied") and not saw("9999999 / 99,999"),
    "Display-only text invented a numeric objective or changed the known objective total")
  frame("Achievement")
  check(not dialogShown("AchievementWnd") and not saw("Known objective") and #actions==before,
    "Selecting an own row opened a second floating detail while the main view was visible")
  activate("category:77")
  check(not saw("Known objective") and #actions==before,"A filtered-out selected row retained inline objectives or triggered an automatic selection")
  activate("category:66")
  check(sawIn("Known objective","AchievementsWnd") and #actions==before,"Restoring the selected row's category lost its inline detail")
  detail.comparison.components[1].id=999;frame()
  check(saw("Progress unavailable"),"Comparison matched a different component by row position")
  detail.comparison.components[1].id=8
  activate("achievementreward");action=actions[#actions]
  check(action[1]=="reward" and action[2]==token and action[3]==1,"Reward request used achievement ID or filtered position")
  activate("achievementlink");action=actions[#actions]
  check(action[1]=="link" and action[2]==token and action[3]==1,"Link action lost selected catalog identity")
  detail.has_reward=false;before=#actions;activate("achievementreward")
  check(#actions==before,"Hostile disabled-button press requested a nonexistent reward")
  detail.source="link";detail.player_name="Linked <player>";state.shown=false;frame("Achievement")
  check(saw("Linked by Linked <player>") and not saw("Your progress") and not saw("Other player"),"Linked detail leaked own comparison or required the main window")
  detail.has_reward=true;before=#actions;activate("achievementlink","Achievement");activate("achievementreward","Achievement")
  check(#actions==before,"Linked detail exposed own reward/share actions")
  state.shown=true;before=#actions;closeWindow="AchievementsWnd";frame()
  check(state.shown==false and detail.shown==true and #actions==before+1 and actions[#actions][2]=="AchievementsWnd",
    "Closing the main view hid or changed the independent linked snapshot")
  frame("Achievement");check(dialogShown("AchievementWnd") and saw("Linked by Linked <player>"),"A linked snapshot depended on the main window staying open")
  state.shown=true;detail=detailFor(1)
  activate("category:all");edits.achievementsearch="LOCKED";frame();check(listed(2) and not listed(1),"Search was not local and case insensitive")
  edits.achievementsearch="<literal>";frame();check(listed(1) and not listed(2),"Literal search treated pattern characters specially")
  before=#actions
  availableWidth,availableHeight=1600,800
  frame()
  check(hasChild("##achievementlistpane") and hasChild("##achievementdetailpane") and sawIn("Known objective","AchievementsWnd") and #actions==before,
    "The wide browser did not keep its result list and selected detail together in separate panes")
  availableWidth,availableHeight=pack=="modern" and 720 or 1000,pack=="modern" and 500 or 680
  frame()
  check(listed(1) and sawIn("Known objective","AchievementsWnd") and not dialogShown("AchievementWnd") and #actions==before,
    "A narrow browser lost the selected result/inline objective or opened a second detail window")
  check(hasChild("##achievementlist") and hasChild("##achievementdetailpane") and not hasChild("##achievementlistpane"),
    "The narrow browser squeezed its result list beside the detail instead of stacking scrolling panes")
  for _,child in ipairs(children)do
    check(type(child.width)=="number" and child.width==child.width and math.abs(child.width)<math.huge and
      (child.width<=0 or child.width<=child.parentWidth),"A responsive pane exceeded its parent width or had an invalid size")
  end
  availableHeight=440;frame()
  check(labels.achievementdescription and not saw("Literal <description> & /cast") and sawIn("Known objective","AchievementsWnd"),
    "Compact detail did not prioritize objectives with its description initially collapsed")
  activate("achievementdescription")
  check(sawIn("Literal <description> & /cast","AchievementsWnd") and sawIn("Known objective","AchievementsWnd") and #actions==before,
    "The explicit description toggle sent an action or hid the selected objectives")
  frame();check(saw("Literal <description> & /cast") and #actions==before,"The description did not retain its local expansion on passive draw")
  activate("achievementdescription")
  check(not saw("Literal <description> & /cast") and saw("Known objective") and #actions==before,
    "Collapsing the description sent an action or removed objective access")
  for _,child in ipairs(children)do
    if child.id=="##achievementlist"then
      check(child.height>0 and child.height<=child.parentHeight/3,"The compact result list consumed the objective detail's vertical space")
    elseif child.id=="##achievementdetailpane"then
      local noOuterScroll=ui.WindowFlags.NoScrollbar|ui.WindowFlags.NoScrollWithMouse
      check((child.flags&noOuterScroll)==noOuterScroll,"The detail pane retained a second outer scroll surface")
    end
  end
  fontWidth=22;availableWidth=720;frame()
  local rowWidth,wrapped=0,false
  for index,id in ipairs({"All","Open","Completed","Locked"})do
    local value=labelSizes["filter:"..id]
    check(value and value.width>=#value.caption*fontWidth,"A larger-font status caption was clipped by a fixed-width filter")
    if index>1 and value.sameLine then rowWidth=rowWidth+6+value.width
    else rowWidth=value.width;if index>1 then wrapped=true end end
    check(rowWidth<=availableWidth,"Status filters overflowed instead of wrapping within the available width")
  end
  check(wrapped and #actions==before,"Oversized filters did not wrap locally or drawing them sent an action")
  local chevron=labelSizes["expandcategory:55"]
  check(chevron and chevron.width==(pack=="gamepad" and 40 or 24),"Category chevrons lost the pack's intended hit target width")
  if pack=="gamepad"then check(chevron.align==0.5,"The controller chevron was not centered in its wider hit target")end
  fontWidth=8
  availableWidth,availableHeight=1000,650
  if pack=="gamepad"then
    T.S=2/3;ioHeight=720;availableHeight=560;frame()
    local opts=windowOptions.AchievementsWnd
    check(T.px(1)==1 and math.abs(opts.h*T.S-(ioHeight-60))<0.001,
      "The 720p gamepad window used rounded one-pixel scale instead of continuous viewport scale")
    local body
    for _,child in ipairs(children)do if child.id=="##achievementbrowser"then body=child end end
    check(body and math.abs(body.height-(availableHeight-110*T.S))<0.001 and #actions==before,
      "Fractional gamepad scale over-reserved the footer or caused a background action")
    T.S=1;ioHeight=1080;availableHeight=650
  end
  state.comparison.pending=true;state.refresh_pending=true;before=#actions
  activate("compareachievements");activate("refreshachievements")
  check(#actions==before and saw("Waiting for the comparison result."),"Pending request guard allowed replay or hid pending status")
  activate("clearachievementcompare");check(actions[#actions][1]=="clearcompare","Explicit Clear did not retire comparison")
  state.comparison.pending=false;state.comparison.ready=true;state.comparison.name="Compared player";state.comparison.message="Comparison status";frame()
  check(saw("Comparing with Compared player") and saw("Comparison status"),"Comparison metadata was not refreshed independently of caches")
  state.refresh_pending=false;activate("refreshachievements");check(actions[#actions][1]=="refresh","Explicit Refresh did not request data")
  activate("compareachievements");check(actions[#actions][1]=="compare","Explicit Compare did not use current target action")
  failure="The server is not ready.";activate("refreshachievements");frame()
  check(saw("The server is not ready."),"A rejected request's reason was not shown")
  state.ready=false;state.progress_ready=false;frame();check(not listed(0) and not listed(1) and saw("Waiting for achievement data."),"Partial initialization exposed entries as complete state")
  state.ready=true;frame();check(saw("Objective counts have not arrived yet."),"Unknown initial count readiness was hidden")
  state.progress_ready=true;state.message="Server status";state.comparison.message="Server status";frame()
  check(saw("Server status"),"Received model diagnostic was omitted")
  state.catalog_token="18446744073709551614";state.state_revision="9007199254740996"
  state.catalog.categories={
    {id=55,parent_id=4294967295,name="Replacement family",description="",display_order=0,associations={},child_ids={66}},
    {id=66,parent_id=55,name="Replacement child",description="",display_order=0,associations={{achievement_id=700}},child_ids={}},
  };state.catalog.definitions={definition(0,700,"Replacement catalog",false)};state.states={{known=true,status="Open"}}
  edits.achievementsearch="";frame()
  check(listed(0) and not listed(1) and catalogReads==2 and not labels["category:66"],
    "Catalog replacement retained stale category/definition tables or reused the old branch's expansion")
  before=#actions;activate("achievement:"..token..":1");check(#actions==before,"A stale row callback survived catalog replacement")
  if pack=="gamepad" then
    activate("achievementkeyboard");check(keyboard.open,"Gamepad search had no on-screen keyboard")
    keyboardResult="ok";frame();frame();check(not keyboard.open and not listed(0),"Gamepad keyboard did not apply its local filter")
    activate("achievementkeyboard");closeWindow="AchievementsWnd";before=#actions;frame()
    check(not keyboard.open and #actions==before,"B closed the window instead of canceling the search keyboard")
  end
  detail=detailFor(0);state.selected_index=0
  closeWindow="AchievementsWnd";before=#actions;frame()
  local closedMain,closedOwn=false,false
  for index=before+1,#actions do
    local closed=actions[index]
    check(closed[1]=="window" and closed[3]==false,"Closing the achievement browser performed an unrelated action")
    closedMain=closedMain or closed[2]=="AchievementsWnd";closedOwn=closedOwn or closed[2]=="AchievementWnd"
  end
  check(closedMain and closedOwn and not state.shown and not detail.shown,"Main close did not retire its own detail window")
  state.shown=false;detail=detailFor(0);frame("Achievement")
  check(dialogShown("AchievementWnd") and saw("Replacement catalog"),"An externally opened own detail could not float with the browser hidden")
  closeWindow="AchievementWnd";frame("Achievement");action=actions[#actions]
  check(action[1]=="window" and action[2]=="AchievementWnd" and action[3]==false,"Independent detail close targeted the wrong window")
  inGame=false;local previousCalls=getterCalls;before=#actions;frame();frame("Achievement")
  check(getterCalls==previousCalls and #actions==before,"Out-of-game draw queried or sent actions")
  inGame=true;state.shown=true;frame();check(catalogReads==3 and stateReads==5 and listed(0),"Logout retained catalog, state or search filters")
end
print("Achievements two-pack UI: "..checks.." checks passed")
