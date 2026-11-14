# RoF2 world and zone packets

What `src/net/world_packets.*` and `src/net/zone_packets.*` encode and decode, and where each layout comes from. The server is EQEmu; opcodes are from stock EQEmu's `utils/patches/patch_RoF2.conf` (generated into `src/net/opcodes_rof2.inc` by `tools/gen_opcodes.py`). "rof2.cpp" means `common/patches/rof2.cpp`, the server's RoF2 translator; opcodes without an ENCODE/DECODE there are passed through, so the layout is the server's own struct in `common/eq_packet_structs.h`. All values are little-endian. "cstr" is a NUL-terminated string, `char[n]` a NUL-padded fixed field.

Sizes in this file are payload sizes, without the 2-byte opcode.

## 1. Sequence

### World (world/client.cpp)

1. C→W `OP_SendLoginInfo` (464 B). EQEmu picks the RoF2 patch from this first packet's opcode and length.
2. W→C, in order: `OP_GuildsList` (not when zoning), `OP_LogServer`, `OP_ApproveWorld` (fixed 544-byte blob; the client may ignore it), `OP_EnterWorld` (name; empty unless auto-login or zoning), `OP_PostEnterWorld` (empty; "access granted"). Not zoning: `OP_ExpansionInfo`, then `SendCharInfo`: `OP_SendMaxCharacters`, `OP_SendMembership`, `OP_SendMembershipDetails`, `OP_SendCharInfo`.
   `OP_SendMembership` and `OP_SendMembershipDetails` are kept (`GameConnection::membership()`, section 2); EQEmu always sends gold.
3. C→W `OP_World_Client_CRC1/2/3` after each character list, and CRC1 again before every `OP_EnterWorld`.
4. C→W `OP_WorldClientReady` (empty) once the character select screen is up, after the first list's checksums. EQEmu's world eats it.
5. Character creation (section 3).
6. C→W `OP_EnterWorld` (72 B). W→C `OP_MOTD`, `OP_SetChatServer`, `OP_SetChatServer2`, (an `OP_GroupUpdate` for grouped characters), then `OP_ZoneServerInfo` or `OP_ZoneUnavail`.
7. C→W `OP_WorldComplete` (empty); world closes the connection (EQEmu: `eqs->Close()`); the client connects to the zone address only after world has dropped the link, and gives up after 30 s ("World did not drop the connection"). Without `OP_WorldComplete`, EQEmu's world drops the idle world client by itself about 3 s later ("Client disconnected (not active in process)").

While zoning (`LoginInfo::zoning = true`), world skips the guild list and character list; the client re-sends CRC1 and `OP_EnterWorld` with the name world sent in its `OP_EnterWorld`.

### Zone-in (zone/client_packet.cpp)

1. C→Z `OP_ZoneEntry` (76 B; the RoF2 zone signature).
2. Z→C (`Handle_Connect_OP_ZoneEntry`): `OP_PlayerProfile`, then the player's own spawn as `OP_ZoneEntry`, then every other spawn and corpse as one `OP_ZoneEntry` each, `OP_TimeOfDay`, tributes, inventory (`OP_CharInventory`), `OP_XTargetResponse`, `OP_Weather`, and, for RoF and later, `OP_NewZone` without being asked.
3. The client waits for its own spawn and `OP_Weather` (`OP_Weather` is ignored until its own spawn arrived), then sends `OP_ReqClientSpawn` (empty). First come `OP_SendAATable` / `OP_UpdateAA` (empty; the server answers with AA data) and, on the **first zone-in of the session only**, `OP_SendTributes` then `OP_RequestGuildTributes` (4 bytes each, unused; the guild list is asked for in a guild or not). Later zone-ins keep the lists. EQEmu answers both only while the client is connecting (`ConnectingOpcodes`). RoF2 clients also send 0x2b8e, 0x3a02 (10 B), 0x4f05 (18 B) and 0x7273 around them; none is in EQEmu's RoF2 table, and this client sends none. **`OP_ReqNewZone` is never sent by the RoF2 client** and is not needed.
4. Z→C (`Handle_Connect_OP_ReqClientSpawn`): doors, objects, zone points, `OP_SendAAStats`, `OP_SendExpZonein`, `OP_ClientReady`, `OP_WorldObjectsSent` (all empty). `OP_ZoneServerReady` has no RoF2 opcode and is dropped.
5. C→Z echo `OP_SendExpZonein` (empty) on receiving it, and echo `OP_WorldObjectsSent` (empty) on receiving it. The second echo makes the server send the zone-in packets (`SendZoneInPackets`: appearance, HP, mana, etc.).
6. C→Z `OP_ClientReady` (empty). The server finishes the connect (`CompleteConnect`) and sends an HP update. The client sends its `OP_ClientReady` once it has seen `OP_SendExpZonein`, then waits for an `OP_ClientReady` from the server (already sent in step 4); neither wait has a timeout.
7. In zone: `OP_ClientUpdate` (46 B) when moving.

### Zoning, camp

- Server-initiated: Z→C `OP_RequestClientZoneChange` (#zone, #goto, summons; zone dword = zone | instance << 16, position, heading, type). A request for the zone the player is in moves the player there and sends nothing (the next `OP_ClientUpdate` tells the server); another zone makes the client answer `OP_ZoneChange`. The player's own zone dword takes its instance from the player profile (`zoneInstance`): stock EQEmu leaves `OP_NewZone`'s instance at 0, so with it alone a summon inside an instance zoned the player.
- Zone lines (client-initiated, `game/zone_lines.{h,cpp}`, `GameConnection::request_zone_change`): during zone-in the zone sends `OP_SendZonepoints`, the targets of its zone points. When the player's feet enter a zone-line region of the zone WLD (`docs/formats/wld.md`, "Regions"), the client resolves the region's tag to a zone, instance and target position (a "zone 255" tag through the zone-point table by number, otherwise the zone id and coordinates in the tag), then sends C→Z `OP_ClientUpdate` with its current position and C→Z `OP_ZoneChange` {zone, instance, target y, x, z, reason 0, success 0}. The EQEmu server (`zone/zoning.cpp` `Handle_OP_ZoneChange`, zone_mode ZoneUnsolicited) checks only the zone: it takes the zone point **nearest to the last reported position** among those leading to that zone (`zone/zone.cpp` `GetClosestZonePoint`, range 40000), refuses the current zone (`SendZoneCancel`), and ignores the coordinates sent; the arrival point is that zone point's target. When the server has a water map that does not put the reported position in a zone line, it logs "Closest zone point ... WARNING" and reports `MQZoneUnknownDest` to its cheat manager but still zones the player.
- C→Z `OP_ZoneChange` (success 0). Z→C `OP_ZoneChange` with success 1 → C→Z `OP_DeleteSpawn` (4 bytes: own spawn id), wait up to 120 s for the zone to drop the link (after that the client closes it), reconnect to world with `zoning = 1`, then `OP_EnterWorld` again. EQEmu's `Handle_OP_DeleteSpawn` queues `OP_LogoutReply` (no RoF2 opcode, so not sent), tells the other clients `OP_DeleteSpawn`, and drops the link at once. Before it, the client may send `OP_SaveOnZoneReq` (0x600d, 484 B: a checksum and the position, armour and actors); this client does not (EQEmu saves on the zone change anyway). The client waits up to **180 s** for the answer; an answer of −1 (not ready) within 7 s of the first request is followed by the same request 500 ms later. EQEmu answers only once the target zone is up, which takes several seconds when world has to boot it (`world/zoneserver.cpp` ServerOP_ZoneToZoneRequest); a client that gave up after 7 s printed "The zone change request timed out." and let the trigger send a second request.
- Camp (`GameConnection::camp`): `/camp` sits the player, then C→Z `OP_Save` (0x4a39, 484 B, kind 0x21; table below) and `OP_Camp` (u32 kind: 0 character select, 1 `/camp desk`, 2 `/camp server`, from the argument's first four letters). After a 30 s timer: `OP_Logout` (empty) and up to 3 s for the zone to drop the link (without closing it). If the link is still up, the client sends `OP_SaveOnZoneReq` (kind 0) and `OP_DeleteSpawn` (own id), unless the respawn window is up, and waits up to 120 s more (then closes it). There is no camp reply: EQEmu's `Handle_OP_Logout` queues `OP_LogoutReply` (opcode 0 in `patch_RoF2.conf`, so never sent; `OP_PreLogoutReply` too) and drops the link at once, so on EQEmu `OP_SaveOnZoneReq` and the `OP_DeleteSpawn` never go out. Then kind 0 goes back through world: the character name is cleared and world authenticates again on the same credentials (`OP_SendLoginInfo` with zoning 0, the login ticket's account and key), which shows character select (`ClientApp::camp_finished`, a new `GameConnection` on the same `WorldTicket`). Kind 1 closes the client, kind 2 goes back to the front end (the login screen).

  A link drop during the camp's 30 s (before `OP_Logout`) is link death, as anywhere in game: once the link's status is 1 (the 180 s no-data timeout, or the server's disconnect), the client logs "disconnected at %s:%u (char. select) %s", shows a disconnected screen (the screen cleared, string 12353 "YOU HAVE BEEN DISCONNECTED." in the middle) for 3 s, waits 16 s more and leaves the camp's way (0 character select, 1 the desktop, 2 the front end; a cancelled camp has reset it to 0), sending no `OP_SaveOnZoneReq` / `OP_DeleteSpawn` (the link is down). Outside the game (character select, the zone-in waits) it is the same screen and 3 s, then the front end. This client: `GameConnection::link_death`, `ClientApp::update_link_death` (the screen without the world, then `camp_finished(kind)` or the login screen); a link that never connected is not a link death.

  Closing the window: at character select or creation it quits; in any other state it is `/camp desk` with its checks (feigning, the sit), and a second close during the camp abandons it (12290) as a second `/camp` does (`ClientApp::window_close_request`; SDL's close request is held, a signal's quit still quits at once).

## 2. World packets

| Packet | Dir | Opcode | Layout as implemented |
|---|---|---|---|
| SendLoginInfo | C→W | 0x7a09 | 464 B. +0 `char[64]` "account\0key\0"; +64 `char[124]` custom-files key (NUL-terminated, empty normally); +188 u8 zoning; +192 u8 0xcc; rest 0 |
| LogServer | W→C | 0x7ceb | 560 B. +4 u8 pvp, +8 u8 enable_FV, +36 `char[32]` world short name |
| ApproveWorld | W→C | 0x7499 | 544-byte constant; not decoded |
| EnterWorld | W→C | 0x578f | cstr name |
| PostEnterWorld | W→C | 0x6259 | empty |
| ExpansionInfo | W→C | 0x590d | 68 B, u32 mask at +64 |
| SendMaxCharacters | W→C | 0x5475 | 12 B, u32 max at +0 |
| SendMembership | W→C | 0x7acc | 116 B: u32 level (0 free, 1 silver, 2 gold), u32 races, u32 classes, u32 count, count × i32 (EQEmu 25; the client keeps the first 22) |
| SendMembershipDetails | W→C | 0x057b | u32 count, count × {u32 level 0..2, u32 feature, i32 value} (EQEmu 72, the last 6 zero); u32 count, × {u32 purchase id, u32 bits} for races, the same for classes; two strings (u32 length + text, EQEmu empty) |
| GuildsList | W→C | 0x507a | 64 zero bytes, u32 count, {u32 id, cstr name}… (entries with id ≥ MAX_GUILD_ID are skipped without fixing the count, so the decoder stops at the end of the payload) |
| SendCharInfo | W→C | 0x00d2 | u32 count, then per character: cstr name + 274 B: u8 class, u32 race, u8 level, u8 shroud class, u32 shroud race, u16 zone, u16 instance, u8 gender, u8 face, 9×{u32 material, unknown1, elite, hero forge, material2, u32 tint}, u8 ×2 (0xff), u32 drakkin tattoo, details, deity, primary id file, secondary id file, u8 hair color, beard color, eye1, eye2, hair style, beard, go home, tutorial, u32 heritage, u8 unknown, u8 enabled, u32 last login, u8 unknown |
| World_Client_CRC1/2/3 | C→W | 0x0f13 / 0x4b8d / 0x298d | 2056 B: u32 eq_crc32(file), u32 size, 256×u32 dword index, 256×u32 dword value |
| WorldClientReady | C→W | 0x23c1 | empty, once at character select |
| EnterWorld | C→W | 0x578f | 72 B: `char[64]` name, u32 tutorial, u32 return home. The original's Return Home sets byte 0x44 and byte 0x45 (set with either flag), so return home reads 0x101; EQEmu moves the character to its home bind and drops the connection when the character may not go home |
| MOTD | W→C | 0x0c22 | cstr |
| SetChatServer(2) | W→C | 0x1bc5 / 0x7eec | cstr "host,port,short.Character,<type letter><8 hex key>" |
| ZoneServerInfo | W→C | 0x4c44 | 130 B: `char[128]` ip, u16 port |
| ZoneUnavail | W→C | 0x4cb4 | `char[16]` zone name, 4×i16 |
| WorldComplete | C→W | 0x4493 | empty |

The checksums: EQEmu reads `uint64 checksum` = crc | size << 32 from the first 8 bytes, stores it on the account, and only rejects it when `World:EnableChecksumVerification` is on **and** the `crc_*` variables (client executable, skillcaps, basedata) are set (and the account is below `GM:MinStatusToBypassCheckSumVerification`). `make_file_checksum()` computes the real value from the player's own files, which passes whenever those files are the expected ones. The server does not check the packet size, so the full 2056 bytes must be sent. eq_crc32 is the standard reflected CRC-32 table, start 0xffffffff, **no final inversion**.

## 3. Character creation

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| CharacterCreateRequest | C→W | 0x6773 | empty |
| CharacterCreateRequest | W→C | 0x6773 | u8 0, u32 n, n×{u32 index, 7×u32 base stats, 7×u32 default allocation} (60 B each), u32 m, m×{u32 expansion required, race, class, deity, allocation index, zone} (24 B each). Stat order STR, DEX, AGI, STA, INT, WIS, CHA |
| ApproveName | C→W | 0x56a2 | 80 B: `char[64]` name, u32 race, u32 class, u32 deity, u32 tutorial flag |
| ApproveName | W→C | 0x56a2 | 1 B: 1 ok, 0 refused |
| CharacterCreate | C→W | 0x6bbf | 96 B: u32 gender, race, class, deity, start_zone, hair color, beard, beard color, hair style, face, eye1, eye2, drakkin heritage, tattoo, details, STR, STA, AGI, DEX, WIS, INT, CHA, tutorial, unknown |
| DeleteCharacter | C→W | 0x1808 | cstr name |
| RandomNameGenerator | both | 0x5954 | 72 B: u32 race, u32 gender, `char[64]` name |

Flow: request the tables (`OP_CharacterCreateRequest`), send `OP_ApproveName`; on reply 1 send `OP_CharacterCreate`; world answers with a fresh `OP_SendCharInfo` on success, or `OP_ApproveName` = 0 on failure (the reserved name is then deleted).

What world checks:
- Name (`HandleNameApprovalPacket`): 4–15 characters, first letter upper case, the rest lower case, no spaces, passes the name filter, not taken. Race must be a player race and class 1..16, or world **drops the connection** (returns false).
- `OP_CharacterCreate` must be exactly 96 bytes, and the account must have a free slot.
- `CheckCharCreateInfoSoF`: (class, race, deity, start_zone) must be a row of `char_create_combinations` (sent in the table above); each stat must be within [base, base + budget] of its allocation, budget = sum of the default allocation, and the total spent ≤ budget. `validate_char_create()` implements this, `default_char_create()` builds a valid request from a combo.
- Start position: `start_zones` row for (zone_id = start_zone, class, deity, race), else defaults; `World:SoFStartZoneID` overrides the zone.
- Example from the server's `utils/sql/svn/2024_required_update.sql` (the live DB may differ; always use the table world sends): Human (1) Warrior (1) may pick deity 396 (Agnostic), 207, 212, 211 with start zone 1 (qeynos); 396, 206, 211, 208, 204 with 10 (freporte); 201 with 45; and (expansion bit 2048) 201/204/206/207/208/211/212/396 with 394 (crescent). Allocation 58: base STR 85, DEX 75, AGI 80, STA 85, INT 75, WIS 75, CHA 75, default 25 points into STA.

## 4. Zone packets

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| ZoneEntry | C→Z | 0x5089 | 76 B: u32 eq_crc32(name), `char[64]` name, u32 spell CRC, u32 spell-assoc CRC (EQEmu reads only the name) |
| ZoneEntry / NewSpawn | Z→C | 0x5089 / 0x6097 | one spawn per packet (below) |
| PlayerProfile | Z→C | 0x6506 | variable (below) |
| NewZone | Z→C | 0x1795 | 948 B NewZone_Struct; decoded: names (+0/+64/+192), ztype +470, fog RGB +471/+475/+479 (×4), fog min/max clip +484/+500 (4 floats each), gravity +516, time type +520, rain/snow +521..+536, sky +570, exp mult +584, safe y/x/z +588/+592/+596, max z +604, underworld +608, minclip/maxclip +612/+616, short name 2 +704, zone id/instance +852/+854, underworld teleport +868, suspend buffs +876, lava +880/+884, no bind +889, no attack +890, fall damage +894, fast regen +896.., fog density +916, no levitate +940 |
| TimeOfDay | Z→C | 0x5070 | 8 B: u8 hour, minute, day, month, u32 year. The calendar advances from this reference by one game minute per three real seconds (hours 1..24, 28 days per month, 12 months per year), as the original's `EQWorldData::AdvanceTime` does (`net::advance_time_of_day`, docs/formats/sky.md) and EQEmu's `common/eqtime.cpp` agrees. `/time` and MQ `GameTime` read the advancing calendar; each packet replaces the reference, even when its fields are unchanged |
| Weather | Z→C | 0x661e | 12 B: u32 val1 (0xff), u32 type (0x31 rain, 2 snow), u32 mode |
| ReqClientSpawn, SendExpZonein, WorldObjectsSent, ClientReady, SendAATable, UpdateAA, Logout | C→Z | 0x35fa, 0x5f8e, 0x5ae2, 0x345d, 0x66b5, 0x66f0, 0x4ac6 | empty |
| ClientUpdate | C→Z | 0x7dfc | 46 B: u16 sequence, u16 spawn id, u16 vehicle id, 4 B, f32 Δx, u32 heading:12 (heading×4 mod 2048), f32 x, f32 Δz, f32 z, f32 y, u32 animation:10, f32 Δy, u32 Δheading:10 signed (×20) |
| ClientUpdate | Z→C | 0x7dfc | 24 B: u16 spawn id, u16 vehicle id, then 5 dwords: [0] y:19 at bit 12; [1] Δz:13 at 0, Δx:13 at 13; [2] x:19 at 0, heading:12 at 19; [3] Δheading:10 at 0, z:19 at 10; [4] animation:10 at 0, Δy:13 at 10 |
| DeleteSpawn | both | 0x7280 | Z→C 5 B: u32 id, u8 1; C→Z 4 B: u32 own spawn id (zoning and camp exit; the zone then drops the link). Received for the target: the target clears and auto attack stops (msgEQRmPlayer @0x4b8380). For the trade partner: the trade is cancelled and closed (CTradeWnd_CancelAndClose: OP_CancelTrade {partner, me}); EQEmu tells only the player who left |
| SpawnAppearance | both | 0x0971 | 8 B: u16 id (0 = me), u16 type, u32 parameter |
| ChangeSize | Z→C | 0x4707 | 16 B: u32 id, f32 size, u32 0, f32 1.0 (sent instead of SpawnAppearance type Size) |
| HPUpdate | Z→C | 0x2828 | 10 B: i16 id, u32 current, i32 max |
| MobHealth | Z→C | 0x37b1 | 3 B: u16 id, u8 percent |
| ChannelMessage | both | 0x2b2d | cstr sender, cstr target, u32 0, u32 language, u32 channel, 5 B, u32 skill, cstr message, 15 zero bytes (the server ignores everything after the message). Sent, the names are at most 63 characters and the message 2047 (SendNewText steve_strncpy's them into 0x40 and 0x800): EQEmu's DECODE strcpy's the names into `char[64]` on its stack |
| SpecialMesg | Z→C | 0x0083 | u8 speak mode, u8 journal mode, u8 language, u32 type, u32 target id, cstr sayer, 3×u32 loc, cstr message |
| FormattedMessage | Z→C | 0x1024 | u32 0, u32 eqstr id, u32 type, ≤9 cstr arguments, u8 0 |
| ZoneChange | both | 0x2d18 | 100 B: `char[64]` name, u16 zone, u16 instance, 2×u32, f32 y, x, z, u32 reason, i32 success, u32 |
| SendZonepoints | Z→C | 0x69a4 | u32 count; count × 32 B {u32 number (zone_points.number), f32 y, x, z, heading (target; 999999 / 999 = keep the player's), u16 zone, u16 instance, u32 (client: non-zero → teleport by index, opcode 0xcadf), u32}; plus one more 32 B entry the client ignores |
| RequestClientZoneChange | Z→C | 0x3fcf | 176 B: u16 zone, u16 instance, u32 0xffffffff, f32 y, x, z, heading, u32 type (0x0b), 144 B, u32 |
| Camp | C→Z | 0x28ec | u32 (0 char select, 1 desktop, 2 server); ignored by EQEmu |
| Save / SaveOnZoneReq | C→Z | 0x4a39 / 0x600d | 484 B: u32 `__MemChecker0` of the 476 bytes from +5, u8 kind (0x21 camp; `SavePCForce` 0), f32 y, x, z, i32 heading, 22 × 20 B armour records, u8 on a vehicle + 3 B, u32 minutes played, i32 primary / secondary actor (IT numbers), i32 air supply, 3 zero bytes. EQEmu's `Save_Struct` is 484 opaque bytes; both handlers call `Save()` |

### Combat (`net/zone_packets.h` "combat", `GameConnection::handle_combat`, rules in `game/combat.{h,cpp}`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| TargetMouse | C→Z | 0x075d | 4 B: u32 new target (0 = none). Sent on every target change |
| TargetCommand | both | 0x58e2 | 4 B: u32 target. The server echoes it when it accepts a target (and sends it for pets); refused targets get "You cannot see your target" |
| AutoAttack / AutoAttack2 | C→Z | 0x109d / 0x3526 | 4 B each: u32 1 on / 0 off; both sent when the attack flag changes. EQEmu reads byte 0 of AutoAttack and ignores AutoAttack2 |
| Consider | both | 0x742b | 20 B: u32 player, u32 target, u32 faction, u32 level, u8 pvp, 3 B. C→Z fills the ids; Z→C: faction 1..9 already reordered for eqstr 12212.. (the server swaps 6↔9, 7↔8), level = the server's con colour (2/4/6/10/13/15/18) |
| Damage | Z→C | 0x6f15 | 30 B packed: u16 target, u16 source, u8 type, u32 spell, i32 damage, f32 force, f32 hit heading, f32 hit pitch, u8 secondary, u32 special (1 wild rampage, 2 rampage). damage 0 miss, −1 block, −2 parry, −3 riposte, −4 dodge, −5 invulnerable, −6 rune; type = `SkillDamageTypes` (0 crush, 1 slash, 4 hand to hand, 7 archery, 8 backstab, 10 bash, 21/23 strike, 30 kick, 36 pierce, 51 throw, 74 frenzy, 231 spell) |
| Death | Z→C | 0x6517 | 32 B, **not translated**: the server's Death_Struct — u32 spawn, killer, corpse id, bind zone (+12), spell (+16), attack skill (+20, a damage type; 0xfa/0xfb/0xfc burn/choke/fall), damage (+24), unknown. rof2_structs.h's Death_Struct (skill at +12) is not what goes out; this client reads skill at +20 |
| HPUpdate | Z→C | 0x2828 | 10 B (above). Own HP (cur, max) at zone-in and on change, without the items' HP (EQEmu `SendHPUpdate`: `GetHP() - itembonuses.HP`); the client adds it back for everything that shows or reports it (`GameConnection::own_hp`, docs/mechanics.md) |
| MobHealth | Z→C | 0x37b1 | 3 B: u16 id, u8 percent; for the current target (and group, pets) |
| ManaChange | Z→C | 0x5467 | 20 B: u32 mana, u32 endurance, u32 spell, u8 keep casting, 3 B, i32 slot |
| ManaUpdate / EnduranceUpdate | Z→C | 0x3791 / 0x5f42 | 10 B: u32 current, u32 max, u16 spawn id (the only source of the maxima: the RoF2 profile has none) |
| ExpUpdate | Z→C | 0x20ed | 8 B: u32 experience into the level on the 0..330 scale, u32 tip flag (the original shows the experience tip when it is 2; it is not AA experience, which only OP_AAExpUpdate carries; EQEmu sends 0) |
| LevelUpdate | Z→C | 0x1eec | 12 B: u32 level, old level, exp |
| SimpleMessage | Z→C | 0x213f | 12 B: u32 eqstr id, u32 colour, u32 unknown ("You gain experience!!") |
| Animation | Z→C | 0x7177 | 4 B: u16 spawn, u8 action, u8 speed (rof2.cpp swaps action and speed relative to the server struct). Action n is the n-th clip of C01..C11, D01..D05, L01..L09, O01, S01..S05, P01..P07, T01..T09, S06..S28, P08, O02, O03 |
| SpawnAppearance (sit) | both | 0x0971 | 8 B: u16 own id, u16 14, u32 110 sit / 100 stand (102 freeze, 105 loot, 111 crouch, 115 lying). The server checks the id is the sender's; it is not echoed to the sender |
| ZonePlayerToBind | Z→C | 0x08d8 | u16 zone (0 = this zone), u16 instance, f32 x, y, z, heading, cstr zone name, u8 1, 3×u32 0 |
| RespawnWindow | Z→C | 0x0ecb | u32 selection, u32 ms left, u32 0, u32 count, count×{u32 id, u32 zone, f32 x, y, z, heading, cstr name, u8 validity (0 available, nonzero unavailable)}, optionally followed by legacy count + 1 zero padding bytes |
| RespawnWindow | C→Z | 0x0ecb | 4 B: u32 chosen option id |

Observed sequences (EQEmu server, `playprobe --fight-nearest`):
- Targeting: C→Z `OP_TargetMouse` id. The server then sends `OP_MobHealth` for that target as its HP changes.
- Consider: C→Z `OP_Consider` {me, target}; Z→C `OP_Consider` with faction and colour. The client prints the line (eqstr 12239 "%1 %2 -- %3"), coloured by its own con level.
- Melee: C→Z `OP_AutoAttack` 1 + `OP_AutoAttack2` 1. The server swings on its own timer when the player is in range (`CombatRange`), sees the target (LOS) and **faces** it (`IsFacingMob`); out of range it sends "Your target is too far away, get closer!". Each swing: Z→C `OP_Damage` (to both sides), `OP_Animation`, `OP_MobHealth`; hits on the player also `OP_HPUpdate`. Client-built lines: "You slash a rodent for 6 points of damage.", "A rodent bites YOU for 5 points of damage.".
- NPC death: Z→C `OP_Death` (the spawn keeps its id as the corpse), then `OP_ExpUpdate` and the experience message. The client prints "You have slain a rodent!", drops the target and stops attacking.
- Player death (Character:RespawnFromHover off): Z→C `OP_Death` with the player as victim, then `OP_ZonePlayerToBind` with zone 0 (bind in this zone). The client answers C→Z `OP_ZoneChange` {zone 0, instance, the given position, reason 0}; the server resolves zone 0 to the bind point (`Handle_OP_ZoneChange`, zone mode ZoneToBindPoint) and answers success 1, and the client re-enters the zone through world like any zone change. With hover respawn on, the server sends `OP_RespawnWindow` instead and waits for the chosen option (or picks option 0 itself when the timer runs out).

Hover completion (EQEmu source sequence, covered by the loopback tests in `tests/hover_respawn_test.cpp`):
- Same zone and instance: `OP_ZonePlayerToBind`, then a living own `OP_ZoneEntry` with a new entity ID, authoritative buffs and HP. The previous ID remains the corpse. Movement stays blocked between the bind acknowledgement and own publication; other players, NPCs and stale records for the corpse ID cannot release that wait. The app consumes the new spawn's position and heading once and resets its movement body, held controls and fall state without loading the zone again.
- RoF2's bind packet contains zero HP/mana/endurance placeholders. During same-zone hover recovery the client preserves current mana/endurance (including zero), applies explicit nonzero values if supplied, and accepts the server's resource updates before or after the bind packet. While the live entity is unpublished, mana/endurance updates can use entity ID zero. HP and buffs after publication belong to the new player ID.
- Another zone or instance uses the normal zone transfer and profile reload. Countdown expiry remains the server's decision; the client does not guess a destination. The selection is sent once, only for an available ID from the current window. Malformed windows preserve the valid current window and cannot unlock a selection.
- A resurrection uses the ordinary `OP_RezzRequest` / `OP_RezzAnswer` confirmation, even when no refreshed respawn window arrives. While hovering, Yes to an offer whose zone dword (+4, zone and instance) is the current zone records consent locally and enables the reserved final resurrection option, leaving the selection where it was; it sends no acceptance packet and does not restart the countdown (ConfirmationDialog_HandleResponse @0x52d540 case 0x67). Yes to an offer naming another zone or instance is echoed in `OP_RezzAnswer` at once. A subsequent destination selection sends `OP_RespawnWindow {option_id}` for either bind or resurrection. This preserves the native hover publication path on stock EQEmu; sending an ordinary `OP_RezzAnswer` acceptance at the prompt would commit immediately on some servers. No echoes `OP_RezzAnswer` decline immediately and leaves bind usable. Outside hover, the ordinary resurrection answer still sends `OP_RezzAnswer` at once. Both UI packs keep the original respawn window drawn alongside the question, and put the question in front once, as it appears (the `ResurrectionOffer` child of ConfirmationDialogBox tells them it is the resurrection offer). Replacement ownership remains registered so the fallback death panel cannot take over. EQEmu reserves the final row for resurrection and numbers rows from zero; local consent is associated only with that verified ordering, never with translated names or the availability byte. A changed reserved destination or newer offer invalidates that consent. New death, completion, zoning and disconnect discard stale resurrection questions without answering them.

### Items, inventory and loot (`net/item_packets.h`, `GameConnection` in `net/game_connection_items.cpp`, rules in `game/inventory.{h,cpp}`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| CharInventory | Z→C | 0x5ca6 | u32 count, count serialized items (below); 4 zero bytes when empty. Sent during zone-in, before the player's spawn is complete |
| ItemPacket | Z→C | 0x368e | u32 ItemPacketType (0x64 merchant, 0x65 trade view, 0x66 loot, 0x67 trade/resync, 0x69 inventory, 0x6a limbo = summoned onto the cursor, 0x73 parcel), one item; a parcel adds u32 time, u32 n + name, u32 n + note |
| ItemLinkResponse | Z→C | 0x70c0 | same as ItemPacket with type 0 (`SendItemPacket(0, inst, ItemPacketViewLink)`) |
| MoveItem | both | 0x32ee | 28 B: InventorySlot from, InventorySlot to, u32 count. InventorySlot = i16 type (0 possessions, 1 bank, 2 shared bank, 3 trade, 4 world, 5 limbo, 9 merchant, 11 corpse), i16, i16 slot, i16 bag sub-slot (-1), i16 augment (-1), i16. Client: count 0 = move/swap a whole item, a count only for stack merges/splits; to = {-1, -1, -1, -1, -1} destroys (`DestroyHeldItemOrMoney`). Server: to invalid deletes (count = charges for OP_DeleteCharge), else a swap/merge |
| DeleteItem | both | 0x18ad | same 28 B; the server's resync sends {slot, invalid, 0xffffffff}; the client takes -count off the stack and removes an emptied one |
| DeleteCharge | Z→C | 0x01b8 | encoded as OP_MoveItem |
| MoneyUpdate | Z→C | 0x640c | 16 B: u32 platinum, gold, silver, copper carried |
| LootRequest | C→Z | 0x0adf | 4 B: u32 corpse spawn id. The server echoes it after the item packets |
| MoneyOnCorpse | Z→C | 0x5f44 | 20 B: u8 response (0 someone else, 1/3 ok, 2 not now, 4 hostiles, 5 too far, 6 all items sent), u8 0x42/0x5a, u8 0xef/0x40, u8, u32 pp, gp, sp, cp (the coin is already added to the player) |
| LootItem | both | 0x4dc9 | 20 B: u32 corpse, u32 looter, u16 corpse slot (23..56, the header slot of the loot item packet, type 11), u16, i32 auto loot (1 = into the inventory, 0 = cursor; the server refuses a cursor loot while the cursor is full), u32. Echoed when looted; then the item arrives as an item packet |
| EndLootRequest | C→Z | 0x30f7 | 4 B: u32 corpse id; answered with an empty OP_LootComplete (0x55c4) |
| ItemLinkClick | C→Z | 0x4cef | 52 B (ItemViewRequest_Struct): u32 item id, 6×u32 augments, u32 link hash, u32 flags (4 \| shift 2 \| ctrl 8 \| alt 0x10), u32 is evolving, u32 evolve group, u32 evolve level, u32 ornament icon |
| AugmentItem | C→Z | 0x661b | 40 B: u64 host serial (instance id, parcel id; ignored by the server), InventorySlot host, u32 socket 0..5, InventorySlot distiller (-1, -1, 0xffff and 2 unwritten bytes when none), u32 action (0 insert, 1 remove, 2 swap, 3 destroy). The cursor augment is never named. No reply packet: the server answers with OP_ItemPacket 0x67 (the host rebuilt, same serial), OP_MoveItem / OP_DeleteItem, OP_ItemPacket 0x6a (an augment back on the cursor) or red chat |
| AugmentInfo | both | 0x0afb | 76 B: u32 item id, u32 window, `char[64]` name, u32 (EQEmu unknown072). Request: the augment's SolventItemID and the display's index, the rest 0; reply: the solvent's name (shown as eqstr 5881; 13325 when the last field is 55, never from EQEmu) |

**Serialized item** (`SerializeItem`, all `#pragma pack(1)`; every field read one by one in `read_item`, and `write_item` reproduces the server's bytes exactly, which the live fixture test checks):

1. ItemSerializationHeader, 77 B: `char[17]` serial ("%016d"), u32 stack count, u32, u8 slot type, u16 slot, u16 sub-slot, u16 augment slot (the item's own InventorySlot; corpse items type 11), u32 price, u32 merchant slot (1 unless a merchant item), u32 scaled value, u32 instance id (the serial number), u32 parcel item id, u32 last cast time, u32 charges (0xffffffff unlimited), u32 attuned, 3×u32, u8 is evolving.
2. If evolving: 25 B (u32 final id, i32 level, f64 progress, u8 activated, i32 max level, 4 B).
3. Two cstr ornament models ("IT<n>" or empty).
4. ItemSerializationHeaderFinish, 26 B: u32 ornament icon, i32 -1, u32 hero model, i32, u8 copied, i32 -1, i32, u8 item class (0 common, 1 container, 2 book).
5. cstr name (omitted entirely, without terminator, when empty), cstr lore, cstr IDFile, cstr "" (always empty).
6. ItemBodyStruct, 255 B (id +0, weight +4, nodrop +9 (0 = No Trade), size +11, equip slots +12, value +16, icon +20, stats +37.., HP +44, AC +56, classes +72, races +76, deity +80, req level +121, delay +142, damage +146, color +150, item type +158, material +159, ...), cstr charm file.
7. ItemSecondaryBodyStruct, 74 B (aug type, aug restrictions, 6 sockets of 6 B, LDoN, bag type/slots/size/weight reduction +68..+71, book, book type), cstr book file.
8. ItemTertiaryBodyStruct, 76 B (lore group +0, ..., max stack +50, no transfer +54, ...).
9. Six spell blocks (click, proc, worn, focus, scroll, bard): 30 B each (i32 spell, u8, u32 type, u8 level, 5×u32 — click: max charges, cast time, recast delay, recast type), cstr name (always empty for bard), u32.
10. ItemQuaternaryBodyStruct, 171 B (script, quest, power, purity, backstab, DS mitigation, 13 heroic stats +22.., heal amount, spell damage, clairvoyance, subtype, heirloom +95, placeable +96, u32 -1 at +124 and +130, no zone +160, no ground +165, u8 1 at +170 (0 for ItemPacketInvalid)).
11. u32 content count, then per content u32 index in the bag and the content item (recursively). Every content's header carries the bag's first sub-slot ({type, bag slot, 0}); the index is the real position.

**Observed sequences (EQEmu server, `playprobe --move-test 1 --link-test 1 --loot 1 --destroy-test 1`):**
- Zone-in: OP_CharInventory with the four starting items of a human warrior (3,772 bytes); a summoned "A Faded Writ" then came as an item packet onto the cursor/general slot.
- Moves: pick up (from → 33, count 0), put down (33 → slot, count 0), unequip and re-equip the primary weapon, and a swap with a full cursor: the server sends nothing back for a good move, and the next login shows the moved items where the client put them. Destroy (33 → invalid) is permanent.
- Links: a said link (`make_item_link`, 8-digit body) comes back in the echoed OP_ChannelMessage; clicking it (OP_ItemLinkClick, hash 0) answers OP_ItemLinkResponse (939 bytes for a Short Sword).
- Looting an NPC corpse right after the kill: OP_MoneyOnCorpse response 1 (no coin), the OP_LootRequest echo, OP_MoneyOnCorpse response 6, then OP_EndLootRequest → OP_LootComplete.

### Merchants, NPC dialogue and trade (`net/merchant_packets.h`, `net/trade_packets.h`, `GameConnection` in `net/game_connection_commerce.cpp`, prices in `game/merchant.{h,cpp}`)

None of these has a RoF2 translator except OP_ShopPlayerBuy/Sell (field copies) and OP_CancelTrade, so the layouts are rof2_structs.h's.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| ShopRequest | C→Z | 0x4fed | 24 B MerchantClick_Struct: u32 merchant, u32 me, then 16 bytes the RoF2 client leaves unset (we send command 1, rate 0, tabs 0, -1). Sent on a right click on a class-41 NPC within 20 units, with no merchant open |
| ShopRequest | Z→C | 0x4fed | same: u32 merchant, u32 0, u32 command (1 open, 0 refused), f32 rate = 1 / (BuyCostMod × CalcPriceMod), i32 tabs (1 buy/sell, 2 recover, 4 parcels), i32. Then the stock as item packets type 0x64 (header slot type 9, slot = instance id = merchant slot, price = per-unit copper, merchant_slot = stock, -1 unlimited) |
| ShopPlayerBuy | C→Z | 0x0ddd | 32 B Merchant_Sell_Struct: u32 merchant, u32 me, u32 item slot (the item's instance id) + u32 high dword (0), u32 quantity, 12 bytes unset (0). Refused locally with an item on the cursor (8548) or too little money (12060/12061) |
| ShopPlayerBuy | Z→C | 0x0ddd | same, quantity bought, u32 total price at +24. The item arrives first as an item packet type 0x67; there is **no** OP_MoneyUpdate: the client takes the price itself (smallest coins first, so its coin split differs from the server's while the total agrees) and prints 1437 "You give%1 to %2." |
| ShopPlayerSell | C→Z | 0x791b | 20 B Merchant_Purchase_Struct: u32 merchant, TypelessInventorySlot {i16 slot, i16 bag sub-slot, i16 augment, i16}, u32 quantity, u32 price (unset by the client; 0). Refused locally: not droppable 12062, a non-empty bag 12064, augmented 5360, a sale pending 12063 |
| ShopPlayerSell | Z→C | 0x791b | same with the copper paid; slot -1 = refused (1364). The sold count leaves the stack (no item packet), 1361 "You receive%1 from %2 for the %3(s).", then OP_MoneyUpdate with the new coin |
| ShopEnd | C→Z | 0x30a8 | 8 B: u32 merchant, u32 me; answered by an empty OP_ShopEndConfirm (0x3196) |
| ShopDelItem | Z→C | 0x724f | 12 B: u32 merchant, u32 player, u32 merchant slot (sold out) |
| TradeRequest | both | 0x77b5 | 8 B: u32 the other side, u32 the sender. C→Z on a left click on a spawn while holding an item (within AvatarHeight + 20, 12294; banker-type classes 40/63/64/66/74 refuse, 8505). Z→C (a player asks): the client opens its trade window at once and answers the Ack |
| TradeRequestAck | both | 0x14bf | 8 B: {the other side, the sender}. For an NPC the server answers it itself ({me, NPC}); then the cursor item goes into the trade (OP_MoveItem cursor → {3, slot}, count 0; a stack merges first) and the give window (4 slots) opens; players get the trade window (8 slots) |
| TradeAcceptClick | both | 0x69e2 | 8 B: u32 partner, u8 ready (1), 3 unset bytes. The server forwards it to a player partner (byte +4 = their ready flag); for an NPC it finishes the trade |
| TradeCoins | Z→C | 0x4206 | 12 B: u32 the receiver's own id, u8 coin type (0 cp .. 3 pp), 3 B, u32 amount added (a delta) |
| MoveCoin | C→Z | 0x0bcf | 20 B: i32 from, i32 to (0 cursor, 1 inventory, 2 bank, 3 trade, 4 shared bank), i32 type from, type to, i32 amount. Coin goes into a trade in two moves, inventory → cursor and cursor → trade |
| CancelTrade | both | 0x354c | 8 B: C→Z {partner, me}; Z→C {the receiver's own id (player partner) or the NPC id, action}. On receipt the client prints 1448/1450 and sends its own cancel back while a partner is set. Refunds come as item packets and OP_MoneyUpdate, then OP_FinishWindow/2 (empty) |
| FinishTrade | Z→C | 0x3993 | empty: the trade window closes; received items follow as type 0x67 (or 0x6a onto the cursor) |
| TradeBusy | both | 0x5505 | 12 B: {requester, me, u8 reason ('b' 1444, 'c' 1445, else 1446), 3 B} |
| ItemLinkClick (say link) | C→Z | 0x4cef | EQEmu say links are item links to item 0xF423F with the phrase id in augment 1 (augment 2 for silent links); clicking one sends the ordinary OP_ItemLinkClick and the server says the phrase |

Hail (key H): says eqstr 13085 "Hail, %1" with the target's display name, or 13084 "Hail". NPC replies through SayString arrive as OP_FormattedMessage 554 "%1 says '%T2'", whose %T2 is an eqstr id expanded with the same arguments (`game/eq_strings.cpp`).

The sell price shown is (int)(value / rate + 0.5) × count, rounded to whole gold from 10,000,000 copper; 0 for Temporary, No Trade, attuned, ornamented or real-estate items and items holding such contents (`game::sell_value`). Buy prices are the item header's price field, unchanged.

Cursor queue: the server keeps the cursor as a queue and re-sends the head as a type 0x6a item whenever the cursor empties; the client places a 0x6a item only into an empty slot, so quest rewards and returned items appear one at a time.

Observed sequences (EQEmu server, `playprobe --shop/--hail/--handin/--trade-with/--trade-accept`, South Qeynos):
- Merchant: OP_ShopRequest → Z→C OP_ShopRequest {command 1, rate 1.0, tabs 1}, a FormattedMessage 554 greeting, then the stock (Voleen Tassen: 9 items, Water Flask 15 cp, Ration 38 cp). Selling the Short Sword: the client showed 68 cp, the server paid 68 (OP_ShopPlayerSell price 0x44, then OP_MoneyUpdate). Buying 2 Water Flasks: item packet 0x67 into General 5, then OP_ShopPlayerBuy price 30; the client took 6sp 10cp → 4sp 0cp. Selling both back: shown 20 cp, paid 20. A sold item shows up in the merchant's list as a limited item (stock 2). OP_ShopEnd → OP_ShopEndConfirm.
- Hail: "Hail, Tabure Ahendle"; his answer's [member of the Steel Warriors] is a say link (item 999999, phrase id 157); clicking it made the server say the phrase and Tabure answered.
- Hand-in: OP_TradeRequest → OP_TradeRequestAck {me, Tabure}; OP_MoveItem cursor → trade 0; OP_TradeAcceptClick → OP_FinishTrade, item packet 0x6a "Dirty Training Tunic (Legendary)" onto the cursor, his speech and five faction messages.
- Player trade: the partner gets OP_TradeRequest, acknowledges, receives the offer as item packet 0x65 (slot 0) and the coin as OP_TradeCoins (+2 silver); both OP_TradeAcceptClick, then OP_MoneyUpdate, the item as 0x67 (stacked into an existing stack), OP_FinishTrade. A client that disconnects with an open give window gets its item back (server refund).
- On every camp the server sends OP_CancelTrade {own id, action 7}; with no trade open it is ignored.

### Bank, coin moves and stack splits (`GameConnection` in `net/game_connection_bank.cpp` and `net/game_connection_items.cpp`, rules in `game/bank.{h,cpp}`)

The bank window has no packets of its own: a right click on a banker (class 40) within 20 units opens the bank window locally ("%1 tells you, 'Welcome to my bank!'" 1465), and it closes itself with "Come back soon!" (12143) when a 500 ms check finds the banker gone or more than 20 units away. Bank items arrive with the inventory (InventorySlot type 1, slots 0..23; shared bank type 2, slots 0..1). Bank coin is in the player profile (`platinum_bank`.., `platinum_shared`) and is then kept by the client: OP_MoneyUpdate only carries the coin on the person. The server checks for a banker within 200 units (`GetClosestBanker`, `USE_NPC_RANGE2` = 40000, a squared distance) on every bank item and coin move.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| MoveCoin (bank) | C→Z | 0x0bcf | as above. Bank coin moves through the cursor: deposit = {1 → 0} (inventory money button and quantity) then {0 → 2 or 4, held type, button type, all held}; withdrawal = {2 or 4 → 0} then {0 → 1}. The client's internal coin indices run platinum 0 .. copper 3 and are reversed when the packet is built (wire 0 copper .. 3 platinum). Moving to a different type converts: going up by d sizes takes amount − amount % 10^d and adds amount / 10^d (the rest stays on the cursor); going down multiplies by 10^d; a move that would take the destination past 10^9 is refused. The server answers OP_MoneyUpdate |
| MoveItem (split) | C→Z | 0x32ee | {slot, cursor 33, quantity}: the quantity dialog's answer for less than the whole stack; the client clones the item with that count onto the cursor first. Taking the whole stack is an ordinary move (count 0) |
| MoveItem (merge) | C→Z | 0x32ee | {cursor, slot, count moved}: as much as fits into a matching stack (`ItemContainer_MergeStacks`); a full stack of the same item does not swap |

Quantity dialog: a click on a stack (not in a world container or on a corpse) opens it with max = min(stack count, max stack). Shift answers max at once, Ctrl answers 1, and a stack of one is taken at once; otherwise the slider starts at max.

Observed sequences (EQEmu server, `playprobe --bank any [--bank-keep 1]`, South Qeynos): 24 bank slots (expansion bit 10). A 1 cp deposit → OP_MoneyUpdate 4 cp (twice); the withdrawal → 5 cp. General → bank and back, the Auto button into the first free slot, general → shared bank slot 0 and back, a split of 1 off 5 stacked items into a free slot and the merge back: the server sent nothing back for any of them. After relogging, the bank held the kept coin (1 cp in the profile) and items (slots 0 and 1), and the split stacks (x4 and x1) were where the client put them. Walking 40 units away closed the window with "Come back soon!".

### Spells (`net/spell_packets.h`, `GameConnection` in `net/game_connection_spells.cpp`, rules and spell data in `game/spells.{h,cpp}`)

Spell data: the client reads `spells_us.txt` (237 '^'-separated columns, `game::SpellTable`). The server's table decides; the client's only gates and labels.

The online client never finishes a cast itself. It gates the cast, starts its own cast bar, locks the gem and the global recovery for 10 s, sends `OP_CastSpell` and waits; the server decides fizzles, interrupts, resists, mana and landing, and ends **every** cast with `OP_ManaChange`.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| CastSpell | C→Z | 0x1287 | 44 B: u32 gem (0..11, 12 item, 13 discipline, 255 AA), u32 spell, InventorySlot (12 B; a gem cast sends all −1 with the last word 0), u32 target spawn, u32 the client's CRC of its spell record, u8 free-target flag + 3 B, f32 y, x, z (target ring; 0) |
| BeginCast | Z→C | 0x318f | 10 B: u32 spell, u16 caster, u32 cast time ms (the server's, after its own modifiers; 1350 for a 1500 ms spell live). Sent to everyone near, the caster included |
| ManaChange | Z→C | 0x5467 | 20 B (above). keep casting 0 = the spell bar enable that ends the cast (slot = the gem, `FindMemmedSpellBySpellID`); keep casting 1 with slot −1 = the periodic mana/endurance update (`CheckManaEndUpdate`, twice per 6 s tick) |
| ManaChange | C→Z | 0x5467 | **empty**: interrupt my cast (`/stopcast`, StopSpellCast reason 0) |
| MemorizeSpell | both | 0x217c | 16 B: u32 slot (book slot for scribing, else gem), u32 spell, u32 scribing (0 scribe, 1 memorize, 2 forget, 3 spell bar refresh, server only), u32 reduction (ms off the recast). C→Z 0/1/2 after the client's own countdown; Z→C echoes 0/1/2 and sends 3 after each successful cast |
| InterruptCast | Z→C | 0x048c | u32 spawn, u32 eqstr id, then NUL-terminated arguments. To the caster: 8 B, e.g. 439 "Your spell is interrupted."; to others: the caster's name as %1 |
| Action | Z→C | 0x744c | 56 B (ActionAlt_Struct): u16 target, u16 source, u16 level, u32 0, f32 instrument mod, f32 force, f32 hit heading, f32 hit pitch, u8 type (231 spell), u32 damage (0), u16, u32 spell, u8 spell level, u8 effect flag (4 = landed), u8 gem, InventorySlot, u32 item cast type. Once when the spell goes off and again with flag 4 when it lands |
| Damage (spell) | Z→C | 0x6f15 | as above with type 231: the client prints the landing text from it (`ReportSuccessfulHit`): on itself CastOnYou, with damage "%1 You have been healed for %2 points." (4600/4601, negative damage) or "%1  You have taken %2 points of damage." (12203/12204); on others "<name><CastOnAnother>" |
| Buff | both | 0x659c | 100 B: u32 entity, SpellBuff (88 B: u8 effect type, u8 level, u8, u8, f32 bard modifier, u32 spell, u32 duration tics, u32 caster, u32 num hits, f32 y, x, z, u32, i32 slot data[12]), u32 slot, u32 fade. Z→C fade 1 removes the slot (sent twice, the second with spell 0xffffffff), 2 refreshes it |
| BuffCreate | Z→C | 0x3377 | u32 entity, u32 tic timer, u8 all buffs (1 = the whole list), u16 count, count × {u32 slot, u32 spell (0xffffffff = removed), u32 tics, u32 num hits, cstr caster}, u8 type (0 own window). Own buff window; slots 0..41 long, 42..61 short (song window), 62 discipline |
| TargetBuffs | Z→C | 0x4f4b | as BuffCreate, type 5 (PC) or 7 (NPC): the target window's buffs |
| BuffRemoveRequest | C→Z | 0x64f2 | 8 B: u32 slot, u32 entity (own id or the pet's): click a buff off. EQEmu fades it if it is not detrimental |
| DeleteSpell | both | 0x3358 | 8 B: i16 book slot, 2 B, u8 success (the echo: 1 = deleted), 3 B |
| LoadSpellSet | C→Z | 0x261d | 16 B: u8 forget[12] (non-zero = forget that gem), u32 count (12). Sent when a spell set is memorized (the spell book's sets, the gem menu, `/memspellset`; `net/game_connection_spell_sets.cpp`), forgetting the gems the set replaces |
| ColoredText | Z→C | 0x43af | u32 chat colour, text (EQEmu copies strlen bytes without a terminator; the struct padding usually supplies one). Buff fade texts |
| LinkedReuse | Z→C | 0x1619 | 12 B: u32 timer 1..19, u32 end, u32 start (seconds): a shared spell timer; tints the gems only |
| Stamina | Z→C | 0x2a79 | 8 B: u32 food, u32 water |
| SpellEffect | Z→C | 0x5936 | 28 B: u32 effect (a spellsnew.eff index, not a spell), u32 caster, u32 target, u32 stage 1 start ms, u32 stage 2 after it ms, u32 stage 2 length ms, u8 full (emitters at density and opacity 1), u8 add (0 removes), u16 unread. Add: the effect's cast stage at once, stage 1 and stage 2 as their starts pass (`SpellEffectTimers`); remove: the timers dropped and the emitters stopped. EQEmu sends a spell's nimbus (spells_new column 193) as {500, 0, 3000, 1, 1} when it is cast |
| AddNimbusEffect | Z→C | 0xc693 | 8 B: u32 spawn, i32 effect: the effect's impact stage on the spawn (`Mob::AddNimbusEffect`, quests only) |
| RemoveNimbusEffect | Z→C | 0x7b1e | 8 B: u32 spawn, i32 effect: stops the effect's emitters on the spawn. EQEmu sends it when a buff with a nimbus fades |

Observed sequences (EQEmu server, `playprobe --spells 1` with a level 1 human cleric):
- Scribing: the scroll goes onto the cursor (C→Z `OP_MoveItem` general slot → cursor), "Beginning to scribe %1..." (12049); after the client's countdown (92 ticks, 3 per frame at most every 2 ms of the display clock) C→Z `OP_MemorizeSpell` {book slot, spell, 0}. The server deletes the scroll and echoes `OP_MemorizeSpell` {slot, spell, 0}; the client writes the book and prints 12006. The book only works sitting (the click handler checks StandState 110 or a mount).
- Memorizing: "Beginning to memorize %1..." (12053); the client counts 92 ticks down, one step every max(100, (level needed − level + 5) × 100) ms, 1 + max(Meditate, 10) / 10 + 2 ticks per step (a profile bard: (DEX + INT) / 30 + 1 instead of the 1). Minor Healing at level 1 took 11.5 s. Then C→Z `OP_MemorizeSpell` {gem, spell, 1}; the echo fills the gem (12007) and starts its refresh at RecastTime + 1000 ms.
- Casting: C→Z `OP_CastSpell`. A fizzle: Z→C `OP_ManaChange` {keep 0, slot = gem} twice and "Your spell fizzles!" (an `OP_SimpleMessage`), no `OP_BeginCast`. A cast: Z→C `OP_BeginCast` {1350 ms}, then at the end `OP_Action` (flag 0), `OP_Action` (flag 4), `OP_Damage` {type 231, damage 0} ("You feel a little better."), `OP_ManaChange` {keep 0, slot = gem} and `OP_MemorizeSpell` {gem, spell, 3, reduction 0}.
- `/stopcast` 300 ms into the cast: C→Z empty `OP_ManaChange`; Z→C `OP_InterruptCast` {me, 439} and `OP_ManaChange` {keep 0, slot = gem}.
- Moving 10 units during the cast: the server lets the cast run and refuses it at the end: `OP_InterruptCast` {me, 439}, `OP_ManaChange` {keep 0}. The client never interrupts itself online.
- Forgetting (right-click a gem): C→Z `OP_MemorizeSpell` {gem, spell, 2}; the echo empties the gem ("You forget %1.", 12496).
- Zone-in: `OP_BuffCreate` {all 1} with the buffs, then each buff that lands again as `OP_Action` ×2 + `OP_BuffCreate` + `OP_Damage`. Every 6 s `OP_Buff` (fade 2) + `OP_BuffCreate` (all 0) refresh each buff's tics.

### Groups (`net/group_packets.h`, `GameConnection` in `net/game_connection_group.cpp`, state in `game/group.{h,cpp}`)

Server side: zone `Handle_OP_Group*` (client_packet.cpp), `Group::*` (groups.cpp), `Client::GroupFollow`, `SendGroupCreatePacket`, `SendGroupLeaderChangePacket`, `SendGroupJoinAcknowledge` (client.cpp), `ZoneDatabase::RefreshGroupFromDB`.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| GroupInvite | both | 0x6110 | 148 B: `char[64]` invitee, `char[64]` inviter, 5×u32 0. Sent by `/invite` when the player has no group; EQEmu forwards it to the invitee (also for GroupInvite2) |
| GroupInvite2 | C→Z | 0x32c2 | same 148 B; sent when inviting into an existing group |
| GroupFollow / GroupFollow2 | both | 0x1649 / 0x2060 | 152 B: `char[64]` inviter, `char[64]` invitee (the accepting player), 5×u32, u32 at +148 (the client copies the u32 at +148 of the invitation, past EQEmu's 148 bytes; sent as 0). Accept with GroupFollow when the invitation came as 0x6110, else GroupFollow2. EQEmu echoes it to the inviter |
| CancelInvite | C→Z | 0x2a50 | 152 B: `char[64]` inviter, `char[64]` the declining player, 20 B, u32 reason at +148 (3 declined with `/disband`, 1 already invited, 4/5 raid states). **EQEmu has no handler** (its OP_GroupCancelInvite is 0x0000 for RoF2, and OP_CancelInvite is not one of its opcodes): on stock EQEmu the inviter is never told. The Hailstone server script (`server/`, setting `group_declines`) passes it on: the packet itself to an inviter in the same zone, else the line as text |
| CancelInvite | Z→C | 0x2a50 | the same 152 B (from the server script): Handle_OP_CancelInvite @0x524d90 prints the line for the reason byte at +148 with the name at +64, colour 0x111: 1 12264, 2 12265, 3 12266 "%1 rejects your offer to join the group.", 4 5086, 5 5088; other reasons nothing |
| GroupDisband | C→Z | 0x4c10 | 148 B GroupGeneric: `char[64]` name1, `char[64]` name2, 20 B. A member leaving: {me, me}; a leader removing a member: {member, leader}. EQEmu decides from the sender's target: a leader with no target disbands the group, with a member targeted removes it |
| GroupDisbandYou | Z→C | 0x1ae5 | 148 B GroupGeneric {you, member}: you left or the group was disbanded. "You have been removed from the group." (12001); the group is deleted |
| GroupDisbandOther | Z→C | 0x74da | 148 B GroupGeneric: name2 left |
| GroupUpdate | Z→C | 0x3abb | 148 B (RoF2 GroupJoin_Struct): `char[64]` owner (mercenaries), `char[64]` member, u8 merc, 3 B, u32 level, 12 B. EQEmu fills only the member name. A member joined (EQEmu also sends it for leader changes and leadership-AA updates; the client adds only unknown names) |
| GroupUpdateB | Z→C | 0x6194 | u32 group id (0), u32 count, cstr leader; count × {u32 slot, cstr name, u16 merc, cstr owner, u32 level, u8 tank, u8 assist, u8 puller, u32 offline, u32 timestamp}. EQEmu: the receiver in slot 0, levels 0x46/0x41 (fake); "group created" (`SendGroupCreatePacket`, the inviter only) has an empty leader and one member with the real level. The client rebuilds every slot |
| GroupLeaderChange | Z→C | 0x21b4 | 148 B: 64 B, `char[64]` leader, 20 B |
| GroupAcknowledge | Z→C | 0x7323 | 4 B: "You have joined the group." (12004). Sent to both sides when a group forms |
| GroupLeadershipAAUpdate | Z→C | 0x02cf | leadership AAs; sent after every group update; the group leader's ability ranks and NPC marker are kept (`GameConnection::handle_leadership_ability`) |
| GroupMakeLeader | C→Z | 0x4229 | 456 B: u32 8, `char[64]` current leader (me), `char[64]` new leader, 324 B 0. The client then sends GroupMentor {0, ""} |
| GroupMentor | C→Z | 0x3342 | 68 B: i32 percent, `char[64]` name ("" clears) |
| GroupRoles | both | 0x70e2 | 148 B: `char[64]` member, `char[64]` leader, 3 × u32, i32 role at +0x8c (1 Main Tank, 2 Main Assist, 3 Puller), u8 on at +0x90 (0 clear, 1 assign), 3 B padding. The group leader's menu and `/grouproles` send requests; server replies update the member's flags and clear the previous holder of an assigned role. Requests leave the local flags unchanged until a server update |
| MobManaUpdate / MobEnduranceUpdate | Z→C | 0x2404 / 0x1c81 | 3 B: u16 spawn, u8 percent; group members' mana and endurance |
| MobHealth | Z→C | 0x37b1 | 3 B (above); group members' HP percentages come this way too |

Observed sequences (EQEmu server, South Qeynos; the GUI client and `playprobe` as the second player):
- Invite into no group: C→Z `OP_GroupInvite` (148 B); the invitee gets `OP_GroupInvite` (148 B, no +148 word). Accepting (`OP_GroupFollow`, 152 B): the inviter gets `OP_GroupUpdateB` {leader "", 1 member, real level}, `OP_GroupLeaderChange`, `OP_GroupAcknowledge`, `OP_GroupUpdate` {joiner, level 0}, `OP_GroupLeadershipAAUpdate` (320 B), `OP_MobHealth`/`OP_MobManaUpdate`/`OP_MobEnduranceUpdate` for the joiner, `OP_GroupUpdateB` {leader, 2 members, levels 70/65}, and the `OP_GroupFollow` echo. The invitee gets the three Mob* updates, `OP_GroupAcknowledge`, `OP_GroupUpdateB` {leader, itself in slot 0, levels 70/65}, `OP_GroupLeadershipAAUpdate`.
- `/g` reaches both; a kill by either gives "You gain party experience!!" to both.
- `/makeleader`: everyone gets `OP_GroupLeaderChange` {new leader}, `OP_GroupUpdate` {new leader} and `OP_GroupLeadershipAAUpdate`.
- Camping or dropping the link (two-member group): the other member gets `OP_GroupDisbandYou` {self, ""} and `OP_GroupLeadershipAAUpdate`.

### Pets (`net/pet_packets.h`, same files)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| PetCommands | C→Z | 0x0159 | 12 B: u32 command (EQEmu PET_*: 0 health, 1 leader, 2 attack, 3 qattack, 4 follow, 5 guard, 6 sit (7 on, 8 off), 9 stop, 12 taunt (13/14), 15 hold (16/17), 18 ghold, 21 spell hold, 24 focus, 27 feign, 28 back off, 29 get lost, 31 regroup), u32 target (attack / qattack: the current target if not yourself; leader: the target if it is an NPC; else 0), u8 flag 0, 3 B. `/pet target` (30) targets the pet locally and sends nothing; an attack without a target sends nothing. EQEmu ignores a command whose target is the pet itself |
| PetCommandState | Z→C | 0x1dc8 | 8 B: u32 button (EQEmu PET_BUTTON_*: 0 sit, 1 stop, 2 regroup, 3 follow, 4 guard, 5 taunt, 6 hold, 7 ghold, 8 focus, 9 spell hold), u32 state |
| PetHoTT | Z→C | 0x794a | 4 B: the focused pet's target |
| PetBuffWindow | Z→C | 0x5882 | the OP_BuffCreate layout (u32 pet, u32 0, u8 1, u16 count, entries, u8 0) |
| SpawnAppearance type 25 | Z→C | 0x0971 | the spawn's owner (MasterID); 0 when a charm breaks |

### Skills and abilities (`net/ability_packets.h`, rules in `game/mechanics/abilities.{h,cpp}`, glue in `app/actions_ui.cpp`)

The client decides locally whether a skill may be used and sends one request; the server rolls the outcome and answers with chat, OP_Damage, OP_SpawnAppearance and so on. Layouts are checked against the server's structs and handlers.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| CombatAbility | C→Z | 0x3eba | 12 B: u32 target, u32 atk (100 combat skill, 11 ranged), u32 skill |
| Bind_Wound | C→Z | 0x0386 | 8 B: u32 target spawn id, u32 0 (BindWound_Struct to, unknown2, type, unknown6) |
| Disarm | C→Z | 0x5ec8 | 16 B: u32 source (me), u32 target, u32 the disarm skill value (profile +0x30e0), u32 0xff |
| PickPocket | both | 0x39e8 | 18 B: u32 target, u32 me, u32 pick pockets value, u32 0xff, u16 0 (PickPocket_Struct to, from, myskill u16, type, unknown, coin, lastsix) |
| Begging | both | 0x6703 | C→Z the same 18 B with the begging value; Z→C BeggingResponse_Struct (20 B, result at +12) |
| FeignDeath | C→Z | 0x52fa | 12 B: u32 me, u32 0 (unknown), u32 0. The client also lies down (stand state 115) and stops attacking |
| Taunt | both | 0x2703 | C→Z 4 B: u32 target (ClientTarget_Struct); Z→C empty (the server's acknowledgement) |
| RemoveTrap | C→Z | 0x71da | 4 B: u32 target |
| Animation | both | 0x7177 | 4 B: u16 spawn id, u8 action, u8 speed (RoF2 order); fishing sends action 5, speed 5 when standing |
| Hide, Sneak, Forage, Mend, SenseHeading, Fishing, InstillDoubt (intimidation), SenseTraps, DisarmTraps, LDoNPickLock, TrackUnknown | C→Z | 0x67fe, 0x5d55, 0x5306, 0x0ecf, 0x260a, 0x1e2a, 0x640e, 0x02af, 0x78bf, 0x9be3, 0x4577 | empty |
| Hide (cancel) | C→Z | 0x67fe | 4 zero bytes: the answer to the server's OP_CancelSneakHide |
| CancelSneakHide | Z→C | 0x0927 | empty: hide and sneak are over |
| SpawnAppearance (sneak / hide) | both | 0x0971 | type 15 (sneak) parameter 0/1 from the server sets the player's sneak flag; the client sends {me, 15, 0} to stop sneaking and {me, 3, 0} to unhide before a disarm |
| SkillUpdate | Z→C | 0x004c | 12 B: u32 skill, u32 value, 4 unknown bytes; the value goes into the profile's skills |
| Track | both | 0x17e5 | C→Z empty (the tracking window asks for the list); Z→C the list (decoded, sorted and filtered for the tracking window: `net/game_connection_tracking.cpp`); a first u32 of 0xff8b344f (`decode_track_reply_header`) is the tracking timer, not a list |

### Doors and ground objects (`net/door_packets.h`, `GameConnection` in `net/game_connection_doors.cpp`, rules in `game/doors.{h,cpp}`, drawing and collision in `app/world_objects.{h,cpp}`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| SpawnDoor | Z→C | 0x7291 | size / 100 records of 100 B: `char[32]` name (the actor is `<name>_ACTORDEF`), f32 y +32, x +36, z +40, heading +44, i32 incline +48 (DefaultDoorAngle, made a float), u16 size +52 (percent), 6 B, u8 id +60, u8 open type +61, u8 state +62, u8 invert +63 (DefaultState), i32 param +64 (lift travel, slide distance: "SpellID"), i32 +68 / +72 / +76 (adventure, dynamic-zone, real-estate door ids), u8 +80 has script, +81 usable, +82 remain open, +83 visible, +84 heading changed, 3 B, f32 +88 / +92 TopSpeed1/2, u8 +96 stamp the time, 3 B. EQEmu sets +81 and +83 to 1 ("both must be 1 to allow clicking doors") |
| ClickDoor | C→Z | 0x3a8f | 16 B: u8 door id, 3 B (unset on the wire; 0 here), i32 lock-pick skill (≤ 254), u32 key item id (the cursor item; -1 without one), u32 player spawn id |
| MoveDoor | Z→C | 0x08e8 | 2 B: u8 door id, u8 action = the new door state (2 opening, 3 closing; EQEmu sends 3/2 for inverted doors) |
| RemoveAllDoors | Z→C | 0x700c | empty |
| GroundSpawn | Z→C | 0x6fca | u32 drop id, cstr actor name (≤ 63 chars, e.g. `IT63_ACTORDEF`; EQG models through Resources/OnDemandResources.txt, e.g. `IT10804` in tradeskill_objects.eqg), u16 zone, u16 instance, u32 drop id again, u32 0, f32 heading, f32 tilt x, f32 tilt y, f32 size (scale), f32 y, x, z, i32 object type (1 = a temporary dropped item; tradeskill containers have their type) |
| GroundSpawn | C→Z | 0x6fca | empty: drop the cursor item on the ground |
| ClickObject | both | 0x4aa1 | 8 B: u32 drop id, u32 player spawn id. C→Z: pick up / open (LMouseUp, under 20 units). Z→C: the object is gone (picked up by someone, decayed); the item itself comes as an item packet of type 0x67 to the cursor |
| ClickObjectAction | Z→C | 0x0c1e | 92 B: u32 drop id (0xffffffff: someone else is using it), i32 -1, i32 -1, u32 container type, u32 0x0a, u32 icon, u32, `char[64]` name: a tradeskill container opened; its contents follow as item packets of type 0x6b (slot type 4, World) |
| ClickObjectAction | C→Z | 0x0c1e | empty: close the container (EQEmu `Object::Close`, which answers OP_ClearObject) |
| ClearObject | Z→C | 0x7a11 | 8 B (`decode_clear_object`): u8 clear, 3 B, u32 slot. Clear 0 removes the world container's item in that slot; anything else empties every slot of the world container, which EQEmu sends when it closes a container and after a combine emptied it; the window stays open |
| TradeSkillCombine | C→Z | 0x579a | 24 B: container slot (12 B InventorySlot; a world container is type 4, slot -1 = SLOT_TRADESKILL_EXPERIMENT_COMBINE), guild-tribute slot (all -1) |
| RecipesFavorite / RecipesSearch | C→Z | 0x71b1 / 0x6290 | 2008 B {u32 container type, u32 bag item id (0 world), 500 × u32 recipe ids} / 80 B {type, id, u32 min trivial, u32 max trivial, `char[56]` partial name, u32 0x300, u32}; one RecipeReply each |
| RecipeReply | Z→C | 0x1db6 | 84 B: type, id, u32 component count, u32 recipe id, u32 trivial, `char[64]` name |
| RecipeDetails | both | 0x6e02 | C→Z {u32 recipe id}; Z→C big-endian recipe id, (10 − n) × 0xffffffff, n × {BE item id, BE icon, name} |
| RecipeAutoCombine | both | 0x40d7 | 40 B: type, id, container slot (12 B, echoed), 12 B slot, u32 recipe id, u32 reply (0 made, 0xfffffff5 failed; the server says why) |

Door behaviour (all in `game/doors.cpp`): the state machine is 0 closed, 1 open, 2 opening, 3 closing, 4 a sprung trap; a spawned door starts in its default state (an "open" spawn state animates open). Every door moves each frame by elapsed ms × 0.02 ticks (nothing moves on a frame later than 15 ticks): hinged types 0–8 turn their heading 8 per tick through a quarter turn (types 5–7 the other way); trap doors and drawbridges (0x1e, 0x23, 0x24, 0x28, 0x82) turn the door angle; types 10–0x1b slide sideways 8/10/14/22 units at 0.35; lifts and portcullises (0x3b–0x54, 0x78, 0x8c–0x92) rise by their distance (the door param for 0x3b, 0x4b–0x54) at 0.45 per tick and come down at 0.25 or 0.45; spinners (100–102 → 0x9c, 105–107 → 0x9d, 0x73/0x74) accelerate to `TopSpeed`. A door that is not in its default state returns by itself after a repop time (10 s for types 0 and 5, 20 s for most). EQEmu's own close timer resets the server's door state silently, so the client's return is what closes ordinary doors. The click range: the 3D distance from the player to the door must not exceed the player's height + 20 + the door actor's bounding radius, else eqstr 1462 "You can't reach that, get closer." (EQEmu checks 100 units, `Range:MaxDistanceToClickDoors`). Teleport doors (types 57/58 with a destination) are moved by the server: OP_RequestClientZoneChange, which `GameConnection` already answers. The actor is placed at (Y, X, Z), turned by Heading (counterclockwise like spawns) and tilted by DoorAngle about its Y axis, scaled by size %.

### Alternate advancement, training, tradeskills and tasks (`net/aa_packets.h`, `net/training_packets.h`, `net/task_packets.h`, `GameConnection` in `net/game_connection_progress.cpp`, rules in `game/alt_adv.h`, `game/training.h`, `game/tasks.h`)

All tried live against an EQEmu server (playprobe `--aa`, `--aa-use`, `--train`, `--task`, `--task-cancel`, `--object`); a successful combine and the bag (inventory container) path only have unit tests: the test character had no tradeskill bag, components or coin.

**Alternate advancement.** After its own spawn arrives the client asks for the table (empty OP_SendAATable, only while the table is empty) and the owned ranks (empty OP_UpdateAA). The server answers with one OP_SendAATable per rank (each ability's owned rank and the next one, or rank 1; 150 to 470 packets, some repeated), OP_RespondAA and OP_AAExpUpdate.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| SendAATable | C→Z | 0x66b5 | empty |
| UpdateAA | C→Z | 0x66f0 | empty |
| SendAATable | Z→C | 0x66b5 | u32 rank id, u8 1, i32 hotkey sid, i32 hotkey sid 2, i32 title sid (dbstr type 1), i32 description sid (type 4), i32 level, i32 cost, i32 ability id (all ranks share it; EQEmu "seq"), i32 rank number; counted i32 array of prerequisite abilities (≤ 0: none; −1 / −2 the legacy 6 general / 12 archetype points), counted i32 array of their ranks; i32 type (window tab), i32 spell, counted i32 array of reuse timers (EQEmu: 1, spell_type), i32 reuse seconds, u32 classes (bit 1 << class), i32 max rank, i32 previous rank id, i32 next rank id (−1 last), i32 total cost; u8 (+0x50), u8 grant only, u8 ignores level (+0x84); u32 charges, i32 expansion, i32 category; 4 B (+0x90 monster only); u32 effect count, then {i32 SPA, base, limit, slot} |
| RespondAA | Z→C | 0x7a27 | 3624 B: u32 spent, 5 × u32 assigned (EQEmu: spent, 0, 0, 0, 0), 300 × {u32 rank id, u32 value, u32 charges}. The original reads at most 300 slots; Hailstone reads every slot the packet holds, for servers that send more |
| AAExpUpdate | Z→C | 0x7d14 | 12 B: u32 AA experience (0..330), u32 unspent points (EQEmu u16 + u16), u8 percent to AAs, 3 B |
| AAAction | C→Z | 0x424e | 16 B: u32 action, u32 rank id, u32 target, u32 value. 0 activate (the owned rank's id and the target's spawn id), 1 set the percentage (value), 2 AA experience off, 3 train a rank. EQEmu answers "Alternate Experience is *ON*/*OFF*" and OP_AAExpUpdate |
| AAAction | Z→C | 0x424e | 12 B: u32 begin, u32 timer, u32 server time (UseAA_Struct). The client stores timer = begin + (its time − the server's) when 0 ≤ timer < 100; activating sends 0, timer, 0 (the timer starts now) |
| ClearAA | Z→C | 0x0f50 | empty: the owned list is cleared (EQEmu's ResetAA) |

Readiness: reuse = the rank's seconds − TotalEffect(SPA 264, the ability id) when the rank has a reuse time; ready when reuse + the latest of its timers ≤ now. Trainability: not owned, enough unspent points, the spend limit, level, monster-only, class, not grant-only, prerequisites met, the expansion bit. The owned ranks' effects go into the character sheet (`SheetAbility`, CacheAAEffects).

**Training.** A right click on a guildmaster (class 20..35, within 20 units) whose class is the profile class + 19 (EQEmu with `Character:AllowCrossClassTrainers` shows every guildmaster as the viewer's class) turns auto attack off and sends OP_GMTraining; otherwise eqstr 1467.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| GMTraining | both | 0x1966 | 448 B: u32 guildmaster, u32 player, 100 × u32 skill limits (0 in the request), f32 greed +0x198 (EQEmu 1.08), u8 success +0x19c (EQEmu 1), 32 language bytes +0x19d (EQEmu 0xc9), 3 B |
| GMTrainSkill | C→Z | 0x2a85 | 12 B: u32 guildmaster, u32 1 for a language, u32 skill or language id |
| GMTrainSkillConfirm | Z→C | 0x4b64 | 76 B: u32 skill (100 + n for language n), u32 cost (copper), u8 newly learned, `char[64]` guildmaster, 3 B. The client takes the coin, one practice point, and prints 12066 (learned the basics), 12067 (fluency) or 12068 (skill) |
| GMEndTraining | C→Z | 0x4d6b | 8 B: guildmaster, player (the server answers OP_GMEndTrainingResponse and the guildmaster's goodbye) |
| SkillUpdate | Z→C | 0x004c | 12 B: u32 skill (100..131: language n − 100), u32 value, u8 announce (EQEmu 1), 3 B. With the announce byte the client prints 12091 "You have become better at %1! (%2)"; EQEmu also sends its own "(value/max)" line, so both appear |

The window's rows are the skills with a guildmaster limit above 0 that the class gets at some level (SkillCaps.txt), except 15, 33 and 72, then the languages the character may learn (a race × level table, with the necromancer, wizard, magician and enchanter exceptions). The cost of a point is (value − 10)³ / 100 copper.

**Tradeskills.** `combine_container` (a bag in a possessions slot) and `combine_world_container` run these checks: the class tomes (types 0x18..0x1b: wizard, magician, necromancer, enchanter; 12010) and racial forges, wheels and kits (0x1f..0x2d; 12011..12023), nothing on the cursor (12024), no stack of more than one (8504), something inside (12025); transformation molds (0x37, 0x38) ask first (6328 / 6339, ConfirmationDialog 0x86 with a copy of the request; a yes sends it, a no drops the pending request). OP_TradeSkillCombine (above) goes out; the server's empty OP_TradeSkillCombine ends the wait. The outcome comes as server messages and item packets (the result on the cursor; the bag's contents deleted). An open world container's slots take items like any other slot (OP_MoveItem with slot type 4, slots 0..9).

**Tasks.**

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| TaskDescription | Z→C | 0x3714 | u32 slot, u32 task id, u8 open journal, u32 type (0 task, 1 shared, 2 quest), u32 reward type; title; u32 duration, u32 duration code, u32 elapsed seconds (the RoF2 translator turns the start time into elapsed time); description; u8 has rewards, u32 coin, u32 xp, u32 faction; reward text; reward item link; u32 points; u8 reward selection |
| TaskActivity | Z→C | 0x08d3 | u32 slot, u32 type, u32 task id, u32 element, u32 step; short form (25 B, a hidden element): u32 0xffffffff, u8 optional; else i32 element type, u8 optional, i32 0, target name, counted item list, i32 goal, counted skill list ("-1"), counted spell list ("0"), zones, i32 dz switch, description override, i32 done, u8 1, zones again |
| CompletedTasks | Z→C | 0x4eba | u32 count, {u32 task id, title, u32 completed time} (the 50 most recent) |
| TaskSelectWindow / SharedTaskSelectWindow | Z→C | 0x705b / 0x48a2 | u32 count, u32 type, u32 giver spawn; per task: u32 id, f32 reward multiplier, u32 duration, u32 duration code, title, description, u8 reward preview, u32 elements, per element: i32 index, i32 type, i32 0, target, counted items, i32 goal, counted skills, counted spells, zones, description, zones (up to 40 tasks) |
| AcceptNewTask | C→Z | 0x0a23 | 12 B: u32 the selection's type, u32 task id (0 declines), u32 giver |
| SharedTaskAcceptNew | C→Z | 0x6646 | 20 B: 0, 0, u32 giver, u32 task id, f32 reward multiplier |
| CancelTask | both | 0x39f0 | 8 B: u32 slot, u32 type. C→Z abandons; Z→C removes the task |
| TaskHistoryRequest | C→Z | 0x5f1c | 4 B: the completed list index |
| TaskHistoryReply | Z→C | 0x3d05 | u32 index, u32 count; per completed element: u32 type, target, counted items, u32 goal, counted "-1", counted "-1", zones, u32 0, description, u8 '1', zones |
| TaskActivityComplete | Z→C | 0x5e19 | 24 B: index, type, task id, element, completed (0 failed), stage completed. The client shows 6005 "Task Stage Completed", or 6003 / 6004 with the title, as large on-screen text (the client prints them to chat) |
| TaskRequestTimer | Z→C | 0x7a48 | 4 B: milliseconds before another shared task request |
| TaskTimers | C→Z | 0x2b0f | empty (/tasktimers) |

The element text is the server's description, else the type's template (with / without a target: kill 3468, loot 3498 / 3470, deliver 3469, speak 6001, explore 6002, tradeskill 6048, forage 6049, fish 6050, cast on 6052 / 6051, use skill on 6054 / 6053, touch 6117, collect 611 / 610) with %1 the goal, %2 the target, %3 the items, %4 the spell names and %5 the skill names (";"-separated ids joined with " or ").

**Reward selection** (`net/reward_packets`, `game/reward_selection`). These are stock RoF2 managers, independent of a server's reward providers. Pending rewards use `0x6411` (`AchievementReward`); task previews use `0x6471` (`RewardSelection`) and cannot claim.

| Action | Direction | Layout after u32 action |
|---|---|---|
| 0 | Z→C | u8 has reward, then one reward set when true; pending upserts by pending id, preview replaces |
| 7 | Z→C | i32 set count, u8 display flags, then reward sets; an empty collection clears the view |
| 1 | C→Z | u32 set id, wire option id, entry id, item id |
| 1 | Z→C | u32 item id, then one serialized RoF2 item, without an ItemPacketType prefix |
| 3 | C→Z | pending id, set id, wire option id, then four zero bytes; pending manager only |
| 3 | Z→C | the same three ids, u8 success, three padding bytes |
| 4 | C→Z | u32 task id; preview manager only |
| 5 | C→Z | u32 achievement definition **index**, not achievement id; pending manager |
| 6 | C→Z | no body; explicitly restore pending rewards |

A set is pending id, set id, title C string, f32 display multiplier, repeated set id, i32 option count. Each option is u32 wire id, u32 entry count, u8 common-to-all, u8 flags, label C string, then typed entries. Each entry starts with u32 type, entry id, string id, description C string. Types are text (0), money (1: platinum/gold/silver/copper DWORDs), items (2: count and id/quantity/u8 lore/lore-group/name records), experience (3), alternate ability (4: rank and cumulative rank), AA points (5: amount and a byte flag), and generic points (11: four DWORDs, amount in the third and currency in the fourth). Unknown types reject the whole packet. Wire option ids belong to the received collection; they are not a provider's catalog ids. Pending action 0 with no reward does not erase unrelated tabs; preview action 0 clears the previous preview first. Action 7 replaces either lane. These distinctions were checked against the stock manager receivers (`0x4656e0` / `0x465a90`).

Numeric display applies the set multiplier separately to each signed 32-bit money denomination, the AA-points amount and the generic-points amount, truncating toward zero. Item quantities, alternate-ability ranks and descriptions are unchanged (`0x464290` / `0x464740` / `0x464900`). Raw unsigned wire values remain available in the API. A product outside the signed 32-bit display range is marked Unavailable instead of reproducing a platform-specific integer-conversion overflow. This affects presentation only; the server owns grants.

Host limits are 8 MiB per packet, 256 sets, 4096 options, 16384 entries and items, and 2 MiB aggregate text. Individual titles, option labels and item names are limited to 4096 bytes and descriptions to 16384. Decode is atomic. Inspection verifies the requested/header/serialized item id and uses the ordinary bounded item-display queue; it never changes inventory.

The UI captures process-unique revision and tab tokens before asking to claim or inspect. Requests reserve their ownership before outgoing extension callbacks and revalidate it before transport. One claim can be pending at a time. The host's conservative 10-second timeout marks a dispatched claim uncertain; replacing or restoring the collection does not authorize another claim or retry it. Only a matching terminal reply or session/zone retirement drains that reservation. An old reply cannot remove a tab from a newer collection. Item inspection replies have only an item id, with no request nonce: retired ids are held until their replies drain, and cannot be reused in the meantime (256 per lane). Closing, zoning and reconnecting retire view ownership. Getters and drawing never query or claim rewards.

**Achievements** (`net/achievement_packets`, `game/achievements`). The server sends definitions and authoritative character state; the client neither evaluates criteria nor awards points, completion or rewards locally. These stock managers do not require a server-specific addon or bundled catalog files. Keep the received definition order, including hidden definitions: an achievement id and a definition index are different identities.

| Packet | Direction | Opcode | Layout |
|---|---|---|---|
| Definitions | Z→C | 0xdab0 | u32 inflated length, exact zlib stream; inflated catalog below |
| Primary update | Z→C | 0x618f | u32 serial, u8 dense, u32 count; per row, u32 definition index only when sparse, then state |
| Progress | Z→C | 0x2b42 | u32 count, then 20-byte records: achievement id, component id, requirement id, requirement type, current count |
| Comparison snapshot | Z→C | 0x059d | one state for every received definition, no leading count or target identity; never initializes own state |
| Refresh | C→Z | 0x47cb | native one-byte 0x30 (servers may also accept an empty request) |
| Open window | Z→C | 0x171e | empty; opens the window without querying again |
| Compare request | C→Z | 0x1665 | u32 target spawn id, u32 requester spawn id |
| Link inspection | C→Z / Z→C | 0x675d / 0x28e0 | player name C string, u32 achievement id, state with counts, u8 trailing flag |
| Earned | Z→C | 0x17f8 | u32 achiever spawn id, achievement id, sound id, caret-delimited link metadata C string |
| Guild announcement | Z→C | GuildUpdate action 3 | action at 0, fixed 64-byte player name at 12, achievement id at 80, link metadata at 84 |

The inflated catalog starts with a category count and categories: id, parent id (`0xffffffff` for a root), name, description, icon C strings, display order, association count and `{achievement id, text C string, display order}` records, then child count and child ids. Next is a definition count and definitions: id, name/description C strings, icon id, persistent byte, version, four component vectors in types 0..3 order, points, and a DWORD reward flag. A component has id, u8 type, required count, name/description C strings, and u8 display order. That byte is not the full requirement id carried by progress.

A state is i16 status (Completed 0, Open 1, Locked 2, Hidden 3), then 16-bit LSB-first satisfaction words for component types **1, 2, 0**, with each bucket rounded up separately. Only Completed carries a completion timestamp. Link inspection additionally carries one DWORD count per component in that same bucket order. Type 3 is descriptive and has no runtime bits or counts. Link metadata uses player name, achievement id, status, signed decimal 16-bit words, optional completed timestamp and counts, each followed by `^`. A chat envelope is `0x12 + '3' + metadata + apostrophe + visible name + 0x12`; the current received catalog supplies the displayed name, and plain text/notification data are escaped once. Sharing writes the full tag into chat composition and does not send a chat message automatically.

Initialization is definitions, complete dense primary, then progress, including an empty progress packet. Counts remain unknown until that last step; only then can omitted records mean zero. Progress replaces the value at the full achievement/component/requirement/type key. Distinct requirement counts are summed using a wider display integer; counts and satisfaction bits never infer status. A sparse state update preserves known counters. Serial comparison handles wrap; replayed/older primary serials are rejected. Hidden or class-ineligible definitions keep their wire indices even though the UI does not list them.

Catalog replacement retires selection, linked detail and comparison ownership. All data retires on character/zone/connection changes. A comparison reserves its current catalog and exact nearby PC spawn incarnation (within 20) before outgoing callbacks; only one identity-less reply may be outstanding. Cancellation or the host's 10-second timeout retains a tombstone until that reply drains or the session retires. A late reply cannot attach to a newer comparison. Link inspection checks the named local player's incarnation, catalog and current detail reservation; changing the selected target does not change link ownership. A nonlocal link can display its received snapshot without a network request. These views never overwrite the character's primary state. Refresh, comparison, link inspection and reward actions are explicit; getters and frame drawing send nothing.

A comparison or link inspection still in flight when definitions change retains its original decoding catalog.
A valid old reply drains that retired reservation without updating the new catalog's views, even when definition
counts, component shapes or ids changed. Malformed replies do not clear a reservation, and repeated catalog
replacements retain the original schema until its reply drains or the session retires. Comparison and inspection
requests from the same replaced catalog share its immutable storage; opening a request does not copy the catalog.

Catalog decode is atomic and validates unique ids, category parent/child consistency, cycles and association references. Host ceilings are 8 MiB per packet, 64 MiB inflated catalog, 8192 categories, 65536 definitions, 262144 components (4096 per definition), associations and requirement records, 32 MiB aggregate catalog text, and 64 KiB link metadata. Individual names/icons are limited to 4096 bytes and descriptions/association text to 16384. Limits are host policy, not native container-capacity claims.

Earned notifications require a matching received PC spawn/name. Their chat uses achievement colours 348 (self) / 349 (others) and filter 32 (show all / self only / hide); the sound cue uses the packet's sound id at its achiever, including stock sound 3695 from the player's assets. The bounded event queue and sound are independent of the chat filter. Guild announcements also allow an unspawned member and produce only filtered guild chat. Hailstone adds a self-completion overlay using the existing overlay's standard fades; exact native notification timing is still listed in unknowns. Neither path changes own completion state. Callback reentry rechecks the catalog and spawn incarnation before posting any remaining effects.

### Chat and slash commands (`game/slash_commands.{h,cpp}`, `net/game_connection_social.cpp`)

A typed line is matched against the 391-entry command table less the commands this client leaves out (the four voice macro commands, /legendsofnorrath, /tcg and /mp3; 384 remain, `src/game/slash_commands.inc`, `tools/gen_slash_commands.py`): localised names first, then built-in names, a typed word matching the first command (in table order) that it is a case-insensitive prefix of (`/wh` = `/who`, `/g` = `/gsay`, `/s` = `/split`, `/lfg` = `/lfgroup` by its localised name). GM-only entries never match for players. No match: eqstr 12380 for a `/` word, else the default channel (say). `;` and `[` lines go to the chat server. Dead players get 538 unless the entry's flag bit 0 allows it. `/achievements` toggles the stock window and sends a refresh only on an explicit open; its native command flag allows it while dead.

Chat out (OP_ChannelMessage 0x2b2d) is echoed locally with the client's own template and the server's copy of our own line is dropped on arrival (lines whose sender is the player are skipped; a tell's copy comes back on channel 7 or 14 and prints 1400 "You told %1, '%2'"). Echoes: say 12344, group 12346 (12345 without a group), shout 12348, auction 12350, OOC 12352, guild 12263 (12242 without a guild), raid 5109 (5108 without a raid), emote "<name> <text>" (channel 22). Incoming templates: 1410 say, 1414 group, 1415 guild, 1416 tell, 1420 shout, 1421 auction, 1422 OOC, 5112 raid, 1036 emote; the language part is 1411 " in <language>," (language names from the client's table) or 1412 when the skill is 24 or less. EQEmu sends a sent tell back as channel 14 (TellEcho).

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| WhoAllRequest | C→Z | 0x674b | 156 B: `char[64]` name, 64 B, u32 race, class, level low, level high, GM (1 / 0), guild (or -2 friends, -3 LFG, -4 traders, -5 buyers), type (0 zone, 2 corpse, 3 all, 4 ripoff); 0xffffffff = no filter |
| WhoAllResponse | Z→C | 0x578c | 64 B header (u32 id, u32 title eqstr, `char[27]` rule, u8, u32, u32 footer eqstr @+0x28, u32 footer count @+0x2c, 3×u32, u32 entries @+0x3c), then per player u32 format, u32 flags, u32 PID eqstr, name, u32 rank eqstr, guild, u32, u32 extra eqstr, u32 zone eqstr, u32 zone, [flags 0x200: zone name], u32 class, level, race, account, u32 |
| RandomReq | C→Z | 0x7b10 | 8 B: u32 low, u32 high (the first word and the rest of the line, `atoi`; 0..0 is 0..100 on EQEmu) |
| RandomReply | Z→C | 0x106b | 76 B: u32 low, high, result, `char[64]` name; shown as 1086 and 1087 unless the roller is ignored |
| Emote | Z→C | 0x373b | u32, text; EQEmu's channel 22 emotes (close clients only, `Range:Emote`) |
| Emote (socials) | C→Z | 0x373b | 1028 B: u32 0xffffffff, `char[1024]` text without the name (" beams a smile at Bob."); EQEmu puts the name in front. The animation goes as OP_Animation {spawn, action, speed 10} when standing still |
| Assist | both | 0x4478 | 4 B: u32 spawn; the answer is that spawn's target |
| LFGCommand | C→Z | 0x6060 | 80 B: u32 0, u8 on +4, u8 match filter +5, u32 from level +8, u32 to level +0xc, `char[64]` comments +0x10. `/lfg on` sends {on, 0, 100, ""} then the group finder's post with its fields |
| LFPCommand | C→Z | 0x49a9 | 84 B: u32 0, u8 on +4, u8 match filter +5, u32 from +8, u32 to +0xc, u32 classes +0x10 (1 << class id), `char[64]` comments +0x14 |
| LFGGetMatchesRequest / LFPGetMatchesRequest | C→Z | 0x0340 / 0x4d7d | 16 B: u32 0, from, to, classes (0xffffffff Show All) / 12 B: u32 0, from, to |
| LFGGetMatchesResponse | Z→C | 0x5048 | u32 (skipped), then to the end {comments, name, i16 class, level, zone, guild, anon} |
| LFPGetMatchesResponse | Z→C | 0x22c6 | u32 (skipped), then {comments, i16 from, i16 to, u32 classes, i16 count, count × {name, i16 class, level, zone, guild}} |
| Consent / ConsentDeny | C→Z | 0x1fd1 / 0x7a45 | the name, NUL-terminated |
| SpawnAppearance 21 / 24 | C→Z | 0x0971 | anonymous flags (1 anonymous, 2 roleplaying) / AFK; other spawns' 18 linkdead, 20 GM, 22 guild id, 23 guild rank, 52 guild show, 53 offline mode update `game::Spawn` (the guild lines: Guilds below) |

Tried live against EQEmu with two clients: `/who` (zone) and `/who all bard` (world, truncated by the server with "Your who request was cut short... too many players."), `/random` with the 1086/1087 lines on both clients, `/loc`, `/time`, `/played`, `/emote` (shown to the sender; the other client was outside EQEmu's emote range), `/tell` with the server's echo, `/reply` and `/rtarget`, `/afk`, `/anon`, `/lfg`, the socials (`/smile`, `/smile <name>`, `/cheer` seen by the other client), `/follow`.

### Guilds (`net/social_packets.h`, state `game::GuildState`, window `ui/social_windows.h`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| GuildsList | W→C, Z→C | 0x507a | 64 B 0, u32 count, count × {u32 id, name} (the encoder skips ids ≥ RoF2's limit without fixing the count) |
| GuildInvite | both | 0x7099 | 136 B (pass-through GuildCommand_Struct): `char[64]` other, `char[64]` me, u16 guild, 2 B, u32 rank (8, recruit, from the client). Received: "%1 is interested in having you join %2." (534) and a 10 s Accept/Decline dialog; for a member of that guild it is a rank change |
| GuildInviteAccept | C→Z | 0x7053 | 136 B: `char[64]` inviter, `char[64]` new member, u32 response (the offered rank, or 9 to decline), u32 guild id |
| GuildInviteExt / GuildInviteExtReply (Hailstone) | C→Z / Z→C | 0x1430 / 0x1431 | 80 B: u32 magic (`net/social_packets.h`), u16 version 1, u16 action, `char[64]` name, u32 guild id, u32 value. 0 hello after every zone-in (the reply's value 1: invitations reach other zones), 1 invite (C→Z: whom, rank 8; Z→C: the inviter, rank), 2 the invitee's answer (the rank, or 9 / 0xb). Not in EQEmu's opcode table: answered by `server/`'s Lua script; without the hello's answer /guildinvite stays EQEmu's same-zone OP_GuildInvite |
| HsZoneLinksRequest / HsZoneLinks (Hailstone) | C→Z / Z→C | 0x1438 / 0x1439 | Request 6 B: u32 magic "HSZL" (`net/zone_link_packets.h`), u16 version 1; sent once a session when the map's World view opens, only after the zone answered the settings request. Reply: magic, u16 version 1, u16 flags (0), u32 count, then count × {u16 from zone id, u16 to zone id}: the zone points the server's expansion and content flags allow. Then, from a script that sends them: i16 current expansion (−1 all), u32 count, count × u16 zone id, the zones they allow (a reply without this part is still whole). Not in EQEmu's opcode table: answered by `server/`'s Lua script (`server/README.md` "Zone links") |
| GuildRemove | C→Z | 0x1444 | 140 B (RoF2 GuildCommand_Struct, DECODE): other, me, u16 guild, 2 B, u32, u32. Yourself: 12252 (a leader gets 12251 and nothing is sent); another: 12254 |
| GuildLeader | C→Z | 0x7e09 | 128 B: `char[64]` me, `char[64]` new leader (leaders only, 12261 otherwise) |
| GuildPromote | C→Z | 0x6a98 | 136 B: `char[64]` target, `char[64]` me, u32 guild id, u32 new rank (the member's rank − 1, 8 for a name off the member list; EQEmu reads only the new rank, and refuses rank 1). Permission 8 (5410), 516 when the new rank is above yours |
| GuildPublicNote | C→Z | 0x5053 | 388 B: u32, `char[64]` me, `char[64]` member, `char[256]` note (EQEmu passes it through; permission checked there) |
| GuildManageBanker | C→Z | 0x3f35 | 136 B: u32, `char[64]` me, `char[64]` member, u32 flags (1 banker: leader only; 2 alt) |
| GuildWar / GuildPeace | C→Z | 0x1ffb / 0x67e3 | 140 B: `char[64]` the other guild's member, `char[64]` me, u32 guild id, u32 days (1..14; 0 for peace), u32 1. EQEmu's handlers only log them |
| SetGuildMOTD / GuildMOTD | C→Z / Z→C | 0x0b0b / 0x3e13 | GuildMOTD_Struct: u32, `char[64]` name, `char[64]` set by, u32, text (the client sends 648 B). Shown as 468 "GUILD MOTD: %1 - %2" |
| GetGuildMOTD | C→Z | 0x36e0 | 0x288 B: u32 guild id, `char[64]` name |
| GuildMemberList | Z→C | 0x12a6 | name, then **big-endian** u32 guild id, u32 count, count × {name, u32 level, banker, class, rank, last on, tribute on, trophy tribute on (EQEmu: 0), total tribute, last tribute, flag (EQEmu: 1; the guild window's tribute states show white when set, yellow when 0), public note, u16 instance, u16 zone, u32 1, u32 0} |
| GuildMemberUpdate | Z→C | 0x69b9 | 80 B: u32 guild, `char[64]` member, u16 zone, u16 instance, u32 last seen, u32 |
| GuildStatus | C→Z | 0x7326 | 140 B (GuildStatus_Struct; EQEmu's DECODE wants exactly that): `char[64]` who (the target's name when none is given; 12260 without either), 76 bytes the original leaves as its stack had them (zeros here); the server answers in text |
| GuildDemote | C→Z | 0x2d4e | 132 B, cleared first: `char[64]` your member name, `char[64]` the target's, u32 new rank (rank + 1). Permission 9 for another member (5410; 516 unless your rank is at least theirs), 0x1e for yourself (the leader cannot) |
| GuildCreate | C→Z | 0x76d9 | 64 B: `char[64]` name (4..27 characters: 6369 / 6370; 6371 without a profile) |
| GuildDelete | C→Z | 0x3708 | 68 B, cleared first: u32 guild id, own name. Leader only (9059); sent after the confirmation dialog 0x6d (9060), then 9061 |
| SpawnAppearance 22 / 23 / 52 | Z→C | 0x0971 | {u16 spawn, u16 type, u32 value}. 22 the guild id (0xffffffff none): for us, a change prints 12878 "You have joined %1." or on leaving 12880 "You are no longer a member of %1." (12879 when the list does not name the old guild); another spawn joining our guild 12881, leaving it 12882. 23 the rank: ours 7655 "You are now the rank of %1." when it differs from PcBase::GuildStatus (set only here and by the profile; the removal's rank 0 is not kept), another member's 7656. 52 bGuildShowAnim: the name sprite's "<guild>" line needs it (EQEmu: the rank's permission 4, also the spawn record's guild_show) and the player not anonymous (roleplaying shows it). Colour 0x111 |

EQEmu sends 22, 23 and 52 for a member to the whole zone on a join or a removal (and at each zone-in, where they repeat what the client has), but a promotion's or demotion's 23 to that member alone, so 7656 is not seen there. The guild window's Alt sends OP_GuildManageBanker with the member's banker bit kept (Send_OP_GuildManageBanker only takes the alt flag, kind 2); EQEmu makes bankers by rank (the bank permissions).

Tried live with two clients: /guildinvite by name and on a target in one zone, the question (classic and modern UI), Yes joins (EQEmu's guild chat both ways), No / the 10 s timeout declines ("You declined the guild invite." / "The player declined the guild invite."). Across zones with `server/`'s script: the invitation, accepting (the new member's roster, rank and guild chat), declining, an offline player ("is not online") and an unknown name ("could not be found"). With a guild made by `#guild create` (2026-10-29, EQEmu 23.10.3 on NMS): /guildsay both ways and across zones; /guildmotd and the window's MOTD box (shown to both, 468); /guildpromote and /guilddemote by command and the window's buttons (SetGuildRank and the rank lines; EQEmu answers a demotion below 8 with "cannot be demoted any further"); /guildremove of another and of yourself (leaving); /guildleader both ways ("Successfully Transfered Leadership", the old leader becomes rank 3); the join and leave lines; the window's roster (level, class, rank, last on, zone, an offline member greyed, the live updates), public note (OP_GuildPublicNote, "Successfully changed public note", OP_GuildMemberPublicNote to the guild), personal note (the character's GuildNotes.txt), alt flag ("has been marked as an alt", Hide Alts), a rank's permission cell (OP_GuildUpdate action 5; EQEmu resends every rank's table), a rank's rename (action 4, echoed to the guild); the "<guild>" line under both players' names. EQEmu's `#guild create` does not send the new leader the rank permissions (Client::SendGuildRanks runs at zone-in), so its permission checks refuse ("You don't have the proper access rights.") until the leader zones.

The guild bank tried live (OP_GuildBank and OP_GuildBankItemList, in the table further down; 2026-10-29, one member, a `#spawn`ed class 66 banker in the guild hall): the zone-in's item list, the banker's greeting (1465), deposit (the held item goes; EQEmu's queued cursor item follows), Put in Bank ("transferred to Guild Bank from Deposits"), an item's permission (Public), split, merge, withdraw (one; "You must empty your hands" with something held) and view (the item display). EQEmu's deposit acknowledgement has its failed flag where RoF2 reads the slot to select, so the selection goes to deposit slot 0 (1 after a refusal).

### Raids (`net/social_packets.h`, state `game::RaidState`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| RaidInvite | C→Z | 0x55ac | 140 B RaidGeneral: u32 action, `char[64]` player, u32, `char[64]` leader, u32 parameter. Invite: action 3 when not in a raid, 0 into ours, player = invitee, leader = me (5064). Accept (`/raidaccept`): action 1 {inviter, me} (5079). Decline: action 31 {inviter, me, 5081} (5080; EQEmu has no handler). Leader change: 30 {me, new leader}. Leaving (`/raiddisband`): 5 {me, me}, the leader's removal of the raid target 5 {me, member}. The leader's commands: 6 move {leader, member, group 0..11 / 0xffffffff}, 7 remove group leader {leader, member} (EQEmu: NYI), 8 / 9 lock / unlock {leader, leader}, 0x20 loot type {me, me, 1..4}, 0x21 / 0x22 add / remove looter {leader, member}; 35 the MOTD (1164 B); 0x24 a member's note (204 B: {me, member} and `char[64]` note at +0x8c). The u32 at +0x44 is the raid id (from RaidUpdate 8) |
| RaidUpdate | Z→C | 0x3973 | by action: 0 add (148 B: + u8 class @140, level, group leader, 5 B; parameter = group 0..11 or 0xffffffff), 1 remove, 5 disband (5062), 8 create, 10 the zone-in reset (136 B), 17/18 lock/unlock, 20 invitation (5065, 5067), 14 the raid leader's leadership ranks and 30 leader (392 B: the 64 dwords from +136 are Leadership_SetRaidAbilityRanks' block, raid ability n at dword n; 30 also 5069 / 5070), 32 loot type (+136), 33 / 34 looter added / removed (+72), 35 MOTD (1164 B; 6349), 36 a member's note (204 B: the member at +72, the note at +140; EQEmu sends the notes after the roster) |
| RaidDelegateAbility | both | 0x2b33 | 92 B DelegateAbility_Struct. C→Z: ability 3 main assist / 4 marker / 5, the member's name at +28. Z→C: ability, member number (EQEmu: markers 0), action (1 removes), spawn id @20, name @28; the raid's three of each by number, lines 0x2242..0x224a |

Tried live: `/raidinvite <second player>` → the second player is told "invites you to join a raid" and accepts (`--auto-accept`), "joined the raid", `/rsay` reaches the other member ("<name> tells the raid,  'raid hello'": template 5112 with an empty language part), `/raiddisband` moves the leadership to the other player and removes the first (and the group EQEmu made).

### Chat server (UCS; `net/chat_server.{h,cpp}`)

World sends `OP_SetChatServer2` "host,port,short.Name,<type><key>" (`world_packets.h decode_chat_server`); after zone-in the client opens a second UDP session to it (one-byte opcodes, EQEmu `utils/patches/mail_opcodes.conf`). The host's name is looked up on a thread of its own, so a slow or failing DNS does not stop the frame; commands typed meanwhile wait for the login. Every payload the client sends starts with one byte EQEmu skips.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| MailLogin | C→UCS | 0x01 | 0, "<mail prefix>.<short>.<name>", "<type><8 hex key>" |
| Mail | C→UCS | 0x02 | 0, command text: "join a, b", "leave a", "leaveall", "list", "announce", "<n> text" (channel n), "buddy [-]name", "ignoreplayer [-]name", "afk on silent" |
| Mail | UCS→C | 0x02 | the joined channels, comma-separated, in /1../10 order |
| ChannelMessage | UCS→C | 0x03 | channel, "short.Name", text, ["SPAM:0:"]; channel "" = the server's own text. Shown as 6723 "%1 tells %2, '%3'" with "channel:n" (6722 for our own) |
| ChannelAnnounceJoin / Leave | UCS→C | 0x04 / 0x05 | channel, name |
| Buddy | UCS→C | 0x06 | u8 1 added / 0 removed, name (the whole list at login) |
| Ignore | UCS→C | 0x15 | u8 0 added / 1 removed, "<mail prefix>.short.Name" |

Tried live: `/join` a channel on both clients, `/1 hi channel` reached the other client as "<name> tells <channel>:1, 'hi channel'", `/friends <name>` and the list.

### Duels, inspect and player commands (`net/player_packets.h`, `GameConnection` in `net/game_connection_player.cpp`, rules in `game/interact.{h,cpp}`)

Tried live against EQEmu with two clients (playprobe `--duel`, `--duel-wait`, `--duel-decline`, `--inspect`, `--cmd`): a challenge, its acceptance on both sides ("…has accepted your challenge to duel to the death!  Fight!"), a decline (2026-10-28: the challenger sees "Nmscuieight has declined your challenge to duel to the death." and can challenge again at once), inspect both ways (the other side sees "… is looking at your equipment..."), `/keys`, `/rewind`, `/corpse`, `/toggleinspect`, `/autosplit`, `/language`, `/bandolier`. A duel's end (death or despawn), `/surname` (needs level 20), `/corpsedrag`, `/shield` and books have unit tests only.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| RequestDuel | C→Z | 0x3af1 | 8 B: target, our id (EQEmu swaps them into Duel_Struct {initiator, target}) |
| RequestDuel | Z→C | 0x3af1 | 8 B: the challenger twice; to the challenger after an acceptance: the accepter twice. The client reads +4 |
| DuelAccept | C→Z | 0x68d3 | 8 B: challenger, our id; sent right after an OP_RequestDuel with the same body |
| DuelAccept | Z→C | 0x68d3 | 8 B; the client does nothing with it |
| (duel refusal) | both | 0x52f7 | 12 B: challenger, our id, u8 kind (0 declined or ignored, 1 already dueling, 2 considering another), 3 uninitialised bytes. Not in EQEmu's table: the server drops it. So a refusal of kind 0 also sends EQEmu's DuelDecline below (not while a duel of our own runs or waits) |
| DuelDecline | C→Z | 0x6a46 | 8 B: partner, our id; sent when the partner dies by our hand (12100) or despawns mid-duel (1408). EQEmu wants 12 bytes and drops it, and these two stay 8 on purpose: its Handle_OP_DuelDecline calls `entity_list.GetID(target_id)->IsClient()` without a null check, and here the partner is leaving or has left the zone; EQEmu ends both duels itself (Client::~Client, Client::Death) |
| DuelDecline | C→Z | 0x6a46 | 12 B (EQEmu's DuelResponse_Struct): challenger, our id, 0; sent beside a kind 0 refusal, only for a challenger in the spawn list (EQEmu's handler does not check that the ids exist). EQEmu clears both players' duel state (without it the challenger can challenge nobody until it zones) and sends the challenger 1383 |
| FormattedMessage 1383 | Z→C | | EQEmu's answer to that DuelDecline, to the challenger: "%1 has declined your challenge to duel to the death." with the name EQEmu takes through `Entity::GetName`, which is always "" (Mob's `GetName` hides it). The client fills in the name of the player it challenged and ends its challenge, as the 0x52f7 refusal would |
| InspectRequest | C→Z | 0x57bc | 8 B: target, our id; players only, within 20 (else 12378). `/inspect`: 8516 no target, 8527 too far, 6821 not a player, OP_LDoNInspect (empty) for class 62 objects |
| InspectAnswer | Z→C | 0x71ac | 1860 B: requester, inspected player, 23 × char[64] worn names, 23 × u32 icons (0xffffffff none), char[288] message. Shown within 20 (else 1402); the window closes when the player goes beyond 20 (checked once a second) |
| InspectMessageUpdate | C→Z | 0x4d25 | 256 B: our inspect message |
| Surname | C→Z | 0x0423 | 100 B: our name[64], u32 0, surname[32]; after ConfirmationDialog 0x6f (9050), prints 9051. No argument with a surname: dialog 0x75 (5155) → empty OP_ClearSurname 0x3fb0 |
| GMSummon / CorpseDrag | C→Z | 0x486f / 0x0904 | 152 B (GMSummon_Struct): corpse name[64], our name[64], u32 0, u32 zone id, the corpse's y, x, z (float bits), u32 0; the corpse is the target, else our first corpse in the zone (13048 none); prints 13047 |
| CorpseDrop | C→Z | 0x7037 | the target's name and NUL |
| Shielding | C→Z | 0x52e5 | 4 B: target (warriors of level 30+; 4030/4031/4033/6826) |
| KeyRing | C→Z | 0x1219 | "KEY\0"; EQEmu answers with the key ring as text |
| Rewind | C→Z | 0x1745 | empty |
| SpawnAppearance | C→Z | 0x0971 | /autosplit: {our id, 28, 0/1} |
| Bandolier | C→Z | 0x7677 | 40 B: u32 action (0 create, 1 remove, 2 activate), u8 set, char[32] name (at most 31 characters and a NUL; the original copies 32 with no NUL, which EQEmu's CreateBandolier strcpy's one byte past `Name[32]`: a deliberate difference), 3 B. Create stores the four weapon slots' items (13, 14, 11, 22); activate moves them locally (the server's `SetBandolier` makes the same moves without telling the client: a weapon put back takes the first free general slot or the first free slot of a bag that EQEmu's `CanItemFitInContainer` lets it into, `game::server_bag_accepts`). Like the original's `ProcessMove`, a swap is all or nothing: when a weapon it puts back has no such slot, nothing moves, eqstr 9342 is printed and no packet is sent |
| ReadBook | C→Z | 0x72df | 8216 B: u32 window 0xffffffff, slot {i16 slot, i16 sub, i16 aug −1, i16 0}, u32 book type, u32 target, u8, u8, char[8194] the item's book file |
| ReadBook | Z→C | 0x72df | the same struct with the text; type 0 note, 1 book, 2 item info |
| RequestTitles / SendTitleList | C→Z / Z→C | 0x6344 / 0x2d08 | empty / {u32 count, count × {u32 id, prefix, suffix}}: the titles we may take (`net/player_packets.h`) |
| AltCurrency | Z→C | 0x6b6d | mode 8: u32 count, count × 25 B {currency, 1, currency, item id, icon, stack size, u8 shown}; mode 7 (88 B): `char[64]`, currency, 1, amount, 2 × u32 |
| AltCurrencyReclaim | C→Z | 0x0339 | 16 B {currency, 0, count, flag: 1 put the cursor item in, 0 take `count` out} |
| AltCurrencyMerchantRequest / Reply | C→Z / Z→C | 0x5409 / 0x27a2 | u32 npc / text "cur\|1\|cur\|n" then "^name\|item\|cost\|0\|1\|races\|classes" per item |
| AltCurrencyPurchase / SellSelection / Sell | C→Z | 0x3788 / 0x532a / 0x40b6 | 12 B {npc, item id, 1} / 80 B {npc, 8 B typeless slot, 17 × u32} (the answer is EQEmu's 88-byte reply, the name at +10, the cost at +76) / 20 B {npc, slot, charges, cost} |
| LeadershipExpToggle / PurchaseLeadershipAA | C→Z | 0x74bd / 0x6c55 | 1 B on / off; u32 ability index (0..15 group, 16..31 raid) |
| UpdateLeadershipAA / LeadershipExpUpdate | Z→C | 0x0026 / 0x2797 | 12 B {ability, new rank, points left} / 28 B {f64 group exp, u32 group points, u32, f64 raid exp, u32 raid points} |
| BlockedBuffs / RemoveBlockedBuffs | both | 0x3033 / 0x0de7 | 128 B: 30 × i32 spell (-1 none), u32 count, u8 pet, u8 initialise (1 replaces the list), u16 flags; the server answers with its list (flags 0x54) or the spells removed; it keeps the lists per zone session, so the client sends them at every zone-in |
| AggroMeterTargetInfo / AggroMeterUpdate / AggroMeterLockTarget | Z→C / Z→C / C→Z | 0x16bc / 0x1781 / 0x1643 | 8 B {lock id, target id} / {u8 secondary changed, [u32 secondary id], u8 count, × {u8 type: 0 us, 1 secondary, 2..6 group, 7..26 extended targets; u16 percent}} / u32 spawn (0 unlocks) |
| SendFindableNPCs | Z→C | 0x4613 | 112 B {u32 0 add / 1 remove, spawn id, `char[64]` name, `char[32]` surname, race, u8 class, 3 bytes unread}: one per NPC the server's `findable` flag marks, at the zone-in and as one spawns or goes (Handle_OP_SendFindableNPCs @0x4b8b50 fed the find window; Hailstone's Find window lists them as points of interest, docs/navigation.md "Find") |
| FindPersonRequest / FindPersonReply | C→Z / Z→C | 0x5cea / 0x7e58 | 40 B {u32, spawn id, u32, u32, our f32 y, x, z, 3 × u32} / 12-byte points {y, x, z}: the destination, then the path (its last point repeated); empty for none |
| PVPStats | Z→C | 0x4b15 | 4612 B: kills, deaths, points, career points, best / worst / current streak, infamy, vitality, then 88-byte entries {`char[64]`, level, race, class, zone, time, points}: last death, last kill, 50 recent kills (EQEmu fills only the counts) |
| PVPLeaderBoardRequest / Reply | C→Z / Z→C | 0x04aa / 0x071f | 4 B sort (0 kills, 1 points, 2 infamy) / 8024 B {u32, our kills, points, rank, deaths, infamy, 100 × {`char[64]`, kills, deaths, points, infamy}}, zeroed by EQEmu |
| CrystalCountUpdate | Z→C | 0x467f | 16 B, RoF2 order: radiant now, radiant career, ebon now, ebon career |
| LFGuild | both | 0x3165 | u32 command first. C→Z: 0 our entry (624 B: comment +72, time zone +596, u8 on +600), 1 the guild's (616 B: comment +12, from / to level +524 / +528, classes +532, AA +536, time zone +540, u8 on +544), 3 search players (28 B: 2 × u32, from, to, min AA, time zone, classes), 4 search guilds (24 B: 2 × u32, level, AA, time zone, class). Z→C: 0 / 1 the entries (posted at +608 / +548), 3 {u32, count, × {name, level, class, AA, time zone, comment}}, 4 {u32, count, × {name, time zone, comment}} |
| RequestGuildTributes / SendGuildTributes | C→Z / Z→C | 0x4d5e / 0x1877 | 4 B (unused), right after OP_SendTributes on the first zone-in, in a guild or not / u32 guild, then the personal TributeInfo layout |
| GuildSelectTribute | both | 0x649f | 16 B {id, tier, id, u32} / {id, tier, id, description} |
| GuildSendActiveTributes / GuildTributeToggleReply / GuildTributeFavorAndTimer | Z→C | 0x3771 / 0x20ae / 0x7fe0 | 40 B {guild, favour, timer ms, on, 8, 2 × id, 2 × tier} / the same / 16 B {guild, favour, timer, trophy timer} |
| GuildSaveActiveTributes / GuildModifyBenefits / GuildTributeToggleReq | C→Z | 0x5f3f / 0x34cd / 0x7745 | 48 B {command, 16, master, 2 × id, 2 × tier, 8} / 80 B {0 save / 1 modify, data, 12, master, 2 × id, 2 × tier, 40} (echoed) / 12 B {1 on / 0 off} |
| GuildOptInOut | both | 0x0727 | C→Z 80 B {guild, on, `char[64]` member, command, master}; Z→C 88 B {guild, `char[64]` member, on, trophy on, donations, time, command} ; the named member (case-insensitive) takes the states, donations, time and the command's low byte as its flag. EQEmu echoes the request's command, sends 1 after a donation and 0x2fb at a member's zone-in. The guild window's Tribute Opt Out sends {guild, 0, the selected member} (Send_OP_GuildOptInOut); the original leaves command and master unset, here 0. The tribute benefit window's Opt In / Opt Out sends the opposite of the member's state, a deliberate difference: the original (CTributeBenefitWnd @0x784f13) always sends 0, which against EQEmu could never opt back in |
| (trophy opt-out) | C→Z | 0x02bc | Not in EQEmu's table: OP_GuildOptInOut's request layout (Send_OP_GuildTrophyOptInOut @0x424120), from the guild window's Trophy Tribute Opt Out. EQEmu has no trophy tribute, so the button is never enabled against it |
| GuildTributeDonateItem / GuildTributeDonatePlat / OpenGuildTributeMaster | both | 0x5bcb / 0x43d2 / 0x49ea | 36 B RoF2 {type, u16 slot, sub, aug, u16, quantity, master, u32, guild, 8} → 24 B {type, slot, sub, aug, u16, quantity, u32, favour} (the item deleted unannounced) / 20 B {platinum, master, u32, guild, u32} → 12 B {platinum, u32, favour} / StartTribute (12 B), class 64 |
| Translocate / RezzRequest → RezzAnswer | both | 0x6580 / 0x3c21 → 0x701c | 92 B Translocate_Struct (zone, spell, u32, `char[64]` caster, y, x, z, u32 complete +88) / 236 B RoF2 Resurrect_Struct (rezzer +92, corpse +160, action +224); the client echoes the packet with the yes / no |
| SharedTaskInvite / SharedTaskInviteResponse | Z→C / C→Z | 0x3444 / 0x7582 | 136 B {i32, i32 invite id, `char[64]` task, `char[64]` inviter} / 12 B {0, invite id, i8 accepted, 3} |
| Stun / ItemRecastDelay / Bind_Wound | Z→C | 0x36a4 / 0x15a9 / 0x0386 | u32 duration ms (+2 B) / 12 B {delay s, recast type, bool} / 8 B {u16 target, u16, u16 type: 3 start, 1 done, 4 target dead, 5 gone, 7 no bandage} |
| Sound (QuestReward_Struct) / CashReward | Z→C | 0x1a30 / 0x5f7a | 68 B {giver, our id, exp, faction, mod, copper +20, silver, gold, platinum} / 16 B {copper, silver, gold, platinum}: coin added without a coin update |
| GuildMemberAdd / Delete / Level / PublicNote / SetGuildRank (= RankAltBanker) / Details | Z→C | 0x2925 / 0x3141 / 0x1bd3 / 0x01f9 / 0x0b9c / 0x69b9 | u32 guild first; see GameConnection::handle_world_social |
| Charm / DisciplineUpdate / Untargetable / YellForHelp / GetGuildMOTDReply | Z→C | 0x5d92 / 0x759e / 0x053c / 0x4e56 / 0x4f1f | 12 B {owner, pet, 1 on / 0 off} / 300 × u32 / 8 B {spawn, targetable} / u32 yeller / the GuildMOTD struct |
| Weblink | Z→C | 0x6f4b | the address, NUL-terminated (EQEmu `Weblink_Struct`); the case @0x4c5b5d opens it in an HTML window (docs/browser.md) |
| Bug | C→Z | 0x73f4 | 8740 B EQEmu BugReport_Struct (not translated): category and its name, reporter, UI path, server x, y, z, heading, minutes played, target id and name, flags (1 can duplicate, 2 crash, 4 target, 8 character flags), the report at +2592, system info at +4642 |
| VetClaimRequest / VetRewardsAvaliable / VetClaimReply | C→Z / Z→C / Z→C | 0x1126 / 0x590e / 0x16d4 | 80 B {`char[68]`, claim id (0xffffffff: the list), u32, action} / {count, × {claim id, available, item count, u8 enabled, × {u32 length, name (not NUL-terminated), item id, charges}}} / 80 B, action 0 claimed, 1 refused |
| Consume | C→Z | 0x4b70 | 24 B RoF2 Consume_Struct {12 B slot, u32 0xffffffff, u32 1 food / 2 drink, u32 1 (the right click: Handle_OP_ItemVerifyReply; the auto-eat here still sends 0xffffffff, type, 2)}; EQEmu deletes one unannounced and answers OP_Stamina |
| ItemVerifyRequest | C→Z | 0x189c | 16 B {12 B slot, u32 target spawn id}: a right click on a common item in the possessions, /useitem, the potion belt; IsItemPending + 1 |
| ItemVerifyReply | Z→C | 0x097b | 20 B {12 B slot, i32 spell (0: the item's click spell; negative: nothing), u32 target}; IsItemPending − 1 (not for slot 0x407); EQEmu sends it first, spell 0, then acts (clicky cast, tome, scroll scribe); the client's cast of the click effect sends no OP_CastSpell (CastSpell's send argument 0, @0x4bff60) |
| Dye | C→Z / Z→C | 0x23b9 | 36 B: nine tints 0xUURRGGBB (UU 0xff tinted); each changed slot takes a Vial of Prismatic Dye (32557); the server sends the wear changes and an empty OP_Dye |
| UpdateAura | both | 0x1456 | Z→C 80 B {0 add, type, `char[64]` name, the aura's spawn id, icon}; both 8 B {1 remove, spawn id} (the client asks, the server echoes); Z→C 4 B {2} clears them |
| GuildBankItemList | Z→C | 0x7850 | while zoning into the guild hall (345): 40 deposit then 200 main entries, each u8 present, then {permission, who for, donator, item id, icon, quantity, u8 can merge, u8 usable, name} |
| GuildBank | both | 0x5134 | RoF2's actions are EQEmu's less one. Z→C: 0 a slot's update (230 B: slot, area, shown, item id, icon, quantity, permission, merge, usable, `char[64]` name, donator, who for), 9 acknowledgement (12 B), 4 deposit acknowledgement (16 B, failed at +12). Z→C 2 and 4 carry a u16 slot, u16 area at +12 the window selects (EQEmu: its failed flag); on 4 the held item goes. C→Z, each {u32 action, u32 the banker's spawn id, u32}: 2 promote, 3 view (value 1 unless Shift), 6 withdraw, 7 split, 8 merge (24 B: u16 slot, u16 area, u32 item id, u32 value), 4 deposit the cursor item (16 B), 5 permissions (88 B: slot, area, item id, permission, `char[64]` member) |
| AdventureMerchantRequest / Response | C→Z / Z→C | 0x6922 / 0x3e47 | 8 B {type 1 LDoN, 2 Discord, 4 Norrath's Keepers, 5 Dark Reign; npc} / u8 count, then "^name\|id\|cost\|theme\|stackable\|lore\|races\|classes" per item |
| AdventureMerchantPurchase / Sell | C→Z (Sell both) | 0x5b72 / 0x2f9b | 16 B {type, npc, item id, u32} / 24 B {1, npc, 8 B typeless slot, charges, price}; the server echoes the sale and deletes the item unannounced (so does OP_AltCurrencySell's echo) |
| LeaveAdventure | C→Z | 0x5d18 | no body |
| DzCompass | Z→C | 0x3e0e | {u32, u32 count, × 24 B {u16 zone, u16 instance, type, switch id, f32 y, x, z}} (server y, x: zone-file x, y) |
| SharedTaskMemberList / MemberChange | Z→C | 0x1e7d / 0x0119 | {u32, u32, count, × {name, u32 mission, u8 leader}} / {u32, u32, u8 added, name} |
| AdventureInfoRequest / AdventureInfo / AdventureDetails | C→Z / Z→C / Z→C | 0x3cb0 / 0x4c54 / 0x5648 | u32 the recruiter (class 60) / its text or the server's refusal / the adventure offered (NUL-terminated strings) |
| AdventureRequest / LDoNButton | C→Z | 0x2c6c / 0x5327 | 12 B {risk 1 normal / 2 high, recruiter, objective 0 any .. 4 rescue} / 1 B accept the offer |
| AdventureData / AdventureUpdate / AdventureFinish | Z→C | 0x7171 / 0x1b01 / 0x400f | 2084 B {u32 (0 with no adventure: all zero), `char[2048]` text, seconds to enter, seconds left, risk, f32 x, y, z, show compass, u32} / 8 B {current, total} / 8 B {won, points} |
| AdventurePointsUpdate | Z→C | 0x65c3 | 140 B {available; each theme's points at +56, +76, +96, +116, +136} |
| AdventureStatsRequest / Reply | C→Z / Z→C | 0x5a62 / 0x2370 | 1 B zero (native sender reserves one byte; EQEmu ignores it) / 96 B: successes, failures, rank, percentage rank × {total, five themes} |
| AdventureLeaderboardRequest / Reply | C→Z / Z→C | 0x7093 / 0x7f79 | 12 B {order 0 wins / 1 percentage, theme 0..5, risk 0 all / 1 normal / 2 high (EQEmu ignores risk)} / 7220 B {order, reserved, our successes, failures, rank, 100 × {`char[64]`, successes, failures}}; EQEmu zeroes both leading words |

| DzExpeditionInfo / DzSetLeaderName / DzExpeditionEndsWarning | Z→C | 0x9119 / 0x3de9 / 0x5189 | every DZ packet starts {u32 client id, u32}: 208 B {assigned, max players, `char[128]` name, `char[64]` leader} / 72 B `char[64]` leader / 12 B minutes left (EQEmu sends its own chat line too) |
| DzMemberList / DzMemberListStatus / DzMemberListName | Z→C | 0x5ae4 / 0x12f5 / 0x32f0 | {count, count × {name, u8 status: 1 online, 2 offline, 3 in the zone, 4 link dead}} (Status: one member whose status changed; more than 1000 members are refused, a client limit) / 76 B {u32 add, `char[64]` name} |
| DzExpeditionLockoutTimers | Z→C | 0x205f | {count, count × {expedition, u32 seconds, i32 type (-1 replay), event}} (the answer to DzListTimers too) |
| DzExpeditionInvite / InviteResponse | Z→C / C→Z | 0x7f4b / 0x1950 | 272 B {`char[64]` inviter, `char[128]` expedition, u8 swapping, `char[64]` swap name, 3, u16 zone, u16 instance} / 80 B {u32, u32, u16 zone, u16 instance, u8 accepted, u8 swapping, `char[64]` swap name, 2} |
| DzChooseZone / DzChooseZoneReply | Z→C / C→Z | 0x0b7d / 0x4de1 | {count, × {u16 zone, u16 instance, u32 id, u32 type, u32 id, description, leader}} / 52 B {ff.., 0x69, ff.., id, zone, instance, type, id, 24 B} |
| DzAddPlayer / DzRemovePlayer / DzMakeLeader / DzSwapPlayer / DzQuit / DzListTimers / DzPlayerList | C→Z | 0x4701 / 0x1abc / 0x543d / 0x405b / 0xb2e3 / 0x7b68 / 0x14c6 | 72 B {u32, u32, `char[64]` name}; swap 136 B {.., added, removed}; the last three no body (EQEmu reads none) |
| SetTitle | C→Z | 0x6527 | 8 B {u32 suffix (0 prefix), u32 title id (0 clears)} |
| SetTitleReply | Z→C | 0x4c21 | 40 B {u32 suffix, `char[32]` text, u32 spawn id}: to everyone near, ourselves too |
| NewTitlesAvailable | Z→C | 0x0d32 | empty: a line when the window's notice box is on |

Duel state (`game::DuelState`) is the player we challenged or accepted, the challenger, and an on flag. The profile's bandolier sets and potion belt are decoded (`PlayerProfile::bandoliers`, `potion_belt`).

Adventure request fields are verified against RoF2 senders `0x420030`, `0x4209f0`, `0x41fed0` and `0x41ff80`.
The stats sender `0x619570` and leaderboard sender `0x6173e0` both gate requests for ten seconds (EQEmu separately
limits them to eight and four seconds). The leaderboard category index interleaves order within theme:
`order = category % 2`, `theme = category / 2`; risk is the second selector's raw index.
The player profile's counted LDoN block has six u32 entries: a reserved entry then the five theme balances,
followed by the available total (`rof2.cpp ENCODE(OP_PlayerProfile)`). Subsequent point updates replace these balances.

The leaderboard reply has no reliable category identifier on stock EQEmu, so Hailstone serializes requests and
holds the latest selection until the preceding reply and ten-second guard allow it. A missing reply is released
after thirty seconds, a Hailstone recovery choice. See `docs/unknowns.md` for the residual late-reply ambiguity.

### Bazaar and barter (`net/market_packets.h`, `GameConnection` in `net/game_connection_market.cpp`, rules in `game/bazaar.{h,cpp}`)

Tried live with two clients (a trader on a rug stall, type 155, and a shopper): Begin Trader on the stall, the slot selection and price load, the search, Find Trader (the path, the button's 15 s wait), a row's Buy (the quantity dialog), /offlinemode's refusal 771. Earlier in the Bazaar of an EQEmu server (playprobe `--shop-trader`, `--bazaar`, `--bazaar-buy`): the trader list (OP_TraderBulkSend), the search (17 and 23 results from simulated traders), a shop at a simulated trader (17 items, close), a purchase's answers (a search purchase without coin: sub action 7).

RoF2 has no OP_TraderBuy: everything goes through OP_Trader (trader mode) and OP_TraderShop (customers), the first dword being the action (`RoF2BazaarTraderBuyerActions`).

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| Trader 1 (begin) | C→Z | 0x4ef5 | 4404 B: u32 1, 200 × char[18] item GUIDs (the server's 16-digit serial), 200 × u32 prices; the slots with a price |
| Trader 2 / 11 | C→Z | 0x4ef5 | 12 B {2 end / 11 list my items, 0, 0} |
| Trader 3 / 20 | C→Z | 0x4ef5 | 32 B: action (3 price change, 0 withdraws; 20 reconcile after a sale), GUID[18], 2 B, u32 count (20), u32 price |
| Trader | Z→C | 0x4ef5 | {action, sub, …}: 2 trader off (6741), 3 price answer (sub 0 6733, 1 6736, 2 6735, 3 6734), 10 a sale (TraderBuy_Struct: 6744 and the register sound, then reconcile), 11 the listing (4404 B like the begin: 6737, 6738 per item, 6740), 13 a customer (6745), 22 TraderAck2 (sub 0: 6742, trader mode on, ask for the listing; 12: 6743; 10/5/else: 784/786/785), 21 {name, code} |
| BecomeTrader | Z→C | 0x61b3 | 8 B: spawn id, trader flag (the spawn's Trader field and nameplate) |
| TraderShop 28 | C→Z | 0x31df | 12 B {28, trader spawn, byte 0 + 3 uninitialised} (a right click on a trader within 20) |
| TraderShop 28 | Z→C | 0x31df | 12 B {28, trader, approval}: on approval the items follow as item packets of type 0x64 (non-stackables at the cursor slot: keyed by arrival, the GUID identifies them) |
| TraderShop 10 | C→Z | 0x31df | 284 B TraderBuy_Struct: action 10, method (0 at the trader, 1 parcel, 2 to the inventory), sub 0, trader (spawn id; search: character id), buyer[64], seller[64], 32 B, item name[64], GUID[17], 3 B, item id, price each, already sold, u32 (vouchers), quantity |
| TraderShop 10 | Z→C | 0x31df | the same struct, sub action = result (0 success, 1 failed, 3 out of date, 4 lore, 5 parcel cap, 6 pending, 7 funds, 8 tokens, 9 Krono): method 0 (1437, coin taken), else 787 "You send…"; then the English line ("You successfully purchased …"). EQEmu's `Client::TradeRequestFailed` echoes the request (sub 0) with trader id and already-sold 0xffffffff; the client reads that as sub 1 ("… was unsuccessful.", no coin taken; `is_eqemu_trade_failure`) |
| TraderShop 4 | C→Z | 0x31df | 4 B {4}: the trader's shop closes (EQEmu answers OP_ShopEndConfirm) |
| TraderShop 7 | C→Z | 0x31df | 132 B BazaarSearch_Struct: action, u8 scope (1, 0 for "local traders"), 11 B, trader (character id, 0 all), class, race, stat, slot, type (0xffffffff any), name[64], min / max cost (platinum), min level 1, max level 100, max results 200, prestige 0, augment 0 |
| TraderShop 18 | C→Z | 0x31df | 40 B {18, 0, trader, GUID[18], 2 B, item id, flags (2 shift)}: the item comes back as a link view |
| TraderShop 26 / 9 | C→Z | 0x31df | 12 B: 26 the search window's first opening after zoning, 9 its welcome button |
| TraderShop 9 / 24 / 25 | Z→C | 0x31df | 9 welcome {9, 0, traders, items} (6762, 6763, 6764); 24 a trader joins the search list {u16 zone, u16 instance, character id, spawn id, name[64]}; 25 leaves {trader} |
| TraderBulkSend | Z→C | 0x6a96 | u32 count, {u16 zone, u16 instance, u32 character id, u32 spawn id, name} |
| BazaarSearch | Z→C | 0x39d6 | u32 0, u16 zone, u32 trader, u32 count, {u32 trader, GUID string, u32 cost, u32 quantity, u32 item id, u32 icon, name, u32 stat}; EQEmu's "done" (Type 12) and delivery cost packets share the opcode with a count ≤ 0 |
| TraderDelItem | Z→C | 0x5829 | 29 B: u32, trader, GUID[17], u32: the item leaves the open shop |
| (Find) | C→Z | 0x0e44 | 4 B {spawn id}: the bazaar search's Find Trader and the barter search's Find, before the find path to that trader or buyer in this zone (no EQEmu opcode) |
| (Offline mode) | C→Z | 0x53d3 | empty: /offlinemode (and the trader and buyer windows' Offline Mode) for a trader or buyer (a GM, or with the RoF expansion), after the Options/OfflineModeConfirm question 13345 when that is on (no EQEmu opcode) |
| Barter 11 / 12 | C→Z | 0x243a | 12 B {11 inspect a buyer / 12 stop, spawn id, 4 B}; 11 comes back with the approval at +8 (0: busy, the merchant window closes; else it opens) |
| BuyerItems 14 / 7 | Z→C | 0x1a6a | {action, u32, a buy line}: 14 a viewed buyer's line (replaced, removed when not active, else added), 7 our own (stored at its slot). The line: u32 slot, u8 enabled, then when enabled u32 item id, name, u32 icon, u32 quantity, u8 active, u32 cost, u32 n, n × {id, quantity, icon, name}, u32 buyer spawn, u32 buyer character, u32 zone, buyer name; EQEmu leaves out the last four (read as 0) and sends removals as {7 / 14, 0, slot, 0} |
| Barter 13 | Z→C | 0x243a | 76 B {13, spawn id, 64 B, u32 enabled}: the byte at +72 is the spawn's buyer flag; the active merchant's window closes |
| BuyerItems 9 (sell) | C→Z | 0x1a6a | {u32 9, the 0x178-byte sale record: u32 method (0 at the buyer, 1 a buyer in this zone, 2 elsewhere), u32 1, u32 buyer spawn, u32 buyer character (0 at the buyer), 8 B, three empty strings, the buy line with its buyer fields, u32 quantity, u32 0}; EQEmu's DECODE reads the same bytes |
| BuyerItems 9 / 10 (result) | Z→C | 0x1a6a | {u32 9 sold / 10 bought, the sale record: +0 method, +4 result (0 sold, 1 failed, 3 pending, 4 out of date, 5 buyer full, 6 no item, 7 / 9 Krono, 8 same zone), buyer [64], item [32], seller [64], the line (name, cost, compensation), +0x170, +0x174 the quantity}; EQEmu writes its sub action at +0 and its error code at +4, the line's item id as 0xffffff |
| Barter 1 (search) | C→Z | 0x243a | 84 B: u32 1, `char[64]` text, u32 transaction (__EQGetTime), u32 0, u32 buyer character (the combo's buyer; 0 all), u32 scope (1 "Local Barterers"); EQEmu reads 83 B packed, its u16 zone from the scope dword's upper bytes (0) |
| BuyerItems 1 (matches) | Z→C | 0x1a6a | {u32 1, search text, u32 transaction, u32, u32, u8, u32 count, count × the line with its buyer fields (EQEmu: u16 zone, u16 instance at the zone dword)}; read only for the last search with the window open; 6063 at 200 or more, 6064 the count. The lines stop where the data does: a count with nothing behind it makes no empty lines (the reader is lenient, so it would otherwise give a default line per count) |
| Barter 2 / 3 (buyer mode) | both | 0x243a | 4 B {2 on / 3 off}: asked when it differs from the flag (off clears it at once); the server's 2: 6058, the welcome (0x13) and BuyerItems 6 with the enabled, active lines; 3: 6059 (EQEmu: on needs a barter stall door, opentype 155) |
| Barter 0 (item search) | both | 0x243a | C→Z 68 B {0, `char[64]` text} (6057 to the status text); Z→C {0, u32 count, count × 76 B {`char[64]` name, u32 item id, u32 icon, u32}} |
| BuyerItems 6 / 7 (our lines) | C→Z | 0x1a6a | 6: {u32 6, u16 count, count × line}; 7: {u32 7, line} (create / modify / toggle in buyer mode); our lines' buyer fields are zero (13 B) |
| Barter 8 (remove a line) | both | 0x243a | 8 B {8, slot} (in buyer mode; EQEmu reads +4 and echoes BuyerItems {7, 0, slot, 0}); Z→C {8, slot} empties the line |
| Barter 0x13 (welcome) | C→Z | 0x243a | 328 B {0x13, u32 0, u8 0, 63 B, `char[256]` message at +0x48} |
| Barter 0x0f / 0x15 (inspect) | C→Z | 0x243a | 0x0f 32 B {0x0f, 0, buyer (spawn id in this zone, else character id), zone, slot, compensation index (-1 the item bought), item id, 2 with Shift}; 0x15 12 B {0x15, item id, 2 with Shift} (an item search result) |
| Barter 0x10 / 0x14 / 0x16 | Z→C | 0x243a | 0x10 {name}: 6060 "browsing"; 0x14 {name at +8, text at +0x48}: 6070 / 788 (EQEmu sends a chat line instead); 0x16: "Your inventory is full!" |
| Barter 0x11 / 0x14 / 0x17 | C→Z | 0x243a | 0x11 4 B the hall's welcome; 0x14 8 B {0x14, buyer character} the buyer's greeting; 0x17 4 B the buyers for the search window's combo, once per zone |
| Barter 0x1a / 0x1b, BuyerItems 0x19 | Z→C | both | 0x1a {zone, character id, spawn id, name[64]} adds a buyer to the combo, 0x1b {character id} removes one, BuyerItems 0x19 {count, records} replaces them (EQEmu never sends it; the records stop where the data does) |
| BuyerItems 5 | Z→C | 0x1a6a | {5, u32 length, "<count>$<record>$..."}: lines in the BART ini format, stored and saved (EQEmu never sends it) |

The trader window opens only with the membership feature 0xe and in The Bazaar (zone flag 0x10000000, only zone 151); Begin Trader needs the player standing on a trader stall (the floor switch under the feet, types 0x99 / 0x9b, else 6743); its 200 slots are the items in Trader's Satchels (bag type 51) in the general slots, in slot order; prices come from `[ItemToSell]` of `BZR_<name>_<server>.ini` (key: the item id, or "id:charges" for an item with a click spell), written by Set Price (6751) for every slot holding the same item. Prices show as "12p 3g 4s 5c". Each price button opens the amount dialog: gold, silver and copper 0..100 as on the original; platinum 0..4,294,967, where the original stops at 1,000,000 (a deliberate difference: the price fields are 32-bit copper on the wire and in EQEmu's `trader.item_cost`, so a price is held at 4,294,967,295 copper).

The barter windows (`game/barter.h`). `/buyer` needs the membership feature 0xf (else "Your ability to create buy lines in the bazaar has been disabled ...", colour 0x14f) and opens the buyer window only with the expansion bit 0x100 (or a server named "test") in a barter zone (151, 346); it loads the 200 buy lines and the welcome text from `BART_<name>_<server>.ini` (`[BuyLines] BuyLine00..` as "*2^slot^enabled^active^id^name^icon^price^quantity^n[^id^qty^name^icon]...", `[General] Welcome`; here BART.ini in the character's folder) and closing it turns buyer mode off and saves them. Out of buyer mode Create / Modify / Toggle / Remove change the lines locally; in buyer mode they go to the server, whose answers change them. The status text and `btrlog.txt` get 6057..6069, the reports ("You've bought N item from X for <coin> and the following: ..."), "Listing Active Lines:". The barter search's sale needs the RoF expansion bit 0x40000 (7728), is refused for one's own line, for a line with compensation in another zone, and without the item; the merchant window's Sell waits for the answer to the last sale ("Whoa!", 12063). Sales and buys give or take the coin locally with sound 0x8a, as EQEmu sends no coin update. Quantities: a sale or compensation item at most 20, a count 0..999, a price 1..1,000,000 per coin button (capped at 1,000,000,000 copper).

### Mail (UCS; `net/chat_server.h`, `GameConnection` in `net/game_connection_mail.cpp`, rules in `game/mail.{h,cpp}`)

Tried live between two clients: send, "New mail from …", the mailbox switch (4534), the header count (4539), the headers, a body with its recipients, delete (the message's status became 4 on the server).

Mail commands go on the chat session as OP_Mail with the mail command text, the leading '[' becoming ';' (e.g. [0x02] ";getheaders\0\0").

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| Mail | C→UCS | 0x02 | ";selectmailbox n", ";getheaders", ";getbody id", ";setmessagestatus NEW\|UNREAD\|READ\|TRASH\|DELETED id …" (EQEmu: R… 3, T… 4, anything else deletes), ";mailto \"<recipients>\" <subject, quoted with a space> <body>"; recipients "<mail prefix>.<server>.<name>" joined with ", " |
| MailLogin | UCS→C | 0x01 | u8 1, u32 count, u32 0, the mailbox addresses comma-separated |
| MailboxChange | UCS→C | 0x14 | the mailbox number: 4534, then the headers are fetched |
| MailHeaderCount | UCS→C | 0x0d | strings: mailbox, a constant, "1", count → 4538 / 4539 / 4540 (4541 failure) |
| MailHeader | UCS→C | 0x0e | strings: mailbox, constant, row, id, time, status (2 unread, 3 read, 4 trash, 5 deleted, else new), from, subject |
| MailSendBody | UCS→C | 0x0f | strings: mailbox, id, body, "1", "", "\nTO:<recipients>\n" |
| MailNew | UCS→C | 0x10 | strings: mailbox, id, time, status, from, subject → 4547 (another mailbox: 4533) |
| MailDeliveryStatus | UCS→C | 0x12 | strings: sequence, recipient (failure), subject, "1" sent / "0" failed → 4530 / 4531 |

Delete in the inbox sends TRASH, in the trash DELETED; Purge sends DELETED for the trash. The composition window's From is the current mailbox's address as MailLogin listed it; it opens and sends only with the membership feature 6.

### Marketplace (`net/marketplace_packets.h`, `GameConnection` in `net/game_connection_marketplace.cpp`, rules in `game/marketplace.{h,cpp}`)

The in-game store (CMarketplaceWnd). Stock EQEmu has none of these opcodes (they are not in `patch_RoF2.conf`): a server that sells through the window answers the original client's opcodes, so the same server works for this client and for an unmodified RoF2 client. Layouts are the original's readers and senders (NMS-Release-Extras' `offsets/Market.h` section 44 and the disassembly of HandleWorldMessage's cases). The funding pages (Add Funds, credit cards) went to the publisher's web service and are not offered. Unit-tested and mock-server tested; not tried live.

The catalog, funding and state feeds are read as the original's `CUnSerializeBuffer` reads them: every field on its own bounds check, so a short packet leaves what it lacks at the defaults (MarketplaceItem's ctor) and goes on with the next field. Catalog strings are NUL-terminated and cut to the node's buffer (name 127, the 64-byte one 63, the rest 1023 characters); the cursor still skips the whole string. The state feed's string is a u32 length and the bytes.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| Hello | C→Z | 0x5bd5 | 76 B: zeros but u32 +0x48 = 1; at the game UI's build, the first frame after every open and every 30 s while shown |
| RequestItems | C→Z | 0x5148 | no body: at the game UI's build (the first zone-in after character select) while the catalog is empty; the catalog is kept all session |
| RequestFunding | C→Z | 0x76c3 | no body: at the game UI's build while the funding list is empty |
| Ready | C→Z | 0x356f | no body: at the game UI's build, at every show, and after a catalog arrives while the window is shown. The server answers with the wallet and the states |
| Items | Z→C | 0x1e9f | a full replace. u32 count, then each: u32 id, u32 price, u32 icon (items.icon), u32 max quantity (0 none; the client stops at 10), name, category path ("A/B/C"), description (STML), cstr64, u8, u8 new, cstr, cstr (the purchase's success text), u8 item of the week, u32 its rank (highest wins), u32 kind (0 in-game; 3 a quest-reward gate; 4, 6 Station Cash), u32, 4 × cstr, u32, u32, u8 featured, u32 was-price (struck through when above the price), u32 available for (seconds, 0 always), u32 on sale for (seconds), u32, u32 money type (0 an integer price, 1 "%1.2f" of price × 0.01, else "N/A"), u32, u8 |
| Funding | Z→C | 0x09bf | a full replace. u32 count, then {u32 amount, u32, cstr64, cstr64} |
| States | Z→C | 0x77a7 | u32 count, then {u32 entry id, u32 state, u32 item id (the preview's and Inspect's), u32, u32 length + the Player Studio maker ("Made By: …"; when set the row is also filed under "Player Studio/<its category>")}. Ignored while a catalog that came with the window hidden waits for the next show. Every row starts in state 2, which keeps it out of the category counts (and so out of the tree); Buy needs state 0. Only rows of an open window take it: answer every Ready |
| Wallet | Z→C | 0x4b7f | ≥ 84 B, fixed offsets: u32 +0x44 status (0), u32 +0x48 balance, u8 +0x4c (1: no zip-code page before the confirmation), u32 +0x50 type (1 the marketplace's; 2 the Krono count, not kept) |
| Purchase | C→Z | 0x4a74 | 80 B: zeros, u32 +0x48 entry id, u32 +0x4c quantity. The window then waits on its results page; 60 s with no answer gives eqstr 7626 |
| ZipCode | C→Z | 0x3289 | 80 B: the zip code (at most 5 digits) at +0, zeros; from the zip page, shown before the confirmation while the wallet's +0x4c is 0 |
| ZipOnFile | Z→C | 0x3171 | the byte +0x48 ORed into the wallet's zip flag |
| PurchaseResult | Z→C | 0x0a37 | fixed offsets: u32 +0x40 entry id, u8 +0xc8 success, u32 +0xd8 (set: eqstr 7621, else 5361), u32 +0xdc new balance, u32 +0xe0 where the item went (1 inventory, 2 bank, 4 shared bank), u32 +0xec failure code (1, 4 → 7622; 2 → 7623; 3 → 7624; 6 → 7632; 8 → 714; else 7626), cstr +0xf0, cstr +0x4f0 (appended to the success text). An OK dialog shows the text and the window comes up |
| Unavailable | Z→C | 0x0114 | no body: eqstr 7631 in chat and the window closes |
| ItemPreviewRequest | C→Z | 0x7f80 | 80 B as the object preview's, but u16 +0x4c = 1: the reply comes on 0xb07f (EQEmu answers every preview on 0x7f80, which this client shows in the object preview window) |
| MarketplacePreview | Z→C | 0xb07f | OP_ItemPreviewRequest's reply for the window: the serialized item and one trailing byte |

Opcodes the original sends that this client does not (their meaning is unknown or belongs to the publisher's services): 0x2a44, 0x3e15, 0x14c3, 0x6c8b, 0x7679, 0x54e6. Opcodes it accepts that this client ignores: 0x1eba (a funding preview's debug line), 0x3036, and the eight the original also ignores (0x1b5d, 0x1f6e, 0x20d9, 0x3042, 0x4ffc, 0x6e2a, 0x740d, 0x7aa9).

### Personal tribute (`net/tribute_packets.h`, `GameConnection` in `net/game_connection_tribute.cpp`, windows in `app/lua_eq_progress.cpp`)

From EQEmu (zone/tribute.cpp and the RoF2 translator). Not tried live.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| SendTributes | C→Z | 0x729b | 4 B (unused; zeros), on the first zone-in only, before OP_RequestGuildTributes (EQEmu answers it only while zoning in): one TributeInfo per tribute |
| TributeInfo | Z→C | 0x4254 | **big-endian** u32 id, u32 tier count, 10 × {level, item id, cost}; u32; name |
| TributeUpdate | both | 0x5961 | 48 B: u32 active, 5 × u32 tribute (0xffffffff none), 5 × u32 tier, u32 master |
| TributeTimer / TributePointUpdate | Z→C | 0x073d / 0x5300 | u32 ms left (the full span when off) / 16 B {favour, u32, career favour, u32} |
| TributeToggle | C→Z | 0x241d | u32 1 on, 0 off (the server charges the active cost) |
| OpenTributeMaster | both | 0x7666 | 12 B {our spawn, master, response (1 open)}; a right click on a class-63 NPC |
| SelectTribute | both | 0x79fc | C→Z 12 B {our spawn, tribute id, 0}; Z→C {spawn, tribute id, description} |
| TributeItem / TributeMoney | both | 0x4f3e / 0x58fb | 24 B {slot (12 B), quantity, master, favour} / 12 B {platinum, master, favour}: the server takes the donation and answers with the favour |

The profile carries favour, career favour and the five chosen tributes (`PlayerProfile::tribute_points`, `career_tribute_points`, `tributes`).

The bonus items arrive through `OP_ItemPacket`, type 108 for personal tribute (inventory type 6, slots 0..4)
and 109 for guild tribute (inventory type 8, slots 0..1). These received items feed local stats and effects.
At login, personal bonus item packets precede `OP_CharInventory`; that possessions/bank snapshot preserves
already received tribute items unless it explicitly supplies their replacements.
The server does not notify their deletion on deactivation or opt-out: received tribute settings, guild active
benefits and our own opt state retire obsolete items. Changed selections retire the affected slot; repeated
unchanged state preserves it. Guild membership and session changes retire the previous context's items too.

Guild benefit editing and activation require received guild permission 23, checked both by the shared window
bridge and before sending a request. Another member's opt-out requires permission 22; self opt-in/out remains
available. Revocation discards unsaved guild-benefit edits. Both UI packs use these shared checks. Source can
send the next guild's tagged permission rows, rank names and active tribute snapshot before the own GuildID
appearance. Those permissions remain separate until membership confirms their guild. The active packet's
leading DWORD can be zero (`SendGuildActiveTributes`) or the guild id (`GuildTributeToggleReply`); an untagged
snapshot uses the received rank-table context. Matching new-guild metadata survives the appearance while old
bonus items retire, and a future guild's state cannot authorize changing the current guild's benefits.
Joining a different guild also clears the old rank until the new GuildRank appearance confirms it; a previous guild's
leader rank cannot authorize actions during split delivery of the two appearances.
The confirmed rank permissions and names survive a zone change (EQEmu's zone-in sends them all again,
`SendGuildRanks` / `SendGuildRankNames`) and are cleared at a session start, a disconnect or a guild change. A
staged table that no GuildID appearance confirmed is dropped at a zone change: untagged rank names go to a staged
table while one exists, so a stale one would otherwise take the current guild's later names.

A guild platinum donation has one outstanding request at a time. As in the original
(`Send_OP_GuildTributeDonatePlat` @0x483e80), it is sent only while carried platinum covers the amount (gold,
silver and copper do not count), and the master window's Donate button is greyed until the reply
(`CTributeMasterWnd` +0x2d8, `UpdateDonateButton` @0x785f40). Its 12-byte reply retains both quantity and
favour. A quantity of -1 clears the request without a debit, as @0x483cd0 does. A client safety deviation: the
original clears the request on any reply, but here only a matching quantity debits the purse, updates the last
donation and clears it; malformed, unmatched and repeated replies leave it alone, since the reply names no
transaction. No coin is removed before the answer. A second deviation: the original subtracts carried platinum
only, but EQEmu (`TakeMoneyFromPP`) takes copper first and makes change without a coin update, so the client's
debit follows the server's. Session reset or a later guild-master open reply retires an unanswered request (the
zone connection orders that open after any earlier donation answer); a local timeout cannot identify a new
transaction. The existing decoder fuzz tests and mock tests cover malformed replies, overlap, duplicates, the
greyed button, making change and reset. No server changes are needed.

### Mercenaries (`game/mercenary.{h,cpp}`, `GameConnection` in `net/game_connection_merc.cpp`, windows in `app/lua_eq_merc.cpp`)

The handlers and senders are checked against 25 recorded vector files (`tests/merc_vectors_test.cpp`, 0 mismatches). EQEmu's `patch_RoF2.conf` names only fourteen of the opcodes, the first thirteen and the switch 0x1b37 (in `net/opcodes_rof2.inc`); the others have no EQEmu name and are constants in `game/mercenary.h` (`kOpMerc*`). The test server has `Mercs:AllowMercs` false and no class-71 NPC, so none of the server's answers were seen live; on zone-in the client sends 0x31e4 and 0x4820 {8} (unanswered).

A mercenary record (merchant entries and owned mercenaries) on the wire, each field read only when it fits: u32 id,
u32 type (dbstr 21), u32 subtype (dbstr 22 tier, 23 description), u32 purchase cost, u32 upkeep cost (gold; shown as
cost / 10 platinum and cost % 10 gold), u32 membership (0 free, 1 silver, 2 gold), u32 alt purchase, u32 alt upkeep,
u32 alt currency id, u8, u32 time left (ms), u32 command (echoed in OP_MercenaryCommand), u32 stance (dbstr 24), u32
stance count, u32, u8 auto-assist, cstr name (63 kept), stance count × {u32 index, u32 stance}. The record holds 10
pairs; this client skips any further ones (stance count kept as sent).

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| MercenaryDataRequest | C→Z | 0x11c1 | u32 liaison spawn id; sent when the merchant window shows (a right click on a class-71 NPC within 20 opens it) |
| MercenaryDataResponse | Z→C | 0x72ce | u32 type count, u32 types, u32 entry count, entries. No types: "You do not meet the requirements to purchase any mercenaries." (with a liaison open) and the merchant window hides; a leftover length logs "ERROR: Corrupt MSG_MERCENARIES."; event 0xd fills the window. A type list or a record's stance pairs stop where the bytes do (a count up to 2^32 over a short tail is not run through) |
| MercenaryDataUpdate | Z→C | 0x61a4 | i32 current index, u32 count, entries, u32 slot count when it fits. No entries: "You do not currently have a mercenary." and the manage window hides; an index inside the list becomes the active mercenary (upkeep tick reset, event 0x13, event 5 or 2 by state), else -1 |
| MercenaryDataUpdateRequest | C→Z | 0x7b89 | empty; sent by OP_MercenaryTimer |
| MercenaryHire | C→Z | 0x7169 | 16 B: u32 selected entry id, u32 mercenary id, u32 liaison id, u8 0 + 3 bytes left unset on the wire (0 here) |
| MercenaryHire | Z→C | 0x7169 | u32 code: 0 hired (time left = the timer interval, event 0x12); 1..10 and 15..22 literal messages; 11 / 12 upkeep charged in coin / alternate currency; 13 out of funds, suspended (event 2); 14 upkeep waived; above 22 nothing |
| MercenarySuspendRequest | C→Z | 0x4407 | u8 '0': suspend, unsuspend and revive |
| MercenarySuspendResponse | Z→C | 0x6f03 | u32 restoration timer: state 1, event 2 |
| MercenaryUnsuspendResponse | Z→C | 0x27a0 | (not read) state 5, active since FastTime, event 3 |
| (suspended) | Z→C | 0x0e52 | (not read) state 2 unless 0, 1 or 4; "Your mercenary has been suspended."; timer 0; event 6 |
| (died) | Z→C | 0x038f | u32 timer: state 0, event 5 |
| (timer over) | Z→C | 0x2749 | (not read) timer 0, event 8 |
| (event 7) | Z→C | 0x20b9 | (not read) event 7 |
| MercenaryDismiss | C→Z | 0x6e83 | empty. With DismissMercenaryConfirm (eqclient.ini, default on) eqstr 7612 asks first; then locally: flag 0, state 5, "Your mercenary is being dismissed!", event 1 |
| MercenaryDismiss | Z→C | 0x6e83 | (not read) the local dismissal |
| MercenaryTimerRequest | C→Z | 0x31e4 | empty, on zone-in, then 0x4820 {u32 8} |
| MercenaryTimer | Z→C | 0x0763 | 20 B: +0 has a mercenary (**only the low byte is read**; EQEmu puts the entity id there, so an id with low byte 0 reads as none), +4 upkeep interval (ms), +8, +0xc state, +0x10 restoration timer. A data update request when the flag goes 0 → set or an active state meets a packet state other than 5; a set flag, a clear packet flag and state 5 dismiss locally |
| (time left) | Z→C | 0x2b4f | u32 time left (ms); the upkeep tick reset, event 0x13 |
| (upkeep charged) | Z→C | 0x5f88 | u8 alternate currency: state 5, the charged report, event 4 |
| (costs) | Z→C | 0x3cbc | u32 purchase, alt purchase, upkeep, alt upkeep; event 0x1a |
| MercenaryCommand | C→Z | 0x27f2 | {u32 record command, u32 stance}: a stance list row chosen (its row green 0xff00ff00, the old one default) |
| MercenaryCommand | Z→C | 0x27f2 | u32 stance: event 0x10 |
| (auto-assist) | C→Z | 0x0b72 | u8: the record's byte, event 0x15 |
| (call for assistance) | C→Z | 0x7e6f | u8 '0' (/mercassist without an argument) |
| (switch) | C→Z | 0x1b37 | u32 index; refused "Already using that mercenary!" for an index >= the owned count or the current one |
| (switch) | Z→C | 0x1b37 | {+0, i32 seconds, u32 result}: 0 with seconds > 0: state 1, timer = FastTime + seconds; 1..5 messages; nonzero: event 0xc |
| (set owner) | C→Z | 0x5df8 | 28 B: own spawn id, target spawn id, 5 zero dwords |
| MercenaryUnknown1 | Z→C | 0x5d26 | u8: event 0x19 |
| (merclog) | C→Z | 0xee80 | empty, /merclog |

Not built: the ownership transfer offer and its answer (0x5df8 from the server, 0x4333) and the upgrade prompt
(0x69ca, 0x6e9f).

### Real estate: housing, guild halls, placed objects (`net/real_estate_packets.h`, `GameConnection` in `net/game_connection_real_estate.cpp`, state in `game/real_estate.{h,cpp}`)

None of these opcodes are in EQEmu's RoF2 table, and a stock EQEmu server sends none of them. A server with housing, such as Project Triune, sends them with the original's numbers. The layouts come from NMS-Release-Extras' research headers (`Housing.h`, `HousingNpc.h`; THIRD_PARTY.md) and were checked against the original client's code; where the two disagree, the client's code wins (the corrections are listed below). Records use u32-length strings with no NUL. Placed objects, escrow and the fixed bodies use NUL-terminated strings. The original reads fail-open, so a short packet leaves defaults and misreads the rest. Hailstone refuses a short packet instead.

| Op | Dir | Payload | Effect |
|---|---|---|---|
| 0x2698 | Z→C | u32 n definitions {u32 id, f01 (the area id that links an estate to an area), price group, upkeep group, capacity, vault, f06..f08, type, category; str name; f11, pet capacity, f13..f16; u32 m × u32}; u32 k cost sets {u32 id, u32 e × {u32 key, u32 l × {u32 currency, u32 sub, u64 amount}}} | the catalog, replaced |
| 0x63fd / 0x6fd0 / 0x4f09 | Z→C | one record: u32 id, f18, parent, definition, guild (-1), f28; str address; str owner; u32 f34; u32 child count; str name; u32 children[count]; u8 n × {u32 currency, f64 balance}; u32 upkeep set, f40; u8 opt out | add / set current (the root) and add / remove |
| 0x3968 / 0x485d | Z→C | u32 re; 7 × {u32 group, u32 bits}; u32 n × {str name, u32 group}; 0x3968 then an optional u32 | an estate's access table |
| 0x087f / 0x1cef / 0x2cf7 | Z→C | u32 re; [0x087f: str name]; u32 n (0x2cf7: one) × item row {u32 re, u32 item, u8 placed, u32, 7 × u32 counters, str, str name, the item as in OP_ItemPacket after its type} | an estate's item rows |
| 0x1967 / 0x1baf / 0x7d28 | Z→C | {u32 re, cstr name} / {u32 re, u32 item} / u32 re | rename / an item gone / forget the rows |
| 0x1228 / 0x66bf / 0x68ba | Z→C | i32 code | result / sell / transfer: the code's eqstr in colour 13 (an unlisted 0x1228 code prints 5223; 0 and -1 print nothing) |
| 0x2691 | both | C→Z 224 B: u32 subtype (0 my plots, 1 my items, 2 a neighborhood row, 4 the plot search), u32 id, u32 0, char[80], char[64] (the plot search's name), char[64], u8 flag, u8 1, 2 B. Z→C: u32, u8 owner type; not 4 / 8: i32 target (4 / 5 plot list; 0 / 1 neighborhoods; 2 members; 3 nothing); 4 / 8: one record (a known id only gets its owner and guild) | searches and their answers |
| 0x45be | both | C→Z the 0xbc-byte body {re, …, +0xac price key, +0xb0 upkeep key}; Z→C u8 | buy; a non-zero answer shows the manage window |
| 0x792c / 0x507f | C→Z | the 0xbc-byte body: {re, -1, -1} / {re, the target's name at +0x6c} | sell, transfer |
| 0x2aa6 | both | u32 action, re, f8; cstr address (≤ 63), cstr owner (≤ 31); u8; u32; u32 n × {u32 currency, u32 coin type (3 platinum .. 0 copper), u64 amount} | escrow. C→Z: 0 deposit the cursor's coin, 1 withdraw one denomination, 3 Add Day, 4 Add Week, 7 Withdraw All (23 B with no entry, 39 with one). Z→C: with the u8 clear and action 0, or set and action 1, the first coin entry becomes the cursor's coin of its type |
| 0x2aa7 / 0x3e00 / 0x66df, 0x76c6 / 0x0c13 | Z→C | u32 n × u32 / {u32 id, the byte at +0x68} | the character's owned-estate lists A, B, C, replaced; A and B toggled |
| 0x5a63 | both | the 0xbc-byte body (+0x0c the giver) | "Do you wish to accept %1 from %2?" (7636, 5 min); the answer echoes it with +0xb9 = yes (a no also prints 7638) |
| 0x1045 / 0x2a9e | Z→C | EQPlacedItem {cstr tag, u8 NPC item, u32, u32, cstr key, u32 re, u32 item, f32 scale, heading, pitch, roll, x, y, z, u8 no collision, u8 wall only, u8 no free mode, u32 wall allowed, 7 × u32} / ≥ 0x28 B with re at +0x20, item at +0x24 | a placed object added or updated / removed |
| 0x49df | Z→C | u32 n × {u32 id, f32[16] row-major local → world, 5 × i32} | the areas, replaced |
| 0x21ec | both | 768 B: cstr key, slot {-1, -1, 0xffff} at +0x14, +0x20 re, +0x24 item, +0x2c name (reply), 40 entries at +0x6c of {type, parameter, text id, sub-menu parameter} (request: 0xff), +0x2ec a filter byte (request: 1), +0x2f0 / +0x2f4 the click, +0x2f8 one more field | an estate item's context menu (ShowItemContextMenu @0x7375f0, 0x542a90) |
| 0x050c / 0x51e3 / 0x64a3 / 0x7ad1 | C→Z | {re, item, the entry's parameter} / 0x7ad1 {re, item, 140 zero bytes} | the menu's entry types 8, 9, 0x13 and 5; meaning not known |
| 0x58c7 | both | C→Z {re, item}; Z→C an item | inspect |
| 0x2b35 | Z→C | u8 mode | toggles the plot search window (only in a neighborhood: 8078 otherwise) |
| 0x218c / 0x796c / 0x6573 / 0x3146 / 0x3424 / 0x2036 / 0x6917 / 0x613d | C→Z | {id, secondary, 0} / u32 plot / {re, u32} / {re, access table} / {char[18] key, slot {-1, -1, 0xffff} at +0x14, +0x20 re, +0x24 item, +0x28 0, +0x2c dest: 3 yard (shed), 2 house (closet), 6 crate} / the same without the destination / empty / u8 pack | travel, plot select, opt out, save access, to storage / inventory, store all, pack the crate |
| 0x1da9 | C→Z | 4 B (the original leaves them uninitialised) | /leaverealestate |
| 0x626e | C→Z | 60 B: +0x00 the item's serial, +0x14 the item index (the cursor: {0, 33, -1, -1}), +0x20 re, +0x24 item (-1 for a cursor item), +0x2c 1, +0x30 1, +0x34 / +0x38 the mouse's screen x, y | asks to place an item (a left click with a placeable item on the cursor, or the items window's Place) |
| 0x626e | Z→C | EQPlacedItem (as 0x1045); a 7-byte item index (u8 location, 3 × u16); 3 × u8; 3 × u32; u32 n and n + 1 × {u32 area key, u32 code} | starts placement mode; the codes judge where the item may go |
| 0x711a | C→Z | 0x60 B: the object's key and ids, +0x14 the 12-byte item index, heading, pitch, roll, scale, x, y, z, the actor's box at +0x48 (max) and +0x54 (min) | confirms a placement (only with a code of 0) |
| 0x6213 | Z→C | any | ends placement mode (ItemPlacement_Stop) |

Occupancy, once a second (`RealEstateManagerClient_Process`):
1. The current estate is the first area holding the player whose estate (a record whose definition's f01 is the area's id, at or under the root) is a plot or guild plot. Without one it is the root.
2. The house is the current estate if it is a house, else a house among a plot's children.
3. The yard is the nearest plot or guild plot at or above the current estate.
4. The crate is the first crate below the yard.

The buy, sell and transfer checks print the original's refusals before anything is sent; the server makes the final decision.

Where the client's code corrects the headers:
- Escrow is one serialized object in both directions, not a fixed struct.
- The server's escrow reply sets the *cursor's* coin, not the purse.
- The house is found from the current estate, before the yard.
- f01 is the area link.
- Sell code 4 is 0x149c and code 1 falls through to the default.
- Unlisted 0x1228 codes print 5223.
- The search request is 224 bytes.

Placement mode (`game/placement.{h,cpp}`) follows 0x5ae160's modes (collision, wall, cursor, free) and checks the item's box against the areas named in the reply's code map: an area holding all 8 corners with code 0 allows it; otherwise the last such area's code, otherwise the root estate's code, or 1. A player inside the box gives 0x1c. A 0x1228 {0} or 0x6213 ends it, and so does a zone change. Escape cancelling it is Hailstone's own.

Not built: the ghost's fit to walls and surfaces (ItemPlacement_UpdatePosition), free mode's mouse moves, the 0x21ea NPC-item binding to a placement, the polls, `/outputfile realestate`'s rows.

### Spawn (ENCODE(OP_ZoneSpawns))

cstr name; u32 spawn id; u8 level; f32 melee radius (size − 0.7); u8 type (0 PC, 1 NPC, 2/3 corpse); u32 bitfield (gender:2, pet, afk, anon:2, gm, sneak, lfg, betabuffed, invis:12 at 10, linkdead 22, showhelm 23, unknown 24, trader 25, animation-on-pop 26, targetable 27, targetable-with-hotkey 28, showname 29, idle-off 30, click-through 31); u8 OtherData (bit0 buyer, bit1 offline mode (EQEmu never sets it), bit2 LDoN chest/interactive, bit4 title follows, bit5 suffix follows; destructibles 0xe1); f32 −1; u32 0; [interactive: 3 cstr, u32 appearance, u32 animation, 10×u32 sounds, u8 collidable, u32 type]; u8 property count + count×u32 (EQEmu: 1, body type; 0 for destructibles); u8 HP %; 6×u8 hair colour, beard colour, eye 1, eye 2, hair style, beard; 3×u32 drakkin heritage, tattoo, details; u8 texture type (equip_chest2), u8 0, u8 0, u8 helm; f32 size; u8 face; f32 walk; f32 run; u32 race; u8 holding; u32 deity; u32 guild id, u32 guild rank (NPC: 0xffffffff, 0); u8 class, pvp, stand state, light, fly mode; cstr last name; u32 AA title; u8 guild show; u8 temp pet; u32 pet owner; u8 FindBits; u32 player state, npc tint, primary tint, secondary tint; 2×u32 0xffffffff; equipment: PC or PC-model race (≤12, 128, 130, 330, 522) → 9×u32 tints + 9×{5 u32 texture}, else 3×{5 u32} (slot 0 zero, primary, secondary); position 5 dwords: [0] y:19 at 12; [1] Δx:13 at 0, Δheading:10 at 13; [2] Δy:13 at 0, z:19 at 13; [3] x:19 at 0, animation:10 at 19; [4] heading:12 at 0, Δz:13 at 12; [title cstr]; [suffix cstr]; 2×u32; u8 mercenary; cstr "0000000000000000"; 2×u32 0xffffffff; 29 zero bytes, which the client reads as u8 always-show-aura, u32 physics effect count (+16 B each), 6×u32 spawn status.

### PlayerProfile (ENCODE(OP_PlayerProfile))

u32 checksum (eq_crc32 of bytes [8, size−1)), u32 size−9, 2×u32; u8 gender, u32 race, u8 class, u8 level ×2; u32 5 + 5 binds {u32 zone, f32 x, y, z, heading}; u32 deity, u32 intoxication; u32 10 + 10×u32; u32 22 + 22×{5 u32} equipment; u32 9 + 9×{5 u32}; u32 9 + 9 tints; u32 9 + 9 tints; u8 hair colour, beard colour, u32, eye 1, eye 2, hair style, beard, face; 3×u32 drakkin; 5 B; f32 height, 3 f32, 2 u32; u32 points, mana, current HP; STR, STA, CHA, DEX, INT, AGI, WIS; 7 u32; counted arrays: AAs (12 B each), skills, innate skills, disciplines, timestamps, recast timers, timestamps 2, spell book, memorised gems, gem refresh; u8; counted buffs (80 B each: u8, f32 instrument mod, u32 caster, u8, u32 counters, u32 duration, u8 level, u32 spell, u8 effect type, u32 hits, u32, u32 counters, 44 B); coin ×4, cursor coin ×4, 3 u32, thirst, hunger, AA spent, u32 5 + assigned + 5 tab values (last = unspent), 2 B; counted bandoliers (cstr + 4×{cstr, u32 id, i32 icon}, kept as `PlayerProfile::bandoliers`); counted potion belt ({cstr, u32, i32}, `potion_belt`); 4 i32; 12 u32 (resists etc.); u32 20; 4 u32; endurance; 2 u32; u32 64 + `char[64]` name; u32 32 + `char[32]` last name; birthday, account start, last login, minutes played, entitled, expansions; counted languages (bytes); u16 zone, u16 instance, f32 y, x, z, heading; u8, pvp, u8, gm; u32 guild, u8 rank, u32, u8, u32; u64 exp; u8; bank coin ×4, shared platinum, … `character_id` after 663 unknown bytes; … AA exp; …; u8 show helm; u32 rest timer; counted 1024 B; 2 u32; u8. The decoder reads every array by its own count prefix, so it follows the server's sizes (20 bandoliers × 4 items is the one fixed count).

### Fellowships

`net/fellowship_packets.*` handles the native fellowship messages independently of whether a server implements
them. Stock EQEmu does not send them; EQEmu PR #5134 "TOB: Fellowships" (open, unmerged, GPL-3, read only as a
protocol reference) does. All six routes also have generated symbolic opcode mappings. Servers without fellowships keep an empty
state. Requests use opcode `0x7121` with a 1076-byte body:
action u32 at 0, target u32 at 4, a NUL-terminated MOTD in the 1024-byte field at 20, and camp type u32 at
1064. Unused fields are zero. Actions are create 1, MOTD 2, accept 3, invite target spawn 5, remove character 6,
promote character 7, create camp 8, destroy camp 9, leave 10, disband 11 and toggle sharing for character 12.
Requests never change membership locally. The server enforces its member limit and validates leadership,
sharing, invitations, cooldowns, camp supplies and placement requirements.

The invitation opcode is `0x62ab`, 0x434 bytes; `Handle_Opcode_0x62ab` @0x667db0 reads only the inviter spawn at
12 and shows no dialog when that spawn is not found. The dialog expires after 100 seconds. Acceptance sends
action 3 and declining action 4, with no target argument (`Send_Opcode_0x7121` @0x668710). PR #5134 reads 4 as
a legacy camp destruction and has no decline handler; the client still sends what RoF2 sends.

**Extension beyond RoF2:** a 1080-byte invitation (PR #5134's form: action 5 at 0, inviter at 4) is accepted
too. Its inviter is taken from 12 when set, else from 4, and when that spawn is not in the zone the dialog is
shown without a name, since such a server may invite across zones. The 0x434-byte form never takes 4 and never
shows a nameless dialog.

Roster opcode `0x6e4d`, version 5, has a 1100-byte header: version, fellowship id, name[64], MOTD[1024], count.
Each of the twelve native member records is 84 bytes: character id u32, name[64], zone u32, level u32, class u32,
last-seen epoch u32. The first record is leader. Records are followed by a u32, twelve 32-byte player handles,
and three twelve-byte arrays (sharing, capped, offline). A 32-byte campsite trailer contains y/x/z floats,
zone, expiry, a second expiry, an unused word and active, all remaining fields u32. Full native size is 2564
bytes; the 2532-byte roster without camp is also accepted. A matching roster that omits this character clears
membership; an unrelated roster cannot replace or clear it. Zone changes preserve membership until the next
server roster, but discard pending invitations and kit queries. A session end clears the state.

**Extension beyond RoF2 (capacity):** all member arrays may instead have 13 through 64 slots. Capacity is inferred
from exact length: `1104 + 119 * slots` without camp, `1136 + 119 * slots` with camp. Count cannot exceed capacity;
duplicate identities, unknown versions, malformed lengths and more than 64 slots are rejected. There is no new
admission rule or client-side 12-member cap. A server sending expanded rosters must expand every array using
this contract. PR #5134 hard-caps membership and native serialization at twelve; raising
its rule alone does not enable larger fellowships. Expanded rosters are covered by synthetic tests, not a live
64-member server session.

Camp refresh `0x0c12` has two u32s: available-only and category mask (all bits for all categories). Reply `0x074d`
is count u32 followed by available u8, camp type u32, description id u32 and a NUL-terminated display name. The
description id is a DBString type-0 lookup, as checked in the original selection handler. Category labels come
from the player's layered `Resources/campsitecategories.txt` (`id^label`); selecting a category sends `1 << id`.
An absent category file leaves the all-category query available; a missing DBString falls back to the supplied
kit name. Vitality `0x2cc6` is 28 bytes: normal
u64, normal cap u64, AA u32, unused u32, AA cap u32. The profile seeds normal/AA vitality from the sixteen bytes
after AA experience (last fellowship join u32, normal u64, AA u32); the dedicated message updates values and caps.
Native gauges 147/148 display `min(pool, cap) * 1000 / cap`, truncated; zero caps draw empty. The client computes
that presentation ratio without overflowing 64-bit values and keeps exact amount strings for Lua. No XP award
or vitality transfer arithmetic is performed locally. Fellowship chat uses channel 21 and colour 342.

## 5. Notes

- **Item ids above 0xFFFFF**: ids that large make item links carry 8-digit ids (`SAY_LINK_BODY_SIZE` 77 in `rof2_limits.h`). `game/item_links.cpp` reads both widths.

## 6. Uncertain

- After a zone-line request, one live run printed two `OP_ZoneChange` success answers and opened a second world session, after which the server dropped the character as link-dead. `GameConnection` now ignores a success answer while already zoning. A possible source of the second answer is a second request (not confirmed): the client used to drop a zone change after 7 s without an answer (a zone that was still booting), and a second trigger then sent another; it now waits 180 s.
- Destructible spawns (OtherData 0xe1): EQEmu writes the interactive block although bit 2 is clear and sets bit 5 (suffix) without writing a suffix; the decoder treats `(other & 0xe0) == 0xe0` as interactive and suffix-less. Untested against live data.
- OpenEQ's spawn decoder unpacks the spawn position with the PlayerPositionUpdateServer bit layout; we follow rof2_structs.h `Spawn_Struct_Position`, which is what the server writes. Check against a live capture.
- Client `OP_ClientUpdate` Δheading scale (×20) is taken from the server's `EQ10toFloat`, used only for boats; the value is not verified against a RoF2 client. Animation is passed raw.
- `ChannelMessage` from the client: we send the server's ENCODE layout; a RoF2 client's trailing bytes are not verified (the server ignores them).
- LoginInfo +192 = 0xcc may be a dword. The server ignores it.
