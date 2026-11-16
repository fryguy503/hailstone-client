#include <doctest/doctest.h>

#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "app/world_geometry_queries.h"
#include "assets/zone.h"
#include "game/object_collision.h"

using namespace hailstone;

namespace {
// WLD clockwise tops; height varies in both axes to exercise the actual triangle plane interpolation.
assets::Mesh surface(float x0, float x1, float y0, float y1, float rise, bool passable = false,
                     assets::BlendMode blend = assets::BlendMode::Opaque, bool underside = false) {
    assets::Mesh mesh;
    for (const glm::vec2 p : {glm::vec2{x0,y0}, {x0,y1}, {x1,y1}, {x1,y0}}) {
        assets::Vertex v;
        v.position = {p.x,p.y,.08f*p.x+.025f*p.y+rise};
        mesh.vertices.push_back(v);
    }
    mesh.indices = underside ? std::vector<std::uint32_t>{0,2,1,0,3,2} : std::vector<std::uint32_t>{0,1,2,0,2,3};
    mesh.materials.push_back({});
    mesh.materials.back().blend = blend;
    mesh.parts.push_back({0,6,0,!passable});
    return mesh;
}

void expect_surface(const game::CollisionMesh& mesh, float x, float y, float ceiling, float height) {
    const auto z = mesh.visual_surface_below({x,y,ceiling});
    REQUIRE(z);
    CHECK(*z == doctest::Approx(height).epsilon(.00001));
}
} // namespace

TEST_CASE("world geometry queries: scenery must match the current connection zone instance and epoch") {
    int connection_a = 0, connection_b = 0;
    const app::WorldSceneKey loaded{&connection_a, 7, 2, 100, "qeynos2"};
    CHECK(loaded.matches(loaded));
    auto current = loaded;
    current.instance = 101;
    CHECK_FALSE(loaded.matches(current)); // same archives, different world
    current = loaded;
    current.epoch = 8;
    CHECK_FALSE(loaded.matches(current)); // retained old scenery during handoff
    current = loaded;
    current.zone = 3;
    CHECK_FALSE(loaded.matches(current));
    current = loaded;
    current.short_name = "qeynos";
    CHECK_FALSE(loaded.matches(current));
    current = loaded;
    current.connection = &connection_b;
    CHECK_FALSE(loaded.matches(current));
    CHECK_FALSE(app::WorldSceneKey{}.matches(app::WorldSceneKey{}));
}

TEST_CASE("world geometry queries: drawn heading follows the rendered model axes including an attached pose") {
    for (const float heading : {0.0f, 128.0f, 256.0f, 384.0f}) {
        auto transform = glm::translate(glm::mat4(1.0f), glm::vec3(30.0f, 40.0f, 50.0f));
        transform = glm::rotate(transform, game::heading_to_yaw(heading), glm::vec3(0, 0, 1));
        transform = glm::rotate(transform, 0.6f, glm::vec3(0, 1, 0));
        transform = glm::scale(transform, glm::vec3(2.0f, 3.0f, 4.0f));
        const auto actual = app::drawn_world_heading(transform);
        REQUIRE(actual);
        CHECK(*actual == doctest::Approx(heading).epsilon(0.00001));
    }
    glm::mat4 invalid(1.0f);
    invalid[0] = glm::vec4(0, 0, 1, 0); // a vertical model front has no horizontal heading
    CHECK_FALSE(app::drawn_world_heading(invalid));
    invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(app::drawn_world_heading(invalid));
}

TEST_CASE("world geometry queries: cached floor accepts zero and native plausibility boundaries only") {
    const auto zero = app::cached_world_anchor_floor(6.0f, 0.0f, 6.0f);
    REQUIRE(zero);
    CHECK(*zero == 0.0f);
    CHECK(app::cached_world_anchor_floor(-2.0f, 0.0f, 6.0f));
    CHECK_FALSE(app::cached_world_anchor_floor(-2.01f, 0.0f, 6.0f));
    CHECK(app::cached_world_anchor_floor(100.0f, 0.0f, 6.0f));
    CHECK_FALSE(app::cached_world_anchor_floor(100.01f, 0.0f, 6.0f));
    CHECK(app::cached_world_anchor_floor(-4.0f, 0.0f, 16.0f));
    CHECK_FALSE(app::cached_world_anchor_floor(-4.01f, 0.0f, 16.0f));
    CHECK(app::cached_world_anchor_floor(128.0f, 0.0f, 16.0f));
    CHECK_FALSE(app::cached_world_anchor_floor(128.01f, 0.0f, 16.0f));
    CHECK(app::cached_world_anchor_floor(800.0f, 0.0f, 200.0f)); // height capped at100
    CHECK_FALSE(app::cached_world_anchor_floor(801.0f, 0.0f, 200.0f));
    CHECK_FALSE(app::cached_world_anchor_floor(-25.01f, 0.0f, 200.0f));
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    CHECK(app::cached_world_anchor_floor(100.0f, 0.0f, nan)); // absent/invalid avatar height uses6
    CHECK_FALSE(app::cached_world_anchor_floor(100.01f, 0.0f, 0.1f));
    CHECK_FALSE(app::cached_world_anchor_floor(nan, 0.0f, 6.0f));
    CHECK_FALSE(app::cached_world_anchor_floor(0.0f, inf, 6.0f));
}

TEST_CASE("world geometry queries: visual surfaces follow slopes low passable props and stairs within the ceiling") {
    assets::Zone zone;
    zone.terrain.push_back(surface(-34,24,-16,16,0));
    zone.terrain.push_back(surface(-22,-16,-4,4,3,true,assets::BlendMode::Masked));
    for (int step=0;step<4;++step)
        zone.terrain.push_back(surface(static_cast<float>(step)*4,step==3?24.0f:static_cast<float>(step+1)*4,
                                       -16,16,static_cast<float>(step+1)*4));
    zone.terrain.push_back(surface(8,24,-16,16,42)); // another floor, beyond the producer's ceiling
    const auto mesh = game::CollisionMesh::build(zone);
    expect_surface(mesh,-30,8,2,-30*.08f+8*.025f);
    expect_surface(mesh,-19,0,2,1.48f); // a passable, masked low rock is still drawn
    const auto movement=mesh.floor_below({-19,0,2},0);
    REQUIRE(movement);
    CHECK(*movement==doctest::Approx(-1.52f)); // unchanged movement collision ignores it
    for (int step=0;step<4;++step) {
        const float x=static_cast<float>(step)*4+2;
        const float top=x*.08f+1*.025f+static_cast<float>(step+1)*4;
        expect_surface(mesh,x,1,top+.1f,top);
        expect_surface(mesh,x,1,x*.08f+1*.025f+.1f,x*.08f+1*.025f); // below each stair stays on lower layer
    }
    expect_surface(mesh,10,0,20,12.8f);
    expect_surface(mesh,10,0,50,42.8f); // the caller can deliberately choose the upper floor
    CHECK_FALSE(mesh.visual_surface_below({50,50,50}));
}

TEST_CASE("world geometry queries: invisible lids transparent effects and underside faces never become visual ground") {
    assets::Zone zone;
    zone.terrain.push_back(surface(-10,10,-10,10,0));
    zone.terrain.push_back(surface(-10,10,-10,10,3,false,assets::BlendMode::Invisible));
    zone.terrain.push_back(surface(-10,10,-10,10,5,false,assets::BlendMode::Transparent50));
    zone.terrain.push_back(surface(-10,10,-10,10,7,false,assets::BlendMode::Opaque,true));
    const auto mesh=game::CollisionMesh::build(zone);
    expect_surface(mesh,1,1,10,.105f);
    const auto movement=mesh.floor_below({1,1,10},0);
    REQUIRE(movement);
    CHECK(*movement==doctest::Approx(7.105f)); // movement semantics deliberately remain independent
    const auto nan=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(mesh.visual_surface_below({nan,0,10}));
    CHECK_FALSE(mesh.visual_surface_below({0,0,nan}));
}

TEST_CASE("world geometry queries: actual EQG draw classes override legacy masked collision materials") {
    for(const auto shader:{assets::EqgShader::Alpha,assets::EqgShader::AddAlpha,assets::EqgShader::Water,
                          assets::EqgShader::WaterFall,assets::EqgShader::LavaH,assets::EqgShader::Unmapped}) {
        auto upper=surface(-10,10,-10,10,3,false,assets::BlendMode::Masked);
        upper.materials.front().eqg_shader=shader;
        assets::Zone zone;
        zone.terrain={surface(-10,10,-10,10,0),upper};
        const auto mesh=game::CollisionMesh::build(zone);
        expect_surface(mesh,0,0,10,0);
        CHECK(game::ObjectShape::from_mesh(upper).visual.empty());
        const auto movement=mesh.floor_below({0,0,10},0);
        REQUIRE(movement);
        CHECK(*movement==3); // the material's original collision policy stays intact
    }
    auto masked=surface(-2,2,-2,2,1,true,assets::BlendMode::Masked);
    masked.materials.front().eqg_shader=assets::EqgShader::Chroma;
    CHECK_FALSE(game::ObjectShape::from_mesh(masked).visual.empty());
}

TEST_CASE("world geometry queries: dynamic visual surfaces retain passable parts and use current transformed normals") {
    const auto shape=game::ObjectShape::from_mesh(surface(-2,2,-2,2,1,true));
    CHECK(shape.records.empty());
    CHECK(shape.tris.empty());
    REQUIRE(shape.visual.size()==2);
    auto transform=glm::translate(glm::mat4(1),glm::vec3{4,5,3});
    const auto at=glm::vec3{4,5,10};
    const auto top=game::visual_surface_under(shape,transform,at);
    REQUIRE(top);
    CHECK(*top==doctest::Approx(4));
    CHECK_FALSE(game::floor_under(shape,glm::inverse(transform),at,0));
    transform=glm::translate(glm::mat4(1),glm::vec3{4,5,3})*
              glm::rotate(glm::mat4(1),glm::pi<float>(),glm::vec3{1,0,0});
    CHECK_FALSE(game::visual_surface_under(shape,transform,at)); // same face now points down
    const auto invisible=game::ObjectShape::from_mesh(surface(-2,2,-2,2,6,false,assets::BlendMode::Invisible));
    REQUIRE_FALSE(invisible.tris.empty());
    CHECK(invisible.visual.empty());
    CHECK_FALSE(game::visual_surface_under(invisible,glm::mat4(1),{0,0,10}));
}

TEST_CASE("world geometry queries: scene floor combines real visual surfaces with current object query results") {
    const auto mesh=game::CollisionMesh::from_triangles({game::CollisionTriangle::make({-2,-2,0},{-2,2,0},{2,2,0},0)});
    const auto sample=[&](float ceiling,float moving_height) {
        return app::world_geometry_surface_floor(mesh,nullptr,[&](const glm::vec3&) {
            return std::optional<float>{moving_height};
        },{-1,0,ceiling});
    };
    REQUIRE(sample(5,3));
    CHECK(*sample(5,3)==3);
    REQUIRE(sample(5,7));
    CHECK(*sample(5,7)==0); // reject an object query that reaches above the caller's ceiling
    CHECK(*sample(5,std::numeric_limits<float>::infinity())==0);
}

TEST_CASE("world geometry queries: visual object replacements match the renderer while physics keeps original geometry") {
    assets::Zone zone;
    zone.terrain.push_back(surface(-10,10,-10,10,0));
    zone.objects["PROP"]=surface(-2,2,-2,2,2);
    zone.draw_objects["PROP"]=surface(-2,2,-2,2,8,true);
    assets::Placement placement;placement.actor="PROP";zone.placements.push_back(placement);
    const auto physics=game::CollisionMesh::build(zone);
    const auto visual=game::CollisionMesh::build_visual(zone);
    expect_surface(physics,0,0,20,2);
    expect_surface(visual,0,0,20,8);
    const auto movement=physics.floor_below({0,0,20},0);
    REQUIRE(movement);
    CHECK(*movement==2);
}
