-- Trade and Give (gamepad pack): stand in for TradeWnd (a trade with a player) and GiveWnd (giving to an NPC). The
-- game opens them: holding an item, A on the targeted player or NPC is the left click on them that starts a trade or
-- a give; a player's trade request opens the trade window as it does in the stock UI (there is no question: Cancel
-- declines it). Every piece is the stock window's:
--   the partner's offer (trade only), your offer's slots (A: put the item held there, or take yours back)
--   your bags (A pick up, X with an item held: into the offer, Y details)
--   your offered coin (A with coin held drops it into the offer) and your purse (A: how much onto the cursor)
--   Trade / Give (the stock button: both must accept; any change to an offer clears both) and Cancel (B)
-- LB / RB move between the sections (the offers, your bags, the coin, the buttons).
local API = hs.UI
local T = dofile("lib/theme.lua")
local pad = dofile("lib/pad.lua")
local W = dofile("lib/window.lua")
local I = dofile("lib/items.lua")
local X = dofile("lib/exchange.lua")
local C = T.C

-- The windows: the stock screen, its child prefix, our slots' child name and count, the accept button.
local KINDS = {
  trade = { screen = "TradeWnd", p = "TRDW_", slot = "TRDW_TradeSlot", slots = 8, accept = "TRDW_Trade_Button",
    cancel = "TRDW_Cancel_Button", verb = "Trade" },
  give = { screen = "GiveWnd", p = "GVW_", slot = "GVW_MyItemSlot", slots = 4, accept = "GVW_Give_Button",
    cancel = "GVW_Cancel_Button", verb = "Give" },
}

local SECTIONS = { "offer", "bags", "coin", "buttons" }

local function draw(kind, name, state)
  local K = KINDS[kind]
  if not W.shown(K.screen) then
    state.bag = nil
    return
  end
  local t = API.trade.State and API.trade.State() or {}
  local partner = API.windows.Text(K.screen, kind == "trade" and "TRDW_HisName" or "GVW_NPCName") or t.partner or ""
  state.nav = state.nav or {}
  local ctx = X.frame(state.nav, SECTIONS)
  -- the first empty slot of our offer, where X puts the item held
  local function put()
    for n = 0, K.slots - 1 do
      if not API.inventory.SlotItem(K.screen, K.slot .. n) then
        API.windows.ClickChild(K.screen, K.slot .. n, "left")
        return
      end
    end
  end
  if W.begin(name, { w = 1840, h = 1000, title = K.verb, subtitle = (kind == "trade" and "Trading with " or "Giving to ") .. partner,
        hints = { { "a", "Pick up / place" }, { "x", "Offer" }, { "y", "Details" }, { "lb", "" }, { "rb", "Section" }, { "b", "Cancel" } } }) then
    local x0, y0, x1, y1 = W.area()
    local wx, wy = ui.GetWindowPos()
    local rightW = T.px(470)
    local leftW = T.px(8 * X.SIZE + 7 * X.GAP)
    local midX = x0 + leftW + T.px(40)
    local rx0 = x1 - rightW
    ui.SetCursorPos(x0 - wx, y0 - wy)
    -- the partner's offer (a player's: an NPC offers nothing)
    if kind == "trade" then
      T.heading(partner ~= "" and (partner .. " offers") or "Their offer", leftW)
      for n = 8, 15 do
        X.row(x0, wx, wy, n - 8, 4)
        local child = K.slot .. n
        X.cell(ctx, "##his" .. n, API.inventory.SlotItem(K.screen, child), partner .. "'s offer",
          function(b) API.windows.ClickChild(K.screen, child, b) end, { noUse = true })
      end
      local _, cy = ui.GetCursorScreenPos()
      T.font(22)
      local x = x0
      for i = 0, 3 do
        local v = API.windows.Text(K.screen, "TRDW_HisMoney" .. i) or "0"
        local tw = T.text(x, cy, v, C.text)
        x = x + tw + T.px(4)
        local uw = T.text(x, cy, X.COINS[i + 1][1], X.COINS[i + 1][2])
        x = x + uw + T.px(18)
      end
      local _, th = ui.CalcTextSize("Ag")
      T.unfont()
      if t.theirReady then T.textIn(x0, cy, x0 + leftW, cy + th, "ACCEPTED", C.good, 1) end
      ui.SetCursorPos(x0 - wx, cy + th + T.px(24) - wy)
    end
    -- our offer
    T.heading("You offer", leftW)
    X.section(ctx, "offer")
    for n = 0, K.slots - 1 do
      X.row(x0, wx, wy, n, 4)
      local child = K.slot .. n
      X.cell(ctx, "##mine" .. n, API.inventory.SlotItem(K.screen, child), "Your offer, slot " .. (n + 1),
        function(b) API.windows.ClickChild(K.screen, child, b) end, { noUse = true, put = put })
    end
    ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) + T.px(4) - wy)
    T.heading("Coin offered", leftW)
    X.coinRow("offer", x0, leftW, function(i) return tonumber((API.windows.Text(K.screen, K.p .. "MyMoney" .. i))) end,
      function(i) API.windows.ClickChild(K.screen, K.p .. "MyMoney" .. i, "left") end, { ctx = ctx, section = "coin" })
    ui.SetCursorPos(x0 - wx, select(2, ui.GetCursorScreenPos()) - wy)
    T.heading("Your coin", leftW)
    X.purse(ctx, x0, leftW)
    -- your bags
    ui.SetCursorPos(midX - wx, y0 - wy)
    X.bags(ctx, state, midX, rx0 - midX - T.px(40), 5, { put = put })
    -- the right column: where the trade stands, the focused item, what is held, Trade and Cancel
    local status, sc
    if t.requested and not t.open then status, sc = "Waiting for " .. partner .. " to answer...", C.dim
    elseif t.myReady and t.theirReady then status, sc = "Both accepted.", C.good
    elseif t.myReady then status, sc = "You accepted. Waiting for " .. partner .. "...", C.endurance
    elseif t.theirReady then status, sc = partner .. " has accepted.", C.good
    else status, sc = kind == "trade" and "Neither of you has accepted yet." or "Put in what you give, then Give.", C.dim end
    T.rect(rx0, y0, x1, y0 + T.px(70), C.sunken, 1, T.px(14))
    T.font(20)
    T.textIn(rx0 + T.px(18), y0, x1 - T.px(18), y0 + T.px(70), T.fit(status, rightW - T.px(36)), sc, 0)
    T.unfont()
    local btnH = T.px(72)
    local holdH = X.holdingHeight(ctx)
    local sumY0 = y0 + T.px(82)
    local sumY1 = y1 - btnH - T.px(12) - (holdH > 0 and holdH + T.px(12) or 0)
    I.summary(ctx.focusedItem, ctx.focusedWhere, rx0, sumY0, x1, sumY1)
    X.holding(ctx, rx0, sumY1 + T.px(12), x1, ctx.held and "A: place   X: offer it" or "A on Coin offered: put it in")
    ui.SetCursorPos(rx0 - wx, y1 - btnH - wy)
    local bw = (rightW - T.px(12)) / 2
    local accept = function() API.windows.ClickChild(K.screen, K.accept, "left") end
    X.section(ctx, "buttons")
    local ok, f1 = T.button("##accept", K.verb, bw, btnH, { primary = true, disabled = t.myReady == true }, pad)
    if ok then accept() end
    ui.SameLine(0, T.px(12))
    local no, f2 = T.button("##cancel", "Cancel", bw, btnH, { glyph = "b" }, pad)
    if no then API.windows.ClickChild(K.screen, K.cancel, "left") end
    if f1 or f2 then state.nav.at = "buttons" end
  end
  if W.finish() then API.windows.ClickChild(K.screen, K.cancel, "left") end
end

local tradeState, giveState = {}, {}
ui.window("Trade", function() draw("trade", "Trade", tradeState) end)
ui.window("Give", function() draw("give", "Give", giveState) end)
