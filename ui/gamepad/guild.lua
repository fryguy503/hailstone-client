-- Guild (gamepad pack): stands in for GuildManagementWnd (the guild key, the EQ menu's Guild). The message of the
-- day on top, the members below (X shows or hides the offline ones, Y the alts), and on the right what can be done
-- with the picked member: a tell, promote, demote, a rank picked from the ranks the game allows (the Access page's
-- rank box), make leader, remove (each asked first), your personal note on them, renaming their rank (the stock
-- text entry), and inviting the target. Each is the stock window's control (hs.ClickChild, the rank box through
-- hs.ComboSelect), enabled as CGuildMgmtWnd::UpdateButtons enables it for the player's rank and the guild's
-- permissions (hs.ChildState), so the game decides what is allowed. The Tribute page is the picked member's tribute
-- line (state, donations, last donation, from the stock TributeList, whose rows are MemberList's) and its opt-out
-- (yourself, or others with the guild permission). The Banner page shows the active banner, kits and appearance.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local K = dofile("lib/keyboard.lua")
local CH = dofile("lib/child.lua")
local TB = dofile("lib/textbox.lua")
local C = T.C

local GW = "GuildManagementWnd"
local listState = {}
local wasShown = false
local state = {} -- the question (W.confirm)
local kb = K.new()
local side = {} -- W.toSide
local bannerPage = false
local bannerControls, bannerList, bannerDescription, bannerSide = CH.new(), {}, {}, {}
local bannerWasEditing = false
-- The window's ShowOffline / HideAlts toggles (its list shows only the members they let through; kept in UI.ini).
local function toggle(id) local _, c = API.windows.ChildState(GW, id) return c == true end

-- The members as the stock list has them (MemberList: name, level, class, rank, alt, last on, zone, public note),
-- with their row (0-based) and whether they are online (the zone is shown only for those).
local function members()
  local rows, _, selected = API.windows.ListInfo(GW, "MemberList")
  local out = {}
  for i = 0, (rows or 0) - 1 do
    local r = API.windows.ListRow(GW, "MemberList", i)
    if r then
      out[#out + 1] = { row = i, name = r[1] or "", level = r[2] or "", class = r[3] or "", rank = r[4] or "",
        alt = (r[5] or "") ~= "", lastOn = r[6] or "", zone = r[7] or "", note = r[8] or "", online = (r[7] or "") ~= "" }
    end
  end
  return out, selected
end

local function targetName()
  local _, name, raw = API.character.Gauge(6)
  if raw == -1 or not name or name == "" then return nil end
  return name
end

local function enabled(child) return API.windows.ChildState(GW, child) ~= false end

-- The Access page's rank box of a member (the child "GT_Member_List:<row>"): its child, choices and the chosen one.
local function rankBox(name)
  for i = 0, (API.windows.ListInfo(GW, "GT_Member_List") or 0) - 1 do
    local r = API.windows.ListRow(GW, "GT_Member_List", i)
    if r and r[2] == name then
      local child = "GT_Member_List:" .. i
      local choices, chosen = API.windows.ComboInfo(GW, child)
      return child, choices or {}, chosen
    end
  end
end

-- The rank list's row of a rank name (for its rename: the list's double click).
local function rankRow(rank)
  for i = 0, (API.windows.ListInfo(GW, "GT_Rank_List") or 0) - 1 do
    local r = API.windows.ListRow(GW, "GT_Rank_List", i)
    if r and r[2] == rank then return i end
  end
end

local ranking = nil -- the member whose new rank is being picked: { name, child, choices, chosen }
local rankFocused = nil

-- Testing: the pack's settings file test.open = ...,guild opens it once, 6 s after
-- zoning in (closed first when the game restored it, so it takes the pad as a window opened in play does).
-- (the setting does not change in play: once it is read without guild, the hook stays off, as M.testShow does)
local testAt, testOff
local function testOpen()
  if testOff or GPSTATE["test_guild"] or not API.session.InGame() then return end
  if not (","..(ui.GetSetting("test.open") or "")..","):find(",guild,", 1, true) then
    testOff = true
    return
  end
  testAt = testAt or ui.GetTime()
  if ui.GetTime() - testAt > 6 then
    GPSTATE["test_guild"] = true
    API.windows.Show(GW, true)
  elseif ui.GetTime() - testAt > 5 then
    API.windows.Show(GW, false)
  end
end

-- The message of the day without its markup, on one line, and shortened to `w` (kept while the text and the size
-- are: it can be a long text).
local motdRaw, motdW, motdS, motdPlain, motdFit = nil, nil, nil, "", ""
local function motdLine(raw, w)
  if raw ~= motdRaw then
    motdRaw, motdW = raw, nil
    motdPlain = (raw:gsub("<[^>]*>", ""):gsub("%s+", " "))
  end
  if w ~= motdW or T.S ~= motdS then
    motdW, motdS = w, T.S
    motdFit = T.fit(motdPlain ~= "" and motdPlain or "(none)", w)
  end
  return motdPlain, motdFit
end

local BANNER_LAYERS = { "BannerBackgroundPreview", "BannerEmblemPreview" }

local function bannerPreview(x, y, size)
  local drawn = false
  for _, id in ipairs(BANNER_LAYERS) do
    local tex, u0, v0, u1, v1 = API.windows.ChildArt(GW, id, "decal")
    if type(tex) == "userdata" then
      local r, g, b, a = API.windows.ChildColor(GW, id, "decal")
      ui.DrawImage(tex, x, y, x + size, y + size, u0, v0, u1, v1, r or 1, g or 1, b or 1, a or 1)
      drawn = true
    end
  end
  return drawn
end

local function banners(x0, y0, x1, y1, wx, wy)
  local leftW = (x1 - x0) * 0.59
  local rightX = x0 + leftW + T.px(24)
  local bw, bh = x1 - rightX, T.px(64)
  local buttons, focusedRow
  local editing = API.windows.ChildShown(GW, "SaveBannerSettings") == true
  local enteredEditor = editing and not bannerWasEditing
  if bannerWasEditing and not editing then bannerList.focusNext = 1 end
  bannerWasEditing = editing
  T.font(22)
  T.draw(x0, y0, CH.text(GW, "TimeRemainingValueLabel"), C.text)
  T.unfont()
  if editing then
    local size = math.min(leftW, T.px(320))
    local y = y0 + T.px(48)
    if bannerPreview(x0, y, size) then y = y + size + T.px(20) end
    T.font(20)
    T.draw(x0, y, "Background pattern " .. CH.text(GW, "BannerBackgroundPattern"), C.dim)
    T.draw(x0, y + T.px(30), "Emblem pattern " .. CH.text(GW, "BannerEmblemPattern"), C.dim)
    T.unfont()
    buttons = {
      { "Layer1TexturePrev", "Previous background" }, { "Layer1TextureNext", "Next background" },
      { "Layer1ColorBtn", "Background color" }, { "Layer2TexturePrev", "Previous emblem" },
      { "Layer2TextureNext", "Next emblem" }, { "Layer2ColorBtn", "Emblem color" },
      { "SaveBannerSettings", "Save" }, { "CancelEditBanner", "Cancel" },
    }
  else
    local y = y0 + T.px(48)
    local half = (leftW - T.px(10)) / 2
    ui.SetCursorPos(x0 - wx, y - wy)
    CH.combo(bannerControls, GW, "BannerFilter", "Kit type", half, T.px(70), { keep = true })
    ui.SameLine(0, T.px(10))
    CH.toggle(GW, "ShowAllPossibleBannersButton", "Show all kits", half, T.px(70), { keep = true })
    y = y + T.px(86)
    local listH = (y1 - y) * 0.57
    ui.SetCursorPos(x0 - wx, y - wy)
    local noData = API.windows.ChildShown(GW, "BannerData") == false
    focusedRow = CH.list(GW, "BannerKitList", bannerList, leftW, listH, { rowH = 52, font = 19,
      empty = noData and "No banner data from this server." or "No banner kits listed.", columns = { { 2, 1 } } })
    W.toSide(bannerSide, focusedRow)
    y = y + listH + T.px(15)
    local size = math.min(T.px(154), y1 - y)
    local textX = x0
    if size > 0 and bannerPreview(x0, y, size) then textX = x0 + size + T.px(15) end
    TB.draw(CH.text(GW, "BannerDescription"), textX, y, x0 + leftW, y1, bannerDescription)
    buttons = {
      { "PlantBannerButton", "Plant banner", "At your location" }, { "RemoveBannerButton", "Remove banner" },
      { "EditBannerButton", "Edit appearance" }, { "RefreshBannerListButton", "Refresh kits" },
    }
  end
  for i, b in ipairs(buttons) do
    ui.SetCursorPos(rightX - wx, y0 + (i - 1) * (bh + T.px(9)) - wy)
    if not editing then W.sideBefore(bannerSide, i) end
    if enteredEditor and i == 3 then ui.SetKeyboardFocusHere() end
    CH.button(GW, b[1], b[2], bw, bh, { sub = b[3], align = 0, primary = b[1] == "SaveBannerSettings" })
    if not editing then W.sideAfter(bannerSide, i, #buttons, bannerList) end
  end
end

ui.window("Guild", function()
  testOpen()
  if not W.shown(GW) then
    kb.open, state.confirm = false, nil
    ranking, rankFocused = nil, nil -- (the picker is for the member picked while it was open)
    bannerControls.pick, bannerWasEditing = nil, false
    wasShown = false
    return
  end
  T.frame()
  -- the pad's focus starts on the list
  local selectedPage = API.windows.TabPage(GW, "Subwindows") == 4
  if not wasShown or selectedPage ~= bannerPage then
    bannerPage = selectedPage
    if bannerPage then bannerList.focusNext = 1 else listState.focusNext = 1 end
  end
  wasShown = true
  local name = API.windows.Text(GW, "GuildName")
  local inGuild = name ~= nil and name ~= ""
  local showOffline, hideAlts = toggle("ShowOfflineButton"), toggle("HideAltButton")
  local all, selectedRow = members()
  local view, selected = {}, nil
  local online = 0
  for _, m in ipairs(all) do
    if m.online then online = online + 1 end
    if (showOffline or m.online) and not (hideAlts and m.alt) then view[#view + 1] = m end
    if m.row == selectedRow then selected = m end
  end
  local hints = { { "b", "Close" } }
  if inGuild then
    local bannerBack = API.windows.ChildShown(GW, "SaveBannerSettings") == true and "Cancel" or "Close"
    hints = bannerPage and CH.hints(bannerControls, { { "lb", "Roster" }, { "a", "Select" }, { "b", bannerBack } })
      or { { "rb", "Banner" }, { "x", showOffline and "Hide offline" or "Show offline" },
        { "y", hideAlts and "Show alts" or "Hide alts" }, { "a", "Select" },
        { "b", (kb.open or state.confirm) and "Cancel" or "Close" } }
  end
  if W.begin("Guild", { w = 1720, h = 960, title = inGuild and name or "Guild",
        subtitle = inGuild and (bannerPage and "Banner" or #all .. " members, " .. online .. " online") or "",
        hints = kb.open and K.HINTS or hints }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if inGuild and not kb.open and not state.confirm and not ranking and not CH.busy(bannerControls) then
      ui.SetCursorPos(x0 - wx, y0 - wy)
      local rosterButton = T.button("##guildRosterPage", "Roster", T.px(150), T.px(50), { primary = not bannerPage }, pad)
      ui.SameLine()
      local bannerButton = T.button("##guildBannerPage", "Banner", T.px(150), T.px(50), { primary = bannerPage }, pad)
      if bannerPage and (rosterButton or W.pressed("lb")) then
        bannerPage = false
        API.windows.SetTabPage(GW, "Subwindows", 0)
        listState.focusNext = 1
      elseif not bannerPage and (bannerButton or W.pressed("rb")) then
        bannerPage = true
        API.windows.SetTabPage(GW, "Subwindows", 4)
        bannerList.focusNext = 1
      end
      y0 = y0 + T.px(64)
      if not bannerPage then
        if W.pressed("x") then API.windows.ClickChild(GW, "ShowOfflineButton", "left") end
        if W.pressed("y") then API.windows.ClickChild(GW, "HideAltButton", "left") end
      end
    elseif inGuild and kb.open then
      y0 = y0 + T.px(64)
    end
    if state.confirm then
      W.confirm(state)
    elseif ranking then
      -- the ranks the game lets this member be moved to
      T.font(24)
      T.draw(x0, y0, "New rank for " .. ranking.name, C.text)
      T.unfont()
      local gw, gh = (cw - 3 * T.px(14)) / 4, T.px(96)
      for i, ch in ipairs(ranking.choices) do
        if (i - 1) % 4 ~= 0 then ui.SameLine(0, T.px(14)) else ui.SetCursorPos(x0 - wx, y0 + T.px(56) + (i - 1) // 4 * (gh + T.px(14)) - wy) end
        if i == 1 and rankFocused ~= ranking then
          ui.SetKeyboardFocusHere()
          rankFocused = ranking
        end
        local current = i - 1 == ranking.chosen
        if T.button("##rk" .. i, ch, gw, gh, { sub = current and "Current" or nil, primary = current }, pad) and ui.GetTime() - ranking.at > 0.4 then
          if not current then
            -- the member's rank box found again by name: the Access page's rows may have changed since the picker
            -- opened, and a row number would then set another member's rank; the choice is checked to still be
            -- the one shown
            local child, choices = rankBox(ranking.name)
            if child and choices[i] == ch then API.windows.ComboSelect(GW, child, i - 1) end
          end
          ranking, rankFocused = nil, nil
          listState.focusNext = 1
          break
        end
      end
    elseif not inGuild then
      T.draw(x0, y0, "You are not in a guild.", C.dim)
      ui.Dummy(1, 1)
    elseif bannerPage then
      if not CH.modal(bannerControls, x0, y0, x1, y1) then banners(x0, y0, x1, y1, wx, wy) end
    else
      -- the message of the day
      local motd = API.windows.Text(GW, "MOTDViewer") or ""
      local author = API.windows.Text(GW, "MOTDAuthorLabel") or ""
      local mh = T.px(96)
      local bw = T.px(260)
      T.rect(x0, y0, x1 - bw - T.px(14), y0 + mh, C.sunken, 1, T.px(12))
      T.font(17)
      T.draw(x0 + T.px(16), y0 + T.px(10), "MESSAGE OF THE DAY" .. (author ~= "" and ("  -  " .. author) or ""), C.dim)
      T.unfont()
      T.font(22)
      local plain, line = motdLine(motd, x1 - bw - x0 - T.px(50))
      T.draw(x0 + T.px(16), y0 + T.px(44), line, plain ~= "" and C.text or C.faint)
      T.unfont()
      ui.SetCursorPos(x1 - bw - wx, y0 - wy)
      if T.button("##motd", "Set MOTD", bw, mh, { disabled = not enabled("UpdateMOTDButton") or kb.open }, pad) then
        K.open(kb, "The guild's message of the day", plain, 255)
        kb.purpose = "motd"
      end
      -- the members
      local ly = y0 + mh + T.px(18)
      local listW = cw * 0.6
      T.font(17)
      local cols = { 0.02, 0.33, 0.58, 0.75 }
      local labels = { "NAME", "LEVEL / CLASS", "RANK", "ZONE / LAST ON" }
      for i, l in ipairs(labels) do T.draw(x0 + listW * cols[i], ly, l, C.dim) end
      T.unfont()
      ly = ly + T.px(32)
      ui.SetCursorPos(x0 - wx, ly - wy)
      if #view == 0 then
        T.draw(x0, ly + T.px(10), showOffline and "No members." or "No members online (X shows the offline ones).", C.dim)
        ui.Dummy(1, 1)
      else
        listState.selected = nil
        for i, m in ipairs(view) do if m.row == selectedRow then listState.selected = i end end
        local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(56) + T.px(10))))
        listState.shown = visible
        local focusedRow = W.list("members", listState, #view, visible, 56, listW, function(i, rx0, ry0, rx1, ry1)
          local m = view[i]
          local col = m.online and C.text or C.faint
          if i == listState.selected then col = C.accent end
          local w = rx1 - rx0
          T.font(22)
          T.textIn(rx0 + w * cols[1], ry0, rx0 + w * cols[2] - T.px(8), ry1, m.name .. (m.alt and " (alt)" or ""), col, 0)
          T.textIn(rx0 + w * cols[2], ry0, rx0 + w * cols[3] - T.px(8), ry1, m.level .. " " .. m.class, col, 0)
          T.textIn(rx0 + w * cols[3], ry0, rx0 + w * cols[4] - T.px(8), ry1, m.rank, col, 0)
          T.textIn(rx0 + w * cols[4], ry0, rx1 - T.px(12), ry1, m.online and m.zone or m.lastOn, col, 0)
          T.unfont()
        end, function(i) API.windows.ListClick(GW, "MemberList", view[i].row, "left") end)
        W.toSide(side, focusedRow)
      end
      -- the right side: the keyboard, or the actions
      local rx0 = x0 + listW + T.px(30)
      local rw = x1 - rx0
      local ry = y0 + mh + T.px(18)
      local function at(y) ui.SetCursorPos(rx0 - wx, y - wy) end
      if kb.open then
        local r = K.draw(kb, pad, rx0, ry, rw)
        if r == "ok" and kb.purpose == "motd" then
          API.windows.SetText(GW, "MOTDInput", kb.text)
          API.windows.ClickChild(GW, "UpdateMOTDButton", "left")
        elseif r == "ok" and kb.purpose == "personal" then
          API.windows.SetText(GW, "PersonalInput", kb.text)
          API.windows.ClickChild(GW, "UpdatePersonalButton", "left")
        end
      else
        local bh = T.px(54) -- (ten actions under the invite)
        local target = targetName()
        at(ry)
        if T.button("##invite", "Invite your target", rw, bh, { sub = target or "No target",
              disabled = not enabled("InviteButton"), align = 0 }, pad) then
          local who = target
          state.confirm = { "Invite " .. (who or "your target") .. " into the guild?", function()
            API.windows.ClickChild(GW, "InviteButton", "left")
          end }
        end
        ry = ry + bh + T.px(20)
        T.font(18)
        T.draw(rx0, ry, selected and (selected.name:upper() .. "  " .. selected.rank:upper()) or "SELECT A MEMBER ON THE LEFT", C.dim)
        T.unfont()
        ry = ry + T.px(34)
        local who = selected and selected.name or ""
        local boxChild, boxChoices, boxChosen
        if selected then boxChild, boxChoices, boxChosen = rankBox(who) end
        local personal = selected and (API.windows.ListRow(GW, "NoteList", selected.row) or {})[3] or ""
        local renameRow = selected and rankRow(selected.rank)
        -- the Tribute page's row of the member: name, tribute state, trophy state, donations, last donation
        local trib = selected and API.windows.ListRow(GW, "TributeList", selected.row) or {}
        local tribSub = selected and ("Tribute " .. (trib[2] or "") .. ", donated " .. (trib[4] or "0") ..
          ((trib[5] or "") ~= "" and (", last " .. trib[5]) or "")) or "Select a member"
        local actions = {
          { "##tell", "Send a tell", "Opens the chat with /tell", selected ~= nil and selected.online, function()
            W.chat("/tell " .. who .. " ")
          end },
          { "##promote", "Promote", "One rank up", enabled("PromoteButton"), function()
            state.confirm = { "Promote " .. who .. "?", function() API.windows.ClickChild(GW, "PromoteButton", "left") end }
          end },
          { "##demote", "Demote", "One rank down", enabled("DemoteButton"), function()
            state.confirm = { "Demote " .. who .. "?", function() API.windows.ClickChild(GW, "DemoteButton", "left") end }
          end },
          { "##leader", "Make leader", "Hand the guild over", enabled("MakeLeaderButton"), function()
            state.confirm = { "Make " .. who .. " the guild leader?", function() API.windows.ClickChild(GW, "MakeLeaderButton", "left") end }
          end },
          { "##remove", "Remove", "Out of the guild", enabled("RemoveButton") and selected ~= nil, function()
            state.confirm = { "Remove " .. who .. " from the guild?", function() API.windows.ClickChild(GW, "RemoveButton", "left") end }
          end },
          { "##rank", "Set rank", "Pick from the ranks you may give", boxChild ~= nil and enabled(boxChild) and #boxChoices > 1,
            function() ranking = { name = who, child = boxChild, choices = boxChoices, chosen = boxChosen, at = ui.GetTime() } end },
          { "##personal", "Personal note", personal ~= "" and personal or "Only you see it", selected ~= nil, function()
            K.open(kb, "Your note on " .. who, personal, 255)
            kb.purpose = "personal"
          end },
          { "##tribute", "Tribute opt out", tribSub, enabled("TributeOptOutButton"), function()
            state.confirm = { "Opt " .. who .. " out of the guild tribute?", function()
              API.windows.ClickChild(GW, "TributeOptOutButton", "left")
            end }
          end },
          { "##renamerank", "Rename rank", selected and selected.rank or "", renameRow ~= nil, function()
            -- the rank list's double click: the stock text entry asks for the name (where the game allows it)
            API.windows.ListClick(GW, "GT_Rank_List", renameRow, "double")
            if pad.navVisible() then pad.set_mode("cursor") end
          end },
        }
        for k, a in ipairs(actions) do
          at(ry)
          W.sideBefore(side, k)
          if T.button(a[1], a[2], rw, bh, { sub = a[3], disabled = not a[4], align = 0 }, pad) then a[5]() end
          W.sideAfter(side, k, #actions, listState)
          ry = ry + bh + T.px(8)
        end
      end
    end
  end
  local close = W.finish()
  if close and ranking then
    close, ranking, rankFocused = false, nil, nil
    listState.focusNext = 1
  end -- B leaves the rank picker
  if bannerPage then
    if not CH.back(bannerControls, close) then return end
    if API.windows.ChildShown(GW, "SaveBannerSettings") == true then
      API.windows.ClickChild(GW, "CancelEditBanner", "left")
      return
    end
  end
  if W.back(close, state, kb, listState) then API.windows.Show(GW, false) end
end)
