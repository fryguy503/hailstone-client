-- Server plugins (Hailstone): the question a server's compiled plugins ask before they load (hs.PluginConsent; docs/
-- plugins.md "The server's compiled plugins ask first"). Which server, each plugin's name, file, size and the start of
-- its SHA-256, the warning that a compiled plugin is a program with full access to the computer, and Allow / Don't
-- allow (both kept for those bytes on this server). Its close box puts the question away until the next zone-in. It
-- stands in for the client's own prompt ("PluginConsent"), which stays off while this runs.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local K = dofile("lib/dialogs_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "PluginConsent"
-- The answer buttons wait this long after a question shows (or a new one takes its place), so a click or key meant
-- for something else in the instant it pops up cannot answer it.
local ARM_SECONDS = 1.0
local shownSerial, shownAt = nil, 0

local function sizeText(n)
  if n >= 1024 * 1024 then return string.format("%.1f MB", n / (1024 * 1024)) end
  if n >= 1024 then return string.format("%.0f KB", n / 1024) end
  return string.format("%d bytes", n)
end

ui.window("Server Plugins", function()
  local q = API.plugins.Consent()
  if not q then
    shownSerial = nil
  elseif q.serial ~= shownSerial then
    shownSerial, shownAt = q.serial, ui.GetTime()
  end
  hud.Dialog(WINDOW, q and q.title or "Allow this server's plugin?", {
    shown = q ~= nil,
    keepTitle = true,
    rollup = false, -- (a question to answer: kept whole)
    size = function() return 460, 150 + 38 * (q and #q.plugins or 1) end,
    place = K.Centered,
    onClose = function() if q then API.plugins.AnswerConsent(nil, q.serial) end end,
  }, function(x, y, w, h, s)
    local cx = ui.GetCursorPos()
    ui.PushTextWrapPos(cx + w)
    ui.TextWrapped(q.intro)
    for _, p in ipairs(q.plugins) do
      ui.BulletText(p.name)
      ui.SameLine()
      ui.TextDisabled(string.format("%s, %s", p.file, sizeText(p.size)))
      ui.Indent(18 * s)
      ui.TextDisabled("sha256 " .. p.short)
      if ui.IsItemHovered() then ui.SetTooltip(p.sha256) end
      ui.Unindent(18 * s)
    end
    ui.Spacing()
    ui.TextColored(C.danger[1], C.danger[2], C.danger[3], 1, q.warning)
    ui.TextDisabled("Your answer is kept for this server. The add-on list on the login screen can change it.")
    ui.PopTextWrapPos()
    ui.Spacing()
    local bw = 150 * s
    local armed = ui.GetTime() - shownAt >= ARM_SECONDS
    if not armed then ui.BeginDisabled() end
    if ui.Button("Allow##consent", bw, K.ButtonH(s)) and armed then API.plugins.AnswerConsent(true, q.serial) end
    ui.SameLine()
    if ui.Button("Don't allow##consent", bw, K.ButtonH(s)) and armed then API.plugins.AnswerConsent(false, q.serial) end
    if not armed then ui.EndDisabled() end
  end)
end)
