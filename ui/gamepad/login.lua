-- The login screen (gamepad pack): eq.LoginScreen stands in for the client's own (ui/dev_screens.cpp login_screen),
-- laid out around what a player does most, the rest a page away. A Hailstone screen, not the original's.
--   Main            the characters seen on this server's accounts as tiles (A plays one at once: its account, world
--                   and character), the server (LB / RB step through the saved ones and New server, A lists them),
--                   its interface (stepped in place, kept with the server), the account (username and password on
--                   the on-screen keys, the saved accounts in a list, Remember account, Save password) and Log in;
--                   Start logs in, or asks for the first field missing
--   Server settings the server's name, address, port, its own files folder, Save and Delete
--   More (Y)        the box set (each account boxed or not, its world and character, Forget), Log in box set, the
--                   game files, the add-ons, About Hailstone (What's new, its GitHub page), the music and Quit
--   Add-ons         a GitHub link to an add-on (docs/addons.md) and its install card, the store (every add-on of the
--                   add-on index: A on one installs it through its card), then every script and plugin: A on one
--                   lists what can be done with it and what it may do (the server's own: nothing that removes or
--                   stops them). The page follows view.addons.open (eq.LoginSet "addonsOpen").
--   What's new      this release's part of CHANGELOG.md (view.about.notes): A on an entry shows it whole
-- The build and the updater's word on it (view.about) sit in the screen's top left corner.
-- B goes back from a page, a list or the keys. A text is typed on the keys or, with a real keyboard, in the box over
-- them (Enter accepts, Escape cancels).
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local KF = dofile("lib/keyfield.lua")
local P = dofile("lib/pregame.lua")
local C = T.C

local NAME = "Login"
local WIDTH = 1180
local ROW_H, GAP = 64, 10
local st = {
  page = "main",      -- "main", "server", "more", "addons", "notes"
  edit = nil,         -- the keys up: { field, title, buf, password, digits, noBlank, back, submit, done }
  pick = nil,         -- a list to choose from: { title, items = { { label, sub, fn, current, disabled } }, back }
  confirm = nil,      -- W.confirm's question
  kb = KF.new(),
  list = { offset = 0 },
  addons = { offset = 0 }, -- the add-ons page's list
  store = { offset = 0 },  -- the add-ons page's store
  notes = { offset = 0 },  -- What's new's list
  note = nil,              -- What's new: the entry shown whole (its index)
  tiles = { offset = 0 },
  h = {},             -- each page's height (design px), measured as it was drawn last
  frame = -10,
}
local F = P.focus()

local function px(n) return T.px(n) end

local function serverName(v)
  if v.transient then return "Test login" end
  local s = v.servers[v.selected]
  return s and s.name or "New server"
end

local function packName(v, id)
  for _, p in ipairs(v.uiPacks) do
    if p.id == id then return p.name, p.description end
  end
  return id, ""
end

-- Shortens text with "..." from its start (a folder: the end is what tells them apart).
-- The first n (up to #text) whose "..." .. text:sub(n) fits, by halves: a shorter tail is never wider.
local function fitStart(text, w)
  if ui.CalcTextSize(text) <= w then return text end
  local lo, hi = 1, math.max(1, #text)
  while lo < hi do
    local mid = (lo + hi) // 2
    if ui.CalcTextSize("..." .. text:sub(mid)) > w then lo = mid + 1 else hi = mid end
  end
  return "..." .. text:sub(lo)
end

-- ---------------------------------------------------------------- the keys
-- Opens the keys for a field. e: field (eq.LoginSet's name, or nil with done), title, text, password, digits,
-- noBlank, capital, back (the item the focus returns to), submit (Enter logs in, as the client's fields do),
-- done(text) (after the field is set), byPad (Start opened it: the keys take the focus).
local function openEdit(e)
  e.buf = e.text or ""
  st.edit = e
  st.kb.shift = e.capital == true
  if e.byPad or P.padDrives() then KF.focus(st.kb) else e.typeFocus = true end
end

local function clean(e, t)
  if e.digits then t = t:gsub("%D", "") end
  if e.noBlank then t = t:gsub("%s", "") end
  local max = e.max or (e.digits and 5 or 64) -- (the client's keyboard keeps 64; a link up to 400)
  if #t > max then t = t:sub(1, max) end
  return t
end

local function closeEdit(accept, enter)
  local e = st.edit
  st.edit = nil
  F:want(e.back)
  if not accept then return end
  if e.field == "port" then
    local n = tonumber(e.buf)
    if n then eq.LoginSet("port", n) end
  elseif e.field then
    eq.LoginSet(e.field, e.buf)
  end
  if e.done then e.done(e.buf) end
  if enter and e.submit then eq.LoginDo("login") end
end

-- The field to fill before the login can go (the client's Start asks for it): host, username, password.
local function missing(v)
  if v.host == "" then
    return { field = "host", title = v.transient and "Server address" or "Address", noBlank = true, back = "##host",
      submit = true }
  end
  if v.username == "" then return { field = "username", title = "Username", back = "##user", submit = true } end
  return { field = "password", title = "Password", password = true, back = "##pass", submit = true }
end

local function drawEdit(x0, y0, x1)
  local e = st.edit
  T.font(24)
  local _, th = ui.CalcTextSize("Ag")
  T.text(x0, y0, e.title, C.dim)
  T.unfont()
  P.at(x0, y0 + th + px(8))
  ui.SetNextItemWidth(x1 - x0)
  if e.typeFocus then
    ui.SetKeyboardFocusHere()
    e.typeFocus = false
  end
  local flags = ui.InputTextFlags.EnterReturnsTrue
  if e.password then flags = flags | ui.InputTextFlags.Password end
  if e.digits then flags = flags | ui.InputTextFlags.CharsDecimal end
  T.font(26)
  local t, enter = ui.InputText("##loginedit", e.buf, (e.max or 64) + 1, flags, { moveToEnd = true })
  T.unfont()
  if t ~= e.buf then e.buf = clean(e, t) end
  P.at(x0, P.y() + px(10))
  local done = KF.draw(st.kb, function() return e.buf end, function(s) e.buf = clean(e, s) end, x0, x1, { keyHeight = 62 })
  if enter then closeEdit(true, true)
  elseif done == "done" or P.press("start") then closeEdit(true, false)
  elseif ui.IsKeyPressed(ui.Key.Escape) then closeEdit(false) end
end

-- ---------------------------------------------------------------- lists
local function openPick(title, items, back, currentLabel)
  st.pick = { title = title, items = items, back = back, currentLabel = currentLabel }
  local cur = 1
  for i, it in ipairs(items) do if it.current then cur = i end end
  local visible = math.min(#items, 8)
  st.list = { offset = math.max(0, math.min(cur - 1, #items - visible)) }
  st.list.focusNext = cur - st.list.offset
end

local function closePick()
  F:want(st.pick.back)
  st.pick = nil
end

local function drawPick(x0, y0, x1)
  local p = st.pick
  T.font(24)
  local _, th = ui.CalcTextSize("Ag")
  T.text(x0, y0, p.title, C.dim)
  T.unfont()
  P.at(x0, y0 + th + px(10))
  local items = p.items
  W.list("pick", st.list, #items, math.min(#items, 8), 72, x1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
    local it = items[i]
    local fa = it.disabled and 0.45 or 1
    local textR = rx1 - px(16)
    if it.current then
      textR = textR - P.pill(rx1 - px(16), (ry0 + ry1) / 2, p.currentLabel or "Selected", C.accent, 1) - px(12)
    end
    if it.sub and it.sub ~= "" then
      local mid = (ry0 + ry1) / 2
      T.textIn(rx0 + px(18), mid - th * 0.95, textR, mid, T.fit(it.label, textR - rx0 - px(18)), C.text, 0, fa)
      T.font(17)
      T.textIn(rx0 + px(18), mid, textR, mid + th * 0.8, fitStart(it.sub, textR - rx0 - px(18)), C.dim, 0, fa)
      T.unfont()
    else
      T.textIn(rx0 + px(18), ry0, textR, ry1, T.fit(it.label, textR - rx0 - px(18)), focused and C.text or C.text, 0, fa)
    end
  end, function(i)
    local it = items[i]
    if it.disabled then return end
    closePick()
    it.fn()
  end)
end

-- ---------------------------------------------------------------- servers
-- The saved servers in turn, then New server (0), and round again.
local function stepServer(v, dir)
  local n = #v.servers
  if v.transient or n == 0 then return end
  local i = (v.selected + dir) % (n + 1)
  if i == 0 then eq.LoginDo("new") else eq.LoginDo("select", i) end
end

local function serverList(v)
  local items = {}
  for i, s in ipairs(v.servers) do
    local where = s.host ~= "" and (s.host .. ":" .. s.port) or "No address yet"
    items[#items + 1] = { label = s.name, sub = where .. "  ·  " .. (packName(v, s.ui)),
      current = v.selected == i, fn = function() eq.LoginDo("select", i) end }
  end
  items[#items + 1] = { label = "New server", sub = "Type its address and port", current = v.selected == 0,
    fn = function()
      eq.LoginDo("new")
      st.page = "server"
      F:want("##name")
    end }
  return items
end

local function accountList(v)
  local items = {}
  for _, a in ipairs(v.accounts) do
    local sub = a.passwordSaved and "Password saved" or "No saved password"
    if a.character ~= "" then sub = a.character .. "  ·  " .. sub end
    items[#items + 1] = { label = a.username, sub = sub, current = a.username == v.username,
      fn = function() eq.LoginDo("account", a.username) end }
  end
  return items
end

-- ---------------------------------------------------------------- continue as
-- The characters as a row of tiles, three at a time; the D-pad past the last shown one moves the row on.
local TILE_H = 104

local function playTile(v, i)
  local q = v.characters[i]
  if q.passwordSaved then
    eq.LoginDo("play", i)
    return
  end
  -- (no saved password: the account goes in the fields, and the password typed now plays the character)
  eq.LoginSet("quick", i)
  eq.LoginDo("account", q.account)
  openEdit({ title = "Password for " .. q.account, password = true, back = "##tile" .. i,
    byPad = P.padDrives(), done = function(text)
      eq.LoginSet("password", text)
      if text ~= "" then eq.LoginDo("play", i) end
    end })
end

local function drawTiles(v, x0, y, x1)
  local n = #v.characters
  local s = st.tiles
  local shown = math.min(n, 3)
  s.offset = math.max(0, math.min(s.offset, n - shown))
  local gap = px(14)
  local tw = math.min((x1 - x0 - gap * (shown - 1)) / shown, px(420))
  local th = px(TILE_H)
  local moved = false
  for k = 1, shown do
    local i = s.offset + k
    local q = v.characters[i]
    P.at(x0 + (k - 1) * (tw + gap), y)
    local id = "##tile" .. i
    if s.focusNext == k then
      F:want(id)
      s.focusNext = nil
    end
    local clicked, focused, tx0, ty0, tx1, ty1 = P.button(F, id, "", tw, th, { disabled = v.busy })
    if q.last then T.rect(tx0, ty0 + px(14), tx0 + px(5), ty1 - px(14), C.accent, 1, px(3)) end
    local fa = v.busy and 0.45 or 1
    local ix0, ix1 = tx0 + px(20), tx1 - px(16)
    -- the badge: the password to type wins over "Last played"
    local badgeW = 0
    if not q.passwordSaved then badgeW = P.pill(ix1, ty0 + px(26), "Needs password", C.endurance, 1)
    elseif q.last then badgeW = P.pill(ix1, ty0 + px(26), "Last played", C.accent, 1) end
    T.font(27)
    local _, nh = ui.CalcTextSize("Ag")
    T.text(ix0, ty0 + px(12), T.fit(q.name, ix1 - ix0 - badgeW - px(10)), C.text, fa)
    T.unfont()
    T.font(19)
    local _, lh = ui.CalcTextSize("Ag")
    local what = q.level > 0 and ("Level " .. q.level .. (q.classes ~= "" and (" " .. q.classes) or "")) or q.classes
    T.text(ix0, ty0 + px(12) + nh + px(2), T.fit(what, ix1 - ix0), C.dim, fa)
    T.unfont()
    T.font(17)
    T.text(ix0, ty0 + px(12) + nh + lh + px(6), T.fit(q.world .. "  ·  " .. q.account, ix1 - ix0), C.faint, fa)
    T.unfont()
    if clicked then playTile(v, i) end
    if focused and not moved and ui.IsWindowFocused(3) and pad.connected() then
      if k == shown and i < n and pad.pressed("right") then
        s.offset, s.focusNext, moved = s.offset + 1, k, true
      elseif k == 1 and i > 1 and pad.pressed("left") then
        s.offset, s.focusNext, moved = s.offset - 1, 1, true
      end
    end
  end
  -- more to either side: an arrow at that end
  local cy = y + th / 2
  if s.offset > 0 then T.triangle("left", x0 - px(14), cy, px(8), C.dim, 1) end
  if s.offset + shown < n then T.triangle("right", x0 + shown * (tw + gap) - gap + px(14), cy, px(8), C.dim, 1) end
  if ui.IsMouseHoveringRect(x0, y, x1, y + th) then
    local wheel = ui.GetIO().wheel or 0
    if wheel ~= 0 then s.offset = math.max(0, math.min(n - shown, s.offset - (wheel > 0 and 1 or -1))) end
  end
  return th
end

-- ---------------------------------------------------------------- the interface
-- The UI pack the server is played with, stepped in place on the main page: the screen changes to it at once (the
-- client loads that pack, which may be another than this one), and a saved server keeps it at once.
local function interfaceRow(v, x0, y, w, h)
  P.at(x0, y)
  local pname, pdesc = packName(v, v.ui)
  local step = P.cycle(F, "##ui", "Interface", pname, w, h, { note = pdesc, disabled = v.busy or #v.uiPacks < 2 })
  if step then
    local cur = 1
    for i, p in ipairs(v.uiPacks) do if p.id == v.ui then cur = i end end
    local n = v.uiPacks[(cur - 1 + step) % #v.uiPacks + 1]
    if n then eq.LoginSet("ui", n.id) end
  end
end

-- ---------------------------------------------------------------- pages
local function heading(text, x0, y, w)
  P.at(x0, y)
  T.heading(text, w)
  return P.y()
end

-- Log in, or the first field still missing on the keys (as the client's Start does).
local function loginOrAsk(v, byPad)
  if v.busy then return end
  if v.ready then
    eq.LoginDo("login")
  else
    local e = missing(v)
    e.text, e.byPad = v[e.field] or "", byPad
    openEdit(e)
  end
end

-- The updater's news (v.update, docs/updater.md) under Log in: its sentence, then its buttons in one row.
local function updateRow(v, x0, y, x1, w)
  local u = v.update
  y = y + P.status({ text = u.text, error = u.state == "failed" }, false, x0, y, x1)
  local items = {}
  if u.canRestart then items[#items + 1] = { "##updrestart", "Restart now", "updateRestart" } end
  if u.canDownload then
    items[#items + 1] = { "##upddownload", u.state == "failed" and "Try again" or "Download", "updateDownload" }
  end
  if u.hasNotes then items[#items + 1] = { "##updnotes", "Release notes", "updateNotes" } end
  if u.canSkip then items[#items + 1] = { "##updskip", "Skip", "updateSkip" } end
  if #items == 0 then return y end
  local rh, gap = px(ROW_H), px(GAP)
  y = y + gap
  local bw = (w - gap * (#items - 1)) / #items
  P.at(x0, y)
  for i, it in ipairs(items) do
    if i > 1 then ui.SameLine(0, gap) end
    if P.button(F, it[1], it[2], bw, rh) then eq.LoginDo(it[3]) end
  end
  return y + rh
end

-- The picked server's suggested add-ons (v.addons.suggestions, docs/addons.md "Suggested add-ons") under Log in:
-- each one's sentence, then Look at it (the install card on the add-ons page), Not now and Don't ask again.
local function suggestionRows(v, x0, y, x1, w)
  if v.managed then return y end
  local rh, gap = px(ROW_H), px(GAP)
  for i, sg in ipairs(v.addons.suggestions or {}) do
    y = y + px(14)
    y = y + P.status({ text = sg.text, error = false }, false, x0, y, x1)
    y = y + gap
    local bw = (w - 2 * gap) / 3
    P.at(x0, y)
    if P.button(F, "##sglook" .. i, "Look at it", bw, rh, { primary = true, disabled = v.busy }) then
      eq.LoginDo("addonSuggestLook", sg.source)
    end
    ui.SameLine(0, gap)
    if P.button(F, "##sglater" .. i, "Not now", bw, rh, { disabled = v.busy }) then
      eq.LoginDo("addonSuggestLater", sg.source)
    end
    ui.SameLine(0, gap)
    if P.button(F, "##sgnever" .. i, "Don't ask again", bw, rh, { disabled = v.busy }) then
      eq.LoginDo("addonSuggestNever", sg.source)
    end
    y = y + rh
  end
  return y
end

local function mainPage(v, x0, y, x1, w)
  local rh, gap = px(ROW_H), px(GAP)
  local busy = v.busy
  -- the one-button way in
  if #v.characters > 0 then
    y = heading("Continue as", x0, y, w)
    y = y + drawTiles(v, x0, y, x1) + px(18)
  end

  y = heading("Server", x0, y, w)
  if v.transient then
    P.at(x0, y)
    local half = (w - gap) / 2
    if P.field(F, "##host", "Address", v.host, half, rh, { placeholder = "Type the address", disabled = busy, labelW = 130 }) then
      openEdit({ field = "host", title = "Server address", text = v.host, noBlank = true, back = "##host", submit = true })
    end
    ui.SameLine(0, gap)
    if P.field(F, "##port", "Port", tostring(v.port), half, rh, { disabled = busy, labelW = 100 }) then
      openEdit({ field = "port", title = "Port", text = tostring(v.port), digits = true, back = "##port", submit = true })
    end
  else
    local sw = px(250)
    P.at(x0, y)
    local note = v.host ~= "" and (v.host .. ":" .. v.port) or "No address yet"
    if P.field(F, "##server", "Playing on", serverName(v), w - sw - gap, px(76), { chevron = true, note = note, disabled = busy }) then
      openPick("Server", serverList(v), "##server")
    end
    ui.SameLine(0, gap)
    if P.button(F, "##settings", "Server settings", sw, px(76), { disabled = busy }) then
      st.page = "server"
      F:want("##name")
    end
    y = y + px(76) - rh
    y = y + rh + gap
    interfaceRow(v, x0, y, w, px(76))
    y = y + px(76) - rh
  end
  y = y + rh + px(18)

  y = heading("Account", x0, y, w)
  P.at(x0, y)
  local accounts = not v.transient and #v.accounts > 0
  local aw = accounts and px(250) or 0
  if P.field(F, "##user", "Username", v.username, w - (accounts and aw + gap or 0), rh,
        { placeholder = "Type your username", disabled = busy }) then
    openEdit({ field = "username", title = "Username", text = v.username, back = "##user", submit = true })
  end
  if accounts then
    ui.SameLine(0, gap)
    if P.button(F, "##accounts", "Saved accounts", aw, rh, { disabled = busy }) then
      openPick("Saved accounts", accountList(v), "##user")
    end
  end
  P.at(x0, y + rh + gap)
  local stars = string.rep("*", #v.password)
  if P.field(F, "##pass", "Password", stars, w, rh, { placeholder = "Type your password", disabled = busy }) then
    openEdit({ field = "password", title = "Password", password = true, back = "##pass", submit = true })
  end
  y = y + 2 * (rh + gap)
  if not v.transient then
    P.at(x0, y)
    local half = (w - gap) / 2
    if P.toggle(F, "##remember", "Remember account", v.remember, half, rh,
          { disabled = busy, note = "Keep the username in this server's list" }) then
      eq.LoginSet("remember", not v.remember)
    end
    ui.SameLine(0, gap)
    local why = not v.canSavePasswords and "No system keyring to keep it in"
      or (not v.remember and "Needs Remember account") or "Kept in the system keyring"
    if P.toggle(F, "##savepw", "Save password", v.savePassword, half, rh,
          { disabled = busy or not v.remember or not v.canSavePasswords, note = why }) then
      eq.LoginSet("savePassword", not v.savePassword)
    end
    y = y + rh + gap
  end
  y = y + px(8)

  -- Log in: greyed until the fields are filled, but A on it then asks for what is missing (as Start does)
  P.at(x0, y)
  local mw = px(300)
  local lh = px(80)
  F:aim("##login")
  T.font(28)
  local clicked, focused = T.button("##login", "Log in", w - mw - gap, lh,
    { primary = v.ready, disabled = busy, glyph = "start", align = 0.5 }, pad)
  T.unfont()
  F:track("##login", focused)
  if clicked and not busy then loginOrAsk(v, P.padDrives()) end
  ui.SameLine(0, gap)
  if P.button(F, "##more", "More options", mw, lh, { glyph = "y", disabled = busy }) then
    st.page = "more"
    F:want(nil)
  end
  y = y + lh
  if v.update then y = updateRow(v, x0, y + px(14), x1, w) end
  y = suggestionRows(v, x0, y, x1, w)
  return y
end

local function serverPage(v, x0, y, x1, w)
  local rh, gap = px(ROW_H), px(GAP)
  local busy = v.busy
  local picked = v.selected > 0
  P.at(x0, y)
  if P.field(F, "##name", "Name", v.name, w, rh, { placeholder = v.host ~= "" and v.host or "Shown in the server list",
        disabled = busy }) then
    openEdit({ field = "name", title = "Server name", text = v.name, back = "##name" })
  end
  P.at(x0, y + rh + gap)
  if P.field(F, "##host", "Address", v.host, w, rh, { placeholder = "Type the address", disabled = busy }) then
    openEdit({ field = "host", title = "Server address", text = v.host, noBlank = true, back = "##host", submit = true })
  end
  P.at(x0, y + 2 * (rh + gap))
  if P.field(F, "##port", "Port", tostring(v.port), w, rh, { disabled = busy }) then
    openEdit({ field = "port", title = "Port", text = tostring(v.port), digits = true, back = "##port", submit = true })
  end
  y = y + 3 * (rh + gap)
  P.at(x0, y)
  local files = picked and v.assetsDir or "A folder made when the server is saved"
  if P.field(F, "##files", "Server files", picked and fitStart(files, w - px(420)) or files, w, rh,
        { disabled = busy or not picked, note = picked and "A opens the folder: the server's own files go here" or nil }) then
    eq.LoginDo("openAssets")
  end
  y = y + rh + px(22)
  P.at(x0, y)
  local bw = (w - 2 * gap) / 3
  if P.button(F, "##save", "Save", bw, px(72), { primary = v.host ~= "", disabled = busy or v.host == "" }) then
    eq.LoginDo("save")
  end
  ui.SameLine(0, gap)
  if P.button(F, "##delete", "Delete", bw, px(72), { disabled = busy or not picked }) then
    local name = serverName(v)
    st.confirm = { "Delete " .. name .. "? Its folder moves to servers/.deleted/.", function()
      eq.LoginDo("delete")
      st.page = "main"
      F:want("##server")
    end }
  end
  ui.SameLine(0, gap)
  if P.button(F, "##back", "Back", bw, px(72), { glyph = "b", align = 0.5 }) then
    st.page = "main"
    F:want("##settings")
  end
  return y + px(72)
end

local function boxRows(v, x0, y, x1, w)
  local rh, gap = px(ROW_H), px(GAP)
  local busy = v.busy
  local shown = math.min(#v.accounts, 6)
  local fw = px(110)
  local cw = (w - px(330) - fw - 3 * gap) / 2
  for i = 1, shown do
    local a = v.accounts[i]
    P.at(x0, y)
    local note = a.canBox and (a.box and "In the box set" or "Not in the box set") or "Save its password to box it"
    if P.toggle(F, "##box" .. i, a.username, a.box, px(330), rh, { disabled = busy or not a.canBox, note = note }) then
      eq.LoginDo("box", a.username, not a.box)
    end
    ui.SameLine(0, gap)
    if P.field(F, "##world" .. i, "World", a.world ~= "" and a.world or "Last played", cw, rh,
          { labelW = 86, chevron = true, disabled = busy }) then
      local items = { { label = "Last played", sub = "The world it played last", current = a.world == "",
        fn = function() eq.LoginDo("world", a.username, "") end } }
      for _, wname in ipairs(v.worlds) do
        items[#items + 1] = { label = wname, current = a.world == wname,
          fn = function() eq.LoginDo("world", a.username, wname) end }
      end
      if #v.worlds == 0 then items[1].sub = "The worlds show here once this server has listed them (log in once)" end
      openPick("World for " .. a.username, items, "##world" .. i)
    end
    ui.SameLine(0, gap)
    if P.field(F, "##char" .. i, "Enters", a.character ~= "" and a.character or "Choose", cw, rh,
          { labelW = 96, chevron = true, disabled = busy }) then
      local items = { { label = "Choose", sub = "Pick it at character select", current = a.character == "",
        fn = function() eq.LoginDo("character", a.username, "") end } }
      for _, c in ipairs(a.characters) do
        if a.world == "" or c.world == a.world then
          local sub = (c.level > 0 and ("Level " .. c.level .. " " .. c.classes) or c.classes)
          if a.world == "" then sub = sub .. "  ·  " .. c.world end
          items[#items + 1] = { label = c.name, sub = sub, current = a.character == c.name,
            fn = function() eq.LoginDo("character", a.username, c.name) end }
        end
      end
      openPick("Character for " .. a.username, items, "##char" .. i)
    end
    ui.SameLine(0, gap)
    if P.button(F, "##forget" .. i, "Forget", fw, rh, { disabled = busy }) then
      st.confirm = { "Forget " .. a.username .. "? Its saved password leaves the keyring.", function()
        eq.LoginDo("forget", a.username)
        F:want(nil)
      end }
    end
    y = y + rh + gap
  end
  if #v.accounts > shown then
    T.font(17)
    T.text(x0, y, (#v.accounts - shown) .. " more accounts: the mouse's login screen lists them all", C.faint)
    local _, th = ui.CalcTextSize("Ag")
    T.unfont()
    y = y + th + gap
  end
  return y
end

local function noop() end

local function morePage(v, x0, y, x1, w)
  local rh, gap = px(ROW_H), px(GAP)
  local busy = v.busy
  if not v.transient and v.selected > 0 and #v.accounts > 0 then
    y = heading("Box set", x0, y, w)
    y = boxRows(v, x0, y, x1, w)
    if v.boxCount > 0 then
      P.at(x0, y)
      if P.button(F, "##loginbox", "Log in box set (" .. v.boxCount .. ")", w, px(72),
            { primary = v.boxReady, disabled = busy or not v.boxReady }) then
        eq.LoginDo("loginBox")
      end
      y = y + px(72) + gap
    end
    y = y + px(12)
  end

  y = heading("Game files", x0, y, w)
  P.at(x0, y)
  local cur = v.gameFolders[1]
  local label = cur and cur.label or "None found"
  local note = v.gameFolderFixed and "Set by --game-dir or EQ_GAME_DIR" or (cur and cur.path) or ""
  if P.field(F, "##game", "Playing with", label, w, px(76), { chevron = not v.gameFolderFixed, note = fitStart(note, w - px(260)),
        disabled = busy or v.gameFolderFixed }) then
    local items = {}
    for i, f in ipairs(v.gameFolders) do
      items[#items + 1] = { label = f.label, sub = f.usable and f.path or (f.path .. "  (not usable now)"),
        current = i == 1, disabled = not f.usable,
        fn = function() if i > 1 then eq.LoginDo("gameFolder", i) end end }
    end
    items[#items + 1] = { label = "Browse...", sub = "Pick another EverQuest folder", fn = function() eq.LoginDo("browse") end }
    openPick("Game files", items, "##game", "In use")
  end
  y = y + px(76) + px(22)

  -- the add-ons (docs/addons.md): a page of their own
  if not v.managed then
    local installed = 0
    for _, r in ipairs(v.addons.rows) do if r.group == "installed" then installed = installed + 1 end end
    P.at(x0, y)
    if P.field(F, "##addons", "Add-ons", installed == 1 and "1 installed" or (installed .. " installed"), w, px(76),
          { chevron = true, note = "Find and install extra windows and features", disabled = busy }) then
      eq.LoginSet("addonsOpen", true)
      F:want("##addonlink")
    end
    y = y + px(76) + gap
  end

  -- About Hailstone (view.about, docs/updater.md): What's new and its GitHub page
  local about = v.about
  P.at(x0, y)
  if P.field(F, "##about", "Hailstone", about.version, w, px(76), { chevron = true, note = about.text }) then
    local items = {
      { label = "What's new", sub = "What changed in this version", disabled = #about.notes == 0,
        fn = function()
          st.page, st.note = "notes", nil
          st.notes.offset, st.notes.focusNext = 0, 1
        end },
    }
    if about.page ~= "" then
      items[#items + 1] = { label = "Open Hailstone's GitHub page", sub = about.page,
        fn = function() eq.LoginDo("openProject") end }
    end
    if about.build ~= "" then items[#items + 1] = { label = "This build: " .. about.build, disabled = true, fn = noop } end
    openPick("Hailstone " .. about.version, items, "##about")
  end
  y = y + px(76) + gap

  P.at(x0, y)
  local half = (w - gap) / 2
  if P.toggle(F, "##music", "Music", v.music, half, rh, { disabled = busy, note = "On the login and character screens" }) then
    eq.LoginSet("music", not v.music)
  end
  ui.SameLine(0, gap)
  if P.button(F, "##quit", "Quit Hailstone", half, rh) then eq.LoginDo("quit") end
  y = y + rh + gap
  P.at(x0, y)
  if P.button(F, "##back", "Back", w, px(64), { glyph = "b", align = 0.5 }) then
    st.page = "main"
    F:want("##more")
  end
  return y + px(64)
end

-- ---------------------------------------------------------------- add-ons

-- Lines of dim text (or red, `bad`) wrapped across the page; returns the y under them.
local function lines(text, x0, y, x1, bad, max)
  if not text or text == "" then return y end
  return y + P.status({ text = text, error = bad }, false, x0, y, x1, { lines = max or 3 }) + px(4)
end

-- What can be done with a row, and what it may do (greyed out, for reading).
local function addonActions(r, back)
  local items = {}
  if r.canStart then
    items[#items + 1] = { label = "Start", sub = "It opens a window of its own", fn = function() eq.LoginDo("addonStart", r.name) end }
  end
  if r.group == "installed" then
    if r.canUpdate then
      local what = r.state == "approval" and "Approve and update" or r.state == "modified" and "Update anyway"
        or r.state == "failed" and "Try again" or "Update"
      items[#items + 1] = { label = what, sub = r.stateText, fn = function() eq.LoginDo("addonUpdate", r.name) end }
    end
    items[#items + 1] = { label = r.autoUpdate and "Stop updating by itself" or "Update by itself",
      sub = "Newer versions that ask for nothing more install at start",
      fn = function() eq.LoginDo("addonAuto", r.name, not r.autoUpdate) end }
    if r.page ~= "" then
      items[#items + 1] = { label = "Open its repository", sub = r.page, fn = function() eq.LoginDo("addonPage", r.name) end }
    end
    items[#items + 1] = { label = "Remove", sub = "Its own files go; yours in its folder stay", disabled = not r.canRemove,
      fn = function()
        st.confirm = { "Remove " .. r.name .. "? Its own files go; yours in its folder stay.", function()
          eq.LoginDo("addonRemove", r.name)
        end }
      end }
  end
  -- the server's compiled plugin: the player's word on it (docs/plugins.md "The server's compiled plugins ask first")
  if r.consent == "allowed" then
    items[#items + 1] = { label = "Don't allow it any more", sub = "It stops running on this server",
      fn = function() eq.LoginDo("addonRevoke", r.name) end }
  elseif r.consent == "denied" then
    items[#items + 1] = { label = "Ask me again", sub = "Your next visit asks whether it may run",
      fn = function() eq.LoginDo("addonAskAgain", r.name) end }
  end
  if r.folder ~= "" then
    items[#items + 1] = { label = "Open its folder", sub = r.folder, fn = function() eq.LoginDo("addonFolder", r.name) end }
  end
  if r.note ~= "" then items[#items + 1] = { label = r.note, disabled = true, fn = noop } end
  for _, c in ipairs(r.caps) do items[#items + 1] = { label = "May " .. c, disabled = true, fn = noop } end
  for _, c in ipairs(r.newCaps) do
    items[#items + 1] = { label = "The new version also asks to " .. c, disabled = true, fn = noop }
  end
  openPick(r.name .. (r.version ~= "" and (" " .. r.version) or ""), items, back)
end

local function addonsPage(v, x0, y, x1, w)
  local a = v.addons
  local rh, gap = px(ROW_H), px(GAP)
  if a.available then
    P.at(x0, y)
    if P.field(F, "##addonlink", "GitHub link", a.link, w, rh,
          { placeholder = "https://github.com/owner/repo", disabled = a.inspecting }) then
      openEdit({ field = "addonLink", title = "GitHub link to an add-on", text = a.link, noBlank = true, max = 400,
        back = "##addonlink", byPad = P.padDrives(),
        done = function(text) if text ~= "" then eq.LoginDo("addonInspect") end end })
    end
    y = y + rh + gap
  else
    y = lines(a.whyOff, x0, y, x1) + gap
  end
  if a.inspecting or a.status.text ~= "" then y = y + P.status(a.status, a.inspecting, x0, y, x1) + gap end

  local o = a.offer
  if o then
    y = heading(o.name .. " " .. o.version, x0, y, w)
    y = lines("A " .. o.kind .. (o.author ~= "" and (" by " .. o.author) or "") .. ", from " .. o.source
      .. (o.installedVersion ~= "" and ("  ·  installed now: " .. o.installedVersion) or ""), x0, y, x1)
    y = lines(o.description, x0, y, x1)
    if o.kind ~= "tool" then y = lines("It asks to: " .. table.concat(o.caps, "; ") .. ".", x0, y, x1, false, 4) end
    y = lines(o.key ~= "" and ("Signed with key " .. o.key .. ": later versions must carry it.")
      or "Not signed: later versions come from whoever controls the repository.", x0, y, x1)
    if o.kind == "plugin" then
      y = lines("A plugin is a program: these permissions limit what it does through Hailstone, not what it can do "
        .. "on your computer. Install only plugins you trust.", x0, y, x1, true)
    elseif o.kind == "tool" then
      y = lines("A tool is a program of its own: it runs on your computer with your rights, outside Hailstone, which "
        .. "starts it when you ask. Install only tools you trust.", x0, y, x1, true)
    end
    if o.older then
      y = lines("This is an older version than the one installed (" .. o.installedVersion .. "): installing it goes "
        .. "back to " .. o.version .. ".", x0, y, x1, true)
    end
    y = lines(o.blocked, x0, y, x1, true)
    P.at(x0, y)
    local bw = (w - 2 * gap) / 3
    local label = o.older and "Install the older version"
      or (o.installedVersion == "" and "Install" or "Install this version")
    if P.button(F, "##addoninstall", label, bw, px(64),
          { primary = o.blocked == "", disabled = o.blocked ~= "" }) then
      eq.LoginDo("addonInstall", o.older and "older" or nil)
      F:want("##addonlink")
    end
    ui.SameLine(0, gap)
    if P.button(F, "##addoncancel", "Cancel", bw, px(64)) then eq.LoginDo("addonCancel") end
    ui.SameLine(0, gap)
    if P.button(F, "##addonpage", "Repository", bw, px(64)) then eq.LoginDo("addonPage") end
    y = y + px(64) + gap
  end

  -- the store (view.addons.store, docs/addons.md "Finding add-ons")
  y = heading("Find add-ons", x0, y, w)
  if a.available and not a.browsed and not a.browsing then eq.LoginDo("addonBrowse") end
  local store = a.store
  if a.browsing then
    y = y + P.status({ text = "Finding add-ons..." }, true, x0, y, x1) + gap
  elseif #store == 0 then
    y = lines(a.available and (a.storeError ~= "" and a.storeError or "No add-ons are listed yet.") or a.whyOff,
      x0, y, x1) + gap
  else
    P.at(x0, y)
    W.list("store", st.store, #store, math.min(#store, 3), 72, x1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
      local it = store[i]
      local mid = (ry0 + ry1) / 2
      T.font(22)
      local _, th = ui.CalcTextSize("Ag")
      local tag, col = "Install", C.accent
      if it.state == "update" then tag = "Update"
      elseif it.state == "installed" then tag, col = "Installed", C.good
      elseif it.state == "manual" then tag, col = "In your folders", C.dim
      elseif it.state == "unavailable" then tag, col = "Not available", C.bad end
      local pw = P.pill(rx1 - px(16), mid, tag, col, 1)
      local right = rx1 - px(32) - pw
      local name = it.name .. (it.version ~= "" and ("  " .. it.version) or "") .. (it.kind ~= "" and ("  ·  " .. it.kind) or "")
      T.textIn(rx0 + px(18), mid - th, right, mid, T.fit(name, right - rx0 - px(18)), C.text, 0)
      T.unfont()
      T.font(17)
      local sub = it.state == "unavailable" and it.problem or it.description
      if it.worlds ~= "" then sub = "For " .. it.worlds .. (sub ~= "" and ("  ·  " .. sub) or "") end
      T.textIn(rx0 + px(18), mid, right, mid + th, T.fit(sub, right - rx0 - px(18)),
        it.state == "unavailable" and C.bad or C.dim, 0)
      T.unfont()
    end, function(i)
      local it = store[i]
      local items = {}
      if it.state == "install" or it.state == "update" then
        items[#items + 1] = { label = it.state == "install" and "Install" or ("Update to " .. it.version),
          sub = "What it is and what it asks to do: nothing is installed yet", disabled = a.inspecting,
          fn = function() eq.LoginDo("addonStoreLook", it.source) end }
      end
      if it.description ~= "" then items[#items + 1] = { label = it.description, disabled = true, fn = noop } end
      if it.worlds ~= "" then items[#items + 1] = { label = "Made for " .. it.worlds, disabled = true, fn = noop } end
      if it.author ~= "" then items[#items + 1] = { label = "By " .. it.author, disabled = true, fn = noop } end
      if it.installedVersion ~= "" then
        items[#items + 1] = { label = "You have " .. it.installedVersion, disabled = true, fn = noop }
      end
      if it.problem ~= "" then items[#items + 1] = { label = it.problem, disabled = true, fn = noop } end
      openPick(it.name .. (it.version ~= "" and (" " .. it.version) or ""), items, "##store")
    end)
    y = P.y() + gap
  end

  y = heading("Your scripts and plugins", x0, y, w)
  local rows = a.rows
  if #rows == 0 then
    y = lines("None yet.", x0, y, x1)
  else
    P.at(x0, y)
    local group = { installed = "Installed", manual = "In your folders",
      server = a.server ~= "" and ("From " .. a.server) or "From this server" }
    W.list("addons", st.addons, #rows, math.min(#rows, 3), 72, x1 - x0, function(i, rx0, ry0, rx1, ry1, focused)
      local r = rows[i]
      local mid = (ry0 + ry1) / 2
      T.font(22)
      local _, th = ui.CalcTextSize("Ag")
      local pw = P.pill(rx1 - px(16), mid, group[r.group] or r.group, r.group == "server" and C.endurance or C.accent, 1)
      local right = rx1 - px(32) - pw
      T.textIn(rx0 + px(18), mid - th, right, mid, T.fit(r.name .. (r.version ~= "" and ("  " .. r.version) or "")
        .. "  ·  " .. r.kind, right - rx0 - px(18)), C.text, 0)
      T.unfont()
      T.font(17)
      local sub = r.stateText ~= "" and r.stateText or r.note ~= "" and r.note or r.description
      local bad = r.state == "failed" or r.state == "blocked"
      T.textIn(rx0 + px(18), mid, right, mid + th, T.fit(sub, right - rx0 - px(18)), bad and C.bad or C.dim, 0)
      T.unfont()
    end, function(i) addonActions(rows[i], "##addonrows") end)
    y = P.y() + gap
  end

  P.at(x0, y)
  local bw = (w - 2 * gap) / 3
  if P.button(F, "##addoncheck", "Check for updates", bw, px(64), { disabled = not a.available }) then
    eq.LoginDo("addonCheck")
  end
  ui.SameLine(0, gap)
  if P.button(F, "##addonrefresh", "Refresh the list", bw, px(64), { disabled = not a.available or a.browsing }) then
    eq.LoginDo("addonBrowse", true)
  end
  ui.SameLine(0, gap)
  if P.button(F, "##back", "Back", bw, px(64), { glyph = "b", align = 0.5 }) then
    eq.LoginSet("addonsOpen", false)
    F:want("##addons")
  end
  return y + px(64)
end

-- ---------------------------------------------------------------- what's new
-- This release's part of CHANGELOG.md (view.about.notes): its entries, each on a row; A shows one whole.
local function notesPage(v, x0, y, x1, w)
  local notes = v.about.notes
  local gap = px(GAP)
  if st.note and notes[st.note] then
    y = lines(notes[st.note], x0, y, x1, false, 16) + gap
    P.at(x0, y)
    if P.button(F, "##noteback", "Back to the list", w, px(64), { glyph = "b", align = 0.5 }) then
      st.notes.focusNext = st.note - st.notes.offset
      st.note = nil
    end
    return y + px(64)
  end
  y = heading("What's new in Hailstone " .. v.about.version, x0, y, w)
  P.at(x0, y)
  W.list("notes", st.notes, #notes, math.min(#notes, 8), 72, x1 - x0, function(i, rx0, ry0, rx1, ry1)
    P.status({ text = notes[i] }, false, rx0 + px(18), ry0 + px(8), rx1 - px(18), { lines = 2 })
  end, function(i) st.note = i end)
  y = P.y() + gap
  P.at(x0, y)
  if P.button(F, "##back", "Back", w, px(64), { glyph = "b", align = 0.5 }) then
    st.page = "more"
    F:want("##about")
  end
  return y + px(64)
end

-- The screen's top left corner: this build and the updater's word on it (view.about), to read only (its buttons
-- are on the More page).
local CORNER_FLAGS = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoInputs | ui.WindowFlags.NoSavedSettings
  | ui.WindowFlags.NoFocusOnAppearing | ui.WindowFlags.NoBringToFrontOnFocus | ui.WindowFlags.AlwaysAutoResize
  | ui.WindowFlags.NoBackground
local CORNER_COLORS = { uptodate = "good", checking = "dim", newer = "gold", unknown = "faint", off = "faint" }
local function corner(v)
  local a = v.about
  if not a then return end
  ui.SetNextWindowPos(px(20), px(16), ui.Cond.Always)
  if ui.Begin("###gamepad.login.about", nil, CORNER_FLAGS) then
    T.resetFonts()
    T.font(24)
    local _, th = T.text(px(20), px(16), "Hailstone " .. a.version, C.text)
    ui.Dummy(ui.CalcTextSize("Hailstone " .. a.version), th)
    T.unfont()
    T.font(18)
    local col = C[CORNER_COLORS[a.status] or "faint"]
    local tw, sh = T.text(px(20), px(16) + th + px(2), a.text, col)
    ui.Dummy(tw, sh)
    T.unfont()
  end
  ui.End()
end

-- ---------------------------------------------------------------- the window
-- (the lists kept from frame to frame: tables made each frame are garbage the collector works through)
local HINTS_CONFIRM = { { "a", "Select" }, { "b", "Cancel" } }
local HINTS_PICK = { { "a", "Choose" }, { "b", "Back" } }
local HINTS_MAIN = { { "a", "Select" }, { "start", "Log in" }, { "y", "More" } }
local HINTS_MAIN_SERVERS = { { "a", "Select" }, { "start", "Log in" }, { "y", "More" }, { "lb", "" }, { "rb", "Server" } }
local HINTS_SERVER = { { "a", "Select" }, { "start", "Log in" }, { "b", "Back" } }
local HINTS_OTHER = { { "a", "Select" }, { "b", "Back" } }

local function hints(v)
  if st.confirm then return HINTS_CONFIRM end
  if st.edit then
    local h = {}
    for _, x in ipairs(KF.hints) do h[#h + 1] = x end
    h[#h + 1] = { "start", "Done" }
    h[#h + 1] = { "b", "Cancel" }
    return h
  end
  if st.pick then return HINTS_PICK end
  if st.page == "main" then
    return (not v.transient and #v.servers > 0) and HINTS_MAIN_SERVERS or HINTS_MAIN
  end
  if st.page == "server" then return HINTS_SERVER end
  return HINTS_OTHER
end

-- Where the focus goes as the screen appears: the character played last, else the first field to fill, else Log in.
local function firstFocus(v)
  if #v.characters > 0 then
    local at = v.quick > 0 and v.quick or 1
    for i, q in ipairs(v.characters) do if q.last and v.quick == 0 then at = i end end
    st.tiles.offset = math.max(0, math.min(at - 1, #v.characters - math.min(#v.characters, 3)))
    st.tiles.focusNext = at - st.tiles.offset
    return
  end
  if v.host == "" then F:want(v.transient and "##host" or "##settings")
  elseif v.username == "" then F:want("##user")
  elseif v.password == "" then F:want("##pass")
  else F:want("##login") end
end

-- The pad's and the keyboard's shortcuts for the page (not while the keys, a list or a question is up).
local function shortcuts(v)
  local pageBack = { server = "##settings", more = "##more" }
  if st.confirm then
    if P.press("b") or ui.IsKeyPressed(ui.Key.Escape) then st.confirm = nil end
    return
  end
  if st.edit then
    if P.press("b") then closeEdit(false) end
    return
  end
  if st.pick then
    if P.press("b") or ui.IsKeyPressed(ui.Key.Escape) then closePick() end
    return
  end
  -- (the keyboard's Enter and Escape: only while this window has the focus and nothing is being typed)
  local keys = W.focused() and not ui.GetIO().typing
  if P.press("start") then loginOrAsk(v, true) end
  if keys and ui.IsKeyPressed(ui.Key.Enter) and v.ready then eq.LoginDo("login") end
  if st.page == "main" then
    -- (B drops ImGui's focus in a window like this one: it goes back where it was)
    if P.press("b") then F:want(F.last) end
    if P.press("y") and not v.busy then
      st.page = "more"
      F:want(nil)
    end
    if not v.busy then
      if P.press("lb") then stepServer(v, -1) end
      if P.press("rb") then stepServer(v, 1) end
    end
  elseif st.page == "addons" then
    if P.press("b") or (keys and ui.IsKeyPressed(ui.Key.Escape)) then
      eq.LoginSet("addonsOpen", false)
      F:want("##addons")
    end
  elseif st.page == "notes" then
    if P.press("b") or (keys and ui.IsKeyPressed(ui.Key.Escape)) then
      if st.note then
        st.notes.focusNext = st.note - st.notes.offset
        st.note = nil
      else
        st.page = "more"
        F:want("##about")
      end
    end
  elseif P.press("b") or (keys and ui.IsKeyPressed(ui.Key.Escape)) then
    F:want(pageBack[st.page])
    st.page = "main"
  end
end

local OPTS = { w = WIDTH, close = false, alwaysFocus = true }
local held = nil -- the view drawn last frame: eq.LoginScreen gives true while it still holds (its revision)

ui.window(NAME, function()
  local v = eq.LoginScreen and eq.LoginScreen(held and held.revision)
  if v == true then v = held else held = v end
  if not v then
    st.frame = -10
    return
  end
  local frame = ui.GetFrameCount()
  local appearing = frame - st.frame > 1
  st.frame = frame
  if appearing then
    st.page, st.edit, st.pick, st.confirm = "main", nil, nil, nil
    firstFocus(v)
    -- testing: test.login = server, more, notes, addons, keys or servers in ui/settings/gamepad.txt opens that page
    -- (or the keys, or the server list) as the screen first appears, and again when the screen appears after the
    -- setting changed, for screenshots and the tests
    local test = ui.GetSetting("test.login") or ""
    if test == st.tested then test = "" else st.tested = test end
    if test == "server" or test == "more" or test == "notes" then st.page = test
    elseif test == "addons" then eq.LoginSet("addonsOpen", true)
    elseif test == "keys" then openEdit({ field = "username", title = "Username", text = v.username, back = "##user", byPad = true })
    elseif test == "servers" and not v.transient then openPick("Server", serverList(v), "##server") end
  end
  if v.transient and st.page == "server" then st.page = "main" end
  -- the add-ons page follows view.addons.open (the client keeps it, as the other pack's card)
  if not v.managed and v.addons.open and st.page ~= "addons" then st.page = "addons"
  elseif (v.managed or not v.addons.open) and st.page == "addons" then st.page = "more" end
  if v.busy then st.edit, st.pick = nil, nil end -- (nothing can change while the server answers)

  -- (a question keeps the page's size: it is drawn over it)
  local key = st.edit and "edit" or st.pick and "pick" or st.page
  local h = st.h[key] or 860
  local opts = OPTS
  opts.h, opts.hints = math.min(1040, h), hints(v)
  if W.begin(NAME, opts) then
    local x0, y0, x1, _, w = W.area()
    shortcuts(v)
    -- the wordmark and what is picked (the header of every page)
    T.font(46)
    local _, th = ui.CalcTextSize("Ag")
    T.text(x0, y0, "Hailstone", C.text)
    local ww = ui.CalcTextSize("Hailstone")
    T.unfont()
    local sub = st.page == "server" and ("Server settings  ·  " .. serverName(v))
      or st.page == "more" and "More options" or st.page == "addons" and "Add-ons"
      or st.page == "notes" and "What's new" or serverName(v)
    T.font(22)
    local _, sh = ui.CalcTextSize("Ag")
    T.text(x0 + ww + px(18), y0 + th - sh - px(6), T.fit(sub, x1 - x0 - ww - px(18)), C.dim)
    T.unfont()
    local y = y0 + th + px(12)
    ui.DrawLine(x0, y, x1, y, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
    y = y + px(16)

    local bottom
    if W.confirm(st) then
      bottom = y + px(260)
    elseif st.edit then
      drawEdit(x0, y, x1)
      bottom = P.y()
    elseif st.pick then
      drawPick(x0, y, x1)
      bottom = P.y()
    elseif st.page == "server" then
      bottom = serverPage(v, x0, y, x1, w)
    elseif st.page == "more" then
      bottom = morePage(v, x0, y, x1, w)
    elseif st.page == "addons" then
      bottom = addonsPage(v, x0, y, x1, w)
    elseif st.page == "notes" then
      bottom = notesPage(v, x0, y, x1, w)
    else
      bottom = mainPage(v, x0, y, x1, w)
    end
    bottom = bottom + px(16)
    bottom = bottom + P.status(v.status, v.busy, x0, bottom, x1)
    -- the window's height for this page next frame: its body, the hints and the padding
    local _, wy = ui.GetWindowPos()
    if not st.confirm then st.h[key] = (bottom - wy + px(24) + px(48) + px(14)) / T.S end
    F:tick()
  end
  W.finish()
  corner(v)
end)
