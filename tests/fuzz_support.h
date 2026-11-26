#pragma once

// Deterministic fuzz-style helpers for the parser tests: a seeded generator, mutation and truncation of known-good
// fixtures, and a guard that turns a huge allocation (a bogus length field trusted by a parser) into a test failure.
// Everything is reproducible: the same seed gives the same inputs on every platform.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "core/log.h"

namespace hailstone::fuzz {

using Bytes = std::vector<std::byte>;

// SplitMix64: tiny, fast, identical everywhere.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    // Uniform in [0, n); n > 0.
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }

private:
    std::uint64_t s_;
};

// A byte that is likely to be a boundary value for a length, count or flag field.
inline std::byte interesting_byte(Rng& rng) {
    static constexpr std::uint8_t kValues[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x0f, 0x10, 0x20, 0x7f,
                                               0x80, 0x81, 0xfe, 0xff, 0xff, 0x00, 0x00, 0x40, 0xc0};
    return static_cast<std::byte>(kValues[rng.below(sizeof kValues)]);
}

// Random data biased towards zeros, 0xff and small numbers, so length and count fields often come out huge or
// negative while strings stay plausible.
inline Bytes random_bytes(Rng& rng, std::size_t n) {
    Bytes out(n);
    for (auto& b : out) b = rng.chance(60) ? interesting_byte(rng) : static_cast<std::byte>(rng.below(256));
    return out;
}

// A copy of `seed` with a few random edits: byte flips, boundary values, an overwritten 32-bit field (0xffffffff,
// 0x80000000, a small count), a deleted or duplicated run, or an appended tail.
inline Bytes mutate(const Bytes& seed, Rng& rng) {
    Bytes out = seed;
    unsigned edits = 1 + static_cast<unsigned>(rng.below(4));
    for (unsigned i = 0; i < edits; ++i) {
        std::size_t kind = rng.below(7);
        if (out.empty()) kind = 6;
        switch (kind) {
        case 0: out[rng.below(out.size())] ^= static_cast<std::byte>(1u << rng.below(8)); break;
        case 1: out[rng.below(out.size())] = interesting_byte(rng); break;
        case 2:
        case 3: {
            static constexpr std::uint32_t kWords[] = {0xffffffffu, 0x80000000u, 0x7fffffffu, 0x00010000u,
                                                       0x0000ffffu, 0x00ffffffu, 0x40000000u, 0x0000007fu};
            std::uint32_t w = rng.chance(70) ? kWords[rng.below(8)] : static_cast<std::uint32_t>(rng.below(300));
            std::size_t at = rng.below(out.size());
            for (std::size_t k = 0; k < 4 && at + k < out.size(); ++k) out[at + k] = static_cast<std::byte>(w >> (8 * k));
            break;
        }
        case 4: {
            std::size_t at = rng.below(out.size());
            std::size_t len = 1 + rng.below(std::min<std::size_t>(16, out.size() - at));
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(at), out.begin() + static_cast<std::ptrdiff_t>(at + len));
            break;
        }
        case 5: {
            std::size_t at = rng.below(out.size());
            std::size_t len = 1 + rng.below(std::min<std::size_t>(16, out.size() - at));
            Bytes run(out.begin() + static_cast<std::ptrdiff_t>(at), out.begin() + static_cast<std::ptrdiff_t>(at + len));
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), run.begin(), run.end());
            break;
        }
        default: {
            Bytes tail = random_bytes(rng, 1 + rng.below(24));
            out.insert(out.end(), tail.begin(), tail.end());
            break;
        }
        }
    }
    return out;
}

// Thrown by the allocation guard (fuzz_alloc_guard.cpp) for a single request above kMaxAllocation while armed.
struct HugeAllocation : std::bad_alloc {
    const char* what() const noexcept override { return "huge allocation requested"; }
};
constexpr std::size_t kMaxAllocation = 64u << 20;

void arm_allocation_guard(bool on);
// The largest single request made while the guard was armed since the last reset (for tests that a small input
// does not allocate much before it is refused).
void reset_largest_allocation();
std::size_t largest_allocation();

// A scoped count for CPU benchmarks. Only one count may be active at a time; the fuzz guard and counting modes do
// not overlap. Counts every global operator new/delete-sized request, not Lua's private allocator.
struct AllocationCount {
    std::size_t calls = 0;
    std::size_t bytes = 0;
};
void start_allocation_count();
AllocationCount stop_allocation_count();

// Mutated input makes parsers log warnings by the thousand; keep them out of the test output.
struct QuietLog {
    QuietLog() { log::set_level(log::Level::Error); }
    ~QuietLog() { log::set_level(log::Level::Info); }
};

// How a call may end. A parser that reports errors by exception may throw any std::exception except bad_alloc;
// a decoder that returns optional must not throw at all.
enum class Throws { Never, Errors };

// Runs `body` on one input and records a failure (with `what` and the input size) for a crash-free but wrong ending:
// an unexpected exception, a huge allocation, or a call slower than the limit. Returns false on failure.
template <typename F> bool run_case(const char* what, const Bytes& input, Throws mode, F&& body) {
    arm_allocation_guard(true);
    auto start = std::chrono::steady_clock::now();
    bool ok = true;
    std::string problem;
    try {
        body(std::span<const std::byte>(input));
    } catch (const HugeAllocation&) {
        ok = false;
        problem = "huge allocation";
    } catch (const std::bad_alloc&) {
        ok = false;
        problem = "bad_alloc";
    } catch (const std::exception& e) {
        if (mode == Throws::Never) {
            ok = false;
            problem = std::string("threw: ") + e.what();
        }
    } catch (...) {
        ok = false;
        problem = "threw a non-exception";
    }
    arm_allocation_guard(false);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (ms > 2000) {
        ok = false;
        problem += " (took " + std::to_string(ms) + " ms)";
    }
    if (!ok) {
        // doctest stops reporting the same message after a while; keep the first few per run readable.
        static int reported = 0;
        if (reported++ < 40) FAIL_CHECK(std::string(what) << ": " << problem << ", input " << input.size() << " bytes");
    }
    return ok;
}

} // namespace hailstone::fuzz
