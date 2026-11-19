#include "plugin/native_plugins.h"
#include "plugin/raid_snapshot.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <tuple>
#include <utility>

#include <SDL3/SDL_loadso.h>

#include "core/ascii_case.h"
#include "core/atomic_file.h"
#include "core/binary_measurement.h"
#include "core/log.h"
#include "core/process_serial.h"
#include "game/targeting.h"
#include "net/item_packets.h"
#include "core/sha256.h"
#include "game/chat_text.h"
#include "game/combat.h"
#include "game/eq_names.h"
#include "game/item_links.h"
#include "game/merchant.h"
#include "game/server_ext_chain.h"
#include "game/spa_ids.h"
#include "game/spells.h"
#include "game/xtarget.h"
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

namespace hailstone::plugin {

namespace fs = std::filesystem;

namespace {

static_assert(HAILSTONE_CAP_READ == kCapRead && HAILSTONE_CAP_COMMANDS == kCapCommands && HAILSTONE_CAP_TARGET == kCapTarget &&
              HAILSTONE_CAP_ACTIONS == kCapActions && HAILSTONE_CAP_MOVE == kCapMove && HAILSTONE_CAP_CHAT == kCapChat &&
              HAILSTONE_CAP_EVENTS == kCapEvents && HAILSTONE_CAP_UI == kCapUi && HAILSTONE_CAP_PACKETS == kCapPackets &&
              HAILSTONE_CAP_EXTEND == kCapExtend && HAILSTONE_CAP_LOGIN == kCapLogin && HAILSTONE_CAP_NET == kCapNet &&
              HAILSTONE_CAP_WEB == kCapWeb);
static_assert(offsetof(HailstoneMapPolicy, revision)==72 && offsetof(HailstoneMapPolicy, context)==80 && sizeof(HailstoneMapPolicy)==88);
// The add-on service records' sizes as released (docs/plugins.md "Server add-on services"): a field moved or added
// anywhere but the end breaks a plugin built against an earlier header. The submission holds a pointer and a size_t.
static_assert(sizeof(HailstoneFollowProfile) == 56 && sizeof(HailstoneMountProfile) == 8);
static_assert(sizeof(HailstoneFollowStatus) == 24 && offsetof(HailstoneFollowStatus, target_incarnation) == 16);
static_assert(sizeof(HailstoneBinaryMeasurement) == 56 && offsetof(HailstoneBinaryMeasurement, sha256) == 24);
static_assert(sizeof(HailstoneMerchantSellContext) == 32);
static_assert(sizeof(HailstoneWorldFrame) == 136 && offsetof(HailstoneWorldFrame, now_ms) == 24);
static_assert(sizeof(HailstoneWorldAnchor) == 32 && sizeof(HailstoneWorldVertex) == 28);
static_assert(sizeof(HailstoneWorldSurfacePatch) == 64 && sizeof(HailstoneWorldSurfacePatchResult) == 576 &&
              offsetof(HailstoneWorldSurfacePatchResult, region_xy) == 64);
static_assert(sizeof(HailstoneWorldReceipt) == 48);
static_assert(sizeof(void*) != 8 || (sizeof(HailstoneWorldSubmission) == 56 && offsetof(HailstoneWorldSubmission, vertices) == 40));

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
} // namespace

struct NativePlugins::Impl {
    NativeEnv env;
    NativePlugins* self = nullptr;
    Hooks::Owner owner = 0;
    std::vector<std::unique_ptr<Loaded>> loaded;
    WorldGeometryHost world_geometry;
    std::vector<Hooks::Owner> world_owners;
    std::vector<Info> failures; // the last load error by name
    std::vector<std::pair<std::string, Sha256Digest>> required; // files found at the last policy check
    // What required_addons_ready answered, and what it answered from: it is asked each frame, and works it out again
    // only when the rules, the answers or what is loaded change.
    struct ReadyInputs {
        std::uint32_t policy_serial = 0, approvals_generation = 0;
        std::vector<std::pair<std::string, Sha256Digest>> required;
        // A loaded plugin's name, file, capabilities and how many of its callbacks have let an exception out.
        std::vector<std::tuple<std::string, Sha256Digest, std::uint32_t, std::size_t>> loaded;
        std::vector<std::pair<std::string, std::string>> failures;
        std::vector<Sha256Digest> pending;
        std::vector<std::pair<Sha256Digest, bool>> session_answers;
        bool operator==(const ReadyInputs&) const = default;
    };
    mutable std::optional<ReadyInputs> ready_inputs;
    mutable bool ready_answer = true;
    mutable std::string ready_problem;

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
               (env.policy->server_file(FileKind::Plugin, hash) || env.policy->required_file(FileKind::Plugin, hash));
    }
    void say(const std::string& text) const {
        if (env.print) env.print(text, 0);
    }
    void command(std::string_view args);

    // The player's word on the server's plugins (plugin/consent.h): the ones waiting for it, and answers that could
    // not be written to servers/<profile>/plugin_approvals.txt (they hold for this run only).
    std::vector<ConsentFile> pending;
    std::uint32_t consent_serial = 0;
    std::vector<PluginApproval> session_answers;
    // A file from the server's plugins/ folder (set_server_dir): it loads only with the player's word.
    bool asks(const fs::path& file, const Sha256Digest& hash) const {
        if (env.policy && env.policy->required_file(FileKind::Plugin, hash)) return true;
        return in_server_dir(file);
    }
    // A library the server sent (servers/<profile>/plugins/), not one of the player's.
    bool in_server_dir(const fs::path& file) const {
        if (server_dir.empty()) return false;
        const fs::path rel = file.lexically_relative(server_dir);
        return !rel.empty() && *rel.begin() != "..";
    }
    fs::path profile_dir() const {
        if (!server_dir.empty()) return server_dir.parent_path();
        return env.policy && env.policy->current() ? env.policy->current()->dir : fs::path();
    }
    // plugin_approvals.txt as last read: consent_for is asked each frame (required_addons_ready), and reads the file
    // again only when this process wrote one, the rules changed or the server is another.
    struct ApprovalsRead {
        fs::path dir;
        std::uint32_t generation = 0, policy_serial = 0;
        std::vector<PluginApproval> answers;
    };
    mutable std::optional<ApprovalsRead> approvals;
    Consent consent_for(const Sha256Digest& hash) const {
        const fs::path dir = profile_dir();
        const std::uint32_t generation = plugin_approvals_generation();
        const std::uint32_t serial = env.policy ? env.policy->serial() : 0;
        if (!approvals || approvals->dir != dir || approvals->generation != generation ||
            approvals->policy_serial != serial)
            approvals = ApprovalsRead{dir, generation, serial, read_plugin_approvals(dir)};
        Consent c = consent_in(approvals->answers, hash);
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
    Sha256Digest hash{};
    std::uint32_t caps = 0;
    SDL_SharedObject* lib = nullptr;
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
    bool world_ready = false;
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
    bool host_threw_warned = false; // (host_threw)
    bool map_policy_running = false; // get_map_policy cannot recursively invoke its caller's policy hook
    std::uint64_t policy_generation = core::next_process_identity(), map_content_revision = 0;
    std::uint64_t lifecycle_generation = core::next_process_identity();
    // Owns the class names it set (set_class_name; game/eq_names.h), taken back with its `extend` or its unload.
    std::uint64_t class_names_owner = core::next_process_identity();
    bool class_name_refused = false; // said once that set_class_name was refused

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
    std::uint64_t lifecycle_revision(const net::GameConnection&) const override {
        return l_ ? l_->lifecycle_generation : 0;
    }

    std::optional<game::FollowProfile> follow_profile(const net::GameConnection&) const override {
        if (!l_ || !active() || !l_->can(kCapExtend) || !h().follow_profile) return std::nullopt;
        HailstoneFollowProfile raw{}; raw.size = sizeof(raw);
        if (!run(h().follow_profile, &raw) || !l_ || !active() || l_->unload_pending || !l_->can(kCapExtend) || raw.size < sizeof(raw)) return std::nullopt;
        const game::FollowProfile out{raw.flags, raw.stop_distance, raw.restart_margin, raw.start_range,
            raw.turn_snap, raw.turn_slow, raw.turn_fast, raw.turn_fast_angle, raw.move_angle,
            raw.move_far_margin, raw.pitch_gain, raw.pitch_down_scale, raw.pitch_limit};
        return game::valid_follow_profile(out) ? std::optional{out} : std::nullopt;
    }
    std::optional<game::MountProfile> mount_profile(const net::GameConnection&) const override {
        if (!l_ || !active() || !l_->can(kCapExtend) || !h().mount_profile) return std::nullopt;
        HailstoneMountProfile raw{}; raw.size = sizeof(raw);
        if (!run(h().mount_profile, &raw) || !l_ || !active() || l_->unload_pending || !l_->can(kCapExtend) || raw.size < sizeof(raw) || raw.instant_acceleration > 1) return std::nullopt;
        return game::MountProfile{raw.instant_acceleration != 0};
    }
    bool map_visible(const net::GameConnection& c, const game::Spawn& s, std::uint32_t purpose) const override {
        if (!l_ || !active() || !l_->can(kCapUi | kCapExtend) || !h().map_visible) return true;
        const auto value = spawn_struct(c, s);
        return run(h().map_visible, &value, purpose) != 0;
    }
    game::MapPolicy map_policy(const net::GameConnection&) const override {
        if (!l_) return {};
        const auto neutral = [&] { return game::MapPolicy{0, {}, l_ ? l_->policy_generation : 0}; };
        if (!active() || !l_->can(kCapUi | kCapExtend) || !h().map_policy || l_->map_policy_running) return neutral();
        struct Reset { bool& running; ~Reset() { running = false; } } reset{l_->map_policy_running};
        l_->map_policy_running = true;
        HailstoneMapPolicy raw{};
        raw.size = sizeof(raw);
        constexpr auto known_flags = HAILSTONE_MAP_POLICY_CUSTOM_SPAWNS |
            HAILSTONE_MAP_POLICY_HIDE_STOCK_NPCS | HAILSTONE_MAP_POLICY_DISABLE_NPC_FIND;
        if (!run(h().map_policy, &raw) || !l_ || !active() || l_->unload_pending ||
            !l_->can(kCapUi | kCapExtend) || raw.size < offsetof(HailstoneMapPolicy, revision) || (raw.flags & ~known_flags)) return neutral();
        const auto end = std::find(std::begin(raw.name), std::end(raw.name), '\0');
        if (end == std::end(raw.name) || std::any_of(std::begin(raw.name), end, [](unsigned char ch) {
                return ch < 32 || ch == 127;
            })) return neutral();
        const auto content = raw.size >= offsetof(HailstoneMapPolicy, context) ? raw.revision : 0;
        if (l_->map_content_revision != content) {
            l_->map_content_revision = content;
            l_->policy_generation = core::next_process_identity();
        }
        return {raw.flags, std::string(std::begin(raw.name), end), l_->policy_generation};
    }
    bool merchant_right_click(net::GameConnection& c, std::uint32_t merchant, const net::ItemSlot& slot,
                              std::uint32_t item, std::uint32_t quantity) override {
        if (!l_ || !active() || !l_->can(kCapPackets | kCapExtend) || !h().merchant_right_click) return false;
        HailstoneMerchantSellContext context{sizeof(HailstoneMerchantSellContext), merchant,
            static_cast<std::uint32_t>(slot.slot), item, quantity, {slot.type, slot.slot, slot.sub, slot.aug}, 0};
        if (c.inventory().cursor()) context.flags |= HAILSTONE_MERCHANT_CURSOR_OCCUPIED;
        const auto* received = c.inventory().at(slot);
        if (!received || game::sell_refusal(*received)) context.flags |= HAILSTONE_MERCHANT_SELL_REFUSED;
        if (received && c.kept_items().holds_kept(*received)) context.flags |= HAILSTONE_MERCHANT_KEPT;
        return run(h().merchant_right_click, &context) != 0;
    }
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
    std::optional<std::string> item_flags(const net::GameConnection&, const net::Item&,
                                         std::string_view) const override;
    std::optional<std::string> item_description_append(const net::GameConnection&, const net::Item&) const override;
    std::uint64_t item_presentation_revision(const net::GameConnection&) const override {
        return l_ && h().item_presentation_revision ? run(h().item_presentation_revision) : 0;
    }
    std::optional<std::string> loot_confirmation(const net::GameConnection&, const net::Item&,
                                                std::span<const std::uint32_t>, bool) const override;
    std::optional<std::string> zone_display_name(const net::GameConnection&, std::uint32_t zone, std::uint32_t instance) const override {
        if (!l_ || !active() || !h().zone_display_name) return {};
        const auto* result = run(h().zone_display_name, zone, instance);
        if (!result) return {};
        std::size_t length = 0; while (length < 128 && result[length]) ++length;
        if (!length || length == 128) return {};
        return std::string(result, length);
    }


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
    if (!out) return 0;
    Size size{};
    std::memcpy(&size, out, sizeof(size)); // an older prefix may have a smaller alignment than the current record
    if (size < sizeof(Size)) return 0;
    std::memcpy(out, &full, std::min<std::size_t>(size, sizeof(T)));
    std::memcpy(out, &size, sizeof(size));
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
    copy_name(v.serial, it.serial);
    v.instance_id = it.instance_id;
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
    // The serialized record is decoded completely or rejected. Missing
    // children therefore mean known empty positions, not unreadable sockets.
    // A bag's children are inventory items and never an augment configuration.
    v.configuration_complete = it.id != 0 && it.item_class <= 2 ? 1u : 0u;
    copy_name(v.serial, it.serial);
    v.instance_id = it.instance_id;
    if (it.item_class != 1 && v.configuration_complete) {
        std::array<bool, 6> seen{};
        if (it.contents.size() > seen.size()) v.configuration_complete = 0;
        else for (const auto& child : it.contents) {
            if (child.index >= seen.size() || !child.item.id || seen[child.index]) {
                v.configuration_complete = 0;
                break;
            }
            seen[child.index] = true;
            v.augment_ids[child.index] = child.item.id;
        }
        if (!v.configuration_complete) std::fill(std::begin(v.augment_ids), std::end(v.augment_ids), 0u);
    }

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
    v.incarnation = s.incarnation;
    if (s.hp_known && s.hp_percent <= 100) v.observation_flags |= HAILSTONE_SPAWN_HP_KNOWN;
    const auto resource = [&](const auto& values, std::uint32_t bit, std::uint32_t& value) {
        const auto it = values.find(s.id);
        if (it != values.end() && it->second <= 100) {
            value = it->second;
            v.observation_flags |= bit;
        }
    };
    resource(c.group().mana_percent, HAILSTONE_SPAWN_MANA_KNOWN, v.mana_percent);
    resource(c.group().endurance_percent, HAILSTONE_SPAWN_ENDURANCE_KNOWN, v.endurance_percent);
    if (s.trader) v.social_flags |= HAILSTONE_SOCIAL_TRADER;
    if (s.buyer) v.social_flags |= HAILSTONE_SOCIAL_BUYER;
    if (s.linkdead) v.social_flags |= HAILSTONE_SOCIAL_LINKDEAD;
    if (s.lfg) v.social_flags |= HAILSTONE_SOCIAL_LFG;
    if (s.afk) v.social_flags |= HAILSTONE_SOCIAL_AFK;
    v.anon = s.anon;
    copy_name(v.last_name, s.last_name);
    copy_name(v.guild_name, c.guild().name_of(s.guild_id));
    copy_name(v.display_name, game::display_name(s.name));

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
    if (may_extend(l, "set_active")) {
        if (l.active != (active != 0)) {
            l.policy_generation = core::next_process_identity();
            l.lifecycle_generation = core::next_process_identity();
        }
        l.active = active != 0;
        if (!l.active) { l.world_ready = false; l.host->world_geometry.retire(l.owner); }
    }
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

void h_set_class_name(void* h, std::uint32_t class_id, const char* name, const char* short_name) {
    Loaded& l = Loaded::of(h);
    if (!may_extend(l, "set_class_name")) return;
    const std::string_view n = name ? name : "", s = short_name ? short_name : "";
    if (!game::set_class_name_override(l.class_names_owner, class_id, n, s) && !std::exchange(l.class_name_refused, true))
        log::warn("plugin {}: set_class_name({}) refused: a player class is 1-16, a name at most {} bytes and a short "
                  "name at most {}, without control characters", l.name, class_id, game::kClassNameMax,
                  game::kClassShortNameMax);
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
    v.zone_instance = c->zone_instance();
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
    v.in_zone = c->in_zone() ? 1 : 0;
    if (w.player() && w.player()->incarnation && c->profile() && c->in_zone() && !c->in_combat() && !c->rest_downtime_left() && !c->stunned() &&
        !ps.dead && (ps.stand_state == 100 || ps.stand_state == 110) && !spells.casting.casting() &&
        !spells.scribing && c->player_stationary()) {
        bool debuffed = false;
        for (const auto& buff : spells.buffs) {
            const auto* spell = buff.empty() ? nullptr : c->spell_table().get(buff.spell);
            if (!buff.empty() && (!spell || !spell->beneficial())) { debuffed = true; break; }
        }
        if (!debuffed) v.observation_flags |= HAILSTONE_PLAYER_PROMPT_SAFE;
    }
 // camping too: the plugins' windows stay up through the camp's 30 s
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

// The bytes write_str16 writes for sv: its u16 length and that many bytes (a longer string is cut to the length).
inline std::size_t str16_size(std::string_view sv) {
    return sizeof(uint16_t) + static_cast<uint16_t>(sv.size());
}

inline void write_str16(unsigned char*& dst, std::string_view sv) {
    uint16_t len = static_cast<uint16_t>(sv.size());
    std::memcpy(dst, &len, sizeof(len));
    dst += sizeof(len);
    if (len > 0) {
        std::memcpy(dst, sv.data(), len);
        dst += len;
    }
}


static_assert(sizeof(HS_Bulk_SpawnHeader) == HS_BULK_SPAWN_HEADER_SIZE);
static_assert(sizeof(HS_Bulk_SpawnFixedRecord) == HS_BULK_SPAWN_RECORD_SIZE);

/// <summary>
/// This is highly optimized code. AI should not modify this code method.
/// PR reviewwr ask the submiter for changes to be reviewed by the submitter
/// may need to break this up, but you need a sizable buffer for this or boom.
/// for 5,000 spawns, assume 2 megs. now 5k is wayyy higher than it should ever be
/// so can get away with less, but you never know. 
/// there is a safety check that if we get down to our last 4,096 bytes, kick out and just return with what we have.
/// on top of that, each record's whole size (fixed part and its seven strings) is measured before it is written,
/// and a record that would not fit before data + datamax ends the list there: names and suffixes come from the
/// server uncapped, so 4,096 bytes alone do not bound a record. No partial record is written.
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer to put data in</param>
/// <param name="datamax">suggestion about 2megs for up to 5k spawns, but has to be greater than 4096</param>
/// <returns></returns>
int h_get_spawns_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    const net::GameConnection* c = readable(Loaded::of(h), "get_spawns_data_bulk");
    if (!c) return 0;
    if (!data || datamax < 4096) return 0;
    const auto& world = c->world();
    const auto& spawns = world.spawns;
    const auto* player = world.player();
    const int playerLevel = player ? player->level : 0;
    const std::int32_t pvpRules = c->pvp_rules();

    HS_BulkSpawnHeader* header = reinterpret_cast<HS_Bulk_SpawnHeader*>(data);
    header->spawnCount = 0;
    header->playerX = player ? player->position.x : 0.0f;
    header->playerY = player ? player->position.y : 0.0f;
    header->playerZ = player ? player->position.z : 0.0f;
   

    unsigned char* pBuffer = data + sizeof(HS_Bulk_SpawnHeader);
    const unsigned char* pSafeLimit = data + (datamax - 4096);
    const unsigned char* pEnd = data + datamax;
    uint32_t count = 0;

    for (const auto& [id, spawn] : spawns) {
        if (pBuffer > pSafeLimit) break;
        const game::eSpawnType spawnType = game::Spawn::GetSpawnType(&spawn);
        float maxRange=0;
        if (spawnType != hailstone::game::eSpawnType::ITEM) maxRange = 14.5;
        float maxRangeTo = 0;
        if (spawnType != hailstone::game::eSpawnType::ITEM) maxRangeTo = 14.5;

        uint32_t flags = HS_BULK_SPAWN_NONE;
        if (spawn.afk)                                  flags |= HS_BULK_SPAWN_AFK;
        if ((spawn.player_state & 0x0C) != 0)           flags |= HS_BULK_SPAWN_AGGRESSIVE; // this is checking 0x4 and 0x8 at the same time.
        if (spawn.anon == 1)                            flags |= HS_BULK_SPAWN_ANON;
        if (spawn.buyer)                                flags |= HS_BULK_SPAWN_BUYER;
        if (spawn.stand_state == STANDSTATE_DEAD)       flags |= HS_BULK_SPAWN_DEAD;
        if (spawn.stand_state == STANDSTATE_DUCK)       flags |= HS_BULK_SPAWN_DUCKING;
        if (spawn.stand_state == STANDSTATE_FEIGN)      flags |= HS_BULK_SPAWN_FEIGNING;
        if (spawn.gm)                                   flags |= HS_BULK_SPAWN_GM;
        if (spawn.summoned)                             flags |= HS_BULK_SPAWN_SUMMONED;
        if (spawn.fly_mode == 2)                        flags |= HS_BULK_SPAWN_LEVITATION;
        if (spawn.linkdead)                             flags |= HS_BULK_SPAWN_LINKDEAD;
        if (spawn.mount_id > 0)                         flags |= HS_BULK_SPAWN_MOUNT;
        if (std::fabs(spawn.speed_run) > 0.0f)          flags |= HS_BULK_SPAWN_MOVING;
        if (game::Spawn::IsNamed(&spawn))               flags |= HS_BULK_SPAWN_NAMED;
        if (spawn.anon == 2)                            flags |= HS_BULK_SPAWN_ROLEPLAYING;
        if (spawn.stand_state == STANDSTATE_SIT)        flags |= HS_BULK_SPAWN_SITTING;
        if (spawn.stand_state == STANDSTATE_STAND)      flags |= HS_BULK_SPAWN_STANDING;
        if ((spawn.player_state & 0x20) != 0)           flags |= HS_BULK_SPAWN_STUNNED;
        if (spawn.targetable)                           flags |= HS_BULK_SPAWN_TARGETABLE;
        if (spawn.trader)                               flags |= HS_BULK_SPAWN_TRADER;
      

        std::string_view bodyDesc = "";
        if (static_cast<std::size_t>(spawn.body_type) < std::size(game::szBodyType)) { // body_type is unsigned
            bodyDesc = game::szBodyType[spawn.body_type];
        }
        // CleanName and DisplayName are the same text, as ${Spawn.CleanName} and ${Spawn.DisplayName} give them.
        const std::string displayName = game::display_name(spawn.name);
        const std::string_view raceName = hailstone::game::Spawn::GetRaceIDName(spawn.race);
        const std::string_view typeDesc = game::Spawn::GetTypeDesc(spawnType);
        const std::size_t recordSize = sizeof(HS_Bulk_SpawnFixedRecord) + str16_size(bodyDesc)
            + 2 * str16_size(displayName) + str16_size(spawn.name) + str16_size(raceName)
            + str16_size(spawn.suffix) + str16_size(typeDesc);
        if (recordSize > static_cast<std::size_t>(pEnd - pBuffer)) break;

        HS_Bulk_SpawnFixedRecord* rec = reinterpret_cast<HS_Bulk_SpawnFixedRecord*>(pBuffer);
        rec->id = spawn.id;
        rec->flags = flags;
        rec->bodyTypeID = spawn.body_type;
        rec->classID = spawn.class_id;
        rec->conColorID = static_cast<int32_t>(game::con_chat_color(game::con_level(spawn, playerLevel, pvpRules)));
        rec->currentHps = spawn.hp_percent;
        rec->genderId = spawn.gender;
        rec->guildID = spawn.guild_id;
        rec->heading = spawn.heading * 0.703125f;
        rec->height = spawn.heights.avatar_height;
        rec->level = spawn.level;
        rec->master = spawn.pet_owner_id;
        rec->maxRange = maxRange;
        rec->maxRangeTo = maxRangeTo;
        rec->pctHPs = spawn.hp_percent;
        rec->playerState = spawn.player_state;
        rec->raceID = spawn.race;
        rec->x = spawn.position.x;
        rec->y = spawn.position.y;
        rec->z = spawn.position.z;
        rec->deity = spawn.deity;
        pBuffer += sizeof(HS_Bulk_SpawnFixedRecord);

        write_str16(pBuffer, bodyDesc);
        write_str16(pBuffer, displayName); // CleanName
        write_str16(pBuffer, displayName); // DisplayName
        write_str16(pBuffer, spawn.name);
        write_str16(pBuffer, raceName);
        write_str16(pBuffer, spawn.suffix);
        write_str16(pBuffer, typeDesc);
        count++;
    }
    header->spawnCount = count;
    return static_cast<int>(pBuffer - data);
}

int h_get_spawn(void* h, std::uint32_t id, HailstoneSpawn* out) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_spawn");
    if (!c) return 0;
    auto it = c->world().spawns.find(id);
    return it == c->world().spawns.end() ? 0 : give(out, spawn_struct(*c, it->second));
}

// A current roster member's in-zone actor: a player (never an NPC or corpse) of a roster name.
const game::RaidMember* raid_member_of(const net::GameConnection& c, const game::Spawn& actor) {
    if (actor.is_npc || actor.is_corpse) return nullptr;
    const auto& members = c.raid().members;
    const auto it = std::find_if(members.begin(), members.end(), [&](const auto& m) { return iequal(m.name, actor.name); });
    return it == members.end() ? nullptr : &*it;
}
int h_select_raid_member(void* h, std::uint32_t id, std::uint64_t incarnation) {
    auto& l = Loaded::of(h);
    if (!l.active || !l.can(kCapRead | kCapTarget) || !id || !incarnation) return 0;
    auto* c = connection_of(l);
    if (!c || !c->in_zone() || !c->raid().active || c->player_status().dead || c->interface_locked()) return 0;
    const auto& spawns = c->world().spawns;
    const auto actor = spawns.find(id);
    if (actor == spawns.end() || actor->second.incarnation != incarnation || !game::is_targetable(actor->second) ||
        c->market_hidden(actor->second)) return 0;
    if (!raid_member_of(*c, actor->second)) {
        // A member's pet: an NPC whose owner is a member in the zone.
        if (!actor->second.is_npc || actor->second.is_corpse || !actor->second.pet_owner_id) return 0;
        const auto owner = spawns.find(actor->second.pet_owner_id);
        if (owner == spawns.end() || !raid_member_of(*c, owner->second)) return 0;
    }
    c->set_target(id);
    return 1;
}
std::optional<net::ItemPacket> received_item(const void* data, std::size_t size) {
    if (!data || size < 4 || size > 262144) return std::nullopt;
    net::AppPacket packet; packet.data.resize(size); std::memcpy(packet.data.data(), data, size);
    return net::decode_item_packet(packet);
}
int h_decode_item_packet(void* h, const void* data, std::size_t size, HailstoneItemSlot* slot, HailstoneItem* out) {
    auto& l = Loaded::of(h);
    if (!slot || !out || !readable(l, "decode_item_packet")) return 0;
    const auto item = received_item(data, size);
    if (!item || !item->item.id) return 0;
    const auto& s = item->item.slot;
    if (!give(out, item_struct(item->item))) return 0;
    *slot = {s.type, s.slot, s.sub, s.aug};
    return 1;
}
int h_inspect_item_packet(void* h, const void* data, std::size_t size) {
    auto& l = Loaded::of(h);
    if (!l.active || !l.can(kCapRead | kCapUi)) return 0;
    auto* c = connection_of(l);
    const auto item = received_item(data, size);
    if (!c || !c->in_zone() || !item || !item->item.id) return 0;
    c->inspect_received_item(item->item); return 1;
}
int h_manual_follow(void* h, const char* action) {
    auto& l = Loaded::of(h);
    if (!l.active || !l.can(kCapMove | kCapExtend) || !action) return 0;
    auto* c = connection_of(l);
    if (!c || !c->in_zone()) return 0;
    std::size_t length = 0; while (length < 8 && action[length]) ++length;
    if (length == 8) return 0;
    const std::string_view value(action, length);
    if (value != "start" && value != "stop" && value != "toggle") return 0;
    if (value != "stop" && (c->interface_locked() || c->player_status().dead)) return 0;
    return c->manual_follow(value) ? 1 : 0;
}
int h_get_follow_status(void* h, HailstoneFollowStatus* out) {
    auto& l = Loaded::of(h);
    const auto* c = readable(l, "get_follow_status");
    if (!c || !c->in_zone()) return 0;
    HailstoneFollowStatus status{}; status.size = sizeof(status);
    if (c->enhanced_follow_active() && !c->enhanced_follow_current()) return give(out, status);
    status.target_id = c->follow_target(); status.active = status.target_id ? 1u : 0u;
    status.enhanced = c->enhanced_follow_active() ? 1u : 0u;
    const auto actor = c->world().spawns.find(status.target_id);
    if (actor != c->world().spawns.end()) status.target_incarnation = actor->second.incarnation;
    return give(out, status);
}
std::uint32_t h_get_memorized_spell(void* h, std::uint32_t index) {
    const auto* c = readable(Loaded::of(h), "get_memorized_spell");
    if (!c || !c->in_zone() || index >= static_cast<std::uint32_t>(std::max(0,c->usable_gems())) ||
        index >= c->spell_state().gems.size()) return 0;
    const auto id = c->spell_state().gems[index].spell;
    return id == 0xffffffffu ? 0 : id;
}
bool world_allowed(const Loaded& l) {
    const auto* c = connection_of(l);
    return l.active && !l.unload_pending && l.can(kCapUi | kCapExtend) && c && c->in_zone();
}

std::uint32_t h_world_capabilities(void* h) {
    const auto& l = Loaded::of(h);
    return world_allowed(l) && l.world_ready ? l.host->world_geometry.capabilities() : 0;
}

int h_world_floor_below(void* h, std::uint64_t frame, std::uint32_t epoch,
                         float x, float y, float z, float* out) {
    auto& l = Loaded::of(h);
    return world_allowed(l) ? l.host->world_geometry.floor(frame, epoch, x, y, z, out)
                            : HAILSTONE_WORLD_DEFERRED;
}

int h_world_anchor(void* h, std::uint64_t frame, std::uint32_t epoch,
                    std::uint32_t id, HailstoneWorldAnchor* out) {
    auto& l = Loaded::of(h);
    return world_allowed(l) ? l.host->world_geometry.anchor(frame, epoch, id, out)
                            : HAILSTONE_WORLD_DEFERRED;
}

int h_world_submit(void* h, const HailstoneWorldSubmission* submission) {
    auto& l = Loaded::of(h);
    return world_allowed(l) && l.host->world_geometry.submit(l.owner, submission) ? 1 : 0;
}

int h_world_surface_patch(void* h, const HailstoneWorldSurfacePatch* query,
                           HailstoneWorldSurfacePatchResult* out) {
    auto& l = Loaded::of(h);
    return world_allowed(l) ? l.host->world_geometry.surface_patch(query, out)
                            : HAILSTONE_WORLD_PATCH_DEFERRED;
}

int h_measure_binary(void* h, std::uint32_t kind, HailstoneBinaryMeasurement* out) {
    const auto& l = Loaded::of(h);
    if (!l.can(kCapRead) || !out || out->size < sizeof(out->size)) return 0;
    std::optional<BinaryMeasurement> measured;
    if (kind == HAILSTONE_BINARY_EXECUTABLE) measured = measure_process_executable();
    else if (kind == HAILSTONE_BINARY_CALLING_PLUGIN) {
        measured = measure_binary_file_cached(l.copy);
        if (measured && measured->sha256 != l.hash) return 0;
    } else return 0;
    if (!measured) return 0;
    HailstoneBinaryMeasurement value{};
    value.size = sizeof(value);
    value.kind = kind;
    value.byte_count = measured->byte_count;
    value.fnv1a63 = measured->fnv1a63;
    std::copy(measured->sha256.begin(), measured->sha256.end(), value.sha256);
    return give(out, value);
}

int h_get_map_policy(void* h, HailstoneMapPolicy* out) {
    const auto& l = Loaded::of(h);
    std::uint32_t size = 0;
    if (out) std::memcpy(&size, out, sizeof(size));
    if (!l.can(kCapRead) || l.map_policy_running || size < sizeof(size)) return 0;
    const auto* connection = connection_of(l);
    const auto policy = connection ? connection->server_ext().map_policy(*connection) : game::MapPolicy{};
    HailstoneMapPolicy value{};
    value.size = sizeof(value);
    value.flags = policy.flags;
    value.revision = policy.revision;
    value.context = connection ? connection->interaction_context() : 0;
    const auto length = std::min(policy.name.size(), sizeof(value.name) - 1);
    std::copy_n(policy.name.begin(), length, value.name);
    return give(out, value);
}

std::uint64_t h_monotonic_ms(void* h) {
    return Loaded::of(h).can(kCapRead) ? static_cast<std::uint64_t>(net::GameConnection::now_ms()) : 0;
}
int h_raid_action(void* h, const char* name, std::uint32_t action, std::int32_t group) {
    auto& l = Loaded::of(h);
    if (!l.active || !l.can(kCapRead | kCapActions) || !name) return 0;
    std::size_t length = 0; while (length < 64 && name[length]) ++length;
    if (!length || length == 64) return 0;
    const std::string n(name, length);
    auto* c = connection_of(l);
    if (!c || !c->in_zone() || c->player_status().dead || c->interface_locked()) return 0;
    const auto& raid = c->raid();
    const auto member = std::find_if(raid.members.begin(), raid.members.end(), [&](const auto& m) { return iequal(m.name, n); });
    if (!raid.active || !raid.locked || member == raid.members.end() || !iequal(raid.leader, c->character_name())) return 0;
    if (action != HAILSTONE_RAID_MOVE || group < -1 || group > 11) return 0;
    const std::int32_t target = group;
    if (target >= 0 && std::count_if(raid.members.begin(), raid.members.end(), [&](const auto& m) {
            return m.group == static_cast<std::uint32_t>(target) && !iequal(m.name, n);
        }) >= 6) return 0;
    c->raid_move_group(member->name, target);
    return 1;
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

std::uint32_t h_cursor_origin(void* h) {
    const net::GameConnection* c = readable(Loaded::of(h), "cursor_origin");
    return c ? c->cursor_origin() : HAILSTONE_CURSOR_ORIGIN_UNKNOWN;
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
    v.tics = static_cast<std::uint32_t>(b.tics); // the SDK's field is unsigned: a negative (permanent) count's bits
    v.level = b.level;
    v.caster_id = b.caster_id;
    return give(out, v);
}
/// <summary>
/// This is highly optimized code. AI should not modify this code method.
/// PR reviewwr ask the submiter for changes to be reviewed by the submitter
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer to put data in</param>
/// <param name="datamax">greater than 1798</param>
/// <returns></returns>
int h_get_buff_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    //AI SHOULD NOT MODIFY THIS METHOD
    //don't allow a deny just grab the raw connection.
    const net::GameConnection* c = readable(Loaded::of(h), "get_buff_data_bulk");
    if (!c || !data || datamax <= 0) return 0;
    const auto& buffs = c->spell_state().buffs;
    // The ticks left, as the character sheet counts them: nothing counts down while the timers are on hold (SPA 436).
    const bool timersHeld = c->spell_state().buff_timers_on_hold;
    const std::int64_t now = net::GameConnection::now_ms();
    int bufferSize = 0;
    unsigned char* pBuffer = data;
    //buffs are stored in buff slots
    //the long buffs, then the songs, then the one disc slot, always last (RoF2: 0-41, 42-61, 62)
    //the disc slot only goes out when the buffer has room for it: every buff and song slot is the minimum
    //precent C++ from padding the struct as we are blasting this into the array.
    static_assert(sizeof(HS_BulkBuffData) == HAILSTONE_BUFF_RECORD_SIZE);
    //needs to be able to fit the data, or return nothing
    constexpr int kNormalBuffSlots = game::kLongBuffSlots + game::kShortBuffSlots; // the disc slot is the one after
    constexpr int32_t kMinBuffBytes = kNormalBuffSlots * static_cast<int32_t>(sizeof(HS_Bulk_BuffData));
    if (datamax < kMinBuffBytes) return 0;

    const hailstone::game::SpellTable& st = c->spell_table();

    for (int slot = 0; slot < game::kBuffSlots; ++slot) {
        // SAFETY CHECK: Ensure writing this next packet won't exceed datamax
        if (bufferSize + sizeof(HS_Bulk_BuffData) > static_cast<std::size_t>(datamax)) {
            // Buffer is full (a 1798-byte buffer stops here before the disc slot). Break out to prevent an overflow.
            break;
        }
        const auto& buff = buffs[static_cast<std::size_t>(slot)];
        // Written in place through the packed struct (the bulk reads may cast, docs/plugins.md).
        HS_Bulk_BuffData* rec = reinterpret_cast<HS_Bulk_BuffData*>(pBuffer);
        *rec = HS_Bulk_BuffData{}; // an empty slot stays all zeros, its spell id too (the client keeps kNoSpell there)
        if (!buff.empty())
        {
            rec->ID = static_cast<int>(buff.spell);
            //get said spell for some data we need
            //blast the id into the buffer and move pointer forward
            rec->casterID = buff.caster_id;
            // On hold, the count as sent: received at now counts no ticks off it (a permanent one stays negative).
            rec->duration = game::buff_tics_left(buff.tics, timersHeld ? now : buff.received_ms, now);
            rec->hitcount = buff.num_hits;
            rec->counterType = -1; // -1 for active slots with no matching effect
            if (slot >= kNormalBuffSlots)
                rec->slotType = HAILSTONE_BUFF_SLOT_DISC;
            else if (slot >= game::kLongBuffSlots)
                rec->slotType = HAILSTONE_BUFF_SLOT_SONG;
            else
                rec->slotType = HAILSTONE_BUFF_SLOT_BUFF;
            if (const auto* spell = st.get(rec->ID)) {
                rec->spelltype = spell->spell_type;

                // Evaluate spell effects for counters
                for (std::size_t i = 0; i < spell->effect.size(); ++i) {
                    bool match = true;
                    switch (spell->effect[i]) {
                    case game::eEQSPA::SPA_DISEASE:    rec->counterType = 0; break;
                    case game::eEQSPA::SPA_POISON:     rec->counterType = 1; break;
                    case game::eEQSPA::SPA_CURSE:      rec->counterType = 2; break;
                    case game::eEQSPA::SPA_CORRUPTION: rec->counterType = 3; break;
                    default:                           match = false;       break;
                    }
                    if (match) { // the first counter effect only
                        rec->counterTotal = spell->base[i];
                        break;
                    }
                }
            }
        }
        pBuffer += sizeof(HS_Bulk_BuffData);
        bufferSize += sizeof(HS_Bulk_BuffData); // Success! Tracks total bytes perfectly

    }
    return bufferSize;
}//end h_get_buff_data_bulk

/// <summary>
/// get_buff_data_bulk with each caster's name in place of the caster's spawn id: one HS_Bulk_BuffNameFixedRecord per
/// buff slot, in the same order and with the same fields, each followed by its caster's name (casterNameLength bytes,
/// no NUL; none in an empty slot). Every buff and song slot with their names or nothing; the disc slot when its record
/// and name fit after them.
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer to put data in</param>
/// <param name="datamax">29 bytes per slot plus each caster's name: at least every buff and song slot's</param>
/// <returns>bytes written</returns>
int32_t h_get_buff_name_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    const net::GameConnection* c = readable(Loaded::of(h), "get_buff_name_data_bulk");
    if (!c || !data || datamax <= 0) return 0;
    const auto& buffs = c->spell_state().buffs;
    // The ticks left, as the character sheet counts them: nothing counts down while the timers are on hold (SPA 436).
    const bool timersHeld = c->spell_state().buff_timers_on_hold;
    const std::int64_t now = net::GameConnection::now_ms();
    int bufferSize = 0;
    unsigned char* pBuffer = data;
    //buffs are stored in buff slots
    //the long buffs, then the songs, then the one disc slot, always last (RoF2: 0-41, 42-61, 62)
    //the disc slot only goes out when the buffer has room for it: every buff and song slot is the minimum
    //precent C++ from padding the struct as we are blasting this into the array.
    static_assert(sizeof(HS_Bulk_BuffNameFixedRecord) == HAILSTONE_BUFF_NAME_RECORD_SIZE);
    //needs to be able to fit the data, or return nothing: every buff and song slot, each caster's name included
    constexpr int kNormalBuffSlots = game::kLongBuffSlots + game::kShortBuffSlots; // the disc slot is the one after
    std::size_t minBuffBytes = 0;
    for (int slot = 0; slot < kNormalBuffSlots; ++slot) {
        const auto& buff = buffs[static_cast<std::size_t>(slot)];
        minBuffBytes += sizeof(HS_Bulk_BuffNameFixedRecord) + (buff.empty() ? 0 : buff.caster_name.size());
    }
    if (minBuffBytes > static_cast<std::size_t>(datamax)) return 0;

    const hailstone::game::SpellTable& st = c->spell_table();

    for (int slot = 0; slot < game::kBuffSlots; ++slot) {
        const auto& buff = buffs[static_cast<std::size_t>(slot)];
        // The caster's name follows the record (none in an empty slot); the record is not written without it.
        const std::size_t nameBytes = buff.empty() ? 0 : buff.caster_name.size();
        // SAFETY CHECK: Ensure writing this next packet won't exceed datamax
        if (static_cast<std::size_t>(bufferSize) + sizeof(HS_Bulk_BuffNameFixedRecord) + nameBytes >
            static_cast<std::size_t>(datamax)) {
            // Buffer is full (one with room for only the buff and song slots stops here before the disc slot).
            // Break out to prevent an overflow.
            break;
        }
        // Written in place through the packed struct (the bulk reads may cast, docs/plugins.md).
        HS_Bulk_BuffNameFixedRecord* rec = reinterpret_cast<HS_Bulk_BuffNameFixedRecord*>(pBuffer);
        *rec = HS_Bulk_BuffNameFixedRecord{}; // an empty slot stays all zeros (the client keeps kNoSpell there)
        if (!buff.empty())
        {
            rec->ID = static_cast<int>(buff.spell);
            //get said spell for some data we need
            //blast the id into the buffer and move pointer forward
            rec->casterNameLength = static_cast<int32_t>(nameBytes);
            // On hold, the count as sent: received at now counts no ticks off it (a permanent one stays negative).
            rec->duration = game::buff_tics_left(buff.tics, timersHeld ? now : buff.received_ms, now);
            rec->hitcount = buff.num_hits;
            rec->counterType = -1; // -1 for active slots with no matching effect
            if (slot >= kNormalBuffSlots)
                rec->slotType = HAILSTONE_BUFF_SLOT_DISC;
            else if (slot >= game::kLongBuffSlots)
                rec->slotType = HAILSTONE_BUFF_SLOT_SONG;
            else
                rec->slotType = HAILSTONE_BUFF_SLOT_BUFF;
            if (const auto* spell = st.get(rec->ID)) {
                rec->spelltype = spell->spell_type;

                // Evaluate spell effects for counters
                for (std::size_t i = 0; i < spell->effect.size(); ++i) {
                    bool match = true;
                    switch (spell->effect[i]) {
                    case game::eEQSPA::SPA_DISEASE:    rec->counterType = 0; break;
                    case game::eEQSPA::SPA_POISON:     rec->counterType = 1; break;
                    case game::eEQSPA::SPA_CURSE:      rec->counterType = 2; break;
                    case game::eEQSPA::SPA_CORRUPTION: rec->counterType = 3; break;
                    default:                           match = false;       break;
                    }
                    if (match) { // the first counter effect only
                        rec->counterTotal = spell->base[i];
                        break;
                    }
                }
            }
        }
        pBuffer += sizeof(HS_Bulk_BuffNameFixedRecord);
        if (nameBytes) std::memcpy(pBuffer, buff.caster_name.data(), nameBytes);
        pBuffer += nameBytes;
        bufferSize += static_cast<int>(sizeof(HS_Bulk_BuffNameFixedRecord) + nameBytes);

    }
    return static_cast<int32_t>(bufferSize);
}//end h_get_buff_name_data_bulk

/// <summary>
/// This is highly optimized code. AI should not modify this code method.
/// PR reviewwr ask the submiter for changes to be reviewed by the submitter
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer passed in</param>
/// <param name="datamax">12 bytes per buff, 30 so minimum of 360, so 1024 is good </param>
/// <returns></returns>
int32_t h_get_pet_buff_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    //AI SHOULD NOT MODIFY THIS METHOD
    const net::GameConnection* c = readable(Loaded::of(h), "get_pet_buff_data_bulk");
    if (!c || !data || datamax <= 0) return 0;
    unsigned char* pBuffer = data;
    int bufferSize = 0;
 
    static_assert(sizeof(HS_BulkPetBuffData) == HAILSTONE_PET_BUFF_RECORD_SIZE);
    const size_t bytesPerBuff = sizeof(HS_BulkPetBuffData);
    const game::SpellTable& st = c->spell_table();
    const std::int64_t now = net::GameConnection::now_ms(); // for the ticks left
    for (const auto& [slot, b] : c->pet_buffs()) {

        if (bufferSize + bytesPerBuff > static_cast<size_t>(datamax)) {
            // Buffer is full. Handle as needed (e.g., break or return error)
            break;
        }
        // Written in place through the packed struct (the bulk reads may cast, docs/plugins.md).
        HS_BulkPetBuffData* rec = reinterpret_cast<HS_BulkPetBuffData*>(pBuffer);
        *rec = HS_BulkPetBuffData{}; // an entry with no spell stays all zeros
        if (b.spell > 0)
        {
            rec->id = b.spell;
            rec->duration = game::buff_tics_left(b.tics, b.received_ms, now);
            const auto* sp = st.get(b.spell);
            if (sp)
            {
                rec->spellType = sp->spell_type;
            }
        }
        pBuffer += bytesPerBuff;
        bufferSize += bytesPerBuff;
    }
    return bufferSize;
}
/// <summary>
/// The current target's buffs in one call, as OP_TargetBuffs last gave them, in the order the client keeps them, each
/// with the slot the server sent: an HS_Bulk_ListHeader (count, total), then one HS_Bulk_TargetBuffFixedRecord per
/// buff followed by its caster's name, casterNameLength bytes with no NUL. An entry with no spell is neither counted
/// nor written. Only whole records go out, as many as fit after the header; count < total means the buffer cut the
/// list short. Nothing (0 bytes) when not even the header fits. The header alone (0, 0) without a target, when the
/// buffs the client holds are another spawn's (the server has not sent the new target's yet), or with no game
/// connection.
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer to put data in</param>
/// <param name="datamax">8 bytes for the HS_Bulk_ListHeader plus 20 per buff and its caster's name; the worst case
/// (EQEmu: 60 buff slots on an NPC by default, Spells:MaxTotalSlotsNPC; a caster's name up to 63 bytes, Buffs_Struct)
/// is 8 + 60 * 83 = 4988, so 8192 holds any on a stock server</param>
/// <returns>bytes written; 0 below 8 bytes</returns>
int32_t h_get_target_buff_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    Loaded& l = Loaded::of(h);
    const net::GameConnection* c = readable(l, "get_target_buff_data_bulk");
    if (!c && !l.can(kCapRead)) return 0;
    static_assert(sizeof(HS_BulkListHeader) == HS_BULK_LIST_HEADER_SIZE);
    const std::size_t headerBytes = sizeof(HS_BulkListHeader);
    if (!data || datamax <= 0 || static_cast<std::size_t>(datamax) < headerBytes) return 0;
    HS_BulkListHeader header{};
    std::size_t bufferSize = headerBytes;
    const std::uint32_t target = c ? c->world().target_id : 0;
    // The same check as the target window (lua_ui_hud.cpp, spell_ui.cpp): the list is only the target's while
    // target_buffs_entity says so.
    if (c && target != 0 && c->spell_state().target_buffs_entity == target) {
        const auto& st = c->spell_state();
        unsigned char* pBuffer = data + headerBytes;
        static_assert(sizeof(HS_BulkTargetBuffFixedRecord) == HAILSTONE_TARGET_BUFF_RECORD_SIZE);
        const std::size_t fixedBytes = sizeof(HS_BulkTargetBuffFixedRecord);
        const game::SpellTable& spells = c->spell_table();
        const std::int64_t now = net::GameConnection::now_ms(); // for the ticks left
        // A held countdown (game::buff_countdown_held) gives the count as sent, as the target window shows it: the
        // zone's buff suspension holds anyone's suspendable buffs, the player's hold only its own list.
        const bool own_hold = st.buff_timers_on_hold && c->target_buffs_are_own();
        bool full = false;
        for (const auto& [slot, b] : st.target_buffs) {
            // The decoder drops removed entries (0xffffffff) and EQEmu sends only valid spells, so an entry with no
            // spell is not expected; skip it, as spawn_buffs (tlo_game.cpp) and the target window do.
            if (b.empty()) continue;
            ++header.total; // every buff held, written or not: count < total means the buffer was too small
            if (full) continue;
            const std::size_t nameBytes = b.caster_name.size();
            // Buffer is full: only whole records go out (the name is part of the record), none after the first cut.
            if (fixedBytes + nameBytes > static_cast<std::size_t>(datamax) - bufferSize) {
                full = true;
                continue;
            }
            // Written in place through the packed struct (the bulk reads may cast, docs/plugins.md).
            HS_BulkTargetBuffFixedRecord* rec = reinterpret_cast<HS_BulkTargetBuffFixedRecord*>(pBuffer);
            *rec = HS_BulkTargetBuffFixedRecord{};
            rec->spellID = static_cast<int32_t>(b.spell);
            const auto* sp = spells.get(b.spell);
            const bool held = game::buff_countdown_held(sp, own_hold, st.zone_suspends_buffs);
            rec->duration = game::buff_tics_left(b.tics, held ? now : b.received_ms, now);
            if (sp) rec->spellType = sp->spell_type;
            rec->slot = static_cast<int32_t>(slot);
            rec->casterNameLength = static_cast<int32_t>(nameBytes);
            pBuffer += fixedBytes;
            if (nameBytes) std::memcpy(pBuffer, b.caster_name.data(), nameBytes);
            pBuffer += nameBytes;
            bufferSize += fixedBytes + nameBytes;
            ++header.count;
        }
    }
    std::memcpy(data, &header, sizeof header); // in the machine's byte order, unaligned, as the records
    return static_cast<int32_t>(bufferSize);
}
/// <summary>
/// This is highly optimized code. AI should not modify this code method.
/// PR reviewwr ask the submiter for changes to be reviewed by the submitter
/// Every extended target slot in one call, slot 0 first: its type, spawn id, the aggro meter's percent and the
/// spawn's HP percent, as HS_Bulk_XTargetData records. Only whole records go out; as many slots as fit.
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer to put data in</param>
/// <param name="datamax">16 bytes per slot; RoF2's 20 slots (EQEmu: 5 plus the AA's) fit in 320, so 512 is good</param>
/// <returns>bytes written</returns>
int32_t h_get_xtarget_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    //AI SHOULD NOT MODIFY THIS METHOD
    const net::GameConnection* c = readable(Loaded::of(h), "get_xtarget_data_bulk");
    if (!c || !data || datamax <= 0) return 0;
    unsigned char* pBuffer = data;
    int32_t bufferSize = 0;

    static_assert(sizeof(HS_BulkXTargetData) == HS_BULK_XTARGET_RECORD_SIZE);
    // A type added to one list (the SDK's HS_BULK_XTargetTypes, game::XTargetType) and not the other.
    static_assert(static_cast<std::uint32_t>(HS_BULK_XTARGET_TYPE_COUNT) == game::kXTargetTypes);
    const size_t bytesPerSlot = sizeof(HS_BulkXTargetData);
    const auto& world = c->world();
    const auto& slots = c->xtargets().slots;
    const auto& aggro = c->aggro().pct;
    for (std::size_t n = 0; n < slots.size(); ++n) {
        if (static_cast<size_t>(bufferSize) + bytesPerSlot > static_cast<size_t>(datamax)) {
            // Buffer is full: only whole records go out.
            break;
        }
        const game::XTargetSlot& x = slots[n];
        // Written in place through the packed struct (the bulk reads may cast, docs/plugins.md).
        HS_BulkXTargetData* rec = reinterpret_cast<HS_BulkXTargetData*>(pBuffer);
        *rec = HS_BulkXTargetData{}; // no spawn in the zone: aggro and HP stay 0
        rec->targetType = static_cast<int32_t>(x.type);
        rec->id = static_cast<int32_t>(x.spawn_id);
        auto it = x.spawn_id ? world.spawns.find(x.spawn_id) : world.spawns.end();
        const bool in_zone = it != world.spawns.end();
        // The aggro meter's entries 7..26 are the extended target slots (net/aggro_packets.h). Only with the slot's
        // spawn in the zone, as the window shows its ETW_AggroPct label: EQEmu (Client::ProcessAggroMeter) updates
        // an entry only while its slot holds a mob it finds and never zeroes it when the slot empties, so a slot
        // emptied since would keep its last mob's percent. A slot past the 20th, which only a server with more
        // has, has no entry.
        const std::size_t entry = static_cast<std::size_t>(net::kAggroXTarget1) + n;
        if (in_zone && entry < aggro.size()) rec->aggroPct = aggro[entry];
        // The HP percent as the extended target window's gauges take it (lua_ui_values.cpp spawn_hp_percent):
        // the player's own points (items' HP included), anyone else's percent from the server; 0 for a corpse.
        // EQEmu sends a mob's OP_MobHealth to every client holding it on its list (Mob::SendHPUpdate,
        // QueueClientsByXTarget), not only to those targeting it, so the spawn's percent is kept current.
        if (in_zone && !it->second.is_corpse) {
            if (it->second.id == world.player_id) {
                const auto hp = c->own_hp();
                rec->pctHPs = hp.max > 0 ? static_cast<int32_t>(std::max<std::int64_t>(0, hp.cur * 100 / hp.max)) : 0;
            } else {
                rec->pctHPs = it->second.hp_percent;
            }
        }
        pBuffer += bytesPerSlot;
        bufferSize += static_cast<int32_t>(bytesPerSlot);
    }
    return bufferSize;
}

// A list of named ids for the bulk reads: an HS_Bulk_ListHeader, then for each id that `each` gives (each(emit)
// calls emit(id, name) for every one, in order) an HAILSTONE_ID_NAME_RECORD followed by the name's nameSize bytes (no
// NUL), as many whole records as fit after the header, none after the first that does not. The header's count says
// how many follow, its total how many `each` gave. Nothing (0 bytes) when not even the header fits.
template <typename Each> int32_t write_id_str_list(unsigned char* data, int32_t datamax, Each&& each) {
    static_assert(sizeof(HAILSTONE_ID_NAME_RECORD) == HAILSTONE_ID_NAME_RECORD_SIZE);
    static_assert(sizeof(HS_BulkListHeader) == HS_BULK_LIST_HEADER_SIZE);
    const std::size_t headerBytes = sizeof(HS_BulkListHeader);
    const std::size_t idBytes = sizeof(HAILSTONE_ID_NAME_RECORD);
    if (!data || datamax <= 0 || static_cast<std::size_t>(datamax) < headerBytes) return 0;
    const std::size_t room = static_cast<std::size_t>(datamax);
    std::size_t bufferSize = headerBytes;
    HS_BulkListHeader header{};
    bool full = false;
    each([&](int32_t id, std::string_view name) {
        ++header.total; // every one given, written or not: count < total means the buffer was too small
        if (full) return;
        if (idBytes + name.size() > room - bufferSize) { // buffer is full: only whole records, the rest left out
            full = true;
            return;
        }
        // Written in place through the packed struct (the bulk reads may cast, docs/plugins.md).
        HAILSTONE_ID_NAME_RECORD* rec = reinterpret_cast<HAILSTONE_ID_NAME_RECORD*>(data + bufferSize);
        rec->id = id;
        rec->nameSize = static_cast<int32_t>(name.size());
        bufferSize += idBytes;
        if (!name.empty()) std::memcpy(data + bufferSize, name.data(), name.size());
        bufferSize += name.size();
        ++header.count;
    });
    std::memcpy(data, &header, sizeof header); // in the machine's byte order, unaligned, as the records
    return static_cast<int32_t>(bufferSize);
}

/// <summary>
/// The alternate abilities the character owns, one ability (group) id each, as aa_value takes it, with its name (the
/// rank's dbstr type 1 title, as the AA window shows it; empty when the game folder's dbstr_us.txt lacks it): an
/// HS_Bulk_ListHeader then one HAILSTONE_ID_NAME_RECORD and its name per ability, in the order of the owned list
/// (the profile's, then OP_RespondAA's). An owned rank the AA table does not hold (OP_SendAATable has not sent it) has
/// no known group id and is left out, as aa_value finds nothing for it. Before the profile, or with no game
/// connection, count and total are 0.
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer to put data in</param>
/// <param name="datamax">8 bytes for the HS_Bulk_ListHeader plus 8 per ability and its name; 16384 holds RoF2's 300 owned</param>
/// <returns>bytes written; 0 below 8 bytes</returns>
int32_t h_get_avail_aa_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    Loaded& l = Loaded::of(h);
    const net::GameConnection* c = readable(l, "get_avail_aa_data_bulk");
    if (!c) return l.can(kCapRead) ? write_id_str_list(data, datamax, [](auto&&) {}) : 0; // no connection: 0, 0
    const auto& aa = c->alt_adv();
    const bool haveProfile = c->profile().has_value();
    return write_id_str_list(data, datamax, [&](auto&& emit) {
        if (!haveProfile) return; // before the profile: count and total 0
        for (const auto& o : aa.owned) {
            if (o.id == 0) continue; // an empty slot
            if (const game::AltAbility* a = aa.get(o.id)) emit(a->group, c->aa_name(*a));
        }
    });
}

/// <summary>
/// The character's combat abilities (disciplines), one spell id each with the spell's name (empty for a spell not in
/// spells_us.txt): the profile's discipline list (PcBase::GetCombatAbility, as combat_ability reads it by index)
/// without its empty entries, in list order, as an HS_Bulk_ListHeader then one HAILSTONE_ID_NAME_RECORD and its name
/// per discipline. Before the profile, or with no game connection, count and total are 0.
/// </summary>
/// <param name="h">host</param>
/// <param name="data">buffer to put data in</param>
/// <param name="datamax">8 bytes for the HS_Bulk_ListHeader plus 8 per discipline and its name; 16384 holds RoF2's 300</param>
/// <returns>bytes written; 0 below 8 bytes</returns>
int32_t h_get_avail_disc_data_bulk(void* h, unsigned char* data, int32_t datamax)
{
    Loaded& l = Loaded::of(h);
    const net::GameConnection* c = readable(l, "get_avail_disc_data_bulk");
    if (!c) return l.can(kCapRead) ? write_id_str_list(data, datamax, [](auto&&) {}) : 0; // no connection: 0, 0
    const auto& profile = c->profile();
    const game::SpellTable& spells = c->spell_table();
    return write_id_str_list(data, datamax, [&](auto&& emit) {
        if (!profile) return; // before the profile: count and total 0
        for (std::uint32_t spell : profile->disciplines) {
            if (spell == 0) continue; // an empty entry
            const game::SpellData* sp = spells.get(spell);
            emit(static_cast<int32_t>(spell), sp ? std::string_view(sp->name) : std::string_view());
        }
    });
}

int32_t h_get_raid_data_bulk(void* h, unsigned char* data, int32_t datamax) {
    const net::GameConnection* c = readable(Loaded::of(h), "get_raid_data_bulk");
    if (!c || !data || datamax < static_cast<int32_t>(sizeof(HS_BulkRaidHeader))) return 0;

    static_assert(sizeof(HS_BulkRaidHeader) == HS_BULK_RAID_HEADER_SIZE);
    static_assert(sizeof(HS_BulkRaidData) == HS_BULK_RAID_RECORD_SIZE);
    const plugin::RaidSnapshot raid = plugin::build_raid_snapshot(*c);

    HS_BulkRaidHeader header{};
    header.size = static_cast<std::uint32_t>(sizeof header);
    header.version = 1;
    header.recordSize = static_cast<std::uint32_t>(sizeof(HS_BulkRaidData));
    header.leaderIndex = raid.leader ? static_cast<std::uint32_t>(*raid.leader) : HS_BULK_RAID_NO_INDEX;
    header.targetIndex = raid.target ? static_cast<std::uint32_t>(*raid.target) : HS_BULK_RAID_NO_INDEX;
    header.active = raid.active ? 1 : 0;
    header.locked = raid.locked ? 1 : 0;
    header.lootType = static_cast<int32_t>(raid.loot_type);
    header.total = static_cast<std::uint32_t>(raid.members.size()); // count < total: the buffer was too small
    header.count = 0;

    int32_t written = static_cast<int32_t>(sizeof header);
    unsigned char* pBuffer = data + sizeof header;
    for (const plugin::RaidMemberSnapshot& m : raid.members) {
        if (static_cast<int32_t>(sizeof(HS_BulkRaidData)) > datamax - written) break; // whole records only
        HS_BulkRaidData* rec = reinterpret_cast<HS_BulkRaidData*>(pBuffer);
        *rec = HS_BulkRaidData{};
        rec->slot = static_cast<int32_t>(header.count);
        rec->spawnID = m.spawn_id;
        rec->group = m.group;
        rec->flags = m.flags;
        rec->level = m.level;
        rec->classID = m.class_id;
        rec->pctHPs = m.pct_hp;
        rec->pctMana = m.pct_mana;
        rec->pctEndurance = m.pct_endurance;
        rec->petSpawnID = m.pet_spawn_id;
        rec->petPctHPs = m.pet_pct_hp;
        rec->x = m.x;
        rec->y = m.y;
        rec->z = m.z;
        rec->distance = m.distance;
        copy_name(rec->name, m.name);
        pBuffer += sizeof(HS_BulkRaidData);
        written += static_cast<int32_t>(sizeof(HS_BulkRaidData));
        ++header.count;
    }
    std::memcpy(data, &header, sizeof header);
    return written;
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

// HS_ABILITY_* (the SDK's) as the app's mq::AbilityKind; nullopt for a value this client does not know.
std::optional<mq::AbilityKind> ability_kind(int kind) {
    switch (kind) {
    case HS_ABILITY_SKILL: return mq::AbilityKind::Skill;
    case HS_ABILITY_DISCIPLINE: return mq::AbilityKind::Discipline;
    case HS_ABILITY_ALT: return mq::AbilityKind::AltAbility;
    default: return std::nullopt;
    }
}

int h_use_ability(void* h, int kind, const char* name) {
    Loaded& l = Loaded::of(h);
    if (!acting(l, "use_ability")) return 0;
    const auto k = ability_kind(kind);
    if (!k || !name || !*name || !l.host->env.use_ability) return 0;
    return l.host->env.use_ability(*k, name) ? 1 : 0;
}

int h_ability_state(void* h, int kind, const char* name, std::int64_t* remaining_ms, std::int64_t* reuse_ms) {
    Loaded& l = Loaded::of(h);
    if (!readable(l, "ability_state")) return -1;
    const auto k = ability_kind(kind);
    if (!k || !name || !*name || !l.host->env.ability) return -1;
    const std::optional<mq::AbilityState> st = l.host->env.ability(*k, name);
    if (!st) return -1;
    if (remaining_ms) *remaining_ms = st->remaining_ms;
    if (reuse_ms) *reuse_ms = st->reuse_ms;
    return st->ready ? 1 : 0;
}

// HS_ABILITY_* for a row's mq::AbilityKind (the two enums order their values differently).
int sdk_ability_kind(mq::AbilityKind kind) {
    switch (kind) {
    case mq::AbilityKind::Skill: return HS_ABILITY_SKILL;
    case mq::AbilityKind::Discipline: return HS_ABILITY_DISCIPLINE;
    case mq::AbilityKind::AltAbility: return HS_ABILITY_ALT;
    }
    return HS_ABILITY_SKILL;
}

int32_t h_get_ability_data_bulk(void* h, unsigned char* out, int32_t out_size) {
    Loaded& l = Loaded::of(h);
    // Without a connection (the login screen) the header alone, count and total 0, as the AA and discipline reads; 0
    // bytes only without 'read'.
    const bool connected = readable(l, "get_ability_data_bulk") != nullptr;
    if (!connected && !l.can(kCapRead)) return 0;
    static_assert(sizeof(HS_BulkListHeader) == HS_BULK_LIST_HEADER_SIZE);
    static_assert(sizeof(HS_Bulk_AbilityRecord) == HAILSTONE_ABILITY_RECORD_SIZE);
    static_assert(offsetof(HS_Bulk_AbilityRecord, kind) == 0);
    static_assert(offsetof(HS_Bulk_AbilityRecord, id) == 4);
    static_assert(offsetof(HS_Bulk_AbilityRecord, ready) == 8);
    static_assert(offsetof(HS_Bulk_AbilityRecord, remainingMs) == 12);
    static_assert(offsetof(HS_Bulk_AbilityRecord, reuseMs) == 20);
    static_assert(offsetof(HS_Bulk_AbilityRecord, nameSize) == 28);
    if (!out || out_size < static_cast<int32_t>(sizeof(HS_BulkListHeader))) return 0;
    const std::vector<AbilityBulkRow> rows =
        connected && l.host->env.ability_bulk ? l.host->env.ability_bulk() : std::vector<AbilityBulkRow>{};
    auto* header = reinterpret_cast<HS_BulkListHeader*>(out);
    header->count = 0;
    header->total = static_cast<int32_t>(rows.size());
    int32_t at = static_cast<int32_t>(sizeof(HS_BulkListHeader));
    for (const AbilityBulkRow& row : rows) {
        const auto name_size = static_cast<int32_t>(row.name.size());
        if (name_size > out_size - at - static_cast<int32_t>(sizeof(HS_Bulk_AbilityRecord))) break;
        auto* rec = reinterpret_cast<HS_Bulk_AbilityRecord*>(out + at);
        rec->kind = sdk_ability_kind(row.kind);
        rec->id = row.id;
        rec->ready = row.ready ? 1 : 0;
        rec->remainingMs = row.remaining_ms;
        rec->reuseMs = row.reuse_ms;
        rec->nameSize = name_size;
        at += static_cast<int32_t>(sizeof(HS_Bulk_AbilityRecord));
        if (name_size > 0) std::memcpy(out + at, row.name.data(), static_cast<std::size_t>(name_size));
        at += name_size;
        ++header->count;
    }
    return at;
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
// A host function that threw (out of memory, a provider's exception): the log says so once per plugin.
void host_threw(void* h) noexcept {
    if (!h) return;
    Loaded& l = Loaded::of(h);
    if (!l.host_threw_warned) {
        l.host_threw_warned = true;
        try {
            log::warn("plugin {}: a host function failed with an error and returned nothing", l.name);
        } catch (...) {
        }
    }
}
template <auto F> struct MainOnly;
template <typename R, typename... A, R (*F)(void*, A...)> struct MainOnly<F> {
    static R call(void* h, A... a) {
        try {
            if (!on_main_thread()) {
                off_thread(h);
                if constexpr (std::is_void_v<R>) return;
                else return R{};
            }
            return F(h, a...);
        } catch (...) {
            // Host allocations/provider callbacks must never unwind through a plugin's C or foreign-runtime frames.
            // The call fails as an unanswered one does; the log says so, once per plugin.
            host_threw(h);
            if constexpr (std::is_void_v<R>) return;
            else return R{};
        }
    }
};
template <auto F> constexpr auto main_only = &MainOnly<F>::call;
// The same, for a host function whose 0 means something: off the client's thread it returns `off` instead.
template <auto F, auto Off> struct MainOnlyOr;
template <typename R, typename... A, R (*F)(void*, A...), auto Off> struct MainOnlyOr<F, Off> {
    static R call(void* h, A... a) {
        if (!on_main_thread()) {
            off_thread(h);
            return static_cast<R>(Off);
        }
        return F(h, a...);
    }
};
template <auto F, auto Off> constexpr auto main_only_or = &MainOnlyOr<F, Off>::call;

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

std::optional<std::string> NativeExtension::item_flags(const net::GameConnection&, const net::Item& item,
                                                       std::string_view stock) const {
    if (!l_ || !h().item_flags) return std::nullopt;
    const HailstoneItem value = item_struct(item);
    const std::string text(stock);
    const char* result = run(h().item_flags, &value, text.c_str());
    return result ? std::optional<std::string>(result) : std::nullopt;
}

std::optional<std::string> NativeExtension::item_description_append(const net::GameConnection&,
                                                                   const net::Item& item) const {
    if (!l_ || !h().item_description_append) return std::nullopt;
    const HailstoneItem value = item_struct(item);
    const char* result = run(h().item_description_append, &value);
    return result ? std::optional<std::string>(result) : std::nullopt;
}

std::optional<std::string> NativeExtension::loot_confirmation(const net::GameConnection&, const net::Item& item,
                                                            std::span<const std::uint32_t> descendants,
                                                            bool complete) const {
    if (!l_ || !h().loot_confirmation) return std::nullopt;
    const HailstoneItem value = item_struct(item);
    const char* result = run(h().loot_confirmation, &value, descendants.data(), descendants.size(), complete ? 1 : 0);
    return result ? std::optional<std::string>(result) : std::nullopt;
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
        for (const auto& n : pending) m->self->unload_file(n);
    });
}

NativePlugins::~NativePlugins() {
    unload_all();
    if (impl_->env.hooks) impl_->env.hooks->remove(impl_->owner);
}

std::vector<NativePlugins::Info> NativePlugins::list() const {
    std::vector<Info> out;
    for (const auto& file : plugin_files(impl_->dirs())) {
        Info i;
        i.name = plugin_name(file);
        // A name found again is shadowed by the first (the server's folder comes first): load would not pick it.
        if (std::any_of(out.begin(), out.end(), [&](const Info& o) { return iequal(o.name, i.name); })) continue;
        i.file = file;
        if (auto bytes = read_file(file)) {
            Sha256Digest h = sha256(*bytes);
            i.sha256 = to_hex(h);
            i.caps = impl_->env.policy ? impl_->env.policy->caps(FileKind::Plugin, h, i.name) : 0;
            i.server = impl_->env.policy && impl_->env.policy->server_file(FileKind::Plugin, h);
            i.kept = impl_->kept(h);
            i.asks = impl_->asks(file, h);
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

bool NativePlugins::world_wanted() const {
    const auto& m = *impl_;
    return m.world_geometry.active() || !m.world_owners.empty() ||
           std::any_of(m.loaded.begin(), m.loaded.end(), [](const auto& l) { return l->hooks.world_frame && world_allowed(*l); });
}

void NativePlugins::world_frame(HailstoneWorldFrame frame, WorldQueries queries) {
    auto& m = *impl_;
    if (m.world_geometry.active()) world_unavailable();
    m.world_owners.clear();
    m.world_geometry.begin(frame, std::move(queries));
    if (!m.world_geometry.active()) return;
    std::vector<Hooks::Owner> candidates;
    for (const auto& l : m.loaded)
        if (world_allowed(*l) && l->hooks.world_frame) candidates.push_back(l->owner);
    for (const auto owner : candidates) {
        const auto it = std::find_if(m.loaded.begin(), m.loaded.end(), [&](const auto& l) { return l->owner == owner; });
        if (it == m.loaded.end() || !world_allowed(**it)) continue;
        auto* l = it->get();
        const auto context = m.world_geometry.frame();
        l->world_ready = true;
        const bool ok = guarded(*l, "world_frame", [&] {
            l->hooks.world_frame(&context);
            return true;
        });
        if (ok && world_allowed(*l)) m.world_owners.push_back(owner);
        else {
            l->world_ready = false;
            m.world_geometry.retire(owner);
        }
    }
    m.world_geometry.seal();
}

const std::vector<OwnedWorldBatch>& NativePlugins::world_batches() const {
    return impl_->world_geometry.batches();
}

void NativePlugins::world_recorded(std::size_t batch, std::uint32_t vertices) {
    impl_->world_geometry.recorded(batch, vertices);
}

void NativePlugins::world_finish(bool submitted, bool presented) {
    auto& m = *impl_;
    if (!m.world_geometry.active()) return;
    const auto frame = m.world_geometry.frame();
    auto results = m.world_geometry.finish(submitted, presented);
    const auto owners = std::move(m.world_owners);
    m.world_owners.clear();
    if (!submitted || !presented) m.world_geometry.cancel();
    // Dispatch copied receipts: a callback may retire another owner without
    // invalidating this walk or giving that owner a stale completion later.
    for (const auto& result : results) {
        const auto it = std::find_if(m.loaded.begin(), m.loaded.end(), [&](const auto& l) {
            return l->owner == result.owner;
        });
        if (it == m.loaded.end() || !world_allowed(**it) || !(**it).hooks.world_frame_result) continue;
        auto& l = **it;
        guarded(l, "world_frame_result", [&] { l.hooks.world_frame_result(&result.receipt); });
    }
    for (const auto owner : owners) {
        const auto it = std::find_if(m.loaded.begin(), m.loaded.end(), [&](const auto& l) { return l->owner == owner; });
        if (it == m.loaded.end()) continue;
        auto& l = **it;
        l.world_ready = submitted && presented && world_allowed(l);
        if (!world_allowed(l) || !l.hooks.world_frame_result) continue;
        HailstoneWorldReceipt receipt{};
        receipt.size = sizeof(receipt);
        receipt.status = HAILSTONE_WORLD_FRAME_FINISHED;
        receipt.frame = frame.frame;
        receipt.epoch = frame.epoch;
        if (submitted) receipt.flags |= HAILSTONE_WORLD_SUBMITTED;
        if (submitted && presented) receipt.flags |= HAILSTONE_WORLD_PRESENTED;
        guarded(l, "world_frame_result", [&] { l.hooks.world_frame_result(&receipt); });
    }
}

void NativePlugins::world_unavailable() {
    auto& m = *impl_;
    const auto frame = m.world_geometry.frame();
    std::vector<Hooks::Owner> owners;
    for (auto& l : m.loaded) {
        if (l->world_ready || std::find(m.world_owners.begin(), m.world_owners.end(), l->owner) != m.world_owners.end())
            owners.push_back(l->owner);
        l->world_ready = false;
    }
    m.world_geometry.cancel();
    m.world_owners.clear();
    for (const auto owner : owners) {
        const auto it = std::find_if(m.loaded.begin(), m.loaded.end(), [&](const auto& l) { return l->owner == owner; });
        if (it == m.loaded.end() || !world_allowed(**it) || !(**it).hooks.world_frame_result) continue;
        auto& l = **it;
        HailstoneWorldReceipt receipt{};
        receipt.size = sizeof(receipt);
        receipt.status = HAILSTONE_WORLD_FRAME_UNAVAILABLE;
        receipt.frame = frame.frame;
        receipt.epoch = frame.epoch;
        guarded(l, "world_frame_result", [&] { l.hooks.world_frame_result(&receipt); });
    }
}

bool NativePlugins::loaded(std::string_view name) const { return impl_->find(name) != nullptr; }

bool NativePlugins::has_plugin(std::string_view name) const {
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
    l->unload_pending = true; l->active = false;
    m.world_geometry.retire(l->owner);
    std::erase(m.world_owners, l->owner);
    if (shutdown && l->shutdown) guarded(*l, "ShutdownPlugin", [&] { l->shutdown(); });
    m.env.hooks->remove(l->owner);
    if (m.env.web) m.env.web->close_owner(web_owner(*l));
    if (m.env.ui) m.env.ui->remove(l->name);
    if (l->extension) {
        l->extension->detach();
        if (m.env.extensions) m.env.extensions->remove(l->chain_id);
    }
    game::clear_class_name_overrides(l->class_names_owner);
    for (const auto& t : std::vector<std::string>(l->tlos)) h_remove_tlo(l.get(), t.c_str());
    SDL_UnloadObject(l->lib);
}
} // namespace

bool NativePlugins::load(std::string_view name, std::string* why) {
    return load_file(name, why, caller_name().empty() ? Asker::Player : Asker::Script);
}

bool NativePlugins::load_file(std::string_view name_in, std::string* why, Asker asker) {
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
    std::uint32_t caps = m.env.policy->caps(FileKind::Plugin, hash, name);
    if (caps == 0) return refuse(std::format("{} may not load: {}", name, m.env.policy->refusal(FileKind::Plugin)));
    // The server's own library is native code with the player's rights: it loads only once the player allowed these
    // bytes for this server. Until then it waits for the answer (the game goes on); a refused one is asked about
    // again only when the player loads it. A script or plugin never puts the question up: it could, at will.
    if (m.asks(*file, hash)) {
        const Consent c = m.consent_for(hash);
        if (c != Consent::Allowed) {
            if (asker == Asker::Script)
                return refuse(name + " is not loaded: this server's compiled plugins load only once you allow them, "
                              "and only you can ask for that");
            if (c == Consent::Unasked || asker == Asker::Player) {
                const bool fresh = m.ask({name, *file, bytes->size(), hash, !m.in_server_dir(*file)});
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
    std::uint32_t got = 0;
    if (!guarded(p, "hailstone_plugin_abi", [&] { got = abi(); return true; })) {
        SDL_UnloadObject(p.lib);
        return refuse(name + " failed while reporting its SDK ABI");
    }
    if (got < 1 || got > HAILSTONE_PLUGIN_ABI) {
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
    p.table.get_buff_data_bulk = main_only<h_get_buff_data_bulk>;
    p.table.get_pet_buff_data_bulk = main_only<h_get_pet_buff_data_bulk>;
    p.table.get_spawns_data_bulk = main_only<h_get_spawns_data_bulk>;
    p.table.get_xtarget_data_bulk = main_only<h_get_xtarget_data_bulk>;
    p.table.get_target_buff_data_bulk = main_only<h_get_target_buff_data_bulk>;
    p.table.get_avail_aa_data_bulk = main_only<h_get_avail_aa_data_bulk>;
    p.table.get_avail_disc_data_bulk = main_only<h_get_avail_disc_data_bulk>;
    p.table.get_buff_name_data_bulk = main_only<h_get_buff_name_data_bulk>;
    p.table.get_raid_data_bulk = main_only<h_get_raid_data_bulk>;
    p.table.use_ability = main_only<h_use_ability>;
    p.table.ability_state = main_only_or<h_ability_state, -1>; // off the thread "unknown", not 0 ("on reuse")
    p.table.get_ability_data_bulk = main_only<h_get_ability_data_bulk>;
    p.table.cursor_origin = main_only<h_cursor_origin>;
    p.owner = m.env.hooks->new_guest("plugin:" + name);

    p.table.select_raid_member = main_only<h_select_raid_member>;
    p.table.decode_item_packet = main_only<h_decode_item_packet>;
    p.table.inspect_item_packet = main_only<h_inspect_item_packet>;
    p.table.manual_follow = main_only<h_manual_follow>;
    p.table.get_follow_status = main_only<h_get_follow_status>;
    p.table.get_memorized_spell = main_only<h_get_memorized_spell>;
    p.table.monotonic_ms = main_only<h_monotonic_ms>;
    p.table.raid_action = main_only<h_raid_action>;
    p.table.world_capabilities = main_only<h_world_capabilities>;
    p.table.world_floor_below = main_only<h_world_floor_below>;
    p.table.world_anchor = main_only<h_world_anchor>;
    p.table.world_submit = main_only<h_world_submit>;
    p.table.world_surface_patch = main_only<h_world_surface_patch>;
    p.table.measure_binary = main_only<h_measure_binary>;
    p.table.get_map_policy = main_only<h_get_map_policy>;
    p.table.set_class_name = main_only<h_set_class_name>;
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
    if (p.can(kCapUi) && m.env.ui) m.env.ui->set_folder(name, plugin_folder(*file, name));

    if (!guarded(p, "hailstone_plugin_set_host", [&] {
            set_host(&p.table);
            return true;
        })) {
        discard(m, std::move(l), false);
        return refuse(name + " failed while taking its host table; InitializePlugin was not called");
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
    log::info("plugin {} loaded from {} (sha256 {}, caps {})", name, file->string(), to_hex(hash), caps_text(caps));
    return true;
}

bool NativePlugins::unload(std::string_view name) {
    Impl& m = *impl_;
    if (const Loaded* l = m.find(name); l && m.env.policy &&
        m.env.policy->required_file(FileKind::Plugin, l->hash)) {
        m.say("[Plugin] " + l->name + " is required by this server: it stays loaded");
        return false;
    }
    return unload_file(name);
}

bool NativePlugins::unload_file(std::string_view name) {
    Impl& m = *impl_;
    auto it = std::find_if(m.loaded.begin(), m.loaded.end(), [&](const auto& l) { return iequal(l->name, name); });
    if (it == m.loaded.end()) return false;
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
    for (auto it = names.rbegin(); it != names.rend(); ++it) unload_file(*it);
}

void NativePlugins::policy_changed(const std::vector<std::string>& autoload) {
    Impl& m = *impl_;
    std::vector<std::string> drop, revoked;
    for (auto& l : m.loaded) {
        std::uint32_t caps = m.env.policy ? m.env.policy->caps(FileKind::Plugin, l->hash, l->name) : 0;
        if (caps == 0) {
            drop.push_back(l->name);
            continue;
        }
        // The player took back the permission for the server's plugin.
        if (m.asks(l->file, l->hash) && m.consent_for(l->hash) != Consent::Allowed) {
            revoked.push_back(l->name);
            continue;
        }
        if (l->caps != caps) {
            l->policy_generation = core::next_process_identity();
            l->lifecycle_generation = core::next_process_identity();
        }
        l->caps = caps;
        if (!l->can(kCapUi | kCapExtend)) {
            l->world_ready = false; m.world_geometry.retire(l->owner); std::erase(m.world_owners, l->owner);
        }
        // No longer allowed to extend the client: its class names go, and it is out of the chain (a plugin that
        // gains it needs a reload).
        if (!l->can(kCapExtend)) game::clear_class_name_overrides(l->class_names_owner);
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
        unload_file(n);
    }
    for (const auto& n : revoked) {
        m.say("[Plugin] " + n + ": you no longer allow it on this server");
        unload_file(n);
    }
    if (!m.env.policy || !m.env.policy->received()) {
        m.required.clear();
        if (!m.pending.empty()) ++m.consent_serial;
        m.pending.clear(); // nothing runs out of the world: no question either
        return;
    }
    // The server's own plugins always load; the player's those `autoload` names.
    bool all = std::find(autoload.begin(), autoload.end(), "*") != autoload.end();
    const auto found = list();
    m.required.clear();
    if (m.env.policy->current() && !m.env.policy->current()->provisional)
        m.required = m.env.policy->required_entries(FileKind::Plugin);
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
        const auto hash = parse_sha256(i.sha256);
        const bool required = hash && std::any_of(m.required.begin(), m.required.end(), [&](const auto& entry) {
            return iequal(entry.first, i.name) && entry.second == *hash;
        });
        bool wanted = i.server || required || all ||
                      std::any_of(autoload.begin(), autoload.end(), [&](const std::string& a) { return iequal(a, i.name); });
        if (wanted) load_file(i.name, nullptr, Asker::Rules);
    }
}

bool NativePlugins::required_addons_ready(std::string* problem) const {
    if (problem) problem->clear();
    const Impl& m = *impl_;
    if (!m.env.policy || !m.env.policy->current()) return true;
    const auto& rules = *m.env.policy->current();
    if (!rules.addons.enabled || rules.provisional) return true;
    Impl::ReadyInputs inputs{m.env.policy->serial(), plugin_approvals_generation(), m.required, {}, {}, {}, {}};
    for (const auto& l : m.loaded) inputs.loaded.emplace_back(l->name, l->hash, l->caps, l->threw_at.size());
    for (const auto& f : m.failures) inputs.failures.emplace_back(f.name, f.error);
    for (const auto& p : m.pending) inputs.pending.push_back(p.sha256);
    for (const auto& a : m.session_answers) inputs.session_answers.emplace_back(a.sha256, a.allowed);
    if (m.ready_inputs != inputs) {
        m.ready_inputs = std::move(inputs);
        m.ready_problem.clear();
        m.ready_answer = check_required_addons(&m.ready_problem);
    }
    if (problem) *problem = m.ready_problem;
    return m.ready_answer;
}

std::size_t NativePlugins::retry_required_addons() {
    Impl& m = *impl_;
    if (!m.env.policy || !m.env.policy->current() || m.env.policy->current()->provisional) return 0;
    std::size_t tried = 0;
    const auto required = m.required; // (load_file can reach policy_changed through a plugin's start)
    for (const auto& [name, hash] : required) {
        if (const Loaded* l = m.find(name)) {
            if (l->hash != hash || l->threw_at.empty()) continue; // running as required, or another plugin's name
            if (l->busy) {
                unload_file(name);
                continue;
            }
            unload_file(name);
            if (m.find(name)) continue;
        }
        log::info("plugin {}: the player retries the required add-on", name);
        load_file(name, nullptr, Asker::Rules);
        ++tried;
    }
    return tried;
}

bool NativePlugins::check_required_addons(std::string* problem) const {
    const Impl& m = *impl_;
    auto fail = [&](std::string text) {
        if (problem) *problem = std::move(text);
        return false;
    };
    for (const auto& l : m.loaded)
        if (m.env.policy->caps(FileKind::Plugin, l->hash, l->name) == 0)
            return fail(l->name + " is still loaded but is not allowed by this server");
    for (const auto& entry : m.required) {
        const auto& name = entry.first;
        const auto& hash = entry.second;
        // Named by its repository, as the server's add-on policy names it (docs/plugins.md).
        const std::string who = "Required add-on " + m.env.policy->required_label(FileKind::Plugin, hash, name);
        if (m.env.policy->caps(FileKind::Plugin, hash, name) == 0)
            return fail(who + " is not permitted by this server's rules");
        const Loaded* l = m.find(name);
        if (l && l->hash != hash)
            return fail(who + " is shadowed by a different plugin");
        // One that let an exception out stays loaded (its other callbacks may still work) but is not counted as
        // running as the server requires: the player is told, and plays on (ClientApp::addon_world_allowed).
        if (l && l->hash == hash && !l->threw_at.empty())
            return fail(who + " failed with an error in " + l->threw_at.front());
        const Consent consent = m.consent_for(hash);
        if (consent == Consent::Denied) return fail(who + " was not approved");
        if (consent == Consent::Unasked) {
            if (std::any_of(m.pending.begin(), m.pending.end(), [&](const ConsentFile& p) { return p.sha256 == hash; }))
                return false;
            return fail(who + " needs your approval");
        }
        if (l && l->hash == hash && l->caps != 0) continue;
        for (const auto& f : m.failures)
            if (iequal(f.name, name)) return fail(who + ": " + f.error);
        return fail(who + " did not load");
    }
    return true;
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
    const Impl& m = *impl_;
    if (m.pending.empty()) return std::nullopt;
    ConsentPrompt p;
    p.profile_dir = m.profile_dir();
    p.files = m.pending;
    p.serial = m.consent_serial;
    return p;
}

void NativePlugins::answer_consent(std::optional<bool> allow) {
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
