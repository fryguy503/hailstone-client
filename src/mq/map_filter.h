#pragma once

// MacroQuest's /mapfilter options and rules (src/plugins/map/MQ2Map.cpp, MQ2MapCommands.cpp and MQ2MapAPI.cpp in
// MacroQuest, used as the specification). The state is shared by the command and the map presenter; nothing here
// draws or writes a map file.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/config.h"
#include "mq/spawn_search.h"

namespace hailstone::game { struct GuildState; struct GroundObject; struct Spawn; }
namespace hailstone::net { class GameConnection; }

namespace hailstone::mq {

enum class MapFilter : int {
    Invalid = -1,
    All = 0,
    Pc,
    PcConColor,
    Group,
    Mount,
    Npc,
    NpcConColor,
    Untargetable,
    Pet,
    Corpse,
    Chest,
    Trigger,
    Trap,
    Timer,
    Ground,
    Target,
    TargetLine,
    TargetRadius,
    TargetMelee,
    Vector,
    Custom,
    CastRadius,
    NormalLabels,
    SpellRadius,
    Aura,
    Object,
    Banner,
    Campfire,
    PcCorpse,
    NpcCorpse,
    Mercenary,
    Named,
    TargetPath,
    Marker,
    Last,
};

enum class MapMarker : std::uint8_t { None, Triangle, Square, Diamond, Ring };

enum MapFilterFlags : std::uint32_t {
    kMapFilterToggle = 1u << 0,
    kMapFilterNoColor = 1u << 1,
    kMapFilterRadius = 1u << 2,
};

struct MapFilterSpec {
    const char* name;
    MapFilter filter;
    MapFilter parent;
    std::uint32_t flags;
    std::uint32_t default_rgb;
    const char* help;
};

std::span<const MapFilterSpec> map_filter_specs();
std::optional<MapFilter> find_map_filter(std::string_view name);
const MapFilterSpec* map_filter_spec(MapFilter filter);
std::string_view map_marker_name(MapMarker marker);
std::optional<MapMarker> find_map_marker(std::string_view name);

// A radius past this cannot describe a zone or a useful map overlay. Marker sizes have their own bound below.
inline constexpr float kMapFilterMaxRadius = 1000000.0f;
inline constexpr int kMapFilterMinMarkerSize = 1;
inline constexpr int kMapFilterMaxMarkerSize = 10000;
inline constexpr int kMapFilterDefaultMarkerSize = 6;

struct MapFilterState {
    bool enabled = false;
    float radius = 0.0f;
    std::uint32_t rgb = 0;
    MapMarker marker = MapMarker::None;
    int marker_size = 0;
};

struct MapFilterMark {
    MapFilter color_filter;
    MapFilter marker_filter;
    std::uint32_t rgb;
    std::string label;
    bool target;
    MapFilterMark()
        : color_filter(MapFilter::Invalid), marker_filter(MapFilter::Invalid), rgb(0), target(false) {}
    bool is_target() const;
};

struct MapFilterSpawnInput {
    bool target = false;
    bool group = false;
    int player_level = 0;
    std::int32_t pvp_rules = 0;
    const SpawnSearchMatcher* custom = nullptr;
};

class MapFilters {
public:
    explicit MapFilters(Config* config = nullptr);

    void load(Config& config);
    void save(Config& config) const;
    std::uint64_t revision() const { return revision_; }

    bool enabled(MapFilter filter) const;
    bool requirements_met(MapFilter filter) const;
    const MapFilterState& state(MapFilter filter) const;
    std::optional<std::reference_wrapper<const MapFilterState>> find_state(MapFilter filter) const;

    void set_enabled(MapFilter filter, bool enabled);
    void set_radius(MapFilter filter, float radius);
    void set_color(MapFilter filter, std::uint32_t rgb);
    void set_marker(MapFilter filter, MapMarker marker, int size);
    void set_marker_enabled(bool enabled);

    // Hailstone's vanilla mode sits outside MacroQuest's 34 options. The player's choice is shared and persisted;
    // a server can lock the effective mode on without changing that choice.
    bool vanilla_enabled() const { return vanilla_enabled_; }
    bool vanilla_locked() const { return vanilla_locked_; }
    bool vanilla_mode() const { return vanilla_locked_ || (vanilla_enabled_ && !suspended_); }
    void set_vanilla_enabled(bool enabled);
    void set_vanilla_locked(bool locked);

    // A server's [commands] rules that deny /mapfilter suspend the player's saved options: every option reads as
    // off (no marks, lines or circles, and TargetPath asks for nothing) and the player's vanilla choice, which the
    // command could no longer change, no longer applies. The saved settings stay for servers that allow it.
    bool suspended() const { return suspended_; }
    void set_suspended(bool suspended);

    void set_custom(std::string_view text, const game::GuildState* guild = nullptr);
    void clear_custom();
    bool custom_enabled() const;
    const std::optional<std::string>& custom_text() const { return custom_text_; }
    const std::optional<SpawnSearch>& custom_search() const { return custom_search_; }

    // The shared map presenter asks these once per collected spawn. A target is admitted before the type filters,
    // then a custom search replaces them; otherwise the MQ2Map type rules decide. Group colour is applied last,
    // while the target colour wins over it.
    std::optional<MapFilterMark> spawn_mark(const game::Spawn& spawn, const MapFilterSpawnInput& input) const;
    bool ground_visible() const;
    static std::string ground_label(const game::GroundObject& object);

private:
    static constexpr std::size_t kCount = static_cast<std::size_t>(MapFilter::Last);
    std::array<MapFilterState, kCount> states_{};
    Config* config_ = nullptr;
    std::uint64_t revision_ = 0;
    std::optional<std::string> custom_text_;
    std::optional<SpawnSearch> custom_search_;
    bool vanilla_enabled_ = false;
    bool vanilla_locked_ = false;
    bool suspended_ = false;

    void changed();
    void persist();
};

struct MapFilterCommandInput {
    const game::GuildState* guild = nullptr;
};

// TargetPath's pacing: a target must stay selected this long before its path is asked for (tabbing through
// targets asks for none of the ones passed over), and two requests are at least this far apart.
inline constexpr std::chrono::milliseconds kTargetPathSettle{500};
inline constexpr std::chrono::milliseconds kTargetPathInterval{1000};

struct MapFilterTargetPathState {
    std::uint64_t context = 0;
    std::uint32_t target = 0;
    std::uint64_t incarnation = 0;
    std::uint64_t generation = 0;
    bool requested = false;
    bool owns_path = false;
    std::chrono::steady_clock::time_point target_since{};             // when this target (or context) was first seen
    std::optional<std::chrono::steady_clock::time_point> last_request; // kept across targets and contexts
};

// Hailstone's TargetPath, not MQ2Map's: MQ2Map's option only lets a right-click on a map label reach the game's own
// handler. Here it keeps the server find path on the current target while the option is on, asking with a silent
// OP_FindPersonRequest once the target has settled (kTargetPathSettle) and no sooner than kTargetPathInterval after
// the last request. The background path never turns the player or plays a sound. A path the player starts takes
// ownership back; TargetPath then leaves it alone until the option is toggled or the target changes again.
// Disabling the option only stops a path this updater still owns.
void update_map_filter_target_path(MapFilters& filters, net::GameConnection& connection,
                                   MapFilterTargetPathState& state, std::chrono::steady_clock::time_point now);

// /mapfilter, help and status. `say` receives one chat line at a time; the caller decides colour and squelch handling.
void run_map_filter_command(MapFilters& filters, std::string_view args, const MapFilterCommandInput& input,
                            const std::function<void(std::string_view)>& say);

} // namespace hailstone::mq
