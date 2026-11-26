#include "assets/model_points.h"

#include <stdexcept>

#include "assets/character.h"
#include "core/byte_io.h"
#include "game/sine_table.h"

namespace hailstone::assets {

namespace {

constexpr std::size_t kPointRecordSize = 0xa4;

std::string cstring(ByteReader& r, std::size_t n) {
    std::string s = r.fixed_string(n);
    if (auto nul = s.find('\0'); nul != std::string::npos) s.resize(nul); // stale bytes follow the first NUL
    return s;
}

glm::vec3 vec3(ByteReader& r) {
    float x = r.f32(), y = r.f32();
    return {x, y, r.f32()};
}

} // namespace

std::vector<ModelPoint> parse_pts(std::span<const std::byte> data) {
    if (data.size() < 12) throw std::runtime_error("PTS file too short");
    ByteReader r(data);
    if (r.fixed_string(4) != "EQPT") throw std::runtime_error("not a PTS file");
    std::uint32_t count = r.le<std::uint32_t>();
    std::uint32_t version = r.le<std::uint32_t>();
    std::vector<ModelPoint> out;
    if (version != 1) return out;
    // Divided, not multiplied: count * size can wrap a 32-bit size_t.
    if ((data.size() - 12) / kPointRecordSize < count) throw std::runtime_error("PTS file truncated");
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ModelPoint p;
        p.name = cstring(r, 64);
        p.bone = cstring(r, 64);
        p.offset = vec3(r);
        p.rotation = vec3(r);
        p.scale = vec3(r);
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<ModelEmitter> parse_prt(std::span<const std::byte> data) {
    if (data.size() < 12) throw std::runtime_error("PRT file too short");
    ByteReader r(data);
    if (r.fixed_string(4) != "PTCL") throw std::runtime_error("not a PRT file");
    std::uint32_t count = r.le<std::uint32_t>();
    std::uint32_t version = r.le<std::uint32_t>();
    static constexpr std::size_t kSizes[] = {0, 0x5c, 0x60, 0x64, 0x68, 0x6c};
    std::vector<ModelEmitter> out;
    if (version < 1 || version > 5) return out;
    const std::size_t size = kSizes[version];
    if ((data.size() - 12) / size < count) throw std::runtime_error("PRT file truncated");
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ByteReader rec(data.subspan(12 + std::size_t{i} * size, size));
        ModelEmitter e;
        e.emitter = rec.le<std::uint32_t>();
        e.point = cstring(rec, 64);
        e.trigger = rec.le<std::int32_t>();
        e.animation = rec.le<std::int32_t>();
        e.unknown_4c = rec.le<std::int32_t>();
        e.unknown_50 = rec.le<std::int32_t>();
        e.delay_ms = rec.le<std::int32_t>();
        e.duration_ms = rec.le<std::int32_t>();
        // The loader's defaults for the fields the older versions lack.
        e.floor = version >= 2 ? rec.le<std::int32_t>() : 0;
        e.variant = version >= 3 ? rec.le<std::int32_t>() : -1;
        e.unknown_64 = version >= 4 ? rec.le<std::int32_t>() : 0;
        e.alternate = version >= 5 ? rec.le<std::uint32_t>() : e.emitter;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<ModelPoint> wld_model_points(std::span<const std::string> bones, const SpOffsetLookup& offsets) {
    std::vector<ModelPoint> out;
    if (bones.empty()) return out;
    const std::string code = bones[0].substr(0, 3);
    auto has = [&](const std::string& name) {
        for (const auto& b : bones)
            if (b == name) return true;
        return false;
    };
    auto add = [&](std::string_view name, const std::string& bone) {
        ModelPoint p;
        p.name = name;
        p.bone = bone;
        out.push_back(std::move(p));
        return &out.back();
    };
    // Bone 0 by its full name.
    add("SPELLPOINT_DEFAULT", bones[0]);
    if (has(code + "CHEST_POINT_TRACK")) add("SPELLPOINT_CHEST", code + "CHEST_POINT_TRACK");
    else if (has(code + "CH_TRACK")) add("SPELLPOINT_CHEST", code + "CH_TRACK");
    if (has(code + "HEHEAD_TRACK")) {
        add("SPELLPOINT_HEAD", code + "HEHEAD_TRACK");
        ModelPoint* m = add("SPELLPOINT_MOUTH", code + "HEHEAD_TRACK");
        m->offset = {0.08f, 0.6f, 0.0f};
        m->rotation = {128.0f, 0.0f, 128.0f};
    } else if (has(code + "HEAD_POINT_TRACK")) {
        add("SPELLPOINT_HEAD", code + "HEAD_POINT_TRACK");
        ModelPoint* m = add("SPELLPOINT_MOUTH", code + "HEAD_POINT_TRACK");
        auto get = [&](std::string_view key, float fallback) {
            if (!offsets) return fallback;
            return offsets(code, key).value_or(fallback);
        };
        m->offset = {get("XOffset", 0.6f), get("YOffset", 0.0f), get("ZOffset", 0.0f)};
        m->rotation = {get("HOffset", 0.0f), get("POffset", 0.0f), get("ROffset", 0.0f)};
        m->scale = {get("XScale", 1.0f), get("YScale", 1.0f), get("ZScale", 1.0f)};
    }
    for (auto [name, own] : {std::pair{"SPELLPOINT_LHAND", "L_POINT_TRACK"}, std::pair{"SPELLPOINT_RHAND", "R_POINT_TRACK"}}) {
        if (has(code + own)) add(name, code + own);
        else if (has(code + "SPELL_POINT_TRACK")) add(name, code + "SPELL_POINT_TRACK");
    }
    for (auto [name, foot, bo] : {std::tuple{"SPELLPOINT_LFOOT", "BOFOOTL_TRACK", "BO_L_TRACK"},
                                  std::tuple{"SPELLPOINT_RFOOT", "BOFOOTR_TRACK", "BO_R_TRACK"}}) {
        if (has(code + foot)) add(name, code + foot);
        else if (has(code + bo)) add(name, code + bo);
    }
    return out;
}

glm::mat4 point_matrix(const ModelPoint& p) {
    // The DLL's row-vector rows are this matrix's columns.
    float sa = game::cmath_sin(p.rotation.x), ca = game::cmath_cos(p.rotation.x);
    float sb = game::cmath_sin(-p.rotation.y), cb = game::cmath_cos(-p.rotation.y);
    float sc = game::cmath_sin(p.rotation.z), cc = game::cmath_cos(p.rotation.z);
    glm::mat4 m(1.0f);
    m[0] = {cb * ca, cb * sa, -sb, 0.0f};
    m[1] = {sc * sb * ca - cc * sa, sc * sb * sa + cc * ca, sc * cb, 0.0f};
    m[2] = {cc * sb * ca + sc * sa, cc * sb * sa - sc * ca, cc * cb, 0.0f};
    m[3] = {p.offset, 1.0f};
    glm::mat4 s(1.0f);
    s[0][0] = p.scale.x;
    s[1][1] = p.scale.y;
    s[2][2] = p.scale.z;
    return m * s;
}

const ModelPoint* find_point(std::span<const ModelPoint> points, std::string_view name) {
    for (const auto& p : points)
        if (p.name == name) return &p;
    return nullptr;
}

const SpellPoints::Model& SpellPoints::of(const CharacterModel& model) {
    if (auto it = models_.find(model.serial.value()); it != models_.end()) return it->second;
    if (models_.size() >= kMaxModels) models_.clear();
    auto it = models_.try_emplace(model.serial.value()).first;
    Model& out = it->second;
    out.bone.fill(-1);
    std::vector<ModelPoint> wld;
    std::span<const ModelPoint> points = model.points;
    if (!model.eqg) {
        wld = wld_model_points(model.bone_names, offsets_);
        points = wld;
    }
    for (std::size_t n = 0; n < kSpellPointNames.size(); ++n) {
        const ModelPoint* p = find_point(points, kSpellPointNames[n]);
        if (!p) continue;
        for (std::size_t b = 0; b < model.bone_names.size(); ++b)
            if (model.bone_names[b] == p->bone) {
                out.bone[n] = static_cast<int>(b);
                break;
            }
        if (out.bone[n] < 0) continue;
        out.local[n] = point_matrix(*p);
        out.mask = static_cast<std::uint16_t>(out.mask | (1u << n));
    }
    return out;
}

} // namespace hailstone::assets
