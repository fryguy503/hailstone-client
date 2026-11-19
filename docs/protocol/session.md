# UDP session protocol (client side)

Implementation: `src/net/session_codec.{h,cpp}` (pure framing), `src/net/session.cpp` (sockets, timers, state), `src/platform/udp_socket.{h,cpp}` (UDP). Tests: `tests/net_session_test.cpp`. Contract: `src/net/session.h`.

Sources: EQEmu `common/net/` (the connection and struct sources) and `crc32.cpp` (read as a spec, GPL-3, nothing copied); OpenEQ `crates/openeq-net/src/stream.rs` for cross-checking. Verified live against an EQEmu login server on 2026-10-20.

## Packet shapes

All protocol fields are **big-endian**. A packet whose first byte is `00` is a protocol packet and its second byte is the protocol opcode. Any other first byte means a bare application packet (see "Application packets").

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

Only reliable stream 0 is used (EQEmu implements only that one for clients). A reliable payload, a reassembled fragment and each part of a combined packet are fed back into the same decoder, so a reliable packet may carry `00 19`, `00 03` or an escaped application packet.

## Encoding a datagram

1. SessionRequest, SessionResponse and OutOfSession go out exactly as built: no encode passes, no CRC.
2. For everything else the encode passes run in order pass 1, pass 2 over the bytes **after the header**: two bytes for a protocol packet, one byte (the opcode's low byte) for a bare application packet.
   - Pass 1 = compression: `5a` + zlib stream (level 6) when the body is over 30 bytes and that is smaller, else `a5` + the body unchanged. Every encoded packet carries the flag, even an empty body.
   - Pass 4 = XOR: each whole little-endian dword is XORed with the previous *ciphertext* dword, starting with the encode key; the trailing 0–3 bytes are XORed with the low byte of the last ciphertext dword.
3. The CRC is appended: CRC-32 (reflected polynomial 0xEDB88320, init 0xffffffff, final complement) over the encode key's four bytes **little-endian** followed by the encoded packet. With 2 CRC bytes the low 16 bits are written big-endian. This equals `zlib.crc32(key_le32 + packet)`.

Decoding reverses it: check and strip the CRC (drop the datagram on a mismatch), then undo pass 2, then pass 1. A compression flag other than `5a`/`a5`, a zlib error, or an inflated size over **8192** bytes (the RoF2 client's cap) drops the datagram. KeepAlive and OutboundPing are recognised before decoding, as EQEmu does. The link-death timer counts from the last keepalive or datagram that passed the CRC and decode; EQEmu's own peer counts any datagram from the address, but then garbage sent from the server's address and port would keep a dead session up for ever (whether the original skips a bad datagram there is not known, docs/unknowns.md). A datagram larger than the receive buffer (64 KiB, more than UDP carries) is dropped on every platform, never returned cut short (POSIX `recvmsg` and `MSG_TRUNC`; Winsock fails it with `WSAEMSGSIZE`).

What EQEmu negotiates: CRC 2 bytes, max packet 512 everywhere; encode passes none/none on the login server (`EQStreamManagerInterfaceOptions(port, false, false)`), none/none on world, compression on zone servers when the `Network:CompressZoneStream` rule is set. The client adopts the server's max packet size (clamped here to 64–8192 to stay sane). A SessionResponse asking for a CRC size other than 0, 2 or 4 bytes ends the connect with an error ("the server asked for a N-byte CRC"): no other size can be checked, and a large one would leave no room in a datagram.

## Application packets

The opcode is a little-endian u16 followed by the payload. When the opcode's low byte is 0 the sender writes an extra `00` first (`00 00 hi …`), so the packet is not mistaken for a protocol packet; the receiver drops it (EQEmu's OP_Padding case). Unreliable application packets (`send(p, false)`, used for OP_ClientUpdate) go out bare, possibly inside a Combined packet; one that does not fit in a datagram is promoted to reliable, as the RoF2 client does.

## Reliable delivery

- **Sending.** Each payload gets the next sequence (from 0, wrapping at 65536). If `payload + 4 + overhead ≤ max packet` (overhead = compression flag + CRC) it is one Packet; otherwise Fragments: the first carries the total length and `max − overhead − 8` bytes, the rest `max − overhead − 4` bytes. With 512/compression/CRC 2 that is 501 and 505 bytes.
- **Window.** At most 400 packets in flight (the slot ring, param 0x2c) and min(congestion window, 100,000) bytes (param 0x2b); the rest wait. The congestion window starts at max(min(max(2f, 4380), 4f), f) for the fragment size f = max − overhead − 4, grows by f (below the threshold) or max(f²/window, 1) per ack while the window is full, and goes back to its start on a timeout resend.
- **Resend.** The RoF2 client's rule (implemented in `net/reliable_channel.cpp` and checked against 1200 recorded vectors): the timeout is **T = min(1.25 × rtt + 300, 8000) ms**, rtt starts at 800 and each ack of a packet sent only once blends its round trip in as `(elapsed + 3·rtt) / 4`; a duplicate cumulative ack adds 400 and a timeout resend 100 (at most once per rtt), both capped at 8000. Every packet last sent before `max(now − T, first send of the newest acknowledged packet)` is sent again (the second case is a "fast" resend: a later packet got through). A packet first sent more than **180 s** ago (params 0x12/0x13) ends the session. Param 0x15 (150 ms) is the multi-buffer hold time (see Combining), not a resend interval.
- **Acks.** Ack(n) acknowledges every packet from the oldest unacknowledged up to n (n widened to 64 bits next to the next sequence, one 65536 back only when it would be ahead of it, so n equal to the next sequence's 16 bits acknowledges everything in flight, as the original does: docs/unknowns.md); one already behind the oldest adds 400 ms to rtt. OutOfOrderAck(n) acknowledges just n. An ack for a packet that is in the window but has not gone out yet is ignored, a deliberate difference: the original takes it and blends a round trip from time 0 (0x7fffffff ms) into rtt. A server acknowledges only what it received, so a stock one never sends such an ack.
- **Receiving.** An in-order packet is delivered and acknowledged (one cumulative Ack per service pass, for `next − 1`); an early one is stored and answered with OutOfOrderAck, up to 4096 of them and 4 MiB together (`kMaxBufferedPackets`, `kMaxBufferedBytes`: a reassembled payload that is itself an early `00 09` can be up to 8 MiB, so the count alone bounds nothing; a stock server's 4096 packets of 512 bytes are 2 MiB, and EQEmu throttles its resends to 300 packets / 140 KiB). An early packet past either limit is dropped and *not* acknowledged, so the server sends it again; the expected packet is never dropped, so the stream advances and frees room; an old one (a duplicate) is answered by re-sending the cumulative Ack, since the peer evidently missed it. Fragments are concatenated until the total length is reached. A first fragment claiming more than **8 MiB** (`kMaxFragmentedPayload`, a Hailstone bound well above the largest RoF2 packet) is not gathered: its pieces are counted against the total and dropped, so the stream stays in step. A first fragment too short to hold its total length (a stock server never sends one) leaves the payload's end unknown, and with it where every later fragmented payload starts: the session ends with an error (and a SessionDisconnect), instead of reading the next piece as a new first one (as EQEmu's own peer would) and reassembling garbage, or dropping payloads and leaving the game out of step with the server. A piece that runs past its payload's declared total (a first fragment longer than its own total, or a later piece past it) ends the session the same way, instead of being cut at the total with the next piece read as a new first one.
- **Delivered, not yet read.** Application packets wait for the owner's `receive()`; the service thread goes on receiving while the owner is busy (zone loading). Their data plus each packet's own size may come to at most `SessionOptions::max_received_bytes` (64 MiB; a zone-in is a few MiB): past that the client sends SessionDisconnect and the session ends ("too much received data waiting to be read"), since dropping a reliable packet would leave the game out of step with the server. The original has no such queue; this is Hailstone's bound.
- **Combining (the multi-buffer).** Every protocol packet the session sends after the handshake (acks, new and resent reliable packets, unreliable packets, stat packets) goes through the multi-buffer. One of at most 255 bytes whose length + 3 fits the hold size joins the multi-buffer, a Combined packet (`00 03`, then a length byte and the packet for each); one that would not fit in the datagram (max packet − compression flag − CRC) sends the buffer first. The buffer goes out when it reaches the hold size, or once its oldest packet is **hold time** old. Larger packets flush the buffer and go out alone, so the order is kept. A buffer holding one packet goes out as that packet. The hold time is UdpManager Params 0x15: **150 ms** for world and zone **0** for the login server; `SessionOptions::hold_time`. The hold size is min(max packet, Params 0x16); 0x16 defaults to −1, which is replaced by the max packet size, so it is 512. A newer cumulative Ack replaces the one still waiting in the buffer instead of adding a second.

EQEmu resends every unacknowledged packet as soon as *any* ack arrives (`m_acked_since_last_resend`), so duplicates of recent packets are normal; they are answered with an Ack and dropped.

## Service thread

The RoF2 client services its UDP session on its own thread: a loop that receives, acknowledges, resends, flushes and times out, then sleeps **20 ms**, until stopped. The main loop only collects the delivered messages. So a long frame, such as zone loading, never stops the session from acknowledging or resending.

Each `Session` starts a service thread on `connect()` that runs a pass every `SessionOptions::service_interval` (20 ms) until the session is closed. The owner's `update()` runs a pass too, and `receive()` takes the packets the passes queued. Everything the session owns is behind one mutex. A datagram the system refuses to send (the network gone while Wi-Fi drops) is logged once, with a line saying how many failed when sending works again. A gap of more than 1 s between two owner `update()` calls is logged at debug level with what the thread received and sent meanwhile, and each session logs a summary when it closes (datagrams, acks, timeout and fast resends, duplicates and early packets received, rtt, longest owner stall).

Servicing the session only from the owner's `update()`, once a frame, is not enough: zone loading blocks the frame for several seconds (2.2 s for qeynos plus 5 s for the global character archives on a warm cache; longer on a busy machine). For that long nothing would be acknowledged: EQEmu kept resending everything it had sent and drops a client whose oldest unacknowledged packet is **30 s** old (`resend_timeout`), and the client's own packets would not be resent. Measured against EQEmu (2026-10-21): with a 40 s loading stall (`--load-stall 40000`) while another character sent the loading one a tell every 3 s, a client without the thread lost the zone ("the server closed the session") in 3 of 6 tries; with the service thread, 0 of 6, and the thread acknowledged 341 datagrams during a 46 s stall. Without traffic during the stall (nothing unacknowledged on the server) even a 35 s stall survived, and 39 ordinary zone-ins and 10 zone-line crossings showed no drop.

## Timings

UCS has a separate profile (`ucs_session_options`), rather than inheriting the world/zone timings below. It is the
UdpManager Params block `UniversalChatProxy_Connect` @0x7f0040 builds on the defaults of `Params::Params(0)` @0x426e70,
checked in the disassembly: 24 reliable slots (0x2c/0x2d), 10,000 bytes (0x2b), no multi-buffer hold (0x15 set to 0;
the default is 50 ms), no statistics requests (0x1a), a 5 s SessionRequest retry (0x21) and a 30 s connection deadline
(`UdpManager_EstablishConnection`'s argument). Keepalive is 44 s, or 14.8 s with ChatKeepAlive (0xe); there is no
receive-idle timeout. Reliable data has a 120 s transport limit (0x13, left at its default; 0x12 too, so no receive-idle limit). The chat owner
(`UniversalChatProxy_GiveTime` @0x7efea0) also checks the oldest unacknowledged data every more than 5 s and fails
the link once that data is over 30 s old. These ages are independent of incoming keepalives.

Transport connection does not authenticate UCS. Commands wait for a complete successful MailLogin reply;
friend/ignore events sent by stock EQEmu before that reply are held until it arrives. Pending commands and events
are bounded (128 commands / 256 KiB, 256 events / 1 MiB). DNS and the transport handshake share a 30 s connection
deadline; once MailLogin is sent, its reply has a separate 30 s deadline (Hailstone's). The original never
reconnects by itself (`CEverQuest::ChatServerGiveTime` @0x5382d0 deletes a disconnected proxy and waits for the next
OP_SetChatServer2). Hailstone deliberately retries transient failures, after 5 s and then twice as long each time up
to 60 s, at most 5 times in a row (the count restarts after an accepted login; `reconnect_delay` 0 turns retries off),
discarding pending work rather than replaying a mail or channel command into a new session. A refusal requires fresh
connection credentials (the next OP_SetChatServer2): an explicit refusal reply, or the link closing after MailLogin
went out and before any reply, which is how stock EQEmu UCS answers a bad key (ucs/clientlist.cpp closes the stream).
Events accepted after the login and not yet polled are still delivered before the Disconnected event. Late replies
cannot authenticate a replacement attempt. Mailbox, transient mail state and the friends and ignore lists are cleared
when the authenticated session is lost (`tests/ucs_session_test.cpp`).

| What | Value | Note |
|---|---|---|
| SessionRequest resend | 1000 ms default; 5000 ms UCS | param 0x21 |
| Connect timeout | 10 s default; 30 s UCS (`SessionOptions::connect_timeout`) | client choice; includes DNS |
| Link dead: nothing received for | 180 s world and zone (`SessionOptions::idle_timeout`); 14.8 s login; 60 s UCS (Hailstone's own) | param 0x12 (NetMgr +0x50 → UdpConnection +0x15c, checked in `UdpConnection_InternalGiveTime` @0x8c638e: the silence reaching it ends the link, reason 2). Login and chat set none; there the limits are the client's (docs/unknowns.md). A stock EQEmu UCS sends a keepalive after 9 s of its own silence, so 60 s without one means the chat link is gone: the client sends nothing reliable on an idle chat link (channel text goes through the zone), so the 30 s unacknowledged-data check alone never sees it |
| SessionStatRequest interval | 30 s | param 0x1a |
| Client keepalive (00 06) | after 15 s without sending (`SessionOptions::keepalive_interval`) | param 0xe; 14.8 s login, 44 s chat (14.8 s with `[Defaults] ChatKeepAlive`). EQEmu drops a client it has not heard from for 60 s (`stale_connection_ms`) |
| Server keepalive | every 9 s of server silence | EQEmu `keepalive_delay_ms` |
| Unacked reliable limit | 180 s default; 120 s UCS | params 0x12/0x13; UCS also checks oldest unacked >30 s at the application layer |
| Multi-buffer hold | 150 ms world and zone, 0 login and UCS (`SessionOptions::hold_time`) | param 0x15 |
| Service thread pass | every 20 ms (`SessionOptions::service_interval`) | |
| Server drops a client whose oldest unacked packet is | 30 s | EQEmu `resend_timeout` |
| Drain before disconnect | until all reliable data is acked, at most 10 s | |

**KeepAlive.** As the original's UdpConnection (`UdpConnection_InternalGiveTime` @0x8c61c4 reads the delay at +0x230, copied from UdpManager Params 0xe; the time compared is since the last datagram sent, +0x1c8): once nothing has been sent for the keepalive delay, `00 06` goes out at once, encoded and with its CRC like any packet but not through the multi-buffer. EQEmu recognises it before the CRC check and counts it as traffic (`m_last_recv`); it sends its own after 9 s of silence. Earlier readings call Params 0xe the no-data timeout: the UdpConnection constructor (@0x8c4194) copies manager +0x58 (Params 0x12) to the no-data limit +0x15c and manager +0x48 (0xe) to the keepalive delay +0x230, and the chat proxy's ini key for the same field is ChatKeepAlive.

## State machine

`connect()` → **Connecting**: a numeric address at once, a host name looked up on a thread of its own (`platform::resolve_async`, IPv4 preferred; at most four lookups run at once, the rest wait their turn) so the frame that connects (login, world, zone) never waits on a name server; the connect timeout counts the lookup too ("cannot resolve ..."). Then it opens a socket of that family (an IPv6 link-local address keeps its interface, `fe80::1%eth0`) and sends a SessionRequest with a random connect code. A SessionResponse with the same connect code → **Connected** (packets sent while connecting are queued and sent then). A response from another port or another address is accepted and that endpoint adopted (NAT; OpenEQ does the same): EQEmu binds 0.0.0.0 and answers from the address the host's route picks, so a server reached on a secondary address answers from its primary one. The connect code alone ties the reply to the request; it goes out in clear, so someone who sees it can answer first and take the session (docs/unknowns.md). `disconnect()` → **Disconnecting** until the reliable queue drains, then SessionDisconnect → **Disconnected**. A SessionDisconnect or OutOfSession from the server, the connect or idle timeout, the 180 s reliable limit, or too much received data waiting to be read → **Disconnected** with `error()` set. OutOfSession is taken without a check: EQEmu sends it with a zero connect code (`ReliableStreamConnectionManager::SendDisconnect`, for a datagram from an address it has no session for), so there is nothing to match against ours; what the original checks is not known (docs/unknowns.md). The cost: a two-byte `00 1d` with the server's address and port forged ends the session, so an off-path sender who can spoof the source and guess the client's port can disconnect a player.
