# Web pages and the in-game browser

What opens web pages, who may open them, and Hailstone's in-game browser.

## The original

RoF2 shows web pages in its own HTML windows, built on Gecko (XULRunner: `xul.dll`, the `libMozilla::*` glue).
Three paths reach one:

- **OP_Weblink** (0x6f4b, zone → client, EQEmu's `SendWebLink` / `Weblink_Struct`: the address, NUL-terminated). The
  payload is taken as a C string and passed to `WebManager_CreateHtmlWnd`: an HTML window opens at once, with nothing
  asked. Checked: `CEverQuest::HandleWorldMessage` @0x4c5b5d.
- **/survey**: the survey page in `SurveyWebWnd`. Checked: `Cmd_survey` @0x478b70.
- **/url** and **/www**: 4095 "Opening %1" and `ShellExecuteA` "open", the system's browser, not an HTML window.
  Checked: `Cmd_url` @0x478520, `Cmd_www` @0x478420.

What `WebManager_CreateHtmlWnd` does with the address (a scheme check, the window's size and controls) is not in the
export (docs/unknowns.md "OP_Weblink").

## What Hailstone does now

| Path | Hailstone |
|---|---|
| OP_Weblink | A new tab in the browser window, opened at once as the original's. Only http and https (others are logged and dropped). Details below. |
| /web [address] | Hailstone's own: the address in a new browser tab (no scheme: `https://`). The player's only; a script or plugin is told to use /url. Without the helper, the system's browser. |
| /url, /www | As the original for the player's own typing. For a script or plugin: needs the `web` permission, and the address must be http(s) on a host the server's `[web] allow` lets through (`core/web_url.h`), never this computer or a private network (`web/page_rules.h`, below). |
| The guild window's Go to URL | The guild's URL through `/url`, without `${...}` expansion, only when http(s). |
| /survey | The survey page (the account id's SHA-1 in it, as the original's) in the browser window; without the helper the system's browser. |

#### OP_Weblink in detail

- **Without the helper** (macOS, or not installed): the system's browser, with 4095 "Opening %1" in chat so the
  player sees why. A deliberate difference.
- **Flood limits** (`app/server_links.h`), against a quest looping on `SendWebLink`. A deliberate difference, past
  what a quest plausibly sends:
  - the same address again within 10 seconds is not opened again;
  - at most 5 links open a minute (the rest are dropped, logged once);
  - once 4 of the links' tabs are open, the newest is sent to the next address instead of a new tab.
- **Whose tab.** A tab is the links' only while it shows the address the last link sent it to; once the player
  browses on from it, it is never taken over.
- **The player's tab, with two limits.** The tab is the player's (not held to a script's rules), as the original's
  HTML window is, except:
  - a link to this computer or the player's network (`web::private_host`: localhost, 192.168.x.x ...) is logged and
    not opened, and the tab (`TabRules::ServerLink`) stays off them after that too: the host and the helper keep it,
    its frames and every request its page makes (a redirect, `fetch`, a form's post, an image, its service workers')
    off this computer and the player's network at every step, though it may go to any public site;
  - the links' tabs keep their cookies in `server-links/` in the server's folder, so a page the server picked does
    not run with the player's sign-ins.

#### Go to URL in detail

The guild's URL is other players' text. One without a scheme gets `http://`, as `/www` does. A deliberate
difference: what the original's `CGuildMgmtWnd` opens is not traced (docs/unknowns.md). Join Channel and Update MOTD
send their text without `${...}` too (docs/plugins.md "MQ commands").

#### Server plugins

The NMS and Project Triune plugins pass OP_Weblink on to the core. Their patches open it only after
`/browser on|external` (off by default). Hailstone opens it as the stock client does, a deliberate difference from
those patches' default.

### Who may open pages

- The `web` permission (`plugin::kCapWeb`, `HAILSTONE_CAP_WEB`, 4096) covers `/url` and `/www` run by a script or a
  plugin. It is part of `all`, so a server without a `hailstone.cfg`, or with `player_caps` at its default, grants
  it. A Lua social in press mode never has it (as with `ui`); one in full mode has it when its `caps` do.
- `hailstone.cfg`:

  ```ini
  [web]
  allow = example.org, wiki.example.net   ; hosts scripts and plugins may open, and those below them (default: any)
  ```

  The list applies to every script and plugin, the server's own included. It never limits the player's own `/url`, or
  OP_Weblink.
- Scripts and plugins never reach this computer or the player's own network, whatever `[web] allow` says
  (`web::private_host`, `web/page_rules.h`). Blocked:
  - `localhost` and `*.localhost`;
  - a name without a dot, or under `.local`, `.lan`, `.internal` or `.home.arpa`;
  - IPv4 and IPv6 literals in the loopback, private, link-local, shared (100.64/10), multicast and reserved ranges,
    in every form a browser reads (`2130706433`, `0x7f.1`, `0177.0.0.1`, `[::ffff:127.0.0.1]`, NAT64).

  This holds for `/url` and `/www` as for their tabs. Not caught: a public name whose DNS answer is a private address
  (docs/todo.md).
- Addresses a script, plugin or the server opens are `http://` or `https://` with a host and no spaces or control
  characters. `file:`, `data:`, `javascript:` and the rest are refused.
  - A `\` ends the host as `/` does (browsers read it so: `https://evil.example\@good.org/` is evil.example).
  - A host name may hold only ASCII letters, digits, `-`, `_` and `.` (an international name in its `xn--` form; an
    IPv6 literal in brackets).

## The in-game browser

CEF (the Chromium Embedded Framework, BSD) in off-screen mode, in a helper program of its own that ships with the
client.

```
app        BrowserWindow, hs.web (Lua), the plugin host's web functions
src/web/   browser_protocol.h: the messages; browser_host.h: the tabs, the helper's life
platform/  helper_process.h: the helper's pipes (SDL3's process I/O), binary stdio in the helper
render     one texture per tab, changed rectangles copied in each frame
tools/hailstone-web/   the helper: links CEF, nothing of the client's but browser_protocol.cpp
```

- **Why a helper.** The client never links CEF, and a crashing page takes only the helper down. Without the helper
  every page opens in the system's browser.
- **Messages** (`web/browser_protocol.h`, version 2: an open carries the tab's rules, `web::TabRules`).
  - Client → helper, on its standard input: hello, open, navigate, resize, mouse, wheel, keys, text, focus,
    back / forward / reload / stop, run script, post a message, close, shut down.
  - Helper → client, on its standard output: ready, frame, popup, address, title, loading, cursor, console line,
    the page's message, closed.
  - Each is a u32 length and a type byte, at most 64 MB. A broken one counts as the helper's death.
  - Frames carry only the changed rectangles (CEF's `OnPaint` dirty rects), BGRA, over the pipe; shared memory is
    the change to make if full-screen frames turn out too slow.
  - A popup (`OnPopupShow`, select lists) is a second, smaller image.
- **The helper's life** (`web/browser_host.h`). Started at the first tab. A helper that exits or sends a broken
  message is restarted a second later, with every open tab at its last address and size. After four deaths within a
  minute the client gives up, closes the tabs and opens pages in the system's browser. One that answers the hello
  with another version is given up at once.
- **Profiles.** Nothing goes in the game folder.
  - The player's own tabs: cookies and cache under `platform::data_dir()/web/<server>/`.
  - Each script and plugin has a profile below it, `hailstone-addons/<owner>-<hash>/` (`WebService::owner_profile`:
    the owner's name lower-cased, other than letters, digits, `-` and `_` as `_`, plus its FNV-1a hash), with its
    own request context, so an add-on's page never sees the player's sign-ins or another add-on's.
  - The server's links (OP_Weblink) have `server-links/` below it.
  - A generic user agent naming Hailstone; no hardware identifiers.
- **Scripts and plugins** (docs/plugins.md "Web pages"): `mq.web` for scripts, `hs.Web*` in a plugin's windows,
  `web_*` in the host table (ABI 3). The page sees only `window.hailstone.post()` and `window.hailstone.onmessage`,
  and only its tab's owner gets what it posts: a page never runs commands or reads the game itself.
- **Rules.** Tabs that scripts and plugins open follow the address rules above at every navigation:
  - the host checks open and navigate; the helper refuses the rest (redirects, links); a tab that reports an address
    outside its sites anyway is closed;
  - inside such a page, the frames and every request (images, scripts, styles, `fetch`, its service workers') are
    held to the same sites and never reach a private address (`web::script_request_allowed`);
  - `data:`, `blob:`, `about:blank` and `about:srcdoc` stay allowed, since they reach no network.

  The player's own tabs (http(s) too) are not limited by host or checked. A page's messages go only to the script or
  plugin that opened its tab; on the player's own tabs they go nowhere. Downloads are off.
- **Rules that change.** The helper keeps a tab's sites for its life, so each frame (`WebService::recheck`) a
  script's or plugin's tab is closed as Refused, its owner told why (`state().why`), when:
  - the owner no longer has `web`, or
  - the server's `[web] allow` is not the one the tab was opened with (the server's `hailstone.cfg` arriving at the
    zone-in after provisional rules, say).

  The owner may open it again under the new rules.

#### Size limits

From the helper (it cuts and drops these itself):

| What | Limit |
|---|---|
| A console line | cut to 4 KB (`kMaxConsoleText`, at a UTF-8 character); a tab keeps its last 200 |
| A page's `hailstone.post()` message | at most 1 MB (`kMaxPageMessage`: a longer one throws in the page) |
| The owner's message queue | at most 256 messages and 8 MB (`kMessageQueueBytes`), the oldest going first, with a log line once |

A longer Console or PageMessage is a broken message to the client, so a page cannot make it hold gigabytes.

From the client (`BrowserHost`), checked before sending:

| What | Limit |
|---|---|
| An address | at most 2 MB (`kMaxUrl`, Chromium's own limit) |
| A Post | at most 1 MB (`kMaxPostMessage`, as the page's) |
| Any message | at most 64 MB |

A longer one is refused and never sent: the helper would take it as a broken stream and quit, which a script could
repeat until the client gave up on the browser.

Send budget:

- While 32 MB wait for the helper to read them (`BrowserHost::kSendBudget`), anything but the client's own Hello,
  Close, Shutdown and a restart's reopened tabs is refused. An `eval`, `text`, `post`, `navigate` or `open` returns
  false.
- The player's mouse, wheel, keys, focus and back/forward/reload/stop always go, so a release is never lost. A
  script's or plugin's back/forward/reload/stop is refused like its other messages.
- An empty queue takes any message, so of several large evals in one frame the first is sent and the rest refused.
- A helper that reads nothing for 10 seconds (`kStallMs`) is ended and restarted as a dead one is.
- `HelperProcess` also ends one whose queue would pass 128 MB (`kMaxPending`), which the budget keeps out of reach.

Tabs and frames:

- A script or plugin holds at most 16 tabs (`WebService::kMaxTabsPerOwner`), closed ones included until it closes
  them. A closed tab's images are let go when it closes.
- A paint larger than 64 MB (a 4096x4096 tab's full frame is just over) goes from the helper as several Frames
  (`web::split_frame`: rectangles grouped, a too large one cut into bands of rows), with the protocol unchanged.

### The helper (`tools/hailstone-web`)

- `hailstone-web --data=<data_dir()/web>`: CEF's root cache folder, its log (`cef.log`), and each tab's profile
  folder below it. CEF writes nothing else (checked on Linux).
- One program for all of CEF's processes. Each tab is an off-screen browser (`windowless_frame_rate` 30); a new
  window (`window.open`, `target=_blank`) opens in the same tab.
- **Navigation checks.**
  - `OnBeforeBrowse` holds every main-frame navigation to the tab's rules (`about:blank` and CEF's own error pages
    are let through), and, in a script's or plugin's tab, every frame's. A refusal is a console line.
  - In such a tab (its client is also its `CefResourceRequestHandler`), `OnBeforeResourceLoad` cancels a request off
    its sites (a console line) and `OnResourceRedirect` sends a redirect off them to `about:blank`.
  - Its profile's request context has a `CefRequestContextHandler` with the same rules for requests that come from
    no tab (service workers).
  - Whether CEF passes WebSocket handshakes through these handlers was not checked (docs/todo.md).
- `window.hailstone` is made in each page's main frame (`OnContextCreated`); the client's `Post` calls
  `hailstone.onmessage(text)` if the page set one. Frames inside the page cannot post.
- Software rendering (`--disable-gpu`, `--disable-gpu-compositing`), no extensions, audio muted. On Linux without
  `DISPLAY` or `WAYLAND_DISPLAY`, Chromium's headless platform (`--ozone-platform=headless`).
- **The sandbox** (`--sandbox=auto|on|off`, from `hailstone.ini` `browser.sandbox`, default `auto`).
  - On Linux the pages' processes run in Chromium's namespace sandbox wherever unprivileged user namespaces work.
  - `auto` leaves it off where `kernel.unprivileged_userns_clone` is 0, `user.max_user_namespaces` is 0 or Ubuntu's
    `kernel.apparmor_restrict_unprivileged_userns` is 1. Chromium would exit there ("No usable sandbox"; the setuid
    `chrome-sandbox` is not shipped).
  - Windows and macOS run without it for now: CEF's sandbox there needs its static sandbox library (and the static
    C runtime on Windows, where the client's is the DLL one) and, on macOS, its helper bundles.
- **Shutting down:** the client sends Shutdown, ends the helper's input and gives it 3 s to close its tabs and run
  `CefShutdown` (which writes the cookies), then ends it. A helper that died or garbled its messages is ended at once.
- **Not yet:** GPU rendering, audio, macOS.
- Built with CEF's flags (no exceptions, no RTTI, `-Werror`). The protocol and the address rules come from
  `hailstone_web_protocol`, built with the client's. MSVC uses the DLL runtime (`/MD`) for both.
- `libcef.so` comes with its symbols (1.4 GB); the install strips it to about 264 MB.

### The window (`ui/<pack>/browser.lua`, `app/lua_ui_web.cpp`, `src/ui/web_textures.h`)

- **Modern:** tabs along the top (the newest in front; x closes one, + opens an empty one), back, forward,
  reload / stop, the address (Enter goes; a refused address is said under it), "Your browser" (the page in the
  system's browser), and the page filling the rest. Resized from its edges.
- **Gamepad:** in the More page. The page, with buttons down the right:
  - Address (the on-screen keys), Back, Forward, Reload;
  - Type into the page (the keys' text goes where the page has its cursor);
  - Point at the page (the pad's cursor mode: the left stick moves the pointer, A clicks);
  - Close the tab.

  LB / RB go through the tabs.
- **The page:** `hs.WebView` draws the tab's texture and popup and sends the mouse and, once clicked, the keyboard.
  While a page has the keyboard the game's key binds are off and SDL's text input is on
  (`Window::set_text_input`). The tab takes the view's size, in UI pixels, and follows the UI scale.
- **Textures:** one BGRA8 texture per tab and layer. One that goes (a closed tab, a new size) is destroyed three
  frames later, since the UI may have drawn it this frame.

### Milestones

1. **Done:** OP_Weblink, `/url` and `/www` in the system's browser; the `web` permission; `[web] allow`.
2. **Done:** the messages, the platform pipes and `BrowserHost`, tested against an in-memory helper and the stand-in
   helper (`tests/mock_web_helper.cpp`) over real pipes, the decoder fuzzed. No CEF needed: runs in every build's
   `hailstone_tests`.
3. **Done for Linux x64 and Windows x64:** `tools/hailstone-web` on CEF's pinned minimal builds, in `browser/` of the
   build and the install. The Windows helper is built with MSVC under Wine (docs/ci.md); its browser tests do not
   run under Wine and are run on Windows by hand. Open: macOS (CEF needs an app bundle with its helper apps and
   framework: `HAILSTONE_BROWSER` is off there).
4. **Done (unit and window tests; not tried in a live session):** BrowserWnd in both packs, `hs.Web*` for the packs
   (docs/dev-ui.md), OP_Weblink and `/web` in it.
5. **Done (unit tests, the stand-in helper):** scripts and plugins (docs/plugins.md "Web pages"):
   `plugin::WebAccess` (`app/web_service.h`) holds every caller to its own tabs, its `web` permission and the
   server's sites; the host table's `web_*` (ABI 3) is polled with `web_message` rather than a callback; the example
   plugin's `/webexample`; tabs close as their owner stops or unloads.
6. **Done:** OP_Weblink and `/survey` in the in-game window, a "Web Browser" row in the Modern pack's EQ menu and
   the gamepad pack's More page. Open: what of `CHtmlWnd` and `WebManager_CreateHtmlWnd` can be traced
   (docs/unknowns.md "OP_Weblink").
