-- Character creation: the client's own creation screen in this UI's look (hs.CharacterCreate, app/lua_ui_pregame.cpp;
-- the client's screen, as ui/dev_screens.cpp character_create_screen draws it, not CCharacterCreation's page). With
-- the 3D scene up the new character's model stands in the middle of the screen, so two cards flank it: at the left
-- who the character is (name, with a random one from the server, gender, race and class as tiles), at the right
-- where they start (deity and start city), their stats with the points to spend, the game's description of what was
-- picked or is pointed at (dbstr_us.txt, hs.CharacterCreateHelp), Appearance (the customization window on the model),
-- Cancel and Create. Without the scene the two cards stand side by side in the middle.
--
-- The rules are the client's (a race, class or deity resets the stats; a stat goes from its base up to base + 25, at
-- most 150; Create once every point is spent and the name has 4 letters or more). Escape is the client's key bind
-- here (CreationEscape), so the cards leave it alone.
local API = hs.UI
local P = dofile("lib/pregame.lua")
local T = P.T
local C = T.color

local function rgba(c) return c[1], c[2], c[3], c[4] or 1 end

-- What the description box shows: the last thing picked, or what the pointer is over this frame.
local about = { kind = "race", id = 0 }
local hover

local function point(kind, id)
  if ui.IsItemHovered() then hover = { kind = kind, id = id } end
end

local function pick(kind, id)
  about = { kind = kind, id = id }
end

-- A tile that can be picked (a race, a class, a gender): the accent when it is the one chosen.
local function tile(label, chosen, w, h, s)
  local A = C.accent
  if chosen then
    ui.PushStyleColor(ui.Col.Button, A[1], A[2], A[3], 0.85)
    ui.PushStyleColor(ui.Col.ButtonHovered, A[1], A[2], A[3], 1)
    ui.PushStyleColor(ui.Col.ButtonActive, A[1] * 0.8, A[2] * 0.8, A[3] * 0.8, 1)
    ui.PushStyleColor(ui.Col.Text, rgba(C.onAccent))
  end
  local clicked = ui.Button(label, w, h)
  if chosen then ui.PopStyleColor(4) end
  return clicked
end

-- The choices as tiles, `cols` to a row; onPick(choice) for the one clicked.
local function grid(id, choices, current, kind, cols, s, bh, onPick)
  local gap = 6 * s
  local w = (ui.GetContentRegionAvail() - gap * (cols - 1)) / cols
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, gap, gap)
  for i, c in ipairs(choices) do
    ui.PushID(id .. i)
    if (i - 1) % cols ~= 0 then ui.SameLine() end
    if tile(T.Fit(c.name, w - 8 * s), c.id == current, w, bh, s) and c.id ~= current then onPick(c) end
    point(kind, c.id)
    ui.PopID()
  end
  ui.PopStyleVar()
end

-- A row's label `w` wide, level with the frame drawn after it on the same line (the card's frame padding is 5).
local function rowLabel(text, color, w, s)
  local x, y = ui.GetCursorScreenPos()
  local _, lh = ui.CalcTextSize("Ag")
  T.Text(x, y + 5 * s, text, color)
  ui.Dummy(w, lh + 10 * s)
  ui.SameLine()
end

local function combo(id, label, choices, index, kind, onPick)
  local cur = choices[index]
  rowLabel(label, C.muted, 96 * P.Scale(), P.Scale())
  ui.SetNextItemWidth(-1)
  if ui.BeginCombo(id, cur and cur.name or "-") then
    for i, c in ipairs(choices) do
      if ui.Selectable(c.name, i == index) and i ~= index then onPick(c) end
      point(kind, c.id)
    end
    ui.EndCombo()
  end
  if cur then point(kind, cur.id) end
end

-- ---------------------------------------------------------------- the left card: who
local function whoCard(view, s, bh)
  P.Title("New character", "Who they are", s)
  P.Heading("Name")
  local randomW = P.ButtonsWidth({ "Random" }, s) + 12 * s
  ui.SetNextItemWidth(ui.GetContentRegionAvail() - randomW - 8 * s)
  local text, enter = ui.InputTextWithHint("##name", "Character name", view.name, 64, ui.InputTextFlags.EnterReturnsTrue)
  text = text:gsub("[^%a]", "") -- (the client keeps the letters alone)
  if text ~= view.name then API.pregame.CharacterCreateDo("name", text) end
  if enter and view.ready then API.pregame.CharacterCreateDo("create") end
  ui.SameLine()
  ui.BeginDisabled(view.namePending)
  if ui.Button(view.namePending and "..." or "Random", randomW, 0) then API.pregame.CharacterCreateDo("randomName") end
  ui.EndDisabled()
  P.Tip("A name from the server for this race and gender")
  P.Faint("4 to 15 letters; the first is made a capital.")

  P.Heading("Gender")
  local half = (ui.GetContentRegionAvail() - 6 * s) / 2
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, 6 * s, 6 * s)
  if tile("Male", view.gender == 0, half, bh, s) then API.pregame.CharacterCreateDo("gender", 0) end
  ui.SameLine()
  if tile("Female", view.gender == 1, half, bh, s) then API.pregame.CharacterCreateDo("gender", 1) end
  ui.PopStyleVar()

  P.Heading("Race")
  grid("race", view.races, view.race, "race", 3, s, bh, function(c)
    if API.pregame.CharacterCreateDo("race", c.id) then pick("race", c.id) end
  end)
  P.Heading("Class")
  grid("class", view.classes, view.class, "class", 3, s, bh, function(c)
    if API.pregame.CharacterCreateDo("class", c.id) then pick("class", c.id) end
  end)
end

-- ---------------------------------------------------------------- the right card: where they start, the stats
local function statsRows(view, s, bh)
  local _, lh = ui.CalcTextSize("Ag")
  local button = lh + 10 * s
  local valueW = ui.CalcTextSize("000")
  for i, st in ipairs(view.stats) do
    ui.PushID("stat" .. i)
    -- a stat the class puts points in is green, as the stock page's
    local controls = button * 2 + valueW + 16 * s
    rowLabel(st.name, st.favored and C.pet or C.text, ui.GetContentRegionAvail() - controls - 8 * s, s)
    point("stat", i)
    ui.BeginDisabled(st.value <= st.base)
    if ui.Button("-", button, 0) then API.pregame.CharacterCreateDo("statDown", i) end
    ui.EndDisabled()
    point("stat", i)
    ui.SameLine()
    local vx, vy = ui.GetCursorScreenPos()
    ui.Dummy(valueW, button)
    local raised = st.value > st.base
    T.Text(vx + valueW, vy + (button - lh) / 2, tostring(st.value), raised and C.accent or C.text, nil, "right")
    ui.SameLine()
    local canUp = view.pointsLeft > 0 and st.value < st.cap
    ui.BeginDisabled(not canUp)
    if ui.Button("+", button, 0) then API.pregame.CharacterCreateDo("statUp", i) end
    ui.EndDisabled()
    if canUp and ui.IsItemClicked(ui.Mouse.Right) then API.pregame.CharacterCreateDo("statMax", i) end
    P.Tip(string.format("%s %d to %d. Right click: as many points as it takes", st.short, st.base, st.cap))
    point("stat", i)
    ui.PopID()
  end
end

local function aboutBox(view, s)
  local cur = hover or about
  if cur.kind == "race" and cur.id == 0 then cur = { kind = "race", id = view.race } end
  local text, title = API.pregame.CharacterCreateHelp(cur.kind, cur.id)
  text = (text or ""):gsub("<[Bb][Rr]%s*/?>", "\n"):gsub("<[^>]*>", "")
  P.Heading(title ~= "" and title or "About")
  local _, lh = ui.CalcTextSize("Ag")
  ui.BeginChild("##about", 0, lh * 6 + 8 * s, false)
  ui.PushTextWrapPos(0)
  if text == "" then P.Faint("No description in the game files.") else P.Muted(text) end
  ui.PopTextWrapPos()
  ui.EndChild()
end

local function detailsCard(view, s, bh)
  P.Title("Beginnings", view.raceName ~= "" and (view.raceName .. " " .. view.className) or nil, s)
  P.Heading("Faith and home")
  combo("##deity", "Deity", view.deities, view.deityIndex, "deity", function(c)
    if API.pregame.CharacterCreateDo("deity", c.id) then pick("deity", c.id) end
  end)
  combo("##city", "Start city", view.cities, view.cityIndex, "city", function(c)
    if API.pregame.CharacterCreateDo("city", c.id) then pick("city", c.id) end
  end)

  P.Heading("Stats")
  local x0, y0 = ui.GetCursorScreenPos()
  local left = string.format("%d point%s left", view.pointsLeft, view.pointsLeft == 1 and "" or "s")
  local resetW = P.ButtonsWidth({ "Reset" }, s)
  P.Pill(x0, y0 + 3 * s, left, view.pointsLeft > 0 and C.gold or C.pet, s)
  P.AlignRight(resetW)
  if ui.SmallButton("Reset") then API.pregame.CharacterCreateDo("resetStats") end
  P.Tip("Every stat back to its base")
  statsRows(view, s, bh)

  aboutBox(view, s)

  ui.Separator()
  P.Status(view.status, view.busy, s)
  if view.pointsLeft > 0 and not view.busy then
    P.Faint(string.format("Spend the %d points left to create.", view.pointsLeft))
  elseif #view.name < 4 and not view.busy then
    P.Faint("Give the character a name to create.")
  end
  if ui.Button("Cancel", 0, bh) then API.pregame.CharacterCreateDo("cancel") end
  P.Tip("Back to the character select")
  if view.scene then
    ui.SameLine()
    if ui.Button("Appearance", 0, bh) then API.pregame.CharacterCreateDo("appearance") end
    P.Tip("Face, hair, eyes and the rest, on the model")
  end
  ui.SameLine()
  local createW = P.ButtonsWidth({ "Create" }, s) + 40 * s
  P.AlignRight(createW)
  if P.Primary("Create", createW, bh, view.ready) then API.pregame.CharacterCreateDo("create") end
end

-- One card: P.Begin / P.End around the drawing, an error re-raised after the window is closed.
local function card(key, x, y, w, s, draw, view, bh)
  local failure
  if P.Begin(key, x, y, w, s) then
    ui.BeginDisabled(view.busy)
    local ok, err = pcall(draw, view, s, bh)
    ui.EndDisabled()
    if not ok then failure = err end
  end
  P.End()
  if failure then error(failure, 0) end
end

ui.window("Character Creation", function()
  local view = API.pregame.CharacterCreate()
  if not view then return end
  hover = nil
  local s = P.Scale()
  local io = ui.GetIO()
  local _, lh = ui.CalcTextSize("Ag")
  local bh = lh + 14 * s
  local w = 380 * s
  local gap = 16 * s
  local _, hl = P.Size("charcreate", w, 600 * s)
  local _, hr = P.Size("charcreate.details", w, 600 * s)
  local xl, xr
  if view.scene then
    xl, xr = gap, io.width - w - gap
  else
    xl = (io.width - 2 * w - gap) / 2
    xr = xl + w + gap
  end
  card("charcreate", xl, math.max(gap, (io.height - hl) / 2), w, s, whoCard, view, bh)
  card("charcreate.details", xr, math.max(gap, (io.height - hr) / 2), w, s, detailsCard, view, bh)
end)
