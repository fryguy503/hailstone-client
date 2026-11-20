-- Login: the client's login screen in this UI's look (hs.LoginScreen, app/lua_ui_pregame.cpp; a Hailstone screen, not
-- the original's). Built around what a player does most: pick a saved server, type or pick an account, Log in. The
-- server's details (name, address, port, its files) fold away under Edit once it is saved; the interface is picked
-- below the login controls; the game files and the texture pack sit under More options; the characters seen
-- on the server's accounts are a card of their own beside the login card (below it on a narrow screen), each played
-- with one click.
-- Add-ons (docs/addons.md) open beside the characters card when the row fits, or on their own, with the store
-- first; the box set opens as a card of its own on the login card's other side. The build, the
-- updater's word on it, What's new and Hailstone's page sit in a small card in the screen's top left corner.
-- Everything the client's own screen (ui/dev_screens.cpp login_screen) offers is here.
local API = hs.UI
local P = dofile("lib/pregame.lua")
local T = P.T
local C = T.color

local ENTER = ui.InputTextFlags.EnterReturnsTrue

local editing = false   -- the saved server's details unfolded
local editingFor = nil  -- the server they were unfolded for (picking another folds them)
local port = { active = false, text = "" } -- the Port field's own text while it is typed in (it may be empty then)
local forgetName = nil  -- the account the Forget question is about
local more = false      -- More options unfolded
local packAtStart = nil -- the texture pack as the screen first showed it (a change is read at the next start)
local boxOpen = false   -- the box set card open
local removeName = nil   -- the add-on the Remove question is about
local removeAsk = false  -- a row's Remove was pressed: the question opens at the window's level
local addonTab = nil    -- the add-ons card's tab to select next frame ("store" or "mine"), nil: as it is
local testedTab = nil   -- test.addontab as last applied
local notesTested = false -- test.whatsnew applied
local held = nil        -- the view drawn last frame: hs.LoginScreen gives true while it still holds (its revision)

-- (built once: tables and long texts made each frame are garbage the collector works through)
local EDIT, DONE, CLOSE, LOOK, ADDONS_QUIT = { "Edit" }, { "Done" }, { "Close" }, { "Look" }, { "Add-ons", "Quit" }
local NO_KEYRING_TIP = "There is no system keyring Hailstone can use (libsecret, the Credential Manager or the "
  .. "Keychain), so no password is saved."
local KEYRING_TIP = "Kept in the system's keyring once the server takes it, never in Hailstone's folder.\n"
  .. "Needed for an account in the box set."
local BOX_LOGIN_TIP = "This window logs in the first account, and a new window starts for each of the others. Each "
  .. "enters its character."
local PLUGIN_WARNING = "A plugin is a program: these permissions limit what it does through Hailstone, not what it "
  .. "can do on your computer. Install only plugins you trust."
local TOOL_WARNING = "A tool is a program of its own: it runs on your computer with your rights, outside Hailstone, "
  .. "which starts it when you ask. Install only tools you trust."
local UPSCALER_WARNING = "An upscaler runs inside Hailstone with your rights: it sees only the frames it upscales "
  .. "through Hailstone, but it is a program and can do anything on your computer. Install only upscalers you trust. "
  .. "Pick it under Options, Graphics, Upscaler; it starts the next time the game does."
local UI_TIP_SAVED = "The look and layout of the game's windows on this server, kept with it. The screen changes to it now."
local UI_TIP_NEW = "The look and layout of the game's windows on this server, kept with it once it is saved. The "
  .. "screen changes to it now."
local DELETE_NOTE = "Its folder (the server's files, maps and the characters' files) moves to servers/.deleted/, from "
  .. "where it can be brought back by hand."

local function packName(view, id)
  for _, p in ipairs(view.uiPacks) do
    if p.id == id then return p.name end
  end
  return id
end

-- servers/<id>/assets of the server's files folder (the whole path is the tooltip).
local function shortPath(path)
  return path:match("([^/\\]+[/\\][^/\\]+[/\\][^/\\]+)[/\\]?$") or path
end

-- A text field showing the view's `key`; an edit goes to the client at once. Returns whether Enter was pressed.
local function field(view, id, key, hint, flags)
  local text, enter = ui.InputTextWithHint(id, hint, view[key], 256, (flags or 0) | ENTER)
  if text ~= view[key] then
    API.pregame.LoginSet(key, text)
    view[key] = text
    held = nil -- (written here: the next frame reads the client's view whole again)
  end
  return enter
end

-- The Port field: its own text while typed in, so it can be cleared to type another number.
local function portField(view)
  local shown = port.active and port.text or tostring(view.port)
  local text, enter = ui.InputTextWithHint("##port", "Port", shown, 5, ui.InputTextFlags.CharsDecimal | ENTER)
  port.active, port.text = ui.IsItemActive(), text
  local n = tonumber(text)
  if n and n ~= view.port then API.pregame.LoginSet("port", math.floor(n)) end
  return enter
end

-- A muted label level with the text of the field beside it (ImGui's AlignTextToFramePadding is not bound: the
-- card's frame padding is 5 * s).
local function label(text, s)
  local x, y = ui.GetCursorPos()
  ui.SetCursorPos(x, y + 5 * s)
  P.Muted(text)
end

-- A two-column row of the server's details: a muted label, then the control (the rest of the width).
local function detail(text, s, draw)
  ui.TableNextRow()
  ui.TableNextColumn()
  label(text, s)
  ui.TableNextColumn()
  ui.SetNextItemWidth(-1)
  return draw()
end

-- ---------------------------------------------------------------- the server
local function serverDetails(view, s)
  local submit = false
  local picked = view.selected > 0
  if ui.BeginTable("##server", 2, ui.TableFlags.SizingFixedFit) then
    ui.TableSetupColumn("label", ui.TableColumnFlags.WidthFixed, 74 * s)
    ui.TableSetupColumn("value", ui.TableColumnFlags.WidthStretch, 0)
    if not view.transient then
      detail("Name", s, function() field(view, "##name", "name", view.host ~= "" and view.host or "Server name") end)
    end
    detail("Address", s, function()
      ui.SetNextItemWidth(ui.GetContentRegionAvail() - 78 * s)
      submit = field(view, "##host", "host", "login.example.com") or submit
      ui.SameLine()
      ui.SetNextItemWidth(70 * s)
      submit = portField(view) or submit
    end)
    if not view.transient then
      detail("Files", s, function()
        local x, y = ui.GetCursorPos()
        ui.SetCursorPos(x, y + 5 * s) -- (level with the label)
        if picked then
          ui.PushTextWrapPos(0)
          P.Faint(shortPath(view.assetsDir))
          ui.PopTextWrapPos()
          P.Tip(view.assetsDir .. "\nThe server's own client files, found before the game files.")
          if ui.Button("Open folder") then API.pregame.LoginDo("openAssets") end
        else
          ui.PushTextWrapPos(0)
          P.Faint("A folder for the server's own files is made when it is saved.")
          ui.PopTextWrapPos()
        end
      end)
    end
    ui.EndTable()
  end
  if not view.transient then
    ui.BeginDisabled(view.host == "")
    if ui.Button(picked and "Save changes" or "Save server") then API.pregame.LoginDo("save") end
    ui.EndDisabled()
    P.Tip("Keep the server's name, address, port and interface.")
    if picked then
      ui.SameLine()
      if P.Danger("Delete server", 0, 0, true) then ui.OpenPopup("##deleteserver") end
    end
  end
  return submit
end

local function serverSection(view, s)
  local submit = false
  P.Heading("Server")
  if view.transient then return serverDetails(view, s) end
  if editingFor ~= view.selected then editing, editingFor = false, view.selected end
  local picked = view.selected > 0
  local editW = math.max(P.ButtonsWidth(EDIT, s), P.ButtonsWidth(DONE, s))
  if picked then ui.SetNextItemWidth(ui.GetContentRegionAvail() - editW - 8 * s) else ui.SetNextItemWidth(-1) end
  local preview = picked and view.servers[view.selected].name or "New server"
  if ui.BeginCombo("##servers", preview, ui.ComboFlags.HeightLarge) then
    for i, sv in ipairs(view.servers) do
      ui.PushID(i)
      if ui.Selectable(sv.name, i == view.selected) and i ~= view.selected then API.pregame.LoginDo("select", i) end
      P.Tip(sv.host .. ":" .. sv.port)
      ui.PopID()
    end
    if #view.servers > 0 then ui.Separator() end
    if ui.Selectable("+ New server", not picked) and picked then API.pregame.LoginDo("new") end
    ui.EndCombo()
  end
  if picked then
    ui.SameLine()
    if ui.Button(editing and "Done" or "Edit", editW, 0) then editing = not editing end
    P.Tip(editing and "Fold the server's details away" or "The server's name, address, port and files")
  end
  if picked and not editing then
    P.Faint(string.format("%s:%d", view.host, view.port))
  else
    ui.Dummy(0, 2 * s)
    submit = serverDetails(view, s)
  end
  return submit
end

-- ---------------------------------------------------------------- the account
local function accountSection(view, s, appearing)
  local submit = false
  P.Heading("Account")
  local accounts = not view.transient and #view.accounts > 0
  local _, lh = ui.CalcTextSize("Ag")
  local fh = lh + 10 * s -- a field's height (the card's frame padding)
  if accounts then ui.SetNextItemWidth(ui.GetContentRegionAvail() - fh - 8 * s) else ui.SetNextItemWidth(-1) end
  if appearing and view.username == "" then ui.SetKeyboardFocusHere() end
  submit = field(view, "##user", "username", "Username") or submit
  -- a saved account typed in full counts as picked (its saved password comes with it)
  if ui.IsItemDeactivatedAfterEdit() and view.password == "" and view.username ~= "" then
    API.pregame.LoginDo("account", view.username)
  end
  if accounts then
    ui.SameLine()
    ui.SetNextItemWidth(fh) -- only the arrow: the server's saved accounts
    if ui.BeginCombo("##accounts", "") then
      for _, a in ipairs(view.accounts) do
        ui.PushID(a.username)
        local label = a.username .. (a.character ~= "" and ("  (" .. a.character .. ")") or "")
        if ui.Selectable(label, a.username == view.username) then API.pregame.LoginDo("account", a.username) end
        P.Tip(a.passwordSaved and "Password saved" or "No password saved")
        ui.PopID()
      end
      ui.EndCombo()
    end
    P.Tip("This server's saved accounts")
  end
  ui.SetNextItemWidth(-1)
  if appearing and view.username ~= "" then ui.SetKeyboardFocusHere() end
  submit = field(view, "##pass", "password", "Password", ui.InputTextFlags.Password) or submit
  if not view.transient then
    local v, changed = ui.Checkbox("Remember account", view.remember)
    if changed then API.pregame.LoginSet("remember", v) end
    P.Tip("Keep the username in this server's accounts.")
    ui.SameLine(0, 16 * s)
    local can = view.remember and view.canSavePasswords
    ui.BeginDisabled(not can)
    v, changed = ui.Checkbox("Save password", view.savePassword)
    if changed then API.pregame.LoginSet("savePassword", v) end
    ui.EndDisabled()
    if not view.canSavePasswords then
      P.Tip(NO_KEYRING_TIP)
    elseif not view.remember then
      P.Tip("Tick Remember account to save its password.")
    else
      P.Tip(KEYRING_TIP)
    end
  end
  return submit
end

-- ---------------------------------------------------------------- more options
-- The texture pack (hs.Option "texture_pack", display.texture_folder; the Options window's Textures row too): Off,
-- then each folder of Hailstone's textures/, such as a pack Hailstone Texture Studio built. Read at the next start:
-- textures already loaded stay as they are.
local function texturePack(s)
  local pack = API.settings.Option("texture_pack")
  if pack == nil then return end
  packAtStart = packAtStart or pack
  detail("Textures", s, function()
    local choices = API.settings.OptionChoices("texture_pack")
    if ui.BeginCombo("##texture_pack", choices[pack + 1] or "A folder of its own") then
      for i, name in ipairs(choices) do
        if ui.Selectable(name, i == pack + 1) and i ~= pack + 1 then API.settings.SetOption("texture_pack", i - 1) end
      end
      ui.EndCombo()
    end
    P.Tip(pack ~= packAtStart and "Hailstone uses it from its next start."
      or "Replacement textures from a folder of Hailstone's textures folder, such as a pack Texture Studio built. "
        .. "Off: the game's own. A change is used from the next start.")
  end)
  if pack ~= packAtStart then
    ui.TableNextRow()
    ui.TableNextColumn()
    ui.TableNextColumn()
    P.Muted("From the next start")
  end
end

local function gameFiles(view, s)
  if #view.gameFolders == 0 and API.settings.Option("texture_pack") == nil then return end
  local current = view.gameFolders[1]
  if not ui.BeginTable("##gamefiles", 2, ui.TableFlags.SizingFixedFit) then return end
  ui.TableSetupColumn("label", ui.TableColumnFlags.WidthFixed, 74 * s)
  ui.TableSetupColumn("value", ui.TableColumnFlags.WidthStretch, 0)
  if current then detail("Game files", s, function()
    if view.gameFolderFixed then
      label(current.label, s)
      P.Tip(current.path .. "\nSet by --game-dir or EQ_GAME_DIR.")
      return
    end
    if ui.BeginCombo("##gamefiles", current.label) then
      for i, f in ipairs(view.gameFolders) do
        ui.PushID(i)
        ui.BeginDisabled(not f.usable)
        if ui.Selectable(f.label, i == 1) and i ~= 1 then API.pregame.LoginDo("gameFolder", i) end
        ui.EndDisabled()
        P.Tip(f.path .. (f.usable and "" or "\nNot usable now: see Browse."))
        ui.PopID()
      end
      ui.Separator()
      if ui.Selectable("Browse...") then API.pregame.LoginDo("browse") end
      ui.EndCombo()
    end
    P.Tip("The EverQuest files to play with, RoF2 or live: " .. current.path .. "\nA server's own files go over them.")
  end) end
  texturePack(s)
  ui.EndTable()
end

-- Whether the box set card can show: a saved server with saved accounts.
local function canBox(view)
  return not view.transient and view.selected > 0 and #view.accounts > 0
end

-- The box set card: the ticked accounts log in together, each in a window of its own.
local function boxSetCard(view, s, bh)
  P.Title("Box set", "Ticked accounts log in together, each in its own window", s)
  ui.BeginDisabled(view.busy)
  local flags = ui.TableFlags.RowBg | ui.TableFlags.SizingFixedFit
  local forget = nil
  if ui.BeginTable("##boxset", 5, flags) then
    ui.TableSetupColumn("Box", ui.TableColumnFlags.WidthFixed, 0)
    ui.TableSetupColumn("Account", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("World", ui.TableColumnFlags.WidthFixed, 92 * s)
    ui.TableSetupColumn("Character", ui.TableColumnFlags.WidthFixed, 92 * s)
    ui.TableSetupColumn("##forget", ui.TableColumnFlags.WidthFixed, 0)
    ui.TableHeadersRow()
    for _, a in ipairs(view.accounts) do
      ui.PushID(a.username)
      ui.TableNextRow()
      ui.TableNextColumn()
      ui.BeginDisabled(not a.canBox)
      local v, changed = ui.Checkbox("##box", a.box)
      if changed then API.pregame.LoginDo("box", a.username, v) end
      ui.EndDisabled()
      P.Tip(a.canBox and "In the box set: Log in box set starts a window for it."
        or "Log in with it once with Save password ticked to box it.")
      ui.TableNextColumn()
      local col = a.username == view.username and C.pet or C.text
      local x, y = ui.GetCursorPos()
      ui.SetCursorPos(x, y + 4 * s) -- (level with the combo boxes' text)
      ui.TextColored(col[1], col[2], col[3], 1, a.username)
      P.Tip(a.passwordSaved and "Password saved" or "No password saved")
      -- the world a box login plays (the server select skipped), and its character there
      ui.TableNextColumn()
      ui.SetNextItemWidth(-1)
      if ui.BeginCombo("##world", a.world == "" and "(last)" or a.world) then
        if ui.Selectable("(last played)", a.world == "") and a.world ~= "" then API.pregame.LoginDo("world", a.username, "") end
        for _, w in ipairs(view.worlds) do
          if ui.Selectable(w, w == a.world) and w ~= a.world then API.pregame.LoginDo("world", a.username, w) end
        end
        ui.EndCombo()
      end
      P.Tip(#view.worlds == 0 and "The game servers show here once this server has listed them (log in once)."
        or "The game server this account plays in a box login.")
      ui.TableNextColumn()
      ui.SetNextItemWidth(-1)
      if ui.BeginCombo("##character", a.character == "" and "(choose)" or a.character) then
        if ui.Selectable("(choose)", a.character == "") and a.character ~= "" then
          API.pregame.LoginDo("character", a.username, "")
        end
        for j, c in ipairs(a.characters) do
          if a.world == "" or c.world == a.world then
            local label = c.name .. (c.level > 0 and string.format("  %d %s", c.level, c.classes) or "")
            if a.world == "" then label = label .. "  (" .. c.world .. ")" end
            ui.PushID(j)
            if ui.Selectable(label, c.name == a.character) and c.name ~= a.character then
              API.pregame.LoginDo("character", a.username, c.name)
            end
            ui.PopID()
          end
        end
        ui.EndCombo()
      end
      P.Tip("The character a box login enters; (choose): pick it at character select.")
      ui.TableNextColumn()
      if ui.SmallButton("Forget") then forget = a.username end
      P.Tip("Forget this account and its saved password")
      ui.PopID()
    end
    ui.EndTable()
  end
  if forget then
    forgetName = forget
    ui.OpenPopup("##forgetaccount")
  end
  if view.boxCount > 0 then
    ui.Dummy(0, 2 * s)
    if P.Primary(string.format("Log in box set (%d)", view.boxCount), -1, bh, view.boxReady) then
      API.pregame.LoginDo("loginBox")
    end
    P.Tip(BOX_LOGIN_TIP)
  end
  ui.EndDisabled()
  ui.Dummy(0, 2 * s)
  P.AlignRight(P.ButtonsWidth(CLOSE, s))
  if ui.Button("Close") then boxOpen = false end

  -- (in this card: the question opens from its Forget buttons)
  if P.Confirm("##forgetaccount", string.format("Forget %s on this server?", forgetName or ""),
      "Its saved password leaves the system keyring.", "Forget", s) and forgetName then
    API.pregame.LoginDo("forget", forgetName)
  end
end

-- ---------------------------------------------------------------- the characters
-- "Level 50 CLR  ·  World"
local function characterLine(c)
  local who = c.level > 0 and string.format("Level %d %s", c.level, c.classes) or c.classes
  if c.world ~= "" then who = who .. "  \194\183  " .. c.world end
  return who
end

local function quickCard(view, s, bh)
  P.Title("Continue as", "A character seen on this server's accounts", s)
  local _, lh = ui.CalcTextSize("Ag")
  local rowH = lh * 2 + 12 * s
  local shown = math.min(#view.characters, 6)
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, 8 * s, 3 * s)
  ui.BeginChild("##quick", 0, shown * (rowH + 3 * s), false)
  for i, c in ipairs(view.characters) do
    ui.PushID(i)
    local clicked, double, x0, y0, x1 = P.Row("##row", view.quick == i, rowH, s)
    if clicked then API.pregame.LoginSet("quick", i) end
    if double then API.pregame.LoginDo("play", i) end
    if ui.IsItemHovered() then
      ui.SetTooltip(string.format("%s on %s\nAccount %s%s", c.name, c.world, c.account,
        c.passwordSaved and "" or "\nNo saved password: type it, then Play."))
    end
    local tx = x0 + 12 * s
    T.Text(tx, y0 + 5 * s, c.name, C.text)
    T.Text(tx, y0 + 6 * s + lh, characterLine(c), C.muted, T.font.small)
    local right = x1 - 8 * s
    if c.last then
      local pw = 0
      ui.PushFont(T.font.tiny)
      pw = ui.CalcTextSize("last played") + 12 * s
      ui.PopFont()
      P.Pill(right - pw, y0 + 6 * s, "last played", C.accent, s)
    end
    T.Text(right, y0 + 6 * s + lh, c.account, c.passwordSaved and C.faint or C.danger, T.font.small, "right")
    ui.PopID()
  end
  ui.EndChild()
  ui.PopStyleVar()
  ui.Dummy(0, 2 * s)
  local q = view.characters[view.quick]
  if P.Primary(q and ("Play " .. q.name) or "Play", -1, bh, q ~= nil and not view.busy) then API.pregame.LoginDo("play") end
  if q and not q.passwordSaved then
    ui.PushTextWrapPos(0)
    P.Faint(string.format("%s has no saved password: type it in Password, then Play.", q.account))
    ui.PopTextWrapPos()
  end
end

-- ---------------------------------------------------------------- add-ons
-- The add-on manager (view.addons, docs/addons.md): a GitHub link, the install card of the add-on it names with
-- what it asks to do, and every script and plugin. The server's own are listed with nothing that removes or stops
-- them: the server sends them at each login and they always run there.
local function bullets(list, s)
  for _, line in ipairs(list) do ui.BulletText(line) end
end

local function offerCard(o, s, bh)
  ui.Dummy(0, 4 * s)
  P.Heading(string.format("%s %s", o.name, o.version))
  P.Muted(string.format("A %s%s, from %s", o.autoload and "managed add-on" or o.kind, o.author ~= "" and (" by " .. o.author) or "", o.source))
  ui.PushTextWrapPos(0)
  if o.description ~= "" then ui.TextWrapped(o.description) end
  if o.autoload then ui.TextWrapped("Starts automatically when enabled. Manage it from My add-ons.") end
  if o.installedVersion ~= "" then P.Faint("Installed now: " .. o.installedVersion) end
  if o.kind ~= "tool" and o.kind ~= "upscaler" and o.kind ~= "ui" then
    ui.Text("It asks to:")
    bullets(o.caps, s)
  end
  P.Faint(o.key ~= "" and ("Signed with key " .. o.key .. ": later versions must carry the same key.")
    or "Not signed: later versions come from whoever controls the repository.")
  if o.kind == "ui" then
    ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, "Full client interface access")
    ui.TextWrapped("A UI pack can call host interface, account and game actions. Install only interfaces you trust.")
  elseif o.kind == "plugin" or o.kind == "tool" or o.kind == "upscaler" then
    ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, o.kind == "tool" and TOOL_WARNING
      or o.kind == "upscaler" and UPSCALER_WARNING or PLUGIN_WARNING)
  end
  if o.older then
    ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, string.format(
      "This is an older version than the one installed (%s): installing it goes back to %s.",
      o.installedVersion, o.version))
  end
  if o.blocked ~= "" then ui.TextColored(C.danger[1], C.danger[2], C.danger[3], 1, o.blocked) end
  ui.PopTextWrapPos()
  ui.Dummy(0, 2 * s)
  local label = o.older and "Install the older version" or (o.installedVersion == "" and "Install" or "Install this version")
  if P.Primary(label, 0, bh, o.blocked == "") then
    API.pregame.LoginDo("addonInstall", o.older and "older" or nil)
    addonTab = "mine" -- (where it shows up, with its progress)
  end
  ui.SameLine()
  if ui.Button("Cancel") then API.pregame.LoginDo("addonCancel") end
  ui.SameLine()
  if ui.Button("Repository") then API.pregame.LoginDo("addonPage") end
  P.Tip(o.page)
end

local function addonRow(a, r, s, busy)
  ui.PushID(r.group .. ":" .. r.name)
  local label = r.name .. (r.version ~= "" and (" " .. r.version) or "") .. "  \194\183  " .. (r.managed and "add-on" or r.kind)
  if r.state ~= "" and r.state ~= "uptodate" and r.state ~= "idle" then label = label .. "  \194\183  " .. r.state end
  if ui.TreeNode(label .. "##row") then
    ui.PushTextWrapPos(0)
    if r.description ~= "" then ui.TextWrapped(r.description) end
    if r.source ~= "" then P.Faint("from " .. r.source .. (r.signed and ", signed" or "")) end
    if r.stateText ~= "" then
      local col = (r.state == "failed" or r.state == "blocked") and C.danger or C.accent
      ui.TextColored(col[1], col[2], col[3], 1, r.stateText)
    end
    if #r.caps > 0 then
      P.Muted("May:")
      bullets(r.caps, s)
    end
    if #r.newCaps > 0 then
      P.Muted("The new version also asks to:")
      bullets(r.newCaps, s)
    end
    if #r.modified > 0 then P.Faint(string.format("You changed %d of its files.", #r.modified)) end
    if r.note ~= "" then P.Faint(r.note) end
    ui.PopTextWrapPos()
    if r.canStart then
      if P.Primary("Start", 0, 0, true) then API.pregame.LoginDo("addonStart", r.name) end
      P.Tip("Start " .. r.name .. " (it opens a window of its own)")
      ui.SameLine()
    end
    if r.group == "installed" then
      if r.managed then
        ui.BeginDisabled(busy == true)
        local enabled, changedEnabled = ui.Checkbox("Enabled", r.enabled)
        if changedEnabled then API.pregame.LoginDo("addonEnabled", r.name, enabled) end
        ui.EndDisabled()
        ui.TextDisabled("Starts automatically when this server allows it.")
      end
      local v, changed = ui.Checkbox("Update by itself", r.autoUpdate)
      if changed then API.pregame.LoginDo("addonAuto", r.name, v) end
      P.Tip("Install newer versions at start when they ask for nothing more")
      if r.canUpdate then
        ui.SameLine()
        local what = r.state == "approval" and "Approve and update" or r.state == "modified" and "Update anyway"
          or r.state == "failed" and "Try again" or "Update"
        if ui.Button(what) then API.pregame.LoginDo("addonUpdate", r.name) end
      end
      if r.page ~= "" then
        ui.SameLine()
        if ui.Button("Repository") then API.pregame.LoginDo("addonPage", r.name) end
        P.Tip(r.page)
      end
      ui.SameLine()
      ui.BeginDisabled(not r.canRemove)
      if ui.Button("Remove") then
        removeName = r.name
        removeAsk = true -- (opened beside its BeginPopup: this row's ID scope would give the popup another ID)
      end
      ui.EndDisabled()
    end
    -- the server's compiled plugin: the player's word on it (docs/plugins.md "The server's compiled plugins ask first")
    if r.consent == "allowed" then
      if ui.Button("Don't allow it any more") then API.pregame.LoginDo("addonRevoke", r.name) end
      P.Tip("It stops running on this server; the server's features that need it go too")
      ui.SameLine()
    elseif r.consent == "denied" then
      if ui.Button("Ask me again") then API.pregame.LoginDo("addonAskAgain", r.name) end
      P.Tip("Your next visit asks again whether it may run")
      ui.SameLine()
    end
    if r.folder ~= "" then
      if r.group == "installed" then ui.SameLine() end
      if ui.Button("Open folder") then API.pregame.LoginDo("addonFolder", r.name) end
    end
    ui.TreePop()
  end
  ui.PopID()
end

local ADDON_GROUPS = { { "installed", "Installed" }, { "manual", "In your folders" }, { "server", "" } }

-- The link field: an add-on found somewhere else than the store.
local function linkField(a, s)
  ui.SetNextItemWidth(-(P.ButtonsWidth(LOOK, s) + 8 * s))
  local text, enter = ui.InputTextWithHint("##addonlink", "https://github.com/owner/repo", a.link, 400, ENTER)
  if text ~= a.link then
    API.pregame.LoginSet("addonLink", text)
    a.link = text
    held = nil
  end
  P.Tip("Paste a link to an add-on's GitHub repository, a release of it or a branch")
  ui.SameLine()
  if (P.Primary("Look", 0, 0, not a.inspecting and a.link ~= "") or enter) and not a.inspecting then
    API.pregame.LoginDo("addonInspect")
  end
end

-- An add-on of the store (view.addons.store, docs/addons.md "Finding add-ons"): its name and kind, the servers it is
-- made for, what it does, and Install (its install card: nothing is installed until the player says yes there).
local STORE_LOOK_TIP = "What it is and what it asks to do: nothing is installed yet"
local function storeItem(a, it, s, lh)
  ui.PushID("store:" .. it.source)
  local x0, y0 = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  ui.Indent(10 * s)
  ui.Dummy(0, 2 * s)
  ui.PushFont(T.font.large)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, it.name)
  ui.PopFont()
  local kind = (it.kind == "plugin" or it.kind == "script" or it.kind == "tool" or it.kind == "upscaler"
    or it.kind == "ui") and it.kind or nil
  if kind or it.worlds ~= "" then
    ui.SameLine()
    local px, py = ui.GetCursorScreenPos()
    local pw = 0
    if kind then pw = P.Pill(px, py + 3 * s, kind, C.accent, s) + 6 * s end
    if it.worlds ~= "" then P.Pill(px + pw, py + 3 * s, "for " .. it.worlds, C.gold, s) end
    ui.NewLine()
  end
  ui.PushTextWrapPos(x0 + w - 10 * s - ui.GetWindowPos())
  if it.description ~= "" then ui.TextWrapped(it.description) end
  local by = {}
  if it.version ~= "" then by[#by + 1] = "version " .. it.version end
  if it.author ~= "" then by[#by + 1] = "by " .. it.author end
  if #by > 0 then P.Faint(table.concat(by, "  \194\183  ")) end
  if it.state == "unavailable" then
    ui.TextColored(C.danger[1], C.danger[2], C.danger[3], 1, it.problem)
  end
  ui.PopTextWrapPos()
  local busy = a.inspecting or not a.available
  if it.state == "install" then
    if P.Primary("Install", 0, 0, not busy) then API.pregame.LoginDo("addonStoreLook", it.source) end
    P.Tip(STORE_LOOK_TIP)
  elseif it.state == "update" then
    if P.Primary("Update to " .. it.version, 0, 0, not busy) then API.pregame.LoginDo("addonStoreLook", it.source) end
    P.Tip("You have " .. it.installedVersion .. ". " .. STORE_LOOK_TIP)
  elseif it.state == "installed" then
    ui.TextColored(C.good[1], C.good[2], C.good[3], 1, "Installed: version " .. it.installedVersion)
  elseif it.state == "manual" then
    P.Faint("One of this name is in your folders already.")
  end
  ui.Unindent(10 * s)
  ui.Dummy(0, 2 * s)
  local _, y1 = ui.GetCursorScreenPos()
  ui.DrawRect(x0, y0, x0 + w, y1 - 4 * s, C.frameBorder[1], C.frameBorder[2], C.frameBorder[3], 1, 6 * s, 1)
  ui.Dummy(0, 2 * s)
  ui.PopID()
end

local function storeTab(a, s, lh)
  if a.available and not a.browsed and not a.browsing then API.pregame.LoginDo("addonBrowse") end
  ui.BeginChild("##addonstore", 0, 19 * lh, false)
  if a.browsing then
    P.Spinner(lh / 2, s)
    ui.SameLine()
    P.Muted("Finding add-ons...")
  elseif not a.available then
    ui.PushTextWrapPos(0)
    P.Faint(a.whyOff)
    ui.PopTextWrapPos()
  elseif #a.store == 0 then
    ui.PushTextWrapPos(0)
    P.Faint(a.storeError ~= "" and a.storeError or "No add-ons are listed yet.")
    ui.PopTextWrapPos()
  else
    for _, it in ipairs(a.store) do storeItem(a, it, s, lh) end
  end
  ui.EndChild()
  if a.available then
    P.Muted("Have a link to an add-on from somewhere else?")
    linkField(a, s)
  end
end

local function mineTab(a, s, lh, busy)
  local groups = ADDON_GROUPS
  groups[3][2] = a.server ~= "" and ("From " .. a.server) or "From this server"
  ui.BeginChild("##addonlist", 0, 19 * lh, false)
  local any = false
  for _, g in ipairs(groups) do
    local first = true
    for _, r in ipairs(a.rows) do
      if r.group == g[1] then
        if first then
          P.Heading(g[2])
          first = false
        end
        any = true
        addonRow(a, r, s, busy)
      end
    end
  end
  if not any then P.Faint("No scripts or plugins yet. Find some in the Find add-ons tab.") end
  ui.EndChild()
  if a.available then
    if ui.Button("Check for updates") then API.pregame.LoginDo("addonCheck") end
    P.Tip("Look for newer versions of your add-ons now")
  end
end

local function addonsCard(view, s, bh, standalone)
  local a = view.addons
  P.Title("Add-ons", "Extra windows and features for Hailstone", s)
  if standalone then
    P.AlignRight(P.ButtonsWidth(CLOSE, s))
    if ui.Button("Close") then API.pregame.LoginSet("addonsOpen", false) end
  end
  P.Status(a.status, a.inspecting, s)
  if a.offer then
    offerCard(a.offer, s, bh)
    ui.Dummy(0, 4 * s)
  end
  local _, lh = ui.CalcTextSize("Ag")
  -- testing: test.addontab = store or mine in ui/settings/modern.txt picks that tab, for screenshots and the tests
  local test = T.Setting("test.addontab")
  if test and test ~= "" and test ~= testedTab then addonTab = test end
  testedTab = test
  if ui.BeginTabBar("##addontabs") then
    if ui.BeginTabItem("Find add-ons", addonTab == "store" and ui.TabItemFlags.SetSelected or 0) then
      storeTab(a, s, lh)
      ui.EndTabItem()
    end
    local mine = 0
    for _, r in ipairs(a.rows) do if r.group == "installed" then mine = mine + 1 end end
    local label = mine > 0 and string.format("My add-ons (%d)###mine", mine) or "My add-ons###mine"
    if ui.BeginTabItem(label, addonTab == "mine" and ui.TabItemFlags.SetSelected or 0) then
      mineTab(a, s, lh, view.busy)
      ui.EndTabItem()
    end
    ui.EndTabBar()
  end
  addonTab = nil
  if a.available and a.browsed then
    if ui.Button("Refresh the list") then API.pregame.LoginDo("addonBrowse", true) end
    P.Tip("Fetch the list of add-ons again")
    if not standalone then ui.SameLine() end
  end
  if not standalone then
    P.AlignRight(P.ButtonsWidth(CLOSE, s))
    if ui.Button("Close") then API.pregame.LoginSet("addonsOpen", false) end
  end

  if removeAsk then
    removeAsk = false
    ui.OpenPopup("##removeaddon")
  end
  if P.Confirm("##removeaddon", string.format("Remove %s?", removeName or ""),
      "Its own files go; files of yours in its folder (its settings) stay.", "Remove", s) and removeName then
    API.pregame.LoginDo("addonRemove", removeName)
  end
end

-- ---------------------------------------------------------------- the interface
-- The UI pack the server is played with, on the card itself (not under Edit): the screen changes to it at once,
-- and a saved server keeps it at once (hs.LoginSet "ui").
local function interfacePicker(view, width)
  P.Muted("Interface") -- (level with Music's text: ImGui aligns a text on the line to its frames' baseline)
  ui.SameLine()
  local available = ui.GetContentRegionAvail()
  ui.SetNextItemWidth(math.max(1, math.min(width, available)))
  if ui.BeginCombo("##ui", packName(view, view.ui)) then
    for _, p in ipairs(view.uiPacks) do
      ui.PushID(p.id)
      local available = p.available ~= false
      ui.BeginDisabled(not available)
      if ui.Selectable(p.name, p.id == view.ui) and available and p.id ~= view.ui then API.pregame.LoginSet("ui", p.id) end
      ui.EndDisabled()
      P.Tip((p.description or "") .. "\nMade for: " .. p.input
        .. (available and "" or "\nUnavailable: " .. (p.reason or "")))
      ui.PopID()
    end
    ui.EndCombo()
  end
  local note = view.selected > 0 and UI_TIP_SAVED or UI_TIP_NEW
  for _, p in ipairs(view.uiPacks) do
    if p.id == view.ui and p.available == false then note = note .. "\nUnavailable: " .. (p.reason or "") end
  end
  P.Tip(note)
end

-- ---------------------------------------------------------------- the corner
-- The updater's news (view.update, docs/updater.md): its sentence and its buttons, in the corner card.
local function updateNotice(view, s)
  local u = view.update
  if not u then return end
  local col = u.state == "failed" and C.danger or C.accent
  ui.PushStyleColor(ui.Col.Text, col[1], col[2], col[3], col[4] or 1)
  ui.TextWrapped(u.text)
  ui.PopStyleColor()
  if u.state == "downloading" and u.progress >= 0 then ui.ProgressBar(u.progress, -1, 4 * s, "") end
  local first = true
  local function button(label, action, primary, tip)
    if not first then ui.SameLine() end
    first = false
    local clicked
    if primary then clicked = P.Primary(label, 0, 0, true) else clicked = ui.Button(label) end
    if clicked then API.pregame.LoginDo(action) end
    P.Tip(tip)
  end
  if u.canRestart then button("Restart now", "updateRestart", true, "Install " .. u.version .. " and start it") end
  if u.canDownload then
    button(u.state == "failed" and "Try again" or "Download", "updateDownload", true, "Download " .. u.version)
  end
  if u.hasNotes then button("Release notes", "updateNotes", false, u.notes) end
  if u.canSkip then button("Skip", "updateSkip", false, "Not offered again until a newer release comes") end
  ui.Dummy(0, 2 * s)
end

-- The corner card (view.about, docs/updater.md "What the player sees"): this build, whether it is up to date (or the
-- updater's news with its buttons), What's new (this release's part of CHANGELOG.md) and Hailstone's GitHub page.
local ABOUT_COLORS = { uptodate = "good", checking = "muted", newer = "gold", unknown = "faint", off = "faint" }
local function aboutCard(view, s)
  local a = view.about
  ui.PushFont(T.font.large)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, "Hailstone " .. a.version)
  ui.PopFont()
  if a.build ~= "" then P.Tip("This build: " .. a.build) end
  if view.update then
    updateNotice(view, s)
  else
    local col = C[ABOUT_COLORS[a.status] or "faint"]
    if a.status == "checking" then
      local _, lh = ui.CalcTextSize("Ag")
      P.Spinner(lh / 2, s)
      ui.SameLine()
    elseif a.status == "uptodate" then
      -- a dot in the line's colour (the fonts have no check mark)
      local _, lh = ui.CalcTextSize("Ag")
      local x, y = ui.GetCursorScreenPos()
      ui.DrawCircleFilled(x + lh * 0.3, y + lh * 0.55, lh * 0.22, col[1], col[2], col[3], 1)
      ui.Dummy(lh * 0.6, lh)
      ui.SameLine()
    end
    ui.PushTextWrapPos(0)
    ui.TextColored(col[1], col[2], col[3], 1, a.text)
    ui.PopTextWrapPos()
  end
  ui.BeginDisabled(#a.notes == 0)
  if ui.Button("What's new") then ui.OpenPopup("##whatsnew") end
  ui.EndDisabled()
  -- testing: test.whatsnew = 1 in ui/settings/modern.txt opens What's new once, for screenshots and the tests
  if not notesTested and #a.notes > 0 and T.Setting("test.whatsnew") == "1" then
    notesTested = true
    ui.OpenPopup("##whatsnew")
  end
  P.Tip(#a.notes == 0 and "The release notes are not in this Hailstone folder (CHANGELOG.md)."
    or "What changed in this version")
  if a.page ~= "" then
    ui.SameLine()
    if ui.Button("GitHub") then API.pregame.LoginDo("openProject") end
    P.Tip("Hailstone's page, in your web browser:\n" .. a.page)
  end

  local io = ui.GetIO()
  local w = math.min(640 * s, io.width - 40 * s)
  ui.SetNextWindowSize(w, 0, ui.Cond.Always)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 16 * s, 14 * s)
  local open = ui.BeginPopup("##whatsnew")
  ui.PopStyleVar()
  if open then
    P.Heading("What's new in Hailstone " .. a.version)
    ui.BeginChild("##notes", 0, math.min(460 * s, io.height * 0.6), false)
    ui.PushTextWrapPos(0)
    for _, line in ipairs(a.notes) do
      ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, "\194\183")
      ui.SameLine()
      ui.Indent(14 * s)
      ui.TextWrapped(line)
      ui.Unindent(14 * s)
      ui.Dummy(0, 1 * s)
    end
    ui.PopTextWrapPos()
    ui.EndChild()
    ui.Dummy(0, 2 * s)
    if a.page ~= "" then
      if ui.Button("Open GitHub") then API.pregame.LoginDo("openProject") end
      ui.SameLine()
    end
    P.AlignRight(P.ButtonsWidth(CLOSE, s))
    if ui.Button("Close") or ui.IsKeyPressed(ui.Key.Escape) then ui.CloseCurrentPopup() end
    ui.EndPopup()
  end
end

-- ---------------------------------------------------------------- the screen
-- The picked server's suggested add-ons (view.addons.suggestions, docs/addons.md "Suggested add-ons"): its sentence,
-- then Look at it (the install card: nothing is installed yet), Not now and Don't ask again.
local function suggestionNotice(view, s)
  for _, sg in ipairs(view.addons.suggestions or {}) do
    ui.PushID("suggest:" .. sg.source)
    ui.PushStyleColor(ui.Col.Text, C.accent[1], C.accent[2], C.accent[3], C.accent[4] or 1)
    ui.TextWrapped(sg.text)
    ui.PopStyleColor()
    if P.Primary(sg.required and "Review required add-on" or "Look at it", 0, 0, not view.busy) then
      API.pregame.LoginDo("addonSuggestLook", sg.source)
    end
    P.Tip("What it is and what it asks to do: nothing is installed yet")
    if not sg.required then
      ui.SameLine()
      if ui.Button("Not now") then API.pregame.LoginDo("addonSuggestLater", sg.source) end
      P.Tip("Not suggested again until Hailstone starts again")
      ui.SameLine()
      if ui.Button("Don't ask again") then API.pregame.LoginDo("addonSuggestNever", sg.source) end
      P.Tip("Not suggested again for this server")
    end
    ui.Dummy(0, 2 * s)
    ui.PopID()
  end
end

local function loginCard(view, s, bh)
  local appearing = ui.IsWindowAppearing()
  local subtitle
  if view.transient then subtitle = "Test login"
  elseif view.selected > 0 then subtitle = view.servers[view.selected].name
  else subtitle = "New server" end
  P.Title("Hailstone", subtitle, s)
  ui.BeginDisabled(view.busy)
  local submit = serverSection(view, s)
  ui.Dummy(0, 4 * s)
  submit = accountSection(view, s, appearing) or submit
  ui.Dummy(0, 6 * s)
  if P.Primary("Log in", -1, bh * 1.2, view.ready) then submit = true end
  if submit and view.ready then API.pregame.LoginDo("login") end
  if view.sessionTicketAvailable and not view.busy then
    if ui.Button("Sign in with session", -1, bh) then API.pregame.LoginDo("resume_token") end
  end
  ui.Dummy(0, 2 * s)
  -- (toggles of their own: ImGui's collapsing header reaches past the card's padding)
  if #view.gameFolders > 0 or API.settings.Option("texture_pack") ~= nil then
    if ui.Selectable(more and "-  Fewer options" or "+  More options", false) then more = not more end
    P.Tip(more and "" or "The game files and the texture pack")
    if more then gameFiles(view, s) end
  end
  if canBox(view) then
    local label = view.boxCount > 0 and string.format("Box set (%d)", view.boxCount) or "Box set"
    if ui.Selectable((boxOpen and "-  " or "+  ") .. label, boxOpen) then boxOpen = not boxOpen end
    P.Tip(boxOpen and "Close the box set" or "Log in several accounts at once, each in its own window")
  end
  ui.EndDisabled()

  ui.Dummy(0, 2 * s)
  ui.Separator()
  P.Status(view.status, view.busy, s)
  ui.BeginDisabled(view.busy)
  local v, changed = ui.Checkbox("Music", view.music)
  if changed then API.pregame.LoginSet("music", v) end
  ui.EndDisabled()
  P.Tip("The music of the login, server select and character select screens (not the game's).")
  if not view.transient then
    ui.SameLine(0, 16 * s)
    local pickerW = 130 * s
    if ui.GetContentRegionAvail() < ui.CalcTextSize("Interface") + 8 * s + pickerW then ui.NewLine() end
    interfacePicker(view, pickerW)
  end
  ui.SameLine()
  local actionsW = P.ButtonsWidth(ADDONS_QUIT, s)
  if ui.GetContentRegionAvail() < actionsW then ui.NewLine() end
  P.AlignRight(actionsW)
  if ui.Button("Add-ons") then API.pregame.LoginSet("addonsOpen", not view.addons.open) end
  P.Tip("Find and install extra windows and features, and keep them up to date")
  ui.SameLine()
  if ui.Button("Quit") then API.pregame.LoginDo("quit") end
  -- Server notices can fill the card. Keep the Add-ons button above that list.
  suggestionNotice(view, s)

  -- the confirmations (outside the disabled block: they open from it)
  if P.Confirm("##deleteserver", string.format("Delete \"%s\"?", subtitle), DELETE_NOTE, "Delete", s) then
    API.pregame.LoginDo("delete")
  end
end

local tested = false
-- the cards, kept from frame to frame (their sizes and places are set each frame)
local BOX_CARD = { key = "login.boxset", draw = boxSetCard }
local LOGIN_CARD = { key = "login", draw = loginCard }
local ADDONS_CARD = { key = "login.addons", draw = addonsCard }
local QUICK_CARD = { key = "login.quick", draw = quickCard }
local ABOUT_CARD = { key = "login.about", draw = aboutCard }
local cards = {}
local function addCard(c, w, h)
  c.w, c.h = w, h
  cards[#cards + 1] = c
end

local function drawCard(c, view, s, bh, maxHeight, standalone)
  local ok, err = true
  if P.Begin(c.key, c.x, c.y, c.w, s, maxHeight) then
    ok, err = pcall(c.draw, view, s, bh, standalone)
  end
  P.End()
  if not ok then error(err, 0) end
end

ui.window("Login", function()
  local view = API.pregame.LoginScreen(held and held.revision)
  if view == true then view = held else held = view end
  if not view then return end
  -- testing: test.login = addons (or boxset, more) in ui/settings/modern.txt opens the add-ons (or box set) card (or
  -- More options) as the screen first appears, for screenshots (as the gamepad pack's test.login)
  if not tested then
    tested = true
    local test = ui.GetSetting("test.login")
    if test == "addons" then API.pregame.LoginSet("addonsOpen", true) end
    if test == "boxset" then boxOpen = true end
    if test == "more" then more = true end
  end
  local s = P.Scale()
  local io = ui.GetIO()
  local _, lh = ui.CalcTextSize("Ag")
  local bh = lh + 14 * s
  local cardW, quickW, addonsW, boxW, aboutW = 430 * s, 330 * s, 480 * s, 500 * s, 270 * s
  local gap, margin = 18 * s, 16 * s
  local addonsOpen = view.addons.open
  local quick = #view.characters > 0
  local box = boxOpen and canBox(view)
  local _, ch = P.Size("login", cardW, 520 * s)
  local _, qh = P.Size("login.quick", quickW, 300 * s)
  local _, ah = P.Size("login.addons", addonsW, 420 * s)
  local _, bxh = P.Size("login.boxset", boxW, 260 * s)
  local _, abh = P.Size("login.about", aboutW, 90 * s)
  -- Centre the cards as a row when they fit. On narrow screens, Add-ons opens on its own so its contents
  -- stay in reach even when the other cards would fill the viewport.
  for i = #cards, 1, -1 do cards[i] = nil end
  if box then addCard(BOX_CARD, boxW, bxh) end
  addCard(LOGIN_CARD, cardW, ch)
  if quick then addCard(QUICK_CARD, quickW, qh) end
  if addonsOpen then addCard(ADDONS_CARD, addonsW, ah) end
  local rowW, rowH, colH = gap * (#cards - 1), 0, gap * (#cards - 1)
  for _, c in ipairs(cards) do
    rowW, rowH, colH = rowW + c.w, math.max(rowH, c.h), colH + c.h
  end
  if addonsOpen and io.width < rowW + 2 * margin then
    local c = ADDONS_CARD
    c.w = math.max(1, math.min(addonsW, io.width - 2 * margin))
    c.x, c.y = (io.width - c.w) / 2, math.max(margin, (io.height - c.h) / 2)
    drawCard(c, view, s, bh, math.max(1, io.height - margin - c.y), true)
    return
  end
  -- the corner card's room: cards that would reach under it move right of it, else below it, when they fit there
  local aboutRight, belowAbout = margin + aboutW + gap, margin + abh + gap
  local function place(x, w, h)
    local y = math.max(margin, (io.height - h) / 2)
    if x < aboutRight and y < belowAbout then
      if aboutRight + w <= io.width - margin then x = aboutRight
      elseif belowAbout + h <= io.height - margin then y = belowAbout end
    end
    return x, y
  end
  if io.width >= rowW + 2 * margin then
    local x, y = place((io.width - rowW) / 2, rowW, rowH)
    for _, c in ipairs(cards) do
      c.x, c.y = x, y
      x = x + c.w + gap
    end
  else
    -- the login card first, then the others
    if box then table.insert(cards, 2, table.remove(cards, 1)) end
    local width = math.max(1, math.min(cardW, io.width - 2 * margin))
    local x, y = place((io.width - width) / 2, width, colH)
    for _, c in ipairs(cards) do
      c.w = width
      c.x, c.y = x, y
      y = y + c.h + gap
    end
  end
  ABOUT_CARD.x, ABOUT_CARD.y, ABOUT_CARD.w = margin, margin, aboutW
  cards[#cards + 1] = ABOUT_CARD
  for _, c in ipairs(cards) do
    local maxHeight
    if c == LOGIN_CARD or c == ADDONS_CARD then maxHeight = math.max(1, io.height - margin - c.y) end
    drawCard(c, view, s, bh, maxHeight)
  end
end)
