-- Character select: the account's characters in this UI's look (hs.CharacterSelect, app/lua_ui_pregame.cpp; the
-- client's screen, as ui/dev_screens.cpp character_select_screen draws it, not CCharacterListWnd). With the 3D scene
-- up the picked character's model stands in the middle of the screen, so the card sits at the left; without it the
-- card is centred. Each character is a row (name, level and class, race, zone); a click picks it (the scene shows
-- that model), a double click or Enter World enters the world. Create Character, Delete (asked first), Return Home
-- (asked first; only when the server allows it) and Back (the login screen: the client logs out) are under the list.
--
-- The keyboard is the client's key binds' here (app/client_app_input.cpp screen_keys: Enter World, next and
-- previous character, quit), so the card does not take Enter or the arrows itself: a second Enter World would go.
local API = hs.UI
local P = dofile("lib/pregame.lua")
local T = P.T
local C = T.color

local function card(view, s, bh)
  local title = view.world ~= "" and view.world or "Characters"
  P.Title(title, view.server ~= "" and view.server or nil, s)
  local picked = view.characters[view.selected]
  ui.BeginDisabled(view.busy)
  if #view.characters == 0 then
    ui.Dummy(0, 10 * s)
    P.Muted("No characters on this account yet.")
    P.Faint("Create one to start playing on " .. title .. ".")
    ui.Dummy(0, 10 * s)
    if P.Primary("Create Character", -1, bh * 1.2, not view.busy) then API.pregame.CharacterSelectDo("create") end
  else
    local _, lh = ui.CalcTextSize("Ag")
    local rowH = lh * 2 + 12 * s
    local shown = math.min(#view.characters, view.scene and 8 or 10)
    ui.PushStyleVar(ui.StyleVar.ItemSpacing, 8 * s, 3 * s)
    ui.BeginChild("##characters", 0, shown * (rowH + 3 * s), false)
    for i, c in ipairs(view.characters) do
      ui.PushID(i)
      local clicked, double, x0, y0, x1 = P.Row("##char", view.selected == i, rowH, s)
      if clicked and view.selected ~= i then API.pregame.CharacterSelectDo("select", i) end
      if double then API.pregame.CharacterSelectDo("enter", i) end
      if ui.IsItemHovered() and c.className ~= "" then
        ui.SetTooltip(string.format("%s\nLevel %d %s %s", c.name, c.level, c.raceName, c.className))
      end
      local tx = x0 + 12 * s
      T.Text(tx, y0 + 5 * s, c.name, C.text)
      -- (kept on the row: the view stays the same table while nothing in it changes)
      c.line = c.line or string.format("Level %d %s  \194\183  %s", c.level, c.classShort, c.raceName)
      T.Text(tx, y0 + 6 * s + lh, c.line, C.muted, T.font.small)
      if c.zone ~= "" then
        local zone = T.Fit(c.zone, (x1 - x0) * 0.45, T.font.small)
        T.Text(x1 - 10 * s, y0 + 6 * s + lh, zone, C.faint, T.font.small, "right")
      end
      ui.PopID()
    end
    ui.EndChild()
    ui.PopStyleVar()
    ui.Dummy(0, 2 * s)
    if P.Primary(picked and ("Enter World as " .. picked.name) or "Enter World", -1, bh * 1.2, picked ~= nil) then
      API.pregame.CharacterSelectDo("enter")
    end
    local half = (ui.GetContentRegionAvail() - 8 * s) / 2
    if ui.Button("Create Character", half, 0) then API.pregame.CharacterSelectDo("create") end
    ui.SameLine()
    if P.Danger("Delete", half, 0, picked ~= nil) then ui.OpenPopup("##deletecharacter") end
    -- Return Home: only when the server lets this character go home (asked first)
    ui.BeginDisabled(not (picked and picked.canGoHome))
    if ui.Button("Return Home", -1, 0) then ui.OpenPopup("##returnhome") end
    ui.EndDisabled()
    P.Tip("Enter the world in the character's home city")
  end
  ui.EndDisabled()

  ui.Separator()
  P.Status(view.status, view.busy, s)
  if ui.Button("Back") then API.pregame.CharacterSelectDo("back") end
  P.Tip("Log out, back to the login screen")

  if picked and P.Confirm("##deletecharacter", string.format("Delete %s permanently?", picked.name),
      "The server removes the character; this cannot be undone.", "Delete", s) then
    API.pregame.CharacterSelectDo("delete")
  end
  if picked and picked.canGoHome and P.Confirm("##returnhome", picked.homeQuestion, nil, "Return Home", s) then
    API.pregame.CharacterSelectDo("home")
  end
end

local held = nil -- the view drawn last frame: hs.CharacterSelect gives true while it still holds (its revision)

ui.window("Character Select", function()
  local view = API.pregame.CharacterSelect(held and held.revision)
  if view == true then view = held else held = view end
  if not view then return end
  local s = P.Scale()
  local io = ui.GetIO()
  local _, lh = ui.CalcTextSize("Ag")
  local bh = lh + 14 * s
  local w = (view.scene and 360 or 460) * s
  local _, h = P.Size("charselect", w, 460 * s)
  local x = view.scene and 16 * s or (io.width - w) / 2
  local failure
  if P.Begin("charselect", x, math.max(16 * s, (io.height - h) / 2), w, s) then
    local ok, err = pcall(card, view, s, bh)
    if not ok then failure = err end
  end
  P.End()
  if failure then error(failure, 0) end
end)
