-- Web Browser: Hailstone's in-game browser (docs/browser.md), the BrowserWnd screen. /web [address] opens it (on the
-- address, in a new tab); a server's web link (OP_Weblink) opens a tab in it, as the original's HTML window. Tabs
-- along the top (the newest comes to the front), back / forward / reload, the address, a button for the system's
-- browser, and the page: it takes the mouse, and the keyboard once clicked (a click outside gives it back).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local WINDOW = "BrowserWnd"

local current, known = nil, {}
local address, editing = "", false
local problem = nil -- why the last address did not open
local openFailed = nil -- why the first tab did not open (not tried again each frame; "Try again" does)

local function label(st)
  local t = st.title ~= "" and st.title or st.url
  if #t > 22 then t = t:sub(1, 21) .. "…" end
  return t
end

local WINDOW_OPTS = {
  size = function() return 960, 700 end,
  resizable = { 420, 300 },
}
local function drawWindow(x, y, w, h, s)
  if not API.web.Available() then
    ui.TextWrapped("The in-game browser is not installed (browser/ beside Hailstone). Web pages open in your "
      .. "own browser instead.")
    return
  end
  local tabs = API.web.Tabs()
  -- A tab that was not there before (a server's link, /web) comes to the front.
  local seen = {}
  for _, id in ipairs(tabs) do
    seen[id] = true
    if not known[id] then current = id end
  end
  known = seen
  if current and not seen[current] then current = tabs[#tabs] end
  if #tabs > 0 then openFailed = nil end
  if not current and #tabs == 0 and not openFailed then
    local id, why = API.web.Open("about:blank", math.floor(w), math.floor(h - 60 * s))
    if id then current = id else openFailed = why or "The browser could not open a page." end
  end
  if openFailed then
    ui.TextColored(1, 0.55, 0.45, 1, openFailed)
    if ui.Button("Try again##webretry") then openFailed = nil end
    return
  end

  -- The tabs.
  for i, id in ipairs(tabs) do
    local st = API.web.State(id)
    if st then
      if i > 1 then ui.SameLine() end
      if ui.Selectable(label(st) .. "##tab" .. id, id == current, 0, 150 * s, 0) then
        current, editing = id, false
      end
      ui.SameLine()
      if ui.SmallButton("x##close" .. id) then API.web.Close(id) end
    end
  end
  ui.SameLine()
  if ui.SmallButton("+##newtab") then
    local id, why = API.web.Open("about:blank")
    if id then current, editing, problem = id, false, nil
    else problem = why or "The browser could not open a page." end -- (the tab in front stays)
  end

  local st = current and API.web.State(current)
  if not st then return end
  if st.closed then
    ui.TextWrapped("This page was closed: " .. (st.why ~= "" and st.why or "the browser stopped"))
    if ui.Button("Close the tab") then API.web.Close(current) end
    return
  end

  -- The address row.
  ui.BeginDisabled(not st.canBack)
  if ui.Button("<##back") then API.web.Nav(current, "back") end
  ui.EndDisabled()
  ui.SameLine()
  ui.BeginDisabled(not st.canForward)
  if ui.Button(">##forward") then API.web.Nav(current, "forward") end
  ui.EndDisabled()
  ui.SameLine()
  if ui.Button((st.loading and "Stop" or "Reload") .. "##reload") then
    API.web.Nav(current, st.loading and "stop" or "reload")
  end
  ui.SameLine()
  if not editing then address = st.url == "about:blank" and "" or st.url end
  ui.SetNextItemWidth(math.max(80 * s, w - 260 * s))
  local text, enter = ui.InputTextWithHint("##address", "Type an address", address, 2048,
    ui.InputTextFlags.EnterReturnsTrue + ui.InputTextFlags.AutoSelectAll)
  editing = ui.IsItemActive()
  address = text
  if enter and text ~= "" then
    local ok, why = API.web.Navigate(current, text)
    problem = not ok and (why or "That address cannot be opened.") or nil
    editing = false
  end
  ui.SameLine()
  if ui.Button("Your browser##external") then API.web.OpenExternal(st.url) end

  if problem then ui.TextColored(1, 0.55, 0.45, 1, problem) end

  -- The page.
  local aw, ah = ui.GetContentRegionAvail()
  API.web.View(current, math.floor(aw), math.floor(ah))
end

ui.window("Web Browser", function()
  hud.Dialog(WINDOW, "Web Browser", WINDOW_OPTS, drawWindow)
end)
