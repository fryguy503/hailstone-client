-- Respawn: replaces the game's RespawnWnd (the server's choice of where to return after death, shown while you hover
-- over your corpse). The choices as a table (the place and its zone; the server's default and unavailable choices
-- marked), the timer bar (the game's gauge, EQType 28) and Respawn, which the window
-- enables once its delay has passed. A row click is the window's list click, Respawn its Select button, so the
-- choice goes to the server as the stock window sends it.
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "RespawnWnd"

-- A small rounded tag after a row's name.
local function badge(text, col)
  ui.SameLine(0, 6)
  local x, y = ui.GetCursorScreenPos()
  ui.PushFont(T.font.tiny)
  local tw, th = ui.CalcTextSize(text)
  ui.PopFont()
  local _, lh = ui.CalcTextSize("Ag")
  local by = y + (lh - th) / 2 - 1
  ui.DrawRectFilled(x, by, x + tw + 8, by + th + 2, col[1], col[2], col[3], 0.22, 4)
  T.Text(x + 4, by + 1, text, col, T.font.tiny)
  ui.Dummy(tw + 8, lh)
end

local WINDOW_OPTS = {
  size = function() return 380, 250 end,
  resizable = { 280, 180 },
  closable = false, -- the server's window: it closes when we are moved
  place = function(W_, H, w, h) return W_ / 2 - w / 2, H * 0.30 end,
}
local function drawWindow(x, y, w, h, s)
  local bh = K.ButtonH(s)
  local ready = W.State(WINDOW, "SelectButton")
  K.Caption("Choose an available destination")
  W.Gauge(WINDOW, "TimeGauge", { eqtype = 28, color = C.hp, height = 8, s = s })
  ui.Dummy(0, 2 * s)
  W.List(WINDOW, "OptionsList", 0, -K.FooterH(s), {
    hide = { [1] = true },
    columns = { [2] = { "Destination", 0 }, [3] = { "Zone", 0 } },
    onRow = function(row, texts)
      local unavailable = texts.disabled == true -- (texts[1] is the original's marker: "*" default, "r" flagged)
      local col = (texts.colors and texts.colors[2]) or (unavailable and C.muted or C.text)
      ui.TextColored(col[1], col[2], col[3], 1, texts[2] or "")
      if unavailable then badge("unavailable", C.muted) elseif texts[1] == "*" then badge("default", C.accent) end
      ui.TableNextColumn()
      local zoneColor = (texts.colors and texts.colors[3]) or C.muted
      ui.TextColored(zoneColor[1], zoneColor[2], zoneColor[3], 1, texts[3] or "")
      return true
    end,
  })
  W.Button(WINDOW, "SelectButton", "Respawn", {
    primary = true, width = -1, height = bh,
    tooltip = ready and "Return at the selected place" or "Choose an available destination; the server may require a short delay",
  })
end

local offerShown = false
ui.window("Respawn", function()
  if not eq.InGame() then return end
  if not eq.WindowShown(WINDOW) then offerShown = false; return end
  local offer = eq.WindowShown("ConfirmationDialogBox") and eq.ChildShown("ConfirmationDialogBox", "ResurrectionOffer") == true
  local takeFocus, returnFocus = offer and not offerShown, offerShown and not offer
  offerShown = offer
  hud.Dialog(WINDOW, "Respawn", WINDOW_OPTS, drawWindow)
  -- A resurrection offer goes in front of the choices once, on the frame it (or this window) appears, the timer
  -- still drawn behind it. Not every frame: focusing a window takes the active widget from the others, so chat
  -- typing and open popups would stop working while the offer waits.
  if takeFocus then ui.SetWindowFocus("###modern.ConfirmationDialogBox")
  elseif returnFocus then ui.SetWindowFocus("###modern.RespawnWnd") end
end)
