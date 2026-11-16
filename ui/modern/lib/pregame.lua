-- The pieces the modern pre-game screens share (login.lua, serverselect.lua, charselect.lua): a borderless card
-- window in this UI's style over the client's backdrop picture, placed by the script (centred, or beside another
-- card), and the status line, buttons, pills and confirmations drawn inside it. The screens are the client's own
-- (app/lua_ui_pregame.cpp: hs.LoginScreen and the rest), not the original's, so nothing here follows eqmain.dll's
-- pages. Sizes are design pixels times s, the UI scale.
--
--   local P = dofile("lib/pregame.lua")
--   local w, h = P.Size("login", 420, 300)              -- last frame's size (the card sizes itself to its contents)
--   if P.Begin("login", x, y, 420 * s, s) then ... end
--   P.End()
local style = dofile("lib/style.lua")
local T = dofile("lib/theme.lua")
local C = T.color

local P = { T = T }

-- The scale the pre-game cards draw at: the display's UI scale (no HUD scale: the HUD is not up yet).
function P.Scale() return ui.GetIO().uiScale or 1 end

-- key -> { w, h } of the card as drawn last frame. A card's height follows its contents, and the
-- binding's SetNextWindowPos has no pivot, so a centred place is worked out from the size it had last frame.
local sizes = {}
function P.Size(key, w, h)
  local z = sizes[key]
  if z then return z[1], z[2] end
  return w, h
end

local pushed = 0 -- what P.Begin pushed inside the window, for P.End
local function rgba(c) return c[1], c[2], c[3], c[4] or 1 end

-- A card: a window with no title bar at x, y (screen pixels, kept there every frame), `width` wide, its height its
-- contents'. Returns whether it is drawn; call P.End() whatever it returned.
function P.Begin(key, x, y, width, s)
  style.Push()
  ui.PushStyleColor(ui.Col.WindowBg, rgba(C.window))
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 18 * s, 16 * s)
  ui.PushStyleVar(ui.StyleVar.WindowRounding, 10 * s)
  ui.SetNextWindowPos(math.floor(x), math.floor(y), ui.Cond.Always)
  ui.SetNextWindowSize(width, 0, ui.Cond.Always)
  -- (a size of 0 fits that axis to the contents each frame: the width stays the card's, the height follows)
  local flags = ui.WindowFlags.NoDecoration | ui.WindowFlags.NoSavedSettings | ui.WindowFlags.NoMove
      | ui.WindowFlags.NoCollapse
  local visible = ui.Begin("###modern.pregame." .. key, nil, flags)
  ui.PopStyleVar(2)
  local ww, wh = ui.GetWindowSize()
  sizes[key] = { ww, wh }
  -- fields, boxes and buttons outlined in the palette's frameBorder (as hud.Dialog's), the card's edge keeping its
  -- hairline; roomier rows than the in-game dialogs, there being few of them
  ui.PushStyleColor(ui.Col.Border, rgba(C.frameBorder))
  ui.PushStyleVar(ui.StyleVar.FrameBorderSize, 1)
  ui.PushStyleVar(ui.StyleVar.FramePadding, 8 * s, 5 * s)
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, 8 * s, 7 * s)
  pushed = 3
  return visible
end

function P.End()
  ui.PopStyleVar(pushed)
  ui.PopStyleColor()
  pushed = 0
  ui.End()
  ui.PopStyleColor()
  style.Pop()
end

-- ---------------------------------------------------------------- text
function P.Caption(text)
  ui.PushFont(T.font.small)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, text)
  ui.PopFont()
end

function P.Muted(text)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, text)
end

function P.Faint(text)
  ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, text)
end

-- A section heading: the palette's heading colour over a hairline.
function P.Heading(text)
  ui.PushStyleColor(ui.Col.Text, rgba(C.heading))
  ui.SeparatorText(text)
  ui.PopStyleColor()
end

-- The card's title: the wordmark (or a screen's name) large, a muted line under it.
function P.Title(title, subtitle, s)
  ui.PushFont(T.font.huge)
  ui.SetWindowFontScale(1.45)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, title)
  ui.SetWindowFontScale(1)
  ui.PopFont()
  if subtitle and subtitle ~= "" then P.Muted(subtitle) end
  ui.Dummy(0, 2 * s)
end

-- A tooltip for the item just drawn, also while it is disabled.
function P.Tip(text)
  if text and text ~= "" and ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then ui.SetTooltip(text) end
end

-- ---------------------------------------------------------------- buttons
-- A button in the accent colour (the screen's one main action), `w` wide (-1: the rest of the line).
function P.Primary(label, w, h, enabled)
  local A, on = C.accent, C.onAccent
  ui.BeginDisabled(not enabled)
  ui.PushStyleColor(ui.Col.Button, A[1], A[2], A[3], 0.85)
  ui.PushStyleColor(ui.Col.ButtonHovered, A[1], A[2], A[3], 1)
  ui.PushStyleColor(ui.Col.ButtonActive, A[1] * 0.8, A[2] * 0.8, A[3] * 0.8, 1)
  ui.PushStyleColor(ui.Col.Text, rgba(on))
  local clicked = ui.Button(label, w, h)
  ui.PopStyleColor(4)
  ui.EndDisabled()
  return clicked and enabled
end

-- A button for an action that cannot be undone (delete), in the palette's danger colour.
function P.Danger(label, w, h, enabled)
  local D = C.danger
  ui.BeginDisabled(enabled == false)
  ui.PushStyleColor(ui.Col.Button, D[1], D[2], D[3], 0.22)
  ui.PushStyleColor(ui.Col.ButtonHovered, D[1], D[2], D[3], 0.45)
  ui.PushStyleColor(ui.Col.ButtonActive, D[1], D[2], D[3], 0.65)
  local clicked = ui.Button(label, w or 0, h or 0)
  ui.PopStyleColor(3)
  ui.EndDisabled()
  return clicked and enabled ~= false
end

-- The width of a row of buttons with these labels (for right-aligning them).
function P.ButtonsWidth(labels, s)
  local w = 0
  for _, l in ipairs(labels) do w = w + ui.CalcTextSize(l) + 16 * s end
  return w + 8 * s * (#labels - 1)
end

-- Puts the cursor so `width` ends at the right edge of the line.
function P.AlignRight(width)
  local avail = ui.GetContentRegionAvail()
  local x, y = ui.GetCursorPos()
  if avail > width then ui.SetCursorPos(x + avail - width, y) end
end

-- ---------------------------------------------------------------- pieces
-- A rounded tag at x, y (screen pixels) in `col`; returns its width.
function P.Pill(x, y, text, col, s)
  ui.PushFont(T.font.tiny)
  local tw, th = ui.CalcTextSize(text)
  ui.PopFont()
  local pw, ph = tw + 12 * s, th + 4 * s
  ui.DrawRectFilled(x, y, x + pw, y + ph, col[1], col[2], col[3], 0.20, ph / 2)
  ui.DrawRect(x, y, x + pw, y + ph, col[1], col[2], col[3], 0.55, ph / 2, 1)
  T.Text(x + 6 * s, y + 2 * s, text, col, T.font.tiny)
  return pw, ph
end

-- A turning arc of `radius` at the cursor, inline (while the screen waits on the server).
function P.Spinner(radius, s)
  local x, y = ui.GetCursorScreenPos()
  ui.Dummy(radius * 2, radius * 2)
  local cx, cy = x + radius, y + radius
  local t = ui.GetTime() * 6
  local A = C.accent
  local n = 14
  for k = 0, n - 1 do
    local a0 = t + k / n * math.pi * 1.5
    local a1 = t + (k + 1) / n * math.pi * 1.5
    local r = radius * 0.8
    ui.DrawLine(cx + math.cos(a0) * r, cy + math.sin(a0) * r, cx + math.cos(a1) * r, cy + math.sin(a1) * r,
      A[1], A[2], A[3], 0.35 + 0.65 * k / n, math.max(1.5, radius * 0.3))
  end
end

-- The screen's status line: its text (red when an error), a spinner while busy. Always one line high, so the card
-- does not jump when a status comes and goes.
function P.Status(status, busy, s)
  local text = status and status.text or ""
  local _, lh = ui.CalcTextSize("Ag")
  if busy then
    P.Spinner(lh / 2, s)
    ui.SameLine()
    if text == "" then text = "Working..." end
  end
  if text == "" then
    ui.Dummy(0, lh)
    return
  end
  local col = status and status.error and C.danger or C.muted
  ui.PushStyleColor(ui.Col.Text, rgba(col))
  ui.TextWrapped(text)
  ui.PopStyleColor()
end

-- A confirmation popup (opened with ui.OpenPopup(id)): a question, a muted note and the action's button in the
-- danger colour beside Cancel. Returns true the frame the action is clicked. (A popup, not a modal one: the
-- bindings have no BeginPopupModal; a click outside it cancels.)
function P.Confirm(id, question, note, action, s)
  local confirmed = false
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 14 * s, 12 * s)
  local open = ui.BeginPopup(id)
  ui.PopStyleVar()
  if open then
    ui.Text(question)
    if note then
      ui.PushTextWrapPos(ui.GetCursorPos() + 320 * s)
      P.Muted(note)
      ui.PopTextWrapPos()
    end
    ui.Dummy(0, 4 * s)
    if P.Danger(action, 0, 0, true) then
      confirmed = true
      ui.CloseCurrentPopup()
    end
    ui.SameLine()
    if ui.Button("Cancel") or ui.IsKeyPressed(ui.Key.Escape) then ui.CloseCurrentPopup() end
    ui.EndPopup()
  end
  return confirmed
end

-- A selectable card row `h` high across the line: returns clicked, double-clicked, and its screen rectangle for the
-- caller to draw the row's text into. Selected rows carry an accent bar at their left.
function P.Row(id, selected, h, s)
  local A = C.accent
  ui.PushStyleColor(ui.Col.Header, A[1], A[2], A[3], 0.16)
  ui.PushStyleColor(ui.Col.HeaderHovered, A[1], A[2], A[3], 0.10)
  ui.PushStyleColor(ui.Col.HeaderActive, A[1], A[2], A[3], 0.24)
  local clicked = ui.Selectable(id, selected, ui.SelectableFlags.AllowDoubleClick, 0, h)
  ui.PopStyleColor(3)
  local double = clicked and ui.IsMouseDoubleClicked(ui.Mouse.Left)
  local x0, y0, x1, y1 = ui.GetItemRect()
  if selected then ui.DrawRectFilled(x0 + 3 * s, y0 + 5 * s, x0 + 6 * s, y1 - 5 * s, A[1], A[2], A[3], 1, 2 * s) end
  return clicked, double, x0, y0, x1, y1
end

return P
