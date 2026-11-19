#pragma once

// Builders for small synthetic WLD files, shared by the WLD tests and the fuzz tests.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "assets/wld.h"
#include "core/byte_io.h"

namespace hailstone::test::wld_fixtures {

using namespace hailstone;
using namespace hailstone::assets;

// WLD fragment types the fixtures build (docs/formats/wld.md).
inline constexpr std::uint32_t kBitmapName = 0x03;
inline constexpr std::uint32_t kBitmapInfo = 0x04;
inline constexpr std::uint32_t kSimpleSprite = 0x05;
inline constexpr std::uint32_t kSkeletonDef = 0x10;
inline constexpr std::uint32_t kSkeletonRef = 0x11;
inline constexpr std::uint32_t kTrackRef = 0x13;
inline constexpr std::uint32_t kActorDef = 0x14;
inline constexpr std::uint32_t kMeshRef = 0x2d;
inline constexpr std::uint32_t kMaterialDef = 0x30;
inline constexpr std::uint32_t kMaterialPalette = 0x31;
inline constexpr std::uint32_t kMeshDef = 0x36;
// The fixtures' opaque material render method.
inline constexpr std::uint32_t kOpaqueRenderMethod = 0x80000001;


// Builds small WLD files: an encoded string table and framed fragments.
class WldBuilder {
public:
    WldBuilder() { strings_.push_back(0); } // offset 0 is never a name

    std::int32_t str(const std::string& s) {
        auto offset = static_cast<std::int32_t>(strings_.size());
        strings_.insert(strings_.end(), s.begin(), s.end());
        strings_.push_back(0);
        return -offset;
    }

    // Adds a fragment; returns its 1-based reference.
    std::int32_t add(std::uint32_t type, std::int32_t name, const ByteWriter& body) {
        ByteWriter w;
        w.le(name);
        w.bytes(body.data());
        frags_.push_back({type, w.data()});
        return static_cast<std::int32_t>(frags_.size());
    }
    std::int32_t add_raw(std::uint32_t type, std::vector<std::byte> payload) {
        frags_.push_back({type, std::move(payload)});
        return static_cast<std::int32_t>(frags_.size());
    }

    std::vector<std::byte> build(std::uint32_t version = wld::kVersionOld) const {
        ByteWriter w;
        w.le(wld::kMagic);
        w.le(version);
        w.le(static_cast<std::uint32_t>(frags_.size()));
        w.le(std::uint32_t{0});
        w.le(std::uint32_t{0});
        w.le(static_cast<std::uint32_t>(strings_.size()));
        w.le(std::uint32_t{0});
        for (std::size_t i = 0; i < strings_.size(); ++i)
            w.le(static_cast<std::uint8_t>(strings_[i] ^ wld::kStringKey[i % 8]));
        for (const auto& [type, payload] : frags_) {
            w.le(static_cast<std::uint32_t>(payload.size()));
            w.le(type);
            w.bytes(payload);
        }
        return w.take();
    }

private:
    std::vector<std::uint8_t> strings_;
    std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> frags_;
};

inline ByteWriter bitmap_name(const std::string& file) {
    ByteWriter w;
    w.le(std::uint32_t{0}); // one name
    std::string s = file + '\0';
    w.le(static_cast<std::uint16_t>(s.size()));
    for (std::size_t i = 0; i < s.size(); ++i)
        w.le(static_cast<std::uint8_t>(static_cast<std::uint8_t>(s[i]) ^ wld::kStringKey[i % 8]));
    return w;
}

inline ByteWriter refs(std::initializer_list<std::int32_t> values, std::uint32_t lead = 0) {
    ByteWriter w;
    w.le(lead);
    w.le(static_cast<std::uint32_t>(values.size()));
    for (auto v : values) w.le(v);
    return w;
}

inline ByteWriter material(std::uint32_t render_method, std::int32_t sprite) {
    ByteWriter w;
    w.le(std::uint32_t{2}); // flags: pair present
    w.le(render_method);
    w.le(std::uint32_t{0x004e4e4e});
    w.f32(0.0f);
    w.f32(0.75f);
    w.le(sprite);
    w.le(std::uint32_t{0}); // the optional pair
    w.f32(0.0f);
    return w;
}

// A mesh of 4 vertices and 3 triangles in runs {slot 2, slot 0, slot 2}.
inline ByteWriter mesh(std::int32_t material_list, bool new_format) {
    ByteWriter w;
    w.le(std::uint32_t{0x00018003});
    w.le(material_list);
    w.le(std::int32_t{0});
    w.le(std::int32_t{0});
    w.le(std::int32_t{0});
    for (float f : {100.0f, 200.0f, 300.0f}) w.f32(f); // centre
    w.zeros(12);
    w.f32(10.0f);
    w.zeros(24); // bounds
    for (int n : {4, 4, 4, 4, 3, 0, 3, 0, 0}) w.le(static_cast<std::uint16_t>(n));
    w.le(std::uint16_t{2}); // scale shift: positions / 4
    for (int v : {0, 0, 0, 4, 0, 0, 0, 8, 0, -4, 0, 12}) w.le(static_cast<std::int16_t>(v));
    for (int i = 0; i < 4; ++i) {
        if (new_format) {
            w.f32(0.5f * static_cast<float>(i));
            w.f32(-1.0f);
        } else {
            w.le(static_cast<std::int16_t>(128 * i));
            w.le(std::int16_t{-256});
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int n : {0, 0, 127}) w.le(static_cast<std::int8_t>(n));
    for (int i = 0; i < 4; ++i) w.le(std::uint32_t{0x80112233}); // B=33 G=22 R=11 A=80
    auto poly = [&](std::uint16_t flags, std::uint16_t a, std::uint16_t b, std::uint16_t c) {
        w.le(flags);
        w.le(a);
        w.le(b);
        w.le(c);
    };
    poly(0, 0, 1, 2);
    poly(0x10, 1, 2, 3);
    poly(0, 0, 2, 3);
    for (int g : {1, 2, 1, 0, 1, 2}) w.le(static_cast<std::uint16_t>(g));
    return w;
}

// Bitmap → animated sprite → material chain, plus a material list of three
// slots, the middle one empty, and a mesh using it.
inline std::vector<std::byte> mesh_wld(bool new_format) {
    WldBuilder b;
    auto bmp1 = b.add(0x03, b.str("A_SPRITE"), bitmap_name("water1.bmp"));
    auto bmp2 = b.add(0x03, b.str("B_SPRITE"), bitmap_name("water2.bmp"));
    ByteWriter info;
    info.le(std::uint32_t{0x18}); // animated, has delay
    info.le(std::uint32_t{2});
    info.le(std::uint32_t{150});
    info.le(bmp1);
    info.le(bmp2);
    auto info_ref = b.add(0x04, b.str("WATER_SPRITE"), info);
    ByteWriter sprite;
    sprite.le(info_ref);
    sprite.le(std::uint32_t{0x50});
    auto sprite_ref = b.add(0x05, 0, sprite);
    auto opaque = b.add(0x30, b.str("WATER_MDF"), material(0x80000001, sprite_ref));
    auto invisible = b.add(0x30, b.str("BOUND_MDF"), material(0, 0));
    auto list = b.add(0x31, b.str("ZONE_MP"), refs({opaque, 0, invisible}));
    b.add(0x36, b.str("R1_DMSPRITEDEF"), mesh(list, new_format));
    return b.build(new_format ? wld::kVersionNew : wld::kVersionOld);
}

} // namespace hailstone::test::wld_fixtures
