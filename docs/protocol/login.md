# Login server protocol (SoD/RoF2 port)

| Part | Where |
|---|---|
| Implementation | `src/net/login.{h,cpp}` (`LoginClient`, `login_codec`), `src/net/des.{h,cpp}` |
| Tool | `netprobe [-v] login HOST[:PORT] USER PASS [SERVER_NAME]` |
| Tests | `tests/net_login_test.cpp`; the live check runs only with `HAILSTONE_LIVE_LOGIN=host:port` |

Server side: EQEmu `loginserver/client.cpp`, `login_types.h`, `world_server.cpp`, `encryption.cpp`, `login_util/login_opcodes_sod.conf` (read as a spec, nothing copied).

The RoF2 client uses EQEmu's "SoD" login port (`client_configuration.sod_port`, default 5999; opcodes from `login_opcodes_sod.conf`). Encode passes none, CRC 2 bytes (see `session.md`).

Session options (`LoginClient::Options::session`):

- max packet 512;
- a keepalive after 14.8 s without sending;
- link death after 14.8 s without receiving (the client's own limit; eqmain sets none);
- multi-buffer hold 0, no stat requests;
- resend adjust 2000 (T = min(1.25 rtt + 2000, 8000) ms);
- 100 reliable slots.

## eqhost.txt

A server's patcher ships `eqhost.txt` in the game folder to name its login server. When the asset folder has one and there is no saved server, Hailstone imports it once as a saved server named after its address, and picks it (`profile_from_eqhost`). `login.eqhost_imported` in `hailstone.ini` records that this was done (or that servers already existed), so deleting every server does not bring it back.

What is read:

- Only the first `Host` line of `[LoginServer]` (or one before any section): `Host=name:port`, `Host=name`, `Host=[v6]:port` or a bare IPv6 address.
- Not `Host1`, `Host2` or other keys, nor the older `[Login Servers]` block format.
- A line without a usable host, or with a port outside 1 to 65535, is skipped for the next.
- A UTF-8 byte order mark is skipped, and at most 64 KiB of the file is read.

Without a port, the server gets the login screen's default for a new server (`ServerProfile::port`, 5997). What the original client uses then is not confirmed (docs/unknowns.md).

## Opcodes

| Opcode | Name | Direction |
|---|---|---|
| 0x0001 | OP_SessionReady | → |
| 0x0002 | OP_Login | → |
| 0x0003 | OP_Logout (unused here) | → |
| 0x0004 | OP_ServerListRequest | → |
| 0x000d | OP_PlayRequest | → |
| 0x0016 | kick request (remove the logged in character; not in EQEmu) | → |
| 0x0017 | OP_ChatMessage | ← |
| 0x0018 | OP_LoginAccepted | ← |
| 0x0019 | OP_ServerListResponse | ← |
| 0x0022 | OP_PlayResponse | ← |
| 0x002f | order-expansion reply (DES-encrypted; ignored) | ← |
| 0x0025–0x002d, 0x0030, 0x0031 | server pushes (chat, GM list, banner, poll, game features, OP_LoginExpansionPacketData; ignored) | ← |
| 0x0032 | the kick request's reply (base reply; not in EQEmu) | ← |

All messages go on the reliable channel.

## Message format

Every message is one application packet, little-endian:

**opcode u16 | sequence u32 | compressed u8 | encrypt type u8 (2 = DES) | unused u32 | body**

A 12-byte header (in `AppPacket` the opcode is separate). Replies begin their body with **success u8 | error id u32 | error text\0** (eqlsstr_us.txt ids; 101 = no error).

- **Encryption.** Type 2 is DES-CBC with an all-zero key and an all-zero IV, fresh for every message. The body is zero-padded to a multiple of 8.
  - The client encrypts only OP_Login.
  - It decrypts only replies 0x18 and 0x2f, whatever their header says, over the body rounded up to a multiple of 8.
  - The all-zero key is a DES weak key: encrypting twice gives the plaintext back.
- **Compression.** The client never compresses. A reply whose byte 6 (the compressed flag) is 1 is inflated with zlib before it is decrypted.

## Flow

#### 1. Session

The session handshake (`session.md`). No session within 10 s, or a failed one: the next host (eqhost.txt `Host`, `Host1`, …; `LoginClient::start` takes a list). None left: **6011**.

#### 2. OP_SessionReady

Header + u16 **11** (14 bytes). The reply is **OP_ChatMessage**: base reply and one more string (`SCHandshakeReply` +0x20; EQEmu sends it empty, with sequence 2 hard-coded). Only the success byte is checked.

#### 3. OP_Login

Encrypt type 2: DES of **user\0 password\0 token\0 flag**.

| Form | Bytes | Used for |
|---|---|---|
| Password | `user 00 password 00 00 00` (the token empty, flag 0) | interactive and saved-account logins |
| Token | `00 00 token 00 01` | an explicit launcher ticket, with no username/password fallback |

#### 4. OP_LoginAccepted

The reply (decrypted): success u8 | error id u32 | text\0 | u8 | u8 | **account id** u32 | **session key**\0 (read to its NUL) | failed attempts u32 | show player count u8 | 7 × u32 offer fields | token string | commerce string.

EQEmu writes the key as a fixed 11-byte field (10 characters and a NUL).

- **Failure** shows the reply's error text (EQEmu sends **105**; login.eqemulator.net **107** for a wrong password) and clears the password box. Only success, error id and text are read: a failure reply can end inside the offer fields.
- **Failed attempts > 0** after a success: **6018** "There have been %1 failed login attempts…" (`LoginClient::notice()`).

#### 5. OP_ServerListRequest

Header only. The reply is **OP_ServerListResponse**: base reply | count u32 | per server:

- address\0;
- type u32 (1 standard, 8 preferred, 16 legends);
- id u32;
- name\0, country\0, string\0 (EQEmu: language);
- status u32 (0 up, 1 down, 4 locked; flags);
- players u32.

Status shown: the locked bit wins, so 4 and 5 read **Locked** (6007); else the down bit, **Down** (6006); else **Up** (6005). (With show-player-count the original shows "Locked (Down)" or the player count; Hailstone does not.) Stock EQEmu sends 4 for a locked world with zones booted, 1 when none is booted. The status does not stop a play request.

While the list is shown, it is re-requested once more than **60 s** have passed since the last request.

Checked: eqmain server row @0x10036750.

#### 6. OP_PlayRequest

Header + server id u32 (16 bytes).

It is refused locally with **6028** (nothing sent) when the server's country is not the client's locale (us), unless the login reply set show-player-count. Country mapping: first two characters, us kr tw jp de fr cn, anything else us.

That is the only local refusal; a down or locked world is asked like any other. EQEmu admits an account with status at least `GM:MinStatusToBypassLockedServer` to a locked world and answers **326** to anyone else. Checked: `ServerSelectWnd::WndNotification` @0x10038310.

The reply is **OP_PlayResponse**: base reply + server id u32, looked up in the current list for the world address. Errors:

| Error | Meaning |
|---|---|
| 111 | already online (EQEmu) |
| 326 | unavailable |
| 337 / 338 | suspended / banned |
| 339 | full |
| **302 / 114** | a character is still logged in and the server offers to remove it (`PlayResult::offer_kick()`), below |

#### 7. The kick

1. After 302 or 114 the login stays up and the player is asked the reply's text (Yes / No; `LoginState::KickOffered`).
2. No: back to the list.
3. Yes: the kick request **0x16**, header only, pending **60 s** with the server's id. "Please wait while the currently logged in player is removed from the world." (**6075**) shows meanwhile (`LoginState::Kicking`).
4. Its reply **0x32** is a base reply. Success re-sends the play request for the same server (10 s); failure shows the reply's error. A timeout is success 0, **103**.

EQEmu's login server sends neither 302 nor 114 and has no handler for 0x16, so this path is never taken there (its "already online" is 111, a plain refusal, only with `World:EnforceCharacterLimitAtLogin`).

Checked: eqmain ServerSelectWnd::OnJoinServer @0x10037350, request @0x100388dc (class 0x100209b0), reply handler @0x10037740 (LoginClient callback +0x78).

## Sequence numbers and pending requests

One counter per connect attempt starts at 1 and is incremented **before** each request, wrapping to 1 at 0xffffff: handshake 2, login 3, list 4, play 5. Each list refresh takes the next number.

Every request is pending with a timeout:

| Request | Timeout | On timeout |
|---|---|---|
| Handshake | 10 s | nothing happens |
| Login | 20 s | **103** "A timeout occurred" |
| List | 120 s | **103** |
| Play | 10 s | **324** |

A reply with sequence below 0xffffff must match a pending request (which it removes) or it is dropped, except a push opcode (0x25–0x2d, 0x30, 0x31). The opcode is not compared. EQEmu echoes the request's sequence. A connection the server drops after the login: **112**.

Error texts come from eqlsstr_us.txt (`load_login_strings`). Without the file, Hailstone has its own English wording for the ids it uses; an id with no text reads "Login message <id> (no text for it)."

The `LoginClient` state machine:

- `start()` → Connecting → Handshake → Authenticating → **LoggedIn**;
- `request_server_list()` → RequestingList → **ServerList**;
- `request_play(id)` → RequestingPlay → **PlayAccepted**;
- any error → Failed with `error()` and `error_id()`.

## Hand-off to the world server

`LoginClient::ticket()` returns a `WorldTicket` once the play request is accepted:

- `world_host`: the chosen server's IP string from the server list;
- `world_port`: 9000 (RoF2 always uses 9000);
- `account_id` and `session_key` (10 characters) from OP_LoginAccepted.

The world connection's first packet is OP_SendLoginInfo (0x7a09, 464 bytes): the account id as **decimal text**, `00`, the session key, `00`, …. EQEmu checks the pair with `ClientList::CheckAuth(id, key)`. The login session can be closed once the ticket is in hand.

## Launcher tickets

The playable client accepts a ticket supplied by a launcher:

```text
hailstone --game-dir /path/to/EverQuest patchme /ticket:ISSUED_TICKET
hailstone --server example --ticket-env LAUNCH_TICKET --world "Example World" --character Example
```

#### Ticket sources

- `--ticket T` is the explicit equivalent of `/ticket:T`.
- `--ticket-env VAR` reads the named environment variable once and removes it from the environment (`platform::take_environment_variable`), so child processes do not inherit it and the ticket stays off the command line.
- Supply exactly one ticket source.

As in RoF2, a ticket takes precedence over `--user`, `--password` and `--account`, which are cleared unread, so an existing launcher template works. Tickets cannot be used with a viewer mode.

The ticket is opaque; the issuer and login server enforce any restrictions. While a ticket is on the command line, unknown arguments are counted in the log but not named (a ticket split at a space would otherwise be logged).

#### Ticket endpoint

The endpoint is the first of these:

1. the explicit saved login-server profile `--server`;
2. the explicit `--host`/`--port`;
3. the `eqhost.txt` of a game folder named for this run (`--game-dir` or `EQ_GAME_DIR`);
4. the `eqhost.txt` of the EverQuest folder Hailstone is installed in (`<EQ>/hailstone/`, as a server's patcher puts it; RoF2 reads the `eqhost.txt` beside it).

Explicit host/port override the chosen endpoint. The asset folder remembered in `hailstone.ini` does not count (it may be another server's): without one of those the client refuses the ticket and sends nothing ("A login ticket needs its server named for this run: --server, --host, or --game-dir ..."). An unknown named profile also fails before sending.

A profile matching the endpoint supplies the server's assets and UI; with none, login is transient. An explicit host defaults to port 5997 (`ServerProfile`).

#### World and character

- Without `--world`, ticket authentication stops at server select.
- `--world NAME|ID` asks for exactly one world by case-insensitive full name or decimal login-server world id (a Hailstone option; RoF2 has none). A missing or ambiguous match leaves the list up with a message; no other world is substituted.
- `--character NAME`, or RoF2's `/name:NAME`, enters that character at character select without a click.
- Normal world admission, locked-server permissions and character checks apply.

#### RoF2 switches

RoF2's own switches (eqgame `InitGameInstance` @005fdec0, rof2decomp notes/connection-and-network.md §2.2):

| Switch | RoF2 | Hailstone |
|---|---|---|
| `/ticket:<t>` | @005fe56a: at most 63 bytes (strncpy 0x3f) to 0x00e666f8, one trailing `"` stripped | the ticket, one trailing `"` stripped; longer than 63 bytes is refused, not cut (a deliberate change) |
| `/name:<char>` | @005fe5e6: 63 bytes to 0x00e66738; `StartNetworkGame` selects that character, writes `Defaults/LastCharSel` and sends OP_EnterWorld without a click (@0053c0a5) | `--character` (at most 63 bytes); `LastCharSel` is not written |
| `/server:<host>` | @005fe5ba: the world host into `EQLogin` +0x58c, which eqmain may overwrite | taken and ignored: the world's address comes from the login server's play reply |
| `patchme` | @005fe54c: the started-by-the-patcher flag | taken and ignored |

#### Ticket login

`LoginClient::start_token` sends the token form of `OP_Login`, then follows the ordinary flow. Authentication starts while assets initialize, so a short-lived ticket is redeemed promptly.

A ticket launch skips `update_at_start` (its restart would lose the ticket); the updater and add-on fetching otherwise stay on.

#### What is kept and wiped

No ticket is saved to settings or the keyring. No password is read, saved or substituted for that attempt.

The application and login client wipe their own copies of the ticket after sending and on cancellation, failure, replacement and destruction, and the decrypted login reply and token fields when released.

Not wiped:

- the encrypted OP_Login (fixed DES key, so effectively plaintext) stays in the session's buffers until reused;
- the process's argument strings, and any copies the system keeps.

#### After a failure

A rejection, timeout or disconnect needs a fresh launcher ticket or an interactive password login. The login screen returns to normal: the saved server the ticket went to (else the one picked before), with its own account and saved password.

#### Replacement token

A successful reply can supply a replacement token, kept only in memory and bound to the issuing login host and port.

- After a normal logout, the login screen (every UI pack) offers **Sign in with session** when that endpoint is selected; a ticket's unprofiled server stays in the fields as a new, unsaved server.
- That action consumes the token once; each successful reply replaces it (an empty one clears it).
- A failure, a manual password login or exit forgets it; changing endpoints cannot consume it.
- The original launcher ticket is never put in this store or replayed.

#### Checked against RoF2

Checked against the RoF2 client (eqgame `0x5fdec0`, eqmain token API `0x10013850`, password API `0x10013720`, serializer `0x10020510`):

- case-insensitive `/ticket:`;
- a 64-byte launcher slot holding at most 63 ticket bytes;
- empty username/password strings followed by ticket, NUL and flag 1.

Hailstone's differences:

- The explicit `--ticket` and `--ticket-env` transports and network API permit up to 4096 opaque bytes, as a Hailstone extension. Embedded NULs and empty tickets are rejected.
- `--world` accepts 256 bytes.
- The token handshake has a bounded timeout (Hailstone policy); the password path keeps the native behaviour.
- Native login success replaces the stored ticket with the reply token automatically (eqmain `0x10009950`); Hailstone needs the explicit session sign-in, to avoid looping through failed token attempts.

#### Validation

Unit and mock-server tests cover packet bytes, launcher parsing, endpoint/world selection, token authentication through character select, rejection and timeout, token rotation and fragmented tickets. Not tested live against a real launcher. The launcher issues tickets and picks the executable; native-client verification and injection hooks do not apply.

## Live check (2026-10-21, EQEmu login server on 127.0.0.1:5997)

`HAILSTONE_LIVE_LOGIN=127.0.0.1:5997 HAILSTONE_LIVE_REFRESH=1 hailstone_tests -tc="live login*"`: login (sequence 3), list (4), list refresh after 60 s (5), play (6); the server answered with sequence 6 and the client zoned in.

## Live check (2026-10-20)

```
$ netprobe login 192.0.2.10:5997 nmsctest1 nmsctest1 "Dev"
connecting to 192.0.2.10 port 5997 as nmsctest1
login accepted: account id 2, session key ##########, failed attempts 0, show player count no
1 server(s):
  id   name                           address          status         players type
  2    Dev                            192.0.2.10   up             76      1
requesting play on 2 (Dev)
play accepted (server 2)
world connection: 192.0.2.10:9000, OP_SendLoginInfo login "2" password "##########"
```

The login server accepted the login and the play request (sequence 5).
