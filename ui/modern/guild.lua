-- Guild: replaces the game's GuildManagementWnd. The guild's name and how many are on, then the game's pages:
--   * Roster: the member list as a sortable table (click a header) with a search (name, class, rank, zone, note)
--     and the window's Show offline / Hide alts toggles; offline members are greyed as the game colours them. The member actions
--     under it (Invite your target, Remove, Promote, Demote, Make Leader, the Alt flag) are the window's buttons:
--     the game enables them for a selected member.
--   * Notes: every member's public note and your personal note on them (kept on this computer); selecting a member
--     puts the notes in the fields below, Update sets each.
--   * Information: the message of the day (who set it, and a field to set a new one when your rank may), the
--     guild's chat channel (Join) and web page (View), each with its Update.
--   * Ranks: the members with a rank drop-down each (the ranks the game lets you move them to), how many members
--     hold each rank (double-click a rank to rename it where the game allows), and which permissions each rank has
--     (click a cell to change it where the game allows; the others are greyed).
--   * Tribute: each member's guild tribute and trophy tribute state, donations and last donation (yellow states
--     are the game's own colouring), the guild's favour, and the opt-out buttons for the selected member (the game
--     enables them for yourself, or for others with the guild permission).
--   * Banner: the guild's active banner, catalog of kits, and appearance editor on servers that send banner data.
-- Dump writes the roster to a file.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local S = dofile("lib/social.lua")
local T = hud.T
local C = T.color
local WINDOW = "GuildManagementWnd"

local search = ""

-- The roster's columns (the window's): 1 name, 2 level, 3 class, 4 rank, 5 alt, 6 last on, 7 zone, 8 public note.
local function online(texts) return (texts[7] or "") ~= "" end

-- The members listed and how many are on: the rows' texts only (S.Rows would make two more tables a row, every frame).
local function rosterCounts()
  local rows = API.windows.ListInfo(WINDOW, "MemberList")
  local total, on = 0, 0
  for r = 0, (rows or 0) - 1 do
    local texts = API.windows.ListRow(WINDOW, "MemberList", r)
    if texts then
      total = total + 1
      if online(texts) then on = on + 1 end
    end
  end
  return total, on
end

-- (the roster list's options and filter are made once; the filter reads this frame's search)
local ROSTER = {
  sortable = true,
  hide = { [9] = true, [10] = true },
  columns = { [5] = { "Alt", 30 }, [8] = { "Public Note", 0 } },
}
local ROSTER_SEARCHED = { 1, 3, 4, 7, 8 }
local rosterNeedle = ""
local function rosterFilter(_, texts) return S.Matches(texts, rosterNeedle, ROSTER_SEARCHED) end

local function roster(s)
  local btnH = S.ButtonHeight(s)
  -- the views: a search and two toggles on one line
  local tw = 220 * s
  search = S.Search("guildsearch", search, "Search name, class, rank, zone or note", -tw)
  ui.SameLine()
  W.Check(WINDOW, "ShowOfflineButton", "Show offline")
  ui.SameLine()
  W.Check(WINDOW, "HideAltButton", "Hide alts")
  local needle = search:lower()
  local total = API.windows.ListInfo(WINDOW, "MemberList") or 0
  local wide = ui.GetContentRegionAvail() >= 7 * 96 * s -- the actions on one row, else two
  local below = (wide and 2 or 3) * btnH + (wide and 16 or 22) * s
  if total == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(40 * s, ah - below), "No guild members to show.",
      "Join or create a guild; its roster arrives from the server.")
  else
    rosterNeedle = needle
    ROSTER.filter = needle ~= "" and rosterFilter or nil
    ROSTER.filterKey = needle
    W.List(WINDOW, "MemberList", 0, -below, ROSTER)
  end
  -- the selected member, then the actions
  local rows, _, sel = API.windows.ListInfo(WINDOW, "MemberList")
  if rows and sel and sel >= 0 then
    local t = API.windows.ListRow(WINDOW, "MemberList", sel)
    if t then
      ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Selected")
      ui.SameLine()
      ui.TextColored(C.text[1], C.text[2], C.text[3], 1, t[1] or "")
      ui.SameLine()
      local info = (t[2] or "") .. " " .. (t[3] or "") .. "  \xC2\xB7  " .. (t[4] or "")
      ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, info)
    end
  else
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Select a member for the rank and roster actions.")
  end
  local buttons = {
    { "InviteButton", "Invite", { primary = true, tooltip = "Invite your target to the guild" } },
    { "PromoteButton", "Promote", { tooltip = "Raise the selected member's rank" } },
    { "DemoteButton", "Demote", { tooltip = "Lower the selected member's rank" } },
    { "MakeLeaderButton", "Make Leader", { tooltip = "Hand the guild's leadership to the selected member" } },
    { "AltFlagButton", "Alt", { tooltip = "Mark or unmark the selected member as an alt" } },
    { "RemoveButton", "Remove", { danger = true, tooltip = "Remove the selected member from the guild" } },
    { "DumpButton", "Dump", { tooltip = "Write the roster with its notes to a file" } },
  }
  if wide then
    W.ButtonRow(WINDOW, buttons, btnH)
  else -- narrow: two rows
    W.ButtonRow(WINDOW, { buttons[1], buttons[2], buttons[3], buttons[4] }, btnH)
    W.ButtonRow(WINDOW, { buttons[5], buttons[6], buttons[7] }, btnH)
  end
end

local function notes(s)
  local btnH = S.ButtonHeight(s)
  W.List(WINDOW, "NoteList", 0, -(btnH * 4 + 60 * s), {
    sortable = true,
    columns = { [2] = { "Public Note", 0 }, [3] = { "Personal Note", 200 } },
  })
  W.Heading("Public note of the selected member")
  local btnW = 80 * s
  W.Edit(WINDOW, "PublicInput", { hint = "Select a member above", width = -(btnW + 8 * s) })
  ui.SameLine()
  W.Button(WINDOW, "UpdatePublicButton", "Update", { width = btnW, primary = true,
    tooltip = "Set this note for the selected member" })
  W.Heading("Your personal note (only you see it)")
  W.Edit(WINDOW, "PersonalInput", { hint = "Select a member above", width = -(btnW + 8 * s), max = 255 })
  ui.SameLine()
  W.Button(WINDOW, "UpdatePersonalButton", "Update", { width = btnW,
    tooltip = "Keep this note on the selected member" })
end

local function motd(s)
  local btnH = S.ButtonHeight(s)
  local btnW = 80 * s
  local author = W.Text(WINDOW, "MOTDAuthorLabel")
  if author ~= "" then W.Field("Set by", author) end
  local below = btnH * 4 + 90 * s -- the new message, channel and web page rows
  if W.Shown(WINDOW, "MOTDViewer") then
    local text = W.Text(WINDOW, "MOTDViewer")
    if text == "" then
      local aw, ah = ui.GetContentRegionAvail()
      S.Empty(aw, math.max(30 * s, ah - below), "No message of the day.")
    else
      W.Stml(WINDOW, "MOTDViewer", 0, -below)
    end
  end
  if W.Shown(WINDOW, "MOTDInput") then
    W.Heading("Message of the day")
    W.Edit(WINDOW, "MOTDInput", { hint = "The message members see when they log in", width = -(btnW + 8 * s), max = 512 })
    ui.SameLine()
    W.Button(WINDOW, "UpdateMOTDButton", "Update", { width = btnW, primary = true })
  end
  W.Heading("Guild channel")
  W.Edit(WINDOW, "ChannelInput", { hint = "The guild's chat channel (the leader sets it)", width = -(btnW * 2 + 16 * s) })
  ui.SameLine()
  W.Button(WINDOW, "UpdateChannel", "Update", { width = btnW })
  ui.SameLine()
  W.Button(WINDOW, "JoinChannel", "Join", { width = btnW, tooltip = "Join the guild's chat channel" })
  W.Heading("Guild web page")
  W.Edit(WINDOW, "URLInput", { hint = "The guild's web address", width = -(btnW * 2 + 16 * s), max = 512 })
  ui.SameLine()
  W.Button(WINDOW, "UpdateURL", "Update", { width = btnW })
  ui.SameLine()
  W.Button(WINDOW, "GotoURL", "View", { width = btnW, tooltip = "Open the guild's web page" })
end

local function tribute(s)
  local btnH = S.ButtonHeight(s)
  W.Field("Guild favour", API.character.Label(122) or "")
  local total = API.windows.ListInfo(WINDOW, "TributeList") or 0
  if total == 0 then
    local aw, ah = ui.GetContentRegionAvail()
    S.Empty(aw, math.max(40 * s, ah - btnH - 16 * s), "No guild members to show.")
  else
    W.List(WINDOW, "TributeList", 0, -(btnH + 16 * s), {
      sortable = true,
      columns = { [2] = { "Tribute", 90 }, [3] = { "Trophy", 90 }, [4] = { "Donated", 90 }, [5] = { "Last Donation", 0 } },
    })
  end
  W.ButtonRow(WINDOW, {
    { "TributeOptOutButton", "Tribute Opt Out", { tooltip = "Opt the selected member out of the guild tribute's benefits" } },
    { "TrophyTributeOptOutButton", "Trophy Opt Out", { tooltip = "Opt the selected member out of the trophy tribute's benefits" } },
  }, btnH)
end

local BANNER_LAYERS = { "BannerBackgroundPreview", "BannerEmblemPreview" }

local function bannerPreview(size)
  local x, y = ui.GetCursorScreenPos()
  local drawn = false
  for _, id in ipairs(BANNER_LAYERS) do
    local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, id, "decal")
    if type(tex) == "userdata" then
      local r, g, b, a = API.windows.ChildColor(WINDOW, id, "decal")
      ui.DrawImage(tex, x, y, x + size, y + size, u0, v0, u1, v1, r or 1, g or 1, b or 1, a or 1)
      drawn = true
    end
  end
  if drawn then ui.Dummy(size, size) end
  return drawn
end

local function banner(s)
  local btnH = S.ButtonHeight(s)
  W.Field("Banner time left", W.Text(WINDOW, "TimeRemainingValueLabel"))
  if W.Shown(WINDOW, "SaveBannerSettings") then
    if ui.BeginChild("##bannerAppearance", 0, -(btnH + 4 * s), false) then
      bannerPreview(80 * s)
      W.Field("Background pattern", W.Text(WINDOW, "BannerBackgroundPattern"))
      W.ButtonRow(WINDOW, {
        { "Layer1TexturePrev", "Previous" }, { "Layer1TextureNext", "Next" }, { "Layer1ColorBtn", "Choose color" },
      }, btnH)
      W.Field("Emblem pattern", W.Text(WINDOW, "BannerEmblemPattern"))
      W.ButtonRow(WINDOW, {
        { "Layer2TexturePrev", "Previous" }, { "Layer2TextureNext", "Next" }, { "Layer2ColorBtn", "Choose color" },
      }, btnH)
    end
    ui.EndChild()
    W.ButtonRow(WINDOW, {
      { "SaveBannerSettings", "Save", { primary = true } }, { "CancelEditBanner", "Cancel" },
    }, btnH)
    return
  end
  W.Combo(WINDOW, "BannerFilter", { width = -210 * s })
  if ui.IsItemHovered() then ui.SetTooltip("Filter banner kits by type") end
  ui.SameLine()
  W.Check(WINDOW, "ShowAllPossibleBannersButton", "Show all kits")
  ui.SameLine()
  W.Button(WINDOW, "RefreshBannerListButton", "Refresh", { tooltip = "Ask for the banner kit list again" })
  local total = API.windows.ListInfo(WINDOW, "BannerKitList") or 0
  local aw, ah = ui.GetContentRegionAvail()
  local detail = W.Text(WINDOW, "BannerDescription")
  local showPreview = API.windows.ChildShown(WINDOW, "BannerBackgroundPreview") == true
  local detailH = (detail ~= "" or showPreview) and math.min(105 * s, math.max(30 * s, ah / 3)) or 0
  local below = btnH + 16 * s + detailH
  if total == 0 then
    local noData = API.windows.ChildShown(WINDOW, "BannerData") == false
    S.Empty(aw, math.max(30 * s, ah - below), noData and "No banner data from this server." or "No banner kits listed.")
  else
    W.List(WINDOW, "BannerKitList", 0, -below, { hide = { [1] = true }, columns = { [2] = { "Banner", 0 } } })
  end
  if detailH > 0 then
    if bannerPreview(detailH) and detail ~= "" then ui.SameLine() end
    if detail ~= "" then W.Stml(WINDOW, "BannerDescription", 0, detailH - 4 * s) end
  end
  W.ButtonRow(WINDOW, {
    { "PlantBannerButton", "Plant Banner", { primary = true, tooltip = "Place the selected banner where you stand" } },
    { "RemoveBannerButton", "Remove Banner", { danger = true, tooltip = "Take the guild's banner out of the world" } },
    { "EditBannerButton", "Edit", { tooltip = "Change the banner's layers and colours" } },
  }, btnH)
end

local function ranks(s)
  -- the game's two views of the list above the permissions: the members (a rank drop-down each) or the ranks
  local members = API.windows.ChildShown(WINDOW, "GT_Member_List") ~= false
  W.Button(WINDOW, "GT_ViewMembers_Btn", "Members", { primary = members, tooltip = "Each member's rank" })
  ui.SameLine()
  W.Button(WINDOW, "GT_ViewRanks_Btn", "Ranks", { primary = not members, tooltip = "The ranks and how many hold each" })
  local listH = 9.5 * (select(2, ui.CalcTextSize("Ag")) + 4)
  if members then
    W.List(WINDOW, "GT_Member_List", 0, listH, {
      hide = { [1] = true, [4] = true }, columns = { [2] = { "Member", 0 }, [3] = { "Rank", 180 } } })
  else
    W.List(WINDOW, "GT_Rank_List", 0, listH, {
      hide = { [1] = true }, columns = { [2] = { "Rank", 0 }, [3] = { "Members", 70 } } })
    if ui.IsItemHovered() then ui.SetTooltip("Double-click a rank to rename it") end
  end
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Permissions by rank (1 Leader .. 8 Recruit)")
  W.List(WINDOW, "GT_Permissions_List", 0, 0, { columns = { [1] = { "Permission", 0 } } })
end

local PAGES = {
  { "Roster", 0 }, { "Notes", 1 }, { "Tribute", 2 }, { "Information", 3 }, { "Banner", 4 }, { "Ranks", 5 },
}

local OPTS = {
  size = function() return 760, 480 end,
  resizable = { 460, 300 },
  keepTitle = true,
}
local function body(x, y, w, h, s)
  S.TestClicks(WINDOW)
  local name = W.Text(WINDOW, "GuildName")
  local total, on = rosterCounts()
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, name ~= "" and name or "No guild")
  ui.PopFont()
  ui.SameLine(0, 10 * s)
  S.Chip(total .. (total == 1 and " member" or " members"), C.muted)
  ui.SameLine(0, 6 * s)
  S.Chip(on .. " online", on > 0 and C.good or C.faint)
  -- the game's pages (the ranks page is Access's)
  local shown = W.Tabs(WINDOW, "Subwindows", PAGES)
  if shown == 1 then notes(s)
  elseif shown == 2 then tribute(s)
  elseif shown == 3 then motd(s)
  elseif shown == 4 then banner(s)
  elseif shown == 5 then ranks(s)
  else roster(s) end
end

ui.window("Guild Management", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Guild", OPTS, body)
end)
