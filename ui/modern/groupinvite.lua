-- Group invite (Hailstone): a popup in the upper middle of the screen while an invitation to a group waits, with who
-- sent it, Accept and Decline. The buttons are the group window's own (FollowButton, DeclineButton), so the answer
-- is the game's; the group window offers them too. Its X puts the popup away for this invitation only. It stands in
-- for the client's own prompt (ui/group_windows.cpp), which stays off while this runs.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "GroupWindow"
local W, H = 300, 92

local dismissed = nil -- the inviter whose popup was put away
local invited, inviter = false, nil -- this frame's (read before hud.Window, which the options and body read)

local GLOW = { 0, 0, 0, 0.6 }
local OPTS = {
  shown = function() return invited and inviter ~= dismissed end,
  size = function() return W, H end,
  place = function(sw, sh, w, h) return (sw - w) / 2, sh * 0.22 end,
  panelOpts = { opaque = true, glow = GLOW },
  onClose = function() dismissed = inviter end,
}
local BUTTONS = {
  { "FollowButton", "Accept", "##inviteFollowButton", { primary = true } },
  { "DeclineButton", "Decline", "##inviteDeclineButton", {} },
}

local function body(x, y, w, h, s)
  local pad = 12 * s
  T.Text(x + pad, y + 10 * s, "GROUP INVITE", C.faint, T.font.tiny)
  local who = (inviter and inviter ~= "") and inviter or "Someone"
  T.Text(x + pad, y + 26 * s, T.Fit(who .. " invites you to join a group.", w - 2 * pad, T.font.small), C.text,
    T.font.small)
  local bh = 26 * s
  local by = y + h - pad - bh
  local bw = (w - 2 * pad - 8 * s) / 2
  for k, b in ipairs(BUTTONS) do
    local bx = x + pad + (k - 1) * (bw + 8 * s)
    if T.Button(b[3], bx, by, bx + bw, by + bh, s, b[2], b[4]) then
      API.windows.ClickChild(WINDOW, b[1], "left")
    end
  end
end

ui.window("Group Invite", function()
  if not API.session.InGame() then return end
  API.windows.Shown("GroupInvite") -- turns the client's own invite prompt off while this popup runs
  invited = API.windows.ChildShown(WINDOW, "FollowButton") == true
  inviter = invited and (API.windows.Text(WINDOW, "Hailstone_Inviter") or "") or nil
  if not invited then dismissed = nil end
  GLOW[1], GLOW[2], GLOW[3] = C.accent[1], C.accent[2], C.accent[3]
  hud.Window(nil, "Group Invite", OPTS, body)
end)
