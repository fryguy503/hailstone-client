#include "ui/ui_ini.h"

#include <charconv>
#include <cstdio>
#include <string>
#include <system_error>

#include "core/wrap_math.h"

namespace hailstone::ui {

namespace fs = std::filesystem;

namespace {

// The original's 32-bit int sums, which wrap: a UI.ini value near the int limits (hand-edited or broken) gives what
// the original gives, without a signed overflow here.
int add(int a, int b) {
    return static_cast<int>(core::wrapped_value(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)));
}
int sub(int a, int b) {
    return static_cast<int>(core::wrapped_value(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)));
}

} // namespace

int convert_to_res(int pos, int extent, int from, int to) {
    // Off screen at `from` (left of 1 or past the right edge): not pulled back onto the new screen.
    bool off = pos < 1 || from <= add(pos, extent);
    if (from != to) {
        int half = from / 2;
        if (half < pos) pos = add(sub(pos, from), to); // right of the middle: same gap to the right edge
        else if (half < add(pos, extent)) pos = add(sub(to / 2, half), pos); // over the middle: same offset from it
    }
    if (!off) {
        if (pos < 0) pos = 0;
        if (to < add(pos, extent)) pos = sub(to, extent);
    }
    if (add(pos, extent) < 0) pos = 0;
    if (to < pos) pos = sub(to, extent);
    return pos;
}

WindowIniInfo window_ini_info(std::string_view screen) {
    // The flags and version each window class passes to the CSidlScreenWnd constructor (the
    // CSidlScreenWnd1 calls; CSidlScreenWnd2 @0x85f120 passes 0xfffffcff, version 1) or to EnableIniStorage.
    struct Entry {
        std::string_view screen, section;
        std::uint32_t flags;
        int version;
    };
    static constexpr Entry kWindows[] = {
        {"PlayerWindow", {}, 0xffffffff, 1},      {"TargetWindow", {}, 0xffffffff, 1},
        {"GroupWindow", {}, 0xffffffff, 1},       {"BuffWindow", {}, 0xffffffff, 1},
        {"ShortDurationBuffWindow", {}, 0xffffffff, 1}, {"CastSpellWnd", {}, 0xffffffff, 1},
        {"CompassWindow", {}, 0xffffffff, 1},     {"ExtendedTargetWnd", {}, 0xffffffff, 1},
        // AuraWindow: CAuraWnd passes 0xffffffff (shown on a character with no "Show" yet). Hailstone: bit 9 off,
        // so it starts closed (a deliberate change from RoF2; the EQ menu's Auras opens it).
        {"AuraWindow", {}, 0xfffffdff, 1},        {"ActionsWindow", {}, 0xfffffdff, 1},
        {"CombatAbilityWnd", {}, 0xfffffdff, 1},  {"PetInfoWindow", {}, 0xfffffdff, 1},
        {"FriendsWindow", {}, 0xfffffdff, 1},     {"RaidWindow", {}, 0xfffffdff, 1},
        {"SelectorWindow", {}, 0xfffffdff, 1},    {"TradeskillWnd", {}, 0xfffffdff, 1},
        {"TaskWnd", {}, 0xfffffdff, 2},           {"OptionsWindow", {}, 0xfffffdff, 3},
        {"BandolierWnd", {}, 0xfffffdff, 1},      {"LeadershipWindow", {}, 0xfffffdff, 1},
        {"RespawnWnd", {}, 0xfffffdff, 1},        {"PotionBeltWnd", {}, 0xfffffdff, 1},
        {"InventoryWindow", {}, 0xfffffcff, 2},   {"MerchantWnd", {}, 0xfffffcff, 3},
        {"GuildManagementWnd", {}, 0xfffffcff, 1}, {"TipWindow", {}, 0xfffffcff, 1},
        {"EQMainWnd", {}, 0xfffffeff, 1},
        // Hailstone's recovery panel starts shown and remembers the player's choice for this character.
        {"AddonPolicy", {}, 0xffffffff, 1},
        {"MapViewWnd", {}, 0xfffffdff, 1},        {"EditLabelWnd", {}, 0xfffffdff, 1},
        // CChatWindowManager: the first chat window is "MainChat" (EnableIniStorage 0xfffffeff).
        {"Chat1", "MainChat", 0xfffffeff, 1},
    };
    for (const Entry& e : kWindows)
        if (e.screen == screen) return {std::string(e.section.empty() ? e.screen : e.section), e.flags, e.version};
    // CItemDisplayManager::CreateWindowInstance @006a1ae0: EnableIniStorage(0xfffffeff, "ItemDisplay%d") with the
    // number of displays made before it (ItemDisplayWindow is the first, ItemDisplayWindow#2 the second, ...).
    if (screen == "ItemDisplayWindow") return {"ItemDisplay0", 0xfffffeff, 1};
    if (screen.starts_with("ItemDisplayWindow#")) {
        int n = 0;
        std::from_chars(screen.data() + 18, screen.data() + screen.size(), n);
        return {"ItemDisplay" + std::to_string(n - 1), 0xfffffeff, 1};
    }
    // CHotButtonWnd: 0xfffffdff, version 2, for every bar.
    if (screen.starts_with("HotButtonWnd")) return {std::string(screen), 0xfffffdff, 2};
    return {std::string(screen), 0xfffffcff, 1};
}

void UiIni::load(const fs::path& defaults, const fs::path& file, const fs::path& seed) {
    defaults_ = {};
    file_ = {};
    defaults_.load(defaults);
    std::error_code ec;
    if (!file_.load(file) && !seed.empty() && fs::exists(seed, ec)) file_.load(seed);
    path_ = file;
    loaded_ = true;
    dirty_ = false;
    ++generation_;
}

std::optional<std::string> UiIni::read(std::string_view section, std::string_view key, bool use_ui,
                                       bool use_defaults) const {
    // CEverQuest::ReadUIINIString: the default, then defaults.ini, then the UI ini; an empty value counts as none.
    std::optional<std::string> v;
    if (use_defaults)
        if (auto d = defaults_.get(section, key); d && !d->empty()) v = d;
    if (use_ui)
        if (auto u = file_.get(section, key); u && !u->empty()) v = u;
    return v;
}

namespace {

std::optional<int> to_int(const std::optional<std::string>& s) {
    if (!s) return std::nullopt;
    int v = 0;
    const char* b = s->data();
    while (*b == ' ') ++b;
    auto [p, ec] = std::from_chars(b, s->data() + s->size(), v); // sscanf "%d"
    if (ec != std::errc()) return std::nullopt;
    return v;
}

constexpr int kWindowedRes = 0x1000; // CResolutionHandler_GetFullscreenSize while windowed

} // namespace

bool UiIni::use_ui(const WindowIniInfo& info) const {
    // LoadIniInfo: the character's file counts when the window's IniVersion is not newer than the one stored.
    int stored = to_int(read(info.section, "INIVersion", true, true)).value_or(1);
    return info.version <= stored;
}

WindowRect UiIni::place(const WindowIniInfo& info, WindowRect r, int sw, int sh, bool sizable) const {
    if (!loaded_) return r;
    bool ui = use_ui(info);
    std::uint32_t flags = info.flags;
    if (!sizable) flags &= ~2u;
    if (flags & 2) { // CSidlScreenWnd_ReadIniResExtent: "<key>1280x1024", then "<key>Windowed"; no conversion
        for (auto [key, extent, screen] : {std::tuple{"Width", &r.w, sw}, std::tuple{"Height", &r.h, sh}}) {
            if (auto v = to_int(read(info.section, std::string(key) + "1280x1024", ui, true))) *extent = *v;
            if (auto v = to_int(read(info.section, std::string(key) + "Windowed", ui, true))) *extent = *v;
            if (screen < *extent) *extent = screen; // a sizable window is no larger than the screen
        }
    }
    if (flags & 1) { // CSidlScreenWnd_ReadIniResCoord
        for (auto [key, pos, extent, ref, screen] : {std::tuple{"XPos", &r.x, r.w, 0x500, sw},
                                                     std::tuple{"YPos", &r.y, r.h, 0x400, sh}}) {
            if (auto v = to_int(read(info.section, std::string(key) + "1280x1024", ui, true)))
                *pos = convert_to_res(*v, extent, ref, screen);
            if (auto v = to_int(read(info.section, std::string(key) + "Windowed", ui, true)))
                *pos = convert_to_res(*v, extent, kWindowedRes, screen);
            if (sub(screen, *pos) < 4) *pos = screen - 4; // at least 4 pixels stay on screen
        }
    }
    return r;
}

std::optional<bool> UiIni::shown(const WindowIniInfo& info) const {
    if (!loaded_ || !(info.flags & 0x100)) return std::nullopt;
    // Defaults come from defaults.ini only for "HotButtonWnd".
    auto v = read(info.section, "Show", use_ui(info), info.section == "HotButtonWnd");
    if (!v) return std::nullopt;
    return to_int(v).value_or(0) == 1;
}

void UiIni::put(std::string_view section, std::string_view key, std::string_view value) {
    if (file_.get(section, key) != value) {
        file_.set(section, key, value);
        dirty_ = true;
    }
}

void UiIni::erase(std::string_view section, std::string_view key) {
    if (file_.get(section, key)) {
        file_.erase(section, key);
        dirty_ = true;
    }
}

void UiIni::set(std::string_view section, std::string_view key, int value) {
    std::string v = std::to_string(value);
    if (file_.get(section, key) != v) {
        file_.set(section, key, v);
        dirty_ = true;
    }
}

void UiIni::store(const WindowIniInfo& info, WindowRect r, int sw, int sh, bool sizable) {
    if (!loaded_) return;
    auto set = [&](std::string_view key, int value) { this->set(info.section, key, value); };
    set("INIVersion", info.version);
    if (info.flags & 1) { // CSidlScreenWnd_StoreIniPosition: moved to the 4096 x 4096 windowed screen
        set("XPosWindowed", convert_to_res(r.x, r.w, sw, kWindowedRes));
        set("YPosWindowed", convert_to_res(r.y, r.h, sh, kWindowedRes));
    }
    if ((info.flags & 2) && sizable) { // CSidlScreenWnd_StoreIniSize
        set("WidthWindowed", r.w);
        set("HeightWindowed", r.h);
    }
}

void UiIni::store_shown(const WindowIniInfo& info, bool shown) {
    if (loaded_ && (info.flags & 0x100)) set(info.section, "Show", shown ? 1 : 0);
}

bool UiIni::save() {
    if (!dirty_ || path_.empty()) return false;
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    if (!file_.save(path_)) return false;
    dirty_ = false;
    return true;
}

bool UiIni::save_if_due(std::chrono::steady_clock::time_point now) {
    if (!dirty_ || (last_save_ && now - *last_save_ < kSaveInterval)) return false;
    last_save_ = now;
    return save();
}

} // namespace hailstone::ui
