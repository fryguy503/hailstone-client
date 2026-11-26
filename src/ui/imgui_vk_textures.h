#pragma once
// The client's ImGui textures (ImTextureIDs: descriptor sets for the Vulkan backend's pipeline), keeping the list of
// live texture IDs (ui/texture_ids.h) that the Lua bindings check.
//
// The sets come from descriptor pools of the client's own (ui/descriptor_pool_book.h: another made whenever those are
// full, and one left empty destroyed unless it is the newest), with a set layout
// defined as the backend's (one sampled image at binding 0, fragment stage; its sampler is the backend's own, linear and
// clamped, which these textures are drawn with), so they bind with its pipeline
// layout. Not ImGui_ImplVulkan_AddTexture: that allocated from one fixed pool and, once it was full, only logged the
// error and wrote the descriptor into a null set (a crash in the driver), so the callers' "no descriptor left"
// fallbacks never ran. The backend keeps its own pool for the font atlas.
#include <imgui_impl_vulkan.h>

#include "ui/texture_ids.h"

namespace hailstone::ui {

// Made by ImGuiLayer once the backend is up, and let go by it (after the device is idle).
void init_imgui_textures(VkDevice device);
void shutdown_imgui_textures();

// A set for `view`, a live texture ID; VK_NULL_HANDLE (logged) when none can be made: before
// init_imgui_textures, or with the device out of memory for another pool.
VkDescriptorSet add_imgui_texture(VkImageView view, VkImageLayout layout);
// Frees the set now: no frame in flight may still draw it (after VkContext::wait_idle, or in a release that
// VkContext::retire runs). After shutdown_imgui_textures (whose pools took their sets with them) it does nothing.
void remove_imgui_texture(VkDescriptorSet set);

} // namespace hailstone::ui
