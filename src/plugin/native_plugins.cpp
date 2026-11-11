#include "plugin/native_plugins.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

#include <SDL3/SDL_loadso.h>

#include "core/ascii_case.h"
#include "core/atomic_file.h"
#include "core/log.h"
#include "core/distribution.h"
#include "core/sha256.h"
#include "game/chat_text.h"
#include "game/combat.h"
#include "game/item_links.h"
#include "game/server_ext_chain.h"
#include "game/spells.h"
#include "game/zone_names.h"
#include "mq/parse.h"
#include "mq/tlo.h"
#include "net/game_connection.h"
#include "net/social_packets.h"
#include "plugin/plugin_tcp.h"
#include "plugin/ui_bridge.h"
#include "plugin/web_access.h"

#define HAILSTONE_HOST
#include "hailstone_plugin.h"
#include "hailstone_builtin_addon.h"

namespace hailstone::plugin {

namespace fs = std::filesystem;

namespace {

static_assert(HAILSTONE_CAP_READ == kCapRead && HAILSTONE_CAP_COMMANDS == kCapCommands && HAILSTONE_CAP_TARGET == kCapTarget &&
              HAILSTONE_CAP_ACTIONS == kCapActions && HAILSTONE_CAP_MOVE == kCapMove && HAILSTONE_CAP_CHAT == kCapChat &&
              HAILSTONE_CAP_EVENTS == kCapEvents && HAILSTONE_CAP_UI == kCapUi && HAILSTONE_CAP_PACKETS == kCapPackets &&
              HAILSTONE_CAP_EXTEND == kCapExtend && HAILSTONE_CAP_LOGIN == kCapLogin && HAILSTONE_CAP_NET == kCapNet &&
              HAILSTONE_CAP_WEB == kCapWeb);

#if defined(_WIN32)
constexpr const char* kExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kExt = ".dylib";
#else
constexpr const char* kExt = ".so";
#endif

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

mq::Value from_c(const HailstoneValue& v) {
    switch (v.kind) {
    case HAILSTONE_BOOL: return mq::Value::boolean(v.i != 0);
    case HAILSTONE_INT: return mq::Value::integer(v.i);
    case HAILSTONE_FLOAT: return mq::Value::number(v.f);
    case HAILSTONE_STRING: return mq::Value::string(v.s ? v.s : "");
    default: return mq::Value::nil();
    }
}

} // namespace

std::string plugin_name(const fs::path& file) {
    std::string stem = file.stem().string();
    if (stem.size() > 3 && stem.compare(0, 3, "lib") == 0) stem.erase(0, 3);
    return stem;
}

namespace {
// The plugin binaries of the directories, in order: those directly in each directory, then those in its
// subdirectories one level down (a plugin in its own folder, plugins/<name>/<name>.so).
std::vector<fs::path> plugin_files(const std::vector<fs::path>& dirs) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& d : dirs) {
        std::vector<fs::path> sub;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == kExt) out.push_back(e.path());
            else if (e.is_directory(ec)) sub.push_back(e.path());
        }
        std::sort(sub.begin(), sub.end());
        for (const auto& s : sub)
            for (const auto& e : fs::directory_iterator(s, ec))
                if (e.is_regular_file(ec) && e.path().extension() == kExt) out.push_back(e.path());
    }
    return out;
}
} // namespace

struct Loaded;
class NativeExtension;

namespace {
HailstoneSpawn spawn_struct(const net::GameConnection& c, const game::Spawn& s);
template <std::size_t N> void copy_name(char (&dst)[N], const std::string& src);
void discard(NativePlugins::Impl& m, std::unique_ptr<Loaded> plugin, bool shutdown);
} // namespace

struct NativePlugins::Impl {
    NativeEnv env;
    NativePlugins* self = nullptr;
    Hooks::Owner owner = 0;
    std::vector<std::unique_ptr<Loaded>> loaded;
    std::vector<Info> failures; // the last load error by name

    Loaded* find(std::string_view name) const;
    std::optional<fs::path> file_of(std::string_view name) const;
    // Where plugins are looked for: the server's plugins/ (set_server_dir) first, then env.dirs.
    fs::path server_dir;
    std::vector<fs::path> dirs() const {
        std::vector<fs::path> out;
        if (!server_dir.empty()) out.push_back(server_dir);
        out.insert(out.end(), env.dirs.begin(), env.dirs.end());
        return out;
    }
    // One of the server's own plugins under its hailstone.cfg: the player cannot unload it.
    bool kept(const Sha256Digest& hash) const {
        return env.policy && env.policy->current() && env.policy->current()->configured &&
               env.policy->server_file(FileKind::Plugin, hash);
    }
    void say(const std::string& text) const {
        if (env.print) env.print(text, 0);
    }
    void command(std::string_view args);
    bool attach(std::unique_ptr<Loaded> plugin, void (*set_host)(const PluginHostApi*), std::string* why);

    // The player's word on the server's plugins (plugin/consent.h): the ones waiting for it, and answers that could
    // not be written to servers/<profile>/plugin_approvals.txt (they hold for this run only).
    std::vector<ConsentFile> pending;
    std::uint32_t consent_serial = 0;
    std::vector<PluginApproval> session_answers;
    // A file from the server's plugins/ folder (set_server_dir): it loads only with the player's word.
    bool asks(const fs::path& file) const {
        if (server_dir.empty()) return false;
        const fs::path rel = file.lexically_relative(server_dir);
        return !rel.empty() && *rel.begin() != "..";
    }
    fs::path profile_dir() const { return server_dir.parent_path(); }
    Consent consent_for(const Sha256Digest& hash) const {
        Consent c = plugin_consent(profile_dir(), hash);
        if (c == Consent::Unasked)
            for (const auto& a : session_answers)
                if (a.sha256 == hash) c = a.allowed ? Consent::Allowed : Consent::Denied;
        return c;
    }
    // Adds a plugin to the question; false when it is in it already. An older version's entry is replaced.
    bool ask(ConsentFile f) {
        for (const auto& p : pending)
            if (p.sha256 == f.sha256) return false;
        std::erase_if(pending, [&](const ConsentFile& p) { return iequal(p.name, f.name); });
        pending.push_back(std::move(f));
        ++consent_serial;
        return true;
    }
};

// One loaded plugin: its library, the host table it was given, and its callbacks.
struct Loaded {
    NativePlugins::Impl* host = nullptr;
    std::string name;
    fs::path file, copy;
    fs::path builtin_ui_dir; // supplied only by the host, never by a descriptor or server/settings file
    Sha256Digest hash{};
    std::uint32_t caps = 0;
    SDL_SharedObject* lib = nullptr;
    bool builtin = false;
    PluginHostApi table{};
    Hooks::Owner owner = 0;
    std::vector<std::string> tlos;

    void (*initialize)() = nullptr;
    void (*shutdown)() = nullptr;
    void (*pulse)() = nullptr;
    void (*game_state)(int) = nullptr;
    void (*begin_zone)() = nullptr;
    void (*end_zone)() = nullptr;
    void (*zoned)() = nullptr;
    void (*add_spawn)(std::uint32_t) = nullptr;
    void (*remove_spawn)(std::uint32_t) = nullptr;
    int (*incoming_chat)(const char*, std::uint32_t) = nullptr;
    int (*filter_chat)(const char*, std::uint32_t) = nullptr;
    void (*update_imgui)() = nullptr;
    int (*packet)(int, int, HailstonePacket*) = nullptr;
    const HailstoneHooks* (*hooks_fn)() = nullptr;

    // ABI 2, the server extension: the plugin's hook table (fields past its size stay null), what its setters set,
    // and its member of env.extensions.
    HailstoneHooks hooks{};
    bool active = true;
    std::uint32_t flags = 0;
    std::optional<HailstoneVitals> vitals;
    std::shared_ptr<NativeExtension> extension;
    int chain_id = 0;
    bool extend_denied = false; // said once that it lacks HAILSTONE_CAP_EXTEND
    bool read_denied = false;   // ... HAILSTONE_CAP_READ (the readers)
    bool web_denied = false;    // ... HAILSTONE_CAP_WEB (web_*)
    std::atomic<bool> thread_warned{false}; // said once that it called the host from another thread
    std::vector<std::string> threw_at;       // the callbacks that let an exception out (said once each)

    // next_spawn's walk: the zone's spawn ids in order, made again at a walk's start (after_id 0) and whenever the
    // spawns are not the ones it was made from (WorldState::spawn_set_serial moved, another count, or an id gone),
    // so a walk is n log n, not n^2.
    std::vector<std::uint32_t> spawn_ids;
    const void* spawn_ids_of = nullptr; // the spawn map they came from
    std::uint64_t spawn_ids_serial = 0;

    // The packet OnPacket is looking at (resize_packet's target).
    net::AppPacket* current_packet = nullptr;
    int busy = 0;              // inside one of its callbacks
    bool unload_pending = false; // unload asked for while busy: done at the next pulse

    // Its TCP connections (tcp_open ...), closed with it.
    struct Tcp {
        std::int32_t id = 0;
        std::unique_ptr<PluginTcp> conn;
    };
    std::vector<std::unique_ptr<Tcp>> tcp;
    std::int32_t next_tcp = 1;

    static Loaded& of(void* h) { return *static_cast<Loaded*>(h); }
    bool can(std::uint32_t cap) const { return (caps & cap) == cap; }
    void denied(const char* what, std::uint32_t cap) const {
        host->say(std::format("[Plugin] {}: {} needs the server's '{}' permission", name, what, caps_text(cap)));
    }
    void threw(const char* where, const char* what) {
        if (std::find(threw_at.begin(), threw_at.end(), where) != threw_at.end()) return;
        if (threw_at.empty()) host->say(std::format("[Plugin] {}: {} failed with an error (see the log)", name, where));
        threw_at.emplace_back(where);
        log::warn("plugin {}: {} let an exception out: {}", name, where, what);
    }
};

// Inside a plugin's code: its capabilities, and it stays loaded until this ends.
struct Busy {
    explicit Busy(Loaded& l) : l_(l), scope_(l.caps, l.name) { ++l_.busy; }
    ~Busy() { --l_.busy; }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;

private:
    Loaded& l_;
    CallerScope scope_;
};

// Runs f, a call into the plugin, with its capabilities (Busy). A plugin must not let a C++ exception out of its
// code (sdk/hailstone_plugin.h); one that does is caught here and the call answers R{} (nothing, 0, NULL, false). That is
// as far as it goes: an exception from a plugin built with another compiler or runtime may not unwind through the
// C interface into the client at all.
template <typename F> auto guarded(Loaded& l, const char* where, F&& f) -> decltype(f()) {
    using R = decltype(f());
    try {
        Busy busy(l);
        return f();
    } catch (const std::exception& e) {
        l.threw(where, e.what());
    } catch (...) {
        l.threw(where, "not a std::exception");
    }
    if constexpr (!std::is_void_v<R>) return R{};
}

// A plugin's hook table, switches and vitals as one server extension (game/server_ext.h), a member of
// env.extensions. Unloading detaches it first: the chain may hold it a little longer (a member removed while the
// chain runs), and the item display may keep its level line; detached, it gives the stock answers.
class NativeExtension final : public game::ServerExtension {
public:
    explicit NativeExtension(Loaded& l) : l_(&l) {
        rules_.ctx = this;
        rules_.gate = [](const void* ctx, std::int32_t skill, std::uint32_t mask,
                         std::int32_t profile_skill) -> std::optional<bool> {
            const auto& x = *static_cast<const NativeExtension*>(ctx);
            int has = 0;
            if (x.l_ && x.h().skill_gate && x.run(x.h().skill_gate, skill, mask, profile_skill, &has)) return has != 0;
            return std::nullopt;
        };
        rules_.spoof_class = [](const void* ctx, std::int32_t skill, std::uint32_t mask) -> std::uint32_t {
            const auto& x = *static_cast<const NativeExtension*>(ctx);
            return x.l_ && x.h().skill_spoof_class ? x.run(x.h().skill_spoof_class, skill, mask) : 0;
        };
        rules_.tracking_allowed = [](const void* ctx, std::uint8_t class_id, std::uint32_t mask, std::uint32_t skill) {
            const auto& x = *static_cast<const NativeExtension*>(ctx);
            int out = 0;
            if (x.l_ && x.h().tracking_allowed && x.run(x.h().tracking_allowed, class_id, mask, skill, &out))
                return out != 0;
            return game::tracking_allowed()(class_id, mask, skill);
        };
    }
    void detach() { l_ = nullptr; }
    bool active() const { return l_ && l_->active; }
    const char* name() const override { return l_ ? l_->name.c_str() : "unloaded plugin"; }

    bool retry_zone_entry(net::GameConnection&) override {
        return l_ && h().zone_entry_retry && run(h().zone_entry_retry) != 0;
    }
    bool handler_command(net::GameConnection&, std::string_view handler, const std::string& args) override {
        if (!l_ || !h().game_command) return false;
        std::string name(handler);
        return run(h().game_command, name.c_str(), args.c_str()) != 0;
    }
    std::optional<std::vector<std::string>> who_lines(const net::GameConnection&,
                                                      const net::WhoResponse& r) const override {
        if (!l_ || !h().who_lines) return std::nullopt;
        std::vector<HailstoneWhoEntry> entries;
        for (const auto& e : r.entries)
            entries.push_back({e.name.c_str(), e.guild.c_str(), e.zone_name.c_str(), e.account.c_str(), e.flags,
                               e.class_, e.level, e.race, e.zone_id, e.format, e.rank_string, e.zone_string});
        HailstoneWho w{};
        w.size = sizeof(HailstoneWho);
        w.entry_size = sizeof(HailstoneWhoEntry);
        w.count = static_cast<std::uint32_t>(entries.size());
        w.entries = entries.data();
        w.title_string = r.title_string;
        w.line = r.line.c_str();
        w.footer_string = r.footer_string;
        w.footer_count = r.footer_count;
        for (int i = 0; i < 3; ++i) w.footer_values[i] = r.footer_values[i];
        std::vector<std::string> lines;
        HailstoneLineFn add = [](void* ctx, const char* line) {
            static_cast<std::vector<std::string>*>(ctx)->emplace_back(line ? line : "");
        };
        if (!run(h().who_lines, &w, add, static_cast<void*>(&lines))) return std::nullopt;
        return lines;
    }

    std::uint8_t cast_class(const net::GameConnection&) const override {
        return l_ && h().cast_class ? run(h().cast_class) : std::uint8_t{0};
    }
    std::optional<bool> is_spellcaster(const net::GameConnection&) const override {
        int out = 0;
        if (l_ && h().is_spellcaster && run(h().is_spellcaster, &out)) return out != 0;
        return std::nullopt;
    }
    std::optional<std::uint8_t> spell_level_needed(const net::GameConnection&, const game::SpellData& s,
                                                   std::uint32_t class_id) const override {
        std::uint8_t out = 0;
        if (l_ && h().spell_level_needed && run(h().spell_level_needed, s.id, class_id, &out)) return out;
        return std::nullopt;
    }
    bool song_blocked(const net::GameConnection&, std::uint8_t raw_bard_level) const override {
        return l_ && h().song_blocked && run(h().song_blocked, raw_bard_level) != 0;
    }
    bool own_begin_cast_line(const net::GameConnection&, const game::SpellData& s) const override {
        return l_ && h().own_begin_cast_line && run(h().own_begin_cast_line, s.id) != 0;
    }
    std::optional<bool> can_use_gem(const net::GameConnection&, int gem) const override {
        int out = 0;
        if (l_ && h().can_use_gem && run(h().can_use_gem, gem, &out)) return out != 0;
        return std::nullopt;
    }
    std::int32_t combat_ability_timer(const net::GameConnection& c, const game::SpellData& s) const override {
        std::int32_t out = 0;
        if (l_ && h().ability_timer_id && run(h().ability_timer_id, s.id, &out)) return out;
        return ServerExtension::combat_ability_timer(c, s);
    }
    std::optional<std::uint32_t> discipline_reuse_timer(const net::GameConnection&, std::int32_t id) const override {
        std::uint32_t out = 0;
        if (l_ && h().discipline_reuse && run(h().discipline_reuse, id, &out)) return out;
        return std::nullopt;
    }
    std::optional<bool> before_combat_ability(net::GameConnection&, const game::SpellData& s) override {
        int allowed = 1;
        if (l_ && h().before_combat_ability && run(h().before_combat_ability, s.id, &allowed)) return allowed != 0;
        return std::nullopt;
    }
    std::optional<bool> before_use_skill(net::GameConnection&, std::uint32_t skill) override {
        int allowed = 1;
        if (l_ && h().before_use_skill && run(h().before_use_skill, skill, &allowed)) return allowed != 0;
        return std::nullopt;
    }
    void after_combat_ability(net::GameConnection&, const game::SpellData& s, bool used) override {
        if (l_ && h().after_combat_ability) run(h().after_combat_ability, s.id, used ? 1 : 0);
    }
    bool reuse_focus_dropped(const net::GameConnection&, const game::SpellData& s) const override {
        return l_ && h().reuse_focus_dropped && run(h().reuse_focus_dropped, s.id) != 0;
    }
    std::optional<game::MapSpawnMark> map_spawn(const net::GameConnection& c, const game::Spawn& s,
                                                const game::MapSpawnMark& defaults) const override {
        if (!l_ || !h().map_spawn) return std::nullopt;
        const HailstoneSpawn spawn = spawn_struct(c, s);
        HailstoneMapSpawn m{};
        m.size = sizeof m;
        m.color = defaults.rgb;
        m.layer = defaults.layer;
        copy_name(m.label, defaults.label);
        if (!run(h().map_spawn, &spawn, &m)) return std::nullopt;
        m.label[sizeof m.label - 1] = '\0';
        return game::MapSpawnMark{m.color & 0xffffffu, std::clamp(m.layer, 0, 3), m.label};
    }
    std::optional<std::string> combat_effect_label(const net::GameConnection&) const override {
        if (!l_ || !h().combat_effect_label) return std::nullopt;
        const char* t = run(h().combat_effect_label);
        return t ? std::optional<std::string>(t) : std::nullopt;
    }
    bool can_use_all_items(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_ITEMS_ANY_CLASS); }
    bool wide_item_links(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_WIDE_ITEM_LINKS); }
    std::uint32_t guild_member_classes(const net::GameConnection&, std::uint32_t class_value) const override {
        return l_ && h().guild_member_classes ? run(h().guild_member_classes, class_value) : 0;
    }
    std::uint8_t hide_check_class(const net::GameConnection&, std::uint8_t class_id) const override {
        return flag(HAILSTONE_FLAG_HIDE_ANY_CLASS) ? std::uint8_t{9} : class_id; // 9 Rogue: sneaking keeps Hide
    }
    bool aa_any_level(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_AA_ANY_LEVEL); }
    bool aa_train_all(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_AA_TRAIN_ALL); }
    bool augment_classes(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_AUGMENT_CLASSES); }
    bool research_classes(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_RESEARCH_CLASSES); }
    bool focus_classes(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_FOCUS_CLASSES); }
    bool backstab_either_hand(const net::GameConnection&) const override {
        return flag(HAILSTONE_FLAG_BACKSTAB_EITHER_HAND);
    }
    bool solo_adventure(const net::GameConnection&) const override { return flag(HAILSTONE_FLAG_SOLO_ADVENTURE); }
    std::vector<std::uint8_t> max_vitals_classes(const net::GameConnection&, std::uint8_t class_id) const override {
        std::uint32_t mask = l_ && h().vitals_classes ? run(h().vitals_classes, class_id) : 0;
        if (!mask) return {class_id};
        std::vector<std::uint8_t> out;
        for (std::uint8_t k = 1; k <= 16; ++k)
            if ((mask >> (k - 1)) & 1u) out.push_back(k);
        return out;
    }
    game::CharacterVitals character_vitals(const net::GameConnection&) const override {
        game::CharacterVitals v;
        if (!l_ || !l_->vitals) return v;
        const HailstoneVitals& n = *l_->vitals;
        auto set = [&](std::optional<std::int32_t>& f, std::uint32_t bit, std::int32_t value) {
            if (n.has & bit) f = value;
        };
        set(v.max_hp, HAILSTONE_VITAL_MAX_HP, n.max_hp);
        set(v.cur_hp, HAILSTONE_VITAL_CUR_HP, n.cur_hp);
        set(v.max_mana, HAILSTONE_VITAL_MAX_MANA, n.max_mana);
        set(v.cur_mana, HAILSTONE_VITAL_CUR_MANA, n.cur_mana);
        set(v.max_endurance, HAILSTONE_VITAL_MAX_ENDURANCE, n.max_endurance);
        set(v.cur_endurance, HAILSTONE_VITAL_CUR_ENDURANCE, n.cur_endurance);
        return v;
    }
    const game::SkillRules* skill_rules(const net::GameConnection&) const override {
        if (!l_ || !(h().skill_gate || h().skill_spoof_class || h().tracking_allowed)) return nullptr;
        return &rules_;
    }
    game::LevelNeededLine level_needed_line(const net::GameConnection&) const override {
        if (!l_ || !h().item_level_line) return {};
        // Kept by the item display: through the shared pointer the chain holds, detached after an unload.
        std::shared_ptr<NativeExtension> self = l_->extension;
        return [self](const game::SpellData& s) -> std::optional<std::string> {
            if (!self->l_ || !self->h().item_level_line) return std::nullopt;
            const char* t = self->run(self->h().item_level_line, s.id);
            return t ? std::optional<std::string>(t) : std::nullopt;
        };
    }
    void label_text(const net::GameConnection&, int id, std::string& text) const override {
        if (!l_ || !h().label) return;
        if (const char* t = run(h().label, id, text.c_str())) text = t;
    }
    bool hide_xtarget_slot(const net::GameConnection&, int slot) const override {
        return l_ && h().hide_xtarget_slot && run(h().hide_xtarget_slot, slot) != 0;
    }
    bool item_right_click(net::GameConnection&, const net::ItemSlot& slot) override {
        if (!l_ || !h().item_right_click) return false;
        HailstoneItemSlot s{slot.type, slot.slot, slot.sub, slot.aug};
        return run(h().item_right_click, static_cast<const HailstoneItemSlot*>(&s)) != 0;
    }
    std::optional<std::string> merchant_level(const net::GameConnection&, const net::Item& item) const override;

private:
    const HailstoneHooks& h() const { return l_->hooks; }
    bool flag(std::uint32_t f) const { return l_ && (l_->flags & f) != 0; }
    // Calls into the plugin with its capabilities, not unloaded under it; an exception it lets out is the stock
    // answer (R{}).
    template <typename R, typename... P, typename... A> R run(R (*fn)(P...), A&&... a) const {
        return guarded(*l_, "a server extension hook", [&]() -> R { return fn(static_cast<P>(a)...); });
    }

    Loaded* l_;
    game::SkillRules rules_{};
};

Loaded* NativePlugins::Impl::find(std::string_view name) const {
    for (const auto& l : loaded)
        if (iequal(l->name, name)) return l.get();
    return nullptr;
}

std::optional<fs::path> NativePlugins::Impl::file_of(std::string_view name) const {
    for (const auto& f : plugin_files(dirs()))
        if (iequal(plugin_name(f), name)) return f;
    return std::nullopt;
    
}

namespace {

   

    
// ---------------------------------------------------------------- the host table

// A string into the plugin's buffer (cut to fit, NUL-terminated); returns its full length.
int copy_out(const std::string& s, char* out, std::size_t out_size) {
    if (out && out_size) {
        std::size_t n = std::min(s.size(), out_size - 1);
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
    }
    return static_cast<int>(s.size());
}

// A directory for the plugin's files, created now.
int dir_out(const fs::path& dir, char* out, std::size_t out_size) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return copy_out(dir.string(), out, out_size);
}

std::uint32_t h_caps(void* h) { return Loaded::of(h).caps; }
const char* h_name(void* h) { return Loaded::of(h).name.c_str(); }

void h_log(void* h, int level, const char* text) {
    std::string line = std::format("plugin {}: {}", Loaded::of(h).name, text ? text : "");
    switch (level) {
    case 0: log::debug("{}", line); break;
    case 2: log::warn("{}", line); break;
    case 3: log::error("{}", line); break;
    default: log::info("{}", line); break;
    }
}

void h_write_chat(void* h, const char* text, std::uint32_t color) {
    Loaded& l = Loaded::of(h);
    if (l.host->env.print && text) l.host->env.print(text, color);
}

void h_do_command(void* h, const char* line) {
    Loaded& l = Loaded::of(h);
    if (!line || !l.host->env.run) return;
    CallerScope scope(l.caps, l.name);
    l.host->env.run(line);
}

// MQ's AddCommand, with the 'commands' capability. The client's own commands are not the plugin's to take
// (Hooks::reserved_command); another plugin's or script's name is taken over, as in MQ.
int h_add_command(void* h, const char* name, HailstoneCommandFn fn, void* user) {
    Loaded& l = Loaded::of(h);
    if (!name || !fn) return 0;
    if (!l.can(kCapCommands)) {
        l.denied("AddCommand", kCapCommands);
        return 0;
    }
    if (l.host->env.hooks->reserved_command(name)) {
        l.host->say(std::format("[Plugin] {}: AddCommand({}) refused: it is the client's own command", l.name, name));
        return 0;
    }
    Loaded* lp = &l;
    return l.host->env.hooks->add_command(
               l.owner, name,
               [lp, fn, user](std::string_view args) {
                   std::string a(args);
                   guarded(*lp, "a command", [&] { fn(user, a.c_str()); });
               },
               kCapCommands)
               ? 1
               : 0;
}

void h_remove_command(void* h, const char* name) {
    Loaded& l = Loaded::of(h);
    if (name) l.host->env.hooks->remove_command(l.owner, name);
}

int h_parse(void* h, const char* text, char* out, std::size_t out_size) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapRead)) {
        l.denied("ParseMacroData", kCapRead);
        return -1;
    }
    if (!text || !l.host->env.context) return -1;
    return copy_out(mq::parse_macro_data(l.host->env.context(), text), out, out_size);
}

int h_add_tlo(void* h, const char* name, HailstoneTloFn fn, void* user) {
    Loaded& l = Loaded::of(h);
    if (!name || !*name || !fn || !l.host->env.registry) return 0;
    mq::Registry& reg = *l.host->env.registry;
    // MQ's AddTopLevelObject fails on a name already taken: a plugin cannot
    // replace the client's TLOs (Me, Target, ...) or another plugin's.
    if (reg.has_tlo(name)) return 0;
    // One type per plugin TLO: its members are whatever the plugin answers.
    mq::Type& t = reg.type(std::string("plugin:") + name);
    Loaded* lp = &l;
    t.to_value = [lp, fn, user](const mq::Context&, const mq::Value& self) {
        HailstoneValue out{};
        if (!guarded(*lp, "a TLO", [&] { return fn(user, self.s.c_str(), nullptr, nullptr, &out) != 0; }))
            return mq::Value::nil();
        return from_c(out);
    };
    t.any_member = [lp, fn, user](const mq::Context&, const mq::Value& self, std::string_view member,
                                  std::string_view index) {
        HailstoneValue out{};
        std::string m(member), i(index);
        if (!guarded(*lp, "a TLO", [&] { return fn(user, self.s.c_str(), m.c_str(), i.c_str(), &out) != 0; }))
            return mq::Value::nil();
        return from_c(out);
    };
    const mq::Type* tp = &t;
    reg.add_tlo(name, [tp](const mq::Context&, std::string_view index) {
        return mq::Value::object(tp, 0, 0, std::string(index));
    });
    l.tlos.emplace_back(name);
    return 1;
}

void h_remove_tlo(void* h, const char* name) {
    Loaded& l = Loaded::of(h);
    if (!name || !l.host->env.registry) return;
    // Only a TLO this plugin added: the client's and other plugins' stay.
    if (std::none_of(l.tlos.begin(), l.tlos.end(), [&](const std::string& n) { return iequal(n, name); })) return;
    mq::Registry& reg = *l.host->env.registry;
    reg.remove_tlo(name);
    mq::Type& t = reg.type(std::string("plugin:") + name);
    t.to_value = nullptr;
    t.any_member = nullptr;
    std::erase_if(l.tlos, [&](const std::string& n) { return iequal(n, name); });
}

int h_send_packet(void* h, int stream, std::uint16_t opcode, const void* data, std::size_t size) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapPackets)) {
        l.denied("send_packet", kCapPackets);
        return 0;
    }
    if (!l.host->env.send || (stream != HAILSTONE_STREAM_WORLD && stream != HAILSTONE_STREAM_ZONE)) return 0;
    net::AppPacket p(opcode);
    if (size) {
        p.data.resize(size);
        std::memcpy(p.data.data(), data, size);
    }
    l.host->env.send(stream == HAILSTONE_STREAM_WORLD ? Stream::World : Stream::Zone, p);
    return 1;
}

int h_resize_packet(void* h, HailstonePacket* p, std::size_t size) {
    Loaded& l = Loaded::of(h);
    if (!p || !l.current_packet || size > 0x100000) return 0;
    l.current_packet->data.resize(size);
    p->data = reinterpret_cast<std::uint8_t*>(l.current_packet->data.data());
    p->size = size;
    return 1;
}

int h_config_dir(void* h, char* out, std::size_t out_size) {
    Loaded& l = Loaded::of(h);
    return dir_out(l.host->env.config_dir / l.name, out, out_size);
}

void* h_imgui_context(void* h) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapUi) || !l.host->env.imgui_context) return nullptr;
    return l.host->env.imgui_context();
}

// ---- ABI 2: the server extension

// A struct the plugin passes for the client to fill: it gets what both know (its `size`, the client's sizeof).
// (HailstoneWebState's size is a size_t, the others' a uint32_t: each keeps its own.)
template <typename T> int give(T* out, const T& full) {
    using Size = decltype(T::size);
    if (!out || out->size < sizeof(Size)) return 0;
    const Size size = out->size;
    std::memcpy(out, &full, std::min<std::size_t>(size, sizeof(T)));
    out->size = size;
    return 1;
}

template <std::size_t N> void copy_name(char (&dst)[N], const std::string& src) {
    std::size_t n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

HailstoneItem item_struct(const net::Item& it) {
    HailstoneItem v{};
    v.size = sizeof v;
    v.id = it.id;
    copy_name(v.name, it.name);
    v.item_type = it.item_type;
    v.item_class = it.item_class;
    v.classes = it.classes;
    v.races = it.races;
    v.icon = it.icon;
    v.stack_count = it.stack_count;
    v.charges = it.charges;
    auto spell = [](std::int32_t id) { return id > 0 ? static_cast<std::uint32_t>(id) : 0u; };
    v.click_spell = spell(it.effects[net::kEffectClick].effect);
    v.scroll_spell = spell(it.effects[net::kEffectScroll].effect);
    return v;
}

bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

HailstoneSpawn spawn_struct(const net::GameConnection& c, const game::Spawn& s) {
    HailstoneSpawn v{};
    v.size = sizeof v;
    v.id = s.id;
    copy_name(v.name, s.name);
    v.level = s.level;
    v.class_id = s.class_id;
    v.race = s.race;
    v.owner_id = s.pet_owner_id;
    v.guild_id = s.guild_id;
    v.is_npc = s.is_npc ? 1 : 0;
    v.is_corpse = s.is_corpse ? 1 : 0;
    v.x = s.position.x;
    v.y = s.position.y;
    v.z = s.position.z;
    v.heading = s.heading;
    v.hp_percent = s.hp_percent;
    v.rider_id = s.rider_id;
    v.is_mercenary = s.mercenary ? 1 : 0;
    v.is_gm = s.gm ? 1 : 0;
    if (c.group().active)
        for (const auto& m : c.group().slots)
            if (!m.name.empty() && same_name(m.name, s.name)) v.in_group = 1;
    if (s.id != c.world().player_id) {
        game::Con con = game::con_level(s, static_cast<int>(c.player_status().level), c.pvp_rules());
        v.con_color = con == game::Con::None ? 0 : game::con_chat_color(con);
    }
    for (std::uint32_t t : s.properties) {
        if (v.body_type_count == std::size(v.body_types)) break;
        v.body_types[v.body_type_count++] = t;
    }
    return v;
}

bool may_extend(Loaded& l, const char* what) {
    if (l.can(kCapExtend)) return true;
    if (!l.extend_denied) l.denied(what, kCapExtend); // once: setters are often called every frame
    l.extend_denied = true;
    return false;
}

net::GameConnection* connection_of(const Loaded& l) {
    return l.host->env.connection ? l.host->env.connection() : nullptr;
}

void h_set_active(void* h, int active) {
    Loaded& l = Loaded::of(h);
    if (may_extend(l, "set_active")) l.active = active != 0;
}

void h_set_flag(void* h, std::uint32_t flag, int on) {
    Loaded& l = Loaded::of(h);
    if (!may_extend(l, "set_flag")) return;
    if (on) l.flags |= flag;
    else l.flags &= ~flag;
}

void h_set_player_classes(void* h, std::uint32_t mask) {
    Loaded& l = Loaded::of(h);
    if (!may_extend(l, "set_player_classes")) return;
    if (net::GameConnection* c = connection_of(l)) c->set_player_classes(mask);
}

void h_set_character_classes(void* h, const char* name, std::uint32_t mask) {
    Loaded& l = Loaded::of(h);
    if (!may_extend(l, "set_character_classes") || !name) return;
    if (net::GameConnection* c = connection_of(l)) c->set_character_classes(name, mask);
}

void h_set_pet_list(void* h, const HailstonePet* pets, std::size_t count) {
    Loaded& l = Loaded::of(h);
    if (!may_extend(l, "set_pet_list")) return;
    net::GameConnection* c = connection_of(l);
    if (!c) return;
    std::vector<game::PetInfo> list;
    for (std::size_t i = 0; pets && i < count && i < 256; ++i) list.push_back({pets[i].spawn_id, pets[i].class_id});
    c->set_pet_list(std::move(list));
}

void h_set_vitals(void* h, const HailstoneVitals* v) {
    Loaded& l = Loaded::of(h);
    if (!may_extend(l, "set_vitals")) return;
    if (!v) {
        l.vitals.reset();
        return;
    }
    l.vitals = *v;
    // As the server's client writes them into its profile (the NMS patch's 0x10153300): the player's current
    // points now, until the next update changes them. Negative values count as 0 there.
    net::GameConnection* c = connection_of(l);
    if (!c) return;
    net::PlayerStatus& ps = c->player_status_mut();
    auto points = [](std::int32_t x) { return static_cast<std::uint32_t>(std::max(x, 0)); };
    if (v->has & HAILSTONE_VITAL_CUR_HP) ps.hp = points(v->cur_hp);
    if (v->has & HAILSTONE_VITAL_MAX_HP) ps.max_hp = points(v->max_hp);
    if (v->has & HAILSTONE_VITAL_CUR_MANA) ps.mana = points(v->cur_mana);
    if (v->has & HAILSTONE_VITAL_MAX_MANA) ps.max_mana = points(v->max_mana);
    if (v->has & HAILSTONE_VITAL_CUR_ENDURANCE) ps.endurance = points(v->cur_endurance);
    if (v->has & HAILSTONE_VITAL_MAX_ENDURANCE) ps.max_endurance = points(v->max_endurance);
}

// ---- ABI 2: the plugin's Lua windows

} // namespace
} // namespace hailstone::plugin

// The C handles of a call's arguments and result (sdk/hailstone_plugin.h), outside any namespace as declared there.
struct HailstoneArgs {
    const std::vector<hailstone::plugin::UiValue>* values = nullptr;
};
struct HailstoneOut {
    hailstone::plugin::UiValue root;
    bool has_root = false;
    std::vector<hailstone::plugin::UiValue*> open; // the arrays and tables being filled
    std::string key;                          // a table's next key (out_key)
};

namespace hailstone::plugin {
namespace {

bool may_ui(Loaded& l, const char* what) {
    if (l.can(kCapUi)) return true;
    l.denied(what, kCapUi);
    return false;
}

// A UiValue as an HailstoneValue: arrays and tables as HAILSTONE_TABLE (read their fields with arg_field).
int value_out(const UiValue& v, HailstoneValue* out) {
    if (!out) return 0;
    *out = HailstoneValue{};
    switch (v.kind) {
    case UiValue::Kind::Nil: return 0;
    case UiValue::Kind::Bool: out->kind = HAILSTONE_BOOL, out->i = v.b ? 1 : 0; break;
    case UiValue::Kind::Int: out->kind = HAILSTONE_INT, out->i = v.i; break;
    case UiValue::Kind::Number: out->kind = HAILSTONE_FLOAT, out->f = v.n; break;
    case UiValue::Kind::String: out->kind = HAILSTONE_STRING, out->s = v.s.c_str(); break;
    case UiValue::Kind::Array:
    case UiValue::Kind::Table: out->kind = HAILSTONE_TABLE; break;
    }
    return 1;
}

int h_add_ui_function(void* h, const char* name, HailstoneUiFn fn, void* user) {
    Loaded& l = Loaded::of(h);
    if (!name || !*name || !fn || !l.host->env.ui || !may_ui(l, "add_ui_function")) return 0;
    Loaded* lp = &l;
    l.host->env.ui->add_function(l.name, name, [lp, fn, user](const std::vector<UiValue>& args) {
        HailstoneArgs a{&args};
        HailstoneOut out;
        const int ok = guarded(*lp, "a UI function", [&] { return fn(user, &a, &out); });
        return ok && out.has_root ? std::move(out.root) : UiValue{};
    });
    return 1;
}

void h_remove_ui_function(void* h, const char* name) {
    Loaded& l = Loaded::of(h);
    if (name && l.host->env.ui) l.host->env.ui->remove_function(l.name, name);
}

std::size_t h_arg_count(void*, const HailstoneArgs* a) { return a && a->values ? a->values->size() : 0; }

int h_arg(void*, const HailstoneArgs* a, std::size_t index, HailstoneValue* out) {
    if (!a || !a->values || index >= a->values->size()) return 0;
    return value_out((*a->values)[index], out);
}

int h_arg_field(void*, const HailstoneArgs* a, std::size_t index, const char* key, HailstoneValue* out) {
    if (!a || !a->values || !key || index >= a->values->size()) return 0;
    const UiValue* f = (*a->values)[index].field(key);
    return f ? value_out(*f, out) : 0;
}

// Where the result's next value goes: the root, the open array's next item, or the open table's field under the
// last out_key; null when there is no place (a second root, a table value without a key).
UiValue* out_slot(HailstoneOut* o) {
    if (!o) return nullptr;
    if (o->open.empty()) {
        if (o->has_root) return nullptr;
        o->has_root = true;
        return &o->root;
    }
    UiValue* c = o->open.back();
    if (c->kind == UiValue::Kind::Array) return &c->items.emplace_back();
    if (o->key.empty()) return nullptr;
    c->fields.emplace_back(std::exchange(o->key, {}), UiValue{});
    return &c->fields.back().value;
}

void h_out_value(void*, HailstoneOut* o, const HailstoneValue* v) {
    UiValue* slot = v ? out_slot(o) : nullptr;
    if (!slot) return;
    switch (v->kind) {
    case HAILSTONE_BOOL: *slot = UiValue::boolean(v->i != 0); break;
    case HAILSTONE_INT: *slot = UiValue::integer(v->i); break;
    case HAILSTONE_FLOAT: *slot = UiValue::number(v->f); break;
    case HAILSTONE_STRING: *slot = UiValue::string(v->s ? v->s : ""); break;
    default: break; // nil
    }
}

void out_begin(HailstoneOut* o, UiValue::Kind kind) {
    if (!o || o->open.size() >= 32) return; // deep enough for any window
    UiValue* slot = out_slot(o);
    if (!slot) return;
    slot->kind = kind;
    o->open.push_back(slot); // its parent is not changed while it is open, so the pointer holds
}

void h_out_begin_array(void*, HailstoneOut* o) { out_begin(o, UiValue::Kind::Array); }
void h_out_begin_table(void*, HailstoneOut* o) { out_begin(o, UiValue::Kind::Table); }

void h_out_key(void*, HailstoneOut* o, const char* key) {
    if (o && key) o->key = key;
}

void h_out_end(void*, HailstoneOut* o) {
    if (o && !o->open.empty()) o->open.pop_back();
}

int h_add_option(void* h, const char* name, const char* label, int min, int max, HailstoneOptionGetFn get,
                 HailstoneOptionSetFn set, void* user) {
    Loaded& l = Loaded::of(h);
    if (!name || !*name || !get || !set || !l.host->env.ui || !may_ui(l, "add_option")) return 0;
    Loaded* lp = &l;
    UiBridge::Option o;
    o.plugin = l.name;
    o.name = name;
    o.label = label && *label ? label : name;
    o.min = min;
    o.max = std::max(min, max);
    o.get = [lp, get, user] { return guarded(*lp, "an option", [&] { return get(user); }); };
    o.set = [lp, set, user](int v) { guarded(*lp, "an option", [&] { set(user, v); }); };
    l.host->env.ui->add_option(std::move(o));
    return 1;
}

int h_add_menu(void* h, const char* label, const char* command) {
    Loaded& l = Loaded::of(h);
    if (!label || !*label || !command || !*command || !l.host->env.ui || !may_ui(l, "add_menu")) return 0;
    l.host->env.ui->add_menu({l.name, label, command});
    return 1;
}

// The plugin's folder: plugins/<name>/ when its binary is there, else plugins/<name>/ beside the binary.
fs::path plugin_folder(const fs::path& file, const std::string& name) {
    fs::path dir = file.parent_path();
    return iequal(dir.filename().string(), name) ? dir : dir / name;
}

// ---- ABI 2: the readers

const net::GameConnection* readable(Loaded& l, const char* what) {
    if (!l.can(kCapRead)) {
        if (!l.read_denied) l.denied(what, kCapRead);
        l.read_denied = true;
        return nullptr;
    }
    return connection_of(l);
}

int h_get_player(void* h, HailstonePlayer* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_player");
    if (!c) return 0;
    HailstonePlayer v{};
    v.size = sizeof v;
    const auto& w = c->world();
    v.spawn_id = w.player_id;
    copy_name(v.name, c->character_name());
    const auto& ps = c->player_status();
    v.level = ps.level;
    if (const auto& p = c->profile()) {
        v.race = p->race;
        v.class_id = p->class_;
    }
    v.visible_class = c->visible_class();
    v.class_mask = c->player_class_mask();
    v.cur_hp = c->own_hp().cur; // the items' HP included (GameConnection::own_hp)
    v.max_hp = c->own_hp().max;
    v.cur_mana = ps.mana;
    v.max_mana = ps.max_mana;
    v.cur_endurance = ps.endurance;
    v.max_endurance = ps.max_endurance;
    v.target_id = w.target_id;
    v.zone_id = c->zone_id();
    v.game_state = c->hook_game_state();
    v.in_group = c->group().active ? 1 : 0;
    v.in_raid = c->raid().active ? 1 : 0;
    v.spell_book_open = c->spell_book_open() ? 1 : 0;
    v.buff_slots = static_cast<std::uint32_t>(c->spell_state().buffs.size());
    v.xtarget_slots = static_cast<std::uint32_t>(c->xtargets().slots.size());
    const auto& spells = c->spell_state();
    v.casting_spell_id = spells.casting.casting() ? spells.casting.spell : 0;
    v.scribing_spell_id = spells.scribing ? spells.scribing->spell : 0;
    v.stand_state = ps.stand_state;
    v.pet_id = c->pets().focused;
    v.filter_pc_spells = c->chat_filters().settings[game::kFilterPcSpells];
    v.in_zone = c->in_zone() ? 1 : 0; // camping too: the plugins' windows stay up through the camp's 30 s
    return give(out, v);
}

std::uint32_t h_combat_ability(void* h, std::int32_t index) {
    const net::GameConnection* c = readable(Loaded::of(h), "combat_ability");
    return c ? c->combat_ability(index) : 0;
}

int h_server_info(void* h, HailstoneServer* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "server_info");
    if (!c || c->ticket().world_host.empty()) return 0;
    HailstoneServer v{};
    v.size = sizeof v;
    if (const auto saved = c->saved_server()) {
        copy_name(v.name, saved->name);
        copy_name(v.host, saved->host);
        v.port = static_cast<std::uint32_t>(saved->port);
    }
    copy_name(v.world, c->ticket().server_name);
    copy_name(v.world_host, c->ticket().world_host);
    return give(out, v);
}

// strcpy_s without MSVC's CRT: copies at most size - 1 bytes and always ends the copy with a 0.
static void copy_cstr(char* dst, size_t size, const char* src)
{
    if (size == 0) return;
    const size_t n = std::min(std::strlen(src), size - 1);
    std::memcpy(dst, src, n);
    dst[n] = 0;
}

// ***************************************************************************
// Function:    CleanupName
// Description: Cleans up NPC names
//              an_iksar_marauder23 = iksar marauder, an
// ***************************************************************************
char* CleanupName(char* szName, size_t BufferSize, bool Article, bool ForWhoList)
{
    char szTemp[2048] = { 0 };
    int j = 0;

    for (size_t i = 0; i < strlen(szName); i++)
    {
        switch (szName[i])
        {
        case '_':
            szTemp[j++] = ' ';
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            break;
        case '#':
            if (!ForWhoList)
                break;
        default:
            szTemp[j++] = szName[i];
        }
    }

    copy_cstr(szName, BufferSize, szTemp);

    if (!Article) return szName;

    if (hailstone::istarts_with(szName, "a "))
    {
        std::snprintf(szTemp, sizeof(szTemp), "%s, a", szName + 2);
        copy_cstr(szName, BufferSize, szTemp);
    }
    else if (hailstone::istarts_with(szName, "an "))
    {
        std::snprintf(szTemp, sizeof(szTemp), "%s, an", szName + 3);
        copy_cstr(szName, BufferSize, szTemp);
    }
    else if (hailstone::istarts_with(szName, "the "))
    {
        std::snprintf(szTemp, sizeof(szTemp), "%s, the", szName + 4);
        copy_cstr(szName, BufferSize, szTemp);
    }

    return szName;
}

int ConColor(int cLevel,int targetlevel)
{
  
    if (targetlevel - cLevel > 3)
    {
        return CONCOLOR_RED;
    }
    if (targetlevel - cLevel > 0)
    {
        return CONCOLOR_YELLOW;
    }
    if (targetlevel - cLevel > -6)
    {
        return CONCOLOR_BLUE;
    }
    if (targetlevel - cLevel > -16)
    {
        return CONCOLOR_LIGHTBLUE;
    }
    if (targetlevel - cLevel > -26)
    {
        return CONCOLOR_LIGHTBLUE;
    }
    return CONCOLOR_GREY;

}

//may need to break this up, but you need a sizable buffer for this or boom.
//for 5,000 spawns, assume 10 megs. now 5k is wayyy higher than it should ever be
//so can get away with less, but you never know. 
//there is a safety check that if we get down to our last 4,096 bytes, kick out and just return with what we have.
// Not registered as a host function yet (e3n will call it), hence [[maybe_unused]].
[[maybe_unused]] int h_get_spawns_bulk(void* h, unsigned char* data, int datamax)
{
    //don't allow a deny just grab the raw connection.
    const net::GameConnection* c = connection_of(Loaded::of(h));
    if (!c) return 0;
    auto spawns = c->world().spawns;
    int bufferSize = 0;
    unsigned char* pBuffer = data;
    for (const auto& [id, spawn] : spawns) {
       
        //if we are down to our last 4k of room
        //kick out, as we may explode
        if ((bufferSize + 4096) > datamax) break;

        int ID = spawn.id;
        memcpy(pBuffer, &ID, sizeof(ID));
        pBuffer += sizeof(ID);
        bufferSize += sizeof(ID);

        bool isAFK = spawn.afk;
        memcpy(pBuffer, &isAFK, sizeof(isAFK));
        pBuffer += sizeof(isAFK);
        bufferSize += sizeof(isAFK);

        bool isAggressive = (spawn.player_state & 0x4 || spawn.player_state & 0x8);
        memcpy(pBuffer, &isAggressive, sizeof(isAggressive));
        pBuffer += sizeof(isAggressive);
        bufferSize += sizeof(isAggressive);

        bool isAnon = (spawn.anon == 1);
        memcpy(pBuffer, &isAnon, sizeof(isAnon));
        pBuffer += sizeof(isAnon);
        bufferSize += sizeof(isAnon);
        int isBlind = 0; //not sure exists in the client, so mark as 0 for now.
        memcpy(pBuffer, &isBlind, sizeof(isBlind));
        pBuffer += sizeof(isBlind);
        bufferSize += sizeof(isBlind);

        int BodyTypeID = spawn.body_type;
        memcpy(pBuffer, &BodyTypeID, sizeof(BodyTypeID));
        pBuffer += sizeof(BodyTypeID);
        bufferSize += sizeof(BodyTypeID);
        
        const char* BodyDesc;

        if (BodyTypeID >= 0 && static_cast<std::size_t>(BodyTypeID) < std::size(game::szBodyType))
        {
            BodyDesc = game::szBodyType[BodyTypeID].data();

        }
        else
        {
            BodyDesc = "";
        }
        int BodyDescLength = static_cast<int>(strlen(BodyDesc));
        //copy the size
        memcpy(pBuffer, &BodyDescLength, sizeof(BodyDescLength));
        pBuffer += sizeof(BodyDescLength);
        bufferSize += sizeof(BodyDescLength);
        //copy the data
        memcpy(pBuffer, BodyDesc, BodyDescLength);
        pBuffer += BodyDescLength;
        bufferSize += BodyDescLength;

        bool isBuyer = spawn.buyer;
        memcpy(pBuffer, &isBuyer, sizeof(isBuyer));
        pBuffer += sizeof(isBuyer);
        bufferSize += sizeof(isBuyer);

        int ClassID = spawn.class_id;
        memcpy(pBuffer, &ClassID, sizeof(ClassID));
        pBuffer += sizeof(ClassID);
        bufferSize += sizeof(ClassID);

        char tempBuffer[2048];
        copy_cstr(tempBuffer, sizeof(tempBuffer), spawn.name.c_str());
        CleanupName(tempBuffer, sizeof(tempBuffer), false, false);
        std::string tCleanName(tempBuffer);
        int tCleanNameLength = static_cast<int>(tCleanName.size());
        //copy the size
        memcpy(pBuffer, &tCleanNameLength, sizeof(tCleanNameLength));
        pBuffer += sizeof(tCleanNameLength);
        bufferSize += sizeof(tCleanNameLength);
        //copy the data
        memcpy(pBuffer, tCleanName.c_str(), tCleanNameLength);
        pBuffer += tCleanNameLength;
        bufferSize += tCleanNameLength;

        int conColorID = ConColor(c->world().player()->level,spawn.level);
        memcpy(pBuffer, &conColorID, sizeof(conColorID));
        pBuffer += sizeof(conColorID);
        bufferSize += sizeof(conColorID);

        int currentEndurance = 0;
        memcpy(pBuffer, &currentEndurance, sizeof(currentEndurance));
        pBuffer += sizeof(currentEndurance);
        bufferSize += sizeof(currentEndurance);

        int64_t currentHps = spawn.hp_percent;
        memcpy(pBuffer, &currentHps, sizeof(currentHps));
        pBuffer += sizeof(currentHps);
        bufferSize += sizeof(currentHps);

        int currentMana = 0;
        memcpy(pBuffer, &currentMana, sizeof(currentMana));
        pBuffer += sizeof(currentMana);
        bufferSize += sizeof(currentMana);

        bool dead = (spawn.stand_state == STANDSTATE_DEAD);
        memcpy(pBuffer, &dead, sizeof(dead));
        pBuffer += sizeof(dead);
        bufferSize += sizeof(dead);
        
        std::string displayName= game::display_name(spawn.name);
        int displayNameLength = static_cast<int>(displayName.length());
        //copy the size
        memcpy(pBuffer, &displayNameLength, sizeof(displayNameLength));
        pBuffer += sizeof(displayNameLength);
        bufferSize += sizeof(displayNameLength);
        //copy the data
        memcpy(pBuffer, displayName.c_str(), displayNameLength);
        pBuffer += displayNameLength;
        bufferSize += displayNameLength;

        bool ducking = (spawn.stand_state == STANDSTATE_DUCK);
        memcpy(pBuffer, &ducking, sizeof(ducking));
        pBuffer += sizeof(ducking);
        bufferSize += sizeof(ducking);

        bool feigning = (spawn.stand_state == STANDSTATE_FEIGN);
        memcpy(pBuffer, &feigning, sizeof(feigning));
        pBuffer += sizeof(feigning);
        bufferSize += sizeof(feigning);

       
        int genderId = spawn.gender;
        memcpy(pBuffer, &genderId, sizeof(genderId));
        pBuffer += sizeof(genderId);
        bufferSize += sizeof(genderId);

        bool GM = spawn.gm;
        memcpy(pBuffer, &GM, sizeof(GM));
        pBuffer += sizeof(GM);
        bufferSize += sizeof(GM);

        int64_t guildID = spawn.guild_id;
        memcpy(pBuffer, &guildID, sizeof(guildID));
        pBuffer += sizeof(guildID);
        bufferSize += sizeof(guildID);

        float heading = spawn.heading * 0.703125f;
        memcpy(pBuffer, &heading, sizeof(heading));
        pBuffer += sizeof(heading);
        bufferSize += sizeof(heading);


        float height = spawn.heights.avatar_height;
        memcpy(pBuffer, &height, sizeof(height));
        pBuffer += sizeof(height);
        bufferSize += sizeof(height);

        bool Invs = 0;
        memcpy(pBuffer, &Invs, sizeof(Invs));
        pBuffer += sizeof(Invs);
        bufferSize += sizeof(Invs);

        bool isSummoned = spawn.summoned;
        memcpy(pBuffer, &isSummoned, sizeof(isSummoned));
        pBuffer += sizeof(isSummoned);
        bufferSize += sizeof(isSummoned);

        int level = spawn.level;
        memcpy(pBuffer, &level, sizeof(level));
        pBuffer += sizeof(level);
        bufferSize += sizeof(level);

        bool levitation = spawn.fly_mode==2;
        memcpy(pBuffer, &levitation, sizeof(levitation));
        pBuffer += sizeof(levitation);
        bufferSize += sizeof(levitation);

        bool linkDead = spawn.linkdead;
        memcpy(pBuffer, &linkDead, sizeof(linkDead));
        pBuffer += sizeof(linkDead);
        bufferSize += sizeof(linkDead);

        float look =0;
        memcpy(pBuffer, &look, sizeof(look));
        pBuffer += sizeof(look);
        bufferSize += sizeof(look);

        int master = spawn.pet_owner_id;
        memcpy(pBuffer, &master, sizeof(master));
        pBuffer += sizeof(master);
        bufferSize += sizeof(master);

        int maxEndurance = 0;
        memcpy(pBuffer, &maxEndurance, sizeof(maxEndurance));
        pBuffer += sizeof(maxEndurance);
        bufferSize += sizeof(maxEndurance);

        game::eSpawnType spawnType = game::Spawn::GetSpawnType(&spawn);

        float maxRange = 0.0;
        if (spawnType != hailstone::game::eSpawnType::ITEM)
        {
            //not quite sure how to get melee range, as there is a different type of spawn object for melee calculations...
            //and frankly shouldn't relaly use this anyway.
            maxRange = 14.5;
        }
        memcpy(pBuffer, &maxRange, sizeof(maxRange));
        pBuffer += sizeof(maxRange);
        bufferSize += sizeof(maxRange);


        float maxRanageTo = 0.0;
        if (spawnType != hailstone::game::eSpawnType::ITEM)
        {
            maxRanageTo = 14.5;
        }
        memcpy(pBuffer, &maxRanageTo, sizeof(maxRanageTo));
        pBuffer += sizeof(maxRanageTo);
        bufferSize += sizeof(maxRanageTo);

        bool mount = false;
        if (spawn.mount_id>0)
        {
            mount = true;
        }
        memcpy(pBuffer, &mount, sizeof(mount));
        pBuffer += sizeof(mount);
        bufferSize += sizeof(mount);


        bool moving = (fabs(spawn.speed_run) > 0.0f);
        memcpy(pBuffer, &moving, sizeof(moving));
        pBuffer += sizeof(moving);
        bufferSize += sizeof(moving);


        std::string name(spawn.name);
        int tNameLength = static_cast<int>(name.size());
        //copy the size
        memcpy(pBuffer, &tNameLength, sizeof(tNameLength));
        pBuffer += sizeof(tNameLength);
        bufferSize += sizeof(tNameLength);
        //copy the data
        memcpy(pBuffer, name.c_str(), tNameLength);
        pBuffer += tNameLength;
        bufferSize += tNameLength;

        bool isNamed = game::Spawn::IsNamed(&spawn);
        memcpy(pBuffer, &isNamed, sizeof(isNamed));
        pBuffer += sizeof(isNamed);
        bufferSize += sizeof(isNamed);

        int64_t pctHPs = spawn.hp_percent;
        memcpy(pBuffer, &pctHPs, sizeof(pctHPs));
        pBuffer += sizeof(pctHPs);
        bufferSize += sizeof(pctHPs);

        int pctMana = 0;
        memcpy(pBuffer, &pctMana, sizeof(pctMana));
        pBuffer += sizeof(pctMana);
        bufferSize += sizeof(pctMana);

        int petID = 0;
        memcpy(pBuffer, &petID, sizeof(petID));
        pBuffer += sizeof(petID);
        bufferSize += sizeof(petID);

        int playerState = spawn.player_state;
        memcpy(pBuffer, &playerState, sizeof(playerState));
        pBuffer += sizeof(playerState);
        bufferSize += sizeof(playerState);


        int raceID = spawn.race;
        memcpy(pBuffer, &raceID, sizeof(raceID));
        pBuffer += sizeof(raceID);
        bufferSize += sizeof(raceID);
       
        std::string raceName(hailstone::game::Spawn::GetRaceIDName(spawn.race));
        int tRaceNameLength = static_cast<int>(raceName.size());
        //copy the size
        memcpy(pBuffer, &tRaceNameLength, sizeof(tRaceNameLength));
        pBuffer += sizeof(tRaceNameLength);
        bufferSize += sizeof(tRaceNameLength);
        //copy the data
        memcpy(pBuffer, raceName.c_str(), tRaceNameLength);
        pBuffer += tRaceNameLength;
        bufferSize += tRaceNameLength;


        bool rolePlaying = (spawn.anon == 2);
        memcpy(pBuffer, &rolePlaying, sizeof(rolePlaying));
        pBuffer += sizeof(rolePlaying);
        bufferSize += sizeof(rolePlaying);

        bool sitting = (spawn.stand_state == STANDSTATE_SIT);
        memcpy(pBuffer, &sitting, sizeof(sitting));
        pBuffer += sizeof(sitting);
        bufferSize += sizeof(sitting);

        bool sneaking = false;
        memcpy(pBuffer, &sneaking, sizeof(sneaking));
        pBuffer += sizeof(sneaking);
        bufferSize += sizeof(sneaking);

        bool standing = (spawn.stand_state == STANDSTATE_STAND);
        memcpy(pBuffer, &standing, sizeof(standing));
        pBuffer += sizeof(standing);
        bufferSize += sizeof(standing);


        bool stunned = false;
        if (spawn.player_state & 0x20)
        {
            stunned = true;
        }
        memcpy(pBuffer, &stunned, sizeof(stunned));
        pBuffer += sizeof(stunned);
        bufferSize += sizeof(stunned);

       
        std::string suffix(spawn.suffix);
        int tsuffixLength = static_cast<int>(suffix.size());
        //copy the size
        memcpy(pBuffer, &tsuffixLength, sizeof(tsuffixLength));
        pBuffer += sizeof(tsuffixLength);
        bufferSize += sizeof(tsuffixLength);
        //copy the data
        memcpy(pBuffer, suffix.c_str(), tsuffixLength);
        pBuffer += tsuffixLength;
        bufferSize += tsuffixLength;


        bool targetable = spawn.targetable;
        memcpy(pBuffer, &targetable, sizeof(targetable));
        pBuffer += sizeof(targetable);
        bufferSize += sizeof(targetable);

        int targetoftargetID = 0;
        memcpy(pBuffer, &targetoftargetID, sizeof(targetoftargetID));
        pBuffer += sizeof(targetoftargetID);
        bufferSize += sizeof(targetoftargetID);


        bool trader = spawn.trader;
        memcpy(pBuffer, &trader, sizeof(trader));
        pBuffer += sizeof(trader);
        bufferSize += sizeof(trader);


        std::string typeDescription(game::Spawn::GetTypeDesc(spawnType));
        int ttypeDescriptionLength = static_cast<int>(typeDescription.size());
        //copy the size
        memcpy(pBuffer, &ttypeDescriptionLength, sizeof(ttypeDescriptionLength));
        pBuffer += sizeof(ttypeDescriptionLength);
        bufferSize += sizeof(ttypeDescriptionLength);
        //copy the data
        memcpy(pBuffer, typeDescription.c_str(), ttypeDescriptionLength);
        pBuffer += ttypeDescriptionLength;
        bufferSize += ttypeDescriptionLength;
        
        bool underwater = false;
        memcpy(pBuffer, &underwater, sizeof(underwater));
        pBuffer += sizeof(underwater);
        bufferSize += sizeof(underwater);

        float x = spawn.position.x;
        memcpy(pBuffer, &x, sizeof(x));
        pBuffer += sizeof(x);
        bufferSize += sizeof(x);

        float y = spawn.position.y;
        memcpy(pBuffer, &y, sizeof(y));
        pBuffer += sizeof(y);
        bufferSize += sizeof(y);

        float z = spawn.position.z;
        memcpy(pBuffer, &z, sizeof(z));
        pBuffer += sizeof(z);
        bufferSize += sizeof(z);


        //so distance calculations can be done
        ;
        float playerx = c->world().player()->position.x;
        memcpy(pBuffer, &playerx, sizeof(playerx));
        pBuffer += sizeof(playerx);
        bufferSize += sizeof(playerx);

        float playery = c->world().player()->position.y;
        memcpy(pBuffer, &playery, sizeof(playery));
        pBuffer += sizeof(playery);
        bufferSize += sizeof(playery);

        float playerz = c->world().player()->position.z;
        memcpy(pBuffer, &playerz, sizeof(playerz));
        pBuffer += sizeof(playerz);
        bufferSize += sizeof(playerz);

        //needed to tell NPC vs PC corpses
        int deity = spawn.deity;
        memcpy(pBuffer, &deity, sizeof(deity));
        pBuffer += sizeof(deity);
        bufferSize += sizeof(deity);


    }

    return bufferSize;
}
int h_get_spawn(void* h, std::uint32_t id, HailstoneSpawn* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_spawn");
    if (!c) return 0;
    auto it = c->world().spawns.find(id);
    return it == c->world().spawns.end() ? 0 : give(out, spawn_struct(*c, it->second));
}

int h_next_spawn(void* h, std::uint32_t after_id, HailstoneSpawn* out) {
    Loaded& l = Loaded::of(h);
    const net::GameConnection* c = readable(l, "next_spawn");
    if (!c) return 0;
    const auto& spawns = c->world().spawns;
    auto rebuild = [&] {
        l.spawn_ids.clear();
        l.spawn_ids.reserve(spawns.size());
        for (const auto& [id, s] : spawns) l.spawn_ids.push_back(id);
        std::sort(l.spawn_ids.begin(), l.spawn_ids.end());
        l.spawn_ids_of = &spawns;
        l.spawn_ids_serial = c->world().spawn_set_serial;
    };
    // A walk that starts at 0, or any spawn added or gone since (a resumed walk, from a saved id, sees them too).
    if (after_id == 0 || l.spawn_ids_of != &spawns || l.spawn_ids.size() != spawns.size() ||
        l.spawn_ids_serial != c->world().spawn_set_serial)
        rebuild();
    for (int pass = 0; pass < 2; ++pass) {
        auto it = std::upper_bound(l.spawn_ids.begin(), l.spawn_ids.end(), after_id);
        if (it == l.spawn_ids.end()) return 0;
        auto s = spawns.find(*it);
        if (s != spawns.end()) return give(out, spawn_struct(*c, s->second));
        rebuild(); // one gone since: made again from the spawns there are
    }
    return 0;
}

int h_get_spell(void* h, std::uint32_t id, HailstoneSpell* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_spell");
    if (!c) return 0;
    const game::SpellData* s = c->spell_table().get(id);
    if (!s) return 0;
    HailstoneSpell v{};
    v.size = sizeof v;
    v.id = s->id;
    copy_name(v.name, s->name);
    for (std::size_t i = 0; i < 16; ++i) v.class_level[i] = s->class_level[i];
    v.timer_id = s->reuse_timer_index;
    v.cast_time_ms = s->cast_time;
    v.recovery_time_ms = s->recovery_time;
    v.recast_time_ms = s->recast_time;
    v.mana = s->mana;
    v.target_type = s->target_type;
    v.skill = s->skill;
    v.is_skill = s->is_skill ? 1 : 0;
    v.gem_icon = static_cast<std::uint32_t>(s->gem_icon);
    return give(out, v);
}

int h_get_item(void* h, const HailstoneItemSlot* slot, HailstoneItem* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_item");
    if (!c || !slot) return 0;
    net::ItemSlot s{slot->type, slot->slot, slot->sub, slot->aug, 0, 0};
    const net::Item* it = c->inventory().at(s);
    return it ? give(out, item_struct(*it)) : 0;
}

int h_get_buff(void* h, std::uint32_t index, HailstoneBuff* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_buff");
    if (!c) return 0;
    const auto& buffs = c->spell_state().buffs;
    if (index >= buffs.size()) return 0;
    const game::BuffSlot& b = buffs[index];
    HailstoneBuff v{};
    v.size = sizeof v;
    v.spell_id = b.empty() ? 0 : b.spell;
    v.tics = b.tics;
    v.level = b.level;
    v.caster_id = b.caster_id;
    return give(out, v);
}

int h_get_xtarget(void* h, std::uint32_t index, HailstoneXTarget* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_xtarget");
    if (!c) return 0;
    const auto& slots = c->xtargets().slots;
    if (index >= slots.size()) return 0;
    const game::XTargetSlot& x = slots[index];
    HailstoneXTarget v{};
    v.size = sizeof v;
    v.type = x.type;
    v.status = x.status;
    v.spawn_id = x.spawn_id;
    copy_name(v.name, x.name);
    return give(out, v);
}

int h_eqstr(void* h, std::uint32_t id, const char* const* args, std::size_t count, char* out, std::size_t out_size) {
    const net::GameConnection* c = readable(Loaded::of(h), "eqstr");
    if (!c || !c->strings().has(id)) return -1;
    std::vector<std::string> a;
    for (std::size_t i = 0; args && i < count && i < 16; ++i) a.emplace_back(args[i] ? args[i] : "");
    return copy_out(c->strings().format(id, a), out, out_size);
}

// The connection for an action, with HAILSTONE_CAP_ACTIONS; null without either.
net::GameConnection* acting(Loaded& l, const char* what) {
    if (!l.can(kCapActions)) {
        l.denied(what, kCapActions);
        return nullptr;
    }
    return connection_of(l);
}

int h_right_click_item(void* h, const HailstoneItemSlot* slot, std::uint32_t class_id) {
    net::GameConnection* c = acting(Loaded::of(h), "right_click_item");
    if (!c || !slot) return 0;
    c->right_click_item_original(net::ItemSlot{slot->type, slot->slot, slot->sub, slot->aug, 0, 0}, {},
                                 static_cast<std::uint8_t>(class_id <= 0xff ? class_id : 0));
    return 1;
}

// ---- ABI 2: more of the client

void h_write_chat_color(void* h, const char* text, std::uint32_t color) {
    Loaded& l = Loaded::of(h);
    if (!text) return;
    if (net::GameConnection* c = connection_of(l)) c->ui_message(text, 0, color);
    else if (l.host->env.print) l.host->env.print(text, 0);
}

int h_server_config_dir(void* h, char* out, std::size_t out_size) {
    Loaded& l = Loaded::of(h);
    const Policy* policy = l.host->env.policy;
    if (!policy || !policy->current() || policy->current()->dir.empty()) return 0;
    return dir_out(policy->current()->dir / "plugin-config" / l.name, out, out_size);
}

int h_character_config_dir(void* h, char* out, std::size_t out_size) {
    Loaded& l = Loaded::of(h);
    const net::GameConnection* c = connection_of(l);
    if (!c) return 0;
    const CharacterFiles files = c->own_files();
    if (files.empty()) return 0;
    return dir_out(files.dir / "plugin-config" / l.name, out, out_size);
}

int h_zone_name(void* h, std::uint32_t zone_id, char* out, std::size_t out_size) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapRead)) {
        if (!l.read_denied) l.denied("zone_name", kCapRead);
        l.read_denied = true;
        return -1;
    }
    std::string name = game::zone_short_name(zone_id);
    return name.empty() ? -1 : copy_out(name, out, out_size);
}

void h_close_spell_book(void* h) {
    if (net::GameConnection* c = acting(Loaded::of(h), "close_spell_book")) c->close_spell_book();
}

void h_clear_spell_recovery(void* h) {
    if (net::GameConnection* c = acting(Loaded::of(h), "clear_spell_recovery")) c->clear_spell_recovery();
}

void h_set_window_pet(void* h, std::uint32_t spawn_id) {
    if (net::GameConnection* c = acting(Loaded::of(h), "set_window_pet")) c->set_window_pet(spawn_id);
}

int h_inspect_item(void* h, std::uint32_t item_id, const std::uint32_t* augments) {
    net::GameConnection* c = acting(Loaded::of(h), "inspect_item");
    if (!c || item_id == 0) return 0;
    game::ItemLink link;
    link.item_id = item_id;
    for (std::size_t k = 0; augments && k < 6; ++k) link.augments[k] = augments[k];
    c->request_item(link);
    return 1;
}

int h_open_bank(void* h, std::uint32_t banker) {
    net::GameConnection* c = acting(Loaded::of(h), "open_bank");
    if (!c) return 0;
    if (banker == 0) banker = c->world().player_id;
    return c->activate_bank(banker) ? 1 : 0;
}

int h_hot_button(void* h, std::int32_t bar, std::int32_t page, std::int32_t button, char* out, std::size_t size) {
    Loaded& l = Loaded::of(h);
    if (!readable(l, "hot_button") || !l.host->env.hot_button) return -1;
    const auto spec = l.host->env.hot_button(bar, page, button);
    if (!spec) return -1;
    return copy_out(*spec, out, size);
}

int h_set_hot_button(void* h, std::int32_t bar, std::int32_t page, std::int32_t button, const char* spec) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapActions)) {
        l.denied("set_hot_button", kCapActions);
        return 0;
    }
    return l.host->env.set_hot_button && l.host->env.set_hot_button(bar, page, button, spec ? spec : "") ? 1 : 0;
}

std::int32_t h_skill(void* h, std::uint32_t skill) {
    const net::GameConnection* c = readable(Loaded::of(h), "skill");
    return c && skill < 100 ? c->skill_value(skill) : 0;
}

std::uint32_t h_aa_value(void* h, std::int32_t group) {
    const net::GameConnection* c = readable(Loaded::of(h), "aa_value");
    if (!c) return 0;
    const auto& aa = c->alt_adv();
    for (const auto& o : aa.owned)
        if (const game::AltAbility* a = aa.get(o.id); a && a->group == group) return o.value;
    return 0;
}

std::int32_t h_hot_bar_page(void* h, std::int32_t bar) {
    Loaded& l = Loaded::of(h);
    if (!readable(l, "hot_bar_page") || !l.host->env.hot_bar_page) return -1;
    return l.host->env.hot_bar_page(bar);
}

void h_set_hot_bar_page(void* h, std::int32_t bar, std::int32_t page) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapActions)) {
        l.denied("set_hot_bar_page", kCapActions);
        return;
    }
    if (l.host->env.set_hot_bar_page) l.host->env.set_hot_bar_page(bar, page);
}

// ---- the plugin's TCP connections (HAILSTONE_CAP_NET)

// The connection, while the plugin still has 'net': one opened before a narrower policy took it away is closed at
// its next use (policy_changed closes them too), so it shows as gone (-1).
Loaded::Tcp* find_tcp(Loaded& l, std::int32_t handle) {
    if (!l.can(kCapNet)) {
        l.tcp.clear();
        return nullptr;
    }
    for (auto& t : l.tcp)
        if (t->id == handle) return t.get();
    return nullptr;
}

std::int32_t h_tcp_open(void* h, const char* address, std::uint16_t port) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapNet)) {
        l.denied("tcp_open", kCapNet);
        return 0;
    }
    if (!address || !*address || l.tcp.size() >= HAILSTONE_TCP_MAX) return 0;
    std::string error;
    auto conn = PluginTcp::open(address, port, error); // a name is looked up off the frame (plugin/plugin_tcp.h)
    if (!conn) {
        log::warn("plugin {}: tcp to {}:{}: {}", l.name, address, port, error);
        return 0;
    }
    log::info("plugin {}: tcp to {}:{}", l.name, address, port);
    auto t = std::make_unique<Loaded::Tcp>();
    t->id = l.next_tcp++;
    t->conn = std::move(conn);
    l.tcp.push_back(std::move(t));
    return l.tcp.back()->id;
}

std::int32_t h_tcp_state(void* h, std::int32_t handle) {
    Loaded::Tcp* t = find_tcp(Loaded::of(h), handle);
    return t ? t->conn->state() : -1;
}

std::int64_t h_tcp_send(void* h, std::int32_t handle, const void* data, std::size_t size) {
    Loaded::Tcp* t = find_tcp(Loaded::of(h), handle);
    if (!t) return -1;
    return t->conn->send(std::span<const std::byte>(static_cast<const std::byte*>(data), data ? size : 0));
}

std::int64_t h_tcp_recv(void* h, std::int32_t handle, void* out, std::size_t size) {
    Loaded::Tcp* t = find_tcp(Loaded::of(h), handle);
    if (!t) return -1;
    return t->conn->recv(std::span<std::byte>(static_cast<std::byte*>(out), out ? size : 0));
}

void h_tcp_close(void* h, std::int32_t handle) {
    Loaded& l = Loaded::of(h);
    std::erase_if(l.tcp, [handle](const auto& t) { return t->id == handle; });
}

int h_tcp_error(void* h, std::int32_t handle, char* out, std::size_t size) {
    Loaded::Tcp* t = find_tcp(Loaded::of(h), handle);
    if (!t) return -1;
    return copy_out(t->conn->error(), out, size);
}

float h_max_camera_distance(void* h) {
    Loaded& l = Loaded::of(h);
    return l.host->env.max_camera_distance ? l.host->env.max_camera_distance() : 0.0f;
}

void h_set_max_camera_distance(void* h, float distance) {
    Loaded& l = Loaded::of(h);
    if (!l.can(kCapActions)) {
        l.denied("set_max_camera_distance", kCapActions);
        return;
    }
    if (l.host->env.set_max_camera_distance && distance > 0.0f) l.host->env.set_max_camera_distance(distance);
}

// ---- ABI 3: web pages

// The plugin's browser, with 'web' (said once otherwise); null when it may not or there is none.
WebAccess* web_of(Loaded& l, const char* what) {
    if (!l.can(kCapWeb)) {
        if (!l.web_denied) l.denied(what, kCapWeb);
        l.web_denied = true;
        return nullptr;
    }
    return l.host->env.web;
}
std::string web_owner(const Loaded& l) { return "plugin:" + l.name; }

std::uint32_t h_web_open(void* h, const char* url, int width, int height) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_open");
    if (!w || !url) return 0;
    std::string why;
    const std::uint32_t id = w->open(web_owner(l), l.caps, url, width, height, &why);
    if (!id) l.host->say(std::format("[Plugin] {}: {}", l.name, why));
    return id;
}
int h_web_navigate(void* h, std::uint32_t tab, const char* url) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_navigate");
    if (!w || !url) return 0;
    std::string why;
    if (w->navigate(web_owner(l), l.caps, tab, url, &why)) return 1;
    l.host->say(std::format("[Plugin] {}: {}", l.name, why));
    return 0;
}
int h_web_nav(void* h, std::uint32_t tab, const char* action) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_nav");
    return w && action && w->nav(web_owner(l), l.caps, tab, action) ? 1 : 0;
}
void h_web_close(void* h, std::uint32_t tab) {
    Loaded& l = Loaded::of(h);
    if (l.host->env.web) l.host->env.web->close(web_owner(l), tab); // its own tab: no permission needed to close it
}
int h_web_eval(void* h, std::uint32_t tab, const char* script) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_eval");
    return w && script && w->eval(web_owner(l), l.caps, tab, script) ? 1 : 0;
}
int h_web_post(void* h, std::uint32_t tab, const char* message) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_post");
    return w && message && w->post(web_owner(l), l.caps, tab, message) ? 1 : 0;
}
int h_web_text(void* h, std::uint32_t tab, const char* text) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_text");
    return w && text && w->text(web_owner(l), l.caps, tab, text) ? 1 : 0;
}
int h_web_message(void* h, std::uint32_t tab, char* out, std::size_t out_size) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_message");
    if (!w) return -1;
    // No buffer: the length only, left queued, so the plugin can make room for it (one given takes it, cut or not:
    // a loop with a fixed buffer never sees the same message twice).
    if (!out || out_size == 0) {
        auto n = w->next_message_size(web_owner(l), tab);
        return n ? static_cast<int>(*n) : -1;
    }
    auto m = w->take_message(web_owner(l), tab);
    return m ? copy_out(*m, out, out_size) : -1;
}
int h_web_state(void* h, std::uint32_t tab, HailstoneWebState* out) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_state");
    if (!w || !out) return 0;
    auto s = w->state(web_owner(l), tab);
    if (!s) return 0;
    // Grown at its end like the other structs: a plugin built with a shorter one gets what both know.
    HailstoneWebState full{};
    full.size = sizeof full;
    copy_out(s->url, full.url, sizeof full.url);
    copy_out(s->title, full.title, sizeof full.title);
    full.loading = s->loading;
    full.can_back = s->can_back;
    full.can_forward = s->can_forward;
    full.closed = s->closed;
    return give(out, full);
}
int h_web_view(void* h, std::uint32_t tab, float width, float height) {
    Loaded& l = Loaded::of(h);
    WebAccess* w = web_of(l, "web_view");
    if (!w || !l.can(kCapUi) || !l.host->env.imgui_context || !l.host->env.imgui_context()) return 0;
    return w->view(web_owner(l), l.caps, tab, width, height) ? 1 : 0;
}

// Host functions are for the client's thread, inside one of the plugin's callbacks (sdk/hailstone_plugin.h): the client's
// state is not locked. Called from another thread one does nothing and returns 0 / NULL; the log says so once per
// plugin.
void off_thread(void* h) {
    if (!h) return;
    Loaded& l = Loaded::of(h);
    if (!l.thread_warned.exchange(true))
        log::warn("plugin {}: host function called from another thread, ignored (host functions are for the "
                  "client's thread, inside a callback)", l.name);
}
template <auto F> struct MainOnly;
template <typename R, typename... A, R (*F)(void*, A...)> struct MainOnly<F> {
    static R call(void* h, A... a) {
        if (!on_main_thread()) {
            off_thread(h);
            if constexpr (std::is_void_v<R>) return;
            else return R{};
        }
        return F(h, a...);
    }
};
template <auto F> constexpr auto main_only = &MainOnly<F>::call;

template <typename F> F symbol(SDL_SharedObject* lib, const char* name) {
    return reinterpret_cast<F>(SDL_LoadFunction(lib, name));
}
// One of the SDK's own exports: hailstone_plugin_<what>, or nmsc_plugin_<what> as plugins built up to 0.6 name it.
template <typename F> F sdk_symbol(SDL_SharedObject* lib, const std::string& what) {
    if (F f = symbol<F>(lib, ("hailstone_plugin_" + what).c_str())) return f;
    return symbol<F>(lib, ("nmsc_plugin_" + what).c_str());
}

} // namespace

std::optional<std::string> NativeExtension::merchant_level(const net::GameConnection&, const net::Item& item) const {
    if (!l_ || !h().list_level) return std::nullopt;
    const HailstoneItem v = item_struct(item);
    const char* t = run(h().list_level, &v);
    return t ? std::optional<std::string>(t) : std::nullopt;
}

NativePlugins::NativePlugins(NativeEnv env) : impl_(std::make_unique<Impl>()) {
    impl_->env = std::move(env);
    impl_->self = this;
    Impl* m = impl_.get();
    m->owner = m->env.hooks->new_owner("plugins");
    m->env.hooks->add_command(m->owner, "/plugin", [m](std::string_view args) { m->command(args); }, kCapCommands);
    // Unloads a plugin asked for from its own code.
    m->env.hooks->on_pulse(m->owner, [m] {
        std::vector<std::string> pending;
        for (const auto& l : m->loaded)
            if (l->unload_pending && !l->busy) pending.push_back(l->name);
        for (const auto& n : pending) m->self->unload(n);
    });
}

NativePlugins::~NativePlugins() {
    unload_all();
    // Public resets keep required modules alive. Final host destruction owns their shutdown.
    while (!impl_->loaded.empty()) {
        auto last = std::move(impl_->loaded.back());
        impl_->loaded.pop_back();
        discard(*impl_, std::move(last), true);
    }
    if (impl_->env.hooks) impl_->env.hooks->remove(impl_->owner);
}

std::vector<NativePlugins::Info> NativePlugins::list() const {
    std::vector<Info> out;
    for (const auto& loaded : impl_->loaded) {
        if (!loaded->builtin) continue;
        Info info;
        info.name = loaded->name;
        info.loaded = info.kept = info.builtin = true;
        info.caps = loaded->caps;
        out.push_back(std::move(info));
    }
    if (distribution::managed()) return out;
    for (const auto& file : plugin_files(impl_->dirs())) {
        Info i;
        i.name = plugin_name(file);
        // A name found again is shadowed by the first (the server's folder comes first): load would not pick it.
        if (std::any_of(out.begin(), out.end(), [&](const Info& o) { return iequal(o.name, i.name); })) continue;
        i.file = file;
        if (auto bytes = read_file(file)) {
            Sha256Digest h = sha256(*bytes);
            i.sha256 = to_hex(h);
            i.caps = impl_->env.policy ? impl_->env.policy->caps(FileKind::Plugin, h) : 0;
            i.server = impl_->env.policy && impl_->env.policy->server_file(FileKind::Plugin, h);
            i.kept = impl_->kept(h);
            i.asks = impl_->asks(file);
            if (i.asks) i.consent = impl_->consent_for(h);
        }
        if (const Loaded* l = impl_->find(i.name)) {
            i.loaded = true;
            i.caps = l->caps;
        }
        for (const auto& f : impl_->failures)
            if (iequal(f.name, i.name)) i.error = f.error;
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.name < b.name; });
    return out;
}

bool NativePlugins::loaded(std::string_view name) const { return impl_->find(name) != nullptr; }

bool NativePlugins::has_plugin(std::string_view name) const {
    if (distribution::managed()) return impl_->find(name) != nullptr;
    if (impl_->find(name)) return true;
    const auto files = plugin_files(impl_->dirs());
    return std::any_of(files.begin(), files.end(), [&](const fs::path& f) { return iequal(plugin_name(f), name); });
}

std::uint32_t NativePlugins::loaded_caps(std::string_view name) const {
    const Loaded* l = impl_->find(name);
    return l && l->name == name ? l->caps : 0;
}

namespace {
// Takes a plugin out of the client: its ShutdownPlugin (when `shutdown`), everything it registered, then its library.
void discard(NativePlugins::Impl& m, std::unique_ptr<Loaded> l, bool shutdown) {
    if (shutdown && l->shutdown) guarded(*l, "ShutdownPlugin", [&] { l->shutdown(); });
    m.env.hooks->remove(l->owner);
    if (m.env.web) m.env.web->close_owner(web_owner(*l));
    if (m.env.ui) m.env.ui->remove(l->name);
    if (l->extension) {
        l->extension->detach();
        if (m.env.extensions) m.env.extensions->remove(l->chain_id);
    }
    for (const auto& t : std::vector<std::string>(l->tlos)) h_remove_tlo(l.get(), t.c_str());
    if (l->lib) SDL_UnloadObject(l->lib);
}
} // namespace

bool NativePlugins::load(std::string_view name, std::string* why) {
    return load_file(name, why, caller_name().empty() ? Asker::Player : Asker::Script);
}

bool NativePlugins::load_builtin(const HailstoneBuiltinAddon& addon, const fs::path& trusted_ui_dir, std::string* why) {
    auto refuse = [&](const char* message) { if (why) *why = message; return false; };
    if (addon.size < sizeof(HailstoneBuiltinAddon) || addon.abi < 1 || addon.abi > HAILSTONE_PLUGIN_ABI ||
        !addon.name || !addon.set_host) return refuse("The built-in add-on descriptor is invalid.");
    const std::string name(addon.name);
    if (name.empty() || name.size() > 128 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        })) return refuse("The built-in add-on name is invalid.");
    constexpr auto managed_caps = kCapRead | kCapEvents | kCapUi | kCapPackets | kCapExtend | kCapLogin;
    if (!addon.capabilities || (addon.capabilities & ~kCapAll) ||
        (distribution::managed() && (addon.capabilities & ~managed_caps)))
        return refuse("The built-in add-on requests unsupported capabilities.");
    if (impl_->find(name)) return refuse("The built-in add-on is already loaded.");
    auto plugin = std::make_unique<Loaded>();
    plugin->host = impl_.get();
    plugin->name = name;
    plugin->caps = addon.capabilities;
    plugin->builtin = true;
    plugin->builtin_ui_dir = trusted_ui_dir;
    plugin->initialize = addon.initialize;
    plugin->shutdown = addon.shutdown;
    plugin->pulse = addon.pulse;
    plugin->game_state = addon.game_state;
    plugin->begin_zone = addon.begin_zone;
    plugin->end_zone = addon.end_zone;
    plugin->zoned = addon.zoned;
    plugin->add_spawn = addon.add_spawn;
    plugin->remove_spawn = addon.remove_spawn;
    plugin->incoming_chat = addon.incoming_chat;
    plugin->filter_chat = addon.filter_chat;
    plugin->update_imgui = addon.update_imgui;
    plugin->packet = addon.packet;
    plugin->hooks_fn = addon.hooks;
    return impl_->attach(std::move(plugin), addon.set_host, why);
}

bool NativePlugins::load_file(std::string_view name_in, std::string* why, Asker asker) {
    if (distribution::managed()) {
        if (why) *why = "Dynamic add-ons are disabled in this distribution.";
        return false;
    }
    Impl& m = *impl_;
    std::string name(name_in);
    auto refuse = [&](std::string text, bool say = true) {
        if (why) *why = text;
        std::erase_if(m.failures, [&](const Info& f) { return iequal(f.name, name); });
        Info f;
        f.name = name;
        f.error = text;
        m.failures.push_back(f);
        if (say) m.say("[Plugin] " + text);
        log::warn("plugin {}: {}", name, text);
        return false;
    };
    if (m.find(name)) return refuse(name + " is already loaded");
    auto file = m.file_of(name);
    if (!file) return refuse("no plugin named " + name);
    name = plugin_name(*file);
    auto bytes = read_file(*file);
    if (!bytes) return refuse("cannot read " + file->string());
    Sha256Digest hash = sha256(*bytes);
    if (!m.env.policy) return refuse("plugins are off");
    std::uint32_t caps = m.env.policy->caps(FileKind::Plugin, hash);
    if (caps == 0) return refuse(std::format("{} may not load: {}", name, m.env.policy->refusal(FileKind::Plugin)));
    // The server's own library is native code with the player's rights: it loads only once the player allowed these
    // bytes for this server. Until then it waits for the answer (the game goes on); a refused one is asked about
    // again only when the player loads it. A script or plugin never puts the question up: it could, at will.
    if (m.asks(*file)) {
        const Consent c = m.consent_for(hash);
        if (c != Consent::Allowed) {
            if (asker == Asker::Script)
                return refuse(name + " is not loaded: this server's compiled plugins load only once you allow them, "
                              "and only you can ask for that");
            if (c == Consent::Unasked || asker == Asker::Player) {
                const bool fresh = m.ask({name, *file, bytes->size(), hash});
                return refuse(name + " waits for your answer: this server's compiled plugins load only once you allow them",
                              fresh);
            }
            return refuse(name + " is not loaded: you did not allow it on this server", false);
        }
    }

    // Load a copy of the bytes that were hashed. Never rewritten in place: another Hailstone (a box set's other
    // windows) may have this very file loaded, and changing its bytes under it crashes it. One with the right bytes
    // is used as it is; else a new file replaces it in one rename (the loaded one keeps its old inode).
    std::error_code ec;
    fs::create_directories(m.env.cache_dir, ec);
    fs::path copy = m.env.cache_dir / (to_hex(hash).substr(0, 16) + "_" + file->filename().string());
    auto cached = read_file(copy);
    if (!cached || sha256(*cached) != hash) {
        if (!write_file_atomic(copy, std::string_view(bytes->data(), bytes->size())))
            return refuse("cannot write " + copy.string());
    }
    auto l = std::make_unique<Loaded>();
    Loaded& p = *l;
    p.host = &m;
    p.name = name;
    p.file = *file;
    p.copy = copy;
    p.hash = hash;
    p.caps = caps;
    p.lib = SDL_LoadObject(copy.string().c_str());
    if (!p.lib) return refuse(std::format("{} did not load: {}", name, SDL_GetError()));
    auto abi = sdk_symbol<std::uint32_t (*)()>(p.lib, "abi");
    auto set_host = sdk_symbol<void (*)(const PluginHostApi*)>(p.lib, "set_host");
    if (!abi || !set_host) {
        SDL_UnloadObject(p.lib);
        return refuse(name +
                      " is not a Hailstone plugin (no hailstone_plugin_abi / hailstone_plugin_set_host; use HAILSTONE_PLUGIN)");
    }
    // Older ABIs load (the tables only grow at their end); a newer one needs a newer client.
    if (std::uint32_t got = abi(); got < 1 || got > HAILSTONE_PLUGIN_ABI) {
        SDL_UnloadObject(p.lib);
        return refuse(std::format("{} was built for plugin ABI {}, this client has {}", name, got, HAILSTONE_PLUGIN_ABI));
    }
    p.initialize = symbol<void (*)()>(p.lib, "InitializePlugin");
    p.shutdown = symbol<void (*)()>(p.lib, "ShutdownPlugin");
    p.pulse = symbol<void (*)()>(p.lib, "OnPulse");
    p.game_state = symbol<void (*)(int)>(p.lib, "SetGameState");
    p.begin_zone = symbol<void (*)()>(p.lib, "OnBeginZone");
    p.end_zone = symbol<void (*)()>(p.lib, "OnEndZone");
    p.zoned = symbol<void (*)()>(p.lib, "OnZoned");
    p.add_spawn = symbol<void (*)(std::uint32_t)>(p.lib, "OnAddSpawn");
    p.remove_spawn = symbol<void (*)(std::uint32_t)>(p.lib, "OnRemoveSpawn");
    p.incoming_chat = symbol<int (*)(const char*, std::uint32_t)>(p.lib, "OnIncomingChat");
    p.filter_chat = symbol<int (*)(const char*, std::uint32_t)>(p.lib, "OnFilterChat");
    p.update_imgui = symbol<void (*)()>(p.lib, "OnUpdateImGui");
    p.packet = symbol<int (*)(int, int, HailstonePacket*)>(p.lib, "OnPacket");
    p.hooks_fn = sdk_symbol<const HailstoneHooks* (*)()>(p.lib, "hooks");

    return m.attach(std::move(l), set_host, why);
}

bool NativePlugins::Impl::attach(std::unique_ptr<Loaded> l, void (*set_host)(const PluginHostApi*), std::string* why) {
    Impl& m = *this;
    Loaded& p = *l;
    const std::string name = p.name;
    const auto file = p.file;
    const auto hash = p.hash;
    const auto caps = p.caps;
    auto refuse = [&](std::string text) {
        if (why) *why = text;
        std::erase_if(m.failures, [&](const Info& f) { return iequal(f.name, name); });
        Info failure;
        failure.name = name;
        failure.error = text;
        m.failures.push_back(std::move(failure));
        m.say("[Plugin] " + text);
        log::warn("plugin {}: {}", name, text);
        return false;
    };
    p.table.abi = HAILSTONE_PLUGIN_ABI;
    p.table.size = sizeof(PluginHostApi);
    p.table.host = &p;
    p.table.caps = main_only<h_caps>;
    p.table.name = main_only<h_name>;
    p.table.log = main_only<h_log>;
    p.table.write_chat = main_only<h_write_chat>;
    p.table.do_command = main_only<h_do_command>;
    p.table.add_command = main_only<h_add_command>;
    p.table.remove_command = main_only<h_remove_command>;
    p.table.parse = main_only<h_parse>;
    p.table.add_tlo = main_only<h_add_tlo>;
    p.table.remove_tlo = main_only<h_remove_tlo>;
    p.table.send_packet = main_only<h_send_packet>;
    p.table.resize_packet = main_only<h_resize_packet>;
    p.table.config_dir = main_only<h_config_dir>;
    p.table.imgui_context = main_only<h_imgui_context>;
    p.table.set_active = main_only<h_set_active>;
    p.table.set_flag = main_only<h_set_flag>;
    p.table.set_player_classes = main_only<h_set_player_classes>;
    p.table.set_character_classes = main_only<h_set_character_classes>;
    p.table.set_pet_list = main_only<h_set_pet_list>;
    p.table.set_vitals = main_only<h_set_vitals>;
    p.table.get_player = main_only<h_get_player>;
    p.table.get_spawn = main_only<h_get_spawn>;
    p.table.next_spawn = main_only<h_next_spawn>;
    p.table.get_spell = main_only<h_get_spell>;
    p.table.get_item = main_only<h_get_item>;
    p.table.get_buff = main_only<h_get_buff>;
    p.table.get_xtarget = main_only<h_get_xtarget>;
    p.table.eqstr = main_only<h_eqstr>;
    p.table.right_click_item = main_only<h_right_click_item>;
    p.table.add_ui_function = main_only<h_add_ui_function>;
    p.table.remove_ui_function = main_only<h_remove_ui_function>;
    p.table.arg_count = main_only<h_arg_count>;
    p.table.arg = main_only<h_arg>;
    p.table.arg_field = main_only<h_arg_field>;
    p.table.out_value = main_only<h_out_value>;
    p.table.out_begin_array = main_only<h_out_begin_array>;
    p.table.out_begin_table = main_only<h_out_begin_table>;
    p.table.out_key = main_only<h_out_key>;
    p.table.out_end = main_only<h_out_end>;
    p.table.add_option = main_only<h_add_option>;
    p.table.add_menu = main_only<h_add_menu>;
    p.table.write_chat_color = main_only<h_write_chat_color>;
    p.table.server_config_dir = main_only<h_server_config_dir>;
    p.table.character_config_dir = main_only<h_character_config_dir>;
    p.table.zone_name = main_only<h_zone_name>;
    p.table.close_spell_book = main_only<h_close_spell_book>;
    p.table.clear_spell_recovery = main_only<h_clear_spell_recovery>;
    p.table.set_window_pet = main_only<h_set_window_pet>;
    p.table.inspect_item = main_only<h_inspect_item>;
    p.table.max_camera_distance = main_only<h_max_camera_distance>;
    p.table.set_max_camera_distance = main_only<h_set_max_camera_distance>;
    p.table.server_info = main_only<h_server_info>;
    p.table.combat_ability = main_only<h_combat_ability>;
    p.table.hot_button = main_only<h_hot_button>;
    p.table.set_hot_button = main_only<h_set_hot_button>;
    p.table.hot_bar_page = main_only<h_hot_bar_page>;
    p.table.set_hot_bar_page = main_only<h_set_hot_bar_page>;
    p.table.skill = main_only<h_skill>;
    p.table.aa_value = main_only<h_aa_value>;
    p.table.tcp_open = main_only<h_tcp_open>;
    p.table.tcp_state = main_only<h_tcp_state>;
    p.table.tcp_send = main_only<h_tcp_send>;
    p.table.tcp_recv = main_only<h_tcp_recv>;
    p.table.tcp_close = main_only<h_tcp_close>;
    p.table.tcp_error = main_only<h_tcp_error>;
    p.table.web_open = main_only<h_web_open>;
    p.table.web_navigate = main_only<h_web_navigate>;
    p.table.web_nav = main_only<h_web_nav>;
    p.table.web_close = main_only<h_web_close>;
    p.table.web_eval = main_only<h_web_eval>;
    p.table.web_post = main_only<h_web_post>;
    p.table.web_text = main_only<h_web_text>;
    p.table.web_message = main_only<h_web_message>;
    p.table.web_state = main_only<h_web_state>;
    p.table.web_view = main_only<h_web_view>;
    p.table.open_bank = main_only<h_open_bank>;
    p.owner = m.env.hooks->new_guest("plugin:" + name);

    Hooks& hooks = *m.env.hooks;
    Loaded* lp = &p;
    // Each callback runs with the plugin's capabilities and counts as the plugin being busy (no unloading under it).
    auto scoped = [lp](auto fn) {
        return [lp, fn](auto&&... a) {
            return guarded(*lp, "a callback", [&] { return fn(std::forward<decltype(a)>(a)...); });
        };
    };
    if (p.pulse) hooks.on_pulse(p.owner, scoped([lp] { lp->pulse(); }));
    if (p.game_state) hooks.on_game_state(p.owner, scoped([lp](int s) { lp->game_state(s); }));
    if (p.begin_zone || p.end_zone || p.zoned)
        hooks.on_zone(p.owner, scoped([lp](ZonePhase z) {
            if (z == ZonePhase::Begin && lp->begin_zone) lp->begin_zone();
            if (z == ZonePhase::End && lp->end_zone) lp->end_zone();
            if (z == ZonePhase::Zoned && lp->zoned) lp->zoned();
        }));
    // Registered under the capability they need, and asked again each time: a narrower policy (policy_changed) keeps
    // the plugin loaded and its hooks registered, but what it lost it no longer sees.
    if (p.add_spawn || p.remove_spawn) {
        if (p.can(kCapRead)) {
            hooks.on_spawn(p.owner, scoped([lp](std::uint32_t id, bool added) {
                if (!lp->can(kCapRead)) return;
                if (added && lp->add_spawn) lp->add_spawn(id);
                if (!added && lp->remove_spawn) lp->remove_spawn(id);
            }));
        } else {
            p.denied("OnAddSpawn / OnRemoveSpawn", kCapRead);
        }
    }
    if (p.incoming_chat || p.filter_chat) {
        if (p.can(kCapEvents)) {
            hooks.on_chat(p.owner, scoped([lp](ChatLine& line) {
                // A line a script or plugin wrote is not incoming chat (MQ's own lines go to OnWriteChatColor, not
                // here): a plugin that acts on "Bob tells you, ..." would act on whatever another add-on printed.
                if (!lp->can(kCapEvents) || line.from_code) return Flow::Continue;
                if (lp->incoming_chat) lp->incoming_chat(line.text.c_str(), line.color);
                if (lp->filter_chat && lp->filter_chat(line.text.c_str(), line.color)) return Flow::Drop;
                return Flow::Continue;
            }));
        } else {
            p.denied("OnIncomingChat", kCapEvents);
        }
    }
    if (p.packet) {
        if (p.can(kCapPackets)) {
            for (Stream st : {Stream::World, Stream::Zone})
                for (Direction dir : {Direction::In, Direction::Out})
                    hooks.on_packet(p.owner, st, dir, 0, scoped([lp, st, dir](net::AppPacket& pk) {
                        if (!lp->can(kCapPackets)) return Flow::Continue;
                        HailstonePacket c{pk.opcode, reinterpret_cast<std::uint8_t*>(pk.data.data()), pk.data.size()};
                        net::AppPacket* before = std::exchange(lp->current_packet, &pk);
                        int drop = lp->packet(st == Stream::World ? HAILSTONE_STREAM_WORLD : HAILSTONE_STREAM_ZONE,
                                              dir == Direction::In ? HAILSTONE_IN : HAILSTONE_OUT, &c);
                        lp->current_packet = before;
                        pk.opcode = c.opcode;
                        if (c.size < pk.data.size()) pk.data.resize(c.size);
                        return drop ? Flow::Drop : Flow::Continue;
                    }));
        } else {
            p.denied("OnPacket", kCapPackets);
        }
    }
    if (p.update_imgui && !p.can(kCapUi)) p.denied("OnUpdateImGui", kCapUi);

    // Its Lua windows (<folder>/ui/<pack>/), with the UI capability.
    if (p.can(kCapUi) && m.env.ui) {
        // UiBridge stores the owner root; its windows live in root/ui/<pack>.
        if (p.builtin) {
            if (!p.builtin_ui_dir.empty()) m.env.ui->set_folder(name, p.builtin_ui_dir.parent_path());
        } else {
            m.env.ui->set_folder(name, plugin_folder(file, name));
        }
    }

    if (!guarded(p, "set_host", [&] { set_host(&p.table); return true; })) {
        discard(m, std::move(l), false);
        return refuse(name + " did not load: set_host failed");
    }
    m.loaded.push_back(std::move(l));
    std::erase_if(m.failures, [&](const Info& f) { return iequal(f.name, name); });
    // A plugin whose start lets an exception out is taken out again with all it registered so far.
    auto failed = [&](const char* where, bool shutdown) {
        auto it = std::find_if(m.loaded.begin(), m.loaded.end(), [&](const auto& x) { return x.get() == lp; });
        std::unique_ptr<Loaded> gone = std::move(*it);
        m.loaded.erase(it);
        discard(m, std::move(gone), shutdown);
        return refuse(std::format("{} did not load: its {} failed with an error", name, where));
    };
    if (lp->initialize && !guarded(*lp, "InitializePlugin", [&] {
            lp->initialize();
            return true;
        }))
        return failed("InitializePlugin", false);
    // The server extension: its hook table (read once, after InitializePlugin may have filled it), switches and
    // vitals as one member of the chain each connection asks.
    if (lp->hooks_fn) {
        bool threw = true;
        const HailstoneHooks* t = guarded(*lp, "hailstone_plugin_hooks", [&] {
            const HailstoneHooks* r = lp->hooks_fn();
            threw = false;
            return r;
        });
        if (threw) return failed("hailstone_plugin_hooks", true);
        if (t && t->size >= sizeof(std::uint32_t))
            std::memcpy(&lp->hooks, t, std::min<std::size_t>(t->size, sizeof(HailstoneHooks)));
        lp->hooks.size = sizeof(HailstoneHooks);
        if (!lp->can(kCapExtend)) may_extend(*lp, "hailstone_plugin_hooks");
        // The command line rewrite: on the hook bus, ahead of the commands, while the plugin is active.
        if (lp->hooks.command_line && lp->can(kCapExtend))
            hooks.on_command_line(p.owner, scoped([lp](std::string& line) {
                if (!lp->active || !lp->hooks.command_line || !lp->can(kCapExtend)) return Flow::Continue;
                if (const char* r = lp->hooks.command_line(line.c_str())) line = r;
                return Flow::Continue;
            }));
    }
    if (lp->can(kCapExtend) && m.env.extensions) {
        lp->extension = std::make_shared<NativeExtension>(*lp);
        std::weak_ptr<NativeExtension> ext = lp->extension;
        lp->chain_id = m.env.extensions->add(lp->extension, [ext](const net::GameConnection&) {
            auto x = ext.lock();
            return x && x->active();
        });
    }
    m.say(std::format("[Plugin] {} loaded ({})", name, caps_text(caps)));
    if (p.builtin) log::info("built-in add-on {} loaded ({})", name, caps_text(caps));
    else log::info("plugin {} loaded from {} (sha256 {}, caps {})", name, file.string(), to_hex(hash), caps_text(caps));
    return true;
}

bool NativePlugins::unload(std::string_view name) {
    Impl& m = *impl_;
    auto it = std::find_if(m.loaded.begin(), m.loaded.end(), [&](const auto& l) { return iequal(l->name, name); });
    if (it == m.loaded.end() || (*it)->builtin) return false;
    if ((*it)->busy) { // the plugin asked from its own code: not under its feet
        (*it)->unload_pending = true;
        return true;
    }
    std::unique_ptr<Loaded> l = std::move(*it);
    m.loaded.erase(it);
    const std::string gone = l->name;
    discard(m, std::move(l), true);
    m.say("[Plugin] " + gone + " unloaded");
    return true;
}

void NativePlugins::unload_all() {
    std::vector<std::string> names;
    for (const auto& l : impl_->loaded) names.push_back(l->name);
    for (auto it = names.rbegin(); it != names.rend(); ++it) unload(*it);
}

void NativePlugins::policy_changed(const std::vector<std::string>& autoload) {
    Impl& m = *impl_;
    std::vector<std::string> drop, revoked;
    for (auto& l : m.loaded) {
        if (l->builtin) continue;
        std::uint32_t caps = m.env.policy ? m.env.policy->caps(FileKind::Plugin, l->hash) : 0;
        if (caps == 0) {
            drop.push_back(l->name);
            continue;
        }
        // The player took back the permission for the server's plugin.
        if (m.asks(l->file) && m.consent_for(l->hash) != Consent::Allowed) {
            revoked.push_back(l->name);
            continue;
        }
        l->caps = caps;
        // No longer allowed to extend the client: out of the chain (a plugin that gains it needs a reload).
        if (l->extension && !l->can(kCapExtend)) {
            l->extension->detach();
            if (m.env.extensions) m.env.extensions->remove(l->chain_id);
            l->extension.reset();
        }
        // No longer allowed windows: its Lua windows, functions, options and menu entries go.
        if (!l->can(kCapUi) && m.env.ui) m.env.ui->remove(l->name);
        // No longer allowed its own connections: they close (tcp_state -1 from now on).
        if (!l->can(kCapNet)) l->tcp.clear();
    }
    for (const auto& n : drop) {
        m.say("[Plugin] " + n + ": it may not run here");
        unload(n);
    }
    for (const auto& n : revoked) {
        m.say("[Plugin] " + n + ": you no longer allow it on this server");
        unload(n);
    }
    if (!m.env.policy || !m.env.policy->received()) {
        if (!m.pending.empty()) ++m.consent_serial;
        m.pending.clear(); // nothing runs out of the world: no question either
        return;
    }
    if (distribution::managed()) return;
    // The server's own plugins always load; the player's those `autoload` names.
    bool all = std::find(autoload.begin(), autoload.end(), "*") != autoload.end();
    const auto found = list();
    // A question about a plugin that may no longer run, or whose bytes changed, goes.
    const auto before = m.pending.size();
    std::erase_if(m.pending, [&](const ConsentFile& p) {
        return std::none_of(found.begin(), found.end(), [&](const Info& i) {
            return i.asks && i.caps != 0 && !i.loaded && i.file == p.file && i.sha256 == to_hex(p.sha256);
        });
    });
    if (m.pending.size() != before) ++m.consent_serial;
    for (const Info& i : found) {
        if (i.loaded || i.caps == 0) continue;
        if (i.asks && i.consent == Consent::Denied) continue; // the player said no: not asked again at each zone-in
        bool wanted = i.server || all ||
                      std::any_of(autoload.begin(), autoload.end(), [&](const std::string& a) { return iequal(a, i.name); });
        if (wanted) load_file(i.name, nullptr, Asker::Rules);
    }
}

void NativePlugins::set_server_dir(const fs::path& dir) {
    Impl& m = *impl_;
    if (dir == m.server_dir) return;
    m.server_dir = dir;
    if (!m.pending.empty()) ++m.consent_serial;
    m.pending.clear(); // another server's question
    m.session_answers.clear();
}

std::optional<ConsentPrompt> NativePlugins::consent() const {
    if (distribution::managed()) return std::nullopt;
    const Impl& m = *impl_;
    if (m.pending.empty()) return std::nullopt;
    ConsentPrompt p;
    p.profile_dir = m.profile_dir();
    p.files = m.pending;
    p.serial = m.consent_serial;
    return p;
}

void NativePlugins::answer_consent(std::optional<bool> allow) {
    if (distribution::managed()) return;
    Impl& m = *impl_;
    if (m.pending.empty()) return;
    std::vector<ConsentFile> files = std::move(m.pending);
    m.pending.clear();
    ++m.consent_serial;
    if (!allow) return; // put away: asked again at the next zone-in
    for (const auto& f : files) {
        log::info("plugin {}: the player {} it on this server (sha256 {})", f.name, *allow ? "allowed" : "refused",
                  to_hex(f.sha256));
        if (!set_plugin_consent(m.profile_dir(), f.name, f.sha256, *allow)) {
            std::erase_if(m.session_answers, [&](const PluginApproval& a) { return a.sha256 == f.sha256; });
            m.session_answers.push_back({f.name, f.sha256, *allow});
        }
    }
    for (const auto& f : files) {
        if (*allow) load_file(f.name, nullptr, Asker::Rules); // read and hashed again: other bytes than these ask again
        else m.say("[Plugin] " + f.name + " is not loaded: you did not allow it. The add-on list on the login screen "
                   "can ask again.");
    }
}

void NativePlugins::recheck_consent() {
    if (distribution::managed()) return;
    Impl& m = *impl_;
    std::vector<ConsentFile> allowed;
    const auto before = m.pending.size();
    std::erase_if(m.pending, [&](const ConsentFile& f) {
        const Consent c = m.consent_for(f.sha256);
        if (c == Consent::Allowed) allowed.push_back(f);
        return c != Consent::Unasked;
    });
    if (m.pending.size() == before) return;
    ++m.consent_serial;
    for (const auto& f : allowed) load_file(f.name, nullptr, Asker::Rules);
}

void NativePlugins::imgui() {
    for (std::size_t i = 0; i < impl_->loaded.size(); ++i) {
        Loaded& l = *impl_->loaded[i];
        if (!l.update_imgui || !l.can(kCapUi)) continue;
        guarded(l, "OnUpdateImGui", [&] { l.update_imgui(); });
    }
}

void NativePlugins::Impl::command(std::string_view args) {
    if (distribution::managed()) { say("[Plugin] User add-ons are disabled in this distribution."); return; }
    std::string_view rest = args;
    auto word = [&] {
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        std::size_t end = rest.find(' ');
        std::string w(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end);
        return w;
    };
    std::string first = word(), second = word();
    if (first.empty() || iequal(first, "list")) {
        auto all = self->list();
        if (all.empty()) say("[Plugin] no plugins in the plugin folders");
        for (const auto& i : all) {
            const char* state = i.loaded ? (i.server ? "loaded (the server's)" : "loaded")
                                : !i.caps ? "may not run on this server"
                                : i.asks && i.consent == Consent::Denied ? "not loaded: you did not allow it"
                                : i.asks && i.consent == Consent::Unasked ? "not loaded: waits for your answer"
                                                                          : "not loaded";
            say(std::format("[Plugin] {} - {}{}", i.name, state, i.caps ? " (" + caps_text(i.caps) + ")" : ""));
        }
        return;
    }
    if (iequal(second, "unload")) {
        if (const Loaded* l = find(first); l && kept(l->hash)) say("[Plugin] " + l->name + " is this server's own plugin: it stays loaded");
        else if (!self->unload(first)) say("[Plugin] " + first + " is not loaded");
        return;
    }
    if (second.empty() || iequal(second, "load")) {
        self->load(first);
        return;
    }
    say("[Plugin] usage: /plugin list | /plugin <name> [load|unload]");
}

} // namespace hailstone::plugin
