-- Guild bank: replaces the game's GuildBankWnd. Two tables, the bank's items and the deposits waiting for a banker,
-- each sortable with a search box (the bank's also has the window's permission filter), the free slots of each,
-- and the actions under each table: Withdraw, View, Split and Merge, the item's permission and Change Member for
-- the bank; Deposit (the item on your cursor) and Put in Bank for the deposits. Buttons a member may not use are
-- greyed by the window itself.
-- Everything is the original's (hs.ListRow / hs.ListClick on ItemList and DepositList, hs.ComboSelect on
-- FilterCombo and PermissionCombo, hs.ClickChild on the buttons): one selection across both lists, as the window
-- keeps it; Split asks how many in the game's quantity dialog.
local API = hs.UI
local hud = dofile("lib/hud.lua")
local W = dofile("lib/widgets.lua")
local T = hud.T
local C = T.color
local WINDOW = "GuildBankWnd"

local search = { ItemList = "", DepositList = "" }
local split = nil -- the bank table's share of the height (0..1), kept in ui/settings

local function filterFn(list)
  local needle = search[list]:lower()
  if needle == "" then return nil end
  return function(_, texts)
    for _, t in ipairs(texts) do if (t or ""):lower():find(needle, 1, true) then return true end end
    return false
  end
end

-- A section's title line: the name, the free slots at the right.
local function title(text, countId)
  local x, y = ui.GetCursorScreenPos()
  local w = ui.GetContentRegionAvail()
  local _, th = T.TextSize("Ag", T.font.normal)
  T.Text(x, y, text, C.accent, T.font.normal)
  local free = W.Text(WINDOW, countId)
  local n = tonumber(free:match("(%d+)"))
  T.Text(x + w, y + 2, free, n == 0 and C.danger or C.muted, T.font.small, "right")
  ui.Dummy(w, th + 2)
end

local function bankSection(s, btnH)
  title("Bank", "BankCountLabel")
  local cw = ui.GetContentRegionAvail()
  ui.SetNextItemWidth(cw * 0.55)
  search.ItemList = ui.InputTextWithHint("##gbsearch", "Search items, permissions", search.ItemList, 64)
  ui.SameLine()
  W.Combo(WINDOW, "FilterCombo")
  if ui.IsItemHovered() then ui.SetTooltip("Show only some of the items") end
  -- the table takes what the two rows under it leave
  local _, lh = ui.CalcTextSize("Ag")
  local _, avail = ui.GetContentRegionAvail()
  W.List(WINDOW, "ItemList", 0, math.max(60 * s, avail - btnH - lh - 28 * s), {
    sortable = true, filter = filterFn("ItemList"), filterKey = search.ItemList:lower(),
  })
  -- actions on the selected bank item
  W.ButtonRow(WINDOW, {
    { "WithdrawButton", "Withdraw", { primary = true, tooltip = "Take the selected item (your cursor must be empty)" } },
    { "ViewItemButton", "View", { tooltip = "Show the selected item's details" } },
    { "SplitButton", "Split", { tooltip = "Split the selected stack (asks how many)" } },
    { "MergeButton", "Merge", { tooltip = "Merge the selected item's stacks" } },
  }, btnH)
  ui.TextColored(C.muted[1], C.muted[2], C.muted[3], 1, "Permission")
  ui.SameLine()
  local bw = 120 * s
  W.Combo(WINDOW, "PermissionCombo", { width = -(bw + 8 * s) })
  if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
    ui.SetTooltip("Who may withdraw the selected item (Single Member asks for the member's name)")
  end
  ui.SameLine()
  W.Button(WINDOW, "ChangeTargetButton", "Change Member", { width = bw, height = 0,
    tooltip = "Give the selected item to another member (asks for the name)" })
end

local function depositSection(s, btnH)
  title("Deposits", "DepositCountLabel")
  ui.SetNextItemWidth(-1)
  search.DepositList = ui.InputTextWithHint("##gdsearch", "Search deposits", search.DepositList, 64)
  W.List(WINDOW, "DepositList", 0, -(btnH + 10 * s), {
    sortable = true, filter = filterFn("DepositList"), filterKey = search.DepositList:lower(),
  })
  W.ButtonRow(WINDOW, {
    { "DepositButton", "Deposit", { primary = true, tooltip = "Deposit the item on your cursor" } },
    { "PromoteButton", "Put in Bank", { tooltip = "Move the selected deposit into the bank (bankers)" } },
  }, btnH)
end

local WINDOW_OPTS = {
  size = function() return 520, 600 end,
  resizable = { 380, 440 },
}
local function drawWindow(x, y, w, h, s)
  local _, lh = ui.CalcTextSize("Ag")
  local btnH = lh + 10 * s
  split = split or tonumber(ui.GetSetting("guildbank.split") or "") or 0.62
  local topH = math.floor(h * split)
  ui.BeginChild("##gbbank", 0, topH, false)
  bankSection(s, btnH)
  ui.EndChild()
  -- a splitter between the two sections
  local sx, sy = ui.GetCursorScreenPos()
  ui.InvisibleButton("##gbsplit", w, 8 * s)
  local hov, act = ui.IsItemHovered(), ui.IsItemActive()
  local a = (hov or act) and 0.35 or 0.10
  ui.DrawLine(sx, sy + 4 * s, sx + w, sy + 4 * s, C.ink[1], C.ink[2], C.ink[3], a, act and 2 or 1)
  if hov and not act then ui.SetTooltip("Drag to share the height between the bank and the deposits") end
  if act and ui.IsMouseDragging(ui.Mouse.Left) then
    split = math.max(0.35, math.min(0.8, split + ui.GetIO().mouseDY / h))
    ui.SetSetting("guildbank.split", string.format("%.3f", split))
  end
  ui.BeginChild("##gbdeposits", 0, 0, false)
  depositSection(s, btnH)
  ui.EndChild()
end

ui.window("Guild Bank", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Guild Bank", WINDOW_OPTS, drawWindow)
end)
