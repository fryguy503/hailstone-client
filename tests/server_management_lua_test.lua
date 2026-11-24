-- Run from an empty scratch directory with Lua 5.4:
--   lua <repo>/tests/server_management_lua_test.lua <repo>/server/quests/global/encounters/hailstone.lua
-- The encounter runs with stub EQEmu clients and packets and real files and shell file operations.
local script = assert(arg[1], "supply the encounter's absolute path")
local windows = package.config:sub(1, 1) == "\\"
local path = "client_files/hailstone.cfg"
local history = "client_files/.hailstone-admin-history"
local lock = "client_files/.hailstone-admin-lock"
local now = os.time()

local function read(name)
	local f = io.open(name, "rb")
	if not f then return nil end
	local bytes = assert(f:read("*a"))
	assert(f:close())
	return bytes
end

local function write(name, bytes)
	local f = assert(io.open(name, "wb"))
	assert(f:write(bytes))
	assert(f:close())
end

local function mkdir(name)
	assert(os.execute(windows and ('mkdir "' .. name:gsub("/", "\\") .. '"') or ('mkdir "' .. name .. '"')))
end

local function rmdir(name)
	assert(os.execute(windows and ('rmdir "' .. name:gsub("/", "\\") .. '"') or ('rmdir "' .. name .. '"')))
end

local function packet(opcode, bytes)
	local p = { data = bytes }
	function p:GetSize() return #self.data end
	function p:GetProtocolOpcode() return opcode end
	function p:ReadInt8(at) return (string.unpack("<i1", self.data, at + 1)) end
	function p:ReadInt16(at) return (string.unpack("<i2", self.data, at + 1)) end
	function p:ReadInt32(at) return (string.unpack("<i4", self.data, at + 1)) end
	function p:ReadFixedLengthString(at, length)
		return (self.data:sub(at + 1, at + length):gsub("%z.*$", ""))
	end
	function p:WriteFixedLengthString(at, value, length)
		assert(at + length <= #self.data)
		self.data = self.data:sub(1, at) .. value .. self.data:sub(at + length + 1)
	end
	function p:WriteInt8(at, value) self:WriteFixedLengthString(at, string.char(value % 256), 1) end
	function p:WriteInt16(at, value) self:WriteFixedLengthString(at, string.pack("<I2", value % 65536), 2) end
	function p:WriteInt32(at, value) self:WriteFixedLengthString(at, string.pack("<I4", value % 4294967296), 4) end
	return p
end

local function client(id, rank)
	local c = { rank = rank, packets = {}, entity = id, vars = {} }
	function c:Admin() return self.rank end
	function c:CharacterID() return id end
	function c:AccountID() return id + 100 end
	function c:GetID() return self.entity end
	function c:GetCleanName() return "Player" .. id end
	function c:GetEntityVariable(key) return self.vars[key] end
	function c:SetEntityVariable(key, value) self.vars[key] = value end
	function c:QueuePacket(p, reliable) self.packets[#self.packets + 1] = { packet = p, reliable = reliable } end
	return c
end

local function zone(name, opts)
	local logs, handlers, bucket = {}, {}, {}
	local api = {
		debug = function(message) logs[#logs + 1] = message end,
		get_data = function(key) return bucket[key] end,
		get_zone_short_name = function() return name end,
		register_player_event = function(_, event, fn) handlers[event] = fn end,
	}
	local layer = setmetatable({ open = function(filename, mode)
		if opts and opts.fail_stage and mode == "wb" and filename:match("^client_files/%.hailstone%-admin%-.*%.tmp$") then
			return nil, "staging unavailable", 13
		end
		return io.open(filename, mode)
	end }, { __index = io })
	local env = setmetatable({ eq = api, Event = { unhandled_opcode = 1, signal = 2 }, io = layer,
		os = setmetatable({ time = function() return now end }, { __index = os }),
		Packet = function(op, size, raw)
			assert(raw)
			return packet(op, ("\0"):rep(size))
		end }, { __index = _G })
	assert(loadfile(script, "t", env))()
	env.event_encounter_load({ encounter = {} })
	return handlers[1], logs, bucket
end

local function transfer(handler, who, op, bytes, optional)
	now = now + 1
	local before = #who.packets
	handler({ self = who, packet = packet(op, bytes) })
	if optional and #who.packets == before then return nil end
	assert(#who.packets == before + 1, "unexpected number of replies")
	return who.packets[#who.packets]
end

local nonce = 0
local function request(handler, who, action, body)
	nonce = nonce + 1
	local envelope = transfer(handler, who, 0x143a, string.pack("<I4I2I2I4", 0x4d415348, 1, action, nonce) .. (body or ""))
	assert(envelope.reliable)
	local p = envelope.packet
	assert(p:GetProtocolOpcode() == 0x143b)
	local magic, version, reply_action, id, status, revision, len, pos = string.unpack("<I4I2I2I4I2I4I2", p.data)
	assert(magic == 0x4d415348 and version == 1 and reply_action == action and id == nonce)
	assert(pos + len == #p.data + 1)
	return status, revision, p.data:sub(pos)
end

local function open(handler, who, status)
	local actual, revision, data = request(handler, who, 1)
	assert(actual == (status or 0), data)
	return revision, data
end

local function edit(section, key, value)
	return string.pack("<I1I1I2", #section, #key, #value) .. section .. key .. value
end

local function save(handler, who, rev, status, ...)
	local changes = { ... }
	local actual, revision, data = request(handler, who, 2, string.pack("<I4I2", rev, #changes) .. table.concat(changes))
	assert(actual == status, string.format("expected %d, got %d: %s", status, actual, data))
	return revision, data
end

local function settings_reply(handler, who, legacy)
	local response = transfer(handler, who, legacy and 0x1424 or 0x1432, string.pack("<I4I2", 0x50534d4e, 1)).packet
	assert(response:GetProtocolOpcode() == (legacy and 0x1425 or 0x1433))
	local flags = response:ReadInt32(6)
	if flags & 4 ~= 0 then
		assert(not legacy and response:GetSize() == 12)
		return flags, response:ReadInt16(10)
	end
	assert(response:GetSize() == 10)
	return flags, nil
end

local function file_hash(handler, who)
	local response = transfer(handler, who, 0x1434, string.pack("<I4I2I2I4", 0x4c465348, 1, 1, 0)).packet
	assert(response:GetProtocolOpcode() == 0x1435 and response:ReadInt32(20) == 1)
	return response.data:sub(29, 60)
end

assert(read(path) == nil, "run in an empty scratch directory")
mkdir("client_files")
local opts = {}
local a = zone("test_a", opts)
local ordinary, low_gm, gm, other = client(1, 0), client(2, 199), client(3, 200), client(4, 250)
local flags, rank = settings_reply(a, ordinary)
assert(flags == 4 and rank == 0)
flags, rank = settings_reply(a, low_gm)
assert(flags == 4 and rank == 199)
flags, rank = settings_reply(a, gm)
assert(flags == 6 and rank == 200)
flags, rank = settings_reply(a, other, true)
assert(flags == 2 and rank == nil, "old settings replies keep their original size")
local missing_api = client(5, 250)
missing_api.Admin = nil
flags, rank = settings_reply(a, missing_api)
assert(flags == 0 and rank == nil, "older server APIs still get a usable settings reply")
flags, rank = settings_reply(a, client(6, 32768))
assert(flags == 0 and rank == nil)
flags, rank = settings_reply(a, client(7, -1))
assert(flags == 4 and rank == 0)
open(a, ordinary, 1)
open(a, low_gm, 1)
local packets = #gm.packets
a({ self = gm, packet = packet(0x143a, string.pack("<I4I2I2I4", 0x4d415348, 1, 1, 1)), connecting = true })
assert(#gm.packets == packets, "only clients that entered the zone can open management")
local rev, proposed = open(a, gm)
assert(proposed:find("[scripts]\nplayer = on", 1, true) and proposed:find("mode = full\ncaps = all", 1, true))
assert(read(path) == nil, "opening an absent policy must not create or apply anything")
write(path, "")
local empty_rev, empty = open(a, gm)
assert(empty_rev ~= rev and empty == "", "an existing empty file uses configured defaults")
write(path, "[management]\nmin_status = 200\0\n")
assert(settings_reply(a, gm) == 4)
open(a, gm, 4) -- fixed-length string decoders do not transport an embedded NUL
assert(os.remove(path))
rev = open(a, gm)
local timestamp = os.date("!%Y%m%dT%H%M%SZ", now + 1)
mkdir(history)
write(history .. "/" .. timestamp .. "-0001.cfg", "existing backup")
write(history .. "/" .. timestamp .. "-0002.txt", "existing metadata")
rev = save(a, gm, rev, 0, edit("commands", "default", "allow"))
local current = assert(read(path))
assert(current:find("\nplayer = on\n", 1, true) and current:find("mode = full\ncaps = all", 1, true))
local first_revision = assert(current:match("^; hailstone%-policy%-revision = ([%w_-]+)"))
assert(first_revision == timestamp .. "-0003", "revision selection must check both history filenames")
assert(read(history .. "/" .. timestamp .. "-0001.cfg") == "existing backup")
assert(read(history .. "/" .. timestamp .. "-0002.txt") == "existing metadata")
assert(read(history .. "/" .. first_revision .. ".cfg") == "")
assert(read(history .. "/" .. first_revision .. ".txt"):find("previous_existed=false", 1, true))

local original = "; custom comment\r\n[gameplay_custom]\r\nmy_option = On # keep me\r\n"
	.. "[Movement]\r\nNPC_Collision = yes ; earlier duplicate\r\n"
	.. "[addons]\r\nsuggest = https://github.com/Owner/Repo/releases/tag/V1.2.0\r\n"
	.. "required = Owner/Old:V1.2.3-RC.1\r\n"
	.. "[movement] ; duplicate section\r\nNPC_Collision   =   on   ; keep my spacing\r\n"
	.. '[COMMANDS]\r\nblocked_message = "old # message" ; keep this too\r\n'
	.. "[existing_custom]\r\nvalue = untouched"
write(path, original)
rev = open(a, gm)
local b = zone("test_b")
local other_rev = open(b, other)
local old_hash = file_hash(b, other)
local message = 'Ask an admin #help; "Édit" the rule.'
rev, current = save(a, gm, rev, 0, edit("movement", "npc_collision", "off"), edit("commands", "blocked_message", message))
assert(current == read(path))
assert(current:find("NPC_Collision = yes ; earlier duplicate", 1, true))
assert(current:find("NPC_Collision   =   off   ; keep my spacing", 1, true))
assert(current:find("my_option = On # keep me", 1, true))
assert(current:find("; keep this too", 1, true))
assert(current:find('"' .. message .. '"', 1, true))
assert(current:find("required = Owner/Old:V1.2.3-RC.1", 1, true))
assert(current:find("suggest = https://github.com/Owner/Repo/releases/tag/V1.2.0", 1, true))
assert(current:sub(-#"value = untouched") == "value = untouched")
local second_revision = assert(current:match("^; hailstone%-policy%-revision = ([%w_-]+)"))
assert(second_revision ~= first_revision)
assert(read(history .. "/" .. second_revision .. ".cfg") == original)
assert(read(history .. "/" .. second_revision .. ".txt"):find("edited=movement.npc_collision,commands.blocked_message", 1, true))
assert(file_hash(b, other) ~= old_hash, "a different zone must send the new policy at next login")
save(b, other, other_rev, 3, edit("client", "deny", "on"))
assert(read(path) == current)

gm.rank = 199
save(a, gm, rev, 1, edit("client", "deny", "on"))
gm.rank = 200
gm.entity = 70
save(a, gm, rev, 3, edit("client", "deny", "on"))
gm.entity = 3
rev = open(a, gm)
save(a, gm, rev, 2, edit("assets", "player", "on\n[client]"))
save(a, gm, rev, 2, edit("commands", "blocked_message", ("x"):rep(241)))
save(a, gm, rev, 2, edit("commands", "blocked_message", "bad \192\175"))
save(a, gm, rev, 2, edit("commands", "blocked_message", "NUL\0inside"))
for _, rule in ipairs({ "/nav:0", "/nav:32768", "/nav:bad", "/nav:-1", "/nav:200\n[client]", ":200", "/:200" }) do
	save(a, gm, rev, 2, edit("commands", "gm", rule))
end
save(a, gm, rev, 2, edit("custom", "private", "1"))
save(a, gm, rev, 2, edit("client", "deny", "on"), edit("client", "deny", "off"))
save(a, gm, rev, 2, edit("addons", "block", "owner/old"))
assert(read(path) == current)

mkdir(lock)
save(a, gm, rev, 5, edit("assets", "player", "off"))
rmdir(lock)
assert(read(path) == current)
rev, current = save(a, gm, rev, 0, edit("management", "min_status", "100"), edit("commands", "blocked_message", ""),
	edit("commands", "gm", ""),
	edit("addons", "suggest", "example/server-hud@1.0.0, https://github.com/example/other/tree/main"))
assert(current:find("min_status = 100", 1, true) and current:find('blocked_message = ""', 1, true))
flags, rank = settings_reply(b, low_gm)
assert(flags == 6 and rank == 199, "account rank and management eligibility travel separately")
open(b, low_gm)
write(path, current:gsub("min_status = 100", "min_status = typo"))
open(a, other, 1)
assert(settings_reply(a, gm) == 4)
write(path, current)

-- Neither a busy lock nor a failing backup can change the active policy. Using a dedicated fresh zone also
-- catches leaked snapshots from a different process or a recycled client entity.
local broken_history = "client_files/.test-saved-history"
assert(os.rename(history, broken_history))
write(history, "not a directory")
local conflict_rev = open(a, gm)
save(a, gm, conflict_rev, 4, edit("socials", "mode", "off"))
assert(read(path) == current)
assert(os.remove(history) and os.rename(broken_history, history))
opts.fail_stage = true
save(a, gm, conflict_rev, 4, edit("socials", "mode", "off"))
opts.fail_stage = false
assert(read(path) == current)
conflict_rev, current = save(a, gm, conflict_rev, 0, edit("scripts", "run", "hud, welcome/init"),
	edit("scripts", "player_caps", "read, target"), edit("plugins", "player", "off"),
	edit("plugins", "player_caps", "read, ui"), edit("assets", "player", "off"), edit("web", "allow", "example.com, [::1]"),
	edit("client", "deny", "off"), edit("abilities", "autoskill", "off"), edit("movement", "solid_ladders", "off"),
	edit("socials", "mode", "press"), edit("socials", "press_commands", "10"), edit("socials", "caps", "commands, chat"),
	edit("socials", "faces", "off"), edit("addons", "enforce", "on"), edit("addons", "default", "deny"),
	edit("addons", "required", "another/hud:1.0.0-Alpha.1+build.0"), edit("addons", "block", "another/bot"),
	edit("addons", "allow", "some/repo"), edit("commands", "allow_groups", "command_helpers"),
	edit("commands", "block_groups", "movement_automation, non_rof2"), edit("commands", "allow", "/lua"),
	edit("commands", "block", "', :, /face, /custom!"), edit("commands", "gm", "/nav:200, /custom!:32767, ::150"))
assert(read(path) == current and current:find("[::1]", 1, true))
assert(current:find("another/hud:1.0.0-Alpha.1+build.0", 1, true))
assert(current:find("gm = /nav:200, /custom!:32767, ::150", 1, true))
now = now + 301
save(a, gm, conflict_rev, 3, edit("client", "deny", "off"))

assert(os.rename(path, "client_files/Hailstone.cfg"))
local mixed_rev, mixed = open(b, other)
assert(mixed == current, "management must discover a recently renamed policy")
save(b, other, mixed_rev, 0, edit("movement", "passable_ladders", "off"))
if not windows then assert(not read(path), "must not create a second case-variant policy") end
assert(read("client_files/Hailstone.cfg"):find("passable_ladders = off", 1, true))
if not windows then
	write(path, "[management]\nmin_status = 1\n")
	open(b, other, 4) -- two case variants must not select an arbitrary policy or permit an editor to change it
	now = now + 301
	local page = transfer(b, other, 0x1434, string.pack("<I4I2I2I4", 0x4c465348, 1, 1, 0)).packet
	assert(page:ReadInt32(20) == 2)
	local expected = assert(read("client_files/Hailstone.cfg"))
	local chunk = transfer(b, other, 0x1434,
		string.pack("<I4I2I2I4I4I4I4", 0x4c465348, 1, 2, page:ReadInt32(8), 0, 0, #expected)).packet
	-- The client takes the first entry when another entry differs only in case.
	assert(chunk:ReadInt16(6) == 2 and chunk.data:sub(25) == expected,
		"an ambiguous cfg must not prevent ordinary client file delivery")
	assert(os.remove(path))
end

print("Server Management Lua tests passed")
