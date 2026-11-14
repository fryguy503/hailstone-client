// Classic character models: WLD skeleton + skinned mesh + animation clips.
//
// The clip assembly (clip codes found by bone-track name, clips borrowed
// from an animation donor only when the model has no track of its own for
// that clip, per-bone fallback to the rest pose, 100 ms default frame time)
// and the rigid per-vertex bone binding follow OpenEQ
// crates/openeq-assets/src/character.rs (MIT OR Apache-2.0); see
// THIRD_PARTY.md. The archive load order follows the original client's
// start-up loads.

#include "assets/character.h"
#include "platform/process.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

#include "assets/eqg_character.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/msvc_atoi.h"
#include "core/log.h"
#include "game/appearance_data.h"
#include "game/race_models.h"

namespace hailstone::assets {

std::uint64_t MeshSerial::next() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(std::string_view line, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        std::size_t p = line.find(sep, start);
        out.push_back(trim(line.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start)));
        if (p == std::string_view::npos) break;
        start = p + 1;
    }
    return out;
}

std::vector<std::string> text_lines(const std::vector<std::byte>& data) {
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

// "globalhum_chr.s3d" / "globalhum_chr2.s3d": a Luclin player-model archive.
// globalpcfroglok_chr.s3d holds the player Froglok, built the same way
// (attachment points, "<code><part><material><variant>1_MDF" materials).
bool is_luclin_archive(std::string_view label) {
    auto slash = label.find_last_of("/\\");
    if (slash != std::string_view::npos) label = label.substr(slash + 1);
    if (label == "globalpcfroglok_chr.s3d") return true;
    return label.size() >= 17 && label.starts_with("global") &&
           (label.substr(9) == "_chr.s3d" || label.substr(9) == "_chr2.s3d") &&
           std::all_of(label.begin() + 6, label.begin() + 9, [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
}

// A clip code: a letter and two digits (C01, L02, P01 ...).
bool is_clip_code(std::string_view s) {
    return s.size() >= 3 && std::isupper(static_cast<unsigned char>(s[0])) &&
           std::isdigit(static_cast<unsigned char>(s[1])) && std::isdigit(static_cast<unsigned char>(s[2]));
}

BonePose to_pose(const wld::TrackFrame& f) {
    BonePose p;
    glm::quat q = f.rotation;
    float len = glm::length(q);
    p.rotation = std::isfinite(len) && len > 1e-6f ? q / len : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    p.translation = f.translation;
    p.scale = glm::vec3(std::isfinite(f.scale) ? f.scale : 1.0f);
    return p;
}

glm::mat4 pose_matrix(const BonePose& p) {
    glm::mat4 r = glm::mat4_cast(p.rotation);
    r[0] *= p.scale.x;
    r[1] *= p.scale.y;
    r[2] *= p.scale.z;
    r[3] = glm::vec4(p.translation, 1.0f);
    return r;
}

BonePose blend(const BonePose& a, const BonePose& b, float t) {
    BonePose p;
    p.rotation = glm::normalize(glm::slerp(a.rotation, b.rotation, t));
    p.translation = glm::mix(a.translation, b.translation, t);
    p.scale = glm::mix(a.scale, b.scale, t);
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// CharacterModel

const AnimationClip* CharacterModel::clip(std::string_view c) const {
    auto it = clips.find(c);
    if (it == clips.end())
        if (auto a = clip_aliases.find(c); a != clip_aliases.end()) it = clips.find(a->second);
    return it == clips.end() ? nullptr : &it->second;
}

const AnimationClip* CharacterModel::clip(std::string_view c, bool holding) const {
    if (holding)
        if (auto a = held_aliases.find(c); a != held_aliases.end())
            if (auto it = clips.find(a->second); it != clips.end()) return &it->second;
    return clip(c);
}

const AnimationClip& CharacterModel::first_clip(std::initializer_list<std::string_view> codes, bool holding) const {
    for (auto c : codes)
        if (const auto* a = clip(c, holding)) return *a;
    return clips.at("");
}

void CharacterModel::sample_local(const AnimationClip& a, float time_seconds, bool loop,
                                  std::vector<BonePose>& out) const {
    out.resize(a.tracks.size());
    float t = std::isfinite(time_seconds) ? std::max(time_seconds, 0.0f) : 0.0f;
    float phase = t * 1000.0f / static_cast<float>(std::max<std::uint32_t>(a.frame_ms, 1));
    float count = static_cast<float>(a.frame_count);
    phase = loop ? std::fmod(phase, count) : std::min(phase, count - 1.0f);
    auto frame = static_cast<std::size_t>(phase);
    float frac = phase - static_cast<float>(frame);
    for (std::size_t b = 0; b < a.tracks.size(); ++b) {
        const auto& keys = a.tracks[b];
        if (keys.size() == 1) {
            out[b] = keys.front();
            continue;
        }
        auto index = [&](std::size_t f) { return loop ? f % keys.size() : std::min(f, keys.size() - 1); };
        out[b] = blend(keys[index(frame)], keys[index(frame + 1)], frac);
    }
}

void CharacterModel::compose(std::span<const BonePose> local, std::vector<glm::mat4>& out) const {
    out.assign(parents.size(), glm::mat4(1.0f));
    for (auto b : order) {
        glm::mat4 m = b < local.size() ? pose_matrix(local[b]) : glm::mat4(1.0f);
        out[b] = parents[b] < 0 ? m : out[static_cast<std::size_t>(parents[b])] * m;
    }
}

void CharacterModel::sample(const AnimationClip& a, float time_seconds, bool loop, std::vector<glm::mat4>& out) const {
    std::vector<BonePose> local;
    sample_local(a, time_seconds, loop, local);
    compose(local, out);
}

void CharacterModel::skin(std::vector<glm::mat4>& bones) const {
    for (std::size_t b = 0; b < bones.size() && b < inverse_bind.size(); ++b) bones[b] = bones[b] * inverse_bind[b];
}

int CharacterModel::find_bone(std::string_view suffix) const {
    std::string exact = code + std::string(suffix);
    for (std::size_t i = 0; i < bone_names.size(); ++i)
        if (upper(bone_names[i]) == exact) return static_cast<int>(i);
    // A borrowed skeleton can keep its donor's code. Match the whole suffix
    // after three letters only: "HAIR_POINT" must not match "R_POINT".
    for (std::size_t i = 0; i < bone_names.size(); ++i) {
        std::string n = upper(bone_names[i]);
        if (n.size() == suffix.size() + 3 && n.ends_with(suffix)) return static_cast<int>(i);
    }
    return -1;
}

int CharacterModel::find_bone_prefix(std::string_view name) const {
    std::string prefix = upper(code) + std::string(name) + "_";
    for (std::size_t i = 0; i < bone_names.size(); ++i)
        if (upper(bone_names[i]).starts_with(prefix)) return static_cast<int>(i);
    std::string rest = std::string(name) + "_";
    for (std::size_t i = 0; i < bone_names.size(); ++i) {
        std::string n = upper(bone_names[i]);
        if (n.size() > 3 + rest.size() && n.compare(3, rest.size(), rest) == 0) return static_cast<int>(i);
    }
    return -1;
}

int CharacterModel::mount_point_bone(std::string_view rider_code) const {
    std::string exact = upper(std::string(rider_code)) + "_MOUNT01";
    for (std::size_t i = 0; i < bone_names.size(); ++i)
        if (upper(bone_names[i]) == exact) return static_cast<int>(i);
    for (std::size_t i = 0; i < bone_names.size(); ++i)
        if (upper(bone_names[i]) == "CHEST03_MOUNT01") return static_cast<int>(i);
    return find_bone_prefix("RIDER");
}

bool CharacterMesh::append_skin_bones(std::vector<glm::mat4>& bones) const {
    const auto body_count = bones.size();
    for (const auto& binding : skin_bindings)
        if (binding.host_bone >= body_count) return false;
    bones.reserve(body_count + skin_bindings.size());
    for (const auto& binding : skin_bindings) bones.push_back(bones[binding.host_bone] * binding.local);
    return true;
}

CharacterMesh CharacterModel::default_mesh() const {
    CharacterMesh m;
    m.vertices = vertices;
    m.indices = indices;
    m.parts = parts;
    m.materials = materials;
    m.tints.assign(materials.size(), 0);
    return m;
}

namespace detail {

void append_wld_mesh(const wld::WldFile& w, const wld::Fragment& mf, std::size_t n, std::int64_t owner,
                     CharacterMesh& out) {
    const auto* md = mf.get<wld::MeshDef>();
    if (!md) return;
    // The pieces' counts are checked against the vertices before any is expanded: a forged count must not make a
    // vector of billions of bone indices.
    std::uint64_t covered = 0;
    for (const auto& [count, bone] : md->vertex_pieces) {
        if (bone >= n) throw std::runtime_error(mf.name + ": vertex piece names bone " + std::to_string(bone));
        covered += count;
    }
    if (!md->vertex_pieces.empty() && covered != md->positions.size())
        throw std::runtime_error(mf.name + ": vertex pieces do not cover the vertices");
    std::vector<std::uint32_t> bones;
    bones.reserve(md->positions.size());
    for (const auto& [count, bone] : md->vertex_pieces) bones.insert(bones.end(), count, bone);
    if (md->vertex_pieces.empty()) {
        if (owner < 0) throw std::runtime_error(mf.name + ": mesh has no vertex pieces and no node");
        bones.assign(md->positions.size(), static_cast<std::uint32_t>(owner));
    }
    if (bones.size() != md->positions.size())
        throw std::runtime_error(mf.name + ": vertex pieces do not cover the vertices");

    Mesh mesh = wld::build_mesh(w, *md, mf.name);
    auto vbase = static_cast<std::uint32_t>(out.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& v = mesh.vertices[i];
        SkinnedVertex sv;
        sv.position = v.position;
        sv.normal = v.normal;
        sv.uv = v.uv;
        sv.bones[0] = static_cast<std::uint16_t>(bones[i]);
        out.vertices.push_back(sv);
    }
    for (const auto& part : mesh.parts) {
        const Material& mat = mesh.materials[part.material];
        // Share material entries between meshes (body and head use one palette).
        std::uint32_t mi = 0;
        for (; mi < out.materials.size(); ++mi)
            if (out.materials[mi].name == mat.name && out.materials[mi].frames == mat.frames) break;
        if (mi == out.materials.size()) {
            out.materials.push_back(mat);
            out.tints.push_back(0);
        }
        MeshPart p;
        p.first_index = static_cast<std::uint32_t>(out.indices.size());
        p.index_count = part.index_count;
        p.material = mi;
        for (std::uint32_t k = 0; k < part.index_count; ++k)
            out.indices.push_back(vbase + mesh.indices[part.first_index + k]);
        out.parts.push_back(p);
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Archives

std::size_t CharacterArchive::archive_bytes() const { return archive ? archive->storage_bytes() : 0; }

std::size_t CharacterArchive::owned_bytes() const { return archive ? archive->owned_bytes() : 0; }

std::size_t CharacterArchive::parsed_bytes() const {
    std::size_t total = 0;
    for (const auto& w : wlds) total += w.retained_bytes();
    return total;
}

std::shared_ptr<const CharacterArchive> load_character_archive(const GameDir& game, std::string_view file) {
    auto path = game.find(file);
    if (!path) return nullptr;
    auto cache = game.shared_file_cache();
    auto out = std::make_shared<CharacterArchive>();
    out->archive = std::make_shared<const PfsArchive>(PfsArchive::open(*path, PfsArchive::storage_for(game, *path), cache));
    for (const auto& member : out->archive->names()) {
        if (!member.ends_with(".wld")) continue;
        auto source = out->archive;
        out->wlds.push_back(wld::WldFile::deferred([source, member, cache]() -> ImmutableFileBytes {
            auto bytes = source->read(member);
            if (!bytes) throw std::runtime_error("missing character WLD " + member);
            return cache ? cache->store(std::move(*bytes)) : ImmutableFileBytes::owned(std::move(*bytes));
        }, source->label() + ":" + member));
    }
    return out;
}

std::vector<std::string> luclin_character_archives(const GameDir& game) {
    // ShouldUseLuclinModel @0048e510: eqclient.ini [Defaults]
    // AllLuclinPcModelsOff, UseLuclin<Race><Male|Female> (default TRUE).
    struct Race {
        const char* ini;
        const char* male;
        const char* female;
    };
    static constexpr Race kRaces[] = {
        {"Human", "hum", "huf"},    {"Barbarian", "bam", "baf"}, {"Erudite", "erm", "erf"},
        {"WoodElf", "elm", "elf"},  {"HighElf", "him", "hif"},   {"DarkElf", "dam", "daf"},
        {"HalfElf", "ham", "haf"},  {"Dwarf", "dwm", "dwf"},     {"Troll", "trm", "trf"},
        {"Ogre", "ogm", "ogf"},     {"Halfling", "hom", "hof"},  {"Gnome", "gnm", "gnf"},
        {"Iksar", "ikm", "ikf"},    {"VahShir", "kem", "kef"},
    };
    std::unordered_map<std::string, bool> keys;
    if (auto data = game.read("eqclient.ini")) {
        bool defaults = false;
        for (const auto& raw : text_lines(*data)) {
            std::string line = trim(raw);
            if (line.starts_with('[')) {
                defaults = lower(line) == "[defaults]";
                continue;
            }
            auto eq = line.find('=');
            if (!defaults || eq == std::string::npos) continue;
            std::string v = lower(trim(line.substr(eq + 1)));
            keys[lower(trim(line.substr(0, eq)))] = !v.empty() && (v[0] == 't' || v[0] == '1');
        }
    }
    auto flag = [&](const std::string& key, bool def) {
        auto it = keys.find(lower(key));
        return it == keys.end() ? def : it->second;
    };
    std::vector<std::string> out;
    if (flag("AllLuclinPcModelsOff", false)) return out;
    for (const auto& r : kRaces)
        for (int g = 0; g < 2; ++g) {
            if (!flag(std::string("UseLuclin") + r.ini + (g ? "Female" : "Male"), true)) continue;
            std::string code = g ? r.female : r.male;
            for (const char* suffix : {"_chr2.s3d", "_chr.s3d"}) {
                std::string file = "global" + code + suffix;
                if (game.exists(file)) out.push_back(file);
            }
        }
    // The modular parts (hair, beards, helms, robes, plate) the Luclin
    // models attach.
    if (!out.empty())
        for (const char* f : {"lgequip.s3d", "lgequip2.s3d", "lgequip_amr.s3d", "lgequip_amr2.s3d", "frogequip.s3d"})
            if (game.exists(f)) out.push_back(f);
    return out;
}

bool chr_txt_entry_is_s3d(const GameDir& game, std::string_view code, std::string_view file) {
    if (lower(code).substr(0, 3) != lower(file).substr(0, 3)) return true;
    // The live client's lists name S3Ds that share the code's letters too ("kob,kob_chr" for kob_chr.s3d), where
    // RoF2's always name an EQG ("kob,kob"): such a file is taken as an S3D when one exists by that name.
    return game.exists(lower(file) + ".s3d");
}

std::vector<std::string> global_character_archives(const GameDir& game, bool luclin) {
    IniFile ini;
    if (auto bytes = game.read("eqclient.ini"))
        ini.parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    const bool elementals = eqclient_ini_bool(ini.get("Defaults", "UseLuclinElementals"), true);
    std::vector<std::string> out;
    auto add = [&](std::string name) {
        name = lower(name);
        if (name.find('.') == std::string::npos) name += ".s3d";
        if (!name.ends_with(".s3d")) return;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    };
    // GlobalLoad.txt: "phase,?,flags,file,text", loaded phase by phase. The
    // flags' fifth letter is C for characters and E for equipment (IT models).
    std::vector<std::string> luclin_files = luclin ? luclin_character_archives(game) : std::vector<std::string>{};
    if (auto data = game.read("Resources/GlobalLoad.txt")) {
        std::vector<std::pair<int, std::string>> entries;
        for (const auto& line : text_lines(*data)) {
            auto f = split(line, ',');
            if (f.size() < 4 || f[3].empty()) continue;
            std::string file = lower(f[3]);
            bool equipment = f[2].size() >= 5 && (f[2][4] == 'E' || f[2][4] == 'e');
            if (file.find(".eqg") != std::string::npos) continue;
            if (file.find("_chr") == std::string::npos && !equipment) continue;
            entries.emplace_back(core::msvc_atoi(f[0]), file);
        }
        std::stable_sort(entries.begin(), entries.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        // The Luclin models load after phase 2 (ConfigureLuclinModels), so
        // their actors come before global_chr's classic ones.
        bool luclin_added = false;
        for (auto& [phase, file] : entries) {
            if (phase >= 3 && !luclin_added) {
                for (const auto& l : luclin_files) add(l);
                luclin_added = true;
            }
            add(file);
        }
        if (!luclin_added)
            for (const auto& l : luclin_files) add(l);
    } else {
        for (const auto& l : luclin_files) add(l);
        add("global_chr");
    }
    // Loaded by ConfigureLuclinModels next to the Luclin models, then the
    // extended armour textures (Global%d_amr).
    // The "Global NPC Models" load (@0x492780) skips Global5_chr2, Global5_chr and frog_mount_chr when
    // UseLuclinElementals is off, the option CreatePlayerActor's race swap obeys. Only this implicit load is gated:
    // whether an entry the player's GlobalLoad.txt names explicitly is skipped too is not confirmed, so it still
    // loads (docs/unknowns.md).
    if (elementals) add("global5_chr");
    add("global7_chr");
    for (int i = 17; i <= 23; ++i) add("global" + std::to_string(i) + "_amr");
    // GlobalLoad_chr.txt: count, then "code,file" lines (ChrTxt_Load). When
    // code and file share their first three letters the file is an EQG (see
    // chr_txt_entry_is_s3d for the live client's lists).
    if (auto data = game.read("Resources/GlobalLoad_chr.txt")) {
        auto lines = text_lines(*data);
        for (std::size_t i = 1; i < lines.size(); ++i) {
            auto f = split(lines[i], ',');
            if (f.size() < 2 || f[1].empty()) continue;
            if (!chr_txt_entry_is_s3d(game, f[0], f[1])) continue;
            add(f[1]);
        }
    }
    std::erase_if(out, [&](const std::string& n) { return !game.exists(n); });
    return out;
}

std::vector<std::string> zone_character_archives(const GameDir& game, std::string_view zone_view) {
    std::vector<std::string> out;
    std::string zone = lower(zone_view);
    if (zone.empty()) return out;
    auto add = [&](const std::string& name) {
        if (game.exists(name) && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    add(zone + "_chr2.s3d");
    add(zone + "2_chr.s3d");
    add(zone + "_chr.s3d");
    if (auto data = game.read(zone + "_chr.txt")) {
        auto lines = text_lines(*data);
        for (std::size_t i = 1; i < lines.size(); ++i) {
            auto f = split(lines[i], ',');
            if (f.size() < 2 || f[1].empty()) continue;
            if (!chr_txt_entry_is_s3d(game, f[0], f[1])) continue; // EQG
            add(lower(f[1]) + ".s3d");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// CharacterLibrary

namespace {

// The resource table's rule for one more definition of `key` at `level`:
// dropped when the name already has one at that level, otherwise it becomes
// the one found (a later level wins). Returns whether it was taken. A name new to `levels` starts from the levels
// `base` (the tables these go on from) has for it.
template <typename Map, typename Levels, typename Value>
bool add_resource(Map& map, Levels& levels, const std::type_identity_t<Levels>* base,
                  const typename Map::key_type& key, int level,
                  Value&& value) {
    std::uint64_t bit = std::uint64_t{1} << (static_cast<unsigned>(level) & 63u);
    auto [seen, fresh] = levels.try_emplace(key, 0);
    if (fresh && base)
        if (auto b = base->find(key); b != base->end()) seen->second = b->second;
    if (seen->second & bit) return false;
    seen->second |= bit;
    map.insert_or_assign(key, std::forward<Value>(value));
    return true;
}

} // namespace

constinit thread_local CharacterLookups* CharacterLookups::current_ = nullptr;

CharacterLookups::~CharacterLookups() {
    current_ = parent_;
    if (parent_) parent_->names_.insert(names_.begin(), names_.end());
}

CharacterLookups::Names CharacterLookups::names() const {
    auto out = std::make_shared<std::vector<std::string>>(names_.begin(), names_.end());
    std::sort(out->begin(), out->end());
    return out;
}

void CharacterLookups::note(const Names& names) {
    if (current_ && names) current_->names_.insert(names->begin(), names->end());
}

void CharacterLookups::note_one(char table, std::string_view name) {
    std::string key;
    key.reserve(name.size() + 1);
    key += table;
    key += name;
    names_.insert(std::move(key));
}

void SharedCharacterCache::next_zone() {
    auto stale = [this](const auto& kv) { return kv.second.used < generation_; };
    std::erase_if(models, [&](const auto& kv) {
        if (!stale(kv)) return false;
        owners.erase(kv.second.value.get());
        return true;
    });
    std::erase_if(dresses, [&](const auto& kv) {
        // Its model gone too: its key's address may be another model's later.
        if (!stale(kv) && owners.contains(kv.first.first)) return false;
        owners.erase(kv.second.value.get());
        return true;
    });
    std::erase_if(items, stale);
    ++generation_;
}

bool CharacterLibrary::unaffected(const std::vector<std::string>& names) const {
    if (zone_names_.empty()) return true;
    return std::none_of(names.begin(), names.end(), [&](const std::string& n) { return zone_names_.contains(n); });
}

bool CharacterLibrary::shares(const void* p) const {
    Lock lock(*mutex_);
    if (!shared_ || !p) return false;
    auto it = shared_->owners.find(p);
    if (it == shared_->owners.end() || !unaffected(it->second.lookups)) return false;
    return !it->second.model || shares(it->second.model);
}

namespace detail {

void CharacterTables::add(const CharacterArchive& ca, std::size_t ai, int level, const CharacterTables* base,
                          std::unordered_set<std::string>* names) {
    // A zone archive's names, whether or not they win over a global one's (conservative: an entry that looked one
    // up is not shared here).
    auto zone_name = [&](char table, const std::string& name) {
        if (names) names->insert(table + name);
    };
    // A bone's clip list, from base's when these tables have none yet.
    auto clip_list = [&](const std::string& bone) -> std::vector<std::string>& {
        auto [it, fresh] = clips_by_bone.try_emplace(bone);
        if (fresh && base)
            if (auto b = base->clips_by_bone.find(bone); b != base->clips_by_bone.end()) it->second = b->second;
        return it->second;
    };
    for (const auto& name : ca.archive->names())
        if (!name.ends_with(".wld")) {
            files.try_emplace(lower(name), ai);
            zone_name('f', lower(name));
        }
    std::string label = lower(ca.archive->label());
    // The equipment archives hold Luclin materials too: the eye colours CHR_EYE<nnn>_MDF (lgequip, and
    // the Froglok's in frogequip) that ActorClient::sub_0040c350 puts on the eyes.
    const bool equip = label.starts_with("lgequip") || label.starts_with("frogequip");
    bool luclin_wld = is_luclin_archive(label) || label.find("_amr") != std::string::npos || equip;
    for (const auto& w : ca.wlds) {
        w.visit_character_index([&](wld::Ref ref, std::uint32_t type, const std::string& name, const wld::TrackRef* t) {
            if (name.empty()) return;
            if (luclin_wld && type == 0x30) {
                luclin_materials.try_emplace(upper(name), FragmentLoc{&w, ref});
                zone_name('l', upper(name));
            }
            if (type == 0x14) {
                constexpr std::string_view kSuffix = "_ACTORDEF";
                std::string actor_name = upper(name);
                if (!actor_name.ends_with(kSuffix)) return;
                std::string code = actor_name.substr(0, actor_name.size() - kSuffix.size());
                zone_name(equip ? 'p' : 'a', code);
                // Luclin modular parts (hair, beards, helms, robes, plate)
                // are IT<n> actors of the lgequip archives; their numbers
                // overlap ordinary items, so they are kept apart.
                if (equip)
                    add_resource(luclin_parts, part_levels, base ? &base->part_levels : nullptr, code, level,
                                 FragmentLoc{&w, ref});
                else {
                    add_resource(actors, actor_levels, base ? &base->actor_levels : nullptr, code, level,
                                 FragmentLoc{&w, ref});
                    actor_wlds.try_emplace(std::pair{&w, code}, FragmentLoc{&w, ref});
                }
            } else if (type == 0x36) {
                add_resource(meshes, mesh_levels, base ? &base->mesh_levels : nullptr, upper(name), level,
                             FragmentLoc{&w, ref});
                zone_name('m', upper(name));
            } else if (t) {
                zone_name('t', name);
                if (is_clip_code(name) && name.size() > 3) zone_name('c', name.substr(3));
                if (is_clip_code(name) && name.size() > 4) zone_name('c', name.substr(4));
                if (!add_resource(tracks, track_levels, base ? &base->track_levels : nullptr, name, level,
                                  TrackLoc{&w, *t}))
                    return;
                if (is_clip_code(name) && name.size() > 3) {
                    auto& clips = clip_list(name.substr(3));
                    std::string code = name.substr(0, 3);
                    if (std::find(clips.begin(), clips.end(), code) == clips.end()) clips.push_back(code);
                    // Luclin models name their takes with a fourth letter
                    // (C01A, L10B): index that reading too.
                    if (name.size() > 4 && (name[3] == 'A' || name[3] == 'B')) {
                        auto& luclin = clip_list(name.substr(4));
                        std::string code4 = name.substr(0, 4);
                        if (std::find(luclin.begin(), luclin.end(), code4) == luclin.end())
                            luclin.push_back(code4);
                    }
                }
            }
        });
    }
}

} // namespace detail

CharacterArchiveIndex::CharacterArchiveIndex(std::vector<std::shared_ptr<const CharacterArchive>> archives,
                                             std::vector<int> levels)
    : archives_(std::move(archives)) {
    for (std::size_t ai = 0; ai < archives_.size(); ++ai)
        tables_.add(*archives_[ai], ai, ai < levels.size() ? levels[ai] : kZoneResourceLevel, nullptr, nullptr);
}

void CharacterArchiveIndex::swap(CharacterArchiveIndex& other) noexcept {
    // Both sets of archives stay owned until their borrowed table keys have changed owners too.
    using std::swap;
    swap(archives_, other.archives_);
    swap(tables_, other.tables_);
}

void CharacterLibrary::swap(CharacterLibrary& other) noexcept {
    using std::swap;
    swap(archives_, other.archives_);
    swap(globals_, other.globals_);
    swap(tables_, other.tables_);
    swap(zone_names_, other.zone_names_);
    swap(mutex_, other.mutex_);
    swap(cache_, other.cache_);
    swap(dress_cache_, other.dress_cache_);
    swap(item_cache_, other.item_cache_);
    swap(shared_, other.shared_);
    swap(eqg_, other.eqg_);
    swap(own_, other.own_);
    swap(appearance_, other.appearance_);
}

CharacterLibrary::CharacterLibrary(std::vector<std::shared_ptr<const CharacterArchive>> archives,
                                   std::vector<int> levels, std::size_t first_zone_archive)
    : archives_(std::move(archives)) {
    for (std::size_t ai = 0; ai < archives_.size(); ++ai)
        tables_.add(*archives_[ai], ai, ai < levels.size() ? levels[ai] : kZoneResourceLevel, nullptr,
                    ai >= first_zone_archive ? &zone_names_ : nullptr);
}

CharacterLibrary::CharacterLibrary(std::shared_ptr<const CharacterArchiveIndex> globals,
                                   std::vector<std::shared_ptr<const CharacterArchive>> zone_archives)
    : globals_(std::move(globals)) {
    const auto* base = globals_ ? &globals_->tables_ : nullptr;
    if (globals_) archives_ = globals_->archives_;
    const std::size_t first = archives_.size();
    archives_.insert(archives_.end(), zone_archives.begin(), zone_archives.end());
    for (std::size_t ai = first; ai < archives_.size(); ++ai)
        tables_.add(*archives_[ai], ai, kZoneResourceLevel, base, &zone_names_);
}

std::vector<std::shared_ptr<const CharacterArchive>> CharacterLibrary::open_archives(
    const GameDir& game, const std::vector<std::string>& names) {
    std::vector<std::shared_ptr<const CharacterArchive>> list;
    for (const auto& n : names) {
        try {
            if (auto a = load_character_archive(game, n)) list.push_back(std::move(a));
        } catch (const std::exception& e) {
            log::warn("character archive {}: {}", n, e.what());
        }
    }
    return list;
}

std::unordered_map<std::string, int> global_archive_levels(const GameDir& game) {
    std::unordered_map<std::string, int> out;
    if (auto data = game.read("Resources/GlobalLoad.txt"))
        for (const auto& line : text_lines(*data)) {
            auto f = split(line, ',');
            if (f.size() < 4 || f[3].empty()) continue;
            std::string file = lower(f[3]);
            if (file.find('.') == std::string::npos) file += ".s3d";
            bool t = !f[2].empty() && (f[2][0] == 'T' || f[2][0] == 't');
            out.try_emplace(file, t ? kGlobalResourceLevel : kZoneResourceLevel);
        }
    return out;
}

namespace {

// Each opened global archive's level (global_archive_levels; 0 when unlisted).
std::vector<int> levels_of(const std::vector<std::shared_ptr<const CharacterArchive>>& list,
                           const std::unordered_map<std::string, int>& by_name) {
    std::vector<int> out;
    out.reserve(list.size());
    for (const auto& a : list) {
        std::string label = lower(a->archive->label());
        if (auto slash = label.find_last_of("/\\"); slash != std::string::npos) label = label.substr(slash + 1);
        auto it = by_name.find(label);
        out.push_back(it == by_name.end() ? kGlobalResourceLevel : it->second);
    }
    return out;
}

} // namespace

std::shared_ptr<const game::AppearanceData> load_appearance_data(const GameDir& game) {
    auto data = std::make_shared<game::AppearanceData>();
    auto text = [&](std::string_view file) -> std::string {
        auto bytes = game.read(file);
        return bytes ? std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()) : std::string();
    };
    data->npc_tints = game::ColorTable::parse(text("Resources/npct.ini"));
    data->item_tints = game::ColorTable::parse(text("Resources/itmt.ini"));
    data->new_armor.parse_tags(text("Resources/NewArmorTagData.txt"));
    data->new_armor.parse_adjustments(text("Resources/NewArmorAdjData.txt"));
    data->customization.parse(text("Resources/PlayerCustomization.txt"));
    // [Defaults] ActorNewArmorFilter (0xe63518), default 24.
    bool defaults = false;
    auto ini = game.read("eqclient.ini");
    for (const auto& raw : ini ? text_lines(*ini) : std::vector<std::string>{}) {
        std::string line = trim(raw);
        if (line.starts_with('[')) {
            defaults = lower(line) == "[defaults]";
            continue;
        }
        auto eq = line.find('=');
        if (defaults && eq != std::string::npos && lower(trim(line.substr(0, eq))) == "actornewarmorfilter")
            data->new_armor_filter = static_cast<std::uint32_t>(std::strtoul(trim(line.substr(eq + 1)).c_str(), nullptr, 10));
    }
    log::info("appearance data: {} NPC tints, {} item tints, {} hero's forge tags, {} adjustments, {} customization entries",
              data->npc_tints.values.size(), data->item_tints.values.size(), data->new_armor.tag_count(),
              data->new_armor.adjustment_count(), data->customization.size());
    return data;
}

CharacterLibrary CharacterLibrary::load(const GameDir& game, std::string_view zone, bool luclin) {
    auto list = open_archives(game, global_character_archives(game, luclin));
    auto zone_list = open_archives(game, zone_character_archives(game, zone));
    std::vector<int> levels = levels_of(list, global_archive_levels(game));
    levels.resize(list.size() + zone_list.size(), kZoneResourceLevel);
    list.insert(list.end(), zone_list.begin(), zone_list.end());
    CharacterLibrary lib(std::move(list), std::move(levels));
    lib.set_eqg_store(std::make_shared<EqgCharacterStore>(game));
    lib.set_model_archive_store(std::make_shared<ModelArchiveStore>(game));
    lib.set_appearance_data(load_appearance_data(game));
    return lib;
}

std::shared_ptr<const CharacterLibrary> CharacterArchiveCache::library_for_zone(const GameDir& game,
                                                                               std::string_view zone) {
    auto memory = [](std::string_view group, const auto& archives) {
        if (auto rss = platform::resident_memory())
            log::debug("memory checkpoint: character archives {}: resident {:.2f} MiB", group, double(*rss) / (1024.0 * 1024.0));
        if (auto commit = platform::private_commit_memory())
            log::debug("memory checkpoint: character archives {}: private commit {:.2f} MiB", group,
                       double(*commit) / (1024.0 * 1024.0));
        std::size_t mapped = 0, owned = 0, parsed = 0;
        constexpr double mib = 1024.0 * 1024.0;
        for (const auto& a : archives) {
            const auto heap = a->owned_bytes();
            const auto file = a->archive_bytes() - heap;
            const auto wld = a->parsed_bytes();
            mapped += file;
            owned += heap;
            parsed += wld;
            log::debug("memory archive: {} {}: mapped {:.2f} MiB, heap {:.2f} MiB, parsed WLD estimate {:.2f} MiB",
                       group, a->archive->label(), double(file) / mib, double(heap) / mib, double(wld) / mib);
        }
        log::info("memory archives: {}: {} archives, mapped {:.2f} MiB, heap {:.2f} MiB, parsed WLD estimate {:.2f} MiB",
                  group, archives.size(), double(mapped) / mib, double(owned) / mib, double(parsed) / mib);
    };
    if (!loaded_) {
        auto t0 = std::chrono::steady_clock::now();
        auto list = CharacterLibrary::open_archives(game, global_character_archives(game, luclin_));
        memory("global", list);
        auto levels = levels_of(list, global_archive_levels(game));
        globals_ = std::make_shared<const CharacterArchiveIndex>(std::move(list), std::move(levels));
        eqg_ = std::make_shared<EqgCharacterStore>(game);
        own_ = std::make_shared<ModelArchiveStore>(game);
        appearance_ = load_appearance_data(game);
        shared_ = std::make_shared<SharedCharacterCache>();
        mutex_ = std::make_shared<std::recursive_mutex>();
        loaded_ = true;
        log::info("character archives: {} global{} in {} ms", globals_->archives().size(), luclin_ ? " (with Luclin models)" : "",
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    // Only the zone's own archives are read and indexed here; the global ones' index is shared.
    auto zone_list = CharacterLibrary::open_archives(game, zone_character_archives(game, zone));
    memory(zone, zone_list);
    auto lib = std::make_shared<CharacterLibrary>(globals_, std::move(zone_list));
    {
        std::lock_guard lock(*mutex_); // the last zone's library may still be in use on another thread
        shared_->next_zone();
    }
    lib->set_mutex(mutex_);
    lib->set_shared_cache(shared_);
    lib->set_eqg_store(eqg_);
    lib->set_model_archive_store(own_);
    lib->set_appearance_data(appearance_);
    return lib;
}

std::shared_ptr<const CharacterLibrary> ModelArchiveStore::library(std::string_view code_view) {
    std::string code = upper(code_view);
    if (auto it = libraries_.find(code); it != libraries_.end()) return it->second;
    std::shared_ptr<const CharacterLibrary> result;
    std::string file = lower(code) + "_chr.s3d";
    if (game_.exists(file)) {
        auto archives = CharacterLibrary::open_archives(game_, {file});
        if (!archives.empty()) {
            auto lib = std::make_shared<CharacterLibrary>(std::move(archives));
            if (lib->has_model(code)) {
                log::info("character model {}: from {} (no global or zone archive has it)", code, file);
                result = std::move(lib);
            }
        }
    }
    libraries_.emplace(std::move(code), result);
    return result;
}

std::shared_ptr<const CharacterLibrary> ModelArchiveStore::opened(std::string_view code) const {
    auto it = libraries_.find(upper(code));
    return it == libraries_.end() ? nullptr : it->second;
}

std::optional<std::vector<std::byte>> ModelArchiveStore::read_file(std::string_view name) const {
    for (const auto& [code, lib] : libraries_)
        if (lib && lib->has_file(name)) return lib->read_file(name);
    return std::nullopt;
}

bool ModelArchiveStore::has_file(std::string_view name) const {
    return std::any_of(libraries_.begin(), libraries_.end(), [&](const auto& l) { return l.second && l.second->has_file(name); });
}

bool CharacterLibrary::has_model(std::string_view code) const {
    Lock lock(*mutex_);
    if (actors().contains(upper(code)) || (eqg_ && eqg_->has_model(code))) return true;
    return own_ && own_->library(code) != nullptr;
}

std::vector<std::string> CharacterLibrary::model_codes() const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    std::vector<std::string> out;
    auto consider = [&](const std::string& code, const FragmentLoc& loc) {
        // Skeletal actors only; IT<n> are item models.
        if (code.size() > 2 && code.starts_with("IT") && std::isdigit(static_cast<unsigned char>(code[2]))) return;
        const auto& actor = loc.fragment()->as<wld::ActorDef>();
        // NOLINTBEGIN(clang-analyzer-core.NullDereference): false positive, the actor tables hold only non-null locations.
        bool skeletal = std::any_of(actor.sprites.begin(), actor.sprites.end(),
                                    [&](wld::Ref r) { return loc.first->get<wld::SkeletonRef>(r) != nullptr; });
        // NOLINTEND(clang-analyzer-core.NullDereference)
        if (skeletal) out.push_back(code);
    };
    for (const auto& [code, loc] : tables_.actors) consider(code, loc);
    if (globals_)
        for (const auto& [code, loc] : globals_->tables_.actors)
            if (!tables_.actors.contains(code)) consider(code, loc); // else the zone's definition won

    std::sort(out.begin(), out.end());
    return out;
}

const CharacterLibrary::TrackLoc* CharacterLibrary::track(const std::string& name) const {
    return tracks().find(name);
}

std::optional<std::vector<std::byte>> CharacterLibrary::read_file(std::string_view name) const {
    Lock lock(*mutex_);
    const std::size_t* index = files().find(lower(name));
    if (!index) {
        if (eqg_)
            if (auto data = eqg_->read_file(name)) return data;
        return own_ ? own_->read_file(name) : std::nullopt;
    }
    return archives_[*index]->archive->read(name);
}

bool CharacterLibrary::has_file(std::string_view name) const {
    Lock lock(*mutex_);
    return files().contains(lower(name)) || (eqg_ && eqg_->has_file(name)) || (own_ && own_->has_file(name));
}

std::shared_ptr<const CharacterModel> CharacterLibrary::model(std::string_view code_view) const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    std::string code = upper(code_view);
    if (auto it = cache_.find(code); it != cache_.end()) {
        CharacterLookups::note(it->second.lookups);
        return it->second.value;
    }
    if (shared_)
        if (auto it = shared_->models.find(code); it != shared_->models.end() && unaffected(it->second.lookups)) {
            it->second.used = shared_->generation();
            CharacterLookups::note(it->second.lookups);
            cache_.emplace(code, Cached<std::shared_ptr<const CharacterModel>>{it->second.value, it->second.lookups});
            return it->second.value;
        }
    std::optional<CharacterLookups> rec;
    if (shared_ || CharacterLookups::active()) rec.emplace();
    std::shared_ptr<const CharacterModel> m;
    if (actors().contains(code)) {
        try {
            m = std::make_shared<const CharacterModel>(build(code));
        } catch (const std::exception& e) {
            log::warn("character model {}: {}", code, e.what());
        }
    }
    if (!m && eqg_) m = eqg_->model(code);
    if (!m && own_)
        if (auto lib = own_->library(code)) m = lib->model(code);
    CharacterLookups::Names names = rec ? rec->names() : nullptr;
    if (shared_ && names && unaffected(*names) && !shared_->models.contains(code)) {
        shared_->models.emplace(code, SharedCharacterCache::Entry<std::shared_ptr<const CharacterModel>>{
                                          m, names, shared_->generation()});
        if (m) shared_->owners[m.get()] = {names, nullptr};
    }
    cache_.emplace(code, Cached<std::shared_ptr<const CharacterModel>>{m, std::move(names)});
    return m;
}

CharacterModel CharacterLibrary::build(std::string_view code_view) const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    std::string code = upper(code_view);
    const FragmentLoc* actor = actors().find(code);
    if (!actor) throw std::runtime_error("no actor " + code + "_ACTORDEF");
    auto model = build_actor(code, *actor->first, *actor->fragment());
    model.compact_animations();
    return model;
}

namespace {

// The skeleton of an actor's sprite list, or nullptr for a static actor.
const wld::Skeleton* actor_skeleton(const wld::WldFile& w, const wld::ActorDef& actor) {
    for (wld::Ref s : actor.sprites)
        if (const auto* sr = w.get<wld::SkeletonRef>(s))
            if (const auto* sk = w.get<wld::Skeleton>(sr->skeleton)) return sk;
    return nullptr;
}

} // namespace

CharacterModel CharacterLibrary::build_actor(const std::string& code, const wld::WldFile& w,
                                             const wld::Fragment& actor_fragment) const {
    const auto& actor = actor_fragment.as<wld::ActorDef>();

    const wld::Skeleton* sk = actor_skeleton(w, actor);
    if (!sk) return build_static_actor(code, w, actor);
    const std::size_t n = sk->nodes.size();
    if (n == 0 || n > 1024) throw std::runtime_error("bad skeleton node count");

    CharacterModel m;
    m.code = code;
    m.source = w.label();
    m.luclin = is_luclin_archive(lower(w.label().substr(0, w.label().find(':'))));
    m.bounding_radius = sk->bounding_radius;

    // Hierarchy from the child lists; each node has at most one parent.
    m.parents.assign(n, -1);
    for (std::size_t i = 0; i < n; ++i)
        for (auto c : sk->nodes[i].children) {
            if (c < 0 || static_cast<std::size_t>(c) >= n || static_cast<std::size_t>(c) == i ||
                m.parents[static_cast<std::size_t>(c)] != -1)
                throw std::runtime_error("bad skeleton hierarchy");
            m.parents[static_cast<std::size_t>(c)] = static_cast<std::int32_t>(i);
        }
    for (std::size_t i = 0; i < n; ++i)
        if (m.parents[i] < 0) m.order.push_back(static_cast<std::uint32_t>(i));
    for (std::size_t k = 0; k < m.order.size(); ++k)
        for (auto c : sk->nodes[m.order[k]].children) m.order.push_back(static_cast<std::uint32_t>(c));
    if (m.order.size() != n) throw std::runtime_error("cycle in skeleton");

    // Rest pose: each node's own track (frame 0).
    AnimationClip rest;
    rest.tracks.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (sk->nodes[i].name == code + "_BLD_DAG") m.blood_bone = static_cast<int>(i);
        const auto* f = w.resolve(sk->nodes[i].track);
        m.bone_names.push_back(f ? f->name : std::string());
        const auto* ref = f ? f->get<wld::TrackRef>() : nullptr;
        const auto* def = ref ? w.get<wld::TrackDef>(ref->track) : nullptr;
        rest.tracks[i].push_back(def && !def->frames.empty() ? to_pose(def->frames.front()) : BonePose{});
    }
    const auto* race_model = game::find_model_code(code);
    const std::array<std::string, 3> name_dags{code + "HEAD_POINT_DAG", code + "HE_DAG",
        race_model && race_model->gender == 1 ? "HUFHEAD_POINT_DAG" : "HUMHEAD_POINT_DAG"};
    for (const auto& name : name_dags) {
        for (std::size_t i = 0; i < n; ++i)
            if (sk->nodes[i].name == name) { m.name_bone = static_cast<int>(i); break; }
        if (m.name_bone >= 0) break;
    }

    // Meshes: the skeleton's list (flag 0x200) or the nodes' sprites.
    std::vector<std::pair<wld::Ref, std::int64_t>> sprites; // sprite, owning node or -1
    for (wld::Ref r : sk->meshes) sprites.emplace_back(r, -1);
    if (sprites.empty())
        for (std::size_t i = 0; i < n; ++i)
            if (sk->nodes[i].sprite) sprites.emplace_back(sk->nodes[i].sprite, static_cast<std::int64_t>(i));
    std::unordered_set<const wld::Fragment*> seen;
    CharacterMesh geometry;
    for (auto [s, owner] : sprites) {
        const wld::Fragment* sprite = w.resolve(s);
        const auto* ref = sprite ? sprite->get<wld::MeshRef>() : nullptr;
        const wld::Fragment* mf = ref ? w.resolve(ref->mesh) : nullptr;
        if (!mf || !mf->holds<wld::MeshDef>() || !seen.insert(mf).second) continue;
        detail::append_wld_mesh(w, *mf, n, owner, geometry);
    }
    m.vertices = std::move(geometry.vertices);
    m.indices = std::move(geometry.indices);
    m.parts = std::move(geometry.parts);
    m.materials = std::move(geometry.materials);
    if (m.indices.empty()) throw std::runtime_error("no visible mesh");

    // Clips. Codes come from tracks named <code><bone track>, for the model's
    // own bones and for the donor's equivalent bones (donor code + the same
    // suffix). A clip the model has any track for uses only its own tracks;
    // otherwise the whole clip is borrowed. Missing bones keep the rest pose.
    // InitAnimationTracks @0x407800 asks GetAlternateAnimTag for the donor with whether the actor has a
    // TUNIC_POINT dag: the Luclin player models do, and their erudites take the human's clips, not the wood elf's.
    bool tunic_point = false;
    for (const auto& b : m.bone_names)
        if (b.size() > code.size() && b.starts_with(code) && std::string_view(b).substr(code.size()) == "TUNIC_POINT_TRACK")
            tunic_point = true;
    m.animation_donor = std::string(game::animation_source(code, tunic_point));
    std::vector<std::string> donor_names(n);
    if (!m.animation_donor.empty())
        for (std::size_t i = 0; i < n; ++i)
            if (m.bone_names[i].starts_with(code))
                donor_names[i] = m.animation_donor + m.bone_names[i].substr(code.size());
    // Another skeleton of the same code shares some bone names (the root "WOL_TRACK", points such as
    // "WOLHEAD_POINT_TRACK"): live's wol_v2_chr WOL has an L01 for them, which gave a zone's classic WOL (no L01)
    // a one-frame walk and froze it. A track in another WLD whose own <code>_ACTORDEF has different bones belongs
    // to that model, not this one; a WLD defining the same skeleton (a zone copy of a global model) still lends.
    std::map<const wld::WldFile*, bool> rivals;
    auto rival = [&](const TrackLoc* loc) {
        if (!loc || loc->wld == &w) return false;
        auto [it, added] = rivals.try_emplace(loc->wld, false);
        if (added)
            if (auto a = actor_wlds().find({loc->wld, code})) {
                const auto* other = actor_skeleton(*loc->wld, a->fragment()->as<wld::ActorDef>());
                std::vector<std::string> names;
                if (other)
                    for (const auto& node : other->nodes) {
                        const auto* f = loc->wld->resolve(node.track);
                        names.push_back(f ? f->name : std::string());
                    }
                it->second = names != m.bone_names;
            }
        return it->second;
    };
    // The donor's tracks likewise, by clip: global_chr's classic ELF shares a few bone names (the root, PE, CH, NE,
    // HE and the points) with the Luclin ELF a Luclin model borrows from, and its C10, C11 and T07..T09, which the
    // Luclin ELF has not, lent those few bones and left the rest of the body in the rest pose: a Luclin human, high
    // elf or erudite threw or kicked in a T-pose. A WLD whose own <donor>_ACTORDEF has an equivalent of fewer than
    // half of this model's bones holds another skeleton, and a borrowed clip all of whose tracks are in such WLDs is
    // not lent. Not "all of them": a model and its donor differ by a bone or two (HUF's HAIR_POINT01). A name the
    // two skeletons share is one track, in whichever WLD the name table kept, so a clip with any track of the right
    // skeleton keeps all its tracks, as before.
    std::set<std::string> suffixes;
    for (const auto& b : m.bone_names)
        if (b.starts_with(code)) suffixes.insert(b.substr(code.size()));
    std::map<const wld::WldFile*, bool> donor_rivals;
    auto donor_rival = [&](const TrackLoc* loc) {
        if (!loc || loc->wld == &w) return false;
        auto [it, added] = donor_rivals.try_emplace(loc->wld, false);
        if (added)
            if (auto a = actor_wlds().find({loc->wld, m.animation_donor})) {
                const auto* other = actor_skeleton(*loc->wld, a->fragment()->as<wld::ActorDef>());
                std::size_t shared = 0;
                if (other)
                    for (const auto& node : other->nodes) {
                        const auto* f = loc->wld->resolve(node.track);
                        if (f && f->name.starts_with(m.animation_donor) &&
                            suffixes.contains(f->name.substr(m.animation_donor.size())))
                            ++shared;
                    }
                it->second = shared * 2 < suffixes.size();
            }
        return it->second;
    };
    std::set<std::string> own_codes, donor_codes;
    for (std::size_t i = 0; i < n; ++i) {
        if (const auto* codes = clips_by_bone().find(m.bone_names[i]))
            for (const auto& c : *codes)
                if (!rival(track(c + m.bone_names[i]))) own_codes.insert(c);
        if (donor_names[i].empty()) continue;
        if (const auto* codes = clips_by_bone().find(donor_names[i])) donor_codes.insert(codes->begin(), codes->end());
    }
    std::set<std::string> all = own_codes;
    all.insert(donor_codes.begin(), donor_codes.end());
    for (const auto& clip_code : all) {
        bool own = own_codes.contains(clip_code);
        AnimationClip a;
        a.borrowed = !own;
        a.tracks.resize(n);
        bool timed = false, found = false, lent = own;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string& name = own ? m.bone_names[i] : donor_names[i];
            const TrackLoc* loc = name.empty() ? nullptr : track(clip_code + name);
            if (own && rival(loc)) loc = nullptr;
            const auto* def = loc ? loc->wld->get<wld::TrackDef>(loc->ref.track) : nullptr;
            if (!def || def->frames.empty()) {
                a.tracks[i] = rest.tracks[i];
                continue;
            }
            found = true;
            if (!own && !donor_rival(loc)) lent = true;
            // Hidden borrowed bones keep the same rest multiplier in both owned and mapped tracks.
            const glm::vec3 multiplier = !own && rest.tracks[i].front().scale.x < 0.1f
                                             ? rest.tracks[i].front().scale : glm::vec3{1.0f};
            if (def->frames.mapped()) {
                a.tracks[i] = AnimationTrack::from_wld(def->frames, multiplier);
            } else {
                a.tracks[i].reserve(def->frames.size());
                for (const auto& f : def->frames) {
                    BonePose p = to_pose(f);
                    p.scale *= multiplier;
                    a.tracks[i].push_back(p);
                }
            }
            if (def->frames.size() > 1) {
                a.frame_count = std::max<std::uint32_t>(a.frame_count, static_cast<std::uint32_t>(def->frames.size()));
                if ((loc->ref.flags & 1) && loc->ref.frame_ms > 0 && !timed) {
                    a.frame_ms = loc->ref.frame_ms;
                    timed = true;
                }
            }
        }
        if (found && lent) m.clips.emplace(clip_code, std::move(a));
    }
    // Luclin takes: "C01A" (else "C01B") also plays as "C01" when the model
    // has no animated C01 of its own (OpenEQ luclin.rs install_animation_aliases).
    // Clip metadata is copied; AnimationTrack keeps the key payload shared with its take.
    for (char take : {'A', 'B'}) {
        std::vector<std::pair<std::string, AnimationClip>> aliases;
        for (const auto& [c, clip] : m.clips) {
            if (c.size() != 4 || c[3] != take) continue;
            auto base = m.clips.find(c.substr(0, 3));
            if (base == m.clips.end() || (base->second.frame_count <= 1 && clip.frame_count > 1))
                aliases.emplace_back(c.substr(0, 3), clip);
        }
        for (auto& [c, clip] : aliases) m.clips.insert_or_assign(c, std::move(clip));
    }
    // InitAnimationTracks @0x407800 binds a tunic-point model's variants A..H to holding columns 0..7, so a spawn
    // holding something plays its B tracks (held_aliases).
    if (m.find_bone("TUNIC_POINT_TRACK") >= 0)
        for (const auto& [c, clip] : m.clips)
            if (c.size() == 4 && c[3] == 'B' && clip.frame_count > 0) m.held_aliases.emplace(c.substr(0, 3), c);
    m.clips.emplace("", std::move(rest));

    // Rest-pose bounds.
    std::vector<glm::mat4> world;
    m.sample(m.clips.at(""), 0.0f, false, world);
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const auto& v : m.vertices) {
        glm::vec3 p = glm::vec3(world[v.bones[0]] * glm::vec4(v.position, 1.0f));
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    m.bounds_min = lo;
    m.bounds_max = hi;
    return m;
}

// An actor drawn by a plain DMSprite (no skeleton): the ships and boats of the character archives (BOAT, SHIP,
// LAUNCH ...), a simple actor in the graphics DLL. One bone at rest carries the whole mesh; there are no clips.
CharacterModel CharacterLibrary::build_static_actor(const std::string& code, const wld::WldFile& w,
                                                    const wld::ActorDef& actor) const {
    const wld::Fragment* mf = nullptr;
    for (wld::Ref r : actor.sprites) {
        const wld::Fragment* sprite = w.resolve(r);
        const auto* ref = sprite ? sprite->get<wld::MeshRef>() : nullptr;
        const wld::Fragment* f = ref ? w.resolve(ref->mesh) : nullptr;
        if (f && f->holds<wld::MeshDef>()) {
            mf = f;
            break;
        }
    }
    if (!mf) throw std::runtime_error("neither a skeletal nor a mesh actor");
    CharacterModel m;
    m.code = code;
    m.source = w.label();
    m.luclin = is_luclin_archive(lower(w.label().substr(0, w.label().find(':'))));
    m.parents = {-1};
    m.order = {0};
    m.bone_names = {std::string()};
    CharacterMesh geometry;
    detail::append_wld_mesh(w, *mf, 1, 0, geometry);
    m.vertices = std::move(geometry.vertices);
    m.indices = std::move(geometry.indices);
    m.parts = std::move(geometry.parts);
    m.materials = std::move(geometry.materials);
    if (m.indices.empty()) throw std::runtime_error("no visible mesh");
    AnimationClip rest;
    rest.tracks.assign(1, {BonePose{}});
    m.clips.emplace("", std::move(rest));
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const auto& v : m.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    m.bounds_min = lo;
    m.bounds_max = hi;
    m.bounding_radius = glm::length(glm::max(glm::abs(lo), glm::abs(hi)));
    return m;
}

} // namespace hailstone::assets
