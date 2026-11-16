-- The address book and the ignore list (gamepad pack): stand in for MailAddressBookWindow (the mail windows'
-- Address Book) and MailIgnoreListWindow (the book's Ignore List), the same window over the two books the game
-- keeps. The names on the left (the book's entries, then the built-in guild and group lists in grey): A shows one
-- in the fields, X adds it to an open message's To field (the list's own right click). The fields on the right
-- are the window's edit boxes, typed with the on-screen keyboard: Name, Addresses (names separated by commas),
-- Private and Note; then Save (the window's Add / Modify), Delete, New and, in the address book, Ignore List.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local C = T.C

local function book(WIN, title, empty)
  local st = CH.new()
  local listState = {}
  local side = {}
  local wasShown = false
  ui.window(title, function()
    CH.testOpen(WIN)
    if not W.shown(WIN) then
      st.kb.open, st.pick, wasShown = false, nil, false
      return
    end
    T.frame()
    if not wasShown then listState.focusNext = 1 end
    wasShown = true
    local compose = API.windows.Shown("MailCompositionWindow") -- (an open message: X adds to its To)
    local hints = { { "a", "Show" }, { "b", "Close" } }
    if compose then table.insert(hints, 2, { "x", "Add to To" }) end
    if W.begin(title, { w = 1600, h = 940, title = title, hints = CH.hints(st, hints) }) then
      local x0, y0, x1, y1, cw = W.area()
      local wx, wy = ui.GetWindowPos()
      if not CH.modal(st, x0, y0, x1, y1) then
        local listW = cw * 0.38
        ui.SetCursorPos(x0 - wx, y0 - wy)
        local row = CH.list(WIN, "NameList", listState, listW, y1 - y0, { rowH = 58, empty = empty })
        if row and compose and W.pressed("x") then API.windows.ListClick(WIN, "NameList", row, "right") end
        W.toSide(side, row)
        -- the selected entry's fields and the buttons
        local rx0 = x0 + listW + T.px(24)
        local rw = x1 - rx0
        local fh = T.px(80)
        local y = y0
        local label = CH.text(WIN, "AddModifyButton")
        local ignore = CH.shown(WIN, "IgnoreListButton") -- the address book's only
        local items = {
          function(w) return CH.field(st, WIN, "NameField", "Name (what you type in To)", w, fh, { max = 64, keep = true }) end,
          function(w) return CH.field(st, WIN, "FieldOfAddresses", "Addresses (names, separated by commas)", w, fh, { max = 1024, keep = true }) end,
          function(w) return CH.toggle(WIN, "NameFieldIsPrivateCheckbox", "Private", w, fh, { on = "Hidden from the recipients", off = "The recipients see the list's name", keep = true }) end,
          function(w) return CH.field(st, WIN, "NoteField", "Note", w, fh, { max = 256, keep = true }) end,
        }
        local n = #items + 1 + (ignore and 1 or 0)
        for k, f in ipairs(items) do
          ui.SetCursorPos(rx0 - wx, y - wy)
          W.sideBefore(side, k)
          f(rw)
          W.sideAfter(side, k, n, listState)
          y = y + fh + T.px(12)
        end
        -- Save / Delete / New on one row
        y = y + T.px(10)
        local bh = T.px(72)
        local bw = (rw - 2 * T.px(10)) / 3
        ui.SetCursorPos(rx0 - wx, y - wy)
        W.sideBefore(side, #items + 1)
        CH.button(WIN, "AddModifyButton", label == "Modify" and "Save" or "Add", bw, bh, { primary = true, keep = true })
        W.sideAfter(side, #items + 1, n, listState)
        ui.SameLine(0, T.px(10))
        CH.button(WIN, "DeleteButton", "Delete", bw, bh, { keep = true })
        ui.SameLine(0, T.px(10))
        CH.button(WIN, "ClearButton", "New", bw, bh, { sub = "Empty the fields", align = 0, keep = true })
        if ignore then
          y = y + bh + T.px(12)
          ui.SetCursorPos(rx0 - wx, y - wy)
          W.sideBefore(side, n)
          CH.button(WIN, "IgnoreListButton", "Ignore List", rw, bh, { sub = "Names whose mail you do not want", align = 0 })
          W.sideAfter(side, n, n, listState)
        end
      end
    end
    if CH.back(st, W.finish()) then API.windows.Show(WIN, false) end
  end)
end

book("MailAddressBookWindow", "Address Book", "No entries yet: fill in the fields and Add.")
book("MailIgnoreListWindow", "Ignore List", "Nobody is ignored.")
