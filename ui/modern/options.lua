-- Options: this client's own settings window, in place of the game's OptionsWindow. A plain tabbed window
-- (General, Display, Graphics, Audio, Controls, Chat, Colors) that moves and resizes by its edges, in this UI's style
-- (lib/style.lua) and on an opaque background: the window colour alone let a bright scene and the windows behind
-- show through its rows of settings. Every game option of the original's General, Display, Mouse and Mail pages is
-- here, under the stock labels and tooltips; the original's Voice and Share pages (its voice chat, Facebook, Twitter
-- and EQPlayers services) are left out.
--
-- Every setting is a client option by name: eq.Option(name) reads it (nil when the client does not have it, and its
-- row is left out), eq.SetOption(name, value) applies and saves it (hailstone.ini; app/client_app_options.cpp).
-- The key bindings, chat filters and user colours come from the original window's lists (app/lua_eq_dialogs.cpp
-- OptionsWindow).
--
-- For contrast (WCAG AA, lib/palettes.lua checks): the section headings in the palette's heading colour, and every
-- field, check box, combo box, slider and button outlined in its frameBorder colour (the window's own edge keeps the
-- hairline), so the controls stand out from the window and not only their text does.
local style = dofile("lib/style.lua")
local T = dofile("lib/theme.lua")
local snap = dofile("lib/snap.lua")
local esc = dofile("lib/escape.lua")
local C = T.color
local WINDOW = "OptionsWindow"

-- ---------------------------------------------------------------- helpers
-- A section heading, in the palette's heading colour.
local function heading(text)
  local h = C.heading
  ui.PushStyleColor(ui.Col.Text, h[1], h[2], h[3], h[4])
  ui.SeparatorText(text)
  ui.PopStyleColor()
end
local LABEL_W = 170 -- the label column, in unscaled pixels
local U = 1         -- the UI scale, this frame

local function tip(text)
  if text and ui.IsItemHovered(ui.HoveredFlags.DelayShort) then ui.SetTooltip(text) end
end

local function row(label, tooltip, draw)
  ui.TableNextRow()
  ui.TableNextColumn()
  ui.Text(label)
  tip(tooltip)
  ui.TableNextColumn()
  ui.SetNextItemWidth(-1)
  draw()
  tip(tooltip)
end

-- A note under the row above, in the value column.
local function note(text)
  ui.TableNextRow()
  ui.TableNextColumn()
  ui.TableNextColumn()
  ui.TextDisabled(text)
end

local function section(title, body)
  heading(title)
  if ui.BeginTable("##" .. title, 2, ui.TableFlags.SizingFixedFit) then
    ui.TableSetupColumn("label", ui.TableColumnFlags.WidthFixed, LABEL_W * U)
    ui.TableSetupColumn("value", ui.TableColumnFlags.WidthStretch, 0)
    body()
    ui.EndTable()
  end
  ui.Spacing()
end

-- A combo box over choices (1-based here, the option's values 0-based); `values` maps a choice to the option's value.
local function combo(name, label, choices, tooltip, values)
  local v = eq.Option(name)
  if v == nil then return end
  row(label, tooltip, function()
    local current = v
    if values then
      current = -1
      for i, x in ipairs(values) do if x == v then current = i - 1 end end
    end
    local preview = choices[current + 1] or tostring(v)
    if ui.BeginCombo("##" .. name, preview) then
      for i, c in ipairs(choices) do
        if ui.Selectable(c .. "##" .. i, i - 1 == current) then
          eq.SetOption(name, values and values[i] or (i - 1))
        end
      end
      ui.EndCombo()
    end
  end)
end

local function check(name, label, tooltip)
  local v = eq.Option(name)
  if v == nil then return end
  row(label, tooltip, function()
    local on, changed = ui.Checkbox("##" .. name, v ~= 0)
    if changed then eq.SetOption(name, on and 1 or 0) end
  end)
end

-- A check box shown inverted: checked while the option is 0 (SyncGeneralPage's TradeskillWnd CtrlBypass box).
local function invcheck(name, label, tooltip)
  local v = eq.Option(name)
  if v == nil then return end
  row(label, tooltip, function()
    local on, changed = ui.Checkbox("##" .. name, v == 0)
    if changed then eq.SetOption(name, on and 0 or 1) end
  end)
end

-- A slider over the option's own values (min..max); text(v) is what it shows.
local function slider(name, label, min, max, text, tooltip)
  local v = eq.Option(name)
  if v == nil then return end
  row(label, tooltip, function()
    local nv, changed = ui.SliderInt("##" .. name, v, min, max, text and text(v) or nil)
    if changed then eq.SetOption(name, nv) end
  end)
end

-- The graphics sliders of 21 ticks (0..20) over each setting's range (app/client_app_zone.cpp kSliders).
local function ticks(name, label, text, tooltip) slider(name, label, 0, 20, text, tooltip) end

local function pct(v) return string.format("%d %%", v) end

-- ---------------------------------------------------------------- General
-- The original's General page (and its Mail page): game options, kept per character (options.ini, over eqclient.ini).

-- One bit of an option (the click-through mask).
local function bit(name, mask, label, tooltip)
  local v = eq.Option(name)
  if v == nil then return end
  row(label, tooltip, function()
    local on, changed = ui.Checkbox("##" .. name .. mask, v & mask ~= 0)
    if changed then eq.SetOption(name, on and (v | mask) or (v & ~mask)) end
  end)
end

local function generalTab()
  section("Character", function()
    check("anonymous", "Anonymous", "Hides your class, level and zone from /who and your guild's roster.")
    check("roleplaying", "Roleplaying", "Shows others that you are roleplaying.")
    check("show_helm", "Show My Helm", "Toggle whether your helm is visible or not.")
    check("fast_camp", "Fast Camp",
      "Hailstone: camping logs out at once instead of after 30 seconds. On most servers your character then stays in the zone, link dead, for about 90 seconds.")
    check("loot_autosplit", "Autosplit", "Splits the coin you loot with your group.")
  end)
  section("Combat", function()
    check("attack_until_xtarget_clear", "Attack Until XTarget Is Clear",
      "Hailstone: when your target dies, auto attack stays on while your extended target list still holds a spawn, and goes off once it is clear.")
    check("auto_target_xtarget", "Auto Target XTarget Attackers",
      "Hailstone: when you have no target, an NPC on your extended target list that attacks you becomes your target, as does one still attacking you when your target dies.")
    check("face_on_attack", "Face Target On Attack",
      "Hailstone: on, the attack key and the attack buttons turn you to face your target when they turn auto attack on, as the original client does. Off: you keep facing where you were. /attack never turns you.")
  end)
  section("Social", function()
    check("guild_invites", "Receive Guild Invites", "Off: guild invitations are declined for you.")
    combo("trade_with", "Allow Trading With", { "Everyone", "Party members only", "No one" },
      "Who can open a trade with you.", { 2, 1, 0 })
    check("raid_invite_confirm", "Confirm Raid Invites",
      "Toggle whether you want to receive a raid invite confirmation window.")
    check("auto_consent_group", "Auto Consent Group",
      "Toggle whether to automatically give your group consent to move your corpse.")
    check("auto_consent_raid", "Auto Consent Raid",
      "Toggle whether to automatically give your raid consent to move your corpse.")
    check("auto_consent_guild", "Auto Consent Guild",
      "Toggle whether to automatically give your guild consent to move your corpse.")
    check("autojoin_help", "Join General Channels", "Toggle whether to autojoin general help chat channels.")
    check("use_tell_windows", "Use Tell Windows",
      "Toggle whether you want tells to appear in their own separate chat windows.")
    check("accept_kick_requests", "Accept Kick Requests",
      "If checked, you will automatically accept any kick requests made by another player logging into your account.")
  end)
  section("Items", function()
    check("fast_item_destroy", "Fast Item Destroy",
      "Toggle whether you want to destroy items without a confirmation window.")
    check("loot_all_confirm", "Confirm Loot All", "Toggle whether you want to loot all with a confirmation window.")
    combo("fast_drop", "Fast Drop", { "With confirmation", "Without confirmation", "Never" },
      "Dropping an item on the ground: whether it asks first, or never drops it.", { 0, 1, -1 })
    combo("loot_no_drop", "Loot No Drop Items", { "Always", "Not Usable", "Never" },
      "When to ask before looting a no-drop item.")
    -- CtrlBypass 1 (the default, the box clear): a right click opens a tradeskill container in the tradeskill
    -- window, ctrl+right click in its own; 0 (checked) swaps them.
    invcheck("tradeskill_ctrl_click", "Open Tradeskill List with Ctrl Right Click",
      "Open Tradeskill List with Ctrl Right Click.")
  end)
  section("Purchases", function()
    check("aa_no_confirm", "Fast AA Purchase",
      "Toggle whether you want to spend AA points without a confirmation window.")
    check("point_merchant_no_confirm", "Fast Adv Purchase",
      "Toggle whether you want to spend Adventure Points without a confirmation window.")
    check("leadership_no_confirm", "Fast LA Purchase",
      "Toggle whether you want to spend leadership points without a confirmation window.")
  end)
  section("Mercenaries and Offline Mode", function()
    check("dismiss_merc_confirm", "Confirm Dismiss Mercenary",
      "Toggle whether you want to dismiss your mercenary with a confirmation window.")
    check("offline_mode_confirm", "Confirm Offline Mode",
      "Toggle whether you want to receive a confirmation window before entering Offline Mode.")
  end)
  section("Interface", function()
    check("context_menus", "Allow Context Menus", "Right-clicking a window opens its menu.")
    check("player_menu", "Player Right-Click Menu",
      "Hailstone: right-clicking another player opens a menu (Send Tell, Invite to Group, Invite to Raid, Trade, Inspect) instead of inspecting them. Needs context menus allowed.")
    -- menus.lua's menus.scale (this pack's look: the size its context menus draw at; Ctrl+wheel over a menu too)
    row("Menu Size", "The size of right-click menus and their text. Ctrl+mouse wheel over a menu changes it too.", function()
      local v = math.floor((tonumber(ui.GetSetting("menus.scale")) or 1.25) * 100 + 0.5)
      local nv, changed = ui.SliderInt("##menus.scale", v, 75, 200, v .. " %%")
      if changed then ui.SetSetting("menus.scale", string.format("%.2f", nv / 100)) end
    end)
    check("pet_window_popup", "Pet Window Auto-Popup", "The pet window opens when you get a pet.")
    check("target_click_help", "Target Click Shows Help",
      "Show help text when you left click on a target object or player.")
    check("pet_owner_names", "Show Pet Owner Names", "Show/hide pet owner names under pet name.")
  end)
  section("Movement and Clicking", function()
    check("allow_auto_duck", "Allow Auto-duck",
      "Toggle whether you will let the game make you duck to fit into small spaces.")
    check("allow_auto_stand", "Allow Auto-stand",
      "Toggle whether moving forward automatically makes your character stand up if sitting.")
    check("illusion_mounts", "Ride While Illusioned",
      "Hailstone: an illusion does not stop you summoning and riding a mount. Off: as in the original, many illusions refuse the mount.")
    check("cast_animation_fallback", "Proc Cast Animations",
      "Hailstone: a spell that lands without a cast (a weapon or item proc) and has no cast animation of its own plays the heal or the nuke cast animation, by whether it is beneficial. Off: nothing plays for such a spell.")
    bit("click_through", 1, "Click Through Self", "Click through yourself.")
    bit("click_through", 2, "Click Through Pets", "Click through pets.")
    bit("click_through", 4, "Click Through Mercs", "Click through mercenaries.")
  end)
  section("Mail", function()
    check("mail_forwarding", "Mail Forwarding", "Forward mail to the address registered in my Station Account.")
    check("new_mail_notify", "New Mail in Chat", "Notify me in the chat window when new mail arrives.")
    check("new_mail_sound", "New Mail Sound", "Play a sound when new mail arrives.")
    check("address_book_confirm_delete", "Confirm Address Book Deletion",
      "Ask for confirmation before deleting an address book entry.")
  end)
end

-- ---------------------------------------------------------------- Display
local FPS = { 0, 30, 60, 75, 90, 120, 144, 165, 240 }
local FPS_NAMES = { "Unlimited", "30", "60", "75", "90", "120", "144", "165", "240" }
local BG_FPS = { 0, -1, 1, 5, 10, 15, 30, 60 }
local BG_FPS_NAMES = { "Same as above", "0 (draw nothing)", "1", "5", "10", "15", "30", "60" }
local SCALES = { 0, 75, 100, 125, 150, 175, 200, 250 }
local SCALE_NAMES = { "Automatic (display)", "75 %", "100 %", "125 %", "150 %", "175 %", "200 %", "250 %" }

-- The target ring's con sections (game::kTargetIndicatorSections 0..6): the row's label and the tooltip's con.
local RING_CONS = {
  { "Gray Con", "gray (trivial)" }, { "Green Con", "green" }, { "Light Blue Con", "light blue" },
  { "Dark Blue Con", "dark blue" }, { "White Con", "white (even)" }, { "Yellow Con", "yellow" }, { "Red Con", "red" },
}

local function displayTab()
  section("Window", function()
    combo("window_mode", "Display Mode", { "Windowed", "Borderless Fullscreen", "Fullscreen" },
      "Windowed: a normal window. Borderless: a window covering the whole screen at the desktop's resolution "
      .. "(quick to switch away from). Fullscreen: exclusive, at the resolution below.")
    local mode = eq.Option("window_mode") or 0
    local res = eq.Option("resolution")
    if res ~= nil then
      row("Resolution", mode == 1 and "Borderless fullscreen always uses the desktop's resolution."
        or "The window's size (windowed) or the screen mode (fullscreen).", function()
        local choices = eq.OptionChoices("resolution")
        ui.BeginDisabled(mode == 1)
        if ui.BeginCombo("##resolution", choices[res + 1] or "Custom", ui.ComboFlags.HeightLarge) then
          for i, c in ipairs(choices) do
            if ui.Selectable(c .. "##r" .. i, i - 1 == res) then eq.SetOption("resolution", i - 1) end
          end
          ui.EndCombo()
        end
        ui.EndDisabled()
      end)
    end
    combo("vsync", "Vertical Sync", { "Off", "On", "Adaptive" },
      "On: no tearing, the frame rate follows the monitor. Off: lowest latency, may tear. "
      .. "Adaptive: syncs, but tears instead of stuttering when a frame is late (where the driver supports it).")
    combo("max_fps", "Frame Rate Limit", FPS_NAMES, "The most frames a second the client draws.", FPS)
    combo("background_fps", "Background Frame Rate", BG_FPS_NAMES,
      "The frame limit while another window has the focus: saves power and heat with the game in the background. "
      .. "At 0 nothing is drawn; the game, chat and scripts keep running.",
      BG_FPS)
  end)
  section("View", function()
    local fov = eq.Option("fov")
    if fov ~= nil then
      row("Field of View", "Degrees across the screen. The original's is 63.", function()
        local w = ui.GetContentRegionAvail()
        ui.SetNextItemWidth(w - 70 * U)
        local nv, changed = ui.SliderInt("##fov", fov, 45, 120, fov .. "°")
        if changed then eq.SetOption("fov", nv) end
        ui.SameLine()
        if ui.Button("Default##fov", 62 * U, 0) then eq.SetOption("fov", -1) end
      end)
    end
  end)
  -- Hailstone's isometric camera (docs/isometric.md): the iso_* options, also /camera.
  section("Isometric Camera", function()
    local deg = function(v) return v .. "°" end
    check("iso_camera", "Isometric Camera",
      "Look down on your character from above and to one side, and click to move. /camera iso and /camera behind "
      .. "switch too; a hot button with /camera toggle gives it a key.")
    slider("iso_pitch", "Pitch", 20, 89, deg, "How steeply the camera looks down.")
    slider("iso_fov", "Field of View", 10, 90, deg, "Degrees across the screen. Narrower looks flatter, more isometric.")
    combo("iso_wasd", "Movement Keys", { "Relative to the camera", "Classic" },
      "Relative to the camera: W, A, S and D move up, left, down and right on the screen and the turn keys turn the "
      .. "camera. Classic: as in the other cameras.")
    slider("iso_reveal", "See-Through Circle", 0, 60, function(v) return v == 0 and "Off" or (v .. " units") end,
      "Walls, trees and hills between the camera and your character are cut away in a circle this size round you.")
    check("iso_reveal_always", "See-Through Always",
      "Cut the circle even when nothing is in the way. Off: only while something hides your character.")
    check("iso_roof", "Lift Roofs and Ceilings",
      "Under a roof or a ceiling, everything above your head round you is taken away, so rooms and caves show "
      .. "from above.")
    check("iso_silhouette", "Silhouette Behind Walls", "Your character's outline shows where something still hides them.")
    check("iso_marker", "Click Marker", "A ring on the ground where a click is taking you.")
    check("iso_hover", "Highlight Under Cursor", "A ring under the character, door or item the mouse is over.")
  end)
  section("Interface", function()
    local uiName = eq.Option("ui_name")
    if uiName ~= nil then
      local names, info = eq.OptionChoices("ui_name"), eq.OptionChoices("ui_name_info")
      local function infoOf(i)
        local d, input = (info[i] or ""):match("^(.-)\n(.*)$")
        return (d or "") .. ((input and input ~= "") and ("\nMade for: " .. input) or "")
      end
      row("User Interface", "The look and layout of every window, kept for this server. Takes effect at the next login.", function()
        if ui.BeginCombo("##ui_name", names[uiName + 1] or "?") then
          for i, n in ipairs(names) do
            if ui.Selectable(n .. "##ui" .. i, i - 1 == uiName) then eq.SetOption("ui_name", i - 1) end
            tip(infoOf(i))
          end
          ui.EndCombo()
        end
      end)
      local now = eq.Option("ui_name_now")
      if now ~= nil and now ~= uiName then
        note("Log in again to switch to " .. (names[uiName + 1] or "it") .. " (now " .. (names[now + 1] or "?") .. ").")
      end
    end
    -- The running UI's colour palettes (the global ui_palette its theme sets up: classic's palette.lua, modern's
    -- lib/theme.lua); a pick shows at once and is kept in the UI's own settings.
    if ui_palette then
      local list, current = ui_palette.list(), ui_palette.current()
      local shown = list[1] and list[1].name or "?"
      for _, p in ipairs(list) do if p.id == current then shown = p.name end end
      row("Colour Palette", "The colours of every window of this interface. Changes at once.", function()
        if ui.BeginCombo("##ui_palette", shown) then
          for _, p in ipairs(list) do
            if ui.Selectable(p.name .. "##pal" .. p.id, p.id == current) then ui_palette.set(p.id) end
            tip(p.description)
          end
          ui.EndCombo()
        end
      end)
    end
    -- The map pack: the map window's maps directory (its MVW_MapsDirCombobox, "Main" "MapSkin" in the character's
    -- UI ini), the subfolders of maps/ (docs/layout.md "Maps"). The map, minimap and zone map change at once.
    local packs, pack = eq.ComboInfo("MapViewWnd", "MVW_MapsDirCombobox")
    if packs and #packs > 0 then
      row("Map Pack", "The folder under maps/ the map, minimap and zone map are read from first, kept for this character. Default: maps/ itself.", function()
        if ui.BeginCombo("##map_pack", packs[pack + 1] or packs[1]) then
          for i, n in ipairs(packs) do
            if ui.Selectable(n .. "##mp" .. i, i - 1 == pack) then eq.ComboSelect("MapViewWnd", "MVW_MapsDirCombobox", i - 1) end
          end
          ui.EndCombo()
        end
      end)
    end
    -- The original's General page font button and list (the system's faces; /fontface).
    local fonts = eq.OptionChoices("font_face")
    if fonts and #fonts > 0 then
      combo("font_face", "Current Font", fonts, "The face every window's text is drawn in (/fontface).")
    end
    combo("ui_scale", "UI Scale", SCALE_NAMES,
      "The size of the windows and text. Takes effect the next time the game starts.", SCALES)
    local want, now = eq.Option("ui_scale"), eq.Option("ui_scale_now")
    if want and want > 0 and now and want ~= now then
      note("Restart the game to change the UI scale (now " .. now .. " %).")
    end
    check("pc_names", "Show Player Names", "The names over other players.")
    check("own_name", "Show My Name",
      "Hailstone: your own name over your head in the third-person views, with your title and guild as /shownames sets them for everyone.")
    check("npc_names", "Show NPC Names", "The names over NPCs.")
    combo("find_trail_style", "Find Trail", { "Gold line (default)", "Rainbow trail" },
      "Choose the gold ground line or thin, winding rainbow strands hovering a few feet above the terrain along your Find route.")
    check("item_compare", "Compare Items",
      "Hailstone: an item's tooltip and its details show what wearing it instead of what you wear would change: your HP, mana, AC, stats and resists as the game works them out, caps included.")
    check("item_compare_verdict", "Upgrade Verdict",
      "Hailstone: the comparison also says Upgrade, Downgrade or Mixed, weighing the changes by what matters for your class. It is a guide: the numbers are always shown.")
    check("con_name_colour", "Con-Coloured NPC Names", "Hailstone: NPC names take their consider colour, as your target's does. Off keeps them yellow.")
    check("target_plate_highlight", "Highlight Target Name",
      "Hailstone: makes your target's name easy to spot, on a dark background with a coloured edge and a small bouncing arrow above it. Turn it off to show the name as plain text in its colour.")
    check("target_plate_through_walls", "Target Name Through Walls",
      "Hailstone: your target's name stays visible behind walls and other things in the way. Off, it is hidden like any other name, as in the original client.")
    check("plate_overlap_fade", "Fade Overlapping Names",
      "Hailstone: when names pile up on top of each other, the ones behind fade out so the nearest stay readable. Your target's name never fades. Turn it off to draw every name at full strength.")
    check("show_target_ring", "Show 3D Target Ring", "A colored ring appears around your current target's feet.")
    check("tab_skip_pets", "Tab Skips Pets",
      "Hailstone: the NPC target keys (Tab, F8) skip players' pets and mercenaries.")
    check("tab_line_of_sight", "Tab Needs Line of Sight",
      "Hailstone: the NPC target keys (Tab, F8) skip NPCs you cannot see, behind walls or the ground.")
    check("replace_gem_spell", "Drop Replaces a Memorized Spell",
      "Hailstone: a spell dragged from the spell book onto a gem that holds another spell memorizes in its place. Off, forget the old spell first, as in the original.")
    check("hotbutton_extras", "More Hot Button States",
      "Hailstone: hot buttons of the Actions window's skills show their timers, Melee Attack lights while auto attack is on, Sit while sitting.")
    check("offer_layout_copy", "Offer Layouts to New Characters",
      "Hailstone: a character's first time in the world opens Copy Layout, to start from another character's windows, hot buttons and settings.")
  end)
  -- Hailstone's name plate text (game/nameplate_style.h; docs/graphics.md "Name plates"): the names over heads in
  -- their own face and size. Changes show at once.
  section("Name Plates", function()
    local faces = eq.OptionChoices("plate_font_face")
    if faces and #faces > 0 then
      combo("plate_font_face", "Font", faces,
        "Hailstone: the face the names over players and NPCs are drawn in. Same as UI follows Current Font.")
    end
    slider("plate_font_size", "Size", 8, 48, nil, "Hailstone: how big the names are, in points (15 is the UI's size).")
    check("plate_font_bold", "Bold",
      "Hailstone: names in the face's bold. A face with no bold is thickened instead.")
    combo("plate_edge", "Edge", { "Shadow", "Outline", "None" },
      "Hailstone: the dark edge that keeps names readable over bright scenery: a drop shadow, an outline all round, or none.")
    slider("plate_far_scale", "Size at Distance", 25, 100, pct,
      "Hailstone: how big a name is at the farthest names are shown, against one close by. 100 % keeps every name the same size.")
    slider("plate_tag_scale", "Guild and Owner Lines", 50, 100, pct,
      "Hailstone: the size of the lines under a name (a player's guild, an NPC's role or a pet's owner), against the name's.")
  end)
  -- Hailstone's target ring settings (app/target_ring.cpp; docs/graphics.md "The target ring"): its colour for each
  -- con (the colour picker, eq.ClickChild TargetRingColor<n>), how opaque and how bright it is, and its cues.
  section("Target Ring", function()
    check("target_ring_cues", "Cues",
      "Hailstone: the ring sweeps in when you change target, and turns faster and pulses while you auto attack.")
    check("target_ring_xray", "Through Models",
      "Hailstone: the part of the ring hidden behind characters and walls shows faintly through them.")
    slider("target_ring_opacity", "Opacity", 25, 200, pct, "How solid the ring is, against the con colour's own.")
    slider("target_ring_glow", "Glow", 0, 100, pct, "How brightly the ring's core shines (with the bloom).")
    -- Max Size (game::cap_ring_radius): 4 .. 40 units, and the slider's top end (41) is Off, the original's size.
    local maxSize = eq.Option("target_ring_max_size")
    if maxSize ~= nil then
      row("Max Size", "Hailstone: the largest the ring gets, so a giant's or a dragon's does not fill the screen; "
          .. "a smaller ring is left as it is. About 6 is a human's. All the way right is Off: every ring at its "
          .. "own size, as on the original client.", function()
        local v = maxSize == 0 and 41 or maxSize
        local nv, changed = ui.SliderInt("##target_ring_max_size", v, 4, 41, v > 40 and "Off" or tostring(v))
        if changed and nv ~= v then eq.SetOption("target_ring_max_size", nv > 40 and 0 or nv) end
      end)
    end
    for i, con in ipairs(RING_CONS) do
      local rgb = eq.Option("target_ring_color_" .. (i - 1))
      if rgb then
        row(con[1], "Hailstone: the ring's colour for a target that cons " .. con[2] .. ". Click to change it.", function()
          local r, g, b = (rgb >> 16 & 0xff) / 255, (rgb >> 8 & 0xff) / 255, (rgb & 0xff) / 255
          ui.PushStyleColor(ui.Col.Button, r, g, b, 1)
          ui.PushStyleColor(ui.Col.ButtonHovered, r, g, b, 0.85)
          ui.PushStyleColor(ui.Col.ButtonActive, r, g, b, 0.7)
          if ui.Button("##ringcolor" .. i, 60 * U, 0) then eq.ClickChild(WINDOW, "TargetRingColor" .. (i - 1)) end
          ui.PopStyleColor(3)
        end)
      end
    end
    row("", nil, function()
      if ui.Button("Default Colors") then eq.ClickChild(WINDOW, "TargetRingColorDefault") end
    end)
  end)
  -- The original's window fade sliders (COptionsWnd::WndNotification, CXWndManager::SetGlobal*): they set every
  -- window's value at once and keep it in the UI ini's [Main].
  section("Windows", function()
    local function global(label, key, default, count, write, text, tooltip)
      local raw = tonumber(eq.UiIni("Main", key) or "") or default
      local v = write.from(raw)
      row(label, tooltip, function()
        local nv, changed = ui.SliderInt("##" .. key, v, 0, count - 1, text(v))
        if changed and nv ~= v then eq.SetUiIni("Main", key, tostring(write.to(nv))) end
      end)
    end
    global("Window Transparency", "GlobalAlpha", 255, 230,
      { from = function(a) return math.max(0, a - 26) end, to = function(v) return v + 26 end },
      function(v) return string.format("%d %%", (v + 26) * 100 // 255) end,
      "How opaque every window is when the mouse is over it.")
    global("Fade-to Transparency", "GlobalFadeToAlpha", 128, 256,
      { from = function(a) return a end, to = function(v) return v end },
      function(v) return string.format("%d %%", v * 100 // 255) end,
      "How opaque a window becomes once the mouse has left it: the windows with Fades ticked in their right-click menu.")
    global("Fade Delay (seconds)", "GlobalFadeDelay", 2000, 21,
      { from = function(ms) return ms // 500 end, to = function(v) return v * 500 end },
      function(v) return string.format("%.1f", v * 0.5) end,
      "How long a window with Fades waits after the mouse leaves it before it fades.")
    global("Fade Duration (seconds)", "GlobalFadeDuration", 500, 20,
      { from = function(ms) return math.max(0, ms // 500 - 1) end, to = function(v) return (v + 1) * 500 end },
      function(v) return string.format("%.1f", v * 0.5 + 0.5) end,
      "How long a window takes to fade, in or out.")
    -- Hailstone: grid snapping for dragged windows (lib/snap.lua; also in every window's right-click menu)
    local snapOn = snap.On()
    row("Snap Windows to Grid",
      "Hailstone: a dragged window lines up on a grid. Hold Shift while dragging to place it freely.", function()
        local v, changed = ui.Checkbox("##hud.snap", snapOn)
        if changed then ui.SetSetting("hud.snap", v and "1" or "0") end
      end)
    if snapOn then
      row("Grid Size", "The grid's spacing, in pixels at a UI scale of 100 %.", function()
        local v, changed = ui.SliderInt("##hud.snap.size", snap.Size(), snap.MIN_SIZE, snap.MAX_SIZE)
        if changed then ui.SetSetting("hud.snap.size", tostring(v)) end
      end)
    end
  end)
  section("Buffs and Songs", function()
    check("show_buff_timer", "Show Buff Timer", "Determines whether the buff timer is displayed on the buff.")
    check("show_buff_counters", "Show Buff Counters",
      "Determines whether the buff counters are displayed on the buff.")
    check("show_buff_limit_use", "Show Buff Limit Uses",
      "Determines whether the buff limit uses are displayed on the buff.")
    check("show_song_timer", "Show Song Timer", "Determines whether the song timer is displayed on the song.")
    check("show_song_counters", "Show Song Counters",
      "Determines whether the song counters are displayed on the song.")
    check("show_song_limit_use", "Show Song Limit Uses",
      "Determines whether the song limit uses are displayed on the song.")
  end)
  -- The original's Particles tabs (Spell, Environment, Actor): each kind's density (COptionsWnd's density combos;
  -- /particledensity sets the environment's).
  section("Particles", function()
    local DENSITIES = { "Off", "Low", "Medium", "High" }
    combo("spell_particle_density", "Spell Particle Density", DENSITIES, "How many particles spells give off.")
    -- Hailstone: whose spells, each on top of the spell density (the original's "On For" filter is not offered).
    combo("spell_particles_self", "Your Spell Particles", DENSITIES,
      "Spells you cast and spells that land on you, on top of the spell particle density.")
    combo("spell_particles_players", "Other Players' Spell Particles", DENSITIES,
      "Spells other players cast, on top of the spell particle density.")
    combo("spell_particles_npcs", "NPC Spell Particles", DENSITIES,
      "Spells NPCs cast, pets and mercenaries included, on top of the spell particle density.")
    combo("environment_particle_density", "Environment Particle Density", DENSITIES,
      "How many particles the zone's clouds give off: torches, braziers, smoke and mist (/particledensity).")
    combo("actor_particle_density", "Actor Particle Density", DENSITIES,
      "How many particles creatures and glowing weapons give off.")
  end)
  section("Picture", function()
    ticks("brightness", "Brightness", function(t) return string.format("%+d %%", (t - 10) * 10) end,
      "Lifts or lowers the whole picture (not the UI).")
    ticks("contrast", "Contrast", function(t) return string.format("%d %%", 50 + t * 5) end,
      "Spreads or narrows the picture's light and dark.")
    ticks("gamma", "Gamma", function(t) return string.format("%.2f", 0.5 + t * 0.05) end,
      "Brightens (higher) or darkens (lower) the shadows and mid tones.")
    ui.TableNextRow()
    ui.TableNextColumn()
    ui.TableNextColumn()
    if ui.Button("Reset Picture", 0, 0) then
      for _, o in ipairs({ "brightness", "contrast", "gamma" }) do eq.SetOption(o, 10) end
    end
  end)
  -- The RoF2 asset folder the game reads (hailstone.ini assets.path); the picker changes it on the login screen.
  local pick = eq.Option("asset_folder_pick")
  if pick ~= nil then
    section("Game Files", function()
      row("Asset Folder", "The EverQuest (Rain of Fear 2) installation the game reads its files from. "
        .. "Hailstone never changes it.", function()
        ui.TextWrapped(eq.OptionChoices("asset_folder")[1] or "?")
      end)
      ui.TableNextRow()
      ui.TableNextColumn()
      ui.TableNextColumn()
      if pick == 0 then
        if ui.Button("Change Asset Folder", 0, 0) then eq.SetOption("asset_folder_pick", 1) end
        tip("Shows the folder picker on the login screen (after camping out), or the next time the game starts.")
      else
        if ui.Button("Keep This Folder", 0, 0) then eq.SetOption("asset_folder_pick", 0) end
      end
      if pick ~= 0 then
        note(eq.Option("asset_folder_override") == 1
          and "Started with --game-dir or EQ_GAME_DIR: that folder is used while it is given."
          or "The folder picker shows on the login screen: camp out to choose another asset folder.")
      end
    end)
  end
end

-- ---------------------------------------------------------------- Graphics
-- Quality presets: each sets the costlier settings together; "Custom" when the settings match none.
local PRESETS = {
  { name = "Low", shadows = 0, msaa = 0, ao = 0, distance_blur = 0, view_distance = 2 },
  { name = "Medium", shadows = 1, msaa = 1, ao = 0, distance_blur = 1, view_distance = 4 },
  { name = "High", shadows = 2, msaa = 2, ao = 1, distance_blur = 1, view_distance = 6 },
  { name = "Ultra", shadows = 3, msaa = 3, ao = 1, distance_blur = 1, view_distance = 12 },
}
local PRESET_KEYS = { "shadows", "msaa", "ao", "distance_blur", "view_distance" }

local function currentPreset()
  for i, p in ipairs(PRESETS) do
    local same = true
    for _, k in ipairs(PRESET_KEYS) do
      if eq.Option(k) ~= p[k] then same = false end
    end
    if same then return i end
  end
end

local msaaAtStart -- anti-aliasing changes at the next start: the setting the client started with
local packAtStart -- so does the texture pack

local function graphicsTab()
  msaaAtStart = msaaAtStart or eq.Option("msaa")
  section("Quality", function()
    row("Preset", "Sets shadows, anti-aliasing, ambient occlusion, distance blur and view distance together.", function()
      local p = currentPreset()
      if ui.BeginCombo("##preset", p and PRESETS[p].name or "Custom") then
        for i, preset in ipairs(PRESETS) do
          if ui.Selectable(preset.name, i == p) then
            for _, k in ipairs(PRESET_KEYS) do eq.SetOption(k, preset[k]) end
          end
        end
        ui.EndCombo()
      end
    end)
    combo("shadows", "Shadows", { "Off", "Low", "Medium", "High" },
      "Sun and moon shadows (modern lighting). Higher is sharper and slower.")
    slider("shadow_clip_plane", "Shadow Clip Plane", 0, 100,
      function(v) return string.format("%d (%d units)", v, v * 5 + 10) end,
      "How far from the camera characters cast shadows (the original's advanced display setting; /shadows on <n>).")
    combo("msaa", "Anti-aliasing", { "Off", "2x MSAA", "4x MSAA", "8x MSAA" },
      "Smooths edges. Takes effect the next time the game starts.")
    local msaa = eq.Option("msaa")
    if msaa ~= nil and msaaAtStart ~= nil and msaa ~= msaaAtStart then
      note("Restart the game to change anti-aliasing.")
    end
    slider("render_scale", "Render Scale", 50, 100, pct,
      "Draws the 3D world at this share of its size and upscales it with AMD FSR 1. Lower is faster and softer; the interface stays sharp.")
    if (eq.Option("render_scale") or 100) < 100 then
      ticks("sharpness", "Upscale Sharpening", function(t) return pct(t * 5) end,
        "How much FSR 1 sharpens the upscaled picture.")
    end
    check("grass", "Radial Flora",
      "Grass, flowers and ferns on the newer zones' open ground, and Hailstone's grass on classic zones such as East Commonlands (/showgrass).")
    slider("flora_distance", "Flora Distance", 400, 1200, function(v) return string.format("%d units", v) end,
      "How far round the camera the grass is drawn. Past 400 units it thins out with distance. Further is slower.")
    check("ao", "Ambient Occlusion", "Soft contact shadows where surfaces meet (modern lighting).")
    check("distance_blur", "Distance Blur", "Softens the far distance a little (modern lighting).")
    check("neighbour_zones", "Neighbour Zones",
      "Draws the next zone past an outdoor zone line in the distance (modern lighting; scenery only).")
    check("horizon_hills", "Horizon Hills",
      "Land, hills and mountains past the edges of outdoor zones, in the zone's own ground, so the world does not end in haze (modern lighting; scenery only).")
    check("ground_npcs", "Ground Floating NPCs",
      "Draws an NPC the server left standing in the air on the ground below it (up to 50 units). Flying and levitating NPCs stay up.")
    ticks("bloom", "Bloom", function(t) return pct(t * 5) end, "A glow around bright lights and the sun.")
    ticks("view_distance", "View Distance", function(t) return string.format("%.2fx", 1 + t * 0.25) end,
      "How far the modern lighting draws, times the original's distance.")
    ticks("clip_plane", "Far Clip Plane", function(t) return pct(t * 100 // 20) end,
      "The original's view distance setting (the classic lighting draws this far; modern multiplies it).")
  end)
  section("Lighting", function()
    combo("lighting", "Lighting", { "Modern", "Classic" },
      "Modern: per-pixel sun, shadows and haze. Classic: the original game's look.")
    combo("tonemap", "Tone Map", { "Automatic", "None", "AgX", "ACES" },
      "How bright light is fitted to the screen. Automatic: AgX with modern lighting, none with classic.")
    ticks("exposure", "Exposure", function(t) return string.format("%+.1f EV", -2 + t * 0.2) end,
      "The overall scene brightness before the tone map, in stops.")
    check("color_grade", "Colour Grade",
      "A subtle grade by the time of day, the weather and the zone's mood (modern lighting).")
  end)
  section("Camera", function()
    slider("max_camera_distance", "Max Camera Zoom", 10, 150, function(v) return string.format("%d units", v) end,
      "How far the mouse wheel zooms the camera out behind you. The default is 60.")
  end)
  -- Texture packs (display.texture_folder): a folder of the Hailstone folder's textures/, such as an HD pack built by
  -- Hailstone Texture Studio (a tool add-on, docs/addons.md), read at the next start.
  local pack = eq.Option("texture_pack")
  if pack ~= nil then
    packAtStart = packAtStart or pack
    section("Textures", function()
      row("Texture Pack", "Replacement textures from a folder of Hailstone's textures folder. Off: the game's own.",
        function()
          local choices = eq.OptionChoices("texture_pack")
          if ui.BeginCombo("##texture_pack", choices[pack + 1] or "A folder of its own") then
            for i, name in ipairs(choices) do
              if ui.Selectable(name, i == pack + 1) then eq.SetOption("texture_pack", i - 1) end
            end
            ui.EndCombo()
          end
        end)
      if pack ~= packAtStart then note("Restart the game to change the texture pack.") end
      ui.TableNextRow()
      ui.TableNextColumn()
      ui.TableNextColumn()
      if eq.Option("texture_studio") == 1 then
        if ui.Button("Texture Studio...", 0, 0) then eq.SetOption("texture_studio", 1) end
        tip("Builds an HD texture pack from your own game files on your graphics card (a program of its own).")
      else
        ui.TextDisabled("Texture Studio builds HD packs: install it from Add-ons on the login screen.")
      end
    end)
  end
  if ui.Button("Restore Default Graphics", 0, 0) then
    -- (anti-aliasing and the lighting mode stay as they are)
    eq.SetOption("view_distance", 6)
    eq.SetOption("exposure", 10)
    eq.SetOption("ao", 1)
    eq.SetOption("distance_blur", 1)
    if eq.Option("color_grade") ~= nil then eq.SetOption("color_grade", 1) end
    eq.SetOption("shadows", 2)
    eq.SetOption("tonemap", 0)
    eq.SetOption("clip_plane", 15)
    if eq.Option("bloom") ~= nil then eq.SetOption("bloom", 10) end
    if eq.Option("render_scale") ~= nil then eq.SetOption("render_scale", 100) end
    if eq.Option("sharpness") ~= nil then eq.SetOption("sharpness", 10) end
    eq.SetOption("max_camera_distance", 60)
    if eq.Option("flora_distance") ~= nil then eq.SetOption("flora_distance", 400) end
  end
end

-- ---------------------------------------------------------------- Audio
local function audioTab()
  section("Volume", function()
    check("mute", "Mute All Sound", "Silences music and sounds without changing the volumes.")
    slider("master", "Master Volume", 0, 100, pct)
    slider("music", "Music", 0, 100, pct)
    slider("effects", "Sound Effects", 0, 100, pct, "Spells, combat and the world's sounds.")
    slider("npc_hits", "NPC Hit Sounds", 0, 100, pct,
      "NPCs being hit, blocking and dodging, on top of Sound Effects (a Hailstone option).")
  end)
  section("Sound", function()
    check("environment", "Environment Sounds", "The zone's ambient sounds: wind, water, birds and the like.")
    check("combat_music", "Combat Music", "Music when a fight starts.")
    check("reverb", "Reverb", "The room's echo on sounds.")
    slider("realism", "Reverb Amount", 0, 100, pct, "How much echo each sound gets (the original's Realism).")
  end)
end

-- ---------------------------------------------------------------- Controls
local KEYS_LIST, KEYS_FILTER = "OKP_KeyboardAssignmentList", "OKP_KeyboardFilterCombobox"

local function keyBindings()
  local choices, selected = eq.ComboInfo(WINDOW, KEYS_FILTER)
  if choices then
    ui.Text("Show")
    ui.SameLine()
    ui.SetNextItemWidth(180 * U)
    if ui.BeginCombo("##keyfilter", choices[selected + 1] or "", ui.ComboFlags.HeightLarge) then
      for i, c in ipairs(choices) do
        if ui.Selectable(c .. "##k" .. i, i - 1 == selected) then eq.ComboSelect(WINDOW, KEYS_FILTER, i - 1) end
      end
      ui.EndCombo()
    end
    ui.SameLine()
  end
  if ui.Button("Default Keys", 0, 0) then eq.WindowButton(WINDOW, "OKP_DefaultKeysButton") end
  ui.TextDisabled("Click a key to change it, then press the new key (Escape clears it). Red: the key is bound to another command too.")
  local rows = eq.ListInfo(WINDOW, KEYS_LIST)
  if not rows then return end
  local _, availH = ui.GetContentRegionAvail()
  local flags = ui.TableFlags.Borders | ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.Resizable
  if ui.BeginTable("##keys", 3, flags, 0, math.max(availH, 120 * U)) then
    ui.TableSetupColumn("Command", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("Key", ui.TableColumnFlags.WidthFixed, 120 * U)
    ui.TableSetupColumn("Alternate", ui.TableColumnFlags.WidthFixed, 120 * U)
    ui.TableHeadersRow()
    for r = 0, rows - 1 do
      local texts = eq.ListRow(WINDOW, KEYS_LIST, r)
      if texts then
        ui.TableNextRow()
        ui.TableNextColumn()
        ui.Text(texts[1] or "")
        for c = 2, 3 do
          ui.TableNextColumn()
          local t = texts[c] or ""
          -- red: the key is bound to another command too
          local kc = texts.colors and texts.colors[c]
          if kc then ui.PushStyleColor(ui.Col.Text, kc[1], kc[2], kc[3], 1) end
          if ui.Selectable((t ~= "" and t or "-") .. "##" .. r .. "_" .. c, false) then
            eq.ListClick(WINDOW, KEYS_LIST, r, "left", c - 1)
          end
          if kc then ui.PopStyleColor() end
          if kc and ui.IsItemHovered() then ui.SetTooltip("This key is bound to another command too.") end
        end
      end
    end
    ui.EndTable()
  end
end

local function controlsTab()
  section("Mouse", function()
    check("mouse_invert_y", "Invert Mouse Y", "Moving the mouse up looks down.")
    check("mouse_wheel_zoom", "Mouse Wheel Zoom", "The mouse wheel moves the camera in and out.")
    check("mouse_look", "Mouse Look", "Holding the right mouse button turns the view.")
    check("look_spring", "Look Spring", "The view springs back level when you move.")
    check("mouse_turn_zoom", "Turn Zoom Camera", "Toggle this to enable or disable zoom camera turning.")
    check("mouse_right_handed", "Right-handed Mouse", "Off: the mouse's left and right buttons are swapped.")
    slider("mouse_sensitivity", "Mouse Sensitivity", 0, 20, nil, "How far the view turns for a mouse movement.")
  end)
  heading("Key Bindings")
  keyBindings()
end

-- ---------------------------------------------------------------- Chat
local FILTERS, FILTER_SETTING = "FilterList", "FilterComboBox"

local function chatTab()
  ui.TextDisabled("What the chat windows show of each kind of message.")
  local rows = eq.ListInfo(WINDOW, FILTERS)
  if not rows then return end
  local _, availH = ui.GetContentRegionAvail()
  local flags = ui.TableFlags.Borders | ui.TableFlags.RowBg | ui.TableFlags.ScrollY
  if ui.BeginTable("##filters", 2, flags, 0, math.max(availH, 120 * U)) then
    ui.TableSetupColumn("Messages", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("Show", ui.TableColumnFlags.WidthFixed, 140 * U)
    ui.TableHeadersRow()
    for r = 0, rows - 1 do
      local texts = eq.ListRow(WINDOW, FILTERS, r)
      if texts then
        ui.TableNextRow()
        ui.TableNextColumn()
        ui.Text(texts[1] or "")
        ui.TableNextColumn()
        ui.SetNextItemWidth(-1)
        if ui.BeginCombo("##filter" .. r, texts[2] or "") then
          -- The setting's choices are the selected filter's (the original's list and its drop-down).
          eq.ListClick(WINDOW, FILTERS, r)
          local choices, selected = eq.ComboInfo(WINDOW, FILTER_SETTING)
          for i, c in ipairs(choices or {}) do
            if ui.Selectable(c .. "##f" .. i, i - 1 == selected) then eq.ComboSelect(WINDOW, FILTER_SETTING, i - 1) end
          end
          ui.EndCombo()
        end
      end
    end
    ui.EndTable()
  end
end

local COLORS = "ColorChooseList"

-- The original's Colors page: each user colour with its sample; a row selected, Change Selected Color opens the
-- colour picker on it (Accept stores it in eqclient.ini) and Reset Default Colors asks before restoring them all.
local function colorsTab()
  ui.TextDisabled("The colours of the chat's kinds of text.")
  local rows, _, selected = eq.ListInfo(WINDOW, COLORS)
  if not rows then return end
  local _, availH = ui.GetContentRegionAvail()
  local footer = 32 * U -- the buttons below
  local flags = ui.TableFlags.Borders | ui.TableFlags.RowBg | ui.TableFlags.ScrollY
  if ui.BeginTable("##colors", 2, flags, 0, math.max(availH - footer, 120 * U)) then
    ui.TableSetupColumn("Text", ui.TableColumnFlags.WidthStretch, 0)
    ui.TableSetupColumn("Sample", ui.TableColumnFlags.WidthFixed, 140 * U)
    ui.TableHeadersRow()
    for r = 0, rows - 1 do
      local texts = eq.ListRow(WINDOW, COLORS, r)
      if texts then
        ui.TableNextRow()
        ui.TableNextColumn()
        if ui.Selectable((texts[1] or "") .. "##c" .. r, r == selected, ui.SelectableFlags.SpanAllColumns) then
          eq.ListClick(WINDOW, COLORS, r)
        end
        ui.TableNextColumn()
        local c = texts.colors and texts.colors[2] or { 1, 1, 1 }
        ui.TextColored(c[1], c[2], c[3], 1, texts[2] or "")
      end
    end
    ui.EndTable()
  end
  if ui.Button("Change Selected Color") and selected >= 0 then eq.ClickChild(WINDOW, "UserColorChange") end
  ui.SameLine()
  if ui.Button("Reset Default Colors") then eq.ClickChild(WINDOW, "UserColorDefault") end
end

-- The plugins' options (eq.PluginOptions; docs/plugins.md "Plugin windows"), one section per plugin: a switch, or
-- a slider over the option's range.
local function pluginsTab()
  local by = {}
  local order = {}
  for _, o in ipairs(eq.PluginOptions()) do -- by owner (a compiled plugin and a script may share a name)
    if not by[o.owner] then
      by[o.owner] = {}
      order[#order + 1] = o.owner
    end
    table.insert(by[o.owner], o)
  end
  for _, owner in ipairs(order) do
    section(by[owner][1].plugin, function()
      for _, o in ipairs(by[owner]) do
        row(o.label, nil, function()
          local id = "##plugin." .. o.owner .. "." .. o.name
          if o.max == 1 and o.min == 0 then
            local on, changed = ui.Checkbox(id, o.value ~= 0)
            if changed then eq.SetPluginOption(o.owner, o.name, on and 1 or 0) end
          else
            local nv, changed = ui.SliderInt(id, o.value, o.min, o.max)
            if changed then eq.SetPluginOption(o.owner, o.name, nv) end
          end
        end)
      end
    end)
  end
end

-- ---------------------------------------------------------------- window
local TABS = {
  { "General", generalTab }, { "Display", displayTab }, { "Graphics", graphicsTab }, { "Audio", audioTab },
  { "Controls", controlsTab }, { "Chat", chatTab }, { "Colors", colorsTab },
}

-- The tabs. The stock page the game turned to (COptionsWnd_SetTabPage; /filter: 4, the chat filters): its tab here,
-- once (the page is then set to -1, which no tab is).
local function pages(visible)
  local want = eq.TabPage(WINDOW, "OPTW_OptionsSubwindows")
  if want and want >= 0 then eq.SetTabPage(WINDOW, "OPTW_OptionsSubwindows", -1) end
  local tabs = TABS
  if #eq.PluginOptions() > 0 then -- the Plugins tab, while a loaded plugin has options
    tabs = { table.unpack(TABS) }
    tabs[#tabs + 1] = { "Plugins", pluginsTab }
  end
  if visible and ui.BeginTabBar("##optionstabs") then
    for _, t in ipairs(tabs) do
      local select = (want == 4 and t[1] == "Chat") and ui.TabItemFlags.SetSelected or 0
      if ui.BeginTabItem(t[1], select) then
        if ui.BeginChild("##" .. t[1], 0, 0, false) then t[2]() end
        ui.EndChild()
        ui.EndTabItem()
      end
    end
    ui.EndTabBar()
  end
end

ui.window("Options", function()
  if not eq.InGame() then return end
  if not eq.WindowShown(WINDOW) then return end
  U = ui.GetIO().uiScale or 1
  ui.SetNextWindowSize(560 * U, 600 * U, ui.Cond.FirstUseEver)
  style.Push()
  local w, b = C.window, C.frameBorder
  ui.PushStyleColor(ui.Col.WindowBg, w[1], w[2], w[3], w[4])
  local visible, open = ui.Begin("Options##hailstone", true, ui.WindowFlags.NoCollapse)
  if esc.Frame(WINDOW) then open = false end
  -- the controls' outline (inside the window only: its own edge keeps the hairline)
  ui.PushStyleColor(ui.Col.Border, b[1], b[2], b[3], b[4])
  ui.PushStyleVar(ui.StyleVar.FrameBorderSize, 1)
  local ok, err = pcall(pages, visible)
  -- (also after an error in a page: what was pushed above)
  ui.PopStyleVar()
  ui.PopStyleColor()
  if visible and ui.BeginPopupContextWindow("##optionsmenu") then -- a right click on an empty spot
    if ui.MenuItem("Close") then open = false end
    esc.MenuItem(WINDOW)
    ui.EndPopup()
  end
  ui.End()
  ui.PopStyleColor()
  style.Pop()
  if not ok then error(err, 0) end
  if not open then eq.ShowWindow(WINDOW, false) end
end)
