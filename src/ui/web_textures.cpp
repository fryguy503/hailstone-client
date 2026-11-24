#include "ui/web_textures.h"

#include <cstring>

#include <imgui_impl_vulkan.h>

#include "render/vk_context.h"
#include "render/vk_util.h"
#include "ui/imgui_vk_textures.h"
#include "web/browser_host.h"

namespace hailstone::ui {

namespace {

// A retired texture is destroyed this many frames later: the frame that drew it last, and those in flight with it.
constexpr std::uint64_t kRetireFrames = render::VkContext::kFramesInFlight + 1;

} // namespace

WebTextures::WebTextures(render::VkContext& vk) : vk_(vk), staging_(render::VkContext::kFramesInFlight) {
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.maxLod = 0.0f;
    render::vk_check(vkCreateSampler(vk_.device(), &s, nullptr, &sampler_), "vkCreateSampler");
}

WebTextures::~WebTextures() {
    vk_.wait_idle();
    for (auto& [key, e] : entries_) destroy(e);
    for (auto& [frame, e] : retired_) destroy(e);
    for (Staging& s : staging_) s.buffer.destroy(vk_.allocator());
    vkDestroySampler(vk_.device(), sampler_, nullptr);
}

WebTextures::View WebTextures::texture(std::uint32_t tab, web::Layer layer) const {
    auto it = entries_.find({tab, layer});
    // No set (none could be made: record tries again each frame): nothing to draw yet.
    if (it == entries_.end() || !it->second.ready || !it->second.set) return {};
    return {it->second.set, it->second.width, it->second.height};
}

WebTextures::Entry WebTextures::make(std::uint16_t width, std::uint16_t height) {
    Entry e;
    e.width = width;
    e.height = height;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_B8G8R8A8_UNORM; // CEF's pixels as they come; a format every device samples
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    render::vk_check(vmaCreateImage(vk_.allocator(), &info, &ai, &e.image, &e.allocation, nullptr), "create web image");
    e.view = render::create_image_view(vk_.device(), e.image, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    e.set = ui::add_imgui_texture(e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return e;
}

void WebTextures::destroy(Entry& e) {
    if (e.set) ui::remove_imgui_texture(e.set);
    if (e.view) vkDestroyImageView(vk_.device(), e.view, nullptr);
    if (e.image) vmaDestroyImage(vk_.allocator(), e.image, e.allocation);
    e = Entry{};
}

// Counted in the renderer's frames, not in record() calls, so a frame that does not record the tabs still counts.
void WebTextures::retire(Entry e) { retired_.emplace_back(vk_.frames_begun() + kRetireFrames, e); }

void WebTextures::collect() {
    if (retired_.empty()) return;
    const std::uint64_t now = vk_.frames_begun();
    std::erase_if(retired_, [&](auto& r) {
        if (r.first > now) return false;
        destroy(r.second);
        return true;
    });
}

void WebTextures::record(const render::FrameInfo& frame, web::BrowserHost& host) {
    collect();

    // Tabs that are gone, or have no image any more.
    for (auto it = entries_.begin(); it != entries_.end();) {
        const web::Tab* t = host.tab(it->first.first);
        const web::Image* img = t ? (it->first.second == web::Layer::Popup ? &t->popup : &t->view) : nullptr;
        if (!img || img->width == 0) {
            retire(it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    // What each tab changed, packed into this slot's staging buffer.
    struct Job {
        Entry* entry;
        const web::Image* image;
        std::vector<VkBufferImageCopy> regions;
    };
    std::vector<Job> jobs;
    VkDeviceSize total = 0;
    for (std::uint32_t id : host.tabs()) {
        const web::Tab* t = host.tab(id);
        for (web::Layer layer : {web::Layer::View, web::Layer::Popup}) {
            const web::Image& img = layer == web::Layer::Popup ? t->popup : t->view;
            std::vector<web::Image::Rect> dirty = host.take_dirty(id, layer);
            if (img.width == 0 || img.height == 0) continue;
            Entry& e = entries_[{id, layer}];
            if (e.width != img.width || e.height != img.height) {
                if (e.image) retire(e);
                e = make(img.width, img.height);
            }
            // add_imgui_texture found no descriptor set (out of memory for a pool): tried again until one is made.
            if (e.image && !e.set) e.set = ui::add_imgui_texture(e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!e.ready) dirty = {{0, 0, img.width, img.height}}; // a new texture takes all of it
            if (dirty.empty()) continue;
            Job job{&e, &img, {}};
            for (const web::Image::Rect& r : dirty) {
                if (r.width == 0 || r.height == 0 || r.x + r.width > img.width || r.y + r.height > img.height) continue;
                VkBufferImageCopy c{};
                c.bufferOffset = total;
                c.bufferRowLength = r.width;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageOffset = {r.x, r.y, 0};
                c.imageExtent = {r.width, r.height, 1};
                job.regions.push_back(c);
                total += VkDeviceSize{r.width} * r.height * 4;
            }
            if (!job.regions.empty()) jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty()) return;

    Staging& s = staging_[frame.frame_slot]; // this slot's last frame is done (begin_frame_commands waited for it)
    if (s.buffer.size < total) {
        s.buffer.destroy(vk_.allocator());
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = std::max<VkDeviceSize>(total, VkDeviceSize{4} << 20);
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        render::vk_check(vmaCreateBuffer(vk_.allocator(), &info, &ai, &s.buffer.buffer, &s.buffer.allocation, &out),
                         "create web staging buffer");
        s.buffer.size = info.size;
        s.mapped = out.pMappedData;
    }
    auto* dst = static_cast<std::byte*>(s.mapped);
    for (const Job& job : jobs) {
        const std::size_t stride = std::size_t{job.image->width} * 4;
        for (const VkBufferImageCopy& c : job.regions) {
            const std::size_t row = std::size_t{c.imageExtent.width} * 4;
            for (std::uint32_t y = 0; y < c.imageExtent.height; ++y)
                std::memcpy(dst + c.bufferOffset + y * row,
                            job.image->bgra.data() + (static_cast<std::size_t>(c.imageOffset.y) + y) * stride +
                                static_cast<std::size_t>(c.imageOffset.x) * 4,
                            row);
        }
    }
    vmaFlushAllocation(vk_.allocator(), s.buffer.allocation, 0, total);

    using render::image_barrier;
    for (Job& job : jobs) {
        Entry& e = *job.entry;
        // The frames before may still sample it: their reads come first (a write-after-read: no access to make
        // visible).
        image_barrier(frame.cmd, e.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      e.ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(frame.cmd, s.buffer.buffer, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(job.regions.size()), job.regions.data());
        image_barrier(frame.cmd, e.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        e.ready = true;
    }
}

} // namespace hailstone::ui
