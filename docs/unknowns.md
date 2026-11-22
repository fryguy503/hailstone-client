# Known gaps

What the client does not implement yet, or implements with an approximation it cannot confirm. Entries are removed once settled.

Each entry starts with the gap's name in bold. Its sub-bullets use these labels where they apply:

- Unknown: what is not confirmed.
- Now: what the client does meanwhile.
- Settle: what would settle it.

Short entries stay on one line.

## Spirit Shrouds and Monster Missions

- **Live acceptance.** Needs a server with Spirit Shrouds, Monster Mission roles and Shroud Bank storage enabled.
  - Check a real mission through role selection, zoning, rejoining, failure and success.
  - Confirm the normal profile and possessions arrive before the mission's rewards.
  - Confirm the shroudkeeper return path, and inspecting and withdrawing stored items near the banker in both interfaces.
- **Profile type zero in the EQEmu draft.** The EQEmu server draft leaves the profile type zero in some snapshots.
  - Now: Hailstone uses the accepted catalog context for local transitions, and recognizes an alternate class or race
    from the character list on relog.
  - A same-class, same-race form needs a populated profile type to be identifiable after reconnecting.
- **Shroud Bank access.** The RoF2 bank's Shroud Bank button sends `OP_AltStorage` action 0 to the current banker; the draft server accepts it only after it has authorized that NPC.
  - The shroudkeeper authorizes and opens storage in one interaction; ordinary bankers need server-side authorization
    for first-time access in the draft.
- **Draft decoder bugs.**
  - The draft server's Spirit Shroud request decoders count two extra bytes (the application opcode stock RoF2 sends); a server using the draft must fix them before form requests work.
  - It must also resend the role offer when a player rejoins a mission before selecting a role.
- **`OP_ShroudRemove` (0x67e8).**
  - Unknown: the RoF2 handler only shows eqstr 5191 (wait for the transformation) in a 45-second dialog, not which way the form changes.
  - Now: no dialog. The next `OP_Shroud` is treated as the return to the original form, as the EQEmu draft uses it.
  - Settle: a capture from a server with shrouds, or a trace of what the original does with the profile type on that `OP_Shroud`.
- **Draft-only paths.** RoF2 does none of these (0x414320 keeps the field as the shroudkeeper's id):
  - a `ShroudSelectionWindow` whose spawn field names the active shared task opens the forms as Monster Mission roles;
  - a roster template id of 16 or less without a catalog is shown as a class name;
  - a character-list `ShroudClass`/`ShroudRace` unlike the base class/race marks the character as shrouded when the
    profile type is 0.
  - Stock EQEmu sends ShroudClass/ShroudRace equal to the base values and no shroud packets, so none fires there.

## Legacy environmental particles

- **Legacy particle switches** (`app/switch_particles.*`, `assets/legacy_particles.*`).
  - Explicit-position stage-2 slots are drawn from the player's old `spells.eff`; actor-dependent old selectors are skipped.
  - Native day/night switch mode 2 also activates with `CDisplay::bRainScreen` (`0x488030`); that is not connected, only its hour test is applied.
  - Closing a switch removes its particles at once, not with the native one-second shutdown fade.
  - Waterfall seen live (Stonebrunt). Open: the unsupported selectors, the rain-screen override, the shutdown tail, the slot level gates (not tried with a low-level character).

## Server add-on policy

- **Required/allow/block policy.** Unit-tested, not live.
  - Live acceptance needs: a first and a return visit; a server fixing its policy; installing missing
    requirements; native consent allowed/refused/closed; a required add-on failing in the world (the chat warning);
    both UI packs and a physical controller; an external upscaler allowed/blocked/updated, then restart.
  - The server does not send its policy until the zone-in.
  - It is a client extension, not a server-side attestation or anti-cheat mechanism.
- **Server add-ons warning and its Retry.** Unit-tested, not live. Not tried: a required plugin that throws and is
  retried on a live server; a required script that fails and is retried; both packs with a physical controller; the
  client's own window (no test drives it).
- **Minimum versions on required add-ons.** Unit-tested, not live. Not tried: an exact, older and newer installed
  version at the zone-in; an older client refused by a server using the versioned form.
- **Texture packs and required tools.**
  - Texture packs (Texture Studio's output in `textures/`) are not add-ons and have no repository, so no list names them. A server can block the Texture Studio tool, not packs already built.
  - A required tool is met by its installation: the client does not start it and cannot tell whether it runs.

## Server map ownership

- **Ownership and NPC Find suppression.** Synthetic tests only; live tracking-class/GM changes, zoning, retirement and
  real server replies still need acceptance in both UI packs. Find replies are matched to requests in order, as
  without a map policy (the Navigation row below).
- **Tracking on the player's own death.**
  - Now: the client empties the tracking list and closes the window when the player dies (as for Find paths and walks); it can be opened again once alive.
  - Unknown: what RoF2's CTrackingWnd does on death. A death usually zones the player anyway, which empties the list in either case.
  - Settle: a capture of a death with the window open, then a same-zone respawn.

## Modular interfaces

- **World-nameplate canvas.** Synthetic tests cover parsing, painting, ownership and input. Actual GPU depth,
  dense scenes, sustained zoning and physical mouse/controller acceptance remain open. Add-ons own their layouts
  and must validate them in a live session.
- **Invisibility versus undead on another spawn** (Hailstone's optional `[Name]`).
  - Now: marked only while the spawn is the target and its OP_TargetBuffs list holds the effect.
  - EQEmu's spawn record and SpawnAppearance carry regular invisibility only, and the server decides whose buffs it sends; a server would have to send more to mark other spawns.
- **Native canvas providers.** Same caption visibility and physical input checks.
  - Open: live interoperability with a compiled nameplate add-on, and dense-scene performance.
  - Stock spawn appearance combines invisibility and Hide; exact invisibility types are known only from received self
    or current-target spell effects.

- **Shared `hs.UI` contract.**
  - The migrated Modern/Gamepad presentations need live acceptance of mouse, keyboard and controller workflows, zoning and interface changes.
  - Declared base packs inherit presentations; exposing a new data/action method does not design its screen.
  - Host feature discovery is separate from server-provided data and the caller's permissions.

## Evolving items

- **Live acceptance.** Checked against RoF2's handlers and EQEmu's serializer: Final Result (action 4), progress
  updates (action 0) and the transfer's actions (1 open, 2 details, 3 transfer). Not tried live: owned and linked item
  inspection, progress through an actual evolution, a transfer and refusal, replacement items across zoning, physical
  controller input.
- **Zero final-id hint.** Sent as received. A stock server that uses only that hint may not answer; servers that
  resolve the family from the source can return the final form.
- **Transfer details reply.**
  - Unknown: RoF2's reply names only the source and destination evolving IDs, with no request nonce, so an older reply for the same pair looks like a fresh one.
  - Now: Hailstone checks the current pair, the live original items and a local approval revision; the server recalculates on commit.
  - Settle: a server-provided nonce.
  - With no preview (EQEmu sends none when it cannot find either item), closing and reopening the window allows a
    fresh request. Hailstone never makes up compatibility or result items.
  - No success acknowledgment: a submitted transfer waits for inventory changes and server messages.

## Adventures (LDoN)

- **Live acceptance.** Checked against the stock RoF2 executable: request, stats and leaderboard opcodes/offsets,
  native ten-second refresh guards, twenty-unit inclusive recruiter range, minute-ceiling timers, tied ranking. Stock
  EQEmu provides the fixed incoming layouts. Untested: live request/offer/entry/completion/leave, cross-zone
  sync, actual server standings, physical controller.
- **Leaderboard category.**
  - Unknown: stock EQEmu zeroes the reply's category words. Requests are serialized, but after the thirty-second recovery timeout a very late reply cannot be tied to the selected category.
  - Settle: a server-provided request identifier.
  - Native disables selectors during its ten-second guard. Hailstone deliberately allows choosing a pending category
    and sends only the latest after the previous reply.
- **Delayed offers.** An offer carries no recruiter or request ID. After the eight-second wait expires and a
  new request is sent, a very late offer can be mistaken for the newer one.
- **Leave replies on other servers.**
  - EQEmu reports a granted leave or expiry with an AdventureInfo notice. Hailstone also clears the assignment
    on an empty AdventureData snapshot.
  - A server that uses a different reply may require a zone change to refresh the client. Settle with a capture of
    the server's leave response.
  - RoF2's adventure dispatcher (`0x420610`) also resets the adventure state (`AdventureMgr_SetState(0)`) on opcode
    0x0dcb, which EQEmu's patch_RoF2.conf does not map and no stock server sends; whether a live server answered a
    leave with it is not established.
- **AdventureData's risk.**
  - RoF2 sets the window's risk choice to risk - 1 (`0x618790`, the 1 normal / 2 high of its own request); stock EQEmu sends 0 normal / 1 high.
  - Now: nonzero shows as high. Right for EQEmu; a server sending RoF2's 1 for normal would show as high.
  - Settle: a capture from the original live servers.
- **Reserved words.**
  - Unknown: the native meaning of AdventureData's leading/trailing reserved words and leaderboard header +4.
  - Hailstone uses the briefing and timers to distinguish an assignment from a finished-only snapshot and ignores
    the leaderboard reserved word.
  - Some native actor interaction predicates beyond recruiter class, corpse status and range are not reproduced.

## Fellowships

- **No server yet.** Stock EQEmu does not send RoF2 fellowship packets, so nothing here has met a server.
  - Native requests and roster/vitality fields checked against the original handlers, and against EQEmu PR #5134
    "TOB: Fellowships" (open, unmerged) as a protocol reference.
  - Untested: live cross-zone sync, XP awards, camp gameplay.
- **64-member rosters.**
  - The 64-member array layout and the 1080-byte invitation are extensions beyond RoF2, documented in `docs/protocol/rof2-packets.md`.
  - PR #5134 hard-caps its model and serializers at twelve, so its membership rule alone cannot enable 64.
  - Settle: a server that sends expanded rosters, and a live roster capture above twelve.
- **Declining an invitation.**
  - Sends action 4, as RoF2 does (`Send_Opcode_0x7121` @0x668710). PR #5134 reads action 4 as a legacy camp
    destruction; that is the server's to fix, since RoF2 sends the same.
  - Unknown: a timed-out invitation sends nothing here; whether the original's dialog sends action 4 on its 100-second timeout is not traced.
  - Settle: the confirmation dialog's timeout path (dialog type 0xc).

## Social and friends

- **`/who friends`.**
  - Sends OP_FriendsWho built as Who_BuildAndSendRequest builds it (confirmed), and shows the server's ordinary who response.
  - Tested only against stock EQEmu's response as read from its source: online, anonymous and roleplaying friends shown, offline friends left out.
  - Settle: a live session against a stock server with friends in other zones, some going online and offline.

## Shared archive memory

- **`assets.shared_memory=auto`** (docs/layout.md "Shared archive memory").
  - Measured on Linux only, one and two clients in one zone (docs/research/shared-asset-memory.md). Windows and macOS
    mapping, locking and eviction are unit-tested only. Not measured: sustained play with zoning, more clients.
  - A cache file is checked once per client, then trusted: damage on disk while a client runs is not noticed by that
    client (the parsers still bound what they read).

## Shared GPU textures

The experimental texture broker (docs/graphics.md "Shared GPU textures", docs/research/shared-gpu-memory.md):

- **Reads after the lease ends.**
  - When sharing stops, a client copies its imports into its own images (`GpuSharedCache::localize`), reading each import once more after leaving the lease.
  - A client whose shared frame did not finish within 2 seconds gives up the lease (by disconnecting) while that frame may still be running.
  - Vulkan gives an external image to one instance at a time. Both cases only read immutable images every client
    keeps in `SHADER_READ_ONLY_OPTIMAL`; the copy's change to `TRANSFER_SRC_OPTIMAL` is on the client's own image object.
  - Unknown: whether a driver may rewrite such an image's memory (compression metadata) on that transition.
  - Settle: Khronos guidance or the driver vendors' word on external images. A validation-layer run of the failure
    paths on AMD, NVIDIA and Intel would at least rule out API misuse.
- **Failure paths** (broker killed, a client killed holding the lease, lease timeout, hung frame, localisation):
  unit-tested only, not on Windows with real GPUs.

## Network and session

- **Launcher tickets.**
  - Checked against the RoF2 client: the token packet layout, the native switches (`/ticket:`, `/name:`, `/server:`;
    eqgame `InitGameInstance` @005fdec0) and returned-token replacement.
  - Not checked: live ticket issuance/redemption, expiration, returned-token lifetime and world restrictions against a production issuer.
  - Settle: a live launch against a token-issuing login server.
- **Return Home on the character select.**
  - Now: enabled for a character whose list entry has both `go_home` and `enabled` set.
  - The original checks `bCanGoHome` and `bUseable` (`CCharacterListWnd::EnableButtons` @0x42f4b0); that `bUseable`
    comes from the list's `enabled` byte is assumed. Stock EQEmu always sends `enabled = 1`.
  - Settle: tracing where OP_SendCharInfo's handler fills `CharSelectInfo` +0x15c.
- **Undefined zone-in packets.** The client does not send the zone-in packets EQEmu's RoF2 table does not define
  (tribute, social post and similar). EQEmu servers do not need them.
- **Unnamed packets get no answer.**
  - Packets with no name in EQEmu's RoF2 table are sent but get no answer: the opcodes behind `/shieldgroup`,
    `/decaycorpse`, `/uptime`, `/servertransfer`, `/acompare`, `/spellscribe`, `/offlinemode` and the veteran reward
    claim; the bazaar's and barter hall's Find (0x0e44); the duel refusal.
  - A plain duel refusal (declined or ignored) also goes as EQEmu's 12-byte OP_DuelDecline, which tells the challenger.
  - "Already dueling" and "considering another duel" (kinds 1 and 2) are not reported on EQEmu, which refuses such
    challenges before they reach the player.
- **Server files: unacknowledged pieces.**
  - The server script cannot see what a client has acknowledged (EQEmu's Lua has no view of the resend queue).
  - Now: what a client that never acknowledges makes the zone hold is bounded only by the rates:
    `files_chunks_per_second_zone` x 32 KB for EQEmu's 30 s resend timeout, about 1 GB at the defaults.
  - Settle (what default is safe): a Lua call for a client's unacknowledged bytes, or a live measurement of the zone's memory under such a client.
- **Server files on a Windows server.**
  - The script notices a file changed in place since its hashing by its size only (a PowerShell start per file to read its time would be too slow).
  - A file rewritten with the same size keeps its old hash until the next listing (`files_rescan`, `#reloadquest`); the client then finds it damaged and keeps its old files.
  - Settle: a cheap way to read a file's time from EQEmu's Lua on Windows.
- **Declining a group invite.**
  - Sends `OP_CancelInvite`, which stock EQEmu does not handle; the inviter is told only on a server with the Hailstone server script (`server/`).
  - The relay was checked live once, on the NMS test server (docs/todo.md "Network and session"), not yet on a stock server.
  - The stock handler's other step, removing the decliner from its group, is not done.
- **Group pet targeting: which pet.**
  - Unknown: RoF2's F1 (ExecuteCmd case TARGETME @0x4d8000) takes the first spawn whose MasterID is the player's and
    that is not a temporary pet; F2..F6 (case PARTY1..5 @0x4d80c0) take the member's PetID (+0x2b4). How RoF2 fills
    PetID, and whether a mercenary, an untargetable spawn or a corpse can be either one, is not traced.
  - Now: both keys and the Modern pet bar take the live, targetable spawn of pet type whose owner id is the member's,
    skipping temporary pets, mercenaries and corpses (`group_pet_of`, net/game_connection_group.cpp).
  - Settle: the writers of PetID in the spawn and charm handlers, or a live RoF2 session with a mercenary and a charm.
- **Login: already online and kick.**
  - EQEmu's login server never answers 302/114. Its "already online" is 111, a plain refusal, and only with
    `World:EnforceCharacterLimitAtLogin`. It has no kick request (0x16) or reply (0x32).
  - The Yes / No question, the kick and the second play request follow eqmain, tested against the mock only.
  - The login reply's two leading bytes (EQEmu sends 0, 0) are not copied by `LoginClient::OnPlayerLogin`; their use is unknown.
  - The handshake reply's extra string is read and unused, as in eqmain.
  - Server pushes 0x25 to 0x31 are logged and ignored. EQEmu sends only 0x31 (OP_LoginExpansionPacketData,
    with `display_expansions`), which eqmain passes to LoginClient's callback +0x70 (the expansion offers, not followed).
- **Server select: Play for every world.**
  - Now: Play is offered for every world, whatever its status. eqmain's play handler
    (`ServerSelectWnd::WndNotification` @0x10038310) and its row builder (@0x10036750) check none; not every caller of the button was read.
  - Settle: the RoF2 client's server select with a locked world listed.
- **OP_Weblink.**
  - The original opens the address in an in-game HTML window (`WebManager_CreateHtmlWnd`, Gecko).
  - Now: Hailstone opens it in its own browser window (the system's browser without the helper), http(s) only (docs/browser.md).
  - Unknown: what the original does with other schemes, and the window's size and controls (not in the export).
    Settle: tracing `WebManager_CreateHtmlWnd` and `CHtmlWnd`.
  - Unknown: whether it reuses one window for a second link, and whether it limits a flood. Hailstone limits
    server links (`app/server_links.h`, docs/browser.md). Settle: tracing `CHtmlWnd` creation in the OP_Weblink case (@0x4c5b5d).
  - The links' tabs are the player's. Hailstone does not open a link to this computer or the player's network, and
    keeps their cookies apart from the player's tabs.
  - Unknown: whether the original refuses any host, or shares the HTML window's cookies with the player's own pages.
- **The guild window's Go to URL** (`CGuildMgmtWnd`, `GotoURL`).
  - Unknown: what the original does with the guild's URL.
  - Now: opened as `/url` would, only when http(s) (one without a scheme gets `http://`); otherwise a line in chat (docs/browser.md).
  - Settle: tracing `CGuildMgmtWnd::WndNotification`'s GotoURL case.
- **Login receive-idle timeout.** A client choice of 14.8 s. The original login manager sets no receive-idle limit;
  what the original login owner does with a silent link is not traced.
- **Chat link (UCS) timeouts and retries.**
  - Follows UniversalChatProxy_Connect @0x7f0040's Params (chat_server.cpp `ucs_session_options`) and the owner check in
    UniversalChatProxy_GiveTime @0x7efea0 (every 5 s, unacknowledged data over 30 s ends the link).
  - Hailstone's own choices, not the original's:
    - a 60 s receive limit on the chat link (the original sets none, so a link that stopped delivering is never noticed);
    - a 30 s deadline for the MailLogin reply (the original waits on the link alone);
    - DNS counted inside the 30 s connection deadline;
    - automatic retries after 5 s doubling to 60 s, at most 5 in a row. The original's ChatServerGiveTime @0x5382d0
      drops a lost proxy and waits for the next OP_SetChatServer2; `reconnect_delay` 0 does the same.
  - A link stock UCS closes before answering the login counts as a refusal and is not retried (EQEmu closes it on a bad key).
  - Settle (whether to keep the retries): how often a stock server drops the chat link mid-session.
  - Dropping a lost chat link prints nothing, as in the original (ChatServerGiveTime).
  - Channel text sent while the link is down goes through the zone and is lost at the chat server silently.
  - After a retry the client rejoins only ChannelAutoJoin's channels (with "Autojoining channels..."), not ones joined by hand.
- **SessionResponse from another address.**
  - Now: a SessionResponse with the client's connect code is taken from any address and port, and the session moves there.
  - Why: EQEmu binds its UDP socket to 0.0.0.0 and replies without IP_PKTINFO, so a host reached on a secondary or
    failover address answers from the address its route picks; refusing it left such servers unable to connect.
  - Cost: the connect code goes out in clear, so an observer of the SessionRequest can answer first and take the
    session (and the login) to their own host. Such an observer can usually forge the server's address too, so
    pinning it would gain little.
  - Unknown: whether the original's UdpConnection checks the reply's address. Settle: tracing its SessionResponse case.
- **OP_OutOfSession (`00 1d`).**
  - Now: ends the session without a check. EQEmu sends it with a zero connect code, so checking ours would refuse a stock server's.
  - Unknown: whether the original's UdpConnection checks anything in it before ending the link, and with which reason.
  - Settle: reading the `00 1d` case of the original's UdpConnection packet handler.
  - Risk: nothing in it is authenticated; anyone who can forge the server's source address and guess the client's UDP
    port (a sweep of the ephemeral range) can end a session.
  - Honouring it only when the session is otherwise quiet would close that, but delay the end of a session a stock
    server has forgotten (it answers every datagram with `00 1d`). So the client keeps EQEmu's behaviour until the
    original's check is known.
- **Link-death timer.**
  - Now: counts from the last keepalive (`00 06`, taken before its CRC is checked, as EQEmu does) or datagram that
    passed the CRC and decode. EQEmu's own peer restarts it for any datagram from the address.
  - Unknown: whether the original's last-receive time (UdpConnection +0x1d0) is set before or after its CRC check.
  - Settle: reading where +0x1d0 is written in the receive path.
- **Cumulative Ack equal to the next sequence.**
  - Now: an Ack (`00 15`) whose 16 bits equal the next sequence to send widens to that sequence itself, not 65536
    back, so it acknowledges every packet in flight, including ones the server cannot have received yet. That is the
    original's AckAll (@0x8cdf50: `ack > next` moves it back, `ack == next` does not).
  - A stock server acknowledges only what it received, and with at most 400 packets in flight such an ack can only be
    one 65536 sequences old.
  - Settle (whether to treat it as one behind the oldest instead): whether the original meant it, or a live capture of
    an old ack arriving that late.
- **eqhost.txt.**
  - Now: a `Host` line without a port gives the new server the login screen's default port (`ServerProfile::port`,
    5997). EQEmu's login server takes RoF2 on 5999 by default (docs/protocol/login.md).
  - Unknown: the original's missing-port default. `LoginClient::Host::Host` 0x10008390 passes the part after `:`
    through atoi; what it does with none is not read.
  - eqmain's `LoginClient::ReadHosts` (0x1000e1d0, rof2decomp notes/eqmain.md) reads `Host`, then `Host1`, `Host2`
    and so on until an empty value, moving to the next on a failed connect. Hailstone's application reads only
    `[LoginServer]`'s first `Host` (its network API accepts a host list).
- **Membership details.** The feature ids' names and the refusal lines are decoded (`game/feature_restrictions.h`);
  the other uses of the 22 entries are not.
- **Spawn suffix in destructible form** (OtherData 0xe1).
  - Now: the client reads no suffix there. EQEmu writes one only when it is not empty.
  - Unknown: the original appears to read one whenever bit 0x20 is set (`EQPlayer::UnSerializeSpawn`), and to treat
    only bit 0x04 as interactive.
  - Settle: a capture of a destructible spawn with a suffix, or the original's register use at `UnSerializeSpawn`.

## Updates

- **First update of a hand-extracted folder.**
  - A hand-extracted Hailstone folder has no record of what was shipped (`update/installed.txt`), so its first update
    removes none of the files a release dropped; later updates do (docs/updater.md).
  - Settle: a file list in the first-install archives.
- **Windows paths of the updater.** WinHTTP, and renaming the running `hailstone.exe`, `SDL3.dll` and CEF's DLLs into
  `update/previous/`, are written from Microsoft's documentation and compile, but have not run. Settle: a release
  installed and updated on Windows.
- **Other windows on the same folder.** Hailstone windows on the same folder that the client did not start itself
  (not the box set's) are not seen when it installs an update at quit.

## Movement and collision

- **Collision radius.**
  - Now: collision sweeps and the ceiling query use a stand-in radius (0.475 x avatar height, 1.78 for a human).
  - Static inspection of the native actor callback resolves an explicit volume override, or a model radius with stored
    scale factors; the runtime selection for player actors is not verified.
  - `CharacterModel::bounding_radius` is not a safe substitute: its provenance is mixed (authored WLD skeleton data,
    derived static-model bounds, or no EQG value).
  - The floor-probe lift and solid-ladder alternatives compensate meanwhile.
  - Settle: a read-only runtime probe of the native actor's selected volume, override, scale and COFactor.
- **Swept-sphere numerics** (`game/sphere_sweep.*`).
  - An independent implementation; native FPU rounding is still approximate.
  - Settled by static inspection, with synthetic float/double regressions: capsule candidate ordering/counting,
    axial/tangent branches, parallel edge-region refusal.
  - The old mismatch counts (12 of 3000 triangle, 73 of 30000 random) are from the older implementation.
  - Settle: replaying the external vectors, and new targeted recordings.
- **Parallel edge region and capsule candidates.**
  - The edge-region rule for a path parallel to the triangle's plane (refused: no hit, `sweep_triangle` when
    `toward == 0`), and the capsule's candidate order and count (cylinder, start cap, far cap; one usable root picks
    the first, even outside the move).
  - Source: a reading of the original's code, not its recordings; not yet replayed against the recorded vectors
    (docs/todo.md). Checked: SweptSphereTriangle's edge-region selector @0x7a63c0, SweptSphereEdge @0x7a9220.
  - Settle: running tests/movement_vectors_test.cpp with `HAILSTONE_VECTORS_DIR`, and recordings aimed at parallel
    moves and centres inside a capsule.
- **Collision against other spawns.**
  - Omits the native bone-attachment geometry (held items); bodies themselves do not block, by the static reading.
  - Remote ladder-contact probes use zone and door geometry, not a full port of actor attachment collision.
  - Hailstone's NPC collision (docs/mechanics.md "NPC collision") is its own rule on top.
- **Vehicles** (our own design, `docs/mechanics.md` "Boats").
  - Our numbers, unit-tested only, not against a real dock or the LAUNCH model: the helm's collision (16 probes of
    radius 1 at the hull's outline), the rails (a drop over 4 units), the hull's refresh (every 200 ms).
  - A probe already touching something where a step starts is not held by it (so a boat is never stuck); a hull
    resting on a dock can therefore be steered through it where only that probe would catch it.
  - Boats the server moves into each other are the server's; so is a boat a zone script moves riders off (PEQ drops
    them at the next zone's dock).
  - With live game files the classic launches and some ships (LAUNCH, race 72 PRE) have no model, so they are drawn
    as humans and are no boats. RoF2's zone `_chr` archives have them.
- **Jump crouch window.**
  - Takes the duck clip's length (0x406740's virtual getter @0x1003d580: +0x2c). A Luclin human's 800 ms gives the
    500 cap either way.
  - The jump animation the still-collision test asks for is the renderer's last choice (a frame late at most).
  - A controlled spawn that is not drawn (first person) counts the 500 ms after a jump.
- **Frame rate.**
  - The game rules run on a 60 Hz game clock (docs/mechanics.md "Frame rate"), as the original's would at 60 frames a
    second. Whether players ran the original at 60 is not known (it was chosen).
  - With `game.fixed_step = 0` the movement rules run once per drawn frame, as the original's code does:
    - a collision hold in the air keeps SpeedZ (a deliberate difference, docs/mechanics.md "Held in the air"; whether
      the original hung at a jump's top at high frame rates was not tried);
    - the air friction (0.99 a frame) makes a standing jump reach about 5.2 at 60 frames a second, 4.8 at 144 and 3.1 at 500.
  - Settle: timing jumps and falls in the original at known frame rates.
- **Mounts.** The camera does not turn about the seat with the mount's pitch.
- **Cameras** (game/cameras.h). Tracing these functions would settle each:
  - the user cameras' look-at pitch takes CMath's table arctangent (EQGraphicsDX9 0x100b9490); its rounding is not
    traced (truncated here);
  - the tether camera's start behind the player uses the interpolated table (CMath +0x00 / +0x04; not traced, the
    exact cosine is used here);
  - the tether camera's eye drops to bone 0 while sitting, or for the actor animations 0x21 / 0x24 (+0x10c0); not modelled;
  - the camera sweep's filter (0x476410) for actor types 9 and 1 is not resolved: spawns are left out of the sweep.
- **Placed objects** are triangles of the zone mesh, not separate actors, so actor-only filters do not apply to them.
- **Passable triangles in the mouse pick.**
  - Settled: the DLL's per-actor test (`CActorData::FindIntersectionLineSegment` 0x10046780) does not know what the
    actor is; eqgame's LMouseUp (0x5384d0) maps the actor `GetClickedActor` (0x48b6b0) returned to a ground item
    (`FUN_004dbba0`), a placed item or a switch only afterwards. A ground item's passable triangles therefore meet
    the same test as a door's.
  - Unknown: whether that test takes an actor's static collision mesh (0x100bebb0, no flag test, every pass) or its
    per-part mesh (0x100c66e0, passable triangles skipped in passes 3 and 4 unless bit 2), and which triangles each
    holds, for door and ground item models alike; so too what happens to the passable parts of a model that also has
    solid ones.
  - Now: a door or ground object whose model has no solid triangle is picked by its passable triangles in every
    pass; passable parts of models with solid ones, and every placed real estate object's, are not picked.
  - Settle: tracing which path an S3D or EQG actor model takes (vf4c / vf54 in 0x10046780), or clicking through a
    passable part in the original.
- **Classic door collision hulls.** A mesh's referenced WLD polyhedron aligns with the visible gate after adding
  the mesh centre (checked with Nagafen's gate). Hailstone uses that shape for movement and sight when the door's
  entire render mesh is passable. Native precedence for models with both an authored hull and solid render faces
  needs a controlled comparison. Hailstone uses the existing solid faces in that case.
- **Touching switches.**
  - The hit callback runs for each door hit in the doors' order, not the DLL's quad tree order.
  - A switch moving into a player who stands still does nothing until the player moves (as the original's
    standing-still shortcut skips the sweep; not checked against the original).
- **Teleport zone points.** Zone points that request a teleport by index send 0xcadf and wait for the server's 0xcadf;
  untested, as EQEmu never sets them. While waiting the player neither acts nor moves, but the window keeps drawing;
  the original blocks the whole client, with no time limit.
- **Sense heading.**
  - The message reads the true heading, not the compass's; its bands are 64 units wide (225 to 288 is "South", 256
    due south); a negative heading gives an empty direction. Checked: UseSkill @0x45c85f.
  - The compass's own error is ported (`game/compass.h`, unit tests only).
  - That vtable entry 0x65bb80 is AboutToShow (slot +0xdc: the target is picked when the window shows) is inferred
    from other windows' tables.
- **Position history** (OP_FloatListThing).
  - Taken: at the last good position after a move, before each OP_ClientUpdate, at zone lines, in DoTeleportB and from spells.
  - Not taken: the GM and server teleport handlers' samples (OP_Translocate, the rez, opcode 0x4fd9,
    HandleWorldMessage's reasons 5 and 7), the camera's (`CRender::LocalViewSetup`), the vehicle-relative sample.
  - The send when the local player is created goes nowhere (the samples are only forgotten).
- **Close request on a loading screen.** Waits for the next screen (as the original's queued message would); a second one quits at once.
- **Give window closed by itself.**
  - Seen once live (2026-11-18): a Give window to an NPC closed about 6 s after opening, nothing pressed; the item went back to the cursor.
  - Not the server's: stock EQEmu sends OP_CancelTrade only in answer to the client's cancel and at logout
    (`Client::SendLogoutPackets`), and has no trade timer.
  - The client's own closes are the original's: the partner out of reach (its avatar height + 20) at the 500 ms check
    (CTradeWnd::OnProcessFrame @0x780b50), the partner dying or leaving, the window hidden. (Hiding the player trade
    window no longer cancels an NPC hand-in.)
  - Settle: the log says which close it was (`trade with <name>: cancelled, ...`, `trade: the give window was closed`);
    the next occurrence settles it.
- **Which zone version the server runs.**
  - RoF2's zone-in packets do not say. With a live asset folder and no `.s3d`, the revamped `.eqg` of bazaar,
    highpasshold or lavastorm loads only when OP_NewZone's safe point stands on its floor (at most 10 units above it;
    game/revamped_zone.h).
  - Measured on PEQ's rows only: the revamp's safe points stand 2.0 to 4.2 units above its floor; the old versions'
    have no floor under them (bazaar, lavastorm) or one 15.9 units down (nektulos).
  - A server whose zone row has another safe point (an unset 0, 0, 0, a point in the air) gets the refusal or, by chance, the revamp.
  - No fallback on the spawns: they arrive after the load has started, so testing them would mean loading twice.
  - The offline viewers keep the refusal.
  - Settle: testing other servers' zone tables.
- **Arena and Nektulos with a live folder.**
  - Now: the `.s3d` wins over the revamped `.eqg` (`is_eqg_zone`); the safe-point test is not asked.
  - Yet PEQ runs the revamp there: its only Arena row's safe point stands 4.2 units above the live arena.eqg's floor
    and 51 above arena.s3d's (6 of 6 spawns on the `.eqg`, 1 of 6 on the `.s3d`); Nektulos version 1 (expansion 10
    on) has 400 of 404 spawns on nektulos.eqg and 39 of 404 on nektulos.s3d.
  - Open: whether a RoF2 folder's own arena.s3d and nektulos.s3d are the live folder's; whether the safe point should
    also choose between a present `.s3d` and the `.eqg`.
  - Settle (the first): comparing the files with a RoF2 install.
- **The local player's fly mode** (docs/mechanics.md "fly mode").
  - The original rebuilds its effect cache, recomputing levitation, whenever something clears the cache flag
    (+0x1a4): OP_Buff in every form, `HitBySpell`, `RemoveAffect`, the buff tick for tick-varying effects, possibly
    worn item changes.
  - Now: a change of a buff slot's spell (or of the SPA 57 values) counts as a rebuild. An OP_Buff refreshing a slot
    with the same spell, the tick and an item change do not put a server-set mode back.
  - A slot losing or gaining a levitate spell stands for `RemoveAffect`'s and `HitBySpell`'s SPA 57 cases, though
    OP_Buff's copy (bufffade 3) calls neither.
  - The "no levitation left" test reads the buff cache's SPA 57; `PlayerZoneClient::Levitating` also counts worn items and AAs.
  - At zone-in Hailstone applies the spawn record, then rebuilds. Whether the original's first rebuild comes after its
    own spawn exists (`UpdateLevitationState` does nothing without one) is not traced; it matters only when the record
    disagrees with the buffs (stock EQEmu sends 2 for a levitating player, else 0).
  - Settle: a live session with GM `#set flymode` and buffs, or a trace of the cache flag's writers.

- **Animated zone objects** (docs/formats/wld.md "Placed-object animation", docs/mechanics.md "Animated zone objects").
  - Phase: each definition's controller starts with the zone's load here; the original's starts when its first actor
    is made, also at the zone's load. The gap between the original's object creation and its first frame was not
    measured, so the phase may differ by that much.
  - Keys: D3DX compresses each animation set (lossiness 0.1) and the original samples the compressed keys; the authored
    keys are sampled here, so a long track (the rings' 121 keys) may move a little differently between keys.
  - Contact: only the player's sweep, floor test and the boats' hull probes meet the moving parts. The other spawns'
    probes, line of sight, picking and the navigation mesh use the static mesh, so /nav may plan through a ring.
  - A triangle across two moving nodes is carried whole by its first moving corner's node for collision.
  - That the sweep meets the parts where the animation has them is read from the line-segment test's per-part mesh
    path (0x100c66e0); the sphere test's polygon path (`CActorData::FindIntersectionSphere` @0x10045840 for a
    hierarchical actor) was not traced.
  - Settle: a live session in Ssraeshza Temple with a non-GM player going through the rings, and a recording of the
    original's phase against its clock.

## Navigation (a Hailstone addition, docs/navigation.md)

- **Find's navigation line and rainbow strands.** Use the static zone collision. Need live acceptance: readability,
  winding stairs, moving platforms (not part of this ground query).
- **The navigation mesh's agent.**
  - Height 6, radius 2, climb 4, slope 55 degrees: chosen, not the original's (it has no navigation). The sweep's own
    radius is itself a stand-in (above).
  - Paths keep wide of walls so the sweep does not catch; a passage the sweep fits through but the mesh does not is left out.
  - Settle: settling the sweep radius would let the agent match it.
- **Navigation links** (docs/navigation.md "Links").
  - The safe drop (20) is ApplyFallDamage's for a character carrying nothing, falling from rest at the default gravity
    in continuous time. Weight shortens it, the zone's gravity can differ, and the physics' own steps were not simulated.
  - The climb of 4 is read from the sweep's lift, not measured per race (a gnome's lift is smaller).
  - Ours: the swim jump's ledge height (5); link spacing (5 along a ledge, 15 between swims); the steep swim pitch (80
    degrees); a swimmer's aim ahead along the path (4); the strafe's ledge test (4 to the side, a drop over 5).
  - Jumps up onto ledges and lifts are not linked.
  - Walked offline in Old Sebilis (no monsters, no breath); not yet tried live.
- **Stacked floors** (docs/navigation.md "Floors over floors"). A corner or the end counts as reached within 8 in
  height; a goal's floor is chosen with height weighing four times distance across (our numbers). Floors closer than
  8 apart could still be confused. Not yet tried live.
- **Doors on a walk.** A walk clicks a closed door it stalls in front of. RoF2's door record has no teleport
  destination (the server decides on the click), so a door that zones the player would be clicked too;
  `/nav option doors off` stops it. Not tried live.
- **Swimming** (docs/navigation.md "Water"). The surface is meshed and the walk steers the look pitch. Our numbers:
  when to head for air (35%), how near a shore to climb out (12), the steepest pitch (63 degrees). Not seen live end to
  end. Levitation is not navigated.
- **/follow's mesh help** (a deliberate change, `nav.follow`). Its 400 leash and 5 s rest after a stuck detour are our
  own numbers. Not yet tried live.
- **Matching `/find` replies.**
  - Unknown: replies to `/find` and the navigation's quiet path request carry nothing to tell which request they answer.
  - Now: each goes to the oldest request waiting; one unanswered for 10 s (a client number) is taken as never answered.
  - Risk: stock EQEmu sends an empty or partial route at once, but a full path from a thread-pool task
    (`Client::SendPath`, `SendPathPacket`). A quiet request for a long path and a shown find that fails just after it
    can swap. A server that stalls longer than 10 s leaves every later reply one off. Not seen live.
  - Would narrow it: not sending a quiet request while a shown find waits (and the reverse), or checking a reply's end
    against the requested spawn.
- **Live coverage.**
  - Seen live on NMS Dev (East Commonlands, 2026-11-01): build and cache, walks to points and a door, /moveto, the
    TLO, the overlay, pause, a key ending the walk, unreachable ground.
  - Not yet seen live: following a moving spawn, a closed door clicked on the way, the server's /find path while the mesh builds.

## Combat, abilities and spells

- **The target and pet windows' buff slot limit.**
  - Now: `OP_TargetBuffs` and `OP_PetBuffWindow` entries for slot 97 or past it are ignored (`net::kMaxSpawnBuffSlots`,
    EQEmu's RoF2 `NPC_BUFFS`), so a hostile packet cannot grow the lists. A stock server never sends more.
  - Settle: the original's handler for these packets, which shows how many slots its target and pet windows take.
- **A cure's counters after a fade.** EQEmu fades a fully cured buff before the cure's landing OP_Action.
  - Now: the client skips its counter prediction for that type only when both arrive in the same network update (one `GameConnection::update`).
  - In different updates, the next debuff of that type shows fewer counters than the server holds (its icon stays;
    EQEmu sends no `slot_data` to correct it). A natural fade of the same type in the same update also skips a real
    partial cure's display.
  - Settle: EQEmu sending `slot_data` in `ENCODE(OP_Buff)`, or a live capture showing the two never split.
- **Counters at zone-in.**
  - Unknown: `OP_BuffCreate` has no `slot_data`, and `ProfileBuff.counters` is decoded but not mapped to `BuffSlot.slot_data`.
  - Now: when a cure reaches such a buff, the client stops predicting and leaves its removal to the server; it does not
    empty the icon or guess which other buff the cure removes. Locally placed buffs and buffs with nonzero `OP_Buff`
    slot data have known counters.
  - Settle (to predict from zone-in data): confirm how the profile's count maps to per-effect slots (see "Spell gems and
    buff stacking" in [mechanics.md](mechanics.md)).
- **The combat flag across a zone change.**
  - Now: OP_PlayerProfile's rest timer starts the downtime only (PcClient_ApplyPlayerProfileData @0x57a04a), as the
    original does, and the combat flag (+0x31bc) keeps its last OP_RestState value. A new login starts out of combat.
    Zoning while in combat leaves the crossed swords up until the next OP_RestState, if EQEmu sends none at zone-in.
  - Settle: whether the original clears +0x31bc elsewhere on zoning (the PcClient constructor or the zone-in path), or a
    capture of zoning while in combat against stock EQEmu.
- **Slam under an illusion.**
  - Now: granted for every race whose innate table has it (Barbarian, Troll, Ogre and their citizen models 90, 92, 93).
  - EQEmu bypasses the Bash skill check only for races 2, 9 and 10, so under a 90 / 92 / 93 illusion a class without
    Bash sends a Slam the server drops.
  - Settle: trying it on a server, or a server change that adds those races.
- **Leadership purchase costs.**
  - Now: RoF2's native progression (constructor `0x55d8e0`, next-cost getter `0x561750`): group Mark NPC costs 3/4/5,
    raid Delegate Main Assist (index 19) costs 6/7/8. Legacy EQEmu `zone/aa.h` charges 1/2/3 at index 0 and 6/6/6 at index 19.
  - The purchase packet carries only the index; the server decides the charge and returned points. No cost-table packet exists.
  - Settle (to match every server's displayed estimate and white/grey training colours): a server-provided cost table,
    or an explicitly selected compatibility policy.

- **Leadership stat display.**
  - Now: follows the native effects: HP regeneration adds 1/2/3 per active group/raid rank; offense adds 7/15/22/30/37.
    Legacy EQEmu applies 4/6/8 and 10/19/28/34/40 server-side instead.
  - The protocol supplies ranks, not a bonus table. Received vitals and server combat stay authoritative.
  - Settle (to match arbitrary server tuning locally): a server-provided effects table.

- **Blood rendering.** Resolves the exact WLD `<code>_BLD_DAG` and the player's `I_BLOODSPRITE` resource. Missing
  authored bones, multi-frame sprites or unsupported materials omit it; new-style actors have no blood attachment in
  the checked native setup. Not established: live appearance, native random-table parity.
- **Hover respawn.** Follows EQEmu's RoF2 packet lifecycle.
  - Unknown: exact native death-camera transitions. Now: the current view stays while dead, with the existing
    unconscious-camera restoration, and death music stops on same-zone recovery.
  - Still to check against a server: live bind, ordinary/Divine resurrection, controller acceptance, and cancelling a
    pre-death isometric click-to-move at same-zone placement.
  - Resurrection answer: ConfirmationDialog_HandleResponse @0x52d540 case 0x67 holds a yes back from OP_RezzAnswer only
    when the offer's corpse name (+0xa0) also matches the local player's name. That name while hovering on EQEmu was
    not checked, so the client compares only the zone dword; a mismatched name would be held back here where the
    original answers at once.
  - Settle: reading the local spawn's name during a live hover with a resurrection offer.
- **Casting animation 0.** A spell whose casting animation (spells_us.txt field 120) is 0, played by Hailstone's added proc cast stage.
  - Now: the option `cast_animation_fallback` (on by default) plays 43 for a beneficial spell and 44 for a detrimental one.
  - The original's EQPlayer::TriggerSpellEffect (@0x593d10) passes the spell's +0x274 to the source's animation as it is, 0 included.
  - Unknown: what action 0 shows on a model, and how many stock spells have it.
  - Settle: landing such a spell (a weapon proc) on the original client and watching its source.
- **Bard instrument songs on classic and Luclin models.**
  - Now: the drum, lute or horn animation plays only when the spawn is not sitting and, for a bard, holds an instrument
    in the primary or secondary hand (PlayerState bit 0x10, item types 23..26).
  - The original sets that bit for the local player from the inventory (0x5923f0) and sends OP_PlayerStateAdd /
    OP_PlayerStateRemove on the transition; the server relays it (EQEmu: `entity_list.QueueClients`). A server that
    drops it leaves other players' songs unanimated, as in the original.
  - Here the local player's bit is set the same way, sent after OP_ClientReady (stock EQEmu ignores those packets before then).
  - A new-style (EQG) model has no such model block (0x58afa0). The original's landing path (0x593d10) still checks
    posture and, for a bard, the instrument modifier before the model routine; not traced for specific landings, so
    "no model block" is not a promise that every landing plays.
- **Spell missiles.**
  - Valid lifetimes, stages, area contacts and special spell 54/859 endings are implemented from checked behaviour.
  - The large-source launch point uses only a real posed role-0/name anchor; without one the packet origin is kept (a
    deliberate safe difference from the native unchecked temporary).
  - Recorded vectors and live server acceptance are missing.
- **Spell particles: what the DLL's particle update reads from the actor and the creator.** Settled by tracing those
  callers and run-time reads in the original client.
  - Axes: the actor's world matrix (vtable +0x2c4) gives a spell emitter's axes, normalised here; whether its rows carry
    the actor's scale was not read. The client applies that scale once, through the root actor's GetScaleFactor (taken
    as the spawn's model scale, +0x31c).
  - Opacity: the actor's GetAlpha (vtable +0x208, its point's +0xac / +0x80 getter) scales the particles' opacity; the
    renderer fades no actor yet, so it is 1 in play.
  - An actor's own emitters (CActor::vf126 @0x1003a950, the DLL's own caller of the creator) take the moddat.ini
    PESFactor of the point's actor.
  - Floor flag: an entry with the floor flag (.prt +0x5c) is made on the floor below its point (a sweep down, + 0.05)
    with the actor's matrix and no point, so it does not follow the actor. What makes it again as the actor moves was
    not traced, so the client keeps it on its point.
  - The emitter-scaling radius (spawn +0x1220) is -1 from stock EQEmu, so shapes and counts are not scaled.
  - Not drawn: +0x178's orientation angle (a column of the attachment's matrix projected on the screen).
  - The DLL's random tables (filled at run time) are replaced by this client's generator.
  - Alpha test: whether particles pass it (ALPHAREF 1, compare function not set by the particle render state) is
    not known; none is done.
  - The animation-started entries' timing is an approximation (docs/formats/particles.md).
- **CalcTickHPChange's PvP scaling** (75% / 70% of a loss).
  - The original asks that the caster spawn's +0x2cc be set and its type be 0.
  - Now: any player caster counts (as CacheBuffSlotEffects reads it, without +0x2cc). If +0x2cc is set only for the
    local player's spawn, another player's DoT is scaled here and not in the original.
  - Settle: tracing the writes of the spawn's +0x2cc.
- **Endurance upkeep** (1 s, ApplyEnduranceUpkeep @0x448bb0). Tests endurance through the character base's untraced
  virtual +0x70.
  - Now: read as the stored endurance (Cur_Endurance would never be negative, and the discipline could never drop).
  - Settle: naming that vtable slot.
- **Eye control.**
  - Refuses ambiguous partial-name matches, and names that go on with a letter after the player's name. The original
    takes the first partial match in its actor list; this client's unordered spawn map has no such order.
  - A held Eye is kept while its spawn lives, whatever other Eyes appear.
  - Need live acceptance: simultaneous same-prefix Eyes, passive-body mount placement, actor-height changes during remote control.
- **A buff's negative tick count** (a permanent buff).
  - EQEmu sends -1 (duration formula 50), -4 (formula 51, an aura's buff out of the aura's range) and -1000 (its
    `PERMANENT_BUFF_DURATION`, a script's override). EQEmu notes the original's duration function returns -2 when an
    extra argument is set, without saying when.
  - Now: the count is kept signed and every negative one is permanent (`game::buff_is_permanent`).
  - The original's buff tick (ProcessAffects, game/mechanics/ticks.cpp) removes a -1 unless the spell's duration
    formula is 50, and keeps any other negative count without counting it down.
  - Settle (what -2 marks and how the buff window shows each): tracing the original's buff duration function and its callers.
- **Buff caster ids** (DoT stacking, the distance modifier of effect values) are taken from the server's buff packets.
  The original keeps only the ones its own landing prediction wrote.
- **Combat line filters and pets.**
  - Keep bTempPet, and classify the focused pet or a temporary pet whose MasterID matches the local player, as
    ReportSuccessfulHit @0x52efd3 does. Permanent-plus-swarm live acceptance is open.
  - Stock EQEmu's RoF2 encoder writes the temporary-pet byte as zero, and swarm spawns omit petOwnerId, so their owner
    cannot be established from the wire. Names and target-cycling flags are not used to infer ownership.
  - Settle: a live session on a server supplying both fields.
- **Packets EQEmu may drop.** `/dismount`, buff removal by the client and the toggled pet commands send packets EQEmu
  may drop (a pet command targeting the pet itself is ignored by the server).
- **Hot buttons: small differences.** Ranged attack, the AA window on right click and the key-map text placement differ slightly.
- **Hot buttons: item links** (type 12).
  - Now: the item is found by its serial and, when no item has it, by the first with its item id (`game::hot_button_item_slot`).
  - Unknown: how the original fills the record's item (+8) from the saved GUID and id. EQEmu gives items new serials in
    each zone, so the id fallback is this client's.
  - Settle: tracing the writes of the record's +8, and trying a saved link after zoning in the original against EQEmu.
- **Hot buttons: dragging off an inventory slot.**
  - The modern pack also makes an inventory slot's hotbutton when the left button drags off the slot, not only on the
    half-second hold (CInvSlotWnd::HandleLButtonHeld). The hold's length is taken as the right button's 500 ms.
  - The original's left button released off the slot (not held) is taken as the click on the slot pressed.
- **Not implemented:** `/gems` (the Gems mini-game window).
- **`/spellscribe`.** Sends opcode 0x51e5 only when the world's OP_LogServer +0x18 is non-zero. EQEmu sends 0 (and has
  no handler), so it does nothing there. The two numbers it reads are unused in the original too.
- **`/offlinemode`.**
  - Sends opcode 0x53d3, which EQEmu drops. The position history the original sends first (Send_OP_FloatListThing) is not kept.
  - A link death within 15 s of it exits the client after 3 s, but the screen shown is the disconnect one, not OfflineMode.bmp's.
  - The server's 0x5369 that cancels it is not in EQEmu's table.
- **`/targetgroupbuff`.** Keeps TargetGroupBuff in the character's options.ini (the original: eqclient.ini [Defaults])
  and sends it at each zone-in.
- **AllowedToAttack's newbie-zone rule.** Reads the target's Zone (+0x210).
  - Now: filled for every spawn with the current zone's id and instance (`AttackCheckInput::zone`), as a spawn is only
    known in the player's own zone.
  - Unknown: where the original writes a spawn's +0x210 (the spawn packet or the zone-in). Settle: tracing those writes.

- **Buffs in a zone that suspends them** (OP_NewZone +876, 0xddfc5c): what is still open.
  - The plugin bulk reads of the player's own buffs (`get_buff_data_bulk`, `get_buff_name_data_bulk`) take no zone
    flag; they stay right only because the held buff tick sets `received_ms` every 6 s. Without the game folder's
    BaseData.txt row for the character (the tick then waits) they count a held buff down by the time since it arrived.
  - `get_pet_buff_data_bulk` takes no zone flag either, so it counts a pet's suspendable buff down while the pet
    windows hold it. The method is its author's to change (its comment asks for that).
  - Pet and target buffs are not ticked by the client. When a quest clears the flag mid-zone (EQEmu's
    `set_zone_header("suspendbuffs", 0)` sends OP_NewZone again), earlier ones count down by all the time since they
    came, until the server resends them.
  - Settle (how far they drift): a live session with a pet in a suspendbuffs zone, and one where a script clears the flag.
- **Spell descriptions with a live game folder and no server `eqstr_us.txt`.**
  - The spell and item displays wrap a description in RoF2's eqstr 8507 ("Spell Effect:  %1"). A live `eqstr_us.txt`
    has another sentence under that id ("Your %1 equipment set failed to load due to an error. (%2)"), so Courage's
    description reads "Your Provides your target with ... equipment set failed to load due to an error. ()".
  - The description itself (dbstr type 6, its `#` / `$` / `@` / `%` codes) is filled correctly; the wrapper id is RoF2's.
  - This is docs/layout.md's "Files the server must match": the stock PEQ test server's `client_files`
    (export_client_files) bring `dbstr_us.txt` and `spells_us.txt` but no `eqstr_us.txt`, so the live file is read and
    any reworded id can show such text.
  - Not worked around in the client (it would mean carrying RoF2's string table).
  - Settle: the server's assets bringing RoF2's `eqstr_us.txt`. Open: whether to refuse a live file's reworded ids instead.

## Items, inventory and commerce

- **A bag's contents past its own BagSlots.**
  - Now: `game::Inventory::put` and `swap` accept any bag sub-slot below `net::kMaxBagContents` (1000), not only those
    below the bag's BagSlots, so an item the server puts at a sub-slot the bag lacks is kept (and shown nowhere).
  - Unknown: whether the original drops such an item, keeps it, or refuses the packet.
  - Settle: the RoF2 client's item placement for a bag (CharacterBase / ItemContainer's put), or such a packet from a
    test server.
- **Mercenary liaison window** (docs/mechanics.md "Leaving an NPC interaction").
  - Its range check is the window's observer 0x6eb560 (vftable 0x9f1ef8 slot 1): a liaison id set and the liaison
    missing or beyond 20 units, 3D, hides it.
  - Unknown: what calls the observer, and how often. Now: Hailstone checks every connection update.
  - Settle: tracing the callers of that slot in the mercenary manager's event dispatch.

- **Ctrl bag moves** (game/bag_move.h).
  - ProcessMove's ValidateMove refusals (lore, equip, storage) are not run before OP_MoveMultipleItems goes (the server checks).
  - FindRoom's place trading (0x56f1c0) is left out; no list MoveItem builds was found where it adds room.
- **Looting: partial stacks and Link All.**
  - The quantity asked for a partial stack goes in OP_LootItem's last field, which EQEmu ignores: it loots the whole stack.
  - Link All's chat input, and the item display icon's link, skip the IME check of CEditWnd::AddItemTag and count the
    input's links from its text.
  - Both add the link at the end of what is typed; AddItemTag goes through ReplaceSelection, so the original puts it at
    the caret or over the selection.
  - Settle: tracing where CEditWnd keeps the caret while the input does not have the focus, and putting the link there.
- **Loot All with several replies outstanding** (a NO DROP question answered, or an individual loot sent alongside the queue).
  - Now: Hailstone waits for every reply before closing the window, in any order, and refuses a new Loot All until then.
  - Handle_OP_LootItem frees the list and checks bCloseOnComplete on the reply that exhausts it (@0x6bd414, @0x6bd64d),
    which suggests the original closes at once.
  - Settle: tracing what that close does to a request still in flight, or a live session with two requests outstanding.
- **Looting: questions open when the session ends.**
  - When the loot session ends (OP_LootComplete, unk_CLootWnd_ResetLootSession) with a NO DROP or Loot All question
    open, the client drops the question unanswered and clears the window's popup flag (+0x398), so a later yes cannot
    loot that slot of the next corpse.
  - Unknown: whether the original's ConfirmationDialog outlives the session.
  - Settle: reading what ResetLootSession does to an open dialog 100 / 0x65.
  - A zone change, and a disconnect, drop every open and waiting question the same way (their side state, such as an
    augment's pending request, taken back as a no would).
- **Shared bank slots** are always usable; the server refuses when the feature is off.
- **The bank as last seen** (`game/bank_snapshot.h`, docs/mechanics.md "The bank as last seen").
  - EQEmu's lazy bank load sends only the filled slots and no end marker.
  - Now: the bank counts as known from its first bank item packet in a zone (the server sends bank and shared bank in
    one pass), or from the bank window opening.
  - A bank emptied elsewhere (a GM command, a quest script, another character's shared bank) still lists its old items
    until the window opens or a bank item comes.
  - Seen live with `Inventory:LazyLoadBank` on (playprobe `--bank-file`). Not tried: a server with the rule off, the
    zone-change path live, the real client's window.
- **The bag keys** (OPEN_INV_BAGS, CLOSE_INV_BAGS, TOGGLE_INV_BAGS; Shift+B toggles by default).
  - OpenAllBags' other test (CharacterBase_IsInvSlotAvailable, or a vtable call on the player) is left out: every general slot counts.
  - Settle: tracing that call in CInvSlotMgr_OpenAllBags @0x6987b0.
- **A purchase's cost** (CMerchantWnd::PurchasePageHandler::RequestGetItem @0x6f5be0: price × quantity).
  - Now: computed in 64 bits, so a cost past 2^31 copper is refused with the can't-afford message.
  - Unknown: whether the original multiplies in 32 bits (wrapping negative, so the request goes out and the server refuses it).
  - Settle: reading the product in RequestGetItem.
- **Item requests** (EverQuestinfo::IsItemPending, which IsOkToTransact reads for trades). The client counts its item
  clicks, casts, memorizing, augments and guild bank requests; loot, merchant, bank, book and combine requests are not counted.
- **Bazaar.**
  - A failed purchase is recognised by EQEmu's echo (trader id and already-sold 0xffffffff). Not tried live.
  - The trader stall check uses the movement's `pTouchingSwitch` (the floor switch under the player, `game/mechanics/touching_switch.h`).
  - The NMS server's `bazaar` porch stalls (open types 153 and 155) lie outside the zone's geometry; only its FRUGS302
    rugs (type 155) inside the hall work. Elsewhere Begin Trader refuses (6743), as the original would.
  - Hide Traders and Hide Buyers hide spawns from drawing, names, mouse picks and the PC / NPC target keys (0x5c58c0's
    IsInvisible test, unit tests only).
- **Mail ignore list.** Kept and edited only. No reader of it besides its window and loader was found in the original
  (MailMgr +0xa4), and EQEmu's UCS has no mail ignore.
- **Bandolier: action 3 and dragging.**
  - What the server does with OP_Bandolier action 3 (the right button held on a slot) is not known; EQEmu drops it for its size.
  - CBandolierWnd::WndNotification has no case for dragging a slot, so the slots are not dragged here.
- **Bandolier swap with full bags.**
  - Now: a weapon going back may take a free general slot, or a free slot of a bag that fits it, never the cursor.
    Otherwise the swap is cancelled with 9342 (BandolierSwap prints it for ProcessMove's error 0x11). Cancelling
    cannot desync, since nothing is sent.
  - EQEmu's MoveItemToInventory also takes an empty cursor slot, and finally pushes onto the cursor queue.
  - Unknown: whether the original's auto-placement (the destination -1 entries BandolierSwap adds, resolved in
    MultipleItemMoveManager_CheckDestinations) can choose the cursor.
  - Settle: reading CheckDestinations' destination search, or a swap with full bags and an empty cursor in the original client.
- **Bandolier swap: which bag takes a returned weapon.**
  - Now: EQEmu's test (`CanItemFitInContainer`: size, quivers arrows only, bandolier bags small throwing items only),
    not the original's `CanGoInBag` (`game::bag_fit`, which also refuses placeable and tradeskill bags).
  - Why: the server places the weapon without telling the client, and a different choice leaves the inventories apart
    until the next zone-in. A deliberate difference from the original if CheckDestinations uses CanGoInBag; reading it would say.
  - A swap the server refuses inside `Character:BandolierSwapDelay` (an EQEmu rule, 0 by default) also leaves the
    client's prediction in place, as it would the original's; EQEmu then resends only the occupied slots.
- **The item display's placeable and trophy lines.**
  - The placeable line leaves out the "(of type: ...)" list the original adds from item instance fields EQEmu does not
    send, and the line before it (0x6a06b0, a placed item's real estate).
  - A trophy's benefit lines need the trophy tribute lists, which EQEmu does not send, so they never show.
  - Collectible, No Destroy and the trophy benefit id come from bytes EQEmu's RoF2 struct calls unknown (unknown37,
    unknown30), always 0 / -1 from a stock server.
- **`/destroycursor <id>`** (Hailstone's, for plugins; docs/plugins.md).
  - Refuses coin, bags and an item whose id differs, and keeps the Keep marks.
  - Does not refuse a real-estate item: RoF2's item data as EQEmu sends it marks none (SerializeItem in EQEmu's
    common/patches/rof2.cpp writes the header's unnamed `unknowna4` as -1 for every item).
  - Settle: knowing which item field, if any, marks a real-estate item on the cursor (from the original's item display
    or real-estate code).
- **Augment appearance-slot gating by game feature 0x15.**
  - Now: the feature is treated as on for EQEmu accounts. The insert into a type-21 socket works live (2026-10-31).
  - Unknown: what the original reads for that feature on an EQEmu account (and so whether it would refuse with 5485).
- **Ladders.**
  - The client climbs ground objects and the zone's placed objects whose actor tag starts with LADDER (EQObject_Construct's
    type 6; docs/mechanics.md "Ladders").
  - Not traced: the zone ladder's height (its actor's vtable +0x60, taken as the top of the mesh's bounds) and its pick
    mask (taken as the ground objects' 2).
  - The NMS dev server's OBJ_LADDER doors (xorbb door 8, type 58; pillarsalra door 7, type 156) are clicked as switches
    (an OP_ClickDoor, nothing else).
  - Unknown: whether the original gives a door's actor type 6 (Ladder_MountClickedLadder runs on a clicked switch of
    type 6). Settle: EQSwitch's actor setup.
  - Passable ladders (Cabilis's LADDERCB101): no code was found that sets the DLL's triangle bit 2 (a passable triangle
    that still reports its hit, 0x100c5af0), so the original is taken to walk through them without climbing; only a
    few instruction patterns were searched.
  - Now: Hailstone climbs them by its own rule (`[movement] passable_ladders`).
  - Settle: finding a writer of bit 2 to a model's triangle flags (the +0x84 array of CSimpleModelDefinitionBase), or a
    live check in the original client at a Cabilis canal ladder.
- **Tradeskill window.**
  - Not done: the recipe list's header sort (CTradeSkillWnd_SortSearchResults), the component highlights
    (CTradeSkillWnd_RefreshIngredientHighlights), a double click's OP_TradeSkillRecipeInspect, the column widths in the UI ini.
  - OP_RecipeAutoCombine's last 12 bytes (the world container's serial and real estate ids, else 0, -1, -1) are sent as an invalid slot.
  - The packs' green / red component counts are Hailstone's own, not the original's highlights.
- **Tradeskill combine batch** (Hailstone's, docs/mechanics.md). Settled by a live session against EQEmu with a
  multi-result recipe and a nearly full inventory:
  - the result is not known before the first combine, so the room check takes any empty bag place, which may be too
    small (the batch then stops with the result on the cursor);
  - a non-stackable component counts as one (EQEmu's HasItem counts its charges but deletes the whole item per unit);
  - the 400 ms wait for the result after the server's answer is a guess (a later result is put away before the next combine).
- **Marketplace** (net/game_connection_marketplace.cpp; live on Project Triune):
  - **Catalog at zone-in.** Project Triune sends the catalog and its states at zone-in. The client drops them while the
    window is hidden, as the original does (0x1e9f to a hidden window sets +0x318, which 0x77a7 tests); the window's
    Ready brings both again.
    - One session showed an empty window and was not logged, so whether the server failed to answer Ready then is not known.
  - **State 1 rows.** Shown but cannot be bought.
    - The original re-checks one every 2 s through the local player (vfunc +0x38, 0x57d9d0, read as "owns this lore
      item" from a neighbouring function's name) and sets it to 0 when that fails.
    - Now: left to the server's next state feed.
    - Settle: a capture of a state-1 row and the item it names.
  - **Kind 3** (a quest-reward gate). The original refuses Buy (exe text @0x9f1368) when PendingRewardMgr is missing or
    its pending rewards reached a cap.
    - The stock pending manager is tracked, but the marketplace's cap and Inspect route (PendingRewardMgr 0x463c10)
      are not connected. Kind 3 is bought as kind 0.
    - Trace the native purchase gate and inspect route before binding these controls.
  - **Popups.** For the zip-code and quest-reward popups, the original's callback (0x6d8570) is read for the purchase
    answer (Results OK, then the buttons enabled). For the other popups this client only enables the window again and
    stays on the page.
  - **Open work:** the Alt modifier on MKPW_DetailsIcon's right click; 0x5f8920 → 0x5f84c0 after a wallet or result
    (may send 0x7679); what the Results OK click's 0x88e060(1) does; the tree's starting state of a new branch (only the
    toggle of "All Items" was read).
  - **Shelf on reset.** The original's ResetView, SyncShelfToSelection and NavigateToEntry set the shelf to array[1]
    ("Player Studio") while they highlight "All Items". This client uses "All Items" (a deliberate difference, docs/mechanics.md).
- **Merchant sell price for a unit price outside int32** (a tiny, negative or NaN rate). `game::sell_value` gives
  0x80000000, the x86 conversion's result. Settle: a capture with an odd merchant rate.

## Reward selection

- **Inspection reply identity.**
  - Item inspection replies identify only the item id, so they cannot prove which of two otherwise identical generations produced them.
  - Now: the host avoids reusing an undrained id and ignores retired replies, with a 256-id bound per lane.
  - Settle (the remaining read-only ambiguity): a wire nonce.
  - Claim terminal replies echo pending/set/option ids but lack a collection nonce; local tab tokens stop an old reply
    deleting a newer tab.
  - The 10-second wait is Hailstone policy, not verified native timer parity.
- **Preview Reward and live acceptance.**
  - Active-task and offered-task Preview Reward controls are wired. The offer notification was confirmed by static
    RoF2 tracing, not a live native click.
  - The Monster tab stays unavailable; trace its applicability before enabling it.
  - Live use reported generally working (2026-11-12). Still needed: a recorded acceptance pass for offered-task
    previews, claims, delivery and item inspection.

## Achievements

- **Four opcodes from EQEmu#5119.**
  - Taken from the server side of EQEmu#5119, not traced in the original client: 0xdab0 (catalog), 0x059d (state
    snapshot), 0x171e (window), 0x675d (link request). Their packet layouts come from that PR too.
  - Confirmed (tools/gen_opcodes.py): 0x6411, 0x6471, 0x47cb, 0x1665, 0x17f8.
  - Now: a change to that server PR is followed here.
  - Settle: finding each in the original's receive dispatch or senders, or a capture of the original client against that server.
- **Traced in the original's receive dispatch (0x4c3250).**
  - 0x618f (case 0x4d363a, handler 0x403b90) steps over its leading u32 unread, then reads the dense flag, count and
    states as decoded here.
  - 0x2b42 (case 0x4d3690, handler 0x403020) stores each 20-byte record by achievement, component, requirement id and
    type. A component shows the sum of its records over every requirement id and type (0x7c53b0).
  - It finds the component by id in buckets 0, 1 and 2, so the type never picks a bucket. A record whose component is
    in none of them, or whose achievement is unknown, is stored and shows nothing; Hailstone skips it and keeps the
    packet's other records.
  - RoF2 keeps those records on the player (+0x2d4c, beside a second table filled from the player profile) across a
    definitions reload, and recomputes counts from them (0x403200). Hailstone clears them with a new catalog and waits
    for the next progress packet, which EQEmu#5119 sends after definitions and primary.
  - 0x28e0 (case 0x4c6d30, handler 0x402cd0) reads a C string of at most 64 bytes, the achievement id, the state with
    counts and a u8, and hands them to the achievement window (0x611730).
  - Open: what that window does with the trailing u8.
- **Category badges.** Resolved from their received XML animation names and installed atlas rectangles. This
  covers category art only; the earned-toast/individual-achievement icon selection below is a separate question.
- **Live acceptance.**
  - Live use reported generally working (2026-11-12); not a recorded acceptance pass for every flow.
  - Remaining checks: full initialization and refresh, hidden/class-specific indices, self/nearby/guild completion
    messages, link/share, comparison, zoning, reward delivery.
  - The host never runs criteria or grants locally.
- **Reply identity.**
  - Comparison replies have no target id or catalog generation; link replies have no request nonce.
  - Now: local catalog epochs, spawn incarnations, one outstanding request and retired tombstones prevent reattributing
    a late reply. The 10-second timeout is host policy.
  - Settle: a wire nonce, or a recorded original/server exchange.
- **Clicked link refresh.**
  - The local-owner-only network refresh of a clicked link follows the owned server's contract; its native lookup
    branch has not been independently traced.
  - Nonlocal links show their received read-only snapshot. No selected-target or range gate is invented for links.
  - Settle: a focused native link-click trace.
- **Completion toast.**
  - Self completion uses Hailstone's overlay, with 500 ms fades and a 3-second hold.
  - Need a native comparison: exact toast layout/timing, the achievement icon resource mapping, and whether native
    sound is suppressed by the achievement chat filter.
  - Current policy filters chat and the self overlay; valid earned sound/events stay independent.
  - Catalog icon ids and names are kept; no proprietary art is bundled.
  - Legacy EQType achievement gauges are not implemented.

## Guilds

- **`#guild create` permissions.**
  - EQEmu's GM command does not send the new leader the rank permissions (Client::SendGuildRanks runs at zone-in and after a change).
  - Until the leader zones, the client refuses `/guildinvite`, `/guildpromote` and the window's buttons ("You don't
    have the proper access rights."). The original's LocalPC_HasGuildPermission lets a GM through only with the GM
    flag on, so it would refuse the same way.
- **Promotion line.** "%1 is now the rank of %2." (7656) for another member's promotion or demotion was not seen live.
  EQEmu sends that member's SpawnAppearance 23 to the member alone (a join's goes to the zone and prints it).
- **Guild bank.**
  - Tried by one member only. Not seen: another member's view of Banker Only and Single Member items; withdrawals
    refused by permission.
  - After a deposit the selection follows EQEmu's failed flag, which sits where RoF2 reads the slot (deposit slot 0,
    or 1 after a refusal).
- **`/guildwar` and `/guildpeace`.** EQEmu's handlers only log them. What a server answers (a war list, the lines both
  guilds see) is not known.
- **Guild banners.**
  - Packet routes, profile fields and actor appearance were recovered from RoF2; reserved fields are documented in the
    protocol notes.
  - A banner-capable server is needed to verify: modification-item consumption, portal travel, upgrade benefits, the
    order of kit and status replies.
  - The in-world cloth uses Hailstone lighting and its two received texture layers.
  - UpdateBannerPage checks bit `0x1000` of PcBase's virtual +0x5c (`0x681b6e`). The client takes it as the profile's
    expansion mask and gates Change, Plant and Remove on it (The Buried Sea in EQEmu's list). The getter behind +0x5c was not traced.
  - While the design editor is open, the original asks for the kit list and status again after every mode 2 status
    reply (`0x685670`); a server answering each status request with mode 2 repeats that until the editor closes. The
    client does the same; a banner server's traffic would show whether it does.
- **Guild window, Tribute page.**
  - The opt-out request's command and master (stack bytes the original leaves unset) are sent as 0.
  - EQEmu echoes the command, so after an opt-out the member's states show yellow (flag 0) until the next roster; the
    original's (unknown) bytes probably show white. Seen live: every opt change came back with command 0.
  - The TributePool label (EQType 122) is the guild's favour, as the benefit window's.
- **Raids: join lines at zone-in.**
  - Zoning in, EQEmu sends one OP_RaidUpdate add (action 0) per member. The client prints "%1 joined the raid."
    (5059) for each after the first, as CRaid::AddRaidMember is read to do for a member new to the list (seen live as
    17 lines for a raid of 18).
  - Unknown: whether the original prints them at zone-in too (a flag that marks the initial list, or EQEmu's packets
    differing from live's).
  - Settle: a capture from the original client zoning into a raid on EQEmu.

## Characters and models

- **Held-model tint index.** `PlayerClient::SetHeldModel`'s OP_WearChange carries `ItemBase_GetPaletteTintIndex`
  (item definition +0x570) of the primary (slot 7) or secondary (slot 8) item. Which serialised item field fills
  +0x570 is not traced, so Hailstone sends 0 (stock EQEmu also sends 0 for the spawn's tint indexes). Tracing the
  item unserialiser's write to +0x570 would settle it.

- **ActorTagInfo.txt beyond the shield flag** (docs/formats/characters.md "ActorTagInfo.txt").
  - Unknown: what fields 3 to 10 and kinds other than 1 (2, and a later file's 37 .. 64) mean. Their layout is known
    (the loader @0x7f4130 keeps field 3 as an integer, fields 5 to 7 when field 4 is non-zero, fields 8 to 10 when all
    are above 0); their use is not traced. How the loader reads a 9-field line's missing tenth field is not traced.
  - Now: only kind 1 is a shield; the other fields are not read.
  - Settle: trace the users of the record's +8, +0xc and +0x10 in the original.
- **Whether a RoF2 install ships ActorTagInfo.txt** (docs/formats/characters.md "ActorTagInfo.txt").
  - Unknown: the only copies at hand come from later live clients' folders.
  - Now: the file is read when present; without it the original's fixed list applies (`game::is_stock_shield_model`),
    which is what the original does either way.
  - Settle: look in a genuine RoF2 install's `Resources` folder.
- **Held-item effects.** Keep WLD cloud nodes, bitmap sequences and EQG surface maps.
  - The WLD creator's manager density configuration (`+0xa8`, separate from EDD actor density) is untraced; the native
    default 1 and descriptor opacity are verified and used.
  - Unresolved: small-node suppression below about 0.05; WLD material brightness/scaled-ambient formulas.
  - Authored default cloud-node tracks animate with the checked `ObjectAnimation` sampler; their clock starts per
    instance here, and the native controller's sharing and start phase are not traced.
  - The `.prt` animation-trigger timing also applies to held items (including cleric-shield animation 54); static-item
    trigger semantics need a native trace.
  - Need acceptance: live equipment changes, zoning, controller views, full visual parity.
- **IsInvisible for remote actors.**
  - The shared rule uses the server's HP percentage for remote actors, as exact signed HP is unavailable.
  - The native visibility producer also consults internal actor proxies, activity timing and more hide flags that are
    not mapped. No speculative flags or partial-alpha fade are introduced.
  - Settle: a native frame-state capture.
- **Other players' carried lights** (modern lighting only).
  - Come from their spawn record's light and SpawnAppearance type 5.
  - Stock EQEmu fills a client's light only when it builds the spawn packet, and never sends type 5 for a client, so
    another player who equips or puts away a torch keeps the light they zoned in with.
  - OP_WearChange (slots 7 and 8) is not used, since the item's light value is not in that packet.
  - Unknown: whether the original derives a remote player's light from anything else.
  - Settle: reading where the original writes a PlayerClient's light (the spawn record's field and Handle_OP_SpawnAppearance 5).
- **`UseLuclinElementals` off.**
  - The original's "Global NPC Models" load (@0x492780) skips Global5_chr2, Global5_chr and frog_mount_chr;
    `global_character_archives` skips its implicit `global5_chr`.
  - Unknown: whether an entry naming one of them in the player's `GlobalLoad.txt` (CDisplay::InitWorld's loop
    @0x492000) is skipped too. Now: Hailstone still loads it.
  - Settle: tracing InitWorld's GlobalLoad loop for a check of the option.
- **Drakkin illusions.**
  - Follow the decoded stock EQEmu RoF2 feature offsets; an inspected native form-change path reads a wider record.
  - Unverified: the conversion boundary and invalid-heritage normalization.
  - Settle: a packet capture at the native handler boundary.
- **The own model's ornaments after a gear change** (`game::worn_look`). Follow PcClient_RefreshWornSlotVisual @0x4919b0.
  - Not traced: whether the original redraws its own model at zone-in from the spawn record or from its inventory.
  - Not traced: whether ItemDefinition+0x11a (which makes ItemBase_HasDisabledOrnamentAug @0x7b2ea0 return at once) is
    in the item packet. Hailstone treats it as unset.
  - The original's material override from ItemBase+0xf0 is not used: stock EQEmu always sends -1 there.
- **Armour colours under an illusion of a race without per-slot armour.**
  - The RoF2 client draws a player under a skeleton illusion in dyed armour with undyed bones (seen by the user), so
    `game::drawn_appearance` drops slots 0-6's tints for every race outside `is_pc_model_race`.
  - The original's code path does not show where: DressSpawn and OP_WearChange pass the tint to RefreshEquipSlot, and
    SwapBodyMaterial has no race rule.
  - Unknown: whether the original drops only the tint or also the armour texture variant (SKE's chest has red, blue and
    grey bones as variants 01..04, which Hailstone still swaps in); whether it holds for every non-player race.
  - Settle: tracing SwapBodyMaterial's ReplaceMaterial calls (`ActorClient_SwapBodyMaterial.jsonl`) for an illusioned player.
- **Luclin faces.**
  - A beard tint on a one-texture face material (female dwarf face variants 2 and 3 have no beard layer) colours the
    whole face here; how the original draws one-texture Luclin materials is not known.
  - The eye palette entries (ActorClient +0x140 / +0x144) of a model without L_EYE / R_EYE materials stay unset (-1);
    the constructor's value is not known.
- **Boneless Hero's Forge pieces.**
  - Keep the initial rigid transform (bone world multiplied by the adjustment).
  - The old bracer UV symptom matches a flip the current parser avoids; current glove/bracer visuals still need native comparison.
  - Later changes to a child's own matrix are not traced. No extra inverse bind is invented for geometry without a skeleton.
- **Modern Hero's Forge.**
  - Uses an armour-specific alias resolver and native normal/environment maps in modern lighting.
  - Need original-client comparison: fits across every race/gender, animation, preview and illusion/equipment transition.
  - Unverified: EQGL v1/v2 scalar tails, and parameters supplied only by native shader defaults.
  - Layer overrides require matching properties in the base mesh.
  - A layer row whose base mesh is missing (a mixed install) is not followed: the piece keeps its direct model, plainly
    lit. What the live client shows then is not traced.
  - The fifth armour word is kept, but explicit nonzero values are not substituted for primary Material in the dressing
    rules. Settle that with a native comparison before changing robe visibility.
- **Incomplete layer catalogs.**
  - Some installed layer catalogs are incomplete: a black robe mapping can request `GENERIC_CLOTH_ROBE_BLACK` while the
    available modern layer is `GENERIC_ROBE_BLACK`.
  - Now: the client keeps its direct legacy appearance when the mapped base exists but the layer is unavailable and
    their bone/piece/vertex/triangle counts agree.
  - A conservative Hailstone compatibility policy, not native armour classification; it infers no replacement layer
    name and does not modify the game files. Correct the source data to get the modern appearance for such a mapping.
- **Held MPL materials** (docs/graphics.md).
  - Now: the coverage texture modulates the diffuse as `saturate(2 * diffuse * coverage)`, at `e_fCoverageScale0` times the UV.
  - The formula is this client's: the original's MPL shader was not read.
  - Settle: reading that shader, or comparing an MPL item (IT11707) beside the original.
- **Resource level** of Luclin model archives and of some global archives is a best guess.
- **Long character tracks at half their frame time** (docs/formats/characters.md, `assets::long_track_archives`).
  - The archives that halve were read from `CDisplay::InitWorld`'s load requests.
  - Unknown: whether GlobalLoad_chr.txt's S3D fallbacks (ChrTxt_Load) set the byte. Now: taken as not halving, as the
    zone `_chr` loads do.
  - The original halves each track by its own key count. A clip here has one frame time, so a track of 15 keys or fewer
    in a halved clip plays at the halved time too (none seen in a stock Luclin clip).
  - Settle: reading ChrTxt_Load's request, or comparing a Luclin swing's length with the original's.
- **Borrowed (donor) tracks.**
  - A donor's tracks lend only when their WLD defines the donor with mostly the model's bones
    (`src/assets/character.cpp`); otherwise the model has no such clip and plays 0x4b1210's replacement. So a Luclin
    human has no C10, C11 or T07..T09 (global_chr's classic ELF has them, the Luclin ELF has not).
  - Unknown: how the original's ActorAnimation finds a borrowed track by name across the loaded WLDs, and whether it
    would lend the classic ELF's few shared bones. The "most bones" threshold is this client's.
  - Settle: tracing that lookup, or watching a Luclin human throw in the original.
- **Race 127's IVM** (the enchanter pet, docs/formats/characters.md).
  - Now: a world click takes it through the spawn-size sphere a spawn without a model gets. In the nameplate depth test
    only its posed held items hide a plate.
  - Unknown: in the original its fully masked body mesh (0x2c) exists. The client reads 0x2c meshes but drops a body
    like this one, which can draw nothing, to keep that behaviour. Whether the original's click and line tests meet
    that mesh's triangles, a sphere, or only the held items was not traced.
  - Settle: tracing the original's actor pick for a hierarchical actor whose only material is masked, or clicking just
    beside an animation pet's weapon in the original client.
- **NPC head textures** follow a simplified rule (texture-type variant, else head-type variant, else base).
- **Animation: classic base requests** (bone groups, `docs/mechanics.md`).
  - Classic base requests (0x4afe60's, key 0 or 1) are not run through 0x4b0900 and the upper body's cycle test (slots
    0x1c / 0x24) as the original does.
  - Whether that delays a walk / stand change was not traced. A run does not cut an interruptible action such as a social here.
- **Animation fields not traced** (`docs/mechanics.md` "Animations"). 0x4afe60 and 0x58af10 read more than Hailstone
  fills. Not passed to the classic choice:
  - the mount fields beyond GetMount's motion;
  - +0x1229 (a nonzero value stops the fidget; Hailstone leaves it 0);
  - the spell book's slot 0x94 (the local player's open book; left 0);
  - +0x1198 (1: the ship / zone 84 branch; left 0);
  - the jump and fall fields (+0x10 the jump strength, left at 1.35; +0x163; +0x180).
  - Settle: tracing those fields' readers.
- **Eye height** of new-style models uses the classic rule.
- **Height fix-up.** The Half Elf, Troll, Ogre, Iksar and Froglok height fix-up is applied once per spawn when the model has a tunic point.
- **Forged numbers in asset text** (zone-line region tags, `_environmentemitters.txt`, weather.ini cloud counts).
  - Past an integer field's range they are clamped to it (`core/saturate.h`). The original converts with atoi / float
    casts whose overflow is undefined, so what it does is not known.
  - An ATP tag's zone point is clamped to 214748364 before it is multiplied by 10.
- **PFS archive limits.**
  - A member may declare at most 256 MiB (the largest stock one, theater.eqg's, is about 37 MB), and the name table
    16 MiB. Limits of ours; what the original does past them is not known.
  - A name the name table lists twice is one member (no stock archive does it).
- **Duplicate `.ter`.** A ZON that lists a `.ter` twice loads it once (no stock zone does it; what the original does is not known).
- **weather.ini clouds.** At most 256 patches per cloud (`kMaxCloudPatches`, a limit of ours; stock files ask for at
  most 80). The original's limit is not known.
- **JPEG UI art.** At most 16384 a side and 8192 x 8192 texels, as PNG and DDS (a limit of ours).
- **EQG model poses.** One model's clips together get at most 8M resampled bone poses (`kMaxModelPoses`, a limit of
  ours; the most a stock model has is about 400,000). A clip past it is left out.
- **A corpse's end pose after a clip change.**
  - A corpse (or a spawn in stand state 115) that already lies keeps its death clip's end pose when the clip changes for
    a held variant (an item put in or taken from its hands), or when it was hidden on purpose (first person, a
    market-hidden spawn) and is drawn again.
  - The original rebuilds the actor, posing the end at once, on a new spawn and on OP_Illusion
    (BuildActorForSpawn @0x48f4b0, docs/mechanics.md).
  - Unknown: whether PlayerClient_UpdateHoldingAnimation or a hidden actor's return restarts D05.
  - Settle: reading those two paths for a call to the animation object's slot 3 with 0x10.
- **Borrowed tracks for a bone named with another model's code** (South Karana's HLF: `HUFBI_L_TRACK`,
  `HUFL_POINT_TRACK`, `HLMHEAD_POINT_TRACK`).
  - Now: Hailstone replaces the bone name's first letters by position, as many as the model's code has, so HLF's left
    arm takes ELF's `<clip>ELFBI_L_TRACK` and hangs as the right one does (docs/formats/characters.md).
  - Unknown: whether the original builds the borrowed name by position or by the model's code (which would leave that
    arm at its rest angle there too).
  - Settle: InitAnimationTracks @0x407800's track-name build, or watching a Halas citizen female idle in South Karana
    on the original client.

## World rendering

- **Radeon MSAA qualification.**
  - Recorded Windows Plane of Knowledge checks: RX 9070 XT at 1/2/4 samples, AO off, the depth pre-pass and classic
    lighting; RTX 3070 and integrated AMD at 4x; RX 9070 XT's single-sample Temporal/FSR 3.1 paths with and without
    frame generation. Live checks only on RX 9070 XT in Plane of Knowledge (an AO/4x MSAA A/B confirmed the global
    dependency clears grass flicker).
  - Unverified: generated-tree LOD transitions, other GPUs' live sessions, Linux/macOS GPU rendering. The
    driver-internal cause of the grass artifact is not established.
  - The AO memory barrier in `VkContext::begin_overlay` repeats the depth image barrier before it (`frame_sync.h`), so
    by the spec it does nothing. The same change stopped zero-height lawn blades sending NaN (`shaders/lawn.vert`),
    the more likely cure. The client keeps the barrier.
  - Settle: re-test the RX 9070 XT grass AO flicker (AO on, 4x MSAA) with the NaN fix alone and the barrier removed. If
    it stays clear, remove the barrier; if it returns, keep it and record the driver version.
- **Sky zones** (docs/graphics.md, Sky zones).
  - The zones that float in the sky are listed by name (`render::horizon_open_below`: airplane, poair), since neither
    their sky value nor their shape says so. Others may belong (Bastion of Thunder, `bothunder`, other planes not checked).
  - The client gives such a zone sky under it; the original draws its painted dome over the zone's fog colour.
  - Settle: visiting each candidate and seeing whether the sky shows under its ground.
- **Sky.** The classic sky layers are drawn, not a procedural sky. Approximations: cloud patch geometry, cloud colour
  sets, sun and moon colours, the bounce light. Sun and moon follow a simple full turn per day (the classic layers);
  the modern sky turns about a tilted pole of its own (docs/graphics.md, Sky).
- **Fog.**
  - Not applied: the area fog distance blend, the weather fog band, lightning fog.
  - The area fog needs `weather.ini [AreaFogSettings]`, which neither the RoF2 nor the live install has, so it shows nowhere.
  - The modern lighting has its own: the haze eased between places, a storm fog band and the lightning in the haze
    (docs/graphics.md, Modern lighting).
- **Lighting.**
  - Multi-pass point lights are summed per vertex.
  - Approximations: the player's light level; the lighting of EQG materials, doors and characters; placed objects without colours.
  - In the classic lighting characters take the region shaders' scene light, with the zone's baked light where the
    player stands as their vertex colour, never below 0.25 (docs/graphics.md).
  - Unknown: how the original's skinned-model shaders (`SkinMeshOld` and the rest) compute their `lit`.
    Settle: reading the vertex shaders' constant use in `RenderEffects/SPL/SkinMesh*.fxo`, and what `CRender` binds to them.
  - Forgotten Halls' missing TER sidecar lighting is corrected separately.
  - Need controlled visual comparison: the modern indoor baked-light balance and additive crystal's HDR intensity,
    matching the reference player's carried light and vision.
- **EQG surfaces.**
  - The RegionLava2 and RegionLavaH formulas are not settled (the modern lighting draws both as its own lava).
  - The sidecar's second line is taken as ms a frame, and its count wins over a longer list (chambers'
    ra_blood01_n_001.txt says 9 and lists 10).
  - Approximations: alpha-test references of water effects; normal/environment/glow map use in the classic look
    (including the packed `CBGG1`/`CBSG1` glow the classic lighting adds at intensity 1); the heightmap lighting colour.
  - A heightmap tile without ecosystem layers takes its nearest painted neighbour's base ecosystem (the original draws none).
- **Buried zone lines** (toxxulia, mistythicket, oldbloodfield).
  - Their zone line boxes lie 200 to 600 below the ground, read as the original reads them (docs/mechanics.md,
    "Heightmap zones' ATP boxes"), so the original never crosses them. These files are newer than RoF2; how later
    clients place the boxes is not known.
  - Now: the client's own fallback (docs/mechanics.md, "Buried zone lines": a box with ground well over it and no floor
    at its depth fires from the ground over its footprint). Not the original's behaviour.
  - Server side: EQEmu's virtual zone points (`is_virtual`, not set on the test server) would make the lines usable.
- **EQG packed surface maps.**
  - The player's `SkinMeshCBSG1` and `SkinMeshCBSGE1` shaders confirm RG normals with fixed tangent Z 0.9, B added to
    ambient illumination and A replacing material gloss. `CB1`/`CBS1` use full RGB normals.
  - Static-model effect selection for the `CBGG1` alias is untraced; it keeps the shared-class interpretation.
  - Modern HDR gain 2.5 and the weather treatment are Hailstone rendering choices.
- **EQG winding.** EQG triangles are treated as counter-clockwise front faces (found by testing).
- **WLD.**
  - Placements with Z = -32768 are loaded as stored.
  - The placement tilt sign is negated (as EQEmu's azone does).
  - Bitmap names in both a zone archive and its object archive are read from each WLD's own archive.
  - User-defined render methods 0x8000001b–1d and 0x8000002a and up are not in the DLL's legacy table
    (CRender::InitLegacyRenderMethodTable @0x1009aa40 stores 0x00–0x29, skipping 0x1b–0x1d). What ResolveTransparency
    reads for them is not known.
  - Now: drawn opaque, except the untextured 0x8000004b and 0x80000053, which stay invisible. Only 0x80000031,
    gequip's PIPE-MOUTH, ships.
  - Settle: reading what lies at CRender +0x238c past the table, or the original drawing that pipe.
- **Flora and detail textures.**
  - Partial: detail textures, radial flora exclusion areas, the flora's exact density rules.
  - The flora's bounds against damaged `.eco` files are Hailstone's: `*ITERATIONS` at most 10,000, `*DENSITY` at most
    100,000, 65,536 plants per tile and ecosystem (the game's files reach 500 and 2,800). How the original takes larger
    values is not traced.
- **WLD 0x2C meshes** (docs/formats/wld.md "0x2C mesh").
  - Now: the layout comes from libeq and the five shipped fragments. The centre's place is settled by the Fiery
    Avenger's blade matching IT2's and the ghost ship's symmetry. Read past, not used: the 12 bytes before the centre,
    the u16 after the mesh-op count, the polygons' flags word (0x4b in all five) and their four other u16, the mesh
    ops, the 0x200 block and the vertex material groups.
  - Unknown: the original's 0x2C loader in EQGraphicsDX9.dll was not found. If 0x4b meant something to it (a 0x2C on
    zone geometry, two-sided drawing), the client would not know: none of the five is zone geometry, and they draw
    with their materials' own culling.
  - Settle: finding the loader's fragment-type dispatch in EQGraphicsDX9.dll.
- **Top-down WLD palette map.** A WLD palette map (detail textures) stored top-down (negative BMP height; none ships) is
  flipped so v = 0 stays the picture's bottom row. Whether 0x10019780 reads such a file in storage order is not traced.
- **Mip filtering.**
  - Now: Hailstone builds a mip chain in linear light for a world texture that ships without one
    (`assets::build_color_mips` on the CPU, or the GPU's blit on an sRGB view for an opaque power-of-two image, with
    the same result; docs/graphics.md).
  - Unknown: how the original made the chain for such a texture (by hand, D3DX, or hardware generation, and in which
    colour space). Settle: reading its texture setup.
  - A cut-out's chain is a client rule (alpha-weighted colour, level 0's coverage kept on every level); the original's
    cut-out mips are not traced.
- **Compressed textures.**
  - A compressed texture (BC1/BC2/BC3) that ships with its own mips goes up compressed where the device supports BC.
  - One without mips, or a BC2/BC3 painted on a colour key, is expanded to RGBA8 for its chain (the colour key taken
    out) and uploaded uncompressed: four to eight times the texture memory. Vulkan's blit cannot filter a compressed
    format, and the client has no BC encoder.
  - The original kept its DXT textures compressed; how it handled a mipless one is not traced.
- **harbingers' BCN door** (BCN.MDS, an EQGS model).
  - Drawn in its bind pose with its first piece (OBJ_beacon0200 of three).
  - The archive loader (0x10066230) hands every member to EQG_LoadFileByMagic @0x10065c00, which loads EQGS
    (0x100631e0) as it loads EQGM (0x100624e0), so it is taken to register `BCN_ACTORDEF` as the .mod models do.
  - Not traced: the name, the piece the actor shows, and whether it plays bcn_default.ani. Not seen live.
- **Zone bounds for the map** use the collision mesh.
- **The land round a zone past a known shore** (docs/graphics.md "Horizon hills").
  - Where the zone's edge is a shore, the land goes on as open sea with no coast, to the land's reach. Elsewhere it is
    made-up land: a generated terrain worn by water (with coasts, headlands and islands where a shore ends).
  - Neither is the world's real shape: nothing in the client's files or a RoF2 server's packets says what lies between
    zones no zone line joins (Qeynos's harbour and Erudin are joined only by boat; the stock PEQ server gives South
    Qeynos no zone point to Erud's Crossing).
  - Settle: a hand-made layout of the continents. It would be Hailstone's own invention, not the original's.
- **Radial flora cap.** The zone-wide cap (4,194,304 plants, about 16 times Moors) was set without measuring every stock
  EQG zone. A run over the game folder with `EQ_GAME_DIR` set would confirm the margin.

- **Particle hold out of view** (docs/graphics.md "Particles", `display.particle_offscreen_hold`).
  - Widens a model's bounds by 30 units for what its particles drift. That reach is a guess: an effect that drifts
    further (a tall aura, a long trail) can stop emitting while some of it would still show at the screen's edge.
  - Settle: measuring the largest distance any actor emitter's particles reach from its point, over the stock EDD and WLD effects.

- **Animated zone objects: light and sway.**
  - Drawn with the first placement's baked light per vertex; each other placement tints it by its own over that one's,
    channel by channel, keeping the first one's gradient (docs/graphics.md "Animated zone objects").
  - The point lights' shadow cubes and the cover map keep the parts in their first pose; a particle cloud on a moving
    node stays where the first pose puts it.
  - Hailstone's plant sway in the wind is not applied to an object that plays its own animation.
  - Settle: comparing a forest zone (Emerald Jungle) with the original side by side.
- **Camera shake (OP_CameraEffect) and server marquee text (OP_Marquee).**
  - Unknown: the visible size of the shake for a given intensity. The offset per axis is read from the disassembly
    (up to half the intensity each way, in the camera position's units), but no shake was seen in the game.
  - Unknown: whether the original adds the shake to every camera mode (first person, third person, isometric); here it
    is added to the drawn camera in all of them, and picking and line of sight keep the true camera.
  - Not done: the original's per-frame camera driver @0x796dd0 falls back to the unshaken position when its render
    camera refuses the shaken one (that camera's vtable slot 0x38 returning nonzero; what makes it refuse is not
    traced). Here the shaken camera is always drawn, so a strong shake may show the camera inside a wall for a frame.
  - Changed on purpose: the original's clicks likely use the shaken render camera too; here picking keeps the true
    camera so a click does not jitter.
  - Settle: a quest's camera effect and marquee against the original client, side by side, and tracing that slot.

## Audio

- **Reverb and music.** Reverb is simplified (no Miles DSP). Music synthesis uses TinySoundFont rather than a DLS
  synthesiser, ignoring sysex and branch controllers.
- **The sound of the bank window opening.** Hailstone's Individual Sounds volume "Opening a bank bag" (`audio.open_bank`)
  - Unknown: whether the original plays a sound when the bank window itself opens; a quick search of the decompiled
    client found none.
  - Now: the volume covers the open wave of a bag in a bank or shared bank slot (`GameConnection::container_sound`).
  - Settle: a trace of the original opening the bank window with sound on.
- **Duplicate sound names** across archives resolve to the highest-numbered archive.
- **Non-finite values.** A NaN or infinite volume, level, send or reverb parameter (an EMT gain of "nan" read with
  strtof) is 0, and a non-finite sample (an IEEE-float WAV's) is silence; otherwise it would stay in the reverb for the
  session. What Miles does with them is not known.
- **Wave cache.** Decoded waves not asked for over two zone changes are freed (`SoundLibrary::drop_idle_waves`; the
  global waves stay). A freed wave's next play is skipped while it decodes again, as any wave's first play is. The
  original's wave cache policy is not traced.
- **The quest reward's coin lines.** The wave plays for OP_Sound (EQEmu's `QuestReward`, the case at 0x4c7d39), not
  for OP_CashReward, which credits the coin without one (0x4c636d → 0x4bcef0). Hailstone credits both and plays the
  wave for OP_Sound, but prints none of the lines the original prints there: one per nonzero coin amount, built from
  the string table (1438, 1440, 1441, 1442: "You receive %1 copper %2." and so on, with 1439 "from %1" naming the
  giver when its spawn is known).
  - Unknown: which id goes with which of the four amounts in the code, and the lines' colour.
  - Settle: the arguments of the BuildStringFromEQSTR and dsp_chat calls between 0x4c7d39 and 0x4c7ff6.

## UI

- **Quest Journal and Quest Tracker** (modern).
  - Not checked live: quest updates across zoning, timed quest expiry, shared roster updates and membership actions,
    mouse move/resize.
  - The journal keeps the server's objective group boundaries as separators; a quest's groups are not labelled as numbered stages.
  - Settle: a live quest session with a packet capture.

- **The NPC target keys' sight test** (game/targeting.h, app/client_app_input.cpp `prepare_target_cycle`).
  - Settled: the scene query (CSceneGraph vtable +0x98 @0x100690f0) drops a spawn that eqgame's 0x5c5860 finds
    hidden by the world. Unknown: whether that sweep (CDisplay 0x490f60 into s3dCollideSphereWithWorld @0x4767a0)
    also hits doors and placed objects.
  - Now: the zone, placed objects and doors as they stand all block, from the camera (a deliberate change; the
    original starts at the eye point) to any of three points on the spawn (the original: one point 2.5 over it).
  - Settle: read 0x4767a0's object callbacks, or press F8 in the original with an NPC behind a closed door.

- **Group targeting after a death.**
  - Now: Hailstone targets the nearest matching player corpse when the member's living spawn is absent.
  - Settle: check whether RoF2 targets a corpse from the Group window and which it chooses when several are present.

- **Labels and gauges** (`app/lua_ui_values.cpp`).
  - ATK leaves out the profile's `+0x3468` bonus.
  - Not tracked: the leadership experience gauges (23, 24), the loyalty velocity (146), the skill damage labels
    (228..236), the item bonus caps (277..295), the loyalty tokens (296), the trophy tribute labels (297..301), the
    leadership experience labels (130, 131), the mercenary label (72), the subscription time (334), the achievement gauges.
  - The HP gauges' `PlayerZoneClient +0x194` test (the pink band) is not known.
- **Vertical hot bars.**
  - The modern pack's hot bars lay out vertically by their own "Buttons per row". The stock window's vertical page
    buttons (`HB_VerticalBarPageButtons`) stay hidden in the model.
  - The gamepad pack's bars turn vertical (a column left of the minimap, smaller buttons on a short screen) by a click
    on their page number, kept as `hotbar.vertical.<window>`. With Show Spinner off there is no page number to click.
- **Mem. This Page.** The spell book's buttons do nothing in RoF2: the window looks them up but never handles them, and
  its layout makes them 1 x 1. Neither pack shows them.
- **No data on EQEmu.** The trophy tribute, zone guide and zone path windows have no data on EQEmu servers.
- **The Find window's quest givers** (server/README.md).
  - The NPCs whose own quest file has a say or hand-in handler, by the file's text.
  - Missed: a handler a Perl plugin or an encounter adds at run time. Counted wrongly: a file that has the handler but does nothing in it.
  - Two NPC types with the same name in a zone, one scripted by id, both show as having quests.
  - Settle: asking EQEmu's own `HasQuestSub` from Lua (not exposed).
  - The tribute trophy list is empty.
- **Not implemented by choice.** Voice macros and audio triggers, and a few commercial-service windows: the
  marketplace's funding and credit-card pages, polls, alerts and event calendar, trading cards, social sharing. The alert
  history window and its key stay; a stock server sends no alerts, so it is empty.
- **Menus.** Context menus and the EQ menu use this client's own style; the cursor attachment's placement is approximate.
- **Scaling.** Windows scale with the display scale; sizes are stored unscaled.
- **Escape and the UI pack's windows.**
  - The modern pack's windows close on the close-top-window key only when "Close with Esc" is ticked in their
    right-click menu: on by default for windows opened to look at something (`lib/escape.lua` DEFAULT_ON), off for the
    rest. The frontmost goes first, by the order they were opened or clicked.
  - Unknown: which of the original's windows close on Escape, in which order, and whether it then also clears the target.
  - Settle: CLOSE_TOP_WINDOW in `__ExecuteCmd` and the window manager's escape handling.
- **File selection** browses folders, not the OS shell namespace.
- **Context tips.**
  - Hooked: experience, group and guild invitations, consent, red con on the attack key and on a target change while
    attacking, world containers, interactive objects, the Actions window's page on show, player customization.
  - EQEmu never sends the experience tip's flag (the second dword 2).
  - Not hooked: the Actions window's page keys while it is already shown (0x614510's other callers) and its Abilities
    page function 0x613a20; the Options window's Display / Keyboard pages (COptionsWnd::WndNotification, 6 / 7); the hot
    bar help (CHotButtonWnd::WndNotification, 0x57 forced); the merchant tips 0x25 / 0x30 / 0x3f (0x4107d0, 0x5aa200,
    0x460b40: their callers not traced); the real estate overview key (0x51: the overview window is not built).
- **Small differences.** `/report` text collection, `/outputfile` destination (the character's folder) and `/survey`
  (shows the address) differ in detail.
- **Slash-command numbers** (`/split`, `game::cmd_atol`; and every other atoi of typed, server or text-file numbers in
  `game/`, `core::msvc_atoi`).
  - The original uses atol on its 32-bit long.
  - Now: the digits are accumulated in 32 bits and wrap, as the port's MSVC atoi does (`spell_description.cpp`);
    `/split` sums the four amounts with a 32-bit wrap.
  - Unknown: whether the CRT the original links wraps or saturates (LONG_MAX) a number out of range. Only `/split`
    amounts past 2147483647 differ.
  - Settle: reading the atol / strtol the original links.
- **The chat log** (/log).
  - Now: the file stays open while logging, and each line is written through at once.
  - Unknown: whether `CEverQuest::OutputTextToLog` @0x5130c0 opens and closes the file per line, or holds it.
  - The difference shows only when the file is moved or deleted while logging: on Linux and macOS the lines then go to
    the removed file until `/log` names a file again or is turned off and on (on Windows the open file cannot be deleted).
  - Settle: reading OutputTextToLog's file handling.

- **`mq.event`** (`mq/event_pattern.h`, MacroQuest's Blech read as the spec).
  - A `#n#` used twice in one pattern hands on its last.
  - LuaEvent sorts the values with `std::sort`, which MSVC runs as a stable insertion sort for up to 32 values; past 32
    `#...#`, the order MSVC's introsort leaves them in is not traced.
  - Patterns that share a literal's beginning are matched each on its own; Blech's shared tree can turn down a line one
    of them alone would match (a deliberate difference).
  - Settle (the first): running a script with such a pattern under MacroQuest.

## Chat

- **Spell Hit Modes** (a Hailstone display extension). Not yet checked live.
  - Critical-heal messages 427/1039 and generic healing reports do not tell direct heals from HoT criticals or ticks,
    so both follow Healing.
  - Some servers disagree on the caster/spell argument order in DoT messages 12954/13327; abbreviations keep only the
    unambiguous amount and victim.
  - Recognised English generic/plain-text hit and heal reports follow the modes; unmatched or translated custom messages stay full.
  - The split into Damage Over Time and Healing by message type (376/377/399..401 as DoTs, 374/375/402..404 as heals)
    rests on the extended chat colours 355..408 below, themselves unconfirmed.
  - Stock EQEmu's caster-less "You have been healed for N points of damage." follows Healing: it reads the
    heal-over-time filter but names no spell, so a tick cannot be told from a direct heal.
  - Settle: live casts, DoT/HoT ticks and a log-parser session on a stock server, which would also identify any further
    structured message ids or plain-text forms needed.

- **Extended chat colours 355..408 and channels 57..107** (docs/mechanics.md "Extended chat routing").
  - Their names, colour-to-channel mapping and defaults were worked out by the contributor from packet captures of a
    later client (ToB), not RoF2; nothing confirms them for RoF2 or a server that sends them. Stock EQEmu sends none.
  - RoF2 puts a colour above 354 (as the client still does for 355, 381 and 384..387) on channel 22 (Other), drawn in
    user colour 0's RGB (`GetUserDefinedColor` reads index 0 for an index past 98).
  - That list also gives channels 82, 83, 86, 87, 90..94 and 96 colours below 355 (2, 4, 13, 15, 274, 283, 285, 286,
    290, 348, 349). Those keep RoF2's channels here, and are in no menu.
  - Settle: a capture of an EQEmu server that sends these colours.

- Chat codes (docs/mechanics.md "Chat codes"): where the original sends `&PCT;` for a `%` that is no code, Hailstone sends the `%` itself, as it did before, because it shows received chat as it comes and would print `&PCT;` from another Hailstone player. That an original client shows a received bare `%` as `%` is not checked. `%M` names the pet window's pet (the player's one pet on a stock server); the original takes the first spawn in its spawn list whose MasterID is the player, temporary pets included, and that list's order is not modelled, so with a swarm pet out the two may name different pets. A capture of what an original client shows for a received `%`, and its pick with a pet and a swarm pet out, would settle them.

## Real estate

Real estate is built to the original (docs/protocol/rof2-packets.md "Real estate") and tested with unit tests and against the mock server, not yet live against a server with housing (Project Triune).

- **Not checked live.** A live session settles the layouts the analysis could not pin down: the item row's counters,
  the cost leaf's `sub`, and the record's f18, f28, f34 and f40, which Hailstone keeps but does not read.
- **Area test.** The original uses the player's position at PlayerZoneClient +0x284. Hailstone tests the server-axis
  position (x, y, z); the axis order of +0x284 is not confirmed.
- **Zone change reset.** Where the original resets its manager on a zone change is not traced (0x5b1cf0 has no function
  in the DB). Hailstone forgets everything but the owned-estate lists at each zone change and relies on the server's arrival volley.
- **RealEstate_CanPurchasePlot's bypass** (the PC's vt+0x90, 0x5ad900) is read as the GM flag. Its
  feature-restriction-10 refusal for guild plots (0x5b5b90, a line built by 0x7dbc50) is not built; the server refuses
  such accounts instead.
- **RealEstate_CanPurchasePlot's parent walks.**
  - Two walks up the parents (0x5b5b60 for a guild plot above, 0x5b5130 for the nearest neighborhood) stop after 10
    steps, as the manager's other walks (FindChildAtPosition) do, so a parent cycle from the server cannot hang the client.
  - Unknown: whether the original bounds these two. Settle: reading their loops.
- **RealEstate_CanSellCurrentPlot.** Only the owner (2), guild (8, 9) and guild permission 28 checks are built. Left to
  the server: 0x5b7fa0's other refusals (3, 5, 6, 7, 10..12), and the lines of code 83 (0x1228) and code 13 (0x66bf),
  which 0x7dbd10 builds.
- **Transfer.** 0x5b50b0's look-up in the PC's table at +0x64 (0x57bfb0) is read as "the plot is the player's"; what
  that table holds is not traced.
- **Item-move checks.** The checks the original runs before Inventory / Closet / Shed / Crate / Place (0x5b5ff0,
  0x5b69b0, 0x5b6eb0, 0x5b72c0, 0x5af1e0, 0x5aee70) are not built; the buttons send and the server answers with a result code.
- **Deposits.** Coin only. The original can also deposit an alternate-currency item from the cursor (0x7f7f50's walk of
  the PC's currency list); not built.
- **Dropped packets.** 0x1097 (a summary's update, 0x5ab4b0) is read and dropped. Of 0x21ea only the spawn's {estate,
  item} is kept (for its right-click menu).
- **Placement mode.**
  - Follows the original's modes, keys and area check, but the ghost only sits on the zone's surface under the mouse.
  - ItemPlacement_UpdatePosition is not built: no wall fit (CollideWithWorld), no box resting on a surface, no
    mouse-delta moves in free mode. The surface-flatness 0x1d test (normal ±0.2) is not built either.
  - Not traced: the start mode (Hailstone starts in cursor mode or the next one allowed); the wall mode's wheel reset
    (0x5043e0); 0x558820 (wrapping the angles to 0..512 is a guess).
- **Placement's area check and reply.**
  - Uses the areas in the server's axis order, as occupancy does. The original asks with the actor's position and box;
    the axis order there is not confirmed.
  - The 0x626e reply's three u8s and three u32s after the item index are kept but not understood.
  - The placement shares the manager's 5 s pending timeout; whether it has its own is not traced.
- **Placed objects.**
  - Whether an existing actor moves when an update for it arrives is not traced; Hailstone moves it to the new data.
  - The right click on a real estate NPC item always asks for the menu. The original first checks the spawn's own
    right-click action within 20 (0x59eee0 / 0x59ef30, not traced).
  - The 0x21ea binding of an NPC item to a placement session (ItemPlacement +0x24 / +0x28) is not built.
- **`/outputfile`.** `/outputfile realestate` writes only the header: the rows (0x5b3600) are not in the analysis.
  `/outputfile guildhall`'s file is not traced either (5439 always).
- **Not built:** the neighborhood polls (0x15e2, 0x3311, 0x39bb, 0x52b5, 0x6860, 0x7497, 0x593a) and the vote window
  (the manage window's vote button).
- **The windows.**
  - Access edits are sent at once as the whole table on 0x3146; the original queues them (SendQueuedUpdates @0x747ed0, not traced).
  - "May manage" (0x4ff6c0 with 0xc) is read as "your own plot, or a GM". The opt-out and upkeep checks (0x5b09d0,
    0x5aee30) are left to the server.
- **Item menu (0x21ec).**
  - Entry types 2 (placement), 0xc / 0xd (target the item's spawn), 0xe (window 0x4b) and 0x11 (dialog 0x67, eqstr
    0x14d9) do nothing.
  - What the item operations 0x050c, 0x51e3, 0x64a3 and 0x7ad1 do is not known; the menu sends them as the original lays them out.
  - The request's +0x2f8 is sent as the item's id. The original reads item definition +0xec, not confirmed to be the id.
- **Wording.**
  - The text of the Store All / Pack / Unpack questions is not traced, so the windows use their own wording.
  - The access groups' and permissions' names are this client's labels.
  - The cost text is this client's own (PopulateCostCombo @0x74d240 not traced).
  - An alternate currency's name is dbstr type 0x12 of its id, as the inventory's and the point merchant's lists read
    it; whether real estate passes the same id is not traced.
  - The plot window's title (the address, plus " (id)" for a GM) is a guess.
- **The items window.** Its button greying (005aee70 / 005af1e0 / 005b0c60) and its show filters are not built. The
  buttons are enabled when a row is selected and no request is pending.
- **Guild halls.** An instanced guild hall is the one whose zone word has bit 15 set, as the original tests it. Whether
  EQEmu-based servers ever set that bit is not known; without it every guild hall zone allows the bank, as on stock EQEmu.

## Scripts (MacroQuest's TLOs)

- **`String.Replace` and `mq.doevents`.**
  - `String.Replace` results are cut to 2047 bytes (MQ's MAX_STRING less one is assumed).
  - `mq.doevents` handles events queued while it runs in the same call, one thread per event, returning to Lua between them.
  - Neither is checked against MacroQuest. Settle: its `MQ2StringType` Replace and `LuaEventProcessor::RunEvents`.
- **`Math.Dec` past 32 bits** (`${Math.Dec[123456789ab]}`).
  - Held at 0xFFFFFFFF (-1), as strtoul does. MQ reads it with MSVC's `sscanf("%x")`, whose result there is not checked.
  - A blank-only index (where MQ's sscanf returns EOF and MQ uses an unset value) is NULL here.
  - Settle: running those two in MacroQuest on Windows.
- **`String.Compare` and `CompareCS`** give -1, 0 or 1. MQ returns `_stricmp` / `strcmp` as they are, whose size (beyond
  the sign) on MSVC is not checked. Settle: `${String[abc].Compare[abz]}` in MacroQuest.
- **`/target` at an equal distance.** RoF2's GetNearestSpawnMaybe (0x5a04e0) keeps the first of equally near matches
  in its spawn list's order. Now: the lower spawn id. Settle: how the original orders its spawn list (insertion on
  OP_NewSpawn or otherwise), from PlayerManagerClient's list code.

## Server-specific extensions

- **Ability Book class filter on an NMS multiclass server.**
  - The server sends every AA one of the character's classes may use with the class mask 0xFFFFFFF (zone/aa.cpp, the AA
    rank packet). The book counts all of them as Shared, so a class filter or the class sort cannot tell them apart.
  - Settle: the AA's real mask, from the server or the AA tables.
  - Skills get their classes from the skill cap table at the character's level; combat abilities from the spell's class levels.
- **Autoskill's "only when its buff has worn off".**
  - Looks for the ability's own spell id among the player's buffs. An AA or combat ability whose spell only triggers
    another spell (the buff carries the other's id) is never seen as buffed, so it is used again whenever its reuse
    timer allows.
  - Settle (which id to look for): the spell's trigger effects (SPA 340, 374, 470 and similar).
  - Its "out of combat" trusts the server's OP_RestState flag, which EQEmu sends only while rest regen is on
    (`Character:RestRegenEnabled`). Without it, out of combat is only "auto attack off".

## Server add-on host services

- **Live acceptance.**
  - Local tests cover generic follow, map/Find eligibility, raid actor selection, merchant click admission.
  - Need acceptance against an announcing server: live movement on slopes, underwater and mounted; controller
    cancellation; private merchant acknowledgments; raid changes across zones; instance labels; world overlay presentation.
- **Raid subgroups.**
  - The generic raid service supports subgroup moves and removal to ungrouped.
  - There is no verified native direct subgroup-leader promotion here. An add-on must expose any separate server
    command explicitly rather than claiming the host performed it.

## Last names and the level-up effect

- **OP_LevelAppearance: what is still open** (`net/rename_packets.h`, `app/actor_emitters.h` start_effect). The
  mapping is settled; the flag-on emitter's axes and one branch of the creator are open.
  - Known: 0x58fed0 calls the actor's vf126 (vtable +0x1f8, dxgfx `0x1003a950`) with the effect id, the spawn's
    spell point (`spawn + 0xfec + slot * 4`, no bounds check; `assets/model_points.h`) and the flag byte, skipping a
    group whose id is not above 0, a missing point or a spawn without an actor. vf126 passes the effect id to the
    actor-emitter creator (CParticleSystem vtable +0x20, 0x10070640) as an index into `actoremittersnew.edd`
    (EQEmu's 0x4d..0x51 = 77..81, Lvl_UpB_01..05 in the shipped file), for 5,000,000 ms, with the particle system's
    actor density and opacity: the same call as an actor's own emitters (docs/formats/particles.md "Actor
    emitters"), so ActorParticleDensity applies. Each call makes a new emitter.
  - Flag off: the emitter is attached to the point (it follows it), scale 1. Built.
  - Flag on: no point; the point's position is cast 1000 units straight down (0x1013681c = -1000) against the
    collision object (`0x10179114`, vtable +0x1c), raised by 0.05 (`0x10132e6c`), with the point matrix's scale and
    the actor's turn (actor vtable +0x2c4). Built, except the turn: the emitter takes the world's axes here. A
    point with no floor within 1000 below keeps its own z (what the original's cast gives then is not read).
  - Duration: a negative definition duration is its own length; a positive one runs for the asked time (the repo's
    reading of `CreateEmitter` @`0x1006c920`, through `0x10070440`). Lvl_UpB_01..05 all have a negative duration
    (0.5 to 1 s).
  - Not traced: the rest of `0x10070440` (it makes nothing when a definition's byte at `+4` is 0; the loaded
    definitions have no such flag here).
  - Hailstone: at most 64 effects run at once, a newer stopping the oldest (the original sets no limit here).
