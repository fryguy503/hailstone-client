#include "app/progress_ui.h"

#include <cstdlib>
#include <glm/geometric.hpp>
#include <imgui.h>

#include "core/env.h"
#include "core/distribution.h"
#include "core/log.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

using Clock = std::chrono::steady_clock;

ProgressUi::ProgressUi() {
    if (!distribution::managed())
        if (const char* t = hailstone::core::env("PROGRESS_TEST")) test_ = t;
    if (const char* w = hailstone::core::env("PROGRESS_WINDOWS")) panel_ = *w && *w != '0';
}

void ProgressUi::draw(net::GameConnection& conn) {
    if (conn.state() != net::ConnectionState::InZone) return;
    if (!test_.empty()) drive_test(conn);

    if (const auto& t = conn.training(); t && !ui::lua::replaces("TrainWindow")) { // off while the Lua window runs
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(("Training: " + t->trainer_name + "###progress_training").c_str(), &open)) {
            ImGui::Text("Practice points: %d", conn.practice_points());
            for (std::size_t i = 0; i < t->rows.size(); ++i) {
                const auto& r = t->rows[i];
                auto id = static_cast<std::uint32_t>(r.kind);
                std::int32_t cost = conn.train_cost(i);
                std::string label = conn.skill_name(id < 100 ? id : 0);
                if (r.language()) label = "Language " + std::to_string(r.id());
                ImGui::Text("%-24s %3d / %3d  %dp %dg %ds %dc", label.c_str(), conn.skill_value(id), r.cap, cost / 1000,
                            cost / 100 % 10, cost / 10 % 10, cost % 10);
                ImGui::SameLine();
                if (ImGui::SmallButton(("Train##t" + std::to_string(i)).c_str())) conn.train(i);
            }
        }
        ImGui::End();
        if (!open) conn.close_trainer();
    }

    if (const auto& o = conn.tasks().offers; o && !ui::lua::replaces("TaskSelectWnd")) { // off while the Lua window runs
        ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Task select###progress_tasks_offer")) {
            for (const auto& t : o->offers) {
                ImGui::PushID(static_cast<int>(t.task_id));
                ImGui::TextWrapped("%s", t.title.c_str());
                ImGui::TextWrapped("%s", t.description.c_str());
                for (const auto& e : t.elements) ImGui::BulletText("%s", conn.task_element_text(e).c_str());
                if (ImGui::Button("Accept")) conn.accept_task(t.task_id);
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::Button("Decline")) conn.decline_tasks();
        }
        ImGui::End();
    }
    if (panel_) draw_panel(conn);
}

void ProgressUi::draw_panel(net::GameConnection& conn) {
    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Progress (debug)###progress_panel", &panel_)) {
        ImGui::End();
        return;
    }
    const auto& aa = conn.alt_adv();
    if (ImGui::CollapsingHeader("Alternate advancement", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("AA exp %u/330, %u points, %u spent, %u%% to AAs", aa.exp, aa.points, aa.spent, aa.percent);
        int percent = aa.percent;
        ImGui::SliderInt("AA exp %", &percent, 0, 100);
        if (ImGui::IsItemDeactivatedAfterEdit()) conn.set_aa_percent(static_cast<std::uint32_t>(percent));
        ImGui::SliderInt("Tab", &aa_tab_, 0, 6);
        std::uint8_t cls = conn.profile() ? conn.profile()->class_ : 0;
        for (const game::AltAbility* a : aa.visible(aa_tab_, cls)) {
            ImGui::PushID(static_cast<int>(a->id));
            auto ready = conn.aa_readiness(a->id);
            bool owned = aa.has(a->id);
            ImGui::Text("%s %d/%d cost %d%s%s", conn.aa_name(*a).c_str(), owned ? a->rank : a->rank - 1, a->max_rank,
                        a->cost, owned ? " (owned)" : "", ready.ready ? "" : " (not ready)");
            if (!owned && conn.aa_can_train(a->id)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Train")) conn.aa_train_button(a->id);
            }
            if (owned && a->activated()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Use")) conn.aa_activate(static_cast<std::uint32_t>(a->group));
            }
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Tasks", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& t : conn.tasks().tasks) {
            ImGui::PushID(static_cast<int>(t.task_id));
            ImGui::TextWrapped("%s (type %u, slot %u)", t.title.c_str(), t.type, t.sequence);
            for (const auto& [id, e] : t.elements) {
                if (e.hidden) ImGui::BulletText("? ? ?");
                else
                    ImGui::BulletText("%s %d/%d", conn.task_element_text(e).c_str(), e.done_count, e.goal_count);
            }
            if (ImGui::SmallButton("Abandon")) conn.cancel_task(t.type, t.sequence);
            ImGui::PopID();
        }
        for (std::size_t i = 0; i < conn.tasks().completed.size(); ++i) {
            const auto& c = conn.tasks().completed[i];
            ImGui::PushID(static_cast<int>(1000000 + i));
            ImGui::Text("Completed: %s", c.title.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("History")) conn.request_task_history(static_cast<std::uint32_t>(i));
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Skills")) {
        for (std::uint32_t s = 0; s < 100; ++s)
            if (std::int32_t v = conn.skill_value(s); v > 0 && v < 255)
                ImGui::Text("%-28s %d", conn.skill_name(s).c_str(), v);
    }
    ImGui::End();
}

void ProgressUi::drive_test(net::GameConnection& conn) {
    if (distribution::managed()) return;
    auto now = Clock::now();
    if (test_since_ == Clock::time_point{}) test_since_ = step_since_ = now;
    if (now - test_since_ < std::chrono::seconds(6) || test_step_ < 0) return;
    const auto& world = conn.world();
    const game::Spawn* me = world.player();
    if (!me) return;
    auto nearest = [&](auto&& pick) -> std::uint32_t {
        std::uint32_t best = 0;
        float best_d = 20.0f;
        for (const auto& [id, s] : world.spawns) {
            if (!s.is_npc || s.is_corpse || id == world.player_id || !pick(s)) continue;
            float d = glm::distance(s.position, me->position);
            if (d < best_d) {
                best_d = d;
                best = id;
            }
        }
        return best;
    };
    auto next = [&](int step) {
        test_step_ = step;
        step_since_ = now;
    };
    auto waited = [&](int s) { return now - step_since_ > std::chrono::seconds(s); };

    if (test_ == "train") {
        if (test_step_ == 0) {
            std::uint32_t id = nearest([](const game::Spawn& s) { return s.class_id >= 20 && s.class_id <= 35; });
            if (!id) {
                log::warn("progress test: no guildmaster within reach");
                return next(-1);
            }
            conn.set_target(id);
            log::info("progress test: open_trainer({}) -> {}", id, conn.open_trainer(id));
            next(1);
        } else if (test_step_ == 1 && conn.training() && waited(2)) {
            log::info("progress test: {} rows, {} practice points", conn.training()->rows.size(), conn.practice_points());
            for (std::size_t i = 0; i < conn.training()->rows.size(); ++i) {
                const auto& r = conn.training()->rows[i];
                log::info("progress test: row {} kind {} value {} cap {} cost {}", i, r.kind,
                          conn.skill_value(static_cast<std::uint32_t>(r.kind)), r.cap, conn.train_cost(i));
            }
            for (std::size_t i = 0; i < conn.training()->rows.size(); ++i)
                if (conn.train(i)) {
                    log::info("progress test: trained row {}", i);
                    break;
                }
            next(2);
        } else if (test_step_ == 2 && waited(4)) {
            log::info("progress test: closing the trainer ({} practice points)", conn.practice_points());
            conn.close_trainer();
            next(-1);
        } else if (test_step_ == 1 && waited(10)) {
            log::warn("progress test: no training window");
            next(-1);
        }
        return;
    }
    if (test_ == "aa") {
        if (test_step_ == 0) {
            const auto& aa = conn.alt_adv();
            log::info("progress test: {} AA ranks, {} owned, {} points, {} spent, exp {} percent {}", aa.table.size(),
                      aa.owned.size(), aa.points, aa.spent, aa.exp, aa.percent);
            std::uint8_t cls = conn.profile() ? conn.profile()->class_ : 0;
            int trainable = 0;
            for (const game::AltAbility* a : aa.visible(0, cls))
                if (conn.aa_can_train(a->id)) ++trainable;
            log::info("progress test: {} visible, {} trainable", aa.visible(0, cls).size(), trainable);
            conn.set_aa_percent(50);
            next(1);
        } else if (test_step_ == 1 && waited(3)) {
            log::info("progress test: AA percent now {}", conn.alt_adv().percent);
            conn.set_aa_percent(0);
            next(2);
        } else if (test_step_ == 2 && waited(3)) {
            log::info("progress test: AA percent now {}", conn.alt_adv().percent);
            next(-1);
        }
        return;
    }
    if (test_ == "task") {
        if (test_step_ == 0) {
            std::uint32_t id = nearest([](const game::Spawn&) { return true; });
            if (!id) {
                log::warn("progress test: no NPC within reach");
                return next(-1);
            }
            conn.set_target(id);
            conn.hail();
            next(1);
        } else if (test_step_ == 1 && conn.tasks().offers && waited(2)) {
            const auto& o = *conn.tasks().offers;
            log::info("progress test: {} offers", o.offers.size());
            if (!o.offers.empty()) conn.accept_task(o.offers.front().task_id);
            next(2);
        } else if (test_step_ == 1 && waited(8)) {
            log::warn("progress test: no task offered");
            next(2);
        } else if (test_step_ == 2 && waited(4)) {
            for (const auto& t : conn.tasks().tasks) {
                log::info("progress test: task {} '{}' {} elements", t.task_id, t.title, t.elements.size());
                for (const auto& [eid, e] : t.elements)
                    log::info("progress test:   {} {}/{}", e.hidden ? "? ? ?" : conn.task_element_text(e), e.done_count,
                              e.goal_count);
            }
            log::info("progress test: {} completed tasks", conn.tasks().completed.size());
            if (!conn.tasks().completed.empty()) conn.request_task_history(0);
            next(3);
        } else if (test_step_ == 3 && waited(3)) {
            for (const auto& [index, elements] : conn.tasks().history)
                log::info("progress test: history of {}: {} elements", index, elements.size());
            next(-1);
        }
        return;
    }
    if (test_ == "combine") {
        if (test_step_ == 0) {
            for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
                const net::Item* it = conn.inventory().at(net::ItemSlot::possessions(s));
                if (it && it->is_bag() && !it->contents.empty()) {
                    log::info("progress test: combining {} (type {}, {} items)", it->name, it->bag_type,
                              it->contents.size());
                    log::info("progress test: combine_container -> {}", conn.combine_container(s));
                    break;
                }
            }
            next(1);
        } else if (test_step_ == 1 && waited(4)) {
            log::info("progress test: combine pending {}", conn.combine_pending());
            next(-1);
        }
        return;
    }
}

} // namespace hailstone::app
