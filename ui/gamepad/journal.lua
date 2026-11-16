-- The conversation journal (gamepad pack), standing in for the stock windows and answered through them (lib/model.lua,
-- app/lua_ui_journal.cpp), so what is logged, kept, sorted and saved is the game's:
--   NPC Journal (JournalNPCWnd)        the NPCs who spoke to you (name, zone, the newest time) and what the one picked
--                                      said; A picks an NPC or an entry (its text shows on the right), X adds it to the
--                                      selection or takes it out (the lists select several, as ctrl-click does); the
--                                      journal on / off, Search (the game's text entry; again clears it), the
--                                      category filter, the selected entries' category, Delete NPC / Delete Entry
--                                      (asked first), Edit Categories and Save. A click on a column name sorts by it
--   Journal Categories (JournalCatWnd) the categories (A picks one: its name and description fill the fields, X adds
--                                      or removes it from the selection), the name and description typed on the
--                                      on-screen keyboard, the default category for new entries, Add, Update, Delete
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local M = dofile("lib/model.lua")
local K = dofile("lib/keyboard.lua")
local P = dofile("lib/textpanel.lua")
local C = T.C

-- ---------------------------------------------------------------- the NPC journal (JournalNPCWnd)
local JW = "JournalNPCWnd"
local npcs, entries, pick, state, text = {}, {}, {}, {}, { scroll = 0 }

ui.window("NPC Journal", function()
  M.testShow()
  if not W.shown(JW) then return end
  T.frame()
  local hints = pick.open and { { "a", "Choose" }, { "b", "Back" } }
    or { { "a", "Select" }, { "x", "Add to selection" }, { "b", "Close" } }
  local _, logging = M.state(JW, "EnableButton")
  if W.begin("NPC Journal", { w = 1800, h = 1000, title = "NPC Journal",
        subtitle = logging and "Logging what NPCs say to you" or "The journal is off", hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if not M.picker(pick, x0, y0, x1, y1) and not W.confirm(state) then
      local bh = T.px(76)
      -- along the top: the journal on / off, the search, the filter, the categories, Save
      local n, gap = 5, T.px(10)
      local bw = (x1 - x0 - (n - 1) * gap) / n
      ui.SetCursorPos(x0 - wx, y0 - wy)
      M.toggle(JW, "EnableButton", "Journal", bw, bh)
      ui.SameLine(0, gap)
      local _, searching = M.state(JW, "SearchButton")
      M.button(JW, "SearchButton", searching and "Clear search" or "Search", bw, bh,
        { sub = searching and M.text(JW, "SearchButton") or "Find words in the entries", align = 0 })
      ui.SameLine(0, gap)
      M.combo(JW, "CategoryFilter", "Show category", bw, bh, pick)
      ui.SameLine(0, gap)
      M.button(JW, "CategoryButton", "Edit Categories", bw, bh)
      ui.SameLine(0, gap)
      M.button(JW, "SaveButton", "Save", bw, bh)
      local top = y0 + bh + T.px(18)
      local midX = x0 + (x1 - x0) * 0.36
      -- the NPCs
      M.list(JW, "NPCList", npcs, x0, top, midX - T.px(20), y1 - bh - T.px(14),
        { multi = true, empty = logging and "No NPC has spoken to you yet." or "Turn the journal on to keep what NPCs say." })
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      local npcOn = M.state(JW, "DeleteNPCButton")
      if T.button("##jdelnpc", "Delete NPC", midX - T.px(20) - x0, bh, { disabled = not npcOn }, pad) then
        state.confirm = { "Delete the selected NPCs and all they said?", function() API.windows.ClickChild(JW, "DeleteNPCButton", "left") end }
      end
      -- what the NPC said, the entry's text, its category and Delete Entry
      local ly1 = top + (y1 - top) * 0.48
      M.list(JW, "DialogList", entries, midX, top, x1, ly1, { multi = true, empty = "Pick an NPC to read what they said." })
      local ty = ly1 + T.px(14)
      local raw = M.text(JW, "Text")
      if text.raw ~= raw then text.raw, text.scroll = raw, 0 end
      P.draw("##jtext", text, raw, midX, ty, x1, y1 - bh - T.px(14), { empty = "Pick an entry to read it." })
      local half = (x1 - midX - gap) / 2
      ui.SetCursorPos(midX - wx, y1 - bh - wy)
      M.combo(JW, "CategorySelect", "Category of the selected entries", half, bh, pick)
      ui.SameLine(0, gap)
      local entryOn = M.state(JW, "DeleteTextButton")
      if T.button("##jdelentry", "Delete Entry", half, bh, { disabled = not entryOn }, pad) then
        state.confirm = { "Delete the selected entries?", function() API.windows.ClickChild(JW, "DeleteTextButton", "left") end }
      end
    end
  end
  if M.back(W.finish(), pick, state, nil, npcs) then API.windows.Show(JW, false) end
end)

-- ---------------------------------------------------------------- the categories (JournalCatWnd)
local CW = "JournalCatWnd"
local cats, cpick, kb = {}, {}, K.new()
local editing -- the edit child the keyboard types into

ui.window("Journal Categories", function()
  if not W.shown(CW) then
    kb.open = false
    return
  end
  T.frame()
  local hints = kb.open and K.HINTS or cpick.open and { { "a", "Choose" }, { "b", "Back" } }
    or { { "a", "Select" }, { "x", "Add to selection" }, { "b", "Close" } }
  if W.begin("Journal Categories", { w = 1500, h = 900, title = "Journal Categories", hints = hints }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    if kb.open then
      ui.SetCursorPos(x0 - wx, y0 - wy)
      local r = K.draw(kb, pad, x0, y0, math.min(x1 - x0, T.px(1100)))
      if r == "ok" and editing then API.windows.SetText(CW, editing, kb.text) end
    elseif not M.picker(cpick, x0, y0, x1, y1) then
      local midX = x0 + (x1 - x0) * 0.5
      local bh = T.px(76)
      M.list(CW, "CategoryList", cats, x0, y0, midX - T.px(20), y1 - bh - T.px(14),
        { multi = true, empty = "No categories yet: type a name on the right, then Add." })
      ui.SetCursorPos(x0 - wx, y1 - bh - wy)
      M.button(CW, "DeleteButton", "Delete", midX - T.px(20) - x0, bh)
      -- the fields and the buttons
      local cw = x1 - midX
      ui.SetCursorPos(midX - wx, y0 - wy)
      ui.BeginGroup()
      local name, desc = M.text(CW, "NameInput"), M.text(CW, "DescInput")
      if T.button("##jcname", "Name", cw, bh, { sub = name ~= "" and name or "(type a name)", align = 0 }, pad) then
        editing = "NameInput"
        K.open(kb, "The category's name", name, 15, true)
      end
      if T.button("##jcdesc", "Description", cw, bh, { sub = desc ~= "" and desc or "(none)", align = 0 }, pad) then
        editing = "DescInput"
        K.open(kb, "The category's description", desc, 255, true)
      end
      M.button(CW, "AddButton", "Add", cw, bh, { primary = true, sub = "A new category with this name", align = 0 })
      M.button(CW, "UpdateButton", "Update", cw, bh, { sub = "The selected one gets this name and description", align = 0 })
      T.heading("New entries")
      M.combo(CW, "DefaultChooser", "Default category", cw, bh, cpick)
      ui.EndGroup()
    end
  end
  local close = W.finish()
  if kb.open and close then
    kb.open = false
  elseif M.back(close, cpick, nil, nil, cats) then
    API.windows.Show(CW, false)
  end
end)
