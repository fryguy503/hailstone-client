-- Server select: the client's world list in this UI's look (hs.ServerSelect, app/lua_ui_pregame.cpp; the client's
-- screen, as ui/dev_screens.cpp server_select_screen draws it, not the original's page). A card in the middle of the
-- screen: the world played last here as one button, then every world the login server listed as a row with its
-- status (green when it can be joined). No player count is shown. A click picks a world, a double click or Play plays it,
-- Enter plays the picked one, Back returns to the login screen. The star on a row marks the world a favourite: the
-- favourites are listed first, then the other worlds, each by name (the client sorts them).
local API = hs.UI
local P = dofile("lib/pregame.lua")
local T = P.T
local C = T.color

-- A five-pointed star around (cx, cy): filled for a favourite, else its outline.
local pts = {} -- (its points, the tables kept from star to star)
for k = 1, 10 do pts[k] = { 0, 0 } end
local function star(cx, cy, r, filled, c, a)
  for k = 0, 9 do
    local ang = -math.pi / 2 + k * math.pi / 5
    local rr = k % 2 == 0 and r or r * 0.42
    local p = pts[k + 1]
    p[1], p[2] = cx + rr * math.cos(ang), cy + rr * math.sin(ang)
  end
  for k = 1, 10 do
    local p, q = pts[k], pts[k % 10 + 1]
    if filled then
      ui.DrawTriangleFilled(cx, cy, p[1], p[2], q[1], q[2], c[1], c[2], c[3], a)
    else
      ui.DrawLine(p[1], p[2], q[1], q[2], c[1], c[2], c[3], a, 1.2)
    end
  end
end

local PLAY = { "Play" }

local function card(view, s, bh)
  P.Title("Choose a world", view.server ~= "" and view.server or nil, s)
  local last = view.servers[view.last]
  ui.BeginDisabled(view.busy)
  if last then
    if P.Primary("Play " .. last.name .. "  (last played)", -1, bh, last.canJoin) then
      API.pregame.ServerSelectDo("play", view.last)
    end
    P.Tip(last.canJoin and "The world you played last on this server" or (last.name .. ": " .. last.status))
    ui.Dummy(0, 4 * s)
  end
  P.Heading("Worlds")
  local _, lh = ui.CalcTextSize("Ag")
  local rowH = lh + 16 * s
  local shown = math.max(1, math.min(#view.servers, 8))
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, 8 * s, 3 * s)
  ui.BeginChild("##worlds", 0, shown * (rowH + 3 * s), false)
  if #view.servers == 0 then P.Faint("No worlds listed.") end
  for i, sv in ipairs(view.servers) do
    ui.PushID(i)
    local clicked, double, x0, y0, x1, y1 = P.Row("##world", view.selected == i, rowH, s)
    -- the star at the left takes its own clicks: it marks or unmarks the world as a favourite
    local sr = 7 * s
    local sx0, sx1 = x0 + 10 * s, x0 + 10 * s + 2 * sr + 8 * s
    local onStar = ui.IsMouseHoveringRect(sx0, y0, sx1, y1)
    if clicked and onStar then
      API.pregame.ServerSelectDo("favorite", i)
    elseif clicked then
      API.pregame.ServerSelectDo("select", i)
      if double and sv.canJoin then API.pregame.ServerSelectDo("play", i) end
    end
    local hot = onStar and ui.IsItemHovered()
    star((sx0 + sx1) / 2, (y0 + y1) / 2, sr, sv.favorite, (sv.favorite or hot) and C.gold or C.faint,
      (sv.favorite or hot) and 1 or 0.8)
    if hot then P.Tip(sv.favorite and "A favourite: listed first. Click to unmark it" or "Mark as a favourite: listed first") end
    local _, nh = T.TextSize("Ag")
    T.Text(sx1 + 4 * s, y0 + (y1 - y0 - nh) / 2, sv.name, sv.canJoin and C.text or C.muted)
    -- at the right: the status pill
    ui.PushFont(T.font.tiny)
    local pw, ph = ui.CalcTextSize(sv.status)
    ui.PopFont()
    pw, ph = pw + 12 * s, ph + 4 * s
    local px = x1 - 10 * s - pw
    P.Pill(px, y0 + (y1 - y0 - ph) / 2, sv.status,
      sv.status == "Locked" and C.gold or ((sv.canJoin and sv.status ~= "Down") and C.pet or C.danger), s)
    ui.PopID()
  end
  ui.EndChild()
  ui.PopStyleVar()
  ui.EndDisabled()

  ui.Dummy(0, 2 * s)
  P.Status(view.status, view.busy, s)
  -- a world listed has an add-on the player does not have (docs/addons.md "Suggested add-ons")
  if view.addonHint and view.addonHint ~= "" then
    ui.PushStyleColor(ui.Col.Text, C.accent[1], C.accent[2], C.accent[3], C.accent[4] or 1)
    ui.TextWrapped(view.addonHint)
    ui.PopStyleColor()
  end
  local picked = view.servers[view.selected]
  local canPlay = not view.busy and picked ~= nil and picked.canJoin
  if ui.Button("Back", 0, bh) then API.pregame.ServerSelectDo("back") end
  P.Tip("Back to the login screen")
  ui.SameLine()
  local playW = P.ButtonsWidth(PLAY, s) + 40 * s
  P.AlignRight(playW)
  if P.Primary("Play", playW, bh, canPlay) then API.pregame.ServerSelectDo("play") end
  -- Enter plays the picked world (the client binds no key on this screen; not while a field elsewhere is typed in)
  if canPlay and ui.IsKeyPressed(ui.Key.Enter) and not ui.GetIO().typing then API.pregame.ServerSelectDo("play") end
end

local held = nil -- the view drawn last frame: hs.ServerSelect gives true while it still holds (its revision)

ui.window("Server Select", function()
  local view = API.pregame.ServerSelect(held and held.revision)
  if view == true then view = held else held = view end
  if not view then return end
  local s = P.Scale()
  local io = ui.GetIO()
  local _, lh = ui.CalcTextSize("Ag")
  local bh = lh + 14 * s
  local w = 460 * s
  local _, h = P.Size("serverselect", w, 420 * s)
  local failure
  if P.Begin("serverselect", (io.width - w) / 2, math.max(16 * s, (io.height - h) / 2), w, s) then
    local ok, err = pcall(card, view, s, bh)
    if not ok then failure = err end
  end
  P.End()
  if failure then error(failure, 0) end
end)
