#include "plugin/native_nameplates.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

namespace hailstone::plugin {
namespace {
constexpr std::array kCommandKinds{
    game::CanvasCommandKind::Rect, game::CanvasCommandKind::RectFilled,
    game::CanvasCommandKind::Gradient, game::CanvasCommandKind::Line,
    game::CanvasCommandKind::Circle, game::CanvasCommandKind::CircleFilled,
    game::CanvasCommandKind::Triangle, game::CanvasCommandKind::TriangleFilled,
    game::CanvasCommandKind::Text, game::CanvasCommandKind::Image,
};
static_assert(kCommandKinds.size() == HAILSTONE_NAMEPLATE_IMAGE + 1);

HailstoneNameplateString borrow(std::string_view text) {
    return {text.data(), static_cast<std::uint32_t>(text.size())};
}

std::uint32_t flag(bool present, HailstoneNameplateCandidateFlag value) {
    return present ? static_cast<std::uint32_t>(value) : 0u;
}

HailstoneNameplateCandidate candidate_view(const game::NameplateCanvasCandidate& candidate) {
    const auto& spawn = candidate.spawn;
    const auto& context = candidate.context;
    HailstoneNameplateCandidate view{};
    view.flags = flag(spawn.npc, HAILSTONE_NAMEPLATE_NPC) | flag(spawn.corpse, HAILSTONE_NAMEPLATE_CORPSE) |
                 flag(spawn.mercenary, HAILSTONE_NAMEPLATE_MERCENARY) | flag(spawn.gm, HAILSTONE_NAMEPLATE_GM) |
                 flag(context.target, HAILSTONE_NAMEPLATE_TARGET) | flag(context.self, HAILSTONE_NAMEPLATE_SELF) |
                 flag(context.in_group, HAILSTONE_NAMEPLATE_GROUP) | flag(context.in_raid, HAILSTONE_NAMEPLATE_RAID) |
                 flag(context.hp_percent.has_value(), HAILSTONE_NAMEPLATE_HP_KNOWN) |
                 flag(context.mana_percent.has_value(), HAILSTONE_NAMEPLATE_MANA_KNOWN) |
                 flag(context.endurance_percent.has_value(), HAILSTONE_NAMEPLATE_ENDURANCE_KNOWN) |
                 flag(context.aggro_percent.has_value(), HAILSTONE_NAMEPLATE_AGGRO_KNOWN) |
                 flag(context.concealed, HAILSTONE_NAMEPLATE_CONCEALED) |
                 flag(context.invisible, HAILSTONE_NAMEPLATE_INVISIBLE) |
                 flag(context.invisible_undead, HAILSTONE_NAMEPLATE_INVISIBLE_UNDEAD);
    view.spawn_id = spawn.id;
    view.incarnation = spawn.incarnation;
    view.level = spawn.level;
    view.class_id = spawn.class_id;
    view.race = spawn.race;
    view.owner_id = spawn.owner;
    view.guild_id = spawn.guild;
    view.rider_id = spawn.rider;
    view.body_type = spawn.body_type;
    view.group_mark = context.group_mark;
    view.raid_mark = context.raid_mark;
    view.x = candidate.x;
    view.y = candidate.y;
    view.depth = candidate.depth;
    view.native_scale = candidate.native_scale;
    view.world_x = spawn.x;
    view.world_y = spawn.y;
    view.world_z = spawn.z;
    view.heading = spawn.heading;
    view.distance = context.distance;
    view.hp_percent = context.hp_percent.value_or(0);
    view.mana_percent = context.mana_percent.value_or(0);
    view.endurance_percent = context.endurance_percent.value_or(0);
    view.aggro_percent = context.aggro_percent.value_or(0);
    view.spawn_name = borrow(spawn.name);
    view.caption = borrow(context.name);
    if (!context.secondary_caption.empty()) view.secondary_caption = borrow(context.secondary_caption);
    if (context.casting) {
        view.flags |= HAILSTONE_NAMEPLATE_CASTING;
        view.cast_spell_id = context.casting->spell_id;
        view.cast_elapsed_ms = context.casting->elapsed_ms;
        view.cast_duration_ms = context.casting->duration_ms;
        view.cast_name = borrow(context.casting->name);
    }
    return view;
}

bool copy_command(const HailstoneNameplateCommand& source, game::CanvasCommand& out) {
    if (source.kind >= kCommandKinds.size() || source.text.bytes > game::kCanvasMaxTextBytes ||
        (source.text.bytes && !source.text.data)) return false;
    out.kind = kCommandKinds[source.kind];
    out.color = source.color;
    out.color2 = source.color2;
    out.alpha = source.alpha;
    out.alpha2 = source.alpha2;
    out.x = source.x;
    out.y = source.y;
    out.x0 = source.x0;
    out.y0 = source.y0;
    out.x1 = source.x1;
    out.y1 = source.y1;
    out.x2 = source.x2;
    out.y2 = source.y2;
    out.radius = source.radius;
    out.rounding = source.rounding;
    if (source.thickness != 0) out.thickness = source.thickness;
    if (source.font_size != 0) out.size = source.font_size;
    if (source.segments != 0) out.segments = source.segments;
    out.font = source.font;
    out.texture = source.texture;
    out.u0 = source.u0;
    out.v0 = source.v0;
    out.u1 = source.u1;
    out.v1 = source.v1;
    if (source.text.bytes) out.text.assign(source.text.data, source.text.bytes);
    return true;
}
} // namespace

NativeNameplateFrame::NativeNameplateFrame(game::NameplateCanvasRegistry& registry, const game::NameplateCanvasFrame& frame)
    : registry_(registry), frame_(frame) {
    candidates_.reserve(frame.candidates.size());
    for (const auto& candidate : frame.candidates) candidates_.push_back(candidate_view(candidate));
    view_ = {sizeof(view_), static_cast<std::uint32_t>(candidates_.size()),
             static_cast<std::uint32_t>(sizeof(HailstoneNameplateCandidate)), 0, frame.serial,
             frame.now_ms, frame.dt_ms, frame.font_size, frame.font,
             {frame.viewport.x, frame.viewport.y, frame.viewport.w, frame.viewport.h}, candidates_.data()};
}

int NativeNameplateFrame::submit(const HailstoneNameplateModel* source) {
    constexpr std::uint32_t allowed = HAILSTONE_NAMEPLATE_HIDE | HAILSTONE_NAMEPLATE_CLICKABLE |
                                      HAILSTONE_NAMEPLATE_THROUGH_WALLS;
    if (!source || source->size < sizeof(*source) || !source->frame || source->frame != frame_.serial ||
        source->candidate_index >= candidates_.size() || source->candidate_index >= claimed_.size() ||
        claimed_.test(source->candidate_index) || (source->flags & ~allowed) != 0 ||
        (source->command_count && source->command_size < sizeof(HailstoneNameplateCommand)) ||
        source->command_count > game::kCanvasMaxCommands ||
        source->command_count > game::kCanvasMaxFrameCommands - command_count_ ||
        (source->command_count && !source->commands)) return 0;

    game::NameplateCanvasModel model;
    model.token = frame_.candidates[source->candidate_index].token;
    model.bounds = {source->bounds.x, source->bounds.y, source->bounds.width, source->bounds.height};
    model.offset_x = source->offset_x;
    model.offset_y = source->offset_y;
    model.hide = (source->flags & HAILSTONE_NAMEPLATE_HIDE) != 0;
    model.clickable = (source->flags & HAILSTONE_NAMEPLATE_CLICKABLE) != 0;
    model.through_walls = (source->flags & HAILSTONE_NAMEPLATE_THROUGH_WALLS) != 0;
    model.commands.reserve(source->command_count);
    // By the plugin's stride: a newer SDK's larger record keeps the fields this host knows at their offsets, and
    // only those are copied (memcpy: a stride need not keep the record aligned).
    const auto* records = static_cast<const unsigned char*>(source->commands);
    for (std::uint32_t i = 0; i < source->command_count; ++i) {
        HailstoneNameplateCommand input;
        std::memcpy(&input, records + static_cast<std::size_t>(i) * source->command_size, sizeof(input));
        if (input.kind == HAILSTONE_NAMEPLATE_IMAGE &&
            std::find(textures_.begin(), textures_.end(), input.texture) == textures_.end()) return 0;
        game::CanvasCommand command;
        if (!copy_command(input, command)) return 0;
        model.commands.push_back(std::move(command));
    }
    if (!game::valid_canvas_model(model)) return 0;
    const auto vertices = game::canvas_model_vertices(model);
    if (vertices > game::kCanvasMaxModelVertices || vertices > game::kCanvasMaxFrameVertices - vertex_count_)
        return 0;
    models_.push_back(std::move(model));
    claimed_.set(source->candidate_index);
    command_count_ += source->command_count;
    vertex_count_ += vertices;
    return 1;
}

int NativeNameplateFrame::measure_text(const char* text, std::uint32_t text_bytes, float font_size, std::int32_t font,
                                       HailstoneNameplateTextMetrics* out) const {
    if (!out || out->size < sizeof(*out) || !text || text_bytes > game::kCanvasMaxTextBytes ||
        !std::isfinite(font_size) || font_size < 4 || font_size > 256 || font < 0 || font > 6) return 0;
    const auto metrics = registry_.measure(std::string_view(text, text_bytes), font_size, font);
    if (!metrics) return 0;
    *out = {sizeof(*out), metrics->width, metrics->height, metrics->ink_x0, metrics->ink_y0,
            metrics->ink_x1, metrics->ink_y1};
    return 1;
}

int NativeNameplateFrame::spell_icon(std::uint32_t icon_id, HailstoneNameplateImage* out) {
    if (!out || out->size < sizeof(*out) || icon_id > 65535) return 0;
    const auto icon = registry_.icon(icon_id);
    if (!icon || !icon->texture) return 0;
    if (std::find(textures_.begin(), textures_.end(), icon->texture) == textures_.end())
        textures_.push_back(icon->texture);
    *out = {sizeof(*out), icon->texture, icon->u0, icon->v0, icon->u1, icon->v1};
    return 1;
}

std::vector<game::NameplateCanvasModel> NativeNameplateFrame::take_models() {
    return std::move(models_);
}

} // namespace hailstone::plugin
