#pragma once

// Replacement models (docs/graphics.md, "Model overrides"): binary glTF files in the texture pack's models/ folder,
// models/<zone>/<actor>.glb (TREE1_ACTORDEF or TREE1, any case), each drawn in place of the zone object of that name.
// The look only: collision, picking, ladders and the ambient probe keep the game's mesh (Zone::objects), so where a
// player can walk does not change. Not the original's.
//
// A file that names the mesh it was made for (asset.extras.hailstone_source, object_mesh_hash) is used only on that
// mesh; one without (made by hand) is used on any object of its name in that zone. Its textures are named by file
// (assets/gltf_mesh.h) and found as replacement textures are, a zone's subfolder first. A placement's baked light (one
// colour per vertex of the game's mesh) is averaged and multiplies the model's own vertex colours.

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "assets/zone.h"

namespace hailstone::assets {

class ModelOverrides {
public:
    // Indexes the .glb files under `folder`/models/<zone>/; an empty or missing folder turns them off.
    void set_folder(const std::filesystem::path& folder);
    const std::filesystem::path& folder() const { return folder_; }
    std::size_t size() const { return files_.size(); }
    // The file for the object `actor` (as in the WLD, e.g. TREE1_ACTORDEF) in `zone`, or null.
    const std::filesystem::path* find(std::string_view zone, std::string_view actor) const;

private:
    std::filesystem::path folder_;
    std::unordered_map<std::string, std::filesystem::path> files_; // "<zone>/<actor>", lower case
};

// The process's model overrides, indexed from the texture pack folder (texture_overrides().folder()) when it changes.
ModelOverrides& model_overrides();

// Puts the replacements for `zone`'s objects into Zone::draw_objects (from the texture pack's folder); how many.
std::size_t apply_model_overrides(Zone& zone);

} // namespace hailstone::assets
