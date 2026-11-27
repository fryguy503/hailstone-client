-- The gamepad UI's colour palettes: the player picks one on Options > Interface > Colour palette (the setting
-- `palette` in ui/settings/gamepad.txt), and lib/theme.lua puts its colours into T.C, which every window reads. Data
-- only (no ui.* calls): tests/lua_scripts_test.cpp loads this file and checks `checks` below.
--
-- A palette names its colours ({ r, g, b, a }), or takes another's (`base`) and changes some. Tokens:
--   bg                   HUD panels (translucent, over the world)
--   window, popup        the windows' body; menus and combo lists
--   raised, sunken       buttons, slots and rows; bar tracks and empty slots
--   hover, selected      a hovered field; a hovered button or selected row (ImGui's pieces)
--   titleActive          a focused window's title bar (ImGui's)
--   edge                 hairline borders
--   ink                  the colour of faint overlays and divider lines: white on dark, black on light
--   shadow, textShadow   drop shadows; the one under every text
--   text, dim, faint     body text, secondary text and hints, de-emphasised text (empty slots)
--   accent, onAccent     focus, selection and primary actions, and text drawn on the accent
--   glyphBg              the pad button glyphs' face
--   mapBg                the map and minimap's disc
--   chatBg, chatText     behind the chat's lines (their colours are the player's user colours, made for a dark
--                        page), and a line without a colour of its own (chatLink: a link there); link: links in
--                        the windows' text. The page is checked alone over the world: its alpha carries the contrast
--   hp, mana, endurance, cast, exp, good, bad   meaning colours (bars, rings, warnings)
--   platinum, gold, silver, copper   the coins
local P = {}

P.default = "midnight"

-- The contrast every palette must keep (WCAG 2.1): { foreground, background, ratio [, under] }. Text needs 4.5:1
-- (AA); hint text and the focus ring 3:1. A translucent background counts as laid over `under` (another token) and
-- that over black and over white (the worse of the two: the world behind), a translucent foreground over its
-- background.
P.checks = {
  { "text", "window", 4.5 }, { "dim", "window", 4.5 }, { "accent", "window", 3.0 }, { "faint", "window", 3.0 },
  { "text", "raised", 4.5, "window" }, { "dim", "raised", 4.5, "window" }, { "text", "popup", 4.5 },
  { "onAccent", "accent", 4.5 }, { "text", "selected", 4.5, "window" }, { "text", "titleActive", 4.5 },
  { "chatText", "chatBg", 4.5 }, { "chatLink", "chatBg", 4.5 }, { "link", "window", 4.5 },
  { "platinum", "window", 4.5 }, { "gold", "window", 4.5 }, { "silver", "window", 4.5 }, { "copper", "window", 4.5 },
}

P.list = {
  {
    id = "midnight", name = "Midnight",
    description = "Hailstone's own: dark blue-grey glass with a bright blue accent.",
    colors = {
      bg = { 0.055, 0.065, 0.085, 0.84 },
      window = { 0.05, 0.058, 0.075, 0.96 },
      popup = { 0.07, 0.08, 0.10, 0.98 },
      raised = { 0.13, 0.145, 0.18, 0.94 },
      sunken = { 0.03, 0.035, 0.045, 0.70 },
      hover = { 0.18, 0.22, 0.28, 1 },
      selected = { 0.18, 0.30, 0.40, 1 },
      titleActive = { 0.10, 0.12, 0.16, 0.98 },
      edge = { 1, 1, 1, 0.07 },
      ink = { 1, 1, 1, 1 },
      shadow = { 0, 0, 0, 0.65 },
      textShadow = { 0, 0, 0, 0.6 },
      text = { 0.94, 0.95, 0.98, 1 },
      dim = { 0.66, 0.71, 0.78, 1 },
      faint = { 0.48, 0.52, 0.58, 1 },
      accent = { 0.27, 0.72, 1.00, 1 },
      onAccent = { 0.02, 0.06, 0.10, 1 },
      glyphBg = { 0.10, 0.11, 0.14, 1 },
      mapBg = { 0.09, 0.105, 0.13, 0.85 },
      chatBg = { 0.055, 0.065, 0.085, 0.78 },
      chatText = { 0.94, 0.95, 0.98, 1 },
      link = { 0.45, 0.75, 1.00, 1 },
      chatLink = { 0.45, 0.75, 1.00, 1 },
      hp = { 0.89, 0.27, 0.31, 1 },
      mana = { 0.30, 0.56, 1.00, 1 },
      endurance = { 0.98, 0.76, 0.26, 1 },
      cast = { 0.74, 0.54, 1.00, 1 },
      exp = { 0.56, 0.86, 0.42, 1 },
      good = { 0.42, 0.86, 0.50, 1 },
      bad = { 0.95, 0.38, 0.36, 1 },
      platinum = { 0.85, 0.88, 0.95, 1 },
      gold = { 0.98, 0.80, 0.30, 1 },
      silver = { 0.75, 0.78, 0.82, 1 },
      copper = { 0.85, 0.55, 0.35, 1 },
    },
  },
  {
    id = "charcoal", name = "Charcoal", base = "midnight",
    description = "Neutral dark grey, no tint, with a soft white accent.",
    colors = {
      bg = { 0.075, 0.075, 0.078, 0.86 },
      window = { 0.07, 0.07, 0.074, 0.96 },
      popup = { 0.095, 0.095, 0.10, 0.98 },
      raised = { 0.16, 0.16, 0.168, 0.95 },
      sunken = { 0.035, 0.035, 0.037, 0.72 },
      hover = { 0.22, 0.22, 0.23, 1 },
      selected = { 0.28, 0.28, 0.30, 1 },
      titleActive = { 0.13, 0.13, 0.14, 0.98 },
      dim = { 0.72, 0.72, 0.74, 1 },
      faint = { 0.54, 0.54, 0.56, 1 },
      accent = { 0.86, 0.87, 0.92, 1 },
      onAccent = { 0.06, 0.06, 0.07, 1 },
      glyphBg = { 0.12, 0.12, 0.13, 1 },
      mapBg = { 0.10, 0.10, 0.105, 0.85 },
      chatBg = { 0.075, 0.075, 0.078, 0.78 },
    },
  },
  {
    id = "daylight", name = "Daylight", base = "midnight",
    description = "A light theme: pale panels and dark text, for bright scenes and bright rooms.",
    colors = {
      bg = { 0.94, 0.95, 0.965, 0.88 },
      window = { 0.955, 0.962, 0.975, 0.98 },
      popup = { 0.985, 0.988, 0.995, 0.99 },
      raised = { 1.00, 1.00, 1.00, 0.95 },
      sunken = { 0.80, 0.82, 0.86, 0.80 },
      hover = { 0.88, 0.91, 0.96, 1 },
      selected = { 0.78, 0.86, 0.96, 1 },
      titleActive = { 0.82, 0.86, 0.92, 1 },
      edge = { 0, 0, 0, 0.16 },
      ink = { 0, 0, 0, 1 },
      shadow = { 0, 0, 0, 0.25 },
      textShadow = { 1, 1, 1, 0.5 },
      text = { 0.07, 0.085, 0.12, 1 },
      dim = { 0.27, 0.31, 0.38, 1 },
      faint = { 0.42, 0.455, 0.52, 1 },
      accent = { 0.04, 0.40, 0.76, 1 },
      onAccent = { 1, 1, 1, 1 },
      glyphBg = { 0.16, 0.18, 0.22, 1 },
      mapBg = { 0.90, 0.91, 0.925, 0.92 },
      chatBg = { 0.075, 0.09, 0.12, 0.93 }, -- the chat's user colours are made for a dark page
      link = { 0.08, 0.40, 0.80, 1 },
      platinum = { 0.33, 0.38, 0.48, 1 },
      gold = { 0.56, 0.38, 0.00, 1 },
      silver = { 0.40, 0.42, 0.45, 1 },
      copper = { 0.60, 0.30, 0.12, 1 },
      good = { 0.10, 0.54, 0.26, 1 },
      bad = { 0.85, 0.15, 0.15, 1 },
    },
  },
  {
    id = "contrast", name = "High Contrast", base = "midnight",
    description = "Black panels, white text and a bright yellow accent: the easiest to read.",
    colors = {
      bg = { 0, 0, 0, 0.92 },
      window = { 0, 0, 0, 1 },
      popup = { 0, 0, 0, 1 },
      raised = { 0.12, 0.12, 0.12, 1 },
      sunken = { 0, 0, 0, 0.9 },
      hover = { 0.20, 0.20, 0.20, 1 },
      selected = { 0.30, 0.27, 0.00, 1 },
      titleActive = { 0.16, 0.16, 0.16, 1 },
      edge = { 1, 1, 1, 0.55 },
      textShadow = { 0, 0, 0, 1 },
      text = { 1, 1, 1, 1 },
      dim = { 0.86, 0.86, 0.86, 1 },
      faint = { 0.72, 0.72, 0.72, 1 },
      accent = { 1.00, 0.90, 0.10, 1 },
      onAccent = { 0, 0, 0, 1 },
      glyphBg = { 0, 0, 0, 1 },
      mapBg = { 0, 0, 0, 0.95 },
      chatBg = { 0, 0, 0, 0.95 },
      chatText = { 1, 1, 1, 1 },
    },
  },
  {
    id = "ember", name = "Ember", base = "midnight", description = "Midnight with a warm orange accent.",
    colors = { accent = { 1.00, 0.60, 0.25, 1 }, onAccent = { 0.10, 0.04, 0.00, 1 }, selected = { 0.36, 0.22, 0.12, 1 } },
  },
  {
    id = "verdant", name = "Verdant", base = "midnight", description = "Midnight with a fresh green accent.",
    colors = { accent = { 0.36, 0.84, 0.52, 1 }, onAccent = { 0.00, 0.08, 0.03, 1 }, selected = { 0.14, 0.30, 0.20, 1 } },
  },
  {
    id = "amethyst", name = "Amethyst", base = "midnight", description = "Midnight with a violet accent.",
    colors = { accent = { 0.74, 0.58, 1.00, 1 }, onAccent = { 0.06, 0.02, 0.12, 1 }, selected = { 0.27, 0.20, 0.40, 1 } },
  },
  {
    id = "rose", name = "Rose", base = "midnight", description = "Midnight with a pink accent.",
    colors = { accent = { 1.00, 0.52, 0.68, 1 }, onAccent = { 0.12, 0.02, 0.05, 1 }, selected = { 0.38, 0.17, 0.25, 1 } },
  },
}

-- Colour vision (Options > Interface > Colour vision: the client option colour_vision, which also sets the con
-- colours, game/accessibility.h). lib/theme.lua lays the mode's colours over whichever palette is picked, so the
-- meaning colours (health, good and bad) stay apart for that colour blindness. Deuteranopia and protanopia take Okabe
-- and Ito's Color Universal Design set (vermillion, sky blue); tritanopia Paul Tol's "vibrant" scheme (red, cyan).
-- Every palette with each mode laid over it keeps `checks` (tests/lua_scripts_test.cpp). `colourVisionModes`: the
-- option's values 0..3.
P.colourVisionModes = { "off", "deuteranopia", "protanopia", "tritanopia" }
local RED_GREEN = {
  hp = { 0.835, 0.369, 0.000, 1 },   -- vermillion
  good = { 0.337, 0.706, 0.914, 1 }, -- sky blue
  bad = { 0.835, 0.369, 0.000, 1 },
}
P.colourVision = {
  deuteranopia = RED_GREEN,
  protanopia = RED_GREEN,
  tritanopia = {
    hp = { 0.800, 0.200, 0.067, 1 },   -- red
    good = { 0.200, 0.733, 0.933, 1 }, -- cyan
    bad = { 0.800, 0.200, 0.067, 1 },
  },
}

return P
