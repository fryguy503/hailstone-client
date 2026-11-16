-- A social's command lines for the editors (socialedit.lua, luasocial.lua): a line's parts as a press runs it
-- (hs.SocialLine: game::split_social_line), a line put back together from them, about how long the lines take, and
-- the lines to Lua and back.
--
-- A press runs one line a step (a step: just over 100 ms), then waits a step, or N steps after "/pause N". The
-- original skips only the comma after a prefix, so a second prefix follows it with no space: "/timer 600,/pause 10,
-- /assist" (with a space, ", /pause 10" would be the command).
local API = hs.UI
local L = {}

L.STEP = 0.1 -- seconds a step takes, about

-- The parts of a line: {pause, timer, timers, command, kind, name, raw}; `command` without the spaces the prefixes
-- leave before it. Kept per text: the editor asks every frame.
local cache, cached = {}, 0
function L.Parts(text, split)
  local p = cache[text]
  if p then return p end
  local r = (split or API.socials.Line)(text) or {}
  local cmd = r.command or text
  if r.pause or (r.timers and #r.timers > 0) then cmd = cmd:gsub("^%s+", "") end
  local timer = nil
  for _, t in ipairs(r.timers or {}) do
    if t > 0 then timer = t break end -- the first that starts the cooldown; a running one keeps later ones out
  end
  p = { pause = r.pause, timer = timer, timers = r.timers or {}, command = cmd, kind = r.kind or "empty",
        name = r.name, raw = text }
  if cached > 256 then cache, cached = {}, 0 end
  cache[text], cached = p, cached + 1
  return p
end

-- A line from its parts: "/timer T,/pause P, command". Nil or 0 leaves a prefix out.
function L.Compose(timer, pause, command)
  local pre = {}
  if timer and timer > 0 then pre[#pre + 1] = "/timer " .. math.floor(timer) end
  if pause and pause > 0 then pre[#pre + 1] = "/pause " .. math.floor(pause) end
  local head = table.concat(pre, ",")
  if head == "" then return command end
  if command == "" then return head end
  return head .. ", " .. command
end

-- The time from the press to each line that runs, in seconds, and the whole run's: {at = {[i] = s}, total, runs}.
function L.Timeline(lines, split)
  local at, t, runs, last = {}, 0, 0, nil
  for i = 1, #lines do
    if lines[i] ~= "" then
      if last then t = t + (last + 1) * L.STEP end
      local p = L.Parts(lines[i], split)
      at[i] = t
      if p.command ~= "" then runs = runs + 1 end
      last = p.pause or 1
    end
  end
  return { at = at, total = t, runs = runs }
end

-- The lines as a Lua social's press: each command an mq.cmd, a /pause after a line an mq.delay (a wait: a server in
-- full mode runs it), a /timer a comment (a Lua social shows a cooldown with its face).
function L.ToLua(lines, split)
  local out, n = {}, 0
  for i = 1, #lines do if lines[i] ~= "" then n = i end end
  for i = 1, n do
    if lines[i] ~= "" then
      local p = L.Parts(lines[i], split)
      if p.timer then
        out[#out + 1] = string.format("-- /timer %d: a %g s cooldown (a face can show one)", p.timer, p.timer / 10)
      end
      if p.command ~= "" then out[#out + 1] = string.format("mq.cmd(%q)", p.command) end
      if p.pause and i < n then
        out[#out + 1] = string.format("mq.delay(%d) -- /pause %d (waits need a server in full mode)", p.pause * 100,
          p.pause)
      end
    end
  end
  return table.concat(out, "\n")
end

-- A press made only of mq.cmd('...') calls and mq.delay(ms) waits (and comments), without a face, as at most five
-- lines; nil and why when it cannot be.
function L.FromLua(press, face, max)
  max = max or 5
  if (face or ""):match("%S") then return nil, "It has a face: command lines cannot show one." end
  local lines = {}
  for raw in (press .. "\n"):gmatch("(.-)\r?\n") do
    local l = raw:match("^%s*(.-)%s*$")
    if l ~= "" and l:sub(1, 2) ~= "--" then -- (blank lines and comments are left out)
      local ms = l:match("^mq%.delay%(%s*(%d+)%s*%)%s*;?%s*%-%-.*$") or l:match("^mq%.delay%(%s*(%d+)%s*%)%s*;?$")
      local q, cmd = l:match("^mq%.cmd%(%s*(['\"])(.-)%1%s*%)%s*;?$")
      if ms then
        local steps = math.max(1, math.min(600, math.floor(tonumber(ms) / 100 + 0.5)))
        if #lines == 0 then
          lines[1] = { pause = steps, command = "" }
        else
          lines[#lines].pause = steps
        end
      elseif q and not cmd:find("[\\'\"]") then -- (no quote or escape inside: the text is the command as it is)
        lines[#lines + 1] = { command = cmd }
      else
        return nil, "Only plain mq.cmd('...') and mq.delay(ms) lines can become command lines."
      end
    end
  end
  if #lines > max then return nil, string.format("It sends %d commands; a social has %d lines.", #lines, max) end
  local out = {}
  for i = 1, max do
    local p = lines[i]
    out[i] = p and L.Compose(nil, p.pause, p.command) or ""
    if #out[i] > 255 then return nil, "A line would be longer than 255 characters." end
  end
  return out
end

return L
