-- Name changes: replaces the game's NameChangeWnd, NameChangePetWnd and NameChangeMercWnd (the server's invitation
-- to rename the character, pet or mercenary, or /changename, /changepetname, /changemercname). One dialog each: the
-- prompt, the name box (15 characters), Submit Name (it reads "Request Pending" and is disabled while a request is
-- out), Disable Reminder (shown with the server's reminder; it asks first) and Remind Me Later or Close. Every piece
-- is the window's own (app/lua_ui_names.cpp); Enter in the box submits, as the original's message 6 does.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/dialogs_kit.lua")

local WINDOWS = {
  { "NameChangeWnd", "NCW_", "Change Name", "New name" },
  { "NameChangePetWnd", "NCPW_", "Change Pet Name", "New pet name" },
  { "NameChangeMercWnd", "NCMW_", "Change Mercenary Name", "New mercenary name" },
}

local OPTS = {
  size = function() return 400, 0 end, -- (sized to the prompt, which wraps at this width)
  place = function(sw, sh, w, h) return K.Centered(sw, sh, w, h) end,
}

for _, def in ipairs(WINDOWS) do
  local win, p, title, hint = def[1], def[2], def[3], def[4]
  -- the ids and tables each window uses, made once (not every frame)
  local promptId, editId = p .. "Prompt", p .. "NameEdit"
  local editOpts = {
    hint = hint,
    max = 16,
    onCommit = function()
      if ui.IsKeyPressed(ui.Key.Enter) then API.windows.Button(win, editId) end
    end,
  }
  local buttons = {
    { p .. "SubmitButton", nil, { primary = true } },
    { p .. "DisableReminderButton", "Disable Reminder", { tooltip = "Stop the server reminding you" } },
    { p .. "CloseButton", nil },
  }
  local function body(x, y, w, h, s)
    local bh = K.ButtonH(s)
    ui.Dummy(w, 0)
    ui.PushTextWrapPos(ui.GetCursorPos() + w)
    W.Label(win, promptId)
    ui.PopTextWrapPos()
    ui.Dummy(0, 4 * s)
    W.Edit(win, editId, editOpts)
    ui.Dummy(0, 4 * s)
    W.ButtonRow(win, buttons, bh)
  end
  ui.window(title, function()
    if not API.session.InGame() then return end
    hud.Dialog(win, title, OPTS, body)
  end)
end
