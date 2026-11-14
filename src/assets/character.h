#pragma once

// Classic (WLD) character models: skeleton, rigidly skinned mesh, materials
// and animation clips, independent of the renderer. Format notes and sources:
// docs/formats/characters.md.
//
// A model is the actor "<CODE>_ACTORDEF" (0x14) -> 0x11 -> skeleton 0x10.
// Each skeleton node names a 0x13 track (e.g. HUMPE_TRACK) whose first frame
// is the rest pose. Animations are other 0x13 tracks named
// "<clip><bone track>", e.g. L01HUMPE_TRACK; a clip code is a letter and two
// digits (C01 combat, L01 walk, L02 run, P01 stand, O01 idle, D02 hit ...).
// Every vertex belongs to one node (0x36 vertex pieces) and is stored in that
// node's space, so skinning is one bone matrix per vertex.

#include <array>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/model_points.h"
#include "assets/model.h"
#include "assets/pfs.h"
#include "assets/texture.h"
#include "assets/texture_overrides.h"
#include "core/game_dir.h"
#include "assets/wld.h"

namespace hailstone { class GameDir; }
namespace hailstone::game {
struct ActorLook;
struct Appearance;
struct AppearanceData;
} // namespace hailstone::game

namespace hailstone::assets {

// A bone's local transform: local = T(translation) * R(rotation) * S(scale).
struct BonePose {
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // normalised
    glm::vec3 translation{0.0f};
    glm::vec3 scale{1.0f}; // uniform for WLD tracks; EQG tracks store three
};

// WLD clips keep their encoded keys and convert only the poses sampled by the renderer. EQG and fixture keys
// stay owned; mutable access to a WLD view detaches it without changing another model or animation alias.
class BoneFrames {
public:
    using value_type = BonePose;
    using size_type = std::size_t;
    using iterator = std::vector<BonePose>::iterator;
    class const_iterator {
    public:
        using value_type = BonePose;
        using reference = BonePose;
        using difference_type = std::ptrdiff_t;
        // Dereferencing decodes a value, so to C++17 algorithms this is only an input iterator (as iota_view's);
        // C++20 ranges see a random-access one.
        using iterator_category = std::input_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;
        struct pointer {
            BonePose value;
            const BonePose* operator->() const { return &value; }
        };
        const_iterator() = default;
        reference operator*() const { return (*owner_)[index_]; }
        pointer operator->() const { return {**this}; }
        reference operator[](difference_type n) const { return *(*this + n); }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        const_iterator& operator--() { --index_; return *this; }
        const_iterator operator--(int) { auto old = *this; --*this; return old; }
        const_iterator& operator+=(difference_type n) {
            index_ = static_cast<size_type>(static_cast<difference_type>(index_) + n); return *this;
        }
        const_iterator& operator-=(difference_type n) { return *this += -n; }
        friend const_iterator operator+(const_iterator i, difference_type n) { i += n; return i; }
        friend const_iterator operator+(difference_type n, const_iterator i) { return i + n; }
        friend const_iterator operator-(const_iterator i, difference_type n) { i -= n; return i; }
        friend difference_type operator-(const_iterator a, const_iterator b) {
            return static_cast<difference_type>(a.index_) - static_cast<difference_type>(b.index_);
        }
        auto operator<=>(const const_iterator&) const = default;

    private:
        friend class BoneFrames;
        const_iterator(const BoneFrames* owner, size_type index) : owner_(owner), index_(index) {}
        const BoneFrames* owner_ = nullptr;
        size_type index_ = 0;
    };

    BoneFrames() = default;
    BoneFrames(std::initializer_list<BonePose> frames) : storage_(std::vector<BonePose>(frames)) {}
    BoneFrames(std::vector<BonePose> frames) : storage_(std::move(frames)) {}
    BoneFrames(const BoneFrames&) = default;
    BoneFrames& operator=(const BoneFrames&) = default;
    BoneFrames(BoneFrames&& other) noexcept;
    BoneFrames& operator=(BoneFrames&& other) noexcept;
    BoneFrames& operator=(std::initializer_list<BonePose> frames) { assign(frames); return *this; }
    BoneFrames& operator=(std::vector<BonePose> frames) { storage_ = std::move(frames); return *this; }

    static BoneFrames from_wld(wld::TrackFrames frames, glm::vec3 scale_multiplier = glm::vec3(1.0f));
    bool encoded() const;
    bool mapped() const;
    size_type size() const;
    bool empty() const { return size() == 0; }
    size_type capacity() const;
    size_type decoded_capacity() const;
    // Owned payload only; mapped backing is separate from model-private memory. Shared encoded owners count once.
    std::size_t owned_payload_bytes(std::unordered_set<const void*>& seen) const;
    BonePose operator[](size_type i) const;
    BonePose at(size_type i) const;
    BonePose front() const { return (*this)[0]; }
    BonePose back() const { return (*this)[size() - 1]; }
    BonePose& operator[](size_type i) { return materialize()[i]; }
    BonePose& at(size_type i);
    BonePose& front() { return materialize().front(); }
    BonePose& back() { return materialize().back(); }
    BonePose* data() { return materialize().data(); }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    iterator begin() { return materialize().begin(); }
    iterator end() { return materialize().end(); }
    void reserve(size_type count) { materialize().reserve(count); }
    void resize(size_type count) { materialize().resize(count); }
    void resize(size_type count, const BonePose& pose) { materialize().resize(count, pose); }
    void push_back(const BonePose& pose) { materialize().push_back(pose); }
    void push_back(BonePose&& pose) { materialize().push_back(std::move(pose)); }
    template<class... Args> BonePose& emplace_back(Args&&... args) {
        return materialize().emplace_back(std::forward<Args>(args)...);
    }
    void clear() {
        if (auto* owned = std::get_if<std::vector<BonePose>>(&storage_)) owned->clear();
        else storage_.emplace<std::vector<BonePose>>();
    }
    void assign(size_type count, const BonePose& pose) { storage_ = std::vector<BonePose>(count, pose); }
    void assign(std::initializer_list<BonePose> frames) { storage_ = std::vector<BonePose>(frames); }
    template<class It> void assign(It first, It last) { storage_ = std::vector<BonePose>(first, last); }

private:
    struct WldFrames {
        wld::TrackFrames frames;
        glm::vec3 scale_multiplier{1.0f};
    };
    std::vector<BonePose>& materialize();
    std::variant<std::vector<BonePose>, WldFrames> storage_;
};

// Copies share key data. Builders detach on mutation; readers receive sampled values. WLD tracks may retain
// encoded immutable backing. Constant tracks keep their logical length so interpolation and looping stay unchanged.
class AnimationTrack {
public:
    using StorageSet = std::unordered_set<const void*>;
    AnimationTrack() = default;
    AnimationTrack(std::initializer_list<BonePose> keys);
    AnimationTrack(BoneFrames frames);
    AnimationTrack(const AnimationTrack&) = default;
    AnimationTrack& operator=(const AnimationTrack&) = default;
    AnimationTrack(AnimationTrack&& other) noexcept;
    AnimationTrack& operator=(AnimationTrack&& other) noexcept;
    static AnimationTrack from_wld(wld::TrackFrames frames, glm::vec3 scale_multiplier = glm::vec3(1.0f));
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool encoded() const { return encoded_ && encoded_->encoded(); }
    bool mapped() const { return encoded_ && encoded_->mapped(); }
    std::size_t decoded_capacity() const { return keys_ ? keys_->capacity() : 0; }
    BonePose operator[](std::size_t i) const;
    BonePose front() const { return (*this)[0]; }
    BonePose back() const { return (*this)[count_ - 1]; }
    void push_back(const BonePose& key);
    void reserve(std::size_t count);
    void resize(std::size_t count);
    void clear() { keys_.reset(); encoded_.reset(); count_ = 0; encoded_constant_ = false; }
    // Stored keys can be encoded file views rather than decoded poses. Sampling uses the logical frame count.
    std::size_t stored_keys() const {
        return encoded_ ? (encoded_constant_ && count_ ? 1 : encoded_->size()) : (keys_ ? keys_->size() : 0);
    }
    const void* storage_id() const { return encoded_ ? static_cast<const void*>(encoded_.get()) : keys_.get(); }
    std::size_t storage_bytes(StorageSet& seen) const;
private:
    friend struct CharacterModel;
    std::shared_ptr<std::vector<BonePose>> keys_;
    std::shared_ptr<const BoneFrames> encoded_;
    std::size_t count_ = 0;
    bool encoded_constant_ = false;
    std::vector<BonePose>& writable();
    void compact();
    std::uint64_t hash() const;
    bool share_if_equal(const AnimationTrack& other);
};

struct AnimationClip {
    std::uint32_t frame_ms = 100;  // 0x13 frame time; 100 ms when the tracks give none
    std::uint32_t frame_count = 1; // longest track
    // One key list per bone, at least one key each. Bones the clip does not
    // animate hold their rest pose.
    std::vector<AnimationTrack> tracks;
    bool borrowed = false; // taken from the animation donor model

    float duration_seconds() const { return static_cast<float>(frame_count * frame_ms) / 1000.0f; }
};

// WLD vertices follow one bone (weights {1, 0, 0, 0}) and are stored in
// that bone's space; EQG vertices blend up to four bones and are stored in
// model (bind) space, so their bones carry inverse bind matrices.
struct SkinnedVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};
    std::array<std::uint16_t, 4> bones{};
    glm::vec4 weights{1.0f, 0.0f, 0.0f, 0.0f};
};

// Geometry of a model in one appearance (armour textures, head and robe
// meshes, held items). Bones are the model's.
// An always-on actor emitter (assets/model_points.h ModelEmitter) of a
// dressed mesh: the body's own or a held item's, hanging from `bone` (a bone
// of the model; its frame) through `local` (the model point's matrix, in that
// frame).
struct MeshEmitter {
    int bone = -1;
    glm::mat4 local{1.0f};
    ModelEmitter emitter;
    std::string model_code; // the point-owning model's moddat.ini PESFactor
};

// A number no other mesh made in this run has, so a mesh freed and another made at its address are told apart
// (app/actor_emitters.h keys an actor's emitters by it). A copy is another mesh and gets a new one; a move keeps it
// and leaves the source a new one.
class MeshSerial {
public:
    MeshSerial() : value_(next()) {}
    MeshSerial(const MeshSerial&) : value_(next()) {}
    MeshSerial& operator=(const MeshSerial& o) {
        if (this != &o) value_ = next();
        return *this;
    }
    MeshSerial(MeshSerial&& o) noexcept : value_(o.value_) { o.value_ = next(); }
    MeshSerial& operator=(MeshSerial&& o) noexcept {
        if (this != &o) {
            value_ = o.value_;
            o.value_ = next();
        }
        return *this;
    }
    std::uint64_t value() const { return value_; }

private:
    static std::uint64_t next() noexcept;
    std::uint64_t value_;
};

struct CharacterMesh {
    struct SkinBinding {
        std::uint16_t host_bone = 0;
        glm::mat4 local{1.0f};
    };
    MeshSerial serial;
    // Attachment palette entries after the model's own bones. A WLD host's
    // raw bone matrix needs a separate inverse bind for each EQG influence.
    std::vector<SkinBinding> skin_bindings;
    bool append_skin_bones(std::vector<glm::mat4>& bones) const;
    std::vector<MeshEmitter> emitters;
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices; // triangle list
    std::vector<MeshPart> parts;
    std::vector<Material> materials;
    // Per material: tint 0xAARRGGBB multiplied into the texture (a zero
    // high byte = none).
    std::vector<std::uint32_t> tints;
    std::size_t retained_bytes() const; // estimate: object and owned payload sizes, excluding allocator overhead
};

struct CharacterModel {
    std::string code;   // "HUM"
    std::string source; // archive and WLD the actor came from
    std::vector<std::string> bone_names; // the nodes' 0x13 track names
    std::vector<std::int32_t> parents;   // -1 for a root
    std::vector<std::uint32_t> order;    // parents before children
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;  // triangle list
    std::vector<MeshPart> parts;
    std::vector<Material> materials;
    // Clip code -> clip. "" is the rest pose (one frame).
    std::map<std::string, AnimationClip, std::less<>> clips;
    // EQG models: classic codes answered by a track of `clips` (eqg_character::classic_aliases: "C05" ->
    // "SLPR_BA_1"); clip() follows them.
    std::map<std::string, std::string, std::less<>> clip_aliases;
    // Models with a tunic point (InitAnimationTracks' +0x34, the Luclin ones): code -> its variant B ("P01" ->
    // "P01B"), the track of holding column 1. PlayerClient_UpdateHoldingAnimation gives a spawn holding anything
    // a column from 1 to 7, and 0x4b1210 takes column 1 for those the model has no variant of (no model ships
    // C..H); with nothing held it plays column 0, `clips` as they are. clip(code, true) follows these.
    std::map<std::string, std::string, std::less<>> held_aliases;
    std::string animation_donor; // model whose clips fill in missing ones, or empty
    // EQG models: the bind pose's inverse per bone (vertices are in bind
    // space). Empty for WLD models, whose vertices are in bone space.
    std::vector<glm::mat4> inverse_bind;
    bool eqg = false;
    bool luclin = false; // from a Luclin player-model archive (global<code>_chr)
    // Rest-pose bounds in model space (Z up, the front faces +X).
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
    // The skeleton's bounding radius (WLD 0x10 flag 2), unscaled; 0 when the
    // file gives none. The actor's GetBoundingRadius is this x its scale.
    float bounding_radius = 0.0f;
    // Actor role 6 resolves the authored <code>_BLD_DAG, independently of its track name.
    int blood_bone = -1;
    int name_bone = -1; // authored actor role 0, independent of spell points
    // EQG models: the named points of `<code>.pts` beside the mesh (spell
    // points, assets/model_points.h). WLD models build theirs from the bone
    // names (assets::wld_model_points), so this stays empty for them.
    std::vector<ModelPoint> points;
    // EQG models: `<code>.prt`'s actor emitters on those points.
    std::vector<ModelEmitter> emitters;

    const AnimationClip* clip(std::string_view code) const;
    // The clip a spawn plays for `code`, `holding` when PlayerClient_UpdateHoldingAnimation gives it a column other
    // than 0: its variant B when the model has one (held_aliases), else clip(code), as 0x4b1210 falls back.
    const AnimationClip* clip(std::string_view code, bool holding) const;
    // First of the codes that exists, else the rest pose.
    const AnimationClip& first_clip(std::initializer_list<std::string_view> codes, bool holding = false) const;

    // Local bone poses at a time; frames are interpolated (slerp / lerp).
    // Looping clips wrap, others hold their last frame.
    void sample_local(const AnimationClip& clip, float time_seconds, bool loop, std::vector<BonePose>& out) const;
    // Model-space bone matrices from local poses.
    void compose(std::span<const BonePose> local, std::vector<glm::mat4>& out) const;
    void sample(const AnimationClip& clip, float time_seconds, bool loop, std::vector<glm::mat4>& out) const;
    // Skinning matrices from model-space bone matrices (× inverse bind).
    void skin(std::vector<glm::mat4>& bones) const;
    // The bone named <code><suffix> (e.g. "R_POINT_TRACK"), else any bone
    // named <3 letters><suffix>, else -1.
    int find_bone(std::string_view suffix) const;
    // CHierarchicalModel::FindBoneByName @0x10047b50 for "<code><name>_DAG": a
    // name ending in DAG is compared on its first strlen - 3 characters, so
    // the first bone starting with "<code><name>_" wins. Falls back to the
    // same name after any three-letter code (a borrowed skeleton).
    int find_bone_prefix(std::string_view name) const;
    // PlayerClient_AttachRiderToMount's dag 0xe on a mount's model (SetBone +0x19c, in this order: "CHEST03_MOUNT01",
    // "<rider code>_MOUNT01", then, when neither is set, "<code>RIDER_DAG"): the exact "<rider code>_MOUNT01", else
    // the exact "CHEST03_MOUNT01" (SetBone @0x10041140 keeps the dag's bone when a name is not found), else
    // find_bone_prefix("RIDER"); -1 when it has none.
    int mount_point_bone(std::string_view rider_code) const;
    // The model's default geometry as a mesh.
    CharacterMesh default_mesh() const;
    struct MemoryBreakdown {
        std::size_t animation_bytes = 0, geometry_bytes = 0, other_bytes = 0;
        std::size_t total() const { return animation_bytes + geometry_bytes + other_bytes; }
    };
    // Shares identical tracks within this model. Copied clips/models already share keys by copy-on-write.
    void compact_animations();
    // Animation includes clips, aliases and skeleton arrays; geometry includes materials. Other includes the
    // object, identifying strings, points and emitters. Uses owned payload sizes with no allocator overhead.
    // A shared set counts track payloads once across multiple models in a cache snapshot.
    MemoryBreakdown memory_breakdown(AnimationTrack::StorageSet* shared_tracks = nullptr) const;
    std::size_t retained_bytes() const; // estimate: object and owned payload sizes, excluding allocator overhead
};

// An archive with validated WLD resource indices; full WLD payloads materialize on first use.
struct CharacterArchive {
    std::shared_ptr<const PfsArchive> archive;
    std::vector<wld::WldFile> wlds;

    // Storage accounting. archive_bytes() is the archive file's size: file-backed bytes live in the page cache and
    // are shared, owned_bytes() of it is in private heap. parsed_bytes() estimates the WLD data itself, which is
    // private; scoped model construction releases decoded payloads, keeping the resource index.
    std::size_t archive_bytes() const;
    std::size_t owned_bytes() const;
    std::size_t parsed_bytes() const;
};

// Opens and parses a character archive; nullptr when the file does not exist.
// Throws std::runtime_error when it is malformed.
std::shared_ptr<const CharacterArchive> load_character_archive(const GameDir& game, std::string_view file);

// The classic character archives the original client loads at start-up, in
// load order: GlobalLoad.txt's _chr entries (phases 1-4), global5/7_chr,
// then Resources/GlobalLoad_chr.txt's S3D entries. EQG entries are skipped.
// Also the equipment archives (GlobalLoad.txt flag E: gequip*, IT models)
// and the extended armour textures global17..23_amr. With `luclin`, the
// Luclin player-model archives enabled in eqclient.ini load after phase 2.
std::vector<std::string> global_character_archives(const GameDir& game, bool luclin = false);
// The resource level each global archive loads at, by its file name (as
// global_character_archives lists it): a GlobalLoad.txt line whose FLAGS start
// with 'T' loads at 0, otherwise at 2 (CDisplay::InitWorld @0x492093); every
// other global archive (Luclin, global5/7, _amr, GlobalLoad_chr.txt) at 0.
std::unordered_map<std::string, int> global_archive_levels(const GameDir& game);
// The Luclin player-model archives eqclient.ini enables
// (global<code>_chr2.s3d, global<code>_chr.s3d), in the client's race order.
std::vector<std::string> luclin_character_archives(const GameDir& game);
// The zone's own character archives: <zone>_chr2, <zone>2_chr, <zone>_chr and
// the S3D entries of <zone>_chr.txt.
std::vector<std::string> zone_character_archives(const GameDir& game, std::string_view zone);
// Whether a "code,file" line of a _chr.txt names an S3D (file.s3d) rather than
// an EQG: RoF2's rule, the code and file differ in their first three letters;
// the live client's lists also name S3Ds that share them ("kob,kob_chr"), so
// such a file is an S3D when file.s3d exists.
bool chr_txt_entry_is_s3d(const GameDir& game, std::string_view code, std::string_view file);

class EqgCharacterStore;
class ModelArchiveStore;
class SharedCharacterCache;

// The names a build looked up in a CharacterLibrary's tables (actors, meshes, tracks, files ...), each as its table's
// letter and the name: what the result depends on. While one is alive on a thread, every lookup on that thread is
// added to it; when it ends its names join the one alive before it (a model built inside a dress counts for both).
// A cached result hands its names on in the same way. SharedCharacterCache keeps results from the global archives
// between zones by them: a zone's own archives that define none of the names cannot change the result.
class CharacterLookups {
public:
    using Names = std::shared_ptr<const std::vector<std::string>>;
    CharacterLookups() : parent_(current_) { current_ = this; }
    ~CharacterLookups();
    CharacterLookups(const CharacterLookups&) = delete;
    CharacterLookups& operator=(const CharacterLookups&) = delete;
    // The names so far, sorted.
    Names names() const;
    static bool active() { return current_ != nullptr; }
    static void note(char table, std::string_view name) {
        if (current_) current_->note_one(table, name);
    }
    static void note(const Names& names);

private:
    void note_one(char table, std::string_view name);
    // Visible here so other translation units need no dynamic TLS initialization wrapper.
    static constinit thread_local CharacterLookups* current_;
    CharacterLookups* parent_;
    std::unordered_set<std::string> names_;
};

// Resources/npct.ini, itmt.ini, NewArmorTagData.txt, NewArmorAdjData.txt,
// PlayerCustomization.txt and eqclient.ini's ActorNewArmorFilter.
std::shared_ptr<const game::AppearanceData> load_appearance_data(const GameDir& game);

namespace detail {
struct TrackLoc {
    const wld::WldFile* wld = nullptr;
    wld::TrackRef ref{};
};
struct FragmentLoc {
    const wld::WldFile* first = nullptr;
    wld::Ref ref = 0;
    const wld::Fragment* fragment() const { return first ? first->at(ref) : nullptr; }
};

// CharacterLibrary's name tables over a list of archives, built archive by archive in load order (see
// kZoneResourceLevel for how a name's definitions resolve).
struct CharacterTables {
    // "HUM" -> the WLD and positive actor reference; no decoded fragment is pinned by indexing.
    std::unordered_map<std::string, FragmentLoc> actors;
    // These exact names belong to the retained immutable WLDs, like TrackLoc's pointers.
    std::unordered_map<std::string_view, TrackLoc> tracks;
    // Every actor definition by WLD and code, not only the one found ("WOL" in wol_v2_chr.wld -> its actor).
    std::map<std::pair<const wld::WldFile*, std::string>, FragmentLoc> actor_wlds;
    // Bone track name -> clip codes that animate it ("HUMPE_TRACK" -> L01, P01 ...).
    std::unordered_map<std::string, std::vector<std::string>> clips_by_bone;
    std::unordered_map<std::string, std::size_t> files; // lower-case name -> archive index
    // "HUMHE01_DMSPRITEDEF" -> the mesh fragment.
    std::unordered_map<std::string, FragmentLoc> meshes;
    // Materials of the Luclin model and armour WLDs ("HUMCH0101_MDF").
    std::unordered_map<std::string, FragmentLoc> luclin_materials;
    std::unordered_map<std::string, FragmentLoc> luclin_parts; // "IT1012"
    // The resource levels each name of actors, luclin_parts, meshes and tracks has a definition at (one bit each).
    std::unordered_map<std::string, std::uint64_t> actor_levels, part_levels, mesh_levels;
    std::unordered_map<std::string_view, std::uint64_t> track_levels;

    // Adds the archive at `index` of the library's list, at resource `level`. With `base`, these tables go on from
    // base's (base's archives come first and these hold only what the new archives add or change): a name takes
    // its level bits and clip list from there. With `names`, every name the archive defines is put in it, as
    // CharacterLookups notes them.
    void add(const CharacterArchive& archive, std::size_t index, int level, const CharacterTables* base,
             std::unordered_set<std::string>* names);
};

// A table seen through a library: `first`, then `second` (either may be null); every lookup is noted
// (CharacterLookups) as `Table` and the name.
template <char Table, class Map>
class TableView {
public:
    using key_type = typename Map::key_type;
    using mapped_type = typename Map::mapped_type;
    TableView(const Map* first, const Map* second) : first_(first), second_(second) {}
    const mapped_type* find(const key_type& k) const {
        note(k);
        for (const Map* m : {first_, second_})
            if (m)
                if (auto it = m->find(k); it != m->end()) return &it->second;
        return nullptr;
    }
    bool contains(const key_type& k) const { return find(k) != nullptr; }

private:
    static void note(std::string_view k) { CharacterLookups::note(Table, k); }
    template <class P>
    static void note(const std::pair<P, std::string>& k) { CharacterLookups::note(Table, k.second); }
    const Map* first_;
    const Map* second_;
};

// Appends a WLD mesh (0x36) to `out`: vertices bound by its vertex pieces, or
// to `owner` when it has none. Materials with the same name and frames are
// shared. Throws std::runtime_error when the pieces do not fit the skeleton.
void append_wld_mesh(const wld::WldFile& wld, const wld::Fragment& mesh, std::size_t bone_count,
                     std::int64_t owner, CharacterMesh& out);

// The ActorClient fields the appearance functions read (game/actor_look.h) for `model` in appearance `a`; `data`
// gives the new-armor filter (null: hero's forge hidden).
game::ActorLook look_of(const CharacterModel& model, const game::Appearance& a, const game::AppearanceData* data);
} // namespace detail

// The graphics DLL's resource level an archive's definitions are added at.
// CResourceManagerData's Add (0x100c77a0) drops a name already present at the
// same level and otherwise inserts at the head of the chain, and Find
// (0x100c7360) takes the first entry with level <= the lookup's maximum (3 at
// almost every call site): so within one level the first loaded definition
// wins, and across levels the latest. StartWorldDisplay's zone `_chr`
// requests pass level 2. CDisplay::InitWorld gives a GlobalLoad.txt line level
// 0 when its FLAGS start with 'T' and 2 otherwise (@0x492093); every NMS line
// starts with T, and GlobalLoad_chr.txt's S3D fallbacks load at 0 as well. So
// zone archives override global definitions.
inline constexpr int kZoneResourceLevel = 2;
inline constexpr int kGlobalResourceLevel = 0;

// The global character archives with their name tables, built once (CharacterArchiveCache) and shared by every zone's
// library, which indexes only its own archives on top.
class CharacterArchiveIndex {
public:
    // `levels` as CharacterLibrary's.
    explicit CharacterArchiveIndex(std::vector<std::shared_ptr<const CharacterArchive>> archives,
                                   std::vector<int> levels = {});
    CharacterArchiveIndex(const CharacterArchiveIndex&) = default;
    CharacterArchiveIndex(CharacterArchiveIndex&&) = default;
    CharacterArchiveIndex& operator=(CharacterArchiveIndex other) noexcept { swap(other); return *this; }
    void swap(CharacterArchiveIndex& other) noexcept;
    const std::vector<std::shared_ptr<const CharacterArchive>>& archives() const { return archives_; }

private:
    friend class CharacterLibrary;
    std::vector<std::shared_ptr<const CharacterArchive>> archives_;
    detail::CharacterTables tables_;
};

// Finds models, animation tracks and textures across a list of archives in
// load order. Actor, mesh and track names resolve as the DLL's resource
// table does (see kZoneResourceLevel); textures by the first archive.
class CharacterLibrary {
public:
    CharacterLibrary() = default;
    CharacterLibrary(const CharacterLibrary&) = default;
    CharacterLibrary(CharacterLibrary&&) = default;
    CharacterLibrary& operator=(CharacterLibrary other) noexcept { swap(other); return *this; }
    void swap(CharacterLibrary& other) noexcept;
    // `levels` gives each archive's resource level (default: kZoneResourceLevel). The archives from
    // `first_zone_archive` on are the zone's own (library_for_zone: after the global ones); what they define is
    // what no result of the shared cache may depend on here (set_shared_cache).
    explicit CharacterLibrary(std::vector<std::shared_ptr<const CharacterArchive>> archives,
                              std::vector<int> levels = {}, std::size_t first_zone_archive = SIZE_MAX);
    // The global archives of `globals` followed by the zone's own (at kZoneResourceLevel): the same tables as the
    // constructor above given both lists with first_zone_archive at the zone's, but only the zone's are indexed here.
    CharacterLibrary(std::shared_ptr<const CharacterArchiveIndex> globals,
                     std::vector<std::shared_ptr<const CharacterArchive>> zone_archives);

    // Loads the global archives and the zone's (zone may be empty); with
    // `luclin`, the Luclin player models eqclient.ini enables.
    static CharacterLibrary load(const GameDir& game, std::string_view zone, bool luclin = false);
    // Opens archives by file name; missing ones are skipped, broken ones logged.
    static std::vector<std::shared_ptr<const CharacterArchive>> open_archives(const GameDir& game,
                                                                             const std::vector<std::string>& names);

    const std::vector<std::shared_ptr<const CharacterArchive>>& archives() const { return archives_; }
    // The shared index of the global archives this library was made on, or null.
    const CharacterArchiveIndex* global_index() const { return globals_.get(); }
    struct CacheMemory {
        std::size_t models = 0, model_bytes = 0;
        std::size_t model_animation_bytes = 0, model_geometry_bytes = 0, model_other_bytes = 0;
        std::size_t appearances = 0, appearance_bytes = 0;
        std::size_t items = 0, item_bytes = 0;
    };
    // Unique model/appearance objects reachable from this library, its shared cache, EQG model cache and opened
    // standalone libraries. Item cache entries own copies and count separately. Excludes maps, lookup records,
    // parsed EQG source meshes/parts, renderer-owned copies and allocator overhead; safe during dressing.
    CacheMemory cache_memory() const;
    bool has_model(std::string_view code) const;
    std::vector<std::string> model_codes() const;

    // Builds (and caches) a model; nullptr when the code has no skeletal
    // actor or the actor is malformed (logged once).
    std::shared_ptr<const CharacterModel> model(std::string_view code) const;
    // Builds a model without the cache; throws std::runtime_error with the reason.
    CharacterModel build(std::string_view code) const;

    // A texture or other file from the archives (name matched case-insensitively),
    // then from the EQG archives opened for EQG models.
    std::optional<std::vector<std::byte>> read_file(std::string_view name) const;
    bool has_file(std::string_view name) const;

    // EQG models (<code>.eqg, Resources/OnDemandResources.txt) for codes
    // without a WLD actor. Shared between the libraries of different zones.
    void set_eqg_store(std::shared_ptr<EqgCharacterStore> store) { eqg_ = std::move(store); }
    // Models no archive above has, from their own <code>_chr.s3d (ModelArchiveStore). Shared like the EQG store.
    void set_model_archive_store(std::shared_ptr<ModelArchiveStore> store) { own_ = std::move(store); }
    // Models, appearances and item models built from the global archives alone, kept between zones' libraries
    // (CharacterArchiveCache): this library takes one when the names it looked up (CharacterLookups) are none its
    // zone archives define, and gives it what it builds that way.
    void set_shared_cache(std::shared_ptr<SharedCharacterCache> cache) { shared_ = std::move(cache); }
    // Whether a result looked up `names` none of which this library's zone archives define (so it is the same here
    // as from the global archives alone).
    bool unaffected(const std::vector<std::string>& names) const;
    bool unaffected(const CharacterLookups::Names& names) const { return !names || unaffected(*names); }
    // Whether the model or dressed mesh at `p` is one of the shared cache's, and this library would hand it out
    // (render::ActorRenderer keeps its GPU copy across zones by this).
    bool shares(const void* p) const;
    // Thread safety: every call that reads or fills a cache (this library's, the shared cache, the EQG and
    // model-archive stores) holds this mutex, the others read only what the constructor made. So a library may be
    // used from several threads (render::ActorRenderer dresses on a worker thread), one such call at a time. The
    // libraries CharacterArchiveCache makes share one, as they share the caches and stores.
    void set_mutex(std::shared_ptr<std::recursive_mutex> m) { mutex_ = std::move(m); }
    // The appearance tables (hero's forge, customization, NPC tints); may be null.
    void set_appearance_data(std::shared_ptr<const game::AppearanceData> data) { appearance_ = std::move(data); }
    const game::AppearanceData* appearance_data() const { return appearance_.get(); }

    // A model's geometry in an appearance (src/assets/character_dress.cpp):
    // armour and face textures, helm and robe meshes, tints, held items.
    // Cached per model and appearance.
    std::shared_ptr<const CharacterMesh> dress(const CharacterModel& model, const game::Appearance& appearance) const;
    // PlayerClient_SetFHEB / SetFHEBColor (game/actor_look.h set_fheb, set_fheb_color) on `model` in appearance `a`:
    // whether the model took the feature (an IT part it has), with the features the call left written back into
    // `a` (src/assets/character_parts.cpp; the player customization window steps through them).
    std::uint32_t try_fheb(const CharacterModel& model, game::Appearance& a, std::uint32_t value, std::uint32_t type,
                           bool color) const;
    // A texture composed of layers ("layers:<file>+<file>...#<tint hex>",
    // the names dress() gives Luclin materials): later layers are blended
    // over the first by their alpha and multiplied by the tint.
    std::optional<Image> layered_texture(std::string_view key, std::string* error = nullptr) const;
    // A material's texture as the renderer uploads it: a "layers:" key (layered_texture), else the file decoded
    // (palette index 0 transparent when `masked`), BMP rows flipped to match the UVs. On failure nullopt, with the
    // reason in `error` (which file was not found or could not be decoded).
    std::optional<Image> texture(std::string_view name, bool masked, std::string* error = nullptr) const;
    // A texture pack's normal or roughness map for the texture `name` (assets/texture_overrides.h), rows as
    // texture()'s; nullopt when there is none (always for a "layers:" key).
    std::optional<Image> texture_map(std::string_view name, TextureMap map) const;
    // The codes the EQG store can build (Resources/OnDemandResources.txt), for tools.
    std::vector<std::string> eqg_model_codes() const;
    // The layer files of a Luclin or armour-archive material ("HUMCH0101_MDF"), or empty.
    std::vector<std::string> luclin_layers(std::string_view material) const;

    // Whether IT<number> exists as an item model.
    bool has_item(std::uint32_t number) const;
    // An item model's geometry in its own space (the grip point at the
    // origin), or nullopt: a WLD actor IT<n> of the equipment archives, else
    // an EQG one (OnDemandResources EQGM, e.g. arxmentisweapons.eqg's
    // IT12753) with its points and actor emitters. `eqg`: its triangles wind
    // the EQG way.
    struct ItemMesh {
        CharacterMesh mesh;
        bool eqg = false;
        std::vector<ModelPoint> points;
        std::vector<ModelEmitter> emitters;
    };
    std::optional<ItemMesh> item_mesh(std::uint32_t number) const;
    // Any WLD actor "<code>_ACTORDEF" of these archives as a static mesh, as item_mesh builds IT<n>, or nullopt
    // (not cached: WorldObjects keeps its models).
    // The original keeps one actor definition table, so CDisplay::CreateActor finds a door's model in the
    // GlobalLoad archives too: global_obj.s3d's POKTELE500, the cities' Plane of Knowledge books.
    std::optional<ItemMesh> actor_mesh(const std::string& code) const;

private:
    using TrackLoc = detail::TrackLoc;
    using FragmentLoc = detail::FragmentLoc;
    using Tables = detail::CharacterTables;
    const TrackLoc* track(const std::string& name) const;

    std::vector<std::shared_ptr<const CharacterArchive>> archives_;
    // The global archives' tables (shared between zones), or null; tables_ holds what the rest of archives_ adds.
    std::shared_ptr<const CharacterArchiveIndex> globals_;
    Tables tables_;
    // A table as the resource rules resolve it: an actor, mesh, track or part this library's own archives took
    // overrides the global one (tables_ first); a file or Luclin material is the first archive's (globals first).
    template <char Table, class Map>
    detail::TableView<Table, Map> view(Map Tables::*table, bool own_first) const {
        const Map* global = globals_ ? &(globals_->tables_.*table) : nullptr;
        const Map* own = &(tables_.*table);
        return own_first ? detail::TableView<Table, Map>(own, global) : detail::TableView<Table, Map>(global, own);
    }
    auto actors() const { return view<'a'>(&Tables::actors, true); }
    auto tracks() const { return view<'t'>(&Tables::tracks, true); }
    auto actor_wlds() const { return view<'a'>(&Tables::actor_wlds, true); }
    auto clips_by_bone() const { return view<'c'>(&Tables::clips_by_bone, true); }
    auto files() const { return view<'f'>(&Tables::files, false); }
    auto meshes() const { return view<'m'>(&Tables::meshes, true); }
    auto luclin_materials() const { return view<'l'>(&Tables::luclin_materials, false); }
    auto luclin_parts() const { return view<'p'>(&Tables::luclin_parts, true); }
    // What the zone's own archives define, as CharacterLookups names them.
    std::unordered_set<std::string> zone_names_;
    // A result with the names it looked up (CharacterLookups), handed on when it is taken from the cache.
    template <class T>
    struct Cached {
        T value;
        CharacterLookups::Names lookups;
    };
    std::shared_ptr<std::recursive_mutex> mutex_ = std::make_shared<std::recursive_mutex>(); // set_mutex
    using Lock = std::lock_guard<std::recursive_mutex>;
    mutable std::unordered_map<std::string, Cached<std::shared_ptr<const CharacterModel>>> cache_;
    mutable std::unordered_map<std::string, Cached<std::shared_ptr<const CharacterMesh>>> dress_cache_;
    mutable std::unordered_map<std::uint32_t, Cached<std::optional<ItemMesh>>> item_cache_;
    std::shared_ptr<SharedCharacterCache> shared_;
    void build_item_mesh(std::uint32_t number, std::optional<ItemMesh>& out) const;
    std::shared_ptr<EqgCharacterStore> eqg_;
    std::shared_ptr<ModelArchiveStore> own_;
    std::shared_ptr<const game::AppearanceData> appearance_;

    CharacterMesh dress_wld(const CharacterModel& model, const game::Appearance& a) const;
    CharacterMesh dress_eqg(const CharacterModel& model, const game::Appearance& a) const;
    // src/assets/character_parts.cpp: the original's ActorClient functions
    // (game/actor_look.h) on Luclin-style models and on new (EQG) models of
    // the classic races (Drakkin).
    friend class DressEnv;
    void dress_luclin(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const;
    void dress_new_model(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const;
    void apply_npc_tint(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const;
    CharacterModel build_actor(const std::string& code, const wld::WldFile& w, const wld::Fragment& actor) const;
    CharacterModel build_static_actor(const std::string& code, const wld::WldFile& w, const wld::ActorDef& actor) const;
    void attach_items(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const;
};

// What zones' libraries share (CharacterLibrary::set_shared_cache): the models, appearances and item models built
// from the global archives alone, each with the names it looked up. The cache keys hold the source: an appearance is
// keyed by the shared model object it dresses, and a library takes an entry only when its own zone archives define
// none of the entry's names, so a zone's override is never served from here, nor kept here.
class SharedCharacterCache {
public:
    template <class T>
    struct Entry {
        T value;
        CharacterLookups::Names lookups;
        std::uint64_t used = 0; // the generation it was last handed out in
    };
    // A shared model or dressed mesh: its names, and for a mesh the model it dresses (valid only with it).
    struct Owner {
        CharacterLookups::Names lookups;
        const CharacterModel* model = nullptr;
    };

    // A new zone's library: what was not handed out since the last call is dropped (memory stays about one zone's
    // worth), then the generation moves on.
    void next_zone();
    std::uint64_t generation() const { return generation_; }

    std::unordered_map<std::string, Entry<std::shared_ptr<const CharacterModel>>> models; // by code
    std::map<std::pair<const CharacterModel*, std::string>, Entry<std::shared_ptr<const CharacterMesh>>> dresses;
    std::unordered_map<std::uint32_t, Entry<std::optional<CharacterLibrary::ItemMesh>>> items;
    std::unordered_map<const void*, Owner> owners; // the models and dressed meshes above, by address

private:
    std::uint64_t generation_ = 1;
};

// Keeps the parsed global archives between zones.
// A live asset folder keeps most models in their own <code>_chr.s3d and loads few of them up front: its
// Resources/GlobalLoad.txt has bea_chr where RoF2's has pbr_chr, so East Commonlands' black bears (PBR) are in no
// archive the zone opens. A model no global, zone or EQG archive has is read from <code>_chr.s3d when the asset
// folder has one, on first use, as a library of that archive alone (its own textures and animations).
class ModelArchiveStore {
public:
    explicit ModelArchiveStore(GameDir game) : game_(std::move(game)) {}
    // The library of <code>_chr.s3d when it exists and has the model; nullptr otherwise. Cached.
    std::shared_ptr<const CharacterLibrary> library(std::string_view code);
    // A library library() has already opened for the code, or nullptr.
    std::shared_ptr<const CharacterLibrary> opened(std::string_view code) const;
    // A file of the archives opened so far.
    std::optional<std::vector<std::byte>> read_file(std::string_view name) const;
    bool has_file(std::string_view name) const;

private:
    friend class CharacterLibrary; // cache diagnostics traverse the persistent standalone libraries
    GameDir game_;
    std::unordered_map<std::string, std::shared_ptr<const CharacterLibrary>> libraries_; // "PBR" -> library or null
};

class CharacterArchiveCache {
public:
    CharacterArchiveCache() = default;
    explicit CharacterArchiveCache(bool luclin) : luclin_(luclin) {}
    // The global archives (loaded on first use) plus the zone's own.
    std::shared_ptr<const CharacterLibrary> library_for_zone(const GameDir& game, std::string_view zone);
    // Load the Luclin player models (set before the first library_for_zone).
    void set_luclin(bool on) { luclin_ = on; }
    // Forgets the global archives, so the next library_for_zone reads them again (the game files' layers changed:
    // another server's assets folder). Libraries handed out keep what they hold.
    void clear() { *this = CharacterArchiveCache{luclin_}; }

private:
    std::shared_ptr<const CharacterArchiveIndex> globals_;
    std::shared_ptr<std::recursive_mutex> mutex_; // the libraries' (CharacterLibrary::set_mutex)
    std::shared_ptr<EqgCharacterStore> eqg_;
    std::shared_ptr<ModelArchiveStore> own_;
    std::shared_ptr<const game::AppearanceData> appearance_;
    std::shared_ptr<SharedCharacterCache> shared_;
    bool loaded_ = false;
    bool luclin_ = false;
};

} // namespace hailstone::assets
