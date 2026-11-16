-- Book: replaces the game's BookWindow (a book read from the inventory). The open spread as two paper-toned pages
-- side by side (one above the other when the window is narrow), their page numbers, and a footer with the page
-- arrows, Cast Spell / Scribe (shown when the book teaches a spell) and Done. The pages are the window's Page0 and
-- Page1 (the game pages the text, 12 lines of under 31 characters each); the arrows, or the mouse wheel over the
-- pages, are clicks on its LeftButton / RightButton, which go grey at the book's ends as the stock ones do.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "BookWindow"

local PAPER = { 0.115, 0.100, 0.080, 0.92 }
local INK = { 0.95, 0.90, 0.80, 1 }

local PAGE_BOX = { plain = true, font = T.font.normal, bg = PAPER, color = INK }
local LINE = string.rep("n", 30) -- a page line's width (the game's lines are under 31 characters)

local function page(id, numberId, w, h, s)
  ui.BeginGroup()
  K.TextBox(WINDOW, id, w, h - 20 * s, PAGE_BOX)
  local n = W.Text(WINDOW, numberId)
  local tw = ui.CalcTextSize(n)
  local cx, cy = ui.GetCursorPos()
  ui.SetCursorPos(cx + (w - tw) / 2, cy)
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, n)
  ui.EndGroup()
end

local OPTS = {
  size = function() return 620, 400 end,
  resizable = { 280, 260 },
}
local LEFT_OPTS, RIGHT_OPTS = { tooltip = "Previous pages" }, { tooltip = "Next pages" }
local CAST = { "CastSpellButton", "Cast Spell", {} }
local SCRIBE = { "ScribeButton", "Scribe", {} }
local DONE = { "DoneButton", "Done", { primary = true } }
local right = {}

local function body(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local ph = h - K.FooterH(s)
  local gap = 10 * s
  -- the spread: side by side when both pages have room for a line, else stacked
  local lineW = ui.CalcTextSize(LINE) + 24 * s
  local x0, y0 = ui.GetCursorScreenPos()
  if w >= 2 * lineW + gap then
    local pw = (w - gap) / 2
    page("Page0", "Page0Number", pw, ph, s)
    ui.SameLine(0, gap)
    page("Page1", "Page1Number", pw, ph, s)
  else
    local half = (ph - 4 * s) / 2
    page("Page0", "Page0Number", w, half, s)
    page("Page1", "Page1Number", w, half, s)
  end
  -- the mouse wheel over the pages turns them
  local wheel = ui.GetIO().wheel or 0
  if wheel ~= 0 and ui.IsWindowHovered(ui.HoveredFlags.ChildWindows)
      and ui.IsMouseHoveringRect(x0, y0, x0 + w, y0 + ph) then
    local id = wheel < 0 and "RightButton" or "LeftButton"
    if W.State(WINDOW, id) then API.windows.ClickChild(WINDOW, id, "left") end
  end
  -- the footer: the arrows on the left, the other buttons on the right
  local aw = 44 * s
  LEFT_OPTS.width, LEFT_OPTS.height = aw, bh
  W.Button(WINDOW, "LeftButton", "<", LEFT_OPTS)
  ui.SameLine()
  RIGHT_OPTS.width, RIGHT_OPTS.height = aw, bh
  W.Button(WINDOW, "RightButton", ">", RIGHT_OPTS)
  for i = #right, 1, -1 do right[i] = nil end
  if API.windows.ChildShown(WINDOW, "CastSpellButton") == true then right[#right + 1] = CAST end
  if API.windows.ChildShown(WINDOW, "ScribeButton") == true then right[#right + 1] = SCRIBE end
  right[#right + 1] = DONE
  local bw = 96 * s
  local rowW = #right * bw + (#right - 1) * 8
  ui.SameLine(math.max(2 * aw + 24 * s, w - rowW + 10 * s))
  for i, b in ipairs(right) do
    if i > 1 then ui.SameLine() end
    local o = b[3]
    o.width, o.height = bw, bh
    W.Button(WINDOW, b[1], b[2], o)
  end
end

ui.window("Book", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Book", OPTS, body)
end)
