-- Manual reward selection. The server alone grants rewards after confirmation.
local API = hs.UI
local hud=dofile("lib/hud.lua")
local C=hud.T.color
local look={
  colors={text=C.text,muted=C.muted,accent=C.gold,warning=C.danger or C.gold,card=C.raised or C.window,selected=C.selected or C.frame},
  cardHeight=84,cardTitleSize=18,footerHeight=116,reviewHeight=152,buttonPadding=24,inspectSize=36,actionHeight=30,inspectHeight=28,
  heading=function(text)ui.PushFont(hud.T.font.large);ui.TextWrapped(text);ui.PopFont()end,
  select=function(label,selected,width,height)return ui.Selectable(label,selected,0,width or 0,height or 0)end,
  button=function(label,enabled,width,primary,height)
    ui.BeginDisabled(not enabled)
    if primary and enabled then
      ui.PushStyleColor(ui.Col.Button,C.accent[1],C.accent[2],C.accent[3],1)
      ui.PushStyleColor(ui.Col.ButtonHovered,C.accent[1],C.accent[2],C.accent[3],0.85)
      ui.PushStyleColor(ui.Col.ButtonActive,C.accent[1],C.accent[2],C.accent[3],0.7)
      ui.PushStyleColor(ui.Col.Text,C.onAccent[1],C.onAccent[2],C.onAccent[3],1)
    end
    local clicked=ui.Button(label,width or 0,height or 0)
    if primary and enabled then ui.PopStyleColor(4)end
    ui.EndDisabled();return clicked
  end,
}

local WINDOW = "RewardSelectionWnd"
local laneName = "pending"
local selections = {pending={},preview={}}
local snapshots = {}
local review, actionMessage

local record = {} -- the state's record, refilled each frame rather than made anew (hs.RewardSelection's `into`)
local function readState()
  local state = API.rewards.Selection(snapshots.pending and snapshots.pending.revision, snapshots.preview and snapshots.preview.revision, record)
  if not state or not state.shown then
    if next(snapshots) then snapshots={} end -- (a new table only when there was something to drop)
    return state
  end
  for _,name in ipairs({"pending","preview"})do
    local lane=state[name] or {}
    if lane.unchanged then
      local cached=snapshots[name]
      lane.tabs=cached and cached.revision==lane.revision and cached.tabs or {}
    else lane.tabs=lane.tabs or {} end
    snapshots[name]={revision=lane.revision,tabs=lane.tabs}
    state[name]=lane
  end
  return state
end
local function cancelReview()review=nil end
local function changeLane(name)
  if laneName~=name then laneName=name;cancelReview();actionMessage=nil end
end
-- Called every frame while the window is hidden: new tables only when there is something to drop.
local function closed()
  cancelReview()
  if next(selections.pending) or next(selections.preview) then selections={pending={},preview={}} end
  if next(snapshots) then snapshots={} end
end
local function result(ok,message)
  actionMessage=not ok and (message or "The reward changed. Select it again.") or nil
end
local function optionById(tab,id)
  for _,option in ipairs(tab and tab.options or {})do
    if not option.common and option.wire_id==id then return option end
  end
end
local function current(lane)
  local selected=selections[laneName]
  if selected.revision~=lane.revision then
    selected.revision,selected.choice=lane.revision,nil;cancelReview();actionMessage=nil
  end
  local tab
  for _,value in ipairs(lane.tabs or {})do if value.token==selected.token then tab=value;break end end
  if not tab then
    tab=(lane.tabs or {})[1];selected.token,selected.choice=tab and tab.token or nil,nil;cancelReview()
  end
  if selected.choice~=nil and not optionById(tab,selected.choice)then selected.choice=nil;cancelReview()end
  return tab,selected
end
local function claimAllowed(lane,tab,choice)
  return laneName=="pending" and tab and not tab.blocked and not lane.claim_pending and not lane.claim_uncertain
    and choice and not choice.common
end
local function matchesReview(lane,tab,selected)
  return review and review.lane==laneName and review.revision==lane.revision and tab
    and review.token==tab.token and review.option==selected.choice
    and claimAllowed(lane,tab,optionById(tab,selected.choice))
end
local function colored(text,color)ui.TextColored(color[1],color[2],color[3],color[4] or 1,text)end
local function actionButton(label,enabled,action,width,primary,height)
  if look.button(label,enabled,width,primary,height) and enabled then action()end
end
local function plain(text)return (text or ""):gsub("[\r\n]"," ")end
local function prefix(text,length)
  -- Ellipses must not split a received UTF-8 name in the middle of a character.
  while length>0 do
    local nextByte=text:byte(length+1)
    if not nextByte or nextByte<128 or nextByte>=192 then break end
    length=length-1
  end
  return text:sub(1,length)
end
local function fitted(text,width,size)
  text=plain(text)
  local _,fontHeight=ui.CalcTextSize("Ag")
  local ratio=size and size/math.max(1,fontHeight) or 1
  local function fits(value)return ui.CalcTextSize(value)*ratio<=width end
  if fits(text)then return text end
  if not fits("...")then return ""end
  local low,high=0,#text
  while low<high do
    local middle=math.floor((low+high+1)/2)
    if fits(prefix(text,middle).."...")then low=middle else high=middle-1 end
  end
  return prefix(text,low).."..."
end
local function itemName(item)
  local name=item.name and item.name~="" and item.name or ("Item "..(item.id or 0))
  return name..((item.quantity or 1)~=1 and (" x"..item.quantity) or "")
end
local function itemIcon(item,scale,designSize)
  local size=(designSize or 30)*scale
  local texture,u0,v0,u1,v1
  -- Reward packets carry item IDs, not icon IDs. Optional icons are shown only
  -- when supplied from a known item definition; rendering never requests one.
  if item and item.icon and item.icon>0 then texture,u0,v0,u1,v1=API.assets.ItemIcon(item.icon)end
  if texture then ui.Image(texture,size,size,u0,v0,u1,v1)
  else
    local x,y=ui.GetCursorScreenPos()
    local c=look.colors.card
    ui.DrawRectFilled(x,y,x+size,y+size,c[1],c[2],c[3],1,4*scale)
    ui.DrawRect(x,y,x+size,y+size,look.colors.muted[1],look.colors.muted[2],look.colors.muted[3],1,3*scale,scale)
    ui.DrawText(x+2*scale,y+size*0.36,look.colors.muted[1],look.colors.muted[2],look.colors.muted[3],1,"ITEM",8*scale)
    ui.Dummy(size,size)
  end
end
local function summary(option)
  local parts,firstItem={},nil
  for _,entry in ipairs(option.entries or {})do
    for _,item in ipairs(entry.items or {})do
      firstItem=firstItem or item;parts[#parts+1]=itemName(item)
    end
    if #(entry.items or {})==0 then
      parts[#parts+1]=(entry.details and entry.details~="" and entry.details)
        or (entry.description and entry.description~="" and entry.description) or entry.kind_label or "Reward"
    end
  end
  -- A one-item choice usually names that item already. Keep the second line
  -- useful instead of repeating the exact title above it.
  if #parts==1 and firstItem and firstItem.name==option.label then
    return "Quantity: "..tostring(firstItem.quantity or 1),firstItem
  end
  return table.concat(parts,"  |  "),firstItem
end
local function entryDetails(lane,tab,option,entry,scale,ordinal)
  local items=entry.items or {}
  if #items==0 then colored(entry.kind_label or "Reward",look.colors.accent)end
  if entry.description and entry.description~="" and entry.description~=option.label then ui.TextWrapped(entry.description)end
  if entry.details and entry.details~="" then ui.TextWrapped(entry.details)end
  for index,item in ipairs(items)do
    if index>1 then ui.Separator()end
    local inspectable=item.id and item.id~=0 and not lane.inspect_pending
    local function inspect()
      result(API.rewards.Inspect(laneName,lane.revision,tab.token,option.wire_id,entry.entry_id,item.id))
    end
    itemIcon(item,scale,look.inspectSize)
    if inspectable and ui.IsItemClicked(0)then inspect()end
    if ui.IsItemHovered()then ui.SetTooltip(itemName(item).."\nInspect item stats")end
    ui.SameLine(0,10*scale)
    ui.BeginGroup()
    ui.PushTextWrapPos(0)
    colored(itemName(item),look.colors.accent)
    ui.PopTextWrapPos()
    if inspectable and ui.IsItemClicked(0)then inspect()end
    if ui.IsItemHovered()then ui.SetTooltip("Inspect item stats")end
    if item.lore==true or item.lore==1 then colored("Lore item",look.colors.muted)end
    local id="###inspect:"..laneName..":"..tab.token..":"..option.wire_id..":"..ordinal..":"..index
    local available=ui.GetContentRegionAvail()
    actionButton("Inspect item"..id,inspectable,inspect,
      math.min(available,ui.CalcTextSize("Inspect item")+look.buttonPadding*scale),false,look.inspectHeight*scale)
    ui.EndGroup()
    ui.Spacing()
  end
  ui.Spacing()
end
local function optionDetails(lane,tab,option,scale)
  for index,entry in ipairs(option.entries or {})do entryDetails(lane,tab,option,entry,scale,index)end
end
local function pageChooser(lane,tab,selected,scale)
  local tabs=lane.tabs or {}
  if #tabs>1 then
    local title=tab and tab.title and tab.title~="" and tab.title or "Select a reward page"
    ui.Text("Reward page")
    ui.SetNextItemWidth(-1)
    if ui.BeginCombo("###rewardpage",title)then
      for index,value in ipairs(tabs)do
        local caption=value.title and value.title~="" and value.title or ("Reward "..index)
        if look.select(caption.."###reward:"..laneName..":"..value.token,selected.token==value.token,nil,30*scale)then
          selected.token,selected.choice=value.token,nil;tab=value;cancelReview();actionMessage=nil
        end
      end
      ui.EndCombo()
    end
  end
  return tab
end
local function choiceCard(tab,option,selected,scale,ordinal)
  local label=option.label and option.label~="" and option.label or ("Choice "..ordinal)
  local chosen=selected.choice==option.wire_id
  local id=laneName..":"..tab.token..":"..option.wire_id
  local c=chosen and look.colors.selected or look.colors.card
  ui.PushStyleColor(ui.Col.ChildBg,c[1],c[2],c[3],1)
  ui.PushStyleVar(ui.StyleVar.WindowPadding,8*scale,6*scale)
  ui.BeginChild("##rewardcard:"..id,0,look.cardHeight*scale,true,ui.WindowFlags.NoScrollbar|ui.WindowFlags.NoScrollWithMouse)
  local x,y=ui.GetCursorScreenPos()
  local width,height=ui.GetContentRegionAvail()
  if look.select("###choice:"..id,chosen,width,height)then
    selected.choice=option.wire_id;cancelReview();actionMessage=nil
  end
  local description,firstItem=summary(option)
  if ui.IsItemHovered()or (ui.IsItemFocused and ui.IsItemFocused())then
    ui.SetTooltip(label..(description~="" and ("\n"..description)or ""))
  end
  ui.SetCursorScreenPos(x,y)
  local ink=chosen and (look.onSelectedInk or look.colors.accent) or look.colors.text
  ui.DrawCircle(x+8*scale,y+12*scale,6*scale,ink[1],ink[2],ink[3],1,scale)
  if chosen then ui.DrawCircleFilled(x+8*scale,y+12*scale,3*scale,ink[1],ink[2],ink[3],1)end
  local title=tostring(ordinal)..". "..label
  ui.PushClipRect(x+23*scale,y,x+width,y+28*scale,true)
  ui.DrawText(x+23*scale,y+2*scale,ink[1],ink[2],ink[3],1,
    fitted(title,math.max(0,width-23*scale),look.cardTitleSize*scale),look.cardTitleSize*scale)
  ui.PopClipRect()
  ui.SetCursorScreenPos(x,y+32*scale)
  if firstItem then itemIcon(firstItem,scale,26);ui.SameLine(0,8*scale)end
  local summaryWidth=ui.GetContentRegionAvail()
  colored(fitted(description~="" and description or "Review the grants in the details pane.",summaryWidth),
    chosen and (look.onSelectedInk or look.colors.muted) or look.colors.muted)
  ui.EndChild();ui.PopStyleVar();ui.PopStyleColor()
  ui.Spacing()
end

local function choiceList(tab,selected,scale,compact)
  local total=0
  for _,option in ipairs(tab and tab.options or {})do if not option.common then total=total+1 end end
  if not compact then
    ui.SeparatorText("Choose one reward")
    colored("Select 1 of "..total.." choices",look.colors.muted)
  end
  local count=0
  for _,option in ipairs(tab and tab.options or {})do
    if not option.common then count=count+1;choiceCard(tab,option,selected,scale,count)end
  end
  if count==0 then ui.TextWrapped("No selectable choices are available.")end
end
local function grantDetails(lane,tab,selected,scale)
  if not tab then
    look.heading(laneName=="pending" and "No pending rewards are listed." or "No reward preview is open.")
    ui.TextWrapped(laneName=="pending" and "Use Restore pending rewards to ask for your outstanding rewards." or "Open a reward preview from the game's normal controls.")
    return
  end
  look.heading(tab.title and tab.title~="" and tab.title or "Reward")
  if tab.blocked then colored("This reward is currently unavailable.",look.colors.warning)end
  local choice=optionById(tab,selected.choice)
  local sole,count=nil,0
  for _,option in ipairs(tab.options or {})do
    if not option.common then sole=option;count=count+1 end
  end
  ui.SeparatorText(choice and "Selected choice" or "Choice details")
  if not choice and count==1 then
    choice=sole
    colored("Review only. Select the card before claiming.",look.colors.muted)
  end
  if choice then
    local _,firstItem=summary(choice)
    if not(firstItem and firstItem.name==choice.label and #(choice.entries or {})==1 and #(choice.entries[1].items or {})==1)then
      look.heading(choice.label and choice.label~="" and choice.label or "Selected reward")
    end
    if firstItem then colored("Inspect items to review their full stats.",look.colors.muted)end
    ui.Spacing()
    optionDetails(lane,tab,choice,scale)
  else ui.TextWrapped("Select a choice to review its full reward details.")end
  local common=false
  for _,option in ipairs(tab.options or {})do
    if option.common then
      if not common then ui.SeparatorText("Included with every choice");common=true end
      if option.label and option.label~="" then ui.TextWrapped(option.label)end
      optionDetails(lane,tab,option,scale)
    end
  end
  if not common then colored("No additional common grants are listed.",look.colors.muted)end
end
local function footer(lane,tab,selected,scale)
  local used=0
  local function footerButton(label,enabled,action,primary)
    local caption=label:match("^(.*)###") or label
    local available=ui.GetContentRegionAvail()
    local width=math.min(available,ui.CalcTextSize(caption)+look.buttonPadding*scale)
    if used>0 and used+8*scale+width<=available then ui.SameLine(0,8*scale)
    else used=0 end
    actionButton(label,enabled,action,width,primary,look.actionHeight*scale);used=used+width+8*scale
  end
  local choice=optionById(tab,selected.choice)
  if review then
    colored("Confirm your reward",look.colors.accent)
    local reviewWidth=ui.GetContentRegionAvail()
    ui.Text(fitted(review.label,reviewWidth))
    if ui.IsItemHovered()then ui.SetTooltip(review.label)end
    colored("The included rewards above will also be granted.",look.colors.muted)
    footerButton("Yes, claim reward###confirmreward",matchesReview(lane,tab,selected),function()
      local confirmed=review;cancelReview()
      result(API.rewards.Claim(confirmed.revision,confirmed.token,confirmed.option))
    end,true)
    footerButton("Cancel###cancelreward",true,cancelReview)
  else
    colored(choice and ("Selected: "..fitted(choice.label~="" and choice.label or "Reward",ui.GetContentRegionAvail()-65*scale))
      or "Choose a reward card before claiming.",look.colors.accent)
    footerButton("Claim selected reward###claimreward",claimAllowed(lane,tab,choice),function()
      review={lane=laneName,revision=lane.revision,token=tab.token,option=selected.choice,
        label=choice.label and choice.label~="" and choice.label or "the selected reward"}
    end,true)
  end
  -- Keep maintenance actions on their own row so they never crowd the claim.
  ui.Spacing();used=0
  footerButton("Restore pending rewards###restorerewards",true,function()
    cancelReview();changeLane("pending");result(API.rewards.RestorePending())
  end)
  footerButton("Close page###closerewardpage",true,function()
    cancelReview();result(API.rewards.Close(laneName))
  end)
  if actionMessage then colored(actionMessage,look.colors.warning)end
end

local function draw(state,scale)
  local width=ui.GetContentRegionAvail()
  local tabWidth=math.min(210*scale,(width-8*scale)/2)
  if look.select("Pending rewards###pendingpage",laneName=="pending",tabWidth,32*scale)then changeLane("pending")end
  ui.SameLine(0,8*scale)
  if look.select("Preview###previewpage",laneName=="preview",tabWidth,32*scale)then changeLane("preview")end
  local lane=state[laneName] or {}
  local tab,selected=current(lane)
  tab=pageChooser(lane,tab,selected,scale)
  if review and not matchesReview(lane,tab,selected)then cancelReview()end
  if laneName=="preview"then colored("Preview only. These rewards cannot be claimed here.",look.colors.warning)end
  if lane.claim_uncertain then
    colored("The claim result is uncertain. Wait for the server's result; restoring the list does not retry or unlock this claim.",look.colors.warning)
  elseif lane.claim_pending then colored("Waiting for the server to confirm the claim.",look.colors.warning)end
  if lane.inspect_pending then colored("Retrieving item details...",look.colors.muted)end
  if lane.message and lane.message~=""then ui.TextWrapped(lane.message)end
  local footerHeight=(review and look.reviewHeight or look.footerHeight)*scale
  local _,height=ui.GetContentRegionAvail()
  local noScroll=ui.WindowFlags.NoScrollbar|ui.WindowFlags.NoScrollWithMouse
  ui.BeginChild("##rewardcontent",0,math.max(100*scale,height-footerHeight-18*scale),false,noScroll)
  local contentWidth,contentHeight=ui.GetContentRegionAvail()
  if contentWidth>=640*scale then
    ui.BeginChild("##rewardchoices",math.floor(contentWidth*0.4),0,true)
    choiceList(tab,selected,scale,false)
    ui.EndChild();ui.SameLine(0,10*scale)
  else
    ui.BeginChild("##rewardchoices",0,math.max(70*scale,math.floor(contentHeight*0.28)),true)
    choiceList(tab,selected,scale,true)
    ui.EndChild();ui.Spacing()
  end
  ui.BeginChild("##rewarddetails",0,0,true)
  grantDetails(lane,tab,selected,scale)
  ui.EndChild();ui.EndChild()
  -- The gamepad window normally gives every child 24px padding. Action rows
  -- use a tighter inset so both 42px controls remain visible without scrolling.
  ui.PushStyleVar(ui.StyleVar.WindowPadding,8*scale,8*scale)
  ui.BeginChild("##rewardfooter",0,footerHeight,true)
  footer(lane,tab,selected,scale)
  ui.EndChild();ui.PopStyleVar()
end

ui.window("Select Reward",function()
  if not API.session.InGame()then closed();return end
  API.windows.Replace(WINDOW) -- every frame, hidden too: the reward key toggles the window only while a pack claims it
  local state=readState()
  if not state or not state.shown then closed();return end
  if laneName=="pending"and not(state.pending or {}).shown and(state.preview or {}).shown then changeLane("preview")end
  if laneName=="preview"and not(state.preview or {}).shown and(state.pending or {}).shown then changeLane("pending")end
  hud.Dialog(WINDOW,"Select Reward",{shown=true,size=function(scale)
      local io=ui.GetIO()
      local lane=state[laneName] or {};local tabs=lane.tabs or {};local count=0
      for _,option in ipairs(tabs[1] and tabs[1].options or {})do if not option.common then count=count+1 end end
      local compact=#tabs<=1 and count<=1
      return math.min(compact and 860 or 920,(io.width-60)/scale),math.min(compact and 540 or 760,(io.height-70)/scale)
    end,resizable={620,500},place=function(sw,sh,w,h)return (sw-w)/2,(sh-h)/2 end,
    onClose=function()closed();API.windows.Show(WINDOW,false)end},function(_,_,_,_,scale)draw(state,scale)end)
end)
