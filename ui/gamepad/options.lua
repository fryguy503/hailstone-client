-- Options (gamepad pack): stands in for OptionsWindow, so the options key and the EQ menu open it. Pages (LB / RB):
--   Gamepad       the pad's settings (eq.gamepad.setting / set_setting: dead zone, thresholds, camera, rumble)
--   Buttons       what each button does in game mode (eq.gamepad.binding / set_binding), A picks a new action
--   Cross Hotbar  which hot button each cross hotbar slot fires (eq.gamepad.cross_slot / set_cross_slot)
--   Interface, Game, Display, Audio   the client options (eq.Option / eq.SetOption), as the classic Options window
--   Camera        the isometric camera's (the iso_* options, docs/isometric.md)
-- Every row is changed with the D-pad left / right (or A); the mouse clicks a row (right click: back).
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local C = T.C

local OW = "OptionsWindow"

-- ---------------------------------------------------------------- rows
-- A row: { label, value text, change(dir) } where dir is +1 / -1; nil change: read only. `note`: dim text.
local function gamepadRow(label, name, lo, hi, step, fmt)
  local v = pad.setting(name)
  if v == nil then return nil end
  local text
  if fmt == "bool" then text = v ~= 0 and "On" or "Off"
  elseif fmt == "pct" then text = string.format("%d%%", math.floor(v * 100 + 0.5))
  else text = string.format(fmt or "%.2f", v) end
  return { label, text, function(dir)
    if fmt == "bool" then pad.set_setting(name, v == 0 and 1 or 0)
    else pad.set_setting(name, math.max(lo, math.min(hi, v + dir * step))) end
  end }
end

local function optionRow(label, name, choices, values)
  local v = eq.Option(name)
  if v == nil then return nil end
  if choices == "bool" then
    return { label, v ~= 0 and "On" or "Off", function() eq.SetOption(name, v ~= 0 and 0 or 1) end }
  elseif type(choices) == "table" and type(choices[1]) ~= "number" then -- names (a range starts with a number)
    local idx = 1
    for i = 1, #choices do if (values and values[i] or (i - 1)) == v then idx = i end end
    return { label, choices[idx] or tostring(v), function(dir)
      local n = (idx - 1 + dir) % #choices + 1
      eq.SetOption(name, values and values[n] or (n - 1))
    end }
  else -- a number range { lo, hi, step, suffix }
    local r = choices
    return { label, tostring(v) .. (r[4] or ""), function(dir) eq.SetOption(name, math.max(r[1], math.min(r[2], v + dir * r[3]))) end }
  end
end

-- What a binding can be (docs/gamepad.md section 4), with friendly names.
local ACTIONS = {
  { "interact", "Interact / use" }, { "cancel", "Cancel" }, { "AUTOPRIM", "Auto attack" }, { "JUMP", "Jump" },
  { "SIT_STAND", "Sit / stand" }, { "AUTORUN", "Auto run" }, { "DUCK", "Duck" }, { "RUN_WALK", "Run / walk" },
  { "target_prev", "Previous NPC target" }, { "target_next", "Next NPC target" },
  { "target_group", "Target self and group" }, { "TARGETME", "Target yourself" }, { "TOGGLETARGET", "Swap target" },
  { "CYCLEPCTARGETS", "Cycle player targets" }, { "TARGETCORPSE", "Target corpse" }, { "CLEAR_TARGET", "Clear target" },
  { "CONSIDER", "Consider" }, { "HAIL", "Hail" }, { "USE", "Use (doors, objects)" },
  { "hotpage_prev", "Hot bar page back" }, { "hotpage_next", "Hot bar page forward" },
  { "CAST1", "Cast gem 1" }, { "CAST2", "Cast gem 2" }, { "CAST3", "Cast gem 3" }, { "CAST4", "Cast gem 4" },
  { "STOP_CAST", "Stop casting" }, { "INVENTORY", "Inventory" }, { "SPELLBOOK", "Spell book" },
  { "menu", "Main menu" }, { "cursor", "Cursor mode" }, { "camera_toggle", "First person / behind" },
  { "camera_iso", "Isometric camera" },
  { "chat", "Chat keyboard" }, { "none", "Nothing" },
}
local ACTION_NAME = {}
for _, a in ipairs(ACTIONS) do ACTION_NAME[a[1]] = a[2] end
local function actionName(a)
  if a == nil then return "-" end
  if a == "" then return "Cross hotbar slot" end
  local bare = a:gsub("^CMD_", "")
  if ACTION_NAME[a] then return ACTION_NAME[a] end
  if ACTION_NAME[bare] then return ACTION_NAME[bare] end
  local bar, button = a:match("^hot:(%d+):(%d+)$")
  if bar then return "Hot bar " .. bar .. ", button " .. button end
  return a
end

local INPUTS = { "a", "b", "x", "y", "lb", "rb", "up", "down", "left", "right", "ls", "rs", "back", "start", "back+start" }
local SLOT_BUTTONS = { "left", "up", "right", "down", "x", "y", "b", "a" }
local SETS = { "LT", "RT", "LT+RT" }

local page = 1
local lists = {}
local picking = nil -- the input whose new action is being chosen

-- The pad's focus goes to row `want` of a list (a page just shown, the action list just opened, the row whose action
-- was just chosen): the row ImGui had focused is gone then, and the pad would have nothing focused.
local function aim(state, count, visible)
  if not state.want then return end
  local i = math.max(1, math.min(state.want, math.max(1, count)))
  if i <= state.offset or i > state.offset + visible then state.offset = math.max(0, math.min(i - 1, count - visible)) end
  state.focusNext = i - state.offset
  state.want = nil
end

local function showPage(p)
  page = p
  picking = nil
  lists[p] = lists[p] or { offset = 0 }
  lists[p].want = lists[p].last or 1
end

local function inputLabel(input)
  local parts = {}
  for b in input:gmatch("[^+]+") do parts[#parts + 1] = pad.label(b) end
  return table.concat(parts, " + ")
end

local PAGES = {
  { "Gamepad", function()
    local rows = {}
    local function add(r) if r then rows[#rows + 1] = r end end
    add(gamepadRow("Gamepad", "enabled", 0, 1, 1, "bool"))
    add(gamepadRow("Stick dead zone", "deadzone", 0, 0.6, 0.05, "pct"))
    add(gamepadRow("Trigger press point", "trigger_threshold", 0.1, 0.9, 0.05, "pct"))
    add(gamepadRow("Walk forward / back past", "move_threshold", 0.1, 0.9, 0.05, "pct"))
    add(gamepadRow("Strafe past", "strafe_threshold", 0.1, 0.9, 0.05, "pct"))
    add(gamepadRow("Camera speed", "camera_speed", 0.5, 8, 0.25, "%.2f"))
    add(gamepadRow("Invert camera up / down", "invert_y", 0, 1, 1, "bool"))
    add(gamepadRow("Cursor speed", "cursor_speed", 200, 3000, 100, "%.0f"))
    add(gamepadRow("Rumble", "rumble", 0, 1, 1, "bool"))
    local ok, name = pad.connected()
    rows[#rows + 1] = { "Connected pad", ok and ((name and name ~= "") and name or "Yes") or "None" }
    return rows
  end },
  { "Buttons", function()
    local rows = {}
    for _, input in ipairs(INPUTS) do
      local a = pad.binding(input)
      if a ~= nil then rows[#rows + 1] = { inputLabel(input), actionName(a), nil, pick = input } end
    end
    return rows
  end },
  { "Cross Hotbar", function()
    local rows = {}
    for s = 1, 3 do
      for n = 1, 8 do
        local bar, button = pad.cross_slot(s, n)
        if bar then
          local label = SETS[s] .. " + " .. pad.label(SLOT_BUTTONS[n])
          local kind, hb = eq.HotButton(bar == 1 and "HotButtonWnd" or ("HotButtonWnd" .. bar), "HB_Button" .. button)
          local what = (hb and hb ~= "") and ("  (" .. hb .. ")") or (kind == "empty" and "  (empty)" or "")
          rows[#rows + 1] = { label, "Bar " .. bar .. ", button " .. button .. what, function(dir)
            -- left / right: the button (1..12), wrapping into the next bar
            local idx = ((bar - 1) * 12 + (button - 1) + dir) % 120
            if pad.set_cross_slot then pad.set_cross_slot(s, n, idx // 12 + 1, idx % 12 + 1) end
          end }
        end
      end
    end
    return rows
  end },
  { "Interface", function()
    local rows = {}
    local trail = optionRow("Find trail", "find_trail_style", { "Gold line (default)", "Rainbow trail" })
    if trail then rows[#rows + 1] = trail end
    local uiName = eq.Option("ui_name")
    if uiName ~= nil then
      local names = eq.OptionChoices("ui_name")
      local r = optionRow("User interface", "ui_name", names)
      if r then rows[#rows + 1] = r end
      local now = eq.Option("ui_name_now")
      if now ~= nil and now ~= uiName then rows[#rows + 1] = { "", "Log in again to switch", nil } end
    end
    do -- this pack's colour palettes (lib/palettes.lua): left / right picks the next one, shown at once
      local list, idx = T.palettes.list, 1
      for i, p in ipairs(list) do if p.id == T.palette then idx = i end end
      rows[#rows + 1] = { "Colour palette", list[idx].name, function(dir)
        T.setPalette(list[(idx - 1 + dir) % #list + 1].id)
      end }
    end
    -- The map pack: the map's maps directory (MVW_MapsDirCombobox, "Main" "MapSkin" in the character's UI ini).
    local packs, pack = eq.ComboInfo("MapViewWnd", "MVW_MapsDirCombobox")
    if packs and #packs > 0 then
      rows[#rows + 1] = { "Map pack", packs[pack + 1] or packs[1], function(dir)
        eq.ComboSelect("MapViewWnd", "MVW_MapsDirCombobox", (pack + dir) % #packs)
      end }
    end
    local r = optionRow("UI scale", "ui_scale", { "Automatic", "75 %", "100 %", "125 %", "150 %", "175 %", "200 %", "250 %" },
      { 0, 75, 100, 125, 150, 175, 200, 250 })
    if r then rows[#rows + 1] = r end
    for _, o in ipairs({ { "Player names", "pc_names" }, { "My name", "own_name" }, { "NPC names", "npc_names" },
        { "Con-coloured NPC names", "con_name_colour" }, { "Highlight target name", "target_plate_highlight" }, { "Target name through walls", "target_plate_through_walls" }, { "Fade overlapping names", "plate_overlap_fade" }, { "Target ring", "show_target_ring" },
        { "Target ring cues", "target_ring_cues" }, { "Target ring through models", "target_ring_xray" },
        { "Target skips pets", "tab_skip_pets" }, { "Target needs sight", "tab_line_of_sight" }, { "Gem replaces spell", "replace_gem_spell" }, { "Buff timers", "show_buff_timer" }, { "Song timers", "show_song_timer" },
        { "Compare items with what you wear", "item_compare" }, { "Upgrade / downgrade verdict", "item_compare_verdict" },
        { "Context menus", "context_menus" }, { "Player right-click menu", "player_menu" }, { "More hot button states", "hotbutton_extras" },
        { "Offer layouts to new characters", "offer_layout_copy" } }) do
      local x = optionRow(o[1], o[2], "bool")
      if x then rows[#rows + 1] = x end
    end
    -- The name plates' text (game/nameplate_style.h).
    local faces = eq.OptionChoices("plate_font_face")
    if faces and #faces > 0 then
      local x = optionRow("Name font", "plate_font_face", faces)
      if x then rows[#rows + 1] = x end
    end
    for _, o in ipairs({ { "Name size", "plate_font_size", { 8, 48, 1, " pt" } }, { "Bold names", "plate_font_bold", "bool" },
        { "Name edge", "plate_edge", { "Shadow", "Outline", "None" } },
        { "Name size at distance", "plate_far_scale", { 25, 100, 5, " %" } },
        { "Guild and owner lines", "plate_tag_scale", { 50, 100, 5, " %" } } }) do
      local x = optionRow(o[1], o[2], o[3])
      if x then rows[#rows + 1] = x end
    end
    for _, o in ipairs({ { "Target ring opacity", "target_ring_opacity", { 25, 200, 25, " %" } },
        { "Target ring glow", "target_ring_glow", { 0, 100, 10, " %" } } }) do
      local x = optionRow(o[1], o[2], o[3])
      if x then rows[#rows + 1] = x end
    end
    -- The ring's Max Size (game::cap_ring_radius): 4 .. 40 units, or Off (0), the original's size.
    local sizeNames, sizeValues = {}, {}
    for v = 4, 40 do sizeNames[#sizeNames + 1] = tostring(v); sizeValues[#sizeValues + 1] = v end
    sizeNames[#sizeNames + 1] = "Off"; sizeValues[#sizeValues + 1] = 0
    local x = optionRow("Target ring max size", "target_ring_max_size", sizeNames, sizeValues)
    if x then rows[#rows + 1] = x end
    return rows
  end },
  { "Game", function()
    local rows = {}
    for _, o in ipairs({ { "Anonymous", "anonymous" }, { "Roleplaying", "roleplaying" }, { "Show my helm", "show_helm" },
        { "Autosplit coin", "loot_autosplit" }, { "Guild invites", "guild_invites" }, { "Confirm Loot All", "loot_all_confirm" },
        { "Fast item destroy", "fast_item_destroy" }, { "Pet window pops up", "pet_window_popup" },
        { "Auto-duck", "allow_auto_duck" }, { "Auto-stand", "allow_auto_stand" }, { "Ride while illusioned", "illusion_mounts" },
        { "Proc cast animations", "cast_animation_fallback" },
        { "Use tell windows", "use_tell_windows" },
        { "Attack until XTarget clear", "attack_until_xtarget_clear" },
        { "Auto target XTarget attackers", "auto_target_xtarget" }, { "Face target on attack", "face_on_attack" },
        { "Fast camp", "fast_camp" } }) do
      local x = optionRow(o[1], o[2], "bool")
      if x then rows[#rows + 1] = x end
    end
    local x = optionRow("Loot No Drop items", "loot_no_drop", { "Always", "Not usable", "Never" })
    if x then rows[#rows + 1] = x end
    return rows
  end },
  { "Display", function()
    local rows = {}
    local list = {
      optionRow("Display mode", "window_mode", { "Windowed", "Borderless", "Fullscreen" }),
      optionRow("Vertical sync", "vsync", { "Off", "On", "Adaptive" }),
      optionRow("Shadows", "shadows", { "Off", "Low", "Medium", "High" }),
      optionRow("Anti-aliasing", "msaa", { "Off", "2x", "4x", "8x" }),
      optionRow("Render scale", "render_scale", { 50, 100, 5, " %" }),
      optionRow("Ambient occlusion", "ao", "bool"),
      optionRow("Grass", "grass", "bool"),
      optionRow("Grass distance", "flora_distance", { 400, 1200, 100, " units" }),
      optionRow("Tree detail distance", "tree_detail", { 50, 200, 10, " %" }),
      optionRow("Lighting", "lighting", { "Modern", "Classic" }),
      optionRow("Max camera zoom", "max_camera_distance", { 10, 150, 10, " units" }),
      optionRow("Ground floating NPCs", "ground_npcs", "bool"),
      -- the texture pack (display.texture_folder, from the next start): Off, then each folder of textures/
      optionRow("Texture pack (next start)", "texture_pack", eq.OptionChoices("texture_pack")),
    }
    for i = 1, 12 do if list[i] then rows[#rows + 1] = list[i] end end
    return rows
  end },
  { "Camera", function()
    -- Hailstone's isometric camera (docs/isometric.md): the iso_* options.
    local rows = {}
    local list = {
      optionRow("Isometric camera", "iso_camera", "bool"),
      optionRow("Pitch", "iso_pitch", { 20, 89, 1, "°" }),
      optionRow("Field of view", "iso_fov", { 10, 90, 1, "°" }),
      optionRow("Movement", "iso_wasd", { "Relative to the camera", "Classic" }),
      optionRow("See-through circle", "iso_reveal", { 0, 60, 2, " units" }),
      optionRow("See-through always", "iso_reveal_always", "bool"),
      optionRow("Lift roofs and ceilings", "iso_roof", "bool"),
      optionRow("Silhouette behind walls", "iso_silhouette", "bool"),
      optionRow("Click marker", "iso_marker", "bool"),
      optionRow("Highlight under cursor", "iso_hover", "bool"),
    }
    for i = 1, 12 do if list[i] then rows[#rows + 1] = list[i] end end
    return rows
  end },
  { "Audio", function()
    local rows = {}
    local list = {
      optionRow("Mute", "mute", "bool"),
      optionRow("Master volume", "master", { 0, 100, 5, " %" }),
      optionRow("Music", "music", { 0, 100, 5, " %" }),
      optionRow("Sound effects", "effects", { 0, 100, 5, " %" }),
      optionRow("NPC hit sounds", "npc_hits", { 0, 100, 5, " %" }),
      optionRow("Environment sounds", "environment", "bool"),
      optionRow("Combat music", "combat_music", "bool"),
    }
    for i = 1, 7 do if list[i] then rows[#rows + 1] = list[i] end end
    return rows
  end },
  -- The plugins' options (eq.PluginOptions; docs/plugins.md "Plugin windows"): a switch, or left / right over its range.
  { "Plugins", function()
    local rows = {}
    for _, o in ipairs(eq.PluginOptions()) do
      local label = o.plugin .. ": " .. o.label
      if o.max == 1 and o.min == 0 then
        rows[#rows + 1] = { label, o.value ~= 0 and "On" or "Off", function()
          eq.SetPluginOption(o.owner, o.name, o.value ~= 0 and 0 or 1)
        end }
      else
        rows[#rows + 1] = { label, tostring(o.value), function(dir)
          eq.SetPluginOption(o.owner, o.name, math.max(o.min, math.min(o.max, o.value + dir)))
        end }
      end
    end
    if #rows == 0 then rows[1] = { "No plugin has options", "", function() end } end
    return rows
  end },
}

local function pageTabs(x0, y0, x1)
  T.font(19)
  local x = x0
  local _, th = ui.CalcTextSize("Ag")
  local h = th + T.px(16)
  local gw = T.glyphWidth(pad, "lb", T.px(28))
  T.glyph(pad, "lb", x + gw / 2, y0 + h / 2, T.px(28))
  x = x + gw + T.px(10)
  for i, p in ipairs(PAGES) do
    local tw = ui.CalcTextSize(p[1])
    local w = tw + T.px(28)
    T.rect(x, y0, x + w, y0 + h, page == i and C.accent or C.raised, page == i and 0.9 or 1, T.px(10))
    T.textIn(x, y0, x + w, y0 + h, p[1], page == i and C.onAccent or C.text, 0.5)
    if ui.IsMouseHoveringRect(x, y0, x + w, y0 + h) and ui.IsMouseClicked(ui.Mouse.Left) then showPage(i) end
    x = x + w + T.px(8)
  end
  local gw2 = T.glyphWidth(pad, "rb", T.px(28))
  T.glyph(pad, "rb", x + gw2 / 2, y0 + h / 2, T.px(28))
  T.unfont()
  return y0 + h + T.px(18)
end

ui.window("Options", function()
  if not W.shown(OW, true) then
    picking = nil
    return
  end
  local hints = picking and { { "a", "Choose" }, { "b", "Back" } }
      or page == 2 and { { "a", "Choose action" }, { "lb", "Page" }, { "rb", "Page" }, { "b", "Close" } }
      or { { "left", "Less" }, { "right", "More" }, { "a", "Change" }, { "lb", "Page" }, { "rb", "Page" }, { "b", "Close" } }
  local back = false
  if W.begin("Options", { w = 1300, h = 940, title = picking and ("Button " .. inputLabel(picking)) or "Options", hints = hints }) then
    local x0, y0, x1, y1, cw = W.area()
    local wx, wy = ui.GetWindowPos()
    if picking then
      lists.pick = lists.pick or { offset = 0 }
      local visible = math.max(1, math.floor((y1 - y0 + T.px(10)) / T.px(66)))
      local current = pad.binding(picking)
      if lists.pick.want == true then -- opened: the focus on the action the button has now
        lists.pick.want = 1
        for i, a in ipairs(ACTIONS) do
          if current == a[1] or current:gsub("^CMD_", "") == a[1] then lists.pick.want = i + 1 end
        end
      end
      aim(lists.pick, #ACTIONS + 1, visible)
      W.list("pick", lists.pick, #ACTIONS + 1, visible, 56, cw, function(i, rx0, ry0, rx1, ry1)
        local label = i == 1 and "Default" or ACTIONS[i - 1][2]
        local on = i > 1 and (current == ACTIONS[i - 1][1] or current:gsub("^CMD_", "") == ACTIONS[i - 1][1])
        T.textIn(rx0 + T.px(18), ry0, rx1 - T.px(18), ry1, label, on and C.accent or C.text, 0)
      end, function(i)
        pad.set_binding(picking, i == 1 and "" or ACTIONS[i - 1][1])
        picking = nil
        lists[page].want = lists[page].last
      end)
      if W.pressed("b") then back = true end
    else
      if W.pressed("lb") then showPage((page - 2) % #PAGES + 1) end
      if W.pressed("rb") then showPage(page % #PAGES + 1) end
      local top = pageTabs(x0, y0, x1)
      ui.SetCursorPos(x0 - wx, top - wy)
      local rows = PAGES[page][2]()
      lists[page] = lists[page] or { offset = 0 }
      local visible = math.max(1, math.floor((y1 - top + T.px(10)) / T.px(66)))
      aim(lists[page], #rows, visible)
      local focusedRow = W.list("page" .. page, lists[page], #rows, visible, 56, cw, function(i, rx0, ry0, rx1, ry1, focused)
        local r = rows[i]
        T.textIn(rx0 + T.px(18), ry0, rx0 + cw * 0.5, ry1, r[1], C.text, 0)
        local vx1 = rx1 - T.px(18)
        if r[3] then -- arrows either side of the value
          T.triangle("right", vx1 - T.px(6), (ry0 + ry1) / 2, T.px(7), focused and C.accent or C.dim, 1)
          vx1 = vx1 - T.px(26)
        end
        local tw = T.textIn(rx0 + cw * 0.5, ry0, vx1, ry1, T.fit(r[2], vx1 - rx0 - cw * 0.5), r[3] and C.text or C.dim, 1)
        if r[3] then T.triangle("left", vx1 - tw - T.px(16), (ry0 + ry1) / 2, T.px(7), focused and C.accent or C.dim, 1) end
        if focused and r[3] then
          if pad.menuPressed("left") then r[3](-1) end
          if pad.menuPressed("right") then r[3](1) end
        end
        if ui.IsItemClicked(ui.Mouse.Right) and r[3] then r[3](-1) end
      end, function(i)
        local r = rows[i]
        if r.pick then picking = r.pick lists.pick = { offset = 0, want = true }
        elseif r[3] then r[3](1) end
      end)
      if focusedRow then lists[page].last = focusedRow end
    end
  end
  local closed = W.finish()
  if picking and (back or closed) then
    picking = nil
    lists[page].want = lists[page].last -- back on the button's row
  elseif closed then
    eq.ShowWindow(OW, false)
  end
end)
