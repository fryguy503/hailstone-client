// The replacement painter emits the same bounded geometry that the world depth pipeline draws.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <imgui.h>

#include "app/nameplate_layer.h"
#include "game/mouse_look.h"

namespace {

struct PlateGui {
    ImGuiContext* context = ImGui::CreateContext();
    PlateGui() {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 1200);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
    }
    ~PlateGui() { ImGui::EndFrame(); ImGui::DestroyContext(context); }
    ImDrawList& draw() { return *ImGui::GetBackgroundDrawList(); }
};

hailstone::app::NameplateText replacement() {
    hailstone::app::NameplateText p;
    p.pos = {800, 100};
    p.name = "Default name";
    p.lines = {"Native owner must be replaced", "Native guild must be replaced"};
    p.presentation.emplace();
    return p;
}

void check_bounds(const ImDrawList& draw, const hailstone::app::NameplateText& p,
                  const hailstone::app::NameplateDrawStyle& style) {
    const auto size = hailstone::app::nameplate_size(p, style);
    for (const ImDrawVert& v : draw.VtxBuffer) {
        CHECK(std::isfinite(v.pos.x));
        CHECK(std::isfinite(v.pos.y));
        CHECK(v.pos.x >= p.pos.x - size.x * 0.5f - 0.51f);
        CHECK(v.pos.x <= p.pos.x + size.x * 0.5f + 0.51f);
        CHECK(v.pos.y >= p.pos.y - 0.51f);
        CHECK(v.pos.y <= p.pos.y + size.y + 0.51f);
    }
}

} // namespace

TEST_CASE("nameplate renderer: replacement hides native secondary labels and unknown resources") {
    PlateGui gui;
    auto p = replacement();
    hailstone::app::NameplateDrawStyle style;
    const auto name_only = hailstone::app::nameplate_size(p, style);
    p.lines.clear();
    CHECK(hailstone::app::nameplate_size(p, style) == name_only);
    p.presentation->health = hailstone::game::NameplateBar{72.0f, 0x2ac47b, "72%"};
    const auto health_only = hailstone::app::nameplate_size(p, style);
    CHECK(health_only.y > name_only.y);
    p.presentation->mana = hailstone::game::NameplateBar{40.0f, 0x3a83dc, ""};
    CHECK(hailstone::app::nameplate_size(p, style).y > health_only.y);
    hailstone::app::paint_nameplate(gui.draw(), p, style);
    CHECK(gui.draw().VtxBuffer.Size > 0);
    check_bounds(gui.draw(), p, style);
}

TEST_CASE("nameplate renderer: all vector symbols and long labels stay inside the measured replacement") {
    PlateGui gui;
    auto p = replacement();
    p.target = true;
    auto& a = *p.presentation;
    a.width = 120.0f;
    a.name = "A very long name that must be truncated safely";
    a.subtitle = "A long subtitle that also needs an ellipsis";
    a.health = hailstone::game::NameplateBar{0.0f, 0x24d472, "0% alive"};
    a.mana = hailstone::game::NameplateBar{100.0f, 0x3388dd, "100%"};
    a.cast = hailstone::game::NameplateBar{65.0f, 0xe4b743, "A long spell name and a timer"};
    for (const char* icon : {"eye", "peace", "sleep", "star", "sun", "moon", "diamond", "triangle", "cross", "skull", "sword", "circle"}) {
        gui.draw()._ResetForNewFrame();
        gui.draw().PushTextureID(ImGui::GetIO().Fonts->TexID);
        gui.draw().PushClipRectFullScreen();
        a.badges = {{icon, "Status label that wraps", 0xffffff}};
        hailstone::app::paint_nameplate(gui.draw(), p, {});
        REQUIRE(gui.draw().VtxBuffer.Size > 0);
        check_bounds(gui.draw(), p, {});
    }
}

TEST_CASE("nameplate renderer: multiple badges wrap and every dimension scales together") {
    PlateGui gui;
    auto p = replacement();
    auto& a = *p.presentation;
    a.width = 100.0f;
    a.health = hailstone::game::NameplateBar{15.0f, 0x33aa55, "15%"};
    a.badges = {{"eye", "Detects you", 0xffaaaa}, {"peace", "Pacified", 0x88aaff}, {"sleep", "Mezzed", 0xcc88ff},
                {"star", "", 0xffff44}, {"sun", "", 0xffaa44}, {"moon", "", 0xddddff},
                {"skull", "", 0xffffff}, {"circle", "1", 0xff88ff}};
    const auto small = hailstone::app::nameplate_size(p, {});
    a.scale = 2.0f;
    const auto large = hailstone::app::nameplate_size(p, {});
    CHECK(large.x == doctest::Approx(small.x * 2.0f));
    CHECK(large.y == doctest::Approx(small.y * 2.0f));
    hailstone::app::paint_nameplate(gui.draw(), p, {});
    check_bounds(gui.draw(), p, {});
}

TEST_CASE("nameplate renderer: bar fill follows known percentage and alpha composes with overlap") {
    PlateGui gui;
    auto p = replacement();
    p.alpha = 0.5f;
    auto& a = *p.presentation;
    a.alpha = 0.5f;
    a.show_name = false;
    a.width = 100.0f;
    a.rounding = 0.0f; // measure the percentage edge directly, without rounded-corner antialias vertices
    a.health = hailstone::game::NameplateBar{25.0f, 0x1a2b3c, ""};
    hailstone::app::paint_nameplate(gui.draw(), p, {});
    const ImU32 fill = IM_COL32(0x1a, 0x2b, 0x3c, 64);
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    int filled_vertices = 0;
    for (const ImDrawVert& v : gui.draw().VtxBuffer) {
        CHECK(((v.col >> IM_COL32_A_SHIFT) & 0xff) <= 64);
        if (v.col == fill) {
            lo = std::min(lo, v.pos.x);
            hi = std::max(hi, v.pos.x);
            ++filled_vertices;
        }
    }
    REQUIRE(filled_vertices > 0);
    CHECK(hi - lo == doctest::Approx(98.0f * 0.25f));
    check_bounds(gui.draw(), p, {});
}

TEST_CASE("nameplate renderer: a hidden replacement emits no geometry") {
    PlateGui gui;
    auto p = replacement();
    p.presentation->hide = true;
    p.presentation->health = hailstone::game::NameplateBar{100, 0xffffff, ""};
    CHECK(hailstone::app::nameplate_size(p, {}) == glm::vec2(0, 0));
    hailstone::app::paint_nameplate(gui.draw(), p, {});
    CHECK(gui.draw().VtxBuffer.Size == 0);
}

TEST_CASE("nameplate renderer: narrow plates and large fonts bound badges and all name placements") {
    PlateGui gui;
    auto p = replacement();
    auto& a = *p.presentation;
    a.width = 24.0f;
    a.font_scale = 2.0f;
    a.height = 1.0f;
    a.border_width = 4.0f;
    a.rounding = 12.0f;
    a.texture = "gradient";
    a.health = hailstone::game::NameplateBar{50, 0x228855, "A label that cannot fit"};
    a.badges = {{"eye", "Status label", 0xffffff}};
    hailstone::app::NameplateDrawStyle style;
    style.size = 48.0f;
    for (const char* position : {"above", "inside", "below"}) {
        gui.draw()._ResetForNewFrame();
        gui.draw().PushTextureID(ImGui::GetIO().Fonts->TexID);
        gui.draw().PushClipRectFullScreen();
        a.name_position = position;
        hailstone::app::paint_nameplate(gui.draw(), p, style);
        check_bounds(gui.draw(), p, style);
    }
}

TEST_CASE("nameplate renderer: stacking pins target and native captions and obeys its height limit") {
    std::vector<hailstone::game::PlateBox> boxes = {{0, 100, 100, 120, 10, true}, {0, 100, 100, 120, 20, false},
                                                  {0, 100, 100, 120, 30, false}, {200, 100, 300, 120, 40, false}};
    std::vector<hailstone::app::NameplateStack> settings = {{true, 4, 96}, {true, 4, 96}, {true, 4, 30}, {false, 4, 96}};
    hailstone::app::stack_nameplate_boxes(boxes, settings);
    CHECK(boxes[0].y0 == 100.0f);
    CHECK(boxes[1].y1 == 96.0f);
    CHECK(boxes[2].y0 == 70.0f); // at the height limit: any remaining overlap still fades
    CHECK(boxes[3].y0 == 100.0f);
    CHECK(boxes[1].depth == 20.0f);
    CHECK(boxes[2].depth == 30.0f);
    CHECK(boxes[1].x0 == 0.0f);
}

TEST_CASE("nameplate renderer: local spell art resolves within the badge box and missing art falls back") {
    PlateGui gui;
    auto p = replacement();
    p.presentation->badges = {{"circle", "Aura", 0xffffff}};
    p.presentation->badges[0].spell_icon = 417;
    hailstone::app::NameplateDrawStyle style;
    int resolutions = 0;
    style.spell_icon = [&](std::uint32_t id) -> std::optional<hailstone::app::NameplateDrawStyle::Icon> {
        ++resolutions;
        CHECK(id == 417);
        return std::nullopt;
    };
    hailstone::app::paint_nameplate(gui.draw(), p, style);
    REQUIRE(resolutions == 1);
    REQUIRE(gui.draw().VtxBuffer.Size > 0);
    check_bounds(gui.draw(), p, style);
    gui.draw()._ResetForNewFrame();
    gui.draw().PushTextureID(ImGui::GetIO().Fonts->TexID);
    gui.draw().PushClipRectFullScreen();
    int token = 0;
    const ImTextureID texture = reinterpret_cast<ImTextureID>(static_cast<void*>(&token));
    style.spell_icon = [&](std::uint32_t) -> std::optional<hailstone::app::NameplateDrawStyle::Icon> {
        return hailstone::app::NameplateDrawStyle::Icon{texture, {0.2f, 0.3f}, {0.4f, 0.5f}};
    };
    hailstone::app::paint_nameplate(gui.draw(), p, style);
    bool image = false;
    for (const ImDrawCmd& cmd : gui.draw().CmdBuffer) if (cmd.GetTexID() == texture && cmd.ElemCount == 6) image = true;
    CHECK(image);
    check_bounds(gui.draw(), p, style);
}

TEST_CASE("nameplate renderer: fitting badge labels is invariant under translation in window pixels") {
    PlateGui gui;
    auto p = replacement();
    p.presentation->show_name = false;
    p.presentation->width = 250;
    p.presentation->badges = {{"peace", "Pacified", 0xffffff}, {"sleep", "Mezzed", 0xffffff}, {"circle", "1", 0xffffff}};
    hailstone::app::NameplateDrawStyle style;
    style.size = 18;
    int vertices = 0;
    for (float x : {128.0f, 258.0f, 720.0f, 1182.0f, 1440.0f}) {
        gui.draw()._ResetForNewFrame();
        gui.draw().PushTextureID(ImGui::GetIO().Fonts->TexID);
        gui.draw().PushClipRectFullScreen();
        p.pos.x = x;
        hailstone::app::paint_nameplate(gui.draw(), p, style);
        if (vertices == 0) vertices = gui.draw().VtxBuffer.Size;
        CHECK(gui.draw().VtxBuffer.Size == vertices);
        check_bounds(gui.draw(), p, style);
    }
}

TEST_CASE("nameplate renderer: structural skins and target decorations remain inside measured bounds") {
    PlateGui gui;
    auto p = replacement();
    p.target = true;
    auto& a = *p.presentation;
    a.width = 24;
    a.height = 2;
    a.cast_width = 600;
    a.cast_gap = 24;
    a.badge_scale = 2.5f;
    a.font_scale = 2;
    a.name_position = "inside";
    a.health = hailstone::game::NameplateBar{72, 0xcc4455, "72%"};
    a.mana = hailstone::game::NameplateBar{36, 0x3388ee, "36%"};
    a.cast = hailstone::game::NameplateBar{55, 0xddbb55, "Observed spell"};
    a.badges = {{"eye", "Detects you", 0xffffff}, {"sleep", "Control", 0xcc88ff}};
    hailstone::app::NameplateDrawStyle style;
    style.size = 48;
    int texture = 0;
    const std::array<const char*, 4> textures{{"flat", "gradient", "gloss", "segments"}};
    for (const char* frame : {"minimal", "solid", "bevel", "glass", "runic"})
        for (const char* target : {"arrow", "chevrons", "brackets", "glow", "none"})
            for (const char* placement : {"above", "below", "left", "right"}) {
                CAPTURE(std::string(frame));
                CAPTURE(std::string(target));
                CAPTURE(std::string(placement));
                gui.draw()._ResetForNewFrame();
                gui.draw().PushTextureID(ImGui::GetIO().Fonts->TexID);
                gui.draw().PushClipRectFullScreen();
                a.frame_style = frame; a.target_style = target; a.badge_position = placement;
                a.texture = textures[static_cast<std::size_t>(texture++ % 4)];
                hailstone::app::paint_nameplate(gui.draw(), p, style);
                check_bounds(gui.draw(), p, style);
                const auto bounds = hailstone::app::nameplate_bounds(p, style);
                for (const auto& v : gui.draw().VtxBuffer) {
                    CHECK(v.pos.x >= bounds.x - 0.51f);
                    CHECK(v.pos.x <= bounds.z + 0.51f);
                }
            }
}

TEST_CASE("nameplate renderer: frame skins change geometry and side auras preserve the name anchor") {
    PlateGui gui;
    auto p = replacement();
    p.presentation->health = hailstone::game::NameplateBar{72, 0xcc4455, "72%"};
    std::vector<int> signatures;
    for (const char* frame : {"minimal", "solid", "bevel", "glass", "runic"}) {
        gui.draw()._ResetForNewFrame();
        gui.draw().PushTextureID(ImGui::GetIO().Fonts->TexID);
        gui.draw().PushClipRectFullScreen();
        p.presentation->frame_style = frame;
        hailstone::app::paint_nameplate(gui.draw(), p, {});
        signatures.push_back(gui.draw().VtxBuffer.Size);
    }
    std::sort(signatures.begin(), signatures.end());
    CHECK(std::unique(signatures.begin(), signatures.end()) == signatures.end());
    p.presentation->frame_style = "solid";
    const auto ordinary = hailstone::app::nameplate_bounds(p, {});
    p.presentation->badges = {{"eye", "Aura", 0xffffff}};
    p.presentation->badge_position = "right";
    const auto right = hailstone::app::nameplate_bounds(p, {});
    CHECK(right.x == ordinary.x);
    CHECK(right.z > ordinary.z);
    p.presentation->badge_position = "left";
    const auto left = hailstone::app::nameplate_bounds(p, {});
    CHECK(left.z == ordinary.z);
    CHECK(left.x < ordinary.x);
    CHECK(p.pos.x == 800.0f);
}

TEST_CASE("nameplate clicks: topmost eligible plate wins and only a short same-plate release selects") {
    using hailstone::app::NameplateHit;
    using hailstone::app::NameplateGesture;
    std::vector<NameplateHit> hits{{1, {10, 10, 100, 40}, {}, false}, {2, {20, 10, 110, 40}, {}, true}};
    auto visible = [](const NameplateHit& h) { return h.through_walls; };
    CHECK(hailstone::app::pick_nameplate(hits, {30, 20}, visible) == 2);
    CHECK_FALSE(hailstone::app::pick_nameplate(hits, {15, 20}, visible));
    CHECK_FALSE(hailstone::app::pick_nameplate(hits, {500, 500}, visible));
    NameplateGesture gesture;
    gesture.begin(2, {30, 20});
    CHECK(gesture.release({32, 21}, 2, false) == 2);
    CHECK_FALSE(gesture.id);
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, false);
    CHECK_FALSE(gesture.release({30, 20}, 2, false)); // returning after a drag is still cancelled
    gesture.begin(2, {30, 20});
    CHECK_FALSE(gesture.release({30, 20}, 1, false));
    gesture.begin(2, {30, 20});
    CHECK_FALSE(gesture.release({30, 20}, 2, true)); // UI capture, a modal or mouse look
    gesture.begin(2, {30, 20});
    gesture.clear();
    CHECK_FALSE(gesture.release({30, 20}, 2, false)); // world transition or actor removal
}

TEST_CASE("nameplate clicks: an uncaptured drag transfers to ordinary camera look without selecting") {
    hailstone::app::NameplateGesture gesture;
    hailstone::game::MouseLook look;
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, false);
    REQUIRE(gesture.transfer_drag(true, true));
    look.left_down({}, true, 100);
    CHECK(look.active);
    CHECK_FALSE(gesture.id);
    CHECK_FALSE(gesture.release({40, 20}, 2, false));
    look.left_up(500);
    CHECK_FALSE(look.active);
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, true);
    CHECK_FALSE(gesture.transfer_drag(true, true)); // UI/modal capture cannot turn into a camera drag later
    gesture.update({45, 20}, false);
    CHECK_FALSE(gesture.transfer_drag(true, true));
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, false);
    CHECK_FALSE(gesture.transfer_drag(true, false)); // isometric left drags retain their existing behavior
}

TEST_CASE("nameplate clicks: native click-through masks and self-view exclusions remain authoritative") {
    hailstone::game::MaskSpawn npc;
    npc.type = 1;
    CHECK(hailstone::app::nameplate_mouse_eligible(npc, 0, false, 6));
    npc.click_through = true;
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible(npc, 0, false, 6));
    npc.click_through = false;
    npc.mercenary = true;
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible(npc, 4, false, 6));
    npc.mercenary = false;
    npc.owner_is_player = true;
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible(npc, 2, false, 6));
    CHECK(hailstone::app::nameplate_mouse_eligible(npc, 0, false, 6));
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible({}, 0, true, 0));
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible({}, 1, true, 6));
    CHECK(hailstone::app::nameplate_mouse_eligible({}, 0, true, 6));
}

TEST_CASE("nameplate clicks: viewport mapping uses the displayed scene projection including jitter") {
    glm::mat4 inverse(1.0f);
    inverse[3] = {0, 0, 100, 1};
    const glm::vec4 view(100, 50, 400, 200);
    auto center = hailstone::app::nameplate_click_ray(inverse, view, {}, {300, 150});
    REQUIRE(center);
    CHECK(center->dir == glm::vec3(0, 0, 100));
    const auto corner = hailstone::app::nameplate_click_ray(inverse, view, {}, {100, 50});
    REQUIRE(corner);
    CHECK(corner->dir == glm::vec3(-1, 1, 100));
    inverse[3].x = 0.125f;
    center = hailstone::app::nameplate_click_ray(inverse, view, {}, {300, 150});
    REQUIRE(center);
    CHECK(center->dir.x == 0.125f);
    CHECK_FALSE(hailstone::app::nameplate_click_ray(inverse, view, {}, {99, 150}));
    CHECK_FALSE(hailstone::app::nameplate_click_ray(glm::mat4(0), view, {}, {300, 150}));
}

TEST_CASE("nameplate clicks: a pillar or visible foreground actor blocks the clicked pixel even with a clear head ray") {
    using hailstone::game::PickTriangle;
    using hailstone::game::PickTriangleSink;
    using hailstone::game::PickSegment;
    using hailstone::game::PickScene;
    const PickSegment pixel{{0, 0, 0}, {100, 50, 0}};
    const PickSegment head{{0, 0, 0}, {30, 0, 0}};
    PickScene scene;
    scene.world = [](const PickSegment&, const PickTriangleSink& sink) {
        sink(PickTriangle{{10, 4, -2}, {10, 6, -2}, {10, 6, 2}, {}, 0});
        sink(PickTriangle{{10, 4, -2}, {10, 6, 2}, {10, 4, 2}, {}, 0});
    };
    CHECK_FALSE(hailstone::app::nameplate_scene_depth(head, scene, {1, 0, 0}));
    const auto pillar = hailstone::app::nameplate_scene_depth(pixel, scene, {1, 0, 0});
    REQUIRE(pillar);
    CHECK(*pillar == doctest::Approx(10));
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, pillar));
    scene.world = {};
    std::vector<hailstone::game::PickActor> actors(1);
    actors[0].mask = hailstone::game::kPickMaskClickThrough; // unclickable actors still occlude the scene
    actors[0].bound_center = {20, 10, 0};
    actors[0].bound_radius = 4;
    actors[0].triangles = [](const PickTriangleSink& sink) {
        sink(PickTriangle{{20, 8, -2}, {20, 12, -2}, {20, 12, 2}, {}, 0});
        sink(PickTriangle{{20, 8, -2}, {20, 12, 2}, {20, 8, 2}, {}, 0});
    };
    scene.actors = actors;
    const auto actor = hailstone::app::nameplate_scene_depth(pixel, scene, {1, 0, 0});
    REQUIRE(actor);
    CHECK(*actor == doctest::Approx(20)); // axial depth, not the longer slanted-ray distance
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, actor));
    actors[0].invisible = true;
    CHECK_FALSE(hailstone::app::nameplate_scene_depth(pixel, scene, {1, 0, 0}));
    CHECK(hailstone::app::nameplate_depth_visible(30, 27.0f));
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, 26.9f));
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("nameplate clicks: a right-button join preserves normal two-button camera state without a plate click") {
    hailstone::app::NameplateGesture gesture;
    hailstone::game::MouseLook look;
    gesture.begin(2, {30, 20});
    look.left_down({}, true, 100); // replay held plate press before ordinary right_down
    gesture.clear();
    look.right_down({}, 120);
    CHECK(look.active);
    CHECK(look.both_buttons);
    CHECK_FALSE(gesture.release({30, 20}, 2, false));
    look.right_up(200);
    look.left_up(220);
    CHECK_FALSE(look.active);
    CHECK_FALSE(look.both_buttons);
}
