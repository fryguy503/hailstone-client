-- Load each actual skin with a narrow mock. Unknown game APIs fail immediately;
-- a hostile disabled-button mock still cannot bypass the UI's action guards.
CLIENT_ROOT = CLIENT_ROOT or (arg and arg[1]) or "."
local attachUI = assert(loadfile(CLIENT_ROOT .. "/tests/lua_ui_api_mock.lua"))()
local checks = 0
local function check(value, why) checks = checks + 1; assert(value, why) end
local function key(label)
  return label:match("^.*###(.*)$") or label:match("^##(.*)$") or label
end
for _, pack in ipairs({"modern", "gamepad"}) do
  local windows, clicks, lines, actions, shortcuts = {}, {}, {}, {}, {}
  local labels,children,lineScopes,iconRequests,images,tooltips={},{},{},{},{},{}
  local currentChild,childStack,contentWidth,contentHeight=nil,{},900,600
  local availableWidth,availableHeight,ioWidth,ioHeight=900,600,1920,1080
  local modernSize,modernPosition
  local styleColors,styleVars,comboOpen,lastWidget=0,0,false,nil
  local radioOutlines,radioSelected=0,0
  local draws={}
  local lastText,clickedText,hoverAll=nil,nil,false
  local clipStack={}
  local paddingStack,themedCaptions={},{}
  local fullReads, unchangedReads = 0, 0
  local frameClose, inGame = false, true
  local revision = "18446744073709551614"
  local token = "18446744073709551615"
  local previewToken = "9007199254740993"
  local function entries()
    return {
      {entry_id=10,kind_label="Money",description="Literal <gold> & coins",details="12 platinum, 3 gold"},
      {entry_id=11,kind_label="Experience",description="Training",details="Experience bonus"},
      {entry_id=12,kind_label="AA",description="Ability",details="3 AA points"},
      {entry_id=13,kind_label="Points",description="Faction",details="7 points"},
      {entry_id=14,kind_label="Item",description="Plain /cast text; no action",items={{id=100000,icon=501,quantity=2,name="Reward <item>",lore=true}}},
    }
  end
  local function tab(tabToken, title)
    return {token=tabToken,title=title or "Provider",blocked=false,options={
      {wire_id=3,common=true,label="Guaranteed rewards",entries=entries()},
      {wire_id=0,common=false,label="Choice <one>",entries={{entry_id=20,kind_label="Item",description="Selected details",items={{id=65536,icon=502,quantity=1,name="Selected item"}}}}},
      {wire_id=4294967295,common=false,label="Other choice",entries={{entry_id=21,kind_label="Text",description="Other full details"}}},
    }}
  end
  local state={shown=true,
    pending={shown=true,revision=revision,tabs={tab(token)},claim_pending=false,claim_uncertain=false,inspect_pending=false,message=""},
    preview={shown=true,revision="9007199254740995",tabs={tab(previewToken,"Preview provider")},claim_pending=false,claim_uncertain=false,inspect_pending=false,message=""}}
  local function record(action, ...)
    actions[#actions+1]={action,...}; return true
  end
  local function copy(value)
    if type(value) ~= "table" then return value end
    local result = {}; for field, child in pairs(value) do result[field] = copy(child) end
    return result
  end
  hs=setmetatable({
    InGame=function()return inGame end,
    ItemIcon=function(icon)
      iconRequests[#iconRequests+1]=icon
      if icon==501 then return "known-item-texture",0.1,0.2,0.3,0.4 end
    end,
    RewardSelection=function(pendingRevision, previewRevision)
      local result = {shown=state.shown}
      for _, name in ipairs({"pending", "preview"}) do
        local lane=copy(state[name])
        local requested=pendingRevision
        if name=="preview" then requested=previewRevision end
        if requested and requested==lane.revision then
          lane.unchanged=true;lane.tabs=nil;unchangedReads=unchangedReads+1
        else fullReads=fullReads+1 end
        result[name]=lane
      end
      return result
    end,
    RewardSelectionInspect=function(...)return record("inspect",...)end,
    RewardSelectionClaim=function(...)return record("claim",...)end,
    RestorePendingRewards=function(...)return record("restore",...)end,
    RewardSelectionClose=function(...)return record("close",...)end,
    ShowWindow=function(...)return record("window",...)end,
    ReplaceWindow=function()return false end,
  },{__index=function(_,name)error("Unexpected game/action API: "..name)end})
  attachUI(hs)
  local function click(label,selected,width,height)
    local id=key(label);lastWidget=id
    labels[id]={label=label,selected=selected,width=width,height=height,child=currentChild}
    local value=clicks[id];clicks[id]=nil
    if value and id:match("^reward:")then comboOpen=false end
    if type(value)=="table"then return value.y>=0 and value.y<(height or 0)end
    return value==true
  end
  local function text(value)
    lastText=value
    lines[#lines+1]=value;lineScopes[#lineScopes+1]={text=value,child=currentChild}
  end
  local function drawing(kind,r,g,b,a,value)
    draws[#draws+1]={kind=kind,r=r,g=g,b=b,a=a,text=value,child=currentChild}
  end
  ui={
    window=function(name,fn)windows[name]=fn end,
    BeginDisabled=function()end,EndDisabled=function()end,
    Button=function(label,width,height)return click(label,false,width,height)end,
    Selectable=function(label,selected,_,width,height)return click(label,selected,width,height)end,
    SameLine=function()end,Separator=function()end,SeparatorText=text,Spacing=function()end,Dummy=function()end,
    BeginChild=function(id,width,height,_,flags)
      local padding=paddingStack[#paddingStack]
      children[#children+1]={id=id,width=width,height=height,parent=currentChild,flags=flags or 0,
        paddingX=padding and padding[1],paddingY=padding and padding[2]}
      childStack[#childStack+1]={contentWidth,contentHeight,currentChild};currentChild=id
      contentWidth=width and width>0 and width or math.max(1,contentWidth+(width or 0))
      contentHeight=height and height>0 and height or math.max(1,contentHeight+(height or 0));return true
    end,EndChild=function()
      local parent=table.remove(childStack);assert(parent,"Unbalanced reward child panes")
      contentWidth,contentHeight,currentChild=parent[1],parent[2],parent[3]
    end,
    BeginGroup=function()end,EndGroup=function()end,
    Text=text,TextWrapped=text,TextColored=function(r,g,b,a,value)text(value);drawing("text",r,g,b,a,value)end,
    PushFont=function()end,PopFont=function()end,
    PushStyleColor=function()styleColors=styleColors+1 end,PopStyleColor=function(count)styleColors=styleColors-(count or 1) end,
    PushStyleVar=function(_,x,y)styleVars=styleVars+1;paddingStack[#paddingStack+1]={x,y}end,
    PopStyleVar=function()styleVars=styleVars-1;table.remove(paddingStack)end,
    SetNextItemWidth=function()end,BeginCombo=function(label)
      if click(label)then comboOpen=not comboOpen end;return comboOpen
    end,EndCombo=function()end,
    IsItemHovered=function()return hoverAll end,IsItemFocused=function()return false end,
    IsItemClicked=function()
      if clickedText and lastText==clickedText then clickedText=nil;return true end
      return false
    end,
    SetTooltip=function(value)tooltips[#tooltips+1]=value end,
    Image=function(texture,width,height,u0,v0,u1,v1)lastText=nil;images[#images+1]={texture,width,height,u0,v0,u1,v1}end,
    DrawRectFilled=function()end,DrawRect=function()end,DrawText=function(x,y,r,g,b,a,value,size)
      text(value);drawing("title",r,g,b,a,value)
      local draw=draws[#draws];draw.x,draw.size,draw.clip=x,size,clipStack[#clipStack]
    end,
    DrawCircle=function(_,_,_,r,g,b,a)radioOutlines=radioOutlines+1;drawing("radio",r,g,b,a)end,
    DrawCircleFilled=function(_,_,_,r,g,b,a)radioSelected=radioSelected+1;drawing("selected-radio",r,g,b,a)end,
    PushClipRect=function(x0,y0,x1,y1)clipStack[#clipStack+1]={x0,y0,x1,y1}end,
    PopClipRect=function()table.remove(clipStack)end,PushTextWrapPos=function()end,PopTextWrapPos=function()end,
    CalcTextSize=function(text)return (utf8.len(text)or #text)*8,16 end,
    GetContentRegionAvail=function()return contentWidth,contentHeight end,
    GetCursorScreenPos=function()return 0,0 end,SetCursorScreenPos=function()end,
    GetIO=function()return {width=ioWidth,height=ioHeight,uiScale=1}end,
    Col={ChildBg=1,Button=2,ButtonHovered=3,ButtonActive=4,Text=5},StyleVar={WindowPadding=1},WindowFlags={NoScrollbar=1,NoScrollWithMouse=2},
  }
  local colors={text={0.9,0.92,0.95,1},muted={0.6,0.62,0.65,1},dim={0.6,0.62,0.65,1},
    gold={0.95,0.7,0.1,1},accent={0.1,0.65,0.9,1},onAccent={0.02,0.03,0.04,1},
    danger={0.9,0.1,0.1,1},warn={1,0.6,0.1,1},raised={0.1,0.12,0.14,1},
    window={0.04,0.05,0.06,1},selected={0.12,0.22,0.3,1},frame={0.14,0.15,0.17,1}}
  local theme
  theme={S=1,px=function(n)return math.floor(n*theme.S+0.5)end,frame=function()end,C=colors,
    button=function(id,caption,width,height,opts)
      themedCaptions[id]=caption
      return click(id,opts and opts.primary,width,height),false
    end}
  local original=dofile
  dofile=function(path)
    if pack=="modern" then
      check(path=="lib/hud.lua","Modern skin loaded an unexpected helper")
      return {T={color=colors,font={large=1}},Dialog=function(screen,title,opts,body)
        check(screen=="RewardSelectionWnd" and title=="Select Reward","Wrong stock reward window")
        modernSize={opts.size(1)}
        modernPosition=opts.place and {opts.place(ioWidth,ioHeight,modernSize[1],modernSize[2])}
        if frameClose then frameClose=false;opts.onClose() else body(0,0,availableWidth,availableHeight,1) end
      end}
    end
    check(path=="lib/window.lua","Gamepad skin loaded an unexpected helper")
    return {T=theme,pad={},shown=function(screen)check(screen=="RewardSelectionWnd","Wrong gamepad stock window");return inGame and state.shown end,
      begin=function()return true end,
      area=function()return 0,0,availableWidth,availableHeight,availableWidth,availableHeight end,
      finish=function()local closed=frameClose;frameClose=false;return closed end,
      pressed=function(name)local pressed=shortcuts[name];shortcuts[name]=nil;return pressed==true end}
  end
  assert(loadfile(CLIENT_ROOT.."/ui/"..pack.."/reward_selection.lua"))();dofile=original
  local draw=windows["Select Reward"];check(type(draw)=="function","Reward skin was not registered")
  local function frame()
    lines={};labels={};children={};lineScopes={};iconRequests={};images={};tooltips={};childStack={};clipStack={};paddingStack={};themedCaptions={};currentChild=nil;lastText=nil
    contentWidth,contentHeight=availableWidth,availableHeight
    radioOutlines,radioSelected=0,0
    draws={}
    draw();clicks={}
    check(#childStack==0 and #clipStack==0 and styleColors==0 and styleVars==0,"Reward rendering left child/style scopes unbalanced")
  end
  local function saw(text)
    for _,line in ipairs(lines)do if line==text then return true end end
    return false
  end
  local function child(id)for _,value in ipairs(children)do if value.id==id then return value end end end
  local function sawIn(text,id)
    for _,value in ipairs(lineScopes)do if value.text==text and value.child==id then return true end end
    return false
  end
  local function activate(id)clicks[id]=true;frame()end
  local function choose()
    activate("choice:pending:"..token..":0")
    activate("claimreward")
  end
  frame()
  check(#actions==0,"Drawing rewards performed a game action or automatic restore")
  local firstCard=child("##rewardcard:pending:"..token..":0")
  check(firstCard and firstCard.parent=="##rewardchoices" and firstCard.height>=80 and firstCard.height<=96 and
    labels["choice:pending:"..token..":0"].selected==false and not child("##rewardproviders") and not labels.rewardpage,
    "A single reward page wasted a provider list or did not offer an explicit, prominent unselected choice card")
  local hit=labels["choice:pending:"..token..":0"]
  check(hit.height>=firstCard.height-12,"Only the choice title, rather than the card body, could be selected")
  check(labels.claimreward.child=="##rewardfooter" and child("##rewardfooter").parent~= "##rewardcontent",
    "Claim controls were placed inside the scrolling grant breakdown")
  check(child("##rewardfooter").paddingX==8 and child("##rewardfooter").paddingY==8,
    "The action footer inherited oversized child padding that clips controller controls")
  check(labels.claimreward.height==(pack=="gamepad" and 42 or 30) and
    labels["inspect:pending:"..token..":3:5:1"].height==(pack=="gamepad" and 38 or 28),
    "Claim or Inspect lost the pack's readable action height and controller hit target")
  check(child("##rewardchoices") and child("##rewarddetails") and
    sawIn("Included with every choice","##rewarddetails") and sawIn("Guaranteed rewards","##rewarddetails"),
    "Common grants were not separated and labelled in the full breakdown")
  check(radioOutlines==2 and radioSelected==0,"Choice cards lacked an explicit unselected radio affordance")
  local knownIcon=false
  for _,image in ipairs(images)do knownIcon=knownIcon or (image[1]=="known-item-texture" and image[4]==0.1 and image[7]==0.4)end
  check(knownIcon and saw("ITEM") and not saw("?"),"A known item icon or the neutral item tile was omitted")
  for _,icon in ipairs(iconRequests)do check(icon==501 or icon==502,"An item ID was guessed to be its icon ID")end
  check(saw("Guaranteed rewards") and saw("Included with every choice") and saw("Literal <gold> & coins") and
    saw("12 platinum, 3 gold") and saw("3 AA points") and saw("7 points") and saw("Reward <item> x2") and saw("Lore item"),
    "Common typed details, server literals, item quantity or lore were omitted")
  local initialFull=fullReads;frame()
  check(fullReads==initialFull and unchangedReads>=2 and saw("Guaranteed rewards"),"Matching revisions did not reuse cached tab data")
  activate("claimreward");activate("confirmreward")
  check(#actions==0,"Claim happened without selecting a choice and affirming a review")
  activate("inspect:pending:"..token..":3:5:1")
  local action=actions[#actions]
  check(action[1]=="inspect" and action[2]=="pending" and action[3]==revision and action[4]==token and
    action[5]==3 and action[6]==14 and action[7]==100000,"Common-item inspection lost exact binding")
  clickedText="Reward <item> x2";frame();action=actions[#actions]
  check(action[1]=="inspect" and action[7]==100000 and clickedText==nil,
    "Clicking the displayed item name did not explicitly inspect that item")
  table.remove(actions) -- keep later claim-count assertions focused on the original inspected item
  clicks["choice:pending:"..token..":0"]={y=70};frame();frame()
  check(labels["choice:pending:"..token..":0"].selected==true and #actions==1,
    "Activating the lower card body did not locally select its exact choice")
  choose();check(#actions==1,"Opening claim review sent the claim")
  frame();check(saw("Selected details") and labels["choice:pending:"..token..":0"].selected==true and
    sawIn("Selected choice","##rewarddetails") and labels.confirmreward.child=="##rewardfooter" and radioSelected==1,
    "Selected card emphasis, full grants or fixed confirmation controls were omitted")
  check(sawIn("Choice <one>","##rewardfooter"),
    "A short review label was truncated despite fitting the confirmation footer")
  if pack=="gamepad"then
    check(themedCaptions["##confirmreward"]=="" and themedCaptions["##pendingpage"]=="",
      "Bright primary actions retained a duplicate dark shadow label from the shared button theme")
  end
  local titleInk=pack=="gamepad" and colors.onAccent or colors.gold
  local summaryInk=pack=="gamepad" and colors.onAccent or colors.muted
  local titleSeen,summarySeen,radioSeen=false,false,false
  for _,drawn in ipairs(draws)do
    if drawn.child=="##rewardcard:pending:"..token..":0"then
      local expected
      if drawn.kind=="title" and drawn.text=="1. Choice <one>"then titleSeen=true;expected=titleInk
      elseif drawn.kind=="text" and drawn.text=="Selected item"then summarySeen=true;expected=summaryInk
      elseif drawn.kind=="selected-radio"then radioSeen=true;expected=titleInk end
      if expected then check(drawn.r==expected[1] and drawn.g==expected[2] and drawn.b==expected[3],
        "Selected reward overlay used accent/low-contrast text instead of the pack's contrast ink")end
    end
  end
  check(titleSeen and summarySeen and radioSeen,"Contrast coverage omitted the selected title, summary or radio")
  activate("confirmreward");action=actions[#actions]
  check(action[1]=="claim" and action[2]==revision and action[3]==token and action[4]==0 and type(action[2])=="string" and type(action[3])=="string",
    "Affirmative claim lost full-width decimal revision/tab identity or option zero")
  local before=#actions;frame();check(#actions==before,"Drawing after affirmation retried a claim")

  choose();state.pending.revision="18446744073709551613";activate("confirmreward")
  check(#actions==before,"A confirmation crossed a changed revision")
  state.pending.revision=revision;choose()
  local replaced=tab("18446744073709551612","Same numeric provider IDs")
  state.pending.tabs={replaced};state.pending.revision="8000000000000000000";activate("confirmreward")
  check(#actions==before,"A confirmation crossed a replaced tab token")
  state.pending.tabs={tab(token)};state.pending.revision=revision;choose()
  activate("choice:pending:"..token..":4294967295");activate("confirmreward")
  check(#actions==before and saw("Other full details") and not saw("Inspect items to review their full stats."),
    "Changing choice retained an old confirmation, omitted details or offered item instructions for a non-item reward")

  choose();activate("previewpage");activate("claimreward");activate("confirmreward")
  check(#actions==before and saw("Preview only. These rewards cannot be claimed here."),"Preview exposed a claim path")
  activate("inspect:preview:"..previewToken..":3:5:1");action=actions[#actions]
  check(action[1]=="inspect" and action[2]=="preview" and action[3]==state.preview.revision and action[4]==previewToken,
    "Preview inspection borrowed claimable lane identity")
  before=#actions;activate("pendingpage")
  for _,blocked in ipairs({"claim_pending","claim_uncertain","blocked"})do
    state.pending.claim_pending=false;state.pending.claim_uncertain=false;state.pending.tabs[1].blocked=false
    choose()
    local fullBefore=fullReads
    if blocked=="blocked" then
      state.pending.tabs[1].blocked=true;state.pending.revision="8000000000000000001"
    else state.pending[blocked]=true end
    activate("confirmreward");activate("claimreward")
    check(#actions==before,"Pending, uncertain or blocked reward admitted a claim")
    if blocked~="blocked" then check(fullReads==fullBefore,"Pending metadata rebuilt unchanged reward entry tables") end
  end
  check(saw("This reward is currently unavailable."),"Blocked reward omitted its status")
  state.pending.tabs[1].blocked=false;state.pending.revision=revision;state.pending.claim_uncertain=true;frame()
  check(saw("The claim result is uncertain. Wait for the server's result; restoring the list does not retry or unlock this claim."),
    "Uncertain status promised an automatic unlock/retry")
  activate("restorerewards");action=actions[#actions]
  check(action[1]=="restore" and #actions==before+1,"Explicit restore used a claim or repeated action")
  before=#actions;activate("claimreward");activate("confirmreward")
  check(#actions==before,"Restore unlocked an uncertain claim")
  state.pending.claim_uncertain=false;state.pending.inspect_pending=true
  activate("inspect:pending:"..token..":3:5:1")
  check(#actions==before and saw("Retrieving item details..."),"Pending inspection sent another query or hid status")
  state.pending.inspect_pending=false;state.pending.message="The server declined this reward.";frame()
  check(saw(state.pending.message),"Denied server status was omitted")

  choose();activate("restorerewards");before=#actions;activate("confirmreward")
  check(#actions==before,"Restore retained an old inline confirmation")
  activate("closerewardpage");action=actions[#actions]
  check(action[1]=="close" and action[2]=="pending","Page close targeted the wrong lane")
  choose();frameClose=true;frame();action=actions[#actions]
  check(action[1]=="window" and action[2]=="RewardSelectionWnd" and action[3]==false,"Frame close did not locally close both lanes")
  before=#actions;activate("confirmreward");check(#actions==before,"Reopening after close retained a confirmation")
  choose();inGame=false;frame();inGame=true;activate("confirmreward")
  check(#actions==before,"A confirmation crossed logout/zone visibility loss")
  choose();state.shown=false;frame();state.shown=true;local fullBefore=fullReads;activate("confirmreward")
  check(#actions==before,"A confirmation crossed a hidden window")
  check(fullReads==fullBefore+2,"Reopening reused an invalid hidden-window lane cache")
  if pack=="gamepad" then
    shortcuts.rb=true;frame();check(saw("Preview only. These rewards cannot be claimed here."),"Gamepad RB did not select preview")
    shortcuts.lb=true;frame();check(not saw("Preview only. These rewards cannot be claimed here."),"Gamepad LB did not return to pending")
  end
  state.pending.tabs={};state.pending.revision="7000000000000000000";fullBefore=fullReads;frame()
  check(fullReads>fullBefore and saw("No pending rewards are listed.") and not saw("Guaranteed rewards"),"New revision did not remove cached provider/details")
  state.pending.tabs={tab(token,"Restored provider")};state.pending.revision="7000000000000000001";frame()
  check(saw("Restored provider") and saw("Guaranteed rewards"),"New revision did not rebuild restored provider details")
  local secondToken="18446744073709551611"
  state.pending.tabs={tab(token,"First reward page"),tab(secondToken,"Second reward page")}
  state.pending.revision="7000000000000000002";before=#actions;frame()
  check(labels.rewardpage~=nil and labels.rewardpage.label=="###rewardpage" and saw("Reward page") and
    not child("##rewardproviders") and #actions==before,
    "Multiple reward pages did not use a compact passive chooser")
  activate("rewardpage");activate("reward:pending:"..secondToken)
  check(saw("Second reward page") and #actions==before,"Changing reward pages selected/claimed a choice or lost its title")
  activate("choice:pending:"..secondToken..":0");activate("claimreward")
  activate("rewardpage");activate("reward:pending:"..token);activate("confirmreward")
  check(#actions==before,"A review survived explicitly changing the selected reward page")
  activate("choice:pending:"..token..":3");activate("claimreward");activate("confirmreward")
  check(#actions==before,"A common-grant group became a selectable/claimable choice card")
  state.pending.tabs={tab(token,"One explicit choice")}
  table.remove(state.pending.tabs[1].options,3)
  state.pending.revision="7000000000000000003";frame()
  activate("claimreward");activate("confirmreward")
  check(#actions==before and labels["choice:pending:"..token..":0"].selected==false,
    "A sole selectable choice was automatically selected or claimed")
  for _,option in ipairs(state.pending.tabs[1].options)do
    for _,entry in ipairs(option.entries or {})do for _,item in ipairs(entry.items or {})do item.icon=nil end end
  end
  state.pending.revision="7000000000000000004";frame()
  check(#iconRequests==0 and #images==0 and saw("ITEM") and saw("Reward <item> x2") and #actions==before,
    "Missing icon metadata caused guessed lookups, lost item text or automatic inspection")
  check(sawIn("Review only. Select the card before claiming.","##rewarddetails") and
    sawIn("Selected details","##rewarddetails") and labels["choice:pending:"..token..":0"].selected==false,
    "A sole choice's received grants required scrolling past choices or were treated as selected")
  availableWidth,availableHeight=600,530;frame()
  local choices=child("##rewardchoices")
  check(choices.width==0 and choices.height>0 and choices.height<150 and child("##rewarddetails") and
    sawIn("Selected details","##rewarddetails") and #actions==before,
    "Narrow reward choices consumed the grant detail area or changed selection automatically")
  local noOuterScroll=ui.WindowFlags.NoScrollbar|ui.WindowFlags.NoScrollWithMouse
  check((child("##rewardcontent").flags&noOuterScroll)==noOuterScroll,
    "An outer content scroller competed with the independent choices and grant panes")
  availableWidth,availableHeight=900,600
  if pack=="modern"then
    ioWidth,ioHeight=960,720;frame()
    check(modernSize[1]<=ioWidth-60 and modernSize[2]<=560 and modernPosition and
      math.abs(modernPosition[2]-(ioHeight-modernSize[2])/2)<0.001 and #actions==before,
      "The default reward window exceeded a 720p viewport or performed an action while resizing")
  end
  if pack=="gamepad"then
    theme.S=2/3;ioHeight=720;availableWidth,availableHeight=640,460;frame()
    check(math.abs(child("##rewardfooter").height-156*theme.S)<0.001 and
      math.abs(child("##rewardcard:pending:"..token..":0").height-96*theme.S)<0.001 and
      math.abs(labels.claimreward.height-42*theme.S)<0.001 and
      (child("##rewardcontents").flags&noOuterScroll)==noOuterScroll and #actions==before,
      "Fractional controller scale over-sized fixed reward cards/footer or sent a background action")
    theme.S=1
  end

  -- Labels are measured at the actual drawn title size, and never cut through
  -- a multibyte name. The full name remains in the detail and hover views.
  local longName=string.rep("\195\137meraude ",40).."relic"
  local longChoice=state.pending.tabs[1].options[2]
  longChoice.label=longName
  longChoice.entries[1].items[1].name=longName
  longChoice.entries[1].items[1].quantity=7
  state.pending.revision="7000000000000000005"
  availableWidth,availableHeight=600,530;hoverAll=true
  before=#actions;frame()
  check(saw("Quantity: 7") and saw(longName.." x7") and #actions==before,
    "A one-item choice repeated its title, omitted its full item name or queried during drawing")
  local fullTooltip=false
  for _,value in ipairs(tooltips)do fullTooltip=fullTooltip or value:find(longName,1,true)~=nil end
  check(fullTooltip,"The truncated choice had no full-name tooltip")
  local measuredTitle=false
  for _,drawn in ipairs(draws)do
    if drawn.kind=="title" and drawn.clip and drawn.child=="##rewardcard:pending:"..token..":0" then
      measuredTitle=true
      check(utf8.len(drawn.text)~=nil and drawn.text:sub(-3)=="..." and drawn.clip and
        ui.CalcTextSize(drawn.text)*(drawn.size/16)<=drawn.clip[3]-drawn.x+0.01,
        "Long choice text overflowed its measured title rectangle or split UTF-8")
    end
  end
  check(measuredTitle,"Long-name coverage did not draw the choice title")
  activate("choice:pending:"..token..":0");activate("claimreward")
  frame()
  check(saw("Confirm your reward") and labels.confirmreward.height>=20 and #actions==before,
    "Reviewing a long-named choice sent a claim or lost its prominent confirmation")
  state.pending.inspect_pending=true;clickedText=longName.." x7";frame()
  check(#actions==before,"A pending inspection could be repeated by clicking the item name")
  clickedText=nil;hoverAll=false
  state.pending.inspect_pending=false
  longChoice.entries[1].items[2]={id=65537,quantity=3,name="Second item with its own full received name"}
  state.pending.revision="7000000000000000006";frame()
  check(saw(longName.." x7") and saw("Second item with its own full received name x3") and
    labels["inspect:pending:"..token..":0:1:1"] and labels["inspect:pending:"..token..":0:1:2"] and #actions==before,
    "A multi-item choice dropped full item names, merged Inspect identities or inspected automatically")
  activate("inspect:pending:"..token..":0:1:2");action=actions[#actions]
  check(#actions==before+1 and action[1]=="inspect" and action[5]==0 and action[6]==20 and action[7]==65537,
    "The second grant item's Inspect action borrowed the first item's identity")
end
print("Reward selection two-pack UI: "..checks.." checks passed")
