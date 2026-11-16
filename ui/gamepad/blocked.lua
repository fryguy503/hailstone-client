-- Blocked spells (gamepad pack): stand in for BlockedBuffWnd (the beneficial spells kept from landing on you) and
-- BlockedPetBuffWnd (on your pet), drawn alike. A field for a spell's name or id (the window's edit box, typed on
-- the on-screen keyboard) with Add (the game enables it once the field names a spell), then the blocked spells,
-- icon and name: A on one unblocks it, as the stock slot's right click does.
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local CH = dofile("lib/child.lua")
local C = T.C

local SLOTS = 30

local function blocked(cfg) -- { window, title, slot (prefix), add, edit, who }
  local st = CH.new()
  local listState = {}
  local wasShown = false
  ui.window(cfg.title, function()
    CH.testOpen(cfg.window)
    if not W.shown(cfg.window) then
      st.kb.open, wasShown = false, false
      return
    end
    T.frame()
    local appearing = not wasShown
    wasShown = true
    local list = {}
    for i = 0, SLOTS - 1 do
      local id = cfg.slot .. i
      local name = API.windows.ChildTooltip(cfg.window, id) or ""
      if name ~= "" then list[#list + 1] = { id = id, name = name } end
    end
    if W.begin(cfg.title, { w = 1200, h = 900, title = cfg.title,
          subtitle = string.format("%d of %d blocked: they will not land on %s", #list, SLOTS, cfg.who),
          hints = CH.hints(st, { { "a", "Edit / unblock" }, { "b", "Close" } }) }) then
      local x0, y0, x1, y1, cw = W.area()
      local wx, wy = ui.GetWindowPos()
      if not CH.modal(st, x0, y0, x1, y1) then
        local fh = T.px(80)
        ui.SetCursorPos(x0 - wx, y0 - wy)
        if appearing then ui.SetKeyboardFocusHere() end
        CH.field(st, cfg.window, cfg.edit, "Spell name or ID", cw - T.px(230), fh, { max = 64, keep = true,
          prompt = "The spell to block (name or ID)" })
        ui.SameLine(0, T.px(10))
        CH.button(cfg.window, cfg.add, "Add", T.px(220), fh, { primary = true, keep = true })
        local ly = y0 + fh + T.px(20)
        ui.SetCursorPos(x0 - wx, ly - wy)
        local rowH = 60
        local visible = math.max(1, math.floor((y1 - ly + T.px(10)) / (T.px(rowH) + T.px(10))))
        if #list == 0 then
          T.font(20)
          T.draw(x0, ly + T.px(10), "Nothing blocked.", C.faint)
          T.unfont()
          -- Record the list origin when the empty state only draws text.
          ui.Dummy(0, 0)
        else
          W.list(cfg.window, listState, #list, visible, rowH, cw, function(i, rx0, ry0, rx1, ry1)
            local b = list[i]
            local s = ry1 - ry0 - T.px(12)
            local tex, u0, v0, u1, v1 = API.windows.ChildArt(cfg.window, b.id, "decal")
            if type(tex) == "userdata" then T.icon(tex, u0, v0, u1, v1, rx0 + T.px(10), ry0 + T.px(6), rx0 + T.px(10) + s, ry0 + T.px(6) + s) end
            T.font(22)
            T.textIn(rx0 + s + T.px(26), ry0, rx1 - T.px(14), ry1, T.fit(b.name, rx1 - rx0 - s - T.px(40)), C.text, 0)
            T.unfont()
          end, function(i) API.windows.ClickChild(cfg.window, list[i].id, "right") end)
        end
      end
    end
    if CH.back(st, W.finish()) then API.windows.Show(cfg.window, false) end
  end)
end

blocked({ window = "BlockedBuffWnd", title = "Blocked Spells", slot = "BBuff", add = "BW_AddSpellToBlock",
  edit = "BW_EditSpellToBlock", who = "you" })
blocked({ window = "BlockedPetBuffWnd", title = "Blocked Pet Spells", slot = "BPBuff", add = "BW_AddSpellToBlockFromPet",
  edit = "BW_EditSpellToBlockFromPet", who = "your pet" })
