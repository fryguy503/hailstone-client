-- Combat abilities: replaces the game's CombatSkillSelectWnd, the list the combat ability window's "S" opens. The
-- character's combat abilities (disciplines) with a search, level and reuse, the selected one's description and
-- Make Hotkey (or a double click), which puts it on the cursor for a hot button or a combat ability button. Rows
-- and the button are the original's (CSSW_SkillSelectorList, CSSW_MakeHotkeyButton).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local WINDOW = "CombatSkillSelectWnd"
local LIST = "CSSW_SkillSelectorList"

local search = ""

local OPTS = {
  size = function() return 320, 360 end,
  resizable = { 240, 240 },
}
local function body(x, y, w, h, s)
  search = P.Search("##csksearch", search, "Search combat skills")
  local needle = search:lower()
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  local descH = 54 * s
  P.Rows(WINDOW, LIST, 0, -(descH + btnH + 16 * s), {
    rowH = 24,
    filter = needle ~= "" and function(row, texts)
      return (texts[1] or ""):lower():find(needle, 1, true) ~= nil
    end or nil,
    filterKey = needle,
    draw = function(row, texts, x0, y0, x1, y1, s_, st)
      local _, th = T.TextSize("Ag", T.font.small)
      -- Name, then the level and the reuse (columns 2 and 7) on the right.
      local right = (texts[2] or "") .. "  " .. (texts[7] or "")
      local rw = T.TextSize(right, T.font.small)
      T.Text(x0 + 8 * s_, (y0 + y1 - th) / 2, T.Fit(texts[1] or "", x1 - x0 - 24 * s_ - rw, T.font.small), st.color,
        T.font.small)
      T.Text(x1 - 8 * s_ - rw, (y0 + y1 - th) / 2, right, C.muted, T.font.small)
    end,
    doubleClick = function() API.windows.ClickChild(WINDOW, "CSSW_MakeHotkeyButton", "left") end,
    empty = "You have no combat abilities",
  })
  local _, dy = ui.GetCursorScreenPos()
  dy = dy + 4 * s
  ui.DrawRectFilled(x, dy, x + w, dy + descH, C.ink[1], C.ink[2], C.ink[3], 0.03, 6 * s)
  ui.SetCursorScreenPos(x + 8 * s, dy + 6 * s)
  if W.Text(WINDOW, "CSSW_SkillDescriptionStmlBox") == "" then
    P.Empty(w - 16 * s, descH - 12 * s, "Select an ability")
  else
    W.Stml(WINDOW, "CSSW_SkillDescriptionStmlBox", w - 16 * s, descH - 12 * s)
  end
  ui.SetCursorScreenPos(x, y + h - btnH)
  W.Button(WINDOW, "CSSW_MakeHotkeyButton", "Make Hotkey", { primary = true, width = w, height = btnH,
    tooltip = "Puts the selected ability on the cursor: drop it on a hot button or a combat ability button" })
end

local function testOpen() API.windows.ClickChild("CombatAbilityWnd", "CAW_OpenCombatSkillSelectWnd", "left") end
ui.window("Combat Skills", function()
  if not API.session.InGame() then return end
  P.TestOpen(WINDOW, testOpen)
  hud.Dialog(WINDOW, "Combat Abilities", OPTS, body)
end)
