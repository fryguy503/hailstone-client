#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace hailstone::assets { struct Image; }

namespace hailstone::render {

class VkContext;
struct GpuSharedTexture;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void destroy(VmaAllocator allocator);
};

struct Texture {
    Texture() = default;
    Texture(const Texture&) = default; // retirement callbacks intentionally retain shared imports
    Texture& operator=(const Texture&) = default;
    Texture(Texture&& other) noexcept { *this = std::move(other); }
    Texture& operator=(Texture&& other) noexcept {
        if (this != &other) {
            image = std::exchange(other.image, VK_NULL_HANDLE);
            allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
            view = std::exchange(other.view, VK_NULL_HANDLE);
            width = std::exchange(other.width, 0);
            height = std::exchange(other.height, 0);
            shared = std::move(other.shared);
        }
        return *this;
    }
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // An imported image has its own local Vulkan objects, with an external payload kept by this owner.
    std::shared_ptr<GpuSharedTexture> shared;
    void destroy(VkDevice device, VmaAllocator allocator);
};

// A shared texture's image and view after GpuSharedCache::localize moved it into a private image (VkContext::
// on_shared_textures_moved). Returns whether this copy changed.
bool refresh_shared_texture(Texture& texture);

// What a texture holds. Colour textures of the world get their mip chain built on the CPU (assets::build_color_mips:
// linear filtering, alpha-weighted colour, cut-out coverage kept); data (normal maps, UI art) keeps the plain chain.
// OpaqueColor is a colour texture whose alpha its material never reads (MaterialState::alpha_unused): made opaque
// first (assets::make_opaque), so the colour under its transparent texels is kept, as the original draws it; a Color
// texture painted on a colour key has it taken out first (assets::remove_color_key: no cyan fringe on the leaves).
enum class TextureUse : std::uint8_t { Data, Color, OpaqueColor };

// Collects uploads of device-local buffers and textures and performs them in
// one submission. submit() does not wait for it (VkContext::submit_upload): the
// frames submitted after it see the results, and its staging memory is freed
// once the GPU is done with it. A target destroyed soon after goes through
// VkContext::retire (or after VkContext::wait_idle).
// Whether the GPU may build this colour image's mip chain itself (UploadBatch::texture) and get what
// assets::build_color_mips would: a single-level RGBA8 colour image, fully opaque (assets::is_opaque: no colour key,
// no coverage to keep, so the chain is a plain average) and a power of two on both sides (the CPU's 2x2 box and a
// 2:1 linear blit then read the same texels; an odd size would be filtered differently). The blit runs on an sRGB
// view of the image, so it filters in linear light as the CPU chain does.
bool gpu_builds_mips(const assets::Image& image, TextureUse use);
// The CPU half of UploadBatch::texture for a colour texture: the image with its mip chain built (and its colour key
// taken out, or made opaque), or nullopt when the image goes up as it is. Safe on any thread; an image it returns
// uploads unchanged as TextureUse::Data. `gpu_mips` false builds the chain here even where gpu_builds_mips() would
// leave it to the GPU (a caller that cuts the chain down: prepare_capped_texture).
std::optional<assets::Image> prepare_texture_image(const assets::Image& image, TextureUse use, bool gpu_mips = true);
// The final immutable external-image payload: colour mips built on the CPU and BC decoded when unsupported.
// Nullopt means the source is already usable unchanged. This never leaves a colour mip chain for an UNORM blit.
std::optional<assets::Image> prepare_immutable_texture_image(const assets::Image& image, TextureUse use, bool bc_textures);
// Drops the image's largest mip levels until it is at most `max_size` texels across (0: none dropped), keeping at
// least one level.
void drop_large_mips(assets::Image& image, std::uint32_t max_size);
// A texture held to `max_size` texels across (0: as it is, `use` unchanged): its chain built on the CPU
// (prepare_texture_image, even where the GPU would build it) and the levels above `max_size` dropped, `use` then
// Data. A neighbour zone's textures (ZoneRenderer::Options::max_texture_size).
void prepare_capped_texture(assets::Image& image, TextureUse& use, std::uint32_t max_size);

class UploadBatch {
public:
    explicit UploadBatch(VkContext& vk) : vk_(vk) {}
    ~UploadBatch();
    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

    Buffer buffer(std::span<const std::byte> data, VkBufferUsageFlags usage);
    // A buffer filled in pieces: the target is created once, and each piece is staged and copied on its own, so
    // the staging holds one piece at a time. A zone's vertex buffer is as big as the zone's geometry, and staging
    // it whole would double that while the copy is made.
    Buffer buffer_pieces(std::span<const std::byte> data, VkBufferUsageFlags usage, std::size_t piece_bytes);

    // The staging this batch holds (host memory the GPU has not read yet).
    std::size_t staging_bytes() const;

    // BCn images are uploaded compressed when the device supports it and
    // decompressed otherwise. Single-level RGBA8 images get a full mip chain; single-level colour images of any format
    // get theirs from assets::build_color_mips, except those gpu_builds_mips() leaves to the GPU (blitted on an sRGB
    // view, the CPU chain again where the device cannot blit one: DeviceCaps::srgb_mip_blit).
    Texture texture(const assets::Image& image, TextureUse use = TextureUse::Data);
    // Only immutable assets: an available external cache can share their payload with other client processes.
    // Colour mips are finalized on the CPU before the external UNORM image is content-keyed and uploaded.
    // Mutable/UI uploads continue to use texture(). Unsupported images retain the normal private upload.
    Texture immutable_texture(const assets::Image& image, TextureUse use = TextureUse::Data);

    void submit();

private:
    struct PendingBuffer {
        Buffer staging;
        Buffer target;
        VkDeviceSize target_offset = 0; // where in the target the staging goes
        VkDeviceSize size = 0;           // how many bytes (0: the staging's whole size)
    };
    struct PendingTexture {
        Buffer staging;
        VkImage image;
        std::vector<VkBufferImageCopy> regions;
        std::uint32_t mip_levels;
        std::uint32_t width, height;
        bool generate_mips; // the levels are blitted from level 0 (on an sRGB-typed image: in linear light)
    };

    Buffer staging(std::span<const std::byte> data);

    VkContext& vk_;
    std::vector<PendingBuffer> buffers_;
    std::vector<PendingTexture> textures_;
};

} // namespace hailstone::render
