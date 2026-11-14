-- Group (gamepad pack), from the main menu: the stock group window's buttons as large tiles (Invite your target,
-- Follow / Decline an invitation, Disband, LFG) and the members to target. Each is a click on the stock
-- GroupWindow's child, so the game enables, hides and handles them as it does its own buttons.
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local roles = dofile("lib/role_icons.lua")
local C = T.C
local ROLE_COLORS = { C.mana, C.accent, C.endurance }

local GW = "GroupWindow"
local BUTTONS = {
  { "FollowButton", "Follow", "Join the group you were invited to" },
  { "DeclineButton", "Decline", "Turn the invitation down" },
  { "InviteButton", "Invite", "Invite your target" },
  { "DisbandButton", "Disband", "Leave the group" },
  { "LFGButton", "LFG", "Looking for group" },
}

-- An invitation: the client's own prompt (Follow / Decline, ui/group_windows.cpp) takes the pad when it appears, so
-- A follows without a trip through the menu (the group window's Follow below does the same).
local invited = false

ui.window("Group", function()
  if not eq.InGame() then return end
  local inv = eq.ChildShown(GW, "FollowButton") == true
  if inv and not invited then pad.focus("Group invitation##hailstone_invite") end
  invited = inv
  if not GPSTATE.groupOpen then return end
  if W.begin("Group", { w = 900, h = 720, title = "Group", hints = { { "a", "Select" }, { "b", "Close" } } }) then
    local x0, _, x1, _, cw = W.area()
    local bw = (cw - T.px(20)) / 3
    local n = 0
    for _, b in ipairs(BUTTONS) do
      if eq.ChildShown(GW, b[1]) ~= false then
        if n % 3 ~= 0 then ui.SameLine(0, T.px(10)) end
        n = n + 1
        local enabled = eq.ChildState(GW, b[1])
        if T.button("##" .. b[1], b[2], bw, T.px(96), { sub = b[3], disabled = enabled == false, align = 0 }, pad) then
          eq.ClickChild(GW, b[1], "left")
        end
      end
    end
    ui.Dummy(cw, T.px(10))
    T.heading("Members", cw)
    local any = false
    for i = 1, 5 do
      local frac, name, raw = eq.Gauge(10 + i)
      if raw ~= -1 and name and name ~= "" then
        any = true
        local mask = roles.Mask(GW, i)
        local clicked, _, bx0, by0, bx1, by1 = T.button("##m" .. i, "", cw, T.px(64), {}, pad)
        local hovered = ui.IsItemHovered()
        local nx, nr = bx0 + T.px(18), bx0 + cw * 0.45
        local iconSize, iconGap = T.px(20), T.px(4)
        local iconW = roles.Width(mask, iconSize, iconGap)
        local fittedName = T.fit(name:gsub("\n.*", ""), nr - nx - (iconW > 0 and iconW + T.px(6) or 0))
        local nameW, nameH = ui.CalcTextSize(fittedName)
        T.draw(nx, (by0 + by1 - nameH) / 2, fittedName)
        local roleHovered = roles.Draw(mask, nx + nameW + T.px(6), (by0 + by1 - iconSize) / 2,
          iconSize, iconGap, ROLE_COLORS)
        T.bar(bx0 + cw * 0.48, by0 + T.px(20), bx1 - T.px(18), by1 - T.px(24), frac, C.hp)
        if hovered and not roleHovered then ui.SetTooltip(name .. "\nRight-click for group roles") end
        if hovered and not ui.GetIO().alt and ui.IsMouseClicked(ui.Mouse.Right) then
          eq.ClickChild(GW, "Gauge" .. i, "right")
        end
        if clicked then eq.ClickChild(GW, "Gauge" .. i, "left") end -- targets the member, as F2..F6
      end
    end
    if not any then T.draw(x0, select(2, ui.GetCursorScreenPos()), "You are not in a group.", C.dim) end
  end
  if W.finish() then GPSTATE.groupOpen = false end
end)
