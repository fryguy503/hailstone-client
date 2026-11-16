#include "app/nameplate_canvas_context.h"

#include <cmath>
#include "game/world_state.h"
#include "net/game_connection.h"

namespace hailstone::app {
void fill_nameplate_canvas_candidate(game::NameplateCanvasCandidate& candidate,
                                    const net::GameConnection& connection, const game::Spawn& spawn,
                                    std::string_view caption, double distance) {
    auto& out = candidate.spawn;
    out.id = spawn.id; out.name = spawn.name; out.incarnation = spawn.incarnation;
    out.level = spawn.level; out.class_id = spawn.class_id; out.race = spawn.race;
    out.owner = spawn.pet_owner_id; out.guild = spawn.guild_id; out.rider = spawn.rider_id;
    out.body_type = spawn.body_type; out.heading = spawn.heading;
    out.x = spawn.position.x; out.y = spawn.position.y; out.z = spawn.position.z;
    out.npc = spawn.is_npc; out.corpse = spawn.is_corpse; out.mercenary = spawn.mercenary; out.gm = spawn.gm;
    auto& context = candidate.context;
    context = {};
    context.name = caption;
    context.self = spawn.id == connection.world().player_id;
    context.target = spawn.id == connection.world().target_id;
    const std::string name = game::display_name(spawn.name);
    context.in_group = connection.group().active && connection.group().has_member(name);
    context.in_raid = connection.raid().active && connection.raid().is_member(name);
    context.distance = distance;
    const auto percent = [](double value) -> std::optional<double> {
        if (!std::isfinite(value) || value < 0 || value > 100) return std::nullopt;
        return value;
    };
    context.hp_percent = percent(spawn.hp_percent);
    if (context.self) {
        const auto hp = connection.own_hp();
        if (hp.max > 0) context.hp_percent = percent(100.0 * static_cast<double>(hp.cur) / static_cast<double>(hp.max));
        const auto& player = connection.player_status();
        if (player.max_mana > 0) context.mana_percent = percent(100.0 * static_cast<double>(player.mana) / player.max_mana);
        if (player.max_endurance > 0) context.endurance_percent = percent(100.0 * static_cast<double>(player.endurance) / player.max_endurance);
    } else {
        if (auto it = connection.group().mana_percent.find(spawn.id); it != connection.group().mana_percent.end())
            context.mana_percent = percent(it->second);
        if (auto it = connection.group().endurance_percent.find(spawn.id); it != connection.group().endurance_percent.end())
            context.endurance_percent = percent(it->second);
    }
    const auto& leadership = connection.leadership();
    for (std::size_t i = 0; i < leadership.group_marks.size(); ++i) {
        if (leadership.group_marks[i] == spawn.id) context.group_mark = static_cast<std::uint32_t>(i + 1);
        if (leadership.raid_marks[i] == spawn.id) context.raid_mark = static_cast<std::uint32_t>(i + 1);
    }
    if (connection.aggro().target_id == spawn.id && connection.aggro().received[net::kAggroPlayer])
        context.aggro_percent = percent(connection.aggro().pct[net::kAggroPlayer]);
    const auto& spells = connection.spell_state();
    std::uint32_t spell = game::kNoSpell, duration = 0;
    std::int64_t started = 0;
    if (context.self && spells.casting.casting()) {
        spell = spells.casting.spell;
        duration = spells.casting.cast_time;
        started = spells.casting.eta_ms - duration;
    } else if (const auto it = spells.others.find(spawn.id); it != spells.others.end()) {
        spell = it->second.spell;
        duration = it->second.cast_time;
        started = it->second.started_ms;
    }
    const auto now = net::GameConnection::now_ms();
    if (spell != game::kNoSpell && duration > 0 && now >= started && now - started < duration) {
        game::CanvasCast cast;
        cast.spell_id = spell;
        cast.duration_ms = static_cast<double>(duration);
        cast.elapsed_ms = static_cast<double>(now - started);
        if (const auto* data = connection.spell_table().get(spell)) cast.name = data->name;
        context.casting = std::move(cast);
    }
}
} // namespace hailstone::app
