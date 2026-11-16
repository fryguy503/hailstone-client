# Web pages and the in-game browser

What opens web pages, who may open them, and the plan for Hailstone's in-game browser.

## The original

RoF2 shows web pages in its own HTML windows, built on Gecko (XULRunner: `xul.dll`, the `libMozilla::*` glue).
Three paths reach one:

- **OP_Weblink** (0x6f4b, zone → client, EQEmu's `SendWebLink` / `Weblink_Struct`: the address, NUL-terminated). The
  case @0x4c5b5d in `CEverQuest::HandleWorldMessage` takes the payload as a C string and calls
  `WebManager_CreateHtmlWnd` with it: an HTML window opens at once, with nothing asked.
- **/survey** (`Cmd_survey` @0x478b70): the survey page in `SurveyWebWnd`.
- **/url** and **/www** (`Cmd_url` @0x478520, `Cmd_www` @0x478420): 4095 "Opening %1" and `ShellExecuteA` "open":
  the system's browser, not an HTML window.

What `WebManager_CreateHtmlWnd` does with the address (a scheme check, the window's size and controls) is not in the
export (docs/unknowns.md "OP_Weblink").

## What Hailstone does now

| Path | Hailstone |
|---|---|
| OP_Weblink | A new tab in the browser window, as the original's HTML window opens at once. Without the helper (macOS, or not installed) the system's browser, with 4095 "Opening %1" in chat so the player sees why: a deliberate difference. Only http and https addresses (others are logged and dropped). A server's link is not the player's choice, so a flood (a quest looping on `SendWebLink`) is held to limits, a deliberate difference past what a quest plausibly sends (`app/server_links.h`): the same address again within 10 seconds is not opened again, at most 5 links open a minute (the rest are dropped, logged once), and once 4 of the links' tabs are open the newest is sent to the next address instead of a new tab. A tab is the links' only while it shows the address the last link sent it to: one the player browsed on from is the player's and is never taken over. The tab is the player's own (shown in the browser window, not held to a script's rules), as the original's HTML window is a browser the player uses, with two limits of Hailstone's: a link to this computer or the player's network (`web::private_host`: localhost, 192.168.x.x ...) is logged and not opened, and the links' tabs keep their cookies in `server-links/` in the server's folder, so a page the server picked does not run with the player's sign-ins. |
| /web [address] | Hailstone's own: the browser window, on the address in a new tab (no scheme: `https://`). The player's only; a script or plugin is told to use /url. Without the helper, the system's browser. |
| /url, /www | As the original for the player's own typing. For a script or plugin: needs the `web` permission, and the address must be http(s) on a host the server's `[web] allow` lets through (`core/web_url.h`), never this computer or a private network (`web/page_rules.h`, below). |
| The guild window's Go to URL | The guild's URL (what an officer set: other players' text) through `/url`, without `${...}` expansion, and only when it is http(s); one without a scheme gets `http://`, as `/www` does. A deliberate difference: what the original's `CGuildMgmtWnd` opens is not traced (docs/unknowns.md). Join Channel and Update MOTD send their text without `${...}` too (docs/plugins.md "MQ commands"). |
| /survey | The survey page (the account id's SHA-1 in it, as the original's) in the browser window, as the original's SurveyWebWnd; without the helper the system's browser, as OP_Weblink. |

The NMS and Project Triune plugins pass OP_Weblink on to the core. Their patches open it only after
`/browser on|external` (off by default: ignored); Hailstone opens it as the stock client does, a deliberate
difference from those patches' default.

### Who may open pages

- The `web` permission (`plugin::kCapWeb`, `HAILSTONE_CAP_WEB`, 4096) covers `/url` and `/www` run by a script or a plugin.
  It is part of `all`, so a server without a `hailstone.cfg`, and `player_caps` left at its default, grant it. A Lua
  social in press mode never has it (as with `ui`); one in full mode has it when its `caps` do.
- `hailstone.cfg`:

  ```ini
  [web]
  allow = example.org, wiki.example.net   ; hosts scripts and plugins may open, and those below them (default: any)
  ```

  The list applies to every script and plugin, the server's own included. It never limits the player's own `/url`, or
  OP_Weblink (the server sends that itself).
- Scripts and plugins never reach this computer or the player's own network, whatever `[web] allow` says
  (`web::private_host`, `web/page_rules.h`): `localhost` and `*.localhost`, a name without a dot or under `.local`,
  `.lan`, `.internal` or `.home.arpa`, and IPv4 and IPv6 literals in the loopback, private, link-local, shared
  (100.64/10), multicast and reserved ranges, in every form a browser reads (`2130706433`, `0x7f.1`, `0177.0.0.1`,
  `[::ffff:127.0.0.1]`, NAT64). This holds for `/url` and `/www` (the system's browser, with the player's own
  sign-ins) as for their tabs. Not caught: a public name whose DNS answer is a private address (docs/todo.md).
- Addresses a script, plugin or the server opens are `http://` or `https://` with a host and no spaces or control
  characters. `file:`, `data:`, `javascript:` and the rest are refused. A `\` ends the host as `/` does (browsers
  read it so: `https://evil.example\@good.org/` is evil.example), and a host name may hold only ASCII letters, digits,
  `-`, `_` and `.` (an international name in its `xn--` form; an IPv6 literal in brackets).

## The in-game browser

Decided 2026-10-30: CEF (the Chromium Embedded Framework, BSD) in off-screen mode, in a helper program of its own
that ships with the client. The player's scripts may have `web`, as above. Server links open at once, as in the
original.

```
app        BrowserWindow, hs.web (Lua), the plugin host's web functions
src/web/   browser_protocol.h: the messages; browser_host.h: the tabs, the helper's life
platform/  helper_process.h: the helper's pipes (SDL3's process I/O), binary stdio in the helper
render     one texture per tab, changed rectangles copied in each frame
tools/hailstone-web/   the helper: links CEF, nothing of the client's but browser_protocol.cpp
```

- **Why a helper.** The client never links CEF: its builds, the sanitizer build and CI stay as they are, and a page
  that crashes takes only the helper down. Without the helper every page opens in the system's browser.
- **Messages** (`web/browser_protocol.h`, version 1). The helper's standard input carries the client's (hello,
  open, navigate, resize, mouse, wheel, keys, text, focus, back / forward / reload / stop, run script, post a
  message, close, shut down); its standard output the helper's (ready, frame, popup, address, title, loading,
  cursor, console line, the page's message, closed). Each is a u32 length and a type byte, at most 64 MB; a broken
  one counts as the helper's death. Frames carry only the changed rectangles (CEF's `OnPaint` dirty rects), BGRA.
  Pixels go over the pipe for now; shared memory is the change to make if a full-screen page's frames turn out too
  slow that way. A popup (`OnPopupShow`, select lists) is a second, smaller image.
- **The helper's life** (`web/browser_host.h`). Started at the first tab. A helper that exits or sends a broken
  message is restarted a second later with every open tab at its last address and size; after four deaths within a
  minute the client gives up, closes the tabs and opens pages in the system's browser. One that answers the hello with
  another version is given up at once.
- **Input.** While the browser window has focus, keys and text go to the page and not to the key binds. The
  mouse is mapped to the page's pixels, the page's cursor comes back, and the window follows the UI scale. Gamepad:
  the stick moves a cursor, and the pack's on-screen keyboard types into the page.
- **Profiles.** Cookies and cache under `platform::data_dir()/web/<server>/`, one per server so one server's pages
  never see another's cookies, for the player's own tabs. Each script and plugin has a profile of its own below it,
  `hailstone-addons/<owner>-<hash>/` (`WebService::owner_profile`: the owner's name lower-cased, what is not a letter,
  digit, `-` or `_` as `_`, and its FNV-1a hash), and the helper a request context for each profile: an add-on's page
  never sees the player's sign-ins or another add-on's. The server's links (OP_Weblink) have `server-links/` below it. Nothing goes in the game folder. A generic user agent naming
  Hailstone; no hardware identifiers.
- **Scripts and plugins** (docs/plugins.md "Web pages"): `mq.web` for scripts, `hs.Web*` in a plugin's windows,
  `web_*` in the host table (ABI 3). The page sees only `window.hailstone.post()` and `window.hailstone.onmessage`,
  and only its tab's owner gets what it posts: a page never runs commands or reads the game itself.
- **Rules.** Tabs that scripts and plugins open follow the address rules above, at every navigation, not only the
  first: the host checks open and navigate, the helper refuses the rest (redirects, links), and a tab that reports
  an address outside its sites anyway is closed. Inside such a page the frames and every request (images, scripts,
  styles, `fetch`, its service workers') are held to the same sites and never reach a private address
  (`web::script_request_allowed`); `data:`, `blob:`, `about:blank` and `about:srcdoc` stay allowed, since they reach
  no network. Every tab is http(s); the player's own are not limited by host, and their frames and requests are not
  checked. Only a script's or plugin's tab hands the page's messages on (to its owner). Downloads are off.
- **Sizes.** A console line is cut to 4 KB (`kMaxConsoleText`, at a UTF-8 character) and a tab keeps its last 200;
  a page's `hailstone.post()` message is at most 1 MB (`kMaxPageMessage`: a longer one throws in the page), and its
  owner's queue holds at most 256 of them and 8 MB (`kMessageQueueBytes`), the oldest going first, with a log line
  once. The helper cuts and drops these itself; a Console or PageMessage longer than its limit is a broken message to
  the client, seen from its first bytes, so a page cannot make the client hold gigabytes. The other way the client
  holds itself to limits before it sends (`BrowserHost`): an address at most 2 MB (`kMaxUrl`, Chromium's own
  limit), a Post at most 1 MB (`kMaxPostMessage`, as the page's), and any message at most 64 MB; a longer one is
  refused and never sent, since the helper takes a message over its limit as a broken stream and quits (which a
  script could repeat until the client gave up on the browser). While 32 MB wait for the helper to read them
  (`BrowserHost::kSendBudget`), anything but the client's own Hello, Close, Shutdown and a restart's reopened tabs is
  refused (an `eval`, `text`, `post`, `navigate` or `open` returns false; the player's mouse, wheel, keys, focus and
  back/forward/reload/stop always go, a few bytes each, so a release is never lost; a script's or plugin's
  back/forward/reload/stop is refused like its other messages); an empty queue takes any
  message, so several large evals in one frame send the first and refuse the rest instead of piling up. A helper that
  takes none of what waits for 10 seconds (`kStallMs`) has stopped reading: it is ended and restarted as a dead one
  is. `HelperProcess` also ends one whose queue would pass 128 MB (`kMaxPending`), which the budget keeps out of
  reach. A script or plugin holds at most
  16 tabs (`WebService::kMaxTabsPerOwner`), closed ones included until it closes them; a closed tab's images are let
  go when it closes. A paint larger than 64 MB (a 4096x4096 tab's full frame is just over) goes from the helper as
  several Frames (`web::split_frame`: rectangles grouped, a too large one cut into bands of rows), so the protocol
  and its version are unchanged.
- **Rules that change.** The helper keeps a tab's sites for its life, so once a frame (`WebService::recheck`) a
  script's or plugin's tab is closed as Refused, its owner told why (`state().why`), when the owner no longer has
  `web` or the server's `[web] allow` is not the one the tab was opened with (the server's `hailstone.cfg` arriving
  at the zone-in after provisional rules, say). The owner may open it again under the new rules.

### The helper (`tools/hailstone-web`)

- `hailstone-web --data=<data_dir()/web>`: CEF's root cache folder, its log (`cef.log`), and each tab's profile
  folder below it. CEF writes nothing else (checked on Linux: nothing outside the folder changed).
- One program for all of CEF's processes. A reader thread blocks on standard input and posts each message to CEF's
  UI thread, where the off-screen callbacks run, so only that thread writes standard output.
- Each tab is an off-screen browser (`windowless_frame_rate` 30) with its own client: `OnPaint` sends the changed
  rectangles, popups are the second layer, address / title / loading / cursor / console lines go back as they
  change. A new window (`window.open`, `target=_blank`) opens in the same tab.
- `OnBeforeBrowse` holds every main-frame navigation to the tab's rules (`about:blank` and CEF's own error pages
  are let through), and, in a script's or plugin's tab, every frame's; a refusal is a console line. Such a tab's
  client is also its `CefResourceRequestHandler`: `OnBeforeResourceLoad` cancels a request off its sites (a console
  line, posted to the UI thread) and `OnResourceRedirect` sends a redirect off them to `about:blank`. Its profile's
  request context has a `CefRequestContextHandler` with the same rules for requests that come from no tab (service
  workers). Whether CEF passes WebSocket handshakes through these handlers was not checked (docs/todo.md).
- `window.hailstone` is made in each page's main frame by the render process (`OnContextCreated`): `post(text)`
  sends a process message to the helper, which hands it to the client; the client's `Post` calls
  `hailstone.onmessage(text)` if the page set one. Frames inside the page cannot post.
- Software rendering (`--disable-gpu`, `--disable-gpu-compositing`), no extensions, audio muted. On Linux without
  `DISPLAY` or `WAYLAND_DISPLAY`, Chromium's headless platform (`--ozone-platform=headless`).
- **The sandbox** (`--sandbox=auto|on|off`, from `hailstone.ini` `browser.sandbox`, default `auto`): on Linux the
  pages' processes run in Chromium's namespace sandbox wherever unprivileged user namespaces work (checked: the
  renderers in a user namespace of their own); `auto` leaves it off where `kernel.unprivileged_userns_clone` is 0,
  `user.max_user_namespaces` is 0 or Ubuntu's `kernel.apparmor_restrict_unprivileged_userns` is 1, since Chromium
  would exit there ("No usable sandbox"; the setuid `chrome-sandbox` is not shipped). Windows and macOS run without
  it for now: CEF's sandbox there needs its static sandbox library (and the static C runtime on Windows, where the
  client's is the DLL one) and, on macOS, its helper bundles.
- **Shutting down:** the client sends Shutdown and ends the helper's input, and gives it 3 s to close its tabs and
  run `CefShutdown`, which writes the cookies; then it ends it. A helper that died or garbled its messages is ended at
  once.
- **Not yet:** GPU rendering, audio, macOS.
- Built with CEF's flags (no exceptions, no RTTI, `-Werror`); the protocol and the address rules come from
  `hailstone_web_protocol`, built with the client's. MSVC uses the DLL runtime (`/MD`) for both.
- `libcef.so` comes with its symbols (1.4 GB); the install strips it to about 264 MB.

### The window (`ui/<pack>/browser.lua`, `app/lua_ui_web.cpp`, `ui/web_textures.h`)

- **Modern:** tabs along the top (the newest comes to the front; x closes one, + opens an empty one), back,
  forward, reload / stop, the address (Enter goes; a refused address is said under it), "Your browser" (the page in
  the system's browser), and the page filling the rest; the window is resized from its edges.
- **Gamepad:** the page with buttons down the right: Address (the on-screen keys), Back, Forward, Reload, Type into
  the page (the keys' text goes where the page has its cursor), Point at the page (the pad's cursor mode: the left
  stick moves the pointer, A clicks), Close the tab; LB / RB go through the tabs. In the More page.
- **The page:** `hs.WebView` draws the tab's texture and its popup, sends the mouse (moves with the held buttons,
  clicks, double clicks, the wheel, leaving) and, once clicked, the keyboard: the editing and arrow keys as keys,
  Ctrl / Cmd with a letter (copy, paste, select all), and typed text as characters. While a page has the keyboard the
  game's key binds are off (ImGui's keyboard capture) and SDL's text input is on (`Window::set_text_input`). The tab
  takes the view's size, in UI pixels.
- **Textures:** one BGRA8 texture per tab and layer; each frame the changed rectangles are copied through a staging
  buffer of the frame's slot into the frame's command buffer, before the UI draws. A texture that goes (a closed
  tab, a new size) is destroyed three frames later, since the UI may have drawn it in this frame already.

### Milestones

1. **Done:** OP_Weblink, `/url` and `/www` in the system's browser; the `web` permission; `[web] allow`.
2. **Done:** the helper's messages, the platform pipes and `BrowserHost`, tested against an in-memory helper and
   the stand-in helper program (`tests/mock_web_helper.cpp`: a test pattern, clicks, messages echoed, crashes) over
   real pipes; the decoder fuzzed. None of it needs CEF, so all of it runs in every build's `hailstone_tests`.
3. **Done for Linux (x64; arm64 and Windows were built by CI's build legs before they were dropped, docs/ci.md, not
   run here):** `tools/hailstone-web` on CEF's pinned
   minimal builds, in `browser/` of the build and the install (below). Open: macOS (CEF needs an app bundle with
   its helper apps and framework: `HAILSTONE_BROWSER` is off there), and a moment for CEF to write its cookies at
   shutdown (done since: below).
4. **Done (unit and window tests; not tried in a live session):** the browser window, BrowserWnd, in the Modern and
   Gamepad packs (below), `hs.Web*` for the packs (docs/dev-ui.md), OP_Weblink and `/web` in it. The texture path
   was seen once in the client on Linux (2026-10-30, the Modern window opened at the login screen by a local change
   that was not kept, Vulkan validation on: the page drawn, no validation messages).
5. **Done (unit tests, the stand-in helper):** scripts and plugins (docs/plugins.md "Web pages"): `plugin::WebAccess`
   (`app/web_service.h`) holds every caller to its own tabs, its `web` permission and the server's sites; `mq.web`,
   `hs.Web*` in a plugin's window, the host table's `web_*` (ABI 3; polled with `web_message` rather than a
   callback), the example plugin's `/webexample`. Tabs close as their owner stops or unloads.
6. **Done:** OP_Weblink and `/survey` in the in-game window, a "Web Browser" row in the Modern pack's EQ menu (and
   the gamepad pack's More page). Open: what of `CHtmlWnd` and `WebManager_CreateHtmlWnd` can be traced
   (docs/unknowns.md "OP_Weblink").
