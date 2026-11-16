-- Choose a skill: replaces the game's SkillsSelectWindow, the picker a right click on an Actions window ability
-- button opens. The skills the button can take as rows with a search; a click is the original list's click (the
-- skill goes on the button and the picker closes), "(none)" empties the button.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "SkillsSelectWindow"
local LIST = "SKSW_SkillSelectorList"

local search = ""

local OPTS = {
  size = function() return 240, 320 end,
  resizable = { 200, 200 },
}
local function body(x, y, w, h, s)
  T.Text(x, y, "Pick the skill for this button", C.muted, T.font.small)
  ui.SetCursorScreenPos(x, y + 20 * s)
  search = P.Search("##skselsearch", search, "Search skills")
  local needle = search:lower()
  P.Rows(WINDOW, LIST, 0, 0, {
    rowH = 24,
    filter = needle ~= "" and function(row, texts)
      return row == 0 or (texts[1] or ""):lower():find(needle, 1, true) ~= nil
    end or nil,
    draw = function(row, texts, x0, y0, x1, y1, s_, st)
      local _, th = T.TextSize("Ag", T.font.small)
      local none = row == 0
      T.Text(x0 + 8 * s_, (y0 + y1 - th) / 2, none and "Clear this button" or T.Fit(texts[1] or "", x1 - x0 - 16 * s_,
        T.font.small), none and C.muted or st.color, T.font.small)
    end,
    empty = "You have no skills to put here",
  })
end

local function testOpen() API.windows.ClickChild("ActionsWindow", "AAP_FirstAbilityButton", "right") end
ui.window("Choose a skill", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  hud.Dialog(WINDOW, "Choose a skill", OPTS, body)
end)
