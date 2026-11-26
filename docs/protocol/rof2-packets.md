# RoF2 world and zone packets

What `src/net/world_packets.*` and `src/net/zone_packets.*` encode and decode, and where each layout comes from.

- The server is EQEmu. Opcodes are from stock EQEmu's `utils/patches/patch_RoF2.conf`, generated into `src/net/opcodes_rof2.inc` by `tools/gen_opcodes.py`.
- "rof2.cpp" means `common/patches/rof2.cpp`, the server's RoF2 translator. Opcodes without an ENCODE/DECODE there are passed through, so the layout is the server's own struct in `common/eq_packet_structs.h`.
- All values are little-endian. "cstr" is a NUL-terminated string, `char[n]` a NUL-padded fixed field.
- Sizes in this file are payload sizes, without the 2-byte opcode.

## 1. Sequence

### World (world/client.cpp)

1. C→W `OP_SendLoginInfo` (464 B). EQEmu picks the RoF2 patch from this first packet's opcode and length.
2. W→C, in order:
   - `OP_GuildsList` (not when zoning), `OP_LogServer`;
   - `OP_ApproveWorld` (fixed 544-byte blob; the client may ignore it);
   - `OP_EnterWorld` (name; empty unless auto-login or zoning);
   - `OP_PostEnterWorld` (empty; "access granted").
   - Not zoning: `OP_ExpansionInfo`, then `SendCharInfo`: `OP_SendMaxCharacters`, `OP_SendMembership`, `OP_SendMembershipDetails`, `OP_SendCharInfo`. `OP_SendMembership` and `OP_SendMembershipDetails` are kept (`GameConnection::membership()`, section 2); EQEmu always sends gold.
3. C→W `OP_World_Client_CRC1/2/3` after each character list, and CRC1 again before every `OP_EnterWorld`.
4. C→W `OP_WorldClientReady` (empty) once the character select screen is up, after the first list's checksums. EQEmu's world eats it.
5. Character creation (section 3).
6. C→W `OP_EnterWorld` (72 B). W→C `OP_MOTD`, `OP_SetChatServer`, `OP_SetChatServer2`, (an `OP_GroupUpdate` for grouped characters), then `OP_ZoneServerInfo` or `OP_ZoneUnavail`. An `OP_ZoneServerInfo` at any other time (before `OP_EnterWorld`, or a second one) is logged and ignored.
7. C→W `OP_WorldComplete` (empty). World closes the connection (EQEmu: `eqs->Close()`). The client connects to the zone only after world has dropped the link, and gives up after 30 s ("World did not drop the connection"). Without it, EQEmu drops the idle world client about 3 s later.

While zoning (`LoginInfo::zoning = true`), world skips the guild list and character list. The client re-sends CRC1 and `OP_EnterWorld` with the name world sent in its `OP_EnterWorld`.

### Zone-in (zone/client_packet.cpp)

1. C→Z `OP_ZoneEntry` (76 B; the RoF2 zone signature).
2. Z→C (`Handle_Connect_OP_ZoneEntry`): `OP_PlayerProfile`, then the player's own spawn as `OP_ZoneEntry`, then every other spawn and corpse as one `OP_ZoneEntry` each, `OP_TimeOfDay`, tributes, inventory (`OP_CharInventory`), `OP_XTargetResponse`, `OP_Weather`, and, for RoF and later, `OP_NewZone` without being asked.
3. The client waits for its own spawn and `OP_Weather` (`OP_Weather` is ignored until its own spawn arrived), then sends `OP_ReqClientSpawn` (empty).
   - First come `OP_SendAATable` / `OP_UpdateAA` (empty; the server answers with AA data).
   - On the **first zone-in of the session only**: `OP_SendTributes` then `OP_RequestGuildTributes` (4 bytes each, unused; the guild list is asked for in a guild or not). Later zone-ins keep the lists. EQEmu answers both only while the client is connecting (`ConnectingOpcodes`).
   - RoF2 clients also send 0x2b8e, 0x3a02 (10 B), 0x4f05 (18 B) and 0x7273 around them; none is in EQEmu's RoF2 table, and this client sends none.
   - **`OP_ReqNewZone` is never sent by the RoF2 client.**
4. Z→C (`Handle_Connect_OP_ReqClientSpawn`): doors, objects, zone points, `OP_SendAAStats`, `OP_SendExpZonein`, `OP_ClientReady`, `OP_WorldObjectsSent` (all empty). `OP_ZoneServerReady` has no RoF2 opcode and is dropped.
5. C→Z echo `OP_SendExpZonein` (empty) on receiving it, and echo `OP_WorldObjectsSent` (empty) on receiving it. The second echo makes the server send the zone-in packets (`SendZoneInPackets`: appearance, HP, mana, etc.).
6. C→Z `OP_ClientReady` (empty), once `OP_SendExpZonein` was seen; the server finishes the connect (`CompleteConnect`) and sends an HP update. The client then waits for the server's `OP_ClientReady` (sent in step 4). Neither wait has a timeout.
7. In zone: `OP_ClientUpdate` (46 B) when moving.

### Zoning, camp

#### Server-initiated zoning

Z→C `OP_RequestClientZoneChange` (#zone, #goto, summons; zone dword = zone | instance << 16, position, heading, type).

- A request for the zone the player is in moves the player there and sends nothing (the next `OP_ClientUpdate` tells the server).
- Another zone makes the client answer `OP_ZoneChange`.

The player's own zone dword takes its instance from the player profile (`zoneInstance`), not `OP_NewZone` (stock EQEmu leaves that 0).

#### Zone lines

Client-initiated: `game/zone_lines.{h,cpp}`, `GameConnection::move_to_zone`.

1. During zone-in the zone sends `OP_SendZonepoints`, its zone points' targets.
2. When the player's feet enter a zone-line region of the zone WLD (`docs/formats/wld.md`, "Regions"), the region's tag gives the zone, instance and target position: a "zone 255" tag through the zone-point table by number, otherwise directly.
3. The client sends a reliable C→Z `OP_ClientUpdate` with its crossing position, including any legacy coordinate adjustment, then C→Z `OP_ZoneChange` {zone, instance, target y, x, z, reason 0, success 0}. The update goes on the reliable stream, as DoTeleportB @0x507ca0 sends it. No movement reports go out while any zone change waits for its answer, retries included: the original blocks in `RequestZoneChangeStatusBlocking` @0x52cee0 and its main loop does not run.

The EQEmu server (`zone/zoning.cpp` `Handle_OP_ZoneChange`, zone_mode ZoneUnsolicited) checks only the zone:

- It takes the zone point **nearest to the last reported position** among those leading to that zone (`zone/zone.cpp` `GetClosestZonePoint`, range 40000).
- It refuses the current zone (`SendZoneCancel`).
- It ignores the coordinates sent; the arrival point is that zone point's target, with each `999999` replaced by the corresponding reported coordinate.
- If its water map does not put the reported position in a zone line, it logs "Closest zone point ... WARNING" and reports `MQZoneUnknownDest` to its cheat manager but still zones the player.

#### The zone change

1. C→Z `OP_ZoneChange` (success 0).
2. Z→C `OP_ZoneChange` with success 1 → C→Z `OP_DeleteSpawn` (4 bytes: own spawn id).
3. Wait up to 120 s for the zone to drop the link (after that the client closes it).
4. Reconnect to world with `zoning = 1`, then `OP_EnterWorld` again.

EQEmu's `Handle_OP_DeleteSpawn` tells the other clients `OP_DeleteSpawn` and drops the link at once (its `OP_LogoutReply` has no RoF2 opcode).

The original may send `OP_SaveOnZoneReq` (0x600d, 484 B) before it; this client does not (EQEmu saves on the zone change anyway).

The client waits up to **180 s** for the answer. An answer of −1 (not ready) within 7 s of the first request is followed by the same request 500 ms later. EQEmu answers only once the target zone is up, which can take seconds when world boots it (`world/zoneserver.cpp` ServerOP_ZoneToZoneRequest).

#### Camp

`GameConnection::camp`:

1. `/camp` sits the player, then sends C→Z `OP_Save` (0x4a39, 484 B, kind 0x21; table below) and `OP_Camp` (u32 kind: 0 character select, 1 `/camp desk`, 2 `/camp server`, from the argument's first four letters).
2. After a 30 s timer: `OP_Logout` (empty), and up to 3 s for the zone to drop the link (without closing it).
3. If the link is still up, the client sends `OP_SaveOnZoneReq` (kind 0) and `OP_DeleteSpawn` (own id), unless the respawn window is up. It waits up to 120 s more (then closes it).

There is no camp reply. EQEmu's `Handle_OP_Logout` drops the link at once (`OP_LogoutReply` and `OP_PreLogoutReply` are opcode 0 in `patch_RoF2.conf`), so step 3 never happens there.

Then, by kind:

- Kind 0 goes back through world: `OP_SendLoginInfo` with zoning 0 on the same `WorldTicket`, which shows character select (`ClientApp::camp_finished`).
- Kind 1 closes the client.
- Kind 2 goes back to the front end (the login screen).

#### Link death

A link drop during the camp's 30 s is link death, as anywhere in game. Once the link's status is 1 (the 180 s no-data timeout, or the server's disconnect), the client:

1. logs "disconnected at %s:%u (char. select) %s";
2. shows a disconnected screen (the screen cleared, string 12353 "YOU HAVE BEEN DISCONNECTED." in the middle) for 3 s;
3. waits 16 s more and leaves the camp's way (0 character select, 1 the desktop, 2 the front end; a cancelled camp has reset it to 0).

It sends no `OP_SaveOnZoneReq` / `OP_DeleteSpawn`. Outside the game (character select, the zone-in waits) it is the same screen and 3 s, then the front end. A link that never connected is not a link death. Code: `GameConnection::link_death`, `ClientApp::update_link_death`.

#### Closing the window

- At character select or creation, it quits.
- In any other state it is `/camp desk` with its checks (feigning, the sit).
- A second close during the camp abandons it (12290), as a second `/camp` does (`ClientApp::window_close_request`).
- SDL's close request is held; a signal's quit still quits at once.

## 2. World packets

| Packet | Dir | Opcode | Layout as implemented |
|---|---|---|---|
| SendLoginInfo | C→W | 0x7a09 | 464 B. +0 `char[64]` "account\0key\0"; +64 `char[124]` custom-files key (NUL-terminated, so at most 123 characters; empty normally); +188 u8 zoning; +192 u8 0xcc; rest 0 |
| LogServer | W→C | 0x7ceb | 560 B. +4 u8 pvp, +8 u8 enable_FV, +36 `char[32]` world short name |
| ApproveWorld | W→C | 0x7499 | 544-byte constant; not decoded |
| EnterWorld | W→C | 0x578f | cstr name |
| PostEnterWorld | W→C | 0x6259 | empty |
| ExpansionInfo | W→C | 0x590d | 68 B, u32 mask at +64 |
| SendMaxCharacters | W→C | 0x5475 | 12 B, u32 max at +0 |
| SendMembership | W→C | 0x7acc | 116 B: u32 level (0 free, 1 silver, 2 gold), u32 races, u32 classes, u32 count, count × i32 (EQEmu 25; the client keeps the first 22) |
| SendMembershipDetails | W→C | 0x057b | u32 count, count × {u32 level 0..2, u32 feature, i32 value} (EQEmu 72, the last 6 zero); u32 count, × {u32 purchase id, u32 bits} for races, the same for classes; two strings (u32 length + text, EQEmu empty) |
| GuildsList | W→C | 0x507a | 64 zero bytes, u32 count, {u32 id, cstr name}… (note 1) |
| SendCharInfo | W→C | 0x00d2 | u32 count, then per character: cstr name + 274 B: u8 class, u32 race, u8 level, u8 shroud class, u32 shroud race, u16 zone, u16 instance, u8 gender, u8 face, 9×{u32 material, unknown1, elite, hero forge, material2, u32 tint}, u8 ×2 (0xff), u32 drakkin tattoo, details, deity, primary id file, secondary id file, u8 hair color, beard color, eye1, eye2, hair style, beard, go home, tutorial, u32 heritage, u8 unknown, u8 enabled, u32 last login, u8 unknown |
| World_Client_CRC1/2/3 | C→W | 0x0f13 / 0x4b8d / 0x298d | 2056 B: u32 eq_crc32(file), u32 size, 256×u32 dword index, 256×u32 dword value |
| WorldClientReady | C→W | 0x23c1 | empty, once at character select |
| EnterWorld | C→W | 0x578f | 72 B: `char[64]` name, u32 tutorial, u32 return home (note 2) |
| MOTD | W→C | 0x0c22 | cstr |
| SetChatServer(2) | W→C | 0x1bc5 / 0x7eec | cstr "host,port,short.Character,<type letter><8 hex key>" |
| ZoneServerInfo | W→C | 0x4c44 | 130 B: `char[128]` ip, u16 port |
| ZoneUnavail | W→C | 0x4cb4 | `char[16]` zone name, 4×i16 |
| WorldComplete | C→W | 0x4493 | empty |

1. GuildsList: entries with id ≥ MAX_GUILD_ID are skipped without fixing the count, so the decoder stops at the end of the payload.
2. EnterWorld: the original's Return Home sets byte 0x44 and byte 0x45 (set with either flag), so return home reads 0x101. EQEmu moves the character to its home bind, and drops the connection when the character may not go home.

#### World checksums

EQEmu reads `uint64 checksum` = crc | size << 32 from the first 8 bytes and stores it on the account. It rejects it only when all of these hold:

- `World:EnableChecksumVerification` is on;
- the `crc_*` variables (client executable, skillcaps, basedata) are set;
- the account is below `GM:MinStatusToBypassCheckSumVerification`.

`make_file_checksum()` computes the real value from the player's own files. The full 2056 bytes must be sent. eq_crc32 is the standard reflected CRC-32 table, start 0xffffffff, **no final inversion**.

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

Flow:

1. Request the tables (`OP_CharacterCreateRequest`).
2. Send `OP_ApproveName`; on reply 1 send `OP_CharacterCreate`.
3. World answers with a fresh `OP_SendCharInfo` on success, or `OP_ApproveName` = 0 on failure (the reserved name is then deleted).

What world checks:

- **Name** (`HandleNameApprovalPacket`): 4–15 characters, first letter upper case, the rest lower case, no spaces, passes the name filter, not taken. Race must be a player race and class 1..16, or world **drops the connection** (returns false).
- `OP_CharacterCreate` must be exactly 96 bytes, and the account must have a free slot.
- `CheckCharCreateInfoSoF`: (class, race, deity, start_zone) must be a row of `char_create_combinations` (sent in the table above). Each stat must be within [base, base + budget] of its allocation, budget = sum of the default allocation, and the total spent ≤ budget (`validate_char_create()`; `default_char_create()` builds a valid request).
- **Start position**: `start_zones` row for (zone_id = start_zone, class, deity, race), else defaults. `World:SoFStartZoneID` overrides the zone.

Example from `utils/sql/svn/2024_required_update.sql` (always use the table world sends). Human (1) Warrior (1) may pick:

| Deities | Start zone |
|---|---|
| 396 (Agnostic), 207, 212, 211 | 1 (qeynos) |
| 396, 206, 211, 208, 204 | 10 (freporte) |
| 201 | 45 |
| 201/204/206/207/208/211/212/396 (expansion bit 2048) | 394 (crescent) |

Allocation 58: base STR 85, DEX 75, AGI 80, STA 85, INT 75, WIS 75, CHA 75, default 25 points into STA.

## 4. Zone packets

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| ZoneEntry | C→Z | 0x5089 | 76 B: u32 eq_crc32(name), `char[64]` name, u32 spell CRC, u32 spell-assoc CRC (EQEmu reads only the name) |
| ZoneEntry / NewSpawn | Z→C | 0x5089 / 0x6097 | one spawn per packet (below) |
| MobRename | Z→C | 0x2c57 | 200 B: `char[64]` old name, `char[64]` repeated old name, `char[64]` new name, u32 reserved, u32 apply flag |
| PlayerProfile | Z→C | 0x6506 | variable (below) |
| NewZone | Z→C | 0x1795 | 948 B NewZone_Struct; decoded: names (+0/+64/+192), ztype +470, fog RGB +471/+475/+479 (×4), fog min/max clip +484/+500 (4 floats each), gravity +516, time type +520, rain/snow +521..+536, sky +570, exp mult +584, safe y/x/z +588/+592/+596, max z +604, underworld +608, minclip/maxclip +612/+616, short name 2 +704, zone id/instance +852/+854, underworld teleport +868, suspend buffs +876, lava +880/+884, no bind +889, no attack +890, fall damage +894, fast regen +896.., fog density +916, no levitate +940 |
| TimeOfDay | Z→C | 0x5070 | 8 B: u8 hour, minute, day, month, u32 year (note 1) |
| Weather | Z→C | 0x661e | 12 B: u32 val1 (0xff), u32 type (0x31 rain, 2 snow), u32 mode |
| ReqClientSpawn, SendExpZonein, WorldObjectsSent, ClientReady, SendAATable, UpdateAA, Logout | C→Z | 0x35fa, 0x5f8e, 0x5ae2, 0x345d, 0x66b5, 0x66f0, 0x4ac6 | empty |
| ClientUpdate | C→Z | 0x7dfc | 46 B: u16 sequence, u16 spawn id, u16 vehicle id, 4 B, f32 Δx, u32 heading:12 (heading×4 mod 2048), f32 x, f32 Δz, f32 z, f32 y, u32 animation:10, f32 Δy, u32 Δheading:10 signed (×20) |
| ClientUpdate | Z→C | 0x7dfc | 24 B: u16 spawn id, u16 vehicle id, then 5 dwords: [0] y:19 at bit 12; [1] Δz:13 at 0, Δx:13 at 13; [2] x:19 at 0, heading:12 at 19; [3] Δheading:10 at 0, z:19 at 10; [4] animation:10 at 0, Δy:13 at 10 |
| DeleteSpawn | both | 0x7280 | Z→C 5 B: u32 id, u8 1; C→Z 4 B: u32 own spawn id (note 2) |
| SpawnAppearance | both | 0x0971 | 8 B: u16 id (0 = me), u16 type, u32 parameter |
| ChangeSize | Z→C | 0x4707 | 16 B: u32 id, f32 size, u32 0, f32 1.0 (sent instead of SpawnAppearance type Size) |
| SetBodyType | Z→C | 0x2115 | 8 B: u32 spawn id, u32 property set id (`Resources/CharacterPropertySets.txt`); other sizes are ignored (0x4c5747). Not in EQEmu's table; stock EQEmu does not send it (docs/mechanics.md, Actor property sets) |
| HPUpdate | Z→C | 0x2828 | 10 B: i16 id, u32 current, i32 max |
| MobHealth | Z→C | 0x37b1 | 3 B: u16 id, u8 percent |
| ChannelMessage | both | 0x2b2d | cstr sender, cstr target, u32 0, u32 language, u32 channel, 5 B, u32 skill, cstr message, 15 zero bytes (note 3) |
| SpecialMesg | Z→C | 0x0083 | u8 speak mode, u8 journal mode, u8 language, u32 type, u32 target id, cstr sayer, 3×u32 loc, cstr message |
| FormattedMessage | Z→C | 0x1024 | u32 0, u32 eqstr id, u32 type, ≤9 cstr arguments, u8 0 |
| ZoneChange | both | 0x2d18 | 100 B: `char[64]` name, u16 zone, u16 instance, 2×u32, f32 y, x, z, u32 reason, i32 success, u32 |
| SendZonepoints | Z→C | 0x69a4 | u32 count; count × 32 B {u32 number (zone_points.number), f32 y, x, z, heading (target; 999999 / 999 = keep the player's), u16 zone, u16 instance, u32 (client: non-zero → teleport by index, opcode 0xcadf), u32}; plus one more 32 B entry the client ignores |
| RequestClientZoneChange | Z→C | 0x3fcf | 176 B: u16 zone, u16 instance, u32 0xffffffff, f32 y, x, z, heading, u32 type (0x0b), 144 B, u32 |
| Camp | C→Z | 0x28ec | u32 (0 char select, 1 desktop, 2 server); ignored by EQEmu |
| Save / SaveOnZoneReq | C→Z | 0x4a39 / 0x600d | 484 B: u32 `__MemChecker0` of the 476 bytes from +5, u8 kind (0x21 camp; `SavePCForce` 0), f32 y, x, z, i32 heading, 22 × 20 B armour records, u8 on a vehicle + 3 B, u32 minutes played, i32 primary / secondary actor (IT numbers), i32 air supply, 3 zero bytes. EQEmu's `Save_Struct` is 484 opaque bytes; both handlers call `Save()` |

1. TimeOfDay: the calendar advances from this reference by one game minute per three real seconds (hours 1..24, 28 days per month, 12 months per year), as `EQWorldData::AdvanceTime` and EQEmu's `common/eqtime.cpp` do (`net::advance_time_of_day`, docs/formats/sky.md). `/time` and MQ `GameTime` read it. Each packet replaces the reference.
2. DeleteSpawn C→Z is sent on zoning and camp exit. Received for the target: the target clears and auto attack stops. For the trade partner: the trade is cancelled (OP_CancelTrade {partner, me}); EQEmu tells only the player who left. Checked: msgEQRmPlayer @0x4b8380, CTradeWnd_CancelAndClose.
3. ChannelMessage: the server ignores everything after the message. Sent names are at most 63 characters and the message 2047 (EQEmu's DECODE strcpy's the names into `char[64]`). Checked: SendNewText (0x40, 0x800).

With a nonzero apply flag, MobRename looks up the first old name case-insensitively, including its spawn suffix.
It replaces the name on the same spawn, preserving the spawn ID and target selection. For the player's own spawn it also
renames the character (0x4d5a10): the connection's character name and the profile's take the new name, so the
own-corpse and leadership checks and the next zone's entry use it.

### Combat (`net/zone_packets.h` "combat", `GameConnection::handle_combat`, rules in `game/combat.{h,cpp}`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| TargetMouse | C→Z | 0x075d | 4 B: u32 new target (0 = none). Sent on every target change |
| TargetCommand | both | 0x58e2 | 4 B: u32 target. The server echoes it when it accepts a target (and sends it for pets); refused targets get "You cannot see your target" |
| AutoAttack / AutoAttack2 | C→Z | 0x109d / 0x3526 | 4 B each: u32 1 on / 0 off; both sent when the attack flag changes. EQEmu reads byte 0 of AutoAttack and ignores AutoAttack2 |
| Consider | both | 0x742b | 20 B: u32 player, u32 target, u32 faction, u32 level, u8 pvp, 3 B. See note 1 |
| ConsiderCorpse | C→Z | 0x5204 | Same 20 B layout as Consider, with the player and corpse IDs and the remaining fields zero |
| Damage | Z→C | 0x6f15 | 30 B packed: u16 target, u16 source, u8 type, u32 spell, i32 damage, f32 force, f32 hit heading, f32 hit pitch, u8 secondary, u32 special (1 wild rampage, 2 rampage). See note 2 |
| Death | Z→C | 0x6517 | 32 B, **not translated**: the server's Death_Struct. See note 3 |
| HPUpdate | Z→C | 0x2828 | 10 B (above). Own HP (cur, max) at zone-in and on change, without the items' HP. See note 4 |
| MobHealth | Z→C | 0x37b1 | 3 B: u16 id, u8 percent; for the current target (and group, pets) |
| ManaChange | Z→C | 0x5467 | 20 B: u32 mana, u32 endurance, u32 spell, u8 keep casting, 3 B, i32 slot |
| ManaUpdate / EnduranceUpdate | Z→C | 0x3791 / 0x5f42 | 10 B: u32 current, u32 max, u16 spawn id (the only source of the maxima: the RoF2 profile has none) |
| ExpUpdate | Z→C | 0x20ed | 8 B: u32 experience into the level on the 0..330 scale, u32 tip flag. See note 5 |
| LevelUpdate | Z→C | 0x1eec | 12 B: u32 level, old level, exp |
| SimpleMessage | Z→C | 0x213f | 12 B: u32 eqstr id, u32 colour, u32 unknown ("You gain experience!!") |
| Animation | Z→C | 0x7177 | 4 B: u16 spawn, u8 action, u8 speed (rof2.cpp swaps action and speed relative to the server struct). See note 6 |
| SpawnAppearance (sit) | both | 0x0971 | 8 B: u16 own id, u16 14, u32 110 sit / 100 stand (102 freeze, 105 loot, 111 crouch, 115 lying). The server checks the id is the sender's; it is not echoed to the sender |
| ZonePlayerToBind | Z→C | 0x08d8 | u16 zone (0 = this zone), u16 instance, f32 x, y, z, heading, cstr zone name, u8 1, 3×u32 0 |
| RespawnWindow | Z→C | 0x0ecb | u32 selection, u32 ms left, u32 0, u32 count, count×{u32 id, u32 zone, f32 x, y, z, heading, cstr name, u8 validity (0 available, nonzero unavailable)}, optionally followed by legacy count + 1 zero padding bytes |
| RespawnWindow | C→Z | 0x0ecb | 4 B: u32 chosen option id |

1. Consider: C→Z fills the ids. Z→C: faction 1..9 is already reordered for eqstr 12212.. (the server swaps 6↔9, 7↔8); level = the server's con colour (2/4/6/10/13/15/18).
2. Damage values: 0 miss, −1 block, −2 parry, −3 riposte, −4 dodge, −5 invulnerable, −6 rune. type = `SkillDamageTypes` (0 crush, 1 slash, 4 hand to hand, 7 archery, 8 backstab, 10 bash, 21/23 strike, 30 kick, 36 pierce, 51 throw, 74 frenzy, 231 spell).
3. Death_Struct: u32 spawn, killer, corpse id, bind zone (+12), spell (+16), attack skill (+20, a damage type; 0xfa/0xfb/0xfc burn/choke/fall), damage (+24), unknown. rof2_structs.h's Death_Struct (skill at +12) is not what goes out; this client reads skill at +20.
4. HPUpdate: EQEmu `SendHPUpdate` sends `GetHP() - itembonuses.HP`. The client adds the items' HP back for everything that shows or reports it (`GameConnection::own_hp`, docs/mechanics.md).
5. ExpUpdate tip flag: the original shows the experience tip when it is 2. It is not AA experience, which only OP_AAExpUpdate carries. EQEmu sends 0.
6. Animation: action n is the n-th clip of C01..C11, D01..D05, L01..L09, O01, S01..S05, P01..P07, T01..T09, S06..S28, P08, O02, O03.

Considering an NPC or player corpse sends `OP_ConsiderCorpse`, as the original does (Cmd_consider @0x4df830,
for spawn type 2 and up, without the consider tip). EQEmu's `Client::Handle_OP_ConsiderCorpse` sends the
remaining decay time and, for player corpses, the resurrection time or status. The client displays these
replies through the usual formatted, simple or plain chat message handlers.

`/consider` sends nothing in two cases, as Cmd_consider does. With no target it prints eqstr 13063 ("Consider
whom?"). A living target whose race carries the race table's vehicle flag (PlayerClient vftable slot +0x50,
PlayerClient_IsVehicleRace @0x59e530: the ships and boats) gets 13064 ("You can't consider that."); a vehicle's
corpse still sends `OP_ConsiderCorpse`. Both lines use the default chat colour (dsp_chat 0x111). The reply
handler (CEverQuest::Consider @0x5239a0) keeps its own lines: 12240 ("Consider who?") when the target is not in
the spawn list and 12241 for a corpse, which stock EQEmu never sends since corpses go through
`OP_ConsiderCorpse`.

#### Combat: observed sequences

EQEmu server, `playprobe --fight-nearest`:

- **Targeting:** C→Z `OP_TargetMouse` id. The server then sends `OP_MobHealth` for that target as its HP changes.
- **Consider:** C→Z `OP_Consider` {me, target}; Z→C `OP_Consider` with faction and colour. The client prints eqstr 12239 "%1 %2 -- %3", coloured by con level.
- **Melee:** C→Z `OP_AutoAttack` 1 + `OP_AutoAttack2` 1. The server swings on its own timer when the player is in range (`CombatRange`), has LOS and **faces** the target (`IsFacingMob`); out of range it sends "Your target is too far away, get closer!". Each swing: Z→C `OP_Damage` (to both sides), `OP_Animation`, `OP_MobHealth`; hits on the player also `OP_HPUpdate`. The client builds the damage lines.
- **NPC death:** Z→C `OP_Death` (the spawn keeps its id as the corpse), then `OP_ExpUpdate` and the experience message. The client prints the slain line ("You have slain a rodent!"), drops the target and stops attacking.
- **Player death** (Character:RespawnFromHover off): Z→C `OP_Death` with the player as victim, then `OP_ZonePlayerToBind` with zone 0 (bind in this zone). The client answers C→Z `OP_ZoneChange` {zone 0, instance, the given position, reason 0}; the server resolves zone 0 to the bind point (ZoneToBindPoint) and answers success 1, and the client re-enters through world.
- **Hover respawn on:** the server sends `OP_RespawnWindow` instead and waits for the chosen option (or picks option 0 itself when the timer runs out).

#### Combat: hover completion

From EQEmu's source; tests: `tests/hover_respawn_test.cpp`.

**Same zone and instance.** `OP_ZonePlayerToBind` arrives, then a living own `OP_ZoneEntry` with a new entity ID, authoritative buffs and HP; the previous ID remains the corpse. Movement stays blocked until that own spawn arrives (no other spawn can release it); then the client takes its position and heading once and resets movement without reloading the zone.

**Resources.** RoF2's bind packet carries zero HP/mana/endurance placeholders. During same-zone recovery the client keeps current mana/endurance (even zero), applies explicit nonzero values, and accepts the server's updates before or after the bind packet. Before the new spawn arrives, mana/endurance updates may use entity ID zero.

**Another zone or instance** uses the normal zone transfer. Countdown expiry is the server's decision. The selection is sent once, only for an available ID from the current window; a malformed window keeps the last valid one.

#### Combat: resurrection while hovering

A resurrection uses the ordinary `OP_RezzRequest` / `OP_RezzAnswer` confirmation, even when no refreshed respawn window arrives.

- **Yes, same zone.** While hovering, Yes to an offer whose zone dword (+4, zone and instance) is the current zone records consent locally and enables the reserved final resurrection option. It sends nothing and does not restart the countdown. Checked: ConfirmationDialog_HandleResponse @0x52d540 case 0x67.
- **Yes, another zone or instance:** echoed in `OP_RezzAnswer` at once.
- **Destination choice:** a later selection sends `OP_RespawnWindow {option_id}` for bind or resurrection (an `OP_RezzAnswer` acceptance at the prompt would commit at once on some servers).
- **No** echoes `OP_RezzAnswer` decline immediately and leaves bind usable.
- **Outside hover**, the ordinary resurrection answer still sends `OP_RezzAnswer` at once.

UI: both UI packs draw the respawn window alongside the question (ConfirmationDialogBox's `ResurrectionOffer` child marks it).

Consent rules:

- EQEmu reserves the final row for resurrection, numbering rows from zero; consent is tied only to that ordering, never to names or the availability byte.
- A changed reserved destination or newer offer invalidates consent.
- New death, completion, zoning and disconnect discard stale questions unanswered.

### Items, inventory and loot (`net/item_packets.h`, `GameConnection` in `net/game_connection_items.cpp`, rules in `game/inventory.{h,cpp}`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| CharInventory | Z→C | 0x5ca6 | u32 count, count serialized items (below); 4 zero bytes when empty. Sent during zone-in, before the player's spawn is complete |
| ItemPacket | Z→C | 0x368e | u32 ItemPacketType (0x64 merchant, 0x65 trade view, 0x66 loot, 0x67 trade/resync, 0x69 inventory, 0x6a limbo = summoned onto the cursor, 0x73 parcel), one item; a parcel adds u32 time, u32 n + name, u32 n + note |
| ItemLinkResponse | Z→C | 0x70c0 | same as ItemPacket with type 0 (`SendItemPacket(0, inst, ItemPacketViewLink)`) |
| MoveItem | both | 0x32ee | 28 B: InventorySlot from, InventorySlot to, u32 count. See note 1 |
| DeleteItem | both | 0x18ad | same 28 B; the server's resync sends {slot, invalid, 0xffffffff}; the client takes -count off the stack and removes an emptied one |
| DeleteCharge | Z→C | 0x01b8 | MoveItem layout. Spends one charge on an item that stays in inventory |
| MoneyUpdate | Z→C | 0x640c | 16 B: u32 platinum, gold, silver, copper carried |
| LootRequest | C→Z | 0x0adf | 4 B: u32 corpse spawn id. The server echoes it after the item packets |
| MoneyOnCorpse | Z→C | 0x5f44 | 20 B: u8 response (0 someone else, 1/3 ok, 2 not now, 4 hostiles, 5 too far, 6 all items sent), u8 0x42/0x5a, u8 0xef/0x40, u8, u32 pp, gp, sp, cp (the coin is already added to the player) |
| LootItem | both | 0x4dc9 | 20 B: u32 corpse, u32 looter, u16 corpse slot, u16, i32 auto loot, u32. See note 2 |
| EndLootRequest | C→Z | 0x30f7 | 4 B: u32 corpse id; answered with an empty OP_LootComplete (0x55c4) |
| ItemLinkClick | C→Z | 0x4cef | 52 B (ItemViewRequest_Struct): u32 item id, 6×u32 augments, u32 link hash, u32 flags (4 \| shift 2 \| ctrl 8 \| alt 0x10), u32 is evolving, u32 evolve group, u32 evolve level, u32 ornament icon |
| AugmentItem | C→Z | 0x661b | 40 B: u64 host serial, InventorySlot host, u32 socket 0..5, InventorySlot distiller, u32 action (0 insert, 1 remove, 2 swap, 3 destroy). See note 3 |
| AugmentInfo | both | 0x0afb | 76 B: u32 item id, u32 window, `char[64]` name, u32 (EQEmu unknown072). See note 4 |
| EvolveItem | C→Z | 0x7cfb | Final Result: 24 B, six u32 values. See note 5 |
| EvolveItem | Z→C | 0x7cfb | Progress: 24 B, u32 action **0**, u32 unused, u64 persistent instance id, u32 percentage, u32 activated. See note 6 |

1. MoveItem. InventorySlot = i16 type (0 possessions, 1 bank, 2 shared bank, 3 trade, 4 world, 5 limbo, 9 merchant, 11 corpse), i16, i16 slot, i16 bag sub-slot (-1), i16 augment (-1), i16.
   - Client: count 0 = move/swap a whole item; a count only for stack merges/splits. to = {-1, -1, -1, -1, -1} destroys (`DestroyHeldItemOrMoney`).
   - Server `OP_MoveItem`: to invalid removes the item, else a swap/merge.
   - Server `OP_DeleteCharge`: EQEmu sends `{from, invalid, 0xffffffff}` once per charge spent on a retained non-stackable item. When it removes an expended item, it sends `OP_MoveItem`.
2. LootItem: the corpse slot is 23..56, the header slot of the loot item packet (type 11). Auto loot 1 = into the inventory, 0 = cursor; the server refuses a cursor loot while the cursor is full. Echoed when looted; then the item arrives as an item packet.
3. AugmentItem: the host serial is instance id, parcel id, ignored by the server. The distiller is (-1, -1, 0xffff and 2 unwritten bytes) when none. The cursor augment is never named. No reply packet: the server answers with OP_ItemPacket 0x67 (the host rebuilt, same serial), OP_MoveItem / OP_DeleteItem, OP_ItemPacket 0x6a (an augment back on the cursor) or red chat.
4. AugmentInfo request: the augment's SolventItemID and the display's index, the rest 0. Reply: the solvent's name, shown as eqstr 5881 (13325 when the last field is 55, never from EQEmu).
5. EvolveItem Final Result values: action **4** (the item display's hyperlink action 1), source template id, final template id hint, current evolution level, unaugmented item-link hash, !Shift (1 normally, 0 with Shift). The server answers with an ordinary ItemLinkResponse.
6. EvolveItem Progress: the id is `instance_id \| (u64(parcel_item_id) << 32)` from item serialization; it updates only the matching evolving instance. Level and template change only through later item packets. Transfer actions 1–3: see "Evolving items" below.

#### Items: serialized item

`SerializeItem`, all `#pragma pack(1)`. `read_item` reads it field by field; `write_item` reproduces the server's bytes exactly (checked by the live fixture test).

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

#### Items: observed sequences

EQEmu server, `playprobe --move-test 1 --link-test 1 --loot 1 --destroy-test 1`:

- **Zone-in:** OP_CharInventory with the starting items; a summoned item then comes as an item packet onto the cursor.
- **Moves:** pick up (from → 33, count 0), put down (33 → slot, count 0), equip swaps and a swap with a full cursor. The server sends nothing back for a good move. Destroy (33 → invalid) is permanent.
- **Links:** a said link (`make_item_link`, 8-digit body) comes back in the echoed OP_ChannelMessage; clicking it (OP_ItemLinkClick, hash 0) answers OP_ItemLinkResponse.
- **Looting** an NPC corpse: OP_MoneyOnCorpse response 1 (no coin), the OP_LootRequest echo, OP_MoneyOnCorpse response 6, then OP_EndLootRequest → OP_LootComplete.

### Merchants, NPC dialogue and trade (`net/merchant_packets.h`, `net/trade_packets.h`, `GameConnection` in `net/game_connection_commerce.cpp`, prices in `game/merchant.{h,cpp}`)

Only OP_ShopPlayerBuy/Sell (field copies) and OP_CancelTrade have a RoF2 translator; the rest use rof2_structs.h's layouts.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| ShopRequest | C→Z | 0x4fed | 24 B MerchantClick_Struct: u32 merchant, u32 me, then 16 bytes the RoF2 client leaves unset (we send command 1, rate 0, tabs 0, -1). Sent on a right click on a class-41 NPC within 20 units, with no merchant open |
| ShopRequest | Z→C | 0x4fed | same: u32 merchant, u32 0, u32 command (1 open, 0 refused), f32 rate = 1 / (BuyCostMod × CalcPriceMod), i32 tabs (1 buy/sell, 2 recover, 4 parcels), i32. Then the stock, see note 1 |
| ShopPlayerBuy | C→Z | 0x0ddd | 32 B Merchant_Sell_Struct: u32 merchant, u32 me, u32 item slot (the item's instance id) + u32 high dword (0), u32 quantity, 12 bytes unset (0). Refused locally with an item on the cursor (8548) or too little money (12060/12061) |
| ShopPlayerBuy | Z→C | 0x0ddd | same, quantity bought, u32 total price at +24. See note 2 |
| ShopPlayerSell | C→Z | 0x791b | 20 B Merchant_Purchase_Struct: u32 merchant, TypelessInventorySlot {i16 slot, i16 bag sub-slot, i16 augment, i16}, u32 quantity, u32 price (unset by the client; 0). Refused locally: not droppable 12062, a non-empty bag 12064, augmented 5360, a sale pending 12063 |
| ShopPlayerSell | Z→C | 0x791b | same with the copper paid; slot -1 = refused (1364, colour 0x113). The sold count leaves the stack (no item packet); the copper is credited, wave 0x8a (the cash register) plays and 1361 "You receive%1 from %2 for the %3(s)." prints in the Merchants colour 0x114 (FinishSellingItem @0x6f4200), then OP_MoneyUpdate with the new coin |
| ShopEnd | C→Z | 0x30a8 | 8 B: u32 merchant, u32 me; answered by an empty OP_ShopEndConfirm (0x3196) |
| ShopDelItem | Z→C | 0x724f | 12 B: u32 merchant, u32 player, u32 merchant slot (sold out) |
| TradeRequest | both | 0x77b5 | 8 B: u32 the other side, u32 the sender. See note 3 |
| TradeRequestAck | both | 0x14bf | 8 B: {the other side, the sender}. See note 4 |
| TradeAcceptClick | both | 0x69e2 | 8 B: u32 partner, u8 ready (1), 3 unset bytes. The server forwards it to a player partner (byte +4 = their ready flag); for an NPC it finishes the trade |
| TradeCoins | Z→C | 0x4206 | 12 B: u32 the receiver's own id, u8 coin type (0 cp .. 3 pp), 3 B, u32 amount added (a delta; Hailstone holds the total at 0xffffffff rather than wrapping) |
| MoveCoin | C→Z | 0x0bcf | 20 B: i32 from, i32 to (0 cursor, 1 inventory, 2 bank, 3 trade, 4 shared bank), i32 type from, type to, i32 amount. Coin goes into a trade in two moves, inventory → cursor and cursor → trade |
| CancelTrade | both | 0x354c | 8 B: C→Z {partner, me}; Z→C {the receiver's own id (player partner) or the NPC id, action}. See note 5 |
| FinishTrade | Z→C | 0x3993 | empty: the trade window closes; received items follow as type 0x67 (or 0x6a onto the cursor) |
| TradeBusy | both | 0x5505 | 12 B: {requester, me, u8 reason ('b' 1444, 'c' 1445, else 1446), 3 B} |
| ItemLinkClick (say link) | C→Z | 0x4cef | the ordinary OP_ItemLinkClick; the server says the phrase. See note 6 |

1. ShopRequest stock: item packets type 0x64 (header slot type 9, slot = instance id = merchant slot, price = per-unit copper, merchant_slot = stock, -1 unlimited).
2. ShopPlayerBuy reply: the item arrives first as an item packet type 0x67. There is **no** OP_MoneyUpdate: the client takes the price itself (smallest coins first; the split may differ from the server's, the total agrees) and prints 1437 "You give%1 to %2." in the Merchants colour 0x114. Wave 0x8a (the cash register) plays only when the purse could pay; otherwise the coin is zeroed silently (FinishBuyingItem @0x6ef220, also a trader purchase, TraderShop 10 method 0).
3. TradeRequest C→Z is sent on a left click on a spawn while holding an item or coin (within AvatarHeight + 20, 12294). Banker-type classes 40/63/64/66/74 refuse (8505). Z→C (a player asks): the client opens its trade window and replies with the Ack if it can trade.
4. TradeRequestAck: for an NPC the server answers itself ({me, NPC}). After the Ack for a pending request, the client opens the window and moves the held item (OP_MoveItem cursor → {3, slot}, count 0; a stack merges first) or coin (OP_MoveCoin cursor → trade) into the offer. NPCs get four slots and players get eight.
5. CancelTrade: the client prints 1448/1450 and sends its own cancel back while a partner is set. Refunds come as item packets and OP_MoneyUpdate, then OP_FinishWindow/2 (empty).
6. Say links: EQEmu say links are item links to item 0xF423F with the phrase id in augment 1 (augment 2 for silent links).

#### Merchants: hail, prices and the cursor queue

**Hail** (key H) says eqstr 13085 "Hail, %1" with the target's display name, or 13084 "Hail". NPC SayString replies arrive as OP_FormattedMessage 554 "%1 says '%T2'", where %T2 is an eqstr id expanded with the same arguments (`game/eq_strings.cpp`).

**Sell price** shown is (int)(value / rate + 0.5) × count, rounded to whole gold from 10,000,000 copper (`game::sell_value`). It is 0 for Temporary, No Trade, attuned, ornamented or real-estate items and items holding such contents. Buy prices are the item header's price field, unchanged.

**Cursor queue.** The server re-sends the head of its cursor queue as a type 0x6a item whenever the cursor empties; the client places a 0x6a item only into an empty slot, so rewards appear one at a time.

#### Merchants: observed sequences

EQEmu server, `playprobe --shop/--hail/--handin/--trade-with/--trade-accept`, South Qeynos:

- **Merchant:** OP_ShopRequest → Z→C OP_ShopRequest {command 1, rate 1.0, tabs 1}, a FormattedMessage 554 greeting, then the stock.
  - Sell: OP_ShopPlayerSell with the price paid, then OP_MoneyUpdate; the client's shown price matched.
  - Buy: item packet 0x67, then OP_ShopPlayerBuy with the total price.
  - A sold item shows up in the merchant's list as a limited item.
  - OP_ShopEnd → OP_ShopEndConfirm.
- **Hail:** an NPC's bracketed reply text is a say link (item 999999 with a phrase id); clicking it makes the server say the phrase.
- **Hand-in:** OP_TradeRequest → OP_TradeRequestAck {me, NPC}; OP_MoveItem cursor → trade 0; OP_TradeAcceptClick → OP_FinishTrade, reward item packet 0x6a onto the cursor, speech and faction messages.
- **Player trade:** the partner gets OP_TradeRequest, acknowledges, receives the offer as item packet 0x65 and coin as OP_TradeCoins. Both OP_TradeAcceptClick, then OP_MoneyUpdate, the item as 0x67, OP_FinishTrade. A client that disconnects with an open give window gets its item back.
- **Camp:** on every camp the server sends OP_CancelTrade {own id, action 7}; with no trade open it is ignored.

### Evolving items (`net/evolving_packets.h`, `net/game_connection_evolving.cpp`)

`OP_EvolveItem` is `0x7cfb`. Actions 0 (progress) and 4 (Final Result) are in the item table above. Checked: EQEmu's RoF2 serializer; native handlers `0x6af8e0` (details), `0x6b0230` (selection/confirmation), `0x6b00b0` (confirmed transfer).

| Action | Direction | Payload |
|---|---|---|
| 1 open | Z→C | 24 B, action 0's envelope; opens the transfer window and clears its selection; no client open request |
| 2 details request | C→Z | 24 B: `u32 action, u32 zero, u64 source ID, u64 destination ID` |
| 2 details reply | Z→C | 30 B header: `u32 action, u64 source ID, u64 destination ID, u32 compatibility percent, u32 max level, u8 source present, u8 destination present`, followed by each present serialized item |
| 3 transfer | C→Z | 24 B, same layout as the details request, sent only after explicit confirmation |

An evolving item's ID combines the serialized header's `parcel_item_id` high word and `instance_id` low word. The serial text is a different identity.

The details reply's header carries the original IDs; its serialized items are the projected results. Hailstone matches the selected pair and rejects bad presence flags, truncated items and trailing data; confirmation rechecks the live originals.

There is no request nonce, so two replies for the same pair cannot be told apart (see `docs/unknowns.md`). The server rechecks on action 3 and reports through ordinary inventory updates and messages; nothing is applied to inventory in advance.

### Bank, coin moves and stack splits (`GameConnection` in `net/game_connection_bank.cpp` and `net/game_connection_items.cpp`, rules in `game/bank.{h,cpp}`)

The bank window has no packets of its own.

- A right click on a banker (class 40) within 20 units opens the bank window locally (1465 "%1 tells you, 'Welcome to my bank!'"); it closes with "Come back soon!" (12143) when a 500 ms check finds the banker gone or beyond 20 units.
- Bank items arrive with the inventory (InventorySlot type 1, slots 0..23; shared bank type 2, slots 0..1).
- Bank coin comes in the player profile (`platinum_bank`.., `platinum_shared`) and is then tracked by the client; OP_MoneyUpdate carries only the coin on the person.
- The server checks for a banker within 200 units (`GetClosestBanker`, squared 40000) on every bank item and coin move.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| MoveCoin (bank) | C→Z | 0x0bcf | as above; bank coin moves through the cursor. See notes 1–2. The server answers OP_MoneyUpdate |
| MoveItem (split) | C→Z | 0x32ee | {slot, cursor 33, quantity}: the quantity dialog's answer for less than the whole stack; the client clones the item with that count onto the cursor first. Taking the whole stack is an ordinary move (count 0) |
| MoveItem (merge) | C→Z | 0x32ee | {cursor, slot, count moved}: as much as fits into a matching stack (`ItemContainer_MergeStacks`); a full stack of the same item does not swap |

1. Bank coin moves. Deposit = {1 → 0} (inventory money button and quantity) then {0 → 2 or 4, held type, button type, all held}. Withdrawal = {2 or 4 → 0} then {0 → 1}. The client's internal coin indices run platinum 0 .. copper 3 and are reversed when the packet is built (wire 0 copper .. 3 platinum).
2. Moving to a different coin type converts. Going up by d sizes takes amount − amount % 10^d and adds amount / 10^d (the rest stays on the cursor). Going down multiplies by 10^d. A move that would take the destination past 10^9 is refused.

**Quantity dialog.** A click on a stack (not in a world container or on a corpse) opens it with max = min(stack count, max stack). Shift answers max at once, Ctrl answers 1, and a stack of one is taken at once; otherwise the slider starts at max.

#### Bank: observed sequences

EQEmu server, `playprobe --bank any [--bank-keep 1]`, South Qeynos. 24 bank slots (expansion bit 10).

- A coin deposit or withdrawal is answered with OP_MoneyUpdate (sent twice for the deposit).
- Item moves between general, bank and shared bank, splits and merges: the server sends nothing back; a relog shows coin and items where the client put them.
- Walking 40 units away closes the window with "Come back soon!".

### Spells (`net/spell_packets.h`, `GameConnection` in `net/game_connection_spells.cpp`, rules and spell data in `game/spells.{h,cpp}`)

Spell data: the client reads `spells_us.txt` (237 '^'-separated columns, `game::SpellTable`). The server's table decides; the client uses its copy only to gate casts and to label (names and messages).

The online client never finishes a cast itself: it gates the cast, starts its cast bar, locks the gem and the global recovery for 10 s, sends `OP_CastSpell` and waits. The server decides everything else and ends **every** cast with `OP_ManaChange`.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| CastSpell | C→Z | 0x1287 | 44 B: u32 gem (0..11, 12 item, 13 discipline, 255 AA), u32 spell, InventorySlot (12 B; a gem cast sends all −1 with the last word 0), u32 target spawn, u32 the client's CRC of its spell record, u8 free-target flag + 3 B, f32 y, x, z (target ring; 0) |
| BeginCast | Z→C | 0x318f | 10 B: u32 spell, u16 caster, u32 cast time ms (the server's, after its modifiers). Sent to everyone near, the caster included |
| ManaChange | Z→C | 0x5467 | 20 B (above). keep casting 0 = the spell bar enable that ends the cast (slot = the gem, `FindMemmedSpellBySpellID`); keep casting 1 with slot −1 = the periodic mana/endurance update (`CheckManaEndUpdate`, twice per 6 s tick) |
| ManaChange | C→Z | 0x5467 | **empty**: interrupt my cast (`/stopcast`, StopSpellCast reason 0) |
| MemorizeSpell | both | 0x217c | 16 B: u32 slot (book slot for scribing, else gem), u32 spell, u32 scribing (0 scribe, 1 memorize, 2 forget, 3 spell bar refresh, server only), u32 reduction (ms off the recast). C→Z 0/1/2 after the client's own countdown; Z→C echoes 0/1/2 and sends 3 after each successful cast |
| InterruptCast | Z→C | 0x048c | u32 spawn, u32 eqstr id, then NUL-terminated arguments. To the caster: 8 B, e.g. 439 "Your spell is interrupted."; to others: the caster's name as %1 |
| Action | Z→C | 0x744c | 56 B (ActionAlt_Struct): u16 target, u16 source, u16 level, u32 0, f32 instrument mod, f32 force, f32 hit heading, f32 hit pitch, u8 type (231 spell), u32 damage (0), u16, u32 spell, u8 spell level, u8 effect flag (4 = landed), u8 gem, InventorySlot, u32 item cast type. Once when the spell goes off and again with flag 4 when it lands |
| Damage (spell) | Z→C | 0x6f15 | as above with type 231: the client prints the landing text from it (`ReportSuccessfulHit`). See note 1 |
| Buff | both | 0x659c | 100 B: u32 entity, SpellBuff (88 B), u32 slot, u32 fade. See note 2 |
| BuffCreate | Z→C | 0x3377 | u32 entity, u32 tic timer, u8 all buffs (1 = the whole list), u16 count, count × {u32 slot, u32 spell (0xffffffff = removed), u32 tics, u32 num hits, cstr caster}, u8 type (0 own window). See note 3 |
| TargetBuffs | Z→C | 0x4f4b | as BuffCreate, type 5 (PC) or 7 (NPC): the target window's buffs; an entry for slot 97 or past it (an NPC's most in RoF2, EQEmu's `NPC_BUFFS`) is ignored |
| BuffRemoveRequest | C→Z | 0x64f2 | 8 B: u32 slot, u32 entity (own id or the pet's): click a buff off. EQEmu fades it if it is not detrimental |
| DeleteSpell | both | 0x3358 | 8 B: i16 book slot, 2 B, u8 success (the echo: 1 = deleted), 3 B |
| LoadSpellSet | C→Z | 0x261d | 16 B: u8 forget[12] (non-zero = forget that gem), u32 count (12). See note 4 |
| ColoredText | Z→C | 0x43af | u32 chat colour, text (EQEmu copies strlen bytes without a terminator; the struct padding usually supplies one). Buff fade texts |
| LinkedReuse | Z→C | 0x1619 | 12 B: u32 timer 1..19, u32 end, u32 start (seconds): a shared spell timer; tints the gems only |
| Stamina | Z→C | 0x2a79 | 8 B: u32 food, u32 water |
| SpellEffect | Z→C | 0x5936 | 28 B: u32 effect (a spellsnew.eff index, not a spell), u32 caster, u32 target, u32 stage 1 start ms, u32 stage 2 after it ms, u32 stage 2 length ms, u8 full (emitters at density and opacity 1), u8 add (0 removes), u16 unread. See note 5 |
| AddNimbusEffect | Z→C | 0xc693 | 8 B: u32 spawn, i32 effect: the effect's impact stage on the spawn (`Mob::AddNimbusEffect`, quests only) |
| RemoveNimbusEffect | Z→C | 0x7b1e | 8 B: u32 spawn, i32 effect: stops the effect's emitters on the spawn. EQEmu sends it when a buff with a nimbus fades |

1. Spell landing text. On itself: CastOnYou; with damage, "%1 You have been healed for %2 points." (4600/4601, negative damage) or "%1  You have taken %2 points of damage." (12203/12204). On others: "<name><CastOnAnother>".
2. Buff. SpellBuff (88 B): u8 effect type, u8 level, u8, u8, f32 bard modifier, u32 spell, u32 duration tics, u32 caster, u32 num hits, f32 y, x, z, u32, i32 slot data[12]. Z→C fade 1 removes the slot (sent twice, the second with spell 0xffffffff); 2 refreshes it. RoF2 copies the whole `slot_data`; Hailstone keeps its local counters only on fade 2 + same spell + all `slot_data` zero (EQEmu always sends zeros), else overwrites.
3. BuffCreate is the own buff window: slots 0..41 long, 42..61 short (song window), 62 discipline. EQEmu sends full lists at zone-in and on buff changes. The list has no `slot_data`, so local counters are kept for spells still in the same slots; omitted slots are cleared.
4. LoadSpellSet is sent when a spell set is memorized (the spell book's sets, the gem menu, `/memspellset`; `net/game_connection_spell_sets.cpp`), forgetting the gems the set replaces.
5. SpellEffect. Add: the effect's cast stage at once, stage 1 and stage 2 as their starts pass (`SpellEffectTimers`). Remove: the timers dropped and the emitters stopped. EQEmu sends a spell's nimbus (spells_new column 193) as {500, 0, 3000, 1, 1} when it is cast.

#### Spells: observed sequences

EQEmu server, `playprobe --spells 1` with a level 1 human cleric:

- **Scribing:** the scroll goes onto the cursor (C→Z `OP_MoveItem`), "Beginning to scribe %1..." (12049). After the client's countdown (92 ticks, 3 per frame at most every 2 ms): C→Z `OP_MemorizeSpell` {book slot, spell, 0}. The server deletes the scroll and echoes it; the client writes the book and prints 12006. The book only works sitting (StandState 110) or mounted.
- **Memorizing:** "Beginning to memorize %1..." (12053). The client counts 92 ticks down, one step every max(100, (level needed − level + 5) × 100) ms, 1 + max(Meditate, 10) / 10 + 2 ticks per step (a profile bard: (DEX + INT) / 30 + 1 instead of the 1). Then C→Z `OP_MemorizeSpell` {gem, spell, 1}; the echo fills the gem (12007) and starts its refresh at RecastTime + 1000 ms.
- **Casting:** C→Z `OP_CastSpell`.
  - A fizzle: Z→C `OP_ManaChange` {keep 0, slot = gem} twice and "Your spell fizzles!" (`OP_SimpleMessage`), no `OP_BeginCast`.
  - A cast: Z→C `OP_BeginCast`, then at the end `OP_Action` (flag 0), `OP_Action` (flag 4), `OP_Damage` {type 231, damage 0}, `OP_ManaChange` {keep 0, slot = gem} and `OP_MemorizeSpell` {gem, spell, 3, reduction 0}.
- **`/stopcast`:** C→Z empty `OP_ManaChange`; Z→C `OP_InterruptCast` {me, 439} and `OP_ManaChange` {keep 0, slot = gem}.
- **Moving** during the cast: the server lets it run and refuses it at the end (`OP_InterruptCast` {me, 439}, `OP_ManaChange` {keep 0}). The client never interrupts itself online.
- **Forgetting** (right-click a gem): C→Z `OP_MemorizeSpell` {gem, spell, 2}; the echo empties the gem ("You forget %1.", 12496).
- **Zone-in:** `OP_BuffCreate` {all 1}, then each buff that lands again as `OP_Action` ×2 + `OP_BuffCreate` + `OP_Damage`. Every 6 s, `OP_Buff` (fade 2) + `OP_BuffCreate` (all 0) refresh tics; EQEmu's `OP_Buff` carries all `slot_data` as zero (`rof2.cpp` `ENCODE(OP_Buff)` TODO; see note 2).

### Groups (`net/group_packets.h`, `GameConnection` in `net/game_connection_group.cpp`, state in `game/group.{h,cpp}`)

Server side: zone `Handle_OP_Group*` (client_packet.cpp), `Group::*` (groups.cpp), `Client::GroupFollow`, `SendGroupCreatePacket`, `SendGroupLeaderChangePacket`, `SendGroupJoinAcknowledge` (client.cpp), `ZoneDatabase::RefreshGroupFromDB`.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| GroupInvite | both | 0x6110 | 148 B: `char[64]` invitee, `char[64]` inviter, 5×u32 0. Sent by `/invite` when the player has no group; EQEmu forwards it to the invitee (also for GroupInvite2) |
| GroupInvite2 | C→Z | 0x32c2 | same 148 B; sent when inviting into an existing group |
| GroupFollow / GroupFollow2 | both | 0x1649 / 0x2060 | 152 B: `char[64]` inviter, `char[64]` invitee (the accepting player), 5×u32, u32 at +148 (note 1) |
| CancelInvite | C→Z | 0x2a50 | 152 B: `char[64]` inviter, `char[64]` the declining player, 20 B, u32 reason at +148 (3 declined with `/disband`, 1 already invited, 4/5 raid states). **EQEmu has no handler** (note 2) |
| CancelInvite | Z→C | 0x2a50 | the same 152 B (from the server script); the line for the reason (note 3) |
| GroupDisband | C→Z | 0x4c10 | 148 B GroupGeneric: `char[64]` name1, `char[64]` name2, 20 B. A member leaving: {me, me}; a leader removing a member: {member, leader}. EQEmu decides from the sender's target: a leader with no target disbands the group, with a member targeted removes it |
| GroupDisbandYou | Z→C | 0x1ae5 | 148 B GroupGeneric {you, member}: you left or the group was disbanded. "You have been removed from the group." (12001); the group is deleted |
| GroupDisbandOther | Z→C | 0x74da | 148 B GroupGeneric: name2 left |
| GroupUpdate | Z→C | 0x3abb | 148 B (RoF2 GroupJoin_Struct): `char[64]` owner (mercenaries), `char[64]` member, u8 merc, 3 B, u32 level, 12 B. EQEmu fills only the member name (note 4) |
| GroupUpdateB | Z→C | 0x6194 | u32 group id (0), u32 count, cstr leader; count × {u32 slot, cstr name, u16 merc, cstr owner, u32 level, u8 tank, u8 assist, u8 puller, u32 offline, u32 timestamp}. The client rebuilds every slot (note 5) |
| GroupLeaderChange | Z→C | 0x21b4 | 148 B: 64 B, `char[64]` leader, 20 B |
| GroupAcknowledge | Z→C | 0x7323 | 4 B: "You have joined the group." (12004). Sent to both sides when a group forms |
| GroupLeadershipAAUpdate | Z→C | 0x02cf | leadership AAs; sent after every group update; the group leader's ability ranks and NPC marker are kept (`GameConnection::handle_leadership_ability`) |
| GroupMakeLeader | C→Z | 0x4229 | 456 B: u32 8, `char[64]` current leader (me), `char[64]` new leader, 324 B 0. The client then sends GroupMentor {0, ""} |
| GroupMentor | C→Z | 0x3342 | 68 B: i32 percent, `char[64]` name ("" clears) |
| GroupRoles | both | 0x70e2 | 148 B: `char[64]` member, `char[64]` leader, 3 × u32, i32 role at +0x8c (1 Main Tank, 2 Main Assist, 3 Puller), u8 on at +0x90 (0 clear, 1 assign), 3 B padding (note 6) |
| MobManaUpdate / MobEnduranceUpdate | Z→C | 0x2404 / 0x1c81 | 3 B: u16 spawn, u8 percent; group members' mana and endurance |
| MobHealth | Z→C | 0x37b1 | 3 B (above); group members' HP percentages come this way too |

1. GroupFollow: the client copies the u32 at +148 of the invitation (past EQEmu's 148 bytes; sent as 0). Accept with GroupFollow when the invitation came as 0x6110, else GroupFollow2. EQEmu echoes it to the inviter.
2. CancelInvite C→Z: EQEmu's OP_GroupCancelInvite is 0x0000 for RoF2 and OP_CancelInvite is not one of its opcodes, so on stock EQEmu the inviter is never told. The Hailstone server script (`server/`, setting `group_declines`) passes it on: the packet to an inviter in the same zone, else the line as text.
3. CancelInvite Z→C: the line for the reason byte at +148, name at +64, colour 0x111: 1 12264, 2 12265, 3 12266 "%1 rejects your offer to join the group.", 4 5086, 5 5088; other reasons nothing. Checked: Handle_OP_CancelInvite @0x524d90.
4. GroupUpdate: a member joined. EQEmu also sends it for leader changes and leadership-AA updates; the client adds only unknown names.
5. GroupUpdateB: EQEmu puts the receiver in slot 0, with fake levels 0x46/0x41. "Group created" (`SendGroupCreatePacket`, the inviter only) has an empty leader and one member with the real level.
6. GroupRoles: the leader's menu and `/grouproles` send requests; local flags change only on the server's update, which also clears the role's previous holder.

#### Groups: observed sequences

EQEmu server; the GUI client and `playprobe` as the second player.

- **Invite into no group:** C→Z `OP_GroupInvite` (148 B). The invitee gets `OP_GroupInvite` (148 B, no +148 word).
- **Accepting** (`OP_GroupFollow`, 152 B). The inviter gets, in order:
  `OP_GroupUpdateB` {leader "", 1 member, real level}, `OP_GroupLeaderChange`, `OP_GroupAcknowledge`, `OP_GroupUpdate` {joiner, level 0}, `OP_GroupLeadershipAAUpdate` (320 B), `OP_MobHealth`/`OP_MobManaUpdate`/`OP_MobEnduranceUpdate` for the joiner, `OP_GroupUpdateB` {leader, 2 members, levels 70/65}, and the `OP_GroupFollow` echo.
  The invitee gets the three Mob* updates, `OP_GroupAcknowledge`, `OP_GroupUpdateB` {leader, itself in slot 0, levels 70/65}, `OP_GroupLeadershipAAUpdate`.
- A kill by either gives "You gain party experience!!" to both.
- `/makeleader`: everyone gets `OP_GroupLeaderChange` {new leader}, `OP_GroupUpdate` {new leader} and `OP_GroupLeadershipAAUpdate`.
- Camping or dropping the link (two-member group): the other member gets `OP_GroupDisbandYou` {self, ""} and `OP_GroupLeadershipAAUpdate`.

### Pets (`net/pet_packets.h`, same files)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| PetCommands | C→Z | 0x0159 | 12 B: u32 command, u32 target, u8 flag 0, 3 B (note 1) |
| PetCommandState | Z→C | 0x1dc8 | 8 B: u32 button (EQEmu PET_BUTTON_*: 0 sit, 1 stop, 2 regroup, 3 follow, 4 guard, 5 taunt, 6 hold, 7 ghold, 8 focus, 9 spell hold), u32 state |
| PetHoTT | Z→C | 0x794a | 4 B: the focused pet's target |
| PetBuffWindow | Z→C | 0x5882 | the OP_BuffCreate layout (u32 pet, u32 0, u8 1, u16 count, entries, u8 0); slots from 97 on are ignored, as for TargetBuffs |
| SpawnAppearance type 25 | Z→C | 0x0971 | the spawn's owner (MasterID); 0 when a charm breaks |

1. PetCommands:
   - Command (EQEmu PET_*): 0 health, 1 leader, 2 attack, 3 qattack, 4 follow, 5 guard, 6 sit (7 on, 8 off), 9 stop, 12 taunt (13/14), 15 hold (16/17), 18 ghold, 21 spell hold, 24 focus, 27 feign, 28 back off, 29 get lost, 31 regroup.
   - Target: attack / qattack, the current target if not yourself; leader, the target if it is an NPC; else 0.
   - `/pet target` (30) targets the pet locally and sends nothing. An attack without a target sends nothing. EQEmu ignores a command whose target is the pet itself.

### Skills and abilities (`net/ability_packets.h`, rules in `game/mechanics/abilities.{h,cpp}`, glue in `app/actions_ui.cpp`)

The client decides locally whether a skill may be used and sends one request; the server rolls the outcome and answers with chat, OP_Damage, OP_SpawnAppearance and so on.

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
| Hide (cancel) | C→Z | 0x67fe | 4 zero bytes: acknowledgement for the server's OP_CancelSneakHide |
| CancelSneakHide | Z→C | 0x0927 | empty: clear Hide and Sneak, then send Hide (cancel), own SpawnAppearance type 3 with parameter 0 unless regular invisibility remains (a known spell, or a level the server set for the player), and type 15 with parameter 0 (note 2) |
| SpawnAppearance (sneak / hide) | both | 0x0971 | type 15 (sneak) parameter 0/1 from the server sets the player's sneak flag; the client sends {me, 15, 0} to stop sneaking and {me, 3, 0} to unhide before a disarm |
| SkillUpdate | Z→C | 0x004c | 12 B: u32 skill, u32 value, 4 unknown bytes; the value goes into the profile's skills |
| Track | both | 0x17e5 | C→Z empty (the tracking window asks for the list); Z→C the list (note 1) |

1. Track Z→C: the list is decoded, sorted and filtered for the tracking window (`net/game_connection_tracking.cpp`). A first u32 of 0xff8b344f (`decode_track_reply_header`) is the tracking timer, not a list.
2. CancelSneakHide: the original (HandleWorldMessage @0x4c7623, @0x5319e0) answers only for the flags it holds:
   Send_OP_Hide @0x52b9e0 while hidden (OP_Hide {0}, type 3 when the visibility level changed, eqstr 12337), then
   @0x52bae0 while sneaking (eqstr 12338, type 15). Hailstone sends all three whatever its flags (a fixed RoF2
   gap): EQEmu's `Client::CancelSneakHide` sends the packet only while it holds the player hidden, its Hide roll
   is independent of the client's, and it clears its hidden flag and relays the reveal to nearby clients only on
   the client's type 3 with 0. The lines are printed only for a flag that was set.

### Doors and ground objects (`net/door_packets.h`, `GameConnection` in `net/game_connection_doors.cpp`, rules in `game/doors.{h,cpp}`, drawing and collision in `app/world_objects.{h,cpp}`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| SpawnDoor | Z→C | 0x7291 | size / 100 records of 100 B: `char[32]` name (the actor is `<name>_ACTORDEF`), f32 y +32, x +36, z +40, heading +44, i32 incline +48 (DefaultDoorAngle, made a float), u16 size +52 (percent), 6 B, u8 id +60, u8 open type +61, u8 state +62, u8 invert +63 (DefaultState), i32 param +64 (lift travel, slide distance: "SpellID"), i32 +68 / +72 / +76 (adventure, dynamic-zone, real-estate door ids), u8 +80 has script, +81 usable, +82 remain open, +83 visible, +84 heading changed, 3 B, f32 +88 / +92 TopSpeed1/2, u8 +96 stamp the time, 3 B. EQEmu sets +81 and +83 to 1 ("both must be 1 to allow clicking doors") |
| ClickDoor | C→Z | 0x3a8f | 16 B: u8 door id, 3 B (unset on the wire; 0 here), i32 lock-pick skill (≤ 254), u32 key item id (the cursor item; -1 without one), u32 player spawn id |
| MoveDoor | Z→C | 0x08e8 | 2 B: u8 door id, u8 action = the new door state (2 opening, 3 closing; EQEmu sends 3/2 for inverted doors) |
| RemoveAllDoors | Z→C | 0x700c | empty |
| GroundSpawn | Z→C | 0x6fca | u32 drop id, cstr actor name (≤ 63 chars; note 1), u16 zone, u16 instance, u32 drop id again, u32 0, f32 heading, f32 tilt x, f32 tilt y, f32 size (scale), f32 y, x, z, i32 object type (1 = a temporary dropped item; tradeskill containers have their type) |
| GroundSpawn | C→Z | 0x6fca | empty: drop the cursor item on the ground |
| ClickObject | both | 0x4aa1 | 8 B: u32 drop id, u32 player spawn id. C→Z: pick up / open (LMouseUp, under 20 units). Z→C: the object is gone (picked up by someone, decayed); the item itself comes as an item packet of type 0x67 to the cursor |
| ClickObjectAction | Z→C | 0x0c1e | 92 B: u32 drop id (0xffffffff: someone else is using it), i32 -1, i32 -1, u32 container type, u32 0x0a, u32 icon, u32, `char[64]` name: a tradeskill container opened; its contents follow as item packets of type 0x6b (slot type 4, World) |
| ClickObjectAction | C→Z | 0x0c1e | empty: close the container (EQEmu `Object::Close`, which answers OP_ClearObject) |
| ClearObject | Z→C | 0x7a11 | 8 B (`decode_clear_object`): u8 clear, 3 B, u32 slot (note 2) |
| TradeSkillCombine | C→Z | 0x579a | 24 B: container slot (12 B InventorySlot; a world container is type 4, slot -1 = SLOT_TRADESKILL_EXPERIMENT_COMBINE), guild-tribute slot (all -1) |
| RecipesFavorite / RecipesSearch | C→Z | 0x71b1 / 0x6290 | 2008 B {u32 container type, u32 bag item id (0 world), 500 × u32 recipe ids} / 80 B {type, id, u32 min trivial, u32 max trivial, `char[56]` partial name, u32 0x300, u32}; one RecipeReply each |
| RecipeReply | Z→C | 0x1db6 | 84 B: type, id, u32 component count, u32 recipe id, u32 trivial, `char[64]` name |
| RecipeDetails | both | 0x6e02 | C→Z {u32 recipe id}; Z→C big-endian recipe id, then ten entries, each a BE 0xffffffff (unused) or {BE item id, BE icon, name} (note 3) |
| RecipeAutoCombine | both | 0x40d7 | 40 B: type, id, container slot (12 B, echoed), 12 B slot, u32 recipe id, u32 reply (0 made, 0xfffffff5 failed; the server says why) |

1. GroundSpawn actor name: e.g. `IT63_ACTORDEF`. EQG models go through Resources/OnDemandResources.txt, e.g. `IT10804` in tradeskill_objects.eqg.
2. ClearObject: clear 0 removes the world container's item in that slot. Anything else empties every slot of the world container; EQEmu sends that when it closes a container and after a combine emptied it. The window stays open.
3. RecipeDetails: EQEmu puts the unused entries first. The client reads nothing after the ten; newer EQEmu (#5100) appends the trivial as a BE u32, which is skipped. Checked: CTradeSkillWnd_HandleRecipeDetails 0x77dfd0.

#### Door behaviour

All in `game/doors.cpp`. The state machine is 0 closed, 1 open, 2 opening, 3 closing, 4 a sprung trap. A spawned door starts in its default state (an "open" spawn state animates open).

Every door moves each frame by elapsed ms × 0.02 ticks (nothing moves on a frame later than 15 ticks):

| Door types | Motion |
|---|---|
| Hinged, 0–8 | turn their heading 8 per tick through a quarter turn (types 5–7 the other way) |
| Trap doors and drawbridges (0x1e, 0x23, 0x24, 0x28, 0x82) | turn the door angle |
| 10–0x1b | slide sideways 8/10/14/22 units at 0.35 |
| Lifts and portcullises (0x3b–0x54, 0x78, 0x8c–0x92) | rise by their distance (the door param for 0x3b, 0x4b–0x54) at 0.45 per tick, come down at 0.25 or 0.45 |
| Spinners (100–102 → 0x9c, 105–107 → 0x9d, 0x73/0x74) | accelerate to `TopSpeed` |

A door not in its default state returns by itself after a repop time (10 s for types 0 and 5, 20 s for most). EQEmu's close timer resets only the server's state silently, so the client's return is what closes ordinary doors.

For a mouse pick, the 3D distance from the player to the hit point must be within the player's height + 20.
Activations without a hit point measure to the door origin and add the scaled actor radius to the reach.
An out-of-range activation shows eqstr 1462 "You can't reach that, get closer."
EQEmu separately checks `Range:MaxDistanceToClickDoors` (100 units by default).

Teleport doors (types 57/58 with a destination) are moved by the server with OP_RequestClientZoneChange.

The actor is placed at (Y, X, Z), turned by Heading (counterclockwise like spawns), tilted by DoorAngle about its Y axis, and scaled by size %.
For classic door actors whose visible triangles are passable, an authored WLD mesh polyhedron provides collision
in the same moving transform. Mouse clicks still hit the displayed mesh. Door-type movement filters still apply.
A replacement spawn record refreshes the scene actor for that door ID, including its model and movement filter.

### Alternate advancement, training, tradeskills and tasks (`net/aa_packets.h`, `net/training_packets.h`, `net/task_packets.h`, `GameConnection` in `net/game_connection_progress.cpp`, rules in `game/alt_adv.h`, `game/training.h`, `game/tasks.h`)

All tried live against an EQEmu server (playprobe `--aa`, `--aa-use`, `--train`, `--task`, `--task-cancel`, `--object`), except a successful combine and the bag (inventory container) path, which have unit tests only.

#### Alternate advancement

After its own spawn arrives, the client asks for the table (empty OP_SendAATable, only while the table is empty) and the owned ranks (empty OP_UpdateAA). The server answers with:

- one OP_SendAATable per rank (each ability's owned rank and the next, or rank 1; hundreds of packets, some repeated);
- OP_RespondAA and OP_AAExpUpdate.

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

- **Readiness:** reuse = the rank's seconds − TotalEffect(SPA 264, the ability id) when the rank has a reuse time. Ready when reuse + the latest of its timers ≤ now.
- **Trainability:** not owned, enough unspent points, the spend limit, level, monster-only, class, not grant-only, prerequisites met, the expansion bit.
- The owned ranks' effects go into the character sheet (`SheetAbility`, CacheAAEffects).

#### Training

A right click on a guildmaster (class 20..35, within 20 units) whose class is the profile class + 19 turns auto attack off and sends OP_GMTraining; otherwise eqstr 1467. (EQEmu's `Character:AllowCrossClassTrainers` shows every guildmaster as the viewer's class.)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| GMTraining | both | 0x1966 | 448 B: u32 guildmaster, u32 player, 100 × u32 skill limits (0 in the request), f32 greed +0x198 (EQEmu 1.08), u8 success +0x19c (EQEmu 1), 32 language bytes +0x19d (EQEmu 0xc9), 3 B |
| GMTrainSkill | C→Z | 0x2a85 | 12 B: u32 guildmaster, u32 1 for a language, u32 skill or language id |
| GMTrainSkillConfirm | Z→C | 0x4b64 | 76 B: u32 skill (100 + n for language n), u32 cost (copper), u8 newly learned, `char[64]` guildmaster, 3 B. The client takes the coin, one practice point, and prints 12066 (learned the basics), 12067 (fluency) or 12068 (skill) |
| GMEndTraining | C→Z | 0x4d6b | 8 B: guildmaster, player (the server answers OP_GMEndTrainingResponse and the guildmaster's goodbye) |
| SkillUpdate | Z→C | 0x004c | 12 B: u32 skill (100..131: language n − 100), u32 value, u8 announce (EQEmu 1), 3 B. With the announce byte the client prints 12091 "You have become better at %1! (%2)"; EQEmu also sends its own "(value/max)" line, so both appear |

The window's rows are:

- the skills with a guildmaster limit above 0 that the class gets at some level (SkillCaps.txt), except 15, 33 and 72;
- then the languages the character may learn (a race × level table, with the necromancer, wizard, magician and enchanter exceptions).

The cost of a point is (value − 10)³ / 100 copper.

#### Tradeskills

`combine_container` (a bag in a possessions slot) and `combine_world_container` run these checks:

- the class tomes (types 0x18..0x1b: wizard, magician, necromancer, enchanter; 12010);
- racial forges, wheels and kits (0x1f..0x2d; 12011..12023);
- nothing on the cursor (12024);
- no stack of more than one (8504);
- something inside (12025);
- transformation molds (0x37, 0x38) ask first (6328 / 6339, ConfirmationDialog 0x86 with a copy of the request). A yes sends it; a no drops the pending request.

OP_TradeSkillCombine (above) goes out; the server's empty OP_TradeSkillCombine ends the wait, and the outcome comes as messages and item packets (the result on the cursor). An open world container's slots take items like any other (OP_MoveItem with slot type 4, slots 0..9).

#### Tasks

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| TaskDescription | Z→C | 0x3714 | u32 slot, u32 task id, u8 open journal, u32 type (0 task, 1 shared, 2 quest), u32 reward type; title; u32 duration, u32 duration code, u32 elapsed seconds (the RoF2 translator turns the start time into elapsed time); description; u8 has rewards, u32 coin, u32 xp, u32 faction; reward text; reward item link; u32 points; u8 reward selection |
| TaskActivity | Z→C | 0x08d3 | u32 slot, u32 type, u32 task id, u32 element, u32 step; short form (25 B, a hidden element): u32 0xffffffff, u8 optional; else i32 element type, u8 optional, i32 0, target name, counted item list, i32 goal, counted skill list ("-1"), counted spell list ("0"), zones, i32 dz switch, description override, i32 done, u8 1, zones again |
| CompletedTasks | Z→C | 0x4eba | u32 count, {u32 task id, title, u32 completed time} (the 50 most recent) |
| TaskSelectWindow / SharedTaskSelectWindow | Z→C | 0x705b / 0x48a2 | u32 count, u32 type, u32 giver spawn; per task: u32 id, f32 reward multiplier, u32 duration, u32 duration code, title, description, u8 reward preview, u32 elements, per element: i32 index, i32 type, i32 0, target, counted items, i32 goal, counted skills, counted spells, zones, description, zones (up to 40 tasks; more than 256 elements in a task refuses the packet, Hailstone's limit) |
| AcceptNewTask | C→Z | 0x0a23 | 12 B: u32 the selection's type, u32 task id (0 declines), u32 giver |
| SharedTaskAcceptNew | C→Z | 0x6646 | 20 B: 0, 0, u32 giver, u32 task id, f32 reward multiplier |
| CancelTask | both | 0x39f0 | 8 B: u32 slot, u32 type. C→Z abandons; Z→C removes the task |
| TaskHistoryRequest | C→Z | 0x5f1c | 4 B: the completed list index |
| TaskHistoryReply | Z→C | 0x3d05 | u32 index (50 or more is ignored), i32 count (the first 20 are read, as in the original); per completed element: u32 type, target, counted items, u32 goal, counted "-1", counted "-1", zones, u32 0, description, u8 '1', zones |
| TaskActivityComplete | Z→C | 0x5e19 | 24 B: index, type, task id, element, completed (0 failed), stage completed. The client shows 6005 "Task Stage Completed", or 6003 / 6004 with the title, as large on-screen text |
| TaskRequestTimer | Z→C | 0x7a48 | 4 B: milliseconds before another shared task request |
| TaskTimers | C→Z | 0x2b0f | empty (/tasktimers) |

The element text is the server's description, else the type's template. Templates (with / without a target):

| Type | String |
|---|---|
| kill | 3468 |
| loot | 3498 / 3470 |
| deliver | 3469 |
| speak | 6001 |
| explore | 6002 |
| tradeskill | 6048 |
| forage | 6049 |
| fish | 6050 |
| cast on | 6052 / 6051 |
| use skill on | 6054 / 6053 |
| touch | 6117 |
| collect | 611 / 610 |

%1 is the goal, %2 the target, %3 the items, %4 the spell names and %5 the skill names (";"-separated ids joined with " or ").

#### Reward selection

`net/reward_packets`, `game/reward_selection`; stock RoF2 managers.

- Pending rewards use `0x6411` (`AchievementReward`).
- Task previews use `0x6471` (`RewardSelection`) and cannot claim.

An offered task's Preview Reward sends the same action-4 request as an active task (gated by the offer's preview byte); it neither accepts nor declines the offer. Checked: `CTaskSelectWnd::WndNotification` @0x76b660, preview sender @0x463a90.

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

##### Reward set layout

A set is pending id, set id, title C string, f32 display multiplier, repeated set id, i32 option count. Each option is u32 wire id, u32 entry count, u8 common-to-all, u8 flags, label C string, then typed entries. Each entry starts with u32 type, entry id, string id, description C string.

| Entry type | Body |
|---|---|
| 0 text | |
| 1 money | platinum/gold/silver/copper DWORDs |
| 2 items | count and id/quantity/u8 lore/lore-group/name records |
| 3 experience | |
| 4 alternate ability | rank and cumulative rank |
| 5 AA points | amount and a byte flag |
| 11 generic points | four DWORDs, amount in the third and currency in the fourth |

Unknown types reject the whole packet. Wire option ids belong to the received collection, not a provider's catalog.

Pending action 0 with no reward does not erase unrelated tabs; preview action 0 clears the previous preview first; action 7 replaces either lane. Checked: manager receivers `0x4656e0` / `0x465a90`.

##### Reward display

The set multiplier applies separately to each signed 32-bit money denomination, the AA-points amount and the generic-points amount, truncating toward zero; item quantities, ranks and descriptions are unchanged. Raw wire values stay available in the API. A product outside the signed 32-bit range shows Unavailable (display only). Checked: `0x464290` / `0x464740` / `0x464900`.

##### Reward limits and ownership

Host limits: 8 MiB per packet, 256 sets, 4096 options, 16384 entries and items, 2 MiB aggregate text; titles, option labels and item names 4096 bytes, descriptions 16384. Decode is atomic. Inspection checks the item id and uses the ordinary item-display queue; it never changes inventory.

- Claims and inspections carry revision and tab tokens, checked again before sending.
- One claim can be pending at a time. After 10 s it is marked uncertain; replacing the collection does not allow another claim. Only a matching reply or session/zone retirement clears it.
- An old reply cannot remove a tab from a newer collection.
- Inspection replies carry only an item id (no nonce), so retired ids are held until their replies drain (256 per lane).
- Closing, zoning and reconnecting retire view ownership. Getters and drawing never query or claim.

#### Achievements

`net/achievement_packets`, `game/achievements`. The server sends definitions and authoritative state; the client awards nothing locally and needs no add-on or bundled catalog.

Keep the received definition order, including hidden definitions: an achievement id and a definition index are different identities.

| Packet | Direction | Opcode | Layout |
|---|---|---|---|
| Definitions | Z→C | 0xdab0 | u32 inflated length, exact zlib stream; inflated catalog below |
| Primary update | Z→C | 0x618f | u32 (unread by RoF2), u8 dense, u32 count; per row, u32 definition index only when sparse, then state |
| Progress | Z→C | 0x2b42 | u32 count, then 20-byte records: achievement id, component id, requirement id, requirement type (any value; part of the record's key), current count |
| Comparison snapshot | Z→C | 0x059d | one state for every received definition, no leading count or target identity; never initializes own state |
| Refresh | C→Z | 0x47cb | native one-byte 0x30 (servers may also accept an empty request) |
| Open window | Z→C | 0x171e | empty; opens the window without querying again |
| Compare request | C→Z | 0x1665 | u32 target spawn id, u32 requester spawn id |
| Link inspection | C→Z / Z→C | 0x675d / 0x28e0 | player name C string, u32 achievement id, state with counts, u8 trailing flag |
| Earned | Z→C | 0x17f8 | u32 achiever spawn id, achievement id, sound id, caret-delimited link metadata C string |
| Guild announcement | Z→C | GuildUpdate action 3 | action at 0, fixed 64-byte player name at 12, achievement id at 80, link metadata at 84 |

##### Achievement catalog

The inflated catalog has, in order:

1. A category count and categories: id, parent id (`0xffffffff` for a root), name, description, icon C strings, display order, association count and `{achievement id, text C string, display order}` records, then child count and child ids.
2. A definition count and definitions: id, name/description C strings, icon id, persistent byte, version, four component vectors in types 0..3 order, points, and a DWORD reward flag.

A component has id, u8 type, required count, name/description C strings, and u8 display order. That byte is not the full requirement id carried by progress.

##### Achievement state

A state is i16 status (Completed 0, Open 1, Locked 2, Hidden 3), then 16-bit LSB-first satisfaction words for component types **1, 2, 0**, with each bucket rounded up separately.

- Only Completed carries a completion timestamp.
- Link inspection also carries one DWORD count per component, in that same bucket order.
- Type 3 is descriptive and has no runtime bits or counts.

Link metadata is player name, achievement id, status, signed decimal 16-bit words, optional completed timestamp and counts, each followed by `^`. A chat envelope is `0x12 + '3' + metadata + apostrophe + visible name + 0x12`; the displayed name comes from the current catalog. Sharing puts the tag into the chat input without sending it.

##### Achievement initialization and progress

Initialization is definitions, complete dense primary, then progress, including an empty progress packet. Counts remain unknown until that last step; only then can omitted records mean zero.

- Progress replaces the value at the full achievement/component/requirement/type key.
- Distinct requirement counts are summed per achievement and component (wider integer) and shown on that component in every bucket 0..2 that holds it; the type is not a bucket.
- A record for an unknown achievement, or a component in none of buckets 0..2, is skipped; the other records still apply.
- Counts and satisfaction bits never infer status. A sparse state update keeps known counters.
- The primary update's leading u32 is ignored, so every well-formed update applies.
- Hidden or class-ineligible definitions keep their wire indices.

Checked: progress handler 0x403020, sum 0x7c53b0, primary update 0x403b90.

##### Achievement comparisons and links

Catalog replacement retires selection, linked detail and comparison views; all data retires on character/zone/connection changes.

- A comparison is bound to the current catalog and the exact nearby PC spawn (within 20). Only one reply (which carries no identity) may be outstanding; after cancellation or the 10 s timeout it is still awaited, so a late reply cannot attach to a newer comparison.
- Link inspection is bound to the named player, catalog and current detail; changing target does not affect it. A nonlocal link can show its received snapshot without a request.
- These views never overwrite the character's own state. Getters and drawing send nothing.
- A request in flight when definitions change keeps its original catalog for decoding; its reply then drains without touching the new views. Malformed replies do not clear it.

##### Achievement limits

Catalog decode is atomic and validates unique ids, category parent/child consistency, cycles and association references. Host ceilings:

- 8 MiB per packet, 64 MiB inflated catalog;
- 8192 categories, 65536 definitions;
- 262144 components (4096 per definition), associations and requirement records;
- 32 MiB aggregate catalog text, 64 KiB link metadata;
- individual names/icons 4096 bytes, descriptions/association text 16384.

Limits are Hailstone's, not the original's.

##### Achievement notifications

Earned notifications require a matching received PC spawn/name.

- Their chat uses achievement colours 348 (self) / 349 (others) and filter 32 (show all / self only / hide).
- The sound cue plays the packet's sound id at the achiever (stock 3695 from the player's assets), whatever the chat filter.
- Guild announcements also allow an unspawned member and produce only filtered guild chat.
- Hailstone adds a self-completion overlay; native notification timing is unknown (docs/unknowns.md).

Neither path changes own completion state.

### Chat and slash commands (`game/slash_commands.{h,cpp}`, `net/game_connection_social.cpp`)

#### Chat: matching a typed line

A typed line is matched against the 391-entry command table, less the commands this client leaves out: the four voice macro commands, /legendsofnorrath, /tcg and /mp3. 384 remain (`src/game/slash_commands.inc`, `tools/gen_slash_commands.py`).

- Localised names are tried first, then built-in names. A typed word matches the first command (in table order) it is a case-insensitive prefix of: `/wh` = `/who`, `/g` = `/gsay`, `/s` = `/split`, `/lfg` = `/lfgroup` by its localised name.
- GM-only entries never match for players.
- No match: eqstr 12380 for a `/` word, else the default channel (say).
- `;` and `[` lines go to the chat server.
- Dead players get 538 unless the entry's flag bit 0 allows it.

`/achievements` toggles the stock window, sends a refresh only on an explicit open, and works while dead.

#### Chat: echoes and templates

Chat out (OP_ChannelMessage 0x2b2d) is echoed locally with the client's own template; the server's copy (sender = the player) is dropped. A tell's copy comes back on channel 7 or 14 (EQEmu: 14, TellEcho) and prints 1400 "You told %1, '%2'".

| Channel | Echo | Incoming |
|---|---|---|
| say | 12344 | 1410 |
| group | 12346 (12345 without a group) | 1414 |
| guild | 12263 (12242 without a guild) | 1415 |
| tell | | 1416 |
| shout | 12348 | 1420 |
| auction | 12350 | 1421 |
| OOC | 12352 | 1422 |
| raid | 5109 (5108 without a raid) | 5112 |
| emote (channel 22) | "<name> <text>" | 1036 |

The language part is 1411 " in <language>," (language names from the client's table), or 1412 when the skill is 24 or less.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| WhoAllRequest | C→Z | 0x674b | 156 B: `char[64]` name, 64 B, u32 race, class, level low, level high, GM (1 / 0), guild (or -2 friends, -3 LFG, -4 traders, -5 buyers), type (0 zone, 2 corpse, 3 all, 4 ripoff); 0xffffffff = no filter |
| FriendsWho | C→Z | 0x3956 | ASCII `0` (zone) or `1` (all), then each saved friend name prefixed by a comma, then NUL; no trailing comma or count. The server answers with WhoAllResponse |
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

#### Chat: /who race labels

The stock chat lines and the `raceName` field returned by `hs.WhoResults()` use the race id sent in
`WhoAllResponse`, which may be the player's current illusion. As `HandleWhoAllResponse` (`0x4bc010`) does, a
playable race takes the client's own name (`CharacterZoneClient_GetRaceDescription`) and any other race the name
in `dbstr_us.txt` type 11 (`GetRaceDesc` at `0x515ce0`). Where RoF2 would print a missing, empty or `UNKNOWN RACE`
entry as it stands, Hailstone deliberately uses its built-in race label instead: `Tree` for the Illusion Tree model
(143), or `Race <id>` for an otherwise unknown id. A zero race id has an empty label.

#### Chat: /who friends

`/who friends` (also the localised `friend` word and `/who all friends`) sends the saved list through FriendsWho instead of WhoAllRequest.

- After the send it shows eqstr 12307 and a line of dashes (colour 0x119); an empty list shows eqstr 12308 instead.
- Without a chat server nothing is sent or shown, as in the original.
- The current UCS world's prefix is removed from saved names, case-insensitively (`World.Alice` and `world.Alice` become `Alice`); other worlds' prefixes stay.

Checked: Who_BuildAndSendRequest `0x524a8a` (send `0x524c1a`, lines `0x524c61`–`0x524c94`, no chat server `0x524a75`), CChatService::GetFriendName `0x7efa90`.

Stock EQEmu returns online friends from the whole world in the normal who response (whichever scope), hiding anonymous/roleplaying friends' details; Hailstone shows the response as is. Tested with packet and mock-server tests; not compared live against RoF2.

#### Chat: live test

Tried live against EQEmu with two clients: `/who`, `/who all <class>`, `/random`, `/loc`, `/time`, `/played`, `/emote`, `/tell` with `/reply` and `/rtarget`, `/afk`, `/anon`, `/lfg`, socials (`/smile`, `/smile <name>`, `/cheer`), `/follow`.

### Guilds (`net/social_packets.h`, state `game::GuildState`, window `guild.lua` in each UI pack, the developer one in `src/ui/social_windows.h`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| GuildsList | W→C, Z→C | 0x507a | 64 B 0, u32 count, count × {u32 id, name} (the encoder skips ids ≥ RoF2's limit without fixing the count) |
| GuildInvite | both | 0x7099 | 136 B (pass-through GuildCommand_Struct): `char[64]` other, `char[64]` me, u16 guild, 2 B, u32 rank (8, recruit, from the client). Received: "%1 is interested in having you join %2." (534) and a 10 s Accept/Decline dialog; for a member of that guild it is a rank change |
| GuildInviteAccept | C→Z | 0x7053 | 136 B: `char[64]` inviter, `char[64]` new member, u32 response (the offered rank, or 9 to decline), u32 guild id |
| GuildInviteExt / GuildInviteExtReply (Hailstone) | C→Z / Z→C | 0x1430 / 0x1431 | 80 B: u32 magic (`net/social_packets.h`), u16 version 1, u16 action, `char[64]` name, u32 guild id, u32 value (note 1) |
| HsZoneLinksRequest / HsZoneLinks (Hailstone) | C→Z / Z→C | 0x1438 / 0x1439 | Request 6 B: u32 magic "HSZL" (`net/zone_link_packets.h`), u16 version 1. Reply: magic, u16 version 1, u16 flags (0), u32 count, then count × {u16 from zone id, u16 to zone id} (note 2) |
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
| GuildStatus | C→Z | 0x7326 | 140 B (GuildStatus_Struct; EQEmu's DECODE wants exactly that): `char[64]` who (the target's name when none is given; 12260 without either), 76 bytes the original leaves unset (zeros here); the server answers in text |
| GuildDemote | C→Z | 0x2d4e | 132 B, cleared first: `char[64]` your member name, `char[64]` the target's, u32 new rank (rank + 1). Permission 9 for another member (5410; 516 unless your rank is at least theirs), 0x1e for yourself (the leader cannot) |
| GuildCreate | C→Z | 0x76d9 | 64 B: `char[64]` name (4..27 characters: 6369 / 6370; 6371 without a profile) |
| GuildDelete | C→Z | 0x3708 | 68 B, cleared first: u32 guild id, own name. Leader only (9059); sent after the confirmation dialog 0x6d (9060), then 9061 |
| SpawnAppearance 22 / 23 / 52 | Z→C | 0x0971 | {u16 spawn, u16 type, u32 value}; colour 0x111 (note 3) |

1. GuildInviteExt actions: 0 hello after every zone-in (the reply's value 1: invitations reach other zones); 1 invite (C→Z: whom, rank 8; Z→C: the inviter, rank); 2 the invitee's answer (the rank, or 9 / 0xb). Not in EQEmu's opcode table: answered by `server/`'s Lua script. Without the hello's answer, /guildinvite stays EQEmu's same-zone OP_GuildInvite.
2. HsZoneLinks: sent once a session when the map's World view opens, after the zone answered the settings request. The pairs are the zone points the server's expansion and content flags allow. Optionally followed by: i16 current expansion (−1 all), u32 count, count × u16 zone id (the zones they allow). Not in EQEmu's opcode table: answered by `server/`'s Lua script (`server/README.md` "Zone links").
3. SpawnAppearance guild types:
   - 22, the guild id (0xffffffff none). For us, a change prints 12878 "You have joined %1.", or on leaving 12880 "You are no longer a member of %1." (12879 when the list does not name the old guild). Another spawn joining our guild: 12881; leaving it: 12882.
   - 23, the rank. Ours prints 7655 "You are now the rank of %1." when it differs from PcBase::GuildStatus (the removal's rank 0 is not kept). Another member's: 7656.
   - 52, bGuildShowAnim. The name sprite's "<guild>" line needs it (EQEmu: the rank's permission 4, also the spawn record's guild_show), and the player not anonymous (roleplaying shows it).

EQEmu sends 22, 23 and 52 for a member to the whole zone on a join or removal (and at each zone-in), but a promotion's or demotion's 23 to that member alone, so 7656 is not seen there.

The guild window's Alt sends OP_GuildManageBanker with the member's banker bit kept (Send_OP_GuildManageBanker, kind 2). EQEmu makes bankers by rank.

#### Guilds: live test

Tried live with two clients (EQEmu 23.10.3, a guild made by `#guild create`):

- /guildinvite by name and on a target, in one zone and (with `server/`'s script) across zones: accept, decline, the 10 s timeout, an offline player and an unknown name.
- /guildsay, /guildmotd and the window's MOTD box (468), /guildpromote and /guilddemote (command and buttons), /guildremove (another and yourself), /guildleader (the old leader becomes rank 3), the join and leave lines.
- The window's roster and live updates; public note (OP_GuildPublicNote, then OP_GuildMemberPublicNote to the guild); personal note (the character's GuildNotes.txt); alt flag; a rank's permission cell (OP_GuildUpdate action 5; EQEmu resends every rank's table) and rename (action 4); the "<guild>" name line.

EQEmu's `#guild create` does not send the new leader the rank permissions (Client::SendGuildRanks runs at zone-in), so its permission checks refuse until the leader zones.

#### Guilds: guild bank live test

Tried live (OP_GuildBank and OP_GuildBankItemList, in the table further down; one member, a class 66 banker): the zone-in item list, the banker's greeting (1465), deposit, Put in Bank, an item's permission, split, merge, withdraw and view.

EQEmu's deposit acknowledgement has its failed flag where RoF2 reads the slot to select, so the selection goes to deposit slot 0 (1 after a refusal).

### Raids (`net/social_packets.h`, state `game::RaidState`)

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| RaidInvite | C→Z | 0x55ac | 140 B RaidGeneral: u32 action, `char[64]` player, u32, `char[64]` leader, u32 parameter. The u32 at +0x44 is the raid id (from RaidUpdate 8). Actions in note 1 |
| RaidUpdate | Z→C | 0x3973 | by action (note 2) |
| RaidDelegateAbility | both | 0x2b33 | 92 B DelegateAbility_Struct (note 3) |

1. RaidInvite actions:
   - Invite: action 3 when not in a raid, 0 into ours; player = invitee, leader = me (5064).
   - Accept (`/raidaccept`): action 1 {inviter, me} (5079).
   - Decline: action 31 {inviter, me, 5081} (5080; EQEmu has no handler).
   - Leader change: 30 {me, new leader}.
   - Leaving (`/raiddisband`): 5 {me, me}; the leader's removal of the raid target: 5 {me, member}.
   - The leader's commands: 6 move {leader, member, group 0..11 / 0xffffffff}; 7 remove group leader {leader, member} (EQEmu: NYI); 8 / 9 lock / unlock {leader, leader}; 0x20 loot type {me, me, 1..4}; 0x21 / 0x22 add / remove looter {leader, member}.
   - 35 the MOTD (1164 B); 0x24 a member's note (204 B: {me, member} and `char[64]` note at +0x8c).
2. RaidUpdate actions:
   - 0 add (148 B: + u8 class @140, level, group leader, 5 B; parameter = group 0..11 or 0xffffffff); 1 remove; 5 disband (5062); 8 create; 10 the zone-in reset (136 B); 17/18 lock/unlock; 20 invitation (5065, 5067).
   - 14 the raid leader's leadership ranks and 30 leader (392 B: the 64 dwords from +136 are Leadership_SetRaidAbilityRanks' block, raid ability n at dword n; 30 also 5069 / 5070).
     EQEmu sends the new leader's name at both +72 and +4 in action 30. The client prefers +72 and falls back to +4 when it is empty.
     If both names are empty, the client applies the ranks and keeps the previous leader without announcing a change. This is a deliberate change from RoF2, whose CRaid_HandleSetLeader (0x47e850) reads +72 alone and clears the leader, printing 5070 with a blank name.
   - 32 loot type (+136); 33 / 34 looter added / removed (+72); 35 MOTD (1164 B; 6349).
   - 36 a member's note (204 B: the member at +72, the note at +140; EQEmu sends the notes after the roster).
3. RaidDelegateAbility:
   - C→Z: ability 3 main assist / 4 marker, the member's name at +28 (EQEmu toggles the role).
   - Z→C: ability, member number (EQEmu: assists 1..3, markers 0), action (1 removes), spawn id @20, name @28.
   - The client keeps the raid's three of each by name (the number only places an assist), lines 0x2242..0x224a.

Tried live: `/raidinvite` and accept, `/rsay` (template 5112 with an empty language part), and `/raiddisband` (leadership passes to the other member).

### Chat server (UCS; `net/chat_server.{h,cpp}`)

World sends `OP_SetChatServer2` "host,port,short.Name,<type><key>" (`world_packets.h decode_chat_server`). After zone-in, the client opens a second UDP session to it (one-byte opcodes, EQEmu `utils/patches/mail_opcodes.conf`).

DNS runs off the frame thread; commands typed meanwhile wait for the login. Every payload the client sends starts with one byte EQEmu skips.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| MailLogin | C→UCS | 0x01 | 0, "<mail prefix>.<short>.<name>", "<type><8 hex key>" |
| Mail | C→UCS | 0x02 | 0, command text: "join a, b", "leave a", "leaveall", "list", "announce", "<n> text" (channel n), "buddy [-]name", "ignoreplayer [-]name", "afk on silent" |
| Mail | UCS→C | 0x02 | the joined channels, comma-separated, in /1../10 order |
| ChannelMessage | UCS→C | 0x03 | channel, "short.Name", text, ["SPAM:0:"]; channel "" = the server's own text. Shown as 6723 "%1 tells %2, '%3'" with "channel:n" (6722 for our own) |
| ChannelAnnounceJoin / Leave | UCS→C | 0x04 / 0x05 | channel, name |
| Buddy | UCS→C | 0x06 | u8 1 added / 0 removed, name (the whole list at login) |
| Ignore | UCS→C | 0x15 | u8 0 added / 1 removed, "<mail prefix>.short.Name" |

Tried live: `/join`, `/1 <text>` to the other client, `/friends <name>` and the list.

### Duels, inspect and player commands (`net/player_packets.h`, `GameConnection` in `net/game_connection_player.cpp`, rules in `game/interact.{h,cpp}`)

Tried live against EQEmu with two clients (playprobe `--duel`, `--duel-wait`, `--duel-decline`, `--inspect`, `--cmd`): challenge, accept and decline (the challenger can challenge again at once), inspect both ways, `/keys`, `/rewind`, `/corpse`, `/toggleinspect`, `/autosplit`, `/language`, `/bandolier`. A duel's end (death or despawn), `/surname` (needs level 20), `/corpsedrag`, `/shield` and books have unit tests only.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| RequestDuel | C→Z | 0x3af1 | 8 B: target, our id (EQEmu swaps them into Duel_Struct {initiator, target}) |
| RequestDuel | Z→C | 0x3af1 | 8 B: the challenger twice; to the challenger after an acceptance: the accepter twice. The client reads +4 |
| DuelAccept | C→Z | 0x68d3 | 8 B: challenger, our id; sent right after an OP_RequestDuel with the same body |
| DuelAccept | Z→C | 0x68d3 | 8 B; the client does nothing with it |
| (duel refusal) | both | 0x52f7 | 12 B: challenger, our id, u8 kind (0 declined or ignored, 1 already dueling, 2 considering another), 3 uninitialised bytes (note 1) |
| DuelDecline | C→Z | 0x6a46 | 8 B: partner, our id; sent when the partner dies by our hand (12100) or despawns mid-duel (1408) (note 2) |
| DuelDecline | C→Z | 0x6a46 | 12 B (EQEmu's DuelResponse_Struct): challenger, our id, 0; sent beside a kind 0 refusal (note 3) |
| FormattedMessage 1383 | Z→C | | EQEmu's answer to that DuelDecline, to the challenger: "%1 has declined your challenge to duel to the death." (note 4) |
| InspectRequest | C→Z | 0x57bc | 8 B: target, our id; players only, within 20 (else 12378). `/inspect`: 8516 no target, 8527 too far, 6821 not a player, OP_LDoNInspect (empty) for class 62 objects |
| InspectAnswer | Z→C | 0x71ac | 1860 B: requester, inspected player, 23 × char[64] worn names, 23 × u32 icons (0xffffffff none), char[288] message. Shown within 20 (else 1402); the window closes when the player goes beyond 20 (checked once a second) |
| InspectMessageUpdate | C→Z | 0x4d25 | 256 B: our inspect message |
| Surname | C→Z | 0x0423 | 100 B: our name[64], u32 0, surname[32]; after ConfirmationDialog 0x6f (9050), prints 9051. No argument with a surname: dialog 0x75 (5155) → empty OP_ClearSurname 0x3fb0 |
| GMLastName | Z→C | 0x46ce | 200 B: name[64] (the key), gmname[64], lastname[64], 4 × u16 (EQEmu sets all to 1; the client tests the dword at 196). The named spawn (and the profile, for the player) takes lastname; apply 0 or an unknown name is ignored |
| LevelAppearance | Z→C | 0x3bc9 | 64 B: u32 spawn id, 5 × {u32 effect id, u32 attach slot, u32 flag}. EQEmu sends effects 0x4d..0x51 (77..81, actoremittersnew.edd) on a level-up, on spell point 1 (chest) or 2 (head), flag 1 for two of them. A group with effect id 0 is skipped. Each used group becomes an emitter of the actor file on the spawn's spell point: flag 0 attached to the point, flag 1 placed once on the floor below it, 0.05 above, with the point's scale (docs/unknowns.md) |
| GMSummon / CorpseDrag | C→Z | 0x486f / 0x0904 | 152 B (GMSummon_Struct): corpse name[64], our name[64], u32 0, u32 zone id, the corpse's y, x, z (float bits), u32 0; the corpse is the target, else our first corpse in the zone (13048 none); prints 13047 |
| CorpseDrop | C→Z | 0x7037 | the target's name and NUL |
| Shielding | C→Z | 0x52e5 | 4 B: target (warriors of level 30+; 4030/4031/4033/6826) |
| KeyRing | C→Z | 0x1219 | "KEY\0"; EQEmu answers with the key ring as text |
| Rewind | C→Z | 0x1745 | empty |
| SpawnAppearance | C→Z | 0x0971 | /autosplit: {our id, 28, 0/1} |
| Bandolier | C→Z | 0x7677 | 40 B: u32 action (0 create, 1 remove, 2 activate), u8 set, char[32] name, 3 B (note 5) |
| ReadBook | C→Z | 0x72df | 8216 B: u32 window 0xffffffff, slot {i16 slot, i16 sub, i16 aug −1, i16 0}, u32 book type, u32 target, u8, u8, char[8194] the item's book file |
| ReadBook | Z→C | 0x72df | the same struct with the text; type 0 note, 1 book, 2 item info |
| RequestTitles / SendTitleList | C→Z / Z→C | 0x6344 / 0x2d08 | empty / {u32 count, count × {u32 id, prefix, suffix}}: the titles we may take (`net/player_packets.h`) |
| AltCurrency | Z→C | 0x6b6d | mode 8: u32 count, count × 25 B {currency, 1, currency, item id, icon, stack size, u8 shown}; mode 7 (88 B): `char[64]`, currency, 1, amount, 2 × u32 |
| AltCurrencyReclaim | C→Z | 0x0339 | 16 B {currency, 0, count, flag: 1 put the cursor item in, 0 take `count` out} |
| AltCurrencyMerchantRequest / Reply | C→Z / Z→C | 0x5409 / 0x27a2 | u32 npc / text "cur\|1\|cur\|n" then "^name\|item\|cost\|0\|1\|races\|classes" per item |
| AltCurrencyPurchase / SellSelection / Sell | C→Z | 0x3788 / 0x532a / 0x40b6 | 12 B {npc, item id, 1} / 80 B {npc, 8 B typeless slot, 17 × u32} (the answer is EQEmu's 88-byte reply, the name at +10, the cost at +76) / 20 B {npc, slot, charges, cost} |
| LeadershipExpToggle / PurchaseLeadershipAA | C→Z | 0x74bd / 0x6c55 | 1 B on / off; u32 ability index (0..15 group, 16..31 raid) |
| UpdateLeadershipAA / LeadershipExpUpdate | Z→C | 0x0026 / 0x2797 | 12 B {ability, new rank, points left} / 28 B {f64 group exp, u32 group points, u32, f64 raid exp, u32 raid points} |
| BlockedBuffs / RemoveBlockedBuffs | both | 0x3033 / 0x0de7 | 128 B: 30 × i32 spell (-1 none), u32 count, u8 pet, u8 initialise (1 replaces the list), u16 flags (note 6) |
| AggroMeterTargetInfo / AggroMeterUpdate / AggroMeterLockTarget | Z→C / Z→C / C→Z | 0x16bc / 0x1781 / 0x1643 | 8 B {lock id, target id} / {u8 secondary changed, [u32 secondary id], u8 count, × {u8 type: 0 us, 1 secondary, 2..6 group, 7..26 extended targets; u16 percent}} / u32 spawn (0 unlocks) |
| SendFindableNPCs | Z→C | 0x4613 | 112 B {u32 0 add / 1 remove, spawn id, `char[64]` name, `char[32]` surname, race, u8 class, 3 bytes unread} (note 7) |
| FindPersonRequest / FindPersonReply | C→Z / Z→C | 0x5cea / 0x7e58 | 40 B {u32, spawn id, u32, u32, our f32 y, x, z, 3 × u32} / 12-byte points {y, x, z}: the destination, then the path (its last point repeated); empty for none |
| PVPStats | Z→C | 0x4b15 | 4612 B: kills, deaths, points, career points, best / worst / current streak, infamy, vitality, then 88-byte entries {`char[64]`, level, race, class, zone, time, points}: last death, last kill, 50 recent kills (EQEmu fills only the counts) |
| PVPLeaderBoardRequest / Reply | C→Z / Z→C | 0x04aa / 0x071f | 4 B sort (0 kills, 1 points, 2 infamy) / 8024 B {u32, our kills, points, rank, deaths, infamy, 100 × {`char[64]`, kills, deaths, points, infamy}}, zeroed by EQEmu |
| CrystalCountUpdate | Z→C | 0x467f | 16 B, RoF2 order: radiant now, radiant career, ebon now, ebon career |
| LFGuild | both | 0x3165 | u32 command first (note 8) |
| RequestGuildTributes / SendGuildTributes | C→Z / Z→C | 0x4d5e / 0x1877 | 4 B (unused), right after OP_SendTributes on the first zone-in, in a guild or not / u32 guild, then the personal TributeInfo layout |
| GuildSelectTribute | both | 0x649f | 16 B {id, tier, id, u32} / {id, tier, id, description} |
| GuildSendActiveTributes / GuildTributeToggleReply / GuildTributeFavorAndTimer | Z→C | 0x3771 / 0x20ae / 0x7fe0 | 40 B {guild, favour, timer ms, on, 8, 2 × id, 2 × tier} / the same / 16 B {guild, favour, timer, trophy timer} |
| GuildSaveActiveTributes / GuildModifyBenefits / GuildTributeToggleReq | C→Z | 0x5f3f / 0x34cd / 0x7745 | 48 B {command, 16, master, 2 × id, 2 × tier, 8} / 80 B {0 save / 1 modify, data, 12, master, 2 × id, 2 × tier, 40} (echoed) / 12 B {1 on / 0 off} |
| GuildOptInOut | both | 0x0727 | C→Z 80 B {guild, on, `char[64]` member, command, master}; Z→C 88 B {guild, `char[64]` member, on, trophy on, donations, time, command} (note 9) |
| (trophy opt-out) | C→Z | 0x02bc | Not in EQEmu's table: OP_GuildOptInOut's request layout (Send_OP_GuildTrophyOptInOut @0x424120), from the guild window's Trophy Tribute Opt Out (never enabled on EQEmu, which has no trophy tribute) |
| GuildTributeDonateItem / GuildTributeDonatePlat / OpenGuildTributeMaster | both | 0x5bcb / 0x43d2 / 0x49ea | 36 B RoF2 {type, u16 slot, sub, aug, u16, quantity, master, u32, guild, 8} → 24 B {type, slot, sub, aug, u16, quantity, u32, favour} (the item deleted unannounced) / 20 B {platinum, master, u32, guild, u32} → 12 B {platinum, u32, favour} / StartTribute (12 B), class 64 |
| Translocate / RezzRequest → RezzAnswer | both | 0x6580 / 0x3c21 → 0x701c | 92 B Translocate_Struct (zone, spell, u32, `char[64]` caster, y, x, z, u32 complete +88) / 236 B RoF2 Resurrect_Struct (rezzer +92, corpse +160, action +224); the client echoes the packet with the yes / no |
| SharedTaskInvite / SharedTaskInviteResponse | Z→C / C→Z | 0x3444 / 0x7582 | 136 B {i32, i32 invite id, `char[64]` task, `char[64]` inviter} / 12 B {0, invite id, i8 accepted, 3} |
| Stun / ItemRecastDelay / Bind_Wound | Z→C | 0x36a4 / 0x15a9 / 0x0386 | u32 duration ms (+2 B) / 12 B {delay s, recast type, bool} / 8 B {u16 target, u16, u16 type: 3 start, 1 done, 4 target dead, 5 gone, 7 no bandage} |
| Sound (QuestReward_Struct) / CashReward | Z→C | 0x1a30 / 0x5f7a | 68 B {giver, our id, exp, faction, mod, copper +20, silver, gold, platinum} / 16 B {copper, silver, gold, platinum}: coin added without a coin update |
| GMKill | Z→C | 0x51d3 | 132 B {`char[64]` the named player, `char[64]` the GM, u32} (EQEmu forwards the GM's own /kill packet to every client in the zone, the GM's included, and does not kill the player itself): the name is compared with our name without case, and on a match the struck-down line (eqstr 1391, colour 10), wave 0x8f and the client's own kill: the profile HP (`0x45ffa0`, PcProfile +0x3394) and the own spawn's HPCurrent (`0x4423b0`, +0x2e4) set to -10000 and the RespawnTimer (+0x2bc) cleared, with nothing sent back, no OP_Death and no respawn window, so the player lies at -10000 until the server's next OP_HPUpdate (in the original the zone message handler at `0x4c3250` takes 0x51d3 to the case at `0x4c8a23` through the byte and pointer tables at `0x4d4998` / `0x4d4980`) |
| GuildMemberAdd / Delete / Level / PublicNote / SetGuildRank (= RankAltBanker) / Details | Z→C | 0x2925 / 0x3141 / 0x1bd3 / 0x01f9 / 0x0b9c / 0x69b9 | u32 guild first; see GameConnection::handle_world_social |
| Charm / DisciplineUpdate / Untargetable / YellForHelp / GetGuildMOTDReply | Z→C | 0x5d92 / 0x759e / 0x053c / 0x4e56 / 0x4f1f | 12 B {owner, pet, 1 on / 0 off} / 300 × u32 / 8 B {spawn, targetable} / u32 yeller / the GuildMOTD struct |
| Weblink | Z→C | 0x6f4b | the address, NUL-terminated (EQEmu `Weblink_Struct`); the case @0x4c5b5d opens it in an HTML window (docs/browser.md) |
| Bug | C→Z | 0x73f4 | 8740 B EQEmu BugReport_Struct (not translated): category and its name, reporter, UI path, server x, y, z, heading, minutes played, target id and name, flags (1 can duplicate, 2 crash, 4 target, 8 character flags), the report at +2592, system info at +4642 |
| VetClaimRequest / VetRewardsAvaliable / VetClaimReply | C→Z / Z→C / Z→C | 0x1126 / 0x590e / 0x16d4 | 80 B {`char[68]`, claim id (0xffffffff: the list), u32, action} / {count, × {claim id, available, item count, u8 enabled, × {u32 length, name (not NUL-terminated), item id, charges}}} / 80 B, action 0 claimed, 1 refused |
| Consume | C→Z | 0x4b70 | 24 B RoF2 Consume_Struct {12 B slot, u32 0xffffffff, u32 1 food / 2 drink, u32 1 (the right click: Handle_OP_ItemVerifyReply; the auto-eat here still sends 0xffffffff, type, 2)}; EQEmu deletes one unannounced and answers OP_Stamina |
| ItemVerifyRequest | C→Z | 0x189c | 16 B {12 B slot, u32 target spawn id}: a right click on a common item in the possessions, /useitem, the potion belt; IsItemPending + 1 |
| ItemVerifyReply | Z→C | 0x097b | 20 B {12 B slot, i32 spell (0: the item's click spell; negative: nothing), u32 target}; IsItemPending − 1 (not for slot 0x407) (note 10) |
| Dye | C→Z / Z→C | 0x23b9 | 36 B: nine tints 0xUURRGGBB (UU 0xff tinted); each changed slot takes a Vial of Prismatic Dye (32557); the server sends the wear changes and an empty OP_Dye |
| UpdateAura | both | 0x1456 | Z→C 80 B {0 add, type, `char[64]` name, the aura's spawn id, icon}; both 8 B {1 remove, spawn id} (the client asks, the server echoes); Z→C 4 B {2} clears them |
| GuildBankItemList | Z→C | 0x7850 | while zoning into the guild hall (345): 40 deposit then 200 main entries, each u8 present, then {permission, who for, donator, item id, icon, quantity, u8 can merge, u8 usable, name} |
| GuildBank | both | 0x5134 | RoF2's actions are EQEmu's less one (note 11) |
| AdventureMerchantRequest / Response | C→Z / Z→C | 0x6922 / 0x3e47 | 8 B {type 1 LDoN, 2 Discord, 4 Norrath's Keepers, 5 Dark Reign; npc} / u8 count, then "^name\|id\|cost\|theme\|stackable\|lore\|races\|classes" per item |
| AdventureMerchantPurchase / Sell | C→Z (Sell both) | 0x5b72 / 0x2f9b | 16 B {type, npc, item id, u32} / 24 B {1, npc, 8 B typeless slot, charges, price}; the server echoes the sale and deletes the item unannounced (so does OP_AltCurrencySell's echo) |
| LeaveAdventure | C→Z | 0x5d18 | no body |
| DzCompass | Z→C | 0x3e0e | {u32, u32 count, × 24 B {u16 zone, u16 instance, type, switch id, f32 y, x, z}} (server y, x: zone-file x, y) |
| SharedTaskMemberList / MemberChange | Z→C | 0x1e7d / 0x0119 | {u32, u32, count, × {name, u32 mission, u8 leader}} / {u32, u32, u8 added, name} |
| AdventureInfoRequest / AdventureInfo / AdventureDetails | C→Z / Z→C / Z→C | 0x3cb0 / 0x4c54 / 0x5648 | u32 the recruiter (class 60) / its text or the server's refusal / the adventure offered (NUL-terminated strings) |
| AdventureRequest / LDoNButton | C→Z | 0x2c6c / 0x5327 | 12 B {risk 1 normal / 2 high, recruiter, objective 0 any .. 4 rescue} / 1 B accept the offer |
| AdventureData / AdventureUpdate / AdventureFinish | Z→C | 0x7171 / 0x1b01 / 0x400f | 2084 B {u32 reserved, `char[2048]` text, seconds to enter, seconds left, risk (EQEmu: 0 normal / 1 high), f32 x, y, z, show compass, u32 reserved} / 8 B {current, total} / 8 B {won, points} |
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

1. Duel refusal 0x52f7: not in EQEmu's table, so the server drops it; a kind 0 refusal therefore also sends EQEmu's DuelDecline below (not while a duel of our own runs or waits).
2. DuelDecline, 8 B: EQEmu wants 12 bytes and drops it; these stay 8 on purpose, because its Handle_OP_DuelDecline dereferences the partner without a null check and here the partner is leaving the zone. EQEmu ends both duels itself (Client::~Client, Client::Death).
3. DuelDecline, 12 B: sent only for a challenger in the spawn list. EQEmu clears both players' duel state (else the challenger can challenge nobody until it zones) and sends the challenger 1383.
4. FormattedMessage 1383: EQEmu's name argument is always "" (`Entity::GetName`), so the client fills in the player it challenged and ends its challenge.
5. Bandolier:
   - The name is at most 31 characters and a NUL: a deliberate difference (the original copies 32 with no NUL, which EQEmu's CreateBandolier strcpy's past `Name[32]`).
   - Create stores the four weapon slots' items (13, 14, 11, 22).
   - Activate moves them locally, as the server's `SetBandolier` does silently: a weapon put back takes the first free general slot, or the first free slot of a bag EQEmu's `CanItemFitInContainer` accepts (`game::server_bag_accepts`).
   - As in the original's `ProcessMove`, a swap is all or nothing: if a weapon has no such slot, nothing moves, eqstr 9342 is printed and nothing is sent.
6. BlockedBuffs: the server answers with its list (flags 0x54) or the spells removed. It keeps the lists per zone session, so the client sends them from the character ini at every zone-in (stock EQEmu stores no player lists). A spell a non-initialise answer adds is announced (eqstr 4566 / 4560), and its effects and its SpellGroup's come off the player or pet. Checked: CEverQuest::ProcessLocalPCIni @0x51e770, Handle_OP_BlockedBuffs @0x4bed90.
7. SendFindableNPCs: one per NPC the server's `findable` flag marks, at zone-in and as one spawns or goes. Hailstone's Find window lists them as points of interest (docs/navigation.md "Find"). Checked: Handle_OP_SendFindableNPCs @0x4b8b50.
8. LFGuild:
   - C→Z: 0 our entry (624 B: comment +72, time zone +596, u8 on +600); 1 the guild's (616 B: comment +12, from / to level +524 / +528, classes +532, AA +536, time zone +540, u8 on +544); 3 search players (28 B: 2 × u32, from, to, min AA, time zone, classes); 4 search guilds (24 B: 2 × u32, level, AA, time zone, class).
   - Z→C: 0 / 1 the entries (posted at +608 / +548); 3 {u32, count, × {name, level, class, AA, time zone, comment}}; 4 {u32, count, × {name, time zone, comment}}.
9. GuildOptInOut:
   - The named member (case-insensitive) takes the states, donations, time and the command's low byte as its flag.
   - EQEmu echoes the request's command, sends 1 after a donation and 0x2fb at a member's zone-in.
   - The guild window's Tribute Opt Out sends {guild, 0, the selected member} (Send_OP_GuildOptInOut; command and master 0).
   - The tribute benefit window's Opt In / Opt Out sends the opposite of the member's state: a deliberate difference, since the original (CTributeBenefitWnd @0x784f13) always sends 0 and could never opt back in on EQEmu.
10. ItemVerifyReply: EQEmu sends it first, spell 0, then acts (clicky cast, tome, scroll scribe). The client's cast of the click effect sends no OP_CastSpell (CastSpell send argument 0, @0x4bff60).
11. GuildBank:
    - Z→C: 0 a slot's update (230 B: slot, area, shown, item id, icon, quantity, permission, merge, usable, `char[64]` name, donator, who for); 9 acknowledgement (12 B); 4 deposit acknowledgement (16 B, failed at +12). Z→C 2 and 4 carry a u16 slot, u16 area at +12 the window selects (EQEmu: its failed flag); on 4 the held item goes.
    - C→Z, each {u32 action, u32 the banker's spawn id, u32}: 2 promote; 3 view (value 1 unless Shift); 6 withdraw, 7 split, 8 merge (24 B: u16 slot, u16 area, u32 item id, u32 value); 4 deposit the cursor item (16 B); 5 permissions (88 B: slot, area, item id, permission, `char[64]` member).

Duel state (`game::DuelState`) is the player we challenged or accepted, the challenger, and an on flag. While the duel is on, the opponent may be attacked (melee, abilities, detrimental spells) whatever the PvP flags and guilds, and a beneficial spell on a player outside the duel is refused (AllowedToAttack reads the on flag and the opponent). Hailstone ends a duel, running or offered, with a zone change: the original keeps its duel state across a zone, but EQEmu ends the duel when the client leaves (Client::~Client), and the old opponent's id names someone else in the new zone. The profile's bandolier sets and potion belt are decoded (`PlayerProfile::bandoliers`, `potion_belt`).

#### Adventures: requests and leaderboard

- EQEmu can send AdventureData with the usual reserved words and blank briefing and timers to retain door access
  for a completed instance. A subsequent AdventureUpdate can still carry the final count.
- EQEmu confirms leave and reports expiry with AdventureInfo text `You are not currently assigned to an adventure`
  (with or without a final period). A denied leave uses a red system chat message and retains the assignment.
- Stats and leaderboard requests are each gated for ten seconds (EQEmu: eight and four).
- The leaderboard category index interleaves order within theme: `order = category % 2`, `theme = category / 2`. Risk is the second selector's raw index.
- The player profile's counted LDoN block has six u32 entries: reserved, the five theme balances, then the available total (`rof2.cpp ENCODE(OP_PlayerProfile)`). Point updates replace these.

Checked: request senders `0x420030`, `0x4209f0`, `0x41fed0`, `0x41ff80`; stats `0x619570`, leaderboard `0x6173e0`.

The leaderboard reply carries no reliable category on stock EQEmu, so Hailstone serializes requests, holding the latest selection until the previous reply and the ten-second guard allow it; a missing reply is released after thirty seconds (Hailstone's choice). Late replies stay ambiguous (`docs/unknowns.md`).

### Spirit Shrouds and Monster Missions (`net/shroud_packets.h`)

| Opcode | Direction | RoF2 | Body |
| --- | --- | --- | --- |
| ShroudSelectionWindow | Z→C | 0x2ec3 | u32 shroudkeeper spawn ID, u32 status, u32 reserved, BEL-separated progression, branch, and form records. Nonzero status directs the player to the Shroud Bank. |
| ShroudRequestStats / ShroudSelect | C→Z | 0x2b37 / 0x1cd0 | u32 selection ID |
| ShroudSelectCancel | C→Z | 0x79da | empty |
| ShroudRespondStats | Z→C | 0x6c04 | u32 selection ID, u32 has-data flag, caret-separated name, description, class, level, stats, AA ranks and items |
| ShroudRequestAbility / ShroudRequestItem | C→Z | 0x3acc / 0x4b25 | u32 selection ID, u32 detail ID |
| ShroudRespondAbility / ShroudRespondItem | Z→C | 0x006e / 0x2507 | u32 selection ID, u32 detail ID, followed by the item instance for the item response. Ability definitions are provided by `OP_SendAATable`. |
| ShroudProgress / ShroudProgress2 | Z→C | 0x541d / 0x3823 | u32 row count followed by BEL-separated progression records |
| Shroud | Z→C | 0x6562 | u32 spawn ID, u16 profile offset, ZoneEntry spawn body. For the local player the offset begins a fixed 0x4ff8-byte base profile followed by a CharInventory body for the new form's possessions. |
| ShroudRemove | Z→C | 0x67e8 | empty (note 1) |
| AltStorage | both | 0x7b71 | u32 action (note 2) |
| AltStorageList / AltStorageNoItems | Z→C | 0x1115 / 0x0728 | u32 retention seconds, u32 count, {name, u32 icon, u64 key, u32 quantity} per row. NoItems is empty: 0x4136f0 prints eqstr 4558 in colour 0xf. A nonzero `ShroudSelectionWindow` status prints eqstr 4557 in colour 0xd (0x414320). |

1. ShroudRemove: RoF2 shows eqstr 5191 ("please wait") in a 45 s dialog (id 0x83; dispatch case 0x4d2645); Hailstone shows none and takes it to mean the next `Shroud` restores the original form, as the EQEmu draft sends it (docs/unknowns.md).
2. AltStorage:
   - To the server: Open (0) adds u32 entity ID. Withdraw (2) adds a reserved u32 and u64 storage key. Item inspection (5) adds a preview flag and a reserved u32 to the key fields.
   - From the server (CAltStorageWnd_HandleMessage 0x61da10): 1 closes the window; 2 acknowledges a withdrawal (all-ones key: refused); 3 clears the list; 4 removes the row with an i32 key; 6 carries {u32 platinum, gold, silver, copper} handed back from the shroud and prints eqstr 6188 in colour 0xf.

| Native Monster Mission opcode | Direction | RoF2 | Body |
| --- | --- | --- | --- |
| MonsterMissionCatalog | both | 0x3c54 | Refresh requests carry two zero u32s. Replies carry four u32 fields with task ID at +8, u8 lock existing assignments, u32 count, then {u32 template ID, i32 minimum, i32 maximum, u32 reserved, u8 selectable, name} per role. Nonpositive maximum means unlimited. |
| MonsterMissionStats | both | 0x3d20 | {u32, u32, u32 template ID, u32} followed by 4096 zero bytes in the request and caret-separated details in the reply. |
| MonsterMissionAbility | both | 0x34f4 | {u32, u32, u32 template ID, u32 rank ID} |
| MonsterMissionItem | both | 0x5f08 | The request uses the ability header with an item ID. The reply carries u32 item ID followed by the item instance. |
| MonsterMissionSelect | C→Z | 0x4eea | {u32, u32, u32 template ID} |
| MonsterMissionAssignment | Z→C | 0x2e8d | {u32, u32, `char[64]` member name, u32 template ID} |
| MonsterMissionResult | Z→C | 0x5a67 | {u32, u32, u32 template ID, u32 result}. Zero acknowledges the selection. |

#### Shrouds: profile type

The first u32 after the player profile's checksum and checksum size is the profile type (1 Spirit Shroud, 2 Monster Mission). The form snapshot uses the same base-profile fields. Character selection applies the roster's alternate race and class to the model. Checked: profile reader 0x57c650 → 0x7bf270 (`+0x340c`), snapshot 0x4c2ea0, character select 0x51c050.

#### Shrouds: native senders and missions

The native Spirit Shroud senders (0x41375f, 0x413842, 0x413912) count the opcode in their length; the payload sizes above exclude it.

The Quest Journal can reopen an offered mission catalog while that task is active; the client shows capacity from the catalog's role names and the roster's assignments. Checked: CTaskManager handler 0x482080, mission handlers 0x481e80, 0x482ae0.

The server decides form admission, storage, mission progress and rewards; profile and inventory change only when it sends them.

#### Shrouds: the EQEmu draft

The local EQEmu draft supplies the form snapshot and a shared-task catalog through `ShroudSelectionWindow`, with the active task ID in place of the shroudkeeper spawn ID.

- Its roster puts each role's class in the template field.
- Its inbound decoders must use the native payload sizes above.
- A withdrawal acknowledgement with the matching key removes the row; a refusal uses the all-ones key.

### Bazaar and barter (`net/market_packets.h`, `GameConnection` in `net/game_connection_market.cpp`, rules in `game/bazaar.{h,cpp}`)

Tried live with two clients (a trader on a rug stall, type 155, and a shopper): Begin Trader, slot selection and price load, the search, Find Trader (the button's 15 s wait), a row's Buy, /offlinemode's refusal 771. Also with playprobe (`--shop-trader`, `--bazaar`, `--bazaar-buy`): the trader list, search, a shop, and a purchase without coin (sub action 7).

RoF2 has no OP_TraderBuy. Everything goes through OP_Trader (trader mode) and OP_TraderShop (customers), the first dword being the action (`RoF2BazaarTraderBuyerActions`).

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| Trader 1 (begin) | C→Z | 0x4ef5 | 4404 B: u32 1, 200 × char[18] item GUIDs (the server's 16-digit serial), 200 × u32 prices; the slots with a price |
| Trader 2 / 11 | C→Z | 0x4ef5 | 12 B {2 end / 11 list my items, 0, 0} |
| Trader 3 / 20 | C→Z | 0x4ef5 | 32 B: action (3 price change, 0 withdraws; 20 reconcile after a sale), GUID[18], 2 B, u32 count (20), u32 price |
| Trader | Z→C | 0x4ef5 | {action, sub, …} (note 1) |
| BecomeTrader | Z→C | 0x61b3 | 8 B: spawn id, trader flag (the spawn's Trader field and nameplate) |
| TraderShop 28 | C→Z | 0x31df | 12 B {28, trader spawn, byte 0 + 3 uninitialised} (a right click on a trader within 20) |
| TraderShop 28 | Z→C | 0x31df | 12 B {28, trader, approval}: on approval the items follow as item packets of type 0x64 (non-stackables at the cursor slot: keyed by arrival, the GUID identifies them) |
| TraderShop 10 | C→Z | 0x31df | 284 B TraderBuy_Struct: action 10, method (0 at the trader, 1 parcel, 2 to the inventory), sub 0, trader (spawn id; search: character id), buyer[64], seller[64], 32 B, item name[64], GUID[17], 3 B, item id, price each, already sold, u32 (vouchers), quantity |
| TraderShop 10 | Z→C | 0x31df | the same struct, sub action = result (0 success, 1 failed, 3 out of date, 4 lore, 5 parcel cap, 6 pending, 7 funds, 8 tokens, 9 Krono) (note 2) |
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
| BuyerItems 14 / 7 | Z→C | 0x1a6a | {action, u32, a buy line}: 14 a viewed buyer's line (replaced, removed when not active, else added), 7 our own (stored at its slot) (note 3) |
| Barter 13 | Z→C | 0x243a | 76 B {13, spawn id, 64 B, u32 enabled}: the byte at +72 is the spawn's buyer flag; the active merchant's window closes |
| BuyerItems 9 (sell) | C→Z | 0x1a6a | {u32 9, the 0x178-byte sale record: u32 method (0 at the buyer, 1 a buyer in this zone, 2 elsewhere), u32 1, u32 buyer spawn, u32 buyer character (0 at the buyer), 8 B, three empty strings, the buy line with its buyer fields, u32 quantity, u32 0}; EQEmu's DECODE reads the same bytes |
| BuyerItems 9 / 10 (result) | Z→C | 0x1a6a | {u32 9 sold / 10 bought, the sale record} (note 4) |
| Barter 1 (search) | C→Z | 0x243a | 84 B: u32 1, `char[64]` text, u32 transaction (__EQGetTime), u32 0, u32 buyer character (the combo's buyer; 0 all), u32 scope (1 "Local Barterers"); EQEmu reads 83 B packed, its u16 zone from the scope dword's upper bytes (0) |
| BuyerItems 1 (matches) | Z→C | 0x1a6a | {u32 1, search text, u32 transaction, u32, u32, u8, u32 count, count × the line with its buyer fields (EQEmu: u16 zone, u16 instance at the zone dword)} (note 5) |
| Barter 2 / 3 (buyer mode) | both | 0x243a | 4 B {2 on / 3 off}: asked when it differs from the flag (off clears it at once); the server's 2: 6058, the welcome (0x13) and BuyerItems 6 with the enabled, active lines; 3: 6059 (EQEmu: on needs a barter stall door, opentype 155) |
| Barter 0 (item search) | both | 0x243a | C→Z 68 B {0, `char[64]` text} (6057 to the status text); Z→C {0, u32 count, count × 76 B {`char[64]` name, u32 item id, u32 icon, u32}} |
| BuyerItems 6 / 7 (our lines) | C→Z | 0x1a6a | 6: {u32 6, u16 count, count × line}; 7: {u32 7, line} (create / modify / toggle in buyer mode); our lines' buyer fields are zero (13 B) |
| Barter 8 (remove a line) | both | 0x243a | 8 B {8, slot} (in buyer mode; EQEmu reads +4 and echoes BuyerItems {7, 0, slot, 0}); Z→C {8, slot} empties the line |
| Barter 0x13 (welcome) | C→Z | 0x243a | 328 B {0x13, u32 0, u8 0, 63 B, `char[256]` message at +0x48} |
| Barter 0x0f / 0x15 (inspect) | C→Z | 0x243a | 0x0f 32 B {0x0f, 0, buyer (spawn id in this zone, else character id), zone, slot, compensation index (-1 the item bought), item id, 2 with Shift}; 0x15 12 B {0x15, item id, 2 with Shift} (an item search result) |
| Barter 0x10 / 0x14 / 0x16 | Z→C | 0x243a | 0x10 {name}: 6060 "browsing"; 0x14 {name at +8, text at +0x48}: 6070 / 788 (EQEmu sends a chat line instead); 0x16: "Your inventory is full!" |
| Barter 0x11 / 0x14 / 0x17 | C→Z | 0x243a | 0x11 4 B the hall's welcome; 0x14 8 B {0x14, buyer character} the buyer's greeting; 0x17 4 B the buyers for the search window's combo, once per zone |
| Barter 0x1a / 0x1b, BuyerItems 0x19 | Z→C | both | 0x1a {zone, character id, spawn id, name[64]} adds a buyer to the combo, 0x1b {character id} removes one, BuyerItems 0x19 {count, records} replaces them (EQEmu never sends it; the records stop where the data does) |
| BuyerItems 5 | Z→C | 0x1a6a | {5, u32 length, "<count>$<record>$..."}: lines in the BART ini format, stored and saved; records stop when the text does, whatever the count (EQEmu never sends it) |

1. Trader Z→C actions:
   - 2 trader off (6741);
   - 3 price answer (sub 0 6733, 1 6736, 2 6735, 3 6734);
   - 10 a sale (TraderBuy_Struct: 6744 and the register sound, then reconcile);
   - 11 the listing (4404 B like the begin: 6737, 6738 per item, 6740);
   - 13 a customer (6745);
   - 22 TraderAck2 (sub 0: 6742, trader mode on, ask for the listing; 12: 6743; 10/5/else: 784/786/785);
   - 21 {name, code}.
2. TraderShop 10 Z→C: method 0 prints 1437 (coin taken), else 787 "You send…"; then "You successfully purchased …". EQEmu's `Client::TradeRequestFailed` echoes the request (sub 0) with trader id and already-sold 0xffffffff; the client reads that as sub 1 (no coin taken; `is_eqemu_trade_failure`).
3. The buy line: u32 slot, u8 enabled, then when enabled u32 item id, name, u32 icon, u32 quantity, u8 active, u32 cost, u32 n, n × {id, quantity, icon, name}, u32 buyer spawn, u32 buyer character, u32 zone, buyer name. EQEmu leaves out the last four (read as 0) and sends removals as {7 / 14, 0, slot, 0}.
4. BuyerItems 9 / 10 sale record: +0 method, +4 result (0 sold, 1 failed, 3 pending, 4 out of date, 5 buyer full, 6 no item, 7 / 9 Krono, 8 same zone), buyer [64], item [32], seller [64], the line (name, cost, compensation), +0x170, +0x174 the quantity. EQEmu writes its sub action at +0 and its error code at +4, the line's item id as 0xffffff.
5. BuyerItems 1: read only for the last search with the window open; 6063 at 200 or more, 6064 the count. Lines stop where the data does: a count with nothing behind it makes no empty lines.

#### Bazaar: trader window

- The trader window opens only with the membership feature 0xe and in The Bazaar (zone flag 0x10000000, only zone 151).
- Begin Trader needs the player standing on a trader stall (the floor switch under the feet, types 0x99 / 0x9b, else 6743).
- Its 200 slots are the items in Trader's Satchels (bag type 51) in the general slots, in slot order.
- Prices come from `[ItemToSell]` of `BZR_<name>_<server>.ini` (key: the item id, or "id:charges" for an item with a click spell). Set Price (6751) writes them for every slot holding the same item.
- Prices show as "12p 3g 4s 5c".

Each price button opens the amount dialog: gold, silver and copper 0..100 as on the original. Platinum is 0..4,294,967 where the original allows 1,000,000: a deliberate difference, since prices are 32-bit copper on the wire and in EQEmu's `trader.item_cost` (held at 4,294,967,295 copper).

#### Bazaar: barter windows

Code: `game/barter.h`. `/buyer` needs the membership feature 0xf (else "Your ability to create buy lines in the bazaar has been disabled ...", colour 0x14f). It opens the buyer window only with the expansion bit 0x100 (or a server named "test") in a barter zone (151, 346).

The window loads the 200 buy lines and the welcome text from `BART_<name>_<server>.ini`; here BART.ini in the character's folder:

- `[BuyLines] BuyLine00..` as "*2^slot^enabled^active^id^name^icon^price^quantity^n[^id^qty^name^icon]...";
- `[General] Welcome`.

Closing it turns buyer mode off and saves them.

- Out of buyer mode, Create / Modify / Toggle / Remove change the lines locally; in buyer mode they go to the server, whose answers change them.
- The status text and `btrlog.txt` get 6057..6069, the reports ("You've bought N item from X for <coin> and the following: ..."), "Listing Active Lines:".
- The barter search's sale needs the RoF expansion bit 0x40000 (7728). It is refused for one's own line, for a line with compensation in another zone, and without the item.
- The merchant window's Sell waits for the answer to the last sale ("Whoa!", 12063).
- Sales and buys give or take the coin locally with sound 0x8a, as EQEmu sends no coin update.
- Quantities: a sale or compensation item at most 20, a count 0..999, a price 1..1,000,000 per coin button (capped at 1,000,000,000 copper).

### Mail (UCS; `net/chat_server.h`, `GameConnection` in `net/game_connection_mail.cpp`, rules in `game/mail.{h,cpp}`)

Tried live between two clients: send, new-mail notice, mailbox switch, headers, a body, delete.

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

- A mail packet with more than 64 strings is dropped (Hailstone's limit; the longest stock layout, MailHeader, has 8), so a packet of NULs cannot become one string per byte.
- Delete in the inbox sends TRASH, in the trash DELETED. Purge sends DELETED for the trash.
- The composition window's From is the current mailbox's address as MailLogin listed it.
- The composition window opens and sends only with the membership feature 6.

### Marketplace (`net/marketplace_packets.h`, `GameConnection` in `net/game_connection_marketplace.cpp`, rules in `game/marketplace.{h,cpp}`)

The in-game store (CMarketplaceWnd). Unit-tested and mock-server tested; not tried live.

- Stock EQEmu has none of these opcodes (not in `patch_RoF2.conf`). A server that sells through the window answers the original client's opcodes, so it serves both this client and an unmodified RoF2 client.
- Layouts are the original's readers and senders (NMS-Release-Extras' `offsets/Market.h` section 44, HandleWorldMessage's cases).
- The funding pages (Add Funds, credit cards) used the publisher's web service and are not offered.

A catalog counting more than 10,000 entries is refused (Hailstone's limit; docs/unknowns.md). The feeds are read as the original's `CUnSerializeBuffer` does, each field bounds-checked on its own, so a short packet leaves the missing fields at their defaults. Catalog strings are NUL-terminated and cut to the node's buffer (name 127, the 64-byte one 63, the rest 1023 characters) while the cursor skips the whole string. The state feed's string is a u32 length and the bytes.

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| Hello | C→Z | 0x5bd5 | 76 B: zeros but u32 +0x48 = 1; at the game UI's build, the first frame after every open and every 30 s while shown |
| RequestItems | C→Z | 0x5148 | no body: at the game UI's build (the first zone-in after character select) while the catalog is empty; the catalog is kept all session |
| RequestFunding | C→Z | 0x76c3 | no body: at the game UI's build while the funding list is empty |
| Ready | C→Z | 0x356f | no body: at the game UI's build, at every show, and after a catalog arrives while the window is shown. The server answers with the wallet and the states |
| Items | Z→C | 0x1e9f | a full replace. u32 count, then each entry (note 1) |
| Funding | Z→C | 0x09bf | a full replace. u32 count, then {u32 amount, u32, cstr64, cstr64} |
| States | Z→C | 0x77a7 | u32 count, then {u32 entry id, u32 state, u32 item id (the preview's and Inspect's), u32, u32 length + the Player Studio maker ("Made By: …"; when set the row is also filed under "Player Studio/<its category>")} (note 2) |
| Wallet | Z→C | 0x4b7f | ≥ 84 B, fixed offsets: u32 +0x44 status (0), u32 +0x48 balance, u8 +0x4c (1: no zip-code page before the confirmation), u32 +0x50 type (1 the marketplace's; 2 the Krono count, not kept) |
| Purchase | C→Z | 0x4a74 | 80 B: zeros, u32 +0x48 entry id, u32 +0x4c quantity. The window then waits on its results page; 60 s with no answer gives eqstr 7626 |
| ZipCode | C→Z | 0x3289 | 80 B: the zip code (at most 5 digits) at +0, zeros; from the zip page, shown before the confirmation while the wallet's +0x4c is 0 |
| ZipOnFile | Z→C | 0x3171 | the byte +0x48 ORed into the wallet's zip flag |
| PurchaseResult | Z→C | 0x0a37 | fixed offsets (note 3). An OK dialog shows the text and the window comes up |
| Unavailable | Z→C | 0x0114 | no body: eqstr 7631 in chat and the window closes |
| ItemPreviewRequest | C→Z | 0x7f80 | 80 B as the object preview's, but u16 +0x4c = 1: the reply comes on 0xb07f (EQEmu answers every preview on 0x7f80, which this client shows in the object preview window) |
| MarketplacePreview | Z→C | 0xb07f | OP_ItemPreviewRequest's reply for the window: the serialized item and one trailing byte |

1. Items entry: u32 id, u32 price, u32 icon (items.icon), u32 max quantity (0 none; the client stops at 10), name, category path ("A/B/C"), description (STML), cstr64, u8, u8 new, cstr, cstr (the purchase's success text), u8 item of the week, u32 its rank (highest wins), u32 kind (0 in-game; 3 a quest-reward gate; 4, 6 Station Cash), u32, 4 × cstr, u32, u32, u8 featured, u32 was-price (struck through when above the price), u32 available for (seconds, 0 always), u32 on sale for (seconds), u32, u32 money type (0 an integer price, 1 "%1.2f" of price × 0.01, else "N/A"), u32, u8.
2. States: ignored while a catalog that came with the window hidden waits for the next show. Every row starts in state 2, which keeps it out of the category counts (and so out of the tree); Buy needs state 0. Only rows of an open window take it: answer every Ready.
3. PurchaseResult: u32 +0x40 entry id, u8 +0xc8 success, u32 +0xd8 (set: eqstr 7621, else 5361), u32 +0xdc new balance, u32 +0xe0 where the item went (1 inventory, 2 bank, 4 shared bank), u32 +0xec failure code (1, 4 → 7622; 2 → 7623; 3 → 7624; 6 → 7632; 8 → 714; else 7626), cstr +0xf0, cstr +0x4f0 (appended to the success text).

Opcodes the original sends that this client does not (their meaning is unknown or belongs to the publisher's services): 0x2a44, 0x3e15, 0x14c3, 0x6c8b, 0x7679, 0x54e6.

Opcodes it accepts that this client ignores: 0x1eba (a funding preview's debug line), 0x3036, and the eight the original also ignores (0x1b5d, 0x1f6e, 0x20d9, 0x3042, 0x4ffc, 0x6e2a, 0x740d, 0x7aa9).

### Personal tribute (`net/tribute_packets.h`, `GameConnection` in `net/game_connection_tribute.cpp`, windows in `app/lua_ui_progress.cpp`)

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

#### Tribute: bonus items

The bonus items arrive through `OP_ItemPacket`: type 108 for personal tribute (inventory type 6, slots 0..4), 109 for guild tribute (inventory type 8, slots 0..1), and feed local stats and effects. At login they precede `OP_CharInventory`, which keeps them unless it supplies replacements.

The server does not announce their deletion on deactivation or opt-out, so the client retires them from the tribute settings, guild active benefits and own opt state: a changed selection retires its slot, and guild membership and session changes retire the previous context's items.

#### Tribute: guild permissions

- Guild benefit editing and activation need guild permission 23 (checked in the UI and before sending); another member's opt-out needs 22; self opt-in/out is always available. Revocation discards unsaved edits.

The server can send the next guild's permission rows, rank names and active tribute snapshot before the own GuildID appearance; they stay staged until that appearance confirms the guild.

- The active packet's leading DWORD is zero (`SendGuildActiveTributes`) or the guild id (`GuildTributeToggleReply`); an untagged snapshot uses the received rank-table context.
- A future guild's state cannot authorize changes to the current guild's benefits, and joining another guild clears the old rank until the new GuildRank appearance arrives.

Confirmed rank permissions and names survive a zone change (EQEmu resends them, `SendGuildRanks` / `SendGuildRankNames`) and are cleared at session start, disconnect or guild change. An unconfirmed staged table is dropped at a zone change.

#### Tribute: guild platinum donations

One guild platinum donation is outstanding at a time. As in the original:

- it is sent only while carried platinum covers the amount (other coins do not count);
- the master window's Donate button is greyed until the reply;
- the 12-byte reply carries quantity and favour;
- a quantity of -1 clears the request without a debit.

Checked: `Send_OP_GuildTributeDonatePlat` @0x483e80, `CTributeMasterWnd` +0x2d8, `UpdateDonateButton` @0x785f40, @0x483cd0.

Two deliberate changes for safety:

1. The original clears the request on any reply; here only a matching quantity debits the purse and clears it (the reply names no transaction). No coin is removed before the answer.
2. The original subtracts carried platinum only; EQEmu (`TakeMoneyFromPP`) takes copper first and makes change silently, and the client's debit follows the server's.

Session reset or a later guild-master open reply retires an unanswered request; there is no local timeout. Tests: decoder fuzz and mock tests.

### Mercenaries (`game/mercenary.{h,cpp}`, `GameConnection` in `net/game_connection_merc.cpp`, windows in `app/lua_ui_merc.cpp`)

Checked against 25 recorded vector files (`tests/merc_vectors_test.cpp`). Not tried live (the test server has `Mercs:AllowMercs` off).

- EQEmu's `patch_RoF2.conf` names only fourteen of the opcodes: the first thirteen and the switch 0x1b37 (in `net/opcodes_rof2.inc`). The others are constants in `game/mercenary.h` (`kOpMerc*`).
- On zone-in the client sends 0x31e4 and 0x4820 {8}.

#### Mercenaries: the record

A mercenary record (merchant entries and owned mercenaries) on the wire, each field read only when it fits:

1. u32 id;
2. u32 type (dbstr 21), u32 subtype (dbstr 22 tier, 23 description);
3. u32 purchase cost, u32 upkeep cost (gold; shown as cost / 10 platinum and cost % 10 gold);
4. u32 membership (0 free, 1 silver, 2 gold);
5. u32 alt purchase, u32 alt upkeep, u32 alt currency id;
6. u8, u32 time left (ms), u32 command (echoed in OP_MercenaryCommand), u32 stance (dbstr 24);
7. u32 stance count, u32, u8 auto-assist, cstr name (63 kept);
8. stance count × {u32 index, u32 stance}.

The record holds 10 pairs; this client skips any further ones (stance count kept as sent).

| Packet | Dir | Opcode | Layout |
|---|---|---|---|
| MercenaryDataRequest | C→Z | 0x11c1 | u32 liaison spawn id; sent when the merchant window shows (a right click on a class-71 NPC within 20 opens it) |
| MercenaryDataResponse | Z→C | 0x72ce | u32 type count, u32 types, u32 entry count, entries (note 1) |
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
| MercenaryTimer | Z→C | 0x0763 | 20 B: +0 has a mercenary (**only the low byte is read**), +4 upkeep interval (ms), +8, +0xc state, +0x10 restoration timer (note 2) |
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

1. MercenaryDataResponse: no types gives "You do not meet the requirements to purchase any mercenaries." (with a liaison open) and hides the merchant window. A leftover length logs "ERROR: Corrupt MSG_MERCENARIES.". Event 0xd fills the window. Type lists and stance pairs stop where the bytes do.
2. MercenaryTimer: EQEmu puts the entity id at +0, so an id with low byte 0 reads as none. A data update request goes out when the flag goes 0 → set, or an active state meets a packet state other than 5. A set flag, a clear packet flag and state 5 dismiss locally.

Not built: the ownership transfer offer and its answer (0x5df8 from the server, 0x4333) and the upgrade prompt (0x69ca, 0x6e9f).

### Real estate: housing, guild halls, placed objects (`net/real_estate_packets.h`, `GameConnection` in `net/game_connection_real_estate.cpp`, state in `game/real_estate.{h,cpp}`)

None of these opcodes are in EQEmu's RoF2 table; a server with housing (such as Project Triune) sends them with the original's numbers.

Layouts come from NMS-Release-Extras' research headers (`Housing.h`, `HousingNpc.h`; THIRD_PARTY.md), checked against the original client's code, which wins where they disagree (corrections below).

- Records use u32-length strings with no NUL; placed objects, escrow and the fixed bodies use NUL-terminated strings.
- The original reads fail-open; Hailstone refuses a short packet.

| Op | Dir | Payload | Effect |
|---|---|---|---|
| 0x2698 | Z→C | u32 n definitions {u32 id, f01 (the area id that links an estate to an area), price group, upkeep group, capacity, vault, f06..f08, type, category; str name; f11, pet capacity, f13..f16; u32 m × u32}; u32 k cost sets {u32 id, u32 e × {u32 key, u32 l × {u32 currency, u32 sub, u64 amount}}} | the catalog, replaced |
| 0x63fd / 0x6fd0 / 0x4f09 | Z→C | one record: u32 id, f18, parent, definition, guild (-1), f28; str address; str owner; u32 f34; u32 child count; str name; u32 children[count]; u8 n × {u32 currency, f64 balance}; u32 upkeep set, f40; u8 opt out | add / set current (the root) and add / remove |
| 0x3968 / 0x485d | Z→C | u32 re; 7 × {u32 group, u32 bits}; u32 n × {str name, u32 group}; 0x3968 then an optional u32 | an estate's access table |
| 0x087f / 0x1cef / 0x2cf7 | Z→C | u32 re; [0x087f: str name]; u32 n (0x2cf7: one) × item row {u32 re, u32 item, u8 placed, u32, 7 × u32 counters, str, str name, the item as in OP_ItemPacket after its type} | an estate's item rows |
| 0x1967 / 0x1baf / 0x7d28 | Z→C | {u32 re, cstr name} / {u32 re, u32 item} / u32 re | rename / an item gone / forget the rows |
| 0x1228 / 0x66bf / 0x68ba | Z→C | i32 code | result / sell / transfer: the code's eqstr in colour 13 (an unlisted 0x1228 code prints 5223; 0 and -1 print nothing) |
| 0x2691 | both | see note 1 | searches and their answers |
| 0x45be | both | C→Z the 0xbc-byte body {re, …, +0xac price key, +0xb0 upkeep key}; Z→C u8 | buy; a non-zero answer shows the manage window |
| 0x792c / 0x507f | C→Z | the 0xbc-byte body: {re, -1, -1} / {re, the target's name at +0x6c} | sell, transfer |
| 0x2aa6 | both | u32 action, re, f8; cstr address (≤ 63), cstr owner (≤ 31); u8; u32; u32 n × {u32 currency, u32 coin type (3 platinum .. 0 copper), u64 amount} | escrow (note 2) |
| 0x2aa7 / 0x3e00 / 0x66df, 0x76c6 / 0x0c13 | Z→C | u32 n × u32 / {u32 id, the byte at +0x68} | the character's owned-estate lists A, B, C, replaced; A and B toggled |
| 0x5a63 | both | the 0xbc-byte body (+0x0c the giver) | "Do you wish to accept %1 from %2?" (7636, 5 min); the answer echoes it with +0xb9 = yes (a no also prints 7638) |
| 0x1045 / 0x2a9e | Z→C | EQPlacedItem {cstr tag, u8 NPC item, u32, u32, cstr key, u32 re, u32 item, f32 scale, heading, pitch, roll, x, y, z, u8 no collision, u8 wall only, u8 no free mode, u32 wall allowed, 7 × u32} / ≥ 0x28 B with re at +0x20, item at +0x24 | a placed object added or updated / removed |
| 0x21ea | Z→C | u32 spawn id, cstr guid at +4 (16 characters), u32 estate at +0x18, u32 item at +0x1c | marks a spawn as a placed NPC item (a merchant, a pet); a right click on it within 60, or with its estate the current one, asks for the item's menu (0x21ec) |
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

1. 0x2691:
   - C→Z 224 B: u32 subtype (0 my plots, 1 my items, 2 a neighborhood row, 4 the plot search), u32 id, u32 0, char[80], char[64] (the plot search's name), char[64], u8 flag, u8 1, 2 B.
   - Z→C: u32, u8 owner type. Not 4 / 8: i32 target (4 / 5 plot list; 0 / 1 neighborhoods; 2 members; 3 nothing). 4 / 8: one record (a known id only gets its owner and guild).
2. 0x2aa6, escrow:
   - C→Z: 0 deposit the cursor's coin, 1 withdraw one denomination, 3 Add Day, 4 Add Week, 7 Withdraw All (23 B with no entry, 39 with one).
   - Z→C: with the u8 clear and action 0, or set and action 1, the first coin entry becomes the cursor's coin of its type.

Occupancy, once a second (`RealEstateManagerClient_Process`):
1. The current estate is the first area holding the player whose estate (a record whose definition's f01 is the area's id, at or under the root) is a plot or guild plot. Without one it is the root.
2. The house is the current estate if it is a house, else a house among a plot's children.
3. The yard is the nearest plot or guild plot at or above the current estate.
4. The crate is the first crate below the yard.

Buy, sell and transfer checks print the original's refusals before sending; the server decides.

Where the client's code corrects the headers:
- Escrow is one serialized object in both directions, not a fixed struct.
- The server's escrow reply sets the *cursor's* coin, not the purse.
- The house is found from the current estate, before the yard.
- f01 is the area link.
- Sell code 4 is 0x149c and code 1 falls through to the default.
- Unlisted 0x1228 codes print 5223.
- The search request is 224 bytes.

#### Real estate: placement mode

Placement mode (`game/placement.{h,cpp}`; modes collision, wall, cursor, free, as 0x5ae160) checks the item's box against the areas in the reply's code map:

- an area holding all 8 corners with code 0 allows it;
- otherwise the last such area's code, otherwise the root estate's code, or 1;
- a player inside the box gives 0x1c.

A 0x1228 {0} or 0x6213 ends it, and so does a zone change. Escape cancelling it is Hailstone's own.

Not built: the ghost's fit to walls and surfaces (ItemPlacement_UpdatePosition), free mode's mouse moves, the binding of a placement session to the 0x21ea NPC item whose guid it is, the polls, `/outputfile realestate`'s rows.

### Screen text and the camera effect (`net/notice_packets.h`, `game/text_overlay.h`, `game/camera_shake.h`)

OP_Marquee (0x502e, zone → client; EQEmu's `ClientMarqueeMessage_Struct`): six u32 and the text, NUL-terminated, at
byte 24. The case @0x4cb7f7 hands the first three to `CTextOverlay::DisplayText` as the colour, the priority and the
alpha (EQEmu names those fields type, unk04 and priority), then the fade-in, fade-out and hold times in ms. A debug log
is the only other effect: no chat line. The text takes the overlay's one line, as OP_TaskActivityComplete's lines do
(the priority is signed: a lower one is ignored while another line shows). Stock EQEmu (`Client::SendMarqueeMessage`)
sends priority 10 and, unless a quest gives another, alpha 510: the overlay keeps only the alpha's low byte, so a fade
past 255 wraps (what EQEmu's notes call the extra blinks). EQEmu has no RoF2 encoder for either packet: the bytes go
out as built. The original reads the struct without a size check; a packet too short for the six words or without the
text's NUL is dropped here.

OP_CameraEffect (0x127f, zone → client; 8 bytes): u32 duration in ms, f32 intensity. The case @0x4c6f73 sets the
camera effect (`game/camera_shake.h`): a stronger intensity replaces the current one (an equal one too, and only a
longer duration moves the end), a weaker one or a NaN is ignored. Each frame adds a random offset of up to half the
intensity each way to the drawn camera's position; the duration runs out, the clock going backwards wipes it, and a
zone-in resets it (the original's zone-in path @0x4939b0). The player itself never moves.

The original adds the offset in the per-frame camera driver @0x796dd0 and hands the shaken position to its render
camera; when that camera refuses the position (its vtable slot 0x38 returns nonzero) it falls back to the unshaken one.
Here the shaken position is only drawn: picking and line of sight keep the true camera, and no fallback is made
(docs/unknowns.md).

### Spawn (ENCODE(OP_ZoneSpawns))

The temporary-pet byte is kept as `game::Spawn::temp_pet` (nonzero = true). Combat chat uses it with the owner to recognise the player's temporary pets, which do not take the permanent pet's window focus. Stock EQEmu writes it as zero and swarm spawns omit petOwnerId, so the record alone cannot establish swarm ownership (see the melee-line rules in docs/mechanics.md).

The record, in order:

1. cstr name; u32 spawn id; u8 level; f32 melee radius (size − 0.7); u8 type (0 PC, 1 NPC, 2/3 corpse).
2. u32 bitfield: gender:2, pet, afk, anon:2, gm, sneak, lfg, betabuffed, invis:12 at 10, linkdead 22, showhelm 23, unknown 24, trader 25, animation-on-pop 26, targetable 27, targetable-with-hotkey 28, showname 29, idle-off 30, click-through 31.
3. u8 OtherData: bit0 buyer, bit1 offline mode (EQEmu never sets it), bit2 LDoN chest/interactive, bit4 title follows, bit5 suffix follows; destructibles 0xe1.
4. f32 −1; u32 0.
5. [interactive: 3 cstr, u32 appearance, u32 animation, 10×u32 sounds, u8 collidable, u32 type].
6. u8 property count + count×u32 (EQEmu: 1, body type; 0 for destructibles).
7. u8 HP %; 6×u8 hair colour, beard colour, eye 1, eye 2, hair style, beard; 3×u32 drakkin heritage, tattoo, details.
8. u8 texture type (equip_chest2), u8 0, u8 0, u8 helm; f32 size; u8 face; f32 walk; f32 run; u32 race; u8 holding; u32 deity.
9. u32 guild id, u32 guild rank (NPC: 0xffffffff, 0).
10. u8 class, pvp, stand state, light, fly mode; cstr last name; u32 AA title; u8 guild show; u8 temp pet; u32 pet owner; u8 FindBits.
11. u32 player state, npc tint, primary tint, secondary tint; 2×u32 0xffffffff.
12. Equipment: PC or PC-model race (≤12, 128, 130, 330, 522) → 9×u32 tints + 9×{5 u32 texture}, else 3×{5 u32} (slot 0 zero, primary, secondary).
13. Position, 5 dwords: [0] y:19 at 12; [1] Δx:13 at 0, Δheading:10 at 13; [2] Δy:13 at 0, z:19 at 13; [3] x:19 at 0, animation:10 at 19; [4] heading:12 at 0, Δz:13 at 12.
14. [title cstr]; [suffix cstr].
15. 2×u32; u8 mercenary; cstr "0000000000000000"; 2×u32 0xffffffff.
16. 29 zero bytes, which the client reads as u8 always-show-aura, u32 physics effect count (+16 B each), 6×u32 spawn status.

### PlayerProfile (ENCODE(OP_PlayerProfile))

Every array is read by its own count prefix (20 bandoliers × 4 items is the one fixed count). In order:

1. u32 checksum (eq_crc32 of bytes [8, size−1)), u32 size−9, 2×u32.
2. u8 gender, u32 race, u8 class, u8 level ×2.
3. u32 5 + 5 binds {u32 zone, f32 x, y, z, heading}; u32 deity, u32 intoxication.
4. u32 10 + 10×u32; u32 22 + 22×{5 u32} equipment; u32 9 + 9×{5 u32}; u32 9 + 9 tints; u32 9 + 9 tints.
5. u8 hair colour, beard colour, u32, eye 1, eye 2, hair style, beard, face; 3×u32 drakkin; 5 B.
6. f32 height, 3 f32, 2 u32; u32 points, mana, current HP; STR, STA, CHA, DEX, INT, AGI, WIS; 7 u32.
7. Counted arrays: AAs (12 B each), skills, innate skills, disciplines, timestamps, recast timers, timestamps 2, spell book, memorised gems, gem refresh; u8.
8. Counted buffs (80 B each: u8, f32 instrument mod, u32 caster, u8, u32 counters, u32 duration, u8 level, u32 spell, u8 effect type, u32 hits, u32, u32 counters, 44 B).
9. Coin ×4, cursor coin ×4, 3 u32, thirst, hunger, AA spent, u32 5 + assigned + 5 tab values (last = unspent), 2 B.
10. Counted bandoliers (cstr + 4×{cstr, u32 id, i32 icon}, kept as `PlayerProfile::bandoliers`); counted potion belt ({cstr, u32, i32}, `potion_belt`).
11. 4 i32; 12 u32 (resists etc.); u32 20; 4 u32; endurance; 2 u32.
12. u32 64 + `char[64]` name; u32 32 + `char[32]` last name.
13. Birthday, account start, last login, minutes played, entitled, expansions; counted languages (bytes).
14. u16 zone, u16 instance, f32 y, x, z, heading; u8, pvp, u8, gm; u32 guild, u8 rank, u32, u8, u32; u64 exp; u8.
15. Bank coin ×4, shared platinum, … `character_id` after 663 unknown bytes; … AA exp; ….
16. u8 show helm; u32 remaining rest cooldown in seconds; counted 1024 B; 2 u32; u8.

### Guild banners

These are zone-session application messages.

#### Banners: senders and handlers

The Guild window (CGuildMgmtWnd) sends them from:

- RequestBannerKitList (`0x684ad0`: the kit list request, then always the status request);
- `0x680a40` (the status request alone);
- SaveBannerSettings (`0x6833a0`);
- `0x683870` (remove);
- Send_PlantGuildBanner (`0x54e340`).

Replies `0x0001`, `0x56c9` and `0x1e50` are handled only while the window is open; `0x4683` always. Checked: HandleWorldMessage `0x4c3250` → HandleBannerKitList `0x6836c0`, `0x685670`, `0x6830c0`, `0x4d1aca`.

The requests carry the guild ID and the character ID (EQEmu's `char_id` in the profile, after the 343-byte block). Checked: PcBase +0x20a8, `0x57ead6`, `0x579c71`.

The window's actions need guild permission 1 Change (`0x6833a0`), 2 Plant or 3 Remove. The page also checks bit `0x1000` of what the client reads as the profile's expansion mask (The Buried Sea; docs/unknowns.md). Checked: UpdateBannerPage `0x681b6e`, PcBase virtual +0x5c.

| Message | Direction | Wire | Payload |
| --- | --- | --- | --- |
| Open guild window | C→Z | `0x0276` | 8 B. Guild ID, character ID |
| Request kits | C→Z | `0x0d92` | 141 B. Character ID, u8 owned-only, 32 u32 slots for unique possessed type-58 item IDs, u32 count, u32 category mask. All categories uses `0xffffffff`, otherwise one bit per category ID |
| Kit list | Z→C | `0x0001` | 16 B header with count at +4 and the background and emblem counts at +8/+12. Rows of 137 B with kit ID, 128 B name, u8 availability and u32 description ID; the first 1023 are kept (the original's limit); trailing bytes are ignored |
| Request banner status | C→Z | `0x4924` | 80 B. Character ID, guild ID, 72 reserved bytes |
| Banner status | Z→C | `0x56c9` | 188 B (anything after is ignored). Mode +4, active kit ID +12, description ID +16, signed ms remaining +20, name +24, planted kit ID +168, two u32 pattern IDs and two RGB values in four-byte slots starting +172 (note 1) |
| Modification token | Z→C | `0x1e50` | Item ID +8 and item name +16 (up to 63 characters) (note 2) |
| Save design | C→Z | `0x4377` | 24 B. Character ID, background and emblem pattern IDs, two RGB fields each with one padding byte, server-supplied modification token ID |
| Plant or reapply style | C→Z | `0x765b` | 87 B. Kit ID +0, native actor position Y/X/Z +68/+72/+76, u8 list-row flag +80, u8 reapply flag +81. Saving reapplies an active kit using a zero vector when the native floor-distance check passes |
| Remove | C→Z | `0x9e18` | 12 B. `0xffffffff`, guild ID, reserved u32 |
| Update planted actor | Z→C | `0x4683` | Spawn ID +8, two pattern indices +12/+16, two tints +20/+24, NUL-terminated spawn name +28 (at most 63 bytes). Applies to an existing NPC |

1. Banner status: kit ID `0xffffffff` means absent. Mode 1 is ignored. Any other mode is stored and then asks again (`0x684ad0`, both requests), except mode 2 with the design editor closed (`0x685670`).
2. Modification token: ID `0xffffffff` names the requirement when the item is unavailable. Save is refused only for that ID (`0x6833a0`), with "You must have a: <item> to save change to banner settings."

#### Banners: other details

- The client zeroes reserved bytes in its requests. Kit and status replies apply to the requesting guild window.
- `OP_NewZone` +912 is the zone's banner placement policy (0 disallows placement, 1 permits placement, 2 permits placement and portal travel).
- Interactive type-4 spawns carry the two pattern indices and two tints after the usual status tail (`0x591e90`). The cloth uses both native texture layers (`0x40af50`).
- Stock EQEmu sends none of the banner manager replies.

### Fellowships

`net/fellowship_packets.*` handles the native fellowship messages. Stock EQEmu does not send them; EQEmu PR #5134 "TOB: Fellowships" (open, unmerged, GPL-3, read only as a protocol reference) does. Servers without fellowships keep an empty state.

#### Fellowships: requests

Requests use opcode `0x7121` with a 1076-byte body. Unused fields are zero.

| Offset | Field |
|---|---|
| 0 | action u32 |
| 4 | target u32 |
| 20 | NUL-terminated MOTD in a 1024-byte field |
| 1064 | camp type u32 |

Actions: create 1, MOTD 2, accept 3, invite target spawn 5, remove character 6, promote character 7, create camp 8, destroy camp 9, leave 10, disband 11, toggle sharing for character 12.

Requests never change membership locally; the server validates everything.

#### Fellowships: invitations

The invitation opcode is `0x62ab`, 0x434 bytes; only the inviter spawn at 12 is read, and no dialog shows when that spawn is not found. The dialog expires after 100 seconds. Acceptance sends action 3, declining action 4, with no target (PR #5134 reads 4 as a legacy camp destruction; the client still sends what RoF2 sends). Checked: `Handle_Opcode_0x62ab` @0x667db0, `Send_Opcode_0x7121` @0x668710.

**Extension beyond RoF2:** a 1080-byte invitation (PR #5134's form: action 5 at 0, inviter at 4) is accepted too, inviter from 12 when set, else 4. If that spawn is not in the zone the dialog shows without a name (such a server may invite across zones). The 0x434-byte form never does either.

#### Fellowships: roster

Roster opcode `0x6e4d`, version 5, has a 1100-byte header: version, fellowship id, name[64], MOTD[1024], count.

- Each of the twelve native member records is 84 bytes: character id u32, name[64], zone u32, level u32, class u32, last-seen epoch u32. The first record is leader.
- Records are followed by a u32, twelve 32-byte player handles, and three twelve-byte arrays (sharing, capped, offline).
- A 32-byte campsite trailer contains y/x/z floats, zone, expiry, a second expiry, an unused word and active, all remaining fields u32.
- Full native size is 2564 bytes; the 2532-byte roster without camp is also accepted.

Membership:

- A matching roster that omits this character clears membership; an unrelated roster cannot replace or clear it.
- Zone changes keep membership but discard pending invitations and kit queries; a session end clears it.

**Extension beyond RoF2 (capacity):** all member arrays may instead have 13 through 64 slots.

- Capacity is inferred from exact length: `1104 + 119 * slots` without camp, `1136 + 119 * slots` with camp.
- Count cannot exceed capacity. Duplicate identities, unknown versions, malformed lengths and more than 64 slots are rejected. There is no client-side 12-member cap.
- A server must expand every array this way (PR #5134 hard-caps at twelve).
- Unit-tested only; not tried against a live server.

#### Fellowships: camps

Camp refresh `0x0c12` has two u32s: available-only and category mask (all bits for all categories). Reply `0x074d` is count u32 followed by available u8, camp type u32, description id u32 and a NUL-terminated display name.

- The description id is a DBString type-0 lookup (as in the original); a missing one falls back to the kit name.
- Category labels come from the player's layered `Resources/campsitecategories.txt` (`id^label`); selecting a category sends `1 << id`. Without the file only the all-category query is offered.

#### Fellowships: vitality and chat

Vitality `0x2cc6` is 28 bytes: normal u64, normal cap u64, AA u32, unused u32, AA cap u32.

- The profile seeds normal/AA vitality from the sixteen bytes after AA experience (last fellowship join u32, normal u64, AA u32). The dedicated message updates values and caps.
- Native gauges 147/148 display `min(pool, cap) * 1000 / cap`, truncated (computed without 64-bit overflow); zero caps draw empty. Lua gets exact amount strings.
- No XP or vitality arithmetic is done locally.

Fellowship chat uses channel 21 and colour 342.

## 5. Notes

- **Item ids above 0xFFFFF**: ids that large make item links carry 8-digit ids (`SAY_LINK_BODY_SIZE` 77 in `rof2_limits.h`). `game/item_links.cpp` reads both widths.

## 6. Uncertain

- A second `OP_ZoneChange` success answer while already zoning is ignored (one live run got two and opened a second world session). Its source is not confirmed; a second request from an earlier 7 s timeout is the likely one.
- Destructible spawns (OtherData 0xe1): EQEmu writes the interactive block although bit 2 is clear, and sets bit 5 (suffix) without writing a suffix. The decoder treats `(other & 0xe0) == 0xe0` as interactive and suffix-less. Untested against live data.
- OpenEQ's spawn decoder unpacks the spawn position with the PlayerPositionUpdateServer bit layout. We follow rof2_structs.h `Spawn_Struct_Position`, which is what the server writes. Check against a live capture.
- Client `OP_ClientUpdate` Δheading scale (×20) is taken from the server's `EQ10toFloat`, used only for boats; the value is not verified against a RoF2 client. Animation is passed raw.
- `ChannelMessage` from the client: we send the server's ENCODE layout; a RoF2 client's trailing bytes are not verified (the server ignores them).
- LoginInfo +192 = 0xcc may be a dword. The server ignores it.
