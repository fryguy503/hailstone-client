-- The Individual Sounds volumes (Hailstone options, audio/individual_sounds.h): the rules both the window of the
-- modern Options and the gamepad's Sounds page follow. Plain Lua, no hs or ui calls, so it can be tested alone.
-- Each volume is a client option by name (hs.Option / hs.SetOption), 0..100, in steps of 5 on a slider or a pad.
local M = {}

M.MIN, M.MAX, M.STEP, M.FULL = 0, 100, 5, 100
local ROUND_HALF = 0.5 -- nearest step, not the one below

-- { option name, label, tooltip } in the order they are listed.
M.SOUNDS = {
  { "own_footsteps", "Own footsteps", "The steps, jumps and landings of your own character." },
  { "pet_footsteps", "Pet footsteps", "The steps of pets (an NPC that has an owner)." },
  { "other_footsteps", "Other footsteps", "The steps of other NPCs and other players." },
  { "open_bag", "Opening a bag", "The sound of a bag opening." },
  { "open_bank", "Opening a bank bag", "The sound of a bag opening when it sits in a bank slot." },
  { "money", "Money", "The coin sound of buying, selling, looting and moving coin." },
  { "thunder", "Thunder", "The thunder of a storm." },
  { "equip", "Equipping items", "The sound of putting on or taking off an item." },
  { "creature_ambient", "Creature ambient sounds",
    "The sounds some creatures make all the time, such as a spectre's hum." },
}

-- A volume brought into 0..100 (whole numbers).
function M.clamp(v) return math.max(M.MIN, math.min(M.MAX, math.floor(v))) end

-- A slider's value on the nearest step.
function M.snap(v) return M.clamp(math.floor(v / M.STEP + ROUND_HALF) * M.STEP) end

-- One step up (dir 1) or down (-1) from a volume, as the pad's left and right do.
function M.step(v, dir) return M.clamp(v + dir * M.STEP) end

-- What a number box holds: the volume it means (clamped), or nil while it is not a number yet (empty).
function M.parse(text)
  local n = tonumber(text)
  if n == nil then return nil end
  return M.clamp(n)
end

-- Every volume the client has back to full: option(name) reads, setOption(name, value) writes.
function M.reset(option, setOption)
  for _, entry in ipairs(M.SOUNDS) do
    if option(entry[1]) ~= nil then setOption(entry[1], M.FULL) end
  end
end

-- Whether a mouse release belongs to a row: the mouse is over the row's rectangle (geo.rowX0..rowX1, rowY0..rowY1).
-- A release elsewhere, in the same frame as the pad's A, is not a click on this row.
function M.overRow(geo, mouseX, mouseY)
  return geo ~= nil and geo.rowX0 ~= nil and mouseX >= geo.rowX0 and mouseX <= geo.rowX1
    and mouseY >= geo.rowY0 and mouseY <= geo.rowY1
end

-- Where on a row a mouse click was, once ImGui has said the row was clicked (the row's button is the only item, so
-- windows on top are respected). `geo`: x positions of the targets drawn on it (chipX0..chipX1, leftX, rightX);
-- `half`: how far either side of an arrow still counts. Returns "chip", "left", "right" or nil (the body).
function M.clickTarget(geo, mouseX, half)
  if not geo then return nil end
  if geo.chipX0 and mouseX >= geo.chipX0 and mouseX <= geo.chipX1 then return "chip" end
  if geo.leftX and math.abs(mouseX - geo.leftX) <= half then return "left" end
  if geo.rightX and math.abs(mouseX - geo.rightX) <= half then return "right" end
  return nil
end

-- What one click of a row does (W.list calls its callback once per click of the row's button). `source` "pad" (A) or
-- "mouse"; `target` from clickTarget. A row with a Test chip is tested by A or its chip only: a click on its body
-- changes nothing. Returns "test", "press", "step" (one up), "less" (one down) or nil.
function M.activation(row, source, target)
  if source == "mouse" then
    if row.test and target == "chip" then return "test" end
    if row[3] and target == "left" then return "less" end
    if row[3] and target == "right" then return "step" end
    if row.test then return nil end
  end
  if row.test then return "test" end
  if row.press then return "press" end
  if row[3] then return "step" end
  return nil
end

-- Does the action: the row's own change function (+1 / -1), its press, or playTest(its option name).
function M.perform(row, action, playTest)
  if action == "test" then playTest(row.test)
  elseif action == "press" then row.press()
  elseif action == "step" then row[3](1)
  elseif action == "less" then row[3](-1) end
end

return M
