-- Skills window: replaces the game's SkillsWindow. The skill list as a sortable table (click a header) with a
-- search box; a row click selects the skill in the game's list, so Make Hot Key works on it, and dragging a row
-- puts its hot button on the cursor, as on the stock list.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color
local WINDOW = "SkillsWindow"

local search = ""

local WINDOW_OPTS = {
  size = function() return 340, 460 end,
  resizable = { 260, 220 },
}
local function drawWindow(x, y, w, h, s)
  ui.SetNextItemWidth(-1)
  search = ui.InputTextWithHint("##skillsearch", "Search skills", search, 64)
  local needle = search:lower()
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  W.List(WINDOW, "SkillList", 0, -(btnH + 8 * s), {
    sortable = true,
    filter = needle ~= "" and function(_, texts) return (texts[1] or ""):lower():find(needle, 1, true) ~= nil end or nil,
  })
  W.ButtonRow(WINDOW, {
    { "MakeHotKeyButton", "Make Hot Key", { tooltip = "A hot button for the selected skill (or drag a row)" } },
    { "DoneButton", "Done" },
  }, btnH)
end

ui.window("Skills", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Skills", WINDOW_OPTS, drawWindow)
end)
