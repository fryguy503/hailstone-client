-- Tribute benefits: replaces the game's TributeBenefitWnd. Two pages: Personal (favour, career favour, the
-- active cost and the time to the next payment as tiles, whether tribute is on, the chosen benefits, the selected
-- one's description, Activate / Deactivate) and Guild (opted in, the guild's favour, cost and time, its benefits and
-- description, Opt In / Out, Activate). The numbers are the game's labels (EQTypes 116..119, 121..123), the lists,
-- descriptions and buttons the original's children, so turning tribute on or off is the game's. The trophy pages
-- are left out (the client does not build them). The page shown is kept here: the original's tab box has none.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "TributeBenefitWnd"

local testTabDone = false

local function label(t) return API.character.Label(t) or "" end

-- the chosen benefits as a list and the selected one's description under it, then the buttons
local function benefits(list, desc, s, btnH, buttons, emptyText, emptyHint)
  local rows = API.windows.ListInfo(WINDOW, list) or 0
  K.Heading("Benefits", rows == 1 and "1 chosen" or (rows .. " chosen"))
  local descH = 90 * s
  if rows == 0 then
    ui.BeginChild("##empty" .. list, 0, -(btnH + 8 * s), false)
    K.Empty(emptyText, emptyHint)
    ui.EndChild()
  else
    W.List(WINDOW, list, 0, -(descH + btnH + 16 * s), {
      columns = { { "Benefit (click for its description)", 0 } },
    })
    ui.BeginChild("##desc" .. list, 0, descH, false)
    local text = K.Breaks(W.Text(WINDOW, desc))
    if K.HasText(text) then K.Stml(WINDOW, desc, text)
    else K.Muted("Select a benefit to read what it does.", C.faint) end
    ui.EndChild()
  end
  W.ButtonRow(WINDOW, buttons, btnH)
end

local function personal(s, btnH)
  local active = W.Text(WINDOW, "ActivateButton") == "Deactivate"
  K.StatRow({
    { "Favor", label(117), C.gold, "Tribute favor you can spend" },
    { "Career favor", label(118), C.text, "All the favor you have earned" },
    { "Active cost", label(119), C.text, "Favor paid for each period while tribute is on" },
    { "Next payment", label(116), C.text, "Time until the next payment (minutes:seconds)" },
  }, 100)
  ui.Dummy(0, 2 * s)
  K.Chip(active and "Tribute is on" or "Tribute is off", active and C.pet or C.faint)
  benefits("ActiveBenefitList", "Description", s, btnH, {
    { "ActivateButton", nil, { primary = not active, tooltip = "Turn your personal benefits on or off" } },
  }, "No benefits chosen.", "A tribute master can set them up for you.")
end

local function guild(s, btnH)
  local active = W.Text(WINDOW, "GuildActivateButton") == "Deactivate"
  local optedIn = W.Text(WINDOW, "GuildStatusLabel") == "Yes"
  K.StatRow({
    { "Opted in", optedIn and "Yes" or "No", optedIn and C.pet or C.muted, "Whether you take part in guild tribute" },
    { "Guild favor", label(122), C.gold },
    { "Active cost", label(123), C.text },
    { "Next payment", label(121), C.text, "Time until the guild's next payment (minutes:seconds)" },
  }, 100)
  ui.Dummy(0, 2 * s)
  K.Chip(active and "Guild tribute is on" or "Guild tribute is off", active and C.pet or C.faint)
  benefits("GuildBenefitList", "GuildDescription", s, btnH, {
    { "OptOutButton", nil, { tooltip = "Take part in your guild's tribute benefits, or stop" } },
    { "GuildActivateButton", nil,
      { tooltip = "Turn the guild's benefits on or off (needs the guild permission to change them)" } },
  }, "Your guild has no benefits chosen.", "Its leaders choose them at a guild tribute master.")
end

local WINDOW_OPTS = {
  size = function() return 480, 460 end,
  resizable = { 360, 340 },
  keepTitle = true,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 40 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  -- testing: test.tab.TributeBenefitWnd=guild in ui/settings/modern.txt opens the Guild page once
  local pick = 0
  if ui.GetSetting("test.tab." .. WINDOW) == "guild" and not testTabDone then
    testTabDone = true
    pick = ui.TabItemFlags.SetSelected
  end
  if ui.BeginTabBar("##tributetabs") then
    if ui.BeginTabItem("Personal") then
      personal(s, btnH)
      ui.EndTabItem()
    end
    if ui.BeginTabItem("Guild", pick) then
      guild(s, btnH)
      ui.EndTabItem()
    end
    ui.EndTabBar()
  end
end

ui.window("Tribute Benefits", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Tribute", WINDOW_OPTS, drawWindow)
end)
