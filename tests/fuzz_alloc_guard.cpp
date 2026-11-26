// Replaces the global operator new so a parser that trusts a bogus length field fails the fuzz tests instead of
// exhausting memory (see fuzz_support.h). Only requests above kMaxAllocation made while the guard is armed throw.

#include <atomic>
#include <cstdlib>
#include <new>

#include "fuzz_support.h"

namespace {
std::atomic<unsigned> g_mode{0}; // 0 off, 1 fuzz guard, 2 allocation count
std::atomic<std::size_t> g_largest{0};
std::atomic<std::size_t> g_count_calls{0};
std::atomic<std::size_t> g_count_bytes{0};

void* guarded_alloc(std::size_t n) {
    const unsigned mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 1) {
        std::size_t seen = g_largest.load(std::memory_order_relaxed);
        while (n > seen && !g_largest.compare_exchange_weak(seen, n, std::memory_order_relaxed)) {}
        if (n > hailstone::fuzz::kMaxAllocation) throw hailstone::fuzz::HugeAllocation();
    } else if (mode == 2) {
        g_count_calls.fetch_add(1, std::memory_order_relaxed);
        g_count_bytes.fetch_add(n, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
} // namespace

namespace hailstone::fuzz {
void arm_allocation_guard(bool on) { g_mode.store(on ? 1u : 0u, std::memory_order_relaxed); }
void reset_largest_allocation() { g_largest.store(0, std::memory_order_relaxed); }
std::size_t largest_allocation() { return g_largest.load(std::memory_order_relaxed); }
void start_allocation_count() {
    g_count_calls.store(0, std::memory_order_relaxed);
    g_count_bytes.store(0, std::memory_order_relaxed);
    g_mode.store(2, std::memory_order_relaxed);
}
AllocationCount stop_allocation_count() {
    g_mode.store(0, std::memory_order_relaxed);
    return {g_count_calls.load(std::memory_order_relaxed), g_count_bytes.load(std::memory_order_relaxed)};
}
} // namespace hailstone::fuzz

void* operator new(std::size_t n) { return guarded_alloc(n); }
void* operator new[](std::size_t n) { return guarded_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return guarded_alloc(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return guarded_alloc(n); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
