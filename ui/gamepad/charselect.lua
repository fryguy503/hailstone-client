-- The character select (gamepad pack): hs.CharacterSelect stands in for the client's own (ui/dev_screens.cpp
-- character_select_screen). A Hailstone screen, not the original's. The characters as tiles; with the pad the one
-- focused is the one selected, so the scene in the middle of the screen shows its model (the panel goes to the left
-- then). A or Start enters the world, X creates a character, Y deletes the selected one (asked first: A deletes, B
-- keeps it), B goes back to the login screen. Return Home, beside Enter World, is there when the server lets the
-- selected character go home (asked first too). With the mouse a click selects and a double click enters.
-- The keyboard's keys here (Enter, Escape, the arrows: the character select key slots 493..496) are the client's
-- (ClientApp::screen_keys), so this window leaves them alone: taking them too would do each thing twice.
local API = hs.UI
local T = dofile("lib/theme.lua")
local W = dofile("lib/window.lua")
local P = dofile("lib/pregame.lua")
local C = T.C

local NAME = "Characters"
local ROW_H = 100
local st = { list = { offset = 0 }, frame = -10, sel = 0, clickAt = 0, clickRow = 0, confirm = nil, home = false }
local F = P.focus()

local function px(n) return T.px(n) end

-- Puts the pad's focus on row i of the list, scrolled into view.
local function focusRow(i, count, visible)
  local s = st.list
  if i <= s.offset or i > s.offset + visible then s.offset = math.max(0, math.min(i - 1, count - visible)) end
  s.focusNext = i - s.offset
end

local function selectRow(i)
  if API.pregame.CharacterSelectDo("select", i) then st.sel = i end
end

-- A row's choice: with the pad A enters with it; with the mouse a click selects it, a second soon after enters.
local function chosen(i)
  if P.padDrives() then
    API.pregame.CharacterSelectDo("enter", i)
    return
  end
  local now = ui.GetTime()
  if st.clickRow == i and now - st.clickAt < 0.45 then
    st.clickRow = 0
    API.pregame.CharacterSelectDo("enter", i)
  else
    selectRow(i)
    st.clickRow, st.clickAt = i, now
  end
end

local function askDelete(v)
  st.confirm, st.home = v.selected, false
  F:want("##delyes")
end

local function askHome(v)
  st.confirm, st.home = v.selected, true
  F:want("##delyes")
end

-- The question before a delete (or a Return Home), over the window: Delete (Return Home) has the focus (A), B or
-- Cancel keeps the character as it is.
local function drawConfirm(v, wx0, wy0, wx1, wy1)
  local c = v.characters[st.confirm]
  if not c or v.busy or (st.home and not c.canGoHome) then
    st.confirm = nil
    return
  end
  local title = st.home and ("Return " .. c.name .. " home?") or ("Delete " .. c.name .. "?")
  local note = st.home and c.homeQuestion or "This cannot be undone."
  ui.DrawRectFilled(wx0, wy0, wx1, wy1, 0, 0, 0, 0.55, px(18))
  local w, h = math.min(wx1 - wx0 - px(48), px(620)), px(250)
  local x0, y0 = (wx0 + wx1 - w) / 2, (wy0 + wy1 - h) / 2
  T.panel(x0, y0, x0 + w, y0 + h, 1, px(16), C.raised)
  T.font(26)
  local _, th = ui.CalcTextSize("Ag")
  T.textIn(x0 + px(24), y0 + px(22), x0 + w - px(24), y0 + px(22) + th, T.fit(title, w - px(48)), C.text, 0.5)
  T.unfont()
  T.font(19)
  T.textIn(x0 + px(24), y0 + px(30) + th, x0 + w - px(24), y0 + px(64) + th, T.fit(note, w - px(48)), C.dim, 0.5)
  T.unfont()
  local bw = (w - px(48) - px(16)) / 2
  P.at(x0 + px(24), y0 + h - px(24) - px(72))
  if P.button(F, "##delyes", st.home and "Return Home" or "Delete", bw, px(72), { primary = true, glyph = "a" }) then
    st.confirm = nil
    if st.home then
      API.pregame.CharacterSelectDo("home")
    else
      API.pregame.CharacterSelectDo("delete")
      F:want("##create")
    end
  end
  ui.SameLine(0, px(16))
  if P.button(F, "##delno", "Cancel", bw, px(72), { glyph = "b" }) or P.press("b") then
    st.confirm = nil
    F:want(nil)
    st.list.focusNext = math.max(1, st.sel - st.list.offset)
  end
end

-- (kept from frame to frame: tables made each frame are garbage the collector works through)
local HINTS_CONFIRM = { { "a", "Select" }, { "b", "Cancel" } }
local HINTS_NONE = { { "a", "Select" }, { "x", "Create" }, { "b", "Back" } }
local HINTS = { { "a", "Enter World" }, { "x", "Create" }, { "y", "Delete" }, { "b", "Back" } }
local OPTS = { close = false, alwaysFocus = true, title = "Characters" }
local where = {}
local held = nil -- the view drawn last frame: hs.CharacterSelect gives true while it still holds (its revision)

ui.window(NAME, function()
  local v = API.pregame.CharacterSelect and API.pregame.CharacterSelect(held and held.revision)
  if v == true then v = held else held = v end
  if not v then
    st.frame = -10
    return
  end
  local frame = ui.GetFrameCount()
  local appearing = frame - st.frame > 1
  st.frame = frame
  local n = #v.characters
  -- (the scene's model stands in the middle: the panel keeps to the left of it)
  -- (as tall as its characters need, from three rows up to most of the screen)
  local rows = math.max(3, math.min(n, 7))
  local ww = v.scene and 720 or 920
  local wh = math.min(1000, 120 + rows * (ROW_H + 10) + 2 * 82 + 64 + 48 + 24 + 10)
  local footerRows = 2
  where[1], where[2] = nil, nil
  if v.world ~= "" then where[#where + 1] = v.world end
  if v.server ~= "" and v.server ~= v.world then where[#where + 1] = v.server end
  local opts = OPTS
  opts.w, opts.h, opts.x = ww, wh, v.scene and 70 or nil
  opts.subtitle = table.concat(where, "  ·  ")
  opts.hints = st.confirm and HINTS_CONFIRM or n == 0 and HINTS_NONE or HINTS
  if W.begin(NAME, opts) then
    local x0, y0, x1, y1, w = W.area()
    local busy = v.busy
    local rh, gap = px(ROW_H), px(10)
    local footer = footerRows * (px(72) + gap) + px(50) + px(14) -- (the status: two lines)
    local visible = math.max(1, math.floor((y1 - y0 - footer + gap) / (rh + gap)))
    local shown = math.min(visible, n)
    if appearing then
      st.confirm = nil
      st.sel = v.selected
      if n > 0 then focusRow(v.selected > 0 and v.selected or 1, n, shown) else F:want("##create") end
    elseif v.selected ~= st.sel then
      -- the selection moved without the pad (the arrow keys): the focus follows it
      st.sel = v.selected
      if v.selected > 0 and P.padDrives() then focusRow(v.selected, n, shown) end
    end
    if not st.confirm and not busy then
      if P.press("start") and v.selected > 0 then API.pregame.CharacterSelectDo("enter") end
      if P.press("x") then API.pregame.CharacterSelectDo("create") end
      if P.press("y") and v.selected > 0 then askDelete(v) end
    end
    if not st.confirm and P.press("b") then API.pregame.CharacterSelectDo("back") end

    if st.confirm then
      -- (in place of the list and buttons: the pad's focus cannot wander off the question)
      local wx, wy = ui.GetWindowPos()
      local sw, sh = ui.GetWindowSize()
      drawConfirm(v, wx, wy, wx + sw, wy + sh)
    else
      local y = y0
      P.at(x0, y)
      if n == 0 then
        -- nobody yet: a word, and Create has the focus
        local h = shown > 0 and shown or 3
        local by1 = y + h * (rh + gap) - gap
        T.rect(x0, y, x1, by1, C.sunken, 1, px(14))
        T.font(30)
        local _, th = ui.CalcTextSize("Ag")
        T.textIn(x0, (y + by1) / 2 - th, x1, (y + by1) / 2, "No characters yet", C.text, 0.5)
        T.unfont()
        T.font(20)
        T.textIn(x0, (y + by1) / 2 + px(6), x1, (y + by1) / 2 + px(40), "Create one to begin your adventure.", C.dim, 0.5)
        T.unfont()
        ui.Dummy(w, by1 - y)
      else
        local row = W.list("chars", st.list, n, shown, ROW_H, w, function(i, rx0, ry0, rx1, ry1, focused)
          local c = v.characters[i]
          local fa = busy and 0.45 or 1
          local ix0, ix1 = rx0 + px(22), rx1 - px(18)
          if i == v.selected then T.rect(rx0, ry0 + px(14), rx0 + px(5), ry1 - px(14), C.accent, 1, px(3)) end
          T.font(27)
          local _, nh = ui.CalcTextSize("Ag")
          T.text(ix0, ry0 + px(10), T.fit(c.name, ix1 - ix0), C.text, fa)
          T.unfont()
          T.font(19)
          local _, lh = ui.CalcTextSize("Ag")
          local what = "Level " .. c.level .. (c.classShort ~= "" and (" " .. c.classShort) or "")
          if c.raceName ~= "" then what = what .. "  ·  " .. c.raceName end
          T.text(ix0, ry0 + px(10) + nh, T.fit(what, ix1 - ix0), C.dim, fa)
          T.unfont()
          if c.zone ~= "" then
            T.font(17)
            T.text(ix0, ry0 + px(12) + nh + lh, T.fit(c.zone, ix1 - ix0), C.faint, fa)
            T.unfont()
          end
        end, function(i)
          if not busy and not st.confirm then chosen(i) end
        end)
        st.list.selected = v.selected
        -- with the pad the focused character is the selected one (the scene shows it)
        if row and P.padDrives() and row ~= v.selected and not busy and not st.confirm then selectRow(row) end
      end

      -- the buttons under the list, the status line over them
      local by = y1 - footerRows * (px(72) + gap) + gap
      local bh = px(72)
      local canEnter = not busy and v.selected > 0
      P.at(x0, by)
      local picked = v.characters[v.selected]
      local canHome = canEnter and picked ~= nil and picked.canGoHome
      local hw = (w - 2 * gap) / 3
      if P.button(F, "##enter", "Enter World", w - hw - gap, bh, { primary = canEnter, disabled = not canEnter, glyph = "a" }) then
        API.pregame.CharacterSelectDo("enter")
      end
      ui.SameLine(0, gap)
      if P.button(F, "##home", "Return Home", hw, bh, { disabled = not canHome }) then askHome(v) end
      -- Create, Delete and Back on one row
      local cw = (w - 2 * gap) / 3
      P.at(x0, by + bh + gap)
      if P.button(F, "##create", "Create Character", cw, bh, { glyph = "x", disabled = busy }) then
        API.pregame.CharacterSelectDo("create")
      end
      ui.SameLine(0, gap)
      if P.button(F, "##delete", "Delete", cw, bh, { glyph = "y", disabled = not canEnter }) then askDelete(v) end
      ui.SameLine(0, gap)
      if P.button(F, "##back", "Back", cw, bh, { glyph = "b" }) then API.pregame.CharacterSelectDo("back") end
      P.status(v.status, busy, x0, by - px(12), x1, { bottom = true, lines = 2 })
    end
    F:tick()
  end
  W.finish()
end)
