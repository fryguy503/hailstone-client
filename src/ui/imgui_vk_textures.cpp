#include "ui/imgui_vk_textures.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "ui/descriptor_pool_book.h"

namespace hailstone::ui {

namespace {

// Sets per pool: the stock UI's art (ui/ui_textures.h), the item and spell icon sheets and the browser's tabs fit in a
// few.
constexpr std::uint32_t kSetsPerPool = 256;

struct State {
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    DescriptorPoolBook<VkDescriptorPool> pools{kSetsPerPool};
    std::vector<VkDescriptorPool> all; // every pool made and not yet destroyed, for shutdown
    std::unordered_map<VkDescriptorSet, VkDescriptorPool> owner;
};

State& state() {
    static State s;
    return s;
}

VkDescriptorPool make_pool(VkDevice device) {
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = kSetsPerPool;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (VkResult r = vkCreateDescriptorPool(device, &pi, nullptr, &pool); r != VK_SUCCESS) {
        log::error("ui textures: no descriptor pool (VkResult {})", static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    return pool;
}

VkDescriptorSet allocate(State& s, VkDescriptorPool pool) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s.layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // A pool the book thinks has room can still say no (VK_ERROR_FRAGMENTED_POOL, or a driver's own limit): the next
    // is tried.
    return vkAllocateDescriptorSets(s.device, &ai, &set) == VK_SUCCESS ? set : VK_NULL_HANDLE;
}

} // namespace

void init_imgui_textures(VkDevice device) {
    State& s = state();
    if (s.device) return;
    // As the backend's own (imgui_impl_vulkan.cpp, ImGui_ImplVulkan_CreateDeviceObjects): sets of identically
    // defined layouts bind with its pipeline layout. Since ImGui 1.92.8 that is a sampled image alone; the sampler is
    // the backend's, in a set of its own.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (VkResult r = vkCreateDescriptorSetLayout(device, &li, nullptr, &s.layout); r != VK_SUCCESS) {
        log::error("ui textures: no descriptor set layout (VkResult {})", static_cast<int>(r));
        s.layout = VK_NULL_HANDLE;
        return;
    }
    s.device = device;
}

void shutdown_imgui_textures() {
    State& s = state();
    if (!s.device) return;
    for (VkDescriptorPool p : s.all) vkDestroyDescriptorPool(s.device, p, nullptr);
    for (const auto& kv : s.owner) texture_id_removed(reinterpret_cast<const void*>(kv.first));
    vkDestroyDescriptorSetLayout(s.device, s.layout, nullptr);
    s = State{};
}

VkDescriptorSet add_imgui_texture(VkImageView view, VkImageLayout layout) {
    State& s = state();
    if (!s.device) {
        log::error("ui textures: a texture asked for before the UI is up");
        return VK_NULL_HANDLE;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool from = VK_NULL_HANDLE;
    for (VkDescriptorPool pool : s.pools.with_room()) {
        if ((set = allocate(s, pool))) {
            from = pool;
            break;
        }
    }
    if (!set) {
        if (VkDescriptorPool pool = make_pool(s.device)) {
            s.pools.added(pool);
            s.all.push_back(pool);
            set = allocate(s, pool);
            if (set) from = pool;
        }
    }
    if (!set) {
        log::error("ui textures: no descriptor set left ({} in {} pools)", s.owner.size(), s.pools.pool_count());
        return VK_NULL_HANDLE;
    }
    s.pools.allocated(from);
    VkDescriptorImageInfo image{VK_NULL_HANDLE, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &image;
    vkUpdateDescriptorSets(s.device, 1, &w, 0, nullptr);
    s.owner.emplace(set, from);
    texture_id_added(reinterpret_cast<const void*>(set));
    return set;
}

void remove_imgui_texture(VkDescriptorSet set) {
    texture_id_removed(reinterpret_cast<const void*>(set));
    State& s = state();
    if (!s.device || !set) return;
    auto it = s.owner.find(set);
    if (it == s.owner.end()) return;
    const VkDescriptorPool pool = it->second;
    vkFreeDescriptorSets(s.device, pool, 1, &set);
    s.owner.erase(it);
    // Its last set gone and a newer pool up: destroyed now. No frame in flight draws a freed set (see the header), so
    // none still refers to the pool.
    if (std::optional<VkDescriptorPool> empty = s.pools.freed(pool)) {
        vkDestroyDescriptorPool(s.device, *empty, nullptr);
        s.all.erase(std::find(s.all.begin(), s.all.end(), *empty));
    }
}

} // namespace hailstone::ui
