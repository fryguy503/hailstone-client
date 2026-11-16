-- Opening windows the way the game does. The pack has no keys of its own: a window is opened by the same path its
-- key or the EQ menu takes, so the game decides what opens (this pack's window, or the C++ one where the pack has
-- none) and nothing behaves differently from the keyboard.
--
-- A.menu(text): runs the EQ menu's entry named `text` (uifiles/default/CascadeMenu.txt: "Inventory",
-- "Alternate Advancement", "Quest Journal", ...), wherever it sits in the menu, as clicking it would: the menu's
-- key command. A.toggle(screen): shows / hides a stock window this pack draws (UiApi::toggle, what its key does).
local API = hs.UI
local A = {}

local function search(level, text, depth)
  local menus = API.menus.Cascades()
  local m = menus[level]
  if not m then return false end
  for i, item in ipairs(m.items) do
    if item.kind == "command" and item.text == text then
      API.menus.CascadeClick(level, i)
      return true
    end
  end
  if depth <= 0 then return false end
  for i, item in ipairs(m.items) do
    if item.kind == "submenu" then
      API.menus.CascadeHover(level, i) -- opens it (and closes the other submenus of this menu)
      if search(level + 1, text, depth - 1) then return true end
    end
  end
  return false
end

-- Runs the EQ menu entry `text`; false when the menu has no such entry.
function A.menu(text)
  if not API.menus.Cascades then return false end
  local wasOpen = #API.menus.Cascades() > 0
  if not wasOpen then API.windows.ClickChild("EQMainWnd", "EQButton", "left") end -- the EQ button opens the menu
  if #API.menus.Cascades() == 0 then return false end
  local ok = search(1, text, 3)
  if not ok and not wasOpen then API.menus.CascadeClose() end
  return ok
end

function A.toggle(screen) API.windows.Show(screen, not API.windows.Shown(screen)) end

-- The EQ menu entry, else the stock window: for windows of this pack (which the menu also reaches).
function A.open(text, screen)
  if A.menu(text) then return true end
  if screen then
    A.toggle(screen)
    return true
  end
  return false
end

return A
