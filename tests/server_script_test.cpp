// server/quests/global/encounters/hailstone.lua, the server side's drop-in quest script, run in the client's Lua
// with a stand-in for the parts of EQEmu's Lua API it uses (eq.*, Packet, the player events), os.time, io.open and
// io.popen. What it checks is the script's own bookkeeping: its limits and the files it keeps open, not EQEmu.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <lua.hpp>

#include "scratch_dir.h"

namespace {

std::string script_path() {
    std::string dir = std::string(__FILE__);
    dir = dir.substr(0, dir.rfind('/') == std::string::npos ? 0 : dir.rfind('/'));
    return dir + "/../server/quests/global/encounters/hailstone.lua";
}

constexpr const char* kHarness = R"lua(
package.config = "/\n;\n?\n!\n-\n" -- the script's Linux commands, whatever runs the test
T = { now = 1000, finds = 0, stats = 0, gets = 0, opens = {}, open_now = 0, open_max = 0, messages = {}, data = {}, files = {},
      mtimes = {}, handlers = {}, powershell = {}, timers = {}, clients = {}, logs = {}, dirs = {}, hashes = 0,
      sleeps = 0, mkdirs = 0 }
os.time = function() return T.now end

-- A packet's bytes: what the Write* calls put there, 0 elsewhere. A fixed-length string (a file piece) is kept whole
-- and read a byte at a time only when asked, and the methods are shared: building every piece byte by byte, and 15
-- closures per packet, made the rate-limit cases take 5 to 18 s.
local Bytes = {}
Bytes.__index = function(b, i)
  local segs = rawget(b, "_segs")
  for k = #segs, 1, -1 do
    local seg = segs[k]
    if i >= seg.o and i < seg.o + seg.n then return seg.s:byte(i - seg.o + 1) or 0 end
  end
  return 0
end
local P = {}
P.__index = P
local function put(p, off, v, n) v = math.floor(v) % (256 ^ n); for i = 0, n - 1 do p.b[off + i] = math.floor(v / 256 ^ i) % 256 end end
local function get(p, off, n) local v = 0; for i = n - 1, 0, -1 do v = v * 256 + p.b[off + i] end; return v end
function P:WriteInt32(o, v) put(self, o, v, 4) end
function P:WriteInt16(o, v) put(self, o, v, 2) end
function P:WriteInt8(o, v) put(self, o, v, 1) end
function P:WriteString(o, s) for i = 1, #s do self.b[o + i - 1] = s:byte(i) end; self.b[o + #s] = 0 end
function P:WriteFixedLengthString(o, s, n)
  -- Bytes written before, inside the string's span, give way to it.
  for k in pairs(self.b) do if type(k) == "number" and k >= o and k < o + n then rawset(self.b, k, nil) end end
  local segs = rawget(self.b, "_segs")
  segs[#segs + 1] = { o = o, n = n, s = s }
end
function P:ReadInt32(o) return get(self, o, 4) end
function P:ReadInt16(o) return get(self, o, 2) end
function P:ReadInt8(o) return get(self, o, 1) end
function P:ReadString(o) local t = {}; while self.b[o] ~= 0 do t[#t + 1] = string.char(self.b[o]); o = o + 1 end; return table.concat(t) end
function P:GetSize() return self.size end
function P:GetProtocolOpcode() return self.op end

local function bytes_packet(op, size)
  return setmetatable({ op = op, size = size, b = setmetatable({ _segs = {} }, Bytes) }, P)
end
Packet = function(op, size) return bytes_packet(op, size) end

Event = { unhandled_opcode = 1, signal = 2 }
eq = {
  debug = function(s) T.logs[#T.logs + 1] = s end,
  get_data = function(k) T.gets = T.gets + 1; return T.data[k] end,
  set_data = function(k, v) T.data[k] = v end,
  delete_data = function(k) T.data[k] = nil end,
  cross_zone_message_player_by_name = function(_, name, text) T.messages[#T.messages + 1] = name .. ": " .. text end,
  get_entity_list = function()
    return {
      GetClientByName = function() return { valid = false } end,
      GetClientByCharID = function(_, id) return T.clients[id] or { valid = false } end,
    }
  end,
  register_player_event = function(_, ev, fn) T.handlers[ev] = fn end,
  unregister_player_event = function() end,
  set_timer = function(name, ms) T.timers[name] = ms end,
  stop_timer = function(name) T.timers[name] = nil end,
}

local real_open, real_popen = io.open, io.popen
io.open = function(path, mode)
  if path == SCRIPT then return real_open(path, mode) end
  if mode and mode:sub(1, 1) == "w" then
    local parts = {}
    return { write = function(_, s) parts[#parts + 1] = s end, close = function() T.files[path] = table.concat(parts) end }
  end
  -- The zone's own scratch files are not read back, except the hashing lock's owner and, when a test asks, the cache.
  local own = path:match("/%.hailstone%-lock/owner$") or (T.read_cache and path:match("/%.hailstone%-cache$"))
  if T.files[path] == nil or (path:match("/%.hailstone") and not own) then return nil end
  T.opens[path] = (T.opens[path] or 0) + 1
  T.open_now = T.open_now + 1
  T.open_max = math.max(T.open_max, T.open_now)
  local pos = 0
  -- An open file sees it rewritten in place (T.files changed), as a real handle would.
  return {
    seek = function(_, whence, off)
      if whence == "end" then pos = #(T.files[path] or "") else pos = off end
      return pos
    end,
    read = function(_, n)
      if type(n) ~= "number" then n = #(T.files[path] or "") end -- "*a"
      local s = (T.files[path] or ""):sub(pos + 1, pos + n); pos = pos + #s; return s
    end,
    close = function() T.open_now = T.open_now - 1 end,
    lines = function()
      local text = T.files[path] or ""
      return text:gmatch("([^\n]*)\n")
    end,
  }
end
io.popen = function(cmd)
  local out = ""
  local one = cmd:match("^find '(client_files/[^']+)' %-maxdepth 0")
  if T.real_root and cmd:match("^find ") and not one then
    -- The script's own listing command, run by the shell in a scratch folder holding a real client_files.
    T.finds = T.finds + 1
    local p = real_popen("cd '" .. T.real_root .. "' && " .. cmd)
    out = p:read("*a")
    p:close()
  elseif one then
    T.stats = T.stats + 1
    if T.files[one] then out = #T.files[one] .. "\t" .. (T.mtimes[one] or "1700000000.0") .. "\n" end
  elseif cmd:match("^find ") then
    T.finds = T.finds + 1
    local names = {}
    for path, text in pairs(T.files) do
      local rel = path:match("^client_files/([^.].*)$")
      if rel then names[#names + 1] = rel .. "\t" .. #text .. "\t" .. (T.mtimes[path] or "1700000000.0") end
    end
    out = table.concat(names, "\n") .. "\n"
  elseif cmd:match("^powershell ") then
    -- The Windows commands: the script file the zone wrote, then the list (no argument) or the hashes of a list.
    local text = T.files["client_files/" .. cmd:match('"client_files\\([^"]+)"')] or ""
    T.powershell[#T.powershell + 1] = text
    local names = {}
    if text:find("Get-FileHash", 1, true) then
      for rel in (T.files["client_files/" .. cmd:match('" (%S+)$')] or ""):gmatch("[^\n]+") do
        names[#names + 1] = string.rep("cd", 32) .. "  " .. rel
      end
    else
      for path, data in pairs(T.files) do
        local rel = path:match("^client_files/([^.].*)$")
        if rel then names[#names + 1] = rel .. "\t" .. #data .. "\t638000000000000000" end
      end
    end
    out = table.concat(names, "\r\n") .. "\r\n"
  elseif cmd:match("^mkdir ") then
    -- The hashing lock: made once, refused while it is there (T.no_mkdir: never made, as on a read-only folder).
    -- Windows' cmd quotes and separates as Windows does: the same folder.
    local dir = cmd:match("^mkdir '([^']+)'") or cmd:match('^mkdir "([^"]+)"'):gsub("\\\\", "/")
    T.mkdirs = T.mkdirs + 1
    if not T.dirs[dir] and not T.no_mkdir then T.dirs[dir] = true; out = "taken\n" end
  elseif cmd:match("^rm %-rf ") or cmd:match("^rmdir ") then
    local dir = cmd:match("^rm %-rf '([^']+)'") or cmd:match('"([^"]+)"'):gsub("\\\\", "/")
    T.dirs[dir] = nil
    for path in pairs(T.files) do if path:sub(1, #dir + 1) == dir .. "/" then T.files[path] = nil end end
  elseif cmd:match("^mv %-f ") then
    local from, to = cmd:match("^mv %-f '([^']+)' '([^']+)'")
    T.files[to], T.files[from] = T.files[from], nil
  elseif cmd == "sleep 1" or cmd:match("^ping ") then
    T.now = T.now + 1
    T.sleeps = T.sleeps + 1
    if T.on_sleep then T.on_sleep(T.sleeps) end
  elseif cmd:match("sha256sum") then
    T.hashes = T.hashes + 1
    local names = {}
    for rel in (T.files["client_files/" .. cmd:match("%-a (%S+)")] or ""):gmatch("[^\n]+") do
      -- As GNU's: a name with a backslash is escaped, and its line starts with one.
      if rel:find("\\", 1, true) then
        names[#names + 1] = "\\" .. string.rep("ab", 32) .. "  " .. rel:gsub("\\", "\\\\")
      else
        names[#names + 1] = string.rep("ab", 32) .. "  " .. rel
      end
    end
    out = table.concat(names, "\n") .. "\n"
  end
  return { read = function() return out end, close = function() end }
end

function add_files(n, size)
  for i = 1, n do T.files[string.format("client_files/f%02d.bin", i)] = string.rep(string.char(64 + i), size) end
end

function client(id, name)
  local c = { id = id, name = name, sent = {}, vars = {}, valid = true }
  T.clients[id] = c
  function c:CharacterID() return c.id end
  function c:GetCleanName() return c.name end
  function c:QueuePacket(p) c.sent[#c.sent + 1] = p end
  function c:Message() end
  function c:SetEntityVariable(k, v) c.vars[k] = v end
  function c:GetEntityVariable(k) return c.vars[k] end
  return c
end

local function send(c, p) T.handlers[Event.unhandled_opcode]({ self = c, packet = p }) end

function list_request(c, first)
  local p = Packet(0x1434, 12)
  p:WriteInt32(0, 0x4c465348); p:WriteInt16(4, 1); p:WriteInt16(6, 1); p:WriteInt32(8, first or 0)
  send(c, p)
  return c.sent[#c.sent]
end

function chunk_request(c, id, index, offset)
  local before = #c.sent
  local p = Packet(0x1434, 24)
  p:WriteInt32(0, 0x4c465348); p:WriteInt16(4, 1); p:WriteInt16(6, 2)
  p:WriteInt32(8, id); p:WriteInt32(12, index); p:WriteInt32(16, offset); p:WriteInt32(20, 32768)
  send(c, p)
  if #c.sent == before then return nil end
  return c.sent[#c.sent]
end

function fire_timer(name)
  assert(T.timers[name], "the timer " .. name .. " runs")
  event_timer({ timer = name })
end

function guild_hello(c)
  local before = #c.sent
  local p = Packet(0x1430, 80)
  p:WriteInt32(0, 0x47534d4e); p:WriteInt16(4, 1); p:WriteInt16(6, 0)
  send(c, p)
  return #c.sent > before
end

function settings_request(c)
  local before = #c.sent
  local p = Packet(0x1432, 6)
  p:WriteInt32(0, 0x50534d4e); p:WriteInt16(4, 1)
  send(c, p)
  return #c.sent > before
end

function cancel_invite(c, inviter)
  local p = Packet(0x2a50, 152)
  p:WriteString(0, inviter); p:WriteString(64, c.name); p:WriteInt8(148, 3)
  send(c, p)
end

function load_script()
  dofile(SCRIPT)
  event_encounter_load({ encounter = {} })
end
)lua";

// Runs the harness, then `scenario`; "" when it ran to the end, else Lua's error. "skip" without the sources.
std::string run(const std::string& scenario) {
    const std::string path = script_path();
    if (!std::ifstream(path)) return "skip"; // an installed test binary run away from the sources
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushstring(L, path.c_str());
    lua_setglobal(L, "SCRIPT");
    std::string err;
    if (luaL_dostring(L, kHarness) != LUA_OK || luaL_dostring(L, scenario.c_str()) != LUA_OK)
        err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
    lua_close(L);
    return err;
}

void check_run(const char* scenario) {
    const std::string err = run(scenario);
    if (err == "skip") return;
    CHECK_MESSAGE(err.empty(), err);
}

} // namespace

TEST_CASE("Server script: a client's list request makes the file list again at most every files_rescan seconds") {
    check_run(R"lua(
add_files(2, 40000)
load_script()
assert(T.finds == 1, "the zone's start lists the files")
local c = client(1, "Alpha")
local page = list_request(c, 0)
assert(page:ReadInt32(12) == 2, "two files listed")
T.now = T.now + 299
list_request(c, 0)
assert(T.finds == 1, "kept for 300 s: " .. T.finds)
T.now = T.now + 1
list_request(c, 0)
assert(T.finds == 2, "listed again after 300 s")
for k = 1, 50 do list_request(client(100 + k, "C" .. k), 0) end
assert(T.finds == 2, "asking again and again (many characters: one is limited) lists nothing more")
T.data["hailstone:files_rescan"] = "0" -- never under 30 s, whatever the owner sets
T.now = T.now + 29
list_request(c, 0)
assert(T.finds == 2, "under 30 s")
T.now = T.now + 1
list_request(c, 0)
assert(T.finds == 3, "30 s")
T.data["hailstone:client_files"] = "0"
T.now = T.now + 1000
page = list_request(c, 0)
assert(T.finds == 3, "client_files 0: nothing listed")
assert(page:ReadInt32(12) == 0, "and an empty list sent")
)lua");
}

TEST_CASE("Server script: one zone hashes at a time; another waits for it and reuses its cache") {
    check_run(R"lua(
T.read_cache = true
eq.get_zone_short_name = function() return "qeynos" end
eq.get_zone_instance_id = function() return 7 end
add_files(3, 100)
-- Alone: the zone takes the lock, hashes, writes the cache and lets the lock go.
load_script()
assert(T.hashes == 1 and T.mkdirs == 1, "hashed under the lock: " .. T.hashes .. " " .. T.mkdirs)
assert(next(T.dirs) == nil and T.files["client_files/.hailstone-lock/owner"] == nil, "and let it go")
assert(T.files["client_files/.hailstone-cache"], "the cache is written")
for path in pairs(T.files) do
  assert(not path:find("hailstone%-hash") or path:find("qeynos%-7%-"), "scratch names carry the zone: " .. path)
end

-- Another zone holds the lock while two new files come: this one waits, then hashes only what it did not.
T.files["client_files/new1.bin"] = "one"
T.files["client_files/new2.bin"] = "two"
T.dirs["client_files/.hailstone-lock"] = true
T.files["client_files/.hailstone-lock/owner"] = T.now .. " freportw-0-1-abc\n"
local cache = T.files["client_files/.hailstone-cache"]
T.on_sleep = function(n)
  if n == 3 then -- the other zone is done: new1 in the cache it wrote, and the lock gone
    T.files["client_files/.hailstone-cache"] = cache .. "new1.bin\t3\t1700000000.0\t" .. string.rep("ef", 32) .. "\n"
    T.dirs["client_files/.hailstone-lock"] = nil
    T.files["client_files/.hailstone-lock/owner"] = nil
  end
end
T.hashes = 0
event_encounter_load({ encounter = {} })
assert(T.sleeps == 3, "waited for the other zone: " .. T.sleeps)
assert(T.hashes == 1, "hashed once, after it: " .. T.hashes)
local page = list_request(client(1, "Alpha"), 0)
assert(page:ReadInt32(12) == 5, "all five listed: " .. page:ReadInt32(12))
assert(T.files["client_files/.hailstone-cache"]:find(string.rep("ef", 32), 1, true), "the other zone's hash kept")
assert(next(T.dirs) == nil, "the lock let go")

-- When the other zone hashed everything, this one hashes nothing.
T.files["client_files/new3.bin"] = "three"
T.dirs["client_files/.hailstone-lock"] = true
T.files["client_files/.hailstone-lock/owner"] = T.now .. " freportw-0-1-abc\n"
cache = T.files["client_files/.hailstone-cache"]
T.on_sleep = function()
  T.files["client_files/.hailstone-cache"] = cache .. "new3.bin\t5\t1700000000.0\t" .. string.rep("01", 32) .. "\n"
  T.dirs["client_files/.hailstone-lock"] = nil
  T.files["client_files/.hailstone-lock/owner"] = nil
end
T.hashes, T.sleeps = 0, 0
event_encounter_load({ encounter = {} })
assert(T.sleeps == 1 and T.hashes == 0, "reused the cache: " .. T.sleeps .. " " .. T.hashes)
)lua");
}

TEST_CASE("Server script: a stale or ownerless hashing lock is removed; a held one never stalls a client's request") {
    check_run(R"lua(
add_files(2, 100)
-- A lock a zone left 1000 s ago is removed, and this zone hashes.
T.dirs["client_files/.hailstone-lock"] = true
T.files["client_files/.hailstone-lock/owner"] = (T.now - 1000) .. " gone-0-1-abc\n"
load_script()
assert(T.sleeps == 0 and T.hashes == 1, "no wait for a stale lock: " .. T.sleeps .. " " .. T.hashes)
local removed = false
for _, l in ipairs(T.logs) do if l:find("removing a hashing lock left by gone-0-1-abc", 1, true) then removed = true end end
assert(removed, "and it says so")
assert(next(T.dirs) == nil, "its own lock let go")

-- A folder where the lock cannot be made: after a few tries the zone hashes without it, not for minutes.
T.no_mkdir = true
T.files["client_files/c.bin"] = "c"
T.hashes, T.sleeps = 0, 0
event_encounter_load({ encounter = {} })
assert(T.hashes == 1 and T.sleeps == 9, "hashed after 10 tries: " .. T.hashes .. " " .. T.sleeps)
T.no_mkdir = false

-- With a list already, a client's request while another zone hashes keeps it and waits for nothing.
local c = client(1, "Alpha")
assert(list_request(c, 0):ReadInt32(12) == 3)
T.files["client_files/d.bin"] = "d"
T.dirs["client_files/.hailstone-lock"] = true
T.files["client_files/.hailstone-lock/owner"] = T.now .. " other-0-1-abc\n"
T.hashes, T.sleeps = 0, 0
T.now = T.now + 300
assert(list_request(c, 0):ReadInt32(12) == 3, "the old list")
assert(T.hashes == 0 and T.sleeps == 0, "nothing hashed, nothing waited for")
T.dirs["client_files/.hailstone-lock"] = nil
T.files["client_files/.hailstone-lock/owner"] = nil
T.now = T.now + 300
assert(list_request(c, 0):ReadInt32(12) == 4, "the new file at the next look")
)lua");
}

TEST_CASE("Server script: on Windows the commands write UTF-8, and a name outside ASCII is left out with a log line") {
    check_run(R"lua(
package.config = "\\\n;\n?\n!\n-\n"
T.files["client_files/hailstone.cfg"] = "[plugins]\n"
T.files["client_files/scripts/donn\195\169es.lua"] = "print(1)\n"
load_script()
assert(#T.powershell == 2, "a list and a hash: " .. #T.powershell)
for _, ps in ipairs(T.powershell) do
  assert(ps:find("[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false", 1, true), "UTF-8 out")
end
local page = list_request(client(1, "Alpha"), 0)
assert(page:ReadInt32(12) == 1, "only the ASCII name is listed: " .. page:ReadInt32(12))
local logged = false
for _, l in ipairs(T.logs) do
  if l:find("donn\195\169es.lua is not sent: a name outside ASCII", 1, true) then logged = true end
  assert(not l:find("could not hash", 1, true), l)
end
assert(logged, "the owner is told why")
)lua");
}

TEST_CASE("Server script: clients fetching different files keep each open, a few at a time") {
    check_run(R"lua(
add_files(10, 40000)
load_script()
local a, b = client(1, "Alpha"), client(2, "Beta")
local id = list_request(a, 0):ReadInt32(8)
for _ = 1, 10 do
  for off = 0, 32768, 32768 do
    local ra = chunk_request(a, id, 0, off)
    local rb = chunk_request(b, id, 1, off)
    assert(ra:ReadInt16(6) == 2 and rb:ReadInt16(6) == 2, "pieces sent")
    assert(ra:ReadInt32(20) == (off == 0 and 32768 or 40000 - 32768), "a piece's length")
  end
end
assert(T.opens["client_files/f01.bin"] == 1 and T.opens["client_files/f02.bin"] == 1,
       "two clients on two files: each opened once, not again at every piece")
T.now = T.now + 1
for i = 0, 9 do chunk_request(a, id, i, 0) end
assert(T.open_max == 8, "at most 8 files open: " .. T.open_max)
chunk_request(a, id, 0, 0)
assert(T.opens["client_files/f01.bin"] == 2, "the one used longest ago was closed")
)lua");
}

TEST_CASE("Server script: a client asking for file pieces too fast is held to files_chunks_per_second") {
    check_run(R"lua(
add_files(1, 40000)
load_script()
local a, b = client(1, "Alpha"), client(2, "Beta")
local id = list_request(a, 0):ReadInt32(8)
local sent = 0
for _ = 1, 1000 do if chunk_request(a, id, 0, 0) then sent = sent + 1 end end
assert(sent == 768, "768 a second by default: " .. sent)
assert(chunk_request(b, id, 0, 0) ~= nil, "another client is not held back")
fire_timer("hailstone:files")
assert(#a.sent == 1 + 768, "the same second: what waits still waits")
T.now = T.now + 1
fire_timer("hailstone:files")
assert(#a.sent == 1 + 769, "the next second: the request that waited (asked again, held once)")
assert(T.timers["hailstone:files"] == nil, "nothing waits: the timer stops")
assert(chunk_request(a, id, 0, 0) ~= nil, "and new ones go at once")
T.data["hailstone:files_chunks_per_second"] = "3"
T.now = T.now + 10 -- a bucket is read again within 10 s
sent = 0
for _ = 1, 10 do if chunk_request(a, id, 0, 0) then sent = sent + 1 end end
assert(sent == 3, "the owner's limit: " .. sent)
T.data["hailstone:files_chunks_per_second"] = "0"
T.now = T.now + 10
fire_timer("hailstone:files") -- the one held under the limit of 3 goes
T.now = T.now + 1
sent = 0
for _ = 1, 2000 do if chunk_request(a, id, 0, 0) then sent = sent + 1 end end
assert(sent == 1024, "0: the zone's limit still holds: " .. sent)
T.data["hailstone:files_chunks_per_second_zone"] = "0"
T.now = T.now + 10
fire_timer("hailstone:files")
sent = 0
for _ = 1, 2000 do if chunk_request(a, id, 0, 0) then sent = sent + 1 end end
assert(sent == 2000, "both 0: no limit")
)lua");
}

TEST_CASE("Server script: all of a zone's clients together are held to files_chunks_per_second_zone") {
    // Characters that ask and never acknowledge make EQEmu hold every piece for 30 s: alts must not add up.
    check_run(R"lua(
add_files(1, 40000)
load_script()
local cs = {}
for k = 1, 5 do cs[k] = client(k, "C" .. k) end
local id = list_request(cs[1], 0):ReadInt32(8)
local sent = 0
for k = 1, 5 do
  for _ = 1, 1000 do if chunk_request(cs[k], id, 0, k) then sent = sent + 1 end end
end
assert(sent == 1024, "1024 a second for the zone by default: " .. sent)
assert(#cs[1].sent == 1 + 768 and #cs[2].sent == 256 and #cs[3].sent == 0, "the first at its own limit, the second the rest")
-- What waits is served as the zone's limit allows, never more in a second.
local before = 0
for k = 1, 5 do before = before + #cs[k].sent end
T.now = T.now + 1
fire_timer("hailstone:files")
local after = 0
for k = 1, 5 do after = after + #cs[k].sent end
assert(after - before <= 1024 and after - before == 5, "each one's held request (asked again, held once): " .. (after - before))
T.data["hailstone:files_chunks_per_second_zone"] = "100"
T.now = T.now + 10
sent = 0
for k = 1, 5 do
  for _ = 1, 50 do if chunk_request(cs[k], id, 0, 7) then sent = sent + 1 end end
end
assert(sent == 100, "the owner's zone limit: " .. sent)
)lua");
}

TEST_CASE("Server script: under the zone's limit, clients downloading at once are served in turn") {
    // Honest clients: each keeps 32 pieces asked for and asks for another as each one comes. Served first come,
    // first served, the ones that ask first would take the zone's whole limit every second.
    check_run(R"lua(
add_files(1, 32768 * 2000)
load_script()
T.data["hailstone:files_chunks_per_second_zone"] = "60"
local cs, asked, seen = {}, {}, {}
for k = 1, 3 do cs[k] = client(k, "C" .. k); asked[k] = 0; seen[k] = 0 end
local id = list_request(cs[1], 0):ReadInt32(8)
seen[1] = 1 -- the list
local function ask(k) chunk_request(cs[k], id, 0, asked[k] * 32768); asked[k] = asked[k] + 1 end
for k = 1, 3 do for _ = 1, 32 do ask(k) end end
for _ = 1, 10 do
  T.now = T.now + 1
  if T.timers["hailstone:files"] then fire_timer("hailstone:files") end
  -- Each client asks again for what came, the first client first, as at the start of every second.
  for k = 1, 3 do
    local got = #cs[k].sent - seen[k]
    seen[k] = #cs[k].sent
    for _ = 1, got do ask(k) end
  end
end
local total = 0
for k = 1, 3 do total = total + #cs[k].sent end
total = total - 1
assert(total <= 60 * 11, "never more than the zone's limit a second: " .. total)
for k = 1, 3 do
  local n = #cs[k].sent - (k == 1 and 1 or 0)
  assert(n >= total / 3 - 40, "client " .. k .. " has its share: " .. n .. " of " .. total)
end
)lua");
}

TEST_CASE("Server script: piece requests over the limit wait and go in order, none dropped") {
    check_run(R"lua(
add_files(1, 32768 * 40)
load_script()
local a = client(1, "Alpha")
local id = list_request(a, 0):ReadInt32(8)
T.data["hailstone:files_chunks_per_second"] = "10"
-- The client's window: 32 pieces at once.
local now = 0
for k = 0, 31 do if chunk_request(a, id, 0, k * 32768) then now = now + 1 end end
assert(now == 10, "10 at once: " .. now)
local offsets = {}
for s = 1, 3 do
  T.now = T.now + 1
  fire_timer("hailstone:files")
end
for k = 2, #a.sent do offsets[#offsets + 1] = a.sent[k]:ReadInt32(16) end
assert(#offsets == 32, "every piece came: " .. #offsets)
for k = 1, 32 do assert(offsets[k] == (k - 1) * 32768, "in the order asked: " .. k) end
-- More than FILES_WAIT_MAX waiting: the excess is dropped (the client asks again).
T.now = T.now + 1
for k = 0, 39 do chunk_request(a, id, 0, k * 32768) end
for k = 0, 39 do chunk_request(a, id, 0, k * 32768 + 1) end
for k = 0, 39 do chunk_request(a, id, 0, k * 32768 + 2) end
local before = #a.sent
for s = 1, 20 do
  T.now = T.now + 1
  if T.timers["hailstone:files"] then fire_timer("hailstone:files") end
end
assert(T.timers["hailstone:files"] == nil, "all served: the timer stopped")
assert(#a.sent - before == 64, "64 waited: " .. (#a.sent - before))
-- A client that leaves loses what waits for it.
T.now = T.now + 1
for k = 0, 19 do chunk_request(a, id, 0, k * 32768) end
a.valid = false
T.clients[1] = nil
T.now = T.now + 1
before = #a.sent
fire_timer("hailstone:files")
assert(#a.sent == before, "nothing sent to a client that left")
assert(T.timers["hailstone:files"] == nil, "and the timer stops")
)lua");
}

TEST_CASE("Server script: a file rewritten since it was hashed is not sent with its old hash") {
    check_run(R"lua(
add_files(2, 40000)
load_script()
local a = client(1, "Alpha")
local id = list_request(a, 0):ReadInt32(8)
assert(chunk_request(a, id, 0, 0):ReadInt16(6) == 2, "a piece")
assert(T.stats == 1, "a file's first piece looks at its size and time")
assert(chunk_request(a, id, 0, 32768):ReadInt16(6) == 2, "the next piece")
assert(T.stats == 1, "later pieces only at the size of the open file")
-- Rewritten in place, another size: the open file shows it.
T.files["client_files/f01.bin"] = string.rep("Z", 50000)
T.now = T.now + 30
local finds = T.finds
local r = chunk_request(a, id, 0, 32768)
assert(r:ReadInt16(6) == 3 and r:ReadInt32(16) == 1, "the list is stale: the client lists again")
assert(T.finds == finds + 1, "and the zone made the list again")
-- The same size, a new time: seen at the file's first piece.
id = list_request(a, 0):ReadInt32(8)
T.mtimes["client_files/f02.bin"] = "1800000000.0"
T.now = T.now + 30
r = chunk_request(a, id, 1, 0)
assert(r:ReadInt16(6) == 3 and r:ReadInt32(16) == 1, "a new time: stale")
assert(T.finds == finds + 2, "listed again")
id = list_request(a, 0):ReadInt32(8)
assert(chunk_request(a, id, 1, 0):ReadInt16(6) == 2, "the new list's file is sent")
-- Within 30 s of the last listing it is not made again, but nothing stale is sent.
T.files["client_files/f02.bin"] = string.rep("Q", 100)
r = chunk_request(a, id, 1, 0)
assert(r:ReadInt16(6) == 3 and r:ReadInt32(16) == 1, "stale")
assert(T.finds == finds + 2, "not listed again within 30 s")
)lua");
}

TEST_CASE("Server script: a same-size rewrite found within 30 s of a listing is reported at every request") {
    check_run(R"lua(
add_files(1, 40000)
load_script()
local a, b = client(1, "Alpha"), client(2, "Beta")
local id = list_request(a, 0):ReadInt32(8)
-- Rewritten with the same size, a new time, soon after the zone's start made the list: it is not made again.
T.files["client_files/f01.bin"] = string.rep("Z", 40000)
T.mtimes["client_files/f01.bin"] = "1800000000.0"
T.now = T.now + 5
local finds = T.finds
local r = chunk_request(a, id, 0, 0)
assert(r:ReadInt16(6) == 3 and r:ReadInt32(16) == 1, "a new time: stale")
assert(T.finds == finds, "within 30 s: not listed again")
-- The client lists again and gets the same entry; its first piece is still stale, for every player.
id = list_request(a, 0):ReadInt32(8)
r = chunk_request(a, id, 0, 0)
assert(r:ReadInt16(6) == 3 and r:ReadInt32(16) == 1, "asked again: still stale")
r = chunk_request(b, list_request(b, 0):ReadInt32(8), 0, 32768)
assert(r:ReadInt16(6) == 3 and r:ReadInt32(16) == 1, "another player, a later piece: still stale")
-- After 30 s the next request makes the list again and the new file goes out.
T.now = T.now + 30
r = chunk_request(a, id, 0, 0)
assert(r:ReadInt16(6) == 3, "stale once more, and the list is made again")
assert(T.finds == finds + 1, "listed again")
id = list_request(a, 0):ReadInt32(8)
assert(chunk_request(a, id, 0, 0):ReadInt16(6) == 2, "the new list's file is sent")
)lua");
}

TEST_CASE("Server script: group declines passed on to another zone are limited per sender") {
    check_run(R"lua(
load_script()
local a, b = client(1, "Alpha"), client(2, "Beta")
for _ = 1, 8 do cancel_invite(a, "Gamma") end
assert(#T.messages == 5, "5 in 10 s: " .. #T.messages)
assert(T.messages[1] == "Gamma: Alpha rejects your offer to join the group.", T.messages[1])
cancel_invite(b, "Gamma")
assert(#T.messages == 6, "another sender is not held back")
T.now = T.now + 9
cancel_invite(a, "Gamma")
assert(#T.messages == 6, "still the same 10 s")
T.now = T.now + 1
cancel_invite(a, "Gamma")
assert(#T.messages == 7, "the next 10 s")
)lua");
}

TEST_CASE("Server script: an inviter reaches at most 5 different players a minute across zones") {
    check_run(R"lua(
Database = function()
  return {
    prepare = function()
      return { execute = function() end, close = function() end,
               fetch_hash = function() return { permission = "255" } end }
    end,
    close = function() end,
  }
end
local ids = {}
eq.get_char_id_by_name = function(name) ids[name] = ids[name] or 100 + #name; return ids[name] end
eq.get_char_name_by_id = function(id) for n, v in pairs(ids) do if v == id then return n end end return "" end
eq.get_guild_id_by_char_id = function() return 0 end
local signals = {}
eq.cross_zone_signal_client_by_char_id = function(id) signals[#signals + 1] = id end
load_script()
local officer = client(1, "Officer")
local said = {}
function officer:IsInAGuild() return true end
function officer:GuildID() return 3 end
function officer:GuildRank() return 1 end
function officer:Message(_, text) said[#said + 1] = text end
local function invite(name)
  local p = Packet(0x1430, 80)
  p:WriteInt32(0, 0x47534d4e); p:WriteInt16(4, 1); p:WriteInt16(6, 1); p:WriteString(8, name)
  T.handlers[Event.unhandled_opcode]({ self = officer, packet = p })
end
local names = { "Aa", "Bbb", "Cccc", "Ddddd", "Eeeeee", "Fffffff" }
for _, n in ipairs(names) do invite(n) end
assert(#signals == 5, "five invitations sent: " .. #signals)
assert(said[#said]:find("You have invited 5 players", 1, true), tostring(said[#said]))
-- Asking one of the five again is not a new player (its own bucket answers that it is pending).
T.data["hailstone:guildinvite:" .. ids.Aa] = nil
invite("Aa")
assert(#signals == 6, "the same player again is allowed")
T.now = T.now + 60
invite("Fffffff")
assert(#signals == 7, "the next minute")
local logged = 0
for _, l in ipairs(T.logs) do if l:find("invites too many players", 1, true) then logged = logged + 1 end end
assert(logged == 1, "logged once: " .. logged)
)lua");
}

TEST_CASE("Server script: the settings' buckets are read at most every 10 s, not at every packet") {
    check_run(R"lua(
add_files(1, 40000)
load_script()
local a = client(1, "Alpha")
local id = list_request(a, 0):ReadInt32(8)
local gets = T.gets
for _ = 1, 200 do chunk_request(a, id, 0, 32768) end
assert(T.gets - gets <= 2, "200 pieces, at most one read for each limit: " .. (T.gets - gets))
for _ = 1, 3 do settings_request(a); guild_hello(a) end
gets = T.gets
for _ = 1, 3 do T.now = T.now + 1; settings_request(a) end
assert(T.gets == gets, "settings within 10 s: no read")
T.data["hailstone:enabled"] = "0"
T.now = T.now + 10
assert(not settings_request(a), "the bucket's 0 is seen within 10 s")
T.data["hailstone:enabled"] = "1"
assert(not settings_request(a), "still the 0 read within 10 s")
event_encounter_load({ encounter = {} }) -- (which reads enabled itself, for the file list)
assert(settings_request(a), "#reloadquest reads them at once")
)lua");
}

TEST_CASE("Server script: enabled 0 answers no file request and lists no files at the zone's start") {
    check_run(R"lua(
add_files(2, 40000)
T.data["hailstone:enabled"] = "0"
load_script()
assert(T.finds == 0, "enabled 0: the zone's start lists nothing: " .. T.finds)
local a = client(1, "Alpha")
assert(list_request(a, 0) == nil, "no list sent")
assert(chunk_request(a, 1, 0, 0) == nil, "no piece sent")
assert(T.finds == 0 and T.stats == 0, "nothing looked at")
T.data["hailstone:enabled"] = "1"
event_encounter_load({ encounter = {} }) -- #reloadquest: the buckets read at once
assert(T.finds == 1, "enabled again: listed")
assert(list_request(a, 0):ReadInt32(12) == 2, "and answered")
)lua");
}

TEST_CASE("Server script: a file's first piece asked for again and again looks at the file once") {
    check_run(R"lua(
add_files(2, 40000)
load_script()
local a = client(1, "Alpha")
local id = list_request(a, 0):ReadInt32(8)
for _ = 1, 500 do chunk_request(a, id, 0, 0) end
assert(T.stats == 1, "one find for the file, not one a request: " .. T.stats)
chunk_request(a, id, 1, 0)
assert(T.stats == 2, "the other file: its own")
-- A new list looks again.
T.now = T.now + 300
id = list_request(a, 0):ReadInt32(8)
chunk_request(a, id, 0, 0)
assert(T.stats == 3, "after the next listing: " .. T.stats)
)lua");
}

TEST_CASE("Server script: an empty client_files list is logged once until files are listed again") {
    check_run(R"lua(
load_script()
local function count()
  local n = 0
  for _, l in ipairs(T.logs) do if l:find("client_files: no files listed", 1, true) then n = n + 1 end end
  return n
end
assert(count() == 1, "the zone's first listing found nothing: " .. count())
local a = client(1, "Alpha")
T.now = T.now + 400
list_request(a, 0)
assert(count() == 1, "not again at every listing")
add_files(1, 100)
T.now = T.now + 400
list_request(a, 0)
for k in pairs(T.files) do T.files[k] = nil end
T.now = T.now + 400
list_request(a, 0)
assert(count() == 2, "again once the files that were there are gone: " .. count())
)lua");
}

TEST_CASE("Server script: list, guild and settings requests are limited per character") {
    check_run(R"lua(
add_files(1, 100)
load_script()
local a, b = client(1, "Alpha"), client(2, "Beta")
local pages = 0
for _ = 1, 20 do
  local before = #a.sent
  list_request(a, 0)
  if #a.sent > before then pages = pages + 1 end
end
assert(pages == 8, "8 whole lists a minute: " .. pages)
assert(list_request(b, 0) ~= nil and #b.sent == 1, "another character is not held back")
T.now = T.now + 60
local before = #a.sent
list_request(a, 0)
assert(#a.sent == before + 1, "the next minute")

local hellos = 0
for _ = 1, 15 do if guild_hello(a) then hellos = hellos + 1 end end
assert(hellos == 10, "10 guild packets in 10 s: " .. hellos)
local answers = 0
for _ = 1, 8 do if settings_request(a) then answers = answers + 1 end end
assert(answers == 5, "5 settings requests in 10 s: " .. answers)
local dropped = 0
for _, l in ipairs(T.logs) do if l:find("asks for the settings too fast", 1, true) then dropped = dropped + 1 end end
assert(dropped == 1, "the first dropped settings request is logged once: " .. dropped)
T.now = T.now + 10
assert(guild_hello(a) and settings_request(a), "the next 10 s")
)lua");
}

TEST_CASE("Server script: a flood of piece requests is dropped before any work, and none rescans the held ones") {
    check_run(R"lua(
add_files(1, 40000)
load_script()
local a, b = client(1, "Alpha"), client(2, "Beta")
local id = list_request(a, 0):ReadInt32(8)
for _ = 1, 768 do chunk_request(a, id, 0, 0) end
for _ = 1, 256 do chunk_request(b, id, 0, 0) end -- the zone's 1024 spent this second
local lookups = 0
local real_list = eq.get_entity_list
eq.get_entity_list = function()
  local l = real_list()
  local get = l.GetClientByCharID
  l.GetClientByCharID = function(self, cid) lookups = lookups + 1; return get(self, cid) end
  return l
end
chunk_request(b, id, 0, 1) -- held
for k = 1, 5000 do chunk_request(b, id, 0, k) end
assert(lookups == 0, "the zone's limit is spent: the held ones are not looked at per packet: " .. lookups)
local over = 0
for _, l in ipairs(T.logs) do if l:find("asks for file pieces too often", 1, true) then over = over + 1 end end
assert(over == 1, "the flood is logged once: " .. over)
-- The next second the timer serves what is held, and requests count from 0 again.
T.now = T.now + 1
fire_timer("hailstone:files")
assert(lookups > 0, "the timer looks at them")
local before = #a.sent
chunk_request(a, id, 0, 0)
assert(#a.sent == before + 1 or T.timers["hailstone:files"] ~= nil, "a new second's request is taken")
)lua");
}

TEST_CASE("Server script: settings requests without the magic are counted before they are logged") {
    check_run(R"lua(
add_files(1, 100)
load_script()
local a = client(1, "Alpha")
local p = Packet(0x1432, 6)
p:WriteInt32(0, 0x12345678)
for _ = 1, 50 do T.handlers[Event.unhandled_opcode]({ self = a, packet = p }) end
local bad = 0
for _, l in ipairs(T.logs) do if l:find("without the NMSP magic", 1, true) then bad = bad + 1 end end
assert(bad == 5, "5 a character in 10 s are looked at: " .. bad)
)lua");
}

TEST_CASE("Server script: a file whose name has a backslash is hashed from sha256sum's escaped line") {
    check_run(R"lua(
T.files["client_files/back\\slash.txt"] = "x"
T.files["client_files/plain.txt"] = "y"
load_script()
for _, l in ipairs(T.logs) do assert(not l:find("could not hash", 1, true), l) end
local a = client(1, "Alpha")
assert(list_request(a, 0):ReadInt32(12) == 2, "both listed")
)lua");
}

#if defined(__linux__)
TEST_CASE("Server script: a client_files that is a link to a folder elsewhere is listed, hailstone.cfg with it") {
    // The script's real find command in a scratch folder: GNU find without -H lists nothing under a starting point
    // that is a link, so the clients were sent no files and no hailstone.cfg (no server rules), with no log line.
    namespace fs = std::filesystem;
    hailstone::test::ScratchDir dir("server_files_link");
    fs::create_directories(dir.path() / "elsewhere" / "scripts");
    std::ofstream(dir.path() / "elsewhere" / "hailstone.cfg") << "[plugins]\n";
    std::ofstream(dir.path() / "elsewhere" / "scripts" / "a.lua") << "print(1)\n";
    std::ofstream(dir.path() / "elsewhere" / ".hidden") << "x";
    fs::create_directory_symlink(dir.path() / "elsewhere", dir.path() / "client_files");
    std::string root = dir.path().string();
    REQUIRE(root.find('\'') == std::string::npos);
    check_run(("T.real_root = '" + root + "'\n" + R"lua(
load_script()
local page = list_request(client(1, "Alpha"), 0)
assert(page:ReadInt32(12) == 2, "two files under the link: " .. page:ReadInt32(12))
local names = {}
local pos = 24
for _ = 1, page:ReadInt32(20) do
  local n = page:ReadInt16(pos + 36)
  local t = {}
  for i = 0, n - 1 do t[#t + 1] = string.char(page:ReadInt8(pos + 38 + i)) end
  names[#names + 1] = table.concat(t)
  pos = pos + 38 + n
end
assert(names[1] == "hailstone.cfg" and names[2] == "scripts/a.lua", table.concat(names, ","))
)lua").c_str());
}
#endif

TEST_CASE("Server script: clients on the old opcodes 0x1422-0x1427 are answered on them") {
    check_run(R"lua(
add_files(1, 100)
load_script()
local old, new = client(1, "Old"), client(2, "New")
local function send(c, p) T.handlers[Event.unhandled_opcode]({ self = c, packet = p }) end
local function ask(c, op, size, magic, action, first)
  local p = Packet(op, size)
  p:WriteInt32(0, magic); p:WriteInt16(4, 1); p:WriteInt16(6, action or 0)
  if first then p:WriteInt32(8, first) end
  local before = #c.sent
  send(c, p)
  assert(#c.sent == before + 1, string.format("0x%04x is answered", op))
  return c.sent[#c.sent]
end

-- Hailstone 0.7.0: settings, the guild hello, a page of the file list and a piece, all on the old numbers.
assert(ask(old, 0x1424, 6, 0x50534d4e).op == 0x1425, "settings")
assert(ask(old, 0x1422, 80, 0x47534d4e).op == 0x1423, "guild hello")
local page = ask(old, 0x1426, 12, 0x4c465348, 1, 0)
assert(page.op == 0x1427 and page:ReadInt16(6) == 1 and page:ReadInt32(12) == 1, "the file list")
local p = Packet(0x1426, 24)
p:WriteInt32(0, 0x4c465348); p:WriteInt16(4, 1); p:WriteInt16(6, 2)
p:WriteInt32(8, page:ReadInt32(8)); p:WriteInt32(12, 0); p:WriteInt32(16, 0); p:WriteInt32(20, 32768)
send(old, p)
assert(old.sent[#old.sent].op == 0x1427 and old.sent[#old.sent]:ReadInt16(6) == 2, "a piece")

-- A current client, and the old one once it asks on the new numbers (an update between logins).
assert(ask(new, 0x1432, 6, 0x50534d4e).op == 0x1433, "settings, new")
assert(ask(new, 0x1430, 80, 0x47534d4e).op == 0x1431, "guild hello, new")
assert(list_request(new, 0).op == 0x1435, "the file list, new")
assert(ask(old, 0x1432, 6, 0x50534d4e).op == 0x1433, "a client asking on the new numbers is answered on them")

-- 0x1423, 0x1425 and 0x1427 are the server's own answers: a client sending them gets nothing.
for _, op in ipairs({0x1423, 0x1425, 0x1427}) do
  local q = Packet(op, 80)
  q:WriteInt32(0, 0x50534d4e)
  local before = #new.sent
  send(new, q)
  assert(#new.sent == before, string.format("0x%04x is not answered", op))
end
)lua");
}

TEST_CASE("Server script: the Find window's quest NPCs, found as EQEmu finds their quest files, made once a zone") {
    check_run(R"lua(
T.files["eqemu_config.json"] = '{"server":{"directories":{"quest_paths":["quests/","hailstone/quests/"]}}}'
T.files["quests/qeynos/Banker_Bob.pl"] = 'sub EVENT_SAY { quest::say("Hail"); }'
T.files["quests/qeynos/v0/1234.lua"] = 'function event_trade(e) end\nfunction event_say(e) end' -- by id, versioned
T.files["quests/global/Wanderer.lua"] = 'local function x() end\nevent_say = function(e) end'
T.files["quests/qeynos/default.pl"] = 'sub EVENT_SAY {}'   -- runs for every NPC: never counts
T.files["quests/qeynos/a_rat.pl"] = 'sub EVENT_DEATH {}'    -- a scripted mob, no quest
eq.get_zone_short_name = function() return "qeynos" end
eq.get_zone_instance_version = function() return 0 end
local npcs = { { 1, "Banker_Bob00" }, { 1234, "Guard_Gehnus01" }, { 7, "a_rat03" }, { 8, "Merchant_Mo00" },
               { 9, "Wanderer00" }, { 1, "Banker_Bob01" } }
eq.get_entity_list = function()
  return { GetNPCList = function()
    local i = 0
    return { entries = function()
      i = i + 1
      local n = npcs[i]
      if not n then return nil end
      return { GetNPCTypeID = function() return n[1] end, GetName = function() return n[2] end }
    end }
  end }
end
load_script()
local c = client(1, "Tester")
local function ask()
  local before = #c.sent
  local p = Packet(0x1436, 6)
  p:WriteInt32(0, 0x4e515348); p:WriteInt16(4, 1)
  T.handlers[Event.unhandled_opcode]({ self = c, packet = p })
  return #c.sent > before and c.sent[#c.sent] or nil
end
local r = assert(ask(), "answered")
assert(r.op == 0x1437 and r:ReadInt32(0) == 0x4e515348 and r:ReadInt16(4) == 1)
local got, pos = {}, 8
for _ = 1, r:ReadInt16(6) do
  local flags, len = r:ReadInt8(pos), r:ReadInt8(pos + 1)
  local name = {}
  for k = 0, len - 1 do name[#name + 1] = string.char(r.b[pos + 2 + k]) end
  got[table.concat(name)] = flags
  pos = pos + 2 + len
end
assert(pos == r:GetSize(), "the size is the entries'")
assert(got["Banker_Bob"] == 2, "talks")
assert(got["Guard_Gehnus"] == 3, "hand-ins and talks, by id in the version's folder")
assert(got["Wanderer"] == 2, "global by name")
assert(got["a_rat"] == nil and got["Merchant_Mo"] == nil, "no quest: not listed")
-- made once: the files are not read again for the next request
local opens = T.opens["quests/qeynos/Banker_Bob.pl"]
assert(opens == 1, "read once, got " .. tostring(opens))
assert(ask())
assert(T.opens["quests/qeynos/Banker_Bob.pl"] == 1)
-- three a character in 10 s
assert(ask() and not ask(), "the fourth in 10 s is dropped")
-- switched off with the bucket
T.now = T.now + 11
T.data["hailstone:quest_npcs"] = "0"
assert(not ask(), "quest_npcs 0: no answer")
)lua");
}
