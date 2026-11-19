-- Looking for Guild (gamepad pack): stands in for LFGuildWnd (/lfguild, the EQ menu's Guild Search). Its four pages,
-- LB / RB to switch:
--   Find a Guild     your entry (your level and class, time zone, a comment), whether it is posted and until when,
--                    and the window's Post / Update / Remove.
--   Recruit          your guild's entry: the classes it wants (sixteen toggles, Select All / None), the level range,
--                    a minimum AA count, time zone and comment, and Post / Update / Remove (the game enables them in
--                    a guild).
--   Search Guilds    your level, AA, class and time zone, Search, the matches (A: the stock list's click) and the
--   Search Players   selected match's comment; the players' search has the classes, levels, AA and time zone.
-- Every field is the window's own edit box or drop-down (the keyboard types the numbers and comments; a drop-down's
-- choices come up as a list) and every button is its button, so what is posted and searched is the game's.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local TB = dofile("lib/textbox.lua")
local C = T.C
local WIN = "LFGuildWnd"

local CLASSES = { "WAR", "CLR", "PAL", "RNG", "SHD", "DRU", "MNK", "BRD", "ROG", "SHM", "NEC", "WIZ", "MAG", "ENC",
  "BST", "BER" }
-- A class's name as the client shows it (a server's own where its plugin named the class), else `fallback`.
local HAS_CLASS_NAMES = API.Supports and API.Supports("character", "ClassName") == true
local function className(id, fallback, short)
  return HAS_CLASS_NAMES and API.character.ClassName(id, short) or fallback
end
local PAGES = { "Find a Guild", "Recruit", "Search Guilds", "Search Players" }

local st = CH.new()
local page = 1
local lists = { {}, {} }
local comments = { {}, {} }
local focusFirst = true

-- A row of controls sharing the width: each f(w) draws one at the cursor.
local function row(x0, y, w, h, fs, wx, wy)
  local gap = T.px(10)
  local bw = (w - (#fs - 1) * gap) / #fs
  for i, f in ipairs(fs) do
    ui.SetCursorPos(x0 + (i - 1) * (bw + gap) - wx, y - wy)
    if i == 1 and focusFirst then
      ui.SetKeyboardFocusHere()
      focusFirst = false
    end
    f(bw, h)
  end
  return y + h + T.px(12)
end

-- Posted or not (the window's On button is checked while the entry is up), and until when.
local function status(x, y, onId, expireId)
  local on = CH.checked(WIN, onId)
  T.font(20)
  local tw = T.text(x, y, on and "Posted" or "Not posted", on and C.good or C.faint)
  local when = CH.text(WIN, expireId)
  if when ~= "" then T.draw(x + tw + T.px(16), y, when, C.dim) end
  T.unfont()
  return y + T.px(34)
end

local function postButtons(prefix, what)
  return {
    function(w, h) CH.button(WIN, prefix .. "OnButton", "Post", w, h, { primary = true, sub = "Post " .. what, align = 0, keep = true }) end,
    function(w, h) CH.button(WIN, prefix .. "UpdateButton", "Update", w, h, { sub = "Send the changes", align = 0, keep = true }) end,
    function(w, h) CH.button(WIN, prefix .. "OffButton", "Remove", w, h, { sub = "Take it down", align = 0, keep = true }) end,
  }
end

-- The sixteen class toggles (prefix GLFP_ or GSFP_), eight a row, then Select All / None.
local function classes(prefix, x0, y, w, wx, wy)
  local fs = {}
  for i, c in ipairs(CLASSES) do
    fs[#fs + 1] = function(bw, bh)
      local id = prefix .. c .. "Checkbox"
      CH.toggle(WIN, id, className(i, c, true), bw, bh, { on = "wanted", off = "-", keep = true })
    end
    if i == 8 then
      y = row(x0, y, w, T.px(64), fs, wx, wy)
      fs = {}
    end
  end
  y = row(x0, y, w, T.px(64), fs, wx, wy)
  return row(x0, y, w, T.px(60), {
    function(bw, bh) CH.button(WIN, prefix .. "SelectAllRolesButton", "Select All", bw, bh, { keep = true }) end,
    function(bw, bh) CH.button(WIN, prefix .. "DeselectAllRolesButton", "Select None", bw, bh, { keep = true }) end,
  }, wx, wy)
end

local function number(id, caption)
  return function(w, h) CH.field(st, WIN, id, caption, w, h, { max = 8, keep = true }) end
end
local function combo(id, caption)
  return function(w, h) CH.combo(st, WIN, id, caption, w, h, { keep = true }) end
end

-- The matches and the selected one's comment under them.
local function results(k, listId, commentId, x0, y, x1, y1, wx, wy, empty)
  local commentH = math.max(T.px(90), (y1 - y) * 0.3)
  ui.SetCursorPos(x0 - wx, y - wy)
  CH.list(WIN, listId, lists[k], x1 - x0, y1 - y - commentH - T.px(14), { rowH = 52, font = 19, empty = empty,
    columns = { { 1, 0.3 }, { 2, 0.2 }, { 3, 0.2 }, { 4, 0.15 }, { 5, 0.15 } } })
  local cy = y1 - commentH
  ui.DrawLine(x0, cy - T.px(6), x1, cy - T.px(6), C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
  local text = API.windows.Text(WIN, commentId) or ""
  if text == "" then
    T.font(19)
    T.draw(x0, cy + T.px(4), "Select a match to read its comment.", C.faint)
    T.unfont()
  else
    TB.draw(text, x0, cy + T.px(4), x1, y1, comments[k])
  end
end

local DRAW = {
  function(x0, y0, x1, y1, cw, wx, wy) -- find a guild
    T.font(24)
    T.draw(x0, y0, CH.plain(WIN, "PLFG_LevelClassLabel"), C.text)
    T.unfont()
    local y = status(x0, y0 + T.px(38), "PLFG_OnButton", "PLFG_ExpireLabel")
    y = row(x0, y + T.px(8), cw, T.px(84), { combo("PLFG_TimeZoneCombo", "Time zone") }, wx, wy)
    y = row(x0, y, cw, T.px(120), { function(w, h) CH.field(st, WIN, "PLFG_CommentsEdit", "About you (guilds see this)", w, h, { max = 256, keep = true }) end }, wx, wy)
    row(x0, y1 - T.px(84), cw, T.px(84), postButtons("PLFG_", "your entry"), wx, wy)
  end,
  function(x0, y0, x1, y1, cw, wx, wy) -- recruit
    local y = status(x0, y0, "GLFP_OnButton", "GLFP_ExpireLabel")
    if not CH.enabled(WIN, "GLFP_UpdateButton") then
      T.font(19)
      T.draw(x0 + T.px(260), y0, "Only a guild can post this.", C.gold)
      T.unfont()
    end
    y = classes("GLFP_", x0, y, cw, wx, wy)
    y = row(x0, y, cw, T.px(84), { number("GLFP_LevelRangeMinEditbox", "Levels from"), number("GLFP_LevelRangeMaxEditbox", "to"),
      number("GLFP_MinAACountEdit", "Minimum AA"), combo("GLFP_TimeZoneCombo", "Time zone") }, wx, wy)
    row(x0, y, cw, T.px(84), { function(w, h) CH.field(st, WIN, "GLFP_CommentsEdit", "About the guild (players see this)", w, h, { max = 256, keep = true }) end }, wx, wy)
    row(x0, y1 - T.px(84), cw, T.px(84), postButtons("GLFP_", "the guild's entry"), wx, wy)
  end,
  function(x0, y0, x1, y1, cw, wx, wy) -- search guilds
    local y = row(x0, y0, cw, T.px(84), { number("PSFG_LevelEdit", "Your level"), number("PSFG_AACountEdit", "Your AA"),
      combo("PSFG_ClassCombo", "Class"), combo("PSFG_TimeZoneCombo", "Time zone"),
      function(w, h) CH.button(WIN, "PSFG_SearchButton", "Search", w, h, { primary = true, keep = true }) end }, wx, wy)
    results(1, "PSFG_SearchResultList", "PSFG_Comment", x0, y, x1, y1, wx, wy, "No guilds found yet: set the fields and Search.")
  end,
  function(x0, y0, x1, y1, cw, wx, wy) -- search players
    local y = classes("GSFP_", x0, y0, cw, wx, wy)
    y = row(x0, y, cw, T.px(84), { number("GSFP_LevelRangeMinEditbox", "Levels from"), number("GSFP_LevelRangeMaxEditbox", "to"),
      number("GSFP_MinAACountEdit", "Minimum AA"), combo("GSFP_TimeZoneCombo", "Time zone"),
      function(w, h) CH.button(WIN, "GSFP_SearchButton", "Search", w, h, { primary = true, keep = true }) end }, wx, wy)
    results(2, "GSFP_SearchResultList", "GSFP_Comment", x0, y, x1, y1, wx, wy, "No players found yet: set the fields and Search.")
  end,
}

local wasShown = false
ui.window("Looking for Guild", function()
  CH.testOpen(WIN)
  if not W.shown(WIN) then
    st.kb.open, st.pick, wasShown = false, nil, false
    return
  end
  T.frame()
  if not wasShown then focusFirst = true end
  wasShown = true
  local tp = tonumber(ui.GetSetting("test.lfguild.page") or "") -- testing: 1..4
  if tp and not GPSTATE.test_lfguild_page then GPSTATE.test_lfguild_page, page = true, tp end
  if W.begin("Looking for Guild", { w = 1720, h = 1000, title = "Looking for Guild", subtitle = PAGES[page],
        hints = CH.hints(st, { { "lb", "Previous" }, { "rb", "Next" }, { "a", "Select" }, { "b", "Close" } }) }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if not CH.modal(st, x0, y0, x1, y1) then
      if W.pressed("lb") then page, focusFirst = (page + 2) % 4 + 1, true end
      if W.pressed("rb") then page, focusFirst = page % 4 + 1, true end
      -- the pages as tabs (the mouse; LB / RB with the pad)
      local tw = (cw - 3 * T.px(10)) / 4
      for i, name in ipairs(PAGES) do
        ui.SetCursorPos(x0 + (i - 1) * (tw + T.px(10)) - wx, y0 - wy)
        if T.button("##lfgpage" .. i, name, tw, T.px(52), { primary = i == page }, pad) then page = i end
      end
      DRAW[page](x0, y0 + T.px(68), x1, y1, cw, wx, wy)
    end
  end
  if CH.back(st, W.finish()) then API.windows.Show(WIN, false) end
end)
