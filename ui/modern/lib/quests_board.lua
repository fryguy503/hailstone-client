-- A leaderboard page shared by the adventure and PvP leaderboards (adventureleaderboard.lua, pvpleaderboard.lua):
-- the drop-downs that pick the board (the original's combo boxes: a pick asks the server, as the original does),
-- your own line as tiles, the board as a sortable table with a name search (your row lit), and Refresh / Done.
-- Everything is the original window's children; the board's columns come from its list.
local W = dofile("lib/widgets.lua")
local K = dofile("lib/quests_kit.lua")
local T = dofile("lib/theme.lua")
local C = T.color

local B = {}
local search = {}

-- opts: win, list, nameCol (1-based), combos = { { id, caption, tooltip }, ... }, you = { { caption, id }, ... },
-- refresh, done (button ids), empty / emptyHint (text), s
function B.Draw(opts)
  local s = opts.s
  local win = opts.win
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  -- the board picked
  local n = #opts.combos
  local cw = (ui.GetContentRegionAvail() - 8 * s * (n - 1)) / n
  for i, c in ipairs(opts.combos) do
    if i > 1 then ui.SameLine(0, 8 * s) end
    ui.BeginGroup()
    K.Muted(c[2])
    W.Combo(win, c[1], { width = cw })
    ui.EndGroup()
    if c[3] and ui.IsItemHovered() then ui.SetTooltip(c[3]) end
  end
  -- your own line
  local me = W.Text(win, opts.meName)
  local tiles = {}
  for _, t in ipairs(opts.you) do
    local v = W.Text(win, t[2])
    tiles[#tiles + 1] = { t[1], v ~= "" and v or "-", t[3] or C.text }
  end
  ui.Dummy(0, 4 * s)
  K.Heading(me ~= "" and ("You: " .. me) or "You")
  K.StatRow(tiles, 80)
  ui.Dummy(0, 2 * s)
  -- the board
  local rows = eq.ListInfo(win, opts.list) or 0
  K.Heading("Leaders", rows == 1 and "1 entry" or (rows .. " entries"))
  local needle = K.Search(search, win, "Search names")
  if rows == 0 then
    ui.BeginChild("##boardempty" .. win, 0, -(btnH + 8 * s), false)
    K.Empty(opts.empty or "No one on this board yet.", opts.emptyHint or "Pick a board above, or Refresh.")
    ui.EndChild()
  else
    local col = opts.nameCol
    W.List(win, opts.list, 0, -(btnH + 8 * s), {
      sortable = true,
      filter = function(_, texts)
        return needle == "" or (texts[col] or ""):lower():find(needle, 1, true) ~= nil
      end,
      filterKey = needle, -- (the filter changes only with the search)
      onRow = me ~= "" and function(row, texts)
        -- your own row in gold
        if texts[col] ~= me then return false end
        for c = 1, #texts do
          if c > 1 then ui.TableNextColumn() end
          ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, texts[c] or "")
        end
        return true
      end or nil,
    })
  end
  W.ButtonRow(win, {
    { opts.refresh, "Refresh", { tooltip = "Ask the server for this board again" } },
    { opts.done, "Done" },
  }, btnH)
end

return B
