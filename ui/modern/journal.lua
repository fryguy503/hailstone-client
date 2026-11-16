-- Conversation journal: replaces the game's JournalNPCWnd (the NPCs you have spoken with and what they said) and
-- JournalCatWnd (its categories). The journal: a tool row (keep a journal, search, the category filter, Save, Edit
-- Categories), the NPCs beside their lines, or above them when narrow, and the selected line's text with the
-- category to give it and Delete Entry. The categories: the list, the name and description to add or update, the
-- default category of new lines, Delete. Both lists select several rows with Shift / Ctrl and sort by a header click,
-- as the original's do; every list, combo, edit and button is the original's (app/lua_ui_journal.cpp), so what is
-- kept, sorted, deleted and saved is the game's.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local D = dofile("lib/dialogs_kit.lua")
local P = dofile("lib/progress.lua")
local T = hud.T
local C = T.color
local NPC = "JournalNPCWnd"
local CAT = "JournalCatWnd"

local function list(win, id, w, h, emptyText, emptyHint, columns)
  local rows = API.windows.ListInfo(win, id) or 0
  local x, y = ui.GetCursorScreenPos()
  W.List(win, id, w, h, { multi = true, headerClick = true, columns = columns })
  if rows == 0 then
    local _, lh = ui.CalcTextSize("Ag")
    local s = ui.GetIO().uiScale or 1
    T.Text(x + 8 * s, y + lh + 10 * s, emptyText, C.faint, T.font.small)
    if emptyHint then T.Text(x + 8 * s, y + 2 * lh + 12 * s, emptyHint, C.faint, T.font.small) end
  end
end

local OPTS = {
  size = function() return 720, 460 end,
  resizable = { 400, 300 },
}
local function body(x, y, w, h, s)
  local bh = D.ButtonH(s)
  local _, logging = W.State(NPC, "EnableButton")
  -- the tool row
  W.Check(NPC, "EnableButton", "Keep a journal", { tooltip = "Write what NPCs say to you into the journal" })
  ui.SameLine(0, 12 * s)
  W.Button(NPC, "SearchButton", nil, { tooltip = "Show only the NPCs who said a text (again: clear the search)" })
  ui.SameLine(0, 12 * s)
  K.Muted("Category")
  ui.SameLine()
  W.Combo(NPC, "CategoryFilter", { width = 150 * s })
  if ui.IsItemHovered() then ui.SetTooltip("Show only the NPCs with a line in this category") end
  ui.SameLine(0, 12 * s)
  local right = 0
  for _, b in ipairs({ "Save", "Edit Categories" }) do right = right + ui.CalcTextSize(b) + 16 * s end
  right = right + 8 * s
  local cx, cy = ui.GetCursorPos()
  local avail = ui.GetContentRegionAvail()
  if avail > right then ui.SetCursorPos(cx + avail - right, cy) end
  W.Button(NPC, "SaveButton", "Save", { tooltip = "Save the journal to disk now" })
  ui.SameLine()
  W.Button(NPC, "CategoryButton", "Edit Categories", { tooltip = "Open the category editor" })
  if not logging then
    K.Muted("The journal is off: tick \"Keep a journal\" to record what NPCs say.", C.faint)
  end
  ui.Dummy(0, 2 * s)

  local wide = w >= 620 * s
  -- the NPCs (beside their lines, or above them when narrow)
  local lw = wide and math.floor((w - 12 * s) * 0.40) or w
  local _, restH = ui.GetContentRegionAvail()
  local npcH = math.floor((restH or 300) * 0.36)
  ui.BeginGroup()
  K.Heading("People", nil)
  ui.BeginChild("##jnpcs", lw, wide and 0 or npcH, false)
  list(NPC, "NPCList", 0, -(bh + 6 * s), "Nobody yet.", logging and "Talk to an NPC to start." or nil,
    { [1] = { "NPC", 0 }, [2] = { "Zone", 0 }, [3] = { "Last spoke", 0 } })
  W.Button(NPC, "DeleteNPCButton", "Delete NPC", { width = -1, height = bh, danger = true,
    tooltip = "Delete the selected NPCs and everything they said" })
  ui.EndChild()
  ui.EndGroup()
  if wide then ui.SameLine(0, 12 * s) end
  -- what they said
  ui.BeginGroup()
  K.Heading("What they said", nil)
  ui.BeginChild("##jlines", 0, 0, false)
  local _, ah = ui.GetContentRegionAvail()
  local textH = math.max(80 * s, (ah - bh * 2 - 20 * s) * 0.45)
  list(NPC, "DialogList", 0, -(textH + bh + 14 * s), "Select an NPC to see their lines.", nil,
    { [1] = { "When", 150 }, [2] = { "Category", 80 }, [3] = { "Line", 0 } })
  D.TextBox(NPC, "Text", 0, textH, { empty = "Select a line to read it." })
  ui.Dummy(0, 2 * s)
  K.Muted("Category")
  if ui.IsItemHovered() then ui.SetTooltip("The category of the selected lines") end
  ui.SameLine()
  local dw = ui.CalcTextSize("Delete Entry") + 24 * s
  W.Combo(NPC, "CategorySelect", { width = math.max(80 * s, ui.GetContentRegionAvail() - dw - 8 * s) })
  if ui.IsItemHovered() then ui.SetTooltip("Give the selected lines this category") end
  ui.SameLine()
  W.Button(NPC, "DeleteTextButton", "Delete Entry", { width = -1, danger = true, tooltip = "Delete the selected lines" })
  ui.EndChild()
  ui.EndGroup()
end

local function testOpen() API.windows.Show(NPC, true) end
ui.window("Conversation Journal", function()
  P.TestOpen(NPC, testOpen) -- testing: test.open.JournalNPCWnd=1
  if not API.session.InGame() then return end
  hud.Dialog(NPC, "Conversation Journal", OPTS, body)
end)

local function testOpen2() API.windows.Show(CAT, true) end
local CAT_OPTS = {
  size = function() return 420, 400 end,
  resizable = { 300, 280 },
}
local function catBody(x, y, w, h, s)
  local bh = D.ButtonH(s)
  local _, lh = ui.CalcTextSize("Ag")
  local fieldsH = 3 * (lh + 12 * s) + 8 * s
  list(CAT, "CategoryList", 0, -(fieldsH + D.FooterH(s)), "No categories yet.", "Type a name below and Add it.",
    { [1] = { "Name", 110 }, [2] = { "Description", 0 } })
  ui.Dummy(0, 2 * s)
  local cw = math.max(ui.CalcTextSize("Description"), ui.CalcTextSize("New lines")) + 12 * s
  local function row(caption, tip)
    K.Muted(caption)
    if tip and ui.IsItemHovered() then ui.SetTooltip(tip) end
    ui.SameLine(cw)
  end
  row("Name")
  W.Edit(CAT, "NameInput", { hint = "Category name", max = 16 })
  row("Description")
  W.Edit(CAT, "DescInput", { hint = "What it is for", max = 256 })
  row("New lines", "The category NPC lines get as they are written")
  W.Combo(CAT, "DefaultChooser")
  if ui.IsItemHovered() then ui.SetTooltip("The category new journal lines get") end
  ui.Dummy(0, 2 * s)
  W.ButtonRow(CAT, {
    { "AddButton", "Add", { primary = true, tooltip = "Add a category with this name and description" } },
    { "UpdateButton", "Update", { tooltip = "Give the selected category this name and description" } },
    { "DeleteButton", "Delete", { danger = true, tooltip = "Delete the selected categories (their lines keep none)" } },
  }, bh)
end

ui.window("Journal Categories", function()
  P.TestOpen(CAT, testOpen2) -- testing: test.open.JournalCatWnd=1
  if not API.session.InGame() then return end
  hud.Dialog(CAT, "Journal Categories", CAT_OPTS, catBody)
end)
