-- The modern UI's colour palettes: the player picks one in Options > Display > Interface > Colour Palette (the
-- setting `palette` in ui/settings/modern.txt), and lib/theme.lua puts its colours into T.color, which every window
-- reads. Data only (no ui.* calls): tests/lua_scripts_test.cpp loads this file and checks `checks` below.
--
-- A palette names its colours ({ r, g, b, a }), or takes another's (`base`) and changes some. Tokens:
--   panel, panelTop      the HUD frames' translucent body and its lighter top
--   window, popup        the dialogs' opaque body; menus, combo lists and tooltips
--   title, titleActive   a dialog's title bar, unfocused and focused
--   inset                bar tracks, icon wells
--   frame, frameHover, frameActive, frameBorder   fields, check boxes, combo boxes, sliders and their outline
--   border, borderHi     hairlines around panels; a hovered one
--   separator            divider lines between sections
--   ink                  the colour of faint overlays (hover washes, zebra rows, hairlines): white on dark, black on light
--   shadow, textShadow   the drop shadow under panels, and the one under text drawn over bars and the world
--   text, muted, faint   body text, secondary text (notes, labels), de-emphasised text (empty slots, hints)
--   heading              section headings (the options window's)
--   accent, onAccent     the one highlight colour (focus, selection, primary buttons) and text drawn on it
--   mapBg                the map window's backdrop
--   chatBg, chatText, chatLink   the chat's page under its lines, drawn whatever the chat frame's own panel fades to
--                        while idle (its checks count the page alone over the world), so its alpha alone must
--                        carry the contrast (the lines' colours are the player's user colours, made for a
--                        dark page), a line without a colour of its own, a link
--   barText, barShadow   text on the health, mana and other bars (light: the fills are saturated on every palette),
--                        and the minimap's zone name and clock on the world
--   gold, hp, hpLow, mana, endurance, pet, xp, aa, cast, good, bad, danger   meaning colours (bars, rings, warnings;
--                        pet is also the "ready" and "can afford" green)
--   platinum, silver, copper   the coins (gold above); magic: a magic item's name; link: links in the dialogs' text
local P = {}

P.default = "midnight"

-- The contrast every palette must keep (WCAG 2.1): { foreground, background, ratio [, under] }. Text needs 4.5:1
-- (AA), a control's outline, de-emphasised hint text and the bars' numbers (drawn with a shadow) 3:1. A translucent
-- background counts as laid over `under` (another token) and that over black and over white (the worse of the two:
-- the world behind), a translucent foreground over its background.
P.checks = {
  { "text", "window", 4.5 }, { "muted", "window", 4.5 }, { "heading", "window", 4.5 }, { "accent", "window", 4.5 },
  { "text", "popup", 4.5 }, { "muted", "popup", 4.5 },
  { "text", "frame", 4.5 }, { "text", "titleActive", 4.5 }, { "text", "title", 4.5 },
  { "onAccent", "accent", 4.5 },
  { "faint", "window", 3.0 }, { "frameBorder", "window", 3.0 },
  { "barText", "hp", 3.0 }, { "barText", "mana", 3.0 }, { "barText", "chatBg", 4.5 },
  { "link", "window", 4.5 }, { "chatLink", "chatBg", 4.5 }, { "chatText", "chatBg", 4.5 }, { "magic", "window", 4.5 }, { "pet", "window", 3.0 },
  { "platinum", "window", 4.5 }, { "silver", "window", 4.5 }, { "copper", "window", 4.5 }, { "gold", "window", 4.5 },
}

P.list = {
  {
    id = "midnight", name = "Midnight",
    description = "Hailstone's own: deep blue-grey glass with an icy blue accent.",
    colors = {
      panel = { 0.055, 0.066, 0.090, 0.80 },
      panelTop = { 0.090, 0.105, 0.140, 0.80 },
      window = { 0.050, 0.060, 0.082, 0.99 },
      popup = { 0.070, 0.082, 0.110, 0.98 },
      title = { 0.060, 0.072, 0.098, 0.98 },
      titleActive = { 0.085, 0.105, 0.145, 0.99 },
      inset = { 0.020, 0.024, 0.034, 0.75 },
      frame = { 0.105, 0.125, 0.165, 1 },
      frameHover = { 0.150, 0.180, 0.235, 1 },
      frameActive = { 0.180, 0.225, 0.300, 1 },
      frameBorder = { 0.420, 0.480, 0.580, 1 },
      border = { 1, 1, 1, 0.08 },
      borderHi = { 1, 1, 1, 0.20 },
      separator = { 1, 1, 1, 0.22 },
      ink = { 1, 1, 1, 1 },
      shadow = { 0, 0, 0, 0.35 },
      textShadow = { 0, 0, 0, 0.85 },
      text = { 0.93, 0.95, 0.98, 1 },
      muted = { 0.66, 0.72, 0.80, 1 },
      faint = { 0.50, 0.55, 0.63, 1 },
      heading = { 0.62, 0.84, 1.00, 1 },
      accent = { 0.48, 0.78, 1.00, 1 }, -- hail blue
      onAccent = { 0.02, 0.06, 0.10, 1 },
      mapBg = { 0.035, 0.045, 0.065, 0.85 },
      chatBg = { 0.035, 0.045, 0.065, 0.78 },
      barText = { 0.97, 0.98, 1.00, 1 },
      barShadow = { 0, 0, 0, 0.85 },
      gold = { 1.00, 0.80, 0.36, 1 },
      hp = { 0.90, 0.24, 0.30, 1 },
      hpLow = { 1.00, 0.42, 0.20, 1 },
      mana = { 0.28, 0.56, 1.00, 1 },
      endurance = { 0.97, 0.76, 0.26, 1 },
      pet = { 0.36, 0.84, 0.54, 1 },
      xp = { 0.67, 0.50, 1.00, 1 },
      aa = { 1.00, 0.78, 0.30, 1 },
      cast = { 0.48, 0.78, 1.00, 1 },
      good = { 0.40, 0.72, 1.00, 1 }, -- a beneficial buff's ring
      bad = { 1.00, 0.36, 0.36, 1 },  -- a detrimental one's
      danger = { 1.00, 0.30, 0.25, 1 },
      platinum = { 0.85, 0.88, 0.95, 1 },
      silver = { 0.75, 0.78, 0.82, 1 },
      copper = { 0.80, 0.52, 0.32, 1 },
      magic = { 0.55, 0.78, 1.00, 1 }, -- a magic item's name
      link = { 0.45, 0.75, 1.00, 1 },  -- links in the dialogs' text
      chatLink = { 0.45, 0.75, 1.00, 1 }, -- and in chat (on chatBg)
      chatText = { 0.93, 0.95, 0.98, 1 }, -- chat lines without a colour of their own
    },
  },
  {
    id = "charcoal", name = "Charcoal", base = "midnight",
    description = "Neutral dark grey, no tint, with a soft white accent.",
    colors = {
      panel = { 0.075, 0.075, 0.078, 0.82 },
      panelTop = { 0.115, 0.115, 0.120, 0.82 },
      window = { 0.070, 0.070, 0.074, 0.99 },
      popup = { 0.095, 0.095, 0.100, 0.98 },
      title = { 0.085, 0.085, 0.090, 0.98 },
      titleActive = { 0.125, 0.125, 0.132, 0.99 },
      inset = { 0.030, 0.030, 0.032, 0.78 },
      frame = { 0.140, 0.140, 0.148, 1 },
      frameHover = { 0.190, 0.190, 0.200, 1 },
      frameActive = { 0.235, 0.235, 0.250, 1 },
      frameBorder = { 0.500, 0.500, 0.520, 1 },
      text = { 0.94, 0.94, 0.94, 1 },
      muted = { 0.72, 0.72, 0.74, 1 },
      faint = { 0.54, 0.54, 0.56, 1 },
      heading = { 0.86, 0.86, 0.90, 1 },
      accent = { 0.80, 0.82, 0.88, 1 },
      onAccent = { 0.06, 0.06, 0.07, 1 },
      mapBg = { 0.045, 0.045, 0.048, 0.85 },
      chatBg = { 0.040, 0.040, 0.042, 0.78 },
      cast = { 0.80, 0.82, 0.88, 1 },
    },
  },
  {
    id = "daylight", name = "Daylight",
    description = "A light theme: pale frosted panels and dark text, for bright scenes and bright rooms.", base = "midnight",
    colors = {
      panel = { 0.940, 0.950, 0.965, 0.86 },
      panelTop = { 1.000, 1.000, 1.000, 0.86 },
      window = { 0.955, 0.962, 0.975, 0.99 },
      popup = { 0.985, 0.988, 0.995, 0.99 },
      title = { 0.880, 0.895, 0.920, 0.99 },
      titleActive = { 0.820, 0.860, 0.920, 1 },
      inset = { 0.780, 0.805, 0.845, 0.85 },
      frame = { 1.000, 1.000, 1.000, 1 },
      frameHover = { 0.910, 0.935, 0.970, 1 },
      frameActive = { 0.850, 0.895, 0.960, 1 },
      frameBorder = { 0.430, 0.470, 0.540, 1 },
      border = { 0, 0, 0, 0.14 },
      borderHi = { 0, 0, 0, 0.28 },
      separator = { 0, 0, 0, 0.22 },
      ink = { 0, 0, 0, 1 },
      shadow = { 0, 0, 0, 0.18 },
      textShadow = { 1, 1, 1, 0.55 },
      text = { 0.070, 0.085, 0.120, 1 },
      muted = { 0.270, 0.310, 0.380, 1 },
      faint = { 0.420, 0.455, 0.520, 1 },
      heading = { 0.040, 0.300, 0.600, 1 },
      accent = { 0.040, 0.380, 0.740, 1 },
      onAccent = { 1, 1, 1, 1 },
      mapBg = { 0.900, 0.910, 0.925, 0.92 },
      chatBg = { 0.075, 0.090, 0.120, 0.93 }, -- the chat's user colours are made for a dark page
      gold = { 0.560, 0.380, 0.000, 1 },
      cast = { 0.150, 0.500, 0.900, 1 },
      good = { 0.100, 0.450, 0.850, 1 },
      bad = { 0.850, 0.150, 0.150, 1 },
      danger = { 0.850, 0.120, 0.080, 1 },
      platinum = { 0.330, 0.380, 0.480, 1 },
      silver = { 0.400, 0.420, 0.450, 1 },
      copper = { 0.600, 0.300, 0.120, 1 },
      magic = { 0.060, 0.360, 0.720, 1 },
      link = { 0.100, 0.420, 0.820, 1 },
      pet = { 0.100, 0.540, 0.260, 1 },
    },
  },
  {
    id = "contrast", name = "High Contrast",
    description = "Black panels, white text and a bright yellow accent with strong outlines: the easiest to read.",
    base = "midnight",
    colors = {
      panel = { 0, 0, 0, 0.92 },
      panelTop = { 0.06, 0.06, 0.06, 0.92 },
      window = { 0, 0, 0, 1 },
      popup = { 0, 0, 0, 1 },
      title = { 0.10, 0.10, 0.10, 1 },
      titleActive = { 0.16, 0.16, 0.16, 1 },
      inset = { 0, 0, 0, 0.90 },
      frame = { 0, 0, 0, 1 },
      frameHover = { 0.14, 0.14, 0.14, 1 },
      frameActive = { 0.22, 0.22, 0.22, 1 },
      frameBorder = { 0.90, 0.90, 0.90, 1 },
      border = { 1, 1, 1, 0.55 },
      borderHi = { 1, 0.90, 0.10, 0.90 },
      separator = { 1, 1, 1, 0.55 },
      textShadow = { 0, 0, 0, 1 },
      text = { 1, 1, 1, 1 },
      muted = { 0.86, 0.86, 0.86, 1 },
      faint = { 0.72, 0.72, 0.72, 1 },
      heading = { 1.00, 0.90, 0.10, 1 },
      accent = { 1.00, 0.90, 0.10, 1 },
      onAccent = { 0, 0, 0, 1 },
      mapBg = { 0, 0, 0, 0.95 },
      chatBg = { 0, 0, 0, 0.95 },
      cast = { 1.00, 0.90, 0.10, 1 },
      good = { 0.30, 0.80, 1.00, 1 },
      bad = { 1.00, 0.30, 0.30, 1 },
    },
  },
  {
    id = "ember", name = "Ember", base = "midnight",
    description = "Midnight with a warm orange accent.",
    colors = {
      accent = { 1.00, 0.62, 0.28, 1 }, heading = { 1.00, 0.72, 0.45, 1 }, onAccent = { 0.10, 0.04, 0.00, 1 },
      cast = { 1.00, 0.62, 0.28, 1 },
    },
  },
  {
    id = "verdant", name = "Verdant", base = "midnight",
    description = "Midnight with a fresh green accent.",
    colors = {
      accent = { 0.40, 0.86, 0.56, 1 }, heading = { 0.56, 0.92, 0.68, 1 }, onAccent = { 0.00, 0.08, 0.03, 1 },
      cast = { 0.40, 0.86, 0.56, 1 },
    },
  },
  {
    id = "amethyst", name = "Amethyst", base = "midnight",
    description = "Midnight with a violet accent.",
    colors = {
      accent = { 0.76, 0.62, 1.00, 1 }, heading = { 0.82, 0.72, 1.00, 1 }, onAccent = { 0.06, 0.02, 0.12, 1 },
      cast = { 0.76, 0.62, 1.00, 1 },
    },
  },
  {
    id = "rose", name = "Rose", base = "midnight",
    description = "Midnight with a pink accent.",
    colors = {
      accent = { 1.00, 0.55, 0.70, 1 }, heading = { 1.00, 0.68, 0.79, 1 }, onAccent = { 0.12, 0.02, 0.05, 1 },
      cast = { 1.00, 0.55, 0.70, 1 },
    },
  },
}

-- Colour vision (Options > Display > Accessibility > Colour Vision: the client option colour_vision, which also sets
-- the con colours, game/accessibility.h). lib/theme.lua lays the mode's colours over whichever palette is picked, so
-- the meaning colours (health and low health, good and bad, danger, pet / ready) stay apart for that colour
-- blindness. Deuteranopia and protanopia take Okabe and Ito's Color Universal Design set (vermillion, reddish purple,
-- sky blue, bluish green); tritanopia Paul Tol's "vibrant" scheme (red, magenta, cyan). Every palette with each mode
-- laid over it keeps `checks` (tests/lua_scripts_test.cpp). `colourVisionModes`: the option's values 0..3.
P.colourVisionModes = { "off", "deuteranopia", "protanopia", "tritanopia" }
local RED_GREEN = {
  hp = { 0.835, 0.369, 0.000, 1 },     -- vermillion
  hpLow = { 0.800, 0.475, 0.655, 1 },  -- reddish purple: apart from the vermillion in lightness and hue
  good = { 0.337, 0.706, 0.914, 1 },   -- sky blue
  bad = { 0.835, 0.369, 0.000, 1 },    -- vermillion
  danger = { 0.835, 0.369, 0.000, 1 },
  pet = { 0.000, 0.620, 0.451, 1 },    -- bluish green
}
P.colourVision = {
  deuteranopia = RED_GREEN,
  protanopia = RED_GREEN,
  tritanopia = {
    hp = { 0.800, 0.200, 0.067, 1 },   -- red
    hpLow = { 0.200, 0.733, 0.933, 1 }, -- cyan: red and cyan are the pair a tritanope tells apart best
    good = { 0.200, 0.733, 0.933, 1 },
    bad = { 0.800, 0.200, 0.067, 1 },
    danger = { 0.800, 0.200, 0.067, 1 },
  },
}

return P
