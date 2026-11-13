// Test vectors recorded from the original eqgame.exe: <Function>.jsonl, one
// JSON object per line, {"fn", "addr", "in": {...}, "out": {...},
// "precision", optional "control_word"}, in HAILSTONE_VECTORS_DIR. Missing
// captures are optional unless HAILSTONE_REQUIRE_SWEEP_VECTORS is present;
// that mode requires an existing directory and recognized triangle records.
//
// Every record of the movement functions is checked against the ports in
// game/mechanics/movement.h, at the record's x87 precision; floats must match
// bit for bit for supported nearest-even modes. The collision sweep
// (game/sphere_sweep.h) uses a stated tolerance; triangle records are compared
// at both the declared mantissa width and production float precision, without
// emulating the full recorded x87 environment. Mocked callees' results
// (in.calls) are inputs.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "game/mechanics/collision_movement.h"
#include "game/collision_callbacks.h"
#include "game/sphere_sweep.h"
#include "game/mechanics/controls.h"
#include "game/mechanics/switches.h"
#include "game/mechanics/floor_height.h"
#include "game/mechanics/can_i_fit_here.h"
#include "game/doors.h"
#include "game/mechanics/movement.h"
#include "game/mechanics/regions.h"
#include "game/mechanics/remote_spawns.h"
#include "vector_files.h"

using namespace hailstone::game::mechanics;
namespace fs = std::filesystem;

namespace {

// ---- a small JSON reader (objects, arrays, numbers, strings, booleans, null)

struct Json;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;
struct Json {
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<JsonObject>, std::shared_ptr<JsonArray>> v;
    const Json* get(const std::string& key) const {
        auto* o = std::get_if<std::shared_ptr<JsonObject>>(&v);
        if (!o) return nullptr;
        auto it = (*o)->find(key);
        return it == (*o)->end() ? nullptr : &it->second;
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    std::optional<Json> parse() {
        auto j = value();
        ws();
        if (!j || i_ != s_.size()) return std::nullopt;
        return j;
    }

private:
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_;
    }
    bool lit(const char* w) {
        std::size_t n = std::strlen(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    std::optional<std::string> str() {
        if (i_ >= s_.size() || s_[i_] != '"') return std::nullopt;
        std::string out;
        for (++i_; i_ < s_.size(); ++i_) {
            char c = s_[i_];
            if (c == '"') {
                ++i_;
                return out;
            }
            if (c == '\\' && i_ + 1 < s_.size()) {
                char e = s_[++i_];
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                out += c;
            }
        }
        return std::nullopt;
    }
    std::optional<Json> value() {
        ws();
        if (i_ >= s_.size()) return std::nullopt;
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            auto obj = std::make_shared<JsonObject>();
            ws();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return Json{obj};
            }
            for (;;) {
                ws();
                auto k = str();
                ws();
                if (!k || i_ >= s_.size() || s_[i_] != ':') return std::nullopt;
                ++i_;
                auto v = value();
                if (!v) return std::nullopt;
                (*obj)[*k] = *v;
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    return Json{obj};
                }
                return std::nullopt;
            }
        }
        if (c == '[') {
            ++i_;
            auto arr = std::make_shared<JsonArray>();
            ws();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return Json{arr};
            }
            for (;;) {
                auto v = value();
                if (!v) return std::nullopt;
                arr->push_back(*v);
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    return Json{arr};
                }
                return std::nullopt;
            }
        }
        if (c == '"') {
            auto t = str();
            if (!t) return std::nullopt;
            return Json{*t};
        }
        if (lit("true")) return Json{true};
        if (lit("false")) return Json{false};
        if (lit("null")) return Json{nullptr};
        char* end = nullptr;
        double d = std::strtod(s_.c_str() + i_, &end);
        if (end == s_.c_str() + i_) return std::nullopt;
        i_ = static_cast<std::size_t>(end - s_.c_str());
        return Json{d};
    }
    const std::string& s_;
    std::size_t i_ = 0;
};

// ---- typed field access

bool is_null(const Json* j) { return !j || std::holds_alternative<std::nullptr_t>(j->v); }

// A float: a %.9g number, "inf", "-inf", "nan" (the default quiet NaN,
// 0xffc00000) or "nan:0x..." (bits).
std::optional<float> as_f32(const Json* j) {
    if (!j) return std::nullopt;
    if (auto* d = std::get_if<double>(&j->v)) return static_cast<float>(*d);
    if (auto* s = std::get_if<std::string>(&j->v)) {
        if (*s == "inf") return std::numeric_limits<float>::infinity();
        if (*s == "-inf") return -std::numeric_limits<float>::infinity();
        std::uint32_t bits = 0x7fc00000u; // "nan": the quiet NaN C's NAN gives
        if (s->rfind("nan:0x", 0) == 0) bits = static_cast<std::uint32_t>(std::strtoul(s->c_str() + 6, nullptr, 16));
        else if (*s != "nan") return std::nullopt;
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    return std::nullopt;
}

// A double: a %.17g number, "inf", "-inf", "nan" or "nan:0x..." (64-bit bits).
std::optional<double> as_f64(const Json* j) {
    if (!j) return std::nullopt;
    if (auto* d = std::get_if<double>(&j->v)) return *d;
    if (auto* s = std::get_if<std::string>(&j->v)) {
        if (*s == "inf") return std::numeric_limits<double>::infinity();
        if (*s == "-inf") return -std::numeric_limits<double>::infinity();
        if (*s == "nan") return static_cast<double>(*as_f32(j)); // the float default, widened
        if (s->rfind("nan:0x", 0) != 0) return std::nullopt;
        std::uint64_t bits = std::strtoull(s->c_str() + 6, nullptr, 16);
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    return std::nullopt;
}

std::optional<std::int64_t> as_int(const Json* j) {
    if (!j) return std::nullopt;
    if (auto* d = std::get_if<double>(&j->v)) return static_cast<std::int64_t>(*d);
    if (auto* b = std::get_if<bool>(&j->v)) return *b ? 1 : 0;
    return std::nullopt;
}

std::int32_t i32(const Json& o, const char* name) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(as_int(o.get(name)).value_or(0)));
}
std::uint32_t u32(const Json& o, const char* name) { return static_cast<std::uint32_t>(as_int(o.get(name)).value_or(0)); }
bool flag(const Json& o, const char* name) { return as_int(o.get(name)).value_or(0) != 0; }
float f32(const Json& o, const char* name) { return as_f32(o.get(name)).value_or(0.0f); }

// The result a mocked callee returned (the first call named `fn`), if it was called.
const Json* call(const Json& in, const char* fn) {
    const Json* calls = in.get("calls");
    auto* arr = calls ? std::get_if<std::shared_ptr<JsonArray>>(&calls->v) : nullptr;
    if (!arr) return nullptr;
    for (const Json& c : **arr) {
        const Json* name = c.get("fn");
        auto* n = name ? std::get_if<std::string>(&name->v) : nullptr;
        if (n && *n == fn) return &c;
    }
    return nullptr;
}
std::int32_t call_ret(const Json& in, const char* fn) {
    const Json* c = call(in, fn);
    return c ? i32(*c, "ret") : 0;
}

// A float loaded onto the FPU (fld quiets a signalling NaN).
X87 st_value(float f) {
    if (is_signalling(f)) {
        std::uint32_t b;
        std::memcpy(&b, &f, 4);
        b |= 0x00400000u;
        std::memcpy(&f, &b, 4);
    }
    return X87(static_cast<double>(f));
}

std::uint32_t bits_of(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

constexpr const char* kTriangleFunction = "CCollisionInfoSphere::vf0";

enum class ReplayRounding : unsigned { Nearest, Downward, Upward, TowardZero };
constexpr std::array<const char*, 4> kReplayRoundingNames{"nearest", "downward", "upward", "toward_zero"};

struct ReplayPrecision {
    FpuPrecision precision = FpuPrecision::Single24;
    std::optional<std::uint16_t> control_word;
    ReplayRounding rounding = ReplayRounding::Nearest;
};

// Old vectors may omit both fields. Explicit metadata is never guessed: PC=01 is reserved and PC=11 (64 bits)
// is unsupported by these ports. RC is retained as provenance; selecting float/double does not emulate x87 RC.
std::optional<ReplayPrecision> replay_precision(const Json& rec, std::string& error) {
    error.clear();
    ReplayPrecision result;
    const Json* p = rec.get("precision");
    if (p) {
        const auto* label = std::get_if<std::string>(&p->v);
        if (!label || (*label != "x87-24" && *label != "x87-53")) {
            error = "precision must be x87-24 or x87-53";
            return std::nullopt;
        }
        result.precision = *label == "x87-53" ? FpuPrecision::Double53 : FpuPrecision::Single24;
    }
    if (const Json* c = rec.get("control_word")) {
        const auto* number = std::get_if<double>(&c->v);
        if (!number || !std::isfinite(*number) || *number < 0 || *number > 65535 || std::trunc(*number) != *number) {
            error = "control_word must be a finite integer in [0, 65535]";
            return std::nullopt;
        }
        if (!p) {
            error = "control_word requires an explicit matching precision";
            return std::nullopt;
        }
        const auto word = static_cast<std::uint16_t>(*number);
        const unsigned pc = (word >> 8) & 3;
        if (pc != 0 && pc != 2) {
            error = "control_word precision control is reserved or unsupported (only 24/53 bits supported)";
            return std::nullopt;
        }
        const FpuPrecision precision = pc == 2 ? FpuPrecision::Double53 : FpuPrecision::Single24;
        if (precision != result.precision) {
            error = "precision disagrees with control_word precision control";
            return std::nullopt;
        }
        result.control_word = word;
        result.rounding = static_cast<ReplayRounding>((word >> 10) & 3);
    }
    return result;
}

std::optional<ReplayPrecision> replay_precision_for(const Json& rec, const std::string& function,
                                                   std::string& error) {
    auto result = replay_precision(rec, error);
    if (result && result->control_word && result->rounding != ReplayRounding::Nearest && function != kTriangleFunction) {
        error = "non-nearest control_word rounding is not emulated by this replay checker";
        return std::nullopt;
    }
    return result;
}

Fpu precision_of(const Json& rec) {
    std::string error;
    const auto precision = replay_precision(rec, error);
    CHECK_MESSAGE(precision.has_value(), error);
    return Fpu(precision ? precision->precision : FpuPrecision::Single24);
}

struct Mismatch {
    int count = 0;
    void check_float(float got, std::optional<float> want, const std::string& where, const char* what) {
        if (!want) {
            report(where, what, "missing");
            return;
        }
        if (bits_of(got) != bits_of(*want)) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "got %.9g (0x%08x), want %.9g (0x%08x)", static_cast<double>(got),
                          bits_of(got), static_cast<double>(*want), bits_of(*want));
            report(where, what, buf);
        }
    }
    void check_double(double got, std::optional<double> want, const std::string& where, const char* what) {
        std::uint64_t g, w = 0;
        std::memcpy(&g, &got, 8);
        if (want) std::memcpy(&w, &*want, 8);
        if (!want || g != w) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "got %.17g (0x%016llx), want %.17g (0x%016llx)", got,
                          static_cast<unsigned long long>(g), want ? *want : 0.0, static_cast<unsigned long long>(w));
            report(where, what, buf);
        }
    }
    void check_int(std::int64_t got, std::optional<std::int64_t> want, const std::string& where, const char* what) {
        if (!want || got != *want)
            report(where, what, "got " + std::to_string(got) + ", want " + (want ? std::to_string(*want) : "missing"));
    }
    void report(const std::string& where, const char* what, const std::string& text) {
        if (++count <= 20) FAIL_CHECK(where << ": " << std::string(what) << ": " << text);
    }
};

using Checker = std::function<void(const Json& rec, const Json& in, const Json& out, const std::string& where,
                                   Mismatch& m)>;

SpawnSpeeds speeds_of(const Json& j) {
    SpawnSpeeds s;
    s.run_speed = f32(j, "run_speed");
    s.speed_multiplier = f32(j, "speed_multiplier");
    s.speed_run = f32(j, "speed_run");
    s.speed_y = f32(j, "speed_y");
    s.speed_x = f32(j, "speed_x");
    s.speed_heading = f32(j, "speed_heading");
    return s;
}

void check_speeds(const SpawnSpeeds& got, const Json* want, const std::string& where, const char* who, Mismatch& m) {
    if (is_null(want)) {
        m.report(where, who, "expected no spawn");
        return;
    }
    std::string w = where + " " + who;
    m.check_float(got.run_speed, as_f32(want->get("run_speed")), w, "run_speed");
    m.check_float(got.speed_multiplier, as_f32(want->get("speed_multiplier")), w, "speed_multiplier");
    m.check_float(got.speed_run, as_f32(want->get("speed_run")), w, "speed_run");
    m.check_float(got.speed_y, as_f32(want->get("speed_y")), w, "speed_y");
    m.check_float(got.speed_x, as_f32(want->get("speed_x")), w, "speed_x");
    m.check_float(got.speed_heading, as_f32(want->get("speed_heading")), w, "speed_heading");
}

// The recorded answers of GetEnvCoefficients' callees (in.answers).
class RecordedEnv final : public EnvQueries {
public:
    explicit RecordedEnv(const Json& a) : a_(a) {}
    bool spawn_vf90() override { return flag(a_, "spawn_levitating_race"); }
    bool possibly_stuck() override { return flag(a_, "possibly_stuck"); }
    bool collision_zone_loaded() override {
        static const char* names[] = {"collision_zone_loaded_1", "collision_zone_loaded_2", "collision_zone_loaded_3"};
        return flag(a_, names[loaded_ < 2 ? loaded_++ : 2]);
    }
    float gravity() override { return f32(a_, "world_gravity"); }
    bool mount_race() override { return flag(a_, "is_mount_race"); }
    bool corpse() override { return flag(a_, "is_corpse"); }
    bool climbing() override { return flag(a_, "physics_climbing"); }
    bool swim_zone() override { return flag(a_, "world_swim_flag"); }
    bool feet_on_ground() override { return flag(a_, "feet_on_ground"); }
    bool spawn_vf58() override { return flag(a_, "spawn_race_check_58"); }
    bool controlled() override { return flag(a_, "spawn_is_controlled"); }
    bool spawn_vf50() override { return flag(a_, "spawn_has_race_info"); }
    float floor_at(float, float, float) override { return f32(a_, "world_floor_height"); }
    bool rising() override { return flag(a_, "rising"); }
    float buoyancy() override { return f32(a_, "physics_buoyancy"); }

private:
    const Json& a_;
    int loaded_ = 0;
};

// The recorded answers of LandingAndFallCheck's callees.
class RecordedLanding final : public LandingQueries {
public:
    explicit RecordedLanding(const Json& a) : a_(a) {}
    bool skip_physics() override { return flag(a_, "spawn_skip_physics"); }
    bool spawn_vf58() override { return flag(a_, "spawn_race_check_58"); }
    bool player_spawn() override { return flag(a_, "is_player_spawn"); }
    std::int32_t sound_race() override { return i32(a_, "spawn_race"); }
    bool rider_zone_client() override { return flag(a_, "rider_zone_client"); }
    bool no_recovery() override { return flag(a_, "spawn_no_recovery"); }
    float floor_below(float, float, float) override {
        return f32(a_, floors_++ == 0 ? "floor_height_1" : "floor_height_2");
    }

private:
    const Json& a_;
    int floors_ = 0;
};

bool has_virtual_call(const Json& out, const char* fn) {
    const Json* calls = out.get("virtual_calls");
    auto* arr = calls ? std::get_if<std::shared_ptr<JsonArray>>(&calls->v) : nullptr;
    if (!arr) return false;
    for (const Json& c : **arr) {
        const Json* name = c.get("fn");
        auto* n = name ? std::get_if<std::string>(&name->v) : nullptr;
        if (n && *n == fn) return true;
    }
    return false;
}

// ProcessControls' callees, replayed strictly in the recorded order: each
// question takes the next recorded call (which must be the same callee) and
// answers with its result; each action must match the next recorded call.
// "this"/"ret" name the objects as the record first saw them, so after the
// hand-back from a boat the controlled player is the "local" object.
class RecordedControls final : public ControlsQueries {
public:
    RecordedControls(const Json& calls, bool initially_local, bool local_null, const Fpu& fpu, const std::string& where,
                     Mismatch& m)
        : initially_local_(initially_local), local_null_(local_null), fpu_(fpu), where_(where), m_(m) {
        if (auto* arr = std::get_if<std::shared_ptr<JsonArray>>(&calls.v))
            for (const Json& c : **arr) {
                const std::string& fn = name(c, "fn");
                if (fn != "log_void_connection" && fn != "close_connection") calls_.push_back(&c);
            }
    }
    void finish() {
        if (!broken_ && next_ != calls_.size()) fail("calls left over: next " + name(*calls_[next_], "fn"));
    }

    bool player_vf58(ControlWho w) override { return flag_ret(take("player_vf58", w)); }
    std::int32_t physics_mode(ControlWho w) override { return int_ret(take("player_vf78_state", w)); }
    float calculate_weight() override {
        const Json* c = take("calculate_weight");
        return c ? as_f32(c->get("ret")).value_or(1.0f) : 1.0f;
    }
    bool is_mount_race(ControlWho) override { return flag_ret(take("is_mount_race")); }
    ControlRef get_mount(ControlWho w) override { return ref(take("get_mount", w)); }
    ControlRef get_rider(ControlWho w) override { return ref(take("get_rider", w)); }
    bool feet_on_ground() override { return flag_ret(take("feet_on_ground")); }
    std::int32_t endurance() override { return int_ret(take("endurance")); }
    std::int32_t max_endurance() override { return int_ret(take("max_endurance")); }
    std::int32_t swimming_skill() override { return int_ret(take("get_adjusted_skill")); }
    std::int32_t speed_cap_effect() override { return int_ret(take("total_effect")); }
    bool loot_window_active() override { return flag_ret(take("loot_window_active")); }
    ControlRef controlled_spawn_of() override {
        ControlRef r = ref(take("controlled_spawn_of"));
        if (r != ControlRef::Controlled) switched_ = true;
        return r;
    }
    bool camera_active() override { return flag_ret(take("camera_active")); }
    double camera_heading() override {
        const Json* c = take("camera_get_heading");
        const Json* v = c ? c->get("ret") : nullptr;
        auto* d = v ? std::get_if<double>(&v->v) : nullptr;
        // The test's camera returns heading * scale on the FPU: the record has the exact product, which
        // the x87 rounds at the record's precision.
        return d ? fpu_.add(*d, 0.0) : 0.0;
    }
    void follow(ControlWho w) override { take("follow_player_ai", w); }
    void do_attack(std::int32_t slot) override { expect_int(take("do_attack"), "slot", slot); }
    void turn_off_auto_follow() override { take("turn_off_auto_follow"); }
    void set_accel(ControlWho w, float target) override { expect_float(take("set_accel", w), "target", target); }
    void print(std::uint32_t id, std::int32_t color) override {
        expect_int(take("get_string"), "id", static_cast<std::int32_t>(id));
        expect_int(take("chat"), "color", color);
    }
    void do_swim_jump() override { take("do_swim_jump"); }
    void proc_mouse(bool can_move) override { expect_int(take("proc_mouse"), "can_move", can_move); }
    void add_pitch(ControlWho w, float delta) override {
        const Json* c = take("add_pitch");
        if (c && name(*c, "physics") != who(w) + "_physics") fail("add_pitch physics " + name(*c, "physics"));
        expect_float(c, "delta", delta);
    }
    void close_loot_window() override { take("loot_window_vfd8"); }
    void sit() override { take("sit"); }
    void set_camera_heading(float v) override { expect_float(take("camera_set_heading"), "value", v); }
    void set_camera_pan(float v) override { expect_float(take("camera_set_pan"), "value", v); }
    void set_camera_distance(float v) override { expect_float(take("camera_set_distance"), "value", v); }
    void set_camera_pitch(float v) override { expect_float(take("camera_set_pitch"), "value", v); }
    void send_control_boat(std::uint32_t id) override {
        expect_int(take("send_control_boat"), "boat_id", static_cast<std::int32_t>(id));
    }

private:
    static std::string name(const Json& c, const char* key) {
        const Json* j = c.get(key);
        auto* s = j ? std::get_if<std::string>(&j->v) : nullptr;
        return s ? *s : std::string();
    }
    std::string who(ControlWho w) const {
        if (w == ControlWho::Controlled) return switched_ ? "local" : "controlled";
        if (local_null_) return "null";
        return initially_local_ ? "controlled" : "local";
    }
    ControlRef ref(const Json* c) const {
        if (!c) return ControlRef::None;
        std::string r = name(*c, "ret");
        if (r == "null" || r.empty()) return ControlRef::None;
        if (r == who(ControlWho::Controlled)) return ControlRef::Controlled;
        if (r == who(ControlWho::Local)) return ControlRef::Local;
        return ControlRef::Other;
    }
    static bool flag_ret(const Json* c) { return c && as_int(c->get("ret")).value_or(0) != 0; }
    static std::int32_t int_ret(const Json* c) {
        return c ? static_cast<std::int32_t>(as_int(c->get("ret")).value_or(0)) : 0;
    }
    const Json* take(const char* fn, std::optional<ControlWho> w = std::nullopt) {
        if (broken_) return nullptr;
        if (next_ >= calls_.size()) {
            fail(std::string("extra call ") + fn);
            return nullptr;
        }
        const Json* c = calls_[next_];
        if (name(*c, "fn") != fn) {
            fail("call " + std::to_string(next_) + ": port " + fn + ", original " + name(*c, "fn"));
            return nullptr;
        }
        if (w && c->get("this") && name(*c, "this") != who(*w))
            fail(std::string(fn) + " this: port " + who(*w) + ", original " + name(*c, "this"));
        ++next_;
        return c;
    }
    void expect_int(const Json* c, const char* key, std::int64_t v) {
        if (c) m_.check_int(v, as_int(c->get(key)), where_, key);
    }
    void expect_float(const Json* c, const char* key, float v) {
        if (c) m_.check_float(v, as_f32(c->get(key)), where_, key);
    }
    void fail(const std::string& text) {
        broken_ = true;
        m_.report(where_, "calls", text);
    }
    std::vector<const Json*> calls_;
    std::size_t next_ = 0;
    bool initially_local_, local_null_, switched_ = false, broken_ = false;
    Fpu fpu_;
    const std::string& where_;
    Mismatch& m_;
};

ControlSpawn control_spawn(const Json& j) {
    ControlSpawn s;
    s.speed_multiplier = f32(j, "speed_multiplier");
    s.swim_state = static_cast<std::uint8_t>(i32(j, "swim_state"));
    // Older recordings call these levitating, race and levitate_speed.
    s.sneaking = flag(j, j.get("sneaking") ? "sneaking" : "levitating");
    s.stand_state = static_cast<std::uint8_t>(i32(j, j.get("stand_state") ? "stand_state" : "race"));
    s.walk_speed = f32(j, "walk_speed");
    s.run_speed = f32(j, "run_speed");
    s.sneak_speed = f32(j, j.get("sneak_speed") ? "sneak_speed" : "levitate_speed");
    s.height = f32(j, "height");
    s.spawn_id = u32(j, "spawn_id");
    s.follow_e40 = i32(j, "follow_e40");
    s.speed_run = f32(j, "speed_run");
    s.heading = f32(j, "heading");
    s.speed_heading = f32(j, "speed_heading");
    s.pitch = f32(j, "pitch");
    return s;
}

// ---- the collision sweep (game/sphere_sweep.h)
//
// The sweep is Hailstone's own implementation of the described behaviour, not the original's instruction stream, so
// its results are compared within a tolerance rather than bit for bit. Each function's tally reports how many records
// agreed exactly, how many needed the tolerance, and the largest differences seen.

namespace sweep = hailstone::game::sweep;

// The tolerances. t is a fraction of the move; normals and slide directions are unit vectors; points are world
// coordinates, so their tolerance grows with their size (a float's spacing near 2000 is 1.2e-4).
constexpr double kSweepTolT = 1e-4;
constexpr double kSweepTolUnit = 1e-3;
constexpr double kSweepTolPoint = 1e-3;
constexpr double kSweepTolPointRel = 1e-6;
constexpr double kSweepTolDistanceRel = 1e-4;
// The original keeps its intermediate points (the sphere's lowest point, where it meets the plane, the contact) in
// float memory even at 53-bit precision, so a hit normal, (centre - contact) / r, carries a float's error of the
// coordinates' size over r, and t one of their size over the move's length: the tolerance grows by this many floats
// of that size.
constexpr double kSweepTolFloats = 8.0;
// Near-grazing records: a hit or a miss decided within rounding of a boundary (the sphere touching an edge or vertex
// exactly, a centre on an edge's line or in the plane, a move parallel to the plane). One counts as grazing when
// moving the radius, the sweep's t limit or the start (by that fraction of the radius, along each axis) gives the
// recorded result.
constexpr double kSweepGrazing = 1e-4;
// The records that still disagree (none grazing, inputs a client could hold) may be at most this share of all;
// each is reported.
// They are capsule (edge) cases whose rule the description leaves open: see docs/unknowns.md.
constexpr double kSweepDisagreeShare = 0.01;

struct SweepTally {
    int records = 0, exact = 0, tolerated = 0, grazing = 0, contact_assumed = 0, unphysical = 0, disagree = 0;
    double worst_t = 0, worst_unit = 0, worst_point = 0, worst_distance = 0;
};
std::map<std::string, SweepTally> sweep_tallies;

// The difference between a result and a recorded one: NaN matches any NaN, an infinity only itself.
double sweep_diff(double got, double want) {
    if (std::isnan(got) || std::isnan(want)) return std::isnan(got) && std::isnan(want) ? 0.0 : HUGE_VAL;
    if (std::isinf(got) || std::isinf(want)) return got == want ? 0.0 : HUGE_VAL;
    return std::fabs(got - want);
}
bool sweep_same_bits(float got, float want) {
    return (std::isnan(got) && std::isnan(want)) || bits_of(got) == bits_of(want);
}

std::optional<std::array<float, 3>> sweep_floats(const Json* j) {
    auto* a = j ? std::get_if<std::shared_ptr<JsonArray>>(&j->v) : nullptr;
    if (!a || (*a)->size() != 3) return std::nullopt;
    std::array<float, 3> v{};
    for (std::size_t k = 0; k < 3; ++k) {
        auto f = as_f32(&(**a)[k]);
        if (!f) return std::nullopt;
        v[k] = *f;
    }
    return v;
}

template <class F>
sweep::Vec3<F> sweep_vec(const Json* j) {
    sweep::Vec3<F> v(0);
    if (auto f = sweep_floats(j))
        for (int k = 0; k < 3; ++k) v[k] = static_cast<F>((*f)[static_cast<std::size_t>(k)]);
    return v;
}

// One record's stop and slide comparison: every field within its tolerance, and whether all were exact.
struct SweepCompare {
    Mismatch& m;
    const std::string& w;
    bool exact = true;
    void field(double got, std::optional<float> want, double tolerance, double& worst, const char* what) {
        if (!want) {
            m.report(w, what, "missing");
            return;
        }
        double d = sweep_diff(got, *want);
        if (!sweep_same_bits(static_cast<float>(got), *want)) exact = false;
        if (d != HUGE_VAL) worst = std::max(worst, d);
        if (!(d <= tolerance)) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "got %.9g, want %.9g (tolerance %.3g)", got, static_cast<double>(*want),
                          tolerance);
            m.report(w, what, buf);
        }
    }
    template <class V>
    void vec(const V& got, const Json* want, double tolerance, double relative, double& worst, const char* what) {
        auto f = sweep_floats(want);
        if (!f) {
            m.report(w, what, "missing");
            return;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            double scale = std::isfinite((*f)[k]) ? std::fabs(static_cast<double>((*f)[k])) : 0.0;
            field(static_cast<double>(got[static_cast<int>(k)]), (*f)[k], tolerance + relative * scale, worst, what);
        }
    }
};

// CCollisionInfoSphere::vf0 @0x7a28e0: the sphere against one triangle record.
template <class F>
void check_sweep_triangle(const Json& in, const Json& out, const std::string& w,
                          const char* tally_name = "SweptSphereTriangle") {
    using V = sweep::Vec3<F>;
    SweepTally& tally = sweep_tallies[tally_name];
    ++tally.records;
    const V start = sweep_vec<F>(in.get("start")), delta = sweep_vec<F>(in.get("delta"));
    const V n = sweep_vec<F>(in.get("tri_normal"));
    const V v0 = sweep_vec<F>(in.get("v0")), v1 = sweep_vec<F>(in.get("v1")), v2 = sweep_vec<F>(in.get("v2"));
    const F inv_length = static_cast<F>(f32(in, "inv_length")), t_max = static_cast<F>(f32(in, "fraction_before"));
    const F radius = static_cast<F>(f32(in, "radius"));
    auto run = [&](const V& c, F t_limit, F r) {
        return sweep::sphere_triangle<F>(c, delta, inv_length, t_limit, r, n, v0, v1, v2);
    };

    // Records with infinite or NaN inputs, or a triangle normal that is not of unit length, test the original's
    // handling of garbage, which the client's triangle records never hold (CollisionTriangle::make): compared like
    // the rest, but a disagreement there is counted apart and is no failure.
    bool physical = std::isfinite(inv_length) && std::isfinite(t_max) && std::isfinite(radius) &&
                    std::fabs(static_cast<double>(glm::length(n)) - 1.0) <= 1e-3;
    double size = 0;
    for (const V& p : {start, delta, v0, v1, v2}) {
        for (int k = 0; k < 3; ++k) physical = physical && std::isfinite(p[k]);
        size = std::max(size, static_cast<double>(glm::length(p)));
    }

    const bool want_hit = flag(out, "hit");
    const std::optional<float> want_t = as_f32(out.get("fraction"));
    const std::optional<std::array<float, 3>> want_n = sweep_floats(out.get("normal"));
    const double move = static_cast<double>(glm::length(delta));
    const double tol_t = kSweepTolT + kSweepTolFloats * 0x1p-24 * size / std::max(move, 1e-30);
    const double tol_n = kSweepTolUnit + kSweepTolFloats * 0x1p-24 * (size + move) / static_cast<double>(radius);
    // Without a hit the sweep's t is untouched.
    auto t_of = [&](const sweep::TriangleHit<F>& h) { return static_cast<double>(h.hit ? h.t : t_max); };
    auto agrees = [&](const sweep::TriangleHit<F>& h) {
        if (h.hit != want_hit || !want_t || !(sweep_diff(t_of(h), *want_t) <= tol_t)) return false;
        if (!h.hit || h.contact_assumed) return true;
        if (!want_n) return false;
        for (int k = 0; k < 3; ++k)
            if (!(sweep_diff(static_cast<double>(h.normal[k]), (*want_n)[static_cast<std::size_t>(k)]) <= tol_n))
                return false;
        return true;
    };

    sweep::TriangleHit<F> h = run(start, t_max, radius);
    if (agrees(h)) {
        bool exact = want_t && sweep_same_bits(static_cast<float>(t_of(h)), *want_t);
        tally.worst_t = std::max(tally.worst_t, sweep_diff(t_of(h), want_t ? *want_t : 0.0f));
        if (h.hit && h.contact_assumed) {
            ++tally.contact_assumed; // the original's normal is whatever its stack held
        } else if (h.hit) {
            for (int k = 0; k < 3; ++k) {
                float wk = (*want_n)[static_cast<std::size_t>(k)];
                exact = exact && sweep_same_bits(static_cast<float>(h.normal[k]), wk);
                tally.worst_unit = std::max(tally.worst_unit, sweep_diff(static_cast<double>(h.normal[k]), wk));
            }
        }
        ++(exact ? tally.exact : tally.tolerated);
        return;
    }
    const F g = static_cast<F>(kSweepGrazing);
    for (F rs : {1 - g, F(1), 1 + g})
        for (F ts : {1 - g, F(1), 1 + g})
            for (int axis = -1; axis < 6; ++axis) {
                V c = start;
                if (axis >= 0) c[axis % 3] += (axis < 3 ? g : -g) * radius;
                if (agrees(run(c, t_max * ts, radius * rs))) {
                    ++tally.grazing;
                    return;
                }
            }
    if (!physical) {
        ++tally.unphysical;
        return;
    }
    ++tally.disagree;
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s: hit %d t %.7g, want hit %d t %.7g", w.c_str(), int(h.hit), t_of(h),
                  int(want_hit), want_t ? static_cast<double>(*want_t) : 0.0);
    MESSAGE(tally_name << " disagrees: " << buf);
}

// GetClampPoint @0x7a2680 and ComputeSkitter @0x7a2720 on a recorded info.
template <class F>
void check_stop_and_slide(const char* name, const sweep::Vec3<F>& from, const sweep::Vec3<F>& delta,
                          const sweep::Vec3<F>& direction, F t, bool hit, const sweep::Vec3<F>& normal, const Json& out,
                          const std::string& w, Mismatch& m) {
    SweepTally& tally = sweep_tallies[name];
    SweepCompare cmp{m, w};
    cmp.vec(sweep::clamp_point<F>(from, delta, t), out.get("stop"), kSweepTolPoint, kSweepTolPointRel,
            tally.worst_point, "stop");
    sweep::Skitter<F> k;
    if (hit) k = sweep::skitter<F>(direction, delta, t, normal);
    cmp.vec(k.direction, out.get("slide_direction"), kSweepTolUnit, 0.0, tally.worst_unit, "slide_direction");
    std::optional<float> want = as_f32(out.get("slide_distance"));
    double scale = want && std::isfinite(*want) ? std::fabs(static_cast<double>(*want)) : 0.0;
    cmp.field(static_cast<double>(k.distance), want, kSweepTolPoint + kSweepTolDistanceRel * scale,
              tally.worst_distance, "slide_distance");
    ++tally.records;
    ++(cmp.exact ? tally.exact : tally.tolerated);
}

std::map<std::string, Checker> checkers() {
    std::map<std::string, Checker> c;
    c["CharacterZoneClient::eqspa_movement_rate"] = [](const Json& rec, const Json& in, const Json& out,
                                                         const std::string& w, Mismatch& m) {
        const Json* spawn = in.get("spawn");
        if (is_null(spawn)) {
            if (!is_null(out.get("spawn"))) m.report(w, "spawn", "expected none");
            return;
        }
        SpawnSpeeds me = speeds_of(*spawn);
        std::optional<SpawnSpeeds> mount;
        if (!is_null(in.get("mount"))) mount = speeds_of(*in.get("mount"));
        bool items = i32(*spawn, "type") == 0 || i32(*spawn, "master_id") != 0 || flag(*spawn, "mercenary");
        Spa271Caches caches{call_ret(in, "buff_effect"), call_ret(in, "aa_effect"), call_ret(in, "item_effect")};
        apply_movement_rate(i32(in, "rate"), i32(in, "run_walk_state") != 0, caches, items, me,
                            mount ? &*mount : nullptr, precision_of(rec));
        check_speeds(me, out.get("spawn"), w, "spawn", m);
        if (mount) check_speeds(*mount, out.get("mount"), w, "mount", m);
    };
    c["EQ_Character::CalculateWeight"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                            Mismatch& m) {
        std::int32_t state = i32(in, "encumbered_message_state");
        if (is_null(in.get("spawn"))) {
            m.check_float(1.0f, as_f32(out.get("value")), w, "factor without a spawn");
            m.check_int(state, as_int(out.get("encumbered_message_state")), w, "message state");
            return;
        }
        WeightInput wi;
        wi.hp = call_ret(in, "cur_hp");
        wi.max_hp = call_ret(in, "max_hp");
        wi.level = i32(in, "level");
        wi.low_level_exempt_off = i32(in, "cb_byte_0x460") != 0;
        wi.carried = i32(in, "carried_weight");
        wi.strength = i32(in, "str");
        wi.bazaar = call_ret(in, "is_bazaar_zone") != 0;
        wi.barter = call_ret(in, "is_barter_zone") != 0;
        m.check_float(calculate_weight(wi, precision_of(rec)), as_f32(out.get("value")), w, "factor");
        std::uint32_t msg = encumbrance_message(state, wi.carried, wi.strength);
        m.check_int(state, as_int(out.get("encumbered_message_state")), w, "message state");
        const Json* chat = call(in, "chat_message");
        m.check_int(msg, chat ? as_int(chat->get("string_id")) : std::optional<std::int64_t>(0), w, "message");
    };
    c["PlayerClient::SetAccel"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                     Mismatch& m) {
        std::optional<AccelRider> rider;
        if (flag(in, "rider")) {
            const Json* pc = in.get("rider_pc");
            rider = AccelRider{!is_null(pc), !is_null(pc) && i32(*pc, "virtual_80") != 0};
        }
        float got = set_accel(f32(in, "speed_run"), f32(in, "target"), i32(in, "immediate") != 0, rider,
                              f32(in, "rate_scale"), f32(in, "run_speed"), precision_of(rec));
        m.check_float(got, as_f32(out.get("speed_run")), w, "speed_run");
    };
    c["PlayerClient_CanSwimJumpNow"] = [](const Json&, const Json& in, const Json& out, const std::string& w,
                                          Mismatch& m) {
        m.check_int(can_swim_jump_now(u32(in, "next_swim"), u32(in, "now")) ? 1 : 0, as_int(out.get("value")), w,
                    "value");
    };
    c["PlayerClient::DoSwimJump"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                       Mismatch& m) {
        const Json* probe = call(in, "is_swim_surface_clear");
        bool clear = probe && i32(*probe, "ret") != 0;
        SwimJump j = do_swim_jump(flag(in, "jump"), clear, f32(in, "swim_strength"), f32(in, "speed_z"), u32(in, "now"),
                                  u32(in, "next_swim"), precision_of(rec));
        m.check_float(j.speed_z, as_f32(out.get("speed_z")), w, "speed_z");
        m.check_int(j.next_swim, as_int(out.get("next_swim")), w, "next_swim");
        m.check_int(j.surface_probed, probe != nullptr, w, "surface probed");
        m.check_int(j.jumped, call(in, "push_along_heading") != nullptr, w, "pushed");
    };
    c["CharacterZoneClient_UpdateLevitationState"] = [](const Json&, const Json& in, const Json& out,
                                                        const std::string& w, Mismatch& m) {
        std::vector<std::int64_t> got;
        const Json* spawn = in.get("spawn");
        if (!is_null(spawn) && !flag(*spawn, "being_flung")) {
            std::optional<std::int32_t> rider;
            if (!is_null(in.get("rider"))) rider = i32(*in.get("rider"), "fly_mode");
            std::int32_t current = i32(*spawn, "fly_mode");
            std::int32_t mode = levitation_mode_value(current, i32(in, "levitate_buff") != 0,
                                                      i32(in, "levitate_buff_2") != 0, rider,
                                                      flag(in, "zone_flag_8") || flag(in, "no_levitate"),
                                                      flag(*spawn, "flying"));
            if (mode != current) {
                got.push_back(mode);
                if (flag(in, "mount")) got.push_back(mode);
            }
        }
        std::vector<std::int64_t> want;
        const Json* calls = out.get("set_fly_mode");
        if (auto* arr = calls ? std::get_if<std::shared_ptr<JsonArray>>(&calls->v) : nullptr)
            for (const Json& set : **arr) want.push_back(as_int(set.get("mode")).value_or(-1));
        m.check_int(static_cast<std::int64_t>(got.size()), static_cast<std::int64_t>(want.size()), w, "set_fly_mode calls");
        for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) m.check_int(got[i], want[i], w, "mode");
    };
    c["PcClient::MaxAirSupply"] = [](const Json&, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        m.check_int(max_air_supply(i32(in, "sta"), call_ret(in, "total_effect"), u32(in, "race")),
                    as_int(out.get("value")), w, "value");
    };
    c["PlayerZoneClient::IHaveFallen"] = [](const Json&, const Json& in, const Json& out, const std::string& w,
                                            Mismatch& m) {
        bool pc = flag(in, "pc");
        const Json* passed = out.get("apply_fall_damage");
        if (pc) {
            // fld / fstp: the damage passed on through the FPU.
            m.check_float(through_fpu(f32(in, "damage")), as_f32(passed), w, "damage passed");
        } else if (!is_null(passed)) {
            m.report(w, "apply_fall_damage", "expected no call");
        }
        auto sound = fall_sound(pc, static_cast<std::uint8_t>(i32(in, "spawn_type")), i32(in, "apply_fall_damage_result"));
        const Json* want = out.get("play_sound");
        if (is_null(want) != !sound) m.report(w, "play_sound", "presence differs");
        else if (sound) m.check_int(*sound, as_int(want), w, "play_sound");
    };
    c["CharacterZoneClient_ApplyFallDamage"] = [](const Json& rec, const Json& in, const Json& out,
                                                  const std::string& w, Mismatch& m) {
        m.check_int(1, as_int(out.get("fall_armed")), w, "fall_armed");
        if (!flag(in, "fall_armed")) {
            // The first fall after zone-in only arms the flag.
            m.check_int(0, as_int(out.get("value")), w, "value (first fall)");
            return;
        }
        FallDamageInput fi;
        fi.fall_speed = f32(in, "speed");
        fi.carried = i32(in, "curr_weight");
        fi.heroic_agi_bonus = i32(in, "heroic_agi_bonus");
        fi.safe_fall_adjusted = i32(in, "safe_fall_adjusted");
        fi.safe_fall_base = i32(in, "safe_fall_base");
        fi.spa228 = i32(in, "spa_228");
        fi.mounted = flag(in, "mounted");
        fi.no_fall_damage_zone = flag(in, "no_fall_damage");
        FallDamage d = apply_fall_damage(fi, precision_of(rec));
        m.check_int(d.damage, as_int(out.get("value")), w, "value");
        const Json* hit = out.get("hit");
        if (is_null(hit) != (d.damage <= 0)) {
            m.report(w, "hit", "presence differs");
        } else if (!is_null(hit)) {
            m.check_int(d.damage, as_int(hit->get("damage")), w, "hit damage");
            m.check_int(kEnvDamageFalling, as_int(hit->get("type")), w, "hit type");
            m.check_int(i32(in, "spawn_id"), as_int(hit->get("target_id")), w, "hit target");
        }
        const Json* hton = call(out, "hton");
        bool safe_fall_sent = hton && i32(*hton, "opcode") == 0x2219;
        m.check_int(d.safe_fall_used && flag(in, "connected"), safe_fall_sent, w, "OP_SafeFallSuccess");
    };
    c["PhysicsObject::GetEnvCoefficients"] = [](const Json& rec, const Json& in, const Json& out,
                                                const std::string& w, Mismatch& m) {
        const Json& sp = *in.get("spawn");
        const Json& ph = *in.get("physics");
        EnvSpawn s;
        s.colliding_type = i32(sp, "colliding_type");
        s.floor_height = f32(sp, "floor_height");
        s.acceleration_friction = f32(sp, "acceleration_friction");
        s.area_friction = f32(sp, "area_friction");
        s.sinks_in_water = flag(sp, "sinks_in_water");
        s.body_wet = static_cast<std::uint8_t>(i32(sp, "body_wet"));
        s.type = static_cast<std::uint8_t>(i32(sp, "type"));
        s.being_flung = flag(sp, "being_flung");
        s.spline_id = i32(sp, "spline_id");
        s.avatar_height = f32(sp, "avatar_height");
        EnvObject o{f32(ph, "y"), f32(ph, "x"), f32(ph, "z"), f32(ph, "speed_y"), f32(ph, "speed_x"),
                    f32(ph, "speed_z"), f32(ph, "speed_run")};
        RecordedEnv q(*in.get("answers"));
        auto coef = get_env_coefficients(flag(in, "levitating"), i32(in, "levitation_mode"), s, o, f32(in, "dt"), q,
                                      precision_of(rec));
        const Json& op = *out.get("physics");
        m.check_float(o.z, as_f32(op.get("z")), w, "z");
        m.check_float(o.speed_z, as_f32(op.get("speed_z")), w, "speed_z");
        const char* names[] = {"accel_friction", "accel_friction_b", "drag", "drag_b"};
        // null: unwritten, or written with the caller's own value (the
        // recorder cannot tell them apart), so only all four null means unwritten.
        bool written = false;
        for (const char* name : names) written = written || !is_null(out.get(name));
        if (written != coef.has_value()) {
            m.report(w, "coefficients", written ? "expected written" : "expected unwritten");
            return;
        }
        if (!coef) return;
        float got[] = {coef->accel_xy, coef->accel_z, coef->friction_xy, coef->friction_z};
        for (int k = 0; k < 4; ++k)
            if (!is_null(out.get(names[k]))) m.check_float(got[k], as_f32(out.get(names[k])), w, names[k]);
    };
    c["PlayerZoneClient_LandingAndFallCheck"] = [](const Json& rec, const Json& in, const Json& out,
                                                   const std::string& w, Mismatch& m) {
        const Json& sp = *in.get("spawn");
        const Json& a = *in.get("answers");
        LandingSpawn s;
        s.floor_height = f32(sp, "floor_height");
        s.avatar_height = f32(sp, "avatar_height");
        s.y = f32(sp, "y");
        s.x = f32(sp, "x");
        s.z = f32(sp, "z");
        s.speed_y = f32(sp, "speed_y");
        s.speed_x = f32(sp, "speed_x");
        s.speed_z = f32(sp, "speed_z");
        s.heading = f32(sp, "heading");
        s.fall_start_z = f32(sp, "fall_start_z");
        s.body_wet = static_cast<std::uint8_t>(i32(sp, "body_wet"));
        s.type = static_cast<std::uint8_t>(i32(sp, "type"));
        s.being_flung = flag(sp, "being_flung");
        s.fling_active_timer = i32(sp, "fling_active_timer");
        s.fling_timer_start = i32(sp, "fling_timer_start");
        LandingWorld world;
        world.connected = flag(a, "connected");
        world.zone_type = static_cast<std::uint8_t>(i32(a, "zone_type"));
        world.world_bottom = f32(a, "world_bottom");
        world.safe_floor = f32(a, "safe_floor");
        RecordedLanding q(a);
        LandingResult r = landing_and_fall_check(s, q, world, precision_of(rec));
        const Json& os = *out.get("spawn");
        m.check_float(s.z, as_f32(os.get("z")), w, "z");
        m.check_float(s.speed_y, as_f32(os.get("speed_y")), w, "speed_y");
        m.check_float(s.speed_x, as_f32(os.get("speed_x")), w, "speed_x");
        m.check_float(s.speed_z, as_f32(os.get("speed_z")), w, "speed_z");
        m.check_float(s.fall_start_z, as_f32(os.get("fall_start_z")), w, "fall_start_z");
        m.check_int(s.fling_active_timer, as_int(os.get("fling_active_timer")), w, "fling_active_timer");
        m.check_int(s.fling_timer_start, as_int(os.get("fling_timer_start")), w, "fling_timer_start");
        const Json* fall = out.get("fall_speed");
        if (is_null(fall) != !r.fall_speed) m.report(w, "fall_speed", "presence differs");
        else if (r.fall_speed) m.check_float(*r.fall_speed, as_f32(fall), w, "fall_speed");
        const Json* placed = out.get("placed_at");
        if (is_null(placed) != !r.placed) {
            m.report(w, "placed_at", "presence differs");
        } else if (r.placed) {
            m.check_float(r.placed->x, as_f32(placed->get("y")), w, "placed y");
            m.check_float(r.placed->y, as_f32(placed->get("x")), w, "placed x");
            m.check_float(r.placed->z, as_f32(placed->get("z")), w, "placed z");
            m.check_float(r.placed->w, as_f32(placed->get("heading")), w, "placed heading");
        }
        const Json* sound = call(out, "play_sound");
        m.check_int(r.land_sound, sound != nullptr, w, "landing sound");
        m.check_int(r.fling_stopped, has_virtual_call(out, "spawn_stop_fling"), w, "fling stopped");
        const Json* hton = call(out, "hton");
        m.check_int(r.vehicle_fall_sent, hton != nullptr, w, "vehicle fall sent");
    };
    c["PhysicsObject::Step"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                  Mismatch& m) {
        // What Step computes itself (its stages are mocked): the elapsed
        // steps it passes to vf18 and the position integration.
        const Json* calls = out.get("virtual_calls");
        auto* arr = calls ? std::get_if<std::shared_ptr<JsonArray>>(&calls->v) : nullptr;
        if (!arr) return;
        Fpu fpu = precision_of(rec);
        const Json& answers = *in.get("answers");
        for (const Json& vc : **arr) {
            const Json* name = vc.get("fn");
            auto* n = name ? std::get_if<std::string>(&name->v) : nullptr;
            if (n && *n == "time_scale") {
                float steps = elapsed_steps(u32(answers, "timer_now_1"), u32(*in.get("spawn"), "player_time_stamp"), fpu);
                m.check_float(steps, as_f32(vc.get("steps")), w, "steps");
            }
        }
        if (!has_virtual_call(out, "bob_and_sway")) return;
        float dt = f32(answers, "time_scale");
        const Json& pi = *in.get("physics");
        const Json& po = *out.get("physics");
        m.check_float(integrate_axis(f32(pi, "y"), f32(po, "speed_y"), dt, fpu), as_f32(po.get("y")), w, "y");
        m.check_float(integrate_axis(f32(pi, "x"), f32(po, "speed_x"), dt, fpu), as_f32(po.get("x")), w, "x");
        m.check_float(integrate_axis(f32(pi, "z"), f32(po, "speed_z"), dt, fpu), as_f32(po.get("z")), w, "z");
    };
    c["GetSwimmingSpeed"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        float scaled = 0.0f;
        double v = swimming_speed(f32(in, "speed"), i32(in, "swimming_skill"), precision_of(rec), f32(in, "speed_scale"),
                                  f32(in, "skill_scale"), &scaled);
        const Json* want = out.get("value");
        auto* d = want ? std::get_if<double>(&want->v) : nullptr;
        if (d && !(v == *d || (std::isnan(v) && std::isnan(*d)))) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "got %.17g, want %.17g", v, *d);
            m.report(w, "value", buf);
        }
        m.check_float(static_cast<float>(v), as_f32(out.get("value_float")), w, "value_float");
        m.check_float(scaled, as_f32(out.get("scaled_speed")), w, "scaled_speed");
    };
    c["CharacterZoneClient::HasSkill"] = [](const Json&, const Json& in, const Json& out, const std::string& w,
                                           Mismatch& m) {
        m.check_int(has_skill(i32(in, "adjusted_skill")), as_int(out.get("value")), w, "value");
    };
    c["0x516cd0"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        bool riding = flag(in, "local_has_mount") && flag(in, "controlled_is_that_mount");
        m.check_float(add_physics_pitch(f32(in, "pitch"), f32(in, "delta"), riding, precision_of(rec)),
                      as_f32(out.get("pitch")), w, "pitch");
    };
    c["unpack_physics"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        const Json& pi = *in.get("physics");
        SpawnPhysics p{f32(pi, "y"), f32(pi, "x"), f32(pi, "z"), f32(pi, "speed_y"), f32(pi, "speed_x"),
                       f32(pi, "speed_z"), f32(pi, "speed_run"), f32(pi, "heading"), f32(pi, "speed_heading"),
                       f32(pi, "pitch")};
        PackedPosition packed{};
        auto* arr = std::get_if<std::shared_ptr<JsonArray>>(&in.get("packed")->v);
        for (std::size_t k = 0; k < 5; ++k) packed[k] = static_cast<std::uint32_t>(as_int(&(**arr)[k]).value_or(0));
        std::int32_t mode = i32(in, "physics_mode");
        UnpackZ z{flag(in, "spawn"), mode == 2 || mode == 5, (i32(in, "keep_level") & 0xff) != 0, flag(in, "vf54"),
                  flag(in, "keep_z_global")};
        unpack_spawn_position(p, packed, z, precision_of(rec));
        const Json& po = *out.get("physics");
        m.check_float(p.y, as_f32(po.get("y")), w, "y");
        m.check_float(p.x, as_f32(po.get("x")), w, "x");
        m.check_float(p.z, as_f32(po.get("z")), w, "z");
        m.check_float(p.speed_y, as_f32(po.get("speed_y")), w, "speed_y");
        m.check_float(p.speed_x, as_f32(po.get("speed_x")), w, "speed_x");
        m.check_float(p.speed_z, as_f32(po.get("speed_z")), w, "speed_z");
        m.check_float(p.speed_run, as_f32(po.get("speed_run")), w, "speed_run");
        m.check_float(p.heading, as_f32(po.get("heading")), w, "heading");
        m.check_float(p.speed_heading, as_f32(po.get("speed_heading")), w, "speed_heading");
        m.check_float(p.pitch, as_f32(po.get("pitch")), w, "pitch");
    };
    c["Handle_OP_ClientUpdate"] = [](const Json&, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        auto who = [&](const char* key) {
            const Json* j = in.get(key);
            auto* s = j ? std::get_if<std::string>(&j->v) : nullptr;
            return s ? *s : std::string();
        };
        PackedPosition packed{};
        auto* arr = std::get_if<std::shared_ptr<JsonArray>>(&in.get("packed")->v);
        for (std::size_t k = 0; k < 5; ++k) packed[k] = static_cast<std::uint32_t>(as_int(&(**arr)[k]).value_or(0));
        std::string found = who("found");
        ClientUpdateInput ci;
        ci.found = found != "none";
        ci.has_mount = who("spawn_mount") != "none";
        ci.has_rider = who("spawn_rider") != "none";
        ci.in_game = i32(in, "game_state") == 5;
        ci.local_player = flag(in, "local_player");
        ci.now = u32(in, "now");
        std::uint32_t last = u32(in, "last_request");
        ClientUpdateResult r = handle_client_update(ci, packed, last);
        m.check_int(r.handled, as_int(out.get("value")), w, "value");
        auto* oarr = std::get_if<std::shared_ptr<JsonArray>>(&out.get("packed")->v);
        for (std::size_t k = 0; k < 5; ++k) m.check_int(packed[k], static_cast<std::uint32_t>(as_int(&(**oarr)[k]).value_or(0)), w, "packed");
        m.check_int(last, as_int(out.get("last_request")), w, "last_request");
        // Which object each step lands on: the one found, or the mount it rides (0x58cf90).
        std::string target = r.unpack_rider ? who("mount_or_self") : found;
        bool spawn_stamped = (r.unpack_rider && found == "spawn") || (r.unpack && target == "spawn");
        bool mount_stamped = (r.unpack_rider && found == "mount") || (r.unpack && target == "mount");
        m.check_int(spawn_stamped, as_int(out.get("spawn_stamped")), w, "spawn_stamped");
        m.check_int(mount_stamped, as_int(out.get("mount_stamped")), w, "mount_stamped");
        m.check_int(r.unpack && r.stop && target == "spawn" ? 6 : 0, as_int(out.get("spawn_motion_state")), w,
                    "spawn_motion_state");
        m.check_int(r.unpack && r.stop && target == "mount" ? 6 : 0, as_int(out.get("mount_motion_state")), w,
                    "mount_motion_state");
    };
    c["0x4877e0"] = [](const Json&, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        auto who = [&](const char* key) {
            const Json* j = in.get(key);
            auto* s = j ? std::get_if<std::string>(&j->v) : nullptr;
            return s ? *s : std::string();
        };
        bool spawn = flag(in, "actor") && flag(in, "client") && flag(in, "spawn");
        AdvanceInput a;
        a.hidden = flag(in, "hidden_1e54") || flag(in, "hidden_1e55");
        a.is_local = who("local_player") == "this_spawn";
        a.is_controlled = who("controlled_player") == "this_spawn";
        a.controlled_by_player = who("controlled_player") != "none" && who("controlled_controls") == "this_spawn";
        a.ms_since_update = u32(in, "ms_since_update");
        std::int32_t type = i32(in, "type");
        a.corpse = (type & 0xff) >= 2; // PlayerClient::IsCorpse: Type 2 and up
        a.speed_run = f32(in, "speed_run");
        a.speed_z = f32(in, "speed_z");
        a.force_move = flag(in, "force_move");
        bool stepped = spawn && advance_spawn(a);
        m.check_int(stepped, as_int(out.get("stepped")), w, "stepped");
    };
    c["CEverQuest::ProcessControls"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                          Mismatch& m) {
        ControlsState st;
        const Json& k = *in.get("keys");
        ControlKeys& keys = st.keys;
        keys.forward = flag(k, "forward");
        keys.backward = flag(k, "backward");
        keys.turn_left = flag(k, "turn_left");
        keys.turn_right = flag(k, "turn_right");
        keys.strafe_left = flag(k, "strafe_left");
        keys.strafe_right = flag(k, "strafe_right");
        keys.look_up = flag(k, "look_up");
        keys.look_down = flag(k, "look_down");
        keys.zoom_in = flag(k, "zoom_in");
        keys.zoom_out = flag(k, "zoom_out");
        keys.camera_pan_plus = flag(k, "camera_pan_plus");
        keys.camera_pan_minus = flag(k, "camera_pan_minus");
        keys.camera_heading_plus = flag(k, "camera_heading_plus");
        keys.camera_heading_minus = flag(k, "camera_heading_minus");
        keys.camera_pitch_plus = flag(k, "camera_pitch_plus");
        keys.camera_pitch_minus = flag(k, "camera_pitch_minus");
        keys.key_148c = flag(k, "key_148c");
        keys.center_view = flag(k, "center_view");
        st.jump_key = in.get("jump_key") ? i32(in, "jump_key") : (flag(k, "jump") ? 1 : 0);
        st.autorun = static_cast<std::uint8_t>(i32(in, "autorun_flags"));
        st.auto_forward = flag(in, "auto_forward");
        st.mouse_look = flag(in, "mouse_look");
        st.turn_keys_strafe = flag(in, "turn_keys_strafe");
        st.forward_sits = flag(in, "forward_sits");
        st.run_walk_state = static_cast<std::uint8_t>(i32(in, "run_walk_state"));
        st.skip_controls = flag(in, "skip_controls");
        st.quiet_net_log = flag(in, "quiet_net_log");
        st.force_move = flag(in, "force_move");
        st.attack_on = flag(in, "attack_on");
        st.has_target = flag(in, "has_target");
        st.connected = flag(in, "connected");
        st.rate_scale = f32(in, "rate_scale");
        st.now = u32(in, "now");
        st.pc_stunned_fc = static_cast<std::int8_t>(i32(in, "pc_stunned_fc"));
        st.controlled_is_local = flag(in, "controlled_is_local");
        st.local_null = flag(in, "local_null");
        st.controlled = control_spawn(*in.get("controlled"));
        if (!is_null(in.get("local"))) st.local = control_spawn(*in.get("local"));
        st.camera_index = i32(in, "camera_index");
        const Json& cam = *in.get("camera");
        st.camera = {f32(cam, "pan"), f32(cam, "pitch"), f32(cam, "distance")};
        st.strafing = static_cast<std::uint8_t>(i32(in, "strafing"));
        st.strafe_scale = f32(in, "strafe_scale");
        st.held_scale = f32(in, "held_scale_148c");
        st.turn_left_count = i32(in, "turn_left_count");
        st.turn_right_count = i32(in, "turn_right_count");
        st.look_up_count = i32(in, "look_up_count");
        st.look_down_count = i32(in, "look_down_count");
        st.jump_started = u32(in, "jump_started");
        st.jump_ended = u32(in, "jump_ended");
        st.last_sent_heading = f32(in, "last_sent_heading");
        st.last_sent_speed = f32(in, "last_sent_speed");
        st.local_dirty = flag(in, "local_position_dirty");
        st.other_dirty = flag(in, "other_position_dirty");
        st.primary_ready = flag(in, "ranged_queued");
        st.secondary_ready = flag(in, "throw_queued");
        bool initially_local = st.controlled_is_local;
        RecordedControls q(*out.get("calls"), st.controlled_is_local, st.local_null, precision_of(rec), w, m);
        process_controls(st, q, precision_of(rec));
        q.finish();
        // out.controlled is the object that was controlled at the start: after a hand-back, the boat.
        const ControlSpawn& got = !initially_local && st.controlled_is_local ? st.local : st.controlled;
        ControlSpawn want = control_spawn(*out.get("controlled"));
        m.check_float(got.speed_multiplier, want.speed_multiplier, w, "speed_multiplier");
        m.check_float(got.speed_run, want.speed_run, w, "speed_run");
        m.check_float(got.heading, want.heading, w, "heading");
        m.check_float(got.speed_heading, want.speed_heading, w, "speed_heading");
        m.check_float(got.pitch, want.pitch, w, "pitch");
        m.check_int(got.follow_e40, want.follow_e40, w, "follow_e40");
        m.check_int(st.controlled_is_local, as_int(out.get("controlled_is_local")), w, "controlled_is_local");
        m.check_int(st.strafing, as_int(out.get("strafing")), w, "strafing");
        m.check_float(st.strafe_scale, as_f32(out.get("strafe_scale")), w, "strafe_scale");
        m.check_float(st.held_scale, as_f32(out.get("held_scale_148c")), w, "held_scale");
        m.check_int(st.turn_left_count, as_int(out.get("turn_left_count")), w, "turn_left_count");
        m.check_int(st.turn_right_count, as_int(out.get("turn_right_count")), w, "turn_right_count");
        m.check_int(st.look_up_count, as_int(out.get("look_up_count")), w, "look_up_count");
        m.check_int(st.look_down_count, as_int(out.get("look_down_count")), w, "look_down_count");
        m.check_int(st.jump_key, as_int(out.get("jump_key")), w, "jump_key");
        m.check_int(static_cast<std::int32_t>(st.jump_started), as_int(out.get("jump_started")), w, "jump_started");
        m.check_int(static_cast<std::int32_t>(st.jump_ended), as_int(out.get("jump_ended")), w, "jump_ended");
        m.check_float(st.last_sent_heading, as_f32(out.get("last_sent_heading")), w, "last_sent_heading");
        m.check_float(st.last_sent_speed, as_f32(out.get("last_sent_speed")), w, "last_sent_speed");
        m.check_int(st.local_dirty, as_int(out.get("local_position_dirty")), w, "local_dirty");
        m.check_int(st.other_dirty, as_int(out.get("other_position_dirty")), w, "other_dirty");
        m.check_int(st.primary_ready, as_int(out.get("ranged_queued")), w, "ranged_queued");
        m.check_int(st.secondary_ready, as_int(out.get("throw_queued")), w, "throw_queued");
    };
    c["Handle_OP_NPCMoveUpdate"] = [](const Json&, const Json& in, const Json& out, const std::string& w,
                                      Mismatch& m) {
        const Json* hex = in.get("packet");
        auto* text = hex ? std::get_if<std::string>(&hex->v) : nullptr;
        std::vector<std::uint8_t> data;
        for (std::size_t i = 0; text && i + 1 < text->size(); i += 2)
            data.push_back(static_cast<std::uint8_t>(std::strtoul(text->substr(i, 2).c_str(), nullptr, 16)));
        data.resize(std::min<std::size_t>(data.size(), u32(in, "size")));
        auto got = unpack_npc_move_update(data);
        const Json* want = out.get("record");
        auto* arr = want ? std::get_if<std::shared_ptr<JsonArray>>(&want->v) : nullptr;
        if (!arr || (*arr)->size() != got.size()) {
            m.report(w, "record", "missing");
            return;
        }
        for (std::size_t k = 0; k < got.size(); ++k)
            m.check_int(got[k], static_cast<std::uint32_t>(as_int(&(**arr)[k]).value_or(0)), w, "record");
    };
    auto height_spawn = [](const Json& in) {
        CollisionHeightSpawn s;
        s.stand_state = static_cast<std::uint8_t>(u32(in, "animation"));
        s.mount_race = flag(in, "mount_race");
        s.has_rider = flag(in, "has_rider");
        s.avatar_height = f32(in, "avatar_height");
        s.length = f32(in, "body_height_0x144");
        return s;
    };
    c["PlayerClient_GetCollisionHeight"] = [height_spawn](const Json& rec, const Json& in, const Json& out,
                                                          const std::string& w, Mismatch& m) {
        m.check_double(collision_height(height_spawn(in), precision_of(rec)), as_f64(out.get("value")), w, "value");
    };
    c["PlayerClient::CheckRegionChange"] = [height_spawn](const Json& rec, const Json& in, const Json& out,
                                                          const std::string& w, Mismatch& m) {
        struct Tape : RegionQueries {
            const Json& in;
            std::vector<std::string> calls;
            std::vector<std::int64_t> args;
            explicit Tape(const Json& i) : in(i) {}
            bool inside(const RegionPoint&) override { return flag(in, "inside"); }
            std::uint32_t regions(const RegionPoint&) override { return u32(in, "region_flags"); }
            bool do_teleport(const RegionPoint& p) override {
                calls.push_back("do_teleport");
                args.push_back(static_cast<std::int32_t>(bits_of(p.x)));
                args.push_back(static_cast<std::int32_t>(bits_of(p.y)));
                args.push_back(static_cast<std::int32_t>(bits_of(p.z)));
                return flag(in, "teleport_succeeds");
            }
            bool is_player_spawn() override {
                calls.push_back("is_player_spawn");
                return flag(in, "player_spawn");
            }
            void print(std::uint32_t id) override {
                calls.push_back("get_string");
                args.push_back(id);
                calls.push_back("chat");
            }
            void feet_environment_change(std::uint32_t flags) override {
                calls.push_back("feet_environment_change");
                args.push_back(static_cast<std::int32_t>(flags));
            }
        } tape(in);
        RegionSpawn s;
        s.body = height_spawn(in);
        s.type = static_cast<std::uint8_t>(u32(in, "spawn_type"));
        s.held_flags = u32(in, "held_flags");
        RegionZone zone;
        zone.area_friction = f32(in, "zone_area_friction");
        zone.acceleration_friction = f32(in, "zone_acceleration_friction");
        zone.zone_id = i32(in, "zone_id");
        RegionPoint at{f32(in, "x"), f32(in, "y"), f32(in, "z")};
        RegionChange r = check_region_change(s, at, zone, tape, precision_of(rec));
        m.check_int(r.teleported, as_int(out.get("value")), w, "value");
        m.check_int(r.asked, as_int(out.get("asked_regions")), w, "asked_regions");
        if (const Json* sample = out.get("sample")) {
            m.check_float(r.sample.x, as_f32(sample->get("x")), w, "sample.x");
            m.check_float(r.sample.y, as_f32(sample->get("y")), w, "sample.y");
            m.check_float(r.sample.z, as_f32(sample->get("z")), w, "sample.z");
        }
        m.check_int(static_cast<std::int32_t>(s.held_flags), as_int(out.get("held_flags")), w, "held_flags");
        // Fields the original left alone hold what was there before, which the record does not give.
        if (r.asked && static_cast<std::int32_t>(s.held_flags) != i32(in, "held_flags")) {
            m.check_float(s.area_friction, as_f32(out.get("area_friction")), w, "area_friction");
            m.check_float(s.acceleration_friction, as_f32(out.get("acceleration_friction")), w,
                          "acceleration_friction");
            if (!r.teleported) m.check_int(s.pvp_flag, as_int(out.get("flag_0x2e1")), w, "flag_0x2e1");
        }
        const Json* want = out.get("calls");
        auto* arr = want ? std::get_if<std::shared_ptr<JsonArray>>(&want->v) : nullptr;
        std::size_t n = arr ? (*arr)->size() : 0;
        if (n != tape.calls.size()) {
            m.report(w, "calls", "got " + std::to_string(tape.calls.size()) + ", want " + std::to_string(n));
            return;
        }
        std::size_t a = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const Json& rc = (**arr)[k];
            const Json* fn = rc.get("fn");
            auto* name = fn ? std::get_if<std::string>(&fn->v) : nullptr;
            if (!name || *name != tape.calls[k]) {
                m.report(w, "calls", "call " + std::to_string(k) + " is " + tape.calls[k]);
                return;
            }
            if (*name == "do_teleport") {
                m.check_int(tape.args[a++], as_int(rc.get("x_bits")), w, "do_teleport x");
                m.check_int(tape.args[a++], as_int(rc.get("y_bits")), w, "do_teleport y");
                m.check_int(tape.args[a++], as_int(rc.get("z_bits")), w, "do_teleport z");
            } else if (*name == "get_string") {
                m.check_int(tape.args[a++], as_int(rc.get("eqstr")), w, "eqstr");
            } else if (*name == "feet_environment_change") {
                m.check_int(tape.args[a++], as_int(rc.get("flags")), w, "flags");
            }
        }
    };
    c["PhysicsObject::ApplyFriction"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                           Mismatch& m) {
        float speed[3] = {f32(in, "speed_y"), f32(in, "speed_x"), f32(in, "speed_z")};
        float push[3] = {f32(in, "push_y"), f32(in, "push_x"), f32(in, "push_z")};
        apply_friction(speed, push, f32(in, "friction"), f32(in, "friction_z"), precision_of(rec));
        m.check_float(speed[0], as_f32(out.get("speed_y")), w, "speed_y");
        m.check_float(speed[1], as_f32(out.get("speed_x")), w, "speed_x");
        m.check_float(speed[2], as_f32(out.get("speed_z")), w, "speed_z");
    };
    c["PhysicsObject::vf3 (the run-speed push)"] = [](const Json& rec, const Json& in, const Json& out,
                                                      const std::string& w, Mismatch& m) {
        // The recorder's fakes: the table and vf0 return their argument; vf4
        // adds the answers with a float add (SSE).
        struct Fakes : PushQueries {
            const Json& answers;
            explicit Fakes(const Json& a) : answers(a) {}
            double cos(float angle) override { return angle; }
            double sin(float angle) override { return angle; }
            double vf0(float value) override { return value; }
            // SSE addss: a NaN result is the first NaN operand, quieted.
            static float add(float x, float y) {
                if (std::isnan(x) || std::isnan(y)) return static_cast<float>(st_value(std::isnan(x) ? x : y).v);
                return x + y;
            }
            void add_push(float p[3]) override {
                p[0] = add(p[0], f32(answers, "vf4_adds_y"));
                p[1] = add(p[1], f32(answers, "vf4_adds_x"));
                p[2] = add(p[2], f32(answers, "vf4_adds_z"));
            }
        } q(*in.get("answers"));
        float push[3] = {f32(in, "push_y"), f32(in, "push_x"), f32(in, "push_z")};
        run_speed_push(f32(in, "speed_run"), f32(in, "heading"), f32(in, "camera_angle"), push, flag(in, "spawn"),
                       f32(in, "a"), f32(in, "b"), q, precision_of(rec));
        m.check_float(push[0], as_f32(out.get("push_y")), w, "push_y");
        m.check_float(push[1], as_f32(out.get("push_x")), w, "push_x");
        m.check_float(push[2], as_f32(out.get("push_z")), w, "push_z");
    };
    c["PhysicsObject_SlowInMode5 (0x8d0800)"] = [](const Json& rec, const Json& in, const Json& out,
                                                       const std::string& w, Mismatch& m) {
        // The spawn's vf64 is the walk speed times 1.0, unrounded.
        double walk = x87_mul(precision_of(rec), st_value(f32(in, "walk_speed")), st_value(1.0f));
        m.check_int(slow_in_mode5(i32(in, "levitation_mode"), walk, f32(in, "speed_run")), as_int(out.get("value")), w,
                    "value");
    };
    c["PlayerClient_IsSmallBoatRace (PlayerClient vf +0x58)"] = [](const Json&, const Json& in, const Json& out,
                                                                   const std::string& w, Mismatch& m) {
        m.check_int(is_small_boat_race(flag(in, "race_gender_info"), u32(in, "race_flags")), as_int(out.get("value")),
                    w, "value");
    };
    c["PhysicsObject::vf15 (buoyancy)"] = [](const Json&, const Json& in, const Json& out, const std::string& w,
                                             Mismatch& m) {
        m.check_float(physics_buoyancy(flag(in, "is_controlled"), f32(in, "move_down_buoyancy"),
                                       f32(in, "buoyancy_in")),
                      as_f32(out.get("buoyancy")), w, "buoyancy");
    };
    c["PlayerZoneClient_DoCollisionMovement"] = [](const Json& rec, const Json& in, const Json& out,
                                                   const std::string& w, Mismatch& m) {
        Fpu fpu = precision_of(rec);
        const Json& a = *in.get("answers");
        auto vec = [](const Json* j) {
            Vec3f v{};
            if (j) v = {f32(*j, "x"), f32(*j, "y"), f32(*j, "z")};
            return v;
        };
        // A logged call: its name and its arguments (floats by bits, or ints).
        struct Arg {
            std::string key;
            bool is_float;
            float f;
            std::int64_t i;
        };
        struct Logged {
            std::string fn;
            std::vector<Arg> args;
        };
        struct Tape : CollisionQueries {
            const Json& a;
            const Fpu& fpu;
            std::vector<Logged> virtual_calls, calls;
            std::uint32_t rider_time = 0;
            std::size_t sweeps = 0;
            std::function<Vec3f(const Json*)> vec;
            Tape(const Json& answers, const Fpu& f, std::function<Vec3f(const Json*)> v)
                : a(answers), fpu(f), vec(std::move(v)) {}
            bool byte(const char* name) const { return (u32(a, name) & 0xff) != 0; }
            void vcall(const char* fn, std::vector<Arg> args = {}) { virtual_calls.push_back({fn, std::move(args)}); }
            void call(const char* fn, std::vector<Arg> args = {}) { calls.push_back({fn, std::move(args)}); }
            static Arg fa(const char* k, float v) { return {k, true, v, 0}; }
            static Arg ia(const char* k, std::int64_t v) { return {k, false, 0.0f, v}; }
            static std::vector<Arg> xyz(const Vec3f& p, const char* x = "x", const char* y = "y", const char* z = "z") {
                return {fa(x, p[0]), fa(y, p[1]), fa(z, p[2])};
            }
            // The fakes leave value * 1.0 on the FPU, unrounded.
            double product(const char* name) const { return x87_mul(fpu, st_value(f32(a, name)), X87(1.0f)); }

            Vec3f actor_location() override { return vec(a.get("actor_location")); }
            bool zone_collision_loaded() override {
                vcall("zone_collision_loaded");
                return byte("zone_collision_loaded");
            }
            bool scene_box(Vec3f& min, Vec3f& max) override {
                vcall("scene_box");
                min = vec(a.get("scene_min"));
                max = vec(a.get("scene_max"));
                return true;
            }
            void zone_client() override { vcall("zone_client"); }
            bool mounted() override {
                call("get_mount");
                return flag(a, "mounted");
            }
            bool is_vehicle_race() override {
                vcall("is_vehicle_race");
                return byte("is_vehicle_race");
            }
            bool has_property(std::uint32_t property) override {
                call("has_property", {ia("property", property), ia("a2", 0), ia("a3", 0)});
                return byte("has_property_64");
            }
            double height() override {
                vcall("height");
                return product("height");
            }
            double collision_radius() override {
                vcall("collision_radius");
                return product("collision_radius");
            }
            bool moving_state() override {
                call("moving_state");
                return byte("moving_state");
            }
            bool rider() override {
                call("get_rider");
                return flag(a, "rider");
            }
            void set_rider_stand_time(std::uint32_t time) override { rider_time = time; }
            std::uint32_t display_time() override { return u32(a, "display_time"); }
            bool sweep_request(const std::array<float, 7>& r) override {
                vcall("sweep_request", {fa("x", r[0]), fa("y", r[1]), fa("z", r[2]), fa("dx", r[3]), fa("dy", r[4]),
                                        fa("dz", r[5]), fa("radius", r[6])});
                return byte("sweep_request");
            }
            bool is_controlled() override {
                vcall("is_controlled");
                return byte("is_controlled");
            }
            // The recorder's mock (SSE float arithmetic): the case's fraction and
            // normal, the hit `start + offset`, or start + (end - start) * t for
            // 0 < t < 1, else the end.
            void sweep(const Vec3f& from, const Vec3f& to, bool, bool, SweepResult& r) override {
                std::vector<Arg> args = xyz(from, "from_x", "from_y", "from_z");
                for (const Arg& x : xyz(to, "to_x", "to_y", "to_z")) args.push_back(x);
                vcall("sweep", std::move(args));
                const Json* list = a.get("sweeps");
                auto* arr = list ? std::get_if<std::shared_ptr<JsonArray>>(&list->v) : nullptr;
                if (!arr || (*arr)->empty()) return;
                const Json& s = (**arr)[std::min(sweeps++, (*arr)->size() - 1)];
                float t = f32(s, "fraction");
                const Json* off = s.get("hit_offset_from_start");
                Vec3f o = vec(off);
                for (int k = 0; k < 3; ++k) {
                    volatile float fa_ = from[static_cast<std::size_t>(k)], fb = to[static_cast<std::size_t>(k)];
                    if (off) {
                        volatile float sum = fa_ + o[static_cast<std::size_t>(k)];
                        r.position[static_cast<std::size_t>(k)] = sum;
                    } else if (t > 0.0f && t < 1.0f) {
                        volatile float d = fb - fa_;
                        volatile float p = d * t;
                        volatile float sum = fa_ + p;
                        r.position[static_cast<std::size_t>(k)] = sum;
                    } else {
                        r.position[static_cast<std::size_t>(k)] = to[static_cast<std::size_t>(k)];
                    }
                }
                r.skitter_dir = vec(s.get("normal"));
                r.skitter_dist = t;
            }
            void no_collide() override { call("no_collide"); }
            void move_without_collision(const Vec3f&, Vec3f& o) override {
                call("move_without_collision");
                o = vec(a.get("move_without_collision_out"));
            }
            bool in_scene_region(const Vec3f& p) override {
                vcall("in_scene_region", xyz(p));
                return byte("in_scene_region");
            }
            bool can_i_fit_here(const Vec3f&, std::uint32_t fl) override {
                call("can_i_fit_here", {ia("flag", fl)});
                return byte("can_i_fit_here");
            }
            void clear_stuck(std::uint32_t arg) override { call("clear_stuck", {ia("arg", arg)}); }
            bool check_stuck() override {
                call("check_stuck");
                return byte("check_stuck");
            }
            bool collision_test(const CollisionTestInfo& i) override {
                vcall("collision_test",
                      {fa("from_x", i.from[0]), fa("from_y", i.from[1]), fa("from_z", i.from[2]), fa("to_x", i.to[0]),
                       fa("to_y", i.to[1]), fa("to_z", i.to[2]), fa("radius", i.radius),
                       fa("radius_squared", i.radius_squared), ia("has_object", i.has_object), ia("flag", i.flag)});
                return byte("collision_test");
            }
            void set_actor_location(const Vec3f& p) override { vcall("set_actor_location", xyz(p)); }
            bool is_player_spawn() override {
                call("is_player_spawn");
                return byte("is_player_spawn");
            }
            void after_place() override { call("after_place"); }
            bool can_swim_jump_now() override {
                call("can_swim_jump_now");
                return byte("can_swim_jump_now");
            }
            bool on_ladder() override {
                call("on_ladder_check");
                return byte("on_ladder_check");
            }
            void finish_move() override { call("finish_move"); }
            void after_move(const Vec3f& from, const Vec3f& actor) override {
                std::vector<Arg> args = xyz(from, "from_x", "from_y", "from_z");
                for (const Arg& x : xyz(actor, "actor_x", "actor_y", "actor_z")) args.push_back(x);
                vcall("after_move", std::move(args));
            }
        } tape(a, fpu, vec);

        static const char* kPhysics[] = {"y", "x", "z", "speed_y", "speed_x", "speed_z", "speed_run", "heading",
                                         "angle", "accel_angle", "speed_heading"};
        const Json& sin = *in.get("spawn");
        const Json* ring_out = out.get("ring_entry");
        auto* ring_arr = ring_out ? std::get_if<std::shared_ptr<JsonArray>>(&ring_out->v) : nullptr;
        CollisionSpawn s;
        for (std::size_t k = 0; k < 11; ++k) s.physics[k] = f32(sin, kPhysics[k]);
        // +0x90 is not in the record's spawn; the ring entry's copy of it is.
        if (ring_arr && (*ring_arr)->size() == 29) s.physics[11] = as_f32(&(**ring_arr)[11]).value_or(0.0f);
        s.floor_height = f32(sin, "floor_height");
        s.colliding_type = i32(sin, "colliding_type");
        s.body_wet = static_cast<std::uint8_t>(u32(sin, "body_wet"));
        s.possibly_stuck = static_cast<std::uint8_t>(u32(sin, "possibly_stuck"));
        s.flag_19d = static_cast<std::uint8_t>(u32(sin, "flag_19d"));
        s.stand_time = u32(sin, "stand_time");
        s.step_count = u32(sin, "step_count");
        s.ring_index = u32(sin, "ring_index");
        s.ring_count = u32(sin, "ring_count");
        s.last_good = vec(sin.get("last_good"));
        std::uint32_t entry_index = s.ring_index;
        Vec3f target = vec(in.get("target"));
        CollisionMove r = do_collision_movement(s, target, tape, fpu);

        m.check_int(r.stopped ? 1 : 0, as_int(out.get("value")), w, "value");
        Vec3f want_target = vec(out.get("target"));
        for (int k = 0; k < 3; ++k)
            m.check_float(target[static_cast<std::size_t>(k)], want_target[static_cast<std::size_t>(k)], w, "target");
        const Json& so = *out.get("spawn");
        for (std::size_t k = 0; k < 11; ++k) m.check_float(s.physics[k], as_f32(so.get(kPhysics[k])), w, kPhysics[k]);
        m.check_float(s.floor_height, as_f32(so.get("floor_height")), w, "floor_height");
        m.check_int(s.colliding_type, as_int(so.get("colliding_type")), w, "colliding_type");
        m.check_int(s.body_wet, as_int(so.get("body_wet")), w, "body_wet");
        m.check_int(s.possibly_stuck, as_int(so.get("possibly_stuck")), w, "possibly_stuck");
        m.check_int(s.flag_19d, as_int(so.get("flag_19d")), w, "flag_19d");
        m.check_int(static_cast<std::int32_t>(s.stand_time), as_int(so.get("stand_time")), w, "stand_time");
        m.check_int(static_cast<std::int32_t>(s.step_count), as_int(so.get("step_count")), w, "step_count");
        m.check_int(s.ring_index, as_int(so.get("ring_index")), w, "ring_index");
        m.check_int(s.ring_count, as_int(so.get("ring_count")), w, "ring_count");
        Vec3f good = vec(so.get("last_good"));
        for (int k = 0; k < 3; ++k)
            m.check_float(s.last_good[static_cast<std::size_t>(k)], good[static_cast<std::size_t>(k)], w, "last_good");
        m.check_int(static_cast<std::int32_t>(tape.rider_time), as_int(out.get("rider_stand_time")), w,
                    "rider_stand_time");
        // The ring entry; after an early return its after part was not written
        // (the record shows what the slot held before, which the input lacks).
        if (!ring_arr || (*ring_arr)->size() != 29) {
            m.report(w, "ring_entry", "missing");
        } else {
            const MovementStat& e = s.ring[entry_index % 20];
            std::vector<float> got(e.before.begin(), e.before.end());
            got.insert(got.end(), e.target.begin(), e.target.end());
            got.push_back(e.floor_before);
            got.push_back(e.floor_after);
            got.insert(got.end(), e.after.begin(), e.after.end());
            std::size_t n = r.early ? 16 : 29;
            for (std::size_t k = 0; k < n; ++k) m.check_float(got[k], as_f32(&(**ring_arr)[k]), w, "ring_entry");
        }
        auto compare = [&](const char* key, const std::vector<Logged>& got) {
            const Json* want = out.get(key);
            auto* arr = want ? std::get_if<std::shared_ptr<JsonArray>>(&want->v) : nullptr;
            std::size_t n = arr ? (*arr)->size() : 0;
            if (n != got.size()) {
                std::string names;
                for (const Logged& l : got) names += l.fn + " ";
                m.report(w, key, "got " + std::to_string(got.size()) + ", want " + std::to_string(n) + ": " + names);
                return;
            }
            for (std::size_t k = 0; k < n; ++k) {
                const Json& rc = (**arr)[k];
                const Json* fn = rc.get("fn");
                auto* name = fn ? std::get_if<std::string>(&fn->v) : nullptr;
                if (!name || *name != got[k].fn) {
                    m.report(w, key, "call " + std::to_string(k) + " is " + got[k].fn);
                    return;
                }
                for (const Arg& arg : got[k].args) {
                    std::string what = got[k].fn + "." + arg.key;
                    if (arg.is_float) m.check_float(arg.f, as_f32(rc.get(arg.key)), w, what.c_str());
                    else m.check_int(arg.i, as_int(rc.get(arg.key)), w, what.c_str());
                }
            }
        };
        compare("virtual_calls", tape.virtual_calls);
        compare("calls", tape.calls);
    };
    auto arr3 = [](const Json* j) {
        std::array<float, 3> v{};
        auto* a = j ? std::get_if<std::shared_ptr<JsonArray>>(&j->v) : nullptr;
        if (a && (*a)->size() == 3)
            for (std::size_t k = 0; k < 3; ++k) v[k] = as_f32(&(**a)[k]).value_or(0.0f);
        return v;
    };
    auto check3 = [](Mismatch& m, const std::array<float, 3>& got, const Json* want, const std::string& w,
                     const char* what) {
        auto* a = want ? std::get_if<std::shared_ptr<JsonArray>>(&want->v) : nullptr;
        if (!a || (*a)->size() != 3) {
            m.report(w, what, "missing");
            return;
        }
        for (std::size_t k = 0; k < 3; ++k) m.check_float(got[k], as_f32(&(**a)[k]), w, what);
    };
    // ---- the collision sweep (game/sphere_sweep.h). Compare the declared mantissa width separately from the
    // production float instantiation. Neither comparison emulates the recorded x87 rounding mode.
    auto at53 = [](const Json& rec) { return precision_of(rec).precision() == FpuPrecision::Double53; };
    c[kTriangleFunction] = [at53](const Json& rec, const Json& in, const Json& out, const std::string& w, Mismatch&) {
        constexpr const char* recorded = "SweptSphereTriangle recorded_precision";
        if (at53(rec)) check_sweep_triangle<double>(in, out, w, recorded);
        else check_sweep_triangle<float>(in, out, w, recorded);
        check_sweep_triangle<float>(in, out, w, "SweptSphereTriangle production_float");
    };
    c["CollisionInfo_GetClampPoint+CCollisionInfoBase::ComputeSkitter"] =
        [at53](const Json& rec, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
            auto run = [&](auto zero) {
                using F = decltype(zero);
                check_stop_and_slide<F>("SweepStopAndSlide", sweep_vec<F>(in.get("start")), sweep_vec<F>(in.get("delta")),
                                        sweep_vec<F>(in.get("direction")), static_cast<F>(f32(in, "fraction")),
                                        flag(in, "hit"), sweep_vec<F>(in.get("normal")), out, w, m);
            };
            if (at53(rec)) run(0.0);
            else run(0.0f);
        };
    c["s3dCollideActorSweep"] = [at53](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                       Mismatch& m) {
        auto run = [&](auto zero) {
            using F = decltype(zero);
            sweep::Vec3<F> from = sweep_vec<F>(in.get("from"));
            sweep::Start<F> s = sweep::start<F>(from, sweep_vec<F>(in.get("to")), static_cast<F>(f32(in, "radius")));
            // What CollideActor left in the info: t 1 without a hit.
            bool hit = flag(in, "hit");
            F t = hit ? static_cast<F>(f32(in, "fraction")) : F(1);
            check_stop_and_slide<F>("s3dCollideActorSweep", from, s.delta, s.direction, t, hit,
                                    sweep_vec<F>(in.get("normal")), out, w, m);
        };
        if (at53(rec)) run(0.0);
        else run(0.0f);
    };
    c["CollisionHit_SetCollidingType"] = [](const Json&, const Json& in, const Json& out, const std::string& w,
                                            Mismatch& m) {
        hailstone::game::HitActor a;
        a.flags = u32(in, "actor_flags");
        a.has_owner = flag(in, "actor_has_owner");
        a.type = i32(in, "actor_type");
        a.is_camera_actor = flag(in, "is_camera_actor");
        a.collisions_enabled = flag(in, "collisions_enabled");
        a.kind = i32(in, "kind");
        a.player_found = flag(in, "player_found");
        a.door_has_switch = flag(in, "door_has_switch");
        // The mocked 0x510210's answer (al) is in the recorded calls.
        bool touch = (call_ret(out, "switch_acts_on_touch") & 0xff) != 0;
        hailstone::game::HitResult r = hailstone::game::collision_hit_set_colliding_type(a, touch);
        m.check_int(r.ret, as_int(out.get("ret")), w, "ret");
        const Json* ct = out.get("colliding_type");
        if (is_null(ct) != !r.colliding_type) m.report(w, "colliding_type", "written or not");
        else if (r.colliding_type) m.check_int(*r.colliding_type, as_int(ct), w, "colliding_type");
        m.check_int(r.touching_switch, as_int(out.get("standing_on_switch")), w, "standing_on_switch");
        std::vector<std::string> got;
        if (r.asked_touch) got.push_back("switch_acts_on_touch");
        if (r.touching_switch) got.push_back("touching_switch");
        const Json* calls = out.get("calls");
        auto* arr = calls ? std::get_if<std::shared_ptr<JsonArray>>(&calls->v) : nullptr;
        std::size_t n = arr ? (*arr)->size() : 0;
        if (n != got.size()) {
            m.report(w, "calls", "count");
            return;
        }
        for (std::size_t k = 0; k < n; ++k) {
            const Json* fn = (**arr)[k].get("fn");
            auto* name = fn ? std::get_if<std::string>(&fn->v) : nullptr;
            if (!name || *name != got[k]) m.report(w, "calls", got[k]);
        }
    };
    auto filter = [](bool with_rider) {
        return [with_rider](const Json&, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
            hailstone::game::FilterActor a;
            a.has_owner = flag(in, "actor_has_owner");
            a.type = i32(in, "actor_type");
            a.spawn_found = flag(in, "spawn_found");
            a.spawn_is_corpse = flag(in, "spawn_is_corpse");
            a.spawn_type = static_cast<std::uint8_t>(u32(in, "spawn_type"));
            a.spawn_vfa8 = flag(in, "spawn_vfa8");
            a.spawn_vf50 = flag(in, "spawn_vf50");
            a.switch_found = flag(in, "switch_found");
            a.switch_type = static_cast<std::uint8_t>(u32(in, "switch_type"));
            a.object9_found = flag(in, "object9_found");
            a.object9_98 = static_cast<std::uint8_t>(u32(in, "object9_98"));
            a.mover_found = flag(in, "mover_found");
            a.spawn_is_movers_rider = flag(in, "spawn_is_movers_rider");
            m.check_int(hailstone::game::collision_filter_movement(a, with_rider), as_int(out.get("ret")), w, "ret");
        };
    };
    c["CollisionFilter_Movement"] = filter(false);
    c["CollisionFilter_MovementWithRider"] = filter(true);
    c["CDisplay::ProcessSwitches"] = [arr3, check3](const Json& rec, const Json& in, const Json& out,
                                                    const std::string& w, Mismatch& m) {
        const Json& si = *in.get("switch");
        Switch sw;
        sw.type = static_cast<std::uint8_t>(u32(si, "type"));
        sw.state = static_cast<std::uint8_t>(u32(si, "state"));
        sw.default_state = static_cast<std::uint8_t>(u32(si, "default_state"));
        sw.hidden = flag(si, "hidden");
        sw.timestamp = 0;
        std::uint32_t now = u32(si, "age_ms");
        sw.custom = i32(si, "custom");
        auto b = arr3(si.get("base"));
        auto p = arr3(si.get("position"));
        for (int k = 0; k < 3; ++k) {
            sw.base[k] = b[static_cast<std::size_t>(k)];
            sw.position[k] = p[static_cast<std::size_t>(k)];
        }
        sw.base_heading = f32(si, "base_heading");
        sw.base_pitch = f32(si, "base_pitch");
        sw.spin = f32(si, "spin");
        sw.spin_accel = f32(si, "spin_accel");
        sw.heading = f32(si, "heading");
        sw.pitch = f32(si, "pitch");
        sw.swing_speed = f32(si, "swing_speed");
        sw.swing_stop = flag(si, "swing_stop");
        sw.particle = flag(si, "has_particle");
        const Json& so = *out.get("switch");
        struct Tape : SwitchQueries {
            const Json& si;
            const Json& so;
            std::int32_t zone;
            std::vector<SwitchSpawn> list;
            std::vector<std::pair<bool, bool>> marks;
            std::vector<std::pair<std::string, std::int64_t>> calls;
            std::optional<std::array<float, 3>> moved, turned;
            Tape(const Json& i, const Json& o, std::int32_t z) : si(i), so(o), zone(z) {}
            void change_state(Switch&, std::uint32_t s) override { calls.push_back({"change_state", s & 0xff}); }
            void repop_switch(Switch&) override { calls.push_back({"repop_switch", 0}); }
            std::uint32_t repop_frequency(const Switch&) override { return u32(si, "repop_frequency"); }
            std::uint8_t slide_switch_left_right(Switch&, std::uint32_t fl, float) override {
                calls.push_back({"slide_switch_left_right", fl});
                return static_cast<std::uint8_t>(u32(si, "slide_switch_ret") & 0xff);
            }
            void update_day_night(Switch&) override { calls.push_back({"update_day_night", 0}); }
            bool local_player() override { return flag(si, "local_player"); }
            bool start_particle(Switch&, std::int32_t id) override {
                calls.push_back({"particle_effect", id});
                return flag(so, "has_particle");
            }
            void stop_particle(Switch&) override {}
            bool is_floor_switch(const Switch& s) override {
                hailstone::game::Door d;
                d.type = s.type;
                return hailstone::game::is_floor_switch(d, zone);
            }
            std::optional<double> actor_radius(const Switch&) override {
                if (!flag(si, "has_actor")) return std::nullopt;
                return as_f64(si.get("actor_radius"));
            }
            void actor_place(Switch&, const float pos[3], const float o[3]) override {
                moved = std::array<float, 3>{pos[0], pos[1], pos[2]};
                turned = std::array<float, 3>{o[0], o[1], o[2]};
            }
            std::span<const SwitchSpawn> spawns() override { return list; }
            void reset_collision_counter(std::size_t i) override { marks[i].first = true; }
            void set_switch_moved(std::size_t i) override { marks[i].second = true; }
        } tape(si, so, i32(in, "zone"));
        if (const Json* sp = in.get("spawns"))
            if (auto* a = std::get_if<std::shared_ptr<JsonArray>>(&sp->v))
                for (const Json& e : **a) {
                    SwitchSpawn s;
                    s.y = f32(e, "y");
                    s.x = f32(e, "x");
                    if (!is_null(e.get("actor_radius"))) s.actor_radius = as_f64(e.get("actor_radius"));
                    tape.list.push_back(s);
                }
        tape.marks.assign(tape.list.size(), {false, false});
        Switch* list[] = {&sw};
        process_switches(list, u32(in, "elapsed_ms"), now, tape, precision_of(rec));
        m.check_int(sw.state, as_int(so.get("state")), w, "state");
        check3(m, {sw.position[0], sw.position[1], sw.position[2]}, so.get("position"), w, "position");
        m.check_float(sw.heading, as_f32(so.get("heading")), w, "heading");
        m.check_float(sw.pitch, as_f32(so.get("pitch")), w, "pitch");
        m.check_float(sw.swing_speed, as_f32(so.get("swing_speed")), w, "swing_speed");
        m.check_int(sw.swing_stop, as_int(so.get("swing_stop")), w, "swing_stop");
        m.check_int(sw.particle, as_int(so.get("has_particle")), w, "has_particle");
        if (is_null(out.get("actor_moved_to")) != !tape.moved) m.report(w, "actor_moved_to", "moved or not");
        else if (tape.moved) {
            check3(m, *tape.moved, out.get("actor_moved_to"), w, "actor_moved_to");
            check3(m, *tape.turned, out.get("actor_turned_to"), w, "actor_turned_to");
        }
        if (const Json* sp = out.get("spawns"))
            if (auto* a = std::get_if<std::shared_ptr<JsonArray>>(&sp->v))
                for (std::size_t k = 0; k < (*a)->size() && k < tape.marks.size(); ++k) {
                    m.check_int(tape.marks[k].first, as_int((**a)[k].get("floor_cache_reset")), w, "floor_cache_reset");
                    m.check_int(tape.marks[k].second, as_int((**a)[k].get("switch_moved")), w, "switch_moved");
                }
        const Json* cl = out.get("calls");
        auto* arr = cl ? std::get_if<std::shared_ptr<JsonArray>>(&cl->v) : nullptr;
        std::size_t n = arr ? (*arr)->size() : 0;
        if (n != tape.calls.size()) {
            m.report(w, "calls", "got " + std::to_string(tape.calls.size()) + ", want " + std::to_string(n));
            return;
        }
        for (std::size_t k = 0; k < n; ++k) {
            const Json& rc = (**arr)[k];
            const Json* fn = rc.get("fn");
            auto* name = fn ? std::get_if<std::string>(&fn->v) : nullptr;
            if (!name || *name != tape.calls[k].first) {
                m.report(w, "calls", "call " + std::to_string(k) + " is " + tape.calls[k].first);
                return;
            }
            const char* key = *name == "change_state" ? "state" : *name == "slide_switch_left_right" ? "flag"
                            : *name == "particle_effect" ? "id" : nullptr;
            if (key) m.check_int(tape.calls[k].second, as_int(rc.get(key)), w, key);
        }
    };
    c["CollsnCallbackForFloor"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w,
                                     Mismatch& m) {
        hailstone::game::FloorHit h;
        h.actor_null = flag(in, "actor_null");
        h.is_camera_actor = flag(in, "is_camera_actor");
        h.player_found = flag(in, "player_found");
        h.actor_is_player = flag(in, "actor_is_player");
        h.has_owner = flag(in, "actor_has_owner");
        h.type = i32(in, "actor_type");
        h.type_again = i32(in, "actor_type_again");
        h.switch_present = flag(in, "switch_present");
        h.switch_type = static_cast<std::uint8_t>(u32(in, "switch_type"));
        h.zone = i32(in, "zone");
        h.player_height = f32(in, "player_height");
        h.distance = f32(in, "distance");
        h.hit_spawn_is_player = flag(in, "hit_spawn_is_player");
        h.hit_spawn_rideable = flag(in, "hit_spawn_rideable");
        hailstone::game::FloorHitResult r = hailstone::game::collsn_callback_for_floor(h, precision_of(rec));
        m.check_int(r.ret, as_int(out.get("ret")), w, "ret");
        m.check_int(r.touching_switch, as_int(out.get("touching_switch_set")), w, "touching_switch_set");
        const Json* cl = out.get("calls");
        auto* arr = cl ? std::get_if<std::shared_ptr<JsonArray>>(&cl->v) : nullptr;
        std::size_t n = arr ? (*arr)->size() : 0;
        m.check_int(r.set_vehicle ? 1 : 0, static_cast<std::int64_t>(n), w, "set_vehicle calls");
    };
    c["FloorActorFilter"] = [](const Json&, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        hailstone::game::FloorFilterActor a;
        a.mover_present = flag(in, "mover_present");
        a.owner_is_mover = flag(in, "owner_is_mover");
        a.has_owner = flag(in, "actor_has_owner");
        a.type = i32(in, "actor_type");
        a.type_again = i32(in, "actor_type_again");
        a.mover_vf24 = flag(in, "mover_vf24");
        a.restriction_mask = static_cast<std::uint8_t>(u32(in, "restriction_mask_low"));
        a.volume_type = i32(in, "volume_type");
        a.volume_radius = as_f64(in.get("volume_radius")).value_or(0.0);
        a.mover_spawn_found = flag(in, "mover_spawn_found");
        a.is_own_actor = flag(in, "is_own_actor");
        a.is_mount_actor = flag(in, "is_mount_actor");
        a.is_rider_actor = flag(in, "is_rider_actor");
        const Json* c0 = call(out, "is_floor_switch");
        bool answer = c0 && as_int(c0->get("ret")).value_or(0) != 0;
        bool asked = false;
        std::uint32_t r = hailstone::game::floor_actor_filter(a, answer, &asked);
        m.check_int(r, as_int(out.get("ret")), w, "ret");
        m.check_int(asked, c0 != nullptr, w, "is_floor_switch asked");
    };
    c["CDisplay::GetPlayerFloorHeight"] = [arr3, check3](const Json& rec, const Json& in, const Json& out,
                                                         const std::string& w, Mismatch& m) {
        FloorCache cache;
        cache.collision_counter = u32(in, "collision_counter");
        cache.location = arr3(in.get("cached_location"));
        cache.floor = f32(in, "cached_floor");
        cache.ceiling = f32(in, "cached_ceiling");
        cache.touching_switch = true;
        FloorWorld world;
        world.controlled = flag(in, "is_controlled_player");
        world.underworld = f32(in, "underworld");
        world.below_allowed = flag(in, "below_allowed");
        struct Tape : FloorQueries {
            const Json& in;
            int vehicle_calls = 0;
            bool swept = false;
            float radius = 0.0f;
            bool normal = false;
            std::array<float, 3> from{}, to{};
            explicit Tape(const Json& i) : in(i) {}
            bool is_3d() override { return flag(in, "is_3d"); }
            bool has_vehicle() override { return flag(in, "has_vehicle"); }
            bool scene_accepts(const std::array<float, 3>&) override { return flag(in, "scene_accepts"); }
            void set_vehicle_none() override { ++vehicle_calls; }
            std::uint32_t sweep(const std::array<float, 3>& a, const std::array<float, 3>& b, float r, bool n,
                                float& hit_z) override {
                swept = true;
                from = a;
                to = b;
                radius = r;
                normal = n;
                hit_z = f32(in, "hit_z");
                return u32(in, "sweep_result");
            }
        } tape(in);
        double v = get_player_floor_height(cache, arr3(in.get("position")), flag(in, "clear_touching"),
                                           f32(in, "avatar_height"), world, tape, precision_of(rec));
        m.check_double(v, as_f64(out.get("value")), w, "value");
        m.check_float(cache.floor, as_f32(out.get("cached_floor")), w, "cached_floor");
        m.check_int(cache.collision_counter, as_int(out.get("collision_counter")), w, "collision_counter");
        check3(m, cache.location, out.get("cached_location"), w, "cached_location");
        m.check_int(!cache.touching_switch, as_int(out.get("touching_cleared")), w, "touching_cleared");
        m.check_int(tape.swept, as_int(out.get("swept")), w, "swept");
        if (tape.swept && flag(out, "swept")) {
            m.check_float(tape.radius, as_f32(out.get("sweep_radius")), w, "sweep_radius");
            m.check_int(tape.normal, as_int(out.get("sweep_gives_actor_normal")), w, "sweep_gives_actor_normal");
            check3(m, tape.from, out.get("sweep_from"), w, "sweep_from");
            check3(m, tape.to, out.get("sweep_to"), w, "sweep_to");
        }
        const Json* cl = out.get("calls");
        auto* arr = cl ? std::get_if<std::shared_ptr<JsonArray>>(&cl->v) : nullptr;
        m.check_int(tape.vehicle_calls, static_cast<std::int64_t>(arr ? (*arr)->size() : 0), w, "set_vehicle calls");
    };
    c["PlayerZoneClient::CanIFitHere"] = [arr3, check3](const Json& rec, const Json& in, const Json& out,
                                                       const std::string& w, Mismatch& m) {
        FloorCache cache;
        cache.floor = f32(in, "cached_floor");
        cache.ceiling_location = arr3(in.get("ceiling_cache_location"));
        cache.ceiling = f32(in, "cached_ceiling");
        cache.collision_counter = u32(in, "collision_counter");
        FitSpawn s;
        s.controlled = flag(in, "is_controlled_player");
        s.terrain_collision = flag(in, "terrain_collision");
        if (flag(in, "has_actor")) s.actor_radius = x87_mul(precision_of(rec), as_f64(in.get("actor_radius")).value_or(0.0), 1.0);
        s.avatar_height = f32(in, "avatar_height");
        s.special_height = f32(in, "special_height");
        s.stand_state = static_cast<std::uint8_t>(u32(in, "stand_state"));
        s.mount_race = flag(in, "is_mount_race");
        s.has_rider = flag(in, "has_rider");
        AutoDuck duck;
        duck.active = static_cast<std::uint8_t>(u32(in, "auto_duck"));
        duck.until = u32(in, "auto_duck_until");
        duck.allowed = flag(in, "auto_duck_allowed");
        // The fakes returned a product of two floats from fmul, so rounded to the record's precision (the
        // recorder writes the exact product).
        Fpu fpu = precision_of(rec);
        struct Tape : FitQueries {
            const Json& in;
            Fpu fpu;
            bool floor_asked = false, ceiling_asked = false, stood = false, wrote = false;
            float ceiling_radius = 0.0f, player_ceiling = 0.0f;
            Tape(const Json& i, Fpu f) : in(i), fpu(f) {}
            double floor(const std::array<float, 3>&) override {
                floor_asked = true;
                return x87_mul(fpu, as_f64(in.get("floor")).value_or(0.0), 1.0);
            }
            double ceiling(const std::array<float, 3>&, float r) override {
                ceiling_asked = true;
                ceiling_radius = r;
                return x87_mul(fpu, as_f64(in.get("ceiling_query")).value_or(0.0), 1.0);
            }
            void stand_up() override { stood = true; }
            void set_player_ceiling(float c) override {
                wrote = true;
                player_ceiling = c;
            }
        } tape(in, fpu);
        bool v = can_i_fit_here(cache, arr3(in.get("position")), s, flag(in, "below_allowed"), duck,
                                u32(in, "display_time"), tape, precision_of(rec));
        m.check_int(v, as_int(out.get("value")), w, "value");
        m.check_int(tape.floor_asked, as_int(out.get("floor_asked")), w, "floor_asked");
        m.check_int(tape.ceiling_asked, as_int(out.get("ceiling_asked")), w, "ceiling_asked");
        m.check_int(tape.stood, as_int(out.get("stood_up")), w, "stood_up");
        m.check_float(cache.floor, as_f32(out.get("cached_floor")), w, "cached_floor");
        check3(m, cache.ceiling_location, out.get("ceiling_cache_location"), w, "ceiling_cache_location");
        // A NaN ceiling the fake returned is recorded with Exact()'s payload, not the one fmul gave: then only
        // NaN-ness is compared.
        auto check_ceiling = [&](float got, const char* key) {
            auto want = as_f32(out.get(key));
            bool fake_nan = tape.ceiling_asked && std::isnan(as_f64(in.get("ceiling_query")).value_or(0.0));
            if (fake_nan && want && std::isnan(*want)) m.check_int(std::isnan(got), 1, w, key);
            else m.check_float(got, want, w, key);
        };
        check_ceiling(cache.ceiling, "cached_ceiling");
        m.check_int(cache.collision_counter, as_int(out.get("collision_counter")), w, "collision_counter");
        if (tape.wrote) check_ceiling(tape.player_ceiling, "player_ceiling");
        m.check_int(duck.active, as_int(out.get("auto_duck")), w, "auto_duck");
        m.check_int(duck.until, as_int(out.get("auto_duck_until")), w, "auto_duck_until");
    };
    c["0x506c90"] = [](const Json& rec, const Json& in, const Json& out, const std::string& w, Mismatch& m) {
        StillSpawn s;
        s.jump_animation = flag(in, "jump_animation");
        s.controlled = flag(in, "is_controlled_player");
        s.levitate = i32(in, "levitate");
        s.auto_duck = static_cast<std::uint8_t>(u32(in, "auto_duck"));
        s.vehicle_race = flag(in, "is_vehicle_race");
        s.heading = f32(in, "heading");
        s.actor_heading = x87_mul(precision_of(rec), as_f64(in.get("actor_heading")).value_or(0.0), 1.0);
        m.check_int(must_collide_still(s), as_int(out.get("value")), w, "value");
    };
    return c;
}

std::optional<fs::path> vectors_dir() { return hailstone::test::vector_dir(); }

} // namespace

TEST_CASE("Movement replay validates explicit FPU metadata and preserves legacy precision") {
    auto metadata = [](const std::string& source, std::string& error) {
        auto record = Parser(source).parse();
        REQUIRE(record.has_value());
        return replay_precision(*record, error);
    };
    std::string error;
    const auto legacy = metadata("{}", error);
    REQUIRE(legacy.has_value());
    CHECK(legacy->precision == FpuPrecision::Single24);
    CHECK_FALSE(legacy->control_word.has_value());
    for (const char* label : {"x87-24", "x87-53"}) {
        auto old = metadata(std::string("{\"precision\":\"") + label + "\"}", error);
        REQUIRE(old.has_value());
        CHECK_FALSE(old->control_word.has_value());
        CHECK(old->precision == (std::string(label) == "x87-53" ? FpuPrecision::Double53 : FpuPrecision::Single24));
    }

    for (unsigned pc : {0u, 2u})
        for (unsigned rc = 0; rc != 4; ++rc) {
            const unsigned word = 0x007f + (pc << 8) + (rc << 10);
            const std::string label = pc == 2 ? "x87-53" : "x87-24";
            auto decoded = metadata("{\"precision\":\"" + label + "\",\"control_word\":" + std::to_string(word) + "}", error);
            REQUIRE(decoded.has_value());
            CHECK(decoded->precision == (pc == 2 ? FpuPrecision::Double53 : FpuPrecision::Single24));
            REQUIRE(decoded->control_word.has_value());
            CHECK(*decoded->control_word == word);
            CHECK(static_cast<unsigned>(decoded->rounding) == rc);
        }
    const auto toward_zero = metadata(R"({"precision":"x87-53","control_word":3711})", error);
    REQUIRE(toward_zero.has_value());
    CHECK(*toward_zero->control_word == 0x0e7f);
    CHECK(toward_zero->rounding == ReplayRounding::TowardZero);

    for (const std::string source : {
             R"({"precision":"unknown"})", R"({"precision":"x87-64"})", R"({"precision":53})",
             R"({"precision":null})", R"({"control_word":3711})",
             R"({"precision":"x87-53","control_word":-1})",
             R"({"precision":"x87-53","control_word":65536})",
             R"({"precision":"x87-53","control_word":3711.5})",
             R"({"precision":"x87-53","control_word":1e999})",
             R"({"precision":"x87-53","control_word":"3711"})",
             R"({"precision":"x87-53","control_word":true})",
             R"({"precision":"x87-53","control_word":null})",
             R"({"precision":"x87-53","control_word":383})",  // reserved PC=01
             R"({"precision":"x87-53","control_word":895})",  // unsupported PC=11
             R"({"precision":"x87-24","control_word":639})",  // PC=53 contradicts label
             R"({"precision":"x87-53","control_word":127})"}) { // PC=24 contradicts label
        CAPTURE(source);
        CHECK_FALSE(metadata(source, error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("Movement replay rejects unsupported rounding for the bit-exact ports") {
    for (unsigned rc = 0; rc != 4; ++rc) {
        const std::string source = "{\"precision\":\"x87-53\",\"control_word\":" +
                                   std::to_string(0x027f + (rc << 10)) + "}";
        auto record = Parser(source).parse();
        REQUIRE(record.has_value());
        std::string error;
        CHECK(replay_precision_for(*record, kTriangleFunction, error).has_value());
        const auto bit_exact = replay_precision_for(*record, "EQ_Character::CalculateWeight", error);
        CHECK(bit_exact.has_value() == (rc == 0));
        if (rc != 0) CHECK_FALSE(error.empty());
    }
}

TEST_CASE("Movement triangle replay compares recorded precision and production float separately") {
    sweep_tallies.clear();
    struct ClearTallies {
        ~ClearTallies() { sweep_tallies.clear(); }
    } clear;
    const auto table = checkers();
    const auto checker = table.find(kTriangleFunction);
    REQUIRE(checker != table.end());
    // Independently authored geometry: a flat right triangle and two radius-one sweeps toward it. The second
    // centre stays outside the triangle; its contact is on the capsule around the edge from (0,0,0) to (10,0,0).
    // These fixtures contain no recorded client geometry. The declared RC is provenance, not emulated arithmetic.
    for (const std::string source : {
             R"({"precision":"x87-53","control_word":3711,
                  "in":{"start":[2,2,-2],"delta":[0,0,4],"inv_length":0.25,"fraction_before":1,"radius":1,
                        "tri_normal":[0,0,-1],"v0":[0,0,0],"v1":[10,0,0],"v2":[0,10,0]},
                  "out":{"hit":true,"fraction":0.25,"normal":[0,0,-1]}})",
             R"({"precision":"x87-53","control_word":3711,
                  "in":{"start":[2,-0.6,-2],"delta":[0,0,4],"inv_length":0.25,"fraction_before":1,"radius":1,
                        "tri_normal":[0,0,-1],"v0":[0,0,0],"v1":[10,0,0],"v2":[0,10,0]},
                  "out":{"hit":true,"fraction":0.3,"normal":[0,-0.6,-0.8]}})"}) {
        const auto record = Parser(source).parse();
        REQUIRE(record.has_value());
        std::string error;
        REQUIRE(replay_precision_for(*record, kTriangleFunction, error).has_value());
        Mismatch mismatches;
        checker->second(*record, *record->get("in"), *record->get("out"), "authored triangle", mismatches);
        CHECK(mismatches.count == 0);
    }
    REQUIRE(sweep_tallies.size() == 2);
    for (const char* label : {"SweptSphereTriangle recorded_precision", "SweptSphereTriangle production_float"}) {
        CAPTURE(label);
        const auto found = sweep_tallies.find(label);
        REQUIRE(found != sweep_tallies.end());
        const auto& tally = found->second;
        CHECK(tally.records == 2);
        CHECK(tally.exact + tally.tolerated == 2);
        CHECK(tally.grazing == 0);
        CHECK(tally.unphysical == 0);
        CHECK(tally.disagree == 0);
    }
}

TEST_CASE("Movement test vectors from the original client") {
    sweep_tallies.clear();
    const bool require_sweep = hailstone::core::env("REQUIRE_SWEEP_VECTORS") != nullptr;
    auto dir = vectors_dir();
    std::error_code ec;
    if (!dir || !fs::is_directory(*dir, ec)) {
        CHECK_MESSAGE(!require_sweep,
                      "HAILSTONE_REQUIRE_SWEEP_VECTORS requires an existing HAILSTONE_VECTORS_DIR");
        MESSAGE("no recorded test vectors (HAILSTONE_VECTORS_DIR); skipped");
        return;
    }
    auto table = checkers();
    int files = 0, triangle_records = 0, no_control_word = 0;
    std::array<int, 4> rounding_counts{};
    for (const auto& entry : fs::directory_iterator(*dir)) {
        if (entry.path().extension() != ".jsonl") continue;
        std::ifstream f(entry.path());
        std::string line;
        int n = 0, checked = 0;
        Mismatch m;
        while (std::getline(f, line)) {
            ++n;
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            hailstone::test::normalise_recorded_names(line);
            auto j = Parser(line).parse();
            std::string where = entry.path().filename().string() + ":" + std::to_string(n);
            if (!j) {
                m.report(where, "record", "not JSON");
                continue;
            }
            const Json* fn = j->get("fn");
            const std::string* name = fn ? std::get_if<std::string>(&fn->v) : nullptr;
            auto it = name ? table.find(*name) : table.end();
            if (it == table.end()) break; // another port's vectors (stats, spells)
            std::string metadata_error;
            auto metadata = replay_precision_for(*j, *name, metadata_error);
            if (!metadata) {
                m.report(where, "FPU metadata", metadata_error);
                continue;
            }
            const Json* in = j->get("in");
            const Json* out = j->get("out");
            if (!in || !out) {
                m.report(where, "record", "no in/out");
                continue;
            }
            if (metadata->control_word) ++rounding_counts[static_cast<unsigned>(metadata->rounding)];
            else ++no_control_word;
            it->second(*j, *in, *out, where, m);
            if (*name == kTriangleFunction) ++triangle_records;
            ++checked;
        }
        if (checked || m.count) {
            if (checked) ++files;
            MESSAGE(entry.path().filename().string() << ": " << checked << " records, " << m.count << " mismatches");
            CHECK(m.count == 0);
        }
    }
    MESSAGE(files << " movement vector files checked from " << dir->string());
    MESSAGE("FPU metadata: " << no_control_word << " records without control_word; recorded RC counts: "
                            << std::string(kReplayRoundingNames[0]) << " " << rounding_counts[0] << ", "
                            << std::string(kReplayRoundingNames[1]) << " " << rounding_counts[1] << ", "
                            << std::string(kReplayRoundingNames[2]) << " " << rounding_counts[2] << ", "
                            << std::string(kReplayRoundingNames[3]) << " " << rounding_counts[3]);
    if (triangle_records)
        MESSAGE("Triangle recorded_precision and production_float comparisons use host arithmetic and the sweep "
                "tolerances; they do not emulate the recorded x87 rounding mode or full FPU state.");
    CHECK_MESSAGE((!require_sweep || triangle_records > 0),
                  "HAILSTONE_REQUIRE_SWEEP_VECTORS requires recognized CCollisionInfoSphere::vf0 records");
    for (const auto& [name, t] : sweep_tallies) {
        if (!t.records) continue;
        MESSAGE(name << ": " << t.records << " records, " << t.exact << " exact, " << t.tolerated
                     << " within the tolerance, " << t.grazing << " near grazing, " << t.disagree << " disagreeing, "
                     << t.unphysical << " disagreeing with garbage inputs (not failures), " << t.contact_assumed
                     << " normals not compared (the original's is undefined); largest differences: t " << t.worst_t
                     << ", unit vectors " << t.worst_unit << ", points " << t.worst_point << ", slide distance "
                     << t.worst_distance);
        CHECK(t.disagree <= kSweepDisagreeShare * t.records);
    }
}
