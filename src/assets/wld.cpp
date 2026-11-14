#include "assets/wld.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <stdexcept>
#include <type_traits>

#include "core/byte_io.h"
#include "core/log.h"

namespace hailstone::assets::wld {

namespace {

glm::vec3 read_vec3(ByteReader& r) {
    float x = r.f32();
    float y = r.f32();
    float z = r.f32();
    return {x, y, z};
}

// Guards counts read from the file against the bytes left in the fragment, so
// a corrupt count fails fast instead of allocating gigabytes.
std::size_t checked_count(ByteReader& r, std::uint32_t count, std::size_t min_item_size) {
    if (min_item_size && count > r.remaining() / min_item_size)
        throw std::out_of_range("count exceeds fragment size");
    return count;
}

BitmapName read_bitmap_name(ByteReader& r) {
    BitmapName b;
    // The stored count is one less than the number of names.
    std::size_t count = checked_count(r, r.le<std::uint32_t>(), 2) + 1;
    for (std::size_t i = 0; i < count; ++i) {
        auto len = r.le<std::uint16_t>();
        auto bytes = decode_string_bytes(r.bytes(len));
        std::size_t n = 0;
        while (n < bytes.size() && bytes[n] != 0) ++n;
        b.files.emplace_back(reinterpret_cast<const char*>(bytes.data()), n);
    }
    return b;
}

BitmapInfo read_bitmap_info(ByteReader& r) {
    BitmapInfo b;
    b.flags = r.le<std::uint32_t>();
    std::size_t count = checked_count(r, r.le<std::uint32_t>(), 4);
    if (b.flags & 0x20) b.current_frame = r.le<std::uint32_t>();
    if (b.flags & 0x08) b.frame_delay_ms = r.le<std::uint32_t>();
    for (std::size_t i = 0; i < count; ++i) b.frames.push_back(r.le<std::int32_t>());
    return b;
}

MaterialDef read_material(ByteReader& r) {
    MaterialDef m;
    m.flags = r.le<std::uint32_t>();
    m.render_method = r.le<std::uint32_t>();
    m.rgb_pen = r.le<std::uint32_t>();
    m.brightness = r.f32();
    m.scaled_ambient = r.f32();
    // The texture reference comes before the optional pair (flag 2), not after
    // (OpenEQ docs/ARCHITECTURE.md, libeq MaterialDef).
    m.bitmap = r.le<std::int32_t>();
    return m;
}

MaterialList read_material_list(ByteReader& r) {
    MaterialList m;
    m.flags = r.le<std::uint32_t>();
    std::size_t count = checked_count(r, r.le<std::uint32_t>(), 4);
    for (std::size_t i = 0; i < count; ++i) m.materials.push_back(r.le<std::int32_t>());
    return m;
}

MeshDef read_mesh(ByteReader& r, bool new_format) {
    MeshDef m;
    m.flags = r.le<std::uint32_t>();
    m.material_list = r.le<std::int32_t>();
    m.animation = r.le<std::int32_t>();
    r.skip(8); // two more references, unused for static meshes
    m.center = read_vec3(r);
    r.skip(12); // unknown, usually zero
    m.max_distance = r.f32();
    m.bounds_min = read_vec3(r);
    m.bounds_max = read_vec3(r);
    auto vertex_count = r.le<std::uint16_t>();
    auto uv_count = r.le<std::uint16_t>();
    auto normal_count = r.le<std::uint16_t>();
    auto color_count = r.le<std::uint16_t>();
    auto polygon_count = r.le<std::uint16_t>();
    auto piece_count = r.le<std::uint16_t>();
    auto group_count = r.le<std::uint16_t>();
    r.skip(2); // vertex material group count
    r.skip(2); // mesh-op count
    m.scale_shift = r.le<std::uint16_t>();
    float scale = 1.0f / static_cast<float>(1u << (m.scale_shift & 31));

    checked_count(r, vertex_count, 6);
    m.positions.reserve(vertex_count);
    for (std::size_t i = 0; i < vertex_count; ++i) {
        float x = r.le<std::int16_t>();
        float y = r.le<std::int16_t>();
        float z = r.le<std::int16_t>();
        m.positions.push_back(m.center + glm::vec3(x, y, z) * scale);
    }
    checked_count(r, uv_count, new_format ? 8 : 4);
    m.uvs.reserve(uv_count);
    for (std::size_t i = 0; i < uv_count; ++i) {
        if (new_format) {
            float u = r.f32();
            float v = r.f32();
            m.uvs.emplace_back(u, v);
        } else {
            float u = r.le<std::int16_t>();
            float v = r.le<std::int16_t>();
            m.uvs.emplace_back(u / 256.0f, v / 256.0f);
        }
    }
    checked_count(r, normal_count, 3);
    m.normals.reserve(normal_count);
    for (std::size_t i = 0; i < normal_count; ++i) {
        float x = r.le<std::int8_t>();
        float y = r.le<std::int8_t>();
        float z = r.le<std::int8_t>();
        m.normals.push_back(glm::vec3(x, y, z) / 127.0f);
    }
    checked_count(r, color_count, 4);
    m.colors.reserve(color_count);
    for (std::size_t i = 0; i < color_count; ++i) m.colors.push_back(r.le<std::uint32_t>());
    checked_count(r, polygon_count, 8);
    m.polygons.reserve(polygon_count);
    for (std::size_t i = 0; i < polygon_count; ++i) {
        Polygon p;
        p.flags = r.le<std::uint16_t>();
        for (auto& v : p.v) v = r.le<std::uint16_t>();
        m.polygons.push_back(p);
    }
    checked_count(r, piece_count, 4);
    for (std::size_t i = 0; i < piece_count; ++i) {
        auto n = r.le<std::uint16_t>();
        auto bone = r.le<std::uint16_t>();
        m.vertex_pieces.push_back({n, bone});
    }
    checked_count(r, group_count, 4);
    for (std::size_t i = 0; i < group_count; ++i) {
        auto n = r.le<std::uint16_t>();
        auto slot = r.le<std::uint16_t>();
        m.render_groups.push_back({n, slot});
    }
    // Vertex material groups and mesh ops follow; not needed for drawing.
    return m;
}

ActorDef read_actor_def(ByteReader& r, const WldFile& wld) {
    ActorDef a;
    a.flags = r.le<std::uint32_t>();
    a.callback = wld.string_at(r.le<std::int32_t>());
    std::size_t action_count = checked_count(r, r.le<std::uint32_t>(), 4);
    std::size_t sprite_count = checked_count(r, r.le<std::uint32_t>(), 4);
    r.skip(4); // bounds reference
    if (a.flags & 0x1) r.skip(4);  // current action
    if (a.flags & 0x2) r.skip(28); // location
    // Each action: u32 level-of-detail count, then that many {u32, f32}
    // (OpenEQ, LanternExtractor). Shipped files only use a count of one.
    for (std::size_t i = 0; i < action_count; ++i) {
        std::size_t lods = checked_count(r, r.le<std::uint32_t>(), 8);
        r.skip(lods * 8);
    }
    for (std::size_t i = 0; i < sprite_count; ++i) a.sprites.push_back(r.le<std::int32_t>());
    return a;
}

ActorInstance read_actor_instance(ByteReader& r) {
    ActorInstance a;
    a.actor = r.le<std::int32_t>();
    a.flags = r.le<std::uint32_t>();
    a.sphere = r.le<std::int32_t>();
    if (a.flags & 0x1) r.skip(4); // current action
    if (a.flags & 0x2) {
        a.has_location = true;
        a.position = read_vec3(r);
        a.rotation_zyx = read_vec3(r);
        r.skip(4);
    }
    if (a.flags & 0x4) a.bounding_radius = r.f32();
    if (a.flags & 0x8) a.scale = r.f32();
    if (a.flags & 0x10) r.skip(4); // sound name reference
    if (a.flags & 0x100) a.vertex_colors = r.le<std::int32_t>();
    // A user-data string follows; unused.
    return a;
}

VertexColors read_vertex_colors(ByteReader& r) {
    VertexColors v;
    r.skip(4);
    std::size_t count = r.le<std::uint32_t>();
    r.skip(12);
    checked_count(r, static_cast<std::uint32_t>(count), 4);
    v.colors.reserve(count);
    for (std::size_t i = 0; i < count; ++i) v.colors.push_back(r.le<std::uint32_t>());
    return v;
}

TrackFrame read_track_frame(ByteReader& r, bool packed) {
    TrackFrame f;
    if (packed) {
        // s16 w, x, y, z (/16384), s16 translation (/256), u16 scale (/256);
        // the scale is unsigned (OpenEQ docs/WLD_PACKED_SCALE.md).
        float q[4];
        for (float& c : q) c = static_cast<float>(r.le<std::int16_t>()) / 16384.0f;
        f.rotation = glm::quat(q[0], q[1], q[2], q[3]);
        for (int k = 0; k < 3; ++k) f.translation[k] = static_cast<float>(r.le<std::int16_t>()) / 256.0f;
        f.scale = static_cast<float>(r.le<std::uint16_t>()) / 256.0f;
    } else {
        f.scale = r.f32();
        f.translation = read_vec3(r);
        float q[4];
        for (float& c : q) c = r.f32();
        f.rotation = glm::quat(q[0], q[1], q[2], q[3]);
    }
    return f;
}

TrackDef read_track_def(ByteReader& r, const ImmutableFileBytes* backing, std::size_t payload_offset) {
    TrackDef t;
    t.flags = r.le<std::uint32_t>();
    bool packed = t.flags & 0x8;
    const std::size_t stride = packed ? 16 : 32;
    std::size_t count = checked_count(r, r.le<std::uint32_t>(), stride);
    if (backing) {
        const auto offset = payload_offset + r.position();
        r.skip(count * stride);
        t.frames = TrackFrames::from_encoded(*backing, offset, count, packed);
    } else {
        t.frames.reserve(count);
        for (std::size_t i = 0; i < count; ++i) t.frames.push_back(read_track_frame(r, packed));
    }
    return t;
}

Skeleton read_skeleton(ByteReader& r, const WldFile& wld) {
    Skeleton s;
    s.flags = r.le<std::uint32_t>();
    std::size_t count = checked_count(r, r.le<std::uint32_t>(), 20);
    r.skip(4); // collision volume reference
    if (s.flags & 0x1) r.skip(12); // centre offset
    if (s.flags & 0x2) s.bounding_radius = r.f32();
    s.nodes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        SkeletonNode n;
        n.name = wld.string_at(r.le<std::int32_t>());
        n.flags = r.le<std::uint32_t>();
        n.track = r.le<std::int32_t>();
        n.sprite = r.le<std::int32_t>();
        std::size_t children = checked_count(r, r.le<std::uint32_t>(), 4);
        for (std::size_t c = 0; c < children; ++c) n.children.push_back(r.le<std::int32_t>());
        s.nodes.push_back(std::move(n));
    }
    if (s.flags & 0x200) {
        std::size_t meshes = checked_count(r, r.le<std::uint32_t>(), 4);
        for (std::size_t i = 0; i < meshes; ++i) s.meshes.push_back(r.le<std::int32_t>());
    }
    return s;
}

// The bulk 0x34 reader's field order (it builds the converter's input record
// from these words; see ParticleCloud).
ParticleCloud read_particle_cloud(ByteReader& r) {
    ParticleCloud c;
    c.flags = r.le<std::uint32_t>();
    c.selector = r.le<std::uint32_t>();
    c.mode = r.le<std::uint32_t>();
    c.bits = r.le<std::uint32_t>();
    c.capacity = r.le<std::uint32_t>();
    for (float& f : c.extra) f = r.f32();
    c.duration_ms = r.le<std::uint32_t>();
    c.radius = r.f32();
    c.angle = r.f32();
    c.lifetime_ms = r.le<std::uint32_t>();
    c.speed = r.f32();
    c.direction = read_vec3(r);
    c.interval_ms = r.le<std::uint32_t>();
    c.size = r.f32();
    c.color = r.le<std::uint32_t>();
    if (c.flags & 0x1) {
        c.ranges[0] = read_vec3(r);
        c.ranges[1] = read_vec3(r);
    }
    if (c.flags & 0x2) r.skip(24);
    if (c.flags & 0x4) c.sprite = r.le<std::int32_t>();
    return c;
}

LightDef read_light_def(ByteReader& r) {
    LightDef l;
    l.flags = r.le<std::uint32_t>();
    std::size_t frames = checked_count(r, r.le<std::uint32_t>(), 4);
    if (l.flags & 0x1) r.skip(4); // current frame
    if (l.flags & 0x2) r.skip(4); // sleep
    if (l.flags & 0x4)
        for (std::size_t i = 0; i < frames; ++i) l.levels.push_back(r.f32());
    if (l.flags & 0x10)
        for (std::size_t i = 0; i < frames; ++i) l.colors.push_back(read_vec3(r));
    return l;
}

// 0x21: u32 node count, then {f32 normal x, y, z, f32 distance, u32 region,
// u32 front, u32 back} per node (libeq WorldTree, OpenEQ bsp_regions.rs).
BspTree read_bsp_tree(ByteReader& r) {
    BspTree t;
    std::size_t count = checked_count(r, r.le<std::uint32_t>(), 28);
    t.nodes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        BspNode n;
        n.normal = read_vec3(r);
        n.distance = r.f32();
        n.region = r.le<std::uint32_t>();
        n.front = r.le<std::uint32_t>();
        n.back = r.le<std::uint32_t>();
        t.nodes.push_back(n);
    }
    return t;
}

// 0x29: u32 flags, u32 count, count × u32 region index (0-based), u32 tag
// length, tag bytes XOR-encoded; a zero length means the fragment name is the
// tag (OpenEQ bsp_regions.rs, LanternExtractor BspRegionType.cs).
RegionTag read_region_tag(ByteReader& r, std::string_view name) {
    RegionTag t;
    t.flags = r.le<std::uint32_t>();
    std::size_t count = checked_count(r, r.le<std::uint32_t>(), 4);
    t.regions.reserve(count);
    for (std::size_t i = 0; i < count; ++i) t.regions.push_back(r.le<std::uint32_t>());
    std::size_t len = checked_count(r, r.le<std::uint32_t>(), 1);
    if (len == 0) {
        t.tag = std::string(name);
    } else {
        auto bytes = decode_string_bytes(r.bytes(len));
        std::size_t n = 0;
        while (n < bytes.size() && bytes[n] != 0) ++n;
        t.tag.assign(reinterpret_cast<const char*>(bytes.data()), n);
    }
    return t;
}

} // namespace

TrackFrames::TrackFrames(TrackFrames&& other) noexcept
    : owned_(std::move(other.owned_)), backing_(std::move(other.backing_)),
      offset_(std::exchange(other.offset_, 0)), count_(std::exchange(other.count_, 0)),
      encoded_(std::exchange(other.encoded_, false)), packed_(std::exchange(other.packed_, false)) {
    other.owned_.clear();
}

TrackFrames& TrackFrames::operator=(TrackFrames&& other) noexcept {
    if (this != &other) {
        owned_ = std::move(other.owned_);
        backing_ = std::move(other.backing_);
        offset_ = std::exchange(other.offset_, 0);
        count_ = std::exchange(other.count_, 0);
        encoded_ = std::exchange(other.encoded_, false);
        packed_ = std::exchange(other.packed_, false);
        other.owned_.clear();
    }
    return *this;
}

TrackFrames TrackFrames::from_encoded(ImmutableFileBytes owner, size_type offset, size_type count, bool packed) {
    const auto bytes = owner.bytes();
    const size_type stride = packed ? 16 : 32;
    if (offset > bytes.size() || count > (bytes.size() - offset) / stride)
        throw std::out_of_range("WLD track frames exceed their backing");
    TrackFrames out;
    out.backing_ = std::move(owner);
    out.offset_ = offset;
    out.count_ = count;
    out.encoded_ = true;
    out.packed_ = packed;
    return out;
}

TrackFrame TrackFrames::operator[](size_type i) const {
    if (!encoded_) return owned_[i];
    if (i >= count_) throw std::out_of_range("WLD track frame index");
    const size_type stride = packed_ ? 16 : 32;
    ByteReader r(backing_.bytes().subspan(offset_ + i * stride, stride));
    return read_track_frame(r, packed_);
}

TrackFrame TrackFrames::at(size_type i) const {
    if (i >= size()) throw std::out_of_range("WLD track frame index");
    return (*this)[i];
}

TrackFrame& TrackFrames::at(size_type i) {
    if (i >= size()) throw std::out_of_range("WLD track frame index");
    materialize();
    return owned_[i];
}

void TrackFrames::materialize() {
    if (!encoded_) return;
    std::vector<TrackFrame> decoded;
    decoded.reserve(count_);
    const TrackFrames& view = *this;
    for (size_type i = 0; i < count_; ++i) decoded.push_back(view[i]);
    owned_ = std::move(decoded);
    reset_encoded();
}

std::vector<std::uint8_t> decode_string_bytes(std::span<const std::byte> in) {
    std::vector<std::uint8_t> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
        out[i] = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(in[i]) ^ kStringKey[i % kStringKey.size()]);
    return out;
}

WldFile WldFile::parse(std::span<const std::byte> data, std::string label) {
    return parse_impl(data, std::move(label), {});
}

WldFile WldFile::parse(ImmutableFileBytes data, std::string label, bool keep_private) {
    const auto bytes = data.bytes();
    // Only a mapping is worth keeping: it is shared with other clients and its pages can be dropped. Private bytes
    // (no cache, or the cache's fallback) are decoded and freed with `data`, as parse(span) does.
    if (!data.mapped() && !keep_private) return parse_impl(bytes, std::move(label), {});
    return parse_impl(bytes, std::move(label), std::move(data));
}

WldFile WldFile::parse_impl(std::span<const std::byte> data, std::string label, ImmutableFileBytes backing) {
    WldFile w;
    w.backing_ = std::move(backing);
    w.label_ = std::move(label);
    ByteReader r(data);
    std::uint32_t fragment_count = 0;
    try {
        if (r.le<std::uint32_t>() != kMagic) throw std::runtime_error(w.label_ + ": not a WLD file");
        auto version = r.le<std::uint32_t>();
        if (version != kVersionOld && version != kVersionNew)
            log::debug("{}: unusual WLD version {:#x}", w.label_, version);
        w.new_format_ = version != kVersionOld;
        fragment_count = r.le<std::uint32_t>();
        r.skip(8); // region count, max object bytes
        auto string_size = r.le<std::uint32_t>();
        r.skip(4); // string count
        w.strings_ = decode_string_bytes(r.bytes(string_size));
        if (fragment_count > r.remaining() / 8) throw std::runtime_error(w.label_ + ": bad WLD fragment count");
        w.fragments_.reserve(fragment_count);
    } catch (const std::out_of_range&) {
        throw std::runtime_error(w.label_ + ": truncated WLD header");
    }

    for (std::size_t i = 0; i < fragment_count; ++i) {
        Fragment f;
        f.offset = r.position();
        std::size_t size = 0;
        try {
            size = r.le<std::uint32_t>();
            f.type = r.le<std::uint32_t>();
            if (size > r.remaining()) throw std::out_of_range("");
        } catch (const std::out_of_range&) {
            throw std::runtime_error(w.label_ + ": fragment " + std::to_string(i + 1) + " runs past the end");
        }
        // Each fragment is parsed with a reader bounded to its declared size
        // (OpenEQ docs/WLD_FRAGMENT_BOUNDARIES.md).
        ByteReader fr(r.bytes(size));
        try {
            if (f.type != 0x35 && fr.remaining() >= 4) f.name = w.string_at(fr.le<std::int32_t>());
            switch (f.type) {
            case 0x03: f.data = read_bitmap_name(fr); break;
            case 0x04: f.data = read_bitmap_info(fr); break;
            case 0x05: {
                BitmapInfoRef b;
                b.info = fr.le<std::int32_t>();
                b.flags = fr.le<std::uint32_t>();
                f.data = b;
                break;
            }
            case 0x10: f.data = read_skeleton(fr, w); break;
            case 0x11: {
                SkeletonRef sr;
                sr.skeleton = fr.le<std::int32_t>();
                sr.flags = fr.le<std::uint32_t>();
                f.data = sr;
                break;
            }
            case 0x12: f.data = read_track_def(fr, w.backing_.bytes().empty() ? nullptr : &w.backing_, f.offset + 8); break;
            case 0x13: {
                TrackRef t;
                t.track = fr.le<std::int32_t>();
                t.flags = fr.le<std::uint32_t>();
                if (t.flags & 1) t.frame_ms = fr.le<std::uint32_t>();
                f.data = t;
                break;
            }
            case 0x14: f.data = read_actor_def(fr, w); break;
            case 0x15: f.data = read_actor_instance(fr); break;
            case 0x1b: f.data = read_light_def(fr); break;
            case 0x1c: {
                LightRef l;
                l.light = fr.le<std::int32_t>();
                l.flags = fr.le<std::uint32_t>();
                f.data = l;
                break;
            }
            case 0x21: f.data = read_bsp_tree(fr); break;
            case 0x22: {
                BspRegion g;
                g.flags = fr.le<std::uint32_t>();
                f.data = g;
                break;
            }
            case 0x26: {
                ParticleSprite s;
                s.flags = fr.le<std::uint32_t>();
                s.sprite = fr.le<std::int32_t>();
                s.material = fr.le<std::uint32_t>();
                f.data = s;
                break;
            }
            case 0x29: f.data = read_region_tag(fr, f.name); break;
            case 0x34: f.data = read_particle_cloud(fr); break;
            case 0x28: {
                PointLight p;
                p.light = fr.le<std::int32_t>();
                p.flags = fr.le<std::uint32_t>();
                p.position = read_vec3(fr);
                p.radius = fr.f32();
                f.data = p;
                break;
            }
            case 0x2d: {
                MeshRef m;
                m.mesh = fr.le<std::int32_t>();
                m.flags = fr.le<std::uint32_t>();
                f.data = m;
                break;
            }
            case 0x30: f.data = read_material(fr); break;
            case 0x31: f.data = read_material_list(fr); break;
            case 0x32: f.data = read_vertex_colors(fr); break;
            case 0x33: {
                VertexColorsRef v;
                v.colors = fr.le<std::int32_t>();
                v.flags = fr.le<std::uint32_t>();
                f.data = v;
                break;
            }
            case 0x35: {
                GlobalAmbient g;
                g.color = fr.le<std::uint32_t>();
                f.data = g;
                break;
            }
            case 0x36: f.data = read_mesh(fr, w.new_format_); break;
            default:
                if (w.unknown_[f.type]++ == 0)
                    log::debug("{}: skipping WLD fragment type {:#04x}", w.label_, f.type);
                break;
            }
        } catch (const std::out_of_range& e) {
            f.data = std::monostate{};
            w.errors_.push_back({i, f.type, e.what()});
        }
        w.fragments_.push_back(std::move(f));
    }
    const auto named_count = static_cast<std::size_t>(std::count_if(w.fragments_.begin(), w.fragments_.end(),
        [](const Fragment& fragment) { return !fragment.name.empty(); }));
    w.by_name_.reserve(named_count);
    for (std::size_t i = 0; i < w.fragments_.size(); ++i)
        if (!w.fragments_[i].name.empty()) w.by_name_.push_back(static_cast<std::uint32_t>(i));
    std::sort(w.by_name_.begin(), w.by_name_.end(), [&w](std::uint32_t a, std::uint32_t b) {
        const auto& left = w.fragments_[a].name;
        const auto& right = w.fragments_[b].name;
        return left < right || (left == right && a < b);
    });
    w.by_name_.erase(std::unique(w.by_name_.begin(), w.by_name_.end(), [&w](std::uint32_t a, std::uint32_t b) {
        return w.fragments_[a].name == w.fragments_[b].name;
    }), w.by_name_.end());
    for (const auto& e : w.errors_)
        log::debug("{}: fragment {} (type {:#04x}) truncated: {}", w.label_, e.index + 1, e.type, e.what);
    return w;
}

// The compact index never stores pointers into the temporary validation parse. Copies share the immutable
// source and payload state. Scoped readers keep pointers alive through nested and concurrent builds.
struct RetainedNode;
struct WldFile::Deferred {
    struct Entry {
        Ref ref;
        std::uint32_t type;
        std::string name;
        TrackRef track{};
    };
    std::vector<Entry> entries;
    std::function<ImmutableFileBytes()> loader;
    mutable std::mutex mutex;
    std::unique_ptr<WldFile> parsed;
    std::size_t readers = 0, parsed_bytes = 0;
    bool unscoped = false;
    DecodeStats stats;
    // Bumped (under `mutex`) by every lease and every move into the retained set: an eviction that finds it changed
    // since it picked this WLD leaves it alone.
    std::uint64_t lease_gen = 0;
    // The retained set's own (under its mutex): whether this WLD is in it, and where.
    bool retained = false;
    std::list<RetainedNode>::iterator retained_at;
    ~Deferred();
};

// One retained payload, most recently released at the front.
struct RetainedNode {
    std::weak_ptr<WldFile::Deferred> state;
    WldFile::Deferred* raw = nullptr; // for the flag; alive while `retained` is set (~Deferred takes it out first)
    std::uint64_t gen = 0;
    std::size_t bytes = 0;
};

namespace {
// Lock order: a WLD's mutex, then this one; never this one and then a WLD's (trim collects first, then locks each).
struct RetainedSet {
    std::mutex mutex;
    std::list<RetainedNode> nodes;
    std::size_t total = 0;
    std::size_t budget = WldFile::kDefaultRetainBudget;
};
RetainedSet& retained_set() {
    static auto* set = new RetainedSet; // never destroyed: WLDs in static storage may outlive any static set
    return *set;
}
} // namespace

WldFile::Deferred::~Deferred() {
    auto& set = retained_set();
    std::lock_guard lock(set.mutex);
    if (retained) {
        set.total -= retained_at->bytes;
        set.nodes.erase(retained_at);
        retained = false;
    }
}

namespace {
// Takes `state` out of the retained set (a new lease). Called with state.mutex held.
void unretain(WldFile::Deferred& state) {
    auto& set = retained_set();
    std::lock_guard lock(set.mutex);
    if (!state.retained) return;
    set.total -= state.retained_at->bytes;
    set.nodes.erase(state.retained_at);
    state.retained = false;
}

// Releases the least recently released payloads until the set is within its budget.
void trim_retained() {
    auto& set = retained_set();
    std::vector<std::pair<std::shared_ptr<WldFile::Deferred>, std::uint64_t>> victims;
    {
        std::lock_guard lock(set.mutex);
        while (set.total > set.budget && !set.nodes.empty()) {
            RetainedNode& n = set.nodes.back();
            n.raw->retained = false;
            set.total -= n.bytes;
            if (auto s = n.state.lock()) victims.emplace_back(std::move(s), n.gen);
            set.nodes.pop_back();
        }
    }
    for (auto& [state, gen] : victims) {
        std::unique_ptr<WldFile> drop; // freed outside the lock
        std::lock_guard lock(state->mutex);
        if (state->readers != 0 || !state->parsed || state->lease_gen != gen) continue;
        state->stats.released_bytes += state->parsed_bytes;
        ++state->stats.releases;
        drop = std::move(state->parsed);
        state->parsed_bytes = 0;
    }
}
} // namespace

void WldFile::set_retain_budget(std::size_t bytes) {
    {
        auto& set = retained_set();
        std::lock_guard lock(set.mutex);
        set.budget = bytes;
    }
    trim_retained();
}

std::size_t WldFile::retain_budget() {
    auto& set = retained_set();
    std::lock_guard lock(set.mutex);
    return set.budget;
}

std::size_t WldFile::retained_total() {
    auto& set = retained_set();
    std::lock_guard lock(set.mutex);
    return set.total;
}

void WldFile::release_retained() {
    std::size_t budget = 0;
    {
        auto& set = retained_set();
        std::lock_guard lock(set.mutex);
        budget = set.budget;
        set.budget = 0;
    }
    trim_retained();
    auto& set = retained_set();
    std::lock_guard lock(set.mutex);
    set.budget = budget;
}

thread_local WldFile::ReadScope* WldFile::ReadScope::current_ = nullptr;

WldFile::ReadScope::ReadScope() : parent_(current_) { current_ = this; }

WldFile::ReadScope::~ReadScope() {
    current_ = parent_;
    // Nested scopes put their leases in the root, so a nested return cannot invalidate its caller's pointers.
    bool added = false;
    for (const auto& state : held_) {
        std::unique_ptr<WldFile> drop; // freed outside the lock
        std::lock_guard lock(state->mutex);
        --state->readers;
        if (state->readers != 0 || state->unscoped || !state->parsed) continue;
        ++state->lease_gen;
        {
            auto& set = retained_set();
            std::lock_guard set_lock(set.mutex);
            if (state->parsed_bytes <= set.budget) {
                set.nodes.push_front({state, state.get(), state->lease_gen, state->parsed_bytes});
                state->retained_at = set.nodes.begin();
                state->retained = true;
                set.total += state->parsed_bytes;
                added = true;
                continue;
            }
        }
        state->stats.released_bytes += state->parsed_bytes;
        ++state->stats.releases;
        drop = std::move(state->parsed);
        state->parsed_bytes = 0;
    }
    if (added) trim_retained();
}

WldFile::DecodeStats WldFile::decode_stats() const {
    if (!deferred_) return {};
    std::lock_guard lock(deferred_->mutex);
    return deferred_->stats;
}

WldFile WldFile::deferred(std::function<std::vector<std::byte>()> loader, std::string label) {
    return deferred([loader = std::move(loader)] { return ImmutableFileBytes::owned(loader()); }, std::move(label));
}

WldFile WldFile::deferred(std::function<ImmutableFileBytes()> loader, std::string label) {
    auto parsed = parse(loader(), label);
    WldFile out;
    out.label_ = std::move(label);
    out.new_format_ = parsed.new_format_;
    out.errors_ = std::move(parsed.errors_);
    out.unknown_ = std::move(parsed.unknown_);
    out.deferred_ = std::make_shared<Deferred>();
    out.deferred_->loader = std::move(loader);
    parsed.visit_character_index([&](Ref ref, std::uint32_t type, const std::string& name, const TrackRef* track) {
        out.deferred_->entries.push_back({ref, type, name, track ? *track : TrackRef{}});
    });
    return out;
}

void WldFile::visit_character_index(const IndexVisitor& visitor) const {
    if (deferred_) {
        for (const auto& e : deferred_->entries)
            visitor(e.ref, e.type, e.name, e.type == 0x13 ? &e.track : nullptr);
        return;
    }
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        const auto& f = fragments_[i];
        if (f.name.empty()) continue;
        if (f.holds<ActorDef>() || f.holds<MeshDef>() || f.holds<MaterialDef>() || f.holds<TrackRef>())
            visitor(static_cast<Ref>(i + 1), f.type, f.name, std::get_if<TrackRef>(&f.data));
    }
}

bool WldFile::materialized() const {
    if (!deferred_) return true;
    std::lock_guard lock(deferred_->mutex);
    return deferred_->parsed != nullptr;
}

const WldFile& WldFile::decoded() const {
    if (!deferred_) return *this;
    std::lock_guard lock(deferred_->mutex);
    if (auto* scope = ReadScope::current_) {
        while (scope->parent_) scope = scope->parent_;
        if (std::find(scope->held_.begin(), scope->held_.end(), deferred_) == scope->held_.end()) {
            scope->held_.push_back(deferred_); // register before incrementing: allocation failure adds no reader
            ++deferred_->readers;
            ++deferred_->lease_gen;
            unretain(*deferred_);
        }
    } else if (!deferred_->unscoped) {
        deferred_->unscoped = true;
        ++deferred_->lease_gen;
        unretain(*deferred_);
    }
    if (!deferred_->parsed) {
        auto parsed = std::make_unique<WldFile>(parse(deferred_->loader(), label_));
        deferred_->parsed_bytes = parsed->retained_bytes();
        // A reclaimed file can be decoded often; cumulative counters at zone settle report the churn.
        log::debug("memory WLD materialized: {}: parsed estimate {:.2f} MiB", label_,
                   double(deferred_->parsed_bytes) / (1024.0 * 1024.0));
        deferred_->parsed = std::move(parsed);
        ++deferred_->stats.decodes;
    }
    return *deferred_->parsed;
}

const Fragment* WldFile::at(Ref ref) const {
    if (deferred_) return decoded().at(ref);
    if (ref <= 0 || static_cast<std::size_t>(ref) > fragments_.size()) return nullptr;
    return &fragments_[static_cast<std::size_t>(ref) - 1];
}

const Fragment* WldFile::resolve(Ref ref) const {
    if (ref > 0) return at(ref);
    if (ref == 0) return nullptr;
    auto name = string_at(ref);
    return name.empty() ? nullptr : find(name);
}

const Fragment* WldFile::find(std::string_view name) const {
    if (deferred_) return decoded().find(name);
    const auto it = std::lower_bound(by_name_.begin(), by_name_.end(), name,
        [this](std::uint32_t index, std::string_view key) { return fragments_[index].name < key; });
    return it == by_name_.end() || fragments_[*it].name != name ? nullptr : &fragments_[*it];
}

std::size_t WldFile::retained_bytes() const {
    if (deferred_) {
        std::lock_guard lock(deferred_->mutex);
        std::size_t bytes = deferred_->entries.size() * sizeof(Deferred::Entry);
        for (const auto& e : deferred_->entries) bytes += e.name.size();
        if (deferred_->parsed) bytes += deferred_->parsed->retained_bytes();
        return bytes;
    }
    std::size_t total = strings_.size() + fragments_.size() * sizeof(Fragment);
    auto vec = [](auto& v) { return v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type); };
    auto str = [](const std::string& s) { return s.size(); };
    for (const auto& f : fragments_) {
        total += str(f.name);
        std::visit(
            [&](const auto& d) {
                using D = std::decay_t<decltype(d)>;
                if constexpr (std::is_same_v<D, BitmapName>) {
                    total += vec(d.files);
                    for (const auto& s : d.files) total += str(s);
                } else if constexpr (std::is_same_v<D, BitmapInfo>) {
                    total += vec(d.frames);
                } else if constexpr (std::is_same_v<D, MaterialList>) {
                    total += vec(d.materials);
                } else if constexpr (std::is_same_v<D, FragmentBox<MeshDef>>) {
                    if (const auto* mesh = d.get())
                        total += sizeof(MeshDef) + vec(mesh->positions) + vec(mesh->uvs) + vec(mesh->normals) +
                                 vec(mesh->colors) + vec(mesh->polygons) + vec(mesh->vertex_pieces) + vec(mesh->render_groups);
                } else if constexpr (std::is_same_v<D, FragmentBox<ParticleCloud>>) {
                    if (d.get()) total += sizeof(ParticleCloud);
                } else if constexpr (std::is_same_v<D, ActorDef>) {
                    total += str(d.callback) + vec(d.sprites);
                } else if constexpr (std::is_same_v<D, VertexColors>) {
                    total += vec(d.colors);
                } else if constexpr (std::is_same_v<D, LightDef>) {
                    total += vec(d.levels) + vec(d.colors);
                } else if constexpr (std::is_same_v<D, TrackDef>) {
                    if (!d.frames.encoded()) total += vec(d.frames);
                } else if constexpr (std::is_same_v<D, Skeleton>) {
                    total += vec(d.nodes) + vec(d.meshes);
                    for (const auto& n : d.nodes) total += str(n.name) + vec(n.children);
                } else if constexpr (std::is_same_v<D, BspTree>) {
                    total += vec(d.nodes);
                } else if constexpr (std::is_same_v<D, RegionTag>) {
                    total += vec(d.regions) + str(d.tag);
                }
            },
            f.data);
    }
    return total;
}

std::string WldFile::string_at(Ref ref) const {
    if (deferred_) return decoded().string_at(ref);
    if (ref >= 0) return {};
    // Negate in 64 bits so INT32_MIN cannot overflow.
    auto offset = static_cast<std::uint64_t>(-static_cast<std::int64_t>(ref));
    if (offset >= strings_.size()) return {};
    // A name ends at its NUL or after kMaxStringLength bytes: the longest in the RoF2 files is 40 (59 in an EQG), and
    // without a bound every reference into one long run without a NUL would copy all of it.
    std::size_t start = static_cast<std::size_t>(offset);
    std::size_t end = start;
    const std::size_t limit = start + std::min(strings_.size() - start, kMaxStringLength);
    while (end < limit && strings_[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(strings_.data() + start), end - start);
}

} // namespace hailstone::assets::wld
