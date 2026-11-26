R"hooks(
local native = ...
local pack, unpack, pcall, type, pairs, getmetatable = table.pack, table.unpack, pcall, type, pairs, getmetatable
local sort, floor, error, select = table.sort, math.floor, error, select
local registrations, methods, sequence, depth = {}, {}, 0, 0
local wrapped=setmetatable({}, {__mode='k'})
local depthGuard={__close=function(g)depth=g.previous end}
local nextGuard={__close=function(g)g.active=false;g.run=nil;g.owner=nil end}
local limits = {total=128, owner=16, method=16, depth=8, values=64, nodes=4096, bytes=65536}

-- Observers never receive the caller's tables or mutable result records. Unknown values and excess data
-- bypass hooks; they are never truncated, substituted or passed across another owner's boundary.
local function copy(values,track)
  local nodes, bytes, active = 0, 0, {}
  local links={}
  local function clone(v, level)
    nodes=nodes+1
    if nodes>limits.nodes or level>16 then error('hook data limit',0) end
    local kind=type(v)
    if kind=='nil' or kind=='boolean' or kind=='number' then return v,v end
    if kind=='string' then
      bytes=bytes+#v
      if bytes>limits.bytes then error('hook data limit',0) end
      return v,v
    end
    if kind~='table' or getmetatable(v)~=nil or active[v] then error('hook data is not plain',0) end
    active[v]=true
    local out,baseline={},{}
    for k,item in pairs(v) do
      if type(k)~='string' and type(k)~='number' then error('hook data key is not plain',0) end
      local key=clone(k,level+1)
      local value,snapshot=clone(item,level+1)
      out[key]=value
      if track then baseline[key]=snapshot end
    end
    active[v]=nil
    if track then links[out]={original=v,baseline=baseline} end
    return out,baseline
  end
  if values.n>limits.values then return nil end
  local ok,result=pcall(clone,values,0)
  if ok then return result,links end
end
local function recover(values,links)
  local function equal(a,b)
    if type(a)~=type(b) then return false end
    if type(a)~='table' then return a==b or (type(a)=='number' and a~=a and b~=b) end
    for k,v in pairs(a) do if not equal(v,b[k]) then return false end end
    for k in pairs(b) do if a[k]==nil then return false end end
    return true
  end
  local function restore(v)
    if type(v)~='table' then return v end
    local known=links[v]
    if known and equal(v,known.baseline) then return known.original end
    local out={}
    for k,item in pairs(v) do out[k]=restore(item) end
    return out
  end
  return restore(values)
end
local function ready(r,execution)
  if not r.active then return false end
  if not native.ready(r.owner,r.need,true) then r.active=false;r.fn=nil;return false end
  return not execution or native.callready(r.owner,r.need)
end
local function prune()
  local kept,activeMethods={},{}
  for _,r in ipairs(registrations) do
    if ready(r) then kept[#kept+1]=r;activeMethods[r.method]=true else r.active=false; r.fn=nil end
  end
  registrations=kept;methods=activeMethods
end
-- Each hooked method's registrations, in call order: what a call of the method walks, so it neither walks every
-- registration nor sorts. Built when the method's registrations changed: `generation` moves with each new
-- registration, and one that stopped (removed, failed, retired, out of authority) is found by the per-call ready
-- check below, which filters it out of a new list. A list is never changed once built: a dispatch keeps the one it
-- started with (a registration made meanwhile waits for the next call, as it always has).
local generation, selections = 0, {}
local function byOrder(a,b) return a.priority==b.priority and a.order<b.order or a.priority<b.priority end
local function selection(method)
  local cached=selections[method]
  if cached and cached.generation==generation then
    -- What a full prune would decide for this method's registrations: no other method's are asked.
    local stale=false
    for i=1,#cached do if not ready(cached[i]) then stale=true end end
    if not stale then return cached end
    local kept={generation=generation}
    for i=1,#cached do if cached[i].active then kept[#kept+1]=cached[i] end end
    cached=kept
  else
    cached={generation=generation}
    for _,r in ipairs(registrations) do if r.method==method and ready(r) then cached[#cached+1]=r end end
    sort(cached,byOrder)
  end
  selections[method]=cached
  -- Every registration of the method stopped: its calls go back to the one-lookup path.
  if #cached==0 then methods[method]=nil end
  return cached
end
local function maintain()
  local kept,activeMethods={},{}
  for _,r in ipairs(registrations) do
    if r.active and native.alive(r.owner) then kept[#kept+1]=r;activeMethods[r.method]=true
    else r.active=false;r.fn=nil end
  end
  registrations=kept;methods=activeMethods
end
local function invoke(r,...)
  if not ready(r,true) then return nil end
  -- Catch ordinary callback errors inside its source runner. A data-budget escape must not retire a
  -- still-valid source; the dispatcher decides whether a failure is an actual hook error or a skipped result.
  local result=pack(native.invoke(r.owner,r.need,true,r.handle,pcall,r.fn,...))
  ready(r)
  if result[1]==nil then return nil end
  if not result[1] then r.active=false; r.fn=nil; return false end
  return unpack(result,2,result.n)
end
local function failed(r,why)
  native.failed(r.method,why)
  r.active=false;r.fn=nil
end
local function describe(method)
  if type(method)~='string' then error('hook method must be a canonical module.Member string',2) end
  return native.describe(method)
end
local function register(method,phase,fn,options)
  local policy=describe(method)
  if not policy.hookable then return nil,policy.reason end
  if phase~='before' and phase~='after' and phase~='around' then error('hook phase must be before, after or around',2) end
  if type(fn)~='function' then error('hook callback must be a function',2) end
  options=options or {}
  if type(options)~='table' or getmetatable(options) then error('hook options must be a plain table',2) end
  local priority=options.priority or 0
  if type(priority)~='number' or priority~=floor(priority) or priority < -1000 or priority>1000 then error('hook priority must be an integer from -1000 to 1000',2) end
  local id=options.id
  if id~=nil and (type(id)~='string' or #id<1 or #id>64 or id:find('[%c]')) then error('hook id must be 1..64 printable bytes',2) end
  local owner,key,tag=native.capture()
  if not owner or not native.ready(owner,policy.requiredCaps,true) then return nil,'Hooks require live UI, Read, Extend and method permissions' end
  prune()
  local owned,methodCount=0,0
  for _,r in ipairs(registrations) do
    if r.key==key then
      if id and r.id==id and r.method==method and r.phase==phase then r.active=false; r.fn=nil
      else owned=owned+1 end
    end
    if r.active and r.method==method then methodCount=methodCount+1 end
  end
  prune()
  if #registrations>=limits.total or owned>=limits.owner or methodCount>=limits.method then return nil,'UI hook registration limit reached' end
  sequence=sequence+1
  local r={handle='hook:'..sequence,owner=owner,key=key,tag=tag,id=id or '',method=method,phase=phase,
    priority=priority,order=sequence,fn=fn,need=policy.requiredCaps,active=true}
  registrations[#registrations+1]=r
  methods[method]=true
  generation=generation+1
  return r.handle
end
local function remove(handle)
  local owner,key=native.capture()
  if not owner then return false end
  prune()
  for _,r in ipairs(registrations) do
    if r.handle==handle and r.key==key then r.active=false; r.fn=nil; return true end
  end
  return false
end
local function list(method)
  local policy=method~=nil and describe(method) or nil
  local owner,key=native.capture()
  prune()
  local out={}
  if policy then out.method=method; out.hookable=policy.hookable; out.reason=policy.reason; out.policy=policy.policy end
  for _,r in ipairs(registrations) do
    if owner and r.key==key and (not method or r.method==method) then
      local enabled=ready(r,true)
      out[#out+1]={handle=r.handle,id=r.id,method=r.method,phase=r.phase,priority=r.priority,order=r.order,owner=r.tag,
        enabled=enabled,unavailableReason=enabled and '' or 'Callback role or live native authority exceeds this call ceiling'}
    end
  end
  return out
end
local function wrap(original,method,context)
  if wrapped[original]==method then return original end
  local policy=describe(method)
  local function dispatcher(...)
    -- No hook on this method (always against a stock server): the original at once, one table lookup and no call
    -- into the client. The HUD reads Gauge and Label dozens of times a frame.
    if not methods[method] then return original(...) end
    if not native.live(context) then return original(...) end
    if depth>=limits.depth then return original(...) end
    local selected=selection(method)
    if #selected==0 then return original(...) end
    if select('#',...)>limits.values then return original(...) end
    local caller=native.capture(context)
    if not caller or not native.ready(caller,policy.requiredCaps,false) then return original(...) end
    local arguments=pack(...)
    if not copy(arguments) then return original(...) end
    local frame={originalCalled=false,unsafe=false}
    local function originalCall(args)
      if frame.originalCalled then error('original UI action already called',0) end
      frame.originalCalled=true
      -- The original runs through its captured caller scope; active hook scopes can only reduce its authority.
      local returned=pack(native.invoke(caller,policy.requiredCaps,false,nil,pcall,original,unpack(args,1,args.n)))
      if not returned[1] then error('original UI caller is no longer available',0) end
      if not returned[2] then frame.hasOriginalError=true; frame.originalError=returned[3]; error(returned[3],0) end
      local result={n=returned.n-2}
      for i=3,returned.n do result[i-2]=returned[i] end
      frame.originalResult=result
      if not copy(result) then frame.unsafe=true end
      return result
    end
    local function around(index,args)
      local r=selected[index]
      if not r then return originalCall(args) end
      if r.phase~='around' or not ready(r,true) then return around(index+1,args) end
      local copied,argumentLinks=copy(args,true)
      if not copied then return around(index+1,args) end
      local continuation <close> = setmetatable({active=true,used=false,owner=r.owner},nextGuard)
      local called=false
      local downstream,downstreamError,hasDownstreamError,resultLinks
      continuation.run=function(...)
        if not ready(r,true) or not native.ready(caller,policy.requiredCaps,false) then error('UI hook continuation is no longer authorized',0) end
        local changed=pack(...)
        if not copy(changed) then error('UI hook arguments exceed the plain-data contract',0) end
        called=true
        changed=recover(changed,argumentLinks)
        local returned=pack(pcall(around,index+1,changed))
        if not returned[1] then
          hasDownstreamError=true;downstreamError=returned[2]
          -- Error payloads are observations too. Keep the exact host error privately for its caller,
          -- and never expose a raw table/userdata or an oversized error through pcall(next).
          local visible=copy(pack(downstreamError))
          if visible then error(visible[1],0) end
          error('UI error exceeds the hook plain-data contract',0)
        end
        downstream=returned[2]
        local visible
        visible,resultLinks=copy(downstream,true)
        if not visible then frame.unsafe=true; error('UI results exceed the hook plain-data contract',0) end
        return unpack(visible,1,visible.n)
      end
      -- A retained next closure holds only the small guard after this scope closes, never the caller's
      -- argument/result graph, original host closure or captured owner records.
      local function next(...)
        if not continuation.active or continuation.used or not native.same(continuation.owner) then error('UI hook continuation is expired, foreign or already used',0) end
        continuation.used=true
        return continuation.run(...)
      end
      local returned=pack(invoke(r,next,unpack(copied,1,copied.n)))
      continuation.active=false
      if hasDownstreamError then error(downstreamError,0) end
      if frame.hasOriginalError then error(frame.originalError,0) end
      if frame.unsafe and frame.originalResult then return frame.originalResult end
      if returned[1]==false then failed(r,returned[2]) end
      if returned[1] and ready(r,true) then
        local result={n=returned.n-1}
        for i=2,returned.n do result[i-1]=returned[i] end
        local checked=copy(result)
        if checked then return recover(result,resultLinks or {}) end
      end
      -- A failing around callback after next must reuse that result, never repeat the underlying action.
      if called then return downstream end
      return around(index+1,args)
    end
    local function dispatch()
      for _,r in ipairs(selected) do
        if r.phase=='before' and ready(r,true) then
          local ok,why=invoke(r,copy(arguments));if ok==false then failed(r,why) end
        end
      end
      local result=around(1,arguments)
      if frame.unsafe and frame.originalResult then return frame.originalResult end
      for _,r in ipairs(selected) do
        if r.phase=='after' and ready(r,true) then
          local visible=copy(result)
          if visible then local ok,why=invoke(r,visible,copy(arguments));if ok==false then failed(r,why) end end
        end
      end
      return result
    end
    local nesting <close> = setmetatable({previous=depth},depthGuard)
    depth=depth+1
    local returned=pack(pcall(dispatch))
    if not returned[1] then error(returned[2],0) end
    return unpack(returned[2],1,returned[2].n)
  end
  wrapped[dispatcher]=method
  return dispatcher
end
return {Register=register,Remove=remove,List=list,Wrap=wrap,Maintain=maintain}
)hooks"
