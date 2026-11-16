CLIENT_ROOT = CLIENT_ROOT or (arg and arg[1]) or "."
local attachUI = assert(loadfile(CLIENT_ROOT .. "/tests/lua_ui_api_mock.lua"))()
for _, pack in ipairs({ "modern", "gamepad" }) do
  local function nop() end
  local clicks, lines, offered, selected, confirmed = {}, {}, {}, {}, {}
  local choice
  ui = {
    PushTextWrapPos=nop,PopTextWrapPos=nop,SetNextItemWidth=nop,BeginDisabled=nop,EndDisabled=nop,
    EndCombo=nop,Dummy=nop,Separator=nop,
    Text=function(text) lines[#lines+1]=text end,
    BeginCombo=function() return true end,
    Selectable=function(label)
      offered[#offered+1]=label
      if label==choice then choice=nil;return true end
      return false
    end,
  }
  hs = {
    ItemExpTransferSelect=function(role,id) selected[#selected+1]={role,id};return true end,
    ItemExpTransferConfirm=function(...) confirmed[#confirmed+1]={...};return true end,
  }
  attachUI(hs)
  local transfer = dofile(CLIENT_ROOT .. "/ui/" .. pack .. "/lib/item_exp_transfer.lua")
  local src, dst, third = "18446744073709551615", "9007199254740993", "9007199254740994"
  local revision = "18446744073709551614"
  local items = {
    {id=src,name="Source <literal>",level=3,maxLevel=5,progress=44},
    {id=dst,name="Destination",level=1,maxLevel=5,progress=12},
    {id=third,name="Other",level=2,maxLevel=5,progress=1},
  }
  local state = {shown=true,items=items,source=src,destination=dst,revision=revision,
    pending=false,compatible=true,hasDetails=true,compatibility=100,canTransfer=true,maxTransferLevel=4,status="Server approved",
    previewSource={name="Source after",level=1,maxLevel=5,progress=0},
    previewDestination={name="Destination after",level=4,maxLevel=5,progress=42},
  }
  local look = {width=400,gap=10,button=function(id,caption,enabled)
    local clicked=clicks[id];clicks[id]=nil
    return clicked and enabled
  end}
  local function draw(click)
    clicks=click and {[click]=true} or {}
    lines,offered={},{}
    transfer.draw(state,look)
  end
  local function saw(text)
    for _,line in ipairs(lines) do if line:find(text,1,true) then return true end end
    return false
  end
  state.compatible=false;state.compatibility=0;state.maxTransferLevel=0;state.canTransfer=false
  draw("##evolve-transfer")
  assert(saw("Compatibility: 0%") and saw("Max transfer level: 0"),"incompatible replies still show server details")
  assert(#confirmed==0,"an incompatible reply cannot transfer")
  state.hasDetails=false
  draw()
  assert(not saw("Compatibility:") and not saw("Max transfer level:"),"unanswered requests show no stale details")
  state.compatible=true;state.compatibility=100;state.maxTransferLevel=4;state.canTransfer=true;state.hasDetails=true
  draw("##evolve-transfer")
  assert(#confirmed==0,"Transfer opens a confirmation without sending")
  draw()
  assert(saw("Confirm experience transfer") and saw("Source <literal>") and saw("Destination"))
  assert(saw("Compatibility: 100%") and saw("Max transfer level: 4") and saw("Destination after"),
    "the server preview is shown for review")
  draw("##evolve-cancel")
  draw("##evolve-confirm")
  assert(#confirmed==0,"Cancel sends nothing")
  draw("##evolve-transfer")
  assert(transfer.back() and not transfer.back(),"Back dismisses the question before closing the window")
  draw("##evolve-confirm")
  assert(#confirmed==0,"Back sends nothing")

  draw("##evolve-transfer")
  draw("##evolve-confirm")
  assert(#confirmed==1 and confirmed[1][1]==src and confirmed[1][2]==dst and confirmed[1][3]==revision,
    "confirmation preserves exact uint64 strings")
  draw("##evolve-confirm")
  assert(#confirmed==1,"confirm cannot be repeated without reviewing again")

  draw("##evolve-transfer")
  state.revision="next"
  draw("##evolve-confirm")
  assert(#confirmed==1 and not saw("Confirm experience transfer"),"a new approval invalidates the question")
  draw("##evolve-transfer")
  state.source=third
  draw("##evolve-confirm")
  assert(#confirmed==1,"changing the source invalidates the question")
  state.source=src
  draw("##evolve-transfer")
  state.destination=third
  draw("##evolve-confirm")
  assert(#confirmed==1,"changing the destination invalidates the question")
  state.destination=dst

  draw("##evolve-transfer")
  state.items={items[2],items[3]}
  draw("##evolve-confirm")
  assert(#confirmed==1,"an item disappearing invalidates the question")
  state.items=items
  draw("##evolve-transfer")
  state.pending=true
  draw("##evolve-confirm")
  assert(#confirmed==1,"a pending request invalidates the question")
  state.pending=false
  state.canTransfer=false
  draw("##evolve-transfer")
  state.canTransfer=true
  draw("##evolve-confirm")
  assert(#confirmed==1,"Transfer requires the server's permission")

  draw("##evolve-transfer")
  state.shown=false
  transfer.sync(state)
  state.shown=true
  draw("##evolve-confirm")
  assert(#confirmed==1,"closing forgets the question")
  draw("##evolve-transfer")
  transfer.reset()
  draw("##evolve-confirm")
  assert(#confirmed==1,"leaving the game forgets the question")

  choice="Other###evolve-source-"..third
  draw("##evolve-transfer")
  assert(#selected==1 and selected[1][1]=="source" and selected[1][2]==third)
  draw("##evolve-confirm")
  assert(#confirmed==1,"selection changes cannot reuse this frame's old approval")
  for _,label in ipairs(offered) do
    assert(label~="Destination###evolve-source-"..dst,"the destination cannot be selected as the source")
    assert(label~="Source <literal>###evolve-destination-"..src,"the source cannot be selected as the destination")
  end
  state.pending=true
  choice="Other###evolve-source-"..third
  draw()
  assert(#selected==1,"a pending request prevents selection changes")
end
