-- The point merchants' window body (pointmerchant.lua: PointMerchantWnd, alternate currencies; newpointmerchant.lua:
-- NewPointMerchantWnd, adventure points and crystals): the merchant and how much of its currency you hold, the
-- goods as a sortable table with a search (a cost above what you hold in red), Purchase, Sell (for the item on your
-- cursor; the offer on hover) and Done. Everything is the window's children: a row click selects in the game's list,
-- the buttons send from its handler, and a right click on a good inspects it (the original's list right click).
--   draw(cfg, w, h, s)  cfg = { window, prefix, currency() -> name, icon() -> tex.. | nil, columns (extra) }
local API = hs.UI
local W = dofile("lib/widgets.lua")
local M = dofile("lib/markets.lua")
local T = dofile("lib/theme.lua")
local C = T.color

local P = {}

local function cost(r) return tonumber(((r.texts[2] or ""):gsub(",", ""))) or 0 end

function P.Draw(cfg, w, h, s)
  local win, pre = cfg.window, cfg.prefix
  local lh, btnH = M.Heights(s)
  local haveText = W.Text(win, pre .. "PointsAvailableValue")
  local have = tonumber((haveText:gsub(",", "")))
  local label = cfg.currency()
  if label == "" then label = "Points" end

  -- the header: the merchant, and on the right what you hold
  local name = W.Text(win, pre .. "NameLabel")
  ui.PushFont(T.font.normal)
  ui.TextColored(C.text[1], C.text[2], C.text[3], 1, name ~= "" and name or "Merchant")
  ui.PopFont()
  local amount = have and T.Thousands(have) or "?"
  local _, th = ui.CalcTextSize("Ag")
  local tex, u0, v0, u1, v1 = nil, nil, nil, nil, nil
  if cfg.icon then tex, u0, v0, u1, v1 = cfg.icon() end
  local iw = tex and (th + 4) or 0
  ui.SameLine(math.max(0, w - ui.CalcTextSize(amount) - ui.CalcTextSize(label) - iw - 10))
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, label)
  ui.SameLine()
  if tex then
    local x, y = ui.GetCursorScreenPos()
    ui.DrawImage(tex, x, y, x + th, y + th, u0, v0, u1, v1)
    ui.Dummy(th, th)
    ui.SameLine(0, 4)
  end
  ui.TextColored(C.gold[1], C.gold[2], C.gold[3], 1, amount)
  M.Tip(have and ("You hold " .. T.Thousands(have) .. " " .. label) or "The server has not said how much you hold")

  local function short(r) return have and cost(r) > have end
  local cols = {
    { "Item", 0, cell = M.TextCell(1), default = "asc" },
    { "Cost", 80, cell = function(r, c)
        local t = T.Thousands(cost(r))
        local col = short(r) and C.bad or C.gold
        ui.DrawText(c.x + c.w - ui.CalcTextSize(t) - 4 * c.s, c.y + (c.h - c.textH) / 2, col[1], col[2], col[3], 1, t)
      end, sort = cost },
  }
  for _, c in ipairs(cfg.columns or {}) do cols[#cols + 1] = c end

  local needle = M.Search(win, "Search goods")
  local _, _, sel = M.Table(win, pre .. "ItemList", 0, -(btnH * 2 + lh + 16 * s), cols, {
    key = win,
    filter = needle ~= "" and function(r) return (r.texts[1] or ""):lower():find(needle, 1, true) ~= nil end or nil,
    filterKey = needle,
    empty = "This merchant has nothing for sale.",
    emptyFiltered = "No good matches.",
    tooltip = function(r)
      local c = cost(r)
      return (r.texts[1] or "") .. "\n" .. T.Thousands(c) .. " " .. label
        .. (short(r) and ("\nYou need " .. T.Thousands(c - have) .. " more") or "")
        .. "\nRight-click to inspect"
    end,
  })

  -- the selected good
  if sel then
    ui.TextColored(C.text[1], C.text[2], C.text[3], 1, sel.texts[1] or "")
    ui.SameLine()
    local col = short(sel) and C.bad or C.gold
    ui.TextColored(col[1], col[2], col[3], 1, T.Thousands(cost(sel)) .. " " .. label)
  else
    ui.TextColored(C.faint[1], C.faint[2], C.faint[3], 1, "Select a good to buy it.")
  end
  local sellTip = API.windows.ChildTooltip(win, pre .. "SellButton")
  W.ButtonRow(win, {
    { pre .. "PurchaseButton", "Purchase", { primary = true,
      tooltip = sel and (short(sel) and ("Not enough " .. label) or "Buy the selected good") or "Select a good first" } },
    { pre .. "SellButton", "Sell", { tooltip = (sellTip and sellTip ~= "") and ("Sell " .. sellTip)
      or "Pick up an item this merchant takes to sell it here" } },
    { pre .. "DoneButton", "Done" },
  }, btnH)
end

return P
