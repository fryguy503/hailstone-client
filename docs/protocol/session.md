# UDP session protocol (client side)

| Part | Files |
|---|---|
| Pure framing | `src/net/session_codec.{h,cpp}` |
| Sockets, timers, state | `src/net/session.cpp` |
| UDP | `src/platform/udp_socket.{h,cpp}` |
| Contract | `src/net/session.h` |
| Tests | `tests/net_session_test.cpp` |

Sources: EQEmu `common/net/` and `crc32.cpp`, read as a spec (GPL-3, nothing copied); OpenEQ `crates/openeq-net/src/stream.rs` for cross-checking. Verified live against an EQEmu login server.

## Packet shapes

All protocol fields are **big-endian**. A first byte `00` marks a protocol packet, whose second byte is the protocol opcode. Any other first byte is a bare application packet (see "Application packets").

| Opcode | Name | Layout after `00 op` |
|---|---|---|
| 01 | SessionRequest | protocol version u32 (=3), connect code u32, max packet size u32 (512) |
| 02 | SessionResponse | connect code u32, encode key u32, CRC bytes u8, encode pass 1 u8, encode pass 2 u8, max packet size u32 |
| 03 | Combined | repeated: length u8, sub-packet (any packet without CRC) |
| 05 | SessionDisconnect | connect code u32 (6 bytes in total; EQEmu's `size()` helper says 8 but it writes 6) |
| 06 | KeepAlive | nothing |
| 07 | SessionStatRequest | timestamp u16, 5 × ping u32 (stat/avg/min/max/last), packets sent u64, packets received u64 (40 bytes) |
| 08 | SessionStatResponse | echoed timestamp u16, server time u32, client sent/recv u64 (echoed), server sent/recv u64 (40 bytes) |
| 09 (0a–0c) | Packet, stream 0 (1–3) | sequence u16, payload |
| 0d (0e–10) | Fragment | sequence u16, then in the **first** fragment only total length u32, then data |
| 11 (12–14) | OutOfOrderAck | sequence u16: this one packet arrived early |
| 15 (16–18) | Ack | sequence u16: everything up to and including this arrived |
| 19 | AppCombined | repeated: length (u8; or `ff` + u16; or `ff ff ff` + u32), application packet |
| 1c | OutboundPing | ignored |
| 1d | OutOfSession | the server has no session for this address |

Only reliable stream 0 is used. A reliable payload, a reassembled fragment and each part of a combined packet go back through the same decoder, so a reliable packet may carry `00 19`, `00 03` or an escaped application packet.

## Encoding a datagram

1. SessionRequest, SessionResponse and OutOfSession go out as built: no encode passes, no CRC.
2. Otherwise the encode passes run in order over the bytes **after the header** (two bytes for a protocol packet, one — the opcode's low byte — for a bare application packet).
   - Pass 1 = compression: `5a` + zlib stream (level 6) when the body is over 30 bytes and that is smaller, else `a5` + the body unchanged. Every encoded packet carries the flag, even an empty body.
   - Pass 4 = XOR: each whole little-endian dword is XORed with the previous *ciphertext* dword, starting with the encode key. The trailing 0–3 bytes are XORed with the low byte of the last ciphertext dword.
3. The CRC is appended: CRC-32 (reflected polynomial 0xEDB88320, init 0xffffffff, final complement) over the encode key's four bytes **little-endian**, then the encoded packet. With 2 CRC bytes, the low 16 bits are written big-endian. This equals `zlib.crc32(key_le32 + packet)`.

#### Decoding

Check and strip the CRC (drop on mismatch), undo pass 2, then pass 1. These drop the datagram:

- a compression flag other than `5a`/`a5`;
- a zlib error;
- an inflated size over **8192** bytes (the RoF2 client's cap);
- a datagram larger than the 64 KiB receive buffer: dropped on every platform, never returned cut short (POSIX `recvmsg` and `MSG_TRUNC`; Winsock `WSAEMSGSIZE`).

KeepAlive and OutboundPing are recognised before decoding, as EQEmu does.

The link-death timer counts from the last keepalive or datagram that passed CRC and decode. EQEmu's peer counts any datagram from the address, which would let garbage from the server's address keep a dead session up. Whether the original skips a bad datagram there is not known (docs/unknowns.md).

#### What EQEmu negotiates

- CRC 2 bytes, max packet 512 everywhere.
- Encode passes none/none on the login server (`EQStreamManagerInterfaceOptions(port, false, false)`) and on world.
- Compression on zone servers when the `Network:CompressZoneStream` rule is set.

The client adopts the server's max packet size, clamped to 64–8192. A SessionResponse asking for a CRC size other than 0, 2 or 4 bytes ends the connect with an error ("the server asked for a N-byte CRC").

## Application packets

The opcode is a little-endian u16 followed by the payload. When the opcode's low byte is 0, the sender writes an extra `00` first (`00 00 hi …`) so it is not mistaken for a protocol packet; the receiver drops it (EQEmu's OP_Padding case).

Unreliable application packets (`send(p, false)`, used for OP_ClientUpdate) go out bare, possibly inside a Combined packet. One too big for a datagram is promoted to reliable, as the RoF2 client does.

## Reliable delivery

#### Sending

Each payload gets the next sequence (from 0, wrapping at 65536). Overhead = compression flag + CRC.

- If `payload + 4 + overhead ≤ max packet`, it is one Packet.
- Otherwise Fragments: the first carries the total length and `max − overhead − 8` bytes, the rest `max − overhead − 4` bytes (501 and 505 with 512/compression/CRC 2).

#### Window

At most 400 packets in flight (param 0x2c) and min(congestion window, 100,000) bytes (param 0x2b); the rest wait. With fragment size f = max − overhead − 4, the congestion window:

- starts at max(min(max(2f, 4380), 4f), f);
- grows per ack while the window is full, by f (below the threshold) or max(f²/window, 1);
- goes back to its start on a timeout resend.

#### Resend

The RoF2 client's rule, in `net/reliable_channel.cpp`, checked against 1200 recorded vectors.

- Timeout **T = min(1.25 × rtt + 300, 8000) ms**.
- rtt starts at 800. Each ack of a packet sent only once blends in `(elapsed + 3·rtt) / 4`.
- A duplicate cumulative ack adds 400, a timeout resend 100 (at most once per rtt), both capped at 8000.
- Every packet last sent before `max(now − T, first send of the newest acknowledged packet)` is resent. The second case is a "fast" resend: a later packet got through.
- A packet first sent more than **180 s** ago (params 0x12/0x13) ends the session.

Param 0x15 (150 ms) is the multi-buffer hold time (see Combining), not a resend interval.

#### Acks

- Ack(n) acknowledges every packet from the oldest unacknowledged up to n. n is widened to 64 bits next to the next sequence, one 65536 back only when it would be ahead of it, so n equal to the next sequence's 16 bits acknowledges everything in flight, as the original does (docs/unknowns.md).
- An Ack already behind the oldest adds 400 ms to rtt.
- OutOfOrderAck(n) acknowledges just n.
- An ack for a packet in the window but not yet sent is ignored. Deliberate difference: the original takes it and blends a round trip from time 0 (0x7fffffff ms) into rtt. A stock server never sends one.

#### Receiving

- An in-order packet is delivered and acknowledged (one cumulative Ack per service pass, for `next − 1`).
- An early packet is stored and answered with OutOfOrderAck, up to 4096 of them and 4 MiB together (`kMaxBufferedPackets`, `kMaxBufferedBytes`); a reassembled early `00 09` can be up to 8 MiB, so the byte cap is the real bound.
- An early packet past either limit is dropped and *not* acknowledged, so the server resends it. The expected packet is never dropped, so the stream advances.
- An old packet (a duplicate) is answered by re-sending the cumulative Ack.

#### Fragments

Fragments are concatenated until the total length is reached.

- A first fragment claiming more than **8 MiB** (`kMaxFragmentedPayload`, a Hailstone bound well above the largest RoF2 packet) is not gathered: its pieces are counted against the total and dropped, keeping the stream in step.
- A first fragment too short to hold its total length leaves every later payload's start unknown, so the session ends with an error and a SessionDisconnect (resyncing would reassemble garbage or desync the game).
- A piece that runs past its payload's declared total ends the session the same way.

#### Delivered, not yet read

Application packets wait for the owner's `receive()`; the service thread keeps receiving while the owner is busy (zone loading).

Their data plus each packet's own size may come to at most `SessionOptions::max_received_bytes` (64 MiB; a zone-in is a few MiB). Past that, the client sends SessionDisconnect and the session ends ("too much received data waiting to be read"), since dropping a reliable packet would desync the game. The original has no such queue; this is Hailstone's bound.

#### Combining (the multi-buffer)

Every protocol packet sent after the handshake (acks, new and resent reliable packets, unreliable packets, stat packets) goes through the multi-buffer, sent as a Combined packet (`00 03`, then a length byte and the packet for each).

- A packet of at most 255 bytes whose length + 3 fits the hold size joins the buffer.
- One that would not fit in the datagram (max packet − compression flag − CRC) sends the buffer first.
- Larger packets flush the buffer and go out alone, keeping order.
- The buffer goes out at the hold size, or once its oldest packet is **hold time** old.
- A buffer holding one packet goes out as that packet.
- A newer cumulative Ack replaces one still waiting in the buffer.

Hold time is UdpManager Params 0x15 (`SessionOptions::hold_time`): **150 ms** for world and zone, **0** for login. Hold size is min(max packet, Params 0x16); 0x16 defaults to −1, meaning the max packet size, so 512.

#### Server resends

EQEmu resends every unacknowledged packet as soon as *any* ack arrives (`m_acked_since_last_resend`), so duplicates are normal; they are answered with an Ack and dropped.

## Service thread

As in the RoF2 client, the session runs on its own thread (receive, acknowledge, resend, flush, time out, sleep **20 ms**), so a long frame (zone loading) never stops acknowledging or resending.

#### How Hailstone runs it

- Each `Session` starts a service thread on `connect()`, running a pass every `SessionOptions::service_interval` (20 ms) until closed.
- The owner's `update()` runs a pass too; `receive()` takes the queued packets.
- Everything the session owns is behind one mutex.

Logging: a refused send is logged once (with a count when sending recovers); an owner stall over 1 s is logged at debug level; each session logs a traffic summary on close.

#### Why a thread

Zone loading blocks the frame for several seconds, during which nothing would be acknowledged or resent. EQEmu drops a client whose oldest unacknowledged packet is **30 s** old (`resend_timeout`). In a test with a 40 s loading stall (`--load-stall 40000`) and a tell every 3 s, the client lost the zone in 3 of 6 tries without the thread and 0 of 6 with it.

## Timings

| What | Value | Note |
|---|---|---|
| SessionRequest resend | 1000 ms default; 5000 ms UCS | param 0x21 |
| Connect timeout | 10 s default; 30 s UCS (`SessionOptions::connect_timeout`) | client choice; includes DNS |
| Link dead: nothing received for | 180 s world and zone (`SessionOptions::idle_timeout`); 14.8 s login; 60 s UCS (Hailstone's own) | param 0x12; see note 1 |
| SessionStatRequest interval | 30 s | param 0x1a |
| Client keepalive (00 06) | after 15 s without sending (`SessionOptions::keepalive_interval`) | param 0xe; 14.8 s login, 44 s chat (14.8 s with `[Defaults] ChatKeepAlive`). EQEmu drops a client it has not heard from for 60 s (`stale_connection_ms`) |
| Server keepalive | every 9 s of server silence | EQEmu `keepalive_delay_ms` |
| Unacked reliable limit | 180 s default; 120 s UCS | params 0x12/0x13; UCS also checks oldest unacked >30 s at the application layer |
| Multi-buffer hold | 150 ms world and zone, 0 login and UCS (`SessionOptions::hold_time`) | param 0x15 |
| Service thread pass | every 20 ms (`SessionOptions::service_interval`) | |
| Server drops a client whose oldest unacked packet is | 30 s | EQEmu `resend_timeout` |
| Drain before disconnect | until all reliable data is acked, at most 10 s | |

1. Link dead ends the link with reason 2. Login and chat set no param 0x12; there the limits are the client's (docs/unknowns.md). A stock EQEmu UCS sends a keepalive after 9 s of silence, so 60 s without one means the chat link is gone; the client sends nothing reliable on an idle chat link, so the 30 s unacknowledged-data check alone never sees it. Checked: NetMgr +0x50 → UdpConnection +0x15c, `UdpConnection_InternalGiveTime` @0x8c638e.

#### UCS transport profile

UCS has its own profile (`ucs_session_options`), from the Params block `UniversalChatProxy_Connect` builds on the `Params::Params(0)` defaults:

- 24 reliable slots (0x2c/0x2d), 10,000 bytes (0x2b);
- no multi-buffer hold (0x15 set to 0; the default is 50 ms);
- no statistics requests (0x1a);
- a 5 s SessionRequest retry (0x21) and a 30 s connection deadline (`UdpManager_EstablishConnection`'s argument);
- keepalive 44 s, or 14.8 s with ChatKeepAlive (0xe);
- no receive-idle timeout (0x12 left at default);
- reliable data has a 120 s transport limit (0x13, default).

The chat owner also checks the oldest unacknowledged data every 5+ s and fails the link once it is over 30 s old, regardless of incoming keepalives.

Checked: `UniversalChatProxy_Connect` @0x7f0040, `Params::Params(0)` @0x426e70, `UniversalChatProxy_GiveTime` @0x7efea0.

#### UCS login and reconnects

The transport connection does not authenticate UCS. Commands wait for a successful MailLogin reply; friend/ignore events stock EQEmu sends before it are held until it arrives. Pending commands and events are bounded (128 commands / 256 KiB, 256 events / 1 MiB).

DNS and the transport handshake share a 30 s deadline; the MailLogin reply has its own 30 s deadline (Hailstone's).

The original never reconnects by itself (it deletes a disconnected proxy and waits for the next OP_SetChatServer2). Hailstone deliberately retries transient failures:

- after 5 s, then twice as long each time up to 60 s;
- at most 5 times in a row (the count restarts after an accepted login);
- `reconnect_delay` 0 turns retries off;
- pending work is discarded, never replayed into a new session.

A refusal needs fresh credentials (the next OP_SetChatServer2). A refusal is an explicit refusal reply, or the link closing after MailLogin and before any reply — how stock EQEmu UCS answers a bad key (ucs/clientlist.cpp).

Events accepted after login are delivered before the Disconnected event; late replies cannot authenticate a replacement attempt. Mailbox, mail state and friends/ignore lists are cleared when the authenticated session is lost (`tests/ucs_session_test.cpp`).

Checked: `CEverQuest::ChatServerGiveTime` @0x5382d0.

#### KeepAlive

Once nothing has been sent for the keepalive delay (Params 0xe), `00 06` goes out at once, encoded with its CRC but bypassing the multi-buffer. EQEmu recognises it before the CRC check, counts it as traffic (`m_last_recv`), and sends its own after 9 s of silence.

Params 0xe is the keepalive delay, not the no-data timeout (that is 0x12), despite earlier readings; the chat proxy's ini key for it is ChatKeepAlive. Checked: `UdpConnection_InternalGiveTime` @0x8c61c4 (delay +0x230, last send +0x1c8), UdpConnection constructor @0x8c4194 (manager +0x58 → +0x15c, +0x48 → +0x230).

## State machine

#### Connecting

`connect()` → **Connecting**.

1. A numeric address is used at once. A host name is looked up on its own thread (`platform::resolve_async`, IPv4 preferred, at most four at once), so the connecting frame never waits on DNS. The connect timeout includes the lookup ("cannot resolve ...").
2. It opens a socket of that family (an IPv6 link-local address keeps its interface, `fe80::1%eth0`) and sends a SessionRequest with a random connect code.
3. A SessionResponse with the same connect code → **Connected**. Packets sent while connecting are queued until then.

A response from another address or port is accepted and that endpoint adopted (NAT; OpenEQ does the same; EQEmu binds 0.0.0.0 and may answer from its primary address). Only the connect code ties reply to request, and it goes out in clear, so an observer can answer first and take the session (docs/unknowns.md).

#### Disconnecting

`disconnect()` → **Disconnecting** until the reliable queue drains, then SessionDisconnect → **Disconnected**.

These end in **Disconnected** with `error()` set:

- a SessionDisconnect or OutOfSession from the server;
- the connect or idle timeout;
- the 180 s reliable limit;
- too much received data waiting to be read.

OutOfSession is taken without a check: EQEmu sends it with a zero connect code (`ReliableStreamConnectionManager::SendDisconnect`). What the original checks is not known (docs/unknowns.md). The cost: an off-path sender who spoofs the server's address and guesses the port can end the session with a two-byte `00 1d`.

`Session::peer_disconnected()` is set only for a decoded six-byte SessionDisconnect carrying the current connect
code. The receipt is protected by the session mutex and resets on the next `connect()`. Timeouts, local closes,
socket failures, OutOfSession and malformed or stale disconnects leave it unset. Application actions use this receipt
when they require a server-confirmed disconnect.
