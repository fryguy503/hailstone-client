// The UI scripts (ui/**/*.lua): every one compiles under the bundled Lua, every generated layout runs and returns a
// table, and no two window scripts of one UI (ui/<name>, ui/ui_packs.h) register the same window name (ui.window's
// name is the script's identity; only one UI runs at a time, so the UIs may share names).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <lua.hpp>

namespace fs = std::filesystem;

namespace {

fs::path ui_dir() {
    // The scripts are in the source tree next to tests/ (this file is compiled from there).
    return fs::path(__FILE__).parent_path().parent_path() / "ui";
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

TEST_CASE("UI scripts: every script compiles, window names are unique") {
    fs::path dir = ui_dir();
    if (!fs::exists(dir)) return; // an installed test binary run away from the sources
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int scripts = 0;
    std::map<std::string, std::string> names; // "<ui>/<ui.window name>" -> script
    const std::regex window(R"re(ui\.window\(\s*"([^"]+)")re");
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".lua") continue;
        const std::string file = e.path().string();
        INFO(file);
        int rc = luaL_loadfile(L, file.c_str());
        CHECK_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
        if (rc != LUA_OK) {
            lua_pop(L, 1);
            continue;
        }
        ++scripts;
        lua_pop(L, 1);
        if (e.path().parent_path().filename() == "lib") continue; // libraries (their comments show ui.window examples)
        std::string text = slurp(e.path());
        for (std::sregex_iterator it(text.begin(), text.end(), window), end; it != end; ++it) {
            const std::string pack = fs::relative(e.path(), dir).begin()->string();
            auto [at, fresh] = names.emplace(pack + "/" + (*it)[1].str(), file);
            CHECK_MESSAGE(fresh, "window name \"" << (*it)[1].str() << "\" is used by " << at->second << " too");
        }
    }
    lua_close(L);
    CHECK(scripts > 150);
}

// Each UI's colour palettes (ui/<name>/lib/palettes.lua: `list`, `default`, `checks`): every palette resolves through
// its `base`, has every colour the default has, and keeps each check's WCAG 2.1 contrast ratio. A translucent
// background is laid over the check's `under` colour (if any) and that over black and over white, and the worse of the
// two counts (the world shows through); a translucent foreground is laid over the background.
namespace {

using Rgba = std::array<double, 4>;
using Palette = std::map<std::string, Rgba>;

Rgba read_color(lua_State* L, int idx) {
    Rgba c{0, 0, 0, 1};
    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (lua_isnumber(L, -1)) c[static_cast<std::size_t>(i)] = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return c;
}

std::string field_string(lua_State* L, int idx, const char* key) {
    lua_getfield(L, idx, key);
    std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s;
}

double channel(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double luminance(const Rgba& c) { return 0.2126 * channel(c[0]) + 0.7152 * channel(c[1]) + 0.0722 * channel(c[2]); }
Rgba over(const Rgba& top, const Rgba& under) {
    double a = top[3];
    return {top[0] * a + under[0] * (1 - a), top[1] * a + under[1] * (1 - a), top[2] * a + under[2] * (1 - a), 1};
}
double contrast(const Rgba& a, const Rgba& b) {
    double x = luminance(a), y = luminance(b);
    if (x < y) std::swap(x, y);
    return (x + 0.05) / (y + 0.05);
}

} // namespace

TEST_CASE("UI palettes: every palette is complete and keeps its UI's contrast checks") {
    fs::path dir = ui_dir();
    if (!fs::exists(dir)) return;
    int packs = 0;
    for (const auto& pack : fs::directory_iterator(dir)) {
        fs::path file = pack.path() / "lib" / "palettes.lua";
        if (!fs::exists(file)) continue;
        INFO(file.string());
        ++packs;
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        int rc = luaL_dofile(L, file.string().c_str());
        REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
        REQUIRE(lua_istable(L, -1));
        const int P = lua_gettop(L);
        // the palettes, in order, each with its base and own colours
        struct Raw { std::string id, base; Palette colors; };
        std::vector<Raw> raws;
        lua_getfield(L, P, "list");
        REQUIRE(lua_istable(L, -1));
        for (lua_Integer i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            int t = lua_gettop(L);
            Raw r{field_string(L, t, "id"), field_string(L, t, "base"), {}};
            CHECK(!r.id.empty());
            CHECK(!field_string(L, t, "name").empty());
            lua_getfield(L, t, "colors");
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) r.colors[lua_tostring(L, -2)] = read_color(L, lua_gettop(L));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 2);
            raws.push_back(std::move(r));
        }
        lua_pop(L, 1);
        REQUIRE(raws.size() >= 2);
        std::map<std::string, const Raw*> by_id;
        for (const Raw& r : raws) by_id[r.id] = &r;
        std::function<Palette(const Raw&, int)> resolve = [&](const Raw& r, int depth) {
            Palette out;
            if (!r.base.empty() && depth < 8) {
                auto it = by_id.find(r.base);
                CHECK_MESSAGE(it != by_id.end(), r.id << ": no base " << r.base);
                if (it != by_id.end()) out = resolve(*it->second, depth + 1);
            }
            for (const auto& [k, v] : r.colors) out[k] = v;
            return out;
        };
        const std::string def = field_string(L, P, "default");
        REQUIRE(by_id.count(def) == 1);
        const Palette base = resolve(*by_id[def], 0);
        // the checks
        struct Check { std::string fg, bg, under; double ratio; };
        std::vector<Check> checks;
        lua_getfield(L, P, "checks");
        REQUIRE(lua_istable(L, -1));
        for (lua_Integer i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            Check c;
            lua_rawgeti(L, -1, 1); c.fg = lua_tostring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1);
            lua_rawgeti(L, -1, 2); c.bg = lua_tostring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1);
            lua_rawgeti(L, -1, 3); c.ratio = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 4); c.under = lua_isstring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1);
            lua_pop(L, 1);
            checks.push_back(c);
        }
        lua_pop(L, 1);
        CHECK(!checks.empty());
        for (const Raw& r : raws) {
            const Palette p = resolve(r, 0);
            for (const auto& [k, v] : base) CHECK_MESSAGE(p.count(k) == 1, r.id << " has no colour " << k);
            for (const Check& c : checks) {
                if (!p.count(c.fg) || !p.count(c.bg) || (!c.under.empty() && !p.count(c.under))) {
                    CHECK_MESSAGE(false, r.id << ": check " << c.fg << " on " << c.bg << " names a missing colour");
                    continue;
                }
                double worst = 1e9;
                for (const Rgba& world : {Rgba{0, 0, 0, 1}, Rgba{1, 1, 1, 1}}) {
                    Rgba under = c.under.empty() ? world : over(p.at(c.under), world);
                    Rgba bg = over(p.at(c.bg), under);
                    worst = std::min(worst, contrast(over(p.at(c.fg), bg), bg));
                }
                CHECK_MESSAGE(worst >= c.ratio, r.id << ": " << c.fg << " on " << c.bg << " is " << worst << ":1, needs "
                                                     << c.ratio << ":1");
            }
        }
        lua_close(L);
    }
    CHECK(packs >= 1);
}

// The modern UI's default layout (ui/modern/lib/layout.lua): at every screen size and UI scale here, each frame is
// on screen, and none covers another where the screen has the room: 1000 x 600 design pixels for a new character
// (a caster: one hot bar and eight gems; a melee one with a group and a target), 1280 x 720 with every frame up at
// once. The frames' sizes are their scripts' (size() in player.lua, hotbuttons.lua, lib/bufftray.lua, ...).
TEST_CASE("UI layout: the modern UI's default places fit every screen") {
    fs::path file = ui_dir() / "modern" / "lib" / "layout.lua";
    if (!fs::exists(file)) return;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int rc = luaL_dofile(L, file.string().c_str());
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    REQUIRE(lua_istable(L, -1));
    lua_setglobal(L, "layout");
    const char* check = R"lua(
local L = layout
local function tray(n, icon, cols) -- lib/bufftray.lua's size for n icons, cols to a row
  local c, rows = math.max(1, math.min(cols, n)), math.max(1, math.ceil(n / cols))
  return c * (icon + 4) - 4 + 12, rows * (icon + 12 + 4) - 4 + 10
end
local SCENES = {
  caster = {
    HotButtonWnd = { 614, 58 }, CastSpellWnd = { 416, 56 }, PlayerWindow = { 300, 116 }, CompassWindow = { 300, 30 },
    Minimap = { 200, 248 }, EQMainWnd = { 44, 44 }, Chat1 = "fit", BuffWindow = { "tray", 1, 34 },
  },
  melee = {
    HotButtonWnd = { 614, 58 }, PlayerWindow = { 300, 104 }, TargetWindow = { 300, 84 }, CompassWindow = { 300, 30 },
    Minimap = { 200, 248 }, EQMainWnd = { 44, 44 }, Chat1 = "fit", BuffWindow = { "tray", 6, 34 },
    GroupWindow = { 210, 200 },
  },
  busy = {
    HotButtonWnd = { 614, 58 }, HotButtonWnd2 = { 614, 58 }, CastSpellWnd = { 596, 56 },
    PlayerWindow = { 300, 162 }, TargetWindow = { 300, 172 }, PetInfoWindow = { 240, 150 }, CompassWindow = { 300, 30 },
    Minimap = { 200, 248 }, EQMainWnd = { 44, 44 }, Chat1 = "fit", BuffWindow = { "tray", 30, 34 },
    ShortDurationBuffWindow = { "tray", 8, 26 }, GroupWindow = { 210, 358 }, ExtendedTargetWnd = { 210, 199 },
    AuraWindow = { 190, 80 }, PotionBeltWnd = { 228, 52 }, BreathWindow = { 240, 38 },
  },
}
local function frames(scene, s, room)
  local f = {}
  for k, v in pairs(scene) do
    if v == "fit" then
      f[k] = { w = 470 * s, h = 250 * s, fit = true }
    elseif v[1] == "tray" then
      local w, h = tray(v[2], v[3], math.min(14, math.max(1, math.floor((room / s - 12 + 4) / (v[3] + 4)))))
      f[k] = { w = w * s, h = h * s }
    else
      f[k] = { w = v[1] * s, h = v[2] * s }
    end
  end
  return f
end
local SIZES = { { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1366, 768 }, { 1440, 900 },
  { 1600, 900 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 1600 },
  { 3840, 2160 }, { 5120, 1440 }, { 5120, 2880 }, { 1080, 1920 } }
local fails, solves = {}, 0
for name, scene in pairs(SCENES) do
  for _, size in ipairs(SIZES) do
    for _, s in ipairs({ 1, 1.25, 1.5, 1.75, 2, 2.5, 3 }) do
      local W, H = size[1], size[2]
      local lw, lh = W / s, H / s
      if lw >= 600 and lh >= 500 then
        -- the trays wrap to the room the layout gives them (lib/bufftray.lua), so solve twice
        local r = L.Solve(W, H, s, frames(scene, s, L.Solve(W, H, s, frames(scene, s, 1e9)).trayRoom))
        solves = solves + 1
        local where = string.format("%s %dx%d at %.2f (%gx%g)", name, W, H, s, lw, lh)
        local keys = {}
        for k in pairs(scene) do
          if not r[k] then fails[#fails + 1] = where .. ": " .. k .. " has no place" else keys[#keys + 1] = k end
        end
        table.sort(keys)
        for _, k in ipairs(keys) do
          local a = r[k]
          if a.x < 0 or a.y < 0 or a.x + a.w > W + 0.5 or a.y + a.h > H + 0.5 then
            fails[#fails + 1] = where .. ": " .. k .. " is off the screen"
          end
        end
        if (name ~= "busy" and lw >= 1000 and lh >= 600) or (lw >= 1280 and lh >= 720) then
          for i = 1, #keys do
            for j = i + 1, #keys do
              if L.Overlap(r[keys[i]], r[keys[j]]) > 0 then
                fails[#fails + 1] = where .. ": " .. keys[i] .. " covers " .. keys[j]
              end
            end
          end
        end
        -- a second chat window goes beside the first, the bottoms in line
        local x, y = L.Place(r, "Chat2", 300 * s, 200 * s, s)
        if not x or y + 200 * s > r.Chat1.y + r.Chat1.h + 0.5 then
          fails[#fails + 1] = where .. ": Chat2 is not placed"
        end
        if L.Place(r, "SomePluginFrame", 100, 100, s) ~= nil then
          fails[#fails + 1] = "a frame it does not know was placed"
        end
      end
    end
  end
end
return fails, solves
)lua";
    rc = luaL_loadstring(L, check);
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    rc = lua_pcall(L, 0, 2, 0);
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    CHECK(lua_tointeger(L, -1) > 200);
    REQUIRE(lua_istable(L, -2));
    const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -2));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -2, i);
        CHECK_MESSAGE(false, std::string(lua_tostring(L, -1)));
        lua_pop(L, 1);
    }
    CHECK(n == 0);
    lua_close(L);
}

// A rolled-up chat (its tab strip only) keeps just its strip's height in the default layout, so the room the open
// chat would take is the screen's again.
TEST_CASE("UI layout: a rolled-up chat keeps only its tab strip's room") {
    fs::path file = ui_dir() / "modern" / "lib" / "layout.lua";
    if (!fs::exists(file)) return;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int rc = luaL_dofile(L, file.string().c_str());
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    lua_setglobal(L, "layout");
    const char* code = R"lua(
local L = layout
local function solve(rolled)
  local frames = {
    Chat1 = { w = 470, h = 26, fit = true, rolled = rolled },
    HotButtonWnd = { w = 614, h = 58 }, PlayerWindow = { w = 300, h = 116 },
    CompassWindow = { w = 300, h = 30 }, EQMainWnd = { w = 44, h = 44 },
  }
  local r = L.Solve(1280, 800, 1, frames)
  return r.Chat1.w, r.Chat1.h, r.Chat1.y
end
local rw, rh, ry = solve(true)
local ow, oh, oy = solve(false)
return rw, rh, ry, ow, oh, oy
)lua";
    rc = luaL_loadstring(L, code);
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    rc = lua_pcall(L, 0, 6, 0);
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    const double oy = lua_tonumber(L, -1), oh = lua_tonumber(L, -2), ow = lua_tonumber(L, -3);
    const double ry = lua_tonumber(L, -4), rh = lua_tonumber(L, -5), rw = lua_tonumber(L, -6);
    CHECK(rw == doctest::Approx(ow));          // still the width the layout gives the chat
    CHECK(rh == doctest::Approx(26));          // (the strip)
    CHECK(oh > rh);
    CHECK(ry > oy);                            // it sits lower, and the room above it is free
    lua_close(L);
}

// The modern UI's grid snapping (ui/modern/lib/snap.lua): a dragged window's near or far edge goes to the nearest
// grid line, or flush with the screen's edge when that is nearer, in whole pixels.
TEST_CASE("UI snap: window positions and resize edges land on the grid") {
    fs::path file = ui_dir() / "modern" / "lib" / "snap.lua";
    if (!fs::exists(file)) return;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int rc = luaL_dofile(L, file.string().c_str());
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    REQUIRE(lua_istable(L, -1));
    lua_setglobal(L, "snap");
    auto axis = [&](double raw, double size, double screen, double grid) {
        lua_getglobal(L, "snap");
        lua_getfield(L, -1, "Axis");
        lua_pushnumber(L, raw);
        lua_pushnumber(L, size);
        lua_pushnumber(L, screen);
        lua_pushnumber(L, grid);
        REQUIRE(lua_pcall(L, 4, 1, 0) == LUA_OK);
        double v = lua_tonumber(L, -1);
        lua_pop(L, 2);
        return v;
    };
    // the near edge: 103 -> 100, 106 -> 110
    CHECK(axis(103, 50, 1000, 10) == 100);
    CHECK(axis(106, 50, 1000, 10) == 110);
    // the far edge nearer a line: left 102, right 149 -> the right edge on 150
    CHECK(axis(102, 47, 1000, 10) == 103);
    // the screen's right edge (996 is no grid line) when it is nearer than either edge's line, not otherwise
    CHECK(axis(754, 240, 996, 10) == 756);
    CHECK(axis(752, 240, 996, 10) == 750);
    // a fractional grid (10 design pixels at a 125 % UI scale) gives whole pixels; a tie keeps the near edge
    CHECK(axis(37, 40, 1000, 12.5) == 38);
    CHECK(axis(30, 40, 1000, 12.5) == 25);
    // no grid: unchanged
    lua_getglobal(L, "snap");
    lua_getfield(L, -1, "Axis");
    lua_pushnumber(L, 33.5);
    lua_pushnumber(L, 10);
    lua_pushnumber(L, 100);
    lua_pushnil(L);
    REQUIRE(lua_pcall(L, 4, 1, 0) == LUA_OK);
    CHECK(lua_tonumber(L, -1) == 33.5);
    // a resize: the dragged far edge goes to the nearest grid line, or the screen's far edge when that is nearer
    auto extent = [&](double origin, double size, double delta, double minSize, double scale, double screen,
                      double grid) {
        lua_getglobal(L, "snap");
        lua_getfield(L, -1, "Extent");
        lua_pushnumber(L, origin);
        lua_pushnumber(L, size);
        lua_pushnumber(L, delta);
        lua_pushnumber(L, minSize);
        lua_pushnumber(L, scale);
        lua_pushnumber(L, screen);
        if (grid > 0) lua_pushnumber(L, grid);
        else lua_pushnil(L);
        REQUIRE(lua_pcall(L, 7, 1, 0) == LUA_OK);
        const double v = lua_tonumber(L, -1);
        lua_pop(L, 2);
        return v;
    };
    CHECK(extent(103, 50, 4, 20, 1, 1000, 10) == 57);        // off-grid origin; far edge 157 -> 160
    CHECK(extent(103, 50, -45, 20, 1, 1000, 10) == 20);      // minimum size wins over the grid
    // a fractional screen grid (10 design pixels at 125 %): 20 + 5 / 1.25 = 24 puts the far edge at 131, nearer the
    // line at 125 than 137.5, so it pulls back to (125 - 101) / 1.25; 10 pixels on (edge 136) it reaches 137.5
    CHECK(extent(101, 20, 5, 10, 1.25, 1000, 12.5) == doctest::Approx(19.2));
    CHECK(extent(101, 20, 10, 10, 1.25, 1000, 12.5) == doctest::Approx(29.2));
    // the screen's right edge (196 is no grid line): flush with it when nearer than the line, the line on a tie
    CHECK(extent(103, 50, 42, 20, 1, 196, 10) == 93);        // edge 195: 196 is nearer than 200
    CHECK(extent(103, 50, 40, 20, 1, 196, 10) == 87);        // edge 193: 190 and 196 tie, the line wins
    CHECK(extent(10, 40, 9, 20, 1.25, 63.4, 12.5) == doctest::Approx((63.4 - 10) / 1.25)); // edge 69 -> 63.4
    CHECK(extent(103, 50, 3, 20, 1.25, 1000, 0) == doctest::Approx(52.4)); // Shift or snap off: raw resize
    lua_close(L);
}

// The modern UI's libraries (lib/hud.lua, lib/theme.lua, lib/style.lua) run here over a stand-in `ui` and `eq`: no
// ImGui, the frame count, the screen, the settings and the game's windows as each check sets them. ui.Begin answers
// false, so a frame is placed and noted but its body is not drawn. Each chunk returns a list of failures.
namespace {

const char* kModernStubs = R"lua(
local base = ...
local real_dofile = dofile
solves = 0
dofile = function(p)
  local m = assert(loadfile(base .. "/" .. p))()
  if p == "lib/layout.lua" then -- counted, to see when the layout is worked out
    local solve = m.Solve
    m.Solve = function(...) solves = solves + 1 return solve(...) end
  end
  return m
end
local function noop() end
local function numbers() -- ui.Col and the like: a distinct number for each name
  local n = 0
  return setmetatable({}, { __index = function(t, k) n = n + 1 rawset(t, k, n) return n end })
end
settings, frame, gets, colorDepth, varDepth = {}, 1, 0, 0, 0
io_ = { width = 1600, height = 900, uiScale = 1, fps = 60, mouseDX = 0, mouseDY = 0 }
ui = setmetatable({
  GetSetting = function(k, d) gets = gets + 1 local v = settings[k] if v == nil then return d end return v end,
  SetSetting = function(k, v) settings[k] = type(v) == "boolean" and (v and "1" or "0") or tostring(v) end,
  GetFrameCount = function() return frame end,
  GetTime = function() return frame / 60 end,
  GetIO = function() return io_ end,
  GetMousePos = function() return 0, 0 end,
  Begin = function() return false end,
  CalcTextSize = function(t) return 7 * (utf8.len(t) or #t), 13 end,
  PushStyleColors = function(l) local n = #l // 5 colorDepth = colorDepth + n pushedColors = l return n end,
  PushStyleVars = function(l) local n = #l // 3 varDepth = varDepth + n return n end,
  PushStyleColor = function() colorDepth = colorDepth + 1 end,
  PushStyleVar = function() varDepth = varDepth + 1 end,
  PopStyleColor = function(n) colorDepth = colorDepth - (n or 1) end,
  PopStyleVar = function(n) varDepth = varDepth - (n or 1) end,
  WindowFlags = numbers(), Cond = numbers(), StyleVar = numbers(), Col = numbers(), Mouse = numbers(),
  HoveredFlags = numbers(),
}, { __index = function() return noop end })
shownWindows = {}
eq = setmetatable({
  ReplaceWindow = function(w) return shownWindows[w] == true, 100, 100 end,
  WindowShown = function(w) return shownWindows[w] == true end,
}, { __index = function() return noop end })
)lua";

std::vector<std::string> run_modern(const char* chunk) {
    std::vector<std::string> fails;
    const fs::path base = ui_dir() / "modern";
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    for (const char* code : {kModernStubs, chunk}) {
        int rc = luaL_loadstring(L, code);
        if (rc == LUA_OK) {
            lua_pushstring(L, base.string().c_str());
            rc = lua_pcall(L, 1, 1, 0);
        }
        if (rc != LUA_OK) {
            fails.emplace_back(lua_tostring(L, -1));
            break;
        }
        if (lua_istable(L, -1)) {
            const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                fails.emplace_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_close(L);
    return fails;
}

} // namespace

// With Snap to grid on, dragging a resizable frame's corner puts its right and bottom edges on the nearest grid line,
// or flush with the screen's edge when that is nearer, with the grid shown (lib/hud.lua over lib/snap.lua). A frame
// that lays itself out from the drag (opts.onResize: the hot bars, whole buttons) gets the mouse's size unsnapped
// and shows no grid, since its own rounding would leave it off the lines.
TEST_CASE("UI modern HUD: a frame's resize snaps to the grid and screen edge, not a self-laid-out one") {
    if (!fs::exists(ui_dir() / "modern" / "lib" / "hud.lua")) return;
    const char* check = R"lua(
local hud = dofile("lib/hud.lua")
local fails = {}
local function expect(ok, what) if not ok then fails[#fails + 1] = what end end
local last, active, mx, my, wpx, wpy = nil, nil, 0, 0, 0, 0
ui.Begin = function() return true end
ui.SetNextWindowPos = function(x, y) wpx, wpy = x, y end
ui.GetWindowPos = function() return wpx, wpy end
ui.GetScrollY = function() return 0 end
ui.InvisibleButton = function(id) last = id return false end
ui.IsItemActive = function() return active ~= nil and last == active end
ui.IsMouseDown = function() return active ~= nil end
ui.IsMouseDragging = function() return active ~= nil end
ui.GetMousePos = function() return mx, my end
settings["hud.snap"], settings["hud.snap.size"] = "1", "10"
local got
local framed = { size = function() return 200, 100 end, resizable = { 50, 50 } }
local own = { size = function() return 200, 100 end, resizable = { 50, 50 }, onResize = function(w, h) got = { w, h } end }
local function drag(name, opts, dx, dy) -- the corner grip pressed at (400, 400), then moved by dx, dy
  shownWindows[name] = true
  settings["hud.pos." .. name] = "103,203"
  settings["hud.size." .. name] = nil
  active, mx, my = nil, 400, 400
  frame = frame + 1
  hud.Window(name, name, opts, function() end)
  active = "##resize"
  frame = frame + 1
  hud.Window(name, name, opts, function() end)
  mx, my = 400 + dx, 400 + dy
  frame = frame + 1
  hud.Window(name, name, opts, function() end)
  local shown = modern.gridFrame == frame
  active = nil
  frame = frame + 1
  hud.Window(name, name, opts, function() end)
  return shown
end
-- far edges 103 + 244 = 347 -> 350 and 203 + 100 = 303 -> 300
expect(drag("SnapFrame", framed, 44, 0), "no grid shown while a frame is resized")
expect(settings["hud.size.SnapFrame"] == "247.000,97.000", "frame size " .. tostring(settings["hud.size.SnapFrame"]))
-- the screen's right edge at 1596: an edge dragged to 1594 goes flush with it (2 away), not to the line at 1590
io_.width = 1596
drag("SnapFrame", framed, 1291, 0)
expect(settings["hud.size.SnapFrame"] == "1493.000,97.000", "edge size " .. tostring(settings["hud.size.SnapFrame"]))
io_.width = 1600
-- Shift held: the size as the mouse put it
io_.shift = true
drag("SnapFrame", framed, 44, 0)
expect(settings["hud.size.SnapFrame"] == "244,100", "Shift size " .. tostring(settings["hud.size.SnapFrame"]))
io_.shift = false
-- the hot bar's kind: unsnapped, no grid
expect(not drag("OwnFrame", own, 44, 3), "the grid shown for a frame that lays itself out")
expect(got and got[1] == 244 and got[2] == 103, "onResize got " .. (got and (got[1] .. "," .. got[2]) or "nothing"))
return fails
)lua";
    for (const auto& f : run_modern(check)) FAIL_CHECK(f);
}

// hud.Layout keeps its solve from frame to frame, and works it out again when a frame shows, changes size or goes
// unseen for more than 2 frames (a hidden frame gives its room back then, not before), or the screen or the HUD's
// scale changes. What it gives is what a solve of the frames drawn now gives.
TEST_CASE("UI modern HUD: the default layout is kept until what it was worked out from changes") {
    if (!fs::exists(ui_dir() / "modern" / "lib" / "hud.lua")) return;
    const char* check = R"lua(
local hud = dofile("lib/hud.lua")
local layout = dofile("lib/layout.lua")
local fails = {}
local function expect(ok, what) if not ok then fails[#fails + 1] = what end end
local SIZES = { PlayerWindow = { 300, 116 }, PetInfoWindow = { 240, 150 }, HotButtonWnd = { 614, 58 } }
local opts = {}
for k, v in pairs(SIZES) do
  shownWindows[k] = true
  opts[k] = { size = function() return v[1], v[2] end }
end
local function step()
  frame = frame + 1
  for _, k in ipairs({ "PlayerWindow", "PetInfoWindow", "HotButtonWnd" }) do
    hud.Window(k, k, opts[k], function() end)
  end
end
local function fresh() -- a solve of the frames drawn in the last 2 frames
  local f = {}
  for k, v in pairs(SIZES) do
    local s = io_.uiScale * hud.Scale()
    if shownWindows[k] then f[k] = { w = math.floor(v[1] * s + 0.5), h = math.floor(v[2] * s + 0.5) } end
  end
  return layout.Solve(io_.width, io_.height, io_.uiScale * hud.Scale(), f)
end
local function same(a, b, what)
  for k, r in pairs(b) do
    if type(r) == "table" then
      local o = a[k]
      expect(o and o.x == r.x and o.y == r.y and o.w == r.w and o.h == r.h, what .. ": " .. k .. " differs")
    end
  end
  for k in pairs(a) do expect(b[k] ~= nil, what .. ": " .. k .. " is placed and should not be") end
end
for _ = 1, 3 do step() end
local n = solves
for _ = 1, 10 do step() end
expect(solves == n, "worked out again with nothing changed (" .. (solves - n) .. " times)")
same(hud.Layout(), fresh(), "steady")
-- the pet frame hides: its room is kept for 2 frames, then given back
shownWindows.PetInfoWindow = false
step()
expect(hud.Layout().PetInfoWindow ~= nil, "a frame hidden 1 frame ago lost its room")
step()
expect(hud.Layout().PetInfoWindow ~= nil, "a frame hidden 2 frames ago lost its room")
n = solves
step()
expect(hud.Layout().PetInfoWindow == nil, "a frame hidden 3 frames ago keeps its room")
expect(solves > n, "not worked out again when a frame went unseen")
same(hud.Layout(), fresh(), "after the pet frame hid")
n = solves
for _ = 1, 5 do step() end
expect(solves == n, "worked out again every frame while a frame stays hidden")
-- it shows again
shownWindows.PetInfoWindow = true
step()
expect(solves > n, "not worked out again when a frame showed")
expect(hud.Layout().PetInfoWindow ~= nil, "a frame shown again has no place")
same(hud.Layout(), fresh(), "after the pet frame showed again")
-- a frame changes size
n = solves
SIZES.PlayerWindow[2] = 162
step()
expect(solves > n, "not worked out again when a frame changed size")
same(hud.Layout(), fresh(), "after the player frame grew")
-- the screen, the UI scale, the HUD scale
for _, change in ipairs({
  function() io_.width = 1920 end, function() io_.height = 1200 end, function() io_.uiScale = 1.5 end,
  function() ui.SetSetting("hud.scale", "1.25") end }) do
  n = solves
  change()
  step()
  expect(solves > n, "not worked out again when the screen or a scale changed")
  same(hud.Layout(), fresh(), "after the screen or a scale changed")
end
-- the style pushed around each frame is popped again
expect(colorDepth == 0 and varDepth == 0, "style pushes left over: " .. colorDepth .. " colours, " .. varDepth .. " vars")
return fails
)lua";
    for (const std::string& f : run_modern(check)) CHECK_MESSAGE(false, f);
}

// T.Fit cuts a text to a width with "...": the longest start that fits (as the one character at a time search it
// replaced found it), never inside a UTF-8 character, at least the first character.
TEST_CASE("UI modern theme: T.Fit cuts to the longest start that fits, between characters") {
    if (!fs::exists(ui_dir() / "modern" / "lib" / "theme.lua")) return;
    const char* check = R"lua(
local T = dofile("lib/theme.lua")
local fails = {}
local function expect(ok, what) if not ok then fails[#fails + 1] = what end end
local function width(t) return (ui.CalcTextSize(t)) end
local function old(text, w) -- the search T.Fit had: one byte shorter at a time
  if width(text) <= w then return text end
  local n = #text
  while n > 1 do
    n = n - 1
    if width(text:sub(1, n) .. "...") <= w then break end
  end
  return text:sub(1, n) .. "..."
end
for _, text in ipairs({ "a", "ab", "Fippy Darkpaw", "a_very_long_name_of_a_spawn_00", "Guard Hanns" }) do
  for w = 0, 260 do
    local got, want = T.Fit(text, w), old(text, w)
    expect(got == want, string.format("%q at %d: %q, was %q", text, w, got, want))
  end
end
for _, text in ipairs({ "\xC3\x84rger \xC3\xBCber \xC3\x96l", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE",
    "\xF0\x9F\x90\x89 dragon \xF0\x9F\x90\x89", "\xC3\x84" }) do
  local chars = utf8.len(text)
  for w = 0, 200 do
    local got = T.Fit(text, w)
    expect(utf8.len(got) ~= nil, string.format("%q at %d: %q splits a character", text, w, got))
    if width(text) <= w then
      expect(got == text, string.format("%q at %d: cut though it fits", text, w))
    else
      local kept = utf8.len(got) - 3
      expect(got:sub(-3) == "...", string.format("%q at %d: no dots", text, w))
      expect(kept >= 1 and (kept < chars or chars == 1), string.format("%q at %d: kept %d characters", text, w, kept))
      -- the longest that fits: one more character would not (unless only the first fits at all)
      if kept > 1 then expect(width(got) <= w, string.format("%q at %d: %q is too wide", text, w, got)) end
      if kept + 1 < chars then
        local longer = text:sub(1, utf8.offset(text, kept + 2) - 1) .. "..."
        expect(width(longer) > w, string.format("%q at %d: %q is not the longest", text, w, got))
      end
    end
  end
end
return fails
)lua";
    for (const std::string& f : run_modern(check)) CHECK_MESSAGE(false, f);
}

// style.Push pushes the palette's look from a list worked out once, and again when the palette changes; ui.SetSetting
// is seen at once by T.Setting, which otherwise reads a key once a frame.
TEST_CASE("UI modern style: the pushed look follows a palette change; settings are read once a frame") {
    if (!fs::exists(ui_dir() / "modern" / "lib" / "style.lua")) return;
    const char* check = R"lua(
local T = dofile("lib/theme.lua")
local style = dofile("lib/style.lua")
local style2 = dofile("lib/style.lua") -- another script's copy shares the list
local fails = {}
local function expect(ok, what) if not ok then fails[#fails + 1] = what end end
local function colorOf(list, name)
  local idx = ui.Col[name]
  for i = 1, #list, 5 do
    if list[i] == idx then return { list[i + 1], list[i + 2], list[i + 3], list[i + 4] } end
  end
end
local function same(a, b) return a and b and a[1] == b[1] and a[2] == b[2] and a[3] == b[3] and a[4] == b[4] end
style.Push()
local first = pushedColors
expect(same(colorOf(first, "CheckMark"), T.color.accent), "CheckMark is not the accent")
expect(same(colorOf(first, "Text"), T.color.text), "Text is not the text colour")
style.Pop()
style2.Push()
expect(rawequal(pushedColors, first), "the list was worked out again with no palette change")
style2.Pop()
expect(colorDepth == 0 and varDepth == 0, "a pop left pushes over")
for _, p in ipairs(T.palettes.list) do
  if p.id ~= T.Palette() then
    T.SetPalette(p.id)
    break
  end
end
style.Push()
expect(not rawequal(pushedColors, first), "a palette change did not work the list out again")
expect(same(colorOf(pushedColors, "CheckMark"), T.color.accent), "CheckMark is not the new palette's accent")
local nested = pushedColors
style.Push() -- nested pushes pop what each pushed
local depth = colorDepth
style.Pop()
expect(colorDepth == depth // 2, "a nested pop popped the wrong count")
style.Pop()
expect(colorDepth == 0 and varDepth == 0, "nested pops left pushes over")
-- a client without the batch calls: one call each, the same counts
local batchColors, batchVars = #nested // 5, 0
ui.PushStyleColors = false -- (nil would find the stand-in's catch-all)
style.Push()
expect(colorDepth == batchColors, "one at a time pushed " .. colorDepth .. " colours, not " .. batchColors)
style.Pop()
expect(colorDepth == 0 and varDepth == 0, "one at a time: pops left pushes over")
-- settings: one read a frame; a write is seen at once
settings["hud.test"] = "a"
local before = gets
expect(T.Setting("hud.test") == "a", "T.Setting did not read the setting")
expect(T.Setting("hud.test") == "a" and gets == before + 1, "read twice in one frame")
ui.SetSetting("hud.test", "b")
expect(T.Setting("hud.test") == "b", "a write in the frame was not seen")
expect(settings["hud.test"] == "b", "the write did not reach the settings")
expect(T.Setting("hud.none") == nil, "a key with no value is not nil")
before = gets
T.Setting("hud.none")
expect(gets == before, "a key with no value was read again in the frame")
settings["hud.test"] = "c" -- (written around the cache: seen the next frame)
frame = frame + 1
expect(T.Setting("hud.test") == "c", "the next frame did not read the setting again")
return fails
)lua";
    for (const std::string& f : run_modern(check)) CHECK_MESSAGE(false, f);
}

// The tradeskill and training windows' "Name" sort (ui/modern/tradeskill.lua, train.lua): rows of one name keep
// the list's order. P.Rows sorts again every frame and table.sort is not stable, so without the tie-break two such
// rows swapped places from frame to frame. The script runs over stubs (every call returns a stand-in) with the sort
// combo set to "Name", and the comparator it hands P.Rows is checked.
TEST_CASE("UI sorts: the tradeskill and training windows' Name sort breaks ties by the row") {
    for (const char* script : {"tradeskill.lua", "train.lua"}) {
        fs::path file = ui_dir() / "modern" / script;
        if (!fs::exists(file)) return;
        INFO(script);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        const char* stubs = R"lua(
local any
local function self() return any end
local function many() return any, any, any, any, any end
any = setmetatable({}, {
  __index = self, __call = many, __concat = function() return "" end, __len = function() return 0 end,
  __add = self, __sub = self, __mul = self, __div = self, __mod = self, __idiv = self, __pow = self, __unm = self,
  __lt = function() return false end, __le = function() return false end,
})
local function stub(t) return setmetatable(t, { __index = self }) end
windows, sorts = {}, {}
ui = stub({
  window = function(name, fn) windows[name] = fn end,
  BeginCombo = function() return true end,
  Selectable = function(label) return label == "Name" end,
})
eq = stub({
  InGame = function() return true end,
  ListInfo = function() return nil end,
  ListRow = function() return nil end,
})
local libs = {
  ["lib/hud.lua"] = stub({ Dialog = function(_, _, _, body) body(0, 0, 640, 560, 1) end }),
  ["lib/progress.lua"] = stub({ Rows = function(_, _, _, _, opts) sorts[#sorts + 1] = opts.sort or false end }),
}
dofile = function(name) return libs[name] or any end
)lua";
        REQUIRE(luaL_dostring(L, stubs) == LUA_OK);
        int rc = luaL_dofile(L, file.string().c_str());
        REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
        const char* check = R"lua(
local _, fn = next(windows)
fn() -- the first frame picks "Name" in the sort combo
fn()
local sort = sorts[#sorts]
assert(sort, "no sort handed to P.Rows")
local a, b, c = { row = 0, texts = { "Bone Chips" } }, { row = 3, texts = { "bone chips" } },
  { row = 1, texts = { "Acid" } }
return sort(a, b), sort(b, a), sort(c, a), sort(a, c)
)lua";
        rc = luaL_dostring(L, check);
        REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
        CHECK(lua_toboolean(L, -4));       // one name: the earlier row first
        CHECK_FALSE(lua_toboolean(L, -3));
        CHECK(lua_toboolean(L, -2));       // by name otherwise, ignoring case
        CHECK_FALSE(lua_toboolean(L, -1));
        lua_close(L);
    }
}

// The ability book (ui/modern/abilitybook.lua) sorted by kind with an ability of a kind it does not know: the window
// still draws (the kind sorts after the known ones) instead of stopping with an error in table.sort. Run over stubs.
TEST_CASE("UI ability book: an ability of an unknown kind sorts last instead of stopping the window") {
    fs::path file = ui_dir() / "modern" / "abilitybook.lua";
    if (!fs::exists(file)) return;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    const char* stubs = R"lua(
local any
local function self() return any end
local function many() return any, any, any, any, any end
any = setmetatable({}, {
  __index = self, __call = many, __concat = function() return "" end, __len = function() return 0 end,
  __add = self, __sub = self, __mul = self, __div = self, __mod = self, __idiv = self, __pow = self, __unm = self,
  __lt = function() return false end, __le = function() return false end,
})
local function stub(t) return setmetatable(t, { __index = self }) end
local settings = { ["abilitybook.sort"] = "Kind" }
windows = {}
ui = stub({
  window = function(name, fn) windows[name] = fn end,
  GetSetting = function(k) return settings[k] end,
  SetSetting = function() end,
  InputText = function(_, text) return text end,
  BeginCombo = function() return false end,
  BeginPopup = function() return false end,
})
local function ability(key, kind)
  return { key = key, name = key, description = "", kind = kind, timer = -1, reuse = 0, remaining = 0, level = 1,
    endurance = 0, mana = 0, classes = 0, favorite = false, ready = true, autoable = false, maxRank = 1, rank = 1,
    icon = -1, spell = 0 }
end
eq = stub({
  InGame = function() return true end,
  AbilityBook = function()
    return { ability("Kick", "skill"), ability("Newfangled", "something new"), ability("Fury", "aa") }
  end,
  AbilityBookState = function()
    return { classes = { { id = 1, name = "Warrior" } }, autoskill = { on = false, allowed = true, held = false, list = {} } }
  end,
})
local libs = {
  ["lib/hud.lua"] = stub({ Dialog = function(_, _, _, body) body(0, 0, 960, 620, 1) end }),
  ["lib/stml.lua"] = stub({ Lines = function() return {} end }),
}
dofile = function(name) return libs[name] or any end
)lua";
    REQUIRE(luaL_dostring(L, stubs) == LUA_OK);
    int rc = luaL_dofile(L, file.string().c_str());
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    rc = luaL_dostring(L, "local _, fn = next(windows) fn() fn()");
    CHECK_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    lua_close(L);
}

// The player and group frames (ui/modern/player.lua, group.lua) keep their tables and the class line and health text
// they made between frames; what they draw still follows the game from one frame to the next, the player frame's
// auto attack ring included. Run over stubs: the
// game's values change between two frames and the drawn text and bars are checked.
TEST_CASE("UI HUD frames: the player and group frames follow the game from frame to frame") {
    for (const char* script : {"player.lua", "group.lua"}) {
        fs::path file = ui_dir() / "modern" / script;
        if (!fs::exists(file)) return;
        INFO(script);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        const char* stubs = R"lua(
local any
local function self() return any end
local function many() return any, any, any, any, any end
any = setmetatable({}, {
  __index = self, __call = many, __concat = function() return "" end, __len = function() return 0 end,
  __add = self, __sub = self, __mul = self, __div = self, __mod = self, __idiv = self, __pow = self, __unm = self,
  __lt = function() return false end, __le = function() return false end,
})
local function stub(t) return setmetatable(t, { __index = self }) end
windows, texts, bars, labels, gauges, colors, children = {}, {}, {}, {}, {}, {}, {}
ui = stub({ window = function(name, fn) windows[name] = fn end, GetTime = function() return 0 end })
eq = stub({
  InGame = function() return true end,
  Label = function(n) return labels[n] end,
  Gauge = function(n) local g = gauges[n] if g then return table.unpack(g) end return nil, nil, -1 end,
  ChildShown = function(_, id) return children[id] == true end,
  ChildState = function() return false end,
  ChildTooltip = function() return "" end,
  ChildColor = function(_, id) local c = colors[id] if c then return table.unpack(c) end end,
  ChildArt = function() return nil end,
  ReplaceWindow = function() return false end,
  Name = function() return "Me" end,
})
local T = stub({
  font = stub({}),
  Text = function(_, _, text, color) texts[#texts + 1] = { text = text, color = color } end,
  TextSize = function() return 0, 0 end,
  Fit = function(text) return text end,
  Hit = function() return false end,
  Button = function() return false end,
  Bar = function(key, _, _, _, _, _, fraction, _, opts)
    bars[key] = { fraction = fraction, text = opts and opts.text, right = opts and opts.right }
  end,
  Thousands = function(n)
    local s = tostring(math.floor(tonumber(n) or 0))
    local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
    return (out:gsub("^,", ""):gsub("^%-,", "-"))
  end,
})
T.color = stub({})
local hud = stub({
  T = T,
  DragItem = function() return false end,
  Window = function(_, _, opts, body)
    panelGlow = opts.panelOpts and opts.panelOpts.glow
    opts.size(1)
    body(0, 0, 300, 140, 1)
  end,
})
dofile = function(name) if name == "lib/hud.lua" then return hud end return any end
function frame()
  texts, bars, panelGlow = {}, {}, nil
  local _, fn = next(windows)
  fn()
  local drawn = {}
  for _, t in ipairs(texts) do drawn[t.text] = t end
  return drawn
end
)lua";
        REQUIRE(luaL_dostring(L, stubs) == LUA_OK);
        int rc = luaL_dofile(L, file.string().c_str());
        REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
        const char* player = R"lua(
labels[2], labels[3], labels[17], labels[18] = "60", "Warrior/Cleric", "1234", "5678"
gauges[1], gauges[2] = { 0.5, "Tester" }, { 0.25, nil, 0 }
local d = frame()
assert(d["Warrior / Cleric"], "the class line")
assert(bars["player.hp"].text == "1,234 / 5,678", tostring(bars["player.hp"].text))
assert(bars["player.hp"].right == "50%", tostring(bars["player.hp"].right))
assert(bars["player.mana"].fraction == 0.25, "the mana bar")
labels[3], labels[17] = "Wizard", "999"
gauges[1], gauges[2] = { 0.2, "Tester" }, { 0.75, nil, 0 }
d = frame()
assert(d["Wizard"] and not d["Warrior / Cleric"], "the class line follows the label")
assert(bars["player.hp"].text == "999 / 5,678", tostring(bars["player.hp"].text))
assert(bars["player.hp"].right == "20%", tostring(bars["player.hp"].right))
assert(bars["player.mana"].fraction == 0.75, "the mana bar follows the gauge")
gauges[2] = { 0, nil, -1 } -- not a caster
labels[17] = ""
frame()
assert(bars["player.mana"] == nil, "no mana bar for a non-caster")
assert(bars["player.hp"].text == nil, "no health text without the label")
assert(panelGlow == nil, "no glow without auto attack")
children["A_AttackIndicatorAnimTop"] = true -- auto attack on
frame()
assert(panelGlow and panelGlow[4] >= 0.55, "a ring while auto attack is on")
children["A_AttackIndicatorAnimTop"] = nil
frame()
assert(panelGlow == nil, "the ring goes when auto attack goes off")
return true
)lua";
        const char* group = R"lua(
gauges[11], gauges[12] = { 0.5, "Alpha", 0 }, { 1, "Beta", 0 }
gauges[31], gauges[32] = { 0.4, nil, 0 }, { 0.9, nil, 0 }
colors["Gauge1"] = { 0, 1, 0 }
local d = frame()
assert(d["Alpha"] and d["Beta"], "both members")
assert(d["Alpha"].color[2] == 1, "the leader's colour")
assert(d["3 / 6"] and d["50%"] and d["100%"], "the count and the members' health")
assert(bars["group.mana1"] and bars["group.mana2"], "both mana bars")
gauges[11], gauges[12] = { 0.3, "Gamma", -2 }, { 0, nil, -1 }
gauges[31] = { 0, nil, -1 }
colors["Gauge1"] = nil
d = frame()
assert(d["Gamma"] and not d["Alpha"] and not d["Beta"], "the members follow the game")
assert(d["Gamma"].color[2] ~= 1, "no stale leader colour")
assert(d["2 / 6"] and d["Away"], "the count and an away member")
assert(not bars["group.mana1"] and not bars["group.mana2"], "no stale mana bars")
assert(bars["group.hp1"].fraction == 0.3, "the health bar follows the gauge")
gauges[11] = { 0, nil, -1 }
d = frame()
assert(d["Not in a group"], "solo")
return true
)lua";
        rc = luaL_dostring(L, std::string(script) == "player.lua" ? player : group);
        CHECK_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
        lua_close(L);
    }
}

// The tracker reads the same task snapshot as the journal without consulting TaskWnd's visibility or selection.
// Keep its interaction checks separate from the shared HUD's placement, lock and resize checks above.
TEST_CASE("UI quest tracker: independent tracked objectives and journal links") {
    if (!fs::exists(ui_dir() / "modern" / "questtracker.lua")) return;
    auto fails = run_modern(R"lua(
local base = ...
local hud = dofile("lib/hud.lua")
local Q = dofile("lib/task_journal.lua")
local real_dofile = dofile
local texts, buttons, window, opts, pending = {}, {}, nil, nil, nil
local cx, cy, calls, opened, journal = 0, 0, 0, nil, nil
local inGame, openAllowed = true, true
local snapshot = {
  { id = 10, type = 2, title = "First quest", duration = 600, left = 128, done = 1, total = 6, objectives = {
    { hidden = true, text = "Hidden secret" },
    { text = "Already done", count = 1, goal = 1, done = true },
    { separator = true },
    { text = "First known", count = 1, goal = 4, zone = "North Ro" },
    { text = "Second known", count = 0, goal = 1, zone = "Befallen" },
    { text = "Third known", count = 2, goal = 3, zone = "Najena" },
    { text = "Fourth known", count = 0, goal = 1, zone = "Upper Guk" },
  } },
  { id = 20, type = 1, title = "Another quest", objectives = { { text = "Other objective", count = 2, goal = 8 } } },
}
eq.InGame = function() return inGame end
eq.Tasks = function() calls = calls + 1 return snapshot end
eq.WindowShown = function() error("the tracker must not inspect TaskWnd") end
eq.ReplaceWindow = eq.WindowShown
eq.OpenTask = function(id) opened = id return openAllowed end
eq.ShowWindow = function(name, shown) journal = { name, shown } end
ui.window = function(name, fn) assert(name == "Quest Tracker") window = fn end
ui.GetWindowPos = function() return 0, 0 end
ui.GetCursorScreenPos = function() return cx, cy end
ui.GetCursorPos = ui.GetCursorScreenPos
ui.SetCursorPos = function(x, y) cx, cy = x, y end
ui.SetCursorScreenPos = ui.SetCursorPos
ui.GetContentRegionAvail = function() return 282, 250 end
ui.IsItemHovered = function() return false end
ui.TextColored = function(_, _, _, _, text) texts[text] = true cy = cy + 16 end
ui.Separator = function() cy = cy + 4 end
hud.T.Text = function(_, _, text) texts[text] = true end
hud.T.Hit = function(id) if pending == id then pending = nil return true end return false end
hud.T.Button = function(id, _, _, _, _, _, label, options)
  buttons[id] = label
  return hud.T.Hit(id) and (not options or options.enabled ~= false)
end
hud.DragItem = function() return false end
hud.Window = function(screen, title, options, body)
  assert(screen == nil and title == "Quest Tracker", "an independent HUD frame")
  opts = options
  assert(options.resizable[1] >= 200 and options.resizable[2] >= 80, "bounded resizing")
  assert(options.rollup and options.onClose, "the HUD owns rolling and closing")
  if options.shown() then
    options.rolled = hud.Rolled(title)
    body(0, 0, 300, options.rolled and options.titleHeight or 350, 1)
  end
end
dofile = function(name) if name == "lib/hud.lua" then return hud end return real_dofile(name) end
assert(loadfile(base .. "/questtracker.lua"))()
local function draw(click)
  texts, buttons, pending, cx, cy = {}, {}, click, 0, 0
  frame = frame + 1
  window()
  return texts
end
draw()
assert(next(texts) == nil, "an empty tracker is not shown by default")
Q.setTrackerVisible(true)
draw()
assert(texts["No tracked quests. Use Track in the Quest Journal to add one."], "explicitly showing an empty tracker offers help")
Q.setTrackerVisible(false)
Q.setTracked(10, true)
Q.setTracked(20, true)
local before = calls
local drawn = draw()
assert(drawn["First quest"] and drawn["Another quest"], "both tracked quests")
assert(drawn["First known"] and drawn["Second known"] and drawn["Third known"], "the first three incomplete objectives")
assert(not drawn["Fourth known"] and not drawn["Hidden secret"] and not drawn["Already done"], "compact visible unfinished rows only")
assert(drawn["1/4"] and drawn["2/3"] and drawn["North Ro"] and drawn["Befallen"], "counts and zones")
assert(drawn["2m 08s remaining"] and drawn["1/6"], "time and total progress")
assert(calls == before + 1, "the snapshot is read once in a frame")
ui.SetSetting("tasks.section.20", "rewards")
draw("##quest20")
assert(opened == 20, "the quest title selects that quest")
assert(settings["tasks.section.20"] == "objectives" and settings["tasks.category"] == "active",
  "a shared quest title opens its objectives with the other active tasks")
local beforeRefresh = calls
assert(Q.tasks() == snapshot and calls == beforeRefresh + 1, "opening a quest invalidates the same-frame snapshot")
ui.SetSetting("tasks.section.10", "rewards")
draw("##all10")
assert(opened == 10, "all objectives opens the correct quest")
assert(settings["tasks.section.10"] == "objectives" and settings["tasks.category"] == "active",
  "all objectives opens objectives in the active category")
ui.SetSetting("tasks.section.10", "rewards")
openAllowed = false
draw("##all10")
assert(settings["tasks.section.10"] == "rewards", "a failed open preserves section preferences")
openAllowed = true
draw("##managequests")
assert(journal[1] == "TaskWnd" and journal[2] == true, "the journal can be restored from the tracker")
draw("##collapse10")
assert(settings["tasks.tracker.collapsed.10"] == "1" and not texts["First known"], "per-quest collapse")
assert(texts["Other objective"], "collapsing one quest leaves another expanded")
draw()
assert(not texts["First known"], "collapse is remembered")
draw("##collapse10")
assert(texts["First known"] and settings["tasks.tracker.collapsed.10"] == "0", "per-quest expand")
draw("##trackerlock")
assert(settings["hud.lock.Quest Tracker"] == "1", "the shared HUD lock setting")
draw("##trackerlock")
assert(settings["hud.lock.Quest Tracker"] == "0", "unlock")
draw("##trackerroll")
assert(settings["hud.rolled.Quest Tracker"] == "1", "the shared HUD roll-up setting")
draw()
assert(not texts["First quest"] and buttons["##trackerroll"] == "+", "rolled up shows only its header")
draw("##trackerroll")
draw()
assert(texts["First known"], "expanded contents return")
opts.onClose()
draw()
assert(not texts["First quest"] and Q.isTracked(10) and Q.isTracked(20), "hiding never clears watch flags")
Q.setTrackerVisible(true)
draw()
assert(texts["First quest"], "hidden tracker can be shown again")
snapshot[1].objectives[4].done = true
draw()
assert(not texts["First known"] and texts["Fourth known"], "progress changes refresh the compact objective set")
Q.setTracked(10, false)
draw()
assert(not texts["First quest"] and texts["Another quest"], "untracking removes only that quest")
snapshot = {}
draw()
assert(texts["No tracked quests. Use Track in the Quest Journal to add one."], "stale watch flags do not create stale tasks")
local prior = calls
inGame = false
draw()
assert(calls == prior and next(texts) == nil, "no tracker outside the game")
return {}
)lua");
    CHECK_MESSAGE(fails.empty(), (fails.empty() ? "" : fails.front()));
}

TEST_CASE("UI task journal: shared snapshots and settings-backed tracking") {
    if (!fs::exists(ui_dir() / "modern" / "lib" / "task_journal.lua")) return;
    auto fails = run_modern(R"lua(
local Q = dofile("lib/task_journal.lua")
local other = dofile("lib/task_journal.lua")
local calls = 0
local snapshot = { { id = 10 }, { id = 20 }, { id = 30 } }
eq.Tasks = function() calls = calls + 1 return snapshot end
assert(Q.tasks() == other.tasks() and calls == 1, "both windows share one snapshot per frame")
frame = frame + 1
snapshot = { { id = 10 }, { id = 30 } }
assert(Q.tasks() == snapshot and calls == 2, "a new frame reads current tasks")
Q.setTracked(20, true)
Q.setTracked(10, true)
local tracked = other.getTracked(Q.tasks())
assert(#tracked == 1 and tracked[1].id == 10, "watch flags cannot keep a removed task visible")
assert(Q.trackerVisible(), "tracking a quest shows the tracker")
other.setTrackerVisible(false)
assert(not Q.trackerVisible() and Q.isTracked(10) and Q.isTracked(20), "visibility is independent of tracking")
Q.setTracked(10, false)
assert(not other.isTracked(10) and Q.isTracked(20), "watch flags are shared through settings")
local done, total = Q.stats({ objectives = {
  { done = true }, { done = false }, { hidden = true, done = true }, { separator = true },
} })
assert(done == 1 and total == 3, "hidden and separator rows do not count as finished objectives")
assert(Q.time(65) == "1m 05s" and Q.time(3600) == "1h 00m", "task time formatting")
assert(Q.inZone({objectives={{currentZone=true,hidden=true},{currentZone=true,done=true}}}) == false,
  "hidden or completed objectives do not make a task current-zone")
assert(Q.inZone({objectives={{currentZone=false},{currentZone=true,done=false}}}),
  "a known incomplete objective can make a task current-zone")
return {}
)lua");
    CHECK_MESSAGE(fails.empty(), (fails.empty() ? "" : fails.front()));
}

// The modern chat's item links in its input (ui/modern/lib/chatlinks.lua): tags shown as names, and back as tags when
// the line is sent. The same item linked twice once found its second name inside the first tag put back and sent a
// tag in a tag.
TEST_CASE("UI chat links: each name goes back as its own tag, the same item twice included") {
    fs::path file = ui_dir() / "modern" / "lib" / "chatlinks.lua";
    if (!fs::exists(file)) return;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int rc = luaL_dofile(L, file.string().c_str());
    REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
    REQUIRE(lua_istable(L, -1));
    lua_setglobal(L, "links");
    const char* check = R"lua(
local function tag(name, digit) return "\18" .. "0" .. string.rep(digit, 76) .. name .. "\18" end
local sword, shield = tag("Rusty Sword", "1"), tag("Shield", "2")
local results = {}
local function run(typed)
  local l = {}
  local shown = links.Collapse(l, typed)
  return shown, links.Expand(l, shown)
end
-- one link, two of the same item, two items, an item whose name is in another's, text after
local cases = {
  "WTS " .. sword .. " cheap",
  sword .. " and " .. sword,
  sword .. " " .. shield .. " " .. sword,
  tag("Shield", "3") .. " or " .. tag("Shield of Ages", "4"),
  tag("Shield of Ages", "4") .. " or " .. tag("Shield", "3"),
  "plain text",
}
for i, typed in ipairs(cases) do
  local shown, sent = run(typed)
  results[#results + 1] = (sent == typed) and "ok" or ("case " .. i .. ": " .. sent:gsub("\18", "^"))
end
-- the names as shown
results[#results + 1] = select(1, run(sword .. " and " .. sword))
-- a name moved before the other in the input: still found
local l = {}
links.Collapse(l, sword .. " " .. shield)
results[#results + 1] = (links.Expand(l, "Shield Rusty Sword") == shield .. " " .. sword) and "ok" or "moved"
-- the name typed as plain text before the link, or inside a longer word: the tag goes where the link is
local ring = tag("Ring", "5")
for i, typed in ipairs({ "Ring for " .. ring, "Bring me " .. ring, "WTS " .. sword .. ", Rusty Sword " .. sword }) do
  local shown, sent = run(typed)
  results[#results + 1] = (sent == typed) and "ok" or ("placed " .. i .. ": " .. sent:gsub("\18", "^"))
end
-- a link put in after text the input held already, then a word typed before it: still the link's own place
l = {}
local shown = "Ring for " .. links.Collapse(l, ring, #"Ring for ")
results[#results + 1] = (links.Expand(l, shown) == "Ring for " .. ring) and "ok" or "offset"
l = {}
shown = "Bring me " .. links.Collapse(l, ring, #"Bring me ")
results[#results + 1] = (links.Expand(l, "Hi, " .. shown) == "Hi, Bring me " .. ring) and "ok" or "edited"
-- the wide and the stock tag in one line, in the order they came
local stock = "\18" .. "0" .. string.rep("a", 55) .. "Old Ring" .. "\18"
l = {}
links.Collapse(l, stock .. " " .. ring)
results[#results + 1] = (l[1].name == "Old Ring" and l[2].name == "Ring" and l[1].at == 1 and l[2].at == 10) and "ok"
  or "order"
return table.concat(results, "|")
)lua";
    REQUIRE(luaL_dostring(L, check) == LUA_OK);
    CHECK(std::string(lua_tostring(L, -1)) == "ok|ok|ok|ok|ok|ok|Rusty Sword and Rusty Sword|ok|ok|ok|ok|ok|ok|ok");
    lua_close(L);
}
