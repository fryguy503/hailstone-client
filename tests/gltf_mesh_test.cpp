// Replacement models: binary glTF read and written (assets/gltf_mesh.h) and drawn in place of zone objects
// (assets/model_overrides.h).

#include <doctest/doctest.h>

#include <fstream>

#include "assets/gltf_mesh.h"
#include "assets/model_overrides.h"
#include "assets/texture_overrides.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

// Two triangles in two parts: an opaque trunk and a two-sided cut-out leaf, wound clockwise like a WLD mesh.
Mesh sample_mesh() {
    Mesh m;
    m.name = "TREE1_ACTORDEF";
    m.front_ccw = false;
    auto vert = [](glm::vec3 p, glm::vec2 uv, std::uint32_t c) {
        Vertex v;
        v.position = p;
        v.normal = glm::normalize(glm::vec3(0.3f, -0.2f, 1.0f));
        v.uv = uv;
        v.color = c;
        return v;
    };
    m.vertices = {vert({0, 0, 0}, {0, 0}, 0xff102030), vert({0, 1, 0}, {0, 1}, 0x80405060),
                  vert({1, 0, 5}, {1, 0}, 0xffffffff), vert({2, 0, 10}, {0.5f, 0.25f}, 0x00ff00ff),
                  vert({2, 3, 10}, {0, 1}, 0xff000000), vert({4, 0, 12}, {1, 1}, 0xff00ff00)};
    m.indices = {0, 1, 2, 3, 4, 5};
    Material bark;
    bark.name = "bark";
    bark.frames = {"hstree_bark.png"};
    Material leaf;
    leaf.name = "leaf \"one\"";
    leaf.frames = {"hs leaf.png"};
    leaf.blend = BlendMode::Masked;
    leaf.two_sided = true;
    m.materials = {bark, leaf};
    m.parts = {{0, 3, 0, true}, {3, 3, 1, true}};
    return m;
}

void write_bytes(const std::filesystem::path& p, const std::vector<std::byte>& b) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

} // namespace

TEST_CASE("glTF: a written mesh reads back in the zone's frame, wound counter-clockwise") {
    const Mesh m = sample_mesh();
    const auto bytes = write_glb(m, 0x0123456789abcdefull);
    std::string error;
    auto read = read_glb(bytes, &error);
    REQUIRE_MESSAGE(read, error);
    CHECK(read->source_hash == 0x0123456789abcdefull);
    const Mesh& r = read->mesh;
    CHECK(r.front_ccw);
    REQUIRE(r.vertices.size() == m.vertices.size());
    for (std::size_t i = 0; i < m.vertices.size(); ++i) {
        CHECK(r.vertices[i].position.x == doctest::Approx(m.vertices[i].position.x));
        CHECK(r.vertices[i].position.y == doctest::Approx(m.vertices[i].position.y));
        CHECK(r.vertices[i].position.z == doctest::Approx(m.vertices[i].position.z));
        CHECK(r.vertices[i].normal.y == doctest::Approx(m.vertices[i].normal.y));
        CHECK(r.vertices[i].normal.z == doctest::Approx(m.vertices[i].normal.z));
        CHECK(r.vertices[i].uv == m.vertices[i].uv);
        CHECK(r.vertices[i].color == m.vertices[i].color);
    }
    // Clockwise in, counter-clockwise out: the second and third corners swap.
    CHECK(r.indices == std::vector<std::uint32_t>{0, 2, 1, 3, 5, 4});
    REQUIRE(r.parts.size() == 2);
    CHECK(r.parts[1].first_index == 3);
    CHECK(r.parts[1].material == 1);
    CHECK_FALSE(r.parts[0].solid);
    REQUIRE(r.materials.size() == 2);
    CHECK(r.materials[0].frames == std::vector<std::string>{"hstree_bark.png"});
    CHECK(r.materials[0].blend == BlendMode::Opaque);
    CHECK(r.materials[1].name == "leaf \"one\"");
    CHECK(r.materials[1].frames == std::vector<std::string>{"hs leaf.png"}); // URI-encoded and decoded
    CHECK(r.materials[1].blend == BlendMode::Masked);
    CHECK(r.materials[1].two_sided);
    // A counter-clockwise mesh keeps its order.
    Mesh ccw = m;
    ccw.front_ccw = true;
    auto again = read_glb(write_glb(ccw));
    REQUIRE(again);
    CHECK(again->source_hash == 0);
    CHECK(again->mesh.indices == m.indices);
}

TEST_CASE("glTF: what is not a usable binary glTF is refused") {
    std::string error;
    CHECK_FALSE(read_glb({}, &error));
    CHECK(error == "not a binary glTF file");
    auto bytes = write_glb(sample_mesh());
    std::vector<std::byte> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
    CHECK_FALSE(read_glb(cut, &error));
    // A buffer named by URI (a file beside it) is never opened.
    const std::string json =
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../../secret.bin","byteLength":12}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":12}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}]})";
    std::string padded = json;
    while (padded.size() % 4) padded += ' ';
    std::vector<std::byte> glb;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) glb.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    };
    u32(0x46546c67), u32(2), u32(static_cast<std::uint32_t>(20 + padded.size())), u32(static_cast<std::uint32_t>(padded.size())), u32(0x4e4f534a);
    for (char c : padded) glb.push_back(static_cast<std::byte>(c));
    CHECK_FALSE(read_glb(glb, &error));
    CHECK(error == "a buffer outside the file");
}

TEST_CASE("object_mesh_hash: positions and triangles count, colours and materials do not") {
    Mesh a = sample_mesh();
    Mesh b = a;
    b.vertices[0].color = 0;
    b.materials[0].frames = {"other.png"};
    CHECK(object_mesh_hash(a) == object_mesh_hash(b));
    b.vertices[2].position.z += 0.5f;
    CHECK(object_mesh_hash(a) != object_mesh_hash(b));
    Mesh c = a;
    std::swap(c.indices[0], c.indices[1]);
    CHECK(object_mesh_hash(a) != object_mesh_hash(c));
}

TEST_CASE("model overrides: a zone's objects take the pack's models, made for them, as draw_objects only") {
    test::ScratchDir dir("model_overrides");
    Zone zone;
    zone.name = "qeytoqrg";
    zone.objects["TREE1_ACTORDEF"] = sample_mesh();
    Mesh other = sample_mesh();
    other.vertices[0].position.x = 7.0f;
    zone.objects["BPINE1_ACTORDEF"] = other;
    zone.objects["HOUSE1_ACTORDEF"] = sample_mesh();
    zone.objects["SHAK_ACTORDEF"] = sample_mesh();

    Mesh replacement = sample_mesh();
    replacement.vertices.push_back(replacement.vertices[0]);
    const auto models = dir.path() / "models";
    // Made for TREE1's mesh: used. Made for TREE1's mesh but placed as BPINE1, whose mesh differs: not used.
    write_bytes(models / "qeytoqrg" / "tree1.glb", write_glb(replacement, object_mesh_hash(zone.objects["TREE1_ACTORDEF"])));
    write_bytes(models / "QEYTOQRG" / "BPINE1_ACTORDEF.glb", write_glb(replacement, object_mesh_hash(zone.objects["TREE1_ACTORDEF"])));
    // Made by hand (no source): used on any mesh of its name. Damaged: not used. Another zone's: not used here.
    write_bytes(models / "qeytoqrg" / "house1.glb", write_glb(replacement));
    write_bytes(models / "qeytoqrg" / "shak.glb", std::vector<std::byte>(40, std::byte{1}));
    write_bytes(models / "gfaydark" / "tree1.glb", write_glb(replacement));

    texture_overrides().set_folder(dir.path());
    const std::size_t applied = apply_model_overrides(zone);
    texture_overrides().set_folder({});
    CHECK(model_overrides().size() == 5);
    CHECK(applied == 2);
    CHECK(zone.draw_objects.size() == 2);
    REQUIRE(zone.draw_objects.contains("TREE1_ACTORDEF"));
    CHECK(zone.draw_objects.contains("HOUSE1_ACTORDEF"));
    CHECK(zone.draw_objects["TREE1_ACTORDEF"].vertices.size() == 7);
    CHECK(zone.draw_objects["TREE1_ACTORDEF"].name == "TREE1_ACTORDEF");
    // The game's meshes stay for collision.
    CHECK(zone.objects["TREE1_ACTORDEF"].vertices.size() == 6);

    // No pack: nothing.
    Zone plain;
    plain.name = "qeytoqrg";
    plain.objects["TREE1_ACTORDEF"] = sample_mesh();
    CHECK(apply_model_overrides(plain) == 0);
    CHECK(plain.draw_objects.empty());
}
