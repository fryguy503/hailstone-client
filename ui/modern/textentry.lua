-- Text prompt: replaces the game's TextEntryWnd (a window asking for a name or a line of text: a bandolier set's
-- name, ...). The prompt, the text box (focused when the prompt opens; Enter accepts), OK and Cancel. What is typed
-- goes into the window's Entry box (hs.SetWindowText: the window keeps it to its length), and Enter is a click on
-- Entry, which the window takes as OK, as the stock edit box's Enter.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "TextEntryWnd"

local focus = true -- focus the box when the prompt opens

local WINDOW_OPTS = {
  rollup = false, -- (a question to answer: kept whole)
  size = function() return 360, 120 end,
  resizable = { 240, 110 },
  place = K.Centered,
}
local function drawWindow(x, y, w, h, s)
  local prompt = W.Text(WINDOW, "Prompt")
  ui.PushTextWrapPos(0)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, prompt ~= "" and prompt or "Enter the text:")
  ui.PopTextWrapPos()
  ui.Dummy(0, 2 * s)
  if focus then
    ui.SetKeyboardFocusHere()
    focus = false
  end
  local current = W.Text(WINDOW, "Entry")
  ui.SetNextItemWidth(-1)
  local text, enter = ui.InputText("##entry", current, 256, ui.InputTextFlags.EnterReturnsTrue)
  if text ~= current then API.windows.SetText(WINDOW, "Entry", text) end
  if enter then API.windows.ClickChild(WINDOW, "Entry", "left") end
  -- the buttons at the bottom of the window
  local bh = K.ButtonH(s)
  local _, cy = ui.GetCursorPos()
  local _, wh = ui.GetWindowSize()
  local by = wh - bh - 8 * s
  if by > cy then ui.SetCursorPos(10 * s, by) end
  W.ButtonRow(WINDOW, {
    { "OK", "OK", { primary = true, tooltip = "Accept (Enter)" } },
    { "Cancel", "Cancel" },
  }, bh)
end

ui.window("Enter Text", function()
  if not API.session.InGame() then return end
  local open = API.windows.Shown(WINDOW)
  if not open then focus = true return end
  hud.Dialog(WINDOW, "Enter Text", WINDOW_OPTS, drawWindow)
end)
