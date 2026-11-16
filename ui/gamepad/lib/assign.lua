-- Putting something on a hot button with the pad (the mouse drags it onto a bar): a window calls
-- AS.start(label, pickUp, from). pickUp() puts the hotbutton on the cursor the way the stock window's drag or button
-- does (hs.PickUp, the AA window's Hotkey button, ...) and returns true; then the slot picker (slotpicker.lua) shows
-- the cross hotbar's sets and the hot bars and drops it on the chosen hot button as a click with a hotbutton on the
-- cursor does (hs.ClickHotButton). Cancelling lets go of it. `from` is the window to give the pad back to.
-- Returns false when nothing was picked up (the entry makes no hotbutton, or one is already on the cursor).
local API = hs.UI
local AS = {}

GPSTATE = GPSTATE or { drawn = {}, closeAsked = {} }

function AS.start(label, pickUp, from)
  if API.cursor.HoldingButton() or not pickUp() or not API.cursor.HoldingButton() then return false end
  GPSTATE.assign = { label = label, from = from }
  return true
end

-- The slot picker without a hotbutton: A on a slot opens the slot's own menu (the game's hotbutton menu, as the
-- right button held on it does: assign a social, a new one opening the social editor, a label, clear ...).
function AS.edit(from)
  if API.cursor.HoldingButton() then return false end
  GPSTATE.assign = { edit = true, label = "Hot Bars", from = from }
  return true
end

function AS.active() return GPSTATE.assign ~= nil end

-- Whether the window `name` (this pack's) may act on an A press now: not while another of the pack's windows is on
-- top of it (the slot picker, a confirmation) and not for a moment after that one closed, when the A that answered
-- it would otherwise also press the item the focus came back to. Call it once a frame per window, after W.begin.
local W = dofile("lib/window.lua")
local covered = {}
function AS.settled(name)
  local now = ui.GetTime()
  if W.newest() ~= name then covered[name] = now end
  return now - (covered[name] or -1) > 0.3
end

return AS
