#include "render/gpu_shared_cache.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "core/byte_io.h"
#include "platform/gpu_ipc.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Win32 type declarations required by Vulkan's external-memory structs; native calls stay in platform/.
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace hailstone::render {
namespace {
constexpr std::uint32_t recipe_magic = 0x31434748, protocol_version = 1;
constexpr std::uint64_t fence_timeout = 5'000'000'000;
enum class Op : std::uint32_t { Hello = 1, Texture = 2, Acquire = 3, Release = 4, Status = 5, StopIfIdle = 6, RetireTexture = 7 };
enum class Status : std::uint32_t { Ok = 0, Private = 1, Poisoned = 2, Invalid = 3, Busy = 4 };

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " (Vulkan result " + std::to_string(result) + ")");
}
VkFormat format_for(assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case assets::PixelFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case assets::PixelFormat::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case assets::PixelFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case assets::PixelFormat::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case assets::PixelFormat::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case assets::PixelFormat::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}
std::uint32_t levels_for(const assets::Image& image, bool generate) {
    return generate ? static_cast<std::uint32_t>(std::bit_width(std::max(image.width, image.height)))
                    : static_cast<std::uint32_t>(image.mips.size());
}
VkImageUsageFlags usage_for(bool generate) {
    return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
           (generate ? VkImageUsageFlags{VK_IMAGE_USAGE_TRANSFER_SRC_BIT} : VkImageUsageFlags{0});
}
std::string json_string(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

struct Identity {
    std::string model;
    std::uint32_t api = 0, driver = 0;
    Sha256Digest memory{};
    bool operator==(const Identity&) const = default;
};
void write_identity(ByteWriter& out, const Identity& identity) {
    out.fixed_string(identity.model, 256);
    out.le(identity.api); out.le(identity.driver);
    out.bytes(std::as_bytes(std::span(identity.memory)));
}
Identity read_identity(ByteReader& in) {
    Identity identity;
    identity.model = in.fixed_string(256);
    identity.api = in.le<std::uint32_t>(); identity.driver = in.le<std::uint32_t>();
    auto bytes = in.bytes(identity.memory.size());
    std::memcpy(identity.memory.data(), bytes.data(), bytes.size());
    require(!identity.model.empty() && identity.model.size() < 256, "GPU cache identity is invalid");
    return identity;
}
Identity identity_for(VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    ByteWriter encoded;
    encoded.le(memory.memoryTypeCount);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        encoded.le(memory.memoryTypes[i].propertyFlags); encoded.le(memory.memoryTypes[i].heapIndex);
    }
    encoded.le(memory.memoryHeapCount);
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        encoded.le<std::uint64_t>(memory.memoryHeaps[i].size); encoded.le(memory.memoryHeaps[i].flags);
    }
    return {properties.deviceName, properties.apiVersion, properties.driverVersion, sha256(encoded.data())};
}
bool unique_model(VkInstance instance, std::string_view model) {
    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPU cache adapters");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPU cache adapters");
    std::size_t matches = 0;
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(device, &properties);
        if (model == properties.deviceName) ++matches;
    }
    return matches == 1;
}
bool external_image_supported(VkPhysicalDevice physical, const assets::Image& image, bool generate) {
#ifdef _WIN32
    VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkPhysicalDeviceImageFormatInfo2 input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    input.pNext = &external; input.format = format_for(image.format); input.type = VK_IMAGE_TYPE_2D;
    input.tiling = VK_IMAGE_TILING_OPTIMAL; input.usage = usage_for(generate);
    VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 output{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2}; output.pNext = &external_properties;
    if (vkGetPhysicalDeviceImageFormatProperties2(physical, &input, &output) != VK_SUCCESS) return false;
    const auto& capability = external_properties.externalMemoryProperties;
    const auto required = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
    if ((capability.externalMemoryFeatures & required) != required ||
        !(capability.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT) ||
        output.imageFormatProperties.maxExtent.width < image.width || output.imageFormatProperties.maxExtent.height < image.height ||
        output.imageFormatProperties.maxMipLevels < levels_for(image, generate)) return false;
    if (generate && levels_for(image, generate) > 1) {
        VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(physical, input.format, &properties);
        const auto features = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & features) != features) return false;
    }
    return true;
#else
    (void)physical; (void)image; (void)generate; return false;
#endif
}
[[maybe_unused]] VkImageCreateInfo image_info(const assets::Image& image, bool generate) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format_for(image.format);
    info.extent = {image.width, image.height, 1}; info.mipLevels = levels_for(image, generate);
    info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage_for(generate); info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}
[[maybe_unused]] VkImageView image_view(VkDevice device, const VolkDeviceTable& api, VkImage image, VkFormat format, std::uint32_t levels) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image; info.viewType = VK_IMAGE_VIEW_TYPE_2D; info.format = format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    VkImageView view = VK_NULL_HANDLE; check(api.vkCreateImageView(device, &info, nullptr, &view), "create GPU cache image view");
    return view;
}
ByteWriter request(Op operation) { ByteWriter out; out.le(protocol_version); out.le(static_cast<std::uint32_t>(operation)); return out; }
ByteWriter response(Status status) { ByteWriter out; out.le(protocol_version); out.le(static_cast<std::uint32_t>(status)); return out; }
Status read_status(ByteReader& in) {
    require(in.le<std::uint32_t>() == protocol_version, "GPU cache protocol version mismatch");
    const auto status = static_cast<Status>(in.le<std::uint32_t>());
    require(status <= Status::Busy, "GPU cache response is invalid");
    return status;
}
} // namespace

bool validate_gpu_cache_texture(const assets::Image& image, bool generate_mips) noexcept {
    if (!image.width || !image.height || image.width > 16384 || image.height > 16384 ||
        image.mips.empty() || image.mips.size() > static_cast<std::size_t>(std::bit_width(std::max(image.width, image.height))) ||
        static_cast<unsigned>(image.format) > static_cast<unsigned>(assets::PixelFormat::BC7) ||
        (generate_mips && (image.format != assets::PixelFormat::RGBA8 || image.mips.size() != 1))) return false;
    std::uint64_t total = 0;
    auto width = image.width, height = image.height;
    for (const auto& mip : image.mips) {
        const auto expected = assets::mip_size(image.format, width, height);
        if (mip.size() != expected || mip.size() > kGpuCacheMaximumTextureBytes - total) return false;
        total += mip.size(); width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    }
    return total > 0;
}
std::vector<std::byte> encode_gpu_cache_texture(const assets::Image& image, bool generate_mips) {
    require(validate_gpu_cache_texture(image, generate_mips), "GPU cache texture recipe is invalid");
    ByteWriter out; out.le(recipe_magic); out.le(protocol_version); out.le(image.width); out.le(image.height);
    out.le<std::uint32_t>(static_cast<std::uint32_t>(image.format)); out.le<std::uint32_t>(generate_mips ? 1 : 0);
    out.le<std::uint32_t>(static_cast<std::uint32_t>(image.mips.size()));
    for (const auto& mip : image.mips) { out.le<std::uint64_t>(mip.size()); out.bytes(mip); }
    return out.take();
}
GpuCacheTextureRecipe decode_gpu_cache_texture(std::span<const std::byte> packet) {
    require(packet.size() <= kGpuCacheMaximumTextureBytes + 1024, "GPU cache recipe is too large");
    ByteReader in(packet);
    require(in.le<std::uint32_t>() == recipe_magic && in.le<std::uint32_t>() == protocol_version, "GPU cache recipe version mismatch");
    GpuCacheTextureRecipe out;
    out.image.width = in.le<std::uint32_t>(); out.image.height = in.le<std::uint32_t>();
    const auto format = in.le<std::uint32_t>(), generate = in.le<std::uint32_t>(), count = in.le<std::uint32_t>();
    require(format <= static_cast<std::uint32_t>(assets::PixelFormat::BC7) && generate <= 1 && count && count <= 15,
            "GPU cache recipe fields are invalid");
    out.image.format = static_cast<assets::PixelFormat>(format); out.generate_mips = generate != 0;
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto length = in.le<std::uint64_t>();
        require(length <= kGpuCacheMaximumTextureBytes - total && length <= in.remaining(), "GPU cache mip length is invalid");
        auto bytes = in.bytes(static_cast<std::size_t>(length)); out.image.mips.emplace_back(bytes.begin(), bytes.end()); total += length;
    }
    require(in.empty() && validate_gpu_cache_texture(out.image, out.generate_mips), "GPU cache texture recipe is invalid");
    return out;
}
Sha256Digest gpu_cache_texture_key(const assets::Image& image, bool generate_mips) {
    require(validate_gpu_cache_texture(image, generate_mips), "GPU cache texture recipe is invalid");
    ByteWriter header; header.le(recipe_magic); header.le(protocol_version); header.le(image.width); header.le(image.height);
    header.le<std::uint32_t>(static_cast<std::uint32_t>(image.format)); header.le<std::uint32_t>(generate_mips ? 1 : 0);
    header.le<std::uint32_t>(static_cast<std::uint32_t>(image.mips.size()));
    Sha256 hash; hash.update("Hailstone immutable GPU texture v1"); hash.update(header.data());
    for (const auto& mip : image.mips) {
        ByteWriter length; length.le<std::uint64_t>(mip.size()); hash.update(length.data()); hash.update(mip);
    }
    return hash.finish();
}

void GpuCacheLeaseState::register_peer(std::uint64_t peer) {
    if (!peer || poisoned_ || std::find(peers_.begin(), peers_.end(), peer) != peers_.end())
        throw std::runtime_error("GPU cache peer registration is invalid");
    peers_.push_back(peer);
}
std::optional<std::uint64_t> GpuCacheLeaseState::acquire(std::uint64_t peer) {
    if (poisoned_ || std::find(peers_.begin(), peers_.end(), peer) == peers_.end()) return std::nullopt;
    if (owner_ == peer || serial_ == std::numeric_limits<std::uint64_t>::max()) { poisoned_ = true; return std::nullopt; }
    if (owner_) return std::nullopt;
    owner_ = peer; return ++serial_;
}
bool GpuCacheLeaseState::release(std::uint64_t peer, std::uint64_t serial) {
    if (poisoned_ || !peer || owner_ != peer || serial_ != serial) { poisoned_ = true; return false; }
    owner_ = 0; return true;
}
void GpuCacheLeaseState::disconnect(std::uint64_t peer) {
    if (peer && owner_ == peer) poisoned_ = true;
    std::erase(peers_, peer);
}
bool GpuCacheLeaseState::has_peer(std::uint64_t peer) const {
    return std::find(peers_.begin(), peers_.end(), peer) != peers_.end();
}

struct GpuSharedTexture::Storage {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyImageView destroy_view = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::function<void()> retire;
    ~Storage() {
        if (view) destroy_view(device, view, nullptr);
        if (image) destroy_image(device, image, nullptr);
        if (memory) free_memory(device, memory, nullptr);
        if (retire) retire();
    }
};
struct GpuSharedCache::Impl {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable api{};
    std::uint32_t queue_family = 0;
    Identity identity;
    std::unique_ptr<platform::GpuIpcClient> channel;
    mutable std::mutex mutex;
    mutable std::map<std::string, std::weak_ptr<GpuSharedTexture>> textures;
    Stats counters;
    std::uint64_t epoch = 0;
    bool closed = false;
    void abort() noexcept { closed = true; if (channel) channel->close(); }
    void finish(std::uint64_t serial) {
        require(!closed && channel, "GPU shared texture connection was lost");
        auto body = request(Op::Release); body.le(epoch); body.le(serial);
        auto reply = channel->call(body.data()); ByteReader input(reply.body);
        require(read_status(input) == Status::Ok && input.empty() && !reply.handle, "GPU cache ownership release failed");
    }
    void retire_texture(const std::string& key) noexcept {
        if (closed || !channel) return;
        try {
            auto digest = parse_sha256(key); if (!digest) { abort(); return; }
            auto body = request(Op::RetireTexture); body.le(epoch); body.bytes(std::as_bytes(std::span(*digest)));
            auto reply = channel->call(body.data()); ByteReader input(reply.body);
            if (read_status(input) != Status::Ok || !input.empty() || reply.handle) abort();
        } catch (...) { abort(); }
    }
};

bool GpuSharedCache::requested() {
    try { return platform::gpu_ipc_session_from_environment().has_value(); } catch (...) { return true; }
}
const char* GpuSharedCache::device_extension() {
#ifdef _WIN32
    return "VK_KHR_external_memory_win32";
#else
    return nullptr;
#endif
}
GpuSharedCache::GpuSharedCache(VkPhysicalDevice physical, VkDevice device, std::uint32_t queue_family, std::string generic_name,
                               VkInstance instance)
    : impl_(std::make_shared<Impl>()) {
    require(platform::gpu_ipc_available(), "Shared GPU textures are unavailable on this platform; using private textures");
    auto session = platform::gpu_ipc_session_from_environment();
    require(session.has_value(), "Shared GPU texture configuration is absent");
    impl_->physical = physical; impl_->device = device; impl_->queue_family = queue_family;
    volkLoadDeviceTable(&impl_->api, device);
    impl_->identity = identity_for(physical);
    require(impl_->identity.model == generic_name && instance && unique_model(instance, generic_name),
            "Shared GPU texture adapter identity changed or is ambiguous");
    impl_->channel = platform::GpuIpcClient::connect(*session);
    try {
        auto hello = request(Op::Hello); write_identity(hello, impl_->identity);
        auto reply = impl_->channel->call(hello.data()); ByteReader input(reply.body);
        const auto status = read_status(input);
        if (status == Status::Private && input.empty() && !reply.handle) {
            impl_->abort(); throw std::invalid_argument("GPU cache adapter, driver or memory configuration mismatch; using private textures");
        }
        require(status == Status::Ok && read_identity(input) == impl_->identity && !reply.handle, "GPU cache handshake is invalid");
        impl_->epoch = input.le<std::uint64_t>(); require(impl_->epoch && input.empty(), "GPU cache handshake is invalid");
    } catch (const std::invalid_argument&) { throw; }
    catch (...) { impl_->abort(); throw GpuSharedCacheFatal("GPU cache handshake failed after connection; stopping this client"); }
}
GpuSharedCache::~GpuSharedCache() { if (impl_) impl_->abort(); }
bool GpuSharedCache::active() const { return impl_ && impl_->channel && !impl_->closed; }
GpuSharedCache::Stats GpuSharedCache::stats() const { std::lock_guard lock(impl_->mutex); return impl_->counters; }

std::shared_ptr<GpuSharedTexture> GpuSharedCache::texture(const assets::Image& image, bool generate_mips) {
    std::lock_guard lock(impl_->mutex);
    require(active(), "GPU shared texture connection was lost");
    ++impl_->counters.requests;
    if (!validate_gpu_cache_texture(image, generate_mips) || !external_image_supported(impl_->physical, image, generate_mips)) {
        ++impl_->counters.fallbacks; return {};
    }
    try {
    const auto digest = gpu_cache_texture_key(image, generate_mips); const auto key = to_hex(digest);
    if (auto found = impl_->textures.find(key); found != impl_->textures.end())
        if (auto alive = found->second.lock()) { ++impl_->counters.local_hits; return alive; }
    auto body = request(Op::Texture); body.bytes(std::as_bytes(std::span(digest)));
    const auto encoded = encode_gpu_cache_texture(image, generate_mips); body.bytes(encoded);
    auto reply = impl_->channel->call(body.data(), 30000); ByteReader input(reply.body);
    const auto status = read_status(input);
    if (status == Status::Private) { require(input.empty() && !reply.handle, "GPU cache fallback response is invalid"); ++impl_->counters.fallbacks; return {}; }
    require(status == Status::Ok && reply.handle, "GPU cache texture import request failed");
    const auto epoch = input.le<std::uint64_t>(), allocation_size = input.le<std::uint64_t>();
    const auto memory_index = input.le<std::uint32_t>(), levels = input.le<std::uint32_t>(), hit = input.le<std::uint32_t>();
    const auto returned_key = input.bytes(digest.size());
    require(epoch == impl_->epoch && allocation_size && allocation_size <= kGpuCacheMaximumBudget &&
            levels == levels_for(image, generate_mips) && hit <= 1 && input.empty() &&
            std::memcmp(returned_key.data(), digest.data(), digest.size()) == 0, "GPU cache texture metadata is invalid");
#ifdef _WIN32
    auto storage = std::make_shared<GpuSharedTexture::Storage>(); storage->device = impl_->device;
    storage->destroy_view = impl_->api.vkDestroyImageView; storage->destroy_image = impl_->api.vkDestroyImage; storage->free_memory = impl_->api.vkFreeMemory;
    storage->retire = [weak = std::weak_ptr<Impl>(impl_), key] { if (auto cache = weak.lock()) cache->retire_texture(key); };
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    auto info = image_info(image, generate_mips); info.pNext = &external;
    check(impl_->api.vkCreateImage(impl_->device, &info, nullptr, &storage->image), "create imported GPU texture");
    VkMemoryRequirements requirements{}; impl_->api.vkGetImageMemoryRequirements(impl_->device, storage->image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(impl_->physical, &memory);
    require(memory_index < memory.memoryTypeCount && (requirements.memoryTypeBits & (std::uint32_t{1} << memory_index)) &&
            requirements.size == allocation_size && (memory.memoryTypes[memory_index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            "GPU cache import memory requirements differ");
    VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT; imported.handle = reply.handle.get();
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &imported; dedicated.image = storage->image;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.pNext = &dedicated;
    // Opaque handle imports use the exporter's values verbatim, not a locally selected memory type/size.
    allocation.allocationSize = allocation_size; allocation.memoryTypeIndex = memory_index;
    check(impl_->api.vkAllocateMemory(impl_->device, &allocation, nullptr, &storage->memory), "import dedicated GPU texture memory");
    check(impl_->api.vkBindImageMemory(impl_->device, storage->image, storage->memory, 0), "bind imported GPU texture");
    storage->view = image_view(impl_->device, impl_->api, storage->image, info.format, levels);
    auto texture = std::make_shared<GpuSharedTexture>(); texture->image = storage->image; texture->view = storage->view;
    texture->width = image.width; texture->height = image.height; texture->mip_levels = levels;
    texture->storage_ = std::move(storage); texture->key_ = key; impl_->textures[key] = texture;
    ++impl_->counters.imports; impl_->counters.imported_allocation_bytes += allocation_size;
    impl_->counters.broker_hits += hit;
    return texture;
#else
    (void)memory_index; ++impl_->counters.fallbacks; return {};
#endif
    } catch (...) {
        impl_->abort(); throw GpuSharedCacheFatal("GPU shared texture request or import failed; stopping this client");
    }
}
GpuSharedCache::Snapshot GpuSharedCache::snapshot() const {
    std::lock_guard lock(impl_->mutex); require(active(), "GPU shared texture connection was lost; stopping this client"); Snapshot textures;
    for (auto it = impl_->textures.begin(); it != impl_->textures.end();) {
        if (auto texture = it->second.lock()) { textures.push_back(std::move(texture)); ++it; }
        else it = impl_->textures.erase(it);
    }
    return textures;
}
GpuSharedCache::FrameLease GpuSharedCache::acquire_frame() {
    std::lock_guard lock(impl_->mutex); require(active(), "GPU shared texture connection was lost");
    auto body = request(Op::Acquire); body.le(impl_->epoch);
    auto reply = impl_->channel->call(body.data(), 15000); ByteReader input(reply.body);
    require(read_status(input) == Status::Ok && !reply.handle, "GPU shared texture ownership is unavailable or poisoned");
    const auto epoch = input.le<std::uint64_t>(), serial = input.le<std::uint64_t>();
    require(epoch == impl_->epoch && serial && input.empty(), "GPU cache frame lease is invalid");
    ++impl_->counters.leases; return FrameLease(impl_, serial);
}
namespace {
void ownership_barriers(const VolkDeviceTable& api, VkCommandBuffer command, const GpuSharedCache::Snapshot& textures,
                        std::uint32_t family, bool acquire) {
    std::vector<VkImageMemoryBarrier2> barriers; barriers.reserve(textures.size());
    for (const auto& texture : textures) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = acquire ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = acquire ? VkAccessFlags2{0} : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        barrier.dstStageMask = acquire ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = acquire ? VK_ACCESS_2_MEMORY_READ_BIT : VkAccessFlags2{0};
        barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : family;
        barrier.dstQueueFamilyIndex = acquire ? family : VK_QUEUE_FAMILY_EXTERNAL;
        barrier.image = texture->image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture->mip_levels, 0, 1};
        barriers.push_back(barrier);
    }
    if (!barriers.empty()) {
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()); dependency.pImageMemoryBarriers = barriers.data();
        api.vkCmdPipelineBarrier2(command, &dependency);
    }
}
} // namespace
void GpuSharedCache::record_acquire(VkCommandBuffer command, const Snapshot& textures) const {
    ownership_barriers(impl_->api, command, textures, impl_->queue_family, true);
}
void GpuSharedCache::record_release(VkCommandBuffer command, const Snapshot& textures) const {
    ownership_barriers(impl_->api, command, textures, impl_->queue_family, false);
}
GpuSharedCache::FrameLease::FrameLease(std::shared_ptr<Impl> owner, std::uint64_t serial) : owner_(std::move(owner)), serial_(serial) {}
GpuSharedCache::FrameLease::~FrameLease() { if (owner_) owner_->abort(); }
GpuSharedCache::FrameLease::FrameLease(FrameLease&& other) noexcept : owner_(std::move(other.owner_)), serial_(other.serial_) { other.serial_ = 0; }
GpuSharedCache::FrameLease& GpuSharedCache::FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) { if (owner_) owner_->abort(); owner_ = std::move(other.owner_); serial_ = other.serial_; other.serial_ = 0; } return *this;
}
void GpuSharedCache::FrameLease::finish() {
    require(owner_ && serial_, "GPU cache frame lease was already finished");
    owner_->finish(serial_); owner_.reset(); serial_ = 0;
}

namespace {
struct OwnedBuffer {
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable* api = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    ~OwnedBuffer() {
        if (mapped) api->vkUnmapMemory(device, memory);
        if (buffer) api->vkDestroyBuffer(device, buffer, nullptr);
        if (memory) api->vkFreeMemory(device, memory, nullptr);
    }
};
struct Submission {
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable* api = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    ~Submission() {
        if (pending) api->vkDeviceWaitIdle(device);
        if (fence) api->vkDestroyFence(device, fence, nullptr);
        if (pool) api->vkDestroyCommandPool(device, pool, nullptr);
    }
    void finish(VkQueue queue) {
        check(api->vkEndCommandBuffer(command), "end GPU cache command buffer");
        VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer = command;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &command_info;
        check(api->vkQueueSubmit2(queue, 1, &submit, fence), "submit GPU cache commands"); pending = true;
        check(api->vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout), "wait for GPU cache completion"); pending = false;
    }
};
struct BrokerImage {
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable* api = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    platform::TypedGpuIpcHandle handle;
    std::uint64_t allocation_size = 0;
    std::uint32_t memory_index = 0, levels = 0;
    std::uint32_t references = 0;
    ~BrokerImage() {
        if (image) api->vkDestroyImage(device, image, nullptr);
        if (memory) api->vkFreeMemory(device, memory, nullptr);
    }
};

struct GpuDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VolkDeviceTable api{};
    VkPhysicalDeviceMemoryProperties memory{};
    Identity identity;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    std::atomic<std::uint64_t> validation_errors{0};
    bool validation = false;
    ~GpuDevice() {
        if (device) { api.vkDeviceWaitIdle(device); api.vkDestroyDevice(device, nullptr); }
        if (messenger && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        auto& errors = static_cast<GpuDevice*>(user)->validation_errors;
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++errors;
        std::fprintf(stderr, "GPU cache validation: %s\n", data && data->pMessage ? data->pMessage : "message unavailable");
        return VK_FALSE;
    }
    void initialize(const GpuCacheBrokerOptions& options) {
        require(platform::gpu_ipc_available(), "Shared GPU texture tools are unavailable on this platform");
        check(volkInitialize(), "initialize Vulkan for GPU cache");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "Hailstone GPU cache"; app.apiVersion = VK_API_VERSION_1_3;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        if (options.validation) {
            std::uint32_t count = 0; check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate Vulkan layers");
            std::vector<VkLayerProperties> layers(count); check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate Vulkan layers");
            require(std::any_of(layers.begin(), layers.end(), [layer](const auto& l) { return std::strcmp(l.layerName, layer) == 0; }),
                    "Requested GPU cache validation layer is unavailable");
            create.enabledLayerCount = 1; create.ppEnabledLayerNames = &layer;
            create.enabledExtensionCount = 1; create.ppEnabledExtensionNames = &debug_extension;
        }
        check(vkCreateInstance(&create, nullptr, &instance), "create GPU cache Vulkan instance"); volkLoadInstance(instance);
        validation = options.validation;
        if (validation) {
            require(vkCreateDebugUtilsMessengerEXT != nullptr, "GPU cache validation callback is unavailable");
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = debug; info.pUserData = this;
            check(vkCreateDebugUtilsMessengerEXT(instance, &info, nullptr, &messenger), "create GPU cache validation callback");
        }
        std::uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPU cache adapters");
        std::vector<VkPhysicalDevice> devices(count); check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPU cache adapters");
        if (options.device_model.empty()) {
            require(options.device_index < devices.size(), "GPU cache adapter index is unavailable"); physical = devices[options.device_index];
        } else {
            for (const auto candidate : devices) {
                VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate, &properties);
                if (options.device_model == properties.deviceName) { require(!physical, "GPU cache adapter model is ambiguous"); physical = candidate; }
            }
            require(physical != VK_NULL_HANDLE, "GPU cache adapter model is unavailable");
        }
        identity = identity_for(physical);
        require(unique_model(instance, identity.model) && identity.api >= VK_API_VERSION_1_3, "GPU cache adapter model is ambiguous or Vulkan 1.3 is unavailable");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        std::uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families); vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, properties.data());
        bool found = false;
        for (std::uint32_t i = 0; i < families; ++i) if (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; found = true; break; }
        require(found, "GPU cache graphics queue is unavailable");
        const char* extension = GpuSharedCache::device_extension();
        require(extension != nullptr, "GPU cache external texture transport is unavailable");
        std::uint32_t extensions_count = 0;
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensions_count, nullptr), "enumerate GPU cache extensions");
        std::vector<VkExtensionProperties> extensions(extensions_count);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensions_count, extensions.data()), "enumerate GPU cache extensions");
        require(std::any_of(extensions.begin(), extensions.end(), [extension](const auto& e) { return std::strcmp(e.extensionName, extension) == 0; }),
                "GPU cache external memory extension is unavailable");
        VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &supported;
        vkGetPhysicalDeviceFeatures2(physical, &features); require(supported.synchronization2, "GPU cache synchronization2 is unavailable");
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; enabled.synchronization2 = VK_TRUE;
        VkPhysicalDeviceFeatures base_features{};
        base_features.textureCompressionBC = features.features.textureCompressionBC;
        const float priority = 1.0f; VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; info.pNext = &enabled;
        info.pEnabledFeatures = &base_features;
        info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queue_info; info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &extension;
        check(vkCreateDevice(physical, &info, nullptr, &device), "create GPU cache device"); volkLoadDeviceTable(&api, device);
        api.vkGetDeviceQueue(device, family, 0, &queue);
    }
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags, VkMemoryPropertyFlags preferred = 0) const {
        for (const auto extra : {preferred, VkMemoryPropertyFlags{0}})
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((bits & (std::uint32_t{1} << i)) && (memory.memoryTypes[i].propertyFlags & (flags | extra)) == (flags | extra)) return i;
        throw std::runtime_error("Required GPU cache memory type is unavailable");
    }
    void make_buffer(OwnedBuffer& out, std::uint64_t size, VkBufferUsageFlags usage) {
        out.device = device; out.api = &api;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = size; info.usage = usage;
        check(api.vkCreateBuffer(device, &info, nullptr, &out.buffer), "create GPU cache transfer buffer");
        VkMemoryRequirements required{}; api.vkGetBufferMemoryRequirements(device, out.buffer, &required);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = required.size;
        allocation.memoryTypeIndex = memory_type(required.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(api.vkAllocateMemory(device, &allocation, nullptr, &out.memory), "allocate GPU cache transfer memory");
        check(api.vkBindBufferMemory(device, out.buffer, out.memory, 0), "bind GPU cache transfer buffer");
        check(api.vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "map GPU cache transfer memory");
    }
    void begin(Submission& out) {
        out.device = device; out.api = &api;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = family; pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        check(api.vkCreateCommandPool(device, &pool, nullptr, &out.pool), "create GPU cache command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool = out.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
        check(api.vkAllocateCommandBuffers(device, &allocation, &out.command), "allocate GPU cache command buffer");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(api.vkCreateFence(device, &fence, nullptr, &out.fence), "create GPU cache fence");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.vkBeginCommandBuffer(out.command, &info), "begin GPU cache commands");
    }
    void barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                 VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 target, VkAccessFlags2 target_access,
                 std::uint32_t base, std::uint32_t levels, std::uint32_t from = VK_QUEUE_FAMILY_IGNORED,
                 std::uint32_t to = VK_QUEUE_FAMILY_IGNORED) const {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = source; barrier.srcAccessMask = source_access; barrier.dstStageMask = target; barrier.dstAccessMask = target_access;
        barrier.oldLayout = old_layout; barrier.newLayout = new_layout; barrier.srcQueueFamilyIndex = from; barrier.dstQueueFamilyIndex = to;
        barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &barrier;
        api.vkCmdPipelineBarrier2(command, &dependency);
    }
    std::shared_ptr<BrokerImage> upload(const assets::Image& image, bool generate, std::uint64_t maximum_size) {
        if (!external_image_supported(physical, image, generate)) return {};
#ifdef _WIN32
        auto out = std::make_shared<BrokerImage>(); out->device = device; out->api = &api; out->levels = levels_for(image, generate);
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        auto info = image_info(image, generate); info.pNext = &external;
        check(api.vkCreateImage(device, &info, nullptr, &out->image), "create exported GPU texture");
        VkMemoryRequirements required{}; api.vkGetImageMemoryRequirements(device, out->image, &required);
        if (!required.size || required.size > maximum_size) return {};
        out->allocation_size = required.size; out->memory_index = memory_type(required.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image = out->image;
        VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        export_info.pNext = &dedicated; export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.pNext = &export_info;
        allocation.allocationSize = required.size; allocation.memoryTypeIndex = out->memory_index;
        check(api.vkAllocateMemory(device, &allocation, nullptr, &out->memory), "allocate exported GPU texture");
        check(api.vkBindImageMemory(device, out->image, out->memory, 0), "bind exported GPU texture");

        std::vector<std::byte> packed; std::vector<VkBufferImageCopy> regions;
        auto width = image.width, height = image.height;
        for (std::uint32_t level = 0; level < image.mips.size(); ++level) {
            VkBufferImageCopy copy{}; copy.bufferOffset = packed.size(); copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            copy.imageExtent = {width, height, 1}; regions.push_back(copy);
            packed.insert(packed.end(), image.mips[level].begin(), image.mips[level].end()); packed.resize((packed.size() + 15) & ~std::size_t{15});
            width = std::max(1u, width / 2); height = std::max(1u, height / 2);
        }
        OwnedBuffer staging; make_buffer(staging, packed.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(staging.mapped, packed.data(), packed.size());
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = staging.memory; range.size = VK_WHOLE_SIZE;
        check(api.vkFlushMappedMemoryRanges(device, 1, &range), "flush GPU cache texture upload");
        Submission submitted; begin(submitted);
        barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, out->levels);
        api.vkCmdCopyBufferToImage(submitted.command, staging.buffer, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(regions.size()), regions.data());
        if (generate) {
            auto w = static_cast<std::int32_t>(image.width), h = static_cast<std::int32_t>(image.height);
            for (std::uint32_t level = 1; level < out->levels; ++level) {
                barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
                const auto next_w = std::max(1, w / 2), next_h = std::max(1, h / 2);
                VkImageBlit blit{}; blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1}; blit.srcOffsets[1] = {w, h, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1}; blit.dstOffsets[1] = {next_w, next_h, 1};
                api.vkCmdBlitImage(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, level - 1, 1);
                w = next_w; h = next_h;
            }
            barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, out->levels - 1, 1);
        } else {
            barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 0, out->levels);
        }
        barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0, 0, out->levels, family, VK_QUEUE_FAMILY_EXTERNAL);
        submitted.finish(queue);
        auto get_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
        require(get_handle != nullptr, "GPU cache external memory export is unavailable");
        VkMemoryGetWin32HandleInfoKHR handle_info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        handle_info.memory = out->memory; handle_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE handle = nullptr; check(get_handle(device, &handle_info, &handle), "export GPU texture memory");
        out->handle = platform::TypedGpuIpcHandle::adopt(handle); return out;
#else
        (void)maximum_size; return {};
#endif
    }
};

struct Broker {
    GpuDevice gpu;
    std::mutex images_mutex, lease_mutex;
    std::condition_variable changed;
    std::map<std::string, std::shared_ptr<BrokerImage>> images;
    std::map<std::uint64_t, std::map<std::string, std::uint32_t>> consumer_images;
    GpuCacheLeaseState lease;
    std::deque<std::uint64_t> waiting;
    std::uint64_t allocated = 0, requests = 0, hits = 0, grants = 0, releases = 0;
    std::uint64_t epoch = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t budget = 0;
    std::uint64_t pending_stop_connection = 0;
    std::uint32_t maximum_entries = 0;
    std::atomic<bool> stop{false};

    void initialize(const GpuCacheBrokerOptions& options) {
        require(options.budget_bytes && options.budget_bytes <= kGpuCacheMaximumBudget && options.maximum_entries &&
                options.maximum_entries <= kGpuCacheMaximumEntries, "GPU cache budget is invalid");
        budget = options.budget_bytes; maximum_entries = options.maximum_entries; gpu.initialize(options);
        if (!epoch) epoch = 1;
    }
    void disconnect(const platform::GpuIpcPeer& peer) {
        std::scoped_lock lock(lease_mutex, images_mutex); lease.disconnect(peer.connection_id); std::erase(waiting, peer.connection_id);
        if (auto found = consumer_images.find(peer.connection_id); found != consumer_images.end()) {
            for (const auto& [key, count] : found->second)
                for (std::uint32_t i = 0; i < count; ++i) drop_reference(key);
            consumer_images.erase(found);
        }
        // A stop acknowledgement must reach its caller before cancelling the transport's pipes.
        if (pending_stop_connection == peer.connection_id) stop = true;
        changed.notify_all();
    }
    void drop_reference(const std::string& key) {
        auto found = images.find(key); require(found != images.end() && found->second->references, "GPU cache reference ledger differs");
        if (--found->second->references == 0) { allocated -= found->second->allocation_size; images.erase(found); }
    }
    platform::GpuIpcReply status(bool ask_stop, std::uint64_t control_peer = 0) {
        std::scoped_lock lock(images_mutex, lease_mutex);
        const bool idle = lease.peer_count() == 0 && lease.owner() == 0;
        const bool accepted = ask_stop && idle && control_peer && (!pending_stop_connection || pending_stop_connection == control_peer);
        if (accepted) pending_stop_connection = control_peer;
        auto out = response(ask_stop && !accepted ? Status::Busy : Status::Ok);
        out.le(epoch); out.le<std::uint32_t>(static_cast<std::uint32_t>(lease.peer_count())); out.le(lease.owner());
        out.le<std::uint32_t>(lease.poisoned() ? 1 : 0); out.le(allocated); out.le<std::uint32_t>(static_cast<std::uint32_t>(images.size()));
        out.le(requests); out.le(hits); out.le(grants); out.le(releases); out.le(gpu.validation_errors.load());
        return {out.take(), nullptr, {}};
    }
    platform::GpuIpcReply handle(const platform::GpuIpcPeer& peer, std::span<const std::byte> bytes) {
        try {
            ByteReader input(bytes); require(input.le<std::uint32_t>() == protocol_version, "GPU cache request version mismatch");
            const auto operation = static_cast<Op>(input.le<std::uint32_t>());
            if (operation == Op::Status || operation == Op::StopIfIdle) {
                require(input.empty(), "GPU cache control request is invalid"); return status(operation == Op::StopIfIdle, peer.connection_id);
            }
            if (operation == Op::Hello) {
                const auto identity = read_identity(input);
                if (!input.empty() || identity != gpu.identity) return {response(Status::Private).take(), nullptr, {}};
                std::scoped_lock lock(lease_mutex, images_mutex);
                if (pending_stop_connection) return {response(Status::Private).take(), nullptr, {}};
                lease.register_peer(peer.connection_id); consumer_images.try_emplace(peer.connection_id);
                auto out = response(Status::Ok); write_identity(out, gpu.identity); out.le(epoch); return {out.take(), nullptr, {}};
            }
            {
                std::lock_guard lock(lease_mutex);
                if (lease.poisoned() || !lease.has_peer(peer.connection_id)) return {response(Status::Poisoned).take(), nullptr, {}};
            }
            if (operation == Op::Acquire) {
                require(input.le<std::uint64_t>() == epoch && input.empty(), "GPU cache lease epoch is invalid");
                std::unique_lock lock(lease_mutex); waiting.push_back(peer.connection_id);
                const bool ready = changed.wait_for(lock, std::chrono::seconds(8), [&] {
                    return stop || lease.poisoned() || (!lease.owner() && !waiting.empty() && waiting.front() == peer.connection_id);
                });
                std::erase(waiting, peer.connection_id);
                if (!ready || stop || lease.poisoned()) { changed.notify_all(); return {response(Status::Poisoned).take(), nullptr, {}}; }
                const auto serial = lease.acquire(peer.connection_id);
                if (!serial) { changed.notify_all(); return {response(Status::Poisoned).take(), nullptr, {}}; }
                ++grants; auto out = response(Status::Ok); out.le(epoch); out.le(*serial);
                std::printf("{\"kind\":\"lease\",\"action\":\"grant\",\"connection\":%llu,\"serial\":%llu}\n",
                    static_cast<unsigned long long>(peer.connection_id), static_cast<unsigned long long>(*serial)); std::fflush(stdout);
                return {out.take(), nullptr, {}};
            }
            if (operation == Op::Release) {
                const auto request_epoch = input.le<std::uint64_t>(), serial = input.le<std::uint64_t>();
                require(request_epoch == epoch && input.empty(), "GPU cache release epoch is invalid");
                std::lock_guard lock(lease_mutex);
                if (!lease.release(peer.connection_id, serial)) { changed.notify_all(); return {response(Status::Poisoned).take(), nullptr, {}}; }
                ++releases; changed.notify_all();
                std::printf("{\"kind\":\"lease\",\"action\":\"release-after-fence\",\"connection\":%llu,\"serial\":%llu}\n",
                    static_cast<unsigned long long>(peer.connection_id), static_cast<unsigned long long>(serial)); std::fflush(stdout);
                return {response(Status::Ok).take(), nullptr, {}};
            }
            if (operation == Op::Texture) {
                const auto digest_bytes = input.bytes(Sha256Digest{}.size()); Sha256Digest digest{};
                std::memcpy(digest.data(), digest_bytes.data(), digest.size());
                const auto recipe = decode_gpu_cache_texture(input.bytes(input.remaining()));
                require(gpu_cache_texture_key(recipe.image, recipe.generate_mips) == digest, "GPU cache recipe digest differs");
                std::lock_guard lock(images_mutex); ++requests;
                const auto key = to_hex(digest); auto found = images.find(key); const bool hit = found != images.end();
                std::shared_ptr<BrokerImage> image;
                if (hit) { image = found->second; ++hits; }
                else {
                    if (images.size() >= maximum_entries || allocated >= budget) return {response(Status::Private).take(), nullptr, {}};
                    image = gpu.upload(recipe.image, recipe.generate_mips, budget - allocated);
                    if (!image) return {response(Status::Private).take(), nullptr, {}};
                    allocated += image->allocation_size; images.emplace(key, image);
                }
                auto& references = consumer_images.at(peer.connection_id);
                auto& count = references[key];
                require(count < std::numeric_limits<std::uint32_t>::max() && image->references < std::numeric_limits<std::uint32_t>::max(),
                        "GPU cache import reference limit exceeded");
                ++count; ++image->references;
                auto out = response(Status::Ok); out.le(epoch); out.le(image->allocation_size); out.le(image->memory_index);
                out.le(image->levels); out.le<std::uint32_t>(hit ? 1 : 0); out.bytes(std::as_bytes(std::span(digest)));
                return {out.take(), image->handle.get(), image};
            }
            if (operation == Op::RetireTexture) {
                const auto request_epoch = input.le<std::uint64_t>(); const auto digest_bytes = input.bytes(Sha256Digest{}.size());
                Sha256Digest digest{}; std::memcpy(digest.data(), digest_bytes.data(), digest.size());
                require(request_epoch == epoch && input.empty(), "GPU cache texture retirement epoch is invalid");
                const auto key = to_hex(digest); std::lock_guard lock(images_mutex);
                auto found = consumer_images.find(peer.connection_id);
                require(found != consumer_images.end(), "GPU cache texture retirement peer differs");
                auto reference = found->second.find(key);
                require(reference != found->second.end() && reference->second, "GPU cache texture retirement reference differs");
                if (--reference->second == 0) found->second.erase(reference);
                drop_reference(key); return {response(Status::Ok).take(), nullptr, {}};
            }
            throw std::runtime_error("GPU cache operation is invalid");
        } catch (...) {
            std::lock_guard lock(lease_mutex); lease.poison(); changed.notify_all();
            // Malformed/authenticated requests and GPU failures fail this experimental epoch closed.
            return {response(Status::Poisoned).take(), nullptr, {}};
        }
    }
};

void print_status(std::span<const std::byte> bytes, bool stopping) {
    ByteReader input(bytes); const auto status = read_status(input);
    require(status == Status::Ok || status == Status::Busy, "GPU cache status request failed");
    const auto epoch = input.le<std::uint64_t>(); const auto consumers = input.le<std::uint32_t>();
    const auto owner = input.le<std::uint64_t>(); const auto poisoned = input.le<std::uint32_t>();
    const auto allocation = input.le<std::uint64_t>(); const auto entries = input.le<std::uint32_t>();
    const auto requests = input.le<std::uint64_t>(), hits = input.le<std::uint64_t>(), grants = input.le<std::uint64_t>();
    const auto releases = input.le<std::uint64_t>(), errors = input.le<std::uint64_t>();
    require(input.empty(), "GPU cache status payload is invalid");
    std::printf("{\"kind\":\"status\",\"epoch\":%llu,\"consumers\":%u,\"owner_connection\":%llu,\"poisoned\":%s,"
        "\"allocated_bytes\":%llu,\"entries\":%u,\"requests\":%llu,\"hits\":%llu,\"leases_granted\":%llu,\"leases_released\":%llu,"
        "\"validation_errors\":%llu,\"stopped\":%s}\n", static_cast<unsigned long long>(epoch), consumers,
        static_cast<unsigned long long>(owner), poisoned ? "true" : "false", static_cast<unsigned long long>(allocation), entries,
        static_cast<unsigned long long>(requests), static_cast<unsigned long long>(hits), static_cast<unsigned long long>(grants),
        static_cast<unsigned long long>(releases), static_cast<unsigned long long>(errors), stopping && status == Status::Ok ? "true" : "false");
}
} // namespace

int run_gpu_cache_broker(const GpuCacheBrokerOptions& options) {
    require(options.hold_seconds && options.hold_seconds <= 7200, "GPU cache broker lifetime is invalid");
    auto session = platform::gpu_ipc_session_from_environment(); require(session.has_value(), "GPU cache broker configuration is absent");
    Broker broker; broker.initialize(options);
    auto server = platform::GpuIpcServer::start(*session,
        [&](const auto& peer, auto bytes) { return broker.handle(peer, bytes); },
        [&](const auto& peer) { broker.disconnect(peer); });
    const auto version = broker.gpu.identity.api;
    std::printf("{\"kind\":\"broker-ready\",\"schema_version\":1,\"device_model\":%s,\"device_api\":\"%u.%u.%u\","
        "\"driver_version\":%u,\"budget_bytes\":%llu,\"maximum_entries\":%u,\"validation_enabled\":%s,\"validation_errors\":%llu}\n",
        json_string(broker.gpu.identity.model).c_str(), VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version),
        broker.gpu.identity.driver, static_cast<unsigned long long>(options.budget_bytes), options.maximum_entries,
        options.validation ? "true" : "false", static_cast<unsigned long long>(broker.gpu.validation_errors.load())); std::fflush(stdout);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(options.hold_seconds);
    while (!broker.stop && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    broker.stop = true; broker.changed.notify_all(); server->stop();
    const auto final = broker.status(false); print_status(final.body, false);
    const bool complete = !broker.lease.poisoned() && broker.lease.owner() == 0 && broker.lease.peer_count() == 0 &&
                          broker.images.empty() && broker.allocated == 0 && broker.gpu.validation_errors.load() == 0;
    std::printf("{\"kind\":\"broker-complete\",\"clean\":%s,\"poisoned\":%s,\"consumers\":%u,\"owner_connection\":%llu,"
        "\"entries\":%u,\"allocated_bytes\":%llu,\"leases_granted\":%llu,\"leases_released\":%llu,\"validation_errors\":%llu}\n",
        complete ? "true" : "false", broker.lease.poisoned() ? "true" : "false", static_cast<unsigned>(broker.lease.peer_count()),
        static_cast<unsigned long long>(broker.lease.owner()), static_cast<unsigned>(broker.images.size()),
        static_cast<unsigned long long>(broker.allocated), static_cast<unsigned long long>(broker.grants),
        static_cast<unsigned long long>(broker.releases), static_cast<unsigned long long>(broker.gpu.validation_errors.load()));
    return complete ? 0 : 1;
}
int run_gpu_cache_control(bool stop_if_idle) {
    auto session = platform::gpu_ipc_session_from_environment(); require(session.has_value(), "GPU cache broker configuration is absent");
    auto client = platform::GpuIpcClient::connect(*session); auto body = request(stop_if_idle ? Op::StopIfIdle : Op::Status);
    auto reply = client->call(body.data()); require(!reply.handle, "GPU cache control returned an unexpected handle");
    print_status(reply.body, stop_if_idle); ByteReader input(reply.body); return read_status(input) == Status::Ok ? 0 : 2;
}

int run_gpu_cache_consumer_proof(const GpuCacheBrokerOptions& options, std::uint32_t iterations) {
    require(iterations >= 1 && iterations <= 4096, "GPU cache proof iteration count is invalid");
    GpuDevice gpu; gpu.initialize(options);
    GpuSharedCache cache(gpu.physical, gpu.device, gpu.family, gpu.identity.model, gpu.instance);
    assets::Image image; image.width = image.height = 16; image.mips.emplace_back(16 * 16 * 4);
    for (std::size_t i = 0; i < image.mips.front().size(); ++i) image.mips.front()[i] = static_cast<std::byte>((i * 71 + i / 11 + 23) & 255);
    auto texture = cache.texture(image, true); require(texture != nullptr, "GPU cache procedural image fell back privately");
    auto snapshot = cache.snapshot(); require(snapshot.size() == 1, "GPU cache procedural snapshot differs");
    OwnedBuffer readback; gpu.make_buffer(readback, image.mips.front().size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto version = gpu.identity.api;
    std::printf("{\"kind\":\"proof-ready\",\"device_model\":%s,\"device_api\":\"%u.%u.%u\",\"texture_key\":%s,\"iterations\":%u}\n",
        json_string(gpu.identity.model).c_str(), VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version),
        json_string(to_hex(gpu_cache_texture_key(image, true))).c_str(), iterations); std::fflush(stdout);
    std::uint64_t last_serial = 0;
    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
        Submission submitted; gpu.begin(submitted); cache.record_acquire(submitted.command, snapshot);
        gpu.barrier(submitted.command, texture->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
        VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {image.width, image.height, 1};
        gpu.api.vkCmdCopyImageToBuffer(submitted.command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        gpu.barrier(submitted.command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 0, 1);
        cache.record_release(submitted.command, snapshot);
        VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}; host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host.buffer = readback.buffer; host.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &host;
        gpu.api.vkCmdPipelineBarrier2(submitted.command, &dependency);
        auto lease = cache.acquire_frame(); last_serial = lease.serial(); submitted.finish(gpu.queue); lease.finish();
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = readback.memory; range.size = VK_WHOLE_SIZE;
        check(gpu.api.vkInvalidateMappedMemoryRanges(gpu.device, 1, &range), "invalidate GPU cache proof readback");
        require(std::memcmp(readback.mapped, image.mips.front().data(), image.mips.front().size()) == 0, "GPU cache procedural readback differs");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    const auto stats = cache.stats();
    std::printf("{\"kind\":\"proof-complete\",\"iterations\":%u,\"imports\":%llu,\"fallbacks\":%llu,\"leases\":%llu,"
        "\"last_serial\":%llu,\"pixel_hash\":%s,\"validation_errors\":%llu}\n", iterations,
        static_cast<unsigned long long>(stats.imports), static_cast<unsigned long long>(stats.fallbacks),
        static_cast<unsigned long long>(stats.leases), static_cast<unsigned long long>(last_serial),
        json_string(to_hex(sha256(image.mips.front()))).c_str(), static_cast<unsigned long long>(gpu.validation_errors.load()));
    return gpu.validation_errors.load() ? 1 : 0;
}

} // namespace hailstone::render
