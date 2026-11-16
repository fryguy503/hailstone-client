CLIENT_ROOT = CLIENT_ROOT or (arg and arg[1]) or "."
local attachUI = assert(loadfile(CLIENT_ROOT .. "/tests/lua_ui_api_mock.lua"))()
local checks=0
local function check(value,why)checks=checks+1;assert(value,why)end
local function achievement(name,extra)
  return "\18".."3Peer^900^1^0^0^"..(extra or "4^4^").."'"..name.."\18"
end
local item="\18".."0"..string.rep("a",55).."Old Ring\18"
local tag=achievement("Hero's <reward> & title")
local helpers={}
for _,pack in ipairs({"modern","gamepad"})do
  local helper=assert(loadfile(CLIENT_ROOT.."/ui/"..pack.."/lib/chatlinks.lua"))()
  helpers[pack]=helper
  local links={}
  local raw="Hero's <reward> & title before "..tag.." and "..tag.." "..item
  local shown=helper.Collapse(links,raw)
  check(not shown:find("Peer^",1,true) and not shown:find("\18",1,true),"Metadata reached the visible editor")
  check(#links==3 and links[1].tag==tag and links[2].tag==tag and links[3].tag==item,"Exact mixed tags or apostrophe label were lost")
  check(helper.Expand(links,shown)==raw,"Duplicate label placement changed ordinary earlier text or nested tags")
  check(helper.ForSubmit(links,shown)==raw,"Valid mixed line was not admitted intact")
  check(helper.ForSubmit({},string.rep("p",1000))==string.rep("p",1000),"Link guard reduced the ordinary plain-text limit")
  links={};shown=helper.Collapse(links,achievement("Tiny",string.rep("1^",270)))
  local submitted,reason=helper.ForSubmit(links,shown)
  check(submitted==nil and reason and shown=="Tiny" and #links==1,"Overlong wire metadata was truncated or displayed")
  links={};local ten=string.rep(achievement("X",""),10);shown=helper.Collapse(links,ten)
  check(helper.ForSubmit(links,shown)==ten,"Ten short achievement tags were rejected")
  links={};shown=helper.Collapse(links,ten..achievement("X",""))
  check(helper.ForSubmit(links,shown)==nil,"The ten-tag limit counted only item tags")
  for _,bad in ipairs({"\18".."3Peer^bad^1^'Oops\18","\18".."3Peer^900^1^0^'Bad\nlabel\18","\18".."3truncated^metadata"})do
    links={};shown=helper.Collapse(links,"prefix "..bad)
    check(shown=="prefix Achievement" and #links==0,"Malformed code3 metadata was exposed or retained as actionable")
  end
  links={};shown=helper.Collapse(links,tag)
  local exactly=string.rep("p",512-#tag)..shown
  check(#helper.ForSubmit(links,exactly)==512,"Exact 512-byte tagged boundary was rejected")
  check(helper.ForSubmit(links,"p"..exactly)==nil,"Expanded 513-byte tagged line was accepted")
end

-- Run each actual editor, including its real Collapse/Expand helper. Only the
-- drawing dependencies are mocked; a pending host insertion sees raw typed text.
for _,pack in ipairs({"modern","gamepad"})do
  local windows,sent={},{}
  local pending,edit,enter,lastShown,lastCapacity,errorText,seenTyped
  local frameNumber=0
  local gameState,charName=5,"Hero"
  GPSTATE={chatOpen=true,chatByKeyboard=true}
  local no=function()end
  local zero=function()return 0 end
  local color=setmetatable({chatBg={0,0,0,1}},{__index=function()return {1,1,1,1}end})
  local T=setmetatable({C=color,color=color,font=setmetatable({small=1},{__index=function()return no end}),
    px=function(n)return n end,TextSize=function(text)return #text*8,16 end,
    Hit=function()return false end,button=function()return false end},
    {__index=function()return no end})
  -- Gamepad uses T.font as a callable; Modern uses its font.small field.
  setmetatable(T.font,{__call=no,__index=function()return 1 end})
  local pad=setmetatable({down=function()return false end,pressed=function()return false end,
    menuPressed=function()return false end,focused=function()return false end},{__index=function()return no end})
  local S=setmetatable({DrawLines=no,UpdateLines=no,AppendLines=no,fontKey=function()return "font"end},{__index=function()return no end})
  local W={T=T,pad=pad,NoNav=0,shown=function()return true end,begin=function()return true end,
    area=function()return 0,0,1500,900,1500,900 end,finish=function()return false end,pressed=function()return false end}
  local enum=setmetatable({},{__index=function()return 0 end})
  ui=setmetatable({window=function(name,fn)windows[name]=fn end,
    GetTime=function()return frameNumber end,GetFrameCount=function()return frameNumber end,
    GetSetting=function()return nil end,GetWindowPos=function()return 0,0 end,
    GetCursorScreenPos=function()return 0,0 end,GetContentRegionAvail=function()return 800,600 end,
    GetIO=function()return {alt=false,wheel=0}end,CalcTextSize=function(text)return #text*8,16 end,
    GetScrollY=zero,GetScrollMaxY=zero,BeginChild=function()return true end,
    IsItemHovered=function()return false end,IsItemActive=function()return false end,
    IsWindowHovered=function()return false end,IsMouseHoveringRect=function()return false end,
    IsMouseDown=function()return false end,IsKeyPressed=function()return false end,
    BeginPopup=function()return false end,InvisibleButton=function()return false end,
    InputText=function(_,text,capacity)
      if edit then text=edit;edit=nil end
      lastShown,lastCapacity=text,capacity
      local accepted=enter;enter=false;return text,accepted==true
    end,
    SetTooltip=function(text)errorText=text end,TextWrapped=function(text)errorText=text end,
    Col=enum,StyleVar=enum,Mouse=enum,Key=enum,InputTextFlags=enum,HoveredFlags=enum,ButtonFlags=enum,
  },{__index=function()return no end})
  local chatWindow={slot=1,title="Main",main=true,tab=1,tabs={{title="Main"}}}
  hs=setmetatable({InGame=function()return true end,UiReady=function()return true end,
    GameState=function()return gameState end,Name=function()return charName end,
    ChatWindows=function()return {chatWindow}end,ChatText=function()return 0,{},true,0 end,
    ChatInfo=function()return 3,true,"Say"end,ChatPage=function()return nil end,
    ChatTakeFocus=function(_,typed)
      seenTyped=typed
      if not pending then return nil end
      local value=pending;pending=nil
      if value.append then return typed..value.text,true end
      return value.text,value.replace
    end,
    ChatSubmit=function(slot,text)sent[#sent+1]={slot,text}end,
  },{__index=function(_,name)error("Unexpected game API: "..name)end})
  attachUI(hs)
  local original=dofile
  dofile=function(path)
    if path=="lib/chatlinks.lua"then return helpers[pack]end
    if path=="lib/stml.lua"then return S end
    if path=="lib/theme.lua"then return T end
    if path=="lib/pad.lua"then return pad end
    if path=="lib/window.lua"then return W end
    if path=="lib/keyboard.lua"then return {KEYS={{"a"}},label=function(key)return key end}end
    if path=="lib/hud.lua"then return {T=T,Window=function(_,_,_,body)body(0,0,1000,700,1)end}end
    error("Unexpected helper: "..path)
  end
  assert(loadfile(CLIENT_ROOT.."/ui/"..pack.."/chat.lua"))();dofile=original
  local function frame()
    frameNumber=frameNumber+1
    if pack=="gamepad" and edit then GPSTATE.chatOpen=true end
    if pack=="modern"then windows["Chat 1"]() else windows["Chat Ticker"]();windows["Chat Log"]()end
  end
  pending={text="/gsay Prefix ",replace=true};frame()
  pending={text=tag,append=true};frame()
  check(lastShown=="/gsay Prefix Hero's <reward> & title" and #sent==0,"Editor exposed metadata or submitted while inserting a link")
  pending={text=" "..tag.." "..item,append=true};frame()
  local expected="/gsay Prefix "..tag.." "..tag.." "..item
  check(seenTyped=="/gsay Prefix "..tag,"Host insertion did not receive expanded typed prefix")
  enter=true;frame();check(#sent==1 and sent[1][1]==1 and sent[1][2]==expected,"Explicit Send did not preserve exact mixed tags and typed prefix")
  edit="Hero's <reward> & title";enter=true;frame()
  local ordinary=pack=="gamepad" and "/say Hero's <reward> & title" or "Hero's <reward> & title"
  check(sent[2][2]==ordinary,"Sent draft retained an old hidden link")
  local long=achievement("Compact label",string.rep("4294967295^",27))
  pending={text="/gsay "..long,replace=true};frame()
  check(lastShown=="/gsay Compact label" and lastCapacity>=512,"Editor truncated a tag to its former small text field")
  enter=true;frame();check(sent[3][2]=="/gsay "..long,"A valid >256-byte tag did not survive Send")
  local tooLong=achievement("Do not truncate",string.rep("1^",270))
  pending={text="/gsay "..tooLong,replace=true};frame();enter=true;frame()
  check(#sent==3 and errorText and not lastShown:find("Peer^",1,true),"Oversized tag was partly sent or its metadata was shown")
  frame();check(lastShown=="/gsay Do not truncate","Rejected tagged draft was discarded")
  edit=string.rep("p",1000);enter=true;frame()
  local plain=pack=="gamepad" and "/say "..string.rep("p",1000) or string.rep("p",1000)
  check(#sent==4 and sent[4][2]==plain,"Expanded-link protection changed ordinary long chat behavior")
  pending={text=tag,replace=true};frame()
  edit="";frame();edit="Hero's <reward> & title";enter=true;frame()
  check(sent[5] and sent[5][2]==ordinary,"Deleting the whole draft retained a hidden link for retyped plain text")
  if pack=="modern"then
    -- a half-typed line belongs to the character: zoning (no character for a while) keeps it, character select or
    -- another character drops it
    edit="half typed";frame();check(lastShown=="half typed","Typed draft was not kept")
    charName=nil;frame();charName="Hero";frame();check(lastShown=="half typed","Zoning dropped the draft")
    gameState=1;charName=nil;frame();gameState=5;charName="Hero";frame()
    check(lastShown=="","Character select kept the draft")
    edit="half typed";frame();charName="Other";frame();check(lastShown=="","Another character inherited the draft")
  end
end
print("Achievement chat composition: "..checks.." checks passed")
