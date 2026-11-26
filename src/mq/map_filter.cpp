#include "mq/map_filter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/float_chars.h"
#include "game/combat.h"
#include "game/doors.h"
#include "game/world_state.h"
#include "mq/spawn_search.h"
#include "mq/tlo.h"
#include "net/game_connection.h"

namespace hailstone::mq {

namespace {

using F = MapFilter;

constexpr std::array<MapFilterSpec, static_cast<std::size_t>(F::Last)> kSpecs{{
    {"All", F::All, F::Invalid, kMapFilterToggle | kMapFilterNoColor, 0, "Enables or disables map functions"},
    {"PC", F::Pc, F::All, kMapFilterToggle, 0xff00ff, "Displays player characters"},
    {"PCConColor", F::PcConColor, F::Pc, kMapFilterToggle | kMapFilterNoColor, 0, "Displays players in consider colours"},
    {"Group", F::Group, F::Pc, kMapFilterToggle, 0x0080c0, "Displays group members in a specific colour"},
    {"Mount", F::Mount, F::All, kMapFilterToggle, 0x707070, "Displays mounts"},
    {"NPC", F::Npc, F::All, kMapFilterToggle, 0x404040, "Displays NPCs"},
    {"NPCConColor", F::NpcConColor, F::Npc, kMapFilterToggle | kMapFilterNoColor, 0, "Displays NPCs in consider colours"},
    {"Untargetable", F::Untargetable, F::All, kMapFilterToggle, 0x404040, "Displays untargetable spawns"},
    {"Pet", F::Pet, F::All, kMapFilterToggle, 0x707070, "Displays pets"},
    {"Corpse", F::Corpse, F::All, kMapFilterToggle, 0x00c000, "Displays corpses"},
    {"Chest", F::Chest, F::All, kMapFilterToggle, 0xc08000, "Displays chests"},
    {"Trigger", F::Trigger, F::All, kMapFilterToggle, 0xc08000, "Displays hidden triggers"},
    {"Trap", F::Trap, F::All, kMapFilterToggle, 0xc08000, "Displays hidden traps"},
    {"Timer", F::Timer, F::All, kMapFilterToggle, 0xc08000, "Displays hidden timers"},
    {"Ground", F::Ground, F::All, kMapFilterToggle, 0xc0c0c0, "Displays ground items"},
    {"Target", F::Target, F::All, kMapFilterToggle, 0xc00000, "Displays your target"},
    {"TargetLine", F::TargetLine, F::Target, kMapFilterToggle, 0x800000, "Displays a line to your target"},
    {"TargetRadius", F::TargetRadius, F::Target, kMapFilterRadius, 0x808080, "Sets a circle radius around your target"},
    {"TargetMelee", F::TargetMelee, F::Target, kMapFilterToggle, 0xff8080, "Draws a melee-range circle around your target"},
    {"Vector", F::Vector, F::All, kMapFilterToggle | kMapFilterNoColor, 0, "Displays heading vectors"},
    {"Custom", F::Custom, F::All, kMapFilterNoColor, 0, "Sets a custom spawn search; omit it to disable"},
    {"CastRadius", F::CastRadius, F::All, kMapFilterRadius, 0x808080, "Sets the casting circle radius"},
    {"NormalLabels", F::NormalLabels, F::Invalid, kMapFilterToggle | kMapFilterNoColor, 0, "Toggles the game's map labels"},
    {"SpellRadius", F::SpellRadius, F::All, kMapFilterRadius, 0x00c000, "Sets the second casting circle radius"},
    {"Aura", F::Aura, F::All, kMapFilterToggle, 0x404040, "Displays auras"},
    {"Object", F::Object, F::All, kMapFilterToggle, 0x404040, "Displays inanimate objects"},
    {"Banner", F::Banner, F::All, kMapFilterToggle, 0x404040, "Displays banners"},
    {"Campfire", F::Campfire, F::All, kMapFilterToggle, 0x404040, "Displays campfires"},
    {"PCCorpse", F::PcCorpse, F::Corpse, kMapFilterToggle, 0x00c000, "Displays player corpses when Corpse is on"},
    {"NPCCorpse", F::NpcCorpse, F::Corpse, kMapFilterToggle, 0x00c000, "Displays NPC corpses when Corpse is on"},
    {"Mercenary", F::Mercenary, F::All, kMapFilterToggle, 0x404040, "Displays mercenaries"},
    {"Named", F::Named, F::All, kMapFilterToggle, 0x404040, "Displays named NPCs"},
    {"TargetPath", F::TargetPath, F::Target, kMapFilterToggle | kMapFilterNoColor, 0, "Draws the server path to your target"},
    {"Marker", F::Marker, F::All, kMapFilterNoColor, 0, "Sets a marker shape and size for a spawn type"},
}};

static_assert(kSpecs.size() == static_cast<std::size_t>(F::Last));
static_assert(kSpecs.front().filter == F::All && kSpecs.back().filter == F::Marker);

bool valid(MapFilter filter) { return filter > F::Invalid && filter < F::Last; }

std::size_t index_of(MapFilter filter) { return static_cast<std::size_t>(filter); }

std::string key(std::string_view name, std::string_view field) {
    return std::format("mapfilter.{}.{}", name, field);
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        text = trim(text);
        if (text.empty()) break;
        const std::size_t end = text.find_first_of(" \t");
        out.push_back(text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end);
    }
    return out;
}

std::string_view rest_after(std::string_view text, std::size_t count) {
    for (std::size_t i = 0; i < count && !text.empty(); ++i) {
        text = trim(text);
        const std::size_t end = text.find_first_of(" \t");
        if (end == std::string_view::npos) return {};
        text.remove_prefix(end);
    }
    return trim(text);
}

int colour_part(std::string_view text) {
    std::int64_t value = 255;
    if (!text.empty()) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) value = 255;
    }
    return static_cast<int>(std::clamp<std::int64_t>(value, 0, 255));
}

std::uint32_t rgb(std::string_view text) {
    const auto parts = words(text);
    const int r = colour_part(parts.size() > 0 ? parts[0] : std::string_view{});
    const int g = colour_part(parts.size() > 1 ? parts[1] : std::string_view{});
    const int b = colour_part(parts.size() > 2 ? parts[2] : std::string_view{});
    return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

bool has_colour(const MapFilterSpec& spec) { return (spec.flags & kMapFilterNoColor) == 0; }
bool is_toggle(const MapFilterSpec& spec) { return (spec.flags & kMapFilterToggle) != 0; }
bool is_radius(const MapFilterSpec& spec) { return (spec.flags & kMapFilterRadius) != 0; }

void status_line(const MapFilters& filters, MapFilter filter, const std::function<void(std::string_view)>& say) {
    const auto* spec = map_filter_spec(filter);
    if (!spec) return;
    const auto& state = filters.state(filter);
    std::string text;
    if (is_toggle(*spec)) text = std::format("{}: {}", spec->name, filters.enabled(filter) ? "show" : "hide");
    else if (filter == F::Custom) {
        if (!filters.custom_enabled()) text = "Custom: Off";
        else text = std::format("Custom: {}", filters.custom_text().value_or(std::string{}));
    } else if (is_radius(*spec)) text = std::format("{}: {:.2f}", spec->name, state.radius);
    else text = std::format("{}: {}", spec->name, filters.enabled(filter) ? 1 : 0);
    if (has_colour(*spec))
        text = std::format("{} (Color: {} {} {})", text, (state.rgb >> 16) & 0xff, (state.rgb >> 8) & 0xff, state.rgb & 0xff);
    say(text);
}

} // namespace

std::span<const MapFilterSpec> map_filter_specs() { return kSpecs; }

std::optional<MapFilter> find_map_filter(std::string_view name) {
    for (const auto& spec : kSpecs)
        if (iequals(name, spec.name)) return spec.filter;
    return std::nullopt;
}

const MapFilterSpec* map_filter_spec(MapFilter filter) {
    if (!valid(filter)) return nullptr;
    const auto& spec = kSpecs[index_of(filter)];
    return spec.filter == filter ? &spec : nullptr;
}

std::string_view map_marker_name(MapMarker marker) {
    switch (marker) {
    case MapMarker::None: return "None";
    case MapMarker::Triangle: return "Triangle";
    case MapMarker::Square: return "Square";
    case MapMarker::Diamond: return "Diamond";
    case MapMarker::Ring: return "Ring";
    }
    return "None";
}

std::optional<MapMarker> find_map_marker(std::string_view name) {
    for (auto marker : {MapMarker::None, MapMarker::Triangle, MapMarker::Square, MapMarker::Diamond, MapMarker::Ring})
        if (iequals(name, map_marker_name(marker))) return marker;
    return std::nullopt;
}

MapFilters::MapFilters(Config* config) : config_(config) {
    for (std::size_t i = 0; i < states_.size(); ++i) {
        const auto& spec = kSpecs[i];
        states_[i].enabled = spec.filter == F::All || spec.filter == F::NormalLabels;
        states_[i].rgb = spec.default_rgb;
    }
    if (config_) load(*config_);
}

void MapFilters::load(Config& config) {
    config_ = &config;
    for (const auto& spec : kSpecs) {
        auto& state = states_[index_of(spec.filter)];
        state.enabled = spec.filter == F::All || spec.filter == F::NormalLabels;
        state.radius = 0.0f;
        state.rgb = spec.default_rgb;
        state.marker = MapMarker::None;
        state.marker_size = 0;
        state.enabled = config.get_bool(key(spec.name, "enabled"), state.enabled);
        if (is_radius(spec)) {
            state.radius = std::clamp(config.get_float(key(spec.name, "radius"), 0.0f), 0.0f, kMapFilterMaxRadius);
            state.enabled = state.radius > 0.0f;
        }
        if (has_colour(spec))
            state.rgb = static_cast<std::uint32_t>(config.get_int(key(spec.name, "color"), static_cast<int>(spec.default_rgb))) &
                        0xffffffu;
        if (auto marker = find_map_marker(config.get_string(key(spec.name, "marker"), "None"))) state.marker = *marker;
        state.marker_size = std::clamp(config.get_int(key(spec.name, "marker-size"), 0), 0, kMapFilterMaxMarkerSize);
    }
    states_[index_of(F::Custom)].enabled = false; // MQ2Map does not persist a custom search
    custom_text_.reset();
    custom_search_.reset();
    vanilla_enabled_ = config.get_bool("mapfilter.Vanilla.enabled", false);
}

void MapFilters::save(Config& config) const {
    for (const auto& spec : kSpecs) {
        const auto& state = states_[index_of(spec.filter)];
        if (spec.filter != F::Custom) config.set_bool(key(spec.name, "enabled"), state.enabled);
        if (is_radius(spec)) config.set(key(spec.name, "radius"), std::format("{:.8g}", state.radius));
        if (has_colour(spec)) config.set_int(key(spec.name, "color"), static_cast<int>(state.rgb));
        config.set(key(spec.name, "marker"), map_marker_name(state.marker));
        if (state.marker_size != 0) config.set_int(key(spec.name, "marker-size"), state.marker_size);
        else config.erase(key(spec.name, "marker-size"));
    }
    config.set_bool("mapfilter.Vanilla.enabled", vanilla_enabled_);
}

bool MapFilters::enabled(MapFilter filter) const {
    if (!valid(filter)) return filter == F::Invalid;
    if (suspended_) return false;
    const auto& spec = kSpecs[index_of(filter)];
    return states_[index_of(filter)].enabled && enabled(spec.parent);
}

bool MapFilters::requirements_met(MapFilter filter) const {
    const auto* spec = map_filter_spec(filter);
    return spec && enabled(spec->parent);
}

const MapFilterState& MapFilters::state(MapFilter filter) const {
    static const MapFilterState invalid{};
    return valid(filter) ? states_[index_of(filter)] : invalid;
}

std::optional<std::reference_wrapper<const MapFilterState>> MapFilters::find_state(MapFilter filter) const {
    if (!valid(filter)) return std::nullopt;
    return std::cref(states_[index_of(filter)]);
}

void MapFilters::changed() {
    ++revision_;
    persist();
}

void MapFilters::persist() {
    if (config_) save(*config_);
}

void MapFilters::set_enabled(MapFilter filter, bool enabled) {
    if (!valid(filter)) return;
    auto& state = states_[index_of(filter)];
    if (state.enabled == enabled) return;
    state.enabled = enabled;
    if (is_radius(kSpecs[index_of(filter)])) {
        state.radius = enabled ? state.radius : 0.0f;
        state.enabled = enabled && state.radius > 0.0f;
    }
    changed();
}

void MapFilters::set_radius(MapFilter filter, float radius) {
    const auto* spec = map_filter_spec(filter);
    if (!spec || !is_radius(*spec)) return;
    if (!std::isfinite(radius)) radius = 0.0f;
    radius = std::clamp(radius, 0.0f, kMapFilterMaxRadius);
    auto& state = states_[index_of(filter)];
    if (state.radius == radius && state.enabled == (radius > 0.0f)) return;
    state.radius = radius;
    state.enabled = radius > 0.0f;
    changed();
}

void MapFilters::set_color(MapFilter filter, std::uint32_t color) {
    const auto* spec = map_filter_spec(filter);
    if (!spec || !has_colour(*spec)) return;
    color &= 0xffffffu;
    auto& state = states_[index_of(filter)];
    if (state.rgb == color) return;
    state.rgb = color;
    changed();
}

void MapFilters::set_marker(MapFilter filter, MapMarker marker, int size) {
    if (!valid(filter)) return;
    size = std::clamp(size, kMapFilterMinMarkerSize, kMapFilterMaxMarkerSize);
    auto& option_state = states_[index_of(filter)];
    if (option_state.marker == marker && option_state.marker_size == size && state(F::Marker).enabled) return;
    option_state.marker = marker;
    option_state.marker_size = size;
    states_[index_of(F::Marker)].enabled = true;
    changed();
}

void MapFilters::set_marker_enabled(bool enabled) {
    auto& option_state = states_[index_of(F::Marker)];
    if (option_state.enabled == enabled) return;
    option_state.enabled = enabled;
    changed();
}

void MapFilters::set_vanilla_enabled(bool enabled) {
    if (vanilla_enabled_ == enabled) return;
    vanilla_enabled_ = enabled;
    changed();
}

void MapFilters::set_vanilla_locked(bool locked) {
    if (vanilla_locked_ == locked) return;
    vanilla_locked_ = locked;
    ++revision_; // the server's lock changes the effective mode, but is never saved as the player's choice
}

void MapFilters::set_suspended(bool suspended) {
    if (suspended_ == suspended) return;
    suspended_ = suspended;
    ++revision_; // like the server's vanilla lock: the effective state changes, the saved choices do not
}

void MapFilters::set_custom(std::string_view text, const game::GuildState* guild) {
    text = trim(text);
    auto parsed = text.empty() ? std::optional<SpawnSearch>{} : std::optional<SpawnSearch>{parse_spawn_search(text, guild)};
    auto held = text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
    if (custom_enabled() == !text.empty() && custom_text_ == held && custom_search_.has_value() == parsed.has_value()) return;
    custom_text_ = std::move(held);
    custom_search_ = std::move(parsed);
    states_[index_of(F::Custom)].enabled = parsed.has_value();
    changed();
}

void MapFilters::clear_custom() { set_custom({}); }
bool MapFilters::custom_enabled() const { return custom_search_.has_value() && enabled(F::Custom); }

bool MapFilterMark::is_target() const { return target; }

std::optional<MapFilterMark> MapFilters::spawn_mark(const game::Spawn& spawn, const MapFilterSpawnInput& input) const {
    if (!enabled(F::All)) return std::nullopt;
    const game::eSpawnType type = spawn_type(spawn);
    if (type == game::eSpawnType::FLYER || type == game::eSpawnType::NONE || type == game::eSpawnType::ITEM)
        return std::nullopt;

    auto base_filter = [&]() -> MapFilter {
        switch (type) {
        case game::eSpawnType::PC: return F::Pc;
        case game::eSpawnType::MOUNT: return F::Mount;
        case game::eSpawnType::PET: return F::Pet;
        case game::eSpawnType::NPC:
            // MQ2Map's Named colour is used for a # name; other named NPCs keep the NPC colour.
            return enabled(F::Named) && spawn.name.find('#') != std::string::npos ? F::Named : F::Npc;
        case game::eSpawnType::CORPSE: return spawn.deity == 0 ? F::NpcCorpse : F::PcCorpse;
        case game::eSpawnType::CHEST: return F::Chest;
        case game::eSpawnType::TRIGGER: return F::Trigger;
        case game::eSpawnType::TRAP: return F::Trap;
        case game::eSpawnType::TIMER: return F::Timer;
        case game::eSpawnType::UNTARGETABLE: return F::Untargetable;
        case game::eSpawnType::AURA: return F::Aura;
        case game::eSpawnType::OBJECT: return F::Object;
        case game::eSpawnType::BANNER: return F::Banner;
        case game::eSpawnType::CAMPFIRE: return F::Campfire;
        case game::eSpawnType::MERCENARY: return F::Mercenary;
        default: return F::Invalid;
        }
    };

    bool shown = false;
    if (input.target && enabled(F::Target)) shown = true;
    else if (custom_enabled()) shown = input.custom && input.custom->matches(spawn);
    else {
        switch (type) {
        case game::eSpawnType::PC: shown = enabled(F::Pc); break;
        case game::eSpawnType::MOUNT: shown = enabled(F::Mount); break;
        case game::eSpawnType::PET: shown = enabled(F::Pet); break;
        case game::eSpawnType::NPC:
            shown = enabled(F::Named) ? is_named(spawn) : enabled(F::Npc);
            break;
        case game::eSpawnType::CORPSE:
            shown = enabled(spawn.deity == 0 ? F::NpcCorpse : F::PcCorpse);
            break;
        case game::eSpawnType::CHEST: shown = enabled(F::Chest); break;
        case game::eSpawnType::TRIGGER: shown = enabled(F::Trigger); break;
        case game::eSpawnType::TRAP: shown = enabled(F::Trap); break;
        case game::eSpawnType::TIMER: shown = enabled(F::Timer); break;
        case game::eSpawnType::UNTARGETABLE: shown = enabled(F::Untargetable); break;
        case game::eSpawnType::AURA: shown = enabled(F::Aura); break;
        case game::eSpawnType::OBJECT: shown = enabled(F::Object); break;
        case game::eSpawnType::BANNER: shown = enabled(F::Banner); break;
        case game::eSpawnType::CAMPFIRE: shown = enabled(F::Campfire); break;
        case game::eSpawnType::MERCENARY: shown = enabled(F::Mercenary); break;
        default: break;
        }
    }
    if (!shown) return std::nullopt;

    const MapFilter base = base_filter();
    if (base == F::Invalid) return std::nullopt;
    MapFilterMark out{};
    out.marker_filter = type == game::eSpawnType::CORPSE ? F::Corpse : base;
    out.target = input.target && enabled(F::Target);
    out.color_filter = out.target ? F::Target : base;
    out.rgb = state(out.color_filter).rgb;
    if (!out.target && type == game::eSpawnType::PC && enabled(F::PcConColor))
        out.rgb = game::con_rgb(game::con_level(spawn, input.player_level, input.pvp_rules));
    else if (!out.target && type == game::eSpawnType::NPC && enabled(F::NpcConColor))
        out.rgb = game::con_rgb(game::con_level(spawn, input.player_level, input.pvp_rules));
    if (!out.target && input.group && enabled(F::Group)) {
        out.color_filter = F::Group;
        out.rgb = state(F::Group).rgb;
    }
    out.label = game::spawn_label(spawn);
    if (type == game::eSpawnType::CORPSE && !icontains(out.label, "corpse")) out.label += "'s Corpse";
    return out;
}

bool MapFilters::ground_visible() const { return enabled(F::Ground); }

std::string MapFilters::ground_label(const game::GroundObject& object) {
    std::string label = object.name;
    constexpr std::string_view suffix = "_ACTORDEF";
    if (label.size() >= suffix.size() && iequals(std::string_view(label).substr(label.size() - suffix.size()), suffix))
        label.resize(label.size() - suffix.size());
    return label.empty() ? std::string("Ground item") : label;
}

void update_map_filter_target_path(MapFilters& filters, net::GameConnection& connection,
                                   MapFilterTargetPathState& state, std::chrono::steady_clock::time_point now) {
    // A fresh state keeps the time of the last request: the pacing is per connection, not per target.
    auto reset = [&state] {
        const auto last = state.last_request;
        state = {};
        state.last_request = last;
    };
    const std::uint64_t context = connection.interaction_context();
    if (state.context != context) reset();

    const auto& world = connection.world();
    const auto target = world.spawns.find(world.target_id);
    const bool enabled = filters.enabled(F::TargetPath) && target != world.spawns.end();
    auto owns_current = [&] {
        return state.owns_path && connection.find_path_generation() == state.generation &&
               connection.player_path().kind() == game::FindKind::Spawn &&
               connection.player_path().target_id() == static_cast<std::int32_t>(state.target);
    };
    if (!enabled) {
        if (owns_current()) connection.stop_find_path(false);
        reset();
        return;
    }

    if (state.target != target->second.id || state.incarnation != target->second.incarnation ||
        state.context != context) {
        if (owns_current()) connection.stop_find_path(false);
        reset();
        state.target = target->second.id;
        state.incarnation = target->second.incarnation;
        state.context = context;
        state.target_since = now;
    }
    if (state.owns_path) {
        if (!owns_current()) state.owns_path = false;
        else return;
    }
    if (state.requested) return;
    if (now - state.target_since < kTargetPathSettle) return;
    if (state.last_request && now - *state.last_request < kTargetPathInterval) return;
    state.requested = true;
    // A shown or pending player Find wins, even when it points at this same target. Its generation, not only its
    // kind/id, tells us when the player has taken ownership back.
    if (connection.has_find_path()) return;
    state.last_request = now;
    state.owns_path = connection.find_spawn(state.target, {false, true});
    if (state.owns_path) state.generation = connection.find_path_generation();
}

void run_map_filter_command(MapFilters& filters, std::string_view args, const MapFilterCommandInput& input,
                            const std::function<void(std::string_view)>& say) {
    args = trim(args);
    if (args.empty()) {
        say("Map filtering settings:");
        say("-----------------------");
        say(std::format("Vanilla: {}{}", filters.vanilla_mode() ? "show" : "hide",
                        filters.vanilla_locked() ? " (server)" : ""));
        for (const auto& spec : kSpecs)
            if (filters.requirements_met(spec.filter)) status_line(filters, spec.filter, say);
        return;
    }
    const auto parts = words(args);
    const std::string_view option_word = parts.empty() ? std::string_view{} : parts.front();
    if (iequals(option_word, "help")) {
        say("Map filtering options:");
        for (const auto& spec : kSpecs)
            say(std::format("{}{}: {}", spec.name, is_toggle(spec) ? "" : " #", spec.help));
        say("Vanilla: Shows only the map background, without marks or overlays");
        say("'option' color [r g b]: set its display colour; omit the numbers to reset");
        return;
    }
    if (iequals(option_word, "vanilla")) {
        bool enabled = !filters.vanilla_enabled();
        const std::string_view rest = rest_after(args, 1);
        if (iequals(rest, "show") || iequals(rest, "on")) enabled = true;
        else if (iequals(rest, "hide") || iequals(rest, "off")) enabled = false;
        filters.set_vanilla_enabled(enabled);
        say(std::format("Vanilla is now set to: {}", enabled ? "show" : "hide"));
        if (filters.vanilla_locked()) say("This server keeps vanilla mode on.");
        return;
    }
    const auto filter = find_map_filter(option_word);
    if (!filter) {
        say("Usage: /mapfilter [option|help]");
        return;
    }
    const auto* spec = map_filter_spec(*filter);
    if (!spec) return;
    std::string_view rest = rest_after(args, 1);
    const auto rest_parts = words(rest);

    if (!rest_parts.empty() && iequals(rest_parts.front(), "color")) {
        if (!has_colour(*spec)) {
            say(std::format("Option '{}' does not have a color.", spec->name));
            return;
        }
        const std::uint32_t color = rest_parts.size() > 1 ? rgb(rest_after(rest, 1)) : spec->default_rgb;
        filters.set_color(*filter, color);
        const auto& state = filters.state(*filter);
        say(std::format("Option '{}' color set to: {} {} {}", spec->name, (state.rgb >> 16) & 0xff,
                        (state.rgb >> 8) & 0xff, state.rgb & 0xff));
        return;
    }

    if (!filters.requirements_met(*filter)) {
        const auto* required = map_filter_spec(spec->parent);
        say(std::format("'{}' requires '{}' option.  Please enable this option first.", spec->name,
                        required ? required->name : "unknown"));
        return;
    }

    if (is_radius(*spec)) {
        double value = 0.0;
        if (const auto parsed = finite_float_from_text<double>(rest)) value = *parsed;
        value = std::clamp(value, 0.0, static_cast<double>(kMapFilterMaxRadius));
        filters.set_radius(*filter, static_cast<float>(value));
        say(std::format("{} is now set to: {:.2f}", spec->name, filters.state(*filter).radius));
        return;
    }

    if (*filter == F::Custom) {
        filters.set_custom(rest, input.guild);
        say(filters.custom_enabled() ? std::format("{} is now set to: {}", spec->name, filters.custom_text().value_or(std::string{}))
                                     : std::format("{} is now set to: Off", spec->name));
        return;
    }

    if (*filter == F::Marker) {
        if (rest.empty()) {
            filters.set_marker_enabled(true);
            say(std::format("{} is now set to: {}", spec->name, filters.enabled(*filter) ? "show" : "hide"));
        } else if (iequals(rest, "show") || iequals(rest, "on")) {
            filters.set_marker_enabled(true);
            say(std::format("{} is now set to: {}", spec->name, filters.enabled(*filter) ? "show" : "hide"));
        } else if (iequals(rest, "hide") || iequals(rest, "off")) {
            filters.set_marker_enabled(false);
            say(std::format("{} is now set to: {}", spec->name, filters.enabled(*filter) ? "show" : "hide"));
        } else {
            const auto type = rest_parts.size() > 0 ? find_map_filter(rest_parts[0]) : std::nullopt;
            const auto marker = rest_parts.size() > 1 ? find_map_marker(rest_parts[1]) : std::nullopt;
            if (!type) {
                say(std::format("Marker unchanged, unknown spawn type: {}", rest_parts.size() > 0 ? rest_parts[0] : ""));
            } else if (!marker) {
                say(std::format("Marker unchanged, unknown shape: '{}'", rest_parts.size() > 1 ? rest_parts[1] : ""));
            } else {
                int size = kMapFilterDefaultMarkerSize;
                if (rest_parts.size() > 2) {
                    std::int64_t parsed = 0;
                    const auto word = rest_parts[2];
                    const auto result = std::from_chars(word.data(), word.data() + word.size(), parsed);
                    if (result.ec != std::errc{} || result.ptr != word.data() + word.size() || parsed <= 0)
                        parsed = 0;
                    if (parsed == 0) {
                        say(std::format("Marker unchanged, invalid size: '{}'", word));
                        return;
                    }
                    size = static_cast<int>(std::clamp<std::int64_t>(parsed, kMapFilterMinMarkerSize, kMapFilterMaxMarkerSize));
                }
                filters.set_marker(*type, *marker, size);
                const auto& state = filters.state(*type);
                say(std::format("Marker '{}' is now set to '{}' with size {}.", map_filter_spec(*type)->name,
                                map_marker_name(state.marker), state.marker_size));
            }
        }
        return;
    }

    if (!is_toggle(*spec)) {
        say(std::format("Option '{}' needs a value.", spec->name));
        return;
    }
    // MQ2Map flips the saved bit, not the effective value (All and the parent may still keep it hidden).
    bool enabled = !filters.state(*filter).enabled;
    if (!rest.empty()) {
        if (iequals(rest, "show") || iequals(rest, "on")) enabled = true;
        else if (iequals(rest, "hide") || iequals(rest, "off")) enabled = false;
    }
    filters.set_enabled(*filter, enabled);
    say(std::format("{} is now set to: {}", spec->name, filters.enabled(*filter) ? "show" : "hide"));
}

} // namespace hailstone::mq
