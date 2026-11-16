-- The menu a buff icon opens on a right click, shared by the buff trays (lib/bufftray.lua), the target window
-- (target.lua) and the pet window (lib/pet.lua): the spell's information (hs.ShowSpell), Add to Blocked Buffs (the
-- Blocked Buffs window's own Add on the spell's id: the server stops landing it) or Add to Blocked Pet Spells (the
-- Blocked Pet Buffs window's), and Remove (the game's own right click on that button) for the player's own buffs.
-- The buff is kept when the menu opens, so a buff that fades or a target that changes while it is open does not move
-- the menu to another spell.
local API = hs.UI
local M = {}
local picked = {} -- window -> the buff its menu is for: { id, spell, name, block, remove }

-- M.Open(window, b [, opts])   opts: block = "buff" or "pet" (which blocked list the menu offers), remove = true
-- (the player's own buff: the game's right click removes it)
function M.Open(window, b, opts)
  picked[window] = {
    id = b.id, spell = b.spell, name = b.name,
    block = opts and opts.block, remove = opts and opts.remove,
  }
  ui.OpenPopup("##buffmenu")
end

-- M.Hint(opts)   the tooltip's last line, for the same opts
function M.Hint(opts)
  if opts and opts.remove then return "Right-click for info, block or remove" end
  if opts and opts.block == "pet" then return "Right-click for info or block pet spells" end
  if opts and opts.block == "buff" then return "Right-click for info or block" end
  return "Right-click for info"
end

-- M.Draw(window)   the menu, while open (inside the window's frame)
function M.Draw(window)
  if not ui.BeginPopup("##buffmenu") then return end
  local b = picked[window]
  if b then
    ui.TextDisabled(b.name)
    ui.Separator()
    if ui.MenuItem("Spell Info") then API.spells.Show(b.spell) end
    if b.block == "buff" then
      if ui.MenuItem("Add to Blocked Buffs") then
        API.windows.SetText("BlockedBuffWnd", "BW_EditSpellToBlock", tostring(b.spell))
        API.windows.ClickChild("BlockedBuffWnd", "BW_AddSpellToBlock", "left")
      end
    elseif b.block == "pet" then
      if ui.MenuItem("Add to Blocked Pet Spells") then
        API.windows.SetText("BlockedPetBuffWnd", "BW_EditSpellToBlockFromPet", tostring(b.spell))
        API.windows.ClickChild("BlockedPetBuffWnd", "BW_AddSpellToBlockFromPet", "left")
      end
    end
    if b.remove and ui.MenuItem("Remove") and API.buffs.Slot(window, b.id) == b.spell then -- (still in that slot)
      API.windows.Button(window, b.id, 3)
    end
  end
  ui.EndPopup()
end

return M
