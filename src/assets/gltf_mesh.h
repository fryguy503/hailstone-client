#pragma once

// Static meshes in glTF 2.0 binary (.glb), for replacement models (assets/model_overrides.h, docs/graphics.md "Model
// overrides"). Not a format the original reads: the look only.
//
// The file's frame is glTF's own (Y up, metres taken as EQ units, front faces counter-clockwise), turned to the zone's
// Z-up frame on reading: glTF (x, y, z) is EQ (x, -z, y). A material's texture is named by its image's URI: the file
// name stands for a texture as a game texture's name does, looked up in the texture pack (assets/texture_overrides.h);
// images inside the file and buffers outside it are refused.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "assets/model.h"

namespace hailstone::assets {

struct GltfMesh {
    Mesh mesh;
    // asset.extras.hailstone_source: texture_content_hash-style key of the mesh it was made to replace
    // (object_mesh_hash), 0 when the file has none.
    std::uint64_t source_hash = 0;
};

// Reads the default scene's meshes (each node's world transform applied), triangles only, into one Mesh with a part
// per primitive. nullopt with `error` set when the file is not a usable binary glTF.
std::optional<GltfMesh> read_glb(std::span<const std::byte> data, std::string* error = nullptr);

// Writes `mesh` as a binary glTF: one primitive per part, each material's frames[0] its image URI. Triangles go out
// counter-clockwise from the front whichever way the mesh winds them (Mesh::front_ccw).
std::vector<std::byte> write_glb(const Mesh& mesh, std::uint64_t source_hash = 0);

// A key for a zone object's geometry (positions and triangles; FNV-1a 64 over them in a fixed byte order), written
// into a replacement so that it is used only for the mesh it was made for.
std::uint64_t object_mesh_hash(const Mesh& mesh);

} // namespace hailstone::assets
