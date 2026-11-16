-- Notes (gamepad pack): stands in for the game's PlayerNotesWindow (/note with no text opens it; /note <text> adds a
-- dated line). The notes fill the window as one text, newest at the bottom: the right stick (or the wheel) scrolls
-- it, LB / RB a page. Edit (A) types into it with the on-screen keys (they add at the end; X deletes from the end; a
-- real keyboard edits anywhere in the box over them), Add a dated note (X) types one line and gives it to /note, as
-- typing "/note ..." does. The window keeps the text (its edit box PlayerNotesInput, 10000 characters) and writes
-- notes.txt when it closes (B or Done).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local G = dofile("lib/stock.lua")
local CH = dofile("lib/child.lua")
local C = T.C
local WIN, EDIT = "PlayerNotesWindow", "PlayerNotesInput"
local MAX = 10000

local kb = G.KF.new()
local st = CH.new()
local editing, refocus = false, nil
local view = { scroll = nil }
local wasShown = false

-- The text's lines word-wrapped to `width` (cached by text and width).
local cache = {}
local function wrap(text, width)
  if cache.text == text and cache.width == width then return cache.lines end
  local out = {}
  for para in (text:gsub("\r", "") .. "\n"):gmatch("([^\n]*)\n") do
    local line = ""
    for word in para:gmatch("%S+") do
      local try = line == "" and word or (line .. " " .. word)
      if ui.CalcTextSize(try) > width and line ~= "" then
        out[#out + 1] = line
        line = word
      else
        line = try
      end
    end
    out[#out + 1] = line
  end
  if out[#out] == "" then out[#out] = nil end -- (the newline every note ends with)
  cache.text, cache.width, cache.lines = text, width, out
  return out
end

-- The notes in x0, y0 .. x1, y1: scrolled with the right stick, the wheel and LB / RB; at the end when first shown.
local function notes(text, x0, y0, x1, y1)
  T.rect(x0, y0, x1, y1, C.sunken, 1, T.px(14))
  T.font(21)
  local p = T.px(16)
  local _, lh = ui.CalcTextSize("Ag")
  local lines = wrap(text, x1 - x0 - 2 * p - T.px(10))
  local fit = math.max(1, math.floor((y1 - y0 - 2 * p) / lh))
  local maxScroll = math.max(0, #lines - fit)
  if view.scroll == nil then view.scroll = maxScroll end
  if W.focused() and pad.mode() == "menu" then
    local ry = pad.axis("ry") or 0
    if math.abs(ry) > 0.2 then view.scroll = view.scroll + ry * 30 / math.max(20, ui.GetIO().fps or 60) end
    if W.pressed("rb") then view.scroll = view.scroll + fit - 1 end
    if W.pressed("lb") then view.scroll = view.scroll - fit + 1 end
  end
  if ui.IsMouseHoveringRect(x0, y0, x1, y1) then
    local wheel = ui.GetIO().wheel or 0
    if wheel ~= 0 then view.scroll = view.scroll - wheel * 3 end
  end
  view.scroll = math.max(0, math.min(maxScroll, view.scroll))
  if #lines == 0 then
    T.draw(x0 + p, y0 + p, "No notes yet. Edit to write some, or add a dated note.", C.faint)
  else
    ui.PushClipRect(x0, y0 + T.px(4), x1, y1 - T.px(4), true)
    local first = math.floor(view.scroll)
    for i = first + 1, math.min(#lines, first + fit) do
      T.draw(x0 + p, y0 + p + (i - first - 1) * lh, lines[i], C.text)
    end
    ui.PopClipRect()
  end
  if maxScroll > 0 then
    local sx, h = x1 - T.px(12), y1 - y0 - 2 * p
    T.rect(sx, y0 + p, sx + T.px(5), y1 - p, C.raised, 1, T.px(3))
    local th = math.max(T.px(30), h * fit / #lines)
    local ty = y0 + p + (h - th) * (view.scroll / maxScroll)
    T.rect(sx, ty, sx + T.px(5), ty + th, C.dim, 0.8, T.px(3))
  end
  T.unfont()
end

ui.window("Notes Window", function()
  G.testShow()
  if not W.shown(WIN) then
    editing, wasShown, view.scroll, st.kb.open = false, false, nil, false
    return
  end
  T.frame()
  local appearing = not wasShown
  wasShown = true
  local text = G.text(WIN, EDIT)
  local hints
  if editing then hints = G.editHints("Done")
  else hints = CH.hints(st, { { "a", "Select" }, { "x", "Add a dated note" }, { "lb", "Page up" }, { "rb", "Page down" },
    { "b", "Close" } }) end
  if W.begin("Notes", { w = 1500, h = 980, title = "Notes", subtitle = string.format("%d of %d characters", #text, MAX),
        hints = hints }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    ui.SetCursorPos(x0 - wx, y0 - wy)
    if editing then
      T.heading("Your notes (the keys add at the end)", w)
      if G.edit(kb, WIN, EDIT, x0, x1, { max = MAX, multiline = true, height = 330, keyHeight = 56 }) == "done" then
        editing, refocus, view.scroll = false, "edit", nil
      end
    elseif not CH.modal(st, x0, y0, x1, y1) then
      local rx0 = x1 - T.px(380)
      notes(text, x0, y0, rx0 - T.px(24), y1)
      local bh = T.px(80)
      local function at(k) ui.SetCursorPos(rx0 - wx, y0 + (k - 1) * (bh + T.px(12)) - wy) end
      at(1)
      if appearing or refocus == "edit" then
        ui.SetKeyboardFocusHere()
        pad.showCursor()
        refocus = nil
      end
      if T.button("##notesedit", "Edit", x1 - rx0, bh, { primary = true, sub = "Type into the notes", align = 0 }, pad) then
        editing = true
        G.openKeys(kb, false)
      end
      at(2)
      CH.refocus(st, "add")
      local function add()
        CH.type(st, "A dated note (as /note <text>)", "", function(t)
          if t:match("%S") then CH.say("/note " .. t) end
          view.scroll = nil
        end, { max = 240, from = "add" })
      end
      if T.button("##notesadd", "Add a dated note", x1 - rx0, bh, { glyph = "x", sub = "One line, with the time", align = 0 }, pad) then add() end
      if W.pressed("x") then add() end
      at(3)
      if T.button("##notesend", "Go to the end", x1 - rx0, bh, { sub = "The newest notes", align = 0 }, pad) then view.scroll = 1e9 end
      at(4)
      if T.button("##notesdone", "Done", x1 - rx0, bh, { glyph = "b", sub = "Saves notes.txt", align = 0 }, pad) then
        API.windows.Show(WIN, false)
      end
    end
  end
  local close = W.finish()
  if close and editing then
    editing, refocus = false, "edit"
  elseif CH.back(st, close) then
    API.windows.Show(WIN, false)
  end
end)
