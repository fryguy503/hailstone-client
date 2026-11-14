#include "assets/model_overrides.h"

#include <cctype>
#include <system_error>

#include "assets/gltf_mesh.h"
#include "assets/texture_overrides.h"
#include "core/game_dir.h"
#include "core/log.h"

namespace hailstone::assets {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The actor's name without the WLD's _ACTORDEF.
std::string short_actor(std::string_view actor) {
    std::string l = lower(actor);
    if (l.ends_with("_actordef")) l.resize(l.size() - 9);
    return l;
}

std::mutex& index_mutex() {
    static std::mutex m;
    return m;
}

} // namespace

void ModelOverrides::set_folder(const std::filesystem::path& folder) {
    files_.clear();
    folder_ = folder;
    std::error_code ec;
    const std::filesystem::path models = folder / "models";
    if (folder.empty() || !std::filesystem::is_directory(models, ec)) return;
    for (std::filesystem::directory_iterator zones(models, ec), end; !ec && zones != end; zones.increment(ec)) {
        if (!zones->is_directory(ec)) continue;
        const std::string zone = lower(zones->path().filename().string());
        for (std::filesystem::directory_iterator it(zones->path(), ec); !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".glb") continue;
            // Of two files for one object (tree1.glb, TREE1_ACTORDEF.glb) the first by path wins: never the order
            // the folder happens to be listed in.
            auto [slot, added] = files_.try_emplace(zone + "/" + short_actor(it->path().stem().string()), it->path());
            if (!added && it->path() < slot->second) slot->second = it->path();
        }
    }
    if (!files_.empty()) log::info("model overrides: {} models in {}", files_.size(), models.string());
}

const std::filesystem::path* ModelOverrides::find(std::string_view zone, std::string_view actor) const {
    auto it = files_.find(lower(zone) + "/" + short_actor(actor));
    return it == files_.end() ? nullptr : &it->second;
}

ModelOverrides& model_overrides() {
    static ModelOverrides overrides;
    return overrides;
}

std::size_t apply_model_overrides(Zone& zone) {
    std::lock_guard lock(index_mutex());
    ModelOverrides& overrides = model_overrides();
    const std::filesystem::path folder = texture_overrides().folder();
    if (folder != overrides.folder()) overrides.set_folder(folder);
    if (overrides.size() == 0) return 0;
    std::size_t applied = 0;
    for (const auto& [actor, mesh] : zone.objects) {
        const std::filesystem::path* file = overrides.find(zone.name, actor);
        if (!file) continue;
        auto bytes = read_file(*file);
        std::string why = "cannot be read";
        std::optional<GltfMesh> model = bytes ? read_glb(*bytes, &why) : std::nullopt;
        if (!model) {
            log::warn("model override {}: {}; the game's is drawn", file->string(), why);
            continue;
        }
        if (model->source_hash != 0 && model->source_hash != object_mesh_hash(mesh)) {
            log::warn("model override {}: made for another {}; the game's is drawn", file->string(), actor);
            continue;
        }
        model->mesh.name = actor;
        zone.draw_objects.insert_or_assign(actor, std::move(model->mesh));
        ++applied;
    }
    if (applied) log::info("{}: {} objects drawn from model overrides", zone.name, applied);
    return applied;
}

} // namespace hailstone::assets
