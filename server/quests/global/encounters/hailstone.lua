-- Hailstone client support for EQEmu servers: one Lua file, no C++ changes (server/README.md).
--
-- Hailstone's server policy is in client_files/hailstone.cfg. GMs can also edit it through Server Management.
-- The client downloads it with the other client files (below; docs/plugins.md),
-- along with the server's own plugins (client_files/plugins/) and scripts (client_files/scripts/). Without a
-- hailstone.cfg the client sets no rules.
--
-- It also carries guild invitations across zones. EQEmu's own /guildinvite finds the invitee only in the inviter's
-- zone. Hailstone sends OP_HailstoneGuild (raw 0x1430) instead when the invitee is not in its zone and the zone answered
-- its hello; every packet both ways is {u32 magic 'NMSG'; u16 version 1; u16 action; char name[64]; u32 guild id;
-- u32 value}, 80 bytes. Actions: 0 the hello after every zone-in (answered with OP_HailstoneGuildReply 0x1431 action 0,
-- value 1: world-wide invitations); 1 an invitation (name: whom) -> the script checks the inviter's permission,
-- leaves the invitation in a data bucket and signals the invitee's zone, which sends the invitee's client
-- OP_HailstoneGuildReply action 1 (name: the inviter); 2 the invitee's answer (value: the rank to accept, 9 or 11 to
-- decline) -> the invitee's zone puts them in the guild with eq.set_guild. A Hailstone client without the answer
-- to its hello, and any other client, keeps to EQEmu's own same-zone invitation.
--
-- And it sends the server's settings for the client: after every zone-in Hailstone sends OP_HailstoneSettingsRequest (raw
-- 0x1432): {u32 magic 'NMSP'; u16 version 1}, and this answers OP_HailstoneSettings (0x1433): {u32 magic; u16 version 1;
-- u32 flags; optional u16 account status}. Bit 1 advertises Server Management. Bit 2 means the account status
-- is present. Older clients ignore the appended field. Replies on the old opcodes keep their original size.
--
-- And it sends the server's client files: what the server owner puts in client_files/ (beside eqemu_config.json:
-- hailstone.cfg, spells_us.txt, eqstr_us.txt ..., and the folders assets/, plugins/ and scripts/). At its first
-- zone-in of a login Hailstone asks for the list (OP_HsFilesRequest raw 0x1434 action 1) and downloads what it lacks a piece at a time (action 2), answered with
-- OP_HsFiles 0x1435 (the client's net/server_files_packets.h has the layout). The list is SHA-256 hashed with
-- sha256sum (GNU find and xargs; on Windows PowerShell's Get-FileHash) when the zone starts (and at #reloadquest),
-- and again when a client asks and it is older than files_rescan seconds (300; never under 30); the hashes are kept
-- in client_files/.hailstone-cache, so only new or changed files are hashed again, by one zone at a time (the others
-- wait on client_files/.hailstone-lock and reuse what it hashed). Names starting with '.' are not
-- sent. A client may ask for files_chunks_per_second pieces a second (768, 24 MB/s), and all of the zone's clients
-- together for files_chunks_per_second_zone (1024, 32 MB/s); more wait their turn (a few dozen per client, on a
-- timer) and are served in order, a client at a time in turn. A file that changed on disk since it was hashed (its
-- size, or its time where find can tell) makes the zone hash again and the client list again, instead of sending
-- bytes that do not match the list.
--
-- And it tells the client's Find window which of the zone's NPCs have quests: Hailstone asks once a zone, when the
-- window first lists the zone's NPCs (OP_HsQuestNpcsRequest raw 0x1436: {u32 magic 'HSQN'; u16 version 1}), and this
-- answers OP_HsQuestNpcs 0x1437: {u32 magic; u16 version 1; u16 count; count x {u8 flags (1 takes hand-ins, 2
-- talks); u8 length; the npc_types name}}. An NPC has quests when its own quest file (found as EQEmu finds it:
-- quests/<zone>/v<version>/, quests/<zone>/, quests/global/; by id, name or name_id; .lua or .pl; the first in each
-- of quest_paths' folders) handles EVENT_ITEM / event_trade (hand-ins) or EVENT_SAY / event_say (talks). The default
-- scripts and global_npc are left out: they run for every NPC. The NPCs are the zone's spawn tables' (one query)
-- and the ones alive now (spawned by scripts). The list is made once per zone process, at the first request, and
-- kept (#reloadquest makes it again).
--
-- And it sends how the zones connect, for the map's zone finder: when its World view first opens in a session,
-- Hailstone asks (OP_HsZoneLinksRequest raw 0x1438: {u32 magic 'HSZL'; u16 version 1}) and this answers
-- OP_HsZoneLinks (0x1439: {u32 magic; u16 version 1; u16 flags 0; u32 count; count x {u16 from zone id; u16 to zone
-- id}; i16 current expansion (-1 all); u32 zone count; count x u16 zone id}; the client's net/zone_link_packets.h):
-- the zone_points rows of version 0 or -1 that the server's expansion and content flags allow, each pair of zones
-- once, then the zones they allow (the client lists and routes through only those). Built at the first request in a zone process with one query of
-- the content database (and one of content_flags), then kept in memory (#reloadquest builds it again); a
-- character is answered at most twice a minute. Without it, the client learns the zone lines of the zones it visits.
--
-- global/script_init.lua loads it with eq.load_encounter("hailstone"). It registers its player event through the
-- encounter, so it runs beside the server's own player scripts (Perl or Lua) instead of replacing them.

-- Settings. Edit them here, or override one live with a data bucket "hailstone:<name>" (#databuckets), which each
-- zone picks up within SETTINGS_REFRESH seconds (at once at #reloadquest).
local settings = {
	enabled                = 1,
	guild_invites_world    = 1,  -- /guildinvite reaches players in other zones (Hailstone clients)
	client_files           = 1,  -- send client_files/ to Hailstone clients (0: they get an empty list)
	group_declines         = 1,  -- tell the inviter when a RoF2 client declines a group invitation
	files_rescan           = 300, -- seconds the client_files/ list is kept before a client's request looks again (min 30)
	files_chunks_per_second = 768, -- file pieces (32 KB) one client is sent a second, more wait; 0: no limit
	files_chunks_per_second_zone = 1024, -- the same for all of the zone's clients together; 0: no limit
	quest_npcs             = 1,  -- tell Hailstone's Find window which of the zone's NPCs have quests (0: say nothing)
	zone_links             = 1,  -- send the zone connections for the map's zone finder (0: never answered)
	server_management      = 0,  -- 1 enables the in-game editor for authorized GM accounts
	server_management_history = 0, -- keep all revisions unless the operator sets a positive retention limit
}

local OP_SETTINGS_REQUEST = 0x1432
local OP_SETTINGS         = 0x1433
local MAGIC      = 0x50534d4e

-- Hailstone 0.7.0 and older send the same packets 0x0e lower (0x1422-0x1427). They are still answered, on the
-- opcodes the client asked on, so a server that updates this script keeps serving clients that have not updated.
-- (On Project Triune's servers the old 0x1422 is its own opcode and never comes here: those old clients get no
-- cross-zone guild invitations, as before.) Kept per client entity, which a zone-in makes anew.
local OLD_OPCODES_VAR   = "hailstone_old_opcodes"
local OLD_OPCODES_SHIFT = 0x0e

-- The opcode to answer `client` on: `op`, or its old number for a client that asked on the old ones.
local function reply_op(client, op)
	if client:GetEntityVariable(OLD_OPCODES_VAR) == "1" then
		return op - OLD_OPCODES_SHIFT
	end
	return op
end

local function log(msg)
	eq.debug("[hailstone] " .. msg)
end

-- EQEmu caches no bucket without a character, account, bot or zone (DataBucket::CanCache): each eq.get_data of these
-- is a query on the zone's thread, and some are read at every packet. So each is read at most every
-- SETTINGS_REFRESH seconds.
local SETTINGS_REFRESH = 10
local setting_cache = {} -- name -> { value (nil: the default), at }

local function setting(name)
	local now = os.time()
	local c   = setting_cache[name]
	if c == nil or now - c.at >= SETTINGS_REFRESH then
		c = { value = tonumber(eq.get_data("hailstone:" .. name)), at = now }
		setting_cache[name] = c
	end
	if c.value == nil then
		return settings[name]
	end
	return c.value
end

local function clamp(v, lo, hi)
	if v < lo then return lo end
	if v > hi then return hi end
	return v
end

-- Requests a client may make, counted per character in windows of `seconds`. A new window drops the counts of
-- windows that are over, so the table holds only the characters that asked lately.
local function limiter(seconds)
	return { seconds = seconds, by_id = {} }
end

-- The current window's count for `id`.
local function window(l, id)
	local now = os.time()
	local r   = l.by_id[id]
	if r == nil or now - r.start >= l.seconds then
		for k, v in pairs(l.by_id) do
			if now - v.start >= l.seconds then
				l.by_id[k] = nil
			end
		end
		r = { start = now, count = 0 }
		l.by_id[id] = r
	end
	return r
end

local function allow(l, id, max)
	local r = window(l, id)
	r.count = r.count + 1
	return r.count <= max, r.count == max + 1
end

-- Runs one statement; returns the statement (to fetch from) or nil. Servers older than the Lua Database class
-- have no table access (and no guild invitations across zones).
local function query(db, sql)
	local stmt = db:prepare(sql)
	if stmt == nil then
		log("query failed: " .. sql:sub(1, 60))
		return nil
	end
	stmt:execute()
	return stmt
end

-- ---------------------------------------------------------------- guild invitations across zones

local OP_GUILD       = 0x1430
local OP_GUILD_REPLY = 0x1431
local GUILD_MAGIC    = 0x47534d4e
local GUILD_SIZE     = 80
local GUILD_HELLO, GUILD_INVITE, GUILD_ANSWER = 0, 1, 2
local GUILD_DECLINE  = 9   -- EQEmu GUILD_INVITE_DECLINE: this and above decline
local GUILD_RECRUIT  = 8
local PERM_INVITE    = 7   -- EQEmu GUILD_ACTION_MEMBERS_INVITE
local SIG_INVITE     = 14220001 -- to the invitee's zone: an invitation waits in its bucket
local INVITE_SECONDS = 20  -- the bucket's life (the client's prompt lapses after 10 s)
local DELIVER_MS     = 3000 -- no invitee's zone took it by then: not online
local CHAT_RED, CHAT_YELLOW = 13, 15
local HAILSTONE_VAR  = "hailstone_client"

local encounter -- for the delivery timers

local function guild_packet(client, action, name, guild_id, value)
	local p = Packet(reply_op(client, OP_GUILD_REPLY), GUILD_SIZE, true)
	p:WriteInt32(0, GUILD_MAGIC)
	p:WriteInt16(4, 1)
	p:WriteInt16(6, action)
	p:WriteString(8, (name or ""):sub(1, 63))
	p:WriteInt32(72, guild_id or 0)
	p:WriteInt32(76, value or 0)
	return p
end

-- The invitation waiting for an invitee (by character id): {inviter id, inviter name, guild id, rank, state},
-- state "p" until the invitee's zone takes it, then "d". One bucket key, not cached by the zones.
local function invite_key(char_id)
	return "hailstone:guildinvite:" .. char_id
end

local function read_invite(char_id)
	local v = eq.get_data(invite_key(char_id))
	if v == nil or v == "" then
		return nil
	end
	local inviter_id, inviter, guild_id, rank, state = v:match("^(%d+)|([^|]*)|(%d+)|(%d+)|(%a)$")
	if inviter_id == nil then
		return nil
	end
	return { inviter_id = tonumber(inviter_id), inviter = inviter, guild_id = tonumber(guild_id), rank = tonumber(rank), state = state }
end

local function write_invite(char_id, i)
	eq.set_data(invite_key(char_id), string.format("%d|%s|%d|%d|%s", i.inviter_id, i.inviter, i.guild_id, i.rank, i.state), tostring(INVITE_SECONDS) .. "s")
end

-- EQEmu's BaseGuildManager::CheckPermission on guild_permissions: the rank's bit (8 - rank) of the action's value.
local function has_guild_permission(guild_id, rank, permission)
	if rank < 1 or rank > 8 then
		return false
	end
	local db   = Database()
	local stmt = query(db, string.format("SELECT `permission` FROM `guild_permissions` WHERE `guild_id` = %d AND `perm_id` = %d", guild_id, permission))
	local allowed = false
	if stmt then
		local row = stmt:fetch_hash()
		if row then
			allowed = math.floor((tonumber(row.permission) or 0) / 2 ^ (8 - rank)) % 2 == 1
		end
		stmt:close()
	end
	db:close()
	return allowed
end

-- An invitation reaches any Hailstone player online, in any zone, as a prompt they must answer: at most
-- INVITE_TARGETS_MAX different players an inviter in INVITE_TARGETS_SECONDS, so an officer cannot prompt the whole
-- server. Counted by the inviter's zone (Hailstone's own limit; the stock handler reaches only its own zone).
local INVITE_TARGETS_MAX, INVITE_TARGETS_SECONDS = 5, 60
local invite_targets = limiter(INVITE_TARGETS_SECONDS)

-- The inviter's zone: the stock handler's checks (Client::Handle_OP_GuildInvite), with its words, then the
-- invitation goes to the invitee's zone.
local function guild_invite(client, name)
	if not client:IsInAGuild() then
		client:Message(CHAT_RED, "Error: You are not in a guild!")
		return
	end
	local guild_id = client:GuildID()
	if not has_guild_permission(guild_id, client:GuildRank(), PERM_INVITE) then
		client:Message(CHAT_RED, "You don't have permission to invite.")
		return
	end
	local target_id = 0
	if name:match("^%a+$") then
		target_id = eq.get_char_id_by_name(name)
	end
	if target_id == 0 then
		client:Message(CHAT_RED, string.format("Prospective guild member %s could not be found.", name))
		return
	end
	name = eq.get_char_name_by_id(target_id)
	if eq.get_guild_id_by_char_id(target_id) ~= 0 then
		client:Message(CHAT_RED, "Player is in a guild.")
		return
	end
	if read_invite(target_id) then
		client:Message(CHAT_RED, "That person is already considering a guild invitation.")
		return
	end
	local w = window(invite_targets, client:CharacterID())
	w.targets = w.targets or {}
	if not w.targets[target_id] then
		if (w.distinct or 0) >= INVITE_TARGETS_MAX then
			client:Message(CHAT_RED, string.format("You have invited %d players in the last minute; wait before inviting more.", INVITE_TARGETS_MAX))
			if not w.logged then
				log(string.format("%s invites too many players; more are refused for %d s", client:GetCleanName(), INVITE_TARGETS_SECONDS))
				w.logged = true
			end
			return
		end
		w.targets[target_id] = true
		w.distinct = (w.distinct or 0) + 1
	end
	write_invite(target_id, { inviter_id = client:CharacterID(), inviter = client:GetCleanName(), guild_id = guild_id, rank = GUILD_RECRUIT, state = "p" })
	eq.set_timer(string.format("guildinvite:%d:%d", client:CharacterID(), target_id), DELIVER_MS, encounter)
	eq.cross_zone_signal_client_by_char_id(target_id, SIG_INVITE)
	log(string.format("%s invites %s (%d) into guild %d", client:GetCleanName(), name, target_id, guild_id))
end

-- The invitee's zone (the signal): the invitation to the client, if it can answer it.
local function guild_deliver(client)
	local id = client:CharacterID()
	local i  = read_invite(id)
	if i == nil or i.state ~= "p" then
		return
	end
	if client:GetEntityVariable(HAILSTONE_VAR) ~= "1" then
		-- Another client answers only EQEmu's same-zone invitation, which this one cannot be.
		eq.delete_data(invite_key(id))
		eq.cross_zone_message_player_by_name(CHAT_RED, i.inviter, string.format("Prospective guild member %s must be in zone to perform guild operations on them.", client:GetCleanName()))
		return
	end
	if client:IsInAGuild() then
		eq.delete_data(invite_key(id))
		eq.cross_zone_message_player_by_name(CHAT_RED, i.inviter, "Player is in a guild.")
		return
	end
	i.state = "d"
	write_invite(id, i)
	client:QueuePacket(guild_packet(client, GUILD_INVITE, i.inviter, i.guild_id, i.rank))
end

-- What EQEmu sends a new member (EntityList::SendGuildMemberAdd: Client::SendGuildRanks, SendGuildRankNames,
-- SendGuildMembersList), sent here as raw RoF2 packets. EQEmu forwards a new member's ServerOP_GuildMemberAdd only to
-- the zones where world already sees a member of that guild, which leaves out an invitee alone in its zone (world
-- learns the new guild from the same set_guild a moment later): without these the client has no rank permissions
-- (its guild chat is not shown) and no roster until it zones.
local OP_GUILD_UPDATE      = 0x2958 -- OP_GuildUpdate (pass-through)
local OP_GUILD_MEMBER_LIST = 0x12a6 -- OP_GuildMemberList (ENCODE(OP_GuildMemberList)'s output)
local GUILD_MAX_FUNCTIONS  = 30

local function rows(sql)
	local out  = {}
	local db   = Database()
	local stmt = query(db, sql)
	if stmt then
		local row = stmt:fetch_hash()
		while row do
			local copy = {} -- fetch_hash refills one table
			for k, v in pairs(row) do
				copy[k] = v
			end
			out[#out + 1] = copy
			row = stmt:fetch_hash()
		end
		stmt:close()
	end
	db:close()
	return out
end

local function send_guild_state(client, guild_id)
	-- GuildUpdateRanks_Struct, action 5: {u32 action; u32; u32; char[64] name; u32 guild; u32 rank; u32 permission;
	-- u8 value; u8[3] 2c 01 00}, every rank 1..8 and permission 1..30 (BaseGuildManager::CheckPermission).
	local perms = {}
	for _, r in ipairs(rows(string.format("SELECT `perm_id`, `permission` FROM `guild_permissions` WHERE `guild_id` = %d", guild_id))) do
		perms[tonumber(r.perm_id) or -1] = tonumber(r.permission) or 0
	end
	for rank = 1, 8 do
		for perm = 1, GUILD_MAX_FUNCTIONS do
			local p = Packet(OP_GUILD_UPDATE, 92, true)
			p:WriteInt32(0, 5)
			p:WriteString(12, client:GetCleanName())
			p:WriteInt32(76, guild_id)
			p:WriteInt32(80, rank)
			p:WriteInt32(84, perm)
			p:WriteInt8(88, math.floor((perms[perm] or 0) / 2 ^ (8 - rank)) % 2)
			p:WriteInt8(89, 0x2c)
			p:WriteInt8(90, 0x01)
			client:QueuePacket(p)
		end
	end
	-- GuildUpdateUCPStruct, action 4: {u32 action; u8[76]; u32 rank; char[76] name}, the ranks with a name.
	for _, r in ipairs(rows(string.format("SELECT `rank`, `title` FROM `guild_ranks` WHERE `guild_id` = %d", guild_id))) do
		local rank, title = tonumber(r.rank) or 0, r.title or ""
		if rank >= 1 and rank <= 8 and title ~= "" then
			local p = Packet(OP_GUILD_UPDATE, 160, true)
			p:WriteInt32(0, 4)
			p:WriteInt32(80, rank)
			p:WriteString(84, title:sub(1, 75))
			client:QueuePacket(p)
		end
	end
	-- The roster, as ENCODE(OP_GuildMemberList) writes it: char[] guild name; u32 guild; u32 count; count x {char[] name;
	-- u32 level, banker (+2 alt), class, rank, last on, tribute on, 0, total tribute, last tribute, 1; char[] note;
	-- u16 instance; u16 zone (online members); u32 1; u32 0}, the numbers big-endian. GuildMemberBaseQuery's columns.
	local members = rows(string.format(
		"SELECT c.`name`, c.`class`, c.`level`, c.`last_login`, c.`zone_id`, g.`rank`, g.`tribute_enable`, g.`total_tribute`, " ..
		"g.`last_tribute`, g.`banker`, g.`public_note`, g.`alt`, g.`online` FROM `guild_members` AS g " ..
		"JOIN `character_data` AS c ON c.`id` = g.`char_id` WHERE g.`guild_id` = %d AND c.`deleted_at` IS NULL", guild_id))
	local guild_name = eq.get_guild_name_by_id(guild_id)
	local size = #guild_name + 1 + 8
	for _, m in ipairs(members) do
		size = size + #(m.name or "") + 1 + 40 + #(m.public_note or "") + 1 + 12
	end
	local p   = Packet(OP_GUILD_MEMBER_LIST, size, true)
	local pos = 0
	local function be32(v)
		v = math.floor(tonumber(v) or 0) % 4294967296
		p:WriteInt8(pos, math.floor(v / 16777216) % 256)
		p:WriteInt8(pos + 1, math.floor(v / 65536) % 256)
		p:WriteInt8(pos + 2, math.floor(v / 256) % 256)
		p:WriteInt8(pos + 3, v % 256)
		pos = pos + 4
	end
	local function cstr(str) p:WriteString(pos, str); pos = pos + #str + 1 end
	cstr(guild_name)
	be32(guild_id)
	be32(#members)
	for _, m in ipairs(members) do
		local online = tonumber(m.online) == 1
		cstr(m.name or "")
		be32(m.level)
		be32((tonumber(m.banker) or 0) + (tonumber(m.alt) or 0) * 2)
		be32(m.class)
		be32(m.rank)
		be32(m.last_login)
		be32(m.tribute_enable)
		be32(0)
		be32(m.total_tribute)
		be32(m.last_tribute)
		be32(1)
		cstr(m.public_note or "")
		local zone = online and (tonumber(m.zone_id) or 0) or 0
		p:WriteInt8(pos, 0); p:WriteInt8(pos + 1, 0)
		p:WriteInt8(pos + 2, math.floor(zone / 256) % 256); p:WriteInt8(pos + 3, zone % 256)
		pos = pos + 4
		be32(1)
		be32(0)
	end
	client:QueuePacket(p)
end

-- The invitee's answer, in whatever zone it is now (Client::Handle_OP_GuildInviteAccept's words).
local function guild_answer(client, inviter, guild_id, response)
	local id = client:CharacterID()
	local i  = read_invite(id)
	if i == nil or i.guild_id ~= guild_id or i.inviter:lower() ~= inviter:lower() then
		return
	end
	eq.delete_data(invite_key(id))
	if response >= GUILD_DECLINE then
		client:Message(CHAT_YELLOW, "You declined the guild invite.")
		eq.cross_zone_message_player_by_name(CHAT_YELLOW, i.inviter, "The player declined the guild invite.")
		return
	end
	if client:IsInAGuild() then
		client:Message(CHAT_YELLOW, "You are already in a guild.  Please leave that guild first.")
		eq.cross_zone_message_player_by_name(CHAT_YELLOW, i.inviter, "Player is already in a guild.")
		return
	end
	eq.set_guild(i.guild_id, i.rank) -- this client (the event's initiator): ZoneGuildManager::SetGuild
	if client:GuildID() == i.guild_id then
		send_guild_state(client, i.guild_id)
	end
	log(string.format("%s joined guild %d at rank %d", client:GetCleanName(), i.guild_id, i.rank))
end

-- A client sends its hello once a zone-in and an invitation or answer when the player acts: at most GUILD_MAX a
-- character in GUILD_SECONDS are looked at, before any setting or table is read.
local GUILD_MAX, GUILD_SECONDS = 10, 10
local guild_limit = limiter(GUILD_SECONDS)

local function on_guild_packet(client, packet)
	if packet:GetSize() < GUILD_SIZE or packet:ReadInt32(0) ~= GUILD_MAGIC or packet:ReadInt16(4) ~= 1 then
		return
	end
	local ok, first_over = allow(guild_limit, client:CharacterID(), GUILD_MAX)
	if not ok then
		if first_over then
			log(string.format("%s sends guild packets too fast; the rest of %d s are dropped", client:GetCleanName(), GUILD_SECONDS))
		end
		return
	end
	local action = packet:ReadInt16(6)
	if action == GUILD_HELLO then
		client:SetEntityVariable(HAILSTONE_VAR, "1")
		local on = setting("enabled") ~= 0 and setting("guild_invites_world") ~= 0 and Database ~= nil
		client:QueuePacket(guild_packet(client, GUILD_HELLO, "", 0, on and 1 or 0))
		return
	end
	if setting("enabled") == 0 or setting("guild_invites_world") == 0 or Database == nil then
		return
	end
	local name = packet:ReadString(8):sub(1, 63)
	if action == GUILD_INVITE then
		guild_invite(client, name)
	elseif action == GUILD_ANSWER then
		guild_answer(client, name, packet:ReadInt32(72), packet:ReadInt32(76))
	end
end

local function on_signal(e)
	if e.signal == SIG_INVITE then
		guild_deliver(e.self)
	end
end

-- The delivery timer "guildinvite:<inviter id>:<invitee id>": no zone took the invitation, so the invitee is offline.
function event_timer(e)
	local inviter_id, target_id = e.timer:match("^guildinvite:(%d+):(%d+)$")
	if inviter_id == nil then
		return
	end
	eq.stop_timer(e.timer, encounter)
	local i = read_invite(tonumber(target_id))
	if i == nil or i.state ~= "p" or i.inviter_id ~= tonumber(inviter_id) then
		return
	end
	eq.delete_data(invite_key(target_id))
	eq.cross_zone_message_player_by_name(CHAT_RED, i.inviter, string.format("Prospective guild member %s is not online.", eq.get_char_name_by_id(tonumber(target_id))))
end

-- ---------------------------------------------------------------- group invitations declined

-- A RoF2 client that declines a group invitation (or refuses one by itself: already invited, a raid) sends
-- OP_CancelInvite (raw 0x2a50, 152 bytes: char inviter[64]; char invitee[64]; 20 bytes; u32 reason). EQEmu's RoF2
-- table maps it to nothing (its OP_GroupCancelInvite is 0x0000 for RoF2), so the inviter is never told. This passes
-- it on: as the packet itself to an inviter in the same zone (the client prints its line: 12264-12266, 5086, 5088),
-- else as that line in text. The invitee's name is the sender's, whatever the packet says.
local OP_CANCEL_INVITE   = 0x2a50
local CANCEL_INVITE_SIZE = 152
local CHAT_GROUP_REFUSAL = 273 -- the colour the client prints the line in (0x111)
-- No EQEmu Lua call tells whether the inviter really invited the sender, so a client could name anyone, in any
-- zone, again and again: at most CANCEL_MAX a sender in CANCEL_SECONDS are passed on, ample for real declines.
local CANCEL_MAX, CANCEL_SECONDS = 5, 10
local cancel_limit = limiter(CANCEL_SECONDS)
local CANCEL_TEXTS = {
	"%s is currently considering joining another group.",
	"%s is already in another group.",
	"%s rejects your offer to join the group.",
	"%s is considering joining another raid.",
	"%s rejects your invite because they are in a raid and you are not in theirs, or they are a raid group leader",
}

local function on_cancel_invite(client, packet)
	if setting("enabled") == 0 or setting("group_declines") == 0 or packet:GetSize() < CANCEL_INVITE_SIZE then
		return
	end
	local inviter = packet:ReadString(0):sub(1, 63)
	local reason  = packet:ReadInt8(148) % 256
	local name    = client:GetCleanName()
	if inviter == "" or inviter:lower() == name:lower() or CANCEL_TEXTS[reason] == nil then
		return
	end
	local ok, first_over = allow(cancel_limit, client:CharacterID(), CANCEL_MAX)
	if not ok then
		if first_over then
			log(string.format("%s sends group declines too fast; the rest of %d s are dropped", name, CANCEL_SECONDS))
		end
		return
	end
	local target = eq.get_entity_list():GetClientByName(inviter)
	if target.valid then
		local p = Packet(OP_CANCEL_INVITE, CANCEL_INVITE_SIZE, true)
		p:WriteString(0, target:GetCleanName())
		p:WriteString(64, name)
		p:WriteInt32(148, reason)
		target:QueuePacket(p)
	else
		eq.cross_zone_message_player_by_name(CHAT_GROUP_REFUSAL, inviter, string.format(CANCEL_TEXTS[reason], name))
	end
end

-- ---------------------------------------------------------------- the server's client files

local OP_FILES_REQUEST = 0x1434
local OP_FILES         = 0x1435
local FILES_MAGIC      = 0x4c465348 -- "HSFL"
local FILES_DIR        = "client_files" -- the zones' working folder is the server folder
local FILES_PAGE       = 64             -- list entries a packet
local FILES_CHUNK_MAX  = 32768
local FILES_RESCAN_MIN = 30             -- a client's request never makes the list again sooner
local FILES_OPEN_MAX   = 8              -- files kept open for the pieces clients ask for
local FILES_WAIT_MAX   = 64             -- piece requests over the limit held per client (twice the client's window)
local FILES_TIMER      = "hailstone:files"
local FILES_TIMER_MS   = 100            -- how often held requests are looked at
local POLICY_FILENAME  = "hailstone.cfg"
local POLICY_REVISION  = "; hailstone-policy-revision = "

local files       = { list = {}, id = 0, built = nil }
local open_files  = {} -- path -> { handle, used }: several clients may be fetching different files at once
local open_count  = 0
local open_tick   = 0
local chunk_limit = limiter(1)
local zone_chunk_limit = limiter(1) -- one id, 0: the whole zone
-- Piece requests over the limits (take_piece), by character id: { first, last, keys, [n] = {id, index, offset,
-- length} }, served in order from the FILES_TIMER timer as the limit allows.
local waiting     = {}
local waiting_order = nil -- waiting's character ids, sorted; nil once a character joins or leaves it
local request_limit = limiter(1) -- piece requests a character makes, counted before any work is done
local files_timer = false
local files_empty_logged = false -- build_client_files logged an empty list
local windows   = package.config:sub(1, 1) == "\\"

-- A name for this process's scratch files in client_files/ (zones may hash at the same time): the zone and instance
-- it runs (no two processes run the same one at once), the time and a table's address. Made at first use, once the
-- zone is up.
local scratch_value = nil
local function scratch()
	if scratch_value == nil then
		local zone = eq.get_zone_short_name and tostring(eq.get_zone_short_name() or "") or ""
		local instance = eq.get_zone_instance_id and tonumber(eq.get_zone_instance_id()) or 0
		scratch_value = string.format("%s-%d-%d-%s", zone:gsub("[^%w_]", ""), instance or 0, os.time(),
			tostring({}):match("(%x+)$") or "0")
	end
	return scratch_value
end

-- Both write UTF-8: PowerShell's pipe is in the console's OEM code page otherwise, which turned a name outside ASCII
-- into bytes neither PS_HASH (reading the list as UTF-8) nor the client could use.
local PS_LIST = [[
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
$root = (Resolve-Path -LiteralPath 'client_files').Path.TrimEnd('\')
Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object {
	$rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
	if ($rel -notmatch '(^|/)\.') { "{0}`t{1}`t{2}" -f $rel, $_.Length, $_.LastWriteTimeUtc.Ticks }
}
]]

local PS_HASH = [[
param($list)
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
$root = (Resolve-Path -LiteralPath 'client_files').Path.TrimEnd('\')
Get-Content -LiteralPath (Join-Path $root $list) -Encoding UTF8 | ForEach-Object {
	$h = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $root $_)
	"{0}  {1}" -f $h.Hash.ToLower(), $_
}
Remove-Item -LiteralPath (Join-Path $root $list)
]]

local function sh_quote(s)
	return "'" .. (s:gsub("'", "'\\''")) .. "'"
end

local function run(cmd)
	local p = io.popen(cmd)
	if not p then
		return nil
	end
	local out = p:read("*a")
	p:close()
	return out
end

local function write_file(path, text)
	local f = io.open(path, "wb")
	if not f then
		return false
	end
	f:write(text)
	f:close()
	return true
end

-- The revision marker is part of the cfg's atomic replacement, so every zone sees the new revision with the
-- new file. Resolve the name from the server's list to preserve existing case variants on case-sensitive disks.
local function policy_filename(list)
	local name
	for _, entry in ipairs(list) do
		if entry.path:lower() == POLICY_FILENAME then
			if name then return nil end -- ambiguous pair of case variants
			name = entry.path
		end
	end
	return name or POLICY_FILENAME
end

local function policy_revision(text)
	if text:sub(1, #POLICY_REVISION) ~= POLICY_REVISION then return nil end
	return text:sub(#POLICY_REVISION + 1):match("^([%w_-]+)\r?\n")
end

local function peek_policy_revision(name)
	if not name then return nil end
	local file = io.open(FILES_DIR .. "/" .. name, "rb")
	if not file then return nil end
	local prefix = file:read(200) or ""
	file:close()
	return policy_revision(prefix) or "" -- distinguish a legacy cfg from a missing file
end

-- EQEmu's Lua has no os.remove or os.rename (SANITIZE_LUA_LIBS), so the shell removes and renames: the
-- scripts delete themselves and their lists.
local function powershell(script, args)
	local name = ".hailstone-" .. scratch() .. ".ps1"
	if not write_file(FILES_DIR .. "/" .. name, script .. "Remove-Item -LiteralPath $PSCommandPath\n") then
		return nil
	end
	return run('powershell -NoProfile -ExecutionPolicy Bypass -File "' .. FILES_DIR .. '\\' .. name .. '" ' .. (args or ""))
end

local function move_file(from, to)
	if windows then
		run('move /Y "' .. from:gsub("/", "\\") .. '" "' .. to:gsub("/", "\\") .. '" >NUL')
	else
		run("mv -f " .. sh_quote(from) .. " " .. sh_quote(to))
	end
end

-- {path, size, mtime} of every file, sorted by path; {} without the folder. On Windows a name outside ASCII is left
-- out: Lua's io.open takes the name in the ANSI code page, not UTF-8, so the zone could list it but not read it.
local function list_client_files()
	local out
	if windows then
		out = powershell(PS_LIST)
	else
		-- -H: a client_files that is itself a link to a folder (on another disk) is followed, as PowerShell and
		-- hailstone_check do; without it find lists nothing there, and the clients get no hailstone.cfg.
		out = run("find -H " .. sh_quote(FILES_DIR) .. " -type f ! -path '*/.*' -printf '%P\\t%s\\t%T@\\n' 2>/dev/null")
	end
	local list = {}
	for line in (out or ""):gmatch("[^\r\n]+") do
		local path, size, mtime = line:match("^(.-)\t(%d+)\t(%S+)$")
		if path and windows and path:find("[\128-\255]") then
			log("client_files: " .. path .. " is not sent: a name outside ASCII cannot be opened on Windows")
		elseif path then
			list[#list + 1] = { path = path, size = tonumber(size), mtime = mtime }
		end
	end
	table.sort(list, function(a, b) return a.path < b.path end)
	return list
end

-- path -> lower-case hex SHA-256 for the paths given.
local function hash_client_files(paths)
	local hashes = {}
	local name   = ".hailstone-hash-" .. scratch()
	if #paths == 0 or not write_file(FILES_DIR .. "/" .. name, table.concat(paths, "\n") .. "\n") then
		return hashes
	end
	local out
	if windows then
		out = powershell(PS_HASH, name)
	else
		out = run("cd " .. sh_quote(FILES_DIR) .. " && xargs -d '\\n' -a " .. name .. " sha256sum -- 2>/dev/null; rm -f " .. name)
	end
	for line in (out or ""):gmatch("[^\r\n]+") do
		-- GNU sha256sum starts the line of a name holding a backslash or a line break with a backslash, and writes
		-- those as \\, \n (and \r in newer coreutils) in the name.
		local escaped, hex, path = line:match("^(\\?)(%x+)  (.+)$")
		if hex and #hex == 64 then
			if escaped ~= "" then
				path = path:gsub("\\(.)", { ["\\"] = "\\", n = "\n", r = "\r" })
			end
			hashes[path] = hex:lower()
		end
	end
	return hashes
end

local function read_hash_cache()
	local cache = {}
	local f = io.open(FILES_DIR .. "/.hailstone-cache", "rb")
	if not f then
		return cache
	end
	for line in f:lines() do
		local path, size, mtime, hex, extra = line:match("^(.-)\t(%d+)\t(%S+)\t(%x+)(.*)$")
		if path then
			cache[path] = { size = tonumber(size), mtime = mtime, sha = hex,
				policy_revision = extra:match("^\t([%w_-]+)$") or "" }
		end
	end
	f:close()
	return cache
end

local function close_open_file(path)
	local o = open_files[path]
	if o then
		o.handle:close()
		open_files[path] = nil
		open_count = open_count - 1
	end
end

local function close_open_files()
	for _, o in pairs(open_files) do
		o.handle:close()
	end
	open_files, open_count = {}, 0
end

-- The file's handle, opened once and kept among the FILES_OPEN_MAX used last; nil if it cannot be opened.
local function open_client_file(path)
	open_tick = open_tick + 1
	local o = open_files[path]
	if o == nil then
		if open_count >= FILES_OPEN_MAX then
			local oldest
			for p, v in pairs(open_files) do
				if oldest == nil or v.used < open_files[oldest].used then
					oldest = p
				end
			end
			close_open_file(oldest)
		end
		local handle = io.open(FILES_DIR .. "/" .. path, "rb")
		if handle == nil then
			return nil
		end
		o = { handle = handle }
		open_files[path] = o
		open_count = open_count + 1
	end
	o.used = open_tick
	return o.handle
end

-- The file's size and time as list_client_files gives them (time nil where that would cost a PowerShell start per
-- file: on Windows only the size is checked); nil when it is gone.
local function stat_client_file(path)
	if windows then
		local f = io.open(FILES_DIR .. "/" .. path, "rb")
		if not f then
			return nil
		end
		local size = f:seek("end")
		f:close()
		return size, nil
	end
	local out = run("find " .. sh_quote(FILES_DIR .. "/" .. path) .. " -maxdepth 0 -type f -printf '%s\\t%T@\\n' 2>/dev/null")
	local size, mtime = (out or ""):match("^(%d+)\t(%S+)")
	if not size then
		return nil
	end
	return tonumber(size), mtime
end

-- One zone hashes at a time. Every zone process makes the list when it starts, so a server starting its zones at once
-- had each hash the same new files side by side. A zone with files to hash first takes client_files/.hailstone-lock
-- (a folder: making one is atomic on every system), writing the time and its scratch name in it; another finding it
-- held waits, a second at a time, then reads the cache the first one wrote and hashes only what is still missing. A
-- lock older than LOCK_STALE seconds (or one with no owner for LOCK_OWNERLESS tries) is a zone's that stopped, and
-- is removed. After LOCK_WAIT tries a zone hashes anyway, as all did before the lock: two zones taking a stale lock
-- at the same moment can still both hash, which costs time and nothing else (each writes the cache whole and moves
-- it into place). A zone that has a list already and was not forced keeps it while another hashes, and looks again
-- at the next request after files_rescan.
local LOCK_DIR        = FILES_DIR .. "/.hailstone-lock"
local LOCK_STALE      = 900 -- seconds
local LOCK_WAIT       = 300 -- tries, a second apart
local LOCK_OWNERLESS  = 10  -- tries

local function take_lock()
	local out
	if windows then
		out = run('mkdir "' .. LOCK_DIR:gsub("/", "\\") .. '" 2>NUL && echo taken')
	else
		out = run("mkdir " .. sh_quote(LOCK_DIR) .. " 2>/dev/null && echo taken")
	end
	if not (out or ""):find("taken", 1, true) then
		return false
	end
	write_file(LOCK_DIR .. "/owner", os.time() .. " " .. scratch() .. "\n")
	return true
end

-- The lock's time and owner, nil when it has none (not held, or held a moment before its owner wrote).
local function lock_owner()
	local f = io.open(LOCK_DIR .. "/owner", "rb")
	if not f then
		return nil
	end
	local t, who = (f:read("*a") or ""):match("^(%d+) (%S+)")
	f:close()
	return tonumber(t), who
end

local function remove_lock()
	if windows then
		run('rmdir /S /Q "' .. LOCK_DIR:gsub("/", "\\") .. '" 2>NUL')
	else
		run("rm -rf " .. sh_quote(LOCK_DIR))
	end
end

local function release_lock()
	local _, who = lock_owner()
	if who == scratch() then
		remove_lock()
	end
end

local function pause_a_second()
	if windows then
		run("ping -n 2 127.0.0.1 >NUL")
	else
		run("sleep 1")
	end
end

-- True when this zone took the lock. False when it gave up waiting (or `wait` is false and another zone holds it).
local function acquire_lock(wait)
	local ownerless, broke = 0, false
	for _ = 1, LOCK_WAIT do
		if take_lock() then
			return true
		end
		local t, who = lock_owner()
		if t == nil and broke then
			return false -- the lock cannot be made here at all: hash without it
		end
		ownerless = t == nil and ownerless + 1 or 0
		if (t ~= nil and os.time() - t >= LOCK_STALE) or ownerless >= LOCK_OWNERLESS then
			log("client_files: removing a hashing lock left by " .. (who or "a zone that wrote no owner"))
			remove_lock()
			broke, ownerless = true, 0
		elseif not wait then
			return false
		else
			pause_a_second()
		end
	end
	log("client_files: another zone is still hashing; this one hashes too")
	return false
end

-- The list, made again when `force` (the zone's start, #reloadquest) or older than files_rescan seconds. It runs
-- find and sha256sum and blocks the zone meanwhile, so a client's request can make it at most that often. Its id
-- changes whenever a file does.
local function build_client_files(force)
	local keep = math.max(setting("files_rescan") or FILES_RESCAN_MIN, FILES_RESCAN_MIN)
	local policy_changed = peek_policy_revision(policy_filename(files.list)) ~= files.policy_revision
	if not force and not policy_changed and files.built and os.time() - files.built < keep then
		return
	end
	local list  = list_client_files()
	local listed_policy = policy_filename(list)
	local listed_revision = peek_policy_revision(listed_policy)
	-- A missing folder and one that cannot be listed look the same from here; either way say so, once until files
	-- are listed again, so that clients sent no hailstone.cfg do not go unnoticed.
	if #list == 0 then
		if not files_empty_logged then
			log("client_files: no files listed (the folder is missing, empty or cannot be read); clients get an empty list")
			files_empty_logged = true
		end
	else
		files_empty_logged = false
	end
	local need, locked
	local function from_cache()
		local cache = read_hash_cache()
		need = {}
		for _, e in ipairs(list) do
			local c = cache[e.path]
			if c and c.size == e.size and c.mtime == e.mtime and (e.path ~= listed_policy or c.policy_revision == listed_revision) then
				e.sha = c.sha
			elseif not e.sha then
				need[#need + 1] = e.path
			end
		end
	end
	from_cache()
	if #need > 0 then
		locked = acquire_lock(force or files.built == nil)
		if not locked and not force and files.built ~= nil then
			local _, who = lock_owner()
			if who ~= nil then
				files.built = os.time() -- the different policy revision will still make the next request look
				return
			end
		end
		from_cache() -- what another zone hashed meanwhile
	end
	if #need > 0 then
		local hashes = hash_client_files(need)
		for _, e in ipairs(list) do
			e.sha = e.sha or hashes[e.path]
		end
	end
	-- A save may occur while this zone scans. Leave this list and cache alone if the cfg changed during hashing.
	if listed_revision ~= peek_policy_revision(listed_policy) then
		if locked then release_lock() end
		return
	end
	local kept, lines, id = {}, {}, 0
	for _, e in ipairs(list) do
		if e.sha and e.size < 2147483648 then
			kept[#kept + 1]   = e
			local revision = e.path == listed_policy and listed_revision or ""
			lines[#lines + 1] = e.path .. "\t" .. e.size .. "\t" .. e.mtime .. "\t" .. e.sha
				.. (revision ~= "" and "\t" .. revision or "")
			local s = e.path .. e.sha
			for i = 1, #s do
				id = (id * 31 + s:byte(i)) % 2147483629
			end
		else
			log("client_files: could not hash " .. e.path)
		end
	end
	if #need > 0 or #kept ~= #list then
		local tmp = FILES_DIR .. "/.hailstone-cache-" .. scratch()
		if write_file(tmp, table.concat(lines, "\n") .. "\n") then
			move_file(tmp, FILES_DIR .. "/.hailstone-cache")
		end
		log(string.format("client_files: %d files, %d hashed", #kept, #need))
	end
	if locked then
		release_lock()
	end
	if id == 0 and #kept > 0 then
		id = 1
	end
	if id ~= files.id then
		close_open_files()
	end
	files = { list = kept, id = id, built = os.time(), policy_revision = listed_revision }
end

local function files_header(p, action)
	p:WriteInt32(0, FILES_MAGIC)
	p:WriteInt16(4, 1)
	p:WriteInt16(6, action)
end

local function send_files_page(client, first)
	local list = files.list
	if setting("client_files") == 0 then
		list = {}
	end
	local count = clamp(#list - first, 0, FILES_PAGE)
	local size  = 24
	for i = first + 1, first + count do
		size = size + 38 + #list[i].path
	end
	local p = Packet(reply_op(client, OP_FILES), size, true)
	files_header(p, 1)
	p:WriteInt32(8, files.id)
	p:WriteInt32(12, #list)
	p:WriteInt32(16, first)
	p:WriteInt32(20, count)
	local pos = 24
	for i = first + 1, first + count do
		local e = list[i]
		p:WriteInt32(pos, e.size)
		for j = 0, 31 do
			p:WriteInt8(pos + 4 + j, tonumber(e.sha:sub(j * 2 + 1, j * 2 + 2), 16))
		end
		p:WriteInt16(pos + 36, #e.path)
		p:WriteFixedLengthString(pos + 38, e.path, #e.path)
		pos = pos + 38 + #e.path
	end
	client:QueuePacket(p)
end

local function send_files_error(client, id, index, code)
	local p = Packet(reply_op(client, OP_FILES), 20, true)
	files_header(p, 3)
	p:WriteInt32(8, id)
	p:WriteInt32(12, index)
	p:WriteInt32(16, code)
	client:QueuePacket(p)
end

local function send_files_chunk(client, id, index, offset, length)
	local e = files.list[index + 1]
	if id ~= files.id or e == nil or setting("client_files") == 0 then
		send_files_error(client, id, index, 1) -- the list changed: the client lists again
		return
	end
	local is_policy = e.path:lower() == POLICY_FILENAME
	-- Ambiguous case variants are ineligible for management, but retain the ordinary file delivery behavior.
	if is_policy and files.policy_revision ~= nil and peek_policy_revision(e.path) ~= files.policy_revision then
		build_client_files(false)
		send_files_error(client, id, index, 1)
		return
	end
	length = clamp(length, 0, FILES_CHUNK_MAX)
	if offset < 0 or offset >= e.size then
		length = 0
	elseif offset + length > e.size then
		length = e.size - offset
	end
	-- A file rewritten since it was hashed would arrive with bytes that do not match its hash: at a file's first
	-- piece the zone looks at its size and time, and at every piece at the size of what it has open. A change
	-- makes the list again (the hashing is at most every FILES_RESCAN_MIN seconds) and tells the client, which
	-- lists again. The time is looked at once per file until the list is next made (files_rescan), for all players
	-- (it costs a process): a client asking for the first piece again and again starts no more, and a rewrite of
	-- the same size after that waits for the next list. A file found changed stays changed until then, so a client
	-- asking again before the list is made again (within FILES_RESCAN_MIN) is told again, not sent the new bytes.
	local f = open_client_file(e.path)
	local changed = e.changed or (f ~= nil and f:seek("end") ~= e.size)
	if not changed and offset == 0 and not e.statted then
		e.statted = true
		local size, mtime = stat_client_file(e.path)
		changed = size ~= e.size or (mtime ~= nil and mtime ~= e.mtime)
	end
	if changed then
		if not e.changed then
			log("client_files: " .. e.path .. " changed since it was hashed")
		end
		e.changed = true
		close_open_file(e.path)
		if os.time() - (files.built or 0) >= FILES_RESCAN_MIN then
			build_client_files(true)
		end
		send_files_error(client, id, index, 1)
		return
	end
	local data = ""
	if f and length > 0 then
		f:seek("set", offset)
		data = f:read(length) or ""
	end
	if is_policy then close_open_file(e.path) end -- Windows cannot replace a cfg held open by another zone
	if not f or #data ~= length then
		close_open_file(e.path)
		log("client_files: could not read " .. e.path)
		send_files_error(client, id, index, 2)
		return
	end
	local p = Packet(reply_op(client, OP_FILES), 24 + length, true)
	files_header(p, 2)
	p:WriteInt32(8, id)
	p:WriteInt32(12, index)
	p:WriteInt32(16, offset)
	p:WriteInt32(20, length)
	if length > 0 then
		p:WriteFixedLengthString(24, data, length)
	end
	client:QueuePacket(p)
end

-- Counts one piece for `char_id` and for the zone if both limits have room for it (nothing counted otherwise).
-- The zone cannot see what a client has received: EQEmu keeps every piece it sent until the client acknowledges it
-- (or 30 s pass and the link is closed), so a client that asks and never acknowledges makes the zone hold what the
-- limits let through in 30 s. The zone's limit bounds that whatever the number of characters (docs/unknowns.md).
local function take_piece(char_id)
	local limit = setting("files_chunks_per_second") or 0
	local zone  = setting("files_chunks_per_second_zone") or 0
	local r = limit > 0 and window(chunk_limit, char_id) or nil
	local z = zone > 0 and window(zone_chunk_limit, 0) or nil
	if z and z.count >= zone then
		return false, "zone"
	end
	if r and r.count >= limit then
		return false, "own"
	end
	if r then r.count = r.count + 1 end
	if z then z.count = z.count + 1 end
	return true
end

-- Holds a request over the limit. A request already held (the client asked again) is not held twice; past
-- FILES_WAIT_MAX it is dropped, and the client asks again for a piece that does not come.
local function hold_request(client, char_id, req)
	local q = waiting[char_id]
	if q == nil then
		q = { first = 1, last = 0, keys = {} }
		waiting[char_id] = q
		waiting_order = nil
	end
	local key = req[1] .. ":" .. req[2] .. ":" .. req[3]
	if q.keys[key] then
		return
	end
	if q.last - q.first + 1 >= FILES_WAIT_MAX then
		if not q.full then
			log(string.format("%s asks for file pieces faster than it is sent them; the excess is dropped", client:GetCleanName()))
			q.full = true
		end
		return
	end
	q.last = q.last + 1
	q[q.last] = req
	q.keys[key] = true
	if not files_timer then
		eq.set_timer(FILES_TIMER, FILES_TIMER_MS, encounter)
		files_timer = true
	end
end

-- What the limits allow of each client's held requests, each client's in order: one piece a client in turn, the
-- turn starting after the client served last, so that under the zone's limit no client is always last in line and
-- left with nothing. Run by the FILES_TIMER timer and at every piece request while any wait. A client that left
-- loses its own; the timer stops when none are held.
local served_last = nil -- the character id served last

local function serve_waiting()
	local list  = eq.get_entity_list()
	if waiting_order == nil then
		waiting_order = {}
		for char_id in pairs(waiting) do
			waiting_order[#waiting_order + 1] = char_id
		end
		table.sort(waiting_order)
	end
	local order = waiting_order
	local start = 1
	if served_last ~= nil then
		for k = 1, #order do
			if order[k] > served_last then
				start = k
				break
			end
		end
	end
	local turn, clients = {}, {}
	for k = 0, #order - 1 do
		local char_id = order[(start - 1 + k) % #order + 1]
		local client = list:GetClientByCharID(char_id)
		if client == nil or not client.valid then
			waiting[char_id] = nil
			waiting_order = nil
		else
			turn[#turn + 1] = char_id
			clients[char_id] = client
		end
	end
	local zone_full = false
	while #turn > 0 and not zone_full do
		local again = {}
		for _, char_id in ipairs(turn) do
			local ok, why = take_piece(char_id)
			if ok then
				local q = waiting[char_id]
				local req = q[q.first]
				q[q.first] = nil
				q.first = q.first + 1
				q.keys[req[1] .. ":" .. req[2] .. ":" .. req[3]] = nil
				send_files_chunk(clients[char_id], req[1], req[2], req[3], req[4])
				served_last = char_id
				if q.first > q.last then
					waiting[char_id] = nil
					waiting_order = nil
				else
					again[#again + 1] = char_id
				end
			elseif why == "zone" then
				zone_full = true
				break
			end
		end
		turn = again
	end
	if next(waiting) == nil and files_timer then
		eq.stop_timer(FILES_TIMER, encounter)
		files_timer = false
	end
end

-- The encounter's timers: the guild invitations' (above) and FILES_TIMER.
local guild_timer = event_timer
function event_timer(e)
	if e.timer == FILES_TIMER then
		serve_waiting()
		return
	end
	guild_timer(e)
end

-- A client lists once a login, page after page, and again if the list changes while it comes (at most 3 times):
-- FILES_LISTS whole lists a character in FILES_LIST_SECONDS are answered, more are dropped. A client never asks
-- again for a page that does not come, so the limit is well above what one honest client asks for.
local FILES_LISTS, FILES_LIST_SECONDS = 8, 60
local list_limit = limiter(FILES_LIST_SECONDS)

-- Whether the zone's limit has room for a piece this second (take_piece's zone half, counting nothing).
local function zone_has_room()
	local zone = setting("files_chunks_per_second_zone") or 0
	return zone <= 0 or window(zone_chunk_limit, 0).count < zone
end

-- Piece requests a character may make in a second before any work is done for them: twice what the limits let it
-- be sent, plus its held ones. An honest client asks once for each piece that comes (and again for one lost), so it
-- never comes near this; a flood of requests is dropped here instead of rescanning the held ones at every packet.
local function files_requests_max()
	local own  = setting("files_chunks_per_second") or 0
	local zone = setting("files_chunks_per_second_zone") or 0
	return 2 * math.max(own, zone, 1024) + FILES_WAIT_MAX
end

local function on_files_request(client, packet)
	-- enabled 0: not answered at all, as the other requests (a client then has no list, and keeps its own files)
	if setting("enabled") == 0 or packet:GetSize() < 12 or packet:ReadInt32(0) ~= FILES_MAGIC then
		return
	end
	local action = packet:ReadInt16(6)
	if action == 1 then
		local pages = math.floor(#files.list / FILES_PAGE) + 1
		local ok, first_over = allow(list_limit, client:CharacterID(), FILES_LISTS * pages)
		if not ok then
			if first_over then
				log(string.format("%s asks for the file list too often; the rest of %d s are dropped", client:GetCleanName(), FILES_LIST_SECONDS))
			end
			return
		end
		local first = math.max(0, packet:ReadInt32(8))
		if first == 0 and setting("client_files") ~= 0 then
			build_client_files(false)
		end
		send_files_page(client, first)
	elseif action == 2 and packet:GetSize() >= 24 then
		-- A client asks for at most 32 pieces at a time (its window), each when one comes. Beyond the limits a
		-- request waits its turn (serve_waiting), behind any of its own that already wait, so the pieces go in the
		-- order asked. While any client's requests wait, every request joins them and they are served in turn at
		-- once, so a client asking at the start of a second cannot take the zone's whole limit ahead of those held.
		local char_id = client:CharacterID()
		local ok, first_over = allow(request_limit, char_id, files_requests_max())
		if not ok then
			if first_over then
				log(string.format("%s asks for file pieces too often; the rest of this second's requests are dropped", client:GetCleanName()))
			end
			return
		end
		local req = { packet:ReadInt32(8), packet:ReadInt32(12), packet:ReadInt32(16), packet:ReadInt32(20) }
		local others = next(waiting) ~= nil
		if others or not take_piece(char_id) then
			hold_request(client, char_id, req)
			-- Nothing can go while the zone's limit is spent this second: the timer serves them once it has room.
			if others and zone_has_room() then
				serve_waiting()
			end
			return
		end
		send_files_chunk(client, req[1], req[2], req[3], req[4])
	end
end

-- ---------------------------------------------------------------- the Find window's quest NPCs

local OP_QUEST_REQUEST = 0x1436
local OP_QUEST_NPCS    = 0x1437
local QUEST_MAGIC      = 0x4e515348 -- "HSQN"
local QUEST_FLAG_ITEMS, QUEST_FLAG_TALKS = 1, 2
local QUEST_MAX        = 4000 -- names in a reply (the count is a u16; a zone has a few hundred)
local QUEST_FILE_MAX   = 1048576 -- bytes of a quest file read for its handlers

local quest_list = nil -- { { name, flags }, ... } once made: kept for the zone process

-- quest_paths from eqemu_config.json (beside which the zone runs), else its "quests" folder, else "quests".
local function quest_dirs()
	local dirs = {}
	local f = io.open("eqemu_config.json", "rb")
	local text = f and f:read(262144) or ""
	if f then f:close() end
	local list = text:match('"quest_paths"%s*:%s*(%b[])')
	if list then
		for d in list:gmatch('"([^"]*)"') do dirs[#dirs + 1] = d end
	end
	if #dirs == 0 then dirs[1] = text:match('"quests"%s*:%s*"([^"]*)"') or "quests" end
	for i, d in ipairs(dirs) do dirs[i] = d:gsub("[/\\]+$", "") end
	return dirs
end

-- `flags` with `bit` set (EQEmu's Lua is 5.1: no bit operators).
local function with_flag(flags, bit)
	if math.floor(flags / bit) % 2 == 1 then return flags end
	return flags + bit
end

-- The handlers a quest file has (QUEST_FLAG_*), each file read once.
local quest_file_flags = {}
local function file_flags(path)
	local known = quest_file_flags[path]
	if known ~= nil then return known end
	local flags = 0
	local f = io.open(path, "rb")
	if f then
		local text = f:read(QUEST_FILE_MAX) or ""
		f:close()
		if text:find("sub%s+EVENT_ITEM") or text:find("function%s+event_trade") or text:find("event_trade%s*=%s*function") then
			flags = with_flag(flags, QUEST_FLAG_ITEMS)
		end
		if text:find("sub%s+EVENT_SAY") or text:find("function%s+event_say") or text:find("event_say%s*=%s*function") then
			flags = with_flag(flags, QUEST_FLAG_TALKS)
		end
	else
		flags = false -- no such file
	end
	quest_file_flags[path] = flags
	return flags
end

-- The NPC's quest file as EQEmu looks for it (QuestParserCollection::GetQIByNPCQuest, without the default scripts),
-- and its handlers; 0 without one.
local function npc_flags(dirs, zone, version, id, name)
	name = name:gsub("`", "-")
	local bases = { tostring(id), name, name .. "_" .. id }
	for _, dir in ipairs(dirs) do
		for _, folder in ipairs({ dir .. "/" .. zone .. "/v" .. version, dir .. "/" .. zone, dir .. "/global" }) do
			for _, base in ipairs(bases) do
				for _, ext in ipairs({ ".lua", ".pl" }) do
					local flags = file_flags(folder .. "/" .. base .. ext)
					if flags then return flags end
				end
			end
		end
	end
	return 0
end

local function build_quest_list()
	local started = os.clock()
	local zone    = (eq.get_zone_short_name() or ""):gsub("[^%w_]", "")
	local version = tonumber(eq.get_zone_instance_version()) or 0
	-- the NPCs: the spawn tables' for this zone and version, and those alive now
	local npcs, seen = {}, {}
	local function add(id, name)
		id = tonumber(id)
		if id and name and name ~= "" and not seen[id] then
			seen[id] = true
			npcs[#npcs + 1] = { id = id, name = name }
		end
	end
	if Database then
		for _, r in ipairs(rows(string.format(
			"SELECT DISTINCT nt.`id`, nt.`name` FROM `spawn2` s JOIN `spawnentry` se ON se.`spawngroupID` = s.`spawngroupID` " ..
			"JOIN `npc_types` nt ON nt.`id` = se.`npcID` WHERE s.`zone` = '%s' AND (s.`version` = %d OR s.`version` = -1)",
			zone, version))) do
			add(r.id, r.name)
		end
	end
	local list = eq.get_entity_list():GetNPCList()
	for npc in (list and list.entries or function() end) do
		local name = (npc:GetName() or ""):gsub("%d+$", "") -- the spawn's name with its number
		add(npc:GetNPCTypeID(), name)
	end
	local dirs, out, names = quest_dirs(), {}, {}
	for _, n in ipairs(npcs) do
		local flags = npc_flags(dirs, zone, version, n.id, n.name)
		if flags ~= 0 and #out < QUEST_MAX then
			local key = n.name:sub(1, 64)
			if names[key] then
				if math.floor(flags / QUEST_FLAG_ITEMS) % 2 == 1 then names[key].flags = with_flag(names[key].flags, QUEST_FLAG_ITEMS) end
				if math.floor(flags / QUEST_FLAG_TALKS) % 2 == 1 then names[key].flags = with_flag(names[key].flags, QUEST_FLAG_TALKS) end
			else
				names[key] = { name = key, flags = flags }
				out[#out + 1] = names[key]
			end
		end
	end
	log(string.format("quest NPCs of %s: %d of %d NPCs, %.0f ms", zone, #out, #npcs, (os.clock() - started) * 1000))
	return out
end

-- A client asks once a zone (and again after it zones back): QUEST_REQUESTS a character in QUEST_SECONDS.
local QUEST_REQUESTS, QUEST_SECONDS = 3, 10
local quest_limit = limiter(QUEST_SECONDS)

local function on_quest_request(client, packet)
	if setting("enabled") == 0 or setting("quest_npcs") == 0 then
		return
	end
	if not allow(quest_limit, client:CharacterID(), QUEST_REQUESTS) then
		return
	end
	if packet:GetSize() < 6 or packet:ReadInt32(0) ~= QUEST_MAGIC then
		return
	end
	quest_list = quest_list or build_quest_list()
	local size = 8
	for _, q in ipairs(quest_list) do size = size + 2 + #q.name end
	local p = Packet(OP_QUEST_NPCS, size, true)
	p:WriteInt32(0, QUEST_MAGIC)
	p:WriteInt16(4, 1)
	p:WriteInt16(6, #quest_list)
	local pos = 8
	for _, q in ipairs(quest_list) do
		p:WriteInt8(pos, q.flags)
		p:WriteInt8(pos + 1, #q.name)
		p:WriteFixedLengthString(pos + 2, q.name, #q.name)
		pos = pos + 2 + #q.name
	end
	client:QueuePacket(p)
end

-- ---------------------------------------------------------------- the zone links (the map's zone finder)

local OP_ZONE_LINKS_REQUEST = 0x1438
local OP_ZONE_LINKS         = 0x1439
local ZONE_LINKS_MAGIC      = 0x4c5a5348
local ZONE_LINKS_MAX, ZONE_LINKS_SECONDS = 2, 60
local zone_links_limit = limiter(ZONE_LINKS_SECONDS)
local zone_links       = nil -- { from1, to1, from2, to2, ... } once built
local zone_list        = nil -- { zone id, ... } the zones allowed; nil for a schema without the content columns
local zone_expansion   = -1  -- Expansion:CurrentExpansion as the rule says it (-1 all)

-- A comma-separated flag list that names one of `set`'s flags.
local function names_flag(list, set)
	for flag in tostring(list or ""):gmatch("[^,]+") do
		if set[flag] then
			return true
		end
	end
	return false
end

-- EQEmu's ContentFilterCriteria::apply, for one row: the expansion range (-1: any) holds the server's current
-- expansion (Expansion:CurrentExpansion, -1 all: 99), a row with content_flags needs one of them enabled, and one
-- with content_flags_disabled one of those disabled.
local function content_allows(row, expansion, enabled, disabled)
	local min, max = tonumber(row.min_expansion) or -1, tonumber(row.max_expansion) or -1
	if min ~= -1 and min > expansion then return false end
	if max ~= -1 and max < expansion then return false end
	if (row.content_flags or "") ~= "" and not names_flag(row.content_flags, enabled) then return false end
	if (row.content_flags_disabled or "") ~= "" and not names_flag(row.content_flags_disabled, disabled) then return false end
	return true
end

local function build_zone_links()
	local expansion = tonumber(eq.get_rule("Expansion:CurrentExpansion")) or -1
	zone_expansion = expansion
	if expansion == -1 then
		expansion = 99
	end
	local enabled, disabled = {}, {}
	local fstmt = query(Database(), "SELECT `flag_name`, `enabled` FROM `content_flags`")
	if fstmt then
		local row = fstmt:fetch_hash()
		while row do
			if tonumber(row.enabled) == 1 then enabled[row.flag_name] = true else disabled[row.flag_name] = true end
			row = fstmt:fetch_hash()
		end
	end
	local ok, db = pcall(Database, Database.Content)
	if not ok or db == nil then
		db = Database()
	end
	local cols = function(a) return string.format("%s.`min_expansion`, %s.`max_expansion`, %s.`content_flags`, %s.`content_flags_disabled`", a, a, a, a) end
	-- The zone point, the zone it is in and the zone it leads to must each be allowed.
	local sql = "SELECT s.`zoneidnumber` AS from_id, p.`target_zone_id` AS to_id, " ..
		cols("p") .. ", " .. cols("s"):gsub("`(%w+)`", "`%1` AS s_%1") .. ", " .. cols("t"):gsub("`(%w+)`", "`%1` AS t_%1") ..
		" FROM `zone_points` p JOIN `zone` s ON s.`short_name` = p.`zone` AND s.`version` = 0" ..
		" JOIN `zone` t ON t.`zoneidnumber` = p.`target_zone_id` AND t.`version` = 0" ..
		" WHERE (p.`version` = 0 OR p.`version` = -1) AND p.`target_zone_id` > 0 AND s.`zoneidnumber` <> p.`target_zone_id`"
	local stmt = query(db, sql)
	local filtered = stmt ~= nil
	if not stmt then -- a schema without the content columns
		stmt = query(db, "SELECT s.`zoneidnumber` AS from_id, p.`target_zone_id` AS to_id FROM `zone_points` p" ..
			" JOIN `zone` s ON s.`short_name` = p.`zone` AND s.`version` = 0" ..
			" WHERE (p.`version` = 0 OR p.`version` = -1) AND p.`target_zone_id` > 0 AND s.`zoneidnumber` <> p.`target_zone_id`")
	end
	local seen, list = {}, {}
	if stmt then
		local row = stmt:fetch_hash()
		while row do
			local from, to = tonumber(row.from_id), tonumber(row.to_id)
			local allowed = from ~= nil and to ~= nil and from > 0 and from < 65536 and to < 65536
			if allowed and filtered then
				local function prefixed(prefix)
					return { min_expansion = row[prefix .. "min_expansion"], max_expansion = row[prefix .. "max_expansion"],
						content_flags = row[prefix .. "content_flags"], content_flags_disabled = row[prefix .. "content_flags_disabled"] }
				end
				allowed = content_allows(row, expansion, enabled, disabled)
					and content_allows(prefixed("s_"), expansion, enabled, disabled)
					and content_allows(prefixed("t_"), expansion, enabled, disabled)
			end
			local key = allowed and (from * 65536 + to)
			if allowed and not seen[key] then
				seen[key] = true
				list[#list + 1] = from
				list[#list + 1] = to
			end
			row = stmt:fetch_hash()
		end
	end
	-- The zones themselves (version 0), as allowed.
	zone_list = nil
	local zstmt = filtered and query(db, "SELECT z.`zoneidnumber` AS id, " .. cols("z") ..
		" FROM `zone` z WHERE z.`version` = 0 AND z.`zoneidnumber` > 0 AND z.`zoneidnumber` < 65536")
	if zstmt then
		zone_list = {}
		local zseen = {}
		local row = zstmt:fetch_hash()
		while row do
			local id = tonumber(row.id)
			if id and not zseen[id] and content_allows(row, expansion, enabled, disabled) then
				zseen[id] = true
				zone_list[#zone_list + 1] = id
			end
			row = zstmt:fetch_hash()
		end
	end
	log(string.format("zone links: %d, %s zones, for the map's zone finder", math.floor(#list / 2),
		zone_list and tostring(#zone_list) or "all"))
	return list
end

local function on_zone_links_request(client, packet)
	local ok, first_over = allow(zone_links_limit, client:CharacterID(), ZONE_LINKS_MAX)
	if not ok then
		if first_over then
			log(string.format("%s asks for the zone links too often; the rest of %d s are dropped", client:GetCleanName(), ZONE_LINKS_SECONDS))
		end
		return
	end
	if setting("enabled") == 0 or setting("zone_links") == 0 or Database == nil then
		return
	end
	if packet:GetSize() < 6 or packet:ReadInt32(0) ~= ZONE_LINKS_MAGIC then
		return
	end
	if zone_links == nil then
		zone_links = build_zone_links()
	end
	local count = math.floor(#zone_links / 2)
	local zones = zone_list and #zone_list or 0
	local p = Packet(OP_ZONE_LINKS, 12 + 4 * count + (zone_list and 6 + 2 * zones or 0), true)
	p:WriteInt32(0, ZONE_LINKS_MAGIC)
	p:WriteInt16(4, 1)
	p:WriteInt16(6, 0)
	p:WriteInt32(8, count)
	for i = 1, count do
		p:WriteInt16(12 + (i - 1) * 4, zone_links[2 * i - 1])
		p:WriteInt16(14 + (i - 1) * 4, zone_links[2 * i])
	end
	if zone_list then
		local at = 12 + 4 * count
		p:WriteInt16(at, zone_expansion)
		p:WriteInt32(at + 2, zones)
		for i = 1, zones do
			p:WriteInt16(at + 6 + (i - 1) * 2, zone_list[i])
		end
	end
	client:QueuePacket(p)
end

-- ----------------------------------------------------------------

-- Server Management uses a dedicated raw opcode so the stock EQEmu tables need no edits. Keep its private
-- state in a separate scope because an encounter and its locals share the zone process's Lua chunk.
local management = (function()
	local REQUEST, REPLY, MAGIC = 0x143a, 0x143b, 0x4d415348 -- "HSAM"
	local OPEN, SAVE = 1, 2
	local OK, DENIED, INVALID, STALE, STORAGE, BUSY = 0, 1, 2, 3, 4, 5
	local LIMIT, MAX_EDITS, MAX_NAME, MAX_VALUE = 16384, 64, 48, 8192
	local HISTORY = FILES_DIR .. "/.hailstone-admin-history"
	local EDIT_LOCK = FILES_DIR .. "/.hailstone-admin-lock"
	local EDIT_LOCK_OWNER = EDIT_LOCK .. "/owner"
	local EDIT_LOCK_STALE = 900 -- seconds
	local REVISION_TIME, REVISION_SEQUENCE = "%d%d%d%d%d%d%d%dT%d%d%d%d%d%dZ", "%d%d%d%d"
	local REVISION_ID = REVISION_TIME .. "%-" .. REVISION_SEQUENCE
	local MAX_SNAPSHOTS = 64
	local snapshots, token, last_use = {}, 0, 0
	local requests = limiter(10)
	-- Turning management on for a server without a cfg must preserve the permissions it already has.
	local NO_POLICY = "[scripts]\nplayer = on\n\n[plugins]\nplayer = on\n\n[socials]\nmode = full\ncaps = all\n"
	local schema = {
		client = { deny = "boolean" },
		scripts = { player = "boolean", player_caps = "caps", run = "scripts" },
		plugins = { player = "boolean", player_caps = "caps" },
		assets = { player = "boolean" },
		socials = { mode = "social_mode", caps = "caps", press_commands = "press_commands", faces = "boolean" },
		abilities = { autoskill = "boolean" },
		map = { vanilla = "boolean" },
		movement = { npc_collision = "boolean", passable_ladders = "boolean", solid_ladders = "boolean" },
		web = { allow = "hosts" },
		addons = { enforce = "boolean", default = "mode", suggest = "suggest", required = "required",
			allow = "repos", block = "repos" },
		commands = { default = "mode", allow = "commands", block = "commands", gm = "gm_commands", allow_groups = "groups",
			block_groups = "groups", blocked_message = "message" },
		management = { min_status = "min_status" },
	}

	local function trim(s)
		return (s:gsub("^[ \t\r\n]+", ""):gsub("[ \t\r\n]+$", ""))
	end

	local function unsigned(text, min, max)
		local n = text:match("^%d+$") and tonumber(text)
		return n and n >= min and n <= max
	end

	local function read(path, limit)
		local file, _, errno = io.open(path, "rb")
		if not file then return nil, errno == 2 end -- missing is different from unreadable
		local data, err = file:read(limit + 1)
		local closed = file:close()
		if err or not closed or (data and #data > limit) then return nil, false end
		return data or "", true
	end

	local function write(path, data)
		local file = io.open(path, "wb")
		if not file then return false end
		local written = file:write(data)
		local closed = file:close()
		return written and closed and read(path, #data) == data
	end

	local function history_file_exists(path)
		local file, _, errno = io.open(path, "rb")
		if file then
			file:close()
			return true
		end
		if errno == 2 then return false end
		return nil -- lack of access cannot be treated as an unused name
	end

	-- The existing write lock serializes revision selection across zones. The public marker contains only a
	-- timestamp and counter. The scratch process identifier is private to the file-service implementation.
	local function next_revision(previous)
		local timestamp = os.date("!%Y%m%dT%H%M%SZ", os.time())
		local first = 1
		if previous then
			local last_time, last_sequence = previous:match("^(" .. REVISION_TIME .. ")%-(" .. REVISION_SEQUENCE .. ")$")
			-- Retention can free lower sequence numbers. Leave the revision monotonic even if the clock retreats.
			if last_time and last_time >= timestamp then timestamp, first = last_time, tonumber(last_sequence) + 1 end
		end
		for sequence = first, 9999 do
			local name = string.format("%s-%04d", timestamp, sequence)
			local backup = HISTORY .. "/" .. name
			local cfg = history_file_exists(backup .. ".cfg")
			local metadata = history_file_exists(backup .. ".txt")
			if cfg == nil or metadata == nil then return nil end
			if not cfg and not metadata and name ~= previous then return name, backup end
		end
		return nil
	end

	local function section_name(line)
		if line:sub(1, 1) ~= "[" then return nil end
		local close = line:find("]", 2, true)
		return trim(line:sub(2, close and close - 1 or #line)):lower()
	end

	local function read_control(raw, section, key)
		local current, value = "", nil
		for original in raw:gmatch("[^\n]+") do
			local line = trim(original)
			local name = section_name(line)
			if name then current = name end
			if not name and current == section and not line:match("^[;#]") then
				local lhs, rhs = line:match("^([^=]+)=(.*)$")
				if lhs and trim(lhs):lower() == key then value = trim((rhs:gsub("[;#].*$", ""))) end
			end
		end
		return value
	end

	local function permitted(rank, raw)
		local minimum = read_control(raw or "", "management", "min_status")
		if minimum == nil then return rank >= 200 end
		return unsigned(minimum, 1, 32767) and rank >= tonumber(minimum)
	end

	local function account_status(client)
		local rank = client.Admin and client:Admin()
		if type(rank) ~= "number" or rank % 1 ~= 0 or rank > 32767 then return nil end
		return math.max(0, rank)
	end

	local function current_policy_filename()
		-- Open and Save do not trust the cached file list. A GM may have renamed the cfg or added another case
		-- variant since that zone scanned, and an editor must not create or choose a competing policy.
		local result
		if windows then
			result = run('if exist "' .. FILES_DIR .. '\\*" (dir /b /a:-d "' .. FILES_DIR .. '\\*" 2>NUL & echo listed)')
		else
			result = run("find -H " .. sh_quote(FILES_DIR) .. " -maxdepth 1 -iname hailstone.cfg -printf '%y %f\\n' 2>/dev/null && echo listed")
		end
		if not result or not result:match("listed[\r\n]+$") then return nil end
		local name
		for line in result:gmatch("[^\r\n]+") do
			local kind, entry = line:match("^(.) (.+)$")
			if windows then kind, entry = "f", line end
			if entry and entry:lower() == POLICY_FILENAME then
				if name or kind ~= "f" then return nil end
				name = entry
			end
		end
		return name or POLICY_FILENAME
	end

	local function load_cfg(fresh)
		local name
		if fresh then name = current_policy_filename()
		else name = policy_filename(files.list) end
		if not name then return nil, false end
		local raw, readable = read(FILES_DIR .. "/" .. name, LIMIT)
		if raw and raw:find("\0", 1, true) then return nil, false end
		return raw, readable, name
	end

	local function enabled()
		return setting("server_management") == 1 and setting("client_files") ~= 0
	end

	local function advertised(rank)
		if rank < 1 or not enabled() then return false end
		local raw, readable = load_cfg()
		return readable and permitted(rank, raw)
	end

	local function valid_utf8(text)
		local pos = 1
		while pos <= #text do
			local a, b = text:byte(pos, pos + 1)
			if a < 32 or a == 127 then return false end
			if a < 128 then
				pos = pos + 1
			else
				local n = (a >= 194 and a <= 223 and 2) or (a >= 224 and a <= 239 and 3)
					or (a >= 240 and a <= 244 and 4)
				if not n or pos + n - 1 > #text then return false end
				if (a == 224 and b < 160) or (a == 237 and b >= 160)
					or (a == 240 and b < 144) or (a == 244 and b >= 144) then return false end
				for i = pos + 1, pos + n - 1 do
					local continuation = text:byte(i)
					if continuation < 128 or continuation > 191 then return false end
				end
				pos = pos + n
			end
		end
		return true
	end

	local booleans = { on = true, off = true, yes = true, no = true, ["true"] = true, ["false"] = true,
		["1"] = true, ["0"] = true }
	local capabilities = { none = true, all = true, read = true, commands = true, target = true, actions = true,
		move = true, chat = true, events = true, ui = true, packets = true, extend = true, login = true, net = true,
		web = true }
	local groups = { non_rof2 = true, addon_commands = true, movement_automation = true, command_helpers = true }

	local function version_number(s)
		return unsigned(s, 0, 4294967295) and (#s == 1 or s:sub(1, 1) ~= "0")
	end

	local function identifiers(s, pre)
		if s == "" or s:sub(1, 1) == "." or s:sub(-1) == "." or s:find("..", 1, true) then return false end
		for id in s:gmatch("[^.]+") do
			if not id:match("^[0-9A-Za-z-]+$") or (pre and id:match("^%d+$") and not version_number(id)) then
				return false
			end
		end
		return true
	end

	local function version(s)
		s = (s:gsub("^[vV]", ""))
		local core, build = s:match("^(.-)%+(.*)$")
		if core then
			if not identifiers(build, false) then return false end
			s = core
		end
		local pre
		core, pre = s:match("^(.-)%-(.*)$")
		if core then
			if not identifiers(pre, true) then return false end
			s = core
		end
		local major, minor, patch = s:match("^(%d+)%.(%d+)%.(%d+)$")
		return major and version_number(major) and version_number(minor) and version_number(patch)
	end

	local function valid_ref(ref)
		if #ref == 0 or #ref > 100 or ref:match("^[-/]") or ref:sub(-1) == "/" or ref:find("//", 1, true) then
			return false
		end
		for part in ref:gmatch("[^/]+") do
			if part:sub(1, 1) == "." or not part:match("^[0-9A-Za-z_.-]+$") then return false end
		end
		return true
	end

	local function repo_identity(s)
		if #s > 400 or s:find("[%s\\]") then return nil end
		local lower = s:lower()
		local url = lower:match("^https?://") or lower:match("^github%.com/") or lower:match("^www%.github%.com/")
		local owner, repo
		if url then
			local prefix = lower:match("^https?://") or ""
			local host = lower:sub(#prefix + 1):match("^www%.github%.com/") or lower:sub(#prefix + 1):match("^github%.com/")
			if not host then return nil end
			local parts = {}
			for part in s:sub(#prefix + #host + 1):gsub("[?#].*$", ""):gmatch("[^/]+") do parts[#parts + 1] = part end
			owner, repo = parts[1], parts[2]
			local path = parts[3]
			if path == "tree" and #parts >= 4 then
				if not valid_ref(table.concat(parts, "/", 4)) then return nil end
			elseif path == "releases" and parts[4] == "tag" and #parts >= 5 then
				if not valid_ref(table.concat(parts, "/", 5)) then return nil end
			elseif path and not (path == "releases" and (#parts == 3 or (#parts == 4 and parts[4] == "latest"))) then
				return nil
			end
		else
			local pair, ref = s:match("^(.-)@(.*)$")
			if pair then
				if not valid_ref(ref) then return nil end
				s = pair
			end
			owner, repo = s:match("^([^/]+)/([^/]+)$")
		end
		if not owner or not repo or #owner > 39 or not owner:match("^[0-9A-Za-z-]+$") or owner:match("^-")
			or owner:match("-$") or owner:find("--", 1, true) then return nil end
		if #repo > 100 or not repo:match("^[0-9A-Za-z_.-]+$") or repo == "." or repo == ".." then return nil end
		if #repo > 4 and repo:sub(-4):lower() == ".git" then repo = repo:sub(1, -5) end
		return (owner .. "/" .. repo):lower()
	end

	local function repositories(text, minimum)
		local result = {}
		if text == "" then return result end
		for original in (text .. ","):gmatch("(.-),") do
			local entry = trim(original)
			local id = repo_identity(entry)
			if not id and minimum then
				local source, min_version = entry:match("^(.*):([^:]+)$")
				if source and version(trim(min_version)) then id = repo_identity(trim(source)) end
			end
			if not id then return nil end
			result[id] = true
		end
		return result
	end

	local function valid_command_word(kind, word)
		if kind == "groups" then return groups[word:lower()] end
		if kind == "commands" then return word ~= "/" end
		local command, status = word:match("^(.+):([0-9]+)$")
		return command and command ~= "/" and unsigned(status, 1, 32767)
	end

	local function valid_command_list(kind, value, original)
		local existing
		for word in value:gmatch("[^ ,]+") do
			if not valid_command_word(kind, word) then
				-- Another row may be edited without rewriting the author's malformed tokens. Only carry exact
				-- tokens that appeared in this key of the snapshot, without adding copies.
				if not existing then
					existing = {}
					for token in (original or ""):gmatch("[^,%s]+") do existing[token] = (existing[token] or 0) + 1 end
				end
				if (existing[word] or 0) == 0 then return false end
				existing[word] = existing[word] - 1
			end
		end
		return true
	end

	local function valid_value(kind, value, original)
		if kind == "message" then return #value <= 240 and valid_utf8(value) end
		if value:find("[%z\1-\31\127;#]") then return false end
		local lower = value:lower()
		if kind == "boolean" then return booleans[lower] end
		if kind == "mode" then return lower == "allow" or lower == "deny" end
		if kind == "social_mode" then return lower == "off" or lower == "press" or lower == "full" end
		if kind == "press_commands" then return unsigned(value, 0, 2147483647) end
		if kind == "min_status" then return unsigned(value, 1, 32767) end
		if kind == "repos" or kind == "required" then return repositories(value, kind == "required") ~= nil end
		if kind == "groups" or kind == "commands" or kind == "gm_commands" then
			return valid_command_list(kind, value, original)
		end
		for word in value:gmatch("[^ ,]+") do
			local low = word:lower()
			if kind == "caps" and not capabilities[low] then return false end
			if kind == "scripts" and (word:sub(1, 1) == "/" or word:find("..", 1, true) or word:find("\\", 1, true)) then
				return false
			end
			if kind == "hosts" and (#word > 253 or (not word:match("^[0-9A-Za-z_.-]+$")
				and not word:match("^%[[0-9a-fA-F:.]+%]$"))) then return false end
			if kind == "suggest" and not repo_identity(word) then return false end
		end
		return true
	end

	local function read_edits(packet, size, raw)
		if size < 18 then return nil end
		local count, pos = packet:ReadInt16(16), 18
		if count < 1 or count > MAX_EDITS then return nil end
		local edits, seen = {}, {}
		for n = 1, count do
			if pos + 4 > size then return nil end
			local a, b, c = packet:ReadInt8(pos), packet:ReadInt8(pos + 1), packet:ReadInt16(pos + 2)
			pos = pos + 4
			if a < 1 or a > MAX_NAME or b < 1 or b > MAX_NAME or c < 0 or c > MAX_VALUE or pos + a + b + c > size then
				return nil
			end
			local section = packet:ReadFixedLengthString(pos, a)
			local key = packet:ReadFixedLengthString(pos + a, b)
			local value = c > 0 and packet:ReadFixedLengthString(pos + a + b, c) or ""
			pos = pos + a + b + c
			if #section ~= a or #key ~= b or #value ~= c then return nil end
			section, key = section:lower(), key:lower()
			local kind = schema[section] and schema[section][key]
			local id = section .. "." .. key
			local original = section == "commands" and read_control(raw or "", section, key) or nil
			if not kind or seen[id] or value:find("[%z\1-\31\127]")
				or not valid_value(kind, kind == "message" and value or trim(value), original) then
				return nil, "Invalid or duplicate setting: [" .. (schema[section] and section or "unknown") .. "]"
			end
			seen[id] = true
			edits[n] = { section = section, key = key, id = id,
				value = kind == "message" and ('"' .. value .. '"') or trim(value) }
		end
		if pos ~= size then return nil end
		return edits
	end

	local function inline_comment(value, quoted)
		if quoted then
			local opening, quote = value:match("^([ \t]*)(['\"])")
			if opening then
				for pos = #value, #opening + 2, -1 do
					if value:sub(pos, pos) == quote then
						local tail = value:sub(pos + 1)
						if tail:match("^[ \t]*$") or tail:match("^[ \t]*[;#]") then
							return value:sub(1, pos), tail
						end
					end
				end
			end
		end
		local comment = value:find("[;#]")
		return comment and value:sub(1, comment - 1) or value, comment and value:sub(comment) or ""
	end

	local function patch(raw, edits)
		local lines, indexes, sections = {}, {}, {}
		local newline = raw:match("(\r?\n)") or "\n"
		local current, pos = "", 1
		while pos <= #raw do
			local nl = raw:find("\n", pos, true)
			local ending = nl and (raw:sub(nl - 1, nl - 1) == "\r" and "\r\n" or "\n") or ""
			local line = raw:sub(pos, nl and nl - #ending or #raw)
			lines[#lines + 1] = { text = line, ending = ending }
			local name = section_name(trim(line))
			if name then
				current = name
			elseif not trim(line):match("^[;#]") then
				local key = line:match("^([^=]+)=")
				if key then indexes[current .. "." .. trim(key):lower()] = #lines end
			end
			sections[current] = #lines
			pos = nl and nl + 1 or #raw + 1
		end
		local insertions, new_sections, new_entries = {}, {}, {}
		for _, edit in ipairs(edits) do
			local i = indexes[edit.id]
			if i then
				local line = lines[i]
				local prefix, value = line.text:match("^(.-=)(.*)$")
				local base, suffix = inline_comment(value, edit.id == "commands.blocked_message")
				if trim(base) ~= edit.value then
					local leading = base:match("^[ \t]*")
					local trailing = trim(base) ~= "" and base:match("[ \t]*$") or ""
					if edit.id == "commands.blocked_message" and trim(suffix) ~= "" then
						-- A comment may itself end with a quote. Keep it as a comment line to avoid that ambiguity.
						prefix = leading .. trim(suffix) .. newline .. prefix
						suffix = ""
					end
					line.text = prefix .. leading .. edit.value .. trailing .. suffix
				end
			else
				local formatted = edit.key .. " = " .. edit.value
				local after = sections[edit.section]
				if after then
					local additions = insertions[after] or {}
					additions[#additions + 1] = formatted
					insertions[after] = additions
				else
					if not new_entries[edit.section] then
						new_entries[edit.section] = {}
						new_sections[#new_sections + 1] = edit.section
					end
					local additions = new_entries[edit.section]
					additions[#additions + 1] = formatted
				end
			end
		end
		local out = {}
		for i, line in ipairs(lines) do
			out[#out + 1] = line.text .. line.ending
			if insertions[i] then
				if line.ending == "" then out[#out + 1] = newline end
				out[#out + 1] = table.concat(insertions[i], newline) .. newline
			end
		end
		if #new_sections > 0 then
			if #out > 0 and out[#out]:sub(-1) ~= "\n" then out[#out + 1] = newline end
			for _, section in ipairs(new_sections) do
				if #out > 0 then out[#out + 1] = newline end
				out[#out + 1] = "[" .. section .. "]" .. newline .. table.concat(new_entries[section], newline) .. newline
			end
		end
		return table.concat(out), newline
	end

	local function mkdir(path)
		local result = windows and run('mkdir "' .. path:gsub("/", "\\") .. '" 2>NUL && echo made')
			or (not windows and run("mkdir -m 700 -- " .. sh_quote(path) .. " 2>/dev/null && echo made"))
		return type(result) == "string" and trim(result) == "made"
	end

	local function remove_file(path)
		if windows then return trim(run('del /Q "' .. path:gsub("/", "\\") .. '" 2>NUL && echo removed') or "") == "removed" end
		return trim(run("rm -f -- " .. sh_quote(path) .. " 2>/dev/null && echo removed") or "") == "removed"
	end

	local function remove_edit_lock()
		remove_file(EDIT_LOCK_OWNER)
		if windows then run('rmdir "' .. EDIT_LOCK:gsub("/", "\\") .. '" 2>NUL')
		else run("rmdir " .. sh_quote(EDIT_LOCK)) end
	end

	local function take_edit_lock()
		if mkdir(EDIT_LOCK) then
			local owner = os.time() .. " " .. scratch() .. "\n"
			if write(EDIT_LOCK_OWNER, owner) then return owner end
			remove_edit_lock()
			return nil, STORAGE, "Could not record the server policy lock owner."
		end
		local owner = read(EDIT_LOCK_OWNER, 512)
		local timestamp = owner and tonumber(owner:match("^(%d+) [%w_-]+\n$"))
		-- A slow or suspended zone may still hold the lock. Only the operator can confirm it is abandoned.
		if timestamp and os.time() - timestamp >= EDIT_LOCK_STALE then
			return nil, BUSY, "Server policy lock may be stale. Ask the server operator to check .hailstone-admin-lock."
		end
		return nil, BUSY, "The server policy is busy or not writable. Try again."
	end

	local function unlock(owner)
		if read(EDIT_LOCK_OWNER, #owner) == owner then remove_edit_lock() end
	end

	local function owned_backup(base, revision)
		local audit = read(base .. ".txt", 4096)
		if not audit then return false end
		local recorded, prior = audit:match("^revision=(" .. REVISION_ID .. ")\nprevious_revision=(" .. REVISION_ID ..
			")\nprevious_existed=true\ncharacter_id=%d+\naccount_id=%d+\nedited=[%w_,.]+\n$")
		if recorded ~= revision then return false end
		local raw = read(base .. ".cfg", LIMIT)
		return raw and policy_revision(raw) == prior
	end

	local function prune_history(newest)
		local keep = setting("server_management_history")
		if keep < 1 or keep % 1 ~= 0 then return end
		local listing
		if windows then listing = run('dir /b /a:-d-l "' .. HISTORY:gsub("/", "\\") .. '\\*" 2>NUL')
		else listing = run("find " .. sh_quote(HISTORY) .. " -maxdepth 1 -type f -printf '%f\\n' 2>/dev/null") end
		if not listing then return end
		local configs, audits, older = {}, {}, {}
		for name in listing:gmatch("[^\r\n]+") do
			local revision, extension = name:match("^(" .. REVISION_ID .. ")%.([a-z]+)$")
			if extension == "cfg" then configs[revision] = true end
			if extension == "txt" then audits[revision] = true end
		end
		for revision in pairs(configs) do
			if revision ~= newest and audits[revision] then older[#older + 1] = revision end
		end
		table.sort(older, function(a, b) return a > b end)
		local count = 1 -- Always keep the backup created by this save, even if the clock moved backward.
		for _, revision in ipairs(older) do
			local base = HISTORY .. "/" .. revision
			if owned_backup(base, revision) then
				count = count + 1
				if count > keep and (not remove_file(base .. ".txt") or not remove_file(base .. ".cfg")) then
					log("management: could not prune revision " .. revision)
				end
			end
		end
	end

	local PS_REPLACE = [[
param($from, $to)
$ErrorActionPreference = 'Stop'
$src = Join-Path (Get-Location) $from
$dst = Join-Path (Get-Location) $to
if ([System.IO.File]::Exists($dst)) {
    [System.IO.File]::Replace($src, $dst, [System.Management.Automation.Language.NullString]::Value)
} else {
    [System.IO.File]::Move($src, $dst)
}
[Console]::WriteLine('replaced')
]]
	local function replace(from, to)
		if windows then
			local output = powershell(PS_REPLACE, '"' .. from:gsub("/", "\\") .. '" "' .. to:gsub("/", "\\") .. '"')
			return output and trim(output) == "replaced"
		end
		local output = run("mv -fT -- " .. sh_quote(from) .. " " .. sh_quote(to) .. " && echo replaced")
		return output and trim(output) == "replaced"
	end

	local function reply(client, action, request, status, revision, data)
		local packet = Packet(REPLY, 20 + #data, true)
		packet:WriteInt32(0, MAGIC)
		packet:WriteInt16(4, 1)
		packet:WriteInt16(6, action)
		packet:WriteInt32(8, request)
		packet:WriteInt16(12, status)
		packet:WriteInt32(14, revision)
		packet:WriteInt16(18, #data)
		if #data > 0 then packet:WriteFixedLengthString(20, data, #data) end
		client:QueuePacket(packet, true)
	end

	local function mark_used(entry)
		last_use = last_use + 1
		entry.last_used = last_use
	end

	local function snapshot(client, id, raw, name)
		if not snapshots[id] then
			local count, oldest = 0, nil
			for char_id, entry in pairs(snapshots) do
				count = count + 1
				if not oldest or entry.last_used < snapshots[oldest].last_used then oldest = char_id end
			end
			if count >= MAX_SNAPSHOTS then snapshots[oldest] = nil end
		end
		token = token % 2147483647 + 1
		local entry = { token = token, raw = raw, name = name, entity = client:GetID(), account = client:AccountID() }
		mark_used(entry)
		snapshots[id] = entry
		return token
	end

	local function save_under_lock(client, previous, edits)
		if not enabled() then return DENIED, "Server Management access denied." end
		local raw, readable, name = load_cfg(true)
		if not readable then return STORAGE, "Could not read hailstone.cfg." end
		if not permitted(account_status(client) or 0, raw) then return DENIED, "Server Management access denied." end
		if raw ~= previous.raw or name ~= previous.name then
			return STALE, "The server configuration changed. Reload before saving."
		end
		local updated, newline = patch(raw or NO_POLICY, edits)
		if updated == (raw or NO_POLICY) then return OK, raw or NO_POLICY, raw, name end
		local addon_policy_changed = false
		for _, edit in ipairs(edits) do
			if edit.section == "addons" then addon_policy_changed = true end
		end
		if addon_policy_changed then
			local required = repositories(read_control(updated, "addons", "required") or "", true)
			local blocked = repositories(read_control(updated, "addons", "block") or "", false)
			if not required or not blocked then return INVALID, "Review the required and blocked add-on lists." end
			for id in pairs(required) do
				if blocked[id] then return INVALID, "An add-on cannot be both required and blocked." end
			end
		end
		local old_revision = policy_revision(updated)
		if old_revision then updated = updated:sub(updated:find("\n", 1, true) + 1) end
		mkdir(HISTORY) -- existing is fine; the verified backup write below is authoritative
		local revision, backup = next_revision(old_revision)
		if not revision then return STORAGE, "Could not reserve a revision in the policy history." end
		updated = POLICY_REVISION .. revision .. newline .. updated
		if #updated > LIMIT then return INVALID, "The resulting hailstone.cfg is larger than 16 KiB." end
		local keys = {}
		for _, edit in ipairs(edits) do keys[#keys + 1] = edit.id end
		local audit = string.format("revision=%s\nprevious_revision=%s\nprevious_existed=%s\ncharacter_id=%d\naccount_id=%d\nedited=%s\n",
			revision, old_revision or "none", tostring(raw ~= nil), client:CharacterID(), client:AccountID(), table.concat(keys, ","))
		if not write(backup .. ".cfg", raw or "") or not write(backup .. ".txt", audit) then
			return STORAGE, "Could not back up hailstone.cfg. Active policy was not changed."
		end
		local tmp = FILES_DIR .. "/.hailstone-admin-" .. revision .. ".tmp"
		local path = FILES_DIR .. "/" .. name
		if not write(tmp, updated) then return STORAGE, "Could not stage hailstone.cfg." end
		if raw ~= nil and not windows then
			local output = run("chmod --reference=" .. sh_quote(path) .. " -- " .. sh_quote(tmp) .. " && echo preserved")
			if not output or trim(output) ~= "preserved" then return STORAGE, "Could not preserve hailstone.cfg permissions." end
		end
		-- Compare again before replacement, including external edits during the backup. Other windows are locked out.
		local latest, still_readable = read(path, LIMIT)
		if not still_readable or latest ~= raw then return STALE, "The server configuration changed. Reload before saving." end
		if not replace(tmp, path) or read(path, LIMIT) ~= updated then return STORAGE, "Could not replace hailstone.cfg." end
		local pruned, prune_error = pcall(prune_history, revision)
		if not pruned then log("management: could not prune policy history: " .. tostring(prune_error)) end
		log(string.format("management: saved revision %s by account %d, character %d (%s)", revision,
			client:AccountID(), client:CharacterID(), table.concat(keys, ", ")))
		return OK, updated, updated, name
	end

	local function on_request(client, packet)
		local size = packet:GetSize()
		if setting("enabled") == 0 or size < 12 or size > LIMIT + 18 or packet:ReadInt32(0) ~= MAGIC
			or packet:ReadInt16(4) ~= 1 then return end
		local action, request = packet:ReadInt16(6), packet:ReadInt32(8)
		if request == 0 or (action ~= OPEN and action ~= SAVE) then return end
		local id = client:CharacterID()
		if not allow(requests, id, 12) then return end
		local rank = account_status(client) or 0
		if rank < 1 or not enabled() then
			reply(client, action, request, DENIED, 0, "Server Management access denied.")
			return
		end
		-- The existing file listing supplies the name for a direct read. Only admitted requests can cause a
		-- filesystem scan, which still checks the authoritative name and policy before opening or writing.
		local raw, readable = load_cfg()
		if not readable then
			reply(client, action, request, STORAGE, 0, "Could not read hailstone.cfg.")
			return
		end
		if not permitted(rank, raw) then
			reply(client, action, request, DENIED, 0, "Server Management access denied.")
			return
		end
		if action == OPEN and size == 12 then
			local current, current_readable, name = load_cfg(true)
			if not current_readable then
				reply(client, action, request, STORAGE, 0, "Could not read hailstone.cfg.")
				return
			end
			if not permitted(account_status(client) or 0, current) then
				reply(client, action, request, DENIED, 0, "Server Management access denied.")
				return
			end
			reply(client, action, request, OK, snapshot(client, id, current, name), current or NO_POLICY)
			return
		end
		if action == OPEN or size < 18 then
			reply(client, action, request, INVALID, 0, "Malformed Server Management request.")
			return
		end
		local previous = snapshots[id]
		if not previous or previous.token ~= packet:ReadInt32(12) or previous.entity ~= client:GetID()
			or previous.account ~= client:AccountID() then
			reply(client, action, request, STALE, 0, "The server no longer has this editor snapshot. Reload before saving.")
			return
		end
		local edits, why = read_edits(packet, size, previous.raw)
		if not edits then
			reply(client, action, request, INVALID, 0, why or "Malformed Server Management changes.")
			return
		end
		mark_used(previous)
		local owner, lock_status, lock_message = take_edit_lock()
		if not owner then
			reply(client, action, request, lock_status, 0, lock_message)
			return
		end
		local ok, status, data, after, filename = pcall(save_under_lock, client, previous, edits)
		unlock(owner)
		if not ok then
			log("management: save failed: " .. tostring(status))
			reply(client, action, request, STORAGE, 0, "Could not save hailstone.cfg.")
			return
		end
		local revision = status == OK and snapshot(client, id, after, filename) or 0
		reply(client, action, request, status, revision, data)
	end

	local function on_disconnect(e)
		local client = e.self
		local id = client:CharacterID()
		local entry = snapshots[id]
		if entry and entry.entity == client:GetID() and entry.account == client:AccountID() then
			snapshots[id] = nil
		end
	end

	return {
		request = REQUEST,
		advertised = advertised,
		account_status = account_status,
		on_request = on_request,
		on_disconnect = on_disconnect,
		reset = function()
			snapshots, last_use, requests = {}, 0, limiter(10)
		end,
	}
end)()

-- The server's settings for the client. Bit 0, the retired player count setting, stays clear.
-- Bit 1 advertises Server Management to an eligible account. The encounter still authorizes each request.
-- Bit 2 carries the recipient's account status for command rules. Older replies do not carry the field.
local function send_settings(client)
	if setting("enabled") == 0 then
		return
	end
	local opcode = reply_op(client, OP_SETTINGS)
	local rank = management.account_status(client)
	local flags = management.advertised(rank or 0) and 2 or 0
	local include_rank = opcode == OP_SETTINGS and rank ~= nil
	if include_rank then flags = flags + 4 end
	local p = Packet(opcode, include_rank and 12 or 10, true)
	p:WriteInt32(0, MAGIC)
	p:WriteInt16(4, 1)
	p:WriteInt32(6, flags)
	if include_rank then p:WriteInt16(10, rank) end
	client:QueuePacket(p)
end

-- A client asks once a zone-in: at most SETTINGS_MAX a character in SETTINGS_SECONDS are answered.
local SETTINGS_MAX, SETTINGS_SECONDS = 5, 10
local settings_limit = limiter(SETTINGS_SECONDS)

-- Every opcode the zone has no handler for comes here; only Hailstone's are ours.
local function on_unhandled_opcode(e)
	local packet = e.packet
	local op     = packet:GetProtocolOpcode()
	if op == OP_GUILD - OLD_OPCODES_SHIFT or op == OP_FILES_REQUEST - OLD_OPCODES_SHIFT
		or op == OP_SETTINGS_REQUEST - OLD_OPCODES_SHIFT then
		e.self:SetEntityVariable(OLD_OPCODES_VAR, "1")
		op = op + OLD_OPCODES_SHIFT
	elseif op == OP_GUILD or op == OP_FILES_REQUEST or op == OP_SETTINGS_REQUEST then
		e.self:SetEntityVariable(OLD_OPCODES_VAR, "0")
	end
	if op == OP_GUILD then
		on_guild_packet(e.self, packet)
		return
	end
	if op == OP_FILES_REQUEST then
		on_files_request(e.self, packet)
		return
	end
	if op == OP_CANCEL_INVITE then
		on_cancel_invite(e.self, packet)
		return
	end
	if op == OP_QUEST_REQUEST then
		on_quest_request(e.self, packet)
		return
	end
	if op == OP_ZONE_LINKS_REQUEST then
		on_zone_links_request(e.self, packet)
		return
	end
	if op == management.request then
		if not e.connecting then management.on_request(e.self, packet) end
		return
	end
	if op ~= OP_SETTINGS_REQUEST then
		return
	end
	-- Counted before anything else, the log line of a bad one too: a flood of them is dropped unlogged.
	local ok, first_over = allow(settings_limit, e.self:CharacterID(), SETTINGS_MAX)
	if not ok then
		if first_over then
			log(string.format("%s asks for the settings too fast; the rest of %d s are dropped", e.self:GetCleanName(), SETTINGS_SECONDS))
		end
		return
	end
	if packet:GetSize() < 6 or packet:ReadInt32(0) ~= MAGIC then
		log(string.format("Hailstone request 0x%04x of size %d without the NMSP magic", op, packet:GetSize()))
		return
	end
	send_settings(e.self)
end

function event_encounter_load(e)
	encounter = e.encounter
	setting_cache = {} -- #reloadquest: the buckets as they are now
	management.reset()
	quest_list, quest_file_flags = nil, {} -- and the quest files as they are now
	zone_links = nil   -- and the zone links built again when next asked
	if setting("enabled") ~= 0 and setting("client_files") ~= 0 then
		build_client_files(true) -- the hashing is done before the first client asks
	end
	eq.register_player_event("hailstone", Event.unhandled_opcode, on_unhandled_opcode)
	eq.register_player_event("hailstone", Event.signal, on_signal)
	if Event.disconnect then eq.register_player_event("hailstone", Event.disconnect, management.on_disconnect) end
end

function event_encounter_unload(e)
	waiting, waiting_order, files_timer, served_last = {}, nil, false, nil
	eq.unregister_player_event("hailstone", Event.unhandled_opcode)
	eq.unregister_player_event("hailstone", Event.signal)
	if Event.disconnect then eq.unregister_player_event("hailstone", Event.disconnect) end
end
