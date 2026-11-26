-- The structure of popup STML. Each pack carries an identical copy of this file (ui/modern/lib and ui/gamepad/lib)
-- for its own renderer; change both together. The cells and surrounding text stay as STML for each pack to style and
-- wrap.
--
-- The subset read, of what RoF2's CStmlWnd::ParseSTMLTable @0x8826a0 reads: <table> holding <tr> rows of <td> or
-- <th> cells (a <th> is read as a <td>), with any attributes on them ignored, and <thead>, <tbody> and <tfoot>
-- skipped (EQEmu's #npcstats sends <tbody>). A table outside it stays in the text around it, so its contents still
-- show: an unclosed cell or row, text between the cells, a nested table, more than 64 cells in a row, and what is not
-- read yet (<caption>, <colgroup>, <col>, rowspan, colspan).
--
-- One pass over the text: a popup can be 8 MB, and a hostile one costs time in proportion to its length.
local M = {}

local MAX_CELLS = 64
local TABLE_TAGS = { table = true, tr = true, td = true, th = true, thead = true, tbody = true, tfoot = true }
local SECTIONS = { thead = true, tbody = true, tfoot = true }

function M.parse(raw)
  local tags = raw:lower()
  local blocks, textStart = {}, 1
  -- The table being read: where it starts, its rows so far, the row and cell open, the end of its last tag.
  local first, rows, columns, row, cellStart, last
  -- A tag's attributes stop at the next < or >, so no tag is scanned past where the next one starts.
  for at, slash, name, rest, after in tags:gmatch("()<(/?)(%a+)([^<>]*)>()") do
    if TABLE_TAGS[name] and (rest == "" or rest:find("^%s")) then
      local closing = slash == "/"
      if first then
        local ok
        if cellStart then
          ok = closing and (name == "td" or name == "th")
          if ok then
            row[#row + 1] = raw:sub(cellStart, at - 1)
            cellStart = nil
          end
        elseif select(2, tags:find("^%s*", last)) + 1 ~= at then
          ok = false -- text between the table's tags, outside its cells
        elseif SECTIONS[name] then
          ok = true
        elseif name == "tr" then
          ok = closing == (row ~= nil)
          if ok and closing then
            rows[#rows + 1] = row
            columns = math.max(columns, #row)
            row = nil
          elseif ok then
            row = {}
          end
        elseif name == "td" or name == "th" then
          ok = not closing and row ~= nil and #row < MAX_CELLS
          if ok then cellStart = after end
        else
          ok = closing and not row and columns > 0
          if ok then
            local text = raw:sub(textStart, first - 1)
            if text:find("%S") then blocks[#blocks + 1] = { text = text } end
            blocks[#blocks + 1] = { rows = rows, columns = columns }
            textStart = after
            first = nil
          end
        end
        -- An incomplete table stays in the surrounding text; a <table> that ended it is read next.
        if not ok then first = nil end
        last = after
      end
      if not first and name == "table" and not closing then
        first, rows, columns, row, cellStart, last = at, {}, 0, nil, nil, after
      end
    end
  end
  local text = raw:sub(textStart)
  if #blocks == 0 or text:find("%S") then blocks[#blocks + 1] = { text = text } end
  return blocks
end

return M
