#pragma once

// Nearest-even x87 arithmetic at 24- or 53-bit precision, reproduced on any CPU.
//
// eqgame.exe computes its float formulas on the x87 FPU. Every arithmetic
// result is rounded to the precision in the FPU control word; loads (of an
// integer, a float or a double) are exact. EQGraphicsDX9.dll creates its
// Direct3D 9 device without D3DCREATE_FPU_PRESERVE (CreateDevice behaviour
// flags 0x24 / 0x84 / 0x44 | 4 at EQGraphicsDX9.dll 0x10098f80..0x10098fbd).
// That motivated the 24-bit nearest-even default here, but does not establish
// the control word later in gameplay: 2026-11-12 Citadel-loaded sessions
// recorded 0x0e7f (53-bit, toward zero) at native collision call boundaries.
// The source of that mode is not established. This helper implements nearest
// even only; it does not emulate the capture's directed rounding modes.
//
// At nearest even, Fpu reproduces both precisions. Its operands are exact (integers, float
// or double constants, or earlier results), so:
//  - 53-bit: one IEEE double operation is exactly the x87 result (same rounding
//    of the exact value to 53 bits; the formulas stay far from the x87's wider
//    exponent range);
//  - 24-bit: the exact result is rounded once to 24 bits. The double result is
//    rounded to nearest, and its exact error term (TwoSum for add/sub, fma for
//    multiply and the division remainder) breaks the one case where rounding
//    the double again would differ: a double that lies exactly halfway between
//    two floats.
// __ftol2_sse (through a double) truncates toward zero; values outside
// the int32 range give 0x80000000.
//
// The mechanics files are built with -ffp-contract=off (CMakeLists.txt) so
// that the compiler never fuses a multiply and an add.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace hailstone::game::mechanics {

enum class FpuPrecision : std::uint8_t {
    Single24, // Hailstone's default; nearest even
    Double53, // IEEE double significand; nearest even (x87 reset is 64-bit)
};

class Fpu {
public:
    explicit constexpr Fpu(FpuPrecision p = FpuPrecision::Single24) : precision_(p) {}
    FpuPrecision precision() const { return precision_; }

    double add(double a, double b) const {
        double s = a + b;
        if (precision_ == FpuPrecision::Double53) return s;
        double bb = s - a;
        double err = (a - (s - bb)) + (b - bb); // TwoSum: a + b = s + err exactly
        return round24(s, err);
    }
    double sub(double a, double b) const { return add(a, -b); }
    double mul(double a, double b) const {
        double p = a * b;
        if (precision_ == FpuPrecision::Double53) return p;
        double err = std::fma(a, b, -p); // a * b = p + err exactly
        return round24(p, err);
    }
    double div(double a, double b) const {
        double q = a / b;
        if (precision_ == FpuPrecision::Double53) return q;
        double rem = std::fma(-q, b, a); // a = q * b + rem exactly
        double err = (b < 0) ? -rem : rem; // the sign of a / b - q
        return round24(q, err);
    }

    // The square root: the exact root rounded once (a negative operand gives the x87's
    // default NaN, 0xfff8... like sqrtsd). In 24-bit mode the operand has at
    // most 24 significant bits, so the remainder's sign settles a tie.
    double sqrt(double a) const {
        double q = std::sqrt(a);
        if (precision_ == FpuPrecision::Double53 || !std::isfinite(q) || q == 0) return q;
        double rem = std::fma(-q, q, a); // a - q * q exactly: the sign of sqrt(a) - q
        return round24(q, rem);
    }

    // __ftol2_sse: truncation toward zero; NaN and out-of-range give INT_MIN.
    static std::int32_t ftol(double v) {
        if (!(v > -2147483649.0 && v < 2147483648.0)) return std::numeric_limits<std::int32_t>::min();
        return static_cast<std::int32_t>(v);
    }
    // A value stored to a float (round to nearest even).
    static double to_float(double v) { return static_cast<double>(static_cast<float>(v)); }

private:
    // Rounds the exact value v + err (|err| at most half an ulp of v) to 24 bits.
    // The x87 keeps its 15-bit exponent at any precision, so a result below
    // FLT_MIN or above FLT_MAX keeps 24 significant bits (a later store to a
    // float rounds it again): the value is scaled into the float range by a
    // power of two (exact), rounded there, and scaled back.
    static double round24(double v, double err) {
        if (!std::isfinite(v) || v == 0) return v;
        int e = 0;
        std::frexp(v, &e);
        double scale = std::ldexp(1.0, 1 - e); // |v * scale| in [1, 2)
        double vs = v * scale, es = err * scale;
        float f = static_cast<float>(vs);
        double fd = f;
        if (es == 0 || fd == vs) return fd / scale;
        // vs sits between f and its neighbour toward vs; only a vs exactly at
        // the midpoint can round the wrong way.
        float g = std::nextafter(f, vs > fd ? std::numeric_limits<float>::infinity()
                                            : -std::numeric_limits<float>::infinity());
        double mid = (fd + static_cast<double>(g)) * 0.5;
        if (vs != mid) return fd / scale;
        bool exact_above = es > 0;
        bool g_above = static_cast<double>(g) > fd;
        return (exact_above == g_above ? static_cast<double>(g) : fd) / scale;
    }

    FpuPrecision precision_;
};

// An x87 operand: a value, and whether it is a float in memory that holds a
// signalling NaN (a value loaded onto the FPU stack is always quiet).
inline bool is_signalling(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return std::isnan(f) && !(b & 0x00400000u);
}

struct X87 {
    double v;
    bool snan = false;
    X87(double d) : v(d) {}
    X87(float f) : v(static_cast<double>(f)), snan(is_signalling(f)) {}
};

// The x87's NaN result for two operands (Intel SDM vol. 1, table 4-7): one
// NaN gives that NaN (quiet); with two, a quiet NaN beats a signalling one,
// else the larger significand wins. SSE, which the doubles below run on,
// returns the first operand instead.
inline std::optional<double> x87_nan(const X87& a, const X87& b) {
    bool an = std::isnan(a.v), bn = std::isnan(b.v);
    if (!an && !bn) return std::nullopt;
    double r;
    if (!bn) r = a.v;
    else if (!an) r = b.v;
    else if (a.snan != b.snan) r = a.snan ? b.v : a.v;
    else {
        std::uint64_t am, bm;
        std::memcpy(&am, &a.v, 8);
        std::memcpy(&bm, &b.v, 8);
        constexpr std::uint64_t kMantissa = (std::uint64_t{1} << 52) - 1;
        r = (bm & kMantissa) > (am & kMantissa) ? b.v : (bm & kMantissa) < (am & kMantissa) ? a.v : (std::signbit(a.v) ? b.v : a.v);
    }
    // The result is quiet. (Set the bit explicitly: the compiler may fold a
    // float -> double -> float round trip that would have quieted it.)
    std::uint64_t rb;
    std::memcpy(&rb, &r, 8);
    rb |= std::uint64_t{1} << 51;
    std::memcpy(&r, &rb, 8);
    return r;
}

inline double x87_mul(const Fpu& f, const X87& a, const X87& b) {
    if (auto n = x87_nan(a, b)) return *n;
    return f.mul(a.v, b.v);
}
inline double x87_add(const Fpu& f, const X87& a, const X87& b) {
    if (auto n = x87_nan(a, b)) return *n;
    return f.add(a.v, b.v);
}
inline double x87_sub(const Fpu& f, const X87& a, const X87& b) {
    if (auto n = x87_nan(a, b)) return *n;
    return f.sub(a.v, b.v);
}
inline double x87_div(const Fpu& f, const X87& a, const X87& b) {
    if (auto n = x87_nan(a, b)) return *n;
    return f.div(a.v, b.v);
}

} // namespace hailstone::game::mechanics
