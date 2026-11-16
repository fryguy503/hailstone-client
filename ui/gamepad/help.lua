-- Help, the storyline and the alert history (gamepad pack), standing in for the stock windows and answered through
-- them (lib/model.lua), so the pages, the history and the settings are the game's:
--   Help (HelpWindow)            the game's help\ page in a panel D-pad up / down scrolls (focused), its links as a list
--                                beside it (A follows one inside the window as clicking it in the stock page does:
--                                hs.StmlLink; the web links are not followed, as the window does not); LB goes Back a
--                                page, X shows the Contents, and the Reset Tips and Customer Service (/petition)
--                                buttons (app/lua_ui_help.cpp)
--   Story (StoryWnd)             the storyline\ pages, newest first: A on one shows it (the window marks it read),
--                                the page beside the list, and "Show new stories" (Automatic) (app/lua_ui_story.cpp)
--   Alert History                the alerts closed (A selects one) and Open Alert (app/lua_ui_fellowship.cpp; EQEmu
--   (AlertHistoryWnd)            sends no alerts, so the list stays empty)
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local P = dofile("lib/textpanel.lua")
local C = T.C

-- A pad shortcut of the current window, not in its first frames: the press that opened it is not one for it.
-- (GPSTATE.appeared holds the frame the window appeared in, lib/window.lua)
local function pressed(name, b)
  local at = GPSTATE.appeared and GPSTATE.appeared[name] or 0
  return ui.GetFrameCount() - at > 15 and W.pressed(b)
end

-- ---------------------------------------------------------------- help (HelpWindow)
local HW = "HelpWindow"
local page = { scroll = 0 }
local links = {}
local cache = { raw = nil, links = nil }

-- The page's links in order: { text, message, data } (as lib/stml.lua of the classic pack reads them: WndNotify="n,data"
-- is message n, href="target" message -1); the web links (a target with a ':' that is not file:///) are left out.
local function pageLinks(raw)
  if cache.raw == raw then return cache.links end
  local out = {}
  for attrs, inner in raw:gmatch("<[Aa]%s+([^>]*)>(.-)</[Aa]>") do
    local message, data = attrs:match('[Ww]nd[Nn]otify="(%d+),([^"]*)"')
    local href = attrs:match('[Hh][Rr][Ee][Ff]%s*=%s*"([^"]*)"')
    local text = P.plain(inner):gsub("^%s+", ""):gsub("%s+$", "")
    if message then
      out[#out + 1] = { text, tonumber(message), data }
    elseif href and (href:find("^file:///") or not href:find(":")) then
      out[#out + 1] = { text ~= "" and text or href, -1, href }
    end
  end
  cache.raw, cache.links = raw, out
  return out
end

ui.window("Help", function()
  M.testShow()
  if not W.shown(HW) then
    page.raw = nil
    return
  end
  T.frame()
  if W.begin("Help", { w = 1720, h = 980, title = "Help",
        hints = { { "a", "Follow link" }, { "lb", "Back" }, { "x", "Contents" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local raw = M.text(HW, "Display")
    if page.raw ~= raw then -- a new page: from its top, the links from the first
      page.raw, page.scroll = raw, 0
      links.offset, links.focusNext = 0, 1
    end
    if pressed("Help", "lb") and (M.state(HW, "BackButton")) then API.windows.ClickChild(HW, "BackButton", "left") end
    if pressed("Help", "x") then API.windows.ClickChild(HW, "Contents", "left") end
    local bh = T.px(70)
    -- the buttons along the top
    ui.SetCursorPos(x0 - wx, y0 - wy)
    M.buttons(HW, { { "BackButton", "Back", { glyph = "lb" } }, { "Contents", "Contents", { glyph = "x" } },
      { "TipsButton", "Reset Tips" }, { "CustomerServiceButton", "Customer Service" } }, x0, x1, bh)
    local top = y0 + bh + T.px(16)
    local midX = x0 + (x1 - x0) * 0.62
    -- the page (focusable: D-pad up / down scroll it)
    P.draw("##helppage", page, raw, x0, top, midX - T.px(20), y1, { empty = "This page is empty." })
    -- its links
    local list = pageLinks(raw)
    ui.SetCursorPos(midX - wx, top - wy)
    T.heading("Links on this page")
    local ly = select(2, ui.GetCursorScreenPos())
    ui.SetCursorPos(midX - wx, ly - wy)
    if #list == 0 then
      T.draw(midX, ly + T.px(6), "None.", C.faint)
    else
      local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / T.px(56)))
      links.shown = visible
      ui.BeginGroup() -- (the rows start at this x)
      W.list("helplinks", links, #list, visible, 46, x1 - midX, function(i, rx0, ry0, rx1, ry1)
        T.font(20)
        T.textIn(rx0 + T.px(16), ry0, rx1 - T.px(16), ry1, T.fit(list[i][1], rx1 - rx0 - T.px(32)), C.link or C.accent, 0)
        T.unfont()
      end, function(i) API.windows.StmlLink(HW, "Display", list[i][2], list[i][3]) end)
      ui.EndGroup()
    end
  end
  if W.finish() then API.windows.Show(HW, false) end
end)

-- ---------------------------------------------------------------- the storyline (StoryWnd)
local SW = "StoryWnd"
local stories, story = {}, { scroll = 0 }

ui.window("Story", function()
  if not W.shown(SW) then return end
  T.frame()
  if W.begin("Story", { w = 1720, h = 960, title = "Storyline", hints = { { "a", "Read" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local midX = x0 + (x1 - x0) * 0.40
    local bh = T.px(76)
    M.list(SW, "StoryList", stories, x0, y0, midX - T.px(20), y1 - bh - T.px(14),
      { only = { 2, 1 }, labels = { "Date", "Story" }, empty = "No stories found." })
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    M.toggle(SW, "AutomaticButton", "Show new stories when they come", midX - T.px(20) - x0, bh)
    local raw = M.text(SW, "Display")
    if story.raw ~= raw then story.raw, story.scroll = raw, 0 end
    P.draw("##storypage", story, raw, midX, y0, x1, y1, { empty = "Pick a story on the left to read it." })
  end
  if W.finish() then API.windows.Show(SW, false) end
end)

-- ---------------------------------------------------------------- the alert history (AlertHistoryWnd)
local AH = "AlertHistoryWnd"
local alerts = {}

ui.window("Alert History", function()
  if not W.shown(AH) then return end
  T.frame()
  if W.begin("Alert History", { w = 1300, h = 820, title = "Alert History",
        hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(72)
    M.list(AH, "AH_Closed_Alert_List", alerts, x0, y0, x1, y1 - bh - T.px(14),
      { empty = "No alerts: the alerts you close are kept here to open again." })
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    M.buttons(AH, { { "AH_Open_Alert_Btn", "Open Alert", { primary = true } } }, x0, x1, bh)
  end
  if W.finish() then API.windows.Show(AH, false) end
end)
