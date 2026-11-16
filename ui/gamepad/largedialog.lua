-- The large dialog (gamepad pack): stands in for LargeDialogWindow, the server's message box (a welcome text, a
-- level-up message, a question with its own Yes / No labels). The title, the text (STML) scrolled with the D-pad
-- up / down (or the wheel), and OK, or Yes and No: each a click on the stock window's button, so the game answers
-- the server as its own buttons do. B or the close button closes it ("closed", as the stock close box).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local S = dofile("lib/stml.lua")
local C = T.C

local LDW = "LargeDialogWindow"
local scroll, lastText = 0, nil

ui.window("Message", function()
  if not W.shown(LDW) then
    scroll, lastText = 0, nil
    return
  end
  local okOnly = API.windows.ChildShown(LDW, "LDW_YesButton") ~= true
  local yes = API.windows.Text(LDW, "LDW_YesButton") or ""
  local no = API.windows.Text(LDW, "LDW_NoButton") or ""
  if yes == "" then yes = "Yes" end
  if no == "" then no = "No" end
  local hints = okOnly and { { "a", "OK" }, { "up", "Scroll" }, { "down", "Scroll" }, { "b", "Close" } }
      or { { "a", "Select" }, { "up", "Scroll" }, { "down", "Scroll" }, { "b", "Close" } }
  local title = API.windows.Title(LDW) or ""
  if W.begin("Message", { w = 1100, h = 760, title = title ~= "" and title or "Message", hints = hints,
        alwaysFocus = true }) then
    local x0, y0, x1, y1, w = W.area()
    local wx, wy = ui.GetWindowPos()
    local bh = T.px(64)
    local textY1 = y1 - bh - T.px(16)
    -- the text, word-wrapped (its <br> are line breaks)
    local text = API.windows.Text(LDW, "LDW_TextBox") or ""
    if text ~= lastText then scroll, lastText = 0, text end
    local rows = S.blockRows(text, w - T.px(36)) -- (laid out again only when the text, width or font change)
    T.rect(x0, y0, x1, textY1, C.sunken, 1, T.px(14))
    local _, lh = ui.CalcTextSize("Ag")
    local fit = math.max(1, math.floor((textY1 - y0 - T.px(24)) / lh))
    local maxScroll = math.max(0, #rows - fit)
    if W.focused() then
      if pad.menuPressed("down") then scroll = scroll + 3 end
      if pad.menuPressed("up") then scroll = scroll - 3 end
      local ry = pad.axis("ry") or 0
      if math.abs(ry) > 0.3 then scroll = scroll + ry * 0.4 end
    end
    if ui.IsMouseHoveringRect(x0, y0, x1, textY1) then scroll = scroll - (ui.GetIO().wheel or 0) * 3 end
    scroll = math.max(0, math.min(maxScroll, scroll))
    ui.PushClipRect(x0 + T.px(8), y0 + T.px(8), x1 - T.px(8), textY1 - T.px(8), true)
    local first = math.floor(scroll) + 1
    for i = first, math.min(#rows, first + fit - 1) do S.drawRow(rows[i], x0 + T.px(18), y0 + T.px(12) + (i - first) * lh) end
    ui.PopClipRect()
    if maxScroll > 0 then -- a scroll bar on the right
      local sx, th = x1 - T.px(10), (textY1 - y0) * fit / #rows
      local ty = y0 + (textY1 - y0 - th) * (scroll / maxScroll)
      T.rect(sx, ty, sx + T.px(5), ty + th, C.dim, 0.8, T.px(3))
    end
    -- the buttons at the bottom right
    local bw = T.px(300)
    ui.SetCursorPos(x1 - wx - (okOnly and bw or 2 * bw + T.px(16)), y1 - wy - bh)
    if okOnly then
      if T.button("##ok", "OK", bw, bh, { primary = true, glyph = "a" }, pad) then API.windows.ClickChild(LDW, "LDW_OkButton", "left") end
    else
      if T.button("##yes", T.fit(yes, bw - T.px(70)), bw, bh, { primary = true, glyph = "a" }, pad) then
        API.windows.ClickChild(LDW, "LDW_YesButton", "left")
      end
      ui.SameLine(0, T.px(16))
      if T.button("##no", T.fit(no, bw - T.px(70)), bw, bh, {}, pad) then API.windows.ClickChild(LDW, "LDW_NoButton", "left") end
    end
  end
  if W.finish() then API.windows.Show(LDW, false) end
end)
