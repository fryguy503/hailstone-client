-- The blocked spells windows (blockedbuff.lua: BlockedBuffWnd, blockedpetbuff.lua: BlockedPetBuffWnd) drawn alike:
-- a field for a spell's name or id with Add (the game's edit box and button: Add is enabled once the field names a
-- spell, Enter adds it too), then the blocked spells as a list, icon and name, each with a remove button (the
-- game's slot takes it as its right click, which unblocks the spell).
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color

local SLOTS = 30

return function(cfg) -- { window, title, slot (prefix), add, edit, who ("you" / "your pet") }
  ui.window(cfg.title, function()
    if not API.session.InGame() then return end
    hud.Dialog(cfg.window, cfg.title, {
      size = function() return 280, 360 end,
      resizable = { 220, 200 },
    }, function(x, y, w, h, s)
      -- the field and Add
      local _, lh = ui.CalcTextSize("Ag")
      local btnW = 64 * s
      local avail = ui.GetContentRegionAvail()
      W.Edit(cfg.window, cfg.edit, {
        width = avail - btnW - 8, hint = "Spell name or ID", max = 64,
        onCommit = function()
          if ui.IsKeyPressed(ui.Key.Enter) and API.windows.ChildState(cfg.window, cfg.add) ~= false then
            API.windows.ClickChild(cfg.window, cfg.add, "left")
          end
        end,
      })
      ui.SameLine()
      W.Button(cfg.window, cfg.add, "Add", { width = btnW, primary = true,
        tooltip = "Block the beneficial spell named in the field" })
      -- the blocked spells
      local list = {}
      for i = 0, SLOTS - 1 do
        local id = cfg.slot .. i
        local name = API.windows.ChildTooltip(cfg.window, id) or ""
        if name ~= "" then list[#list + 1] = { id = id, name = name } end
      end
      ui.Dummy(0, 2 * s)
      ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, string.format("%d of %d blocked", #list, SLOTS))
      ui.Separator()
      ui.BeginChild("##blocked", 0, 0, false)
      if #list == 0 then
        ui.Dummy(0, 4 * s)
        ui.PushTextWrapPos(0)
        ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1,
          "Nothing blocked. A spell blocked here will not land on " .. cfg.who .. ".")
        ui.PopTextWrapPos()
      end
      local rowH = 28 * s
      local cw = ui.GetContentRegionAvail()
      for _, b in ipairs(list) do
        local rx, ry = ui.GetCursorScreenPos()
        ui.PushID(b.id)
        ui.InvisibleButton("##row", cw, rowH)
        local hov = ui.IsItemHovered()
        if hov then ui.DrawRectFilled(rx, ry, rx + cw, ry + rowH, C.ink[1], C.ink[2], C.ink[3], 0.05, 5 * s) end
        local tex, u0, v0, u1, v1 = API.windows.ChildArt(cfg.window, b.id, "decal")
        if type(tex) ~= "userdata" then tex = nil end
        local isz = rowH - 4 * s
        T.Icon(rx + 2 * s, ry + 2 * s, isz, s, tex, u0, v0, u1, v1, nil, { radius = 4 })
        local _, th = T.TextSize("Ag", T.font.small)
        local xw = 22 * s
        local tx = rx + isz + 10 * s
        T.Text(tx, ry + (rowH - th) / 2, T.Fit(b.name, rx + cw - xw - 8 * s - tx, T.font.small), C.text, T.font.small)
        -- the remove button (the slot's right click)
        local bx0, by0 = rx + cw - xw - 2 * s, ry + (rowH - xw) / 2
        local removed = T.Hit("##x", bx0, by0, bx0 + xw, by0 + xw)
        local xh = ui.IsItemHovered()
        if hov or xh then
          local c = xh and C.danger or C.muted
          if xh then ui.DrawRectFilled(bx0, by0, bx0 + xw, by0 + xw, c[1], c[2], c[3], 0.18, 4 * s) end
          local m, d = 7 * s, xw - 7 * s
          ui.DrawLine(bx0 + m, by0 + m, bx0 + d, by0 + d, c[1], c[2], c[3], 1, 1.5 * s)
          ui.DrawLine(bx0 + d, by0 + m, bx0 + m, by0 + d, c[1], c[2], c[3], 1, 1.5 * s)
        end
        if xh then ui.SetTooltip("Unblock " .. b.name) elseif hov then ui.SetTooltip(b.name .. "\nRight-click to unblock") end
        ui.SetCursorScreenPos(rx, ry + rowH + 2 * s)
        ui.Dummy(0, 0)
        ui.PopID()
        if removed or (hov and ui.IsMouseClicked(ui.Mouse.Right) and not ui.GetIO().alt) then
          API.windows.ClickChild(cfg.window, b.id, "right")
          T.TakeRightClick()
        end
      end
      ui.EndChild()
    end)
  end)
end
