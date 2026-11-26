-- Item and achievement links show only their labels in the editor. Exact tags
-- stay in `links` and are restored for handoff and explicit submission.
local M = {}

-- The wide form (76 digits, 8-digit ids) first, then the stock one (55 digits, 5-digit ids).
local ITEM_BODIES = { "^%d" .. string.rep("%x", 76) .. "(.*)$", "^%d" .. string.rep("%x", 55) .. "(.*)$" }

local function label(body)
  if body:sub(1, 1) == "3" then
    local metadata, name = body:match("^3(.-)'(.*)$")
    if not metadata or #metadata > 65536 or not metadata:match("%^") or not metadata:match("%^$") or
        body:find("[%z\1-\17\19-\31\127]") then return "Achievement", false end
    local player, id, status, rest = metadata:match("^([^%^]+)%^(%d+)%^([0-3])%^(.*)$")
    if not player or #player > 63 or #id > 10 or (#id == 10 and id > "4294967295") then return "Achievement", false end
    -- The catalog determines the number and meaning of these numeric values;
    -- semantic validation remains in the host. Never convert their widths here.
    for value in rest:gmatch("([^%^]*)%^") do
      if not value:match("^%-?%d+$") then return "Achievement", false end
    end
    if rest ~= "" and not rest:match("%^$") then return "Achievement", false end
    return name ~= "" and name or "Achievement", true
  end
  for _, pattern in ipairs(ITEM_BODIES) do
    local name = body:match(pattern)
    if name then return name ~= "" and name or "Item", true end
  end
end

-- text with each link tag as its name; the tags go to the end of `links`, in order, each with where its name is in
-- the input (`at`: the byte it starts on, the text going in after `offset` bytes of it).
function M.Collapse(links, text, offset)
  offset = offset or 0
  local out, n, pos = {}, 0, 1
  while true do
    local i, j, body = text:find("\18(.-)\18", pos)
    if not i then break end
    local name, valid = label(body)
    local plain = text:sub(pos, i - 1)
    out[#out + 1] = plain
    n = n + #plain
    if name then
      if valid then links[#links + 1] = { name = name, tag = text:sub(i, j), at = offset + n + 1 } end
      out[#out + 1] = name
      n = n + #name
    else
      out[#out + 1] = text:sub(i, j)
      n = n + j - i + 1
    end
    pos = j + 1
  end
  local tail = text:sub(pos)
  -- An incomplete pasted achievement envelope must not expose its metadata.
  local incomplete = tail:find("\18" .. "3", 1, true)
  out[#out + 1] = incomplete and (tail:sub(1, incomplete - 1) .. "Achievement") or tail
  return table.concat(out)
end

-- Where `name` is in text from `from` on, passing over the tags already put back (`spans`) and, with `word`, a match
-- that runs into a letter or digit ("Ring" in "Bring").
local function findName(text, name, from, spans, word)
  while true do
    local i, j = text:find(name, from, true)
    if not i then return nil end
    local inside = nil
    for _, sp in ipairs(spans) do
      if i <= sp[2] and j >= sp[1] then inside = sp; break end
    end
    if inside then
      from = inside[2] + 1
    elseif word and (text:sub(i - 1, i - 1):find("^[%w]") or text:sub(j + 1, j + 1):find("^[%w]")) then
      from = i + 1
    else
      return i, j
    end
  end
end

-- The ranges for both highlighting and sending. A link whose name is still where it went in (nothing typed before it
-- since) goes back there: a plain word typed earlier with the same name stays plain. Else, the links being in the order
-- they went in, each is looked for after the one before it first (two of the same item go back in their own places),
-- a whole word before a part of one; then anywhere. A name inside a tag already put back is passed over (a tag in a
-- tag is a broken link).
local function linkSpans(links, text)
  if #links == 0 then return end
  local spans = {} -- the names that go back as tags: { first, last, tag } in text as it is
  local placed = {}
  for k, link in ipairs(links) do
    local at = link.at
    if at and text:sub(at, at + #link.name - 1) == link.name and findName(text, link.name, at, spans, false) == at then
      placed[k] = { at, at + #link.name - 1, link.tag }
      spans[#spans + 1] = placed[k]
    end
  end
  local after = 1 -- just past the link before
  for k, link in ipairs(links) do
    local sp = placed[k]
    if not sp then
      local i, j = findName(text, link.name, after, spans, true)
      if not i then i, j = findName(text, link.name, after, spans, false) end
      if not i then i, j = findName(text, link.name, 1, spans, true) end
      if not i then i, j = findName(text, link.name, 1, spans, false) end
      if i then
        sp = { i, j, link.tag }
        spans[#spans + 1] = sp
      end
    end
    if sp then after = sp[2] + 1 end
  end
  if #spans == 0 then return end
  table.sort(spans, function(a, b) return a[1] < b[1] end)
  return spans
end

function M.ColorSpans(links, text, color)
  local spans = linkSpans(links, text)
  if spans then
    for _, sp in ipairs(spans) do sp[3] = color end
  end
  return spans
end

-- Text with each link's name back as its tag.
function M.Expand(links, text)
  local spans = linkSpans(links, text)
  if not spans then return text end
  local out, pos = {}, 1
  for _, sp in ipairs(spans) do
    out[#out + 1] = text:sub(pos, sp[1] - 1)
    out[#out + 1] = sp[3]
    pos = sp[2] + 1
  end
  out[#out + 1] = text:sub(pos)
  return table.concat(out)
end

-- Link insertion uses the English host editor's ten-tag/512-byte budget. The
-- ordinary text editor keeps its existing longer limit. Validate tagged drafts
-- after expansion instead of cutting through a tag during network submission.
function M.ForSubmit(links, text, prefix)
  local expanded = (prefix or "") .. M.Expand(links, text)
  local count, pos = 0, 1
  while true do
    local first = expanded:find("\18", pos, true)
    if not first then break end
    local last = expanded:find("\18", first + 1, true)
    if not last then return nil, "Chat input contains an incomplete link." end
    count = count + 1
    if count > 10 then return nil, "Chat input exceeds ten links." end
    pos = last + 1
  end
  if count > 0 and #expanded > 512 then return nil, "Chat input exceeds 512 bytes including links." end
  return expanded
end

return M
