#include "render/gpu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "assets/texture.h"
#include "render/gpu_shared_cache.h"
#include "render/vk_context.h"
#include "render/vk_util.h"

namespace hailstone::render {

namespace {

VkFormat vk_format(assets::PixelFormat f) {
    switch (f) {
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

std::uint32_t full_mip_count(std::uint32_t w, std::uint32_t h) {
    return static_cast<std::uint32_t>(std::bit_width(std::max(w, h)));
}

} // namespace

void Buffer::destroy(VmaAllocator allocator) {
    if (buffer) vmaDestroyBuffer(allocator, buffer, allocation);
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

void Texture::destroy(VkDevice device, VmaAllocator allocator) {
    if (shared) {
        shared.reset();
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        return;
    }
    if (view) vkDestroyImageView(device, view, nullptr);
    if (image) vmaDestroyImage(allocator, image, allocation);
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
}

UploadBatch::~UploadBatch() {
    // Abandoned uploads: free staging memory; targets were never handed out valid.
    for (auto& b : buffers_) b.staging.destroy(vk_.allocator());
    for (auto& t : textures_) t.staging.destroy(vk_.allocator());
}

Buffer UploadBatch::staging(std::span<const std::byte> data) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(data.size(), 4);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Buffer b;
    VmaAllocationInfo out{};
    vk_check(vmaCreateBuffer(vk_.allocator(), &info, &ai, &b.buffer, &b.allocation, &out), "create staging buffer");
    b.size = info.size;
    std::memcpy(out.pMappedData, data.data(), data.size());
    vk_check(vmaFlushAllocation(vk_.allocator(), b.allocation, 0, VK_WHOLE_SIZE), "flush staging buffer");
    return b;
}

Buffer UploadBatch::buffer(std::span<const std::byte> data, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(data.size(), 4);
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    Buffer target;
    vk_check(vmaCreateBuffer(vk_.allocator(), &info, &ai, &target.buffer, &target.allocation, nullptr), "create buffer");
    target.size = info.size;
    buffers_.push_back({staging(data), target, 0, 0});
    return target;
}

Buffer UploadBatch::buffer_pieces(std::span<const std::byte> data, VkBufferUsageFlags usage, std::size_t piece_bytes) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(data.size(), 4);
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    Buffer target;
    vk_check(vmaCreateBuffer(vk_.allocator(), &info, &ai, &target.buffer, &target.allocation, nullptr), "create buffer");
    target.size = info.size;
    if (data.empty() || piece_bytes == 0) return target;
    // Each piece is submitted on its own, so its staging is released as the GPU reads it and the next piece is
    // staged; the whole buffer is never held twice.
    for (std::size_t off = 0; off < data.size(); off += piece_bytes) {
        const std::size_t n = std::min(piece_bytes, data.size() - off);
        buffers_.push_back({staging(data.subspan(off, n)), target, off, n});
        submit();
        // The piece's staging goes once the GPU has read it, so the next piece does not pile up behind it.
        vk_.release_finished_uploads(piece_bytes);
    }
    return target;
}

std::size_t UploadBatch::staging_bytes() const {
    std::size_t n = 0;
    for (const auto& b : buffers_) n += b.staging.size;
    for (const auto& t : textures_) n += t.staging.size;
    return n;
}

bool gpu_builds_mips(const assets::Image& image, TextureUse use) {
    auto pow2 = [](std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
    return use != TextureUse::Data && image.format == assets::PixelFormat::RGBA8 && image.mips.size() == 1 &&
           (image.width > 1 || image.height > 1) && pow2(image.width) && pow2(image.height) && assets::is_opaque(image);
}

std::optional<assets::Image> prepare_texture_image(const assets::Image& source, TextureUse use, bool gpu_mips) {
    if (use != TextureUse::Data && source.mips.size() == 1 && (source.width > 1 || source.height > 1)) {
        // A fully opaque power-of-two image: its chain is a plain average, which the GPU builds as the CPU would
        // (blitted on an sRGB view, in linear light), so it goes up as it is and the chain is never held in host
        // memory. Anything else (a colour key, coverage to keep, an odd size) gets the CPU's chain.
        if (gpu_mips && gpu_builds_mips(source, use)) return std::nullopt;
        assets::Image converted = assets::to_rgba8(source);
        if (use == TextureUse::OpaqueColor) assets::make_opaque(converted);
        else assets::remove_color_key(converted);
        assets::build_color_mips(converted);
        return converted;
    }
    if (use == TextureUse::Color && source.mips.size() > 1 &&
        (source.format == assets::PixelFormat::BC2 || source.format == assets::PixelFormat::BC3 ||
         source.format == assets::PixelFormat::RGBA8)) {
        // A game DDS with alpha and its own mips: built again from level 0 when it is painted on a colour key, as
        // its stored mips carry the key into the edges. (A pack's BC7 has its key out already, in its .k variant.)
        assets::Image level0 = source;
        level0.mips.resize(1);
        level0 = assets::to_rgba8(level0);
        if (assets::remove_color_key(level0)) {
            assets::build_color_mips(level0);
            return level0;
        }
    }
    return std::nullopt;
}

std::optional<assets::Image> prepare_immutable_texture_image(const assets::Image& source, TextureUse use,
                                                            bool bc_textures) {
    // The external protocol uses an UNORM image. Finish colour mips on the CPU before hashing/exporting, rather
    // than leaving an sRGB mip blit to the private renderer's image-creation path.
    if (auto prepared = prepare_texture_image(source, use, false)) return prepared;
    if (source.format != assets::PixelFormat::RGBA8 && !bc_textures) return assets::to_rgba8(source);
    return std::nullopt;
}

void drop_large_mips(assets::Image& image, std::uint32_t max_size) {
    if (max_size == 0) return;
    while (image.mips.size() > 1 && std::max(image.width, image.height) > max_size) {
        image.mips.erase(image.mips.begin());
        image.width = std::max(1u, image.width / 2);
        image.height = std::max(1u, image.height / 2);
    }
}

void prepare_capped_texture(assets::Image& image, TextureUse& use, std::uint32_t max_size) {
    if (max_size == 0) return;
    // The chain is built here even for an image the GPU could build it for: only a chain has levels to drop.
    if (auto built = prepare_texture_image(image, use, false)) {
        image = std::move(*built);
        use = TextureUse::Data;
    }
    drop_large_mips(image, max_size);
}

Texture UploadBatch::texture(const assets::Image& source, TextureUse use) {
    const assets::Image* image = &source;
    assets::Image converted;
    if (auto prepared = prepare_texture_image(source, use)) {
        converted = std::move(*prepared);
        image = &converted;
    }
    if (image == &source && source.format != assets::PixelFormat::RGBA8 && !vk_.caps().bc_textures) {
        converted = assets::to_rgba8(source);
        image = &converted;
    }
    // The GPU builds the chain of what prepare_texture_image left to it (gpu_builds_mips), on an sRGB view; a device
    // that cannot blit that format gets the CPU's chain instead.
    bool srgb_mips = image == &source && gpu_builds_mips(source, use);
    if (srgb_mips && !vk_.caps().srgb_mip_blit) {
        converted = *prepare_texture_image(source, use, false);
        image = &converted;
        srgb_mips = false;
    }

    const bool generate = image->format == assets::PixelFormat::RGBA8 && image->mips.size() == 1;
    const std::uint32_t levels = generate ? full_mip_count(image->width, image->height)
                                          : static_cast<std::uint32_t>(image->mips.size());
    const VkFormat format = vk_format(image->format);

    std::vector<std::byte> packed;
    std::vector<VkBufferImageCopy> regions;
    std::uint32_t w = image->width, h = image->height;
    for (std::uint32_t level = 0; level < image->mips.size(); ++level) {
        VkBufferImageCopy region{};
        region.bufferOffset = packed.size();
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.imageExtent = {w, h, 1};
        regions.push_back(region);
        packed.insert(packed.end(), image->mips[level].begin(), image->mips[level].end());
        // Keep offsets 16-byte aligned (a multiple of any texel block size).
        packed.resize((packed.size() + 15) & ~std::size_t{15});
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    // An image whose chain the GPU builds is sRGB-typed, so vkCmdBlitImage decodes to linear, filters and encodes
    // back (as assets::build_color_mips filters in linear light); its view stays UNORM, so the shaders read the same
    // values as from a CPU-built chain (they decode sRGB themselves).
    const std::array<VkFormat, 2> view_formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (srgb_mips) {
        info.format = VK_FORMAT_R8G8B8A8_SRGB;
        info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        format_list.viewFormatCount = static_cast<std::uint32_t>(view_formats.size());
        format_list.pViewFormats = view_formats.data();
        info.pNext = &format_list;
    }
    info.extent = {image->width, image->height, 1};
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (generate ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    Texture t;
    vk_check(vmaCreateImage(vk_.allocator(), &info, &ai, &t.image, &t.allocation, nullptr), "create texture image");
    t.view = create_image_view(vk_.device(), t.image, format, VK_IMAGE_ASPECT_COLOR_BIT, levels);
    t.width = image->width;
    t.height = image->height;

    textures_.push_back({staging(packed), t.image, std::move(regions), levels, image->width, image->height, generate});
    return t;
}

Texture UploadBatch::immutable_texture(const assets::Image& source, TextureUse use) {
    if (GpuSharedCache* cache = vk_.gpu_shared_cache(); cache && cache->active()) {
        auto prepared = prepare_immutable_texture_image(source, use, vk_.caps().bc_textures);
        const assets::Image& image = prepared ? *prepared : source;
        const bool generate = image.format == assets::PixelFormat::RGBA8 && image.mips.size() == 1;
        if (auto owner = cache->texture(image, generate)) {
            Texture texture;
            texture.image = owner->image;
            texture.view = owner->view;
            texture.width = owner->width;
            texture.height = owner->height;
            texture.shared = std::move(owner);
            vk_.note_shared_gpu_texture(true);
            return texture;
        }
        vk_.note_shared_gpu_texture(false);
        return texture(image, TextureUse::Data);
    }
    vk_.note_shared_gpu_texture(false);
    return texture(source, use);
}

void UploadBatch::submit() {
    if (buffers_.empty() && textures_.empty()) return;
    // Not waited for: the frames after it on the queue see what it wrote (the barriers below), and its staging
    // buffers go once its fence is signalled (VkContext::retire_after_upload).
    VkDeviceSize staged = 0;
    for (const auto& b : buffers_) staged += b.staging.size;
    for (const auto& t : textures_) staged += t.staging.size;
    const std::uint64_t serial = vk_.submit_upload([&](VkCommandBuffer cmd) {
        for (auto& b : buffers_) {
            VkBufferCopy copy{0, b.target_offset, b.size ? b.size : b.staging.size};
            vkCmdCopyBuffer(cmd, b.staging.buffer, b.target.buffer, 1, &copy);
        }
        for (auto& t : textures_) {
            image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(cmd, t.staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(t.regions.size()), t.regions.data());
            if (t.generate_mips) {
                std::int32_t w = static_cast<std::int32_t>(t.width), h = static_cast<std::int32_t>(t.height);
                for (std::uint32_t level = 1; level < t.mip_levels; ++level) {
                    image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                  VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
                    std::int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
                    VkImageBlit blit{};
                    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                    blit.srcOffsets[1] = {w, h, 1};
                    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                    blit.dstOffsets[1] = {nw, nh, 1};
                    vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                    image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, level - 1, 1);
                    w = nw;
                    h = nh;
                }
                image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, t.mip_levels - 1, 1);
            } else {
                image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        }
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        // The buffers are read as vertices and indices, and by shaders as storage (the zone's light grid in
        // zone.frag) or uniform buffers. (No compute pipelines exist; add COMPUTE_SHADER here if one ever reads them.)
        mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    }, staged);
    std::vector<Buffer> staging;
    staging.reserve(buffers_.size() + textures_.size());
    for (auto& b : buffers_) staging.push_back(b.staging);
    for (auto& t : textures_) staging.push_back(t.staging);
    buffers_.clear();
    textures_.clear();
    vk_.retire_after_upload(serial, [allocator = vk_.allocator(), staging]() mutable {
        for (Buffer& b : staging) b.destroy(allocator);
    });
}

} // namespace hailstone::render
