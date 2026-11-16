#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "app/world_geometry_queries.h"
#include "assets/zone.h"
#include "game/visual_surface_patch.h"

using namespace hailstone;
using game::CollisionTriangle;
using game::VisualSurfacePatch;
using Status=game::VisualSurfacePatchStatus;

namespace {
void rectangle(std::vector<CollisionTriangle>& out,float x0,float y0,float x1,float y1,float z,
               const glm::mat4& transform=glm::mat4(1)){
    const auto point=[&](float x,float y){return glm::vec3(transform*glm::vec4(x,y,z,1));};
    const auto a=point(x0,y0),b=point(x0,y1),c=point(x1,y1),d=point(x1,y0);
    out.push_back(CollisionTriangle::make(a,b,c,0));out.push_back(CollisionTriangle::make(a,c,d,0));
}
VisualSurfacePatch large_patch(){return {{{{-100,-100,0},{100,-100,0},{-100,100,0}}},6,.01f};}
game::VisualSurfacePatchResult certify(const game::CollisionMesh& mesh,VisualSurfacePatch patch,
                                       std::uint32_t& work,const game::AnimatedObjects* animated=nullptr){
    return game::certify_visual_surface_patch(patch,
        [&](const glm::vec2& low,const glm::vec2& high,const game::VisualTriangleVisitor& visit,std::uint32_t& budget){
            if(!mesh.visual_surfaces(low,high,visit,budget))return false;
            return !animated||animated->visual_surfaces(low,high,visit,budget);
        },[&](const glm::vec3& at){return app::world_geometry_surface_floor(mesh,animated,
            [](const glm::vec3&){return std::optional<float>{};},at);},work);
}
}

TEST_CASE("visual surface patch: off-grid small props and rotated thin props cannot be certified flat"){
    for(const bool thin:{false,true}){
        std::vector<CollisionTriangle> triangles;
        rectangle(triangles,-120,-120,120,120,0);
        const auto transform=glm::translate(glm::mat4(1),glm::vec3{12.3f,-18.7f,0})*
            glm::rotate(glm::mat4(1),.71f,glm::vec3{0,0,1});
        rectangle(triangles,thin?-4.0f:-1.0f,thin?-.02f:-1.0f,thin?4.0f:1.0f,thin?.02f:1.0f,3,transform);
        const auto mesh=game::CollisionMesh::from_triangles(std::move(triangles));
        std::uint32_t work=100000;
        const auto result=certify(mesh,large_patch(),work);
        REQUIRE(result.status==Status::Mismatch);
        CHECK(result.height_known);
        CHECK(result.source_face_known);
        CHECK(result.region_count>=3);
        CHECK(result.region_count<=32);
        CHECK(result.witness.z==doctest::Approx(3));
        const auto local=glm::vec3(glm::inverse(transform)*glm::vec4(result.witness,1));
        CHECK(std::abs(local.x)<(thin?4.0f:1.0f));
        CHECK(std::abs(local.y)<(thin?.02f:1.0f));
        CHECK(work<100000);
    }
}

TEST_CASE("visual surface patch: a mismatch identifies the highest face rather than an earlier lower overlap"){
    std::vector<CollisionTriangle> triangles;
    rectangle(triangles,-120,-120,120,120,0);
    rectangle(triangles,-30,-30,-20,-20,2);
    rectangle(triangles,-30,-30,-20,-20,4);
    const auto mesh=game::CollisionMesh::from_triangles(triangles);
    std::uint32_t work=100000;
    const auto result=certify(mesh,large_patch(),work);
    REQUIRE(result.status==Status::Mismatch);
    CHECK(result.witness.z==4);
    REQUIRE(result.source_face_known);
    for(const auto vertex:result.source_face)CHECK(vertex.z==4);
}

TEST_CASE("visual surface patch: complete coplanar union certifies but duplicate coverage never fills a hole"){
    VisualSurfacePatch patch{{{{0,0,0},{10,0,0},{0,10,0}}},6,.01f};
    std::vector<CollisionTriangle> pieces;
    rectangle(pieces,0,0,10,2,0);rectangle(pieces,0,2,2,10,0);
    rectangle(pieces,4,2,10,10,0);rectangle(pieces,2,4,4,10,0); // hole x2..4,y2..4, inside the input triangle
    for(int duplicate=0;duplicate<20;++duplicate)rectangle(pieces,0,0,2,10,0);
    auto mesh=game::CollisionMesh::from_triangles(pieces);
    std::uint32_t work=100000;
    const auto missing=certify(mesh,patch,work);
    REQUIRE(missing.status==Status::Missing);
    CHECK_FALSE(missing.height_known);
    REQUIRE(missing.region_count>=3);
    for(std::size_t n=0;n<missing.region_count;++n){
        CHECK(missing.region[n].x>=2-1e-10);CHECK(missing.region[n].x<=4+1e-10);
        CHECK(missing.region[n].y>=2-1e-10);CHECK(missing.region[n].y<=4+1e-10);
    }
    CHECK(missing.witness.x>2);CHECK(missing.witness.x<4);
    CHECK(missing.witness.y>2);CHECK(missing.witness.y<4);
    rectangle(pieces,2,2,4,4,0);
    mesh=game::CollisionMesh::from_triangles(pieces);work=100000;
    CHECK(certify(mesh,patch,work).status==Status::Planar);
    rectangle(pieces,2.5f,2.5f,3.5f,3.5f,2); // still inspect higher faces after complete coplanar coverage
    mesh=game::CollisionMesh::from_triangles(pieces);work=100000;
    CHECK(certify(mesh,patch,work).status==Status::Mismatch);
}

TEST_CASE("visual surface patch: lower floors overhead roofs and exact local layer ceilings stay distinct"){
    for(const float upper:{6.0f,6.01f,42.0f}){
        std::vector<CollisionTriangle> triangles;
        rectangle(triangles,-120,-120,120,120,-8);
        rectangle(triangles,-120,-120,120,120,0);
        rectangle(triangles,-30,-30,-20,-20,upper);
        const auto mesh=game::CollisionMesh::from_triangles(std::move(triangles));
        std::uint32_t work=100000;
        const auto result=certify(mesh,large_patch(),work);
        CHECK(result.status==(upper==6?Status::Mismatch:Status::Planar));
        if(result.status==Status::Mismatch)CHECK(result.witness.z==upper);
    }
    std::vector<CollisionTriangle> lower;rectangle(lower,-120,-120,120,120,-2);
    auto mesh=game::CollisionMesh::from_triangles(lower);std::uint32_t work=100000;
    const auto result=certify(mesh,large_patch(),work);
    CHECK(result.status==Status::Mismatch);CHECK(result.witness.z==-2);
    lower.clear();rectangle(lower,-120,-120,120,120,0);
    mesh=game::CollisionMesh::from_triangles(lower);auto exact=large_patch();exact.tolerance=0;work=100000;
    CHECK(certify(mesh,exact,work).status==Status::Planar);
}

TEST_CASE("visual surface patch: work exhaustion malformed inputs and incomplete enumeration defer atomically"){
    std::vector<CollisionTriangle> triangles;rectangle(triangles,-120,-120,120,120,0);
    const auto mesh=game::CollisionMesh::from_triangles(triangles);
    for(const std::uint32_t initial:{0u,1u,5u,20u}){
        auto work=initial;CHECK(certify(mesh,large_patch(),work).status==Status::Deferred);CHECK(work<=initial);
    }
    auto query=large_patch();query.vertices[2]=query.vertices[1];std::uint32_t work=100000;
    CHECK(certify(mesh,query,work).status==Status::Deferred);
    query=large_patch();query.vertices[0].z=std::numeric_limits<float>::quiet_NaN();
    CHECK(certify(mesh,query,work).status==Status::Deferred);
    CHECK(game::certify_visual_surface_patch(large_patch(),
        [](const glm::vec2&,const glm::vec2&,const game::VisualTriangleVisitor&,std::uint32_t&){return false;},
        [](const glm::vec3&){return std::optional<float>(0.0f);},work).status==Status::Deferred);
}

TEST_CASE("visual surface patch: current nonsolid animated geometry participates in same-frame certification"){
    assets::Zone zone;
    assets::Mesh ground;
    for(const glm::vec3 p: {glm::vec3{-10,-10,0},{-10,10,0},{10,10,0},{10,-10,0}}){assets::Vertex v;v.position=p;ground.vertices.push_back(v);}
    ground.indices={0,1,2,0,2,3};ground.materials.push_back({});ground.parts.push_back({0,6,0,true});
    zone.terrain.push_back(ground);
    auto prop=ground;
    prop.vertices[0].position={1,1,2};prop.vertices[1].position={1,2,2};
    prop.vertices[2].position={2,2,2};prop.vertices[3].position={2,1,2};prop.parts[0].solid=false;
    zone.objects["PROP"]=prop;
    assets::ObjectAnimation animation;
    animation.nodes.resize(1);auto& node=animation.nodes[0];node.moves=true;node.interval_ms=100;
    node.keys.push_back({glm::quat{1,0,0,0},glm::vec3{0,0,0},1});
    node.keys.push_back({glm::quat{1,0,0,0},glm::vec3{5,0,0},1});
    node.keys.push_back({glm::quat{1,0,0,0},glm::vec3{0,0,0},1});
    animation.vertex_node.assign(4,0);animation.rest_inverse.assign(1,glm::mat4(1));animation.period_ms=200;
    zone.object_animations["PROP"]=animation;assets::Placement placement;placement.actor="PROP";zone.placements.push_back(placement);
    const auto mesh=game::CollisionMesh::build_visual(zone);auto animated=game::AnimatedObjects::build(zone);
    const VisualSurfacePatch patch{{{{0,0,0},{4,0,0},{0,4,0}}},6,.01f};
    std::uint32_t work=100000;
    CHECK(certify(mesh,patch,work,&animated).status==Status::Mismatch);
    animated.set_visual_time(100);work=100000;
    CHECK(certify(mesh,patch,work,&animated).status==Status::Planar);
    CHECK(animated.time()==0);CHECK(animated.visual_time()==100);
    CHECK_FALSE(animated.floor_below({6,1.5f,10},0)); // visual-only prop still does not block movement
}

TEST_CASE("visual surface patch: large translated coordinates and narrow clipped slivers never become false planar proofs"){
    for(const float offset:{-50000.0f,0.0f,50000.0f}){
        std::vector<CollisionTriangle> triangles;
        const auto transform=glm::translate(glm::mat4(1),glm::vec3{offset,offset,0});
        rectangle(triangles,-120,-120,120,120,0,transform);
        const auto prop=transform*glm::translate(glm::mat4(1),glm::vec3{12.3f,-18.7f,0})*
            glm::rotate(glm::mat4(1),.71f,glm::vec3{0,0,1});
        rectangle(triangles,-4,-.02f,4,.02f,3,prop);
        const auto mesh=game::CollisionMesh::from_triangles(triangles);
        auto query=large_patch();for(auto& vertex:query.vertices){vertex.x+=offset;vertex.y+=offset;}
        std::uint32_t work=100000;
        const auto result=certify(mesh,query,work);
        CHECK(result.status!=Status::Planar);
        CHECK(result.status!=Status::Missing);
        if(result.status==Status::Mismatch){CHECK(result.height_known);CHECK(result.source_face_known);}
    }
}

TEST_CASE("visual surface patch: source union does not reconstruct producer edges into false uncovered slivers"){
    std::vector<CollisionTriangle> triangles;rectangle(triangles,-120,-120,180,180,0);
    const auto mesh=game::CollisionMesh::from_triangles(triangles);
    const VisualSurfacePatch query{{{{9.748799324f,7.505578995f,0},{9.291680336f,7.104696274f,0},
                                     {8.256721497f,8.28483963f,0}}},6,.015f};
    std::uint32_t work=100000;
    const auto result=certify(mesh,query,work);
    INFO(std::string(result.diagnostic?result.diagnostic:"none"));
    CHECK(result.status==Status::Planar);
}

TEST_CASE("visual surface patch: a higher stair touching only a producer edge has no positive-area overlap"){
    std::vector<CollisionTriangle> triangles;
    rectangle(triangles,-16,0,16,4,4);rectangle(triangles,-16,4,16,8,8);
    const auto mesh=game::CollisionMesh::from_triangles(triangles);
    const VisualSurfacePatch query{{{{-12.021119117736816f,4,4},{-12.016929626464844f,4,4},
                                     {-11.924247741699219f,.50946891307830811f,4}}},6,.002f};
    std::uint32_t work=100000;
    const auto result=certify(mesh,query,work);
    INFO(std::string(result.diagnostic?result.diagnostic:"none"));
    CHECK(result.status==Status::Planar);
}
