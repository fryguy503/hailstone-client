-- The gamepad as the gamepad pack's windows see it: hs.gamepad (docs/gamepad.md section 5) when the client has it,
-- else a stand-in that reports no pad, so every window still works with the mouse and keyboard. Each call looks
-- hs.gamepad up again, so the real API is used as soon as it is there; what cannot change within a frame (whether a
-- pad is connected, its type and labels) is read once a frame, since every focusable row asks.
--
-- Also the ImGui navigation calls the pack needs (ui.IsItemFocused, ui.SetWindowFocus, ui.IsWindowAppearing,
-- ui.GetIO().nav_active / nav_visible), with stand-ins for a client that does not have them yet.
local API = hs.UI
local pad = {}

-- ---------------------------------------------------------------- stand-in
-- Labels per pad type (hs.gamepad.glyph's answers), for the stand-in and for drawing the face buttons.
local GLYPHS = {
  xbox = { a = "A", b = "B", x = "X", y = "Y", lb = "LB", rb = "RB", lt = "LT", rt = "RT", back = "View",
    start = "Menu", guide = "Xbox", ls = "LS", rs = "RS", up = "Up", down = "Down", left = "Left", right = "Right",
    touchpad = "Touchpad" },
  playstation = { a = "Cross", b = "Circle", x = "Square", y = "Triangle", lb = "L1", rb = "R1", lt = "L2", rt = "R2",
    back = "Share", start = "Options", guide = "PS", ls = "L3", rs = "R3", up = "Up", down = "Down", left = "Left",
    right = "Right", touchpad = "Touchpad" },
  nintendo = { a = "B", b = "A", x = "Y", y = "X", lb = "L", rb = "R", lt = "ZL", rt = "ZR", back = "-", start = "+",
    guide = "Home", ls = "LS", rs = "RS", up = "Up", down = "Down", left = "Left", right = "Right", touchpad = "Touchpad" },
}
GLYPHS.generic = GLYPHS.xbox
pad.GLYPHS = GLYPHS
-- The default bindings (docs/gamepad.md section 3), shown by the Options page while the client has no hs.gamepad.
local DEFAULT_BINDINGS = {
  a = "interact", b = "cancel", x = "AUTOPRIM", y = "CMD_JUMP", lb = "target_prev", rb = "target_next",
  up = "target_group", down = "SIT_STAND", left = "hotpage_prev", right = "hotpage_next", ls = "AUTORUN",
  rs = "camera_toggle", start = "menu", back = "cursor", ["back+start"] = "chat",
}
pad.DEFAULT_BINDINGS = DEFAULT_BINDINGS
local DEFAULT_SETTINGS = { enabled = 1, deadzone = 0.2, trigger_threshold = 0.5, camera_speed = 2.5, invert_y = 0, rumble = 1,
  move_threshold = 0.35, strafe_threshold = 0.5, cursor_speed = 900 }
local INPUTS = { a = 1, b = 1, x = 1, y = 1, lb = 1, rb = 1, lt = 1, rt = 1, back = 1, start = 1, guide = 1, ls = 1, rs = 1,
  up = 1, down = 1, left = 1, right = 1, touchpad = 1 }
local function knownInput(input)
  if INPUTS[input] or input == "back+start" then return true end
  local chord, b = input:match("^(l?t?%+?r?t?)%+(%a+)$")
  return chord ~= nil and (chord == "lt" or chord == "rt" or chord == "lt+rt") and INPUTS[b] ~= nil
end
local function settingName(name) return (name:gsub("^gamepad%.", "")) end

local function no() return false end
local stub = {
  connected = no,
  type = function() return "generic" end,
  down = no, pressed = no, released = no,
  axis = function() return 0 end,
  mode = function() return "game" end,
  set_mode = function() return "game" end,
  cross_set = function() return 0 end,
  cross_slot = function(set, slot) -- LT: bar 1, RT: bar 2, LT+RT: bar 3, buttons 1..8 (nil out of range)
    if type(set) ~= "number" or type(slot) ~= "number" or set < 1 or set > 3 or slot < 1 or slot > 8 then return nil end
    local saved = ui.GetSetting("gamepad.cross." .. set .. "." .. slot)
    local bar, button = (saved or ""):match("^(%d+):(%d+)$")
    if bar then return tonumber(bar), tonumber(button) end
    return set, slot
  end,
  set_cross_slot = function(set, slot, bar, button)
    ui.SetSetting("gamepad.cross." .. set .. "." .. slot, bar .. ":" .. button)
    return true
  end,
  glyph = function(b) return GLYPHS.xbox[b] or tostring(b) end,
  -- "" for a chord with no binding of its own (its cross slot applies), nil for an unknown input
  binding = function(input)
    if not knownInput(input) then return nil end
    local saved = ui.GetSetting("gamepad.bind." .. input)
    if saved and saved ~= "" then return saved end
    return DEFAULT_BINDINGS[input] or ""
  end,
  set_binding = function(input, action)
    if not knownInput(input) then return false end
    ui.SetSetting("gamepad.bind." .. input, action or "") -- "" puts the default back
    return true
  end,
  setting = function(name)
    name = settingName(name)
    if DEFAULT_SETTINGS[name] == nil then return nil end
    return tonumber(ui.GetSetting("gamepad." .. name) or "") or DEFAULT_SETTINGS[name]
  end,
  set_setting = function(name, value)
    name = settingName(name)
    if DEFAULT_SETTINGS[name] == nil then return false end
    if type(value) == "boolean" then value = value and 1 or 0 end
    ui.SetSetting("gamepad." .. name, value)
    return true
  end,
  rumble = function() return false end,
  takes_b = no,
  request_focus = function(window) if ui.SetWindowFocus then ui.SetWindowFocus(window) end end,
}

local MEMBERS = {
  connected = "Connected", type = "Type", down = "Down", pressed = "Pressed", released = "Released",
  axis = "Axis", mode = "Mode", set_mode = "SetMode", cross_set = "CrossSet", cross_slot = "CrossSlot",
  set_cross_slot = "SetCrossSlot", glyph = "Glyph", binding = "Binding", set_binding = "SetBinding",
  setting = "Setting", set_setting = "SetSetting", rumble = "Rumble", request_focus = "RequestFocus",
  drop_cursor = "DropCursor", takes_b = "TakesB",
}
local function api(name)
  local member = MEMBERS[name]
  local f = member and API.Supports("gamepad", member) and API.gamepad[member]
  if f then return f end
  return stub[name]
end

-- pad.connected(), pad.pressed("a"), ...: the real call or the stand-in's.
setmetatable(pad, { __index = function(_, name) return api(name) end })

-- True when the client has hs.gamepad (the C++ side), not the stand-in.
function pad.real() return API.Supports("gamepad", "Connected") end

-- Whether a pad is connected and its type, read once a frame (the client reads the pad as the frame begins); the
-- labels per type (hs.gamepad.glyph answers by the type alone). Read again when hs.gamepad comes or goes.
local seenFrame, seenApi, isConnected, rawType, padKind = nil, nil, false, "generic", "generic"
local labels = {}
local function frameState()
  local f, g = ui.GetFrameCount(), API.Supports("gamepad", "Connected") and API.gamepad or nil
  if f == seenFrame and rawequal(g, seenApi) then return end
  if not rawequal(g, seenApi) then labels = {} end
  seenFrame, seenApi = f, g
  isConnected = api("connected")()
  rawType = api("type")()
  padKind = GLYPHS[rawType] and rawType or "generic"
end
local function connected()
  frameState()
  return isConnected
end

-- A button that went down this frame, only while the pad drives the windows (menu mode): the windows' own
-- shortcuts (B closes, Y details, LB / RB pages) must not fire while the pad plays the game.
function pad.menuPressed(b)
  return connected() and api("mode")() == "menu" and api("pressed")(b)
end

function pad.kind()
  frameState()
  return padKind
end

function pad.label(b)
  frameState()
  if b == nil then return api("glyph")(b) or GLYPHS.xbox[b] or b end
  local byType = labels[rawType]
  if not byType then
    byType = {}
    labels[rawType] = byType
  end
  local l = byType[b]
  if l == nil then
    l = api("glyph")(b) or GLYPHS.xbox[b] or b
    byType[b] = l
  end
  return l
end

-- ---------------------------------------------------------------- ImGui navigation
-- The focused item: ImGui's nav focus while the pad drives the windows (menu mode) or its cursor shows, else the
-- hovered one (the mouse). Never both: the pointer resting over another item while the pad moves the focus would
-- give a second highlight, describe the wrong item and take the window's pad shortcuts (X, Y) to it.
function pad.focused()
  if not ui.IsItemFocused then return ui.IsItemHovered() end
  local padDrives = connected() and api("mode")() == "menu"
  if padDrives or pad.navVisible() then return ui.IsItemFocused() end
  return ui.IsItemHovered()
end

function pad.navVisible()
  local io = ui.GetIO()
  return io.nav_visible == true
end

-- Puts the pad's focus on a window of this pack (menu mode), when one is connected.
function pad.focus(window)
  if connected() then api("request_focus")(window) end
end

-- Focus put on an item from code (ui.SetKeyboardFocusHere) while the pad drives the windows: its cursor shown at once,
-- so the pad's next press moves it (ImGui only shows a hidden cursor on the first press).
function pad.showCursor()
  if ui.SetNavCursorVisible and connected() and api("mode")() == "menu" then ui.SetNavCursorVisible(true) end
end

-- True when ImGui pressed the focused button only because Y went down: ImGui's navigation takes Y as its "input"
-- key (ImGuiKey_NavGamepadInput), which activates a button as A does. The pack's windows give Y its own meaning
-- (details, all, ...), so a button activated with Y held is not a press. `clicked` is the button's result.
function pad.yActivated(clicked)
  return clicked == true and connected() and api("down")("y")
end

return pad
