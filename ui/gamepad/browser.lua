-- Web Browser (gamepad pack): Hailstone's in-game browser (docs/browser.md), the BrowserWnd screen; /web and a
-- server's web links (OP_Weblink) open it, as in the Modern pack. The buttons down the right: back, forward, reload,
-- an address (typed with the on-screen keys), text typed into the page where it has its cursor, the pad's pointer
-- for the page (cursor mode: the left stick moves it, A clicks, B leaves), closing the tab. LB / RB go through the
-- tabs. The mouse and keyboard work on the page as they do in the Modern pack.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local G = dofile("lib/stock.lua")
local CH = dofile("lib/child.lua")
local C = T.C
local WIN = "BrowserWnd"

local st = CH.new()
local current, known = nil, {}
local problem = nil

local HINTS = { { "a", "Select" }, { "lb", "Previous tab" }, { "rb", "Next tab" }, { "b", "Close" } }
local spare = {} -- (known's partner: see below)
local function tabIndex(tabs, id)
  for i, t in ipairs(tabs) do if t == id then return i end end
  return 0
end

local function title(s)
  local t = s.title ~= "" and s.title or s.url
  if #t > 60 then t = T.cutBytes(t, 59) .. "…" end
  return t
end

ui.window("Web Browser", function()
  G.testShow()
  if not W.shown(WIN) then
    st.kb.open = false
    return
  end
  T.frame()
  local tabs = API.web.Tabs()
  -- (the tabs seen last frame and this one: two tables used in turn, not a new one a frame)
  local seen = spare
  for id in pairs(seen) do seen[id] = nil end
  for _, id in ipairs(tabs) do
    seen[id] = true
    if not known[id] then current = id end -- a new tab (a link, /web) comes to the front
  end
  known, spare = seen, known
  if current and not seen[current] then current = tabs[#tabs] end
  local s = current and API.web.State(current)
  local subtitle = not API.web.Available() and "Not installed"
    or s and string.format("%s (%d of %d)", title(s), tabIndex(tabs, current), #tabs)
    or "No page open"
  local hints = CH.hints(st, HINTS)
  if W.begin("Web Browser", { w = 1700, h = 1000, title = "Web Browser", subtitle = subtitle, hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not API.web.Available() then
      ui.SetCursorPos(x0 - wx, y0 - wy)
      T.heading("The in-game browser is not installed: web pages open in your own browser.", x1 - x0)
    elseif not CH.modal(st, x0, y0, x1, y1) then
      local rx0 = x1 - T.px(360)
      local bw, bh = x1 - rx0, T.px(70)
      local function at(k) ui.SetCursorPos(rx0 - wx, y0 + (k - 1) * (bh + T.px(10)) - wy) end
      local function button(k, id, label, opts)
        at(k)
        return T.button("##web" .. id, label, bw, bh, opts or {}, pad)
      end
      local open = s and not s.closed
      if button(1, "addr", "Address", { primary = true, sub = s and s.url ~= "about:blank" and s.url or "Type one", align = 0 }) then
        CH.type(st, "An address", s and s.url ~= "about:blank" and s.url or "", function(t)
          if not t:match("%S") then return end
          local ok, why
          if open then ok, why = API.web.Navigate(current, t)
          else
            local id
            id, why = API.web.Open(t)
            ok = id ~= nil
            if ok then current = id end -- (a failed open keeps the tab that was in front)
          end
          problem = not ok and (why or "That address cannot be opened.") or nil
        end, { max = 1024, from = "addr" })
      end
      if open then
        if button(2, "back", "Back", { align = 0, disabled = not s.canBack }) and s.canBack then API.web.Nav(current, "back") end
        if button(3, "fwd", "Forward", { align = 0, disabled = not s.canForward }) and s.canForward then
          API.web.Nav(current, "forward")
        end
        if button(4, "reload", s.loading and "Stop" or "Reload", { align = 0 }) then
          API.web.Nav(current, s.loading and "stop" or "reload")
        end
        if button(5, "type", "Type into the page", { align = 0, sub = "Where the page's cursor is" }) then
          CH.type(st, "Text for the page", "", function(t) if t ~= "" then API.web.Text(current, t) end end,
            { max = 1024, from = "type" })
        end
        if button(6, "point", "Point at the page", { align = 0, sub = "Stick moves, A clicks, B ends" }) then
          pad.set_mode("cursor")
        end
        if button(7, "close", "Close the tab", { align = 0 }) then API.web.Close(current) end
      elseif button(2, "new", "New tab", { align = 0 }) then
        local id, why = API.web.Open("about:blank")
        if id then current = id end
        problem = not id and (why or "The browser could not open a page.") or nil
      end
      if W.pressed("rb") or W.pressed("lb") then
        local step = W.pressed("rb") and 1 or -1
        for i, id in ipairs(tabs) do
          if id == current then
            current = tabs[(i - 1 + step) % #tabs + 1]
            break
          end
        end
      end
      -- The page.
      local px1, py0 = rx0 - T.px(20), y0
      if problem then
        T.draw(x0, y0, problem, C.bad or C.text)
        py0 = y0 + T.px(36)
      end
      ui.SetCursorPos(x0 - wx, py0 - wy)
      if open then
        API.web.View(current, math.floor(px1 - x0), math.floor(y1 - py0))
      elseif s and s.closed then
        T.draw(x0, py0, "This page was closed: " .. (s.why ~= "" and s.why or "the browser stopped"), C.faint)
      else
        T.draw(x0, py0, "No page open: Address opens one.", C.faint)
      end
    end
  end
  if CH.back(st, W.finish()) then API.windows.Show(WIN, false) end
end)
