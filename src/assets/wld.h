#pragma once

// WLD files: the classic (S3D) world, object, light and character format.
// Layout and sources: docs/formats/wld.md. In short:
//   header: u32 magic 0x54503d02, u32 version (0x00015500 old, 0x1000c800 new),
//           u32 fragment count, u32 region count, u32 max object bytes,
//           u32 string-table size, u32 string count
//   string table, XOR-encoded with an 8-byte key
//   fragments: {u32 size, u32 type, size bytes of payload}; the payload starts
//           with an i32 name reference
// A reference is a signed 32-bit value: n > 0 is fragment n (1-based), n < 0
// is the string at offset -n of the decoded table, and 0 is "none".
//
// This parser decodes the fragments needed for static zone rendering and
// keeps every other fragment as an opaque entry so references still line up.

#include <array>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/model.h"
#include "core/shared_file_cache.h"

namespace hailstone::assets::wld {

constexpr std::uint32_t kMagic = 0x54503d02;
constexpr std::uint32_t kVersionOld = 0x00015500;
constexpr std::uint32_t kVersionNew = 0x1000c800;
constexpr std::array<std::uint8_t, 8> kStringKey{0x95, 0x3a, 0xc5, 0x2a, 0x95, 0x7a, 0x95, 0x6a};

using Ref = std::int32_t;

// 0x03 BMINFO: texture file names (diffuse first, then optional detail layers).
struct BitmapName {
    std::vector<std::string> files;
};

// 0x04 SIMPLESPRITEDEF: one or more 0x03 frames; animated when it has a delay.
struct BitmapInfo {
    std::uint32_t flags = 0;
    std::uint32_t current_frame = 0;
    std::uint32_t frame_delay_ms = 0;
    std::vector<Ref> frames;
};

// 0x05 SIMPLESPRITE: reference to a 0x04.
struct BitmapInfoRef {
    Ref info = 0;
    std::uint32_t flags = 0;
};

// 0x30 MATERIALDEFINITION.
struct MaterialDef {
    std::uint32_t flags = 0;
    std::uint32_t render_method = 0;
    std::uint32_t rgb_pen = 0;
    float brightness = 0.0f;
    float scaled_ambient = 0.0f;
    Ref bitmap = 0; // 0x05, or 0 for no texture
};

// 0x31 MATERIALPALETTE: a mesh's material slots. Slots are positional.
struct MaterialList {
    std::uint32_t flags = 0;
    std::vector<Ref> materials;
};

inline constexpr std::uint16_t kPolygonPassable = 0x10; // no collision (CTerrain::AddWldRegionMesh: record bit 0)

struct Polygon {
    std::uint16_t flags = 0; // kPolygonPassable
    std::array<std::uint16_t, 3> v{};
};

// 0x36 DMSPRITEDEF2: a mesh. Positions here are final (centre added).
struct MeshDef {
    std::uint32_t flags = 0;
    Ref material_list = 0;
    Ref animation = 0;
    glm::vec3 center{0.0f};
    float max_distance = 0.0f;
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
    std::uint16_t scale_shift = 0;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec3> normals;
    std::vector<std::uint32_t> colors; // as stored: bytes B, G, R, A
    std::vector<Polygon> polygons;
    std::vector<std::array<std::uint16_t, 2>> vertex_pieces;  // {vertex count, bone}
    std::vector<std::array<std::uint16_t, 2>> render_groups;  // {polygon count, material slot}
};

// 0x14 ACTORDEF: an object definition referring to its sprites (0x2d mesh
// references, or 0x11 skeleton references for animated objects).
struct ActorDef {
    std::uint32_t flags = 0;
    std::string callback;
    std::vector<Ref> sprites;
};

// 0x2D DMSPRITE: reference to a 0x36 mesh.
struct MeshRef {
    Ref mesh = 0;
    std::uint32_t flags = 0;
};

// 0x15 ACTORINST: a placed actor.
struct ActorInstance {
    Ref actor = 0; // in objects.wld a string reference to "<NAME>_ACTORDEF"
    std::uint32_t flags = 0;
    Ref sphere = 0;
    bool has_location = false;
    glm::vec3 position{0.0f};
    // Raw angles as stored, in 1/512ths of a full turn, in file order:
    // around Z, around Y, around X.
    glm::vec3 rotation_zyx{0.0f};
    float bounding_radius = 1.0f;
    float scale = 1.0f;
    Ref vertex_colors = 0; // 0x33
};

// 0x32 DMRGBTRACKDEF: per-vertex colours for one placed object.
struct VertexColors {
    std::vector<std::uint32_t> colors; // as stored: bytes B, G, R, A
};

// 0x33 DMRGBTRACK: reference to a 0x32.
struct VertexColorsRef {
    Ref colors = 0;
    std::uint32_t flags = 0;
};

// 0x1B LIGHTDEFINITION.
struct LightDef {
    std::uint32_t flags = 0;
    std::vector<float> levels;
    std::vector<glm::vec3> colors;
};

// 0x1C LIGHT: reference to a 0x1B.
struct LightRef {
    Ref light = 0;
    std::uint32_t flags = 0;
};

// 0x28 POINTLIGHT: a placed light, referring to a 0x1C.
struct PointLight {
    Ref light = 0;
    std::uint32_t flags = 0;
    glm::vec3 position{0.0f};
    float radius = 0.0f;
};

// One key of a 0x12 track: local rotation, translation and uniform scale.
struct TrackFrame {
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // as stored (not normalised)
    glm::vec3 translation{0.0f};
    float scale = 1.0f;
};

// Track keys may stay in an immutable WLD snapshot instead of expanding every packed 16-byte key to a private
// 32-byte TrackFrame. Const reads decode values with ByteReader; fixture/build-time mutation makes an owned copy.
class TrackFrames {
public:
    using value_type = TrackFrame;
    using size_type = std::size_t;
    using iterator = std::vector<TrackFrame>::iterator;
    class const_iterator {
    public:
        using value_type = TrackFrame;
        using reference = TrackFrame;
        using difference_type = std::ptrdiff_t;
        // Dereferencing decodes a value, so to C++17 algorithms this is only an input iterator (as iota_view's);
        // C++20 ranges see a random-access one.
        using iterator_category = std::input_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;
        struct pointer {
            TrackFrame value;
            const TrackFrame* operator->() const { return &value; }
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
        friend class TrackFrames;
        const_iterator(const TrackFrames* owner, size_type index) : owner_(owner), index_(index) {}
        const TrackFrames* owner_ = nullptr;
        size_type index_ = 0;
    };

    TrackFrames() = default;
    TrackFrames(std::initializer_list<TrackFrame> frames) : owned_(frames) {}
    TrackFrames(std::vector<TrackFrame> frames) : owned_(std::move(frames)) {}
    TrackFrames(const TrackFrames&) = default;
    TrackFrames& operator=(const TrackFrames&) = default;
    TrackFrames(TrackFrames&& other) noexcept;
    TrackFrames& operator=(TrackFrames&& other) noexcept;
    TrackFrames& operator=(std::initializer_list<TrackFrame> frames) { assign(frames); return *this; }
    TrackFrames& operator=(std::vector<TrackFrame> frames) {
        owned_ = std::move(frames); reset_encoded(); return *this;
    }

    // Offsets and counts are checked against the owner's bytes before any view is made.
    static TrackFrames from_encoded(ImmutableFileBytes owner, size_type offset, size_type count, bool packed);
    bool encoded() const { return encoded_; }
    bool mapped() const { return encoded_ && backing_.mapped(); }
    // The full owned source may back several tracks; callers deduplicate it by identity for storage accounting.
    std::span<const std::byte> private_encoded_bytes() const {
        return encoded_ && !backing_.mapped() ? backing_.bytes() : std::span<const std::byte>{};
    }
    size_type size() const { return encoded_ ? count_ : owned_.size(); }
    bool empty() const { return size() == 0; }
    size_type capacity() const { return encoded_ ? count_ : owned_.capacity(); }
    size_type decoded_capacity() const { return owned_.capacity(); }
    TrackFrame operator[](size_type i) const;
    TrackFrame at(size_type i) const;
    TrackFrame front() const { return (*this)[0]; }
    TrackFrame back() const { return (*this)[size() - 1]; }
    TrackFrame& operator[](size_type i) { materialize(); return owned_[i]; }
    TrackFrame& at(size_type i);
    TrackFrame& front() { materialize(); return owned_.front(); }
    TrackFrame& back() { materialize(); return owned_.back(); }
    TrackFrame* data() { materialize(); return owned_.data(); }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    iterator begin() { materialize(); return owned_.begin(); }
    iterator end() { materialize(); return owned_.end(); }
    void reserve(size_type count) { materialize(); owned_.reserve(count); }
    void resize(size_type count) { materialize(); owned_.resize(count); }
    void resize(size_type count, const TrackFrame& frame) { materialize(); owned_.resize(count, frame); }
    void push_back(const TrackFrame& frame) { materialize(); owned_.push_back(frame); }
    void push_back(TrackFrame&& frame) { materialize(); owned_.push_back(std::move(frame)); }
    template<class... Args> TrackFrame& emplace_back(Args&&... args) {
        materialize(); return owned_.emplace_back(std::forward<Args>(args)...);
    }
    void clear() { owned_.clear(); reset_encoded(); }
    void assign(size_type count, const TrackFrame& frame) { owned_.assign(count, frame); reset_encoded(); }
    void assign(std::initializer_list<TrackFrame> frames) { owned_.assign(frames); reset_encoded(); }
    template<class It> void assign(It first, It last) { owned_.assign(first, last); reset_encoded(); }

private:
    void materialize();
    void reset_encoded() { backing_ = {}; offset_ = count_ = 0; encoded_ = packed_ = false; }
    std::vector<TrackFrame> owned_;
    ImmutableFileBytes backing_;
    size_type offset_ = 0, count_ = 0;
    bool encoded_ = false, packed_ = false;
};

// 0x12 TRACKDEFINITION: the keys of one skeleton node.
struct TrackDef {
    std::uint32_t flags = 0; // 0x8: packed s16 keys
    TrackFrames frames;
};

// 0x13 TRACKINSTANCE: reference to a 0x12.
struct TrackRef {
    Ref track = 0;
    std::uint32_t flags = 0;
    std::uint32_t frame_ms = 0; // when flags & 1
};

struct SkeletonNode {
    std::string name;
    std::uint32_t flags = 0;
    Ref track = 0;  // 0x13
    Ref sprite = 0; // usually a 0x2d mesh reference, or 0
    std::vector<std::int32_t> children; // node indices
};

// 0x10 HIERARCHICALSPRITEDEF: a skeleton. Meshes either hang off one node
// (SkeletonNode::sprite) or list per-vertex node runs (MeshDef::vertex_pieces).
struct Skeleton {
    std::uint32_t flags = 0;
    float bounding_radius = 0.0f; // when flags & 2 (the actor's bounding radius, CActorDefinition +0x38)
    std::vector<SkeletonNode> nodes;
    std::vector<Ref> meshes; // 0x2d references, when flags & 0x200
};

// 0x11 HIERARCHICALSPRITE: reference to a 0x10.
struct SkeletonRef {
    Ref skeleton = 0;
    std::uint32_t flags = 0;
};

// One node of the 0x21 BSP tree. A node with no children is a leaf; its
// region is a 1-based index into the file's 0x22 fragments (0 = none).
// Children are 1-based node indices (0 = none). A point p is in front when
// dot(normal, p) + distance > 0.
struct BspNode {
    glm::vec3 normal{0.0f};
    float distance = 0.0f;
    std::uint32_t region = 0;
    std::uint32_t front = 0;
    std::uint32_t back = 0;
};

// 0x21 WORLDTREE: the zone's BSP tree (one per zone WLD).
struct BspTree {
    std::vector<BspNode> nodes;
};

// 0x22 REGION: one BSP region. Only its position in the file matters here
// (BSP leaves and 0x29 lists refer to regions by their order).
struct BspRegion {
    std::uint32_t flags = 0;
};

// 0x29 ZONE: a tag ("WTN_...", "DRNTP...") for a set of regions. The tag is
// the XOR-encoded payload string when present, else the fragment name.
struct RegionTag {
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> regions; // 0-based region indices
    std::string tag;
};

// 0x35 GLOBALAMBIENTLIGHTDEF: the zone's global ambient colour. It has no
// name reference; the payload is one colour, stored B, G, R, A.
// CS3DFile::LoadGlobalAmbient @0x1001b6a0 passes it to CSceneGraph vtable
// +0xd0, which feeds the shaders' SpecialAmbient (docs/formats/wld.md).
struct GlobalAmbient {
    std::uint32_t color = 0;
};

// 0x26 BLITSPRITEDEF: a particle cloud's sprite. 12 bytes after the name in
// every shipped archive: flags (0 throughout), a full 32-bit 0x05 reference and
// a material word, 0x80000017 throughout, a negative alias into the renderer's
// material table (OpenEQ docs/WLD_PARTICLE_MATERIALS.md; texture resolver
// 0x1001d520 handles kind 0x26 by following the 0x05 -> 0x04 -> 0x03 chain).
struct ParticleSprite {
    std::uint32_t flags = 0;
    Ref sprite = 0; // 0x05
    std::uint32_t material = 0;
};

// 0x34 PCLOUDDEF: a particle emitter hung on a skeleton node (the skeleton
// reader's third attachment kind next to 0x2d meshes and 0x11 skeletons).
// An 80-byte fixed body after the flags-bearing first word, then optional
// fields by flags: 0x01 two float triples, 0x02 24 skipped bytes, 0x04 the
// sprite reference. Offsets are body offsets after the name reference; the
// meanings are the ones the graphics DLL's converter gives them
// (game::particle_cloud_definition, docs/formats/particles.md).
struct ParticleCloud {
    std::uint32_t flags = 0;    // +0x00 optional fields
    std::uint32_t selector = 0; // +0x04 copied to the emitter definition's +0x13c (3 throughout)
    std::uint32_t mode = 0;     // +0x08 emission shape, 0..5
    std::uint32_t bits = 0;     // +0x0c behaviour bits (0x80 radius, 0x100 fade, 0x4000/0x8000 follow, 0x10000 scale)
    std::uint32_t capacity = 0; // +0x10 particles at most
    std::array<float, 4> extra{}; // +0x14 gravity, then three floats copied to +0x148..+0x150
    std::uint32_t duration_ms = 0; // +0x24 0 = indefinite
    float radius = 0.0f;           // +0x28 shape radius (modes 1, 2, 4, 5)
    float angle = 0.0f;            // +0x2c spread, 512ths of a turn (mode 3)
    std::uint32_t lifetime_ms = 0; // +0x30
    float speed = 0.0f;            // +0x34
    glm::vec3 direction{0.0f, 0.0f, 1.0f}; // +0x38..+0x40 in the node's frame
    std::uint32_t interval_ms = 0; // +0x44 between emissions
    float size = 1.0f;             // +0x48 the quad's full edge
    std::uint32_t color = 0;       // +0x4c stored B, G, R in the low three bytes; the top byte is not used
    std::array<glm::vec3, 2> ranges{}; // flag 0x01: box corners (mode 0)
    // flag 0x04: the 0x26 sprite. The DLL's readers load only the low byte of
    // this word (zero-extended at the texture read), which names the right fragment
    // only for the first definitions of a file; later same-name clouds reuse
    // the first one's registered definition by name before that read. The
    // full reference is kept here and resolves to the same sprite.
    Ref sprite = 0;
};

// Rare large payloads must not set the size of every fragment: tracks and
// small references account for most entries. Copies still own independent
// parsed values, including their mutable vectors.
template <typename T> class FragmentBox {
public:
    FragmentBox(T value) : value_(std::make_unique<T>(std::move(value))) {}
    FragmentBox(const FragmentBox& other)
        : value_(other.value_ ? std::make_unique<T>(*other.value_) : nullptr) {}
    FragmentBox& operator=(const FragmentBox& other) {
        if (this != &other) {
            FragmentBox copy(other);
            value_.swap(copy.value_);
        }
        return *this;
    }
    FragmentBox(FragmentBox&&) noexcept = default;
    FragmentBox& operator=(FragmentBox&&) noexcept = default;
    T* get() noexcept { return value_.get(); }
    const T* get() const noexcept { return value_.get(); }

private:
    std::unique_ptr<T> value_;
};

using Payload = std::variant<std::monostate, BitmapName, BitmapInfo, BitmapInfoRef, MaterialDef,
                             MaterialList, FragmentBox<MeshDef>, ActorDef, MeshRef, ActorInstance, VertexColors,
                             VertexColorsRef, LightDef, LightRef, PointLight, TrackDef, TrackRef,
                             Skeleton, SkeletonRef, BspTree, BspRegion, RegionTag, GlobalAmbient,
                             ParticleSprite, FragmentBox<ParticleCloud>>;

struct Fragment {
    std::uint32_t type = 0;
    std::string name;
    std::size_t offset = 0; // file offset of the fragment header
    Payload data;           // monostate when the type is unsupported or failed to parse

    template <typename T> T* get() noexcept {
        if constexpr (std::is_same_v<T, MeshDef> || std::is_same_v<T, ParticleCloud>) {
            auto* box = std::get_if<FragmentBox<T>>(&data);
            return box ? box->get() : nullptr;
        } else {
            return std::get_if<T>(&data);
        }
    }
    template <typename T> const T* get() const noexcept {
        if constexpr (std::is_same_v<T, MeshDef> || std::is_same_v<T, ParticleCloud>) {
            const auto* box = std::get_if<FragmentBox<T>>(&data);
            return box ? box->get() : nullptr;
        } else {
            return std::get_if<T>(&data);
        }
    }
    template <typename T> bool holds() const noexcept { return get<T>() != nullptr; }
    template <typename T> T& as() {
        if (auto* value = get<T>()) return *value;
        throw std::bad_variant_access{};
    }
    template <typename T> const T& as() const {
        if (const auto* value = get<T>()) return *value;
        throw std::bad_variant_access{};
    }
};

class WldFile {
public:
    struct Deferred; // a deferred WLD's shared state (wld.cpp only)
    // Throws std::runtime_error when the header or the fragment framing is
    // malformed. A fragment whose payload does not fit its declared size is
    // kept as an empty entry and listed in errors().
    static WldFile parse(std::span<const std::byte> data, std::string label = "<memory>");
    // A mapped snapshot (the shared cache's) keeps its encoded tracks as read-only views into the mapping. Private
    // bytes are decoded as parse(span) does and freed on return, unless `keep_private` (the fuzzers, to reach the
    // encoded-key reader without a cache).
    static WldFile parse(ImmutableFileBytes data, std::string label = "<memory>", bool keep_private = false);

    // Validate once and keep only the named character-resource index. The loader must return the same immutable
    // bytes on every call. Full fragments decode on first use; ReadScope can reclaim them. Zone WLDs use parse().
    static WldFile deferred(std::function<std::vector<std::byte>()> loader, std::string label);
    // A loader returning mapped bytes lets decoded tracks, and models built from them, keep encoded views.
    static WldFile deferred(std::function<ImmutableFileBytes()> loader, std::string label);
    using IndexVisitor = std::function<void(Ref, std::uint32_t, const std::string&, const TrackRef*)>;
    // Only successfully decoded named character resource types (0x13, 0x14, 0x30, 0x36), in fragment order.
    void visit_character_index(const IndexVisitor& visitor) const;
    bool materialized() const;
    struct DecodeStats {
        std::size_t decodes = 0, releases = 0;
        std::uint64_t released_bytes = 0;
    };
    DecodeStats decode_stats() const;

    // Raw fragment pointers obtained inside a scope live until its outermost scope ends on this thread. Each
    // touched deferred WLD is leased once; when the last concurrent lease ends, its decoded payload goes to the
    // retained set below (or is released at once when that is off or the payload is larger than it). Nested builds
    // share the outer lease. Unscoped reads pin their payload for the WLD's life, preserving the raw-pointer API.
    // Models returned by CharacterLibrary copy the source data and do not carry these pointers out of the scope.
    class ReadScope {
    public:
        ReadScope();
        ~ReadScope();
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;
    private:
        friend class WldFile;
        static thread_local ReadScope* current_;
        ReadScope* parent_ = nullptr;
        std::vector<std::shared_ptr<Deferred>> held_;
    };

    // Retained decoded payloads: a deferred WLD whose last lease ended keeps its decoded data while the estimated
    // bytes (retained_bytes()) of all such WLDs, process-wide, stay within the budget; past it the least recently
    // released are released. The next lease of a retained WLD takes it back without decoding again: a cache miss in
    // CharacterLibrary (a model, a dressing) does not inflate and parse global_chr.wld once more for each model. A
    // WLD that is destroyed leaves the set. 0 turns retention off (released as the last lease ends).
    static constexpr std::size_t kDefaultRetainBudget = std::size_t{128} << 20;
    static void set_retain_budget(std::size_t bytes); // releases at once what no longer fits
    static std::size_t retain_budget();
    static std::size_t retained_total(); // the estimated bytes held by the retained set now
    static void release_retained();      // releases the whole set (it fills again with the next leases)

    const std::string& label() const { return label_; }
    bool new_format() const { return new_format_; }
    const std::vector<Fragment>& fragments() const { return decoded().fragments_; }

    // A positive reference; nullptr when out of range or not positive.
    const Fragment* at(Ref ref) const;
    // Any reference: fragment index, or a named fragment for a string reference.
    const Fragment* resolve(Ref ref) const;
    // The first fragment with this name. Names can repeat (Plane of Knowledge's
    // object WLD has later duplicates); the first definition is used, as in OpenEQ.
    const Fragment* find(std::string_view name) const;
    // String at a negative reference; empty when invalid, cut at kMaxStringLength.
    std::string string_at(Ref ref) const;
    static constexpr std::size_t kMaxStringLength = 256;

    template <typename T> const T* get(Ref ref) const {
        const Fragment* f = resolve(ref);
        return f ? f->get<T>() : nullptr;
    }

    struct Error {
        std::size_t index = 0; // 0-based fragment index
        std::uint32_t type = 0;
        std::string what;
    };
    const std::vector<Error>& errors() const { return errors_; }
    // Fragment types this parser does not decode, with counts.
    const std::map<std::uint32_t, std::size_t>& unknown_types() const { return unknown_; }
    // An estimate of the private memory this file retains: the decoded string table, the fragment objects and the
    // vectors and strings inside their payloads (skeleton nodes and their child lists, meshes, owned track frames),
    // plus private encoded backing once. Indices, diagnostics, capacities, allocator overhead and mapped page-cache
    // pages are not counted, so it compares categories, not exact residency.
    std::size_t retained_bytes() const;

private:
    std::shared_ptr<Deferred> deferred_;
    const WldFile& decoded() const;
    static WldFile parse_impl(std::span<const std::byte> data, std::string label, ImmutableFileBytes backing);
    ImmutableFileBytes backing_;
    std::string label_;
    bool new_format_ = false;
    std::vector<std::uint8_t> strings_; // decoded string table
    std::vector<Fragment> fragments_;
    // Sorted unique names refer back to fragment-owned strings. Indices keep
    // ordinary WldFile copies and moves independent of the source's storage.
    std::vector<std::uint32_t> by_name_;
    std::vector<Error> errors_;
    std::map<std::uint32_t, std::size_t> unknown_;
};

// Drawable conversion (src/assets/wld_zone.cpp).

// Maps a 0x30 render method to a blend mode (table in docs/formats/wld.md).
BlendMode blend_mode(std::uint32_t render_method);

// Converts a stored B,G,R,A colour to model.h's 0xAABBGGRR.
std::uint32_t bgra_to_rgba(std::uint32_t stored);

// Builds a drawable mesh from a 0x36 fragment. Polygon runs address the
// material list by position, so empty or unresolved slots are kept while the
// runs are resolved; polygons are then grouped into one MeshPart per used
// slot, in slot order, and Mesh::materials keeps only the used slots.
Mesh build_mesh(const WldFile& wld, const MeshDef& def, std::string name);

// Builds a static mesh from a skeleton in its first (rest) pose: each node's
// frame 0 is composed parent * T * R * S, and each mesh is moved by its node
// (whole mesh) or by its vertex runs. Returns nullopt when the skeleton is
// malformed or a mesh cannot be tied to a node.
std::optional<Mesh> build_skeleton_pose(const WldFile& wld, const Skeleton& skeleton, std::string name);

// Each node's rest-pose matrix in actor space (frame 0, parent * T * R * S),
// or nullopt for a malformed hierarchy (a bad child index, two parents, a cycle).
std::optional<std::vector<glm::mat4>> skeleton_rest_pose(const WldFile& wld, const Skeleton& skeleton);

// A skeleton node carrying a 0x34 particle cloud.
struct ParticleAttachment {
    const Fragment* cloud = nullptr; // the 0x34
    glm::mat4 node{1.0f};            // the node's rest-pose matrix in actor space
};
// The particle nodes of a skeleton, in node order; empty when it has none or
// its hierarchy is malformed.
std::vector<ParticleAttachment> skeleton_particles(const WldFile& wld, const Skeleton& skeleton);

// Object-to-world matrix for a placed actor: T(position) * Rz(z) * Ry(-y) * Rx(x) * S(scale),
// right-handed rotations about the Z-up axes, angles in 1/512ths of a turn.
// See docs/formats/wld.md for the evidence behind the signs.
glm::mat4 placement_transform(const ActorInstance& inst);

// XOR-decodes a string-table or bitmap-name buffer (the key restarts at 0).
std::vector<std::uint8_t> decode_string_bytes(std::span<const std::byte> in);

} // namespace hailstone::assets::wld
