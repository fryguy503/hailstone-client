// Movement animation choice and playback rate (game/mechanics/anim_select.h).
// Follows the original's PlayerAnimation_SelectMovementAnim @0x5888a0,
// 0x58af10, 0x4afc90 and 0x4afe60. The float steps follow the original's x87 instruction order;
// comments give the addresses.
#include "game/mechanics/anim_select.h"

#include "game/race_models.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace hailstone::game::mechanics {

namespace {

// The exe's float constants, bit for bit.
float constant(std::uint32_t bits) { return std::bit_cast<float>(bits); }
const float kStrideScale = constant(0x3f44ec4f); // 0x9d70ec: 1/1.3
const float kSwimFactor = constant(0x4031c71c);  // 0x9d70e8: 1/0.36
const float kWalkFactor = constant(0x3fb981db);  // 0x9d70e4: 1/0.69
const float kRunFactor = constant(0x3f2bcfe4);   // 0x9d70e0: 1/1.49
const float kNewStyleWalk = constant(0x3fb33333); // 0x9c8350: 1.4
const float kStillLimit = constant(0x3c23d70a);  // 0x9c4be4: 0.01
const float kThreeQuarters = constant(0x3f400000); // 0x9c59f8

// Rounds to 24 significant bits keeping the x87's exponent range (a value
// with a 24-bit significand: the double square root of it rounds once).
double round24_wide(double v) {
    if (!std::isfinite(v) || v == 0) return v;
    int e = 0;
    std::frexp(v, &e);
    double scale = std::ldexp(1.0, 1 - e);
    return static_cast<double>(static_cast<float>(v * scale)) / scale;
}

// The square root at the FPU's precision. The operand always has at most the
// precision's bits (a float or an earlier result), so the double square root
// rounded again to 24 bits is the correctly rounded result.
double x87_sqrt(const Fpu& fpu, double v) {
    if (std::isnan(v)) return v;
    if (v < 0) return -std::numeric_limits<double>::quiet_NaN(); // the x87's indefinite
    double r = std::sqrt(v);
    return fpu.precision() == FpuPrecision::Double53 ? r : round24_wide(r);
}

// Stored as a float.
float store(double v) { return static_cast<float>(v); }

// The loaded value below the operand, ordered (NaN: false).
bool below(double st0, double operand) { return st0 < operand; }

// sqrt((y*y + x*x) + z*z) as 0x588a9a / 0x588c2b form it: loads x, y, z;
// y*y into the y slot, x*x into the x slot, x*x + y*y, z*z, sum + z*z.
double length3(const Fpu& f, float y, float x, float z) {
    double yy = x87_mul(f, X87(y), X87(y));
    double xx = x87_mul(f, X87(x), X87(x));
    double s = x87_add(f, X87(xx), X87(yy));
    double zz = x87_mul(f, X87(z), X87(z));
    return x87_sqrt(f, x87_add(f, X87(s), X87(zz)));
}

// 0x588a26: CMath sqrt (EQGraphicsDX9 CMath::vf4: the square root at the precision) of y*y + x*x
// stored as a float, * 0.5 + 1, stored. (x*x first; y*y into y's slot; the
// sum's destination is y*y.)
float ground_rate(const Fpu& f, float y, float x) {
    double xx = x87_mul(f, X87(x), X87(x));
    double yy = x87_mul(f, X87(y), X87(y));
    float sum = store(x87_add(f, X87(yy), X87(xx)));
    double r = x87_sqrt(f, X87(sum).v);
    r = x87_mul(f, X87(r), X87(0.5f));
    return store(x87_add(f, X87(r), X87(1.0f)));
}

// The stride's unrounded value (0x588a72): scale * animScale, then * 1/1.3 when shrink.
double stride_value(const Fpu& f, const MovementAnimInput& in, bool shrink) {
    double s = x87_mul(f, X87(in.model_scale), X87(in.anim_scale));
    if (shrink) s = x87_mul(f, X87(s), X87(kStrideScale));
    return s;
}

// 0x4afc90's decision: 0 keep, 1 double, 2 * 0.75.
int old_speed_rule(std::int32_t anim, std::int32_t race, std::int32_t gender, bool styled, bool display_129) {
    bool late = false;
    if (race != 0x14a) {
        if (anim == 0x14) return 1;
        if (anim == 0x13) {
            if (race != 8 || gender != 0) return 1;
            late = true;
        }
    }
    if (!late) {
        if (anim == 0x10 && styled) return 1;
        if (race == 0xd8) return 1;
    }
    if (display_129 && (race == 0xd2 || race == 0xd1 || race == 0xd3 || race == 0xd4)) return 1;
    if (styled && gender == 1 && (anim == 2 || anim == 4 || (race == 2 && anim == 5))) return 1;
    if (anim == 0x14 && race == 0x14a) return 2;
    return 0;
}

// The sequence table at 0xaae3d8 (0x28-byte rows for codes 0x4f..0xb6; read
// from the exe): the tag (+0x10 from 0xaae3dc), the fallback sequence (-8),
// the priority (-4), blend in / out (+0 / +4, ms), the layer kind (-0xc,
// 0x58afa0's choice of layers) and the flags +0x19 (0x5885e0: the
// stand after it may be the combat stand) and +0x1a (0x589770: column 5
// may fall back to column 0);
// with the new-to-old map at 0xaaf428 (the old-style number of each code).
struct Sequence {
    std::string_view tag;
    std::uint8_t old, fallback, priority;
    float blend_in, blend_out;
    std::uint8_t layer_kind, flag19, flag1a;
};
constexpr Sequence kSequences[] = {
    {"XXXX", 0x00, 0x4f, 0, 100, 100, 0, 0, 1}, {"SLPR", 0x05, 0x6e, 100, 100, 100, 1, 1, 0},
    {"BLPR", 0x05, 0x50, 100, 100, 100, 1, 1, 0}, {"STPR", 0x02, 0x50, 100, 100, 100, 1, 1, 0},
    {"SLSC", 0x06, 0x50, 100, 100, 100, 1, 1, 0}, {"BLSC", 0x06, 0x53, 100, 100, 100, 1, 1, 0},
    {"STSC", 0x06, 0x53, 100, 100, 100, 1, 1, 0}, {"SL2H", 0x03, 0x50, 100, 100, 100, 1, 1, 0},
    {"BL2H", 0x03, 0x51, 100, 100, 100, 1, 1, 0}, {"ST2H", 0x04, 0x52, 100, 100, 100, 1, 1, 0},
    {"BSTB", 0x02, 0x52, 100, 100, 100, 1, 1, 0}, {"TOSS", 0x05, 0x50, 100, 100, 100, 1, 1, 0},
    {"BASH", 0x07, 0x50, 100, 100, 100, 1, 1, 0}, {"PATK", 0x08, 0x50, 100, 100, 100, 1, 1, 0},
    {"BATK", 0x09, 0x50, 100, 100, 100, 1, 1, 1}, {"WATK", 0x0a, 0x50, 100, 100, 100, 0, 1, 0},
    {"KICK", 0x01, 0x6e, 100, 100, 100, 0, 1, 0}, {"RKCK", 0x0b, 0x5f, 100, 100, 100, 0, 1, 0},
    {"SHTH", 0x00, 0x6e, 100, 100, 100, 1, 0, 0}, {"UNSH", 0x00, 0x6e, 100, 100, 100, 1, 0, 0},
    {"SHTF", 0x00, 0x61, 100, 100, 100, 1, 0, 0}, {"UNSF", 0x00, 0x62, 100, 100, 100, 1, 0, 0},
    {"MKCK", 0x2d, 0x5f, 100, 100, 100, 0, 1, 0}, {"MHA1", 0x2e, 0x50, 100, 100, 100, 0, 1, 0},
    {"MHA2", 0x2f, 0x50, 100, 100, 100, 0, 1, 0}, {"WALK", 0x11, 0x6e, 100, 100, 100, 2, 0, 0},
    {"BWLK", 0x11, 0x68, 100, 100, 100, 2, 0, 0}, {"NRUN", 0x12, 0x68, 100, 100, 100, 2, 0, 0},
    {"TURN", 0x22, 0x6e, 100, 100, 100, 2, 0, 0}, {"LTRN", 0x22, 0x6b, 100, 100, 100, 2, 0, 0},
    {"KNEL", 0x24, 0x6e, 100, 100, 100, 0, 0, 0}, {"STND", 0x20, 0x4f, 25, 250, 100, 0, 0, 0},
    {"IDLE", 0x1a, 0x6e, 50, 250, 100, 0, 0, 0}, {"STDA", 0x20, 0x6e, 25, 100, 100, 0, 1, 0},
    {"IDLA", 0x1a, 0x6f, 50, 100, 100, 0, 0, 0}, {"STDB", 0x20, 0x6e, 25, 100, 100, 0, 0, 0},
    {"IDLB", 0x1a, 0x6f, 50, 100, 100, 0, 0, 0}, {"STDC", 0x20, 0x6e, 25, 100, 100, 0, 0, 0},
    {"IDLC", 0x1a, 0x6f, 50, 100, 100, 0, 0, 0}, {"NSIT", 0x26, 0x6e, 25, 100, 100, 0, 0, 0},
    {"SIDL", 0x49, 0x76, 100, 100, 100, 0, 0, 0}, {"STDG", 0x21, 0x6e, 50, 100, 100, 0, 0, 0},
    {"STNG", 0x21, 0x6e, 50, 100, 100, 0, 0, 0}, {"CRCH", 0x18, 0x6e, 25, 100, 100, 0, 0, 0},
    {"JMPA", 0x13, 0x7c, 100, 100, 100, 0, 0, 0}, {"JMPU", 0x14, 0x6e, 100, 100, 100, 0, 0, 0},
    {"CWLK", 0x16, 0x68, 100, 100, 100, 0, 0, 0}, {"CLMB", 0x17, 0x68, 100, 100, 100, 0, 0, 0},
    {"TWTR", 0x19, 0x80, 25, 100, 100, 0, 0, 0}, {"SWIM", 0x25, 0x68, 100, 100, 100, 0, 0, 0},
    {"STUN", 0x00, 0x6e, 100, 100, 100, 0, 0, 0}, {"DRUM", 0x27, 0x6e, 100, 100, 100, 1, 0, 1},
    {"LUTE", 0x28, 0x6e, 100, 100, 100, 1, 0, 1}, {"HORN", 0x29, 0x6e, 100, 100, 100, 1, 0, 1},
    {"GCST", 0x2b, 0x6e, 100, 100, 100, 1, 0, 1}, {"DCST", 0x2a, 0x85, 100, 100, 100, 1, 0, 1},
    {"MCST", 0x2c, 0x85, 100, 100, 100, 1, 0, 1}, {"GAPO", 0x2a, 0x6e, 100, 100, 100, 1, 0, 1},
    {"OFSM", 0x2b, 0x85, 100, 100, 100, 1, 0, 1}, {"OFLG", 0x2b, 0x85, 100, 100, 100, 1, 0, 1},
    {"HESM", 0x2a, 0x85, 100, 100, 100, 1, 0, 1}, {"HELG", 0x2a, 0x85, 100, 100, 100, 1, 0, 1},
    {"OFAE", 0x2c, 0x85, 100, 100, 100, 1, 0, 1}, {"OFPB", 0x2c, 0x85, 100, 100, 100, 1, 0, 1},
    {"FLCH", 0x0c, 0x6e, 100, 100, 100, 1, 0, 1}, {"MSHT", 0x0d, 0x8f, 100, 100, 100, 1, 0, 1},
    {"SPAS", 0x0f, 0x8f, 100, 100, 100, 0, 0, 1}, {"CRMP", 0x10, 0x8f, 255, 100, 100, 0, 0, 1},
    {"DODG", 0x0c, 0x8f, 100, 100, 100, 1, 0, 1}, {"PRRY", 0x0c, 0x8f, 100, 100, 100, 1, 0, 1},
    {"RPST", 0x0c, 0x8f, 100, 100, 100, 1, 0, 1}, {"FLDM", 0x0e, 0x8f, 100, 100, 100, 0, 0, 1},
    {"FALL", 0x15, 0x8f, 100, 100, 100, 0, 0, 1}, {"NBOW", 0x46, 0x6e, 50, 100, 100, 0, 0, 1},
    {"SLTE", 0x43, 0x6e, 50, 100, 100, 0, 0, 1}, {"WAVE", 0x1d, 0x6e, 50, 100, 100, 0, 0, 1},
    {"HNOD", 0x30, 0x6e, 50, 100, 100, 0, 0, 1}, {"CLAP", 0x33, 0x6e, 50, 100, 100, 0, 0, 1},
    {"DOVR", 0x34, 0x6e, 50, 100, 100, 0, 0, 1}, {"NPNT", 0x40, 0x6e, 50, 100, 100, 0, 0, 1},
    {"LAGH", 0x36, 0x6e, 50, 100, 100, 0, 0, 1}, {"SHRG", 0x41, 0x6e, 50, 100, 100, 0, 0, 1},
    {"TRIU", 0x1b, 0x6e, 50, 100, 100, 0, 0, 1}, {"AGNY", 0x1c, 0x6e, 50, 100, 100, 0, 0, 1},
    {"NGTV", 0x1e, 0x6e, 50, 100, 100, 0, 0, 1}, {"BORD", 0x1f, 0x6e, 50, 100, 100, 0, 0, 1},
    {"PRAY", 0x32, 0x6e, 50, 100, 100, 0, 0, 1}, {"BLSH", 0x41, 0x6e, 50, 100, 100, 0, 0, 1},
    {"COGH", 0x37, 0x6e, 50, 100, 100, 0, 0, 1}, {"CRNG", 0x38, 0x6e, 50, 100, 100, 0, 0, 1},
    {"DNCE", 0x3a, 0x6e, 50, 100, 100, 0, 0, 1}, {"HSHK", 0x3b, 0x6e, 50, 100, 100, 0, 0, 1},
    {"STRE", 0x3d, 0x6e, 50, 100, 100, 0, 0, 1}, {"SHVR", 0x44, 0x6e, 50, 100, 100, 0, 0, 1},
    {"HLGH", 0x3f, 0x6e, 50, 100, 100, 0, 0, 1}, {"IMPT", 0x45, 0x6e, 50, 100, 100, 0, 0, 1},
    {"SKNL", 0x3e, 0x6e, 50, 100, 100, 0, 0, 1}, {"CATK", 0x09, 0x5d, 100, 100, 100, 1, 1, 0},
    {"HIPS", 0x3c, 0x6e, 50, 100, 100, 0, 0, 0}, {"RAIS", 0x42, 0x6e, 50, 100, 100, 0, 0, 0},
    {"SMLE", 0x4d, 0x6e, 50, 100, 100, 0, 0, 0}, {"TILT", 0x39, 0x6e, 50, 100, 100, 0, 0, 0},
    {"KBEG", 0x3e, 0x6e, 50, 100, 100, 0, 0, 0}, {"SATK", 0x06, 0x50, 100, 100, 100, 1, 1, 0},
};
static_assert(std::size(kSequences) == 0x68);
const Sequence* sequence(std::uint8_t code) {
    return code >= 0x4f && code <= 0xb6 ? &kSequences[code - 0x4f] : nullptr;
}

// The old-to-new map at 0xaaf5c8 (the new-style object's slot 0x14 for an
// old-style number).
constexpr std::uint8_t kOldToNew[0x4f] = {
    0x4f, 0x5f, 0x52, 0x56, 0x58, 0x50, 0x53, 0x5b, 0x5c, 0x5d, 0x5e, 0x60, 0x8f, 0x90, 0x96, 0x91,
    0x92, 0x68, 0x6a, 0x7b, 0x7c, 0x97, 0x7d, 0x7e, 0x7a, 0x7f, 0x6f, 0xa1, 0xa2, 0x9a, 0xa3, 0xa4,
    0x6e, 0x79, 0x6b, 0x4f, 0x6d, 0x80, 0x76, 0x82, 0x83, 0x84, 0x86, 0x85, 0x87, 0x65, 0x66, 0x67,
    0x9b, 0x4f, 0xa5, 0x9c, 0x9d, 0xa6, 0x9f, 0xa7, 0xa8, 0xb4, 0xa9, 0xaa, 0xb1, 0xab, 0xaf, 0xad,
    0x9e, 0xa0, 0xb2, 0x99, 0xac, 0xae, 0x98, 0x6e, 0x6f, 0x77, 0x6e, 0x4f, 0x4f, 0xb3, 0x6a,
};

// The old-style names at 0xb787a0 (0x14 bytes each), as CreateActorDefinition fills them.
constexpr std::string_view kOldNames[0x4f] = {
    "", "C01", "C02", "C03", "C04", "C05", "C06", "C07", "C08", "C09", "C10", "C11", "D01",
    "D02", "D03", "D04", "D05", "L01", "L02", "L03", "L04", "L05", "L06", "L07", "L08", "L09",
    "O01", "S01", "S02", "S03", "S04", "S05", "P01", "P02", "P03", "P04", "P05", "P06", "P07",
    "T01", "T02", "T03", "T04", "T05", "T06", "T07", "T08", "T09", "S06", "S07", "S08", "S09",
    "S10", "S11", "S12", "S13", "S14", "S15", "S16", "S17", "S18", "S19", "S20", "S21", "S22",
    "S23", "S24", "S25", "S26", "S27", "S28", "P08", "O02", "O03", "P09", "L11", "L12", "S29",
    "L10",
};

} // namespace

std::string_view new_style_tag(std::uint8_t code) {
    const Sequence* s = sequence(code);
    return s && code != 0x4f ? s->tag : std::string_view{};
}

std::uint8_t new_to_old_anim(std::uint8_t code) {
    const Sequence* s = sequence(code);
    return s ? s->old : 0;
}

std::uint8_t old_to_new_anim(std::uint8_t old) {
    return old < static_cast<std::uint8_t>(AnimationType::End) ? kOldToNew[old] : old;
}

std::uint8_t new_style_fallback(std::uint8_t code) {
    const Sequence* s = sequence(code);
    return s ? s->fallback : 0x4f;
}

bool new_style_attack_flag(std::uint8_t code) {
    const Sequence* s = sequence(code);
    return s && s->flag19 != 0;
}

bool new_style_mount_fallback_flag(std::uint8_t code) {
    const Sequence* s = sequence(code);
    return s && s->flag1a != 0;
}

std::uint8_t new_style_layer_kind(std::uint8_t code) {
    const Sequence* s = sequence(code);
    return s ? s->layer_kind : 0;
}

std::int32_t new_style_priority(std::uint8_t code) {
    const Sequence* s = sequence(code);
    return s ? s->priority : 0;
}

float new_style_blend_ms(std::uint8_t from, std::uint8_t to) {
    const Sequence* a = sequence(from);
    const Sequence* b = sequence(to);
    return ((b ? b->blend_in : 100.0f) + (a ? a->blend_out : 100.0f)) / 2.0f;
}

bool is_jump_animation(std::uint8_t anim, bool new_style) {
    if (new_style) return anim == kAnimJumpRun || anim == kAnimJumpStand;
    return anim == static_cast<std::uint8_t>(AnimationType::JumpRun) ||
           anim == static_cast<std::uint8_t>(AnimationType::JumpStand);
}

std::string_view old_anim_clip(std::uint8_t old) {
    return old > 0 && old < static_cast<std::uint8_t>(AnimationType::End) ? kOldNames[old] : std::string_view{};
}

bool movement_still(float y, float x, float z, const Fpu& f) {
    // 0x58af17: loads x, y, z; y*y, x*x, x*x + y*y, z*z, sum + z*z; compared with 0.01.
    double yy = x87_mul(f, X87(y), X87(y));
    double xx = x87_mul(f, X87(x), X87(x));
    double s = x87_add(f, X87(xx), X87(yy));
    double zz = x87_mul(f, X87(z), X87(z));
    return below(x87_add(f, X87(s), X87(zz)), kStillLimit);
}

bool new_style_can_idle(std::int32_t a) { return a == 0x6e || a == 0x70 || a == 0x76; }

IdleAnimResult choose_idle_animation(const IdleAnimInput& in) {
    // The original's order; the start is the low byte of 0xa1e344 (0xfe).
    IdleAnimResult res;
    std::uint8_t anim = kNoAnimation;
    float speed = 1.0f;
    bool replay = false, play_once = true, reverse = false;
    constexpr std::uint32_t kStunned = 0x20; // 0x9d77b0
    if (in.state_1198 == 1) {
        play_once = false;
        anim = kAnimStand;
    } else if (in.player_state & kStunned) {
        play_once = false;
        anim = 0x81; // STUN
    } else {
        const std::uint32_t time = in.display_time, last = in.still_since;
        bool not_standing = false;
        if (time == last) {
            if (in.stand_state == 0x6f) {
                speed = 2.0f;
                anim = 0x7a; // CRCH
                replay = true;
            } else if (in.body_wet >= 5) {
                not_standing = true;
            } else if (in.animation == 0x80 || in.animation == 0x7f) {
                not_standing = true;
            } else {
                anim = kAnimStand;
                replay = true;
                play_once = false;
            }
        } else if (in.body_wet >= 5) {
            not_standing = true;
        } else if (in.turning) {
            const float v = in.speed_heading;
            if (0.0f < v) {
                replay = true;
                if (in.stand_state != 0x6f) {
                    anim = 0x6c; // LTRN
                } else {
                    anim = kAnimDuckWalk;
                    reverse = true;
                }
            } else if (0.0f > v) {
                replay = true;
                anim = in.stand_state == 0x6f ? kAnimDuckWalk : 0x6b; // TURN
            }
        } else if (time - last > 5000 && in.stand_state != 0x6f) {
            const std::int32_t roll = in.random_percent ? in.random_percent() : 1;
            res.set_still_since = true; // +0x360 = the display time
            if (new_style_can_idle(in.animation) && !in.attacking) {
                const std::int32_t threshold = in.owner_38c != 0 ? 0x63 : 0x4b;
                if (roll <= threshold || in.byte_1229 != 0) {
                    anim = kAnimStand;
                    play_once = false;
                } else {
                    anim = kAnimIdle;
                }
            }
        } else if (in.stand_state == 0x6f) {
            if (in.animation != 0x7a) {
                speed = 2.0f;
                anim = 0x7a;
                replay = true;
            }
        } else if (in.animation != 0x80 && in.animation != 0x7f) {
            anim = kAnimStand;
            play_once = false;
        }
        if (not_standing) {
            anim = kAnimTread;
            play_once = false;
        }
    }
    if (anim == kNoAnimation) return res; // equals 0xa1e344
    if (in.stand_state == 0x6e) {
        if (anim == kAnimStand || anim == kAnimIdle) {
            anim = anim == kAnimStand ? kAnimSit : kAnimSitIdle;
            if (in.local_first_person) return res;
        } else if (anim >= 0x82 && anim <= 0x84) {
            return res;
        }
    }
    res.start = {true, anim, play_once, reverse, speed, replay};
    return res;
}

MovementAnimResult select_movement_anim(const MovementAnimInput& in, const Fpu& f) {
    MovementAnimResult res;
    AnimStart& out = res.start;
    std::uint8_t anim = kNoAnimation;
    float speed = 1.0f;
    bool play_once = true, reverse = false, replay = false;
    auto play = [&] {
        out.played = anim != kNoAnimation;
        if (!out.played) return;
        out.anim = anim;
        out.play_once = play_once;
        out.reverse = reverse;
        out.speed = speed;
        out.replay = replay;
    };

    if (in.colliding_type == 6) { // climbing
        if (0.0f < in.speed_z) {
            anim = kAnimClimb;
            replay = true;
        } else if (0.0f > in.speed_z) {
            anim = kAnimClimb;
            reverse = true;
            replay = true;
        }
        play();
        return res;
    }
    const float y = in.speed_y, x = in.speed_x;
    if (0.001f > y && -0.001f < y && 0.001f > x && -0.001f < x) { // horizontally still
        if (in.body_wet >= 5) {
            anim = kAnimTread;
        } else if (in.feet_wet < 5) {
            anim = kAnimStand;
            play_once = false;
            replay = true;
            if (static_cast<double>(in.jump_strength) * 0.5 <= in.speed_z) {
                anim = kAnimJumpStand;
            } else if (-2.25f > in.speed_z) {
                anim = kAnimFall;
            }
        }
        play();
        return res;
    }
    if (in.local_player) {
        res.asked_spell_book = true;
        res.spell_book_slot_0x94 = in.spell_book_active;
    }
    speed = ground_rate(f, y, x);
    play_once = false;
    replay = true;
    reverse = false;
    const bool shrink = !in.old_model_scale;
    const float stride = store(stride_value(f, in, shrink));
    if (in.body_wet >= 5) { // swimming (0x588a9a)
        double rate = x87_div(f, X87(x87_mul(f, X87(length3(f, y, x, in.speed_z)), X87(kSwimFactor))),
                              X87(stride_value(f, in, shrink)));
        speed = 2.0 < rate ? 2.0f : store(rate);
        anim = kAnimSwim;
        play();
        return res;
    }
    // Whether the run threshold is below the forward speed.
    enum class Threshold { Walk, MountMean, NewStyleNpc };
    auto threshold_below = [&](Threshold kind) {
        double t = in.walk_speed;
        if (kind == Threshold::MountMean) {
            t = x87_sub(f, X87(in.run_speed), X87(in.walk_speed));
            t = x87_mul(f, X87(t), X87(0.5f));
            t = x87_add(f, X87(t), X87(in.walk_speed));
        } else if (kind == Threshold::NewStyleNpc) {
            t = x87_mul(f, X87(in.walk_speed), X87(kNewStyleWalk));
        }
        return below(t, in.speed_run);
    };
    if (in.feet_wet >= 5) { // wading
        if (in.animation != kAnimTread && in.animation != kAnimSwim) {
            res.asked_mount_race = true;
            anim = threshold_below(in.mount_race ? Threshold::MountMean : Threshold::Walk) ? kAnimRun : kAnimWalk;
        }
    } else {
        res.asked_mount_race = true;
        Threshold kind = Threshold::Walk;
        if (in.mount_race) {
            kind = Threshold::MountMean;
        } else if (in.spawn_type == 1) {
            res.asked_new_style_race = true;
            if (in.new_style_race) kind = Threshold::NewStyleNpc;
        }
        anim = in.stand_state == 0x6f ? kAnimDuckWalk : kAnimWalk;
        const float z = in.speed_z;
        if (in.jump_strength <= z) {
            anim = kAnimJumpRun;
            play_once = true;
            speed = 1.0f;
        } else if (-3.2f > z) {
            anim = kAnimFall;
            replay = true;
        } else if (threshold_below(kind)) {
            anim = kAnimRun;
        } else if (!(z == 0.0f)) {
            replay = false;
        }
    }
    if (0.0f > in.speed_run) { // backward
        if (anim == kAnimWalk) {
            anim = kAnimBackWalk;
        } else if (in.animation != kAnimSwim && in.animation != kAnimTread) {
            reverse = true;
        }
    }
    auto stride_rate = [&](float factor) { // 0x588c2b / 0x588c86
        double r = x87_mul(f, X87(length3(f, y, x, in.speed_z)), X87(factor));
        return store(x87_div(f, X87(r), X87(stride)));
    };
    if (anim == kAnimWalk || anim == kAnimBackWalk) speed = stride_rate(kWalkFactor);
    else if (anim == kAnimRun) speed = stride_rate(kRunFactor);
    play();
    return res;
}

OldStyleAnimResult select_old_style_anim(const OldStyleAnimInput& in, const Fpu& f) {
    OldStyleAnimResult res;
    auto animate = [&](AnimationType anim, bool a, bool b, float speed, bool c) {
        res.start = {true, static_cast<std::uint8_t>(anim), a, b, speed, c};
    };
    auto race_is = [&](std::int32_t r) { return in.race == r; };
    const bool still = movement_still(in.speed_y, in.speed_x, in.speed_z, f) && !in.has_mount;
    if (in.has_mount && !(0.001f > in.mount_speed_y && -0.001f < in.mount_speed_y && 0.001f > in.mount_speed_x &&
                          -0.001f < in.mount_speed_x)) {
        if (in.local_player && in.spell_book_active) res.spell_book_slot_0x94 = true;
    }
    if (still) {
        if (in.state_1198 == 1) {
            animate(AnimationType::Stand, false, false, 1.0f, false);
            return res;
        }
        res.idle_sounds = true;
        if (in.display_time == in.still_since) {
            if (in.stand_state == 0x6f) {
                animate(AnimationType::Duck, true, false, 10000.0f, true);
            } else if (in.body_wet < 5 && in.animation != 0x25 && in.animation != 0x19) {
                animate(AnimationType::Stand, false, false, 1.0f, true);
            } else {
                animate(AnimationType::Tread, false, false, 1.0f, false);
            }
            return res;
        }
        if (in.body_wet >= 5 && !race_is(0x18) && !race_is(0x3d) && !race_is(0x4a)) {
            animate(AnimationType::Tread, false, false, 1.0f, false);
            return res;
        }
        if (in.flag) {
            if (race_is(0x31)) {
                animate(AnimationType::JumpRun, true, false, 1.5f, true);
            } else if (in.state_1198 == 1) {
                animate(AnimationType::Stand, false, false, 1.0f, false);
            } else if (0.0f < in.speed_heading) {
                animate(in.stand_state == 0x6f ? AnimationType::DuckWalk : AnimationType::Turn, true, true, 1.0f, true);
            } else if (0.0f > in.speed_heading) {
                animate(in.stand_state == 0x6f ? AnimationType::DuckWalk : AnimationType::Turn, true, false, 1.0f,
                        true);
            }
            return res;
        }
        if (in.display_time - in.still_since > 5000) {
            if (in.stand_state == 0x6f) return res;
            std::int32_t roll = in.random_percent ? in.random_percent() : 100;
            res.set_still_since = true;
            res.still_since = in.display_time;
            if (!(in.can_idle && in.can_idle(in.animation)) || in.attacking) return res;
            std::int32_t chance = in.owner_38c != 0 ? 0x63 : 0x4b;
            if (roll <= chance || in.byte_1229 != 0) {
                animate(AnimationType::Stand, false, false, 1.0f, false);
            } else if (in.spawn_type == 1 && (race_is(0x31) || race_is(0x9e) || race_is(0xc4))) {
                animate(AnimationType::Idle, true, false, 1.5f, false);
            } else {
                animate(AnimationType::Idle, true, false, 1.0f, false);
            }
            return res;
        }
        if (in.stand_state == 0x6f) return res;
        if (in.animation == 0x25 || in.animation == 0x19) return res;
        animate(AnimationType::Stand, false, false, 1.0f, in.animation == 0x11 || in.animation == 0x12);
        return res;
    }

    res.set_still_since = true;
    res.still_since = 0;
    if (in.byte_163 != 0) {
        animate(AnimationType::Fall, false, false, 1.0f, true);
        return res;
    }
    bool a = true, b = false, c = false;
    if (in.state_1198 == 1) {
        AnimationType anim;
        float speed = 1.0f;
        if (in.zone_id == 0x54 && -4800.0f < in.y) {
            anim = AnimationType::Stand;
        } else {
            anim = AnimationType::Run;
            a = false;
            if (in.animation != 0x11 && !(in.animation == 0x12 && !(0.0f >= in.speed_run))) {
                if (in.animation == 0x12) {
                    b = true;
                    speed = 0.5f;
                }
                a = true;
                anim = AnimationType::Walk;
            }
        }
        animate(anim, a, b, speed, false);
        return res;
    }
    if (in.colliding_type == 6) {
        if (0.0f < in.speed_z) animate(AnimationType::Climb, true, false, 1.0f, true);
        else if (0.0f > in.speed_z) animate(AnimationType::Climb, true, true, 1.0f, true);
        res.idle_sounds = true;
        return res;
    }
    const float y = in.speed_y, x = in.speed_x;
    if (0.001f > y && -0.001f < y && 0.001f > x && -0.001f < x) {
        res.idle_sounds = true;
        if (in.body_wet >= 5) {
            animate(race_is(0x31) && in.flag ? AnimationType::JumpRun : AnimationType::Tread, true, false, 1.0f, false);
            return res;
        }
        if (in.feet_wet >= 5) return res;
        AnimationType anim;
        const float fall = in.speed_z;
        if (static_cast<double>(in.jump_strength) * 0.5 <= fall) {
            anim = AnimationType::JumpStand;
            c = true;
        } else if (-2.25f > fall && in.dword_180 == 0) {
            if (in.stand_state == 0x6e) {
                animate(AnimationType::Stand, false, false, 1.0f, true);
                return res;
            }
            anim = AnimationType::Fall;
            a = false;
            c = true;
        } else if (race_is(0x31) && in.flag) {
            anim = AnimationType::JumpRun;
        } else {
            animate(AnimationType::Stand, false, false, 1.0f, true);
            return res;
        }
        animate(anim, a, false, 1.0f, c);
        return res;
    }

    // 0x4b05d7: the ground rate (x*x first, y*y into its slot, x*x + y*y).
    float speed;
    {
        double xx = x87_mul(f, X87(x), X87(x));
        double yy = x87_mul(f, X87(y), X87(y));
        float sum = store(x87_add(f, X87(yy), X87(xx)));
        double r = x87_mul(f, X87(x87_sqrt(f, X87(sum).v)), X87(0.5f));
        speed = store(x87_add(f, X87(r), X87(1.0f)));
    }
    a = false;
    c = true;
    b = false;
    res.idle_sounds = true;
    if (in.body_wet >= 5) {
        animate(race_is(0x31) && in.flag ? AnimationType::JumpRun : AnimationType::Swimming, false, false, speed, true);
        return res;
    }
    AnimationType anim = AnimationType::None;
    if (in.feet_wet < 5) {
        anim = in.stand_state == 0x6f ? AnimationType::DuckWalk : AnimationType::Walk;
        const float fall = in.speed_z;
        if (in.jump_strength <= fall) {
            speed = 1.0f;
            anim = AnimationType::JumpRun;
            a = true;
        } else if (-3.2f > fall) {
            anim = AnimationType::Fall;
            c = true;
        } else if (below(in.walk_speed, in.speed_run)) {
            if (in.stand_state != 0x6f) anim = AnimationType::Run;
        } else if (!(in.speed_z == 0.0f)) {
            c = false;
        }
    } else if (in.animation != 0x19 && in.animation != 0x25) {
        anim = below(in.walk_speed, in.speed_run) ? AnimationType::Run : AnimationType::Walk;
    }
    if (0.0f > in.speed_run && in.animation != 0x25 && in.animation != 0x19) b = true;
    if (anim != AnimationType::None) animate(anim, a, b, speed, c);
    return res;
}

double old_model_anim_speed(std::int32_t anim, float speed, std::int32_t race, std::int32_t gender, bool new_model,
                            bool display_129, const Fpu& f) {
    switch (old_speed_rule(anim, race, gender, new_model, display_129)) {
    case 1: return x87_add(f, X87(speed), X87(speed)); // speed + speed
    case 2: return x87_mul(f, X87(speed), X87(kThreeQuarters));
    default: return static_cast<double>(speed);
    }
}

float old_action_speed(std::uint8_t anim, float speed, std::int32_t race, std::int32_t gender, bool new_model) {
    // The exe's constants: 0x9c73e0 0.4, 0x9c4998 0.5, 0x9ca690 0.65, 0x9c59f8 0.75.
    constexpr float kMountRaceRun = 0.4f;
    constexpr float kHalf = 0.5f;
    constexpr float kRace330 = 0.65f;
    constexpr float kSocial = 0.75f;
    constexpr std::int32_t kRace330Id = 0x14a;
    constexpr std::int32_t kRace42 = 0x2a, kRace120 = 0x78;
    constexpr std::int32_t kGenderNeutral = 2;
    constexpr std::uint8_t kWalk = 0x11, kRun = 0x12;
    if (!new_model || race == kRace330Id || anim == kWalk || anim == kRun) {
        // 0x8cf970's list is is_mount_race's: their run plays at 0.4, their other animations at 0.5.
        if (game::is_mount_race(race)) return anim == kRun ? speed * kMountRaceRun : kHalf;
        if ((race == kRace42 || race == kRace120) && gender == kGenderNeutral) {
            if (anim != kWalk) return kHalf;
        } else if (race == kRace330Id) {
            return speed * kRace330;
        }
        return speed;
    }
    if (speed == 1.0f) {
        // 0x4b07e0 (the stand / idle) or 0x4b0840 (the idle and O02 / O03): half; a social (an S clip): 0.75.
        auto is = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
        if (anim == is(AnimationType::Stand) || anim == is(AnimationType::Sit) ||
            anim == is(AnimationType::StandAfter) || anim == is(AnimationType::Rider_74) ||
            anim == is(AnimationType::Idle) || anim == is(AnimationType::IdleAfter) ||
            anim == is(AnimationType::SitIdle))
            return kHalf;
        if (old_anim_clip(anim).starts_with('S')) return kSocial;
    }
    return speed;
}

} // namespace hailstone::game::mechanics

namespace hailstone::game::mechanics {

namespace {

// 0x59f750: the old-style models' choice (read from the disassembly; its jump table at 0x59f854 /
// 0x59f87c).
std::uint8_t old_attack_animation(const AttackAnimInput& in) {
    std::uint32_t a;
    bool check_slot = true;
    if (in.item_type == 9) {
        a = 0x2c;
    } else if (in.range != 0) {
        a = in.item_type == 0x1b ? 9 : in.item_type == 0x1c ? 10 : 5;
    } else {
        switch (in.skill) {
        case 0: case 1: case 0x4a: a = 5; break;
        case 2: case 3: a = 3; break;
        case 0xa: a = 7; break;
        case 0x15: case 0x34: a = 0x2e; break;
        case 0x17: a = 0x2f; break;
        case 0x1a: a = 0x2d; break;
        case 0x1c: a = 8; break;
        case 0x1e: case 0x47: a = 1; break;
        case 0x26: a = 0xb; break;
        default: a = in.item_type == 0x23 ? 4 : 2; break;
        }
    }
    // 0x59f81f (every path with skill 0x47 reaches it): a monk's intimidation is a round kick, and skips the
    // secondary hand's swing.
    if (in.skill == 0x47 && in.class_id == 7) {
        a = 0xb;
        check_slot = false;
    }
    if (check_slot && in.slot == 0xe) a = 6;
    if (in.race == 0x80) {
        if (a == 7) return 6;
        if (a == 0x2e && in.skill == 0x15) return 7;
    }
    return static_cast<std::uint8_t>(a);
}

// 0x59f8d0: the new-style models' choice (its jump table at 0x59fa40 / 0x59fa6c).
std::uint8_t new_attack_animation(const AttackAnimInput& in) {
    const bool primary = in.slot == 0xd, secondary = in.slot == 0xe;
    std::uint32_t a = secondary ? 0x55 : 0x52; // STSC / STPR
    enum { kWeapon, kSlotCheck, kRace } next = kWeapon;
    if (in.item_type == 9) {
        a = 0x87; // MCST
    } else if (in.range != 0) {
        if (in.item_type == 0x1b) a = 0x5d;      // BATK
        else if (in.item_type == 0x1c) a = 0x5e; // WATK
        else if (in.body_wet == 0) a = 0x51;     // BLPR
        else return 0x5e;
    } else if (in.body_wet != 0) {
        return 0x5e;
    } else if (in.skill > 0x47) {
        if (in.item_type == 0x23) a = 0x58; // ST2H
    } else {
        switch (in.skill) {
        case 0: // 1H blunt
            if (primary) { a = 0x51; next = kRace; } else { a = 0x54; next = kSlotCheck; }
            break;
        case 1: // 1H slashing
            if (primary) { a = 0x50; next = kRace; } else { a = 0x53; next = kSlotCheck; }
            break;
        case 2: case 3: a = 0x56; break;               // SL2H
        case 0xa: a = 0x5b; next = kSlotCheck; break;  // BASH
        case 0x15: case 0x34: a = 0x66; break;         // MHA1
        case 0x17: a = 0x67; next = kSlotCheck; break; // MHA2
        case 0x1a: a = 0x65; next = kSlotCheck; break; // MKCK
        case 0x1c:                                     // PATK / SATK
            if (primary) { a = 0x5c; next = kRace; } else { a = 0xb6; next = kSlotCheck; }
            break;
        case 0x1e: case 0x47: a = 0x5f; break;         // KICK
        case 0x26: a = 0x60; next = kSlotCheck; break; // RKCK
        default:
            if (in.item_type == 0x23) a = 0x58;
            break;
        }
    }
    if (next == kWeapon) { // 0x59fa0b
        if (in.skill == 0x47 && in.class_id == 7) {
            a = 0x60;
            next = kRace;
        } else {
            next = kSlotCheck;
        }
    }
    if (next == kSlotCheck && secondary) a = 0x53; // 0x59f945
    if (in.race == 0x80) {                          // 0x59f94f
        if (a == 0x5b) return 0x53;
        if (a == 0x66 && in.skill == 0x15) return 0x5b;
    }
    return static_cast<std::uint8_t>(a);
}

} // namespace

std::uint8_t attack_animation(const AttackAnimInput& in) {
    // PlayerZoneClient_PlayAttackAnimation @0x5a19d0: by NpcManager_IsNewStyleModelRace(race).
    return in.new_style ? new_attack_animation(in) : old_attack_animation(in);
}

} // namespace hailstone::game::mechanics
