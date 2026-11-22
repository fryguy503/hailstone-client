#include "game/appearance_data.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "game/race_models.h"

namespace hailstone::game {

namespace {

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    for (std::size_t p = 0; p < text.size();) {
        std::size_t q = text.find('\n', p);
        if (q == std::string_view::npos) q = text.size();
        std::string_view line = text.substr(p, q - p);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.push_back(line);
        p = q + 1;
    }
    return out;
}

std::vector<std::string_view> split(std::string_view line, char sep) {
    std::vector<std::string_view> out;
    for (std::size_t p = 0;;) {
        std::size_t q = line.find(sep, p);
        if (q == std::string_view::npos) {
            out.push_back(line.substr(p));
            return out;
        }
        out.push_back(line.substr(p, q - p));
        p = q + 1;
    }
}

std::int64_t to_int(std::string_view s) { return std::strtoll(std::string(s).c_str(), nullptr, 10); }
float to_float(std::string_view s) { return std::strtof(std::string(s).c_str(), nullptr); }

std::string code3(std::string_view code) { return std::string(code.substr(0, 3)); }

// A comma list read token by token; every value kept.
std::vector<std::int64_t> int_list(std::string_view s) {
    std::vector<std::int64_t> out;
    for (std::string_view t : split(s, ','))
        if (!t.empty()) out.push_back(to_int(t));
    return out;
}

} // namespace

ColorTable ColorTable::parse(std::string_view ini) {
    ColorTable t;
    std::map<std::string, std::string> keys;
    bool data = false;
    for (std::string_view line : lines(ini)) {
        if (line.starts_with('[')) {
            data = line.starts_with("[Data]");
            continue;
        }
        auto eq = line.find('=');
        if (!data || eq == std::string_view::npos) continue;
        keys[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
    }
    // Deliberate difference: ColorTable_LoadFromIni takes any positive Number and allocates that many entries. A
    // server's asset folder can hold these files, so past kMaxEntries the rest are dropped rather than filling
    // billions of default entries.
    std::int64_t number = keys.contains("Number") ? to_int(keys["Number"]) : 0;
    number = std::min<std::int64_t>(number, kMaxEntries);
    for (std::int64_t i = 1; i <= number; ++i) {
        auto it = keys.find("Value" + std::to_string(i));
        auto v = it == keys.end() ? 0xffffffu : static_cast<std::uint32_t>(to_int(it->second));
        t.values.push_back(v | 0xff000000u);
    }
    return t;
}

void NewArmorTables::parse_tags(std::string_view text) {
    const auto all = model_codes_with_flags(0x20, 0);
    const auto luclin = model_codes_with_flags(0x20, 0x08);
    const auto newer = model_codes_with_flags(0x28, 0);
    for (std::string_view line : lines(text)) {
        if (line.empty() || line.front() == '#') continue;
        auto f = split(line, '\t');
        auto field = [&](std::size_t i) { return i < f.size() ? f[i] : std::string_view{}; };
        const auto id = static_cast<std::uint32_t>(to_int(field(0)));
        std::string codes(field(1).substr(0, 3)); // TextFileReader_NextField(.., 4)
        NewArmorTag tag;
        tag.it_number = static_cast<std::int32_t>(to_int(field(2)));
        tag.it_pair = static_cast<std::int32_t>(to_int(field(3)));
        tag.ignore_adjustment = to_int(field(4)) != 0;
        tag.show_hair = to_int(field(5)) != 0;
        tag.hide_mesh = to_int(field(6)) != 0;
        if (codes.empty()) continue;
        std::vector<std::string_view> own{codes};
        const std::vector<std::string_view>* list = &own;
        bool males = false, females = false;
        if (codes.size() < 3) {
            if (codes[0] == '*') list = &all;
            else if (codes[0] == 'L') list = &luclin;
            else if (codes[0] == 'N') list = &newer;
            if (codes.size() > 1 && codes[1] == 'M') males = true;
            else if (codes.size() > 1 && codes[1] == 'F') females = true;
        }
        for (std::string_view code : *list) {
            const RaceModel* m = find_model_code(code);
            const int gender = m ? m->gender : -1;
            if ((males && gender != 0) || (females && gender != 1)) continue;
            tags_[{id, code3(code)}] = tag;
        }
    }
}

void NewArmorTables::parse_adjustments(std::string_view text) {
    for (std::string_view line : lines(text)) {
        auto f = split(line, ',');
        if (f.size() < 11) continue;
        NewArmorAdjustment a;
        a.part = static_cast<std::int32_t>(to_int(f[1]));
        a.variant = f.size() > 11 ? static_cast<std::int32_t>(to_int(f[11])) : 0;
        float s[3] = {to_float(f[2]), to_float(f[3]), to_float(f[4])};
        float t[3] = {to_float(f[5]), to_float(f[6]), to_float(f[7])};
        constexpr float kDegrees = 0.017453292f; // 0x9c7b2c
        // D3DXMatrixRotationYawPitchRoll(yaw = rx, pitch = ry, roll = rz):
        // row-vector M = Rz(roll) Rx(pitch) Ry(yaw).
        const float yaw = to_float(f[8]) * kDegrees, pitch = to_float(f[9]) * kDegrees, roll = to_float(f[10]) * kDegrees;
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        const float cr = std::cos(roll), sr = std::sin(roll);
        float m[3][3] = {
            {cr * cy + sr * sp * sy, sr * cp, sr * sp * cy - cr * sy},
            {cr * sp * sy - sr * cy, cr * cp, sr * sy + cr * sp * cy},
            {cp * sy, -sp, cp * cy},
        };
        // Row r of the D3D matrix (scaled by s[r]) is column r of the
        // column-major matrix for p' = M p; the translation is column 3.
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) a.matrix[static_cast<std::size_t>(r * 4 + c)] = m[r][c] * s[r];
        for (int c = 0; c < 3; ++c) a.matrix[static_cast<std::size_t>(12 + c)] = t[c];
        a.matrix[3] = a.matrix[7] = a.matrix[11] = 0.0f;
        a.matrix[15] = 1.0f;
        std::string code(f[0]);
        while (!code.empty() && code.back() == ' ') code.pop_back();
        adjustments_[{a.part, a.variant, code3(code)}] = a;
    }
}

std::optional<NewArmorTag> NewArmorTables::find_tag(std::uint32_t id, std::string_view code) const {
    auto it = tags_.find({id, code3(code)});
    if (it == tags_.end()) return std::nullopt;
    return it->second;
}

const NewArmorAdjustment* NewArmorTables::find_adjustment(std::int32_t part, std::int32_t variant,
                                                          std::string_view code) const {
    auto it = adjustments_.find({part, variant, code3(code)});
    return it == adjustments_.end() ? nullptr : &it->second;
}

void PlayerCustomization::parse(std::string_view text) {
    for (std::string_view line : lines(text)) {
        if (line.empty() || line.front() == '#') continue;
        auto f = split(line, '^');
        if (f.size() < 13) continue;
        CustomizationEntry e;
        e.name = std::string(f[2]);
        e.base_color = static_cast<std::uint32_t>(to_int(f[3])) | 0xff000000u;
        for (auto v : int_list(f[4])) e.classes.push_back(static_cast<std::int32_t>(v));
        for (auto v : int_list(f[5])) e.colors.push_back(static_cast<std::uint32_t>(v) | 0xff000000u);
        e.faces = static_cast<std::int32_t>(to_int(f[6]));
        e.hair_styles = static_cast<std::int32_t>(to_int(f[7]));
        e.eyes = static_cast<std::int32_t>(to_int(f[8]));
        e.beards = static_cast<std::int32_t>(to_int(f[9]));
        e.tattoos = static_cast<std::int32_t>(to_int(f[10]));
        e.facial_attachments = static_cast<std::int32_t>(to_int(f[11]));
        entries_[{static_cast<std::int32_t>(to_int(f[0])), static_cast<std::int32_t>(to_int(f[1])),
                  static_cast<std::int32_t>(to_int(f[12]))}] = std::move(e);
    }
}

void ActorTagTable::parse(std::string_view text) {
    for (std::string_view line : lines(text)) {
        auto f = split(line, '^');
        if (f.size() < 2) continue;
        std::uint32_t model = 0;
        std::int32_t kind = 0;
        auto [end1, ec1] = std::from_chars(f[0].data(), f[0].data() + f[0].size(), model);
        auto [end2, ec2] = std::from_chars(f[1].data(), f[1].data() + f[1].size(), kind);
        // A line whose first two fields are not whole numbers is skipped, the safe choice: how the original's integer
        // reader (@0x7f4130) takes such a field is not traced.
        if (ec1 != std::errc{} || end1 != f[0].data() + f[0].size() || ec2 != std::errc{} ||
            end2 != f[1].data() + f[1].size())
            continue;
        ++records_;
        // A model listed twice takes its last line, as in the original: its loader (@0x7f4130) does not look for an
        // earlier record while loading and puts each new one at its hash bucket's head (@0x7f43a7), found first.
        if (kind == 1)
            shields_.insert(model);
        else
            shields_.erase(model);
    }
}

const CustomizationEntry* PlayerCustomization::find(std::int32_t race, std::int32_t heritage, std::int32_t gender) const {
    auto it = entries_.find({race, heritage, gender});
    return it == entries_.end() ? nullptr : &it->second;
}

std::uint32_t PlayerCustomization::hair_color(std::int32_t race, std::int32_t heritage, std::int32_t gender,
                                              std::int32_t index) const {
    const CustomizationEntry* e = find(race, heritage, gender);
    if (!e || index < 0 || static_cast<std::size_t>(index) >= e->colors.size()) return 0;
    return e->colors[static_cast<std::size_t>(index)];
}

std::vector<std::int32_t> PlayerCustomization::heritages(std::int32_t race, std::int32_t class_id,
                                                        std::int32_t gender) const {
    std::vector<std::int32_t> out;
    for (const auto& [key, e] : entries_)
        if (std::get<0>(key) == race && std::get<2>(key) == gender &&
            std::find(e.classes.begin(), e.classes.end(), class_id) != e.classes.end())
            out.push_back(std::get<1>(key));
    return out;
}

std::uint32_t PlayerCustomization::base_color(std::int32_t race, std::int32_t heritage, std::int32_t gender) const {
    const CustomizationEntry* e = find(race, heritage, gender);
    return e ? e->base_color : 0;
}

} // namespace hailstone::game
