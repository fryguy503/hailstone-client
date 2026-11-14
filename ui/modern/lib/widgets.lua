-- Modern controls bound to a game window's children (by ScreenID), for the windows this UI redraws inside a
-- hud.Dialog: they lay out in ImGui's flow (SameLine, columns, tables), so a dialog resized by its edges reflows.
-- Each asks the original child what it shows (eq.WindowText, eq.ChildState, eq.ListInfo, ...) and hands clicks to
-- it (eq.ClickChild, eq.ListClick, ...), as classic's lib/sidl.lua does for the stock layouts: the window's own
-- handler runs, so the mechanics are the game's.
--
--   local W = dofile("lib/widgets.lua")
--   if W.Button("MerchantWnd", "MW_Buy_Button", "Buy") then ... end   -- (the click already went to the game)
--   W.List("MerchantWnd", "MW_ItemList", 0, -40)                      -- fills the width, leaves 40 px below
local T = dofile("lib/theme.lua")
local C = T.color
local stml = dofile("lib/stml.lua")

local W = {}

-- ---------------------------------------------------------------- reading the original
function W.Shown(win, id) return eq.ChildShown(win, id) ~= false end

-- The child's text (a label, button or edit box), "" when it has none.
function W.Text(win, id)
  local t = eq.WindowText(win, id)
  return t or ""
end

-- enabled, checked (nil when the child has no state of its own)
function W.State(win, id)
  local e, c = eq.ChildState(win, id)
  if e == nil then return true, nil end
  return e, c
end

-- The child's text in its colour (the colour the window gives it, else `color`, else the normal text colour).
function W.Label(win, id, color, wrap)
  local text = W.Text(win, id)
  local r, g, b = eq.ChildColor(win, id)
  local c = r and { r, g, b, 1 } or color or C.text
  if wrap then ui.PushTextWrapPos(0) end
  ui.TextColored(c[1], c[2], c[3], c[4] or 1, text)
  if wrap then ui.PopTextWrapPos() end
  return text
end

-- A muted caption and a value on one line (the value from a child, or a string).
function W.Field(caption, value, color)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, caption)
  ui.SameLine()
  local c = color or C.text
  ui.TextColored(c[1], c[2], c[3], 1, value or "")
end

-- A small section heading with a hairline under it.
function W.Heading(text)
  ui.Dummy(0, 2)
  ui.TextColored(C.accent[1], C.accent[2], C.accent[3], 1, text)
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  ui.DrawLine(x, y, x + w, y, C.ink[1], C.ink[2], C.ink[3], 0.08, 1)
  ui.Dummy(0, 3)
end

-- ---------------------------------------------------------------- buttons
-- A button for the child: its label (`label`, else the child's text), greyed and inert while the original is
-- disabled, lit while it is checked (a toggle). The click goes to the game; returns true when it did.
-- opts: width, height (0: fit), primary, tooltip, hidden (draw nothing when the original is hidden), danger.
function W.Button(win, id, label, opts)
  opts = opts or {}
  if opts.hidden ~= false and eq.ChildShown(win, id) == false then return false end
  local enabled, checked = W.State(win, id)
  label = label or W.Text(win, id)
  local base = (opts.danger and C.danger) or ((opts.primary or checked) and C.accent) or nil
  local n = 0
  if base then
    ui.PushStyleColor(ui.Col.Button, base[1], base[2], base[3], checked and 0.40 or 0.24)
    ui.PushStyleColor(ui.Col.ButtonHovered, base[1], base[2], base[3], 0.40)
    ui.PushStyleColor(ui.Col.ButtonActive, base[1], base[2], base[3], 0.55)
    n = 3
  end
  ui.BeginDisabled(not enabled)
  local clicked = ui.Button((label ~= "" and label or id) .. "##" .. id, opts.width or 0, opts.height or 0)
  ui.EndDisabled()
  if n > 0 then ui.PopStyleColor(n) end
  if opts.tooltip and ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then ui.SetTooltip(opts.tooltip) end
  if clicked and enabled then
    eq.ClickChild(win, id, "left")
    return true
  end
  return false
end

-- A check box for a check-box child (its checked state is the original's; a click toggles the original).
function W.Check(win, id, label, opts)
  opts = opts or {}
  if opts.hidden ~= false and eq.ChildShown(win, id) == false then return false end
  local enabled, checked = W.State(win, id)
  ui.BeginDisabled(not enabled)
  local _, changed = ui.Checkbox((label or W.Text(win, id)) .. "##" .. id, checked == true)
  ui.EndDisabled()
  if opts.tooltip and ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then ui.SetTooltip(opts.tooltip) end
  if changed and enabled then
    eq.ClickChild(win, id, "left")
    return true
  end
  return false
end

-- A row of buttons that share the width evenly: { { id, label [, opts] }, ... }; returns the id clicked. A button
-- the window hides is left out, unless its opts say hidden = false (a stock template this UI draws as a button).
-- The caller's opts are not written to: each button gets a copy with the row's width and height (an opts table kept
-- by the caller keeps its own height, so a later call without `height` does not inherit an earlier one's).
local rowShown, rowOpts = {}, {} -- (reused: nothing is kept between calls)
function W.ButtonRow(win, list, height)
  local n = 0
  for _, b in ipairs(list) do
    if eq.ChildShown(win, b[1]) ~= false or (b[3] and b[3].hidden == false) then
      n = n + 1
      rowShown[n] = b
    end
  end
  for i = #rowShown, n + 1, -1 do rowShown[i] = nil end
  if n == 0 then return nil end
  local avail = ui.GetContentRegionAvail()
  local gap = 8 -- ImGui's item spacing
  local bw = math.max(40, (avail - gap * (n - 1)) / n)
  local hit = nil
  for i = 1, n do
    local b = rowShown[i]
    if i > 1 then ui.SameLine() end
    for k in pairs(rowOpts) do rowOpts[k] = nil end
    if b[3] then for k, v in pairs(b[3]) do rowOpts[k] = v end end
    rowOpts.width, rowOpts.height = bw, height or rowOpts.height
    if W.Button(win, b[1], b[2], rowOpts) then hit = b[1] end
  end
  for i = 1, n do rowShown[i] = nil end
  return hit
end

-- ---------------------------------------------------------------- fields
-- An edit box for an edit child: typing changes the original's text (the window's buttons use what was typed).
-- opts: width (default: the rest of the line), hint, multiline (with height), password, onCommit(text).
-- Returns text, committed.
function W.Edit(win, id, opts)
  opts = opts or {}
  local enabled = eq.ChildState(win, id) ~= false
  ui.SetNextItemWidth(opts.width or -1)
  ui.BeginDisabled(not enabled)
  local current = W.Text(win, id)
  local text, changed
  if opts.multiline and ui.InputTextMultiline then
    text, changed = ui.InputTextMultiline("##" .. id, current, opts.max or 4096, opts.width or -1, opts.height or 80)
  elseif opts.hint and ui.InputTextWithHint then
    text, changed = ui.InputTextWithHint("##" .. id, opts.hint, current, opts.max or 256)
  else
    text, changed = ui.InputText("##" .. id, current, opts.max or 256)
  end
  local committed = ui.IsItemDeactivatedAfterEdit()
  ui.EndDisabled()
  if changed then eq.SetWindowText(win, id, text) end
  if committed and opts.onCommit then opts.onCommit(text) end
  return text, committed
end

-- A drop-down for a combo child: picking a choice chooses it in the original. Returns the 0-based choice picked.
function W.Combo(win, id, opts)
  opts = opts or {}
  local choices, selected = eq.ComboInfo(win, id)
  if not choices then return nil end
  local enabled = eq.ChildState(win, id) ~= false
  ui.SetNextItemWidth(opts.width or -1)
  ui.BeginDisabled(not enabled)
  local picked = nil
  if ui.BeginCombo("##" .. id, choices[selected + 1] or "", ui.ComboFlags.HeightLarge) then
    for i, c in ipairs(choices) do
      local chosen = i - 1 == selected
      if ui.Selectable(c .. "##" .. i, chosen) and not chosen then
        eq.ComboSelect(win, id, i - 1)
        picked = i - 1
      end
      if chosen then ui.SetItemDefaultFocus() end
    end
    ui.EndCombo()
  end
  ui.EndDisabled()
  return picked
end

-- A slider for a slider child (its value and step count are the original's). opts: width, format(value, count).
function W.Slider(win, id, opts)
  opts = opts or {}
  local enabled, _, value, count = eq.ChildState(win, id)
  if not count or count < 1 then return nil end
  ui.SetNextItemWidth(opts.width or -1)
  ui.BeginDisabled(enabled == false)
  local fmt = opts.format and opts.format(value, count) or "%d"
  local v, changed = ui.SliderInt("##" .. id, value, 0, math.max(count - 1, 0), fmt)
  ui.EndDisabled()
  if changed and v ~= value then eq.SetSlider(win, id, v) end
  return v
end

-- A modern bar for a gauge child (its own value, else EQType `eqtype`). opts: color, height, text.
function W.Gauge(win, id, opts)
  opts = opts or {}
  local frac, text, r, g, b = eq.ChildGauge(win, id)
  if frac == nil and opts.eqtype then
    local _
    frac, text, _, r, g, b = eq.Gauge(opts.eqtype)
  end
  local x, y = ui.GetCursorScreenPos()
  local w = opts.width or ui.GetContentRegionAvail()
  local s = opts.s or (ui.GetIO().uiScale or 1)
  local h = (opts.height or 12) * s
  -- A special value (full and red, the purple or pink band) keeps the game's tint for its meaning.
  local col = r and { r, g, b, 1 } or opts.color or C.accent
  T.Bar(win .. "." .. id, x, y, x + w, y + h, s, frac or 0, col,
    { center = opts.text or ((text and text ~= "") and text or nil), font = T.font.tiny, noTrail = true })
  ui.Dummy(w, h)
  return frac
end

-- ---------------------------------------------------------------- lists
-- A list child as a modern table: its columns, headers and rows (eq.ListInfo / eq.ListRow), virtual (only the rows
-- in view are read), a click selects in the original in the column under the mouse (eq.ListClick), right-click is
-- the original's, and a drag picks the row up where the original lets it (eq.PickUp: an ability, a skill).
--   w, h: the child's size (0: the rest of the width, negative h: leave that much below)
--   opts.columns = { [n] = { label, width } }  replace a column's header or width (design pixels, 0 = stretch)
--   opts.hide = { [n] = true }   leave a column out
--   opts.onRow(row, texts) -> bool   draw the row's cells yourself (return true); else the texts are drawn
--   opts.filter(row, texts) -> bool  false leaves the row out (reads every row: short lists only)
--   opts.sortable = true         click a header to sort the view (reads every row: short lists only)
--   opts.doubleClick(row)        a double click on a row (else the double click goes to the original, eq.ListClick
--                                "double": the guild ranks' rename, the group finder's tell)
--   opts.headerClick = true      a click on a header goes to the original (eq.ListHeaderClick: the window sorts)
-- A column the original fills with combo boxes (texts.comboColumn) draws them: eq.ComboInfo / eq.ComboSelect on the
-- child "<list>:<row>".
-- A list that selects several rows (eq.ListInfo's sixth answer) shows each row it has selected; a click carries
-- Shift / Ctrl (1 / 2) as on the stock list, which such a list extends or toggles with.
-- Returns rows, selected (0-based, -1 none).
--
-- A sorted or filtered list keeps a copy of its rows' texts (lists rebuild their rows when asked, so reading every
-- row every frame costs), as lib/markets.lua's M.Rows does: read whole when the row count, the first or the last row
-- changes (or after a click on it), else a few rows a frame, and every row in view each frame; the sort's keys are
-- worked out and the rows sorted again only when the copy, or the sort, changes. opts.filterKey (any value that
-- changes when the filter would: the search text) lets the filtered order be kept too; without it the filter runs
-- over the copy each frame. The rows in view are always drawn from what the list answers now.
local listCache = {} -- [win][id] = { n, rows = { [row + 1] = texts or false }, next, version, dirty, sort, ... }
local LIST_READS = 8 -- rows read a frame besides the first, the last and those in view

local function sameTexts(a, b)
  if not a or not b then return (not a) == (not b) end
  local n = #a
  if n ~= #b then return false end
  for i = 1, n do if a[i] ~= b[i] then return false end end
  return true
end

-- The list's copy, brought up to date (see above).
local function listCopy(win, id, n, reread)
  local byId = listCache[win]
  if not byId then byId = {}; listCache[win] = byId end
  local c = byId[id]
  if not c then
    c = { n = -1, rows = {}, next = 0, version = 0 }
    byId[id] = c
  end
  local whole = c.n ~= n or c.dirty or reread
  if not whole and n > LIST_READS then -- (a shorter list is read whole below anyway)
    whole = not sameTexts(c.rows[1], eq.ListRow(win, id, 0) or false)
      or not sameTexts(c.rows[n], eq.ListRow(win, id, n - 1) or false)
  end
  if whole then
    local rows = c.rows
    for r = 0, n - 1 do rows[r + 1] = eq.ListRow(win, id, r) or false end
    for r = #rows, n + 1, -1 do rows[r] = nil end
    c.n, c.next, c.dirty, c.version = n, 0, false, c.version + 1
  elseif n > 0 then
    for _ = 1, math.min(n, LIST_READS) do
      local r = c.next
      local texts = eq.ListRow(win, id, r) or false
      if not sameTexts(c.rows[r + 1], texts) then
        c.rows[r + 1] = texts
        c.version = c.version + 1
      end
      c.next = (r + 1) % n
    end
  end
  return c
end

-- The copy's rows (0-based) in the sort's order: sorted again only when the copy or the sort changed. A number
-- (commas aside) sorts as one against another number, else the texts sort without case; ties keep the list's order.
local function listSorted(c)
  local st = c.sort
  if c.sorted and c.sortedVersion == c.version and c.sortedBy == st then return c.sorted end
  local sorted = {}
  for r = 0, c.n - 1 do if c.rows[r + 1] then sorted[#sorted + 1] = r end end
  if st then
    local col, asc = st[1], st[2]
    local num, low = {}, {}
    for _, r in ipairs(sorted) do
      local t = c.rows[r + 1][col] or ""
      num[r] = tonumber((t:gsub(",", "")))
      low[r] = t:lower()
    end
    table.sort(sorted, function(a, b)
      local na, nb = num[a], num[b]
      if na and nb then
        if na ~= nb then if asc then return na < nb else return na > nb end end
      else
        local x, y = low[a], low[b]
        if x ~= y then if asc then return x < y else return x > y end end
      end
      return a < b
    end)
  end
  c.sorted, c.sortedVersion, c.sortedBy, c.filterOk = sorted, c.version, st, false
  return sorted
end

-- The rows shown (0-based, in order): the sorted rows the filter keeps.
local function listOrder(c, opts)
  local sorted = listSorted(c)
  local filter = opts.filter
  if not filter then return sorted end
  local fkey = opts.filterKey
  if fkey ~= nil and c.filterOk and c.filterKey == fkey then return c.filtered end
  local out = c.filtered or {}
  local k = 0
  for _, r in ipairs(sorted) do
    if filter(r, c.rows[r + 1]) then
      k = k + 1
      out[k] = r
    end
  end
  for i = #out, k + 1, -1 do out[i] = nil end
  c.filtered, c.filterKey, c.filterOk = out, fkey, fkey ~= nil
  return out
end

-- One row of W.List (the row's texts read now; a sorted or filtered list's copy `c` is kept up to date with them).
local function listRow(win, id, row, selected, multi, shownCols, textH, s, opts, c)
  local texts, cr, cg, cb, tex, u0, v0, u1, v1 = eq.ListRow(win, id, row)
  if c and not sameTexts(c.rows[row + 1], texts or false) then
    c.rows[row + 1] = texts or false
    c.version = c.version + 1
  end
  if not texts then return end
  ui.TableNextRow()
  ui.TableNextColumn()
  ui.PushID(row)
  local column = ui.TableGetHoveredColumn()
  local comboAt = texts.comboColumn and texts.comboColumn + 1
  if comboAt then ui.SetNextItemAllowOverlap() end -- (the cell's combo box takes its clicks)
  local on = row == selected
  if multi then on = multi[row] == true end
  local picked = ui.Selectable("##row", on,
    ui.SelectableFlags.SpanAllColumns | (ui.SelectableFlags.AllowDoubleClick or 0), 0, textH)
  if T.DragFrom(win, id, row + 1) then picked = false end
  if picked then
    local col = shownCols[(column or 0) + 1] and shownCols[(column or 0) + 1] - 1 or column
    local io = ui.GetIO()
    eq.ListClick(win, id, row, "left", col, (io.shift and 1 or 0) | (io.ctrl and 2 or 0))
    if ui.IsMouseDoubleClicked(ui.Mouse.Left) then
      if opts.doubleClick then opts.doubleClick(row) else eq.ListClick(win, id, row, "double", col) end
    end
    if c then c.dirty = true end
  end
  if ui.IsItemClicked(ui.Mouse.Right) and (not ui.GetIO().alt or texts.altRight) then
    eq.ListClick(win, id, row, "right", column)
    if ui.GetIO().alt then T.TakeAltRightClick() else T.TakeRightClick() end
    if c then c.dirty = true end
  end
  ui.SameLine(0, 0)
  if not (opts.onRow and opts.onRow(row, texts, tex, u0, v0, u1, v1)) then
    for k, col in ipairs(shownCols) do
      if k > 1 then ui.TableNextColumn() end
      if k == 1 and tex then
        local x, y = ui.GetCursorScreenPos()
        ui.DrawImage(tex, x, y, x + textH, y + textH, u0, v0, u1, v1)
        ui.Dummy(textH, textH)
        ui.SameLine()
      end
      local cc = texts.colors and texts.colors[col]
      local choices, chosen
      if col == comboAt then choices, chosen = eq.ComboInfo(win, id .. ":" .. row) end
      if choices then
        -- the row's combo box (the original's CListWnd::SetItemWnd)
        local child = id .. ":" .. row
        ui.BeginDisabled(eq.ChildState(win, child) == false)
        ui.SetNextItemWidth(-1)
        ui.PushStyleVar(ui.StyleVar.FramePadding, 4 * s, 0)
        if ui.BeginCombo("##cell", choices[chosen + 1] or "", 0) then
          for i, ch in ipairs(choices) do
            local cur = i - 1 == chosen
            if ui.Selectable(ch .. "##" .. i, cur) and not cur then
              eq.ComboSelect(win, child, i - 1)
              if c then c.dirty = true end
            end
          end
          ui.EndCombo()
        end
        ui.PopStyleVar(1)
        ui.EndDisabled()
      elseif cc then ui.TextColored(cc[1], cc[2], cc[3], 1, texts[col] or "")
      else ui.TextColored(cr or 1, cg or 1, cb or 1, 1, texts[col] or "") end
    end
  end
  ui.PopID()
end

local NO_OPTS = {}
function W.List(win, id, w, h, opts)
  opts = opts or NO_OPTS
  local rows, columns, selected, labels, widths, multi = eq.ListInfo(win, id)
  if not rows then return 0, -1 end
  local s = ui.GetIO().uiScale or 1
  columns = math.max(columns, 1)
  local header = false
  for c = 1, columns do
    local o = opts.columns and opts.columns[c]
    if (o and o[1] or labels[c] or "") ~= "" then header = true end
  end
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingFixedFit
      | ui.TableFlags.BordersInnerV | ui.TableFlags.Resizable
  if opts.sortable and ui.TableFlags.Sortable then flags = flags | ui.TableFlags.Sortable end
  local shownCols = {}
  for c = 1, columns do if not (opts.hide and opts.hide[c]) then shownCols[#shownCols + 1] = c end end
  if #shownCols == 0 then shownCols = { 1 } end
  local outer = h or 0
  if not ui.BeginTable("##list" .. id, #shownCols, flags, w or 0, outer) then return rows, selected end
  ui.TableSetupScrollFreeze(0, header and 1 or 0)
  for _, c in ipairs(shownCols) do
    local o = opts.columns and opts.columns[c]
    local lbl = (o and o[1]) or labels[c] or ""
    local wpx = (o and o[2]) or widths[c] or 0
    if wpx > 0 then
      ui.TableSetupColumn(lbl .. "##c" .. c, ui.TableColumnFlags.WidthFixed, wpx * s)
    else
      ui.TableSetupColumn(lbl .. "##c" .. c, ui.TableColumnFlags.WidthStretch, 0)
    end
  end
  local headerClicked = false
  if header then
    if opts.headerClick and ui.TableHeadersRowClicked then
      local col = ui.TableHeadersRowClicked()
      if col >= 0 then
        eq.ListHeaderClick(win, id, shownCols[col + 1] and shownCols[col + 1] - 1 or col)
        headerClicked = true -- (the window may have sorted its rows)
      end
    else
      ui.TableHeadersRow()
    end
  end
  -- the order rows are shown in: the original's, or sorted / filtered when asked (from the copy)
  local copy = (opts.filter or opts.sortable) and listCopy(win, id, rows, headerClicked) or nil
  local order = nil
  if copy then
    if opts.sortable and ui.TableGetSortSpecs then
      local col, asc = ui.TableGetSortSpecs()
      if col then copy.sort = { shownCols[col + 1] or 1, asc } end
    end
    order = listOrder(copy, opts)
  end
  local count = order and #order or rows
  local _, textH = ui.CalcTextSize("Ag")
  do -- only the rows in view are read; the rest are spacer rows (a row is the text plus 2 px of cell padding a side)
    local lineH = textH + 4
    local headH = header and lineH or 0
    local first = math.max(0, math.floor((ui.GetScrollY() - headH) / lineH) - 1)
    local _, view = ui.GetWindowSize() -- the table's own scrolling child
    local last = math.min(count - 1, first + math.ceil(view / lineH) + 2)
    if first > 0 then ui.TableNextRow(); ui.TableNextColumn(); ui.Dummy(1, first * lineH - 4) end
    for i = first, last do
      listRow(win, id, order and order[i + 1] or i, selected, multi, shownCols, textH, s, opts, copy)
    end
    if last < count - 1 then ui.TableNextRow(); ui.TableNextColumn(); ui.Dummy(1, (count - 1 - last) * lineH - 4) end
  end
  ui.EndTable()
  return rows, selected
end

-- An STML child's text (wrapped, its colours and links; a link click goes to the window). h: the box's height
-- (0: the rest; negative: leave that much below).
local stmlCache = {}
function W.Stml(win, id, w, h)
  local raw = W.Text(win, id)
  local byId = stmlCache[win]
  if not byId then byId = {}; stmlCache[win] = byId end
  local c = byId[id]
  if not c or c.raw ~= raw then
    -- (the link handler is made with the lines, not once a line a frame)
    c = { raw = raw, lines = stml.Lines(raw, nil), onLink = function(link) eq.StmlLink(win, id, link[1], link[2]) end }
    byId[id] = c
  end
  ui.BeginChild("##stml" .. id, w or 0, h or 0, false)
  local width = ui.GetContentRegionAvail()
  for _, line in ipairs(c.lines) do stml.DrawLine(line, width, c.onLink) end
  ui.EndChild()
end

-- ---------------------------------------------------------------- item slots
-- An item slot child of a window (loot, trade, bank, merchant, ...) as a modern tile at the cursor: the item its
-- widget holds, the slot clicks the game takes (lib/items.lua), a gold ring while the window marks it selected.
-- size in screen pixels; placeholder: drawn in an empty slot.
local items = dofile("lib/items.lua")
local slotRecs = {} -- [win][id] = { key, onClick }: made the first time the slot draws, not each frame
local function slotRec(win, id)
  local byId = slotRecs[win]
  if not byId then byId = {}; slotRecs[win] = byId end
  local rec = byId[id]
  if not rec then
    rec = { key = "##slot" .. win .. id, onClick = function(button, outside) eq.ClickChild(win, id, button, outside) end }
    -- The item comparison of the slot's item (Hailstone: eq.CompareSlotItem), plain and with the selected ring.
    local compare = function() return eq.CompareSlotItem(win, id) end
    rec.opts, rec.selectedOpts = { compare = compare }, { selected = true, compare = compare }
    byId[id] = rec
  end
  return rec
end
function W.Slot(win, id, size, placeholder, where)
  local x, y = ui.GetCursorScreenPos()
  local s = ui.GetIO().uiScale or 1
  local _, checked = W.State(win, id)
  local rec = slotRec(win, id)
  items.Slot(rec.key, x, y, size, s, eq.SlotItem(win, id), placeholder, where or "", rec.onClick,
    checked and rec.selectedOpts or rec.opts)
  ui.SetCursorScreenPos(x, y)
  ui.Dummy(size, size)
end

local gridIds = {} -- [prefix][i] = prefix .. i
-- A grid of slot children named prefix..first .. prefix..last, `perRow` across (0: as many as fit).
function W.SlotGrid(win, prefix, first, last, size, perRow, gap)
  gap = gap or 4
  local avail = ui.GetContentRegionAvail()
  perRow = (perRow and perRow > 0) and perRow or math.max(1, math.floor((avail + gap) / (size + gap)))
  local n = 0
  local ids = gridIds[prefix]
  if not ids then ids = {}; gridIds[prefix] = ids end
  for i = first, last do
    local id = ids[i]
    if not id then id = prefix .. i; ids[i] = id end
    if eq.ChildShown(win, id) ~= false then
      if n % perRow ~= 0 then ui.SameLine(0, gap) end
      W.Slot(win, id, size)
      n = n + 1
    end
  end
  return n
end

-- ---------------------------------------------------------------- tabs
local tabShown = {} -- tab box -> the page shown last frame
-- Tabs for a tab-box child: `pages` = { { label, pageIndex (0-based) }, ... }; the page shown is the original's
-- (a click switches the original's page too, so what the game fills only on the visible page is filled).
-- Returns the 0-based page shown.
local testTab = {}
function W.Tabs(win, id, pages)
  local key = win .. "." .. id
  -- Testing: test.tab.<window>.<tab box id>=n in ui/settings/<name>.txt opens page n once (as classic's)
  if not testTab[key] then
    testTab[key] = true
    local t = tonumber(ui.GetSetting("test.tab." .. key) or "")
    if t then eq.SetTabPage(win, id, t) end
  end
  local current = eq.TabPage(win, id) or 0
  local shown = current
  local follow = tabShown[key] ~= current -- the game changed the page (or this is the first frame): select it
  if ui.BeginTabBar("##tabs" .. id) then
    for _, pg in ipairs(pages) do
      local flags = (follow and pg[2] == current) and ui.TabItemFlags.SetSelected or 0
      if ui.BeginTabItem(pg[1] .. "##" .. pg[2], flags) then
        if pg[2] ~= current and not follow then
          eq.SetTabPage(win, id, pg[2])
          current = pg[2]
        end
        shown = pg[2]
        ui.EndTabItem()
      end
    end
    ui.EndTabBar()
  end
  tabShown[key] = shown
  return shown
end

-- Coins (platinum, gold, silver, copper) as small coloured chips: { pp, gp, sp, cp }.
local COIN = { { "p", C.platinum }, { "g", C.gold }, { "s", C.silver }, { "c", C.copper } }
function W.Coins(values)
  local first = true
  for i, v in ipairs(values) do
    v = tonumber(v) or 0
    if v ~= 0 or i == 1 then
      if not first then ui.SameLine(0, 8) end
      first = false
      local c = COIN[i][2]
      ui.TextColored(C.text[1], C.text[2], C.text[3], 1, T.Thousands(v))
      ui.SameLine(0, 1)
      ui.TextColored(c[1], c[2], c[3], 1, COIN[i][1])
    end
  end
end

return W
