-- The readers (gamepad pack): windows that show a text to read, each standing in for its stock window and acting
-- through its children, so the game does what the stock buttons do. The text scrolls with the right stick (or the
-- wheel); the buttons are along the bottom.
--   Tip (TipWindow)                 the context tip (STML), the earlier tips (its History combo: A steps through
--                                   them), its two "don't show" boxes, Show Another Tip and Close (B)
--   Note (NoteWindow)               a note or scroll read from the inventory on a paper-toned page; Cast Spell and
--                                   Scribe when it teaches a spell (they show only then, as on the stock window), Done (B)
--   Book (BookWindow)               the open spread (Page0 / Page1, the game pages the text), LB / RB turn the pages
--                                   (its LeftButton / RightButton, greyed at the ends), Cast Spell / Scribe, Done (B)
--   Spell (SpellDisplayWindow ...)  the spell's icon and name over the game's description; several can be open at
--                                   once (SpellDisplayWindow#2 ...: hs.ReplaceWindow numbers them); B closes one
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local TB = dofile("lib/textbox.lua")
local G = dofile("lib/stock.lua")
local C = T.C

local PAPER = { 0.13, 0.115, 0.09, 1 }
local INK = { 0.95, 0.90, 0.80, 1 }

-- A row of the window's buttons across x0..x1 at y (those it hides left out); `list` = { { id, label, opts } }.
local function buttons(win, list, x0, x1, y, h)
  local shown = {}
  for _, b in ipairs(list) do if G.shown(win, b[1]) then shown[#shown + 1] = b end end
  local gap = T.px(12)
  local bw = (x1 - x0 - (#shown - 1) * gap) / math.max(1, #shown)
  local wx, wy = ui.GetWindowPos()
  ui.SetCursorPos(x0 - wx, y - wy)
  for i, b in ipairs(shown) do
    if i > 1 then ui.SameLine(0, gap) end
    G.button(win, b[1], b[2], bw, h, b[3])
  end
end

-- The STML child `id` in a sunken (or paper) box that scrolls.
local function textBox(win, id, x0, y0, x1, y1, state, bg, ink)
  T.rect(x0, y0, x1, y1, bg or C.sunken, 1, T.px(14))
  local stml = G.text(win, id)
  if ink then stml = "<c \"#" .. G.hex(ink[1], ink[2], ink[3]):sub(2) .. "\">" .. stml .. "</c>" end
  return TB.draw(stml, x0 + T.px(20), y0 + T.px(16), x1 - T.px(12), y1 - T.px(16), state)
end

-- ---------------------------------------------------------------- tip
local TIP = "TipWindow"
local tipText = {}
ui.window("Tip", function()
  if not W.shown(TIP) then return end
  T.frame()
  if W.begin("Tip", { w = 1200, h = 860, title = "Tip", hints = { { "a", "Select" }, { "rs", "Scroll" }, { "b", "Close" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local gap, bh = T.px(12), T.px(64)
    local top = y0
    local choices = API.windows.ComboInfo(TIP, "History")
    if G.shown(TIP, "History") and choices and #choices > 1 then
      ui.SetCursorPos(x0 - wx, top - wy)
      G.combo(TIP, "History", "Earlier tips", w, T.px(80))
      top = top + T.px(80) + gap
    end
    local by = y1 - bh
    local cy = by - gap - bh
    textBox(TIP, "Display", x0, top, x1, cy - gap, tipText)
    local hw = (w - gap) / 2
    ui.SetCursorPos(x0 - wx, cy - wy)
    G.check(TIP, "NoShowThisButton", "Don't show this tip again", hw, bh)
    ui.SameLine(0, gap)
    G.check(TIP, "NoShowButton", "Don't show any tips", hw, bh)
    buttons(TIP, { { "NextTipButton", "Show Another Tip" }, { "CloseButton", "Close", { primary = true, glyph = "b" } } },
      x0, x1, by, bh)
  end
  if W.finish() then API.windows.ClickChild(TIP, "CloseButton", "left") end
end)

-- ---------------------------------------------------------------- note
local NOTE = "NoteWindow"
local noteText = {}
ui.window("Note", function()
  if not W.shown(NOTE) then return end
  T.frame()
  if W.begin("Note", { w = 1000, h = 860, title = "Note", hints = { { "a", "Select" }, { "rs", "Scroll" }, { "b", "Done" } } }) then
    local x0, y0, x1, y1 = W.area()
    local bh = T.px(64)
    textBox(NOTE, "Note", x0, y0, x1, y1 - bh - T.px(14), noteText, PAPER, INK)
    buttons(NOTE, { { "CastSpellButton", "Cast Spell" }, { "ScribeButton", "Scribe" },
      { "DoneButton", "Done", { primary = true, glyph = "b" } } }, x0, x1, y1 - bh, bh)
  end
  if W.finish() then API.windows.ClickChild(NOTE, "DoneButton", "left") end
end)

-- ---------------------------------------------------------------- book
local BOOK = "BookWindow"
ui.window("Book", function()
  if not W.shown(BOOK) then return end
  T.frame()
  if W.begin("Book", { w = 1500, h = 880, title = "Book",
        hints = { { "lb", "Previous" }, { "rb", "Next" }, { "a", "Select" }, { "b", "Done" } } }) then
    local x0, y0, x1, y1, w = W.area()
    local gap, bh = T.px(20), T.px(64)
    local py1 = y1 - bh - T.px(14)
    local pw = (w - gap) / 2
    -- the two pages (12 short lines each, as the game pages the text) and their numbers
    for i, id in ipairs({ "Page0", "Page1" }) do
      local px0 = x0 + (i - 1) * (pw + gap)
      T.rect(px0, y0, px0 + pw, py1, PAPER, 1, T.px(14))
      T.font(26)
      local _, lh = ui.CalcTextSize("Ag")
      local y = y0 + T.px(24)
      ui.PushClipRect(px0, y0, px0 + pw, py1 - T.px(40), true)
      for line in (G.text(BOOK, id) .. "\n"):gmatch("(.-)\n") do
        T.draw(px0 + T.px(28), y, line, INK)
        y = y + lh
      end
      ui.PopClipRect()
      T.unfont()
      T.font(18)
      T.textIn(px0, py1 - T.px(40), px0 + pw, py1 - T.px(8), G.text(BOOK, id .. "Number"), C.dim, 0.5)
      T.unfont()
    end
    -- LB / RB (and the wheel over the pages) turn them
    local wheel = ui.IsMouseHoveringRect(x0, y0, x1, py1) and (ui.GetIO().wheel or 0) or 0
    local function turn(id) if G.enabled(BOOK, id) then API.windows.ClickChild(BOOK, id, "left") end end
    if W.pressed("lb") or wheel > 0 then turn("LeftButton") end
    if W.pressed("rb") or wheel < 0 then turn("RightButton") end
    local wx, wy = ui.GetWindowPos()
    local aw = T.px(140)
    ui.SetCursorPos(x0 - wx, y1 - bh - wy)
    G.button(BOOK, "LeftButton", "Back", aw, bh, { glyph = "lb", always = true })
    ui.SameLine(0, T.px(12))
    G.button(BOOK, "RightButton", "Next", aw, bh, { glyph = "rb", always = true })
    buttons(BOOK, { { "CastSpellButton", "Cast Spell" }, { "ScribeButton", "Scribe" },
      { "DoneButton", "Done", { primary = true, glyph = "b" } } }, x0 + 2 * aw + T.px(40), x1, y1 - bh, bh)
  end
  if W.finish() then API.windows.ClickChild(BOOK, "DoneButton", "left") end
end)

-- ---------------------------------------------------------------- spell displays
for i = 1, 6 do
  local window = i == 1 and "SpellDisplayWindow" or ("SpellDisplayWindow#" .. i)
  local name = "Spell" .. (i == 1 and "" or (" " .. i))
  local desc = {}
  ui.window(name, function()
    if not W.shown(window) then return end
    T.frame()
    local title = API.windows.Title(window) or ""
    -- (each further one a little lower and to the right, as the stock ones cascade)
    local off = (i - 1) * 40
    if W.begin(name, { w = 1000, h = 760, x = 460 + off, y = 160 + off, hints = { { "rs", "Scroll" }, { "b", "Close" } } }) then
      local x0, y0, x1, y1 = W.area()
      local size = T.px(72)
      local tex, u0, v0, u1, v1 = API.windows.ChildArt(window, "SDW_IconButton", "decal")
      T.rect(x0, y0, x0 + size, y0 + size, C.sunken, 1, T.px(10))
      if type(tex) == "userdata" then T.icon(tex, u0, v0, u1, v1, x0 + T.px(4), y0 + T.px(4), x0 + size - T.px(4), y0 + size - T.px(4)) end
      T.font(32)
      T.textIn(x0 + size + T.px(20), y0, x1 - T.px(60), y0 + size, T.fit(title ~= "" and title or "Spell", x1 - x0 - size - T.px(90)), C.text, 0)
      T.unfont()
      local ty = y0 + size + T.px(16)
      textBox(window, "SDW_SpellDescription", x0, ty, x1, y1, desc)
    end
    if W.finish() then API.windows.Show(window, false) end
  end)
end
