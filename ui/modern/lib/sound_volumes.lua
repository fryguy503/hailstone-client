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
  { "open_bag", "Bags", "The sounds of opening and closing a carried bag." },
  { "open_bank", "Bank bags", "The sounds of opening and closing a bag in a bank or shared bank slot." },
  { "money", "Money", "The coin sound of buying, selling, looting and moving coin." },
  { "thunder", "Thunder", "The thunder of a storm." },
  { "equip", "Equipping items", "The sound of putting on or taking off an item." },
  { "creature_ambient", "Creature ambient sounds",
    "The sounds some creatures make all the time, such as a spectre's hum." },
  { "beep", "Beep", "The /beep command's sound, a script's alert (its sound: Beep sound)." },
}

-- The Beep sound's option (hs.Option "beep_sound": an index into hs.OptionChoices "beep_sound", the tone first, -1 a
-- sound this game folder does not have), drawn after the Beep volume.
M.BEEP_SOUND = "beep_sound"
-- The short list's names (hs.OptionChoices "beep_sound_featured"): the choices right after the tone; every other
-- sound file of the game folder follows them.
M.BEEP_SOUND_FEATURED = "beep_sound_featured"
M.BEEP_SOUND_LABEL = "Beep sound"
M.BEEP_SOUND_TIP = "What /beep plays: the tone, or one of your game folder's sounds. Test on the Beep row plays it."

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

return M
