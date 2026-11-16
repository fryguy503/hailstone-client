-- Tribute master: replaces the game's TributeMasterWnd (a right click on a tribute master, or a guild tribute
-- master). Favor, career favor, the active cost and the next payment as tiles; tribute on / off; the benefits on
-- offer (searchable, sortable by cost or level) beside the ones chosen, Upgrade / Downgrade between them while
-- Modify Benefits is on (Save Benefits sends the change); the selected benefit's description; and the donation
-- bar: the item on your cursor, the platinum to give (a click asks how much), Donate, and what the last donation
-- was worth. Every number, list and button is the original's child (TMW_*), so the rules (what may be chosen, the
-- guild's two slots, donations) are the game's. In guild mode the guild's favor, cost and time show instead.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = hud.T
local C = T.color
local WINDOW = "TributeMasterWnd"

local state = {}

local function label(t) return API.character.Label(t) or "" end

local function lists(s, h, editing, btnH)
  local gap = 12 * s
  local leftW = (ui.GetContentRegionAvail() - gap) * 0.58
  -- on offer
  ui.BeginChild("##tmavail", leftW, h, false)
  K.Heading("On offer")
  local needle = K.Search(state, "tmsearch", "Search benefits")
  W.List(WINDOW, "TMW_AvailableBenefitList", 0, -(btnH + 6 * s), {
    sortable = true,
    filter = needle ~= "" and function(_, texts) return (texts[1] or ""):lower():find(needle, 1, true) ~= nil end or nil,
    columns = { [3] = { "Lvl", 40 } },
  })
  W.Button(WINDOW, "TMW_UpgradeButton", "Add / Upgrade", { width = -1, height = btnH, primary = editing,
    tooltip = editing and "Add the selected benefit, or raise the chosen one's tier"
      or "Press Modify Benefits first" })
  ui.EndChild()
  ui.SameLine(0, gap)
  -- chosen
  ui.BeginChild("##tmactive", 0, h, false)
  K.Heading("Chosen", editing and "editing" or nil)
  W.List(WINDOW, "TMW_ActiveBenefitList", 0, -(btnH + 6 * s), {
    columns = { { "Benefit", 0 } },
    onRow = function(_, texts)
      local t = texts[1] or ""
      if t == "(none)" then K.Muted("empty slot", C.faint)
      else ui.TextColored(C.text[1], C.text[2], C.text[3], 1, t) end
      return true
    end,
  })
  W.Button(WINDOW, "TMW_DowngradeButton", "Downgrade / Remove", { width = -1, height = btnH,
    tooltip = editing and "Lower the selected benefit's tier, or drop it at the first tier"
      or "Press Modify Benefits first" })
  ui.EndChild()
end

local function donations(s, btnH)
  K.Heading("Donate", "Last donation worth " .. W.Text(WINDOW, "TMW_ValueLabel") .. " favor")
  local size = 40 * s
  local x, y = ui.GetCursorScreenPos()
  -- the item to give: what you hold on the cursor (the original's TMW_SelectedItem shows it)
  local tex, u0, v0, u1, v1 = API.windows.ChildArt(WINDOW, "TMW_SelectedItem", "decal")
  T.Icon(x, y, size, s, type(tex) == "userdata" and tex or nil, u0, v0, u1, v1, tex and C.gold or nil)
  if T.Hit("##tmitem", x, y, x + size, y + size) then API.windows.ClickChild(WINDOW, "TMW_SelectedItem", "left") end
  if ui.IsItemHovered() then ui.SetTooltip(API.windows.ChildTooltip(WINDOW, "TMW_SelectedItem")) end
  ui.SetCursorScreenPos(x + size + 10 * s, y + (size - btnH) / 2)
  local plat = W.Text(WINDOW, "TMW_CoinButton")
  W.Button(WINDOW, "TMW_CoinButton", (plat ~= "" and plat or "0") .. " platinum",
    { width = 130 * s, height = btnH, tooltip = "How much platinum to give (click to choose)" })
  ui.SameLine(0, 8 * s)
  W.Button(WINDOW, "TMW_DonateButton", "Donate", { primary = true, width = -1, height = btnH,
    tooltip = "Give the item on your cursor, else the platinum chosen, for favor" })
  ui.SetCursorScreenPos(x, y + size + 4 * s)
  ui.Dummy(0, 0)
end

local WINDOW_OPTS = {
  size = function() return 640, 600 end,
  resizable = { 480, 460 },
  keepTitle = true,
  place = function(sw, sh, w, h) return sw / 2 - w / 2, sh / 2 - h / 2 - 30 end,
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local guildTime = W.Text(WINDOW, "TMW_GuildTimeLabel")
  local guild = guildTime ~= ""
  if guild then
    K.StatRow({
      { "Guild favor", W.Text(WINDOW, "TMW_GuildCurrentPointsLabel"), C.gold },
      { "Active cost", W.Text(WINDOW, "TMW_GuildCostLabel"), C.text },
      { "Next payment", guildTime, C.text, "Time until the guild's next payment (minutes:seconds)" },
    }, 110)
  else
    K.StatRow({
      { "Favor", label(117), C.gold, "Tribute favor you can spend" },
      { "Career favor", label(118), C.text, "All the favor you have earned" },
      { "Active cost", label(119), C.text, "Favor paid for each period while tribute is on" },
      { "Next payment", label(116), C.text, "Time until the next payment (minutes:seconds)" },
    }, 100)
  end
  ui.Dummy(0, 2 * s)
  -- on / off, and the edit switch
  local editing = W.Text(WINDOW, "TMW_LockButton") == "Save Benefits"
  W.Check(WINDOW, "TMW_StatusButton", guild and "Guild tribute on" or "Tribute on",
    { tooltip = "Turn the chosen benefits on or off (they cost favor while on)" })
  ui.SameLine()
  K.Chip(W.Text(WINDOW, "TMW_StatusText"), W.Text(WINDOW, "TMW_StatusText") == "Active" and C.pet or C.faint)
  ui.SameLine()
  local bw = 150 * s
  ui.SetCursorPos(select(1, ui.GetCursorPos()) + math.max(0, ui.GetContentRegionAvail() - bw), select(2, ui.GetCursorPos()))
  W.Button(WINDOW, "TMW_LockButton", nil, { width = bw, primary = editing,
    tooltip = editing and "Send the benefits as chosen" or "Start changing the chosen benefits" })
  if editing then K.Muted("Pick a benefit, Add / Upgrade or Downgrade it, then Save Benefits.", C.gold) end
  -- the lists fill what the description and the donations leave
  local descH = 76 * s
  local donateH = 40 * s + lh + 18 * s
  local _, availH = ui.GetContentRegionAvail()
  local listH = math.max(140 * s, availH - descH - donateH - lh - 20 * s)
  lists(s, listH, editing, btnH)
  K.Heading("Description")
  ui.BeginChild("##tmdesc", 0, descH, false)
  local text = K.Breaks(W.Text(WINDOW, "TMW_Description"))
  if K.HasText(text) then K.Stml(WINDOW, "TMW_Description", text)
  else K.Muted("Select a benefit to read what it does.", C.faint) end
  ui.EndChild()
  donations(s, btnH)
end

ui.window("Tribute Master", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Tribute Master", WINDOW_OPTS, drawWindow)
end)
