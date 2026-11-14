#pragma once

// The GPU texture broker tool's Vulkan side (tools/gpu_cache_broker.cpp): its own device, the uploads it exports,
// and a procedural two-process check that imports and leases work. Not part of the client.

#include <cstdint>
#include <string>

#include "render/gpu_cache_protocol.h"

namespace hailstone::gpu_lab {

struct GpuCacheBrokerOptions {
    std::string device_model;
    std::uint32_t device_index = 0;
    bool validation = false;
    std::uint64_t budget_bytes = render::gpu_cache::kMaximumBudget;
    std::uint32_t maximum_entries = render::gpu_cache::kMaximumEntries;
    std::uint32_t hold_seconds = 4320;
};

int run_gpu_cache_broker(const GpuCacheBrokerOptions& options);
// Uses only a generated texture; run in two processes beside a broker it checks imports and frame leases.
int run_gpu_cache_consumer_proof(const GpuCacheBrokerOptions& options, std::uint32_t iterations);
int run_gpu_cache_control(bool stop_if_idle);

} // namespace hailstone::gpu_lab
