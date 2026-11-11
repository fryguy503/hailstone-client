-- The display and layout windows, and the main menu's More list (gamepad pack). The windows stand in for the stock
-- ones and are answered through them (lib/model.lua), so what is set, loaded or copied is the game's:
--   Video Modes (VideoModesWindow)     this client's display sizes (A picks one), in full screen its refresh rates and
--                                      bit depth (A opens their choices), Accept (in full screen the game asks to keep
--                                      the mode; app/lua_eq_video.cpp)
--   Load Skin (LoadskinWnd, /loadskin) the user interfaces (A picks one), Keep Your Layout, Load Skin
--                                      (app/lua_eq_layout.cpp)
--   Copy Layout (LayoutCopyWindow,     the layouts to copy from (A picks one), what to copy with it (hot buttons,
--   /copylayout)                       their loadouts, socials; Hailstone's chat and options) and Copy (the game
--                                      asks first); at a character's first entry it says why it came up and has a
--                                      toggle to stop offering it
--   More (the main menu's More tile)   the game's other windows, which only their keys, the EQ menu or a slash
--                                      command open otherwise: help, the storyline, the NPC journal, adventures,
--                                      expeditions, PvP, the zone guide, alerts, trophies, the display and
--                                      the user interface. A opens one (the stock window's show, as its key)
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local C = T.C

-- ---------------------------------------------------------------- video modes (VideoModesWindow)
local VW = "VideoModesWindow"
local sizes, vpick = {}, {}

ui.window("Video Modes", function()
  M.testShow()
  if not W.shown(VW) then return end
  T.frame()
  local hints = vpick.open and { { "a", "Choose" }, { "b", "Back" } } or { { "a", "Select" }, { "b", "Close" } }
  if W.begin("Video Modes", { w = 1200, h = 900, title = "Video Modes", hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.picker(vpick, x0, y0, x1, y1) then
      local midX = x0 + (x1 - x0) * 0.5
      local bh = T.px(76)
      ui.SetCursorPos(x0 - wx, y0 - wy)
      T.heading("Resolution")
      local ly = select(2, ui.GetCursorScreenPos())
      M.list(VW, "VIDW_VideoResolutionsList", sizes, x0, ly, midX - T.px(20), y1, { labels = { "" }, empty = "No display modes." })
      local cw = x1 - midX
      ui.SetCursorPos(midX - wx, ly - wy)
      ui.BeginGroup()
      M.combo(VW, "VIDW_RefreshRateCombobox", "Refresh rate", cw, bh, vpick, { none = "In full screen only" })
      M.combo(VW, "VIDW_BitDepthCombobox", "Bit depth", cw, bh, vpick, { none = "In full screen only" })
      M.button(VW, "VIDW_AcceptButton", "Accept", cw, bh, { primary = true })
      ui.EndGroup()
    end
  end
  if M.back(W.finish(), vpick, nil, nil, sizes) then eq.ShowWindow(VW, false) end
end)

-- ---------------------------------------------------------------- load skin (LoadskinWnd)
local LW = "LoadskinWnd"
local skins = {}

ui.window("Load Skin", function()
  if not W.shown(LW) then return end
  T.frame()
  if W.begin("Load Skin", { w = 1100, h = 820, title = "Load Skin", subtitle = "The user interfaces",
        hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local midX = x0 + (x1 - x0) * 0.5
    local bh = T.px(76)
    M.list(LW, "SkinList", skins, x0, y0, midX - T.px(20), y1, { empty = "No user interfaces found." })
    local cw = x1 - midX
    ui.SetCursorPos(midX - wx, y0 - wy)
    ui.BeginGroup()
    M.toggle(LW, "INIButton", "Keep Your Layout", cw, bh)
    M.button(LW, "LoadButton", "Load Skin", cw, bh, { primary = true })
    ui.EndGroup()
  end
  if W.finish() then eq.ShowWindow(LW, false) end
end)

-- ---------------------------------------------------------------- copy layout (LayoutCopyWindow)
local CW = "LayoutCopyWindow"
local layouts = {}

ui.window("Copy Layout", function()
  if not W.shown(CW) then return end
  T.frame()
  local first = eq.ChildShown(CW, "LC_FirstVisitText") == true
  if W.begin("Copy Layout", { w = 1400, h = 960, title = "Copy Layout",
        subtitle = first and "A new character: start from one of your others, or close to keep the default"
          or "Another character's (or the default) window layout over this one",
        hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local midX = x0 + (x1 - x0) * 0.58
    local bh = T.px(76)
    ui.SetCursorPos(x0 - wx, y0 - wy)
    T.heading("Source layouts")
    local ly = select(2, ui.GetCursorScreenPos())
    M.list(CW, "LC_LayoutsList", layouts, x0, ly, midX - T.px(20), y1, { empty = "No other layouts found." })
    local cw = x1 - midX
    ui.SetCursorPos(midX - wx, y0 - wy)
    ui.BeginGroup()
    T.heading("Copy with it")
    M.toggle(CW, "LC_CopyHotButtonCheckBox", "Hot Buttons", cw, bh)
    M.toggle(CW, "LC_CopyHotButtonLoadoutsCheckBox", "Loadouts", cw, bh)
    M.toggle(CW, "LC_CopySocialsCheckBox", "Socials", cw, bh)
    M.toggle(CW, "LC_CopyChatCheckBox", "Chat filters and colours", cw, bh)
    M.toggle(CW, "LC_CopyOptionsCheckBox", "Game options", cw, bh)
    if first then M.toggle(CW, "LC_OfferCheckBox", "Offer for new characters", cw, bh) end
    ui.Dummy(1, T.px(10))
    M.button(CW, "LC_CopyButton", "Copy", cw, bh, { primary = true })
    ui.EndGroup()
  end
  if W.finish() then eq.ShowWindow(CW, false) end
end)

-- ---------------------------------------------------------------- More (the main menu's tile)
-- { title, what it is, stock window [, open() in its place, shown() (the tile is there)] }
local TRACKING = 53 -- the skill: using it opens the tracking window (UseSkill; CTrackingWnd::AboutToShow)
local function tracks()
  for _, s in ipairs(eq.UsableSkills()) do if s.id == TRACKING then return true end end
  return false
end
local MORE = {
  { "Help", "The game's manual", "HelpWindow" },
  { "Report a Bug", "Saved with your logs and screenshots", "BugReportWindow" },
  { "Storyline", "The stories of the world", "StoryWnd" },
  { "NPC Journal", "What NPCs said to you", "JournalNPCWnd" },
  { "Adventures", "The adventure you are on", "AdventureRequestWnd" },
  { "Expeditions", "Members and lockouts", "DynamicZoneWnd" },
  { "PvP Stats", "Kills, deaths, the leaderboard", "PvPStatsWnd" },
  { "Find", "Merchants, bankers, trainers and more", nil, function() eq.FindOpen(true) end },
  { "Zone Path", "The zones on the way", "ZonePathWnd" },
  { "Alert History", "Alerts you closed", "AlertHistoryWnd" },
  { "Tribute Trophies", "Trophy tribute benefits", "TributeTrophyWnd" },
  { "Video Modes", "Display size and refresh rate", "VideoModesWindow" },
  { "Load Skin", "Change the user interface", "LoadskinWnd" },
  { "Copy Layout", "Another character's window layout", "LayoutCopyWindow" },
  { "Notes", "Your own notes (/note)", "PlayerNotesWindow" },
  { "Lua Scripts", "Run, stop and autorun your scripts", "ScriptsWnd", nil,
    function() return not (ui.ManagedDistribution and ui.ManagedDistribution()) end },
  { "Web Browser", "Web pages in the game (/web)", "BrowserWnd" },
  { "Tracking", "What is near (the Tracking skill)", nil, function() eq.UseSkill(TRACKING) end, tracks },
}
ui.window("More", function()
  if not eq.InGame() then
    GPSTATE.moreOpen = false
    return
  end
  if not GPSTATE.moreOpen then return end
  T.frame()
  if W.begin("More", { w = 1240, h = 940, title = "More", subtitle = "The game's other windows",
        hints = { { "a", "Open" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    -- two columns of buttons
    local list = {}
    for _, m in ipairs(MORE) do if not m[5] or m[5]() then list[#list + 1] = m end end
    -- the loaded plugins' entries (eq.PluginMenu; docs/plugins.md "Plugin windows")
    for i, e in ipairs(eq.PluginMenu()) do
      list[#list + 1] = { e.label, e.plugin, nil, function() eq.PluginMenuClick(i) end }
    end
    local cols, gap, bh = #list > 14 and 3 or 2, T.px(14), T.px(82)
    local bw = (x1 - x0 - (cols - 1) * gap) / cols
    for i, m in ipairs(list) do
      local c, r = (i - 1) % cols, (i - 1) // cols
      ui.SetCursorPos(x0 + c * (bw + gap) - wx, y0 + r * (bh + T.px(10)) - wy)
      if T.button("##more" .. i, m[1], bw, bh, { sub = m[2], align = 0 }, pad) then
        GPSTATE.moreOpen = false
        if m[4] then m[4]() else eq.ShowWindow(m[3], true) end
      end
    end
  end
  if W.finish() then GPSTATE.moreOpen = false end
end)
