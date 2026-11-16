// ClientApp: Text overlay, nameplates and the Lua UI host glue.

#include "app/client_app_internal.h"
#include "app/nameplate_layer.h"
#include "app/nameplate_canvas_context.h"
#include "ui/lua/lua_host.h"
#include "ui/texture_ids.h"

#include "game/item_preview.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "mq/lua_mq.h"

namespace hailstone::app::client_internal {

void ClientApp::draw_text_overlay() {
    // CTextOverlay::Draw @0x41f610 (game/text_overlay.h): the line wrapped in {x0 + W/4, y0 + 0.33 H, x0 + 3W/4, y1},
    // centred (DrawWrappedText flags 0x10), in font 6 (the static at 0xab3448 passed to the text object's draw,
    // vtable +0xc): EQGraphicsDX9's font table @0x100b2100 makes it Arial with a cell height of 24, the same as
    // the UI's font 6 (ui::ui_font).
    if (!conn_) return;
    auto frame = conn_->text_overlay().frame(static_cast<std::uint32_t>(net::GameConnection::now_ms()));
    if (!frame) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float left = static_cast<float>(static_cast<int>(display.x * 0.25f));
    float top = static_cast<float>(static_cast<int>(display.y * 0.33f));
    float width = static_cast<float>(static_cast<int>(display.x * 0.5f));
    ImFont* font = ui::ui_font(6);
    if (!font) font = ImGui::GetFont();
    float size = font->FontSize;
    std::uint32_t c = frame->argb;
    ImU32 colour = IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, (c >> 24) & 0xff);
    // Wrapped into lines, each centred in the rectangle.
    const std::string& text = frame->text;
    const char* p = text.c_str();
    const char* end = p + text.size();
    float y = top;
    while (p < end) {
        const char* line_end = font->CalcWordWrapPositionA(size / font->FontSize, p, end, width);
        if (line_end == p) ++line_end;
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, line_end);
        dl->AddText(font, size, ImVec2(left + (width - ts.x) * 0.5f, y), colour, p, line_end);
        y += size;
        p = line_end;
        while (p < end && *p == ' ') ++p;
    }
}

std::int32_t ClientApp::viewer_see_invisible_rank() const {
    if (!conn_) return 0;
    for (const auto& kv : conn_->world().spawns)
        if (kv.second.invis_rank > 0) return conn_->see_invisible_rank();
    return 0;
}

void ClientApp::update_market_hidden(bool in_game) {
    market_hidden_.clear();
    if (!in_game || !conn_) return;
    const auto see_invisible = viewer_see_invisible_rank();
    for (const auto& [id, s] : conn_->world().spawns)
        if (conn_->spawn_invisible(s, see_invisible)) market_hidden_.insert(id);
}

void ClientApp::draw_nameplates(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& scene_view_proj,
                                VkExtent2D extent, float far_plane) {
    plate_hits_.clear();
    if (!conn_) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // The 3D view's pixels to its place in the window (/viewport).
    float sx = view_area_.w / static_cast<float>(extent.width), sy = view_area_.h / static_cast<float>(extent.height);
    dl->PushClipRect(ImVec2(view_area_.x, view_area_.y), ImVec2(view_area_.x + view_area_.w, view_area_.y + view_area_.h));
    const auto& world = conn_->world();
    // The leadership marks (game::name_sprite_with_marks).
    game::NameSpriteMarks marks;
    {
        const auto& l = conn_->leadership();
        marks.leadership = conn_->has_gates_of_discord();
        marks.npc_health = conn_->leadership_ability_active(1, 1) || conn_->leadership_ability_active(0x11, 1);
        marks.target_id = world.target_id;
        marks.assist_targets = l.assist_targets;
        marks.group_marks = l.group_marks;
        marks.raid_marks = l.raid_marks;
    }
    const game::NameSpriteSwitches switches{pc_names_, npc_names_, own_name_};
    const std::uint32_t hidden = hidden_spawn();
    // Hailstone (docs/graphics.md "Name plates"), a deliberate difference: each plate's text is hidden wherever the
    // scene is nearer, pixel by pixel (app/nameplate_layer.h); without the pipeline (a missing shader) it is drawn
    // plain.
    if (!plate_layer_) plate_layer_ = std::make_unique<NameplateLayer>(vk_, shader_dir_);
    plate_layer_->begin(frame, dl, glm::vec4(view_area_.x, view_area_.y, view_area_.w, view_area_.h), camera_.near_plane,
                        far_plane);
    auto* canvas_registry = automation_ ? &automation_->nameplates() : nullptr;
    const bool canvas_active = canvas_registry && !canvas_registry->empty() && plate_layer_->supports_canvas();
    NameplateCanvasPaintStyle canvas_style;
    if (canvas_active) {
        canvas_style.font = ui::plate_font();
        canvas_style.font_at = [](int index) { return ui::ui_font(index); };
        canvas_style.image_live = [](std::uintptr_t texture) {
            return ui::texture_id_live(reinterpret_cast<void*>(texture));
        };
    }
    // Hailstone (game/nameplate_style.h): the name plates' own font, size, edge and tag size.
    {
        NameplateDrawStyle style;
        style.font = ui::plate_font();
        style.size = ui::plate_font_size();
        style.faux_bold = ui::plate_font_faux_bold();
        style.edge = plate_style_.edge;
        style.tag_scale = static_cast<float>(plate_style_.tag_scale) / 100.0f;
        style.canvas = canvas_style;
        plate_layer_->set_style(style);
    }
    plate_count_ = 0;
    for (const auto& [id, s] : world.spawns) {
        if (market_hidden_.contains(id)) continue; // the same viewer awareness as actors and picking
        // ToggleNpcNameSprites / ToggleCharacterNameSprites; the player's own name too, as the original draws it (not
        // in first person, where its model is hidden).
        const bool me = id == world.player_id;
        if (!game::name_sprite_switched_on(s, me, me && hidden == id, switches)) continue;
        // PlayerClient::SetNameSpriteState draws nothing without bDisplayNameSprite (the spawn's showname bit); body
        // type 11 has no name either (EQEmu's BodyType::NoTarget, "no name, can't target": the server's teleport
        // triggers, #TPTrigger*, are sent with showname set).
        if (!s.show_name || s.body_type == 11) continue;
        glm::vec3 base = s.display_valid ? s.display_position : s.position;
        // The head is straight above the spawn, so one farther across the ground than the names' range is out of it
        // whatever its height: passed over before its model is looked up (most of a busy zone, every frame).
        if (glm::distance(glm::vec2(base), glm::vec2(camera_.position)) > kNameplateRange) continue;
        // Just above the model's head: its rest-pose top at the actor's scale above the model origin (the
        // spawn's z, lowered for EQG models). Without a model: the eye (ViewHeight above the feet).
        float top = s.heights.view_height - s.heights.avatar_height;
        if (actor_renderer_) {
            if (auto model = actor_renderer_->model_for(s))
                top = model->bounds_max.z * s.heights.model_scale +
                      render::ActorRenderer::model_z_offset(*model, s.heights.model_scale, world.height_tables.get());
        }
        glm::vec3 head = base + glm::vec3(0, 0, top + 1.5f);
        // A rider sits on its mount's seat bone, well above its spawn's place: its head from there.
        if (glm::vec3 seated; actor_renderer_ && actor_renderer_->drawn_rider_top(id, seated))
            head = seated + glm::vec3(0, 0, 1.5f);
        const float head_distance = glm::distance(head, camera_.position);
        if (!std::isfinite(head_distance) || head_distance > kNameplateRange) continue;
        glm::vec4 clip = view_proj * glm::vec4(head, 1.0f);
        if (!std::isfinite(clip.w) || clip.w <= 0.1f) continue;
        glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f) continue;
        float px = view_area_.x + (ndc.x * 0.5f + 0.5f) * static_cast<float>(extent.width) * sx;
        float py = view_area_.y + (0.5f - ndc.y * 0.5f) * static_cast<float>(extent.height) * sy;
        if (plate_count_ == plates_.size()) plates_.emplace_back();
        PlateEntry& entry = plates_[plate_count_];
        NameplateText& plate = entry.plate;
        plate.canvas.reset();
        entry.canvas_owner = 0;
        std::string& label = plate.name; // (built in place: the string keeps its capacity from frame to frame)
        label = game::display_name(s.name);
        // An NPC's last name is its owner line under the name (below); a player's follows the name.
        const bool npc = s.is_npc && !s.is_corpse;
        // PlayerClient::SetNameSpriteState @0x58e2d0 by __ShowNames (/shownames): a player's title and suffix above
        // level 3, the last name at 2, 3, 4 and 6, the guild at 3 and 4 (below); an NPC's name whatever the level.
        const bool player = !s.is_npc && !s.is_corpse;
        const int level = show_names_;
        if (!s.last_name.empty() && !npc && (!player || level == 2 || level == 3 || level == 4 || level == 6))
            label += " " + s.last_name;
        // A player's "Offline " and "Trader " / "Buyer " before the name, the last two in the title's place.
        const game::NameSpriteMarketWords market = game::name_sprite_market_words(s, conn_->strings());
        if (!s.title.empty() && (!player || level > 3) && !market.replaces_title) label = s.title + " " + label;
        if (!s.suffix.empty() && (!player || level > 3)) label += " " + s.suffix;
        if (!market.prefix.empty()) label.insert(0, market.prefix);
        if (!player) label = game::name_sprite_with_marks(s, label, marks);
        conn_->server_ext().nameplate_text(*conn_, s, label);
        plate.pos = glm::vec2(px, py);
        plate.depth = clip.w;
        plate.lines.clear();
        plate.alpha = 1.0f;
        // Hailstone (option plate_far_scale): a farther name drawn smaller.
        plate.scale = game::plate_distance_factor(head_distance, kNameplateRange, plate_style_.far_scale);
        plate.target = id == world.target_id;
        plate.highlight = plate.target && target_plate_highlight_;
        plate.through_walls = !game::plate_hidden_by_scene(plate.target, target_plate_through_walls_);
        // An NPC's owner line ("(Banker)", a pet's "(Soandso`s pet)" with PetOwnerNames), the name sprite's second
        // line (PlayerClient::SetNameSpriteState's "\n(owner)").
        if (std::string owner = game::npc_owner_line(s, conn_->client_option(game::ClientOption::PetOwnerNames));
            !owner.empty())
            plate.lines.push_back(std::move(owner));
        // The guild tag under a player's name ("<Guild Name>", the name sprite's second line).
        if (game::name_sprite_shows_guild(s, show_names_)) {
            std::string guild = conn_->guild().name_of(s.guild_id);
            if (!guild.empty()) plate.lines.push_back("<" + guild + ">");
        }
        plate.color = s.is_corpse ? IM_COL32(170, 170, 170, 255) : s.is_npc ? IM_COL32(255, 220, 120, 255) : IM_COL32(140, 190, 255, 255);
        // Hailstone (option con_name_colour, a deliberate difference; docs/graphics.md "Name plates"): an NPC's name takes
        // its consider colour, as the target plate below does. Off: this client's yellow (neither is the original's tint).
        if (auto rgb = game::npc_name_colour(s, conn_->player_status().level, conn_->pvp_rules(), con_name_colour_))
            plate.color = IM_COL32((*rgb >> 16) & 0xff, (*rgb >> 8) & 0xff, *rgb & 0xff, 255);
        // PlayerClient::SetNameSpriteTint: an NPC with IsMercenary (+0x208) is amber (game::mercenary_name_colour).
        if (auto rgb = game::mercenary_name_colour(s))
            plate.color = IM_COL32((*rgb >> 16) & 0xff, (*rgb >> 8) & 0xff, *rgb & 0xff, 255);
        if (plate.target) {
            const std::uint32_t rgb = game::con_rgb(game::con_level(s, conn_->player_status().level, conn_->pvp_rules()));
            plate.color = IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 255);
        }
        // PlayerClient::SetNameSpriteTint @0x58bf00: a GM's name is green (game::gm_name_colour). The original fades
        // a target's tint toward white; keeping the green on a targeted GM, over the consider colour above, is this
        // client's choice.
        if (auto rgb = game::gm_name_colour(s))
            plate.color = IM_COL32((*rgb >> 16) & 0xff, (*rgb >> 8) & 0xff, *rgb & 0xff, 255);
        entry.id = id;
        entry.head = head;
        ++plate_count_;
    }
    if (canvas_active) {
        game::NameplateCanvasFrame canvas_frame;
        canvas_frame.now_ms = static_cast<double>(net::GameConnection::now_ms());
        canvas_frame.dt_ms = static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0;
        canvas_frame.font_size = ui::plate_font_size();
        if (canvas_frame.font_size <= 0.0f)
            canvas_frame.font_size = canvas_style.font ? canvas_style.font->FontSize : ImGui::GetFontSize();
        canvas_frame.viewport = {view_area_.x, view_area_.y, view_area_.w, view_area_.h};
        std::vector<std::size_t> candidates;
        candidates.reserve(plate_count_);
        for (std::size_t i = 0; i < plate_count_; ++i) candidates.push_back(i);
        const auto priority = [&](std::size_t a, std::size_t b) {
            const auto& left = plates_[a]; const auto& right = plates_[b];
            if (left.plate.target != right.plate.target) return left.plate.target;
            if (left.plate.depth != right.plate.depth) return left.plate.depth < right.plate.depth;
            return left.id < right.id;
        };
        const auto count = std::min(candidates.size(), game::kCanvasMaxCandidates);
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count), candidates.end(), priority);
        candidates.resize(count);
        canvas_frame.candidates.reserve(count);
        std::vector<std::size_t> frame_entries;
        frame_entries.reserve(count);
        for (const auto index : candidates) {
            const auto& entry = plates_[index];
            const auto spawn = world.spawns.find(entry.id);
            if (spawn == world.spawns.end()) continue;
            game::NameplateCanvasCandidate candidate;
            candidate.x = entry.plate.pos.x; candidate.y = entry.plate.pos.y;
            candidate.depth = entry.plate.depth; candidate.native_scale = entry.plate.scale;
            fill_nameplate_canvas_candidate(candidate, *conn_, spawn->second, entry.plate.name,
                                            glm::distance(entry.head, camera_.position));
            canvas_frame.candidates.push_back(std::move(candidate));
            frame_entries.push_back(index);
        }
        canvas_registry->set_measure([canvas_style](std::string_view text, float size, int font) {
            return measure_nameplate_canvas_text(text, size, font, canvas_style);
        });
        canvas_registry->set_icon([this](std::uint32_t icon) -> std::optional<game::CanvasImage> {
            if (!ui_textures_) return std::nullopt;
            const auto cell = ui_textures_->spell_icon(static_cast<std::int32_t>(icon));
            if (!cell || !ui::texture_id_live(cell->id)) return std::nullopt;
            return game::CanvasImage{reinterpret_cast<std::uintptr_t>(cell->id), cell->u0, cell->v0, cell->u1, cell->v1};
        });
        const auto connection = conn_.get();
        const auto serial = world.spawn_set_serial;
        auto submissions = canvas_registry->frame(canvas_frame);
        canvas_registry->set_measure({});
        canvas_registry->set_icon({});
        // A callback can close its session. Never use candidates from a retired connection or entity set.
        if (conn_.get() != connection || !conn_ || conn_->world().spawn_set_serial != serial) {
            plate_count_ = 0;
            dl->PopClipRect();
            return;
        }
        for (auto& submission : submissions) {
            if (submission.candidate_index >= frame_entries.size() || !canvas_registry->owner_valid(submission.owner)) continue;
            auto& entry = plates_[frame_entries[submission.candidate_index]];
            entry.canvas_owner = submission.owner;
            entry.plate.through_walls = submission.model.through_walls;
            entry.plate.canvas = std::move(submission.model);
        }
        std::size_t shown = 0;
        for (std::size_t i = 0; i < plate_count_; ++i) {
            if (plates_[i].plate.canvas && plates_[i].plate.canvas->hide) continue;
            if (shown != i) plates_[shown] = std::move(plates_[i]);
            ++shown;
        }
        plate_count_ = shown;
    }
    // Hailstone (game/nameplate_overlap.h): the nearer name is drawn over the farther, and a name a nearer one covers
    // fades toward a tenth of its alpha, smoothly (tau 0.3 s). The target stays at full alpha at its projected head.
    plate_boxes_.clear();
    for (std::size_t i = 0; i < plate_count_; ++i) {
        NameplateText& p = plates_[i].plate;
        const glm::vec2 size = plate_layer_->size_of(p);
        // Hailstone: the plate's bottom edge at the head, so a larger name (or more lines) grows upward, off the head.
        if (!p.canvas) p.pos.y -= size.y;
        const auto bounds = plate_layer_->bounds_of(p);
        plate_boxes_.push_back({bounds.x, bounds.y, bounds.z, bounds.w, p.depth, p.target});
    }
    if (plate_overlap_fade_) game::plate_target_alpha(plate_boxes_, plate_alpha_);
    else plate_alpha_.assign(plate_boxes_.size(), 1.0f);
    game::plate_draw_order(plate_boxes_, plate_order_);
    const float dt = ImGui::GetIO().DeltaTime;
    for (std::size_t i = 0; i < plate_count_; ++i)
        plates_[i].plate.alpha = plate_fade_.step(plates_[i].id, plate_alpha_[i], dt, plates_[i].plate.target);
    plate_fade_.end_frame();
    for (std::size_t i : plate_order_) {
        const auto& entry = plates_[i];
        const auto& plate = entry.plate;
        if (!plate_layer_->draw(plate)) continue;
        const bool visible_canvas = plate.canvas && std::any_of(plate.canvas->commands.begin(), plate.canvas->commands.end(),
            [&](const game::CanvasCommand& command) {
                const float alpha = command.kind == game::CanvasCommandKind::Gradient ? std::max(command.alpha, command.alpha2) : command.alpha;
                return alpha * plate.alpha > 0.05f;
            });
        if (visible_canvas && plate.canvas->clickable &&
            canvas_registry && canvas_registry->owner_valid(entry.canvas_owner) && plate_hits_.size() < 1024) {
            auto bounds = plate_layer_->bounds_of(plate);
            bounds.x = std::max(bounds.x, view_area_.x); bounds.y = std::max(bounds.y, view_area_.y);
            bounds.z = std::min(bounds.z, view_area_.x + view_area_.w); bounds.w = std::min(bounds.w, view_area_.y + view_area_.h);
            const auto spawn = world.spawns.find(entry.id);
            if (bounds.z > bounds.x && bounds.w > bounds.y && spawn != world.spawns.end())
                plate_hits_.push_back({entry.id, bounds, entry.head, plate.through_walls, plate.depth,
                                       entry.canvas_owner, spawn->second.incarnation});
        }
    }
    plate_hit_connection_ = conn_.get();
    plate_hit_serial_ = world.spawn_set_serial;
    plate_hit_zone_ = conn_->zone_id();
    plate_hit_time_ = Clock::now();
    if (!plate_hits_.empty()) {
        plate_inverse_view_ = glm::inverse(scene_view_proj);
        plate_camera_ = camera_.position; plate_forward_ = camera_.forward();
        plate_hit_view_ = {view_area_.x, view_area_.y, view_area_.w, view_area_.h};
    }
    dl->PopClipRect();
}

void ClientApp::register_ui_api(lua_State* L) { ui_api_->register_functions(L); }

// What the Lua windows' hs.* functions read this frame.
void ClientApp::sync_item_preview() {
    sync_item_preview(previews_[0], conn_ ? &conn_->item_preview() : nullptr);
    sync_item_preview(previews_[1], conn_ ? &conn_->marketplace().preview : nullptr);
}

void ClientApp::sync_item_preview(PreviewSlot& slot, const net::GameConnection::ItemPreview* ip) {
    if (!ip || !ip->open || !ip->show || !ip->item) {
        if (slot.view) slot.view->clear_subject();
        slot.serial = 0xffffffffu;
        return;
    }
    if (!slot.view) {
        if (!actor_library_) return;
        try {
            slot.view = std::make_unique<ItemPreviewView>(vk_, game_, actor_library_, shader_dir_);
        } catch (const std::exception& e) {
            log::warn("item preview unavailable: {}", e.what());
            return;
        }
    }
    const game::Spawn* me = conn_->world().player();
    // 0x577700: on a copy of the player (0x5769a0) or the item's own model (0x577440); built again for a
    // new reply or when the player's own look changed.
    if (ip->serial == slot.serial && (!ip->on_player || !me || me->appearance == slot.look)) return;
    slot.serial = ip->serial;
    if (ip->on_player && me && actor_renderer_) {
        slot.look = me->appearance;
        auto choice = actor_renderer_->choose_model(*me);
        if (!choice.model) {
            slot.view->clear_subject();
            return;
        }
        const net::Item* primary = conn_->inventory().at(net::ItemSlot::possessions(net::kSlotPrimary));
        const net::Item* secondary = conn_->inventory().at(net::ItemSlot::possessions(net::kSlotSecondary));
        game::Appearance look = game::preview_appearance(me->appearance, *ip->item, primary, secondary);
        auto mesh = slot.view->library().dress(*choice.model, look);
        slot.view->set_subject(choice.model, mesh, me->heights.model_scale, 0.0f);
    } else {
        slot.view->set_item_subject(game::item_model_number(ip->item->id_file));
    }
}

void ClientApp::update_ui_host() {
    UiHost& h = ui_host_;
    h.conn = conn_.get();
    if (!h.item_preview_image) {
        h.item_preview_image = [this](int w, int ht, int view) -> void* {
            auto& slot = previews_[view == 1 ? 1 : 0];
            return slot.view ? slot.view->image(w, ht) : nullptr;
        };
        h.item_preview_control = [this](std::string_view what, float x, float y, int view) {
            auto& slot = previews_[view == 1 ? 1 : 0];
            if (slot.view) slot.view->control(what, x, y);
        };
        // Send_OP_ItemPreviewRequest @0x6ffc20 by item id (the guild bank's Alt right click, sub_0067cb80).
        h.preview_item = [this](std::uint32_t item_id) {
            if (conn_) conn_->preview_item_id(item_id);
        };
        h.plugin_caps = [this](std::string_view name) { return automation_ ? automation_->plugin_caps(name) : 0u; };
        h.plugin_consent = [this]() -> std::optional<plugin::ConsentPrompt> {
            return automation_ ? automation_->plugin_consent() : std::nullopt;
        };
        h.answer_plugin_consent = [this](std::optional<bool> allow) {
            if (automation_) automation_->answer_plugin_consent(allow);
        };
    }
    h.web = web_service_.get();
    h.scripts = automation_ ? &automation_->scripts() : nullptr;
    h.registry = automation_ ? &automation_->registry() : nullptr;
    if (!h.tlo_context && automation_) h.tlo_context = [this]() -> const mq::Context& { return automation_->context(); };
    h.in_game = screen_ == Screen::InGame && player_ready_ && conn_;
    h.ui_ready = h.in_game || screen_ == Screen::Characters;
    h.game_state = h.in_game ? 5 : screen_ == Screen::Characters ? 1 : screen_ == Screen::Create ? 2 : 0;
    h.server_select = screen_ == Screen::Servers ? &server_view_ : nullptr;
    h.login = screen_ == Screen::Login ? &login_view_ : nullptr;
    h.char_select = screen_ == Screen::Characters ? &char_view_ : nullptr;
    h.char_create_view = screen_ == Screen::Create ? &create_view_ : nullptr;
    h.login_server = screen_ == Screen::Servers || screen_ == Screen::Characters ? server_profile_.name : std::string();
    // An action left from a screen the client has since left is not taken on its next showing.
    if (!h.login) h.login_action.reset();
    if (!h.char_select) h.char_select_action.reset();
    if (!h.char_create_view) h.char_create_action.reset();
    h.char_create_open = screen_ == Screen::Create;
    if (!h.char_create_submit) {
        h.char_create_submit = [this](const std::string& name, const net::CharCreate& cc) {
            if (!conn_) return;
            conn_->create_character(name, cc);
            creating_ = true;
            create_view_.busy = true;
            create_view_.name = name;
        };
        h.char_create_cancel = [this] { screen_ = Screen::Characters; };
    }
    h.last_server = screen_ == Screen::Servers
                        ? config_.get_string("login.last_world." + (server_profile_.id.empty() ? std::string("_test") : server_profile_.id), "")
                        : std::string();
    h.character = stats_ui_ ? stats_ui_->character() : nullptr;
    // The player customization window's model (app/lua_ui_customization.cpp).
    h.char_library = actor_renderer_ ? &actor_renderer_->library() : nullptr;
    h.player_model.reset();
    if (actor_renderer_ && h.in_game)
        if (const game::Spawn* me = conn_->world().player()) h.player_model = actor_renderer_->model_for(*me);
    h.hot_buttons = actions_ui_ ? &actions_ui_->hot_buttons() : nullptr;
    h.actions = actions_ui_.get();
    h.layout_setting = [this](std::string_view key) { return lua_ui_ ? lua_ui_->setting(key) : std::string(); };
    h.set_layout_setting = [this](std::string_view key, std::string_view value) {
        if (lua_ui_) lua_ui_->set_setting(key, value);
    };
    h.heading = game::yaw_to_heading(yaw_);
    // CCompassWnd::OnProcessFrame's heading error (game/compass.h), over the base Sense Heading skill.
    if (actions_ui_) {
        const auto* ch = h.in_game ? h.character : nullptr;
        actions_ui_->compass().process(static_cast<std::uint32_t>(net::GameConnection::now_ms()),
                                       h.in_game && ui_api_ && ui_api_->shown("CompassWindow"),
                                       ch ? ch->sheet().skills[game::mechanics::skill::kSenseHeading] : -1,
                                       [] { return static_cast<std::int32_t>(std::rand() & 0x7fff); });
    }
    h.air = movement_.air_supply();
    h.max_air = movement_.max_air_supply();
    h.zone_short_name = conn_ ? conn_->world().zone_short_name : std::string();
    h.zone_long_name = conn_ ? conn_->zone_display_name() : std::string();
    h.game_hour = sky_hour_;
    h.feet_valid = h.in_game;
    h.guide_line = guide_.active() && h.in_game ? &guide_.line() : nullptr;
    if (!h.guide_to) {
        h.guide_to = [this](std::uint32_t id) {
            if (!conn_ || !conn_->npc_find_allowed(id)) return false;
            const auto& spawns = conn_->world().spawns;
            auto it = spawns.find(id);
            if (it == spawns.end()) return false;
            guide_.start(id, game::display_name(it->second.name),
                find_walk_eligibility([this] { return conn_.get(); }, *conn_, id));
            guide_zone_ = conn_->world().zone_short_name;
            return true;
        };
        h.end_guide = [this] { guide_.stop(); };
        h.walk_to = [this](std::uint32_t id) -> std::string {
            if (!nav_ || !ui_host_.guide_to || !ui_host_.guide_to(id)) return "no such NPC";
            // Close enough to talk to (within the guide path's arrival, so it says "You have reached").
            return nav_->go_to_spawn_guarded(id, 10.0f,
                find_walk_eligibility([this] { return conn_.get(); }, *conn_, id));
        };
    }
    for (int i = 0; i < 3; ++i) h.feet[i] = feet_[i];
    h.zone_box_valid = collision_ && collision_->bounds();
    if (h.zone_box_valid)
        for (int i = 0; i < 3; ++i) {
            h.zone_box_lo[i] = collision_->bounds()->first[i];
            h.zone_box_hi[i] = collision_->bounds()->second[i];
        }
    // Every frame: reload_server_files makes a new ItemUi, and the item display windows read these the next frame.
    h.item_displays = item_ui_ ? &item_ui_->item_displays() : nullptr;
    h.item_display_ids = item_ui_ ? &item_ui_->item_display_ids() : nullptr;
    if (!h.press_hot_button) {
        h.press_hot_button = [this](int bar, int button, bool mouse) {
            if (conn_ && actions_ui_) actions_ui_->hot_button(*conn_, stats_ui_ ? stats_ui_->character() : nullptr,
                                                              ability_view(), bar, button, mouse);
        };
        h.hot_button_menu = [this](int bar, int button) {
            if (conn_ && actions_ui_) actions_ui_->hot_button_right_click(*conn_, bar, button);
        };
        {
            // DoHotButton's reach outside the actions code (game/hot_button_press.h).
            HotButtonHooks hooks;
            auto screen = [](game::HotButtonWindow w) {
                return w == game::HotButtonWindow::Options ? "OptionsWindow"
                       : w == game::HotButtonWindow::Inventory ? "InventoryWindow" : "SpellBookWnd";
            };
            hooks.window_active = [this, screen](game::HotButtonWindow w) {
                if (w == game::HotButtonWindow::SpellBook && spell_ui_) return spell_ui_->book_open();
                return ui_api_ && ui_api_->shown(screen(w));
            };
            hooks.window_show = [this, screen](game::HotButtonWindow w, bool show) {
                if (w == game::HotButtonWindow::SpellBook) {
                    if (spell_ui_ && spell_ui_->book_open() != show) spell_ui_->toggle_book();
                } else if (ui_api_) {
                    ui_api_->show(screen(w), show);
                }
            };
            hooks.execute_cmd = [this](std::uint32_t command) { key_command({static_cast<int>(command), true}); };
            hooks.toggle_view = [this] { toggle_view(); };
            hooks.pet_button = [this](int button) { return ui_api_ && ui_api_->click_child("PetInfoWindow", "Pet" + std::to_string(button) + "_Button"); };
            hooks.use_item = [this](std::int32_t slot, std::int32_t bag) {
                if (ui_api_) ui_api_->right_click_item(net::ItemSlot::possessions(static_cast<std::int16_t>(slot), static_cast<std::int16_t>(bag)));
            };
            hooks.open_social_editor = [&h](std::int32_t social) {
                if (h.open_social_editor) h.open_social_editor(social);
            };
            hooks.show_aa_ability = [this](std::int32_t group) {
                if (ui_api_) ui_api_->activate_window("AAWindow", group);
            };
            hooks.spell_gem_right_click = [this](std::int32_t gem) {
                if (ui_api_) ui_api_->click_gem(gem, true);
            };
            // Lua socials (game/lua_social.h): their Lua runs in the script host with the modifiers held now.
            hooks.press_lua_social = [this](std::int32_t social, int bar, int button) {
                if (!automation_ || !actions_ui_) return;
                const game::Social& s = actions_ui_->hot_buttons().social(social);
                mq::ButtonPress p;
                p.social = social;
                p.name = s.name;
                p.lua = s.lua;
                p.bar = bar;
                p.button = button;
                const ImGuiIO& io = ImGui::GetIO();
                p.shift = io.KeyShift;
                p.ctrl = io.KeyCtrl;
                p.alt = io.KeyAlt;
                automation_->scripts().press_button(p);
            };
            hooks.stop_lua_social = [this](std::int32_t social) {
                return automation_ && automation_->scripts().stop_button(social);
            };
            hooks.edit_lua_social = [&h](std::int32_t social) { h.lua_social_edit = social; };
            hooks.forget_lua_social = [this](std::int32_t social) {
                if (automation_) automation_->scripts().forget_button(social);
            };
            actions_ui_->set_hooks(std::move(hooks));
        }
        h.hot_button_label = [this](int bar, int button) {
            if (!conn_ || !actions_ui_) return std::string();
            return actions_ui_->button_label(*conn_, actions_ui_->hot_buttons().current(bar, button));
        };
        h.set_hot_button = [this](int bar, int button, const game::HotButton& b) {
            if (actions_ui_) actions_ui_->set_hot_button(bar, button, b);
        };
        h.button_label = [this](const game::HotButton& b) {
            return conn_ && actions_ui_ ? actions_ui_->button_label(*conn_, b) : b.label;
        };
        h.toggle_spell_book = [this] {
            if (spell_ui_) spell_ui_->toggle_book();
        };
        h.run_button = [this](const game::HotButton& b) {
            if (conn_ && actions_ui_) actions_ui_->run_button(*conn_, stats_ui_ ? stats_ui_->character() : nullptr, ability_view(), b);
        };
        h.autoskill_held = [this] { return automation_ && automation_->autoskill_held(); };
        h.target_in_melee_range = [this] {
            const auto* ch = stats_ui_ ? stats_ui_->character() : nullptr;
            return conn_ && actions_ui_ && ch && actions_ui_->target_in_melee_range(*conn_, *ch, ability_view());
        };
        h.skill_name = [this](std::uint32_t skill) {
            if (!conn_ || !actions_ui_) return std::string();
            game::HotButton b;
            b.type = game::HotButtonType::Skill;
            b.slot = static_cast<std::int32_t>(skill);
            return actions_ui_->button_label(*conn_, b);
        };
        h.spell_book_open = [this] { return spell_ui_ && spell_ui_->book_open(); };
        h.drop_confirm = &drop_confirm_;
        h.game = &game_;
        h.chat = &game_view_.chat;
        h.submit_chat = [this](const std::string& line) {
            if (conn_) conn_->send_command(line);
        };
        h.click_link = [this](const game::ItemLink& link) {
            if (conn_ && item_ui_) item_ui_->link_clicked(*conn_, link);
        };
        h.inspect_item = [this](const net::Item& item) {
            if (item_ui_) item_ui_->inspect(item);
        };
        h.get_option = [this](std::string_view o) { return option_value(o).value_or(0); };
        h.find_option = [this](std::string_view o) { return option_value(o); };
        h.set_option = [this](std::string_view o, int v) { set_option_value(o, v); };
        h.option_choices = [this](std::string_view o) { return option_choices(o); };
        h.run_command = [this](int command) {
            if (conn_) key_command({command, true});
        };
        h.close_item_display = [this](int index) {
            if (item_ui_) item_ui_->close_item_display(index);
        };
    }
    wire_bug_reports(h);
}

// /luasocial [n | new]: opens the Lua social editor (the UI pack's window takes UiHost::lua_social_edit).
void ClientApp::lua_social_command(std::string_view args) {
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    auto say = [this](const std::string& text) {
        if (conn_) conn_->ui_message(text);
    };
    if (!actions_ui_) return;
    game::HotButtonStore& store = actions_ui_->hot_buttons();
    constexpr int kSocials = game::kSocialPages * game::kSocialButtons;
    int social = -1;
    if (args.empty() || args == "new") {
        for (int i = 0; i < kSocials && social < 0; ++i)
            if (store.social(i).empty()) social = i;
        if (social < 0) return say("[Lua] every social is in use");
    } else {
        int n = 0;
        auto [p, ec] = std::from_chars(args.data(), args.data() + args.size(), n);
        if (ec != std::errc() || n < 1 || n > kSocials) return say("[Lua] usage: /luasocial [1..120 | new]");
        social = n - 1;
    }
    ui_host_.lua_social_edit = social;
}

} // namespace hailstone::app::client_internal
