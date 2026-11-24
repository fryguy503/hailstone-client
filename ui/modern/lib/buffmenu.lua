-- The menu a buff icon opens on a right click, shared by the buff trays (lib/bufftray.lua), the target window
-- (target.lua) and the pet window (lib/pet.lua): the spell's information (hs.ShowSpell), Add to Blocked Buffs (the
-- Blocked Buffs window's own Add on the spell's id: the server stops landing it) or Add to Blocked Pet Spells (the
-- Blocked Pet Buffs window's), and Remove for the player's own buffs and removable pet buffs.
-- Keep the selected spell, and for a pet its entity and server slot, while the menu is open.
local API = hs.UI
local M = {}
local picked = {} -- window -> the buff its menu is for

local function canRemove(opts, b)
  return opts and (opts.remove or (opts.block == "pet" and b and b.pet))
end

-- M.Open(window, b [, opts])   opts: block = "buff" or "pet", remove = true for player buffs,
-- windowOptions = true (open the tray's frame menu). A removable pet buff carries b.pet and b.slot.
function M.Open(window, b, opts)
  picked[window] = {
    id = b.id, spell = b.spell, name = b.name,
    block = opts and opts.block, remove = canRemove(opts, b), windowOptions = opts and opts.windowOptions,
    pet = b.pet, slot = b.slot,
  }
  ui.OpenPopup("##buffmenu")
end

-- M.Hint(opts)   the tooltip's last line, for the same opts
function M.Hint(opts, b)
  if canRemove(opts, b) then return "Right-click for info, block or remove" end
  if opts and opts.block == "pet" then return "Right-click for info or block pet spells" end
  if opts and opts.block == "buff" then return "Right-click for info or block" end
  return "Right-click for info"
end

local function stillInSlot(window, b)
  if not b.pet then return API.buffs.Slot(window, b.id) == b.spell end
  local spell, _, _, _, _, pet, slot = API.buffs.Slot(window, b.id, false, true)
  return spell == b.spell and pet == b.pet and slot == b.slot
end

-- M.Draw(window)   the menu, while open (inside the window's frame); returns true to open the frame menu
function M.Draw(window)
  if not ui.BeginPopup("##buffmenu") then return end
  local b = picked[window]
  local openOptions
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
    if b.remove and ui.MenuItem("Remove") and stillInSlot(window, b) then
      API.windows.Button(window, b.id, 3)
    end
    if b.windowOptions then
      ui.Separator()
      openOptions = ui.MenuItem("Window options")
    end
  end
  ui.EndPopup()
  return openOptions
end

return M
